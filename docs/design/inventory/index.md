<!-- docs: sources=docs/design/inventory/schema.json,docs/design/inventory/names.json,scripts/inventory/check-inventory.py,scripts/inventory/check-no-ms-assets.py,scripts/inventory/asset-types.txt,scripts/inventory/make-ms-hashes.ps1,scripts/inventory/guest/hash-assets.ps1 reviewed=2026-11-14 -->
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
