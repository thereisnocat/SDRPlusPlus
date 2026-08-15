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

**2026-08-06: USB confirmed unusable on this Mac specifically, not a radio or cable
problem.** With the RSR200 connected, `FT_CreateDeviceInfoList` consistently reports zero
devices, but the system log tells a more specific story than "not detected": macOS *does*
see the FT601Q attach — logged by `icdd` as `[USB][ FTDI SuperSpeed-FIF]`, vendor-specific
class `(ff,ff,ff)`, the right signature for this chip — and it stays enumerated for almost
exactly one second before dropping, every time. Ruled out one variable at a time, each with
a real retest rather than assumed: two different USB cables, two different Mac ports
(confirmed genuinely different via the USB location ID changing between attempts, not a
stale log entry), and the radio's power source, already on a linear supply used
successfully for other radios. `FT_SetVIDPID(0x0403, 0x601F)` was tried in case the OEM
unit used a non-default product ID — `0x601F` turned out to already be the header's own
documented default for the FT601, so this was never the cause. Decisive cross-check: the
same radio, same cable, enumerates cleanly on a Windows machine and shows correctly in
Device Manager as "FTDI SuperSpeed-FIFO Bridge" with FTDI's driver bound. That rules out
the radio's USB interface entirely — what's left is this Mac's USB 3.0 SuperSpeed stack
specifically failing to sustain the FT601Q's link past about a second, most likely an
Apple Silicon USB-C/XHCI compatibility quirk rather than anything a cable or port swap can
fix. Phase 6 is blocked on this Mac until that is resolved or a different Mac/adapter path
is found; it is not blocked on the radio, and USB development can happen on the Windows
machine where the hardware is already known to work. LAN (Phase 2) depends on none of this
and is the immediate next step.

**Worth retrying before that conclusion is treated as final.** The Windows session's own
Phase 6 work (ENGINEERING_NOTES.md §4) independently found this exact radio's USB
SuperSpeed link can get stuck — there, downgraded to Hi-Speed rather than dropping outright
— in a way that no cable or port change cleared, only **a full power cycle of the radio
itself**. That was never tried here: every retest above changed the cable, the Mac port, or
confirmed the power *supply*, but the radio was never fully powered off and back on while
connected to this Mac.

**2026-08-09: reversed — root cause was the D3XX driver version, not the Mac's USB stack.**
The "Apple Silicon USB-C/XHCI compatibility quirk" conclusion above was wrong. Before a
power cycle was tried, the radio's owner downgraded the installed D3XX driver from 1.1.8 to
1.1.6 (confirmed via `otool -L`/`md5` against `libftd3xx.1.1.6.dylib`, byte-identical). With
1.1.6 in place, `FT_CreateDeviceInfoList` reports the device stably rather than dropping
after ~1 s, and — a stronger test than enumeration — `FT_Create` opens a real handle that
was held open and re-checked every 2 s for a full 20 s window before closing cleanly, no
drops at any point. The radio was never power-cycled in this test; the driver downgrade
alone explains the fix. 1.1.8 evidently has a genuine regression against this chip/host
combination that 1.1.6 does not share — this was a driver bug, not a hardware limitation of
this Mac, so a different Mac or adapter path is not needed after all.

Not yet proven: this is open-and-hold, not sustained high-throughput streaming. The actual
`FT_SetStreamPipe`/`FT_ReadPipeEx` queued-read path `transport_usb.cpp` depends on has not
been exercised against the radio on the Mac yet — that is the next real test before calling
Phase 6 done here, not just enumeration/open. `FT_SetVIDPID` is also currently unavailable
at 1.1.6 (`nm` shows no `_FT_SetVIDPID` symbol in this dylib version, unlike 1.1.8) — not
needed for the fix above since the default VID/PID already matched, but worth knowing if
anything later reaches for it on macOS specifically.

**2026-08-09, same day: Phase 6 proven live on the Mac — real IQ, spectrum, signals
received.** Getting there needed three more fixes, none of them the radio's fault:

1. **Bare install name on the vendor dylib.** `/usr/local/lib/libftd3xx.dylib`'s own
   `LC_ID_DYLIB` was just `libftd3xx.dylib`, no path — so anything linking against it (our
   module) inherited that same bare reference, and `dlopen` couldn't find it outside the
   directory it happened to be built in. Fixed at the source: `install_name_tool -id
   /usr/local/lib/libftd3xx.dylib /usr/local/lib/libftd3xx.dylib` on the installed driver
   itself, so every future link against it — not just this one module — gets a resolvable
   absolute path automatically. Confirmed: a completely fresh `rsr200_source` rebuild after
   this needed no manual patching.
2. **`install_name_tool` invalidates the code signature it touches, and macOS kills on
   sight.** Step 1 above left `libftd3xx.dylib`'s signature broken rather than absent, and
   loading a dylib with an *invalid* (not just missing) signature is a hard `SIGKILL` from
   the kernel — confirmed via the crash report: `CODESIGNING` / `Invalid Page` /
   `EXC_BAD_ACCESS`, at `dlopen` inside `ModuleManager::loadModule`. Fixed with an ad-hoc
   re-sign: `codesign -s - -f /usr/local/lib/libftd3xx.dylib`, and the same for the module
   dylib itself after any local `install_name_tool` edit to it.
3. **The real bug: `FT_ReadPipeAsync` takes a logical FIFO channel (0-3) on Linux/macOS, not
   the raw USB endpoint address.** Once the module loaded, Start failed immediately with
   `FT_INVALID_PARAMETER` (status 6) from the very first queued read. `FT_SetStreamPipe`,
   `FT_ReadPipe`, `FT_WritePipe`, `FT_FlushPipe`, `FT_AbortPipe` all take the raw endpoint
   byte (`0x82` for this device's bulk IN pipe) and all worked fine with it. But
   `FT_ReadPipeEx`/`FT_ReadPipeAsync`/`FT_WritePipeEx`/`FT_WritePipeAsync` are different —
   the header's own doc comment on exactly those four calls says `ucFifoID ... Valid values
   are 0-3`, a different identifier space entirely. Confirmed by direct test rather than
   inferred from the comment alone: a standalone probe
   (`d3xx_readpipe_probe.c` in scratch) called `FT_ReadPipeAsync(h, 0x82, ...)` →
   `FT_INVALID_PARAMETER`, then `FT_ReadPipeAsync(h, 0, ...)` → `FT_IO_PENDING`, completing
   normally. `transport_usb.cpp`'s non-Windows `queueOverlappedRead` now converts via
   `(endpointAddress & 0x0F) - 2` before calling `FT_ReadPipeAsync`, scoped to just that one
   wrapper — `FT_SetStreamPipe` and the plain `FT_WritePipe` used for commands are untouched
   since they already worked with the raw address. This is a genuine platform divergence
   from Windows, not a guess: Windows' `FT_ReadPipeEx` takes the raw endpoint directly and is
   already verified there (0.00% packet loss, `test/test_usb_live.cpp`), so the two D3XX SDKs
   disagree on this parameter's meaning for the exact same call family.

After all three fixes: the RSR200 Source module loads, initializes, opens the radio, and
streams — confirmed by direct observation (spectrum and signals visible), not just clean
logs. Phase 6 is genuinely done on this Mac now, not just on Windows. Not yet measured here:
sustained throughput / packet-loss numbers over a long run, the way Windows has
(`test_usb_live.cpp` hasn't been run on macOS). `FT_SetVIDPID`'s absence at 1.1.6 (noted
above) remains unaddressed but still unneeded.

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
| **2** | **Transport now wired into the actual module, 2026-08-12 — not yet run against the radio.** `src/rsr200_lan_transport.h`'s `LanTcpTransport` (TCP connect, block resync from an arbitrary byte stream, command send; `test/test_lan_transport.cpp` proves it against a synthetic loopback server) had existed since before this date but was never reachable from `main.cpp` — no UI, no way to select it. Now wired in properly: a "Transport" combo (USB / LAN TCP) in `menuHandler()`, a "Radio IP address" text field shown only for LAN, `start()`/`stop()` branch on the selection and hold whichever concrete transport is active behind a single `Transport* activeTransport`, and both of `main.cpp`'s ad-hoc direct-transport calls (the version-query probe, the Stop Stream command) now go through `activeTransport->isLan()`/`streamPort()` instead of hardcoding USB framing. `Device` itself needed zero changes — it was already fully transport-agnostic internally. Getting to this point involved a real networking side-quest (see the section below): the radio's LAN interface turned out not to be joined to the network at all until power-cycled, and it landed on a DHCP-assigned address, not the documented static default — worth a DHCP reservation for the radio's MAC before relying on this long-term. **First live IQ over LAN achieved 2026-08-12** — real streaming blocks received, correct documented size, sync words and inverted-counter check passing on every block — but the connection sequence itself doesn't yet match the radio's documented requirements; see "First live LAN connection" below for what's real and what's still needed before this is actually usable. | No for what's built; yes for what's left |
| **3** | Full single-channel control: ADC clock, decimation, attenuators, input switching, 24-bit, Nyquist zone display and spectrum inversion. | Yes |
| **4** | **Done** — dual channel Separate mode + `registerChannels()`. `main.cpp`'s dual-channel checkbox sets port/DSP mode bytes and switch register, plus (the missing piece, see §10) sends channel 2's diversity weight to unity via `Device::setHardwareDiversity(1.0, 0.0, ...)` — without that, ADC2 reads as a clean zero regardless of everything else being correct. Confirmed live in the real app: both channels alive, phasing and decorrelation nulling local signals by more than 30 dB. | Yes |
| **5** | UDP transport for higher rates; block reassembly and loss reporting. Its own transport, `KIND_LAN_UDP`, alongside the TCP one built in Phase 2 rather than replacing it — DP §4.2 has commands go over TCP even when the IQ stream itself is UDP. | Yes |
| **6** | **Done and verified on Windows, and now proven live on macOS too (2026-08-09) — spectrum and signals received on the Mac over real USB.** `src/transport_usb.{h,cpp}` via D3XX: `FT_SetStreamPipe` plus a queue of chunked overlapped reads (several 4096-byte packets per call) kept perpetually in flight, as DP §2.1 recommends. Windows and Linux/macOS ship genuinely different D3XX SDKs — different async-read call name (`FT_ReadPipeEx` vs `FT_ReadPipeAsync`), different blocking-write signature (`LPOVERLAPPED` vs a millisecond timeout), **and a third, undocumented-until-now difference: the Linux/macOS `*_Ex`/`*_Async` read/write calls take a logical FIFO channel (0-3), not the raw USB endpoint address Windows and every other pipe call use** — see section 1's 2026-08-09 entries for the full diagnosis. All three abstracted behind small wrapper functions so `rsr200_device.h` and `main.cpp` stay platform-agnostic. CMake links `/usr/local/{include,lib}` on non-MSVC, matching FTDI's own install instructions. Windows path verified against real hardware: 0.00% packet loss sustained, `test/test_usb_live.cpp`. That Windows run needed a full radio power cycle to get a proper SuperSpeed link — see ENGINEERING_NOTES.md §4; on the Mac, the blocker turned out to be a driver version regression (fixed by downgrading to 1.1.6) plus the FIFO-channel bug above, not a power cycle. `main.cpp` wires it into a working single-channel SDR++ source module. Not yet measured on macOS: sustained packet-loss numbers over a long run, the way Windows has. | Yes |
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

## 12. UX issues found in live testing (2026-08-09)

Notes from Ralph's first extended live session against the real radio over USB, once
streaming worked. Recorded as a punch list, not designed or fixed yet.

1. **ADC clock can't be set precisely — fixed 2026-08-10.** The UI advertises 0.1 MHz
   resolution, but the control's per-step jump is visibly larger than 0.1 MHz — the
   slider/drag granularity doesn't match the value granularity it claims.

   Root cause was in the shared `SliderFloatWithSteps` widget
   (`core/src/gui/widgets/stepped_slider.cpp`), not anything RSR200-specific — every module
   using it (HackRF's LNA/VGA gain, AirspyHF's attenuator, USRP/PlutoSDR/RFspace/Perseus/Soapy
   gains, and this ADC clock control) shared the same bug, just less visibly on controls with
   only a handful of coarse steps. It faked a float slider by driving a real
   `ImGui::SliderInt` over a `[0, (max-min)/step]` index range and passing the *already
   MHz-formatted display string* in as that int slider's `format` argument, purely so the
   drag handle would show "125.0" instead of a raw step number. That works fine for dragging,
   but breaks Ctrl+Click-to-type, ImGui's normal way to enter an exact value: the popup text
   box parses whatever you type back using the *slider's real underlying type* (`int`, the
   step index) — the display-string `format` is only ever used to pre-fill the box, never
   consulted when parsing what you typed into it. So typing an exact value silently landed on
   `v_min + (the number you typed) * v_step` instead of the number itself: on this control
   (70..200 MHz range, 0.1 MHz step), typing "150.0" landed on `70 + 150*0.1 = 85.0`, not
   150.0 — with no error, no clamping to signal anything was wrong, just a confidently wrong
   number. That's what "can't be set precisely" actually was: dragging alone can't reliably
   land on one exact value out of 1300 possible positions crammed into one slider's pixel
   width (an inherent, expected limitation), and the escape hatch for exact entry was silently
   broken, so there was no reliable way to hit an exact frequency at all.

   Fixed by driving a real `ImGui::SliderFloat` on the value itself instead of faking one over
   an int index, then snapping the result to the nearest step after either a drag or a typed
   entry. Ctrl+Click now parses back using the actual float value shown, so typing "150.0"
   sets 150.0 MHz, not 85.0. Fixes the same latent bug in every other module listed above as a
   side effect, verified by rebuilding all of them (`rsr200_source`, `hackrf_source`,
   `airspyhf_source`, `rfspace_source`, `sdrpp_core`) clean.
2. **ADC clock × receiver decimation × software decimation gives a huge but opaque space of
   bandwidth/sample-rate choices.** The three controls compose to determine the effective
   rate, but nothing in the interface shows that relationship. Worth an alternate control
   surface that exposes bandwidth or sample rate directly and derives the three underlying
   parameters, rather than making the user reason through the composition themselves.
3. **Receiver decimation = 2 in 24-bit mode produces choppy audio, even at the lowest clock
   speed.** The manufacturer recommends 24-bit mode, but it isn't practical at the sample
   rates the low decimation settings imply. Whatever alternate interface comes out of item 2
   should surface 24-bit mode's effect on usable bandwidth alongside the rate controls, not
   as a separate, disconnected checkbox.
4. **Wants a small always-visible spectrum window zoomed to the current frequency — designed
   and implemented 2026-08-10, not RSR200-specific.** Grew into a bigger feature than just
   this item once designed: a real dedicated spectrum feed (not reusing the main waterfall's
   view, which was exactly the "limited resolution" this item already flagged) with a shaded,
   draggable, independently-asymmetric passband, living in the Radio module above the mode
   selector. Full design and implementation notes in `RADIO_SPECTRUM_FILTER_PLAN.md` — applies
   to every source, not just RSR200.
5. **Firmware version still displays wrong — not BCD-decoded. Fixed 2026-08-11.** Confirmed
   directly against the real unit: it reports the raw firmware field as the binary value
   0x0225 (549 decimal), and Ralph independently confirmed the true firmware is 225 -- BCD
   digits "0225", not the binary integer. Fixed at the source rather than at display time:
   `rsr200_protocol.h` gained a generic `bcdToDecimal()` helper, applied at both places the
   32-bit firmware field gets parsed (`parseEmbeddedCommand()` for the USB/embedded reply,
   `parseLanVersionPacket()` for the LAN one) -- `main.cpp`'s display code needed no change,
   since it was always just printing whatever `Reply::firmware` already contained. Serial
   number is untouched; the DP describes only the firmware field with the "4 digit
   hexadecimal value" phrasing that signals packed BCD, and the observed serial number (40)
   shows no sign of the same misreading. `test/test_protocol.cpp`'s existing version-report
   checks had encoded firmware "225" as the raw binary byte `0xE1` -- exactly the assumption
   this fix corrects -- so they were updated to a genuinely BCD-packed `0x0225`, plus a new
   dedicated block directly exercising `bcdToDecimal()` itself. Full suite (`core/test/
   run_tests.sh`) passes, 0 failures across all 13 suites. Confirmed live 2026-08-11 once the
   device was reconnected: display now reads "Serial 40, firmware 225" against the real
   device, matching Ralph's independently-confirmed true firmware version exactly.
6. **Resampler predecimation overflow, hit live during clock/decimation testing.** Caught in
   the log during the same live session, right after item 1/2's imprecise-clock and
   opaque-rate-relationship issues would have been in play: a burst of rapid Start/Stop and
   sample-rate changes around 12:14 ended with `[Resamp] predec: -2147483648, interp: 2,
   decim: 1, inacc: 0.000000%, taps: 152` on two consecutive lines, immediately after a
   `Start!`. `-2147483648` is `INT32_MIN` — a signed 32-bit underflow in the predecimation
   calculation, not a plausible real value. This is core SDR++'s resampler code, not
   RSR200-specific, so some reachable ADC-clock/decimation/software-decimation combination
   from items 1-2 produces a degenerate rate that overflows it. The app itself did not crash
   and kept running. Not yet reproduced deliberately or root-caused — worth trying to hit it
   again with a known rate/decimation combination once the alternate rate-control interface
   from item 2 exists, since that would make the triggering combination reproducible instead
   of an accident of a testing sequence.
7. **Baseband recordings past ~4 GB come out unreadable — a real, pre-existing core bug,
   not a reception problem.** A ~16-minute recording at these settings (24-bit, 4-channel
   dual-IQ, 3.90625 MSp/s post-decimation ⇒ ~62.5 MB/s) grew to 80.9 GB and played back as
   silence. Root cause, confirmed by hex-dumping the header: the `data` chunk's declared
   size was a literal `0x00000000`. `core/src/utils/riff.h`'s `ChunkHeader::size` is a plain
   `uint32_t`, which tops out around 4.29 GB — at this recording's rate that ceiling is hit
   in about **65 seconds**, so any recording longer than roughly a minute at these settings
   produces a file whose header can't state its own true size. `wav::Format::FORMAT_RF64`
   (the standard fix for exactly this, using a 64-bit `ds64` chunk) exists as an enum value
   in `core/src/utils/wav.h` but is never actually implemented anywhere in `wav.cpp` —
   `_format` is stored and never read again — and is explicitly commented out in the
   recorder's UI (`misc_modules/recorder/src/main.cpp:60`, `// Disabled for now`). Not
   RSR200-specific, but RSR200's realistic recording rates make it trivial to hit where most
   other sources rarely would. **The actual sample data survives intact** — confirmed by
   reading real, varying, non-zero content from the start of the file through within 320 KB
   of the end — only the size field was wrong. Recovered this specific file by patching the
   RIFF and `data` chunk size fields (offsets 4 and 252) to `0xFFFFFFFF`, the standard
   "unknown length, read to EOF" convention most tools (ffmpeg, Audacity, SoX) honor; that
   is a manual per-file workaround, not a fix. A real fix means either implementing RF64
   properly (the scaffolding is already half there) or having the recorder warn/split files
   as the 4 GB boundary approaches.

   **Second confirmed instance, same day:** a separate 30.4 GB float32 recording
   (`baseband_1021631Hz_20-49-54_09-08-2026.wav`) showed the same corruption from a
   different angle — its `data` size field wasn't `0` but `291816128` (~278 MB), matching
   `real_content_bytes mod 2^32` exactly (`30356587200 mod 2^32 = 291816128`), i.e. the
   32-bit counter had wrapped around **seven times** during the recording. Worse than the
   all-zero case in one way: a strict reader wouldn't see "no data," it would read a
   plausible-looking ~9 seconds of real audio and stop there, discarding the other ~30 GB
   silently rather than failing obviously. Confirms the overflow theory precisely rather
   than just being consistent with it, and recovered the same way (RIFF/`data` size fields
   → `0xFFFFFFFF`).
8. **Baseband recording bandwidth follows software decimation, not the full front-end
   span — probably explains why this same recording looked like it covered only the empty
   gap between 19m and 16m.** Confirmed in code: `misc_modules/recorder/src/main.cpp:181`
   sets the recorder's rate from `sigpath::iqFrontEnd.getSampleRate()`, i.e. the
   *post*-software-decimation rate, not the undecimated front-end bandwidth the display can
   show. With 2x software decimation active, the recorder only ever sees half the spectral
   width around the tuned center frequency that the full front end covers. Tuned between the
   two broadcast bands, that halved window lands mostly in the gap between them, clipping
   the very band edges where the signals were. Not corruption — a real, direct consequence
   of the decimation setting doing what it's designed to do — but it's exactly the confusion
   items 1-2 already predicted, and worth surfacing explicitly (e.g. in the recording
   filename or a UI note) so "software decimation" being applied to recordings, not just the
   display, isn't a surprise discovered after the fact.
9. **`file_source` had no playback pacing at all, and it measurably degraded decorrelation
   quality on replay versus live.** `worker()`/`dualWorker()`/`floatWorker()` were gated only
   by `swap()`'s downstream backpressure — nothing paced the loop to the recording's real
   rate, so blocks were delivered in whatever bursty, CPU-scheduling-dependent pattern
   backpressure happened to allow, unlike live hardware's naturally even cadence.
   `phasing_test_source`'s own generator already paces itself to real time for exactly this
   reason (its own comment: "without this the generator would spin as fast as downstream can
   consume") — `file_source` never got the same treatment. Measured, not assumed: the same
   dual-channel recording nulled 8-10 dB on playback against reference band width and gain
   settings confirmed identical to a live session that nulled 20+ dB; after adding the same
   real-time pacing pattern (`paceToRealTime()`, committed), the same file improved to
   12-15 dB. A real, confirmed contributor — but not the whole gap, since 12-15 dB is still
   short of the ~22-26 dB baseline this method has shown on real air elsewhere in this
   project (see the WNYC 820 wideband-decorrelation writeup, section 1). **Open**: what
   accounts for the remaining ~10 dB. Diagnostics already run and ruled out during this
   investigation, so the next pass doesn't have to re-check them: the reference-band
   auto-tracking fix from earlier the same day (confirmed via direct logging — only 6
   legitimate VFO-driven changes across a full session, no reset-storm recurrence), the
   resampler predecimation overflow from item 6 (no occurrence in the session logs checked),
   dual-channel interleave order (`I1,Q1,I2,Q2`, identical on the write and read side,
   independent of sample format), and the embedded phasing metadata chunk (byte-identical
   between the two recordings compared). Convergence time was also ruled out directly by the
   reporter (played continuously from the start, null was stable after settling).

## 13. Recording needs a sweeping fix, not more per-file patches

Logged as open rather than fixed — explicitly not started. Every recording-related problem
found in this session (2026-08-09) got its own one-off patch: hand-editing a broken header's
size fields, hand-diagnosing which byte width a given file actually used, hand-adding pacing
after the fact. That doesn't scale to "every time a recording is made" and isn't meant to —
each fix was the right call for the specific file or bug at the time, but the pattern across
all of them is the actual signal: the recording subsystem (`core/src/utils/riff.{h,cpp}`,
`wav.{h,cpp}`, `misc_modules/recorder/`, `source_modules/file_source/`) needs one coherent
pass, not more individual patches as each new symptom turns up. What that pass should cover,
gathered from everything found so far rather than re-derived from scratch next time:

- **The 4 GB ceiling (item 7).** `riff::ChunkHeader::size` is `uint32_t`; RSR200 recording
  rates blow through it in about a minute. `wav::Format::FORMAT_RF64` is a stub — the enum
  exists, `_format` is stored, and nothing in `wav.cpp` ever reads it back. Real fix is
  either implementing RF64 properly (`ds64` chunk, per the spec, not the placeholder that's
  there now) or having the recorder split/warn as the boundary approaches. Recovering an
  already-broken file by hand (`0xFFFFFFFF` size fields) is a workaround, not this fix.
- **Recorded bandwidth silently tracks software decimation (item 8).** Correct given how the
  signal path is wired, but surprising and undocumented — worth surfacing in the UI or the
  filename rather than only in this document.
- **`file_source` had no real-time pacing (item 9, now fixed) and no bit-depth
  auto-detection (also now fixed) until this session** — both were silent, `file_source`
  would just play back wrong or slow with no error. Both are done, but they were only found
  because specific files broke in specific ways; nothing structural stops the next gap in
  the same family from being found the same way, one broken recording at a time.
- **The `auxi` chunk's `stopTime` is never actually set.** `misc_modules/recorder/src/main.cpp`
  calls `wavmeta::makeAuxi(freq, samplerate, now, now)` at `start()` — both `startTime` and
  `stopTime` get the same value, and nothing patches `stopTime` when recording actually
  stops, the way the RIFF/data chunk sizes correctly do get patched on close. Any tool that
  trusts this chunk's stop time (this project's own cross-checks during this session
  included) gets a lie by omission, not an error.
- **No end-to-end test coverage for the record → playback round trip.** Every bug in this
  section was found by a human noticing a recording sounded or looked wrong, then a human
  and Claude tracing it by hand. `core/test/` has no test that writes a WAV with `wav::Writer`
  across all four `SampleType`s and multiple channel counts, reads it back with `WavReader`,
  and asserts the samples round-trip exactly — the kind of test that would have caught the
  int32/uint8 gap and the float32 default before a real recording ever hit it.

Not scoped further than this list — the point of this section is to make the next real pass
start from a complete picture instead of the next single symptom.

**2026-08-09, later the same day: scoped into a real plan.** See
[RECORDING_REFACTOR_PLAN.md](RECORDING_REFACTOR_PLAN.md) — RF64 implementation approach
(researched against WavViewDX and SDR Console's actual documented behavior, not assumed),
a phased rollout, and the open questions that need a decision before Phase 1 starts. Not
started; planning only.

## 14. Windows CI never actually builds RSR200 — found 2026-08-10, fixed

The published Windows build (`sdrpp_windows_x64`) has never included RSR200 support, at
all, on any platform, despite the module compiling and working live on both Mac (this
session, 2026-08-09/10) and Windows (the earlier Windows session). Root cause, confirmed by
reading `.github/workflows/build_all.yml` directly rather than guessed: every *other*
optional source module built there (BladeRF, LimeSDR, Perseus, SDRplay, RFNM, FobosSDR,
HydraSDR) is explicitly turned on in the Windows job's CMake configure step —
`OPT_BUILD_RSR200_SOURCE` was simply never added when the module was written. Separately,
`make_windows_package.ps1` also never had an entry to copy `rsr200_source.dll` into the
package at all — the same "silently ships missing" trap this project's own memory already
flags for exactly this reason (it previously happened to `phasing` on both platforms).

**Fixed:**
- `make_windows_package.ps1`: added the missing copy entry for `rsr200_source.dll` and its
  runtime DLL dependency.
- `build_all.yml`: added `-DOPT_BUILD_RSR200_SOURCE=ON` to the Windows job's CMake configure
  line.
- The FTD3XX WinUSB SDK (header, import lib, runtime DLL) the module needs to build against
  is now vendored in-repo at `third_party/ftd3xx_winusb/` and installed by a real
  (non-placeholder) `Install FTD3XX SDK for RSR200` step in `build_all.yml`, rather than
  downloaded live from FTDI at CI time.

**Why vendored instead of downloaded, like every other SDK in this workflow:** FTDI's own
drivers page (`ftdichip.com/drivers/d3xx-drivers/`) sits behind Cloudflare bot-detection that
blocks non-interactive HTTP clients — confirmed against `curl` (even with a spoofed browser
User-Agent) and separately confirmed the same would defeat GitHub Actions'
`Invoke-WebRequest`, since both get served the Cloudflare JS challenge page instead of the
file, even hitting the direct `wp-content/uploads/.../*.zip` URL. Only an actual interactive
browser session got past it. Since a live download in CI would be exactly as unreliable, the
handful of files actually needed (~177KB total) were extracted from the real SDK and
committed directly instead. See `third_party/ftd3xx_winusb/NOTICE.md` for exact provenance
and the redistribution license terms.

**A real trap hit along the way, worth flagging for next time:** the URL initially supplied
(`FTD3XXDriver_WHQLCertified_v1.3.0.10.zip`) looked plausible — WHQL-certified, x64, recent —
but turned out to be FTDI's *other*, older Windows D3XX package: a WDF-based kernel driver
(`ftdibus3.sys`/`.inf`, no `FTD3XXWU` import library at all), which FTDI's own page marks
"soon to be deprecated". The module needs the *WinUSB* package specifically
(`Winusb_D3XX_Release_1.4.0.1.zip`, marked "(Recommended)" / "*Latest Windows WinUSB based
driver" on the same page) — confirmed by actually extracting both zips and comparing their
contents rather than trusting either filename. `CMakeLists.txt`'s own comment already named
the right package ("FTDI's Winusb_D3XX_Release package"); worth reading that literally next
time instead of taking a supplied URL at face value.

Local Windows builds (the earlier Windows session's own dev machine) were never affected by
any of this — they already had the SDK installed locally; this was CI-only.

## 15. Demod IF sample rates can produce pathological resampler filters against RSR200 — found 2026-08-10, not fixed

Found while debugging an unrelated Radio-module CPU/memory blowup (full writeup in
`RADIO_SPECTRUM_FILTER_PLAN.md`'s "Regression found and fixed in live testing" section) — this
specific issue is real but distinct, RSR200-relevant, and left for later rather than fixed
alongside that one.

Every `demod::X` class hardcodes its own fixed IF sample rate with no awareness of what rate
the actual source produces (`demod::AM::getIFSampleRate()` returns a flat `15000.0`, same
pattern for NFM/DSB/USB/etc.). `dsp::multirate::RationalResampler` reduces
`outSamplerate/inSamplerate` to lowest terms and sizes its polyphase FIR off the result — fine
when that ratio is "nice," but RSR200's actual sample rate (driven by ADC clock ÷ decimation,
neither of which is chosen with resampler-friendliness in mind — e.g. observed live at
~1.95MHz) doesn't share a large GCD with a flat `15000`. Confirmed live: selecting AM against
a real RSR200 logs `[Resamp] predec: 64, interp: 15000, decim: 15259, ..., taps: 1159667` —
1.16 million filter taps, three times during startup/mode-provisioning. Each occurrence is a
real, multi-second CPU spike building that filter (confirmed via `sample`-ing the running
process), though it only happened at startup in the session where this was found, not
continuously — unlike the Radio spectrum preview's own version of this same failure mode,
which *was* continuous and is what actually prompted this investigation.

Not fixed here: the fix that worked for the spectrum preview (snap the requested rate to a
power-of-two decimation of the source's actual rate, so `RationalResampler`'s own
predecimation stage absorbs the whole ratio and the polyphase-filter path never runs at all)
doesn't obviously transfer to demod IF rates, which are fixed per-mode constants baked into
each `demod::X::getIFSampleRate()` and presumably chosen for other reasons (audio quality,
filter design assumptions downstream in each demodulator) rather than picked freely the way
the preview's width was. Whether it's safe to snap those too, or whether RSR200's own
`adcClockMHz`/decimation controls should instead be steered (or at least warned) away from
rates that don't divide nicely against common demod IF rates, needs its own look rather than
a quick copy of the preview's fix.

## Live-testing feedback (2026-08-11) -- VHF preamp doesn't activate

Reported: "Selecting Use VHF input (else HF1) and VHF preamp does not activate the VHF
preamp. I've tested this with HDSDR and the Reuter RSR200 EXTio module and it works there,
so I know the preamp is properly connected to the radio."

Confirmed live against the real device (connected via USB, shows up generically as an
"FTDI SuperSpeed-FIFO Bridge") with a temporary diagnostic that logged the actual bytes
`applyConfig()` constructs and sends for `VAR_SWITCH`: the correct value was always being
built -- `0x0082` for VHF input + preamp both on (bit 1 + bit 7), matching the protocol
exactly. So the bug was never about *what* gets sent, only about *when*.

Root cause: `Device::configuredOnce`, which gates whether the ADC clock and data-
transmission commands get resent on `applyConfig()`, is set `true` on the first successful
configuration and was **never reset** on `stop()`. `applyConfig()`'s own comment already
documents why that matters: "Changing the ADC clock or the transmission settings triggers a
synchronisation event" -- and `stop()`'s own comment says "Stop Stream closes the USB
endpoint entirely... the next Start has to reopen and reconfigure from scratch either way."
But because `configuredOnce` stayed `true` across a stop/start within the same process, a
same-session restart (exactly how a user has to test these particular checkboxes, since
they're disabled while running) would see `clockChanged`/`formatChanged` both false --
nothing else changed -- and skip resending the clock and transmission-format commands
entirely, sending *only* the switch register on its own. The radio apparently needs the
preceding synchronisation event those two commands trigger for the switch register's
relay-affecting bits (VHF routing, preamp) to actually take hold; sent alone, the same
correct value doesn't reach the relays.

Reproduced and verified the mechanism directly with a second diagnostic logging
`configuredOnce`/`formatChanged`/`clockChanged` on every `applyConfig()` call:

    First start (fresh process):  configuredOnce=0 -> clock=1 transmission=1 (correct)
    Same-session restart (before fix): configuredOnce=1, nothing else changed
                                        -> clock=0 transmission=0 (bug: switch sent alone)
    Same-session restart (after fix):  configuredOnce=0 -> clock=1 transmission=1 (fixed)

Fixed by resetting `configuredOnce` (and `pending`/`streaming`/`expectSequence`, the same
class of stale per-session state) inside `Device::setTransport(nullptr)` -- the existing
signal `stop()` already sends when it tears the transport down, so no change to `main.cpp`
was needed. Every fresh `start()` now gets the full reconfiguration sequence again, matching
what `stop()`'s own comment already said should happen.

Not independently confirmed by ear/eye that the preamp is now audibly/visually stronger --
the antenna in use during this session showed very weak FM broadcast reception generally
(flat noise floor, no clear stations, regardless of preamp state), which made a clean before/
after signal-level comparison unreliable. The structural fix is well-evidenced on its own
(exact reproduction of the reported symptom's trigger condition, a comment already in the
code stating what should happen, and confirmed correct command sequencing before/after), but
worth Ralph's own ears/HDSDR-style comparison to close the loop.

Rebuilt clean, redeployed to all three locations, relaunched, confirmed running without
crashing.

## Live-testing feedback, continued (2026-08-11) -- preamp still not activating; primary source re-verified

Reported: "The preamp is still not activating. It gives a visual indication on its screen
when it's working. I verified that it HDSDR. It does not happen here." -- i.e. the radio's
own front-panel indicator, not just SDR++'s own display, is the ground truth, and it
confirms the fix above did not resolve the actual problem.

Went back to the primary source rather than this plan doc's own transcription of it --
`RSR200_DP_ENG_V52.pdf`, found locally in `~/Downloads/Reuter RSR200 documents/`. Confirmed:

- **Bit assignment is exactly right.** DP 3.3's "Set variable 16 bit value" table, variable
  5 ("Switch"): "Bit 7: Preamplifier VHF, 0 = off, 1 = on" -- matches
  `SW_VHF_PREAMP = 1 << 7` in `rsr200_protocol.h` precisely. Rules out a transcription error
  in the bit position.
- **Firmware version confirmed 225, not 549** -- Ralph identified `verFirmware` as printed
  (549) is a decimal misreading of a BCD-encoded 0x0225; the real firmware is 225, exactly
  the version this DP (v0.52) was written against. Rules out a firmware-version protocol
  mismatch (already flagged as a known, unrelated display bug in the punch list, item 5).
- **Found the actual missing piece, DP 4.1**: "After sending a command block, you must wait
  for confirmation of the last command... before new commands can be sent." `applyConfig()`
  has never done this -- it fires up to five separate 0xF5/0xF2/0xB4 commands back-to-back
  with nothing reading the transport to observe any embedded confirmation in between, let
  alone waiting for one.

Tried fixing that gap two ways:

1. **Re-send VAR_SWITCH again after Start Stream**, on the theory that the switch register
   might only be honored once streaming is actually underway. Tested live, checked the
   radio's own front-panel indicator directly -- no change, preamp still doesn't light.
   Reverted (see radio_module -- sorry, `main.cpp`'s `start()` -- diff; removed cleanly).

2. **A fixed `sleep_for()` pacing delay between each command** in `applyConfig()`, since a
   true wait-for-the-real-confirmation loop was judged too risky to add here: the USB read
   it would need to pump (`Transport::nextFrame()`) is configured with no pipe timeout
   (`FT_SetPipeTimeout(..., 0)`) and runs synchronously on whatever thread calls `start()`,
   before the worker thread that normally drains frames even exists -- if streaming ever
   stalled during that wait, the call would hang forever instead of failing cleanly. A fixed
   sleep looked like the safe compromise. **It was not safe**: reproducibly broke Start
   Stream outright on the real device ("Last error: Start Stream failed"). Best working
   theory: DP 4.1 says USB streaming auto-starts right after power-up into a small
   (4096-byte double-buffered) transmit FIFO -- sleeping without ever reading
   `Transport::nextFrame()` just lets that already-unread IN pipe back up further before the
   next OUT write, rather than actually helping. Reverted immediately once confirmed live;
   `rsr200_device.h`'s `applyConfig()` is back to firing all commands with no gap, exactly as
   before this round, plus a comment recording why a bare sleep here doesn't work so a future
   pass doesn't retry the same broken idea.

**Status: not yet resolved.** What's now been ruled out, with reasonable confidence: wrong
bit position, wrong command format, wrong firmware-version assumptions, the `configuredOnce`
reset bug (real, fixed, confirmed via diagnostic -- but not sufficient on its own), sending
the command at the wrong point relative to Start Stream. What's still open: DP 4.1's
documented wait-for-confirmation requirement is real and unimplemented, but implementing it
safely needs the confirmation to be observed by *reading* the stream during the wait, not
just pausing -- which means either (a) draining frames during the pause instead of sleeping
blindly, or (b) a larger change moving `applyConfig()`'s command sequence into the worker
thread's own already-running `pump()`/`service()` loop as a small state machine, so waiting
for a real ack no longer risks blocking the caller. Neither attempted yet after the sleep
regression; worth trying (a) first, next round, as the smaller change.

Also worth requesting from Ralph directly next round, since code archaeology and safe live
experimentation have both been exhausted without resolving this: a raw USB packet capture
from the Windows/HDSDR session (e.g. Wireshark + USBPcap) showing the actual byte sequence
and timing HDSDR's ExtIO module uses when the preamp successfully engages, to compare
directly against SDR++'s sequence rather than continuing to infer timing requirements from
the DP's prose alone.

Reverted cleanly, rebuilt, redeployed to all three locations, relaunched, confirmed running
without the Start-Stream regression.

## VHF preamp -- root cause found and fixed, confirmed live (2026-08-11)

Resolved via a USB packet capture Ralph took of HDSDR's ExtIO module actually engaging the
real preamp (`HDSDR-RSR200-VHF-Preamp.pcapng`, ~557 MB, 17134 packets). Parsed directly with
scapy (`pip3 install --user scapy`; no Wireshark/tshark available, so raw USBPcap URBs were
walked by hand -- the first 2 bytes of each captured packet are the USBPcap header length,
27 in every case here, so payload = `raw[27:]`, and the RSR200's own command structure starts
right there unchanged: 4-byte command number, then the documented byte layout). Filtered for
the `0xF5` (Set Variable) instruction byte across the whole capture -- only 8 such commands
in the entire session:

    t+0.000s  var=3 (antenna ctl HF1/VHF)  value=0x0000   -- startup init
    t+0.004s  var=4 (antenna ctl HF2)      value=0x0000   -- startup init
    t+0.009s  var=1 (attenuator ADC1)      value=0x0000   -- startup init
    t+0.013s  var=2 (attenuator ADC2)      value=0x0000   -- startup init
    t+0.017s  var=5 (switch)               value=0x0001   -- startup init (bit 0 only)
    t+0.035s  var=5 (switch)               value=0x0001   -- duplicate/retry
    t+15.384s var=5 (switch)               value=0x0003   -- bits 0+1: VHF input selected
    t+21.660s var=5 (switch)               value=0x001B   -- bits 0+1+3+4: preamp engaged

The one command that actually engages the preamp adds bits 3 and 4 to the switch register --
**not bit 7**, which is what `SW_VHF_PREAMP` (and this project's own reading of DP 3.3's
table, "Bit 7: Preamplifier VHF") had been sending for the entire investigation. Bits 3+4 are
documented as "Remote power supply HF1/VHF": bit 3 on/off, bit 4 plain +12V vs RS-232
"Control" mode -- nominally meant for powering an *external* active antenna accessory
(RLA4/RFA2/RAP, DP 4.4), not an internal preamp. The straightforward reading: on this
hardware, the VHF preamp module is wired and powered through the same rail an external
remote-powered accessory would use, rather than through a separately switched internal
circuit -- so bit 7 may simply be inert on this unit/firmware regardless of what DP 3.3's
table says it should do. (Also notable, though unrelated to the bug: HDSDR always sets bit 0,
ADC2 CLK inverted, even in its very first startup command before VHF/preamp are touched at
all -- an idle default of theirs, not something tied to this control. Left alone; nothing
observed depends on it.)

Fixed in `main.cpp`'s `buildConfig()`: `vhfPreamp` now sets `SW_REMOTE_PWR_CH1 |
SW_REMOTE_CTRL_CH1` (bits 3+4) instead of `SW_VHF_PREAMP` (bit 7), matching the captured
sequence exactly (`SW_ADC1_TO_VHF | SW_REMOTE_PWR_CH1 | SW_REMOTE_CTRL_CH1` = 0x1B, byte-for-
byte identical to HDSDR's own command). Verified live against the real hardware -- Ralph
confirmed directly on the radio's own front-panel display, not just SDR++'s spectrum: **the
preamp indicator is now active and shows the correct text.**

`configuredOnce` fix from earlier in this investigation stays in place -- both were real bugs
independently required for correct behaviour (the earlier one so the full command sequence
gets resent on every restart, this one so the resent commands actually mean the right thing).

Rebuilt clean, redeployed to all three locations, relaunched, confirmed running and the fix
active. This closes out the preamp investigation.

### Related, found by Ralph during this same confirmation pass: VHF spectrum reads inverted -- fixed

Tuning to a known station by RDS while on VHF input showed the displayed frequency and the
actual received station disagreeing, and dragging the waterfall's own frequency-range control
moved the spectrum in the opposite direction from HF operation -- both symptoms of a
mirrored/inverted spectrum. Ralph diagnosed and confirmed a workaround himself: checking
"Invert IQ" in the source panel corrects it.

Root cause was already half-fixed in the code and just never wired up: `tuneFor()` (DP's own
Nyquist-zone arithmetic) computes `Tuning::spectrumInverted` correctly on every retune --
true for any even-numbered zone -- but nothing downstream ever read that field. Confirmed this
is *not* simply "VHF inverted, HF1 not": at a 125 MHz ADC clock, 30 MHz (HF1-ish) lands in
zone 1 (odd, not inverted) while 80 MHz (within VHF's 66-150 MHz range) lands in zone 2 (even,
inverted) -- but VHF's own range spans multiple zones of alternating parity depending on the
exact frequency and clock, so a single manual checkbox can't stay correct across a retune the
way a per-block check tied to the actual tuning can.

Fixed in `Device::deliver()`: when `tuning.spectrumInverted` is true, every delivered
sample's Q component is negated (conjugating the signal, the standard correction for a
mirrored spectrum, and the same net effect "Invert IQ" was achieving manually) -- applied
fresh on every block, so it tracks retuning automatically rather than needing the checkbox
touched again. This makes "Invert IQ" redundant for this cause specifically; it's left alone
as a manual override for other reasons someone might still want it (e.g. genuinely
reversed antenna wiring), and it stays *off* in the persisted config from this session, so
there's no double-negation to worry about.

Added six new checks in `test/test_device.cpp` ("Spectrum inversion follows the current
tuning, not a fixed setting") exercising exactly this: one `Device` instance, tuned first to
30 MHz (zone 1) then 80 MHz (zone 2) then back to 30 MHz, with a known synthetic I/Q sample
poked into each block, checking Q comes through unchanged/negated/unchanged across the three
retunes. Full suite (`core/test/run_tests.sh`) passes, 0 failures across all 13 suites, before
ever touching the real hardware. Rebuilt, redeployed to all three locations, relaunched,
confirmed running with a full, correctly-aligned FM broadcast band visible across 90-104 MHz
on VHF input.

## LAN interface: finding the radio on the network, and wiring in the transport (2026-08-12)

Started from Ralph reaching out to the manufacturer about the SFP module and the LAN LED;
their answer was that a blinking green LED just means "network connection working." That
turned out to be a much lower-level claim than it sounds like -- see below.

**The chase.** An `nmap -Pn -p 55557,55558 192.168.1.10` result reported both ports, and was
read as "the radio is online." Several rounds of independent verification all disagreed with
that:

- A stale *reject* route cached on Ralph's laptop for `192.168.1.10` (flagged `UHRLWI` in
  `netstat -rn` -- the `R` is reject) made every connection attempt fail before ever touching
  the network. Cleared with `sudo route delete -host` + `sudo arp -d`; genuinely clean
  attempts afterward still got no ARP reply at all.
- The router's own connected-devices list showed nothing at `.10`.
- A clean, cache-cleared retest from the desktop Mac (the one that ran the original scan)
  came back identical -- no ARP reply, both ports timing out.
- The original scan itself, reread carefully, never actually showed what it was taken to
  show: both ports were `filtered`, not `open` -- nmap's term for "got no response, can't
  tell open from closed," most consistent with nothing being there to respond at all -- and
  `-Pn` skips host-discovery entirely, so "Host is up" in that output was never a real
  check, just nmap taking the flag's word for it. The likely actual explanation: that scan
  was run *before* Ralph moved his desktop Mac off `.10` to free the address for the radio,
  so it was almost certainly seeing the desktop's own ports, not the radio's.

**Root cause: the LED lied, in the specific way LEDs on physical-layer transceivers always
can.** "Blinking green" almost certainly just meant the SFP transceiver had a physical link
with the switch -- a lower layer than "joined the network," which needs a completed DHCP
negotiation (or a working static-IP fallback) to mean anything. A link light coming up
requires none of that. Confirmed by power-cycling the radio and doing a full subnet
discovery scan (`sudo nmap -sn 192.168.1.0/24`) rather than continuing to guess at one
address: the radio appeared at `192.168.1.176` -- a DHCP-assigned address, not the documented
static default `192.168.1.10` -- with `55557/tcp open` (matching the documented TCP command
port exactly) and `55558/tcp closed` (also correct -- that port is UDP per DP §3.3/RSR200_
PLAN.md §3.3, and a TCP probe against a UDP-only port getting a clean RST rather than a
timeout is exactly what a live, responsive host should do). The MAC's OUI (`00:0A:35`)
resolves to Xilinx, consistent with this being genuinely FPGA-based hardware rather than a
coincidence.

**Practical follow-up, not yet done:** the radio's address is DHCP-assigned and therefore not
stable across lease renewal or the next power cycle. A DHCP reservation for its MAC in the
router (or configuring a static IP on the radio itself) would keep this from moving again.

**Transport wiring landed the same day** — see phase 2's table entry above for what changed
in `main.cpp`. Not yet tested: actually connecting SDR++ to the radio over LAN and confirming
live IQ, the actual next milestone.

## First live LAN connection, and the real gap it found (2026-08-12)

Once the radio was reachable (previous section), a standalone smoke test was written --
`test/test_lan_live.cpp`, the LAN-side equivalent of `test_usb_live.cpp` but going through
the `Device` layer instead of raw frame counting, since that's the actual code path
`main.cpp` uses. **It connected and streamed real data on the first real attempt**: TCP
connect succeeded, `applyConfig()`/`startStream()` were accepted, and 80+ blocks of the
documented exact size (522704 bytes, 130560 samples) arrived over several seconds, with
plausible near-zero sample values (10 MHz, no strong local signal there).

Two things looked wrong and needed real diagnosis rather than another guess:

- **Sequence gaps on almost every block.**
- **The version-query reply, and one config command's acknowledgement, never arrived** --
  "no acknowledgement for command 176" (176 = 0xB0, `cmdSetLoBoth`'s instruction byte).

A raw byte dump (bypassing `Device`, reading `LanTcpTransport::nextFrame()` directly and
hex-dumping the trailer of each block) ruled out a framing bug conclusively: the sync words
matched exactly, and `invCounter` was the exact bitwise complement of `counter` on every
single block -- both would essentially never happen by coincidence if resync were finding
the wrong byte offsets. So the blocks themselves are being found and parsed correctly; the
*counter values* just don't behave like a plain "+1 per block" sequence (they jittered and
occasionally went backward -- e.g. 9080, 9077, 17500, 17506, 17514, 17503).

**Reading the actual manufacturer PDF (`RSR200_DP_ENG_V52.pdf`, not just this plan doc's own
transcription of it) found the real cause, in section 4.2's documented connection procedure
for LAN/TCP:**

> "Command processing should be carried out according to the back and forth principle...
> (Command → Confirmation → Next command...)."

Before "Start stream" is ever sent, the radio is in **packet mode** (DP §3): every command
(Read version numbers, Set data transmission, Set ADC clock, Set generators, Set variable,
...) gets an individual reply -- an 8-byte "Confirmation" or "Special confirmation" packet
(DP §3.2), or for the version query specifically a 12-byte "Version numbers LAN" packet --
sent as its own standalone TCP write, *not* embedded in a streaming block. Only once
streaming has actually started does reply data move into the embedded-commands area at the
end of each block instead.

`Device::applyConfig()` sends up to six commands (ADC clock, data transmission, switch,
attenuator ×2, LO/tune) back-to-back via a fire-and-forget `send()`, and nothing ever reads
anything back until `pump()` starts being called -- which only happens *after* `startStream()`
has already been sent. So every one of those individual confirmation packets sits unread in
the TCP socket's receive buffer the whole time, in a format (`LanTcpTransport::nextFrame()`)
has no code path for at all -- it only knows how to resync against fixed-size streaming
blocks. By the time real block parsing starts, whatever accumulated gets silently absorbed
into resync's search rather than actually processed, and the version reply and several
commands' acknowledgements are simply lost. The remaining question -- exactly why the
*counter value itself* comes out jittery rather than just "correctly framed but starting
from an unexpected number" -- isn't fully explained yet and needs more investigation once
proper packet-mode handling exists to test against a clean baseline.

**This is a real, scoped gap, not a quick fix, and is exactly what's next:**

1. `LanTcpTransport` needs a genuine packet-mode read path -- parsing standalone 8/12-byte
   confirmation/version-reply packets, separate from the streaming-block resync logic it
   already has.
2. `Device`'s LAN configuration sequence needs to actually wait for and consume each
   command's individual confirmation before sending the next one, matching DP §4.2's
   documented "Command → Confirmation → Next command" procedure -- currently it fires all of
   them with no acknowledgement wait at all, LAN or USB. (USB doesn't need this to work
   because embedded replies show up for free in blocks USB is already streaming
   continuously; LAN's packet-mode-before-streaming behavior has no equivalent free ride.)

Not committed yet. Files touched so far this session for this work: `main.cpp`'s LAN wiring
(committed target for next commit), `rsr200_lan_transport.h` (added errno/`WSAGetLastError()`
detail to `connect()`'s error message -- was previously a bare "connect() failed" with no way
to tell "nothing listening" from "firewalled" from "network unreachable" apart, found while
debugging this), and the new `test/test_lan_live.cpp`.

## Packet-mode handling: corrected via packet capture (2026-08-12, later same day)

Original implementation (this section's earlier text) assumed every pre-streaming LAN
command gets its own standalone reply packet, per a literal reading of DP 4.2. **Two packet
captures against the real radio disproved this**: only "Read version numbers" actually gets
a standalone reply (confirmed working, `serial=40 firmware=225`, byte-for-byte correct).
Every other config command (clock, data transmission, variables) — the radio ACKs the TCP
bytes and then sends back nothing at all, confirmed across multiple command types and
command orders. Reverted `Device::send()`'s synchronous packet-mode branch entirely; it's
back to always registering an async `pending` ack, same as before this whole investigation.
`Transport::readPacket()`/`LanTcpTransport::readPacket()` stay -- still correct, still used,
just only for the version query now (called directly by main.cpp/test_lan_live.cpp, not
through `Device::send()`). `test_device.cpp` reverted to its pre-session state (git checkout
-- the added tests were for the now-removed branch). `test_lan_transport.cpp`'s new
`readPacket()` tests stay valid.

**Live-retested after the revert: version reply now works correctly end-to-end.** But two
things remain unexplained and unresolved: the block sequence counter still jitters
(non-monotonic, occasionally negative deltas) exactly as before, and the last config command
(`tune`/0xB0) still never gets its embedded ack recognized before timing out. Fixing the
version reply did not fix either of these -- they're a separate, still-open problem, not
solved by anything in this session. Next step, if resumed: another packet capture, this time
covering the full applyConfig()-through-first-several-blocks sequence, to see directly
whether the embedded reply for command 0xB0 is actually present in what the radio sends and
just not being matched, or genuinely never sent.

Committed state as of this entry: not yet committed. Everything from today's LAN work
(module wiring, this packet-mode investigation and revert, the errno improvement, the two
new source/test files) is uncommitted in the working tree.

## `stop()` was leaving the radio streaming forever, contaminating every later connection (2026-08-12, later still)

The jitter and the missing 0xB0 (`tune`) ack from the previous section were retested against
what looked like a fresh connection each time, but a third packet capture — taken specifically
because the *first* few blocks of a brand new session looked like they already contained
streaming-shaped data before this session's own `Start Stream` had even gone out — found the
real reason: `LanTcpTransport::stop()` called `close()` internally.

`main.cpp`'s (and `test_lan_live.cpp`'s) stop sequence is: stop the transport (to unstick the
pump thread's blocked `nextFrame()`), join the thread, *then* send `Stop Stream`, *then*
actually close. That ordering depends on the socket still being writable after `stop()` —
which it wasn't. `close()` tears down the whole socket, so the `sendCommand()` for `Stop
Stream` that follows it silently failed every single time, on every test run this entire
session. The radio was never actually told to stop. Every subsequent connection — including
ones that looked "clean" from this end — inherited a radio still streaming from a session that
never ended, which is a real confound for anything downstream that depends on a known-clean
start (exactly the kind of thing that could plausibly explain jitter or a missing ack, which is
why this got tracked down before trusting either as a genuine protocol issue).

**Fixed** by replacing `close()` in `stop()` with `shutdown(fd, SHUT_RD)` (`SD_RECEIVE` on
Windows): this makes any pending or future `recv()` return immediately, exactly like a
peer-initiated close, while leaving the write side open — so a `Stop Stream` sent right
afterward still actually reaches the radio. A real `close()` is still required once that send
is done; `stop()` only ever shuts down the read side now. `test_lan_transport.cpp`'s shutdown
test was updated to match: `stop()` alone no longer disconnects, `sendCommand()` still succeeds
after it, and `close()` is what actually disconnects.

Ralph power-cycled the radio for a genuinely clean baseline and retested. **Both the jitter and
the missing 0xB0 ack persisted unchanged** — ruling out session contamination as their cause.
Whatever they are, they're real, not an artifact of a radio that was never actually told to
stop.

## Decimation test: the jitter doesn't just need a longer warmup (2026-08-12, later still)

With a genuinely clean baseline confirmed above, the next question was whether the counter
jitter was a startup transient (old queued data draining before real-time samples begin) that
would eventually settle at any rate, or something rate-dependent. `test_lan_live.cpp` was
extended to take `decimExp` and `pumpSeconds` as CLI arguments (previously fixed at the
`Config{}` default, decimExp=3, and a hardcoded 5-second run with output capped at 15 blocks)
so this could actually be tested instead of guessed at.

- **`decimExp=5`** (1.953 MSp/s): chaotic for roughly the first 5 blocks, then settled into a
  clean, monotonic +1-per-block sequence for the rest of the run.
- **`decimExp=3`** (7.8125 MSp/s), extended to 10 seconds / 112 blocks: chaotic for the first
  ~8 blocks, then settled into a **highly regular but non-`+1` repeating pattern** — runs of
  5–7 consecutive blocks at a steady `delta=+3`, interrupted by a recurring triplet (typically
  `+10, -7, +11`, with some variation) — that repeated for the entire remaining run without
  ever converging to a clean +1 sequence. The missing-0xB0-ack error also recurred partway
  through this run (~block 34–35), same as always.

Two things this rules out: it isn't a simple "needs more warmup time" problem (10 seconds and
112 blocks did not converge it), and the steady-state delta isn't simply equal to `decimExp` —
decimExp=3 gave delta=+3 but decimExp=5 gave delta=+1, not +5, so whatever relationship exists
between decimation and the counter's own step size isn't a direct 1:1 mapping. It also isn't
random: within a fixed decimExp, the pattern repeats with real structure (fixed-length runs of
identical deltas, a recurring triplet shape), which points at something systematic in how the
radio's firmware generates that field — not corruption, not lost data, not a resync bug on this
side (sync words and the inverted counter validate on every single block in every one of these
runs, with no exceptions, across every test done today).

**Not investigated further today.** The natural next step, if this gets picked back up, is
either another packet capture targeting the repeating triplet specifically, or testing more
`decimExp` values to see if the steady-state delta and the glitch period fit some other
relationship (e.g. tied to an internal DMA/batch size rather than to decimation directly).

## Sequence-gap detection disabled for LAN, kept for USB (2026-08-12, later still)

`SampleBlock::sequenceGap` (`Device::noteSequence()`) existed to flag lost data by checking
whether the block counter advanced by exactly 1. That assumption is simply wrong for LAN, for
two independent reasons, either of which would be enough on its own:

1. **LAN is TCP.** TCP already guarantees reliable, in-order, lossless byte delivery. If
   `LanTcpTransport::nextFrame()` hands back a block at all, its sync words and inverted
   counter have both already validated (that's `nextFrame()`'s own resync search) — meaning it
   is definitively the next chunk of bytes the radio sent, full stop. Nothing can have been
   lost or reordered at the transport layer the way a USB packet genuinely can be lost on the
   bus; a dead LAN connection is caught separately, by `nextFrame()` itself returning false.
2. **The counter field's own value isn't reliably `+1` regardless.** The decimation testing
   above found its steady-state delta varies by configured rate (not by any relationship this
   session could pin down) and, even within one steady rate, jumps in a repeating,
   non-monotonic pattern that doesn't settle out over a 10-second run. None of that reflects
   lost data — the framing is provably correct every time — it's some quirk of how the radio's
   own firmware generates that specific field.

Using it for gap detection on LAN was producing constant false positives with no real
diagnostic value. **Fixed**: `noteSequence()` now takes a `checkGap` bool. USB
(`parseUsbPacket`) still passes `true` — unchanged, and still meaningful there, since USB
packets genuinely can be lost on the bus. LAN (`parseLanBlock`) now passes `false`; the raw
counter is still captured and exposed via `SampleBlock::sequence` for whatever diagnostic value
it has, LAN just no longer derives `sequenceGap` from it. `test_device.cpp`'s LAN
counter-jump test was updated to assert `!gap`, and real gap-detection coverage (consecutive
vs. jumped counters) moved into the existing USB test block, since USB is the only place the
flag still means anything. Full suite (39 checks) passes.

Deployed (repo-root `SDR++.app`/`SDR++ RB.app`, `root_dev/modules/`) and smoke-tested to launch
and shut down cleanly. **Note for next time:** `/Applications/SDR++.app` is Ralph's kept
unforked comparison baseline, not a fork deploy target — it was mistakenly overwritten once
during this work and has to be restored from Time Machine; only `/Applications/SDR++ RB.app`,
if used at all, is a legitimate `/Applications` target.

Still open, unchanged by any of the above: the sequence counter's steady-state/jitter behavior
itself (documented above, not fixed, just no longer misread as data loss), and the missing
0xB0 (`tune`) embedded acknowledgement. Nothing from today's LAN work is committed yet.

## Live app tested: connects, tunes, shows spectrum — but audio is choppy and sounds out of order (2026-08-12, later still)

Ralph tested the real module (not a standalone smoke test) against the live radio over LAN:
connects, streams, reacts correctly to frequency and decimation changes, spectrum displays.
Audio, though, is choppy and sounds like it may be out of order — not reliably understandable.
The natural suspicion was the still-open counter jitter documented above, so that got checked
directly rather than assumed.

Three saved packet captures turned up in `~/Documents/` from earlier in the session
(`rsr200_lan_capture.pcap`, `capture2.pcap` — both tiny, from the pre-streaming packet-mode
investigation — and `capture3.pcap`, 47MB, covering a real streaming run). `capture3.pcap` was
reanalyzed offline with a scapy script (reassemble the radio→host TCP stream, resync against
`SYNC_BYTES` exactly like `findBlockStart()` does, extract 85 valid blocks) to test two things
neither of which needed live hardware access, just the existing capture:

1. **Does an odd-delta block's IQ payload literally duplicate an earlier block's content**
   (i.e. is the radio re-sending a chunk of samples it already sent)? MD5-hashed each block's
   IQ payload and checked it against the last 20 blocks' hashes. **No duplicates found, at
   all, anywhere in the capture.** The same `+10, -7, +11`-shaped jitter pattern from the
   earlier decimExp=3 live test reproduced exactly in this capture too (e.g. blocks 8–10:
   counters 131645, 131655 [+10], 131648 [-7], 131659 [+11], 131662 [+3]...), which is good
   independent confirmation the pattern is real and reproducible — but the content behind it
   is never a repeat.
2. **Is there an actual discontinuity in the IQ signal at odd-delta block boundaries that
   isn't present at normal (`delta=+3`) boundaries** — i.e. does the sample stream really
   jump/rewind in time there, even if the bytes aren't literally duplicated? Measured, at
   every one of the 84 boundaries, the magnitude of the jump from a block's last sample to the
   next block's first sample, and compared it against that block's own typical
   sample-to-sample jump size. **The ratio came out essentially the same regardless of delta**
   (normal boundaries: mean 1.04; odd boundaries: mean 1.09) — no elevated discontinuity at
   the jittery boundaries at all.

**This rules out the counter jitter as the explanation for the choppy audio.** The actual
sample content is continuous and correctly ordered straight through the capture, jitter or no
jitter — confirms the earlier decision to stop treating the counter as a data-loss/reordering
signal for LAN was right, but also means the choppy audio Ralph is hearing is a *separate,
still-unexplained* problem, not a symptom of the same root cause. Don't keep attributing it to
"the remaining items" without evidence — this capture is fairly direct evidence against that.

`deliver()` in `main.cpp` (the module's own handoff into SDR++'s stream) was checked as the
next-most-likely suspect and looks correct: `memcpy` from `Device`'s buffer into the stream's
own `writeBuf`, then `swap()` — the same pattern every other source module in this codebase
uses, synchronous, no reuse-before-consumption race.

**Leading remaining suspect, not yet tested: delivery pacing/throughput, not content
correctness.** At decimExp=3 (7.8125 MSp/s, 1ch 16-bit), the block rate implies roughly 31
MB/s sustained — `LanTcpTransport::nextFrame()`'s `recv()`-then-resync loop, or something
downstream in SDR++ core's own audio path, may not be keeping that paced evenly in real time,
which would produce audible glitches even with perfectly-ordered content (bursty delivery
rather than a steady stream). `RECORDING_PERFORMANCE_PLAN.md` documents a real, previously-hit
throughput ceiling in SDR++ core's `Splitter` at comparable rates, for an unrelated module —
worth checking whether this is the same class of problem before assuming it's RSR200-specific.
Not yet investigated: needs either a live profiling pass (`sample` on the running process
while audio is actually playing, the same technique that nailed the recording throughput
issue) or a lower-rate live A/B test (does decimExp=5, roughly a quarter the data rate, sound
noticeably cleaner?) to confirm before chasing a fix. Checked in with Ralph before proceeding
further on this.

## Throughput/pacing theory ruled out; status fields also clean (2026-08-12, later still)

Ralph ran the lower-rate A/B test himself: 64x hardware decimation (ADC clock is actually
91.6 MHz on this radio, not the 125 MHz used in most of this doc's worked examples — 91.6/64 =
1.431 MSp/s) plus another 8x of software decimation on top, for a final rate a small fraction
of the decimExp=3 rate the choppiness was first noticed at. **Made no difference — audio is
still choppy.** This rules out the leading suspect from the previous section outright: if it
were a throughput/pacing ceiling, a rate this much lower should have shown a clear
improvement, and it didn't.

Two things make this even more conclusive:

- **Decimation can't cause a live mid-stream race either way.** Every rate-affecting control in
  `main.cpp`'s menu (ADC clock, decimation) sits inside the same `BeginDisabled`/`EndDisabled`
  block gated on `running` — they simply can't be changed while streaming. So this was
  necessarily a clean stop → reconfigure → restart, never a live race between the module
  declaring a new rate to `core::setInputSampleRate()` and the radio actually switching over.
- **91.6 MHz / 64x is essentially the same low-rate regime as the earlier decimExp=5 test**,
  which showed *zero* counter jitter (clean, settled +1-per-block). Audio was still choppy
  under a config already confirmed jitter-free — independent confirmation the jitter and the
  choppy audio are genuinely two separate problems, not cause and effect.

Also checked directly against `capture3.pcap` (no new hardware access needed): the per-block
status fields (temperature/Auto-ATT byte, GPS/overload bits, `cmdNo`) across all 85 blocks,
since a flapping Auto-ATT flag would cause a real, audible 12 dB gain pop on every flap that
would sound exactly like "choppy." **Rock solid the entire run** — temp byte constant at 63
(Auto-ATT never active), GPS/overload bits constant, `cmdNo` incrementing sensibly. Rules this
out too.

Ruled out so far, in total: block reordering/duplication, discontinuity at the jittery
boundaries, throughput/pacing at a given rate, a live decimation race, and Auto-ATT gain
flapping. That's most of the LAN-block-data-path candidates exhausted without finding the
cause — increasingly looks like this isn't a bug in the RSR200 module's LAN parsing/delivery
path at all.

**Next step, in progress:** the same demodulation over USB, on the same radio, same
demodulator settings — the cheapest remaining test that actually discriminates LAN-specific
causes from something shared (the `Device` layer's `deliver()`/unpack/gain path, common to
both transports, or the demod chain itself, unrelated to RSR200 entirely). If USB is clean,
it's LAN-specific after all, in a way none of the above tests caught. If USB is *also* choppy,
this has nothing to do with any of today's LAN work. Ralph is running this test now; result not
yet in.

## LAN audio choppiness: conclusion — a real RSR200 LAN firmware throughput limit, not a fixable bug (2026-08-12, later still)

**USB confirmed clean; LAN confirmed choppy, same radio, same settings.** Ralph recorded ten
seconds each of the identical demodulation over both transports for direct comparison
(`baseband_994506Hz_19-09-18...wav` = USB, `...19-09-42...wav` = LAN). Parsing both directly
(16-bit stereo I/Q, no live hardware needed) found them statistically identical — same mean
RMS, no dropout runs longer than 3 frames (noise) in either, no level spikes, zero duplicate
content in either file. **The received sample data is equally clean on both transports.** That
matters because a WAV recording only ever stores sample *values*, never arrival timing — a
recording can look and sound perfect even if the underlying delivery was wildly uneven,
because disk writes have slack a live audio callback doesn't. So this ruled out any
data-correctness bug (reordering, duplication, corruption) as the cause, without ruling out a
timing/pacing problem specifically in *live* playback.

A live profile (`sample`, 10 seconds, LAN connected and audibly choppy) then found where time
was actually going: the network worker thread was correctly ~99.9% idle, blocked in
`recvfrom()` waiting on the socket — not a CPU-bound bottleneck anywhere in this module's own
code. The live CoreAudio output callback, though, spent most of its time blocked inside
`dsp::sink::RingBuffer`/`dsp::buffer::RingBuffer` (`core/src/dsp/sink/ring_buffer.h`,
`core/src/dsp/buffer/ring_buffer.h`) — both explicitly flagged by the original SDR++ authors
themselves (`// NOTE: THIS IS COMPLETELY UNTESTED AND PROBABLY BROKEN!!!`, `// IMPORTANT: THIS
IS TRASH AND MUST BE REWRITTEN IN THE FUTURE`), and which do have a real structural race
(`readable`/`writable` tracked as two separate counters under two separate mutexes, updated as
two separate lock/unlock pairs rather than atomically together). That's shared core code used
by every module's audio output, not RSR200-specific, so four fixes were tried first, scoped
entirely to this module, before considering touching it:

1. **Chunk `deliver()`'s hand-off into smaller pieces** (closer to USB's own ~1020-frame
   packet granularity, instead of one 130560-frame `swap()` per LAN block). No change — still
   choppy. Ruled out: burst *size* as the trigger.
2. **Individually pace each chunk to real time**, via `paceToRealTime()` copied from
   `file_source`'s own proven `worker()` (`source_modules/file_source/src/main.cpp`) — which
   solves an analogous problem (a file has no natural real-time pacing) for a documented,
   previously-confirmed reason: that module's own comment records a real, measured
   consequence of skipping this exact thing, a dual-channel RSR200 *recording* nulling 20+ dB
   shallower on playback than the same antennas nulled live, traced to decorrelation's
   adaptive solve being sensitive to how evenly-spaced its input blocks are. Still no change
   live. Ruled out: burst *pacing within an already-arrived block* as the trigger — which
   makes sense in hindsight, since neither fix touches data that hasn't arrived yet.
3. **A genuine jitter buffer** (`deliver()` only appends to a queue; a separate
   `deliverWorker()` thread drains it at a steady paced rate), motivated by a third piece of
   direct evidence: cross-referencing `capture3.pcap`'s own packet timestamps against the
   block counter found the counter's long-documented jitter (§"First live LAN connection")
   and a real ~80ms delivery stall are *the same event*, recurring roughly every 9 blocks
   (~450ms), with zero exceptions across the whole 85-block capture. Decimation-independent
   (tied to a block count, not wall-clock time — Ralph's own 18x-rate A/B test earlier showed
   no difference, which fits) and LAN-only (USB's continuous small-packet framing is a
   different code path in the radio's own firmware). **Live result: choppiness changed shape**
   — less frequent but longer, and the spectrum display started visibly freezing along with
   the audio, meaning something was starving further downstream than just the audio ring
   buffer. Added a live buffer-occupancy readout to the module's own status panel
   (`jbMinFrames`/`jbMaxFrames`, later `measuredSampleRate`) rather than keep guessing —
   **found the buffer chronically draining to near-zero and staying there**, the signature of
   a persistent rate mismatch, not bounded jitter a fixed buffer can absorb.
4. **Adapt the buffer's drain rate to a live-measured delivery rate** instead of the nominal
   decimation formula. First attempt (a fast EWMA of instantaneous per-block rate, re-read on
   every ~1024-frame chunk — over a thousand times a second at this rate) made things *worse*,
   shorter and more frequent choppiness: the drain pacing itself was chasing the radio's own
   ~450ms-period stall in near-real-time rather than smoothing over it. Fixed the estimator to
   a proper total-frames-over-total-elapsed-time measurement across a 3-second window (long
   enough to average out many stall cycles, updating once per window instead of every block)
   — this settled into a *stable* reading, ~1.17-1.25 MSp/s against a nominal 1.431 MSp/s
   (91.6 MHz ÷ 64), but audio got *worse*, not better. **Ralph caught the actual reasoning
   error directly**: the pacing rate isn't just "whatever keeps the buffer topped up" — it's
   what the resampler/demodulator chain, and transitively the audio hardware's own fixed
   real-time output clock, is built around (via `core::setInputSampleRate()`, left at the
   nominal rate throughout all of this). Draining any slower than that feeds the pipeline in
   slow motion relative to what it's told to expect; the audio device pulls at its own
   real-time clock regardless of what the buffer thinks, and starves waiting for correctly
   *timed* data, not just correctly *ordered* data. Reverted the drain rate back to always the
   nominal formula, with the jitter buffer restricted to its original, correct job — absorbing
   the short stall, not renegotiating what the rate means. **Result: no different from before
   any of this** — Ralph: "I'm seeing no difference. Measured rate started low and has settled
   around 1.25... with the audio and spectrum effects that mismatch would predict." Confirmed
   again on a full retest at unchanged settings (still 91.6 MHz, still 64x decimation, nothing
   changed on the radio side): "Measured rate is between 1.2 and 1.35... Jitter buffer... is
   mostly 1 or 2 digits, occasionally a third which appeared to be a 1" — i.e. still chronic
   near-zero occupancy, still ~13-18% below nominal, regardless of what the drain rate was set
   to.

**That last result is the real finding, and it's decisive**: the buffer starves at the *same*
measured rate whether draining at that rate, at the nominal rate, or anywhere between. Pacing
strategy provably isn't the variable. Combined with everything above — the stall recurs on a
fixed *block count*, not wall-clock time, and 64x is the documented maximum decimation, so
there's no lower rate left to try that hasn't effectively already been tried; wire bandwidth at
this rate (~5.7 MB/s) is nowhere near a real constraint for any modern LAN, and the receiving
thread was independently confirmed via profiling to be idle almost the entire time, not
struggling to keep up — this stopped looking like anything reachable from the receiving side.
**The RSR200's LAN interface appears to genuinely be unable to sustain its own nominal
decimation rate in real time, consistently, by roughly 13-18%, independent of buffering or
pacing strategy on this end.** A jitter buffer, however large, cannot manufacture data the
radio never sends; it can only delay how long it takes to notice the shortfall. This may be a
genuine firmware limitation or design constraint of this radio's LAN implementation
specifically (its continuous, small-packet USB path is architecturally quite different and
doesn't show any of this) — not something fixable from the SDR++ side without either lying
about the sample rate (a real tradeoff: smoother audio at the cost of incorrect tuning/filter
math, not attempted) or understanding the radio's own firmware well enough to work around
whatever causes the recurring stall, which is beyond what black-box testing from this side can
determine.

**Decided with Ralph: stop chasing smoothness here.** All four attempts reverted;
`main.cpp`'s `deliver()` is back to its original simple form (one `memcpy` + `swap()` per
block, no chunking, no pacing, no jitter buffer, no diagnostic UI). Full rebuild, redeployed to
the repo-root bundles and `root_dev/modules/` (never `/Applications/SDR++.app` — see this
file's `/Applications` note below and [[sdrpp-applications-deploy-boundary]] in memory),
smoke-tested to launch and shut down cleanly. **USB remains fully functional and is the correct
transport for actual listening on this radio today.** LAN stays usable for lower-stakes
purposes where occasional audio degradation doesn't matter (spectrum display, remote tuning,
command/control) but should not be relied on for real-time audio until/unless the radio's own
firmware behavior here is better understood — worth reporting to Reuter as a possible firmware
observation, since nothing found today points at anything fixable on the receiving end.

Nothing from today's LAN work (module wiring, the packet-mode investigation, the `stop()` fix,
sequence-gap handling, and this whole choppiness investigation) is committed yet.

## Same LAN ceiling, a different symptom: recordings coming out shorter than the real session (2026-08-13)

A day later, a separate bug report turned out to very likely be this same, still-unresolved LAN
throughput ceiling — see `RECORDING_PERFORMANCE_PLAN.md` phase 17 for the full account. Short
version: baseband recordings made over LAN at high sample rates (single-channel ~20+ MSp/s,
dual-channel ~10+ MSp/s per channel — the same total-byte-throughput region as the choppiness
above) come out genuinely shorter than the real recording session, by a variable amount (46-85%
of the session missing across four real test recordings), with no gap or backpressure
indication anywhere in the recording pipeline itself. A fixed-duration periodic stall consuming
a larger *fraction* of the session as the configured rate increases (each block represents
proportionally less real time at a higher rate) is fully consistent with both this and the
smaller (~13-18%) shortfall measured above at a much lower rate. Not yet directly confirmed —
the clean test is the same recording over USB at a comparable rate, mirroring how this
investigation used USB as its own control throughout. A separate, genuinely different bug
(`sigpath::phasing`'s own splitter architecture, unrelated to RSR200 or LAN specifically) was
also found and is a likely *compounding* factor for dual-channel recordings specifically, but
does not explain the single-channel case — see `RECORDING_PERFORMANCE_PLAN.md` phase 17 for
that half of it.

**Correction, same day, to how the section above frames the cause.** Ralph: yesterday's
choppiness testing (the section above, "~13-18% below nominal") was over **WiFi**; today's
recordings showing the much larger 46-85% shortfall were over **Ethernet**. That the threshold
is this different between the two physical links is a real, important data point the section
above didn't have: a genuine radio-firmware-internal limitation (the framing settled on above —
"this may be a genuine firmware limitation... independent of buffering or pacing strategy on
this end") would be expected to hit the *same* ceiling regardless of which physical link
carries the TCP connection, since the radio's own firmware has no visibility into that. A
threshold that moves this much between WiFi and Ethernet instead points at the **physical link
itself** as (at least) the dominant factor — most plausibly the radio's own WiFi interface
specifically, which embedded instrument-grade WiFi hardware is often considerably less capable
than a modern router or laptop chipset, well before any RSR200-firmware-internal ceiling would
even be reached. Doesn't reopen everything above — the profiling, the ruled-out receiving-side
causes, and the packet-capture evidence of a real, block-count-tied recurring stall are all
still valid observations — but it does mean "independent of buffering or pacing strategy on
this end" was too strong a claim, and "genuine firmware limitation" undersold how much of this
might simply be WiFi's own, considerably lower, real-world throughput ceiling. Practical
upshot, not yet fully confirmed: **Ethernet is very likely the right recommendation for any
serious LAN use of this radio** (matching what's already true for the recording case above —
higher sample rates hold up much better over Ethernet), with WiFi reserved for lower-rate,
lower-stakes use. Whether Ethernet has a real ceiling of its *own* above and beyond WiFi's
(rather than just a much higher one) is still an open question — the recording numbers above
(clean shortfall pattern up to some point even on Ethernet, at rates far above what broke down
over WiFi) suggest it might, but that hasn't been isolated from the `sigpath::phasing` and
general recording-pipeline factors also in play for those specific tests.

## Per-device settings keying refactor (2026-08-15)

Prompted by designing `RECORDING_SCHEDULER_PLAN.md` (a separate, not-yet-started plan for a new
scheduling module): that plan's survey of every source module in the tree found that essentially
all of them — including RFspace and SpyServer, RSR200's closest architectural peers (network
sources with no hardware enumeration, just a user-typed host) — key their tunable settings under
`config.conf["devices"][<connection identity>]`, even when that identity is nothing more than a
typed hostname. RSR200 was the outlier: one flat blob (`adcClockMHz`, `decimExp`, `bits24`, etc.
directly on `config.conf`) shared regardless of which physical unit `lanHost` pointed at, or
whether USB or LAN was selected.

Confirmed via a direct AskUserQuestion ("full refactor now") and implemented on branch
`rsr200DevicesKeying`:

- Settings now live under `config.conf["devices"][key]`, `key` = the connected device's serial
  for USB, `lanHost` for LAN — matching RTL-SDR's serial-keying and RFspace/SpyServer's
  host-keying exactly.
- USB gained real device enumeration and a device-select combo + Refresh button in the menu.
  This uses `UsbTransport::listDeviceInfo()`/`openBySerial()`, both of which already existed in
  `transport_usb.h`/`.cpp` but were unused by `main.cpp` — it previously just opened device index
  0 unconditionally. Falls back to index-0 open if no serial is selected/present, matching every
  other enumerated source module's own fallback-to-first-device behavior.
- One-time migration on first load of an old-schema config file: the flat legacy keys move into
  `devices.<key>`, keyed by whatever device/host was current at that moment, then are erased from
  the top level so the migration can't re-run and clobber a real edit later.
- Verified against the user's actual live `rsr200_config.json` (not a synthetic test file): real
  settings (LAN, `192.168.1.176`, atten2=14, VHF+preamp on, etc.) migrated with every value
  preserved exactly, flat keys removed, `transportSel`/`lanHost` correctly left flat. Backed up
  before testing.
- Not verified: real USB device enumeration/serial-open against physical hardware — no USB
  pass-through available in this environment. The code path is built on `transport_usb.cpp`'s
  already-tested `openBySerial()`/`listDeviceInfo()`, but hasn't itself been exercised live.

See `RECORDING_SCHEDULER_PLAN.md` section 2.2 for the full survey this came out of (and its own
2026-08-15 correction: RTL-SDR does *not* actually reload settings on `SourceManager::selectSource()`
either, contrary to that section's first draft — traced more carefully while doing this refactor).
