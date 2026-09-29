---
schema_version: 1
id: win32k-shadow-native-api
domain: 08-graphics-ui
status: active
title: "TODO-16 -- Win32k Shadow Native API (SSDT Table 1 Router)"
---

# TODO-16 -- Win32k Shadow Native API (SSDT Table 1 Router)

> **Goal:** Define the **contract** between the existing main native SSDT ([`../02-kernel-core/TODO-12-native-api-ssdt.md`](../02-kernel-core/TODO-12-native-api-ssdt.md), Table 0, `0x0000+`) and Win32k **Table 1** services (`0x1000+`). Ring 3 still uses the same `SYSCALL` / `SYSRET` fast path; only the dispatch target changes once the service number selects Table 1. This TODO does **not** own per-function GDI/USER bodies: those live in [`TODO-15-win32k-shadow-ssdt.md`](TODO-15-win32k-shadow-ssdt.md). The **1300-row slot map** is [`TODO-A-Win32k-Shadow-SSDT-Master-Table.md`](TODO-A-Win32k-Shadow-SSDT-Master-Table.md), parallel to [`../02-kernel-core/TODO-A-SSDT-Master-Table.md`](../02-kernel-core/TODO-A-SSDT-Master-Table.md) for Table 0.

> **Why a separate file:** Kernel core `TODO-12` stays focused on `NTSTATUS`, main SSDT wiring, Zw aliases, audit/filter hooks, and the 470-entry native surface. The ~1300 Win32k shadow services are owned by **08 Graphics UI** (same domain as `win32k.sys` on Windows). This file is the `TODO-12`-shaped **router** slice for that domain only.

## Inputs

- [`../02-kernel-core/TODO-12-native-api-ssdt.md`](../02-kernel-core/TODO-12-native-api-ssdt.md) -- `ssdt_dispatch`, `ssdt_init`, filter bitmaps, audit hook, `KeUserModeCallback` (§25, §26)
- [`../02-kernel-core/TODO-A-SSDT-Master-Table.md`](../02-kernel-core/TODO-A-SSDT-Master-Table.md) -- Table 0 numbering discipline (reference for how `TODO-A` files are maintained)
- [`TODO-A-Win32k-Shadow-SSDT-Master-Table.md`](TODO-A-Win32k-Shadow-SSDT-Master-Table.md) -- authoritative Table 1 indices and owner column
- [`TODO-15-win32k-shadow-ssdt.md`](TODO-15-win32k-shadow-ssdt.md) -- `win32k_init`, handler implementation sections, Win32k unit tests
- [`TODO-14-win32-gdi-user32-stubs.md`](TODO-14-win32-gdi-user32-stubs.md) -- PE stubs that must match `TODO-A` indices
- [`../12-user-platform-sdk/TODO-04-ntdll-user-runtime.md`](../12-user-platform-sdk/TODO-04-ntdll-user-runtime.md) -- thin syscall wrappers from user mode; index literals must track `TODO-A`

## Outcome

- Table 1 service numbers use one contiguous band starting at `WIN32K_SERVICE_BASE` (planned `0x1000`) through the high water mark in `TODO-A` (1300 entries), without colliding with Table 0. Today the table holds 1,024 slots (`SSDT_SHADOW_MAX`), so rows past `0x13FF` cannot register; the fix is owned by `TODO-15` §1.
- Unimplemented Table 1 slots return `STATUS_NOT_IMPLEMENTED` using the same stub policy as Table 0.
- `WIN32K_SSDT_COUNT` (or equivalent) is static-asserted against `TODO-A` so header and docs cannot drift silently.
- Optional kernel `ZwGdi*` / `ZwUser*` fast paths reuse the privilege model from `02-kernel-core/TODO-12` §12 where Win32k must be callable from kernel mode without user probes.

## Implementation Order

| Step | Deliverable                                                             | Depends On                   | Status |
| ---- | ----------------------------------------------------------------------- | ---------------------------- | ------ |
| 1    | Table 1 base, bounds check, dispatch to Win32k handler array            | `02-kernel-core/TODO-12` §4  | [ ]    |
| 2    | `win32k_init()` registration order after compositor prerequisites       | `02-kernel-core/TODO-12` §1  | [ ]    |
| 3    | Extend or reuse per-process syscall filter bitmap for shadow indices    | `02-kernel-core/TODO-12` §25 | [ ]    |
| 4    | Public constants in `include/kernel/nt/win32k_ssdt.h` matching `TODO-A` | `TODO-A`                     | [ ]    |
| 5    | Co-review `KeUserModeCallback` usage with `02-kernel-core/TODO-12` §26  | `02-kernel-core/TODO-12` §26 | [ ]    |

## 1. Dispatch split and guard rails

- [x] Bit layout documented: bits 13:12 select the table, 11:0 the index (`SSDT_TABLE_SHIFT` / `SSDT_INDEX_MASK` in `ssdt.h`; `docs/graphics/win32k-shadow-router.md`); it must stay stable for the stubs
- [x] Out-of-range Table 1 indices return `STATUS_NOT_IMPLEMENTED` (`index >= table->max` in `ssdt_dispatch()`, `src/kernel/nt/ssdt.c`), never a silent wrap
- [ ] Legacy `SYS_GETMESSAGE` style numbers migrate only through the explicit checklist in `08-graphics-ui/TODO-15` §12 (numbers allocated per `02-kernel-core/TODO-12` §33), not dual live paths

## 2. NTSTATUS and user-buffer rules

- [ ] Every Table 1 handler returns `NTSTATUS`; status code headers are shared with `02-kernel-core/TODO-12` §1
- [ ] Any user pointer touched from `NtGdi*` / `NtUser*` follows probe rules from `02-kernel-core/TODO-23-exception-dispatch-seh.md` §13

## 3. Static asserts, headers, and audits

- [ ] `WIN32K_SSDT_COUNT` (or max index + 1) matches the `TODO-A` footer total (1300 rows)
- [ ] When `/audit-ssdt` grows Table 1 support, wire it so CI fails on orphan registrations or undocumented slots

## 4. User-mode index contract

- [ ] `gdi32.dll` / `user32.dll` syscall indices are generated or asserted from the same values as `TODO-A` (no handwritten drift)

## 5. KeUserModeCallback integration review

Co-review note (owned by `02-kernel-core/TODO-12-native-api-ssdt.md §26`): Win32k shadow syscalls that return via `KeUserModeCallback` must reuse the existing upcall frame layout rather than inventing a new one. No code ships here; this section is a review gate before the TODO is closed.

- [ ] Confirm the callback index table (planned with `02-kernel-core/TODO-12` §26; `include/kernel/nt/callback.h` does not exist yet) covers every Win32k shadow entry that requires a user-mode upcall.
- [ ] Commit: `"todo: mark §5 review gate complete"` (no code change expected).

**Test checkpoint:** review note added to the canonical handoff doc; no functional test required.

---

## Unit Tests

- [ ] Dispatch: first shadow index (`0x1000`) routes to Win32k after `win32k_init()`; illegal table id still fails the same way as main SSDT tests in `02-kernel-core/TODO-12` §4
- [ ] Register in the same test category plan as `02-kernel-core/TODO-12` (see its Unit Tests section)

## Verification

- [ ] `bash scripts/build.sh clean` then `tail -1 build/build.log` shows `=== BUILD OK ===`
- [ ] Serial shows shadow SSDT registration line with expected service count once `02-kernel-core/TODO-12` §1 is complete
- [ ] All four platforms from `CLAUDE.md` (QEMU WHPX, QEMU TCG, VirtualBox, bare metal) when Win32k init is enabled
