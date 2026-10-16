#pragma once
#include <algorithm>
#include <cmath>
#include <mutex>
#include <vector>
#include <fftw3.h>
#include "../channel/frequency_xlator.h"
#include "../filter/fir.h"
#include "../taps/low_pass.h"
#include "../buffer/buffer.h"
#include "quadrature.h"

// SCA (Subsidiary Communications Authorization) subcarriers: an FM broadcast carries, above the 57 kHz RDS, extra audio services (radio reading
// services, foreign-language programmes, background music) as narrow-band FM subcarriers, usually at 67 kHz or 92 kHz and sometimes at 41 or 76 kHz.
// Each is frequency modulated about its own carrier by +/-5 kHz or so, with audio up to about 7 kHz.
namespace dsp::demod {

    // Takes the demodulated composite (real, at `samplerate`) and gives the audio of the subcarrier at `carrier` Hz.
    class SCADemod {
    public:
        SCADemod() {}
        ~SCADemod() { release(); }

        void init(double samplerate, double carrier = 67000.0) {
            release();
            _samplerate = samplerate;
            _carrier = carrier;
            xlator.init(NULL, -carrier, samplerate);
            chanTaps = taps::lowPass(12000.0, 8000.0, samplerate);
            chanFir.init(NULL, chanTaps);
            quad.init(NULL, 5000.0, samplerate);   // +/-5 kHz of subcarrier deviation reads +/-1.0
            audioTaps = taps::lowPass(7500.0, 4000.0, samplerate);
            audioFir.init(NULL, audioTaps);
            xlator.out.free();
            chanFir.out.free();
            quad.out.free();
            audioFir.out.free();
            cbuf = buffer::alloc<complex_t>(STREAM_BUFFER_SIZE);
            cbuf2 = buffer::alloc<complex_t>(STREAM_BUFFER_SIZE);
            fbuf = buffer::alloc<float>(STREAM_BUFFER_SIZE);
            dcAlpha = 2.0f * 3.14159265f * 20.0f / (float)samplerate;
            _init = true;
        }

        void setCarrier(double carrier) {
            if (!_init) { return; }
            std::lock_guard<std::mutex> lck(mtx);
            _carrier = carrier;
            xlator.setOffset(-carrier, _samplerate);
        }

        double carrier() const { return _carrier; }

        void setGain(float gain) { _gain = gain; }

        void reset() {
            if (!_init) { return; }
            std::lock_guard<std::mutex> lck(mtx);
            xlator.reset();
            chanFir.reset();
            quad.reset();
            audioFir.reset();
            dc = 0.0f;
        }

        // The composite in, the subcarrier's audio out (same count).
        int process(int count, const float* in, float* out) {
            if (!_init) { memset(out, 0, count * sizeof(float)); return count; }
            std::lock_guard<std::mutex> lck(mtx);
            for (int i = 0; i < count; i++) { cbuf[i] = { in[i], 0.0f }; }
            xlator.process(count, cbuf, cbuf2);
            chanFir.process(count, cbuf2, cbuf);
            quad.process(count, cbuf, fbuf);
            audioFir.process(count, fbuf, out);
            for (int i = 0; i < count; i++) {
                dc += dcAlpha * (out[i] - dc);   // a frequency error of the subcarrier is a constant, not audio
                out[i] = (out[i] - dc) * _gain;
            }
            return count;
        }

    private:
        void release() {
            if (!_init) { return; }
            buffer::free(cbuf);
            buffer::free(cbuf2);
            buffer::free(fbuf);
            taps::free(chanTaps);
            taps::free(audioTaps);
            _init = false;
        }

        bool _init = false;
        double _samplerate = 250000.0, _carrier = 67000.0;
        float _gain = 0.5f, dc = 0.0f, dcAlpha = 0.0f;
        std::mutex mtx;
        channel::FrequencyXlator xlator;
        tap<float> chanTaps, audioTaps;
        filter::FIR<complex_t, float> chanFir;
        Quadrature quad;
        filter::FIR<float, float> audioFir;
        complex_t* cbuf = NULL;
        complex_t* cbuf2 = NULL;
        float* fbuf = NULL;
    };

    // A running spectrum of the composite from 0 to 100 kHz, for finding subcarriers: the pilot, the stereo difference, RDS and SCA show as humps.
    class MultiplexSpectrum {
    public:
        static constexpr int N = 16384;
        static constexpr int POINTS = 500;

        MultiplexSpectrum() {}
        ~MultiplexSpectrum() { release(); }

        void init(double samplerate) {
            release();
            _samplerate = samplerate;
            in = (float*)fftwf_malloc(sizeof(float) * N);
            spec = (fftwf_complex*)fftwf_malloc(sizeof(fftwf_complex) * (N / 2 + 1));
            plan = fftwf_plan_dft_r2c_1d(N, in, spec, FFTW_ESTIMATE);
            win.resize(N);
            for (int i = 0; i < N; i++) { win[i] = 0.5f - 0.5f * cosf(2.0f * 3.14159265f * i / N); }
            fill = 0;
            frames = 0;
            buf.assign(N, 0.0f);
            power.assign(N / 2 + 1, 0.0f);
            _init = true;
        }

        void setEnabled(bool e) { enabled = e; }

        void feed(int count, const float* data) {
            if (!_init || !enabled) { return; }
            for (int i = 0; i < count; i++) {
                buf[fill++] = data[i];
                if (fill == N) {
                    frame();
                    // keep the newest half for 50% overlap
                    std::copy(buf.begin() + N / 2, buf.end(), buf.begin());
                    fill = N / 2;
                }
            }
        }

        // POINTS values in dB from 0 to 100 kHz (the strongest bin of each stretch); false until there is a frame.
        bool get(std::vector<float>& db) {
            std::lock_guard<std::mutex> lck(mtx);
            if (frames == 0) { return false; }
            db = shown;
            return true;
        }

    private:
        void frame() {
            for (int i = 0; i < N; i++) { in[i] = buf[i] * win[i]; }
            fftwf_execute(plan);
            const float norm = 1.0f / ((float)N * 0.5f * (float)N * 0.5f);
            const float a = frames == 0 ? 1.0f : 0.2f;
            const int top = (int)(100000.0 / _samplerate * N);
            for (int i = 0; i <= top && i <= N / 2; i++) {
                const float p = (spec[i][0] * spec[i][0] + spec[i][1] * spec[i][1]) * norm;
                power[i] += a * (p - power[i]);
            }
            std::vector<float> out(POINTS);
            for (int k = 0; k < POINTS; k++) {
                const int lo = k * top / POINTS, hi = std::max(lo + 1, (k + 1) * top / POINTS);
                float m = 0.0f;
                for (int i = lo; i < hi; i++) { m = std::max(m, power[i]); }
                out[k] = 10.0f * log10f(m + 1e-20f);
            }
            std::lock_guard<std::mutex> lck(mtx);
            shown = out;
            frames++;
        }

        void release() {
            if (!_init) { return; }
            fftwf_destroy_plan(plan);
            fftwf_free(in);
            fftwf_free(spec);
            _init = false;
        }

        bool _init = false;
        bool enabled = false;
        double _samplerate = 250000.0;
        float* in = NULL;
        fftwf_complex* spec = NULL;
        fftwf_plan plan = NULL;
        std::vector<float> win, buf, power, shown;
        int fill = 0;
        uint64_t frames = 0;
        std::mutex mtx;
    };
}
