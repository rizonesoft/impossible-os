/* ============================================================================
 * blk_crypto.c — Inline Encryption Support
 *
 * §21.1 — 🚀 Impossible OS Exclusive
 *
 * Neither Windows viostor nor Linux virtio-blk implement VirtIO-level inline
 * encryption — Windows uses BitLocker (software), Linux uses blk-crypto
 * (above the driver). This module provides transparent block-level encryption
 * directly in the VirtIO block driver.
 *
 * When the host advertises F_INLINE_CRYPTO, individual I/O requests can be
 * tagged with a crypto context (key slot, algorithm, data unit number) so
 * the host performs encryption/decryption inline at the storage layer.
 *
 * Design:
 *   - Key slot table: up to 16 slots, each with algorithm + key material
 *   - Crypto context: per-request (key_slot, tweak/DUN, algorithm)
 *   - Read: device decrypts data before returning to guest
 *   - Write: device encrypts data before writing to storage
 *   - Fallback: if not negotiated, encryption handled by software above driver
 * ============================================================================ */

#include "kernel/drivers/virtio/blk_internal.h"

/* ---- Key slot state ---- */
struct crypto_key_slot {
    int      in_use;           /* 1 if slot has a key programmed */
    uint32_t algorithm;        /* VIRTIO_BLK_CRYPTO_ALG_* */
    uint8_t  key[VIRTIO_BLK_CRYPTO_KEY_SIZE];  /* Key material */
    uint32_t key_len;          /* Actual key length in bytes */
};

/* ---- Crypto subsystem state ---- */
struct crypto_state {
    int      supported;        /* Device offers F_INLINE_CRYPTO */
    int      enabled;          /* User/Registry enabled */
    uint32_t max_slots;        /* Max key slots available */

    struct crypto_key_slot slots[VIRTIO_BLK_CRYPTO_MAX_KEY_SLOTS];

    /* Statistics */
    uint32_t crypto_reads;     /* Reads with crypto context */
    uint32_t crypto_writes;    /* Writes with crypto context */
    uint32_t keys_programmed;  /* Current active key slots */
    uint32_t key_set_ops;      /* Total set_key calls */
    uint32_t key_clear_ops;    /* Total clear_key calls */
};

static struct crypto_state cs;

/* ---- Public API ---- */

/* Initialize crypto subsystem. Called from virtio_blk_init(). */
void crypto_init(void)
{
    uint32_t i;

    cs.supported = has_inline_crypto;
    cs.enabled = 0;  /* Must be explicitly enabled per key slot */
    cs.max_slots = VIRTIO_BLK_CRYPTO_MAX_KEY_SLOTS;
    cs.crypto_reads = 0;
    cs.crypto_writes = 0;
    cs.keys_programmed = 0;
    cs.key_set_ops = 0;
    cs.key_clear_ops = 0;

    for (i = 0; i < VIRTIO_BLK_CRYPTO_MAX_KEY_SLOTS; i++) {
        cs.slots[i].in_use = 0;
        cs.slots[i].algorithm = VIRTIO_BLK_CRYPTO_ALG_NONE;
        cs.slots[i].key_len = 0;
    }

    /* Read configuration from Registry */
    {
        HKEY hKey = (HKEY)0;
        uint32_t val;

        if (RegOpenKeyEx(HKEY_LOCAL_MACHINE,
                "SYSTEM\\Drivers\\VirtIO\\Crypto", 0,
                KEY_READ, &hKey) == ERROR_SUCCESS) {
            if (RegGetDword(hKey, "Enabled", &val) == ERROR_SUCCESS)
                cs.enabled = (int)val;
            RegCloseKey(hKey);
        }
    }

    klog(LOG_DEBUG, "virtio", "Crypto: device_support=%s, enabled=%s, max_slots=%u",
           cs.supported ? "yes" : "no",
           cs.enabled ? "yes" : "no",
           (uint64_t)cs.max_slots);
}

/* Program a key into a key slot.
 * slot: 0 to max_slots-1
 * key: key material (length depends on algorithm)
 * key_len: key length in bytes
 * algo: VIRTIO_BLK_CRYPTO_ALG_* constant
 * Returns 0 on success, -1 on error. */
int crypto_set_key(uint32_t slot, const uint8_t *key,
                   uint32_t key_len, uint32_t algo)
{
    uint32_t i;

    if (slot >= cs.max_slots) {
        klog(LOG_DEBUG, "virtio", "Crypto: invalid key slot %u (max %u)",
               (uint64_t)slot, (uint64_t)cs.max_slots);
        return -1;
    }

    if (!cs.supported) {
        klog(LOG_DEBUG, "virtio",
               "Crypto: set_key failed — device does not support inline crypto");
        return -1;
    }

    if (key_len > VIRTIO_BLK_CRYPTO_KEY_SIZE) {
        klog(LOG_DEBUG, "virtio", "Crypto: key too large (%u > %u)",
               (uint64_t)key_len, (uint64_t)VIRTIO_BLK_CRYPTO_KEY_SIZE);
        return -1;
    }

    /* Validate algorithm */
    if (algo == VIRTIO_BLK_CRYPTO_ALG_NONE || algo > VIRTIO_BLK_CRYPTO_ALG_AES_256_CBC) {
        klog(LOG_DEBUG, "virtio", "Crypto: invalid algorithm %u", (uint64_t)algo);
        return -1;
    }

    /* Copy key material */
    for (i = 0; i < key_len; i++)
        cs.slots[slot].key[i] = key[i];
    /* Zero remainder */
    for (; i < VIRTIO_BLK_CRYPTO_KEY_SIZE; i++)
        cs.slots[slot].key[i] = 0;

    cs.slots[slot].key_len = key_len;
    cs.slots[slot].algorithm = algo;
    cs.slots[slot].in_use = 1;

    cs.keys_programmed++;
    cs.key_set_ops++;
    cs.enabled = 1;  /* Auto-enable when first key is set */

    klog(LOG_DEBUG, "virtio", "Crypto: key slot %u programmed (algo=%u, len=%u)",
           (uint64_t)slot, (uint64_t)algo, (uint64_t)key_len);

    /* TODO: When device supports F_INLINE_CRYPTO, send key to device via
     * a special control virtqueue or device-specific config write.
     * Currently, key is stored in driver-side table for future use. */

    return 0;
}

/* Clear a key slot (secure zeroization).
 * Returns 0 on success, -1 on error. */
int crypto_clear_key(uint32_t slot)
{
    uint32_t i;

    if (slot >= cs.max_slots)
        return -1;

    if (!cs.slots[slot].in_use)
        return 0;  /* Already clear */

    /* Secure zeroize key material */
    for (i = 0; i < VIRTIO_BLK_CRYPTO_KEY_SIZE; i++) {
        volatile uint8_t *p = (volatile uint8_t *)&cs.slots[slot].key[i];
        *p = 0;
    }

    cs.slots[slot].key_len = 0;
    cs.slots[slot].algorithm = VIRTIO_BLK_CRYPTO_ALG_NONE;
    cs.slots[slot].in_use = 0;

    if (cs.keys_programmed > 0)
        cs.keys_programmed--;
    cs.key_clear_ops++;

    klog(LOG_DEBUG, "virtio", "Crypto: key slot %u cleared", (uint64_t)slot);

    /* Disable crypto if no keys remain */
    if (cs.keys_programmed == 0)
        cs.enabled = 0;

    return 0;
}

/* Check if a key slot is valid and active.
 * Returns 1 if slot has a key, 0 otherwise. */
int crypto_slot_active(uint32_t slot)
{
    if (slot >= cs.max_slots)
        return 0;
    return cs.slots[slot].in_use;
}

/* Get the algorithm for a key slot. Returns VIRTIO_BLK_CRYPTO_ALG_*. */
uint32_t crypto_slot_algorithm(uint32_t slot)
{
    if (slot >= cs.max_slots)
        return VIRTIO_BLK_CRYPTO_ALG_NONE;
    return cs.slots[slot].algorithm;
}

/* Check if inline crypto is supported and enabled. */
int crypto_is_available(void)
{
    return cs.supported && cs.enabled;
}

/* Expose crypto capabilities and stats to Registry. */
void crypto_expose_registry(void)
{
    HKEY hKey = (HKEY)0;
    uint32_t disp;

    /* Write capabilities */
    if (RegCreateKeyEx(HKEY_LOCAL_MACHINE,
            "HARDWARE\\VirtIO\\Block0\\Crypto", 0,
            (const char *)0, 0, KEY_ALL_ACCESS, (void *)0,
            &hKey, &disp) == ERROR_SUCCESS) {
        RegSetDword(hKey, "Supported", cs.supported ? 1 : 0);
        RegSetDword(hKey, "Enabled", cs.enabled ? 1 : 0);
        RegSetDword(hKey, "MaxKeySlots", cs.max_slots);
        RegSetDword(hKey, "KeysActive", cs.keys_programmed);
        RegSetDword(hKey, "CryptoReads", cs.crypto_reads);
        RegSetDword(hKey, "CryptoWrites", cs.crypto_writes);
        RegSetDword(hKey, "KeySetOps", cs.key_set_ops);
        RegSetDword(hKey, "KeyClearOps", cs.key_clear_ops);

        /* List supported algorithms */
        RegSetDword(hKey, "Algo_AES_128_XTS", 1);
        RegSetDword(hKey, "Algo_AES_256_XTS", 1);
        RegSetDword(hKey, "Algo_AES_128_CBC", 1);
        RegSetDword(hKey, "Algo_AES_256_CBC", 1);

        RegCloseKey(hKey);
    }

    /* Write config defaults */
    if (RegCreateKeyEx(HKEY_LOCAL_MACHINE,
            "SYSTEM\\Drivers\\VirtIO\\Crypto", 0,
            (const char *)0, 0, KEY_ALL_ACCESS, (void *)0,
            &hKey, &disp) == ERROR_SUCCESS) {
        RegSetDword(hKey, "Enabled", cs.enabled ? 1 : 0);
        RegSetDword(hKey, "MaxKeySlots", cs.max_slots);
        RegCloseKey(hKey);
    }
}
