/* ============================================================================
 * assign_security.h -- SeAssignSecurity: security-descriptor inheritance
 *
 * When a new object (file, directory, registry key) is created without an
 * explicit security descriptor, the SRM builds one by inheriting ACEs from the
 * parent container's DACL. This is the kernel-side inheritance engine.
 *
 * Interim contract: the subject is passed as an ACCESS_TOKEN* directly rather
 * than a SECURITY_SUBJECT_CONTEXT (that type is owned by the deferred
 * SeAccessCheck engine work); this matches the mic.c / privileges.c interim
 * convention. Owner/Group/DACL fallback defaults come from the token.
 *
 * ParentSD/CreatorSD/token DACLs MUST be TRUSTED owned kernel descriptors (their
 * AclSize is used as the readable extent). An imported/untrusted SD must be
 * bounded by RtlSelfRelativeToAbsoluteSD(known_len) at the syscall boundary
 * (NtSetSecurityObject) before reaching this function.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/nt/ntstatus.h"
#include "kernel/security/acl.h"

struct access_token;

/*
 * SeAssignSecurity -- build an inherited SECURITY_DESCRIPTOR for a new object.
 *
 * DACL source, in priority order:
 *   1. CreatorSD's explicit DACL (SE_DACL_PRESENT set, SE_DACL_DEFAULTED clear)
 *      -- copied verbatim (an explicit NULL DACL is honored as grant-all).
 *   2. else, unless the child is SE_DACL_PROTECTED: ACEs inherited from
 *      ParentSD's DACL per the container/object-inherit rules below.
 *   3. else, the creator token's DefaultDacl (if non-NULL).
 *   4. else a fail-closed creator/SYSTEM/World-read default (never a NULL DACL).
 *
 * Inheritance (per parent ACE, ACCESS_ALLOWED/DENIED types only):
 *   - IsDirectory == 0 (file child): inherit ACEs flagged OBJECT_INHERIT_ACE;
 *     the child copy carries only INHERITED_ACE (leaves do not propagate).
 *   - IsDirectory != 0 (directory child): CONTAINER_INHERIT_ACE ACEs apply to
 *     the directory and keep their propagation flags (unless NO_PROPAGATE);
 *     OBJECT_INHERIT-only ACEs are copied as INHERITED_ACE|INHERIT_ONLY_ACE so
 *     they propagate to file grandchildren without granting on the directory.
 *   - NO_PROPAGATE_INHERIT_ACE strips the propagation flags on the child copy.
 *   - Inherited deny ACEs are emitted before allow ACEs (canonical order).
 *
 * Owner  = CreatorSD->Owner if set, else CreatorToken->UserSid (required).
 * Group  = CreatorSD->Group if set, else CreatorToken->PrimaryGroup (optional).
 *
 * *NewSD receives a single owned allocation containing the descriptor, its DACL,
 * and copied Owner/Group SIDs; free it with SeDeassignSecurity. Returns
 * STATUS_SUCCESS; STATUS_INVALID_PARAMETER (no owner resolvable, malformed SID,
 * malformed explicit DACL, or an unsupported inheritable conditional ACE);
 * STATUS_INSUFFICIENT_RESOURCES (descriptor would exceed one page); or
 * STATUS_NO_MEMORY (the backing allocation failed).
 */
NTSTATUS SeAssignSecurity(const SECURITY_DESCRIPTOR *ParentSD,
                          const SECURITY_DESCRIPTOR *CreatorSD,
                          int IsDirectory,
                          struct access_token *CreatorToken,
                          SECURITY_DESCRIPTOR **NewSD);

/* Free a descriptor returned by SeAssignSecurity and NULL the caller pointer. */
void SeDeassignSecurity(SECURITY_DESCRIPTOR **NewSD);
