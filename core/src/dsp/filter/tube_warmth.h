#pragma once
#include "../processor.h"
#include "../math/constants.h"
#include "biquad.h"
#include <cmath>
#include <cstdint>
#include <algorithm>
#include <type_traits>

// Simulates the sound of an old tube radio: a small low-mid resonance bump and rolled-off
// treble (matching a small paper speaker and a bandwidth-limited output transformer), soft
// waveshaping saturation biased for even-order harmonics (the "warm" character tube stages
// have, versus the odd-order harmonics symmetric solid-state/digital clipping produces --
// see the design discussion this was built from, 2026-08-20), and a trace of hiss and mains
// hum for texture. Nothing here needs anything genuinely new: it's the same Processor<T,T>
// shape as dsp::filter::Deemphasis right next to it, just a different per-sample transfer
// function.
//
// Two independent knobs, both 0..1, deliberately not a whole parametric-EQ-plus-compressor
// panel: `warmth` blends between the dry signal and the fully-shaped one (EQ + saturation
// together, since real tube compression is a side effect of the same stage saturating, not a
// separate process -- warmth=0 is an exact passthrough of the shaped path's *input*, not a
// weakened version of the effect); `noise` scales hiss/hum independently, since someone might
// want the tone character without added noise. Every other parameter (EQ frequencies/gain,
// saturation drive/bias, hiss/hum levels at full `noise`) is a fixed, chosen-by-ear constant,
// not exposed -- this is meant to sound like "an old radio", not to be a mixing tool.

namespace dsp::filter {

    // Small, fast, self-contained PRNG for the hiss generator -- deliberately not the
    // standard library's rand() (global state, not something a per-instance DSP block should
    // share) or <random> (more machinery than a cheap texture generator needs). Xorshift32,
    // Marsaglia's original construction; period 2^32-1, passes normal statistical tests for
    // something this undemanding.
    inline uint32_t tubeWarmthXorshift32(uint32_t& state) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }

    template <class T>
    class TubeWarmth : public Processor<T, T> {
        using base_type = Processor<T, T>;
    public:
        TubeWarmth() {}

        TubeWarmth(stream<T>* in, double sampleRate) { init(in, sampleRate); }

        void init(stream<T>* in, double sampleRate) {
            _sampleRate = sampleRate;
            updateFilters();
            base_type::init(in);
        }

        void setSampleRate(double sampleRate) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            base_type::tempStop();
            _sampleRate = sampleRate;
            updateFilters();
            base_type::tempStart();
        }

        // 0..1. Blends between the dry signal and the fully EQ'd + saturated one -- warmth=0
        // is an exact passthrough (see the file's own top comment for why this is a dry/wet
        // blend rather than scaling the saturation drive itself down to nothing).
        void setWarmth(float amount) {
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            _warmth = std::clamp(amount, 0.0f, 1.0f);
        }

        // 0..1. Hiss + 60Hz hum level, independent of warmth -- scaled directly, not blended,
        // so it's either present at a level proportional to this knob or entirely silent at 0,
        // regardless of how much (if any) tone-shaping is also applied.
        void setNoise(float amount) {
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            _noise = std::clamp(amount, 0.0f, 1.0f);
        }

        void reset() {
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            base_type::tempStop();
            peakL.reset(); lowpassL.reset();
            if constexpr (std::is_same_v<T, stereo_t>) { peakR.reset(); lowpassR.reset(); }
            humPhase = 0.0f;
            base_type::tempStart();
        }

        inline int process(int count, const T* in, T* out) {
            for (int i = 0; i < count; i++) {
                // Shared across channels -- real mains hum is coherent left/right, coming
                // from one shared power supply, not an independent per-channel source.
                const float hum = (_noise > 0.0f) ? sinf(humPhase) * _noise * HUM_PEAK : 0.0f;
                humPhase += humPhaseStep;
                if (humPhase >= 2.0f * FL_M_PI) { humPhase -= 2.0f * FL_M_PI; }

                if constexpr (std::is_same_v<T, float>) {
                    out[i] = shapeChannel(in[i], peakL, lowpassL, noiseStateL) + hum;
                }
                if constexpr (std::is_same_v<T, stereo_t>) {
                    out[i].l = shapeChannel(in[i].l, peakL, lowpassL, noiseStateL) + hum;
                    out[i].r = shapeChannel(in[i].r, peakR, lowpassR, noiseStateR) + hum;
                }
            }
            return count;
        }

        int run() {
            int count = base_type::_in->read();
            if (count < 0) { return -1; }
            process(count, base_type::_in->readBuf, base_type::out.writeBuf);
            base_type::_in->flush();
            if (!base_type::out.swap(count)) { return -1; }
            return count;
        }

    private:
        // Fixed by ear, not exposed -- see the file's own top comment for why. Small paper
        // speaker + resonant cabinet: a modest low-mid bump; small output transformer: rolled
        // off well before typical hi-fi treble. Saturation driven and biased hard enough to be
        // clearly audible at warmth=1 without being cartoonish. Unlike RSR200's numbers, none
        // of this comes from a manual -- there's no spec sheet for "sounds like a tube radio",
        // just the description this feature was built from and iterating by ear.
        static constexpr double PEAK_FREQ_HZ = 350.0;
        static constexpr double PEAK_Q = 0.8;
        static constexpr double PEAK_GAIN_DB = 5.0;
        static constexpr double LOWPASS_FREQ_HZ = 4200.0;
        static constexpr double LOWPASS_Q = 0.707;
        static constexpr float DRIVE = 2.0f;
        static constexpr float BIAS = 0.15f;
        static constexpr float HISS_PEAK = 0.015f;   // full scale at noise=1
        static constexpr float HUM_PEAK = 0.005f;    // full scale at noise=1
        static constexpr double HUM_FREQ_HZ = 60.0;  // mains ripple -- a stylistic constant,
                                                       // not modelling any specific country's
                                                       // supply frequency

        void updateFilters() {
            peakL.setPeaking(PEAK_FREQ_HZ, PEAK_Q, PEAK_GAIN_DB, _sampleRate);
            lowpassL.setLowPass(LOWPASS_FREQ_HZ, LOWPASS_Q, _sampleRate);
            if constexpr (std::is_same_v<T, stereo_t>) {
                peakR.setPeaking(PEAK_FREQ_HZ, PEAK_Q, PEAK_GAIN_DB, _sampleRate);
                lowpassR.setLowPass(LOWPASS_FREQ_HZ, LOWPASS_Q, _sampleRate);
            }
            humPhaseStep = (float)(2.0 * DB_M_PI * HUM_FREQ_HZ / _sampleRate);
        }

        // The waveshaper's own transfer function. Biasing the input off-centre before a
        // symmetric tanh makes the *effective* curve asymmetric around x=0 -- exactly how a
        // single-ended tube stage, biased off the middle of its own characteristic curve,
        // generates predominantly even-order harmonics instead of the odd-order ones a
        // symmetric (unbiased) soft clipper produces. Normalized by the curve's own slope at
        // the bias point (not by its value at some fixed input level) so quiet passages keep
        // close to unity gain and only loud ones get audibly compressed -- matching how a real
        // tube stage barely colors a quiet signal but squashes hot ones.
        inline float shape(float x) const { return (tanhf(DRIVE * x + BIAS) - tanhBias) * driveNorm; }

        inline float shapeChannel(float x, Biquad& peak, Biquad& lowpass, uint32_t& noiseState) const {
            float wet = lowpass.process(shape(peak.process(x)));
            float y = x + _warmth * (wet - x);
            if (_noise > 0.0f) {
                float hiss = (((float)(tubeWarmthXorshift32(noiseState) & 0xFFFFFF) / (float)0xFFFFFF) * 2.0f - 1.0f);
                y += hiss * _noise * HISS_PEAK;
            }
            return y;
        }

        double _sampleRate = 48000.0;
        float _warmth = 0.0f;
        float _noise = 0.0f;

        Biquad peakL, lowpassL, peakR, lowpassR;
        uint32_t noiseStateL = 0xC0FFEE01u, noiseStateR = 0xC0FFEE02u;   // any nonzero seeds
        float humPhase = 0.0f;
        float humPhaseStep = 0.0f;

        // tanhf(BIAS) and 1/localSlope are both fixed (DRIVE/BIAS are compile-time constants),
        // so they're plain static constants rather than something recomputed per sample or
        // even per setWarmth() call.
        static inline const float tanhBias = tanhf(BIAS);
        static inline const float driveNorm = 1.0f / (DRIVE * (1.0f - tanhBias * tanhBias));
    };

}
