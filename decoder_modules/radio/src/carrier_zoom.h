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

    // Per-carrier peak detection (see CARRIER_PEAK_LABELS_PLAN.md). All four resolved with Ralph
    // 2026-08-17 as fixed "look and see" constants, not sliders -- expected to be retuned once
    // this has run against a real graveyard channel, same treatment already given to
    // DEFAULT_RESOLUTION_HZ/DEFAULT_UPDATE_INTERVAL_SEC above.
    static constexpr float PEAK_PROMINENCE_DB = 6.0f;
    static constexpr int MAX_PEAKS = 5;
    // Roughly twice a 4-term Nuttall window's own mainlobe half-width, so two accepted peaks are
    // almost certainly two distinct signals rather than one carrier's own mainlobe shoulder
    // counted twice.
    static constexpr int MIN_PEAK_SEPARATION_BINS = 8;
    // A tracked peak survives this many consecutive detection misses before being dropped --
    // absorbs a real signal briefly dipping below the prominence threshold for one hop without
    // its line/label flickering out and back.
    static constexpr int MAX_PEAK_MISSES = 3;
    // Fixed exponential-smoothing factor for a tracked peak's offset -- how much of the gap
    // between the tracked value and this frame's fresh detection to close per update. Not ramped
    // the way CarrierZoomPlot's own dB-range convergence is (carrier_zoom_plot.cpp) -- that one
    // needs to snap quickly right after a reset/retune; a tracked peak's identity is already
    // fresh the moment it's created (see acceptPeak() below), so a single fixed factor is enough.
    static constexpr float PEAK_SMOOTHING_ALPHA = 0.3f;

    struct PeakInfo {
        double offsetHz;
        float magnitudeDb;
    };

    void init(const std::string& name, double offset) {
        _name = name + "_carrier_zoom";
        _widthHz = snapWidth(DEFAULT_WIDTH_HZ);
        _resolutionHz = DEFAULT_RESOLUTION_HZ;
        _updateIntervalSec = DEFAULT_UPDATE_INTERVAL_SEC;
        _windowSize = 0;
        _hopSize = 0;

        dspVFO = sigpath::iqFrontEnd.addVFO(_name, _widthHz, _widthHz, offset);
        if (!dspVFO) { return; }

        recomputeWindowAndHop(true);

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
        trackedPeaks.clear();
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
    //
    // The whole body (VFO rate change included) runs with fftSink stopped -- see this class's
    // own header comment on why: a first version stopped fftSink only around the
    // reshape-resize/FFT-realloc step, leaving dspVFO->setOutSamplerate() itself unprotected,
    // and that gap was enough to produce a real, live heap corruption crash (a different
    // allocation site each time, the classic symptom of a corruption detected long after the
    // write that caused it -- see git history for the two live crash reports this came from).
    void setWidth(double desiredWidthHz) {
        if (!_init) { return; }
        double snapped = snapWidth(desiredWidthHz);
        if (snapped == _widthHz) { return; }
        fftSink.stop();
        _widthHz = snapped;
        dspVFO->setOutSamplerate(_widthHz, _widthHz);
        // Same fix RadioSpectrumPreview::setWidth() needed (RADIO_SPECTRUM_FILTER_PLAN.md round
        // 11): setOutSamplerate() only ever narrows an existing passband to fit a smaller rate,
        // it never widens one back out for a larger one -- force the full unfiltered width back
        // explicitly, every time, since this VFO has no passband concept of its own.
        dspVFO->setPassband(-_widthHz / 2.0, _widthHz / 2.0);
        recomputeWindowAndHop(false);
        fftSink.start();
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
        fftSink.stop();
        _resolutionHz = hz;
        recomputeWindowAndHop(false);
        fftSink.start();
    }
    double getResolutionHz() { return _resolutionHz; }

    void setUpdateIntervalSec(double sec) {
        if (!_init) { return; }
        sec = std::clamp(sec, MIN_UPDATE_INTERVAL_SEC, MAX_UPDATE_INTERVAL_SEC);
        if (sec == _updateIntervalSec) { return; }
        fftSink.stop();
        _updateIntervalSec = sec;
        recomputeWindowAndHop(false);
        fftSink.start();
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

    // Thread-safe access to the latest tracked peaks (see CARRIER_PEAK_LABELS_PLAN.md), same
    // acquire/release-under-histMtx shape as acquireHistory()/releaseHistory() above -- peak
    // tracking lives on the same fftSink worker thread and under the same lock as history itself,
    // so sharing the lock (rather than a second one) avoids a second lock-ordering relationship to
    // reason about for no real benefit. Returns false (do not use peaks) if not initialized.
    bool acquirePeaks(std::vector<PeakInfo>& peaks) {
        histMtx.lock();
        peaks.clear();
        if (!_init) { return false; }
        peaks.reserve(trackedPeaks.size());
        for (auto& tp : trackedPeaks) { peaks.push_back({ tp.offsetHz, tp.magnitudeDb }); }
        return true;
    }
    void releasePeaks() { histMtx.unlock(); }

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
    //
    // Assumes fftSink is already stopped by the caller (setWidth()/setResolutionHz()/
    // setUpdateIntervalSec(), or not yet started at all during init()'s first=true call) --
    // this function itself never touches fftSink, deliberately, so every caller's own
    // stop-mutate-start bracket covers the *entire* mutation (VFO rate change included, for
    // setWidth()) rather than just the part that happens to live in here.
    void recomputeWindowAndHop(bool first) {
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
            allocateFFT();
            if (!first) {
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

    // Finds up to MAX_PEAKS local maxima in one FFT row and refines each to sub-bin precision.
    // See CARRIER_PEAK_LABELS_PLAN.md's "Design" section for the full reasoning behind each step.
    // Runs on fftSink's own worker thread, called from fftHandler() below -- same thread history
    // itself is built on, no separate synchronization needed here.
    void detectPeaks(const std::vector<float>& row, int n, double widthHz, std::vector<PeakInfo>& out) {
        out.clear();
        if (n < 3) { return; }

        // Noise floor: 20th-percentile value, via nth_element rather than a full sort -- O(n)
        // average, cheap even at MAX_WINDOW_SIZE and the fastest allowed hop rate since it only
        // runs once per FFT. A percentile rather than the mean: with at most a handful of real
        // carriers against a narrow span of mostly-noise bins, a low percentile tracks the floor
        // without a real carrier dragging a plain mean upward.
        std::vector<float> sorted(row.begin(), row.begin() + n);
        int floorIdx = (int)(0.2 * (n - 1));
        std::nth_element(sorted.begin(), sorted.begin() + floorIdx, sorted.end());
        float noiseFloor = sorted[floorIdx];
        float threshold = noiseFloor + PEAK_PROMINENCE_DB;

        // Candidate local maxima above the prominence threshold, strongest-first so the
        // minimum-separation accept/reject pass below keeps the tallest peak in any cluster of
        // bins that are all above threshold together (one real carrier's mainlobe is several bins
        // wide at this resolution, not a single bin).
        std::vector<int> candidates;
        for (int i = 1; i < n - 1; i++) {
            if (row[i] > row[i - 1] && row[i] > row[i + 1] && row[i] >= threshold) {
                candidates.push_back(i);
            }
        }
        std::sort(candidates.begin(), candidates.end(), [&](int a, int b) { return row[a] > row[b]; });

        std::vector<int> accepted;
        for (int cand : candidates) {
            if ((int)accepted.size() >= MAX_PEAKS) { break; }
            bool tooClose = false;
            for (int acc : accepted) {
                if (std::abs(cand - acc) < MIN_PEAK_SEPARATION_BINS) { tooClose = true; break; }
            }
            if (!tooClose) { accepted.push_back(cand); }
        }

        for (int bin : accepted) {
            // Parabolic interpolation for sub-bin precision -- a proper local max keeps delta
            // within [-0.5, 0.5] by construction; clamp defensively anyway since row[] is real
            // measured data, not an ideal parabola.
            float y0 = row[bin - 1], y1 = row[bin], y2 = row[bin + 1];
            float denom = y0 - 2.0f * y1 + y2;
            float delta = (denom != 0.0f) ? (0.5f * (y0 - y2) / denom) : 0.0f;
            delta = std::clamp(delta, -0.5f, 0.5f);
            double refinedBin = (double)bin + (double)delta;

            // Bin -> Hz offset: same "bin 0 = low edge of the span" convention fftHandler()'s own
            // pre-rotation comment documents.
            double offsetHz = (refinedBin / (double)n) * widthHz - widthHz / 2.0;
            out.push_back({ offsetHz, y1 });
        }
    }

    // Matches this frame's freshly detected peaks against trackedPeaks (persisted across calls,
    // under histMtx alongside history itself -- see acquirePeaks()'s own comment), smoothing
    // matched offsets and aging out ones that stop being detected. See
    // CARRIER_PEAK_LABELS_PLAN.md's "Cross-frame tracking" paragraph for the full reasoning.
    void updateTrackedPeaks(const std::vector<PeakInfo>& fresh, double widthHz) {
        // Matching tolerance: a few bins' worth of Hz, derived from the live resolution rather
        // than a fixed Hz constant, so it scales with whatever resolution the user has dialed in.
        double toleranceHz = (double)MIN_PEAK_SEPARATION_BINS * (widthHz / (double)(std::max)(_windowSize, 1));

        std::vector<bool> matched(fresh.size(), false);
        for (auto& tp : trackedPeaks) {
            int bestIdx = -1;
            double bestDist = toleranceHz;
            for (size_t i = 0; i < fresh.size(); i++) {
                if (matched[i]) { continue; }
                double dist = std::abs(fresh[i].offsetHz - tp.offsetHz);
                if (dist <= bestDist) { bestDist = dist; bestIdx = (int)i; }
            }
            if (bestIdx >= 0) {
                matched[bestIdx] = true;
                tp.offsetHz += (fresh[bestIdx].offsetHz - tp.offsetHz) * PEAK_SMOOTHING_ALPHA;
                tp.magnitudeDb = fresh[bestIdx].magnitudeDb;
                tp.misses = 0;
            }
            else {
                tp.misses++;
            }
        }
        trackedPeaks.erase(std::remove_if(trackedPeaks.begin(), trackedPeaks.end(),
                                           [](const TrackedPeak& tp) { return tp.misses > MAX_PEAK_MISSES; }),
                            trackedPeaks.end());
        for (size_t i = 0; i < fresh.size(); i++) {
            if (!matched[i]) { trackedPeaks.push_back({ fresh[i].offsetHz, fresh[i].magnitudeDb, 0 }); }
        }
    }

    // Runs on fftSink's own worker thread. Guaranteed not to run concurrently with
    // allocateFFT()/freeFFT() -- every caller that can change _widthHz/_resolutionHz/
    // _updateIntervalSec (setWidth()/setResolutionHz()/setUpdateIntervalSec()) stops fftSink
    // for its entire mutation, not just the recomputeWindowAndHop() part -- see those functions'
    // own comments for why the wider bracket (VFO rate change included) turned out to matter.
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

        // Peak detection/tracking runs against the still-local `row` before it's moved into
        // history below -- history keeps the raw dB rows for the waterfall, tracking keeps its
        // own smoothed, persistent copy of just the peak locations (see CARRIER_PEAK_LABELS_PLAN.md).
        std::vector<PeakInfo> fresh;
        _this->detectPeaks(row, n, _this->_widthHz, fresh);

        std::lock_guard<std::mutex> lck(_this->histMtx);
        _this->updateTrackedPeaks(fresh, _this->_widthHz);
        _this->history.push_back(std::move(row));
        int maxRows = _this->computeMaxRows();
        while ((int)_this->history.size() > maxRows) { _this->history.pop_front(); }
    }

    // Smoothed, persistent peak state -- see updateTrackedPeaks()'s own comment.
    struct TrackedPeak {
        double offsetHz;
        float magnitudeDb;
        int misses;
    };
    std::vector<TrackedPeak> trackedPeaks;

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
