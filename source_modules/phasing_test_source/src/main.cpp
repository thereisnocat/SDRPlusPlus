#include <imgui.h>
#include <module.h>
#include <gui/gui.h>
#include <gui/smgui.h>
#include <signal_path/signal_path.h>
#include <core.h>
#include <utils/optionlist.h>
#include <utils/flog.h>
#include "signal_model.h"
#include <complex>
#include <atomic>
#include <thread>
#include <chrono>
#include <mutex>
#include <cmath>

SDRPP_MOD_INFO{
    /* Name:            */ "phasing_test_source",
    /* Description:     */ "Two-channel phasing test signal generator",
    /* Author:          */ "Ralph Brandi",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ 1
};

ConfigManager config;

#define CONCAT(a, b) ((std::string(a) + b).c_str())

// Synthetic two-channel source for developing and regression-testing the phasing
// feature (see PHASING_PLAN.md, Phase 0). It models a receiver with two coherent
// antenna inputs: each generated signal appears in both channels with a known complex
// weight, so the weight that nulls it is known exactly in advance.
//
// That is the whole point. On the air you can null an interferer but you never learn
// what the correct weight was, so you cannot tell a converged adaptive algorithm from
// a lucky one. Here the answer is computed and displayed.
//
// All of the signal maths lives in signal_model.h, which has no SDR++ dependency and
// is covered by test/test_signal_model.cpp. This file is the SDR++ shell around it:
// GUI, config, threading, stream plumbing.

using namespace phtest;

class PhasingTestSourceModule : public ModuleManager::Instance {
public:
    PhasingTestSourceModule(std::string name) {
        this->name = name;

        // Lower rates keep the CPU cost of the whole downstream chain low while
        // developing; the higher ones mirror plausible Fobos HF settings.
        samplerates.define(250000, "250kHz", 250000.0);
        samplerates.define(500000, "500kHz", 500000.0);
        samplerates.define(1000000, "1.0MHz", 1000000.0);
        samplerates.define(2500000, "2.5MHz", 2500000.0);
        samplerates.define(5000000, "5.0MHz", 5000000.0);
        samplerates.define(10000000, "10.0MHz", 10000000.0);
        srId = samplerates.keyId(1000000);

        loadConfig();

        handler.ctx = this;
        handler.selectHandler = menuSelected;
        handler.deselectHandler = menuDeselected;
        handler.menuHandler = menuHandler;
        handler.startHandler = start;
        handler.stopHandler = stop;
        handler.tuneHandler = tune;
        handler.stream = &out;

        sigpath::sourceManager.registerSource("Phasing Test", &handler);
    }

    ~PhasingTestSourceModule() {
        stop(this);
        sigpath::sourceManager.unregisterSource("Phasing Test");
    }

    void postInit() {}
    void enable() { enabled = true; }
    void disable() { enabled = false; }
    bool isEnabled() { return enabled; }

private:
    // Snapshot the GUI-owned parameters for one block of generation. Taken once per
    // block rather than per sample, so the lock is uncontended in practice.
    Params snapshot() {
        std::lock_guard<std::mutex> lck(paramMtx);
        return params;
    }

    static void menuSelected(void* ctx) {
        PhasingTestSourceModule* _this = (PhasingTestSourceModule*)ctx;
        core::setInputSampleRate(_this->params.sampleRate);
        flog::info("PhasingTestSourceModule '{0}': Menu Select!", _this->name);
    }

    static void menuDeselected(void* ctx) {
        PhasingTestSourceModule* _this = (PhasingTestSourceModule*)ctx;
        flog::info("PhasingTestSourceModule '{0}': Menu Deselect!", _this->name);
    }

    static void start(void* ctx) {
        PhasingTestSourceModule* _this = (PhasingTestSourceModule*)ctx;
        if (_this->running) { return; }

        // 5ms blocks, bounded so neither end of the samplerate list gives a silly size.
        _this->blockSize = std::clamp<int>((int)(_this->params.sampleRate / 200.0), 256, STREAM_BUFFER_SIZE / 2);
        _this->gen.reset();

        _this->run = true;
        _this->workerThread = std::thread(&PhasingTestSourceModule::worker, _this);

        _this->running = true;
        flog::info("PhasingTestSourceModule '{0}': Start!", _this->name);
    }

    static void stop(void* ctx) {
        PhasingTestSourceModule* _this = (PhasingTestSourceModule*)ctx;
        if (!_this->running) { return; }
        _this->running = false;

        _this->run = false;
        _this->out.stopWriter();
        if (_this->workerThread.joinable()) { _this->workerThread.join(); }
        _this->out.clearWriteStop();

        flog::info("PhasingTestSourceModule '{0}': Stop!", _this->name);
    }

    static void tune(double freq, void* ctx) {
        PhasingTestSourceModule* _this = (PhasingTestSourceModule*)ctx;
        // Tone offsets are relative to centre, so retuning deliberately does not move
        // them. This keeps the test signals on screen while the rest of the app behaves
        // exactly as it would with a real radio.
        _this->freq = freq;
    }

    void worker() {
        auto nextBlock = std::chrono::steady_clock::now();

        while (run) {
            const Params p = snapshot();

            // dsp::complex_t is two floats, so the stream buffer is an interleaved
            // (re, im) float array as far as the generator is concerned.
            gen.generate(p, blockSize, (float*)out.writeBuf);

            if (!out.swap(blockSize)) { break; }

            // Pace to real time. Without this the generator would spin as fast as the
            // FFT path can consume it, which is neither a useful test condition nor a
            // kind thing to do to a CPU core.
            nextBlock += std::chrono::nanoseconds((int64_t)(1e9 * (double)blockSize / p.sampleRate));
            const auto now = std::chrono::steady_clock::now();
            if (nextBlock < now) { nextBlock = now; }
            std::this_thread::sleep_until(nextBlock);
        }
    }

    // Push a GUI-edited value into the worker-visible parameter block.
    template <typename T, typename U>
    void set(T Params::* field, U value) {
        std::lock_guard<std::mutex> lck(paramMtx);
        params.*field = (T)value;
    }

    static void menuHandler(void* ctx) {
        PhasingTestSourceModule* _this = (PhasingTestSourceModule*)ctx;
        char buf[256];
        bool dirty = false;

        // -- Sample rate -------------------------------------------------------
        if (_this->running) { SmGui::BeginDisabled(); }
        SmGui::LeftLabel("Samplerate");
        SmGui::FillWidth();
        SmGui::ForceSync();
        if (SmGui::Combo(CONCAT("##_phtest_sr_", _this->name), &_this->srId, _this->samplerates.txt)) {
            _this->set(&Params::sampleRate, _this->samplerates.value(_this->srId));
            core::setInputSampleRate(_this->samplerates.value(_this->srId));
            dirty = true;
        }
        if (_this->running) { SmGui::EndDisabled(); }

        // -- Wanted signal -----------------------------------------------------
        SmGui::Text("Wanted signal");
        if (SmGui::Checkbox(CONCAT("Enabled##_phtest_wen_", _this->name), &_this->wantedEnabled)) {
            _this->set(&Params::wantedEnabled, _this->wantedEnabled);
            dirty = true;
        }
        SmGui::LeftLabel("Offset (Hz)");
        SmGui::FillWidth();
        if (SmGui::InputInt(CONCAT("##_phtest_woff_", _this->name), &_this->wantedOffsetHz, 1000, 10000)) {
            _this->set(&Params::wantedOffset, _this->wantedOffsetHz);
            dirty = true;
        }
        SmGui::LeftLabel("Level");
        SmGui::FillWidth();
        if (SmGui::SliderFloat(CONCAT("##_phtest_wlvl_", _this->name), &_this->wantedLevelF, -100.0f, 0.0f, SmGui::FMT_STR_FLOAT_DB_ONE_DECIMAL)) {
            _this->set(&Params::wantedLevel, _this->wantedLevelF);
            dirty = true;
        }
        SmGui::LeftLabel("B Gain");
        SmGui::FillWidth();
        if (SmGui::SliderFloat(CONCAT("##_phtest_wg_", _this->name), &_this->wantedGainF, -40.0f, 40.0f, SmGui::FMT_STR_FLOAT_DB_TWO_DECIMAL)) {
            _this->set(&Params::wantedGain, _this->wantedGainF);
            dirty = true;
        }
        SmGui::LeftLabel("B Phase");
        SmGui::FillWidth();
        if (SmGui::SliderFloat(CONCAT("##_phtest_wp_", _this->name), &_this->wantedPhaseF, -180.0f, 180.0f, SmGui::FMT_STR_FLOAT_TWO_DECIMAL)) {
            _this->set(&Params::wantedPhase, _this->wantedPhaseF);
            dirty = true;
        }

        // -- Interferer --------------------------------------------------------
        SmGui::Text("Interferer");
        if (SmGui::Checkbox(CONCAT("Enabled##_phtest_ien_", _this->name), &_this->interfEnabled)) {
            _this->set(&Params::interfEnabled, _this->interfEnabled);
            dirty = true;
        }
        SmGui::LeftLabel("Offset (Hz)");
        SmGui::FillWidth();
        if (SmGui::InputInt(CONCAT("##_phtest_ioff_", _this->name), &_this->interfOffsetHz, 1000, 10000)) {
            _this->set(&Params::interfOffset, _this->interfOffsetHz);
            dirty = true;
        }
        SmGui::LeftLabel("Level");
        SmGui::FillWidth();
        if (SmGui::SliderFloat(CONCAT("##_phtest_ilvl_", _this->name), &_this->interfLevelF, -100.0f, 0.0f, SmGui::FMT_STR_FLOAT_DB_ONE_DECIMAL)) {
            _this->set(&Params::interfLevel, _this->interfLevelF);
            dirty = true;
        }
        SmGui::LeftLabel("B Gain");
        SmGui::FillWidth();
        if (SmGui::SliderFloat(CONCAT("##_phtest_ig_", _this->name), &_this->interfGainF, -40.0f, 40.0f, SmGui::FMT_STR_FLOAT_DB_TWO_DECIMAL)) {
            _this->set(&Params::interfGain, _this->interfGainF);
            dirty = true;
        }
        SmGui::LeftLabel("B Phase");
        SmGui::FillWidth();
        if (SmGui::SliderFloat(CONCAT("##_phtest_ip_", _this->name), &_this->interfPhaseF, -180.0f, 180.0f, SmGui::FMT_STR_FLOAT_TWO_DECIMAL)) {
            _this->set(&Params::interfPhase, _this->interfPhaseF);
            dirty = true;
        }

        // -- Channel B effects -------------------------------------------------
        SmGui::Text("Channel B");
        SmGui::LeftLabel("Delay (samp)");
        SmGui::FillWidth();
        if (SmGui::SliderFloat(CONCAT("##_phtest_dly_", _this->name), &_this->delaySamplesF, -16.0f, 16.0f, SmGui::FMT_STR_FLOAT_THREE_DECIMAL)) {
            _this->set(&Params::delaySamples, _this->delaySamplesF);
            dirty = true;
        }
        if (SmGui::Checkbox(CONCAT("Swap A/B##_phtest_swap_", _this->name), &_this->swapChannels)) {
            _this->set(&Params::swapChannels, _this->swapChannels);
            dirty = true;
        }

        // -- Noise -------------------------------------------------------------
        SmGui::Text("Noise (independent per channel)");
        if (SmGui::Checkbox(CONCAT("Enabled##_phtest_nen_", _this->name), &_this->noiseEnabled)) {
            _this->set(&Params::noiseEnabled, _this->noiseEnabled);
            dirty = true;
        }
        SmGui::LeftLabel("Level");
        SmGui::FillWidth();
        if (SmGui::SliderFloat(CONCAT("##_phtest_nlvl_", _this->name), &_this->noiseLevelF, -140.0f, -20.0f, SmGui::FMT_STR_FLOAT_DB_ONE_DECIMAL)) {
            _this->set(&Params::noiseLevel, _this->noiseLevelF);
            dirty = true;
        }

        // -- Test combiner -----------------------------------------------------
        // Stands in for the real phaser until Phase 1 exists, so this module can be
        // verified on its own: null the interferer and it should vanish from the
        // waterfall, leaving the wanted tone sitting on the noise floor.
        SmGui::Text("Test combiner");
        SmGui::LeftLabel("View");
        SmGui::FillWidth();
        SmGui::ForceSync();
        if (SmGui::Combo(CONCAT("##_phtest_view_", _this->name), &_this->viewId, "Channel A\0Channel B\0A - w*B\0")) {
            _this->set(&Params::view, _this->viewId);
            dirty = true;
        }
        SmGui::LeftLabel("w Gain");
        SmGui::FillWidth();
        if (SmGui::SliderFloat(CONCAT("##_phtest_cg_", _this->name), &_this->combGainF, -40.0f, 40.0f, SmGui::FMT_STR_FLOAT_DB_TWO_DECIMAL)) {
            _this->set(&Params::combGain, _this->combGainF);
            dirty = true;
        }
        SmGui::LeftLabel("w Phase");
        SmGui::FillWidth();
        if (SmGui::SliderFloat(CONCAT("##_phtest_cp_", _this->name), &_this->combPhaseF, -180.0f, 180.0f, SmGui::FMT_STR_FLOAT_TWO_DECIMAL)) {
            _this->set(&Params::combPhase, _this->combPhaseF);
            dirty = true;
        }

        const Params p = _this->snapshot();
        std::complex<double> nw;

        SmGui::FillWidth();
        SmGui::ForceSync();
        if (SmGui::Button(CONCAT("Null interferer##_phtest_ni_", _this->name))) {
            if (nullWeight(p, true, nw)) {
                _this->applyCombinerWeight(nw);
                dirty = true;
            }
        }
        SmGui::FillWidth();
        SmGui::ForceSync();
        if (SmGui::Button(CONCAT("Null wanted##_phtest_nws_", _this->name))) {
            if (nullWeight(p, false, nw)) {
                _this->applyCombinerWeight(nw);
                dirty = true;
            }
        }

        // The numbers the real phaser should converge to. Shown always, so they are
        // available as targets once the phasing module exists.
        if (nullWeight(p, true, nw)) {
            snprintf(buf, sizeof(buf), "Interferer nulls at: %+.2f dB, %+.2f deg",
                     20.0 * std::log10(std::abs(nw)), std::arg(nw) * (180.0 / M_PI));
        }
        else {
            snprintf(buf, sizeof(buf), "Interferer nulls at: n/a");
        }
        SmGui::Text(buf);

        if (nullWeight(p, false, nw)) {
            snprintf(buf, sizeof(buf), "Wanted nulls at:     %+.2f dB, %+.2f deg",
                     20.0 * std::log10(std::abs(nw)), std::arg(nw) * (180.0 / M_PI));
        }
        else {
            snprintf(buf, sizeof(buf), "Wanted nulls at:     n/a");
        }
        SmGui::Text(buf);

        // A non-zero delay makes the two tones demand different weights, which a single
        // complex weight cannot supply. That is precisely the wideband limitation the
        // multi-tap weight of a later phase has to solve, so flag when the test case has
        // deliberately been made unsolvable by a scalar.
        if (p.delaySamples != 0.0 && p.wantedEnabled && p.interfEnabled) {
            SmGui::Text("Delay != 0: no single w nulls both tones (wideband case)");
        }

        if (dirty) { _this->saveConfig(); }
    }

    void applyCombinerWeight(const std::complex<double>& w) {
        combGainF = (float)(20.0 * std::log10(std::abs(w)));
        combPhaseF = (float)(std::arg(w) * (180.0 / M_PI));
        viewId = VIEW_COMBINED;
        std::lock_guard<std::mutex> lck(paramMtx);
        params.combGain = combGainF;
        params.combPhase = combPhaseF;
        params.view = viewId;
    }

    void loadConfig() {
        config.acquire();
        json& c = config.conf;
        if (c.contains("samplerate")) {
            int sr = c["samplerate"];
            if (samplerates.keyExists(sr)) { srId = samplerates.keyId(sr); }
        }
        if (c.contains("wantedEnabled")) { wantedEnabled = c["wantedEnabled"]; }
        if (c.contains("wantedOffset")) { wantedOffsetHz = c["wantedOffset"]; }
        if (c.contains("wantedLevel")) { wantedLevelF = c["wantedLevel"]; }
        if (c.contains("wantedGain")) { wantedGainF = c["wantedGain"]; }
        if (c.contains("wantedPhase")) { wantedPhaseF = c["wantedPhase"]; }
        if (c.contains("interfEnabled")) { interfEnabled = c["interfEnabled"]; }
        if (c.contains("interfOffset")) { interfOffsetHz = c["interfOffset"]; }
        if (c.contains("interfLevel")) { interfLevelF = c["interfLevel"]; }
        if (c.contains("interfGain")) { interfGainF = c["interfGain"]; }
        if (c.contains("interfPhase")) { interfPhaseF = c["interfPhase"]; }
        if (c.contains("delaySamples")) { delaySamplesF = c["delaySamples"]; }
        if (c.contains("noiseEnabled")) { noiseEnabled = c["noiseEnabled"]; }
        if (c.contains("noiseLevel")) { noiseLevelF = c["noiseLevel"]; }
        if (c.contains("swapChannels")) { swapChannels = c["swapChannels"]; }
        if (c.contains("view")) { viewId = c["view"]; }
        if (c.contains("combGain")) { combGainF = c["combGain"]; }
        if (c.contains("combPhase")) { combPhaseF = c["combPhase"]; }
        config.release();

        // Mirror the GUI-facing values into the worker's parameter block.
        std::lock_guard<std::mutex> lck(paramMtx);
        params.sampleRate = samplerates.value(srId);
        params.wantedEnabled = wantedEnabled;
        params.wantedOffset = wantedOffsetHz;
        params.wantedLevel = wantedLevelF;
        params.wantedGain = wantedGainF;
        params.wantedPhase = wantedPhaseF;
        params.interfEnabled = interfEnabled;
        params.interfOffset = interfOffsetHz;
        params.interfLevel = interfLevelF;
        params.interfGain = interfGainF;
        params.interfPhase = interfPhaseF;
        params.delaySamples = delaySamplesF;
        params.noiseEnabled = noiseEnabled;
        params.noiseLevel = noiseLevelF;
        params.swapChannels = swapChannels;
        params.view = viewId;
        params.combGain = combGainF;
        params.combPhase = combPhaseF;
    }

    void saveConfig() {
        config.acquire();
        json& c = config.conf;
        c["samplerate"] = samplerates.key(srId);
        c["wantedEnabled"] = wantedEnabled;
        c["wantedOffset"] = wantedOffsetHz;
        c["wantedLevel"] = wantedLevelF;
        c["wantedGain"] = wantedGainF;
        c["wantedPhase"] = wantedPhaseF;
        c["interfEnabled"] = interfEnabled;
        c["interfOffset"] = interfOffsetHz;
        c["interfLevel"] = interfLevelF;
        c["interfGain"] = interfGainF;
        c["interfPhase"] = interfPhaseF;
        c["delaySamples"] = delaySamplesF;
        c["noiseEnabled"] = noiseEnabled;
        c["noiseLevel"] = noiseLevelF;
        c["swapChannels"] = swapChannels;
        c["view"] = viewId;
        c["combGain"] = combGainF;
        c["combPhase"] = combPhaseF;
        config.release(true);
    }

    std::string name;
    bool enabled = true;
    bool running = false;
    double freq = 0.0;

    SourceManager::SourceHandler handler;
    dsp::stream<dsp::complex_t> out;

    OptionList<int, double> samplerates;
    int srId = 0;

    // GUI-facing copies. SmGui needs int*/float*/bool*, and these are also what gets
    // persisted; the authoritative values the worker reads live in `params`.
    bool wantedEnabled = true;
    int wantedOffsetHz = 100000;
    float wantedLevelF = -20.0f;
    float wantedGainF = 0.0f;
    float wantedPhaseF = 0.0f;
    bool interfEnabled = true;
    int interfOffsetHz = -150000;
    float interfLevelF = -10.0f;
    float interfGainF = -3.0f;
    float interfPhaseF = 137.0f;
    float delaySamplesF = 0.0f;
    bool noiseEnabled = true;
    float noiseLevelF = -80.0f;
    bool swapChannels = false;
    int viewId = VIEW_A;
    float combGainF = 0.0f;
    float combPhaseF = 0.0f;

    // Worker-visible parameters, guarded by paramMtx.
    std::mutex paramMtx;
    Params params;

    // Worker state
    Generator gen;
    std::thread workerThread;
    std::atomic<bool> run = false;
    int blockSize = 4096;
};

MOD_EXPORT void _INIT_() {
    json def = json({});
    config.setPath(core::args["root"].s() + "/phasing_test_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new PhasingTestSourceModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (PhasingTestSourceModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
