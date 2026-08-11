#include <gui/widgets/mini_spectrum.h>
#include <gui/style.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cmath>

namespace ImGui {
    // Width, in screen pixels, of the grab zone around each edge -- wide enough to actually
    // hit with a mouse without needing pixel-perfect precision, matching the spirit of the
    // main waterfall's own lbwSel/rbwSel hit boxes (see waterfall.cpp).
    static constexpr float EDGE_GRAB_PX = 8.0f;

    bool MiniSpectrum::draw(const char* strId, ImVec2 size, const float* fftData, int fftSize, double spanHz,
                             double* passbandLo, double* passbandHi,
                             double minEdge, double maxEdge, double carrierOffsetHz) {
        ImGuiWindow* window = GetCurrentWindow();
        if (window->SkipItems) { return false; }

        ImVec2 pos = GetCursorScreenPos();
        ImRect bb(pos, ImVec2(pos.x + size.x, pos.y + size.y));
        ImGui::Dummy(size);

        ImDrawList* dl = window->DrawList;
        ImU32 bg = GetColorU32(ImGuiCol_FrameBg);
        ImU32 border = GetColorU32(ImGuiCol_Border);
        ImU32 trace = GetColorU32(ImGuiCol_PlotLines);
        ImU32 shadow = GetColorU32(ImGuiCol_PlotLines, 0.2f);
        ImU32 passbandFill = IM_COL32(255, 255, 255, 40);
        ImU32 edgeColor = IM_COL32(255, 255, 0, 255);
        ImU32 centerColor = IM_COL32(255, 0, 0, 180);

        dl->AddRectFilled(bb.Min, bb.Max, bg);

        auto hzToX = [&](double hz) {
            return bb.Min.x + (float)((hz + (spanHz / 2.0)) / spanHz) * size.x;
        };
        auto xToHz = [&](float x) {
            return (double)((x - bb.Min.x) / size.x) * spanHz - (spanHz / 2.0);
        };

        // Passband shading, drawn first so the trace renders on top of it
        float loX = std::clamp(hzToX(*passbandLo), bb.Min.x, bb.Max.x);
        float hiX = std::clamp(hzToX(*passbandHi), bb.Min.x, bb.Max.x);
        dl->AddRectFilled(ImVec2(loX, bb.Min.y), ImVec2(hiX, bb.Max.y), passbandFill);

        // Spectrum trace. Mirrors WaterFall::drawFFT()'s own technique exactly: a one-pixel
        // vertical "shadow" line from each trace point down to the baseline stands in for a
        // proper filled-area plot without having to solve filling a non-convex path.
        //
        // The dB range is auto-scaled from the data itself rather than taken from the caller:
        // this plot covers a much narrower span than a typical full spectrum view, so a range
        // tuned for that wider view made everything except one strong signal look like a flat
        // floor, effectively invisible. Smoothed frame to frame (an IIR towards the instant
        // min/max, not the raw value) so the scale doesn't visibly jump around on noise.
        if (fftData != NULL && fftSize > 1) {
            float instMin = fftData[0], instMax = fftData[0];
            for (int i = 1; i < fftSize; i++) {
                instMin = std::min(instMin, fftData[i]);
                instMax = std::max(instMax, fftData[i]);
            }
            // A little headroom above the peak and below the floor so the trace doesn't ride
            // the very top/bottom edge of the plot.
            float margin = std::max((instMax - instMin) * 0.1f, 1.0f);
            instMin -= margin;
            instMax += margin;
            if (!rangeInit) {
                // Not an instant hard-set to this one frame's min/max -- resetRange() (mode
                // switches) used to do exactly that, and it traded one bug for another: the
                // very next frame after a retune can still hold transitional data (the
                // preview's own FFT accumulation buffer takes a moment to fully flush the
                // *previous* tuned frequency's content once RxVFO's offset changes -- it isn't
                // instant just because the retune call itself is), so latching hard onto
                // whatever that first frame happened to show could lock in a bad initial range
                // just as easily as the stale-carryover bug it was fixing. Ramping the
                // smoothing rate down from "start here" (frame 0, effectively instant) towards
                // the steady per-frame rate over the next several frames rides through that
                // transitional window instead of committing to any single sample from it,
                // while still converging in a few frames rather than dozens.
                rangeMin = instMin;
                rangeMax = instMax;
                rangeInit = true;
                framesSinceInit = 0;
            }
            else {
                framesSinceInit++;
                float alpha = std::max(0.2f, 1.0f / (float)(framesSinceInit + 1));
                rangeMin += (instMin - rangeMin) * alpha;
                rangeMax += (instMax - rangeMax) * alpha;
            }

            float range = std::max(rangeMax - rangeMin, 1.0f);
            float scale = size.y / range;
            int steps = std::max(1, (int)size.x);
            float prevY = 0.0f;
            for (int i = 0; i <= steps; i++) {
                float x = bb.Min.x + (float)i;
                int bin = std::clamp((int)((float)i / size.x * (float)fftSize), 0, fftSize - 1);
                float y = bb.Max.y - ((fftData[bin] - rangeMin) * scale);
                y = std::clamp(y, bb.Min.y + 1, bb.Max.y);
                if (i > 0) {
                    dl->AddLine(ImVec2(x - 1, prevY), ImVec2(x, y), trace, 1.0f);
                }
                dl->AddLine(ImVec2(x, y), ImVec2(x, bb.Max.y), shadow, 1.0f);
                prevY = y;
            }
        }

        // Carrier tick -- at carrierOffsetHz, not necessarily plot-center (0 Hz); see this
        // parameter's own doc comment in the header for why those two differ for USB/LSB.
        float carrierX = hzToX(carrierOffsetHz);
        dl->AddLine(ImVec2(carrierX, bb.Min.y), ImVec2(carrierX, bb.Max.y), centerColor, style::uiScale);

        // Edge handles
        dl->AddLine(ImVec2(loX, bb.Min.y), ImVec2(loX, bb.Max.y), edgeColor, style::uiScale);
        dl->AddLine(ImVec2(hiX, bb.Min.y), ImVec2(hiX, bb.Max.y), edgeColor, style::uiScale);

        dl->AddRect(bb.Min, bb.Max, border);

        // Interaction: independent per-edge dragging, hit-tested the same way the main
        // waterfall hit-tests its own bandwidth-edge handles (see waterfall.cpp's
        // processInputs()) rather than through a single ImGui button -- two separate grab
        // zones that can each be mid-drag need finer control than one Button/InvisibleButton
        // click region would give.
        ImVec2 mouse = GetMousePos();
        bool hoverLo = mouse.x >= loX - EDGE_GRAB_PX && mouse.x <= loX + EDGE_GRAB_PX && mouse.y >= bb.Min.y && mouse.y <= bb.Max.y;
        bool hoverHi = mouse.x >= hiX - EDGE_GRAB_PX && mouse.x <= hiX + EDGE_GRAB_PX && mouse.y >= bb.Min.y && mouse.y <= bb.Max.y;

        if (!IsMouseDown(ImGuiMouseButton_Left)) {
            draggingLo = false;
            draggingHi = false;
        }
        else if (!draggingLo && !draggingHi && IsMouseClicked(ImGuiMouseButton_Left)) {
            // Prefer whichever edge is closer if both grab zones overlap (possible when the
            // passband is very narrow)
            if (hoverLo && hoverHi) {
                if (fabsf(mouse.x - loX) <= fabsf(mouse.x - hiX)) { draggingLo = true; }
                else { draggingHi = true; }
            }
            else if (hoverLo) { draggingLo = true; }
            else if (hoverHi) { draggingHi = true; }
        }

        bool changed = false;
        if (draggingLo || hoverLo || draggingHi || hoverHi) {
            SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        }
        if (draggingLo) {
            double hz = std::clamp(xToHz(mouse.x), minEdge, 0.0);
            if (hz != *passbandLo) {
                *passbandLo = hz;
                changed = true;
            }
        }
        else if (draggingHi) {
            double hz = std::clamp(xToHz(mouse.x), 0.0, maxEdge);
            if (hz != *passbandHi) {
                *passbandHi = hz;
                changed = true;
            }
        }

        return changed;
    }
}
