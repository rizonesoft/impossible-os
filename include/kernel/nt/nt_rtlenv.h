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

/* Per-PASS ceiling on block-inspection work, counted in WCHARs examined by the
 * name lookup. Bounds the O(refs * block) product that makes an expansion
 * expensive: rtl_env_block_lookup walks the whole block on a MISS, and a
 * 32767-WCHAR Source holds up to 10922 `%V%` references, so a near-cap block
 * costs ~1.1e10 comparisons per pass (seconds of kernel time) with no bound at
 * all. At ~1e9 simple comparisons/sec this ceiling caps one pass near ~8 ms;
 * exceeding it is STATUS_INSUFFICIENT_RESOURCES -- a resource-policy refusal,
 * deliberately distinct from STATUS_INVALID_PARAMETER (malformed input).
 *
 * The budget is per PASS and each pass starts from a FRESH copy, NOT one counter
 * shared across both: a shared counter would let pass 1 consume most of it and
 * pass 2 trip on identical work, making the outcome depend on which pass you are
 * in rather than on the inputs. Fresh-per-pass keeps the verdict a pure function
 * of (block, Source) -- if pass 1 fits, pass 2 does the same work and also fits
 * -- so a budget refusal can never land mid-write. The two passes' consumed work
 * is compared afterwards, which also makes this a concurrent-mutation detector
 * alongside the existing written-vs-required check (TODO-22 s20).
 *
 * WHAT THIS INTENTIONALLY REFUSES: against a near-cap (1 MiWCHAR) block a
 * legitimate expansion gets ~8 full-block misses. Real environments are a few
 * KiB, where the same ceiling allows thousands, so only pathological
 * block/reference combinations are rejected. The VALUE is a reasoned ceiling,
 * not a measured one; deriving it from worst-case bare-metal timing is owned by
 * TODO-22 s21 (item: "Measure RTL_ENV_EXPAND_WORK_MAX"). */
#define RTL_ENV_EXPAND_WORK_MAX    8388608u

/* Expand `%VAR%` references in `Source` using `Environment` (a double-NUL-
 * terminated UTF-16 "NAME=VALUE\0"... block), writing the expanded UTF-16 result
 * into `Destination->Buffer`.
 *
 *   Environment    MUST be NULL: the calling process's own environment, which is
 *                  synthesized from task_current()'s environ (a kernel-resident
 *                  block whose exact extent is known). A NON-NULL Environment is
 *                  refused with STATUS_NOT_SUPPORTED -- see the gate below.
 *
 * NON-NULL Environment IS A DELIBERATE, TEMPORARY ABI DIVERGENCE (TODO-22 s20).
 * ntdll documents the explicit-block form, and this entry accepted it until the
 * s19 review showed why it cannot be done safely through this signature: a bare
 * `void *` carries no allocation EXTENT, so the double-NUL scan had to trust the
 * caller's terminator and would read up to RTL_ENV_BLOCK_MAX_WCHARS (2 MiB) past
 * a short or unterminated allocation. No amount of capping fixes that -- a cap
 * bounds the runaway but still reads adjacent allocations, and probing mapped
 * pages proves nothing about the C object's length. The ABI-preserving safe
 * shape is a BOUNDARY that probes + copies the caller's block into a terminated
 * kernel snapshot (whose extent it then knows) and calls rtl_env_expand_block
 * below; that boundary is owned by TODO-22 s21 (item: "Boundary probe+copy for a
 * non-NULL Environment"). Until it exists the form is refused explicitly rather
 * than served unsafely, and the symbol MUST NOT be described as
 * compatibility-complete when it is exported. Nothing is user-reachable today
 * (no SSDT row, no kernel32 caller), so this refusal takes no capability away
 * from any existing caller: every in-kernel caller and test uses the
 * extent-taking rtl_env_expand_block entry instead.
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
 * STATUS_UNSUCCESSFUL, STATUS_INSUFFICIENT_RESOURCES, STATUS_NO_MEMORY, or
 * STATUS_NOT_SUPPORTED (non-NULL Environment). */
NTSTATUS RtlExpandEnvironmentStrings_U(void *Environment, UNICODE_STRING *Source,
                                       UNICODE_STRING *Destination,
                                       uint32_t *ReturnedLength);

/* The expansion engine, taking the one fact the ntdll ABI above cannot express:
 * `block_extent` -- the number of WCHARs the caller GUARANTEES are readable at
 * `block`. Every in-kernel caller either built the block itself (and knows its
 * length) or owns a fixed array, so the extent is always available on this side;
 * only the public `void *` form lacks it. The scan is bounded by
 * min(block_extent, RTL_ENV_BLOCK_MAX_WCHARS), so a missing terminator can never
 * read past the allocation -- it fails STATUS_INVALID_PARAMETER instead.
 *
 * `block` MUST be kernel-resident, immutable for the call, and a well-formed
 * double-NUL-terminated block within `block_extent`; `block_extent` MUST be >= 2
 * (the smallest legal block is the empty "\0\0" form, which is validated as a
 * genuine double NUL rather than assumed -- TODO-22 s20). Source / Destination /
 * ReturnedLength semantics, the no-alias matrix, the two-pass agreement check,
 * and the full status set are exactly as documented for
 * RtlExpandEnvironmentStrings_U above, plus STATUS_INSUFFICIENT_RESOURCES when a
 * pass exceeds RTL_ENV_EXPAND_WORK_MAX lookup work. */
NTSTATUS rtl_env_expand_block(const uint16_t *block, uint32_t block_extent,
                              UNICODE_STRING *Source, UNICODE_STRING *Destination,
                              uint32_t *ReturnedLength);

/* As rtl_env_expand_block, but with the per-pass work ceiling as an explicit
 * POLICY input instead of RTL_ENV_EXPAND_WORK_MAX. The budget is a resource
 * choice, not a correctness constant: a caller handling especially untrusted
 * input may want a tighter one, and the unit tests need to reach the
 * fit/overshoot boundary with small synthetic counts rather than burning the
 * production ceiling (~8.4e6 inspections) per case. Identical semantics
 * otherwise; `budget` of RTL_ENV_EXPAND_WORK_MAX is exactly rtl_env_expand_block.
 *
 * A budget below RTL_ENV_BLOCK_MAX_WCHARS can refuse a single legitimate
 * reference against a large block -- that is the floor the RTL_ENV_EXPAND_WORK_MAX
 * _Static_assert pins for the default, and a caller passing less owns that
 * tradeoff deliberately. */
NTSTATUS rtl_env_expand_block_budget(const uint16_t *block, uint32_t block_extent,
                                     UNICODE_STRING *Source,
                                     UNICODE_STRING *Destination,
                                     uint32_t *ReturnedLength, uint64_t budget);

struct task;

/* Win32 userenv.dll ExpandEnvironmentStringsForUserW(HANDLE hToken, LPCWSTR lpSrc,
 * LPWSTR lpDst, DWORD dwSize) -- expand `%VAR%` in `Source` against the environment
 * of the user identified by `htoken`, writing the UTF-16 result into `Destination`.
 *
 *   htoken == NULL: expand against `caller`'s current process environment (the
 *                   SMP-safe authoritative store). This builds a bounded block from
 *                   caller->environ and runs it through the expansion ENGINE with
 *                   the extent the builder reported -- NOT through
 *                   RtlExpandEnvironmentStrings_U, whose public ABI refuses a
 *                   non-NULL Environment. All the aliasing / two-pass / cap /
 *                   work-budget guarantees apply unchanged; they live in the
 *                   engine, which both entries share. The strict Win32
 *                   "system variables only" subset
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

