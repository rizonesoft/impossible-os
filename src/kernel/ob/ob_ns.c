/* ============================================================================
 * ob_ns.c -- Object namespace: directories, symlinks, path resolution
 *
 * Implements the hierarchical in-memory object namespace.
 * ============================================================================ */

#include "kernel/ob/ob.h"
#include "kernel/ob/ob_ns.h"
#include "kernel/mm/heap.h"
#include "kernel/klog.h"
#include "kernel/nt/nt_rtlstr.h"   /* rtl_upcase_char_inline -- canonical compiled fold */

extern void *memset(void *s, int c, size_t n);
extern size_t strlen(const char *s);
extern int strcmp(const char *a, const char *b);
extern int strncmp(const char *a, const char *b, size_t n);
extern char *strncpy(char *dst, const char *src, size_t n);

/* --- Root directory ------------------------------------------------------ */

void *ObpRootDirectory = NULL;

/* --- Helper: find entry by name in a directory --------------------------- */

/* Case-insensitive object-name compare via the compiled invariant fold (the
 * same authority the atom table + registry use). The NT object namespace is
 * case-insensitive by default, so "\BaseNamedObjects\Foo" and "...\foo" resolve
 * to the same object. Names are byte strings today; each byte is folded as its
 * invariant upcase. Security compares route through the COMPILED fold, never the
 * disk-backed nls_upcase_char. */
static int dir_name_eq(const char *stored, const char *name, size_t name_len)
{
    size_t i;
    if (strlen(stored) != name_len)
        return 0;
    for (i = 0; i < name_len; i++) {
        if (rtl_upcase_char_inline((uint8_t)stored[i]) !=
            rtl_upcase_char_inline((uint8_t)name[i]))
            return 0;
    }
    return 1;
}

static OBJECT_DIRECTORY_ENTRY *dir_find(OBJECT_DIRECTORY *dir, const char *name,
                                        size_t name_len)
{
    OBJECT_DIRECTORY_ENTRY *e = dir->first;
    while (e) {
        if (dir_name_eq(e->name, name, name_len))
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
    dir->lock   = (spinlock_t)SPINLOCK_INIT;

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

/* --- ObpRemoveFromDirectory ---------------------------------------------- */

int ObpRemoveFromDirectory(void *directory, void *object)
{
    OBJECT_DIRECTORY *dir = (OBJECT_DIRECTORY *)directory;
    OBJECT_DIRECTORY_ENTRY **pp;
    OBJECT_DIRECTORY_ENTRY *entry = NULL;
    uint64_t irqf;

    if (!directory || !object)
        return -1;

    /* Serialize with every other reader + writer on this directory via
     * dir->lock (added in the -9 namespace-locking pass). The lock is
     * IRQ-safe because ob_thread_mark_dead / ob_process_mark_dead can
     * be called from any context that does handle teardown, and the
     * previous bare cli/sti implementation proved that IRQ-level
     * preemption must not reach a partially-unlinked entry. Drop the
     * lock BEFORE ObDereferenceObject -- on_delete callbacks may
     * allocate / take other locks / schedule, none of which are safe
     * while holding a spinlock. */
    spin_lock_irqsave(&dir->lock, &irqf);

    /* Two-pointer removal: track the predecessor's next-field address
     * so unlink is a single pointer write. */
    pp = &dir->first;
    while (*pp) {
        if ((*pp)->object == object) {
            entry = *pp;
            *pp = entry->next;
            dir->count--;
            /* Clear OB_FLAG_NAMED + hdr->name BEFORE freeing the entry.
             * The name string lived inside the entry node; after kfree
             * a stale header->name would dangle. Done under the lock
             * so any reader that is waiting sees a consistent header
             * on the next lookup. */
            {
                OBJECT_HEADER *hdr = OB_HEADER_FROM_BODY(object);
                hdr->flags &= ~OB_FLAG_NAMED;
                hdr->name = (const char *)0;
            }
            break;
        }
        pp = &(*pp)->next;
    }
    spin_unlock_irqrestore(&dir->lock, irqf);

    if (!entry)
        return -1;  /* not found */

    /* Safe to free + deref outside the lock: the entry has been
     * unlinked so no concurrent reader can reach it. */
    kfree(entry);
    ObDereferenceObject(object);
    return 0;
}

/* --- ObInsertObject ------------------------------------------------------ */

int ObInsertObject(void *object, const char *name, void *directory)
{
    OBJECT_DIRECTORY *dir = (OBJECT_DIRECTORY *)directory;
    OBJECT_DIRECTORY_ENTRY *entry;
    size_t name_len;
    uint64_t irqf;

    if (!object || !name || !directory)
        return -1;

    name_len = strlen(name);
    if (name_len == 0 || name_len >= OB_NAME_MAX)
        return -1;

    /* Allocate outside the lock: kmalloc may take the heap spinlock
     * and the heap is a higher-layer resource. We fill the node and
     * commit it atomically under dir->lock. */
    entry = (OBJECT_DIRECTORY_ENTRY *)kmalloc(sizeof(OBJECT_DIRECTORY_ENTRY));
    if (!entry)
        return -1;

    strncpy(entry->name, name, OB_NAME_MAX - 1);
    entry->name[OB_NAME_MAX - 1] = '\0';
    entry->object = object;

    spin_lock_irqsave(&dir->lock, &irqf);

    /* Re-check duplicate + capacity under the lock so a concurrent
     * insert of the same name cannot slip through. */
    if (dir_find(dir, name, name_len) || dir->count >= OB_DIR_MAX_ENTRIES) {
        spin_unlock_irqrestore(&dir->lock, irqf);
        kfree(entry);
        return -1;
    }

    /* Codex -9 [H] fix: take the directory-owned reference BEFORE
     * publishing the entry. Previously we linked into dir->first
     * first, dropped the lock, then called ObReferenceObject -- a
     * window where another CPU could find the object by name and
     * call ObpRemoveFromDirectory (dropping the creation ref, since
     * no dir ref exists yet) leading to a UAF on the delayed
     * ObReferenceObject. ObReferenceObject is an atomic increment,
     * safe to call with the spinlock held. */
    ObReferenceObject(object);

    /* Prepend to linked list + commit the header fields under the lock
     * so any concurrent reader either sees the old state (entry not in
     * list) or the new state (entry linked with OB_FLAG_NAMED set). */
    entry->next = dir->first;
    dir->first = entry;
    dir->count++;

    {
        OBJECT_HEADER *hdr = OB_HEADER_FROM_BODY(object);
        hdr->flags |= OB_FLAG_NAMED;
        hdr->name = entry->name;
    }

    spin_unlock_irqrestore(&dir->lock, irqf);

    return 0;
}

/* --- ObpLookupDirectory -------------------------------------------------- */

/*
 * Codex -9 [H] fix: ObpLookupDirectory now returns a REFERENCED
 * directory. Caller MUST call ObDereferenceObject on the returned
 * body when done. This pins intermediate + final directories across
 * the parent's lock release so a concurrent ObMakeTemporaryObject +
 * ObDereferenceObject on a test-created directory cannot free the
 * body out from under us.
 *
 * Invariant inside the loop: `cur_dir` always owns exactly one
 * reference. We start by ref'ing the root (even though it is
 * PERMANENT, this keeps the invariant uniform). Each advance takes a
 * new reference on the child under the parent's lock, then drops the
 * old `cur_dir` reference after unlock.
 */
static void *obp_lookup_directory_d(const char *path, const char **remaining,
                                    uint32_t *depth)
{
    void *cur_dir = ObpRootDirectory;
    const char *p = path;
    uint64_t irqf;

    if (!cur_dir || !path) {
        if (remaining) *remaining = path;
        return NULL;
    }

    /* Take the initial ref on the root so the invariant holds. */
    ObReferenceObject(cur_dir);

    /* Skip leading backslash */
    if (*p == '\\')
        p++;

    while (*p) {
        const char *seg_start = p;
        size_t seg_len;
        OBJECT_DIRECTORY *dir;
        OBJECT_DIRECTORY_ENTRY *entry;
        /* Captured + ref'd under the lock so the body stays alive
         * across the unlock. We drop the ref at end-of-iteration for
         * non-advanced types (non-dir, non-symlink); for advanced
         * types we transfer the ref into `cur_dir`. */
        void *entry_object = NULL;
        const OBJECT_TYPE *entry_type = NULL;
        char sym_target[OB_SYMLINK_MAX];

        /* Find end of this path component */
        while (*p && *p != '\\')
            p++;
        seg_len = (size_t)(p - seg_start);

        if (seg_len == 0) {
            if (*p == '\\') p++;
            continue;
        }

        dir = (OBJECT_DIRECTORY *)cur_dir;
        spin_lock_irqsave(&dir->lock, &irqf);
        entry = dir_find(dir, seg_start, seg_len);
        if (entry) {
            entry_object = entry->object;
            entry_type   = OB_HEADER_FROM_BODY(entry_object)->type;
            /* Pin the entry object across the unlock. */
            ObReferenceObject(entry_object);
            if (entry_type == ObpSymlinkType) {
                OBJECT_SYMBOLIC_LINK *sl =
                    (OBJECT_SYMBOLIC_LINK *)entry_object;
                strncpy(sym_target, sl->target, OB_SYMLINK_MAX - 1);
                sym_target[OB_SYMLINK_MAX - 1] = '\0';
            }
        }
        spin_unlock_irqrestore(&dir->lock, irqf);

        if (!entry) {
            /* No match -- return current dir (caller owns the ref) */
            if (remaining) *remaining = seg_start;
            return cur_dir;
        }

        /* Follow symlinks only when more path follows this component.
         * If the symlink is the final path component, stop at the parent
         * directory and return remaining = segment name so
         * ObLookupObjectByName can open the symlink object
         * (NtOpenSymbolicLinkObject). */
        if (entry_type == ObpSymlinkType) {
            if (*p == '\0') {
                ObDereferenceObject(entry_object); /* drop symlink ref */
                if (remaining) *remaining = seg_start;
                return cur_dir;
            }
            if (*depth >= OB_SYMLINK_DEPTH) {
                ObDereferenceObject(entry_object);
                if (remaining) *remaining = seg_start;
                return cur_dir;
            }
            (*depth)++;  /* one shared budget across directory + leaf resolution */
            /* Resolve the symlink target from root. MUST be outside
             * the parent dir's lock -- the recursive walk locks other
             * directories, and holding one lock across a recursive
             * acquisition would deadlock. */
            const char *sym_rem = NULL;
            void *target_dir = obp_lookup_directory_d(sym_target, &sym_rem,
                                                      depth);
            ObDereferenceObject(entry_object); /* symlink body done with */
            if (!target_dir) {
                if (remaining) *remaining = seg_start;
                return cur_dir;
            }
            if (sym_rem && *sym_rem) {
                ObDereferenceObject(target_dir);
                if (remaining) *remaining = seg_start;
                return cur_dir;
            }
            /* Transfer ref: drop old cur_dir, adopt target_dir. */
            ObDereferenceObject(cur_dir);
            cur_dir = target_dir;
            if (*p == '\\') p++;
            continue;
        }

        /* Must be a directory to continue walking */
        if (entry_type != ObpDirectoryType) {
            ObDereferenceObject(entry_object); /* drop non-dir ref */
            if (remaining) *remaining = seg_start;
            return cur_dir;
        }

        /* Transfer ref: drop old cur_dir, adopt entry_object. */
        ObDereferenceObject(cur_dir);
        cur_dir = entry_object;

        /* Skip separator */
        if (*p == '\\')
            p++;
    }

    if (remaining) *remaining = p;  /* empty string = fully resolved */
    return cur_dir;
}

void *ObpLookupDirectory(const char *path, const char **remaining)
{
    uint32_t depth = 0;
    return obp_lookup_directory_d(path, remaining, &depth);
}

/* --- ObLookupObjectByName ------------------------------------------------ */

/* Depth-carrying resolver. `depth` is the cumulative symlink-redirect budget
 * shared across BOTH the directory walk (obp_lookup_directory_d) and leaf
 * symlink recursion below, so a symlink cycle (A -> \B\X, B -> \A\X) can never
 * recurse past OB_SYMLINK_DEPTH frames and exhaust the kernel stack. */
static int ob_lookup_by_name_d(const char *path, const OBJECT_TYPE *type,
                               uint32_t access, void **result, uint32_t *depth)
{
    const char *remaining = NULL;
    void *dir;
    OBJECT_DIRECTORY *dobj;
    OBJECT_DIRECTORY_ENTRY *entry;
    OBJECT_HEADER *hdr;
    size_t rem_len;

    (void)access;  /* used by security-check retrofit */

    if (!path || !result)
        return -1;

    *result = NULL;

    /* ObpLookupDirectory returns a REF-OWNED dir; we MUST deref before
     * returning (except when we hand the ref off to the caller, which
     * happens only in the "path resolves to this directory" case). */
    dir = obp_lookup_directory_d(path, &remaining, depth);
    if (!dir)
        return -1;

    /* If fully resolved, the path names a directory itself */
    if (!remaining || !*remaining) {
        hdr = OB_HEADER_FROM_BODY(dir);
        if (type && hdr->type != type) {
            ObDereferenceObject(dir);
            return -1;
        }
        /* Transfer the ref we already own to the caller. */
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
            if (remaining[i] == '\\') {
                ObDereferenceObject(dir);
                return -1;  /* unresolved intermediate component */
            }
        }
    }

    dobj = (OBJECT_DIRECTORY *)dir;
    {
        uint64_t irqf;
        void *entry_object = NULL;
        const OBJECT_TYPE *entry_type = NULL;
        int type_mismatch = 0;
        int is_leaf_symlink = 0;
        char sym_target[OB_SYMLINK_MAX];

        spin_lock_irqsave(&dobj->lock, &irqf);
        entry = dir_find(dobj, remaining, rem_len);
        if (entry) {
            entry_object = entry->object;
            entry_type   = OB_HEADER_FROM_BODY(entry_object)->type;
            if (entry_type == ObpSymlinkType && type != ObpSymlinkType) {
                OBJECT_SYMBOLIC_LINK *sl =
                    (OBJECT_SYMBOLIC_LINK *)entry_object;
                strncpy(sym_target, sl->target, OB_SYMLINK_MAX - 1);
                sym_target[OB_SYMLINK_MAX - 1] = '\0';
                is_leaf_symlink = 1;
            } else if (type && entry_type != type) {
                type_mismatch = 1;
            } else {
                /* Ref under the lock so the body we hand back is
                 * guaranteed alive until the caller derefs. */
                ObReferenceObject(entry_object);
            }
        }
        spin_unlock_irqrestore(&dobj->lock, irqf);

        ObDereferenceObject(dir);  /* drop the owned parent-dir ref */

        if (!entry || type_mismatch)
            return -1;
        if (is_leaf_symlink) {
            /* Bound leaf-symlink chains by the SAME cumulative budget as the
             * directory walk (one shared *depth counter) so a self-referential
             * or cyclic leaf symlink cannot recurse the kernel stack and the
             * total redirect count across both phases stays <= OB_SYMLINK_DEPTH. */
            if (*depth >= OB_SYMLINK_DEPTH)
                return -1;
            (*depth)++;
            return ob_lookup_by_name_d(sym_target, type, access, result, depth);
        }
        *result = entry_object;
        return 0;
    }
}

int ObLookupObjectByName(const char *path, const OBJECT_TYPE *type,
                         uint32_t access, void **result)
{
    uint32_t depth = 0;
    return ob_lookup_by_name_d(path, type, access, result, &depth);
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
    /* \ObjectManager -- root for kernel-metadata pseudo-files (info
     * files registered via ob_info_file_register). Created here at
     * namespace init time so the first info-file register is pure
     * insert + no dir-allocate; also keeps it out of any test-phase
     * leak-tracking window. */
    (void)ns_mkdir(ObpRootDirectory, "ObjectManager");

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
