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
#include "kernel/boot_version_constants.h"
#include "kernel/boot_info.h"   /* for struct boot_loader_identity */

struct boot_info_header;  /* forward decl -- full definition in kernel/boot_info.h */

enum boot_version_fault_class {
    BOOT_VERSION_OK                = BOOT_VERSION_FAULT_VAL_OK,
    BOOT_VERSION_FAULT_NULL_HDR    = BOOT_VERSION_FAULT_VAL_NULL_HDR,    /* header pointer NULL */
    BOOT_VERSION_FAULT_BAD_MAGIC   = BOOT_VERSION_FAULT_VAL_BAD_MAGIC,   /* header.magic mismatches BOOT_INFO_MAGIC */
    BOOT_VERSION_FAULT_BAD_VERSION = BOOT_VERSION_FAULT_VAL_BAD_VERSION, /* header.version != BOOT_INFO_VERSION */
    BOOT_VERSION_FAULT_BAD_SIZE    = BOOT_VERSION_FAULT_VAL_BAD_SIZE,    /* header.size != sizeof(struct boot_info) */
    BOOT_VERSION_FAULT_SEC_ROLLBACK = BOOT_VERSION_FAULT_VAL_SEC_ROLLBACK, /* anti-rollback refusal */
    BOOT_VERSION_FAULT_BAD_SHA     = BOOT_VERSION_FAULT_VAL_BAD_SHA,     /* .bootproto sha256 drift */
    BOOT_VERSION_FAULT_BAD_PARSE   = BOOT_VERSION_FAULT_VAL_BAD_PARSE,   /* .bootproto ELF parse failure;
                                                                          * raw parser error enum stored
                                                                          * in observed_loader_sec_ver. */
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
    uint32_t _reserved[3];             /* pad to 48 bytes -- v1 record ends here */

    /* v2 extension (Bootloader Build Identity): producer's build
     * identity captured at fault time. Allows kernel transcribe to
     * attribute the prior-boot fault to the loader that emitted it,
     * NOT to the (different) loader that emitted the next successful
     * handoff. Persisted alongside the v1 fields in the same NVRAM
     * variable; size discriminator (sz == 48 vs sz == 112) tells the
     * kernel which version it's reading.
     *
     * Back-compat: an OLD kernel (knows only size 48) reading a v2
     * record sees a size mismatch and clears it -- one-way upgrade
     * break, documented in the boot-protocol changelog. Forward-
     * compat: a NEW kernel reading a v1 record renders identity as
     * "unavailable (legacy fault record)" and proceeds. */
    struct boot_loader_identity loader_identity; /* +64 -> 112 bytes */
};

_Static_assert(sizeof(struct boot_version_fault) == 112,
    "boot_version_fault layout pinned at 112 bytes (v2: includes loader identity)");

#define BOOT_VERSION_FAULT_RECORD_SIZE_V1  48u
#define BOOT_VERSION_FAULT_RECORD_SIZE_V2  ((uint64_t)sizeof(struct boot_version_fault))

/* Per-field offset asserts. The bootloader has its own mirror struct
 * (struct bl_boot_version_fault in src/boot/uefi/bootx64.c) and writes
 * observed/expected_loader_sec_ver as the rollback payload; the kernel
 * consumer reads them at the same offsets. Pinning sizeof alone is
 * not enough -- a same-size field reorder would silently desynchronize
 * the rollback diagnostic. The bootloader mirror MUST replicate these
 * asserts byte-for-byte. */
_Static_assert(__builtin_offsetof(struct boot_version_fault, record_magic) == 0,
    "boot_version_fault.record_magic at offset 0");
_Static_assert(__builtin_offsetof(struct boot_version_fault, fault_class) == 4,
    "boot_version_fault.fault_class at offset 4");
_Static_assert(__builtin_offsetof(struct boot_version_fault, observed_loader_sec_ver) == 28,
    "boot_version_fault.observed_loader_sec_ver at offset 28 (rollback payload)");
_Static_assert(__builtin_offsetof(struct boot_version_fault, expected_loader_sec_ver) == 32,
    "boot_version_fault.expected_loader_sec_ver at offset 32 (rollback payload)");

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

/* Return the operator-response hint for a given fault class. Rollback
 * refusals and structural ABI drift have DIFFERENT operator responses:
 * rollback says "boot a newer signed kernel or clear the policy
 * variable"; ABI drift says "rebuild both halves". The hint string is
 * consumed by boot_version_render_fatal() on the serial fatal screen
 * AND by boot_version_blackbox_transcribe() in X:\Diag\boot-proto-fault.txt,
 * so both paths produce matching advice from one source of truth.
 * Returns a constant string; NEVER NULL. */
const char *boot_version_fault_operator_hint(uint32_t fault_class);

#endif /* KERNEL_BOOT_VERSION_H */
