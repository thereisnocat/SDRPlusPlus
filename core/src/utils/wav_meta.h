#pragma once
#include <stdint.h>
#include <string.h>
#include <string>
#include <vector>
#include <ctime>

// Metadata chunks for SDR recordings. See PHASING_PLAN.md section 4.2.
//
// Embedded in the file rather than written to a sidecar, because these recordings are
// kept for years and a sidecar is exactly the thing that gets separated from its audio by
// a move, a copy to another disk, or an archive tool.
namespace wavmeta {

    // ---------------------------------------------------------------------------
    // "auxi" -- the de-facto SDR metadata chunk shared by HDSDR, SDR Console and
    // SpectraVue. Writing it means the recording carries its centre frequency and timing
    // in a form other software already reads.
    //
    // The layout below was decoded from a real HDSDR recording rather than recalled, with
    // the filename as independent ground truth: HDSDR_20171123_005716Z_1130kHz_RF.wav
    // yielded StartTime 2017-11-23 00:57:16, dayOfWeek 4 (a Thursday, which it was), and
    // CenterFreq 1130000. Note that the Perseus family does NOT use this chunk -- the
    // original Perseus writes "rcvr", and the Perseus22 .p22 format is not RIFF at all.
    // ---------------------------------------------------------------------------

#pragma pack(push, 1)
    // Win32 SYSTEMTIME, which is what the chunk stores.
    struct SystemTime {
        uint16_t year;
        uint16_t month;
        uint16_t dayOfWeek;   // 0 = Sunday
        uint16_t day;
        uint16_t hour;
        uint16_t minute;
        uint16_t second;
        uint16_t milliseconds;
    };

    struct AuxiChunk {
        SystemTime startTime;
        SystemTime stopTime;
        uint32_t centerFreq;    // Hz
        uint32_t adFrequency;   // sample rate at the A/D, 0 if not meaningful
        uint32_t ifFrequency;
        uint32_t bandwidth;
        uint32_t iqOffset;
        uint32_t dbOffset;
        uint32_t maxVal;
        // Named "unused" in the layouts this was decoded from, but SDRuno does use them:
        // it stores the initial gains there in thousandths of a dB. We write zeros, which
        // such a reader takes as a valid 0.000 dB rather than as "absent" -- worth knowing
        // before anyone treats these as free space.
        uint32_t initialGain1;
        uint32_t initialGain2;
        // Followed by a NUL-terminated "next file" name, padding the chunk to 164 bytes
        // in the files examined. We write the name empty and pad to the same length so
        // readers expecting a fixed size are not surprised.
        char nextFilename[96];
    };
#pragma pack(pop)

    static_assert(sizeof(SystemTime) == 16, "SYSTEMTIME must be 16 bytes");
    static_assert(sizeof(AuxiChunk) == 164, "auxi chunk must match the observed 164 bytes");

    inline SystemTime toSystemTime(std::time_t t, int milliseconds = 0) {
        std::tm tmv{};
#ifdef _WIN32
        gmtime_s(&tmv, &t);
#else
        gmtime_r(&t, &tmv);
#endif
        SystemTime st{};
        st.year = (uint16_t)(tmv.tm_year + 1900);
        st.month = (uint16_t)(tmv.tm_mon + 1);
        st.dayOfWeek = (uint16_t)tmv.tm_wday;
        st.day = (uint16_t)tmv.tm_mday;
        st.hour = (uint16_t)tmv.tm_hour;
        st.minute = (uint16_t)tmv.tm_min;
        st.second = (uint16_t)tmv.tm_sec;
        st.milliseconds = (uint16_t)milliseconds;
        return st;
    }

    inline AuxiChunk makeAuxi(double centerFreq, double sampleRate, std::time_t start, std::time_t stop) {
        AuxiChunk c{};
        c.startTime = toSystemTime(start);
        c.stopTime = toSystemTime(stop);
        c.centerFreq = (uint32_t)centerFreq;
        c.adFrequency = (uint32_t)sampleRate;
        c.bandwidth = (uint32_t)sampleRate;
        return c;
    }

    // ---------------------------------------------------------------------------
    // "sdpc" -- SDR++ phasing channels. Our own chunk, for what auxi has no room for:
    // auxi predates anyone recording two antennas at once, so it carries a single centre
    // frequency and no notion of per-channel antenna labels or coherence.
    //
    // Keeping our fields out of auxi leaves the standard chunk standard, so other
    // software still reads what it understands and ignores the rest.
    //
    // Layout (little-endian), frozen at version 1:
    //
    //   uint32 version        1
    //   uint32 channelCount   channels interleaved in the data chunk (2 complex = 4 WAV channels)
    //   uint32 flags          bit 0: phase coherent, bit 1: sample aligned
    //   uint32 combinedA      index of the channel the phaser used as A
    //   uint32 combinedB      index of the channel the phaser used as B
    //   uint32 nameBytes      length of the names blob that follows
    //   char[] names          NUL-separated channel names, in channel order
    // ---------------------------------------------------------------------------

    constexpr char PHASING_CHUNK_ID[4] = { 's', 'd', 'p', 'c' };
    constexpr uint32_t PHASING_VERSION = 1;
    constexpr uint32_t PHASING_FLAG_COHERENT = 1u << 0;
    constexpr uint32_t PHASING_FLAG_ALIGNED = 1u << 1;

    struct PhasingInfo {
        uint32_t version = PHASING_VERSION;
        uint32_t channelCount = 0;
        bool phaseCoherent = false;
        bool sampleAligned = false;
        uint32_t combinedA = 0;
        uint32_t combinedB = 1;
        std::vector<std::string> names;
    };

    inline std::vector<uint8_t> serializePhasing(const PhasingInfo& info) {
        std::string blob;
        for (const auto& n : info.names) {
            blob += n;
            blob.push_back('\0');
        }

        uint32_t flags = 0;
        if (info.phaseCoherent) { flags |= PHASING_FLAG_COHERENT; }
        if (info.sampleAligned) { flags |= PHASING_FLAG_ALIGNED; }

        const uint32_t fields[6] = {
            PHASING_VERSION, info.channelCount, flags,
            info.combinedA, info.combinedB, (uint32_t)blob.size()
        };

        std::vector<uint8_t> out(sizeof(fields) + blob.size());
        memcpy(out.data(), fields, sizeof(fields));
        if (!blob.empty()) { memcpy(out.data() + sizeof(fields), blob.data(), blob.size()); }
        return out;
    }

    inline bool parsePhasing(const void* data, size_t len, PhasingInfo& out) {
        if (len < 24) { return false; }
        uint32_t f[6];
        memcpy(f, data, sizeof(f));
        if (f[0] != PHASING_VERSION) { return false; }

        out.version = f[0];
        out.channelCount = f[1];
        out.phaseCoherent = (f[2] & PHASING_FLAG_COHERENT) != 0;
        out.sampleAligned = (f[2] & PHASING_FLAG_ALIGNED) != 0;
        out.combinedA = f[3];
        out.combinedB = f[4];

        out.names.clear();
        const uint32_t nameBytes = f[5];
        if (nameBytes == 0 || 24 + (size_t)nameBytes > len) { return true; }

        const char* p = (const char*)data + 24;
        size_t off = 0;
        while (off < nameBytes) {
            const size_t remaining = nameBytes - off;
            const size_t n = strnlen(p + off, remaining);
            out.names.push_back(std::string(p + off, n));
            off += n + 1;
        }
        return true;
    }
}
