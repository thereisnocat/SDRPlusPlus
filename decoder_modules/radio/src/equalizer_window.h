#pragma once
#include "equalizer_host.h"
#include <gui/gui.h>
#include <gui/style.h>
#include <imgui.h>
#include <string>
#include <vector>

// The single, app-wide owner of the parametric EQ's floating editor window -- same "exactly
// one instance, any RadioModule instance can open it, opening from a different owner
// retargets rather than opening a second window" shape as CarrierZoomWindow right next to this
// file, and for the same reason (CARRIER_ZOOM_PLAN.md's "No multiple simultaneous windows to
// start"). See equalizer_host.h's own comment for why this one goes through an interface
// (EqualizerHost) instead of owning its own self-contained view the way CarrierZoomWindow
// owns CarrierZoomView.
class EqualizerWindow {
public:
    // ownerName: the triggering RadioModule's own `name`, used the same way
    // CarrierZoomWindow::open() uses it -- distinguishing "the same instance re-opening"
    // (bring to front, keep editing the same host) from "a different instance" (retarget).
    // host: not owned, must outlive this window being open for it -- RadioModule::disable()
    // closes this window if it currently owns it, exactly mirroring the existing
    // gCarrierZoomWindow.isOpenFor(name) check there, for the same reason (don't leave a
    // floating window pointed at a host that's about to be destroyed).
    void open(const std::string& ownerName, EqualizerHost* host) {
        if (show && _ownerName == ownerName) {
            focusRequested = true;
            return;
        }
        _ownerName = ownerName;
        _host = host;
        show = true;
        focusRequested = true;
        newProfileName[0] = '\0';
    }

    void close() {
        if (!show) { return; }
        show = false;
        _host = nullptr;
        _ownerName.clear();
    }

    bool isOpenFor(const std::string& callerName) { return show && _ownerName == callerName; }

    void draw(const std::string& callerName) {
        if (!isOpenFor(callerName) || !_host) { return; }

        if (focusRequested) {
            ImGui::SetNextWindowFocus();
            // Forced every (re)open/retarget, not just the first-ever show -- same reasoning
            // and the same fix as CarrierZoomWindow's own identical block: a small
            // accidentally-saved imgui.ini size from early testing shouldn't stick around
            // forever once ImGuiCond_FirstUseEver has already seen this window title once.
            ImGui::SetNextWindowSize(ImVec2(560.0f * style::uiScale, 520.0f * style::uiScale), ImGuiCond_Always);
            focusRequested = false;
        }

        bool stillOpen = true;
        ImGui::Begin("Equalizer", &stillOpen);

        // Same hover-scoped lock as CarrierZoomWindow, and the same reason: this window is
        // meant to sit open for extended periods alongside normal use of the rest of the app
        // (tweaking a band while listening), not something to be dealt with and dismissed.
        gui::mainWindow.lockWaterfallControls = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);

        bool eqEnabled = _host->isEqEnabled();
        if (ImGui::Checkbox("Enabled", &eqEnabled)) { _host->setEqEnabled(eqEnabled); }

        ImGui::Separator();

        static const char* bandLabels[] = { "Low Shelf", "Peak 1", "Peak 2", "Peak 3", "Peak 4", "High Shelf" };
        const int bandCount = _host->getEqBandCount();
        for (int i = 0; i < bandCount; i++) {
            ImGui::PushID(i);
            ImGui::TextUnformatted(i < 6 ? bandLabels[i] : "Band");

            double freqHz; float gainDb; double q;
            _host->getEqBand(i, freqHz, gainDb, q);

            float freq = (float)freqHz;
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            if (ImGui::SliderFloat("##freq", &freq, 20.0f, 15000.0f, "%.0f Hz", ImGuiSliderFlags_Logarithmic)) {
                _host->setEqBand(i, (double)freq, gainDb, q);
            }

            float gain = gainDb;
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.65f);
            if (ImGui::SliderFloat("##gain", &gain, -18.0f, 18.0f, "%.1f dB")) {
                _host->setEqBand(i, freqHz, gain, q);
            }
            ImGui::SameLine();
            float qf = (float)q;
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            if (ImGui::SliderFloat("##q", &qf, 0.1f, 10.0f, "Q %.2f", ImGuiSliderFlags_Logarithmic)) {
                _host->setEqBand(i, freqHz, gainDb, (double)qf);
            }

            ImGui::Spacing();
            ImGui::PopID();
        }

        if (ImGui::Button("Flatten (0dB every band)")) {
            for (int i = 0; i < bandCount; i++) {
                double freqHz; float gainDb; double q;
                _host->getEqBand(i, freqHz, gainDb, q);
                _host->setEqBand(i, freqHz, 0.0f, q);
            }
        }

        ImGui::Separator();
        ImGui::TextUnformatted("Profiles");

        std::string lastLoaded = _host->getLastLoadedEqProfile();
        std::vector<std::string> names = _host->getEqProfileNames();
        for (const std::string& n : names) {
            ImGui::PushID(n.c_str());
            // Load/Delete first, then the name -- simpler and more robust than trying to
            // right-align the buttons after a variable-length name, which needs the row's
            // available width recomputed correctly relative to the current cursor position,
            // not just the window's own total width.
            if (ImGui::SmallButton("Load")) { _host->loadEqProfile(n); }
            ImGui::SameLine();
            if (ImGui::SmallButton("Delete")) { _host->deleteEqProfile(n); }
            ImGui::SameLine();
            if (n == lastLoaded) { ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "%s (current)", n.c_str()); }
            else { ImGui::TextUnformatted(n.c_str()); }
            ImGui::PopID();
        }
        if (names.empty()) { ImGui::TextDisabled("No saved profiles yet."); }

        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 80.0f * style::uiScale);
        ImGui::InputText("##newProfileName", newProfileName, sizeof(newProfileName));
        ImGui::SameLine();
        bool canSave = newProfileName[0] != '\0';
        if (!canSave) { style::beginDisabled(); }
        if (ImGui::Button("Save")) {
            _host->saveEqProfile(newProfileName);
            newProfileName[0] = '\0';
        }
        if (!canSave) { style::endDisabled(); }

        ImGui::End();

        if (!stillOpen) { close(); }
    }

private:
    EqualizerHost* _host = nullptr;
    bool show = false;
    bool focusRequested = false;
    std::string _ownerName;
    char newProfileName[64] = "";
};
