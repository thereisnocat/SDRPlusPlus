#pragma once
#include "../block.h"
#include "../buffer/buffer.h"
#include <mutex>
#include <atomic>
#include <cmath>
#include <cstring>

namespace dsp::combine {

    // Two-channel antenna phasing combiner. See PHASING_PLAN.md section 2.2.
    //
    //   Y[n] = A[n] - w * B[n]
    //
    // w is a complex weight whose magnitude and argument are exactly the gain and phase
    // knobs of a physical phasing box. w = 0 passes A alone, w = -1 gives the sum, w = +1
    // gives the difference. Choosing w to cancel a signal present in both channels nulls
    // that signal; the weight that does so is Wiener-optimal at A*conj(B)/|B|^2, which is
    // what the adaptive mode of a later phase will solve for.
    //
    // Deliberately NOT built on dsp::Operator. Operator::run() reads both inputs and, if
    // the counts disagree, flushes both and returns 0 -- silently discarding samples --
    // and dsp::stream::flush() is all-or-nothing with no partial-consumption path. Two
    // upstream resamplers emitting different block sizes would therefore glitch. This
    // block keeps its own accumulation buffers and consumes min(availA, availB) per pass,
    // retaining the remainder. discardCount is instrumented so a test can assert that
    // nothing was ever dropped.
    class Phaser : public block {
    public:
        enum Mode {
            MODE_A_ONLY,   // pass channel A through untouched; the bypass state
            MODE_B_ONLY,   // pass channel B through untouched
            MODE_MANUAL    // Y = A - w*B with the user's weight
        };

        struct Metrics {
            float powerA = 0.0f;    // mean |A|^2 over the last processed block
            float powerB = 0.0f;
            float powerOut = 0.0f;
        };

        Phaser() {}

        Phaser(stream<complex_t>* a, stream<complex_t>* b) { init(a, b); }

        virtual ~Phaser() {
            if (!_block_init) { return; }
            stop();
            buffer::free(bufA);
            buffer::free(bufB);
            _block_init = false;
        }

        void init(stream<complex_t>* a, stream<complex_t>* b) {
            _a = a;
            _b = b;
            bufA = buffer::alloc<complex_t>(CAPACITY);
            bufB = buffer::alloc<complex_t>(CAPACITY);
            sizeA = 0;
            sizeB = 0;
            registerInput(_a);
            registerInput(_b);
            registerOutput(&out);
            _block_init = true;
        }

        void setInputs(stream<complex_t>* a, stream<complex_t>* b) {
            assert(_block_init);
            std::lock_guard<std::recursive_mutex> lck(ctrlMtx);
            tempStop();
            unregisterInput(_a);
            unregisterInput(_b);
            _a = a;
            _b = b;
            registerInput(_a);
            registerInput(_b);
            sizeA = 0;
            sizeB = 0;
            tempStart();
        }

        void setMode(Mode mode) {
            std::lock_guard<std::mutex> lck(paramMtx);
            _mode = mode;
        }

        Mode getMode() {
            std::lock_guard<std::mutex> lck(paramMtx);
            return _mode;
        }

        // Weight as the user thinks of it: a gain in dB and a phase in degrees.
        void setWeight(float gainDb, float phaseDeg) {
            const float mag = std::pow(10.0f, gainDb / 20.0f);
            const float rad = phaseDeg * (float)(M_PI / 180.0);
            std::lock_guard<std::mutex> lck(paramMtx);
            _gainDb = gainDb;
            _phaseDeg = phaseDeg;
            _target = { mag * std::cos(rad), mag * std::sin(rad) };
        }

        void getWeight(float& gainDb, float& phaseDeg) {
            std::lock_guard<std::mutex> lck(paramMtx);
            gainDb = _gainDb;
            phaseDeg = _phaseDeg;
        }

        complex_t getWeightComplex() {
            std::lock_guard<std::mutex> lck(paramMtx);
            return _target;
        }

        Metrics getMetrics() {
            Metrics m;
            m.powerA = powerA.load(std::memory_order_relaxed);
            m.powerB = powerB.load(std::memory_order_relaxed);
            m.powerOut = powerOut.load(std::memory_order_relaxed);
            return m;
        }

        // Cancellation achieved, in dB: how far the output sits below channel A. This is
        // the number the user is actually optimising when turning the knobs.
        float getNullDepth() {
            const float a = powerA.load(std::memory_order_relaxed);
            const float o = powerOut.load(std::memory_order_relaxed);
            if (a <= 0.0f) { return 0.0f; }
            // Floor the residual rather than special-casing zero. A perfect cancellation
            // would otherwise report 0 dB, which reads as "no cancellation at all" -- the
            // exact opposite of what happened.
            return 10.0f * std::log10(a / std::max(o, 1e-20f));
        }

        // Samples dropped because an input ran far enough ahead to overrun the holding
        // buffer. Must stay at zero in normal operation; a non-zero value means the two
        // channels are badly out of step, not merely block-misaligned.
        uint64_t getDiscardCount() { return discardCount.load(std::memory_order_relaxed); }

        // Only safe while the block is stopped, or from inside tempStop/tempStart.
        void reset() {
            assert(_block_init);
            std::lock_guard<std::recursive_mutex> lck(ctrlMtx);
            tempStop();
            sizeA = 0;
            sizeB = 0;
            discardCount.store(0, std::memory_order_relaxed);
            {
                std::lock_guard<std::mutex> lck2(paramMtx);
                _current = _target;
            }
            tempStart();
        }

        int run() {
            // Top up whichever channel is behind, rather than reading one buffer from
            // each per pass. Reading both in lockstep would require the two inputs to
            // deliver the same *number of reads*, not merely the same number of samples:
            // a channel arriving in 4800-sample blocks would run dry while one arriving
            // in 3000-sample blocks still had data queued, and this block would then wait
            // forever on the exhausted side while the other backed up behind it.
            //
            // Reading only the shorter side also bounds the buffers -- neither can get
            // more than one read ahead of the other -- and leaves the upstream
            // backpressure intact.
            if (sizeA <= sizeB) {
                const int c = _a->read();
                if (c < 0) { return -1; }
                accumulate(bufA, sizeA, _a->readBuf, c);
                _a->flush();
            }
            else {
                const int c = _b->read();
                if (c < 0) { return -1; }
                accumulate(bufB, sizeB, _b->readBuf, c);
                _b->flush();
            }

            // Only as much as both channels can supply; the surplus stays buffered.
            const int count = std::min(sizeA, sizeB);
            if (count <= 0) { return 0; }

            process(count, bufA, bufB, out.writeBuf);

            consume(bufA, sizeA, count);
            consume(bufB, sizeB, count);

            if (!out.swap(count)) { return -1; }
            return count;
        }

        stream<complex_t> out;

    protected:
        // Holding capacity per channel. A single upstream read can be as large as
        // STREAM_BUFFER_SIZE, so this has to be at least that; in practice sources write
        // blocks a few thousand samples long and the retained remainder is tiny.
        static const int CAPACITY = STREAM_BUFFER_SIZE;

        void accumulate(complex_t* buf, int& size, const complex_t* src, int count) {
            if (count <= 0) { return; }

            if (count >= CAPACITY) {
                // Pathological: one read alone fills the buffer. Keep the newest.
                discardCount.fetch_add(size + (count - CAPACITY), std::memory_order_relaxed);
                memcpy(buf, src + (count - CAPACITY), CAPACITY * sizeof(complex_t));
                size = CAPACITY;
                return;
            }

            if (size + count > CAPACITY) {
                // Drop the oldest held samples to make room. Dropping the oldest rather
                // than the newest keeps the two channels as close to aligned as possible.
                const int over = size + count - CAPACITY;
                discardCount.fetch_add(over, std::memory_order_relaxed);
                memmove(buf, buf + over, (size - over) * sizeof(complex_t));
                size -= over;
            }

            memcpy(buf + size, src, count * sizeof(complex_t));
            size += count;
        }

        static void consume(complex_t* buf, int& size, int count) {
            const int left = size - count;
            if (left > 0) { memmove(buf, buf + count, left * sizeof(complex_t)); }
            size = left;
        }

        void process(int count, const complex_t* a, const complex_t* b, complex_t* outBuf) {
            Mode mode;
            complex_t target;
            {
                std::lock_guard<std::mutex> lck(paramMtx);
                mode = _mode;
                target = _target;
            }

            if (mode == MODE_A_ONLY) {
                memcpy(outBuf, a, count * sizeof(complex_t));
                _current = target;
            }
            else if (mode == MODE_B_ONLY) {
                memcpy(outBuf, b, count * sizeof(complex_t));
                _current = target;
            }
            else {
                // Ramp the weight across the block instead of snapping to it. A step
                // change in w puts a discontinuity in the output that is plainly audible
                // as a click while the user is sweeping a control.
                const float invN = 1.0f / (float)count;
                const float dRe = (target.re - _current.re) * invN;
                const float dIm = (target.im - _current.im) * invN;
                float wRe = _current.re;
                float wIm = _current.im;

                for (int i = 0; i < count; i++) {
                    outBuf[i].re = a[i].re - (wRe * b[i].re - wIm * b[i].im);
                    outBuf[i].im = a[i].im - (wRe * b[i].im + wIm * b[i].re);
                    wRe += dRe;
                    wIm += dIm;
                }
                _current = target;
            }

            updateMetrics(count, a, b, outBuf);
        }

        void updateMetrics(int count, const complex_t* a, const complex_t* b, const complex_t* y) {
            // sum(x * conj(x)) is sum|x|^2, so one volk call per channel.
            lv_32fc_t acc;
            const float invN = 1.0f / (float)count;

            volk_32fc_x2_conjugate_dot_prod_32fc(&acc, (const lv_32fc_t*)a, (const lv_32fc_t*)a, count);
            powerA.store(lv_creal(acc) * invN, std::memory_order_relaxed);

            volk_32fc_x2_conjugate_dot_prod_32fc(&acc, (const lv_32fc_t*)b, (const lv_32fc_t*)b, count);
            powerB.store(lv_creal(acc) * invN, std::memory_order_relaxed);

            volk_32fc_x2_conjugate_dot_prod_32fc(&acc, (const lv_32fc_t*)y, (const lv_32fc_t*)y, count);
            powerOut.store(lv_creal(acc) * invN, std::memory_order_relaxed);
        }

        stream<complex_t>* _a = NULL;
        stream<complex_t>* _b = NULL;

        complex_t* bufA = NULL;
        complex_t* bufB = NULL;
        int sizeA = 0;
        int sizeB = 0;

        std::mutex paramMtx;
        Mode _mode = MODE_A_ONLY;
        complex_t _target = { 0.0f, 0.0f };
        float _gainDb = -1000.0f;
        float _phaseDeg = 0.0f;

        // Worker-thread only.
        complex_t _current = { 0.0f, 0.0f };

        std::atomic<float> powerA{ 0.0f };
        std::atomic<float> powerB{ 0.0f };
        std::atomic<float> powerOut{ 0.0f };
        std::atomic<uint64_t> discardCount{ 0 };
    };
}
