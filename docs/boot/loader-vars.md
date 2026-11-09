<!-- docs: covers=todo/01-boot-platform/TODO-07-boot-entry-store-menu-policy.md sources=src/boot/uefi/bootx64.c,src/boot/uefi/loader_vars.c,include/boot/loader_vars.h,src/kernel/uefi_runtime.c reviewed=2026-11-09 -->
# OS-Visible Loader UEFI Variables

> **Owner:** [OS-Visible Loader UEFI Variables](../../todo/01-boot-platform/TODO-07-boot-entry-store-menu-policy.md#15-os-visible-loader-uefi-variables).
> Mirrors the upstream Linux systemd-boot Boot Loader Interface so user-mode
> tools (`bootctl`, `systemctl reboot --boot-loader-entry`, custom UEFI
> introspection) Just Work against an Impossible OS-booted system.

## Why this exists

Without an OS-visible loader interface, dynamic entries (A/B-generated,
recovery-generated, network-discovered, chainload) are invisible to
userspace until the on-disk `bootentries.json` is regenerated. Linux
ports of `bootctl` need a stable UEFI-variable surface; Impossible OS
publishes that surface using systemd-boot's canonical vendor GUID so
those tools work unmodified.

## Vendor GUID

systemd-boot's canonical vendor GUID:

```
4a67b082-0a4c-41cf-b6c7-440b29bb8c4f
```

ALL read-only `Loader*` variables and both one-shot vars listed below
live under this GUID. Impossible OS-specific extension capabilities
live under our own vendor GUID (`6f35d3a4-c0e6-4a82-b5d8-7c9d2e4f8a13`)
in a separate variable named `ImpossibleOSLoaderFeaturesExt` so we
never lie about systemd-boot capability bits.

## Read-only variables (bootloader -> OS)

Every variable in this table describes the boot that wrote it, so each is published **volatile** (`EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS`, no `NON_VOLATILE`): firmware drops it at reset, and no value outlives its boot. systemd-boot publishes the same names the same way (its `export-vars.c` passes no `NON_VOLATILE` flag). The publisher is `boot_loader_vars_publish()` in `src/boot/uefi/loader_vars.c`; it runs after the policy decision lands in `boot_info` and after the per-entry counter decrement, BEFORE the kernel handoff. The two timing variables are cleared there and set again by `boot_loader_vars_publish_timing()` just before the final memory map, once the TSC is calibrated; that call logs `[BOOT] loader-vars: timing, published N, cleared M, failed K`.

Before a chainload to another loader (a detected Windows or Linux boot manager), every variable in the table is cleared instead: the chained loader reports its own boot. This is this loader's own policy, not a systemd-boot rule: systemd-boot fills its common set (`LoaderDevicePartUUID`, `LoaderImageIdentifier`, `LoaderFirmwareInfo`, `LoaderFirmwareType`) only when absent, so a stale value would survive into its boot, and a chained Windows loader publishes nothing, so absent is accurate where a leftover would not be. The serial line then reads `[BOOT] loader-vars: chainload, published 0, cleared 11, failed 0`.

Each variable has two outcomes per boot: **set** to this boot's value, or **cleared** because this boot cannot determine it. Absent means unknown; a value is never left over from an earlier boot. Before every write the variable is deleted (attributes 0, size 0, which deletes whatever attributes the old copy had): until 2026-11-08 the loader wrote these `NON_VOLATILE`, and UEFI refuses to rewrite an existing variable with different attributes, so the delete is also what migrates those old copies on the first boot of the new loader.

| Variable | Format | Set when | Cleared when |
|---|---|---|---|
| `LoaderInfo` | UCS-2 string | Always: `"Impossible OS bootloader 1.0"`. | Never. |
| `LoaderFirmwareInfo` | UCS-2 string | `<gST->FirmwareVendor> <major>.<minor>` from `gST->FirmwareRevision`, the minor padded to two digits as systemd-boot prints it (`EDK II 1.00`). | No system table, an empty vendor, or a vendor over 256 characters (never truncated). |
| `LoaderFirmwareType` | UCS-2 string | `"UEFI <major>.<minor>"` from `gST->Hdr.Revision`, minor padded the same way (`UEFI 2.70`, `UEFI 2.00`). | No system table. |
| `LoaderImageIdentifier` | UCS-2 string | The loader's own ESP path as firmware reports it in LoadedImage FilePath (for example `\EFI\BOOT\BOOTX64.EFI`), as systemd-boot does. | The device path cannot be measured with GetDevicePathSize or is not a plain file path. |
| `LoaderDevicePartUUID` | UCS-2 string | The ESP is on a GPT disk: its partition GUID, 36-char lowercase RFC 4122. | MBR or unknown partition style, or an all-zero or all-FF GUID. An MBR disk signature is not a partition UUID. |
| `LoaderEntries` | UCS-2, NUL-separated | The store was accepted: the visible entry ids in menu order, double-NUL terminated. | The store was rejected or absent, or no entry is visible. |
| `LoaderEntrySelected` | UCS-2 string | An entry was selected: its id (mirrors `boot_info.selected_entry_id`). | Nothing was selected (for example `FALLBACK_STORE_INVALID`). |
| `LoaderTimeInitUSec` | UCS-2 decimal | The TSC was calibrated: microseconds from reset (the TSC counts from reset) to loader entry. | Not measured, or 0. Set by the late timing call. |
| `LoaderTimeExecUSec` | UCS-2 decimal | As above, from reset to just before the final memory map and kernel handoff. `systemd-analyze` reports the loader's time as Exec minus Init. | Not measured, or 0 (systemd rejects a zero ExecUSec). |
| `LoaderFeatures` | uint64 LE | Always: capability bitmap (see below). | Never. |

`ImpossibleOSLoaderFeaturesExt` (below) follows the same rules.

**Not published: `LoaderConfigTimeout` and `LoaderEntryDefault`.** These are persistent settings that `bootctl set-timeout` and `bootctl set-default` write for the boot loader to honour, so the per-boot publisher never sets or clears them. Until 2026-11-08 this loader wrote both every boot as a report of its own effective timeout and default; that overwrote, and on a rejected store deleted, a setting someone else owns. `LoaderConfigTimeout` is still not honoured (bit 0 is not advertised).

**`LoaderEntryDefault` is honoured (bit 2, since 2026-11-09).** The loader reads it once per boot: a printable-ASCII id of at most 62 characters, with or without a trailing NUL, that names a candidate entry becomes the default. It ranks below a `LoaderEntryOneShot`, a tagged `BootCurrent` option and every forced selection, and above the lowest `sort_key` (see [boot policy](boot-policy.md#2-the-precedence-ladder)). An id that names no candidate is ignored, and so is one that names a discovered entry (another OS found on disk, `chainload-N`): those ids are discovery slots renumbered every boot, so `d` refuses to save them too. A malformed or unreadable value is logged and left in place, because it is the operator's. The boot menu's `d` key writes it (`NV|BS|RT`, UCS-2 with a NUL), or deletes it when the highlighted entry already is the saved default. A value an older build of this loader left as a report is honoured like any other; it names the entry that was the default when it was written, and `d` on that entry clears it.

## One-shot variables (OS -> bootloader)

The OS writes these to request a one-shot override for the next boot.
The bootloader reads + DELETES each var after consumption so the
override fires exactly once.

| Variable | Format | Semantic |
|---|---|---|
| `LoaderEntryOneShot` | UCS-2 string | Next boot will select this entry id (overlays `bootcurrent_entry_id` in the policy ladder). |
| `LoaderConfigTimeoutOneShot` | UCS-2 decimal | Next boot's menu timeout in seconds (overrides per-entry `timeout_override`). Cap: 3600s. |

A malformed value (non-printable-ASCII id, non-numeric timeout, value
exceeding 3600s) is **deleted without honoring** -- the next boot
reverts to the configured default. This prevents a stuck one-shot from
breaking the system.

An id is honoured only whole: a non-printable character anywhere, or an id longer than the 64-character buffer, makes the value malformed rather than cut short. A one-shot whose delete the firmware refuses is **not honoured** either, because it would otherwise apply on every boot. The loader logs `[BOOT] loader-vars: <name> delete refused -- not honoured` and sets `boot_info.loader_vars_degraded`. A read error other than not-found is not taken as absence: the variable is deleted unread, not honoured, logged as `unreadable` and reported the same way. The consumer is `boot_loader_one_shot_consume()` in `src/boot/uefi/loader_vars.c`, tested against the in-memory store in `test_loader_vars.c`.

## LoaderFeatures capability bitmap

Bit positions follow the upstream systemd-boot Boot Loader Interface
spec (`https://systemd.io/BOOT_LOADER_INTERFACE/`) **verbatim**.
Advertising a bit means the bootloader implements the documented
semantic; not advertising means the capability is absent. Lying about
capability bits would break `bootctl status` reports and one-shot /
boot-counting behavior.

| Bit | Upstream semantic | Impossible OS | Notes |
|---:|---|:---:|---|
| 0 | `LoaderConfigTimeout` writable from OS | **not advertised** | We do not honor OS-side writes to base timeout. |
| 1 | `LoaderConfigTimeoutOneShot` honored | **advertised** | Bootloader reads + deletes per-boot. |
| 2 | `LoaderEntryDefault` writable from OS | **advertised** | Read every boot; the menu's `d` key writes it. Ranks above the lowest `sort_key`. |
| 3 | `LoaderEntryOneShot` honored | **advertised** | Bootloader reads + deletes per-boot. |
| 4 | Boot counting (BLS tries=/done=) | **advertised** | Counter protocol from boot-entry §5. |
| 5 | XBOOTLDR partition | **not advertised** | We do not support the XBOOTLDR partition. |

The published bitmap is exactly `(1<<1) | (1<<3) | (1<<4) = 0x1A`.

## Impossible OS extension features

The variable `ImpossibleOSLoaderFeaturesExt` (under
`IMPOSSIBLE_OS_VENDOR_GUID`) advertises capabilities specific to
Impossible OS that are not part of the systemd-boot surface. Linux
tools never read this; Impossible-OS-aware userspace (sysinfo CLI,
future bootcfg native binary) does.

| Bit | Capability |
|---:|---|
| 0 | Per-decision audit JSONL (`X:\Boot\history.jsonl`) -- shipped by the policy-audit feature. |
| 1 | Per-entry kinds (split / UKI / chainload / network / resume / safe / recovery / installer / diagnostics / test) -- shipped by the entry-kinds feature. |
| 2 | Per-entry health-gated mark-good (`ImpossibleOS-MarkGood` cross-boot record) -- shipped by the health-gate feature. |
| 3 | Demote-not-drop UX (entries with `tries_left=0` stay visible with `last_failure_reason`). |

## Failure model

A `SetVariable` failure (variable store full, firmware refusal, runtime services unavailable) sets `boot_info.loader_vars_degraded = 1` and boot continues; the kernel surfaces the flag in the boot audit JSONL. Because the delete runs first, a write that fails leaves the variable absent rather than holding an older value. A delete that fails is also a failure, and the write is skipped. The loader prints one line per boot, `[BOOT] loader-vars: published N, cleared M, failed K`, and `scripts/test-smoke.sh` fails a boot whose line reports a failure (the valid-store leg also requires exactly 9 set and 2 cleared).

The kernel boot tests (`src/kernel/test/test_loader_vars.c`) run the same publisher against an in-memory variable store that drops volatile variables at a simulated reset and refuses attribute-changing rewrites as UEFI does, so the two-boot behaviour, the migration of old persistent copies and the failure paths are all exercised.

## systemd-boot compatibility statement

A Linux user-mode tool that reads the systemd-boot Boot Loader
Interface against an Impossible OS-booted system observes:

- The variables in the table above, each present when this boot knows its value and absent when it does not; none carries a value from an earlier boot.
- `LoaderEntries` parseable as a NUL-separated list.
- `LoaderEntrySelected` matches an id in `LoaderEntries`.
- `LoaderFeatures` advertises exactly the bits the bootloader honors.
- `LoaderEntryOneShot` writable -- the next reboot will boot the
  named entry once, then revert.

`bootctl status`, `bootctl list`, `systemctl reboot
--boot-loader-entry=<id>`, and similar commands operate against this
surface without modification.

## Security model

- The reserved-name guard in `NtSetSystemEnvironmentValueEx` does
  **not** block user-mode writes to `Loader*` vars under the
  systemd-boot vendor GUID: that would break the one-shot interface
  Linux tools depend on. The read-only variables are volatile, and the bootloader deletes and rewrites each one every boot, so a value written from user mode does not survive into the next boot's report.
- Impossible OS-owned NV vars (`ImpossibleOS-MarkGood`,
  `ImpossibleOS-CurBootCtr`, `ImpossibleOS-HealthSubset`,
  `ImpossibleOS-BootSticky`) remain guarded by the syscall-level
  reserved-name check.
- `ImpossibleOSLoaderFeaturesExt` is published under our vendor GUID;
  user-mode reads are allowed, writes are not (the syscall guard
  rejects writes to the vendor GUID by default).
