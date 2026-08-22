// Checks for dsp::filter::ParametricEQ (core/src/dsp/filter/parametric_eq.h) and the two
// shelf filter types dsp::filter::Biquad (core/src/dsp/filter/biquad.h) gained for it --
// setPeaking()/setLowPass() are already covered by test_tube_warmth.cpp, which predates this
// file and originally owned the Biquad struct before it was pulled out into its own header.
//
//   c++ -std=c++17 -O2 -I<repo>/core/src -I/opt/homebrew/include \
//       -o /tmp/test_parametric_eq core/test/test_parametric_eq.cpp \
//       -L<repo>/build/core -lsdrpp_core -L/opt/homebrew/lib -lvolk -Wl,-rpath,<repo>/build/core

#include <dsp/types.h>
#include <dsp/stream.h>
#include <dsp/filter/parametric_eq.h>
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

static std::vector<float> sine(double freqHz, double sampleRate, int n, float amp = 0.3f) {
    std::vector<float> v(n);
    for (int i = 0; i < n; i++) { v[i] = amp * sinf(2.0f * (float)M_PI * (float)freqHz * (float)i / (float)sampleRate); }
    return v;
}

int main() {
    printf("\nParametricEQ\n\n");

    // -----------------------------------------------------------------
    printf("Biquad shelf filters\n");
    {
        // Low shelf: boosts well below f0, leaves well above it untouched.
        Biquad lowLo, lowHi;
        lowLo.setLowShelf(200.0, 0.707, 6.0, 48000.0);
        lowHi.setLowShelf(200.0, 0.707, 6.0, 48000.0);
        const int n = 8000;
        auto below = sine(50.0, 48000.0, n);
        auto above = sine(4000.0, 48000.0, n);
        std::vector<float> belowOut(n), aboveOut(n);
        for (int i = 0; i < n; i++) { belowOut[i] = lowLo.process(below[i]); }
        for (int i = 0; i < n; i++) { aboveOut[i] = lowHi.process(above[i]); }
        const double belowGain = rms(belowOut, n / 2) / rms(below, n / 2);
        const double aboveGain = rms(aboveOut, n / 2) / rms(above, n / 2);
        check(std::fabs(belowGain - std::pow(10.0, 6.0 / 20.0)) < 0.05,
              "50Hz (well below a 200Hz low shelf) gets the full +6dB");
        check(std::fabs(aboveGain - 1.0) < 0.05, "4kHz (well above it) is left close to untouched");

        // High shelf: the mirror image.
        Biquad highLo, highHi;
        highLo.setHighShelf(3000.0, 0.707, -6.0, 48000.0);
        highHi.setHighShelf(3000.0, 0.707, -6.0, 48000.0);
        auto lo = sine(200.0, 48000.0, n);
        auto hi = sine(8000.0, 48000.0, n);
        std::vector<float> loOut(n), hiOut(n);
        for (int i = 0; i < n; i++) { loOut[i] = highLo.process(lo[i]); }
        for (int i = 0; i < n; i++) { hiOut[i] = highHi.process(hi[i]); }
        const double loGain = rms(loOut, n / 2) / rms(lo, n / 2);
        const double hiGain = rms(hiOut, n / 2) / rms(hi, n / 2);
        check(std::fabs(loGain - 1.0) < 0.05, "200Hz (well below a 3kHz high shelf) is left close to untouched");
        check(std::fabs(hiGain - std::pow(10.0, -6.0 / 20.0)) < 0.05,
              "8kHz (well above it) gets the full -6dB cut");
    }

    // -----------------------------------------------------------------
    printf("\nFlat EQ (every band at 0dB) is close to a passthrough\n");
    {
        ParametricEQ<float> eq;
        eq.init(nullptr, 48000.0);
        // A broadband-ish signal (several tones spanning the band the 6 default frequencies
        // cover), not just one -- a single well-chosen tone could accidentally sit exactly at
        // a filter's own zero-crossing and pass even a genuinely broken flat response.
        const int n = 4000;
        std::vector<float> in(n), out(n);
        for (int i = 0; i < n; i++) {
            float t = (float)i / 48000.0f;
            in[i] = 0.1f * (sinf(2.0f * (float)M_PI * 150.0f * t) + sinf(2.0f * (float)M_PI * 900.0f * t) +
                            sinf(2.0f * (float)M_PI * 3000.0f * t) + sinf(2.0f * (float)M_PI * 7000.0f * t));
        }
        eq.process(n, in.data(), out.data());
        double maxErr = 0.0;
        for (int i = n / 2; i < n; i++) { maxErr = (std::max)(maxErr, (double)std::fabs(out[i] - in[i])); }
        check(maxErr < 1e-4, "output tracks the input closely once every band settles (max error < 1e-4)");
    }

    // -----------------------------------------------------------------
    printf("\nA single boosted band shapes only its own neighborhood\n");
    {
        ParametricEQ<float> eq;
        eq.init(nullptr, 48000.0);
        // Band 3 (index 3) defaults to 2000Hz -- boost it, leave everything else flat.
        double f; float g; double q;
        eq.getBand(3, f, g, q);
        eq.setBand(3, f, 9.0f, 1.2);

        const int n = 8000;
        auto atBand = sine(f, 48000.0, n);
        auto farFromBand = sine(150.0, 48000.0, n);   // near band 0's default, left at 0dB
        std::vector<float> atOut(n), farOut(n);
        eq.process(n, atBand.data(), atOut.data());
        ParametricEQ<float> eq2;
        eq2.init(nullptr, 48000.0);
        eq2.setBand(3, f, 9.0f, 1.2);
        eq2.process(n, farFromBand.data(), farOut.data());

        const double atGain = rms(atOut, n / 2) / rms(atBand, n / 2);
        const double farGain = rms(farOut, n / 2) / rms(farFromBand, n / 2);
        check(atGain > 2.0, "a tone at the boosted band's frequency comes out clearly louder");
        check(std::fabs(farGain - 1.0) < 0.1, "a tone far from it is left close to untouched");
    }

    // -----------------------------------------------------------------
    printf("\nStereo channels stay independent, including across a live band change\n");
    {
        ParametricEQ<stereo_t> eq;
        eq.init(nullptr, 48000.0);
        eq.setBand(2, 800.0, 8.0f, 1.0);

        const int n = 4000;
        std::vector<stereo_t> in(n), out(n);
        for (int i = 0; i < n; i++) { in[i] = { 0.2f, 0.6f }; }   // quiet left, loud right
        eq.process(n, in.data(), out.data());
        check(out.back().l != out.back().r, "differently-driven channels stay different after processing");

        // Simulate a live knob drag mid-stream (setBand() while samples keep flowing) -- the
        // bug this specifically guards against: naively copying the whole left Biquad struct
        // onto the right one to keep their coefficients in sync would also drag the left
        // channel's running filter *state* onto the right channel, corrupting it.
        eq.setBand(2, 900.0, 4.0f, 1.0);
        std::vector<stereo_t> in2(n), out2(n);
        for (int i = 0; i < n; i++) { in2[i] = { 0.1f, 0.9f }; }
        eq.process(n, in2.data(), out2.data());
        check(std::fabs(out2.back().l) < std::fabs(out2.back().r),
              "after a live band change, the quieter channel is still the quieter one");

        // Cross-check against two independent mono instances at the same final band settings
        // and the same per-channel level, confirming the stereo result isn't just "plausible"
        // but numerically matches running each channel completely on its own.
        ParametricEQ<float> monoL, monoR;
        monoL.init(nullptr, 48000.0); monoL.setBand(2, 800.0, 8.0f, 1.0); monoL.setBand(2, 900.0, 4.0f, 1.0);
        monoR.init(nullptr, 48000.0); monoR.setBand(2, 800.0, 8.0f, 1.0); monoR.setBand(2, 900.0, 4.0f, 1.0);
        std::vector<float> monoInL(n, 0.2f), monoInR(n, 0.6f), monoOutL(n), monoOutR(n);
        monoL.process(n, monoInL.data(), monoOutL.data());
        monoR.process(n, monoInR.data(), monoOutR.data());
        std::vector<float> monoInL2(n, 0.1f), monoInR2(n, 0.9f), monoOutL2(n), monoOutR2(n);
        monoL.process(n, monoInL2.data(), monoOutL2.data());
        monoR.process(n, monoInR2.data(), monoOutR2.data());
        check(std::fabs(out2.back().l - monoOutL2.back()) < 1e-4f,
              "stereo left matches an independently-run mono instance at the same settings");
        check(std::fabs(out2.back().r - monoOutR2.back()) < 1e-4f,
              "stereo right matches its own independent mono instance too");
    }

    // -----------------------------------------------------------------
    printf("\ngetBand() reflects what setBand() was actually given, clamped to documented bounds\n");
    {
        ParametricEQ<float> eq;
        eq.init(nullptr, 48000.0);

        eq.setBand(1, 500.0, 3.5f, 0.9);
        double f; float g; double q;
        eq.getBand(1, f, g, q);
        check(f == 500.0 && g == 3.5f && q == 0.9, "an in-range band round-trips exactly");

        eq.setBand(1, 999999.0, 100.0f, 100.0);
        eq.getBand(1, f, g, q);
        check(f <= 20000.0 && g <= 18.0f && q <= 10.0, "an out-of-range band is clamped, not passed through raw");
    }

    printf("\n%s (%d failure%s)\n\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
