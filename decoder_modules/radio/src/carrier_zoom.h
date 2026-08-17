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
#include <deque>
#include <vector>
#include <string>
#include <algorithm>
#include <cmath>

// The DSP feed for the Radio module's single-frequency high-resolution carrier zoom (see
// CARRIER_ZOOM_PLAN.md). Structurally the same shape as RadioSpectrumPreview
// (spectrum_preview.h) -- its own invisible IQFrontEnd::addVFO() tap, its own FFTW plan,
// thread-safe acquire/release of the latest result -- but tuned for the opposite trade: narrow,
// fine, and slow instead of wide, coarse, and fast, with resolution and update rate as two
// independent, user-adjustable free sliders rather than one fixed internal rate.
//
// Unlike RadioSpectrumPreview, this is meant to be a single, app-wide instance (see
// CARRIER_ZOOM_PLAN.md's "no multiple simultaneous windows" decision) -- this class itself has
// no opinion about that, it's just a single VFO-tap-plus-FFT feed that can be pointed at any one
// offset at a time; the singleton/retargeting policy lives in whatever owns the one instance of
// this class (see RadioModule's own wiring).
class CarrierZoomView {
public:
    ~CarrierZoomView() { deinit(); }

    // "Look and see" starting points, not derived from anything -- see CARRIER_ZOOM_PLAN.md's
    // open questions. Ralph resolved the width default explicitly (500Hz total, +/-250Hz);
    // resolution/update-rate defaults are a first guess, adjustable by feel once this is
    // actually running against a real graveyard channel.
    static constexpr double DEFAULT_WIDTH_HZ = 500.0;
    static constexpr double DEFAULT_RESOLUTION_HZ = 2.0;
    static constexpr double DEFAULT_UPDATE_INTERVAL_SEC = 1.0;

    // Slider ranges. MIN_UPDATE_INTERVAL_SEC is also a practical floor on the fastest achievable
    // update rate (see setUpdateIntervalSec()'s own comment) -- without one, a fast update-rate
    // slider position combined with a fine resolution position would demand recomputing a large
    // FFT many times a second indefinitely, and (separately) push the 60-second-capped history
    // buffer's row count high enough to matter for CarrierZoomPlot's own per-frame texture
    // rebuild cost.
    static constexpr double MIN_RESOLUTION_HZ = 0.2;
    static constexpr double MAX_RESOLUTION_HZ = 20.0;
    static constexpr double MIN_UPDATE_INTERVAL_SEC = 0.2;
    static constexpr double MAX_UPDATE_INTERVAL_SEC = 10.0;

    // Per Ralph (2026-08-16): "History can be relatively brief, no more than a minute."
    static constexpr double HISTORY_SECONDS = 60.0;

    // Hard ceiling/floor on the FFT size itself, independent of the resolution slider's own Hz
    // range -- guards against a pathological width/resolution combination (e.g. a very wide zoom
    // with the finest resolution) asking for an unreasonably large or degenerately small FFT.
    static constexpr int MIN_WINDOW_SIZE = 32;
    static constexpr int MAX_WINDOW_SIZE = 65536;

    void init(const std::string& name, double offset) {
        _name = name + "_carrier_zoom";
        _widthHz = snapWidth(DEFAULT_WIDTH_HZ);
        _resolutionHz = DEFAULT_RESOLUTION_HZ;
        _updateIntervalSec = DEFAULT_UPDATE_INTERVAL_SEC;
        _windowSize = 0;
        _hopSize = 0;

        dspVFO = sigpath::iqFrontEnd.addVFO(_name, _widthHz, _widthHz, offset);
        if (!dspVFO) { return; }

        applyWindowAndHop(true);

        reshape.init(&dspVFO->out, _windowSize, _hopSize - _windowSize);
        fftSink.init(&reshape.out, fftHandler, this);

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
        freeFFT();
        std::lock_guard<std::mutex> lck(histMtx);
        history.clear();
    }

    bool isInit() { return _init; }

    void setOffset(double offset) {
        if (!_init) { return; }
        dspVFO->setOffset(offset);
    }

    // Zoom width -- the total span shown, centered on the tuned frequency. Snapped to a clean
    // power-of-two decimation of the source's live rate, exactly RadioSpectrumPreview's own
    // snapWidth() reasoning (see that class and RADIO_SPECTRUM_FILTER_PLAN.md's postmortem on
    // the 1.16-million-tap resampler bug this protects against).
    void setWidth(double desiredWidthHz) {
        if (!_init) { return; }
        double snapped = snapWidth(desiredWidthHz);
        if (snapped == _widthHz) { return; }
        _widthHz = snapped;
        dspVFO->setOutSamplerate(_widthHz, _widthHz);
        // Same fix RadioSpectrumPreview::setWidth() needed (RADIO_SPECTRUM_FILTER_PLAN.md round
        // 11): setOutSamplerate() only ever narrows an existing passband to fit a smaller rate,
        // it never widens one back out for a larger one -- force the full unfiltered width back
        // explicitly, every time, since this VFO has no passband concept of its own.
        dspVFO->setPassband(-_widthHz / 2.0, _widthHz / 2.0);
        applyWindowAndHop(false);
    }
    double getWidth() { return _widthHz; }

    // Resolution slider (Hz per FFT bin) and update-rate slider (seconds per new row), each
    // independently free per Ralph (2026-08-16) -- see CARRIER_ZOOM_PLAN.md's "Design" section
    // for why this needs an overlapping STFT (window size from resolution, hop size from update
    // rate, hop <= window) rather than the plain non-overlapping scheme RadioSpectrumPreview
    // itself uses, where the two would otherwise be locked together.
    void setResolutionHz(double hz) {
        if (!_init) { return; }
        hz = std::clamp(hz, MIN_RESOLUTION_HZ, MAX_RESOLUTION_HZ);
        if (hz == _resolutionHz) { return; }
        _resolutionHz = hz;
        applyWindowAndHop(false);
    }
    double getResolutionHz() { return _resolutionHz; }

    void setUpdateIntervalSec(double sec) {
        if (!_init) { return; }
        sec = std::clamp(sec, MIN_UPDATE_INTERVAL_SEC, MAX_UPDATE_INTERVAL_SEC);
        if (sec == _updateIntervalSec) { return; }
        _updateIntervalSec = sec;
        applyWindowAndHop(false);
    }
    double getUpdateIntervalSecRequested() { return _updateIntervalSec; }
    // The interval actually being achieved, which can differ from the requested value once
    // clamped against the hop-can't-exceed-window floor (see applyWindowAndHop()) -- what the UI
    // should display, not the raw slider value.
    double getUpdateIntervalSecActual() { return (_widthHz > 0.0) ? ((double)_hopSize / _widthHz) : _updateIntervalSec; }

    // Thread-safe access to the current history: rows[0..rowCount) are dB-magnitude arrays of
    // fftSize bins each, oldest-to-newest, spanning spanHz. Mirrors RadioSpectrumPreview's own
    // acquireFFT()/releaseFFT() shape. Returns false (do not use outputs) if not initialized.
    bool acquireHistory(std::vector<const float*>& rows, int& fftSize, double& spanHz) {
        histMtx.lock();
        fftSize = _windowSize;
        spanHz = _widthHz;
        rows.clear();
        if (!_init) { return false; }
        rows.reserve(history.size());
        for (auto& row : history) { rows.push_back(row.data()); }
        return true;
    }
    void releaseHistory() { histMtx.unlock(); }

private:
    // Copied from RadioSpectrumPreview::snapWidth() (spectrum_preview.h) rather than shared --
    // see that function's own extensive comment for the full reasoning (RationalResampler's
    // predecimation stage is exactly a power-of-two decimator, so snapping to one keeps the
    // whole polyphase-filter path skipped entirely rather than merely made smaller). This
    // class's own addVFO() call goes through the exact same RxVFO/resampler machinery, so it's
    // exposed to the identical pathology and needs the identical protection.
    double snapWidth(double desiredWidth) {
        double effectiveSr = sigpath::iqFrontEnd.getEffectiveSamplerate();
        if (effectiveSr <= 0.0) { return (std::max)(desiredWidth, 100.0); }
        int shift = 0;
        int maxShift = (int)std::round(std::log2((double)dsp::multirate::PowerDecimator<dsp::complex_t>::getMaxRatio()));
        while (shift < maxShift && (effectiveSr / (double)(1LL << (shift + 1))) >= desiredWidth) { shift++; }
        return (std::max)(effectiveSr / (double)(1LL << shift), 100.0);
    }

    // Recomputes the FFT window size (from width/resolution) and hop size (from width/update
    // rate), and applies both to the live Reshaper. Reallocates the FFTW plan and clears the
    // history buffer only when the window size itself actually changed -- existing rows are the
    // wrong width to keep once that happens, same "don't try to preserve incompatible state,
    // just restart cleanly" call MiniSpectrum::resetRange() already makes elsewhere in this
    // module's own UI.
    void applyWindowAndHop(bool first) {
        int newWindow = std::clamp((int)std::round(_widthHz / _resolutionHz), MIN_WINDOW_SIZE, MAX_WINDOW_SIZE);
        // Hop can never exceed the window (per CARRIER_ZOOM_PLAN.md: "the update-rate slider's
        // fastest setting is capped [by the sample rate], not at zero" -- the mirror statement
        // is that its *slowest* sensible setting is "wait for one whole fresh window", since a
        // hop wider than that would just be discarding decimated samples between windows for no
        // benefit). minHop enforces MIN_UPDATE_INTERVAL_SEC's wall-clock floor, but can't be
        // pushed past newWindow either, or std::clamp's own lo<=hi invariant would break.
        int minHop = (std::max)(1, (int)std::round(MIN_UPDATE_INTERVAL_SEC * _widthHz));
        minHop = (std::min)(minHop, newWindow);
        int newHop = std::clamp((int)std::round(_updateIntervalSec * _widthHz), minHop, newWindow);

        bool windowChanged = first || (newWindow != _windowSize);
        _windowSize = newWindow;
        _hopSize = newHop;

        if (!first) {
            reshape.setKeep(_windowSize);
            reshape.setSkip(_hopSize - _windowSize);
        }

        if (windowChanged) {
            if (!first) {
                // fftSink's worker thread must not be running while fftInBuf/fftOutBuf/fftPlan
                // are being freed and reallocated below -- stop()/start() (not tempStop(), a
                // full stop) joins that thread, guaranteeing fftHandler() can't be mid-call
                // during the swap. Reallocating a live FFT plan/buffers is the one thing this
                // class does that RadioSpectrumPreview never needed to (its own FFT_SIZE is a
                // fixed compile-time constant, never resized at runtime).
                fftSink.stop();
            }
            allocateFFT();
            if (!first) {
                fftSink.start();
                std::lock_guard<std::mutex> lck(histMtx);
                history.clear();
            }
        }
    }

    void allocateFFT() {
        freeFFT();
        fftInBuf = (fftwf_complex*)fftwf_malloc(_windowSize * sizeof(fftwf_complex));
        fftOutBuf = (fftwf_complex*)fftwf_malloc(_windowSize * sizeof(fftwf_complex));
        fftPlan = fftwf_plan_dft_1d(_windowSize, fftInBuf, fftOutBuf, FFTW_FORWARD, FFTW_ESTIMATE);
        fftAllocated = true;
    }

    void freeFFT() {
        if (!fftAllocated) { return; }
        fftwf_destroy_plan(fftPlan);
        fftwf_free(fftInBuf);
        fftwf_free(fftOutBuf);
        fftAllocated = false;
    }

    int computeMaxRows() {
        double hopIntervalSec = (_widthHz > 0.0) ? ((double)_hopSize / _widthHz) : 1.0;
        if (hopIntervalSec <= 0.0) { return 1; }
        return (std::max)(1, (int)std::ceil(HISTORY_SECONDS / hopIntervalSec));
    }

    // Runs on fftSink's own worker thread. Guaranteed not to run concurrently with
    // allocateFFT()/freeFFT() -- see applyWindowAndHop()'s own comment on why the full
    // fftSink.stop()/start() bracket around a window-size change is there.
    static void fftHandler(dsp::complex_t* data, int count, void* ctx) {
        CarrierZoomView* _this = (CarrierZoomView*)ctx;
        int n = _this->_windowSize;
        if (count < n || !_this->fftAllocated) { return; }

        // Nuttall window with an alternating sign, exactly IQFrontEnd::updateFFTPath()'s and
        // RadioSpectrumPreview's own trick: pre-rotates the spectrum by half the FFT size so
        // bin 0 of the output lands on the *low* edge of the span instead of DC-then-wraparound.
        for (int i = 0; i < n; i++) {
            float w = dsp::window::nuttall(i, n) * ((i % 2) ? -1.0f : 1.0f);
            _this->fftInBuf[i][0] = data[i].re * w;
            _this->fftInBuf[i][1] = data[i].im * w;
        }
        fftwf_execute(_this->fftPlan);

        std::vector<float> row(n);
        volk_32fc_s32f_power_spectrum_32f(row.data(), (lv_32fc_t*)_this->fftOutBuf, n, n);

        std::lock_guard<std::mutex> lck(_this->histMtx);
        _this->history.push_back(std::move(row));
        int maxRows = _this->computeMaxRows();
        while ((int)_this->history.size() > maxRows) { _this->history.pop_front(); }
    }

    bool _init = false;
    std::string _name;
    double _widthHz = DEFAULT_WIDTH_HZ;
    double _resolutionHz = DEFAULT_RESOLUTION_HZ;
    double _updateIntervalSec = DEFAULT_UPDATE_INTERVAL_SEC;
    int _windowSize = 0;
    int _hopSize = 0;

    dsp::channel::RxVFO* dspVFO = NULL;
    dsp::buffer::Reshaper<dsp::complex_t> reshape;
    dsp::sink::Handler<dsp::complex_t> fftSink;

    bool fftAllocated = false;
    fftwf_complex* fftInBuf = NULL;
    fftwf_complex* fftOutBuf = NULL;
    fftwf_plan fftPlan;

    std::mutex histMtx;
    std::deque<std::vector<float>> history;
};
