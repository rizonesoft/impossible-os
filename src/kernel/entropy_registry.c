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
 *                                launder provenance. DORMANT across
 *                                reboots until the registry hive load is
 *                                wired at boot (owned by the registry
 *                                completion TODO); the consume path is
 *                                live for any value present in the boot
 *                                registry tree.
 *   entropy_populate_registry()  HKLM\SYSTEM\Boot\Entropy mirror.
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
         * still destroy it -- stale admin entropy must not linger. */
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
        RegCloseKey(hKey);
        return;
    }

    g_external_seen = 1;

    /* PURE one-shot core: copy out + wipe the source buffer. */
    got = entropy_external_oneshot(stored, size, copy,
                                   (uint32_t)sizeof(copy));

    /* OVERWRITE-BEFORE-USE: store the zeroed buffer over the registry
     * value and delete it BEFORE absorbing the copy -- after this point
     * no registry reader (or a re-entered consume) can see the bytes. */
    RegSetValueEx(hKey, "ExternalEntropy", 0, REG_BINARY, stored, size);
    RegDeleteValue(hKey, "ExternalEntropy");
    RegCloseKey(hKey);

    /* Q_LOW: admin-supplied bytes are unverifiable -- they strengthen the
     * pool but never upgrade the credited class (no provenance launder). */
    csprng_add_entropy(copy, got, ENTROPY_Q_LOW);
    crypto_wipe(copy, sizeof(copy));
    g_external_consumed_len = got;

    klog(LOG_INFO, "entropy",
         "external entropy: %u bytes consumed (one-shot; value destroyed "
         "before use)", (uint64_t)got);
}

void entropy_populate_registry(void)
{
    HKEY hKey = (HKEY)0;
    uint32_t disp = 0;
    entropy_class_t diag_cls, credited;

    if (RegCreateKeyEx(HKEY_LOCAL_MACHINE, ENTROPY_REG_PATH, 0,
                       (const char *)0, 0, KEY_ALL_ACCESS, (void *)0,
                       &hKey, &disp) != ERROR_SUCCESS) {
        klog(LOG_WARN, "entropy",
             "failed to create HKLM\\" ENTROPY_REG_PATH " key");
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
    RegSetDword(hKey, "ExternalEntropySeen",  (uint32_t)g_external_seen);
    RegSetDword(hKey, "ExternalEntropyBytes", g_external_consumed_len);

    RegCloseKey(hKey);

    /* Boot-checkpoint line (kernel tests run BEFORE this point in the
     * Phase-3 sequence, so the registry mirror is serial-log-validated
     * rather than unit-tested -- mask/class only, never bytes). */
    klog(LOG_INFO, "entropy",
         "registry: HKLM\\SYSTEM\\Boot\\Entropy mask=0x%x class=%s "
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
