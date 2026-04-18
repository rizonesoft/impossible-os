/* ============================================================================
 * ob_ns.c -- Object namespace: directories, symlinks, path resolution
 *
 * Implements the hierarchical in-memory object namespace.
 * ============================================================================ */

#include "kernel/ob/ob.h"
#include "kernel/ob/ob_ns.h"
#include "kernel/mm/heap.h"
#include "kernel/klog.h"

extern void *memset(void *s, int c, size_t n);
extern size_t strlen(const char *s);
extern int strcmp(const char *a, const char *b);
extern int strncmp(const char *a, const char *b, size_t n);
extern char *strncpy(char *dst, const char *src, size_t n);

/* --- Root directory ------------------------------------------------------ */

void *ObpRootDirectory = NULL;

/* --- Helper: find entry by name in a directory --------------------------- */

static OBJECT_DIRECTORY_ENTRY *dir_find(OBJECT_DIRECTORY *dir, const char *name,
                                        size_t name_len)
{
    OBJECT_DIRECTORY_ENTRY *e = dir->first;
    while (e) {
        if (strlen(e->name) == name_len &&
            strncmp(e->name, name, name_len) == 0)
            return e;
        e = e->next;
    }
    return NULL;
}

/* --- ob_ns_create_directory ---------------------------------------------- */

void *ob_ns_create_directory(void *parent)
{
    void *body = ob_alloc_object(ObpDirectoryType);
    if (!body)
        return NULL;

    OBJECT_DIRECTORY *dir = (OBJECT_DIRECTORY *)body;
    dir->first  = NULL;
    dir->count  = 0;
    dir->parent = parent;

    OBJECT_HEADER *hdr = OB_HEADER_FROM_BODY(body);
    hdr->flags |= OB_FLAG_PERMANENT | OB_FLAG_KERNEL_ONLY;

    return body;
}

/* --- ob_ns_create_symlink ------------------------------------------------ */

void *ob_ns_create_symlink(const char *target)
{
    void *body = ob_alloc_object(ObpSymlinkType);
    if (!body)
        return NULL;

    OBJECT_SYMBOLIC_LINK *sl = (OBJECT_SYMBOLIC_LINK *)body;
    strncpy(sl->target, target, OB_SYMLINK_MAX - 1);
    sl->target[OB_SYMLINK_MAX - 1] = '\0';

    OBJECT_HEADER *hdr = OB_HEADER_FROM_BODY(body);
    hdr->flags |= OB_FLAG_PERMANENT | OB_FLAG_KERNEL_ONLY;

    return body;
}

/* --- ObInsertObject ------------------------------------------------------ */

int ObInsertObject(void *object, const char *name, void *directory)
{
    OBJECT_DIRECTORY *dir = (OBJECT_DIRECTORY *)directory;
    OBJECT_DIRECTORY_ENTRY *entry;
    size_t name_len;

    if (!object || !name || !directory)
        return -1;

    name_len = strlen(name);
    if (name_len == 0 || name_len >= OB_NAME_MAX)
        return -1;

    /* Check for duplicate */
    if (dir_find(dir, name, name_len))
        return -1;

    if (dir->count >= OB_DIR_MAX_ENTRIES)
        return -1;

    /* Allocate entry node */
    entry = (OBJECT_DIRECTORY_ENTRY *)kmalloc(sizeof(OBJECT_DIRECTORY_ENTRY));
    if (!entry)
        return -1;

    strncpy(entry->name, name, OB_NAME_MAX - 1);
    entry->name[OB_NAME_MAX - 1] = '\0';
    entry->object = object;

    /* Prepend to linked list */
    entry->next = dir->first;
    dir->first = entry;
    dir->count++;

    /* Mark the object as named */
    OBJECT_HEADER *hdr = OB_HEADER_FROM_BODY(object);
    hdr->flags |= OB_FLAG_NAMED;
    hdr->name = entry->name;

    /* Hold a reference for the namespace entry */
    ObReferenceObject(object);

    return 0;
}

/* --- ObpLookupDirectory -------------------------------------------------- */

void *ObpLookupDirectory(const char *path, const char **remaining)
{
    void *cur_dir = ObpRootDirectory;
    const char *p = path;
    uint32_t symlink_count = 0;

    if (!cur_dir || !path) {
        if (remaining) *remaining = path;
        return NULL;
    }

    /* Skip leading backslash */
    if (*p == '\\')
        p++;

    while (*p) {
        const char *seg_start = p;
        size_t seg_len;
        OBJECT_DIRECTORY *dir;
        OBJECT_DIRECTORY_ENTRY *entry;
        OBJECT_HEADER *hdr;

        /* Find end of this path component */
        while (*p && *p != '\\')
            p++;
        seg_len = (size_t)(p - seg_start);

        if (seg_len == 0) {
            if (*p == '\\') p++;
            continue;
        }

        dir = (OBJECT_DIRECTORY *)cur_dir;
        entry = dir_find(dir, seg_start, seg_len);

        if (!entry) {
            /* No match -- return current dir with remaining path */
            if (remaining) *remaining = seg_start;
            return cur_dir;
        }

        hdr = OB_HEADER_FROM_BODY(entry->object);

        /* Follow symlinks only when more path follows this component.
         * If the symlink is the final path component, stop at the parent
         * directory and return remaining = segment name so
         * ObLookupObjectByName can open the symlink object (NtOpenSymbolicLinkObject). */
        if (hdr->type == ObpSymlinkType) {
            if (*p == '\0') {
                if (remaining)
                    *remaining = seg_start;
                return cur_dir;
            }
            if (++symlink_count > OB_SYMLINK_DEPTH) {
                if (remaining) *remaining = seg_start;
                return cur_dir;
            }
            OBJECT_SYMBOLIC_LINK *sl = (OBJECT_SYMBOLIC_LINK *)entry->object;
            /* Resolve the symlink target from root */
            const char *sym_rem = NULL;
            void *target_dir = ObpLookupDirectory(sl->target, &sym_rem);
            if (!target_dir) {
                if (remaining) *remaining = seg_start;
                return cur_dir;
            }
            /* If symlink resolved fully and there's more path, continue from target */
            if (sym_rem && *sym_rem) {
                if (remaining) *remaining = seg_start;
                return cur_dir;
            }
            cur_dir = target_dir;
            /* Skip separator after this component */
            if (*p == '\\') p++;
            continue;
        }

        /* Must be a directory to continue walking */
        if (hdr->type != ObpDirectoryType) {
            /* Non-directory found -- return parent dir with remaining */
            if (remaining) *remaining = seg_start;
            return cur_dir;
        }

        cur_dir = entry->object;

        /* Skip separator */
        if (*p == '\\')
            p++;
    }

    if (remaining) *remaining = p;  /* empty string = fully resolved */
    return cur_dir;
}

/* --- ObLookupObjectByName ------------------------------------------------ */

int ObLookupObjectByName(const char *path, const OBJECT_TYPE *type,
                         uint32_t access, void **result)
{
    const char *remaining = NULL;
    void *dir;
    OBJECT_DIRECTORY *dobj;
    OBJECT_DIRECTORY_ENTRY *entry;
    OBJECT_HEADER *hdr;
    size_t rem_len;

    (void)access;  /* used by §8 security checks */

    if (!path || !result)
        return -1;

    *result = NULL;

    dir = ObpLookupDirectory(path, &remaining);
    if (!dir)
        return -1;

    /* If fully resolved, the path names a directory itself */
    if (!remaining || !*remaining) {
        /* The path resolved to a directory -- check type match */
        hdr = OB_HEADER_FROM_BODY(dir);
        if (type && hdr->type != type)
            return -1;
        ObReferenceObject(dir);
        *result = dir;
        return 0;
    }

    /* remaining should be a single component (the leaf name) */
    rem_len = strlen(remaining);
    /* Strip trailing backslash if any */
    if (rem_len > 0 && remaining[rem_len - 1] == '\\')
        rem_len--;

    /* Check there's no further separator (would mean unresolved intermediate) */
    {
        size_t i;
        for (i = 0; i < rem_len; i++) {
            if (remaining[i] == '\\')
                return -1;  /* unresolved intermediate component */
        }
    }

    dobj = (OBJECT_DIRECTORY *)dir;
    entry = dir_find(dobj, remaining, rem_len);
    if (!entry)
        return -1;

    /* Follow symlink at leaf level unless caller asked for the symlink type
     * (NtOpenSymbolicLinkObject stops at the link; other lookups resolve). */
    hdr = OB_HEADER_FROM_BODY(entry->object);
    if (hdr->type == ObpSymlinkType && type != ObpSymlinkType) {
        OBJECT_SYMBOLIC_LINK *sl = (OBJECT_SYMBOLIC_LINK *)entry->object;
        return ObLookupObjectByName(sl->target, type, access, result);
    }

    /* Type check */
    if (type && hdr->type != type)
        return -1;

    ObReferenceObject(entry->object);
    *result = entry->object;
    return 0;
}

/* --- Helper: create and insert a sub-directory --------------------------- */

static void *ns_mkdir(void *parent, const char *name)
{
    void *dir = ob_ns_create_directory(parent);
    if (!dir) {
        klog(LOG_ERROR, "ob", "ns_mkdir: failed to create '%s'", name);
        return NULL;
    }
    if (ObInsertObject(dir, name, parent) < 0) {
        klog(LOG_ERROR, "ob", "ns_mkdir: failed to insert '%s'", name);
        return NULL;
    }
    return dir;
}

/* --- ob_ns_init ---------------------------------------------------------- */

void ob_ns_init(void)
{
    void *device_dir;
    void *kernel_objects_dir;
    void *dos_devices_dir;
    void *bno_dir;
    void *sessions_dir;
    void *s0_dir;
    void *symlink;

    /* Create root `\` */
    ObpRootDirectory = ob_ns_create_directory(NULL);
    if (!ObpRootDirectory) {
        klog(LOG_ERROR, "ob", "Failed to create root namespace");
        return;
    }

    /* Standard directories */
    device_dir         = ns_mkdir(ObpRootDirectory, "Device");
    kernel_objects_dir = ns_mkdir(ObpRootDirectory, "KernelObjects");
    dos_devices_dir    = ns_mkdir(ObpRootDirectory, "DosDevices");
    bno_dir            = ns_mkdir(ObpRootDirectory, "BaseNamedObjects");

    /* \Sessions\0\BaseNamedObjects -- alias for user-mode named objects */
    sessions_dir = ns_mkdir(ObpRootDirectory, "Sessions");
    if (sessions_dir) {
        s0_dir = ns_mkdir(sessions_dir, "0");
        if (s0_dir) {
            /* Create symlink: \Sessions\0\BaseNamedObjects -> \BaseNamedObjects */
            symlink = ob_ns_create_symlink("\\BaseNamedObjects");
            if (symlink)
                ObInsertObject(symlink, "BaseNamedObjects", s0_dir);
        }
    }

    /* C: -> \Device\HardDisk0\Partition0 via symlink in \DosDevices */
    if (dos_devices_dir && device_dir) {
        /* Create the target directory chain first */
        void *hd0_dir = ns_mkdir(device_dir, "HardDisk0");
        if (hd0_dir) {
            void *part0_dir = ns_mkdir(hd0_dir, "Partition0");
            (void)part0_dir;
        }

        symlink = ob_ns_create_symlink("\\Device\\HardDisk0\\Partition0");
        if (symlink)
            ObInsertObject(symlink, "C:", dos_devices_dir);
    }

    /* Suppress unused warnings */
    (void)kernel_objects_dir;
    (void)bno_dir;

    klog(LOG_INFO, "ob", "Namespace: \\, \\Device, \\KernelObjects, "
         "\\DosDevices, \\BaseNamedObjects, \\Sessions\\0\\BaseNamedObjects");
}
