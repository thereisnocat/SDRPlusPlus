// Checks for wideband (multi-tap, per-bin) decorrelation: does solving the eigen split
// independently per FFT bin actually null more than a single global weight can, on a scene
// a scalar weight structurally cannot handle? See PHASING_PLAN.md section 2.6a.
//
// Two stations, L and X, at different frequencies with DIFFERENT inter-channel ratios --
// exactly the crowded-band scene test_crowded_band.cpp uses to demonstrate the reference-
// band bug, reused here because it demonstrates something else: a single global weight can
// only null the direction of whichever ONE station's covariance dominates the estimate. No
// reference band can fix that when there are two stations you would like nulled at once --
// scoping a reference band around L would still leave X untouched, and vice versa. Per-bin
// decorrelation nulls both simultaneously, because each bin's covariance only ever sees the
// station sitting in it.
//
//   c++ -std=c++17 -O2 -I<repo>/core/src -I/opt/homebrew/include \
//       -o /tmp/t core/test/test_wideband_decorrelation.cpp \
//       core/src/dsp/combine/wideband_decorrelator.cpp \
//       -L/opt/homebrew/lib -lfftw3f && /tmp/t
#include <dsp/types.h>
#include <dsp/combine/wideband_decorrelator.h>
#include <dsp/combine/decorrelator.h>
#include <dsp/combine/ref_band.h>
#include <dsp/combine/phaser.h>
#include <dsp/stream.h>
#include <complex>
#include <random>
#include <vector>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <thread>
#include <chrono>

using namespace dsp::combine;
using dsp::complex_t;

static int failures = 0;

static void check(bool cond, const std::string& what) {
    printf("  %-70s %s\n", what.c_str(), cond ? "ok" : "FAIL");
    if (!cond) { failures++; }
}

// Applies a two-filter combine y = (k0 * a) + (k1 * b), both as N-tap FIRs against
// reversed tap arrays -- the same convention WidebandDecorrelator::copyTaps returns.
static void applyTwoFilter(const std::vector<complex_t>& a, const std::vector<complex_t>& b,
                           const std::vector<complex_t>& k0Rev, const std::vector<complex_t>& k1Rev,
                           std::vector<complex_t>& y) {
    const int n = (int)k0Rev.size();
    y.assign(a.size(), { 0.0f, 0.0f });
    for (size_t i = n - 1; i < a.size(); i++) {
        std::complex<double> acc(0, 0);
        for (int k = 0; k < n; k++) {
            const std::complex<double> av(a[i - (n - 1) + k].re, a[i - (n - 1) + k].im);
            const std::complex<double> bv(b[i - (n - 1) + k].re, b[i - (n - 1) + k].im);
            const std::complex<double> k0(k0Rev[k].re, k0Rev[k].im);
            const std::complex<double> k1(k1Rev[k].re, k1Rev[k].im);
            acc += k0 * av + k1 * bv;
        }
        y[i] = { (float)acc.real(), (float)acc.imag() };
    }
}

// Power inside a band centred at offsetHz, using the project's own RefBand so scoring
// matches exactly what the app's null-depth meter would show.
static double bandPower(const std::vector<complex_t>& x, double sampleRate, double offsetHz, double widthHz) {
    RefBand rb; rb.configure(sampleRate, offsetHz, widthHz);
    std::vector<complex_t> zero(x.size(), { 0.0f, 0.0f });
    std::complex<double> rab(0, 0); double rbb = 0, raa = 0;
    rb.accumulate(x.data(), zero.data(), (int)x.size(), rab, rbb, &raa);
    return raa;
}

int main() {
    printf("\nWideband (per-bin) decorrelation\n\n");

    const double sr = 200000.0;
    const int N = 4000000;

    const double offL = 0.0, offD = 0.0, offX = 70000.0;
    const std::complex<double> ratioL = std::polar(0.7, 2.39);
    const std::complex<double> ratioD = std::polar(1.3, -0.9);
    const std::complex<double> ratioX = std::polar(1.1, 0.4);
    const double ampL = 1.0, ampD = 0.05, ampX = 2.5;

    std::mt19937 rng(11);
    std::normal_distribution<double> g(0.0, 1.0);

    std::vector<complex_t> a(N), b(N);
    double phL = 0, phD = 0, phX = 0;
    const double wL = 2.0 * DB_M_PI * offL / sr, wD = 2.0 * DB_M_PI * offD / sr, wX = 2.0 * DB_M_PI * offX / sr;
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

    // ---- Scalar decorrelation, wideband (no reference band): the pre-existing limit. ----
    double scalarL, scalarX;
    {
        Covariance cov;
        std::complex<double> rab(0, 0); double rbb = 0, raa = 0;
        RefBand::accumulateWideband(a.data(), b.data(), N, rab, rbb, &raa);
        cov.raa = raa / N; cov.rbb = rbb / N; cov.rab = rab / (double)N;
        const Eigen2 e = solveEigen2(cov);
        Matrix2 none; std::complex<double> k0, k1;
        combineCoefficients(e.uMin, none, false, k0, k1);
        scalarL = 20.0 * std::log10(std::abs(k0 + k1 * ratioL));
        scalarX = 20.0 * std::log10(std::abs(k0 + k1 * ratioX));
        printf("scalar, no reference band:  L %+.1f dB   X %+.1f dB\n", scalarL, scalarX);
    }

    // ---- Wideband (per-bin) decorrelation: no reference band needed. ----
    WidebandDecorrelator wbd;
    wbd.configure(4096, 64, 0.05f);

    // Both channels need the solver's alignment delay applied BEFORE feeding, not just
    // before applying -- the true K0(f)/K1(f) response is generally two-sided (it can
    // land anywhere in [-taps/2, taps/2) once channels differ by more than a flat ratio,
    // even with no physical timing skew at all, since a piecewise-varying ratio across
    // frequency itself has a spread impulse response). Extracting lags [0, taps) straight
    // from the IFFT only captures that response if it is not the true zero-centred one,
    // so both channels get the shift neither is privileged the way Wiener's A is.
    const int align = wbd.alignmentDelay();
    std::vector<complex_t> aDelayed(N, { 0.0f, 0.0f }), bDelayed(N, { 0.0f, 0.0f });
    for (int i = align; i < N; i++) { aDelayed[i] = a[i - align]; bDelayed[i] = b[i - align]; }

    const int block = 8192;
    for (int i = 0; i + block <= N; i += block) {
        wbd.feed(aDelayed.data() + i, bDelayed.data() + i, block);
    }
    check(wbd.hasSolution(), "the solver converges given enough data");

    // The power gate (section 2.6c) must be doing something, not compiled in as a no-op --
    // and with two real signals plus a lot of empty spectrum in this scene, it should
    // exclude most bins while still keeping enough active to describe both L and X.
    printf("  active bins: %d / %d\n", wbd.activeBinCount(), wbd.fftSize());
    check(wbd.activeBinCount() < wbd.fftSize(), "the gate excludes at least some bins (not a no-op)");
    check(wbd.activeBinCount() >= 4, "and still leaves enough active to describe two stations");

    std::vector<complex_t> k0Rev, k1Rev;
    check(wbd.copyTaps(true, k0Rev, k1Rev), "MIN taps are available");

    std::vector<complex_t> y;
    applyTwoFilter(aDelayed, bDelayed, k0Rev, k1Rev, y);

    // Score well after the FIR has filled and past the transient at the very start.
    const int settleFrom = align + 200000;
    const double pL = bandPower(std::vector<complex_t>(y.begin() + settleFrom, y.end()), sr, offL, 4000.0);
    const double pX = bandPower(std::vector<complex_t>(y.begin() + settleFrom, y.end()), sr, offX, 4000.0);
    const double pLraw = bandPower(std::vector<complex_t>(aDelayed.begin() + settleFrom, aDelayed.end()), sr, offL, 4000.0);
    const double pXraw = bandPower(std::vector<complex_t>(aDelayed.begin() + settleFrom, aDelayed.end()), sr, offX, 4000.0);

    const double wbL = 10.0 * std::log10(pL / pLraw);
    const double wbX = 10.0 * std::log10(pX / pXraw);
    printf("wideband (per-bin), no reference band:  L %+.1f dB   X %+.1f dB   (relative to raw A)\n",
           wbL, wbX);

    check(wbL < -15.0, "wideband decorrelation nulls L by a large margin");
    check(wbX < -15.0, "wideband decorrelation ALSO nulls X, the other station, at the same time");
    check(wbL < scalarL - 10.0, "and does far better on L than the scalar, wideband-scoped solver");

    // -- Through the Phaser, end to end: the wiring, not just the underlying math. --
    printf("\nThrough the Phaser, end to end\n");
    {
        dsp::stream<complex_t> as, bs;
        Phaser ph;
        ph.init(&as, &bs);
        ph.setMode(Phaser::MODE_DECORR_MIN);
        ph.setAdaptRate(0.3f);
        ph.setWideband(true, 64);
        ph.reset();

        check(ph.isWidebandActive(), "isWidebandActive() is true for a decorrelation mode");
        check(ph.isWidebandDecorrelating(), "isWidebandDecorrelating() distinguishes it from Wiener wideband");

        ph.start();
        std::vector<complex_t> got;
        std::thread rd([&] {
            while (true) {
                int c = ph.out.read();
                if (c < 0) { break; }
                for (int i = 0; i < c; i++) { got.push_back(ph.out.readBuf[i]); }
                ph.out.flush();
            }
        });

        const int block = 8192;
        for (int i = 0; i + block <= N; i += block) {
            memcpy(as.writeBuf, a.data() + i, block * sizeof(complex_t));
            memcpy(bs.writeBuf, b.data() + i, block * sizeof(complex_t));
            as.swap(block);
            bs.swap(block);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        const float meanRho = ph.getWidebandCoherence();
        ph.stop();
        ph.out.stopReader();
        rd.join();
        ph.out.clearReadStop();

        printf("  mean coherence across bins: %.3f\n", meanRho);
        check(meanRho > 0.3f, "mean coherence is reported and non-trivial");

        const size_t from = got.size() * 3 / 4;
        const double pLe = bandPower(std::vector<complex_t>(got.begin() + from, got.end()), sr, offL, 4000.0);
        const double pXe = bandPower(std::vector<complex_t>(got.begin() + from, got.end()), sr, offX, 4000.0);
        const double lDb = 10.0 * std::log10(pLe / pLraw);
        const double xDb = 10.0 * std::log10(pXe / pXraw);
        printf("  through the real Phaser:  L %+.1f dB   X %+.1f dB\n", lDb, xDb);
        check(lDb < -15.0, "L is nulled through the actual Phaser class");
        check(xDb < -15.0, "so is X, at the same time, with no reference band set");

        // Mode switch must be free -- both MIN and MAX taps come from the same per-bin
        // solve, so switching does not require waiting for a fresh analysis window.
        ph.setMode(Phaser::MODE_DECORR_MAX);
        check(ph.isWidebandDecorrelating(), "MAX is still wideband-decorrelating after the switch");
    }

    printf("\n%s (%d failure%s)\n\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
