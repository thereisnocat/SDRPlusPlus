#pragma once
#include "../block.h"
#include "../buffer/buffer.h"
#include "../math/constants.h"
#include "channel_sync.h"
#include "ref_band.h"
#include "decorrelator.h"
#include "wideband_solver.h"
#include "wideband_decorrelator.h"
#include <mutex>
#include <atomic>
#include <cmath>
#include <cstring>
#include <complex>
#include <vector>
#include <algorithm>

namespace dsp::combine {

    // Fractional-sample delay line, used to line the two channels up when their feedlines
    // differ in length. A scalar phasing weight cannot correct a timing offset: it shows
    // up as a phase slope across the band, so the null only works at one spot in the
    // passband. The multi-tap weight of a later phase subsumes this entirely.
    //
    // Interpolation is 4-point cubic (Catmull-Rom), which is ample for the sub-sample
    // trims this is for. At 1 Msps one sample is roughly 200 m of coax, so in practice the
    // useful range is well under a sample and the integer part rarely matters.
    class DelayLine {
    public:
        static const int HISTORY = 64;

        void init() {
            hist.assign(HISTORY, complex_t{ 0.0f, 0.0f });
        }

        void reset() {
            std::fill(hist.begin(), hist.end(), complex_t{ 0.0f, 0.0f });
        }

        // out[n] = in[n - delay], with the delay gliding from delayStart to delayEnd
        // across the block. Ramping matters: a step change in delay is a step change in
        // time, which breaks up audibly while a control is being dragged. Both must be in
        // [2, HISTORY-3] so the interpolator has samples either side to work with.
        void process(const complex_t* in, complex_t* out, int count, float delayStart, float delayEnd) {
            if (count <= 0) { return; }
            if ((int)scratch.size() < HISTORY + count) { scratch.resize(HISTORY + count); }

            memcpy(scratch.data(), hist.data(), HISTORY * sizeof(complex_t));
            memcpy(scratch.data() + HISTORY, in, count * sizeof(complex_t));

            const float lo = 2.0f, hi = (float)(HISTORY - 3);
            const float d0 = (std::min)((std::max)(delayStart, lo), hi);
            const float d1 = (std::min)((std::max)(delayEnd, lo), hi);
            const float step = (d1 - d0) / (float)count;

            if (d0 == d1 && d0 == std::floor(d0)) {
                // Whole-sample delay that is not moving: a straight copy.
                memcpy(out, scratch.data() + HISTORY - (int)d0, count * sizeof(complex_t));
            }
            else {
                for (int n = 0; n < count; n++) {
                    const float d = d0 + step * (float)n;

                    // Work in absolute position rather than splitting the delay itself.
                    // Splitting d into floor/fraction and then interpolating *forward* by
                    // that fraction lands at n - di + f, whose effective delay is di - f,
                    // not di + f: the fractional part pulls the wrong way, and every time
                    // d crosses an integer the position jumps by nearly two samples.
                    const float pos = (float)n - d;
                    const int i = (int)std::floor(pos);
                    const float f = pos - (float)i;

                    // Catmull-Rom through x0..x3, evaluated at f between x1 and x2.
                    const complex_t* p = scratch.data() + HISTORY + i;
                    const complex_t x0 = p[-1], x1 = p[0], x2 = p[1], x3 = p[2];

                    const float a0r = x1.re;
                    const float a1r = 0.5f * (x2.re - x0.re);
                    const float a2r = x0.re - 2.5f * x1.re + 2.0f * x2.re - 0.5f * x3.re;
                    const float a3r = 0.5f * (x3.re - x0.re) + 1.5f * (x1.re - x2.re);
                    out[n].re = ((a3r * f + a2r) * f + a1r) * f + a0r;

                    const float a0i = x1.im;
                    const float a1i = 0.5f * (x2.im - x0.im);
                    const float a2i = x0.im - 2.5f * x1.im + 2.0f * x2.im - 0.5f * x3.im;
                    const float a3i = 0.5f * (x3.im - x0.im) + 1.5f * (x1.im - x2.im);
                    out[n].im = ((a3i * f + a2i) * f + a1i) * f + a0i;
                }
            }

            // Carry the tail forward as the next block's history.
            if (count >= HISTORY) {
                memcpy(hist.data(), in + count - HISTORY, HISTORY * sizeof(complex_t));
            }
            else {
                memmove(hist.data(), hist.data() + count, (HISTORY - count) * sizeof(complex_t));
                memcpy(hist.data() + HISTORY - count, in, count * sizeof(complex_t));
            }
        }

    private:
        std::vector<complex_t> hist;
        std::vector<complex_t> scratch;
    };

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
    // upstream resamplers emitting different block sizes would therefore glitch. It
    // delegates to ChannelSync, which accumulates per channel and hands back only what
    // both can supply, and instruments a discard count so a test can assert that nothing
    // was ever dropped.
    class Phaser : public block {
    public:
        enum Mode {
            MODE_A_ONLY,   // pass channel A through untouched; the bypass state
            MODE_B_ONLY,   // pass channel B through untouched
            MODE_MANUAL,   // Y = A - w*B with the user's weight
            MODE_AUTO,     // same, with w solved for continuously
            MODE_HOLD,     // same, with w frozen wherever adaptation left it
            MODE_DECORR_MIN, // principal component removed: the dominant arrival is nulled
            MODE_DECORR_MAX  // principal component kept: the dominant arrival is peaked
        };

        static bool isCombining(Mode m) { return m != MODE_A_ONLY && m != MODE_B_ONLY; }
        static bool isDecorrelating(Mode m) { return m == MODE_DECORR_MIN || m == MODE_DECORR_MAX; }

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
            _block_init = false;
        }

        void init(stream<complex_t>* a, stream<complex_t>* b) {
            _a = a;
            _b = b;
            sync.init(2);
            delayA.init();
            delayB.init();
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
            sync.reset();
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
            const float rad = phaseDeg * (float)(DB_M_PI / 180.0);
            std::lock_guard<std::mutex> lck(paramMtx);
            _gainDb = gainDb;
            _phaseDeg = phaseDeg;
            _target = { mag * std::cos(rad), mag * std::sin(rad) };
        }

        // Channel B's timing relative to A, in samples; may be fractional and may be
        // negative. Only applied in MODE_MANUAL, so bypass stays bit-exact.
        void setDelay(float samples) {
            std::lock_guard<std::mutex> lck(paramMtx);
            _delay = (std::min)((std::max)(samples, -(float)MAX_DELAY), (float)MAX_DELAY);
        }

        float getDelay() {
            std::lock_guard<std::mutex> lck(paramMtx);
            return _delay;
        }

        // How much of each block's freshly solved weight to take, per block. Small values
        // lock onto the persistent interferer and ignore fades; large ones chase whatever
        // is loudest right now, including the signal you are trying to keep.
        void setAdaptRate(float rate) {
            std::lock_guard<std::mutex> lck(paramMtx);
            _adaptRate = (std::min)((std::max)(rate, 0.0001f), 1.0f);
        }

        float getAdaptRate() {
            std::lock_guard<std::mutex> lck(paramMtx);
            return _adaptRate;
        }

        // Solve a multi-tap weight instead of a single complex one, so the null can vary
        // across the band. Only meaningful while adapting -- nobody hand-tunes 32 taps --
        // so it applies to MODE_AUTO and MODE_HOLD and is ignored in MODE_MANUAL.
        void setWideband(bool enabled, int taps) {
            std::lock_guard<std::mutex> lck(paramMtx);
            _wideband = enabled;
            _wbTaps = std::clamp(taps, 4, 128);
            _wbDirty = true;
        }

        void getWideband(bool& enabled, int& taps) {
            std::lock_guard<std::mutex> lck(paramMtx);
            enabled = _wideband;
            taps = _wbTaps;
        }

        bool isWidebandActive() {
            std::lock_guard<std::mutex> lck(paramMtx);
            return _wideband && (_mode == MODE_AUTO || _mode == MODE_HOLD || isDecorrelating(_mode));
        }

        // True when the active combination is the per-bin decorrelator rather than the
        // scalar eigendecomposition -- distinct from isWidebandActive() because the UI
        // shows different things for it (no single coherence/separation figure means much
        // once the split is frequency-dependent).
        bool isWidebandDecorrelating() {
            std::lock_guard<std::mutex> lck(paramMtx);
            return _wideband && isDecorrelating(_mode);
        }

        // Weighted-by-power mean coherence across bins, from the wideband decorrelator.
        // Meaningful only while isWidebandDecorrelating() is true.
        float getWidebandCoherence() { return wbDecorrCoherence.load(std::memory_order_relaxed); }

        // How many of the FFT bins had enough power to be actively solved on the last
        // pass, versus passed through untouched by the power gate (see
        // wideband_decorrelator.h). A handful active means the gate is behaving like a
        // de-facto reference band around one strong signal; many active means real signal
        // was found across much of the observed span.
        void getWidebandActiveBins(int& active, int& total) {
            active = wbDecorrActiveBins.load(std::memory_order_relaxed);
            total = wbDecorrTotalBins.load(std::memory_order_relaxed);
        }

        // Needed only by the reference band, to place its mixer.
        void setSampleRate(double sampleRate) {
            std::lock_guard<std::mutex> lck(paramMtx);
            _sampleRate = sampleRate;
            _refDirty = true;
        }

        // Restrict adaptation to a slice of spectrum, offset from centre. Without this,
        // minimising output power nulls whatever is loudest, which when the wanted signal
        // peaks is the wanted signal.
        void setReferenceBand(bool enabled, double offsetHz, double widthHz) {
            std::lock_guard<std::mutex> lck(paramMtx);
            _refEnabled = enabled;
            _refOffset = offsetHz;
            _refWidth = widthHz;
            _refDirty = true;
        }

        void getReferenceBand(bool& enabled, double& offsetHz, double& widthHz) {
            std::lock_guard<std::mutex> lck(paramMtx);
            enabled = _refEnabled;
            offsetHz = _refOffset;
            widthHz = _refWidth;
        }

        // Arm a noise measurement. The next second or so of covariance is taken as the
        // noise-only reference and inverted to a whitening transform, after which a
        // maximum-power combination is also a maximum-SNR one. Point the radio at a quiet
        // channel first: whatever is on the air while this runs becomes "noise".
        void captureNoise(double seconds = 1.0) {
            std::lock_guard<std::mutex> lck(paramMtx);
            _noiseCapture = Covariance();
            _noiseCaptureTerms = 0.0;
            // updateCovariance() below feeds this from whichever of RefBand::accumulate
            // (one term per decimation() raw samples -- its own doc calls out "far fewer
            // terms than samples fed in") or ::accumulateWideband (one term per sample)
            // ends up in use, chosen by _refEnabled at the time each block actually runs.
            // The target here has to match that same rate, not assume the whole-span one
            // regardless: with a reference band on, sizing this for one-term-per-sample
            // turned a labelled "1 s" capture into two-plus minutes on real air (a
            // decimation of ~150), correct on the label but not on the clock.
            const double termsPerSample = _refEnabled ? (1.0 / (double)(std::max)(refBand.decimation(), 1)) : 1.0;
            _noiseCaptureWanted = (std::max)(1.0, seconds * _sampleRate * termsPerSample);
            _capturingNoise = true;
        }

        bool isCapturingNoise() {
            std::lock_guard<std::mutex> lck(paramMtx);
            return _capturingNoise;
        }

        bool hasNoiseReference() {
            std::lock_guard<std::mutex> lck(paramMtx);
            return _haveWhitening;
        }

        void setWhiteningEnabled(bool enabled) {
            std::lock_guard<std::mutex> lck(paramMtx);
            _whiteningEnabled = enabled;
        }

        bool getWhiteningEnabled() {
            std::lock_guard<std::mutex> lck(paramMtx);
            return _whiteningEnabled;
        }

        void clearNoiseReference() {
            std::lock_guard<std::mutex> lck(paramMtx);
            _haveWhitening = false;
            _whiteningEnabled = false;
            _whitening = Matrix2();
        }

        // How strongly the two channels agree, 0 to 1. Near 1 means there is a dominant
        // arrival worth separating; low values mean a diffuse mixture and nothing to null.
        float getCoherence() { return coherenceValue.load(std::memory_order_relaxed); }

        // Gap between the two principal components, in dB.
        float getComponentSeparation() { return separationValue.load(std::memory_order_relaxed); }

        // The combination currently being applied, as y = k0*A + k1*B. Only meaningful in
        // the decorrelation modes. Exposed so a radio that can combine in hardware can be
        // handed the solution rather than having it found by hand.
        void getCombineCoefficients(complex_t& k0, complex_t& k1) {
            k0 = { coefA_re.load(std::memory_order_relaxed), coefA_im.load(std::memory_order_relaxed) };
            k1 = { coefB_re.load(std::memory_order_relaxed), coefB_im.load(std::memory_order_relaxed) };
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
        // True when the depth below is measured inside the reference band rather than
        // across the whole spectrum.
        bool isNullDepthBandLimited() {
            std::lock_guard<std::mutex> lck(paramMtx);
            return _refEnabled && _mode != MODE_A_ONLY && _mode != MODE_B_ONLY;
        }

        float getNullDepth() {
            if (isNullDepthBandLimited()) {
                const float bd = bandDepth.load(std::memory_order_relaxed);
                return bd;
            }
            const float a = powerA.load(std::memory_order_relaxed);
            const float o = powerOut.load(std::memory_order_relaxed);
            if (a <= 0.0f) { return 0.0f; }
            // Floor the residual rather than special-casing zero. A perfect cancellation
            // would otherwise report 0 dB, which reads as "no cancellation at all" -- the
            // exact opposite of what happened.
            return 10.0f * std::log10(a / (std::max)(o, 1e-20f));
        }

        // Samples dropped because an input ran far enough ahead to overrun the holding
        // buffer. Must stay at zero in normal operation; a non-zero value means the two
        // channels are badly out of step, not merely block-misaligned.
        uint64_t getDiscardCount() { return sync.discardCount(); }

        // Only safe while the block is stopped, or from inside tempStop/tempStart.
        void reset() {
            assert(_block_init);
            std::lock_guard<std::recursive_mutex> lck(ctrlMtx);
            tempStop();
            sync.reset();
            delayA.reset();
            delayB.reset();
            refBand.reset();
            metricBand.reset();
            {
                std::lock_guard<std::mutex> lck2(paramMtx);
                _current = _target;
                _delayCurrent = _delay;
            }
            tempStart();
        }

        int run() {
            // Top up whichever channel is behind rather than reading one buffer from each
            // per pass; ChannelSync explains why that distinction matters.
            const int ch = sync.nextChannel();
            stream<complex_t>* in = ch ? _b : _a;

            const int c = in->read();
            if (c < 0) { return -1; }
            sync.feed(ch, in->readBuf, c);
            in->flush();

            // Only as much as both channels can supply; the surplus stays buffered.
            const int count = sync.available();
            if (count <= 0) { return 0; }

            process(count, sync.data(0), sync.data(1), out.writeBuf);
            sync.consume(count);

            if (!out.swap(count)) { return -1; }
            return count;
        }

        stream<complex_t> out;

    protected:
        void process(int count, const complex_t* a, const complex_t* b, complex_t* outBuf) {
            Mode mode;
            complex_t target;
            float delay;
            float adaptRate;
            bool refEnabled;
            bool wideband;
            bool widebandDecorr;
            bool decorrelating;
            {
                std::lock_guard<std::mutex> lck(paramMtx);
                mode = _mode;
                target = _target;
                delay = _delay;
                adaptRate = _adaptRate;
                refEnabled = _refEnabled;
                decorrelating = isDecorrelating(_mode);
                wideband = _wideband && (_mode == MODE_AUTO || _mode == MODE_HOLD);
                widebandDecorr = _wideband && decorrelating;
                if (_wbDirty) {
                    wbSolver.configure(1024, _wbTaps, (std::max)(_adaptRate, 0.02f));
                    wbHist.assign((std::max)(_wbTaps - 1, 0), complex_t{ 0.0f, 0.0f });
                    wbTapsRev.clear();
                    // A larger FFT than the Wiener path's: resolving one station out of
                    // several sharing the observed span needs bins finer than the ~10 kHz
                    // spacing between broadcast channels, which is the whole reason this
                    // mode needs no reference band -- see wideband_decorrelator.h.
                    wbDecorr.configure(4096, _wbTaps, (std::max)(_adaptRate, 0.02f));
                    wbDecorrHistA.clear();
                    wbDecorrHistB.clear();
                    _wbDirty = false;
                }
                if (_refDirty) {
                    refBand.configure(_sampleRate, _refOffset, _refWidth);
                    metricBand.configure(_sampleRate, _refOffset, _refWidth);
                    _refDirty = false;
                }
            }

            // Both channels run through a delay line in manual mode, A at a fixed bulk
            // delay and B offset from it, so a negative relative delay is expressible and
            // changing the control does not jump the output. The pass-through modes skip
            // the lines entirely, which is what keeps bypass bit-identical to the input.
            if (isCombining(mode)) {
                if ((int)dlyA.size() < count) { dlyA.resize(count); dlyB.resize(count); }
                if (widebandDecorr) {
                    // Neither channel is privileged here the way Wiener's A is -- y is
                    // k0(f)*A + k1(f)*B, both frequency-dependent -- so both get the same
                    // centring delay rather than only one. See wideband_decorrelator.h.
                    const float align = (float)wbDecorr.alignmentDelay();
                    delayA.process(a, dlyA.data(), count, align, align);
                    delayB.process(b, dlyB.data(), count, align, align);
                    a = dlyA.data();
                    b = dlyB.data();
                }
                else if (wideband) {
                    // The taps span both signs of lag, so A is held back by half the span
                    // and B is left alone -- the filter absorbs any timing difference,
                    // which is why the scalar delay control is retired here.
                    const float align = (float)wbSolver.alignmentDelay();
                    delayA.process(a, dlyA.data(), count, align, align);
                    a = dlyA.data();
                }
                else {
                    delayA.process(a, dlyA.data(), count, (float)BULK_DELAY, (float)BULK_DELAY);
                    delayB.process(b, dlyB.data(), count, (float)BULK_DELAY + _delayCurrent, (float)BULK_DELAY + delay);
                    _delayCurrent = delay;
                    a = dlyA.data();
                    b = dlyB.data();
                }
            }

            // Solve for the weight that cancels whatever the two channels have in common,
            // then take a fraction of it. This is the Wiener solution: minimising
            // E|A - wB|^2 gives w = E[A conj(B)] / E[|B|^2]. A block estimate is both
            // cheaper and steadier than a sample-wise gradient.
            if (mode == MODE_AUTO) {
                std::complex<double> rab(0.0, 0.0);
                double rbb = 0.0;
                if (refEnabled) { refBand.accumulate(a, b, count, rab, rbb); }
                else { RefBand::accumulateWideband(a, b, count, rab, rbb); }

                if (rbb > 1e-20) {
                    const std::complex<double> wOpt = rab / rbb;
                    std::lock_guard<std::mutex> lck(paramMtx);
                    const std::complex<double> wOld(_target.re, _target.im);
                    const std::complex<double> wNew = wOld + (double)adaptRate * (wOpt - wOld);
                    _target = { (float)wNew.real(), (float)wNew.imag() };
                    _gainDb = 20.0f * std::log10((std::max)((float)std::abs(wNew), 1e-12f));
                    _phaseDeg = (float)(std::arg(wNew) * 180.0 / DB_M_PI);
                    target = _target;
                }
            }

            if (widebandDecorr) {
                wbDecorr.feed(a, b, count);
                wbDecorr.copyTaps(mode == MODE_DECORR_MIN, wbDecorrK0Rev, wbDecorrK1Rev);
                // Kept alive so the scalar coherence/separation meters, the noise capture
                // for whitening, and the reference-band null-depth reading still mean
                // something if the operator switches back to scalar mode -- this mode has
                // its own coherence readout (getWidebandCoherence()) but does not touch
                // whitening or the reference band scalar path itself.
                if (decorrelating || _capturingNoise) {
                    updateCovariance(count, a, b, refEnabled, adaptRate);
                }
                applyWidebandDecorrelation(count, a, b, outBuf);
                wbDecorrCoherence.store(wbDecorr.meanCoherence(), std::memory_order_relaxed);
                wbDecorrActiveBins.store(wbDecorr.activeBinCount(), std::memory_order_relaxed);
                wbDecorrTotalBins.store(wbDecorr.fftSize(), std::memory_order_relaxed);
                updateMetrics(count, a, b, outBuf);
                if (refEnabled) { updateBandDepth(count, a, outBuf); }
                return;
            }

            if (wideband) {
                if (mode == MODE_AUTO) { wbSolver.feed(a, b, count); }
                wbSolver.copyTaps(wbTapsRev);
                applyWideband(count, a, b, outBuf);
                updateMetrics(count, a, b, outBuf);
                if (refEnabled) { updateBandDepth(count, a, outBuf); }
                return;
            }

            if (decorrelating || _capturingNoise) {
                updateCovariance(count, a, b, refEnabled, adaptRate);
            }

            if (decorrelating) {
                applyDecorrelation(count, a, b, outBuf, mode);
                updateMetrics(count, a, b, outBuf);
                if (refEnabled) { updateBandDepth(count, a, outBuf); }
                return;
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
            if (refEnabled && isCombining(mode)) { updateBandDepth(count, a, outBuf); }
        }

        // Score the null where the user pointed the solver, not across the whole band.
        void updateBandDepth(int count, const complex_t* a, const complex_t* y) {
            std::complex<double> ignored(0.0, 0.0);
            double pOut = 0.0, pIn = 0.0;
            metricBand.accumulate(a, y, count, ignored, pOut, &pIn);
            if (pIn > 1e-20 && pOut > 1e-20) {
                bandDepth.store((float)(10.0 * std::log10(pIn / pOut)), std::memory_order_relaxed);
            }
        }

        // Y[n] = A[n] - sum_k g[k] B[n-k], with the taps held reversed so each output is a
        // single dot product against a forward-ordered history.
        void applyWideband(int count, const complex_t* a, const complex_t* b, complex_t* outBuf) {
            const int n = (int)wbTapsRev.size();
            if (n < 1) {
                memcpy(outBuf, a, count * sizeof(complex_t));
                return;
            }
            if ((int)wbHist.size() != n - 1) { wbHist.assign(n - 1, complex_t{ 0.0f, 0.0f }); }

            wbLine.resize(n - 1 + count);
            memcpy(wbLine.data(), wbHist.data(), (n - 1) * sizeof(complex_t));
            memcpy(wbLine.data() + (n - 1), b, count * sizeof(complex_t));

            for (int i = 0; i < count; i++) {
                lv_32fc_t acc;
                volk_32fc_x2_dot_prod_32fc(&acc, (const lv_32fc_t*)(wbLine.data() + i),
                                           (const lv_32fc_t*)wbTapsRev.data(), n);
                outBuf[i].re = a[i].re - lv_creal(acc);
                outBuf[i].im = a[i].im - lv_cimag(acc);
            }

            memcpy(wbHist.data(), wbLine.data() + count, (n - 1) * sizeof(complex_t));
        }

        // Y[n] = sum_k k0[k] A[n-k] + sum_k k1[k] B[n-k]. Two filters rather than one,
        // because neither channel is privileged the way applyWideband()'s A is -- see
        // wideband_decorrelator.h. If a solution has not landed yet (still filling its
        // first analysis window), pass A through so there is no discontinuity to click.
        void applyWidebandDecorrelation(int count, const complex_t* a, const complex_t* b, complex_t* outBuf) {
            const int n = (int)wbDecorrK0Rev.size();
            if (n < 1) {
                memcpy(outBuf, a, count * sizeof(complex_t));
                return;
            }
            if ((int)wbDecorrHistA.size() != n - 1) {
                wbDecorrHistA.assign(n - 1, complex_t{ 0.0f, 0.0f });
                wbDecorrHistB.assign(n - 1, complex_t{ 0.0f, 0.0f });
            }

            wbLineA.resize(n - 1 + count);
            wbLineB.resize(n - 1 + count);
            memcpy(wbLineA.data(), wbDecorrHistA.data(), (n - 1) * sizeof(complex_t));
            memcpy(wbLineA.data() + (n - 1), a, count * sizeof(complex_t));
            memcpy(wbLineB.data(), wbDecorrHistB.data(), (n - 1) * sizeof(complex_t));
            memcpy(wbLineB.data() + (n - 1), b, count * sizeof(complex_t));

            for (int i = 0; i < count; i++) {
                lv_32fc_t accA, accB;
                volk_32fc_x2_dot_prod_32fc(&accA, (const lv_32fc_t*)(wbLineA.data() + i),
                                           (const lv_32fc_t*)wbDecorrK0Rev.data(), n);
                volk_32fc_x2_dot_prod_32fc(&accB, (const lv_32fc_t*)(wbLineB.data() + i),
                                           (const lv_32fc_t*)wbDecorrK1Rev.data(), n);
                outBuf[i].re = lv_creal(accA) + lv_creal(accB);
                outBuf[i].im = lv_cimag(accA) + lv_cimag(accB);
            }

            memcpy(wbDecorrHistA.data(), wbLineA.data() + count, (n - 1) * sizeof(complex_t));
            memcpy(wbDecorrHistB.data(), wbLineB.data() + count, (n - 1) * sizeof(complex_t));
        }

        // Accumulate the 2x2 covariance, over the reference band when one is set so the
        // decomposition describes the signal the user pointed at rather than the whole span.
        void updateCovariance(int count, const complex_t* a, const complex_t* b,
                              bool refEnabled, float adaptRate) {
            std::complex<double> rab(0.0, 0.0);
            double rbb = 0.0, raa = 0.0;
            const int terms = refEnabled ? refBand.accumulate(a, b, count, rab, rbb, &raa)
                                         : RefBand::accumulateWideband(a, b, count, rab, rbb, &raa);
            if (terms <= 0) { return; }

            const double inv = 1.0 / (double)terms;
            raa *= inv;
            rbb *= inv;
            rab *= inv;

            std::lock_guard<std::mutex> lck(paramMtx);

            if (_capturingNoise) {
                // Plain running mean: a noise reference wants the whole window weighted
                // equally, not the tail favoured.
                const double n = _noiseCaptureTerms;
                const double m = n + (double)terms;
                _noiseCapture.raa = (_noiseCapture.raa * n + raa * (double)terms) / m;
                _noiseCapture.rbb = (_noiseCapture.rbb * n + rbb * (double)terms) / m;
                _noiseCapture.rab = (_noiseCapture.rab * n + rab * (double)terms) / m;
                _noiseCaptureTerms = m;
                if (m >= _noiseCaptureWanted) {
                    _capturingNoise = false;
                    if (_noiseCapture.valid()) {
                        _whitening = inverseSqrt(_noiseCapture);
                        _haveWhitening = true;
                    }
                }
            }

            const double lambda = (std::min)(1.0, (std::max)(0.001, (double)adaptRate));
            _cov.raa = _cov.raa * (1.0 - lambda) + raa * lambda;
            _cov.rbb = _cov.rbb * (1.0 - lambda) + rbb * lambda;
            _cov.rab = _cov.rab * (1.0 - lambda) + rab * lambda;
        }

        void applyDecorrelation(int count, const complex_t* a, const complex_t* b,
                                complex_t* outBuf, Mode mode) {
            Covariance cov;
            Matrix2 whitening;
            bool whitened = false;
            {
                std::lock_guard<std::mutex> lck(paramMtx);
                cov = _cov;
                whitening = _whitening;
                whitened = _whiteningEnabled && _haveWhitening;
            }

            if (!cov.valid()) {
                memcpy(outBuf, a, count * sizeof(complex_t));
                return;
            }

            const Covariance working = whitened ? transform(cov, whitening) : cov;
            const Eigen2 e = solveEigen2(working);
            coherenceValue.store((float)coherence(cov), std::memory_order_relaxed);
            separationValue.store((float)e.separationDb(), std::memory_order_relaxed);

            std::complex<double> k0, k1;
            combineCoefficients(mode == MODE_DECORR_MIN ? e.uMin : e.uMax,
                                whitening, whitened, k0, k1);

            // Ramp across the block, for the same reason the scalar weight does: the
            // coefficients move every block while adapting, and stepping them clicks.
            const float invN = 1.0f / (float)count;
            const complex_t t0 = { (float)k0.real(), (float)k0.imag() };
            const complex_t t1 = { (float)k1.real(), (float)k1.imag() };
            const float d0r = (t0.re - _k0.re) * invN, d0i = (t0.im - _k0.im) * invN;
            const float d1r = (t1.re - _k1.re) * invN, d1i = (t1.im - _k1.im) * invN;
            float c0r = _k0.re, c0i = _k0.im, c1r = _k1.re, c1i = _k1.im;

            for (int i = 0; i < count; i++) {
                outBuf[i].re = (c0r * a[i].re - c0i * a[i].im) + (c1r * b[i].re - c1i * b[i].im);
                outBuf[i].im = (c0r * a[i].im + c0i * a[i].re) + (c1r * b[i].im + c1i * b[i].re);
                c0r += d0r; c0i += d0i;
                c1r += d1r; c1i += d1i;
            }
            _k0 = t0;
            _k1 = t1;
            coefA_re.store(t0.re, std::memory_order_relaxed);
            coefA_im.store(t0.im, std::memory_order_relaxed);
            coefB_re.store(t1.re, std::memory_order_relaxed);
            coefB_im.store(t1.im, std::memory_order_relaxed);
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

        // Bulk delay applied to both channels in manual mode, so the user's relative
        // delay can swing either way around it.
        static const int BULK_DELAY = 24;
        static const int MAX_DELAY = 16;

        stream<complex_t>* _a = NULL;
        stream<complex_t>* _b = NULL;
        ChannelSync sync;
        DelayLine delayA, delayB;
        RefBand refBand;
        RefBand metricBand;
        WidebandSolver wbSolver;
        std::vector<complex_t> wbTapsRev, wbHist, wbLine;
        WidebandDecorrelator wbDecorr;
        std::vector<complex_t> wbDecorrK0Rev, wbDecorrK1Rev, wbDecorrHistA, wbDecorrHistB, wbLineA, wbLineB;
        std::vector<complex_t> dlyA, dlyB;

        std::mutex paramMtx;
        Mode _mode = MODE_A_ONLY;
        complex_t _target = { 0.0f, 0.0f };
        float _gainDb = -1000.0f;
        float _phaseDeg = 0.0f;
        float _delay = 0.0f;
        float _adaptRate = 0.05f;
        bool _refEnabled = false;
        double _refOffset = 0.0;
        double _refWidth = 20000.0;
        double _sampleRate = 1000000.0;
        bool _refDirty = true;
        Covariance _cov;
        Covariance _noiseCapture;
        Matrix2 _whitening;
        double _noiseCaptureTerms = 0.0;
        double _noiseCaptureWanted = 0.0;
        bool _capturingNoise = false;
        bool _haveWhitening = false;
        bool _whiteningEnabled = false;
        bool _wideband = false;
        int _wbTaps = 32;
        bool _wbDirty = true;

        // Worker-thread only.
        complex_t _current = { 0.0f, 0.0f };
        float _delayCurrent = 0.0f;
        complex_t _k0 = { 1.0f, 0.0f };
        complex_t _k1 = { 0.0f, 0.0f };

        std::atomic<float> powerA{ 0.0f };
        std::atomic<float> powerB{ 0.0f };
        std::atomic<float> powerOut{ 0.0f };
        std::atomic<float> bandDepth{ 0.0f };
        std::atomic<float> coherenceValue{ 0.0f };
        std::atomic<float> separationValue{ 0.0f };
        std::atomic<float> coefA_re{ 1.0f }, coefA_im{ 0.0f };
        std::atomic<float> coefB_re{ 0.0f }, coefB_im{ 0.0f };
        std::atomic<float> wbDecorrCoherence{ 0.0f };
        std::atomic<int> wbDecorrActiveBins{ 0 };
        std::atomic<int> wbDecorrTotalBins{ 1 };
    };
}
