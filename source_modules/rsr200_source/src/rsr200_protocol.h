#pragma once
#include <stdint.h>
#include <string.h>
#include <vector>
#include <cstddef>
#include <cmath>
#include <algorithm>

// Wire protocol for the Reuter RSR200B, from RSR200_DP_ENG_V52.pdf (firmware 225).
// See RSR200_PLAN.md.
//
// Deliberately free of any SDR++ dependency: block geometry, command construction, sample
// unpacking and Nyquist-zone arithmetic are all pure functions over bytes, so they can be
// written and tested against the documented layouts long before the radio is on the desk.
// Everything here is little-endian on the wire.
//
// The USB and LAN interfaces share this command set and differ only in framing, which is
// why the two transports sit above this rather than each carrying their own copy.

namespace rsr200 {

    // ---------------------------------------------------------------------------
    // Modes
    // ---------------------------------------------------------------------------

    enum Interface {
        IFACE_USB = 1,
        IFACE_LAN = 2,
        IFACE_DSP_ONLY = 3      // change DSP mode without touching an interface
    };

    enum StreamPort {
        PORT_UDP = 0,
        PORT_TCP = 1,
        PORT_USB = 2
    };

    // DSP mode bits 0-1. "Independent" is what dual-channel phasing needs; it requires the
    // port mode to be dual-channel as well.
    enum OpMode {
        OP_INDEPENDENT = 0,     // "Separate": two unrelated channels
        OP_PARALLEL_ADD = 1,    // ADC1 + ADC2 summed
        OP_SERIAL = 2,          // time-interleaved sampling, doubles the Nyquist zones
        OP_DIVERSITY = 3        // ADC1 + ADC2 with the hardware magnitude/phase weight
    };

    // "Set frequency generators or IP address" (0xB0) selector.
    enum GenSelect {
        GEN_LO_CH1 = 0,
        GEN_LO_CH2 = 1,
        GEN_LO_BOTH = 2,        // the only phase-safe way to tune both channels
        GEN_MAG_PHASE_CH2 = 9,
        GEN_IP_ADDRESS = 10
    };

    // "Set variable 16 bit value" (0xF5) variable numbers.
    enum Variable {
        VAR_CLOCK_CORRECTION = 0,
        VAR_ATTENUATOR_ADC1 = 1,
        VAR_ATTENUATOR_ADC2 = 2,
        VAR_ANTENNA_HF1_VHF = 3,
        VAR_ANTENNA_HF2 = 4,
        VAR_SWITCH = 5,
        VAR_ANTENNA_FREQ_HF1_VHF = 6,
        VAR_ANTENNA_FREQ_HF2 = 7
    };

    // Bits of the VAR_SWITCH register.
    enum SwitchBits {
        SW_ADC2_CLK_INVERTED = 1 << 0,
        SW_ADC1_TO_VHF = 1 << 1,        // clear: ADC1 to HF1
        SW_ADC2_TO_HF2 = 1 << 2,        // clear: ADC2 parallel with ADC1
        SW_REMOTE_PWR_CH1 = 1 << 3,
        SW_REMOTE_CTRL_CH1 = 1 << 4,    // set: control signalling, clear: plain +12 V
        SW_REMOTE_PWR_CH2 = 1 << 5,
        SW_REMOTE_CTRL_CH2 = 1 << 6,
        SW_VHF_PREAMP = 1 << 7
    };

    struct StreamFormat {
        int channels = 1;       // 1 or 2
        int bits = 24;          // 16 or 24

        bool operator==(const StreamFormat& o) const {
            return channels == o.channels && bits == o.bits;
        }
    };

    // Decimation is 2^(exp+1) for exp in 0..5, so 2 to 64. Sample rate = ADC clock / rate.
    inline int decimationRate(int exp) { return 1 << (std::clamp(exp, 0, 5) + 1); }

    inline int decimationExponentFor(int rate) {
        for (int e = 0; e <= 5; e++) {
            if (decimationRate(e) == rate) { return e; }
        }
        return -1;
    }

    // ---------------------------------------------------------------------------
    // LAN block geometry
    //
    // Blocks carry a fixed 130560 samples, so the byte length varies with the format --
    // except at 24 bit, where one and two channel blocks are the same length and the dual
    // channel case therefore carries only half as many samples per channel.
    // ---------------------------------------------------------------------------

    constexpr int LAN_SAMPLES_PER_BLOCK = 130560;
    constexpr int UDP_PACKET_BYTES = 1458;      // 2 byte index + 1456 payload
    constexpr int UDP_PAYLOAD_BYTES = 1456;
    constexpr uint16_t LAN_TCP_PORT = 55557;
    constexpr uint16_t LAN_UDP_PORT = 55558;

    // Little-endian on the wire: 78 56 34 12 F0 DE BC 9A
    constexpr uint8_t SYNC_BYTES[8] = { 0x78, 0x56, 0x34, 0x12, 0xF0, 0xDE, 0xBC, 0x9A };

    struct BlockLayout {
        StreamFormat format;
        int samplesPerChannel = 0;
        int bytesPerFrame = 0;          // one sample across all channels
        size_t iqBytes = 0;
        size_t blockBytes = 0;

        size_t counterOffset = 0;
        size_t invCounterOffset = 0;
        size_t syncOffset = 0;
        size_t tempOffset = 0;
        size_t gpsOffset = 0;
        size_t cmdNoOffset = 0;
        size_t cmdCountOffset = 0;
        size_t commandsOffset = 0;
        size_t commandSpace = 0;

        uint8_t startStreamSizeCode = 0;
        int udpPackets = 0;
    };

    inline BlockLayout lanLayout(const StreamFormat& fmt) {
        BlockLayout l;
        l.format = fmt;
        l.bytesPerFrame = (fmt.bits / 8) * 2 * fmt.channels;

        // 24 bit blocks are a fixed length regardless of channel count, so dual channel
        // halves the samples rather than doubling the block.
        if (fmt.bits == 24) {
            l.samplesPerChannel = (fmt.channels == 2) ? LAN_SAMPLES_PER_BLOCK / 2
                                                      : LAN_SAMPLES_PER_BLOCK;
        }
        else {
            l.samplesPerChannel = LAN_SAMPLES_PER_BLOCK;
        }
        l.iqBytes = (size_t)l.samplesPerChannel * (size_t)l.bytesPerFrame;

        l.counterOffset = l.iqBytes;
        l.invCounterOffset = l.counterOffset + 4;
        l.syncOffset = l.invCounterOffset + 4;
        l.tempOffset = l.syncOffset + 8;
        l.gpsOffset = l.tempOffset + 1;
        l.cmdNoOffset = l.gpsOffset + 2;
        l.cmdCountOffset = l.cmdNoOffset + 1;
        l.commandsOffset = l.cmdCountOffset + 4;

        // Documented block lengths. They are all exact multiples of the UDP payload size,
        // which is what makes fragmentation come out even.
        if (fmt.bits == 16 && fmt.channels == 1) {
            l.blockBytes = 522704;
            l.startStreamSizeCode = 7;
        }
        else if (fmt.bits == 16 && fmt.channels == 2) {
            l.blockBytes = 1045408;
            l.startStreamSizeCode = 15;
        }
        else if (fmt.bits == 24 && fmt.channels == 2) {
            l.blockBytes = 784784;
            l.startStreamSizeCode = 5;
        }
        else {
            l.blockBytes = 784784;
            l.startStreamSizeCode = 0;      // "all other values" mean 1 channel 24 bit
        }

        l.commandSpace = l.blockBytes - l.commandsOffset;
        l.udpPackets = (int)((l.blockBytes + UDP_PAYLOAD_BYTES - 1) / UDP_PAYLOAD_BYTES);
        return l;
    }

    // ---------------------------------------------------------------------------
    // USB packet geometry: a fixed 4096 bytes carrying exactly one command.
    // ---------------------------------------------------------------------------

    constexpr size_t USB_PACKET_BYTES = 4096;
    constexpr size_t USB_IQ_OFFSET = 4;
    constexpr size_t USB_IQ_BYTES = 4080;
    constexpr size_t USB_TEMP_OFFSET = 4084;
    constexpr size_t USB_GPS_OFFSET = 4085;
    constexpr size_t USB_CMD_NO_OFFSET = 4087;
    constexpr size_t USB_COMMAND_OFFSET = 4088;
    constexpr size_t USB_COMMAND_BYTES = 8;
    constexpr uint8_t USB_ENDPOINT_IN = 0x82;
    constexpr uint8_t USB_ENDPOINT_OUT = 0x02;

    inline int usbSamplesPerPacket(const StreamFormat& fmt) {
        return (int)(USB_IQ_BYTES / (size_t)((fmt.bits / 8) * 2 * fmt.channels));
    }

    // ---------------------------------------------------------------------------
    // Status header (temperature / GPS correction / overload), shared by both framings
    // ---------------------------------------------------------------------------

    struct Status {
        bool autoAttActive = false;   // temperature reads 0x80 while the attenuator is in
        int temperatureC = 0;         // meaningless when autoAttActive
        bool freqCorrectionValid = false;
        int freqCorrectionRaw = 0;    // signed 14 bit
        bool overloadCh1 = false;
        bool overloadCh2 = false;
    };

    // Frequency correction resolution depends on whether the radio is disciplining its own
    // clock: 0.5 Hz per LSB when it is, 0.1 Hz when it is only measuring.
    inline double freqCorrectionHz(const Status& s, bool internalControlOn) {
        return (double)s.freqCorrectionRaw * (internalControlOn ? 0.5 : 0.1);
    }

    inline Status parseStatus(uint8_t temp, uint8_t gpsLo, uint8_t gpsHi) {
        Status s;

        // 0x80 in the temperature byte is the Auto-ATT indicator, not -128 degrees.
        s.autoAttActive = (temp == 0x80);
        s.temperatureC = s.autoAttActive ? 0 : (int)(int8_t)temp;

        s.overloadCh1 = (gpsHi & 0x40) != 0;
        s.overloadCh2 = (gpsHi & 0x80) != 0;

        int raw = (int)gpsLo | (((int)gpsHi & 0x3F) << 8);
        if (raw & 0x2000) { raw |= ~0x3FFF; }               // sign extend from 14 bits

        // 0x2000 is the largest negative value and means "no valid measurement", either
        // because GPS is not being received or because the frequency has just changed.
        s.freqCorrectionValid = (raw != -8192);
        s.freqCorrectionRaw = raw;
        return s;
    }

    // ---------------------------------------------------------------------------
    // Sample unpacking
    //
    // Outputs interleaved (re, im) float pairs, which is layout-compatible with
    // dsp::complex_t. outB is only touched for a dual channel format.
    // ---------------------------------------------------------------------------

    // Enabling Auto-ATT scales the whole data stream down by 2 bits to make headroom, so
    // everything downstream has to be scaled back up by the same amount.
    constexpr float AUTO_ATT_GAIN = 4.0f;

    inline float fullScaleFor(int bits) {
        return (bits == 16) ? 32768.0f : 8388608.0f;
    }

    inline int32_t read24(const uint8_t* p) {
        int32_t v = (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16));
        if (v & 0x800000) { v |= ~0xFFFFFF; }
        return v;
    }

    inline int16_t read16(const uint8_t* p) {
        return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
    }

    // Returns the number of samples written per channel.
    inline int unpack(const uint8_t* iq, int frames, const StreamFormat& fmt,
                      float gain, float* outA, float* outB) {
        const float scale = gain / fullScaleFor(fmt.bits);
        const int step = (fmt.bits / 8) * 2;    // one complex sample of one channel

        if (fmt.channels == 1) {
            for (int i = 0; i < frames; i++) {
                const uint8_t* p = iq + (size_t)i * step;
                if (fmt.bits == 16) {
                    outA[2 * i] = (float)read16(p) * scale;
                    outA[2 * i + 1] = (float)read16(p + 2) * scale;
                }
                else {
                    outA[2 * i] = (float)read24(p) * scale;
                    outA[2 * i + 1] = (float)read24(p + 3) * scale;
                }
            }
            return frames;
        }

        // Dual channel is interleaved per sample: I1 Q1 I2 Q2.
        for (int i = 0; i < frames; i++) {
            const uint8_t* p = iq + (size_t)i * step * 2;
            if (fmt.bits == 16) {
                outA[2 * i] = (float)read16(p) * scale;
                outA[2 * i + 1] = (float)read16(p + 2) * scale;
                outB[2 * i] = (float)read16(p + 4) * scale;
                outB[2 * i + 1] = (float)read16(p + 6) * scale;
            }
            else {
                outA[2 * i] = (float)read24(p) * scale;
                outA[2 * i + 1] = (float)read24(p + 3) * scale;
                outB[2 * i] = (float)read24(p + 6) * scale;
                outB[2 * i + 1] = (float)read24(p + 9) * scale;
            }
        }
        return frames;
    }

    // ---------------------------------------------------------------------------
    // LAN block validation and resynchronisation
    // ---------------------------------------------------------------------------

    inline uint32_t readU32(const uint8_t* p) {
        return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    }

    inline void writeU32(uint8_t* p, uint32_t v) {
        p[0] = (uint8_t)(v & 0xFF);
        p[1] = (uint8_t)((v >> 8) & 0xFF);
        p[2] = (uint8_t)((v >> 16) & 0xFF);
        p[3] = (uint8_t)((v >> 24) & 0xFF);
    }

    // A block is credible when the sync words are present and the counter matches its own
    // ones' complement. Both checks together make a false positive very unlikely, which is
    // what lets a receiver find block boundaries in an arbitrary byte stream.
    inline bool blockTrailerValid(const uint8_t* block, const BlockLayout& l) {
        if (memcmp(block + l.syncOffset, SYNC_BYTES, sizeof(SYNC_BYTES)) != 0) { return false; }
        const uint32_t c = readU32(block + l.counterOffset);
        const uint32_t inv = readU32(block + l.invCounterOffset);
        return (c ^ inv) == 0xFFFFFFFFu;
    }

    // Offset of the first complete, valid block in `data`, or -1. Scans for the sync words
    // rather than trying every offset, then validates the candidate.
    inline ptrdiff_t findBlockStart(const uint8_t* data, size_t len, const BlockLayout& l) {
        if (len < l.blockBytes) { return -1; }
        for (size_t p = l.syncOffset; p + sizeof(SYNC_BYTES) <= len; p++) {
            if (data[p] != SYNC_BYTES[0]) { continue; }
            if (memcmp(data + p, SYNC_BYTES, sizeof(SYNC_BYTES)) != 0) { continue; }
            const size_t start = p - l.syncOffset;
            if (start + l.blockBytes > len) { return -1; }
            if (blockTrailerValid(data + start, l)) { return (ptrdiff_t)start; }
        }
        return -1;
    }

    // ---------------------------------------------------------------------------
    // Commands, PC -> RSR200
    //
    // Every command starts with a 32-bit number of our choosing, which comes back in the
    // acknowledgement. Never send 0: the radio uses 0 to mark commands it generated itself.
    // LAN truncates the unused tail of each command; USB always sends the full length.
    //
    // The "repeat counter" is documented for retries but firmware 22x ignores it, so a
    // retry should be a fresh command number rather than a repeat.
    // ---------------------------------------------------------------------------

    namespace instr {
        constexpr uint8_t ENABLE_FW_UPDATE = 0x0B;
        constexpr uint8_t START_FW_UPDATE = 0x0C;
        constexpr uint8_t FW_DATA = 0x0D;
        constexpr uint8_t READ_VERSION = 0x12;
        constexpr uint8_t START_STREAM = 0x15;
        constexpr uint8_t STOP_STREAM = 0x16;
        constexpr uint8_t SET_GENERATORS = 0xB0;
        constexpr uint8_t SET_AUTO_ATT = 0xB1;
        constexpr uint8_t RESET = 0xB2;
        constexpr uint8_t SET_DATA_TRANSMISSION = 0xB4;
        constexpr uint8_t SET_ADC_CLOCK = 0xF2;
        constexpr uint8_t SET_VARIABLE = 0xF5;
    }

    // usbLen/lanLen are the documented sizes; the builder emits whichever applies.
    inline std::vector<uint8_t> makeCommand(uint32_t number, uint8_t instruction,
                                            const std::vector<uint8_t>& params,
                                            size_t usbLen, size_t lanLen, bool lan) {
        std::vector<uint8_t> c(lan ? lanLen : usbLen, 0);
        writeU32(c.data(), number);
        c[4] = instruction;
        for (size_t i = 0; i < params.size() && 5 + i < c.size(); i++) { c[5 + i] = params[i]; }
        return c;
    }

    inline std::vector<uint8_t> cmdReset(uint32_t no, bool lan) {
        return makeCommand(no, instr::RESET, {}, 8, 8, lan);
    }

    inline std::vector<uint8_t> cmdReadVersion(uint32_t no, bool lan, uint8_t repeat = 0) {
        return makeCommand(no, instr::READ_VERSION, { repeat }, 8, 6, lan);
    }

    inline std::vector<uint8_t> cmdStartStream(uint32_t no, bool lan, StreamPort port, uint8_t sizeCode) {
        return makeCommand(no, instr::START_STREAM, { (uint8_t)port, sizeCode }, 8, 7, lan);
    }

    inline std::vector<uint8_t> cmdStopStream(uint32_t no, bool lan, StreamPort port, uint8_t repeat = 0) {
        return makeCommand(no, instr::STOP_STREAM, { (uint8_t)port, repeat }, 8, 7, lan);
    }

    // clockHz is rounded to the radio's 0.1 MHz step. RSR200B accepts 70 .. 200 MHz.
    inline std::vector<uint8_t> cmdSetAdcClock(uint32_t no, bool lan, double clockHz,
                                               bool gpsDisciplineOn, uint8_t repeat = 0) {
        int units = (int)std::lround(clockHz / 100000.0);       // 0.1 MHz per unit
        units = std::clamp(units, 700, 2000);
        const uint8_t lsb = (uint8_t)(units & 0xFF);
        uint8_t msb = (uint8_t)((units >> 8) & 0x7F);
        if (!gpsDisciplineOn) { msb |= 0x80; }                  // bit 7 is "GPS-Dis"
        return makeCommand(no, instr::SET_ADC_CLOCK, { lsb, msb, repeat }, 8, 8, lan);
    }

    inline std::vector<uint8_t> cmdSetGenerator(uint32_t no, bool lan, GenSelect select,
                                                uint32_t value, uint8_t repeat = 0) {
        std::vector<uint8_t> p = {
            (uint8_t)select,
            (uint8_t)(value & 0xFF), (uint8_t)((value >> 8) & 0xFF),
            (uint8_t)((value >> 16) & 0xFF), (uint8_t)((value >> 24) & 0xFF),
            repeat
        };
        return makeCommand(no, instr::SET_GENERATORS, p, 12, 11, lan);
    }

    // Tuning both channels with one command is the only way to keep them phase locked.
    inline std::vector<uint8_t> cmdSetLoBoth(uint32_t no, bool lan, double loHz) {
        return cmdSetGenerator(no, lan, GEN_LO_BOTH, (uint32_t)(int32_t)std::llround(loHz));
    }

    // Hardware diversity weight: magnitude 1/8192 per LSB, phase spanning +/-180 degrees.
    inline uint32_t packMagnitudePhase(double magnitude, double phaseDegrees) {
        int mag = (int)std::lround(std::clamp(magnitude, 0.0, 7.9999) * 8192.0);
        mag = std::clamp(mag, 0, 65535);
        // 0x8000 is -180 degrees and 0x7FFF is +180 minus one LSB, so a full circle spans
        // 65536 steps. Clamp after scaling, not before: clamping the angle to 179.99 first
        // lands a step short of the top of the range.
        int phi = (int)std::lround(phaseDegrees / 360.0 * 65536.0);
        phi = std::clamp(phi, -32768, 32767);
        return ((uint32_t)(uint16_t)(int16_t)phi << 16) | (uint32_t)(uint16_t)mag;
    }

    inline std::vector<uint8_t> cmdSetVariable(uint32_t no, bool lan, Variable v,
                                               uint16_t value, uint8_t repeat = 0) {
        std::vector<uint8_t> p = { (uint8_t)v, (uint8_t)(value & 0xFF), (uint8_t)(value >> 8), repeat };
        return makeCommand(no, instr::SET_VARIABLE, p, 12, 9, lan);
    }

    inline uint8_t portModeByte(int decimationExp, bool dualChannel, bool bits16, bool swapChannels) {
        uint8_t b = (uint8_t)(std::clamp(decimationExp, 0, 5) & 0x07);
        if (swapChannels) { b |= 1 << 3; }
        if (dualChannel) { b |= 1 << 4; }
        if (bits16) { b |= 1 << 5; }
        return b;
    }

    inline uint8_t dspModeByte(OpMode mode, bool upperSideband) {
        uint8_t b = (uint8_t)(mode & 0x03);
        if (upperSideband) { b |= 1 << 3; }
        return b;
    }

    inline std::vector<uint8_t> cmdSetDataTransmission(uint32_t no, bool lan, Interface iface,
                                                       uint8_t portMode, uint8_t dspMode,
                                                       uint8_t repeat = 0) {
        std::vector<uint8_t> p = { (uint8_t)iface, portMode, dspMode, repeat };
        return makeCommand(no, instr::SET_DATA_TRANSMISSION, p, 12, 9, lan);
    }

    inline std::vector<uint8_t> cmdSetAutoAttenuator(uint32_t no, bool lan, uint8_t threshold,
                                                     uint32_t holdTimeClocks,
                                                     uint16_t gainCh1, uint16_t gainCh2,
                                                     uint8_t repeat = 0) {
        std::vector<uint8_t> p = {
            threshold,
            (uint8_t)(holdTimeClocks & 0xFF),
            (uint8_t)((holdTimeClocks >> 8) & 0xFF),
            (uint8_t)((holdTimeClocks >> 16) & 0xFF),
            (uint8_t)(gainCh1 & 0xFF), (uint8_t)(gainCh1 >> 8),
            (uint8_t)(gainCh2 & 0xFF), (uint8_t)(gainCh2 >> 8),
            repeat
        };
        return makeCommand(no, instr::SET_AUTO_ATT, p, 16, 14, lan);
    }

    // ---------------------------------------------------------------------------
    // Replies, RSR200 -> PC
    // ---------------------------------------------------------------------------

    enum ReplyKind {
        REPLY_NONE,
        REPLY_CONFIRMATION,          // command executed, nothing to report
        REPLY_SPECIAL,               // command executed, with feedback data
        REPLY_VERSION                // serial number and firmware version
    };

    struct Reply {
        ReplyKind kind = REPLY_NONE;
        uint32_t confirmedCommand = 0;   // 0 also means "self-generated by the radio"
        uint8_t echoedInstruction = 0;
        uint8_t data[3] = { 0, 0, 0 };
        uint32_t serial = 0;
        uint32_t firmware = 0;
        bool selfGenerated = false;
    };

    // An 8-byte command as embedded in a USB packet or a LAN block.
    inline Reply parseEmbeddedCommand(const uint8_t* c) {
        Reply r;
        if (c[0] == instr::READ_VERSION) {
            r.kind = REPLY_VERSION;
            r.serial = (uint32_t)c[1] | ((uint32_t)c[2] << 8) | ((uint32_t)c[3] << 16);
            r.firmware = readU32(c + 4);
            return r;
        }
        r.confirmedCommand = readU32(c + 4);
        r.selfGenerated = (r.confirmedCommand == 0);
        if (c[0] == 0 && c[1] == 0 && c[2] == 0 && c[3] == 0) {
            r.kind = REPLY_CONFIRMATION;
        }
        else {
            r.kind = REPLY_SPECIAL;
            r.echoedInstruction = c[0];
            r.data[0] = c[1];
            r.data[1] = c[2];
            r.data[2] = c[3];
        }
        return r;
    }

    // The 12-byte standalone packet the radio sends over LAN when not streaming.
    inline bool parseLanVersionPacket(const uint8_t* p, size_t len, Reply& out) {
        if (len < 12 || readU32(p) != 12 || p[4] != instr::READ_VERSION) { return false; }
        out = Reply();
        out.kind = REPLY_VERSION;
        out.serial = (uint32_t)p[5] | ((uint32_t)p[6] << 8) | ((uint32_t)p[7] << 16);
        out.firmware = readU32(p + 8);
        return true;
    }

    // ---------------------------------------------------------------------------
    // Tuning and Nyquist zones
    //
    // The ADC digitises everything; anything above half the clock folds back. So a wanted
    // RF frequency does not map directly onto the mixer setting -- it has to be reduced to
    // its position within the digitised baseband, and every even zone arrives with its
    // spectrum reversed.
    // ---------------------------------------------------------------------------

    struct Tuning {
        int zone = 1;               // 1-based Nyquist zone
        double loHz = 0.0;          // mixer setting, within 0 .. adcClock/2
        bool spectrumInverted = false;
        double aliasBelowHz = 0.0;  // nearest image from the zone below
        double aliasAboveHz = 0.0;  // nearest image from the zone above
    };

    inline Tuning tuneFor(double rfHz, double adcClockHz) {
        Tuning t;
        const double half = adcClockHz * 0.5;
        if (half <= 0.0) { return t; }

        t.zone = (int)std::floor(rfHz / half) + 1;
        if (t.zone < 1) { t.zone = 1; }

        if (t.zone % 2 == 1) {
            t.loHz = rfHz - (double)(t.zone - 1) * half;
            t.spectrumInverted = false;
        }
        else {
            t.loHz = (double)t.zone * half - rfHz;
            t.spectrumInverted = true;
        }

        // Frequencies that land on the same baseband position from the neighbouring zones,
        // i.e. what an inadequate anti-alias filter will let through on top of the signal.
        t.aliasBelowHz = (double)(t.zone - 1) * half - t.loHz;
        t.aliasAboveHz = (double)t.zone * half + t.loHz;
        if (t.zone % 2 == 0) {
            t.aliasBelowHz = (double)(t.zone - 1) * half + t.loHz;
            t.aliasAboveHz = (double)t.zone * half - t.loHz + half;
        }
        if (t.aliasBelowHz < 0.0) { t.aliasBelowHz = 0.0; }
        return t;
    }

    inline double sampleRateHz(double adcClockHz, int decimationExp) {
        return adcClockHz / (double)decimationRate(decimationExp);
    }
}
