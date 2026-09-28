<!-- docs: covers=todo/02-kernel-core/TODO-A-SSDT-Master-Table.md sources=include/kernel/nt/ssdt.h,src/kernel/nt/ssdt.c,include/kernel/nt/service_numbers.h,.claude/skills/audit-ssdt/SKILL.md reviewed=2026-09-28 order=36 -->
# SSDT Master Table

## What is it?

The SSDT master table is the ledger of every main-table (`NtXxx`) system service Impossible OS plans to expose: which service number maps to which function, which roadmap section owns it, and whether it is done. It is a single Markdown file, [`TODO-A-SSDT-Master-Table.md`](../../todo/02-kernel-core/TODO-A-SSDT-Master-Table.md), not code. Dispatch itself happens in `ssdt_dispatch()`, described in [Native API and the SSDT](native-api-ssdt.md); this page explains how to read the ledger and keep it true.

## How does it work?

Each row has five columns: `Index | Function | § | Owner | Done`. `Index` is the hex service number; main-table indices run from `0x0000` to `0x03FF`, within the `SSDT_MAIN_MAX` capacity of 1024 slots and the low 12 bits selected by `SSDT_INDEX_MASK` ([`ssdt.h`](../../include/kernel/nt/ssdt.h)). `§` names the section of the owning roadmap that specifies the service. `Owner` names the implementing roadmap in the ledger's compact form (a `T` plus the roadmap number, the section, and usually the source file), sometimes with a note such as a deferred IRP path. `Done` is `[x]` for complete, `[/]` for partial or stub, and `[ ]` for unregistered.

Rows are grouped into bold range headings by function, not by owner: File I/O pulls rows from several sections of the Native API roadmap and from other roadmaps, because the ledger is organized the way a Win32 or NT caller looks things up. A fully done range says so on its heading, for example Time and Timer (`0x00F0` to `0x00FF`, 5/5) and ETW (`0x01D0` to `0x01DF`, 7/7). The Win32k shadow table (indices from `0x1000`, `NtGdiXxx` and `NtUserXxx`) has its own ledger, the [Win32k Shadow SSDT Master Table](../../todo/08-graphics-ui/TODO-A-Win32k-Shadow-SSDT-Master-Table.md).

The `Done` column is a claim about the code, not the other way round. At run time `ssdt_dispatch()` in [`ssdt.c`](../../src/kernel/nt/ssdt.c) reads the handler in the live table: an invalid table selector returns `STATUS_INVALID_PARAMETER`, and an index past capacity or still holding the default stub returns `STATUS_NOT_IMPLEMENTED`. Counted from the file on 2026-09-28 (rows matching `^| 0x[0-9A-F]{4} |`, then each `Done` value), the ledger holds 477 rows under 34 range headings: 155 `[x]`, 59 `[/]` and 263 `[ ]`, so 214 rows (44.9%) are at least partly wired. These are ledger labels, not an audit: the same review found `NtSaveKey`, `NtSaveKeyEx` and `NtRestoreKey` marked `[x]` while their handler returns `STATUS_NOT_SUPPORTED` past the privilege check, and downgraded them, so treat any `[x]` as a claim until `/audit-ssdt` has confirmed it. The numeric constants live in [`service_numbers.h`](../../include/kernel/nt/service_numbers.h), where `SSDT_MAIN_COUNT` is also 477.

## What are its interfaces?

| Interface | Purpose |
| --- | --- |
| The ledger file | [`TODO-A-SSDT-Master-Table.md`](../../todo/02-kernel-core/TODO-A-SSDT-Master-Table.md), one row per main-table service number |
| `ssdt_register(service_number, handler)` | The call a `[x]` row claims has happened ([`ssdt.c`](../../src/kernel/nt/ssdt.c)) |
| `ssdt_dispatch()` | Reads the live table at the row's index |
| `SSDT_MAIN_MAX`, `SSDT_TABLE_SHIFT`, `SSDT_INDEX_MASK` | Capacity and bit-layout constants an index must respect ([`ssdt.h`](../../include/kernel/nt/ssdt.h)) |
| `SSDT_Nt*` defines, `SSDT_MAIN_COUNT` | The numeric source of truth an `Index` must match ([`service_numbers.h`](../../include/kernel/nt/service_numbers.h)) |
| `/audit-ssdt` | The reconciliation workflow that re-derives the `Done` column from the code ([`SKILL.md`](../../.claude/skills/audit-ssdt/SKILL.md)) |

## How do I use it?

To find where a service lives, or confirm it does not exist yet, search the ledger for the function name and read `Owner` and `Done`. To check the ledger against the tree instead of trusting it, run the audit workflow in Claude Code:

```
/audit-ssdt
```

It collects every `ssdt_register()` call and every `NTSTATUS Nt*` handler, checks `service_numbers.h` for duplicate or missing values, reads each `[x]` handler's body to confirm it does real work, and rewrites the `Done` column and the progress line. The audit only ever downgrades on evidence: a `[x]` row whose handler turns out partial becomes `[/]` or `[ ]`. `/implement-ssdt-range` is the companion workflow that implements a block of open rows.

## What is not implemented yet?

- **263 unregistered rows (55.1%).** They span every range, with large blocks in debug and exception services (owned by the [Exception Dispatch and SEH](../../todo/02-kernel-core/TODO-23-exception-dispatch-seh.md) and [Kernel Debugger](../../todo/02-kernel-core/TODO-29-kernel-debugger-kd-protocol.md) roadmaps), power (the [Power Management](../../todo/02-kernel-core/TODO-26-power-management.md) roadmap) and the kernel transaction manager.
- **59 partial rows (12.4%).** A handler exists but is incomplete, for example IRP-based file controls such as `NtDeviceIoControlFile` and `NtFsControlFile`, and `NtSaveKey`/`NtRestoreKey`, which pass their privilege checks and then return `STATUS_NOT_SUPPORTED` because resolving their `FileHandle` to a path does not exist yet; that item is parked in the [Registry Completion](../../todo/02-kernel-core/TODO-14-registry-completion.md#2-advanced-key-operations) roadmap.

Each open row is owned by the roadmap section named in its `Owner` column, which is where the work is tracked; the ledger has no sections of its own.

## How does it compare with Windows 11 and Linux?

The ledger has no comparison table of its own because it is an inventory, not a feature. Its shape follows Windows: a flat service-number space grouped by function, with a separate shadow table for the Win32k graphics and window services, the same split as `ntoskrnl.exe` and `win32k.sys`. Linux has one linear system call table with no table-select bits, so the comparison is structural only.

## See also

- [SSDT Master Table ledger](../../todo/02-kernel-core/TODO-A-SSDT-Master-Table.md)
- [Native API and the SSDT](native-api-ssdt.md)
- [Kernel Bulletproofing](kernel-bulletproofing.md)
