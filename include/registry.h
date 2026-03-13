/* ============================================================================
 * registry.h — Windows-Compatible Registry System
 *
 * Replaces the Codex system with a Win32-compatible hierarchical registry.
 * Uses the same API naming as Win32 (RegOpenKeyEx, RegSetValueEx, etc.),
 * the same root keys (HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER, etc.), and
 * the same value types (REG_SZ, REG_DWORD, REG_BINARY, etc.).
 *
 * Storage: in-memory tree backed by static pools (512 keys, 1024 values).
 * Persistence: binary hive files with crash-safe journaling (future).
 *
 * Tree structure (examples):
 *   HKLM\System\Display\Width = 1280 (REG_DWORD)
 *   HKLM\System\Theme\DarkMode = 1 (REG_DWORD)
 *   HKCU\Shell\Prompt = "C:\>" (REG_SZ)
 *   HKLM\Hardware\CPU\Vendor = "GenuineIntel" (REG_SZ)
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* ---- Value types (match Win32 constants) ---- */

#define REG_NONE           0   /* No defined value type           */
#define REG_SZ             1   /* Null-terminated string          */
#define REG_EXPAND_SZ      2   /* String with environment vars    */
#define REG_BINARY         3   /* Raw binary data                 */
#define REG_DWORD          4   /* 32-bit unsigned integer         */
#define REG_DWORD_BIG_ENDIAN 5 /* 32-bit big-endian (rare)       */
#define REG_LINK           6   /* Symbolic link (reserved)        */
#define REG_MULTI_SZ       7   /* Array of null-terminated strings */
#define REG_QWORD         11   /* 64-bit unsigned integer         */

/* ---- Limits ---- */

#define REG_MAX_KEY_NAME     255   /* Max key name length (chars)          */
#define REG_MAX_VALUE_NAME   255   /* Max value name length (chars)        */
#define REG_MAX_VALUE_SIZE   512   /* Max value data size (bytes)          */
#define REG_MAX_PATH         512   /* Max full path length (chars)         */
#define REG_CHILD_BUCKETS     16   /* Hash buckets per key for child lookup */

/* ---- Error codes (match Win32 subset) ---- */

#define ERROR_SUCCESS           0
#define ERROR_FILE_NOT_FOUND    2
#define ERROR_ACCESS_DENIED     5
#define ERROR_INVALID_HANDLE    6
#define ERROR_OUTOFMEMORY      14
#define ERROR_INVALID_PARAMETER 87
#define ERROR_MORE_DATA        234
#define ERROR_NO_MORE_ITEMS    259
#define ERROR_KEY_DELETED     1018

/* ---- Access rights (match Win32 subset) ---- */

#define KEY_READ           0x20019
#define KEY_WRITE          0x20006
#define KEY_ALL_ACCESS     0xF003F
#define KEY_QUERY_VALUE    0x0001
#define KEY_SET_VALUE      0x0002
#define KEY_CREATE_SUB_KEY 0x0004
#define KEY_ENUMERATE_SUB_KEYS 0x0008

/* ---- Security / key flags ---- */

#define REG_FLAG_VOLATILE   0x01   /* Key not persisted to disk */
#define REG_FLAG_READONLY   0x02   /* Key is read-only          */
#define REG_FLAG_ALLOCATED  0x80   /* Pool slot is in use       */

/* ---- Registry value ---- */

typedef struct reg_value {
    char               name[REG_MAX_VALUE_NAME + 1];
    uint32_t           type;        /* REG_* type code               */
    uint8_t            data[REG_MAX_VALUE_SIZE];
    uint32_t           data_size;   /* Actual bytes used in data[]   */
    struct reg_value  *next;        /* Linked list within a key      */
} reg_value_t;

/* ---- Registry key ---- */

typedef struct reg_key {
    char               name[REG_MAX_KEY_NAME + 1];
    struct reg_key    *parent;       /* Parent key (NULL for roots)  */
    struct reg_key    *children[REG_CHILD_BUCKETS]; /* FNV-1a hash buckets */
    struct reg_key    *hash_next;    /* Collision chain within bucket */
    uint32_t           child_count;  /* Number of child keys         */
    reg_value_t       *values;       /* Linked list of values        */
    uint32_t           value_count;  /* Number of values             */
    uint64_t           last_write_time; /* PIT ticks at last modification */
    uint32_t           flags;        /* REG_FLAG_* bits              */
} reg_key_t;

/* ---- HKEY handle ---- */

typedef struct {
    reg_key_t *key;         /* Pointer to the open key   */
    uint32_t   access;      /* Access mode (KEY_* bits)  */
} reg_handle_t;

/* HKEY is an opaque pointer to a handle.  Predefined handles use
 * sentinel addresses that registry_init() maps to root keys. */
typedef reg_handle_t *HKEY;

/* ---- Predefined root key handles ---- */

/* Sentinel addresses — never dereferenced directly.  The API functions
 * detect these and map them to the corresponding root reg_key_t. */
#define HKEY_CLASSES_ROOT      ((HKEY)(uintptr_t)0x80000000)
#define HKEY_CURRENT_USER      ((HKEY)(uintptr_t)0x80000001)
#define HKEY_LOCAL_MACHINE     ((HKEY)(uintptr_t)0x80000002)
#define HKEY_USERS             ((HKEY)(uintptr_t)0x80000003)
#define HKEY_CURRENT_CONFIG    ((HKEY)(uintptr_t)0x80000005)

/* ---- Pool configuration ---- */

#define REG_KEY_POOL_SIZE     512
#define REG_VALUE_POOL_SIZE  1024

/* ---- Lifecycle API ---- */

/* Initialize the registry: zero pools, create predefined root keys.
 * Must be called once during kernel boot. */
void registry_init(void);

/* ---- Internal: pool statistics (for debug logging) ---- */

uint32_t reg_keys_used(void);
uint32_t reg_values_used(void);

