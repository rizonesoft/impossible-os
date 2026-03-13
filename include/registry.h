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

#define REG_FLAG_VOLATILE       0x01   /* Key not persisted to disk           */
#define REG_FLAG_READONLY       0x02   /* Key is read-only                    */
#define REG_FLAG_HKCU_REDIRECT  0x08   /* HKCU: redirects to HKU\{user}       */
#define REG_FLAG_HKCR_MERGED    0x10   /* HKCR: merged HKLM+HKCU Classes view */
#define REG_FLAG_ALLOCATED      0x80   /* Pool slot is in use                 */

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

/* ---- Value type aliases ---- */

#define REG_DWORD_LITTLE_ENDIAN  REG_DWORD   /* Explicit alias (Win32) */

/* ---- Link key flag ---- */

#define REG_FLAG_LINK       0x04   /* Key is a symbolic link (REG_LINK) */

/* ---- Value type helpers ---- */

/* Return a human-readable name for a REG_* type code (e.g., "REG_SZ").
 * Returns "REG_UNKNOWN" for unrecognized types. */
const char *reg_type_name(uint32_t type);

/* Expand %VARIABLE% tokens in 'src' (REG_EXPAND_SZ semantics).
 * Looks up variables via the registry under HKLM\System\Environment.
 * Writes the expanded string to 'dst' (max 'dst_size' bytes).
 * Returns the number of bytes written (including null terminator),
 * or 0 on error / buffer too small. */
uint32_t reg_expand_sz(const char *src, char *dst, uint32_t dst_size);

/* ---- REG_MULTI_SZ helpers ----
 *
 * REG_MULTI_SZ encoding: each string is null-terminated, followed by
 * the next string. The entire sequence ends with a double null:
 *   "foo\0bar\0baz\0\0"
 */

/* Count the number of strings in a MULTI_SZ buffer.
 * 'data' must be double-null-terminated. */
uint32_t reg_multi_sz_count(const uint8_t *data, uint32_t data_size);

/* Get the Nth string (0-based) from a MULTI_SZ buffer.
 * Returns a pointer into 'data', or NULL if index out of range. */
const char *reg_multi_sz_get(const uint8_t *data, uint32_t data_size,
                              uint32_t index);

/* Pack an array of strings into MULTI_SZ format.
 * 'strings' is an array of 'count' null-terminated strings.
 * Writes packed data to 'out' (max 'out_size' bytes).
 * Returns total bytes written (including final double-null),
 * or 0 if buffer too small. */
uint32_t reg_multi_sz_pack(const char **strings, uint32_t count,
                            uint8_t *out, uint32_t out_size);

/* ---- REG_LINK helpers ---- */

/* Check if a key is a symbolic link (has REG_FLAG_LINK set). */
int reg_key_is_link(const reg_key_t *key);

/* Get the link target path from a link key.
 * The target is stored as the default (unnamed) value of type REG_LINK.
 * Returns pointer to the path string, or NULL if not a link. */
const char *reg_key_get_link_target(const reg_key_t *key);

/* ---- Child key management ---- */

/* Add a child key to a parent using FNV-1a hash buckets.
 * Sets child->parent. Returns 0 on success, -1 on error. */
int reg_add_child(reg_key_t *parent, reg_key_t *child);

/* Find a child key by name (case-insensitive).
 * Returns pointer to child, or NULL if not found. */
reg_key_t *reg_find_child(reg_key_t *parent, const char *name);

/* Allocate and add a new child key to parent. Convenience wrapper.
 * Returns pointer to new key, or NULL on failure. */
reg_key_t *reg_create_child(reg_key_t *parent, const char *name);

/* ---- HKCU / HKCR redirection ---- */

/* Set the current user name for HKCU → HKU\{user} redirection.
 * Must be called during user login. Max 255 chars. */
void reg_set_current_user(const char *username);

/* Resolve HKCU: returns the HKU\{current_user} key.
 * If no user is set, returns HKU\Default. */
reg_key_t *reg_resolve_hkcu(void);

/* Resolve HKCR: returns the HKLM\SOFTWARE\Classes key.
 * (Merged view with HKCU\SOFTWARE\Classes is transparent
 *  and will be handled by RegQueryValueEx in a future step.) */
reg_key_t *reg_resolve_hkcr(void);

/* Populate factory-default values in the registry tree.
 * Maps old Codex paths to Win32-style paths under HKLM/HKU. */
void registry_populate_defaults(void);

/* ---- Handle pool ---- */

#define REG_HANDLE_POOL_SIZE  128   /* Max simultaneously open handles */

/* ---- Disposition constants (RegCreateKeyEx) ---- */

#define REG_CREATED_NEW_KEY      0x00000001
#define REG_OPENED_EXISTING_KEY  0x00000002

/* ---- Win32-Compatible Key Operations (§2.1) ---- */

/* Open a sub-key relative to hKey.  Walks backslash-separated path.
 * Follows REG_LINK keys transparently.
 * Returns ERROR_SUCCESS on success, ERROR_FILE_NOT_FOUND if not found. */
long RegOpenKeyEx(HKEY hKey, const char *lpSubKey, uint32_t ulOptions,
                  uint32_t samDesired, HKEY *phkResult);

/* Create or open a sub-key.  Creates intermediate keys as needed.
 * *lpdwDisposition set to REG_CREATED_NEW_KEY or REG_OPENED_EXISTING_KEY.
 * lpClass, lpSecurityAttributes, and dwReserved are ignored (Win32 compat). */
long RegCreateKeyEx(HKEY hKey, const char *lpSubKey, uint32_t dwReserved,
                    const char *lpClass, uint32_t dwOptions,
                    uint32_t samDesired, void *lpSecurityAttributes,
                    HKEY *phkResult, uint32_t *lpdwDisposition);

/* Close an open key handle.  Predefined handles (HKLM etc.) are no-ops. */
long RegCloseKey(HKEY hKey);

/* Delete a sub-key and all its values (key must have no child keys).
 * Returns ERROR_ACCESS_DENIED if child keys exist (use RegDeleteTree). */
long RegDeleteKey(HKEY hKey, const char *lpSubKey);

/* Recursively delete a sub-key and all its children + values. */
long RegDeleteTree(HKEY hKey, const char *lpSubKey);

/* ---- RegGetValue flags (RRF_*) ---- */

#define RRF_RT_REG_SZ        0x00000002   /* Accept REG_SZ results         */
#define RRF_RT_REG_EXPAND_SZ 0x00000004   /* Accept REG_EXPAND_SZ          */
#define RRF_RT_REG_BINARY    0x00000008   /* Accept REG_BINARY             */
#define RRF_RT_REG_DWORD     0x00000010   /* Accept REG_DWORD              */
#define RRF_RT_REG_QWORD     0x00000040   /* Accept REG_QWORD              */
#define RRF_RT_ANY           0x0000FFFF   /* Accept any type               */
#define RRF_NOEXPAND         0x10000000   /* Don't expand REG_EXPAND_SZ    */

/* ---- Win32-Compatible Value Operations (§2.2) ---- */

/* Set or create a named value under hKey.
 * NULL/empty valueName = default "(Default)" value. */
long RegSetValueEx(HKEY hKey, const char *lpValueName, uint32_t Reserved,
                   uint32_t dwType, const uint8_t *lpData, uint32_t cbData);

/* Query a named value.  If buffer too small, sets *lpcbData to required
 * size and returns ERROR_MORE_DATA.  If lpData is NULL, just returns size. */
long RegQueryValueEx(HKEY hKey, const char *lpValueName, uint32_t *lpReserved,
                     uint32_t *lpType, uint8_t *lpData, uint32_t *lpcbData);

/* Convenience: open subKey + query value + optional REG_EXPAND_SZ expansion.
 * dwFlags: RRF_RT_* for type filtering, RRF_NOEXPAND to skip expansion. */
long RegGetValue(HKEY hKey, const char *lpSubKey, const char *lpValue,
                 uint32_t dwFlags, uint32_t *pdwType,
                 void *pvData, uint32_t *pcbData);

/* Delete a named value from hKey. */
long RegDeleteValue(HKEY hKey, const char *lpValueName);

/* ---- Win32-Compatible Enumeration (§2.3) ---- */

/* Enumerate child keys by 0-based index.
 * Returns key name, name size, and last write time.
 * Returns ERROR_NO_MORE_ITEMS when index >= child count. */
long RegEnumKeyEx(HKEY hKey, uint32_t dwIndex, char *lpName,
                  uint32_t *lpcchName, uint32_t *lpReserved,
                  char *lpClass, uint32_t *lpcchClass,
                  uint64_t *lpftLastWriteTime);

/* Enumerate values by 0-based index.
 * Returns value name, type, and data.
 * Returns ERROR_NO_MORE_ITEMS when index >= value count.
 * Returns ERROR_MORE_DATA if data buffer too small. */
long RegEnumValue(HKEY hKey, uint32_t dwIndex, char *lpValueName,
                  uint32_t *lpcchValueName, uint32_t *lpReserved,
                  uint32_t *lpType, uint8_t *lpData, uint32_t *lpcbData);

/* Query key metadata: sub-key count, value count, max name/data sizes,
 * and last write time.  Any output pointer may be NULL. */
long RegQueryInfoKey(HKEY hKey, char *lpClass, uint32_t *lpcchClass,
                     uint32_t *lpReserved, uint32_t *lpcSubKeys,
                     uint32_t *lpcbMaxSubKeyLen,
                     uint32_t *lpcbMaxClassLen,
                     uint32_t *lpcValues, uint32_t *lpcbMaxValueNameLen,
                     uint32_t *lpcbMaxValueLen, uint32_t *lpcbSecurityDescriptor,
                     uint64_t *lpftLastWriteTime);

/* ---- Typed Convenience Helpers (§2.4) ---- */

/* Read/write REG_DWORD (uint32_t) */
long RegGetDword(HKEY hKey, const char *lpValueName, uint32_t *pValue);
long RegSetDword(HKEY hKey, const char *lpValueName, uint32_t dwValue);

/* Read/write REG_SZ (null-terminated string) */
long RegGetString(HKEY hKey, const char *lpValueName,
                  char *lpBuf, uint32_t cbBuf);
long RegSetString(HKEY hKey, const char *lpValueName, const char *lpString);

/* Read/write REG_QWORD (uint64_t) */
long RegGetQword(HKEY hKey, const char *lpValueName, uint64_t *pValue);
long RegSetQword(HKEY hKey, const char *lpValueName, uint64_t qwValue);

/* One-shot: open key, read value, close key.
 * Combines RegOpenKeyEx + RegQueryValueEx + RegCloseKey. */
long RegReadKeyValue(HKEY hRootKey, const char *lpPath,
                     const char *lpValueName, uint32_t *lpType,
                     uint8_t *lpData, uint32_t *lpcbData);
