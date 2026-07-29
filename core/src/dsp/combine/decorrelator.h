#pragma once
#include <complex>
#include <cmath>
#include <algorithm>

namespace dsp::combine {

    // Principal component analysis of the 2x2 covariance between two coherent channels.
    // See PHASING_PLAN.md section 2.6.
    //
    // Every signal reaching two antennas is present in both. What distinguishes them is
    // the complex ratio between the channels, which is set by arrival direction. So the
    // useful decomposition is not "common versus uncommon" but "the dominant arrival
    // versus everything orthogonal to it":
    //
    //   - the principal eigenvector is the combination with maximum power, i.e. whatever
    //     is arriving strongest;
    //   - the minor eigenvector is everything else, with that dominant arrival removed.
    //
    // With two channels there is exactly one degree of freedom, so this removes one
    // dominant signal per processed band -- which is precisely what makes a medium wave
    // local disappear and reveal what it was covering.
    //
    // For 2x2 Hermitian the decomposition is closed form: no iteration, no matrix library.

    struct Covariance {
        double raa = 0.0;                    // E|A|^2
        double rbb = 0.0;                    // E|B|^2
        std::complex<double> rab{ 0.0, 0.0 }; // E[A conj(B)]

        bool valid() const { return raa > 0.0 && rbb > 0.0; }
    };

    struct Eigen2 {
        double lambdaMax = 0.0;
        double lambdaMin = 0.0;
        std::complex<double> uMax[2] = { { 1.0, 0.0 }, { 0.0, 0.0 } };  // unit norm
        std::complex<double> uMin[2] = { { 0.0, 0.0 }, { 1.0, 0.0 } };

        // How far apart the two components are, in dB. A large separation means one
        // arrival really does dominate, so the null is meaningful; a small one means the
        // channels are seeing a diffuse mixture and there is nothing to separate.
        double separationDb() const {
            if (lambdaMin <= 0.0 || lambdaMax <= 0.0) { return 0.0; }
            return 10.0 * std::log10(lambdaMax / lambdaMin);
        }
    };

    inline void normalise2(std::complex<double> v[2]) {
        const double n = std::sqrt(std::norm(v[0]) + std::norm(v[1]));
        if (n > 1e-300) { v[0] /= n; v[1] /= n; }
        else { v[0] = { 1.0, 0.0 }; v[1] = { 0.0, 0.0 }; }
    }

    inline Eigen2 solveEigen2(const Covariance& c) {
        Eigen2 e;
        const double trace = c.raa + c.rbb;
        const double det = c.raa * c.rbb - std::norm(c.rab);
        const double disc = std::sqrt((std::max)(0.0, trace * trace - 4.0 * det));

        e.lambdaMax = 0.5 * (trace + disc);
        e.lambdaMin = 0.5 * (trace - disc);

        // (R - lambda I) u = 0 gives u = [rab, lambda - raa].
        if (std::abs(c.rab) > 1e-300) {
            e.uMax[0] = c.rab;
            e.uMax[1] = std::complex<double>(e.lambdaMax - c.raa, 0.0);
            e.uMin[0] = c.rab;
            e.uMin[1] = std::complex<double>(e.lambdaMin - c.raa, 0.0);
        }
        else {
            // Already uncorrelated: the channels are their own components, stronger first.
            const bool aStronger = (c.raa >= c.rbb);
            e.uMax[0] = aStronger ? std::complex<double>(1.0, 0.0) : std::complex<double>(0.0, 0.0);
            e.uMax[1] = aStronger ? std::complex<double>(0.0, 0.0) : std::complex<double>(1.0, 0.0);
            e.uMin[0] = aStronger ? std::complex<double>(0.0, 0.0) : std::complex<double>(1.0, 0.0);
            e.uMin[1] = aStronger ? std::complex<double>(1.0, 0.0) : std::complex<double>(0.0, 0.0);
        }
        normalise2(e.uMax);
        normalise2(e.uMin);
        return e;
    }

    // Normalised cross-correlation, |Rab| / sqrt(Raa Rbb), in 0..1. The Perseus22 calls
    // this "Rho" and shows it so the operator can judge whether the estimation bandwidth
    // was chosen well: a value near 1 means the two channels really are seeing the same
    // thing and there is a dominant arrival to separate out.
    inline double coherence(const Covariance& c) {
        if (!c.valid()) { return 0.0; }
        return (std::min)(1.0, std::abs(c.rab) / std::sqrt(c.raa * c.rbb));
    }

    // A 2x2 complex matrix, stored row-major.
    struct Matrix2 {
        std::complex<double> m[2][2] = { { { 1.0, 0.0 }, { 0.0, 0.0 } },
                                         { { 0.0, 0.0 }, { 1.0, 0.0 } } };
    };

    // R^(-1/2), for whitening. Applied to a channel pair it produces two outputs of equal
    // power that are uncorrelated -- the manual's "orthonormalisation". Calibrated on
    // noise-only data, it is what turns a maximum-power combination into a maximum-SNR
    // one: without it, combining for power alone favours whichever channel is noisiest.
    inline Matrix2 inverseSqrt(const Covariance& c) {
        Matrix2 w;
        if (!c.valid()) { return w; }

        const Eigen2 e = solveEigen2(c);
        const double l1 = (std::max)(e.lambdaMax, 1e-300);
        const double l2 = (std::max)(e.lambdaMin, 1e-300);
        const double s1 = 1.0 / std::sqrt(l1);
        const double s2 = 1.0 / std::sqrt(l2);

        // W = sum_k lambda_k^(-1/2) u_k u_k^H
        for (int r = 0; r < 2; r++) {
            for (int col = 0; col < 2; col++) {
                w.m[r][col] = s1 * e.uMax[r] * std::conj(e.uMax[col])
                            + s2 * e.uMin[r] * std::conj(e.uMin[col]);
            }
        }
        return w;
    }

    // R' = W R W^H, so a covariance measured on raw channels can be expressed in whitened
    // coordinates without touching a single sample.
    inline Covariance transform(const Covariance& c, const Matrix2& w) {
        const std::complex<double> r[2][2] = {
            { { c.raa, 0.0 }, c.rab },
            { std::conj(c.rab), { c.rbb, 0.0 } }
        };

        std::complex<double> wr[2][2];
        for (int i = 0; i < 2; i++) {
            for (int j = 0; j < 2; j++) {
                wr[i][j] = w.m[i][0] * r[0][j] + w.m[i][1] * r[1][j];
            }
        }

        std::complex<double> out[2][2];
        for (int i = 0; i < 2; i++) {
            for (int j = 0; j < 2; j++) {
                out[i][j] = wr[i][0] * std::conj(w.m[j][0]) + wr[i][1] * std::conj(w.m[j][1]);
            }
        }

        Covariance res;
        res.raa = out[0][0].real();
        res.rbb = out[1][1].real();
        res.rab = out[0][1];
        return res;
    }

    // The coefficients to apply to the raw channels: y = k0*A + k1*B.
    //
    // The eigenvector u selects a combination of the *whitened* channels, y = u^H W x.
    // Since u^H W x = (W^H u)^H x, the whitening folds into the coefficients and never has
    // to touch a sample.
    inline void combineCoefficients(const std::complex<double> u[2], const Matrix2& w,
                                    bool whitened, std::complex<double>& k0,
                                    std::complex<double>& k1) {
        if (!whitened) {
            k0 = std::conj(u[0]);
            k1 = std::conj(u[1]);
            return;
        }
        // c = W^H u, then k = conj(c).
        const std::complex<double> c0 = std::conj(w.m[0][0]) * u[0] + std::conj(w.m[1][0]) * u[1];
        const std::complex<double> c1 = std::conj(w.m[0][1]) * u[0] + std::conj(w.m[1][1]) * u[1];
        k0 = std::conj(c0);
        k1 = std::conj(c1);
    }
}
