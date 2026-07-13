/* ============================================================================
 * nt_rtlenv.h -- ntdll Rtl environment layer (UTF-16 %VAR% expansion)
 *
 * The public NT-ABI entry RtlExpandEnvironmentStrings_U expands `%VAR%`
 * references in a UTF-16 Source string using a UTF-16 environment block, writing
 * the result into a caller UNICODE_STRING. It is the UTF-16 counterpart of the
 * UTF-8 env_expand() (kernel/env.h) and is used by ExpandEnvironmentStringsW
 * (Win32 wrapper) and the shell's Win32-mode argument expansion.
 *
 * KERNEL-RESIDENT INPUT CONTRACT: the internal transformer operates ONLY on
 * kernel-resident buffers with explicit, bounded lengths -- it never scans user
 * memory to an unbounded terminator. Any caller holding a user-space Environment
 * block (e.g. a process's PEB block, mapped VMM_USER_RW and mutable by a sibling
 * thread) MUST probe + copy it into a bounded kernel snapshot FIRST; that
 * boundary work is owned by the Nt/Win32 environment syscall wrappers (the
 * NtQueryEnvironmentVariable and ExpandEnvironmentStringsW work), mirroring the
 * nt_rtlstr contract. RtlExpandEnvironmentStrings_U honors a NULL Environment
 * (the documented "calling process's own block" form) by synthesizing a bounded
 * block from the current task's UTF-8 environ -- the authoritative kernel-
 * resident store -- not by dereferencing the user PEB.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/nt/ntstatus.h"
#include "kernel/ob/peb.h"        /* UNICODE_STRING */

/* Hard cap on the environment block a single expansion will consider, in
 * WCHARs (includes inter-entry NULs + terminators). Bounds both the double-NUL
 * scan of a supplied block and the size of a synthesized current-process block,
 * so a stripped terminator or a pathological environ cannot drive an unbounded
 * kernel walk/allocation. 64 KiWCHAR = 128 KiB, far above any real env block
 * (the PEB env region is a single 4 KiB page). */
#define RTL_ENV_BLOCK_MAX_WCHARS   65536u

/* Expand `%VAR%` references in `Source` using `Environment` (a double-NUL-
 * terminated UTF-16 "NAME=VALUE\0"... block), writing the expanded UTF-16 result
 * into `Destination->Buffer`.
 *
 *   Environment    kernel-resident UTF-16 env block, or NULL to use the calling
 *                  process's own environment (synthesized from task_current()'s
 *                  environ). A non-NULL block MUST be a well-formed double-NUL-
 *                  terminated block (as ntdll requires): RTL_ENV_BLOCK_MAX_WCHARS
 *                  only backstops a runaway scan and does NOT substitute for a
 *                  valid terminator, so the boundary that copies a user block
 *                  into a kernel snapshot must guarantee the terminator lies
 *                  within the copied length. A block with no terminator inside
 *                  the cap fails STATUS_INVALID_PARAMETER. A block that aliases
 *                  the Destination range is rejected (STATUS_INVALID_PARAMETER).
 *   Source         UNICODE_STRING with the template (Length is an even byte count).
 *   Destination    UNICODE_STRING output buffer; MaximumLength bounds the write.
 *   ReturnedLength  optional; set to the required buffer size in BYTES including
 *                  the WCHAR NUL (both on success and on STATUS_BUFFER_TOO_SMALL).
 *
 * Semantics match env_expand: single-pass, `%%` PRESERVED verbatim (empty name =
 * unresolved var, Win32/ntdll behavior -- not cmd.exe's `%%`->`%` escape), a
 * `%NAME%` whose name is empty, over ENV_NAME_MAX WCHARs, or absent from the
 * block is copied verbatim, and an unmatched trailing `%` is copied verbatim.
 * Name match is case-insensitive ASCII fold (consistent with the UTF-8 storage
 * layer; full Unicode case folding for env names is a unified NLS concern).
 *
 * NO-ALIAS + IMMUTABLE-INPUT CONTRACT: the output range [Destination->Buffer,
 * +MaximumLength) MUST NOT overlap the Source data, the Environment block, the
 * Source or Destination UNICODE_STRING descriptors, or the ReturnedLength cell;
 * ReturnedLength MUST NOT overlap the Source data, Environment, or either
 * descriptor. Any such overlap returns STATUS_INVALID_PARAMETER (in-place use is
 * NOT supported). Source and the Environment block MUST remain unmodified for the
 * duration of the call: the function expands in two passes over the SAME inputs
 * and rejects (STATUS_INVALID_PARAMETER) if the two passes disagree, but an
 * equal-length concurrent mutation cannot be detected -- callers pass a private
 * per-call snapshot.
 *
 * On STATUS_BUFFER_TOO_SMALL no partial output is written and Destination->Length
 * is left unchanged. On success Destination->Buffer is NUL-terminated and
 * Destination->Length is the result length in bytes excluding the NUL. Returns
 * STATUS_SUCCESS, STATUS_BUFFER_TOO_SMALL, STATUS_INVALID_PARAMETER, or
 * STATUS_NO_MEMORY. */
NTSTATUS RtlExpandEnvironmentStrings_U(void *Environment, UNICODE_STRING *Source,
                                       UNICODE_STRING *Destination,
                                       uint32_t *ReturnedLength);
