#pragma once
#include <imgui.h>
#include <module.h>
#include <gui/gui.h>
#include <gui/style.h>
#include <signal_path/signal_path.h>
#include <config.h>
#include <dsp/chain.h>
#include <dsp/noise_reduction/noise_blanker.h>
#include <dsp/noise_reduction/fm_if.h>
#include <dsp/noise_reduction/power_squelch.h>
#include <dsp/noise_reduction/ctcss_squelch.h>
#include <dsp/multirate/rational_resampler.h>
#include <dsp/filter/deephasis.h>
#include <dsp/filter/tube_warmth.h>
#include <core.h>
#include <stdint.h>
#include <utils/optionlist.h>
#include <gui/widgets/mini_spectrum.h>
#include "radio_interface.h"
#include "demod.h"
#include "spectrum_preview.h"
#include "carrier_zoom_window.h"

ConfigManager config;

// Single, app-wide owner of the carrier-zoom feature (see CARRIER_ZOOM_PLAN.md and this class's
// own header) -- exactly one instance, shared by every RadioModule instance in this
// single-translation-unit module, same convention `config` right above already relies on.
CarrierZoomWindow gCarrierZoomWindow;

#define CONCAT(a, b) ((std::string(a) + b).c_str())

std::map<DeemphasisMode, double> deempTaus = {
    { DEEMP_MODE_22US, 22e-6 },
    { DEEMP_MODE_50US, 50e-6 },
    { DEEMP_MODE_75US, 75e-6 }
};

std::map<IFNRPreset, double> ifnrTaps = {
    { IFNR_PRESET_NOAA_APT, 9},
    { IFNR_PRESET_VOICE, 15 },
    { IFNR_PRESET_NARROW_BAND, 31 },
    { IFNR_PRESET_BROADCAST, 32 }
};

class RadioModule : public ModuleManager::Instance {
public:
    RadioModule(std::string name) {
        this->name = name;

        // Initialize option lists
        deempModes.define("None", DEEMP_MODE_NONE);
        deempModes.define("22us", DEEMP_MODE_22US);
        deempModes.define("50us", DEEMP_MODE_50US);
        deempModes.define("75us", DEEMP_MODE_75US);

        ifnrPresets.define("NOAA APT", IFNR_PRESET_NOAA_APT);
        ifnrPresets.define("Voice", IFNR_PRESET_VOICE);
        ifnrPresets.define("Narrow Band", IFNR_PRESET_NARROW_BAND);

        squelchModes.define("off", "Off", SQUELCH_MODE_OFF);
        squelchModes.define("power", "Power", SQUELCH_MODE_POWER);
        //squelchModes.define("snr", "SNR", SQUELCH_MODE_SNR);
        squelchModes.define("ctcss_mute", "CTCSS (Mute)", SQUELCH_MODE_CTCSS_MUTE);
        squelchModes.define("ctcss_decode", "CTCSS (Decode Only)", SQUELCH_MODE_CTCSS_DECODE);
        //squelchModes.define("dcs_mute", "DCS (Mute)", SQUELCH_MODE_DCS_MUTE);
        //squelchModes.define("dcs_decode", "DCS (Decode Only)", SQUELCH_MODE_DCS_DECODE);

        for (int i = 0; i < dsp::noise_reduction::_CTCSS_TONE_COUNT; i++) {
            float tone = dsp::noise_reduction::CTCSS_TONES[i];
            char buf[64];
            sprintf(buf, "%.1fHz", tone);
            ctcssTones.define((int)round(tone) * 10, buf, (dsp::noise_reduction::CTCSSTone)i);
        }
        ctcssTones.define(-1, "Any", dsp::noise_reduction::CTCSS_TONE_ANY);

        // Initialize the config if it doesn't exist
        bool created = false;
        config.acquire();
        if (!config.conf.contains(name)) {
            config.conf[name]["selectedDemodId"] = 1;
            created = true;
        }
        selectedDemodID = config.conf[name]["selectedDemodId"];
        config.release(created);

        // Initialize the VFO
        vfo = sigpath::vfoManager.createVFO(name, ImGui::WaterfallVFO::REF_CENTER, 0, 200000, 200000, 50000, 200000, false);
        onUserChangedBandwidthHandler.handler = vfoUserChangedBandwidthHandler;
        onUserChangedBandwidthHandler.ctx = this;
        vfo->wtfVFO->onUserChangedBandwidth.bindHandler(&onUserChangedBandwidthHandler);

        // Initialize the spectrum preview -- corrected to the real default bandwidth/offset by
        // selectDemodByID() below, this initial width is just a placeholder that's never
        // actually shown
        preview.init(name, vfo->getOffset(), 40000.0);

        // Initialize IF DSP chain
        ifChainOutputChanged.ctx = this;
        ifChainOutputChanged.handler = ifChainOutputChangeHandler;
        ifChain.init(vfo->output);

        nb.init(NULL, 500.0 / 24000.0, 10.0);
        fmnr.init(NULL, 32);
        powerSquelch.init(NULL, MIN_SQUELCH);

        ifChain.addBlock(&nb, false);
        ifChain.addBlock(&powerSquelch, false);
        ifChain.addBlock(&fmnr, false);

        // Initialize audio DSP chain
        afChain.init(&dummyAudioStream);

        ctcss.init(NULL, 50000.0);
        resamp.init(NULL, 250000.0, 48000.0);
        hpTaps = dsp::taps::highPass(300.0, 100.0, 48000.0);
        hpf.init(NULL, hpTaps);
        deemp.init(NULL, 50e-6, 48000.0);
        tubeWarmth.init(NULL, 48000.0);
        tubeWarmth.setWarmth(tubeWarmthAmount);
        tubeWarmth.setNoise(tubeWarmthNoise);

        afChain.addBlock(&ctcss, false);
        afChain.addBlock(&resamp, true);
        afChain.addBlock(&hpf, false);
        afChain.addBlock(&deemp, false);
        // Last in the chain, deliberately -- it colors the final, already-correct (de-
        // emphasized, high-passed) audio, rather than something upstream stages then have to
        // demodulate/filter through.
        afChain.addBlock(&tubeWarmth, false);

        // Initialize the sink
        srChangeHandler.ctx = this;
        srChangeHandler.handler = sampleRateChangeHandler;
        stream.init(afChain.out, &srChangeHandler, audioSampleRate);
        sigpath::sinkManager.registerStream(name, &stream);

        // Select the demodulator
        selectDemodByID((DemodID)selectedDemodID);

        // Start IF chain
        ifChain.start();

        // Start AF chain
        afChain.start();

        // Start stream, the rest was started when selecting the demodulator
        stream.start();

        // Register the menu
        gui::menu.registerEntry(name, menuHandler, this, this);

        // Register the module interface
        core::modComManager.registerInterface("radio", name, moduleInterfaceHandler, this);
    }

    ~RadioModule() {
        core::modComManager.unregisterInterface(name);
        gui::menu.removeEntry(name);
        stream.stop();
        if (enabled) {
            disable();
        }
        sigpath::sinkManager.unregisterStream(name);
    }

    void postInit() {}

    void enable() {
        enabled = true;
        if (!vfo) {
            vfo = sigpath::vfoManager.createVFO(name, ImGui::WaterfallVFO::REF_CENTER, 0, 200000, 200000, 50000, 200000, false);
            vfo->wtfVFO->onUserChangedBandwidth.bindHandler(&onUserChangedBandwidthHandler);
            preview.init(name, vfo->getOffset(), 40000.0);
        }
        ifChain.setInput(vfo->output, [=](dsp::stream<dsp::complex_t>* out){ ifChainOutputChangeHandler(out, this); });
        ifChain.start();
        selectDemodByID((DemodID)selectedDemodID);
        afChain.start();
    }

    void disable() {
        enabled = false;
        ifChain.stop();
        if (selectedDemod) { selectedDemod->stop(); }
        afChain.stop();
        preview.deinit();
        // This instance's own VFO is about to disappear -- if it currently owns the (single,
        // app-wide) carrier zoom window, close it rather than leaving it pointed at a source
        // that's going away out from under it.
        if (gCarrierZoomWindow.isOpenFor(name)) { gCarrierZoomWindow.close(); }
        if (vfo) { sigpath::vfoManager.deleteVFO(vfo); }
        vfo = NULL;
    }

    bool isEnabled() {
        return enabled;
    }

    std::string name;

    enum DemodID {
        RADIO_DEMOD_NFM,
        RADIO_DEMOD_WFM,
        RADIO_DEMOD_AM,
        RADIO_DEMOD_DSB,
        RADIO_DEMOD_USB,
        RADIO_DEMOD_CW,
        RADIO_DEMOD_LSB,
        RADIO_DEMOD_RAW,
        // Appended rather than inserted next to AM/DSB where it logically belongs: this value
        // is persisted as-is in config.conf[name]["selectedDemodId"], so inserting it earlier
        // would silently renumber every mode after it and reassign existing users' saved demod
        // selection to the wrong mode on their next launch. Must stay numerically equal to
        // RADIO_IFACE_MODE_SAM in radio_interface.h -- see that file's comment.
        RADIO_DEMOD_SAM,
        _RADIO_DEMOD_COUNT,
    };

private:
    static void menuHandler(void* ctx) {
        RadioModule* _this = (RadioModule*)ctx;

        if (!_this->enabled) { style::beginDisabled(); }

        float menuWidth = ImGui::GetContentRegionAvail().x;
        ImGui::BeginGroup();

        // Filter preview: spectrum around the carrier with a shaded, draggable passband. Offset
        // kept in sync with the real VFO's tuned frequency every frame here rather than through
        // an event -- setOffset() is cheap (just retunes a frequency translator) and this is
        // the only place in the module that runs every frame regardless of what changed. Width
        // is NOT touched here on purpose -- see previewWidthHz's own comment; it's set once at
        // mode selection and deliberately left alone for the rest of that mode's use.
        //
        // Uses wtfVFO->centerOffset, not vfo->getOffset() (== wtfVFO->generalOffset): those two
        // differ for USB/LSB, which tune from the lower/upper edge rather than the center
        // (getVFOReference() == REF_LOWER/REF_UPPER) -- generalOffset is the edge the frequency
        // readout shows, centerOffset is generalOffset re-based to the middle of the passband,
        // and centerOffset is what the real demod VFO's own offset is actually set from
        // (VFOManager::VFO::setOffset() calls dspVFO->setOffset(wtfVFO->centerOffset), never
        // generalOffset). Centering the preview on generalOffset instead, as an earlier version
        // of this did, drew the shaded passband straddling the carrier for USB/LSB -- looking
        // like it covered both sides -- while the real filter, correctly centered on
        // centerOffset, was actually and correctly entirely to one side the whole time. Only
        // the picture was wrong; using the same reference point the real VFO already uses
        // fixes the picture to match.
        if (_this->vfo) {
            _this->preview.setOffset(_this->vfo->wtfVFO->centerOffset);
        }
        int specSize = 0;
        float* specData = _this->preview.acquireFFT(specSize);
        // carrierOffsetHz: the plot is centered on centerOffset (see above), but the actual
        // carrier is centerOffset - generalOffset away from that for USB/LSB -- 0 for
        // REF_CENTER modes, where the two are the same point.
        double carrierOffsetHz = _this->vfo ? (_this->vfo->wtfVFO->generalOffset - _this->vfo->wtfVFO->centerOffset) : 0.0;
        // Edges can be dragged out to the mode's true ceiling (maxBandwidth), not just the
        // current Bandwidth field's value -- that field is a synced *readout* of the passband
        // now (see applyPassbandEdges()), not a cage around it. See RxVFO::clampPassband()'s
        // comment for why the DSP layer itself was changed to match.
        bool passbandChanged = _this->specWidget.draw(CONCAT("##_radio_spectrum_preview_", _this->name), ImVec2(menuWidth, 80.0f * style::uiScale),
                                                        specData, specSize, _this->preview.getWidth(),
                                                        &_this->passbandLo, &_this->passbandHi,
                                                        -_this->maxBandwidth / 2.0, _this->maxBandwidth / 2.0, carrierOffsetHz);
        _this->preview.releaseFFT();
        if (passbandChanged) { _this->applyPassbandEdges(); }

        // Carrier zoom trigger (see CARRIER_ZOOM_PLAN.md) -- IsItemHovered()/IsItemClicked()
        // refer to the last-pushed ImGui item, which is specWidget.draw()'s own internal
        // ImGui::Dummy(size) hit-test proxy (mini_spectrum.cpp); nothing drawn between there and
        // here changes that. Seeds the initial offset from the same wtfVFO->centerOffset the
        // preview itself is centered on (see the comment above this block).
        if (ImGui::IsItemHovered() && (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) || ImGui::IsItemClicked(ImGuiMouseButton_Right))) {
            // absoluteFreqHz: waterfall center + the same centerOffset passed as the VFO tap's
            // own offset -- centerOffset, not generalOffset, per this function's own comment
            // above on why those differ for USB/LSB (see CARRIER_PEAK_LABELS_PLAN.md's "Why this
            // is feasible" section for the full reasoning on why this one, specifically, is the
            // correct zero-point for a carrier-offset label).
            double centerOffset = _this->vfo ? _this->vfo->wtfVFO->centerOffset : 0.0;
            gCarrierZoomWindow.open(_this->name, centerOffset, gui::waterfall.getCenterFrequency() + centerOffset);
        }
        gCarrierZoomWindow.draw(_this->name);

        ImGui::Columns(4, CONCAT("RadioModeColumns##_", _this->name), false);
        if (ImGui::RadioButton(CONCAT("NFM##_", _this->name), _this->selectedDemodID == 0) && _this->selectedDemodID != 0) {
            _this->selectDemodByID(RADIO_DEMOD_NFM);
        }
        if (ImGui::RadioButton(CONCAT("WFM##_", _this->name), _this->selectedDemodID == 1) && _this->selectedDemodID != 1) {
            _this->selectDemodByID(RADIO_DEMOD_WFM);
        }
        ImGui::NextColumn();
        if (ImGui::RadioButton(CONCAT("AM##_", _this->name), _this->selectedDemodID == 2) && _this->selectedDemodID != 2) {
            _this->selectDemodByID(RADIO_DEMOD_AM);
        }
        // Grouped visually with AM/DSB since that's where a user would look for it, even though
        // its enum value (and column position below) had to be appended at the end -- see the
        // comment on RADIO_DEMOD_SAM's declaration.
        if (ImGui::RadioButton(CONCAT("SAM##_", _this->name), _this->selectedDemodID == RADIO_DEMOD_SAM) && _this->selectedDemodID != RADIO_DEMOD_SAM) {
            _this->selectDemodByID(RADIO_DEMOD_SAM);
        }
        if (ImGui::RadioButton(CONCAT("DSB##_", _this->name), _this->selectedDemodID == 3) && _this->selectedDemodID != 3) {
            _this->selectDemodByID(RADIO_DEMOD_DSB);
        }
        ImGui::NextColumn();
        if (ImGui::RadioButton(CONCAT("USB##_", _this->name), _this->selectedDemodID == 4) && _this->selectedDemodID != 4) {
            _this->selectDemodByID(RADIO_DEMOD_USB);
        }
        if (ImGui::RadioButton(CONCAT("CW##_", _this->name), _this->selectedDemodID == 5) && _this->selectedDemodID != 5) {
            _this->selectDemodByID(RADIO_DEMOD_CW);
        };
        ImGui::NextColumn();
        if (ImGui::RadioButton(CONCAT("LSB##_", _this->name), _this->selectedDemodID == 6) && _this->selectedDemodID != 6) {
            _this->selectDemodByID(RADIO_DEMOD_LSB);
        }
        if (ImGui::RadioButton(CONCAT("RAW##_", _this->name), _this->selectedDemodID == 7) && _this->selectedDemodID != 7) {
            _this->selectDemodByID(RADIO_DEMOD_RAW);
        };
        ImGui::Columns(1, CONCAT("EndRadioModeColumns##_", _this->name), false);

        ImGui::EndGroup();

        if (!_this->bandwidthLocked) {
            ImGui::LeftLabel("Bandwidth");
            ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
            if (ImGui::InputFloat(("##_radio_bw_" + _this->name).c_str(), &_this->bandwidth, 1, 100, "%.0f")) {
                _this->bandwidth = std::clamp<float>(_this->bandwidth, _this->minBandwidth, _this->maxBandwidth);
                _this->setBandwidthSymmetric(_this->bandwidth);
            }
        }

        // VFO snap interval
        ImGui::LeftLabel("Snap Interval");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::InputInt(("##_radio_snap_" + _this->name).c_str(), &_this->snapInterval, 1, 100)) {
            if (_this->snapInterval < 1) { _this->snapInterval = 1; }
            _this->vfo->setSnapInterval(_this->snapInterval);
            config.acquire();
            config.conf[_this->name][_this->selectedDemod->getName()]["snapInterval"] = _this->snapInterval;
            config.release(true);
        }

        // Deemphasis mode
        if (_this->deempAllowed) {
            ImGui::LeftLabel("De-emphasis");
            ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
            if (ImGui::Combo(("##_radio_wfm_deemp_" + _this->name).c_str(), &_this->deempId, _this->deempModes.txt)) {
                _this->setDeemphasisMode(_this->deempModes[_this->deempId]);
            }
        }

        // Squelch
        if (_this->squelchAllowed) {
            ImGui::LeftLabel("Squelch Mode");
            ImGui::FillWidth();
            if (ImGui::Combo(("##_radio_sqelch_mode_" + _this->name).c_str(), &_this->squelchModeId, _this->squelchModes.txt)) {
                _this->setSquelchMode(_this->squelchModes[_this->squelchModeId]);
            }
            switch (_this->squelchModes[_this->squelchModeId]) {
            case SQUELCH_MODE_POWER:
                ImGui::LeftLabel("Squelch Level");
                ImGui::FillWidth();
                if (ImGui::SliderFloat(("##_radio_sqelch_lvl_" + _this->name).c_str(), &_this->squelchLevel, _this->MIN_SQUELCH, _this->MAX_SQUELCH, "%.3fdB")) {
                    _this->setSquelchLevel(_this->squelchLevel);
                }
                break;

            case SQUELCH_MODE_CTCSS_MUTE:
                if (_this->squelchModes[_this->squelchModeId] == SQUELCH_MODE_CTCSS_MUTE) {
                    ImGui::LeftLabel("CTCSS Tone");
                    ImGui::FillWidth();
                    if (ImGui::Combo(("##_radio_ctcss_tone_" + _this->name).c_str(), &_this->ctcssToneId, _this->ctcssTones.txt)) {
                        _this->setCTCSSTone(_this->ctcssTones[_this->ctcssToneId]);
                    }
                }
            }
        }

        // Noise blanker
        if (_this->nbAllowed) {
            if (ImGui::Checkbox(("Noise blanker (W.I.P.)##_radio_nb_ena_" + _this->name).c_str(), &_this->nbEnabled)) {
                _this->setNBEnabled(_this->nbEnabled);
            }
            if (!_this->nbEnabled && _this->enabled) { style::beginDisabled(); }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
            if (ImGui::SliderFloat(("##_radio_nb_lvl_" + _this->name).c_str(), &_this->nbLevel, _this->MIN_NB, _this->MAX_NB, "%.3fdB")) {
                _this->setNBLevel(_this->nbLevel);
            }
            if (!_this->nbEnabled && _this->enabled) { style::endDisabled(); }
        }

        // FM IF Noise Reduction
        if (_this->FMIFNRAllowed) {
            if (ImGui::Checkbox(("IF Noise Reduction##_radio_fmifnr_ena_" + _this->name).c_str(), &_this->FMIFNREnabled)) {
                _this->setFMIFNREnabled(_this->FMIFNREnabled);
            }
            if (_this->selectedDemodID == RADIO_DEMOD_NFM) {
                if (!_this->FMIFNREnabled && _this->enabled) { style::beginDisabled(); }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
                if (ImGui::Combo(("##_radio_fmifnr_ena_" + _this->name).c_str(), &_this->fmIFPresetId, _this->ifnrPresets.txt)) {
                    _this->setIFNRPreset(_this->ifnrPresets[_this->fmIFPresetId]);
                }
                if (!_this->FMIFNREnabled && _this->enabled) { style::endDisabled(); }
            }
        }

        // High pass
        if (_this->highPassAllowed) {
            if (ImGui::Checkbox(("High Pass##_radio_hpf_" + _this->name).c_str(), &_this->highPass)) {
                _this->setHighPass(_this->highPass);
            }
        }

        // Tube Warmth
        if (_this->highPassAllowed) {
            if (ImGui::Checkbox(("Tube Warmth##_radio_tubewarmth_" + _this->name).c_str(), &_this->tubeWarmthEnabled)) {
                _this->setTubeWarmth(_this->tubeWarmthEnabled);
            }
            if (_this->tubeWarmthEnabled) {
                ImGui::LeftLabel("Warmth");
                ImGui::FillWidth();
                if (ImGui::SliderFloat(("##_radio_tubewarmth_amt_" + _this->name).c_str(), &_this->tubeWarmthAmount, 0.0f, 1.0f)) {
                    _this->setTubeWarmthAmount(_this->tubeWarmthAmount);
                }
                ImGui::LeftLabel("Noise");
                ImGui::FillWidth();
                if (ImGui::SliderFloat(("##_radio_tubewarmth_noise_" + _this->name).c_str(), &_this->tubeWarmthNoise, 0.0f, 1.0f)) {
                    _this->setTubeWarmthNoise(_this->tubeWarmthNoise);
                }
            }
        }

        // Demodulator specific menu
        _this->selectedDemod->showMenu();

        // Display the squelch diagnostics
        switch (_this->squelchModes[_this->squelchModeId]) {
        case SQUELCH_MODE_CTCSS_MUTE:
            ImGui::TextUnformatted("Received Tone:");
            ImGui::SameLine();
            {
                auto ctone = _this->ctcss.getCurrentTone();
                auto dtone = _this->ctcssTones[_this->ctcssToneId];
                if (ctone != dsp::noise_reduction::CTCSS_TONE_NONE) {
                    if (dtone == dsp::noise_reduction::CTCSS_TONE_ANY || ctone == dtone) {
                        ImGui::TextColored(ImVec4(0, 1, 0, 1), "%.1fHz", dsp::noise_reduction::CTCSS_TONES[_this->ctcss.getCurrentTone()]);
                    }
                    else {
                        ImGui::TextColored(ImVec4(1, 0, 0, 1), "%.1fHz", dsp::noise_reduction::CTCSS_TONES[_this->ctcss.getCurrentTone()]);
                    }
                }
                else {
                    ImGui::TextUnformatted("None");
                }
            }
            break;
            
        case SQUELCH_MODE_CTCSS_DECODE:
            ImGui::TextUnformatted("Received Tone:");
            ImGui::SameLine();
            {
                auto ctone = _this->ctcss.getCurrentTone();
                if (ctone != dsp::noise_reduction::CTCSS_TONE_NONE) {
                    ImGui::TextColored(ImVec4(0, 1, 0, 1), "%.1fHz", dsp::noise_reduction::CTCSS_TONES[_this->ctcss.getCurrentTone()]);
                }
                else {
                    ImGui::TextUnformatted("None");
                }
            }
            break;
        }

        if (!_this->enabled) { style::endDisabled(); }
    }

    demod::Demodulator* instantiateDemod(DemodID id) {
        demod::Demodulator* demod = NULL;
        switch (id) {
            case DemodID::RADIO_DEMOD_NFM:  demod = new demod::NFM(); break;
            case DemodID::RADIO_DEMOD_WFM:  demod = new demod::WFM(); break;
            case DemodID::RADIO_DEMOD_AM:   demod = new demod::AM();  break;
            case DemodID::RADIO_DEMOD_SAM:  demod = new demod::SAM(); break;
            case DemodID::RADIO_DEMOD_DSB:  demod = new demod::DSB(); break;
            case DemodID::RADIO_DEMOD_USB:  demod = new demod::USB(); break;
            case DemodID::RADIO_DEMOD_CW:   demod = new demod::CW();  break;
            case DemodID::RADIO_DEMOD_LSB:  demod = new demod::LSB(); break;
            case DemodID::RADIO_DEMOD_RAW:  demod = new demod::RAW(); break;
            default:                        demod = NULL;             break;
        }
        if (!demod) { return NULL; }

        // Default config
        double bw = demod->getDefaultBandwidth();
        config.acquire();
        if (!config.conf[name].contains(demod->getName())) {
            config.conf[name][demod->getName()]["bandwidth"] = bw;
            config.conf[name][demod->getName()]["snapInterval"] = demod->getDefaultSnapInterval();
            config.conf[name][demod->getName()]["squelchLevel"] = MIN_SQUELCH;
            config.conf[name][demod->getName()]["squelchEnabled"] = false;
            config.release(true);
        }
        else {
            config.release();
        }
        bw = std::clamp<double>(bw, demod->getMinBandwidth(), demod->getMaxBandwidth());

        // Initialize
        demod->init(name, &config, ifChain.out, bw, stream.getSampleRate());

        return demod;
    }

    void selectDemodByID(DemodID id) {
        auto startTime = std::chrono::high_resolution_clock::now();
        demod::Demodulator* demod = instantiateDemod(id);
        if (!demod) {
            flog::error("Demodulator {0} not implemented", (int)id);
            return;
        }
        selectedDemodID = id;
        selectDemod(demod);

        // Save config
        config.acquire();
        config.conf[name]["selectedDemodId"] = id;
        config.release(true);
        auto endTime = std::chrono::high_resolution_clock::now();
        flog::warn("Demod switch took {0} us", (int64_t)((std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime)).count()));
    }

    void selectDemod(demod::Demodulator* demod) {
        // Stopcurrently selected demodulator and select new
        afChain.setInput(&dummyAudioStream, [=](dsp::stream<dsp::stereo_t>* out){ stream.setInput(out); });
        if (selectedDemod) {
            selectedDemod->stop();
            delete selectedDemod;
        }
        selectedDemod = demod;

        // Give the demodulator the most recent audio SR
        selectedDemod->AFSampRateChanged(audioSampleRate);

        // Set the demodulator's input
        selectedDemod->setInput(ifChain.out);

        // Set AF chain's input
        afChain.setInput(selectedDemod->getOutput(), [=](dsp::stream<dsp::stereo_t>* out){ stream.setInput(out); });

        // Load config
        bandwidth = selectedDemod->getDefaultBandwidth();
        minBandwidth = selectedDemod->getMinBandwidth();
        maxBandwidth = selectedDemod->getMaxBandwidth();
        bandwidthLocked = selectedDemod->getBandwidthLocked();
        snapInterval = selectedDemod->getDefaultSnapInterval();
        deempAllowed = selectedDemod->getDeempAllowed();
        deempId = deempModes.valueId((DeemphasisMode)selectedDemod->getDefaultDeemphasisMode());
        squelchModeId = squelchModes.valueId(SQUELCH_MODE_OFF);
        squelchLevel = MIN_SQUELCH;
        ctcssToneId = ctcssTones.valueId(dsp::noise_reduction::CTCSS_TONE_67Hz);
        highPass = false;
        tubeWarmthEnabled = false;
        tubeWarmthAmount = 0.5f;
        tubeWarmthNoise = 0.15f;

        postProcEnabled = selectedDemod->getPostProcEnabled();
        FMIFNRAllowed = selectedDemod->getFMIFNRAllowed();
        FMIFNREnabled = false;
        fmIFPresetId = ifnrPresets.valueId(IFNR_PRESET_VOICE);
        nbAllowed = selectedDemod->getNBAllowed();
        squelchAllowed = selectedDemod->getSquelchAllowed();
        highPassAllowed = selectedDemod->getHighPassAllowed();
        nbEnabled = false;
        nbLevel = 0.0f;
        double ifSamplerate = selectedDemod->getIFSampleRate();
        config.acquire();
        if (config.conf[name][selectedDemod->getName()].contains("bandwidth")) {
            bandwidth = config.conf[name][selectedDemod->getName()]["bandwidth"];
            bandwidth = std::clamp<double>(bandwidth, minBandwidth, maxBandwidth);
        }
        // Default every mode switch to the full symmetric window (today's exact behaviour if
        // this mode's passband was never trimmed), then load whatever was actually saved for
        // this specific mode, clamped in case the saved value predates a since-changed
        // bandwidth. Applied further down by applyPassbandEdges(), after setBandwidth(bandwidth)
        // -- the VFO's clamp needs the new bandwidth window in place first.
        passbandLo = -bandwidth / 2.0;
        passbandHi = bandwidth / 2.0;
        if (config.conf[name][selectedDemod->getName()].contains("passbandLo")) {
            passbandLo = std::clamp<double>(config.conf[name][selectedDemod->getName()]["passbandLo"], -bandwidth / 2.0, 0.0);
        }
        if (config.conf[name][selectedDemod->getName()].contains("passbandHi")) {
            passbandHi = std::clamp<double>(config.conf[name][selectedDemod->getName()]["passbandHi"], 0.0, bandwidth / 2.0);
        }
        // Re-derive bandwidth from whatever the passband actually ended up being, rather than
        // trusting the separately-persisted "bandwidth" key on its own -- found live testing
        // this same day that the two can disagree: every build prior to this fix persisted
        // `bandwidth` from the live-dragged passband (see applyPassbandEdges()'s own history),
        // so a config that was ever saved by one of those earlier builds can have a
        // `bandwidth` stuck at a stale value with no real relationship to the (separately,
        // correctly loaded) passbandLo/Hi above -- observed directly in this fix's own test
        // config: AM's "bandwidth" stuck at 15000 (its IF-rate ceiling) while passbandLo/Hi
        // still correctly held a real ~8kHz trim. Harmless no-op for a freshly-defaulted
        // passband (passbandHi - passbandLo is exactly bandwidth by construction in that
        // case); only changes anything for a config with this exact leftover mismatch.
        bandwidth = std::clamp<double>(passbandHi - passbandLo, minBandwidth, maxBandwidth);
        if (config.conf[name][selectedDemod->getName()].contains("snapInterval")) {
            snapInterval = config.conf[name][selectedDemod->getName()]["snapInterval"];
        }

        if (config.conf[name][selectedDemod->getName()].contains("squelchMode")) {
            std::string squelchModeStr = config.conf[name][selectedDemod->getName()]["squelchMode"];
            if (squelchModes.keyExists(squelchModeStr)) {
                squelchModeId = squelchModes.keyId(squelchModeStr);
            }
        }
        if (config.conf[name][selectedDemod->getName()].contains("squelchLevel")) {
            squelchLevel = config.conf[name][selectedDemod->getName()]["squelchLevel"];
        }
        if (config.conf[name][selectedDemod->getName()].contains("ctcssTone")) {
            int ctcssToneX10 = config.conf[name][selectedDemod->getName()]["ctcssTone"];
            if (ctcssTones.keyExists(ctcssToneX10)) {
                ctcssToneId = ctcssTones.keyId(ctcssToneX10);
            }
        }
        if (config.conf[name][selectedDemod->getName()].contains("highPass")) {
            highPass = config.conf[name][selectedDemod->getName()]["highPass"];
        }
        if (config.conf[name][selectedDemod->getName()].contains("tubeWarmthEnabled")) {
            tubeWarmthEnabled = config.conf[name][selectedDemod->getName()]["tubeWarmthEnabled"];
        }
        if (config.conf[name][selectedDemod->getName()].contains("tubeWarmthAmount")) {
            tubeWarmthAmount = config.conf[name][selectedDemod->getName()]["tubeWarmthAmount"];
        }
        if (config.conf[name][selectedDemod->getName()].contains("tubeWarmthNoise")) {
            tubeWarmthNoise = config.conf[name][selectedDemod->getName()]["tubeWarmthNoise"];
        }
        if (config.conf[name][selectedDemod->getName()].contains("deempMode")) {
            if (!config.conf[name][selectedDemod->getName()]["deempMode"].is_string()) {
                config.conf[name][selectedDemod->getName()]["deempMode"] = deempModes.key(deempId);
            }

            std::string deempOpt = config.conf[name][selectedDemod->getName()]["deempMode"];
            if (deempModes.keyExists(deempOpt)) {
                deempId = deempModes.keyId(deempOpt);
            }
        }
        if (config.conf[name][selectedDemod->getName()].contains("FMIFNREnabled")) {
            FMIFNREnabled = config.conf[name][selectedDemod->getName()]["FMIFNREnabled"];
        }
        if (config.conf[name][selectedDemod->getName()].contains("fmifnrPreset")) {
            std::string presetOpt = config.conf[name][selectedDemod->getName()]["fmifnrPreset"];
            if (ifnrPresets.keyExists(presetOpt)) {
                fmIFPresetId = ifnrPresets.keyId(presetOpt);
            }
        }
        if (config.conf[name][selectedDemod->getName()].contains("noiseBlankerEnabled")) {
            nbEnabled = config.conf[name][selectedDemod->getName()]["noiseBlankerEnabled"];
        }
        if (config.conf[name][selectedDemod->getName()].contains("noiseBlankerLevel")) {
            nbLevel = config.conf[name][selectedDemod->getName()]["noiseBlankerLevel"];
        }
        config.release();

        // Configure VFO
        if (vfo) {
            vfo->setBandwidthLimits(minBandwidth, maxBandwidth, selectedDemod->getBandwidthLocked());
            vfo->setReference(selectedDemod->getVFOReference());
            // VFOManager::VFO::setReference() only updates wtfVFO (the waterfall's own
            // bookkeeping -- lowerOffset/centerOffset/upperOffset all get correctly
            // recomputed for the new reference type internally) and stops there; unlike
            // setOffset()/setCenterOffset(), it never calls dspVFO->setOffset(...), so the
            // *real* demodulation VFO's actual tuned offset is left exactly where the
            // *previous* mode's reference type last put it. Switching between a REF_CENTER
            // mode (AM, NFM, ...) and a REF_LOWER/REF_UPPER one (USB, LSB) changes what
            // centerOffset actually *means* relative to the tuned/carrier frequency -- by up
            // to half the bandwidth -- so without this, the real audio stays demodulated at
            // the stale pre-switch offset until something else happens to call setOffset()
            // again, not just the spectrum preview (this codebase's first code to actually
            // read and display centerOffset directly) showing a stale picture. Re-applying
            // centerOffset to itself is a safe, idempotent way to force that missing sync:
            // setCenterOffset() always treats its argument as the true center regardless of
            // reference type, so this doesn't change what wtfVFO already correctly computed --
            // it only adds the dspVFO->setOffset() call setReference() itself never makes.
            // Found live 2026-08-10, chasing a spectrum-preview-only symptom ("switching from
            // USB to AM triggers the balloon") that turned out to be this instead -- a
            // pre-existing gap in VFOManager, not anything introduced by the preview feature.
            vfo->setCenterOffset(vfo->wtfVFO->centerOffset);
            vfo->setSnapInterval(snapInterval);
            vfo->setSampleRate(ifSamplerate, bandwidth);
        }

        // Configure bandwidth
        setBandwidth(bandwidth);

        // Configure passband edges (see the "Load config" section above).
        applyPassbandEdges();
        if (vfo) {
            preview.setOffset(vfo->wtfVFO->centerOffset);
            // The only place previewWidthHz gets set -- computed once per mode selection and
            // then left alone for the rest of this mode's use, on Ralph's own direct
            // instruction after three earlier attempts at "recompute it live, just more
            // carefully" all still visibly changed the zoom while trimming ("it goes from a
            // representation of a section of the spectrum covering maybe three channels to
            // zoomed in on one ... it looks wrong, it feels wrong, it acts wrong"). Simplest
            // fix available and the one actually asked for: stop trying to keep it fresh at
            // all. Trade-off: RadioSpectrumPreview::setWidth()'s own re-snap-against-the-live-
            // source-rate protection (see its comment) now only runs at mode-select time
            // rather than every frame -- fine for a source whose rate settles once at startup
            // and stays put, which is what's actually been observed; a source that changes
            // rate mid-session without a mode switch could in principle need this revisited.
            //
            // Based on max(getDefaultBandwidth(), passbandHi - passbandLo), not either alone:
            //
            // Not `bandwidth` -- found live testing this same day that `bandwidth` can be
            // stale independent of the passband actually in effect: every build prior to that
            // fix persisted `bandwidth` from the live-dragged passband (see
            // applyPassbandEdges()'s own history above), so a session that hit any of those
            // earlier builds can have a leftover `bandwidth` in config.json with no real
            // relationship to the (separately, correctly persisted) passbandLo/Hi -- observed
            // directly in testing: AM's `bandwidth` stuck at 15000, its IF-rate ceiling, while
            // passbandLo/Hi still correctly held a real ~8kHz trim.
            //
            // Not passbandHi - passbandLo alone either, despite that being the fix for the
            // above -- found live testing immediately after: trimming a mode's passband
            // narrow (exactly what this feature is *for* -- dodging an adjacent signal) and
            // then leaving and returning to that mode locked the preview into that same narrow
            // zoom on every subsequent visit, since the persisted passband itself is narrow by
            // then. The whole point of the preview is to show context around the passband,
            // which a deliberately-narrowed passband doesn't stop being true for -- so the
            // *mode's own typical/default width* is the right floor for how much context is
            // worth showing, independent of how tightly the passband has actually been trimmed
            // this session. Still grows past that default if the passband is ever wider than
            // it (an edge case, but a sane one to keep exact).
            // Parenthesised (std::max) -- windows.h's own max() macro turns an unparenthesised
            // std::max(...) into MSVC error C2589. Same trap as bare M_PI; see the fix for
            // iq_frontend.h's lockDecimation() for the full explanation.
            previewWidthHz = previewWidthFor((std::max)(selectedDemod->getDefaultBandwidth(), passbandHi - passbandLo));
            preview.setWidth(previewWidthHz);
        }
        // Forces the preview's dB auto-scale to re-range from scratch rather than smoothing in
        // from wherever the *previous* mode left it -- specWidget is one persistent widget
        // instance, not recreated per mode, so without this a mode switch to a signal with a
        // different level than the last mode's carried that old range into the new mode's
        // first several frames. Found live: numerically nothing about the frequency span had
        // changed (confirmed directly -- width computation logged identical before/after a
        // USB -> LSB -> USB round trip) but the *picture* still looked like the "balloon"
        // regression this whole investigation has been chasing, because this, not the
        // frequency axis, was what had actually changed. See MiniSpectrum::resetRange()'s own
        // comment.
        specWidget.resetRange();

        // Configure noise blanker
        nb.setRate(500.0 / ifSamplerate);
        setNBLevel(nbLevel);
        setNBEnabled(nbAllowed && nbEnabled);

        // Configure FM IF Noise Reduction
        setIFNRPreset((selectedDemodID == RADIO_DEMOD_NFM) ? ifnrPresets[fmIFPresetId] : IFNR_PRESET_BROADCAST);
        setFMIFNREnabled(FMIFNRAllowed ? FMIFNREnabled : false);

        // Configure squelch
        setSquelchMode(squelchAllowed ? squelchModes[squelchModeId] : SQUELCH_MODE_OFF);
        setSquelchLevel(squelchLevel);
        setCTCSSTone(ctcssTones[ctcssToneId]);

        // Configure AF chain
        if (postProcEnabled) {
            // Configure resampler
            afChain.stop();
            double afsr = selectedDemod->getAFSampleRate();
            ctcss.setSamplerate(afsr);
            resamp.setInSamplerate(afsr);
            afChain.enableBlock(&resamp, [=](dsp::stream<dsp::stereo_t>* out){ stream.setInput(out); });
            setAudioSampleRate(audioSampleRate);

            // Configure the HPF
            setHighPass(highPass && highPassAllowed);

            // Configure deemphasis
            setDeemphasisMode(deempModes[deempId]);

            // Configure Tube Warmth (see setTubeWarmth()'s own comment on reusing
            // highPassAllowed rather than a dedicated flag)
            tubeWarmth.setWarmth(tubeWarmthAmount);
            tubeWarmth.setNoise(tubeWarmthNoise);
            setTubeWarmth(tubeWarmthEnabled && highPassAllowed);
        }
        else {
            // Disable everything if post processing is disabled
            afChain.disableAllBlocks([=](dsp::stream<dsp::stereo_t>* out){ stream.setInput(out); });
        }

        // Start new demodulator
        selectedDemod->start();
    }


    // Sets the overall bandwidth without touching the passband trim -- RxVFO::setBandwidth()
    // deliberately doesn't re-clamp it either (see that function's comment). Used by
    // selectDemod(), which manages passbandLo/Hi explicitly itself right after calling this
    // (loading a persisted trim per mode, not just defaulting to symmetric); everywhere else
    // that lets the user set a bandwidth as a single scalar should go through
    // setBandwidthSymmetric() instead, below.
    void setBandwidth(double bw) {
        bw = std::clamp<double>(bw, minBandwidth, maxBandwidth);
        bandwidth = bw;
        if (!selectedDemod) { return; }
        vfo->setBandwidth(bandwidth);
        selectedDemod->setBandwidth(bandwidth);
        // Deliberately does NOT touch previewWidthHz/preview.setWidth() -- that's set once at
        // mode selection only now and left alone for the rest of the mode's use, including
        // across bandwidth edits made through here. See previewWidthHz's own comment.

        config.acquire();
        config.conf[name][selectedDemod->getName()]["bandwidth"] = bandwidth;
        config.release(true);
    }

    // The numeric Bandwidth field and the main waterfall's own bandwidth-drag handles both
    // give a single scalar width, which can't represent an existing asymmetric trim -- treat
    // that as the user intentionally asking for a fresh symmetric window at that width, same
    // as it's always behaved, rather than re-clamping whatever trim was already there into the
    // new width (which is what happened before RxVFO::clampPassband() stopped treating
    // "bandwidth" as a second ceiling on the passband -- see its comment for the full story of
    // why that felt like the filter randomly collapsing whenever this field was touched).
    void setBandwidthSymmetric(double bw) {
        setBandwidth(bw);
        passbandLo = -bandwidth / 2.0;
        passbandHi = bandwidth / 2.0;
        applyPassbandEdges();
    }

    // Pushes passbandLo/passbandHi (already updated in place by MiniSpectrum::draw(), or
    // freshly loaded/defaulted elsewhere) down to the real VFO and persists them.
    //
    // Also grows `bandwidth` -- and only grows it, never shrinks -- when the passband has been
    // dragged wider than it. Narrowing the passband already changes the audible filter entirely
    // on its own (RxVFO's own bandpass trim, independent of `bandwidth`), but *widening* past
    // `bandwidth` used to have no audible effect at all: the demodulator itself keeps its own
    // separate internal filter/AGC sized from `bandwidth` (see demod::AM::setBandwidth() and
    // its siblings), completely independent of RxVFO's passband trim, and that second, narrower
    // filter was the one actually limiting the audio -- dragging the preview's edges out past it
    // widened the *display* and the VFO-level filter, but the demod's own filter downstream
    // never got told to widen too, so nothing audible changed. Reported live 2026-08-11:
    // "Dragging the filters beyond the value in the Bandwidth field does not change the audio
    // response... Dragging beyond that value should increase that value."
    //
    // An earlier version of this file avoided calling setBandwidth() from here at all --
    // *every* live drag frame used to sync bandwidth this way, and for USB/LSB that reliably
    // detuned the actual audio, because VFOManager::VFO's WaterfallVFO recomputes centerOffset
    // from bandwidth for those modes (anchored to the fixed edge they tune from), and nothing
    // was propagating that new centerOffset down to the real demod VFO's tuned offset. That
    // propagation gap is now fixed at its actual source, VFOManager::VFO::setBandwidth() itself
    // (see its own comment -- the same missing-resync pattern round 10 already found and fixed
    // in setReference()), so calling it from here is safe again. Still only doing it for actual
    // growth, though, not on every frame regardless of direction: shrinking doesn't need it (the
    // passband trim alone already does the job), and there's no reason to touch the numeric
    // field/persisted config on a trim that's just making the filter narrower than its own
    // ceiling, not asking for a new ceiling.
    void applyPassbandEdges() {
        if (!vfo || !selectedDemod) { return; }
        double neededBandwidth = passbandHi - passbandLo;
        if (neededBandwidth > bandwidth) {
            // For USB/LSB, growing `bandwidth` here moves wtfVFO's centerOffset (see
            // VFOManager::VFO::setBandwidth()'s own comment -- it keeps the fixed tuning edge in
            // place while the *other* edge moves, which is exactly right for the real demod
            // audio). But passbandLo/passbandHi, both here and inside RxVFO, are expressed as Hz
            // offsets *from centerOffset* -- the same coordinate frame MiniSpectrum::draw() plots
            // in -- so when centerOffset itself shifts, both edges silently shift right along
            // with it in absolute-frequency terms, not just the one the user actually dragged.
            // Reported live 2026-08-11: dragging USB/LSB's outer edge out was also dragging the
            // *inner* edge -- the one sitting right next to the carrier -- away from it,
            // attenuating the low audio frequencies nearest the carrier that it's supposed to be
            // passing untouched. Re-basing both edges by exactly however far centerOffset moved,
            // right here before they're pushed down to the VFO, keeps every edge pinned to the
            // same absolute frequency it was at before this call -- including the one just
            // dragged, since MiniSpectrum::draw() computed its new value in the *old*
            // centerOffset's coordinate frame too. A no-op for REF_CENTER modes (AM, SAM, ...),
            // where setBandwidth() never moves centerOffset in the first place.
            double oldCenterOffset = vfo->wtfVFO->centerOffset;
            setBandwidth(neededBandwidth);
            double centerOffsetShift = vfo->wtfVFO->centerOffset - oldCenterOffset;
            passbandLo -= centerOffsetShift;
            passbandHi -= centerOffsetShift;
        }
        vfo->setPassband(passbandLo, passbandHi);
        passbandLo = vfo->getPassbandLo();
        passbandHi = vfo->getPassbandHi();

        config.acquire();
        config.conf[name][selectedDemod->getName()]["passbandLo"] = passbandLo;
        config.conf[name][selectedDemod->getName()]["passbandHi"] = passbandHi;
        config.release(true);
    }

    // How wide a slice around the tuned frequency the spectrum preview shows -- deliberately
    // wider than the current demod bandwidth (the whole point is to see the adjacent channel
    // the asymmetric filter exists to dodge), clamped so a bandwidth-locked wideband mode like
    // WFM doesn't ask for an unreasonable amount of spectrum. See
    // RADIO_SPECTRUM_FILTER_PLAN.md.
    static double previewWidthFor(double bandwidth) {
        return std::clamp<double>(bandwidth * 4.0, 3000.0, 500000.0);
    }

    void setAudioSampleRate(double sr) {
        audioSampleRate = sr;
        if (!selectedDemod) { return; }
        selectedDemod->AFSampRateChanged(audioSampleRate);
        if (!postProcEnabled && vfo) {
            // If postproc is disabled, IF SR = AF SR
            minBandwidth = selectedDemod->getMinBandwidth();
            maxBandwidth = selectedDemod->getMaxBandwidth();
            bandwidth = selectedDemod->getIFSampleRate();
            vfo->setBandwidthLimits(minBandwidth, maxBandwidth, selectedDemod->getBandwidthLocked());
            vfo->setSampleRate(selectedDemod->getIFSampleRate(), bandwidth);
            return;
        }

        afChain.stop();

        // Configure resampler
        resamp.setOutSamplerate(audioSampleRate);

        // Configure the HPF sample rate
        hpTaps = dsp::taps::highPass(300.0, 100.0, audioSampleRate);
        hpf.setTaps(hpTaps);

        // Configure deemphasis sample rate
        deemp.setSamplerate(audioSampleRate);

        // Configure Tube Warmth's own sample-rate-dependent filters
        tubeWarmth.setSampleRate(audioSampleRate);

        afChain.start();
    }

    void setHighPass(bool enabled) {
        // Update the state
        highPass = enabled;

        // Check if post-processing is enabled and that a demodulator is selected
        if (!postProcEnabled || !selectedDemod) { return; }

        // Set the state of the HPF in the AF chain
        afChain.setBlockEnabled(&hpf, enabled, [=](dsp::stream<dsp::stereo_t>* out){ stream.setInput(out); });

        // Save config
        config.acquire();
        config.conf[name][selectedDemod->getName()]["highPass"] = enabled;
        config.release(true);
    }

    void setTubeWarmth(bool enabled) {
        tubeWarmthEnabled = enabled;
        if (!postProcEnabled || !selectedDemod) { return; }
        afChain.setBlockEnabled(&tubeWarmth, enabled, [=](dsp::stream<dsp::stereo_t>* out){ stream.setInput(out); });
        config.acquire();
        config.conf[name][selectedDemod->getName()]["tubeWarmthEnabled"] = enabled;
        config.release(true);
    }

    void setTubeWarmthAmount(float amount) {
        tubeWarmthAmount = amount;
        tubeWarmth.setWarmth(amount);
        if (!selectedDemod) { return; }
        config.acquire();
        config.conf[name][selectedDemod->getName()]["tubeWarmthAmount"] = amount;
        config.release(true);
    }

    void setTubeWarmthNoise(float amount) {
        tubeWarmthNoise = amount;
        tubeWarmth.setNoise(amount);
        if (!selectedDemod) { return; }
        config.acquire();
        config.conf[name][selectedDemod->getName()]["tubeWarmthNoise"] = amount;
        config.release(true);
    }

    void setDeemphasisMode(DeemphasisMode mode) {
        deempId = deempModes.valueId(mode);
        if (!postProcEnabled || !selectedDemod) { return; }
        bool deempEnabled = (mode != DEEMP_MODE_NONE);
        if (deempEnabled) { deemp.setTau(deempTaus[mode]); }
        afChain.setBlockEnabled(&deemp, deempEnabled, [=](dsp::stream<dsp::stereo_t>* out){ stream.setInput(out); });

        // Save config
        config.acquire();
        config.conf[name][selectedDemod->getName()]["deempMode"] = deempModes.key(deempId);
        config.release(true);
    }

    void setNBEnabled(bool enable) {
        nbEnabled = enable;
        if (!selectedDemod) { return; }
        ifChain.setBlockEnabled(&nb, nbEnabled, [=](dsp::stream<dsp::complex_t>* out){ selectedDemod->setInput(out); });

        // Save config
        config.acquire();
        config.conf[name][selectedDemod->getName()]["noiseBlankerEnabled"] = nbEnabled;
        config.release(true);
    }

    void setNBLevel(float level) {
        nbLevel = std::clamp<float>(level, MIN_NB, MAX_NB);
        nb.setLevel(nbLevel);
        if (!selectedDemod) { return; }

        // Save config
        config.acquire();
        config.conf[name][selectedDemod->getName()]["noiseBlankerLevel"] = nbLevel;
        config.release(true);
    }

    void setSquelchMode(SquelchMode mode) {
        squelchModeId = squelchModes.valueId(mode);
        if (!selectedDemod) { return; }

        // Disable all squelch blocks
        ifChain.disableBlock(&powerSquelch, [=](dsp::stream<dsp::complex_t>* out){ selectedDemod->setInput(out); });
        afChain.disableBlock(&ctcss, [=](dsp::stream<dsp::stereo_t>* out){ stream.setInput(out); });

        // Enable the block depending on the mode
        switch (mode) {
        case SQUELCH_MODE_OFF:
            break;

        case SQUELCH_MODE_POWER:
            // Enable the power squelch block
            ifChain.enableBlock(&powerSquelch, [=](dsp::stream<dsp::complex_t>* out){ selectedDemod->setInput(out); });
            break;

        case SQUELCH_MODE_SNR:
            // TODO
            break;

        case SQUELCH_MODE_CTCSS_MUTE:
            // Set the required tone and enable the CTCSS squelch block
            ctcss.setRequiredTone(ctcssTones[ctcssToneId]);
            afChain.enableBlock(&ctcss, [=](dsp::stream<dsp::stereo_t>* out){ stream.setInput(out); });
            break;

        case SQUELCH_MODE_CTCSS_DECODE:
            // Set the required tone to none and enable the CTCSS squelch block
            ctcss.setRequiredTone(dsp::noise_reduction::CTCSS_TONE_NONE);
            afChain.enableBlock(&ctcss, [=](dsp::stream<dsp::stereo_t>* out){ stream.setInput(out); });
            break;

        case SQUELCH_MODE_DCS_MUTE:
            // TODO
            break;

        case SQUELCH_MODE_DCS_DECODE:
            // TODO
            break;
        }

        // Save config
        config.acquire();
        config.conf[name][selectedDemod->getName()]["squelchMode"] = squelchModes.key(squelchModeId);
        config.release(true);
    }

    void setSquelchLevel(float level) {
        squelchLevel = std::clamp<float>(level, MIN_SQUELCH, MAX_SQUELCH);
        powerSquelch.setLevel(squelchLevel);
        if (!selectedDemod) { return; }

        // Save config
        config.acquire();
        config.conf[name][selectedDemod->getName()]["squelchLevel"] = squelchLevel;
        config.release(true);
    }

    void setCTCSSTone(dsp::noise_reduction::CTCSSTone tone) {
        // Check for an invalid value
        if (tone == dsp::noise_reduction::CTCSS_TONE_NONE) { return; }

        // If not in CTCSS mute mode, do nothing
        if (squelchModes[squelchModeId] != SQUELCH_MODE_CTCSS_MUTE) { return; }

        // Set the tone
        ctcssToneId = ctcssTones.valueId(tone);
        ctcss.setRequiredTone(tone);
        if (!selectedDemod) { return; }

        // Save config
        config.acquire();
        config.conf[name][selectedDemod->getName()]["ctcssTone"] = ctcssTones.key(ctcssToneId);
        config.release(true);
    }

    void setFMIFNREnabled(bool enabled) {
        FMIFNREnabled = enabled;
        if (!selectedDemod) { return; }
        ifChain.setBlockEnabled(&fmnr, FMIFNREnabled, [=](dsp::stream<dsp::complex_t>* out){ selectedDemod->setInput(out); });

        // Save config
        config.acquire();
        config.conf[name][selectedDemod->getName()]["FMIFNREnabled"] = FMIFNREnabled;
        config.release(true);
    }

    void setIFNRPreset(IFNRPreset preset) {
        // Don't save if in broadcast mode
        if (preset == IFNR_PRESET_BROADCAST) {
            if (!selectedDemod) { return; }
            fmnr.setBins(ifnrTaps[preset]);
            return;
        }

        fmIFPresetId = ifnrPresets.valueId(preset);
        if (!selectedDemod) { return; }
        fmnr.setBins(ifnrTaps[preset]);

        // Save config
        config.acquire();
        config.conf[name][selectedDemod->getName()]["fmifnrPreset"] = ifnrPresets.key(fmIFPresetId);
        config.release(true);
    }

    static void vfoUserChangedBandwidthHandler(double newBw, void* ctx) {
        RadioModule* _this = (RadioModule*)ctx;
        _this->setBandwidthSymmetric(newBw);
    }

    static void sampleRateChangeHandler(float sampleRate, void* ctx) {
        RadioModule* _this = (RadioModule*)ctx;
        _this->setAudioSampleRate(sampleRate);
    }

    static void ifChainOutputChangeHandler(dsp::stream<dsp::complex_t>* output, void* ctx) {
        RadioModule* _this = (RadioModule*)ctx;
        if (!_this->selectedDemod) { return; }
        _this->selectedDemod->setInput(output);
    }

    static void moduleInterfaceHandler(int code, void* in, void* out, void* ctx) {
        RadioModule* _this = (RadioModule*)ctx;

        // If no demod is selected, reject the command
        if (!_this->selectedDemod) { return; }

        // Execute commands
        if (code == RADIO_IFACE_CMD_GET_MODE && out) {
            int* _out = (int*)out;
            *_out = _this->selectedDemodID;
        }
        else if (code == RADIO_IFACE_CMD_SET_MODE && in && _this->enabled) {
            int* _in = (int*)in;
            _this->selectDemodByID((DemodID)*_in);
        }
        else if (code == RADIO_IFACE_CMD_GET_BANDWIDTH && out) {
            float* _out = (float*)out;
            *_out = _this->bandwidth;
        }
        else if (code == RADIO_IFACE_CMD_SET_BANDWIDTH && in && _this->enabled) {
            float* _in = (float*)in;
            if (_this->bandwidthLocked) { return; }
            _this->setBandwidth(*_in);
        }
        else if (code == RADIO_IFACE_CMD_GET_SQUELCH_MODE && out) {
            SquelchMode* _out = (SquelchMode*)out;
            *_out = _this->squelchModes[_this->squelchModeId];
        }
        else if (code == RADIO_IFACE_CMD_SET_SQUELCH_MODE && in && _this->enabled) {
            SquelchMode* _in = (SquelchMode*)in;
            _this->setSquelchMode(*_in);
        }
        else if (code == RADIO_IFACE_CMD_GET_SQUELCH_LEVEL && out) {
            float* _out = (float*)out;
            *_out = _this->squelchLevel;
        }
        else if (code == RADIO_IFACE_CMD_SET_SQUELCH_LEVEL && in && _this->enabled) {
            float* _in = (float*)in;
            _this->setSquelchLevel(*_in);
        }
        else if (code == RADIO_IFACE_CMD_GET_CTCSS_TONE && out) {
            dsp::noise_reduction::CTCSSTone* _out = (dsp::noise_reduction::CTCSSTone*)out;
            *_out = _this->ctcssTones[_this->ctcssToneId];
        }
        else if (code == RADIO_IFACE_CMD_SET_CTCSS_TONE && in && _this->enabled) {
            dsp::noise_reduction::CTCSSTone* _in = (dsp::noise_reduction::CTCSSTone*)in;
            _this->setCTCSSTone(*_in);
        }
        else if (code == RADIO_IFACE_CMD_GET_HIGHPASS && out) {
            bool* _out = (bool*)out;
            *_out = _this->highPass;
        }
        else if (code == RADIO_IFACE_CMD_SET_HIGHPASS && in && _this->enabled) {
            bool* _in = (bool*)in;
            _this->setHighPass(*_in);
        }
        else {
            return;
        }

        // Success
        return;
    }

    // Handlers
    EventHandler<double> onUserChangedBandwidthHandler;
    EventHandler<float> srChangeHandler;
    EventHandler<dsp::stream<dsp::complex_t>*> ifChainOutputChanged;
    EventHandler<dsp::stream<dsp::stereo_t>*> afChainOutputChanged;

    VFOManager::VFO* vfo = NULL;

    // Filter preview: a small always-visible spectrum around the carrier, with a shaded,
    // draggable, independently-asymmetric passband -- see RADIO_SPECTRUM_FILTER_PLAN.md.
    // passbandLo/passbandHi are Hz offsets from center (lo <= 0 <= hi), the local copies
    // MiniSpectrum::draw() reads/writes each frame; applyPassbandEdges() pushes whatever it
    // left them at down to the real VFO and persists them.
    RadioSpectrumPreview preview;
    ImGui::MiniSpectrum specWidget;
    double passbandLo = 0.0;
    double passbandHi = 0.0;
    // The preview's zoom width, in Hz. Computed exactly once, in selectDemod() at mode
    // selection, and then left alone for the rest of that mode's use -- not on bandwidth
    // edits, not on passband drags, not on a per-frame re-derive. Two earlier, progressively
    // more careful attempts at keeping this "live" (re-deriving from `bandwidth` every frame;
    // then only re-deriving on explicit bandwidth changes) both still visibly changed the
    // preview's zoom while trimming the passband, which is what actually mattered: a section
    // of spectrum covering several channels turning into a single already-filtered signal
    // zoomed in close, looking artificial ("a balloon sitting on a flat surface") and losing
    // the adjacent-channel context the whole feature exists to show. Frozen entirely on
    // Ralph's own direct instruction after live-testing all three attempts, 2026-08-10.
    // Trade-off: RadioSpectrumPreview::setWidth()'s own protection against a bad
    // source-sample-rate ratio (see its comment) now only gets (re-)applied at mode-select
    // time rather than continuously, so a source that changes rate mid-session without a mode
    // switch is unprotected until the next one -- not something observed in practice, since
    // the one source tested against settles its rate once at startup and holds it.
    double previewWidthHz = 40000.0;

    // IF chain
    dsp::chain<dsp::complex_t> ifChain;
    dsp::noise_reduction::NoiseBlanker nb;
    dsp::noise_reduction::FMIF fmnr;
    dsp::noise_reduction::PowerSquelch powerSquelch;

    // Audio chain
    dsp::stream<dsp::stereo_t> dummyAudioStream;
    dsp::chain<dsp::stereo_t> afChain;
    dsp::noise_reduction::CTCSSSquelch ctcss;
    dsp::multirate::RationalResampler<dsp::stereo_t> resamp;
    dsp::tap<float> hpTaps;
    dsp::filter::FIR<dsp::stereo_t, float> hpf;
    dsp::filter::Deemphasis<dsp::stereo_t> deemp;
    dsp::filter::TubeWarmth<dsp::stereo_t> tubeWarmth;

    SinkManager::Stream stream;

    demod::Demodulator* selectedDemod = NULL;

    OptionList<std::string, DeemphasisMode> deempModes;
    OptionList<std::string, IFNRPreset> ifnrPresets;
    OptionList<std::string, SquelchMode> squelchModes;
    OptionList<int, dsp::noise_reduction::CTCSSTone> ctcssTones;

    double audioSampleRate = 48000.0;
    float minBandwidth;
    float maxBandwidth;
    float bandwidth;
    bool bandwidthLocked;
    int snapInterval;
    int selectedDemodID = 1;
    bool postProcEnabled;

    int squelchModeId = 0;
    float squelchLevel;
    int ctcssToneId = 0;
    bool squelchAllowed = false;

    bool highPass = false;
    bool highPassAllowed = false;

    // Tube Warmth: simulates the sound of an old tube radio (core/src/dsp/filter/tube_warmth.h
    // has the actual DSP and the design reasoning). Deliberately reuses highPassAllowed rather
    // than adding a near-identical getTubeWarmthAllowed() virtual to every demodulator file --
    // the two features share the same real boundary (CW/RAW want clean, undistorted audio;
    // everything else can take coloration), so a second, always-in-lockstep flag would just be
    // one more thing to keep synchronised for no behavioral difference. tubeWarmthAmount/Noise
    // default to modest-but-audible values (not 0) so turning the feature on actually does
    // something immediately, rather than requiring the two sliders to also be raised by hand.
    bool tubeWarmthEnabled = false;
    float tubeWarmthAmount = 0.5f;
    float tubeWarmthNoise = 0.15f;

    int deempId = 0;
    bool deempAllowed;

    bool FMIFNRAllowed;
    bool FMIFNREnabled = false;
    int fmIFPresetId;

    bool notchEnabled = false;
    float notchPos = 0;
    float notchWidth = 500;

    bool nbAllowed;
    bool nbEnabled = false;
    float nbLevel = 10.0f;

    const double MIN_NB = 1.0;
    const double MAX_NB = 10.0;
    const double MIN_SQUELCH = -100.0;
    const double MAX_SQUELCH = 0.0;

    bool enabled = true;
};
