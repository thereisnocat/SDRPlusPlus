// Standalone checks for the phasing test source's signal model.
//
//   c++ -std=c++17 -O2 -o /tmp/phtest test/test_signal_model.cpp && /tmp/phtest
//
// No SDR++ dependency, no build system. These verify the one property the module
// exists to provide: that the reported nulling weight actually nulls, so later phases
// can be scored against a known right answer.

#include "../src/signal_model.h"
#include <cstdio>
#include <vector>
#include <string>

using namespace phtest;

static int failures = 0;

static void check(bool cond, const std::string& what) {
    printf("  %-58s %s\n", what.c_str(), cond ? "ok" : "FAIL");
    if (!cond) { failures++; }
}

// Mean power of an interleaved complex buffer, in dB.
static double powerDb(const std::vector<float>& buf) {
    double acc = 0.0;
    const int n = (int)buf.size() / 2;
    for (int i = 0; i < n; i++) {
        acc += (double)buf[2 * i] * buf[2 * i] + (double)buf[2 * i + 1] * buf[2 * i + 1];
    }
    return 10.0 * std::log10(acc / n + 1e-300);
}

// Generate `count` samples under `p` and return their mean power in dB.
static double runPower(Params p, int count = 65536) {
    Generator g;
    g.reset();
    std::vector<float> buf(2 * count);
    g.generate(p, count, buf.data());
    return powerDb(buf);
}

// Set the combiner to the weight that should null the given tone.
static bool armNull(Params& p, bool interferer) {
    std::complex<double> w;
    if (!nullWeight(p, interferer, w)) { return false; }
    p.combGain = 20.0 * std::log10(std::abs(w));
    p.combPhase = std::arg(w) * (180.0 / phtest::PI);
    p.view = VIEW_COMBINED;
    return true;
}

static Params clean() {
    Params p;
    p.noiseEnabled = false;
    p.delaySamples = 0.0;
    return p;
}

int main() {
    printf("\nphasing test source -- signal model checks\n\n");

    // ---------------------------------------------------------------------
    printf("Nulling with the reported weight\n");
    {
        // Interferer only, so residual power is purely the null depth.
        Params p = clean();
        p.wantedEnabled = false;
        const double before = runPower(p);
        check(armNull(p, true), "null weight is defined for the interferer");
        const double after = runPower(p);
        printf("        interferer %.1f dB -> %.1f dB (%.1f dB of cancellation)\n",
               before, after, before - after);
        check(before - after > 120.0, "interferer cancelled by >120 dB");
    }
    {
        Params p = clean();
        p.interfEnabled = false;
        const double before = runPower(p);
        check(armNull(p, false), "null weight is defined for the wanted signal");
        const double after = runPower(p);
        printf("        wanted     %.1f dB -> %.1f dB (%.1f dB of cancellation)\n",
               before, after, before - after);
        check(before - after > 120.0, "wanted signal cancelled by >120 dB");
    }

    // ---------------------------------------------------------------------
    printf("\nNulling one tone leaves the other standing\n");
    {
        Params p = clean();
        p.wantedLevel = -20.0;
        p.interfLevel = -10.0;
        armNull(p, true);
        const double after = runPower(p);
        // Only the wanted tone should survive, scaled by |1 - w*g_wanted|.
        std::complex<double> w = weightFromPolar(p.combGain, p.combPhase);
        ToneWeights tw = toneWeights(p, false);
        const double expected = 20.0 * std::log10(std::abs(tw.a - w * tw.b));
        printf("        residual %.2f dB, predicted %.2f dB\n", after, expected);
        check(std::abs(after - expected) < 0.01, "residual matches the wanted tone alone");
        check(after > -40.0, "wanted tone is not collaterally destroyed");
    }

    // ---------------------------------------------------------------------
    printf("\nA/B swap inverts the null weight\n");
    {
        Params a = clean();
        a.wantedEnabled = false;
        std::complex<double> w1, w2;
        nullWeight(a, true, w1);
        a.swapChannels = true;
        nullWeight(a, true, w2);
        printf("        unswapped %+.3f dB %+.2f deg, swapped %+.3f dB %+.2f deg\n",
               20.0 * std::log10(std::abs(w1)), std::arg(w1) * 180.0 / phtest::PI,
               20.0 * std::log10(std::abs(w2)), std::arg(w2) * 180.0 / phtest::PI);
        check(std::abs(w1 * w2 - std::complex<double>(1.0, 0.0)) < 1e-9,
              "swapped weight is the reciprocal of the unswapped one");

        // And it still actually nulls, which is the property that matters.
        Params p = a;
        armNull(p, true);
        const double after = runPower(p);
        check(after < -120.0, "swapped channels still null to <-120 dB");
    }

    // ---------------------------------------------------------------------
    printf("\nDelay makes the two tones need different weights (wideband case)\n");
    {
        Params p = clean();
        p.delaySamples = 3.7;  // fractional, exercised exactly via per-tone phase
        std::complex<double> wi, ww;
        nullWeight(p, true, wi);
        nullWeight(p, false, ww);
        const double sep = std::arg(wi / ww) * (180.0 / phtest::PI);
        printf("        interferer %+.2f deg, wanted %+.2f deg, separation %.2f deg\n",
               std::arg(wi) * 180.0 / phtest::PI, std::arg(ww) * 180.0 / phtest::PI, sep);
        check(std::abs(sep) > 1.0, "the two tones demand measurably different weights");

        // A scalar weight can null one but must leave the other.
        armNull(p, true);
        const double after = runPower(p);
        check(after > -40.0, "a single weight cannot null both tones at once");

        // With zero delay the same two tones are still independent, but each is
        // individually nullable -- confirming the delay is what creates the tilt.
        p.delaySamples = 0.0;
        std::complex<double> wi0, ww0;
        nullWeight(p, true, wi0);
        nullWeight(p, false, ww0);
        check(std::abs(std::abs(wi0) - std::abs(ww0)) > 1e-9 || std::abs(std::arg(wi0 / ww0)) > 1e-9,
              "with no delay the tones still differ (by their own gain/phase)");
    }

    // ---------------------------------------------------------------------
    printf("\nNoise sets the achievable null floor\n");
    {
        Params p = clean();
        p.wantedEnabled = false;
        p.noiseEnabled = true;
        p.noiseLevel = -80.0;
        armNull(p, true);
        const double after = runPower(p);
        printf("        residual with -80 dBFS noise: %.1f dB\n", after);
        check(after > -85.0 && after < -73.0, "residual sits at the noise floor, not below");
    }
    {
        // Noise level calibration: with no signals, measured power should equal the
        // stated dBFS. This is what makes tone and noise levels comparable.
        Params p = clean();
        p.wantedEnabled = false;
        p.interfEnabled = false;
        p.noiseEnabled = true;
        p.noiseLevel = -40.0;
        const double m = runPower(p);
        printf("        stated -40.0 dBFS, measured %.2f dB\n", m);
        check(std::abs(m - (-40.0)) < 0.5, "noise power matches its label");
    }

    // ---------------------------------------------------------------------
    printf("\nTone level calibration\n");
    {
        Params p = clean();
        p.interfEnabled = false;
        p.wantedLevel = -20.0;
        p.view = VIEW_A;
        const double m = runPower(p);
        printf("        stated -20.0 dBFS, measured %.3f dB\n", m);
        check(std::abs(m - (-20.0)) < 0.01, "tone power matches its label");
    }

    // ---------------------------------------------------------------------
    printf("\nPhasors stay on the unit circle over a long run\n");
    {
        Params p = clean();
        Generator g;
        g.reset();
        std::vector<float> buf(2 * 8192);
        for (int i = 0; i < 2000; i++) { g.generate(p, 8192, buf.data()); }
        const double mw = std::sqrt(g.wantedPhasor.re * g.wantedPhasor.re + g.wantedPhasor.im * g.wantedPhasor.im);
        const double mi = std::sqrt(g.interfPhasor.re * g.interfPhasor.re + g.interfPhasor.im * g.interfPhasor.im);
        printf("        after 16.4 Msamples: |wanted| = %.12f, |interferer| = %.12f\n", mw, mi);
        check(std::abs(mw - 1.0) < 1e-9 && std::abs(mi - 1.0) < 1e-9, "no magnitude drift");
    }

    printf("\n%s (%d failure%s)\n\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
