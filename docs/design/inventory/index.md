<!-- docs: sources=docs/design/inventory/schema.json,docs/design/inventory/names.json,scripts/inventory/check-inventory.py,scripts/inventory/check-no-ms-assets.py,scripts/inventory/asset-types.txt,scripts/inventory/make-ms-hashes.ps1,scripts/inventory/guest/hash-assets.ps1,scripts/inventory/capture.py,scripts/inventory/capture-ui.py,scripts/inventory/capture_core.py,scripts/inventory/runtime.json,docs/design/inventory/profiles.json reviewed=2026-11-14 -->
# Windows 11 Inventory

The inventory is the Windows 11 interface written down as data: every surface, page, control, menu entry, shortcut, gesture, measurement and animation of the reference build, one JSON object per line. It is what makes "the same thing, in the same place" a number a check can test. The plan that fills it is the [Windows 11 parity inventory roadmap](../../graphics/win11-parity-inventory.md).

## What is in this folder?

- `schema.json`: the record format, in JSON Schema 2020-12.
- `<surface>/<file>.json`: the records of one surface, such as `taskbar/bar.json`. Each line is one record with an `id` (`win11.<surface>.<path>`), a `kind`, a `label`, its `order` under its `parent`, where the fact came from (`source`) and the build it was read from (`capturedFrom`).
- `<surface>/<file>.<profile>.json`: the same elements measured under another capture profile, such as `taskbar/bar.fhd-150.json`. Each line names an existing record by `id` and gives at least one of its `bounds`, `measure` or `textMetrics` at that profile, so a layout rule can be derived from several scale points without repeating the record.
- `<surface>/baseline.json`: an independent list of what should be there, read from the reference machine itself, against which the captured records are counted.
- `names.json`: the Microsoft trade mark names Impossible OS replaces, each with our name and the reason.

## What does a record measure?

A measurement is typed, never prose. A `metric` record carries `measure` (a value and a unit: pixels, milliseconds, a count, a percentage). A `motion` record carries `motion` (duration, the animated property, the distance and the cubic curve). A type ramp style carries `textMetrics` (size, ascent, descent, line gap and per-character advances), and a visual state carries `colours` (a colour role's value per theme) and names its `state` (rest, hover, pressed and so on). A measurement says how it follows display scale (`scaling`: multiplied, multiplied then rounded to a whole pixel, or constant), a shortcut says where it acts (`shortcutScope`) and whether an app may take it (`overridable`), and any record can name the `session` it was captured through. A number must be finite: `NaN`, `Infinity` and an overflowing exponent are refused.

## How is it checked?

```bash
python3 scripts/inventory/check-inventory.py              # the whole inventory
python3 scripts/inventory/check-inventory.py --surface taskbar
python3 scripts/inventory/check-inventory.py --json       # for a tool
python3 scripts/inventory/check-inventory.py --index      # the staged inventory, as the commit gate checks it
```

The checker validates every line against the schema, then checks the tree as a whole. It refuses a duplicate `id`, a `parent` that names no record, a parent cycle, an `order` used twice under one parent, an `itemCount` that differs from the number of child records present, and a profile observation that names no record or a profile other than its file's. A conflict is reported at every line involved in it, so `--surface` finds a clash with another surface whichever side sorts first; observation files of other surfaces are not read at all. It prints the first 50 errors as `file:line: rule: message` and exits 1. It also holds every record to the rule below: a `label` over 80 characters is refused as a sentence (`label is a sentence`), any `http://` or `https://` text anywhere in a record or observation is refused (`link in record`), and a `behaviour` that repeats its `label` is refused. `names.json` must list each reference name once, with our name and a reason. A clean run prints `inventory: N records, 0 errors`. `scripts/lint.sh` (Check 40) runs it on every lint, against the staged contents under the commit gate.

## How is a surface captured?

```bash
python3 scripts/inventory/capture.py --surface notepad-smoke --file window --dry-run   # capture and check, publish nothing
python3 scripts/inventory/capture.py --surface taskbar --file bar --merge               # refresh a published file
python3 scripts/inventory/capture.py --verify-shots                                    # re-hash the screenshot store
```

`capture.py` runs on the host. It copies the guest tool `capture-ui.py`, the shared `capture_core.py`, the surface module `surfaces/<name>.py` and `profiles.json` into the reference VM, and runs the tool there on a fresh restore of `clean` through `ref-run.ps1`. The VM's Python and `comtypes` come from `runtime.json`, which pins each download by SHA-256; `ref-vm.ps1` installs them when it takes `clean`.

- **The profile is checked first.** The tool reads the session's profile through the same probe `ref-vm.ps1 -Verify` uses and exits 2 on any value that differs, naming it: `profile fhd-100: scaling is 125, expected 100`. `profiles.json` is the one place a profile's values are written.
- **A surface module says how to reach it.** `open()` brings it up by its own command or key, `root()` finds its element and `close()` dismisses it the way a user would. Three optional lists cover what structure cannot tell: `ENVIRONMENT_LISTS` (lists whose rows are the machine's own files, apps or devices: recorded once, never entered), `VALUES` (elements whose name is a live value, such as a caret position: recorded with the name withheld, and never used to build an id) and `IDENTITY` (a name for an element whose siblings look the same).
- **The walk reads structure only:** name, automation id, class, control type, rectangle, shortcut and whether the element is enabled. It never reads a value, a text range, a toggle or a selection, and a name too long to be a label (a sentence of help text) is used for neither the label nor the id. Unnamed hosting panes are spliced out, so an id follows what a user sees (`win11.notepad-smoke.window.menubar.file.save-as`). Each menu, flyout, combo box and split button is opened one level, its entries recorded as `menuItem` records with `itemCount` on the opener, and closed again with nothing chosen.
- **One walk is not evidence.** The first open of a menu after launch can differ from every later one, so the tool walks until two passes agree, at most four times; a surface that never repeats itself, or a flyout that never settles, makes the capture incomplete (exit 3), and an incomplete capture publishes nothing and removes nothing.
- **Hand-written fields survive a re-capture.** `--merge` carries `behaviour`, `disposition` and `inconsistency` across by `id`, refreshes every captured fact and drops a record the capture no longer sees, naming it. Look-alike siblings are told apart by position and every one carries its number behind a double hyphen, the first included (`insert-table--1`). No name can make `--` (a name folds to single hyphens, and `IDENTITY` segments must be slugs), so a positional id is recognisable in any capture: a note on one is never carried, and is dropped by name when its group changes. A note that would land on a positional id, or anywhere beneath one, stops the merge until the surface module's `IDENTITY` names the element.
- **A capture is taken only whole.** The runner must exit 0, `meta.json` must say the capture is complete, and `records.json` must hold exactly the records it counted; a short or cut file from a broken transfer is refused like an incomplete capture (exit 3).
- **Nothing unchecked is published.** The new file is written into a scratch copy of the inventory and `check-inventory.py` runs over it; only a clean result is published. Publishing stages the screenshots, `shots.json` and the records beside their destinations and then swaps each in, records last, keeping every replaced file aside until the last swap succeeds, so a failure at any point leaves the previous publication exactly as it was.
- **Screenshots stay on the operator's machine.** Each state is saved as `R:\11\ref-shots\<surface>\<state>.<profile>.png`, the state named after the opener's id with its dots kept (`window.menubar.file-open`), so no two states share a file; `shots.json` records its surface, state, profile, size and SHA-256, and `--verify-shots` names every missing or changed file. Elements with no usable name are listed in `build/inventory/<surface>-unnamed.json` for a hand-written label. `--fixture` also keeps the walked tree for a test fixture; it costs one more walk, so an ordinary capture (Notepad: about 45 s) does not take it.

## What does it never hold?

Microsoft's own material. The inventory records facts about the interface; the words and pictures Microsoft made stay outside the repository.

| Recorded | Never recorded |
| --- | --- |
| labels: the name a control shows | help text, and a tooltip longer than a label |
| positions, sizes and order | the description under a setting |
| control types, automation ids and class names | icons, images, screenshots and wallpaper |
| shortcuts and gestures | sounds and fonts |
| measured numbers: sizes, durations, curves, colours | links to Microsoft pages |

- **A `behaviour` is ours.** It is written in our own words from using the control, never copied from Windows, and the capture tool never fills it: it is one of the hand-written fields a re-capture carries across.
- **Screenshots stay off the repository.** The capture keeps them on the operator's machine (`R:\11\ref-shots`) and commits only their names, sizes and hashes.
- **No Microsoft file is committed.** `scripts/inventory/check-no-ms-assets.py` refuses an image, sound, font or cursor file whose SHA-256 and size match a file of the reference machine. The list it checks, `scripts/inventory/ms-hashes.txt`, holds only hashes and sizes: `make-ms-hashes.ps1` makes it once per reference build from `C:\Windows`, the Program Files folders, `C:\ProgramData` and every inbox app package, so the check runs in CI with no VM. The file types are listed once, in `asset-types.txt`, which the hashing script reads too. `scripts/lint.sh` Check 40 runs it over every tracked file, and over the staged contents under the commit gate.
- **Trade mark names are replaced.** A record keeps the reference name, because the label is a fact; Impossible OS ships the name `names.json` gives for it (`Microsoft Store` is `Store`, `OneDrive` is `Cloud Files`). A new trade mark name found during capture gets an entry before any surface of ours uses it.

The check catches a copied file, a copied link and a copied sentence long enough to be prose. It cannot tell whether a short `behaviour` was copied, so that part rests on the rule.
