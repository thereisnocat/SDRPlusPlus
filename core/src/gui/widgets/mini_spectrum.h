#pragma once
#include <imgui.h>

namespace ImGui {
    // A small, self-contained spectrum plot with a shaded, independently-draggable passband
    // region -- built for the Radio module's per-VFO filter preview (see
    // RADIO_SPECTRUM_FILTER_PLAN.md), but has no knowledge of VFOs, demodulators or config;
    // it just draws whatever FFT data and edges it's given and reports back if the user moved
    // an edge, the same "pass pointers, return bool changed" shape every other custom widget
    // in this codebase already follows (see SliderFloatWithSteps).
    class MiniSpectrum {
    public:
        // fftData: fftSize dB-magnitude bins spanning spanHz, centered at Hz offset 0 in this
        // call's coordinate system -- NOT necessarily the carrier; see carrierOffsetHz below.
        // May be NULL (draws an empty plot, e.g. while waiting for the first frame).
        // passbandLo/passbandHi: Hz offsets from that same center (lo <= 0 <= hi) -- read to
        // draw the shaded region, written to directly while the user drags one of its edges.
        // minEdge/maxEdge: how far either edge is allowed to move -- the mode's true ceiling,
        // not necessarily the caller's current "bandwidth" value; see the Radio module's own
        // call site comment for why that distinction matters.
        // carrierOffsetHz: where the tuned/carrier frequency actually is, as a Hz offset from
        // the same center 0 Hz is measured from -- usually 0 (dead center), but not for
        // USB/LSB: those tune from one edge of the passband rather than its middle (see the
        // Radio module's own call site comment), so plotting a marker at a hardcoded 0 drew it
        // in the middle of the passband instead of at the actual carrier, which for USB/LSB
        // made the whole display read as "centered on the carrier with content on both sides"
        // when the real signal is entirely to one side. Only affects where the tick is drawn;
        // doesn't change how fftData or the passband edges are interpreted.
        // Returns true on any frame where passbandLo/passbandHi were changed by a drag.
        //
        // The vertical (dB) scale is auto-ranged from fftData itself, smoothed frame to frame,
        // rather than taking a min/max from the caller: this plot covers a much narrower span
        // than a typical full-view spectrum display, so a range tuned for that wider view (e.g.
        // the main waterfall's own FFT min/max settings) routinely made everything except the
        // one strong signal in view look like a flat floor -- effectively invisible even when
        // real, weaker signals were present in the data.
        bool draw(const char* strId, ImVec2 size, const float* fftData, int fftSize, double spanHz,
                  double* passbandLo, double* passbandHi, double minEdge, double maxEdge, double carrierOffsetHz = 0.0);

        // Makes the next several draw() calls converge onto a fresh range quickly instead of
        // smoothing in slowly from wherever it last was. Call this whenever the underlying
        // signal being shown has changed for a reason that has nothing to do with normal
        // signal variation -- a mode switch, primarily. Without this, switching modes carries
        // the *previous* mode's signal-level range into the new mode's first several frames
        // (this is one persistent widget instance, not recreated per mode), which visibly
        // looks like a compressed, wrong-scale "balloon" for a moment until the smoothing
        // catches up -- reported live as still-broken-looking because the frequency span
        // itself (a separate, already-fixed concern -- see RADIO_SPECTRUM_FILTER_PLAN.md)
        // hadn't changed at all, only this had.
        //
        // Not an instant hard-set to whatever the very next frame shows, on purpose: an
        // earlier version of this did exactly that, and traded one bug for a narrower version
        // of the same one -- the preview's own FFT can still be showing transitional data for
        // a frame or two right after a retune (the accumulation buffer needs a moment to fully
        // flush the *previous* frequency's content), so latching hard onto whichever frame
        // happens to land first can lock in a bad range just as easily as the original stale-
        // carryover bug did. See draw()'s own comment for the ramped-convergence fix.
        void resetRange() { rangeInit = false; }

    private:
        bool draggingLo = false;
        bool draggingHi = false;
        float rangeMin = -100.0f;
        float rangeMax = 0.0f;
        bool rangeInit = false;
        int framesSinceInit = 0;
    };
}
