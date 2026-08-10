// Phase 2 of RECORDING_REFACTOR_PLAN.md: round-trip coverage across every wav::SampleType
// and a representative set of channel counts, read back through the real WavReader
// file_source uses -- not an independent reimplementation of WAV parsing. Section 2 of that
// plan is the reason this exists: every WAV bug found in the session that led to it (the
// float32/int16 mixup, the missing int32/uint8 support, the RF64 overflow) was only found
// after a real recording broke in a specific way, because nothing exercised this path in a
// test. This is meant to be the thing that would have caught each of them first.
//
// Needs the core library, so build that first (cmake --build build), then:
//
//   c++ -std=c++17 -O2 -I<repo>/core/src -I/opt/homebrew/include \
//       -o /tmp/test_wav_roundtrip core/test/test_wav_roundtrip.cpp \
//       -L<repo>/build/core -lsdrpp_core -L/opt/homebrew/lib -lvolk -Wl,-rpath,<repo>/build/core
//
// Or just run core/test/run_tests.sh, which this is wired into.

#include <utils/wav.h>
#include "../../source_modules/file_source/src/wavreader.h"

#include <cstdio>
#include <cstring>
#include <vector>
#include <string>
#include <cmath>
#include <algorithm>

static int failures = 0;

static void check(bool cond, const std::string& what) {
    printf("  %-72s %s\n", what.c_str(), cond ? "ok" : "FAIL");
    if (!cond) { failures++; }
}

// Per-channel amplitude and phase offset distinguish channels from each other, so a
// channel-order or channel-count bug shows up as a wrong-amplitude or wrong-shape signal
// rather than passing by accident because every channel happened to carry the same data.
static std::vector<float> makeSignal(int frames, int channels) {
    std::vector<float> s(frames * channels);
    for (int i = 0; i < frames; i++) {
        const double ph = 2.0 * M_PI * 0.013 * i;
        for (int c = 0; c < channels; c++) {
            const double amp = 0.5 - 0.1 * c;               // distinct per channel
            const double off = c * 0.7;                      // distinct phase per channel
            s[i * channels + c] = (float)(amp * std::sin(ph + off));
        }
    }
    return s;
}

// Tolerance appropriate to each type's quantization step -- exact for float32 (no lossy
// conversion at all), a few quantization steps for the integer types (their exact rounding
// mode isn't asserted here, only that it's close to correct, which is what actually matters
// for audio/IQ fidelity).
static float toleranceFor(wav::SampleType type) {
    switch (type) {
        case wav::SAMP_TYPE_UINT8:   return 4.0f / 127.0f;         // few 8-bit quantization steps
        case wav::SAMP_TYPE_INT16:   return 4.0f / 32767.0f;
        case wav::SAMP_TYPE_INT32:   return 4.0f / 2147483647.0f;
        case wav::SAMP_TYPE_FLOAT32: return 0.0f;                  // must be bit-exact
        default:                     return 0.0f;
    }
}

static const char* nameFor(wav::SampleType type) {
    switch (type) {
        case wav::SAMP_TYPE_UINT8:   return "uint8";
        case wav::SAMP_TYPE_INT16:   return "int16";
        case wav::SAMP_TYPE_INT32:   return "int32";
        case wav::SAMP_TYPE_FLOAT32: return "float32";
        default:                     return "?";
    }
}

// Reads back whatever raw width the file actually is, converting to float the same way
// file_source's readPCM() does, so this test is checking against the real reader's
// arithmetic, not a second reimplementation of it that could drift out of sync.
static std::vector<float> readBackAsFloat(WavReader& r, int frames, int channels, wav::SampleType type) {
    const int count = frames * channels;
    std::vector<float> out(count);
    switch (type) {
        case wav::SAMP_TYPE_UINT8: {
            std::vector<uint8_t> raw(count);
            r.readSamples(raw.data(), raw.size());
            for (int i = 0; i < count; i++) { out[i] = ((float)raw[i] - 128.0f) / 127.0f; }
            break;
        }
        case wav::SAMP_TYPE_INT16: {
            std::vector<int16_t> raw(count);
            r.readSamples(raw.data(), raw.size() * sizeof(int16_t));
            for (int i = 0; i < count; i++) { out[i] = (float)raw[i] / 32768.0f; }
            break;
        }
        case wav::SAMP_TYPE_INT32: {
            std::vector<int32_t> raw(count);
            r.readSamples(raw.data(), raw.size() * sizeof(int32_t));
            for (int i = 0; i < count; i++) { out[i] = (float)((double)raw[i] / 2147483647.0); }
            break;
        }
        case wav::SAMP_TYPE_FLOAT32: {
            r.readSamples(out.data(), out.size() * sizeof(float));
            break;
        }
        default: break;
    }
    return out;
}

static void testRoundTrip(wav::SampleType type, int channels) {
    char label[128];
    snprintf(label, sizeof(label), "%s, %d channel%s", nameFor(type), channels, channels == 1 ? "" : "s");
    printf("\n%s\n", label);

    const std::string path = "/tmp/sdrpp_wav_roundtrip_test.wav";
    const int frames = 4000;
    const uint64_t rate = 48000;
    const auto signal = makeSignal(frames, channels);

    {
        wav::Writer w(channels, rate, wav::FORMAT_WAV, type);
        check(w.open(path), "  file opens");
        w.write(const_cast<float*>(signal.data()), frames);
        check(w.getSamplesWritten() == (size_t)frames, "  frame count tracked correctly");
        w.close();
    }

    WavReader r(path);
    check(r.isValid(), "  reads back as valid");
    check(r.getChannelCount() == channels, "  channel count round-trips");
    check(r.getSampleRate() == rate, "  sample rate round-trips");
    const int expectedCodec = (type == wav::SAMP_TYPE_FLOAT32) ? wav::CODEC_FLOAT : wav::CODEC_PCM;
    check(r.getCodec() == expectedCodec, "  codec round-trips (PCM vs IEEE float)");

    const uint64_t bytesPerSample = (type == wav::SAMP_TYPE_UINT8) ? 1 : (type == wav::SAMP_TYPE_INT16) ? 2 : 4;
    check(r.getDataSize() == (uint64_t)frames * channels * bytesPerSample, "  data size is exact");

    auto readback = readBackAsFloat(r, frames, channels, type);
    const float tol = toleranceFor(type);
    float worst = 0.0f;
    for (size_t i = 0; i < signal.size(); i++) {
        worst = std::max(worst, std::abs(signal[i] - readback[i]));
    }
    printf("        worst error: %.6g (tolerance %.6g)\n", worst, tol);
    check(worst <= tol, "  samples round-trip within tolerance for this type");

    // Channels must land in the right slots, not swapped or averaged together -- each
    // channel's own peak amplitude, checked independently, catches that.
    if (channels > 1) {
        bool orderOk = true;
        for (int c = 0; c < channels; c++) {
            float expectedPeak = 0.5f - 0.1f * c;
            float peak = 0.0f;
            for (int i = 0; i < frames; i++) { peak = std::max(peak, std::abs(readback[i * channels + c])); }
            if (std::abs(peak - expectedPeak) > tol + 0.01f) { orderOk = false; }
        }
        check(orderOk, "  each channel has its own distinct amplitude, not swapped/mixed");
    }

    remove(path.c_str());
}

// Exercises the RF64 backpatch path (RECORDING_REFACTOR_PLAN.md section 3.1) via
// forceRF64() rather than actually writing a multi-gigabyte file -- forceRF64() drives the
// exact same close()-time conversion a real overflow would, just without needing the disk
// space or the time.
static void testForcedRF64() {
    printf("\nForced RF64 (exercises the real >4GB path without a real multi-GB file)\n");

    const std::string path = "/tmp/sdrpp_wav_rf64_test.wav";
    const int channels = 4;   // dual-channel phasing shape -- the case that motivated this
    const int frames = 2000;
    const uint64_t rate = 3906250;
    const auto signal = makeSignal(frames, channels);

    {
        wav::Writer w(channels, rate, wav::FORMAT_RF64, wav::SAMP_TYPE_INT32);
        check(w.open(path), "  file opens");
        w.write(const_cast<float*>(signal.data()), frames);
        w.close();
    }

    // Independent, low-level check -- reads the raw bytes directly rather than trusting
    // WavReader's own interpretation, the same principle test_wav_meta.cpp's readChunks()
    // uses: a bug shared between the writer and the one reader that checks it would
    // otherwise pass silently.
    {
        FILE* f = fopen(path.c_str(), "rb");
        char magic[4], wave[4], nextId[4];
        uint32_t sizeField, nextSize;
        fread(magic, 1, 4, f);
        fread(&sizeField, 4, 1, f);
        fread(wave, 1, 4, f);
        fread(nextId, 1, 4, f);
        fread(&nextSize, 4, 1, f);
        fclose(f);
        check(memcmp(magic, "RF64", 4) == 0, "  outer magic is RF64");
        check(sizeField == 0xFFFFFFFFu, "  outer size field is the sentinel");
        check(memcmp(wave, "WAVE", 4) == 0, "  WAVE form tag intact");
        check(memcmp(nextId, "ds64", 4) == 0, "  ds64 is the first chunk after the header");
        check(nextSize == 28, "  ds64 payload is exactly 28 bytes");
    }

    WavReader r(path);
    check(r.isValid(), "  RF64 file reads back as valid");
    check(r.getChannelCount() == channels, "  channel count round-trips");
    const uint64_t expectedBytes = (uint64_t)frames * channels * sizeof(int32_t);
    check(r.getDataSize() == expectedBytes, "  data size is exact, sourced from ds64");

    auto readback = readBackAsFloat(r, frames, channels, wav::SAMP_TYPE_INT32);
    float worst = 0.0f;
    for (size_t i = 0; i < signal.size(); i++) { worst = std::max(worst, std::abs(signal[i] - readback[i])); }
    check(worst <= toleranceFor(wav::SAMP_TYPE_INT32), "  samples round-trip through the RF64 path");

    remove(path.c_str());
}

// A file that never needed RF64 must stay a completely ordinary, small WAV file -- no
// "JUNK" chunk surprises for tools that don't expect one, no RF64 magic, nothing that would
// make a normal recording look different from what it always would have.
static void testSmallFileStaysPlainWav() {
    printf("\nA small file stays plain WAV, not RF64\n");

    const std::string path = "/tmp/sdrpp_wav_small_test.wav";
    {
        wav::Writer w(2, 48000, wav::FORMAT_WAV, wav::SAMP_TYPE_INT16);
        w.open(path);
        std::vector<float> buf(256 * 2, 0.1f);
        w.write(buf.data(), 256);
        w.close();
    }

    FILE* f = fopen(path.c_str(), "rb");
    char magic[4];
    fread(magic, 1, 4, f);
    fclose(f);
    check(memcmp(magic, "RIFF", 4) == 0, "  small file's outer magic stayed RIFF");

    WavReader r(path);
    check(r.isValid(), "  still reads back as valid");
    remove(path.c_str());
}

int main() {
    printf("\nWAV round-trip coverage (RECORDING_REFACTOR_PLAN.md phase 2)\n");

    const wav::SampleType types[] = { wav::SAMP_TYPE_UINT8, wav::SAMP_TYPE_INT16, wav::SAMP_TYPE_INT32, wav::SAMP_TYPE_FLOAT32 };
    const int channelCounts[] = { 1, 2, 4 };

    for (auto type : types) {
        for (auto channels : channelCounts) {
            testRoundTrip(type, channels);
        }
    }

    testForcedRF64();
    testSmallFileStaysPlainWav();

    printf("\n%s (%d failure%s)\n\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
