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
 * TODO-22 s24 (item: "Measure RTL_ENV_EXPAND_WORK_MAX"). */
#define RTL_ENV_EXPAND_WORK_MAX    8388608u

/* Resource-policy ceiling on the COUNTED RtlExpandEnvironmentStrings Source
 * template, in WCHARs (see the counted entry below). Native ntdll takes an
 * unbounded SIZE_T SourceLength; a kernel cannot, because the outer source scan
 * is NOT charged against RTL_ENV_EXPAND_WORK_MAX (that budget bounds the
 * O(refs * block) LOOKUP product, not the linear template walk), and the inner
 * pass indexes the template with a uint32. The UNICODE_STRING (`_U`) forms are
 * implicitly bounded because Length is a USHORT (<= 32767 WCHARs); the counted
 * SIZE_T form removes that bound and needs an explicit one (TODO-22 s25 item 3).
 *
 * This is a RESOURCE-POLICY refusal (STATUS_INSUFFICIENT_RESOURCES), NOT a
 * malformed-input one (STATUS_INVALID_PARAMETER): a structurally valid template
 * larger than this is a resource the kernel declines to scan, mirroring the
 * work-budget refusal, and is a DELIBERATE, DOCUMENTED divergence from native's
 * unbounded SIZE_T -- named a source-policy constant of its own rather than
 * reusing RTL_ENV_BLOCK_MAX_WCHARS, whose meaning is the block-scan bound, not a
 * template bound. 1 MiWCHAR templates are admitted (far past any real cmdline);
 * the value stays <= UINT32_MAX so the post-check narrowing to the pass index is
 * exact. */
#define RTL_ENV_SOURCE_MAX_WCHARS  1048576u

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
 * below; that boundary is owned by TODO-22 s24 (item: "Boundary probe+copy for a
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

/* ntdll RtlExpandEnvironmentStrings(PVOID Environment, PWSTR Source,
 * SIZE_T SourceLength, PWSTR Destination, SIZE_T DestinationLength,
 * PSIZE_T ReturnLength) -- the COUNTED (raw ptr + WCHAR count) form a PE import
 * table may name in place of the UNICODE_STRING `_U` entry (phnt ntrtl.h,
 * winsiderss 2026-07-15). All lengths are in WCHARs. `RtlExpandEnvironmentStrings_U`
 * delegates DOWN to the same SIZE_T-safe engine (TODO-22 s25).
 *
 *   Environment       MUST be NULL (the calling process's own environment,
 *                     synthesized from task_current()'s environ). A non-NULL
 *                     bare pointer carries no allocation extent and is refused
 *                     STATUS_NOT_SUPPORTED -- the foreign-block boundary that
 *                     probes + copies it is owned by TODO-22 s24.
 *   Source/SourceLength   the UTF-16 template and its WCHAR count. A template
 *                     longer than RTL_ENV_SOURCE_MAX_WCHARS is a RESOURCE-POLICY
 *                     refusal (STATUS_INSUFFICIENT_RESOURCES), a deliberate
 *                     documented divergence from native's unbounded SIZE_T.
 *   Destination/DestinationLength   the output buffer and its WCHAR capacity.
 *   ReturnLength      optional; set to the required buffer size in WCHARs
 *                     INCLUDING the terminating NUL, on BOTH success and
 *                     STATUS_BUFFER_TOO_SMALL. (This is the ReactOS `TotalLength`
 *                     convention, which starts at 1 for the NUL; it deliberately
 *                     DIFFERS from RtlQueryEnvironmentVariable below, whose count
 *                     excludes the NUL on success -- do not unify them.)
 *
 * The result count has NO UNICODE_STRING USHORT ceiling (the _U wrapper adds
 * that). The only refusals are saturation of the output counter or a required
 * size that cannot be expressed as `required + 1` WCHARs (STATUS_UNSUCCESSFUL,
 * ReturnLength 0), both unreachable under the current caps. `%%`-preservation,
 * literal-copy of unknown/oversize names, the no-alias matrix, the two-pass
 * agreement check, and the per-pass work budget are exactly as documented for
 * the engine above. Returns STATUS_SUCCESS, STATUS_BUFFER_TOO_SMALL,
 * STATUS_INVALID_PARAMETER, STATUS_UNSUCCESSFUL, STATUS_INSUFFICIENT_RESOURCES,
 * STATUS_NO_MEMORY, or STATUS_NOT_SUPPORTED. Thread context only. */
NTSTATUS RtlExpandEnvironmentStrings(void *Environment,
                                     const uint16_t *Source, uint64_t SourceLength,
                                     uint16_t *Destination, uint64_t DestinationLength,
                                     uint64_t *ReturnLength);

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

/* ===========================================================================
 * ntdll Rtl environment exports (TODO-22 s21)
 *
 * Real ntdll resolves environment access entirely in user mode over the
 * PEB-resident block. Impossible OS keeps the authoritative environment in the
 * KERNEL (task->environ), so these are thin kernel-side routines over the same
 * env_get_copy / env_set / env_unset storage API the Nt env syscalls use -- the
 * compat bridge that lets an ntdll-importing binary and the CRT resolve.
 *
 * FOREIGN-BLOCK REFUSAL IS UNIFORM ACROSS THIS LAYER. Query and Set below refuse a
 * non-NULL `Environment` with STATUS_NOT_SUPPORTED, exactly as
 * RtlExpandEnvironmentStrings_U does and for the identical reason: a bare pointer
 * carries no allocation EXTENT, so reading it means trusting a caller-supplied
 * terminator. The consequence is stated plainly rather than papered over: a block
 * from RtlCreateEnvironment is a CLONE/DESTROY artifact only -- it cannot be
 * queried or mutated through these entries -- so this export set is NOT
 * compatibility-complete and MUST NOT be described as such. TODO-22 s24 restores
 * the explicit-block forms through ONE trusted probe+copy snapshot helper shared by
 * the query and expansion paths.
 *
 * REACHABILITY GATE: none of these is user-reachable (no SSDT row, no kernel32
 * caller, no ntdll export-table row), and they MUST NOT become reachable while
 * TODO-22 s20's BLOCKING item is open -- the block/expansion path takes ~2 MiB via
 * pmm_alloc_contiguous on an unsynchronized PMM bitmap, so two CPUs can
 * double-allocate the same frames (owner: 03-memory-concurrency/TODO-03 s1). A unit
 * test pins the absence of an ntdll export row so wiring one is a deliberate act,
 * not an accident.
 * =========================================================================== */

/* ntdll RtlQueryEnvironmentVariable_U(PVOID Environment, PUNICODE_STRING Name,
 * PUNICODE_STRING Value). Copies the value of `Name` into `Value->Buffer`.
 *
 *   Environment  MUST be NULL (the calling process's own environment, read from
 *                task_current()'s authoritative environ). Non-NULL is refused with
 *                STATUS_NOT_SUPPORTED -- see the foreign-block note above.
 *   Name         UTF-16, case-insensitive. `=` is rejected (STATUS_INVALID_PARAMETER)
 *                EXCEPT at position 0, which is ntdll's own rule and admits the
 *                hidden "=X:" drive-cwd form (TODO-22 s12). The storage layer is
 *                STRICTER still (env_name_classify accepts only the exact "=<A-Z>:"
 *                shape), so a name like "=FOO" passes this ntdll-shaped check and is
 *                then refused by storage -- the layering is deliberate: this entry
 *                enforces the ABI's rule, the store enforces its own.
 *   Value        IN/OUT. MaximumLength (bytes) bounds the write. Only
 *                MaximumLength and Buffer are INPUTS -- Length is written by this
 *                call, so whatever a caller arrives with there is ignored rather
 *                than validated (real ntdll overwrites it too).
 *
 * NO-ALIAS: the output range [Value->Buffer, +MaximumLength) MUST NOT overlap the
 * Value descriptor itself; an overlap returns STATUS_INVALID_PARAMETER. The Length
 * store would otherwise corrupt the content the caller is about to read, and the
 * write pointer is snapshotted at entry precisely so a self-aliasing descriptor
 * cannot move it mid-call. The sibling NtQueryEnvironmentVariable handler and
 * rtl_env_expand_block enforce the same rule.
 *
 * LENGTH CONVENTION (both on success and on STATUS_BUFFER_TOO_SMALL): Value->Length
 * is the CONTENT byte count EXCLUDING the terminating NUL. The buffer FITS when
 * MaximumLength >= Length -- the NUL is NOT required to fit, and is written only
 * when there is room for a full WCHAR. Its kernel32 caller depends on the count
 * convention (GetEnvironmentVariableW adds the NUL itself and sizes from it).
 *
 * THE EXACT-FIT RULE FOLLOWS THE WRK, AND THE SOURCES CONFLICT -- same tie-break as
 * RtlDestroyEnvironment below. At MaximumLength == Length the WRK
 * (base/ntos/rtl/environ.c, the real NT source) SUCCEEDS:
 *     Value->Length = CurrentValue.Length;
 *     if (Value->MaximumLength >= CurrentValue.Length) {
 *         RtlCopyMemory(...);
 *         if (Value->MaximumLength > CurrentValue.Length) { ...Buffer[...] = L'\0'; }
 *         Status = STATUS_SUCCESS;
 *     }
 * whereas ReactOS (sdk/lib/rtl/env.c) compares strictly (`*ReturnLength <
 * ValueLength`) and returns STATUS_BUFFER_TOO_SMALL for the same call. We match the
 * WRK: the target is Win11 binary compatibility, not ReactOS compatibility.
 *
 * THE SHARP EDGE THIS INHERITS, STATED SO NOBODY REDISCOVERS IT: at exact fit,
 * STATUS_SUCCESS does NOT imply Buffer is NUL-terminated. A caller that treats
 * success as "I now hold a C string" is wrong here exactly as it is wrong on
 * Windows -- read Value->Length. Our one deliberate divergence from the WRK is the
 * NUL guard: the WRK's `MaximumLength > Length` writes a 2-byte WCHAR when an ODD
 * MaximumLength leaves only ONE spare byte, so we require two (>= Length + 2). The
 * two rules are identical for every even MaximumLength, i.e. for every well-formed
 * UTF-16 caller.
 *
 * Returns STATUS_SUCCESS, STATUS_VARIABLE_NOT_FOUND (0xC0000100) when absent,
 * STATUS_BUFFER_TOO_SMALL, STATUS_INVALID_PARAMETER, STATUS_NAME_TOO_LONG,
 * STATUS_NO_MEMORY, or STATUS_NOT_SUPPORTED. Thread context only. */
NTSTATUS RtlQueryEnvironmentVariable_U(void *Environment, UNICODE_STRING *Name,
                                       UNICODE_STRING *Value);

/* ntdll RtlQueryEnvironmentVariable(PVOID Environment, PCWSTR Name,
 * SIZE_T NameLength, PWSTR Value, SIZE_T ValueLength, PSIZE_T ReturnLength) --
 * the COUNTED (raw ptr + WCHAR count) read form (phnt ntrtl.h, winsiderss
 * 2026-07-15). `RtlQueryEnvironmentVariable_U` delegates DOWN to the shared
 * value-copy core after its own descriptor validation (TODO-22 s25).
 *
 *   Environment      MUST be NULL (calling process's own environment). Non-NULL
 *                    is refused STATUS_NOT_SUPPORTED, as for the _U form.
 *   Name/NameLength  the UTF-16 name and its WCHAR count. Case-insensitive ASCII
 *                    fold. `=` is rejected except at index 0 (the hidden "=X:"
 *                    drive-cwd form). A name longer than ENV_NAME_MAX WCHARs
 *                    cannot name a storable variable and is STATUS_NAME_TOO_LONG.
 *   Value/ValueLength  the output buffer and its WCHAR capacity.
 *   ReturnLength     the value's WCHAR count. On SUCCESS it EXCLUDES the NUL; on
 *                    STATUS_BUFFER_TOO_SMALL it INCLUDES the NUL (the required
 *                    buffer size). This is ReactOS's counted convention, pinned
 *                    deliberately -- its `_U` wrapper corrects the too-small
 *                    value with `ReturnLength -= 1` to reach the _U's
 *                    excludes-NUL-on-both-paths rule, which is exactly how
 *                    RtlQueryEnvironmentVariable_U maps this count to
 *                    Value->Length.
 *
 * EXACT FIT (ValueLength == content WCHARs, excluding the NUL) SUCCEEDS -- the
 * WRK rule (base/ntos/rtl/environ.c), NOT ReactOS's strict-less-than -- because
 * the target is Win11 binary compatibility. The NUL is written only when
 * ValueLength leaves room for a whole extra WCHAR; at exact fit SUCCESS does NOT
 * imply NUL-termination (read ReturnLength). Returns STATUS_SUCCESS,
 * STATUS_VARIABLE_NOT_FOUND, STATUS_BUFFER_TOO_SMALL, STATUS_INVALID_PARAMETER,
 * STATUS_NAME_TOO_LONG, STATUS_NO_MEMORY, or STATUS_NOT_SUPPORTED. Thread
 * context only. */
NTSTATUS RtlQueryEnvironmentVariable(void *Environment,
                                     const uint16_t *Name, uint64_t NameLength,
                                     uint16_t *Value, uint64_t ValueLength,
                                     uint64_t *ReturnLength);

/* ntdll RtlSetEnvironmentVariable(PVOID *Environment, PUNICODE_STRING Name,
 * PUNICODE_STRING Value). Sets or deletes a variable.
 *
 *   Environment  POINTER-TO-POINTER in the real ABI because ntdll may reallocate an
 *                explicit block and write the new pointer back. Only the
 *                `Environment == NULL` form (the calling process's own environment)
 *                is supported; ANY non-NULL pointer is refused with
 *                STATUS_NOT_SUPPORTED BEFORE `*Environment` is dereferenced.
 *                The distinction matters and is not pedantry: `Environment != NULL`
 *                with `*Environment == NULL` is a DIFFERENT ntdll form (an initially
 *                empty caller-owned environment to be allocated and written back),
 *                so treating it as the current process would silently mutate the
 *                wrong environment. Refusing before the deref also means an
 *                untrusted pointer is never read.
 *   Name         validated exactly as RtlQueryEnvironmentVariable_U above.
 *   Value        NULL DELETES the variable. Deleting a variable that does not exist
 *                is a SILENT SUCCESS (STATUS_SUCCESS), matching ntdll.
 *
 * For the supported NULL form there is no cell to write back: all pointer storage is
 * left untouched. Returns STATUS_SUCCESS, STATUS_INVALID_PARAMETER,
 * STATUS_NAME_TOO_LONG, STATUS_NO_MEMORY, STATUS_QUOTA_EXCEEDED (block cap), or
 * STATUS_NOT_SUPPORTED. Thread context only. */
NTSTATUS RtlSetEnvironmentVariable(void **Environment, UNICODE_STRING *Name,
                                   UNICODE_STRING *Value);

/* ntdll RtlCreateEnvironment(BOOLEAN CloneCurrent, PVOID *Environment). Produces a
 * block in the env_create_block format (see env.h), freed by RtlDestroyEnvironment.
 *
 *   clone_current != 0  -> a sorted snapshot of the calling process's environment.
 *   clone_current == 0  -> an EMPTY block. NOT the Registry-derived block that
 *                          userenv's CreateEnvironmentBlock(bInherit = FALSE) means:
 *                          ntdll walks no Registry, so empty is the correct and
 *                          complete answer here (env_create_empty_block, env.h).
 *
 * The resulting block cannot be queried or mutated through the Rtl entries above
 * until TODO-22 s24 lands the trusted-snapshot boundary; it is a clone/destroy
 * artifact today. Returns STATUS_SUCCESS, STATUS_INVALID_PARAMETER (NULL out_env),
 * STATUS_NO_MEMORY, or STATUS_BUFFER_TOO_SMALL (environ over
 * ENV_CREATE_BLOCK_MAX_WCHARS). Thread context only. */
NTSTATUS RtlCreateEnvironment(uint8_t clone_current, void **out_env);

/* ntdll RtlDestroyEnvironment(PVOID Environment). Frees a block from
 * RtlCreateEnvironment; NULL is a no-op. `env` MUST be a pointer previously returned
 * by RtlCreateEnvironment (the env_destroy_block contract: it recovers the size from
 * the hidden header, so an arbitrary pointer reads the wrong predecessor).
 *
 * RETURNS NTSTATUS, NOT VOID: the sources conflict (the ReactOS NDK prototypes it
 * VOID, the WRK returns NTSTATUS). NTSTATUS is the strict binary-compat superset on
 * x86-64 -- a VOID-expecting caller simply ignores RAX, whereas a status-expecting
 * caller reading RAX from a VOID function would read garbage. Always STATUS_SUCCESS
 * today (the free path is best-effort by env_destroy_block's own contract). */
NTSTATUS RtlDestroyEnvironment(void *env);

/* RtlCreateEnvironmentEx Flags (phnt ntrtl.h, winsiderss 2026-07-15). TRANSLATE_FROM_OEM
 * MODIFIES TRANSLATE and is meaningless alone -- 0x2 without 0x1 is an incompatible
 * combination, not an independent request. EMPTY selects an empty block over a clone. */
#define RTL_CREATE_ENVIRONMENT_TRANSLATE            0x1u
#define RTL_CREATE_ENVIRONMENT_TRANSLATE_FROM_OEM   0x2u
#define RTL_CREATE_ENVIRONMENT_EMPTY                0x4u

/* ntdll RtlSetCurrentEnvironment(PVOID Environment, PVOID *PreviousEnvironment). Adopts
 * `Environment` (a block from RtlCreateEnvironment / RtlCreateEnvironmentEx, in the
 * env_create_block format) as the calling process's live environment, optionally handing
 * back the replaced environment through `PreviousEnvironment` as a fresh block the caller
 * frees with RtlDestroyEnvironment (NULL there simply frees the old store).
 *
 * OWNERSHIP TRANSFER on success, matching native ntdll (which installs the pointer as
 * PEB ProcessParameters->Environment): on STATUS_SUCCESS the `Environment` block is
 * CONSUMED -- the caller must NOT free it and frees `PreviousEnvironment` instead. This
 * kernel has no PEB env pointer, so it copies the contents into the live store and frees
 * the source, which is ownership-equivalent from the caller's view. On ANY failure the
 * caller retains `Environment` (nothing is freed) and the prior environment stays live.
 * The swap is atomic (one environ_lock span) and all-or-nothing. NTSTATUS + PVOID per
 * phnt/WRK (ReactOS's VOID/PWSTR is the outlier, matching RtlDestroyEnvironment above).
 *
 * Environment MUST be provenanced (env_block_extent best-effort-rejects a malformed
 * header): a NULL pointer is STATUS_INVALID_PARAMETER, a non-provenanced one is
 * STATUS_NOT_SUPPORTED, and in both the live environment is untouched. Like the other Rtl
 * env entries it is NOT user-reachable until TODO-22 s24's probe+copy + s20's PMM item
 * close (no ntdll export row). Returns STATUS_SUCCESS, STATUS_INVALID_PARAMETER,
 * STATUS_NOT_SUPPORTED, STATUS_NO_MEMORY, or STATUS_QUOTA_EXCEEDED. Thread context only. */
NTSTATUS RtlSetCurrentEnvironment(void *Environment, void **PreviousEnvironment);

/* ntdll RtlSetEnvironmentStrings(PCWSTR NewEnvironment, SIZE_T NewEnvironmentSize).
 * Replaces the calling process's environment from a counted UTF-16 double-NUL block.
 * `NewEnvironmentSize` is in BYTES and bounds the SCAN (not a single deref): an ODD count
 * is refused rather than floored (flooring would drop a byte and adopt a different block),
 * and 0 is STATUS_INVALID_PARAMETER (the empty environment is the 4-byte "\0\0" form, not
 * a zero-length block).
 *
 * STRICT counted-block adoption (env_replace_from_block_utf16): a missing terminator, an
 * over-cap or malformed entry, or trailing data past the terminator refuses the WHOLE
 * block with the live environment untouched -- an all-or-nothing ABI never installs a
 * silently truncated subset. Rides the nt_rtlenv kernel-resident input contract:
 * NewEnvironment must be a mapped block of at least NewEnvironmentSize bytes (a user
 * pointer needs TODO-22 s24's probe+copy). Returns STATUS_SUCCESS,
 * STATUS_INVALID_PARAMETER, STATUS_QUOTA_EXCEEDED, or STATUS_NO_MEMORY. Thread context. */
NTSTATUS RtlSetEnvironmentStrings(const uint16_t *NewEnvironment,
                                  uint64_t NewEnvironmentSize);

/* ntdll RtlCreateEnvironmentEx(PVOID SourceEnv, PVOID *Environment, ULONG Flags). Creates
 * an environment block (env_create_block format, freed by RtlDestroyEnvironment). A
 * non-NULL SourceEnv (translate a caller block) needs TODO-22 s24's trusted snapshot and
 * is refused STATUS_NOT_SUPPORTED, never silently ignored.
 *
 * Flag precedence (validate BEFORE acting, so EMPTY cannot mask a malformed Flags):
 * unknown bit -> STATUS_INVALID_PARAMETER; TRANSLATE_FROM_OEM without TRANSLATE ->
 * STATUS_INVALID_PARAMETER (incompatible); either TRANSLATE bit -> STATUS_NOT_SUPPORTED
 * (translation unimplemented, never accept-and-ignore); EMPTY -> an empty block; Flags==0
 * -> a clone of the current environment. Returns STATUS_SUCCESS, STATUS_INVALID_PARAMETER,
 * STATUS_NOT_SUPPORTED, STATUS_NO_MEMORY, or STATUS_BUFFER_TOO_SMALL (environ over the
 * block cap on the clone path). Thread context only. */
NTSTATUS RtlCreateEnvironmentEx(void *SourceEnv, void **Environment, uint32_t Flags);

