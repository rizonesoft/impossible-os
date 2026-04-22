/* ============================================================================
 * ob_info_file.c -- Read-only kernel-metadata pseudo-files
 *
 * Read-only kernel-metadata pseudo-files exposed in the Object Manager
 * namespace under `\ObjectManager\<name>`.
 *
 * The info-file type sits alongside ObpFileType but never touches the
 * VFS: its body carries a kernel-supplied read callback plus the fixed
 * virtual byte-size of the exposed snapshot. ob_info_file_read snaps
 * that callback with the caller-provided offset + size and reports
 * bytes written; reads past `size` return 0 (EOF).
 *
 * Files register under `\ObjectManager\<name>`. The `\ObjectManager`
 * directory is created lazily on the first register call so subsystems
 * can publish themselves during their own init without depending on a
 * specific ob_init ordering.
 * ============================================================================ */

#include "kernel/ob/ob_info_file.h"
#include "kernel/ob/ob.h"
#include "kernel/ob/ob_ns.h"
#include "kernel/ob/ob_type.h"
#include "kernel/klog.h"

extern void *memcpy(void *dst, const void *src, size_t n);

/* ---- Type singleton ----------------------------------------------------- */

const OBJECT_TYPE *ObpInfoFileType = NULL;

/* ---- ObjectManager directory (created by ob_ns_init) -------------------- */

/* \ObjectManager is created by ob_ns_init alongside \Device, \KernelObjects,
 * etc. We cache a non-ref-owning pointer on first register; the permanent
 * namespace directory outlives any caller so we don't need a ref. */
static void *s_object_manager_dir = NULL;

static void *get_objectmanager_dir(void)
{
    if (s_object_manager_dir)
        return s_object_manager_dir;

    void *dir = NULL;
    if (ObLookupObjectByName("\\ObjectManager", ObpDirectoryType, 0,
                             &dir) != 0 || !dir) {
        klog(LOG_ERROR, "ob", "ob_info_file: \\ObjectManager not in namespace");
        return NULL;
    }
    /* Cache + drop the lookup ref. The directory is PERMANENT so it
     * stays alive without our ref; the cache pointer is stable. */
    s_object_manager_dir = dir;
    ObDereferenceObject(dir);
    return s_object_manager_dir;
}

/* ---- Type registration -------------------------------------------------- */

void ob_info_file_type_init(void)
{
    ObpInfoFileType = ob_create_type(&(OBJECT_TYPE){
        .name      = "ObjectInfoFile",
        .body_size = sizeof(OB_INFO_FILE),
        /* No on_close: info files hold no resources.
         * No on_delete: the body is a trivial struct.
         * No on_parse / on_open. */
    });
    if (!ObpInfoFileType)
        klog(LOG_ERROR, "ob", "Failed to register ObpInfoFileType");
}

/* ---- ob_info_file_register --------------------------------------------- */

int ob_info_file_register(const char *name, ob_info_read_fn read_fn,
                          uint32_t size)
{
    if (!name || !read_fn || size == 0) {
        klog(LOG_WARN, "ob", "ob_info_file_register: invalid args "
             "(name=%p read_fn=%p size=%u)",
             (uint64_t)(uintptr_t)name, (uint64_t)(uintptr_t)read_fn,
             (uint64_t)size);
        return -1;
    }
    if (!ObpInfoFileType) {
        klog(LOG_ERROR, "ob", "ob_info_file_register('%s'): type not initialized",
             name);
        return -1;
    }

    void *dir = get_objectmanager_dir();
    if (!dir)
        return -1;

    OB_INFO_FILE *info = (OB_INFO_FILE *)ob_alloc_object(ObpInfoFileType);
    if (!info) {
        klog(LOG_ERROR, "ob", "ob_info_file_register('%s'): alloc failed", name);
        return -1;
    }

    info->read_fn = read_fn;
    info->size    = size;
    info->_pad    = 0;

    /* Mark PERMANENT before the namespace insert so a racing lookup
     * that increments + decrements the ref cannot destroy the object
     * between insert and the flag write. */
    OB_HEADER_FROM_BODY(info)->flags |= OB_FLAG_PERMANENT | OB_FLAG_KERNEL_ONLY;

    if (ObInsertObject(info, name, dir) < 0) {
        /* Insert failed (duplicate name or full directory). This is not
         * necessarily an error -- duplicate register is how callers
         * probe "already registered" today. Log at WARN so the boot
         * path still surfaces capacity exhaustion, but do not pollute
         * the klog error count on the expected duplicate case. Clear
         * PERMANENT so the deref below actually frees the orphaned
         * body. */
        OB_HEADER_FROM_BODY(info)->flags &= ~OB_FLAG_PERMANENT;
        ObDereferenceObject(info);
        return -1;
    }

    /* Drop the creation ref -- the directory entry now holds a ref on
     * the body, and OB_FLAG_PERMANENT keeps ref_count > 0 from ever
     * destroying the object anyway. Without this drop, the creation
     * ref leaks forever: ObMakeTemporaryObject + ObpRemoveFromDirectory
     * in tests would leave ref_count = 1 and the body would never
     * free. Mirrors ob_create_file_handle's post-AllocateHandle drop. */
    ObDereferenceObject(info);

    klog(LOG_INFO, "ob", "ob_info_file: registered \\ObjectManager\\%s (%u bytes)",
         (uint64_t)(uintptr_t)name, (uint64_t)size);
    return 0;
}

/* ---- ob_info_file_open_handle ------------------------------------------ */

HANDLE ob_info_file_open_handle(HANDLE_TABLE *ht, const char *name)
{
    if (!ht || !name)
        return INVALID_HANDLE_VALUE;

    /* Build "\ObjectManager\<name>" on the stack. 64 chars (OB_NAME_MAX)
     * leaf + 16-char prefix fits comfortably; larger names are refused
     * by ObInsertObject's OB_NAME_MAX anyway. */
    char path[128];
    const char prefix[] = "\\ObjectManager\\";
    const size_t prefix_len = sizeof(prefix) - 1;  /* strlen, compile-time */
    size_t name_len = 0;
    while (name[name_len])
        name_len++;
    if (prefix_len + name_len + 1 > sizeof(path)) {
        return INVALID_HANDLE_VALUE;
    }
    {
        size_t i;
        for (i = 0; i < prefix_len; i++) path[i] = prefix[i];
        for (i = 0; i < name_len;   i++) path[prefix_len + i] = name[i];
        path[prefix_len + name_len] = '\0';
    }

    void *body = NULL;
    if (ObLookupObjectByName(path, ObpInfoFileType, 0, &body) != 0 || !body)
        return INVALID_HANDLE_VALUE;

    /* ObLookupObjectByName returned a ref-owning body. Transfer that
     * ref into the handle: ObpAllocateHandle adds a ref for the handle,
     * and we drop the lookup ref afterward. Mirrors ob_create_file_handle. */
    HANDLE h = ObpAllocateHandle(ht, body, 0, 0);
    ObDereferenceObject(body);
    return h;
}

/* ---- ob_info_file_read -------------------------------------------------- */

int32_t ob_info_file_read(HANDLE_TABLE *ht, HANDLE h, void *buf,
                          uint32_t size, uint32_t offset)
{
    if (!ht || !buf)
        return -1;

    HANDLE_TABLE_ENTRY *entry = ObpLookupHandle(ht, h);
    if (!entry)
        return -1;

    OBJECT_HEADER *hdr = OB_HEADER_FROM_BODY(entry->object);
    if (hdr->type != ObpInfoFileType)
        return -1;

    OB_INFO_FILE *info = (OB_INFO_FILE *)entry->object;
    if (!info->read_fn || info->size == 0)
        return -1;

    /* EOF: reads past the declared file size return 0. */
    if (offset >= info->size)
        return 0;

    /* Clamp the request to not read past the file's fixed size. */
    uint32_t avail = info->size - offset;
    uint32_t want  = size < avail ? size : avail;
    if (want == 0)
        return 0;

    return info->read_fn((uint8_t *)buf, want, offset);
}
