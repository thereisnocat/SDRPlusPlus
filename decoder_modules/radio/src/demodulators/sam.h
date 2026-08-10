#pragma once
#include "../demod.h"
#include <dsp/demod/sam.h>

namespace demod {
    class SAM : public Demodulator {
    public:
        SAM() {}

        SAM(std::string name, ConfigManager* config, dsp::stream<dsp::complex_t>* input, double bandwidth, double audioSR) {
            init(name, config, input, bandwidth, audioSR);
        }

        ~SAM() { stop(); }

        void init(std::string name, ConfigManager* config, dsp::stream<dsp::complex_t>* input, double bandwidth, double audioSR) {
            this->name = name;
            _config = config;

            // Load config
            config->acquire();
            if (config->conf[name][getName()].contains("agcAttack")) {
                agcAttack = config->conf[name][getName()]["agcAttack"];
            }
            if (config->conf[name][getName()].contains("agcDecay")) {
                agcDecay = config->conf[name][getName()]["agcDecay"];
            }
            if (config->conf[name][getName()].contains("carrierAgc")) {
                carrierAgc = config->conf[name][getName()]["carrierAgc"];
            }
            if (config->conf[name][getName()].contains("pllBandwidth")) {
                pllBandwidth = config->conf[name][getName()]["pllBandwidth"];
            }
            config->release();

            // Define structure
            demod.init(input, carrierAgc ? dsp::demod::SAM<dsp::stereo_t>::AGCMode::CARRIER : dsp::demod::SAM<dsp::stereo_t>::AGCMode::AUDIO,
                       bandwidth, agcAttack / getIFSampleRate(), agcDecay / getIFSampleRate(), 100.0 / getIFSampleRate(),
                       hzToRadPerSample(pllBandwidth), hzToRadPerSample(PLL_CAPTURE_RANGE_HZ), getIFSampleRate());
        }

        void start() { demod.start(); }

        void stop() { demod.stop(); }

        void showMenu() {
            float menuWidth = ImGui::GetContentRegionAvail().x;
            if (ImGui::Checkbox(("Carrier AGC##_radio_sam_carrier_agc_" + name).c_str(), &carrierAgc)) {
                demod.setAGCMode(carrierAgc ? dsp::demod::SAM<dsp::stereo_t>::AGCMode::CARRIER : dsp::demod::SAM<dsp::stereo_t>::AGCMode::AUDIO);
                _config->acquire();
                _config->conf[name][getName()]["carrierAgc"] = carrierAgc;
                _config->release(true);
            }
            ImGui::LeftLabel("AGC Attack");
            ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
            if (ImGui::SliderFloat(("##_radio_sam_agc_attack_" + name).c_str(), &agcAttack, 1.0f, 200.0f)) {
                demod.setAGCAttack(agcAttack / getIFSampleRate());
                _config->acquire();
                _config->conf[name][getName()]["agcAttack"] = agcAttack;
                _config->release(true);
            }
            ImGui::LeftLabel("AGC Decay");
            ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
            if (ImGui::SliderFloat(("##_radio_sam_agc_decay_" + name).c_str(), &agcDecay, 1.0f, 20.0f)) {
                demod.setAGCDecay(agcDecay / getIFSampleRate());
                _config->acquire();
                _config->conf[name][getName()]["agcDecay"] = agcDecay;
                _config->release(true);
            }
            ImGui::LeftLabel("PLL Bandwidth");
            ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
            if (ImGui::SliderFloat(("##_radio_sam_pll_bw_" + name).c_str(), &pllBandwidth, 2.0f, 100.0f, "%.0fHz")) {
                demod.setPLLBandwidth(hzToRadPerSample(pllBandwidth));
                _config->acquire();
                _config->conf[name][getName()]["pllBandwidth"] = pllBandwidth;
                _config->release(true);
            }

            // Carrier lock indicator. A single sample's phase error is noisy on its own (the
            // audio riding on the carrier constantly perturbs it a little even when well
            // locked), so it's smoothed here across menu draws rather than read raw.
            lockSmoothing = (lockSmoothing * 0.9f) + (fabsf(demod.getLockError()) * 0.1f);
            ImGui::TextUnformatted("Carrier:");
            ImGui::SameLine();
            if (lockSmoothing < LOCK_THRESHOLD_RAD) {
                ImGui::TextColored(ImVec4(0, 1, 0, 1), "Locked");
            }
            else {
                ImGui::TextColored(ImVec4(1, 0, 0, 1), "Searching");
            }
        }

        void setBandwidth(double bandwidth) { demod.setBandwidth(bandwidth); }

        void setInput(dsp::stream<dsp::complex_t>* input) { demod.setInput(input); }

        void AFSampRateChanged(double newSR) {}

        // ============= INFO =============

        const char* getName() { return "SAM"; }
        double getIFSampleRate() { return 15000.0; }
        double getAFSampleRate() { return getIFSampleRate(); }
        double getDefaultBandwidth() { return 10000.0; }
        double getMinBandwidth() { return 1000.0; }
        double getMaxBandwidth() { return getIFSampleRate(); }
        bool getBandwidthLocked() { return false; }
        double getDefaultSnapInterval() { return 1000.0; }
        int getVFOReference() { return ImGui::WaterfallVFO::REF_CENTER; }
        bool getDeempAllowed() { return false; }
        bool getPostProcEnabled() { return true; }
        int getDefaultDeemphasisMode() { return DEEMP_MODE_NONE; }
        bool getFMIFNRAllowed() { return false; }
        bool getNBAllowed() { return false; }
        bool getHighPassAllowed() { return true; }
        bool getSquelchAllowed() { return true; }
        dsp::stream<dsp::stereo_t>* getOutput() { return &demod.out; }

    private:
        // Converts a loop bandwidth (or, for PLL_CAPTURE_RANGE_HZ, a frequency pull-in limit)
        // given in Hz into the normalized radians/sample units dsp::loop::PLL expects, at SAM's
        // own fixed IF sample rate.
        double hzToRadPerSample(double hz) { return 2.0 * FL_M_PI * hz / getIFSampleRate(); }

        // How far off-frequency (in Hz) the loop is allowed to pull the carrier in from. Wide
        // enough to absorb realistic tuning error, narrow enough that it won't wander off and
        // lock onto an adjacent broadcast channel's carrier instead. Not user-facing -- unlike
        // PLL bandwidth (tracking speed once locked), this is a one-time capture limit that
        // isn't worth a knob of its own.
        static constexpr double PLL_CAPTURE_RANGE_HZ = 250.0;

        // Smoothed |phase error| threshold, in radians, below which the carrier is considered
        // locked for the UI indicator. Purely cosmetic -- has no effect on demodulation.
        static constexpr float LOCK_THRESHOLD_RAD = 0.35f;

        dsp::demod::SAM<dsp::stereo_t> demod;

        ConfigManager* _config = NULL;

        float agcAttack = 50.0f;
        float agcDecay = 5.0f;
        bool carrierAgc = false;
        float pllBandwidth = 20.0f;
        float lockSmoothing = FL_M_PI;

        std::string name;
    };
}
