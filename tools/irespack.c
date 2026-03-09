/* ============================================================================
 * irespack.c — IRES (Icon Resource) packer for Impossible OS
 *
 * Host-side build tool. Reads PNG files from resources/icons/color/{size}/
 * directories and packs them into a single icons.ires binary.
 *
 * Usage: ./irespack <output.ires> <icon_dir>
 *   icon_dir must contain subdirectories named by size: 16/ 24/ 32/ etc.
 *   Each subdirectory contains PNGs named by icon: folder_closed.png, etc.
 *
 * Compile: gcc -O2 -o irespack tools/irespack.c -lm
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>
#include <sys/stat.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

/* ---- IRES format constants ---- */

#define IRES_MAGIC      0x53455249  /* "IRES" little-endian */
#define IRES_VERSION    1
#define MAX_ICONS       64
#define MAX_SIZES       16
#define MAX_NAME_LEN    64

/* ---- IRES file header (16 bytes) ---- */

typedef struct __attribute__((packed)) {
    uint32_t magic;         /* 0x53455249 = "IRES" */
    uint16_t version;       /* 1 */
    uint16_t icon_count;    /* Number of unique icons */
    uint8_t  size_count;    /* Number of pixel sizes */
    uint8_t  reserved[3];
    uint32_t file_size;     /* Total file size */
} ires_header_t;

/* ---- Per-icon, per-size entry in index ---- */

typedef struct __attribute__((packed)) {
    uint32_t data_offset;   /* Byte offset from file start (0 = not available) */
    uint16_t width;
    uint16_t height;
} ires_size_entry_t;

/* ---- Per-icon index entry ---- */

typedef struct __attribute__((packed)) {
    uint16_t icon_id;       /* Maps to system_icon_t */
    uint16_t name_offset;   /* Offset into name string table */
    /* Followed by size_count × ires_size_entry_t */
} ires_index_entry_t;

/* ---- Build-time data structures ---- */

typedef struct {
    char     name[MAX_NAME_LEN];
    uint16_t icon_id;
    struct {
        uint8_t *pixels;    /* BGRA pixel data */
        int      width;
        int      height;
        uint32_t byte_count;
    } sizes[MAX_SIZES];
} icon_data_t;

/* ---- Known icon name → ID mapping ---- */
/* Must match system_icon_t enum in icon_store.h */

typedef struct {
    const char *name;
    uint16_t    id;
} icon_id_map_t;

/* Enum values from icon_store.h — ICON_MONO_COUNT is 57 */
#define ICON_MONO_COUNT 57

static const icon_id_map_t known_icons[] = {
    { "folder_closed",     ICON_MONO_COUNT + 0 },
    { "folder_open",       ICON_MONO_COUNT + 1 },
    { "file_default",      ICON_MONO_COUNT + 2 },
    { "exe_default",       ICON_MONO_COUNT + 3 },
    { "computer",          ICON_MONO_COUNT + 4 },
    { "recycle_bin_empty",  ICON_MONO_COUNT + 5 },
    { "recycle_bin_full",   ICON_MONO_COUNT + 6 },
    { "control_deck",      ICON_MONO_COUNT + 7 },
    { NULL, 0 }
};

static uint16_t lookup_icon_id(const char *name)
{
    for (int i = 0; known_icons[i].name; i++) {
        if (strcmp(name, known_icons[i].name) == 0)
            return known_icons[i].id;
    }
    return 0xFFFF;  /* unknown */
}

/* ---- Supported sizes ---- */

static const uint16_t supported_sizes[] = {
    16, 24, 32, 48, 64, 72, 96, 128, 256
};
#define SIZE_COUNT (sizeof(supported_sizes) / sizeof(supported_sizes[0]))

static int find_size_index(int size)
{
    for (int i = 0; i < (int)SIZE_COUNT; i++) {
        if (supported_sizes[i] == size)
            return i;
    }
    return -1;
}

/* ---- Strip .png extension to get icon name ---- */

static void basename_no_ext(const char *filename, char *out, int maxlen)
{
    const char *dot = strrchr(filename, '.');
    int len = dot ? (int)(dot - filename) : (int)strlen(filename);
    if (len >= maxlen) len = maxlen - 1;
    memcpy(out, filename, len);
    out[len] = '\0';
}

/* ---- Main ---- */

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <output.ires> <icon_dir>\n", argv[0]);
        return 1;
    }

    const char *out_path = argv[1];
    const char *icon_dir = argv[2];

    icon_data_t icons[MAX_ICONS];
    int icon_count = 0;

    memset(icons, 0, sizeof(icons));

    /* ---- Scan each size directory ---- */

    for (int si = 0; si < (int)SIZE_COUNT; si++) {
        char dir_path[512];
        snprintf(dir_path, sizeof(dir_path), "%s/%d", icon_dir, supported_sizes[si]);

        DIR *d = opendir(dir_path);
        if (!d) continue;  /* Size dir doesn't exist — skip */

        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            /* Skip non-PNG files */
            const char *ext = strrchr(ent->d_name, '.');
            if (!ext || strcmp(ext, ".png") != 0) continue;

            /* Get icon name from filename */
            char name[MAX_NAME_LEN];
            basename_no_ext(ent->d_name, name, MAX_NAME_LEN);

            /* Look up icon ID */
            uint16_t id = lookup_icon_id(name);
            if (id == 0xFFFF) {
                fprintf(stderr, "  [SKIP] Unknown icon: %s/%s\n",
                        dir_path, ent->d_name);
                continue;
            }

            /* Find or create icon entry */
            int idx = -1;
            for (int i = 0; i < icon_count; i++) {
                if (strcmp(icons[i].name, name) == 0) {
                    idx = i;
                    break;
                }
            }
            if (idx < 0) {
                if (icon_count >= MAX_ICONS) {
                    fprintf(stderr, "ERROR: Too many icons (max %d)\n", MAX_ICONS);
                    closedir(d);
                    return 1;
                }
                idx = icon_count++;
                strncpy(icons[idx].name, name, MAX_NAME_LEN - 1);
                icons[idx].icon_id = id;
            }

            /* Load PNG via stb_image */
            char file_path[512];
            snprintf(file_path, sizeof(file_path), "%s/%s", dir_path, ent->d_name);

            int w, h, channels;
            uint8_t *rgba = stbi_load(file_path, &w, &h, &channels, 4);
            if (!rgba) {
                fprintf(stderr, "  [FAIL] Cannot load: %s\n", file_path);
                continue;
            }

            /* Convert RGBA → BGRA (swap R and B channels) */
            uint32_t pixel_count = (uint32_t)(w * h);
            for (uint32_t p = 0; p < pixel_count; p++) {
                uint8_t r = rgba[p * 4 + 0];
                uint8_t b = rgba[p * 4 + 2];
                rgba[p * 4 + 0] = b;
                rgba[p * 4 + 2] = r;
            }

            icons[idx].sizes[si].pixels     = rgba;
            icons[idx].sizes[si].width      = w;
            icons[idx].sizes[si].height     = h;
            icons[idx].sizes[si].byte_count = pixel_count * 4;

            printf("  [OK] %s @ %dpx (%dx%d, %u bytes)\n",
                   name, supported_sizes[si], w, h, pixel_count * 4);
        }
        closedir(d);
    }

    if (icon_count == 0) {
        fprintf(stderr, "ERROR: No icons found in %s\n", icon_dir);
        return 1;
    }

    /* ---- Build name string table ---- */

    char name_table[4096];
    uint32_t name_table_size = 0;
    uint16_t name_offsets[MAX_ICONS];

    for (int i = 0; i < icon_count; i++) {
        name_offsets[i] = (uint16_t)name_table_size;
        int len = (int)strlen(icons[i].name) + 1;  /* include null terminator */
        memcpy(name_table + name_table_size, icons[i].name, len);
        name_table_size += len;
    }

    /* ---- Calculate offsets ---- */

    uint32_t header_size     = sizeof(ires_header_t);
    uint32_t size_table_size = SIZE_COUNT * sizeof(uint16_t);
    uint32_t index_entry_sz  = sizeof(ires_index_entry_t) +
                               SIZE_COUNT * sizeof(ires_size_entry_t);
    uint32_t index_size      = (uint32_t)icon_count * index_entry_sz;

    uint32_t data_start = header_size + size_table_size + index_size + name_table_size;
    /* Align to 4 bytes */
    data_start = (data_start + 3) & ~3u;

    /* Calculate per-icon per-size data offsets */
    uint32_t current_offset = data_start;

    typedef struct {
        uint32_t offsets[MAX_SIZES];
    } icon_offsets_t;
    icon_offsets_t all_offsets[MAX_ICONS];

    for (int i = 0; i < icon_count; i++) {
        for (int si = 0; si < (int)SIZE_COUNT; si++) {
            if (icons[i].sizes[si].pixels) {
                all_offsets[i].offsets[si] = current_offset;
                current_offset += icons[i].sizes[si].byte_count;
            } else {
                all_offsets[i].offsets[si] = 0;
            }
        }
    }

    uint32_t total_file_size = current_offset;

    /* ---- Write output file ---- */

    FILE *fp = fopen(out_path, "wb");
    if (!fp) {
        fprintf(stderr, "ERROR: Cannot create %s\n", out_path);
        return 1;
    }

    /* Header */
    ires_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic      = IRES_MAGIC;
    hdr.version    = IRES_VERSION;
    hdr.icon_count = (uint16_t)icon_count;
    hdr.size_count = (uint8_t)SIZE_COUNT;
    hdr.file_size  = total_file_size;
    fwrite(&hdr, sizeof(hdr), 1, fp);

    /* Size table */
    fwrite(supported_sizes, sizeof(uint16_t), SIZE_COUNT, fp);

    /* Index entries */
    for (int i = 0; i < icon_count; i++) {
        ires_index_entry_t idx_entry;
        idx_entry.icon_id      = icons[i].icon_id;
        idx_entry.name_offset  = name_offsets[i];
        fwrite(&idx_entry, sizeof(idx_entry), 1, fp);

        for (int si = 0; si < (int)SIZE_COUNT; si++) {
            ires_size_entry_t se;
            se.data_offset = all_offsets[i].offsets[si];
            se.width       = (uint16_t)icons[i].sizes[si].width;
            se.height      = (uint16_t)icons[i].sizes[si].height;
            fwrite(&se, sizeof(se), 1, fp);
        }
    }

    /* Name string table */
    fwrite(name_table, 1, name_table_size, fp);

    /* Padding to align data start */
    uint32_t written = header_size + size_table_size + index_size + name_table_size;
    while (written < data_start) {
        fputc(0, fp);
        written++;
    }

    /* Pixel data */
    for (int i = 0; i < icon_count; i++) {
        for (int si = 0; si < (int)SIZE_COUNT; si++) {
            if (icons[i].sizes[si].pixels) {
                fwrite(icons[i].sizes[si].pixels, 1,
                       icons[i].sizes[si].byte_count, fp);
            }
        }
    }

    fclose(fp);

    /* ---- Summary ---- */

    printf("\n[OK] IRES packed: %s (%d icons, %d sizes, %u bytes)\n",
           out_path, icon_count, (int)SIZE_COUNT, total_file_size);

    /* Clean up */
    for (int i = 0; i < icon_count; i++) {
        for (int si = 0; si < (int)SIZE_COUNT; si++) {
            if (icons[i].sizes[si].pixels)
                stbi_image_free(icons[i].sizes[si].pixels);
        }
    }

    return 0;
}
