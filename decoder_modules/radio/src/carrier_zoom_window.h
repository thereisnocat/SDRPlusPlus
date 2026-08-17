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
    void open(const std::string& ownerName, double offset) {
        if (show && _ownerName == ownerName) {
            focusRequested = true;
            return;
        }
        view.deinit();
        view.init(ownerName, offset);
        _ownerName = ownerName;
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

        gui::mainWindow.lockWaterfallControls = true;

        if (focusRequested) {
            ImGui::SetNextWindowFocus();
            focusRequested = false;
        }

        bool stillOpen = true;
        ImGui::Begin("Carrier Zoom", &stillOpen);

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

        std::vector<const float*> rows;
        int fftSize = 0;
        double spanHz = 0.0;
        view.acquireHistory(rows, fftSize, spanHz);
        plot.draw("##carrier_zoom_plot", ImVec2(ImGui::GetContentRegionAvail().x, 300.0f * style::uiScale),
                  rows.data(), (int)rows.size(), fftSize, spanHz);
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
};
