<!-- docs: covers=todo/08-graphics-ui/TODO-A-Win32k-Shadow-SSDT-Master-Table.md sources=include/kernel/nt/ssdt.h,src/kernel/nt/ssdt.c reviewed=2026-09-29 order=17 -->
# Win32k Shadow SSDT Master Table

## What is it?

The master table is the ledger of every planned Win32k shadow service: one row per service number from `0x1000`, giving its index, function name, the [Win32k Shadow SSDT](win32k-shadow-ssdt.md) section that implements it, an owner and a done mark. It lists 1,300 services in 29 functional ranges, from device contexts at `0x1000` to user-mode callback thunks ending at `0x15A2`. No row is done yet, which matches the code: no shadow service is registered.

## How does it work?

**The ledger.** Rows are grouped under range headings: GDI objects, drawing, text, bitmaps, pens and regions; USER windows, messages, input, menus and the clipboard; then paths, transforms, printing, advanced fonts, window properties, dialogs, keyboard and hooks, DPI and accessibility, touch, monitors, shell integration, DirectX, the 55 Impossible OS extensions at `0x1300` to `0x1336`, and internal GDI, DXGK and USER ranges above them.

**Against the code.**

- **Done marks.** All 1,300 rows are `[ ]`, and `ssdt_register()` is never called for table 1 ([`ssdt.c`](../../src/kernel/nt/ssdt.c)), so the ledger and the code agree.
- **Capacity.** The shadow table holds 1,024 slots, `0x1000` to `0x13FF` ([`ssdt.h`](../../include/kernel/nt/ssdt.h)). The 419 rows numbered `0x1400` and above cannot be registered as written; either the table grows (with the filter bitmap) or those rows move. The decision is filed in [Shadow SSDT Infrastructure](../../todo/08-graphics-ui/TODO-15-win32k-shadow-ssdt.md#1-shadow-ssdt-infrastructure-table-1-dispatch).
- **Headings.** Several range headings overlap their neighbours (for example `0x1130-0x116F` and `0x1160-0x117F`), so a heading is a label, not a reservation; the row indexes are what count.
- **No automated check.** Unlike the [main table's ledger](../kernel/ssdt-master-table.md), nothing yet reconciles this file against the code; extending the audit to table 1 is part of [Static asserts, headers, and audits](../../todo/08-graphics-ui/TODO-16-win32k-shadow-native-api.md#3-static-asserts-headers-and-audits).

## What are its interfaces?

| Interface | Status |
| --- | --- |
| The ledger columns: Index, Function, section, Owner, Done | In use (all rows `[ ]`) |
| `SSDT_SHADOW_MAX` (1,024), the table the rows register into | Shipped |
| `WIN32K_SSDT_COUNT` asserted against the ledger's total | Planned |
| An audit reconciling registrations against this file | Planned |

## How do I use it?

Before implementing a shadow service, find its row, register the handler at that index in the owning section, and mark the row done in the same commit. Count planned and done rows with `grep -c '^| 0x' todo/08-graphics-ui/TODO-A-Win32k-Shadow-SSDT-Master-Table.md`.

## What is not implemented yet?

- Every service in the ledger; the waves are listed on the [Win32k Shadow SSDT](win32k-shadow-ssdt.md) page.
- Fitting the numbering into the table: [Shadow SSDT Infrastructure (Table 1 Dispatch)](../../todo/08-graphics-ui/TODO-15-win32k-shadow-ssdt.md#1-shadow-ssdt-infrastructure-table-1-dispatch).
- The ledger check: [Static asserts, headers, and audits](../../todo/08-graphics-ui/TODO-16-win32k-shadow-native-api.md#3-static-asserts-headers-and-audits).

## How does it compare with Windows 11 and Linux?

Windows 11 has no public ledger: the order of `win32k.sys` services changes between builds, which is why the user-mode stubs in `win32u.dll` are rebuilt with each release. Linux numbers its single table in `syscall_64.tbl`, and a number is never reused once assigned. Impossible OS takes the Linux habit of a fixed, checked-in table and applies it to the Windows two-table layout.

## See also

- [Win32k Shadow SSDT Master Table](../../todo/08-graphics-ui/TODO-A-Win32k-Shadow-SSDT-Master-Table.md)
- [SSDT Master Table](../kernel/ssdt-master-table.md) for the main table
- [Win32k Shadow SSDT](win32k-shadow-ssdt.md)
- [Win32k Shadow Native API](win32k-shadow-router.md)
