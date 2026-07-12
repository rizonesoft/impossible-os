#ifndef KERNEL_NT_PLEDGE_H
#define KERNEL_NT_PLEDGE_H

/* ============================================================================
 * pledge.h -- OpenBSD-style process self-restriction (pledge + unveil)
 *
 * pledge(): a process voluntarily and IRREVERSIBLY reduces the set of syscall
 * CATEGORIES it may invoke. unveil(): the process reduces the filesystem
 * subtree it may reach. Both are "tighten-only" -- once applied, a restriction
 * can be narrowed but never widened, for the life of the process (and its
 * fork children, which inherit fail-closed).
 *
 * Design contract:
 *   - pledge_mask stores ALLOWED category bits. A new pledge INTERSECTS with
 *     the current mask (CAS loop), never ORs -- OR would EXPAND privilege. The
 *     PLEDGE_PLEDGED sentinel distinguishes "empty pledge" (mask = sentinel
 *     only, all categories denied) from "never pledged" (mask == 0).
 *   - Unclassified/omitted syscalls DENY when pledged (fail-closed allowlist),
 *     so a syscall accidentally missing from the classifier over-restricts a
 *     pledged process (kills it) rather than silently permitting it.
 *   - A violation TERMINATES the process, but never from inside a syscall
 *     handler mid-audit-session: the coarse dispatcher check terminates BEFORE
 *     nt_audit_begin; the fine handler checks only RETURN STATUS_PLEDGE_VIOLATION
 *     and the dispatcher terminates AFTER nt_audit_end has run.
 *   - unveil paths are stored FOLDED (case-insensitive, vfs_path_fold) and
 *     matched on a component boundary so C:\AllowedEvil is not a child of
 *     C:\Allowed. unveil DENIES access (STATUS_ACCESS_DENIED); it does not
 *     terminate.
 * ============================================================================ */

#include "kernel/types.h"
#include "kernel/nt/ntstatus.h"
#include "kernel/fs/vfs.h"        /* VFS_MAX_PATH, VFS_O_* */

struct task;

/* ---- Pledge categories (allowed-operation bits) ------------------------- */
#define PLEDGE_STDIO      (1ull << 0)   /* read/write/close on open handles */
#define PLEDGE_RPATH      (1ull << 1)   /* open files/dirs for read */
#define PLEDGE_WPATH      (1ull << 2)   /* open files for write/truncate */
#define PLEDGE_CPATH      (1ull << 3)   /* create/delete files */
#define PLEDGE_INET       (1ull << 4)   /* network sockets */
#define PLEDGE_PROC       (1ull << 5)   /* fork / create process or thread */
#define PLEDGE_EXEC       (1ull << 6)   /* exec a new image */
#define PLEDGE_DNS        (1ull << 7)   /* DNS resolution */
#define PLEDGE_TTY        (1ull << 8)   /* terminal I/O */
#define PLEDGE_CATEGORY_MASK  0x1FFull  /* bits 0-8 */

/* Sentinel bit set whenever the process has pledged at all (even an empty
 * promise) so pledge_mask != 0 distinguishes pledged-empty from never-pledged. */
#define PLEDGE_PLEDGED    (1ull << 63)

/* Classifier verdicts (pledge_syscall_category return). A concrete category
 * mask (subset of PLEDGE_CATEGORY_MASK) means "allowed iff (pledge_mask &
 * required) != 0". These two sentinels are distinct from any category bit. */
#define PLEDGE_REQ_CORE   0ull            /* always allowed (survival syscalls) */
#define PLEDGE_REQ_DENY   (1ull << 62)    /* unclassified / forbidden -> violation */

/* ---- Unveil permission bits (subset of "rwxc") -------------------------- */
#define UNVEIL_R          0x1u
#define UNVEIL_W          0x2u
#define UNVEIL_X          0x4u
#define UNVEIL_C          0x8u
#define UNVEIL_PERM_MASK  0xFu

/* One unveiled path prefix. `path` is stored FOLDED (vfs_path_fold), absolute,
 * separator-normalized, without a trailing '\' (except the "X:\" root). */
typedef struct unveil_entry {
    struct unveil_entry *next;
    uint8_t  perms;                  /* UNVEIL_* bits granted on this subtree */
    char     path[VFS_MAX_PATH];     /* folded canonical prefix */
} unveil_entry_t;

/* ---- Pure decision core (no task/global state; unit-tested) -------------- */

/* Parse a space-separated promise string ("stdio rpath ...") into a category
 * mask (WITHOUT the sentinel). Returns 0 on success, -1 on an unknown token.
 * An empty/whitespace-only string yields mask 0 (a valid empty pledge). */
int pledge_parse(const char *promises, uint64_t *out_mask);

/* Classify a main-table SSDT service number: PLEDGE_REQ_CORE (always allow),
 * PLEDGE_REQ_DENY (unclassified -> violation), or a category mask to test
 * against the process's pledge_mask. File-OPEN syscalls return PLEDGE_REQ_CORE
 * here because their rpath/wpath/cpath split needs the ACCESS_MASK, enforced by
 * the fine check in the handler. */
uint64_t pledge_syscall_category(uint32_t service_number);

/* Map decoded VFS open flags to the pledge category bits an open requires. */
uint64_t pledge_file_categories(uint32_t vfs_flags);

/* Decision: is a syscall requiring `required` permitted under `mask`?
 * 1 = allowed, 0 = denied. mask includes the PLEDGE_PLEDGED sentinel. */
int pledge_is_allowed(uint64_t mask, uint64_t required);

/* Compute the mask resulting from applying `requested` to `current` (the
 * tighten-only intersection). Pure; used by pledge_apply and the unit tests. */
uint64_t pledge_tighten(uint64_t current, uint64_t requested);

/* Pure unveil match: does the folded `path` fall under an entry that grants
 * every bit in `want`? 1 = allowed, 0 = denied. A NULL list allows all. */
int unveil_list_covers(const unveil_entry_t *list, const char *folded_path,
                       uint8_t want);

/* Map decoded VFS open flags to the unveil permission bits an open requires. */
uint8_t unveil_perms_for_vfs(uint32_t vfs_flags);

/* ---- Live (task-aware) API ---------------------------------------------- */

/* Apply a pledge to task `t` (tighten-only). Sets the PLEDGE_PLEDGED sentinel.
 * Always succeeds (returns STATUS_SUCCESS); a widening request is silently
 * narrowed to the current mask. SMP-safe via a CAS loop on pledge_mask. */
NTSTATUS pledge_apply(struct task *t, uint64_t requested);

/* 1 if the current syscall entered from user mode (ssdt_previous_mode() ==
 * SSDT_USER_MODE). Fine handler-side pledge/unveil checks gate on this so a
 * kernel-internal Zw call is never restricted, matching the coarse gate. */
int pledge_user_mode(void);

/* Coarse dispatch-time pledge check for the current task. Called from
 * ssdt_dispatch with the already-decoded coordinates. Returns STATUS_SUCCESS
 * (allowed) or STATUS_PLEDGE_VIOLATION. Does NOT terminate -- the caller
 * terminates at a safe point. Reads pledge_mask with an acquire load. */
NTSTATUS pledge_check_syscall(struct task *t, uint32_t table_id,
                              uint32_t index, uint32_t service_number);

/* Coarse pledge check for a legacy INT 0x80 SYS_* call by the current task.
 * The legacy dispatcher is a distinct ring-3 entry path, so pledge must gate it
 * too (the SSDT check alone would be bypassable via INT 0x80). STATUS_SUCCESS or
 * STATUS_PLEDGE_VIOLATION; does NOT terminate. */
NTSTATUS pledge_check_legacy(struct task *t, uint64_t sys_nr);

/* Fine pledge check for a file open/create by the current task. `vfs_flags`
 * are the decoded VFS_O_* flags. STATUS_SUCCESS or STATUS_PLEDGE_VIOLATION.
 * Does NOT terminate. A NULL/unpledged task returns SUCCESS. */
NTSTATUS pledge_check_file(struct task *t, uint32_t vfs_flags);

/* Fine pledge check for an NtSetInformationFile class: rename/dispose require
 * cpath, truncate/allocate/set-attributes require wpath, others are benign.
 * STATUS_SUCCESS or STATUS_PLEDGE_VIOLATION. Does NOT terminate. */
NTSTATUS pledge_check_setinfo(struct task *t, uint32_t info_class);

/* Terminate the CURRENT process for a pledge violation. Logs the offending
 * service number, then task_exit(). NORETURN. Call only at a point with no
 * in-flight audit session (pre-audit coarse check, or post-handler tail). */
void pledge_terminate(uint32_t service_number) __attribute__((noreturn));

/* ---- Unveil live API ---------------------------------------------------- */

/* Add or update an unveil entry for task `t`. `canon_path` is an absolute,
 * separator-normalized path (this function folds it). If the set is already
 * locked, returns STATUS_ACCESS_DENIED. Re-unveiling an existing path can only
 * NARROW its perms (tighten-only intersect) -- a dropped w/x/c bit is never
 * restored. STATUS_INSUFFICIENT_RESOURCES if the entry allocation fails. */
NTSTATUS unveil_add(struct task *t, const char *canon_path, uint8_t perms);

/* Lock task `t`'s unveil set: no further unveil_add calls succeed. Idempotent.
 * A first unveil call already restricts; NtUnveil(NULL,NULL) calls this. */
void unveil_lock(struct task *t);

/* Access check for task `t` against a resolved absolute path. `want` is the
 * required UNVEIL_* bits. STATUS_SUCCESS if the task has no unveil set (allow
 * all) or the path is covered with sufficient perms; STATUS_ACCESS_DENIED
 * otherwise. Folds the path internally. */
NTSTATUS unveil_check(struct task *t, const char *resolved_path, uint8_t want);

/* ---- Lifecycle ---------------------------------------------------------- */

/* Inherit pledge_mask + a deep copy of the unveil set from `parent` into
 * `child`, BEFORE the child is published to the scheduler. Returns 0 on
 * success, -1 if cloning the unveil list ran out of memory (the caller MUST
 * fail child creation closed). Safe to call with an unrestricted parent. */
int pledge_unveil_inherit(struct task *child, struct task *parent);

/* Free the unveil list of a dead task at the reap barrier (task_cleanup).
 * Idempotent; safe on an unrestricted task. */
void pledge_unveil_teardown(struct task *t);

/* Register NtPledge / NtUnveil in the SSDT. Called from the NT registration
 * block at boot. */
void pledge_register_ssdt(void);

#endif /* KERNEL_NT_PLEDGE_H */
