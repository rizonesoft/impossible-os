/* ============================================================================
 * mkfs-ixfs.c — Host-side IXFS v2 image formatter
 *
 * Standalone tool (runs on Linux/macOS) to create IXFS disk images.
 *
 * Usage:
 *   mkfs-ixfs -o output.img -s 32M [-l "Label"] [--populate dir/]
 *
 * Layout (same as kernel ixfs_format):
 *   Block 0:              Superblock
 *   Blocks 1..B:          Block bitmap
 *   Blocks B+1..B+C:      Checksum table (CRC32C per block)
 *   Blocks B+C+1..B+C+I:  Inode table
 *   Blocks ..+16:         Journal
 *   Blocks ..+2:          Refcount table
 *   Blocks ..+1:          Snapshot table
 *   Blocks data_start..:  Data blocks
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>

/* ---- IXFS constants (must match kernel ixfs.h) ---- */

#define IXFS_MAGIC           0x49584653
#define IXFS_VERSION         2
#define IXFS_BLOCK_SIZE      4096
#define IXFS_DEFAULT_INODES  128
#define IXFS_MAX_NAME        252
#define IXFS_ROOT_INODE      1
#define IXFS_INLINE_EXTENTS  4
#define IXFS_INLINE_MAX      48
#define IXFS_INLINE_FLAG     0x02
#define IXFS_JOURNAL_BLOCKS  16
#define IXFS_JOURNAL_MAGIC   0x4A584653
#define IXFS_REFCOUNT_BLOCKS 2
#define IXFS_SNAPSHOT_BLOCKS 1
#define IXFS_S_FILE          0x8000
#define IXFS_S_DIR           0x4000
#define IXFS_PERM_FILE       0x01B4
#define IXFS_PERM_DIR        0x01ED

/* ---- On-disk structures (packed, must match kernel) ---- */

#pragma pack(push, 1)

struct ixfs_extent {
    uint64_t e_start;
    uint32_t e_count;
};

struct ixfs_superblock {
    uint32_t s_magic;
    uint32_t s_version;
    uint32_t s_block_size;
    uint64_t s_total_blocks;
    uint64_t s_free_blocks;
    uint32_t s_total_inodes;
    uint32_t s_free_inodes;
    uint32_t s_bitmap_start;
    uint32_t s_bitmap_blocks;
    uint32_t s_inode_start;
    uint32_t s_inode_blocks;
    uint32_t s_data_start;
    uint32_t s_root_inode;
    uint8_t  s_volume_name[32];
    uint32_t s_journal_start;
    uint32_t s_journal_blocks;
    uint32_t s_journal_seq;
    uint32_t s_refcount_start;
    uint32_t s_refcount_blocks;
    uint32_t s_snapshot_start;
    uint32_t s_snapshot_count;
    uint32_t s_checksum_start;
    uint32_t s_checksum_blocks;
    uint32_t s_checksum;
    uint8_t  s_reserved[380];
};

struct ixfs_inode {
    uint16_t i_mode;
    uint16_t i_links;
    uint16_t i_uid;
    uint16_t i_gid;
    uint64_t i_size;
    uint32_t i_blocks;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_atime;
    struct ixfs_extent i_extents[IXFS_INLINE_EXTENTS];
    uint8_t  i_extent_count;
    uint8_t  i_extent_flags;
    uint16_t i_extent_pad;
    uint64_t i_extent_block;
};

struct ixfs_dir_entry {
    uint32_t d_inode;
    char     d_name[IXFS_MAX_NAME];
};

struct ixfs_journal_header {
    uint32_t j_magic;
    uint32_t j_seq;
    uint32_t j_blocks;
    uint32_t j_head;
    uint32_t j_tail;
    uint32_t j_active;
};

#pragma pack(pop)

#define INODES_PER_BLOCK  (IXFS_BLOCK_SIZE / sizeof(struct ixfs_inode))
#define DIRENTS_PER_BLOCK (IXFS_BLOCK_SIZE / sizeof(struct ixfs_dir_entry))

/* ---- Image state ---- */

static uint8_t *g_disk;           /* allocated disk image */
static uint64_t g_disk_size;
static struct ixfs_superblock g_sb;
static uint32_t g_next_inode = 2; /* 0=reserved, 1=root */
static uint32_t *g_checksum_table;

/* ---- CRC32C (Castagnoli) ---- */

static uint32_t crc32c(const void *data, size_t len)
{
    static uint32_t table[256];
    static int table_init = 0;
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFF;
    size_t i;
    int j;

    if (!table_init) {
        for (i = 0; i < 256; i++) {
            uint32_t c = (uint32_t)i;
            for (j = 0; j < 8; j++) {
                if (c & 1) c = (c >> 1) ^ 0x82F63B78;
                else       c >>= 1;
            }
            table[i] = c;
        }
        table_init = 1;
    }

    for (i = 0; i < len; i++)
        crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);

    return crc ^ 0xFFFFFFFF;
}

/* ---- Block helpers ---- */

static uint8_t *block_ptr(uint32_t block)
{
    return g_disk + (uint64_t)block * IXFS_BLOCK_SIZE;
}

static void bitmap_set(uint32_t block)
{
    uint8_t *bitmap = block_ptr(g_sb.s_bitmap_start);
    bitmap[block / 8] |= (uint8_t)(1 << (block % 8));
}

static int bitmap_test(uint32_t block)
{
    uint8_t *bitmap = block_ptr(g_sb.s_bitmap_start);
    return (bitmap[block / 8] >> (block % 8)) & 1;
}

static uint32_t alloc_block(void)
{
    uint32_t i;
    for (i = g_sb.s_data_start; i < (uint32_t)g_sb.s_total_blocks; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            g_sb.s_free_blocks--;
            return i;
        }
    }
    fprintf(stderr, "mkfs-ixfs: out of blocks\n");
    exit(1);
}

static uint32_t alloc_inode(void)
{
    uint32_t ino = g_next_inode++;
    if (ino >= g_sb.s_total_inodes) {
        fprintf(stderr, "mkfs-ixfs: out of inodes\n");
        exit(1);
    }
    g_sb.s_free_inodes--;
    return ino;
}

/* ---- Inode access ---- */

static struct ixfs_inode *inode_ptr(uint32_t ino)
{
    uint32_t block = g_sb.s_inode_start + (uint32_t)(ino / INODES_PER_BLOCK);
    uint32_t offset = (uint32_t)((ino % INODES_PER_BLOCK) *
                      sizeof(struct ixfs_inode));
    return (struct ixfs_inode *)(block_ptr(block) + offset);
}

/* ---- Directory helpers ---- */

static void dir_add_entry(uint32_t parent_ino, const char *name,
                          uint32_t child_ino)
{
    struct ixfs_inode *parent = inode_ptr(parent_ino);
    uint32_t blk;
    struct ixfs_dir_entry *de;
    uint32_t i, max_entries;
    uint8_t *data;

    if (parent->i_extent_count == 0 || parent->i_extents[0].e_count == 0) {
        /* Allocate first directory data block */
        blk = alloc_block();
        memset(block_ptr(blk), 0, IXFS_BLOCK_SIZE);
        parent->i_extents[0].e_start = blk;
        parent->i_extents[0].e_count = 1;
        parent->i_extent_count = 1;
        parent->i_blocks = 1;
    }

    blk = (uint32_t)parent->i_extents[0].e_start;
    data = block_ptr(blk);
    max_entries = (uint32_t)DIRENTS_PER_BLOCK;

    /* Find first free slot */
    for (i = 0; i < max_entries; i++) {
        de = (struct ixfs_dir_entry *)(data +
             i * sizeof(struct ixfs_dir_entry));
        if (de->d_inode == 0) {
            de->d_inode = child_ino;
            strncpy(de->d_name, name, IXFS_MAX_NAME - 1);
            de->d_name[IXFS_MAX_NAME - 1] = '\0';
            parent->i_size = (uint64_t)(i + 1) *
                             sizeof(struct ixfs_dir_entry);
            return;
        }
    }

    fprintf(stderr, "mkfs-ixfs: directory full (max %u entries)\n",
            max_entries);
    exit(1);
}

static uint32_t create_directory(uint32_t parent_ino, const char *name)
{
    uint32_t ino = alloc_inode();
    struct ixfs_inode *inode = inode_ptr(ino);

    memset(inode, 0, sizeof(*inode));
    inode->i_mode = IXFS_S_DIR | IXFS_PERM_DIR;
    inode->i_links = 1;

    /* Add . and .. entries */
    dir_add_entry(ino, ".", ino);
    dir_add_entry(ino, "..", parent_ino);

    /* Add to parent directory */
    if (name)
        dir_add_entry(parent_ino, name, ino);

    return ino;
}

static uint32_t create_file(uint32_t parent_ino, const char *name,
                            const uint8_t *fdata, uint64_t size)
{
    uint32_t ino = alloc_inode();
    struct ixfs_inode *inode = inode_ptr(ino);

    memset(inode, 0, sizeof(*inode));
    inode->i_mode = IXFS_S_FILE | IXFS_PERM_FILE;
    inode->i_links = 1;
    inode->i_size = size;

    if (size <= IXFS_INLINE_MAX) {
        /* Inline: store data directly in extent area */
        memcpy(inode->i_extents, fdata, (size_t)size);
        inode->i_extent_flags = IXFS_INLINE_FLAG;
        inode->i_blocks = 0;
    } else {
        /* Normal: allocate data blocks */
        uint64_t remaining = size;
        uint32_t ext_idx = 0;
        const uint8_t *src = fdata;

        while (remaining > 0 && ext_idx < IXFS_INLINE_EXTENTS) {
            uint32_t blk = alloc_block();
            uint32_t chunk = remaining > IXFS_BLOCK_SIZE ?
                             IXFS_BLOCK_SIZE : (uint32_t)remaining;

            memcpy(block_ptr(blk), src, chunk);
            if (chunk < IXFS_BLOCK_SIZE)
                memset(block_ptr(blk) + chunk, 0,
                       IXFS_BLOCK_SIZE - chunk);

            /* Try to extend previous extent */
            if (ext_idx > 0 &&
                inode->i_extents[ext_idx - 1].e_start +
                inode->i_extents[ext_idx - 1].e_count == blk) {
                inode->i_extents[ext_idx - 1].e_count++;
            } else {
                inode->i_extents[ext_idx].e_start = blk;
                inode->i_extents[ext_idx].e_count = 1;
                ext_idx++;
            }

            inode->i_blocks++;
            src += chunk;
            remaining -= chunk;
        }
        inode->i_extent_count = (uint8_t)ext_idx;
    }

    /* Add to parent directory */
    dir_add_entry(parent_ino, name, ino);
    return ino;
}

/* ---- Populate from host directory ---- */

static void populate_dir(uint32_t ixfs_parent_ino, const char *host_path)
{
    DIR *d = opendir(host_path);
    struct dirent *ent;
    char child_path[4096];

    if (!d) {
        fprintf(stderr, "mkfs-ixfs: cannot open '%s': %s\n",
                host_path, strerror(errno));
        return;
    }

    while ((ent = readdir(d)) != NULL) {
        struct stat st;

        if (strcmp(ent->d_name, ".") == 0 ||
            strcmp(ent->d_name, "..") == 0)
            continue;

        snprintf(child_path, sizeof(child_path), "%s/%s",
                 host_path, ent->d_name);

        if (stat(child_path, &st) != 0) {
            fprintf(stderr, "  skip '%s': %s\n",
                    child_path, strerror(errno));
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            uint32_t cino = create_directory(ixfs_parent_ino, ent->d_name);
            printf("  mkdir %s/ (inode %u)\n", ent->d_name, cino);
            populate_dir(cino, child_path);
        } else if (S_ISREG(st.st_mode)) {
            FILE *fp = fopen(child_path, "rb");
            uint8_t *buf;
            uint32_t fino;

            if (!fp) continue;

            buf = (uint8_t *)calloc(1, (size_t)(st.st_size + 1));
            if (!buf) { fclose(fp); continue; }

            if (st.st_size > 0) {
                size_t rd = fread(buf, 1, (size_t)st.st_size, fp);
                if (rd < (size_t)st.st_size)
                    memset(buf + rd, 0, (size_t)st.st_size - rd);
            }
            fclose(fp);

            fino = create_file(ixfs_parent_ino, ent->d_name,
                               buf, (uint64_t)st.st_size);
            printf("  file  %s (%lld B, inode %u%s)\n",
                   ent->d_name, (long long)st.st_size, fino,
                   st.st_size <= IXFS_INLINE_MAX ? ", inline" : "");
            free(buf);
        }
    }

    closedir(d);
}

/* ---- Parse size string ---- */

static uint64_t parse_size(const char *str)
{
    char *end;
    uint64_t val = strtoull(str, &end, 10);

    if (*end == 'K' || *end == 'k') val *= 1024;
    else if (*end == 'M' || *end == 'm') val *= 1024 * 1024;
    else if (*end == 'G' || *end == 'g') val *= 1024ULL * 1024 * 1024;

    return val;
}

/* ---- Main ---- */

int main(int argc, char *argv[])
{
    const char *output = NULL;
    const char *label = "IXFS";
    const char *populate = NULL;
    uint64_t size = 32ULL * 1024 * 1024;
    uint64_t offset = 0;  /* byte offset into existing file */
    FILE *fp;
    uint32_t total_blocks, bitmap_blocks, inode_blocks, checksum_blocks;
    uint32_t used_blocks, i;

    /* Parse arguments */
    for (i = 1; i < (uint32_t)argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < (uint32_t)argc)
            output = argv[++i];
        else if (strcmp(argv[i], "-s") == 0 && i + 1 < (uint32_t)argc)
            size = parse_size(argv[++i]);
        else if (strcmp(argv[i], "-l") == 0 && i + 1 < (uint32_t)argc)
            label = argv[++i];
        else if (strcmp(argv[i], "--populate") == 0 &&
                 i + 1 < (uint32_t)argc)
            populate = argv[++i];
        else if (strcmp(argv[i], "--offset") == 0 &&
                 i + 1 < (uint32_t)argc)
            offset = parse_size(argv[++i]);
        else if (strcmp(argv[i], "-h") == 0 ||
                 strcmp(argv[i], "--help") == 0) {
            printf("Usage: mkfs-ixfs -o FILE [-s SIZE] [-l LABEL] "
                   "[--populate DIR] [--offset BYTES]\n\n"
                   "  -o FILE        Output image (required)\n"
                   "  -s SIZE        Volume size (default: 32M)\n"
                   "  -l LABEL       Volume label (default: IXFS)\n"
                   "  --populate DIR Copy host directory into image\n"
                   "  --offset BYTES Write at offset in existing file\n");
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return 1;
        }
    }

    if (!output) {
        fprintf(stderr, "mkfs-ixfs: -o output.img is required\n");
        return 1;
    }

    /* Allocate disk image */
    g_disk_size = size;
    g_disk = (uint8_t *)calloc(1, (size_t)g_disk_size);
    if (!g_disk) {
        fprintf(stderr, "mkfs-ixfs: cannot allocate %llu bytes\n",
                (unsigned long long)g_disk_size);
        return 1;
    }

    /* Compute layout */
    total_blocks = (uint32_t)(g_disk_size / IXFS_BLOCK_SIZE);
    bitmap_blocks = (total_blocks + IXFS_BLOCK_SIZE * 8 - 1) /
                    (IXFS_BLOCK_SIZE * 8);
    inode_blocks = (uint32_t)((IXFS_DEFAULT_INODES *
                   sizeof(struct ixfs_inode) + IXFS_BLOCK_SIZE - 1) /
                   IXFS_BLOCK_SIZE);
    checksum_blocks = (total_blocks * 4 + IXFS_BLOCK_SIZE - 1) /
                      IXFS_BLOCK_SIZE;
    used_blocks = 1 + bitmap_blocks + checksum_blocks + inode_blocks +
                  IXFS_JOURNAL_BLOCKS + IXFS_REFCOUNT_BLOCKS +
                  IXFS_SNAPSHOT_BLOCKS + 1;

    if (used_blocks >= total_blocks) {
        fprintf(stderr, "mkfs-ixfs: volume too small (%u blocks needed, "
                "%u available)\n", used_blocks, total_blocks);
        free(g_disk);
        return 1;
    }

    printf("mkfs-ixfs: %s — %u blocks (%llu bytes)\n",
           output, total_blocks, (unsigned long long)g_disk_size);

    /* Build superblock */
    memset(&g_sb, 0, sizeof(g_sb));
    g_sb.s_magic = IXFS_MAGIC;
    g_sb.s_version = IXFS_VERSION;
    g_sb.s_block_size = IXFS_BLOCK_SIZE;
    g_sb.s_total_blocks = total_blocks;
    g_sb.s_free_blocks = total_blocks - used_blocks;
    g_sb.s_total_inodes = IXFS_DEFAULT_INODES;
    g_sb.s_free_inodes = IXFS_DEFAULT_INODES - 2;
    g_sb.s_bitmap_start = 1;
    g_sb.s_bitmap_blocks = bitmap_blocks;
    g_sb.s_checksum_start = 1 + bitmap_blocks;
    g_sb.s_checksum_blocks = checksum_blocks;
    g_sb.s_inode_start = 1 + bitmap_blocks + checksum_blocks;
    g_sb.s_inode_blocks = inode_blocks;
    g_sb.s_data_start = 1 + bitmap_blocks + checksum_blocks +
                        inode_blocks + IXFS_JOURNAL_BLOCKS +
                        IXFS_REFCOUNT_BLOCKS + IXFS_SNAPSHOT_BLOCKS;
    g_sb.s_root_inode = IXFS_ROOT_INODE;
    strncpy((char *)g_sb.s_volume_name, label, 31);
    g_sb.s_journal_start = 1 + bitmap_blocks + checksum_blocks +
                           inode_blocks;
    g_sb.s_journal_blocks = IXFS_JOURNAL_BLOCKS;
    g_sb.s_journal_seq = 0;
    g_sb.s_refcount_start = g_sb.s_journal_start + IXFS_JOURNAL_BLOCKS;
    g_sb.s_refcount_blocks = IXFS_REFCOUNT_BLOCKS;
    g_sb.s_snapshot_start = g_sb.s_refcount_start + IXFS_REFCOUNT_BLOCKS;
    g_sb.s_snapshot_count = 0;

    /* Mark metadata blocks as used in bitmap */
    for (i = 0; i < used_blocks; i++)
        bitmap_set(i);

    /* Write journal header */
    {
        struct ixfs_journal_header *jh =
            (struct ixfs_journal_header *)block_ptr(g_sb.s_journal_start);
        jh->j_magic = IXFS_JOURNAL_MAGIC;
        jh->j_seq = 0;
        jh->j_blocks = IXFS_JOURNAL_BLOCKS;
        jh->j_head = 0;
        jh->j_tail = 0;
        jh->j_active = 0;
    }

    /* Create root directory (inode 1) with . and .. */
    {
        struct ixfs_inode *root = inode_ptr(IXFS_ROOT_INODE);
        uint32_t root_blk = g_sb.s_data_start;
        struct ixfs_dir_entry *de;

        memset(root, 0, sizeof(*root));
        root->i_mode = IXFS_S_DIR | IXFS_PERM_DIR;
        root->i_links = 1;
        root->i_extents[0].e_start = root_blk;
        root->i_extents[0].e_count = 1;
        root->i_extent_count = 1;
        root->i_blocks = 1;

        de = (struct ixfs_dir_entry *)block_ptr(root_blk);
        de[0].d_inode = IXFS_ROOT_INODE;
        strcpy(de[0].d_name, ".");
        de[1].d_inode = IXFS_ROOT_INODE;
        strcpy(de[1].d_name, "..");
        root->i_size = 2 * sizeof(struct ixfs_dir_entry);
    }

    /* Populate from host directory if requested */
    if (populate) {
        printf("Populating from: %s\n", populate);
        populate_dir(IXFS_ROOT_INODE, populate);
    }

    /* Compute per-block checksums for allocated data blocks */
    g_checksum_table = (uint32_t *)calloc(total_blocks, sizeof(uint32_t));
    if (g_checksum_table) {
        for (i = g_sb.s_data_start; i < total_blocks; i++) {
            if (bitmap_test(i))
                g_checksum_table[i] = crc32c(block_ptr(i),
                                             IXFS_BLOCK_SIZE);
        }
        /* Write checksum table to disk */
        memcpy(block_ptr(g_sb.s_checksum_start), g_checksum_table,
               total_blocks * sizeof(uint32_t));
    }

    /* Compute and write superblock checksum */
    g_sb.s_checksum = 0;
    g_sb.s_checksum = crc32c(&g_sb, 112);

    /* Copy superblock to disk image */
    memcpy(block_ptr(0), &g_sb, sizeof(g_sb));

    /* Write to file */
    if (offset > 0) {
        /* Write into existing file at offset (for GPT partition) */
        fp = fopen(output, "r+b");
        if (!fp) {
            fprintf(stderr, "mkfs-ixfs: cannot open '%s' for r+w: %s\n",
                    output, strerror(errno));
            free(g_disk);
            free(g_checksum_table);
            return 1;
        }
        if (fseeko(fp, (off_t)offset, SEEK_SET) != 0) {
            fprintf(stderr, "mkfs-ixfs: seek error to offset %llu\n",
                    (unsigned long long)offset);
            fclose(fp);
            free(g_disk);
            free(g_checksum_table);
            return 1;
        }
    } else {
        /* Create new standalone image */
        fp = fopen(output, "wb");
        if (!fp) {
            fprintf(stderr, "mkfs-ixfs: cannot create '%s': %s\n",
                    output, strerror(errno));
            free(g_disk);
            free(g_checksum_table);
            return 1;
        }
    }

    if (fwrite(g_disk, 1, (size_t)g_disk_size, fp) !=
        (size_t)g_disk_size) {
        fprintf(stderr, "mkfs-ixfs: write error\n");
        fclose(fp);
        free(g_disk);
        free(g_checksum_table);
        return 1;
    }

    fclose(fp);
    printf("mkfs-ixfs: done — %u/%u blocks used, %u inodes, "
           "label=\"%s\"\n",
           (uint32_t)(total_blocks - g_sb.s_free_blocks),
           total_blocks, g_sb.s_total_inodes, label);

    free(g_disk);
    free(g_checksum_table);
    return 0;
}
