<!-- docs: covers=todo/01-boot-platform/TODO-07-boot-entry-store-menu-policy.md sources=src/boot/uefi/bootx64.c,src/kernel/uefi_runtime.c reviewed=2026-09-28 -->
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

All published with `EFI_VARIABLE_NON_VOLATILE | _BOOTSERVICE_ACCESS |
_RUNTIME_ACCESS`. The bootloader writes them after the policy decision
lands in `boot_info` and after the per-entry counter decrement, BEFORE
the kernel handoff.

| Variable | Format | Contents |
|---|---|---|
| `LoaderInfo` | UCS-2 string | `"Impossible OS bootloader 1.0"` -- loader identifier. |
| `LoaderFirmwareInfo` | UCS-2 string | `<gST->FirmwareVendor> <gST->FirmwareRevision>` |
| `LoaderFirmwareType` | UCS-2 string | `"UEFI <major>.<minor>"` from `gST->Hdr.Revision`. |
| `LoaderImageIdentifier` | UCS-2 string | `\EFI\ImpossibleOS\BOOTX64.EFI` -- canonical install path. |
| `LoaderDevicePartUUID` | UCS-2 string | ESP partition GUID (36-char lowercase RFC 4122). Absent if no GPT GUID. |
| `LoaderEntries` | UCS-2, NUL-separated | All entry ids from the parsed store, double-NUL terminated. |
| `LoaderEntryDefault` | UCS-2 string | Entry id with the lowest sort_key (the ladder's preferred default). |
| `LoaderEntrySelected` | UCS-2 string | This boot's selected entry id (mirrors `boot_info.selected_entry_id`). |
| `LoaderConfigTimeout` | UCS-2 decimal | Effective menu timeout in seconds. |
| `LoaderTimeInitUSec` | UCS-2 decimal | Loader entry timestamp in microseconds. 0 if TSC not calibrated. |
| `LoaderTimeExecUSec` | UCS-2 decimal | Kernel handoff timestamp in microseconds. Equal to InitUSec when not measured separately. |
| `LoaderFeatures` | uint64 LE | Capability bitmap (see below). |

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
| 2 | `LoaderEntryDefault` writable from OS | **not advertised** | Default is `sort_key` order; no OS-side override. |
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

Every `gRT->SetVariable` call is wrapped in `loader_set_var()`. On
failure (NVRAM quota full, firmware refusal, runtime services
unavailable), the bootloader sets `boot_info.loader_vars_degraded = 1`
and continues. Userland still boots; the kernel surfaces the degraded
flag in the boot audit JSONL so operators see degraded publication
state.

Bootloader is read-only on `LoaderInfo` / `LoaderEntries` / etc. once
the boot completes: a user-mode write to a read-only var would persist
in NVRAM but the bootloader overwrites it next boot, so consistency
is eventually restored.

## systemd-boot compatibility statement

A Linux user-mode tool that reads the systemd-boot Boot Loader
Interface against an Impossible OS-booted system observes:

- All 12 documented read-only variables present (or `LoaderDevicePartUUID`
  absent on systems without a GPT ESP).
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
  Linux tools depend on. The bootloader simply overwrites read-only
  vars next boot.
- Impossible OS-owned NV vars (`ImpossibleOS-MarkGood`,
  `ImpossibleOS-CurBootCtr`, `ImpossibleOS-HealthSubset`,
  `ImpossibleOS-BootSticky`) remain guarded by the syscall-level
  reserved-name check.
- `ImpossibleOSLoaderFeaturesExt` is published under our vendor GUID;
  user-mode reads are allowed, writes are not (the syscall guard
  rejects writes to the vendor GUID by default).
