# Recording Scheduler: design & implementation plan

**Status: 2026-08-15 — PLANNING, not started.** This document is the plan only. Whether/when
implementation begins is a separate decision (per the user: depends on session budget left
after the plan itself is written). No branch, module directory, or CMake change exists yet.

## 0. Why a new module, not `misc_modules/scheduler`

`misc_modules/scheduler` (`OPT_BUILD_SCHEDULER`, default `OFF`) already exists but is an
unfinished tech demo, confirmed by direct reading, not just by its three-years-stale git
history:

- Its module class is literally named `DemoModule` (`misc_modules/scheduler/src/main.cpp:15`).
- Its constructor hardcodes two fake entries, `"Test"` and `"Another test"`, both pointing at
  the same in-memory `Task` object (`main.cpp:40-41`) — nothing is ever loaded from or saved to
  a config file; the module has no `ConfigManager` at all.
- The one column meant to show a live countdown literally renders the string `"todo"`
  (`main.cpp:116`).
- `sched_task.h`'s trigger-list table renders one hardcoded row, `"Every day at 00:00:00"`
  (`sched_task.h:63`) — there is no actual trigger/recurrence data structure behind it.
- The only two actions it ships, `TuneVFO` and `StartRecorder`
  (`misc_modules/scheduler/src/actions/`), both have an **empty `trigger()`** — nothing has ever
  actually fired a scheduled action in this codebase.
- There is no worker thread anywhere in the module — nothing evaluates wall-clock time against
  anything, ever, even if you did fill in real data.

So this isn't a working feature with a few gaps; it's a UI mockup with no engine underneath. It
confirms the user's read exactly. Building a new module from scratch under its own name/directory
(suggested: `misc_modules/recording_scheduler`, display name "Recording Scheduler" — easy to
rename later, not load-bearing) means:

- Nothing here collides if upstream ever picks their own `scheduler` back up.
- No obligation to preserve `DemoModule`'s data shapes, action-class abstraction, or the
  `Task`/`sched_action::Action` split — none of it is reusable (confirmed above: no persistence,
  no engine, no real trigger data), so this plan designs the data model and engine fresh rather
  than trying to repair a shape that was never finished enough to reveal whether it was right.

## 1. Requirements, as given

1. Schedule multiple recordings, each with a start time and a stop time.
2. Each entry can be **one-shot** or **repeating**.
3. Expired one-shot entries are **kept**, not deleted — their date/time can be edited to re-arm
   them for a future run.
4. Entries can be **deleted** and **duplicated**.
5. An entry selects a **radio** (source module instance) and can **save the current
   configuration of that radio** into the entry, to be reproduced at run time.
6. There must be a way to **view an entry's saved settings without applying them** to the
   program's actual current state — checking what's scheduled must not disturb what's currently
   being listened to.
7. The scheduler does **not** need its own UI for editing radio-specific fields (gain, sample
   rate, decimation, etc. — different per module) — only capture-current / view-saved /
   apply-at-trigger-time. Explicitly out of scope, per the user.
8. The scheduler **does** need its own UI for the Recorder module's fields specifically, because
   the Recorder module is the same for every radio and has a small, fixed field set. A given
   entry's Recorder fields default to the Recorder's current live settings, editable per entry.

## 2. Architecture survey (what already exists to build on)

### 2.1 Only one radio is ever "the" active source

`SourceManager` (`core/src/signal_path/source.h`) holds exactly one `selectedName` /
`selectedHandler` at a time (`source.h:91-92`); `sourcemenu::draw()`
(`core/src/gui/menus/source.cpp:283-300`) disables the source combo outright whenever the SDR is
running (`if (running) { style::beginDisabled(); }`, line 289) — you cannot even pick a different
device mid-stream today, let alone run two at once. This is a real, hard constraint the whole
plan has to respect: **the scheduler can switch which radio is active, but never run two radios
concurrently.** Section 6.4 below covers what that means for overlapping entries.

Source instances are looked up by name via `sigpath::sourceManager.getSourceNames()`
(`source.h:75`) and switched with `selectSource(name)` (`source.h:46`, implemented
`source.cpp`); `start()`/`stop()`/`tune()` round out the control surface the scheduler needs
(`source.h:48-50`).

### 2.2 No generic "get/reproduce a radio's settings" hook exists — this is the central open problem

Every source module keeps its **own** `ConfigManager` pointed at its own JSON file (e.g.
`core::args["root"] + "/rsr200_config.json"`), separate from the shared app `config.json`
(`core::configManager`). Two different internal conventions were found by direct reading:

- **Multi-device hardware sources** (RTL-SDR, Airspy, HackRF, SDRplay, LimeSDR, Perseus,
  PlutoSDR, BladeRF, HydraSDR, FobosSDR, RFspace, SDDC, Soapy, SpyServer, USRP, HermesLite — 17 of
  25 source modules, confirmed by grep) key settings by device name/serial under
  `config.conf["devices"][deviceName][...]`, e.g. `rtl_sdr_source/src/main.cpp:199-249`.
- **RSR200** (the user's own radio, and the one that actually matters for this feature) originally
  kept a **flat**, single-device config with no `devices` nesting at all. **Update, 2026-08-15:**
  this survey led directly to refactoring RSR200 onto the same `devices.<key>` convention —
  serial for USB (using enumeration/open-by-serial that already existed in `transport_usb.h` but
  went unused by `main.cpp`), `lanHost` for LAN (matching RFspace/SpyServer below). See
  `source_modules/rsr200_source/src/main.cpp`'s `currentDeviceKey()`/`loadDeviceSettings()`/
  `saveConfig()`, branch `rsr200DevicesKeying`. RSR200 is no longer the outlier described below.
- File/network/server-style sources (`file_source`, `network_source`, `rtl_tcp_source`,
  `sdrpp_server_source`, `rfnm_source`, `harogic_source`, `kcsdr_source`, `dragonlabs_source`,
  `spectran_source`/`spectran_http_source`, `phasing_test_source`) have no persistent per-device
  settings blob at all in the same sense — not meaningfully "reproducible" the same way, and out
  of scope for this feature regardless.

Worse than the schema difference: **re-selecting a source does not reload its settings from
disk in *any* module checked, not just RSR200.** This section originally claimed RTL-SDR was
better off here ("happens to reload via `selectByName()`") — traced more carefully during the
RSR200 devices-keying refactor (2026-08-15) and that's wrong. `selectByName()` is only ever
called from RTL-SDR's own constructor and its own on-screen device combo
(`rtl_sdr_source/src/main.cpp:93,391-392`) — never from `menuSelected()`
(`main.cpp:275-279`, the function `SourceManager::selectSource()` actually calls via
`SourceHandler::selectHandler`), which does nothing with config at all. So RTL-SDR and RSR200
turn out to behave identically here: both load their settings from disk exactly once, at
construction, and `SourceManager::selectSource()` never re-triggers that for either one. Writing
new values into either module's own config file between scheduler runs would silently do
nothing, because the already-alive module instance (module instances persist for the app's
whole lifetime, per `ModuleManager`) would just go on using whatever's already in its own
memory. This doesn't change the recommendation below — it was already designed around not being
able to rely on a reload-on-select — it just corrects the claim that any existing module gets
this for free.

**This means there is no safe, generic way to "reproduce a radio's settings" today — it has to be
built, deliberately, as an opt-in per-module capability, not assumed to fall out of existing
plumbing.** Recommended shape (additive, does not touch any module that doesn't opt in):

- Add two new optional callbacks to `SourceManager::SourceHandler` (`source.h:15-24`), both
  `nullptr` by default so every existing source module keeps compiling and behaving identically
  without any change on their part:
  ```cpp
  json (*captureConfigHandler)(void* ctx);         // returns a snapshot of this source's
                                                     // current settings as JSON, or an empty
                                                     // json{} if not implemented
  void (*applyConfigHandler)(const json& cfg, void* ctx); // apply a previously-captured
                                                            // snapshot; safe to call whether
                                                            // or not this source is currently
                                                            // selected/running
  ```
- A source module that implements both simply serializes/deserializes whatever fields it already
  has (for RSR200: `adcClockMHz`, `decimExp`, `bits24`, `dualChannel`, `swapChannels`, `useVhf`,
  `vhfPreamp`, `atten1`, `atten2`, `transportSel`, `lanHost` — the exact field list already in
  `loadConfig()`/`saveConfig()`, `main.cpp:505-543`) directly into/out of its own live in-memory
  state (not via a disk round-trip — sidesteps the reload-doesn't-happen problem entirely) and
  calls its existing `saveConfig()` so the change also persists normally.
- The scheduler stores whatever JSON `captureConfigHandler` returned, opaquely — it never needs
  to know or validate the field names inside. It **does** need to know, and store alongside the
  blob, which underlying module type it came from (`core::moduleManager.getInstanceModuleName(sourceName)`,
  `core/src/module.h:90`), so it can refuse to apply a snapshot to a same-named-but-different-type
  source later (device swapped, name reused) and can show "settings not supported for this radio"
  for any source whose module never registered the two new callbacks.
- **v1 scope**: implement `captureConfigHandler`/`applyConfigHandler` for **RSR200 only** — it's
  the user's actual radio and the only one that needs this to be useful right now. Section 8
  (phasing) adds a second module later (RTL-SDR is the obvious next one, both to prove the
  mechanism generalizes and because it's a plausible secondary radio) once the mechanism itself
  is proven out.

### 2.3 Cross-module control already exists — `ModuleComManager` — but the Recorder's exposed surface is too small

`core::modComManager` (`core/src/module_com.h`) is exactly the established, already-idiomatic
mechanism for one module to drive another: `registerInterface(moduleType, instanceName, handler,
ctx)` / `callInterface(instanceName, code, in, out)`. The Recorder module already registers
itself this way — `core::modComManager.registerInterface("recorder", name, moduleInterfaceHandler,
this)` (`misc_modules/recorder/src/main.cpp:135`) — and already exposes a small command set via
`misc_modules/recorder/src/recorder_interface.h`:

```cpp
enum { RECORDER_IFACE_CMD_GET_MODE, RECORDER_IFACE_CMD_SET_MODE,
       RECORDER_IFACE_CMD_START, RECORDER_IFACE_CMD_STOP };
enum { RECORDER_MODE_BASEBAND, RECORDER_MODE_AUDIO };
```

That's enough to start/stop a recording and read/set baseband-vs-audio mode, but not enough for
"the choices of the Recorder module" the user wants schedulable. The full, confirmed field list
a `RecorderModule` instance actually persists (`recorder/src/main.cpp:82-120` load,
mirrored on save) is:

| Field | Type | Notes |
|---|---|---|
| `mode` | enum | Baseband / Audio (`RECORDER_MODE_*`, already exposed) |
| `recPath` | string | Output folder |
| `timezone` | enum | Local / UTC, for filename timestamps |
| `container` | enum | WAV only today (RF64 deliberately disabled — `main.cpp:62`) |
| `sampleType` | enum | Uint8 / Int16 / Float32 / Int32 |
| `audioStream` | string | Which audio stream to record (Audio mode only) |
| `audioVolume` | float | |
| `stereo` | bool | |
| `ignoreSilence` | bool | |
| `recordDualChannel` | bool | Phasing dual-channel baseband capture |
| `nameTemplate` | string (1023 char buffer) | `$t $f $h $m $s $d $M $y` placeholder syntax |

Plan: extend `recorder_interface.h` with a `RECORDER_IFACE_CMD_GET_CONFIG` /
`RECORDER_IFACE_CMD_SET_CONFIG` pair that moves a whole JSON blob of these fields through `in`/
`out` (simplest — one command pair instead of one per field, and trivially extensible if a field
is added later), plus keep `START`/`STOP`/`GET_MODE`/`SET_MODE` as they are since the scheduler
needs `START`/`STOP` directly regardless. This is a small, additive, self-contained change to a
module already built for exactly this kind of external control — no redesign.

Discovering which Recorder **instances** exist to schedule against: iterate
`core::moduleManager.instances` (`core/src/module.h:101`, a public `std::map`), filtering
`getInstanceModuleName(name) == "recorder"` (`module.h:90`) — no change needed here, this is
already fully general.

### 2.4 Precedent for everything else the new module needs

- **Own persisted config file, keyed like RSR200/Recorder's**: `ConfigManager config;` +
  `config.setPath(root + "/recording_scheduler_config.json")` in `_INIT_()`, `acquire()`/
  `release(true)` around writes — identical pattern in every misc/source module read this
  session.
- **A background worker thread polling on an interval, independent of the GUI frame rate**:
  `misc_modules/scanner/src/main.cpp` (`workerThread = std::thread(&ScannerModule::worker,
  this)`, `main.cpp:125-141`) — start on module enable, join on disable/destroy, exactly the
  shape the scheduler's own trigger-checking loop needs, just always-on rather than
  button-triggered.
- **An editable list of named, persisted entries with an add/edit/delete table UI**:
  `misc_modules/frequency_manager` (`std::map<std::string, FrequencyBookmark> bookmarks`,
  `main.cpp:812`) — closest working precedent in the tree for the entry-list table itself
  (`ImGui::BeginTable` + `Selectable` + double-click-to-edit popup), same shape the abandoned
  `DemoModule`/`Task::showEditMenu` were visibly trying (and failing) to build toward.
- **Module registration boilerplate**: `option(OPT_BUILD_RECORDING_SCHEDULER "..." ON)` in
  `CMakeLists.txt` (next to the other misc-module options, `CMakeLists.txt:68-76`) +
  `add_subdirectory("misc_modules/recording_scheduler")` guarded the same way
  (`CMakeLists.txt:361-363` is `OPT_BUILD_SCHEDULER`'s own block, for reference). Recommend
  defaulting **ON** — unlike the abandoned demo, this module would actually work, and this
  user's stated preference this session has consistently been "just build it in, I'll manage it
  from the in-app module list" (see the RSR200-always-on change).

## 3. Data model

One entry, persisted in the new module's own config file as `entries[<uuid>]`:

```jsonc
{
  "id": "d3b0...",                    // stable id, independent of display name (renaming
                                        // must not orphan history/status)
  "name": "6-shot 41m Sunday sked",
  "enabled": true,                     // master on/off without deleting

  "recurrence": {
    "type": "once",                    // "once" | "daily" | "weekly"
    "startEpoch": 1755500400,          // "once": absolute start instant
    "stopEpoch": 1755504000,           // absolute stop instant (or see durationSec below)
    // "daily"/"weekly" instead use wall-clock time-of-day + duration, evaluated against
    // whatever date it's next due:
    "startTimeOfDay": "16:00:00",
    "durationSec": 3600,
    "daysOfWeek": [0, 3]               // "weekly" only; 0=Sunday..6=Saturday
  },

  "sourceName": "RSR200",              // sigpath::sourceManager instance name at capture time
  "sourceModuleType": "rsr200_source", // core::moduleManager.getInstanceModuleName(sourceName)
                                        // at capture time -- guards against a same-name/
                                        // different-module mismatch later
  "sourceConfigSnapshot": { ... },     // opaque JSON from captureConfigHandler(); empty/absent
                                        // if this radio doesn't support capture (section 2.2)
  "sourceConfigCapturedAt": 1755498000,
  "frequency": 7355000.0,              // captured VFO/tune frequency, applied via
                                        // sourceManager.tune() after selecting+starting

  "recorderName": "Recorder",          // target Recorder instance name (section 2.3)
  "recorderConfigSnapshot": { ... },   // the fields table in 2.3, editable in-place in this
                                        // module's own UI (not opaque, unlike the radio blob)

  "status": "scheduled",               // "scheduled" | "ran" | "skipped" | "running"
  "lastRunEpoch": null,
  "lastSkipReason": null                // e.g. "source busy with another entry"
}
```

Editing the date/time of a `"once"` entry whose `status` is `"ran"`/`"skipped"` simply resets
`status` back to `"scheduled"` — this is the entire "kept, editable, re-armable" mechanism the
user asked for; no separate archive/history table needed for v1 (`lastRunEpoch`/
`lastSkipReason` already retain the most recent outcome across re-arms).

## 4. UI plan

Single `gui::menu.registerEntry` panel, same integration point every other misc module uses:

- **Entry table** (Name / Radio / Next or Last Run / Status), `Selectable` rows, matching
  `frequency_manager`'s table idiom. Row toolbar: **Add**, **Duplicate** (deep-copies everything
  except `id`, appends " (copy)" to the name, resets `status`), **Delete** (confirm dialog,
  matching the existing `GenericDialog` yes/no pattern already used in `gui/menus/source.cpp:342-346`
  for offset deletion), **Enable/Disable** checkbox per row.
- **Edit popup**, opened on double-click (matching the existing double-click-to-edit convention
  in both `frequency_manager` and the abandoned scheduler's own `Task::showEditMenu`):
  - Name field.
  - Recurrence editor: Once / Daily / Weekly radio choice; date+time pickers for Once; time-of-day
    + duration + day-of-week checkboxes for Daily/Weekly.
  - Radio section: combo of `sigpath::sourceManager.getSourceNames()`; **"Update from current
    settings"** button (calls `captureConfigHandler` on the *currently selected & running* source
    if it matches the combo's choice — disabled otherwise, with a tooltip explaining why); **"View
    saved settings"** button opens a **read-only** popup dumping the captured JSON as plain text —
    deliberately just a debug-style dump rather than a per-field pretty-printer, since the whole
    point (section 2.2) is that the scheduler doesn't and shouldn't know the field semantics of
    every radio module. This satisfies requirement 6 (view without applying) trivially, since it
    never touches `sigpath::sourceManager` at all.
  - Recorder section: combo of discovered Recorder instances (section 2.3); the field table from
    2.3 rendered as real editable controls (this module *does* know the Recorder's schema, by
    design); a **"Reset to current Recorder settings"** button that re-reads the live instance via
    `RECORDER_IFACE_CMD_GET_CONFIG` and overwrites the entry's snapshot with it — matches
    requirement 8's "current settings as defaults, but editable per entry."
  - Apply/Cancel, matching the existing popup convention.

## 5. Engine

One worker thread, started in `postInit()`/module-enable and joined on disable/destroy (section
2.4 precedent), ticking roughly once per second:

1. For every `enabled` entry whose recurrence is currently due (start time reached, not yet
   `"running"`/already-`"ran"`-today for repeating types): attempt to fire it.
2. **Firing sequence**: if the target source isn't already the selected+running one, stop the
   currently running source (if any), `selectSource()` the target, call `applyConfigHandler` with
   the saved snapshot (if the module supports it — section 2.2), `start()`, then `tune()` to the
   saved frequency. Then locate the target Recorder instance, push the entry's
   `recorderConfigSnapshot` via `RECORDER_IFACE_CMD_SET_CONFIG`, and `RECORDER_IFACE_CMD_START`.
   Mark `status = "running"`.
3. **Stop sequence**, at the entry's stop time: `RECORDER_IFACE_CMD_STOP` on the target Recorder.
   Recommend *not* auto-stopping the source itself (leaves it live for the user or a
   back-to-back entry) — flagged as a v1 default, easy to make configurable later if it turns out
   wrong in practice. Mark `status = "ran"`, set `lastRunEpoch`, and for `"once"` entries also
   flip `enabled` semantics to "expired but kept" per section 3 (no deletion).
4. **Conflict handling**: before firing, if another entry is already `"running"` (regardless of
   which radio), skip this one — set `status = "skipped"`, `lastSkipReason = "<name> is already
   running"`. This is the direct consequence of section 2.1's single-active-source constraint;
   silently pre-empting a running recording to start another is a real correctness risk (an
   in-progress file gets torn down mid-write) and not something to default to without the user
   explicitly asking for pre-emption semantics later.
5. **App-not-running caveat** (must be surfaced in the UI, not just this doc): this is an in-app
   scheduler, not a system service — an entry due while SDR++ itself isn't running simply never
   fires, with no catch-up-on-next-launch in v1. Worth a one-line reminder in the module's own
   panel (e.g. under the table) so it's not a surprise days later.

## 6. Phased build order (each phase independently testable, mirroring how the playback-controls
feature was staged this session)

| # | Phase | Deliverable |
|---|---|---|
| 0 | Skeleton | `misc_modules/recording_scheduler`, CMake option (default ON), empty menu panel, builds/loads/toggles cleanly. No behavior. |
| 1 | Data + list UI | Entry data model, own config file, Add/Duplicate/Delete/Enable table — no scheduling, no radio/recorder integration yet. Verifies persistence and the CRUD/re-arm story (section 3) in isolation. |
| 2 | Source snapshot (RSR200 only) | Add `captureConfigHandler`/`applyConfigHandler` to `SourceManager::SourceHandler` (additive) and implement both in `rsr200_source`. Wire "Update from current settings" + read-only "View saved settings" in the new module. No engine yet — capture/view only. |
| 3 | Recorder snapshot | Extend `recorder_interface.h` with `GET_CONFIG`/`SET_CONFIG`; wire the editable Recorder field section + "Reset to current" button. |
| 4 | Recurrence UI | Once/Daily/Weekly editor, date/time pickers, no engine yet. |
| 5 | Engine | Worker thread, firing/stop sequences, conflict skip logic, status tracking — end to end for RSR200 + one Recorder instance. This is the phase that turns the module from a settings-manager into an actual scheduler. |
| 6 | Second radio | Implement `captureConfigHandler`/`applyConfigHandler` for one more module (RTL-SDR is a reasonable choice — common, and now confirmed to need the same treatment as RSR200 per section 2.2's correction, so it also validates the mechanism on a `devices.<serial>`-keyed module) to prove the mechanism is genuinely per-module-additive, not RSR200-specific in practice. |
| 7 | Polish | Conflict/expiry indicators in the table, next-run countdown, the app-not-running reminder text, any rough edges found using it for real. |

Phases 2 and 3 are the two that touch code outside the new module itself (`source.h`,
`recorder_interface.h`, `rsr200_source`) — everything else is fully contained in
`misc_modules/recording_scheduler`.

## 7. Explicit non-goals (v1)

- No system-level/launchd-style scheduling — the app must be running (section 5.5).
- No true concurrent multi-radio recording — one active source at a time, matching how the rest
  of the app already works (section 2.1); overlaps are skipped, not queued or parallelized.
- No in-scheduler editing of radio-specific fields (gain, sample rate, decimation, etc.) — by the
  user's own explicit scoping (requirement 7); capture/apply/view only.
- No support for source modules that haven't had `captureConfigHandler`/`applyConfigHandler`
  added — the UI should say so plainly per-radio rather than silently no-op.
- No source pre-emption of an in-progress recording to honor a conflicting schedule entry.

## 8. Open decisions to confirm before/at implementation start

1. Module directory/display name — `misc_modules/recording_scheduler` / "Recording Scheduler" is
   a placeholder suggestion, trivial to change.
2. Default CMake option state — recommended `ON` (section 2.4), matching this session's
   RSR200-always-on precedent, but worth a explicit yes/no before phase 0.
3. Stop-sequence source behavior (section 5.3) — leave the source running after the recorder
   stops (recommended default) vs. also stop the source.
4. Whether `"daily"`/`"weekly"` recurrence should have an optional end date, or run indefinitely
   until disabled — section 3's schema has room for one (`daysOfWeek`/`startTimeOfDay` block) but
   no end-date field yet; easy to add before phase 4 if wanted.
