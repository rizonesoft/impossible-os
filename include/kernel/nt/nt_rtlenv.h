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
 * kernel walk/allocation. 1 MiWCHAR = 2 MiB, sized to cover a full storage-layer
 * ENV_BLOCK_MAX (1 MiB of UTF-8 -> at most 1 MiWCHAR, hit when every stored byte
 * is ASCII), so any environ the store accepts is expandable through the
 * NULL-Environment form. The cap is a CEILING, not an allocation size: the NULL
 * path allocates only what the environ actually needs (env_build_block_utf16
 * returns ENV_ERR_NOSPACE past the cap -> STATUS_INVALID_PARAMETER), so a typical
 * few-KiB environ still costs a few KiB.
 *
 * This was 64 KiWCHAR until TODO-22 s19 because rtl_env_expand_pass counted its
 * output in a uint32 and a larger block could wrap that accumulator at 2^32
 * WCHARs. The pass now counts in a SATURATING uint64 and the public entry rejects
 * a saturated count before any narrowing, so the wrap that forced the smaller cap
 * is gone. */
#define RTL_ENV_BLOCK_MAX_WCHARS   1048576u

/* Largest content length (in WCHARs, excluding the NUL) an expansion can report
 * through a UNICODE_STRING. Length/MaximumLength are USHORT BYTE counts, so a
 * NUL-terminated result needs (n + 1) * 2 <= 65535 -> n <= 32766. A requirement
 * above this is UNSATISFIABLE at any buffer size (the caller cannot express a
 * large enough MaximumLength), so the entry reports STATUS_UNSUCCESSFUL rather
 * than STATUS_BUFFER_TOO_SMALL: a too-small status would send a grow-and-retry
 * caller into an unbounded loop. Real ntdll guards identically (its
 * UNICODE_STRING_MAX_CHARS check returns STATUS_UNSUCCESSFUL). */
#define RTL_ENV_MAX_RESULT_WCHARS  32766u

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
 * Destination->Length is the result length in bytes excluding the NUL.
 *
 * A result longer than RTL_ENV_MAX_RESULT_WCHARS returns STATUS_UNSUCCESSFUL and
 * sets ReturnedLength to 0: such a length is unrepresentable in a UNICODE_STRING
 * at any buffer size, so reporting BUFFER_TOO_SMALL would loop a grow-and-retry
 * caller forever. 0 (not the real size) is published so no truncated length ever
 * reaches a caller, matching real ntdll, which zeroes its ResultLength on this
 * branch and stores it unconditionally (TODO-22 s19).
 *
 * Returns STATUS_SUCCESS, STATUS_BUFFER_TOO_SMALL, STATUS_INVALID_PARAMETER,
 * STATUS_UNSUCCESSFUL, or STATUS_NO_MEMORY. */
NTSTATUS RtlExpandEnvironmentStrings_U(void *Environment, UNICODE_STRING *Source,
                                       UNICODE_STRING *Destination,
                                       uint32_t *ReturnedLength);

struct task;

/* Win32 userenv.dll ExpandEnvironmentStringsForUserW(HANDLE hToken, LPCWSTR lpSrc,
 * LPWSTR lpDst, DWORD dwSize) -- expand `%VAR%` in `Source` against the environment
 * of the user identified by `htoken`, writing the UTF-16 result into `Destination`.
 *
 *   htoken == NULL: expand against `caller`'s current process environment (the
 *                   SMP-safe authoritative store). This builds a bounded block from
 *                   caller->environ and runs it through RtlExpandEnvironmentStrings_U,
 *                   so ALL the aliasing / two-pass / cap guarantees of that path
 *                   apply unchanged. The strict Win32 "system variables only" subset
 *                   for a NULL token needs an SMP-safe Registry snapshot and is
 *                   deferred with CreateEnvironmentBlock (env.h) -- documented, not
 *                   fabricated.
 *   htoken != NULL: a per-user expansion needs a token-SID -> HKU-hive map +
 *                   LoadUserProfile (see env_create_block); refused with
 *                   STATUS_NOT_SUPPORTED rather than expanding against the wrong
 *                   identity.
 *
 * `htoken` is an ACCESS_TOKEN* (typed void* to keep this header free of the
 * security headers); only its NULL-ness is consulted today. `caller` must be
 * non-NULL for the supported path. An environ larger than RTL_ENV_BLOCK_MAX_WCHARS
 * fails STATUS_INVALID_PARAMETER (an over-cap INTERNAL block is not a caller-buffer
 * problem -- mapped exactly as the RtlExpandEnvironmentStrings_U NULL-Environment
 * path maps its own over-cap build failure, NOT a retryable STATUS_BUFFER_TOO_SMALL).
 * Returns the RtlExpandEnvironmentStrings_U status set, plus STATUS_NOT_SUPPORTED
 * (per-user token) and STATUS_INVALID_PARAMETER (NULL caller). Thread context only. */
NTSTATUS ExpandEnvironmentStringsForUser(struct task *caller, const void *htoken,
                                         UNICODE_STRING *Source,
                                         UNICODE_STRING *Destination,
                                         uint32_t *ReturnedLength);

