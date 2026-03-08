/* ============================================================================
 * mkfs-ixfs.c — Host-side IXFS formatter
 *
 * Creates an IXFS filesystem on a raw disk image file.
 * Compiled with the host compiler: gcc -o mkfs-ixfs mkfs-ixfs.c
 *
 * Usage: mkfs-ixfs <image-file> [volume-name]
 *
 * The tool will:
 *   1. Write the IXFS superblock
 *   2. Initialize the block bitmap
 *   3. Zero the inode table
 *   4. Create root directory with . and ..
 *   5. Create default folder hierarchy (Impossible, Users, Programs)
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* --- IXFS constants (must match include/ixfs.h) --- */

#define IXFS_MAGIC           0x49584653
#define IXFS_VERSION         1
#define IXFS_BLOCK_SIZE      4096
#define IXFS_SECTORS_PER_BLK (IXFS_BLOCK_SIZE / 512)
#define IXFS_INLINE_EXTENTS  4
#define IXFS_MAX_NAME        252
#define IXFS_ROOT_INODE      1
#define IXFS_DEFAULT_INODES  256
#define IXFS_JOURNAL_BLOCKS  16
#define IXFS_JOURNAL_MAGIC   0x4A584653
#define IXFS_REFCOUNT_BLOCKS 2
#define IXFS_SNAPSHOT_BLOCKS 1

/* Inode type + permission flags */
#define IXFS_S_FILE          0x8000
#define IXFS_S_DIR           0x4000
#define IXFS_PERM_DIR        0x01ED  /* rwxrwxr-x */
#define IXFS_PERM_FILE       0x01B4  /* rw-rw-r-- */

/* --- On-disk structures (must match include/ixfs.h) --- */

struct ixfs_extent {
    uint64_t e_start;
    uint32_t e_count;
} __attribute__((packed));

struct ixfs_superblock {
    uint32_t s_magic;
    uint32_t s_version;
    uint32_t s_block_size;
    uint32_t s_total_blocks;
    uint32_t s_free_blocks;
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
    uint8_t  s_reserved[400];
} __attribute__((packed));

struct ixfs_inode {
    uint16_t i_mode;
    uint16_t i_links;
    uint16_t i_uid;
    uint16_t i_gid;
    uint32_t i_size;
    uint32_t i_blocks;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_atime;
    struct ixfs_extent i_extents[IXFS_INLINE_EXTENTS];
    uint8_t  i_extent_count;
    uint8_t  i_extent_flags;
    uint16_t i_extent_pad;
    uint64_t i_extent_block;
} __attribute__((packed));

struct ixfs_journal_header {
    uint32_t jh_magic;
    uint32_t jh_head;
    uint32_t jh_tail;
    uint32_t jh_seq;
} __attribute__((packed));

struct ixfs_dir_entry {
    uint32_t d_inode;
    char     d_name[IXFS_MAX_NAME];
} __attribute__((packed));

/* --- Globals --- */

static FILE *disk;
static struct ixfs_superblock sb;
static uint8_t *bitmap;
static uint32_t bitmap_bytes;
static uint32_t next_free_inode = 2; /* 0 reserved, 1 root */
static uint8_t block_buf[IXFS_BLOCK_SIZE];

/* --- Block I/O --- */

static int write_block(uint32_t block, const void *buf)
{
    if (fseek(disk, (long)block * IXFS_BLOCK_SIZE, SEEK_SET) != 0)
        return -1;
    if (fwrite(buf, IXFS_BLOCK_SIZE, 1, disk) != 1)
        return -1;
    return 0;
}

static int read_block(uint32_t block, void *buf)
{
    if (fseek(disk, (long)block * IXFS_BLOCK_SIZE, SEEK_SET) != 0)
        return -1;
    if (fread(buf, IXFS_BLOCK_SIZE, 1, disk) != 1)
        return -1;
    return 0;
}

/* --- Bitmap --- */

static void bitmap_set(uint32_t bit)
{
    bitmap[bit / 8] |= (uint8_t)(1 << (bit % 8));
}

static uint32_t alloc_block(void)
{
    uint32_t i;
    for (i = sb.s_data_start; i < sb.s_total_blocks; i++) {
        if (!(bitmap[i / 8] & (1 << (i % 8)))) {
            bitmap_set(i);
            sb.s_free_blocks--;
            return i;
        }
    }
    return 0;
}

/* --- Inode I/O --- */

static int write_inode(uint32_t ino, const struct ixfs_inode *inode)
{
    uint32_t inodes_per_block = IXFS_BLOCK_SIZE / sizeof(struct ixfs_inode);
    uint32_t block = sb.s_inode_start + (ino / inodes_per_block);
    uint32_t offset = (ino % inodes_per_block) * sizeof(struct ixfs_inode);

    if (read_block(block, block_buf) != 0)
        return -1;

    memcpy(block_buf + offset, inode, sizeof(struct ixfs_inode));
    return write_block(block, block_buf);
}

/* --- Directory helpers --- */

/* Create a directory and return its inode number.
 * parent_ino = inode of the parent directory.
 * name = name of the new directory (added to parent's dir block).
 * parent_data_block = data block of the parent to add entry to.
 */
static uint32_t create_dir(uint32_t parent_ino, uint32_t parent_data_block,
                           const char *name)
{
    uint32_t ino = next_free_inode++;
    uint32_t data_block;
    struct ixfs_inode inode;
    struct ixfs_dir_entry *de;
    struct ixfs_inode parent;

    /* Allocate a data block for the new directory */
    data_block = alloc_block();
    if (data_block == 0) return 0;

    /* Initialize inode */
    memset(&inode, 0, sizeof(inode));
    inode.i_mode = IXFS_S_DIR | IXFS_PERM_DIR;
    inode.i_links = 1;
    inode.i_size = 2 * sizeof(struct ixfs_dir_entry);  /* . and .. */
    inode.i_blocks = 1;
    inode.i_extents[0].e_start = (uint64_t)data_block;
    inode.i_extents[0].e_count = 1;
    inode.i_extent_count = 1;

    write_inode(ino, &inode);
    sb.s_free_inodes--;

    /* Write . and .. entries in the new directory */
    memset(block_buf, 0, IXFS_BLOCK_SIZE);
    de = (struct ixfs_dir_entry *)block_buf;
    de[0].d_inode = ino;
    strncpy(de[0].d_name, ".", IXFS_MAX_NAME - 1);
    de[1].d_inode = parent_ino;
    strncpy(de[1].d_name, "..", IXFS_MAX_NAME - 1);
    write_block(data_block, block_buf);

    /* Add entry to parent directory */
    read_block(parent_data_block, block_buf);

    /* Find the parent's current entry count from the inode */
    /* Read parent inode to get current size */
    {
        uint32_t inodes_per_block = IXFS_BLOCK_SIZE / sizeof(struct ixfs_inode);
        uint32_t p_block = sb.s_inode_start + (parent_ino / inodes_per_block);
        uint32_t p_offset = (parent_ino % inodes_per_block) * sizeof(struct ixfs_inode);
        uint8_t tmp[IXFS_BLOCK_SIZE];

        read_block(p_block, tmp);
        memcpy(&parent, tmp + p_offset, sizeof(parent));
    }

    {
        uint32_t entry_idx = parent.i_size / sizeof(struct ixfs_dir_entry);
        uint32_t byte_off = entry_idx * sizeof(struct ixfs_dir_entry);

        /* Re-read the parent data block */
        read_block(parent_data_block, block_buf);
        de = (struct ixfs_dir_entry *)(block_buf + byte_off);
        de->d_inode = ino;
        strncpy(de->d_name, name, IXFS_MAX_NAME - 1);
        write_block(parent_data_block, block_buf);

        /* Update parent inode size */
        parent.i_size += sizeof(struct ixfs_dir_entry);
        write_inode(parent_ino, &parent);
    }

    return ino;
}

/* --- Main --- */

int main(int argc, char *argv[])
{
    const char *image_path;
    const char *vol_name = "Impossible OS";
    long file_size;
    uint32_t total_blocks, bitmap_blocks, inode_blocks, used_blocks;
    uint32_t i;
    struct ixfs_inode root_inode;
    struct ixfs_dir_entry *de;
    uint32_t root_data_block;

    if (argc < 2) {
        fprintf(stderr, "Usage: %s <disk-image> [volume-name]\n", argv[0]);
        fprintf(stderr, "\nFormats a raw disk image as IXFS.\n");
        fprintf(stderr, "The image must already exist (use qemu-img or dd).\n");
        return 1;
    }

    image_path = argv[1];
    if (argc >= 3) vol_name = argv[2];

    /* Open disk image */
    disk = fopen(image_path, "r+b");
    if (!disk) {
        perror("Cannot open disk image");
        return 1;
    }

    /* Get file size */
    fseek(disk, 0, SEEK_END);
    file_size = ftell(disk);
    fseek(disk, 0, SEEK_SET);

    if (file_size < (long)(IXFS_BLOCK_SIZE * 16)) {
        fprintf(stderr, "Error: disk image too small (min 64 KiB)\n");
        fclose(disk);
        return 1;
    }

    /* Calculate layout */
    total_blocks = (uint32_t)(file_size / IXFS_BLOCK_SIZE);
    bitmap_blocks = (total_blocks + (IXFS_BLOCK_SIZE * 8) - 1)
                  / (IXFS_BLOCK_SIZE * 8);
    inode_blocks = (IXFS_DEFAULT_INODES * sizeof(struct ixfs_inode)
                   + IXFS_BLOCK_SIZE - 1) / IXFS_BLOCK_SIZE;

    /* Build superblock */
    memset(&sb, 0, sizeof(sb));
    sb.s_magic         = IXFS_MAGIC;
    sb.s_version       = IXFS_VERSION;
    sb.s_block_size    = IXFS_BLOCK_SIZE;
    sb.s_total_blocks  = total_blocks;
    sb.s_total_inodes  = IXFS_DEFAULT_INODES;
    sb.s_free_inodes   = IXFS_DEFAULT_INODES - 2;
    sb.s_bitmap_start  = 1;
    sb.s_bitmap_blocks = bitmap_blocks;
    sb.s_inode_start   = 1 + bitmap_blocks;
    sb.s_inode_blocks  = inode_blocks;
    sb.s_data_start    = 1 + bitmap_blocks + inode_blocks + IXFS_JOURNAL_BLOCKS
                       + IXFS_REFCOUNT_BLOCKS + IXFS_SNAPSHOT_BLOCKS;
    sb.s_root_inode    = IXFS_ROOT_INODE;
    sb.s_journal_start = 1 + bitmap_blocks + inode_blocks;
    sb.s_journal_blocks = IXFS_JOURNAL_BLOCKS;
    sb.s_journal_seq   = 0;
    sb.s_refcount_start = sb.s_journal_start + IXFS_JOURNAL_BLOCKS;
    sb.s_refcount_blocks = IXFS_REFCOUNT_BLOCKS;
    sb.s_snapshot_start = sb.s_refcount_start + IXFS_REFCOUNT_BLOCKS;
    sb.s_snapshot_count = 0;
    strncpy((char *)sb.s_volume_name, vol_name, 31);

    /* Count used blocks: sb + bitmap + inodes + journal + refcount + snapshot + root */
    used_blocks = 1 + bitmap_blocks + inode_blocks + IXFS_JOURNAL_BLOCKS
                + IXFS_REFCOUNT_BLOCKS + IXFS_SNAPSHOT_BLOCKS + 1;
    sb.s_free_blocks = total_blocks - used_blocks;

    /* Allocate and initialize bitmap */
    bitmap_bytes = (total_blocks + 7) / 8;
    bitmap = calloc(1, bitmap_bytes);
    if (!bitmap) { perror("malloc"); fclose(disk); return 1; }

    for (i = 0; i < used_blocks; i++)
        bitmap_set(i);

    /* Write superblock */
    memset(block_buf, 0, IXFS_BLOCK_SIZE);
    memcpy(block_buf, &sb, sizeof(sb));
    write_block(0, block_buf);

    /* Write bitmap */
    for (i = 0; i < bitmap_blocks; i++) {
        uint32_t offset = i * IXFS_BLOCK_SIZE;
        uint32_t remaining = bitmap_bytes - offset;
        if (remaining > IXFS_BLOCK_SIZE) remaining = IXFS_BLOCK_SIZE;
        memset(block_buf, 0, IXFS_BLOCK_SIZE);
        memcpy(block_buf, bitmap + offset, remaining);
        write_block(sb.s_bitmap_start + i, block_buf);
    }

    /* Zero inode table */
    memset(block_buf, 0, IXFS_BLOCK_SIZE);
    for (i = 0; i < inode_blocks; i++)
        write_block(sb.s_inode_start + i, block_buf);

    /* Create root directory inode (inode 1) */
    root_data_block = sb.s_data_start;
    memset(&root_inode, 0, sizeof(root_inode));
    root_inode.i_mode      = IXFS_S_DIR | IXFS_PERM_DIR;
    root_inode.i_links     = 1;
    root_inode.i_size      = 2 * sizeof(struct ixfs_dir_entry);
    root_inode.i_blocks    = 1;
    root_inode.i_extents[0].e_start = (uint64_t)root_data_block;
    root_inode.i_extents[0].e_count = 1;
    root_inode.i_extent_count = 1;
    write_inode(IXFS_ROOT_INODE, &root_inode);

    /* Write root directory data (. and ..) */
    memset(block_buf, 0, IXFS_BLOCK_SIZE);
    de = (struct ixfs_dir_entry *)block_buf;
    de[0].d_inode = IXFS_ROOT_INODE;
    strncpy(de[0].d_name, ".", IXFS_MAX_NAME - 1);
    de[1].d_inode = IXFS_ROOT_INODE;
    strncpy(de[1].d_name, "..", IXFS_MAX_NAME - 1);
    write_block(root_data_block, block_buf);

    printf("  Superblock:    block 0\n");
    printf("  Bitmap:        blocks %u-%u (%u blocks)\n",
           sb.s_bitmap_start, sb.s_bitmap_start + bitmap_blocks - 1,
           bitmap_blocks);
    printf("  Inode table:   blocks %u-%u (%u inodes)\n",
           sb.s_inode_start, sb.s_inode_start + inode_blocks - 1,
           IXFS_DEFAULT_INODES);
    printf("  Data start:    block %u\n", sb.s_data_start);
    printf("  Journal:       blocks %u-%u (%u blocks)\n",
           sb.s_journal_start, sb.s_journal_start + IXFS_JOURNAL_BLOCKS - 1,
           IXFS_JOURNAL_BLOCKS);

    /* Create default folder hierarchy */
    {
        uint32_t imp_ino, usr_ino;

        /* C:\Impossible */
        imp_ino = create_dir(IXFS_ROOT_INODE, root_data_block, "Impossible");
        if (imp_ino) {
            /* Read Impossible's data block to create subdirs */
            struct ixfs_inode imp_inode;
            uint32_t inodes_per_block = IXFS_BLOCK_SIZE / sizeof(struct ixfs_inode);
            uint32_t p_block = sb.s_inode_start + (imp_ino / inodes_per_block);
            uint32_t p_offset = (imp_ino % inodes_per_block) * sizeof(struct ixfs_inode);
            uint8_t tmp[IXFS_BLOCK_SIZE];
            read_block(p_block, tmp);
            memcpy(&imp_inode, tmp + p_offset, sizeof(imp_inode));

            create_dir(imp_ino, (uint32_t)imp_inode.i_extents[0].e_start, "System");
            create_dir(imp_ino, (uint32_t)imp_inode.i_extents[0].e_start, "Commands");
        }

        /* C:\Users */
        usr_ino = create_dir(IXFS_ROOT_INODE, root_data_block, "Users");
        if (usr_ino) {
            struct ixfs_inode usr_inode;
            uint32_t inodes_per_block = IXFS_BLOCK_SIZE / sizeof(struct ixfs_inode);
            uint32_t p_block = sb.s_inode_start + (usr_ino / inodes_per_block);
            uint32_t p_offset = (usr_ino % inodes_per_block) * sizeof(struct ixfs_inode);
            uint8_t tmp[IXFS_BLOCK_SIZE];
            read_block(p_block, tmp);
            memcpy(&usr_inode, tmp + p_offset, sizeof(usr_inode));

            create_dir(usr_ino, (uint32_t)usr_inode.i_extents[0].e_start, "Default");
        }

        /* C:\Programs */
        create_dir(IXFS_ROOT_INODE, root_data_block, "Programs");
    }

    /* Flush final bitmap and superblock */
    for (i = 0; i < bitmap_blocks; i++) {
        uint32_t offset = i * IXFS_BLOCK_SIZE;
        uint32_t remaining = bitmap_bytes - offset;
        if (remaining > IXFS_BLOCK_SIZE) remaining = IXFS_BLOCK_SIZE;
        memset(block_buf, 0, IXFS_BLOCK_SIZE);
        memcpy(block_buf, bitmap + offset, remaining);
        write_block(sb.s_bitmap_start + i, block_buf);
    }

    memset(block_buf, 0, IXFS_BLOCK_SIZE);
    memcpy(block_buf, &sb, sizeof(sb));
    write_block(0, block_buf);

    /* Initialize journal area */
    {
        struct ixfs_journal_header jh;
        memset(&jh, 0, sizeof(jh));
        jh.jh_magic = IXFS_JOURNAL_MAGIC;
        jh.jh_head = 1;
        jh.jh_tail = 1;
        jh.jh_seq = 0;
        memset(block_buf, 0, IXFS_BLOCK_SIZE);
        memcpy(block_buf, &jh, sizeof(jh));
        write_block(sb.s_journal_start, block_buf);
        memset(block_buf, 0, IXFS_BLOCK_SIZE);
        for (i = 1; i < IXFS_JOURNAL_BLOCKS; i++)
            write_block(sb.s_journal_start + i, block_buf);
    }

    printf("  Folders:       Impossible/{System,Commands}, Users/Default, Programs\n");
    printf("\n[OK] IXFS formatted: %u blocks (%u MiB), \"%s\"\n",
           total_blocks, total_blocks * IXFS_BLOCK_SIZE / (1024 * 1024),
           sb.s_volume_name);

    free(bitmap);
    fclose(disk);
    return 0;
}
