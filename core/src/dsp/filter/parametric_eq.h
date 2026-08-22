#pragma once
#include "../processor.h"
#include "biquad.h"
#include <algorithm>
#include <type_traits>

// A general-purpose 6-band parametric equalizer: a low shelf, four peaking bands, and a high
// shelf, cascaded in series. Built for the Radio module's user-facing tone control (see
// EqualizerWindow/EqualizerHost in decoder_modules/radio) -- unlike dsp::filter::TubeWarmth's
// fixed-by-ear EQ, every parameter here is meant to be exposed and adjusted, so there are no
// baked-in constants: each band's frequency, gain, and Q are runtime state.
//
// Six bands (shelf/peak/peak/peak/peak/shelf) is a deliberate, ordinary choice for a
// communications receiver's audio -- enough to shape a band's worth of character (cut hum,
// tame sibilance, carve out an interferer) without presenting a 20+ band professional mixing
// console for what is, in this application, a listening tool.

namespace dsp::filter {

    template <class T>
    class ParametricEQ : public Processor<T, T> {
        using base_type = Processor<T, T>;
    public:
        static constexpr int NUM_BANDS = 6;

        ParametricEQ() {}

        ParametricEQ(stream<T>* in, double sampleRate) { init(in, sampleRate); }

        void init(stream<T>* in, double sampleRate) {
            _sampleRate = sampleRate;
            for (int i = 0; i < NUM_BANDS; i++) {
                freqHz[i] = defaultFreqHz(i);
                gainDb[i] = 0.0f;
                q[i] = 0.9;
            }
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

        // Band 0 is always a low shelf, band NUM_BANDS-1 always a high shelf, everything
        // between is peaking -- a fixed topology (not a per-band type choice) matching how a
        // typical hardware/plugin parametric EQ with shelved outer bands works.
        void setBand(int i, double freq, float gain, double bandQ) {
            assert(base_type::_block_init);
            assert(i >= 0 && i < NUM_BANDS);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            freqHz[i] = std::clamp(freq, 20.0, 20000.0);
            gainDb[i] = std::clamp(gain, -18.0f, 18.0f);
            q[i] = std::clamp(bandQ, 0.1, 10.0);
            updateBand(i);
        }

        void getBand(int i, double& freq, float& gain, double& bandQ) const {
            assert(i >= 0 && i < NUM_BANDS);
            freq = freqHz[i];
            gain = gainDb[i];
            bandQ = q[i];
        }

        void reset() {
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            base_type::tempStop();
            for (int i = 0; i < NUM_BANDS; i++) { bandsL[i].reset(); }
            if constexpr (std::is_same_v<T, stereo_t>) {
                for (int i = 0; i < NUM_BANDS; i++) { bandsR[i].reset(); }
            }
            base_type::tempStart();
        }

        inline int process(int count, const T* in, T* out) {
            for (int n = 0; n < count; n++) {
                if constexpr (std::is_same_v<T, float>) {
                    float x = in[n];
                    for (int i = 0; i < NUM_BANDS; i++) { x = bandsL[i].process(x); }
                    out[n] = x;
                }
                if constexpr (std::is_same_v<T, stereo_t>) {
                    float l = in[n].l, r = in[n].r;
                    for (int i = 0; i < NUM_BANDS; i++) {
                        l = bandsL[i].process(l);
                        r = bandsR[i].process(r);
                    }
                    out[n] = { l, r };
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

        // A reasonable log-ish spread across a communications-receiver audio band, used to
        // seed a freshly-constructed EQ (or a UI "reset to defaults" action) -- not something
        // process() itself depends on, since freqHz[] is always explicit runtime state.
        static double defaultFreqHz(int band) {
            static constexpr double defaults[NUM_BANDS] = { 100.0, 300.0, 800.0, 2000.0, 5000.0, 8000.0 };
            return defaults[std::clamp(band, 0, NUM_BANDS - 1)];
        }

    private:
        // Recomputes coefficients on both channels' own Biquad independently -- deliberately
        // not `bandsR[i] = bandsL[i]`, which would be a whole-struct copy and silently drag
        // channel R's own running filter *state* (x1/x2/y1/y2) along with the coefficients,
        // stomping whatever R's independent history was every time a knob changes (an audible
        // glitch, and a correctness break -- exactly the "channels must not cross" property
        // dsp::filter::TubeWarmth's own tests already check for). Recomputing twice costs a
        // few trig calls, only when a band actually changes, never per-sample.
        void updateBand(int i) {
            if (i == 0) {
                bandsL[i].setLowShelf(freqHz[i], q[i], gainDb[i], _sampleRate);
                if constexpr (std::is_same_v<T, stereo_t>) { bandsR[i].setLowShelf(freqHz[i], q[i], gainDb[i], _sampleRate); }
            }
            else if (i == NUM_BANDS - 1) {
                bandsL[i].setHighShelf(freqHz[i], q[i], gainDb[i], _sampleRate);
                if constexpr (std::is_same_v<T, stereo_t>) { bandsR[i].setHighShelf(freqHz[i], q[i], gainDb[i], _sampleRate); }
            }
            else {
                bandsL[i].setPeaking(freqHz[i], q[i], gainDb[i], _sampleRate);
                if constexpr (std::is_same_v<T, stereo_t>) { bandsR[i].setPeaking(freqHz[i], q[i], gainDb[i], _sampleRate); }
            }
        }

        void updateFilters() {
            for (int i = 0; i < NUM_BANDS; i++) { updateBand(i); }
        }

        double _sampleRate = 48000.0;
        double freqHz[NUM_BANDS];
        float gainDb[NUM_BANDS];
        double q[NUM_BANDS];

        Biquad bandsL[NUM_BANDS], bandsR[NUM_BANDS];
    };

}
