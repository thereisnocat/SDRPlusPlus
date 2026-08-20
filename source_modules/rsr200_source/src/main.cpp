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
#include <gui/dialogs/dialog_box.h>
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
        handler.captureConfigHandler = captureConfig;
        handler.applyConfigHandler = applyConfig;
        handler.moduleType = "rsr200_source";   // must match SDRPP_MOD_INFO's Name above

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
        // Found live 2026-08-20: hardware diversity still needs the *wire* format at 2
        // channels, same as Separate mode -- only the DSP mode below (OP_DIVERSITY) changes.
        // The first version of this code dropped to format.channels = 1 for hwDiversityMode,
        // on the assumption that since the radio combines the channels internally, only one
        // channel's worth of data needs to cross the wire. Wrong: RSR200_OM_V225.pdf's own
        // changelog says the vendor software's "AntDiv" preset "switches the RSR200B to
        // 2-channel reception for antenna diversity" -- and live testing confirmed it: with
        // format.channels forced to 1, the app received something, but every other sample
        // belonged to the *other* physical channel, producing a strong, regular comb of
        // spurs across the entire band (an evenly-spaced-artifact signature of exactly this
        // kind of deinterleaving mismatch) instead of a real spectrum. See deliver() below for
        // which of the two received channels actually carries the combined result.
        c.format.channels = dualChannel ? 2 : 1;
        c.format.bits = bits24 ? 24 : 16;
        c.tunedHz = tunedHz;
        // Highest priority: hardware diversity (RSR200_PLAN.md phase 7) combines the channels
        // on the radio -- format.channels above stays 2 (see the comment there), only opMode
        // changes. dualChannel means "I'm using both antennas together"; hwDiversityMode is a
        // layered sub-state reached only through the solve/apply workflow below, not a plain
        // checkbox, saying *how* they're currently combined (radio vs. software). Leaving
        // dualChannel itself checked while hwDiversityMode is on is deliberate -- "Back to
        // Separate mode" needs to know to re-register two channels.
        if (hwDiversityMode) {
            c.opMode = OP_DIVERSITY;
        }
        // Serial mode is single-channel only -- dualChannel wins if both are somehow set (the
        // UI only shows the Serial control while !dualChannel, but this keeps buildConfig()
        // itself correct regardless of how the fields got into that state).
        else if (dualChannel) {
            c.opMode = OP_INDEPENDENT;
        }
        else if (serialMode) {
            c.opMode = OP_SERIAL;
            // A manual choice, per Ralph (2026-08-19): picking the wrong sideband costs ~30dB
            // on the wanted signal instead of the interferer, but it's a quick, obvious, and
            // easily-recoverable mistake while tuning -- just flip to the other one -- not
            // something worth taking the choice away for. See Config::upperSideband's own
            // comment in rsr200_device.h for the zone-parity rule the UI hints at (but doesn't
            // enforce) to make the right first guess easy.
            c.upperSideband = serialUpper;
        }
        else {
            c.opMode = OP_PARALLEL_ADD;
        }
        c.swapChannels = swapChannels;
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
        // something tied to this control. Left off by our own default here: it's a
        // dual-channel ADC2 clock phase setting, unrelated to what this checkbox does, and
        // changing our own default for it isn't supported by anything actually seen going
        // wrong so far -- *except* Serial mode, where RSR200_DP_ENG_V52.pdf is explicit this
        // bit is a required precondition ("2 = ADC1 + ADC2 serial (CLK ADC2 must be
        // inverted!)"), so it's set whenever serialMode is on, regardless of what HDSDR's own
        // idle-default behavior otherwise suggests about leaving it alone.
        c.switchRegister = (useVhf ? SW_ADC1_TO_VHF : 0) |
                            (vhfPreamp ? (SW_REMOTE_PWR_CH1 | SW_REMOTE_CTRL_CH1) : 0) |
                            (dualChannel ? SW_ADC2_TO_HF2 : 0) |
                            (serialMode ? SW_ADC2_CLK_INVERTED : 0);
        c.attenuator1 = atten1;
        c.attenuator2 = atten2;
        c.autoAttThreshold = autoAttThreshold;
        c.autoAttHoldTimeSec = autoAttHoldTimeSec;
        c.autoAttGainCh1 = autoAttGainCh1;
        c.autoAttGainCh2 = autoAttGainCh2;
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

        // Reload (or first-time-seed) this device/host's own stored settings before touching
        // anything else -- matches rfspace_source/spyserver_source's own connect-time reload,
        // so a transport/host/USB-device change made in the menu since construction (or since
        // the last Start) takes effect now rather than starting with whatever the previous
        // device's fields happened to be.
        _this->loadDeviceSettings();

        if (_this->transportSel == 0) {
            std::string err;
            bool opened = !_this->selectedUsbSerial.empty() &&
                          _this->usb.openBySerial(_this->selectedUsbSerial, err);
            if (!opened) {
                // Fall back to "whatever's plugged in first" -- matches RTL-SDR's own
                // selectFirst() fallback when the saved/chosen device isn't present, and covers
                // the common single-device case where there's nothing to actually choose from.
                opened = _this->usb.open(0, err);
            }
            if (!opened) {
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
            bool learnedNewSerial;
            {
                std::lock_guard<std::mutex> lck(_this->statusMtx);
                learnedNewSerial = !_this->haveVersion || _this->verSerial != r.serial;
                _this->verSerial = r.serial;
                _this->verFirmware = r.firmware;
                _this->haveVersion = true;
            }
            // Outside statusMtx -- learnUsbRadioSerial() takes config's own lock, and this
            // callback can fire from either the GUI thread (LAN, synchronously within start())
            // or the worker thread (USB, from within pump()), so keeping the two locks
            // non-overlapping avoids adding any new lock-ordering constraint between them.
            if (learnedNewSerial) { _this->learnUsbRadioSerial(r.serial); }
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
        //
        // format.channels == 2 now covers *both* Sep mode and hardware diversity (see
        // buildConfig()'s comment -- found live 2026-08-20 that Diversity mode still needs the
        // 2-channel wire format), so hwDiversityMode has to be checked first within this
        // branch, not as a sibling `else if` the way it was before that fix -- that older
        // shape relied on Diversity being format.channels == 1 and would otherwise send the
        // solved weight followed immediately by unity, always ending on unity.
        if (cfg.format.channels == 2) {
            if (_this->hwDiversityMode) {
                // Hardware diversity mode (RSR200_PLAN.md phase 7): the weight was already
                // solved and confirmed by the "Solve from current phasing"/"Apply to hardware"
                // UI before this start() was triggered (see those handlers -- applying hardware
                // diversity goes through a full stop()/start() cycle rather than a live
                // reconfigure, deliberately, to reuse this already-tested startup sequence
                // instead of writing a new one that changes Config while a worker thread might
                // still be touching related state). cfg.opMode is already OP_DIVERSITY by the
                // time buildConfig() built this Config, so applyConfig() above has already told
                // the radio to combine the channels in hardware -- this is just handing it the
                // actual weight to combine them *with*.
                if (!_this->device.setHardwareDiversity(_this->hwDivMagnitude, _this->hwDivPhaseDeg, now)) {
                    _this->lastError = "failed to set hardware diversity weight";
                    _this->closeActiveTransport();
                    return;
                }
            }
            else if (!_this->device.setHardwareDiversity(1.0, 0.0, now)) {
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

        if (b.chB && hwDiversityMode) {
            // Hardware diversity (RSR200_PLAN.md phase 7): the wire is still 2-channel (see
            // buildConfig()'s comment), but nothing reads outA/outB while this mode has the
            // ChannelSet unregistered -- writing there would just block deliver() forever the
            // first time canSwap never comes true, since no reader is left to drain it. The
            // radio has already combined the two channels itself by this point; per DP/OM's own
            // wording ("the data stream from channel 2 is added to data stream 1") and the OM's
            // note that channel 2's own overload ("OV") detection keeps working independently
            // in Diversity mode, channel 1 is the one carrying the combined result -- channel B
            // here is raw ADC2, kept only for the radio's own internal use, not something this
            // module has any reader for. Not yet confirmed against real hardware which channel
            // actually holds the combined signal (RSR200_PLAN.md phase 7 dated section) -- if
            // the spectrum still looks wrong after this fix, the two-line swap to read b.chB
            // instead is the next thing to try, not a deeper redesign.
            memcpy(out.writeBuf, b.chA, (size_t)b.frames * sizeof(dsp::complex_t));
            if (!out.swap(b.frames)) { run = false; }
        }
        else if (b.chB) {
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
        if (_this->transportSel == 0) {
            SmGui::LeftLabel("USB device");
            std::string devItems;
            for (auto& [desc, serial] : _this->usbDeviceList) {
                devItems += desc + " (" + serial + ")";
                devItems += '\0';
            }
            devItems += '\0';
            bool noDevices = _this->usbDeviceList.empty();
            if (noDevices) { SmGui::BeginDisabled(); }
            SmGui::FillWidth();
            if (SmGui::Combo(CONCAT("##_rsr200_usbdev_", _this->name), &_this->usbDeviceId, devItems.c_str())) {
                _this->selectedUsbSerial = _this->usbDeviceList[_this->usbDeviceId].second;
                dirty = true;
            }
            if (noDevices) { SmGui::EndDisabled(); }
            // Own row, not SameLine() with the combo above -- that combo already claims the
            // panel's full width (matching every other single-combo row in this file, e.g.
            // Transport/Decimation just above/below), so there's no room left on that line for
            // a button; RTL-SDR's own combo+button same-line layout works there because its
            // device combo has no LeftLabel in front of it eating into the row first.
            if (SmGui::Button(CONCAT("Refresh##_rsr200_usbrefresh_", _this->name))) {
                _this->refreshUsbDeviceList();
            }
            if (noDevices) {
                SmGui::Text("No RSR200 (D3XX) devices found -- Start will still try the first "
                            "one seen, if any shows up.");
            }
        }
        else {
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

        // Serial mode ("SerL"/"SerU", RSR200_PLAN.md phase 7, RSR200_OM_V225.pdf's own "Use of
        // SerL and SerU" section) -- single-channel only, so only offered while dual channel is
        // off. Time-interleaves the two ADCs onto one channel for ~30dB of digital rejection of
        // whichever regular-width Nyquist zone neighbors the one actually being received, on
        // top of (or instead of) external analog filtering -- it does not change the delivered
        // sample rate or zone width (see Config::sampleRateHz()'s own comment in
        // rsr200_device.h), so no core::setInputSampleRate() call is needed here, unlike every
        // other control on this panel that affects the sampling rate.
        //
        // Lower/Upper is a manual choice, per Ralph (2026-08-19): picking the wrong one costs
        // ~30dB on the wanted signal instead of the interferer, but it's a quick, obvious,
        // easily-recoverable mistake while tuning -- flip to the other one -- not something
        // worth taking the choice away for. The hint text names which one the manual's own
        // zone-parity rule (odd zone -> SerL, even -> SerU, from tuneFor() -- the same pure
        // zone/LO function Device::tune() itself uses) suggests for the *current* tuning, so
        // the right first guess is easy without removing the override.
        if (!_this->dualChannel) {
            if (SmGui::Checkbox(CONCAT("Serial mode (SerL/SerU)##_rsr200_serial_", _this->name), &_this->serialMode)) {
                dirty = true;
            }
            if (_this->serialMode) {
                bool suggestUpper = (tuneFor(_this->tunedHz, (double)_this->adcClockMHz * 1e6).zone % 2 == 0);
                if (SmGui::RadioButton(CONCAT("SerL (lower)##_rsr200_serl_", _this->name), !_this->serialUpper)) {
                    _this->serialUpper = false;
                    dirty = true;
                }
                SmGui::SameLine();
                if (SmGui::RadioButton(CONCAT("SerU (upper)##_rsr200_seru_", _this->name), _this->serialUpper)) {
                    _this->serialUpper = true;
                    dirty = true;
                }
                SmGui::Text(suggestUpper ? "Current tuning suggests SerU" : "Current tuning suggests SerL");
                // Known limitation, not yet closed: retuning is deliberately a lightweight
                // Device::tune() call (just the LO command -- DP §4.6's own "no
                // resynchronisation necessary" guarantee is what makes live retuning safe at
                // all), which never revisits this bit or the hint above. Crossing a Nyquist
                // zone boundary while running would need the DSP mode byte resent too, and
                // RSR200_DP_ENG_V52.pdf is explicit that changing it (unlike tuning) stops
                // streaming and requires an explicit restart -- a real operational sequence,
                // not a one-line fix, and not yet implemented or verified against the radio.
                SmGui::Text("Retuning across a zone boundary while running needs a stop/restart\nfor the hint (and SerL/SerU) to catch up.");
            }
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

        // DP: "When activating the Auto-ATT function, limit the manual adjustment range to a
        // maximum of step 19" -- the automatic +16dB step needs headroom above whatever the
        // manual setting already used, so both sliders' own max bound becomes conditional
        // while Auto-ATT is on, not just a separate clamp applied elsewhere.
        const int attMax = (_this->autoAttThreshold > 0) ? 19 : 35;
        SmGui::LeftLabel("Attenuator 1 (0 = +7dB gain, 35 = -28dB)");
        SmGui::FillWidth();
        if (SmGui::SliderInt(CONCAT("##_rsr200_att1_", _this->name), &_this->atten1, 0, attMax)) {
            dirty = true;
        }
        if (_this->dualChannel) {
            SmGui::LeftLabel("Attenuator 2");
            SmGui::FillWidth();
            if (SmGui::SliderInt(CONCAT("##_rsr200_att2_", _this->name), &_this->atten2, 0, attMax)) {
                dirty = true;
            }
        }

        // Auto-ATT (RSR200_PLAN.md phase 7). The threshold combo doubles as the on/off
        // control -- "Off" *is* threshold 0, so there's no separate checkbox that could ever
        // disagree with it. Resolved with Ralph 2026-08-20: starts at Off until explicitly
        // raised, and a manual attenuator above 19 is silently clamped down the moment the
        // threshold leaves Off, matching how every other control here already auto-corrects
        // without a prompt.
        SmGui::LeftLabel("Auto-ATT threshold");
        SmGui::FillWidth();
        const int prevThreshold = _this->autoAttThreshold;
        if (SmGui::Combo(CONCAT("##_rsr200_autoatt_thresh_", _this->name), &_this->autoAttThreshold,
                          "Off\0-6 dB\0-12 dB\0-18 dB\0-24 dB\0-30 dB\0")) {
            if (prevThreshold == 0 && _this->autoAttThreshold > 0) {
                _this->atten1 = std::min(_this->atten1, 19);
                _this->atten2 = std::min(_this->atten2, 19);
            }
            dirty = true;
        }
        if (_this->autoAttThreshold > 0) {
            SmGui::LeftLabel("Hold time (s)");
            SmGui::FillWidth();
            if (SmGui::SliderFloat(CONCAT("##_rsr200_autoatt_hold_", _this->name),
                                    &_this->autoAttHoldTimeSec, 0.0f, 2.0f)) {
                dirty = true;
            }
            // Hold time is a 24 bit raw-clock-cycle field on the wire (DP), not seconds -- at
            // typical ADC clock rates that field caps out well under a second (e.g. ~134ms at
            // 125 MHz), so a value that reads as perfectly reasonable in this seconds-based
            // slider can silently be more than the radio can actually hold. Shown here rather
            // than just clamping the slider's own range, since the achievable ceiling moves
            // with the ADC clock control above.
            const double adcHz = (double)_this->adcClockMHz * 1e6;
            const double actualHoldSec = (double)autoAttHoldTimeClocks(_this->autoAttHoldTimeSec, adcHz) / adcHz;
            if (actualHoldSec + 1e-6 < _this->autoAttHoldTimeSec) {
                char holdBuf[128];
                snprintf(holdBuf, sizeof(holdBuf), "Clamped to %.3f s at the current ADC clock (24 bit field).", actualHoldSec);
                SmGui::Text(holdBuf);
            }

            SmGui::LeftLabel("Gain calibration, channel 1");
            SmGui::FillWidth();
            if (SmGui::SliderFloat(CONCAT("##_rsr200_autoatt_gain1_", _this->name),
                                    &_this->autoAttGainCh1, 0.5f, 20.0f)) {
                dirty = true;
            }
            if (_this->dualChannel) {
                SmGui::LeftLabel("Gain calibration, channel 2");
                SmGui::FillWidth();
                if (SmGui::SliderFloat(CONCAT("##_rsr200_autoatt_gain2_", _this->name),
                                        &_this->autoAttGainCh2, 0.5f, 20.0f)) {
                    dirty = true;
                }
            }
            SmGui::Text("Compensates the attenuator's own ~16dB while engaged (nominal 6.3096x) --\n"
                        "a device tolerance to calibrate, not usually something to change by feel.");
            if (!_this->bits24) {
                SmGui::Text("Warning: 16-bit's usable resolution drops to an effective 14 bits under\n"
                            "Auto-ATT. 24-bit is recommended whenever Auto-ATT is on.");
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
            // Scoped tightly around just the reads of statusMtx-protected fields below --
            // found live 2026-08-20: this lock previously had no closing brace of its own,
            // so it stayed held for the rest of the whole `if (_this->running)` block,
            // including the hardware-diversity buttons further down that call stop(_this)
            // (which joins workerThread). deliver() (running on workerThread) takes this
            // same statusMtx to record each block's status -- so the GUI thread would hold
            // statusMtx, block forever in join() waiting for workerThread, while
            // workerThread blocked forever waiting for statusMtx the GUI thread was never
            // going to release. A true self-deadlock, reported by Ralph as the app freezing
            // with a spinning cursor the moment "Apply to hardware"'s confirmation dialog
            // was accepted.
            {
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
                // RSR200_PLAN.md phase 7: "GPS Hz" in Reuter's own control panel -- the deviation
                // of the ADC clock frequency the GPS receiver measures from the set value,
                // whether or not GPS discipline is actually correcting for it (OM: disabling
                // discipline still "displays the current deviation of the ADC clock" without
                // applying it). Both freqCorrectionHz() and the resolution it picks by
                // gpsDiscipline (0.5Hz/LSB disciplining, 0.1Hz/LSB measuring-only) already
                // existed and are already covered by test_protocol.cpp -- this is only the
                // display line, no new fields or commands.
                if (_this->lastStatus.freqCorrectionValid) {
                    snprintf(buf, sizeof(buf), "GPS correction: %+.1f Hz", freqCorrectionHz(_this->lastStatus, _this->gpsDiscipline));
                }
                else {
                    snprintf(buf, sizeof(buf), "GPS correction: no valid measurement (GPS not received, or just retuned)");
                }
                SmGui::Text(buf);
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

            // Hardware diversity (RSR200_PLAN.md phase 7, PHASING_PLAN.md §7.2's own "solve in
            // software, hold in hardware" two-step). Deliberately placed outside the
            // BeginDisabled()/EndDisabled() range above and gated on _this->running instead --
            // every other control on this panel is a static setting only editable while
            // stopped, but solving needs live phasing data and applying needs a live source to
            // apply *to*. Only offered while dualChannel is on and hardware diversity isn't
            // already active -- solving in Diversity mode is meaningless (the radio only
            // returns the already-combined result by then, PHASING_PLAN.md §7.1).
            if (_this->dualChannel && !_this->hwDiversityMode) {
                SmGui::Text("Hardware diversity");
                if (SmGui::Button(CONCAT("Solve from current phasing##_rsr200_hwdiv_solve_", _this->name))) {
                    using Mode = dsp::combine::Phaser::Mode;
                    Mode mode = sigpath::phasing.getMode();
                    if (mode == Mode::MODE_A_ONLY || mode == Mode::MODE_B_ONLY) {
                        // Bypass: channel B (or A) isn't in the signal path at all, nothing to
                        // solve. Caught explicitly rather than falling through to
                        // hardwareWeightFor() -- it would report the exact same "not
                        // representable" a genuinely-near-zero ratio does, for a completely
                        // different reason, and the two need different messages.
                        _this->hwDivBypassed = true;
                        _this->hwDivSolved = true;
                    }
                    else {
                        _this->hwDivBypassed = false;
                        std::complex<double> k0, k1;
                        if (dsp::combine::Phaser::isDecorrelating(mode)) {
                            // Additive form already -- decorrelation solves y = k0*A + k1*B
                            // directly, and getCombineCoefficients() is only meaningful here
                            // (see its own doc comment in core/src/dsp/combine/phaser.h).
                            dsp::complex_t ck0, ck1;
                            sigpath::phasing.getCombineCoefficients(ck0, ck1);
                            k0 = std::complex<double>(ck0.re, ck0.im);
                            k1 = std::complex<double>(ck1.re, ck1.im);
                        }
                        else {
                            // MODE_MANUAL/MODE_AUTO ("Auto-null")/MODE_HOLD: a *subtractive*
                            // weight w in Y = A - w*B (Phaser::run()'s own arithmetic --
                            // outBuf = a - w*b). Found live 2026-08-20: calling
                            // getCombineCoefficients() unconditionally here (the first version
                            // of this code) silently read stale/default coefficients while
                            // Auto-null held a real, active +10.5dB/54.1deg weight, because
                            // that accessor is only ever updated by the decorrelation solver --
                            // reported "not representable" for the wrong reason (an
                            // uninitialized near-zero ratio, not the real one). Reconstruct w
                            // from getWeight()'s gain/phase exactly the way Phaser::setWeight()
                            // itself does (mag = 10^(dB/20), rad = deg*pi/180), then convert to
                            // the same additive form the decorrelation path already produces,
                            // per this file's/rsr200_protocol.h's own documented sign
                            // convention: "a subtractive weight w would need g = -w".
                            float gainDb, phaseDeg;
                            sigpath::phasing.getWeight(gainDb, phaseDeg);
                            double mag = std::pow(10.0, (double)gainDb / 20.0);
                            double rad = (double)phaseDeg * PI / 180.0;
                            std::complex<double> w(mag * std::cos(rad), mag * std::sin(rad));
                            k0 = std::complex<double>(1.0, 0.0);
                            k1 = -w;
                        }
                        HardwareWeight h = hardwareWeightFor(k0, k1);
                        _this->hwDivMagnitude = h.magnitude;
                        _this->hwDivPhaseDeg = h.phaseDegrees;
                        _this->hwDivRepresentable = h.representable;
                        _this->hwDivSuggestSwap = h.suggestSwap;
                        _this->hwDivSolved = true;
                    }
                }
                if (_this->hwDivSolved) {
                    if (_this->hwDivBypassed) {
                        SmGui::Text("Phasing is bypassed (A-only/B-only) -- nothing to combine yet.");
                    }
                    else if (_this->hwDivRepresentable) {
                        snprintf(buf, sizeof(buf), "Solved: magnitude %.3f, phase %+.1f deg",
                                 _this->hwDivMagnitude, _this->hwDivPhaseDeg);
                        SmGui::Text(buf);
                    }
                    else if (_this->hwDivSuggestSwap) {
                        SmGui::Text("Ratio needs channel swap -- toggle Swap channels and re-solve.");
                    }
                    else {
                        SmGui::Text("Not representable in the radio's own range.");
                    }
                    bool canApply = _this->hwDivRepresentable && !_this->hwDivBypassed;
                    if (!canApply) { SmGui::BeginDisabled(); }
                    if (SmGui::Button(CONCAT("Apply to hardware##_rsr200_hwdiv_apply_", _this->name))) {
                        _this->hwDivApplyConfirmOpen = true;
                    }
                    if (!canApply) { SmGui::EndDisabled(); }
                }
            }
            if (_this->hwDiversityMode) {
                snprintf(buf, sizeof(buf), "Hardware diversity active: magnitude %.3f, phase %+.1f deg",
                         _this->hwDivMagnitude, _this->hwDivPhaseDeg);
                SmGui::Text(buf);
                if (SmGui::Button(CONCAT("Back to Separate mode##_rsr200_hwdiv_back_", _this->name))) {
                    _this->hwDiversityMode = false;
                    _this->hwDivSolved = false;
                    if (_this->dualChannel) {
                        sigpath::sourceManager.registerChannels("RSR200", &_this->channels);
                    }
                    stop(_this);
                    start(_this);
                }
            }

            // Confirmation, per Ralph (2026-08-20): applying interrupts independent dual-channel
            // reception (halves the data rate, ends software phasing) even though it's
            // reversible via "Back to Separate mode" above, so it shouldn't happen from a single
            // accidental click. Drawn every frame regardless of hwDivApplyConfirmOpen --
            // GenericDialog() itself no-ops when its own `open` flag is false (core/src/gui/
            // dialogs/dialog_box.h), same pattern Frequency Manager's own delete confirmations use.
            if (ImGui::GenericDialog(("rsr200_hwdiv_apply_confirm" + _this->name).c_str(), _this->hwDivApplyConfirmOpen,
                                      GENERIC_DIALOG_BUTTONS_YES_NO, [_this]() {
                    ImGui::Text("Switching to hardware diversity mode. This interrupts independent\n"
                                "dual-channel reception (radio combines the channels itself; software\n"
                                "phasing has nothing left to work with). Continue?");
                }) == GENERIC_DIALOG_BUTTON_YES) {
                _this->hwDiversityMode = true;
                sigpath::sourceManager.unregisterChannels("RSR200");
                stop(_this);
                start(_this);
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

    // Which device this module is currently pointed at, and therefore which slot under
    // config.conf["devices"][...] its tunable settings (adcClockMHz, decimExp, etc.) are
    // stored under -- host string for LAN, matching how the other network-only sources with
    // nothing to enumerate do the same thing (rfspace_source/spyserver_source both key by
    // "host:port"). Before this refactor RSR200 kept one flat blob regardless of which
    // physical unit/host it was pointed at -- see RECORDING_SCHEDULER_PLAN.md section 2.2 for
    // the survey that motivated this and AskUserQuestion confirmation to do the full refactor
    // (2026-08-15).
    //
    // USB is keyed by the *radio's own* protocol-level serial ("radio-<N>", the number shown
    // in Status as "Serial N" once connected), not the FTDI bridge chip's USB descriptor
    // serial (selectedUsbSerial) -- real hardware testing the same day found that descriptor
    // serial reads as "1", which looks exactly like a generic default FTDI bridge chips are
    // often left with rather than something programmed uniquely per RSR200 unit. Keying by it
    // would give no real per-unit distinction, just a fixed bucket dressed up as one. The
    // radio's own serial is only known *after* a successful connect + version reply though
    // (learnUsbRadioSerial(), called from the version-reply handler in start()), so
    // selectedUsbSerial is still used as a provisional key ("usb-<serial>") for the very first
    // connect to a given FTDI device; config.conf["usbRadioSerials"][selectedUsbSerial]
    // remembers the mapping afterwards so later sessions go straight to the real key without
    // needing to reconnect first. Must be called with `config` already acquired -- every
    // existing call site (loadDeviceSettings/saveConfig/loadConfig's migration block) already
    // holds it around this call.
    std::string currentDeviceKey() {
        if (transportSel == 0) {
            if (!selectedUsbSerial.empty() && config.conf["usbRadioSerials"].contains(selectedUsbSerial)) {
                return "radio-" + config.conf["usbRadioSerials"][selectedUsbSerial].get<std::string>();
            }
            return selectedUsbSerial.empty() ? "usb-default" : ("usb-" + selectedUsbSerial);
        }
        return std::string(lanHost);
    }

    // Called from the version-reply handler in start() the first time (per connect) the
    // radio's own serial becomes known. Re-homes whatever provisional "usb-<ftdi serial>"
    // blob this connect started from onto "radio-<radio serial>", and records the FTDI<->radio
    // serial mapping for next time. The just-applied live settings are treated as
    // authoritative for the real key regardless of what (if anything) was already stored there
    // -- deliberately does not reload/replace the live in-memory fields mid-connection, to
    // avoid a surprise behavior change while the radio's already streaming with them applied.
    // No-op for LAN, and a no-op if this connect was already keyed by the radio's own serial
    // (e.g. a second Start in the same session, or a session that already had the mapping).
    void learnUsbRadioSerial(uint32_t serial) {
        if (transportSel != 0) { return; }
        std::string radioKey = "radio-" + std::to_string(serial);
        config.acquire();
        std::string oldKey = currentDeviceKey();
        if (!selectedUsbSerial.empty()) {
            config.conf["usbRadioSerials"][selectedUsbSerial] = std::to_string(serial);
        }
        if (oldKey != radioKey) {
            writeDeviceSettingsJson(config.conf["devices"][radioKey]);
            // Only ever a "usb-..." provisional key by construction (see currentDeviceKey()
            // above) -- can't be a real LAN host or a different radio's own key, so this can
            // never remove anything but its own leftover.
            if (oldKey.rfind("usb-", 0) == 0) { config.conf["devices"].erase(oldKey); }
        }
        config.release(true);
    }

    // Serializes the live, GUI-editable per-device fields into `d` -- shared by saveConfig()
    // and loadDeviceSettings()'s first-time-seeing-this-device seed path, so a freshly-seen
    // device's stored blob starts out matching whatever's currently on screen rather than
    // empty/default.
    void writeDeviceSettingsJson(json& d) {
        d["adcClockMHz"] = adcClockMHz;
        d["gpsDiscipline"] = gpsDiscipline;
        d["decimExp"] = decimExp;
        d["bits24"] = bits24;
        d["dualChannel"] = dualChannel;
        d["serialMode"] = serialMode;
        d["serialUpper"] = serialUpper;
        d["swapChannels"] = swapChannels;
        d["useVhf"] = useVhf;
        d["vhfPreamp"] = vhfPreamp;
        d["atten1"] = atten1;
        d["atten2"] = atten2;
        d["autoAttThreshold"] = autoAttThreshold;
        d["autoAttHoldTimeSec"] = autoAttHoldTimeSec;
        d["autoAttGainCh1"] = autoAttGainCh1;
        d["autoAttGainCh2"] = autoAttGainCh2;
    }

    // Reads the live per-device fields out of `d` -- shared by loadDeviceSettings() (reading
    // this module's own persisted devices.<key> blob) and applyConfig() (the SourceHandler
    // capture/apply hook, reading an externally-supplied snapshot -- see source.h and
    // RECORDING_SCHEDULER_PLAN.md section 2.2).
    void readDeviceSettingsJson(const json& d) {
        if (d.contains("adcClockMHz")) { adcClockMHz = d["adcClockMHz"]; }
        if (d.contains("gpsDiscipline")) { gpsDiscipline = d["gpsDiscipline"]; }
        if (d.contains("decimExp")) { decimExp = d["decimExp"]; }
        if (d.contains("bits24")) { bits24 = d["bits24"]; }
        if (d.contains("dualChannel")) { dualChannel = d["dualChannel"]; }
        if (d.contains("serialMode")) { serialMode = d["serialMode"]; }
        if (d.contains("serialUpper")) { serialUpper = d["serialUpper"]; }
        if (d.contains("swapChannels")) { swapChannels = d["swapChannels"]; }
        if (d.contains("useVhf")) { useVhf = d["useVhf"]; }
        if (d.contains("vhfPreamp")) { vhfPreamp = d["vhfPreamp"]; }
        if (d.contains("atten1")) { atten1 = d["atten1"]; }
        if (d.contains("atten2")) { atten2 = d["atten2"]; }
        if (d.contains("autoAttThreshold")) { autoAttThreshold = d["autoAttThreshold"]; }
        if (d.contains("autoAttHoldTimeSec")) { autoAttHoldTimeSec = d["autoAttHoldTimeSec"]; }
        if (d.contains("autoAttGainCh1")) { autoAttGainCh1 = d["autoAttGainCh1"]; }
        if (d.contains("autoAttGainCh2")) { autoAttGainCh2 = d["autoAttGainCh2"]; }
    }

    // (Re)loads the per-device fields for whatever currentDeviceKey() is *right now* into the
    // live fields. Called once at construction (after the flat fields below are loaded, so the
    // key is already known) and again at the top of start() -- mirroring rfspace_source/
    // spyserver_source, which likewise only recompute their own device key and reload at
    // connect time, not on every keystroke while the host field is being edited. A device seen
    // for the first time gets its stored blob seeded from whatever's currently live (matching
    // rtl_sdr_source's own "if devices doesn't contain this name yet, seed it" pattern at
    // rtl_sdr_source/src/main.cpp:199-209) rather than silently reading nothing.
    void loadDeviceSettings() {
        config.acquire();
        std::string key = currentDeviceKey();
        bool isNew = !config.conf["devices"].contains(key);
        json& d = config.conf["devices"][key];
        if (isNew) { writeDeviceSettingsJson(d); }
        readDeviceSettingsJson(d);
        config.release(isNew);
    }

    // SourceHandler::captureConfigHandler/applyConfigHandler (source.h) -- lets an external
    // module (the recording scheduler) capture/reproduce this radio's settings without going
    // through config.conf["devices"][...] at all, sidestepping the "re-selecting a source
    // doesn't reload from disk" problem that section applies to every module, this one
    // included (RECORDING_SCHEDULER_PLAN.md section 2.2).
    static json captureConfig(void* ctx) {
        RSR200SourceModule* _this = (RSR200SourceModule*)ctx;
        json d;
        _this->writeDeviceSettingsJson(d);
        return d;
    }

    static void applyConfig(const json& cfg, void* ctx) {
        RSR200SourceModule* _this = (RSR200SourceModule*)ctx;
        _this->readDeviceSettingsJson(cfg);
        _this->saveConfig();
        // Must be safe to call whether or not RSR200 is the currently selected source (a
        // scheduled apply may target a radio that isn't active right now) -- setInputSampleRate
        // affects whatever source *is* selected, so only touch it when that's actually this one,
        // matching every other rate-affecting control in menuHandler below.
        if (sigpath::sourceManager.getSelectedName() == "RSR200") {
            core::setInputSampleRate(_this->buildConfig().sampleRateHz());
        }
    }

    // Scans currently-connected D3XX devices and re-syncs usbDeviceId to whichever one
    // matches selectedUsbSerial (if it's still plugged in) -- called once at construction and
    // from the menu's own Refresh button. Mirrors rtl_sdr_source's own refresh()-at-construction
    // convention (main.cpp:82).
    void refreshUsbDeviceList() {
        usbDeviceList = UsbTransport::listDeviceInfo();
        usbDeviceId = 0;
        for (size_t i = 0; i < usbDeviceList.size(); i++) {
            if (usbDeviceList[i].second == selectedUsbSerial) {
                usbDeviceId = (int)i;
                break;
            }
        }
    }

    void loadConfig() {
        config.acquire();
        json& c = config.conf;
        if (c.contains("transportSel")) { transportSel = c["transportSel"]; }
        if (c.contains("lanHost")) {
            std::string h = c["lanHost"];
            strncpy(lanHost, h.c_str(), sizeof(lanHost) - 1);
            lanHost[sizeof(lanHost) - 1] = '\0';
        }
        if (c.contains("selectedUsbSerial")) { selectedUsbSerial = c["selectedUsbSerial"].get<std::string>(); }

        // One-time migration from the pre-refactor flat schema (adcClockMHz etc. sitting
        // directly on config.conf, shared across every device/host this module was ever
        // pointed at) into devices.<currentDeviceKey()> -- keyed by whatever device/host is
        // current right now, since that's the only association the old flat data ever had.
        // Only runs once: guarded on "devices" not existing yet, and erases the old flat keys
        // as it copies them so it can never re-migrate (and re-overwrite a real per-device
        // edit) on a later load.
        bool migrated = false;
        if (c.contains("adcClockMHz") && !c.contains("devices")) {
            json& d = c["devices"][currentDeviceKey()];
            static const char* legacyKeys[] = { "adcClockMHz", "gpsDiscipline", "decimExp", "bits24",
                                                  "dualChannel", "swapChannels", "useVhf", "vhfPreamp",
                                                  "atten1", "atten2" };
            for (const char* k : legacyKeys) {
                if (c.contains(k)) { d[k] = c[k]; c.erase(k); }
            }
            migrated = true;
        }
        config.release(migrated);

        refreshUsbDeviceList();
        loadDeviceSettings();
    }

    void saveConfig() {
        config.acquire();
        json& c = config.conf;
        c["transportSel"] = transportSel;
        c["lanHost"] = std::string(lanHost);
        c["selectedUsbSerial"] = selectedUsbSerial;
        writeDeviceSettingsJson(c["devices"][currentDeviceKey()]);
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
    // Serial mode ("SerL"/"SerU", RSR200_PLAN.md §6/§7, phase 7, RSR200_OM_V225.pdf's own "Use
    // of SerL and SerU" section) -- OP_SERIAL, single-channel only (mutually exclusive with
    // dualChannel; buildConfig() below gives dualChannel priority if somehow both are set).
    // serialUpper is a manual choice, per Ralph (2026-08-19) -- see buildConfig()'s own comment.
    bool serialMode = false;
    bool serialUpper = false;

    // Hardware diversity (RSR200_PLAN.md phase 7, resolved with Ralph 2026-08-20: applying
    // requires confirmation, session-only, none of this persists). Session-only, deliberately --
    // matches Carrier Zoom's own "no persisted state across closes" precedent for the same
    // reason: a solved weight is only meaningful for whatever antenna/propagation conditions
    // produced it, carrying it across a restart the way a plain setting persists would be
    // actively misleading, not a convenience. None of these fields are written to
    // writeDeviceSettingsJson()/readDeviceSettingsJson().
    bool hwDiversityMode = false;      // true once actually switched to OP_DIVERSITY
    bool hwDivSolved = false;          // a solve has been done since the last mode change
    bool hwDivBypassed = false;        // Phasing was in MODE_A_ONLY/MODE_B_ONLY at solve time
    double hwDivMagnitude = 1.0;
    double hwDivPhaseDeg = 0.0;
    bool hwDivRepresentable = false;
    bool hwDivSuggestSwap = false;
    bool hwDivApplyConfirmOpen = false;

    bool swapChannels = false;
    bool useVhf = false;
    bool vhfPreamp = false;
    int atten1 = 0;
    int atten2 = 0;

    // Auto-ATT (RSR200_PLAN.md phase 7). Resolved with Ralph 2026-08-20: starts at 0 (off)
    // until explicitly raised, rather than jumping to some working threshold the moment the
    // feature is turned on; a 200ms hold time is the initial "look and see" default (own
    // caveat: whether that's actually representable depends on the current ADC clock -- see
    // the menu's own hint text, and autoAttHoldTimeClocks()'s test in test_protocol.cpp for
    // the arithmetic); the manual attenuator is silently clamped to 19 the moment the
    // threshold goes from 0 to nonzero, not warned about first, matching how every other
    // control on this panel already auto-corrects without a prompt.
    int autoAttThreshold = 0;          // 0 = off, 1..5 = -6..-30 dB in fixed 6dB steps
    float autoAttHoldTimeSec = 0.2f;   // float, not double, to bind directly to SmGui::SliderFloat
    float autoAttGainCh1 = 6.3096f;    // calibration multiplier, DP's own nominal value
    float autoAttGainCh2 = 6.3096f;

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

    // USB device selection. selectedUsbSerial is persisted flat (like RTL-SDR's own "device"
    // key) and identifies *which FTDI bridge chip* to open -- it's the provisional
    // devices.<key> lookup key via currentDeviceKey() until the radio's own serial is learned
    // (see currentDeviceKey()/learnUsbRadioSerial() above), and always what selects which
    // physical USB device open()/openBySerial() targets, regardless of that. Empty until a
    // device's been picked at least once, in which case start() falls back to "whatever's
    // plugged in first" (UsbTransport::open(0, ...)), covering the common single-device case
    // where there's nothing to actually choose between. usbDeviceList/usbDeviceId are GUI-only
    // (not persisted) -- refreshUsbDeviceList() re-derives usbDeviceId from selectedUsbSerial
    // on every scan.
    std::string selectedUsbSerial;
    std::vector<std::pair<std::string, std::string>> usbDeviceList;
    int usbDeviceId = 0;

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
