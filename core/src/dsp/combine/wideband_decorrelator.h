#pragma once
#include "../types.h"
#include <vector>
#include <complex>
#include <cmath>

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

        // Bins whose total power sits below (median bin power + gateDb) are excluded from
        // the solve and treated as pass-through (k0=1, k1=0) rather than actively combined.
        // Found necessary 2026-08-05, on real air: without it, every one of the thousands
        // of noise-floor bins across a wide observed span gets its own eigendecomposition
        // -- on pure noise, that decomposition is not "no answer", it is an ARBITRARY
        // answer, since noise is still technically "coherent" with itself to some randomly
        // varying degree from block to block. Those bins' contributions all leak into every
        // realized tap via the inverse FFT, and which particular noise realization was
        // present at solve time is what made the result swing by 20 dB between otherwise
        // similar windows of the same real recording. Gating by POWER rather than by
        // frequency distance from any one target preserves the feature's real advantage --
        // several genuinely strong, unrelated stations still each pass the gate on their
        // own merits, wherever they sit in the span -- which a reference-band-style
        // frequency window could not do. 20 dB is not derived from anything more principled
        // than what measured best on one real recording; treat it as a starting point.
        // See PHASING_PLAN.md section 2.6c.
        void setGateThresholdDb(float db) { _gateDb = db; }
        float getGateThresholdDb() const { return _gateDb; }

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

        // How many of fftSize bins passed the power gate on the last solve. Mostly a
        // sanity readout: a handful of bins means the gate is behaving like a de-facto
        // reference band around one strong signal; a large fraction means most of the
        // observed span had real signal in it.
        int activeBinCount() const { return _activeBins; }

        // Diagnostic: one bin's current covariance estimate, updated every hop rather than
        // only at each solve() -- for watching how fast a single bin's estimate actually
        // moves, which is what distinguishes real non-stationarity from a settling
        // transient. Added investigating the real-air instability in section 2.6b; not
        // used by the production combining path.
        bool getBinStats(int bin, double& saa, double& sbb, std::complex<double>& sab) const {
            if (bin < 0 || bin >= (int)_saa.size()) { return false; }
            saa = _saa[bin];
            sbb = _sbb[bin];
            sab = _sab[bin];
            return true;
        }

        // Bin index nearest a given offset from centre, for pointing getBinStats() at a
        // station without the caller re-deriving FFT bin arithmetic.
        int binForOffset(double offsetHz, double sampleRate) const {
            int bin = (int)std::lround(offsetHz / sampleRate * _fftSize);
            bin %= _fftSize;
            if (bin < 0) { bin += _fftSize; }
            return bin;
        }

    private:
        void processFrame();
        void solve();

        int _fftSize = 4096;
        int _taps = 32;
        float _forgetting = 0.1f;
        float _gateDb = 20.0f;
        bool _solved = false;
        int _framesSinceSolve = 0;
        float _meanCoherence = 0.0f;
        int _activeBins = 0;

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
