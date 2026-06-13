/* ============================================================================
 * entropy_registry.c -- entropy diagnostics surfaces (registry + BlackBox)
 *
 * Three Phase-3 surfaces over the entropy model -- none of them ever
 * carries seed bytes (mask / quality / class only):
 *
 *   entropy_external_consume()   one-shot admin entropy injection (Win11
 *                                ExternalEntropy parity): consumes the
 *                                HKLM\SYSTEM\Boot\Entropy\ExternalEntropy
 *                                REG_BINARY value exactly once -- the
 *                                stored value is OVERWRITTEN WITH ZEROS
 *                                and deleted BEFORE the copied bytes are
 *                                absorbed, so no later reader (including
 *                                a second consume call) can see them.
 *                                Absorbed at ENTROPY_Q_LOW: admin-supplied
 *                                bytes are unverifiable and must never
 *                                launder provenance. SECURE-DELETE GATE:
 *                                a value is absorbed ONLY when registry
 *                                persistence is inactive (in-memory-only,
 *                                no on-disk recovery source can retain the
 *                                bytes). On-disk / cross-reboot offerings
 *                                are DEFERRED -- the hive .bak/journal
 *                                would otherwise let a recovered value be
 *                                absorbed twice; secure deletion across all
 *                                recovery sources lands with the registry
 *                                hive-load wiring (TODO-14 Advanced Hive
 *                                Features), the same work that activates
 *                                the cross-reboot path.
 *   entropy_populate_registry()  HKLM\SYSTEM\Boot\Entropy\Diagnostics
 *                                mirror -- a dedicated sub-key, kept separate
 *                                from the ExternalEntropy input value so the
 *                                mirror never carries raw admin bytes.
 *   entropy_publish_json()       X:\Diag\entropy.json BlackBox report.
 *
 * All three run once on the Phase-3 boot path (boot_desktop.c) after
 * seed_file_phase3(), so they reflect the final boot entropy state.
 * BSP boot path only -- no SMP concerns by construction.
 * ============================================================================ */

#include "kernel/types.h"
#include "kernel/entropy.h"
#include "kernel/csprng.h"
#include "kernel/klog.h"
#include "kernel/mm/pmm.h"
#include "kernel/fs/vfs.h"
#include "kernel/util/json_builder.h"
#include "kernel/seed_file.h"
#include "registry.h"
#include "libc/string.h"
#include "libs/monocypher/monocypher.h"

#define ENTROPY_REG_PATH       "SYSTEM\\Boot\\Entropy"
/* Diagnostics MIRROR lives in a dedicated sub-key, never the same key the
 * admin drops the raw ExternalEntropy one-shot input into -- so a deferred
 * (not-yet-consumed) on-disk offering can never appear beside the
 * mask/class/counter mirror, and the mirror surface holds diagnostics only. */
#define ENTROPY_DIAG_PATH      "SYSTEM\\Boot\\Entropy\\Diagnostics"
#define ENTROPY_JSON_PATH      "X:\\Diag\\entropy.json"
#define ENTROPY_JSON_PAGES     1u
#define ENTROPY_JSON_SIZE      (ENTROPY_JSON_PAGES * 4096u)

/* One-shot consume bookkeeping for the diagnostics surfaces (length only,
 * never bytes). BSP boot path only. */
static uint32_t g_external_consumed_len;
static int      g_external_seen;

void entropy_external_consume(void)
{
    HKEY hKey = (HKEY)0;
    uint8_t stored[ENTROPY_EXTERNAL_MAX];
    uint8_t copy[ENTROPY_EXTERNAL_MAX];
    uint32_t type = 0, size = (uint32_t)sizeof(stored);
    uint32_t got;
    long rc;

    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, ENTROPY_REG_PATH, 0,
                     KEY_ALL_ACCESS, &hKey) != ERROR_SUCCESS)
        return;  /* no key -> nothing offered */

    rc = RegQueryValueEx(hKey, "ExternalEntropy", (uint32_t *)0,
                         &type, stored, &size);
    if (rc == ERROR_MORE_DATA) {
        /* Oversize offering: hard-reject (never truncate-consume), but
         * still destroy the in-memory value -- stale admin entropy must not
         * linger. A rejected offering is never absorbed, so on-disk
         * recovery-source residue is a non-issue here; durable on-disk
         * destroy of a rejected value lands with the secure one-shot
         * deletion in the registry hive-load wiring (TODO-14 Advanced Hive
         * Features). */
        klog(LOG_WARN, "entropy",
             "external entropy: %u bytes exceeds the %u-byte cap -- "
             "rejected and destroyed", (uint64_t)size,
             (uint64_t)ENTROPY_EXTERNAL_MAX);
        memset(stored, 0, sizeof(stored));
        RegSetValueEx(hKey, "ExternalEntropy", 0, REG_BINARY,
                      stored, (uint32_t)sizeof(stored));
        RegDeleteValue(hKey, "ExternalEntropy");
        g_external_seen = 1;
        RegCloseKey(hKey);
        return;
    }
    if (rc != ERROR_SUCCESS || type != REG_BINARY || size == 0) {
        /* RegQueryValueEx may have copied value bytes into `stored` before
         * this type/size check -- wipe them so a malformed (wrong-type)
         * offering never leaves seed material resident on the kernel stack.
         * The on-disk input value (if any) is left untouched: the diagnostics
         * mirror is a separate sub-key, so it is never polluted, and secure
         * removal of a malformed on-disk offering is the hive-load wiring's
         * job (TODO-14 Advanced Hive Features). */
        crypto_wipe(stored, sizeof(stored));
        RegCloseKey(hKey);
        return;
    }

    g_external_seen = 1;

    /* PURE one-shot core: copy out + wipe the source buffer. */
    got = entropy_external_oneshot(stored, size, copy,
                                   (uint32_t)sizeof(copy));

    /* SECURE-DELETE-BEFORE-ABSORB. A one-shot secret may be absorbed only
     * once its bytes cannot reappear from ANY source. With the current hive
     * format an on-disk offering survives a main-hive delete: hive_save()
     * backs the pre-delete hive up to <hive>.bak and recovery falls back to
     * it, so "delete + flush" does NOT erase the value -- a later main-hive
     * recovery reintroduces it and the one-shot is absorbed twice (entropy
     * reuse). Secure deletion across main + journal + .bak is owned by the
     * registry hive-load wiring (02-kernel-core/TODO-14 Advanced Hive
     * Features), the same work that activates the cross-reboot offering.
     * Until it lands, refuse any offering that could have an on-disk copy:
     * absorb ONLY when registry persistence is inactive (in-memory-only
     * value, no recovery source to retain it -- today's only live case).
     * This is the runtime half of the cross-reboot deferral, so wiring hive
     * load without the secure delete cannot silently re-enable reuse. */
    if (registry_persistence_active()) {
        /* DEFER, do not consume: leave the ExternalEntropy value untouched in
         * the live tree so a later registry_flush() preserves the on-disk copy
         * for the secure-deletion + hive-load wiring (deleting it here would be
         * serialized away by the next flush, losing the deferred offering). The
         * value never pollutes the diagnostics surface because that mirror is a
         * separate sub-key (ENTROPY_DIAG_PATH). Do NOT absorb, do NOT flush. */
        crypto_wipe(copy, sizeof(copy));
        RegCloseKey(hKey);
        klog(LOG_WARN, "entropy",
             "external entropy: on-disk one-shot deferred to hive-load "
             "wiring -- offering NOT absorbed");
        return;
    }

    /* In-memory-only path: overwrite the value with zeros and delete it
     * BEFORE absorbing the copy. No on-disk copy exists, so the delete is
     * fully effective -- no later reader (or re-entered consume) can see
     * the bytes. */
    RegSetValueEx(hKey, "ExternalEntropy", 0, REG_BINARY, stored, size);
    RegDeleteValue(hKey, "ExternalEntropy");
    RegCloseKey(hKey);

    /* Q_LOW: admin-supplied bytes are unverifiable -- they strengthen the
     * pool but never upgrade the credited class (no provenance launder). */
    csprng_add_entropy(copy, got, ENTROPY_Q_LOW);
    crypto_wipe(copy, sizeof(copy));
    g_external_consumed_len = got;

    klog(LOG_INFO, "entropy",
         "external entropy: %u bytes consumed (one-shot; in-memory value "
         "destroyed before use)", (uint64_t)got);
}

void entropy_populate_registry(void)
{
    HKEY hKey = (HKEY)0;
    uint32_t disp = 0;
    entropy_class_t diag_cls, credited;

    if (RegCreateKeyEx(HKEY_LOCAL_MACHINE, ENTROPY_DIAG_PATH, 0,
                       (const char *)0, 0, KEY_ALL_ACCESS, (void *)0,
                       &hKey, &disp) != ERROR_SUCCESS) {
        klog(LOG_WARN, "entropy",
             "failed to create HKLM\\" ENTROPY_DIAG_PATH " key");
        return;
    }

    diag_cls = entropy_classify(entropy_source_mask(),
                                entropy_source_quality());
    credited = csprng_credited_class();

    RegSetDword(hKey, "SourceMask",     entropy_source_mask());
    RegSetDword(hKey, "QualityPacked",  entropy_source_quality());
    RegSetString(hKey, "Class",         entropy_class_str(diag_cls));
    RegSetString(hKey, "CreditedClass", entropy_class_str(credited));
    RegSetDword(hKey, "CryptoGateOk",   (uint32_t)csprng_crypto_ok());
    RegSetDword(hKey, "SeedFileHwProvenance",
                (uint32_t)seed_file_hw_provenance());
    RegSetDword(hKey, "ExternalEntropySeen",  (uint32_t)g_external_seen);
    RegSetDword(hKey, "ExternalEntropyBytes", g_external_consumed_len);

    RegCloseKey(hKey);

    /* Boot-checkpoint line (kernel tests run BEFORE this point in the
     * Phase-3 sequence, so the registry mirror is serial-log-validated
     * rather than unit-tested -- mask/class only, never bytes). */
    klog(LOG_INFO, "entropy",
         "registry: HKLM\\SYSTEM\\Boot\\Entropy\\Diagnostics mask=0x%x class=%s "
         "credited=%s gate=%u",
         (uint64_t)entropy_source_mask(), entropy_class_str(diag_cls),
         entropy_class_str(credited), (uint64_t)csprng_crypto_ok());
}

void entropy_publish_json(void)
{
    uintptr_t phys = pmm_alloc_contiguous(ENTROPY_JSON_PAGES);
    struct json_builder jb;
    entropy_class_t diag_cls, credited;

    if (!phys) {
        klog(LOG_WARN, "entropy",
             "JSON: cannot alloc %u page(s) for entropy.json",
             (uint64_t)ENTROPY_JSON_PAGES);
        return;
    }

    diag_cls = entropy_classify(entropy_source_mask(),
                                entropy_source_quality());
    credited = csprng_credited_class();

    jb_init(&jb, (char *)phys, ENTROPY_JSON_SIZE);
    jb_putc(&jb, '{');
    jb_puts(&jb, "\"schema_version\":1,");
    jb_puts(&jb, "\"source_mask\":");
    jb_u32_dec(&jb, entropy_source_mask());
    jb_puts(&jb, ",\"quality_packed\":");
    jb_u32_dec(&jb, entropy_source_quality());
    jb_puts(&jb, ",\"class\":");
    jb_str(&jb, entropy_class_str(diag_cls));
    jb_puts(&jb, ",\"credited_class\":");
    jb_str(&jb, entropy_class_str(credited));
    jb_puts(&jb, ",\"crypto_gate_ok\":");
    jb_u32_dec(&jb, (uint32_t)csprng_crypto_ok());
    jb_puts(&jb, ",\"seed_file_hw_provenance\":");
    jb_u32_dec(&jb, (uint32_t)seed_file_hw_provenance());
    jb_puts(&jb, ",\"external_entropy\":{\"seen\":");
    jb_u32_dec(&jb, (uint32_t)g_external_seen);
    jb_puts(&jb, ",\"bytes_consumed\":");
    jb_u32_dec(&jb, g_external_consumed_len);
    jb_puts(&jb, "}}");

    if (jb_truncated(&jb)) {
        klog(LOG_WARN, "entropy", "JSON: entropy.json truncated -- skipped");
        pmm_free_frame(phys);
        return;
    }

    {
        struct vfs_node *f = vfs_open(ENTROPY_JSON_PATH,
                                      VFS_O_WRITE | VFS_O_CREATE |
                                      VFS_O_TRUNC);
        if (!f) {
            klog(LOG_WARN, "entropy",
                 "JSON: could not open %s for write", ENTROPY_JSON_PATH);
        } else {
            int wrote = vfs_write(f, 0, (uint32_t)jb_pos(&jb),
                                  (uint8_t *)jb_buf(&jb));
            vfs_close(f);
            if (wrote == (int)jb_pos(&jb)) {
                klog(LOG_INFO, "entropy", "JSON: wrote %s (%u bytes)",
                     ENTROPY_JSON_PATH, (uint64_t)jb_pos(&jb));
            } else {
                /* Same recovery contract as boot-health.json: TRUNC
                 * already zeroed the file, so a partial prefix is worse
                 * than an empty file -- truncate to empty best-effort. */
                klog(LOG_WARN, "entropy",
                     "JSON: short write to %s (wrote=%d, expected=%u); "
                     "truncating to empty", ENTROPY_JSON_PATH,
                     (uint64_t)wrote, (uint64_t)jb_pos(&jb));
                f = vfs_open(ENTROPY_JSON_PATH,
                             VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
                if (f)
                    vfs_close(f);
            }
        }
    }

    pmm_free_frame(phys);
}
