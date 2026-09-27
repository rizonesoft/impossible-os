<!-- docs: covers=todo/01-boot-platform/TODO-14-boot-diagnostics.md -->
# `boot-timeline.json` Wire Format (schema v1)

> Canonical wire format for the per-boot stage timeline. Single source of
> truth for the kernel writer (`boot_timeline_dump_json` in
> `src/kernel/main/boot_progress.c`) and any host-side viewer or converter
> (a `systemd-analyze plot`-style SVG, a `chrome://tracing` import, an ETW
> pipeline, or a boot-blame / regression tool). The machine-readable
> emitter already ships; this document is the contract a viewer codes
> against.

## File location

- **BlackBox present:** `X:\Perf\boot-timeline.json` (next to `boot-profile.log` under the perf dump tree).
- **Fallback (no BlackBox partition):** `<klog_dir>\boot-timeline.json` (typically `C:\Impossible\System\Logs\`).
- **When emission proceeds**, written with a single create + truncate (`O_WRITE | O_CREATE | O_TRUNC`), so a successful write never leaves stale tail bytes from a longer prior timeline.
- **Freshness caveat:** the create + truncate happens only *after* the emit preconditions below pass and serialization begins. A boot that hits a no-write precondition leaves the **previous** boot's `boot-timeline.json` on disk unchanged -- the file is not guaranteed to reflect the current boot. Consumers MUST correlate it with a current-boot marker (the BlackBox boot sequence or `boot-profile.log` mtime) before treating it as live.
- **Write-failure recovery (best-effort):** if the truncating open succeeds but the `vfs_write` is short or fails, the `O_TRUNC` has already destroyed the prior file, so the writer **attempts** to re-truncate to **zero length** ("no data this boot"). This recovery is best-effort: if the recovery open itself fails, a **malformed partial JSON prefix** can remain on disk. Consumers MUST therefore validate that the top-level array parses, and treat **both** an empty file and an unparseable file as no-data for this boot -- distinct from a stale prior-boot file.

## Emit preconditions

The file is **not written** at all (the function returns early, leaving any prior file in place) when:

- Fewer than 2 timing steps were recorded (`count < 2`) -- a timeline of one point is meaningless.
- The TSC frequency is uncalibrated (`boot_timing_tsc_freq() < 1000`), so ms deltas cannot be computed.
- The serialization buffer could not be allocated.

The serialization buffer is 16 KiB (4 pages). If records would overflow it, the array is closed cleanly after the last record that fit (no partial record, no dangling trailing comma) rather than truncating mid-object.

## Versioning

- The file is a **bare JSON array** with **no embedded version field**; the schema is versioned **doc-side** as v1.
- Bump this document's version when a field is removed, a field's type changes, a `source` enum value is renamed, or the meaning of an existing field changes. Adding a new optional field is **not** a bump.
- Consumers MUST tolerate unknown trailing fields (forward-compat) and MUST treat `unreliable: true` records as advisory-only.

## Record shape

The top level is a JSON array. Each element is one timeline record with exactly these fields:

| Field         | Type   | Description                                                                                                          |
| ------------- | ------ | ------------------------------------------------------------------------------------------------------------------- |
| `stage`       | string | Stage name. An FPDT phase label, or the kernel boot-step name copied until the first of: 32 chars, NUL, `"`, or `\`. An embedded quote or backslash **truncates** the name at that byte (it is not escaped or skipped-over), so a label containing either renders only its prefix. |
| `phase`       | int    | Boot phase number (0-3) for TSC records; always `0` for FPDT-sourced records.                                        |
| `post`        | string | POST code as a lowercase hex string. `"0x0000"` for FPDT records. For TSC records it is the `uint16` step POST code formatted with a **minimum** width of 2 (`"0x%02x"`), so 16-bit codes such as `"0x1001"` render 4 digits -- do not assume a fixed 2-digit width. |
| `start_ms`    | int    | Milliseconds from the timeline anchor to the start of this stage. See **Anchoring**.                                 |
| `duration_ms` | int    | Milliseconds this stage took. **Source-local:** for a TSC record it is the delta to the next TSC step (`0` for the last TSC step); for an FPDT record it is the firmware-published phase duration (the last FPDT phase is typically `0`). A timeline that carries FPDT records therefore has an intermediate zero-duration record, not only a trailing one. |
| `target_ms`   | int    | Per-stage budget from `boot_perf_budget_lookup()`; `0` when no budget is defined for the stage.                      |
| `source`      | string | `"fpdt"` (firmware-published ACPI FPDT phase) or `"tsc"` (kernel TSC-sampled boot step).                             |
| `unreliable`  | bool   | `true` when this record's timing cannot be trusted as absolute. See **Reliability**.                                 |

## Record ordering

Up to 5 FPDT phase records are prepended (firmware reset through bootloader handoff), followed by the kernel TSC step records in recording order. The earliest FPDT entry anchors the timeline to firmware reset when FPDT is reliable.

## Anchoring (`start_ms` semantics)

- When the FPDT firmware anchor **and** the bootloader `bl_entry` sample are both valid, TSC `start_ms` values are **absolute milliseconds since firmware reset**.
- When FPDT is unavailable or garbage (for example, some VirtualBox firmware publishes an empty FPDT record), `start_ms` falls back to **milliseconds since bootloader entry**, and every TSC record is stamped `unreliable: true` so a consumer never mistakes a relative fallback timeline for an absolute one.

## Reliability (`unreliable`)

A record is stamped `unreliable: true` when any of these hold: the FPDT firmware anchor was missing, the `bl_entry` sample was `0`, a TSC sample ran backwards (reverse-ordered relative to the previous step), or the anchor / delta arithmetic saturated (`UINT32_MAX`). For such a record, `start_ms` / `duration_ms` are relative or suspect, not absolute. **Consumers MUST preserve this flag through any conversion** so the boot-blame and regression surfaces are not silently misled by fallback-anchored timings.

## Consumers

- **Kernel writer:** `boot_timeline_dump_json()` (`src/kernel/main/boot_progress.c`), called from `boot_desktop.c` late in boot.
- **BlackBox artifact catalog:** `docs/boot/black-box-artifacts.md`.
- **`target_ms` / `unreliable` preservation** is required by the boot-blame + regression surfaces; converters MUST round-trip both -> XREF: `01-boot-platform/TODO-29` (boot perf health & observability).
- **Offline viewers:** [`tools/boot-timeline/boot_timeline.py`](../../tools/boot-timeline/boot_timeline.py) renders a captured artifact as a `systemd-analyze plot`-style Gantt SVG (`svg`) or a `chrome://tracing` event trace (`trace`). Host-side by design, so the kernel image carries no renderer. Shipped by `01-boot-platform/TODO-14` §16. A converter MUST preserve `target_ms` and `unreliable` per the rule above; this one carries both into the SVG tooltip and the trace `args`, and refuses an artifact whose `unreliable` is not a real JSON boolean rather than coercing it.
