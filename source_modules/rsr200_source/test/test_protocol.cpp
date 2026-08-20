// Checks for the RSR200 wire protocol, against the byte layouts and worked examples in
// RSR200_DP_ENG_V52.pdf and RSR200_OM_V225.pdf.
//
//   c++ -std=c++17 -O2 -o /tmp/t test/test_protocol.cpp && /tmp/t
//
// No hardware and no SDR++ dependency. Every number here is quoted from the documents, so
// a failure means either the header is wrong or the document was misread -- which is the
// whole point of writing this before the radio arrives.

#include "../src/rsr200_protocol.h"
#include <cstdio>
#include <string>
#include <vector>
#include <complex>

using namespace rsr200;

static int failures = 0;

static void check(bool cond, const std::string& what) {
    printf("  %-62s %s\n", what.c_str(), cond ? "ok" : "FAIL");
    if (!cond) { failures++; }
}

int main() {
    printf("\nRSR200 wire protocol\n\n");

    // -----------------------------------------------------------------
    printf("LAN block geometry matches the documented lengths\n");
    {
        const BlockLayout a = lanLayout({ 1, 16 });
        const BlockLayout b = lanLayout({ 2, 16 });
        const BlockLayout c = lanLayout({ 1, 24 });
        const BlockLayout d = lanLayout({ 2, 24 });

        check(a.blockBytes == 522704, "1 channel 16 bit block is 522704 bytes");
        check(b.blockBytes == 1045408, "2 channel 16 bit block is 1045408 bytes");
        check(c.blockBytes == 784784, "1 channel 24 bit block is 784784 bytes");
        check(d.blockBytes == 784784, "2 channel 24 bit block is 784784 bytes");

        // DP 2.2.1.4: "At 24 bit, block lengths for both 1 and 2-channel mode are the same.
        // Unlike all other formats, 24 bit 2-channel mode therefore transmits only half
        // the usual number of samples per block."
        check(a.samplesPerChannel == 130560, "1x16 carries 130560 samples");
        check(b.samplesPerChannel == 130560, "2x16 carries 130560 samples per channel");
        check(c.samplesPerChannel == 130560, "1x24 carries 130560 samples");
        check(d.samplesPerChannel == 65280, "2x24 carries only 65280 samples per channel");

        // Trailer offsets, read straight off the tables.
        check(a.counterOffset == 522240 && a.commandsOffset == 522264, "1x16 trailer at 522240, commands at 522264");
        check(b.counterOffset == 1044480 && b.commandsOffset == 1044504, "2x16 trailer at 1044480, commands at 1044504");
        check(c.counterOffset == 783360 && c.commandsOffset == 783384, "1x24 trailer at 783360, commands at 783384");
        check(d.counterOffset == 783360, "2x24 trailer at 783360");

        check(a.startStreamSizeCode == 7, "1x16 start-stream size code is 7");
        check(b.startStreamSizeCode == 15, "2x16 start-stream size code is 15");
        check(d.startStreamSizeCode == 5, "2x24 start-stream size code is 5");
    }

    // -----------------------------------------------------------------
    printf("\nBlock lengths divide evenly into UDP packets\n");
    {
        // DP 2.2.2.1 gives 359 packets for the 1x16 block. That every block length is an
        // exact multiple of the 1456-byte payload is clearly deliberate, and worth pinning
        // down: a remainder would mean a short final packet to handle.
        for (StreamFormat f : { StreamFormat{1,16}, StreamFormat{2,16}, StreamFormat{1,24}, StreamFormat{2,24} }) {
            const BlockLayout l = lanLayout(f);
            char buf[96];
            snprintf(buf, sizeof(buf), "%d channel %d bit divides into %d whole packets", f.channels, f.bits, l.udpPackets);
            check(l.blockBytes % UDP_PAYLOAD_BYTES == 0, buf);
        }
        check(lanLayout({ 1, 16 }).udpPackets == 359, "1x16 block is 359 UDP packets, as documented");
    }

    // -----------------------------------------------------------------
    printf("\nUSB packet geometry\n");
    {
        // DP 2.1.1 - 2.1.4 give these sample counts explicitly.
        check(usbSamplesPerPacket({ 1, 16 }) == 1020, "1x16: 1020 samples per 4096 byte packet");
        check(usbSamplesPerPacket({ 2, 16 }) == 510, "2x16: 510 samples per packet");
        check(usbSamplesPerPacket({ 1, 24 }) == 680, "1x24: 680 samples per packet");
        check(usbSamplesPerPacket({ 2, 24 }) == 340, "2x24: 340 samples per packet");
    }

    // -----------------------------------------------------------------
    printf("\nStatus header\n");
    {
        Status s = parseStatus(25, 0x00, 0x00);
        check(s.temperatureC == 25 && !s.autoAttActive, "an ordinary temperature reads through");

        // DP 3.2: "the reading -128 C (0x80) is reserved to indicate that the attenuator is
        // active. This value must not be used as a temperature reading."
        s = parseStatus(0x80, 0x00, 0x00);
        check(s.autoAttActive, "0x80 is the Auto-ATT flag, not a temperature");

        s = parseStatus(0, 0x00, 0xC0);
        check(s.overloadCh1 && s.overloadCh2, "overload bits are 6 and 7 of the GPS high byte");

        // Signed 14-bit, so 0x3FFF is -1.
        s = parseStatus(0, 0xFF, 0x3F);
        check(s.freqCorrectionRaw == -1, "frequency correction is a signed 14 bit value");
        s = parseStatus(0, 0x64, 0x00);
        check(s.freqCorrectionRaw == 100, "positive corrections read correctly");
        check(freqCorrectionHz(s, true) == 50.0, "0.5 Hz per LSB while disciplining");
        check(freqCorrectionHz(s, false) == 10.0, "0.1 Hz per LSB while only measuring");

        // DP 3.2: "If GPS reception is not possible, the highest possible negative value is
        // output (0x2000)."
        s = parseStatus(0, 0x00, 0x20);
        check(!s.freqCorrectionValid, "0x2000 means no valid measurement");

        // Overload bits must not corrupt the correction value.
        s = parseStatus(0, 0x64, 0xC0);
        check(s.freqCorrectionRaw == 100 && s.overloadCh1 && s.overloadCh2,
              "overload bits are masked out of the correction");
    }

    // -----------------------------------------------------------------
    printf("\nSample unpacking\n");
    {
        // 16 bit, single channel: full scale positive and negative.
        const uint8_t iq16[] = { 0x00, 0x40, 0x00, 0xC0 };      // +16384, -16384
        float a[4] = { 0 }, b[4] = { 0 };
        unpack(iq16, 1, { 1, 16 }, 1.0f, 1.0f, a, b);
        check(std::abs(a[0] - 0.5f) < 1e-6f && std::abs(a[1] + 0.5f) < 1e-6f,
              "16 bit scales to +/-1.0 at full scale");

        // 24 bit sign extension across the three-byte boundary.
        const uint8_t iq24[] = { 0x00, 0x00, 0x40, 0x00, 0x00, 0xC0 };
        unpack(iq24, 1, { 1, 24 }, 1.0f, 1.0f, a, b);
        check(std::abs(a[0] - 0.5f) < 1e-6f && std::abs(a[1] + 0.5f) < 1e-6f,
              "24 bit sign extends and scales correctly");

        // Dual channel interleave is I1 Q1 I2 Q2, and must not get crossed.
        const uint8_t dual16[] = { 0x00, 0x40, 0x00, 0x20, 0x00, 0xC0, 0x00, 0xE0 };
        unpack(dual16, 1, { 2, 16 }, 1.0f, 1.0f, a, b);
        check(std::abs(a[0] - 0.5f) < 1e-6f && std::abs(a[1] - 0.25f) < 1e-6f, "channel 1 lands in A");
        check(std::abs(b[0] + 0.5f) < 1e-6f && std::abs(b[1] + 0.25f) < 1e-6f, "channel 2 lands in B");

        // DP 4.7: enabling Auto-ATT scales the stream down 2 bits, so it has to be scaled
        // back up or every level downstream is 12 dB wrong.
        unpack(iq16, 1, { 1, 16 }, AUTO_ATT_GAIN, AUTO_ATT_GAIN, a, b);
        check(std::abs(a[0] - 2.0f) < 1e-5f, "Auto-ATT compensation is a factor of 4");

        // Auto-ATT's per-channel calibration gain is genuinely different per channel (device
        // tolerances, DP: "calibrate!") -- gainA and gainB must be applied independently, not
        // averaged or cross-applied, or a channel's own calibration would leak into the other.
        unpack(dual16, 1, { 2, 16 }, 2.0f, 3.0f, a, b);
        check(std::abs(a[0] - 1.0f) < 1e-6f && std::abs(b[0] + 1.5f) < 1e-6f,
              "channel A and channel B take their own, independent gain");
    }

    // -----------------------------------------------------------------
    printf("\nBlock resynchronisation\n");
    {
        const BlockLayout l = lanLayout({ 1, 16 });
        // A partial block, then a complete one, as a receiver would see mid-stream.
        const size_t junk = 1234;
        std::vector<uint8_t> stream(junk + l.blockBytes, 0xAA);
        uint8_t* blk = stream.data() + junk;
        writeU32(blk + l.counterOffset, 0x12345678);
        writeU32(blk + l.invCounterOffset, ~0x12345678u);
        memcpy(blk + l.syncOffset, SYNC_BYTES, sizeof(SYNC_BYTES));

        check(findBlockStart(stream.data(), stream.size(), l) == (ptrdiff_t)junk,
              "finds a block boundary in a stream that starts mid-block");
        check(blockTrailerValid(blk, l), "a well-formed trailer validates");

        // A corrupted counter must be rejected even though the sync words are intact --
        // that pairing is what makes a false positive unlikely.
        writeU32(blk + l.invCounterOffset, 0);
        check(!blockTrailerValid(blk, l), "counter and its inverse must agree");
        check(findBlockStart(stream.data(), stream.size(), l) == -1, "no false positive on a bad counter");
    }

    // -----------------------------------------------------------------
    printf("\nCommand construction\n");
    {
        // Lengths, from the tables in DP 3.3.
        check(cmdReset(1, false).size() == 8 && cmdReset(1, true).size() == 8, "Reset is 8 bytes both ways");
        check(cmdReadVersion(1, false).size() == 8 && cmdReadVersion(1, true).size() == 6, "Read version 8 / 6");
        check(cmdStartStream(1, false, PORT_TCP, 7).size() == 8 && cmdStartStream(1, true, PORT_TCP, 7).size() == 7,
              "Start stream 8 / 7");
        check(cmdSetGenerator(1, false, GEN_LO_BOTH, 0).size() == 12 && cmdSetGenerator(1, true, GEN_LO_BOTH, 0).size() == 11,
              "Set generators 12 / 11");
        check(cmdSetVariable(1, false, VAR_SWITCH, 0).size() == 12 && cmdSetVariable(1, true, VAR_SWITCH, 0).size() == 9,
              "Set variable 12 / 9");
        check(cmdSetDataTransmission(1, false, IFACE_LAN, 0, 0).size() == 12 &&
              cmdSetDataTransmission(1, true, IFACE_LAN, 0, 0).size() == 9, "Set data transmission 12 / 9");
        check(cmdSetAutoAttenuator(1, false, 0, 0, 0, 0).size() == 16 &&
              cmdSetAutoAttenuator(1, true, 0, 0, 0, 0).size() == 14, "Set auto attenuator 16 / 14");

        // Layout: 32 bit command number, then the instruction.
        auto c = cmdStartStream(0xDEADBEEF, true, PORT_TCP, 15);
        check(readU32(c.data()) == 0xDEADBEEF, "command number occupies bytes 0-3");
        check(c[4] == instr::START_STREAM && c[5] == PORT_TCP && c[6] == 15, "instruction and parameters follow");

        // ADC clock is in 0.1 MHz units with GPS discipline in bit 7 of the high byte.
        c = cmdSetAdcClock(1, false, 125000000.0, true);
        check(c[5] == (uint8_t)(1250 & 0xFF) && c[6] == (uint8_t)(1250 >> 8), "125 MHz encodes as 1250 units");
        c = cmdSetAdcClock(1, false, 125000000.0, false);
        check((c[6] & 0x80) != 0, "GPS-Dis is bit 7 of the high byte");
        c = cmdSetAdcClock(1, false, 300000000.0, true);
        check(((int)c[5] | (((int)c[6] & 0x7F) << 8)) == 2000, "clock clamps to the 200 MHz maximum");

        // 32-bit LO, little endian.
        c = cmdSetLoBoth(7, true, 10000000.0);
        check(c[5] == GEN_LO_BOTH, "tuning both channels uses selector 2");
        check(readU32(c.data() + 6) == 10000000u, "LO is a 32 bit little endian value in Hz");
    }

    // -----------------------------------------------------------------
    printf("\nPort and DSP mode bytes\n");
    {
        // Decimation exponent in bits 0-2, rate = 2^(exp+1).
        check(decimationRate(0) == 2 && decimationRate(5) == 64, "decimation spans 2 to 64");
        check(decimationExponentFor(16) == 3, "a rate of 16 is exponent 3");

        check(portModeByte(3, false, false, false) == 0x03, "single channel 24 bit, decimation 16");
        check((portModeByte(0, true, false, false) & (1 << 4)) != 0, "bit 4 selects dual channel");
        check((portModeByte(0, false, true, false) & (1 << 5)) != 0, "bit 5 selects 16 bit");
        check((portModeByte(0, false, false, true) & (1 << 3)) != 0, "bit 3 swaps the channels");

        check(dspModeByte(OP_INDEPENDENT, false) == 0, "Separate is operating mode 0");
        check(dspModeByte(OP_DIVERSITY, false) == 3, "Diversity is operating mode 3");
        check((dspModeByte(OP_SERIAL, true) & (1 << 3)) != 0, "bit 3 picks the upper sideband");
    }

    // -----------------------------------------------------------------
    printf("\nHardware diversity weight packing\n");
    {
        // DP 3.3: magnitude 1 LSB = 1/8192; phase 0x8000 = -180, 0x7FFF = +180 - 1 LSB.
        uint32_t v = packMagnitudePhase(1.0, 0.0);
        check((v & 0xFFFF) == 8192, "unity magnitude is 8192");
        check((v >> 16) == 0, "zero phase is zero");
        v = packMagnitudePhase(1.0, 180.0);
        check((int16_t)(v >> 16) == 32767, "+180 degrees saturates at 0x7FFF");
        v = packMagnitudePhase(1.0, -180.0);
        check((int16_t)(v >> 16) == -32768, "-180 degrees is 0x8000");
    }

    // -----------------------------------------------------------------
    printf("\nAuto-ATT command encoding and unit conversions\n");
    {
        // DP's own worked value: nominal 16dB attenuation = 6.3096x = raw set value 6461.
        check(autoAttGainLsb(6.3096) == 6461, "6.3096x is the DP's own worked value, 6461 raw LSBs");
        check(autoAttGainLsb(0.0) == 0, "zero multiplier is zero LSBs");
        check(autoAttGainLsb(1000.0) == 65535, "clamps to the field's 16 bit range rather than wrapping");

        // "0 ... 0xFFFFFF = 1 ... 2^24 ADC CLK" -- a plain seconds*Hz conversion, clamped to
        // the field's 24 bits, and (per DP's own caution) meant to be re-derived from the
        // *current* ADC clock every time either changes, not carried as a fixed raw count.
        check(autoAttHoldTimeClocks(0.05, 125e6) == 6250000, "0.05s at 125 MHz is 6,250,000 clocks");
        check(autoAttHoldTimeClocks(0.0, 125e6) == 0, "zero hold time is zero clocks");

        // A real, easy-to-miss consequence of the field only being 24 bits: found while
        // writing this test, not something already known when the UI's default was chosen.
        // 0xFFFFFF clocks caps out at barely over 134ms at a 125 MHz ADC clock (2^24-1 /
        // 125e6), and less still at higher clock rates -- so a "look and see" hold-time
        // default in the low hundreds of milliseconds, which reads as perfectly reasonable
        // next to the field's *seconds* framing, can silently be requesting more than the
        // wire format can actually carry. main.cpp's UI has to show the *achieved* hold time
        // (this same conversion, then back to seconds) rather than just echoing back whatever
        // was typed in, or a clamp here becomes invisible to whoever set it.
        check(autoAttHoldTimeClocks(1.0, 125e6) == 0xFFFFFF,
              "a full second at 125 MHz overflows the 24 bit field and clamps rather than wraps");
        check(autoAttHoldTimeClocks(0.2, 125e6) == 0xFFFFFF,
              "even 200ms overflows the field at a 125 MHz clock -- see the comment above");

        // Field layout, DP's own "Set automatic attenuator" table: threshold (byte 5), hold
        // time (24 bit LE, bytes 6-8), channel 1 gain (16 bit LE, bytes 9-10), channel 2 gain
        // (16 bit LE, bytes 11-12), repeat (byte 13).
        auto c = cmdSetAutoAttenuator(1, false, 3, 0x123456, 6461, 500, 9);
        check(c[5] == 3, "threshold occupies byte 5");
        check(c[6] == 0x56 && c[7] == 0x34 && c[8] == 0x12,
              "hold time is a 24 bit little endian value in ADC clock cycles");
        check(c[9] == (uint8_t)(6461 & 0xFF) && c[10] == (uint8_t)(6461 >> 8),
              "channel 1 gain is a 16 bit little endian value");
        check(c[11] == (uint8_t)(500 & 0xFF) && c[12] == (uint8_t)(500 >> 8),
              "channel 2 gain is its own, independent 16 bit little endian value");
        check(c[13] == 9, "repeat counter occupies byte 13");
    }

    // -----------------------------------------------------------------
    printf("\nNyquist zone mapping\n");
    {
        // OM 5.1 example 1: at a 125 MHz clock, "Frequencies around 125 MHz +/- 30 MHz are
        // mapped to the 0 - 30 MHz range", and 95 MHz appears at 30 MHz.
        Tuning t = tuneFor(10e6, 125e6);
        check(t.zone == 1 && std::abs(t.loHz - 10e6) < 1.0 && !t.spectrumInverted,
              "10 MHz at a 125 MHz clock is zone 1, straight through");

        t = tuneFor(95e6, 125e6);
        check(t.zone == 2 && std::abs(t.loHz - 30e6) < 1.0, "95 MHz maps to 30 MHz baseband");
        check(t.spectrumInverted, "zone 2 arrives with its spectrum reversed");

        // OM 5.1 example 2: 155 MHz is in the 3rd zone and collides with 95 MHz reception,
        // which only works if both land on the same baseband frequency.
        t = tuneFor(155e6, 125e6);
        check(t.zone == 3 && std::abs(t.loHz - 30e6) < 1.0, "155 MHz also maps to 30 MHz");
        check(!t.spectrumInverted, "zone 3 is the right way up again");

        // OM 5.1 example 3: with a 166 MHz clock the 3rd zone is 166 - 249 MHz, chosen to
        // cover the 174 - 240 MHz DAB band.
        t = tuneFor(174e6, 166e6);
        check(t.zone == 3, "174 MHz sits in zone 3 at a 166 MHz clock");
        t = tuneFor(240e6, 166e6);
        check(t.zone == 3, "240 MHz is still in zone 3, so the band fits");

        check(std::abs(sampleRateHz(70e6, 5) - 1093750.0) < 1.0, "70 MHz over 64 is 1.09375 MSp/s");
        check(std::abs(sampleRateHz(200e6, 0) - 100e6) < 1.0, "200 MHz over 2 is 100 MSp/s");
    }

    // -----------------------------------------------------------------
    printf("\nBCD decoding\n");
    {
        check(bcdToDecimal(0x0225) == 225, "0x0225 BCD-decodes to 225, not 549");
        check(bcdToDecimal(0) == 0, "zero decodes to zero");
        check(bcdToDecimal(0x9999) == 9999, "all-nines round-trips");
        check(bcdToDecimal(0x0007) == 7, "single low digit, no leading-zero artefacts");
    }

    // -----------------------------------------------------------------
    printf("\nReply parsing\n");
    {
        // Plain confirmation: four zero bytes then the command number being confirmed.
        uint8_t conf[8] = { 0, 0, 0, 0, 0x2A, 0x00, 0x00, 0x00 };
        Reply r = parseEmbeddedCommand(conf);
        check(r.kind == REPLY_CONFIRMATION && r.confirmedCommand == 42, "plain confirmation parses");

        // Special confirmation echoes the instruction and carries three data bytes.
        uint8_t spec[8] = { instr::SET_ADC_CLOCK, 0xE2, 0x04, 0x00, 0x07, 0x00, 0x00, 0x00 };
        r = parseEmbeddedCommand(spec);
        check(r.kind == REPLY_SPECIAL && r.echoedInstruction == instr::SET_ADC_CLOCK, "special confirmation parses");
        check(r.data[0] == 0xE2 && r.data[1] == 0x04 && r.confirmedCommand == 7, "its feedback data comes through");

        // DP 3.2: "In self-generated commands, the RSR200 always sends number 0 to the PC."
        uint8_t self[8] = { instr::SET_ADC_CLOCK, 0xD0, 0x04, 0x00, 0, 0, 0, 0 };
        r = parseEmbeddedCommand(self);
        check(r.selfGenerated, "command number 0 marks a self-generated report");

        // DP 3.2/3.3 describe the firmware field as a "4 digit hexadecimal value" -- packed
        // BCD, one decimal digit per nibble -- not a plain binary count. 0x0225 on the wire
        // (bytes 0x25, 0x02, 0x00, 0x00 little-endian) is firmware "225" once BCD-decoded;
        // it is not the binary value 0x0225 = 549 decimal. See bcdToDecimal()'s own comment.
        uint8_t ver[8] = { instr::READ_VERSION, 0x34, 0x12, 0x00, 0x25, 0x02, 0x00, 0x00 };
        r = parseEmbeddedCommand(ver);
        check(r.kind == REPLY_VERSION && r.serial == 0x1234 && r.firmware == 225, "USB version report parses (BCD-decoded)");

        // The 12-byte standalone LAN version packet, which is the only reply sent outside
        // the stream.
        uint8_t lanver[12] = { 12, 0, 0, 0, instr::READ_VERSION, 0x34, 0x12, 0x00, 0x25, 0x02, 0x00, 0x00 };
        check(parseLanVersionPacket(lanver, sizeof(lanver), r) && r.serial == 0x1234 && r.firmware == 225,
              "LAN version packet parses (BCD-decoded)");
        lanver[0] = 8;
        check(!parseLanVersionPacket(lanver, sizeof(lanver), r), "a wrong length is rejected");
    }

    // -----------------------------------------------------------------
    printf("\nHanding a software combination to the hardware combiner\n");
    {
        // The radio computes Y = A + g*B. A signal whose channel-B copy is r times its
        // channel-A copy is cancelled by g = -1/r.
        const std::complex<double> r = std::polar(0.7, 2.39);      // -3 dB, 137 degrees
        const std::complex<double> g = -1.0 / r;

        // An additive coefficient pair that nulls it, as the decorrelator would produce:
        // any scaling of (1, g) is the same combination.
        const std::complex<double> k0 = std::polar(0.31, 1.1);
        const std::complex<double> k1 = k0 * g;

        HardwareWeight h = hardwareWeightFor(k0, k1);
        check(h.representable, "the ratio is inside the radio's range");
        check(std::abs(h.magnitude - std::abs(g)) < 1e-9, "magnitude survives the overall scaling");
        check(std::abs(h.phaseDegrees - std::arg(g) * 180.0 / rsr200::PI) < 1e-9, "as does phase");

        // The real test: quantise through the wire format, then check the weight the radio
        // would actually apply still cancels.
        const uint32_t packed = packMagnitudePhase(h.magnitude, h.phaseDegrees);
        const double qMag = (double)(packed & 0xFFFF) / 8192.0;
        const double qPhase = (double)(int16_t)(packed >> 16) / 32768.0 * 180.0;
        const std::complex<double> qg = std::polar(qMag, qPhase * rsr200::PI / 180.0);
        const double residual = 20.0 * std::log10(std::abs(1.0 + qg * r));
        printf("        after quantisation the null is %.1f dB deep\n", residual);
        check(residual < -60.0, "quantisation is not what limits the null");

        // Out of range: channel B needs more than 8x, which the 16 bit magnitude cannot
        // express. Swapping the channels inverts the ratio and brings it back.
        HardwareWeight big = hardwareWeightFor(std::complex<double>(1.0, 0.0),
                                               std::complex<double>(20.0, 0.0));
        check(!big.representable && big.suggestSwap, "too large a ratio asks for a channel swap");
        HardwareWeight swapped = hardwareWeightFor(std::complex<double>(20.0, 0.0),
                                                   std::complex<double>(1.0, 0.0));
        check(swapped.representable, "and swapping brings it back into range");

        // A combination that ignores channel A entirely cannot be written as A + g*B.
        HardwareWeight none = hardwareWeightFor(std::complex<double>(0.0, 0.0),
                                                std::complex<double>(1.0, 0.0));
        check(!none.representable && none.suggestSwap, "a channel B only combination asks for a swap");

        // The sign convention is the easy thing to get wrong: the hardware adds where the
        // software phaser's manual weight subtracts.
        const std::complex<double> additive = hardwareWeightFor(
            std::complex<double>(1.0, 0.0), std::complex<double>(-0.5, 0.0)).magnitude
            * std::polar(1.0, hardwareWeightFor(std::complex<double>(1.0, 0.0),
                                                std::complex<double>(-0.5, 0.0)).phaseDegrees * rsr200::PI / 180.0);
        check(std::abs(additive - std::complex<double>(-0.5, 0.0)) < 1e-9,
              "a negative coefficient becomes a 180 degree phase, not a negative magnitude");
    }

    printf("\n%s (%d failure%s)\n\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
