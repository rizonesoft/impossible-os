/* ============================================================================
 * ixfs_test.c -- IXFS performance and feature tests
 * ============================================================================ */

#include "ixfs_internal.h"

/* KERNEL_TESTS-gated (release-flavor test-surface exclusion): this whole
 * TU is test-only content, already pruned from the release build's C_SRCS
 * (Makefile). Guarding the body too is a second independent layer, so it
 * cannot link even if a future build-script change bypasses the prune. */
#ifdef KERNEL_TESTS

void ixfs_test_performance(void)
{
    int pass;
    uint32_t vi;
    struct ixfs_volume *vol = (struct ixfs_volume *)0;
    for (vi = 0; volumes && vi < IXFS_MAX_VOLUMES; vi++) {
        if (volumes[vi].in_use) {
            vol = &volumes[vi];
            break;
        }
    }
    if (!vol) {
        klog(LOG_DEBUG, "TEST", "No IXFS volume mounted");
        return;
    }

    klog(LOG_DEBUG, "TEST", "--- IXFS Performance Features Test ---");

    /* --- Test 1: Block Group Allocator --- */
    {
        uint32_t b1, b2, b3;
        uint32_t g1, g2, g3;
        uint64_t saved_free = vol->sb.s_free_blocks;

        b1 = ixfs_alloc_block(vol);
        b2 = ixfs_alloc_block(vol);
        b3 = ixfs_alloc_block(vol);

        g1 = b1 / IXFS_BLOCKS_PER_GROUP;
        g2 = b2 / IXFS_BLOCKS_PER_GROUP;
        g3 = b3 / IXFS_BLOCKS_PER_GROUP;

        /* Locality: all 3 in same group */
        pass = (b1 != 0 && b2 != 0 && b3 != 0 && g1 == g2 && g2 == g3);
        klog(LOG_DEBUG, "TEST", "Block groups: alloc 3 blocks -> group %u, locality=%s",
               pass ? "OK" : "FAIL", (uint64_t)g1, pass ? "yes" : "NO");

        /* Hint advancement: blocks should be sequential */
        pass = (b2 == b1 + 1 && b3 == b2 + 1);
        klog(LOG_DEBUG, "TEST", "Block groups: hint b%u->b%u->b%u, sequential=%s",
               pass ? "OK" : "FAIL",
               (uint64_t)b1, (uint64_t)b2, (uint64_t)b3,
               pass ? "yes" : "NO");

        /* All allocated blocks must be >= data_start */
        pass = (b1 >= vol->sb.s_data_start);
        klog(LOG_DEBUG, "TEST", "Block groups: data_start guard (b1=%u >= %u)",
               pass ? "OK" : "FAIL",
               (uint64_t)b1, (uint64_t)vol->sb.s_data_start);

        /* Free and verify count restored */
        ixfs_free_block(vol, b1);
        ixfs_free_block(vol, b2);
        ixfs_free_block(vol, b3);

        pass = (vol->sb.s_free_blocks == saved_free);
        klog(LOG_DEBUG, "TEST", "Block groups: free restored (%u/%u)",
               pass ? "OK" : "FAIL",
               (uint64_t)vol->sb.s_free_blocks, (uint64_t)saved_free);
    }

    /* --- Test 2: Buffer Cache --- */
    {
        uint32_t test_blk;
        uint8_t write_buf[IXFS_BLOCK_SIZE];
        uint8_t read_buf1[IXFS_BLOCK_SIZE];
        uint8_t read_buf2[IXFS_BLOCK_SIZE];
        uint32_t i;
        int match;

        test_blk = ixfs_alloc_block(vol);
        if (test_blk == 0) {
            klog(LOG_DEBUG, "TEST", "FAIL: Cache: cannot allocate test block");
        } else {
            /* Write a known pattern via cache */
            for (i = 0; i < IXFS_BLOCK_SIZE; i++)
                write_buf[i] = (uint8_t)(i & 0xFF);
            ixfs_write_block(vol, test_blk, write_buf);

            /* Two reads -- both should come from cache */
            ixfs_read_block(vol, test_blk, read_buf1);
            ixfs_read_block(vol, test_blk, read_buf2);

            match = 1;
            for (i = 0; i < IXFS_BLOCK_SIZE; i++) {
                if (read_buf1[i] != write_buf[i] ||
                    read_buf2[i] != write_buf[i]) {
                    match = 0;
                    break;
                }
            }

            klog(LOG_DEBUG, "TEST", "Buffer cache: write->read integrity=%s (block %u)",
                   match ? "OK" : "FAIL",
                   match ? "ok" : "CORRUPTED", (uint64_t)test_blk);

            /* Check dirty state */
            {
                struct ixfs_cache_entry *ce = ixfs_cache_find(vol, test_blk);
                pass = (ce != (struct ixfs_cache_entry *)0 && ce->dirty == 1);
                klog(LOG_DEBUG, "TEST", "Buffer cache: cached=%s, dirty=%s",
                       pass ? "OK" : "FAIL",
                       ce ? "yes" : "no",
                       (ce && ce->dirty) ? "yes" : "no");
            }

            /* Flush and verify clean */
            ixfs_cache_flush(vol);
            {
                struct ixfs_cache_entry *ce = ixfs_cache_find(vol, test_blk);
                pass = (ce != (struct ixfs_cache_entry *)0 && ce->dirty == 0);
                klog(LOG_DEBUG, "TEST", "Buffer cache: post-flush dirty=%s",
                       pass ? "OK" : "FAIL",
                       (ce && ce->dirty) ? "yes" : "no");
            }

            ixfs_free_block(vol, test_blk);
        }
    }

    /* --- Test 3: Directory Hash Index --- */
    if (vfs_is_mounted('C')) {
        struct vfs_node *c_root = vfs_get_drive_root('C');
        if (c_root && c_root->ops && c_root->ops->create) {
            struct vfs_node *tdir;
            uint32_t created = 0, found = 0, i;
            char fname[8];

            c_root->ops->create(c_root, "_hashtest", VFS_DIRECTORY);
            tdir = c_root->ops->finddir(c_root, "_hashtest");

            if (!tdir) {
                klog(LOG_DEBUG, "TEST", "FAIL: Hash index: cannot create _hashtest dir");
            } else {
                /* Create 70 files (exceeds threshold of 64) */
                for (i = 0; i < 70; i++) {
                    fname[0] = 'h'; fname[1] = 'f'; fname[2] = '_';
                    fname[3] = (char)('0' + (i / 10));
                    fname[4] = (char)('0' + (i % 10));
                    fname[5] = '\0';
                    if (tdir->ops && tdir->ops->create &&
                        tdir->ops->create(tdir, fname, VFS_FILE) == 0)
                        created++;
                }

                klog(LOG_DEBUG, "TEST", "Hash index: created %u/70 files (threshold=%u)",
                       created >= 65 ? "OK" : "FAIL",
                       (uint64_t)created, (uint64_t)IXFS_HASH_THRESHOLD);

                /* Lookup 7 files (every 10th) -- triggers hash build */
                for (i = 0; i < 70; i += 10) {
                    fname[0] = 'h'; fname[1] = 'f'; fname[2] = '_';
                    fname[3] = (char)('0' + (i / 10));
                    fname[4] = (char)('0' + (i % 10));
                    fname[5] = '\0';
                    if (tdir->ops->finddir(tdir, fname))
                        found++;
                }

                pass = (found == 7);
                klog(LOG_DEBUG, "TEST", "Hash index: finddir found %u/7 via hash",
                       pass ? "OK" : "FAIL", (uint64_t)found);

                /* Verify hash table was actually built */
                {
                    struct ixfs_vnode *tv;
                    tv = (struct ixfs_vnode *)tdir->fs_data;
                    pass = (tv && tv->dir_hash != (void *)0);
                    if (pass) {
                        klog(LOG_DEBUG, "TEST", "Hash index: table built (%u nodes, %u buckets)\n",
                               (uint64_t)tv->dir_hash->node_count,
                               (uint64_t)IXFS_HASH_BUCKETS);
                    } else {
                        klog(LOG_DEBUG, "TEST", "FAIL: Hash index: table NOT built");
                    }
                }

                /* Cleanup */
                for (i = 0; i < 70; i++) {
                    fname[0] = 'h'; fname[1] = 'f'; fname[2] = '_';
                    fname[3] = (char)('0' + (i / 10));
                    fname[4] = (char)('0' + (i % 10));
                    fname[5] = '\0';
                    if (tdir->ops && tdir->ops->unlink)
                        tdir->ops->unlink(tdir, fname);
                }
                c_root->ops->unlink(c_root, "_hashtest");
            }
        }
    } else {
        klog(LOG_DEBUG, "TEST", "Hash index: C:\\ not mounted");
    }

    /* --- Test 4: Extent-Based Allocation --- */
    if (vfs_is_mounted('C')) {
        struct vfs_node *c_root = vfs_get_drive_root('C');
        if (c_root && c_root->ops && c_root->ops->create) {
            int rc = c_root->ops->create(c_root, "_extent_test.dat", VFS_FILE);
            if (rc != 0) {
                klog(LOG_DEBUG, "TEST", "FAIL: Extents: cannot create test file");
            } else {
                struct vfs_node *f = vfs_open("C:\\_extent_test.dat", VFS_O_WRITE);
                if (f) {
                    struct ixfs_vnode *fv;
                    uint8_t wbuf[128];
                    uint32_t wi;

                    for (wi = 0; wi < 128; wi++)
                        wbuf[wi] = (uint8_t)(wi & 0xFF);

                    /* Write 5 blocks: each write goes to a different block */
                    for (wi = 0; wi < 5; wi++)
                        vfs_write(f, wi * IXFS_BLOCK_SIZE, 128, wbuf);

                    vfs_close(f);

                    /* Check extent layout */
                    f = vfs_open("C:\\_extent_test.dat", VFS_O_READ);
                    if (f) {
                        fv = (struct ixfs_vnode *)f->fs_data;
                        pass = (fv->inode.i_extent_count == 1 &&
                                fv->inode.i_extents[0].e_count == 5);
                        klog(LOG_DEBUG, "TEST", "Extents: 5 blocks merged into %u extent(s) (count=%u)",
                               (uint64_t)fv->inode.i_extent_count,
                               (uint64_t)fv->inode.i_extents[0].e_count);

                        klog(LOG_DEBUG, "TEST", "Extents: 64-bit block addressing (start=%u, max 64 TiB)",
                               (uint64_t)fv->inode.i_extents[0].e_start);

                        /* Verify data integrity */
                        {
                            uint8_t rbuf[128];
                            int n = vfs_read(f, 0, 128, rbuf);
                            int ok = 1;
                            if (n == 128) {
                                for (wi = 0; wi < 128; wi++) {
                                    if (rbuf[wi] != (uint8_t)(wi & 0xFF)) {
                                        ok = 0; break;
                                    }
                                }
                            } else { ok = 0; }
                            klog(LOG_DEBUG, "TEST", "Extents: read-back data integrity",
                                   ok ? "OK" : "FAIL");
                        }
                        vfs_close(f);
                    }
                }
                c_root->ops->unlink(c_root, "_extent_test.dat");
            }
        }
    }
    /* --- Test 5: Write-Ahead Log (Journal) --- */
    {
        uint8_t *jbuf = (uint8_t *)kmalloc(IXFS_BLOCK_SIZE);
        if (jbuf) {
            struct ixfs_journal_header *jh;
            uint32_t j_start = vol->sb.s_journal_start;
            uint32_t j_blocks = vol->sb.s_journal_blocks;

            /* Re-read superblock from disk to get latest values */
            if (j_start == 0) {
                struct ixfs_superblock fresh_sb;
                uint8_t *sbuf = (uint8_t *)kmalloc(IXFS_BLOCK_SIZE);
                if (sbuf) {
                    ixfs_disk_read(vol, 0, sbuf);
                    {
                        uint8_t *d = (uint8_t *)&fresh_sb;
                        uint32_t k;
                        for (k = 0; k < sizeof(struct ixfs_superblock); k++)
                            d[k] = sbuf[k];
                    }
                    j_start = fresh_sb.s_journal_start;
                    j_blocks = fresh_sb.s_journal_blocks;
                    /* Update volume's superblock with correct values */
                    vol->sb.s_journal_start = j_start;
                    vol->sb.s_journal_blocks = j_blocks;
                    vol->sb.s_journal_seq = fresh_sb.s_journal_seq;
                    kfree(sbuf);
                }
            }

            if (j_start != 0) {
                /* Read journal header from disk */
                ixfs_disk_read(vol, j_start, jbuf);
                jh = (struct ixfs_journal_header *)jbuf;
                pass = (jh->jh_magic == IXFS_JOURNAL_MAGIC);
                klog(LOG_DEBUG, "TEST", "Journal: header magic=0x%x (area=%u blocks at block %u)",
                       (uint64_t)jh->jh_magic,
                       (uint64_t)j_blocks,
                       (uint64_t)j_start);

                /* Test transaction cycle */
                {
                    uint32_t pre_seq = vol->sb.s_journal_seq;
                    ixfs_txn_begin(vol);
                    pass = (vol->txn.active == 1 &&
                            vol->txn.txn_id == pre_seq + 1);
                    klog(LOG_DEBUG, "TEST", "Journal: txn_begin (id=%u, active=%s)",
                           pass ? "OK" : "FAIL",
                           (uint64_t)vol->txn.txn_id,
                           vol->txn.active ? "yes" : "no");

                    /* Commit empty transaction */
                    ixfs_txn_commit(vol);
                    pass = (vol->txn.active == 0 &&
                            vol->sb.s_journal_seq == pre_seq + 1);
                    klog(LOG_DEBUG, "TEST", "Journal: txn_commit (seq=%u, active=%s)",
                           pass ? "OK" : "FAIL",
                           (uint64_t)vol->sb.s_journal_seq,
                           vol->txn.active ? "yes" : "no");
                }
            } else {
                klog(LOG_DEBUG, "TEST", "Journal: no journal area on this volume");
            }
            kfree(jbuf);
        }
    }
    /* --- Test 6: Copy-on-Write + Snapshots --- */
    {
        if (vol->refcount_table) {
            /* Test refcount table */
            uint32_t rc_ok = 1;
            uint32_t i2;
            /* All metadata blocks should have refcount 1 */
            for (i2 = 0; i2 < vol->sb.s_data_start && i2 < vol->refcount_bytes; i2++) {
                if (bitmap_test(vol->block_bitmap, i2) &&
                    vol->refcount_table[i2] == 0)
                    rc_ok = 0;
            }
            klog(LOG_DEBUG, "TEST", "CoW: refcount table initialized (%u blocks tracked)",
                   rc_ok ? "OK" : "FAIL",
                   (uint64_t)vol->refcount_bytes);

            /* Test snapshot create */
            {
                uint32_t pre_count = vol->sb.s_snapshot_count;
                int create_ok = ixfs_snapshot_create("test_snap");
                pass = (create_ok == 0 &&
                        vol->sb.s_snapshot_count == pre_count + 1);
                klog(LOG_DEBUG, "TEST", "Snapshot: create \"test_snap\" (count=%u)",
                       pass ? "OK" : "FAIL",
                       (uint64_t)vol->sb.s_snapshot_count);
            }

            /* Test snapshot list */
            {
                int n = ixfs_snapshot_list();
                pass = (n >= 1);
                klog(LOG_DEBUG, "TEST", "Snapshot: list found %d snapshot(s)",
                       pass ? "OK" : "FAIL", n);
            }

            /* Test snapshot delete */
            {
                uint32_t pre_count = vol->sb.s_snapshot_count;
                int del_ok = ixfs_snapshot_delete("test_snap");
                pass = (del_ok == 0 &&
                        vol->sb.s_snapshot_count == pre_count - 1);
                klog(LOG_DEBUG, "TEST", "Snapshot: delete \"test_snap\" (count=%u)",
                       pass ? "OK" : "FAIL",
                       (uint64_t)vol->sb.s_snapshot_count);
            }
        } else {
            klog(LOG_DEBUG, "TEST", "CoW: no refcount table");
        }
    }
    /* --- Test 7: Sparse File Support --- */
    {
        struct vfs_node *c_root = vfs_get_drive_root('C');
        struct vfs_node *sfile;
        /* Create test file via VFS */
        if (c_root && c_root->ops && c_root->ops->create)
            c_root->ops->create(c_root, "_sparse_test.bin", 0);
        sfile = vfs_open("C:\\_sparse_test.bin", VFS_O_WRITE);
        if (sfile) {
            const char *sparse_data = "SPARSE_CONTENT!";
            uint32_t sparse_len = 15;
            uint32_t sparse_offset = IXFS_BLOCK_SIZE * 2; /* skip 2 blocks */
            int wrote;
            uint8_t rbuf[32];
            int rd;
            uint32_t log_sz = 0, act_blk = 0;

            /* Write data at block 2, leaving blocks 0-1 as holes */
            wrote = vfs_write(sfile, sparse_offset, sparse_len,
                              (const uint8_t *)sparse_data);
            pass = (wrote == (int)sparse_len);
            klog(LOG_DEBUG, "TEST", "Sparse: write at offset %u (%d bytes)",
                   pass ? "OK" : "FAIL",
                   (uint64_t)sparse_offset, wrote);
            vfs_close(sfile);

            /* Re-open for reading */
            sfile = vfs_open("C:\\_sparse_test.bin", VFS_O_READ);
            if (sfile) {
                /* Read hole at block 0: should return zeroes */
                uint32_t ki;
                int all_zero = 1;
                rd = vfs_read(sfile, 0, 16, rbuf);
                for (ki = 0; ki < 16 && ki < (uint32_t)rd; ki++) {
                    if (rbuf[ki] != 0)
                        all_zero = 0;
                }
                pass = (rd == 16 && all_zero);
                klog(LOG_DEBUG, "TEST", "Sparse: hole read returns zeroes (%d bytes, zero=%s)",
                       pass ? "OK" : "FAIL", rd,
                       all_zero ? "yes" : "no");

                /* Stat: logical size vs actual blocks */
                ixfs_stat(sfile, &log_sz, &act_blk);
                pass = (log_sz > act_blk * IXFS_BLOCK_SIZE);
                klog(LOG_DEBUG, "TEST", "Sparse: stat logical=%u, actual=%u blk (sparse=%s)",
                       pass ? "OK" : "FAIL",
                       (uint64_t)log_sz, (uint64_t)act_blk,
                       (log_sz > act_blk * IXFS_BLOCK_SIZE) ? "yes" : "no");

                vfs_close(sfile);
            }
            c_root->ops->unlink(c_root, "_sparse_test.bin");
        } else {
            klog(LOG_DEBUG, "TEST", "Sparse: cannot create test file");
        }
    }
    /* --- Test 8: Inline Small Files --- */
    if (vfs_is_mounted('C')) {
        struct vfs_node *c_root = vfs_get_drive_root('C');
        if (c_root && c_root->ops && c_root->ops->create) {
            /* Create a test file -- should start inline */
            c_root->ops->create(c_root, "_inline_test.txt", VFS_FILE);
            {
                struct vfs_node *inf = vfs_open("C:\\_inline_test.txt", VFS_O_WRITE);
                if (inf) {
                    const char *small = "Hello inline!";    /* 13 bytes, fits in 48 */
                    uint32_t small_len = 13;
                    struct ixfs_vnode *iv;

                    /* Subtest 1: write small data -- should stay inline */
                    vfs_write(inf, 0, small_len, (const uint8_t *)small);
                    vfs_close(inf);

                    inf = vfs_open("C:\\_inline_test.txt", VFS_O_READ);
                    if (inf) {
                        iv = (struct ixfs_vnode *)inf->fs_data;
                        pass = (iv->inode.i_extent_flags & IXFS_INLINE) &&
                               (iv->inode.i_blocks == 0) &&
                               (iv->inode.i_size == small_len);
                        klog(LOG_DEBUG, "TEST", "Inline: write %u B, flag=INLINE, blocks=%u",
                               pass ? "OK" : "FAIL",
                               (uint64_t)small_len,
                               (uint64_t)iv->inode.i_blocks);

                        /* Subtest 2: read back inline data */
                        {
                            uint8_t rbuf2[48];
                            int rd2 = vfs_read(inf, 0, small_len, rbuf2);
                            int match2 = 1;
                            uint32_t qi;
                            if (rd2 == (int)small_len) {
                                for (qi = 0; qi < small_len; qi++) {
                                    if (rbuf2[qi] != (uint8_t)small[qi]) {
                                        match2 = 0; break;
                                    }
                                }
                            } else { match2 = 0; }
                            klog(LOG_DEBUG, "TEST", "Inline: read-back integrity=%s",
                                   match2 ? "OK" : "FAIL",
                                   match2 ? "ok" : "CORRUPTED");
                        }
                        vfs_close(inf);

                        /* Subtest 3: write >48 bytes -- triggers promotion */
                        inf = vfs_open("C:\\_inline_test.txt", VFS_O_WRITE);
                        if (inf) {
                            const char *big = "This string is definitely longer than forty-eight bytes of data!";
                            uint32_t big_len = 64;
                            vfs_write(inf, 0, big_len, (const uint8_t *)big);
                            vfs_close(inf);

                            inf = vfs_open("C:\\_inline_test.txt", VFS_O_READ);
                            if (inf) {
                                iv = (struct ixfs_vnode *)inf->fs_data;
                                pass = !(iv->inode.i_extent_flags & IXFS_INLINE) &&
                                       (iv->inode.i_blocks > 0) &&
                                       (iv->inode.i_size == big_len);
                                klog(LOG_DEBUG, "TEST", "Inline: promotion at %u B, blocks=%u",
                                       pass ? "OK" : "FAIL",
                                       (uint64_t)big_len,
                                       (uint64_t)iv->inode.i_blocks);

                                /* Subtest 4: read after promotion */
                                {
                                    uint8_t rbuf3[64];
                                    int rd3 = vfs_read(inf, 0, big_len, rbuf3);
                                    int match3 = 1;
                                    uint32_t qi2;
                                    if (rd3 == (int)big_len) {
                                        for (qi2 = 0; qi2 < big_len; qi2++) {
                                            if (rbuf3[qi2] != (uint8_t)big[qi2]) {
                                                match3 = 0; break;
                                            }
                                        }
                                    } else { match3 = 0; }
                                    klog(LOG_DEBUG, "TEST", "Inline: post-promotion read=%s",
                                           match3 ? "OK" : "FAIL",
                                           match3 ? "ok" : "CORRUPTED");
                                }
                                vfs_close(inf);
                            }
                        }
                    }
                }
                c_root->ops->unlink(c_root, "_inline_test.txt");
            }
        }
    }
    /* --- Test 9: Per-Block Checksums --- */
    {
        /* Subtest 1: checksum table loaded */
        pass = (vol->checksum_table != (uint32_t *)0 &&
                vol->checksum_count == vol->sb.s_total_blocks);
        klog(LOG_DEBUG, "TEST", "Checksums: table loaded (%u entries)",
               pass ? "OK" : "FAIL",
               (uint64_t)vol->checksum_count);

        /* Subtest 2: write updates checksum (non-zero) */
        if (vol->checksum_table) {
            uint32_t test_blk = ixfs_alloc_block(vol);
            if (test_blk != 0) {
                uint8_t *tbuf = (uint8_t *)kmalloc(IXFS_BLOCK_SIZE);
                if (tbuf) {
                    uint32_t qi;
                    for (qi = 0; qi < IXFS_BLOCK_SIZE; qi++)
                        tbuf[qi] = (uint8_t)(qi & 0xFF);
                    ixfs_write_block(vol, test_blk, tbuf);
                    pass = (vol->checksum_table[test_blk] != 0);
                    klog(LOG_DEBUG, "TEST", "Checksums: write updated crc=0x%x",
                           pass ? "OK" : "FAIL",
                           (uint64_t)vol->checksum_table[test_blk]);
                    kfree(tbuf);
                }
                ixfs_free_block(vol, test_blk);
            }
        }

        /* Subtest 3: superblock self-checksum */
        {
            uint32_t saved_ck, computed_ck;
            /* Flush first so s_checksum reflects current in-memory state */
            ixfs_flush_superblock(vol);
            saved_ck = vol->sb.s_checksum;
            vol->sb.s_checksum = 0;
            computed_ck = ixfs_crc32c(&vol->sb, 112);
            vol->sb.s_checksum = saved_ck;
            pass = (saved_ck != 0 && computed_ck == saved_ck);
            klog(LOG_DEBUG, "TEST", "Checksums: superblock self-check (0x%x)",
                   pass ? "OK" : "FAIL",
                   (uint64_t)saved_ck);
        }

        /* Subtest 4: scrub reports 0 corruptions */
        {
            int scrub_res = ixfs_scrub();
            pass = (scrub_res == 0);
            klog(LOG_DEBUG, "TEST", "Checksums: scrub result=%d corruptions",
                   pass ? "OK" : "FAIL", scrub_res);
        }
    }

    /* --- Test 10: 64-Bit Block Addressing --- */
    {
        /* Subtest 1: version is 2 */
        pass = (vol->sb.s_version == 2);
        klog(LOG_DEBUG, "TEST", "64-bit: s_version=%u (expected 2)",
               pass ? "OK" : "FAIL",
               (uint64_t)vol->sb.s_version);

        /* Subtest 2: s_total_blocks is 64-bit (8 bytes) */
        pass = (sizeof(vol->sb.s_total_blocks) == 8);
        klog(LOG_DEBUG, "TEST", "64-bit: sizeof(s_total_blocks)=%u (expected 8)",
               pass ? "OK" : "FAIL",
               (uint64_t)sizeof(vol->sb.s_total_blocks));

        /* Subtest 3: i_size is 64-bit (8 bytes) */
        {
            struct ixfs_inode tmp_inode;
            pass = (sizeof(tmp_inode.i_size) == 8);
            klog(LOG_DEBUG, "TEST", "64-bit: sizeof(i_size)=%u (expected 8)",
                   pass ? "OK" : "FAIL",
                   (uint64_t)sizeof(tmp_inode.i_size));
        }
    }

    klog(LOG_DEBUG, "TEST", "--- IXFS Performance Tests Complete ---");
}

#endif /* KERNEL_TESTS */
