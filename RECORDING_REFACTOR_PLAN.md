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

## 4. Goal 3: accurately reproducing live conditions

Two separate mechanisms, already partly addressed, partly not:

**Timing fidelity — done.** `file_source`'s lack of real-time pacing was a confirmed,
measured contributor to a shallower null on playback than live (8-10 dB vs 20+ dB live,
with reference band width and gain confirmed identical; improved to 12-15 dB after adding
pacing). That fix is already committed. What's still open, tracked in `RSR200_PLAN.md`
section 12 item 9, is the remaining ~10 dB gap — a separate investigation from this refactor,
not blocking it.

**Bandwidth fidelity — not yet addressed, and this plan needs a decision on it.** Dual-channel
recording currently captures at the *post*-software-decimation rate
(`sigpath::iqFrontEnd.getSampleRate()`). That means whatever bandwidth was chosen for display
convenience during the live session becomes a hard ceiling on what a later re-phase or
re-decorrelation attempt can ever recover — the opposite of "accurately reproduce live
conditions" if the live antennas actually delivered more bandwidth than was displayed.
**Recommended: dual-channel baseband recording should tap upstream of software decimation**,
at the true front-end rate, so a recording always preserves everything the hardware actually
delivered regardless of what the operator chose to look at live. Software decimation stays a
display/CPU-load convenience; it should not silently also be a recording-fidelity decision.
Tradeoff, stated plainly: recordings get larger (this is exactly why goal 1 has to land
first) and use more CPU to write.

## 5. Phased plan

| Phase | Scope | Depends on |
|---|---|---|
| **1** | Implement RF64 for real: `riff::Writer` gets the JUNK-placeholder-then-backpatch pattern (3.1); `wav::Writer` actually reads `_format`/decides RF64 vs plain WAV at close time based on real size, not a pre-selected format; `WavReader` (file_source's, currently already doing its own oversized-chunk workaround by trusting file size over declared chunk size) gets taught to recognize `"RF64"`/`"ds64"` properly rather than relying on that workaround indefinitely. | None — foundational, unblocks everything else. |
| **2** | Round-trip test suite: write through `wav::Writer` across all four `SampleType`s (`UINT8`/`INT16`/`INT32`/`FLOAT32`) and channel counts (1, 2, 4), read back through `WavReader`, assert exact sample match. Include one test that deliberately crosses the 4 GB boundary (small `STREAM_BUFFER_SIZE`-scale synthetic run, not a real multi-GB CI artifact) to exercise the RF64 backpatch path specifically. | Phase 1 |
| **3** | Fix `auxi`'s `stopTime` — patch it at `close()` the same way RIFF/data sizes already are. | None — independent, small, can land anytime. |
| **4** | Move dual-channel baseband recording upstream of software decimation (4. above). Needs the "recommended" call above confirmed, since it changes file sizes and CPU cost by default. | Phase 1 (files get bigger; needs RF64 in place first) |
| **5** | Default the dual-channel recorder's sample type to `FLOAT32` for anything beyond 16-bit (3.3) — specifically, stop offering/defaulting to `INT32` for RSR200's 24-bit mode. Keep other `SampleType`s available for anyone who wants them; this is a default change, not a removal. | Phase 2 (need the round-trip tests covering float32 dual-channel before changing what ships by default) |
| **6** | **Verify against real WavViewDX and real SDR Console** — both confirmed available. Record a real dual-channel RSR200 session past 4 GB, open it in each tool unmodified (no Linrad conversion step), confirm center frequency, channel count/order, and sample data all come through correctly. This is the actual acceptance test for goals 1 and 2 — nothing above is "done" until this passes, only "implemented." | Phases 1, 5 |
| **7** (conditional on phase 6's result) | If phase 6 shows WavViewDX reading the native RF64 file directly and correctly: update `tools/wav2linrad.cpp` to at least not be silently wrong (currently `Int16`-only and inherits the pre-RF64 32-bit size read) — either fix it to match the current format properly, or mark it clearly as legacy/optional now that direct RF64 import works. If phase 6 shows it does *not* read the native file correctly: `wav2linrad.cpp` becomes load-bearing rather than optional, and fixing its gaps moves from "nice to have" to required. | Phase 6 |

## 6. Open questions for Ralph, not resolved by research

- **Phase 4's tradeoff**: bigger files, more CPU, in exchange for recordings that can be
  re-phased/re-decorrelated at full bandwidth regardless of what was displayed live. Worth
  confirming this is the right default before it lands, since it's a real behavior change to
  existing recording sizes.
- **Phase 5's default change**: switching RSR200 dual-channel recording's default sample
  type away from `INT32` to `FLOAT32` changes file size (both are 4 bytes/sample, so this one
  is actually neutral) and — worth being explicit — means existing recordings made as INT32
  stay as they are; this only affects new recordings going forward. Confirm that's the right
  call rather than, say, offering it as a choice with FLOAT32 merely recommended in the UI.
- **Whether to keep the Linrad export path (`tools/wav2linrad.cpp`) as a going concern at
  all**, resolved by phase 6's actual test result rather than guessed now.
