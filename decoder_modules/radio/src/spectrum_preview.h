#pragma once
#include <signal_path/signal_path.h>
#include <dsp/channel/rx_vfo.h>
#include <dsp/buffer/reshaper.h>
#include <dsp/sink/handler_sink.h>
#include <dsp/window/nuttall.h>
#include <dsp/multirate/power_decimator.h>
#include <fftw3.h>
#include <volk/volk.h>
#include <mutex>
#include <algorithm>
#include <cmath>

// A small, dedicated spectrum feed for the Radio module's filter-preview widget (see
// RADIO_SPECTRUM_FILTER_PLAN.md). Deliberately not the main gui::waterfall's FFT: that one is
// cropped to whatever the user currently has the main view panned/zoomed to, which reproduces
// exactly the resolution problem this feature exists to fix if the main view is zoomed out (a
// 10-40kHz slice of a multi-MHz-wide view would only be a handful of that FFT's bins), and it
// only covers exactly the current passband on the *demodulator's own* VFO output, whereas this
// widget also needs to show the adjacent-channel spectrum around it. So this creates a second,
// invisible RxVFO of its own -- the same call (IQFrontEnd::addVFO(), bypassing VFOManager)
// VFOManager::VFO itself uses for the real one, minus the ImGui::WaterfallVFO half that would
// make it show up as a second box on the main waterfall -- sized a few times wider than the
// current demod bandwidth, with its own small dedicated FFT computed directly off that wider
// slice. That gives genuinely good, self-contained resolution regardless of the main
// waterfall's zoom/pan state, at the cost of a second lightweight VFO extraction + FFT
// pipeline running per RadioModule instance.
class RadioSpectrumPreview {
public:
    ~RadioSpectrumPreview() { deinit(); }

    void init(std::string name, double offset, double width) {
        _name = name + "_spectrum_preview";
        _width = snapWidth(width);

        dspVFO = sigpath::iqFrontEnd.addVFO(_name, _width, _width, offset);
        if (!dspVFO) { return; }

        reshape.init(&dspVFO->out, FFT_SIZE, 0);
        updateReshapeRate();
        fftSink.init(&reshape.out, fftHandler, this);

        fftInBuf = (fftwf_complex*)fftwf_malloc(FFT_SIZE * sizeof(fftwf_complex));
        fftOutBuf = (fftwf_complex*)fftwf_malloc(FFT_SIZE * sizeof(fftwf_complex));
        fftPlan = fftwf_plan_dft_1d(FFT_SIZE, fftInBuf, fftOutBuf, FFTW_FORWARD, FFTW_ESTIMATE);

        std::lock_guard<std::mutex> lck(dbMtx);
        dbOut = dsp::buffer::alloc<float>(FFT_SIZE);
        for (int i = 0; i < FFT_SIZE; i++) { dbOut[i] = -200.0f; }

        reshape.start();
        fftSink.start();
        _init = true;
    }

    void deinit() {
        if (!_init) { return; }
        _init = false;
        fftSink.stop();
        reshape.stop();
        sigpath::iqFrontEnd.removeVFO(_name);
        fftwf_destroy_plan(fftPlan);
        fftwf_free(fftInBuf);
        fftwf_free(fftOutBuf);
        std::lock_guard<std::mutex> lck(dbMtx);
        dsp::buffer::free(dbOut);
        dbOut = NULL;
    }

    void setOffset(double offset) {
        if (!_init) { return; }
        dspVFO->setOffset(offset);
    }

    // Width in Hz of the slice this preview covers, centered on its offset -- not the
    // passband being demodulated, the wider window the preview shows *around* it.
    //
    // Meant to be called every frame, not just on bandwidth changes: snapWidth() derives its
    // answer from the source's *current* actual sample rate, and that can change for reasons
    // that have nothing to do with this preview at all (RSR200 settling its ADC clock/GPS
    // discipline at startup was what actually caught this live -- "New DSP samplerate:
    // 1953125" followed shortly by "...976562.5" in the same session). RxVFO::setInSamplerate()
    // (called on every such change, for every VFO including this one) only updates the
    // *input* side of the resampler ratio and leaves whatever output rate was already snapped
    // in place -- so a width that was a clean power-of-two fraction of the old rate can silently
    // stop being one the moment the rate changes underneath it, reintroducing the exact
    // multi-million-tap pathology snapWidth() exists to avoid, with nothing to ever notice and
    // re-snap it. Re-deriving here every frame is the fix: cheap when nothing has changed
    // (one snapWidth() call, no-op setOutSamplerate below since the guard skips it), self-
    // healing when it has, and doesn't depend on catching every possible external trigger for
    // "the source's rate changed" -- there isn't a clean single hook for that to catch instead.
    void setWidth(double desiredWidth) {
        if (!_init) { return; }
        double snapped = snapWidth(desiredWidth);
        if (snapped == _width) { return; }
        _width = snapped;
        dspVFO->setOutSamplerate(_width, _width);
        // RxVFO::setOutSamplerate() only clamps the existing passband against the new sample
        // rate -- it deliberately never widens _passbandLo/_passbandHi back out to match a
        // *wider* new bandwidth (see its own comment: real VFOs always get an explicit
        // setPassband() call right after, from applyPassbandEdges(), so re-deriving a fresh
        // symmetric passband here too would just be redundant work there). This preview VFO is
        // the one caller that never makes that follow-up call, since it has no passband concept
        // of its own -- it only ever wants the full raw width, unfiltered. Without this, any
        // resize to a *wider* width than whatever passband was last in effect here (which can be
        // the very first non-default width ever set, since init() starts one from a default 40kHz
        // that's promptly resized once the real mode's preview width is known) left the old,
        // narrower passband quietly still in effect -- RxVFO's filterNeeded flag doesn't know
        // "narrower than the new full width" from "user deliberately trimmed it", so it kept
        // running that stale filter over what's supposed to be raw wide context. Found live:
        // AM<->SAM never showed this because both have the same default bandwidth, so the
        // snapped preview width never actually changes between them and the `snapped == _width`
        // no-op above skips this whole path -- DSB/USB have different default bandwidths, so
        // resizing (and the corruption) happens every single time. The rendered symptom was a
        // smooth, low-detail dome in place of the real spiky spectrum -- exactly what plotting a
        // filter's own passband response looks like instead of live signal, and it never healed
        // on its own since nothing else was ever going to call setPassband() here again to fix
        // it. Explicitly forcing it back to the new full width, every time, is the fix.
        dspVFO->setPassband(-_width / 2.0, _width / 2.0);
        updateReshapeRate();
    }

    double getWidth() { return _width; }

    // Thread-safe access to the latest computed spectrum: FFT_SIZE dB-magnitude bins spanning
    // getWidth() Hz, bin 0 = the low edge. Mirrors WaterFall::acquireLatestFFT()/
    // releaseLatestFFT()'s own lock-and-return-pointer shape. Returns NULL if not yet
    // initialized or no data has been computed yet.
    float* acquireFFT(int& size) {
        dbMtx.lock();
        size = FFT_SIZE;
        return dbOut;
    }

    void releaseFFT() { dbMtx.unlock(); }

    static constexpr int FFT_SIZE = 512;

private:
    // dsp::multirate::RationalResampler reduces outSamplerate/inSamplerate to lowest terms and
    // builds a polyphase FIR sized off the resulting interp/decim -- fine for a "nice" ratio,
    // catastrophic for an unlucky one. Caught live: an RSR200 at a non-power-of-two-friendly
    // sample rate (1953125Hz) resampling towards an arbitrary round preview width produced a
    // resampler asking for **1,159,667 taps**, pegging every CPU core building and then running
    // that filter continuously. Rather than pick an arbitrary width and hope the ratio against
    // whatever the source's actual rate happens to be is reasonable, snap to the nearest
    // power-of-two decimation of the real input rate instead: RationalResampler's own
    // predecimation stage is exactly a power-of-two decimator, so this makes interp/decim
    // reduce to 1:1 and the whole polyphase filter step gets skipped entirely (see its
    // `if (interp == decim)` fast path) rather than merely reduced. This trades an exact
    // "4x the current bandwidth" width for "closest power-of-two-of-the-input-rate to 4x the
    // current bandwidth" -- close enough for what's a rough visual guide, not a precise value
    // anything downstream depends on.
    double snapWidth(double desiredWidth) {
        double effectiveSr = sigpath::iqFrontEnd.getEffectiveSamplerate();
        if (effectiveSr <= 0.0) { return std::max(desiredWidth, 1000.0); }
        int shift = 0;
        int maxShift = (int)std::round(std::log2((double)dsp::multirate::PowerDecimator<dsp::complex_t>::getMaxRatio()));
        while (shift < maxShift && (effectiveSr / (double)(1LL << (shift + 1))) >= desiredWidth) { shift++; }
        return std::max(effectiveSr / (double)(1LL << shift), 1000.0);
    }

    void updateReshapeRate() {
        // Same shape as IQFrontEnd::genReshapeParams(): cap how many samples get windowed at
        // whatever the update rate can actually afford, zero-padding the rest, rather than
        // letting a narrow-bandwidth mode's slower preview samplerate demand more samples per
        // FFT than its own update interval provides (see clampPassband()'s equivalent concern
        // over in rx_vfo.h -- same "a control fed from something the user can shrink a lot
        // needs its own floor" pattern).
        int interval = std::max(1, (int)std::round(_width / FFT_RATE_HZ));
        nzSize = std::min(interval, FFT_SIZE);
        reshape.setKeep(nzSize);
        reshape.setSkip(interval - nzSize);
    }

    static void fftHandler(dsp::complex_t* data, int count, void* ctx) {
        RadioSpectrumPreview* _this = (RadioSpectrumPreview*)ctx;

        for (int i = 0; i < _this->nzSize; i++) {
            // Nuttall window with an alternating sign, exactly IQFrontEnd::updateFFTPath()'s
            // own trick: pre-rotates the spectrum by half the FFT size so bin 0 of the output
            // lands on the *low* edge of the span instead of DC-then-wraparound, without a
            // separate fftshift pass.
            float w = dsp::window::nuttall(i, _this->nzSize) * ((i % 2) ? -1.0f : 1.0f);
            _this->fftInBuf[i][0] = data[i].re * w;
            _this->fftInBuf[i][1] = data[i].im * w;
        }
        for (int i = _this->nzSize; i < FFT_SIZE; i++) {
            _this->fftInBuf[i][0] = 0.0f;
            _this->fftInBuf[i][1] = 0.0f;
        }

        fftwf_execute(_this->fftPlan);

        std::lock_guard<std::mutex> lck(_this->dbMtx);
        if (_this->dbOut) {
            volk_32fc_s32f_power_spectrum_32f(_this->dbOut, (lv_32fc_t*)_this->fftOutBuf, FFT_SIZE, FFT_SIZE);
        }
    }

    static constexpr double FFT_RATE_HZ = 20.0;

    bool _init = false;
    std::string _name;
    double _width;
    int nzSize = FFT_SIZE;

    dsp::channel::RxVFO* dspVFO = NULL;
    dsp::buffer::Reshaper<dsp::complex_t> reshape;
    dsp::sink::Handler<dsp::complex_t> fftSink;

    fftwf_complex* fftInBuf = NULL;
    fftwf_complex* fftOutBuf = NULL;
    fftwf_plan fftPlan;

    std::mutex dbMtx;
    float* dbOut = NULL;
};
