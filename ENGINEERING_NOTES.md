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
