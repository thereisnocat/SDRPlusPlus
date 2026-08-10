# Recording subsystem refactor plan

Not started. This is a plan, not a change — see [RSR200_PLAN.md](RSR200_PLAN.md) section 13
for how this got scoped: a string of one-off patches to broken recordings in one session
made clear the pattern needed one coherent pass, not more individual fixes.

## 1. Goals, in the order Ralph gave them

1. **Large files must work.** No file a normal recording session produces should come out
   with a header that can't state its own true size.
2. **Interoperable, primarily with WavViewDX, and with SDR Console where possible.** Not
   "opens in our own player" — opens in the actual third-party tools DXers use to review
   recordings, without a manual conversion step where that can be avoided.
3. **Accurately reproduce live conditions**, specifically for two-IQ-stream recordings made
   for phasing and decorrelation. A recording is only as useful as how faithfully replaying
   it reproduces what the antennas actually delivered live.

Non-goal: this plan does not cover audio-only (demodulated) recording, which has none of
these problems — scope is baseband/IQ recording, `RECORDER_MODE_BASEBAND`.

## 2. What's already broken — recap, not re-derivation

Full writeups are in `RSR200_PLAN.md` section 12, items 7-9. Short version, so this document
stands on its own:

- **The 4 GB ceiling.** `core/src/utils/riff.h`'s `ChunkHeader::size` is `uint32_t`. RSR200
  recording rates blow through it in under a minute. `wav::Format::FORMAT_RF64` exists as an
  enum value and is never implemented — `_format` is stored in `wav::Writer` and never read
  again anywhere in `wav.cpp`.
- **Recorded bandwidth silently tracks software decimation**, not the full front-end span
  (`misc_modules/recorder/src/main.cpp:181`, `samplerate = sigpath::iqFrontEnd.getSampleRate()`).
  Directly relevant to goal 3 below.
- **`file_source` had no real-time pacing** (fixed 2026-08-09, `paceToRealTime()`) and **no
  PCM bit-depth auto-detection** (also fixed the same day) until a real recording exposed
  both. The fixes are in; the pattern — a silent gap found only because a specific file broke
  in a specific way — is what this plan exists to stop.
- **`auxi`'s `stopTime` is never actually set.** `wavmeta::makeAuxi(freq, samplerate, now,
  now)` is called once at `start()` with the same value for both start and stop; nothing
  patches `stopTime` when recording actually stops, unlike the RIFF/data chunk sizes, which
  correctly do get seeked-back-and-patched on close.
- **No round-trip test coverage.** Nothing in `core/test/` writes through `wav::Writer`
  across all four `SampleType`s and multiple channel counts and reads the result back through
  `WavReader`, asserting the samples match. Every bug above was found by a human noticing a
  recording sounded or looked wrong.

## 3. Research: what the actual target software expects

Read, not assumed — this project's whole practice this session has been "measure, don't
guess," and format compatibility claims are exactly the kind of thing worth getting wrong
quietly for years if nobody checks.

### 3.1 RF64 is the right, and only reasonable, large-file answer

Both target tools already use it as their large-file format, not a niche add-on:

- **WavViewDX** is demonstrated handling a "90+ minute, 46-GB recording" from SDRconnect —
  RF64, in real use, at exactly the scale this project's own recordings reach.
- **SDR Console**: "If the file size is not selected or is selected with a value greater
  than 2GB then WAV RF64 is used, otherwise WAV is used" — RF64 is SDR Console's own default
  behavior past a size threshold, not a special mode.

The RF64 structure itself (confirmed against the EBU/BWF spec lineage and independent
implementer sources, not just Wikipedia's summary):

- The outer chunk's 4-byte ID changes from `"RIFF"` to `"RF64"`.
- Its 4-byte size field is set to `0xFFFFFFFF` — a sentinel meaning "see ds64."
- A mandatory `"ds64"` chunk **must be the first chunk after that header**, before `"fmt "`:
  `riffSize` (u64), `dataSize` (u64), `sampleCount` (u64), `tableLength` (u32, entry count for
  oversized non-data chunks — 0 in every case this project has), followed by that many
  8-byte table entries (none, normally).
- A plain `"data"` chunk still follows `"fmt "` as usual; its own 32-bit size field is also
  set to `0xFFFFFFFF` when ds64 is in use, with the real size coming from ds64's `dataSize`.

**Implementation pattern that avoids committing to RF64 up front:** reserve a
`"ds64"`-chunk-sized placeholder — written as an inert `"JUNK"` chunk — as the very first
thing after the header, before opening ever writes real sample data. Write the rest of the
file as ordinary `RIFF`/`WAVE` the whole time. At `close()`, if the true size stayed under
the 32-bit ceiling, leave it alone — a completely standard, maximally-compatible WAV file
with one harmless, spec-legal unknown chunk any RIFF reader already skips. If it exceeded
the ceiling, seek back and rewrite: bytes 0-3 (`"RIFF"` → `"RF64"`), bytes 4-7 (→
`0xFFFFFFFF`), and the placeholder's contents (`"JUNK"` → `"ds64"` with the real 64-bit
values). This is the same seek-back-and-patch mechanism `riff::Writer::endChunk()` already
uses for ordinary chunk sizes — an extension of an existing, working pattern, not a new one.
Every recording gets the placeholder; only the ones that need it ever become RF64. Small
recordings stay exactly as portable as they are today.

### 3.2 WavViewDX: the interleave convention already matches, and there's already a tested export path for the case it doesn't

- WavViewDX imports "a wide variety of IQ files including formats from Perseus, Jaguar,
  SDR#, SDRconnect, SDR Console, Winradio RXW, Winradio DDC, and Linrad," with **both single
  and dual-channel recordings**, and for dual-tuner cases specifically reads interleaved
  **I‑tunerA, Q‑tunerA, I‑tunerB, Q‑tunerB** — which is exactly, byte-for-byte, the
  `I1,Q1,I2,Q2` interleave `misc_modules/recorder/src/main.cpp` already writes and
  `source_modules/file_source/src/main.cpp`'s `dualWorker()` already reads. **No change
  needed here** — this part of the existing format is already right.
- Separately, WavViewDX explicitly supports **"Linrad dual channel for phasing"** as an
  import format, and this project already has real, tested code for that exact path:
  `tools/wav2linrad.cpp`, `core/src/utils/linrad_raw.h`, `core/test/test_linrad_raw.cpp`,
  `source_modules/rsr200_source/test/test_linrad_recording.cpp` — built and verified earlier
  in this project specifically because "the tool that matters here is WavViewDX (Reinhard
  Weiss), which imports 'Linrad RAW, single and dual-channel' among its formats" (comment,
  `linrad_raw.h`). That comment also already explains, correctly, why Linrad is an *export*
  format and not the archive format: Linrad's 41-byte header has nowhere to record phase
  coherence, sample alignment, or antenna names, which the RSPduo case in particular needs
  (its inter-tuner phase redraws every start).
- **But `wav2linrad.cpp` has its own real gaps, inherited from the exact bugs this session
  found:** it only handles `Int16` sample data (its own top comment says so directly), and it
  reads the source WAV's `dataBytes` as a plain `uint32_t` — the same field that overflows
  past 4 GB. A large or non-int16 RSR200 recording would silently mis-convert through this
  tool today. If the recorder's default sample type changes (3.4 below), this tool needs
  updating in lockstep or it becomes the next place today's exact bug resurfaces.
- **Open question this plan cannot resolve by reading documentation, only by testing**:
  whether WavViewDX can read this project's *own* RF64 WAV file directly (with its extra
  `auxi`/`sdpc` chunks, which a compliant reader should just skip) — in which case the Linrad
  export path becomes an alternative rather than the only route in. Phase 6 below is that
  test, not a guess.

### 3.3 SDR Console: sample format expectation matters more than chunk layout

SDR Console's own documented data format is narrower than what this project currently
writes: **"the data is in the data chunk as either 16-bit signed integers or 32-bit floating
point values."** No 24-bit, no 32-bit integer PCM. RSR200's 24-bit ADC mode currently gets
written as `SAMP_TYPE_INT32` (there's no dedicated 24-in-32 container type in `wav.h`'s
`SampleType` enum) — which is exactly the one format SDR Console's own documentation doesn't
list. **Concrete, actionable finding: for anything beyond 16-bit, `SAMP_TYPE_FLOAT32` is the
interoperable choice, not `SAMP_TYPE_INT32`**, independent of any precision argument — it's
simply the format the second target tool actually reads.

The `auxi` chunk this project already writes is independently confirmed (from the existing
`wav_meta.h` comment, decoded against a real HDSDR file rather than recalled) as "the de-facto
SDR metadata chunk shared by HDSDR, SDR Console and SpectraVue" — so the chunk *choice* for
center-frequency/timestamp metadata is already the right one; only its `stopTime` bug (2.
above) needs fixing.

Sources: [WavViewDX (Arctic DX blog)](http://arcticdx.blogspot.com/2025/02/wavviewdx-probably-most-versatile.html), [WavViewDX introduction (SWLing Post)](https://swling.com/blog/2025/10/an-introduction-to-wavviewdx-sdr-playback-software-a-totsuka-dxers-circle-article-by-kazu-gosui/), [Reinhard Weiss' tools page](https://rweiss.de/dxer/tools.html), [RF64 (Wikipedia)](https://en.wikipedia.org/wiki/RF64), [ds64 chunk structure discussion](https://markheath.net/post/naudio-rf64-bwf), [SDR-Radio.com data recording](https://www.sdr-radio.com/data-recording).

### 3.4 Does the int32-vs-float32 concern apply to other dual-stream sources? Checked, not assumed, 2026-08-09

Asked before starting Phase 5: does the same problem that motivated it (a source with genuine
>16-bit precision tempting a user toward `SAMP_TYPE_INT32`, the one format SDR Console
doesn't read) also apply to the other dual-channel-capable sources in this codebase —
FobosSDR and the SDRplay RSPduo? Checked each source's actual data path and each device's
real ADC resolution rather than assuming the RSR200 finding generalizes.

- **FobosSDR: no.** `fobos_rx_read_sync` (`/usr/local/include/fobos.h`) hands samples to
  `source_modules/fobossdr_source/src/main.cpp` directly as `float*` — there is no raw
  integer bit-depth choice in our own code for this source at all; the vendor's own driver
  already did whatever ADC-to-float conversion it needed. And even that aside, the
  [FobosSDR's actual ADC is 14-bit](https://www.ab9il.net/software-defined-radio/hf-radio-fobos-sdr.html)
  — comfortably inside what a 16-bit integer already represents losslessly. Nothing about
  this hardware ever gives a user a reason to reach for `INT32`.
- **SDRplay RSPduo: no, more strongly.** The vendor callback
  (`source_modules/sdrplay_source/src/main.cpp:1259`) delivers `short* xi, short* xq` —
  natively 16-bit integers, a hard ceiling regardless of the true ADC depth underneath. That
  true depth is itself
  [14-bit, dropping to 12/10/8-bit at higher sample rates](https://www.sdrplay.com/wp-content/uploads/2018/05/RSPduoDatasheetV0.6.pdf).
  There is no path by which this source can ever deliver more than 16 bits of real signal
  information — recording it as `INT16` isn't a compromise, it's already lossless.
- **RSR200 remains the only source in this codebase with the concern Phase 5 was written
  for.** Its documented 24-bit mode is genuinely more precise than 16-bit, which is exactly
  why someone would reach for `INT32` in the first place — and `INT32` is the one format
  neither WavViewDX's SDR Console interop notes nor SDR Console's own documentation lists.

**This changes Phase 5's scope.** The recorder's sample type is a single global setting
(`sampleTypeId`, `misc_modules/recorder/src/main.cpp:70`, already defaulting to `INT16`) —
not per-source. That default is already correct and already interoperable for FobosSDR and
RSPduo; changing it globally to `FLOAT32` would cost those users file size for zero
precision benefit and would not be fixing anything they actually have. The real, narrowly
scoped problem is only: when a user *does* need more than 16 bits (RSR200's 24-bit mode, and
no other source currently in this codebase), `INT32` shouldn't be the obvious reach — the
interoperable choice, `FLOAT32`, should be. `sigpath::sourceManager.getSelectedName()`
(`core/src/signal_path/source.h:43`) exists and would make a source-aware default
technically feasible, if that complexity is judged worth it later; the phase 5 entry below
reflects the narrower, UI-level fix instead.

## 4. Goal 3: accurately reproducing live conditions

**Corrected 2026-08-09, same day as the first draft.** The first draft of this plan treated
bandwidth (recording upstream of software decimation, to preserve everything the hardware
delivered regardless of what was displayed live) as the open question here. Ralph's
correction: that reading was wrong. Receiver (hardware) decimation and software decimation
*together* are what make a wide range of otherwise-unreachable sample rates available at
all — using both is a deliberate, valuable feature of the current design, not a fidelity
compromise to route around. Recording *after* software decimation is acceptable, and
preferable: it records the rate that was actually chosen and actually usable, not an
artificially inflated one nothing downstream asked for.

What "accurately reproduce live conditions" actually means, restated correctly: **don't
introduce rate errors** — no half-speed or double-speed playback, no misinterpreted sample
width, nothing that makes a recording sound like anything other than the real signal at the
real rate it was captured at. That's a timing/format-correctness concern, not a bandwidth
one, and it's already substantially addressed:

- `file_source`'s lack of real-time pacing was a confirmed, measured contributor to a
  shallower null on playback than live (8-10 dB vs 20+ dB live, with reference band width
  and gain confirmed identical; improved to 12-15 dB after adding pacing). Fixed and
  committed.
- The float32/int16 misdetection and missing int32/uint8 support (RSR200_PLAN.md section 12,
  items covering the two recordings that motivated this whole plan) are fixed and committed.

What's still open (`RSR200_PLAN.md` section 12 item 9) is the remaining ~10 dB gap after the
pacing fix — a separate DSP investigation, not a recording-format question, and not blocking
this plan. **No bandwidth/tap-point change is planned.** Recording continues to happen after
software decimation, as it does today.

## 5. Phased plan

| Phase | Scope | Depends on |
|---|---|---|
| **1** ✅ | **Done, 2026-08-09.** Implement RF64 for real: `riff::Writer` gets the JUNK-placeholder-then-backpatch pattern (3.1); `wav::Writer` actually reads `_format`/decides RF64 vs plain WAV at close time based on real size, not a pre-selected format; `WavReader` (file_source's, currently already doing its own oversized-chunk workaround by trusting file size over declared chunk size) gets taught to recognize `"RF64"`/`"ds64"` properly rather than relying on that workaround indefinitely. Verified: full `core/test/run_tests.sh` suite unchanged and passing; a round-trip spot-check (small plain-WAV and forced-RF64) confirms header shape and exact sample data round-trip; the two already-hand-patched real recordings from earlier this session still read with the exact same correct data sizes. Found and fixed one real edge case along the way: `forceRF64()` alone didn't make the outer chunk's own size field carry the sentinel unless its own real size also happened to be huge — fixed so the outer chunk's sentinel decision also respects `needsRF64` having already been set. | None — foundational, unblocks everything else. |
| **2** ✅ | **Done, 2026-08-09.** Round-trip test suite (`core/test/test_wav_roundtrip.cpp`): write through `wav::Writer` across all four `SampleType`s (`UINT8`/`INT16`/`INT32`/`FLOAT32`) and channel counts (1, 2, 4) — 12 combinations, 105 checks — read back through the real `WavReader`. Covers exact channel/rate/codec/size round-tripping, per-type quantization tolerance, channel-order correctness, plus the forced-RF64 path (`forceRF64()`, no real multi-GB file needed) and confirming a small file stays plain WAV. Wired into `run_tests.sh`; full suite 13/13. | Phase 1 |
| **3** ✅ | **Done, 2026-08-09.** Fix `auxi`'s `stopTime` — patch it at `close()` the same way RIFF/data sizes already are. Generalized rather than special-cased: `riff::Writer` gained `tellp()`/`patchAt()`, `wav::Writer` built `patchChunk(id, offset, data, len)` on top, and the recorder's `stop()` now patches the real stop time in before `close()`. Verified with a new round-trip test mirroring exactly how the recorder uses it; full suite 13/13. | None — independent, small, can land anytime. |
| ~~**4**~~ | ~~Move dual-channel baseband recording upstream of software decimation.~~ **Rejected 2026-08-09** — see section 4 above. Hardware and software decimation together are how a wide range of sample rates becomes reachable at all; recording after software decimation is the right behavior, not a fidelity compromise. No tap-point change planned. | — |
| **5** (narrowed 2026-08-09, see 3.4) | ~~Default the dual-channel recorder's sample type to `FLOAT32` globally.~~ Section 3.4 found the global default (`INT16`) is already correct and already interoperable for every other dual-stream source in this codebase (FobosSDR, RSPduo) — changing it for everyone would cost them file size for no benefit and fix nothing they have. Narrowed to: make `FLOAT32`, not `INT32`, the obvious reach in the UI specifically for the >16-bit case (RSR200's 24-bit mode is the only one that currently exists) — e.g. dropdown ordering/labeling or a tooltip noting `INT32`'s interop gap, not a default value change. Global default stays `INT16`. | Phase 2 |
| **6** | **Verify against real WavViewDX and real SDR Console** — both confirmed available. Record a real dual-channel RSR200 session past 4 GB, open it in each tool unmodified (no Linrad conversion step), confirm center frequency, channel count/order, and sample data all come through correctly. This is the actual acceptance test for goals 1 and 2 — nothing above is "done" until this passes, only "implemented." | Phases 1, 5 |
| **7** (conditional on phase 6's result) | If phase 6 shows WavViewDX reading the native RF64 file directly and correctly: update `tools/wav2linrad.cpp` to at least not be silently wrong (currently `Int16`-only and inherits the pre-RF64 32-bit size read) — either fix it to match the current format properly, or mark it clearly as legacy/optional now that direct RF64 import works. If phase 6 shows it does *not* read the native file correctly: `wav2linrad.cpp` becomes load-bearing rather than optional, and fixing its gaps moves from "nice to have" to required. | Phase 6 |

Phase numbers are kept as originally assigned, gaps included, rather than renumbered —
matches this project's convention elsewhere of keeping corrected reasoning visible instead
of silently rewriting it away.

## 6. Open questions for Ralph, not resolved by research

- ~~Phase 4's tradeoff~~ — resolved, rejected, see section 4.
- ~~Phase 5's default change~~ — resolved, narrowed rather than made globally, see section 3.4.
  Global default stays `INT16` (already correct for FobosSDR/RSPduo); only the >16-bit case
  (RSR200 today) gets nudged toward `FLOAT32` over `INT32`, and that's a UI-level nudge, not
  a default value change, so existing recordings and other sources are unaffected either way.
- **Whether to keep the Linrad export path (`tools/wav2linrad.cpp`) as a going concern at
  all**, resolved by phase 6's actual test result rather than guessed now.
