#pragma once
#include "../demod.h"
#include <dsp/demod/broadcast_fm.h>
#include "../rds_demod.h"
#include <gui/widgets/symbol_diagram.h>
#include <fstream>
#include <rds.h>

namespace demod {
    enum RDSRegion {
        RDS_REGION_EUROPE,
        RDS_REGION_NORTH_AMERICA
    };

    class WFM : public Demodulator {
    public:
        WFM() : diag(0.5, 4096)  {}

        WFM(std::string name, ConfigManager* config, dsp::stream<dsp::complex_t>* input, double bandwidth, double audioSR) : diag(0.5, 4096) {
            init(name, config, input, bandwidth, audioSR);
        }

        ~WFM() {
            stop();
            gui::waterfall.onFFTRedraw.unbindHandler(&fftRedrawHandler);
        }

        void init(std::string name, ConfigManager* config, dsp::stream<dsp::complex_t>* input, double bandwidth, double audioSR) {
            this->name = name;
            _config = config;

            // Define RDS regions
            rdsRegions.define("eu", "Europe", RDS_REGION_EUROPE);
            rdsRegions.define("na", "North America", RDS_REGION_NORTH_AMERICA);

            // Register FFT draw handler
            fftRedrawHandler.handler = fftRedraw;
            fftRedrawHandler.ctx = this;
            gui::waterfall.onFFTRedraw.bindHandler(&fftRedrawHandler);

            // Default
            std::string rdsRegionStr = "eu";

            // Load config
            _config->acquire();
            bool modified = false;
            if (config->conf[name][getName()].contains("stereo")) {
                _stereo = config->conf[name][getName()]["stereo"];
            }
            if (config->conf[name][getName()].contains("lowPass")) {
                _lowPass = config->conf[name][getName()]["lowPass"];
            }
            if (config->conf[name][getName()].contains("rds")) {
                _rds = config->conf[name][getName()]["rds"];
            }
            if (config->conf[name][getName()].contains("rdsIncremental")) {
                _rdsIncremental = config->conf[name][getName()]["rdsIncremental"];
            }
            if (config->conf[name][getName()].contains("rdsInfo")) {
                _rdsInfo = config->conf[name][getName()]["rdsInfo"];
            }
            if (config->conf[name][getName()].contains("sca")) {
                _sca = config->conf[name][getName()]["sca"];
            }
            if (config->conf[name][getName()].contains("scaKHz")) {
                _scaKHz = config->conf[name][getName()]["scaKHz"];
            }
            if (config->conf[name][getName()].contains("scaGainDb")) {
                _scaGainDb = config->conf[name][getName()]["scaGainDb"];
            }
            if (config->conf[name][getName()].contains("showMultiplex")) {
                _showMux = config->conf[name][getName()]["showMultiplex"];
            }
            if (config->conf[name][getName()].contains("rdsRegion")) {
                rdsRegionStr = config->conf[name][getName()]["rdsRegion"];
            }
            _config->release(modified);

            // Load RDS region
            if (rdsRegions.keyExists(rdsRegionStr)) {
                rdsRegionId = rdsRegions.keyId(rdsRegionStr);
                rdsRegion = rdsRegions.value(rdsRegionId);
            }
            else {
                rdsRegion = RDS_REGION_EUROPE;
                rdsRegionId = rdsRegions.valueId(rdsRegion);
            }

            // Init DSP
            demod.init(input, bandwidth / 2.0f, getIFSampleRate(), _stereo, _lowPass, _rds);
            _bandwidth = bandwidth;
            applySca();
            demod.setMultiplexEnabled(_showMux);
            rdsDemod.init(&demod.rdsOut, _rdsInfo);
            hs.init(&rdsDemod.out, rdsHandler, this);
            reshape.init(&rdsDemod.soft, 4096, (1187 / 30) - 4096);
            diagHandler.init(&reshape.out, _diagHandler, this);

            // Init RDS display
            diag.lines.push_back(-0.8);
            diag.lines.push_back(0.8);
        }

        void start() {
            demod.start();
            rdsDemod.start();
            hs.start();
            reshape.start();
            diagHandler.start();
        }

        void stop() {
            demod.stop();
            rdsDemod.stop();
            hs.stop();
            reshape.stop();
            diagHandler.stop();
        }

        void showMenu() {
            if (ImGui::Checkbox(("Low Pass##_radio_wfm_lowpass_" + name).c_str(), &_lowPass)) {
                demod.setLowPass(_lowPass);
                _config->acquire();
                _config->conf[name][getName()]["lowPass"] = _lowPass;
                _config->release(true);
            }
            if (ImGui::Checkbox(("Stereo##_radio_wfm_stereo_" + name).c_str(), &_stereo)) {
                setStereo(_stereo);
                _config->acquire();
                _config->conf[name][getName()]["stereo"] = _stereo;
                _config->release(true);
            }
            if (ImGui::Checkbox(("Decode RDS##_radio_wfm_rds_" + name).c_str(), &_rds)) {
                demod.setRDSOut(_rds);
                _config->acquire();
                _config->conf[name][getName()]["rds"] = _rds;
                _config->release(true);
            }

            // SCA: subsidiary audio services on subcarriers above the RDS (67 kHz and 92 kHz are the usual ones)
            if (ImGui::Checkbox(("SCA subcarrier##_radio_wfm_sca_" + name).c_str(), &_sca)) {
                applySca();
                _config->acquire();
                _config->conf[name][getName()]["sca"] = _sca;
                _config->release(true);
            }
            if (_sca) {
                bool changed = false;
                const float presets[] = { 67.0f, 92.0f, 41.0f, 76.0f };
                for (int i = 0; i < 4; i++) {
                    if (i) { ImGui::SameLine(); }
                    char label[32];
                    snprintf(label, sizeof(label), "%.0f##_radio_wfm_sca_p%d_%s", presets[i], i, name.c_str());
                    const bool on = fabsf(_scaKHz - presets[i]) < 0.05f;
                    if (on) { ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive)); }
                    if (ImGui::Button(label, ImVec2(ImGui::GetFrameHeight() * 1.6f, 0))) { _scaKHz = presets[i]; changed = true; }
                    if (on) { ImGui::PopStyleColor(); }
                }
                ImGui::SameLine();
                ImGui::TextUnformatted("kHz");
                ImGui::LeftLabel("Frequency");
                ImGui::FillWidth();
                if (ImGui::InputFloat(("##_radio_wfm_sca_khz_" + name).c_str(), &_scaKHz, 0.1f, 1.0f, "%.1f kHz")) {
                    _scaKHz = std::clamp(_scaKHz, 20.0f, 100.0f);
                    changed = true;
                }
                ImGui::LeftLabel("Gain");
                ImGui::FillWidth();
                if (ImGui::SliderFloat(("##_radio_wfm_sca_gain_" + name).c_str(), &_scaGainDb, -20.0f, 30.0f, "%.0f dB")) {
                    demod.setSCAGain(0.5f * powf(10.0f, _scaGainDb / 20.0f));
                    _config->acquire();
                    _config->conf[name][getName()]["scaGainDb"] = _scaGainDb;
                    _config->release(true);
                }
                // The subcarrier and its sidebands have to fit inside the channel (the bandwidth), or there is nothing to hear
                const double need = 2.0 * (_scaKHz * 1000.0 + 13000.0);
                if (_bandwidth < need) {
                    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "Widen the bandwidth to at least %.0f kHz to hear %.0f kHz.", need / 1000.0, _scaKHz);
                }
                if (changed) {
                    applySca();
                    _config->acquire();
                    _config->conf[name][getName()]["scaKHz"] = _scaKHz;
                    _config->release(true);
                }
            }
            if (ImGui::Checkbox(("Show multiplex##_radio_wfm_mux_" + name).c_str(), &_showMux)) {
                demod.setMultiplexEnabled(_showMux);
                _config->acquire();
                _config->conf[name][getName()]["showMultiplex"] = _showMux;
                _config->release(true);
            }
            if (_showMux) { drawMultiplex(); }

            // TODO: This might break when the entire radio module is disabled
            if (_rds) {
                if (ImGui::Checkbox(("RDS Incremental Update##_radio_wfm_rds_inc_" + name).c_str(), &_rdsIncremental)) {
                    _config->acquire();
                    _config->conf[name][getName()]["rdsIncremental"] = _rdsIncremental;
                    _config->release(true);
                }
                if (ImGui::Checkbox(("Advanced RDS Info##_radio_wfm_rds_info_" + name).c_str(), &_rdsInfo)) {
                    setAdvancedRds(_rdsInfo);
                    _config->acquire();
                    _config->conf[name][getName()]["rdsInfo"] = _rdsInfo;
                    _config->release(true);
                }
                ImGui::SameLine();
                ImGui::FillWidth();
                if (ImGui::Combo(("##_radio_wfm_rds_region_" + name).c_str(), &rdsRegionId, rdsRegions.txt)) {
                    rdsRegion = rdsRegions.value(rdsRegionId);
                    _config->acquire();
                    _config->conf[name][getName()]["rdsRegion"] = rdsRegions.key(rdsRegionId);
                    _config->release(true);
                }
            }

            float menuWidth = ImGui::GetContentRegionAvail().x;

            if (_rds && _rdsInfo) {
                ImGui::BeginTable(("##radio_wfm_rds_info_tbl_" + name).c_str(), 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders);
                if (rdsDecode.piCodeValid()) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted("PI Code");
                    ImGui::TableSetColumnIndex(1);
                    if (rdsRegion == RDS_REGION_NORTH_AMERICA) {
                        ImGui::Text("0x%04X (%s)", rdsDecode.getPICode(), rdsDecode.getCallsign().c_str());
                    }
                    else {
                        ImGui::Text("0x%04X", rdsDecode.getPICode());
                    }
                    
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted("Country Code");
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%d", rdsDecode.getCountryCode());

                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted("Program Coverage");
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%s (%d)", rds::AREA_COVERAGE_TO_STR[rdsDecode.getProgramCoverage()], rdsDecode.getProgramCoverage());

                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted("Reference Number");
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%d", rdsDecode.getProgramRefNumber());
                }
                else {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted("PI Code");
                    ImGui::TableSetColumnIndex(1);
                    if (rdsRegion == RDS_REGION_NORTH_AMERICA) {
                        ImGui::TextUnformatted("0x---- (----)");
                    }
                    else {
                        ImGui::TextUnformatted("0x----");
                    }
                    
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted("Country Code");
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted("--");  // TODO: String

                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted("Program Coverage");
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted("------- (--)");

                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted("Reference Number");
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted("--");
                }

                if (rdsDecode.programTypeValid()) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted("Program Type");
                    ImGui::TableSetColumnIndex(1);
                    if (rdsRegion == RDS_REGION_NORTH_AMERICA) {
                        ImGui::Text("%s (%d)", rds::PROGRAM_TYPE_US_TO_STR[rdsDecode.getProgramType()], rdsDecode.getProgramType());
                    }
                    else {
                        ImGui::Text("%s (%d)", rds::PROGRAM_TYPE_EU_TO_STR[rdsDecode.getProgramType()], rdsDecode.getProgramType());
                    }
                }
                else {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted("Program Type");
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted("------- (--)");  // TODO: String
                }

                if (rdsDecode.musicValid()) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted("Music");
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%s", rdsDecode.getMusic() ? "Yes":"No");
                }
                else {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted("Music");
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted("---");
                }

                ImGui::EndTable();

                ImGui::SetNextItemWidth(menuWidth);
                diag.draw();
            }
        }

        void setBandwidth(double bandwidth) {
            _bandwidth = bandwidth;
            demod.setDeviation(bandwidth / 2.0f);
        }

        void setInput(dsp::stream<dsp::complex_t>* input) {
            demod.setInput(input);
        }

        void AFSampRateChanged(double newSR) {}

        // ============= INFO =============

        const char* getName() { return "WFM"; }
        double getIFSampleRate() { return 250000.0; }
        double getAFSampleRate() { return getIFSampleRate(); }
        double getDefaultBandwidth() { return 150000.0; }
        double getMinBandwidth() { return 50000.0; }
        double getMaxBandwidth() { return getIFSampleRate(); }
        bool getBandwidthLocked() { return false; }
        double getDefaultSnapInterval() { return 100000.0; }
        int getVFOReference() { return ImGui::WaterfallVFO::REF_CENTER; }
        bool getDeempAllowed() { return true; }
        bool getPostProcEnabled() { return true; }
        int getDefaultDeemphasisMode() { return DEEMP_MODE_50US; }
        bool getFMIFNRAllowed() { return true; }
        bool getNBAllowed() { return false; }
        bool getHighPassAllowed() { return true; }
        bool getSquelchAllowed() { return true; }
        dsp::stream<dsp::stereo_t>* getOutput() { return &demod.out; }

        // ============= DEDICATED FUNCTIONS =============

        void setStereo(bool stereo) {
            _stereo = stereo;
            demod.setStereo(_stereo);
        }

        void setAdvancedRds(bool enabled) {
            rdsDemod.setSoftEnabled(enabled);
            _rdsInfo = enabled;
        }

    private:
        void applySca() {
            demod.setSCAGain(0.5f * powf(10.0f, _scaGainDb / 20.0f));
            demod.setSCA(_sca ? (double)_scaKHz * 1000.0 : 0.0);
        }

        // The composite from 0 to 100 kHz: the pilot, the stereo difference, RDS and any subcarriers show as humps. Click one to listen to it.
        void drawMultiplex() {
            std::vector<float> db;
            const float w = ImGui::GetContentRegionAvail().x;
            const float h = 90.0f * style::uiScale;
            ImVec2 p0 = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton(("##_radio_wfm_mux_plot_" + name).c_str(), ImVec2(w, h));
            const bool hovered = ImGui::IsItemHovered();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), IM_COL32(14, 16, 22, 255));
            if (!demod.getMultiplex(db) || db.size() < 8) {
                dl->AddText(ImVec2(p0.x + 6, p0.y + 6), IM_COL32(180, 180, 180, 255), "Waiting for a signal...");
                return;
            }
            std::vector<float> sorted(db.begin() + db.size() / 4, db.end());
            std::sort(sorted.begin(), sorted.end());
            const float lo = sorted[sorted.size() / 10] - 6.0f;
            const float hi = std::max(*std::max_element(db.begin() + 3, db.end()) + 2.0f, lo + 30.0f);
            struct Mark { float khz; const char* text; ImU32 col; };
            const Mark marks[] = { {19.0f, "19", IM_COL32(120, 200, 120, 255)}, {38.0f, "L-R", IM_COL32(120, 160, 220, 255)}, {57.0f, "RDS", IM_COL32(230, 180, 90, 255)},
                                   {67.0f, "67", IM_COL32(230, 110, 110, 255)}, {92.0f, "92", IM_COL32(230, 110, 110, 255)} };
            for (const Mark& m : marks) {
                const float x = p0.x + w * m.khz / 100.0f;
                dl->AddLine(ImVec2(x, p0.y + 12), ImVec2(x, p0.y + h), (m.col & 0x00FFFFFF) | 0x60000000);
                dl->AddText(ImVec2(x - 6, p0.y + 1), m.col, m.text);
            }
            if (_sca) {
                const float x = p0.x + w * _scaKHz / 100.0f;
                dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p0.y + h), IM_COL32(255, 255, 255, 255), 2.0f);
            }
            ImVec2 prev;
            for (size_t k = 0; k < db.size(); k++) {
                const float x = p0.x + w * (float)k / (float)(db.size() - 1);
                const float y = p0.y + h - std::clamp((db[k] - lo) / (hi - lo), 0.0f, 1.0f) * (h - 2.0f) - 1.0f;
                if (k) { dl->AddLine(prev, ImVec2(x, y), IM_COL32(110, 210, 255, 255)); }
                prev = ImVec2(x, y);
            }
            if (hovered) {
                const float khz = (ImGui::GetIO().MousePos.x - p0.x) / w * 100.0f;
                char t[32];
                snprintf(t, sizeof(t), "%.1f kHz", khz);
                dl->AddText(ImVec2(p0.x + w - 70.0f * style::uiScale, p0.y + 12), IM_COL32(220, 220, 220, 255), t);
                if (ImGui::IsItemClicked() && khz > 30.0f) {
                    for (float c : { 41.0f, 67.0f, 76.0f, 92.0f }) { if (fabsf(khz - c) < 3.5f) { _scaKHz = c; } }
                    if (fabsf(_scaKHz - khz) > 3.5f && !(fabsf(khz - 41.0f) < 3.5f || fabsf(khz - 67.0f) < 3.5f || fabsf(khz - 76.0f) < 3.5f || fabsf(khz - 92.0f) < 3.5f)) { _scaKHz = roundf(khz * 10.0f) / 10.0f; }
                    _sca = true;
                    applySca();
                    _config->acquire();
                    _config->conf[name][getName()]["sca"] = _sca;
                    _config->conf[name][getName()]["scaKHz"] = _scaKHz;
                    _config->release(true);
                }
            }
        }

        static void rdsHandler(uint8_t* data, int count, void* ctx) {
            WFM* _this = (WFM*)ctx;
            _this->rdsDecode.process(data, count);
        }

        static void _diagHandler(float* data, int count, void* ctx) {
            WFM* _this = (WFM*)ctx;
            float* buf = _this->diag.acquireBuffer();
            memcpy(buf, data, count * sizeof(float));
            _this->diag.releaseBuffer();
        }

        static void fftRedraw(ImGui::WaterFall::FFTRedrawArgs args, void* ctx) {
            WFM* _this = (WFM*)ctx;
            if (!_this->_rds) { return; }

            // Generate string depending on RDS mode
            char buf[256];
            if (_this->rdsDecode.PSNameValid() && _this->rdsDecode.radioTextValid()) {
                sprintf(buf, "RDS: %s - %s", _this->rdsDecode.getPSName(_this->_rdsIncremental).c_str(), _this->rdsDecode.getRadioText(_this->_rdsIncremental).c_str());
            }
            else if (_this->rdsDecode.PSNameValid()) {
                sprintf(buf, "RDS: %s", _this->rdsDecode.getPSName(_this->_rdsIncremental).c_str());
            }
            else if (_this->rdsDecode.radioTextValid()) {
                sprintf(buf, "RDS: %s", _this->rdsDecode.getRadioText(_this->_rdsIncremental).c_str());
            }
            else {
                return;
            }

            // Calculate paddings
            ImVec2 min = args.min;
            min.x += 5.0f * style::uiScale;
            min.y += 5.0f * style::uiScale;
            ImVec2 tmin = min;
            tmin.x += 5.0f * style::uiScale;
            tmin.y += 5.0f * style::uiScale;
            ImVec2 tmax = ImGui::CalcTextSize(buf);
            tmax.x += tmin.x;
            tmax.y += tmin.y;
            ImVec2 max = tmax;
            max.x += 5.0f * style::uiScale;
            max.y += 5.0f * style::uiScale;

            // Draw back drop
            args.window->DrawList->AddRectFilled(min, max, IM_COL32(0, 0, 0, 128));

            // Draw text
            args.window->DrawList->AddText(tmin, IM_COL32(255, 255, 0, 255), buf);
        }

        dsp::demod::BroadcastFM demod;
        RDSDemod rdsDemod;
        dsp::sink::Handler<uint8_t> hs;
        EventHandler<ImGui::WaterFall::FFTRedrawArgs> fftRedrawHandler;

        dsp::buffer::Reshaper<float> reshape;
        dsp::sink::Handler<float> diagHandler;
        ImGui::SymbolDiagram diag;

        rds::Decoder rdsDecode;

        ConfigManager* _config = NULL;

        bool _stereo = false;
        bool _lowPass = true;
        bool _rds = false;
        bool _rdsInfo = false;
        bool _rdsIncremental = true;
        bool _sca = false;
        bool _showMux = false;
        float _scaKHz = 67.0f;
        float _scaGainDb = 0.0f;
        double _bandwidth = 150000.0;
        float muGain = 0.01;
        float omegaGain = (0.01*0.01)/4.0;

        int rdsRegionId = 0;
        RDSRegion rdsRegion = RDS_REGION_EUROPE;

        OptionList<std::string, RDSRegion> rdsRegions;


        std::string name;
    };
}