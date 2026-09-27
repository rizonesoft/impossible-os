<!-- docs: covers=todo/01-boot-platform/TODO-03-bootloader-error-recovery.md sources=include/kernel/boot_info.h,src/boot/uefi/boot_info_mirror.h reviewed=2026-09-28 -->
# Boot Error History Ring -- Schema and Operator Guide

> Wire format and operator-visible decode tables for the multi-attempt
> boot diagnostics ring shipped by the boot-error-history TODO sections
> in `todo/01-boot-platform/TODO-03-bootloader-error-recovery.md`.
> Producer: schema + 3 append sites + NVRAM cookie. Consumer:
> storage reader, kernel renderer, unit tests, and the cascade smoke
> fixture.

The ring captures the last 8 boot attempts so a cascade-failure
sequence (retry storm, repeated kernel-load corruption, etc.) leaves
an ordered trail instead of overwriting a single-slot record on each
iteration. It complements the single-slot `BootError` channel: the
single slot remains the operator's "what was the most recent boot
fatal?" query; the ring answers "show me the attempt history."

## Storage layout

Two UEFI variables under the `IMPOSSIBLE_OS_VENDOR_GUID`
(`{6F35D3A4-C0E6-4A82-B5D8-7C9D2E4F8A13}`) namespace, both with
`NV+BS+RT` attributes:

| Variable name       | Size    | Description                                |
| ------------------- | ------- | ------------------------------------------ |
| `BootHistorySeq`    | 4 bytes | u32 monotonic cookie. `seq` increments by 1 per append. |
| `BootErrorHistory`  | 128 bytes | Fixed 8-entry ring. `head = seq % 8`.    |

The 128-byte ring fits any UEFI NVRAM quota. The
`X:\Diag\boot-error-history.bin` BlackBox-file path is reserved for
future expansion (when the ring outgrows NVRAM); the current producer
is NVRAM-only and the consumer reads NVRAM exclusively.

## Per-entry layout (`struct boot_error_history_entry`, 16 bytes)

```c
struct boot_error_history_entry {
    uint32_t boot_seq;        /* offset  0: monotonic from BootHistorySeq */
    uint32_t unix_time;       /* offset  4: epoch seconds (0 = no clock)  */
    uint16_t err_code;        /* offset  8: BOOT_ERR_* or 0 for sentinels */
    uint16_t source_section;  /* offset 10: phase code or BOOT_SECTION_*  */
    uint32_t _pad;            /* offset 12: zero on write, ignored on read*/
};
```

Layout pinned by `_Static_assert` blocks on both sides
(`include/kernel/boot_info.h` + `src/boot/uefi/boot_info_mirror.h`).
A failed assert means kernel + bootloader drifted; rebuild both
before shipping.

## Atomicity contract: ring-FIRST cookie-LAST

UEFI 2.10 `SetVariable` is per-variable atomic; there is no
transactional batch. The producer always:

1. Reads existing cookie + ring (validate size + attrs; on mismatch
   delete with returned attrs and treat as absent -- mirrors the §13
   `BootError` repair pattern).
2. Stamps the new entry at `head = (seq+1) % 8` with `unix_time` from
   `RuntimeServices->GetTime()` (0 if unavailable).
3. Writes the **ring variable first** (full 128 bytes, NV+BS+RT).
4. Writes the **cookie second** (4 bytes, NV+BS+RT).

A torn write between steps 3 and 4 leaves the cookie pointing at the
previous slot; readers compute `head = seq % 8` against the un-
incremented cookie and see the prior head, never the half-written
new entry. Bounded loss; never UB.

### Cookie-wrap guard

`new_seq = seq + 1`; if `new_seq == 0` (wrap from `UINT32_MAX` or an
attacker-seeded cookie), the producer promotes `new_seq` to 1. This
avoids a collision with the empty-slot sentinel `boot_seq == 0` that
the consumer's "count non-zero entries" pass uses to detect ring
boundaries.

## Append sites

| Site                       | `source_section`                | `err_code`     |
| -------------------------- | ------------------------------- | -------------- |
| Bootloader `boot_fatal()`  | `g_boot_section` hint (0x01NN)  | `BOOT_ERR_*`   |
| Bootloader EBS-success     | `BOOT_SECTION_EBS_OK` 0xFFFE    | 0              |
| Kernel `boot_phase3()`     | `BOOT_SECTION_KERNEL_PHASE3` 0xFFFF | 0          |

A clean boot writes 2 entries (EBS-success + kernel Phase-3); a fatal
adds 1 fatal entry instead of the EBS-success / Phase-3 pair.

### Source-section enum

| Value     | Symbol                          | Meaning                          |
| --------- | ------------------------------- | -------------------------------- |
| 0x0101    | `BOOT_SECTION_BL_INIT`          | `efi_main` entry                 |
| 0x0102    | `BOOT_SECTION_BL_CONF`          | `parse_boot_conf`                |
| 0x0103    | `BOOT_SECTION_BL_KERNEL`        | `load_kernel`                    |
| 0x0104    | `BOOT_SECTION_BL_PAGETABLES`    | `setup_page_tables`              |
| 0x0105    | `BOOT_SECTION_BL_EBS`           | EBS retry loop entry             |
| 0xFFFD    | `BOOT_SECTION_UNKNOWN`          | Hint never set (path-attribution gap; logged WARN) |
| 0xFFFE    | `BOOT_SECTION_EBS_OK`           | Bootloader reached `ExitBootServices` |
| 0xFFFF    | `BOOT_SECTION_KERNEL_PHASE3`    | Kernel reached steady state      |

The 0x01NN range cannot collide with the `BOOT_ERR_*` registry
(currently capped at 0x0013) or the three high sentinels.  The
`BOOT_ERR_*` registry lives in the `err_code` field, not in
`source_section`; the producer never writes a `BOOT_ERR_*` value into
`source_section`, so the consumer renderer's "fall back to
`section-0xNNNN`" behavior for unknown values is correct for any
future code that escapes the documented ranges.

### Producer trust model

The producer-side reader treats the following as "found-but-malformed"
and deletes the variable so the next write can land canonical attrs:

- `EFI_BUFFER_TOO_SMALL` / `STATUS_BUFFER_TOO_SMALL` (oversized
  variable, e.g. seeded by a pre-OS tool).
- Non-canonical attributes (any combo other than `NV+BS+RT`).
- Wrong size on success.

Other `EFI_ERROR` codes (`EFI_NOT_FOUND`, unsupported, access denied)
are treated as absent: no delete, ring stays zero-initialised.

## Consumer renderer

`boot_history_render()` runs in `boot_phase0` / `boot_hw.c` after
`uefi_vars_init()` returns. It calls `boot_history_read()` and emits a
klog block when the ring has at least one non-zero entry:

```
[BOOT] Recent boot history (N attempts):
  seq=K time=YYYY-MM-DDTHH:MM:SSZ src=<label> err=0xNNNN
  ...
```

Entries render oldest-first by `boot_seq`. Sentinels decode to stable
labels (`unknown`, `ebs-success`, `kernel-Phase3`); bootloader phases
decode to `bl-init` / `bl-conf` / `bl-kernel` / `bl-pagetables` /
`bl-ebs`; everything else falls back to `section-0xNNNN`.

`unix_time == 0` renders as `(no clock)`.

## Operator decode table: `err_code` -> recovery slug

The ring's `err_code` field reuses the `BOOT_ERR_*` namespace from the
single-slot `BootError` channel. Operators looking up codes can scan
the same recovery URLs that the QR-code error screen uses; the
4-hex-digit suffix in `https://impossibleos.co/err/<NNNN>` matches the
`err_code` value rendered here.

## Quota footprint

Per-machine NVRAM quota is typically 64 KiB on Lenovo-class
firmware; the consumer cap is much higher. Total bytes spent by this
feature plus the related `BootError` and `ImpossibleBootProtoFault`
channels:

| Variable                       | Size    |
| ------------------------------ | ------- |
| `BootError`                    | 4 B     |
| `BootHistorySeq`               | 4 B     |
| `BootErrorHistory`             | 128 B   |
| `ImpossibleBootProtoFault`     | 112 B   |
| **Total**                      | **248 B** |

Well below any per-machine quota.
