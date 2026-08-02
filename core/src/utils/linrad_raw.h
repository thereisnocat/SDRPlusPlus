#pragma once
#include <stdint.h>
#include <string.h>
#include <ctime>
#include <vector>

// Linrad raw file header, for interoperability rather than as our archive format.
// See PHASING_PLAN.md section 4.2.
//
// Why this exists: a dual-channel recording is only useful if the software that analyses
// it can open it, and the tool that matters here is WavViewDX (Reinhard Weiss), which
// imports "Linrad RAW, single and dual-channel" among its formats. Linrad itself has a
// two-channel phasing window, so the same file also opens in an independent implementation
// of the feature this format exists to serve. Neither SDRuno nor SDRconnect can record two
// tuners to one file at all, so their formats were never candidates.
//
// The conversion is nearly free: Linrad's dual-channel payload is int16 interleaved
// I1 Q1 I2 Q2, which is byte for byte what our recorder already writes when the sample
// type is Int16. Only the wrapper differs -- 41 bare bytes here, RIFF chunks there.
//
// What is lost going this way, and why we do not use it as the archive format: the header
// below has nowhere to record phase coherence, sample alignment or per-channel antenna
// names. That matters -- the RSPduo redraws its inter-tuner phase on every start, so a
// recording that cannot say "these channels are not coherent" is one you cannot safely
// re-phase later. Our own 'sdpc' chunk carries those; this is an export.
namespace linrad {

    // rx_input_mode bit flags, from globdef.h in the Linrad source. Absent size bits
    // (DWORD/BYTE/FLOAT/QWORD) mean 16-bit samples, which is what we write.
    constexpr uint32_t DWORD_INPUT  = 1;
    constexpr uint32_t TWO_CHANNELS = 2;
    constexpr uint32_t IQ_DATA      = 4;
    constexpr uint32_t BYTE_INPUT   = 8;
    constexpr uint32_t NO_DUPLEX    = 16;
    constexpr uint32_t DIGITAL_IQ   = 32;
    constexpr uint32_t FLOAT_INPUT  = 64;
    constexpr uint32_t QWORD_INPUT  = 128;

    // A real dual-channel file observed in the wild carries 0x26, which is exactly
    // TWO_CHANNELS | IQ_DATA | DIGITAL_IQ with no size bit set.
    constexpr uint32_t MODE_DUAL_IQ16 = TWO_CHANNELS | IQ_DATA | DIGITAL_IQ;

#pragma pack(push, 1)
    // 41 bytes only because it is packed -- the natural alignment of the two doubles
    // would pad this to 48 and produce a file nothing can read. The static_assert below
    // is the guard, in the same spirit as the one on the auxi chunk.
    struct Header {
        int32_t  proprietaryChunk;  // always -1
        double   timestamp;         // seconds since the Unix epoch
        double   passbandCenter;    // MHz, not Hz
        int32_t  passbandDirection; // +1, or -1 when the spectrum is inverted
        int32_t  inputMode;         // rx_input_mode bits above
        int32_t  rfChannels;        // 2 for a dual-channel recording
        int32_t  adChannels;        // 4 for a dual-channel recording (I1 Q1 I2 Q2)
        int32_t  adSpeed;           // sample rate, samples/second
        uint8_t  saveInitFlag;      // 0
    };
#pragma pack(pop)

    static_assert(sizeof(Header) == 41, "Linrad raw header must be 41 bytes; check packing");

    // centerFreqHz is in Hz and converted here, because the field is MHz and getting that
    // wrong by 1e6 is the single easiest mistake to make with this format.
    inline Header makeDualChannelHeader(double centerFreqHz, double sampleRate,
                                        std::time_t start, bool spectrumInverted = false) {
        Header h{};
        h.proprietaryChunk = -1;
        h.timestamp = (double)start;
        h.passbandCenter = centerFreqHz / 1e6;
        h.passbandDirection = spectrumInverted ? -1 : 1;
        h.inputMode = (int32_t)MODE_DUAL_IQ16;
        h.rfChannels = 2;
        h.adChannels = 4;
        h.adSpeed = (int32_t)sampleRate;
        h.saveInitFlag = 0;
        return h;
    }

    inline std::vector<uint8_t> serialize(const Header& h) {
        std::vector<uint8_t> out(sizeof(Header));
        memcpy(out.data(), &h, sizeof(Header));
        return out;
    }

    inline bool parse(const void* data, size_t len, Header& out) {
        if (len < sizeof(Header)) { return false; }
        memcpy(&out, data, sizeof(Header));
        // The leading -1 is the only identification this format offers; it is a sentinel
        // rather than a magic number, so treat a mismatch as "not a Linrad file" and
        // nothing more.
        if (out.proprietaryChunk != -1) { return false; }
        if (out.passbandDirection != 1 && out.passbandDirection != -1) { return false; }
        return true;
    }
}
