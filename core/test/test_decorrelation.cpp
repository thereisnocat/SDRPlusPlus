// Checks for decorrelation: separating the dominant arrival from everything else.
//
//   c++ -std=c++17 -O2 -I<repo>/core/src -I/opt/homebrew/include \
//       -o /tmp/test_decorrelation core/test/test_decorrelation.cpp \
//       -L<repo>/build/core -lsdrpp_core -L/opt/homebrew/lib -lvolk -Wl,-rpath,<repo>/build/core
//
// The scene throughout is the medium wave one this exists for: a strong local station and
// a weak distant one arriving from a different direction, so they reach the two antennas
// with different complex ratios. On one antenna the DX is buried. See PHASING_PLAN.md 2.6.

#include <dsp/types.h>
#include <dsp/stream.h>
#include <dsp/combine/phaser.h>
#include <dsp/combine/decorrelator.h>
#include <thread>
#include <vector>
#include <string>
#include <cstdio>
#include <cmath>
#include <complex>
#include <random>

using namespace dsp::combine;

static int failures = 0;

static void check(bool cond, const std::string& what) {
    printf("  %-60s %s\n", what.c_str(), cond ? "ok" : "FAIL");
    if (!cond) { failures++; }
}

// Two arrivals with different inter-channel ratios, plus independent per-channel noise.
struct Scene {
    std::mt19937 rng{ 7 };
    std::normal_distribution<double> g{ 0.0, 1.0 };

    std::complex<double> localB{ 0.7 * std::cos(2.39), 0.7 * std::sin(2.39) };  // -3 dB, 137 deg
    std::complex<double> dxB{ 1.3 * std::cos(-0.9), 1.3 * std::sin(-0.9) };
    double localAmp = 1.0, dxAmp = 0.05, noiseAmp = 0.01;
    double noiseAmpB = -1.0;      // negative: same as channel A

    // Tracks what each component contributes, so the effect on each can be scored.
    double sumLocalA = 0, sumDxA = 0;

    void fill(dsp::complex_t* a, dsp::complex_t* b, int n) {
        const double nb = (noiseAmpB < 0.0) ? noiseAmp : noiseAmpB;
        for (int i = 0; i < n; i++) {
            const std::complex<double> sLocal(g(rng) * localAmp, g(rng) * localAmp);
            const std::complex<double> sDx(g(rng) * dxAmp, g(rng) * dxAmp);
            const std::complex<double> na(g(rng) * noiseAmp, g(rng) * noiseAmp);
            const std::complex<double> nb2(g(rng) * nb, g(rng) * nb);

            const std::complex<double> av = sLocal + sDx + na;
            const std::complex<double> bv = sLocal * localB + sDx * dxB + nb2;
            a[i] = { (float)av.real(), (float)av.imag() };
            b[i] = { (float)bv.real(), (float)bv.imag() };

            sumLocalA += std::norm(sLocal);
            sumDxA += std::norm(sDx);
        }
    }
};

// Gain a given combination applies to a component with inter-channel ratio `ratio`.
static double componentGainDb(std::complex<double> k0, std::complex<double> k1,
                              std::complex<double> ratio) {
    const std::complex<double> y = k0 + k1 * ratio;
    return 20.0 * std::log10(std::max(std::abs(y), 1e-30));
}

int main() {
    printf("\nDecorrelation\n\n");

    // -----------------------------------------------------------------
    printf("The eigen decomposition separates a dominant arrival\n");
    {
        Scene sc;
        const int N = 200000;
        std::vector<dsp::complex_t> a(N), b(N);
        sc.fill(a.data(), b.data(), N);

        Covariance cov;
        std::complex<double> rab(0, 0);
        double rbb = 0, raa = 0;
        RefBand::accumulateWideband(a.data(), b.data(), N, rab, rbb, &raa);
        cov.raa = raa / N; cov.rbb = rbb / N; cov.rab = rab / (double)N;

        const double rho = coherence(cov);
        const Eigen2 e = solveEigen2(cov);
        printf("        rho %.4f, component separation %.1f dB\n", rho, e.separationDb());
        check(rho > 0.95, "a dominant arrival shows as high coherence");
        check(e.separationDb() > 15.0, "and as a large gap between the components");

        // Eigenvectors must be unit norm and orthogonal, or nothing downstream is valid.
        const double nMax = std::norm(e.uMax[0]) + std::norm(e.uMax[1]);
        const double nMin = std::norm(e.uMin[0]) + std::norm(e.uMin[1]);
        const std::complex<double> dot = std::conj(e.uMax[0]) * e.uMin[0] + std::conj(e.uMax[1]) * e.uMin[1];
        check(std::abs(nMax - 1.0) < 1e-9 && std::abs(nMin - 1.0) < 1e-9, "eigenvectors are unit norm");
        check(std::abs(dot) < 1e-9, "and orthogonal to each other");

        // The point of the whole exercise.
        std::complex<double> k0, k1;
        Matrix2 none;
        combineCoefficients(e.uMin, none, false, k0, k1);
        const double localMin = componentGainDb(k0, k1, sc.localB);
        const double dxMin = componentGainDb(k0, k1, sc.dxB);

        combineCoefficients(e.uMax, none, false, k0, k1);
        const double localMax = componentGainDb(k0, k1, sc.localB);
        const double dxMax = componentGainDb(k0, k1, sc.dxB);

        printf("        Min: local %+.1f dB, DX %+.1f dB   Max: local %+.1f dB, DX %+.1f dB\n",
               localMin, dxMin, localMax, dxMax);
        check(localMin < -30.0, "the minor component nulls the dominant arrival deeply");
        check(dxMin > -12.0, "while barely touching the weaker one");
        check(localMax > -1.0 && dxMax < -10.0, "the principal component does the opposite");

        // What the operator actually cares about: the DX was 26 dB under the local on one
        // antenna; afterwards it should be well above it.
        const double before = 20.0 * std::log10(sc.dxAmp / sc.localAmp);
        const double after = before + (dxMin - localMin);
        printf("        DX to local: %+.1f dB on one antenna, %+.1f dB after Min\n", before, after);
        check(after > before + 30.0, "a buried station is lifted clear of the local");
    }

    // -----------------------------------------------------------------
    printf("\nDegenerate cases do not produce nonsense\n");
    {
        Covariance zero;
        check(coherence(zero) == 0.0, "an empty covariance reports no coherence");
        Eigen2 e = solveEigen2(zero);
        check(std::isfinite(e.lambdaMax) && std::isfinite(e.lambdaMin), "and still yields finite eigenvalues");

        // Uncorrelated channels: nothing to separate, so each channel is its own component.
        Covariance uncorr;
        uncorr.raa = 4.0; uncorr.rbb = 1.0; uncorr.rab = { 0.0, 0.0 };
        e = solveEigen2(uncorr);
        check(std::abs(e.lambdaMax - 4.0) < 1e-9 && std::abs(e.lambdaMin - 1.0) < 1e-9,
              "uncorrelated channels give their own powers as eigenvalues");
        check(std::abs(std::abs(e.uMax[0]) - 1.0) < 1e-9, "the stronger channel is the principal component");
        check(coherence(uncorr) == 0.0, "and coherence is zero");

        // Perfectly correlated: one component carries everything.
        Covariance perfect;
        perfect.raa = 1.0; perfect.rbb = 1.0; perfect.rab = { 1.0, 0.0 };
        check(std::abs(coherence(perfect) - 1.0) < 1e-9, "identical channels are perfectly coherent");
        e = solveEigen2(perfect);
        check(e.lambdaMin < 1e-9, "and leave nothing in the minor component");
    }

    // -----------------------------------------------------------------
    printf("\nWhitening equalises the channels\n");
    {
        // Channel B is far noisier. Without whitening, a maximum-power combination is
        // pulled toward it; the manual makes the same point.
        Covariance noise;
        noise.raa = 1.0;
        noise.rbb = 16.0;
        noise.rab = { 0.0, 0.0 };

        const Matrix2 w = inverseSqrt(noise);
        const Covariance whitened = transform(noise, w);
        printf("        noise powers %.1f / %.1f -> %.3f / %.3f after whitening\n",
               noise.raa, noise.rbb, whitened.raa, whitened.rbb);
        check(std::abs(whitened.raa - 1.0) < 1e-6 && std::abs(whitened.rbb - 1.0) < 1e-6,
              "both channels come out at unit power");
        check(std::abs(whitened.rab) < 1e-6, "and uncorrelated");

        // Whitening a correlated covariance must also produce identity.
        Covariance corr;
        corr.raa = 2.0; corr.rbb = 3.0; corr.rab = { 1.0, 0.5 };
        const Covariance w2 = transform(corr, inverseSqrt(corr));
        check(std::abs(w2.raa - 1.0) < 1e-6 && std::abs(w2.rbb - 1.0) < 1e-6 && std::abs(w2.rab) < 1e-6,
              "a correlated covariance whitens to the identity too");
    }

    // -----------------------------------------------------------------
    // Live bug: "Measure noise" on an empty channel, then "Use it" with Decorrelate --
    // null-strongest selected, turned a -130 dBm noise floor into -30 dBm and reported a
    // "null" of -95 dB (95 dB of *gain*, not cancellation). W's own diagonal is
    // 1/sqrt(lambda) in noise-only eigencoordinates, and any real receiver's actual noise
    // floor is a small fraction of full scale -- unlike the synthetic noise.rbb = 16.0 case
    // above, chosen for a clean 4x/12dB whitening ratio, not to be realistically quiet.
    printf("\nA realistic noise floor does not blow up the whitened output\n");
    {
        // What "Measure noise" on an empty channel actually captures: quiet, independent,
        // and small relative to full scale -- 1/1000 the amplitude of the signal case above,
        // not the previous section's deliberately mismatched-but-comfortable 1.0/16.0.
        Covariance noise;
        noise.raa = 1e-6;
        noise.rbb = 1e-6;
        noise.rab = { 0.0, 0.0 };
        const Matrix2 w = inverseSqrt(noise);

        // A real signal block afterward -- the same dominant-arrival scene as the first
        // section, at its normal (not artificially quiet) scale.
        Scene sc;
        const int N = 200000;
        std::vector<dsp::complex_t> a(N), b(N);
        sc.fill(a.data(), b.data(), N);
        Covariance cov;
        std::complex<double> rab(0, 0);
        double rbb = 0, raa = 0;
        RefBand::accumulateWideband(a.data(), b.data(), N, rab, rbb, &raa);
        cov.raa = raa / N; cov.rbb = rbb / N; cov.rab = rab / (double)N;

        const Covariance working = transform(cov, w);
        const Eigen2 e = solveEigen2(working);

        std::complex<double> k0, k1;
        combineCoefficients(e.uMin, w, true, k0, k1);
        double gain = std::sqrt(std::norm(k0) + std::norm(k1));
        printf("        null-strongest whitened gain: %.4f (%.1f dB)\n", gain, 20.0 * std::log10(gain));
        check(std::abs(gain - 1.0) < 1e-6, "null-strongest stays at unit gain despite a realistic noise floor");

        combineCoefficients(e.uMax, w, true, k0, k1);
        gain = std::sqrt(std::norm(k0) + std::norm(k1));
        printf("        peak-strongest whitened gain: %.4f (%.1f dB)\n", gain, 20.0 * std::log10(gain));
        check(std::abs(gain - 1.0) < 1e-6, "peak-strongest stays at unit gain too");
    }

    // -----------------------------------------------------------------
    printf("\nThrough the Phaser, end to end\n");
    {
        auto run = [](Phaser::Mode mode, double* localOut, double* dxOut) {
            Scene sc;
            dsp::stream<dsp::complex_t> a, b;
            Phaser ph;
            ph.init(&a, &b);
            ph.setMode(mode);
            ph.setAdaptRate(0.3f);
            ph.reset();
            ph.start();

            std::vector<dsp::complex_t> got;
            std::thread rd([&] {
                while (true) {
                    int c = ph.out.read();
                    if (c < 0) { break; }
                    for (int i = 0; i < c; i++) { got.push_back(ph.out.readBuf[i]); }
                    ph.out.flush();
                }
            });

            const int N = 8192;
            for (int k = 0; k < 60; k++) {
                sc.fill(a.writeBuf, b.writeBuf, N);
                a.swap(N);
                b.swap(N);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            const float rho = ph.getCoherence();
            ph.stop();
            ph.out.stopReader();
            rd.join();
            ph.out.clearReadStop();

            // Settled portion only.
            const size_t from = got.size() * 3 / 4;
            double acc = 0.0;
            for (size_t i = from; i < got.size(); i++) {
                acc += (double)got[i].re * got[i].re + (double)got[i].im * got[i].im;
            }
            *localOut = 10.0 * std::log10(acc / (double)(got.size() - from) + 1e-300);
            *dxOut = rho;
            return got.size();
        };

        double powerMin = 0, rhoMin = 0, powerMax = 0, rhoMax = 0;
        run(Phaser::MODE_DECORR_MIN, &powerMin, &rhoMin);
        run(Phaser::MODE_DECORR_MAX, &powerMax, &rhoMax);

        printf("        output power: Min %.1f dB, Max %.1f dB   (rho %.3f)\n", powerMin, powerMax, rhoMin);
        check(powerMax > powerMin + 20.0, "Max keeps far more power than Min");
        check(rhoMin > 0.9, "coherence is reported to the caller");

        // A_ONLY must stay bit-identical, so decorrelation has not disturbed bypass.
        double powerA = 0, rhoA = 0;
        run(Phaser::MODE_A_ONLY, &powerA, &rhoA);
        printf("        channel A alone: %.1f dB\n", powerA);
        check(powerA > powerMin + 20.0, "and Min is well below plain channel A");
    }

    printf("\n%s (%d failure%s)\n\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
