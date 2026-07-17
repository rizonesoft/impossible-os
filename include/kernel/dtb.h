/* ============================================================================
 * dtb.h -- Devicetree Blob (FDT) header validation + minimal node walker
 *
 * Implements the subset of the Devicetree Specification v0.4 needed to
 * validate a flattened DTB handed to the kernel via UEFI configuration
 * table (linaro,dtb GUID) on EBBR-class platforms and to count the
 * top-level discovery nodes (/chosen, /memory, /cpus) so the firmware-
 * platform arbitration policy in firmware_platform.c can decide whether
 * the DTB handoff is real and complete enough to trust.
 *
 * Scope here is deliberately narrow:
 *   - Validate the FDT header (magic 0xD00DFEED big-endian, totalsize
 *     plausible, version >= 17 last_comp_version <= 17).
 *   - Walk the structure block once, counting nodes whose names match
 *     "chosen", "memory", "memory@*", "cpus", "cpu@*".
 *   - Expose count + total-size getters.
 *
 * Full property extraction (/chosen/bootargs strings, the per-region
 * /memory@N reg ranges, /cpus/cpu@N compatible strings) is owned by
 * the dedicated DTB parser TODO and
 * the firmware-quirk database section; this header is intentionally
 * minimal so the platform-arbitration policy has a hard "is the DTB
 * real?" oracle without pulling in a full FDT library.
 *
 * SMP: dtb_init runs once on the BSP from firmware_platform_init at
 * Phase 1. Read-only thereafter -- no locks required on the getters.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* Devicetree Specification v0.4 5.2: Flattened Devicetree (FDT) header.
 * All fields are stored big-endian on the wire. */
#define FDT_MAGIC                    0xD00DFEEDu
#define FDT_SUPPORTED_VERSION        17u
#define FDT_FIRST_SUPPORTED_VERSION  16u

/* Conservative size cap. Real-world DTBs are well under 1 MiB; cap at
 * 4 MiB defensively so a malformed header cannot force the walker to
 * scan an arbitrarily large region. */
#define FDT_TOTALSIZE_MAX            (4u * 1024u * 1024u)

void dtb_init(uintptr_t phys_addr);

/* True iff dtb_init succeeded on a well-formed FDT header. */
int dtb_is_valid(void);

/* Total size declared in the FDT header (bytes). 0 when invalid. */
uint32_t dtb_total_size(void);

/* Counts populated during dtb_init's structure-block walk. Each is
 * 0 when the DTB is invalid or absent. */
uint32_t dtb_chosen_count(void);
uint32_t dtb_memory_count(void);
uint32_t dtb_cpu_count(void);

#ifdef KERNEL_TESTS
/* Test-flavor only: reset hook so unit tests can re-run dtb_init() against
 * fixture buffers. Declared here rather than privately in each consumer so a
 * signature change is caught by the compiler, not at link time. */
void dtb_reset_for_test(void);
#endif /* KERNEL_TESTS */
