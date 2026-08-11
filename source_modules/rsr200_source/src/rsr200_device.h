#pragma once
#include "rsr200_protocol.h"
#include <complex>
#include <functional>
#include <string>
#include <deque>

// Transport-agnostic device layer for the RSR200. See RSR200_PLAN.md.
//
// Everything that is true of the radio regardless of how it is connected lives here:
// configuration ordering, command numbering, the acknowledgement and retry rules, frame
// parsing, and sample delivery. A transport supplies whole frames and carries command
// bytes; it knows nothing about what they mean.
//
// Written against a fake transport so the sequencing can be tested before either the radio
// or a socket exists. No SDR++ dependency.

namespace rsr200 {

    // -----------------------------------------------------------------------------
    // Transport
    //
    // Framing belongs to the transport because framing *is* the difference between the
    // interfaces: USB reads whole 4096-byte packets from an endpoint, TCP has to resync a
    // byte stream against the block trailer, and UDP has to reassemble indexed fragments.
    // Above this line those are all just "a frame arrived".
    // -----------------------------------------------------------------------------

    class Transport {
    public:
        enum Kind { KIND_USB, KIND_LAN_TCP, KIND_LAN_UDP };

        virtual ~Transport() {}
        virtual Kind kind() const = 0;

        // Command encoding is shortened on LAN and fixed-length on USB.
        bool isLan() const { return kind() != KIND_USB; }

        // The stream port to name in Start/Stop stream.
        StreamPort streamPort() const {
            switch (kind()) {
                case KIND_USB: return PORT_USB;
                case KIND_LAN_UDP: return PORT_UDP;
                default: return PORT_TCP;
            }
        }

        virtual bool sendCommand(const uint8_t* data, size_t len) = 0;

        // Blocks until a whole frame is available. False means stopped or failed.
        virtual bool nextFrame(std::vector<uint8_t>& out) = 0;

        // LAN transports need the block geometry to frame at all; USB ignores it.
        virtual void setLayout(const BlockLayout&) {}
    };

    // -----------------------------------------------------------------------------
    // What comes out
    // -----------------------------------------------------------------------------

    struct SampleBlock {
        const float* chA = nullptr;   // interleaved re, im
        const float* chB = nullptr;   // null unless dual channel
        int frames = 0;
        Status status;
        uint32_t sequence = 0;        // packet or block counter
        bool sequenceGap = false;     // counter skipped: data was lost
    };

    struct Config {
        double adcClockHz = 125e6;
        bool gpsDiscipline = true;
        int decimationExp = 3;                  // rate 16
        StreamFormat format{ 1, 16 };
        OpMode opMode = OP_PARALLEL_ADD;
        bool swapChannels = false;
        bool upperSideband = false;             // only meaningful in serial mode
        double tunedHz = 10e6;
        uint16_t switchRegister = 0;
        int attenuator1 = 0;                    // 0..35
        int attenuator2 = 0;
        bool autoAttEnabled = false;

        double sampleRateHz() const { return rsr200::sampleRateHz(adcClockHz, decimationExp); }
    };

    // -----------------------------------------------------------------------------
    // Device
    // -----------------------------------------------------------------------------

    class Device {
    public:
        // How long to wait for an acknowledgement, and how many times to try.
        static constexpr uint64_t ACK_TIMEOUT_MS = 500;
        static constexpr int MAX_ATTEMPTS = 3;

        void setTransport(Transport* t) {
            transport = t;
            if (!t) {
                // DP 3.3: Stop Stream closes the USB endpoint entirely, so whatever the
                // radio's own switch/clock/format state was carries no guarantee into the
                // *next* Start -- "the next Start has to reopen and reconfigure from scratch
                // either way" (see applyConfig()'s own comment). But configuredOnce, once set
                // true by the process's first-ever successful applyConfig(), was never being
                // cleared here -- so a same-process restart (stop, flip a setting, start
                // again) would see clockChanged/formatChanged both false (nothing the user
                // touched actually differs from last time) and skip re-sending the ADC clock
                // and data-transmission commands entirely, even though the endpoint closing
                // means the radio needs them again just as much as it did the very first time.
                //
                // Found live 2026-08-11 chasing "Use VHF input + VHF preamp doesn't activate
                // the preamp": confirmed via direct diagnostic logging that applyConfig() was
                // constructing and sending the exact correct VAR_SWITCH bytes (variable 5,
                // value 0x0082 for VHF + preamp both set) -- so the bug wasn't in *what* gets
                // sent, only in *when*: a fresh-process first start sends clock + data-
                // transmission + switch together and the preamp engages, but a same-session
                // restart (the natural way to test a checkbox that's disabled while running)
                // sends only the switch register on its own, with neither of the two commands
                // the radio apparently needs first to actually apply it to the relays.
                //
                // pending/streaming/expectSequence reset alongside it for the same reason --
                // all of it is state from a USB session the endpoint closing just ended, not
                // something the next one should start from.
                configuredOnce = false;
                pending.hasCommand = false;
                streaming = false;
                expectSequence = false;
            }
        }
        Transport* getTransport() const { return transport; }

        const Config& config() const { return cfg; }
        double sampleRate() const { return cfg.sampleRateHz(); }
        Tuning currentTuning() const { return tuning; }
        BlockLayout layout() const { return lanLayout(cfg.format); }

        std::function<void(const SampleBlock&)> onSamples;
        std::function<void(const Reply&)> onReply;
        std::function<void(const std::string&)> onError;

        // ---------------------------------------------------------------------
        // Configuration
        //
        // Order matters and is dictated by the documents:
        //  - Changing the transmission settings on LAN stops any running stream, and the
        //    restart has to name a size code matching the new format (DP 3.3).
        //  - Changing the ADC clock or the transmission settings triggers a synchronisation
        //    event, which is what puts the two channels back in phase (DP 4.6). So the
        //    clock is set before the LOs, never after.
        //
        // Tried and reverted 2026-08-11: a fixed sleep_for() between each of the commands
        // below, on the theory (DP 3.1/4.1: "wait for confirmation... before new commands
        // can be sent") that firing all five back-to-back gives the radio's firmware no real
        // processing time. Broke Start Stream outright on real hardware -- confirmed live,
        // reproducibly. Most likely cause: the RSR200 auto-starts USB streaming right after
        // power-up (DP 4.1) into a small (4096-byte double-buffered) transmit FIFO, and
        // nothing reads Transport::nextFrame() during applyConfig() at all, sleeping or not
        // -- so a sleep-based pause just gives the already-unread IN pipe more time to back
        // up before the next OUT write, rather than actually helping. A real fix would need
        // to drain frames (or otherwise service the IN pipe) during any deliberate pause
        // here, not just wait -- do that in a future pass rather than reintroducing a bare
        // sleep. See RSR200_PLAN.md's live-testing entry for the full account (bit-for-bit
        // correct VAR_SWITCH content confirmed via diagnostic, confirmed against the radio's
        // own front-panel indicator that the preamp still doesn't engage either way).
        // ---------------------------------------------------------------------

        bool applyConfig(const Config& next, uint64_t nowMs) {
            if (!transport) { return fail("no transport"); }

            const bool formatChanged = !(next.format == cfg.format)
                                    || next.decimationExp != cfg.decimationExp
                                    || next.opMode != cfg.opMode
                                    || next.swapChannels != cfg.swapChannels
                                    || next.upperSideband != cfg.upperSideband;
            const bool clockChanged = next.adcClockHz != cfg.adcClockHz
                                   || next.gpsDiscipline != cfg.gpsDiscipline;

            if (streaming && (formatChanged || clockChanged)) {
                if (!stopStream(nowMs)) { return false; }
            }

            cfg = next;

            if (clockChanged || !configuredOnce) {
                if (!send(cmdSetAdcClock(nextNumber(), transport->isLan(),
                                         cfg.adcClockHz, cfg.gpsDiscipline), nowMs, true)) {
                    return false;
                }
            }

            if (formatChanged || !configuredOnce) {
                const uint8_t pm = portModeByte(cfg.decimationExp, cfg.format.channels == 2,
                                                cfg.format.bits == 16, cfg.swapChannels);
                const uint8_t dm = dspModeByte(cfg.opMode, cfg.upperSideband);
                const Interface iface = transport->isLan() ? IFACE_LAN : IFACE_USB;
                if (!send(cmdSetDataTransmission(nextNumber(), transport->isLan(), iface, pm, dm),
                          nowMs, true)) {
                    return false;
                }
            }

            if (!send(cmdSetVariable(nextNumber(), transport->isLan(), VAR_SWITCH, cfg.switchRegister),
                      nowMs, true)) { return false; }
            if (!send(cmdSetVariable(nextNumber(), transport->isLan(), VAR_ATTENUATOR_ADC1,
                                     (uint16_t)std::clamp(cfg.attenuator1, 0, 35)), nowMs, true)) { return false; }
            if (!send(cmdSetVariable(nextNumber(), transport->isLan(), VAR_ATTENUATOR_ADC2,
                                     (uint16_t)std::clamp(cfg.attenuator2, 0, 35)), nowMs, true)) { return false; }

            // Tuning last, so it follows the synchronisation event rather than preceding it.
            if (!tune(cfg.tunedHz, nowMs)) { return false; }

            configuredOnce = true;
            transport->setLayout(layout());
            return true;
        }

        // Always both oscillators in one command. DP 4.6: tuning them separately and later
        // returning them to a common frequency leaves the phase relationship undefined
        // until the next synchronisation event, which silently ruins phasing.
        bool tune(double rfHz, uint64_t nowMs) {
            if (!transport) { return fail("no transport"); }
            cfg.tunedHz = rfHz;
            tuning = tuneFor(rfHz, cfg.adcClockHz);
            return send(cmdSetLoBoth(nextNumber(), transport->isLan(), tuning.loHz), nowMs, true);
        }

        // The hardware diversity weight, for users who would rather the radio combined the
        // channels than send both. Not usable as an adaptive control: the round trip
        // through the command channel is far too slow for a loop.
        bool setHardwareDiversity(double magnitude, double phaseDegrees, uint64_t nowMs) {
            if (!transport) { return fail("no transport"); }
            return send(cmdSetGenerator(nextNumber(), transport->isLan(), GEN_MAG_PHASE_CH2,
                                        packMagnitudePhase(magnitude, phaseDegrees)), nowMs, true);
        }

        // Hand a combination worked out in software to the radio's own combiner. The
        // caller is expected to be in Separate mode while solving and to switch to
        // OP_DIVERSITY afterwards; this only carries the weight across.
        //
        // Returns false without sending anything when the ratio is outside the radio's
        // range, in which case `out` says whether swapping the channels would fix it.
        bool setHardwareDiversityFrom(std::complex<double> k0, std::complex<double> k1,
                                      uint64_t nowMs, HardwareWeight* out = nullptr) {
            const HardwareWeight h = hardwareWeightFor(k0, k1);
            if (out) { *out = h; }
            if (!h.representable) { return false; }
            return setHardwareDiversity(h.magnitude, h.phaseDegrees, nowMs);
        }

        bool startStream(uint64_t nowMs) {
            if (!transport) { return fail("no transport"); }
            transport->setLayout(layout());
            expectSequence = false;
            // Start and Stop are not acknowledged (DP 3.3), so nothing to wait for.
            if (!send(cmdStartStream(nextNumber(), transport->isLan(), transport->streamPort(),
                                     layout().startStreamSizeCode), nowMs, false)) {
                return false;
            }
            streaming = true;
            return true;
        }

        bool stopStream(uint64_t nowMs) {
            if (!transport) { return fail("no transport"); }
            if (!send(cmdStopStream(nextNumber(), transport->isLan(), transport->streamPort()),
                      nowMs, false)) {
                return false;
            }
            streaming = false;
            return true;
        }

        bool isStreaming() const { return streaming; }

        // ---------------------------------------------------------------------
        // Running
        // ---------------------------------------------------------------------

        // Take one frame from the transport and deliver whatever it holds. False when the
        // transport has stopped.
        bool pump() {
            if (!transport) { return false; }
            if (!transport->nextFrame(frameBuf)) { return false; }
            if (transport->kind() == Transport::KIND_USB) { parseUsbPacket(frameBuf); }
            else { parseLanBlock(frameBuf); }
            return true;
        }

        // Re-issue anything that has gone unacknowledged. Call periodically.
        //
        // DP 3.5 documents a repeat counter for this, then says firmware 22x ignores it and
        // simply executes the command again. So a retry goes out under a *fresh* number and
        // the caller must tolerate the original having landed as well -- bumping the repeat
        // field would achieve nothing.
        void service(uint64_t nowMs) {
            if (!pending.hasCommand) { return; }
            if (nowMs - pending.sentAtMs < ACK_TIMEOUT_MS) { return; }

            if (pending.attempts >= MAX_ATTEMPTS) {
                pending.hasCommand = false;
                fail("no acknowledgement for command " + std::to_string((int)pending.instruction));
                return;
            }

            pending.attempts++;
            pending.number = nextNumber();
            writeU32(pending.bytes.data(), pending.number);
            pending.sentAtMs = nowMs;
            transport->sendCommand(pending.bytes.data(), pending.bytes.size());
        }

        bool awaitingAck() const { return pending.hasCommand; }
        uint32_t pendingNumber() const { return pending.number; }
        int pendingAttempts() const { return pending.attempts; }
        uint32_t lastCommandNumber() const { return commandCounter; }

    private:
        bool fail(const std::string& msg) {
            if (onError) { onError(msg); }
            return false;
        }

        // Never 0: the radio reserves 0 to mark commands it generated itself, so a PC
        // command numbered 0 would be indistinguishable from one.
        uint32_t nextNumber() {
            if (++commandCounter == 0) { commandCounter = 1; }
            return commandCounter;
        }

        bool send(const std::vector<uint8_t>& bytes, uint64_t nowMs, bool expectAck) {
            if (!transport->sendCommand(bytes.data(), bytes.size())) {
                return fail("transport rejected a command");
            }
            if (expectAck) {
                pending.hasCommand = true;
                pending.number = readU32(bytes.data());
                pending.instruction = bytes[4];
                pending.bytes = bytes;
                pending.sentAtMs = nowMs;
                pending.attempts = 0;
            }
            return true;
        }

        void noteSequence(uint32_t counter) {
            lastBlock.sequenceGap = expectSequence && (counter != lastSequence + 1);
            lastSequence = counter;
            expectSequence = true;
            lastBlock.sequence = counter;
        }

        void deliver(const uint8_t* iq, int frames) {
            const float gain = lastBlock.status.autoAttActive ? AUTO_ATT_GAIN : 1.0f;
            const size_t need = (size_t)frames * 2;
            if (bufA.size() < need) { bufA.resize(need); }
            if (cfg.format.channels == 2 && bufB.size() < need) { bufB.resize(need); }

            unpack(iq, frames, cfg.format, gain, bufA.data(),
                   cfg.format.channels == 2 ? bufB.data() : nullptr);

            // tuneFor() already works out, per DP's own Nyquist-zone arithmetic, exactly
            // when the current tuning lands in an even zone and therefore comes off the ADC
            // mirrored -- tuning.spectrumInverted was computed right there every time tune()
            // runs, and nothing downstream ever looked at it. Found live 2026-08-11: Ralph
            // noticed VHF reception reading at the wrong frequency (RDS-confirmed) and the
            // waterfall panning backwards relative to HF, diagnosed it as a mirrored
            // spectrum, and confirmed manually enabling SDR++'s own "Invert IQ" option fixed
            // it -- correct as an explanation, but wrong as a place to leave the fix: which
            // zone a tuned frequency falls in depends on the exact frequency and the ADC
            // clock rate, not just "HF vs VHF" (at a 125 MHz clock, for instance, VHF's own
            // 66-150 MHz range straddles two different zones with opposite parity), so a
            // single manual checkbox can't stay correct across a whole retune the way this
            // per-block check does. Negating the Q sample of every complex pair conjugates
            // the signal, which is the standard correction for a mirrored spectrum -- same
            // effect as the "Invert IQ" option this makes redundant, just applied
            // automatically and only when the current tuning actually needs it.
            if (tuning.spectrumInverted) {
                for (size_t i = 1; i < need; i += 2) { bufA[i] = -bufA[i]; }
                if (cfg.format.channels == 2) {
                    for (size_t i = 1; i < need; i += 2) { bufB[i] = -bufB[i]; }
                }
            }

            lastBlock.chA = bufA.data();
            lastBlock.chB = (cfg.format.channels == 2) ? bufB.data() : nullptr;
            lastBlock.frames = frames;
            if (onSamples) { onSamples(lastBlock); }
        }

        // A command is only new when the command number changes; the same data repeats in
        // every frame until the radio writes something else (DP 3.1).
        void handleCommandNumber(uint8_t number, const uint8_t* commands, int count) {
            if (number == 0 || number == lastCommandNo) { return; }
            lastCommandNo = number;
            for (int i = 0; i < count; i++) {
                const Reply r = parseEmbeddedCommand(commands + (size_t)i * 8);
                // Only a reply carrying our own number clears a pending command; a
                // self-generated report (number 0) must not.
                if (pending.hasCommand && !r.selfGenerated && r.confirmedCommand == pending.number) {
                    pending.hasCommand = false;
                }
                if (onReply) { onReply(r); }
            }
        }

        void parseUsbPacket(const std::vector<uint8_t>& p) {
            if (p.size() < USB_PACKET_BYTES) { return; }
            lastBlock.status = parseStatus(p[USB_TEMP_OFFSET], p[USB_GPS_OFFSET], p[USB_GPS_OFFSET + 1]);
            noteSequence(readU32(p.data()));
            handleCommandNumber(p[USB_CMD_NO_OFFSET], p.data() + USB_COMMAND_OFFSET, 1);
            deliver(p.data() + USB_IQ_OFFSET, usbSamplesPerPacket(cfg.format));
        }

        void parseLanBlock(const std::vector<uint8_t>& b) {
            const BlockLayout l = layout();
            if (b.size() < l.blockBytes) { return; }
            if (!blockTrailerValid(b.data(), l)) {
                fail("LAN block failed its sync check");
                return;
            }

            lastBlock.status = parseStatus(b[l.tempOffset], b[l.gpsOffset], b[l.gpsOffset + 1]);
            noteSequence(readU32(b.data() + l.counterOffset));

            // Embedded replies are assumed to be 8 bytes each, matching both confirmation
            // forms. The document says only that several may be present and gives their
            // count; see RSR200_PLAN.md for why this wants confirming on hardware.
            uint32_t count = readU32(b.data() + l.cmdCountOffset);
            count = std::min<uint32_t>(count, (uint32_t)(l.commandSpace / 8));
            handleCommandNumber(b[l.cmdNoOffset], b.data() + l.commandsOffset, (int)count);

            deliver(b.data(), l.samplesPerChannel);
        }

        struct PendingCommand {
            bool hasCommand = false;
            uint32_t number = 0;
            uint8_t instruction = 0;
            std::vector<uint8_t> bytes;
            uint64_t sentAtMs = 0;
            int attempts = 0;
        };

        Transport* transport = nullptr;
        Config cfg;
        Tuning tuning;
        PendingCommand pending;

        uint32_t commandCounter = 0;
        uint8_t lastCommandNo = 0;
        uint32_t lastSequence = 0;
        bool expectSequence = false;
        bool streaming = false;
        bool configuredOnce = false;

        std::vector<uint8_t> frameBuf;
        std::vector<float> bufA, bufB;
        SampleBlock lastBlock;
    };
}
