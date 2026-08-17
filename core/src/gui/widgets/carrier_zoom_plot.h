#pragma once
#include <imgui.h>
#include <utils/opengl_include_code.h>
#include <mutex>

namespace ImGui {
    // A small, self-contained live-trace + scrolling-waterfall-history plot, built for the
    // Radio module's single-frequency carrier zoom (see CARRIER_ZOOM_PLAN.md). Pure rendering,
    // no knowledge of VFOs, FFTs, or demodulators -- takes ready-made dB-magnitude rows and
    // draws them, the same "pass data in, draw it" convention MiniSpectrum already follows.
    //
    // Renders the scrolling history as a GPU texture blitted with a single AddImage() call,
    // exactly WaterFall::drawWaterfall()'s own technique (see waterfall.cpp) -- not reused
    // directly (see CARRIER_ZOOM_PLAN.md for why this widget doesn't just instantiate a second
    // WaterFall), but the same *approach* is deliberate, not incidental: the alternative -- one
    // ImDrawList rectangle per pixel column per history row, reissued every render frame -- scales
    // directly with history depth, and this feature's whole point is minutes of accumulated rows.
    class CarrierZoomPlot {
    public:
        ~CarrierZoomPlot();

        // rows: rowCount pointers to fftSize-float dB-magnitude arrays, ordered oldest-to-newest
        // (rows[rowCount-1] is the most recent, matching how a caller would naturally append to
        // a history buffer). May have rowCount == 0 (draws an empty plot -- e.g. no data yet).
        // spanHz: the Hz width each row covers, centered on the tuned frequency (bin 0 = the low
        // edge, matching RadioSpectrumPreview's own FFT convention). Unlike MiniSpectrum, there's
        // no separate carrier-vs-center-of-plot distinction here -- this view has no USB/LSB-style
        // asymmetric reference point, it's always centered directly on the tuned frequency itself.
        // nominalFreqHz: the absolute frequency the plot is centered on (spanHz/2 either side of
        // it) -- used only to label peakOffsetsHz below with an absolute frequency; unused if
        // peakCount == 0.
        // peakOffsetsHz/peakCount: per-carrier measured offsets from nominalFreqHz (see
        // CARRIER_PEAK_LABELS_PLAN.md), each drawn as a line distinct from the fixed
        // nominal-frequency line plus an absolute-frequency label. Plain array, not a struct type,
        // deliberately -- this widget stays pure rendering with no CarrierZoomView/DSP dependency,
        // same "pass data in, draw it" convention `rows` above already follows.
        void draw(const char* strId, ImVec2 size, const float* const* rows, int rowCount, int fftSize, double spanHz,
                  double nominalFreqHz = 0.0, const double* peakOffsetsHz = nullptr, int peakCount = 0);

    private:
        void ensureTexture();
        void rebuildTexture(const float* const* rows, int rowCount, int fftSize, float rangeMin, float rangeMax);

        bool texInit = false;
        GLuint textureId = 0;
        std::mutex texMtx;

        float rangeMin = -100.0f;
        float rangeMax = 0.0f;
        bool rangeInit = false;
        int framesSinceInit = 0;
    };
}
