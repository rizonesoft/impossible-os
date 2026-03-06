/* ============================================================================
 * make-initrd.c — Pack files into an Impossible OS initrd archive.
 *
 * Archive format (IXRD):
 *   [initrd_header]              — 4-byte magic + 4-byte file count
 *   [initrd_file_entry] × count  — 64-byte name + 4-byte offset + 4-byte size
 *   [file data...]               — raw file contents
 *
 * Usage:
 *   make-initrd -o build/initrd.img file1.txt file2.txt ...
 *
 * Compiled with HOST gcc (not the cross-compiler):
 *   gcc -o build/tools/make-initrd scripts/make-initrd.c
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define MAGIC        0x44525849   /* "IXRD" */
#define MAX_NAME     64
#define MAX_FILES    64
#define HEADER_SIZE  8            /* magic(4) + count(4) */
#define ENTRY_SIZE   72           /* name(64) + offset(4) + size(4) */

struct file_info {
    char     name[MAX_NAME];
    uint8_t *data;
    uint32_t size;
};

/* Extract basename from a path */
static const char *basename_of(const char *path)
{
    const char *p = path;
    const char *last = path;

    while (*p) {
        if (*p == '/' || *p == '\\')
            last = p + 1;
        p++;
    }
    return last;
}

int main(int argc, char **argv)
{
    const char *output = NULL;
    struct file_info files[MAX_FILES];
    int file_count = 0;
    int i;
    FILE *out;
    uint32_t data_offset;
    uint32_t current_offset;

    /* Parse arguments */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output = argv[++i];
        } else {
            /* Input file */
            FILE *f;
            long fsize;
            const char *name;

            if (file_count >= MAX_FILES) {
                fprintf(stderr, "Error: too many files (max %d)\n", MAX_FILES);
                return 1;
            }

            f = fopen(argv[i], "rb");
            if (!f) {
                fprintf(stderr, "Error: cannot open '%s'\n", argv[i]);
                return 1;
            }

            fseek(f, 0, SEEK_END);
            fsize = ftell(f);
            fseek(f, 0, SEEK_SET);

            files[file_count].data = malloc(fsize);
            if (!files[file_count].data) {
                fprintf(stderr, "Error: out of memory\n");
                fclose(f);
                return 1;
            }

            if (fread(files[file_count].data, 1, fsize, f) != (size_t)fsize) {
                fprintf(stderr, "Error: failed to read '%s'\n", argv[i]);
                fclose(f);
                return 1;
            }
            fclose(f);

            files[file_count].size = (uint32_t)fsize;

            name = basename_of(argv[i]);
            strncpy(files[file_count].name, name, MAX_NAME - 1);
            files[file_count].name[MAX_NAME - 1] = '\0';

            file_count++;
        }
    }

    if (!output) {
        fprintf(stderr, "Usage: make-initrd -o <output> <file1> [file2] ...\n");
        return 1;
    }

    if (file_count == 0) {
        fprintf(stderr, "Error: no input files\n");
        return 1;
    }

    /* Calculate data offset */
    data_offset = HEADER_SIZE + ENTRY_SIZE * file_count;

    /* Write the archive */
    out = fopen(output, "wb");
    if (!out) {
        fprintf(stderr, "Error: cannot create '%s'\n", output);
        return 1;
    }

    /* Header: magic + file count */
    {
        uint32_t magic = MAGIC;
        uint32_t count = (uint32_t)file_count;
        fwrite(&magic, 4, 1, out);
        fwrite(&count, 4, 1, out);
    }

    /* File entries */
    current_offset = data_offset;
    for (i = 0; i < file_count; i++) {
        char name_padded[MAX_NAME];
        memset(name_padded, 0, MAX_NAME);
        strncpy(name_padded, files[i].name, MAX_NAME - 1);

        fwrite(name_padded, MAX_NAME, 1, out);
        fwrite(&current_offset, 4, 1, out);
        fwrite(&files[i].size, 4, 1, out);

        current_offset += files[i].size;
    }

    /* File data */
    for (i = 0; i < file_count; i++)
        fwrite(files[i].data, 1, files[i].size, out);

    fclose(out);

    printf("Created initrd: %s (%d files, %u bytes)\n",
           output, file_count, current_offset);

    /* Cleanup */
    for (i = 0; i < file_count; i++)
        free(files[i].data);

    return 0;
}
