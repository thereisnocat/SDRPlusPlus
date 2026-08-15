// Recording Scheduler -- schedule recordings by start/stop time, per entry, against a chosen
// radio + Recorder settings. See RECORDING_SCHEDULER_PLAN.md at the repo root for the full
// design and phased build order. This file currently covers phases 0-1 only:
//
//   Phase 0: module skeleton -- builds/loads/toggles cleanly, empty menu panel.
//   Phase 1: entry data model, own persisted config file, Add/Duplicate/Delete/Enable list UI.
//
// Deliberately NOT here yet (later phases, see the plan doc): recurrence editing, radio
// settings capture/apply, Recorder settings capture/apply, and the engine thread that actually
// fires anything. An entry today is just a named, enabled/disabled, persisted, re-orderable
// row -- inert otherwise.
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
#include <map>
#include <string>
#include <chrono>
#include <atomic>
#include <cstring>

SDRPP_MOD_INFO{
    /* Name:            */ "recording_scheduler",
    /* Description:     */ "Schedule recordings by start/stop time, per radio+recorder settings",
    /* Author:          */ "Ralph Brandi",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ -1
};

ConfigManager config;

#define CONCAT(a, b) ((std::string(a) + b).c_str())

// One schedule entry. Deliberately minimal for phase 1 -- name/enabled/status only. The full
// schema (recurrence, source snapshot, recorder snapshot -- RECORDING_SCHEDULER_PLAN.md
// section 3) gets built out incrementally in later phases; keeping this struct small now
// rather than pre-declaring fields nothing reads or writes yet.
struct Entry {
    std::string id;      // stable, independent of display name -- see section 3
    std::string name = "New entry";
    bool enabled = true;
    std::string status = "scheduled";   // placeholder until phase 5's engine gives it meaning

    bool selected = false;

    json toJson() const {
        json j;
        j["name"] = name;
        j["enabled"] = enabled;
        j["status"] = status;
        return j;
    }

    static Entry fromJson(const std::string& id, const json& j) {
        Entry e;
        e.id = id;
        if (j.contains("name")) { e.name = j["name"]; }
        if (j.contains("enabled")) { e.enabled = j["enabled"]; }
        if (j.contains("status")) { e.status = j["status"]; }
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
        // double-click-to-edit convention. Phase 1 scope: name + enabled only.
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
                    ImGui::TextDisabled("Schedule/radio/recorder settings aren't wired up yet");
                    ImGui::TextDisabled("(RECORDING_SCHEDULER_PLAN.md phases 2-5).");

                    if (ImGui::Button(CONCAT("Apply##recsched_edit_apply_", _this->name))) {
                        it->second.name = _this->editedName;
                        _this->saveConfig();
                        _this->editedId.clear();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button(CONCAT("Cancel##recsched_edit_cancel_", _this->name))) {
                        _this->editedId.clear();
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
