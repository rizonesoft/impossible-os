/* ============================================================================
 * registry.h -- Windows-Compatible Registry System
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
#define ERROR_REGISTRY_IO_FAILED 1016  /* hive flush/save I/O failure */
#define ERROR_ALREADY_EXISTS   183   /* RegRenameKey: target name in use */
#define ERROR_PRIVILEGE_NOT_HELD 1314 /* RegSaveKey/RestoreKey: SeBackup/SeRestore not held */
#define REG_FORCE_RESTORE      0x00000008  /* RegRestoreKey: wipe existing subtree first */

/* ---- Access rights (match Win32 subset) ---- */

#define KEY_READ           0x20019
#define KEY_WRITE          0x20006
#define KEY_ALL_ACCESS     0xF003F
#define KEY_QUERY_VALUE    0x0001
#define KEY_SET_VALUE      0x0002
#define KEY_CREATE_SUB_KEY 0x0004
#define KEY_ENUMERATE_SUB_KEYS 0x0008
#define KEY_NOTIFY         0x0010
#define KEY_CREATE_LINK    0x0020
#define DELETE             0x00010000   /* DELETE standard right (RegDeleteKey) */
#define MAXIMUM_ALLOWED    0x02000000   /* samDesired: grant the maximum the DACL permits */
#define REG_MAX_KEY_DEPTH  512          /* Max key path depth (backslash-separated levels) */
/* GENERIC_* rights (mapped onto KEY_* by reg_effective_access); guarded so a
 * TU that also pulls acl.h / nt_types.h does not double-define. */
#ifndef GENERIC_READ
#define GENERIC_READ       0x80000000
#define GENERIC_WRITE      0x40000000
#define GENERIC_EXECUTE    0x20000000
#define GENERIC_ALL        0x10000000
#endif

/* ---- Security / key flags ---- */

#define REG_OPTION_NON_VOLATILE 0x00000000  /* RegCreateKeyEx dwOptions: persisted */
#define REG_OPTION_VOLATILE     0x00000001  /* RegCreateKeyEx dwOptions: RAM-only  */

#define REG_FLAG_VOLATILE       0x01   /* Key not persisted to disk           */
#define REG_FLAG_READONLY       0x02   /* Key is read-only                    */
#define REG_FLAG_HKCU_REDIRECT  0x08   /* HKCU: redirects to HKU\{user}       */
#define REG_FLAG_HKCR_MERGED    0x10   /* HKCR: merged HKLM+HKCU Classes view */
#define REG_FLAG_ALLOCATED      0x80   /* Pool slot is in use                 */

/* ---- Change-notification filters (Win32 RegNotifyChangeKeyValue) ---- */

#define REG_NOTIFY_CHANGE_NAME       0x01 /* sub-key create/delete       */
#define REG_NOTIFY_CHANGE_ATTRIBUTES 0x02 /* key metadata change         */
#define REG_NOTIFY_CHANGE_LAST_SET   0x04 /* value create/modify/delete  */
#define REG_NOTIFY_CHANGE_SECURITY   0x08 /* security descriptor change  */

/* ---- Registry value ---- */

typedef struct reg_value {
    char               name[REG_MAX_VALUE_NAME + 1];
    uint32_t           type;        /* REG_* type code               */
    uint8_t            data[REG_MAX_VALUE_SIZE];
    uint32_t           data_size;   /* Actual bytes used in data[]   */
    struct reg_value  *next;        /* Linked list within a key      */
} reg_value_t;

/* ---- Registry key ---- */

struct reg_watcher;   /* forward decl -- change-notification watcher (below) */

typedef struct reg_key {
    char               name[REG_MAX_KEY_NAME + 1];
    struct reg_key    *parent;       /* Parent key (NULL for roots)  */
    struct reg_key    *children[REG_CHILD_BUCKETS]; /* FNV-1a hash buckets */
    struct reg_key    *hash_next;    /* Collision chain within bucket */
    uint32_t           child_count;  /* Number of child keys         */
    reg_value_t       *values;       /* Linked list of values        */
    uint32_t           value_count;  /* Number of values             */
    /* monotonic uptime_ns() at last mutation; converted to FILETIME at query time */
    uint64_t           last_write_time;
    uint32_t           flags;        /* REG_FLAG_* bits              */
    /* Self-relative SD blob; NOT owned (points at the static SeCreateDefaultSD
     * default) -- never freed. */
    const void        *security_descriptor;
    /* Head of the singly-linked list of change-notification watchers on this
     * key (threaded via reg_watcher_t.next; slots live in reg_watcher_pool). */
    struct reg_watcher *watchers;
} reg_key_t;

/* ---- Change-notification watcher ----
 *
 * A callback registered against a key (and optionally its subtree) that fires
 * when a matching mutation (name/value/attributes) occurs.  The Win32-facing
 * event/semaphore-driven RegNotifyChangeKeyValue + NtNotifyChangeKey path (with
 * KEY_NOTIFY enforcement + hEvent object references) lands with the registry
 * syscall work; this is the kernel-internal callback engine that subsystems
 * (theme, display) use. */
typedef void (*reg_notify_fn)(const char *key_path, uint32_t change_type,
                              const char *value_name,  /* NULL if not applicable */
                              void *ctx);

typedef struct reg_watcher {
    uint64_t            watcher_id;   /* monotonic 64-bit id (never reused; 0 invalid) */
    reg_key_t          *key;          /* key this watcher is attached to      */
    uint32_t            filter;       /* REG_NOTIFY_CHANGE_* bitmask          */
    int                 watch_subtree;
    reg_notify_fn       callback;
    void               *ctx;
    int                 active;       /* 0 once unregistered/torn down        */
    uint64_t            hit_count;    /* telemetry: total fires               */
    uint64_t            coalesce_ms;  /* min ms between fires (0 = off)       */
    uint64_t            last_fired_ms;/* last fire timestamp (ms)             */
    struct reg_watcher *next;         /* next watcher on the same key         */
} reg_watcher_t;

/* ---- HKEY handle ---- */

typedef struct {
    reg_key_t *key;         /* Pointer to the open key   */
    uint32_t   access;      /* Access mode (KEY_* bits)  */
} reg_handle_t;

/* HKEY is an opaque pointer to a handle.  Predefined handles use
 * sentinel addresses that registry_init() maps to root keys. */
typedef reg_handle_t *HKEY;

/* ---- Predefined root key handles ---- */

/* Sentinel addresses -- never dereferenced directly.  The API functions
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

/* ---- Win32-Compatible Key Operations ---- */

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

/* Delete a key by its internal reg_key_t pointer (NT API support).
 * Used by NtDeleteKey which has a handle to the key itself, not parent+name.
 * Key must have no child keys; returns ERROR_ACCESS_DENIED if it does.
 * Frees values, unlinks from parent, marks slot freed. */
long RegDeleteKeyDirect(reg_key_t *key);

/* Rename a key by its internal reg_key_t pointer.
 * Validates new_name length and uniqueness within the parent.
 * Re-links the key in the parent's hash bucket under the new name.
 * Returns ERROR_ALREADY_EXISTS if the new name collides with a different
 * sibling, ERROR_INVALID_PARAMETER for an empty/over-long name or one
 * containing a '\\' separator, ERROR_ACCESS_DENIED if key is a root. */
long RegRenameKeyDirect(reg_key_t *key, const char *new_name);

/* ---- Advanced key operations (Win32) ---- */

/* Recursively copy all sub-keys + values of hKeySrc\lpSubKey into hKeyDest.
 * Requires KEY_READ on source, KEY_WRITE on destination. */
long RegCopyTree(HKEY hKeySrc, const char *lpSubKey, HKEY hKeyDest);

/* Rename a sub-key in place (unlink/relink under the new name in the parent's
 * hash bucket).  Returns ERROR_ALREADY_EXISTS if lpNewKeyName already exists. */
long RegRenameKey(HKEY hKey, const char *lpSubKeyName, const char *lpNewKeyName);

/* Serialize the sub-tree rooted at hKey to a standalone .hive file at lpFile.
 * Requires SeBackupPrivilege (fail-closed until TODO-15 SePrivilegeCheck lands). */
long RegSaveKey(HKEY hKey, const char *lpFile, void *lpSecurityAttributes);

/* Restore a .hive file into the sub-tree rooted at hKey.  REG_FORCE_RESTORE
 * (0x8) wipes the existing sub-tree first.  Requires SeRestorePrivilege
 * (fail-closed until TODO-15 SePrivilegeCheck lands). */
long RegRestoreKey(HKEY hKey, const char *lpFile, uint32_t dwFlags);

/* KCB (Key Control Block) LRU cache diagnostics: cumulative hit / miss counts
 * for the recently-accessed (parent, name) -> child resolution cache.  Used by
 * the unit test to assert the hot-key hit rate; either pointer may be NULL. */
void reg_kcb_get_stats(uint64_t *hits, uint64_t *misses);

/* ---- Change notifications (kernel-internal callback engine) ---- */

/* Register a change-notification callback on hKey (and its subtree if
 * watch_subtree).  filter is a REG_NOTIFY_CHANGE_* bitmask; coalesce_ms rate-
 * limits fires (0 = every change).  Returns a non-zero watcher id, or 0 on a
 * bad handle / exhausted 64-slot pool.  Kernel-internal + trusted: no
 * KEY_NOTIFY check (the Win32 hKey path enforces access). */
uint64_t reg_notify_register(HKEY hKey, uint32_t filter, int watch_subtree,
                             reg_notify_fn callback, void *ctx,
                             uint64_t coalesce_ms);

/* Deactivate + unlink a single watcher by id.  Returns ERROR_SUCCESS or
 * ERROR_FILE_NOT_FOUND if no live watcher has that id. */
long RegUnregisterNotify(uint64_t watcher_id);

/* Deactivate + unlink every watcher on `key` (called before a key is
 * tombstoned so a freed slot cannot stay linked). */
void reg_notify_unregister_all(reg_key_t *key);

/* Fire all matching watchers on `key` and its watch_subtree ancestors for a
 * change of `change_type` (a single REG_NOTIFY_CHANGE_* bit); value_name is the
 * affected value or NULL.  Hooked into the mutation paths. */
void reg_dispatch_notify(reg_key_t *key, uint32_t change_type,
                         const char *value_name);

/* True if `ancestor` is `key` or an ancestor of `key` (subtree membership
 * primitive; the dispatch walk uses the parent chain directly). */
int reg_is_descendant(reg_key_t *ancestor, reg_key_t *key);

/* Unload a hive subtree: recursively free all children, values, and the
 * key itself.  Used by NtUnloadKey.  Returns ERROR_ACCESS_DENIED for roots. */
long RegUnloadHive(reg_key_t *key);

/* Check if an HKEY is a predefined sentinel (HKLM, HKCU, etc.).
 * Returns 1 for predefined handles, 0 for user-allocated handles. */
int RegIsPredefinedKey(HKEY hKey);

/* Resolve a predefined HKEY sentinel to its internal reg_key_t pointer.
 * Returns NULL for non-predefined handles. */
reg_key_t *reg_resolve_predefined(HKEY hkey);

/* ---- RegGetValue flags (RRF_*) ---- */

#define RRF_RT_REG_SZ        0x00000002   /* Accept REG_SZ results         */
#define RRF_RT_REG_EXPAND_SZ 0x00000004   /* Accept REG_EXPAND_SZ          */
#define RRF_RT_REG_BINARY    0x00000008   /* Accept REG_BINARY             */
#define RRF_RT_REG_DWORD     0x00000010   /* Accept REG_DWORD              */
#define RRF_RT_REG_QWORD     0x00000040   /* Accept REG_QWORD              */
#define RRF_RT_ANY           0x0000FFFF   /* Accept any type               */
#define RRF_NOEXPAND         0x10000000   /* Don't expand REG_EXPAND_SZ    */

/* ---- Win32-Compatible Value Operations ---- */

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

/* Read an enumerated child key's full metadata under the parent's enumeration
 * right, without opening a capped handle to the child.  Used by
 * NtEnumerateKey(KeyFullInformation).  LastWriteTime is returned as FILETIME.
 * Requires KEY_ENUMERATE_SUB_KEYS on hKeyParent.  Any out pointer may be NULL. */
long reg_query_child_full_info(HKEY hKeyParent, const char *child_name,
                               uint32_t *sub_keys, uint32_t *values,
                               uint32_t *max_subkey_len, uint32_t *max_val_name,
                               uint32_t *max_val_data, uint64_t *last_write_ft);

/* Check that an open HKEY was granted at least `required_mask` (KEY_* bits).
 * Predefined root sentinels have implicit full access.  Returns ERROR_SUCCESS
 * if granted, ERROR_ACCESS_DENIED otherwise, ERROR_INVALID_HANDLE for a NULL
 * or freed handle.  This is the KEY_* access-rights enforcement chokepoint;
 * every public RegXxx entry point calls it before acting. */
long reg_check_access(HKEY hKey, uint32_t required_mask);

/* Convert a stored monotonic last_write_time (uptime_ns at mutation) to a
 * Windows FILETIME (100-ns since 1601) at read time.  Returns 0 for a
 * never-written key or before the wall clock is sourced.  Used by RegQueryInfoKey
 * / RegEnumKeyEx and the NtQueryKey / NtEnumerateKey info-class handlers so the
 * LastWriteTime they report is a real FILETIME, not raw ticks. */
uint64_t reg_last_write_filetime(uint64_t stored_uptime_ns);

/* Force an immediate synchronous flush of the single hive containing hKey
 * (walks the parent chain to the hive root, saves that hive only).  Volatile
 * keys have no hive backing: returns ERROR_SUCCESS (no-op).  Returns
 * ERROR_INVALID_HANDLE for a bad handle, or a save-failure code on I/O error. */
long RegFlushKey(HKEY hKey);

/* ---- Win32-Compatible Enumeration ---- */

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

/* ---- Typed Convenience Helpers ---- */

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

/* ---- Hive File Format ---- */

#define HIVE_MAGIC         0x48474552U   /* "REGH" in little-endian */
#define HIVE_VERSION       1
#define HIVE_HEADER_SIZE   4096
#define HIVE_ROOT_NAME_MAX 64

/* On-disk hive file header (4096 bytes, page-aligned) */
typedef struct __attribute__((packed)) {
    uint32_t magic;                          /* HIVE_MAGIC ("REGH") */
    uint32_t version;                        /* Format version (1) */
    uint32_t checksum;                       /* CRC32 of header (with this field zeroed) */
    uint32_t reserved0;
    uint64_t timestamp;                      /* PIT ticks at save time */
    char     root_name[HIVE_ROOT_NAME_MAX];  /* Root key name (e.g. "SYSTEM") */
    uint32_t total_keys;                     /* Number of keys in file */
    uint32_t total_values;                   /* Number of values in file */
    uint32_t data_offset;                    /* Offset to first key record */
    uint32_t data_size;                      /* Total size of key+value data */
    uint8_t  padding[HIVE_HEADER_SIZE - 96]; /* Pad to 4096 bytes */
} hive_header_t;

/* Serialize a root key tree to a hive file on disk.
 * Returns 0 on success, -1 on error. */
int hive_save(reg_key_t *root, const char *filepath);

/* Deserialize a hive file into an existing root key tree.
 * Returns number of values loaded, or -1 on error.
 * On corrupt file: logs warning, returns -1 (caller uses defaults). */
int hive_load(const char *filepath, reg_key_t *root);

/* ---- Hive Disk Layout ---- */

#define REG_HIVE_DIR   "C:\\Impossible\\System\\Config\\Registry"
#define REG_HIVE_COUNT 4    /* SYSTEM, SOFTWARE, HARDWARE, DEFAULT */

/* Mark the hive containing 'key' as dirty (pending flush).  Walks the
 * parent chain to find the hive root and sets its dirty flag. */
void registry_mark_dirty(reg_key_t *key);

/* Flush dirty hives to disk (called periodically from compositor loop).
 * Only writes hives whose dirty flag is set. */
void registry_flush(void);

/* Flush dirty hives with status reporting.  Returns 0 on success, or the
 * number of hives whose save failed (>0), or -1 if the registry is not
 * ready to flush (no mount, not initialized).  Used by NtFlushKey to
 * propagate I/O errors to user mode. */
int registry_flush_checked(void);

/* True once the on-disk hive layer has engaged this boot (ready + hive table
 * initialized by the first save/load) -- a LATCH that stays true even if C:
 * later unmounts, because recoverable hive copies (main + journal + .bak) may
 * already exist. Deliberately NOT a current-mount test (that would re-open the
 * absorb path for disk-originated secrets after an unmount). Side-effect free:
 * unlike registry_flush_checked() it does NOT trigger a hive save. Used by
 * one-shot-secret consumers that must NOT absorb a value whose bytes could
 * survive in an on-disk recovery source. */
int registry_persistence_active(void);

/* Save all hives to disk unconditionally (for clean shutdown). */
void registry_save_all(void);

/* Load all hive files from disk into the registry tree.
 * Called after VFS is mounted. Missing files are silently skipped. */
void registry_load_hives(void);
