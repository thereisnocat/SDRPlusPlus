// rsr200_lan_transport.h pulls in <winsock2.h>/<ws2tcpip.h> on Windows -- has to come before
// any header that might drag in the legacy <winsock.h> unprotected (imgui/gui/signal_path all
// eventually reach <windows.h> on this platform, and so -- less obviously -- does
// transport_usb.h's own <FTD3XX.h> on Windows, the FTDI WinUSB driver header, which needs
// <windows.h> itself for HANDLE/DWORD/etc.), or MSVC redefines half of winsock2.h against it
// (found via Windows CI: ~100 C2011/C2375 errors, all in this module; moving this above
// imgui/gui/signal_path alone wasn't enough, since transport_usb.h -- included right before
// it -- was still winning that race on its own. Neither header depends on the other, so
// swapping their order is free). network_sink/src/main.cpp already establishes the winsock2-
// first convention -- its own <utils/networking.h> (the other winsock2 user in this codebase)
// is its first include for the same reason; matching it here rather than inventing a second
// pattern.
#include "rsr200_protocol.h"
#include "rsr200_device.h"
#include "rsr200_lan_transport.h"
#include "transport_usb.h"
#include <imgui.h>
#include <module.h>
#include <gui/gui.h>
#include <gui/smgui.h>
#include <signal_path/signal_path.h>
#include <core.h>
#include <utils/flog.h>
#include <atomic>
#include <thread>
#include <mutex>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <algorithm>

SDRPP_MOD_INFO{
    /* Name:            */ "rsr200_source",
    /* Description:     */ "Reuter RSR200B source module (USB/LAN)",
    /* Author:          */ "Ralph Brandi",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ 1
};

ConfigManager config;

#define CONCAT(a, b) ((std::string(a) + b).c_str())

// SDR++ module shell around rsr200::Device (RSR200_PLAN.md phases 0/1/1b),
// rsr200::UsbTransport (phase 6) and rsr200::LanTcpTransport (phase 2, wired into this
// module 2026-08-12 -- the transport itself already existed and passed its own tests
// against a synthetic loopback server, just wasn't reachable from the UI yet).
//
// Everything protocol- and framing-specific lives in rsr200_protocol.h/rsr200_device.h/
// transport_usb.*/rsr200_lan_transport.h, all covered by their own tests or (for the two
// transports) a live-hardware smoke test. This file is just the GUI/config/threading shell:
// menu controls set an rsr200::Config, applyConfig() sequences the commands the documents
// require, and a worker thread turns rsr200::Device::pump() into SDR++ stream writes.
// Device itself is fully transport-agnostic (Transport::isLan()/streamPort()/setLayout()),
// so switching transports here only means choosing which one to open/connect and holding a
// Transport* to it -- see activeTransport below.

using namespace rsr200;

class RSR200SourceModule : public ModuleManager::Instance {
public:
    RSR200SourceModule(std::string name) {
        this->name = name;

        loadConfig();

        handler.ctx = this;
        handler.selectHandler = menuSelected;
        handler.deselectHandler = menuDeselected;
        handler.menuHandler = menuHandler;
        handler.startHandler = start;
        handler.stopHandler = stop;
        handler.tuneHandler = tune;
        handler.stream = &out;

        // Two coherent channels, offered to core for the phasing front end -- the payoff
        // described in RSR200_PLAN.md section 7. Only registered while dual channel mode is
        // selected; otherwise this is an ordinary single-stream source.
        channels.count = 2;
        channels.streams = { &outA, &outB };
        channels.names = { "HF1", "HF2" };
        channels.phaseCoherent = true;
        channels.sampleAligned = true;

        sigpath::sourceManager.registerSource("RSR200", &handler);
        if (dualChannel) { sigpath::sourceManager.registerChannels("RSR200", &channels); }
    }

    ~RSR200SourceModule() {
        stop(this);
        sigpath::sourceManager.unregisterChannels("RSR200");
        sigpath::sourceManager.unregisterSource("RSR200");
    }

    void postInit() {}
    void enable() { enabled = true; }
    void disable() { enabled = false; }
    bool isEnabled() { return enabled; }

private:
    // Build the Config the menu currently describes.
    Config buildConfig() {
        Config c;
        c.adcClockHz = (double)adcClockMHz * 1e6;
        c.gpsDiscipline = gpsDiscipline;
        c.decimationExp = decimExp;
        c.format.channels = dualChannel ? 2 : 1;
        c.format.bits = bits24 ? 24 : 16;
        c.opMode = dualChannel ? OP_INDEPENDENT : OP_PARALLEL_ADD;
        c.swapChannels = swapChannels;
        c.tunedHz = tunedHz;
        // vhfPreamp sets the *remote power* bits (3+4), not SW_VHF_PREAMP (bit 7) --
        // confirmed 2026-08-11 from a USB packet capture of HDSDR's ExtIO module actually
        // engaging the radio's own front-panel preamp indicator. DP 3.3's own table labels
        // bit 7 "Preamplifier VHF, 0=off, 1=on", and that's what this code sent for weeks of
        // live testing -- always the textbook-correct bytes per the written spec, confirmed
        // by direct diagnostic logging, and it never once lit the indicator. The capture
        // shows HDSDR never touches bit 7 at all: the one and only command it sends when the
        // preamp is engaged is SET_VARIABLE(switch, 0x001B) -- bits 0, 1, 3, 4 -- added on
        // top of whatever was already set (0x0003, bits 0+1, from selecting VHF input
        // moments earlier). Bits 3+4 are documented as "Remote power supply HF1/VHF": bit 3
        // on/off, bit 4 plain +12V vs RS-232 "Control" mode -- normally meant for powering an
        // external active antenna (RLA4/RFA2/RAP), per DP 4.4. The straightforward reading:
        // on this hardware, the VHF preamp module is wired and powered exactly like an
        // external remote-powered accessory would be, through the same rail, rather than
        // through a separately switched internal circuit -- so bit 7 may be genuinely inert
        // on this unit/firmware regardless of what the table says it should do. Matches the
        // captured sequence exactly: SW_ADC1_TO_VHF | SW_REMOTE_PWR_CH1 | SW_REMOTE_CTRL_CH1.
        //
        // Bit 0 (SW_ADC2_CLK_INVERTED) is also always set in the capture's every command,
        // including before VHF/preamp are touched at all -- HDSDR's own idle default, not
        // something tied to this control. Left alone here: it's a dual-channel ADC2 clock
        // phase setting, unrelated to what this checkbox does, and changing our own default
        // for it isn't supported by anything actually seen going wrong so far.
        c.switchRegister = (useVhf ? SW_ADC1_TO_VHF : 0) |
                            (vhfPreamp ? (SW_REMOTE_PWR_CH1 | SW_REMOTE_CTRL_CH1) : 0) |
                            (dualChannel ? SW_ADC2_TO_HF2 : 0);
        c.attenuator1 = atten1;
        c.attenuator2 = atten2;
        c.autoAttEnabled = false;   // not yet exposed -- see RSR200_PLAN.md phase 7
        return c;
    }

    static void menuSelected(void* ctx) {
        RSR200SourceModule* _this = (RSR200SourceModule*)ctx;
        core::setInputSampleRate(_this->buildConfig().sampleRateHz());
        flog::info("RSR200SourceModule '{0}': Menu Select!", _this->name);
    }

    static void menuDeselected(void* ctx) {
        RSR200SourceModule* _this = (RSR200SourceModule*)ctx;
        flog::info("RSR200SourceModule '{0}': Menu Deselect!", _this->name);
    }

    static void start(void* ctx) {
        RSR200SourceModule* _this = (RSR200SourceModule*)ctx;
        if (_this->running) { return; }

        _this->lastError.clear();
        if (_this->transportSel == 0) {
            std::string err;
            if (!_this->usb.open(0, err)) {
                _this->lastError = "open failed: " + err;
                flog::error("RSR200SourceModule '{0}': {1}", _this->name, _this->lastError);
                return;
            }
            _this->activeTransport = &_this->usb;
        }
        else {
            if (!_this->lan.connect(_this->lanHost)) {
                _this->lastError = "LAN connect failed: " + _this->lan.lastError();
                flog::error("RSR200SourceModule '{0}': {1}", _this->name, _this->lastError);
                return;
            }
            _this->activeTransport = &_this->lan;
        }

        _this->device.setTransport(_this->activeTransport);
        _this->device.onError = [_this](const std::string& msg) {
            std::lock_guard<std::mutex> lck(_this->statusMtx);
            _this->lastError = msg;
            flog::error("RSR200SourceModule '{0}': {1}", _this->name, msg);
        };
        _this->device.onReply = [_this](const Reply& r) {
            if (r.kind != REPLY_VERSION) { return; }
            std::lock_guard<std::mutex> lck(_this->statusMtx);
            _this->verSerial = r.serial;
            _this->verFirmware = r.firmware;
            _this->haveVersion = true;
        };
        _this->device.onSamples = [_this](const SampleBlock& b) { _this->deliver(b); };

        const uint64_t now = nowMs();
        const Config cfg = _this->buildConfig();
        if (!_this->device.applyConfig(cfg, now)) {
            _this->lastError = "initial configuration failed";
            _this->closeActiveTransport();
            return;
        }

        // OM section 6.2: channel 2's diversity magnitude/phase weight (command 0xB0,
        // selector 9) sits in the signal path even in Sep mode -- the official software sets
        // it to unity (magnitude 1.0, phase 0) when switching to Sep, and the DP documents
        // adjustable values as defaulting to zero on power-up. Without this, channel 2 reads
        // as a clean, exact zero: real ADC2 data multiplied by a zero weight looks identical
        // to no data at all. Confirmed against real hardware in test/test_usb_dual_live.cpp
        // (RSR200_PLAN.md section 10) -- omitting this was the entire cause of the "ADC2 is
        // dead" misdiagnosis earlier in that section.
        if (cfg.format.channels == 2) {
            if (!_this->device.setHardwareDiversity(1.0, 0.0, now)) {
                _this->lastError = "failed to set channel 2 to unity gain";
                _this->closeActiveTransport();
                return;
            }
        }

        // A cheap, read-only probe -- not modelled on Device (which has no "send an
        // unsolicited command" method), so sent directly through the transport. isLan()
        // picks the right (USB fixed-length vs LAN shortened) command encoding regardless
        // of which transport is actually active.
        auto verCmd = cmdReadVersion(9999, _this->activeTransport->isLan());
        _this->activeTransport->sendCommand(verCmd.data(), verCmd.size());
        if (_this->activeTransport->isLan()) {
            // Packet mode (streaming hasn't started yet): the version reply is its own
            // standalone 12-byte packet, not embedded in a block -- there is no block for it
            // to embed into yet. Read and parse it directly rather than relying on
            // Device::onReply seeing it appear in a stream that doesn't exist. See
            // RSR200_PLAN.md's "First live LAN connection" section.
            std::vector<uint8_t> verReply;
            if (_this->activeTransport->readPacket(verReply, 12)) {
                Reply r;
                if (parseLanVersionPacket(verReply.data(), verReply.size(), r) && _this->device.onReply) {
                    _this->device.onReply(r);
                }
            }
        }
        // For USB the reply arrives embedded in the stream like any other (DP 3.3), and
        // Device::onReply above already watches for REPLY_VERSION regardless of who sent
        // the command.

        if (!_this->device.startStream(now)) {
            _this->lastError = "Start Stream failed";
            _this->closeActiveTransport();
            return;
        }

        _this->run = true;
        _this->workerThread = std::thread(&RSR200SourceModule::worker, _this);

        _this->running = true;
        flog::info("RSR200SourceModule '{0}': Start!", _this->name);
    }

    static void stop(void* ctx) {
        RSR200SourceModule* _this = (RSR200SourceModule*)ctx;
        if (!_this->running) { return; }
        _this->running = false;

        // Interrupt the worker's blocked read before touching anything it might still be
        // using, then join, then a full close(). For USB, that's abortReads() (see
        // transport_usb.h -- releasing the buffer/OVERLAPPED pool out from under a thread
        // still waiting on it is a race); for LAN, LanTcpTransport::stop() closes the
        // socket, which is the only portable way to unstick a thread blocked in a BSD
        // recv() call. DP 3.3: Stop Stream closes the USB endpoint entirely regardless, so
        // the next Start has to reopen and reconfigure from scratch either way; the LAN
        // side reconnects fresh next Start for the same reason, by symmetry.
        _this->run = false;
        if (_this->activeTransport == &_this->lan) { _this->lan.stop(); }
        else { _this->usb.abortReads(); }
        _this->out.stopWriter();
        _this->outA.stopWriter();
        _this->outB.stopWriter();
        if (_this->workerThread.joinable()) { _this->workerThread.join(); }
        _this->out.clearWriteStop();
        _this->outA.clearWriteStop();
        _this->outB.clearWriteStop();

        // activeTransport may already be disconnected (LAN: stop() above closed the socket)
        // -- sendCommand() on a dead transport just returns false, which is fine here, the
        // radio's own Stop Stream handling is best-effort on a link that's already gone.
        if (_this->activeTransport) {
            auto stopCmd = cmdStopStream(9998, _this->activeTransport->isLan(), _this->activeTransport->streamPort());
            _this->activeTransport->sendCommand(stopCmd.data(), stopCmd.size());
        }
        _this->closeActiveTransport();

        flog::info("RSR200SourceModule '{0}': Stop!", _this->name);
    }

    // Closes whichever transport is active (if any), tells Device to let go of it, and
    // clears activeTransport -- the one place all of start()'s failure paths and stop()
    // itself funnel through, so neither has to know which concrete transport is in use.
    void closeActiveTransport() {
        if (activeTransport == &lan) { lan.close(); }
        else if (activeTransport == &usb) { usb.close(); }
        activeTransport = nullptr;
        device.setTransport(nullptr);
    }

    static void tune(double freq, void* ctx) {
        RSR200SourceModule* _this = (RSR200SourceModule*)ctx;
        _this->tunedHz = freq;
        if (_this->running) { _this->device.tune(freq, nowMs()); }
    }

    static uint64_t nowMs() {
        return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // Runs on the worker thread (the same thread Device::onSamples/onReply/onError fire on,
    // since they're called synchronously out of pump()).
    void worker() {
        while (run) {
            if (!device.pump()) { break; }
            device.service(nowMs());
        }
    }

    // Device hands back pointers into its own internal buffers, already unpacked to
    // interleaved float re/im and Auto-ATT compensated -- copy straight into the stream's
    // write buffer and swap, the same shape as every other source module's worker.
    //
    // Simple and direct on purpose. A whole line of fixes was tried here for LAN audio
    // choppiness -- chunking the hand-off, pacing chunks to real time, a genuine jitter
    // buffer, adapting its drain rate to a measured delivery rate -- and none of it helped;
    // the last version made things worse. See RSR200_PLAN.md's LAN section (2026-08-12
    // audio choppiness investigation) for the full chain: live measurement across every one
    // of those attempts converged on the same conclusion, that the RSR200's LAN interface
    // genuinely cannot sustain its own nominal decimation rate in real time (a recurring
    // stall tied to block count, present at the lowest documented decimation setting, with
    // wire bandwidth nowhere near a real constraint) -- not a bug reachable from this
    // module's own delivery code, so no amount of buffering or pacing on this side fixes it.
    // Reverted to this simple form rather than carry the complexity of a fix that doesn't
    // fix anything. USB is unaffected and remains the clean path for real-time listening.
    void deliver(const SampleBlock& b) {
        {
            std::lock_guard<std::mutex> lck(statusMtx);
            lastStatus = b.status;
            lastSequenceGap = b.sequenceGap;
            if (b.sequenceGap) { gapCount++; }
        }

        if (b.chB) {
            memcpy(outA.writeBuf, b.chA, (size_t)b.frames * sizeof(dsp::complex_t));
            memcpy(outB.writeBuf, b.chB, (size_t)b.frames * sizeof(dsp::complex_t));
            if (!outA.swap(b.frames)) { run = false; }
            if (!outB.swap(b.frames)) { run = false; }
        }
        else {
            memcpy(out.writeBuf, b.chA, (size_t)b.frames * sizeof(dsp::complex_t));
            if (!out.swap(b.frames)) { run = false; }
        }
    }

    static void menuHandler(void* ctx) {
        RSR200SourceModule* _this = (RSR200SourceModule*)ctx;
        bool dirty = false;

        if (_this->running) { SmGui::BeginDisabled(); }

        SmGui::LeftLabel("Transport");
        SmGui::FillWidth();
        if (SmGui::Combo(CONCAT("##_rsr200_transport_", _this->name), &_this->transportSel, "USB\0LAN (TCP)\0")) {
            dirty = true;
        }
        if (_this->transportSel != 0) {
            SmGui::LeftLabel("Radio IP address");
            SmGui::FillWidth();
            if (SmGui::InputText(CONCAT("##_rsr200_lanhost_", _this->name), _this->lanHost, sizeof(_this->lanHost))) {
                dirty = true;
            }
        }

        SmGui::LeftLabel("ADC clock (MHz)");
        SmGui::FillWidth();
        if (SmGui::SliderFloatWithSteps(CONCAT("##_rsr200_clk_", _this->name), &_this->adcClockMHz,
                                        70.0f, 200.0f, 0.1f, SmGui::FMT_STR_FLOAT_ONE_DECIMAL)) {
            // Affects sampleRateHz() -- core has to be told immediately, not just whenever
            // dirty next gets handled, or it keeps demodulating at whatever rate was in
            // effect when the source was first selected.
            core::setInputSampleRate(_this->buildConfig().sampleRateHz());
            dirty = true;
        }

        if (SmGui::Checkbox(CONCAT("GPS discipline##_rsr200_gps_", _this->name), &_this->gpsDiscipline)) {
            dirty = true;
        }

        SmGui::LeftLabel("Decimation");
        SmGui::FillWidth();
        SmGui::ForceSync();
        char decItems[256];
        {
            // "2\0004\0008\00016\00032\00064\000" but built from the real formula (DP 3.3)
            // rather than hand-copied, so it can't drift from decimationRate().
            int off = 0;
            for (int e = 0; e <= 5; e++) {
                off += snprintf(decItems + off, sizeof(decItems) - off, "%d", decimationRate(e));
                decItems[off++] = '\0';
            }
            decItems[off] = '\0';
        }
        if (SmGui::Combo(CONCAT("##_rsr200_dec_", _this->name), &_this->decimExp, decItems)) {
            core::setInputSampleRate(_this->buildConfig().sampleRateHz());
            dirty = true;
        }

        if (SmGui::Checkbox(CONCAT("24-bit##_rsr200_24_", _this->name), &_this->bits24)) {
            dirty = true;
        }

        if (!_this->running) { SmGui::ForceSync(); }
        if (SmGui::Checkbox(CONCAT("Dual channel (HF1 + HF2, phasing)##_rsr200_dual_", _this->name), &_this->dualChannel)) {
            if (_this->dualChannel) {
                sigpath::sourceManager.registerChannels("RSR200", &_this->channels);
            }
            else {
                sigpath::sourceManager.unregisterChannels("RSR200");
            }
            dirty = true;
        }
        if (_this->dualChannel) {
            SmGui::Text("HF1 -> channel A (ADC1), HF2 -> channel B (ADC2).");
        }

        if (SmGui::Checkbox(CONCAT("Swap channels##_rsr200_swap_", _this->name), &_this->swapChannels)) {
            dirty = true;
        }

        SmGui::Text("Channel 1 antenna input");
        if (SmGui::Checkbox(CONCAT("Use VHF input (else HF1)##_rsr200_vhf_", _this->name), &_this->useVhf)) {
            dirty = true;
        }
        if (SmGui::Checkbox(CONCAT("VHF preamp##_rsr200_preamp_", _this->name), &_this->vhfPreamp)) {
            dirty = true;
        }

        SmGui::LeftLabel("Attenuator 1 (0 = +7dB gain, 35 = -28dB)");
        SmGui::FillWidth();
        if (SmGui::SliderInt(CONCAT("##_rsr200_att1_", _this->name), &_this->atten1, 0, 35)) {
            dirty = true;
        }
        if (_this->dualChannel) {
            SmGui::LeftLabel("Attenuator 2");
            SmGui::FillWidth();
            if (SmGui::SliderInt(CONCAT("##_rsr200_att2_", _this->name), &_this->atten2, 0, 35)) {
                dirty = true;
            }
        }

        if (_this->running) { SmGui::EndDisabled(); }

        const Config cur = _this->buildConfig();
        char buf[256];
        snprintf(buf, sizeof(buf), "Sample rate: %.3f MSp/s", cur.sampleRateHz() / 1e6);
        SmGui::Text(buf);

        // -- Status, updated from the worker thread --------------------------------
        SmGui::Text("Status");
        if (_this->running) {
            std::lock_guard<std::mutex> lck(_this->statusMtx);
            if (_this->haveVersion) {
                snprintf(buf, sizeof(buf), "Serial %u, firmware %u", _this->verSerial, _this->verFirmware);
            }
            else {
                snprintf(buf, sizeof(buf), "Serial/firmware: waiting for reply...");
            }
            SmGui::Text(buf);

            if (_this->lastStatus.autoAttActive) {
                SmGui::Text("Temperature: Auto-ATT active");
            }
            else {
                snprintf(buf, sizeof(buf), "Temperature: %d C", _this->lastStatus.temperatureC);
                SmGui::Text(buf);
            }
            snprintf(buf, sizeof(buf), "Overload: CH1 %s  CH2 %s",
                     _this->lastStatus.overloadCh1 ? "YES" : "no",
                     _this->lastStatus.overloadCh2 ? "YES" : "no");
            SmGui::Text(buf);
            snprintf(buf, sizeof(buf), "Sequence gaps seen: %llu", (unsigned long long)_this->gapCount);
            SmGui::Text(buf);
            if (!_this->lastError.empty()) {
                snprintf(buf, sizeof(buf), "Error: %s", _this->lastError.c_str());
                SmGui::Text(buf);
            }
        }
        else {
            SmGui::Text("Not running.");
            if (!_this->lastError.empty()) {
                snprintf(buf, sizeof(buf), "Last error: %s", _this->lastError.c_str());
                SmGui::Text(buf);
            }
        }

        // core::setInputSampleRate() for the rate-affecting controls (ADC clock, decimation)
        // is called right where they change, above -- not here. Every rate-affecting control
        // is disabled while running (see BeginDisabled/EndDisabled above), so `dirty` can
        // never be set by them while running; gating a repeat of that call on `running` here
        // would be unreachable dead code, not a real second code path.
        if (dirty) { _this->saveConfig(); }
    }

    void loadConfig() {
        config.acquire();
        json& c = config.conf;
        if (c.contains("adcClockMHz")) { adcClockMHz = c["adcClockMHz"]; }
        if (c.contains("gpsDiscipline")) { gpsDiscipline = c["gpsDiscipline"]; }
        if (c.contains("decimExp")) { decimExp = c["decimExp"]; }
        if (c.contains("bits24")) { bits24 = c["bits24"]; }
        if (c.contains("dualChannel")) { dualChannel = c["dualChannel"]; }
        if (c.contains("swapChannels")) { swapChannels = c["swapChannels"]; }
        if (c.contains("useVhf")) { useVhf = c["useVhf"]; }
        if (c.contains("vhfPreamp")) { vhfPreamp = c["vhfPreamp"]; }
        if (c.contains("atten1")) { atten1 = c["atten1"]; }
        if (c.contains("atten2")) { atten2 = c["atten2"]; }
        if (c.contains("transportSel")) { transportSel = c["transportSel"]; }
        if (c.contains("lanHost")) {
            std::string h = c["lanHost"];
            strncpy(lanHost, h.c_str(), sizeof(lanHost) - 1);
            lanHost[sizeof(lanHost) - 1] = '\0';
        }
        config.release();
    }

    void saveConfig() {
        config.acquire();
        json& c = config.conf;
        c["adcClockMHz"] = adcClockMHz;
        c["gpsDiscipline"] = gpsDiscipline;
        c["decimExp"] = decimExp;
        c["bits24"] = bits24;
        c["dualChannel"] = dualChannel;
        c["swapChannels"] = swapChannels;
        c["useVhf"] = useVhf;
        c["vhfPreamp"] = vhfPreamp;
        c["atten1"] = atten1;
        c["atten2"] = atten2;
        c["transportSel"] = transportSel;
        c["lanHost"] = std::string(lanHost);
        config.release(true);
    }

    std::string name;
    bool enabled = true;
    bool running = false;

    SourceManager::SourceHandler handler;
    dsp::stream<dsp::complex_t> out;

    // Dual channel mode: the two raw channels, plus the set describing them to core.
    dsp::stream<dsp::complex_t> outA;
    dsp::stream<dsp::complex_t> outB;
    ChannelSet channels;

    // GUI-facing, persisted settings.
    float adcClockMHz = 125.0f;
    bool gpsDiscipline = true;
    int decimExp = 3;             // rate 16, matching rsr200::Config's own default
    bool bits24 = false;
    bool dualChannel = false;
    bool swapChannels = false;
    bool useVhf = false;
    bool vhfPreamp = false;
    int atten1 = 0;
    int atten2 = 0;
    double tunedHz = 10e6;

    // Transport selection. 0 = USB, 1 = LAN (TCP) -- matches transportItems' order in
    // menuHandler(). LAN UDP (RSR200_PLAN.md phase 5) isn't implemented yet, so it isn't
    // offered here. lanHost is the radio's LAN IP -- the RSR200 documents a static default
    // of 192.168.1.10 but also runs a DHCP client for ~5s after power-up, so a real radio on
    // a real network may land on a DHCP-assigned address instead; there's no discovery
    // mechanism (no mDNS/broadcast in the documented protocol), so this has to be typed in.
    // Plain char buffer, not std::string, matching every other source module's InputText
    // field (network_source, rtl_tcp_source, spyserver_source, ...).
    int transportSel = 0;
    char lanHost[64] = "192.168.1.10";

    // Transport + protocol layer. Device (rsr200_device.h) is fully transport-agnostic --
    // it only ever talks to whichever Transport* was handed to setTransport(), and asks
    // that object (isLan()/streamPort()) rather than assuming USB. usb/lan are the two
    // concrete transports; activeTransport points at whichever start() opened, so the rest
    // of this file (the version-query probe, the Stop Stream command) can go through it
    // without caring which one it is either.
    UsbTransport usb;
    LanTcpTransport lan;
    Transport* activeTransport = nullptr;
    Device device;

    // Worker thread and the status it publishes, guarded by statusMtx since the GUI thread
    // reads it while the worker thread (running Device's callbacks) writes it.
    std::thread workerThread;
    std::atomic<bool> run = false;

    std::mutex statusMtx;
    Status lastStatus;
    bool lastSequenceGap = false;
    uint64_t gapCount = 0;
    bool haveVersion = false;
    uint32_t verSerial = 0;
    uint32_t verFirmware = 0;
    std::string lastError;
};

MOD_EXPORT void _INIT_() {
    json def = json({});
    config.setPath(core::args["root"].s() + "/rsr200_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new RSR200SourceModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (RSR200SourceModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
