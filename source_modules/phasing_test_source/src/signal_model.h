#pragma once
#include <complex>
#include <cmath>
#include <cstdint>
#include <utility>

// Pure signal model for the two-channel phasing test source. Deliberately free of any
// SDR++ dependency so it can be compiled and checked on its own:
//
//   c++ -std=c++17 -O2 -o /tmp/t test/test_signal_model.cpp && /tmp/t
//
// See PHASING_PLAN.md Phase 0. The point of this module is that the complex weight
// which nulls a given generated signal is known exactly, so an adaptive algorithm can
// be scored against a right answer instead of against "the waterfall looks better".

namespace phtest {

    // Minimal complex double for the sample loop. std::complex is used for setup math
    // (it has abs/arg/polar/division) but its operator* carries NaN-handling branches.
    struct cd {
        double re, im;
        inline cd operator+(const cd& o) const { return { re + o.re, im + o.im }; }
        inline cd operator-(const cd& o) const { return { re - o.re, im - o.im }; }
        inline cd operator*(const cd& o) const { return { re * o.re - im * o.im, im * o.re + re * o.im }; }
    };

    inline cd toCd(const std::complex<double>& c) { return { c.real(), c.imag() }; }

    // Complex weight from a gain in dB and a phase in degrees.
    inline std::complex<double> weightFromPolar(double gainDb, double phaseDeg) {
        return std::polar(std::pow(10.0, gainDb / 20.0), phaseDeg * (M_PI / 180.0));
    }

    // xorshift128+. Uniform rather than Gaussian noise: the spectrum is flat either way,
    // which is all a noise floor needs to be here, and this costs a few ns per sample.
    struct Rng {
        uint64_t s0 = 0x9E3779B97F4A7C15ull;
        uint64_t s1 = 0xBF58476D1CE4E5B9ull;

        void seed(uint64_t a, uint64_t b) {
            s0 = a ? a : 0x9E3779B97F4A7C15ull;
            s1 = b ? b : 0xBF58476D1CE4E5B9ull;
            for (int i = 0; i < 16; i++) { next(); }
        }

        inline uint64_t next() {
            uint64_t x = s0, y = s1;
            s0 = y;
            x ^= x << 23;
            s1 = x ^ y ^ (x >> 17) ^ (y >> 26);
            return s1 + y;
        }

        // Uniform in [-1, 1).
        inline double uni() {
            return (double)(int32_t)(next() >> 32) * (1.0 / 2147483648.0);
        }
    };

    enum View {
        VIEW_A = 0,
        VIEW_B = 1,
        VIEW_COMBINED = 2
    };

    struct Params {
        double sampleRate = 1000000.0;

        bool wantedEnabled = true;
        double wantedOffset = 100000.0;   // Hz from centre
        double wantedLevel = -20.0;       // dBFS
        double wantedGain = 0.0;          // dB, channel B relative to A
        double wantedPhase = 0.0;         // degrees

        bool interfEnabled = true;
        double interfOffset = -150000.0;
        double interfLevel = -10.0;
        double interfGain = -3.0;
        double interfPhase = 137.0;

        double delaySamples = 0.0;        // channel B delay, may be fractional
        bool noiseEnabled = true;
        double noiseLevel = -80.0;        // dBFS per channel, independent
        bool swapChannels = false;

        int view = VIEW_A;
        double combGain = 0.0;            // test combiner weight w
        double combPhase = 0.0;
    };

    // How one tone appears in each of the two channels.
    struct ToneWeights {
        std::complex<double> a;
        std::complex<double> b;
    };

    // Tone t is generated as:
    //
    //   A[n] = amp                                  * e^(j*2*pi*f*n/Fs)
    //   B[n] = amp * g * e^(-j*2*pi*f*tau/Fs)       * e^(j*2*pi*f*n/Fs)
    //
    // g is the user-set inter-channel weight and tau the channel B delay in samples.
    // Because every component is a pure tone, a delay is exactly a per-tone phase
    // rotation -- no interpolator needed, and exact for fractional tau.
    inline ToneWeights toneWeights(const Params& p, bool interferer) {
        const double offset = interferer ? p.interfOffset : p.wantedOffset;
        const double level = interferer ? p.interfLevel : p.wantedLevel;
        const double gain = interferer ? p.interfGain : p.wantedGain;
        const double phase = interferer ? p.interfPhase : p.wantedPhase;
        const bool on = interferer ? p.interfEnabled : p.wantedEnabled;

        ToneWeights tw;
        if (!on) {
            tw.a = 0.0;
            tw.b = 0.0;
            return tw;
        }

        const double amp = std::pow(10.0, level / 20.0);
        const double delayPhase = -2.0 * M_PI * offset * p.delaySamples / p.sampleRate;

        tw.a = std::complex<double>(amp, 0.0);
        tw.b = std::complex<double>(amp, 0.0) * weightFromPolar(gain, phase) * std::polar(1.0, delayPhase);

        // The swap is applied to the generated channels, so it inverts the roles the
        // combiner sees and therefore inverts the nulling weight. Folding it in here
        // keeps the reported null weight honest.
        if (p.swapChannels) { std::swap(tw.a, tw.b); }
        return tw;
    }

    // The weight w for which Y = A - w*B removes this tone entirely. False if the tone
    // is absent from channel B, in which case no w can null it.
    inline bool nullWeight(const Params& p, bool interferer, std::complex<double>& w) {
        const ToneWeights tw = toneWeights(p, interferer);
        if (std::abs(tw.b) < 1e-12) { return false; }
        w = tw.a / tw.b;
        return true;
    }

    // Stateful generator. Phasors and noise state persist across blocks so that
    // successive calls produce one continuous signal.
    class Generator {
    public:
        void reset() {
            wantedPhasor = { 1.0, 0.0 };
            interfPhasor = { 1.0, 0.0 };
            rngA.seed(0x243F6A8885A308D3ull, 0x13198A2E03707344ull);
            rngB.seed(0xA4093822299F31D0ull, 0x082EFA98EC4E6C89ull);
        }

        // Writes `count` interleaved (re, im) float pairs, i.e. 2*count floats.
        // dsp::complex_t is layout-compatible, so callers pass (float*)stream.writeBuf.
        void generate(const Params& p, int count, float* out) {
            const ToneWeights wanted = toneWeights(p, false);
            const ToneWeights interf = toneWeights(p, true);

            const cd cAw = toCd(wanted.a);
            const cd cBw = toCd(wanted.b);
            const cd cAi = toCd(interf.a);
            const cd cBi = toCd(interf.b);

            const cd dW = toCd(std::polar(1.0, 2.0 * M_PI * p.wantedOffset / p.sampleRate));
            const cd dI = toCd(std::polar(1.0, 2.0 * M_PI * p.interfOffset / p.sampleRate));

            // Uniform noise in [-1,1) has RMS 1/sqrt(3) per component, so a complex
            // sample has RMS sqrt(2/3). Scale so the dBFS figure the user typed matches
            // the complex RMS, making noise and tone levels directly comparable.
            const double namp = p.noiseEnabled ? std::pow(10.0, p.noiseLevel / 20.0) * std::sqrt(1.5) : 0.0;

            const cd combW = toCd(weightFromPolar(p.combGain, p.combPhase));

            cd pw = wantedPhasor;
            cd pi = interfPhasor;

            for (int n = 0; n < count; n++) {
                cd a = cAw * pw + cAi * pi;
                cd b = cBw * pw + cBi * pi;

                if (namp != 0.0) {
                    a = a + cd{ rngA.uni() * namp, rngA.uni() * namp };
                    b = b + cd{ rngB.uni() * namp, rngB.uni() * namp };
                }

                // No swap here: toneWeights() has already folded p.swapChannels into
                // cA*/cB*, so `a` and `b` are the post-swap channels. Swapping again
                // would cancel it and quietly desynchronise the reported null weight
                // from the signal actually being generated.

                cd y;
                if (p.view == VIEW_A) { y = a; }
                else if (p.view == VIEW_B) { y = b; }
                else { y = a - combW * b; }

                out[2 * n] = (float)y.re;
                out[2 * n + 1] = (float)y.im;

                pw = pw * dW;
                pi = pi * dI;
            }

            // Renormalise once per block so the phasors cannot drift in magnitude.
            const double mw = std::sqrt(pw.re * pw.re + pw.im * pw.im);
            const double mi = std::sqrt(pi.re * pi.re + pi.im * pi.im);
            if (mw > 0.0) { pw = { pw.re / mw, pw.im / mw }; }
            if (mi > 0.0) { pi = { pi.re / mi, pi.im / mi }; }
            wantedPhasor = pw;
            interfPhasor = pi;
        }

        cd wantedPhasor = { 1.0, 0.0 };
        cd interfPhasor = { 1.0, 0.0 };

    private:
        Rng rngA;
        Rng rngB;
    };
}
