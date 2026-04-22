/* ============================================================================
 * ob_ns.h -- Object namespace: directories and symbolic links
 *
 * Provides a hierarchical in-memory namespace rooted at `\`.
 * Directories hold named entries; symbolic links redirect lookups.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* Forward declarations -- full defs need ob.h which includes us */
struct object_header;
struct object_type;

/* --- Namespace limits ---------------------------------------------------- */

#define OB_NAME_MAX        64   /* max chars per component name */
#define OB_PATH_MAX       256   /* max total path length */
#define OB_DIR_MAX_ENTRIES 128  /* max entries per directory */
#define OB_SYMLINK_MAX    256   /* max symlink target length */
#define OB_SYMLINK_DEPTH    8   /* max symlink redirections per lookup */

/* --- OBJECT_DIRECTORY_ENTRY ---------------------------------------------- */

typedef struct object_directory_entry {
    char  name[OB_NAME_MAX];              /* component name */
    void *object;                         /* body pointer (NULL = free) */
    struct object_directory_entry *next;   /* linked list next */
} OBJECT_DIRECTORY_ENTRY;

/* --- OBJECT_DIRECTORY body ----------------------------------------------- */

typedef struct object_directory {
    OBJECT_DIRECTORY_ENTRY *first;   /* head of entry linked list */
    uint32_t                count;   /* number of entries */
    void                   *parent;  /* parent directory body (NULL for root) */
} OBJECT_DIRECTORY;

/* --- OBJECT_SYMBOLIC_LINK body ------------------------------------------- */

typedef struct object_symbolic_link {
    char target[OB_SYMLINK_MAX];   /* target path string */
} OBJECT_SYMBOLIC_LINK;

/* --- Root namespace ------------------------------------------------------ */

/* The global root directory `\` -- set during ob_init */
extern void *ObpRootDirectory;

/* --- Namespace API ------------------------------------------------------- */

/*
 * ObpLookupDirectory -- walk a path and find the deepest matching directory.
 *
 * Starting from root `\`, splits on `\` and descends through directories.
 * On return, *remaining points to the unresolved tail of the path.
 * Returns the directory body pointer, or NULL on failure.
 */
void *ObpLookupDirectory(const char *path, const char **remaining);

/*
 * ObInsertObject -- insert a named object into a directory.
 *
 * Sets OB_FLAG_NAMED on the object's header and stores the name pointer.
 * Returns 0 on success, -1 if the name already exists or the directory is full.
 */
int ObInsertObject(void *object, const char *name, void *directory);

/*
 * ObpRemoveFromDirectory -- reverse of ObInsertObject. Removes `object` from
 * `directory`'s entry list, frees the directory-entry node, clears
 * OB_FLAG_NAMED on the header, and drops the directory's reference on the
 * object. After the deref, if the object's ref count hits zero AND the
 * PERMANENT flag has been cleared (via ObMakeTemporaryObject), the object
 * is destroyed and its body freed -- this is the call that closes the
 * "dir holds a ref forever" leak class TODO-03 -9 surfaced.
 *
 * Returns 0 on success, -1 if `object` is not in `directory` (caller bug).
 * Narrow-scope helper: only the subsystem-specific mark-dead paths
 * (ob_thread_mark_dead, ob_process_mark_dead, ...) should call this.
 * General OB consumers should go through NtClose / ObMakeTemporaryObject.
 */
int ObpRemoveFromDirectory(void *directory, void *object);

/*
 * ObLookupObjectByName -- resolve a full path to an object.
 *
 * Walks the namespace, follows symlinks, calls type->on_parse for
 * namespace-extending objects.  Calls ObReferenceObject on the result.
 * Returns 0 on success (object stored in *result), -1 on failure.
 */
int ObLookupObjectByName(const char *path, const OBJECT_TYPE *type,
                         uint32_t access, void **result);

/*
 * ob_ns_create_directory -- allocate and return a new directory object body.
 *
 * The directory is permanent and named.  Caller should insert it
 * into a parent directory with ObInsertObject.
 */
void *ob_ns_create_directory(void *parent);

/*
 * ob_ns_create_symlink -- allocate a symbolic link object body.
 *
 * Target is copied into the body.  Caller should insert it with ObInsertObject.
 */
void *ob_ns_create_symlink(const char *target);

/*
 * ob_ns_init -- create the root namespace tree.
 *
 * Called from ob_init after directory and symlink types are registered.
 */
void ob_ns_init(void);
