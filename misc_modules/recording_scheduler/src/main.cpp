// Recording Scheduler -- schedule recordings by start/stop time, per entry, against a chosen
// radio + Recorder settings. See RECORDING_SCHEDULER_PLAN.md at the repo root for the full
// design and phased build order. This file currently covers phases 0-4 only:
//
//   Phase 0: module skeleton -- builds/loads/toggles cleanly, empty menu panel.
//   Phase 1: entry data model, own persisted config file, Add/Duplicate/Delete/Enable list UI.
//   Phase 2: radio settings capture/apply/view (RSR200 only for now -- the only source module
//            that implements SourceHandler::captureConfigHandler/applyConfigHandler so far).
//   Phase 3: Recorder settings capture/edit (any Recorder instance -- that module's schema is
//            fixed/known, unlike a radio's, so it's edited in place rather than opaque).
//   Phase 4: recurrence editing -- Once/Daily/Weekly, start/stop time.
//
// Deliberately NOT here yet: the engine thread that actually fires anything. Every entry today
// can be fully configured -- radio, recorder, and now when -- but nothing is ever applied or
// started automatically. That's phase 5.
//
// New module, not an extension of the existing (abandoned, non-functional) misc_modules/
// scheduler -- see RECORDING_SCHEDULER_PLAN.md section 0 for why.

#include <imgui.h>
#include <module.h>
#include <gui/gui.h>
#include <gui/style.h>
#include <gui/dialogs/dialog_box.h>
#include <config.h>
#include <core.h>
#include <signal_path/signal_path.h>
#include <recorder_interface.h>
#include <map>
#include <vector>
#include <string>
#include <chrono>
#include <atomic>
#include <cstring>
#include <ctime>
#include <cstdio>

SDRPP_MOD_INFO{
    /* Name:            */ "recording_scheduler",
    /* Description:     */ "Schedule recordings by start/stop time, per radio+recorder settings",
    /* Author:          */ "Ralph Brandi",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ -1
};

ConfigManager config;

#define CONCAT(a, b) ((std::string(a) + b).c_str())

// "YYYY-MM-DD HH:MM:SS" <-> unix seconds (local time). sscanf/mktime/localtime_r|s rather than
// strptime -- strptime is POSIX-only and this codebase also builds under MSVC (no strptime in
// its CRT), matching the portability constraint already established elsewhere in this tree
// (e.g. the file_source Windows timer fix earlier this session).
static int64_t parseDateTime(const std::string& s) {
    struct tm tmv = {};
    int y, mo, d, h, mi, se;
    if (sscanf(s.c_str(), "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &se) != 6) { return 0; }
    tmv.tm_year = y - 1900;
    tmv.tm_mon = mo - 1;
    tmv.tm_mday = d;
    tmv.tm_hour = h;
    tmv.tm_min = mi;
    tmv.tm_sec = se;
    tmv.tm_isdst = -1;
    return (int64_t)mktime(&tmv);
}

static std::string formatDateTime(int64_t epoch) {
    if (epoch == 0) { return ""; }
    time_t t = (time_t)epoch;
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return std::string(buf);
}

// Once/Daily/Weekly, matching RECORDING_SCHEDULER_PLAN.md section 3's schema exactly.
// "once" uses absolute instants (startEpoch/stopEpoch); "daily"/"weekly" instead use a
// wall-clock time-of-day + duration, evaluated against whichever date is next due -- that
// evaluation is phase 5's job (the engine), not this struct's. No end-date for daily/weekly
// yet (plan section 8, open decision 4) -- runs indefinitely until the entry is disabled.
struct Recurrence {
    std::string type = "once";          // "once" | "daily" | "weekly"
    int64_t startEpoch = 0;             // "once" only
    int64_t stopEpoch = 0;              // "once" only
    std::string startTimeOfDay = "00:00:00";   // "daily"/"weekly" only, "HH:MM:SS"
    int durationMin = 60;               // "daily"/"weekly" only, minutes
    bool daysOfWeek[7] = { false, false, false, false, false, false, false };   // Sun..Sat, "weekly" only

    json toJson() const {
        json j;
        j["type"] = type;
        j["startEpoch"] = startEpoch;
        j["stopEpoch"] = stopEpoch;
        j["startTimeOfDay"] = startTimeOfDay;
        j["durationMin"] = durationMin;
        json dow = json::array();
        for (int i = 0; i < 7; i++) { if (daysOfWeek[i]) { dow.push_back(i); } }
        j["daysOfWeek"] = dow;
        return j;
    }

    static Recurrence fromJson(const json& j) {
        Recurrence r;
        if (j.contains("type")) { r.type = j["type"]; }
        if (j.contains("startEpoch")) { r.startEpoch = j["startEpoch"]; }
        if (j.contains("stopEpoch")) { r.stopEpoch = j["stopEpoch"]; }
        if (j.contains("startTimeOfDay")) { r.startTimeOfDay = j["startTimeOfDay"]; }
        if (j.contains("durationMin")) { r.durationMin = j["durationMin"]; }
        if (j.contains("daysOfWeek")) {
            for (auto& v : j["daysOfWeek"]) {
                int d = v;
                if (d >= 0 && d < 7) { r.daysOfWeek[d] = true; }
            }
        }
        return r;
    }
};

// One schedule entry. Deliberately minimal for phase 1 -- name/enabled/status only. The full
// schema (recurrence, source snapshot, recorder snapshot -- RECORDING_SCHEDULER_PLAN.md
// section 3) gets built out incrementally in later phases; keeping this struct small now
// rather than pre-declaring fields nothing reads or writes yet.
struct Entry {
    std::string id;      // stable, independent of display name -- see section 3
    std::string name = "New entry";
    bool enabled = true;
    std::string status = "scheduled";   // placeholder until phase 5's engine gives it meaning

    // Phase 2: which radio this entry targets, and a captured snapshot of its settings.
    // sourceModuleType (core::moduleManager.getInstanceModuleName(sourceName) at capture time)
    // guards against applying a stale snapshot to a same-named-but-different-module source
    // later (device swapped, name reused) -- not enforced yet (that's phase 5's job), just
    // recorded now. sourceConfigSnapshot is opaque on purpose -- see source.h/
    // RECORDING_SCHEDULER_PLAN.md section 2.2 -- this module never interprets its fields,
    // only captures/stores/displays/re-applies it whole.
    std::string sourceName;
    std::string sourceModuleType;
    json sourceConfigSnapshot;
    int64_t sourceConfigCapturedAt = 0;   // unix seconds, 0 = never captured

    // Phase 3: which Recorder instance this entry targets, and its settings -- unlike the
    // radio snapshot above, this one *is* editable in place (RECORDING_SCHEDULER_PLAN.md
    // section 2.3: the Recorder module is identical for every radio and has a small, fixed
    // field set, so this module can reasonably know its schema). "Reset to current" overwrites
    // it wholesale from RECORDER_IFACE_CMD_GET_CONFIG; otherwise it's just edited here directly.
    std::string recorderName;
    json recorderConfigSnapshot;

    // Phase 4: when this entry runs. Default matches Recurrence's own default (a "once" entry
    // with no start/stop set yet -- a freshly Added entry is inert until edited, same as its
    // radio/recorder sections above already are).
    Recurrence recurrence;

    bool selected = false;

    json toJson() const {
        json j;
        j["name"] = name;
        j["enabled"] = enabled;
        j["status"] = status;
        j["sourceName"] = sourceName;
        j["sourceModuleType"] = sourceModuleType;
        j["sourceConfigSnapshot"] = sourceConfigSnapshot;
        j["sourceConfigCapturedAt"] = sourceConfigCapturedAt;
        j["recorderName"] = recorderName;
        j["recorderConfigSnapshot"] = recorderConfigSnapshot;
        j["recurrence"] = recurrence.toJson();
        return j;
    }

    static Entry fromJson(const std::string& id, const json& j) {
        Entry e;
        e.id = id;
        if (j.contains("name")) { e.name = j["name"]; }
        if (j.contains("enabled")) { e.enabled = j["enabled"]; }
        if (j.contains("status")) { e.status = j["status"]; }
        if (j.contains("sourceName")) { e.sourceName = j["sourceName"]; }
        if (j.contains("sourceModuleType")) { e.sourceModuleType = j["sourceModuleType"]; }
        if (j.contains("sourceConfigSnapshot")) { e.sourceConfigSnapshot = j["sourceConfigSnapshot"]; }
        if (j.contains("sourceConfigCapturedAt")) { e.sourceConfigCapturedAt = j["sourceConfigCapturedAt"]; }
        if (j.contains("recorderName")) { e.recorderName = j["recorderName"]; }
        if (j.contains("recorderConfigSnapshot")) { e.recorderConfigSnapshot = j["recorderConfigSnapshot"]; }
        if (j.contains("recurrence")) { e.recurrence = Recurrence::fromJson(j["recurrence"]); }
        return e;
    }
};

// Timestamp + a per-process counter, hex-joined -- unique enough for a locally-generated id
// with no chance of two entries created in the same session colliding, without pulling in a
// real UUID dependency for something that's purely an internal map key never shown to the
// user.
static std::string generateId() {
    static std::atomic<uint32_t> counter{ 0 };
    uint64_t ms = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    char buf[64];
    snprintf(buf, sizeof(buf), "%llx-%x", (unsigned long long)ms, counter.fetch_add(1));
    return std::string(buf);
}

class RecordingSchedulerModule : public ModuleManager::Instance {
public:
    RecordingSchedulerModule(std::string name) {
        this->name = name;
        gui::menu.registerEntry(name, menuHandler, this, NULL);
        loadConfig();
    }

    ~RecordingSchedulerModule() {
        gui::menu.removeEntry(name);
    }

    void postInit() {}
    void enable() { enabled = true; }
    void disable() { enabled = false; }
    bool isEnabled() { return enabled; }

private:
    void loadConfig() {
        config.acquire();
        entries.clear();
        if (config.conf.contains("entries")) {
            for (auto& [id, ej] : config.conf["entries"].items()) {
                entries[id] = Entry::fromJson(id, ej);
            }
        }
        config.release();
    }

    void saveConfig() {
        config.acquire();
        json ej;
        for (auto& [id, e] : entries) {
            ej[id] = e.toJson();
        }
        config.conf["entries"] = ej;
        config.release(true);
    }

    void addEntry() {
        Entry e;
        e.id = generateId();
        e.name = "New entry";
        entries[e.id] = e;
        saveConfig();
    }

    // Deep-copies everything except id -- appends " (copy)" to the name, matching
    // RECORDING_SCHEDULER_PLAN.md section 4's Duplicate behavior. Resets status back to
    // "scheduled" too: a duplicate of an already-fired one-shot is a fresh entry, not a
    // continuation of the original's history.
    void duplicateEntry(const std::string& id) {
        auto it = entries.find(id);
        if (it == entries.end()) { return; }
        Entry e = it->second;
        e.id = generateId();
        e.name += " (copy)";
        e.status = "scheduled";
        e.selected = false;
        entries[e.id] = e;
        saveConfig();
    }

    void deleteEntry(const std::string& id) {
        entries.erase(id);
        saveConfig();
    }

    static void menuHandler(void* ctx) {
        RecordingSchedulerModule* _this = (RecordingSchedulerModule*)ctx;

        // Edit popup -- opened on double-click, matching frequency_manager's own
        // double-click-to-edit convention. Phase 1+2 scope: name, enabled, and the radio
        // section (pick a source, capture/view its settings). Recurrence and Recorder
        // sections are still phases 3-4.
        if (!_this->editedId.empty()) {
            gui::mainWindow.lockWaterfallControls = true;
            std::string popupId = "Edit Schedule Entry##recsched_edit_" + _this->name;
            ImGui::OpenPopup(popupId.c_str());
            if (ImGui::BeginPopup(popupId.c_str(), ImGuiWindowFlags_NoResize)) {
                auto it = _this->entries.find(_this->editedId);
                if (it == _this->entries.end()) {
                    _this->editedId.clear();
                }
                else {
                    ImGui::LeftLabel("Name");
                    if (ImGui::InputText(CONCAT("##recsched_edit_name_", _this->name), _this->editedName, sizeof(_this->editedName))) {}
                    ImGui::Checkbox(CONCAT("Enabled##recsched_edit_enabled_", _this->name), &it->second.enabled);

                    ImGui::Separator();
                    ImGui::TextUnformatted("Radio");

                    auto sourceNames = sigpath::sourceManager.getSourceNames();
                    int srcId = -1;
                    std::string srcItems;
                    for (size_t i = 0; i < sourceNames.size(); i++) {
                        if (sourceNames[i] == it->second.sourceName) { srcId = (int)i; }
                        srcItems += sourceNames[i];
                        srcItems += '\0';
                    }
                    srcItems += '\0';
                    int comboId = (srcId < 0) ? 0 : srcId;
                    ImGui::LeftLabel("Source");
                    ImGui::FillWidth();
                    if (!sourceNames.empty() && ImGui::Combo(CONCAT("##recsched_src_", _this->name), &comboId, srcItems.c_str())) {
                        it->second.sourceName = sourceNames[comboId];
                    }
                    if (sourceNames.empty()) { ImGui::TextDisabled("No radios currently registered."); }

                    bool haveSource = !it->second.sourceName.empty();
                    if (!haveSource) { style::beginDisabled(); }
                    if (ImGui::Button(CONCAT("Update from current settings##recsched_capture_", _this->name))) {
                        json snap = sigpath::sourceManager.captureSourceConfig(it->second.sourceName);
                        it->second.sourceConfigSnapshot = snap;
                        it->second.sourceModuleType = core::moduleManager.getInstanceModuleName(it->second.sourceName);
                        it->second.sourceConfigCapturedAt = snap.empty() ? 0 :
                            (int64_t)std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                        _this->saveConfig();
                    }
                    ImGui::SameLine();
                    bool haveSnapshot = !it->second.sourceConfigSnapshot.empty();
                    if (!haveSnapshot) { style::beginDisabled(); }
                    if (ImGui::Button(CONCAT("View saved settings##recsched_view_", _this->name))) {
                        _this->showViewSettings = true;
                    }
                    if (!haveSnapshot) { style::endDisabled(); }
                    if (!haveSource) { style::endDisabled(); }

                    if (haveSource && it->second.sourceConfigCapturedAt != 0) {
                        ImGui::Text("Settings captured for %s.", it->second.sourceName.c_str());
                    }

                    ImGui::Separator();
                    ImGui::TextUnformatted("Recorder");

                    // No dedicated "list interfaces of a type" API on ModuleComManager, but
                    // every instance's own module type is already tracked on ModuleManager
                    // regardless (RECORDING_SCHEDULER_PLAN.md section 2.3).
                    std::vector<std::string> recNames;
                    for (auto& [instName, inst] : core::moduleManager.instances) {
                        if (core::moduleManager.getInstanceModuleName(instName) == "recorder") {
                            recNames.push_back(instName);
                        }
                    }
                    int recId = -1;
                    std::string recItems;
                    for (size_t i = 0; i < recNames.size(); i++) {
                        if (recNames[i] == it->second.recorderName) { recId = (int)i; }
                        recItems += recNames[i];
                        recItems += '\0';
                    }
                    recItems += '\0';
                    int recComboId = (recId < 0) ? 0 : recId;
                    ImGui::LeftLabel("Recorder instance");
                    ImGui::FillWidth();
                    if (!recNames.empty() && ImGui::Combo(CONCAT("##recsched_rec_", _this->name), &recComboId, recItems.c_str())) {
                        it->second.recorderName = recNames[recComboId];
                        _this->saveConfig();
                    }
                    if (recNames.empty()) { ImGui::TextDisabled("No Recorder instances currently exist."); }

                    bool haveRecorder = !it->second.recorderName.empty();
                    if (!haveRecorder) { style::beginDisabled(); }
                    if (ImGui::Button(CONCAT("Reset to current Recorder settings##recsched_recreset_", _this->name))) {
                        if (core::modComManager.interfaceExists(it->second.recorderName)) {
                            json cfg;
                            core::modComManager.callInterface(it->second.recorderName, RECORDER_IFACE_CMD_GET_CONFIG, nullptr, &cfg);
                            it->second.recorderConfigSnapshot = cfg;
                            _this->saveConfig();
                        }
                    }
                    if (!haveRecorder) { style::endDisabled(); }

                    if (haveRecorder) {
                        json& rc = it->second.recorderConfigSnapshot;
                        bool recDirty = false;

                        int mode = rc.value("mode", 0);
                        if (ImGui::RadioButton(CONCAT("Baseband##recsched_recmode_bb_", _this->name), mode == 0)) { rc["mode"] = 0; recDirty = true; }
                        ImGui::SameLine();
                        if (ImGui::RadioButton(CONCAT("Audio##recsched_recmode_au_", _this->name), mode == 1)) { rc["mode"] = 1; recDirty = true; }

                        char pathBuf[1024];
                        strncpy(pathBuf, rc.value("recPath", std::string("")).c_str(), sizeof(pathBuf) - 1);
                        pathBuf[sizeof(pathBuf) - 1] = 0;
                        ImGui::LeftLabel("Folder");
                        ImGui::FillWidth();
                        if (ImGui::InputText(CONCAT("##recsched_recpath_", _this->name), pathBuf, sizeof(pathBuf))) {
                            rc["recPath"] = std::string(pathBuf);
                            recDirty = true;
                        }

                        char tzBuf[64];
                        strncpy(tzBuf, rc.value("timezone", std::string("local")).c_str(), sizeof(tzBuf) - 1);
                        tzBuf[sizeof(tzBuf) - 1] = 0;
                        ImGui::LeftLabel("Timezone (local/utc)");
                        ImGui::FillWidth();
                        if (ImGui::InputText(CONCAT("##recsched_rectz_", _this->name), tzBuf, sizeof(tzBuf))) {
                            rc["timezone"] = std::string(tzBuf);
                            recDirty = true;
                        }

                        ImGui::LeftLabel("Container");
                        // Only WAV is offered by the Recorder module's own UI today (RF64 is
                        // deliberately disabled there too) -- read-only here for the same reason.
                        ImGui::TextUnformatted(rc.value("container", std::string("WAV")).c_str());

                        static const struct { int value; const char* label; } sampleTypeOpts[] = {
                            { 0, "Uint8" }, { 1, "Int16" }, { 3, "Float32" }, { 2, "Int32" }
                        };
                        int sampleType = rc.value("sampleType", 1);
                        int stId = 1;
                        std::string stItems;
                        for (size_t i = 0; i < 4; i++) {
                            if (sampleTypeOpts[i].value == sampleType) { stId = (int)i; }
                            stItems += sampleTypeOpts[i].label;
                            stItems += '\0';
                        }
                        stItems += '\0';
                        ImGui::LeftLabel("Sample type");
                        ImGui::FillWidth();
                        if (ImGui::Combo(CONCAT("##recsched_recst_", _this->name), &stId, stItems.c_str())) {
                            rc["sampleType"] = sampleTypeOpts[stId].value;
                            recDirty = true;
                        }

                        char asBuf[256];
                        strncpy(asBuf, rc.value("audioStream", std::string("")).c_str(), sizeof(asBuf) - 1);
                        asBuf[sizeof(asBuf) - 1] = 0;
                        ImGui::LeftLabel("Audio stream (Audio mode)");
                        ImGui::FillWidth();
                        if (ImGui::InputText(CONCAT("##recsched_recas_", _this->name), asBuf, sizeof(asBuf))) {
                            rc["audioStream"] = std::string(asBuf);
                            recDirty = true;
                        }

                        bool stereo = rc.value("stereo", true);
                        if (ImGui::Checkbox(CONCAT("Stereo##recsched_recstereo_", _this->name), &stereo)) { rc["stereo"] = stereo; recDirty = true; }

                        bool ignoreSilence = rc.value("ignoreSilence", false);
                        if (ImGui::Checkbox(CONCAT("Ignore silence##recsched_recignsil_", _this->name), &ignoreSilence)) { rc["ignoreSilence"] = ignoreSilence; recDirty = true; }

                        bool dualChannel = rc.value("recordDualChannel", false);
                        if (ImGui::Checkbox(CONCAT("Dual channel##recsched_recdual_", _this->name), &dualChannel)) { rc["recordDualChannel"] = dualChannel; recDirty = true; }

                        char ntBuf[1024];
                        strncpy(ntBuf, rc.value("nameTemplate", std::string("$t_$f_$h-$m-$s_$d-$M-$y")).c_str(), sizeof(ntBuf) - 1);
                        ntBuf[sizeof(ntBuf) - 1] = 0;
                        ImGui::LeftLabel("Name template");
                        ImGui::FillWidth();
                        if (ImGui::InputText(CONCAT("##recsched_recnt_", _this->name), ntBuf, sizeof(ntBuf))) {
                            rc["nameTemplate"] = std::string(ntBuf);
                            recDirty = true;
                        }

                        if (recDirty) { _this->saveConfig(); }
                    }

                    ImGui::Separator();
                    ImGui::TextUnformatted("Schedule");

                    Recurrence& rec = it->second.recurrence;
                    bool recurDirty = false;

                    int typeId = (rec.type == "daily") ? 1 : (rec.type == "weekly") ? 2 : 0;
                    ImGui::LeftLabel("Repeats");
                    ImGui::FillWidth();
                    if (ImGui::Combo(CONCAT("##recsched_rectype_", _this->name), &typeId, "Once\0Daily\0Weekly\0")) {
                        rec.type = (typeId == 1) ? "daily" : (typeId == 2) ? "weekly" : "once";
                        recurDirty = true;
                    }

                    if (rec.type == "once") {
                        char startBuf[32];
                        strncpy(startBuf, formatDateTime(rec.startEpoch).c_str(), sizeof(startBuf) - 1);
                        startBuf[sizeof(startBuf) - 1] = 0;
                        ImGui::LeftLabel("Start (YYYY-MM-DD HH:MM:SS)");
                        ImGui::FillWidth();
                        if (ImGui::InputText(CONCAT("##recsched_recstart_", _this->name), startBuf, sizeof(startBuf))) {
                            rec.startEpoch = parseDateTime(startBuf);
                            recurDirty = true;
                        }

                        char stopBuf[32];
                        strncpy(stopBuf, formatDateTime(rec.stopEpoch).c_str(), sizeof(stopBuf) - 1);
                        stopBuf[sizeof(stopBuf) - 1] = 0;
                        ImGui::LeftLabel("Stop (YYYY-MM-DD HH:MM:SS)");
                        ImGui::FillWidth();
                        if (ImGui::InputText(CONCAT("##recsched_recstop_", _this->name), stopBuf, sizeof(stopBuf))) {
                            rec.stopEpoch = parseDateTime(stopBuf);
                            recurDirty = true;
                        }

                        if (rec.startEpoch != 0 && rec.stopEpoch != 0 && rec.stopEpoch <= rec.startEpoch) {
                            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.0f, 1.0f), "Stop should be after start.");
                        }
                    }
                    else {
                        char todBuf[16];
                        strncpy(todBuf, rec.startTimeOfDay.c_str(), sizeof(todBuf) - 1);
                        todBuf[sizeof(todBuf) - 1] = 0;
                        ImGui::LeftLabel("Start time of day (HH:MM:SS)");
                        ImGui::FillWidth();
                        if (ImGui::InputText(CONCAT("##recsched_rectod_", _this->name), todBuf, sizeof(todBuf))) {
                            rec.startTimeOfDay = std::string(todBuf);
                            recurDirty = true;
                        }

                        ImGui::LeftLabel("Duration (minutes)");
                        ImGui::FillWidth();
                        if (ImGui::InputInt(CONCAT("##recsched_recdur_", _this->name), &rec.durationMin)) {
                            if (rec.durationMin < 1) { rec.durationMin = 1; }
                            recurDirty = true;
                        }

                        if (rec.type == "weekly") {
                            static const char* dayLabels[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
                            ImGui::TextUnformatted("Days");
                            for (int i = 0; i < 7; i++) {
                                if (i > 0) { ImGui::SameLine(); }
                                std::string cbId = std::string(dayLabels[i]) + "##recsched_recdow_" + std::to_string(i) + "_" + _this->name;
                                if (ImGui::Checkbox(cbId.c_str(), &rec.daysOfWeek[i])) {
                                    recurDirty = true;
                                }
                            }
                        }
                    }

                    if (recurDirty) { _this->saveConfig(); }

                    ImGui::Separator();
                    ImGui::TextDisabled("Nothing fires automatically yet -- see");
                    ImGui::TextDisabled("RECORDING_SCHEDULER_PLAN.md phase 5.");

                    if (ImGui::Button(CONCAT("Apply##recsched_edit_apply_", _this->name))) {
                        it->second.name = _this->editedName;
                        _this->saveConfig();
                        _this->editedId.clear();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button(CONCAT("Cancel##recsched_edit_cancel_", _this->name))) {
                        _this->editedId.clear();
                    }

                    // Read-only dump of the captured snapshot -- deliberately just a raw
                    // JSON text block, not a per-field editor: this module doesn't know (and
                    // per RECORDING_SCHEDULER_PLAN.md section 2.2, deliberately doesn't need
                    // to know) what any of a given radio's fields mean. Never touches
                    // sigpath::sourceManager, so viewing this can't disturb whatever's
                    // actually running right now -- satisfies plan requirement 6.
                    if (_this->showViewSettings) {
                        std::string viewId = "Saved Settings##recsched_view_popup_" + _this->name;
                        ImGui::OpenPopup(viewId.c_str());
                        if (ImGui::BeginPopup(viewId.c_str(), ImGuiWindowFlags_NoResize)) {
                            std::string dump = it->second.sourceConfigSnapshot.dump(2);
                            ImGui::InputTextMultiline(CONCAT("##recsched_view_text_", _this->name),
                                                       (char*)dump.c_str(), dump.size() + 1,
                                                       ImVec2(400.0f * style::uiScale, 300.0f * style::uiScale),
                                                       ImGuiInputTextFlags_ReadOnly);
                            if (ImGui::Button(CONCAT("Close##recsched_view_close_", _this->name))) {
                                _this->showViewSettings = false;
                            }
                            ImGui::EndPopup();
                        }
                        else {
                            _this->showViewSettings = false;
                        }
                    }
                }
                ImGui::EndPopup();
            }
        }

        if (ImGui::BeginTable(CONCAT("recsched_table_", _this->name), 3,
                               ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                               ImVec2(0, 150.0f * style::uiScale))) {
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("Status");
            ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed, 30.0f * style::uiScale);
            ImGui::TableSetupScrollFreeze(3, 1);
            ImGui::TableHeadersRow();

            for (auto& [id, e] : _this->entries) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                if (ImGui::Selectable((e.name + "##recsched_row_" + id).c_str(), &e.selected,
                                       ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_SelectOnClick)) {
                    if (!ImGui::GetIO().KeyShift && !ImGui::GetIO().KeyCtrl) {
                        for (auto& [_id, _e] : _this->entries) {
                            if (_id != id) { _e.selected = false; }
                        }
                    }
                }
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && _this->editedId.empty()) {
                    _this->editedId = id;
                    strncpy(_this->editedName, e.name.c_str(), sizeof(_this->editedName) - 1);
                    _this->editedName[sizeof(_this->editedName) - 1] = '\0';
                }

                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(e.status.c_str());

                ImGui::TableSetColumnIndex(2);
                bool en = e.enabled;
                if (ImGui::Checkbox(CONCAT("##recsched_en_", id), &en)) {
                    e.enabled = en;
                    _this->saveConfig();
                }
            }
            ImGui::EndTable();
        }

        // Figure out which single entry (if any) is selected, for Duplicate/Delete -- both
        // are no-ops with nothing/multiple selected rather than guessing.
        std::string selId;
        int selCount = 0;
        for (auto& [id, e] : _this->entries) {
            if (e.selected) { selId = id; selCount++; }
        }
        bool haveOneSelected = (selCount == 1);

        if (ImGui::Button(CONCAT("Add##recsched_add_", _this->name))) {
            _this->addEntry();
        }
        ImGui::SameLine();
        if (!haveOneSelected) { style::beginDisabled(); }
        if (ImGui::Button(CONCAT("Duplicate##recsched_dup_", _this->name))) {
            _this->duplicateEntry(selId);
        }
        ImGui::SameLine();
        if (ImGui::Button(CONCAT("Delete##recsched_del_", _this->name))) {
            _this->showDeleteConfirm = true;
            _this->deleteTargetId = selId;
        }
        if (!haveOneSelected) { style::endDisabled(); }

        if (ImGui::GenericDialog(CONCAT("recsched_del_confirm_", _this->name), _this->showDeleteConfirm,
                                  GENERIC_DIALOG_BUTTONS_YES_NO, [_this]() {
                                      std::string nm = _this->entries.count(_this->deleteTargetId) ?
                                                        _this->entries[_this->deleteTargetId].name : "";
                                      ImGui::Text("Deleting schedule entry \"%s\". Are you sure?", nm.c_str());
                                  }) == GENERIC_DIALOG_BUTTON_YES) {
            _this->deleteEntry(_this->deleteTargetId);
        }

        ImGui::TextDisabled("This module doesn't schedule anything yet -- entries are just");
        ImGui::TextDisabled("stored/edited for now. See RECORDING_SCHEDULER_PLAN.md.");
    }

    std::string name;
    bool enabled = true;

    std::map<std::string, Entry> entries;

    std::string editedId;
    char editedName[1024] = { 0 };

    bool showDeleteConfirm = false;
    std::string deleteTargetId;

    bool showViewSettings = false;
};

MOD_EXPORT void _INIT_() {
    json def = json({});
    config.setPath(core::args["root"].s() + "/recording_scheduler_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new RecordingSchedulerModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (RecordingSchedulerModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
