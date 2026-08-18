#pragma once
#include "carrier_zoom.h"
#include <gui/widgets/carrier_zoom_plot.h>
#include <gui/gui.h>
#include <gui/style.h>
#include <imgui.h>
#include <string>
#include <vector>

// The single, app-wide owner of CarrierZoomView + its floating window (see CARRIER_ZOOM_PLAN.md
// -- "No multiple simultaneous windows to start"). Exactly one instance of this exists (a plain
// global, declared in radio_module.h right alongside this module's own `config` global -- same
// single-translation-unit convention that global already relies on). Any RadioModule instance
// can open it; opening it from a *different* instance than whichever currently owns it retargets
// (tears down the old VFO tap, starts a fresh one at the new offset and back at default
// width/resolution/update-rate) rather than opening a second window or refusing the trigger.
class CarrierZoomWindow {
public:
    // Called from the trigger (double-click/right-click on the Radio module's own filter-preview
    // spectrum). ownerName: the triggering RadioModule's own `name` -- used only to distinguish
    // "the same instance re-triggering" (bring the window to front, keep its current state) from
    // "a different instance" (retarget and reset to defaults); never persisted or displayed.
    // absoluteFreqHz: the tuned frequency this offset is relative to (waterfall center + the same
    // centerOffset passed as `offset`), captured once here rather than tracked live afterward --
    // see CARRIER_PEAK_LABELS_PLAN.md's own note on why: this snapshots at open/retarget time,
    // matching CarrierZoomView's own VFO tap, which is likewise only ever set here, never
    // live-followed if the underlying receiver gets retuned again while this window stays open.
    void open(const std::string& ownerName, double offset, double absoluteFreqHz) {
        if (show && _ownerName == ownerName) {
            focusRequested = true;
            return;
        }
        view.deinit();
        view.init(ownerName, offset);
        _ownerName = ownerName;
        _absoluteFreqHz = absoluteFreqHz;
        show = true;
        focusRequested = true;
    }

    void close() {
        if (!show) { return; }
        show = false;
        view.deinit();
        _ownerName.clear();
    }

    bool isOpenFor(const std::string& callerName) { return show && _ownerName == callerName; }

    // Called every frame from whichever RadioModule instance's own menuHandler() currently owns
    // this window -- see that call site's own comment for why only the owner draws it. This
    // window's actual on-screen lifetime is therefore tied to *some* RadioModule instance's own
    // per-frame draw call still running at all, matching the precedent already set by this
    // codebase's other module-owned floating windows (e.g.
    // decoder_modules/weather_sat_decoder's own NOAA HRPT window) -- not something unique to
    // this feature.
    void draw(const std::string& callerName) {
        if (!isOpenFor(callerName)) { return; }

        if (focusRequested) {
            ImGui::SetNextWindowFocus();
            // A good, usable default size, forced on every (re)open/retarget -- not just the
            // very first time this window is ever shown (ImGuiCond_FirstUseEver would skip this
            // once anything is already saved in imgui.ini for this window title, which after a
            // lot of prior testing at small, accidental sizes it very likely already is). Ralph,
            // 2026-08-19: "the current default is unusable... it would help tremendously if the
            // default window was open much wider." Matches this window's own existing "no
            // persisted state across closes" design (CARRIER_ZOOM_PLAN.md) -- window size resets
            // to this default on every open/retarget the same way the sliders already do, rather
            // than being yet another thing this feature would otherwise need to remember.
            ImGui::SetNextWindowSize(ImVec2(950.0f * style::uiScale, 620.0f * style::uiScale), ImGuiCond_Always);
            focusRequested = false;
        }

        bool stillOpen = true;
        ImGui::Begin("Carrier Zoom", &stillOpen);

        // Only lock the main waterfall's own click-to-tune/drag handling while the mouse is
        // actually over this window -- not for this window's entire open lifetime, which is what
        // every other floating window in this codebase that sets this flag does (dialog boxes,
        // frequency manager, the recording scheduler, ...), but which made the rest of the app
        // unusable while this one stayed open (Ralph, 2026-08-19: "Is it possible to make the
        // window work in a way where the rest of the interface is usable?" -- confirmed as the
        // cause of an earlier, separate report that retuning required closing this window first).
        // Those other windows are typically small, transient, and meant to be dealt with and
        // dismissed; this one is meant to sit open for extended periods alongside normal use of
        // the rest of the app, so tying the lock to hover (checked fresh every frame, covers a
        // drag that starts and stays inside this window same as an unconditional lock would, but
        // releases the instant the mouse leaves) is the right scope for it specifically, not a
        // general fix applicable to those other windows too.
        gui::mainWindow.lockWaterfallControls = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);

        // Free sliders, per Ralph (2026-08-16) -- see CARRIER_ZOOM_PLAN.md. Logarithmic: each
        // covers roughly a 50-100x dynamic range, and the values users actually want (a couple
        // Hz of resolution, roughly a second between updates) sit well below the middle of a
        // plain linear range.
        float width = (float)view.getWidth();
        if (ImGui::SliderFloat("Width (Hz)", &width, 50.0f, 5000.0f, "%.0f", ImGuiSliderFlags_Logarithmic)) {
            view.setWidth((double)width);
        }

        float resolution = (float)view.getResolutionHz();
        if (ImGui::SliderFloat("Resolution (Hz/bin)", &resolution, (float)CarrierZoomView::MIN_RESOLUTION_HZ,
                                (float)CarrierZoomView::MAX_RESOLUTION_HZ, "%.2f", ImGuiSliderFlags_Logarithmic)) {
            view.setResolutionHz((double)resolution);
        }

        float updateInterval = (float)view.getUpdateIntervalSecRequested();
        if (ImGui::SliderFloat("Update interval (s)", &updateInterval, (float)CarrierZoomView::MIN_UPDATE_INTERVAL_SEC,
                                (float)CarrierZoomView::MAX_UPDATE_INTERVAL_SEC, "%.2f", ImGuiSliderFlags_Logarithmic)) {
            view.setUpdateIntervalSec((double)updateInterval);
        }
        // The interval actually achieved can differ from the slider's own requested value once
        // clamped against the hop-can't-exceed-window floor (see
        // CarrierZoomView::applyWindowAndHop()) -- shown as a plain readout rather than snapping
        // the slider itself back, so dragging it stays smooth even near that floor.
        ImGui::Text("Actual: %.2fs/row", view.getUpdateIntervalSecActual());

        // Peak markers on/off (CARRIER_PEAK_LABELS_PLAN.md, resolved with Ralph 2026-08-17):
        // detection/tracking itself always keeps running in CarrierZoomView regardless of this
        // checkbox -- it only gates whether CarrierZoomPlot draws the lines/labels, so toggling
        // it is instant, not a re-detect-on-toggle delay.
        ImGui::Checkbox("Show carrier peaks", &showPeaks);

        // CarrierZoomPlot takes a plain offset array, not CarrierZoomView::PeakInfo directly --
        // it stays pure rendering with no DSP-header dependency (see its own header comment), so
        // the magnitude half of each PeakInfo (not needed for drawing) is dropped here.
        //
        // Acquired and fully released *before* acquireHistory() below, never nested inside it --
        // both accessors lock the same non-reentrant histMtx (see acquirePeaks()'s own comment on
        // why it shares that lock rather than adding a second one), and locking a std::mutex a
        // thread already holds is undefined behavior -- in practice, on the GUI thread, an
        // instant permanent deadlock (the beachball this exact bug produced the one time it
        // shipped, live-tested, before this comment existed).
        std::vector<double> peakOffsetsHz;
        if (showPeaks) {
            std::vector<CarrierZoomView::PeakInfo> peaks;
            view.acquirePeaks(peaks);
            peakOffsetsHz.reserve(peaks.size());
            for (auto& p : peaks) { peakOffsetsHz.push_back(p.offsetHz); }
            view.releasePeaks();
        }

        std::vector<const float*> rows;
        int fftSize = 0;
        double spanHz = 0.0;
        view.acquireHistory(rows, fftSize, spanHz);
        plot.draw("##carrier_zoom_plot", ImVec2(ImGui::GetContentRegionAvail().x, 300.0f * style::uiScale),
                  rows.data(), (int)rows.size(), fftSize, spanHz, _absoluteFreqHz,
                  peakOffsetsHz.data(), (int)peakOffsetsHz.size());
        view.releaseHistory();

        ImGui::End();

        if (!stillOpen) { close(); }
    }

private:
    CarrierZoomView view;
    ImGui::CarrierZoomPlot plot;
    bool show = false;
    bool focusRequested = false;
    std::string _ownerName;
    double _absoluteFreqHz = 0.0;
    bool showPeaks = true;
};
