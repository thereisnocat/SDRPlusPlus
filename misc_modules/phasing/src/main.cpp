#include <imgui.h>
#include <module.h>
#include <gui/gui.h>
#include <gui/style.h>
#include <gui/widgets/volume_meter.h>
#include <signal_path/signal_path.h>
#include <core.h>
#include <config.h>
#include <utils/flog.h>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

SDRPP_MOD_INFO{
    /* Name:            */ "phasing",
    /* Description:     */ "Antenna phasing and diversity combining",
    /* Author:          */ "Ralph Brandi",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ 1
};

ConfigManager config;

#define CONCAT(a, b) ((std::string(a) + b).c_str())

// Controls for combining two coherent receive channels: Y = A - w*B. See PHASING_PLAN.md
// section 2.3.
//
// This is deliberately radio-agnostic. Everything it touches is on sigpath::phasing, so
// any source that registers a ChannelSet gets these controls for free -- the Fobos, an
// RSPduo in dual tuner mode, a dual channel recording played back from a file.
//
// The controls are laid out for the way a null is actually found: a coarse control to get
// close and a fine one to sit in the notch, with the null depth on screen throughout.
// Phasing nulls are sharp, and a single 360-degree-wide phase slider is unusable at the
// bottom of one.

namespace {
    // A saved setting for one frequency. DXers null the same local pest on the same band
    // repeatedly; making them re-find it every session is the difference between a feature
    // that gets used and one that does not.
    struct Memory {
        double frequency = 0.0;
        int mode = 0;
        float gainDb = 0.0f;
        float phaseDeg = 0.0f;
        float delay = 0.0f;
        int chA = 0;
        int chB = 1;
    };

    std::string formatFreq(double hz) {
        char buf[64];
        if (hz >= 1e6) { snprintf(buf, sizeof(buf), "%.4f MHz", hz / 1e6); }
        else if (hz >= 1e3) { snprintf(buf, sizeof(buf), "%.3f kHz", hz / 1e3); }
        else { snprintf(buf, sizeof(buf), "%.0f Hz", hz); }
        return std::string(buf);
    }

    float wrap180(float deg) {
        while (deg > 180.0f) { deg -= 360.0f; }
        while (deg < -180.0f) { deg += 360.0f; }
        return deg;
    }
}

class PhasingModule : public ModuleManager::Instance {
public:
    PhasingModule(std::string name) {
        this->name = name;
        gui::menu.registerEntry(name, menuHandler, this);
    }

    ~PhasingModule() {
        gui::menu.removeEntry(name);
    }

    void postInit() {}
    void enable() { enabled = true; }
    void disable() { enabled = false; }
    bool isEnabled() { return enabled; }

private:
    // Settings live per source, so the weight that nulls a pest on one radio does not get
    // applied to a different one the next time it is selected.
    std::string settingsKey() {
        const std::string& src = sigpath::sourceManager.getSelectedName();
        return src.empty() ? "default" : src;
    }

    void loadSettings() {
        const std::string key = settingsKey();
        config.acquire();
        json& c = config.conf["sources"][key];
        if (c.contains("mode")) { mode = c["mode"]; }
        if (c.contains("gainDb")) { gainCoarse = c["gainDb"]; }
        if (c.contains("phaseDeg")) { phaseCoarse = c["phaseDeg"]; }
        if (c.contains("delay")) { delay = c["delay"]; }
        if (c.contains("adaptRate")) { adaptRate = c["adaptRate"]; }
        if (c.contains("refEnabled")) { refEnabled = c["refEnabled"]; }
        if (c.contains("refOffset")) { refOffset = c["refOffset"]; }
        if (c.contains("refWidth")) { refWidth = c["refWidth"]; }

        memories.clear();
        if (c.contains("memories")) {
            for (auto& m : c["memories"]) {
                Memory mem;
                mem.frequency = m.value("frequency", 0.0);
                mem.mode = m.value("mode", 0);
                mem.gainDb = m.value("gainDb", 0.0f);
                mem.phaseDeg = m.value("phaseDeg", 0.0f);
                mem.delay = m.value("delay", 0.0f);
                mem.chA = m.value("chA", 0);
                mem.chB = m.value("chB", 1);
                memories.push_back(mem);
            }
        }
        config.release();

        gainFine = 0.0f;
        phaseFine = 0.0f;
        loadedKey = key;
        applyToPhaser();
    }

    void saveSettings() {
        config.acquire();
        json& c = config.conf["sources"][settingsKey()];
        c["mode"] = mode;
        c["gainDb"] = gainCoarse;
        c["phaseDeg"] = phaseCoarse;
        c["delay"] = delay;
        c["adaptRate"] = adaptRate;
        c["refEnabled"] = refEnabled;
        c["refOffset"] = refOffset;
        c["refWidth"] = refWidth;
        json mems = json::array();
        for (const auto& m : memories) {
            json j;
            j["frequency"] = m.frequency;
            j["mode"] = m.mode;
            j["gainDb"] = m.gainDb;
            j["phaseDeg"] = m.phaseDeg;
            j["delay"] = m.delay;
            j["chA"] = m.chA;
            j["chB"] = m.chB;
            mems.push_back(j);
        }
        c["memories"] = mems;
        config.release(true);
    }

    float effectiveGain() { return std::clamp(gainCoarse + gainFine, -40.0f, 40.0f); }
    float effectivePhase() { return wrap180(phaseCoarse + phaseFine); }

    void applyToPhaser() {
        sigpath::phasing.setMode((dsp::combine::Phaser::Mode)mode);
        // Auto and hold own the weight themselves; pushing the UI's copy at them would
        // undo what the solver just worked out.
        if (mode != dsp::combine::Phaser::MODE_AUTO && mode != dsp::combine::Phaser::MODE_HOLD) {
            sigpath::phasing.setWeight(effectiveGain(), effectivePhase());
        }
        sigpath::phasing.setDelay(delay);
        sigpath::phasing.setAdaptRate(adaptRate);
        applyReferenceBand();
    }

    void applyReferenceBand() {
        sigpath::phasing.setSampleRate(sigpath::iqFrontEnd.getSampleRate());
        sigpath::phasing.setReferenceBand(refEnabled, refOffset, refWidth);
    }

    void recall(const Memory& m) {
        mode = m.mode;
        gainCoarse = m.gainDb;
        gainFine = 0.0f;
        phaseCoarse = m.phaseDeg;
        phaseFine = 0.0f;
        delay = m.delay;
        sigpath::phasing.setChannelPair(m.chA, m.chB);
        applyToPhaser();
        saveSettings();
    }

    static void menuHandler(void* ctx) {
        PhasingModule* _this = (PhasingModule*)ctx;
        const float menuWidth = ImGui::GetContentRegionAvail().x;

        if (!sigpath::phasing.isActive()) {
            ImGui::TextWrapped("No multi-channel source selected.");
            ImGui::TextWrapped("Phasing needs a radio offering two coherent channels, or a "
                               "dual channel recording played back from a file.");
            _this->wasActive = false;
            return;
        }

        // Reload when the source changes, so each radio keeps its own settings.
        if (!_this->wasActive || _this->loadedKey != _this->settingsKey()) {
            _this->loadSettings();
            _this->wasActive = true;
        }

        int chA = 0, chB = 1;
        sigpath::phasing.getChannelPair(chA, chB);

        // -- Channels ----------------------------------------------------------
        ImGui::Text("A: %s     B: %s",
                    sigpath::phasing.getChannelName(chA).c_str(),
                    sigpath::phasing.getChannelName(chB).c_str());
        ImGui::SameLine();
        ImGui::FillWidth();
        if (ImGui::Button(CONCAT("Swap##_phasing_swap_", _this->name))) {
            // Which antenna is the reference matters, and the answer is usually found by
            // trying both.
            sigpath::phasing.setChannelPair(chB, chA);
        }

        // -- Output ------------------------------------------------------------
        // Mode and monitor are one control: there is a single output stream, so choosing
        // what to listen to and choosing what the combiner does are the same choice.
        ImGui::LeftLabel("Output");
        ImGui::FillWidth();
        if (ImGui::Combo(CONCAT("##_phasing_mode_", _this->name), &_this->mode,
                         "Channel A only\0Channel B only\0Manual\0Auto-null\0Hold\0")) {
            _this->applyToPhaser();
            _this->saveSettings();
        }

        const bool combining = dsp::combine::Phaser::isCombining((dsp::combine::Phaser::Mode)_this->mode);
        const bool adapting = (_this->mode == dsp::combine::Phaser::MODE_AUTO);

        // While adapting, the weight belongs to the algorithm; the controls become a
        // readout. Editing them would be overwritten within a block anyway.
        if (adapting) {
            sigpath::phasing.getWeight(_this->gainCoarse, _this->phaseCoarse);
            _this->gainFine = 0.0f;
            _this->phaseFine = 0.0f;
        }
        if (!combining || adapting) { style::beginDisabled(); }

        // -- Gain --------------------------------------------------------------
        ImGui::LeftLabel("Gain");
        ImGui::FillWidth();
        if (ImGui::SliderFloat(CONCAT("##_phasing_gain_", _this->name), &_this->gainCoarse, -40.0f, 40.0f, "%.1f dB")) {
            _this->applyToPhaser();
            _this->saveSettings();
        }
        ImGui::LeftLabel("  fine");
        ImGui::FillWidth();
        if (ImGui::SliderFloat(CONCAT("##_phasing_gainf_", _this->name), &_this->gainFine, -1.0f, 1.0f, "%+.3f dB")) {
            _this->applyToPhaser();
        }

        // -- Phase -------------------------------------------------------------
        ImGui::LeftLabel("Phase");
        ImGui::FillWidth();
        if (ImGui::SliderFloat(CONCAT("##_phasing_phase_", _this->name), &_this->phaseCoarse, -180.0f, 180.0f, "%.1f deg")) {
            _this->applyToPhaser();
            _this->saveSettings();
        }
        ImGui::LeftLabel("  fine");
        ImGui::FillWidth();
        if (ImGui::SliderFloat(CONCAT("##_phasing_phasef_", _this->name), &_this->phaseFine, -5.0f, 5.0f, "%+.2f deg")) {
            _this->applyToPhaser();
        }

        // Typed entry, because a slider cannot reliably land on an exact figure: dragging
        // to "137 degrees" gives 136.98 or 137.02, and a phasing null is sharp enough that
        // the difference is visible in the meter. These are authoritative -- editing one
        // folds the vernier away and sets the value outright. The +/- buttons step by 0.01.
        float exactGain = _this->effectiveGain();
        float exactPhase = _this->effectivePhase();

        ImGui::LeftLabel("Exact dB");
        ImGui::FillWidth();
        if (ImGui::InputFloat(CONCAT("##_phasing_gainx_", _this->name), &exactGain, 0.01f, 0.1f, "%.2f")) {
            _this->gainCoarse = std::clamp(exactGain, -40.0f, 40.0f);
            _this->gainFine = 0.0f;
            _this->applyToPhaser();
            _this->saveSettings();
        }

        ImGui::LeftLabel("Exact deg");
        ImGui::FillWidth();
        if (ImGui::InputFloat(CONCAT("##_phasing_phasex_", _this->name), &exactPhase, 0.01f, 0.1f, "%.2f")) {
            _this->phaseCoarse = wrap180(exactPhase);
            _this->phaseFine = 0.0f;
            _this->applyToPhaser();
            _this->saveSettings();
        }

        if (ImGui::Button(CONCAT("Centre fine##_phasing_fold_", _this->name))) {
            // Fold the verniers into the coarse controls so they have full range again.
            _this->gainCoarse = _this->effectiveGain();
            _this->phaseCoarse = _this->effectivePhase();
            _this->gainFine = 0.0f;
            _this->phaseFine = 0.0f;
            _this->applyToPhaser();
            _this->saveSettings();
        }

        // -- Delay -------------------------------------------------------------
        ImGui::LeftLabel("Delay");
        ImGui::FillWidth();
        if (ImGui::SliderFloat(CONCAT("##_phasing_delay_", _this->name), &_this->delay, -16.0f, 16.0f, "%.3f samples")) {
            _this->applyToPhaser();
            _this->saveSettings();
        }

        if (!combining || adapting) { style::endDisabled(); }

        // -- Adaptation --------------------------------------------------------
        if (combining) {
            if (!adapting) { style::beginDisabled(); }
            ImGui::LeftLabel("Rate");
            ImGui::FillWidth();
            if (ImGui::SliderFloat(CONCAT("##_phasing_rate_", _this->name), &_this->adaptRate, 0.001f, 0.5f, "%.3f", ImGuiSliderFlags_Logarithmic)) {
                sigpath::phasing.setAdaptRate(_this->adaptRate);
                _this->saveSettings();
            }
            if (!adapting) { style::endDisabled(); }

            if (adapting) {
                // The workflow a phasing box imposes anyway: converge on the pest while
                // it is the loudest thing present, then lock before the wanted signal
                // comes up and the algorithm starts nulling that instead.
                if (ImGui::Button(CONCAT("Freeze##_phasing_freeze_", _this->name))) {
                    _this->mode = dsp::combine::Phaser::MODE_HOLD;
                    _this->applyToPhaser();
                    _this->saveSettings();
                }
                ImGui::SameLine();
                ImGui::TextWrapped("adapting");
            }
            else if (_this->mode == dsp::combine::Phaser::MODE_HOLD) {
                if (ImGui::Button(CONCAT("Resume##_phasing_resume_", _this->name))) {
                    _this->mode = dsp::combine::Phaser::MODE_AUTO;
                    _this->applyToPhaser();
                    _this->saveSettings();
                }
                ImGui::SameLine();
                if (ImGui::Button(CONCAT("Copy to manual##_phasing_tomanual_", _this->name))) {
                    sigpath::phasing.getWeight(_this->gainCoarse, _this->phaseCoarse);
                    _this->gainFine = 0.0f;
                    _this->phaseFine = 0.0f;
                    _this->mode = dsp::combine::Phaser::MODE_MANUAL;
                    _this->applyToPhaser();
                    _this->saveSettings();
                }
            }

            // Adapting on the whole band nulls whatever is loudest, which when the DX
            // peaks is the DX. Pointing the solver at a stretch containing only the pest
            // is the thing an SDR can do that an analogue phasing box cannot.
            if (ImGui::Checkbox(CONCAT("Reference band##_phasing_refen_", _this->name), &_this->refEnabled)) {
                _this->applyReferenceBand();
                _this->saveSettings();
            }
            if (_this->refEnabled) {
                ImGui::LeftLabel("  offset");
                ImGui::FillWidth();
                if (ImGui::InputDouble(CONCAT("##_phasing_refoff_", _this->name), &_this->refOffset, 1000.0, 10000.0, "%.0f Hz")) {
                    _this->applyReferenceBand();
                    _this->saveSettings();
                }
                ImGui::LeftLabel("  width");
                ImGui::FillWidth();
                if (ImGui::InputDouble(CONCAT("##_phasing_refwid_", _this->name), &_this->refWidth, 1000.0, 10000.0, "%.0f Hz")) {
                    _this->refWidth = std::max(_this->refWidth, 100.0);
                    _this->applyReferenceBand();
                    _this->saveSettings();
                }
                if (ImGui::Button(CONCAT("From VFO##_phasing_refvfo_", _this->name))) {
                    // Point it at whatever the user is looking at.
                    _this->refOffset = gui::waterfall.selectedVFO.empty() ? 0.0
                                     : gui::waterfall.vfos[gui::waterfall.selectedVFO]->generalOffset;
                    _this->applyReferenceBand();
                    _this->saveSettings();
                }
                ImGui::TextWrapped("Band-limiting is approximate; strong signals just "
                                   "outside it still pull on the estimate.");
            }
        }

        // -- Null depth --------------------------------------------------------
        const float depth = sigpath::phasing.getNullDepth();
        if (depth > _this->peakDepth) { _this->peakDepth = depth; }
        else { _this->peakDepth -= 0.25f; }   // slow decay so the best result stays visible
        _this->peakDepth = std::max(_this->peakDepth, 0.0f);

        const bool banded = sigpath::phasing.isNullDepthBandLimited();
        ImGui::LeftLabel(banded ? "Null depth (band)" : "Null depth (wide)");
        ImGui::Text("%.1f dB (best %.1f)", depth, _this->peakDepth);
        ImGui::FillWidth();
        ImGui::VolumeMeter(std::clamp(depth, 0.0f, 60.0f), std::clamp(_this->peakDepth, 0.0f, 60.0f), 0, 60);
        if (ImGui::Button(CONCAT("Reset peak##_phasing_peak_", _this->name))) { _this->peakDepth = 0.0f; }

        // The honest caveat. A deep notch at the VFO while the rest of the band barely
        // moves is correct behaviour for a single complex weight, and reads as a bug
        // unless it is said out loud.
        ImGui::TextWrapped("A single weight nulls deeply over a narrow span. Away from the "
                           "null frequency, cancellation falls off.");
        if (!banded) {
            // Whole-band depth answers "did total power drop", which is not the same
            // question as "did the pest go away", and can mark a correct null down.
            ImGui::TextWrapped("Measured across the whole band, so a weight that cancels one "
                               "signal but lifts another can score poorly. Set a reference "
                               "band to score the null where it matters.");
        }

        if (ImGui::Button(CONCAT("Reset combiner##_phasing_reset_", _this->name))) {
            // Settings persist per source, so a delay or weight left over from an earlier
            // session goes on quietly spoiling every null until it is noticed.
            _this->gainCoarse = 0.0f;
            _this->gainFine = 0.0f;
            _this->phaseCoarse = 0.0f;
            _this->phaseFine = 0.0f;
            _this->delay = 0.0f;
            _this->peakDepth = 0.0f;
            _this->applyToPhaser();
            _this->saveSettings();
        }

        // -- Memories ----------------------------------------------------------
        ImGui::Separator();
        ImGui::Text("Memories");

        std::string items;
        for (const auto& m : _this->memories) { items += formatFreq(m.frequency) + '\0'; }
        items += '\0';
        ImGui::FillWidth();
        if (!_this->memories.empty()) {
            ImGui::Combo(CONCAT("##_phasing_mem_", _this->name), &_this->memId, items.c_str());
        }
        else {
            ImGui::TextWrapped("None saved.");
        }

        if (ImGui::Button(CONCAT("Save##_phasing_memsave_", _this->name), ImVec2(menuWidth / 3.0f - 8, 0))) {
            Memory m;
            m.frequency = gui::waterfall.getCenterFrequency();
            m.mode = _this->mode;
            m.gainDb = _this->effectiveGain();
            m.phaseDeg = _this->effectivePhase();
            m.delay = _this->delay;
            m.chA = chA;
            m.chB = chB;
            _this->memories.push_back(m);
            _this->memId = (int)_this->memories.size() - 1;
            _this->saveSettings();
        }
        ImGui::SameLine();
        if (ImGui::Button(CONCAT("Recall##_phasing_memrec_", _this->name), ImVec2(menuWidth / 3.0f - 8, 0))) {
            if (_this->memId >= 0 && _this->memId < (int)_this->memories.size()) {
                _this->recall(_this->memories[_this->memId]);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(CONCAT("Delete##_phasing_memdel_", _this->name), ImVec2(menuWidth / 3.0f - 8, 0))) {
            if (_this->memId >= 0 && _this->memId < (int)_this->memories.size()) {
                _this->memories.erase(_this->memories.begin() + _this->memId);
                _this->memId = std::max(0, _this->memId - 1);
                _this->saveSettings();
            }
        }

        // -- Diagnostics -------------------------------------------------------
        const uint64_t discards = sigpath::phasing.getDiscardCount();
        if (discards) {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Dropped %llu samples", (unsigned long long)discards);
        }
        if (!sigpath::phasing.isPhaseCoherent()) {
            ImGui::TextWrapped("This source does not guarantee phase coherence across a "
                               "restart, so a recalled setting may need re-trimming.");
        }
    }

    std::string name;
    bool enabled = true;
    bool wasActive = false;
    std::string loadedKey;

    int mode = dsp::combine::Phaser::MODE_A_ONLY;
    float gainCoarse = 0.0f;
    float gainFine = 0.0f;
    float phaseCoarse = 0.0f;
    float phaseFine = 0.0f;
    float delay = 0.0f;
    float peakDepth = 0.0f;
    float adaptRate = 0.05f;
    bool refEnabled = false;
    double refOffset = 0.0;
    double refWidth = 20000.0;

    std::vector<Memory> memories;
    int memId = 0;
};

MOD_EXPORT void _INIT_() {
    json def = json({});
    def["sources"] = json({});
    config.setPath(core::args["root"].s() + "/phasing_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new PhasingModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (PhasingModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
