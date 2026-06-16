/* ============================================================================
 * partition.c -- Partition Scanner & Sub-Block-Device Layer
 *
 * Scans all block devices for GPT (preferred) or MBR partition tables.
 * For each discovered partition, a sub-block-device is registered that
 * transparently offsets all LBA reads/writes by the partition start.
 *
 * After creating sub-devices, each partition's first sector is probed
 * for known filesystem signatures:
 *   - FAT32: BPB boot signature 0x55AA + "FAT32   " at offset 82
 *   - IXFS:  Superblock magic 0x49584653 at offset 0
 *   - NTFS:  OEM ID "NTFS    " at offset 0x03 + boot signature 0x55AA
 *   - ext2:  Magic 0xEF53 at superblock offset 56 (1024 bytes into partition)
 * ============================================================================ */

#include "kernel/fs/partition.h"
#include "kernel/fs/mbr.h"
#include "kernel/fs/gpt.h"
#include "kernel/fs/fat32.h"
#include "kernel/fs/ixfs.h"
#include "kernel/fs/ntfs.h"
#include "kernel/fs/vfs.h"
#include "kernel/drivers/blkdev.h"
#include "kernel/drivers/serial.h"
#include "kernel/klog.h"
#include "kernel/boot_info.h"   /* g_boot_info.boot_partition_guid -- A/B boot-disk binding */
#include "kernel/printk.h"

/* ---- Partition storage ---- */
static struct partition_info part_store[PART_MAX];
static int part_count;

/* ---- String helper ---- */
static void part_strcpy(char *dst, const char *src, int n)
{
    int i;
    for (i = 0; i < n - 1 && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

static int part_strlen(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

/* Case-insensitive string compare */
static int part_streqi(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == *b;
}

/* Simple itoa for small positive numbers */
static void part_itoa(int val, char *buf)
{
    char tmp[8];
    int i = 0, j;
    if (val == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    while (val > 0 && i < 7) {
        tmp[i++] = '0' + (val % 10);
        val /= 10;
    }
    for (j = 0; j < i; j++)
        buf[j] = tmp[i - 1 - j];
    buf[i] = '\0';
}

/* ---- Sub-blkdev read/write callbacks ---- */

static int part_read(uint64_t lba, uint32_t count, void *buf, void *drv_data)
{
    struct partition_info *pi = (struct partition_info *)drv_data;
    /* Bounds check */
    if (lba + count > pi->sector_count)
        return -1;
    return blkdev_read(pi->parent, pi->start_lba + lba, count, buf);
}

static int part_write(uint64_t lba, uint32_t count, const void *buf,
                      void *drv_data)
{
    struct partition_info *pi = (struct partition_info *)drv_data;
    if (lba + count > pi->sector_count)
        return -1;
    return blkdev_write(pi->parent, pi->start_lba + lba, count, buf);
}

static int part_flush(void *drv_data)
{
    /* Device write caches are per-disk, not per-partition: forward the
     * sync to the parent so vfs_flush durability holds on partitions. */
    struct partition_info *pi = (struct partition_info *)drv_data;
    return blkdev_sync(pi->parent);
}

/* ---- Filesystem probing ---- */

static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

/* Check for FAT32: boot signature 0x55AA at offset 510,
 * "FAT32   " at offset 82, and valid bytes_per_sector. */
static int probe_fat32(const uint8_t *sect)
{
    uint16_t sig = read_le16(sect + 510);
    if (sig != 0xAA55)
        return 0;

    /* Check for FAT32 filesystem type string at BPB offset 82 */
    if (sect[82] == 'F' && sect[83] == 'A' &&
        sect[84] == 'T' && sect[85] == '3' && sect[86] == '2')
        return 1;

    /* Also accept if sectors_per_fat_16 == 0 (FAT32 indicator) and
     * bytes_per_sector is valid */
    {
        uint16_t bps = read_le16(sect + 11);
        uint16_t spf16 = read_le16(sect + 22);
        if (bps == 512 && spf16 == 0 && sect[13] != 0)
            return 1;
    }

    return 0;
}

/* Check for IXFS: magic 0x49584653 at offset 0 of superblock (sector 0) */
static int probe_ixfs(const uint8_t *sect)
{
    uint32_t magic = read_le32(sect);
    return (magic == 0x49584653) ? 1 : 0;
}

/* Check for ext2: magic 0xEF53 at offset 56 within the superblock.
 * The ext2 superblock starts at byte 1024 (sector 2 for 512-byte sectors).
 * We receive sector 2 data in 'sect'. */
static int probe_ext2_sector2(const uint8_t *sect)
{
    /* ext2 superblock magic at offset 56 relative to the superblock start.
     * Since superblock is at byte 1024 and sector 2 starts at byte 1024,
     * the magic is at offset 56 in this sector. */
    uint16_t magic = read_le16(sect + 56);
    return (magic == 0xEF53) ? 1 : 0;
}

static int probe_filesystem(const struct blkdev *sub_dev)
{
    uint8_t sect[512];

    /* Read sector 0 of the partition */
    if (blkdev_read(sub_dev, 0, 1, sect) != 0)
        return PART_FS_UNKNOWN;

    if (probe_ixfs(sect))
        return PART_FS_IXFS;

    /* NTFS must be probed before FAT32: the FAT32 fallback heuristic
     * (0x55AA + bps=512 + sectors_per_fat_16=0 + spc!=0) also matches
     * NTFS boot sectors.  The NTFS OEM ID check is more specific. */
    if (ntfs_probe(sect))
        return PART_FS_NTFS;

    if (probe_fat32(sect))
        return PART_FS_FAT32;

    /* For ext2, need to read sector 2 (byte 1024) */
    if (blkdev_read(sub_dev, 2, 1, sect) == 0) {
        if (probe_ext2_sector2(sect))
            return PART_FS_EXT2;
    }

    return PART_FS_UNKNOWN;
}

const char *partition_fs_name(int fs_type)
{
    switch (fs_type) {
    case PART_FS_FAT32: return "FAT32";
    case PART_FS_IXFS:  return "IXFS";
    case PART_FS_EXT2:  return "ext2";
    case PART_FS_NTFS:  return "NTFS";
    default:            return "Unknown";
    }
}

/* ---- Register a partition as a sub-blkdev ---- */

static void register_partition(const struct blkdev *parent,
                                int disk_idx, int part_num,
                                uint64_t start_lba, uint64_t sector_count,
                                const char *type_name,
                                const char *part_name,
                                int ixfs_slot,
                                const struct gpt_guid *unique_guid)
{
    struct partition_info *pi;
    struct blkdev sub = {0};   /* zero optional callbacks (flush/discard/shutdown) */
    char name[16];
    char num_buf[4];
    int pos;

    if (part_count >= PART_MAX) {
        serial_write("[PART] Partition table full\n");
        return;
    }

    /* Build name like "disk0p1" */
    part_strcpy(name, "disk", sizeof(name));
    part_itoa(disk_idx, num_buf);
    pos = part_strlen(name);
    part_strcpy(name + pos, num_buf, (int)sizeof(name) - pos);
    pos = part_strlen(name);
    name[pos] = 'p'; name[pos + 1] = '\0';
    pos = part_strlen(name);
    part_itoa(part_num, num_buf);
    part_strcpy(name + pos, num_buf, (int)sizeof(name) - pos);

    /* Fill partition info */
    pi = &part_store[part_count];
    pi->parent       = parent;
    pi->start_lba    = start_lba;
    pi->sector_count = sector_count;
    pi->disk_index   = disk_idx;
    pi->part_index   = part_num;
    pi->fs_type      = PART_FS_UNKNOWN;  /* Probed after registration */
    pi->is_efi       = (type_name && type_name[0] == 'E' && type_name[1] == 'F'
                        && type_name[2] == 'I') ? 1 : 0;
    pi->ixfs_slot    = ixfs_slot;   /* A/B slot identity (-1 if not IXFS-family) */
    if (unique_guid)
        pi->unique_guid = *unique_guid;
    else
        pi->unique_guid = (struct gpt_guid){0};  /* MBR/raw: no GPT GUID */
    /* Store GPT partition name for drive letter assignment */
    pi->gpt_name[0] = '\0';
    if (part_name) {
        int j;
        for (j = 0; part_name[j] && j < 36; j++)
            pi->gpt_name[j] = part_name[j];
        pi->gpt_name[j] = '\0';
    }

    /* Build sub-blkdev. Zero-init first: blkdev_register copies the
     * struct, and a garbage function pointer in an unset member (flush,
     * discard) is a wild indirect call later (#UD at boot, 2026-06-12
     * incident -- blkdev_sync on a partition jumped into boot_info). */
    sub = (struct blkdev){0};
    part_strcpy(sub.name, name, sizeof(sub.name));
    sub.sector_size  = parent->sector_size;
    sub.sector_count = sector_count;
    sub.read         = part_read;
    sub.write        = part_write;
    sub.flush        = part_flush;
    sub.driver_data  = (void *)pi;
    sub.active       = 1;

    if (blkdev_register(&sub) != 0) {
        serial_write("[PART] Failed to register sub-blkdev\n");
        return;
    }

    /* Now probe filesystem on the newly registered sub-device */
    {
        const struct blkdev *registered = blkdev_get(name);
        if (registered)
            pi->fs_type = probe_filesystem(registered);
    }

    /* Log: "Disk 0, Partition 1: FAT32, 16 MiB" */
    {
        uint64_t mb = sector_count * 512 / (1024 * 1024);
        if (type_name && type_name[0])
            klog(LOG_DEBUG, "blk", "Disk %u, Partition %u: %s, %u MiB (%s)",
                 (uint64_t)disk_idx, (uint64_t)part_num,
                 partition_fs_name(pi->fs_type), mb, type_name);
        else
            klog(LOG_DEBUG, "blk", "Disk %u, Partition %u: %s, %u MiB",
                 (uint64_t)disk_idx, (uint64_t)part_num,
                 partition_fs_name(pi->fs_type), mb);
    }

    part_count++;
}

/* ---- Main scanner ---- */

static void scan_device(const struct blkdev *dev, int disk_idx)
{
    uint8_t sect[512];
    struct mbr_table mtbl;
    int i;

    if (blkdev_read(dev, 0, 1, sect) != 0)
        return;

    mtbl = mbr_parse(sect);
    if (!mtbl.valid) {
        /* No partition table -- try "super-floppy" mode:
         * treat the entire device as a single raw partition.
         * This handles bare FAT32/exFAT/ext2 images without MBR/GPT. */
        int fs = probe_filesystem(dev);
        if (fs != PART_FS_UNKNOWN) {
            klog(LOG_DEBUG, "blk", "%s: no partition table, raw %s volume",
                   dev->name, partition_fs_name(fs));
            register_partition(dev, disk_idx, 1,
                               0, dev->sector_count, "Raw Volume", NULL, -1, NULL);
        }
        return;
    }

    /* Check for GPT (protective MBR with type 0xEE) */
    if (mtbl.has_gpt) {
        struct gpt_table gtbl = gpt_parse(dev, sect);
        if (gtbl.valid && gtbl.count > 0) {
            int gi;
            klog(LOG_DEBUG, "blk", "%s: %u partition(s)",
                   dev->name, (uint64_t)gtbl.count);
            for (gi = 0; gi < gtbl.count; gi++) {
                uint64_t sectors = gtbl.parts[gi].end_lba
                                 - gtbl.parts[gi].start_lba + 1;
                register_partition(dev, disk_idx, gi + 1,
                                  gtbl.parts[gi].start_lba, sectors,
                                  gpt_type_name(&gtbl.parts[gi].type_guid),
                                  gtbl.parts[gi].name,
                                  gpt_ixfs_slot(&gtbl.parts[gi].type_guid),
                                  &gtbl.parts[gi].unique_guid);
            }
        }
        return;  /* GPT found -- skip MBR */
    }

    /* MBR partitions */
    if (mtbl.count > 0) {
        klog(LOG_DEBUG, "blk", "%s: MBR %u partition(s), disk sig 0x%08X",
               dev->name, (uint64_t)mtbl.count,
               (uint64_t)mtbl.disk_signature);

        /* Register primary partitions (skip extended containers) */
        for (i = 0; i < mtbl.count; i++) {
            if (mtbl.parts[i].is_extended)
                continue;  /* Don't register the container itself */
            register_partition(dev, disk_idx, i + 1,
                              (uint64_t)mtbl.parts[i].start_lba,
                              (uint64_t)mtbl.parts[i].sector_count,
                              mbr_type_name(mtbl.parts[i].type), NULL, -1, NULL);
        }

        /* Walk EBR chain for each extended partition */
        for (i = 0; i < mtbl.count; i++) {
            if (!mtbl.parts[i].is_extended)
                continue;
            {
                struct mbr_entry logical[32];
                int lcount = mbr_walk_ebr(dev,
                                          mtbl.parts[i].start_lba,
                                          dev->sector_count,
                                          logical, 32);
                if (lcount > 0) {
                    int li;
                    for (li = 0; li < lcount; li++) {
                        register_partition(dev, disk_idx, 5 + li,
                                          (uint64_t)logical[li].start_lba,
                                          (uint64_t)logical[li].sector_count,
                                          mbr_type_name(logical[li].type),
                                          NULL, -1, NULL);
                    }
                }
            }
        }
    } else {
        /* MBR signature present (0x55AA) but no usable partition entries.
         * This happens with bare FAT32/exFAT volumes whose BPB shares
         * the same boot signature as MBR.  Try super-floppy mode. */
        int fs = probe_filesystem(dev);
        if (fs != PART_FS_UNKNOWN) {
            printk("[RAW] %s: no partition table, raw %s volume\n",
                   dev->name, partition_fs_name(fs));
            register_partition(dev, disk_idx, 1,
                               0, dev->sector_count, "Raw Volume", NULL, -1, NULL);
        }
    }
}

void partition_scan_all(void)
{
    int i;
    int base_count = blkdev_count();

    for (i = 0; i < base_count; i++) {
        const struct blkdev *dev = blkdev_get_by_index(i);
        if (!dev) continue;
        scan_device(dev, i);
    }
}

/* A/B dual-slot (TODO-21): the slot whose IXFS was actually mounted as C: and
 * whether it differs from the bootloader's selection. Written once, single-
 * threaded, during partition_mount_filesystems() at boot; read afterward by
 * the mark-boot-successful path. No concurrent writers, so no lock needed. */
static int g_ab_mounted_slot = -1;
static int g_ab_slot_mismatch = 0;

int ab_boot_mounted_slot(void) { return g_ab_mounted_slot; }
int ab_boot_slot_mismatch(void) { return g_ab_slot_mismatch; }

/* Build the "disk<D>p<P>" sub-blkdev name for a partition. */
static void part_build_subdev_name(const struct partition_info *pi,
                                   char *name, int name_sz)
{
    char num_buf[4];
    int pos;
    part_strcpy(name, "disk", name_sz);
    part_itoa(pi->disk_index, num_buf);
    pos = part_strlen(name);
    part_strcpy(name + pos, num_buf, name_sz - pos);
    pos = part_strlen(name);
    name[pos] = 'p'; name[pos + 1] = '\0';
    pos = part_strlen(name);
    part_itoa(pi->part_index, num_buf);
    part_strcpy(name + pos, num_buf, name_sz - pos);
}

/* Identify the boot disk's disk_index by matching the ESP unique GUID the
 * bootloader recorded in boot_info.boot_partition_guid (raw on-disk bytes, the
 * layout gpt.c parses). Returns the disk_index, or -1 when it can't be
 * identified (non-GPT boot, all-zero GUID, or no scanned partition matches).
 * A/B root selection is restricted to this disk so a same-slot IXFS on a
 * DIFFERENT physical disk cannot be mounted as the system root (TODO-21). */
static int ab_boot_disk_index(void)
{
    const uint8_t *p = g_boot_info.boot_partition_guid;
    struct gpt_guid bg;
    int i, z;

    if (g_boot_info.boot_partition_style != 2)
        return -1;  /* non-GPT boot -- no GPT unique GUID to match against */

    bg.data1 = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
               ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    bg.data2 = (uint16_t)p[4] | ((uint16_t)p[5] << 8);
    bg.data3 = (uint16_t)p[6] | ((uint16_t)p[7] << 8);
    for (i = 0; i < 8; i++)
        bg.data4[i] = p[8 + i];

    z = (bg.data1 == 0 && bg.data2 == 0 && bg.data3 == 0);
    for (i = 0; z && i < 8; i++)
        if (bg.data4[i]) z = 0;
    if (z)
        return -1;  /* all-zero -- not a real GPT GUID */

    for (i = 0; i < part_count; i++)
        if (gpt_guid_equal(&part_store[i].unique_guid, &bg))
            return part_store[i].disk_index;
    return -1;
}

/* Mount the active A/B slot's IXFS as C:. Prefer the partition whose
 * ixfs_slot matches the bootloader's selection. On an A/B (slot-tagged) disk
 * the kernel mounts ONLY the selected slot -- it never cross-mounts another
 * slot's root, because the bootloader already rejected the alternative (a
 * cross-slot fallback would boot the metadata-rejected, possibly-exhausted
 * root and defeat rollback). If the selected slot is absent or fails to init,
 * C: is left unmounted (g_ab_slot_mismatch=1): this boot will not reach
 * mark-boot-successful, so the bootloader's tries counter advances and the
 * NEXT boot rolls back. Only a disk with NO slot-tagged IXFS (legacy
 * single-slot / raw volume) keeps the first-IXFS fallback. Success is recorded
 * in g_ab_mounted_slot (>= 0). */
/* Attempt to mount part_store[idx]'s IXFS as C:. Returns 1 on success (records
 * g_ab_mounted_slot), 0 if the partition's filesystem is not mountable IXFS or
 * ixfs_init fails. */
static int try_mount_ixfs_as_c(int idx)
{
    struct partition_info *pi = &part_store[idx];
    char name[16];
    const struct blkdev *sub_dev;

    if (pi->fs_type != PART_FS_IXFS)
        return 0;  /* selected slot's root failed to probe as IXFS (corrupt) */
    part_build_subdev_name(pi, name, (int)sizeof(name));
    sub_dev = blkdev_get(name);
    if (!sub_dev)
        return 0;
    if (ixfs_init(sub_dev) != 0) {
        klog(LOG_WARN, "blk", "IXFS: failed to init %s", name);
        return 0;
    }
    vfs_mount('C', ixfs_get_driver(), ixfs_get_root());
    g_ab_mounted_slot = pi->ixfs_slot;
    return 1;
}

static void mount_active_ixfs(int active_slot)
{
    int i;
    int target = -1;        /* part_store index of the selected slot's partition */
    int first = -1;         /* first mountable IXFS (legacy non-A/B fallback) */
    int have_slot_tagged = 0;

    /* Determine which disk A/B root selection binds to. Prefer the boot disk
     * (ESP GUID match). If that cannot be identified, infer the SOLE disk that
     * carries slot-tagged partitions. If slot-tagged partitions exist on MORE
     * THAN ONE disk and the boot disk is unknown, the choice is ambiguous --
     * fail closed rather than risk mounting another disk's same-slot root.
     *
     * A/B detection keys on the GPT slot tag (`ixfs_slot >= 0`), NOT on the
     * filesystem probe: a slot whose root is corrupt enough to fail probe_ixfs
     * is still an A/B slot, and must NOT let the disk fall through to the
     * legacy first-IXFS path (which could mount the wrong root and hide the
     * failure). The filesystem probe gates only whether the SELECTED slot is
     * actually mountable (try_mount_ixfs_as_c). */
    int bind_disk = ab_boot_disk_index();   /* >=0 boot disk, -1 unknown */
    int ab_ambiguous = 0;
    if (bind_disk < 0) {
        int sole = -2;  /* -2 = none seen, -1 = multiple disks */
        for (i = 0; i < part_count; i++) {
            struct partition_info *pi = &part_store[i];
            if (pi->is_efi || pi->ixfs_slot < 0)
                continue;
            if (sole == -2) sole = pi->disk_index;
            else if (sole != pi->disk_index) sole = -1;
        }
        if (sole >= 0) bind_disk = sole;        /* exactly one A/B disk */
        else if (sole == -1) ab_ambiguous = 1;  /* multiple A/B disks, boot disk unknown */
        /* sole == -2: no slot-tagged partition at all -> legacy, bind_disk stays -1 */
    }
    if (ab_ambiguous) {
        g_ab_slot_mismatch = 1;
        klog(LOG_WARN, "blk",
             "A/B: boot disk unidentifiable with multiple A/B disks -- "
             "C: not mounted (fail-closed)");
        return;
    }

    for (i = 0; i < part_count; i++) {
        struct partition_info *pi = &part_store[i];
        if (pi->is_efi)
            continue;
        /* Bind selection to the chosen disk: a partition on a different
         * physical disk is NOT a candidate. */
        if (bind_disk >= 0 && pi->disk_index != bind_disk)
            continue;
        /* Slot tag drives A/B detection + target (FS-independent). */
        if (pi->ixfs_slot >= 0) {
            have_slot_tagged = 1;
            if (pi->ixfs_slot == active_slot && target < 0)
                target = i;
        }
        /* Legacy fallback candidate: first mountable IXFS volume. */
        if (pi->fs_type == PART_FS_IXFS && first < 0)
            first = i;
    }

    if (have_slot_tagged) {
        /* A/B disk: mount ONLY the selected slot. No cross-slot/cross-disk
         * fallback -- the bootloader already rejected the alternatives. If the
         * selected slot is absent, its root failed to probe as IXFS, or
         * ixfs_init fails, leave C: unmounted: this boot will not reach
         * mark-boot-successful, so the bootloader's tries counter advances and
         * the NEXT boot rolls back. */
        if (target >= 0 && try_mount_ixfs_as_c(target)) {
            g_ab_slot_mismatch = 0;
            klog(LOG_INFO, "blk", "A/B: mounted slot %d as C:",
                 (uint64_t)active_slot);
            return;
        }
        g_ab_slot_mismatch = 1;
        klog(LOG_WARN, "blk",
             "A/B: selected slot %d root unavailable/corrupt -- C: not mounted "
             "(boot will fail; bootloader rolls back next boot)",
             (uint64_t)active_slot);
        return;
    }

    /* Legacy / non-A/B disk: mount the first mountable IXFS as C:. */
    if (first >= 0)
        (void)try_mount_ixfs_as_c(first);
}

void partition_mount_filesystems(int active_slot)
{
    int i;
    /* C: = the active A/B slot's IXFS (system), D:+ = non-EFI FAT32 partitions.
     * EFI System Partitions are hidden (no drive letter), like Windows.
     * Special case: partition named "Logs" mounts as X:. */
    char next_fat32_letter = 'D';

    /* Mount the bootloader-selected root slot as C: first; the inactive slot
     * and the metadata partition deliberately get no drive letter. */
    mount_active_ixfs(active_slot);

    for (i = 0; i < part_count; i++) {
        struct partition_info *pi = &part_store[i];
        char name[16];
        const struct blkdev *sub_dev;

        /* A/B root slots NEVER get a data drive letter, regardless of how
         * their first sector probes. A slot-tagged root whose IXFS superblock
         * is corrupt could otherwise probe as FAT32/NTFS and get mounted as
         * D:, letting a recovery/mutation path touch the failed or standby
         * rollback root. The GPT slot tag (ixfs_slot >= 0) is authoritative
         * here, independent of fs_type. Root mounting is mount_active_ixfs's
         * job (C: only). */
        if (pi->ixfs_slot >= 0)
            continue;

        /* IXFS roots are handled by mount_active_ixfs above; the inactive
         * slot stays unmounted (it is a root, not a data volume). */
        if (pi->fs_type != PART_FS_FAT32 && pi->fs_type != PART_FS_NTFS)
            continue;

        /* Skip EFI System Partition -- no drive letter (like Windows) */
        if (pi->is_efi)
            continue;

        part_build_subdev_name(pi, name, (int)sizeof(name));

        sub_dev = blkdev_get(name);
        if (!sub_dev)
            continue;

        if (pi->fs_type == PART_FS_FAT32) {
            struct fat32_volume *fat_vol = fat32_init(sub_dev);
            if (!fat_vol) {
                klog(LOG_WARN, "blk", "FAT32: failed to init %s", name);
                continue;
            }
            /* BlackBox partition mounts as X:\ by GPT name */
            if (part_streqi(pi->gpt_name, "BlackBox")) {
                /* Check dirty bit -- fsck if previous crash */
                if (fat32_is_dirty(fat_vol)) {
                    klog(LOG_WARN, "blk",
                         "BlackBox: partition dirty -- possible corruption");
                    fat32_run_fsck(fat_vol, 1);  /* repair mode */
                }
                /* Mark dirty on mount (cleared on clean shutdown) */
                fat32_mark_dirty(fat_vol);

                vfs_mount('X', fat32_get_driver(),
                          fat32_get_root(fat_vol));
                klog(LOG_INFO, "blk",
                     "BlackBox partition mounted as X:\\");
            } else if (next_fat32_letter <= 'Z') {
                vfs_mount(next_fat32_letter,
                          fat32_get_driver(),
                          fat32_get_root(fat_vol));
                next_fat32_letter++;
                /* Skip X if we reach it (reserved for BlackBox) */
                if (next_fat32_letter == 'X')
                    next_fat32_letter = 'Y';
            }
        } else if (pi->fs_type == PART_FS_NTFS) {
            struct ntfs_volume *ntfs_vol = ntfs_init(sub_dev);
            if (!ntfs_vol) {
                klog(LOG_WARN, "blk", "NTFS: failed to init %s", name);
                continue;
            }

            /* Initialize MFT cache (must be before sysfiles, which reads MFT).
             * Non-fatal: mount continues without caching if this fails. */
            ntfs_cache_init(ntfs_vol, 0);

            /* Load $Bitmap cluster allocation data (NTFS spec 12.1).
             * Required before ntfs_load_sysfiles() for free space counting,
             * and before any cluster allocation (write, create, etc.).
             * Non-fatal: mount continues read-only without bitmap. */
            ntfs_bitmap_load(ntfs_vol);

            /* Load MFT record allocator (NTFS spec 12.3).
             * Enables ntfs_alloc_mft_record() / ntfs_free_mft_record().
             * Requires bitmap to be loaded first.
             * Non-fatal: write operations will fail gracefully. */
            ntfs_mft_alloc_load(ntfs_vol);

            /* Load system metafiles ($Volume label, $Bitmap, $UpCase, $MFTMirr).
             * Non-fatal: individual failures are logged but mount continues. */
            ntfs_load_sysfiles(ntfs_vol);
            if (next_fat32_letter <= 'Z') {
                struct vfs_node *ntfs_root = ntfs_get_root(ntfs_vol);
                if (ntfs_root) {
                    vfs_mount(next_fat32_letter,
                              ntfs_get_driver(), ntfs_root);
                    klog(LOG_INFO, "ntfs",
                         "Mounted NTFS volume on drive %c: (%u sectors)",
                         (uint64_t)next_fat32_letter,
                         ntfs_vol->total_sectors);

                    /* Run self-test if this is the test volume */
                    ntfs_run_self_test(ntfs_vol, ntfs_root);

                    next_fat32_letter++;
                }
            }
        }
    }
}
