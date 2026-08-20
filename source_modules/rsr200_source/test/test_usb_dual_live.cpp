// Diagnostic for the silent-channel-B report: dual channel ("Separate" mode, both antennas
// routed per RSR200_PLAN.md section 7) against the real radio, dumping raw bytes and
// per-channel RMS instead of assuming the documented byte layout holds on this unit's
// firmware. Not a unit test -- needs hardware. Mirrors exactly what
// RSR200SourceModule::start() sends (same command builders, same order), so whatever this
// program sees is what the real module would have gotten too.
//
// Firmware note: this radio reports firmware 549 (see test_usb_live.cpp's Version reply);
// RSR200_DP_ENG_V52.pdf was written against firmware 225. If the wire layout changed between
// those, the raw dump below is what settles it -- not another reading of the document.

#include "../src/transport_usb.h"
#include "../src/rsr200_protocol.h"
#include <cstdio>
#include <cmath>
#include <chrono>

using namespace rsr200;

static uint64_t nowMs() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

int main() {
    auto devices = UsbTransport::listDevices();
    if (devices.empty()) {
        printf("No D3XX device enumerated.\n");
        return 1;
    }
    printf("Link speed: %s\n", UsbTransport::isSuperSpeed(0) ? "SuperSpeed" : "Hi-Speed fallback");

    UsbTransport usb;
    std::string err;
    if (!usb.open(0, err)) {
        printf("open() failed: %s\n", err.c_str());
        return 1;
    }

    // Same settings RSR200SourceModule::buildConfig() uses by default: 125 MHz clock, GPS
    // discipline on, decimation exponent 3 (rate 16), 16-bit, dual channel, Separate mode,
    // ADC2 routed to HF2, no VHF/preamp, no attenuation, tuned to 10 MHz.
    const double adcClockHz = 125e6;
    const StreamFormat fmt{ 2, 16 };
    const int decimExp = 3;
    const uint16_t switchReg = SW_ADC2_TO_HF2;
    const double tuneHz = 10e6;

    uint32_t n = 1;
    auto send = [&](std::vector<uint8_t> cmd, const char* label) {
        bool ok = usb.sendCommand(cmd.data(), cmd.size());
        printf("%-28s %s\n", label, ok ? "sent" : "WRITE FAILED");
    };

    send(cmdSetAdcClock(n++, false, adcClockHz, true), "Set ADC clock");
    const uint8_t pm = portModeByte(decimExp, /*dualChannel=*/true, /*bits16=*/true, /*swap=*/false);
    const uint8_t dm = dspModeByte(OP_INDEPENDENT, /*upperSideband=*/false);
    printf("Port mode byte: 0x%02X   DSP mode byte: 0x%02X\n", pm, dm);
    send(cmdSetDataTransmission(n++, false, IFACE_USB, pm, dm), "Set data transmission");
    send(cmdSetVariable(n++, false, VAR_SWITCH, switchReg), "Set switch register");
    // OM section 6.2: the diversity magnitude/phase circuit for channel 2 sits in the signal
    // path even in Sep mode ("then set to magnitude 1.000 and phase 0.00"), and DP section 4
    // says adjustable values default to zero on power-up. Never sent before this test --
    // if magnitude defaults to 0, channel 2's real data gets multiplied by zero, which is
    // exactly the clean, exact-zero symptom measured in every prior run.
    send(cmdSetGenerator(n++, false, GEN_MAG_PHASE_CH2, packMagnitudePhase(1.0, 0.0)),
         "Set channel 2 diversity weight to unity");
    send(cmdSetVariable(n++, false, VAR_ATTENUATOR_ADC1, 0), "Set attenuator 1");
    send(cmdSetVariable(n++, false, VAR_ATTENUATOR_ADC2, 0), "Set attenuator 2");
    send(cmdSetLoBoth(n++, false, tuneHz), "Set LO (both channels)");
    send(cmdStartStream(n++, false, PORT_USB, /*sizeCode=*/15), "Start stream");   // 15 = 2ch/16bit per RSR200_PLAN.md table
    send(cmdReadVersion(n++, false), "Read version");

    const int framesPerPacket = usbSamplesPerPacket(fmt);
    printf("Format: %d channel(s), %d bit -> %d frames/packet\n", fmt.channels, fmt.bits, framesPerPacket);

    std::vector<uint8_t> frame;
    std::vector<uint8_t> peakFrame;
    std::vector<float> bufA(framesPerPacket * 2), bufB(framesPerPacket * 2);
    double sumSqA = 0.0, sumSqB = 0.0;
    int64_t sampleCount = 0;
    bool sawVersion = false;
    uint8_t lastSeenCmdNo = 0;
    int packets = 0;
    int nonzeroBPackets = 0;
    int firstNonzeroBPacket = -1;
    double maxBRms = 0.0;
    int maxBPacket = -1;
    const uint64_t t0 = nowMs();

    while (nowMs() - t0 < 5000 && packets < 3000) {
        if (!usb.nextFrame(frame)) {
            printf("nextFrame() failed after %d packets\n", packets);
            break;
        }
        if (frame.size() != USB_PACKET_BYTES) { continue; }
        packets++;

        const uint8_t cmdNo = frame[USB_CMD_NO_OFFSET];
        if (cmdNo != 0 && cmdNo != lastSeenCmdNo) {
            lastSeenCmdNo = cmdNo;
            const Reply r = parseEmbeddedCommand(frame.data() + USB_COMMAND_OFFSET);
            if (r.kind == REPLY_VERSION) {
                sawVersion = true;
                // Firmware is BCD-ish: 0x225 reads as "225", not decimal 549.
                printf("Version reply: serial=%u firmware=%X\n", r.serial, r.firmware);
            }
            else if (r.kind == REPLY_SPECIAL) {
                printf("Special confirmation: echoedInstruction=0x%02X data=[%u,%u,%u] confirmedCmd=%u\n",
                       r.echoedInstruction, r.data[0], r.data[1], r.data[2], r.confirmedCommand);
                if (r.echoedInstruction == instr::SET_DATA_TRANSMISSION) {
                    printf(">>> Set Data Transmission ack byte = %u (per DP: %s)\n", r.data[0],
                           r.data[0] == 0 ? "settings applied immediately"
                                          : "interface must be closed, reinitialized, and reconnected!");
                }
            }
            else if (r.kind == REPLY_CONFIRMATION) {
                printf("Plain confirmation, confirmedCmd=%u\n", r.confirmedCommand);
            }
        }

        if (packets == 10 || packets == 800) {
            printf("\nRaw IQ bytes, packet %d:\n  ", packets);
            for (int i = 0; i < 32; i++) { printf("%02X ", frame[USB_IQ_OFFSET + i]); }
            printf("\n  (I1 Q1 I2 Q2 16-bit interleave: bytes 0-3 channel A's first sample, 4-7 channel B's)\n\n");
        }

        const Status st = parseStatus(frame[USB_TEMP_OFFSET], frame[USB_GPS_OFFSET], frame[USB_GPS_OFFSET + 1]);
        // This standalone live probe doesn't configure Auto-ATT at all, so autoAttActive is
        // never true here -- kept as the simple single-condition form (unlike
        // rsr200_device.h's own deliver(), which also has to account for the wider
        // "threshold > 0" enabled-but-not-yet-engaged case, see RSR200_PLAN.md phase 7).
        const float gain = st.autoAttActive ? AUTO_ATT_GAIN : 1.0f;
        unpack(frame.data() + USB_IQ_OFFSET, framesPerPacket, fmt, gain, gain, bufA.data(), bufB.data());

        // Per-packet (not cumulative) RMS, so a "B turns on after N packets" transition is
        // visible instead of averaged away.
        double pktSqA = 0.0, pktSqB = 0.0;
        for (int i = 0; i < framesPerPacket; i++) {
            const double ai = bufA[2 * i], aq = bufA[2 * i + 1];
            const double bi = bufB[2 * i], bq = bufB[2 * i + 1];
            pktSqA += ai * ai + aq * aq;
            pktSqB += bi * bi + bq * bq;
        }
        sumSqA += pktSqA;
        sumSqB += pktSqB;
        sampleCount += framesPerPacket;

        const double pktRmsB = std::sqrt(pktSqB / framesPerPacket);
        if (pktRmsB > 1e-9) {
            nonzeroBPackets++;
            if (firstNonzeroBPacket < 0) { firstNonzeroBPacket = packets; }
        }
        if (pktRmsB > maxBRms) {
            maxBRms = pktRmsB;
            maxBPacket = packets;
            peakFrame = frame;
        }

        if (packets <= 60 || packets % 500 == 0) {
            const double pktRmsA = std::sqrt(pktSqA / framesPerPacket);
            printf("packet %d: this-packet RMS A=%.6f B=%.6f  cmdNo=%u temp=%dC\n",
                   packets, pktRmsA, pktRmsB, cmdNo, st.temperatureC);
        }
    }

    printf("\nPackets with any nonzero channel B content: %d / %d (first at packet %d)\n",
           nonzeroBPackets, packets, firstNonzeroBPacket);
    printf("Peak channel B RMS: %.6f at packet %d\n", maxBRms, maxBPacket);
    if (maxBPacket > 0 && !peakFrame.empty()) {
        printf("Raw IQ bytes at peak-B packet %d:\n  ", maxBPacket);
        for (int i = 0; i < 32; i++) { printf("%02X ", peakFrame[USB_IQ_OFFSET + i]); }
        printf("\n");
    }

    const double rmsA = sampleCount > 0 ? std::sqrt(sumSqA / (double)sampleCount) : 0.0;
    const double rmsB = sampleCount > 0 ? std::sqrt(sumSqB / (double)sampleCount) : 0.0;
    printf("\nFinal: %d packets, %lld samples/channel\n", packets, (long long)sampleCount);
    printf("RMS channel A (HF1): %.6f\n", rmsA);
    printf("RMS channel B (HF2): %.6f\n", rmsB);
    printf("If B is genuinely near the 16-bit noise floor while A is not, the radio isn't\n"
           "producing HF2 data at all (a config/routing problem). If B is non-trivial but the\n"
           "picture still looked flat in SDR++, the bug is more likely in how this module hands\n"
           "channel B to the stream, not in the radio or unpack().\n");

    auto stopCmd = cmdStopStream(n++, false, PORT_USB);
    usb.sendCommand(stopCmd.data(), stopCmd.size());
    usb.close();
    return 0;
}
