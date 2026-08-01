/* ============================================================================
 * boot_info.h -- Parsed boot information passed to the kernel
 *
 * Field ownership (producer / consumer / first valid phase / lifetime / owning
 * roadmap / validation rule) lives in the canonical matrix at
 *   docs/boot/boot-info-fields.md
 * That doc is the single source of truth; this header keeps only the C-level
 * policy strictly required by the compiler (ABI header offsets, the static
 * asserts that pin count-field offsets, and the `struct boot_config` size
 * contract).
 *
 * Bootloader mirror of this struct lives in src/boot/uefi/boot_info_mirror.h
 * (same layout, UEFI UINT* types instead of uint*_t). Adding, removing, or
 * reordering a field requires updating BOTH headers in the same commit and
 * adding a matching F(...) line in tools/boot-info-manifest/dump-fields.inc
 * so the build-time ABI manifest catches the drift.
 *
 * Manifest: build/boot-info-abi.{kernel,mirror}.json is regenerated on every
 * `bash scripts/build.sh` and diffed by tools/boot-info-manifest/compare.sh;
 * any field / offset / size disagreement fails the build with the first
 * mismatching field named. Static asserts below remain the compile-time
 * first line of defense.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/boot_init.h"   /* boot_result_t */

/* Maximum number of memory map entries we store.
 * Real hardware (especially laptops with NVRAM, MMIO, etc.) can have 100+
 * descriptors.  256 is generous and still fits comfortably at 0x10000. */
#define BOOT_MMAP_MAX_ENTRIES 512

/* UEFI memory type constants (matches EFI_MEMORY_TYPE enum 0–14).
 * Defined here so kernel code can reference them without UEFI headers. */
#define UEFI_MMAP_RESERVED              0   /* EfiReservedMemoryType */
#define UEFI_MMAP_LOADER_CODE           1   /* EfiLoaderCode */
#define UEFI_MMAP_LOADER_DATA           2   /* EfiLoaderData */
#define UEFI_MMAP_BOOT_SERVICES_CODE    3   /* EfiBootServicesCode */
#define UEFI_MMAP_BOOT_SERVICES_DATA    4   /* EfiBootServicesData */
#define UEFI_MMAP_RUNTIME_CODE          5   /* EfiRuntimeServicesCode */
#define UEFI_MMAP_RUNTIME_DATA          6   /* EfiRuntimeServicesData */
#define UEFI_MMAP_CONVENTIONAL          7   /* EfiConventionalMemory */
#define UEFI_MMAP_UNUSABLE              8   /* EfiUnusableMemory */
#define UEFI_MMAP_ACPI_RECLAIM          9   /* EfiACPIReclaimMemory */
#define UEFI_MMAP_ACPI_NVS             10   /* EfiACPIMemoryNVS */
#define UEFI_MMAP_MMIO                 11   /* EfiMemoryMappedIO */
#define UEFI_MMAP_MMIO_PORT            12   /* EfiMemoryMappedIOPortSpace */
#define UEFI_MMAP_PAL_CODE             13   /* EfiPalCode */
#define UEFI_MMAP_PERSISTENT           14   /* EfiPersistentMemory */
/* Warm-kernel-update preserved memory (section 14). Used when
 * firmware does not expose EfiUnacceptedMemoryType (UEFI 2.10+) for
 * the staged region. Treated like UEFI_MMAP_RESERVED by the
 * free-memory handoff: never reclaimed, matched to a
 * BOOT_PAYLOAD_WARM_UPDATE_STATE descriptor by the consumer. */
#define BOOT_MMAP_WARM_UPDATE          15

/* ---- boot_info ABI header (S15) ----
 * Fixed at offset 0 of struct boot_info. The bootloader fills magic, version,
 * and size before kernel handoff. The kernel validates these before memcpy
 * to detect stale BOOTX64.EFI / kernel.exe mismatches.
 *
 * ABI rules:
 *   - Never reorder fields in boot_info_header.
 *   - Bump BOOT_INFO_VERSION when adding/removing/reordering fields in
 *     struct boot_info (not for adding fields to _reserved regions).
 *   - Always rebuild BOOTX64.EFI and kernel.exe together after a bump.
 *   - Bootloader writes sizeof(struct boot_info) into header.size at
 *     compile time -- a size mismatch means the structs diverged. */
/* MAGIC + VERSION guarded by #ifndef so a stale-ABI fixture build
 * can override via `-DBOOT_INFO_VERSION=N` or `-DBOOT_INFO_MAGIC=...`
 * on the command line without forking this header. All consumers
 * (kernel TUs, bootloader mirror, manifest dumpers) read these
 * macros at compile time, so the override flows transparently. */
#ifndef BOOT_INFO_MAGIC
#define BOOT_INFO_MAGIC    0x49504F53  /* "IPOS" (Impossible OS) */
#endif
/* Bootloader build identity (v13). 64-byte struct populated by the
 * UEFI bootloader from compile-time constants emitted by
 * tools/boot-info-manifest/gen-loader-identity.sh into
 * build/boot_loader_identity.h. Lives at the tail of struct boot_info
 * (back-compat: a stale kernel reading a fresh handoff still sees its
 * known prefix; a stale bootloader against a fresh kernel leaves the
 * field zero and the kernel detects "loader did not populate" via the
 * all-zero git_sha sentinel). */
struct __attribute__((packed)) boot_loader_identity {
    uint8_t  git_sha[20];        /* git rev-parse HEAD raw bytes */
    uint64_t build_unix_time;    /* git log -1 --format=%ct (LE) */
    char     build_label[24];    /* git describe --dirty --always (NUL-terminated) */
    uint8_t  _pad[12];           /* reserved zero */
};

_Static_assert(sizeof(struct boot_loader_identity) == 64,
    "boot_loader_identity must be exactly 64 bytes -- ABI contract");
_Static_assert(__builtin_offsetof(struct boot_loader_identity, git_sha) == 0,
    "boot_loader_identity.git_sha at offset 0");
_Static_assert(__builtin_offsetof(struct boot_loader_identity, build_unix_time) == 20,
    "boot_loader_identity.build_unix_time at offset 20");
_Static_assert(__builtin_offsetof(struct boot_loader_identity, build_label) == 28,
    "boot_loader_identity.build_label at offset 28");
_Static_assert(__builtin_offsetof(struct boot_loader_identity, _pad) == 52,
    "boot_loader_identity._pad at offset 52");

#ifndef BOOT_INFO_VERSION
/* v11 adds ESP integrity fields (esp_size_mb, esp_filesystem_type,
 * esp_type_guid_valid) populated by the UEFI bootloader's pre-load
 * sanity gate;
 * v10 added BOOT_FLAG_INVOKED_VIA_UKI;
 * v9 added flags + os_loader/required_security_version */
/* v20 adds policy audit surface: audit_degraded (1 if NVRAM sticky
 *     read failed or BlackBox publish path is unavailable), plus a
 *     6-byte snapshot of the ImpossibleOS-BootSticky NVRAM record
 *     consumed pre-policy (recovery_trigger, watchdog_rollback_request,
 *     last_outcome, audit_degraded_last_boot, last_event_code). The
 *     kernel's boot_audit_publish() composes a JSONL line from the
 *     selection block + this snapshot and appends to
 *     X:\Boot\history.jsonl, then acks consumed triggers via
 *     uefi_var_set. Bootloader is read-only on the sticky var.
 * v18 adds firmware trust-landscape surface (sbat_level, dbx_size,
 *     degraded_trust_flags) populated by uefi_secureboot_init() in the
 *     kernel-populated section. SBAT (Secure Boot Advanced Targeting,
 *     Microsoft revocation infrastructure) level string and dbx
 *     (forbidden signature database, UEFI 2.10 specification 32.4.2)
 *     presence are read post-EBS via gRT->GetVariable.
 *     degraded_trust_flags is a BOOT_DEGRADED_TRUST_* bitmask combining
 *     secure-boot state + SBAT/dbx absence + setup-mode signal.
 *     boot_decision degraded-trust rule warns (non-fatal) on each set
 *     bit; boot_device_populate_registry surfaces to
 *     HKLM\SYSTEM\Boot\Trust\*. v18 is the partial-ship subset of the
 *     boot-media artifact-signing/manifest-verification feature;
 *     manifest signature verification itself remains blocked on
 *     bootloader-side crypto vendor work tracked in
 *     todo/09-desktop-shell/TODO-07-cng-crypto.md (crypto primitives)
 *     and todo/15-installer-release/TODO-01-release-artifacts.md
 *     (release-side signing).
 * v16 adds local boot device path detail (boot_nvme_nsid, boot_nvme_eui64,
 *     boot_pci_device, boot_pci_function) populated by walking the boot
 *     device's UEFI device path for NVMe Messaging nodes (UEFI 2.10 spec
 *     10.3.4.21) and PCI Hardware nodes (UEFI 2.10 spec 10.3.2.1).
 *     Persisted in HKLM\SYSTEM\Boot\Device\ (NamespaceId, NamespaceEui64,
 *     PciDevice, PciFunction). Win11 surfaces this via MSFT_Disk.UniqueId
 *     + BusType; Linux surfaces it via /sys/class/nvme/nvmeX/nsid + sysfs
 *     PCI BDF.
 * v15 adds extended UEFI boot-variable capability surface (boot_current_attrs,
 *     boot_option_support, os_indications_supported, boot_description[64])
 *     populated by the UEFI bootloader from BootCurrent's Boot####.Attributes,
 *     the BootOptionSupport / OsIndicationsSupported globals, and the Boot####
 *     Description string. Read-only firmware capabilities for diagnostics +
 *     registry persistence under HKLM\SYSTEM\Boot\Device\.
 * v14 adds UKI-embedded payload addresses (uki_initrd_addr/_size,
 *     uki_recovery_addr/_size, uki_modules_addr/_size) populated by
 *     the UEFI bootloader after walking the LoadedImage's PE section
 *     table for .initrd / .recovery / .modules. Zero addr = section
 *     not present in the UKI; non-zero addr = kernel-physical address
 *     of a bootloader-allocated EfiLoaderData copy of the payload.
 *     Required for whole-chain Secure Boot signature coverage of
 *     initrd/recovery/module payloads.
 * v13 adds boot_loader_identity at the tail: bootloader build-identity
 *     attribution (git_sha + build_unix_time + label), populated by
 *     the UEFI bootloader from compile-time constants.
 * v12 added ESP integrity fields. */
/* v23 appends gop_handles[]/gop_handle_count at the tail: multi-GPU GOP handle
 *     enumeration (TODO-27 sec4). New fields (not a reserved-region carve), so
 *     the version bumps. Single-GPU + headless boots leave gop_handle_count 0;
 *     boot_info.fb stays the authoritative primary framebuffer. */
#define BOOT_INFO_VERSION  23
#endif

/* Producer-valid marker for the sec6 A/B slot-status snapshot (boot_info
 * .ab_status_valid). Set by a bootloader that publishes the snapshot; the
 * kernel renders detailed A/B diagnostics only when it matches. Distinguishes
 * a real zero-state from an older same-version bootloader that never wrote the
 * reserved-region snapshot bytes. Mirror keeps an identical define. */
#ifndef AB_STATUS_VALID_MAGIC
#define AB_STATUS_VALID_MAGIC  0xABu
#endif

/* True when the bootloader actually published the sec6 A/B slot-status
 * snapshot. The kernel VPD gates detailed slot/rollback rendering on this:
 * because the snapshot bytes were carved from a reserved region WITHOUT a
 * BOOT_INFO_VERSION bump, an older same-version (v22) bootloader sets
 * ab_meta_lba but leaves ab_status_valid zero -- rendering its zero-state as a
 * healthy boot would mask a real rollback. Pure so the skew gate is testable. */
static inline int ab_boot_status_published(uint64_t ab_meta_lba,
                                           uint8_t ab_status_valid)
{
    return ab_meta_lba != 0 && ab_status_valid == AB_STATUS_VALID_MAGIC;
}
/* v21 adds loader_vars_degraded + loader_vars_pad: bootloader sets the
 * degraded flag if any systemd-boot-compatible LoaderXxx UEFI variable
 * write failed (NVRAM quota, firmware refusal). Kernel surfaces this in
 * audit + diagnostics. */

/* Upper bound for pre-copy address validation: the UEFI bootloader
 * identity-maps [0, 4 GiB) with 2 MiB pages in setup_page_tables()
 * before jumping to the kernel.  Any handoff pointer above this limit
 * cannot be safely dereferenced during Phase 0 and must be rejected
 * before boot_info_validate_header() reads the magic/version/size. */
#define BOOT_INFO_EARLY_MAP_END  0x100000000ULL

struct boot_info_header {
    uint32_t magic;     /* must be BOOT_INFO_MAGIC */
    uint16_t version;   /* BOOT_INFO_VERSION */
    uint16_t size;      /* sizeof(struct boot_info) as seen by the bootloader */
};

/* S16: boot_info handoff validators.
 *
 * Split into address and header phases so pre-copy failure paths can
 * decide whether dereferencing the handoff pointer is safe before
 * logging observed header values.
 *
 * boot_info_validate_addr(p, size, max_addr) -- pure, no dereferences.
 *   Rejects NULL, addresses below 0x1000 (NULL page / BDA), misaligned
 *   pointers, sizes that cannot hold the header or exceed uint16_t, and
 *   any p + size range that wraps around or crosses max_addr.
 *   Early boot passes BOOT_INFO_EARLY_MAP_END; tests pass (uintptr_t)-1.
 *
 * boot_info_validate_header(hdr, kernel_struct_size) -- dereferences
 *   the header.  Caller MUST have confirmed the pointer is safe via
 *   boot_info_validate_addr() first.  Rejects bad magic, version not
 *   equal to BOOT_INFO_VERSION, or size != kernel_struct_size.
 *
 * boot_info_validate(p, kernel_struct_size) -- convenience for unit
 *   tests: runs boot_info_validate_addr() with max = (uintptr_t)-1 then
 *   boot_info_validate_header().  Production code in boot_phase0()
 *   calls the two phases separately so it can bound addresses to the
 *   4 GiB early identity map.
 *
 * All three functions return BOOT_OK on success, BOOT_FATAL on rejection.
 */
boot_result_t boot_info_validate_addr(const void *p,
                                      size_t kernel_struct_size,
                                      uintptr_t max_addr);

boot_result_t boot_info_validate_header(const struct boot_info_header *hdr,
                                        size_t kernel_struct_size);

boot_result_t boot_info_validate(const void *p,
                                 size_t kernel_struct_size);

/* payload descriptor validator error classes. Kept near the
 * validator prototype so callers and tests do not have to forward into
 * the struct boot_info region of the header. */
enum boot_payload_error {
    BOOT_PAYLOAD_ERR_OK                  = 0,
    BOOT_PAYLOAD_ERR_COUNT_OOR           = 1,  /* payload_count > BOOT_PAYLOAD_MAX */
    BOOT_PAYLOAD_ERR_PREFIX_VIOLATED     = 2,  /* occupied slot at index >= payload_count */
    BOOT_PAYLOAD_ERR_RANGE_WRAP          = 3,  /* phys_start + length overflows uint64_t */
    BOOT_PAYLOAD_ERR_OVERLAP_BOOT_INFO   = 4,  /* overlaps the struct boot_info handoff region */
    BOOT_PAYLOAD_ERR_OVERLAP_RT_MMAP     = 5,  /* overlaps a UEFI runtime memory region */
    BOOT_PAYLOAD_ERR_OVERLAP_USB_DMA     = 6,  /* overlaps usb_controller.dma_pages[] */
    BOOT_PAYLOAD_ERR_OVERLAP_FB          = 7,  /* overlaps the linear framebuffer */
    BOOT_PAYLOAD_ERR_ALIGNMENT           = 8,  /* alignment not a power of 2 */
    BOOT_PAYLOAD_ERR_UNKNOWN_REQUIRED    = 9,  /* unknown type with FLAG_REQUIRED set */
    BOOT_PAYLOAD_ERR_UNKNOWN_FLAGS       = 10, /* unknown flag bits with FLAG_REQUIRED set */
    BOOT_PAYLOAD_ERR_TOTAL_MISMATCH      = 11, /* payload_total_bytes != recomputed sum */
    BOOT_PAYLOAD_ERR_OVERFLOW_TRUNCATED  = 12, /* payload_overflow != 0 -- producer dropped payloads */
    BOOT_PAYLOAD_ERR_NONE_NOT_EMPTY      = 13, /* type=NONE but slot carries
                                                * data (any of flags,
                                                * phys_start, length,
                                                * alignment, checksum,
                                                * producer_id, _reserved
                                                * non-zero); ABI contract
                                                * requires NONE == fully
                                                * zeroed empty slot. */
    BOOT_PAYLOAD_ERR_DESCRIPTOR_OVERLAP   = 14, /* two occupied descriptors
                                                * inside the packed prefix
                                                * have overlapping
                                                * [phys_start, phys_start+length)
                                                * ranges. Caught even when
                                                * neither overlaps a retained
                                                * boot region (boot_info,
                                                * rt_mmap, USB DMA, fb), so
                                                * a malformed loader cannot
                                                * publish aliased payloads
                                                * that downstream consumers
                                                * would treat as distinct. */
    BOOT_PAYLOAD_ERR_MISSING_VALID_FLAG    = 15, /* occupied descriptor (type
                                                  * != NONE) is missing
                                                  * BOOT_PAYLOAD_FLAG_VALID.
                                                  * The flag is the explicit
                                                  * producer/consumer ABI
                                                  * gate; type != NONE alone
                                                  * is not enough -- without
                                                  * this enforcement a
                                                  * future or alternate
                                                  * producer that forgets
                                                  * the flag would still
                                                  * have its descriptor
                                                  * consumed, defeating the
                                                  * flag's purpose. */
};

/* Forward declaration so the validator prototype can reference
 * struct boot_info before its full definition appears below. */
struct boot_info;

/* Validate the typed payload descriptor array after the header copy is
 * complete and g_boot_info is populated. Runs every overlap, range,
 * packed-prefix, total-bytes, and unknown-required check documented in
 * the boot_payload_desc contract. On BOOT_FATAL the specific failure
 * class is written to *out_error (when non-NULL) and a LOG_ERROR klog
 * entry names the offending descriptor index + retained region. */
boot_result_t boot_payload_validate(const struct boot_info *info,
                                    enum boot_payload_error *out_error);

/* Enumerate typed payload descriptors. Returns a pointer to the `index`-th
 * descriptor in the packed prefix whose `type` matches, or NULL when:
 *   - info is NULL
 *   - type is BOOT_PAYLOAD_NONE (NONE is the empty-slot sentinel, never a
 *     legitimate lookup target)
 *   - payload_count is out of range
 *   - fewer than (index + 1) descriptors of `type` exist
 *
 * Callers use index=0 for "first of type". To count all of a type, scan
 * upward until the function returns NULL. The pointer is into the caller's
 * `info->payload_descriptors[]` and is valid for the lifetime of `info`
 * (i.e., until the kernel releases the boot_info region after Phase 3).
 * The validator (boot_payload_validate) must have already returned BOOT_OK
 * for `info` before calling this -- the lookup performs no re-validation. */
/* `type` is a uint32_t to avoid forward-declaring enum boot_payload_type
 * before the full enum is defined later in the header. Callers should
 * pass values from `enum boot_payload_type` directly -- they decay to
 * uint32_t exactly; the validator already stores type as uint32_t for
 * the same ABI-stability reason. */
/* Anti-rollback and security-version binding (v9).
 *
 * Windows 11 ships `OsLoaderSecurityVersion` in its Loader Parameter
 * Block; Linux uses shim+SBAT revocation vectors. Impossible OS binds
 * a monotonic counter in UEFI NVRAM: the bootloader reads
 * `IPOSRequiredSecVersion` early, the kernel ships with a build-time
 * `os_loader_security_version`, and the bootloader refuses to jump
 * when shipped < required. After a successful Phase 3 proof-of-life,
 * the kernel (gated by an explicit opt-in policy flag) raises the
 * NVRAM value via UEFI RT `SetVariable`. A stored-but-older
 * `kernel.exe` therefore cannot boot on a machine whose policy has
 * crossed a given version line.
 *
 * Write timing is the critical safety invariant: the NVRAM raise
 * happens AFTER `kernel_subsystem_ready(SUBSYS_BOOT_COMPLETE)` and
 * after the POST16_BOOT_OK marker lands; a pre-jump update is a
 * classic brick vector (kernel crashes early, counter advanced, old
 * signed kernel now refused, no recovery path).
 *
 * BOOT_FLAG_ROLLBACK_REFUSAL is set by the bootloader on the refusal
 * path; the kernel never sees it because the bootloader halts before
 * jumping. The bit exists for future crash-recorder readers that
 * want to reconstruct "what was the last halt reason" from the
 * boot_info image in low memory.
 */
#define BOOT_FLAG_ROLLBACK_REFUSAL    (1u << 0)  /* bootloader halted on security-version downgrade */
#define BOOT_FLAG_ROLLBACK_READ_FAILED (1u << 1)  /* bootloader halted on IPOSRequiredSecVersion read/validation */
/* outgoing kernel staged a warm-update handoff -- see warm-update section */
#define BOOT_FLAG_WARM_UPDATE         (1u << 2)
/* BOOT_FLAG_INVOKED_VIA_UKI (v10): set when the bootloader detected a
 * `.linux` PE section in its own LoadedImage and used the embedded
 * kernel instead of loading `\\kernel.exe` from the ESP filesystem.
 * Whole-chain Secure Boot signature semantics: when set, kernel +
 * cmdline + osrel were all covered by the firmware-verified PE
 * signature. When clear, only BOOTX64.EFI was signature-covered;
 * kernel.exe + boot.conf were per-file-signed via the split path.
 * Consumed by the kernel for attestation reporting and the boot-reason
 * enum (UKI-style unified signed boot artifact -- UAPI Group spec). */
#define BOOT_FLAG_INVOKED_VIA_UKI     (1u << 3)
#define BOOT_FLAG_MASK_KNOWN \
    (BOOT_FLAG_ROLLBACK_REFUSAL | BOOT_FLAG_ROLLBACK_READ_FAILED | \
     BOOT_FLAG_WARM_UPDATE | BOOT_FLAG_INVOKED_VIA_UKI)

#define BOOT_SECURITY_VERSION_MAX   0x7FFFFFFFu

enum boot_rollback_error {
    BOOT_ROLLBACK_ERR_OK                 = 0,
    BOOT_ROLLBACK_ERR_NULL_INFO          = 1,
    BOOT_ROLLBACK_ERR_UNKNOWN_FLAG       = 2,
    BOOT_ROLLBACK_ERR_VERSION_OOR        = 3,
    BOOT_ROLLBACK_ERR_RESERVED_NONZERO   = 4,  /* _rollback_pad must be zero */
};

boot_result_t boot_rollback_validate(const struct boot_info *info,
                                     enum boot_rollback_error *out_error);

int boot_rollback_should_raise(const struct boot_info *info,
                               int opt_in,
                               uint32_t *new_value);

/* Compositor-steady gate (anti-rollback raise timing). mark_steady()
 * latches the steady signal from the compositor first stable frame --
 * today this is the only steady source; a pre-frame hang
 * intentionally withholds the raise so the machine can still fall
 * back to the prior image. raise_if_steady() performs the one-shot
 * NVRAM raise of IPOSRequiredSecVersion when the opt-in policy is set
 * and shipped > required. The raise site used to fire in Phase 3
 * immediately after POST16_BOOT_OK, which could advance the rollback
 * floor on a boot that crashed during compositor init.
 * Test-only reset helper is declared but not exposed to release
 * callers; tests link against it via the test harness. */
void boot_rollback_mark_steady(void);
int  boot_rollback_is_steady(void);
int  boot_rollback_was_raised(void);
int  boot_rollback_raise_if_steady(void);
/* Schedule the raise asynchronously on sys_wq so the compositor
 * first-frame thread does not block on the UEFI Runtime Services
 * SetVariable call (10-100 ms NVRAM flash on real firmware). The
 * actual NVRAM write happens later in the worker thread.
 *
 * Precondition: caller must have invoked mark_steady() first. A call
 * before the steady signal is latched is a no-op (returns 0) and does
 * NOT consume the request slot, so a later post-steady caller still
 * owns the raise. This protects the public API from caller-ordering
 * mistakes that would otherwise permanently strand the rollback
 * floor for the boot.
 *
 * Idempotence is conditional on the eventual outcome:
 *  - On confirmed SUCCESS or permanent OPT-OUT, the request is
 *    latched (s_attempted=1, s_enqueued=1) and subsequent calls
 *    return 0 forever. This is the common-case "already serviced"
 *    no-op.
 *  - On TRANSIENT SetVariable failure, the implementation atomically
 *    rolls BOTH s_attempted and s_enqueued back to 0 under the
 *    state lock so a future caller can retry the raise. A later
 *    request_raise() therefore CAN return 1 again -- this is by
 *    design (Codex 2026-04-30 step-13 M1: blocking retry through
 *    the public API would silently strand the rollback floor on
 *    UEFI runtime hiccups).
 *
 * Fallback: if sys_wq is null OR the workqueue pool is exhausted,
 * runs boot_rollback_raise_if_steady() synchronously on the caller s
 * thread. Compositor first-frame is one-shot; silently dropping the
 * only raise attempt would violate the section test checkpoint.
 *
 * Returns 1 if work was enqueued OR the synchronous fallback ran.
 * Returns 0 in TWO distinct cases that callers MUST NOT collapse:
 *  - Pre-steady no-op (retryable): mark_steady has not fired yet;
 *    no slot was consumed and a later post-steady call can still
 *    own the raise.
 *  - Already-serviced no-op (terminal for this boot): the request
 *    is latched on success or permanent opt-out and subsequent
 *    calls stay 0 forever. */
int  boot_rollback_request_raise(void);
#ifdef KERNEL_TESTS
void boot_rollback_reset_for_test(void);
#endif

/* Warm-kernel-update handoff (section 14).
 *
 * An outgoing kernel that supports live update stages a memory region
 * and publishes a BOOT_PAYLOAD_WARM_UPDATE_STATE descriptor in the
 * handoff boot_info. The incoming kernel consumes the descriptor BEFORE
 * pmm_init() reclaims the region: reattaches the preserved memory,
 * inspects the continuation flags to determine which subsystem state
 * to restore, and either (a) continues as a warm update OR (b) falls
 * back to cold init if any required continuation bit is unrecognized.
 *
 * The memory region appears in boot_mmap[] as either
 * EfiUnacceptedMemoryType (UEFI 2.10+) when firmware supports the
 * discriminator or BOOT_MMAP_WARM_UPDATE (kernel-only value) when it
 * does not. Either way, the section-4 payload-overlap validator
 * treats the region as retained so PMM free-memory handoff does not
 * reclaim it.
 *
 * Continuation flags live in boot_payload_desc.flags (for the
 * warm-update descriptor specifically) above the standard
 * BOOT_PAYLOAD_FLAG_* bits. Each BOOT_WARM_UPDATE_CONT_* bit signals
 * a specific subsystem's pre-handoff quiesce state; the incoming
 * kernel must understand every set bit to safely reattach state.
 * Unknown bits -> fail-closed fallback to cold init (warm update NOT
 * applied on this boot; the outgoing kernel's policy should retry
 * cold on the next reboot).
 */
#define BOOT_WARM_UPDATE_CONT_PAGE_TABLES       (1u << 8)   /* outgoing kernel preserved its top-level page tables */
#define BOOT_WARM_UPDATE_CONT_SCHEDULER_QUIESCED (1u << 9)  /* scheduler drained: no runnable user threads in handoff */
#define BOOT_WARM_UPDATE_CONT_VFS_WRITEBACK      (1u << 10) /* VFS caches flushed to backing store before handoff */
#define BOOT_WARM_UPDATE_CONT_FD_TABLE           (1u << 11) /* file-descriptor table preserved verbatim */
#define BOOT_WARM_UPDATE_CONT_OBJECT_HANDLES     (1u << 12) /* Object Manager handle table preserved */
#define BOOT_WARM_UPDATE_CONT_HW_QUEUES          (1u << 13) /* device driver queues quiesced (NVMe/xHCI/VirtIO) */

/* Reserved bit range for warm-update continuation flags. Lives ABOVE
 * the standard BOOT_PAYLOAD_FLAG_* bits so the two mask families do
 * not overlap. */
#define BOOT_WARM_UPDATE_CONT_MASK_KNOWN                                   \
    (BOOT_WARM_UPDATE_CONT_PAGE_TABLES       |                             \
     BOOT_WARM_UPDATE_CONT_SCHEDULER_QUIESCED |                            \
     BOOT_WARM_UPDATE_CONT_VFS_WRITEBACK      |                            \
     BOOT_WARM_UPDATE_CONT_FD_TABLE           |                            \
     BOOT_WARM_UPDATE_CONT_OBJECT_HANDLES     |                            \
     BOOT_WARM_UPDATE_CONT_HW_QUEUES)

/* Decision returned by boot_warm_update_consume. WARM_ACCEPTED means
 * the incoming kernel has reattached the preserved region and the
 * caller should skip the cold-init path for the tagged subsystems.
 * COLD_FALLBACK means the descriptor was present but unusable;
 * caller must proceed with full cold init AND emit a diagnostic so
 * the operator knows the warm update did not take. */
enum boot_warm_update_decision {
    BOOT_WARM_UPDATE_COLD_FALLBACK = 0,  /* descriptor missing, malformed, or carried unknown flags */
    BOOT_WARM_UPDATE_ACCEPTED      = 1,  /* descriptor accepted; preserved region reattached */
};

/* Error classes for the structured COLD_FALLBACK rejection. */
enum boot_warm_update_error {
    BOOT_WARM_UPDATE_ERR_OK                = 0,
    BOOT_WARM_UPDATE_ERR_NULL_DESC         = 1,  /* desc pointer is NULL (no warm-update descriptor at all) */
    BOOT_WARM_UPDATE_ERR_WRONG_TYPE        = 2,  /* desc.type != BOOT_PAYLOAD_WARM_UPDATE_STATE */
    BOOT_WARM_UPDATE_ERR_UNKNOWN_CONT_FLAG = 3,  /* desc.flags has CONT_* bits outside MASK_KNOWN */
    BOOT_WARM_UPDATE_ERR_UNALIGNED         = 4,  /* phys_start not page-aligned (4K) */
    BOOT_WARM_UPDATE_ERR_EMPTY             = 5,  /* length == 0 */
    BOOT_WARM_UPDATE_ERR_MISSING_FLAGS     = 6,  /* desc.flags missing required BOOT_PAYLOAD_FLAG_VALID|RESERVED */
};

/* Forward declare boot_payload_desc so the prototype resolves --
 * the full struct is defined further down in this header. */
struct boot_payload_desc;

/* Validate + consume a warm-update descriptor. Returns
 * BOOT_WARM_UPDATE_ACCEPTED when the descriptor is well-formed and all
 * continuation bits are recognized; callers can proceed with warm
 * update. Returns BOOT_WARM_UPDATE_COLD_FALLBACK on any issue; the
 * specific reason is written to *out_error (when non-NULL).
 *
 * Fail-closed: on ANY rejection the caller MUST NOT partially
 * reattach -- the outgoing kernel's handoff is treated as absent and
 * cold init proceeds. A partial reattach would leave the kernel with
 * unknown state bits unreconciled, the exact failure mode this ABI
 * prevents. */
enum boot_warm_update_decision
boot_warm_update_consume(const struct boot_payload_desc *desc,
                         enum boot_warm_update_error *out_error);

/* Human-readable name for a continuation flag (single set bit).
 * Returns "reserved" for bits outside MASK_KNOWN. */
const char *boot_warm_update_cont_name(uint32_t flag_bit);

/* Boot-path provenance and decision record (v8).
 *
 * Media role, recovery, network boot, and resume each carry their own
 * details; this record is the one shared answer to "what path was
 * selected and why". Three pieces:
 *
 *   boot_path  -- enum boot_path_type: which flow actually ran
 *                 (cold boot, installer, recovery, network, resume,
 *                 fast startup, diagnostic).
 *   boot_reason -- enum boot_reason_code: the policy reason the
 *                  loader selected that path (user selected BootNext,
 *                  rollback triggered, resume invalidated, network
 *                  marked insecure, manifest/measured-boot failure,
 *                  recovery trigger asserted, fast-startup hibernation
 *                  image validated, diagnostic mode requested).
 *   boot_source_flags -- BOOT_SOURCE_FLAG_* bitmask: the inputs that
 *                        drove the decision (BootCurrent mismatch,
 *                        BootNext set, media removable/present,
 *                        rollback + recovery + resume-invalidation
 *                        triggers, network insecure, manifest /
 *                        measured-boot status). **Closed** mask:
 *                        every bit in BOOT_SOURCE_FLAG_MASK_KNOWN
 *                        maps to a kernel-side policy and the
 *                        validator hard-rejects bits outside it.
 *                        Unlike caps_present / caps_degraded there
 *                        is NO forward-compat tolerance -- adding a
 *                        new flag requires a BOOT_INFO_VERSION bump
 *                        AND a kernel-side interpreter for the new
 *                        bit. This is the single "ABI footgun"
 *                        difference between decision flags and
 *                        capability bits; producers that need a
 *                        future decision input must coordinate the
 *                        version bump with the kernel first.
 *   boot_fallback_depth -- how many fallbacks were traversed before
 *                          the selected path was reached. 0 = primary
 *                          choice took; N > 0 = Nth fallback (capped
 *                          at BOOT_FALLBACK_DEPTH_MAX by the
 *                          validator).
 *
 * The record pairs with the per-path descriptors produced by
 * neighboring roadmap sections (media role, recovery image, network
 * provenance, hibernation resume); consumers read this record FIRST
 * to know which descriptor to trust. The kernel exports the record to
 * HKLM\SYSTEM\Boot\Decision and to BlackBox so recovery, attestation,
 * and rollback logic can explain why the current boot chose what it
 * chose.
 */
/* Value 0 in both enums is the UNSET sentinel: a BSS-zero boot_info
 * from a producer that never populated the boot-decision fields
 * (Multiboot2 adapter pre-population wiring, alternate firmware that
 * doesn't know about v9, etc.) lands at UNSET in both fields and the
 * validator rejects via Rule 1/2 instead of certifying the record as
 * a phantom NORMAL boot. (Codex 2026-04-30 capability-negotiation
 * adversarial finding: the previous v8 layout had
 * BOOT_PATH_NORMAL = BOOT_REASON_NORMAL = 0, so multiboot2_parse.c
 * which writes neither field passed validation as if it were a real
 * cold boot.) Bumped to v9. */
enum boot_path_type {
    BOOT_PATH_UNSET        = 0,  /* sentinel: producer must overwrite */
    BOOT_PATH_NORMAL       = 1,  /* normal cold boot from primary boot device */
    BOOT_PATH_INSTALLER    = 2,  /* installer image handoff */
    BOOT_PATH_RECOVERY     = 3,  /* recovery partition / Windows RE equivalent */
    BOOT_PATH_NETWORK      = 4,  /* PXE/HTTP boot */
    BOOT_PATH_RESUME       = 5,  /* S4 hibernation resume (validated) */
    BOOT_PATH_FAST_STARTUP = 6,  /* Windows-style fast startup (hybrid boot) */
    BOOT_PATH_DIAGNOSTIC   = 7,  /* operator-triggered diagnostic mode */
};
#define BOOT_PATH_TYPE_MAX  BOOT_PATH_DIAGNOSTIC

enum boot_reason_code {
    BOOT_REASON_UNSET               = 0,  /* sentinel: producer must overwrite */
    BOOT_REASON_NORMAL              = 1,  /* primary path, no policy trigger */
    BOOT_REASON_USER_SELECTED       = 2,  /* BootNext or operator picked this entry */
    BOOT_REASON_ROLLBACK            = 3,  /* rollback after failed previous boot */
    BOOT_REASON_RESUME_VALIDATED    = 4,  /* hibernation image passed validation */
    BOOT_REASON_RESUME_INVALIDATED  = 5,  /* hibernation image rejected; fell back */
    BOOT_REASON_NETWORK_INSECURE    = 6,  /* network boot used but flagged insecure */
    BOOT_REASON_MANIFEST_FAILURE    = 7,  /* image manifest check failed */
    BOOT_REASON_MEASURED_BOOT_FAIL  = 8,  /* measured-boot/TPM check failed */
    BOOT_REASON_RECOVERY_TRIGGER    = 9,  /* recovery trigger asserted by firmware or operator */
    BOOT_REASON_FAST_STARTUP_HIT    = 10, /* fast-startup image present + valid */
    BOOT_REASON_DIAGNOSTIC_REQUEST  = 11, /* operator requested diagnostic flow */
    BOOT_REASON_FALLBACK            = 12, /* loader exhausted primary + chose fallback */
    BOOT_REASON_MEDIA_ROLE_MARKER   = 13, /* media-role marker on ESP / BlackBox selected this path (v17) */
};
#define BOOT_REASON_CODE_MAX  BOOT_REASON_MEDIA_ROLE_MARKER

/* Media role detected from /IPOS/role.txt on the ESP and on the BlackBox
 * service partition (v17). Read by the UEFI bootloader BEFORE kernel
 * load: ESP marker is authoritative; BlackBox marker is cross-checked
 * against ESP. Mismatch -> fall back to BOOT_MEDIA_ROLE_NORMAL with
 * boot_media_role_mismatch=1. Absent BlackBox marker is NOT a mismatch
 * (single-partition media is legal -- e.g. an ISO with only the ESP).
 *
 * Coupling to boot_path/boot_reason: when media role is installer /
 * recovery / diagnostics, the bootloader sets boot_path to the
 * matching enum and boot_reason = BOOT_REASON_MEDIA_ROLE_MARKER so
 * Registry/recovery/attestation consumers see a coherent decision
 * record. live and manufacturing roles run as boot_path=NORMAL since
 * their handoff is a normal cold boot, just flagged by the medium
 * type for post-boot policy. */
enum boot_media_role {
    BOOT_MEDIA_ROLE_UNSET         = 0,  /* sentinel: producer must overwrite */
    BOOT_MEDIA_ROLE_NORMAL        = 1,  /* default; role.txt absent or unrecognized */
    BOOT_MEDIA_ROLE_INSTALLER     = 2,  /* installer media (Windows-style setup-on-USB) */
    BOOT_MEDIA_ROLE_LIVE          = 3,  /* live boot media (no install, run from medium) */
    BOOT_MEDIA_ROLE_RECOVERY      = 4,  /* recovery media (Windows RE / dracut rescue equivalent) */
    BOOT_MEDIA_ROLE_MANUFACTURING = 5,  /* factory provisioning / pre-imaging tooling */
    BOOT_MEDIA_ROLE_DIAGNOSTICS   = 6,  /* operator-triggered diagnostics medium */
};
#define BOOT_MEDIA_ROLE_MAX  BOOT_MEDIA_ROLE_DIAGNOSTICS

/* Firmware trust-landscape signals (v18). Computed by
 * uefi_secureboot_init() and surfaced via g_boot_info.degraded_trust
 * _flags. boot_decision degraded-trust rule emits one [WARN] klog
 * line per set bit; non-fatal -- the signals are advisory. Each bit
 * means the firmware view of trust is degraded relative to a
 * production deployment, NOT that the boot is unsafe.
 *
 * SECURE_BOOT_OFF: SecureBoot variable read returned 0 (firmware
 *   reports Secure Boot disabled). User-disabled or system did not
 *   enroll PK/KEK.
 * SECURE_BOOT_UNREADABLE: SecureBoot variable read failed (firmware
 *   does not expose the variable, or RT services unavailable).
 *   Distinct from OFF because it reflects "we cannot tell" rather
 *   than "user disabled".
 * SETUP_MODE: SetupMode variable == 1, meaning PK is not enrolled
 *   and the firmware will accept any new PK/KEK without
 *   verification. Implies a manufacturing/factory state.
 * SBAT_ABSENT: SbatLevel global variable not present. Microsoft
 *   shim-derived bootloaders publish this for revocation tracking;
 *   absence means we cannot assert a SBAT level baseline.
 * DBX_ABSENT: dbx (forbidden signature DB) variable not present
 *   under EFI_IMAGE_SECURITY_DATABASE_GUID. Most production firmware
 *   ships a non-empty dbx; absence means revocation infrastructure
 *   is unconfigured.
 *
 * BOOT_DEGRADED_TRUST_MASK_KNOWN is a CLOSED mask -- adding a bit
 * requires a BOOT_INFO_VERSION bump. */
#define BOOT_DEGRADED_TRUST_SECURE_BOOT_OFF        (1u << 0)
#define BOOT_DEGRADED_TRUST_SECURE_BOOT_UNREADABLE (1u << 1)
#define BOOT_DEGRADED_TRUST_SETUP_MODE             (1u << 2)
#define BOOT_DEGRADED_TRUST_SBAT_ABSENT            (1u << 3)
#define BOOT_DEGRADED_TRUST_DBX_ABSENT             (1u << 4)

#define BOOT_DEGRADED_TRUST_MASK_KNOWN                  \
    (BOOT_DEGRADED_TRUST_SECURE_BOOT_OFF        |       \
     BOOT_DEGRADED_TRUST_SECURE_BOOT_UNREADABLE |       \
     BOOT_DEGRADED_TRUST_SETUP_MODE             |       \
     BOOT_DEGRADED_TRUST_SBAT_ABSENT            |       \
     BOOT_DEGRADED_TRUST_DBX_ABSENT)

/* Pretty-printer for one BOOT_DEGRADED_TRUST_* bit value. Returns a
 * pointer to a static lower-case ASCII string; "unknown" for any
 * value outside MASK_KNOWN. Used by boot_decision degraded-trust
 * rule and Registry populator. */
const char *boot_degraded_trust_bit_name(uint32_t bit);

/* BOOT_SOURCE_FLAG_* bitmask bits -- inputs that drove the decision.
 * BOOT_SOURCE_FLAG_MASK_KNOWN is a CLOSED mask: validator hard-rejects
 * any bit outside it. Adding a new flag requires a BOOT_INFO_VERSION
 * bump AND a matching kernel-side interpreter -- there is NO
 * forward-compat tolerance here (unlike caps_present / caps_degraded).
 * See the v8 comment block above for the full contract and the
 * rationale for the stricter stance.  */
#define BOOT_SOURCE_FLAG_BOOT_NEXT_SET            (1u << 0)  /* BootNext variable was populated */
#define BOOT_SOURCE_FLAG_BOOT_CURRENT_MISMATCH    (1u << 1)  /* BootCurrent != expected primary entry */
#define BOOT_SOURCE_FLAG_MEDIA_REMOVABLE          (1u << 2)  /* boot media flagged removable */
#define BOOT_SOURCE_FLAG_MEDIA_PRESENT            (1u << 3)  /* boot media actually present (BlockIO confirmed) */
#define BOOT_SOURCE_FLAG_ROLLBACK_TRIGGERED       (1u << 4)  /* rollback counter asserted */
#define BOOT_SOURCE_FLAG_RECOVERY_TRIGGERED       (1u << 5)  /* recovery trigger asserted */
#define BOOT_SOURCE_FLAG_RESUME_INVALIDATED       (1u << 6)  /* hibernation image invalidated */
#define BOOT_SOURCE_FLAG_NETWORK_INSECURE         (1u << 7)  /* network boot with insecure-channel flag */
#define BOOT_SOURCE_FLAG_MANIFEST_FAILED          (1u << 8)  /* image manifest check failed */
#define BOOT_SOURCE_FLAG_MEASURED_BOOT_FAILED     (1u << 9)  /* measured-boot/TPM check failed */

#define BOOT_SOURCE_FLAG_MASK_KNOWN                                   \
    (BOOT_SOURCE_FLAG_BOOT_NEXT_SET             |                     \
     BOOT_SOURCE_FLAG_BOOT_CURRENT_MISMATCH     |                     \
     BOOT_SOURCE_FLAG_MEDIA_REMOVABLE           |                     \
     BOOT_SOURCE_FLAG_MEDIA_PRESENT             |                     \
     BOOT_SOURCE_FLAG_ROLLBACK_TRIGGERED        |                     \
     BOOT_SOURCE_FLAG_RECOVERY_TRIGGERED        |                     \
     BOOT_SOURCE_FLAG_RESUME_INVALIDATED        |                     \
     BOOT_SOURCE_FLAG_NETWORK_INSECURE          |                     \
     BOOT_SOURCE_FLAG_MANIFEST_FAILED           |                     \
     BOOT_SOURCE_FLAG_MEASURED_BOOT_FAILED)

/* Upper bound on the fallback chain depth we consider legal. In
 * practice loaders should not burn through more than a handful of
 * fallbacks before halting; a value beyond this is a producer bug or a
 * storage-loop detection miss. */
#define BOOT_FALLBACK_DEPTH_MAX  16

enum boot_decision_error {
    BOOT_DECISION_ERR_OK                 = 0,
    BOOT_DECISION_ERR_NULL_INFO          = 1,  /* info pointer is NULL */
    BOOT_DECISION_ERR_BAD_PATH           = 2,  /* boot_path > BOOT_PATH_TYPE_MAX */
    BOOT_DECISION_ERR_BAD_REASON         = 3,  /* boot_reason > BOOT_REASON_CODE_MAX */
    BOOT_DECISION_ERR_UNKNOWN_FLAG       = 4,  /* boot_source_flags has bits outside KNOWN_MASK */
    BOOT_DECISION_ERR_FALLBACK_OOR       = 5,  /* boot_fallback_depth > BOOT_FALLBACK_DEPTH_MAX */
    BOOT_DECISION_ERR_REASON_PATH        = 6,  /* reason not allowed with current path (R5) */
    BOOT_DECISION_ERR_FALLBACK_REASON    = 7,  /* fallback_depth>0 but reason not fallback-class (R6) */
    BOOT_DECISION_ERR_REASON_FLAG        = 8,  /* trigger-reason without matching flag (R7) */
    BOOT_DECISION_ERR_BAD_SELECTION_REASON = 9,  /* selection_reason UNSET or > REASON_MAX (v19) */
    BOOT_DECISION_ERR_BAD_SELECTED_ID    = 10, /* selected_entry_id not NUL-terminated within 64 bytes (v19) */
    BOOT_DECISION_ERR_BAD_REJECTED_ENTRY = 11, /* rejected_entries integrity violation (v19) */
};

/* Validate decision record fields.  Runs in Phase 0 after
 * boot_caps_validate() succeeds.  On BOOT_FATAL the specific failure
 * is written to *out_error (when non-NULL); a LOG_ERROR line names the
 * offending field before boot_halt() fires from the caller.  Unknown
 * flag bits in boot_source_flags trigger BAD_FLAG when they are ALSO
 * outside BOOT_SOURCE_FLAG_MASK_KNOWN -- the unknown-tolerance rule
 * from capability negotiation does NOT apply here, because every
 * decision input maps to a documented policy and a producer cannot
 * assert a brand-new flag without kernel support. Widening the mask
 * requires a BOOT_INFO_VERSION bump. */
boot_result_t boot_decision_validate(const struct boot_info *info,
                                     enum boot_decision_error *out_error);

/* Human-readable name for the decision record enums. Returns a short
 * stable string ("normal" / "recovery" / etc.); unknown values return
 * "invalid". */
const char *boot_path_name(uint32_t path);
const char *boot_reason_name(uint32_t reason);
const char *boot_media_role_name(uint32_t role);

/* Capability negotiation and degraded-feature flags.
 *
 * Boot_info carries three capability words that let the loader and
 * kernel distinguish optional data from required data, and let
 * alternate boot adapters (Multiboot2, PXE/HTTP, future Secure Launch)
 * explicitly declare what they could NOT provide instead of silently
 * zeroing fields. Policy, in priority order:
 *
 *   1. `caps_required` is a bitmask of features the loader asserts the
 *      kernel MUST support. The kernel checks each set bit against
 *      BOOT_CAP_MASK_KNOWN. If any required bit is not in the known
 *      set, boot halts BEFORE Phase 0 consumes unsupported data. This
 *      is the "stale kernel on newer loader" failure gate.
 *   2. `caps_present` is a bitmask of features the loader ACTUALLY
 *      populated. Bits here mean the companion fields (payload
 *      descriptors, runtime services pointer, TPM event log, etc.)
 *      are valid and non-zero. Unknown bits are ignored (forward
 *      compatibility): older kernels can safely boot against newer
 *      loaders.
 *   3. `caps_degraded` is a bitmask of KNOWN capabilities the loader
 *      could NOT provide. Alternate boot adapters (Multiboot2, PXE,
 *      etc.) MUST set these bits for every feature they skipped, so
 *      the kernel's per-subsystem init can log a specific
 *      `capability degraded: <name>` reason instead of guessing from
 *      zero-valued fields.
 *
 * The invariants (validated by boot_caps_validate):
 *   - `caps_required & ~BOOT_CAP_MASK_KNOWN == 0` (no unknown required
 *     bits)
 *   - `caps_required & caps_degraded == 0` (required ^ degraded is a
 *     producer bug: "required but not provided" is a hard fail)
 *   - `caps_present & caps_degraded == 0` (same bit cannot be both
 *     present and degraded; contradictory)
 *   - Unknown bits in `caps_present` or `caps_degraded` are SILENTLY
 *     IGNORED (forward compatibility; unknown-optional policy).
 */
/* Capability bit list -- the SOLE source of truth for known BOOT_CAP_*
 * names AND their bit positions. The X-macro generates:
 *   - the BOOT_CAP_* enum constants (mask values used by every consumer)
 *   - BOOT_CAP_MASK_KNOWN (the "known" universe the validator enforces)
 *   - the bit-name switch arms in boot_caps.c
 *   - the table-driven coverage test in test_boot_caps.c
 *
 * Adding a new capability is one edit: add an `X(NAME, bit)` row here
 * and a switch case in boot_caps_bit_name() (a missing case surfaces
 * immediately via the table-driven test). There is no separate
 * `#define BOOT_CAP_NAME` line to forget -- the enum constant is
 * generated from this list. (Codex 2026-04-30 re-adversarial Round 2
 * finding: parallel #define + list scheme still permits drift if a
 * developer adds a #define without updating the list.)
 *
 * Type note: enum constants are `int` per C99; values up to (1u<<31)
 * fit. The 9 bits in use today are well under that. Future bits
 * beyond 30 will need either `static const uint64_t` (loses
 * case-label usability) or a `1ull << bit` cast at use site. The
 * validator and consumers always widen to uint64_t at use time
 * (`info->caps_present & BOOT_CAP_FOO`). */
#define BOOT_CAP_LIST(X) \
    X(PAYLOAD_DESCRIPTORS,  0)  /* typed payload descriptor array populated + validated */ \
    X(RUNTIME_SERVICES,     1)  /* uefi_runtime_services + uefi_rt_available set */         \
    X(SECURE_BOOT_STATE,    2)  /* secure_boot_enabled reflects firmware state */           \
    X(TPM_EVENT_LOG,        3)  /* tpm_event_log populated from EFI_TCG2_PROTOCOL */        \
    X(USB_HANDOVER,         4)  /* usb_controller.active + DMA state handed off */          \
    X(MEDIA_ROLE,           5)  /* boot_device classification + removable-media flags */    \
    X(NETWORK_PROVENANCE,   6)  /* reserved for network-boot provenance */                  \
    X(RESUME_METADATA,      7)  /* reserved for hibernation/resume handoff */               \
    X(ALT_PROTOCOL_ADAPTER, 8)  /* 1 = non-native-UEFI adapter (Multiboot2/etc.) */

/* Generate the BOOT_CAP_* mask constants from BOOT_CAP_LIST. Values
 * fit in `int` for all current entries; widening to uint64_t at use
 * time is implicit. */
enum {
#define BOOT_CAP__ENUM_ENT(name, bit) BOOT_CAP_##name = (1u << (bit)),
    BOOT_CAP_LIST(BOOT_CAP__ENUM_ENT)
#undef BOOT_CAP__ENUM_ENT
};

/* MASK_KNOWN derived from the list -- by construction cannot drift. */
#define BOOT_CAP__OR_ENTRY(name, bit) | (1u << (bit))
#define BOOT_CAP_MASK_KNOWN ((uint64_t)(0u BOOT_CAP_LIST(BOOT_CAP__OR_ENTRY)))

enum boot_caps_error {
    BOOT_CAPS_ERR_OK                 = 0,
    BOOT_CAPS_ERR_NULL_INFO          = 1,  /* info pointer is NULL */
    BOOT_CAPS_ERR_UNKNOWN_REQUIRED   = 2,  /* caps_required has bits outside MASK_KNOWN */
    BOOT_CAPS_ERR_REQUIRED_DEGRADED  = 3,  /* required bit is also set in caps_degraded */
    BOOT_CAPS_ERR_PRESENT_DEGRADED   = 4,  /* caps_present & caps_degraded != 0 */
    BOOT_CAPS_ERR_UNCLASSIFIED_KNOWN = 5,  /* a known bit is in NEITHER caps_present nor caps_degraded */
};

/* Validate capability words. On BOOT_FATAL the specific failure is
 * written to *out_error (when non-NULL) and a LOG_FATAL line names
 * the offending bitmask delta + classification. Called from Phase 0
 * AFTER boot_payload_validate succeeds and BEFORE any subsystem
 * consumes a capability-gated field. */
boot_result_t boot_caps_validate(const struct boot_info *info,
                                 enum boot_caps_error *out_error);

/* Human-readable name for a single BOOT_CAP_* bit. Returns
 * "reserved(0xNN)" for bits outside BOOT_CAP_MASK_KNOWN. */
const char *boot_caps_bit_name(uint64_t cap_bit);

/* Authoritative gate for kernel consumers. Returns non-zero when every
 * bit in `bits` is set in g_boot_info.caps_present, zero otherwise.
 * Consumers call this as a single-line gate:
 *
 *     if (!boot_caps_require(BOOT_CAP_TPM_EVENT_LOG)) return;
 *     ... parse event log ...
 *
 * The check lives here (not inline at every call site) so the source
 * of truth for "capability available" is boot_caps_validate's
 * classification, not scattered reads of legacy companion fields. A
 * loader that reports a capability as degraded via caps_degraded will
 * cause this helper to return zero even when the companion field is
 * non-empty; this is the fail-closed semantic that prevents consumers
 * from trusting legacy companion fields when the negotiated state
 * disagrees. */
int boot_caps_require(uint64_t bits);

/* Post-handoff kernel refinement: set one or more bits in
 * g_boot_info.caps_present after a kernel-side probe confirms the
 * capability is actually available. Used for capabilities whose
 * truth is only knowable after kernel init (e.g.
 * BOOT_CAP_SECURE_BOOT_STATE -- the bootloader unconditionally
 * degrades it, the kernel promotes it after reading the UEFI RT
 * variable). Monotonic: only sets bits, never clears. Must not be
 * called before boot_caps_validate(). */
void boot_caps_mark_present(uint64_t bits);

const struct boot_payload_desc *
boot_payload_find(const struct boot_info *info,
                  uint32_t type,
                  uint32_t index);

/* A single memory region from the bootloader */
struct boot_mmap_entry {
    uint64_t base_addr;
    uint64_t length;
    uint32_t type;              /* simplified: 1=available, 2=reserved, 3=ACPI, 4=NVS, 5=bad */
    uint32_t uefi_memory_type;  /* original EFI_MEMORY_TYPE (UEFI_MMAP_* constants above) */
    uint64_t attribute;         /* UEFI memory attribute flags (EFI_MEMORY_RUNTIME, etc.) */
};

/* --- UEFI Configuration Table ---
 * Copied from EFI_SYSTEM_TABLE.ConfigurationTable[] before ExitBootServices.
 * Each entry is a {GUID, VendorTable} pair pointing to platform data. */
#define BOOT_CONFIG_TABLE_MAX 32

struct boot_uefi_guid {
    uint32_t data1;
    uint16_t data2;
    uint16_t data3;
    uint8_t  data4[8];
};

struct boot_uefi_config_entry {
    struct boot_uefi_guid guid;
    uintptr_t             table_addr;   /* physical address of vendor table */
};

/* Well-known Configuration Table GUIDs (for kernel-side lookup) */
#define UEFI_GUID_ACPI_20 \
    ((struct boot_uefi_guid){ 0x8868e871, 0xe4f1, 0x11d3, \
        { 0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81 } })
#define UEFI_GUID_ACPI_10 \
    ((struct boot_uefi_guid){ 0xeb9d2d30, 0x2d88, 0x11d3, \
        { 0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d } })
#define UEFI_GUID_SMBIOS3 \
    ((struct boot_uefi_guid){ 0xf2fd1544, 0x9794, 0x4a2c, \
        { 0x99, 0x2e, 0xe5, 0xbb, 0xcf, 0x20, 0xe3, 0x94 } })
#define UEFI_GUID_SMBIOS \
    ((struct boot_uefi_guid){ 0xeb9d2d31, 0x2d88, 0x11d3, \
        { 0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d } })
#define UEFI_GUID_MEM_ATTR \
    ((struct boot_uefi_guid){ 0xdcfa911d, 0x26eb, 0x469f, \
        { 0xa2, 0x20, 0x38, 0xb7, 0xdc, 0x46, 0x12, 0x20 } })
#define UEFI_GUID_RT_PROPS \
    ((struct boot_uefi_guid){ 0xeb66918a, 0x7eef, 0x402a, \
        { 0x84, 0x2e, 0x93, 0x1d, 0x21, 0xc3, 0x8a, 0xe9 } })
#define UEFI_GUID_CONFORMANCE \
    ((struct boot_uefi_guid){ 0x36122546, 0xf7e7, 0x4c8f, \
        { 0xbd, 0x9b, 0xeb, 0x85, 0x25, 0xb5, 0x0c, 0x0b } })
#define UEFI_GUID_DTB \
    ((struct boot_uefi_guid){ 0xb1b621d5, 0xf19c, 0x41a5, \
        { 0x83, 0x0b, 0xd9, 0x15, 0x2c, 0x69, 0xaa, 0xe0 } })
#define UEFI_GUID_FPDT \
    ((struct boot_uefi_guid){ 0x564b1aaa, 0xafe3, 0x4b6c, \
        { 0x83, 0xa9, 0x27, 0x00, 0x80, 0x50, 0x01, 0x00 } })

/* UEFI memory attribute flag -- marks regions that survive ExitBootServices */
#define UEFI_MEMORY_ATTR_RUNTIME  0x8000000000000000ULL

/* Runtime memory region descriptor (for SetVirtualAddressMap) */
#define BOOT_RT_MMAP_MAX 64

struct boot_rt_mem_entry {
    uint64_t phys_addr;   /* physical start address */
    uint64_t num_pages;   /* number of 4 KiB pages */
    uint64_t attribute;   /* EFI memory attributes (cache type + EFI_MEMORY_RUNTIME) */
    uint32_t type;        /* EFI_MEMORY_TYPE (UEFI_MMAP_RUNTIME_CODE or _DATA) */
    uint32_t reserved;    /* alignment padding */
};

/* UEFI Runtime Service function pointers -- copied individually from
 * EFI_RUNTIME_SERVICES before ExitBootServices.  Stored as uintptr_t
 * because the bootloader (PE/COFF ms_abi) and kernel (ELF sysv) use
 * different calling conventions; the kernel casts at use time. */
struct boot_uefi_runtime {
    uintptr_t get_time;                   /* EFI_GET_TIME */
    uintptr_t set_time;                   /* EFI_SET_TIME */
    uintptr_t get_variable;               /* EFI_GET_VARIABLE */
    uintptr_t set_variable;               /* EFI_SET_VARIABLE */
    uintptr_t get_next_variable_name;     /* EFI_GET_NEXT_VARIABLE_NAME */
    uintptr_t reset_system;               /* EFI_RESET_SYSTEM */
    uintptr_t update_capsule;             /* EFI_UPDATE_CAPSULE */
    uintptr_t query_capsule_capabilities; /* EFI_QUERY_CAPSULE_CAPABILITIES */
    uintptr_t query_variable_info;        /* EFI_QUERY_VARIABLE_INFO */
    uintptr_t get_wakeup_time;            /* EFI_GET_WAKEUP_TIME */
    uintptr_t set_wakeup_time;            /* EFI_SET_WAKEUP_TIME */
    uint8_t   svam_called;                /* 1 if bootloader called SetVirtualAddressMap */
    uint8_t   pad[7];
};

/* GOP pixel format constants */
#define GOP_PIXEL_RGBX   0   /* PixelRedGreenBlueReserved8BitPerColor */
#define GOP_PIXEL_BGRX   1   /* PixelBlueGreenRedReserved8BitPerColor */
#define GOP_PIXEL_BITMASK 2  /* PixelBitMask (custom channel masks) */

/* Framebuffer information from the bootloader */
struct boot_framebuffer {
    uintptr_t addr;         /* physical address */
    uint32_t pitch;         /* bytes per scanline */
    uint32_t width;         /* pixels */
    uint32_t height;        /* pixels */
    uint8_t  bpp;           /* bits per pixel */
    uint8_t  type;          /* 0=indexed, 1=RGB, 2=EGA text */
    uint8_t  pixel_format;  /* GOP_PIXEL_RGBX/BGRX/BITMASK */
    uint8_t  pad0;
};

/* GOP mode entry (enumerated before ExitBootServices) */
#define BOOT_GOP_MODE_MAX  32

struct boot_gop_mode {
    uint32_t width;
    uint32_t height;
    uint32_t pixels_per_scanline;
    uint8_t  pixel_format;  /* GOP_PIXEL_* */
    uint8_t  pad[3];
};

/* Per-GOP-handle record for multi-head display (TODO-27 sec4). The bootloader
 * enumerates every GOP handle via LocateHandleBuffer and records one entry per
 * handle; the kernel multi-head driver consumes this
 * (04-drivers-hardware/TODO-17 sec6 display_register_head). fb_addr is non-zero
 * (and fb_valid==1) ONLY when the handle's framebuffer passed the SAME
 * validation as the primary (non-NULL Mode->Info, supported pixel format,
 * pitch>=width, no height*pitch*4 overflow, FrameBufferSize covers the
 * surface); a secondary head whose mode was never SetMode-configured records
 * geometry only with fb_addr=0/fb_valid=0 so the kernel never maps a stale or
 * non-display MMIO range. Exactly one entry has is_primary==1. */
#define BOOT_GOP_HANDLE_MAX  4

struct boot_gop_handle {
    uint64_t fb_addr;       /* FrameBufferBase; 0 unless fb_valid==1 */
    uint64_t fb_size;       /* FrameBufferSize reported by firmware (0 if unknown) */
    uint32_t width;
    uint32_t height;
    uint32_t pitch;         /* bytes per scanline = PixelsPerScanLine * 4 */
    uint8_t  pixel_format;  /* GOP_PIXEL_* */
    uint8_t  is_primary;    /* 1 = selected primary head (drives boot_info.fb) */
    uint8_t  fb_valid;      /* 1 = fb_addr passed full validation, safe to map */
    uint8_t  pad;
};
_Static_assert(sizeof(struct boot_gop_handle) == 32,
    "boot_gop_handle must be 32 bytes -- kernel + bootloader mirror ABI");

/* Boot configuration from \EFI\ImpossibleOS\boot.conf */
#define BOOT_CONF_CMDLINE_MAX 256

/* Boot configuration layout (shared between UEFI bootloader and kernel).
 *
 * WARNING: The UEFI bootloader (bootx64.c) has a MIRROR of this struct
 * using UEFI types. Both definitions MUST stay in sync field-for-field.
 *
 * Field offset table (assembly/ABI contract):
 *   Offset  Size  Field
 *   ------  ----  ---------------
 *     0       1   debug
 *     1       1   verbose
 *     2       1   serial_debug
 *     3       1   boot_mode
 *     4       2   splash_timeout
 *     6       1   heartbeat
 *     7       1   postcode
 *     8       1   postbars
 *     9       1   test
 *    10       1   test_suite
 *    11       1   test_quiet
 *    12       1   diag_delay
 *    13       1   diag_splash
 *    14       1   deferred
 *    15       1   async_init
 *    16       1   crash_test
 *    17      15   _reserved[]
 *    32     256   cmdline          <-- STABLE ABI offset
 *   288       1   config_found
 *   289       1   tap                (S4: TODO-04 user-mode TAP mode)
 *   290       2   _pad_align         (align uint16_t)
 *   292       2   utest_timeout_ms
 *   294      64   utest_filter[]
 *   358     154   _pad[]
 *   ---     ---
 *   512 total (sector-aligned)
 */
struct boot_config {
    /* Core */
    uint8_t  debug;            /* 1 = debug mode on (live flush to B:\) */
    uint8_t  verbose;          /* 1 = text mode, skip splash */
    uint8_t  serial_debug;     /* 1 = serial COM1 output */
    uint8_t  boot_mode;        /* 0=normal, 1=safe, 2=recovery */
    uint16_t splash_timeout;   /* seconds (0 = no timeout) */
    /* Kernel Heartbeat */
    uint8_t  heartbeat;        /* 0=off, 1=auto, 2=always */
    uint8_t  postcode;         /* 0=off, 1=auto, 2=always */
    /* VPD (Visual POST Display) */
    uint8_t  postbars;         /* 0=off, 1=on (integrated), 2=diag (full) */
    /* Test mode */
    uint8_t  test;             /* 1 = run unit tests only, then shutdown */
    uint8_t  test_suite;       /* category filter: 0..TEST_CAT_COUNT-1 = specific, 0xFF = all (default) */
    uint8_t  test_quiet;       /* 0 = verbose, 1 = suppress PASS lines (FAIL +
                                * summary only), 2 = quiet PASS lines PLUS the
                                * per-suite [COUNT] trace. Read with ascii_atoi
                                * since it was introduced, so widening it from a
                                * flag to this enum adds no field and needs no
                                * BOOT_INFO_VERSION bump. TEST_QUIET_* in
                                * include/kernel/test/test.h names the values. */
    /* Debug diagnostics */
    uint8_t  diag_delay;       /* seconds to pause on each diag screen (0 = skip) */
    uint8_t  diag_splash;      /* 1 = show diag on splash (bare metal, no serial) */
    /* Deferred init */
    uint8_t  deferred;         /* 1 = defer non-critical inits (default), 0 = all in-phase */
    /* Async init */
    uint8_t  async_init;       /* 1 = parallel subsystem init on APs, 0 = sequential (default) */
    /* Crash test */
    uint8_t  crash_test;       /* 1 = trigger deliberate BSOD after desktop init */
    /* Object Manager tracing (S15) */
    uint8_t  ob_handle_trace;  /* 1 = log every handle alloc/free to klog */
    /* Config version (S7) -- future boot.conf changes can key on this */
    uint8_t  config_version;   /* 0 = unversioned (legacy), 1+ = versioned */
    /* Error screen test (S14) -- trigger boot_fatal from bootloader for QR/BSOD testing */
    uint8_t  error_screen_test; /* 1 = call boot_fatal() before kernel load */
    /* Compositor mode (TODO-05 desktop UI test framework, headless
     * compositor section). 0 = normal display + VSYNC + swap (default);
     * 1 = headless: no fb_swap, no real VSYNC wait, tests drive frames
     * via compositor_step_frames(N). Bare-metal boot rejects 1. */
    uint8_t  compositor;
    /* Multi-monitor test-matrix expected output count (TODO-05 desktop
     * UI test framework, multi-monitor matrix section). 0 = use
     * whatever the hardware negotiates (default). 1..3 = assert the
     * display stack came up with exactly this many outputs; a mismatch
     * between the boot.conf expectation and the driver's reported
     * count is a hard fail. Today fb_get_output_count() always returns
     * 1 until the virtio-gpu multi-output driver lands. */
    uint8_t  test_monitors_count;
    /* Anti-rollback opt-in policy: 1 = kernel may raise
     * IPOSRequiredSecVersion NVRAM counter to os_loader_security
     * _version after POST16_BOOT_OK. 0 = never advance automatically
     * (default; matches pre-section-13 behavior). Shipped release
     * images set this to 1 in boot.conf when the release cadence
     * wants the monotonic counter to tick forward. */
    uint8_t  anti_rollback_raise;
    /* Firmware quirk override (firmware quirk database). Bitmask of
     * FW_QUIRK_* values to suppress. Default 0 = trust the predicate
     * table. boot.conf line `firmware_quirk_disable=broken_fpdt,bogus_mat`
     * is parsed by the bootloader into this mask. */
    uint8_t  firmware_quirk_disable;
    /* Firmware RNG collection (early entropy). 1 = collect
     * EFI_RNG_PROTOCOL bytes pre-EBS (default); 0 = boot.conf
     * firmware_rng=off escape hatch for firmware whose RNG hangs inside
     * GetRNG (no pre-EBS preemption exists to recover a non-returning
     * firmware call -- the operator disables the source instead). */
    uint8_t  firmware_rng;
    /* Random-seed carryover file lifecycle (early entropy). 1 = read +
     * rotate X:\Boot\random-seed.bin in Phase 3 (default); 0 = boot.conf
     * seed_file=off escape hatch (e.g. media whose filesystem is too
     * damaged to trust with writes). Kernel-side consumer:
     * seed_file_phase3(). */
    uint8_t  seed_file;
    /* Measured-boot baseline enrollment opt-in (measured-boot attestation).
     * 1 = the kernel MAY enroll/rotate the golden baseline this boot, honored
     * ONLY when boot_mode == 2 (recovery) so a normal boot can never silently
     * become the golden baseline (no first-boot trust-on-first-use). boot.conf
     * line `tpm_enroll=1`; default 0. */
    uint8_t  tpm_enroll;
    /* Reserved -- new config fields go here without shifting cmdline.
     * Bootloader zero-fills the entire struct, so new fields default to 0
     * in older bootloaders that don't know about them. */
    uint8_t  _reserved[5];
    /* Command line (offset 32 -- stable across versions) */
    char     cmdline[BOOT_CONF_CMDLINE_MAX];
    /* Status */
    uint8_t  config_found;     /* 1 if boot.conf was successfully parsed */
    /* User-mode test launcher knobs (S4 of TODO-04). All fields zero by
     * default -- a boot.conf without these keys matches the pre-S4
     * behavior exactly. */
    uint8_t  tap;              /* 1 = emit TAP (ok/not ok/1..N) lines around each binary */
    uint8_t  _pad_utest[2];    /* align the uint16_t below */
    uint16_t utest_timeout_ms; /* per-binary wall-clock timeout; 0 = default (10s) */
    char     utest_filter[64]; /* glob/literal filter; empty = run every test_*.exe */
    /* Per-test isolation (S6 of TODO-04). Default 1 (on); set to 0 in
     * boot.conf for debugging a broken cleanup hook. Production test
     * runs always isolate. */
    uint8_t  utest_isolation;  /* 0 = disable scratch-dir + Registry wipe + handle-leak */
    uint8_t  _pad_utest2[3];   /* align following struct fields */
    /* CI-friendly output formats (S7 of TODO-04). Both default 0;
     * orthogonal to tap= so a single run with tap=1 xml=1 json=1
     * emits all three formats interleaved on serial and scripts/
     * test.sh splits them into separate artifacts. */
    uint8_t  xml;              /* 1 = emit [UTEST-XML] JUnit XML lines */
    uint8_t  json;             /* 1 = emit [UTEST-JSON] lines (per-binary + summary) */
    /* Test-type taxonomy (S8 of TODO-04). stress_iters is RESERVED:
     * it is parsed from boot.conf and plumbed through
     * test_usermode_set_stress_iters, but has no consumer today.
     * Stress binaries (test_stress_*.exe) loop internally because
     * kernel task_create is monotonic -- a launcher-side loop of N
     * spawns would exhaust TASK_MAX after ~20 binaries. When a future
     * env-passing syscall lands, this value will be forwarded to the
     * child so a stress binary can query its desired runtime iteration
     * count. Range 0-65535 (uint16); 0 = default. */
    uint16_t stress_iters;     /* reserved (see comment above); 0 = default */
    /* Test-runner skip knobs (S10 of TODO-04 follow-up). Both default 0
     * for back-compat: a plain `test=1` boot still walks the kernel
     * TEST_CAT_* sweep AND fires the user-mode launcher in sequence.
     * Set to 1 in boot.conf (or via -NoKernelTests / -NoUsermodeTests
     * in run-qemu.ps1) to suppress one half so an aggregate runner
     * targets exactly the layer it claims:
     *   - run-all-kernel-tests.bat   -> test_usermode_skip=1 (kernel only)
     *   - run-all-usermode-tests.bat -> test_kernel_skip=1   (usermode only)
     *   - per-binary usermode bats   -> test_kernel_skip=1 + utest_filter
     * debug=1 always runs both halves regardless (the human "show me
     * everything" path). Setting both flags to 1 under test=1 is a
     * no-op boot. */
    uint8_t  test_kernel_skip;   /* 1 = skip kernel TEST_CAT_* suites under test=1 */
    uint8_t  test_usermode_skip; /* 1 = skip user-mode launcher under test=1 */
    uint8_t  _pad_test_skip[2];
    /* Pad to 512 bytes total (sector-aligned). */
    uint8_t  _pad[142];
};

/* Compile-time enforcement of bootloader ABI contract.
 * If you add a field, shrink _reserved[] / _pad[] to keep stable offsets. */
_Static_assert(__builtin_offsetof(struct boot_config, cmdline) == 32,
    "cmdline must be at byte offset 32 -- bootloader ABI contract");
_Static_assert(__builtin_offsetof(struct boot_config, config_found) == 288,
    "config_found must be at byte offset 288 -- after cmdline[256]");
_Static_assert(__builtin_offsetof(struct boot_config, tap) == 289,
    "tap must be at byte offset 289 (TODO-04 S4 stable ABI)");
_Static_assert(__builtin_offsetof(struct boot_config, utest_timeout_ms) == 292,
    "utest_timeout_ms must be at byte offset 292 (TODO-04 S4 stable ABI)");
_Static_assert(__builtin_offsetof(struct boot_config, utest_filter) == 294,
    "utest_filter must be at byte offset 294 (TODO-04 S4 stable ABI)");
_Static_assert(__builtin_offsetof(struct boot_config, utest_isolation) == 358,
    "utest_isolation must be at byte offset 358 (TODO-04 S6 stable ABI)");
_Static_assert(__builtin_offsetof(struct boot_config, xml) == 362,
    "xml must be at byte offset 362 (TODO-04 S7 stable ABI)");
_Static_assert(__builtin_offsetof(struct boot_config, json) == 363,
    "json must be at byte offset 363 (TODO-04 S7 stable ABI)");
_Static_assert(__builtin_offsetof(struct boot_config, stress_iters) == 364,
    "stress_iters must be at byte offset 364 (TODO-04 S8 stable ABI)");
_Static_assert(__builtin_offsetof(struct boot_config, test_kernel_skip) == 366,
    "test_kernel_skip must be at byte offset 366 (TODO-04 S10 follow-up stable ABI)");
_Static_assert(__builtin_offsetof(struct boot_config, test_usermode_skip) == 367,
    "test_usermode_skip must be at byte offset 367 (TODO-04 S10 follow-up stable ABI)");
_Static_assert(sizeof(struct boot_config) == 512,
    "boot_config must be exactly 512 bytes (sector-aligned)");

/* USB device discovered by UEFI firmware before ExitBootServices.
 * The bootloader uses EFI_USB_IO_PROTOCOL to enumerate all USB devices while
 * firmware is active, then passes this inventory to the kernel so it can skip
 * re-enumeration after taking over the xHCI controller. */
#define BOOT_USB_MAX_DEVICES     16
#define BOOT_USB_MAX_ENDPOINTS    4

struct boot_usb_endpoint {
    uint8_t  address;       /* bEndpointAddress (bit 7 = direction: 1=IN) */
    uint8_t  attributes;    /* bmAttributes (bits 1:0 = transfer type) */
    uint16_t max_packet;    /* wMaxPacketSize */
    uint8_t  interval;      /* bInterval (polling interval) */
    uint8_t  pad[3];
};

struct boot_usb_device {
    uint8_t  active;            /* 1 if entry is valid */
    uint8_t  port;              /* root hub port (1-based, from device path) */
    uint8_t  speed;             /* USB_SPEED_*: 1=FS, 2=LS, 3=HS, 4=SS */
    uint8_t  device_class;      /* bDeviceClass from device descriptor */
    uint8_t  iface_class;       /* bInterfaceClass of primary interface */
    uint8_t  iface_subclass;    /* bInterfaceSubClass */
    uint8_t  iface_protocol;    /* bInterfaceProtocol */
    uint8_t  num_endpoints;     /* number of populated entries in endpoints[] */
    uint16_t vendor_id;         /* idVendor */
    uint16_t product_id;        /* idProduct */
    /* MSC geometry (populated if iface_class == 0x08) */
    uint8_t  is_msc;            /* 1 if MSC BOT interface found */
    uint8_t  is_hid;            /* 1 if HID interface found */
    uint16_t pad0;
    uint32_t block_size;        /* bytes per sector (from EFI_BLOCK_IO_MEDIA) */
    uint64_t block_count;       /* total sectors (LastBlock + 1) */
    /* Endpoints (bulk-in, bulk-out, interrupt-in, etc.) */
    struct boot_usb_endpoint endpoints[BOOT_USB_MAX_ENDPOINTS];
};

/* xHCI controller state allocated by bootloader in EfiLoaderData memory.
 * Survives ExitBootServices.  Kernel must call pmm_mark_region_used() for
 * each non-zero physical address to prevent PMM from reclaiming them.
 * Set usb_handover_complete = 1 when all structures are valid. */
#define BOOT_USB_MAX_SCRATCHPADS 16
#define BOOT_USB_MAX_DMA_PAGES   16  /* max pages to reserve in PMM */

struct boot_usb_controller {
    /* PCI identity */
    uint8_t  pci_bus;
    uint8_t  pci_dev;
    uint8_t  pci_func;
    uint8_t  active;             /* 1 if controller was found and configured */

    /* MMIO base (physical -- kernel must remap via vmm_map_mmio_uc) */
    uint64_t mmio_phys;
    uint32_t mmio_size;

    /* Capability register cache */
    uint8_t  cap_length;
    uint16_t hci_version;
    uint32_t max_slots;
    uint32_t max_intrs;
    uint32_t max_ports;
    uint32_t db_offset;
    uint32_t rts_offset;
    uint8_t  ac64;               /* 64-bit addressing */
    uint8_t  csz;                /* context size: 0=32B, 1=64B */
    uint32_t max_scratchpads;

    /* DMA structure physical addresses (allocated as EfiLoaderData pages) */
    uint64_t dcbaa_phys;         /* DCBAA: (max_slots+1) × 8B, 64B aligned */
    uint64_t scratchpad_array_phys;
    uint64_t scratchpad_base_phys; /* contiguous scratchpad pages base */
    uint32_t scratchpad_page_count; /* actual pages allocated */
    uint32_t scratchpad_pad;
    uint64_t cmd_ring_phys;      /* Command Ring: 256 TRBs × 16B = 4 KiB */
    uint64_t evt_ring_phys;      /* Event Ring: 256 TRBs × 16B = 4 KiB */
    uint64_t erst_phys;          /* ERST: 1 entry × 16B (1 page) */

    /* Page tracking for kernel PMM reservation */
    uint64_t dma_pages[BOOT_USB_MAX_DMA_PAGES];
    uint32_t dma_page_count;     /* number of valid entries in dma_pages[] */
    uint32_t alloc_fail_status;  /* DEBUG: last AllocatePages failure EFI_STATUS (low 32 bits) */
    uint32_t alloc_fail_page;    /* DEBUG: which page# failed */
};

/* --- Typed payload descriptor array ---
 * Bootloader enumerates optional physical payloads (modules, initrd,
 * recovery image, hibernation metadata, TPM log copy, network config,
 * random seed, USB handover state) and publishes them here for kernel
 * consumers in.
 *
 * Contract:
 *   - payload_descriptors is a fixed-size array; payload_count is the
 *     packed-prefix length (a bootloader MUST NOT leave an occupied slot
 *     at index >= payload_count).  The kernel validator enforces this
 *     invariant so future consumers scanning the full array can trust
 *     that index < payload_count covers every real payload.
 *   - Unknown type enums are SKIPPED for type-specific validation so
 *     older kernels can ignore future payloads -- EXCEPT when the
 *     descriptor sets BOOT_PAYLOAD_FLAG_REQUIRED, which forces a fatal
 *     boot failure.
 *   - Every descriptor is overlap-checked against boot_info itself, the
 *     runtime memory map, USB DMA pages, and the framebuffer regardless
 *     of type -- unknown or future types still cannot stomp retained
 *     boot regions.
 */
#define BOOT_PAYLOAD_MAX  32

/* Well-known payload types. New values are safe to add; the kernel's
 * unknown-type skip rule keeps older kernels booting against newer
 * bootloaders that emit payloads this kernel does not recognize. */
enum boot_payload_type {
    BOOT_PAYLOAD_NONE              = 0,   /* empty slot */
    BOOT_PAYLOAD_MODULE            = 1,   /* generic kernel module (owner: module and initrd handoff contract) */
    BOOT_PAYLOAD_INITRD            = 2,   /* initrd / initramfs (owner: module and initrd handoff contract) */
    BOOT_PAYLOAD_RECOVERY_IMAGE    = 3,   /* recovery env image (owner: recovery partition bootloader) */
    BOOT_PAYLOAD_HIBERNATION_META  = 4,   /* hibernation metadata (owner: hibernation/resume handoff) */
    BOOT_PAYLOAD_TPM_EVENT_LOG     = 5,   /* TPM TCG event log copy (owner: TPM measured boot event log parser) */
    BOOT_PAYLOAD_NETWORK_CONFIG    = 6,   /* network boot config blob (owner: network boot provenance) */
    BOOT_PAYLOAD_RANDOM_SEED       = 7,   /* bootloader RNG seed (owner: early entropy seed handoff) */
    BOOT_PAYLOAD_USB_HANDOVER      = 8,   /* xHCI DMA state blob (owner: USB zero-delay handover DMA state) */
    BOOT_PAYLOAD_WARM_UPDATE_STATE = 9,   /* warm-kernel-update preserved memory (owner: warm-kernel-update ABI) */
};

/* Descriptor flags (bitmask). New bits are ignored by older kernels if
 * not listed in BOOT_PAYLOAD_FLAG_MASK_KNOWN, which is how capability
 * negotiation will extend this surface without a version bump. */
#define BOOT_PAYLOAD_FLAG_VALID        (1u << 0)  /* 1 = descriptor is valid and should be processed */
#define BOOT_PAYLOAD_FLAG_CHECKSUMMED  (1u << 1)  /* 1 = checksum low 32 bits carry CRC-32C of payload */
#define BOOT_PAYLOAD_FLAG_REQUIRED     (1u << 2)  /* 1 = unknown type aborts boot instead of skipping */
#define BOOT_PAYLOAD_FLAG_RESERVED     (1u << 3)  /* 1 = PMM must mark this region reserved */
#define BOOT_PAYLOAD_FLAG_MASK_KNOWN   \
    (BOOT_PAYLOAD_FLAG_VALID | BOOT_PAYLOAD_FLAG_CHECKSUMMED | \
     BOOT_PAYLOAD_FLAG_REQUIRED | BOOT_PAYLOAD_FLAG_RESERVED)

/* Producer identity -- which subsystem populated this descriptor. Lets
 * consumers attribute ownership without re-deriving it from type. */
enum boot_payload_producer {
    BOOT_PRODUCER_NONE          = 0,
    BOOT_PRODUCER_UEFI          = 1,   /* src/boot/uefi/bootx64.c */
    BOOT_PRODUCER_MULTIBOOT2    = 2,   /* RESERVED -- alt-boot policy = unsupported (docs/boot/alt-boot.md) */
    BOOT_PRODUCER_KERNEL_TEST   = 3,   /* src/kernel/test/test_boot_info.c fixture */
};

struct boot_payload_desc {
    /* enum boot_payload_type (u32 for ABI stability).
     * type=BOOT_PAYLOAD_NONE means empty slot; ALL other fields MUST be
     * zero or the validator rejects with BOOT_PAYLOAD_ERR_NONE_NOT_EMPTY. */
    uint32_t type;
    uint32_t flags;        /* BOOT_PAYLOAD_FLAG_* bitmask; must be 0 when type=NONE */
    uint64_t phys_start;   /* physical address of payload; must be 0 when type=NONE */
    uint64_t length;       /* size in bytes; must be 0 when type=NONE */
    uint64_t alignment;    /* required natural alignment (power of 2); 0 = none. Must be 0 when type=NONE. */
    uint64_t checksum;     /* CRC-32C in low 32 bits if FLAG_CHECKSUMMED; else 0. Must be 0 when type=NONE. */
    uint32_t producer_id;  /* enum boot_payload_producer; must be 0 when type=NONE */
    uint32_t _reserved;    /* pad to 48 bytes; must be 0 always */
};

_Static_assert(sizeof(struct boot_payload_desc) == 48,
    "boot_payload_desc ABI size pinned at 48 bytes -- update mirror + manifest on change");

/* enum boot_payload_error is declared earlier alongside the validator
 * prototype so callers and tests can include this header without
 * needing the full struct boot_info definition in scope. */

/* All boot info collected from UEFI bootloader */
struct boot_info {
    /* ABI header -- must be at offset 0 (S15) */
    struct boot_info_header header;

    /* Memory map */
    struct boot_mmap_entry mmap[BOOT_MMAP_MAX_ENTRIES];
    uint32_t mmap_count;
    uint8_t  mmap_truncated;    /* 1 if firmware had more entries than BOOT_MMAP_MAX_ENTRIES */
    uint8_t  mmap_quirks;       /* 1 if any descriptors had validation warnings (S12) */
    uint8_t  _mmap_pad[2];

    /* Basic memory (from tag type 4) */
    uint32_t mem_lower_kb;  /* conventional memory in KiB */
    uint32_t mem_upper_kb;  /* extended memory in KiB */

    /* Framebuffer */
    struct boot_framebuffer fb;
    uint8_t  fb_available;  /* 1 if framebuffer tag was found */
    uint8_t  hidpi;         /* 1 if negotiated GOP width >= 2560 (set by bootloader) */
    uint8_t  fb_pad[2];     /* alignment padding */

    /* GOP mode list (enumerated by bootloader) */
    struct boot_gop_mode gop_modes[BOOT_GOP_MODE_MAX];
    uint32_t gop_mode_count;     /* number of valid entries */
    uint32_t gop_mode_selected;  /* ordinal into gop_modes[] (0..gop_mode_count-1);
                                    equals gop_mode_count when the active raw firmware
                                    mode was not enumerated into the bounded table
                                    (sentinel = "off-table"). Consumers MUST check
                                    `gop_mode_selected < gop_mode_count` before indexing. */

    /* ACPI */
    uintptr_t acpi_rsdp_addr;   /* physical address of RSDP */
    uint8_t  acpi_version;      /* 1 = RSDP v1, 2 = RSDP v2 */
    uint8_t  acpi_available;    /* 1 if ACPI tag was found */

    /* Module (GRUB module) */
    uintptr_t module_start;     /* physical address of first module */
    uintptr_t module_end;       /* physical address of end of module */
    uint8_t   module_available; /* 1 if a module was loaded */

    /* Boot configuration (parsed from boot.conf) */
    struct boot_config config;

    /* UEFI Configuration Table (copied from EFI_SYSTEM_TABLE) */
    struct boot_uefi_config_entry config_table[BOOT_CONFIG_TABLE_MAX];
    uint32_t config_table_count;

    /* UEFI Runtime Services (for kernel to call after ExitBootServices) */
    uintptr_t uefi_runtime_services;  /* phys addr of EFI_RUNTIME_SERVICES table */
    uint8_t   uefi_rt_available;      /* 1 if runtime services pointer was saved */
    struct boot_rt_mem_entry rt_mmap[BOOT_RT_MMAP_MAX]; /* runtime memory regions */
    uint32_t rt_mmap_count;           /* number of runtime regions */
    uint32_t uefi_mmap_desc_size;     /* UEFI descriptor size (for SVAM) */
    uint32_t uefi_mmap_desc_version;  /* UEFI descriptor version (for SVAM) */
    struct boot_uefi_runtime uefi_runtime; /* individual RT function pointers */

    /* TPM Measured Boot (event log from EFI_TCG2_PROTOCOL) */
    uintptr_t tpm_event_log;          /* phys addr of copied event log buffer */
    uint32_t  tpm_event_log_size;     /* size of event log in bytes */
    uint8_t   tpm_available;          /* 1 if TPM was detected */
    uint8_t   tpm_version;            /* 0=none, 1=TPM 1.2, 2=TPM 2.0 */
    uint16_t  tpm_event_count;        /* number of events in log */

    /* USB devices discovered by UEFI firmware before ExitBootServices */
    struct boot_usb_device usb_devices[BOOT_USB_MAX_DEVICES];
    uint32_t usb_device_count;      /* number of valid entries */
    uint8_t  usb_discovery_ok;      /* 1 if USB discovery completed successfully */
    uint8_t  usb_handover_complete; /* 1 if bootloader allocated DMA + configured controller */
    uint8_t  usb_pad[2];

    /* xHCI controller DMA state (allocated by bootloader in EfiLoaderData) */
    struct boot_usb_controller usb_controller;

    /* Boot Timing (TSC timestamps from bootloader + FPDT) */
    struct {
        /* FPDT firmware performance record (nanoseconds, from firmware) */
        uint64_t reset_end;               /* SEC phase complete */
        uint64_t os_loader_load_start;    /* bootloader load began */
        uint64_t os_loader_start_start;   /* bootloader started executing */
        uint64_t exit_bs_entry;           /* ExitBootServices called */
        uint64_t exit_bs_exit;            /* ExitBootServices returned */
        uint8_t  fpdt_available;          /* 1 if FPDT was found */
        /* Bootloader phase timestamps (TSC ticks via rdtsc) */
        uint64_t bl_entry;                /* efi_main entered */
        uint64_t gop_start;               /* init_gop start */
        uint64_t gop_end;                 /* init_gop end */
        uint64_t conf_start;              /* parse_boot_conf start */
        uint64_t conf_end;                /* parse_boot_conf end */
        uint64_t kernel_load_start;       /* load_kernel start */
        uint64_t kernel_load_end;         /* load_kernel end */
        uint64_t splash_start;            /* boot_splash start */
        uint64_t splash_end;              /* boot_splash end */
        uint64_t exit_bs;                 /* just before ExitBootServices */
        uint64_t kernel_jump;             /* just before jumping to kernel */
        uint64_t tsc_freq;                /* TSC frequency in Hz (0 = unknown) */
    } timing;

    /* Serial port (probed by bootloader; 0 = no UART detected) */
    uint16_t serial_port;           /* I/O base.  serial_source==2 (no-SPCR
                                     * scratch probe) -> 0x3F8 (COM1) or
                                     * 0x2F8 (COM2) only.  serial_source==1
                                     * (ACPI SPCR firmware-authoritative)
                                     * may publish any non-zero 16-bit I/O
                                     * base in [1, 0xFFF8] -- COM3 0x3E8,
                                     * COM4 0x2E8, or vendor-custom.
                                     * Upper bound 0xFFF8 keeps the 16550
                                     * register block (base..base+7) in
                                     * the 16-bit I/O port space. */
    uint8_t  serial_source;         /* 0=none, 1=SPCR, 2=I/O-probe */
    uint8_t  _serial_pad;
    uint32_t serial_baud;           /* baud rate from SPCR (0 = use default 38400) */

    /* NVRAM boot error from previous boot (S13); 0 = last boot OK */
    uint32_t last_boot_error;

    /* Boot device info (populated by bootloader from LoadedImage) */
    uint8_t  boot_device_type;      /* 0=unknown, 1=SATA, 2=NVMe, 3=USB, 4=network, 5=SD, 6=eMMC */
    uint8_t  _boot_dev_pad[3];      /* alignment */
    char     boot_device_path[128]; /* UEFI device path text (DevicePathToText) */

    /* UEFI boot variables (§6: read pre-ExitBootServices) */
    uint16_t uefi_boot_current;     /* BootCurrent: firmware-selected Boot#### entry */
    uint16_t uefi_boot_next;        /* BootNext: one-shot override (0xFFFF = not set) */
    uint8_t  uefi_boot_next_valid;  /* 1 if BootNext was present */
    uint8_t  uefi_boot_order_count; /* number of valid entries in uefi_boot_order[] */
    uint8_t  _boot_var_pad[2];      /* alignment */
    uint16_t uefi_boot_order[16];   /* first 16 entries of BootOrder variable */

    /* Boot partition info (extracted from device path HardDrive node) */
    uint8_t  boot_partition_guid[16]; /* raw GUID bytes (GPT) or 4-byte MBR sig in [0..3] */
    uint8_t  boot_partition_style;    /* 0=unknown, 1=MBR, 2=GPT */

    /* Removable media info (from EFI_BLOCK_IO_PROTOCOL.Media) */
    uint8_t  boot_device_removable;  /* 0=fixed, 1=removable (USB/SD/external) */
    uint8_t  boot_media_present;     /* 0=no media, 1=media inserted */
    uint8_t  _rem_pad;               /* alignment */

    /* Kernel-populated fields (set after boot; never written by the bootloader) */
    uint8_t  secure_boot_enabled;   /* 1 if Secure Boot is active (uefi_secureboot_init) */
    uint8_t  _kp_pad[3];            /* alignment */
    uint32_t degraded_mask;         /* bitmask of non-critical subsystems that failed init */
    uint32_t hv_flags;              /* hypervisor feature flags (HV_FLAG_*) */
    char     hv_vendor[16];         /* hypervisor vendor string (null-terminated) */

    /* Typed payload descriptor array. See boot_payload_desc above.
     * Bootloader MUST populate payload_count as the packed-prefix length;
     * the kernel validator rejects any occupied slot at index >=
     * payload_count. payload_total_bytes MUST equal the sum of length for
     * all occupied slots; the kernel validator rejects a mismatch. */
    struct boot_payload_desc payload_descriptors[BOOT_PAYLOAD_MAX];
    uint32_t payload_count;         /* 0..BOOT_PAYLOAD_MAX; packed-prefix length */
    uint32_t payload_overflow;      /* 1 if bootloader had > BOOT_PAYLOAD_MAX payloads */
    uint64_t payload_total_bytes;   /* sum of length for all occupied slots */

    /* Capability negotiation -- see BOOT_CAP_* bitmasks and
     * boot_caps_validate() contract above. */
    uint64_t caps_required;         /* bits loader asserts kernel MUST support */
    uint64_t caps_present;          /* bits for features loader actually populated */
    uint64_t caps_degraded;         /* known-but-not-provided bits (adapter degradation) */

    /* Boot-path provenance and decision record (v8). See the
     * enum boot_path_type / boot_reason_code / BOOT_SOURCE_FLAG_*
     * block above for semantics. Consumers read this BEFORE inspecting
     * any per-path descriptors (recovery image, resume metadata,
     * network provenance) so they know which descriptor to trust. */
    uint32_t boot_path;             /* enum boot_path_type */
    uint32_t boot_reason;           /* enum boot_reason_code */
    uint32_t boot_source_flags;     /* BOOT_SOURCE_FLAG_* bitmask */
    uint32_t boot_fallback_depth;   /* 0 = primary, N = Nth fallback */

    /* Anti-rollback and security-version binding (v9). See the
     * BOOT_FLAG_* + boot_rollback_validate() contract above. */
    uint32_t flags;                         /* BOOT_FLAG_* bitmask (currently ROLLBACK_REFUSAL) */
    uint32_t os_loader_security_version;    /* security version baked into kernel.exe at build */
    uint32_t required_security_version;     /* value bootloader read from IPOSRequiredSecVersion NVRAM */
    uint32_t _rollback_pad;                 /* reserved; zero */

    /* EFI System Partition integrity gate (v11). Populated by the UEFI
     * bootloader's pre-load sanity check (esp_integrity_check) BEFORE
     * parse_boot_conf and load_kernel. Surfaces the ESP identity to
     * post-boot tools via HKLM\HARDWARE\BOOT\ESP\* without re-reading
     * firmware. The unique ESP partition GUID itself lives in
     * boot_partition_guid above; the type-GUID match result is the
     * single bit esp_type_guid_valid. In UKI invocation mode the
     * bootloader populates esp_size_mb when BlockIO is available but
     * leaves esp_type_guid_valid = 0 and esp_filesystem_type = 0 --
     * the trust anchor for UKI is the signed PE image itself, so the
     * disk identity check is skipped (split-path-only gate). */
    uint32_t esp_size_mb;            /* ESP partition size in MiB; 0 if unknown */
    uint8_t  esp_filesystem_type;    /* 0=unknown, 1=FAT16, 2=FAT32 */
    uint8_t  esp_type_guid_valid;    /* 1 if GPT type GUID matched ESP type GUID; 0 otherwise/UKI/non-GPT */
    uint8_t  _esp_pad[2];            /* alignment; reserved zero */

    /* Bootloader build identity (v13). Populated by the UEFI
     * bootloader from compile-time constants in
     * build/boot_loader_identity.h before the boot_info header magic
     * write. Zero git_sha[] (impossible for a real SHA-1) is the
     * "loader did not populate" sentinel for back-compat with stale
     * bootloaders. */
    struct boot_loader_identity loader_identity;

    /* UKI-embedded payload addresses (v14). Populated by the UEFI
     * bootloader after walking the LoadedImage's PE section table
     * for .initrd / .recovery / .modules and copying each payload
     * out of the LoadedImage region into an AllocatePages-allocated
     * EfiLoaderData range that survives ExitBootServices. Zero addr
     * means the section was absent in the UKI (back-compat with UKI
     * builds that ship without payload sections). Non-zero addr is a
     * kernel-physical pointer; size is the original PE section
     * VirtualSize. Set ONLY when boot_info.flags &
     * BOOT_FLAG_INVOKED_VIA_UKI; ignored on the legacy split path. */
    uint64_t uki_initrd_addr;        /* kernel-physical pointer to copied .initrd payload, 0 if absent */
    uint64_t uki_initrd_size;        /* size in bytes, 0 if absent */
    uint64_t uki_recovery_addr;      /* kernel-physical pointer to copied .recovery payload, 0 if absent */
    uint64_t uki_recovery_size;
    uint64_t uki_modules_addr;       /* kernel-physical pointer to copied .modules cpio payload, 0 if absent */
    uint64_t uki_modules_size;

    /* v15: Extended UEFI boot-variable capability surface (TODO-05 extended-boot-vars feature).
     * BootCurrent's Boot####.Attributes word, two firmware-side capability
     * variables, and the Boot#### Description string. Defaults to 0 / empty
     * when the firmware variable is absent. Read-only; OsIndicationsSupported
     * is the read-only capability variable (UEFI 2.10 spec 8.5.4), distinct
     * from the OsIndications write-path capsule trigger banned by the UEFI
     * hardening TODO. */
    uint32_t boot_current_attrs;          /* EFI_LOAD_OPTION.Attributes for BootCurrent (UEFI 2.10 spec 3.1.3) */
    uint32_t boot_option_support;         /* BootOptionSupport global (UEFI 2.10 spec 3.1.4) */
    uint64_t os_indications_supported;    /* OsIndicationsSupported global (UEFI 2.10 spec 8.5.4) */
    char     boot_description[64];        /* Boot#### Description ASCII-truncated, NUL-terminated */

    /* v16: Local boot device path detail (TODO-05 boot-device-detail).
     * Per-bus identifiers extracted from the boot device's full UEFI
     * device path: NVMe NSID + EUI-64 (UEFI 2.10 spec 10.3.4.21),
     * leaf PCI device + function (UEFI 2.10 spec 10.3.2.1).
     * Sentinels: nsid=0, eui64=all-zero, pci_*=0xFF when not on that bus. */
    uint32_t boot_nvme_nsid;              /* NVMe NamespaceId; 0 = not NVMe or absent */
    uint8_t  boot_nvme_eui64[8];          /* NVMe NamespaceUuid byte order (EUI-64); all-zero if absent */
    uint8_t  boot_pci_device;             /* leaf PCI Device number (0..31), 0xFF if not on PCI */
    uint8_t  boot_pci_function;           /* leaf PCI Function number (0..7), 0xFF if not on PCI */
    uint8_t  _v16_pad[2];                 /* alignment to 4-byte boundary */

    /* v17: Media role detected from /IPOS/role.txt on the ESP and on
     * the BlackBox service partition (boot-media role-detection
     * feature). See enum boot_media_role above for the role values,
     * semantics, and the boot_path/boot_reason coupling rule. ESP
     * marker is authoritative; BlackBox marker is cross-checked.
     * mismatch=1 means both partitions had a marker but disagreed --
     * the loader fell back to BOOT_MEDIA_ROLE_NORMAL and emitted a
     * serial [WARN] line. mismatch=0 covers both "agreed" and
     * "BlackBox absent" -- absent is not a mismatch. */
    uint32_t boot_media_role;             /* enum boot_media_role */
    uint8_t  boot_media_role_mismatch;    /* 1 = ESP/BlackBox markers disagreed */
    uint8_t  _v17_pad[3];                 /* alignment to 4-byte boundary */

    /* v18: Firmware trust-landscape surface (artifact-signing /
     * manifest-verification feature). Populated by uefi_secureboot
     * _init() in src/kernel/uefi_runtime.c after the SecureBoot /
     * SetupMode reads complete; relies on RuntimeServices->
     * GetVariable so it runs post-EBS in the kernel-populated section
     * (NOT the bootloader). Read by boot_decision_validate() (advisory
     * warning) and boot_device_populate_registry() (HKLM\SYSTEM\Boot
     * \Trust\*). Manifest signature verification itself is blocked on
     * bootloader-side crypto and lives in a separate ABI bump when
     * those primitives ship; v18 carries only the SBAT/dbx/trust-flags
     * subset. See BOOT_DEGRADED_TRUST_* above for bitmask semantics
     * and the no-bit-outside-MASK_KNOWN closed-mask invariant. */
    uint32_t sbat_level_size;             /* bytes of SbatLevel read; 0 = absent/unreadable */
    char     sbat_level[64];              /* SbatLevel string truncated to 63 bytes + NUL */
    uint32_t dbx_size;                    /* bytes in dbx variable; 0 = absent/unreadable */
    uint32_t degraded_trust_flags;        /* BOOT_DEGRADED_TRUST_* bitmask */

    /* v19: Boot policy selection (boot-entry policy feature). The bootloader
     * walks the precedence ladder (hotkey > watchdog > A/B > recovery >
     * store-default > fallback) and records the chosen entry id + reason
     * here. Parallel to boot_path/boot_reason: those are the path-flow
     * axis, this is the policy-ladder axis. When the chosen entry implies
     * a path change (recovery, diagnostics), the loader updates
     * boot_path/boot_reason as well.
     *
     * rejected_entries[] records per-entry policy-layer rejects: ladder
     * filter misses (kind_skipped, not active, hidden, machine_id mismatch,
     * tries_exhausted, path_escape) plus the BootCurrent unmapped diagnostic.
     * Parser-level rejects (CRC mismatch, schema_version, etc.) fail the
     * whole store; this array is for per-entry rejects after the store
     * passed parsing. See enum boot_selection_reason / boot_reject_reason
     * in include/boot/boot_policy.h.
     *
     * Producer status: LIVE. The bootloader's boot_policy_invoke()
     * (src/boot/uefi/bootx64.c) reads bootentries.json from the ESP,
     * decodes Boot####.OptionalData, runs boot_policy_decide(), and
     * publishes the decision into the v19 fields BEFORE ExitBootServices.
     *
     * UNSET (0) is reserved as a producer-bug sentinel: kernel-side
     * consumers (registry / policy audit / loader-variables feature)
     * MUST validate selection_reason within
     * [BOOT_SELECTION_STORE_DEFAULT..BOOT_SELECTION_REASON_MAX] and
     * REJECT UNSET as a missing-producer fault. The bootloader code
     * path always publishes a non-UNSET reason, including a synthetic
     * BOOT_SELECTION_FALLBACK_STORE_INVALID for AllocatePool exhaustion
     * (publishes the empty-selected_entry_id sentinel directly without
     * heap), so consumers seeing UNSET have caught a regression.
     *
     * For BOOT_SELECTION_FALLBACK_STORE_INVALID, selected_entry_id is
     * EMPTY (the documented sentinel for an unusable parsed store).
     * The bootloader synthesizes a fallback envelope via
     * boot_entries_synthesize_fallback() for kind capture and the
     * load-time identifier, but the v19 selected_entry_id stays empty
     * to distinguish "policy chose nothing from the store" from
     * "policy chose entry-X". For every other selection_reason, the
     * field is non-empty (NUL-terminated entry id from the parsed
     * store).
     *
     * rejected_entry_count==0 is always legal (no entries filtered out). */
    char     selected_entry_id[64];       /* internal id of the entry chosen by the ladder */
    uint32_t selection_reason;            /* enum boot_selection_reason */
    uint32_t _selection_pad;              /* alignment to 8-byte boundary */
    struct {
        char     id[64];                  /* envelope.id of the rejected entry */
        uint32_t reason;                  /* enum boot_reject_reason */
    } rejected_entries[64];               /* BOOT_ENTRIES_MAX_ENTRIES; packed prefix */
    uint32_t rejected_entry_count;        /* 0..64; packed-prefix length */
    uint32_t rejected_entry_overflow;     /* 1 if more than 64 rejects observed */

    /* v20: Policy audit surface. The bootloader reads
     * `ImpossibleOS-BootSticky` (256-byte UEFI variable) pre-policy
     * and surfaces the relevant bits here. NVRAM is exceptional-only:
     * the bootloader NEVER writes the sticky var; the kernel's
     * boot_audit_publish() acks consumed triggers post-publish so a
     * crash between observe and ack leaves the trigger pending.
     *
     * audit_degraded == 1 means the bootloader could not trust the
     * sticky read (missing / wrong attrs / wrong size / bad CRC).
     * Consumers must treat the sticky_* fields as zeros in that case
     * and emit BOOT_AUDIT_EVENT_AUDIT_DEGRADED in the JSONL line.
     *
     * sticky_present == 1 indicates a valid sticky record was read;
     * 0 means absent or untrusted. */
    uint8_t  audit_degraded;
    uint8_t  sticky_present;
    uint8_t  sticky_recovery_trigger;
    uint8_t  sticky_watchdog_rollback_request;
    uint8_t  sticky_last_outcome;
    uint8_t  sticky_audit_degraded_last_boot;
    uint16_t sticky_last_event_code;      /* enum boot_audit_event_code */
    uint32_t sticky_last_boot_seq;
    uint32_t sticky_consumed_trigger_seq;
    uint32_t _audit_pad;                  /* alignment to 8-byte boundary */

    /* v21: OS-visible Loader UEFI variables (systemd-boot interface
     * compatibility). The bootloader publishes 11 read-only Loader*
     * variables under vendor GUID 4a67b082-0a4c-41cf-b6c7-440b29bb8c4f
     * and consumes 2 one-shot vars (LoaderEntryOneShot,
     * LoaderConfigTimeoutOneShot). A SetVariable failure on any of
     * these (NVRAM quota full, firmware refusal) flips
     * loader_vars_degraded so the kernel can record degraded
     * publication state in the boot audit JSONL. The bootloader does
     * NOT halt on these failures -- userland still boots, just
     * without complete Loader* observability. See docs/boot/loader-
     * vars.md. */
    uint8_t  loader_vars_degraded;
    /* A/B dual-slot boot (TODO-21): the slot the bootloader selected pre-EBS
     * after reading the on-disk ab_boot_metadata record. Encoding matches
     * include/boot/ab_boot_metadata.h: 0 = Slot A (AB_BOOT_SLOT_A), 1 = Slot B
     * (AB_BOOT_SLOT_B). Default 0 so an older zero-filling bootloader, a
     * single-slot disk, or a non-A/B boot path all resolve to Slot A. The
     * kernel mounts EXACTLY this slot's IXFS as C: (partition_mount_filesystems)
     * and records the slot it actually mounted; a selected-vs-mounted mismatch
     * blocks the future mark-boot-successful path from marking the wrong slot
     * good. Carved from _loader_vars_pad (reserved region) -- no
     * BOOT_INFO_VERSION bump per the reserved-region rule. */
    uint8_t  active_slot;
    /* A/B slot-status snapshot for kernel boot diagnostics (TODO-21 sec6).
     * Published by the bootloader's select_active_slot from the
     * ab_boot_meta_decide result AT SELECTION TIME -- authoritative because the
     * on-disk metadata mutates during boot (ab_bl_increment_tries increments
     * tries pre-EBS; the kernel mark-good resets it at first-frame), so a kernel
     * re-read at VPD time would show a different state than was actually decided.
     * ab_select_reason: 0=normal, 1=rollback, 2=both-exhausted (matches
     * enum ab_boot_select_reason). ab_from_slot: the slot rolled away from
     * (rollback) or the recorded active slot (both-exhausted); 0 when normal.
     * ab_slot_tries[s]: per-slot tries consumed as selected (0..AB_BOOT_MAX_TRIES).
     * ab_slot_flags: bit s = slot s successful (use ab_boot_slot_flag()).
     * All zero on a non-A/B / single-slot / older-bootloader boot. Carved from
     * _loader_vars_pad (reserved region) -- no BOOT_INFO_VERSION bump.
     *
     * ab_status_valid is the producer-valid marker: a bootloader that publishes
     * this snapshot sets it to AB_STATUS_VALID_MAGIC. Because these bytes were
     * carved from reserved space WITHOUT a version bump, an older same-version
     * (v22) bootloader publishes ab_meta_lba but leaves the snapshot zero; the
     * kernel MUST gate the detailed slot/rollback render on ab_status_valid
     * (not merely ab_meta_lba != 0) or a real rollback could be silently
     * rendered as a healthy zero-state ("snapshot unavailable" instead). */
    uint8_t  ab_select_reason;
    uint8_t  ab_from_slot;
    uint8_t  ab_slot_tries[2];
    uint8_t  ab_slot_flags;
    uint8_t  ab_status_valid;

    /* v22: A/B-metadata partition range (TODO-21 atomic-write handoff). The
     * bootloader reconciles the metadata partition (primary+backup GPT) in
     * select_active_slot and publishes its location here so the kernel write
     * path locates the partition WITHOUT re-deriving GPT state via the weaker
     * kernel gpt_parse (which lacks the bootloader's primary+backup
     * reconciliation -- a stale/split-brain primary would diverge). ab_meta_lba
     * is the partition's first absolute LBA on the boot/parent disk (parent
     * block units, matches partition_info.start_lba); ab_meta_block_count is its
     * size in those blocks. Both 0 when no A/B metadata partition exists
     * (non-A/B disk / non-GPT boot). The boot disk itself is identified by the
     * existing boot_partition_guid. */
    uint64_t ab_meta_lba;
    uint32_t ab_meta_block_count;
    uint32_t _ab_meta_pad;          /* reserved; zero (align to 8) */

    /* v23: Multi-GPU GOP handle enumeration (TODO-27 sec4). One record per GOP
     * handle the bootloader found via LocateHandleBuffer (capped at
     * BOOT_GOP_HANDLE_MAX); exactly one has is_primary==1 and drives
     * boot_info.fb. Secondary heads carry geometry for the kernel multi-head
     * driver (04-drivers-hardware/TODO-17 sec6); each head's fb_addr is non-zero
     * only when fb_valid==1. gop_handle_count==0 ONLY when no GOP handle was
     * exported (headless / no display / enumeration failed); a successful
     * single-GPU boot publishes gop_handle_count==1 with gop_handles[0] as the
     * primary. A count==0 consumer must fall back to boot_info.fb (which is
     * itself zeroed on headless); it must NOT treat count==0 as the single-head
     * case -- that is count==1. */
    struct boot_gop_handle gop_handles[BOOT_GOP_HANDLE_MAX];
    uint32_t gop_handle_count;
    uint32_t _gop_handle_pad;       /* reserved; zero (align to 8) */
};

/* Compile-time enforcement of ABI header layout (S15) */
_Static_assert(__builtin_offsetof(struct boot_info, header) == 0,
    "boot_info_header must be at offset 0 -- ABI contract");
_Static_assert(sizeof(struct boot_info_header) == 8,
    "boot_info_header must be exactly 8 bytes -- ABI contract");
/* header.size is uint16_t; struct must fit so the runtime size check cannot
 * wrap.  If this fails, widen boot_info_header.size to uint32_t and bump
 * BOOT_INFO_VERSION. */
_Static_assert(sizeof(struct boot_info) <= 65535,
    "boot_info too large for uint16_t header.size field -- widen size field or trim struct");

/* S16: Cross-struct layout fingerprint.  The bootloader at
 * src/boot/uefi/bootx64.c keeps a mirror definition of struct boot_info.
 * Total-size matching alone cannot catch same-size field reorders that
 * would otherwise silently diverge the two interpretations.  Both the
 * kernel header and the bootloader mirror pin the byte offsets of the
 * five critical count fields below.  A build failure on EITHER side
 * flags drift; a cross-build mismatch of sizes still triggers the
 * runtime header.size check.  When adding fields, update BOTH sides
 * atomically.  If an offset shifts intentionally, update both asserts
 * in the same commit.  Computed once from the canonical kernel layout:
 *   mmap_count           = 16392  (after header + 512 mmap entries)
 *   gop_mode_count       = 16948
 *   config_table_count   = 18280
 *   rt_mmap_count        = 20352
 *   usb_device_count     = 21504
 */
_Static_assert(__builtin_offsetof(struct boot_info, mmap_count) == 16392,
    "boot_info.mmap_count offset drift -- update kernel + bootloader mirror");
_Static_assert(__builtin_offsetof(struct boot_info, gop_mode_count) == 16948,
    "boot_info.gop_mode_count offset drift -- update kernel + bootloader mirror");
_Static_assert(__builtin_offsetof(struct boot_info, config_table_count) == 18280,
    "boot_info.config_table_count offset drift -- update kernel + bootloader mirror");
_Static_assert(__builtin_offsetof(struct boot_info, rt_mmap_count) == 20352,
    "boot_info.rt_mmap_count offset drift -- update kernel + bootloader mirror");
_Static_assert(__builtin_offsetof(struct boot_info, usb_device_count) == 21504,
    "boot_info.usb_device_count offset drift -- update kernel + bootloader mirror");
_Static_assert(__builtin_offsetof(struct boot_info, boot_device_type) == 21924,
    "boot_info.boot_device_type offset drift -- update kernel + bootloader mirror");
_Static_assert(__builtin_offsetof(struct boot_info, boot_device_path) == 21928,
    "boot_info.boot_device_path offset drift -- update kernel + bootloader mirror");
_Static_assert(__builtin_offsetof(struct boot_info, uefi_boot_current) == 22056,
    "boot_info.uefi_boot_current offset drift -- update kernel + bootloader mirror");
_Static_assert(__builtin_offsetof(struct boot_info, uefi_boot_next) == 22058,
    "boot_info.uefi_boot_next offset drift -- update kernel + bootloader mirror");
_Static_assert(__builtin_offsetof(struct boot_info, uefi_boot_next_valid) == 22060,
    "boot_info.uefi_boot_next_valid offset drift -- update kernel + bootloader mirror");
_Static_assert(__builtin_offsetof(struct boot_info, uefi_boot_order_count) == 22061,
    "boot_info.uefi_boot_order_count offset drift -- update kernel + bootloader mirror");
_Static_assert(__builtin_offsetof(struct boot_info, uefi_boot_order) == 22064,
    "boot_info.uefi_boot_order offset drift -- update kernel + bootloader mirror");
_Static_assert(__builtin_offsetof(struct boot_info, boot_partition_guid) == 22096,
    "boot_info.boot_partition_guid offset drift -- update kernel + bootloader mirror");
_Static_assert(__builtin_offsetof(struct boot_info, boot_partition_style) == 22112,
    "boot_info.boot_partition_style offset drift -- update kernel + bootloader mirror");
_Static_assert(__builtin_offsetof(struct boot_info, boot_device_removable) == 22113,
    "boot_info.boot_device_removable offset drift -- update kernel + bootloader mirror");
_Static_assert(__builtin_offsetof(struct boot_info, boot_media_present) == 22114,
    "boot_info.boot_media_present offset drift -- update kernel + bootloader mirror");
_Static_assert(__builtin_offsetof(struct boot_info, hv_flags) == 22124,
    "boot_info.hv_flags offset drift -- update kernel + bootloader mirror");
_Static_assert(__builtin_offsetof(struct boot_info, hv_vendor) == 22128,
    "boot_info.hv_vendor offset drift -- update kernel + bootloader mirror");

/* v14: UKI-embedded payload offset asserts. The six fields are
 * appended after struct boot_loader_identity (offset 23760, size
 * 64; tail at 23824). Per-field asserts catch any same-size reorder
 * that the total size + magic-version pair would miss. */
_Static_assert(__builtin_offsetof(struct boot_info, uki_initrd_addr) == 23824,
    "boot_info.uki_initrd_addr offset drift -- update kernel + bootloader mirror");
_Static_assert(__builtin_offsetof(struct boot_info, uki_initrd_size) == 23832,
    "boot_info.uki_initrd_size offset drift -- update kernel + bootloader mirror");
_Static_assert(__builtin_offsetof(struct boot_info, uki_recovery_addr) == 23840,
    "boot_info.uki_recovery_addr offset drift -- update kernel + bootloader mirror");
_Static_assert(__builtin_offsetof(struct boot_info, uki_recovery_size) == 23848,
    "boot_info.uki_recovery_size offset drift -- update kernel + bootloader mirror");
_Static_assert(__builtin_offsetof(struct boot_info, uki_modules_addr) == 23856,
    "boot_info.uki_modules_addr offset drift -- update kernel + bootloader mirror");
_Static_assert(__builtin_offsetof(struct boot_info, uki_modules_size) == 23864,
    "boot_info.uki_modules_size offset drift -- update kernel + bootloader mirror");

/* v15: Extended UEFI boot-variable capability surface offset asserts.
 * Appended at struct tail after the v14 UKI cluster (which ends at 23872). */
_Static_assert(__builtin_offsetof(struct boot_info, boot_current_attrs) == 23872,
    "boot_info.boot_current_attrs offset drift -- update kernel + bootloader mirror");
_Static_assert(__builtin_offsetof(struct boot_info, boot_option_support) == 23876,
    "boot_info.boot_option_support offset drift -- update kernel + bootloader mirror");
_Static_assert(__builtin_offsetof(struct boot_info, os_indications_supported) == 23880,
    "boot_info.os_indications_supported offset drift -- update kernel + bootloader mirror");
_Static_assert(__builtin_offsetof(struct boot_info, boot_description) == 23888,
    "boot_info.boot_description offset drift -- update kernel + bootloader mirror");

/* v16: Local boot device path detail offset asserts. Appended after v15
 * boot_description (which ends at 23888 + 64 = 23952). */
_Static_assert(__builtin_offsetof(struct boot_info, boot_nvme_nsid) == 23952,
    "boot_info.boot_nvme_nsid offset drift -- update kernel + bootloader mirror");
_Static_assert(__builtin_offsetof(struct boot_info, boot_nvme_eui64) == 23956,
    "boot_info.boot_nvme_eui64 offset drift -- update kernel + bootloader mirror");
_Static_assert(__builtin_offsetof(struct boot_info, boot_pci_device) == 23964,
    "boot_info.boot_pci_device offset drift -- update kernel + bootloader mirror");
_Static_assert(__builtin_offsetof(struct boot_info, boot_pci_function) == 23965,
    "boot_info.boot_pci_function offset drift -- update kernel + bootloader mirror");

/* v17: media-role offsets. boot_media_role is uint32 so it lands on the
 * next 4-byte boundary after the v16 _v16_pad[2] tail; mismatch is the
 * single byte after, _v17_pad[3] aligns the next addition to 4 bytes. */
_Static_assert(__builtin_offsetof(struct boot_info, boot_media_role) == 23968,
    "boot_info.boot_media_role offset drift -- update kernel + bootloader mirror");
_Static_assert(__builtin_offsetof(struct boot_info, boot_media_role_mismatch) == 23972,
    "boot_info.boot_media_role_mismatch offset drift -- update kernel + bootloader mirror");

/* v18: trust-landscape offsets. After v17's _v17_pad[3] the next
 * uint32 lands at 23976 (the boot_media_role_mismatch byte at 23972
 * plus the 3-byte pad). sbat_level[64] then runs to 24044; dbx_size
 * + degraded_trust_flags pin the tail. */
_Static_assert(__builtin_offsetof(struct boot_info, sbat_level_size) == 23976,
    "boot_info.sbat_level_size offset drift -- update kernel + bootloader mirror");
_Static_assert(__builtin_offsetof(struct boot_info, sbat_level) == 23980,
    "boot_info.sbat_level offset drift -- update kernel + bootloader mirror");
_Static_assert(__builtin_offsetof(struct boot_info, dbx_size) == 24044,
    "boot_info.dbx_size offset drift -- update kernel + bootloader mirror");
_Static_assert(__builtin_offsetof(struct boot_info, degraded_trust_flags) == 24048,
    "boot_info.degraded_trust_flags offset drift -- update kernel + bootloader mirror");
/* A/B dual-slot active_slot (TODO-21): pinned RELATIVE to loader_vars_degraded
 * so it survives unrelated v22+ tail additions; carved from the reserved
 * _loader_vars_pad so total struct size is unchanged. The bootloader mirror
 * pins the identical relationship -- a drift on either side fails the build. */
_Static_assert(__builtin_offsetof(struct boot_info, active_slot) ==
               __builtin_offsetof(struct boot_info, loader_vars_degraded) + 1,
    "boot_info.active_slot must immediately follow loader_vars_degraded -- "
    "update kernel + bootloader mirror");
/* sec6 A/B slot-status snapshot: contiguous after active_slot so the kernel VPD
 * and the bootloader publish agree byte-for-byte; carved from _loader_vars_pad
 * so total struct size is unchanged (no BOOT_INFO_VERSION bump). */
_Static_assert(__builtin_offsetof(struct boot_info, ab_select_reason) ==
               __builtin_offsetof(struct boot_info, active_slot) + 1,
    "boot_info.ab_select_reason must immediately follow active_slot -- "
    "update kernel + bootloader mirror");
_Static_assert(__builtin_offsetof(struct boot_info, ab_slot_tries) ==
               __builtin_offsetof(struct boot_info, ab_from_slot) + 1,
    "boot_info.ab_slot_tries must immediately follow ab_from_slot");
_Static_assert(__builtin_offsetof(struct boot_info, ab_slot_flags) ==
               __builtin_offsetof(struct boot_info, ab_slot_tries) + 2,
    "boot_info.ab_slot_flags must follow the 2-byte ab_slot_tries array");
_Static_assert(__builtin_offsetof(struct boot_info, ab_status_valid) ==
               __builtin_offsetof(struct boot_info, ab_slot_flags) + 1,
    "boot_info.ab_status_valid must immediately follow ab_slot_flags -- "
    "update kernel + bootloader mirror");
/* v22 A/B metadata range (TODO-21): 8-byte-aligned uint64 + uint32 + pad.
 * Pinned relative + size so the bootloader mirror cannot drift; the manifest
 * compare gate cross-checks absolute offsets kernel-vs-mirror at build. */
_Static_assert(__builtin_offsetof(struct boot_info, ab_meta_lba) % 8 == 0,
    "boot_info.ab_meta_lba must be 8-byte aligned (uint64)");
_Static_assert(__builtin_offsetof(struct boot_info, ab_meta_block_count) ==
               __builtin_offsetof(struct boot_info, ab_meta_lba) + 8,
    "boot_info.ab_meta_block_count must immediately follow ab_meta_lba -- "
    "update kernel + bootloader mirror");

/* Global boot info -- populated by the UEFI bootloader (storage def in
 * src/kernel/main/boot_hw.c). Alternate boot protocols are unsupported
 * (docs/boot/alt-boot.md). */
extern struct boot_info g_boot_info;

/*: Populate HKLM\SYSTEM\Boot\Device\ from g_boot_info.
 * Called from registry_populate_defaults() after registry_init(). */
void boot_device_populate_registry(void);

/* Populate HKLM\SYSTEM\Boot\Decision\* from the validated decision
 * record (boot_path / boot_reason / boot_source_flags / boot_fallback
 * _depth). Called from registry_populate_defaults() in Phase 2 after
 * boot_device_populate_registry(). Writes Path / PathName / Reason /
 * ReasonName / SourceFlags / FallbackDepth. */
void boot_decision_populate_registry(void);

/* ============================================================================
 * Boot Error History Ring (producer schema)
 * ============================================================================
 *
 * Bounded ring of the last 8 boot attempts, persisted in NVRAM.  Three
 * append sites: bootloader boot_fatal() (any fatal pre-EBS), bootloader
 * EBS-success (sentinel 0xFFFE), kernel boot_phase3 entry (sentinel 0xFFFF).
 *
 * Storage: a single `BootErrorHistory` UEFI variable (128 bytes, NV+BS+RT
 * attrs) under IMPOSSIBLE_OS_VENDOR_GUID, plus a `BootHistorySeq` u32
 * cookie under the same GUID.  head = seq % BOOT_HIST_RING_LEN.  Wrap is
 * unconditional: oldest entry is overwritten on append.
 *
 * Atomicity: SetVariable is per-variable; there is no transactional
 * batch.  The producer writes the ring FIRST, then increments the
 * cookie.  A torn write between the two leaves a stale-but-bounded read
 * (head points at the previous slot), never UB.
 *
 * The kernel-side reader, BlackBox transcribe, and renderer are owned
 * by the consumer half of the boot-error history feature; this block
 * pins ONLY the producer schema. */

#define BOOT_HIST_RING_LEN          8u   /* fixed entry count */
#define BOOT_HIST_BIN_SIZE          128u /* BOOT_HIST_RING_LEN * sizeof(entry) */
#define BOOT_SECTION_UNKNOWN        0xFFFDu /* g_boot_section default; surfaces hint gaps */
#define BOOT_SECTION_EBS_OK         0xFFFEu /* bootloader reached ExitBootServices */
#define BOOT_SECTION_KERNEL_PHASE3  0xFFFFu /* kernel reached boot_phase3 */

struct boot_error_history_entry {
    uint32_t boot_seq;        /* monotonic from BootHistorySeq cookie */
    uint32_t unix_time;       /* RuntimeServices->GetTime epoch seconds, 0 if unavailable */
    uint16_t err_code;        /* BOOT_ERR_* (see efi.h); 0 for sentinels */
    uint16_t source_section;  /* roadmap-doc section number or BOOT_SECTION_* sentinel */
    uint32_t _pad;            /* reserved; must be 0 on write, ignored on read */
};

_Static_assert(sizeof(struct boot_error_history_entry) == 16,
    "boot_error_history_entry must be exactly 16 bytes -- ring ABI contract");
_Static_assert(__builtin_offsetof(struct boot_error_history_entry, boot_seq) == 0,
    "boot_error_history_entry.boot_seq must be at offset 0");
_Static_assert(__builtin_offsetof(struct boot_error_history_entry, unix_time) == 4,
    "boot_error_history_entry.unix_time must be at offset 4");
_Static_assert(__builtin_offsetof(struct boot_error_history_entry, err_code) == 8,
    "boot_error_history_entry.err_code must be at offset 8");
_Static_assert(__builtin_offsetof(struct boot_error_history_entry, source_section) == 10,
    "boot_error_history_entry.source_section must be at offset 10");
_Static_assert(__builtin_offsetof(struct boot_error_history_entry, _pad) == 12,
    "boot_error_history_entry._pad must be at offset 12");
_Static_assert(BOOT_HIST_BIN_SIZE ==
    BOOT_HIST_RING_LEN * sizeof(struct boot_error_history_entry),
    "BOOT_HIST_BIN_SIZE must equal BOOT_HIST_RING_LEN * entry size");

/* Kernel-side Phase-3 sentinel append.  Writes (BOOT_SECTION_KERNEL_PHASE3,
 * 0) into the ring via uefi_var_set + uefi_var_set_u32 (RT services
 * remain callable post-EBS).  Idempotent on repeated call within the
 * same boot.  Silently no-ops when RT services are unavailable. */
void boot_history_kernel_mark_phase3(void);

/* Return the boot_seq successfully committed to NVRAM by mark_phase3, or 0
 * if not yet called or NVRAM writes failed. Consumers use this to
 * distinguish "this boot's seq" from "highest seq in ring (may be older)"
 * when the Phase-3 mark could not be persisted. */
uint32_t boot_history_kernel_phase3_committed_seq(void);

/* Consumer-side reader.  Reads the 8-entry NVRAM ring + cookie via
 * uefi_var_get / uefi_var_get_u32, validates size + attrs (mismatch
 * triggers the same delete-then-warn repair as the writer side), and
 * fills `out_ring` with up to BOOT_HIST_RING_LEN entries.  Returns the
 * count of non-zero entries (0..BOOT_HIST_RING_LEN); zero-initializes
 * out_ring on any failure.  Pure-data: callable safely from boot_hw.c
 * after uefi_vars_init() returns BOOT_OK.  The X:\Diag\boot-error-
 * history.bin BlackBox-file path is reserved for future expansion
 * (when the ring outgrows NVRAM); for now NVRAM is the sole channel. */
size_t boot_history_read(struct boot_error_history_entry out_ring[BOOT_HIST_RING_LEN]);

/* Decode a source_section value into a stable human label.  Sentinels
 * (UNKNOWN/EBS_OK/KERNEL_PHASE3) get named strings; bootloader-phase
 * codes (0x01NN) get "bl-init"/"bl-conf"/"bl-kernel"/"bl-pagetables"/
 * "bl-ebs"; everything else falls back to "section-0xNNNN".  Returns
 * the number of bytes (excluding terminator) written to `out` -- never
 * exceeds `cap - 1`. */
size_t boot_history_decode_source_section(uint16_t src, char *out, size_t cap);

/* Top-level renderer.  Calls boot_history_read and emits a klog block
 * "[BOOT] Recent boot history (N attempts):" plus one line per non-
 * zero entry, oldest-first by boot_seq.  Silent no-op when count==0.
 * Wire AFTER uefi_vars_init in boot_phase0/boot_hw.c. */
void boot_history_render(void);
