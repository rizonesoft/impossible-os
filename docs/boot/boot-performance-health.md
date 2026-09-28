<!-- docs: covers=todo/01-boot-platform/TODO-29-boot-perf-health-observability.md sources=src/kernel/boot_perf_budget.c,include/kernel/boot_perf_budget.h,src/kernel/main/boot_health.c,src/kernel/main/boot_trend.c,include/kernel/boot_trend.h,src/kernel/main/boot_desktop.c,src/kernel/smbios.c,src/kernel/drivers/mouse.c,src/kernel/uefi_config.c reviewed=2026-09-28 order=29 -->
# Boot Performance and Health Observability

## What is it?

This layer turns the per-step boot timing the kernel already records into signals an operator can act on. Each named boot step has a target duration: a step 1.5 times over target logs a warning, one 4 times over logs an error. At the end of boot the kernel writes two files: `X:\Diag\boot-health.json`, a snapshot of everything degraded, missing, slow or quirky on this boot, and `X:\Perf\boot-trend.json`, a rolling record of the last 16 boots that warns when a step's median time grows more than 15%.

The budgets, the health snapshot and the trend file ship and run on every boot, along with three targeted fixes: faster SMBIOS parsing, faster PS/2 mouse setup, and detailed reporting of firmware memory that is both writable and executable. The rest of the roadmap is open: loading fonts and icons after the desktop appears, a heartbeat during long steps, timing inside the slowest steps, critical-path attribution, and per-driver storage health.

## How does it work?

`boot_perf_budget.c` defines 18 per-step budgets, each a step name, a target in milliseconds and a reason, plus a whole-boot target of 4000 ms. `boot_perf_budget_classify()` compares a measured duration with 1.5 times and 4 times the target. `boot_perf_budget_check()` checks every recorded step and `boot_perf_total_check()` the whole boot.

Late in `boot_desktop.c`, `boot_health_publish_json()` writes the health snapshot: schema version, time, Secure Boot state (with "unreadable" kept distinct from "disabled"), degraded capabilities and subsystems, missing capabilities, budget breaches, writable-and-executable memory findings (up to 16, with `mat_overflowed` set when there were more; the 8-line limit applies only to the serial log), active firmware quirks and recent boot times. A short write truncates the file to zero bytes rather than leave half a JSON document.

`boot_trend_publish_json()` then reads the existing trend file, adds this boot's step times, keeps the newest 16 entries, and publishes it through a temporary file renamed over the old one. On BlackBox's FAT32 volume that rename is not atomic: the old file is deleted first, so a failure partway through can lose the previous history ([FAT32 hardening follow-ups](../../todo/05-storage-filesystems/TODO-04-fat32-hardening-vfs-semantics.md#18-post-ship-follow-up-backfill-orphan-cohort-2026-07-31)). It compares the median of the newest three boots with the three before them for each step, and warns when growth exceeds 15%; the first boots stay silent until there is enough history. Without BlackBox mounted it does nothing.

Both writers run before the command shell starts, so their work delays the handoff to user mode even though the reported boot total does not include it.

The three targeted fixes are local. `smbios_copy_firmware_table()` copies the SMBIOS table into RAM once, so parsing no longer reads firmware memory byte by byte, and logs the slowest structure types. `mouse_init()` uses a short polling limit (`PS2_WAIT_SHORT`, 10,000 iterations) for register polls and a long one (`PS2_WAIT_LONG`, 2,000,000 iterations) only for the slow self-test and ID reads, instead of one uniform limit. These are iteration counts, not clock deadlines, so their real duration depends on how fast the platform services port I/O. It also checks the acknowledgement after every setup command (`mouse_expect_ack()`). `mat_init()` in `uefi_config.c` logs each writable-and-executable firmware region with its address, size and attributes, capped at 8 lines, instead of only a count.

## What are its interfaces?

| Interface | Purpose |
| --------- | ------- |
| `boot_perf_budget_classify()`, `boot_perf_budget_check()`, `boot_perf_total_check()` | Per-step and whole-boot budget checks ([`boot_perf_budget.c`](../../src/kernel/boot_perf_budget.c), [`boot_perf_budget.h`](../../include/kernel/boot_perf_budget.h)) |
| `boot_health_publish_json()` | Writes `X:\Diag\boot-health.json` ([`boot_health.c`](../../src/kernel/main/boot_health.c)) |
| `boot_trend_publish_json()` | Updates `X:\Perf\boot-trend.json` through a temporary file and rename ([`boot_trend.c`](../../src/kernel/main/boot_trend.c), [`boot_trend.h`](../../include/kernel/boot_trend.h)) |
| `smbios_copy_firmware_table()` | Copies the SMBIOS table to RAM before parsing ([`smbios.c`](../../src/kernel/smbios.c)) |
| `mouse_init()`, `mouse_expect_ack()` | PS/2 mouse setup with split timeouts and checked acknowledgements ([`mouse.c`](../../src/kernel/drivers/mouse.c)) |
| `mat_init()` | Per-region reporting of writable-and-executable firmware memory ([`uefi_config.c`](../../src/kernel/uefi_config.c)) |

## How do I use it?

The layer runs on every boot. Its serial lines:

- `BOOT-BUDGET: <step> -> <next> took <n>ms (target <n>ms): <reason>` when a step is over budget, and `BOOT-BUDGET: TOTAL boot took <n>ms (target 4000ms): ...` when the whole boot is.
- `BOOT-TREND: <step> 3-run median grew <n>% (<n>ms -> <n>ms)` when a step is getting slower across boots.
- `SMBIOS: profile: total <n> ms across all parsed structures`, followed by `SMBIOS: profile: top<n> ...` lines for the three slowest structure types.
- `PS/2 mouse: init <n>ms, timeouts=<n>, exit=<reason>`.
- `MAT[<n>] WX VIOLATION at phys=0x... pages=<n> attr=0x... (RO=<0|1> XP=<0|1> RP=<0|1>)` for each writable-and-executable firmware region.

After boot, `type X:\Diag\boot-health.json` and `type X:\Perf\boot-trend.json` show the two files. Their fields are documented in the [boot-health.json](boot-health-schema.md) and [boot-trend.json](boot-trend-schema.md) wire-format pages. The unit tests run in the `boot` suite (`bash scripts/test.sh SUITE=boot`).

## What is not implemented yet?

- Fonts and icons still load before the desktop is ready rather than afterwards: [Async Font and Icon Loader (Post-Desktop-Ready)](../../todo/01-boot-platform/TODO-29-boot-perf-health-observability.md#7-async-font-and-icon-loader-post-desktop-ready).
- There is no heartbeat during long steps; it needs the boot watchdog's timer and a `boot_info` change: [Boot Heartbeat Telemetry During Long Phases](../../todo/01-boot-platform/TODO-29-boot-perf-health-observability.md#8-boot-heartbeat-telemetry-during-long-phases).
- The EXEC step has no internal timing, and its regression against the 100 ms target is not bisected: [EXEC Step Latency Profile](../../todo/01-boot-platform/TODO-29-boot-perf-health-observability.md#9-exec-step-latency-profile).
- There is no cause classification or critical-path view of the boot timeline: [Boot Critical-Path / Dependency / Resource-Wait Attribution](../../todo/01-boot-platform/TODO-29-boot-perf-health-observability.md#18-boot-critical-path--dependency--resource-wait-attribution).
- Storage health is one aggregate flag, not a per-driver record: [Per-Driver Degraded-State Registry for Storage](../../todo/01-boot-platform/TODO-29-boot-perf-health-observability.md#21-per-driver-degraded-state-registry-for-storage).
- A hypervisor that silently turns the framebuffer's write-combining mapping into write-through is not reported as a quirk: [PAT WC -> WT Hypervisor Trap Quirk](../../todo/01-boot-platform/TODO-29-boot-perf-health-observability.md#17-pat-wc---wt-hypervisor-trap-quirk).
- The two JSON writers run before the shell starts, outside the reported boot total: [Boot Trend JSON Writer Hardening](../../todo/01-boot-platform/TODO-29-boot-perf-health-observability.md#20-boot-trend-json-writer-hardening).
- The whole-boot budget line says "DESKTOP_READY" but measures up to the last recorded step: [Post-Ship Follow-Up Backfill](../../todo/01-boot-platform/TODO-29-boot-perf-health-observability.md#19-post-ship-follow-up-backfill-orphan-cohort-2026-07-31).
- The trend median and growth calculation, the Secure Boot classifier and the memory-attribute classifier have unit tests; the budget checker, the health JSON serializer and the trend history window do not: [Unit Tests](../../todo/01-boot-platform/TODO-29-boot-perf-health-observability.md#unit-tests).

## How does it compare with Windows 11 and Linux?

Windows 11 records boot timing in ETW traces that are analysed afterwards, and Linux offers `systemd-analyze` for the same after-the-fact view, including `critical-chain` for dependencies. Neither raises a budget alarm during boot, writes a single health file, or warns when boot is trending slower, which Impossible OS does. It is behind Windows on loading shell assets after the desktop appears, and behind Linux on critical-path attribution.

## See also

- [Boot Performance and Health Observability roadmap](../../todo/01-boot-platform/TODO-29-boot-perf-health-observability.md)
- [Boot Health Wire Format](boot-health-schema.md)
- [Boot Trend Wire Format](boot-trend-schema.md)
- [Boot Timeline Wire Format](boot-timeline-schema.md)
- [Boot Health Gate](boot-health.md)
- [Boot Watchdog](boot-watchdog.md)
