/* ============================================================================
 * ico.h -- ICO file loader for Impossible OS
 *
 * Parses Windows .ico files containing multiple sizes as embedded PNG or BMP
 * data. Returns an ico_file_t with up to ICO_MAX_ENTRIES decoded bitmaps.
 *
 * Usage:
 *   ico_file_t ico;
 *   if (ico_load(&ico, "C:\\path\\to\\app.ico") == 0) {
 *       icon_bitmap_t *best = ico_get_best(&ico, 32);
 *       icon_draw(surface, best, x, y);
 *   }
 *   ico_free(&ico);
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "icon_store.h"

#define ICO_MAX_ENTRIES  16  /* Max icon sizes in one .ico file */

/* One decoded size from an ICO file */
typedef struct {
    icon_bitmap_t bitmap;   /* Decoded BGRA pixels, width, height */
    uint8_t       valid;    /* 1 = decoded successfully */
} ico_entry_t;

/* Parsed ICO file with all decoded sizes */
typedef struct {
    ico_entry_t  entries[ICO_MAX_ENTRIES];
    int          count;     /* Number of entries decoded */
} ico_file_t;

/* Load and decode all sizes from an ICO file.
 * Returns 0 on success, -1 on failure. */
int ico_load(ico_file_t *ico, const char *path);

/* Find the best matching size in a loaded ICO file.
 * Returns pointer to the closest icon_bitmap_t, or NULL. */
icon_bitmap_t *ico_get_best(const ico_file_t *ico, uint32_t target_size);

/* Free all decoded bitmaps in an ICO file. */
void ico_free(ico_file_t *ico);
