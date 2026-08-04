# Plan: Generic Antenna Phasing / Diversity Combining for SDR++

**Goal:** Let a user connect two antennas to a receiver that provides two coherent
receive channels, and interactively combine those channels (gain + phase, manual or
adaptive) to null an interfering signal or peak a wanted one — the classic "phasing"
technique DXers use with an MFJ-1026, a Perseus22, or a Reuter RSR200.

**Non-goal:** Making this a Fobos-only feature. The Fobos SDR is the first *adapter*;
the mechanism lives in core so the SDRplay RSPduo, Microtelecom Perseus22, Reuter
RSR200, and any future multi-channel radio can opt in with a small per-driver change.

---

## 1. What the codebase gives us today

### 1.1 The Fobos already ships both HF channels — we throw one away

`source_modules/fobossdr_source/src/main.cpp:476-507`. In direct-sampling (HF) mode,
`fobos_rx_read_sync()` fills a buffer of `dsp::complex_t` where the two components are
*not* I and Q of one channel — they are two independent real ADC channels:

| Component | Channel |
|---|---|
| `.re`     | HF1 |
| `.im`     | HF2 |

The worker's `PORT_HF1` branch zeroes `.im`; the `PORT_HF2` branch zeroes `.re`. Both
branches then push the (now real-valued) stream into a single `dsp::channel::RxVFO`
with `setOffset(freq)` — the DDC's complex mixer plus low-pass does double duty as the
Hilbert/analytic converter, which is why a real input works and the image is rejected.

**This is the whole ballgame.** The two channels are already sample-aligned in one
buffer, from one ADC clock, delivered over one USB transfer. There is no
synchronization problem to solve, only a combining stage to add. The nulling code is
literally replacing two `= 0.0f` statements with a second DDC and a combiner.

### 1.2 The signal path is single-stream from source to frontend

- `SourceManager::SourceHandler` (`core/src/signal_path/source.h:13-22`) carries exactly
  one `dsp::stream<dsp::complex_t>* stream`.
- `SourceManager::selectSource()` (`source.cpp:53-58`) hands that stream straight to
  `sigpath::iqFrontEnd.setInput()` — or to `server::setInput()` in headless mode.
- `IQFrontEnd` (`iq_frontend.h`) then does buffering → decimation → DC block → conjugate
  → splitter → FFT + VFOs.

So a combiner must sit **upstream of `IQFrontEnd`**, at the point `selectSource()` wires
things up. That placement is also what we want for usability: the waterfall, every VFO,
the recorder, and the server all see the phased result.

### 1.3 The DSP framework already has the right shapes

- `dsp::Operator<A,B,O>` (`core/src/dsp/operator.h`) is the two-input block base class;
  `math::Add` and `math::Multiply` are working examples.
- `dsp::chain<T>` supports runtime enable/disable of a block with output rewiring —
  exactly the "bypass when phasing is off" behavior we need.
- volk is available (`volk_32fc_x2_multiply_32fc`, conjugate dot products), and fftw3 is
  already a core dependency (used for the waterfall FFT), so a frequency-domain adaptive
  weight later on costs no new dependency.
- `SmGui` (`core/src/gui/smgui.h`) provides `SliderFloat`, `Checkbox`, `Combo`, `Button`
  — enough for the whole control surface, and it is the *serializable* widget set that
  survives the SDR++ server protocol.

### 1.4 Traps found while reading

**(a) `Operator::run()` drops data on count mismatch.** `math::Add::run()` reads both
inputs and, if `a_count != b_count`, flushes both and returns 0 — silently discarding
samples. `dsp::stream::flush()` is all-or-nothing (`core/src/dsp/stream.h`); there is no
partial consumption. A combiner built naively on `Operator` will glitch whenever the two
upstream resamplers emit different block sizes. **The phaser must own internal
accumulation ring buffers per input and consume `min(availA, availB)` per pass.** This
is the single most likely source of "it works but there's a tick every few seconds" bugs.

**(b) `SourceHandler` is ABI-fragile.** Every source module allocates a `SourceHandler`
as a member and hands core a pointer. Appending fields to the struct means core reads
past the end of structs allocated by modules compiled against the old header. Do **not**
extend `SourceHandler` — use a side registry keyed by source name (§2.1).

**(c) The SDRplay module is hardcoded to single-tuner.** *(Fixed in Phase 7.)*
`source_modules/sdrplay_source/src/main.cpp:233,502` set
`sdrplay_api_RspDuoMode_Single_Tuner` unconditionally, and `rspDuoSelectAntennaPort()`
(line 462) *swaps* the active tuner rather than running both. RSPduo support is a real
driver change, not a config toggle.

**(d) `source_modules/fobos_source/` is an empty directory.** The live module is
`fobossdr_source`. Ignore the empty one (or delete it).

---

## 2. Design

Three layers, each independently useful:

```
  ┌─ radio driver ────────────┐   ┌─ core ─────────────────┐   ┌─ UI ──────────┐
  │ fobossdr_source           │   │ ChannelSourceRegistry  │   │ phasing       │
  │  deinterleave + 2× DDC ──►│──►│ dsp::combine::Phaser ──│──►│ misc_module   │
  │ sdrplay_source (RSPduo)   │   │        │               │   │ (controls +   │
  │ perseus_source (P22)      │   │        ▼               │   │  null meter)  │
  │ rsr200_source             │   │   IQFrontEnd           │   └───────────────┘
  └───────────────────────────┘   └────────────────────────┘
```

### 2.1 Layer 1 — a multi-channel source contract (core)

New, additive, ABI-safe. In `core/src/signal_path/source.h`:

```
struct ChannelSet {                     // registered alongside a SourceHandler
    int count;                          // number of coherent channels (>= 2)
    std::vector<dsp::stream<dsp::complex_t>*> streams;
    std::vector<std::string> names;     // "HF1", "HF2" / "Tuner A", "Tuner B" / "ANT1", "ANT2"
    bool phaseCoherent;                 // relative phase stable across restarts?
    bool sampleAligned;                 // hardware guarantees sample alignment?
};

void SourceManager::registerChannels(const std::string& sourceName, ChannelSet* set);
void SourceManager::unregisterChannels(const std::string& sourceName);
ChannelSet* SourceManager::getChannels(const std::string& sourceName);
Event<std::string> onChannelsRegistered;
Event<std::string> onChannelsUnregistered;
```

Sources that never call `registerChannels` behave exactly as today. `selectSource()`
gains one branch: if the selected source has a `ChannelSet`, wire its streams through the
phaser and pass the phaser's output to the frontend; otherwise pass `handler->stream` as
before.

**Correction made during Phase 1:** this section originally said the branch should also
test whether the phaser is *enabled*, with phasing switched out of the path entirely when
off. That does not work. With a `ChannelSet` registered the source is writing to its
channel streams, and an unread `dsp::stream` blocks its writer on the second swap — so
"unwiring" the phaser would stall the source's worker outright. Bypass is therefore a
*mode*, `Phaser::MODE_A_ONLY`, in which the output is a straight copy of channel A. That
is bit-identical to the source having emitted that channel directly, which is exactly
what it did before, and it is verified as such in `core/test/test_phaser.cpp`.

Design for N channels in the struct even though we only combine 2 at first — a 4-port
receiver is a plausible future and the registry shouldn't need re-cutting.

Note the semantic split: `phaseCoherent == false` (e.g. two separately-locked LOs whose
offset is stable *within* a run but random *across* runs) is still perfectly usable —
a constant offset is absorbed into the weight. It only means saved weights can't be
restored blindly, which the UI should reflect by re-running auto-null on start.

**Measured on the RSPduo, and it is exactly that case.** With a strong medium wave carrier
reaching both ports: coherence `1.0000`, and the cross-correlation phase drifts under
0.15° across a run — so a null holds once set. But across a stop and restart the phase
lands anywhere in ±180°; six consecutive runs gave +91, +148, +111, −73, −143, +90 degrees.
The two tuners share a clock, but their local oscillators come up at an arbitrary relative
phase.

The practical consequence is worth stating plainly, because it shapes what memories are
for: on a source with `phaseCoherent == false`, a stored **gain and phase is not
restorable** — recalling a memory usefully returns the frequency, mode and channel pair,
but the weight itself must be re-found. Auto-null or decorrelation does that in a second or
two, so the workflow is "recall, then re-converge" rather than "recall and listen". An
obvious future refinement is for the UI to start adaptation automatically after recalling a
memory on such a source.

### 2.2 Layer 2 — the combiner block (core DSP)

New `core/src/dsp/combine/phaser.h`. Output:

```
  Y[n] = A[n] − w · B[n]
```

`w` is a complex weight. In polar terms that's exactly the two knobs a phasing box
gives you: **gain** (|w|, in dB) and **phase** (arg w, in degrees). `w = 0` gives
channel A alone; `w = −1` gives the sum; `w = +1` gives the difference.

Modes:

| Mode | Behavior |
|---|---|
| `A_ONLY` / `B_ONLY` | pass-through, replaces the existing HF1/HF2 port selection |
| `MANUAL` | user sets gain + phase directly |
| `AUTO_NULL` | adapt `w` to minimize output power (see §2.4) |
| `HOLD` | freeze the adapted `w`, stop updating |

Structural requirements:
- **Own ring buffers**, per trap (a) above. Read whatever each input offers, accumulate,
  process `min(availA, availB)`, retain the remainder.
- **Smooth weight changes.** Snapping `w` between blocks produces an audible click.
  Interpolate `w` linearly across each processing block (or one-pole toward the target).
- **Report metrics** for the UI, computed per block and published atomically:
  mean |A|², mean |B|², mean |Y|², and derived null depth `10·log10(⟨|A|²⟩ / ⟨|Y|²⟩)`.
- **Alignment control**: integer sample delay + fractional delay (short Farrow or
  windowed-sinc interpolator) on channel B, for feedline-length mismatch. A pure
  scalar `w` cannot fix a delay difference — it shows up as a frequency-dependent
  phase slope, i.e. a null that only works at one spot in the passband.

### 2.3 Layer 3 — the UI (`misc_modules/phasing/`)

A misc module registering a menu entry via `gui::menu.registerEntry`, following the
`recorder` module's pattern (`misc_modules/recorder/src/main.cpp:119`). It appears
greyed-out with an explanatory line when the selected source has no `ChannelSet`.

Controls, ordered by what a DXer actually reaches for:

- **Enable** (master bypass)
- **Mode** combo: A only / B only / Manual / Auto-null / Hold
- **Phase** −180…+180° — coarse slider plus a fine vernier (±5° around the coarse value);
  phasing nulls are sharp, and a single 360°-wide slider is unusable at the null
- **Gain** −40…+40 dB, likewise coarse + fine
- **Swap A/B** button — which antenna is the reference matters, and the answer is
  usually found by trying both
- **Delay** integer samples + fractional, for cable mismatch
- **Null depth** readout in dB, with a peak-hold bar; this is the number the user is
  actually optimizing, and showing it turns blind knob-twiddling into a guided search
- **Auto section**: adaptation rate, Freeze button, and a *reference band* selector
- **Memories**: save/recall (frequency, mode, gain, phase, delay) via `ConfigManager`,
  keyed per device serial. DXers null the same local pest on the same band repeatedly;
  making them re-find the null every session is the difference between a feature that
  gets used and one that doesn't.
- **Monitor** combo: listen to A / B / combined without disturbing the weight — makes
  "is this actually helping?" answerable

**A 2-D gain/phase pad — built, and polar rather than rectangular.** The sketch above said
x = phase, y = gain, but the justification for wanting it at all was that this is how the
Perseus22 and RSR200 present it, and both are polar. Polar is also the better picture on its
own merits: the weight *is* a complex number, so the complex plane is its natural view, and
phase wraps round the circle instead of hitting a discontinuity at the edge of a box. Angle
is phase, radius is gain, centre is −40 dB, the emphasised ring is unity, the edge is +40 dB.

The "null depth shaded behind it" is a map painted by exploring, not a prediction: depth at
an untried setting cannot be known without applying it and measuring. Each cell records the
best depth seen there, so a sweep leaves a field behind, with a marker on the deepest point
and a control to return to it.

It also draws read-only in the adaptive and decorrelation modes, showing where the solver
has gone. For decorrelation, which applies a coefficient pair rather than one weight, the
equivalent `w = −k1/k0` is shown so the solver's answer lands on the same picture as a
hand-set one — a weight parked somewhere implausible is the first visible sign that the
covariance estimate is being pulled by the wrong signal.

**Two refinements from building it (Phase 3):**

- *Mode and Monitor are one control.* The list above has both, but there is a single
  output stream, so choosing what the combiner does and choosing what you hear are the
  same choice. Two controls would imply an independence that does not exist. They are one
  "Output" combo.
- *Sliders cannot land on an exact figure.* Dragging to "137 degrees" gives 136.98 or
  137.02, and a phasing null is sharp enough that the difference shows in the meter. The
  coarse and fine sliders are for hunting; a pair of numeric fields underneath are
  authoritative and step by 0.01. Both are needed -- neither alone is usable.

**Explicitly out of scope: headless/server mode.** misc_module ImGui menus are not
serialized over the SDR++ server protocol — only source menus, via `SmGui`. Putting the
controls in a misc module therefore means `sdrpp_server` users get no phasing UI. That
is an accepted limitation, not an oversight: mirroring the control set into the source
module's `SmGui` menu would duplicate it per driver, which is precisely the coupling this
design exists to avoid. The core pieces stay driver-agnostic, so if server support is
ever wanted it is a protocol extension, not a redesign.

### 2.4 The adaptive algorithm

**Block Wiener solution**, per block of N samples (N ≈ 4096):

```
  Rbb = Σ |B[n]|²                    (volk_32fc_magnitude_squared + accumulate)
  Rab = Σ A[n] · conj(B[n])          (volk_32fc_x2_conjugate_dot_prod_32fc)
  w_opt = Rab / (Rbb + ε)
  w ← (1−μ) · w + μ · w_opt          (μ from the "adaptation rate" control)
```

This is cheaper and far more numerically stable than sample-wise NLMS, and both kernels
are single volk calls. NLMS is the fallback if convergence granularity ever matters.

**The wanted-signal problem.** Naive minimization of output power nulls whatever is
strongest — which, when the DX signal peaks, is the DX signal. Three mitigations, in
increasing order of value:

1. **Slow adaptation** (long μ) so the weight locks onto the persistent interferer and
   ignores fades. Cheap, helps, insufficient alone.
2. **Freeze/Hold.** Let the user converge on the pest during a quiet moment, then lock.
   This is the workflow real phasing boxes impose anyway, and it is what most users will
   actually use.
3. **Reference-band adaptation.** Compute `Rab`/`Rbb` from a *user-selected slice of
   spectrum* containing only the interferer (a carrier, a buzz, an adjacent broadcast),
   while applying the resulting `w` to the full band. Implementation: a narrow DDC pair
   tapping A and B at the reference offset, feeding the correlator; the main path is
   untouched. This is the feature SDR++ can offer that an analog phasing box cannot —
   the user can *see* the pest on the waterfall and point at it.

**A wideband canceller cannot be validated with tones (found in Phase 5).** The obvious
test signal -- two pure tones with different inter-channel weights -- makes the multi-tap
weight look *worse* than a scalar, and it is not a bug in the solver. With tonal input
`Sbb(f)` is essentially zero away from the two bins, so `W(f) = Sab/Sbb` is undefined
across almost the whole band and its impulse response is not concentrated in time;
truncating it to N taps discards nearly all of it. Demonstrating the multi-tap weight
requires a *broadband* interferer reaching both channels with a timing difference, which
is why the synthetic source grew one in this phase. It was flagged as a Phase 5
prerequisite back when the source was designed, and it was the whole job.

**Null depth has to be measured where the null is (found in Phase 4).** The obvious metric,
`10*log10(mean|A|^2 / mean|Y|^2)` over the whole band, answers "did total power drop",
which is not the same question as "did the pest go away". A weight that cancels the
interferer perfectly is free to *amplify* everything else, so whole-band depth can mark the
correct answer down: in testing, a reference-band solution that nulled the interferer
exactly scored 3.4 dB while a wideband compromise that nulled nothing properly scored 4.2.
When a reference band is set, the depth is therefore measured inside it, and the UI says
which of the two it is showing.

### 2.6 Decorrelation: separating the dominant arrival

Prompted by the Perseus22, whose manual describes a "Decorrelation" function alongside
"Blending" modes named *Decorrelated Max* and *Decorrelated Min*. Its wording is precise
and worth quoting: the Signal function *"calculates the correlation matrix between the two
channels, producing two uncorrelated signals"*. That is an eigendecomposition of the 2×2
covariance — principal component analysis on two antennas.

**It is not "common versus uncommon".** Every signal reaching two antennas is present in
both; what distinguishes them is the complex ratio between channels, set by arrival
direction. So the decomposition separates *the dominant arrival* from *everything
orthogonal to it*:

- the **principal** eigenvector is the maximum-power combination — whatever arrives
  strongest;
- the **minor** eigenvector is everything else, with that arrival removed.

Two channels give exactly one degree of freedom, so this removes **one** dominant signal
per processed band. That is precisely the medium wave case: null the local and whatever it
was covering becomes audible.

For 2×2 Hermitian the decomposition is closed form — `(R - λI)u = 0` gives
`u = [Rab, λ - Raa]` — so there is no iteration and no matrix library
(`dsp/combine/decorrelator.h`).

**Relationship to the scalar weight.** `MODE_DECORR_MIN` and the existing auto-null solve
nearly the same problem, but the Wiener form `Y = A - wB` privileges channel A and its
weight grows without bound when the interferer is stronger in B. The eigenvector form is
symmetric and unit-norm, so it nulls the dominant component exactly whichever channel
favours it.

**`MODE_DECORR_MAX` is genuinely new** — the combiner could previously only subtract.
Peaking the dominant arrival is what makes a strong local *better*, rather than absent.

**Whitening.** A maximum-power combination is not a maximum-SNR one if the two channels
carry unequal noise: it drifts toward the noisier channel. Measuring the covariance on a
quiet channel and applying `R^(-1/2)` fixes that, and the manual makes the same point about
its own Noise function. The transform folds into the output coefficients
(`u^H W x = (W^H u)^H x`), so it never touches a sample.

**Coherence (the Perseus22's "Rho")** is `|Rab| / sqrt(Raa·Rbb)`, shown so the operator can
see whether there is a dominant arrival to separate at all. It answers a question null
depth cannot: whether the two channels are looking at the same thing.

**Why medium wave suits it so well.** At MW the antennas sit within a small fraction of a
wavelength, so the inter-channel ratio is flat across a 10 kHz channel and one complex
weight is exactly right — coherence runs near 1. At HF with wider spacing that ratio varies
across the band, which is what the multi-tap weight of §2.5 exists to handle.

**Placement.** The Perseus22 does this per channel window with an estimation bandwidth of
1–2 kHz for AM. Ours sits upstream of the VFOs, so the covariance is estimated over the
reference band (§2.4) and the resulting weights applied broadband. That is equivalent for
one VFO. Nulling a different local on each of several simultaneous VFOs needs the per-VFO
channelisation change §2.5 sets aside.

### 2.5 Wideband nulling (the honest limitation)

A scalar `w` produces a deep null only over the bandwidth where the two antenna+feedline
responses differ by a frequency-flat complex ratio. Across a few tens of kHz that holds
well. Across the several MHz SDR++ typically displays, it does not — the user will see a
deep notch at one frequency and progressively worse cancellation away from it.

Two answers, and the plan should ship the cheap one first:

- **Phase 5 (planned): multi-tap weight.** Replace scalar `w` with an N-tap complex FIR
  (N = 8…64), solved in the frequency domain via `W(f) = Sab(f) / (Sbb(f) + ε)` with
  cross/auto-spectra averaged over blocks, inverse-transformed and windowed to N taps.
  fftw3 is already linked. This gives a frequency-dependent null, subsumes the fractional
  delay control entirely, and is the difference between "notches one carrier" and
  "removes a neighbour's switching supply across the whole band."
- **Alternative not taken: per-VFO narrowband combining.** Deeper nulls per VFO, but it
  requires two complete channelization frontends and a much larger architectural change.
  Revisit only if the multi-tap wideband weight proves inadequate in practice.

Say this plainly in the UI too — a null-depth meter that reads 45 dB at the VFO while the
rest of the band barely moves will otherwise read as a bug.

---

## 3. Per-radio adapters

### 3.1 Fobos SDR (first implementation)

In `fobossdr_source`:

1. Add a fourth entry to the existing `ports` OptionList: `"hf_dual"` → `PORT_HF_DUAL`
   ("HF1 + HF2 (Phasing)"). This reuses the existing, already-persisted port mechanism.
2. Add a second `dsp::channel::RxVFO ddcB` and a second output `dsp::stream` alongside
   the existing one.
3. New worker branch: deinterleave `readBuf[i].re → chA`, `readBuf[i].im → chB` (volk
   `volk_32f_deinterleave_32f_x2`), zero-fill each imaginary part, feed both DDCs with
   the *same* offset. Both DDCs are configured identically and fed identical counts, so
   they stay in lockstep — but the phaser must not *rely* on that (trap (a)).
4. `registerChannels()` with `phaseCoherent = true`, `sampleAligned = true` when the port
   is `PORT_HF_DUAL`; unregister on stop or port change.
5. Keep `PORT_HF1` / `PORT_HF2` working exactly as they do now.

**Two different sample rates, and it matters — don't conflate them.**

- **`sampleRate`** — the rate the user picks in the menu and the rate SDR++ sees. The list
  is whatever `fobos_rx_get_samplerates()` reports, *plus* three synthetic entries
  (5, 2.5, 1.25 MHz) defined at `main.cpp:179-181`. So sub-50 Msps operation is normal
  and probably typical for HF work.
- **`actualSr`** — the rate the ADC and therefore the DDC input actually runs at. This is
  floored at 50 Msps regardless of the menu choice: `main.cpp:271` requests
  `(sampleRate >= 50e6) ? sampleRate : 50e6`. The synthetic sub-50 rates are produced by
  the DDC decimating down, not by slowing the hardware.

Everything downstream of the source module sees `sampleRate`. The DDC cost is set by
`actualSr`.

**Working assumption: 50 Msps is fine.** Taken as given from prior observation of the
hardware rather than treated as a blocker. The plan is built on it and it is recorded in
§7 as an assumption to revisit rather than a question to answer before starting.

**Cost concern: measured, and the prediction was backwards.** RxVFO throughput at the
50 Msps the ADC actually delivers, one instance versus two:

| Display rate | 1 DDC | 2 DDCs (each) |
|---|---|---|
| 1.25 MHz | 8.34x realtime | 7.51x |
| 2.5 MHz | 6.59x | 6.29x |
| 5 MHz | 4.13x | 3.93x |
| 10 MHz | 2.33x | 2.41x |

Two DDCs cost roughly 10% each rather than twice as much, because they run on separate
cores -- total throughput nearly doubles, 417 to 751 Msps.

The paragraph this replaces predicted that a *low* display rate would be the worst case,
and held a shared `PowerDecimator` in reserve as the mitigation. Both halves were wrong.
1.25 MHz is the **cheapest** case, not the dearest, and the reason is visible in the log
line `predec: 32`: `RationalResampler` already contains a power-of-2 pre-decimator, so the
mitigation was inside the block the whole time. A bigger decimation ratio means more of the
work happens in the cheap stage. The genuine worst case is the *highest* display rate, and
it still leaves 2.4x headroom.

**Mapping confirmed.** `.re` is HF1, `.im` is HF2, measured with an antenna on HF1 only:
`.re` sits 35.1 dB above `.im` and the correlation between components is 0.005. The
assumption held, so the one-line fix the plan budgeted for was not needed. The same
measurement showed the two ports have no measurable crosstalk, which matters because
crosstalk is what would otherwise put a floor under achievable null depth -- with both
ports open the components correlate at 0.97, purely because both pick up the same ambient
noise, and connecting one antenna collapses that to 0.005.

### 3.2 SDRplay RSPduo

Real driver work: switch `rspDuoMode` to `sdrplay_api_RspDuoMode_Dual_Tuner`, open both
`rxChannelA` and `rxChannelB`, run two callbacks into two streams, and keep the existing
single-tuner path for the other RSP models. `registerChannels()` with
`phaseCoherent = true` (shared clock; the LO phase offset is arbitrary but stable within
a run), `sampleAligned = true`. Both tuners must share sample rate and be tuned together
— the module already centralizes tuning, so this is mostly bookkeeping.

*Done, with one correction.* `phaseCoherent` had to be **false**: the offset is stable
within a run, as expected, but it is redrawn on every start, so it is not something a
saved setting can survive. That is a property of the radio, not a defect — it only means
weights are per-session.

The antenna port control needed a dual-mode path of its own. It compared the requested
tuner against `openDev.tuner` and called `sdrplay_api_SwapRspDuoActiveTuner` on a
mismatch, but in dual mode `openDev.tuner` is `Tuner_Both` and there is no active tuner
to swap — the call returns `InvalidParam`. Selecting the tuner-2 port would additionally
have repointed `channelParams` at channel B, quietly sending later gain changes to the
wrong tuner. In dual mode the only live choice is tuner 1's input, 50 Ω or Hi-Z, since
tuner 2 always has its own port.

### 3.3 Microtelecom Perseus22 / Reuter RSR200

No modules exist for either today (`perseus_source` targets the original Perseus). Both
are new source modules in their own right; the phasing work is then just the
`registerChannels()` call. Worth listing as validation targets rather than deliverables —
the point of the core contract is that these cost a few lines each once the driver exists.

### 3.4 Two-channel file source — test infrastructure *and* the playback half of §4

A synthetic/file-backed two-channel source is the only way to regression-test any of this
without hardware on the bench. It is also exactly what offline re-phasing (§4) needs on
the playback side. **These are one deliverable, not two** — build it early and it earns
its keep twice.

Two pieces:

- **Dual-channel WAV playback.** Extend `file_source` to read a 4-channel float WAV
  (I1 Q1 I2 Q2) and `registerChannels()` when it detects one. Then the entire phasing UI
  works on a recording with no further changes.
- **Synthetic generator** (optional but cheap): a source emitting a wanted tone plus an
  "interferer" at a settable inter-channel gain/phase, which makes the §6 acceptance
  criteria mechanical rather than a bench exercise.

With the radio unavailable, the synthetic generator is no longer optional — it is Phase 0
and the foundation for everything before hardware validation (§5). Dual-channel WAV
playback follows in Phase 2.

---

## 4. Dual-channel recording for offline re-phasing

Confirmed as a deliverable — this is the Perseus22 workflow: record both antennas raw,
and find the null later at leisure rather than fighting for it live while the DX fades.
For a DXpedition or an unattended overnight recording it is the difference between one
attempt at a null and unlimited attempts.

The good news is that most of the machinery already exists or is being built anyway.

### 4.1 Tap point — the one real design constraint

The recorder currently captures baseband via `sigpath::iqFrontEnd.bindIQStream()`
(`misc_modules/recorder/src/main.cpp:209`). That tap is **downstream of the phaser**, so
it would capture the already-combined stream — the opposite of what's wanted.

Raw dual-channel capture needs a tap on the `ChannelSet` streams *upstream* of the
phaser, and it must not steal samples from the phaser. So the channel plumbing (§2.1)
needs a `dsp::routing::Splitter` per channel, owned by the core-side wiring rather than
by the phaser, with `bindChannelStream(int channel, stream*)` /
`unbindChannelStream(...)` mirroring the existing `IQFrontEnd::bindIQStream` API. Fold
this into Phase 1 — retrofitting splitters after the phaser is wired is more disruptive
than putting them in from the start.

Consequence worth noting: the raw channels are upstream of the phaser and of
`IQFrontEnd`'s decimation and DC blocker, but they are the **DDC outputs**, so they run at
the user-selected `sampleRate` — *not* at the ≥50 Msps `actualSr` the ADC uses (§3.1).
Recording captures the band the user is actually looking at, at the rate they chose, which
is both the affordable option and the correct one: re-phasing only ever needs to work
within the displayed band.

### 4.2 Container

`wav::Writer` is already fully channel-generic: `setChannels(int)`, and
`write(float* samples, int count)` treats `count` as frames and handles `count *
_channels` internally (`core/src/utils/wav.cpp:150-156`). A 4-channel interleaved
float WAV (I1 Q1 I2 Q2) therefore costs essentially nothing on the write side — set
channels to 4 and interleave the two complex streams.

Caveats to handle:

- **Data rate doubles.** Per frame: 2 channels × 2 components = 4 samples, so 16 bytes at
  float32 or 8 at int16. Concretely, at the rates a DXer would plausibly use:

  | `sampleRate` | int16 | float32 |
  |---|---|---|
  | 1.25 MHz | 600 MB/min (36 GB/h) | 1.2 GB/min |
  | 2.5 MHz  | 1.2 GB/min (72 GB/h) | 2.4 GB/min |

  Large but tractable — an overnight unattended run at 1.25 MHz/int16 is roughly 290 GB.
  Default dual-channel captures to int16 (the existing `sampleTypes` control already
  covers it; the extra headroom of float32 buys little here) and show an estimated MB/min
  next to the checkbox so the user finds out before filling the disk rather than after.
  Worth saying in the UI that a lower `sampleRate` is the effective lever.
- **Metadata: an embedded RIFF chunk, decided.** A re-phasing session needs center
  frequency, sample rate, and the fact that this is a coherent dual-channel capture rather
  than quadraphonic audio. Sample rate rides in the WAV header; center frequency currently
  only appears in the filename template.

  Embedded rather than a JSON sidecar, because these recordings are kept for years, over
  which a sidecar is exactly the kind of thing that gets separated from its audio by a file
  move, a copy to another disk, or an archive tool. Metadata that cannot be parted from the
  samples it describes is worth a little extra work.

  **What the survey of real files actually showed** (checked rather than assumed, and it
  contradicted the first draft of this section):

  | Format | Container | Metadata |
  |---|---|---|
  | Perseus22 `.p22` | **not RIFF** — proprietary, magic `P22REC013` | fixed binary header, centre frequency as a float64 |
  | Perseus `.wav` | RIFF | `rcvr` chunk, 34 bytes |
  | QS1R `.wav` | RIFF | `rcvr` chunk, 38 bytes, with a receiver-name string |
  | HDSDR / SDR Console / SpectraVue | RIFF | `auxi` chunk, 164 bytes |

  So there is no Perseus22 RIFF chunk to follow — the Perseus22 does embed its metadata,
  but in a container of its own that has nothing to do with WAV. The decision to embed
  stands; the choice of *which* chunk has to be made on other grounds.

  **Write `auxi`.** It has the widest reader support of the three, and its layout is now
  verified rather than recalled, decoded from
  `HDSDR_20171123_005716Z_1130kHz_RF.wav` with the filename as independent ground truth:

  ```
  offset  size  field
     0     16   StartTime   SYSTEMTIME: year, month, dayOfWeek, day, hour, min, sec, ms
    16     16   StopTime    same layout
    32      4   CenterFreq  Hz            (read 1130000; filename says 1130kHz)
    36      4   ADFrequency
    40      4   IFFrequency
    44      4   Bandwidth
    48      4   IQOffset
    52      4   DBOffset
    56      4   MaxVal
    60      4   (repeat of centre frequency in the sample examined)
    64      4   unused
    68    ...   NUL-terminated name of the next file in a split recording
  total 164 bytes
  ```

  Start time 2017-11-23 00:57:16, day-of-week 4 (a Thursday, which it was), and centre
  frequency 1130000 all agree with the filename, so the field offsets are not in doubt.

  `auxi` carries a single centre frequency and has no concept of per-channel antenna
  labels or a coherence flag — it predates anyone recording two antennas at once. The
  phasing-specific metadata (channel names, `coherent`, which two channels were combined)
  therefore needs a second, custom chunk alongside it. Keep the standard chunk standard and
  put our own fields in our own chunk, so other software still reads what it can.

  Not in scope, but worth noting for later: reading `.p22` directly would let a Perseus22
  recording be re-phased in SDR++, which is a genuinely attractive feature. It is a
  separate project — a proprietary container reader, not a chunk.

  Implementation is small and already supported: `riff::Writer` has
  `beginChunk`/`write`/`endChunk` (`core/src/utils/riff.h`). The one constraint is
  ordering — `wav::Writer::open()` writes `fmt ` and then immediately opens the `data`
  chunk (`core/src/utils/wav.cpp:73-79`), so extra chunks have to be emitted between those
  two, from inside `open()`. That means `wav::Writer` needs the chunks handed to it
  *before* opening, not added afterwards.
- **Other software will misread it.** A 4-channel WAV opens elsewhere as quad audio.
  Unavoidable, and the same trade the Perseus22 makes; the sidecar mitigates it.

### 4.2.1 Interoperability, and why we still keep our own container

Checked against what actually exists rather than assumed, because the answer decides
whether Yet Another Format is justified.

**Neither SDRuno nor SDRconnect can record two tuners to one file.** Franco Venturi's
`rsp-recorder` writes four formats -- WavViewDX-raw, Linrad, SDRuno and SDRconnect -- and
instructs that for dual-tuner RSPduo captures you must use only the first two. The SDRplay
formats are single-tuner. SDRconnect does do live diversity on the RSPduo, so the gap is
specifically in *recording* the two channels separately, which is exactly what re-phasing
needs. That msiner's DuoTools exists at all is the same evidence from another angle.

**WavViewDX is the tool that matters, and it reads Linrad dual channel.** Reinhard Weiss's
importer list includes "Linrad RAW, single and dual-channel" alongside Perseus, P22, SDR++
and the rest. The same author's RSR200 Recorder writes files it can open, which is worth
knowing given an RSR200 is on the way.

**So the format question is settled by what carries the phasing metadata.** The Linrad
header has no field for phase coherence, and that is not a detail we can drop: the RSPduo
redraws its inter-tuner phase on every start, so a file that cannot say "these channels are
not coherent" is one that cannot be safely re-phased later. Nothing off the shelf carries
that, so our `sdpc` chunk stays.

**What makes this cheap is that everyone already agrees on the payload.** Linrad, DuoWAV,
rsp-recorder and this recorder all write int16 interleaved I1 Q1 I2 Q2. Four independent
implementations, one order. Our data chunk is therefore byte-for-byte a Linrad dual-channel
payload whenever the sample type is Int16, and conversion is a 41-byte header.

`core/src/utils/linrad_raw.h` defines that header and `tools/wav2linrad.cpp` performs the
conversion, verified byte-identical against the data chunk it came from. The header is
41 bytes only because it is packed -- natural alignment of its two doubles would pad it to
48 and produce a file nothing can read -- so `test_linrad_raw.cpp` asserts the size and
every field offset. It has no version field and only a leading `-1` sentinel for
identification, so nothing in the format would ever tell a reader we got it wrong.

Export is deliberately one-way for now. Bringing Linrad and RSR200 Recorder captures *in*
would let them be re-phased here, and is worth doing once there are real files to test
against.

### 4.3 Playback

Handled by the dual-channel `file_source` in §3.4. `WavReader`
(`source_modules/file_source/src/wavreader.h`) does not currently expose channel count
publicly and both workers hardcode two channels (`blockSize * 2 * sizeof(int16_t)` at
`main.cpp:214`, `sizeof(dsp::complex_t)` at `main.cpp:238`), so it needs a small
generalization. Once `file_source` calls `registerChannels()` for a 4-channel file, the
phasing module lights up on playback with no code specific to recording at all.

That is the payoff of the §2.1 contract: "re-phase a recording" and "phase a live radio"
are the same code path, and a recording made from a Fobos can be re-phased by someone who
owns no Fobos.

### 4.4 Recorder UI

One checkbox in baseband mode — "Record both channels (for later phasing)" — visible only
when the selected source has a `ChannelSet`, with the estimated data rate beside it.
Default off.

---

## 5. Implementation phases

| Phase | Scope | Deliverable |
|---|---|---|
**No phase requires the radio to be on the bench.** The Fobos is currently unavailable, so
the sequence is arranged so that everything up to and including auto-null is developed and
tested against a synthetic two-channel source. Hardware validation is a single pass
(Phase 6) once the radio is back, not a gate at the front.

| Phase | Scope | Needs hardware? | Deliverable |
|---|---|---|---|
| **0** | Synthetic source | No | **Done** — `source_modules/phasing_test_source/`. Two-channel signal generator: wanted tone + interferer at a settable inter-channel gain/phase, fractional channel-B delay, independent per-channel noise, A/B swap, and a built-in scalar test combiner so the module is self-verifying before the real phaser exists. Signal maths lives in `src/signal_model.h` (no SDR++ dependency) and is covered by `test/test_signal_model.cpp`; the worker/stream lifecycle is covered by `test/test_worker.cpp`. |
| **1** | Core plumbing | No | **Done** — `ChannelSet` registry in `SourceManager`, the `Phasing` front end (`core/src/signal_path/phasing.*`) owning a splitter per channel plus `bindChannelStream`, `dsp::combine::Phaser` (scalar, manual, own accumulation buffers), `selectSource()` wiring, bypassed by default via `MODE_A_ONLY`. Covered by `core/test/test_phaser.cpp` and `core/test/test_phasing.cpp`. The Phase 0 source gained a "Dual channel" mode that registers a `ChannelSet`. Confirmed live: with the source in dual channel mode the waterfall is indistinguishable from plain channel A, with no gaps or audible stutter. |
| **2** | Dual-channel I/O | No | **Done** — `wav::Writer` gained `addChunk()`; `core/src/utils/wav_meta.h` writes a verified `auxi` plus our `sdpc` chunk; the recorder has a "Record both channels" option writing I1 Q1 I2 Q2 with a MB/min estimate; `file_source` detects a 4-channel file and registers a `ChannelSet` so the phasing path lights up on playback. The two-input synchronisation the recorder needed was extracted from `Phaser` into `dsp::combine::ChannelSync` and is now shared. Covered by `core/test/test_wav_meta.cpp`; run everything with `core/test/run_tests.sh`. Confirmed live: a 19-second capture of the synthetic source wrote 4 channels at 1 Msps with `auxi` (centre 9917072 Hz) and `sdpc` (names 'A'/'B', both coherence flags) intact, and played back through the phasing path as two clean tones at the original offsets. |
| **3** | UI module | No | **Done** — `misc_modules/phasing/`: output select, coarse+fine gain and phase with exact numeric entry, swap, delay, null-depth meter with peak hold, per-frequency memories, settings keyed per source. `Phaser` gained a fractional-sample delay line. Confirmed live against the synthetic source. |
| **4** | Auto-null | No | **Done** — block Wiener solution (`MODE_AUTO`/`MODE_HOLD`), adaptation rate, Freeze/Resume/Copy-to-manual, and a reference band (`dsp/combine/ref_band.h`) restricting the solver to a slice of spectrum. Converges to the synthetic source's known weight to three decimals. Confirmed live. |
| **5** | Wideband | No | **Done** — `dsp/combine/wideband_solver.h/.cpp` solves an N-tap complex weight from averaged cross/auto-spectra and applies it as a short FIR; the delay control is disabled under it. Needed a broadband interferer in the synthetic source first (see below). Against a 3.7-sample skew a scalar recovers nothing while 64 taps hold the full null. |
| **6** | Fobos adapter + validation | **Yes** | `PORT_HF_DUAL`, dual DDC, `registerChannels()`. Then the deferred bench checks: confirm the `.re`/`.im` → HF1/HF2 mapping, phase stability over time and across a stop/start, and the CPU cost of two ≥50 Msps DDCs. First on-air nulls. |
| **7** | More radios | Yes | **RSPduo dual-tuner done** — `sdrplay_source` opens `Tuner_Both` in `Dual_Tuner`, splits the two callbacks into two streams and registers a `ChannelSet`. Measured on hardware: both `rspDuoSampleFreq` choices decimate to 2 MS/s, and the two callbacks deliver in exact lockstep (identical sample counts, call counts and samples per call). `phaseCoherent` stays false, now measured rather than assumed: coherence 1.0000 and drift under 0.15° *within* a run, but the A/B phase lands somewhere new on every start (six restarts gave +90.63, +147.93, +110.88, −73.43, −142.66, +90.35), so a saved weight cannot be restored across a stop/start. The antenna port control also needed a dual-mode path — see §3.2. Perseus22 and RSR200 modules still to come. |
| **8** | Decorrelation | No | **Done** — `dsp/combine/decorrelator.h`: closed-form 2×2 eigendecomposition, coherence, and whitening. `MODE_DECORR_MIN` nulls the dominant arrival, `MODE_DECORR_MAX` peaks it; noise measurement enables a maximum-SNR combine. Covered by `core/test/test_decorrelation.cpp`, where a station 26 dB beneath a local ends up 43 dB above it. See §2.6. |

Phase 0 is doing real work here, not box-ticking. A synthetic source with a *known*
inter-channel weight gives a ground truth no on-air test can: you can assert that auto-null
converges to the exact complex weight you injected, and that manual mode nulls to the noise
floor. On-air, you never know what the right answer was.

Phase 2 sits ahead of the UI for the same reason — once recording and playback work, a
single capture of a real local pest (made whenever the radio next surfaces, by anyone with
suitable hardware) becomes permanent regression material for Phases 3–5.

Resist the urge to build the adaptive algorithm before the manual controls work — manual
mode is the ground truth you debug auto-null against.

**What moving Fobos to Phase 6 costs:** the `.re`/`.im` channel mapping stays unverified
until then, so Phase 6 could surface a swapped or wrong assumption. The exposure is small —
the fix would be confined to the deinterleave step in the source module's worker, since
everything upstream of `registerChannels()` is driver-local and everything downstream is
channel-agnostic by construction. Cheap insurance: make the Phase 0 synthetic source able
to emit its two channels in either order, so the swap path is exercised from the start.

Build gating: `option(OPT_BUILD_PHASING "..." ON)` in `CMakeLists.txt` alongside the
other misc modules; the core pieces (registry, phaser block) are unconditional but inert.

---

## 6. Acceptance criteria

1. **Synthetic:** with the two-channel test source emitting one tone at a known
   inter-channel gain/phase, manual mode reaches ≥ 50 dB cancellation at the correct
   settings, and auto-null converges to within 0.5 dB / 2° of those settings in < 2 s.
2. **No regression:** existing single-channel sources are bit-identical with the phaser
   compiled in and disabled.
3. **No glitching:** 30 minutes of continuous dual-channel operation with no dropped
   blocks (instrument the phaser's ring buffers with a discard counter and assert zero).
4. **Weight changes are inaudible:** sweeping the phase control produces no clicks.
5. **On-air:** with two antennas on a Fobos, a local carrier is nulled by ≥ 25 dB and the
   wanted signal on an adjacent frequency is not degraded by more than 3 dB.

**Criterion 5 first met 2026-07-29**, on an RSPduo rather than a Fobos: with two real
antennas on medium wave, both auto-null and decorrelation pulled stations out from under
strong locals *in daylight* — the unfavourable case, with groundwave locals at full
strength and little skywave to recover. This is the first end-to-end confirmation on
signals nobody generated, and it validates the whole synthetic-source bet described in
§5: six phases developed with no radio, and the behaviour on air matched what the test
source predicted. Repeat on the Fobos in Phase 6 to close the criterion as written.

---

## 7. Open questions

- **Does the Fobos preserve the HF1/HF2 mapping across firmware versions?** Nothing in
  the API guarantees `.re` is HF1; it's an undocumented convention the current code
  relies on. Verify in Phase 6 (inject on one port, see which component moves) and leave
  a comment recording the assumption. If it ever flips, existing dual-channel recordings
  become ambiguous — another argument for recording channel *names* in the sidecar (§4.2)
  rather than relying on positional convention.
- **Is 50 Msps the real ADC rate in direct-sampling mode, and what does
  `fobos_rx_get_samplerates()` report there?** Deferred, not blocking — see the assumption
  below. Log `actualSr` in Phase 6. HF coverage to 50 MHz would need an ADC well above
  100 Msps, so either the usable coverage or the rate is lower than the constants suggest;
  worth knowing but it changes no design decision here.
- **Interaction with `IQFrontEnd`'s DC blocker and conjugate stage.** Both run
  downstream of the phaser and should be harmless, but the DC blocker's adaptation may
  interact with a rapidly-changing weight during auto-null convergence. Check in Phase 4.
- **A custom chunk id for the phasing metadata (§4.2).** `auxi` covers centre frequency
  and timing; channel names, the coherence flag and which pair was combined need a chunk of
  our own. The id should be unlikely to collide with anything else in the wild.

**Working assumptions (deliberate, revisit only if contradicted):**

- The Fobos operates correctly at 50 Msps in direct-sampling mode, per prior observation
  of the hardware. Two DDCs at that rate are assumed affordable; §3.1 records the fallback
  if the Phase 6 measurement says otherwise.
- `.re` = HF1 and `.im` = HF2, per the existing code.

**Resolved:** headless/server phasing support is out of scope (§2.3). Dual-channel
recording for offline re-phasing is in scope, now Phase 2 (§4), and its metadata goes in an
embedded RIFF chunks -- a verified `auxi` plus a custom one for the phasing fields --
rather than a sidecar (§4.2). Hardware
access is not on the critical path — Phases 0–5 are developed against a synthetic
two-channel source (§5).

---

## 8. Before shipping

Decisions already made that only take effect at release, collected here so they are not
rediscovered at the last minute.

- **Set `OPT_BUILD_PHASING_TEST_SOURCE` to `OFF`** in `CMakeLists.txt`. It is `ON` during
  development because Phases 1–5 are built and regression-tested against it, but a
  synthetic signal generator has no business in the source list of a shipped build. The
  option and the module stay in the tree — only the default changes. A reminder comment
  sits next to the option itself, which is where it will actually be noticed.
- ~~**Say the wideband limitation out loud in the UI** (§2.5).~~ Done in Phase 3, and it
  still applies whenever the multi-tap weight is switched off.
- **Freeze the custom phasing chunk's layout** before the first dual-channel recording
  exists, since every capture made afterwards has to stay readable by the same parser
  (§4.2). The `auxi` half is already pinned down against real files.
