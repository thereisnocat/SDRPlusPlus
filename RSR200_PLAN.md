# Plan: Reuter RSR200B source module for SDR++

**Goal:** A source module for the Reuter RSR200B direct-digitising receiver, usable over
both its USB 3.0 and its Ethernet interface, with its two coherent channels exposed to the
phasing front end built in `PHASING_PLAN.md`.

**Sources:** `RSR200_DP_ENG_V52.pdf` (data protocol, v0.52, firmware 225) and
`RSR200_OM_V225.pdf` (operator's manual, v2.25). Everything below is drawn from those two
documents; where they disagree with each other it is called out explicitly.

---

## 1. Both prerequisites are resolved

Each interface had a dependency outside our control. Both have been checked, and both are
fine.

**(a) FTDI D3XX for macOS: available, and better than expected.** `d3xx-osx.1.1.8.dmg`
ships `ftd3xx.h`, `Types.h`, `libftd3xx.1.1.8.dylib` and a static library, plus a set of
streaming example programs. Inspected:

- **Universal binary** — `x86_64` and `arm64`, so Apple Silicon is covered.
- **Entirely user space.** It links only `libSystem`, `libobjc`, `libc++`, IOKit,
  CoreFoundation and Security. There is **no kernel extension and no system extension** to
  install and approve, which on current macOS is the difference between "copy two files"
  and a fight.
- The API the RSR200 documentation asks for is all present: `FT_SetStreamPipe`,
  `FT_ReadPipeAsync`, `FT_InitializeOverlapped`, `FT_GetOverlappedResult`,
  `FT_SetPipeTimeout`, and `FT_Create` with `FT_OPEN_BY_DESCRIPTION` (DP §4.1's preferred
  method) and `FT_OPEN_BY_SERIAL_NUMBER`.

**Installation is a pending manual step and needs sudo**, so it is Ralph's to do, not
something the build can arrange. Per the bundled ReadMe: copy `ftd3xx.h` and `Types.h` to
`/usr/local/include`, and `libftd3xx.dylib` plus `libftd3xx.1.1.8.dylib` to
`/usr/local/lib`.

> **The same install-name trap as the SDRplay library, and worse than first written here.**
> `libftd3xx.1.1.8.dylib` records a bare install name of `libftd3xx.dylib`. An earlier draft
> of this section said that resolves via dyld's fallback search path and the rewrite was
> merely more robust. **That was wrong — it does not resolve at all.** Tested: dyld treats a
> slash-less install name as a path relative to the *current working directory*, and never
> consults `/usr/local/lib`:
>
> ```
> Library not loaded: libftd3xx.dylib
>   tried: 'libftd3xx.dylib' (no such file), '<cwd>/libftd3xx.dylib' (no such file)
> ```
>
> So rewriting the dependency with `install_name_tool -change` is **mandatory**, not a
> nicety. The module's CMake must do what `sdrplay_source/CMakeLists.txt` now does: link the
> versioned file by absolute path and rewrite the recorded dependency as a post-build step.
> Note that this needs `-Wl,-headerpad_max_install_names` at link time when the replacement
> path is longer than the original, or `install_name_tool` refuses with "larger updated load
> commands do not fit".

> **Quarantine blocks the library even once it is found.** Files copied out of the
> downloaded DMG carry `com.apple.quarantine`, and Gatekeeper refuses to load a quarantined
> library into a process — reported as *"code signature not valid for use in process:
> library load disallowed by system policy"*, which reads like a signing problem and is not
> one. The signature is fine (FTDI, team `658CPPCMJJ`, verifies clean). Clear it after
> installing:
>
> ```
> sudo xattr -d com.apple.quarantine /usr/local/lib/libftd3xx.dylib /usr/local/lib/libftd3xx.1.1.8.dylib
> ```
>
> Approving the block in **System Settings → Privacy & Security** works too — the first
> failed load raises the prompt — and that route leaves the xattr in place, so a lingering
> `com.apple.quarantine` is *not* evidence the library is still blocked. Test by loading it
> rather than by reading attributes. (That is what was done here.)
>
> The approval is tied to that exact binary, so **a D3XX update will be blocked again**,
> with the same misleading "code signature not valid" wording. Deleting the attribute
> removes the cause rather than recording an exception, which is why it is the better habit
> of the two — particularly in any scripted or repeated install.
>
> Verified on this machine: with the install name rewritten and Gatekeeper satisfied, the
> library loads with no `DYLD_LIBRARY_PATH` set and `FT_CreateDeviceInfoList` returns
> `FT_OK` with zero devices — the correct answer with no radio attached. The USB path is
> therefore proven end to end apart from the transport itself.

> **The `make` step in the bundled ReadMe is optional.** The Makefile builds seventeen demo
> programs (`streamer`, `LoopBack`, `GetDevInfo`, …); the library itself ships prebuilt, so
> the two headers and two dylibs are the whole driver. The ReadMe's `./rw` does not exist in
> this package — there is no such source and no such target — and every target it *does*
> build carries a `-dynamic` suffix, so even the real ones are `streamer-dynamic`. The demos
> also link `-L.`, so building them means copying the folder somewhere writable first, the
> DMG being read-only. `GetDevInfo` and `streamer` are the two worth building once the radio
> is present.

**(b) SFP module: ordered.** The RSR200's LAN port is an SFP 1000 cage rather than an
RJ-45 jack, so a copper 1000BASE-T SFP is needed. On order alongside the radio.

**Implementation order is unchanged.** LAN still goes first — plain BSD sockets, no vendor
SDK, no install step, and it works on every platform SDR++ targets. USB is now a definite
follow-on rather than a maybe.

## 2. What the radio is

- Direct-digitising receiver, 1 kHz – 66 MHz on HF1 and HF2, 66 – 150 MHz on VHF.
- **Two 16-bit ADCs**, clocked together at an adjustable **70 – 200 MHz** (0.1 MHz steps).
- Three inputs: **HF1** and **VHF** are switchable and both feed channel 1; **HF2** feeds
  channel 2. Channel 2 can alternatively be switched in parallel with channel 1.
- FPGA does the down-conversion and decimation; output is IQ at 16 or 24 bits.
- Integrated GPS receiver disciplines the ADC clock.
- Remote power and RS-232-over-coax control for active antennas (RLA4, RFA2, RAP1/2).

**Sample rate = ADC clock / decimation.** Decimation is `2^(D+1)` for `D = 0..5`, so 2 to
64. That gives 1.09 MSp/s (70 MHz / 64) at the bottom and 100 MSp/s (200 MHz / 2) at the
top, with the interfaces capping the practical maximum well below that: roughly 85 MSp/s
over USB and 30 MSp/s over LAN at 2×16-bit.

For our purposes the *bottom* of the range matters most — SDR++ on HF wants a few MSp/s,
not thirty — and 1.09 MSp/s is comfortably reachable.

**Retries need a new command number, not a repeat.** Every command carries a "repeat
counter" intended for exactly that, but DP §3.5 says firmware 22x does not check it: a
repeated command is simply executed again. So the command layer must treat a missing
acknowledgement by re-issuing under a *fresh* number and tolerating the possibility that the
original also took effect — not by bumping the repeat field.

> **Document conflict.** OM §5.1 says the highest decimation is 16, giving a 4.375 MSp/s
> minimum. DP §3.3 (newer, firmware 225) documents decimation up to 64. The manual section
> looks stale. **Verify on hardware**; if only 16 is available the minimum sample rate is
> 4.375 MSp/s, which is a much heavier default for SDR++ and changes the UI's sensible
> defaults.

---

## 3. One protocol, two framings

The command set and the semantics are shared; only the framing differs. That shapes the
whole design: **a transport interface with two implementations, and a single protocol layer
above it.**

### 3.1 USB framing

Fixed **4096-byte packets** from bulk endpoint `0x82`; commands are written to `0x02`.

| Offset | Size | Contents |
|---|---|---|
| 0 | 4 | packet counter, incrementing |
| 4 | 4080 | IQ data |
| 4084 | 1 | temperature, °C, signed |
| 4085 | 2 | GPS frequency correction (signed 14-bit) + overload bits 6,7 |
| 4087 | 1 | command number |
| 4088 | 8 | one command |

**One command per packet.** 4080 bytes of IQ is 1020 samples at 1×16-bit, 510 at 2×16-bit,
680 at 1×24-bit, 340 at 2×24-bit.

The FT601Q has a double 4096-byte buffer; the PC must drain one while the other fills or
data is lost. The manual recommends asynchronous overlapped reads with many 4096-byte
buffers queued.

### 3.2 LAN framing

TCP server on **port 55557**, UDP on **55558**. Default address **192.168.1.10**, with a
DHCP client running for ~5 s after power-up.

Blocks are a **fixed 130560 samples** and therefore a variable byte length:

| Mode | Block bytes | `Start stream` size code |
|---|---|---|
| 1 ch, 16 bit | 522704 | 7 |
| 2 ch, 16 bit | 1045408 | 15 |
| 1 ch, 24 bit | 784784 | any other value |
| 2 ch, 24 bit | 784784 | 5 |

Note the last row: at 24-bit the block length is the same for one and two channels, so
**2×24-bit carries only 65280 samples per channel**, half the usual count.

After the IQ comes a 32-bit block counter, the same counter **inverted**, then two
synchronisation words (`0x12345678`, `0x9ABCDEF0`, little-endian on the wire as
`78 56 34 12 F0 DE BC 9A`), then temperature / GPS / command number, a **32-bit count of
commands**, and the commands themselves.

The counter-plus-inverse-plus-sync pattern exists so a receiver can find block boundaries
in a byte stream and detect loss. **Use it** — do not assume the first byte received is a
block boundary.

Unlike USB, a LAN block can carry **several commands**, and the order can matter.

### 3.3 UDP framing

The radio fragments each LAN block into **1458-byte packets**: a 16-bit packet index
followed by 1456 bytes of block payload. Reassemble by index. Only the last packet of a
block contains the trailer and commands.

UDP is unacknowledged, so a lost packet is a hole in the block. The manual's advice is
sound and we should follow it: **use UDP for the IQ stream only, and send commands over
TCP**, keeping both sockets open.

---

## 4. Commands

PC → radio, always as discrete packets (never embedded):

```
bytes 0-3   32-bit command number, our choice, used to match the acknowledgement
byte  4     instruction
bytes 5+    parameters
```

USB uses fixed lengths (8, 12 or 16 bytes); LAN truncates the unused tail.

| Instr | Command | USB / LAN | Notes |
|---|---|---|---|
| `0x12` | Read version numbers | 8 / 6 | returns 24-bit serial + firmware |
| `0x15` | Start stream | 8 / 7 | port: 0=UDP, 1=TCP, 2=USB; plus size code |
| `0x16` | Stop stream | 8 / 7 | **on USB this closes the send endpoint entirely** |
| `0xB0` | Set LO / magnitude+phase / IP | 12 / 11 | see §6 and §7 |
| `0xB1` | Set automatic attenuator | 16 / 14 | |
| `0xB2` | Reset | 8 / 8 | soft reset, reloads firmware from flash |
| `0xB4` | Set data transmission | 12 / 9 | interface, port mode, DSP mode |
| `0xF2` | Set ADC clock | 8 / 8 | 0.1 MHz units; also GPS discipline on/off |
| `0xF5` | Set 16-bit variable | 12 / 9 | attenuators, input switching, antenna control |

**Port mode byte** (`0xB4` byte 6): bits 0–2 decimation exponent `D`; bit 3 channel
selection/swap; bit 4 channel count (0 = single, 1 = dual); bit 5 bit width (0 = 24, 1 = 16).

**DSP mode byte** (`0xB4` byte 7): bits 0–1 operating mode — 0 independent ("Separate",
requires dual channel), 1 parallel/added, 2 serial (doubles the effective ADC rate,
requires inverted ADC2 clock), 3 parallel diversity. Bit 3 selects sideband in serial mode.

**16-bit variables** (`0xF5`): 0 clock correction; 1 and 2 attenuators (0…35, meaning
+7 dB gain to −28 dB attenuation); 3, 4, 6, 7 antenna control; **5 the switch register** —
bit 1 routes ADC1 to HF1 or VHF, bit 2 routes ADC2 to ADC1 (parallel) or HF2, bits 3–6
remote power, bit 7 the VHF preamp.

### 4.1 Commands coming back

Replies are embedded in the stream, not sent separately (except a LAN version reply when
not streaming). Each block carries a **command number**; the same command data repeats in
every block until the radio writes a new one and changes the number. **A changed number is
the signal to parse.** Number 0 means "self-generated by the radio", so never send 0.

Self-generated commands matter: the radio reduces the ADC clock on its own if the FPGA
passes 87 °C, and reports it this way.

---

## 5. Traps in the data path

**Auto-ATT shifts the whole stream by 2 bits.** DP §4.7: enabling it scales the entire data
stream down by 12.04 dB to make headroom. The module must apply the matching 4× gain when
converting samples, or levels jump the moment the feature is switched on.

**Temperature −128 °C (`0x80`) is not a temperature.** It is the Auto-ATT active indicator,
and it persists for up to ~0.5 s after the attenuator releases. Displaying it as a
temperature would be wrong and alarming.

**Overload bits live in the GPS field.** Bits 6 and 7 of the third header byte are the ADC1
and ADC2 overload flags; the frequency correction is the signed 14-bit remainder.

**Stopping the USB stream is not symmetric.** DP §3.3: `Stop stream` on USB closes the send
endpoint completely — no further data *or command replies* can come back, though the radio
still receives. Restarting the stream is the only way out. Prefer leaving USB streaming
running and simply not reading, or be very deliberate about the stop path.

**GPS unavailable reads as `0x2000`**, the largest negative value, not as zero.

---

## 6. Tuning, and the Nyquist zone problem

Tuning is `0xB0` with a signed 32-bit LO frequency in Hz, at 1 Hz resolution.

But the ADC digitises everything, and signals above half the ADC clock fold back. The
module cannot just pass SDR++'s centre frequency through as the LO. For a requested RF
frequency `f` and ADC clock `Fs`:

```
zone   = floor(f / (Fs/2)) + 1
LO     = (zone odd)  ? f - (zone-1)*(Fs/2)
                     : zone*(Fs/2) - f        // spectrum inverted
```

Odd zones map straight through; **even zones arrive frequency-reversed** and the module
must tell the front end to conjugate, or the waterfall is mirrored and SSB comes out on the
wrong sideband. `IQFrontEnd` already has a conjugate stage (`setInvertIQ`) for exactly this.

The user still has to supply an appropriate anti-alias filter for anything but the first
zone — that is inherent to the radio, and the manual spends four pages on it. The module's
job is to **show which zone the current tuning lands in** and what the alias frequencies
are, so the situation is visible rather than mysterious.

The `SerL`/`SerU` modes (DSP mode 2) sample the two ADCs offset in time to double the
effective rate and widen the zones, at the cost of ~30 dB alias rejection rather than the
filters' full depth. Worth exposing eventually; not needed for a first version.

---

## 7. Dual channel, and how this meets the phasing work

This is where the RSR200 becomes interesting for what we have already built.

**"Separate" mode (DSP mode 0) with dual-channel output** gives two independent IQ streams,
one per ADC. Set **switch variable 5 bit 2 = 1** to route ADC2 to HF2, and the two channels
are two antennas.

DP §4.6 is explicit that from firmware V223 the two channels can run **fully phase
correlated**: the latencies are matched, and synchronisation circuits reset the mixer DDS
generators and decimator state machines together so the phase relationship is exact.
Synchronisation happens on an ADC clock change or a data-transmission change.

The critical operational rule, and it must be respected by the command sequencing:

> "Simultaneous adjustment of both LO frequencies with one command guarantees synchronism;
> no resynchronisation is necessary."

So **always tune with `0xB0` channel selector 2 ("both channels")**, never as two separate
per-channel commands. Tuning them individually and then back to a common frequency leaves
the phase relationship undefined until the next synchronisation event.

Given that, the module registers a `ChannelSet` with `count = 2`, names `HF1`/`HF2`,
**`phaseCoherent = true`, `sampleAligned = true`** — and the entire phasing feature
(manual controls, auto-null, reference band, wideband multi-tap, dual-channel recording)
works with no further changes. That is the payoff of having put the mechanism in core.

### 7.1 The radio can also phase in hardware

`0xB0` with channel selector **9** sets a magnitude and phase for channel 2, and DSP mode 3
("Diversity") adds the channels in hardware. Magnitude is unsigned 16-bit at 1/8192 per
LSB (up to 8× gain); phase is signed 16-bit spanning ±180°. OM §5.4.2 claims up to 60 dB of
suppression at low frequencies, 20–30 dB at HF.

This overlaps with our software phaser, and the honest position is that they are for
different things:

- **Hardware diversity** halves the data rate (one combined channel instead of two) and
  costs no PC CPU. It is a single complex weight, manually set, applied before transmission.
- **Software phasing** keeps both channels — so it can adapt, use a reference band, apply a
  wideband multi-tap weight, and let a recording be re-phased afterwards. None of that is
  possible once the radio has already added the channels together.

**Offer both, default to software.** Expose hardware diversity as an operating mode for
users who are bandwidth-constrained or want the radio to do the work; use Separate mode for
everything else. Do not try to drive the hardware weight from our adaptive solver as a
*control loop* — the round trip through the command channel is far too slow.

### 7.2 Solve in software, hold in hardware

The two are better combined than chosen between, and the decorrelator of `PHASING_PLAN.md`
§2.6 makes it straightforward. There is a chicken-and-egg to respect: the weight cannot be
*found* in Diversity mode, because the radio then returns only the combined result. So the
workflow is necessarily two-step.

1. Run in Separate mode with both channels. `MODE_DECORR_MIN` finds the combination that
   nulls the dominant arrival, and `Phasing::getCombineCoefficients` exposes it as an
   additive pair `y = k0·A + k1·B`.
2. `hardwareWeightFor()` converts that to the radio's magnitude and phase, and
   `Device::setHardwareDiversityFrom()` sends it.
3. Switch to `OP_DIVERSITY` and single-channel output. The radio now holds the null on its
   own, at half the data rate and no PC cost.

**Mind the sign.** The radio computes `Y = A + g·B` — it *adds*, where the software
phaser's manual weight is defined for subtraction. An additive coefficient pair converts
directly as `g = k1/k0`; a subtractive weight would need `g = -w`. Getting this backwards
produces a combination that peaks the interferer instead of nulling it, which looks like a
sign of life and is therefore easy to accept.

**Mind the range.** The magnitude is a 16-bit value at 1/8192 per LSB, so it spans 0 to
just under 8. A combination needing more than 8× on channel 2 is inexpressible;
`hardwareWeightFor()` reports that and suggests a channel swap, which inverts the ratio and
is a single bit in the port mode.

Quantisation is not the limitation: pushing a solved weight through the wire format and
back leaves the null 88 dB deep, far below the 20–60 dB the manual attributes to antenna
and propagation stability.

---

## 8. Module architecture

```
source_modules/rsr200_source/
    CMakeLists.txt
    src/
        main.cpp          SDR++ module: menu, config, SourceHandler, ChannelSet
        rsr200_protocol.h wire format: block geometry, commands, unpacking, zone maths
        rsr200_device.h   device: config ordering, command numbering, acks, frame parsing
        transport_lan.cpp TCP + UDP sockets, block resync, UDP reassembly
        transport_lan.cpp TCP + UDP sockets, block resync, UDP reassembly
        transport_usb.cpp FTDI D3XX
        framing.h/.cpp    block layouts, sample unpacking, embedded-command parsing
```

The split matters because the two transports differ *only* in framing and I/O. Keeping
`rsr200.cpp` transport-agnostic means the LAN path can be finished and tested before USB is
attempted, and USB can be compiled out on platforms without D3XX (it is a Windows, Linux and
macOS library, but not universal) without `#ifdef`s spreading through the module.

**Sample unpacking.** 16-bit is a plain `int16 → float` scale (volk has
`volk_16i_s32f_convert_32f`). 24-bit needs manual little-endian sign extension from 3 bytes;
no volk kernel fits, so a hand loop, which is fine at these rates. Both must apply the
Auto-ATT 4× compensation when it is enabled.

**Threading.** One worker thread per transport reading and parsing, writing into either one
`dsp::stream` (single channel) or the two `ChannelSet` streams (dual). Deinterleaving the
dual-channel formats is the same shape as the Fobos work in `PHASING_PLAN.md` §3.1.

**Write sizing.** A USB packet is only 1020 samples; at high rates that would mean tens of
thousands of stream swaps per second. Accumulate several packets into a sensible block
(~5 ms worth) before swapping. LAN blocks are already 130560 samples, which is fine as-is
and sits comfortably inside `STREAM_BUFFER_SIZE`.

---

## 9. Phases

| Phase | Scope | Needs the radio? |
|---|---|---|
| **0** | **Done** — D3XX confirmed universal and user-space, SFP module ordered (§1). Remaining: install D3XX to `/usr/local` (needs sudo). | No |
| **1** | **Done** — `src/rsr200_protocol.h`: block geometry, USB packet geometry, status header, 16/24-bit unpacking, block resynchronisation, all nine PC→radio commands, reply parsing, port/DSP mode bytes, hardware diversity weight packing, Nyquist zone mapping. `test/test_protocol.cpp` checks every documented figure and the manual's worked examples; wired into `core/test/run_tests.sh`. | No |
| **1b** | **Done** — `src/rsr200_device.h`: the transport-agnostic device layer. Configuration ordering, command numbering, acknowledgement and fresh-number retry, embedded reply extraction, sequence-gap detection, sample delivery. Covered by `test/test_device.cpp` against a fake transport. Phase 2 is now mostly plugging in a socket. | No |
| **2** | LAN transport: TCP connect, version query, block resync, UDP reassembly. It implements one interface — `sendCommand` and `nextFrame`. First live IQ. | Yes |
| **3** | Full single-channel control: ADC clock, decimation, attenuators, input switching, 24-bit, Nyquist zone display and spectrum inversion. | Yes |
| **4** | **Done** — dual channel Separate mode + `registerChannels()`. `main.cpp`'s dual-channel checkbox sets port/DSP mode bytes and switch register, plus (the missing piece, see §10) sends channel 2's diversity weight to unity via `Device::setHardwareDiversity(1.0, 0.0, ...)` — without that, ADC2 reads as a clean zero regardless of everything else being correct. Confirmed live in the real app: both channels alive, phasing and decorrelation nulling local signals by more than 30 dB. | Yes |
| **5** | UDP transport for higher rates; block reassembly and loss reporting. | Yes |
| **6** | **Done and verified on Windows; compiles on Linux/macOS, not yet run there.** `src/transport_usb.{h,cpp}` via D3XX: `FT_SetStreamPipe` plus a queue of chunked overlapped reads (several 4096-byte packets per call) kept perpetually in flight, as DP §2.1 recommends. Windows and Linux/macOS ship genuinely different D3XX SDKs — different async-read call name (`FT_ReadPipeEx` vs `FT_ReadPipeAsync`), different blocking-write signature (`LPOVERLAPPED` vs a millisecond timeout) — abstracted behind two small wrapper functions so `rsr200_device.h` and `main.cpp` stay platform-agnostic; verified by downloading and diffing FTDI's actual Linux and macOS SDK headers rather than guessing (they're identical to each other, both genuinely different from the Windows one). CMake links `/usr/local/{include,lib}` on non-MSVC, matching FTDI's own install instructions and how the Mac side already had it installed for phase 0. Windows path verified against real hardware: 0.00% packet loss sustained, `test/test_usb_live.cpp`. Needed a full radio power cycle to get a proper SuperSpeed link — see ENGINEERING_NOTES.md §4. The Linux/macOS path has not been build- or run-tested on those platforms — only checked line-by-line against the real headers on a Windows machine with no Linux/macOS toolchain available. `main.cpp` wires it into a working single-channel SDR++ source module. | Yes |
| **7** | Extras: hardware diversity mode, antenna control (RLA4/RFA2/RAP), GPS correction display, Auto-ATT UI, serial (`SerL`/`SerU`) modes. | Yes |

Phase 1 is worth doing properly and can start immediately: the byte layouts are fully
specified in the documents, so the parser and the command builders can be written and
tested before the radio arrives. That is the same approach that made phases 0–5 of the
phasing work possible without hardware, and it applies just as well here.

**Explicitly out of scope: firmware update** (DP §3.4). The document warns that a failed
update leaves the device in an undefined state recoverable only with a Xilinx Platform
Cable II and the manufacturer's golden image. There is no reason for an SDR program to
carry that risk.

---

## 10. Open questions

- **Maximum decimation: 16 or 64?** (§2) — the two documents disagree, and it sets the
  lowest usable sample rate.
- **Are embedded LAN replies fixed at 8 bytes each?** A block gives a count of the commands
  it carries, and both confirmation forms are 8 bytes, so the device layer assumes a fixed
  stride. The document never says so outright, and the standalone LAN version reply is 12
  bytes with a length prefix — if embedded replies are length-prefixed too, the stride is
  wrong. Contained to one function, but worth checking against a real block early.
- **Does `Set data transmission` need the stream stopped on LAN?** DP §3.3 says switching
  LAN mode automatically stops streaming and requires a fresh `Start stream` with a matching
  size code; the exact ordering wants confirming against the device.
- **What happens to the LAN stream when the TCP client disconnects abruptly?** Only one
  client is permitted; whether a stale server-side connection blocks reconnection after a
  crash affects how defensive the connect path must be.
- **Does 24-bit dual channel really halve the sample count per block** (§3.2), and is the
  effective sample rate therefore halved, or is the block rate doubled? The document states
  the block layout but not the timing.
- ~~ADC2/HF2 produces no data in dual-channel "Separate" mode over USB~~ **RESOLVED.** The
  investigation is worth keeping in full because of how it unfolded, not just the answer.
  `test/test_usb_dual_live.cpp` first found channel B reading a clean, exact zero on every
  packet, reproducibly, from a fresh power cycle, with port mode and DSP mode bytes confirmed
  bit-correct against DP §3.3 and every command's acknowledgment confirming the exact value
  sent. A swap-bit test (the dead slot moved with whichever logical channel the swap bit
  assigned to ADC2, both directions, reproduced from a fresh power cycle) led to the wrong
  conclusion that ADC2's hardware was dead — disproven when a recording made with Reinhard
  Weiss' RSR200 Recorder, confirmed running over USB (LAN is not yet functional on this radio
  at all, §1), showed both channels alive and strong for a full 255-second capture (verified
  byte-for-byte against the file's Linrad dual-channel header with
  `test/test_linrad_recording.cpp`). That left a real, narrower question: what does a working
  USB command sequence send that ours didn't? **Reading the DP and OM in full, not just
  grepping them, found it.** OM §6.2 (a changelog entry for a years-old bug-fix version, not
  the main feature description) states outright that channel 2's diversity magnitude/phase
  weight (DP §3.3's "Set frequency generators", command `0xB0`, selector 9) sits in the signal
  path even in Sep mode, and the official software sets it to unity (magnitude 1.0, phase 0)
  when switching there. DP §4 documents adjustable values as defaulting to zero on power-up.
  We had never sent that command — `Device::setHardwareDiversity()` existed in
  `rsr200_device.h` from phase 1b but nothing ever called it outside the (separate, later)
  hardware-diversity feature. Real ADC2 data multiplied by a zero weight is indistinguishable
  from no data at all, which is exactly what every prior test measured. Added the call to
  `main.cpp`'s `start()` (unity weight, sent whenever `format.channels == 2`) and confirmed
  live in the real app: both channels alive, phasing and decorrelation nulling local signals
  by more than 30 dB, exactly as designed. Nothing about the earlier evidence was wrong — the
  swap-bit test correctly showed the fault tracked *ADC2*, it just couldn't distinguish
  "ADC2 is broken" from "ADC2 needs a command we never send," and only an independent working
  implementation, followed by reading the primary documentation cover to cover, could.

## 11. Errata noticed in the documents

Worth keeping a list, both to avoid re-deriving them and to send back to the manufacturer.

- DP §2.2.1.3 is headed "TCP block structure at dual-channel mode 24 bits" but its table
  reads "TCP block 1-channel 24 bit" and describes 130560 single-channel samples. §2.2.1.4
  carries the same heading and *is* the dual-channel layout. The §2.2.1.3 heading is wrong.
- DP §2.2.1.2's table labels the sample at offset 1044474 as "quadrature data 130560th
  sample channel 2" where the surrounding rows make clear it is channel 1.
- OM §5.1's decimation maximum (16) contradicts DP §3.3 (64); see §2 above.
- OM §5.3 gives the default IP as 192.168.1.10 while DP §2.2 gives 191.168.1.10 — the
  latter is a typo (191.168/16 is not private address space), but worth knowing which the
  firmware actually uses before hunting for a radio that will not answer.
