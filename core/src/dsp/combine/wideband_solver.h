#pragma once
#include "../types.h"
#include <vector>
#include <complex>

namespace dsp::combine {

    // Solves for a multi-tap complex weight, so the null can follow a frequency-dependent
    // difference between two antenna paths. See PHASING_PLAN.md section 2.5.
    //
    // A single complex weight nulls deeply only where the two channels differ by a
    // frequency-flat ratio. Over a few tens of kHz that holds; over the megahertz SDR++
    // usually displays it does not, so a scalar notches one carrier and leaves the rest of
    // a broadband pest untouched. An N-tap weight has a frequency-dependent response and
    // can cancel across the band. It also subsumes timing offsets between the feedlines
    // completely, which is why the separate delay control is retired in this mode.
    //
    // Solved in the frequency domain, applied in the time domain. Cross- and auto-spectra
    // are averaged over overlapping windows, W(f) = Sab(f)/(Sbb(f)+eps) is inverse
    // transformed, and the result is truncated and tapered to N taps. Solving is the only
    // part that needs an FFT and it happens a few times a second; the filtering itself is
    // an ordinary short FIR.
    //
    // fftw types are kept out of this header deliberately: it is reached from module code
    // through signal_path.h, and every module would otherwise pull in fftw3.h.
    class WidebandSolver {
    public:
        WidebandSolver();
        ~WidebandSolver();

        WidebandSolver(const WidebandSolver&) = delete;
        WidebandSolver& operator=(const WidebandSolver&) = delete;

        // fftSize sets the frequency resolution of the solution; tapCount how much of the
        // impulse response is kept. forgetting is the per-window averaging weight -- small
        // values give a steadier estimate that takes longer to settle.
        void configure(int fftSize, int tapCount, float forgetting);
        void reset();

        // Accumulate spectra. Channel A must already carry the alignment delay reported by
        // alignmentDelay(), since the solution is centred in the tap span rather than
        // starting at zero lag.
        void feed(const complex_t* a, const complex_t* b, int count);

        // Latest taps, reversed so they can be dot-producted straight against a forward
        // history buffer. False until enough data has been seen to solve.
        bool copyTaps(std::vector<complex_t>& out);

        // Samples of delay that must be applied to channel A. The optimal response is
        // generally two-sided -- channel B may need to be *advanced* relative to A -- so
        // the tap span is centred and A is held back by half of it.
        int alignmentDelay() const { return _taps / 2; }

        int tapCount() const { return _taps; }
        int fftSize() const { return _fftSize; }
        bool hasSolution() const { return _solved; }

    private:
        void processFrame();
        void solve();

        int _fftSize = 1024;
        int _taps = 32;
        float _forgetting = 0.1f;
        bool _solved = false;
        int _framesSinceSolve = 0;

        // Opaque fftwf plans and buffers; see the .cpp.
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
        std::vector<complex_t> _pendA, _pendB;   // samples awaiting a full frame
        std::vector<std::complex<double>> _sab;
        std::vector<double> _sbb;
        std::vector<complex_t> _tapsRev;
    };
}
