// Regression test for a real bug: the "Reference band" control was unreachable from the UI
// while either Decorrelate mode was selected (misc_modules/phasing had it gated behind
// `combining && !decorrelating`), so decorrelation always ran RefBand::accumulateWideband()
// over the whole tuned span rather than the narrow band around one carrier that the
// Perseus22 manual specifies and PHASING_PLAN.md section 2.6 already documented as
// necessary. The eigen math itself was never wrong -- see test_decorrelation.cpp -- what
// was wrong is what band the covariance got computed over. Fixed in misc_modules/phasing;
// this proves the mechanism using the project's own RefBand/decorrelator code rather than a
// re-derivation, on a scene that looks like a real medium wave band rather than the clean
// two-signal scene test_decorrelation.cpp uses.
//
//   c++ -std=c++17 -O2 -I<repo>/core/src -o /tmp/t test_crowded_band.cpp && /tmp/t
#include <dsp/types.h>
#include <dsp/combine/ref_band.h>
#include <dsp/combine/decorrelator.h>
#include <complex>
#include <random>
#include <vector>
#include <cstdio>
#include <cmath>

using namespace dsp::combine;

static int failures = 0;

static void check(bool cond, const std::string& what) {
    printf("  %-70s %s\n", what.c_str(), cond ? "ok" : "FAIL");
    if (!cond) { failures++; }
}

static double gainDbFor(std::complex<double> k0, std::complex<double> k1, std::complex<double> ratio) {
    return 20.0 * std::log10(std::max(std::abs(k0 + k1 * ratio), 1e-30));
}

int main() {
    printf("\nDecorrelation on a crowded band\n\n");

    const double sr = 200000.0;
    const int N = 2000000;

    // L = the station on the dial (a WNYC stand-in), at DC -- the DDC is tuned to it.
    // D = a weak DX signal buried under it, same frequency, different arrival ratio.
    // X = an unrelated, LOUDER station elsewhere in the visible span. This is what a real
    //     crowded MW band looks like: several signals, most of them irrelevant to the one
    //     the operator is trying to null, and not necessarily the quietest of the bunch.
    const double offL = 0.0, offD = 0.0, offX = 70000.0;
    const std::complex<double> ratioL = std::polar(0.7, 2.39);   // -3 dB, 137 deg
    const std::complex<double> ratioD = std::polar(1.3, -0.9);
    const std::complex<double> ratioX = std::polar(1.1, 0.4);    // an unrelated direction
    const double ampL = 1.0, ampD = 0.05, ampX = 2.5;            // X dominates the WHOLE
                                                                  // span; L merely dominates
                                                                  // at its OWN frequency.

    std::mt19937 rng(11);
    std::normal_distribution<double> g(0.0, 1.0);

    std::vector<dsp::complex_t> a(N), b(N);
    double phL = 0, phD = 0, phX = 0;
    const double wL = 2.0 * M_PI * offL / sr, wD = 2.0 * M_PI * offD / sr, wX = 2.0 * M_PI * offX / sr;
    for (int i = 0; i < N; i++) {
        const std::complex<double> sL = std::polar(ampL, phL) * std::complex<double>(g(rng) * 0.3 + 1.0, 0);
        const std::complex<double> sD = std::polar(ampD, phD) * std::complex<double>(g(rng) * 0.3 + 1.0, 0);
        const std::complex<double> sX = std::polar(ampX, phX) * std::complex<double>(g(rng) * 0.3 + 1.0, 0);
        const std::complex<double> na(g(rng) * 0.01, g(rng) * 0.01), nb(g(rng) * 0.01, g(rng) * 0.01);

        const std::complex<double> av = sL + sD + sX + na;
        const std::complex<double> bv = sL * ratioL + sD * ratioD + sX * ratioX + nb;
        a[i] = { (float)av.real(), (float)av.imag() };
        b[i] = { (float)bv.real(), (float)bv.imag() };

        phL += wL; phD += wD; phX += wX;
    }

    const double rawLDb = 20.0 * std::log10(std::abs(ratioL));

    // -- Wideband: no reference band -- what the UI shipped as the only reachable path for
    //    decorrelation modes before this fix, since refEnabled defaults to false. --
    double lMinWide, lMaxWide;
    {
        Covariance cov;
        std::complex<double> rab(0, 0); double rbb = 0, raa = 0;
        RefBand::accumulateWideband(a.data(), b.data(), N, rab, rbb, &raa);
        cov.raa = raa / N; cov.rbb = rbb / N; cov.rab = rab / (double)N;

        const Eigen2 e = solveEigen2(cov);
        Matrix2 none;
        std::complex<double> k0, k1;
        combineCoefficients(e.uMin, none, false, k0, k1);
        lMinWide = gainDbFor(k0, k1, ratioL);
        const double xMinWide = gainDbFor(k0, k1, ratioX);
        combineCoefficients(e.uMax, none, false, k0, k1);
        lMaxWide = gainDbFor(k0, k1, ratioL);

        printf("  wideband: L via Null %+.1f dB, L via Peak %+.1f dB, X via Null %+.1f dB\n",
               lMinWide, lMaxWide, xMinWide);

        // This is the bug, demonstrated: "null strongest" nulls X, the louder station
        // elsewhere in the span, and barely touches L -- the one on the dial.
        check(lMinWide > rawLDb - 6.0, "wideband: L is barely touched by 'null strongest'");
        check(xMinWide < -15.0, "wideband: X, not L, is what actually got nulled");
        // The exact backward symptom reported: L comes through LOUDER under Null than Peak.
        check(lMinWide > lMaxWide, "wideband: L is louder via Null than via Peak (the reported bug)");
    }

    // -- Reference band centered on L, wide enough for L+D, narrow enough to exclude X. --
    double lMinBand;
    {
        RefBand rb;
        rb.configure(sr, 0.0, 4000.0);
        Covariance cov;
        std::complex<double> rab(0, 0); double rbb = 0, raa = 0;
        const int terms = rb.accumulate(a.data(), b.data(), N, rab, rbb, &raa);
        cov.raa = raa / terms; cov.rbb = rbb / terms; cov.rab = rab / (double)terms;

        const Eigen2 e = solveEigen2(cov);
        Matrix2 none;
        std::complex<double> k0, k1;
        combineCoefficients(e.uMin, none, false, k0, k1);
        lMinBand = gainDbFor(k0, k1, ratioL);
        combineCoefficients(e.uMax, none, false, k0, k1);
        const double lMaxBand = gainDbFor(k0, k1, ratioL);

        printf("  4 kHz band on L: L via Null %+.1f dB, L via Peak %+.1f dB\n", lMinBand, lMaxBand);

        // With the band correctly narrowed, the bug's symptom is gone: L is what gets
        // nulled, and Null buries L well below Peak, the right way around.
        check(lMinBand < rawLDb - 10.0, "banded: L is meaningfully nulled by 'null strongest'");
        check(lMinBand < lMaxBand - 10.0, "banded: L is quieter via Null than via Peak, correctly");
    }

    check(lMinBand < lMinWide - 15.0,
          "narrowing the reference band is what turns a non-null into a real one");

    printf("\n%s (%d failure%s)\n\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
