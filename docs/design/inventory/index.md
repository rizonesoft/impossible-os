<!-- docs: sources=docs/design/inventory/schema.json,scripts/inventory/check-inventory.py reviewed=2026-11-14 -->
# Windows 11 Inventory

The inventory is the Windows 11 interface written down as data: every surface, page, control, menu entry, shortcut, gesture, measurement and animation of the reference build, one JSON object per line. It is what makes "the same thing, in the same place" a number a check can test. The plan that fills it is the [Windows 11 parity inventory roadmap](../../graphics/win11-parity-inventory.md).

## What is in this folder?

- `schema.json`: the record format, in JSON Schema 2020-12.
- `<surface>/<file>.json`: the records of one surface, such as `taskbar/bar.json`. Each line is one record with an `id` (`win11.<surface>.<path>`), a `kind`, a `label`, its `order` under its `parent`, where the fact came from (`source`) and the build it was read from (`capturedFrom`).
- `<surface>/<file>.<profile>.json`: the same elements measured under another capture profile, such as `taskbar/bar.fhd-150.json`. Each line names an existing record by `id` and gives its `bounds` (and, where measured, `measure` and `textMetrics`) at that profile, so a layout rule can be derived from several scale points without repeating the record.
- `<surface>/baseline.json`: an independent list of what should be there, read from the reference machine itself, against which the captured records are counted.

## What does a record measure?

A measurement is typed, never prose. A `metric` record carries `measure` (a value and a unit: pixels, milliseconds, a count, a percentage). A `motion` record carries `motion` (duration, the animated property, the distance and the cubic curve). A type ramp style carries `textMetrics` (size, ascent, descent, line gap and per-character advances), and a visual state carries `colours` (a colour role's value per theme) and names its `state` (rest, hover, pressed and so on). A measurement says how it follows display scale (`scaling`: multiplied, multiplied then rounded to a whole pixel, or constant), a shortcut says where it acts (`shortcutScope`) and whether an app may take it (`overridable`), and any record can name the `session` it was captured through. A number must be finite: `NaN`, `Infinity` and an overflowing exponent are refused.

## How is it checked?

```bash
python3 scripts/inventory/check-inventory.py              # the whole inventory
python3 scripts/inventory/check-inventory.py --surface taskbar
python3 scripts/inventory/check-inventory.py --json       # for a tool
```

The checker validates every line against the schema, then checks the tree as a whole. It refuses a duplicate `id`, a `parent` that names no record, a parent cycle, an `order` used twice under one parent, an `itemCount` that differs from the number of child records present, and a profile observation that names no record or a profile other than its file's. A conflict is reported at every line involved in it, so `--surface` finds a clash with another surface whichever side sorts first; observation files of other surfaces are not read at all. It prints the first 50 errors as `file:line: rule: message` and exits 1. A clean run prints `inventory: N records, 0 errors`.

## What does it never hold?

Microsoft's own material. Names, positions, types, ids, shortcuts and numbers are facts about the interface; icons, screenshots, sounds, fonts, wallpaper and help text are Microsoft's work and stay outside the repository.
