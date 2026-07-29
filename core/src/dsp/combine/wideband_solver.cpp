#include "wideband_solver.h"
#include "../math/constants.h"
#include <fftw3.h>
#include <mutex>
#include <cmath>
#include <cstring>
#include <algorithm>

namespace dsp::combine {

    namespace {
        // fftwf plan creation is not thread safe, and sources, the recorder and the
        // waterfall all build plans of their own.
        std::mutex planMutex;

        inline fftwf_complex* fc(void* p) { return (fftwf_complex*)p; }
    }

    WidebandSolver::WidebandSolver() {}

    WidebandSolver::~WidebandSolver() {
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

    void WidebandSolver::configure(int fftSize, int tapCount, float forgetting) {
        // Round the FFT size to a power of two and keep the taps inside it.
        int m = 64;
        while (m < fftSize && m < 8192) { m <<= 1; }
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

            // Hann, for the usual reason: a strong narrowband signal in a rectangular
            // window smears across the spectrum and corrupts the estimate in bins it has
            // no business in.
            _window.resize(_fftSize);
            for (int i = 0; i < _fftSize; i++) {
                _window[i] = 0.5f - 0.5f * std::cos(2.0f * FL_M_PI * (float)i / (float)_fftSize);
            }
        }

        reset();
    }

    void WidebandSolver::reset() {
        _sab.assign(_fftSize, std::complex<double>(0.0, 0.0));
        _sbb.assign(_fftSize, 0.0);
        _pendA.clear();
        _pendB.clear();
        _tapsRev.assign(_taps, complex_t{ 0.0f, 0.0f });
        _solved = false;
        _framesSinceSolve = 0;
    }

    void WidebandSolver::feed(const complex_t* a, const complex_t* b, int count) {
        if (!_planFwdA || count <= 0) { return; }

        _pendA.insert(_pendA.end(), a, a + count);
        _pendB.insert(_pendB.end(), b, b + count);

        // 50% overlap, so the Hann window does not throw away half the data.
        const int hop = _fftSize / 2;
        while ((int)_pendA.size() >= _fftSize) {
            processFrame();
            _pendA.erase(_pendA.begin(), _pendA.begin() + hop);
            _pendB.erase(_pendB.begin(), _pendB.begin() + hop);
        }

        // Don't let a stall grow the backlog without bound.
        const size_t cap = (size_t)_fftSize * 4;
        if (_pendA.size() > cap) {
            _pendA.erase(_pendA.begin(), _pendA.begin() + (_pendA.size() - cap));
            _pendB.erase(_pendB.begin(), _pendB.begin() + (_pendB.size() - cap));
        }
    }

    void WidebandSolver::processFrame() {
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
            _sab[i] = _sab[i] * (1.0 - lambda) + av * std::conj(bv) * lambda;
            _sbb[i] = _sbb[i] * (1.0 - lambda) + std::norm(bv) * lambda;
        }

        // Solving every frame would be wasted work; the estimate barely moves in one hop.
        if (++_framesSinceSolve >= 8) {
            _framesSinceSolve = 0;
            solve();
        }
    }

    void WidebandSolver::solve() {
        // Regularise against the average power rather than an absolute floor, so the
        // result does not depend on how the input happens to be scaled.
        double meanSbb = 0.0;
        for (int i = 0; i < _fftSize; i++) { meanSbb += _sbb[i]; }
        meanSbb /= (double)_fftSize;
        if (meanSbb <= 0.0) { return; }
        const double eps = meanSbb * 1e-6;

        fftwf_complex* W = fc(_bufW);
        for (int i = 0; i < _fftSize; i++) {
            const std::complex<double> w = _sab[i] / (_sbb[i] + eps);
            W[i][0] = (float)w.real();
            W[i][1] = (float)w.imag();
        }

        fftwf_execute((fftwf_plan)_planInv);

        // W already maps B onto the *delayed* A, because that is what feed() was given, so
        // its impulse response is the filter wanted and sits naturally around lag
        // alignmentDelay(). Shifting it again here would apply the alignment twice.
        const fftwf_complex* h = fc(_bufH);
        const float scale = 1.0f / (float)_fftSize;

        std::vector<complex_t> g(_taps);
        for (int k = 0; k < _taps; k++) {
            const int lag = k;
            // Taper the ends: an abruptly truncated impulse response rings in frequency.
            const float taper = 0.5f - 0.5f * std::cos(2.0f * FL_M_PI * ((float)k + 0.5f) / (float)_taps);
            g[k].re = h[lag][0] * scale * taper;
            g[k].im = h[lag][1] * scale * taper;
        }

        // Store reversed so applying the filter is a straight dot product against a
        // forward-ordered history buffer.
        _tapsRev.resize(_taps);
        for (int k = 0; k < _taps; k++) { _tapsRev[k] = g[_taps - 1 - k]; }
        _solved = true;
    }

    bool WidebandSolver::copyTaps(std::vector<complex_t>& out) {
        if (!_solved) { return false; }
        out = _tapsRev;
        return true;
    }
}
