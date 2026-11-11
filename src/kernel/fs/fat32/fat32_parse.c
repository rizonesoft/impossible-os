/* ============================================================================
 * fat32_parse.c -- FAT32 on-disk structure decoding
 *
 * Every decision the driver makes from bytes on the disk before it trusts
 * them: the boot sector and BPB, the FSInfo sector, 8.3 names and long-name
 * runs. It holds no volume state and does no I/O, so tools/fuzz builds it for
 * the host and drives it under AddressSanitizer with nothing but klog.
 * ============================================================================ */

#include "fat32_internal.h"
#include "kernel/le_bytes.h"

/* ---- Names ---- */

void fat32_strcpy(char *dst, const char *src, uint32_t max)
{
    uint32_t i;
    for (i = 0; i < max - 1 && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

int fat32_strcasecmp(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (ca != cb)
            return 0;
        a++;
        b++;
    }
    return *a == *b;
}

uint8_t fat32_lfn_checksum(const uint8_t sfn[11])
{
    uint8_t sum = 0;
    int i;
    for (i = 0; i < 11; i++)
        sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + sfn[i]);
    return sum;
}

void fat32_short_name_to_str(const uint8_t *raw, char *out)
{
    int i, j = 0;

    for (i = 0; i < 8; i++) {
        if (raw[i] != ' ')
            out[j++] = (char)(raw[i] >= 'A' && raw[i] <= 'Z'
                        ? raw[i] + 32 : raw[i]);
    }

    if (raw[8] != ' ') {
        out[j++] = '.';
        for (i = 8; i < 11; i++) {
            if (raw[i] != ' ')
                out[j++] = (char)(raw[i] >= 'A' && raw[i] <= 'Z'
                            ? raw[i] + 32 : raw[i]);
        }
    }

    out[j] = '\0';
}

/* Copy one LFN slot's characters into a FAT32_MAX_NAME buffer at the position its
 * sequence number gives. That number comes from the disk, so a slot reaching past the
 * last character position stops there: the final byte stays free for the terminator. */
void lfn_extract_chars(const struct fat32_lfn_entry *lfn,
                       char *name_buf, int seq_index)
{
    int pos = seq_index * FAT32_LFN_CHARS;
    int i;

    if (seq_index < 0)
        return;

    for (i = 0; i < FAT32_LFN_CHARS; i++, pos++) {
        uint16_t ch = (i < 5) ? lfn->name1[i]
                    : (i < 11) ? lfn->name2[i - 5]
                    : lfn->name3[i - 11];

        if (ch == 0 || ch == 0xFFFF || pos >= FAT32_MAX_NAME - 1)
            return;
        name_buf[pos] = (ch < 128) ? (char)ch : '_';
    }
}

/* ---- BPB strict validation (Microsoft FAT spec S3) ---- */

/* Suppresses LOG_ERROR diagnostics inside fat32_validate_bpb when set.
 * Negative-test code in test_ixfs.c constructs deliberately-malformed
 * BPBs and verifies the validator rejects them; the validator's klog
 * was rendering as [FAIL] in the boot log even though the test was
 * passing. Test code wraps validate calls with set(1) / set(0). */
static int s_validate_quiet = 0;

void fat32_validate_set_quiet(int q) { s_validate_quiet = q ? 1 : 0; }

#define FAT32_BPB_FAIL(fmt, ...) do { \
    if (!s_validate_quiet) \
        klog(LOG_ERROR, "fat32", fmt, ##__VA_ARGS__); \
} while (0)

int fat32_bpb_check(const struct fat32_bpb *bpb, const uint8_t *boot)
{
    uint8_t jmp  = boot[0];
    uint16_t bps = bpb->bytes_per_sector;
    uint8_t spc  = bpb->sectors_per_cluster;
    uint8_t nf   = bpb->num_fats;
    uint16_t rec = le16_at(boot + 17);  /* RootEntCnt */
    uint32_t rc  = bpb->root_cluster;
    uint32_t ts  = bpb->total_sectors;

    /* jmpBoot[0] must be 0xEB (short jump) or 0xE9 (near jump) */
    if (jmp != 0xEB && jmp != 0xE9) {
        FAT32_BPB_FAIL("BPB validation failed: bad jmpBoot 0x%02X",
             (uint64_t)jmp);
        return -1;
    }

    /* BytsPerSec: the FAT spec allows 512, 1024, 2048 and 4096, but this driver
     * addresses the volume in 512-byte sectors throughout (cluster math, the
     * FAT entry count per sector, FSInfo), so any other size would be misread.
     * Refuse it until those paths take the volume's own sector size. */
    if (bps != 512) {
        FAT32_BPB_FAIL("BPB validation failed: BytsPerSec %u (only 512 is supported)",
             (uint64_t)bps);
        return -1;
    }

    /* SecPerClus must be a power of two, 1-128 */
    if (spc == 0 || (spc & (spc - 1)) != 0 || spc > 128) {
        FAT32_BPB_FAIL("BPB validation failed: bad SecPerClus %u",
             (uint64_t)spc);
        return -1;
    }

    /* NumFATs must be 1 or 2 */
    if (nf != 1 && nf != 2) {
        FAT32_BPB_FAIL("BPB validation failed: bad NumFATs %u",
             (uint64_t)nf);
        return -1;
    }

    /* RootEntCnt must be 0 for FAT32 */
    if (rec != 0) {
        FAT32_BPB_FAIL("BPB validation failed: RootEntCnt %u (must be 0)",
             (uint64_t)rec);
        return -1;
    }

    /* RootClus must be >= 2 */
    if (rc < 2) {
        FAT32_BPB_FAIL("BPB validation failed: RootClus %u (must be >= 2)",
             (uint64_t)rc);
        return -1;
    }

    /* TotSec32 must be > 0 */
    if (ts == 0) {
        FAT32_BPB_FAIL("BPB validation failed: TotSec32 is 0");
        return -1;
    }

    /* Geometry trust-boundary checks: reserved_sectors, fat_size_sectors,
     * and num_fats are attacker-controlled raw-disk fields that the
     * fat32_init code uses to compute first_data_sector (= reserved +
     * num_fats * fat_size). Without these guards, a forged BPB can wrap
     * u32 arithmetic so first_data_sector underflows total_sectors,
     * driving cluster scans, free-count math, and fsck across bogus FAT
     * ranges. Validate the inputs AND the computed result. */
    uint32_t resv = bpb->reserved_sectors;
    uint32_t fsz  = bpb->fat_size_sectors;

    if (resv == 0) {
        FAT32_BPB_FAIL("BPB validation failed: reserved_sectors is 0");
        return -1;
    }
    if (fsz == 0) {
        FAT32_BPB_FAIL("BPB validation failed: fat_size_sectors is 0");
        return -1;
    }

    /* Compute first_data_sector in u64 to detect u32 wrap. */
    uint64_t fds = (uint64_t)resv + ((uint64_t)nf * (uint64_t)fsz);
    if (fds > (uint64_t)0xFFFFFFFFu) {
        FAT32_BPB_FAIL(
             "BPB validation failed: first_data_sector wraps u32 (resv=%u num_fats=%u fat_sz=%u)",
             (uint64_t)resv, (uint64_t)nf, (uint64_t)fsz);
        return -1;
    }
    if ((uint32_t)fds >= ts) {
        FAT32_BPB_FAIL(
             "BPB validation failed: first_data_sector %u >= total_sectors %u",
             (uint64_t)(uint32_t)fds, (uint64_t)ts);
        return -1;
    }

    /* root_cluster must be addressable: (total - first_data) / spc + 1
     * gives the maximum cluster index. FAT32 spec caps the FAT entry
     * value at 0x0FFFFFEF; root_cluster above that is a forged BPB. */
    uint32_t data_sectors = ts - (uint32_t)fds;
    uint32_t max_cluster  = (data_sectors / spc) + 1u;
    if (max_cluster > 0x0FFFFFEFu)
        max_cluster = 0x0FFFFFEFu;
    if (rc > max_cluster) {
        FAT32_BPB_FAIL(
             "BPB validation failed: RootClus %u > max_cluster %u",
             (uint64_t)rc, (uint64_t)max_cluster);
        return -1;
    }

    /* Every data cluster needs a FAT entry. With a short FAT, a cluster's
     * "entry" falls past the table into whatever follows it, and the walks and
     * the allocator would read unrelated bytes as chain links. */
    if ((uint64_t)fsz * FAT32_ENTRIES_PER_SECTOR <
        (uint64_t)(data_sectors / spc) + 2u) {
        FAT32_BPB_FAIL(
             "BPB validation failed: FAT of %u sectors cannot map %u clusters",
             (uint64_t)fsz, (uint64_t)(data_sectors / spc));
        return -1;
    }

    return 0;
}

/* ---- Long-name runs ---- */

void fat32_lfn_run_reset(fat32_lfn_run_t *r)
{
    int k;
    for (k = 0; k < FAT32_MAX_NAME; k++)
        r->name[k] = '\0';
    r->active = 0;
    r->checksum = 0;
    r->next_seq = 0;
}

/* One LFN slot. A run starts at a slot carrying LFN_LAST_ENTRY and must then
 * count down 1 at a time under one checksum; any break drops it, so a run left
 * behind by a deletion or an interrupted rename never names the next file. */
void fat32_lfn_run_feed(fat32_lfn_run_t *r, const struct fat32_lfn_entry *lfn)
{
    int seq = lfn->seq & LFN_SEQ_MASK;

    if (lfn->seq & LFN_LAST_ENTRY) {
        fat32_lfn_run_reset(r);
        r->active = 1;
        r->checksum = lfn->checksum;
        r->next_seq = seq;
    }
    if (!r->active)
        return;
    if (seq != r->next_seq || lfn->checksum != r->checksum ||
        seq < 1 || seq > FAT32_LFN_MAX_ENTRIES) {
        r->active = 0;
        return;
    }
    lfn_extract_chars(lfn, r->name, seq - 1);
    r->next_seq = seq - 1;
}

/* The short entry that ends a run. 1 when the run names it: the run counted
 * down to slot 1, holds a name, and carries the checksum this short name
 * produces (the rule Windows and Linux apply). The run is consumed either way. */
int fat32_lfn_run_bind(fat32_lfn_run_t *r, const uint8_t sfn[11])
{
    int bound = r->active && r->next_seq == 0 && r->name[0] != '\0' &&
                fat32_lfn_checksum(sfn) == r->checksum;
    r->active = 0;
    return bound;
}

/* The name a listing shows for a short entry: its bound long name, else the
 * 8.3 name. Returns 1 when the long name was used. */
int fat32_dirent_name(fat32_lfn_run_t *r, const struct fat32_dir_entry *de,
                      char out[FAT32_MAX_NAME])
{
    if (fat32_lfn_run_bind(r, de->name)) {
        fat32_strcpy(out, r->name, FAT32_MAX_NAME);
        return 1;
    }
    fat32_short_name_to_str(de->name, out);
    return 0;
}

/* ---- Boot sector and FSInfo ---- */

int fat32_parse_boot_sector(const uint8_t *boot, uint64_t dev_sectors,
                            struct fat32_bpb *bpb, char label[12])
{
    int i, end;

    if (boot[510] != 0x55 || boot[511] != 0xAA) {
        FAT32_BPB_FAIL("FAT32: invalid boot signature");
        return -1;
    }

    /* Every multibyte BPB field sits at an odd or unaligned offset, so each is
     * assembled from bytes rather than read through a cast pointer. */
    bpb->bytes_per_sector    = le16_at(boot + 11);
    bpb->sectors_per_cluster = boot[13];
    bpb->reserved_sectors    = le16_at(boot + 14);
    bpb->num_fats            = boot[16];
    bpb->fat_size_sectors    = le32_at(boot + 36);
    bpb->root_cluster        = le32_at(boot + 44);
    bpb->fs_info_sector      = le16_at(boot + 48);

    bpb->total_sectors = le16_at(boot + 19);
    if (bpb->total_sectors == 0)
        bpb->total_sectors = le32_at(boot + 32);

    if (dev_sectors > 0 && bpb->total_sectors > dev_sectors)
        bpb->total_sectors = (uint32_t)dev_sectors;

    /* A wrapped sum is caught by fat32_bpb_check, which recomputes it in 64
     * bits before anything uses this value. */
    bpb->first_fat_sector  = bpb->reserved_sectors;
    bpb->first_data_sector = bpb->reserved_sectors +
                             (uint32_t)bpb->num_fats * bpb->fat_size_sectors;

    /* Volume label: BPB offset 71, 11 bytes, space-padded */
    for (i = 0; i < 11; i++)
        label[i] = (char)boot[71 + i];
    label[11] = '\0';
    for (end = 10; end >= 0 && label[end] == ' '; end--)
        label[end] = '\0';

    return fat32_bpb_check(bpb, boot);
}

int fat32_parse_fsinfo(const uint8_t *fsi, const struct fat32_bpb *bpb,
                       uint32_t *free_count, uint32_t *next_free)
{
    uint32_t clusters;

    if (le32_at(fsi + 0) != FSINFO_LEAD_SIG ||
        le32_at(fsi + 484) != FSINFO_STRUCT_SIG ||
        le32_at(fsi + 508) != FSINFO_TRAIL_SIG)
        return 0;

    *free_count = le32_at(fsi + 488);
    *next_free  = le32_at(fsi + 492);

    /* A hint outside the data clusters is ignored, not trusted. bpb has passed
     * fat32_bpb_check, so first_data_sector < total_sectors. */
    clusters = (bpb->total_sectors - bpb->first_data_sector) /
               bpb->sectors_per_cluster;
    if (*next_free < 2 || *next_free >= clusters + 2u)
        *next_free = 2;
    /* More free clusters than the volume has is a forged count: unknown, so
     * the driver rescans rather than reporting it. */
    if (*free_count != FSINFO_UNKNOWN && *free_count > clusters)
        *free_count = FSINFO_UNKNOWN;
    return 1;
}
