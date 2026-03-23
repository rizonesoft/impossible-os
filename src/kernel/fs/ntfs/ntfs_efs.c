/* ============================================================================
 * ntfs_efs.c — Encrypting File System (EFS) Support (§9.3)
 *
 * Implements:
 *   - $EFS attribute parser (type 0x100, $LOGGED_UTILITY_STREAM named "$EFS")
 *   - Data Decryption Field (DDF) decoder
 *   - FEK (File Encryption Key) cache (LRU, 16 entries per volume)
 *   - CNG crypto integration stubs (return NTFS_ERR_NOT_READY until CNG is
 *     implemented in TODO-305-CNG-Crypto)
 *   - Transparent read/write paths for encrypted file data
 *
 * EFS on-disk format (Windows):
 *   - $DATA attribute has NTFS_ATTR_FLAG_ENCRYPTED (0x4000)
 *   - $LOGGED_UTILITY_STREAM (0x100) named "$EFS" contains:
 *     * EFS header: version, DDF offset/count, DRF offset/count
 *     * DDF array: per-user { cert thumbprint, RSA-wrapped FEK }
 *     * DRF array: recovery agent { cert thumbprint, RSA-wrapped FEK }
 *   - File data is AES-256 encrypted with the FEK
 *   - Mutual exclusion: compressed + encrypted is FORBIDDEN by NTFS
 *
 * → XREF: TODO-305-CNG-Crypto §3.1 — EFS integration glue (cng_efs_*)
 * ============================================================================ */

#include "kernel/fs/ntfs.h"
#include "kernel/fs/ntfs_internal.h"
#include "kernel/mm/heap.h"
#include "kernel/klog.h"

/* ============================================================================
 * FEK Cache — per-volume LRU cache for decrypted File Encryption Keys
 *
 * Avoids repeated RSA unwrap for the same file. Limited to 16 entries;
 * LRU eviction when full. Access counter used for eviction ordering.
 * ============================================================================ */

/* Static FEK cache (one per volume — single volume for now) */
static struct ntfs_efs_fek_entry efs_fek_cache[NTFS_EFS_FEK_CACHE_SIZE];
static uint64_t efs_cache_access_counter = 0;

int ntfs_efs_cache_lookup(struct ntfs_volume *vol, uint64_t inode,
                          uint8_t *fek_out)
{
    int i;
    (void)vol;

    for (i = 0; i < NTFS_EFS_FEK_CACHE_SIZE; i++) {
        if (efs_fek_cache[i].valid && efs_fek_cache[i].inode == inode) {
            efs_fek_cache[i].last_access = ++efs_cache_access_counter;
            ntfs_memcpy(fek_out, efs_fek_cache[i].fek, NTFS_EFS_FEK_LEN);
            return NTFS_OK;
        }
    }
    return NTFS_ERR_NOT_FOUND;
}

void ntfs_efs_cache_store(struct ntfs_volume *vol, uint64_t inode,
                          const uint8_t *fek)
{
    int i;
    int lru_idx = 0;
    uint64_t lru_min = (uint64_t)-1;
    (void)vol;

    /* Check if already cached (update in place) */
    for (i = 0; i < NTFS_EFS_FEK_CACHE_SIZE; i++) {
        if (efs_fek_cache[i].valid && efs_fek_cache[i].inode == inode) {
            ntfs_memcpy(efs_fek_cache[i].fek, fek, NTFS_EFS_FEK_LEN);
            efs_fek_cache[i].last_access = ++efs_cache_access_counter;
            return;
        }
    }

    /* Find empty slot or LRU victim */
    for (i = 0; i < NTFS_EFS_FEK_CACHE_SIZE; i++) {
        if (!efs_fek_cache[i].valid) {
            lru_idx = i;
            break;
        }
        if (efs_fek_cache[i].last_access < lru_min) {
            lru_min = efs_fek_cache[i].last_access;
            lru_idx = i;
        }
    }

    /* Store */
    efs_fek_cache[lru_idx].inode = inode;
    ntfs_memcpy(efs_fek_cache[lru_idx].fek, fek, NTFS_EFS_FEK_LEN);
    efs_fek_cache[lru_idx].valid = 1;
    efs_fek_cache[lru_idx].last_access = ++efs_cache_access_counter;
}

void ntfs_efs_cache_evict(struct ntfs_volume *vol, uint64_t inode)
{
    int i;
    (void)vol;

    for (i = 0; i < NTFS_EFS_FEK_CACHE_SIZE; i++) {
        if (efs_fek_cache[i].valid && efs_fek_cache[i].inode == inode) {
            /* Zero out the FEK for security before invalidating */
            ntfs_memset(efs_fek_cache[i].fek, 0, NTFS_EFS_FEK_LEN);
            efs_fek_cache[i].valid = 0;
            return;
        }
    }
}

/* ============================================================================
 * $EFS Attribute Parser
 *
 * Windows EFS attribute layout (inside $LOGGED_UTILITY_STREAM named "$EFS"):
 *
 *   Offset  Size  Field
 *   0x00    4     Total length of EFS attribute data
 *   0x04    4     Reserved (0)
 *   0x08    4     EFS version (2 for Win2K, 3 for XP+)
 *   0x0C    4     Number of DDF entries
 *   0x10    4     Offset to DDF array (from attribute data start)
 *   0x14    4     Number of DRF entries (recovery agents)
 *   0x18    4     Offset to DRF array (from attribute data start)
 *   0x1C    ...   Reserved / padding
 *
 * Each DDF entry:
 *   0x00    4     Total length of this DDF entry
 *   0x04    4     Version (1)
 *   0x08    4     SID offset (from DDF entry start)
 *   0x0C    4     SID length
 *   0x10    4     Certificate thumbprint offset (from DDF entry start)
 *   0x14    4     Certificate thumbprint length
 *   0x18    4     Encrypted FEK offset (from DDF entry start)
 *   0x1C    4     Encrypted FEK length
 * ============================================================================ */

/* Minimum EFS header size */
#define EFS_HDR_MIN_SIZE  0x1C

/* DDF entry header minimum size */
#define EFS_DDF_HDR_SIZE  0x20

int ntfs_efs_parse(const uint8_t *record, const struct ntfs_mft_header *hdr,
                   struct ntfs_efs_info *out)
{
    struct ntfs_attr_header ah;
    const uint8_t *attr;
    const uint8_t *data;
    uint32_t data_len;
    uint32_t ddf_offset;
    uint32_t ddf_count;
    uint32_t i;
    const uint8_t *ddf_ptr;

    if (!record || !hdr || !out)
        return NTFS_ERR_IO;

    ntfs_memset(out, 0, sizeof(*out));

    /* Find $LOGGED_UTILITY_STREAM (type 0x100) named "$EFS" */
    attr = ntfs_attr_find_named(record, hdr,
                                NTFS_ATTR_LOGGED_UTILITY_STREAM,
                                "$EFS", &ah);
    if (!attr) {
        /* No $EFS attribute — file is not EFS-encrypted */
        return NTFS_ERR_NOT_FOUND;
    }

    /* $EFS should be resident (stored inline in MFT record) */
    if (ah.non_resident != 0) {
        klog(LOG_WARN, "ntfs", "EFS: $EFS attribute is non-resident "
             "(unsupported, need extension record read)");
        return NTFS_ERR_NOT_FOUND;
    }

    data = attr + ah.content_offset;
    data_len = ah.content_length;

    if (data_len < EFS_HDR_MIN_SIZE) {
        klog(LOG_WARN, "ntfs", "EFS: $EFS attribute too small (%u bytes)",
             (uint64_t)data_len);
        return NTFS_ERR_BAD_MAGIC;
    }

    /* Parse EFS header */
    out->version   = ntfs_le32(data + 0x08);
    ddf_count      = ntfs_le32(data + 0x0C);
    ddf_offset     = ntfs_le32(data + 0x10);
    out->raw_data  = data;
    out->raw_length = data_len;

    klog(LOG_DEBUG, "ntfs", "EFS: version=%u ddf_count=%u ddf_off=0x%x",
         (uint64_t)out->version, (uint64_t)ddf_count, (uint64_t)ddf_offset);

    /* Validate DDF offset */
    if (ddf_offset >= data_len || ddf_count == 0) {
        klog(LOG_DEBUG, "ntfs", "EFS: no DDF entries (count=%u off=0x%x)",
             (uint64_t)ddf_count, (uint64_t)ddf_offset);
        out->ddf_count = 0;
        return NTFS_OK;
    }

    /* Cap DDF count to our maximum */
    if (ddf_count > NTFS_EFS_MAX_DDF)
        ddf_count = NTFS_EFS_MAX_DDF;
    out->ddf_count = ddf_count;

    /* Parse DDF entries */
    ddf_ptr = data + ddf_offset;
    for (i = 0; i < ddf_count; i++) {
        uint32_t entry_len;
        uint32_t thumb_off, thumb_len;
        uint32_t fek_off, fek_len;
        uint32_t sid_off, sid_len;
        uint32_t ddf_base;

        ddf_base = (uint32_t)(ddf_ptr - data);
        if (ddf_base + EFS_DDF_HDR_SIZE > data_len)
            break;

        entry_len = ntfs_le32(ddf_ptr + 0x00);
        if (entry_len < EFS_DDF_HDR_SIZE || ddf_base + entry_len > data_len)
            break;

        sid_off   = ntfs_le32(ddf_ptr + 0x08);
        sid_len   = ntfs_le32(ddf_ptr + 0x0C);
        thumb_off = ntfs_le32(ddf_ptr + 0x10);
        thumb_len = ntfs_le32(ddf_ptr + 0x14);
        fek_off   = ntfs_le32(ddf_ptr + 0x18);
        fek_len   = ntfs_le32(ddf_ptr + 0x1C);

        /* Store SID info */
        out->ddfs[i].sid_offset = ddf_base + sid_off;
        out->ddfs[i].sid_length = sid_len;

        /* Store thumbprint — copy up to 20 bytes (SHA-1) */
        if (thumb_off + thumb_len <= entry_len &&
            thumb_len <= NTFS_EFS_THUMB_LEN) {
            ntfs_memcpy(out->ddfs[i].cert_thumbprint,
                        ddf_ptr + thumb_off, thumb_len);
        }

        /* Store encrypted FEK location */
        out->ddfs[i].encrypted_fek_offset = ddf_base + fek_off;
        out->ddfs[i].encrypted_fek_length = fek_len;

        klog(LOG_DEBUG, "ntfs",
             "EFS: DDF[%u] thumb_len=%u fek_off=0x%x fek_len=%u",
             (uint64_t)i, (uint64_t)thumb_len,
             (uint64_t)(ddf_base + fek_off), (uint64_t)fek_len);

        ddf_ptr += entry_len;
    }

    return NTFS_OK;
}

int ntfs_efs_find_ddf(const struct ntfs_efs_info *info,
                      const uint8_t *thumbprint,
                      struct ntfs_efs_ddf *out_ddf)
{
    uint32_t i;

    if (!info || !thumbprint || !out_ddf)
        return NTFS_ERR_IO;

    for (i = 0; i < info->ddf_count; i++) {
        if (ntfs_memcmp(info->ddfs[i].cert_thumbprint,
                        thumbprint, NTFS_EFS_THUMB_LEN) == 0) {
            *out_ddf = info->ddfs[i];
            return NTFS_OK;
        }
    }

    return NTFS_ERR_ACCESS_DENIED;
}

/* ============================================================================
 * CNG Crypto Stubs
 *
 * These functions form the integration boundary with TODO-305-CNG-Crypto.
 * They are properly typed and wired into the read/write paths. When CNG
 * primitives are implemented (AES-256, RSA-2048, cert store), these stubs
 * will be replaced with real crypto calls:
 *
 *   ntfs_efs_unwrap_fek  → cng_efs_get_fek()    (RSA OAEP decrypt)
 *   ntfs_efs_decrypt_data → cng_efs_decrypt_data() (AES-256 decrypt)
 *   ntfs_efs_encrypt_data → cng_efs_encrypt_data() (AES-256 encrypt)
 * ============================================================================ */

int ntfs_efs_unwrap_fek(const struct ntfs_efs_ddf *ddf,
                        const uint8_t *efs_raw,
                        uint8_t *fek_out)
{
    /* TODO: Replace with cng_efs_get_fek() when CNG is implemented.
     *
     * Real implementation:
     *   1. Locate user's private key via cert thumbprint in CNG key store
     *   2. RSA-OAEP decrypt the wrapped FEK blob at
     *      efs_raw + ddf->encrypted_fek_offset
     *   3. Extract 32-byte AES-256 FEK from the decrypted blob
     */
    (void)ddf;
    (void)efs_raw;
    (void)fek_out;

    klog(LOG_WARN, "ntfs",
         "EFS: cannot unwrap FEK — CNG key store not initialized "
         "(→ TODO-305-CNG-Crypto §1.3)");
    return NTFS_ERR_NOT_READY;
}

int ntfs_efs_decrypt_data(const uint8_t *fek, void *buf, uint64_t len)
{
    /* TODO: Replace with cng_efs_decrypt_data() when CNG is implemented.
     *
     * Real implementation:
     *   1. AES-256-CBC or AES-256-XTS decrypt buf in-place
     *   2. Block size = 16 bytes (AES block)
     *   3. IV derived from file offset (sector-based)
     */
    (void)fek;
    (void)buf;
    (void)len;

    klog(LOG_WARN, "ntfs",
         "EFS: cannot decrypt — CNG not initialized "
         "(→ TODO-305-CNG-Crypto §1.1)");
    return NTFS_ERR_NOT_READY;
}

int ntfs_efs_encrypt_data(const uint8_t *fek, void *buf, uint64_t len)
{
    /* TODO: Replace with cng_efs_encrypt_data() when CNG is implemented.
     *
     * Real implementation:
     *   1. AES-256-CBC or AES-256-XTS encrypt buf in-place
     *   2. Block size = 16 bytes (AES block)
     *   3. IV derived from file offset (sector-based)
     */
    (void)fek;
    (void)buf;
    (void)len;

    klog(LOG_WARN, "ntfs",
         "EFS: cannot encrypt — CNG not initialized "
         "(→ TODO-305-CNG-Crypto §1.1)");
    return NTFS_ERR_NOT_READY;
}

/* ============================================================================
 * ntfs_efs_add_user — Add authorized user to $EFS DDF list
 * ============================================================================ */

int ntfs_efs_add_user(struct ntfs_volume *vol, uint64_t inode,
                      const uint8_t *cert_thumbprint)
{
    /* TODO: When CNG is implemented:
     *   1. Read file's MFT record
     *   2. Parse existing $EFS attribute
     *   3. Lookup user's cert by thumbprint in CNG cert store
     *   4. Unwrap existing FEK (need one existing DDF to decrypt)
     *   5. RSA-OAEP wrap FEK with new user's public key
     *   6. Build new DDF entry { thumbprint, SID, wrapped FEK }
     *   7. Append to DDF array in $EFS attribute
     *   8. Write updated $EFS via ntfs_attr_update()
     */
    (void)vol;
    (void)inode;
    (void)cert_thumbprint;

    klog(LOG_WARN, "ntfs",
         "EFS: cannot add user — CNG not initialized "
         "(→ TODO-305-CNG-Crypto §1.2)");
    return NTFS_ERR_NOT_READY;
}

/* ============================================================================
 * Encrypted Data Read/Write Paths
 *
 * These integrate with ntfs_read_file_data() and ntfs_write_data() to
 * provide transparent encryption/decryption for EFS files.
 * ============================================================================ */

int64_t ntfs_read_encrypted_data(struct ntfs_volume *vol,
                                  const uint8_t *record,
                                  const struct ntfs_mft_header *hdr,
                                  const struct ntfs_data_run *runs,
                                  int run_count,
                                  uint64_t real_size,
                                  uint64_t file_offset,
                                  uint64_t length,
                                  void *buffer)
{
    struct ntfs_efs_info efs_info;
    int64_t bytes_read;
    int rc;

    if (!vol || !record || !hdr || !runs || !buffer)
        return -1;

    /* Step 1: Read raw (encrypted) data from disk using normal data reader */
    bytes_read = ntfs_read_data(vol, runs, run_count, real_size,
                                file_offset, length, buffer);
    if (bytes_read <= 0)
        return bytes_read;

    /* Step 2: Parse $EFS attribute to get encryption info */
    rc = ntfs_efs_parse(record, hdr, &efs_info);
    if (rc != NTFS_OK) {
        klog(LOG_WARN, "ntfs",
             "EFS: encrypted flag set but no $EFS attribute found");
        return -1;
    }

    /* Step 3: Try to get FEK (cached or unwrap via CNG) */
    {
        uint8_t fek[NTFS_EFS_FEK_LEN];

        /* Check FEK cache first */
        /* Note: We don't have the inode number here, so we can't use the
         * cache from this path. The VFS layer (ntfs_vfs.c) should cache
         * at a higher level. For now, attempt unwrap directly. */

        /* Try to unwrap FEK from the first DDF entry.
         * (Real implementation would match user's cert thumbprint) */
        if (efs_info.ddf_count == 0) {
            klog(LOG_WARN, "ntfs", "EFS: no DDF entries — no authorized users");
            return -1;
        }

        rc = ntfs_efs_unwrap_fek(&efs_info.ddfs[0], efs_info.raw_data, fek);
        if (rc == NTFS_ERR_NOT_READY) {
            /* CNG not available — return raw encrypted data with warning */
            klog(LOG_WARN, "ntfs",
                 "EFS: returning raw encrypted data (CNG not ready)");
            return -1;
        }
        if (rc != NTFS_OK)
            return -1;

        /* Step 4: Decrypt the data in-place */
        rc = ntfs_efs_decrypt_data(fek, buffer, (uint64_t)bytes_read);

        /* Zero out FEK from stack for security */
        ntfs_memset(fek, 0, NTFS_EFS_FEK_LEN);

        if (rc != NTFS_OK)
            return -1;
    }

    return bytes_read;
}

int ntfs_write_encrypted_data(struct ntfs_volume *vol, uint64_t inode,
                               uint64_t offset, uint64_t length,
                               const void *buffer)
{
    uint8_t fek[NTFS_EFS_FEK_LEN];
    int rc;

    if (!vol || !buffer || length == 0)
        return NTFS_ERR_IO;

    /* Step 1: Check FEK cache */
    rc = ntfs_efs_cache_lookup(vol, inode, fek);
    if (rc != NTFS_OK) {
        /* Need to read MFT record and unwrap FEK */
        uint8_t *rec;
        struct ntfs_mft_header hdr;
        struct ntfs_efs_info efs_info;

        rec = (uint8_t *)kmalloc(vol->frs_size);
        if (!rec)
            return NTFS_ERR_IO;

        rc = ntfs_read_mft_record(vol, inode, rec, &hdr);
        if (rc != NTFS_OK) {
            kfree(rec);
            return rc;
        }

        rc = ntfs_efs_parse(rec, &hdr, &efs_info);
        if (rc != NTFS_OK) {
            kfree(rec);
            klog(LOG_WARN, "ntfs",
                 "EFS write: encrypted flag set but no $EFS attribute");
            return NTFS_ERR_NOT_FOUND;
        }

        if (efs_info.ddf_count == 0) {
            kfree(rec);
            klog(LOG_WARN, "ntfs", "EFS write: no DDF entries");
            return NTFS_ERR_ACCESS_DENIED;
        }

        rc = ntfs_efs_unwrap_fek(&efs_info.ddfs[0], efs_info.raw_data, fek);
        kfree(rec);

        if (rc != NTFS_OK) {
            ntfs_memset(fek, 0, NTFS_EFS_FEK_LEN);
            return rc;
        }

        /* Cache the FEK for future writes */
        ntfs_efs_cache_store(vol, inode, fek);
    }

    /* Step 2: Encrypt data copy and write to disk */
    {
        uint8_t *enc_buf;

        enc_buf = (uint8_t *)kmalloc((uint32_t)length);
        if (!enc_buf) {
            ntfs_memset(fek, 0, NTFS_EFS_FEK_LEN);
            return NTFS_ERR_IO;
        }

        ntfs_memcpy(enc_buf, buffer, length);

        rc = ntfs_efs_encrypt_data(fek, enc_buf, length);
        if (rc != NTFS_OK) {
            kfree(enc_buf);
            ntfs_memset(fek, 0, NTFS_EFS_FEK_LEN);
            return rc;
        }

        /* Write encrypted data using the normal write engine */
        rc = ntfs_write_data(vol, inode, offset, length, enc_buf);
        kfree(enc_buf);
    }

    ntfs_memset(fek, 0, NTFS_EFS_FEK_LEN);
    return rc;
}
