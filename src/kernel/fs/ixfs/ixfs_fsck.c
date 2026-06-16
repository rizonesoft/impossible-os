/* ============================================================================
 * ixfs_fsck.c -- IXFS filesystem integrity check + auto-repair
 *
 * Verifies the superblock, inode table, free-block bitmap, directory tree,
 * per-block refcounts, and the write-ahead journal, then (in fix mode)
 * applies a SAFE repair subset. Pass-based, mirroring e2fsck / chkdsk and
 * the sibling fat32_fsck().
 *
 * Recovery contract (the recovery environment runs this on each slot):
 *   - The volume must be quiesced (no concurrent VFS writers). fsck flushes
 *     the write-back cache first, then operates on raw disk I/O for all
 *     scans and repairs so it never acts on stale cached blocks nor has its
 *     repairs reverted by a later cache flush.
 *   - Repair is the SAFE subset only. Cross-linked and snapshot-shared
 *     blocks are never freed; a block is freed only when the freshly
 *     computed reference graph proves it is single-owner. A corrupt journal
 *     is discarded, never replayed. A partial directory walk disables every
 *     destructive pass (a transient OOM must never wipe live data).
 *
 * Safety invariants this checker enforces:
 *   - Cache coherence: flush + raw I/O (the cache must not revert repairs).
 *   - Metadata accounting: every block below s_data_start (bitmap, inode
 *     table, checksum/journal/refcount/snapshot tables) is marked in-use, so
 *     the bitmap rebuild never frees a metadata region.
 *   - Snapshot / refcount awareness: the expected reference graph unions the
 *     live tree AND every active snapshot's saved inode table; freeing is
 *     gated on the per-block expected refcount.
 *   - Journal validation: header bounds + per-entry target range + payload
 *     checksum are validated before any replay; an invalid journal is
 *     discarded under fix, reported under check-only.
 * ============================================================================ */

#include "ixfs_internal.h"
#include "kernel/mm/pmm.h"

/* ---- bit helpers for the scratch expected-use bitmap ---- */

static inline void fsck_bit_set(uint8_t *bits, uint64_t idx)
{
    bits[idx / 8] |= (uint8_t)(1u << (idx % 8));
}

static inline int fsck_bit_test(const uint8_t *bits, uint64_t idx)
{
    return (bits[idx / 8] >> (idx % 8)) & 1;
}

/* Freestanding byte fill/copy (the kernel has no shared mem* header; each
 * subsystem provides its own, mirroring ntfs_memset/ntfs_memcpy). */
static inline void fsck_zero(void *p, uint32_t n)
{
    uint8_t *b = (uint8_t *)p;
    uint32_t i;
    for (i = 0; i < n; i++)
        b[i] = 0;
}

static inline void fsck_copy(void *dst, const void *src, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    uint32_t i;
    for (i = 0; i < n; i++)
        d[i] = s[i];
}

/* Zeroed scratch allocation: kmalloc for <= 4 KiB, pmm_alloc_contiguous
 * otherwise (CLAUDE.md sizing rule -- fsck scratch buffers routinely exceed
 * one page on real volumes). *out_pages = pmm page count (0 = kmalloc). */
static void *fsck_alloc(uint32_t bytes, uint32_t *out_pages)
{
    void *p;

    if (bytes == 0)
        bytes = 1;

    if (bytes <= 4096u) {
        *out_pages = 0;
        p = kmalloc(bytes);
    } else {
        uint32_t pages = (bytes + 4095u) / 4096u;
        *out_pages = pages;
        p = (void *)(uintptr_t)pmm_alloc_contiguous(pages);
    }
    if (p) {
        uint32_t n = (*out_pages) ? (*out_pages * 4096u) : bytes;
        fsck_zero(p, n);
    }
    return p;
}

static void fsck_free(void *p, uint32_t pages)
{
    if (!p)
        return;
    if (pages == 0) {
        kfree(p);
        return;
    }
    {
        uintptr_t base = (uintptr_t)p;
        uint32_t pg;
        for (pg = 0; pg < pages; pg++)
            pmm_free_frame(base + (uintptr_t)pg * 4096u);
    }
}

/* A metadata range [start, start+blocks) is valid when it is absent
 * (blocks == 0) or sits wholly below `limit` (s_data_start) with no overflow
 * and does not claim the superblock at block 0. Every on-disk metadata region
 * (bitmap, inode table, checksum/journal/refcount/snapshot tables) must pass
 * this before fsck trusts the "mark every block below s_data_start used" rule. */
static int fsck_meta_range_ok(uint64_t start, uint64_t blocks, uint64_t limit)
{
    if (blocks == 0)
        return 1;                       /* region absent */
    if (start == 0)
        return 0;                       /* block 0 is the superblock */
    if (start + blocks < start)
        return 0;                       /* overflow */
    return (start + blocks) <= limit;
}

/* ============================================================================
 * Pure validators (no I/O) -- exposed for unit tests.
 * ============================================================================ */

int ixfs_fsck_check_superblock(const struct ixfs_superblock *sb)
{
    int errs = 0;
    uint32_t crc;

    if (!sb)
        return 1;

    if (sb->s_magic != IXFS_MAGIC)
        errs++;
    if (sb->s_version == 0 || sb->s_version > IXFS_VERSION)
        errs++;
    if (sb->s_block_size != IXFS_BLOCK_SIZE)
        errs++;
    if (sb->s_total_blocks == 0)
        errs++;
    if (sb->s_root_inode != IXFS_ROOT_INODE)
        errs++;

    /* Layout ordering: bitmap < inode table < data start, and the data
     * region must fit inside the volume. All other metadata regions
     * (checksum/journal/refcount/snapshot tables) are required to sit
     * below s_data_start so the "mark every block below s_data_start used"
     * rule covers them; verify they do. */
    if (!(sb->s_bitmap_start < sb->s_inode_start &&
          sb->s_inode_start < sb->s_data_start))
        errs++;
    if ((uint64_t)sb->s_data_start > sb->s_total_blocks)
        errs++;

    /* CRC32C of bytes [0..111] (the checksum field at offset 128 and the
     * reserved tail are excluded by construction). */
    crc = ixfs_crc32c(sb, 112);
    if (crc != sb->s_checksum)
        errs++;

    return errs;
}

uint32_t ixfs_fsck_reconcile_bitmap(const uint8_t *expected, uint8_t *actual,
                                    uint32_t nbits, int fix)
{
    uint32_t mismatches = 0;
    uint32_t i;

    if (!expected || !actual)
        return 0;

    for (i = 0; i < nbits; i++) {
        int e = (expected[i / 8] >> (i % 8)) & 1;
        int a = (actual[i / 8] >> (i % 8)) & 1;
        if (e != a) {
            mismatches++;
            if (fix) {
                if (e)
                    actual[i / 8] |= (uint8_t)(1u << (i % 8));
                else
                    actual[i / 8] &= (uint8_t)~(1u << (i % 8));
            }
        }
    }
    return mismatches;
}

int ixfs_fsck_journal_entry_valid(uint32_t je_type, uint32_t je_target,
                                  uint32_t je_checksum, const uint8_t *je_data,
                                  uint32_t je_data_len, uint64_t total_blocks)
{
    if (je_type == IXFS_JE_COMMIT)
        return 1;                       /* commit records carry no payload */
    if (je_type != IXFS_JE_DATA)
        return 0;                       /* unknown entry type */
    if (je_target == 0 || (uint64_t)je_target >= total_blocks)
        return 0;                       /* target out of range */
    if (!je_data || je_data_len == 0)
        return 0;                       /* a real DATA entry carries payload */
    if (ixfs_journal_checksum(je_data, je_data_len) != je_checksum)
        return 0;                       /* payload checksum mismatch */
    return 1;
}

/* ============================================================================
 * Orchestrator helpers.
 * ============================================================================ */

/* Mark every block referenced by an inode's extents in the expected-use
 * bitmap and bump the expected per-block refcount (saturating at 255).
 * When count_cross != 0, a data-region block already claimed by a previously
 * scanned LIVE inode is reported as a cross-link. Returns the number of
 * out-of-range extent references (inode errors). */
static uint32_t fsck_scan_inode_extents(struct ixfs_volume *vol,
                                        const struct ixfs_inode *inode,
                                        uint8_t *expected, uint8_t *refc,
                                        uint64_t nbits, uint32_t data_start,
                                        int count_cross, uint32_t *cross_links)
{
    uint32_t errors = 0;
    uint32_t idx;
    uint32_t max_idx = inode->i_blocks;

    /* A corrupt inline extent count is itself an inode error (ixfs_get_block
     * clamps it, so the walk below is still safe). */
    if (inode->i_extent_count > IXFS_INLINE_EXTENTS)
        errors++;

    /* Bound the walk: a corrupt i_blocks must never spin past the volume. */
    if (max_idx > nbits)
        max_idx = (uint32_t)nbits;

    for (idx = 0; idx < max_idx; idx++) {
        uint32_t blk = ixfs_get_block(vol, inode, idx);
        if (blk == 0)
            continue;                   /* sparse hole */
        if (blk < data_start || (uint64_t)blk >= nbits) {
            errors++;                   /* extent points outside the data region */
            continue;
        }
        if (count_cross && fsck_bit_test(expected, blk))
            (*cross_links)++;           /* two live owners claim one block */
        fsck_bit_set(expected, blk);
        if (refc && refc[blk] < 255)
            refc[blk]++;
    }
    return errors;
}

/* ============================================================================
 * ixfs_fsck_volume -- the per-volume worker.
 * ============================================================================ */

int ixfs_fsck_volume(struct ixfs_volume *vol, int fix,
                     struct ixfs_fsck_report *report)
{
    struct ixfs_fsck_report local;
    uint8_t *expected = (uint8_t *)0;   /* expected block-use bitmap */
    uint8_t *refc = (uint8_t *)0;       /* expected per-block refcount */
    uint8_t *visited = (uint8_t *)0;    /* dir-reachable inode bitset */
    uint32_t *queue = (uint32_t *)0;    /* BFS queue of inode numbers */
    uint32_t exp_pages = 0, refc_pages = 0, vis_pages = 0, q_pages = 0;
    uint8_t *sb_buf = (uint8_t *)0;
    uint8_t *blk_buf = (uint8_t *)0;
    uint64_t total_blocks;
    uint32_t total_inodes;
    uint32_t data_start;
    uint32_t ino;
    int traversal_failed = 0;
    int repair_failed = 0;
    int incomplete = 0;            /* a verification pass could not complete */
    int rc = 0;
    uint32_t errors = 0;

    if (!report)
        report = &local;
    fsck_zero(report, (uint32_t)sizeof(*report));

    if (!vol || !vol->in_use)
        return -1;

    klog(LOG_INFO, "fsck", "IXFS fsck starting (%s mode)",
         fix ? "repair" : "read-only");

    /* Read-only volumes (legacy v1) cannot be repaired; force check-only. */
    if (vol->read_only) {
        report->read_only = 1;
        if (fix) {
            klog(LOG_WARN, "fsck",
                 "volume is read-only; downgrading to check-only");
            fix = 0;
        }
    }

    /* Cache coherence: flush dirty cached blocks so the on-disk image
     * reflects current state, then operate on raw disk I/O throughout. */
    if (ixfs_cache_flush(vol) != 0)
        klog(LOG_WARN, "fsck", "cache flush reported errors before scan");

    total_blocks = vol->sb.s_total_blocks;
    total_inodes = vol->sb.s_total_inodes;
    data_start   = vol->sb.s_data_start;

    sb_buf  = (uint8_t *)kmalloc(IXFS_BLOCK_SIZE);
    blk_buf = (uint8_t *)kmalloc(IXFS_BLOCK_SIZE);
    if (!sb_buf || !blk_buf) {
        klog(LOG_ERROR, "fsck", "out of memory for scratch buffers");
        rc = -1;
        goto done;
    }

    /* ---- Pass 1: superblock ---- */
    {
        int sb_errs = ixfs_fsck_check_superblock(&vol->sb);
        if (ixfs_disk_read(vol, 0, sb_buf) == 0) {
            const struct ixfs_superblock *disk_sb =
                (const struct ixfs_superblock *)sb_buf;
            /* The on-disk copy must agree with the mounted authority. */
            if (disk_sb->s_magic != vol->sb.s_magic ||
                disk_sb->s_checksum != vol->sb.s_checksum)
                sb_errs++;
        } else {
            sb_errs++;
        }
        report->superblock_errors = (uint32_t)sb_errs;

        /* A broken layout makes every later pass meaningless and repair
         * unsafe -- refuse to touch the volume. */
        if (sb_errs > 0 &&
            (!(vol->sb.s_bitmap_start < vol->sb.s_inode_start &&
               vol->sb.s_inode_start < vol->sb.s_data_start) ||
             (uint64_t)vol->sb.s_data_start > total_blocks ||
             total_blocks == 0 || data_start == 0)) {
            report->structural_corruption = 1;
            klog(LOG_ERROR, "fsck",
                 "superblock layout is corrupt; refusing to scan/repair");
            rc = -1;
            goto done;
        }
    }

    /* ---- hostile-count guard (must precede allocation) ----
     * The scan buffers are sized through uint32 arithmetic; a corrupt
     * superblock with an absurd total_blocks/total_inodes would truncate
     * the allocation and let later indexing (by the untruncated count)
     * overrun the buffer. Bound both counts against the device and the
     * IXFS maximum (256 groups * 32768 blocks) before allocating. */
    {
        uint64_t dev_blocks = vol->dev
            ? (vol->dev->sector_count / IXFS_SECTORS_PER_BLK) : 0;
        uint64_t max_blocks = (uint64_t)IXFS_MAX_BLOCK_GROUPS *
                              IXFS_BLOCKS_PER_GROUP;
        if (total_blocks == 0 || total_blocks > max_blocks ||
            (dev_blocks != 0 && total_blocks > dev_blocks) ||
            total_inodes == 0 || (uint64_t)total_inodes > total_blocks) {
            report->structural_corruption = 1;
            klog(LOG_ERROR, "fsck",
                 "superblock counts implausible (blocks=%u inodes=%u); refusing",
                 (uint64_t)total_blocks, (uint64_t)total_inodes);
            rc = -1;
            goto done;
        }
        /* Every metadata region must sit below s_data_start with no overflow;
         * otherwise the "mark [0, s_data_start) used" rule would miss it and
         * bitmap repair / journal write-through could clobber a data block. */
        if (!fsck_meta_range_ok(vol->sb.s_bitmap_start, vol->sb.s_bitmap_blocks, data_start) ||
            !fsck_meta_range_ok(vol->sb.s_inode_start, vol->sb.s_inode_blocks, data_start) ||
            !fsck_meta_range_ok(vol->sb.s_checksum_start, vol->sb.s_checksum_blocks, data_start) ||
            !fsck_meta_range_ok(vol->sb.s_journal_start, vol->sb.s_journal_blocks, data_start) ||
            !fsck_meta_range_ok(vol->sb.s_refcount_start, vol->sb.s_refcount_blocks, data_start) ||
            !fsck_meta_range_ok(vol->sb.s_snapshot_start,
                                vol->sb.s_snapshot_start ? IXFS_SNAPSHOT_BLOCKS : 0,
                                data_start)) {
            report->structural_corruption = 1;
            klog(LOG_ERROR, "fsck",
                 "superblock metadata region out of bounds or in data area; refusing");
            rc = -1;
            goto done;
        }
        /* Metadata regions must also be pairwise non-overlapping (a crafted
         * superblock could nest the journal inside the inode table, etc.). */
        {
            struct { uint64_t s, n; } mr[7];
            int nmr = 0, a, b;
#define FSCK_ADD_MR(cnt, start, blocks) \
            do { if (cnt) { mr[nmr].s = (start); mr[nmr].n = (blocks); nmr++; } } while (0)
            mr[nmr].s = 0; mr[nmr].n = 1; nmr++;  /* superblock at block 0 */
            FSCK_ADD_MR(vol->sb.s_bitmap_blocks,   vol->sb.s_bitmap_start,   vol->sb.s_bitmap_blocks);
            FSCK_ADD_MR(vol->sb.s_inode_blocks,    vol->sb.s_inode_start,    vol->sb.s_inode_blocks);
            FSCK_ADD_MR(vol->sb.s_checksum_blocks, vol->sb.s_checksum_start, vol->sb.s_checksum_blocks);
            FSCK_ADD_MR(vol->sb.s_journal_blocks,  vol->sb.s_journal_start,  vol->sb.s_journal_blocks);
            FSCK_ADD_MR(vol->sb.s_refcount_blocks, vol->sb.s_refcount_start, vol->sb.s_refcount_blocks);
            FSCK_ADD_MR(vol->sb.s_snapshot_start,  vol->sb.s_snapshot_start, IXFS_SNAPSHOT_BLOCKS);
#undef FSCK_ADD_MR
            for (a = 0; a < nmr; a++)
                for (b = a + 1; b < nmr; b++)
                    if (mr[a].s < mr[b].s + mr[b].n && mr[b].s < mr[a].s + mr[a].n) {
                        report->structural_corruption = 1;
                        report->superblock_errors++;
                        klog(LOG_ERROR, "fsck",
                             "superblock metadata regions overlap; refusing");
                        rc = -1;
                        goto done;
                    }
        }
    }

    /* ---- allocate the scan structures ---- */
    {
        uint32_t bitmap_bytes = (uint32_t)((total_blocks + 7) / 8);
        uint32_t vis_bytes    = (total_inodes + 7) / 8;
        uint32_t q_bytes      = total_inodes * (uint32_t)sizeof(uint32_t);

        expected = (uint8_t *)fsck_alloc(bitmap_bytes, &exp_pages);
        refc     = (uint8_t *)fsck_alloc((uint32_t)total_blocks, &refc_pages);
        visited  = (uint8_t *)fsck_alloc(vis_bytes, &vis_pages);
        queue    = (uint32_t *)fsck_alloc(q_bytes, &q_pages);
        if (!expected || !refc || !visited || !queue) {
            klog(LOG_ERROR, "fsck", "out of memory for scan structures");
            rc = -1;
            goto done;
        }
    }

    /* ---- Pass 2: mark all metadata blocks [0, s_data_start) used ---- */
    {
        uint32_t b;
        for (b = 0; b < data_start && (uint64_t)b < total_blocks; b++) {
            fsck_bit_set(expected, b);
            if (refc[b] < 255)
                refc[b]++;
        }
    }

    /* ---- Pass 3: live inode scan (cross-link detection here) ---- */
    {
        struct ixfs_inode inode;
        for (ino = 1; ino < total_inodes; ino++) {
            if (ixfs_read_inode(vol, ino, &inode) != 0) {
                report->inode_errors++;
                continue;
            }
            /* Free inode: no type bits and no links. */
            if (inode.i_links == 0 && (inode.i_mode & IXFS_S_TYPEMASK) == 0)
                continue;
            /* Allocated: validate the type field. */
            {
                uint16_t type = inode.i_mode & IXFS_S_TYPEMASK;
                if (type != IXFS_S_FILE && type != IXFS_S_DIR)
                    report->inode_errors++;
            }
            report->inode_errors += fsck_scan_inode_extents(
                vol, &inode, expected, refc, total_blocks, data_start,
                1 /*detect cross-links*/, &report->cross_links);
        }
    }

    /* ---- Pass 4: snapshot saved-inode-table scan (legitimate sharing) ---- */
    if (vol->sb.s_snapshot_count > 0) {
        uint32_t si;
        struct ixfs_inode inode;
        for (si = 0; si < IXFS_MAX_SNAPSHOTS; si++) {
            struct ixfs_snapshot_entry *se = &vol->snapshots[si];
            uint32_t root = se->se_root_block;
            uint32_t nblk = se->se_inode_blocks;
            uint32_t bi;
            if (se->se_flags == 0)
                continue;               /* inactive snapshot slot */
            if (root < data_start || (uint64_t)root >= total_blocks)
                continue;               /* corrupt snapshot pointer */
            if ((uint64_t)root + nblk > total_blocks)
                nblk = (uint32_t)(total_blocks - root);
            /* The snapshot's saved inode table is also metadata; protect it. */
            for (bi = 0; bi < nblk; bi++) {
                uint32_t mb = root + bi;
                if (!fsck_bit_test(expected, mb)) {
                    fsck_bit_set(expected, mb);
                    if (refc[mb] < 255)
                        refc[mb]++;
                }
            }
            /* Walk each saved inode's extents so snapshot-referenced data
             * blocks are counted (and therefore never freed). */
            for (bi = 0; bi < nblk; bi++) {
                uint32_t mb = root + bi;
                uint32_t k;
                if (ixfs_disk_read(vol, mb, blk_buf) != 0)
                    continue;
                for (k = 0; k < IXFS_INODES_PER_BLOCK; k++) {
                    struct ixfs_inode *sin =
                        (struct ixfs_inode *)(blk_buf +
                            k * sizeof(struct ixfs_inode));
                    if (sin->i_links == 0 &&
                        (sin->i_mode & IXFS_S_TYPEMASK) == 0)
                        continue;
                    fsck_copy(&inode, sin, (uint32_t)sizeof(inode));
                    (void)fsck_scan_inode_extents(vol, &inode, expected, refc,
                                                  total_blocks, data_start,
                                                  0 /*no cross-link flag*/,
                                                  &report->cross_links);
                }
            }
        }
    }

    /* ---- Pass 5: directory-tree walk from root (orphan detection) ---- */
    {
        uint32_t head = 0, tail = 0;
        struct ixfs_inode dinode;

        if (total_inodes > IXFS_ROOT_INODE) {
            fsck_bit_set(visited, IXFS_ROOT_INODE);
            queue[tail++] = IXFS_ROOT_INODE;
        }

        while (head < tail) {
            uint32_t dir_ino = queue[head++];
            uint32_t nblocks, b;

            if (ixfs_read_inode(vol, dir_ino, &dinode) != 0) {
                traversal_failed = 1;
                break;
            }
            if ((dinode.i_mode & IXFS_S_TYPEMASK) != IXFS_S_DIR)
                continue;               /* not a directory: no children */

            nblocks = (uint32_t)((dinode.i_size + IXFS_BLOCK_SIZE - 1)
                                 / IXFS_BLOCK_SIZE);
            if (nblocks > dinode.i_blocks)
                nblocks = dinode.i_blocks;

            for (b = 0; b < nblocks; b++) {
                uint32_t dblk = ixfs_get_block(vol, &dinode, b);
                uint32_t e;
                int block_dirty = 0;
                uint32_t block_repairs = 0; /* counted only on durable write */
                if (dblk == 0)
                    continue;
                /* A directory extent pointing outside the data region is
                 * corrupt; reading/repairing it could parse or overwrite a
                 * metadata block. Treat as an incomplete walk (disables all
                 * destructive repair). */
                if (dblk < data_start || (uint64_t)dblk >= total_blocks) {
                    traversal_failed = 1;
                    break;
                }
                if (ixfs_disk_read(vol, dblk, blk_buf) != 0) {
                    traversal_failed = 1;
                    break;
                }
                for (e = 0; e < IXFS_DIRENTS_PER_BLOCK; e++) {
                    struct ixfs_dir_entry *de =
                        (struct ixfs_dir_entry *)(blk_buf +
                            e * sizeof(struct ixfs_dir_entry));
                    uint32_t tino = de->d_inode;
                    struct ixfs_inode tnode;
                    int dangling = 0;

                    if (tino == 0)
                        continue;       /* free slot */
                    if (tino < IXFS_ROOT_INODE || tino >= total_inodes) {
                        report->bad_dirents++;
                        if (fix && !traversal_failed) {
                            de->d_inode = 0;
                            block_dirty = 1;
                            block_repairs++;
                        }
                        continue;
                    }
                    if (ixfs_read_inode(vol, tino, &tnode) != 0)
                        dangling = 1;
                    else if (tnode.i_links == 0 &&
                             (tnode.i_mode & IXFS_S_TYPEMASK) == 0)
                        dangling = 1;    /* entry points at a free inode */

                    if (dangling) {
                        report->bad_dirents++;
                        if (fix && !traversal_failed) {
                            de->d_inode = 0;
                            block_dirty = 1;
                            block_repairs++;
                        }
                        continue;
                    }
                    if (!fsck_bit_test(visited, tino)) {
                        fsck_bit_set(visited, tino);
                        if ((tnode.i_mode & IXFS_S_TYPEMASK) == IXFS_S_DIR &&
                            tail < total_inodes)
                            queue[tail++] = tino;
                    }
                }
                if (block_dirty && fix && !traversal_failed) {
                    /* Count the dirent repairs only once the block is durably
                     * written; a failed write is a torn repair. */
                    if (ixfs_disk_write(vol, dblk, blk_buf) != 0)
                        repair_failed = 1;
                    else
                        report->dirents_repaired += block_repairs;
                }
            }
            if (traversal_failed)
                break;
        }
    }

    /* ---- Pass 6: orphan inodes (allocated but unreachable) ---- */
    {
        struct ixfs_inode inode;
        for (ino = 1; ino < total_inodes; ino++) {
            if (ino == IXFS_ROOT_INODE)
                continue;
            if (fsck_bit_test(visited, ino))
                continue;
            if (ixfs_read_inode(vol, ino, &inode) != 0)
                continue;
            if (inode.i_links == 0 && (inode.i_mode & IXFS_S_TYPEMASK) == 0)
                continue;               /* already free */

            report->orphan_inodes++;

            /* Free the orphan only when the directory walk is complete AND
             * every block it owns is single-owner (expected refcount <= 1).
             * A shared block (snapshot or cross-link) leaves the orphan as
             * report-only. A partial walk disables all freeing. */
            if (fix && !traversal_failed && !report->structural_corruption) {
                uint32_t idx, nb = inode.i_blocks;
                int shared = 0;
                if (nb > total_blocks)
                    nb = (uint32_t)total_blocks;
                for (idx = 0; idx < nb; idx++) {
                    uint32_t blk = ixfs_get_block(vol, &inode, idx);
                    if (blk == 0)
                        continue;
                    if ((uint64_t)blk < total_blocks && refc[blk] > 1) {
                        shared = 1;
                        break;
                    }
                }
                if (shared)
                    continue;           /* shared: report-only */

                /* Free the inode FIRST, then release its blocks from the
                 * EXPECTED set + refcount (pass-7 reconcile is the single
                 * bitmap writer and persists the result). If the inode write
                 * fails, the blocks stay used: never mark a block free while
                 * an allocated inode still references it. A pre-zero copy
                 * keeps the extent list to walk after the inode is zeroed. */
                {
                    struct ixfs_inode freed = inode;
                    fsck_zero(&inode, (uint32_t)sizeof(inode));
                    if (ixfs_write_inode(vol, ino, &inode) != 0) {
                        repair_failed = 1;
                        continue;       /* inode not freed -> keep blocks used */
                    }
                    report->inodes_repaired++;
                    for (idx = 0; idx < nb; idx++) {
                        uint32_t blk = ixfs_get_block(vol, &freed, idx);
                        if (blk == 0 || (uint64_t)blk >= total_blocks)
                            continue;
                        if (blk >= data_start) {
                            if (fsck_bit_test(expected, blk))
                                expected[blk / 8] &= (uint8_t)~(1u << (blk % 8));
                            if (refc[blk] > 0)
                                refc[blk]--;
                        }
                    }
                }
            }
        }
    }

    /* ---- Pass 7: bitmap reconcile (after orphan free) ---- */
    {
        int do_fix = fix && !report->structural_corruption;
        report->bitmap_mismatches = ixfs_fsck_reconcile_bitmap(
            expected, vol->block_bitmap, (uint32_t)total_blocks, do_fix);
        if (do_fix && report->bitmap_mismatches > 0) {
            report->blocks_repaired += report->bitmap_mismatches;
            if (ixfs_flush_bitmap(vol) != 0)
                repair_failed = 1;
            ixfs_init_groups(vol);
        }
    }

    /* ---- Pass 8: free counts ---- */
    {
        uint64_t used = 0, b;
        uint32_t free_inodes = 0;
        struct ixfs_inode inode;
        for (b = 0; b < total_blocks; b++)
            if (fsck_bit_test(expected, b))
                used++;
        {
            uint64_t exp_free = total_blocks - used;
            if (vol->sb.s_free_blocks != exp_free) {
                report->free_count_errors++;
                if (fix && !report->structural_corruption)
                    vol->sb.s_free_blocks = exp_free;
            }
        }
        for (ino = 0; ino < total_inodes; ino++) {
            if (ino == IXFS_INODE_FREE) {
                free_inodes++;          /* inode 0 is reserved/never used */
                continue;
            }
            if (ixfs_read_inode(vol, ino, &inode) != 0)
                continue;
            if (inode.i_links == 0 && (inode.i_mode & IXFS_S_TYPEMASK) == 0)
                free_inodes++;
        }
        if (vol->sb.s_free_inodes != free_inodes) {
            report->free_count_errors++;
            if (fix && !report->structural_corruption)
                vol->sb.s_free_inodes = free_inodes;
        }
        if (fix && !report->structural_corruption &&
            report->free_count_errors > 0) {
            if (ixfs_flush_superblock(vol) != 0)
                repair_failed = 1;
        }
    }

    /* ---- Pass 9: refcount reconcile ---- */
    if (vol->refcount_table) {
        uint64_t b;
        uint32_t mism = 0;
        for (b = data_start; b < total_blocks && b < vol->refcount_bytes; b++) {
            uint8_t want = refc[b];
            uint8_t have = vol->refcount_table[b];
            if (want == 0)
                continue;               /* free block: table value irrelevant */
            if (have != want)
                mism++;
        }
        report->refcount_mismatches = mism;
        /* Rebuild only when the reference graph is trustworthy: no cross-links
         * and a complete walk. */
        if (fix && mism > 0 && report->cross_links == 0 &&
            !traversal_failed && !report->structural_corruption) {
            for (b = data_start; b < total_blocks && b < vol->refcount_bytes; b++)
                if (refc[b] > 0)
                    vol->refcount_table[b] = refc[b];
            if (ixfs_refcount_flush(vol) != 0)
                repair_failed = 1;
        }
    }

    /* ---- Pass 10: journal validation + replay/discard ---- */
    {
        uint32_t jbase = vol->sb.s_journal_start;
        uint32_t jsize = vol->sb.s_journal_blocks;
        int journal_present = (jsize > 0 &&
                               (uint64_t)jbase + jsize <= total_blocks);
        if (journal_present && ixfs_disk_read(vol, jbase, sb_buf) == 0) {
            struct ixfs_journal_header *jh =
                (struct ixfs_journal_header *)sb_buf;
            if (jh->jh_magic != IXFS_JOURNAL_MAGIC) {
                /* No journal signature: nothing to replay (normal for a
                 * freshly formatted or journal-less volume). */
            } else if (jh->jh_head >= jsize || jh->jh_tail >= jsize ||
                       jh->jh_head == 0 || jh->jh_tail == 0) {
                report->journal_errors++;   /* header positions out of range */
            } else {
                uint32_t slot = jh->jh_tail;
                uint32_t guard = 0;
                while (slot != jh->jh_head && guard < jsize) {
                    struct ixfs_journal_entry *je =
                        (struct ixfs_journal_entry *)blk_buf;
                    if (ixfs_disk_read(vol, jbase + slot, blk_buf) != 0) {
                        report->journal_errors++;
                        break;
                    }
                    if (!ixfs_fsck_journal_entry_valid(
                            je->je_type, je->je_target, je->je_checksum,
                            je->je_data, 4080, total_blocks))
                        report->journal_errors++;
                    slot++;
                    if (slot >= jsize)
                        slot = 1;       /* wrap (slot 0 is the header) */
                    guard++;
                }

                if (fix && !report->structural_corruption) {
                    if (report->journal_errors == 0) {
                        /* Validated: safe replay. A failed replay leaves the
                         * volume torn -> untrustworthy. */
                        if (ixfs_journal_recover(vol) != 0)
                            repair_failed = 1;
                    } else {
                        /* Corrupt journal: discard rather than replay garbage. */
                        jh->jh_tail = jh->jh_head;
                        if (ixfs_disk_write(vol, jbase, sb_buf) != 0)
                            repair_failed = 1;
                        klog(LOG_WARN, "fsck",
                             "discarded corrupt journal (not replayed)");
                    }
                }
            }
        }
    }

    /* ---- Pass 11: data-block checksums (delegated to scrub) ---- */
    if (vol == ixfs_get_active_volume()) {
        int scrub = ixfs_scrub();
        if (scrub > 0) {
            report->data_checksum_errors = (uint32_t)scrub;
        } else if (scrub < 0) {
            /* The checksum pass did not run (missing state / OOM); an
             * unexecuted pass is not evidence of a trustworthy volume. */
            incomplete = 1;
            klog(LOG_WARN, "fsck",
                 "data-checksum scrub did not complete; result untrustworthy");
        }
    }

    /* ---- Coherence: persist cached repairs, then drop stale cache entries ----
     * Repairs land via a mix of cached writes (bitmap/superblock/refcount/inode)
     * and raw writes (directory blocks, journal). Flush the cached ones to disk,
     * then invalidate ALL cache entries so a stale clean copy of a raw-written
     * block can never shadow the repaired on-disk image for a later reader. */
    if (fix && !report->structural_corruption) {
        /* A failed flush leaves dirty repairs unwritten; do NOT invalidate
         * (that would drop them) -- mark the run untrustworthy instead. */
        if (ixfs_cache_flush(vol) != 0)
            repair_failed = 1;
        else
            ixfs_cache_init(vol);
    }

done:
    fsck_free(expected, exp_pages);
    fsck_free(refc, refc_pages);
    fsck_free(visited, vis_pages);
    fsck_free(queue, q_pages);
    if (sb_buf)
        kfree(sb_buf);
    if (blk_buf)
        kfree(blk_buf);

    if (rc != 0 && report->structural_corruption == 0)
        return rc;                      /* allocation / no-volume failure */

    errors = report->superblock_errors + report->inode_errors +
             report->bitmap_mismatches + report->free_count_errors +
             report->orphan_inodes + report->bad_dirents +
             report->cross_links + report->refcount_mismatches +
             report->journal_errors + report->data_checksum_errors;

    report->repaired = (report->blocks_repaired || report->inodes_repaired ||
                        report->dirents_repaired);

    klog(LOG_INFO, "fsck",
         "IXFS fsck done: %u errors (orphans=%u bad-dirents=%u cross=%u "
         "bitmap=%u journal=%u) -- %s",
         (uint64_t)errors, (uint64_t)report->orphan_inodes,
         (uint64_t)report->bad_dirents, (uint64_t)report->cross_links,
         (uint64_t)report->bitmap_mismatches, (uint64_t)report->journal_errors,
         report->structural_corruption ? "STRUCTURAL CORRUPTION" :
         traversal_failed ? "incomplete walk" :
         (fix ? (errors ? "repaired" : "clean") :
                (errors ? "errors remain" : "clean")));

    /* Return contract (mirrors fat32_fsck):
     *   0  = consistent (clean, or fully repaired with every write OK).
     *   -1 = untrustworthy: structural corruption, partial walk, a torn
     *        repair write, or check-only mode that found errors. */
    if (report->structural_corruption || traversal_failed || incomplete ||
        (fix && repair_failed))
        return -1;
    if (!fix && errors > 0)
        return -1;
    if (fix) {
        /* The safe-repair subset does NOT fix every class: cross-links,
         * data-checksum mismatches, superblock/inode-field corruption, and
         * snapshot-shared orphans / bad dirents left unrepaired all keep the
         * volume untrustworthy even after a fix run. (inodes_repaired <=
         * orphan_inodes and dirents_repaired <= bad_dirents by construction,
         * so the subtractions never underflow.) */
        uint32_t unrepaired = report->superblock_errors + report->inode_errors +
            report->cross_links + report->data_checksum_errors +
            (report->orphan_inodes - report->inodes_repaired) +
            (report->bad_dirents - report->dirents_repaired);
        if (unrepaired > 0)
            return -1;
    }
    return 0;
}

/* ============================================================================
 * Public entry: fsck the active volume.
 * ============================================================================ */

int ixfs_fsck(int fix, struct ixfs_fsck_report *report)
{
    struct ixfs_volume *vol = ixfs_get_active_volume();
    if (!vol)
        return -1;
    return ixfs_fsck_volume(vol, fix, report);
}
