# RIOT Controller Introduction

An offline, Windows-first introduction to the RIOT controller. Explore a simulated
JC-Riot first-start walkthrough, Device Library, Devices and Device Tags without
controller hardware. **No field equipment is connected. Every change affects only
local demo state; no controller address is contacted.**

## Run on Windows

1. Extract the complete package to a folder.
2. Install Python **3.10 or newer** if needed. Enable the Python launcher or
   "Add Python to PATH" during installation. No Python packages are required.
3. Double-click `start.bat`. Keep its console window open.
4. The browser opens **http://127.0.0.1:8080**. If it does not, open that URL yourself.

If port 8080 is occupied, run `start.bat --port 8081` from a terminal.
Use `start.bat --no-browser` to avoid automatic browser opening.
Stop with **Ctrl+C** in the server console (or close that console).
Alternatively run `python server.py --open`.

## JC-Riot first-start walkthrough

A fresh session starts blank: no Devices, Tags, Dashboard placements, alarm rules
or Logger selections. The existing compact Dashboard and shell are reused.

1. Open **Setup -> Devices** and **Add simulated M31**. This adds one illustrative
   M31 instance, **LD02**, with 4 AI, 4 DI and 4 DO points. This represents choosing
   a Device Type and creating an instance; it does not upload or validate TOML.
2. Open **Tags**, select **Dashboard** and **Log** on the desired rows, then use
   the single **Save Changes**. Direct Device Tags are used, without aliases.
   Live refresh does not replace selection drafts or focused controls.
3. Return to **Dashboard** to see changing simulated analog/digital inputs.
   M31 **Configure** offers simulated DO controls and online/offline quality.
   These never operate a UART, Modbus connection or physical output.
4. Open **Datalog**, enable demo logging and **Save Changes**. The initial
   sample is taken on the next local refresh. The default interval is **60
   simulated seconds**; the demo UTC clock runs at **10x** from
   **2026-10-15 12:00:00 UTC**, so this takes about six real seconds.
5. **Download sample CSV** exports up to the last **240 rows**, with UTC time,
   stable fixture identity, reference/name, value, units, quality and a
   **SIMULATED** marker on every row. Offline samples are BAD with an empty value,
   not valid zero. Unselecting Log stops future samples but retains buffered rows.
6. **Reset Demo** always returns the Introduction to blank JC first start,
   clearing its Devices, Tags, placements, selections and sample buffer, and
   restarting the demo clock. This also works from the older scenarios.

The Communications panel is a **read-only illustration** of the saved JC mapping,
not hardware detection or configuration. No full serial, alarm, rule, scaling,
retention or storage editor is replicated. Datalog samples are generated only
while the local pages/API are refreshed (no background acquisition); the bounded
sample buffer clears on server restart. Selections/settings persist in the
existing local state file. Existing saved older scenarios are retained on load;
choose **JC-Riot first start (blank)** or **Reset Demo** to enter this walkthrough.
The separate P0 Tag prototype is unchanged and is not affected by Reset Demo.

### JC walkthrough validation (2026-10-15)

- 31 Introduction/prototype tests passed, including blank reset, changing
  clock/values, interval sampling, exact 240-row buffer bound, BAD CSV values,
  local restart persistence and save-failure rollback.
- JavaScript syntax and `git diff --check` passed.
- Local Edge browser exercised add M31, Dashboard/Log selection, live updates,
  draft preservation/rejected Save, actual CSV attachment download and blank
  reset. CSV headers, selected identity/reference and SIMULATED markers checked.
- Dashboard, Tags, Setup and Datalog checked at **320/1440 CSS pixels**, light
  and dark, without document overflow; desktop Tag rows measured **29.5px**.
- Zero JavaScript errors and zero non-loopback request attempts observed.
  All evidence is desktop simulation, not JC hardware or firmware validation.

## Tag-centric design simulator

**Design and training simulator for the proposed Tag model, not firmware.** It follows the
*RIOT Tag-Centric Design Proposal* (October 24, 2026): every Tag belongs to the device or
function that created it, is removed with it, and is edited in one Tag table.

Start the server as usual (`start.bat`) and open **http://127.0.0.1:8080/tc**, or use the
**Tag-Centric Simulator** link in the Introduction navigation. Pick an **Example site** and
**Load example**: *Plunger-lift well* (M31, MVS, Flow run, Tubing Optimizer v8.00 feature, PID
loop, Calculation; 39 Tags), *Gathering hub* (240 of 256 Tags) or *Blank controller*.

| Page | What it shows |
| --- | --- |
| Dashboard | Tags with Dash ticked, and every owner |
| Tags | The one Tag table: rename, units, 4-20 mA scaling, Log, Dash, High/Low alarms, Modbus address, Used by; one Save for all changed rows |
| Devices | Add an M31 rack, Modbus device (profile), MVS, manual value or calculation; Remove shows what uses each Tag first |
| Device page | Channel counts or selected points (Tags follow), simulated comms loss, Replace device (Tags keep ID and uses) |
| Flow and Control | Flow runs (illustrative rate, hour, 06:00 day, grand) and PID loops: Gain, Reset (repeats/min), Rate (min), error deadband, Auto/Manual with bumpless transfer, and Override control by Low or High Select (FloBoss style), with a simulated process |
| Features | Tubing Optimizer v5 / v8.00 as add-on features with their own Tags; simplified cycle, not the real program |
| Logger | Logging on/off and interval; CSV keyed by Tag ID |
| Maintenance | Measurements: Tags used of 256, memory estimate, owners, PID loops, scan time; audit log |

Limits: 256 Tags, 6 Flow runs, 6 MVS, 16 channels per type per M31 rack, PID loops 16 (a planning
limit until measured on a JC and a Nano). Memory per Tag (116 B) is a planning estimate and the scan
time is this PC's, not the controller's. Profiles are illustrative fixtures. Configuration is saved
in `state/tag_centric_v1.json` (separate from the other demos); live values restart on server start.
Model: `tagsim.py`; pages: `web/tagsim.js`, `web/tagsim.css`; tests: `tests/test_tagsim.py`.

## P0 Tag consolidation prototype

**Configuration design prototype, not implemented in firmware.** From this
folder, run:

```powershell
python server.py --prototype --open --port 8081
```

Or run `start.bat --prototype --port 8081`. Open
**http://127.0.0.1:8081/prototype/tags** if the browser does not open.
The server remains loopback-only; stop it with Ctrl+C. No PlatformIO toolchain
or additional Python packages are required.

The original Introduction also has a **Tag Prototype** navigation link.
`--prototype` starts the original Introduction in memory only: it neither loads
nor writes its saved session. The original `state/current_state.json` is not
reset, overwritten or migrated. Normal launch without this flag retains the
original Introduction's existing save behavior.

P0 uses a separate version-1, `RIOT-P0-DEMO` configuration namespace in
`state/tag_prototype_v1.json`. It is created only on explicit Save Changes and
restored on restart. To maintain another isolated review, use
`--prototype-state "C:\path\to\review.json"`; do not point it at an original
Introduction session. Invalid/version-incompatible prototype state is retained
and displays a load error, never silently reset. Keep runtime JSON out of a
distribution. There is no prototype reset or firmware backup/restore workflow.

### Review walkthrough

1. **Tags** shows compact process rows with source, illustrative value/quality,
   History, Alarms, Usage and the shared Dashboard Configure/Edit treatment.
   Edit opens Source first; History, Alarms, Usage and Advanced diagnostics
   start collapsed. Section links open the relevant section.
2. Rename Sales Static Pressure; enable history, H/HH limits or illustrative
   Dashboard/Flow 1/Rule/MQTT assignments. One **Save Changes** validates the
   entire draft. These are configuration relationships, not running consumers.
3. **Device Library -> Device -> selected Device Tags** preserves M31, Burner,
   Sales Totalizer and ABB NGC8206 fixtures. The IMV25 and Rosemount fixtures are
   added independently. Unselected definitions do not create process Tags.
   A selected, unused Device row's **Add Tag** opens a prefilled source draft;
   **Tags -> Add Tag** can also select a saved point. A new process ID is issued on Save.
   New Tags adopt selected point semantics; M31 current points demonstrate an
   explicit 4-20 mA to gauge-kPa conversion.
4. On Replacement Rosemount, select DP, Static Pressure and Process Temperature
   and Save. **Replace IMV25** previews all three mappings, including units and
   pressure basis. Confirm and Save to replace all three atomically, retaining
   process IDs, names, History/Alarms and consumer assignments. Advanced
   diagnostics shows old/new Device-point IDs, binding revisions and UTC audit
   records. Unselected, disabled or incompatible targets are rejected.
5. Change a Device's simulated state to disconnected, disabled, incompatible,
   or deleted/missing. Deletion requires confirmation and retains unresolved
   process bindings. **Recreate** can reuse its slot label but always issues a
   new Device ID; old bindings never follow the slot automatically.
6. Drafts survive the three-second live refresh. Navigation/Reload warns before
   discarding changes; a change in another tab blocks Save until Reload.
   Advanced diagnostics can simulate a rejected save. The draft and previously
   committed memory/file remain intact; real file-write failures behave likewise.

### P0 limits and evidence

- IMV25/Rosemount values, units and register-independent points are illustrative
  fixtures, not manufacturer mappings or validated profiles. ABB NGC8206 remains
  illustrative too. There is no live acquisition, parser qualification, hardware
  validation, Modbus write or MQTT traffic.
- History/Alarm/Flow/Rule/Dashboard/MQTT settings demonstrate ID relationships
  only. No new history graph/archive, alarm evaluation/ACK, Flow calculation,
  totalization or actual consumer configuration engine is implemented.
- Static point samples are read locally. Last-known values/age are retained
  only for the current server session and binding generation; restart clears
  that sample cache. Never treat these ages as instrument acquisition time.
- Existing process quantity/units/pressure basis cannot change in place in P0.
  Create a different process Tag for a different meaning. Flow input checks
  demonstrate kPa DP, gauge/absolute kPa static pressure and Celsius temperature;
  commissioning atmosphere and reverse-DP behavior are not calculated.
- IDs and tombstones are durable within one prototype store, not production
  globally unique identities. Names must be unique here. Device creation is
  limited to IMV25/Rosemount fixtures; other original fixtures are preserved.
- Counts, allocation strategy and unbounded local audit records are desktop
  demonstration choices, **not proven Nano capacity or firmware memory savings**.
  Firmware sources, binary stores and backend caches are untouched.

## Try it

- Explore Dashboard, then Setup -> Devices -> M31 Rack.
- Toggle simulated DO1..DO4 and return to Dashboard to see the change.
- Try **Burner Offline**, **M31 Communication Failure**, or **High Process Alarm**
  or **NGC Offline** using the Scenario selector. Communication failures show BAD quality and
  last-known values; offline M31 output commands are refused.
- Choose an installed Device as Primary I/O, edit groups and item assignments
  under Setup -> Display -> Dashboard Layout, then **Save Changes**.
- **Reset Demo** returns to **blank JC first start** and clears Introduction demo edits.

Scenario changes reset simulated values and layout to that scenario baseline. JSON scenario files live in
`scenarios/`: `normal_site.json` supplies the base site; the other files supply
offline Devices and value overrides. Edits are saved automatically in
`state/current_state.json` and restored on server restart. The state file is created
on first launch and includes the selected scenario, Device online states, Tag
values, Dashboard settings and recent demo events. Uptime starts fresh each run.
Writes use a temporary file and atomic replacement; failed saves retain the prior
memory/disk state and show an error. Invalid saved state stops startup and is
retained: move it aside manually to start a fresh demo, or correct it before retrying.
Use a writable extracted folder. Do not enter real passwords or site secrets.

## Offline and isolation

All browser assets are included. There are no CDNs, external fonts, APIs, network
scans, Modbus/MQTT clients, or hardware writes. The server binds **127.0.0.1 only**.
The displayed Device IPs are documentation examples and are never contacted.
The JC clock is accelerated synthetic UTC; older scenarios use the local server's
UTC clock. Both are labelled simulated, never RIOT controller time.
Normal operation requires neither repository source nor firmware toolchains.
Distribute this whole folder, including generated assets and scenario files.

## Presentation reuse / maintenance

`sync_ui.py` extracts assets from the controller source, `firmware/src/riot_p4.cpp`
in the private homershaw/Riot repo (branch `P4`), without changing firmware. By
default it reads a checkout at `..\..\P4` (next to riotdemo); pass `--source` otherwise:

| Generated file | Source |
|---|---|
| `web/riot.css` | Nano `CSS` common HMI raw literal |
| `web/dashboard.html` | `DASHC` plus `UXJ` collapse helper |
| `web/dashboard-layout.html` | `DBLAY` configuration UI and its real browser JS |

From this folder, a maintainer runs `python sync_ui.py` after Nano UI changes,
then tests the introduction. Extraction refuses missing or ambiguous markers.
The generated files are shipped in the package; extraction is **not** a startup
requirement. Never hand-edit generated assets.

The shared shell/navigation and introduction-only pages are adapted in
`web/shell.html`, `web/introduction.css`, and `web/introduction.js`.
These pages deliberately simplify the firmware workflows. Drift risk remains in
these adaptations and the server's API subset; re-run tests and browser checks
after regeneration. The simulator mirrors only fields used by the extracted
Dashboard/Layout scripts, not every firmware API field or runtime guarantee.
`/api/sim/state` is the central simulated model.

The header uses the supplied Outlaw Automation logo, copied from
`docs/OAI-LOGO-SMALL.PNG` to `web/OAI-LOGO-SMALL.PNG`. The packaged copy is served
locally and needs no repository access or internet connection.

## First-pass limits

- Profiles/tag counts are illustrative, **not validated production TOML profiles**.
  R44VL04 has a catalog entry only. No TOML upload or Device creation is included.
- No firmware polling, Flow/Tag/Rule/alarm engine, notifications, or totalization.
  Values are scenario samples; displayed flow volumes do not accumulate.
- Maintenance writes (firmware, format, restore) are unavailable, not simulated
  successes. History contains session events only.
- Shared state belongs to one local server; browser tabs share the same demo.
- Layout edits use local simulator JSON, not firmware binary stores or real backup/restore.
- ABB NGC8206 Tags (Methane, Ethane, Heating Value, Analysis Ready) are representative
  examples, not a validated ABB register map, complete gas analysis, or production profile.

## Developer checks

Run `python -m unittest discover -s tests -v`. Tests use temporary loopback servers
and cover API contracts, output isolation, scenarios, and atomic layout drafts.
No PlatformIO build is needed: WEB1 changes no firmware or dependencies.

### P0 validation (2026-10-12)

Source checkpoint: `feature/p4-nano-generic-platform`, `9da8291`.
Local simulator evidence only, not Nano/field validation:

- Python 3.12: 28 tests passed (nine original plus 19 prototype tests), including
  save/restart, replacement continuity, binding compatibility, identity reuse,
  stale revisions, corrupt-file retention and failed-save preservation.
- JavaScript and inline generated scripts parsed in the Edge/Chromium engine;
  actual pages executed without JavaScript errors.
- Local Edge checks exercised keyboard Edit/disclosures/Save, navigation/Reload
  dirty warnings, draft protection across live refresh, simulated failed saves,
  another-tab revision conflicts, delayed pre-save refresh responses, point
  selection and replacement/failure states.
- Rendered pages were reviewed in light/dark at 320, 390, 760 and 1440 px.
  Responsive layouts had no document-width overflow. Edit's computed typography,
  spacing and normal color matched Dashboard Configure in the same theme.
- An actual server restart restored saved identities, settings and source audit.
  Observed browser resource/API requests used only the local loopback host.
- Shared assets were regenerated with `python sync_ui.py`; only the compact
  Configure/Edit CSS and its matching Dashboard rule changed. Firmware was not
  edited. The original saved Introduction session hash remained unchanged.

The integrated browser connection was unavailable, so browser validation used
local Edge automation. That validation tooling is not required to run the demo.
Default-browser opening and distribution remain separately pending; user review
of this new P0 UI is pending. No firmware build/upload or Git staging/commit/push.

### First-pass validation (2026-10-10)

Checked on Windows with Python 3.12.10:

- `start.bat --no-browser` launched the loopback server successfully.
- Seven standard-library tests passed.
- Browser navigation, all four scenarios, DO1..DO4 state reflected on Dashboard,
  Reset Demo, and shared Dashboard Layout save were exercised.
- Dashboard and Setup had no horizontal overflow at phone widths down to 320 px.
- Observed browser resource requests used only `http://127.0.0.1:8080`;
  CSS, scripts, fonts and data require no remote service.

Automatic opening of the user's default browser and a clean-computer download/
extract/install flow still need distribution testing. No firmware build,
upload, staging, or commit was performed.

NGC/persistence follow-up: nine tests passed, including saved-state restart,
selected-scenario reset, corrupt-file retention, and simulated disk-write failure
rollback. Live preview checks confirmed NGC Tags/Offline quality, then verified
that an online override survived an actual server restart and Reset Demo restored
NGC Offline (not Normal Site). Runtime JSON is ignored by Git and should not be
included in a clean distribution. Tests use isolated temporary files.
