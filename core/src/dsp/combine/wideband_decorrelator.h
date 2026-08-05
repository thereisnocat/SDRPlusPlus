#pragma once
#include "../types.h"
#include <vector>
#include <complex>

namespace dsp::combine {

    // Frequency-dependent decorrelation: the same eigendecomposition as decorrelator.h,
    // solved independently per FFT bin instead of once for the whole band. See
    // PHASING_PLAN.md section 2.6a.
    //
    // The scalar decorrelator's null is deep only where the two channels differ by a
    // frequency-flat ratio -- the same honest limitation section 2.5 documents for the
    // scalar Wiener weight, and measured on real air (WNYC 820, 2026-08-05): narrowing the
    // reference band from 4 kHz to 1 kHz bought under 0.2 dB, meaning 23 dB was the ceiling
    // for one complex weight on that station, not a scoping problem.
    //
    // A useful side effect of solving per bin: each bin sees only the energy at its own
    // frequency, so this needs no reference band to behave on a crowded span the way the
    // scalar solver needs one -- the FFT already does the separation the reference band
    // exists to approximate with a mixer and a decimator. Every bin gets its own null (or
    // peak) of whatever locally dominates it, independent of what is happening elsewhere in
    // the observed bandwidth.
    //
    // Structurally this mirrors WidebandSolver: overlap-processed, Hann-windowed spectral
    // accumulation, solved a few times a second, applied as an ordinary short FIR the rest
    // of the time. The difference is three spectra instead of two (Saa, Sbb, Sab, since
    // neither channel is privileged the way the Wiener form privileges A) and two sets of
    // taps instead of one, since y = k0(f)*A + k1(f)*B has two frequency responses to solve
    // and realise.
    //
    // No whitening in this version -- a per-bin noise covariance is a real extension, not a
    // trivial one, and is left for if the unwhitened form proves inadequate in practice.
    //
    // fftw types are kept out of this header for the same reason as wideband_solver.h: it
    // is reached from module code through signal_path.h.
    class WidebandDecorrelator {
    public:
        WidebandDecorrelator();
        ~WidebandDecorrelator();

        WidebandDecorrelator(const WidebandDecorrelator&) = delete;
        WidebandDecorrelator& operator=(const WidebandDecorrelator&) = delete;

        void configure(int fftSize, int tapCount, float forgetting);
        void reset();

        // Accumulate spectra. Both channels must already carry the alignment delay
        // reported by alignmentDelay() -- unlike the Wiener solver, neither channel is the
        // privileged reference here, so both need the same centring delay.
        void feed(const complex_t* a, const complex_t* b, int count);

        // Latest taps for the requested split: minNotMax true nulls the per-bin dominant
        // arrival (mirrors MODE_DECORR_MIN), false peaks it (MODE_DECORR_MAX). Both are
        // solved from the same covariance every pass, so switching between them costs
        // nothing. Reversed, like WidebandSolver's, so applying is a plain dot product.
        bool copyTaps(bool minNotMax, std::vector<complex_t>& k0Out, std::vector<complex_t>& k1Out);

        int alignmentDelay() const { return _taps / 2; }
        int tapCount() const { return _taps; }
        int fftSize() const { return _fftSize; }
        bool hasSolution() const { return _solved; }

        // Mean coherence across bins with meaningful power, weighted by that power. Lets
        // the UI show something in wideband mode without pretending a single scalar rho
        // describes a frequency-dependent decomposition.
        float meanCoherence() const { return _meanCoherence; }

    private:
        void processFrame();
        void solve();

        int _fftSize = 4096;
        int _taps = 32;
        float _forgetting = 0.1f;
        bool _solved = false;
        int _framesSinceSolve = 0;
        float _meanCoherence = 0.0f;

        void* _planFwdA = nullptr;
        void* _planFwdB = nullptr;
        void* _planInv = nullptr;
        void* _bufA = nullptr;
        void* _bufB = nullptr;
        void* _bufOutA = nullptr;
        void* _bufOutB = nullptr;
        void* _bufW = nullptr;
        void* _bufH = nullptr;

        std::vector<float> _window;
        std::vector<complex_t> _pendA, _pendB;
        std::vector<double> _saa, _sbb;
        std::vector<std::complex<double>> _sab;

        std::vector<complex_t> _k0MinRev, _k1MinRev, _k0MaxRev, _k1MaxRev;
    };
}
