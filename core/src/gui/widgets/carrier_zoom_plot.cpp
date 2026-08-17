#include <gui/widgets/carrier_zoom_plot.h>
#include <gui/style.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace ImGui {
    // Same 13-anchor gradient as the main waterfall's own default color map (DEFAULT_COLOR_MAP
    // in waterfall.cpp) -- reusing the exact colors, not the class, for a visually consistent
    // look without taking on any of WaterFall's own coupling (see CARRIER_ZOOM_PLAN.md).
    static const float COLOR_MAP[][3] = {
        { 0x00, 0x00, 0x20 }, { 0x00, 0x00, 0x30 }, { 0x00, 0x00, 0x50 }, { 0x00, 0x00, 0x91 },
        { 0x1E, 0x90, 0xFF }, { 0xFF, 0xFF, 0xFF }, { 0xFF, 0xFF, 0x00 }, { 0xFE, 0x6D, 0x16 },
        { 0xFF, 0x00, 0x00 }, { 0xC6, 0x00, 0x00 }, { 0x9F, 0x00, 0x00 }, { 0x75, 0x00, 0x00 },
        { 0x4A, 0x00, 0x00 }
    };
    static constexpr int COLOR_COUNT = 13;

    // Fixed texture width the FFT bins get resampled onto, independent of both the true FFT
    // size and the widget's actual on-screen pixel width -- GL_LINEAR filtering smooths the
    // final stretch to whatever size the widget is actually drawn at. Matches the shape of
    // WaterFall's own fixed `dataWidth` (600 there; picked here, not copied, since this widget's
    // typical span is far narrower and doesn't need as much horizontal texel budget).
    static constexpr int TEX_W = 512;

    static uint32_t dbToPixel(float v, float rangeMin, float rangeMax) {
        float frac = (rangeMax > rangeMin) ? (v - rangeMin) / (rangeMax - rangeMin) : 0.0f;
        frac = std::clamp(frac, 0.0f, 1.0f);
        float pos = frac * (float)(COLOR_COUNT - 1);
        int lowerId = (int)floorf(pos);
        int upperId = (std::min)(lowerId + 1, COLOR_COUNT - 1);
        float ratio = pos - (float)lowerId;
        float r = COLOR_MAP[lowerId][0] * (1.0f - ratio) + COLOR_MAP[upperId][0] * ratio;
        float g = COLOR_MAP[lowerId][1] * (1.0f - ratio) + COLOR_MAP[upperId][1] * ratio;
        float b = COLOR_MAP[lowerId][2] * (1.0f - ratio) + COLOR_MAP[upperId][2] * ratio;
        // 0xAABBGGRR packing (A in the high byte, R in the low byte) -- matches GL_RGBA +
        // GL_UNSIGNED_BYTE's in-memory byte order on a little-endian host, same packing
        // WaterFall::updatePallette() itself uses for the exact same texture format.
        return ((uint32_t)255 << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | (uint32_t)r;
    }

    // Peak-frequency labels always in kHz, per Ralph (2026-08-17) -- deliberately NOT
    // utils::formatFreq() (freq_formatting.h), the app's general-purpose MHz/KHz/Hz-auto-selecting
    // convention used for the main tuned-frequency readout and everywhere else in the app: that
    // function switches to MHz above 1,000,000Hz, but Carrier Zoom's whole use case is
    // broadcast-band DXing, where a frequency is always quoted in kHz regardless of which side of
    // the 1MHz mark it happens to fall on (e.g. "1400 kHz", never "1.4 MHz"). Shares
    // formatFreq()'s own precision/trim algorithm (6 decimal places on the kHz value, trailing
    // zeros and a bare trailing decimal point both trimmed) so a whole-kHz peak reads as plain
    // "1400 kHz" while a fractional one still shows exactly as many decimals as it needs -- same
    // behavior Ralph asked this to match, just with the unit pinned instead of auto-selected.
    static std::string formatPeakFreqKHz(double hz) {
        char str[32];
        snprintf(str, sizeof(str), "%.06lf", hz / 1000.0);
        int len = (int)strlen(str) - 1;
        while ((str[len] == '0' || str[len] == '.') && len > 0) {
            len--;
            if (str[len] == '.') { len--; break; }
        }
        return std::string(str).substr(0, len + 1) + " kHz";
    }

    CarrierZoomPlot::~CarrierZoomPlot() {
        if (texInit) { glDeleteTextures(1, &textureId); }
    }

    void CarrierZoomPlot::draw(const char* strId, ImVec2 size, const float* const* rows, int rowCount, int fftSize, double spanHz,
                                double nominalFreqHz, const double* peakOffsetsHz, int peakCount) {
        ImGuiWindow* window = GetCurrentWindow();
        if (window->SkipItems) { return; }

        ImVec2 pos = GetCursorScreenPos();
        ImRect bb(pos, ImVec2(pos.x + size.x, pos.y + size.y));
        ImGui::Dummy(size);

        ImDrawList* dl = window->DrawList;
        ImU32 bg = GetColorU32(ImGuiCol_FrameBg);
        ImU32 border = GetColorU32(ImGuiCol_Border);
        ImU32 trace = GetColorU32(ImGuiCol_PlotLines);
        ImU32 shadow = GetColorU32(ImGuiCol_PlotLines, 0.2f);
        ImU32 centerColor = IM_COL32(255, 0, 0, 180);
        // Distinct from the nominal-frequency line above -- cyan reads clearly against both the
        // dark trace-pane background and the waterfall's own red/yellow/white hot colors near the
        // top of the dB range (right where a strong carrier's own waterfall pixels already are),
        // and matches the cyan/green-marker convention other SDR spectrum tools already use (see
        // CARRIER_PEAK_LABELS_PLAN.md).
        ImU32 peakColor = IM_COL32(0, 255, 255, 200);
        ImU32 peakLabelBg = IM_COL32(0, 0, 0, 160);

        dl->AddRectFilled(bb.Min, bb.Max, bg);

        // Layout: a live trace on top (same auto-ranged-dB technique as MiniSpectrum's own
        // trace), a scrolling waterfall history beneath it.
        float traceH = (std::max)(size.y * 0.3f, 30.0f * style::uiScale);
        float gap = 2.0f * style::uiScale;
        ImRect traceBB(bb.Min, ImVec2(bb.Max.x, bb.Min.y + traceH));
        ImRect wfBB(ImVec2(bb.Min.x, bb.Min.y + traceH + gap), bb.Max);

        const float* latest = (rowCount > 0) ? rows[rowCount - 1] : NULL;

        // Auto-ranged dB scale, ramped convergence on reset -- exactly MiniSpectrum's own
        // technique (see that widget's own comment for why an instant hard reset was tried and
        // reverted there: the underlying FFT accumulation can still hold a frame or two of
        // transitional data right after a retune/resize, and locking onto one of those frames
        // is a narrower version of the stale-range bug the reset exists to fix).
        if (latest != NULL && fftSize > 1) {
            float instMin = latest[0], instMax = latest[0];
            for (int i = 1; i < fftSize; i++) {
                instMin = (std::min)(instMin, latest[i]);
                instMax = (std::max)(instMax, latest[i]);
            }
            float margin = (std::max)((instMax - instMin) * 0.1f, 1.0f);
            instMin -= margin;
            instMax += margin;
            if (!rangeInit) {
                rangeMin = instMin;
                rangeMax = instMax;
                rangeInit = true;
                framesSinceInit = 0;
            }
            else {
                framesSinceInit++;
                float alpha = (std::max)(0.2f, 1.0f / (float)(framesSinceInit + 1));
                rangeMin += (instMin - rangeMin) * alpha;
                rangeMax += (instMax - rangeMax) * alpha;
            }
        }
        float range = (std::max)(rangeMax - rangeMin, 1.0f);

        // Live trace. Mirrors WaterFall::drawFFT()/MiniSpectrum's own technique: a one-pixel
        // vertical "shadow" line from each trace point down to the baseline stands in for a
        // proper filled-area plot.
        if (latest != NULL && fftSize > 1) {
            float scale = traceH / range;
            int steps = (std::max)(1, (int)size.x);
            float prevY = 0.0f;
            for (int i = 0; i <= steps; i++) {
                float x = traceBB.Min.x + (float)i;
                int bin = std::clamp((int)((float)i / size.x * (float)fftSize), 0, fftSize - 1);
                float y = traceBB.Max.y - ((latest[bin] - rangeMin) * scale);
                y = std::clamp(y, traceBB.Min.y + 1, traceBB.Max.y);
                if (i > 0) {
                    dl->AddLine(ImVec2(x - 1, prevY), ImVec2(x, y), trace, 1.0f);
                }
                dl->AddLine(ImVec2(x, y), ImVec2(x, traceBB.Max.y), shadow, 1.0f);
                prevY = y;
            }
        }

        // Waterfall history, rendered as a single GPU-texture blit -- see this class's own
        // header comment for why (redrawing one rect per pixel column per row, every render
        // frame, would scale directly with history depth, and this feature's whole point is
        // minutes of accumulated rows).
        if (rowCount > 0 && fftSize > 0) {
            ensureTexture();
            rebuildTexture(rows, rowCount, fftSize, rangeMin, rangeMax);
            std::lock_guard<std::mutex> lck(texMtx);
            dl->AddImage((void*)(intptr_t)textureId, wfBB.Min, wfBB.Max);
        }
        else {
            dl->AddRectFilled(wfBB.Min, wfBB.Max, bg);
        }

        // Center tick -- this view is always centered directly on the tuned frequency itself
        // (no USB/LSB-style asymmetric reference point the way MiniSpectrum has; see
        // CARRIER_ZOOM_PLAN.md), so 0Hz is plot-center by construction, no separate
        // carrierOffsetHz parameter needed the way MiniSpectrum::draw() has one.
        float centerX = bb.Min.x + size.x / 2.0f;
        dl->AddLine(ImVec2(centerX, bb.Min.y), ImVec2(centerX, bb.Max.y), centerColor, style::uiScale);

        // Per-carrier peak lines + absolute-frequency labels (CARRIER_PEAK_LABELS_PLAN.md,
        // resolved with Ralph 2026-08-17: absolute frequency alone, one decimal, no separate
        // offset shown). Sorted left-to-right by offset first -- peakOffsetsHz arrives in
        // whatever order CarrierZoomView::trackedPeaks happens to hold internally (append order,
        // not offset order), and a stable left-to-right order is what the collision-staggering
        // below (alternating label row) depends on to only compare each label against its
        // immediate on-screen neighbor.
        if (peakCount > 0 && spanHz > 0.0) {
            std::vector<double> sortedOffsets(peakOffsetsHz, peakOffsetsHz + peakCount);
            std::sort(sortedOffsets.begin(), sortedOffsets.end());

            // Collision mitigation: two label rows, each independently tracking its own
            // last-placed label's right edge. Each label prefers to alternate rows from the
            // previous one (index parity), falling back to the other row (checked against its
            // own last occupant, not the preferred row's) when the preferred one would collide.
            // If *neither* row is free -- verified live, this genuinely happens: this plot's own
            // window renders far narrower than a typical label's own text width whenever it's
            // freshly opened (a pre-existing sizing quirk of this floating window, unrelated to
            // peak labels specifically), so with several close peaks two rows can both fill up --
            // the label's own text is skipped entirely rather than drawn overlapping and
            // unreadable; the line itself is still always drawn, so a real signal stays visible
            // even when there's no room left to caption it. Still not a general label-layout
            // solver (see CARRIER_PEAK_LABELS_PLAN.md) -- a deliberately simple two-row-or-nothing
            // scheme, not a third row or dynamic font shrinking.
            float lastLabelRight[2] = { -1.0e9f, -1.0e9f };
            int idx = 0;
            for (double offsetHz : sortedOffsets) {
                float frac = (float)((offsetHz + spanHz / 2.0) / spanHz);
                float x = bb.Min.x + std::clamp(frac, 0.0f, 1.0f) * size.x;
                dl->AddLine(ImVec2(x, bb.Min.y), ImVec2(x, bb.Max.y), peakColor, style::uiScale);

                std::string label = formatPeakFreqKHz(nominalFreqHz + offsetHz);
                ImVec2 textSize = ImGui::CalcTextSize(label.c_str());
                float labelX = std::clamp(x - textSize.x / 2.0f, bb.Min.x, bb.Max.x - textSize.x);

                int preferred = idx % 2;
                int other = 1 - preferred;
                int row = -1;
                if (labelX >= lastLabelRight[preferred]) { row = preferred; }
                else if (labelX >= lastLabelRight[other]) { row = other; }

                if (row >= 0) {
                    float labelY = traceBB.Min.y + (float)row * textSize.y;
                    dl->AddRectFilled(ImVec2(labelX, labelY), ImVec2(labelX + textSize.x, labelY + textSize.y), peakLabelBg);
                    dl->AddText(ImVec2(labelX, labelY), peakColor, label.c_str());
                    lastLabelRight[row] = labelX + textSize.x;
                }
                idx++;
            }
        }

        dl->AddRect(bb.Min, bb.Max, border);
    }

    void CarrierZoomPlot::ensureTexture() {
        if (texInit) { return; }
        glGenTextures(1, &textureId);
        texInit = true;
    }

    // Rebuilt (and the whole texture re-uploaded) on every draw() call rather than only when the
    // underlying data has actually changed -- deliberately simpler than WaterFall's own
    // dirty-flag tracking (see waterfallUpdate/updateWaterfallTexture() in waterfall.cpp). This
    // widget's data changes far less often (once per hop interval -- hundreds of ms to seconds,
    // see CarrierZoomView) than WaterFall's own (many times a second at full sample rate), and
    // the resample-and-repack cost here (TEX_W x rowCount, rowCount bounded by the 60-second
    // history cap) is small enough that redoing it unconditionally, every render frame, costs
    // less than the bookkeeping needed to avoid it would.
    void CarrierZoomPlot::rebuildTexture(const float* const* rows, int rowCount, int fftSize, float rangeMin, float rangeMax) {
        std::vector<uint32_t> pixels((size_t)TEX_W * (size_t)rowCount);
        for (int y = 0; y < rowCount; y++) {
            // rows[] is oldest-to-newest; texture row 0 is drawn at the TOP of the widget and
            // should be the newest data -- every waterfall in this app scrolls new-data-at-top,
            // older rows moving down -- so texture row y reads from rows[rowCount-1-y].
            const float* src = rows[rowCount - 1 - y];
            uint32_t* dst = &pixels[(size_t)y * (size_t)TEX_W];
            for (int x = 0; x < TEX_W; x++) {
                int bin = std::clamp((int)((float)x / (float)TEX_W * (float)fftSize), 0, fftSize - 1);
                dst[x] = dbToPixel(src[bin], rangeMin, rangeMax);
            }
        }
        std::lock_guard<std::mutex> lck(texMtx);
        glBindTexture(GL_TEXTURE_2D, textureId);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, TEX_W, rowCount, 0, GL_RGBA, GL_UNSIGNED_BYTE, (uint8_t*)pixels.data());
    }
}
