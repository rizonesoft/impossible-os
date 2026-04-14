/* ============================================================================
 * nt_namespace.c -- NT namespace syscall SSDT handlers (TODO-05 §17)
 *
 * Wires 6 NtXxx directory/symlink operations into the SSDT (0x0120-0x0125):
 *   NtCreateDirectoryObject, NtOpenDirectoryObject, NtQueryDirectoryObject,
 *   NtCreateSymbolicLinkObject, NtOpenSymbolicLinkObject, NtQuerySymbolicLinkObject.
 *
 * Handlers parse OBJECT_ATTRIBUTES into an ASCII path, allocate via the OB
 * namespace API (ob_ns_create_directory/ob_ns_create_symlink + ObInsertObject),
 * and return process-handle-table HANDLE values.
 *
 * NOTE on UNICODE_STRING: handlers cast OBJECT_ATTRIBUTES->ObjectName->Buffer
 * directly to const char* (kernel-wide ASCII pattern; UTF-16 decode tracked
 * in TODO-13 §4 reg_decode_unicode_string and the broader follow-on work).
 * ============================================================================ */

#include "kernel/nt/ssdt.h"
#include "kernel/nt/ntstatus.h"
#include "kernel/nt/nt_types.h"
#include "kernel/nt/service_numbers.h"
#include "kernel/sched/task.h"
#include "kernel/ob/ob.h"
#include "kernel/ob/ob_ns.h"
#include "kernel/klog.h"

/* Library entry points already implemented in src/kernel/ob/ob.c */
extern int NtOpenDirectoryObject(HANDLE_TABLE *ht, const char *name,
                                 uint32_t access, HANDLE *out_handle);
extern int NtQueryDirectoryObject(HANDLE_TABLE *ht, HANDLE dir_handle,
                                  OBJECT_DIRECTORY_INFORMATION *buffer,
                                  uint32_t buffer_count, uint32_t *context,
                                  uint32_t *return_count);

/* ---- Helpers ------------------------------------------------------------ */

/* Extract ASCII path from OBJECT_ATTRIBUTES.  Returns NULL on bad OA. */
static const char *oa_name(OBJECT_ATTRIBUTES *oa)
{
    if (!oa || !oa->ObjectName || !oa->ObjectName->Buffer)
        return (const char *)0;
    return (const char *)oa->ObjectName->Buffer;
}

/* Bounded path-length check (rejects unterminated or oversized inputs). */
static int path_within_bounds(const char *p)
{
    uint32_t i;
    if (!p)
        return 0;
    for (i = 0; i < OB_PATH_MAX; i++) {
        if (p[i] == '\0')
            return 1;
    }
    return 0;
}

/* Bounded strlen for fixed-size string fields. */
static uint32_t bounded_strlen(const char *s, uint32_t max)
{
    uint32_t i;
    for (i = 0; i < max; i++) {
        if (s[i] == '\0') return i;
    }
    return max;
}

/* Copy n bytes from src to dst (no overlap check). */
static void ns_memcpy(void *dst, const void *src, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    uint32_t i;
    for (i = 0; i < n; i++)
        d[i] = s[i];
}

/* Split a full path into "parent_dir_path" + "leaf_name".  Returns 0 on
 * success.  The parent path is written into parent_buf (NUL-terminated);
 * leaf_out is set to point at the leaf inside the original path. */
static int split_path(const char *path, char *parent_buf,
                      uint32_t parent_buf_size, const char **leaf_out)
{
    const char *last_sep = (const char *)0;
    const char *p;
    uint32_t parent_len;

    if (!path || !parent_buf || !leaf_out || parent_buf_size < 2)
        return -1;

    /* Find last '\' separator */
    for (p = path; *p; p++) {
        if (*p == '\\')
            last_sep = p;
    }

    if (!last_sep) {
        /* No separator: the path itself is a leaf under root (`\`) */
        parent_buf[0] = '\\';
        parent_buf[1] = '\0';
        *leaf_out = path;
        return 0;
    }

    /* Special case: leaf in root ("\Foo") */
    if (last_sep == path) {
        parent_buf[0] = '\\';
        parent_buf[1] = '\0';
        *leaf_out = path + 1;
        return 0;
    }

    parent_len = (uint32_t)(last_sep - path);
    if (parent_len + 1 > parent_buf_size)
        return -1;

    ns_memcpy(parent_buf, path, parent_len);
    parent_buf[parent_len] = '\0';
    *leaf_out = last_sep + 1;
    return 0;
}

/* ======================================================================== */
/* NtCreateDirectoryObject (SSDT 0x0120)                                   */
/*                                                                          */
/* a1 = HANDLE* DirectoryHandle (out)                                      */
/* a2 = ACCESS_MASK DesiredAccess                                          */
/* a3 = OBJECT_ATTRIBUTES* ObjectAttributes                                */
/* ======================================================================== */

static NTSTATUS NtCreateDirectoryObject_handler(uint64_t a1, uint64_t a2,
                                                uint64_t a3, uint64_t a4,
                                                uint64_t a5, uint64_t a6)
{
    HANDLE *out = (HANDLE *)a1;
    uint32_t access = (uint32_t)a2;
    OBJECT_ATTRIBUTES *oa = (OBJECT_ATTRIBUTES *)a3;
    const char *path;
    char parent_path[OB_PATH_MAX];
    const char *leaf;
    void *parent_dir;
    void *new_dir;
    HANDLE h;

    (void)a4; (void)a5; (void)a6;

    if (!out)
        return STATUS_INVALID_PARAMETER;

    path = oa_name(oa);
    if (!path || !path_within_bounds(path))
        return STATUS_INVALID_PARAMETER;

    /* Reject zero-length leaf or root-only path */
    {
        uint32_t plen = bounded_strlen(path, OB_PATH_MAX);
        if (plen == 0 || (plen == 1 && path[0] == '\\'))
            return STATUS_INVALID_PARAMETER;
    }

    if (split_path(path, parent_path, sizeof(parent_path), &leaf) < 0)
        return STATUS_INVALID_PARAMETER;
    if (leaf[0] == '\0' || bounded_strlen(leaf, OB_NAME_MAX + 1) > OB_NAME_MAX)
        return STATUS_INVALID_PARAMETER;

    /* Resolve parent directory */
    parent_dir = (void *)0;
    if (ObLookupObjectByName(parent_path, ObpDirectoryType, 0, &parent_dir) != 0
        || !parent_dir)
        return STATUS_OBJECT_NAME_NOT_FOUND;

    /* Reject if leaf already exists in parent */
    {
        char full_check[OB_PATH_MAX];
        uint32_t parent_len = bounded_strlen(parent_path, OB_PATH_MAX);
        uint32_t leaf_len = bounded_strlen(leaf, OB_NAME_MAX);
        void *existing = (void *)0;

        if (parent_len + 1 + leaf_len + 1 > sizeof(full_check)) {
            ObDereferenceObject(parent_dir);
            return STATUS_INVALID_PARAMETER;
        }
        ns_memcpy(full_check, parent_path, parent_len);
        /* Avoid double-backslash when parent is root */
        if (!(parent_len == 1 && parent_path[0] == '\\'))
            full_check[parent_len++] = '\\';
        ns_memcpy(full_check + parent_len, leaf, leaf_len);
        full_check[parent_len + leaf_len] = '\0';

        if (ObLookupObjectByName(full_check, (const OBJECT_TYPE *)0, 0,
                                 &existing) == 0 && existing) {
            ObDereferenceObject(existing);
            ObDereferenceObject(parent_dir);
            return STATUS_OBJECT_NAME_COLLISION;
        }
    }

    /* Create the new directory body.  ob_ns_create_directory sets
     * OB_FLAG_PERMANENT, so we must explicitly clear it on rollback paths
     * to allow ObDereferenceObject to actually free the object. */
    new_dir = ob_ns_create_directory(parent_dir);
    if (!new_dir) {
        ObDereferenceObject(parent_dir);
        return STATUS_NO_MEMORY;
    }

    /* Allocate the handle BEFORE inserting into the namespace.  This
     * guarantees we never leave a half-created entry in the directory if
     * handle allocation fails -- rollback is just a deref. */
    h = ObpAllocateHandle(&task_current()->handle_table, new_dir, access, 0);
    if (h == INVALID_HANDLE_VALUE) {
        OBJECT_HEADER *hdr = OB_HEADER_FROM_BODY(new_dir);
        hdr->flags &= ~OB_FLAG_PERMANENT;  /* allow free on deref */
        ObDereferenceObject(new_dir);
        ObDereferenceObject(parent_dir);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* Now insert.  If insert fails, free the handle (which derefs to 1),
     * clear permanent, and deref the creation ref to free the object. */
    if (ObInsertObject(new_dir, leaf, parent_dir) != 0) {
        OBJECT_HEADER *hdr = OB_HEADER_FROM_BODY(new_dir);
        ObpFreeHandle(&task_current()->handle_table, h);
        hdr->flags &= ~OB_FLAG_PERMANENT;
        ObDereferenceObject(new_dir);
        ObDereferenceObject(parent_dir);
        return STATUS_OBJECT_NAME_COLLISION;
    }

    ObDereferenceObject(new_dir);
    ObDereferenceObject(parent_dir);

    *out = h;
    return STATUS_SUCCESS;
}

/* ======================================================================== */
/* NtOpenDirectoryObject (SSDT 0x0121)                                     */
/*                                                                          */
/* a1 = HANDLE* DirectoryHandle (out)                                      */
/* a2 = ACCESS_MASK DesiredAccess                                          */
/* a3 = OBJECT_ATTRIBUTES* ObjectAttributes                                */
/* ======================================================================== */

static NTSTATUS NtOpenDirectoryObject_handler(uint64_t a1, uint64_t a2,
                                              uint64_t a3, uint64_t a4,
                                              uint64_t a5, uint64_t a6)
{
    HANDLE *out = (HANDLE *)a1;
    uint32_t access = (uint32_t)a2;
    OBJECT_ATTRIBUTES *oa = (OBJECT_ATTRIBUTES *)a3;
    const char *path;
    int rc;

    (void)a4; (void)a5; (void)a6;

    if (!out)
        return STATUS_INVALID_PARAMETER;

    path = oa_name(oa);
    if (!path || !path_within_bounds(path))
        return STATUS_INVALID_PARAMETER;

    rc = NtOpenDirectoryObject(&task_current()->handle_table, path,
                               access, out);
    if (rc != 0)
        return STATUS_OBJECT_NAME_NOT_FOUND;
    return STATUS_SUCCESS;
}

/* ======================================================================== */
/* NtQueryDirectoryObject (SSDT 0x0122)                                    */
/*                                                                          */
/* a1 = HANDLE DirectoryHandle                                             */
/* a2 = void* Buffer (OBJECT_DIRECTORY_INFORMATION array)                  */
/* a3 = uint32_t Length (buffer size in bytes)                             */
/* a4 = packed flags: bit 0 = ReturnSingleEntry, bit 8 = RestartScan       */
/* a5 = uint32_t* Context                                                  */
/* a6 = uint32_t* ReturnLength                                             */
/*                                                                          */
/* The two BOOLEAN flags are packed into the low byte and bit 8 of a4 to   */
/* keep both Context and ReturnLength as full 64-bit pointers (they may    */
/* live above 4 GiB in kernel heap or future user-mode mappings).          */
/* ======================================================================== */

static NTSTATUS NtQueryDirectoryObject_handler(uint64_t a1, uint64_t a2,
                                               uint64_t a3, uint64_t a4,
                                               uint64_t a5, uint64_t a6)
{
    HANDLE dir_handle = (HANDLE)(int32_t)a1;
    OBJECT_DIRECTORY_INFORMATION *buf = (OBJECT_DIRECTORY_INFORMATION *)a2;
    uint32_t buf_len = (uint32_t)a3;
    uint32_t single_entry = (uint32_t)(a4 & 0x1);
    uint32_t restart = (uint32_t)((a4 >> 8) & 0x1);
    uint32_t *context = (uint32_t *)a5;
    uint32_t *ret_len = (uint32_t *)a6;
    uint32_t buf_count;
    uint32_t local_ctx = 0;
    uint32_t local_ret = 0;
    int rc;

    if (!buf || buf_len < sizeof(OBJECT_DIRECTORY_INFORMATION))
        return STATUS_INVALID_PARAMETER;

    buf_count = buf_len / (uint32_t)sizeof(OBJECT_DIRECTORY_INFORMATION);

    /* ReturnSingleEntry caps the effective slot count to 1, matching
     * NtQueryDirectoryObject Win32 contract: caller iterates one entry
     * per call advancing Context. */
    if (single_entry && buf_count > 1)
        buf_count = 1;

    if (restart && context)
        *context = 0;

    /* If caller did not provide context/ret_len, use locals */
    if (!context) context = &local_ctx;
    if (!ret_len) ret_len = &local_ret;

    rc = NtQueryDirectoryObject(&task_current()->handle_table, dir_handle,
                                buf, buf_count, context, ret_len);
    if (rc != 0)
        return STATUS_INVALID_HANDLE;

    return *ret_len == 0 ? STATUS_NO_MORE_ENTRIES : STATUS_SUCCESS;
}

/* ======================================================================== */
/* NtCreateSymbolicLinkObject (SSDT 0x0123)                                */
/*                                                                          */
/* a1 = HANDLE* LinkHandle (out)                                           */
/* a2 = ACCESS_MASK DesiredAccess                                          */
/* a3 = OBJECT_ATTRIBUTES* ObjectAttributes                                */
/* a4 = UNICODE_STRING* LinkTarget                                         */
/* ======================================================================== */

static NTSTATUS NtCreateSymbolicLinkObject_handler(uint64_t a1, uint64_t a2,
                                                   uint64_t a3, uint64_t a4,
                                                   uint64_t a5, uint64_t a6)
{
    HANDLE *out = (HANDLE *)a1;
    uint32_t access = (uint32_t)a2;
    OBJECT_ATTRIBUTES *oa = (OBJECT_ATTRIBUTES *)a3;
    UNICODE_STRING *target_us = (UNICODE_STRING *)a4;
    const char *path;
    const char *target;
    char parent_path[OB_PATH_MAX];
    const char *leaf;
    void *parent_dir;
    void *new_link;
    HANDLE h;

    (void)a5; (void)a6;

    if (!out)
        return STATUS_INVALID_PARAMETER;
    if (!target_us || !target_us->Buffer)
        return STATUS_INVALID_PARAMETER;

    path = oa_name(oa);
    if (!path || !path_within_bounds(path))
        return STATUS_INVALID_PARAMETER;

    target = (const char *)target_us->Buffer;
    if (bounded_strlen(target, OB_SYMLINK_MAX) >= OB_SYMLINK_MAX)
        return STATUS_INVALID_PARAMETER;

    {
        uint32_t plen = bounded_strlen(path, OB_PATH_MAX);
        if (plen == 0 || (plen == 1 && path[0] == '\\'))
            return STATUS_INVALID_PARAMETER;
    }

    if (split_path(path, parent_path, sizeof(parent_path), &leaf) < 0)
        return STATUS_INVALID_PARAMETER;
    if (leaf[0] == '\0' || bounded_strlen(leaf, OB_NAME_MAX + 1) > OB_NAME_MAX)
        return STATUS_INVALID_PARAMETER;

    parent_dir = (void *)0;
    if (ObLookupObjectByName(parent_path, ObpDirectoryType, 0, &parent_dir) != 0
        || !parent_dir)
        return STATUS_OBJECT_NAME_NOT_FOUND;

    new_link = ob_ns_create_symlink(target);
    if (!new_link) {
        ObDereferenceObject(parent_dir);
        return STATUS_NO_MEMORY;
    }

    /* Handle alloc before insert (same atomicity reasoning as
     * NtCreateDirectoryObject_handler). */
    h = ObpAllocateHandle(&task_current()->handle_table, new_link, access, 0);
    if (h == INVALID_HANDLE_VALUE) {
        OBJECT_HEADER *hdr = OB_HEADER_FROM_BODY(new_link);
        hdr->flags &= ~OB_FLAG_PERMANENT;
        ObDereferenceObject(new_link);
        ObDereferenceObject(parent_dir);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (ObInsertObject(new_link, leaf, parent_dir) != 0) {
        OBJECT_HEADER *hdr = OB_HEADER_FROM_BODY(new_link);
        ObpFreeHandle(&task_current()->handle_table, h);
        hdr->flags &= ~OB_FLAG_PERMANENT;
        ObDereferenceObject(new_link);
        ObDereferenceObject(parent_dir);
        return STATUS_OBJECT_NAME_COLLISION;
    }

    ObDereferenceObject(new_link);
    ObDereferenceObject(parent_dir);

    *out = h;
    return STATUS_SUCCESS;
}

/* ======================================================================== */
/* NtOpenSymbolicLinkObject (SSDT 0x0124)                                  */
/*                                                                          */
/* a1 = HANDLE* LinkHandle (out)                                           */
/* a2 = ACCESS_MASK DesiredAccess                                          */
/* a3 = OBJECT_ATTRIBUTES* ObjectAttributes                                */
/*                                                                          */
/* Returns the link object itself (no auto-resolution) so callers can use  */
/* NtQuerySymbolicLinkObject to read the target.                           */
/* ======================================================================== */

static NTSTATUS NtOpenSymbolicLinkObject_handler(uint64_t a1, uint64_t a2,
                                                 uint64_t a3, uint64_t a4,
                                                 uint64_t a5, uint64_t a6)
{
    HANDLE *out = (HANDLE *)a1;
    uint32_t access = (uint32_t)a2;
    OBJECT_ATTRIBUTES *oa = (OBJECT_ATTRIBUTES *)a3;
    const char *path;
    void *body = (void *)0;
    HANDLE h;

    (void)a4; (void)a5; (void)a6;

    if (!out)
        return STATUS_INVALID_PARAMETER;

    path = oa_name(oa);
    if (!path || !path_within_bounds(path))
        return STATUS_INVALID_PARAMETER;

    /* Type-filter: ObLookupObjectByName follows leaf symlinks by default
     * when the caller asks for the target type.  Pass ObpSymlinkType to
     * force the lookup to stop at the symlink object itself. */
    if (ObLookupObjectByName(path, ObpSymlinkType, access, &body) != 0
        || !body)
        return STATUS_OBJECT_NAME_NOT_FOUND;

    h = ObpAllocateHandle(&task_current()->handle_table, body, access, 0);
    ObDereferenceObject(body);

    if (h == INVALID_HANDLE_VALUE)
        return STATUS_INSUFFICIENT_RESOURCES;

    *out = h;
    return STATUS_SUCCESS;
}

/* ======================================================================== */
/* NtQuerySymbolicLinkObject (SSDT 0x0125)                                 */
/*                                                                          */
/* a1 = HANDLE LinkHandle                                                  */
/* a2 = UNICODE_STRING* LinkTarget (caller-allocated; we fill .Buffer)     */
/* a3 = uint32_t* ReturnedLength (out)                                     */
/* ======================================================================== */

static NTSTATUS NtQuerySymbolicLinkObject_handler(uint64_t a1, uint64_t a2,
                                                  uint64_t a3, uint64_t a4,
                                                  uint64_t a5, uint64_t a6)
{
    HANDLE link_handle = (HANDLE)(int32_t)a1;
    UNICODE_STRING *target_us = (UNICODE_STRING *)a2;
    uint32_t *ret_len = (uint32_t *)a3;
    HANDLE_TABLE_ENTRY *entry;
    OBJECT_HEADER *hdr;
    OBJECT_SYMBOLIC_LINK *link;
    uint32_t target_len;

    (void)a4; (void)a5; (void)a6;

    if (!target_us)
        return STATUS_INVALID_PARAMETER;

    entry = ObpLookupHandle(&task_current()->handle_table, link_handle);
    if (!entry || !entry->object)
        return STATUS_INVALID_HANDLE;

    hdr = OB_HEADER_FROM_BODY(entry->object);
    if (hdr->type != ObpSymlinkType)
        return STATUS_OBJECT_TYPE_MISMATCH;

    link = (OBJECT_SYMBOLIC_LINK *)entry->object;
    target_len = bounded_strlen(link->target, OB_SYMLINK_MAX);

    /* Always report required length, even if buffer is too small */
    if (ret_len)
        *ret_len = target_len;

    if (!target_us->Buffer || target_us->MaximumLength < target_len)
        return STATUS_BUFFER_TOO_SMALL;

    ns_memcpy(target_us->Buffer, link->target, target_len);
    target_us->Length = (uint16_t)target_len;
    return STATUS_SUCCESS;
}

/* ---- SSDT Registration -------------------------------------------------- */

void nt_namespace_register_ssdt(void)
{
    ssdt_register(SSDT_NtCreateDirectoryObject,
                  (SSDT_HANDLER)NtCreateDirectoryObject_handler);
    ssdt_register(SSDT_NtOpenDirectoryObject,
                  (SSDT_HANDLER)NtOpenDirectoryObject_handler);
    ssdt_register(SSDT_NtQueryDirectoryObject,
                  (SSDT_HANDLER)NtQueryDirectoryObject_handler);
    ssdt_register(SSDT_NtCreateSymbolicLinkObject,
                  (SSDT_HANDLER)NtCreateSymbolicLinkObject_handler);
    ssdt_register(SSDT_NtOpenSymbolicLinkObject,
                  (SSDT_HANDLER)NtOpenSymbolicLinkObject_handler);
    ssdt_register(SSDT_NtQuerySymbolicLinkObject,
                  (SSDT_HANDLER)NtQuerySymbolicLinkObject_handler);

    klog(LOG_INFO, "nt",
         "NT namespace: 6 handlers registered (S17 directory + symlink)");
}
