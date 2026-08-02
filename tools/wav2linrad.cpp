// Converts a dual-channel SDR++ phasing recording (4-channel RIFF/WAVE, Int16) into a
// Linrad dual-channel raw file, so it can be opened in WavViewDX or Linrad.
//
//   c++ -std=c++17 -O2 -I../core/src -o wav2linrad wav2linrad.cpp
//   ./wav2linrad recording.wav recording.raw
//
// The sample data is copied through untouched: Linrad's dual-channel payload is int16
// interleaved I1 Q1 I2 Q2, which is exactly what the recorder writes. Only the container
// changes, so this is lossless for the samples and lossy only for the metadata Linrad's
// header has no room for (antenna names, phase coherence, sample alignment).
#include "utils/linrad_raw.h"
#include "utils/wav_meta.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

    struct Riff {
        uint16_t channels = 0;
        uint32_t sampleRate = 0;
        uint16_t bitsPerSample = 0;
        double centerFreq = 0.0;      // from auxi, 0 if absent
        std::time_t startTime = 0;    // from auxi, 0 if absent
        bool haveAuxi = false;
        long dataOffset = 0;
        uint32_t dataBytes = 0;
        wavmeta::PhasingInfo phasing;
        bool havePhasing = false;
    };

    bool readExact(std::FILE* f, void* dst, size_t n) {
        return std::fread(dst, 1, n, f) == n;
    }

    std::time_t fromSystemTime(const wavmeta::SystemTime& st) {
        std::tm tmv{};
        tmv.tm_year = st.year - 1900;
        tmv.tm_mon = st.month - 1;
        tmv.tm_mday = st.day;
        tmv.tm_hour = st.hour;
        tmv.tm_min = st.minute;
        tmv.tm_sec = st.second;
#ifdef _WIN32
        return _mkgmtime(&tmv);
#else
        return timegm(&tmv);
#endif
    }

    // Walks the RIFF chunk list. Deliberately tolerant: unknown chunks are skipped, which
    // is the whole reason the recording is in a chunked container to begin with.
    bool scan(std::FILE* f, Riff& out, std::string& err) {
        char riff[4], wave[4];
        uint32_t riffSize;
        if (!readExact(f, riff, 4) || !readExact(f, &riffSize, 4) || !readExact(f, wave, 4)) {
            err = "file is too short to be RIFF";
            return false;
        }
        if (memcmp(riff, "RIFF", 4) != 0 || memcmp(wave, "WAVE", 4) != 0) {
            err = "not a RIFF/WAVE file";
            return false;
        }

        for (;;) {
            char id[4];
            uint32_t size;
            if (!readExact(f, id, 4) || !readExact(f, &size, 4)) { break; }
            const long body = std::ftell(f);

            if (memcmp(id, "fmt ", 4) == 0 && size >= 16) {
                uint16_t fmt, ch, bits, align;
                uint32_t rate, bps;
                readExact(f, &fmt, 2); readExact(f, &ch, 2); readExact(f, &rate, 4);
                readExact(f, &bps, 4); readExact(f, &align, 2); readExact(f, &bits, 2);
                out.channels = ch; out.sampleRate = rate; out.bitsPerSample = bits;
            }
            else if (memcmp(id, "auxi", 4) == 0 && size >= sizeof(wavmeta::AuxiChunk)) {
                wavmeta::AuxiChunk a{};
                if (readExact(f, &a, sizeof(a))) {
                    out.centerFreq = (double)a.centerFreq;
                    out.startTime = fromSystemTime(a.startTime);
                    out.haveAuxi = true;
                }
            }
            else if (memcmp(id, wavmeta::PHASING_CHUNK_ID, 4) == 0) {
                std::vector<uint8_t> blob(size);
                if (readExact(f, blob.data(), size)) {
                    out.havePhasing = wavmeta::parsePhasing(blob.data(), blob.size(), out.phasing);
                }
            }
            else if (memcmp(id, "data", 4) == 0) {
                out.dataOffset = body;
                out.dataBytes = size;
            }

            // Chunks are word aligned.
            if (std::fseek(f, body + (long)size + (long)(size & 1), SEEK_SET) != 0) { break; }
        }

        if (!out.dataOffset) { err = "no data chunk"; return false; }
        return true;
    }
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
            "usage: wav2linrad <input.wav> <output.raw> [centerFreqHz]\n"
            "\n"
            "Converts a 4-channel Int16 SDR++ phasing recording to Linrad dual-channel raw.\n"
            "The centre frequency is taken from the auxi chunk unless given explicitly.\n");
        return 2;
    }

    std::FILE* in = std::fopen(argv[1], "rb");
    if (!in) { std::fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }

    Riff r;
    std::string err;
    if (!scan(in, r, err)) {
        std::fprintf(stderr, "%s: %s\n", argv[1], err.c_str());
        std::fclose(in);
        return 1;
    }

    // Refuse rather than write a file that looks valid and is not. Linrad's header has no
    // field that would let a reader discover either of these was wrong.
    if (r.channels != 4) {
        std::fprintf(stderr, "%s has %u channels; a dual-channel phasing recording has 4 "
                             "(I1 Q1 I2 Q2)\n", argv[1], r.channels);
        std::fclose(in); return 1;
    }
    if (r.bitsPerSample != 16) {
        std::fprintf(stderr, "%s is %u-bit; Linrad dual-channel raw is int16. Re-record with "
                             "the sample type set to Int16.\n", argv[1], r.bitsPerSample);
        std::fclose(in); return 1;
    }

    double center = (argc > 3) ? std::atof(argv[3]) : r.centerFreq;
    if (center <= 0.0) {
        std::fprintf(stderr, "no centre frequency in the file and none given; pass it as the "
                             "third argument, in Hz\n");
        std::fclose(in); return 1;
    }

    std::FILE* out = std::fopen(argv[2], "wb");
    if (!out) { std::fprintf(stderr, "cannot create %s\n", argv[2]); std::fclose(in); return 1; }

    const linrad::Header h = linrad::makeDualChannelHeader(center, r.sampleRate, r.startTime);
    const std::vector<uint8_t> hdr = linrad::serialize(h);
    std::fwrite(hdr.data(), 1, hdr.size(), out);

    std::fseek(in, r.dataOffset, SEEK_SET);
    std::vector<uint8_t> buf(1 << 20);
    uint32_t left = r.dataBytes;
    while (left) {
        const size_t want = (left < buf.size()) ? left : buf.size();
        const size_t got = std::fread(buf.data(), 1, want, in);
        if (!got) { break; }
        std::fwrite(buf.data(), 1, got, out);
        left -= (uint32_t)got;
    }

    std::fclose(in);
    std::fclose(out);

    std::printf("wrote %s\n", argv[2]);
    std::printf("  centre %.6f MHz, %u S/s, %u frames\n",
                center / 1e6, r.sampleRate, r.dataBytes / 8);
    if (r.havePhasing && !r.phasing.names.empty()) {
        std::printf("  channels:");
        for (const auto& n : r.phasing.names) { std::printf(" '%s'", n.c_str()); }
        std::printf("\n");
    }
    if (r.havePhasing && !r.phasing.phaseCoherent) {
        std::printf("  NOTE: these channels are not phase coherent. The Linrad header cannot\n"
                    "        record that, so keep the .wav -- it is the only copy that says so.\n");
    }
    return 0;
}
