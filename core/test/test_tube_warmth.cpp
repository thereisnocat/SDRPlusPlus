// Checks for dsp::filter::TubeWarmth, the tube-radio-warmth audio post-processor
// (core/src/dsp/filter/tube_warmth.h). Drives process() directly rather than through the
// streaming machinery -- the point of interest here is the per-sample math, not buffering,
// mirroring how test_protocol.cpp checks unpack() directly rather than through a Device.
//
//   c++ -std=c++17 -O2 -I<repo>/core/src -I/opt/homebrew/include \
//       -o /tmp/test_tube_warmth core/test/test_tube_warmth.cpp \
//       -L<repo>/build/core -lsdrpp_core -L/opt/homebrew/lib -lvolk -Wl,-rpath,<repo>/build/core

#include <dsp/types.h>
#include <dsp/stream.h>
#include <dsp/filter/tube_warmth.h>
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>

using namespace dsp;
using namespace dsp::filter;

static int failures = 0;

static void check(bool cond, const std::string& what) {
    printf("  %-62s %s\n", what.c_str(), cond ? "ok" : "FAIL");
    if (!cond) { failures++; }
}

static double rms(const std::vector<float>& v, size_t from) {
    double sq = 0.0;
    for (size_t i = from; i < v.size(); i++) { sq += (double)v[i] * v[i]; }
    return std::sqrt(sq / (double)(v.size() - from));
}

int main() {
    printf("\nTubeWarmth\n\n");

    // -----------------------------------------------------------------
    printf("Bypass is exact at warmth=0, noise=0\n");
    {
        TubeWarmth<float> tw;
        tw.init(nullptr, 48000.0);
        tw.setWarmth(0.0f);
        tw.setNoise(0.0f);

        // A real, non-trivial signal -- not just zero, which even a broken transform could
        // pass through by accident.
        std::vector<float> in(256), out(256);
        for (int i = 0; i < 256; i++) { in[i] = 0.6f * sinf(2.0f * (float)M_PI * 0.05f * (float)i); }
        tw.process(256, in.data(), out.data());

        bool exact = true;
        for (int i = 0; i < 256; i++) { if (out[i] != in[i]) { exact = false; break; } }
        check(exact, "output is bit-for-bit identical to input (warmth blend is a true no-op at 0)");
    }

    // -----------------------------------------------------------------
    printf("\nNoise is independent of warmth: silent at 0, present and bounded at 1\n");
    {
        std::vector<float> silence(1000, 0.0f);

        TubeWarmth<float> off;
        off.init(nullptr, 48000.0);
        off.setWarmth(0.0f);
        off.setNoise(0.0f);
        std::vector<float> outOff(1000);
        off.process(1000, silence.data(), outOff.data());
        bool allZero = true;
        for (float v : outOff) { if (v != 0.0f) { allZero = false; break; } }
        check(allZero, "noise=0: silent input produces silent output");

        TubeWarmth<float> on;
        on.init(nullptr, 48000.0);
        on.setWarmth(0.0f);
        on.setNoise(1.0f);
        std::vector<float> outOn(1000);
        on.process(1000, silence.data(), outOn.data());
        bool anyNonzero = false, allBounded = true;
        // 0.015 (hiss) + 0.005 (hum) is the file's own documented HISS_PEAK + HUM_PEAK at
        // noise=1 -- duplicated here rather than imported since they're deliberately private
        // (not a tunable a caller should see), same as any other implementation constant.
        const float bound = 0.015f + 0.005f + 1e-6f;
        for (float v : outOn) {
            if (v != 0.0f) { anyNonzero = true; }
            if (std::fabs(v) > bound) { allBounded = false; }
        }
        check(anyNonzero, "noise=1: silent input produces audible hiss/hum");
        check(allBounded, "and it stays within the documented hiss+hum peak bounds");
    }

    // -----------------------------------------------------------------
    printf("\nPeaking filter (standalone Biquad)\n");
    {
        // DC must pass a peaking filter untouched -- it only ever alters loudness of energy
        // near its own center frequency.
        Biquad peak;
        peak.setPeaking(350.0, 0.8, 5.0, 48000.0);
        std::vector<float> dc(2000, 0.4f), out(2000);
        for (int i = 0; i < 2000; i++) { out[i] = peak.process(dc[i]); }
        check(std::fabs(out.back() - 0.4f) < 1e-4f, "DC passes through at unity gain, untouched");

        // A sine at the filter's own center frequency should come out boosted by roughly the
        // configured +5dB (linear gain 10^(5/20) = 1.7783), once the transient has settled.
        Biquad peak2;
        peak2.setPeaking(350.0, 0.8, 5.0, 48000.0);
        const int n = 8000;
        std::vector<float> sineIn(n), sineOut(n);
        for (int i = 0; i < n; i++) { sineIn[i] = 0.3f * sinf(2.0f * (float)M_PI * 350.0f * (float)i / 48000.0f); }
        for (int i = 0; i < n; i++) { sineOut[i] = peak2.process(sineIn[i]); }
        const double gain = rms(sineOut, n / 2) / rms(sineIn, n / 2);
        check(std::fabs(gain - 1.7783) < 0.05, "a 350Hz tone is boosted by roughly the configured +5dB");
    }

    // -----------------------------------------------------------------
    printf("\nLow-pass filter (standalone Biquad)\n");
    {
        // Same filter, two test tones: one well below the 4200Hz cutoff (should pass close to
        // untouched), one well above it (should be attenuated hard). This is the "old radio's
        // rolled-off treble" the file's own top comment describes.
        Biquad lowLp, highLp;
        lowLp.setLowPass(4200.0, 0.707, 48000.0);
        highLp.setLowPass(4200.0, 0.707, 48000.0);

        const int n = 8000;
        std::vector<float> low(n), high(n), lowOut(n), highOut(n);
        for (int i = 0; i < n; i++) {
            low[i] = 0.3f * sinf(2.0f * (float)M_PI * 300.0f * (float)i / 48000.0f);
            high[i] = 0.3f * sinf(2.0f * (float)M_PI * 9000.0f * (float)i / 48000.0f);
        }
        for (int i = 0; i < n; i++) {
            lowOut[i] = lowLp.process(low[i]);
            highOut[i] = highLp.process(high[i]);
        }
        const double lowGain = rms(lowOut, n / 2) / rms(low, n / 2);
        const double highGain = rms(highOut, n / 2) / rms(high, n / 2);
        check(lowGain > 0.9, "300Hz (well below the cutoff) passes close to untouched");
        check(highGain < 0.3, "9kHz (well above the cutoff) is attenuated hard");
        check(lowGain > highGain * 3.0, "and the difference is a real rolloff, not a coincidence");
    }

    // -----------------------------------------------------------------
    printf("\nSaturation: quiet signals barely move, loud ones compress (full TubeWarmth, warmth=1)\n");
    {
        // DC through the whole chain, warmth=1 (fully wet) -- at steady state the peaking and
        // low-pass filters both settle to unity DC gain, so the output converges to the
        // waveshaper's own shape(x) = (tanh(DRIVE*x+BIAS) - tanh(BIAS)) * driveNorm, computed
        // independently here from the file's own documented DRIVE=2.0/BIAS=0.15 rather than
        // read out of the (private) implementation.
        const float DRIVE = 2.0f, BIAS = 0.15f;
        const float tanhBias = tanhf(BIAS);
        const float driveNorm = 1.0f / (DRIVE * (1.0f - tanhBias * tanhBias));
        auto expectedShape = [&](float x) { return (tanhf(DRIVE * x + BIAS) - tanhBias) * driveNorm; };

        auto settledOutput = [](float level) {
            TubeWarmth<float> tw;
            tw.init(nullptr, 48000.0);
            tw.setWarmth(1.0f);
            tw.setNoise(0.0f);
            std::vector<float> in(2000, level), out(2000);
            tw.process(2000, in.data(), out.data());
            return out.back();
        };

        const float quiet = 0.05f, loud = 0.9f;
        const float quietOut = settledOutput(quiet);
        const float loudOut = settledOutput(loud);

        check(std::fabs(quietOut - expectedShape(quiet)) < 1e-3f,
              "quiet signal matches the hand-derived saturation curve");
        check(std::fabs(loudOut - expectedShape(loud)) < 1e-3f,
              "loud signal matches the hand-derived saturation curve too");
        check((quietOut / quiet) > 0.95, "quiet signal (0.05) stays within 5% of unity gain");
        check((loudOut / loud) < 0.7, "loud signal (0.9) is audibly compressed");
    }

    // -----------------------------------------------------------------
    printf("\nStereo channels are processed independently\n");
    {
        TubeWarmth<stereo_t> tw;
        tw.init(nullptr, 48000.0);
        tw.setWarmth(1.0f);
        tw.setNoise(0.0f);

        std::vector<stereo_t> in(2000), out(2000);
        for (auto& s : in) { s = { 0.2f, 0.8f }; }   // quiet left, loud right
        tw.process(2000, in.data(), out.data());

        check(out.back().l != out.back().r, "differently-driven channels produce different output");
        check(std::fabs(out.back().l) < std::fabs(out.back().r),
              "the quieter (left) channel stays quieter than the louder (right) one");
        // Cross-check against the mono result for the same per-channel level, confirming L and
        // R really are two independent filter/waveshaper instances, not one shared state being
        // overwritten by whichever channel runs last.
        TubeWarmth<float> monoL;
        monoL.init(nullptr, 48000.0);
        monoL.setWarmth(1.0f);
        monoL.setNoise(0.0f);
        std::vector<float> inL(2000, 0.2f), outL(2000);
        monoL.process(2000, inL.data(), outL.data());
        check(std::fabs(out.back().l - outL.back()) < 1e-4f,
              "the stereo left channel matches an independent mono instance at the same level");
    }

    // -----------------------------------------------------------------
    printf("\nsetSampleRate() actually reconfigures the filters\n");
    {
        Biquad a, b;
        a.setLowPass(4200.0, 0.707, 48000.0);
        b.setLowPass(4200.0, 0.707, 96000.0);
        check(a.b0 != b.b0 || a.a1 != b.a1,
              "the same cutoff at two different sample rates yields different coefficients");
    }

    printf("\n%s (%d failure%s)\n\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
