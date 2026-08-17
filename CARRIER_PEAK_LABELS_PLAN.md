# Carrier Zoom: per-carrier frequency measurement and labeling

Design brief from Ralph, 2026-08-17, given as a feasibility question first (see "Why this is
feasible" below for the answer that was given before this plan was written):

> Is it possible to measure the actual frequency of each carrier and provide a label with that
> frequency and maybe a different colored line to differentiate from a line at the nominal
> frequency?

This is a follow-on to `CARRIER_ZOOM_PLAN.md` (implemented, commit `89e46b87`, heap-corruption
fix in `2d700441`) — it adds carrier-measurement on top of the existing zoom/waterfall display,
not a new top-level feature. Familiarity with that plan (`CarrierZoomView`,
`ImGui::CarrierZoomPlot`, `CarrierZoomWindow`) is assumed throughout.

## Why this is feasible

The zoom display already exists specifically to make small carrier offsets *visible*
(`CARRIER_ZOOM_PLAN.md`'s whole reason for existing); this plan adds *measuring* what's already
visible, using one standard, cheap DSP technique plus straightforward peak-finding — no new DSP
infrastructure, no new VFO taps, nothing that touches `CarrierZoomView`'s existing FFT feed
itself.

- **Sub-bin frequency precision**: a raw peak bin only resolves to the resolution slider's own
  Hz/bin granularity. **Parabolic (quadratic) interpolation** across the three bins around a
  local maximum — fit a parabola through `(peak-1, peak, peak+1)` in dB-magnitude, the vertex
  gives a fractional-bin correction — is a standard, cheap refinement that works especially well
  here because `fftHandler()` already windows with Nuttall (`carrier_zoom.h:266-269`), whose very
  low sidelobes (~-93dB, 4-term Nuttall) keep a real carrier's mainlobe shape close to the clean
  parabola the interpolation assumes. With the resolution slider able to reach 0.2Hz/bin
  (`MIN_RESOLUTION_HZ`), interpolated precision on a stable carrier should land well under 1Hz —
  plenty to separate graveyard-channel stations typically a few Hz to a few tens of Hz apart.
- **Multiple carriers, not just the strongest**: needs real peak-finding (local maxima with a
  minimum prominence above the noise floor and a minimum separation between accepted peaks), not
  `argmax`. The same low-sidelobe Nuttall window that helps precision also helps here: a single
  strong carrier's own sidelobes are much less likely to get mistaken for a second station than
  they would be with a lower-quality window, so a conservative separation threshold should be
  enough to avoid double-counting one carrier as two.
- **Absolute frequency for the label**: `CarrierZoomView` only knows Hz-offset-from-center today;
  nothing about carrier zoom currently knows the absolute tuned frequency at all (the plot draws
  a fixed line at 0Hz-offset, never a number). The absolute frequency is available where the
  window gets triggered, though — `radio_module.h:267` already has
  `_this->vfo->wtfVFO->centerOffset` at hand, and the standard pattern elsewhere in this codebase
  (`main_window.cpp:412` etc.) is `gui::waterfall.getCenterFrequency() + <offset>`. This needs
  **`centerOffset` specifically, not `generalOffset`** — see `radio_module.h:229-238`'s own
  comment on why those two differ for USB/LSB (edge-referenced vs. passband-center-referenced):
  `CarrierZoomView`'s VFO tap is itself seeded from `centerOffset` (`radio_module.h:267`), so
  that's the true zero-point every peak's offset is measured against, and the only one that keeps
  "nominal + measured offset" honest for every mode, not just the AM/SAM/NFM/WFM ones where the
  two happen to coincide.
- **Rendering** is the easy part — more `AddLine()`/`AddText()` calls in `CarrierZoomPlot::draw()`
  alongside the center tick that's already there (`carrier_zoom_plot.cpp:142-143`).

## Design

### Peak detection — `CarrierZoomView` (`decoder_modules/radio/src/carrier_zoom.h`)

Runs inside `fftHandler()` (`carrier_zoom.h:260-282`), right after `row` (the dB-magnitude
spectrum) is computed and before it's pushed into `history` — the row and `_windowSize` are
already in hand there, on the same worker thread `allocateFFT()`/`freeFFT()` are already
guaranteed not to race against (see that function's own synchronization comment).

1. **Noise floor estimate**: the 20th-percentile value of `row`, via `std::nth_element` (O(n)
   average, cheap even at `MAX_WINDOW_SIZE = 65536` and the fastest allowed hop rate, since it
   only runs once per FFT — same cost class as the FFT itself). A percentile rather than the mean:
   with a narrow span (500Hz default) and at most a handful of real carriers, the vast majority of
   bins are genuinely noise, so a low percentile tracks the floor robustly without a real carrier
   dragging it upward the way a plain mean would.
2. **Candidate peaks**: bins `1..n-2` where `row[i] > row[i-1] && row[i] > row[i+1]` (local
   maximum) and `row[i] >= noiseFloor + PEAK_PROMINENCE_DB`.
3. **Minimum separation**: `MIN_PEAK_SEPARATION_BINS` — greedily accept candidates strongest-first,
   rejecting any candidate within that many bins of an already-accepted peak. Proposed default
   **8 bins**, roughly twice a 4-term Nuttall window's own mainlobe half-width, so two accepted
   peaks are almost certainly two distinct signals rather than one carrier's mainlobe shoulder
   counted twice.
4. **Cap**: at most `MAX_PEAKS` accepted (proposed default **5**) — a busy graveyard channel could
   have more candidate bumps than are worth labeling at once; keep the strongest N.
5. **Sub-bin refinement**: for each accepted integer bin `i`, parabolic interpolation —
   `δ = 0.5 * (row[i-1] - row[i+1]) / (row[i-1] - 2*row[i] + row[i+1])`, refined bin `= i + δ`
   (a proper local max keeps `δ` within `[-0.5, 0.5]` by construction; clamp defensively anyway).
6. **Bin → Hz offset**: same convention `fftHandler()`'s own pre-rotation comment documents (bin 0
   = low edge of the span): `offsetFromCenterHz = (refinedBin / n) * _widthHz - _widthHz / 2`.

**Cross-frame tracking**, to avoid label/line jitter on a per-frame noisy detection: a persistent
`trackedPeaks` member (alongside `history`, under the same `histMtx`), each holding an
exponentially-smoothed `offsetHz`, a `magnitudeDb`, and a miss counter. Each new detection pass
matches raw peaks to the nearest existing tracked peak within a small Hz tolerance (a few bins'
worth); a match blends the offset (fixed smoothing factor, same flavor as `CarrierZoomPlot`'s own
dB-range convergence in `carrier_zoom_plot.cpp:96-100`, not copied — that one ramps from a hard
reset, this one doesn't need to); an unmatched raw peak starts a new tracked entry; a tracked
entry unmatched this frame increments its miss counter and is dropped after a few consecutive
misses (a real signal fading for one hop shouldn't make its line/label disappear and reappear).
This also gives peaks a stable identity across frames, so labels don't reorder or flicker as
relative strength shifts slightly frame to frame. Deliberately not doing this by averaging
several raw rows together first: at fast update-rate slider settings the STFT hop is already
much smaller than the window (heavy overlap), so successive rows are already highly correlated
and get most of that smoothing for free; at slow settings, the cross-frame tracker above is doing
the real work anyway, and averaging raw rows first would just add latency on top of it for no
extra benefit.

New accessor, mirroring `acquireHistory()`/`releaseHistory()`'s existing shape
(`carrier_zoom.h:162-172`):
```cpp
struct PeakInfo { double offsetHz; float magnitudeDb; };
bool acquirePeaks(std::vector<PeakInfo>& peaks); // same histMtx, same false-if-!_init contract
void releasePeaks();
```

### Absolute frequency — `CarrierZoomWindow` (`decoder_modules/radio/src/carrier_zoom_window.h`)

`open()` gains one parameter, captured once at open/retarget time (not recomputed every frame):
```cpp
void open(const std::string& ownerName, double offset, double absoluteFreqHz);
```
Call site (`radio_module.h:267`) passes `gui::waterfall.getCenterFrequency() +
_this->vfo->wtfVFO->centerOffset` alongside the `centerOffset` already passed as `offset`. This
deliberately snapshots the nominal frequency at open/retarget time rather than tracking later
retuning of the same `RadioModule` instance while the zoom window stays open — matching
`CarrierZoomView`'s own existing lifecycle (the VFO tap itself is also only ever set at
open/retarget, never live-followed; see `CARRIER_ZOOM_PLAN.md`'s "Lifecycle" note). Worth
flagging as a real, pre-existing limitation this plan doesn't fix: if the user retunes the
underlying receiver while a Carrier Zoom window is open on it, both the spectrum data *and* now
the label stay pinned to the frequency at the moment it was opened, not the live tuning. Given the
feature's own stated use (park on a known graveyard channel, watch it settle in over tens of
seconds), that's likely fine as-is, but it's a clean, independent follow-up if it turns out not to
be.

`draw()` passes the stored `absoluteFreqHz`, plus peaks acquired via the new
`acquirePeaks()`/`releasePeaks()`, down into `plot.draw(...)`.

### Rendering — `ImGui::CarrierZoomPlot` (`core/src/gui/widgets/carrier_zoom_plot.h/.cpp`)

`draw()` gains two parameters: `double nominalFreqHz`, and the peaks list (`const PeakInfo*
peaks, int peakCount`, mirroring the existing `rows`/`rowCount` convention rather than a
`std::vector` reference, matching this file's existing plain-pointer style).

For each peak, alongside the existing center-tick line (`carrier_zoom_plot.cpp:142-143`):
- **X position**: `x = bb.Min.x + (peak.offsetHz + spanHz / 2.0) / spanHz * size.x` — same mapping
  the center tick already uses (center tick is just this formula's `offsetHz == 0` case).
- **Line**: a distinct color from the existing center line's `IM_COL32(255, 0, 0, 180)` red —
  proposed **cyan, `IM_COL32(0, 255, 255, 200)`** — reads clearly against both the dark
  trace-pane background and the waterfall's own red/yellow/white hot colors at the top of the dB
  range (where a strong carrier's own waterfall pixels already are), and matches the
  cyan/green-marker convention other SDR spectrum tools already use, so it won't read as
  unfamiliar. Spans the same `bb.Min.y..bb.Max.y` range the center line does, or could be
  restricted to just the trace pane (`traceBB`) if a full-height line over the waterfall reads as
  too busy once history has scrolled in — worth a quick look once actually running before
  deciding.
- **Label**: `dl->AddText(...)` near the top of the trace pane, showing the absolute frequency —
  `nominalFreqHz + peak.offsetHz`. Exact format is an open question below (candidates: plain Hz
  with one decimal, e.g. `1489997.3 Hz`; or the offset alongside it, e.g. `1489997.3 Hz (-2.7)`).
- **Label collision**: with a narrow default span (±250Hz) and up to `MAX_PEAKS = 5` labels, two
  peaks close together in Hz will have close-together X positions and can overlap text. Proposed
  first cut: alternate label vertical offset (stagger every-other label a line-height lower) when
  two peaks' pixel X positions are closer than the label's own rendered width — a simple, local
  fix, not a general label-layout solver. Flagged as something to actually look at once real
  multi-station data is on screen, not a doc-level guess.

## Resolved (Ralph, 2026-08-17)

- **Label content**: absolute frequency alone — no separate offset-from-nominal shown alongside
  it. One decimal place (matches the sub-Hz-class precision parabolic interpolation can plausibly
  deliver): `dl->AddText(...)` formats as `"%.1f Hz"` (or, once a real absolute frequency in the
  hundreds-of-kHz-to-MHz range is substituted in, whatever `%.1f` renders for that full value —
  e.g. `"1489997.3 Hz"` — no thousands separators or kHz/MHz unit-switching, matching this
  codebase's existing plain-Hz label convention, `main_window.cpp:646`).
- **`PEAK_PROMINENCE_DB`**: 6dB, as proposed. A starting number, expected to be retuned once this
  is running against a real graveyard channel.
- **Peak markers on/off**: a checkbox in the `CarrierZoomWindow` UI, alongside the existing
  width/resolution/update-rate sliders (`carrier_zoom_window.h:68-88`). Default on. Unchecking it
  should skip drawing the lines/labels only — `acquirePeaks()` still runs every frame regardless
  (detection itself stays live so the checkbox is instant, not a re-detect-on-toggle delay).
- **`MAX_PEAKS` / `MIN_PEAK_SEPARATION_BINS`**: fixed constants to start (5 and 8, as proposed),
  not exposed as sliders. May be adjusted based on real-world testing against an actual graveyard
  channel, same "look and see, retune by feel" treatment already given to
  `PEAK_PROMINENCE_DB` and to `CarrierZoomView`'s own default resolution/update-rate values in
  `CARRIER_ZOOM_PLAN.md`.

## Status

**2026-08-17: implemented.** Peak detection/tracking in `CarrierZoomView`
(`decoder_modules/radio/src/carrier_zoom.h`: `detectPeaks()`, `updateTrackedPeaks()`,
`acquirePeaks()`/`releasePeaks()`), `absoluteFreqHz` plumbed from `RadioModule` through
`CarrierZoomWindow::open()`/`draw()`, and cyan peak lines + absolute-frequency labels +
"Show carrier peaks" checkbox in `ImGui::CarrierZoomPlot` (`core/src/gui/widgets/
carrier_zoom_plot.h/.cpp`).

Two real bugs found and fixed during live verification, not caught by compiling/launching alone:

- **Deadlock (beachball) on first open.** `CarrierZoomWindow::draw()` called
  `view.acquirePeaks()` (locks `histMtx`) *while already holding* the lock `view.acquireHistory()`
  took a few lines above — `std::mutex` isn't reentrant, so the GUI thread blocked on a lock it
  already held, forever. Fixed by sequencing the two acquire/release pairs one after the other,
  never nested.
- **Garbled, overlapping peak labels** with more than two close peaks. The first label-collision
  fix only compared each label to its immediately preceding neighbor, so every *other* label could
  silently re-collide with the one two slots back on the same row. Fixed with a proper two-row
  check (each label tries its preferred row, falls back to the other, checked against that row's
  own last occupant) — and, since the floating window itself can render far narrower than a
  label's own text width (a pre-existing sizing quirk of this window, unrelated to peak labels
  specifically — confirmed with Ralph this isn't how he'd actually run it sized that narrow), a
  label whose text won't fit on *either* row is skipped rather than drawn overlapping and
  unreadable; its line is still always drawn.

Verified live against the real, running session (root config, RSR200 recording scheduler active,
tuned to a genuinely crowded medium-wave band, not synthetic test data): full multi-target rebuild
with Perseus support clean (zero new errors/warnings beyond this project's own pre-existing
noise), bundled, driven interactively via paced `cliclick`. Confirmed: the peak-labels checkbox
toggled cleanly 6x rapidly with no hang; two real, close carriers (`1399715.1 Hz` / `1399759.7
Hz`, ~45Hz apart) were detected, labeled legibly, and tracked smoothly frame to frame as their
levels shifted; a combined stress pass (width/resolution/update-interval sliders plus repeated
checkbox toggling, mirroring the earlier `CarrierZoomView` heap-corruption fix's own stress
methodology) survived with peaks continuing to track correctly throughout; app stayed alive and
responsive, quit cleanly by PID afterward.

**Not yet verified**: real side-by-side identification of two co-channel stations at a known
graveyard frequency by call sign/programming (this session's test found genuine close carriers on
a live band generally, not a specific known graveyard channel) — same caveat
`CARRIER_ZOOM_PLAN.md` itself already carries, now extended to the labels built on top of it.
`PEAK_PROMINENCE_DB`/`MAX_PEAKS`/`MIN_PEAK_SEPARATION_BINS` are all still their first-guess
defaults, expected to be retuned by feel per the "Resolved" section above.

**2026-08-17, same day: label units switched to kHz.** Ralph's own follow-up after trying it:
labels initially reused `utils::formatFreq()` (this codebase's shared MHz/KHz/Hz-auto-selecting
convention, used for the main tuned-frequency readout and every other frequency label in the app)
— correct in general, but wrong for this specific widget: broadcast-band DXing always describes a
frequency in kHz regardless of which side of the 1MHz mark it falls on ("1400 kHz", never "1.4
MHz"), so a medium-wave carrier just above 1MHz was showing as e.g. "1.399689MHz" instead. Fixed
with a small local `formatPeakFreqKHz()` in `carrier_zoom_plot.cpp` that shares `formatFreq()`'s
own precision/trim algorithm (6 decimal places on the kHz value, trailing zeros and a bare
trailing decimal point both trimmed) but pins the unit to kHz rather than auto-selecting it —
deliberately not a change to the shared `utils::formatFreq()` itself, which stays correct as-is
for the main readout and everywhere else that isn't this specifically-broadcast-band-focused
widget. Rebuilt, bundled with Perseus support, verified live (labels now read e.g. "1399.665116
kHz"), quit cleanly.
