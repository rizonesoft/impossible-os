/* ============================================================================
 * boot_version.h -- boot protocol version negotiation and stale-loader
 * error path (kernel-side classifier + fatal rendering + NVRAM
 * persistence + late-boot BlackBox transcription).
 *
 * Boot protocol compatibility policy: EXACT match required between the
 * bootloader's compile-time `BOOT_INFO_VERSION` + `sizeof(struct
 * boot_info)` and the kernel's. The kernel refuses to proceed on any
 * mismatch; there is no downgrade adapter.
 *
 * Rationale: the boot_info ABI is under development, structurally
 * bumped several times per month, and every consumer (PMM reservation,
 * payload validator, typed payload find, runtime services mapping)
 * reads fields by offset. A silently-truncated boot_info copy would
 * hand typed consumers stale bytes from a prior boot's memory, which
 * is exactly the class of drift the /manifest detector was built
 * to catch post-commit. Enforcing strict equality at runtime closes
 * the last window (a user deploying mismatched binaries).
 *
 * When a future release branches its ABI as stable, this header can
 * grow a compatibility window (min/max version pair) without
 * perturbing the fatal-rendering or NVRAM-persistence paths: only
 * boot_version_classify() would change.
 *
 * Fault flow:
 *   1. boot_phase0 calls boot_version_classify(header, expected_size,
 *      &fault). Returns BOOT_OK on match, BOOT_FATAL on any drift.
 *   2. On BOOT_FATAL, caller calls boot_version_render_fatal(&fault)
 *      which prints the observed/expected diagnostics via klog
 *      (serial + framebuffer via boot_halt fallback), attempts an
 *      NVRAM persist via uefi_set_variable() (best-effort -- fails
 *      silently if uefi_runtime is not yet available), and halts.
 *   3. On the next successful boot, boot_version_blackbox_transcribe()
 *      runs late in Phase 3 (after VFS and BlackBox are up), reads any
 *      persisted NVRAM record, writes X:\Diag\boot-proto-fault.txt
 *      with a human-readable transcript, and clears the NVRAM slot so
 *      the record does not replay on every boot.
 * ============================================================================ */

#ifndef KERNEL_BOOT_VERSION_H
#define KERNEL_BOOT_VERSION_H

#include "kernel/types.h"
#include "kernel/boot_init.h"

struct boot_info_header;  /* forward decl -- full definition in kernel/boot_info.h */

/* BVPF = "Boot Version Protocol Fault". NVRAM record readers match
 * this magic to distinguish a legitimate fault record from garbage in
 * a variable that happens to share a name with a future rename. */
#define BOOT_VERSION_FAULT_MAGIC   0x42565046u

enum boot_version_fault_class {
    BOOT_VERSION_OK                = 0,
    BOOT_VERSION_FAULT_NULL_HDR    = 1,  /* header pointer NULL -- pre-validator reach */
    BOOT_VERSION_FAULT_BAD_MAGIC   = 2,  /* header.magic mismatches BOOT_INFO_MAGIC */
    BOOT_VERSION_FAULT_BAD_VERSION = 3,  /* header.version != BOOT_INFO_VERSION */
    BOOT_VERSION_FAULT_BAD_SIZE    = 4,  /* header.size != sizeof(struct boot_info) */
    BOOT_VERSION_FAULT_SEC_ROLLBACK = 5, /* RESERVED for anti-rollback */
};

/* Fixed 48-byte layout. Pinned so the NVRAM record can be read by a
 * future boot_version_blackbox_transcribe() without worrying about
 * layout drift between the boot that wrote the record and the boot
 * that reads it. Adding fields requires bumping a record-version tag
 * (not yet present; reserve _reserved[] for that future extension). */
struct boot_version_fault {
    uint32_t record_magic;             /* BOOT_VERSION_FAULT_MAGIC */
    uint32_t fault_class;              /* enum boot_version_fault_class */
    uint32_t observed_magic;           /* header.magic as seen by kernel */
    uint32_t expected_magic;           /* BOOT_INFO_MAGIC */
    uint16_t observed_version;         /* header.version as seen by kernel */
    uint16_t expected_version;         /* BOOT_INFO_VERSION */
    uint32_t observed_size;            /* header.size as seen by kernel */
    uint32_t expected_size;            /* sizeof(struct boot_info) */
    /* anti-rollback forward-compat slots. Zero today; populated
     * by when the anti-rollback gate is integrated. Distinct
     * fault_class value (BOOT_VERSION_FAULT_SEC_ROLLBACK) tells
     * transcribers "this is a rollback refusal, not structural ABI
     * drift" -- different operator response (rollback refusal means
     * boot a newer kernel; ABI drift means rebuild both halves). */
    uint32_t observed_loader_sec_ver;
    uint32_t expected_loader_sec_ver;
    uint32_t _reserved[3];             /* pad to 48 bytes */
};

_Static_assert(sizeof(struct boot_version_fault) == 48,
    "boot_version_fault layout pinned at 48 bytes");

/* Classify a boot_info header against the kernel's expectations.
 * Fills *out_fault with observed + expected values (whether or not
 * the header matches); fault_class indicates the failure mode.
 * Returns BOOT_OK on match, BOOT_FATAL on any mismatch. */
boot_result_t boot_version_classify(const struct boot_info_header *hdr,
                                    size_t expected_struct_size,
                                    struct boot_version_fault *out_fault);

/* Render the fault via klog (LOG_FATAL) and halt. Does not return.
 * Attempts an NVRAM persist as a side effect; a failed persist is
 * logged but does not block the halt. */
void boot_version_render_fatal(const struct boot_version_fault *fault)
    __attribute__((noreturn));

/* Persist `fault` to a dedicated UEFI NVRAM variable
 * (ImpossibleBootProtoFault). Returns 1 on successful write, 0
 * otherwise. At early boot the uefi_runtime service layer may not be
 * available yet; this function tolerates that (returns 0). */
int boot_version_persist_nvram(const struct boot_version_fault *fault);

/* Late-boot transcription hook. Reads any persisted NVRAM record,
 * writes X:\Diag\boot-proto-fault.txt, and clears the NVRAM slot.
 * No-op if no record exists or uefi_runtime/VFS/BlackBox are not up.
 * Called from boot_desktop.c next to the hw_dump_write_file + boot
 * _reserved_blackbox_dump calls in Phase 3. */
void boot_version_blackbox_transcribe(void);

/* Name of the human-readable fault transcript in BlackBox. */
#define BOOT_VERSION_FAULT_BLACKBOX_FILE "boot-proto-fault.txt"

/* Fault-class name for logging. */
const char *boot_version_fault_class_name(uint32_t fault_class);

#endif /* KERNEL_BOOT_VERSION_H */
