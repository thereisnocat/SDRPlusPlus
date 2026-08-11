# Radio module: spectrum preview + draggable asymmetric passband

Originates from RSR200_PLAN.md section 12 item 4 ("wants a small always-visible spectrum
window zoomed to the current frequency") and item 1's neighbour concern about the existing
zoom control's limited resolution, but this isn't RSR200-specific -- it lives entirely in
`decoder_modules/radio` and applies to every source. Design brief from Ralph, 2026-08-10:

> The right place for the spectrum window is in the radio module above the mode selection
> radio buttons. It should display the carrier and the surrounding spectrum with a shaded
> area showing the current filter. The filter should be able to be adjusted by dragging on
> the edge of the shaded area. Filters should also be able to be asymmetrical, to account
> for avoiding adjacent channel interference.

## Why not just reuse the main waterfall's FFT

`gui::waterfall`'s public `acquireLatestFFT()` returns whatever's currently on-screen, cropped
to the user's current pan/zoom (`getViewBandwidth()`/`getViewOffset()`). Reusing it directly
would reproduce the exact resolution problem item 4 already flagged with the existing zoom
control: zoomed out to browse a band, a 10-40kHz slice around one VFO might only cover a
handful of bins of that array. `getFFTBuffer()`/`rawFFTs` (the full-input-bandwidth raw FFT,
independent of pan/zoom) exists but has no synchronized public read API, and even that would
still tie resolution to the *whole* input bandwidth divided by one shared FFT size -- fine for
a 2MHz-wide waterfall, still coarse for a 10kHz AM channel.

There's a second, harder problem: the surrounding spectrum has to include the *adjacent*
channel the whole feature exists to help you avoid -- which means it can't be read from
anywhere already filtered down to the current passband (the demod chain's own VFO output is
exactly that, so it's unusable as a source here by construction).

## Design

**A second, invisible VFO extraction, dedicated to this preview.** `IQFrontEnd::addVFO()` is
the same call `VFOManager::VFO`'s constructor already uses for the real demod VFO -- calling it
directly (bypassing `VFOManager`) gets a fully independent `RxVFO` tap with no
`ImGui::WaterfallVFO` counterpart, so it never shows up as a second box on the main waterfall.
Width tracks the current mode's bandwidth (`clamp(bandwidth * 4, 3kHz, 500kHz)` -- wide enough
to show the adjacent channel this feature exists for, capped so WFM doesn't pull absurd
amounts of spectrum) and is set as both the RxVFO's bandwidth *and* its output samplerate, so
`RxVFO` never builds a filter for this tap at all -- just a clean frequency-shifted, decimated
slice. A small dedicated FFT (fftw, Nuttall window, alternating-sign pre-shift so bin 0 is the
left edge instead of DC -- exactly `IQFrontEnd::updateFFTPath()`'s own trick, copied rather
than reinvented) runs on that slice at a fixed ~20Hz update rate, sized down automatically via
the same `min(sampleRate/fftRate, fftSize)` zero-pad logic `IQFrontEnd::genReshapeParams` uses,
so narrow-bandwidth modes (CW) don't ask for more FFT resolution than their preview's own
(deliberately floor-clamped) sample rate can actually deliver.

**Asymmetric passband, implemented once, shared by every mode.** The actual RF-shape-defining
filter for every mode except SSB already lives in one place: `RxVFO::generateTaps()`, called
by the real (non-preview) VFO shared by the whole demod chain -- currently always
`taps::lowPass(bandwidth/2, ...)`, a filter symmetric around the tuned center by construction.
`dsp::taps::bandPass<complex_t>(bandStart, bandStop, ...)` already exists and already supports
independent, non-symmetric edges (used nowhere yet) -- swapping to it only when the edges
aren't symmetric (`lo != -bandwidth/2 || hi != bandwidth/2`) means every mode that never
touches the new control keeps generating the exact byte-identical symmetric filter it does
today; nothing about existing behaviour changes unless the user actually drags an edge.

`bandwidth` (the existing numeric field, mode defaults, min/max limits, sample-rate-driving
logic, waterfall VFO box width) stays exactly what it is today -- the *outer* window. The new
drag-edges are a *trim within that window* (`passbandLo/Hi`, clamped to `[-bandwidth/2,
bandwidth/2]`), not a replacement for it: widen the outer window with the existing numeric
field first, then pull one edge in via the new control to dodge a specific adjacent signal --
narrowing only the side that needs it, which is the whole point ("asymmetrical, to avoid
adjacent channel interference"). Persisted per mode exactly like `bandwidth` already is
(`config.conf[name][demodName]["passbandLo"/"passbandHi"]`), clamped into whatever the current
bandwidth window is on load in case either changed since the value was saved.

> **Superseded 2026-08-10** (see "Live-testing feedback round 2" below): this paragraph
> describes the *original* design, where `bandwidth` was a hard outer ceiling the passband was
> clamped inside. Live testing found that model actively broken -- editing `bandwidth` at all
> silently clamped an asymmetric trim back towards symmetric, described by Ralph as "the filter
> collapsing." The relationship is now inverted: `passbandLo/Hi` are clamped only against the
> true hard limit (the sample rate), and `bandwidth` is a *derived readout* of `hi - lo`, kept
> in sync after every drag rather than constraining it. Left here rather than rewritten in place
> so the "why" of the original attempt (and why it didn't hold up) stays legible.

**New reusable widget, `ImGui::MiniSpectrum`** (`core/src/gui/widgets/mini_spectrum.{h,cpp}`,
alongside `stepped_slider`/`bandplan` -- this codebase's existing home for standalone GUI
widgets): draws the dB spectrum line, a center tick for the carrier, a shaded rect between
`passbandLo`/`passbandHi`, and two draggable edge handles. Pure rendering + mouse interaction,
no knowledge of VFOs or demodulators -- takes an FFT buffer/span and current edges, returns
whether the user changed them, exactly the "pass pointers, return bool changed" convention
`SliderFloatWithSteps` and every other custom widget here already follows.

## Scope decisions worth flagging

- Every mode gets the preview and the drag control, including CW/RAW -- simpler than
  special-casing, and even CW benefits from seeing what's next to a 200Hz filter.
- No new per-mode `Demodulator` interface changes. This lives entirely at the `RxVFO` layer,
  upstream of where any demodulator-specific code runs, so all 8 demodulator wrapper classes
  are untouched.
- Preview VFO offset isn't event-driven -- `menuHandler()` just calls
  `preview.setOffset(vfo->getOffset())` unconditionally every frame the menu draws.
  `RxVFO::setOffset()` is cheap (retunes a frequency translator, no filter rebuild), and this
  avoids needing a new offset-changed event alongside the bandwidth one that already exists.
- Preview VFO keeps running (using CPU) even while the radio panel/menu is scrolled off
  screen, since nothing currently tells it to stop in that case -- not wired up, matching how
  the rest of this module already doesn't distinguish "enabled" from "visible."

## Status

2026-08-10: implemented --
- `dsp::channel::RxVFO`: `setPassband(lo, hi)`/`getPassbandLo()`/`getPassbandHi()`, filter
  switched from real-tap `taps::lowPass` to complex-tap `taps::bandPass` (mathematically
  identical output for the untouched symmetric case, see the comment on `generateTaps()` for
  the one real cost: every RxVFO now runs complex-tap FIR instead of real-tap, a modest fixed
  CPU increase from the feature existing at all, independent of whether it's in use).
- `VFOManager::VFO`: thin `setPassband`/`getPassbandLo`/`getPassbandHi` passthrough.
- `ImGui::MiniSpectrum` (`core/src/gui/widgets/mini_spectrum.{h,cpp}`): the plot + shaded
  draggable-edges widget.
- `RadioSpectrumPreview` (`decoder_modules/radio/src/spectrum_preview.h`): the dedicated
  invisible wide-VFO-plus-small-FFT feed.
- Wired into `RadioModule`: preview created alongside the main VFO, resized on every bandwidth
  change, offset kept in sync every frame; passband loaded/defaulted per mode exactly like
  `bandwidth` already is, persisted the same way; widget drawn above the mode selector as
  specified.

Verified: clean build of `sdrpp_core` and `radio` in isolation, then a full-project rebuild
(both the `build` and `build_release` trees -- the latter needed its own `cmake` reconfigure
to pick up the two new globbed source files, easy to miss with two separate build trees in
play) with no errors outside the pre-existing, unrelated `rtl_sdr_source`/libusb path issue.
Local `.app` launched live against `root_dev`'s existing enabled Radio instance: stable ~35%
CPU and ~150MB memory over 15+ seconds, no crashes, nothing in the unified log.

**2026-08-10, confirmed visible:** Ralph confirmed the widget is now visible above the mode
selector against the live RSR200, after the stale-module mixup documented further down got
sorted out. **Still not yet done:** actual interactive verification that dragging the shaded
edges feels right, that the asymmetric filter audibly does what it should against a real
signal, and that the 4x-bandwidth preview width default is a sensible starting point rather
than something that needs tuning once seen live -- no computer-use/GUI-automation tool
available to drive this directly, so this still needs a real pass from Ralph the way every
other UI item in this project's punch lists has.

## Regression found and fixed in live testing (2026-08-10, same day)

Ralph reported the widget wasn't visible, against the real, live, connected RSR200. It was
actually there but starved: the app was overloaded (CoreAudio logging "skipping cycle due to
overload" repeatedly) with CPU pegged over 100% and memory climbing steadily past 800MB over
~15 minutes -- bad enough that rendering itself was starving. `sample`'d the running process
rather than guess further: real, ongoing time in `RxVFO::run()`, `Reshaper::run()`/
`bufferWorker()`, `RationalResampler::process()`, and actual `libfftw3f` FFT kernels -- genuine
sustained computation, not idle waiting. The actual smoking gun was in the resampler's own
setup log: `[Resamp] predec: 64, interp: 15000, decim: 15259, ..., taps: 1159667` -- **1.16
million filter taps**, for the preview VFO's own resampler.

Root cause: `dsp::multirate::RationalResampler` reduces `outSamplerate/inSamplerate` to lowest
terms and sizes a polyphase FIR off the result -- fine for a "nice" ratio, catastrophic for an
unlucky one. The preview picked its width from an arbitrary round-number heuristic
(`bandwidth * 4`, clamped) with no awareness of the source's actual sample rate; against
RSR200's actual rate (a non-power-of-two-friendly ~1.95MHz from its ADC clock/decimation
combination) that ratio didn't reduce cleanly, and the resulting filter was being both built
*and run continuously* at that size -- explaining both the sustained CPU and, since it's
per-instance state torn down and rebuilt on every width/offset change, why memory kept
climbing rather than settling. This wasn't a one-off fluke of this specific run: the exact
same failure mode would recur any time the preview's heuristic width happened to land on a bad
ratio against whatever rate the source actually produces, source-dependent and not something
"pick a different round number" reliably avoids.

**Fixed** by not trusting an arbitrary width to produce a reasonable ratio at all: `snapWidth()`
in `spectrum_preview.h` now snaps the requested width to the nearest power-of-two decimation of
the source's *actual* live sample rate (`sigpath::iqFrontEnd.getEffectiveSamplerate()`).
`RationalResampler`'s own predecimation stage is exactly a power-of-two decimator, so this
makes `interp`/`decim` reduce to 1:1 and the whole polyphase-filter path gets skipped
entirely (`if (interp == decim)` fast path) rather than merely made smaller -- by construction,
not by luck. Trades an exact "4x bandwidth" width for "closest power-of-two-of-the-input-rate
to 4x bandwidth," which is fine for what's a rough visual guide.

Verified against the same live RSR200 session: CPU dropped from 100%+ (peaking over 160%) to
10-17%, memory flat at ~165MB over 45+ seconds (previously climbing continuously past 800MB in
15 minutes), matching this project's established healthy baseline for this app.

**Found, but out of scope to fix here -- a separate, pre-existing bug, not caused by this
feature:** the exact same `interp:15000, decim:15259, taps:1159667` pathology also appeared
for what's almost certainly the *main* AM demod VFO (`interp` exactly matches
`demod::AM::getIFSampleRate()`'s hardcoded `15000`), independent of anything this feature
added. Every demodulator hardcodes its own fixed IF sample rate (AM=15000, NFM/DSB/USB/etc.
similarly) with no awareness of what rate the actual source produces, which is exactly the
setup that produces a bad ratio. Unlike the preview VFO, this only happened at startup (3
times, matching mode-provisioning during load) rather than continuously, so it wasn't the
cause of the sustained overload -- but it's a real, several-second CPU spike whenever RSR200
(or plausibly any source with a similarly "unfriendly" sample rate) is used with AM, and the
same `RationalResampler` tap-count blowup could in principle recur continuously rather than
just at startup under different conditions. Worth its own investigation later -- noted here
and in RSR200_PLAN.md's punch list rather than silently left for someone to rediscover.

## The actual "still don't see it" cause: a stale module, not a rendering bug (2026-08-10)

After the resampler fix above, Ralph still reported not seeing the widget. Chased this hard
in the wrong direction first: re-sampled the live process repeatedly, found CPU/memory still
elevated, "fixed" the preview's width-selection twice more (snapping every frame, not just on
bandwidth change) -- each time verified against `root_dev`, each time inconclusive or
seemingly still broken. Eventually ran a clean A/B test (this feature's `init()` short-
circuited to a no-op entirely, rebuilt, same live RSR200) and got the *same* elevated CPU/
memory either way -- proof the sustained load was never this feature's doing at all, probably
just what dual-channel phasing against a live, high-rate RSR200 actually costs. That ruled out
performance as the explanation for "no widget" and pointed at an actual rendering problem, so
added direct instrumentation (a file write from inside `MiniSpectrum::draw()` itself) to prove
whether it was even being called.

It wasn't -- and the reason turned up immediately once looked for directly:
`root_dev/config.json`'s `modulesDirectory` points at `root_dev/modules/`, a plain copy of
each `.dylib`, completely independent of whatever's inside `SDR++.app/Contents/Plugins/`. Every
rebuild-and-test cycle this session had been copying the freshly built `radio.dylib` into the
`.app` bundle and relaunching it against `-r root_dev` -- but `root_dev`'s own `modules/`
folder was last synced hours earlier, before this entire feature existed (`nm` on it turned up
zero references to `MiniSpectrum` or `RadioSpectrumPreview`). Every "live test" after that
point was silently exercising old code, no matter how new the `.app` bundle looked from the
outside. Confirmed by checking `nm -u` on the stale copy (no `MiniSpectrum::draw` reference at
all) versus the freshly-synced one (reference present), then confirming the freshly-synced
version actually gets called via `sample` catching `ImGui::MiniSpectrum::draw` live in the
running process's call stack -- direct proof, not inference.

Separately, and likely the more consequential half of the same mistake: `SDR++ RB.app` (a
second, pre-existing app bundle in the repo root, untouched by any rebuild this entire
session) was last modified at 13:07 -- hours before this feature, the SAM demodulator, or the
ADC clock precision fix even existed. If that bundle is what actually gets opened day to day
rather than `SDR++.app`, none of today's work would have been visible no matter what got
fixed in the source tree.

**Fixed by rebuilding and redeploying everywhere the app could actually be running from**,
not just the one bundle being actively tested: `SDR++.app`, `SDR++ RB.app` (via a
space-free-temporary-name-then-rename workaround -- `make_macos_bundle.sh` passes its bundle
path unquoted throughout, so a path containing a space breaks it; not fixed here, out of
scope), and `root_dev/modules/*.dylib`. Confirmed via direct profiler evidence
(`ImGui::MiniSpectrum::draw` caught executing in the live, foreground, correctly-deployed
process) rather than inference this time.

**Lesson for next time this project runs a "rebuild, redeploy, relaunch, verify" loop across a
session this long:** confirm *which* deployed copy is actually being exercised before trusting
a test result, especially once more than one plausible app bundle or module directory exists.
A live process staying healthy or a feature silently not doing anything can both look
identical to "already fixed" if the thing under test was never actually updated.

## Live-testing feedback round 2 (2026-08-10) -- interaction model fixes

Ralph's feedback once the widget was actually visible: dragging feels good, the preview
width is fine as-is, trimming audibly works -- but three real problems.

**"Changing the value in the Bandwidth field beneath causes the filter to collapse. Locking
the outside bounds of the filter is counter to how this feature works in other software."**
Root cause: `RxVFO::clampPassband()` clamped `_passbandLo`/`_passbandHi` against
`[-_bandwidth/2, _bandwidth/2]` -- treating "bandwidth" as a second, outer ceiling the
passband had to live inside. But bandwidth is a single symmetric scalar; it can't represent
an asymmetric trim, so *any* future touch to it (typing a number, even dragging the main
waterfall's own bandwidth handles) re-clamped an asymmetric passband back towards symmetric,
which is what "collapse" actually was. Fixed by clamping against the one thing that's an
actual hard limit -- the sample rate itself (`_outSamplerate/2`) -- and nothing else;
`setBandwidth()` no longer touches the passband at all. Bandwidth is now a *derived readout*
of the passband (`applyPassbandEdges()` sets `bandwidth = hi - lo` after every drag) rather
than a cage around it, matching the "other software" behaviour Ralph described. The numeric
field and the main waterfall's bandwidth-drag handles still exist and still work, but now
mean "reset to a fresh symmetric window of this width" (`setBandwidthSymmetric()`) -- an
intentional, predictable full reset instead of a silent, lossy partial clamp.

**"Currently not seeing spectrum other than the individual signal."** The plot's dB range was
borrowed from `gui::waterfall.getFFTMin()/getFFTMax()` -- tuned for a much wider view, so
against this preview's narrower span, anything but one strong signal read as a flat floor.
Fixed: `MiniSpectrum` now auto-ranges from the actual data each frame (smoothed so the scale
doesn't visibly jump on noise) instead of taking a min/max from the caller at all.

**"The representation of the filter in USB/LSB appears inaccurate, falling on each side of
the carrier but appearing to work as if it were all on one or the other side."** Real bug,
diagnosed precisely rather than guessed at: the preview was centered on
`vfo->getOffset()` (== `wtfVFO->generalOffset`), but USB/LSB tune from an edge, not the
center (`getVFOReference() == REF_LOWER`/`REF_UPPER`), and `VFOManager::VFO::setOffset()`
actually drives the *real* demod VFO's offset from `wtfVFO->centerOffset` --
`generalOffset` re-based to the true middle of the passband -- never from `generalOffset`
directly. So the *audio* was already being filtered correctly (relative to the correct
center), explaining "works as if it were all on one side"; only the *picture* was wrong,
drawn straddling the carrier because it was centered on the wrong reference point entirely.
Fixed by centering the preview on `wtfVFO->centerOffset`, the same point the real VFO already
uses.

Rebuilt and redeployed to all three places the app can run from this time (`SDR++.app`,
`SDR++ RB.app`, `root_dev/modules`), confirmed via `sample` that `MiniSpectrum::draw` still
executes and the app stays stable, per the lesson from the previous round. Not yet re-tested
live by Ralph.

## Live-testing feedback round 3 (2026-08-10) -- preview zoom stability + carrier tick

Two more from Ralph after round 2 landed. Both real, both diagnosed against the actual code
this time rather than guessed at.

**"The amount of spectrum being shown seems to change dynamically as I adjust the filter
edges, and after the first move, neighboring signals stop being shown and never return."**
Self-inflicted by round 2's own fix: making `applyPassbandEdges()` derive
`bandwidth = hi - lo` and sync it (to fix the numeric-field "collapse" bug) meant `bandwidth`
now changes on every drag frame -- and `menuHandler()`'s per-frame sync was still computing
the preview's zoom width from that same live `bandwidth` (`previewWidthFor(bandwidth)`).
Narrowing the passband narrowed `bandwidth`, which narrowed the preview's zoom to match,
permanently losing whatever adjacent signal had been visible. Fixed by decoupling the two:
new `previewWidthHz` member, set only by `setBandwidth()` (an intentional "change the
bandwidth" action -- mode switch, typing a number, dragging the main waterfall's handles),
left alone by `applyPassbandEdges()` entirely. The preview now stays at a stable zoom level
through however many fine drags on the mini-spectrum itself, only resizing on those
intentional actions -- matching "the window seems to be a good width for now" from round 1,
which was true before any dragging and should stay true throughout.

**"USB/LSB is better, but the spectrum is shown centered on the nominal carrier frequency
rather than all above or all below."** Round 2 fixed the *filter*'s reference point
(`wtfVFO->centerOffset`, matching what the real demod VFO actually uses) but left the *tick
mark* meant to show "here's the carrier" hardcoded at the plot's horizontal center (0 Hz) --
correct for REF_CENTER modes, where centerOffset and the carrier are the same point, wrong
for USB/LSB, where they're `bandwidth/2` apart. The tick was drawn at the true middle of the
passband, not at the carrier, which is exactly what "centered on the carrier ... incorrect"
describes: the marker made it look like the carrier sits in the middle with content on both
sides, when the real carrier is at the edge of the shaded region with content only above (USB)
or below (LSB). Fixed with a new explicit `carrierOffsetHz` parameter to
`MiniSpectrum::draw()`, computed as `generalOffset - centerOffset` (0 for REF_CENTER modes,
where nothing changes) and used only for where the tick itself is drawn -- doesn't touch how
the passband shading or FFT data are positioned, which round 2 already got right.

Rebuilt and redeployed to all three locations again (`SDR++.app`, `SDR++ RB.app`,
`root_dev/modules`). Not yet re-tested live by Ralph.

## Live-testing feedback round 4 (2026-08-10) -- the real bug behind rounds 2 and 3

Ralph's report this round was serious enough to warrant re-reading rounds 2/3's own fixes
rather than patching further symptoms: USB "majorly messed up" (zooms in, never out, wrong
representation), LSB audio actually wrong with an audible heterodyne ("the filter is clearly
including the carrier"), and the carrier tick itself visibly moving while dragging. AM still
zoomed in and didn't recover too.

Root cause, found by re-deriving what `vfo->setBandwidth()` actually does rather than
assuming it was inert: round 2's `applyPassbandEdges()` synced `bandwidth = hi - lo` and
called `vfo->setBandwidth(bandwidth)` on *every drag frame*, to fix the numeric-field
"collapse" bug and keep the field/waterfall-box live-updated. But `VFOManager::VFO`'s
`WaterfallVFO` computes `centerOffset` **from** bandwidth for REF_LOWER/REF_UPPER modes --
`generalOffset +- bandwidth/2`, anchored to the fixed edge USB/LSB actually tune from (see
round 2's own centerOffset-vs-generalOffset writeup) -- and `centerOffset` is what the real
demod VFO's own tuning offset is actually set from. So every drag frame wasn't just resizing a
window; for USB/LSB it was **silently retuning the actual demodulated frequency**, dozens of
times a second, while SSB audio played. That's the heterodyne (a genuine second frequency
beating against the first, because the frequency itself was moving), the carrier tick visibly
sliding (its position is `generalOffset - centerOffset`, and centerOffset was the thing
moving), and -- since `setBandwidth()` also resizes the main waterfall's VFO box -- the zoom
churn on AM too, where centerOffset doesn't move but the box width still did, on every frame,
in whatever direction the trim's total width happened to be drifting.

**Fixed by removing the sync entirely**, not further constraining it: `applyPassbandEdges()`
(the drag path) now only calls `vfo->setPassband(lo, hi)` and persists the two edges -- it
never touches `bandwidth`, `vfo->setBandwidth()`, or `preview.setWidth()` again. The numeric
Bandwidth field and the main waterfall's VFO box now stay exactly where they were last set
*explicitly* (typing a number, dragging the main waterfall's own handles, or a mode switch)
and simply don't track fine trims made on the mini-spectrum widget. This is a real reduction
in scope from what round 2 promised ("Bandwidth is a synced readout of that now") -- but the
alternative was dragging the passband quietly detuning the radio out from under live SSB
audio, which is a correctness bug, not a rough edge. The original round-2 complaint that
started all this ("locking the outside bounds of the filter") is still fixed on its own
terms: `minEdge`/`maxEdge` passed to the widget are still the mode's true ceiling
(`maxBandwidth`), never `bandwidth` itself, so dragging still isn't blocked by a stale field
value -- that part of the diagnosis was correct, it just got bundled with a sync mechanism
that shouldn't have existed.

Rebuilt and redeployed to all three locations, this time verified by checksum against the
freshly built source (not just symbol presence) to catch a stale copy immediately rather than
downstream. Not yet re-tested live by Ralph.

## Live-testing feedback round 5 (2026-08-10) -- freeze the preview width, stop trying to be clever

LSB's heterodyne is fixed. But the zoom-churn complaint (rounds 3 and 4) wasn't actually gone
-- it "still happens in every mode." Two attempts at re-deriving the preview's width live
(first from `bandwidth` every frame, then only on explicit bandwidth changes) both still
visibly changed the zoom while trimming. Ralph's own diagnosis, and the fix taken essentially
verbatim: "calculate the displayed width at mode selection, AND THEN DON'T CHANGE IT." Also
named precisely what the bug actually looked like, worth keeping verbatim for anyone tuning
this further -- a section of spectrum covering maybe three channels turning into one
already-filtered signal zoomed in close "looks like a balloon sitting on a flat surface. It
looks wrong, it feels wrong, it acts wrong."

Implemented literally: `previewWidthHz` is now set in exactly one place,
`selectDemod()`, at mode selection -- not in `setBandwidth()` (bandwidth edits no longer
resize it either, a further reduction from round 4), not per-frame in `menuHandler()`. Only
the preview's *offset* is still synced every frame (tuned frequency can genuinely change
without a mode switch; width, per this round, should not visibly change at all outside one).

Known, accepted trade-off: `RadioSpectrumPreview::setWidth()`'s protection against a
pathological resampler ratio (see the round-1 postmortem section above) now only gets applied
once, at mode-select time, instead of continuously. A source whose sample rate changes
mid-session without a mode switch would be unprotected until the next mode selection refreshes
it. Not something observed in practice -- RSR200 settles its rate once early and holds it --
but worth knowing if that pathology (an absurd tap count logged at startup) ever reappears
*during* normal use rather than only at startup.

Rebuilt and redeployed to all three locations, verified by timestamp this round. Not yet
re-tested live by Ralph.

## Live-testing feedback round 6 (2026-08-10) -- the freeze wasn't the whole story: stale persisted bandwidth

Round 5's freeze fix genuinely stopped `previewWidthHz` from being touched anywhere except
mode selection -- confirmed directly this round by instrumenting the actual running process
(a file-write from inside `menuHandler()`, checked against a live session) rather than
re-reasoning about the code again. With no interaction at all, the width held rock steady for
20+ seconds. So the "still changing" Ralph reported wasn't happening at the mechanism rounds
3-5 all focused on -- it was already fixed. The bug was in what that frozen calculation was
being fed.

Diagnosing this also took a detour worth recording: the diagnostic build wasn't producing any
output at first, which led down a red herring chasing a nonexistent `"menuOrder"` config key
before finding the real one (`"menuElements"`, in `core/src/gui/main_window.cpp`) -- and *that*
turned out fine. The actual cause of "no diagnostic output" was the exact same mistake as the
round-1 postmortem's lesson, made again: forgot to copy the freshly-built `radio.dylib` into
`root_dev/modules/` (only into the `.app` bundle's `Contents/Plugins/`), so the running process
was still executing a build from before the diagnostic even existed. Caught this time by
checking `strings root_dev/modules/radio.dylib` for the diagnostic's own log strings before
concluding anything from its absence of output, rather than trusting "I definitely deployed
everywhere" a second time.

With deployment actually confirmed, the diagnostic immediately showed the real bug: AM's
persisted `bandwidth` in this session's config was `15000.0` -- its IF-rate ceiling -- while
`passbandLo`/`passbandHi` correctly held a real ~8.3kHz trim (`-4336.7` to `+3947.4`). Every
build prior to round 4's fix persisted `bandwidth` from the live-dragged passband (see that
section above); a config that was ever saved by one of those earlier builds during this same
multi-round testing session ended up with a `bandwidth` value stuck wherever a drag last left
it, completely decoupled from the correctly-loaded passband. Since
`previewWidthFor(bandwidth)` used that stale value, and the *displayed* numeric Bandwidth
field used it too, this alone was enough to explain a persistently-wrong (though, per this
round's testing, not actively *changing*) zoom, independent of anything rounds 3-5 touched.

Fixed two ways: (1) `previewWidthHz` is now derived from `passbandHi - passbandLo` directly
(the value that's actually authoritative -- both the preview and the real filter use it) --
not from the separately-tracked `bandwidth`, which turned out to be corruptible; (2)
`selectDemod()` now also re-derives `bandwidth` itself from the loaded `passbandHi -
passbandLo` every time a mode is selected, self-healing exactly this kind of stale leftover
value from an earlier build without requiring a manual reset. Verified live: `root_dev`'s own
AM entry, still showing the corrupted `15000.0` moments before launch, read back
`8284.08` (`= 3947.37 - (-4336.71)`, exact) immediately after the mode was selected on
startup -- direct confirmation of the self-heal, not inferred.

LSB "pretty close to working" from this same report suggests the retuning fix (round 4) is
holding; this round's fix should address the still-wrong zoom in the other modes without
touching that.

Rebuilt and redeployed to all three locations, this time re-confirming via `strings` on the
deployed binaries specifically for the diagnostic's own marker text before trusting deployment
succeeded (see the detour above), not just a timestamp check. Not yet re-tested live by Ralph.

## Live-testing feedback round 7 (2026-08-10) -- round 6's own fix created this exact symptom

"Switching from USB to LSB and back to USB triggers the balloon representation." Round 6's
fix (base the preview width on `passbandHi - passbandLo`, the actual passband extent, instead
of the corruptible `bandwidth`) was correct for what it was fixing -- but has an obvious
problem in hindsight: the whole point of this feature is trimming the passband narrower to
dodge an adjacent signal, and a deliberately-narrowed passband is exactly what
`passbandHi - passbandLo` then reports back as "the width to zoom the context view to." Trim
USB down for a crowded band, leave, come back -- the preview now locks onto that same narrow
trim's width on every subsequent visit to the mode, reproducing the "balloon" symptom this
whole investigation has been chasing, except now caused by the *previous* fix rather than
anything from rounds 1-5.

Fixed by using `max(selectedDemod->getDefaultBandwidth(), passbandHi - passbandLo)` instead of
the passband extent alone. `getDefaultBandwidth()` is a fixed, mode-intrinsic constant --
never stale (it's not persisted, not user-editable, just a property of the demodulator class
itself), and never narrowed by the very feature that's supposed to work against a reasonably
wide backdrop. Keeps round 6's actual fix (immunity to a corrupted `bandwidth` value) while no
longer punishing the feature for being used as intended. Still grows past the default if a
trim somehow ends up wider than it, an edge case worth keeping exact rather than special-cased
away.

Rebuilt and redeployed to all three locations, all three confirmed by timestamp immediately
after the source build. Not yet re-tested live by Ralph.

## Live-testing feedback round 8 (2026-08-10) -- the "balloon" was never about frequency span

Ralph's report after round 7's fix ("same behavior") led to actually reproducing the exact
scenario programmatically this time, rather than reasoning about the code again: enabled
Rigctl Server's `autoStart` in the test config and drove a live, in-process USB -> LSB -> USB
sequence over its TCP interface (`M USB -1` / `M LSB -1` / `M USB -1`, the same
`RADIO_IFACE_CMD_SET_MODE` path both the GUI buttons and rigctl clients funnel through --
confirmed identical, not just assumed) while logging every value `selectDemod()` computes.
Result: numerically identical before and after the round trip -- `passbandLo`, `passbandHi`,
`previewWidthHz`, and the actual snapped `preview.getWidth()` all matched exactly. Round 7's
fix genuinely holds; the frequency axis was not moving.

Reported back to Ralph as "confirmed stable" -- and the reply arrived before that message even
finished sending: **"the numbers may not have changed, but the representation did."** That
single sentence retargeted the entire investigation. Every round from 3 through 7 had been
chasing the frequency span (`spanHz`, `previewWidthHz`, `passbandLo/Hi`) because "zooms in" is
what a frequency-axis bug sounds like -- but the actual defect was in the *vertical* (dB)
scale the whole time, something none of the numeric logging from rounds 3-7 would ever have
shown, because it isn't a number that gets computed once and printed -- it's `MiniSpectrum`'s
own internal smoothing state (`rangeMin`/`rangeMax` in `core/src/gui/widgets/mini_spectrum.h`,
added back in round 2 to fix a real, different problem -- see that section above).

`specWidget` is one persistent widget instance, living for the whole lifetime of the
`RadioModule`, not recreated per mode. Its dB auto-range smooths in slowly (a 0.2 IIR
coefficient per frame, so several frames to fully converge) from whatever it last was. Switch
from a mode with a strong, wide-dynamic-range signal (AM against a strong broadcast station,
say) to USB or LSB with a weaker or narrower one, and for the first several frames after the
switch the display is still scaled for the *previous* mode's signal levels -- compressing the
new mode's actual trace into a small fraction of the plot's height, which looks exactly like
what's been described throughout this investigation as a "balloon": a rounded hump sitting on
a flat, mostly-empty floor. The frequency span was never wrong; the vertical scale just hadn't
caught up yet, and evaluating the picture immediately after switching (the natural thing to
do) means seeing it mid-transition every time.

Fixed with `MiniSpectrum::resetRange()`, called from `selectDemod()` on every mode switch:
forces the very next `draw()` to set the range fresh from that frame's actual data instead of
smoothing in from the previous mode's. The smoothing itself stays exactly as it was for
*within* a mode (still useful there, still what round 2 added it for -- see that section);
this only stops it from carrying stale context across a mode switch, which is the one case it
was never supposed to smooth through in the first place.

Rebuilt and redeployed to all three locations, all three confirmed both by timestamp and by
grepping the deployed binaries for zero remaining diagnostic strings (this round's diagnostic,
used to drive and verify the rigctl round-trip test, has been fully removed). Not yet
re-tested live by Ralph.

## Live-testing feedback round 9 (2026-08-10) -- round 8's reset was too blunt an instrument

"Switching from a good USB display to AM mode brought the balloon." Round 8 correctly
diagnosed the mechanism (stale dB-range smoothing carried across a mode switch) but its fix
had a real gap: `resetRange()` made the *next* `draw()` call hard-set the range to exactly
whatever that one frame's data showed, no smoothing at all. That's fine if that frame is
representative -- but the preview's own FFT accumulation buffer doesn't flush the *previous*
tuned frequency's content instantly just because `RxVFO::setOffset()` itself is cheap and
immediate; there's a real, if brief, window where the very next frame or two can still hold
transitional data. Locking the range hard onto whichever frame happens to land in that window
is a narrower version of the exact bug being fixed -- a bad *single sample* instead of a bad
*history*, but still visibly wrong for a stretch until the (slow, 0.2-per-frame) steady-state
smoothing eventually drags it back.

Fixed by ramping the convergence rate down instead of switching abruptly between "instant" and
"slow": the first frame after a reset still sets the range directly (unavoidable -- there's no
prior data to blend from), but each frame after that uses `max(0.2, 1/(framesSinceReset+1))` as
its blend rate rather than jumping straight to the steady 0.2. That's ~100% on frame 1, 50% on
frame 2, 33% on frame 3, decaying to the normal rate within a handful of frames -- rides through
a couple of transitional frames without either camping on one of them or taking dozens of
frames to recover, the two failure modes rounds 2-8 traded back and forth between.

Rebuilt and redeployed to all three locations, confirmed by timestamp. Not yet re-tested live
by Ralph.

## Live-testing feedback round 10 (2026-08-10) -- not a smoothing bug at all: a pre-existing VFOManager gap

The key clue this round: "AM to SAM and back works. USB then back to AM triggers the
balloon." Both are mode switches; round 9's ramped-convergence fix should have applied
equally to both if the cause were purely about smoothing settle time. It didn't, which meant
the actual difference between the two cases had to matter -- and the one thing that
differs is reference type: AM and SAM are both `REF_CENTER`; USB is `REF_LOWER`.

Traced `VFOManager::VFO::setReference()` directly rather than theorizing further:

    void VFOManager::VFO::setReference(int ref) {
        wtfVFO->setReference(ref);
    }

`WaterfallVFO::setReference()` *does* correctly recompute `centerOffset` for the new
reference type internally (it calls its own `setOffset(generalOffset)`). But
`VFOManager::VFO::setReference()` stops there -- compare against `setOffset()` and
`setCenterOffset()`, both of which explicitly follow up with `dspVFO->setOffset(...)`.
`setReference()` never does. So switching between a `REF_CENTER` mode and a
`REF_LOWER`/`REF_UPPER` one changes what `centerOffset` *means* relative to the tuned
frequency (`centerOffset = generalOffset` for REF_CENTER; `generalOffset +- bandwidth/2` for
REF_LOWER/UPPER) -- correctly, in `wtfVFO`'s own bookkeeping -- but the *real* demodulation
VFO's actual tuned offset is left exactly where the previous mode's reference type last set
it, until something else happens to call `setOffset()`/`setCenterOffset()` again. AM/SAM never
exercises this at all (`REF_CENTER` to `REF_CENTER`, centerOffset never changes), which is
exactly why that round trip "just works" -- there was nothing to desync in the first place.

This is a real, pre-existing gap in `VFOManager` itself, not anything the spectrum preview
introduced -- the preview is just the first code in this tree that actually reads and
displays `centerOffset` directly, so it's the first thing to visibly show the mismatch. Fixed
in `RadioModule::selectDemod()`, immediately after `vfo->setReference(...)`:
`vfo->setCenterOffset(vfo->wtfVFO->centerOffset)` -- safe and idempotent, since
`setCenterOffset()` always treats its argument as the true center regardless of reference
type, so this doesn't change anything `wtfVFO` already computed; it only adds the
`dspVFO->setOffset()` call `setReference()` itself never makes.

Given `dspVFO`'s offset is also what the actual demodulated *audio* frequency comes from, this
may have been causing incorrect tuning after a REF_CENTER <-> REF_LOWER/UPPER mode switch
independent of anything visual -- worth Ralph specifically listening for correct frequency
after switching AM <-> USB/LSB now, not just checking the spectrum picture.

Rebuilt and redeployed to all three locations, confirmed by timestamp. Not yet re-tested live
by Ralph.

## Live-testing feedback round 11 (2026-08-10) -- the real balloon: preview VFO silently self-filtering after a resize

Round 10's VFO-reference fix was real (a genuine pre-existing tuning bug), but Ralph reported
it didn't fix the balloon: "AM to SAM and back works. Going to USB or DSB and then back to AM
brings the balloon. Whatever you're doing isn't fixing the issue." Also flagged that a
screenshot I'd judged "looks fine" actually showed the bug -- a smooth, low-detail dome instead
of the real spiky mediumwave spectrum, next to the correctly-spiky main waterfall right beside
it. Lesson: judge these against a live screenshot, not against the raw dB numbers alone.

Reproduced directly rather than theorizing further: launched fresh, confirmed a clean AM
selection renders a proper spiky trace, then drove `M AM -1` / `M DSB -1` / `M AM -1` over
rigctl and screenshotted immediately after and again 3s later. Confirmed: the trace collapses
into a smooth dome right after the round trip and **does not self-heal** -- still domed at 3s,
ruling out the transitional-FFT-buffer explanation round 9's ramped convergence was built for.

The real mechanism, in `RxVFO::setOutSamplerate()`:

    _outSamplerate = outSamplerate;
    _bandwidth = bandwidth;
    clampPassband();   // only clamps the *existing* passband against the new sample rate
    filterNeeded = passbandTrimmed() || (_bandwidth != _outSamplerate);

`clampPassband()` only pulls the passband *in* if it now exceeds the new (possibly narrower)
sample rate -- it deliberately never pushes `_passbandLo`/`_passbandHi` back *out* to match a
*wider* new bandwidth, per its own comment: every real caller follows `setOutSamplerate()` with
an explicit `setPassband()` (via `applyPassbandEdges()`) to establish what the passband should
actually be now, so re-deriving a fresh symmetric one here too would just be redundant.

`RadioSpectrumPreview` is the one caller that breaks that assumption. It calls
`dspVFO->setOutSamplerate(_width, _width)` on every resize and never calls `setPassband()` at
all -- it has no passband concept of its own, it only ever wants the full raw width, unfiltered.
So whenever a resize makes `_width` *wider* than whatever passband was in effect before
(including the very first real resize ever, since `init()` starts from a placeholder 40kHz that
gets resized the moment the actual mode's preview width is computed), the old, now-too-narrow
passband silently stayed in effect. `passbandTrimmed()` has no way to distinguish "genuinely
narrower than intended" from "just hasn't been widened back out yet" -- so `filterNeeded` came
back true, and the preview started quietly bandpass-filtering what was supposed to be raw,
unfiltered context. A filter's own passband response, plotted as if it were signal, is exactly
a smooth dome -- and since nothing else was ever going to call `setPassband()` on this VFO
again, it never recovered on its own.

Explains the exact AM/SAM-vs-USB/DSB split: AM and SAM share the same default bandwidth
(10000Hz), so they compute the identical snapped preview width every time -- `setWidth()`'s own
`if (snapped == _width) return;` no-op skips the whole resize path, so nothing ever gets the
chance to go stale between them. USB (2800Hz default) and DSB (4600Hz default) both differ from
AM's width, so switching to either and back always exercises the resize path, and always hits
this bug.

Fix, in `RadioSpectrumPreview::setWidth()`: after `dspVFO->setOutSamplerate(...)`, explicitly
call `dspVFO->setPassband(-_width / 2.0, _width / 2.0)` to force the passband back to the full
new width every time -- the one thing this VFO always wants regardless of what it last had.

Verified visually, not just by log numbers: rebuilt, redeployed to `root_dev/modules/` only
first (fast iteration), confirmed via rigctl-driven AM/DSB/AM round trip + screenshots that the
trace stayed properly spiky immediately after the switch and 3s later -- then ran a 6-switch
stress sequence (LSB -> USB -> SAM -> DSB) and confirmed it stayed clean throughout. Temporary
`fprintf`-to-`/tmp` diagnostics added for this round's investigation were removed before the
final build. Rebuilt clean (both `build` and `build_release`) and redeployed to all three
locations (`SDR++.app`, `SDR++ RB.app`, `root_dev/modules/radio.dylib`), relaunched, confirmed
running without crashing.
