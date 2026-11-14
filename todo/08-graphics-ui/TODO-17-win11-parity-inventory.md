---
schema_version: 1
id: win11-parity-inventory
domain: 08-graphics-ui
status: active
title: "TODO-17 -- Windows 11 Parity Inventory"
---

# TODO-17 -- Windows 11 Parity Inventory

> **Validated:** 2026-11-14 | validate-todo-file clean (structure / IO table / XREF / test wiring)
> **Gap-audited:** 2026-11-14 | gap-audit + codex-gap-audit; 11 findings filed (§19 capture matrix and fixtures, boot and recovery screens, profile matrix, kept motion recordings)

> **Goal:** "The same thing, in the same place" becomes measurable. A machine-readable inventory in `docs/design/inventory/` lists every visible element of the Windows 11 interface (shell surfaces, Settings pages, File Explorer, common dialogs, Control Panel, system tools, inbox utilities, shortcuts, gestures, metrics and motion) as one record per element: its name, kind, parent, order, control type, position at a fixed capture profile, and its behaviour in our own words. Every interface section in the roadmap then cites inventory ids, every record is owned by exactly one section that builds it, and a coverage report shows how much of the reference Impossible OS implements. The inventory also records where Windows 11 is inconsistent with itself and the one rule Impossible OS ships instead.

> [!IMPORTANT]
> **Current state:** Measured 2026-11-14. No inventory, schema, capture tool or baseline exists. The design spec ([`docs/design/shell.md`](../../docs/design/shell.md), [`controls.md`](../../docs/design/controls.md), [`tokens.json`](../../docs/design/tokens.json)) describes the shell from memory and public measurements, not from a captured reference. The operator's host is Windows 11 Pro 26H2, build 26300.9818 (an Insider build), with Python 3.14 at `C:\Python314\python.exe`, reachable from WSL through interop. No reference virtual machine exists yet; its installation image is at `R:\11\win11.iso`. The sister project at `D:\Office` already runs this method for Microsoft 365 (schema, capture tool, baseline, coverage and ownership check); its design is the model, its code is not copied.

> [!NOTE]
> **The standard is pixel-perfect (owner direction, 2026-11-14).** Every geometry, colour, material and motion of the built interface matches the Windows 11 reference exactly, and only what is copyrighted is replaced by original work: icons, fonts, wallpaper, sounds and wording. Those replaced regions are the only pixels allowed to differ, and they must occupy the same rectangles: an icon the same size in the same place, text with the same metrics so every line breaks and ends where the reference does. §16 proves the structure and §18 proves the pixels.

> [!IMPORTANT]
> **Capture once (owner direction, 2026-11-14).** The capture campaign of §8 to §14 takes 100 percent of the reference in one pass: every record, every baseline row, and the screenshot of every surface in every state and profile. Every later comparison (§16, §17, §18) reads those stored captures and never starts the VM. The VM is started again only to capture something the campaign missed, recorded as a capture gap, never to compare an implementation against something already captured. The VM, its checkpoint and the capture tools (§2, §3) are kept after the campaign for that purpose and are never cleaned up.

> **Owner direction, 2026-11-14:** the inventory runs first in the plan and is wanted fast, so capture work is grouped: each of §8 to §14 covers one group of surfaces and is reviewed and pushed once. The Impossible OS interface is modelled 1:1 on Windows 11, leaving out anything copyrighted, and is made more consistent than the original wherever Windows 11 disagrees with itself. The inventory follows the Office inventory method, and nothing is skipped: a surface found during capture that no section below names gets a new section here, filed last.

## Inputs

- A clean Windows 11 reference virtual machine (§2): the capture source. Captures never run against the operator's daily desktop, because the shell's surfaces are singletons and hold personal data.
- Windows UI Automation (UIA): names, control types, automation ids, class names and bounding rectangles of every element.
- [`docs/design/shell.md`](../../docs/design/shell.md), [`docs/design/controls.md`](../../docs/design/controls.md), [`docs/design/tokens.json`](../../docs/design/tokens.json): the spec the inventory corrects in §17.
- [`scripts/test-tooling.sh`](../../scripts/test-tooling.sh): the pack the host tests of §1, §3 and §7 join as a nested suite.
- Legal boundary: the inventory records names, positions, types, ids, shortcuts and numbers, with behaviour written in our own words. It never commits Microsoft icons, screenshots, sounds, fonts, wallpaper or help text; screenshots stay outside the repository in `R:\11\ref-shots\` (§3), with only their hash manifest committed. The rule and its checker are §4.
- Vendor verdict: the capture tool is a new host script on the `comtypes` UIA bindings (MIT). The completeness baselines of §5 are read from the reference machine's own files and Registry, plus the control list of the WinUI Gallery (MIT; read its LICENSE file before importing). Nothing else is vendored.
- -> XREF: `TODO-07-ui-accessibility-automation-ime.md §1,§6` -- the semantic tree and automation transport that §16 reads to compare the built OS against the inventory
- -> XREF: `TODO-18-ui-framework-engines.md §1` -- the engines the inventoried surfaces are built on; §15 assigns records to them
- -> XREF: `TODO-19-fluent-component-library.md §1` -- the controls the inventory's control records are owned by
- -> XREF: `TODO-03-theme-system.md §9` -- the Fluent token corpus that §17 re-measures from captured values
- -> XREF: `D00 T05 §6,§9` -- reference screenshot comparison stays the pixel check; this inventory is the structural check beside it
- -> XREF: `D09 T11 §1,§3,§4` -- the Control Panel framework and the applets inventoried in §13

## Outcome

- `docs/design/inventory/schema.json` defines a record; `python3 scripts/inventory/check-inventory.py` validates every inventory file and refuses duplicates, orphans, missing sources and Microsoft text.
- `docs/design/inventory/<surface>/*.json` holds the records of each surface named in §8 to §14, one JSON object per line.
- `docs/design/inventory/REFERENCE.md` pins the reference build, the capture profile and every documented absence.
- `python3 scripts/inventory/inventory-report.py` writes `docs/design/inventory/COVERAGE.md`: records per surface and kind, dispositions, baseline completeness and implemented counts.
- `python3 scripts/inventory/inventory-report.py --owners --check` names every record no roadmap section owns, and every record two sections claim.
- `docs/design/inventory/expected.json` lists every surface, state and profile the one capture campaign must take, and `inventory-report.py --campaign --check` proves each has records and a shot.
- `docs/design/inventory/consistency.json` lists each place Windows 11 disagrees with itself and the single rule Impossible OS ships.

## Record Quick Reference

| Kind                                  | What it records                                                                          |
| ------------------------------------- | ---------------------------------------------------------------------------------------- |
| `surface`, `region`                   | A top-level surface (taskbar, Start, a window) and a named area inside it                |
| `page`, `group`, `setting`            | A Settings or dialog page, a heading under it, one control that holds a value            |
| `control`, `menu`, `menuItem`         | A control, a menu or flyout it opens, one entry of that menu                             |
| `dialog`, `dialogTab`                 | A dialog and one tab of a property sheet                                                 |
| `contextMenu`                         | A right-click menu, with the item kind that triggers it                                  |
| `shortcut`, `gesture`                 | A key chord or a pointer, touch or pen gesture and the command it runs                   |
| `metric`, `motion`, `sound`, `cursor` | A measured size, an animation's duration and curve, an event sound's role, a cursor role |
| `state`                               | A visual state of a control type (rest, hover, pressed, disabled, focused, selected)     |

## Implementation Order

| ⭐  | Order | Deliverable                                                                                 | Depends On   | Status |
| --- | :---: | ------------------------------------------------------------------------------------------- | ------------ | :----: |
| 💎  |   1   | §1 Inventory Schema and Checker                                                             | --           |  [x]   |
| 💎  |   2   | §2 Reference Build and Capture Profile                                                      | §1           |  [ ]   |
| 💎  |   3   | §3 UIA Capture Tool                                                                         | §1, §2, §4   |  [ ]   |
| 💎  |   4   | §4 Microsoft-Free Rule                                                                      | §1           |  [ ]   |
| 💎  |   5   | §5 Completeness Baselines                                                                   | §1, §2, §4   |  [ ]   |
| ⭐  |   6   | §6 Record Dispositions and Consistency Ledger                                               | §1           |  [ ]   |
| 💎  |   7   | §7 Coverage Report and Ownership Check                                                      | §1, §5, §6   |  [ ]   |
| 💎  |   8   | §8 Shell Surfaces Inventory: Desktop, Taskbar, Start and Flyouts                            | §3, §19      |  [ ]   |
| 💎  |   9   | §9 Shell Surfaces Inventory: Windows, Session Screens and Overlays                          | §8           |  [ ]   |
| 💎  |  10   | §10 Settings Inventory: Frame, System, Devices, Network and Personalization                 | §3, §19      |  [ ]   |
| 💎  |  11   | §11 Settings Inventory: Apps, Accounts, Language, Gaming, Accessibility, Privacy and Update | §10          |  [ ]   |
| 💎  |  12   | §12 File Explorer and Common Dialogs Inventory                                              | §9           |  [ ]   |
| 💎  |  13   | §13 Control Panel, System Tools and Inbox Apps Inventory                                    | §12          |  [ ]   |
| 💎  |  14   | §14 Shortcut, Gesture, Metric, Motion and State Catalogues                                  | §8, §10, §12 |  [ ]   |
| ⭐  |  15   | §15 Inventory-to-Roadmap Authoring                                                          | §6, §7, §14  |  [ ]   |
| ⭐  |  16   | §16 Built-OS Layout Check                                                                   | §7, T07 §6   |  [ ]   |
| ⭐  |  17   | §17 Design Spec Reconciliation                                                              | §8, §9, §14  |  [ ]   |
| ⭐  |  18   | §18 Pixel Comparison Against Reference Screenshots                                          | §14, §16     |  [ ]   |
| 💎  |  19   | §19 Capture Matrix, Fixtures and Device Stand-ins                                           | §2, §5       |  [ ]   |

> 💎 = parity work: it measures what Windows 11 already does.
> ⭐ = exclusive work: the consistency ledger, the 1:1 ownership proof and the built-OS check have no Windows or Linux counterpart.

---

## 1. Inventory Schema and Checker

> **Spawned-by:** root

**Design:** n/a -- a host tool and a data format, it draws nothing

The record format and the checker every later section writes against. One JSON object per line, so a diff shows one record.

- [x] Write `docs/design/inventory/schema.json` (JSON Schema 2020-12) for one record
  - Required: `id` (dotted, lower case, `win11.<surface>.<path>`), `surface`, `kind` (the kinds in Record Quick Reference), `label`, `order`, `source`, `capturedFrom`
  - Optional: `parent`, `controlType`, `uiaId`, `className`, `bounds` (`x`, `y`, `width`, `height` in pixels), `captureProfile`, `shortcut`, `itemCount`, `enabled`, `behaviour`, `openedBy`, `disposition`, `inconsistency`, `baselineId`, `baselineMissing`
  - `bounds` requires `captureProfile`; a `menuItem`, `setting`, `dialogTab` or `group` requires `parent`
  - Typed measurements: `measure` (value, unit, readBy) required on a `metric`, `motion` (duration, property, distance, curve) on a `motion`, plus `textMetrics` and `colours` (per theme)
  - `$defs/observation`: one element under another profile, in `<file>.<profile>.json`, keyed by id and profile and kept out of the canonical id, order and count rules (plan review)
  - Downstream fields: `measure.scaling`, `state`, `shortcutScope`, `overridable` for §14, `session` for §19, and `textMetrics` on an observation (completion review)
- [x] Report a conflict at every in-scope line of it, so `--surface` finds a clash with another surface from either side, and skip other surfaces' observation files unread (round and performance review)
- [x] Refuse `NaN`, `Infinity` and overflowing exponents as `non-finite` (round review)
- [x] Refuse a `--surface` that is a path rather than a folder name, name one peer and a count in each conflict message, and accept an observation that carries only `measure` or `textMetrics` (round 2)
- [x] Write `scripts/inventory/check-inventory.py` on the `jsonschema` package: validates every `docs/design/inventory/*/*.json` line, exits 1 naming file, line and rule on the first 50 failures
- [x] Refuse a duplicate `id`, a `parent` that names no record, a `parent` cycle, and an `order` used twice under one parent
- [x] Hold every `itemCount` against the number of child records present, so a file that lost a page, a control or a choice fails
- [x] Refuse a `source` outside the closed list
  - `uia:<build>`, `registry:<build>`, `file:<build>`, `hand:<build>` (a hand-recorded fact, for surfaces UIA cannot reach) and `baseline:<name>`
- [x] Add `--surface <name>` to check one folder (parents still resolve across the tree), and `--json` for a tool
- [x] Write `scripts/inventory/tests/test_check_inventory.py` (stdlib `unittest`): one failing fixture per rule and one passing file, wired into `scripts/test-tooling.sh` as a nested suite
- [x] Add the docs directive and an overview to `docs/design/inventory/index.md` so the folder builds on the site
- [x] Commit: `"inventory: record schema and checker"`

**Test checkpoint:** `python3 scripts/inventory/check-inventory.py` prints `inventory: 0 records, 0 errors` and exits 0 on the empty tree. Each failing fixture exits 1 and names its rule. `python3 scripts/site/build.py --check` passes.

> **Test runner:** N/A (host tooling) | validation: `python3 scripts/inventory/tests/test_check_inventory.py` (33 cases), nested suite `inventory_check` of `scripts/test-tooling.sh`

> **Notes:**
> - Shipped `docs/design/inventory/schema.json` and `scripts/inventory/check-inventory.py`: schema validation per JSON line, then whole-tree rules (duplicate id, orphan parent, cycle, repeated order, itemCount, observation id and profile).
> - Measurements are typed (`measure`, `motion`, `textMetrics`, `colours`), and per-profile captures live in `<file>.<profile>.json` observation files, both from the plan review, so §14 and §17 have a machine contract.
> - `baseline*.json` files are left to the baseline checks of §5; `disposition` (§6) and the label rules (§4) extend this same schema.
> - Overview page: `docs/design/inventory/index.md`.

---

## 2. Reference Build and Capture Profile

> **Spawned-by:** root

**Design:** n/a -- pins the reference machine, it draws nothing

Positions only mean something against one fixed machine. This section builds that machine and writes down what it is.

> [!NOTE]
> **Decision, 2026-11-14:** the reference is a clean Hyper-V virtual machine, not the operator's desktop. The shell's surfaces are singletons that hold personal data (pins, recent files, notifications), and a capture that opens Start on a desktop in use disturbs it. Reversal cost: none, the tool takes the machine as a parameter. The installation image is in place: the operator put the Windows 11 ISO at `R:\11\win11.iso` on 2026-11-14 (7.2 GB). The VM, its disk and its checkpoints live in `R:\11\ios-win11-ref\`, beside the image (operator choice, 2026-11-14).

- [ ] Write `scripts/inventory/ref-vm.ps1`: creates the Generation 2 VM `ios-win11-ref` from the ISO with an unattended answer file
  - 4 vCPU, 4 GiB (operator decision 2026-11-14, so the host stays usable during captures), 80 GiB dynamic disk, TPM enabled, no network after setup
  - ISO: `R:\11\win11.iso`. VM path: `R:\11\ios-win11-ref\` (configuration, `disk.vhdx`, checkpoints); both are parameters with these defaults
  - The answer file makes one local account `ref` and declines every optional prompt
- [ ] Set the VM's display to 1920 by 1080 at 100 percent scaling, display language en-US, regional format en-US, dark mode off (the default), and take the checkpoint `clean`
- [ ] Write `scripts/inventory/ref-run.ps1`: restores the `clean` checkpoint, copies the capture tool in through PowerShell Direct, runs one capture command, and copies the result out
- [ ] Record the reference in `docs/design/inventory/REFERENCE.md`
  - Edition, version, build and update revision read from the VM's Registry, and the date read
  - The capture profile `fhd-100`: 1920 by 1080, 100 percent, en-US, light theme, default accent, default taskbar alignment
- [ ] Record the second and third profiles used by §14: `fhd-100-dark` (dark mode) and `fhd-150` (150 percent scaling)
- [ ] State the rule in `REFERENCE.md`: every captured record carries the build in `capturedFrom`, and a capture from any other build is a new reference recorded there first
- [ ] Add `ref-vm.ps1 --verify`: exits 0 only when the VM exists, the checkpoint exists and every profile value holds
- [ ] Run `ref-vm.ps1` against `R:\11\win11.iso` and record the edition and build it installs in `REFERENCE.md`
- [ ] Commit: `"inventory: reference machine and capture profile"`

**Test checkpoint:** `powershell.exe -File scripts/inventory/ref-vm.ps1 --verify` prints `ref-vm: ios-win11-ref ok, profile fhd-100` and exits 0. `REFERENCE.md` names one build. Platform: the operator's Windows host with Hyper-V.

---

## 3. UIA Capture Tool

> **Spawned-by:** root

**Design:** n/a -- a host tool, it draws nothing

One tool walks a surface's UI Automation tree and writes records. It reads structure and never reads what a person typed or chose.

- [ ] Write `scripts/inventory/capture-ui.py` for Windows Python on `comtypes`: `--surface <name> --out <file>`; it refuses to run when any value of the capture profile does not hold, naming the value
- [ ] Walk from the root element the surface module names and emit one record per element
  - Fields: `kind`, `label`, `parent`, `order`, `controlType`, `uiaId`, `className`, `source: uia:<build>`
  - `bounds` is relative to the surface's own top left corner
- [ ] Give each surface a module under `scripts/inventory/surfaces/<name>.py` with three functions
  - `open()` brings the surface up by its own command or key, `root()` finds the element, `close()` dismisses it the way a user would
- [ ] Expand one level: open each menu, flyout, combo box and split button, record its entries as `menuItem` records with `itemCount` on the opener, and close it again with nothing chosen
- [ ] Never record data: no text of an edit box, no state of a check box, no entry of a list that holds the machine's own data
  - Such lists hold files, apps, devices, networks or accounts; each is one record with `controlType: list` and no children, named in the module's `ENVIRONMENT_LISTS`; the synthetic fixtures of §19 supply the row template, captured separately
- [ ] Add `--merge`: carries the hand-written fields (`behaviour`, `disposition`, `inconsistency`) across a re-capture by `id`, refreshes every captured fact, and keeps no record the capture did not see
- [ ] Run `check-inventory.py` on the result before writing it; a file that fails is not published
- [ ] Save a screenshot with every capture: each surface in each state and profile to `R:\11\ref-shots\<surface>\<state>.<profile>.png`
  - Commit only the manifest `docs/design/inventory/shots.json` (surface, state, profile, size, SHA-256); `--verify-shots` re-hashes the store and names every missing or changed file
- [ ] Record an element UIA exposes no name for under its `uiaId` or class, and list it in `build/inventory/<surface>-unnamed.json` for a hand-written label
- [ ] Write `scripts/inventory/tests/test_capture.py` against a recorded UIA tree fixture (a JSON dump, no live window) and wire it into `scripts/test-tooling.sh`
- [ ] Commit: `"inventory: UI Automation capture tool"`

**Test checkpoint:** inside the reference VM, `python capture-ui.py --surface notepad-smoke --out notepad.json` writes at least 40 records and `check-inventory.py` passes on them; with the display at 125 percent the same command exits 2 with `profile fhd-100: scaling is 125, expected 100`. The fixture tests pass on Linux with no Windows present.

---

## 4. Microsoft-Free Rule

> **Spawned-by:** root

**Design:** n/a -- a repository rule and its checker, it draws nothing

The inventory holds facts. Names of controls, their order, their sizes and their shortcuts are facts; a sentence of explanation, an icon and a screenshot are someone's work.

- [ ] Write the rule in `docs/design/inventory/REFERENCE.md`
  - Recorded: labels, positions, types, ids, shortcuts and measured numbers
  - Never recorded: help text, tooltips longer than a label, descriptions under a setting, icons, images, sounds, fonts and links to Microsoft
- [ ] Every `behaviour` is written in our own words from using the control; the capture tool never fills it
- [ ] Add to `check-inventory.py`: refuse a `label` longer than 80 characters (a sentence, not a name), any `http` link, and any `behaviour` that equals a `label`
- [ ] Add `scripts/inventory/check-no-ms-assets.py`: refuses a staged image, sound, font or cursor file whose bytes match a file of the reference VM
  - The hash list `scripts/inventory/ms-hashes.txt` (SHA-256 and size only, no bytes) is made once by `ref-run.ps1` from the VM's `C:\Windows` during the campaign and committed, so the check runs in CI without the VM
- [ ] Record replaced trade mark names in `docs/design/inventory/names.json`: the reference name, our name, and the reason
  - Microsoft Store, OneDrive, Copilot, Edge, Xbox, Microsoft account, Windows Hello, Windows Security
- [ ] Add lint coverage: `scripts/lint.sh` runs `check-inventory.py` when a file under `docs/design/inventory/` is staged
- [ ] Commit: `"inventory: facts only, no Microsoft text or assets"`

**Test checkpoint:** a fixture record with a 120-character label fails with `label is a sentence`; a fixture with a Microsoft link fails with `link in record`. `bash scripts/lint.sh` passes on the clean tree.

---

## 5. Completeness Baselines

> **Spawned-by:** root

**Design:** n/a -- reads the reference machine's own lists, it draws nothing

A capture proves what it saw. A baseline proves nothing was missed: an independent list of everything that should be there, read from the reference machine itself.

- [ ] `settings` baseline: read the Settings search index the build ships and write `docs/design/inventory/settings/baseline.json`
  - Source: the `AllSystemSettings_*.xml` file under `C:\Windows\ImmersiveControlPanel\Settings\`
  - One row per setting with its id, page id and deep-link name, no description text
- [ ] `control-panel` baseline: read the canonical names and CLSIDs under `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\ControlPanel\NameSpace` and the `.cpl` files under `C:\Windows\System32`
- [ ] `known-folders` baseline: read every entry under `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\FolderDescriptions` (name, category, relative path, parent)
- [ ] `start-apps` baseline: read the app list of the clean VM (`Get-StartApps`) as name and kind, so §8 and §13 can prove every inbox app has an owner
- [ ] `controls` baseline: import the control page list of the WinUI Gallery at a pinned commit as control name and category only
  - MIT: read the LICENSE file and add the notice to `CREDITS.md` in the same commit
- [ ] `classic-controls` baseline: the window classes `comctl32.dll` registers, read by enumerating the classes of a test dialog in the VM
- [ ] `shell-commands` baseline: the `shell:` folder names and the verbs registered under `HKCR\*\shell`, `HKCR\Directory\shell`, `HKCR\Directory\Background\shell` and `HKCR\Drive\shell`
- [ ] Each baseline file records its source and the build, and `check-inventory.py` accounts for every baseline row
  - A row is matched by a captured record (`baselineId`), listed as documented absent in `REFERENCE.md` with a reason, or counted as pending
- [ ] Commit: `"inventory: completeness baselines from the reference build"`

**Test checkpoint:** `python3 scripts/inventory/check-inventory.py --baselines` prints one line per baseline with its row count, and every count is above zero: `settings`, `control-panel`, `known-folders`, `start-apps`, `controls`, `classic-controls`, `shell-commands`.

---

## 6. Record Dispositions and Consistency Ledger

> **Spawned-by:** root

**Design:** n/a -- decisions about records, it draws nothing

Two decisions are made per record, once, and written on it: what we do with it, and which rule it follows where the original has two.

> [!TIP]
> Windows 11 ships two context menus, two settings programs, three generations of dialogs and four title bar heights. The ledger is where Impossible OS does better: one rule per question, written down before anything is built.

- [ ] Add `disposition` to the checker as required on every non-catalogue record
  - `build` (as captured), `equivalent` (our own version of something tied to a Microsoft cloud service, account, store or assistant) or `excluded` (with a `reason`)
- [ ] Write `docs/design/inventory/consistency.json`: one entry per inconsistency with `id`, `question`, `variants` (the record ids that disagree), `rule` (what Impossible OS does) and `reason`
- [ ] Seed the ledger with the known disagreements, each resolved to one rule
  - Context menus: the modern menu with a "more options" classic menu behind it. Rule: one menu, every verb in it, grouped and ordered by one scheme
  - Settings and Control Panel: the same setting reachable in two programs with two layouts. Rule: one setting has one page; the Control Panel frame lists the same pages
  - Dialog generations: property sheets, task dialogs and modern content dialogs. Rule: one dialog frame, one button order, one spacing scale
  - Title bars: 32, 40 and 48 pixel bars and three caption button styles. Rule: one height per density and one caption style
  - Corner radii, focus rectangles, scroll bars, tooltips and menu fonts that differ between old and new programs. Rule: the token in `tokens.json`, everywhere
  - Dark mode gaps: dialogs and tools that stay light. Rule: every surface follows the theme
  - Keyboard: access keys present in old dialogs and missing in new pages. Rule: every command has an access key and every page is reachable by keyboard alone
- [ ] Add `inconsistency` on a record as the ledger id it belongs to; the checker refuses an id the ledger does not define and a ledger variant that names no record
- [ ] Write the disposition rule for cloud-tied records in `REFERENCE.md`
  - Sign-in with a Microsoft account, the Store, cloud file sync, the assistant, the news feed, advertising and telemetry consent pages
- [ ] Commit: `"inventory: dispositions and the consistency ledger"`

**Test checkpoint:** a fixture record without `disposition` fails; a fixture `inconsistency` naming an undefined ledger id fails. `consistency.json` holds at least the seven seeded entries and passes its own schema.

---

## 7. Coverage Report and Ownership Check

> **Spawned-by:** root

**Design:** n/a -- a report generator, it draws nothing

The report is the measure of progress, and the ownership check is the proof that nothing is ownerless and nothing is claimed twice.

- [ ] Write `scripts/inventory/inventory-report.py`, which writes `docs/design/inventory/COVERAGE.md`
  - Records per surface and kind, dispositions per surface, baseline rows matched, absent, pending and unmatched, and implemented counts
- [ ] Define ownership: a roadmap section owns a record when its body carries `**Inventory:** <id or id prefix>`; a prefix owns every record under it that no longer prefix owns
- [ ] Add `--owners --check`: exits 1 listing every `build` or `equivalent` record with no owner and every record with two owners of the same prefix length
- [ ] Define implemented: a record counts when its owning section is shipped with both stamps
  - Read from the classifier in `.claude/hooks/sequencer_triage.py`: reuse its function, do not restate its rule
- [ ] Make the pre-commit lint refuse a stale `COVERAGE.md` when an inventory file or a TODO file is staged
- [ ] Add `--campaign --check`: exits 1 naming every row of §19's `expected.json` with no records or no shot, every baseline row unmatched and every `unnamed` element without a label
  - The campaign of §8 to §14 ends, and the VM is shut down, only when it exits 0; a later miss is filed as a capture gap in `REFERENCE.md`
- [ ] Write `docs/design/inventory/authoring.json`: the surface to owning-file map §15 fills, read by the report to print pending surfaces
- [ ] Write `scripts/inventory/tests/test_report.py` with a three-record fixture tree: one owned, one ownerless, one doubly owned
- [ ] Commit: `"inventory: coverage report and 1:1 ownership check"`

**Test checkpoint:** on the fixture tree, `inventory-report.py --owners --check` exits 1 and prints exactly two lines, `no owner: win11.fixture.b` and `two owners: win11.fixture.c`. On the real tree `COVERAGE.md` regenerates byte-identically twice in a row.

---

## 8. Shell Surfaces Inventory: Desktop, Taskbar, Start and Flyouts

> **Spawned-by:** root

**Design:** n/a -- records the reference, draws nothing

**Inventory:** writes the files under `docs/design/inventory/desktop/`, `taskbar/`, `start/` and `flyouts/` named in each item

One item is one surface: write its capture module, run it in the reference VM, hand-write the fields the tool cannot read, and commit that surface's file once `check-inventory.py --surface <name>` passes. The section is reviewed and pushed once, after its last surface (operator decision 2026-11-14: capture work is grouped so each group is one review).

- [ ] Desktop Surface: writes `docs/design/inventory/desktop/desktop.json`
  - Capture the desktop surface: the icon grid (cell size, margins, label width, label line count, selection and hover rectangles) as `metric` records, with the Recycle Bin as the only default icon
  - Capture the desktop context menu (empty area): every entry and submenu in order
    - View, Sort by, Refresh, Paste, Paste shortcut, Undo, New, Display settings, Personalize, and the terminal entry, with each submenu's entries
  - Capture the classic menu behind "Show more options" for the empty area, for a file icon, a folder icon, a shortcut icon and the Recycle Bin, as separate `contextMenu` records
  - Capture the New submenu's entries and the order they appear in
  - Record by hand the desktop's behaviours
    - Rubber-band selection, drag to move, Ctrl-drag copy, Alt-drag shortcut, F2 rename, auto arrange, align to grid, show desktop icons
    - Icon size by Ctrl and wheel: the three sizes and their pixel values
  - Capture the wallpaper fit modes (Fill, Fit, Stretch, Tile, Center, Span) as records under the surface; the wallpaper image itself is never recorded
  - Capture the drag-and-drop feedback: the drop tip text shapes ("Move to", "Copy to", "Create link in") as labels and the cursor role for each
  - Done when: `check-inventory.py --surface desktop` passes; `desktop.json` holds at least one `contextMenu` record for each of the five trigger kinds and the `itemCount` of each matches its children.
- [ ] Taskbar: writes `docs/design/inventory/taskbar/taskbar.json`
  - Capture the bar: height, icon size, button width and spacing, the active and running indicator sizes, at centre alignment and at left alignment, as `metric` records
  - Capture each default item in order
    - Start, Search (its four display modes: hidden, icon, icon and label, box), Task View, the widgets entry, pinned apps, running apps, the tray, the clock, the notification bell, the show-desktop edge
  - Capture the taskbar context menu (empty area) and the app button context menu
    - The jump list frame: pinned, recent, tasks, the app name, pin or unpin, close window, close all windows
  - Capture the hover thumbnail: one window, several windows (the row of previews), the close button on a preview, the preview's title row, and the delay before it shows
  - Record by hand the button states
    - Not running, running, active, attention (flashing and steady), progress (normal, paused, error, indeterminate), badge, grouped windows
  - Capture the overflow: what happens when buttons no longer fit (the overflow button and its flyout)
  - Record by hand the behaviours
    - Drag to reorder, drag a file over a button to switch, middle click for a new window, Shift-click, Ctrl-click cycling, Win and a digit, auto-hide and its reveal edge
  - Capture the taskbar on a second display and its three button modes (all taskbars, main and where open, where open), on the topology of §19
  - Done when: `check-inventory.py --surface taskbar` passes; the file holds a `metric` record for bar height, icon size and button width at both alignments, and one `state` record per button state listed above.
- [ ] System Tray and Overflow: writes `docs/design/inventory/taskbar/tray.json`
  - Capture the tray's fixed order
    - The hidden icons chevron, app icons, the input indicator, the pen and touch keyboard entries, the network, volume and battery group (one button), the clock, the notification bell
  - Capture the hidden icons flyout: its grid, cell size and the drag between the flyout and the bar
  - Capture the clock: its two lines, the seconds option, the short and long date forms, and its tooltip's shape (label only)
  - Capture the context menu of the clock, of the network, volume and battery group, and of an app icon as the frame only
  - Record by hand the icon states as `state` records naming the role, never the artwork
    - Network: wired, wireless by strength, no internet, disconnected, airplane
    - Volume: four levels and muted. Battery: levels, charging, saver
  - Capture the privacy indicators: microphone in use, location in use, and the screen sharing or recording indicator
  - Capture the input indicator flyout (the language list frame) and the touch keyboard button
  - Done when: `check-inventory.py --surface taskbar` passes with `tray.json` present; every tray item has an `order` and the three status icons have their `state` records.
- [ ] Start Menu: writes `docs/design/inventory/start/start.json`
  - Capture the Start surface: its size, corner radius, position against the taskbar at centre and left alignment, and each region
    - Regions: search box, pinned, the all-apps entry, recommended, the bottom bar
  - Capture the pinned grid: columns and rows, cell size, page dots, folders, and the three layout choices
    - Folders: create by drag, open, rename, the folder's own grid
    - Layout choices: more pins, default, more recommendations
  - Capture the all-apps list: letter headings, the letter jump grid, folders in the list, and the category and grid views where the build offers them
    - The list itself is an environment list
  - Capture the recommended region as a frame: row layout, the "more" entry, and the empty state. Its entries are data and are not recorded
  - Capture the bottom bar: the account button and its menu, the folder shortcuts that can sit beside power, and the power button and its menu
    - Folder shortcuts: Settings, File Explorer, Documents, Downloads, Music, Pictures, Videos, Network, Personal folder
    - Power menu: Lock, Sleep, Shut down, Restart, and the update variants
  - Capture the context menu of a pinned app, an all-apps entry, a folder and a recommended entry
  - Record by hand the behaviours: type to search, drag to reorder, drag out to unpin, keyboard navigation between regions, and which key closes the menu
  - Match every `start-apps` baseline row to the section of §13 that inventories that app, or mark it excluded with a reason
  - Done when: `check-inventory.py --surface start` passes; the power menu and the account menu each have their `itemCount`; `check-inventory.py --baselines` shows zero unmatched `start-apps` rows.
- [ ] Search Flyout: writes `docs/design/inventory/start/search.json`
  - Capture the search surface before typing: the box, the filter row, recent searches as a frame, and the suggested apps row as a frame
    - Filter row: All, Apps, Documents, Web, Settings, Folders, Photos and the "more" list
  - Capture the results layout: the best match row, the category groups and their order, and the detail pane on the right with its action list for an app, a file, a folder and a setting
  - Capture the action list of each result kind as `menuItem` records
    - Open, Run as administrator, Open file location, Pin to Start, Pin to taskbar, Uninstall, Copy path
  - Record by hand the query behaviours: prefix and word matching, the calculation and unit answers, the filter prefixes typed as words, and what Enter, Ctrl-Shift-Enter and Tab do
  - Give web results, the highlights panel and cloud content the disposition decided under §6 and record them as frames only
  - Capture the search settings menu at the top right and its entries
  - Done when: `check-inventory.py --surface start` passes with `search.json` present; each of the four result kinds has its action list recorded with a matching `itemCount`.
- [ ] Quick Settings: writes `docs/design/inventory/flyouts/quick-settings.json`
  - Capture the flyout: its width, the tile grid (columns, tile size, the split tile with its arrow), the sliders, and the bottom row (battery, edit, settings)
  - Capture every tile the build offers and its two parts where it splits
    - Wi-Fi, Bluetooth, VPN, Airplane mode, Accessibility, Battery saver or Energy saver, Night light, Mobile hotspot, Nearby sharing, Cast, Project, Live captions, Rotation lock, Focus
  - Capture each tile's second page, each with its back button and footer link
    - The Wi-Fi network list frame, the Bluetooth device list frame, the VPN list, the Accessibility switches, the Cast and Project lists
  - Capture the volume slider row: the mute button, the slider, the output chooser page (devices, spatial sound, the per-app volume mixer frame)
  - Capture the brightness slider and when it is absent (a desktop display without brightness control)
  - Capture edit mode: the pin and unpin control, drag to reorder, the Add list, and Done
  - Capture the media control card above the tiles: its size, the app row, the title rows, and the three transport buttons
  - Record by hand the keyboard path through the flyout and the state of each tile (off, on, unavailable)
  - Done when: `check-inventory.py --surface flyouts` passes; every tile record has a `state` list and every split tile has a child page record.
- [ ] Notification Center, Calendar and Toast: writes `docs/design/inventory/flyouts/notifications.json`
  - Capture the notification center: the header (title, the do-not-disturb button, clear all), the group per app, the notification card, and the empty state
  - Capture the card's slots
    - The app row, title, body lines, the image slot, the progress slot, the input slot, the button row (up to five), and the expand, settings and close controls
  - Capture the calendar: the collapsed date row, the month grid (6 by 7), the month, year and decade zoom levels, the scroll between months, and the focus session row beneath it
  - Capture a toast on screen: its position, size, the stack of several toasts, the time it stays, and the swipe or close dismissal
    - Overflowing groups, long text, expanded and collapsed cards come from the notification fixtures of §19
  - Record by hand the toast kinds
    - Plain, with buttons, with a text reply, with a selection, with a progress bar, an alarm or reminder that stays, an incoming call, and an urgent toast that breaks through do not disturb
  - Capture the card's context menu: turn off for this app, go to notification settings, make high priority
  - Record the motion: toast slide in and out, the center opening, and a card collapsing on dismissal, as `motion` records measured under §14
  - Done when: `check-inventory.py --surface flyouts` passes; one `control` record exists per card slot and one `state` record per toast kind.
- [ ] Commit: `"inventory: shell surfaces inventory captured"` (one commit per surface file, the last one closes the section)

**Test checkpoint:** every surface item's own "Done when" line holds, `python3 scripts/inventory/check-inventory.py` exits 0 over the whole tree, and `inventory-report.py` lists every file this section names with a record count above zero. Platform: the reference VM for capture; the checker runs on Linux.

---

## 9. Shell Surfaces Inventory: Windows, Session Screens and Overlays

> **Spawned-by:** root

**Design:** n/a -- records the reference, draws nothing

**Inventory:** writes the files under `docs/design/inventory/window/`, `session/` and `flyouts/overlays.json` named in each item

One item is one surface: write its capture module, run it in the reference VM, hand-write the fields the tool cannot read, and commit that surface's file once `check-inventory.py --surface <name>` passes. The section is reviewed and pushed once, after its last surface (operator decision 2026-11-14: capture work is grouped so each group is one review).

- [ ] Window Chrome and System Menu: writes `docs/design/inventory/window/chrome.json`
  - Capture the window frame of a classic program and of a modern program, at active and inactive
    - Title bar height, caption button size and hit area, icon and title position, border thickness, corner radius, shadow extent
  - Capture the caption buttons' states (rest, hover, pressed, the close button's own hover colour role) and the maximize and restore glyph swap
  - Capture the system menu (Alt-Space and title bar right-click): Restore, Move, Size, Minimize, Maximize, Close with its shortcut text
  - Record by hand the resize areas: edge and corner grip thickness inside and outside the frame, the minimum window size, and the cursor role per area
  - Record by hand the title bar behaviours
    - Double-click to maximize, drag from maximized to restore, shake to minimize others, drag to a screen edge, double-click the top edge to fill vertically
  - Capture the tool window, dialog and borderless variants: which buttons each has and its title bar height
  - Add the four title bar heights found to the consistency ledger entry of §6 as its variants
  - Record the accent colour on title bars and borders option and what it changes
  - Done when: `check-inventory.py --surface window` passes; `metric` records exist for title bar height, caption button size, border, radius and shadow for both program generations, and each carries the `inconsistency` id where the two differ.
- [ ] Snap, Alt+Tab, Task View and Virtual Desktops: writes `docs/design/inventory/window/switching.json`
  - Capture the snap layout flyout on the maximize button: each layout, its zones as fractions of the screen, and the layouts offered at 1920 by 1080 and at a wide and a tall display
  - Capture the snap bar at the top edge when a window is dragged, and the edge and corner drop zones with their preview outline
  - Capture snap assist: the picker that offers the other windows for the empty zones, and the snap group shown on the taskbar and in Alt+Tab
  - Record by hand the Win and arrow key table: every transition between left, right, quadrant, maximized, restored and minimized, and the move between displays
  - Capture Alt+Tab: the grid, thumbnail size rule, the title row, the selection outline, the close button on a thumbnail, wrap-around, and the Ctrl-Alt-Tab sticky form
  - Capture Task View: the window grid, the desktop strip, new desktop, rename, reorder, and the two context menus
    - A desktop's menu: rename, choose background, move left or right, close
    - A window's menu: snap, move to desktop, show on all desktops, close
  - Record by hand the desktop switching keys and the four-finger gesture, and what happens to windows when a desktop closes
  - Done when: `check-inventory.py --surface window` passes; every snap layout has its zones recorded as four numbers that sum to the whole screen, and the arrow key table has no unlisted transition.
- [ ] Lock, Sign-In and Secure Screens: writes `docs/design/inventory/session/secure.json`
  - Note: UI Automation does not reach the secure desktop. These records are `hand:` records, measured from screenshots kept in the scratch directory.
  - Record the lock screen: the clock and date position and size, the status row, the network and accessibility buttons, and how it dismisses (key, click, swipe)
  - Record the sign-in screen
    - The account picture, name, the credential box and its reveal and submit buttons, the other users list
    - The sign-in options row (password, PIN, and the other methods as roles) and the bottom right buttons (network, accessibility, power)
  - Record the wrong password, locked account, password expired and first sign-in states and the text slots each uses (slot and length, not Microsoft's wording)
  - Record the security options screen (Ctrl-Alt-Delete): Lock, Switch user, Sign out, Change a password, Task Manager, Cancel, and the bottom right buttons
  - Record the elevation prompt: its two forms (consent and credentials), the header colours by publisher trust as roles, the details expander, the button order, and the dimmed desktop behind it
  - Record the shut down dialog (Alt-F4 on the desktop) and its list, the "apps are preventing shut down" screen, and the sign-out and restart progress screens
  - Record the power button menu on the sign-in screen and the accessibility flyout's list
  - Done when: `check-inventory.py --surface session` passes; every record's `source` is `hand:<build>` and no record carries a label longer than the checker allows.
- [ ] First-Run Setup: writes `docs/design/inventory/session/setup.json`
  - Record the setup frame from the `pre-oobe` checkpoint of §19: the page layout (illustration side, content side), the button row, the progress between pages, and the accessibility and volume buttons
  - Record each page in order as a `page` record with its controls: region, keyboard layout, second layout, network, device name, account, privacy choices, and the final preparing screen
  - Give every page tied to a Microsoft account, cloud backup, advertising or the assistant its disposition under §6
  - Record the keyboard path through setup and which pages can be skipped
  - Record the "getting ready" animation as a `motion` record: its duration, text slot changes and background behaviour
  - Record the first desktop after setup: which windows open, which toasts appear, and the default pins on the taskbar and Start as frames
  - Done when: `check-inventory.py --surface session` passes; `setup.json` lists the pages in order with no gap in `order`, and each cloud-tied page has `disposition` other than `build`.
- [ ] Boot, Update, Stop and Recovery Screens: writes `docs/design/inventory/session/boot.json`
  - Captured from the host side (Hyper-V screenshot of the VM framebuffer), because UIA does not run there; every record is `hand:<build>` with a shot
  - The boot logo and spinner, the boot manager menu, the "working on updates" screens before and after restart, and the Safe mode desktop's marks
  - The stop error screen, raised by a host NMI (`Debug-VM -InjectNonMaskableInterrupt` with `NMICrashDump` set), including its progress and restart
  - The recovery environment: automatic repair, the troubleshoot menu, startup settings and the reset path, each page as a `page` with its choices; a checkpoint is taken before each so the VM returns clean
  - Reconcile each screen against its owner in `D01 T15`, `D01 T14` and `D01 T22`, and note gaps in §15
  - Done when: `check-inventory.py --surface session` passes; every screen listed has records and a shot, and the stop error records name each text slot without Microsoft's words.
- [ ] Shell Overlays: writes `docs/design/inventory/flyouts/overlays.json`
  - Capture the volume and brightness indicator: its position, size, the icon slot, the bar, the number, and its timeout
  - Capture clipboard history (Win-V): the item card, pin, delete, clear all, the tab row it shares with the emoji panel, and the empty and disabled states
  - Capture the emoji panel (Win-period): the tab row, the search box, the category row, the grid, and the skin tone chooser
    - Tabs: recent, emoji, GIF, kaomoji, symbols, clipboard; GIF gets its disposition under §6
  - Capture the power user menu (Win-X) and every entry in order
  - Capture the Run dialog: the icon, the label, the combo box, the three buttons and the elevated variant's extra line
  - Capture the Project flyout (Win-P) and its four modes, and the Cast flyout (Win-K) as a frame
  - Capture the input switcher (Win-Space) and the touch keyboard: its layouts (default, small, split, traditional), the key rows, and the handwriting panel as a frame
    - The shifted, symbol, emoji and contextual (password, number, URL) layers, and IME composition with its candidate window and paging, typed from fixed text fixtures in the Japanese and Chinese input methods
  - Capture the capture bar (Win-Shift-S): the mode buttons (rectangle, window, full screen, freeform), the video toggle, and close
  - Capture voice typing (Win-H): the bar, the microphone button, the settings flyout
  - Done when: `check-inventory.py --surface flyouts` passes; `overlays.json` holds one `surface` record per overlay named above, each with at least one child.
- [ ] Commit: `"inventory: shell surfaces inventory captured"` (one commit per surface file, the last one closes the section)

**Test checkpoint:** every surface item's own "Done when" line holds, `python3 scripts/inventory/check-inventory.py` exits 0 over the whole tree, and `inventory-report.py` lists every file this section names with a record count above zero. Platform: the reference VM for capture; the checker runs on Linux.

---

## 10. Settings Inventory: Frame, System, Devices, Network and Personalization

> **Spawned-by:** root

**Design:** n/a -- records the reference, draws nothing

**Inventory:** writes the files under `docs/design/inventory/settings/` named in each item

One item is one surface: write its capture module, run it in the reference VM, hand-write the fields the tool cannot read, and commit that surface's file once `check-inventory.py --surface <name>` passes. The section is reviewed and pushed once, after its last surface (operator decision 2026-11-14: capture work is grouped so each group is one review).

- [ ] Settings Frame and Home: writes `docs/design/inventory/settings/frame.json`
  - Capture the Settings window: the navigation pane (account header, search box, the category list in order), the content area's maximum width, the breadcrumb title, and the back button
  - Capture the page templates as `metric` and `control` records
    - The settings card (icon, title, description slot, the control on the right), the expander card, the group heading, the link list at the page foot, and the "related settings" block
  - Capture each control form a card can end in
    - Toggle with its On or Off word, combo box, button, chevron to a sub-page, slider, radio list, link, and the info bar above a group
  - Capture the Home page: its cards as frames and which are cloud-tied
  - Capture settings search: the suggestion list, the results page, and the no-results state
  - Capture the narrow layout: at what width the navigation pane collapses to a button and how the page reflows
  - Record by hand the deep-link behaviour: a page opens directly from a link, and the back button's history
  - Write the per-page capture module the next twelve sections share, `surfaces/settings_page.py`
    - It opens a page by its deep-link name, scrolls it to the end, expands every expander, and records page, group, setting and choice records
  - Done when: `check-inventory.py --surface settings` passes; `frame.json` holds the eleven categories in order and one `control` record per card ending form.
- [ ] Settings: System, Part 1: writes `docs/design/inventory/settings/system-1.json`
  - Note: Each page is captured with §10's module: every group, every setting with its control type, every fixed choice of a list, every sub-page.
  - Display
    - Brightness, night light and its sub-page, colour profile, HDR, scale, resolution, orientation, advanced display, graphics
    - Multiple displays: the arrangement canvas, identify, detect, the mode list
  - Sound: output and input device lists as frames, volume, mono audio, the device properties sub-page, the volume mixer sub-page, troubleshoot links, and the classic sound link
  - Notifications
    - The master switch, do not disturb and its schedule sub-page, the priority notifications sub-page
    - The per-app list frame and the per-app sub-page's controls
  - Focus: session length, the four switches for what a session hides
  - Power and battery: power mode, screen and sleep timeouts, energy saver, battery usage chart frame, lid and power button choices
  - Storage: the usage breakdown frame, storage sense and its sub-page, cleanup recommendations, and advanced storage settings
    - Advanced sub-pages: disks and volumes, storage spaces, backup options, drive optimization
  - Nearby sharing and Multitasking: every switch and list, including the snap options expander and the Alt+Tab and desktop choices
  - Reconcile against the `settings` baseline: every baseline row under these pages is matched, documented absent or hardware-gated with the reason
  - Done when: `check-inventory.py --surface settings` passes; every page listed above exists as a `page` record whose `itemCount` equals its children, and no baseline row under these pages is unmatched.
- [ ] Settings: System, Part 2: writes `docs/design/inventory/settings/system-2.json`
  - Activation: state row, edition row, the change key and upgrade entries, with licensing given its disposition
  - Troubleshoot: recommended preference and the other troubleshooters list as a frame
  - Recovery: fix problems, reset this PC and each page of its wizard, advanced startup, go back
  - Projecting to this PC and Remote Desktop: every switch, list and the confirmation dialog
  - Clipboard: history, sync across devices (cloud-tied), clear data
  - Optional features: the installed list frame, the add dialog, the history page, and the classic features link
  - About: device specifications rows, Windows specifications rows, rename, the copy buttons, and the related links
  - For developers or Advanced: every group and switch the build offers (terminal default, file explorer options, virtual workspaces, sudo, long paths)
  - Reconcile against the `settings` baseline for these pages
  - Done when: `check-inventory.py --surface settings` passes; the reset wizard's pages are recorded in order under Recovery, and no baseline row under these pages is unmatched.
- [ ] Settings: Bluetooth and Devices: writes `docs/design/inventory/settings/devices.json`
  - The category page: the device tiles row as a frame, the Bluetooth switch, add device and each page of its dialog (the three device kinds)
  - Devices: the list frame, a device's card and menu, and the device discovery choices
  - Printers and scanners: the list frame, add device, a printer's page (queue, properties, preferences links), and the printing preference switches
  - Mobile devices, Cameras: every control, with phone linking given its disposition
  - Mouse: primary button, pointer speed, enhance precision, scroll choices, and the additional settings links
  - Touchpad: every gesture group (taps, scroll and zoom, three-finger, four-finger, advanced gestures) with each list's fixed choices
  - Pen, Touch, AutoPlay and USB: every switch and list
  - Reconcile against the `settings` baseline
    - A page absent on the VM because the hardware is absent is recorded as hardware-gated in `REFERENCE.md` and reached through its device stand-in of §19, or the physical capture of §19 where none exists
  - Done when: `check-inventory.py --surface settings` passes; the touchpad gesture lists carry their fixed choices as `menuItem` records, and every hardware-gated page has records and a shot from §19's stand-in or physical capture.
- [ ] Settings: Network and Internet: writes `docs/design/inventory/settings/network.json`
  - The category page: the status header (connection name, properties, data usage) and the list of pages
  - Wi-Fi: the switch, show available networks, manage known networks and a network's page, hardware properties, random hardware addresses
  - Ethernet: authentication, the network profile choice, metered connection, and the two assignment dialogs
    - IP assignment and its edit dialog: automatic and manual, both address families
    - DNS assignment and its edit dialog with the encryption choices
  - VPN: add a connection and every field of its dialog, the per-connection page, the two advanced switches
  - Mobile hotspot, Airplane mode, Proxy (automatic and manual groups and their dialogs), Dial-up
  - Advanced network settings
    - The adapter list frame and an adapter's expander, data usage, hardware and connection properties
    - Advanced sharing settings and its three profile groups, network reset
  - Reconcile against the `settings` baseline for these pages
  - Done when: `check-inventory.py --surface settings` passes; the IP and DNS edit dialogs are recorded as `dialog` records with every field, and no baseline row under these pages is unmatched.
- [ ] Settings: Personalization: writes `docs/design/inventory/settings/personalization.json`
  - The category page: the preview, the theme row as a frame
  - Background
    - The kind list (picture, solid colour, slideshow, and the cloud-tied kind), the recent images row, browse, the fit list
    - The colour grid and custom colour dialog, slideshow interval and shuffle
  - Colours
    - Mode (light, dark, custom with its two lists), transparency, accent (automatic or manual)
    - The accent grid's rows and columns, custom colour, the two "show accent on" switches
  - Themes: the theme grid frame, save, the four parts of a theme (background, colour, sounds, cursor), contrast themes link, desktop icon settings dialog
  - Lock screen: the kind list, the status app list, show picture on sign-in, screen timeout and screen saver links, and the screen saver dialog
  - Text input: the theme grid, the custom theme editor's tabs and controls
  - Start: the layout choice, each switch, and the Folders sub-page
  - Taskbar: taskbar items, system tray icons, other system tray icons, and taskbar behaviours with every list's fixed choices
  - Fonts and Device usage: the fonts page frame, a font family's page, and the usage switches with their disposition
  - Done when: `check-inventory.py --surface settings` passes; the accent grid is recorded as rows and columns counts, and the taskbar behaviours expander has every list's choices.
- [ ] Commit: `"inventory: settings inventory captured"` (one commit per surface file, the last one closes the section)

**Test checkpoint:** every surface item's own "Done when" line holds, `python3 scripts/inventory/check-inventory.py` exits 0 over the whole tree, and `inventory-report.py` lists every file this section names with a record count above zero. Platform: the reference VM for capture; the checker runs on Linux.

---

## 11. Settings Inventory: Apps, Accounts, Language, Gaming, Accessibility, Privacy and Update

> **Spawned-by:** root

**Design:** n/a -- records the reference, draws nothing

**Inventory:** writes the files under `docs/design/inventory/settings/` named in each item

One item is one surface: write its capture module, run it in the reference VM, hand-write the fields the tool cannot read, and commit that surface's file once `check-inventory.py --surface <name>` passes. The section is reviewed and pushed once, after its last surface (operator decision 2026-11-14: capture work is grouped so each group is one review).

- [ ] Settings: Apps: writes `docs/design/inventory/settings/apps.json`
  - Installed apps: search, filter and sort lists, the three view modes, an app's menu, and the advanced options page
    - Menu: advanced options, modify, uninstall
    - Advanced options: permissions, background, defaults, terminate, repair, reset
  - Advanced app settings: where to get apps, share across devices, app execution aliases frame, archive apps
  - Default apps: search by file type or link type, the app list frame, an app's page (its types), and the chooser dialog
  - Offline maps, Apps for websites, Video playback: every control, with maps given its disposition
  - Startup: the list frame, the per-app switch and its impact label, the sort list
  - Optional features link and the Resume page where the build offers it
  - Reconcile against the `settings` baseline for these pages
  - Done when: `check-inventory.py --surface settings` passes; the default app chooser is recorded as a `dialog`, and no baseline row under these pages is unmatched.
- [ ] Settings: Accounts: writes `docs/design/inventory/settings/accounts.json`
  - The category page and Your info: the picture choices (camera, browse), the account kind row, the cloud sign-in entry with its disposition
  - Sign-in options
    - Every method as a role (face, fingerprint, PIN, security key, password, picture password) with its set up, change and remove dialogs
    - The additional settings group: dynamic lock, restart apps, show account details, use sign-in info after update
  - Email and accounts, Family, Windows Backup, Access work or school: frames, with each cloud-tied page's disposition
  - Other users: add account and every page of its dialog (local account path), an account's expander (change type, remove), and kiosk setup
  - Passkeys and Linked devices where the build offers them
  - Record the change password, create PIN and remove account dialogs with every field and the button order
  - Reconcile against the `settings` baseline for these pages
  - Done when: `check-inventory.py --surface settings` passes; the local account creation path is recorded page by page, and every cloud-tied page has a `disposition` other than `build`.
- [ ] Settings: Time and Language: writes `docs/design/inventory/settings/time-language.json`
  - Date and time: set automatically, the time zone switch and list, sync now, calendars in the taskbar, and the change date and time dialog
    - The zone list is fixed and is recorded by zone id, not display text
  - Language and region
    - The display language list, preferred languages and a language's options page (language pack, speech, handwriting, keyboards)
    - Country or region, regional format and its change formats sub-page (calendar, first day, short and long date, short and long time)
  - Typing: touch keyboard, text suggestions, autocorrect, multilingual suggestions, typing insights, and advanced keyboard settings
    - Advanced: input method override, per-app input, the language bar, hot keys
  - Speech: language, microphone setup, voices list frame
  - Record the administrative language settings link and the classic dialog it opens as owned by §13
  - Record the format strings each regional choice produces as examples of the pattern, not text
  - Reconcile against the `settings` baseline for these pages
  - Done when: `check-inventory.py --surface settings` passes; the regional format sub-page has all six format lists, and no baseline row under these pages is unmatched.
- [ ] Settings: Gaming: writes `docs/design/inventory/settings/gaming.json`
  - Game Bar: the controller button switch and shortcut entry, with the overlay itself given its disposition
  - Captures: the save location, record what happened, frame rate, quality, audio and cursor choices
  - Game Mode: the switch and the graphics link
  - Record which of these pages depend on a Microsoft service and give each its disposition
  - Decide under §6 whether the category ships as Gaming or folds its two real settings (capture and game mode) into System, and record the decision in the ledger
  - Reconcile against the `settings` baseline for these pages
  - Done when: `check-inventory.py --surface settings` passes; every record under the category has a `disposition`, and the ledger holds the category decision.
- [ ] Settings: Accessibility: writes `docs/design/inventory/settings/accessibility.json`
  - Vision group, part 1
    - Text size (slider and preview), visual effects (scroll bars, transparency, animation, dismiss notifications after)
    - Mouse pointer and touch (style, colour, size, touch indicator), text cursor (indicator, size, colour, thickness)
  - Vision group, part 2
    - Magnifier (zoom, increment, view, the reading and smoothing choices), colour filters (the six filters and the preview)
    - Contrast themes (the four themes and the editor's eight colour slots), narrator (every group as settings; the voices as a frame)
  - Hearing group: audio (mono, flash screen choices), captions (style list and the style editor's tabs), live captions
  - Interaction group: speech, keyboard, mouse keys, eye control
    - Keyboard: sticky, filter and toggle keys with every sub-switch, on-screen keyboard, underline access keys, the print screen key choice
  - Record each feature's keyboard shortcut and whether the shortcut can be turned off
  - Reconcile against the `settings` baseline for these pages
  - Done when: `check-inventory.py --surface settings` passes; the contrast theme editor has eight colour slot records, and sticky, filter and toggle keys each have their sub-switches.
- [ ] Settings: Privacy and Security: writes `docs/design/inventory/settings/privacy.json`
  - Security group: the security app link, find my device, device encryption, and the developer entry, each with its disposition
  - Windows permissions group: general, speech, inking and typing, diagnostics and feedback, activity history, search permissions, searching Windows
    - Searching Windows: indexing mode, the excluded folders frame, the advanced indexing link
  - App permissions group: one page per capability, each with the same three-level shape
    - Shape: device switch, apps switch, per-app list frame, recent activity
    - Location, camera, microphone, voice activation, notifications, account info, contacts, calendar, phone calls, call history, email, tasks, messaging, radios, other devices, app diagnostics
    - Automatic file downloads, documents, downloads folder, music library, pictures, videos, file system, screenshot borders, screenshots and screen recording
  - Record the shared shape once as a template record and each capability as an instance, so the builder writes one page and a table
  - Give telemetry, advertising id and cloud-tied pages their disposition; record what Impossible OS shows instead (a plain statement that nothing is sent)
  - Reconcile against the `settings` baseline for these pages
  - Done when: `check-inventory.py --surface settings` passes; every capability page listed above exists as a record that names the template record as its `parent` pattern, and no baseline row under these pages is unmatched.
- [ ] Settings: Windows Update: writes `docs/design/inventory/settings/update.json`
  - The category page: the status header in each state, the main button, and the update row with its progress
    - States: up to date, checking, available, downloading, installing, restart required, failed, paused, each reached through the local update source of §19
  - Pause updates and its duration list, update history (the grouped list frame, uninstall updates link), and advanced options
    - Advanced: other products, get me up to date, metered download, restart notification, active hours and its dialog, optional updates
    - Advanced, continued: delivery optimization and its sub-page, recovery link, restart apps, configured policies
  - The insider programme page as excluded, with the reason
  - Record the restart prompts outside Settings: the tray icon state, the toast, and the power menu's update variants, as owned by §8
  - Map each record to the Impossible OS update service's own states under `D10 T03` and record gaps as follow-ups in §15
  - Reconcile against the `settings` baseline for these pages
  - Done when: `check-inventory.py --surface settings` passes; the status header has one `state` record per state listed, and `check-inventory.py --baselines` shows zero unmatched `settings` rows across §10 to §11.
- [ ] Commit: `"inventory: settings inventory captured"` (one commit per surface file, the last one closes the section)

**Test checkpoint:** every surface item's own "Done when" line holds, `python3 scripts/inventory/check-inventory.py` exits 0 over the whole tree, and `inventory-report.py` lists every file this section names with a record count above zero. Platform: the reference VM for capture; the checker runs on Linux.

---

## 12. File Explorer and Common Dialogs Inventory

> **Spawned-by:** root

**Design:** n/a -- records the reference, draws nothing

**Inventory:** writes the files under `docs/design/inventory/explorer/` and `dialogs/` named in each item

One item is one surface: write its capture module, run it in the reference VM, hand-write the fields the tool cannot read, and commit that surface's file once `check-inventory.py --surface <name>` passes. The section is reviewed and pushed once, after its last surface (operator decision 2026-11-14: capture work is grouped so each group is one review).

- [ ] File Explorer Frame: writes `docs/design/inventory/explorer/frame.json`
  - Capture the window's rows: the tab row, the navigation row, the command bar, the body and the status bar
    - Tab row: tab size, close button, new tab button, tab overflow, drag to reorder and to tear out
    - Navigation row: back, forward, up, refresh, address bar, search box
  - Capture the address bar's two forms: breadcrumb segments with their drop-down lists and the overflow chevron, and the editable text form with its history list and autosuggest
  - Capture the command bar in order, and how it changes with the selection and with the folder kind
    - New and its menu, cut, copy, paste, rename, share, delete, Sort, View, Filter, the "see more" menu, and the Details or Preview toggle
  - Capture the navigation pane: its entries, expand arrows, indent, the pin glyph, and the pane's own context menu and options
    - Entries: Home, Gallery, the cloud entry, the pinned folders group, This PC, drives, Network, Linux
    - Options: show all folders, expand to current folder, show libraries, show network, show This PC
  - Capture the details pane and the preview pane: width, the splitter, the property rows frame, and the empty state
  - Capture the status bar: item count, selected count and size, and the two view switch buttons
  - Record by hand the keyboard map of the frame: Alt-D, Ctrl-L, Ctrl-E, Ctrl-T, Ctrl-W, Ctrl-N, Alt-arrows, Alt-P, Alt-Shift-P, F4, F6 pane order
  - Done when: `check-inventory.py --surface explorer` passes; the command bar's entries are recorded for three selection cases (nothing, one file, one folder) and the navigation pane's top-level entries have a contiguous `order`.
- [ ] File Explorer Views and Columns: writes `docs/design/inventory/explorer/views.json`
  - Capture each view mode as `metric` records: cell size, icon size, label lines, spacing, and the compact view variant
    - Extra large, large, medium and small icons, list, details, tiles, content
  - Capture the details view: the header, the default columns per folder kind, and the "choose details" dialog
    - Header: sort arrow, filter drop-down, resize, reorder, size to fit
    - Folder kinds: general, documents, pictures, music, videos; the dialog is a frame over the `known-folders` and property baselines
  - Capture Sort, Group by and Filter menus with their fixed entries, and the group header row with its collapse arrow and count
  - Capture the selection model by hand: click, Ctrl-click, Shift-click, rubber band, item check boxes, select all, invert, and the focus rectangle against the selection fill
  - Capture each special view
    - Home (quick access, favourites, recent as frames), Gallery, This PC (the drive tile with its usage bar and the device groups), Network
    - Recycle Bin (its own columns and command bar), a search results view (its header, the search options menu), a library, a compressed folder, an empty folder
  - Capture in-place rename, the new-item placeholder name pattern, and the info tip's row shape
  - Capture the drag image and the drop highlight in the list and on the navigation pane
  - Done when: `check-inventory.py --surface explorer` passes; all eight view modes have their metrics, and the five folder kinds each have a default column list.
- [ ] File Explorer Context Menus and Commands: writes `docs/design/inventory/explorer/menus.json`
  - Capture the modern context menu for each trigger
    - A file (document, image, archive, executable, shortcut), a folder, a drive, a removable drive, the folder background
    - A navigation pane entry, a library, a Recycle Bin item, several items at once
  - Capture the icon row at the top of the modern menu (cut, copy, rename, share, delete) and when each is present
  - Capture the classic menu behind "Show more options" for the same triggers, and record both under one ledger entry of §6 with the merged order Impossible OS ships
  - Capture the New menu, the Send to menu, the Open with submenu frame, and the right-drag menu (copy here, move here, create shortcuts here, cancel)
  - Record by hand the verb table: for each verb its label, access key, shortcut, default (bold) status, and the item kinds it applies to
  - Capture the "see more" menu of the command bar and its extras per folder kind
    - Menu: undo, pin to quick access, select all, select none, invert selection, properties, options
    - Extras: map network drive, eject, format, mount, extract all, empty Recycle Bin
  - Match every `shell-commands` baseline verb to a record or document it absent
  - Done when: `check-inventory.py --surface explorer` passes; each trigger has both its modern and classic `contextMenu` records carrying the same `inconsistency` id, and no `shell-commands` baseline row is unmatched.
- [ ] File Explorer Dialogs: writes `docs/design/inventory/explorer/dialogs.json`
  - Capture the Properties sheet for a file, a folder, a shortcut, a drive and several items: each tab with every control and the button row
    - Tabs: General, Shortcut, Security, Details, Previous Versions, Sharing, Customize, Tools, Hardware, Quota, Compatibility
  - Capture the dialogs the sheet opens: Advanced Attributes, Change Icon, the permissions editor and its advanced form, Remove Properties, the folder picture chooser
  - Capture Folder Options: the General, View and Search tabs, with the View tab's advanced settings tree recorded entry by entry (each is a fixed setting)
  - Capture the file operation dialog: the compact and detailed forms, the speed graph frame, pause and cancel, several operations stacked, and the taskbar progress it drives
  - Capture the conflict and error dialogs
    - Replace or skip, the compare chooser with its two columns, folder merge, "file in use", "access denied" with its elevation button
    - "Path too long", "not enough space", and the delete confirmations (to Recycle Bin, permanent, too large)
  - Capture the wizards: extract compressed folder, map network drive, add a network location, format, and the eject and "problem ejecting" messages
  - Capture Open with: the app list frame, the "always" and "just once" buttons, and the "choose another app" form
  - Done when: `check-inventory.py --surface explorer` passes; every property sheet tab is a `dialogTab` whose `itemCount` matches, and the Folder Options View tree has one `setting` per entry.
- [ ] Common Dialogs: writes `docs/design/inventory/dialogs/common.json`
  - Capture the Open and Save dialogs
    - The embedded Explorer frame, the file name row, the type filter, the encoding or extra control slot, the button row
    - The overwrite prompt, and the multi-select and folder-pick variants
  - Capture the older dialogs still shipped
    - Browse for Folder (tree form), the classic colour dialog (basic grid, custom grid, the picker), the font dialog (three lists, effects, sample, script), Find and Replace, Page Setup
  - Capture the Print dialogs: the classic one (printer list frame, range, copies, the preferences button) and the modern one (preview side, settings side, more settings)
  - Capture the message box: the four icons as roles, the button sets, default button, the width rule for long text, and the sound each plays
    - Button sets: OK, OK Cancel, Yes No, Yes No Cancel, Retry Cancel, Abort Retry Ignore, Cancel Try Continue
  - Capture the task dialog: main instruction, content, command links, radio buttons, the verification check box, the expander, the footer, and the progress bar
  - Capture the credential prompt, the About dialog frame, and the "program not responding" and crash report dialogs
  - Record all dialog frames found under the dialog generations ledger entry of §6, with the one frame, margin scale and button order Impossible OS ships
  - Done when: `check-inventory.py --surface dialogs` passes; the seven message box button sets each have a record with their order, and every dialog carries the dialog generations `inconsistency` id.
- [ ] Commit: `"inventory: file explorer and common dialogs inventory captured"` (one commit per surface file, the last one closes the section)

**Test checkpoint:** every surface item's own "Done when" line holds, `python3 scripts/inventory/check-inventory.py` exits 0 over the whole tree, and `inventory-report.py` lists every file this section names with a record count above zero. Platform: the reference VM for capture; the checker runs on Linux.

---

## 13. Control Panel, System Tools and Inbox Apps Inventory

> **Spawned-by:** root

**Design:** n/a -- records the reference, draws nothing

**Inventory:** writes the files under `docs/design/inventory/control-panel/`, `tools/` and `apps/` named in each item

One item is one surface: write its capture module, run it in the reference VM, hand-write the fields the tool cannot read, and commit that surface's file once `check-inventory.py --surface <name>` passes. The section is reviewed and pushed once, after its last surface (operator decision 2026-11-14: capture work is grouped so each group is one review).

- [ ] Control Panel Frame and Applet List: writes `docs/design/inventory/control-panel/frame.json`
  - Capture the Control Panel window: the address bar, the search box, "View by" (category, large icons, small icons), and the category layout (eight categories with their task links)
  - Capture the all-items list in icon view: every applet by canonical name, in order, matched to the `control-panel` baseline
  - Capture each category's second-level page: its headings and task links
  - Decide each applet's home under the Settings and Control Panel ledger entry of §6
    - Which Settings page of §10 to §11 owns the same setting, and what the Control Panel entry opens in Impossible OS
  - Capture the Windows Tools folder: every entry by name, matched to the sections of §13 that inventory it
  - Capture the task pane links on the left of applet pages as a frame
  - Done when: `check-inventory.py --surface control-panel` passes; `check-inventory.py --baselines` shows zero unmatched `control-panel` rows, and every applet record names its home.
- [ ] Control Panel Property Sheets: writes `docs/design/inventory/control-panel/sheets.json`
  - Sound: Playback, Recording, Sounds, Communications tabs, a device's Properties sheet and each of its tabs, and the sound event list as event names
  - Mouse and Keyboard properties: every tab and control (buttons, pointers and the scheme list, pointer options, wheel, hardware; speed, hardware)
  - System Properties: Computer Name, Hardware, Advanced, System Protection, Remote, and the dialogs they open
    - Performance options with its three tabs and the visual effects list entry by entry
    - Virtual memory, user profiles, startup and recovery, environment variables and its editors
  - Power Options: the plan list, the plan's page, and the advanced settings tree entry by entry
  - Region and Date and Time: every tab, the customize format sheet's tabs, additional clocks, internet time
  - Every remaining applet with disposition `build`: the page or sheet, every tab, every control
    - Programs and Features, Windows Features, Default Programs, User Accounts, Credential Manager, Indexing Options, File History, Backup and Restore, Recovery, Internet Options, Devices and Printers, Fonts
    - Ease of Access Center, Network and Sharing Center, Firewall, Security and Maintenance, Storage Spaces, Work Folders, Speech Recognition, Colour Management, AutoPlay, Troubleshooting
  - Record each sheet's home under the Settings and Control Panel ledger entry of §6, as for the applets above
  - Split this section into one section per applet group if the record count passes 1,500, filing the new sections last in this file
  - Done when: `check-inventory.py --surface control-panel` passes; every applet of §13 with disposition `build` has at least one `dialog` or `page` record here, and each `dialogTab` has a matching `itemCount`.
- [ ] Task Manager: writes `docs/design/inventory/tools/task-manager.json`
  - Capture the frame: the navigation pane and its collapsed form, the title bar search box, the command bar per page, and the settings page
  - Processes: the three groups (apps, background, system), every available column, the context menu, expand to windows, and the command bar
    - The heat colouring levels and the status column icons are recorded as roles
    - Command bar: run new task, end task, efficiency mode
  - Performance: the resource list, each resource's graph frame and statistics rows, the graph's context menu, and the link to the resource monitor
    - Resources: CPU, memory, each disk, each network adapter, each GPU
    - Graph menu: logical processors, kernel times, summary view, copy
  - App history, Startup apps, Users: every column, context menu and command
  - Details: every available column (the full "select columns" list), the context menu, and the Services page with its columns and menu
    - Menu: priority, affinity dialog, analyse wait chain dialog, create dump, UAC virtualization, go to service
  - Settings: default start page, update speed, window management switches
  - Record the dialogs: run new task, end process confirmation, set affinity, and the summary and always-on-top forms
  - Done when: `check-inventory.py --surface tools` passes; each of the seven pages is a `page` record, and the Processes and Details column lists carry their `itemCount`.
- [ ] Management Consoles: writes `docs/design/inventory/tools/consoles.json`
  - Capture the console frame once: menu bar, toolbar, the scope tree, the result pane, the actions pane, the status bar, and the Action and View menus
  - Device Manager: the category tree as category names, the toolbar, the View menu modes, a device's context menu, and the device Properties sheet
    - Sheet tabs: General, Driver, Details and its property list, Events, Resources, Power Management
  - Disk Management: the volume list columns, the graphical view, every context menu, and each wizard and dialog
    - Graphical view: disk row, partition block, legend as roles. Menus: disk, partition, unallocated space
    - Wizards and dialogs: new simple volume, extend, shrink, change letter, format, initialize disk, convert, properties
  - Event Viewer: the tree, the event list columns, the detail pane's two tabs, the actions pane, filter and custom view dialogs with every field
  - Services: the list columns, the extended and standard tabs, a service's Properties sheet (General, Log On, Recovery, Dependencies), and the toolbar's control buttons
  - Task Scheduler: the tree, the task list, the detail tabs, and the create task dialog with each tab
    - Tabs: General, Triggers and the trigger editor, Actions and the action editor, Conditions, Settings
  - The remaining consoles: the tree, the list columns, and each Properties sheet
    - Computer Management, Local Users and Groups, Performance Monitor, Shared Folders, Local Security Policy, Group Policy editor, Certificates
  - Split into one section per console if the record count passes 1,500, filing the new sections last in this file
  - Done when: `check-inventory.py --surface tools` passes; the shared console frame is recorded once and each console names it as `parent` pattern; every wizard is recorded page by page with contiguous `order`.
- [ ] System Utilities: writes `docs/design/inventory/tools/utilities.json`
  - Registry Editor: menu bar and every menu, the address bar, the tree, the value list columns, the context menus, the five value editors, find, permissions, export and import, favourites
  - Resource Monitor: the five tabs, each table's columns, the graphs pane, and the filter check boxes
  - System Information: the tree's category names, the two-column list, find, and the File menu
  - System Configuration: the General, Boot, Services, Startup and Tools tabs with every control and the boot advanced options dialog
  - Disk Cleanup and Optimize Drives: the category list as entries, the buttons, the schedule dialog
  - The security app: its navigation list and each area's page as frames
    - The scanning engine gets its disposition; the firewall, account and device pages are recorded as settings
  - The smaller tools as frames with every tab and control
    - Character Map, the remote desktop client, the memory diagnostic, the reliability monitor, the system restore wizard, the graphics diagnostic tool
  - The command prompt and console host property sheet: Options, Font, Layout, Colours, Terminal tabs
  - Done when: `check-inventory.py --surface tools` passes; every Windows Tools entry of §13 resolves to a record in this section.
- [ ] Inbox Apps: Notepad, Calculator, Paint: writes `docs/design/inventory/apps/notepad.json`, `calculator.json`, `paint.json`
  - Notepad: the tab row, the menu bar and every menu entry with its shortcut, the editor area, the status bar, and the settings page
    - Status bar items: position, characters, zoom, line ending, encoding
    - Also the formatting bar where the build has one, the find and replace bar, go to, and the font page
  - Notepad dialogs and states: unsaved changes prompt, the unsaved tab indicator, session restore on start, the encoding and line ending choices on save, print and page setup
  - Calculator: the navigation menu's mode list, the display, the history and memory panes, always-on-top form
    - Modes: standard, scientific, graphing, programmer, date calculation, and every converter
  - Calculator key grids: every key of the standard, scientific and programmer grids in row and column order with its keyboard key
    - Programmer mode also has the word size, base and bit keypad controls
  - Paint: the menu, the toolbar groups in order, every tool and shape by name, the colour palette size, the canvas handles, the status bar, and the layers pane
    - Groups: selection, image, tools, brushes, shapes, size, colours, layers
  - Paint dialogs: resize and skew, image properties, edit colours, save formats list, and the assistant features with their disposition
  - Reconcile each app against the app TODO that will own it (`D11 T08`, `D11 T09`, `D10 T02`; `D09 T10` is now a pointer to `D11 T08`) and note in §15 where two files claim one app
  - Done when: `check-inventory.py --surface apps` passes; every menu has an `itemCount`, every calculator key has a `shortcut`, and the three files together name no record without a `disposition`.
- [ ] Inbox Apps: Capture, Media, Clock and Terminal: writes `docs/design/inventory/apps/snipping.json`, `media.json`, `clock.json`, `terminal.json`
  - Snipping Tool: the main window (new, mode list, delay list, record toggle), the editor's toolbar, and its settings page
    - Toolbar: pen, highlighter, eraser, ruler, crop, text actions, shapes, undo, redo, save, copy, share
  - Photos: the navigation list, the gallery grid, the viewer's toolbar and film strip, the info pane, and the editor's tabs with each control
    - Editor tabs: crop, adjust, filter as names, mark-up; cloud sources get their disposition
  - Media Player: the navigation list, the library views, the now playing page, the transport bar in order, the mini player, the equalizer, and the settings page
  - Clock: the five pages (focus sessions, timer, alarm, stopwatch, world clock), each page's cards and add or edit dialogs
  - Sound Recorder and Sticky Notes: the recording list, the waveform frame and transport; the note window's toolbar and the notes list, with sync given its disposition
  - Terminal: the tab row and its drop-down, the settings tree, the command palette, the search bar, and the pane commands
    - Settings tree: startup, interaction, appearance, colour schemes, rendering, actions, each profile's pages
  - Reconcile each app against its owning TODO (`D11 T12`, `D11 T11`, `D11 T06`, `D11 T13`, `D09 T08`) and note gaps in §15
  - Done when: `check-inventory.py --surface apps` passes; each app file has one `surface` record and every settings page under it is a `page` with a matching `itemCount`.
- [ ] Accessibility Tools: writes `docs/design/inventory/tools/accessibility.json`
  - Magnifier: the toolbar (zoom out, zoom in, the level, views list, reading controls, settings), the three views (full screen, lens, docked), and the lens size limits
  - On-Screen Keyboard: every key by row and column for the standard layout, the options dialog, the prediction bar, and the hover and scan modes
  - Narrator: the home window's links, and the command table (every command with its key chord) recorded as `shortcut` records
  - Voice access and live captions: the bar, its menu and settings, and the caption window's position choices
  - The accessibility flyout on the sign-in screen and in quick settings: the switch list
  - Record the system-wide effects each tool has on other surfaces as `behaviour` on the tool's record
    - The focus highlight, the text cursor indicator, colour filters applied at the compositor
  - Reconcile against `D10 T06 §1` to `§8` , `T07 §3` (the screen-reader hooks Narrator sits on) and `D11 T13 §4` (the On-Screen Keyboard), and note gaps in §15
  - Done when: `check-inventory.py --surface tools` passes; the on-screen keyboard has one record per key with a contiguous `order` per row, and every Narrator command has a `shortcut`.
- [ ] Commit: `"inventory: control panel, system tools and inbox apps inventory captured"` (one commit per surface file, the last one closes the section)

**Test checkpoint:** every surface item's own "Done when" line holds, `python3 scripts/inventory/check-inventory.py` exits 0 over the whole tree, and `inventory-report.py` lists every file this section names with a record count above zero. Platform: the reference VM for capture; the checker runs on Linux.

---

## 14. Shortcut, Gesture, Metric, Motion and State Catalogues

> **Spawned-by:** root

**Design:** n/a -- records the reference, draws nothing

**Inventory:** writes the files under `docs/design/inventory/input/` and `system/` named in each item

One item is one surface: write its capture module, run it in the reference VM, hand-write the fields the tool cannot read, and commit that surface's file once `check-inventory.py --surface <name>` passes. The section is reviewed and pushed once, after its last surface (operator decision 2026-11-14: capture work is grouped so each group is one review).

- [ ] Keyboard Shortcut Catalogue: writes `docs/design/inventory/input/shortcuts.json`
  - Note: No machine list of shortcuts exists, so this catalogue is recorded by hand and each chord is pressed once in the VM to confirm what it does.
  - Windows key chords: every letter, digit, arrow, punctuation and function key combination with the Windows key, alone and with Shift, Ctrl and Alt
  - Window management: Alt-Tab family, Alt-F4, Alt-Space, Alt-Esc, Ctrl-Alt-Tab, the snap chords, the virtual desktop chords
  - Text editing: caret movement, selection, clipboard, undo and redo, word and paragraph moves, and the delete variants, as the one table every text control follows
  - Dialogs and controls: Tab order, access keys, Enter and Esc defaults, F4, Space, arrow navigation in lists, trees, tabs, menus and grids, type-ahead find
  - File Explorer and the desktop: every chord of §12 collected into the table
  - Taskbar, Start and flyouts: focus cycling (Win-T, Win-B, Win-A, Win-N), Shift and Ctrl click variants, and the context menu key
  - Record each chord's scope (global, shell, window, control) and whether an app may override it
    - A chord two surfaces use for different things becomes a ledger entry of §6
  - Done when: `check-inventory.py --surface input` passes; no two `shortcut` records share a chord and scope, and every Windows key chord has a `behaviour`.
- [ ] Pointer, Touch and Pen Gesture Catalogue: writes `docs/design/inventory/input/gestures.json`
  - Mouse: click, double-click, right-click, middle-click, wheel and horizontal wheel, Ctrl-wheel zoom, drag threshold, hover time, and the extra buttons (back, forward)
    - Double-click time and distance thresholds are read from the system metrics
  - Touchpad: one, two, three and four finger taps, two-finger scroll and pinch, three and four finger swipes in each direction with their default actions
  - Touch: tap, press and hold (the ring and the menu), swipe from each screen edge, pan with inertia and the boundary bounce, pinch, and the hit-target size rule
  - Pen: tap, barrel button, eraser, hover, press and hold, and the pen menu as a frame
  - Drag and drop: modifier keys and the effect each selects, the drop cursors by role, spring-loaded folders and taskbar buttons, auto-scroll at edges, and Esc to cancel
  - Scrolling: lines per notch, the smooth scroll curve, the scroll bar's click-to-page and Shift-click-to-jump, the overlay scroll bar's reveal and collapse timing
  - Record each numeric threshold as a `metric` record with its source (the system metric or parameter name it was read from)
  - Done when: `check-inventory.py --surface input` passes; every threshold is a `metric` with a `source`, and each touchpad gesture names its default action.
- [ ] Metrics, Motion, Sound and Cursor Catalogue: writes `docs/design/inventory/system/metrics.json`, `motion.json`, `sounds.json`, `cursors.json`
  - Add `capture-ui.py --metrics`: reads every system metric and every system parameter that is a size, a time or a count, and writes one `metric` record each with the name it was read by
  - Record the system colour roles (the element names, not the values) and the type ramp: each named text style's size, weight and line height as measured from the reference surfaces
  - Record text metrics per type ramp style as numbers: ascent, descent, line gap, and the advance width of each printable ASCII character and of 20 fixed test strings, so a metric-compatible font can be proven to lay out identically
  - Add `scripts/inventory/measure-motion.py`: records a surface into the scratch directory and writes one `motion` record
    - It records at the display's refresh rate, tracks one edge of the moving element, and writes duration, distance and the nearest cubic curve
    - The recording is kept in `R:\11\ref-shots\motion\` with its frame timestamps and raw edge samples, hashed into `shots.json`, so a curve is re-fitted without the VM
  - Measure the motions
    - Window open, close, minimize, maximize and restore; Start, flyout and menu open and close; toast in and out; Task View and desktop switch
    - Hover and press on a button; the page transition in Settings; the tab switch underline; the progress ring's cycle
  - Record the sound events by name and role with their trigger; no sound file is recorded
    - Default beep, critical stop, exclamation, notification, device connect and disconnect, and the rest of the event list of §13
  - Record the cursor roles with hot spot and nominal size; no cursor file is recorded
    - Arrow, text, busy, working in background, the eight resize directions, move, link, unavailable, precision, pen, the drag effects
  - Record the elevation levels found: which surfaces cast a shadow, its extent and offset, per surface kind
  - Done when: `check-inventory.py --surface system` passes; each motion listed has a `motion` record with a duration between 50 and 1,000 milliseconds, and every motion has its recording and samples listed in `shots.json`.
- [ ] Visual States, Themes and Scale Variants: writes `docs/design/inventory/system/states.json` and per-profile bounds files
  - Add `capture-ui.py --profile <id>`: re-captures a surface under another profile and writes only `id` and `bounds` to `<surface>/<file>.<profile>.json`, so layout rules can be derived from the five scale points of §19
  - Re-capture the desktop, taskbar, Start, quick settings, window chrome, Settings frame and File Explorer frame under every scale profile of §19 (125 to 200 percent), and derive for each `metric` whether it scales, stays fixed or rounds
  - Add `scripts/inventory/sample-colours.py`: samples an element's fill, border and text colour in a given state from a screenshot in the scratch directory
    - It writes them as colour roles with their measured values in light and in dark
  - Record the state matrix for each control type of the `controls` baseline, in light, dark and all four contrast themes
    - States: rest, hover, pressed, disabled, focused (keyboard), selected, selected and hover
  - Record the focus visual: the two-ring rectangle, its thickness, offset and corner rule, and which surfaces still draw the older dotted rectangle (a ledger entry of §6)
  - Record the materials: which surfaces use the wallpaper-tinted backdrop, which use the blurred translucent one, and which are opaque
    - Also what each falls back to when transparency is off, on battery saver and in a contrast theme
  - Record the accent colour's uses: which elements take the accent, and the light and dark variants derived from it
  - Done when: `check-inventory.py --surface system` passes; every control type of the `controls` baseline has all seven states in light, dark and the four contrast themes, and each metric of the seven re-captured surfaces has a scaling class.
- [ ] Commit: `"inventory: shortcut, gesture, metric, motion and state catalogues captured"` (one commit per surface file, the last one closes the section)

**Test checkpoint:** every surface item's own "Done when" line holds, `python3 scripts/inventory/check-inventory.py` exits 0 over the whole tree, and `inventory-report.py` lists every file this section names with a record count above zero. Platform: the reference VM for capture; the checker runs on Linux.

---

## 15. Inventory-to-Roadmap Authoring

> **Spawned-by:** root

**Design:** n/a -- writes roadmap sections, it draws nothing

The step that turns records into precise work: every record ends up owned by exactly one roadmap section, written so an implementer with no other context can build it.

> [!WARNING]
> **Never create a second home.** Most surfaces already have a roadmap file (taskbar: `TODO-10`, Start and tray: `TODO-11`, window manager: `TODO-08`, file manager: `D09 T09`, control panel: `D09 T11`). A record is assigned to the existing section that covers it; a new section is filed only for records no section covers, with the next free number, body last, and an Implementation Order row.

- [ ] Write `scripts/inventory/author-sections.py --surface <name>`: lists the surface's records grouped by parent, the existing sections whose text names them, and the records no section names
- [ ] For each surface in the order of `authoring.json`, add an `**Inventory:** <id prefix>` line to each existing section that owns records
  - Correct the section's wording where it disagrees with the captured record: the reference wins over older wording, and a deliberate departure cites its ledger entry
- [ ] For uncovered records, draft one new section per coherent group of at most 10 checklist items in the owning file
  - Each item names one record id, the control from `TODO-19` it is built with, its position rule and its `behaviour`
- [ ] Generate each new section's item skeleton from the records (`author-sections.py --draft`), then rewrite it by hand: the script proposes, the author decides
- [ ] File the missing owners found during capture, each through `/create-todo` with a reciprocal XREF here
  - The Settings pages beyond the frame (`D09 T11 §2` owns the navigation frame and applet rows, not the System, Personalization and other pages), a consoles frame, the secure screens, first-run setup pages, and any inbox app with no file
- [ ] Resolve any double ownership the capture finds (the 2026-11-14 audit moved the apps that had two homes into `11-apps` and left pointers behind)
  - Name one owner per app and turn the other into an XREF, never by deleting shipped work
- [ ] Run `python3 scripts/todo-plan.py --sync` and rebuild the todo-graph cache after each surface, and commit one surface per commit
- [ ] Finish when `inventory-report.py --owners --check` exits 0
- [ ] Commit: `"todo: every inventory record owned by one section"`

**Test checkpoint:** `python3 scripts/inventory/inventory-report.py --owners --check` exits 0 with `owners: N records, 0 without owner, 0 with two owners`. `bash scripts/lint.sh` passes, including Checks 22, 23, 30 and 37.

---

## 16. Built-OS Layout Check

> **Spawned-by:** root

**Design:** n/a -- a comparison tool, it draws nothing

The inventory measures the reference; this measures Impossible OS against it, so "1:1" is a number and a regression is a failing test.

- [ ] Write `scripts/inventory/layout-check.py`: boots the image under QEMU at 1920 by 1080 and reads the automation tree through the transport of `TODO-07 §6`
  - Each element maps to an inventory record by the `inventoryId` its automation node carries (`TODO-07 §1`)
- [ ] Compare per record: present, order under its parent, control type, label, and bounds with no tolerance: a `metric` record and an element's bounds match the reference to the pixel at each capture profile
- [ ] Skip a record whose `disposition` is `excluded`, and compare an `equivalent` on presence and position only
- [ ] Apply the consistency ledger: a record with an `inconsistency` id is compared against the ledger's `rule` value, not the captured variant
- [ ] Write the result to `build/inventory/layout-report.json` and print `layout: N matched, M differ, K missing` with the first 20 differences
- [ ] Add the check to the smoke matrix as an advisory leg, blocking only for the surfaces listed in `docs/design/inventory/enforced.json`
  - Create `enforced.json` empty here; a surface is appended once every record it holds is owned and built
- [ ] Write a fixture test: a recorded automation tree with one moved control must report exactly one difference
- [ ] Commit: `"inventory: built-OS layout check against the reference"`

**Test checkpoint:** on the fixture, `layout-check.py --tree fixture.json` prints `layout: 11 matched, 1 differ, 0 missing` and exits 1. On the built image the command completes and writes `build/inventory/layout-report.json`. Test on: QEMU TCG and KVM.

---

## 17. Design Spec Reconciliation

> **Spawned-by:** root

**Design:** [`shell.md#taskbar`](../../docs/design/shell.md#taskbar), [`shell.md#start-menu`](../../docs/design/shell.md#start-menu), [`shell.md#window-chrome`](../../docs/design/shell.md#window-chrome), [`controls.md#which-rules-apply-to-every-control`](../../docs/design/controls.md#which-rules-apply-to-every-control)

The spec was written before the reference was measured. This section makes it agree with the inventory, and makes the ledger's rules the spec's rules.

- [ ] Write `scripts/inventory/spec-diff.py`: lists every size, radius, spacing, type and motion token in `docs/design/tokens.json` that differs from its `metric` or `motion` record
  - The token to record map lives in `docs/design/inventory/token-map.json`
- [ ] Correct `tokens.json` where it differs from the reference and no ledger rule says otherwise; regenerate `include/desktop/theme_tokens.h` with `scripts/site/gen_theme_header.py`
- [ ] Rewrite each section of `docs/design/shell.md` whose measurements changed, citing the inventory record ids beside each number
- [ ] Add to `docs/design/controls.md` every control of the `controls` baseline it lacks, with its measurements and state matrix from §14
- [ ] Add a "Consistency rules" section to `docs/design/index.md` generated from `consistency.json`, so each rule is published once
- [ ] Record the one existing deliberate departure (the frostier glass) as a ledger entry or remove it
  - Owner direction of 2026-11-14: the shell is 1:1 except where it is made more consistent
- [ ] Make lint Check 30 run `spec-diff.py --check` so a token cannot drift from the reference again
- [ ] Commit: `"design: spec measured from the Windows 11 inventory"`

**Test checkpoint:** `python3 scripts/inventory/spec-diff.py --check` prints `spec: 0 tokens differ from the reference` and exits 0. `python3 scripts/site/build.py --check` passes and `include/desktop/theme_tokens.h` is regenerated with no hand edit.

---


## 18. Pixel Comparison Against Reference Screenshots

> **Spawned-by:** root

**Design:** n/a -- a comparison tool, it draws nothing

The layout check proves every element is in the right place. This proves the pixels: the built surface is compared with a screenshot of the reference, and only the replaced copyrighted regions may differ.

- [ ] Read the reference screenshots §3 saved during the campaign, through `shots.json`; never start the VM
  - A surface or state with no shot is a capture gap filed in `REFERENCE.md`, not a reason to capture during a comparison
  - The screenshots are Microsoft's pixels: they stay outside the repository; `check-no-ms-assets.py` of §4 refuses them
- [ ] Write `scripts/inventory/pixel-check.py --surface <name>`: renders the same surface and state in Impossible OS under the headless compositor at the same profile and compares it with the reference screenshot
- [ ] Build the mask from the inventory: regions whose record is an icon, an image, a wallpaper area or a text run are masked; every other pixel is compared
- [ ] Unmasked pixels must be equal: no geometry tolerance, exact colour for solid fills, and a difference of at most 2 per channel inside blurred backdrop and shadow areas
- [ ] Masked text regions are compared by their ink box: same origin, same advance width and same line count as the reference, using the text metrics recorded in §14
- [ ] Masked icon and image regions are compared by rectangle only: same position and size
- [ ] Output `build/inventory/pixel-report.json` and `pixel: N surfaces, M differ` with, per differing surface, the count of differing pixels and their bounding boxes; write a difference image to the scratch directory, never to the repository
- [ ] Wire it beside the layout check: advisory until a surface is fully built, then blocking for the surfaces listed in `docs/design/inventory/enforced.json`
- [ ] Commit: `"inventory: pixel comparison against reference screenshots"`

**Test checkpoint:** with a fixture reference and a fixture render that differ by one pixel outside any mask, `pixel-check.py --fixture` prints `pixel: 1 surfaces, 1 differ` and exits 1; the same pair differing only inside a text mask with equal ink boxes exits 0. Platform: reference screenshots from the campaign store; the comparison runs on Linux against QEMU TCG and never starts the VM.

---

## 19. Capture Matrix, Fixtures and Device Stand-ins

> **Spawned-by:** §2 (split)

**Design:** n/a -- prepares the reference machine and its expected captures, it draws nothing

**Needs:** bare-metal

Capture once only works if the campaign knows, before it starts, every surface, state and profile it must take, and the VM can actually show each one. This section writes that list and builds the fixtures that make each entry reachable.

- [ ] Write `docs/design/inventory/expected.json`, the capture matrix, before any surface is captured
  - One row per surface, state and profile, with its prerequisite fixture and the evidence it needs (records, shot, recording); §7's `--campaign --check` reconciles against it and fails on any row without evidence
- [ ] Keep the setup flow replayable: take the checkpoint `pre-oobe` at the first setup page before the answer file runs, and run §9's First-Run Setup capture from it, for both the local-account and the network branches
- [ ] Extend the profile matrix in `REFERENCE.md` and `ref-vm.ps1 --verify`: 100, 125, 150, 175 and 200 percent scaling, light and dark, all four contrast themes, and text size 150 percent
  - Add `fhd-100-rtl` (Arabic display language, installed from the language pack while setup still has network) for every frame surface, and a 2560 by 1080 wide and a 1080 by 1920 tall viewport for snap layouts
- [ ] Define the multi-display topology: two displays at different scales, then the primary swapped, captured through the session that provides them, with the session type recorded on each record
  - Evidence for all three taskbar button modes on the second display is required before §8 closes
- [ ] Add synthetic list fixtures: a script that fills the VM with made-up files, apps, devices, networks and accounts, so a list's row template, controls, bounds and states are captured while the payload stays fictional
  - Capture each such list empty, populated and overflowing; `ENVIRONMENT_LISTS` of §3 then hides only real data, never the row layout
- [ ] Add service-state fixtures with controlled connectivity: a local update source and a network switch that is toggled per checkpoint
  - Update checking, downloading, installing, failed and pending restart; network authentication and discovery failure; loading, timeout, validation error and disconnected states of Settings pages
- [ ] Add notification fixtures: a script that posts deterministic toasts in several groups, with long text and overflow, captured expanded, collapsed and at text size 150 percent
- [ ] Prove a device stand-in for each hardware-gated surface (battery and its states, brightness, lid, touchpad, pen, touch, rotation and tablet posture)
  - Hyper-V's virtual battery on a prerelease configuration version, and a clone of the reference disk under QEMU with `virtio-multitouch` and a tablet device; each proven stand-in is a fixture row in `expected.json`
- [ ] Capture what no stand-in can show on a physical Windows 11 laptop installed from the same image at the same profiles, recorded as `hand:<build>` with screenshots in the same store
- [ ] Commit: `"inventory: capture matrix, fixtures and device stand-ins"`

**Test checkpoint:** `python3 scripts/inventory/inventory-report.py --campaign --check` against a fixture tree fails naming the row when one whole surface is deleted, and again when one state of one profile is deleted; `ref-vm.ps1 --verify` reports every profile of the matrix; every hardware-gated row of `expected.json` names a proven stand-in or the physical capture. Platform: the operator's Windows host with Hyper-V, and the physical laptop for the rows no stand-in covers.

---

## OS Comparison

| ⭐  | Feature                                 | 🪟 Win11                   | 🐧 Linux             | 🚀 Impossible OS     |
| --- | --------------------------------------- | -------------------------- | -------------------- | -------------------- |
| 💎  | Machine-readable UI tree                | ✅ UI Automation           | ✅ AT-SPI            | ⬜ Planned in T07 §1 |
| 💎  | Published design measurements           | ⚠️ Partial design docs     | ⚠️ GNOME HIG, prose  | ⬜ Planned in §17    |
| ⭐  | Every element inventoried as data       | ❌ None                    | ❌ None              | ⬜ Planned in §8-§14 |
| ⭐  | Every element owned by one work item    | ❌ None                    | ❌ None              | ⬜ Planned in §15    |
| ⭐  | One rule where the original has several | ❌ Two menus, two settings | ⚠️ Varies by toolkit | ⬜ Planned in §6     |
| ⭐  | Built shell checked against a reference | ❌ None                    | ❌ None              | ⬜ Planned in §16    |

> **After §1-§7:** the method exists and can measure anything. **After §8-§14:** the whole Windows 11 interface is data. **After §15-§17:** every element has an owner, the spec is measured, and the built OS is checked against the reference on every boot matrix run.

---

## Unit Tests

> Host-side tests, not kernel tests: `scripts/inventory/tests/` (stdlib `unittest`), wired into `scripts/test-tooling.sh` as a nested suite so CI runs them. No kernel test category is needed.

- [ ] Create `scripts/inventory/tests/test_check_inventory.py` with:
  - A valid three-record file passes and prints `3 records, 0 errors`
  - A duplicate `id`, an orphan `parent`, a `parent` cycle, a repeated `order`, a wrong `itemCount`, a `bounds` without `captureProfile` and an unknown `source` each exit 1 naming the rule
  - A label of 81 characters, a link, a missing `disposition` and an undefined `inconsistency` id each exit 1
- [ ] Create `scripts/inventory/tests/test_capture.py` with:
  - The recorded tree fixture yields the expected record count and the expected `id` for a nested menu item
  - `--merge` keeps a hand-written `behaviour` and drops a record the capture no longer sees
  - A list named in `ENVIRONMENT_LISTS` yields one record and no children
- [ ] Create `scripts/inventory/tests/test_report.py` and `test_layout_check.py` with the fixtures named in §7 and §16
- [ ] Register the suite in `scripts/test-tooling.sh` and add its paths to the scoped-pack triggers
- [ ] Commit: `"test: inventory tooling suite"`

---

## Verification

- [ ] `python3 scripts/inventory/check-inventory.py` exits 0 over the whole inventory and prints the total record count
- [ ] `python3 scripts/inventory/check-inventory.py --baselines` shows zero unmatched rows for all seven baselines
- [ ] `python3 scripts/inventory/inventory-report.py --owners --check` exits 0
- [ ] `docs/design/inventory/COVERAGE.md` regenerates byte-identically, and `python3 scripts/site/build.py --check` passes
- [ ] `git grep -n -i -E 'microsoft\.com|learn\.microsoft' -- docs/design/inventory` prints nothing
- [ ] `bash scripts/test-tooling.sh --quiet` passes with the inventory suite listed
- [ ] `python3 scripts/inventory/layout-check.py` runs on QEMU TCG and KVM and writes its report; WHPX, VirtualBox and bare metal are not needed, the tool reads the automation tree and no hardware

**Test runner:** N/A (host tooling, no kernel or user-mode suite) | validation: `scripts/inventory/tests/` as a nested suite of `scripts/test-tooling.sh`
