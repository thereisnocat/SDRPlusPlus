// Round-trip checks for dual-channel recording: the metadata chunks, the four-channel
// interleave, and the fact that a recording survives being written and read back.
//
// Needs the core library, so build that first (cmake --build build), then:
//
//   c++ -std=c++17 -O2 -I<repo>/core/src -I/opt/homebrew/include \
//       -o /tmp/test_wav_meta core/test/test_wav_meta.cpp \
//       -L<repo>/build/core -lsdrpp_core -L/opt/homebrew/lib -lvolk -Wl,-rpath,<repo>/build/core
//
// The metadata is embedded rather than kept in a sidecar because these recordings are
// kept for years (PHASING_PLAN.md section 4.2), so "does it come back out of the file
// intact" is the property that matters most here.

#include <utils/wav.h>
#include <utils/wav_meta.h>
#include <dsp/types.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>
#include <cmath>
#include <fstream>
#include <map>

static int failures = 0;

static void check(bool cond, const std::string& what) {
    printf("  %-58s %s\n", what.c_str(), cond ? "ok" : "FAIL");
    if (!cond) { failures++; }
}

// Minimal RIFF walker, so the test reads the file the way a stranger's tool would rather
// than trusting the writer's own bookkeeping.
static std::map<std::string, std::vector<uint8_t>> readChunks(const std::string& path, size_t* dataOffset, uint32_t* dataSize) {
    std::map<std::string, std::vector<uint8_t>> out;
    std::ifstream f(path, std::ios::binary);
    char id[4];
    uint32_t sz;
    f.read(id, 4);
    f.read((char*)&sz, 4);
    f.read(id, 4);   // "WAVE"
    while (f.read(id, 4)) {
        if (!f.read((char*)&sz, 4)) { break; }
        std::string cid(id, 4);
        if (cid == "data") {
            *dataOffset = (size_t)f.tellg();
            *dataSize = sz;
            break;
        }
        std::vector<uint8_t> buf(sz);
        f.read((char*)buf.data(), sz);
        if (sz & 1) { f.seekg(1, std::ios::cur); }
        out[cid] = std::move(buf);
    }
    return out;
}

int main() {
    const std::string path = "/tmp/sdrpp_dual_test.wav";
    const int frames = 8192;
    const double sampleRate = 192000.0;
    const double centerFreq = 6925000.0;

    printf("\nDual-channel recording round trip\n\n");

    // Two distinguishable channels: A a tone, B the same tone scaled and rotated.
    std::vector<dsp::complex_t> chA(frames), chB(frames);
    for (int i = 0; i < frames; i++) {
        const double ph = 2.0 * M_PI * 0.01 * i;
        chA[i] = { (float)(0.5 * std::cos(ph)), (float)(0.5 * std::sin(ph)) };
        chB[i] = { (float)(0.25 * std::cos(ph + 1.0)), (float)(0.25 * std::sin(ph + 1.0)) };
    }

    // ---------------------------------------------------------------- write
    {
        wav::Writer w;
        w.setChannels(4);
        w.setSampleType(wav::SAMP_TYPE_FLOAT32);
        w.setSamplerate((uint64_t)sampleRate);

        const std::time_t t0 = 1761763200;   // 2025-10-29 16:00:00 UTC
        wavmeta::AuxiChunk auxi = wavmeta::makeAuxi(centerFreq, sampleRate, t0, t0 + 42);
        w.addChunk("auxi", &auxi, sizeof(auxi));

        wavmeta::PhasingInfo info;
        info.channelCount = 2;
        info.phaseCoherent = true;
        info.sampleAligned = true;
        info.combinedA = 0;
        info.combinedB = 1;
        info.names = { "HF1", "HF2" };
        auto blob = wavmeta::serializePhasing(info);
        w.addChunk(wavmeta::PHASING_CHUNK_ID, blob.data(), blob.size());

        check(w.open(path), "file opens for writing");

        std::vector<float> interleaved(frames * 4);
        for (int i = 0; i < frames; i++) {
            interleaved[4 * i + 0] = chA[i].re;
            interleaved[4 * i + 1] = chA[i].im;
            interleaved[4 * i + 2] = chB[i].re;
            interleaved[4 * i + 3] = chB[i].im;
        }
        w.write(interleaved.data(), frames);
        w.close();
    }

    // ---------------------------------------------------------------- read
    size_t dataOffset = 0;
    uint32_t dataSize = 0;
    auto chunks = readChunks(path, &dataOffset, &dataSize);

    printf("\nChunks present and in the right order\n");
    check(chunks.count("fmt ") == 1, "fmt chunk written");
    check(chunks.count("auxi") == 1, "auxi chunk written");
    check(chunks.count("sdpc") == 1, "phasing chunk written");
    check(dataSize == (uint32_t)(frames * 4 * sizeof(float)), "data chunk sized for 4 channels");

    {
        // fmt must say four channels or nothing else will read the file correctly.
        const auto& f = chunks["fmt "];
        uint16_t channelCount;
        uint32_t rate;
        memcpy(&channelCount, f.data() + 2, 2);
        memcpy(&rate, f.data() + 4, 4);
        printf("        fmt: %u channels at %u Hz\n", channelCount, rate);
        check(channelCount == 4, "fmt declares 4 channels");
        check(rate == (uint32_t)sampleRate, "fmt declares the sample rate");
    }

    printf("\nauxi survives the round trip\n");
    {
        const auto& a = chunks["auxi"];
        check(a.size() == 164, "auxi is the expected 164 bytes");
        wavmeta::AuxiChunk parsed;
        memcpy(&parsed, a.data(), sizeof(parsed));
        printf("        start %04u-%02u-%02u %02u:%02u:%02u dow=%u, centre %u Hz\n",
               parsed.startTime.year, parsed.startTime.month, parsed.startTime.day,
               parsed.startTime.hour, parsed.startTime.minute, parsed.startTime.second,
               parsed.startTime.dayOfWeek, parsed.centerFreq);
        check(parsed.centerFreq == (uint32_t)centerFreq, "centre frequency preserved");
        check(parsed.startTime.year == 2025 && parsed.startTime.month == 10 && parsed.startTime.day == 29,
              "start date preserved");
        check(parsed.startTime.dayOfWeek == 3, "day of week is correct (2025-10-29 was a Wednesday)");
        check(parsed.stopTime.second == parsed.startTime.second + 42 % 60 || parsed.stopTime.minute != parsed.startTime.minute,
              "stop time differs from start");
        check(parsed.adFrequency == (uint32_t)sampleRate, "sample rate recorded in auxi");
    }

    printf("\nPhasing chunk survives the round trip\n");
    {
        const auto& c = chunks["sdpc"];
        wavmeta::PhasingInfo got;
        check(wavmeta::parsePhasing(c.data(), c.size(), got), "phasing chunk parses");
        check(got.channelCount == 2, "channel count preserved");
        check(got.phaseCoherent && got.sampleAligned, "flags preserved");
        check(got.names.size() == 2, "two channel names present");
        if (got.names.size() == 2) {
            printf("        names: '%s', '%s'\n", got.names[0].c_str(), got.names[1].c_str());
            check(got.names[0] == "HF1" && got.names[1] == "HF2", "channel names preserved");
        }
    }

    printf("\nSamples survive the round trip\n");
    {
        std::ifstream f(path, std::ios::binary);
        f.seekg(dataOffset);
        std::vector<float> raw(frames * 4);
        f.read((char*)raw.data(), raw.size() * sizeof(float));

        double errA = 0.0, errB = 0.0;
        for (int i = 0; i < frames; i++) {
            errA = std::max(errA, (double)std::abs(raw[4 * i + 0] - chA[i].re));
            errA = std::max(errA, (double)std::abs(raw[4 * i + 1] - chA[i].im));
            errB = std::max(errB, (double)std::abs(raw[4 * i + 2] - chB[i].re));
            errB = std::max(errB, (double)std::abs(raw[4 * i + 3] - chB[i].im));
        }
        printf("        worst error: channel A %.3g, channel B %.3g\n", errA, errB);
        check(errA == 0.0 && errB == 0.0, "float32 round trip is exact");

        // The channels must not have been swapped or mixed: B is a quarter amplitude.
        double peakA = 0.0, peakB = 0.0;
        for (int i = 0; i < frames; i++) {
            peakA = std::max(peakA, (double)std::hypot(raw[4 * i + 0], raw[4 * i + 1]));
            peakB = std::max(peakB, (double)std::hypot(raw[4 * i + 2], raw[4 * i + 3]));
        }
        printf("        peak |A| %.3f, peak |B| %.3f\n", peakA, peakB);
        check(std::abs(peakA - 0.5) < 1e-4 && std::abs(peakB - 0.25) < 1e-4,
              "channels land in the right slots, not swapped");
    }

    printf("\nA plain 2-channel recording is unaffected\n");
    {
        const std::string p2 = "/tmp/sdrpp_mono_test.wav";
        wav::Writer w;
        w.setChannels(2);
        w.setSampleType(wav::SAMP_TYPE_FLOAT32);
        w.setSamplerate(48000);
        w.open(p2);
        std::vector<float> buf(256 * 2, 0.25f);
        w.write(buf.data(), 256);
        w.close();

        size_t off = 0;
        uint32_t sz = 0;
        auto c2 = readChunks(p2, &off, &sz);
        check(c2.count("auxi") == 0 && c2.count("sdpc") == 0, "no metadata chunks when none were added");
        check(sz == 256 * 2 * sizeof(float), "data chunk sized normally");
        remove(p2.c_str());
    }

    remove(path.c_str());
    printf("\n%s (%d failure%s)\n\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
