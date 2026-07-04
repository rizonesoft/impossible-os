/* ============================================================================
 * nt_registry.c -- NT registry syscall SSDT handlers (core CRUD + advanced)
 *
 * Wires 20 NtXxx registry operations into the SSDT, translating NT
 * object-namespace paths (\Registry\Machine\...) into Win32 RegXxx calls
 * against the existing registry engine (registry.c).
 *
 * TODO-05 sections 14 (core CRUD) and 15 (advanced: flush/notify/save/
 * restore/hive load).
 * ============================================================================ */

#include "kernel/nt/nt_registry.h"
#include "kernel/nt/ssdt.h"
#include "kernel/nt/ntstatus.h"
#include "kernel/nt/nt_types.h"
#include "kernel/nt/service_numbers.h"
#include "kernel/klog.h"
#include "kernel/sched/task.h"
#include "kernel/ob/ob.h"
#include "kernel/ob/ob_file.h"
#include "kernel/fs/vfs.h"
#include "registry.h"

/* ---- String helpers (freestanding) -------------------------------------- */

static uint32_t nt_reg_strlen(const char *s)
{
    uint32_t len = 0;
    if (!s) return 0;
    while (s[len]) len++;
    return len;
}

static void nt_reg_memcpy(void *dst, const void *src, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    uint32_t i;
    for (i = 0; i < n; i++)
        d[i] = s[i];
}

static void nt_reg_memset(void *dst, uint8_t val, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    uint32_t i;
    for (i = 0; i < n; i++)
        d[i] = val;
}

/* Case-insensitive prefix match.  Returns 1 if 's' starts with 'prefix'. */
static int nt_reg_prefix(const char *s, const char *prefix)
{
    while (*prefix) {
        char a = *s, b = *prefix;
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
        s++;
        prefix++;
    }
    return 1;
}

/* ---- NT path -> HKEY + subpath translation ------------------------------ */

/* NT registry paths use the object namespace format:
 *   \Registry\Machine\SOFTWARE\...  -> HKEY_LOCAL_MACHINE, "SOFTWARE\..."
 *   \Registry\User\...              -> HKEY_USERS, "..."
 *   \Registry\Machine\...           -> HKEY_LOCAL_MACHINE, "..."
 *
 * Returns the root HKEY and sets *subpath to the remainder after the root
 * prefix.  Returns NULL on unrecognized paths. */
static HKEY nt_reg_resolve_path(const char *nt_path, const char **subpath)
{
    const char *p = nt_path;

    if (!p || *p != '\\')
        return (HKEY)0;

    /* Skip leading backslash */
    p++;

    /* Must start with "Registry\" */
    if (!nt_reg_prefix(p, "Registry\\"))
        return (HKEY)0;
    p += 9;  /* skip "Registry\" */

    /* Determine root key */
    if (nt_reg_prefix(p, "Machine\\")) {
        *subpath = p + 8;
        return HKEY_LOCAL_MACHINE;
    }
    if (nt_reg_prefix(p, "Machine") && p[7] == '\0') {
        /* Exact match with no trailing path. The trailing-NUL guard stops
         * `MachineXYZ` from prefix-aliasing to the HKLM root (a wrong-object
         * open/write/delete): the with-subpath form is handled above. */
        *subpath = "";
        return HKEY_LOCAL_MACHINE;
    }
    if (nt_reg_prefix(p, "User\\")) {
        *subpath = p + 5;
        return HKEY_USERS;
    }
    if (nt_reg_prefix(p, "User") && p[4] == '\0') {
        *subpath = "";
        return HKEY_USERS;
    }
    if (nt_reg_prefix(p, "CurrentConfig\\")) {
        *subpath = p + 14;
        return HKEY_CURRENT_CONFIG;
    }
    if (nt_reg_prefix(p, "CurrentConfig") && p[13] == '\0') {
        *subpath = "";
        return HKEY_CURRENT_CONFIG;
    }

    return (HKEY)0;
}

/* ---- Helper: extract path from OBJECT_ATTRIBUTES ----------------------- */

static const char *reg_oa_path(OBJECT_ATTRIBUTES *oa)
{
    if (!oa || !oa->ObjectName || !oa->ObjectName->Buffer)
        return (const char *)0;
    return (const char *)oa->ObjectName->Buffer;
}

/* ---- Helper: tombstone-aware HKEY resolver (shared by and) ----
 * Forward-declared here; implementation in the block at file-end. */
static reg_key_t *resolve_hkey(HKEY hkey);

/* ---- Helper: convert Win32 error to NTSTATUS ---------------------------- */

static NTSTATUS reg_win32_to_nt(long err)
{
    switch (err) {
    case ERROR_SUCCESS:          return STATUS_SUCCESS;
    case ERROR_FILE_NOT_FOUND:   return STATUS_OBJECT_NAME_NOT_FOUND;
    case ERROR_ACCESS_DENIED:    return STATUS_ACCESS_DENIED;
    case ERROR_INVALID_HANDLE:   return STATUS_INVALID_HANDLE;
    case ERROR_OUTOFMEMORY:      return STATUS_NO_MEMORY;
    case ERROR_INVALID_PARAMETER:return STATUS_INVALID_PARAMETER;
    case ERROR_MORE_DATA:        return STATUS_BUFFER_TOO_SMALL;
    case ERROR_NO_MORE_ITEMS:    return STATUS_NO_MORE_ENTRIES;
    case ERROR_KEY_DELETED:      return STATUS_KEY_DELETED;
    case ERROR_REGISTRY_IO_FAILED: return STATUS_REGISTRY_IO_FAILED;
    case ERROR_ALREADY_EXISTS:   return STATUS_OBJECT_NAME_COLLISION;
    case ERROR_PRIVILEGE_NOT_HELD: return STATUS_PRIVILEGE_NOT_HELD;
    default:                     return STATUS_UNSUCCESSFUL;
    }
}

/* ======================================================================== */
/* NtCreateKey (SSDT 0x0090)                                               */
/*                                                                          */
/* a1 = HANDLE* KeyHandle (out)                                            */
/* a2 = ACCESS_MASK DesiredAccess                                          */
/* a3 = OBJECT_ATTRIBUTES* ObjectAttributes                                */
/* a4 = uint32_t TitleIndex                                                */
/* a5 = UNICODE_STRING* Class (ignored)                                    */
/* a6 = uint32_t CreateOptions                                             */
/*                                                                          */
/* Note: The real NT NtCreateKey has 7 parameters (Disposition is the 7th). */
/* SSDT_HANDLER only carries 6 uint64_t arguments. Disposition is not       */
/* supported through the SSDT dispatch path; callers that need it should    */
/* query key existence before/after create. The Disposition output is       */
/* available via the Zw wrapper (kernel-mode, direct call).                 */
/* ======================================================================== */

static NTSTATUS NtCreateKey_handler(uint64_t a1, uint64_t a2, uint64_t a3,
                                    uint64_t a4, uint64_t a5, uint64_t a6)
{
    HANDLE *out = (HANDLE *)a1;
    OBJECT_ATTRIBUTES *oa = (OBJECT_ATTRIBUTES *)a3;
    uint32_t create_options = (uint32_t)a6;
    uint32_t desired_access = (uint32_t)a2;
    const char *nt_path;
    const char *subpath;
    HKEY root, result_key;
    uint32_t disp = 0;
    long rc;

    (void)a4; (void)a5;

    if (!out)
        return STATUS_INVALID_PARAMETER;

    nt_path = reg_oa_path(oa);
    if (!nt_path)
        return STATUS_INVALID_PARAMETER;

    root = nt_reg_resolve_path(nt_path, &subpath);
    if (!root)
        return STATUS_OBJECT_NAME_NOT_FOUND;

    /* Grant the caller-requested DesiredAccess on the returned handle (not a
     * blanket KEY_ALL_ACCESS); per-operation checks enforce it thereafter. */
    rc = RegCreateKeyEx(root, subpath, 0, (const char *)0,
                        create_options, desired_access, (void *)0,
                        &result_key, &disp);
    if (rc != ERROR_SUCCESS)
        return reg_win32_to_nt(rc);

    /* Return a pseudo-handle: cast HKEY to HANDLE.
     * The caller uses this handle in subsequent NtXxx registry calls. */
    *out = (HANDLE)(uintptr_t)result_key;

    return STATUS_SUCCESS;
}

/* ======================================================================== */
/* NtOpenKey (SSDT 0x0092)                                                 */
/*                                                                          */
/* a1 = HANDLE* KeyHandle (out)                                            */
/* a2 = ACCESS_MASK DesiredAccess                                          */
/* a3 = OBJECT_ATTRIBUTES* ObjectAttributes                                */
/* ======================================================================== */

static NTSTATUS NtOpenKey_handler(uint64_t a1, uint64_t a2, uint64_t a3,
                                  uint64_t a4, uint64_t a5, uint64_t a6)
{
    HANDLE *out = (HANDLE *)a1;
    OBJECT_ATTRIBUTES *oa = (OBJECT_ATTRIBUTES *)a3;
    const char *nt_path;
    const char *subpath;
    HKEY root, result_key;
    uint32_t desired_access = (uint32_t)a2;
    long rc;

    (void)a4; (void)a5; (void)a6;

    if (!out)
        return STATUS_INVALID_PARAMETER;

    nt_path = reg_oa_path(oa);
    if (!nt_path)
        return STATUS_INVALID_PARAMETER;

    root = nt_reg_resolve_path(nt_path, &subpath);
    if (!root)
        return STATUS_OBJECT_NAME_NOT_FOUND;

    /* Grant the caller-requested DesiredAccess on the returned handle. */
    rc = RegOpenKeyEx(root, subpath, 0, desired_access, &result_key);
    if (rc != ERROR_SUCCESS)
        return reg_win32_to_nt(rc);

    *out = (HANDLE)(uintptr_t)result_key;
    return STATUS_SUCCESS;
}

/* ======================================================================== */
/* NtOpenKeyEx (SSDT 0x0094)                                               */
/*                                                                          */
/* a1 = HANDLE* KeyHandle (out)                                            */
/* a2 = ACCESS_MASK DesiredAccess                                          */
/* a3 = OBJECT_ATTRIBUTES* ObjectAttributes                                */
/* a4 = uint32_t OpenOptions                                               */
/* ======================================================================== */

static NTSTATUS NtOpenKeyEx_handler(uint64_t a1, uint64_t a2, uint64_t a3,
                                    uint64_t a4, uint64_t a5, uint64_t a6)
{
    /* OpenOptions adds REG_OPTION_OPEN_LINK support; for now, delegate
     * to the same NtOpenKey path (link following is handled by reg_walk_path). */
    (void)a4;
    return NtOpenKey_handler(a1, a2, a3, 0, a5, a6);
}

/* ======================================================================== */
/* NtDeleteKey (SSDT 0x0095)                                               */
/*                                                                          */
/* a1 = HANDLE KeyHandle                                                   */
/* ======================================================================== */

static NTSTATUS NtDeleteKey_handler(uint64_t a1, uint64_t a2, uint64_t a3,
                                    uint64_t a4, uint64_t a5, uint64_t a6)
{
    HKEY hkey = (HKEY)(uintptr_t)a1;
    reg_key_t *key;
    long rc;

    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;

    if (!hkey)
        return STATUS_INVALID_HANDLE;

    /* Predefined handles (HKLM, HKCU, etc.) cannot be deleted */
    if (RegIsPredefinedKey(hkey))
        return STATUS_ACCESS_DENIED;

    /* NtDeleteKey takes a handle to the key itself (not parent + subkey).
     * Route through resolve_hkey for tombstone-aware liveness check. */
    key = resolve_hkey(hkey);
    if (!key)
        return STATUS_INVALID_HANDLE;

    /* The direct-pointer NT path bypasses the Win32 RegDeleteKey chokepoint;
     * enforce DELETE access on the handle so a read-only handle cannot delete. */
    if (reg_check_access(hkey, DELETE) != ERROR_SUCCESS)
        return STATUS_ACCESS_DENIED;

    /* Return the specific NT status for children before the generic delete */
    if (key->child_count > 0)
        return STATUS_KEY_HAS_CHILDREN;

    rc = RegDeleteKeyDirect(key);
    if (rc != ERROR_SUCCESS)
        return reg_win32_to_nt(rc);

    /* Close the handle */
    RegCloseKey(hkey);
    return STATUS_SUCCESS;
}

/* ======================================================================== */
/* NtSetValueKey (SSDT 0x0096)                                             */
/*                                                                          */
/* a1 = HANDLE KeyHandle                                                   */
/* a2 = UNICODE_STRING* ValueName                                          */
/* a3 = uint32_t TitleIndex                                                */
/* a4 = uint32_t Type                                                      */
/* a5 = void* Data                                                         */
/* a6 = uint32_t DataSize                                                  */
/* ======================================================================== */

static NTSTATUS NtSetValueKey_handler(uint64_t a1, uint64_t a2, uint64_t a3,
                                      uint64_t a4, uint64_t a5, uint64_t a6)
{
    HKEY hkey = (HKEY)(uintptr_t)a1;
    UNICODE_STRING *vname = (UNICODE_STRING *)a2;
    uint32_t type = (uint32_t)a4;
    const uint8_t *data = (const uint8_t *)a5;
    uint32_t data_size = (uint32_t)a6;
    const char *name_str;
    long rc;

    (void)a3; /* TitleIndex */

    if (!hkey)
        return STATUS_INVALID_HANDLE;

    name_str = (vname && vname->Buffer) ? (const char *)vname->Buffer : "";

    rc = RegSetValueEx(hkey, name_str, 0, type, data, data_size);
    return reg_win32_to_nt(rc);
}

/* ======================================================================== */
/* NtQueryValueKey (SSDT 0x0097)                                           */
/*                                                                          */
/* a1 = HANDLE KeyHandle                                                   */
/* a2 = UNICODE_STRING* ValueName                                          */
/* a3 = KEY_VALUE_INFORMATION_CLASS                                        */
/* a4 = void* KeyValueInformation (out buffer)                             */
/* a5 = uint32_t Length (buffer size)                                      */
/* a6 = uint32_t* ResultLength (out)                                       */
/* ======================================================================== */

static NTSTATUS NtQueryValueKey_handler(uint64_t a1, uint64_t a2, uint64_t a3,
                                        uint64_t a4, uint64_t a5, uint64_t a6)
{
    HKEY hkey = (HKEY)(uintptr_t)a1;
    UNICODE_STRING *vname = (UNICODE_STRING *)a2;
    KEY_VALUE_INFORMATION_CLASS info_class = (KEY_VALUE_INFORMATION_CLASS)a3;
    void *buffer = (void *)a4;
    uint32_t buf_len = (uint32_t)a5;
    uint32_t *result_len = (uint32_t *)a6;
    const char *name_str;
    uint32_t type = 0;
    uint8_t data[REG_MAX_VALUE_SIZE];
    uint32_t data_size = REG_MAX_VALUE_SIZE;
    long rc;
    uint32_t name_len;

    if (!hkey)
        return STATUS_INVALID_HANDLE;

    name_str = (vname && vname->Buffer) ? (const char *)vname->Buffer : "";

    /* Query the value via Win32 API */
    rc = RegQueryValueEx(hkey, name_str, (uint32_t *)0, &type, data, &data_size);
    if (rc != ERROR_SUCCESS)
        return reg_win32_to_nt(rc);

    name_len = nt_reg_strlen(name_str);

    switch (info_class) {
    case KeyValueBasicInformation: {
        uint32_t needed = __builtin_offsetof(KEY_VALUE_BASIC_INFORMATION, Name) + name_len;
        if (result_len) *result_len = needed;
        if (buf_len < needed)
            return STATUS_BUFFER_TOO_SMALL;
        if (buffer) {
            KEY_VALUE_BASIC_INFORMATION *info = (KEY_VALUE_BASIC_INFORMATION *)buffer;
            nt_reg_memset(buffer, 0, needed);
            info->TitleIndex = 0;
            info->Type = type;
            info->NameLength = name_len;
            if (name_len > 0)
                nt_reg_memcpy(info->Name, name_str, name_len);
        }
        return STATUS_SUCCESS;
    }
    case KeyValueFullInformation: {
        uint32_t needed = __builtin_offsetof(KEY_VALUE_FULL_INFORMATION, Name) + name_len + data_size;
        if (result_len) *result_len = needed;
        if (buf_len < needed)
            return STATUS_BUFFER_TOO_SMALL;
        if (buffer) {
            KEY_VALUE_FULL_INFORMATION *info = (KEY_VALUE_FULL_INFORMATION *)buffer;
            nt_reg_memset(buffer, 0, needed);
            info->TitleIndex = 0;
            info->Type = type;
            info->NameLength = name_len;
            info->DataOffset = (uint32_t)(__builtin_offsetof(KEY_VALUE_FULL_INFORMATION, Name) + name_len);
            info->DataLength = data_size;
            if (name_len > 0)
                nt_reg_memcpy(info->Name, name_str, name_len);
            if (data_size > 0)
                nt_reg_memcpy((uint8_t *)buffer + info->DataOffset, data, data_size);
        }
        return STATUS_SUCCESS;
    }
    case KeyValuePartialInformation: {
        uint32_t needed = __builtin_offsetof(KEY_VALUE_PARTIAL_INFORMATION, Data) + data_size;
        if (result_len) *result_len = needed;
        if (buf_len < needed)
            return STATUS_BUFFER_TOO_SMALL;
        if (buffer) {
            KEY_VALUE_PARTIAL_INFORMATION *info = (KEY_VALUE_PARTIAL_INFORMATION *)buffer;
            nt_reg_memset(buffer, 0, needed);
            info->TitleIndex = 0;
            info->Type = type;
            info->DataLength = data_size;
            if (data_size > 0)
                nt_reg_memcpy(info->Data, data, data_size);
        }
        return STATUS_SUCCESS;
    }
    default:
        return STATUS_INVALID_PARAMETER;
    }
}

/* ======================================================================== */
/* NtDeleteValueKey (SSDT 0x0098)                                          */
/*                                                                          */
/* a1 = HANDLE KeyHandle                                                   */
/* a2 = UNICODE_STRING* ValueName                                          */
/* ======================================================================== */

static NTSTATUS NtDeleteValueKey_handler(uint64_t a1, uint64_t a2, uint64_t a3,
                                         uint64_t a4, uint64_t a5, uint64_t a6)
{
    HKEY hkey = (HKEY)(uintptr_t)a1;
    UNICODE_STRING *vname = (UNICODE_STRING *)a2;
    const char *name_str;
    long rc;

    (void)a3; (void)a4; (void)a5; (void)a6;

    if (!hkey)
        return STATUS_INVALID_HANDLE;

    name_str = (vname && vname->Buffer) ? (const char *)vname->Buffer : "";

    rc = RegDeleteValue(hkey, name_str);
    return reg_win32_to_nt(rc);
}

/* ======================================================================== */
/* NtEnumerateKey (SSDT 0x0099)                                            */
/*                                                                          */
/* a1 = HANDLE KeyHandle                                                   */
/* a2 = uint32_t Index                                                     */
/* a3 = KEY_INFORMATION_CLASS                                              */
/* a4 = void* KeyInformation (out buffer)                                  */
/* a5 = uint32_t Length (buffer size)                                      */
/* a6 = uint32_t* ResultLength (out)                                       */
/* ======================================================================== */

static NTSTATUS NtEnumerateKey_handler(uint64_t a1, uint64_t a2, uint64_t a3,
                                       uint64_t a4, uint64_t a5, uint64_t a6)
{
    HKEY hkey = (HKEY)(uintptr_t)a1;
    uint32_t index = (uint32_t)a2;
    KEY_INFORMATION_CLASS info_class = (KEY_INFORMATION_CLASS)a3;
    void *buffer = (void *)a4;
    uint32_t buf_len = (uint32_t)a5;
    uint32_t *result_len = (uint32_t *)a6;
    char name[REG_MAX_KEY_NAME + 1];
    uint32_t name_size = REG_MAX_KEY_NAME + 1;
    uint64_t last_write = 0;
    long rc;
    uint32_t name_len;

    if (!hkey)
        return STATUS_INVALID_HANDLE;

    rc = RegEnumKeyEx(hkey, index, name, &name_size,
                      (uint32_t *)0, (char *)0, (uint32_t *)0, &last_write);
    if (rc == ERROR_NO_MORE_ITEMS)
        return STATUS_NO_MORE_ENTRIES;
    if (rc != ERROR_SUCCESS)
        return reg_win32_to_nt(rc);

    name_len = nt_reg_strlen(name);

    switch (info_class) {
    case KeyBasicInformation: {
        uint32_t needed = __builtin_offsetof(KEY_BASIC_INFORMATION, Name) + name_len;
        if (result_len) *result_len = needed;
        if (buf_len < needed)
            return STATUS_BUFFER_TOO_SMALL;
        if (buffer) {
            KEY_BASIC_INFORMATION *info = (KEY_BASIC_INFORMATION *)buffer;
            nt_reg_memset(buffer, 0, needed);
            info->LastWriteTime = last_write;
            info->TitleIndex = 0;
            info->NameLength = name_len;
            if (name_len > 0)
                nt_reg_memcpy(info->Name, name, name_len);
        }
        return STATUS_SUCCESS;
    }
    case KeyNameInformation: {
        uint32_t needed = __builtin_offsetof(KEY_NAME_INFORMATION, Name) + name_len;
        if (result_len) *result_len = needed;
        if (buf_len < needed)
            return STATUS_BUFFER_TOO_SMALL;
        if (buffer) {
            KEY_NAME_INFORMATION *info = (KEY_NAME_INFORMATION *)buffer;
            nt_reg_memset(buffer, 0, needed);
            info->NameLength = name_len;
            if (name_len > 0)
                nt_reg_memcpy(info->Name, name, name_len);
        }
        return STATUS_SUCCESS;
    }
    case KeyFullInformation: {
        /* Read the child's metadata directly under the parent's enumeration
         * right -- an access-capped re-open of the child would spuriously fail
         * for an enumeration-only handle and would convert LastWriteTime twice. */
        {
            uint32_t sub_keys = 0, child_values = 0;
            uint32_t max_subkey_len = 0, max_value_name_len = 0, max_value_data_len = 0;
            uint64_t child_last_write = 0;
            uint32_t needed = sizeof(KEY_FULL_INFORMATION);
            long q_rc = reg_query_child_full_info(hkey, name,
                            &sub_keys, &child_values, &max_subkey_len,
                            &max_value_name_len, &max_value_data_len,
                            &child_last_write);
            if (q_rc != ERROR_SUCCESS)
                return reg_win32_to_nt(q_rc);
            if (result_len) *result_len = needed;
            if (buf_len < needed)
                return STATUS_BUFFER_TOO_SMALL;
            if (buffer) {
                KEY_FULL_INFORMATION *info = (KEY_FULL_INFORMATION *)buffer;
                nt_reg_memset(buffer, 0, needed);
                info->LastWriteTime = child_last_write;
                info->TitleIndex = 0;
                info->ClassOffset = (uint32_t)-1;
                info->ClassLength = 0;
                info->SubKeys = sub_keys;
                info->MaxNameLen = max_subkey_len;
                info->MaxClassLen = 0;
                info->Values = child_values;
                info->MaxValueNameLen = max_value_name_len;
                info->MaxValueDataLen = max_value_data_len;
            }
        }
        return STATUS_SUCCESS;
    }
    case KeyNodeInformation:
    default:
        return STATUS_INVALID_PARAMETER;
    }
}

/* ======================================================================== */
/* NtEnumerateValueKey (SSDT 0x009A)                                       */
/*                                                                          */
/* a1 = HANDLE KeyHandle                                                   */
/* a2 = uint32_t Index                                                     */
/* a3 = KEY_VALUE_INFORMATION_CLASS                                        */
/* a4 = void* KeyValueInformation (out buffer)                             */
/* a5 = uint32_t Length (buffer size)                                      */
/* a6 = uint32_t* ResultLength (out)                                       */
/* ======================================================================== */

static NTSTATUS NtEnumerateValueKey_handler(uint64_t a1, uint64_t a2, uint64_t a3,
                                            uint64_t a4, uint64_t a5, uint64_t a6)
{
    HKEY hkey = (HKEY)(uintptr_t)a1;
    uint32_t index = (uint32_t)a2;
    KEY_VALUE_INFORMATION_CLASS info_class = (KEY_VALUE_INFORMATION_CLASS)a3;
    void *buffer = (void *)a4;
    uint32_t buf_len = (uint32_t)a5;
    uint32_t *result_len = (uint32_t *)a6;
    char vname[REG_MAX_VALUE_NAME + 1];
    uint32_t vname_size = REG_MAX_VALUE_NAME + 1;
    uint32_t type = 0;
    uint8_t data[REG_MAX_VALUE_SIZE];
    uint32_t data_size = REG_MAX_VALUE_SIZE;
    long rc;
    uint32_t name_len;

    if (!hkey)
        return STATUS_INVALID_HANDLE;

    rc = RegEnumValue(hkey, index, vname, &vname_size,
                      (uint32_t *)0, &type, data, &data_size);
    if (rc == ERROR_NO_MORE_ITEMS)
        return STATUS_NO_MORE_ENTRIES;
    if (rc != ERROR_SUCCESS)
        return reg_win32_to_nt(rc);

    name_len = nt_reg_strlen(vname);

    switch (info_class) {
    case KeyValueBasicInformation: {
        uint32_t needed = __builtin_offsetof(KEY_VALUE_BASIC_INFORMATION, Name) + name_len;
        if (result_len) *result_len = needed;
        if (buf_len < needed)
            return STATUS_BUFFER_TOO_SMALL;
        if (buffer) {
            KEY_VALUE_BASIC_INFORMATION *info = (KEY_VALUE_BASIC_INFORMATION *)buffer;
            nt_reg_memset(buffer, 0, needed);
            info->TitleIndex = 0;
            info->Type = type;
            info->NameLength = name_len;
            if (name_len > 0)
                nt_reg_memcpy(info->Name, vname, name_len);
        }
        return STATUS_SUCCESS;
    }
    case KeyValueFullInformation: {
        uint32_t needed = __builtin_offsetof(KEY_VALUE_FULL_INFORMATION, Name) + name_len + data_size;
        if (result_len) *result_len = needed;
        if (buf_len < needed)
            return STATUS_BUFFER_TOO_SMALL;
        if (buffer) {
            KEY_VALUE_FULL_INFORMATION *info = (KEY_VALUE_FULL_INFORMATION *)buffer;
            nt_reg_memset(buffer, 0, needed);
            info->TitleIndex = 0;
            info->Type = type;
            info->NameLength = name_len;
            info->DataOffset = (uint32_t)(__builtin_offsetof(KEY_VALUE_FULL_INFORMATION, Name) + name_len);
            info->DataLength = data_size;
            if (name_len > 0)
                nt_reg_memcpy(info->Name, vname, name_len);
            if (data_size > 0)
                nt_reg_memcpy((uint8_t *)buffer + info->DataOffset, data, data_size);
        }
        return STATUS_SUCCESS;
    }
    case KeyValuePartialInformation: {
        uint32_t needed = __builtin_offsetof(KEY_VALUE_PARTIAL_INFORMATION, Data) + data_size;
        if (result_len) *result_len = needed;
        if (buf_len < needed)
            return STATUS_BUFFER_TOO_SMALL;
        if (buffer) {
            KEY_VALUE_PARTIAL_INFORMATION *info = (KEY_VALUE_PARTIAL_INFORMATION *)buffer;
            nt_reg_memset(buffer, 0, needed);
            info->TitleIndex = 0;
            info->Type = type;
            info->DataLength = data_size;
            if (data_size > 0)
                nt_reg_memcpy(info->Data, data, data_size);
        }
        return STATUS_SUCCESS;
    }
    default:
        return STATUS_INVALID_PARAMETER;
    }
}

/* ======================================================================== */
/* NtQueryKey (SSDT 0x009B)                                                */
/*                                                                          */
/* a1 = HANDLE KeyHandle                                                   */
/* a2 = KEY_INFORMATION_CLASS                                              */
/* a3 = void* KeyInformation (out buffer)                                  */
/* a4 = uint32_t Length (buffer size)                                      */
/* a5 = uint32_t* ResultLength (out)                                       */
/* ======================================================================== */

static NTSTATUS NtQueryKey_handler(uint64_t a1, uint64_t a2, uint64_t a3,
                                   uint64_t a4, uint64_t a5, uint64_t a6)
{
    HKEY hkey = (HKEY)(uintptr_t)a1;
    KEY_INFORMATION_CLASS info_class = (KEY_INFORMATION_CLASS)a2;
    void *buffer = (void *)a3;
    uint32_t buf_len = (uint32_t)a4;
    uint32_t *result_len = (uint32_t *)a5;
    uint32_t sub_keys = 0, values = 0;
    uint32_t max_subkey_len = 0, max_value_name_len = 0, max_value_data_len = 0;
    uint64_t last_write = 0;
    long rc;

    (void)a6;

    if (!hkey)
        return STATUS_INVALID_HANDLE;

    rc = RegQueryInfoKey(hkey, (char *)0, (uint32_t *)0, (uint32_t *)0,
                         &sub_keys, &max_subkey_len, (uint32_t *)0,
                         &values, &max_value_name_len, &max_value_data_len,
                         (uint32_t *)0, &last_write);
    if (rc != ERROR_SUCCESS)
        return reg_win32_to_nt(rc);

    /* Resolve HKEY to reg_key_t* safely: handles predefined sentinels
     * and tombstoned user handles uniformly via resolve_hkey. */
    {
        reg_key_t *resolved_key = resolve_hkey(hkey);

    switch (info_class) {
    case KeyBasicInformation: {
        uint32_t name_len = resolved_key ? nt_reg_strlen(resolved_key->name) : 0;
        uint32_t needed = __builtin_offsetof(KEY_BASIC_INFORMATION, Name) + name_len;
        if (result_len) *result_len = needed;
        if (buf_len < needed)
            return STATUS_BUFFER_TOO_SMALL;
        if (buffer) {
            KEY_BASIC_INFORMATION *info = (KEY_BASIC_INFORMATION *)buffer;
            nt_reg_memset(buffer, 0, needed);
            info->LastWriteTime = last_write;
            info->TitleIndex = 0;
            info->NameLength = name_len;
            if (name_len > 0 && resolved_key)
                nt_reg_memcpy(info->Name, resolved_key->name, name_len);
        }
        return STATUS_SUCCESS;
    }
    case KeyFullInformation: {
        uint32_t needed = sizeof(KEY_FULL_INFORMATION);
        if (result_len) *result_len = needed;
        if (buf_len < needed)
            return STATUS_BUFFER_TOO_SMALL;
        if (buffer) {
            KEY_FULL_INFORMATION *info = (KEY_FULL_INFORMATION *)buffer;
            nt_reg_memset(buffer, 0, needed);
            info->LastWriteTime = last_write;
            info->TitleIndex = 0;
            info->ClassOffset = (uint32_t)-1;  /* no class string */
            info->ClassLength = 0;
            info->SubKeys = sub_keys;
            info->MaxNameLen = max_subkey_len;
            info->MaxClassLen = 0;
            info->Values = values;
            info->MaxValueNameLen = max_value_name_len;
            info->MaxValueDataLen = max_value_data_len;
        }
        return STATUS_SUCCESS;
    }
    case KeyNameInformation: {
        uint32_t name_len = resolved_key ? nt_reg_strlen(resolved_key->name) : 0;
        uint32_t needed = __builtin_offsetof(KEY_NAME_INFORMATION, Name) + name_len;
        if (result_len) *result_len = needed;
        if (buf_len < needed)
            return STATUS_BUFFER_TOO_SMALL;
        if (buffer) {
            KEY_NAME_INFORMATION *info = (KEY_NAME_INFORMATION *)buffer;
            nt_reg_memset(buffer, 0, needed);
            info->NameLength = name_len;
            if (name_len > 0 && resolved_key)
                nt_reg_memcpy(info->Name, resolved_key->name, name_len);
        }
        return STATUS_SUCCESS;
    }
    default:
        return STATUS_INVALID_PARAMETER;
    }
    } /* end resolved_key block */
}

/* ============================================================================
 * Advanced Registry Operations
 * ============================================================================ */

/* ---- Helper: resolve HKEY handle to reg_key_t safely -------------------- */

/* Returns NULL for NULL/invalid/tombstoned handles. Tombstoned keys have
 * name[0] == '\0' after delete/unload, which we treat as "stale handle"
 * and refuse to return (minimal handle-liveness enforcement pending full
 * refcount tracking -> XREF: 02-kernel-core/TODO-14-registry-completion.md
 * (registry syscalls)). */
static reg_key_t *resolve_hkey(HKEY hkey)
{
    reg_key_t *k;

    if (!hkey)
        return (reg_key_t *)0;
    if (RegIsPredefinedKey(hkey))
        return reg_resolve_predefined(hkey);
    k = hkey->key;
    if (!k || k->name[0] == '\0')  /* tombstone check */
        return (reg_key_t *)0;
    return k;
}

/* ---- Helper: resolve FILE handle from current task to VFS path --------- */

/* ======================================================================== */
/* NtFlushKey (SSDT 0x009C)                                                */
/*                                                                          */
/* a1 = HANDLE KeyHandle                                                   */
/*                                                                          */
/* Flushes dirty registry keys to their backing hive files. The parameter  */
/* selects a specific key's hive in Windows; our implementation flushes    */
/* all dirty hives (registry_flush iterates the hive table).               */
/* ======================================================================== */

static NTSTATUS NtFlushKey_handler(uint64_t a1, uint64_t a2, uint64_t a3,
                                   uint64_t a4, uint64_t a5, uint64_t a6)
{
    HKEY hkey = (HKEY)(uintptr_t)a1;
    reg_key_t *key;
    long wrc;

    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;

    if (!hkey)
        return STATUS_INVALID_HANDLE;

    key = resolve_hkey(hkey);
    if (!key)
        return STATUS_INVALID_HANDLE;

    /* Per-hive flush: only the hive containing hkey is saved, so an unrelated
     * dirty hive's I/O error cannot fail this call. */
    wrc = RegFlushKey(hkey);
    /* ERROR_INVALID_HANDLE here means C: is not mounted (the handle was already
     * validated above) -- treat as a no-op success.  Every other non-success
     * (ACCESS_DENIED, REGISTRY_IO_FAILED) is surfaced. */
    if (wrc == ERROR_SUCCESS || wrc == ERROR_INVALID_HANDLE)
        return STATUS_SUCCESS;
    return reg_win32_to_nt(wrc);
}

/* ======================================================================== */
/* NtNotifyChangeKey (SSDT 0x009D)                                         */
/*                                                                          */
/* Registry change notifications. Not implemented: requires async I/O      */
/* completion infrastructure and per-key watcher lists.                    */
/* -> XREF: 02-kernel-core/TODO-14-registry-completion.md (change notifs) */
/* ======================================================================== */

static NTSTATUS NtNotifyChangeKey_handler(uint64_t a1, uint64_t a2, uint64_t a3,
                                          uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    /* SCOPE-GAP-ALLOWED: blocked on registry watcher infrastructure */
    return STATUS_NOT_IMPLEMENTED;
}

/* ======================================================================== */
/* NtRenameKey (SSDT 0x009F)                                               */
/*                                                                          */
/* a1 = HANDLE KeyHandle                                                   */
/* a2 = UNICODE_STRING* NewName                                            */
/* ======================================================================== */

static NTSTATUS NtRenameKey_handler(uint64_t a1, uint64_t a2, uint64_t a3,
                                    uint64_t a4, uint64_t a5, uint64_t a6)
{
    HKEY hkey = (HKEY)(uintptr_t)a1;
    UNICODE_STRING *nn = (UNICODE_STRING *)a2;
    reg_key_t *key;
    const char *name_str;
    long rc;

    (void)a3; (void)a4; (void)a5; (void)a6;

    if (!hkey)
        return STATUS_INVALID_HANDLE;
    if (!nn || !nn->Buffer)
        return STATUS_INVALID_PARAMETER;
    if (RegIsPredefinedKey(hkey))
        return STATUS_ACCESS_DENIED;

    key = resolve_hkey(hkey);
    if (!key)
        return STATUS_INVALID_HANDLE;

    /* Direct-pointer path bypasses the Win32 chokepoint; renaming destroys the
     * old key name, so require DELETE on the handle. */
    if (reg_check_access(hkey, DELETE) != ERROR_SUCCESS)
        return STATUS_ACCESS_DENIED;

    name_str = (const char *)nn->Buffer;
    rc = RegRenameKeyDirect(key, name_str);
    return reg_win32_to_nt(rc);
}

/* ======================================================================== */
/* NtSaveKey (SSDT 0x00A0) / NtSaveKeyEx (SSDT 0x00A1)                     */
/*                                                                          */
/* a1 = HANDLE KeyHandle                                                   */
/* a2 = HANDLE FileHandle                                                  */
/* a3 = uint32_t Format (Ex only; ignored)                                 */
/*                                                                          */
/* Serializes the key's subtree to the hive file backing FileHandle.       */
/* FileHandle must be an open handle to an empty file on a mounted drive.  */
/* ======================================================================== */

static NTSTATUS NtSaveKey_handler(uint64_t a1, uint64_t a2, uint64_t a3,
                                  uint64_t a4, uint64_t a5, uint64_t a6)
{
    HKEY hkey = (HKEY)(uintptr_t)a1;
    reg_key_t *key;

    (void)a2;

    (void)a3; (void)a4; (void)a5; (void)a6;

    if (!hkey)
        return STATUS_INVALID_HANDLE;

    key = resolve_hkey(hkey);
    if (!key)
        return STATUS_INVALID_HANDLE;

    /* Subtree export is privilege-gated (SeBackupPrivilege).  No evaluator
     * exists yet (TODO-15 s2) -- fail closed so any handle holder cannot export
     * an arbitrary subtree.  Trusted kernel code uses hive_save() directly. */
    if (reg_check_access(hkey, KEY_READ) != ERROR_SUCCESS)
        return STATUS_ACCESS_DENIED;
    return STATUS_PRIVILEGE_NOT_HELD;
}

/* ======================================================================== */
/* NtRestoreKey (SSDT 0x00A2)                                              */
/*                                                                          */
/* a1 = HANDLE KeyHandle                                                   */
/* a2 = HANDLE FileHandle                                                  */
/* a3 = uint32_t Flags (REG_WHOLE_HIVE_VOLATILE, REG_NO_LAZY_FLUSH, etc.)  */
/*                                                                          */
/* Deserializes a hive file into the key subtree.                          */
/* ======================================================================== */

static NTSTATUS NtRestoreKey_handler(uint64_t a1, uint64_t a2, uint64_t a3,
                                     uint64_t a4, uint64_t a5, uint64_t a6)
{
    HKEY hkey = (HKEY)(uintptr_t)a1;
    reg_key_t *key;

    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;

    if (!hkey)
        return STATUS_INVALID_HANDLE;

    key = resolve_hkey(hkey);
    if (!key)
        return STATUS_INVALID_HANDLE;

    /* Subtree import is privilege-gated (SeRestorePrivilege).  No evaluator
     * exists yet (TODO-15 s2) -- fail closed so any handle holder cannot
     * overwrite a subtree from an arbitrary file.  Trusted kernel code uses
     * hive_load() directly. */
    if (reg_check_access(hkey, KEY_WRITE) != ERROR_SUCCESS)
        return STATUS_ACCESS_DENIED;
    return STATUS_PRIVILEGE_NOT_HELD;
}

/* ======================================================================== */
/* NtLoadKey (SSDT 0x00A3) / NtLoadKeyEx (SSDT 0x00A4)                     */
/*                                                                          */
/* a1 = OBJECT_ATTRIBUTES* TargetKey (registry mountpoint)                 */
/* a2 = OBJECT_ATTRIBUTES* SourceFile (hive file path)                     */
/* a3-a6 = Flags, TrustKeyHandle, EventHandle, etc. (Ex variants; ignored) */
/* ======================================================================== */

static NTSTATUS NtLoadKey_handler(uint64_t a1, uint64_t a2, uint64_t a3,
                                  uint64_t a4, uint64_t a5, uint64_t a6)
{
    OBJECT_ATTRIBUTES *target_oa = (OBJECT_ATTRIBUTES *)a1;
    OBJECT_ATTRIBUTES *source_oa = (OBJECT_ATTRIBUTES *)a2;
    const char *target_path;
    const char *source_path;
    const char *subpath;
    HKEY root, mount_key;
    uint32_t disposition = 0;
    long rc;
    int values_loaded;

    (void)a3; (void)a4; (void)a5; (void)a6;

    if (!target_oa || !target_oa->ObjectName || !target_oa->ObjectName->Buffer)
        return STATUS_INVALID_PARAMETER;
    if (!source_oa || !source_oa->ObjectName || !source_oa->ObjectName->Buffer)
        return STATUS_INVALID_PARAMETER;

    target_path = (const char *)target_oa->ObjectName->Buffer;
    source_path = (const char *)source_oa->ObjectName->Buffer;

    /* Strip \??\ device namespace prefix from source path if present */
    if (source_path[0] == '\\' && source_path[1] == '?' &&
        source_path[2] == '?' && source_path[3] == '\\')
        source_path += 4;

    /* Resolve target registry path to a root + subpath */
    root = nt_reg_resolve_path(target_path, &subpath);
    if (!root)
        return STATUS_OBJECT_NAME_NOT_FOUND;

    /* Create (or open) the mountpoint key.  Capture disposition so we can
     * roll back a newly-created key if hive_load subsequently fails. */
    rc = RegCreateKeyEx(root, subpath, 0, (const char *)0, 0,
                        KEY_ALL_ACCESS, (void *)0, &mount_key, &disposition);
    if (rc != ERROR_SUCCESS)
        return reg_win32_to_nt(rc);

    /* Load the hive file into the mountpoint */
    values_loaded = hive_load(source_path, mount_key->key);
    if (values_loaded < 0) {
        /* Rollback: if we just created this mountpoint, remove it so the
         * failed load does not leave an orphaned key in the tree. */
        if (disposition == REG_CREATED_NEW_KEY && mount_key->key)
            RegDeleteKeyDirect(mount_key->key);
        RegCloseKey(mount_key);
        return STATUS_REGISTRY_CORRUPT;
    }

    RegCloseKey(mount_key);
    return STATUS_SUCCESS;
}

/* ======================================================================== */
/* NtUnloadKey (SSDT 0x00A5) / NtUnloadKeyEx (SSDT 0x00A6)                 */
/*                                                                          */
/* a1 = OBJECT_ATTRIBUTES* TargetKey                                       */
/* a2 = HANDLE Event (Ex only; ignored)                                    */
/* ======================================================================== */

static NTSTATUS NtUnloadKey_handler(uint64_t a1, uint64_t a2, uint64_t a3,
                                    uint64_t a4, uint64_t a5, uint64_t a6)
{
    OBJECT_ATTRIBUTES *oa = (OBJECT_ATTRIBUTES *)a1;
    const char *nt_path;
    const char *subpath;
    HKEY root, key;
    reg_key_t *rk;
    long rc;

    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;

    if (!oa || !oa->ObjectName || !oa->ObjectName->Buffer)
        return STATUS_INVALID_PARAMETER;

    nt_path = (const char *)oa->ObjectName->Buffer;
    root = nt_reg_resolve_path(nt_path, &subpath);
    if (!root)
        return STATUS_OBJECT_NAME_NOT_FOUND;

    /* Must specify a specific subkey; cannot unload a root */
    if (!subpath || subpath[0] == '\0')
        return STATUS_ACCESS_DENIED;

    rc = RegOpenKeyEx(root, subpath, 0, KEY_ALL_ACCESS, &key);
    if (rc != ERROR_SUCCESS)
        return reg_win32_to_nt(rc);

    rk = key->key;
    if (!rk) {
        RegCloseKey(key);
        return STATUS_INVALID_HANDLE;
    }

    rc = RegUnloadHive(rk);

    /* Close the handle; the underlying key is gone after unload */
    RegCloseKey(key);
    return reg_win32_to_nt(rc);
}

/* ---- SSDT Registration -------------------------------------------------- */

void nt_registry_register_ssdt(void)
{
    /* Core CRUD */
    ssdt_register(SSDT_NtCreateKey,         (SSDT_HANDLER)NtCreateKey_handler);
    ssdt_register(SSDT_NtOpenKey,           (SSDT_HANDLER)NtOpenKey_handler);
    ssdt_register(SSDT_NtOpenKeyEx,         (SSDT_HANDLER)NtOpenKeyEx_handler);
    ssdt_register(SSDT_NtDeleteKey,         (SSDT_HANDLER)NtDeleteKey_handler);
    ssdt_register(SSDT_NtSetValueKey,       (SSDT_HANDLER)NtSetValueKey_handler);
    ssdt_register(SSDT_NtQueryValueKey,     (SSDT_HANDLER)NtQueryValueKey_handler);
    ssdt_register(SSDT_NtDeleteValueKey,    (SSDT_HANDLER)NtDeleteValueKey_handler);
    ssdt_register(SSDT_NtEnumerateKey,      (SSDT_HANDLER)NtEnumerateKey_handler);
    ssdt_register(SSDT_NtEnumerateValueKey, (SSDT_HANDLER)NtEnumerateValueKey_handler);
    ssdt_register(SSDT_NtQueryKey,          (SSDT_HANDLER)NtQueryKey_handler);

    /* Advanced: flush / notify / rename / save / restore / hive load */
    ssdt_register(SSDT_NtFlushKey,          (SSDT_HANDLER)NtFlushKey_handler);
    ssdt_register(SSDT_NtNotifyChangeKey,   (SSDT_HANDLER)NtNotifyChangeKey_handler);
    ssdt_register(SSDT_NtRenameKey,         (SSDT_HANDLER)NtRenameKey_handler);
    ssdt_register(SSDT_NtSaveKey,           (SSDT_HANDLER)NtSaveKey_handler);
    ssdt_register(SSDT_NtSaveKeyEx,         (SSDT_HANDLER)NtSaveKey_handler);
    ssdt_register(SSDT_NtRestoreKey,        (SSDT_HANDLER)NtRestoreKey_handler);
    ssdt_register(SSDT_NtLoadKey,           (SSDT_HANDLER)NtLoadKey_handler);
    ssdt_register(SSDT_NtLoadKeyEx,         (SSDT_HANDLER)NtLoadKey_handler);
    ssdt_register(SSDT_NtUnloadKey,         (SSDT_HANDLER)NtUnloadKey_handler);
    ssdt_register(SSDT_NtUnloadKeyEx,       (SSDT_HANDLER)NtUnloadKey_handler);

    klog(LOG_INFO, "nt",
         "NT registry: 20 handlers registered (S14 core + S15 advanced)");
}
