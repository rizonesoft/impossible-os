/* ============================================================================
 * codex.h — Codex Registry System
 *
 * The Codex is Impossible OS's equivalent of the Windows Registry.
 * It stores system configuration, user preferences, hardware info,
 * and application settings in a hierarchical key-value tree.
 *
 * Tree structure:
 *   System\Display\Width = 1280 (INT32)
 *   System\Theme\DarkMode = 1 (BOOL)
 *   User\Default\Shell\Prompt = "C:\>" (STRING)
 *   Hardware\CPU\Vendor = "GenuineIntel" (STRING)
 *
 * Root keys (created automatically):
 *   System\   — OS configuration
 *   Hardware\ — detected hardware
 *   User\     — per-user settings
 *   Apps\     — per-application settings
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* ---- Value types ---- */

typedef enum {
    CODEX_STRING = 0,   /* null-terminated string */
    CODEX_INT32  = 1,   /* 32-bit signed integer  */
    CODEX_INT64  = 2,   /* 64-bit signed integer  */
    CODEX_BINARY = 3,   /* raw byte array         */
    CODEX_BOOL   = 4    /* 0 or 1                 */
} codex_type_t;

/* ---- Limits ---- */

#define CODEX_MAX_NAME     64    /* max name length for keys and values     */
#define CODEX_MAX_STRING   256   /* max string value length                 */
#define CODEX_MAX_BINARY   512   /* max binary value size                   */
#define CODEX_MAX_PATH     512   /* max full path length                    */
#define CODEX_MAX_CHILDREN 64    /* max child keys per parent               */
#define CODEX_MAX_VALUES   32    /* max values per key                      */

/* ---- Value storage ---- */

typedef struct codex_value {
    char           name[CODEX_MAX_NAME];
    codex_type_t   type;
    uint32_t       data_size;       /* bytes used in data union */
    union {
        char       str[CODEX_MAX_STRING];
        int32_t    i32;
        int64_t    i64;
        uint8_t    bin[CODEX_MAX_BINARY];
        uint8_t    boolean;
    } data;
    struct codex_value *next;       /* linked list within a key */
} codex_value_t;

/* ---- Key node ---- */

typedef struct codex_key {
    char               name[CODEX_MAX_NAME];
    struct codex_key  *parent;
    struct codex_key  *children;    /* first child (linked list) */
    struct codex_key  *sibling;     /* next sibling in parent's children list */
    codex_value_t     *values;      /* linked list of values */
} codex_key_t;

/* ---- API: Lifecycle ---- */

/* Initialize the Codex with root keys (System, Hardware, User, Apps).
 * Must be called once during kernel boot. */
void codex_init(void);

/* ---- API: Key operations ---- */

/* Open a key by backslash-separated path (e.g., "System\Theme").
 * Returns a pointer to the key, or NULL if not found. */
codex_key_t *codex_open(const char *path);

/* Create a key (and any intermediate keys) by path.
 * Returns a pointer to the (possibly new) key, or NULL on failure. */
codex_key_t *codex_create(const char *path);

/* Delete a key and all its children + values.
 * Returns 0 on success, -1 on failure. */
int codex_delete_key(const char *path);

/* Populate default values (System, Hardware, User).
 * Call after codex_init() + framebuffer + PMM are ready. */
void codex_populate_defaults(void);

/* ---- API: Value accessors ---- */

int codex_get_string(codex_key_t *key, const char *name, char *buf, uint32_t buf_size);
int codex_get_int32(codex_key_t *key, const char *name, int32_t *out);
int codex_get_int64(codex_key_t *key, const char *name, int64_t *out);
int codex_get_bool(codex_key_t *key, const char *name, uint8_t *out);

int codex_set_string(codex_key_t *key, const char *name, const char *value);
int codex_set_int32(codex_key_t *key, const char *name, int32_t value);
int codex_set_int64(codex_key_t *key, const char *name, int64_t value);
int codex_set_bool(codex_key_t *key, const char *name, uint8_t value);

int codex_delete_value(codex_key_t *key, const char *name);

/* ---- API: Enumeration ---- */

/* Get the name of the child key at 'index' (0-based).
 * Returns 0 on success, -1 if index out of range. */
int codex_enum_keys(codex_key_t *key, uint32_t index, char *name, uint32_t size);

/* Get the value at 'index' (0-based).
 * Returns pointer to the value, or NULL if index out of range. */
codex_value_t *codex_enum_values(codex_key_t *key, uint32_t index);

/* ---- API: Disk persistence ---- */

/* Save all dirty root trees to .codex files in C:\Impossible\System\Config\Codex\.
 * Returns number of files saved. */
int codex_save(void);

/* Load .codex files from disk into the in-memory tree.
 * Call at boot before codex_populate_defaults(). Returns number of values loaded. */
int codex_load(void);

/* Flush dirty trees to disk if enough time has passed (2-second interval).
 * Call from compositor loop or timer tick. */
void codex_flush(void);

/* Mark a root tree as dirty (called automatically by set/delete operations). */
void codex_mark_dirty(const char *root_name);
