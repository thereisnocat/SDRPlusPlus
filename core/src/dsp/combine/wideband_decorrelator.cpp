#include "wideband_decorrelator.h"
#include "decorrelator.h"
#include "../math/constants.h"
#include <fftw3.h>
#include <mutex>
#include <cmath>
#include <cstring>
#include <algorithm>

namespace dsp::combine {

    namespace {
        // Shared with wideband_solver.cpp's mutex in spirit, not in the object -- fftwf
        // plan creation is not thread safe and several parts of the app build plans.
        std::mutex planMutex;

        inline fftwf_complex* fc(void* p) { return (fftwf_complex*)p; }

        // Extracts a tapered, reversed N-tap FIR from bin values Kf, via the same route
        // WidebandSolver::solve() uses: inverse-FFT to an impulse response, take the first
        // N lags (valid because the caller pre-delayed its input by taps/2, converting a
        // two-sided response into a causal one), taper the ends, scale, reverse for a
        // forward dot product.
        void extractTaps(const std::vector<std::complex<double>>& Kf, int fftSize, int taps,
                         void* bufW, void* bufH, void* planInv, const std::vector<float>& /*unused*/,
                         std::vector<complex_t>& outRev) {
            fftwf_complex* W = fc(bufW);
            for (int i = 0; i < fftSize; i++) {
                W[i][0] = (float)Kf[i].real();
                W[i][1] = (float)Kf[i].imag();
            }
            fftwf_execute((fftwf_plan)planInv);
            const fftwf_complex* h = fc(bufH);
            const float scale = 1.0f / (float)fftSize;

            std::vector<complex_t> g(taps);
            for (int k = 0; k < taps; k++) {
                const float taper = 0.5f - 0.5f * std::cos(2.0f * FL_M_PI * ((float)k + 0.5f) / (float)taps);
                g[k].re = h[k][0] * scale * taper;
                g[k].im = h[k][1] * scale * taper;
            }
            outRev.resize(taps);
            for (int k = 0; k < taps; k++) { outRev[k] = g[taps - 1 - k]; }
        }
    }

    WidebandDecorrelator::WidebandDecorrelator() {}

    WidebandDecorrelator::~WidebandDecorrelator() {
        std::lock_guard<std::mutex> lck(planMutex);
        if (_planFwdA) { fftwf_destroy_plan((fftwf_plan)_planFwdA); }
        if (_planFwdB) { fftwf_destroy_plan((fftwf_plan)_planFwdB); }
        if (_planInv) { fftwf_destroy_plan((fftwf_plan)_planInv); }
        if (_bufA) { fftwf_free(fc(_bufA)); }
        if (_bufB) { fftwf_free(fc(_bufB)); }
        if (_bufOutA) { fftwf_free(fc(_bufOutA)); }
        if (_bufOutB) { fftwf_free(fc(_bufOutB)); }
        if (_bufW) { fftwf_free(fc(_bufW)); }
        if (_bufH) { fftwf_free(fc(_bufH)); }
    }

    void WidebandDecorrelator::configure(int fftSize, int tapCount, float forgetting) {
        int m = 64;
        while (m < fftSize && m < 16384) { m <<= 1; }
        tapCount = std::clamp(tapCount, 2, m / 4);

        const bool sizeChanged = (m != _fftSize) || !_planFwdA;
        _fftSize = m;
        _taps = tapCount;
        _forgetting = std::clamp(forgetting, 0.001f, 1.0f);

        if (sizeChanged) {
            std::lock_guard<std::mutex> lck(planMutex);
            if (_planFwdA) { fftwf_destroy_plan((fftwf_plan)_planFwdA); }
            if (_planFwdB) { fftwf_destroy_plan((fftwf_plan)_planFwdB); }
            if (_planInv) { fftwf_destroy_plan((fftwf_plan)_planInv); }
            if (_bufA) { fftwf_free(fc(_bufA)); }
            if (_bufB) { fftwf_free(fc(_bufB)); }
            if (_bufOutA) { fftwf_free(fc(_bufOutA)); }
            if (_bufOutB) { fftwf_free(fc(_bufOutB)); }
            if (_bufW) { fftwf_free(fc(_bufW)); }
            if (_bufH) { fftwf_free(fc(_bufH)); }

            _bufA = fftwf_malloc(sizeof(fftwf_complex) * _fftSize);
            _bufB = fftwf_malloc(sizeof(fftwf_complex) * _fftSize);
            _bufOutA = fftwf_malloc(sizeof(fftwf_complex) * _fftSize);
            _bufOutB = fftwf_malloc(sizeof(fftwf_complex) * _fftSize);
            _bufW = fftwf_malloc(sizeof(fftwf_complex) * _fftSize);
            _bufH = fftwf_malloc(sizeof(fftwf_complex) * _fftSize);

            _planFwdA = fftwf_plan_dft_1d(_fftSize, fc(_bufA), fc(_bufOutA), FFTW_FORWARD, FFTW_ESTIMATE);
            _planFwdB = fftwf_plan_dft_1d(_fftSize, fc(_bufB), fc(_bufOutB), FFTW_FORWARD, FFTW_ESTIMATE);
            _planInv = fftwf_plan_dft_1d(_fftSize, fc(_bufW), fc(_bufH), FFTW_BACKWARD, FFTW_ESTIMATE);

            _window.resize(_fftSize);
            for (int i = 0; i < _fftSize; i++) {
                _window[i] = 0.5f - 0.5f * std::cos(2.0f * FL_M_PI * (float)i / (float)_fftSize);
            }
        }

        reset();
    }

    void WidebandDecorrelator::reset() {
        _saa.assign(_fftSize, 0.0);
        _sbb.assign(_fftSize, 0.0);
        _sab.assign(_fftSize, std::complex<double>(0.0, 0.0));
        _pendA.clear();
        _pendB.clear();
        _k0MinRev.assign(_taps, complex_t{ 0.0f, 0.0f });
        _k1MinRev.assign(_taps, complex_t{ 0.0f, 0.0f });
        _k0MaxRev.assign(_taps, complex_t{ 0.0f, 0.0f });
        _k1MaxRev.assign(_taps, complex_t{ 0.0f, 0.0f });
        _solved = false;
        _framesSinceSolve = 0;
        _meanCoherence = 0.0f;
    }

    void WidebandDecorrelator::feed(const complex_t* a, const complex_t* b, int count) {
        if (!_planFwdA || count <= 0) { return; }

        _pendA.insert(_pendA.end(), a, a + count);
        _pendB.insert(_pendB.end(), b, b + count);

        const int hop = _fftSize / 2;
        while ((int)_pendA.size() >= _fftSize) {
            processFrame();
            _pendA.erase(_pendA.begin(), _pendA.begin() + hop);
            _pendB.erase(_pendB.begin(), _pendB.begin() + hop);
        }

        const size_t cap = (size_t)_fftSize * 4;
        if (_pendA.size() > cap) {
            _pendA.erase(_pendA.begin(), _pendA.begin() + (_pendA.size() - cap));
            _pendB.erase(_pendB.begin(), _pendB.begin() + (_pendB.size() - cap));
        }
    }

    void WidebandDecorrelator::processFrame() {
        fftwf_complex* inA = fc(_bufA);
        fftwf_complex* inB = fc(_bufB);
        for (int i = 0; i < _fftSize; i++) {
            const float w = _window[i];
            inA[i][0] = _pendA[i].re * w;
            inA[i][1] = _pendA[i].im * w;
            inB[i][0] = _pendB[i].re * w;
            inB[i][1] = _pendB[i].im * w;
        }

        fftwf_execute((fftwf_plan)_planFwdA);
        fftwf_execute((fftwf_plan)_planFwdB);

        const fftwf_complex* A = fc(_bufOutA);
        const fftwf_complex* B = fc(_bufOutB);
        const double lambda = _forgetting;

        for (int i = 0; i < _fftSize; i++) {
            const std::complex<double> av(A[i][0], A[i][1]);
            const std::complex<double> bv(B[i][0], B[i][1]);
            _saa[i] = _saa[i] * (1.0 - lambda) + std::norm(av) * lambda;
            _sbb[i] = _sbb[i] * (1.0 - lambda) + std::norm(bv) * lambda;
            _sab[i] = _sab[i] * (1.0 - lambda) + av * std::conj(bv) * lambda;
        }

        if (++_framesSinceSolve >= 8) {
            _framesSinceSolve = 0;
            solve();
        }
    }

    void WidebandDecorrelator::solve() {
        std::vector<std::complex<double>> K0min(_fftSize), K1min(_fftSize);
        std::vector<std::complex<double>> K0max(_fftSize), K1max(_fftSize);

        // The gate threshold is relative to the median bin's power -- an estimate of the
        // noise floor that self-calibrates to whatever the source's gain and bandwidth
        // happen to be, rather than an absolute level that would need retuning per radio.
        std::vector<double> power(_fftSize);
        for (int i = 0; i < _fftSize; i++) { power[i] = _saa[i] + _sbb[i]; }
        std::vector<double> sortedPower = power;
        std::nth_element(sortedPower.begin(), sortedPower.begin() + sortedPower.size() / 2, sortedPower.end());
        const double medianPower = sortedPower[sortedPower.size() / 2];
        const double gateThreshold = medianPower * std::pow(10.0, _gateDb / 10.0);

        double coherenceSum = 0.0, weightSum = 0.0;
        int active = 0;
        const Matrix2 noWhitening;

        for (int i = 0; i < _fftSize; i++) {
            Covariance cov;
            cov.raa = _saa[i];
            cov.rbb = _sbb[i];
            cov.rab = _sab[i];

            // Below the gate: pass A through unchanged rather than solving. A bin with no
            // real signal still has SOME apparent correlation from block to block -- noise
            // is not literally zero-coherence, just randomly varying -- and solving it
            // anyway means every one of potentially thousands of such bins contributes an
            // arbitrary, momentary direction to the taps via the inverse FFT. Measured on
            // real air: leaving them in is what made the achieved null swing by 20 dB
            // between otherwise similar windows of the same recording. See
            // PHASING_PLAN.md section 2.6c.
            if (!cov.valid() || power[i] < gateThreshold) {
                K0min[i] = K0max[i] = { 1.0, 0.0 };
                K1min[i] = K1max[i] = { 0.0, 0.0 };
                continue;
            }
            active++;

            const Eigen2 e = solveEigen2(cov);
            std::complex<double> k0, k1;
            combineCoefficients(e.uMin, noWhitening, false, k0, k1);
            K0min[i] = k0; K1min[i] = k1;
            combineCoefficients(e.uMax, noWhitening, false, k0, k1);
            K0max[i] = k0; K1max[i] = k1;

            // Weight by bin power so a handful of noise-only bins with a spuriously high
            // ratio-of-nothing coherence do not dominate the average the meter shows.
            coherenceSum += coherence(cov) * power[i];
            weightSum += power[i];
        }

        _activeBins = active;
        _meanCoherence = (weightSum > 0.0) ? (float)(coherenceSum / weightSum) : 0.0f;

        extractTaps(K0min, _fftSize, _taps, _bufW, _bufH, _planInv, _window, _k0MinRev);
        extractTaps(K1min, _fftSize, _taps, _bufW, _bufH, _planInv, _window, _k1MinRev);
        extractTaps(K0max, _fftSize, _taps, _bufW, _bufH, _planInv, _window, _k0MaxRev);
        extractTaps(K1max, _fftSize, _taps, _bufW, _bufH, _planInv, _window, _k1MaxRev);

        _solved = true;
    }

    bool WidebandDecorrelator::copyTaps(bool minNotMax, std::vector<complex_t>& k0Out,
                                         std::vector<complex_t>& k1Out) {
        if (!_solved) { return false; }
        k0Out = minNotMax ? _k0MinRev : _k0MaxRev;
        k1Out = minNotMax ? _k1MinRev : _k1MaxRev;
        return true;
    }
}
