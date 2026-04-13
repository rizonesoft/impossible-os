/* ============================================================================
 * nt_registry.c -- NT registry syscall SSDT handlers (core CRUD)
 *
 * Wires 10 NtXxx registry operations into the SSDT, translating NT
 * object-namespace paths (\Registry\Machine\...) into Win32 RegXxx calls
 * against the existing registry engine (registry.c).
 *
 * TODO-05 section 14.
 * ============================================================================ */

#include "kernel/nt/nt_registry.h"
#include "kernel/nt/ssdt.h"
#include "kernel/nt/ntstatus.h"
#include "kernel/nt/nt_types.h"
#include "kernel/nt/service_numbers.h"
#include "kernel/klog.h"
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
    if (nt_reg_prefix(p, "Machine")) {
        /* Exact match with no trailing path */
        *subpath = "";
        return HKEY_LOCAL_MACHINE;
    }
    if (nt_reg_prefix(p, "User\\")) {
        *subpath = p + 5;
        return HKEY_USERS;
    }
    if (nt_reg_prefix(p, "User")) {
        *subpath = "";
        return HKEY_USERS;
    }
    if (nt_reg_prefix(p, "CurrentConfig\\")) {
        *subpath = p + 14;
        return HKEY_CURRENT_CONFIG;
    }
    if (nt_reg_prefix(p, "CurrentConfig")) {
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

/* ---- Helper: convert Win32 error to NTSTATUS ---------------------------- */

static NTSTATUS reg_win32_to_nt(long err)
{
    switch (err) {
    case ERROR_SUCCESS:          return STATUS_SUCCESS;
    case ERROR_FILE_NOT_FOUND:   return STATUS_OBJECT_NAME_NOT_FOUND;
    case ERROR_ACCESS_DENIED:    return STATUS_KEY_HAS_CHILDREN;
    case ERROR_INVALID_HANDLE:   return STATUS_INVALID_HANDLE;
    case ERROR_OUTOFMEMORY:      return STATUS_NO_MEMORY;
    case ERROR_INVALID_PARAMETER:return STATUS_INVALID_PARAMETER;
    case ERROR_MORE_DATA:        return STATUS_BUFFER_TOO_SMALL;
    case ERROR_NO_MORE_ITEMS:    return STATUS_NO_MORE_ENTRIES;
    case ERROR_KEY_DELETED:      return STATUS_KEY_DELETED;
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
    const char *nt_path;
    const char *subpath;
    HKEY root, result_key;
    uint32_t disp = 0;
    long rc;

    (void)a2; (void)a4; (void)a5;

    if (!out)
        return STATUS_INVALID_PARAMETER;

    nt_path = reg_oa_path(oa);
    if (!nt_path)
        return STATUS_INVALID_PARAMETER;

    root = nt_reg_resolve_path(nt_path, &subpath);
    if (!root)
        return STATUS_OBJECT_NAME_NOT_FOUND;

    rc = RegCreateKeyEx(root, subpath, 0, (const char *)0,
                        create_options, KEY_ALL_ACCESS, (void *)0,
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
    long rc;

    (void)a2; (void)a4; (void)a5; (void)a6;

    if (!out)
        return STATUS_INVALID_PARAMETER;

    nt_path = reg_oa_path(oa);
    if (!nt_path)
        return STATUS_INVALID_PARAMETER;

    root = nt_reg_resolve_path(nt_path, &subpath);
    if (!root)
        return STATUS_OBJECT_NAME_NOT_FOUND;

    rc = RegOpenKeyEx(root, subpath, 0, KEY_ALL_ACCESS, &result_key);
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
     * Use RegDeleteKeyDirect which operates on the internal reg_key_t. */
    key = hkey->key;
    if (!key)
        return STATUS_INVALID_HANDLE;

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
        uint32_t needed = sizeof(KEY_VALUE_BASIC_INFORMATION) - 1 + name_len;
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
        uint32_t needed = sizeof(KEY_VALUE_FULL_INFORMATION) - 1 + name_len + data_size;
        if (result_len) *result_len = needed;
        if (buf_len < needed)
            return STATUS_BUFFER_TOO_SMALL;
        if (buffer) {
            KEY_VALUE_FULL_INFORMATION *info = (KEY_VALUE_FULL_INFORMATION *)buffer;
            nt_reg_memset(buffer, 0, needed);
            info->TitleIndex = 0;
            info->Type = type;
            info->NameLength = name_len;
            info->DataOffset = (uint32_t)(sizeof(KEY_VALUE_FULL_INFORMATION) - 1 + name_len);
            info->DataLength = data_size;
            if (name_len > 0)
                nt_reg_memcpy(info->Name, name_str, name_len);
            if (data_size > 0)
                nt_reg_memcpy((uint8_t *)buffer + info->DataOffset, data, data_size);
        }
        return STATUS_SUCCESS;
    }
    case KeyValuePartialInformation: {
        uint32_t needed = sizeof(KEY_VALUE_PARTIAL_INFORMATION) - 1 + data_size;
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
        uint32_t needed = sizeof(KEY_BASIC_INFORMATION) - 1 + name_len;
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
        uint32_t needed = sizeof(KEY_NAME_INFORMATION) - 1 + name_len;
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
    case KeyFullInformation:
    case KeyNodeInformation:
        /* These require additional data (class string, sub-key counts) that
         * RegEnumKeyEx does not provide per-child.  Return basic for now. */
        return STATUS_INVALID_PARAMETER;
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
        uint32_t needed = sizeof(KEY_VALUE_BASIC_INFORMATION) - 1 + name_len;
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
        uint32_t needed = sizeof(KEY_VALUE_FULL_INFORMATION) - 1 + name_len + data_size;
        if (result_len) *result_len = needed;
        if (buf_len < needed)
            return STATUS_BUFFER_TOO_SMALL;
        if (buffer) {
            KEY_VALUE_FULL_INFORMATION *info = (KEY_VALUE_FULL_INFORMATION *)buffer;
            nt_reg_memset(buffer, 0, needed);
            info->TitleIndex = 0;
            info->Type = type;
            info->NameLength = name_len;
            info->DataOffset = (uint32_t)(sizeof(KEY_VALUE_FULL_INFORMATION) - 1 + name_len);
            info->DataLength = data_size;
            if (name_len > 0)
                nt_reg_memcpy(info->Name, vname, name_len);
            if (data_size > 0)
                nt_reg_memcpy((uint8_t *)buffer + info->DataOffset, data, data_size);
        }
        return STATUS_SUCCESS;
    }
    case KeyValuePartialInformation: {
        uint32_t needed = sizeof(KEY_VALUE_PARTIAL_INFORMATION) - 1 + data_size;
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

    /* Resolve HKEY to reg_key_t* safely (handles predefined sentinels) */
    {
        reg_key_t *resolved_key;
        if (RegIsPredefinedKey(hkey))
            resolved_key = reg_resolve_predefined(hkey);
        else
            resolved_key = hkey->key;

    switch (info_class) {
    case KeyBasicInformation: {
        uint32_t name_len = resolved_key ? nt_reg_strlen(resolved_key->name) : 0;
        uint32_t needed = sizeof(KEY_BASIC_INFORMATION) - 1 + name_len;
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
        uint32_t needed = sizeof(KEY_NAME_INFORMATION) - 1 + name_len;
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

/* ---- SSDT Registration -------------------------------------------------- */

void nt_registry_register_ssdt(void)
{
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

    klog(LOG_INFO, "nt", "NT registry: 10 core CRUD handlers registered (S14)");
}
