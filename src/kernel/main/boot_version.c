/* ============================================================================
 * boot_version.c -- boot protocol version negotiation implementation.
 *
 * Version-negotiation + stale-loader error path. See
 * include/kernel/boot_version.h for the full flow and compatibility
 * policy (exact match required).
 *
 * Scope boundary: this module handles kernel-side classification,
 * fatal rendering, NVRAM persistence, and late-boot BlackBox
 * transcription. Bootloader-side pre-jump version display (the other
 * half of item 2) is a separate artifact that needs kernel ELF
 * parsing and is tracked as a remaining checklist item.
 * anti-rollback integration populates the loader_sec_ver slots when
 * that section ships.
 * ============================================================================ */

#include "kernel/boot_version.h"
#include "kernel/boot_info.h"
#include "kernel/uefi_runtime.h"
#include "kernel/klog.h"
#include "kernel/boot_halt.h"
#include "kernel/mm/heap.h"
#include "kernel/fs/vfs.h"
#include "libc/string.h"

/* NVRAM GUID for the fault variable. Distinct from the POST16 GUID
 * so a rogue POST16 overwriter (or vice versa) cannot confuse the
 * two records. */
static const struct boot_uefi_guid s_fault_guid = {
    0x494D504F, 0x5354, 0x4656,   /* "IMPOSTFV" */
    { 0x50, 0x52, 0x4F, 0x54, 0x4F, 0x46, 0x4C, 0x54 }  /* "PROTOFLT" */
};

/* UCS-2 variable name. */
static const uint16_t s_fault_name[] = {
    'I','m','p','o','s','s','i','b','l','e','B','o','o','t',
    'P','r','o','t','o','F','a','u','l','t', 0
};

#define FAULT_ATTRS (EFI_VARIABLE_NON_VOLATILE | \
                     EFI_VARIABLE_BOOTSERVICE_ACCESS | \
                     EFI_VARIABLE_RUNTIME_ACCESS)

const char *boot_version_fault_operator_hint(uint32_t fault_class)
{
    /* Single source of truth for operator-response wording. Consumers:
     * boot_version_render_fatal (serial fatal screen) and
     * boot_version_blackbox_transcribe (X:\Diag transcript). Keep the
     * three classes' hints distinct -- the same advice for rollback
     * and ABI drift would send operators down the wrong recovery
     * path. */
    switch (fault_class) {
    case BOOT_VERSION_FAULT_SEC_ROLLBACK:
        return "boot a newer signed kernel OR clear the "
               "IPOSRequiredSecVersion NVRAM variable under operator "
               "consent";
    case BOOT_VERSION_FAULT_BAD_SHA:
        return "rebuild bootloader AND kernel in lockstep -- the "
               "descriptor layout drifted at the sub-version level";
    case BOOT_VERSION_FAULT_BAD_PARSE:
        return "the kernel ELF is missing or malformed -- reflash "
               "kernel.exe and re-sign";
    default:
        /* Default = structural ABI drift (BAD_VERSION / BAD_SIZE /
         * BAD_MAGIC / NULL_HDR / OK with mismatch). Operators MUST
         * rebuild AND re-flash both halves: rebuilding alone leaves
         * the previous bootloader/kernel pair on the ESP, so the
         * machine re-enters the same refusal on next boot. */
        return "rebuild AND re-flash bootloader + kernel "
               "(`bash scripts/build.sh` then write the updated "
               "system disk)";
    }
}

const char *boot_version_fault_class_name(uint32_t fault_class)
{
    switch (fault_class) {
    case BOOT_VERSION_OK:                    return "OK";
    case BOOT_VERSION_FAULT_NULL_HDR:        return "NULL_HDR";
    case BOOT_VERSION_FAULT_BAD_MAGIC:       return "BAD_MAGIC";
    case BOOT_VERSION_FAULT_BAD_VERSION:     return "BAD_VERSION";
    case BOOT_VERSION_FAULT_BAD_SIZE:        return "BAD_SIZE";
    case BOOT_VERSION_FAULT_SEC_ROLLBACK:    return "SEC_ROLLBACK";
    case BOOT_VERSION_FAULT_BAD_SHA:         return "BAD_SHA";
    case BOOT_VERSION_FAULT_BAD_PARSE:       return "BAD_PARSE";
    default:                                 return "UNKNOWN";
    }
}

static void zero_fault(struct boot_version_fault *f)
{
    uint32_t i;
    f->record_magic            = BOOT_VERSION_FAULT_MAGIC;
    f->fault_class             = BOOT_VERSION_OK;
    f->observed_magic          = 0u;
    f->expected_magic          = BOOT_INFO_MAGIC;
    f->observed_version        = 0u;
    f->expected_version        = BOOT_INFO_VERSION;
    f->observed_size           = 0u;
    f->expected_size           = 0u;
    f->observed_loader_sec_ver = 0u;
    f->expected_loader_sec_ver = 0u;
    for (i = 0u; i < sizeof(f->_reserved) / sizeof(f->_reserved[0]); i++)
        f->_reserved[i] = 0u;
}

boot_result_t boot_version_classify(const struct boot_info_header *hdr,
                                    size_t expected_struct_size,
                                    struct boot_version_fault *out_fault)
{
    struct boot_version_fault local;
    struct boot_version_fault *f =
        (out_fault != (struct boot_version_fault *)0) ? out_fault : &local;

    zero_fault(f);
    f->expected_size = (uint32_t)expected_struct_size;

    if (hdr == (const struct boot_info_header *)0) {
        f->fault_class = BOOT_VERSION_FAULT_NULL_HDR;
        return BOOT_FATAL;
    }

    /* Observed values are read ONCE up front so the fault record is
     * self-consistent even if the bootloader's memory is briefly
     * stomped between field reads (e.g. a firmware bug that writes to
     * the handoff region post-ExitBootServices). */
    f->observed_magic   = hdr->magic;
    f->observed_version = hdr->version;
    f->observed_size    = hdr->size;

    if (f->observed_magic != BOOT_INFO_MAGIC) {
        f->fault_class = BOOT_VERSION_FAULT_BAD_MAGIC;
        return BOOT_FATAL;
    }
    if ((uint16_t)f->observed_version != BOOT_INFO_VERSION) {
        f->fault_class = BOOT_VERSION_FAULT_BAD_VERSION;
        return BOOT_FATAL;
    }
    if ((size_t)f->observed_size != expected_struct_size) {
        f->fault_class = BOOT_VERSION_FAULT_BAD_SIZE;
        return BOOT_FATAL;
    }
    return BOOT_OK;
}

int boot_version_persist_nvram(const struct boot_version_fault *fault)
{
    if (fault == (const struct boot_version_fault *)0)
        return 0;
    uint64_t status = uefi_set_variable(&s_fault_guid, s_fault_name,
                                        FAULT_ATTRS,
                                        sizeof(*fault), fault);
    /* UEFI_UNSUPPORTED (the uefi_runtime layer returns this when it is
     * not yet available) is NOT an error -- the caller is halting
     * anyway, and the fatal screen + serial log have already
     * surfaced the diagnostic. Only status==0 (EFI_SUCCESS) is a real
     * "persisted" result. */
    return status == 0u ? 1 : 0;
}

void boot_version_render_fatal(const struct boot_version_fault *fault)
{
    /* Two LOG_FATAL lines separate the "what class" label from the
     * observed/expected values, so grep-ing boot logs for the label
     * yields both lines. */
    const char *class_name = boot_version_fault_class_name(
        fault ? fault->fault_class : BOOT_VERSION_FAULT_NULL_HDR);

    /* Class-specific headline. SEC_ROLLBACK is operator-actionable
     * distinct from structural ABI drift; per-class wording comes from
     * boot_version_fault_operator_hint() so render_fatal and
     * blackbox_transcribe never drift from each other. The headline
     * label is class-specific; the trailing operator guidance is the
     * canonical hint string. */
    const char *hint = boot_version_fault_operator_hint(
        fault ? fault->fault_class : BOOT_VERSION_FAULT_NULL_HDR);
    if (fault && fault->fault_class == BOOT_VERSION_FAULT_SEC_ROLLBACK) {
        klog(LOG_FATAL, "boot",
             "Security-version downgrade refused -- class=%s "
             "(operator response: %s)",
             (uint64_t)(uintptr_t)class_name,
             (uint64_t)(uintptr_t)hint);
    } else {
        klog(LOG_FATAL, "boot",
             "Boot protocol version mismatch -- class=%s "
             "(operator response: %s)",
             (uint64_t)(uintptr_t)class_name,
             (uint64_t)(uintptr_t)hint);
    }
    if (fault) {
        klog(LOG_FATAL, "boot",
             "  observed magic=0x%x version=%u size=%u",
             (uint64_t)fault->observed_magic,
             (uint64_t)fault->observed_version,
             (uint64_t)fault->observed_size);
        klog(LOG_FATAL, "boot",
             "  expected magic=0x%x version=%u size=%u",
             (uint64_t)fault->expected_magic,
             (uint64_t)fault->expected_version,
             (uint64_t)fault->expected_size);
        if (fault->fault_class == BOOT_VERSION_FAULT_SEC_ROLLBACK) {
            klog(LOG_FATAL, "boot",
                 "  security version: observed=%u expected=%u "
                 "(anti-rollback refusal; boot a newer kernel)",
                 (uint64_t)fault->observed_loader_sec_ver,
                 (uint64_t)fault->expected_loader_sec_ver);
        }
    }

    /* NVRAM persistence deliberately NOT attempted here for the Phase 0
     * caller: uefi_runtime_init runs LATER in boot_phase0, so
     * uefi_set_variable would always return UEFI_UNSUPPORTED on the
     * stale-loader path this function was designed for. The fault is
     * fully diagnosed on serial + framebuffer via klog(LOG_FATAL)
     * above; post-halt persistence is covered by the bootloader-side
     * pre-jump NVRAM write tracked as a remaining checklist item.
     * If a FUTURE caller with uefi_runtime already live (for example,
     * anti-rollback post-SetVirtualAddressMap) wants NVRAM
     * persistence, it can call boot_version_persist_nvram(fault)
     * directly before invoking render_fatal. */
    (void)fault;  /* fall through to halt */

    boot_halt("boot_version: protocol mismatch -- rebuild bootloader + kernel");
    /* boot_halt terminates in an HLT loop; the __builtin_unreachable
     * is a belt-and-suspenders hint so the 'noreturn' contract on
     * boot_version_render_fatal survives through compilers that do
     * not trust boot_halt's HLT loop alone. */
    __builtin_unreachable();
}

/* ---- Late-boot transcription ---------------------------------------------
 *
 * Reads any persisted NVRAM record, renders a human-readable text file
 * under X:\Diag\, and clears the NVRAM slot so the record does not
 * replay on every boot. Called from boot_desktop.c next to the
 * hw_dump_write_file and boot_reserved_blackbox_dump calls.
 *
 * Deliberately tolerant: NO record, uefi_runtime down, VFS down,
 * X:\ missing, kmalloc failure -- all silent no-ops. The goal is
 * "when BlackBox is up AND a record exists, write it"; anything else
 * keeps the system booting.
 * -------------------------------------------------------------------------- */

static uint32_t hex_nibble(uint32_t v)
{
    return v + ((v < 10u) ? '0' : ('A' - 10));
}

static uint32_t append_str(char *buf, uint32_t pos, uint32_t max,
                           const char *s)
{
    while (*s && pos < max - 1u)
        buf[pos++] = *s++;
    return pos;
}

static uint32_t append_dec(char *buf, uint32_t pos, uint32_t max,
                           uint64_t v)
{
    char tmp[24];
    int n = 0;
    if (v == 0u) tmp[n++] = '0';
    else while (v > 0u && n < 24) { tmp[n++] = (char)('0' + (v % 10u)); v /= 10u; }
    while (n > 0 && pos < max - 1u) buf[pos++] = tmp[--n];
    return pos;
}

static uint32_t append_hex32(char *buf, uint32_t pos, uint32_t max,
                             uint32_t v)
{
    if (pos + 10u >= max) return pos;
    buf[pos++] = '0';
    buf[pos++] = 'x';
    int i;
    for (i = 28; i >= 0; i -= 4)
        buf[pos++] = (char)hex_nibble((v >> i) & 0xFu);
    return pos;
}

static uint32_t append_line(char *buf, uint32_t pos, uint32_t max,
                            const char *s)
{
    pos = append_str(buf, pos, max, s);
    if (pos < max - 1u) buf[pos++] = '\n';
    return pos;
}

void boot_version_blackbox_transcribe(void)
{
    extern int klog_using_blackbox;
    if (!klog_using_blackbox)
        return;

    struct boot_version_fault rec;
    uint64_t sz = sizeof(rec);
    uint32_t attrs = 0;
    uint64_t status = uefi_get_variable(&s_fault_guid, s_fault_name,
                                        &attrs, &sz, &rec);
    if (status != 0u)
        return;  /* no record (or RT unavailable) */
    if (sz != sizeof(rec) || rec.record_magic != BOOT_VERSION_FAULT_MAGIC) {
        klog(LOG_WARN, "boot",
             "boot_version transcribe: stale/corrupt NVRAM record "
             "(size=%u magic=0x%x); clearing",
             (uint64_t)sz, (uint64_t)rec.record_magic);
        /* Clear via zero-length SetVariable. */
        (void)uefi_set_variable(&s_fault_guid, s_fault_name,
                                FAULT_ATTRS, 0u, (const void *)0);
        return;
    }

    /* Render to a small buffer then write via vfs. Same pattern as
     * hw_dump_write_file. 2 KiB is plenty for a ~20-line text dump. */
    const uint32_t max_sz = 2048u;
    char *buf = (char *)kmalloc(max_sz);
    if (!buf) {
        klog(LOG_WARN, "boot",
             "boot_version transcribe: kmalloc(%u) failed",
             (uint64_t)max_sz);
        return;
    }
    uint32_t pos = 0u;

    pos = append_line(buf, pos, max_sz,
                      "Impossible OS -- Boot Protocol Fault (prior boot)");
    pos = append_line(buf, pos, max_sz,
                      "=================================================");
    pos = append_str(buf, pos, max_sz, "Fault class: ");
    pos = append_line(buf, pos, max_sz,
                      boot_version_fault_class_name(rec.fault_class));

    pos = append_str(buf, pos, max_sz, "Observed magic:   ");
    pos = append_hex32(buf, pos, max_sz, rec.observed_magic);
    pos = append_line(buf, pos, max_sz, "");
    pos = append_str(buf, pos, max_sz, "Expected magic:   ");
    pos = append_hex32(buf, pos, max_sz, rec.expected_magic);
    pos = append_line(buf, pos, max_sz, "");

    pos = append_str(buf, pos, max_sz, "Observed version: ");
    pos = append_dec(buf, pos, max_sz, (uint64_t)rec.observed_version);
    pos = append_line(buf, pos, max_sz, "");
    pos = append_str(buf, pos, max_sz, "Expected version: ");
    pos = append_dec(buf, pos, max_sz, (uint64_t)rec.expected_version);
    pos = append_line(buf, pos, max_sz, "");

    pos = append_str(buf, pos, max_sz, "Observed size:    ");
    pos = append_dec(buf, pos, max_sz, (uint64_t)rec.observed_size);
    pos = append_line(buf, pos, max_sz, "");
    pos = append_str(buf, pos, max_sz, "Expected size:    ");
    pos = append_dec(buf, pos, max_sz, (uint64_t)rec.expected_size);
    pos = append_line(buf, pos, max_sz, "");

    if (rec.fault_class == BOOT_VERSION_FAULT_SEC_ROLLBACK) {
        pos = append_line(buf, pos, max_sz, "");
        pos = append_line(buf, pos, max_sz,
                          "Anti-rollback refusal:");
        pos = append_str(buf, pos, max_sz, "  observed security version: ");
        pos = append_dec(buf, pos, max_sz,
                         (uint64_t)rec.observed_loader_sec_ver);
        pos = append_line(buf, pos, max_sz, "");
        pos = append_str(buf, pos, max_sz, "  expected security version: ");
        pos = append_dec(buf, pos, max_sz,
                         (uint64_t)rec.expected_loader_sec_ver);
        pos = append_line(buf, pos, max_sz, "");
        pos = append_str(buf, pos, max_sz, "Operator response: ");
        pos = append_line(buf, pos, max_sz,
                          boot_version_fault_operator_hint(rec.fault_class));
    } else if (rec.fault_class == BOOT_VERSION_FAULT_BAD_PARSE) {
        /* Bootloader stored the raw parser error enum in
         * observed_loader_sec_ver so this transcript can name the
         * specific step that failed. Values mirror
         * `enum bootproto_result` in src/boot/uefi/elf_bootproto.h;
         * keep the switch in sync with that enum. */
        pos = append_line(buf, pos, max_sz, "");
        pos = append_str(buf, pos, max_sz,
                         ".bootproto parse failure: ");
        const char *parse_name;
        switch (rec.observed_loader_sec_ver) {
        case 0:  parse_name = "OK (unexpected)";      break;
        case 1:  parse_name = "NULL_IMAGE";           break;
        case 2:  parse_name = "EHDR_BOUNDS";          break;
        case 3:  parse_name = "NOT_ELF64";            break;
        case 4:  parse_name = "SHT_BOUNDS";           break;
        case 5:  parse_name = "SHT_ENTSIZE";          break;
        case 6:  parse_name = "SHSTR_IDX";            break;
        case 7:  parse_name = "SHSTR_BOUNDS";         break;
        case 8:  parse_name = "NAME_BOUNDS";          break;
        case 9:  parse_name = "NOT_FOUND";            break;
        case 10: parse_name = "SECT_BOUNDS";          break;
        case 11: parse_name = "SECT_ALIGN";           break;
        case 12: parse_name = "SECT_SIZE";            break;
        default: parse_name = "UNKNOWN";              break;
        }
        pos = append_line(buf, pos, max_sz, parse_name);
        pos = append_line(buf, pos, max_sz, "");
        pos = append_str(buf, pos, max_sz, "Operator response: ");
        pos = append_line(buf, pos, max_sz,
                          boot_version_fault_operator_hint(rec.fault_class));
    } else if (rec.fault_class == BOOT_VERSION_FAULT_BAD_SHA) {
        pos = append_line(buf, pos, max_sz, "");
        pos = append_line(buf, pos, max_sz,
                          ".bootproto manifest hash drift: magic/version/"
                          "size matched but SHA-256 diverged.");
        pos = append_str(buf, pos, max_sz, "Operator response: ");
        pos = append_line(buf, pos, max_sz,
                          boot_version_fault_operator_hint(rec.fault_class));
    } else {
        pos = append_line(buf, pos, max_sz, "");
        pos = append_str(buf, pos, max_sz, "Operator response: ");
        pos = append_line(buf, pos, max_sz,
                          boot_version_fault_operator_hint(rec.fault_class));
    }

    /* Write via vfs_open/write, same pattern as hw_dump_write_file. */
    const char *diag_dir = "X:\\Diag\\";
    {
        struct vfs_node *dir = vfs_open(diag_dir, VFS_O_READ);
        if (dir && dir->ops && dir->ops->create)
            dir->ops->create(dir, BOOT_VERSION_FAULT_BLACKBOX_FILE, VFS_FILE);
    }
    char path[64];
    int pi = 0;
    int j;
    for (j = 0; diag_dir[j] && pi < (int)sizeof(path) - 1; j++)
        path[pi++] = diag_dir[j];
    const char *fn = BOOT_VERSION_FAULT_BLACKBOX_FILE;
    for (j = 0; fn[j] && pi < (int)sizeof(path) - 1; j++)
        path[pi++] = fn[j];
    path[pi] = '\0';

    int transcript_written = 0;
    /* Stale-tail prevention. The filename is fixed across boots, so a
     * shorter newer transcript would otherwise leave stale tail bytes
     * past the end of the new payload (vfs_write at offset 0 does NOT
     * shrink an existing FAT32 file). VFS_O_TRUNC is defined in vfs.h
     * but vfs_open ignores it today; the systemic VFS-layer fix
     * (FAT32 cached-state refresh + exact-slot handle cleanup +
     * directory policy unification + flag wiring) is owned by the
     * VFS_O_TRUNC end-to-end TODO in 05-storage-filesystems/TODO-04
     * (the same section that owns the boot_reserved_blackbox_dump
     * workaround). Until that section lands, mirror the explicit-
     * truncate workaround:
     *   1. open + read node->size to detect a pre-existing longer file
     *   2. close + vfs_truncate(path, 0) when prior_size > 0
     *   3. reopen for write
     *   4. write + check byte-exact return
     * Any failure path leaves the NVRAM record intact for next-boot
     * retry rather than destroying it after a torn write. */
    struct vfs_node *f = vfs_open(path, VFS_O_WRITE);
    if (f) {
        uint64_t prior_size = f->size;
        if (prior_size > 0u) {
            vfs_close(f);
            int trunc_rc = vfs_truncate(path, 0);
            if (trunc_rc != 0) {
                /* LOG_ERROR (matches boot_reserved_blackbox_dump
                 * severity contract): a confirmed pre-existing
                 * transcript could not be replaced safely. NVRAM is
                 * retained but a persistent BlackBox failure must
                 * surface in ERROR-level log review. */
                klog(LOG_ERROR, "boot",
                     "boot_version: vfs_truncate(%s) failed on "
                     "prior-size=%lu file (%d); transcript NOT "
                     "written to avoid stale-tail corruption; "
                     "NVRAM record retained for next-boot retry",
                     (uint64_t)(uintptr_t)path,
                     (uint64_t)prior_size,
                     (uint64_t)trunc_rc);
                kfree(buf);
                return;
            }
            f = vfs_open(path, VFS_O_WRITE);
            if (!f) {
                klog(LOG_ERROR, "boot",
                     "boot_version: vfs_open(%s) failed after "
                     "truncate; transcript NOT written; NVRAM "
                     "retained for next-boot retry",
                     (uint64_t)(uintptr_t)path);
                kfree(buf);
                return;
            }
        }
        int wrote = vfs_write(f, 0, pos, (const uint8_t *)buf);
        vfs_close(f);
        if (wrote >= 0 && (uint32_t)wrote == pos) {
            transcript_written = 1;
            klog(LOG_WARN, "boot",
                 "boot_version: prior boot failed with protocol fault; "
                 "transcript written to %s",
                 (uint64_t)(uintptr_t)path);
        } else {
            /* LOG_ERROR: short/errored write may have torn the
             * transcript -- the only durable record (NVRAM) is
             * retained, but the artifact a triage reader sees on
             * disk could be partial. Surface in ERROR-level review. */
            klog(LOG_ERROR, "boot",
                 "boot_version: vfs_write short/error (wrote=%d of %u); "
                 "transcript may be torn; NVRAM retained for retry",
                 (uint64_t)wrote, (uint64_t)pos);
        }
    } else {
        /* LOG_WARN (not LOG_ERROR): on a freshly-mounted BlackBox the
         * walk_path can fail before the FAT32 dir cache refreshes,
         * with no stale-tail risk because no prior file exists. The
         * NVRAM record is retained for next-boot retry; no FAIL marker
         * needed (matches the boot_reserved_blackbox_dump pattern). */
        klog(LOG_WARN, "boot",
             "boot_version: vfs_open failed for %s; retaining NVRAM "
             "record for next-boot retry",
             (uint64_t)(uintptr_t)path);
    }
    kfree(buf);

    /* Clear the NVRAM slot ONLY after the transcript landed. Commit-
     * after-success is load-bearing: a transient X:\Diag write
     * failure here must not destroy the only copy of the fault
     * record. Per UEFI spec, SetVariable with DataSize=0 deletes. */
    if (transcript_written) {
        (void)uefi_set_variable(&s_fault_guid, s_fault_name,
                                FAULT_ATTRS, 0u, (const void *)0);
    }
}
