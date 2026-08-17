# Radio module: single-frequency high-resolution carrier zoom

Design brief from Ralph, 2026-08-16, given as a feasibility question first (see the "Why this
is feasible" section below for the answer that was given before this plan was written):

> Right clicking or double clicking on the zoomed in spectrum display in the Radio module
> should bring up a new window that provides a very high resolution spectrum display of just
> the single frequency currently tuned. There is software called Jaguar that works with the
> Perseus that uses this kind of display as its default, so that users can see all the carriers
> on a given frequency. Often on frequencies where there are many stations, such as a graveyard
> frequency like 1490 kHz, each station will have a slightly different frequency that they're
> broadcasting on, offset from the nominal frequency. That offset can sometimes be used to
> identify stations.

"The zoomed in spectrum display" is the existing filter-preview widget from
`RADIO_SPECTRUM_FILTER_PLAN.md` — `ImGui::MiniSpectrum` (`core/src/gui/widgets/mini_spectrum.h`)
fed by `RadioSpectrumPreview` (`decoder_modules/radio/src/spectrum_preview.h`), drawn above the
mode selector in the Radio module's own panel. This plan is a new, second feature that opens off
of it, not a modification of it.

## Why this is feasible, and why it needs new infrastructure rather than just reusing the preview

`RadioSpectrumPreview` already proves the core trick this feature needs: a **second, invisible
VFO tap**, independent of the main waterfall's pan/zoom, dedicated to a small local FFT. It gets
that via `sigpath::iqFrontEnd.addVFO()` — the same call `VFOManager::VFO` itself uses for the
real demod VFO — bypassing `VFOManager` so it never shows up as a box on the main waterfall.
That mechanism is exactly right for this feature too. What's wrong for this feature is *how it's
tuned*: the existing preview is deliberately a wide, coarse, fast-updating "context" view (a few
times the demod bandwidth, 512 bins, ~20Hz refresh, and its own doc comment calls it "a rough
visual guide, not a precise value anything downstream depends on"). This feature wants the
opposite trade along every one of those axes: narrow, fine, slow.

**The core tradeoff is physics, not a code limitation.** Frequency resolution and time resolution
trade off directly (`resolution_Hz ≈ decimated_sample_rate / FFT_size`, and the time to
accumulate one FFT's worth of input is `FFT_size / decimated_sample_rate`). Two stations on the
same nominal graveyard frequency, a few Hz apart, need sub-Hz-to-low-single-digit-Hz resolution
to visually separate — which unavoidably means several seconds (or longer) per update. This is
not a shortcoming to engineer around; it's the same trade Jaguar's own extreme-zoom display
makes, and why that display is a slow-breathing waterfall rather than a real-time one.

Concretely, decimating down to a few hundred Hz of bandwidth around the tuned frequency is well
within what this codebase already does today: `dsp::multirate::PowerDecimator::getMaxRatio()` is
`1 << 13` = 8192:1 (`core/src/dsp/multirate/decim/plans.h`), and `RadioSpectrumPreview::snapWidth()`
already snaps a requested VFO width to the nearest power-of-two decimation of the source's live
sample rate specifically to keep that decimation cheap and glitch-free (see
`RADIO_SPECTRUM_FILTER_PLAN.md`'s postmortem on the 1.16-million-tap resampler bug that exact
mechanism was built to avoid) — the same snapping logic applies here, just aimed at a much
smaller target width. At a decimated rate of ~200Hz, a few-thousand-point FFT window gives
sub-Hz resolution, and — per Ralph's direction below on making resolution and update rate
independent sliders — that fine a window doesn't have to mean an equally slow *update* rate:
overlapping successive windows (hop size < window size) can refresh the display far more often
than "wait for a whole fresh window's worth of samples" would otherwise require, at the cost of
neighboring rows sharing input samples rather than being fully independent in time. Either way,
the frequency-resolution side of this genuinely does cost real wall-clock time to achieve — that
part is physics, not a code limitation. See "Design" below for exactly how the two knobs are
decoupled.

## Why a single static FFT frame isn't enough, and what to build instead

A single high-resolution FFT snapshot at this resolution is visually noisy — two carriers a few
Hz apart, similar strength, are genuinely hard to pick out bin-to-bin in one frame with normal
band noise. Jaguar's own display is a **waterfall**, not a static spectrum trace, specifically
because a real carrier shows up as a stable vertical streak accumulating over tens of seconds to
minutes, while noise doesn't — that persistence is what actually makes the offset readable, not
the resolution number alone. This plan is a small scrolling FFT-row history under a live trace,
not a bigger version of `MiniSpectrum`.

**Not reusing `WaterFall` directly for this.** `gui::waterfall`'s `WaterFall` class
(`core/src/gui/widgets/waterfall.h/.cpp`) is a single global instance tightly coupled to the
whole app's tuning model — VFO boxes, band plan overlays, click-to-tune, frequency-scale
dragging, and (per the recording-scheduler work) the active-recording tuning lock. Standing up a
second one for a small popup would drag in all of that unrelated machinery for no benefit, the
same reason `MiniSpectrum` was built as a small purpose-built widget rather than misusing
`WaterFall` for the filter-preview strip. This feature gets its own small widget instead: a
rolling ring buffer of FFT-magnitude rows (bounded to a brief wall-clock span — see the 60-second
cap under "Design" below) plus a bare heatmap `ImDrawList` blit — no VFO concept, no band plan,
no click-to-tune, just rows of color scrolling down under a live trace line, closer in spirit to
how `MiniSpectrum` itself draws its single trace than to the full `WaterFall`.

## Design

**Single, app-wide feed, `CarrierZoomView`** (proposed: `decoder_modules/radio/src/carrier_zoom.h`,
alongside `spectrum_preview.h`) — structurally the same shape as `RadioSpectrumPreview` (own
`addVFO()` tap, own FFTW plan, thread-safe acquire/release of the latest result), but:
- **One instance total, not one per `RadioModule`** — per Ralph (2026-08-16): "No multiple
  simultaneous windows to start." A static/global object (parallel to how `gui::waterfall` is
  itself a single global despite VFOs being per-instance), owned outside any particular
  `RadioModule` instance. Triggering it from a `RadioModule` instance while it's already showing
  a *different* instance's frequency **retargets** it — closes out the old VFO tap and opens a
  new one at the new offset — rather than opening a second window or refusing the second trigger.
  Triggering it again from the *same* instance that already has it open just brings the existing
  window to the front (no retarget, nothing to change). This is the simplest option that
  satisfies "no multiple simultaneous windows," picked because Ralph flagged this as something he
  may revisit once he's used it a bit — easiest starting point to change later in either direction
  (true per-instance windows, or a hard refusal instead of retargeting) once there's a real
  opinion about which is actually more useful in practice.
- **Width**: starts at **500Hz total span, ±250Hz either side of the nominal carrier, per Ralph
  (2026-08-16)** ("half kiloHertz, .25 on either side"), user-adjustable (zoom in/out) from there
  while the window is open, unlike the preview's fixed-per-mode width. Snapped via the same
  `snapWidth()`-style logic against the source's live sample rate.
- **Resolution and update rate are two independent free sliders, per Ralph (2026-08-16)** —
  not a fixed tuning, not presets. This means the underlying FFT can't be the plain
  non-overlapping `RadioSpectrumPreview`-style scheme (there, window size sets *both* resolution
  and update interval together, since the next FFT can't start until the previous one's worth of
  fresh samples has arrived). Instead: an overlapping short-time FFT, where **window size**
  (the resolution slider) and **hop size** — how many new samples are consumed before the next
  FFT fires, always `<= window size` (the update-rate slider) — are set independently. A large
  window with a small hop gives fine resolution *and* frequent updates, at the cost of successive
  rows sharing most of their input samples (standard, well-understood STFT overlap — the rows are
  correlated with their neighbors rather than fully independent samples in time, same as any
  overlapped spectrogram, not a defect). The one real floor: hop size can't outrun the decimated
  sample rate itself (can't consume samples faster than they arrive) — the update-rate slider's
  fastest setting is capped there, not at zero.
- **Output feeds a rolling history**, not just one current-frame buffer: each computed FFT row
  is pushed into the new widget's ring buffer (for the waterfall history) in addition to being
  kept as the single latest row (for the live trace on top, mirroring `MiniSpectrum`'s own
  trace-drawing code). Capped at **60 seconds of wall-clock history, per Ralph (2026-08-16)** —
  "relatively brief, no more than a minute" — not a fixed row count: since the update-rate slider
  changes how many rows correspond to 60 seconds, the ring buffer's row capacity is
  `ceil(60s / current_hop_interval)`, recomputed (and reallocated) whenever the update-rate
  slider moves, rather than a single constant picked for one particular rate.
- **Lifecycle tied to the popup window being open**, deliberately unlike `RadioSpectrumPreview`
  (which the filter-preview plan explicitly left running even when its panel is scrolled off
  screen — see that plan's "Scope decisions" section). This feature's decimation is far more
  extreme and its whole reason for existing is a deliberate, occasional, user-initiated deep
  look, not an always-on background cost — `init()`/`deinit()` called from the window's own
  open/close, not from `RadioModule`'s own constructor/destructor.

**New widget, `ImGui::CarrierZoomPlot`** (proposed: `core/src/gui/widgets/carrier_zoom_plot.h/.cpp`,
alongside `mini_spectrum.{h,cpp}`) — pure rendering, no knowledge of VFOs or the Radio module,
matching every other custom widget in this codebase (`SliderFloatWithSteps`, `MiniSpectrum`):
takes a ring buffer of FFT rows plus the current span, draws a live trace on top and a scrolling
heatmap beneath it. Auto-ranged color/dB scale from the visible data, the same reasoning
`MiniSpectrum`'s own auto-ranging fix used (a fixed range tuned for a wide view reads as a flat
floor against this much narrower one).

**New floating window**, a single global instance living alongside `CarrierZoomView`'s own
singleton, shown/hidden by a global bool flag flipped from the trigger below (and, per the
retargeting behavior above, aware of *which* `RadioModule` instance currently owns it). A plain
`ImGui::Begin()`/`End()` window (not a popup — this should stay open and interactive alongside
the rest of the UI, closable via its own titlebar, not dismissed by an outside click the way an
`ImGui::Popup` would be) containing: the `CarrierZoomPlot`, a width/zoom control, the resolution
and update-rate sliders described above, and a numeric readout of the current center frequency
for reference. Closing it calls `CarrierZoomView::deinit()`, per the lifecycle decision above.
**No persisted state across closes, per Ralph (2026-08-16)** — width, resolution, and
update-rate all reset to their defaults every time the window is (re-)opened, no new
`ConfigManager` keys for this feature. Retargeting (opening from a different `RadioModule`
instance while already open) also resets to these same defaults, rather than carrying the
previous instance's slider positions over onto the new one.

**Trigger**: `MiniSpectrum::draw()` already hit-tests its region via a plain `ImGui::Dummy(size)`
item (`core/src/gui/widgets/mini_spectrum.cpp`), which responds to the normal
`IsItemHovered()`/`IsItemClicked()`/`IsMouseDoubleClicked()` family with no changes to the
widget's own code. The check (`IsItemHovered() && (IsMouseDoubleClicked(ImGuiMouseButton_Left) ||
IsItemClicked(ImGuiMouseButton_Right))`) lives in `RadioModule::menuHandler()`, right after the
existing `specWidget.draw(...)` call. It calls into the global `CarrierZoomView`/window state:
open-or-retarget-to-this-instance, seeding the initial offset from the same
`vfo->wtfVFO->centerOffset` the preview itself already uses.

## Resolved (Ralph, 2026-08-16, same day as the initial plan)

- **Resolution and update rate**: two independent free sliders, not presets or a fixed tuning.
  See the overlapping-STFT design above.
- **History depth**: brief, no more than 60 seconds of wall-clock time.
- **State persistence across closes**: none. Resets to defaults every time the window is opened.
- **Default zoom width**: 500Hz total span, ±250Hz either side of the nominal carrier.
- **Multiple simultaneous zoom windows**: none, to start — a single, app-wide `CarrierZoomView`/
  window, retargeted rather than duplicated when triggered from a different `RadioModule`
  instance than whichever currently owns it. Explicitly flagged by Ralph as something he may
  revisit once he's used it — the design keeps this as one clearly-named decision point
  (open-or-retarget-to-this-instance) rather than something spread across multiple places, so
  it's cheap to change later (to true independent per-instance windows, or to refusing the
  second trigger instead of retargeting) without a larger rework.

## Open questions still worth resolving before or during implementation

- **Default resolution/update-rate slider positions** on first open — a "look and see" number,
  same flavor as the width default was before it got resolved above, now that both are free
  sliders rather than fixed. Needs to be something reasonable out of the box (not maximally
  slow/fine, not maximally fast/coarse) rather than landing at either extreme.

## Status

**2026-08-16: implemented**, commit `89e46b87`. `core/src/gui/widgets/carrier_zoom_plot.{h,cpp}`
(the widget), `decoder_modules/radio/src/carrier_zoom.h` (the DSP feed),
`decoder_modules/radio/src/carrier_zoom_window.h` (the single app-wide owner + floating window),
wired into `radio_module.h`'s `menuHandler()`/`disable()`.

One design refinement made during implementation, not called out explicitly above: the "default
resolution/update-rate slider positions" open question resolved itself into concrete numbers
while building `CarrierZoomView` — `DEFAULT_RESOLUTION_HZ = 2.0`, `DEFAULT_UPDATE_INTERVAL_SEC =
1.0`, with a practical floor of `MIN_UPDATE_INTERVAL_SEC = 0.2` (seconds) that the update-rate
slider can't go below — needed regardless of "free slider," since nothing else bounds how large
the 60-second-capped history's row count (and therefore `CarrierZoomPlot`'s own per-frame texture
rebuild cost) can get at the fast end. Documented on `CarrierZoomView` itself, not hidden.

Verified live against a real, running session (the actual `root` config, RSR200 recording
scheduler active, not a synthetic test): built clean end to end (the `radio` target alone, then a
full multi-target rebuild — zero new errors, only this project's own pre-existing warning noise),
bundled, and driven interactively via `cliclick` rather than just launched-and-not-crashed. The
double-click trigger opened the window with sensible defaults (width snapped against the live
source's actual rate, resolution 2.00Hz/bin, update interval clamped to 0.50s/row at that width);
dragging the width and resolution sliders live re-rendered the plot correctly each time (wider
width + finer resolution produced a visibly sharper waterfall, and the actual-update-interval
readout tracked the hop-vs-window clamp correctly throughout); the rendered waterfall showed a
real, stable carrier streak at center — the exact signal this feature exists to make visible, not
staged. Closed cleanly via the window's own titlebar button. The app stayed alive and responsive
throughout the whole interaction and for 15+ seconds afterward, then quit cleanly by PID. A
second, independent SDR++ process the user had running separately (a different app bundle) was
left completely untouched throughout.

**Not yet verified**: real side-by-side separation of two actual co-channel stations at a real
graveyard frequency (this session's test tuned to a live medium-wave band generally, not
specifically parked on a known multi-station graveyard channel) — the feature's actual real-world
payoff, as opposed to its mechanics, needs Ralph's own ears/eyes against a frequency he knows has
multiple stations on it.

**2026-08-18: third heap-corruption bug found and fixed, in `dsp::buffer::Reshaper` usage, not
`CarrierZoomView` itself.** Ralph hit this live, pushing the width slider down toward ~200Hz and
resolution down toward ~0.3Hz/bin (rapidly, exploring whether the wide lines near the nominal
frequency were actually multiple carriers) — same crash signature and same call site
(`CarrierZoomView::allocateFFT()` ← `recomputeWindowAndHop()` ← `setWidth()`) as the two crashes
`2d700441` already fixed, meaning that fix, while real and necessary, wasn't the last gap.

Root cause: `setWidth()`/`setResolutionHz()`/`setUpdateIntervalSec()` bracket `fftSink.stop()`/
`start()` around the whole mutation (per `2d700441`'s own fix), but never touched `reshape` at
all — `reshape.setKeep()`/`setSkip()` were the only calls made against it, from inside
`recomputeWindowAndHop()`. Read `core/src/dsp/buffer/reshaper.h` directly to check
`Reshaper::setKeep()`/`setSkip()`'s own safety (both call `tempStop()`/`tempStart()`, and reading
`core/src/dsp/block.h` confirmed those really do fully join-and-recreate `Reshaper`'s own two
worker threads, not some lighter partial pause -- so `reshape`'s *own* internal state was never
literally stale). The actual gap: `reshape`'s own worker thread (`Reshaper::run()`, reading from
`dspVFO->out`) keeps running the entire time `dspVFO->setOutSamplerate()` executes earlier in
`setWidth()`, unprotected by anything -- the same shape of race the `2d700441` fix closed one
layer down (fftSink vs. reshape's own reconfiguration), just one layer further up the pipeline
(reshape vs. the VFO's own reconfiguration), and missed because the earlier investigation focused
on fftSink/FFT-buffer sizing specifically, not on auditing every block in the chain for the same
class of gap.

Fix: `reshape.stop()`/`reshape.start()` now bracket the entire mutation in all three setters too,
matching `fftSink`'s own bracket, stopped downstream-to-upstream (`fftSink` then `reshape`) and
started upstream-to-downstream (`reshape` then `fftSink`) -- the general safe pattern for
reconfiguring a live pipeline. `reshape.setKeep()`/`setSkip()`'s own `tempStop()`/`tempStart()`
calls become harmless no-ops once `reshape.stop()` has already set `running=false`, not a second
stop of an already-stopped block (confirmed by reading `tempStop()`/`tempStart()`'s own guard
logic in `block.h`).

Verified live against the exact configuration that crashed (width ≈200Hz, resolution ≈0.3Hz/bin,
Ralph's own recording (`baseband_1123428Hz_02-57-00_17-08-2026.wav`) at 1490kHz): 100+ rapid,
unpaced slider interactions across width/resolution/update-interval, including sustained
alternating min-to-max jumps on all three sliders simultaneously and settings matching/exceeding
the crash trigger (down to 191Hz width, 0.29Hz/bin, 0.27s update interval) — all survived, app
stayed alive and responsive throughout, quit cleanly by PID afterward. Full multi-target rebuild
with Perseus support, bundled, final smoke test also clean.
