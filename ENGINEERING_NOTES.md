# Engineering notes: antenna phasing and the RSR200 driver

A retrospective on the session that produced `PHASING_PLAN.md`, `RSR200_PLAN.md` and the
twenty commits on `antennaPhasing`. Written for the next person to touch this — possibly
the same person, months later.

This is not a transcript. It reconstructs the decisions, the mistakes, and what caught
them, which is the part worth keeping.

---

## 1. The central bet, and whether it paid

Halfway through planning, the Fobos SDR turned out to be 1,500 km away and unavailable
indefinitely. The plan had been written with hardware verification as **Phase 0** — a gate
reading "nothing else starts until this passes."

That was restructured so the Fobos work collapsed into a single late phase, and everything
before it ran against a **synthetic two-channel source with known-correct answers**.

**This was the highest-leverage decision in the session.** Six phases were built and
verified without a radio. More importantly, the synthetic source is what caught almost
every real bug, because it can do something no radio can: state the correct answer in
advance.

> On the air you can null an interferer, but you never learn what the right weight *was*.
> A converged adaptive algorithm and a lucky one look identical.

The test source prints "Interferer nulls at: +3.00 dB, −137.00 deg". When auto-null
converged to `+3.000 / −137.000`, that was proof, not encouragement.

**The pattern generalises:** when a feature's correctness is hard to observe, build the
instrument that makes it observable *before* building the feature. The cost was one module;
the return was five bugs that would otherwise have surfaced as "the nulling seems flaky."

**The bet paid off on 2026-07-29.** With two real antennas on an RSPduo, auto-null and
decorrelation both pulled stations out from under strong locals in daylight — the hard
case for medium wave. Nothing had to be retuned or re-derived to make that work: the
behaviour on air was the behaviour the synthetic source predicted. Six phases were built
against a signal generator and worked the first time they met an antenna, which is the
strongest available evidence that the ground truth was the right thing to build first.

A second sequencing decision followed the same logic: dual-channel **recording** was moved
*ahead* of the UI, so that one capture of a real interferer becomes permanent regression
material rather than each test depending on whatever is on the air that evening.

---

## 2. Bugs, and what actually caught them

Ordered by how instructive they are, not by when they happened.

### 2.1 The inverted interpolator fraction (worst)

**Symptom:** user reported "changing the delay causes the sound to break up."

**Cause:** the fractional delay split `d` into `floor(d)` and a fraction `f`, then
interpolated *forward* by `f` — landing at `n − floor(d) + f`. That is an effective delay
of `floor(d) − f`, not `floor(d) + f`. Two consequences: the delay was wrong by up to a
full sample *at rest*, and every time a sweep crossed a whole sample the fraction wrapped
from 0.99 to 0 and the read position jumped nearly two samples.

**Why the tests missed it:** the existing delay test used delays of 0.0 and 1.0 — both
whole samples, which take the `memcpy` fast path and never touch the interpolator at all.
**The tested branch was the one without the bug.**

**Lesson:** when a function has a fast path and a slow path, a test that only exercises the
fast path is worse than no test, because it produces false confidence. Now covered by a
fractional-delay check (lands within 0.01 dB of theory) and a sweep across whole samples
asserting no splice.

**This exact mistake recurred in Phase 6**, which is why it leads the list. Half the Fobos
open attempts were failing; an open/close loop showed a clean ok/FAILED/ok/FAILED
alternation, so a bare retry was written, committed, and *then* tested — and it did not
work, because two back-to-back opens both fail. The alternation was a symptom, not the
mechanism. What the device actually needs is about half a second of wall time after a
streaming session; probing every 500 ms it returns on the second attempt, ~0.8 s after the
close, every time. The corrected fix was verified before the commit stood, by giving the
measurement harness the same strategy and watching 8 of 8 cycles succeed where 4 of 8 had
failed. **A reproducible pattern is not the same as an explanation**, and the cost of
finding that out is the same whether it is a delay line or a USB driver.

**Second lesson, about process:** the first fix shipped for this was *ramping the delay*,
aimed at a plausible cause that had not been confirmed. The ramping is correct and worth
having — a step change in delay is a step change in time — but it was not the reported bug.
Only after building a measurement harness did the real cause appear. **Measure before
claiming a diagnosis.** The user's "that's minor" nearly caused this to be deprioritised;
it was a control silently wrong by up to a sample whenever it was used at all.

### 2.2 The read-lockstep deadlock

**Cause:** `Phaser::run()` read one buffer from each input per pass. That requires the two
inputs to deliver the same *number of reads*, not the same number of samples. Fed 40×3000
on A and 25×4800 on B, B ran dry while A still had 15 blocks queued, and the block waited
forever on the exhausted side.

**Why it is interesting:** the plan already warned about `dsp::Operator` discarding
mismatched inputs, and that warning was heeded — accumulation buffers were added. The
warning was simply not *deep enough*. Avoiding `Operator` was necessary but not sufficient.

The fix reads only the channel that is behind, which also bounds the buffers and preserves
backpressure. Later extracted to `ChannelSync` and shared with the recorder.

**Lesson:** a documented trap tells you where to look, not how far down.

### 2.3 The metric that punished the correct answer

**Cause:** null depth was `10·log10(mean|A|² / mean|Y|²)` across the whole band — it
measures "did total power drop", which is not "did the pest go away". A weight that cancels
an interferer exactly is free to *amplify* everything else.

**Observed:** a reference-band solution that nulled the interferer perfectly scored
**3.4 dB**, while a wideband compromise that nulled nothing properly scored **4.2 dB**.

**Why it matters more than a normal bug:** this was the number the user had been told to
optimise. It would have quietly misdirected every attempt to use the feature, and nothing
about it looks wrong. Null depth is now measured *inside the reference band* when one is
set, and the UI labels which it is showing.

**Lesson:** a metric is part of the interface. Getting it subtly wrong is worse than having
none, because it is trusted.

### 2.4 A test signal that could not test the feature

**Symptom:** the wideband multi-tap weight scored *worse* than the scalar it was meant to
beat, and identically for every delay.

**Cause:** the test used two pure tones. With tonal input, `Sbb(f)` is essentially zero away
from the two bins, so `W(f) = Sab/Sbb` is undefined across almost the whole band and its
impulse response is not concentrated in time. Truncating to N taps discards nearly all of
it. **Nothing was wrong with the solver.**

A wideband canceller can only be demonstrated with a *broadband* interferer reaching both
channels with a timing difference — which the synthetic source could not produce, because
that capability had been flagged as a Phase 5 prerequisite back in Phase 0 and deferred.

Once added: a 3.7-sample skew leaves a scalar recovering **nothing** while 64 taps hold the
full null — a 21 dB difference.

**Lesson:** "the feature underperforms" and "the test cannot express the feature" look
identical from the failure output. When a result contradicts theory, suspect the harness
before the implementation.

### 2.5 The double A/B swap

`toneWeights()` folded the channel swap into the weights, and `generate()` swapped the
samples *again*. Double swap cancels, so with swap enabled the reported null weight silently
disagreed with the signal actually generated.

Caught within minutes by the first test run — and it is precisely the bug that would later
have looked like an adaptive-algorithm failure.

### 2.6 The double-applied alignment delay

The wideband solver was fed the already-delayed channel A, so its impulse response already
sat at the right lag, and `solve()` shifted it a second time. Found by the numbers being
wrong in a way that scaled with tap count.

### 2.6a The reference band that was written into the design and left out of the UI

**Symptom, reported from real use:** on WNYC 820, the Perseus22 fully nulls the local and
reveals a distant station underneath; the same station on the Fobos with this software left
WNYC still strongly audible, with the distant signal a bare echo. Separately, "Decorrelate:
null strongest" sometimes left a station *louder* than "Decorrelate: peak strongest" did —
backward from what either name promises.

**Where the check started, and where it didn't stop.** The obvious first move was the
eigendecomposition itself — `solveEigen2`, `combineCoefficients`, `inverseSqrt` — and it
checked out completely: correct closed-form 2×2 Hermitian solution, unit-norm orthogonal
eigenvectors, and `test_decorrelation.cpp` already proves it lifts a station 26 dB under a
local to 43 dB above it. A wrong finding here would have been the easy, wrong stopping
point: "confirmed, the math is right," case closed, symptom unexplained. The actual fault
was one layer up — not *how* the covariance is decomposed, but *what band it is measured
over* — and `PHASING_PLAN.md` §2.6 had already specified the answer: "the covariance is
estimated over the reference band." The implementation had quietly stopped doing that.

**The bug:** `misc_modules/phasing` put the "Reference band" checkbox inside
`if (combining && !decorrelating)` — the Adaptation section. Decorrelation is drawn from a
separate block that never included it. `refEnabled` defaults to false. So decorrelation had
no reachable path to anything but `RefBand::accumulateWideband()` — the covariance measured
over the *entire tuned span*, not the one carrier the operator is trying to null. On a
receiver viewing a slice of the MW band, that is dozens of other stations folding into a
single number.

**Why that produces exactly the reported symptoms, and not some other kind of wrongness:**
with several coherent arrivals in view, "the dominant one" is whichever single station is
loudest across the *whole* span — not necessarily, and often not, the one on the dial. Null
the dominant arrival and you may null a station you never heard; the one you were listening
to is barely touched, because it was never what the eigenvector represented. Which of Null
or Peak sounds louder on any given station becomes a matter of how that station's own
gain/phase ratio happens to project onto whichever direction the *actually*-dominant signal
defined — which is exactly the kind of thing that can look "backward" for one station and
correct for another, with no code error anywhere in sight.

**How this got settled rather than argued:** reproduced with the project's own
`RefBand`/decorrelator code, not a re-derivation, on a three-signal scene — a station on the
dial, a weak one buried under it, and a louder unrelated one elsewhere in the span:

```
wideband:              L via Null  -0.1 dB   L via Peak  -2.9 dB   (X via Null -22.3 dB)
4 kHz band centred on L:  L via Null -21.6 dB   L via Peak  +1.7 dB
```

Wideband nulls X and leaves L *louder* under Null than Peak — the reported backward result,
reproduced on demand rather than described from memory. Narrowing the band fixes both.
Kept as `core/test/test_crowded_band.cpp` so this cannot silently regress.

**The lesson, stated plainly:** correct math wired to the wrong input is indistinguishable,
from the output alone, to a user, from wrong math — and it is tempting to stop checking the
moment the equations turn out fine. The plan document already contained the fix; nobody had
compared the shipped UI against what §2.6 said the UI was supposed to let the operator do.
When a feature has a design note describing how it's meant to be used, checking the
implementation *against that note* is a distinct step from checking that the algorithm is
implemented correctly, and skipping it is how a component can be simultaneously
well-tested and unusable.

### 2.6b A promising synthetic result that did not survive real air, and the control that caught it

**The setup:** asked to try wideband multi-tap decorrelation, on the reasoning that §2.5's
frequency-flat-ratio limitation applies to decorrelation too and a per-bin solve should
subsume it the same way the Wiener multi-tap solver already does for auto-null. Built,
tested synthetically first (the discipline this project has followed throughout), and the
synthetic result was unambiguous: two stations nulled simultaneously, no reference band
needed, deeper than even the scoped scalar result. A clean, decisive win.

**Then it was run against real air**, the WNYC recording from §2.6a, in short windows so a
result could be had in seconds rather than minutes. The first two windows (0–30 s, 0–60 s)
matched or modestly beat the scalar. A window starting at minute 6.3 gave 16.2 dB, well
below the scalar's 23 dB ceiling from the same recording.

**The comfortable explanation arrived immediately, and was wrong.** Early evening is
exactly when medium-wave skywave starts developing, so "a co-channel station is fading in
and degrading the null" was the obvious story, and it would have been easy to write down
and move on — it fit, it required no further work, and it did not implicate the new code.
**It was checked rather than accepted**, by running the *scalar* method over the identical
30-second window. If propagation were the cause, both methods see the same air and both
should degrade. The scalar gave 22.2 dB — stable, not degraded. Whatever went wrong in
that window belongs to the per-bin solver specifically, not to conditions outside it.

**What this is really about:** an explanation that requires no further investigation is
not thereby more likely to be correct, and "the physics would predict this" is exactly the
kind of explanation that stops a check before it starts, because it is plausible and
external to the code just written. The fix here was not clever — reuse the working scalar
implementation as a control on the same data — but reaching for it instead of the tidier
propagation story is the entire difference between an honest result and a wrong one shipped
with a good story attached. The root cause of the per-bin instability is still not
diagnosed; what is settled is that it is real, and that finding out cost one extra
five-line program rather than a false confirmation.

**Shipped anyway, labelled honestly.** The multi-station capability the synthetic test
proved is real and does not depend on whatever is unstable about single-station depth. The
UI says outright that single-station use is experimental and may underperform a well-scoped
reference band, rather than presenting a feature whose one real-world measurement was a
clean win when the actual record is mixed.

**The next request tested the leading hypothesis, and it failed the same way the propagation
story did.** The candidate explanation was a coarse-filter problem: 64 taps resolves to about
125 kHz, far blunter than the 488 Hz bins the covariance was actually solved at, so a bin's
fine solution could not be realized. That predicts more taps should help, roughly
monotonically. Swept 16 through 256 at the exact window that had measured 16.2 dB:
12.0, 27.6, 16.2, 18.0, 6.6 dB. No trend either direction, and the one tap count that briefly
looked promising (32, giving 27.6 dB on this window) failed on three further independent
windows at the same setting — 23.1, 34.5, 16.2, 15.2 dB, no more stable than 64 had been.

**The value of this run was the negative result, and it needed the same discipline as the
propagation check to get to.** It would have been easy to report "32 taps improved it" from
the first data point and stop — a single measurement that confirms a hypothesis is exactly as
tempting to accept without a second look as a single measurement that fits a plausible outside
cause. Testing the SAME setting against independent windows before believing it is the same
move as running the scalar control: check whether an apparent fix generalizes before crediting
it, not only whether an apparent cause does. What the sweep actually shows is a per-bin
estimate that swings by more than 20 dB depending on which 30 seconds of real air it is asked
to describe, largely independent of how many taps the result is truncated to — which points
away from a filter-resolution problem and toward the covariance estimate itself being fragile
on real noise, an open question rather than a closed one.

### 2.6c The instability, found by watching a bin instead of sweeping more constants

**Told to stop sweeping scalar knobs and go look directly at what was moving.** Two
substantial constants had already been ruled out by measurement — taps, then the
forgetting factor — and both rulings were negative results with no next lever obviously
implied. The next step was qualitatively different: log one FFT bin's covariance and
solved weight over time within a single window, and see with actual numbers whether it
oscillates, drifts, or jumps, rather than guessing at a third parameter to sweep.

**The answer was not what the hypothesis-in-progress expected, and that mattered.** The
carrier bin — the strongest, cleanest signal available, the one most likely to misbehave
if anything about the per-bin approach were fundamentally unsound — turned out to be
almost perfectly stable: coherence above 0.999, solved weight settled within 2 ms and
barely moving for the full five seconds logged. If the instability lived in individual
bins failing to converge, the carrier bin should have shown it, and it did not. That result
by itself killed the leading mental model (fragile per-bin estimates generally) and forced
a different question: not "which bin is wrong" but "what happens to bins that have nothing
to say."

**The mechanism, once looked at directly, was simple enough to have been guessed sooner —
but guessing it would have been exactly the kind of unchecked plausible story that had
already misled twice.** Every one of 16384 bins gets solved and IFFT'd into the same
64 taps, including thousands with no real signal at all. A noise-only bin's
eigendecomposition does not return "nothing" — noise still has SOME momentary, randomly
varying coherence from block to block, so the solver returns an arbitrary direction that
fits that block's particular noise. Thousands of arbitrary directions, all folded into the
same handful of realized taps via the inverse FFT, is a plausible way for a result to
depend on exactly which 30 seconds of air happened to be sampled — which is precisely the
symptom on record.

**Verified by intervention, not just by a story that fit.** Re-solved the same window with
everything outside a shrinking distance from WNYC forced to pass-through: 15.9 dB with
nothing excluded, climbing monotonically to 35.5 dB at a 2 kHz window, 45.7 dB at 500 Hz.
The 500 Hz result was distrusted on sight — three bins fitting one specific window
suspiciously well is the shape of overfitting, not of a real fix — and the 2 kHz setting
was checked against four more independent windows before being credited, the same
discipline as every other claim in this section: 35.2 to 35.7 dB, tighter than the scalar
method's own spread on the same windows.

**Caught the fix's own naive form before committing to it.** A frequency window is a
reference band wearing a different name, and this feature's entire point was not needing
one — a gate centred on WNYC would have silently broken the multi-station win the moment it
shipped, the same way the reference-band bug two sections earlier broke decorrelation by
having a real capability sitting behind an unreachable switch. Recognising that before
writing the production version, rather than after someone noticed the regression, is what
the frequency-vs-power distinction in section 2.6c is actually about. A power threshold —
excluded a bin if it sits far enough below the median bin's power, regardless of where it
sits — was checked on the same two-station synthetic scene that proved the original win,
confirmed both stations' bins still passed the gate, and only then implemented as the real
default.

**What made this investigation work, in order:** a concrete diagnostic instead of another
parameter sweep; a result that contradicted the working hypothesis, taken seriously instead
of explained away; a mechanism simple enough to verify by direct intervention rather than
just narrated; the fix's most obvious form checked against the exact case it would have
broken, before shipping it as the default rather than after.

### 2.7 Smaller ones

- **Phase clamp before scaling.** Clamping to 179.99° before scaling to a 16-bit value
  lands one LSB short of `0x7FFF`. The clamp belongs *after* scaling.
- **Missing `#include <complex>`** in `rsr200_protocol.h`, invisible because the test
  happened to include it first. Header self-sufficiency is not optional.

---

## 3. Where the reasoning went wrong

Worth recording separately, because these were not coding errors.

**Built a second explanation on top of a correct one.** When the log showed no sign of a
4-channel recording, the correct cause (block buffering, already identified and stated) was
set aside in favour of an invented one ("the file source doesn't load until you re-pick the
file"). The file *did* auto-load. **Having found the right explanation, stop.**

**Told the user to watch for something that was never logged.** The recorder printed nothing
about channel count, and the instruction said to watch the log. That is not a documentation
slip — starting a 480 MB/min capture silently is a real omission, now fixed.

**Assumed instead of reading, twice.**
- Assumed the Perseus22 used `auxi` RIFF chunks. `.p22` is not RIFF at all — proprietary,
  magic `P22REC013`. The whole "follow their chunk" plan did not exist.
- Assumed decorrelation was a whole-spectrum per-bin operation. The manual specifies a
  narrowband estimate with a 1–2 kHz bandwidth for AM.

Both were caught by reading the source material. Both would have produced confidently wrong
implementations.

**Test assertions wrong more often than code.** Expected −37 dB where −40 was right;
asserted a null depth above 30 dB when the ceiling was 20.04 by construction; measured a
ramp-in transient instead of steady state; measured a delay line's startup fill. Every one
was the test, not the implementation. **When a new test fails, suspect the test first.**

---

## 4. Environment traps that cost real time

**A stale library shadowing the real one.** `build/libsdrpp_core.dylib` was a leftover next
to the true output in `build/core/`. Test binaries linked the stale copy and segfaulted with
"mutex lock failed" — in code that was fine. The app was never affected, because its rpath
pointed at the right one.

This bit three times before being fixed properly, which is what produced
**`core/test/run_tests.sh`**: it rebuilds core, then recompiles *and* runs every suite, so a
test binary can never be older than the library it links.

**CMake `GLOB` is evaluated at configure time.** A new `.cpp` will not build until `cmake`
is re-run, and the failure is an undefined-symbol link error that looks like a code problem.

**Redirected output is block-buffered.** Watching a log for events that arrive seconds apart
shows nothing, which reads as "the feature didn't fire". Relaunching under
`script -q /dev/null` forces a pty and line buffering. This directly caused the misdiagnosis
in §3.

**Vendor libraries with bare install names.** Both the SDRplay API (`libsdrplay_api.so.3`)
and FTDI D3XX (`libftd3xx.dylib`) record install names that resolve only via dyld's
*fallback* search path. Link by absolute path and rewrite the dependency with
`install_name_tool -change`.

---

## 5. Design decisions worth remembering

**Bypass has to be a mode, not a rewiring.** The plan originally said phasing could be
switched out of the signal path. It cannot: with a `ChannelSet` registered the source is
writing to its channel streams, and an unread `dsp::stream` blocks its writer on the second
swap — "unwiring" would stall the source outright. Bypass is `MODE_A_ONLY`, which is
bit-identical to what the source used to emit, and asserted as such.

**Mode and Monitor are one control.** The plan listed them separately. With a single output
stream, choosing what the combiner does *is* choosing what you hear. Two controls would
imply an independence that does not exist.

**Sliders cannot land on an exact figure.** Dragging to "137 degrees" gives 136.98 or
137.02, and a phasing null is sharp enough that the meter shows it. Coarse and fine sliders
for hunting, numeric fields for landing — both are needed, neither alone is usable. This
came from user feedback, not from the plan.

**Framing belongs to the transport.** For the RSR200, framing *is* the difference between
the interfaces: USB reads whole packets from an endpoint, TCP resynchronises a byte stream,
UDP reassembles indexed fragments. Above that line they are all "a frame arrived", and one
device drives either.

**Anything that moves while audio flows must be ramped.** Learned for the combining weight
in Phase 1, then *not* applied to the delay added in Phase 3 — treated as a property of the
weight rather than a general rule. It is a general rule.

**Metadata belongs inside the file.** Recordings kept for years outlive their sidecars. The
`auxi` chunk layout was decoded from a real HDSDR file and validated against that file's own
name, rather than recalled.

**Retries need a fresh identifier.** The RSR200 documents a repeat counter for exactly this,
then says firmware 22x ignores it and simply executes the command again. A retry must go out
under a new command number and tolerate the original having landed.

---

## 6. What the plans got right

Both plan documents were revised as work landed, and the revisions are as valuable as the
plans. Four corrections are recorded in `PHASING_PLAN.md` itself:

1. The `Operator` trap was real but under-stated (§1.4).
2. Bypass cannot be a rewiring (§2.1).
3. Null depth has to be measured where the null is (§2.4).
4. A wideband canceller cannot be validated with tones (§2.5).

**Writing the plan first was worth it**, and not because it was followed. It was worth it
because each deviation was forced to be *justified in writing*, which is what turned four
mistakes into four documented findings instead of four silent code changes.

The plan also correctly identified, before any code existed, that the Fobos already ships
both HF channels and the driver throws one away — which is the fact the entire feature rests
on.

---

## 7. If picking this up cold

1. Read `PHASING_PLAN.md` §1 and §2, then `RSR200_PLAN.md` §1–§4.
2. Run `core/test/run_tests.sh`. Eight suites; all must pass.
3. Build, then launch with `--root root_dev`, under `script -q /dev/null` if you need to
   watch the log live.
4. The **Phasing Test** source generates everything needed to exercise every feature, and
   prints the correct answer for each. Use it before reaching for a radio.
5. Hardware state and its effect on sequencing is not in the repo — see the project memory.

**The single most useful habit from this session:** when something looks wrong, build the
smallest possible thing that measures it, rather than reasoning toward a plausible cause.
The delay bug, the metric bug, and the wideband test-signal problem were all diagnosed that
way after at least one wrong guess apiece.
