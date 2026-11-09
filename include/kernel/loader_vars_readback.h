/*
 * include/kernel/loader_vars_readback.h -- post-boot check of the Boot Loader
 * Interface variables the loader published.
 *
 * The loader sets or clears its LoaderXxx variables (vendor GUID
 * 4a67b082-0a4c-41cf-b6c7-440b29bb8c4f, attributes BS+RT, never NV) just before
 * handing over, and records what it chose in boot_info. After boot the kernel
 * reads two of them back through runtime services and checks that the firmware
 * kept them and that they agree with boot_info: LoaderInfo must be there, and
 * LoaderEntrySelected must name boot_info.selected_entry_id, or be absent when
 * nothing was selected. Contract: docs/boot/loader-vars.md.
 */
#ifndef KERNEL_LOADER_VARS_READBACK_H
#define KERNEL_LOADER_VARS_READBACK_H

#include "kernel/types.h"
#include "kernel/nt/ntstatus.h"
#include "kernel/uefi_vars.h"  /* efi_guid_t */

/* systemd Boot Loader Interface vendor GUID (Linux compat, do not change). */
#define LOADER_INTERFACE_GUID_INIT \
    ((efi_guid_t){ 0x4a67b082, 0x0a4c, 0x41cf, \
        { 0xb6, 0xc7, 0x44, 0x0b, 0x29, 0xbb, 0x8c, 0x4f } })

/* Longest LoaderEntrySelected the loader can publish: a 63-char id + NUL. */
#define LOADER_READBACK_ID_CHARS 64u

enum loader_readback_verdict {
    LOADER_READBACK_MATCH = 0,   /* present and equal, or absent as expected */
    LOADER_READBACK_MISSING,     /* expected, but the firmware does not have it */
    LOADER_READBACK_UNEXPECTED,  /* nothing was selected, yet the variable exists */
    LOADER_READBACK_MISMATCH,    /* present, but the value is not what boot_info holds */
    LOADER_READBACK_BAD_ATTRS,   /* present, but not exactly BS+RT */
    LOADER_READBACK_UNREADABLE,  /* any other firmware error */
    LOADER_READBACK_VERDICT_COUNT
};

/* Pure. Classify one GetVariable of LoaderEntrySelected against `want_id`
 * ("" when the loader selected nothing). `data`/`bytes` are what the read
 * returned; they are only looked at when `st` is STATUS_SUCCESS. */
enum loader_readback_verdict
loader_readback_classify_selected(NTSTATUS st, uint32_t attrs,
                                  const uint16_t *data, size_t bytes,
                                  const char *want_id);

/* Pure. Classify one GetVariable of LoaderInfo: it must exist, be BS+RT, and
 * hold a non-empty NUL-terminated UCS-2 string. */
enum loader_readback_verdict
loader_readback_classify_info(NTSTATUS st, uint32_t attrs,
                              const uint16_t *data, size_t bytes);

/* Name of a verdict for the log ("match", "missing", ...). */
const char *loader_readback_verdict_name(enum loader_readback_verdict v);

/* Phase 3: read both variables back, log one line, and return the number of
 * variables that did not match (0 on a consistent boot). */
unsigned int loader_vars_readback(void);

#endif /* KERNEL_LOADER_VARS_READBACK_H */
