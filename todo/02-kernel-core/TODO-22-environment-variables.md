---
schema_version: 1
id: environment-variables
domain: 02-kernel-core
status: active
title: "TODO-22 -- Environment Variables & Process Arguments"
---

# TODO-22 -- Environment Variables & Process Arguments

> **Validated:** 2026-07-13 | validate-todo-file clean (structure / IO table / XREF / test wiring); fixed cross-file XREF section-number drift (TODO-11 §2/§7 swap + §9->§5, TODO-12 §2->§7, TODO-14 §5->§4, TODO-15 §3->§9, D12 T04->T02 label)

> **Gap-audited:** 2026-07-13 | gap-audit + codex-gap-audit (needs-attention, 6 findings, all confirmed via receiving-code-review). Filed §18 (cmd dynamic pseudo-vars + delayed `!VAR!`), §19 (ntdll Rtl env layer); hardened §1 (environ_lock + UAF-safe `env_get_copy` lifetime), §4 (bounded/atomic argv+envp ingestion, E2BIG cap + argv->CommandLine encode), §9 (SetEnvironmentVariable process-local, no Registry write-back), §3 (single-pass expansion), §15 (empty-cmdline argv[0]=module path). Branch C: POSIX environ/clearenv/putenv -> D12 T02; Branch D noted: batch FOR/IF/setlocal interpreter, /proc/PID/environ.

> **Goal:** Implement per-process environment variable storage, `%VAR%` expansion, `PATH`-based command lookup, `argv`/`argc` kernel preparation, the Win32 `GetEnvironmentVariable`/`SetEnvironmentVariable` API surface, and the `.profile` shell startup script. No env API exists at all today: there is no `env_get`, no `SYS_GETENV`, no `PATH` lookup, and no argv array in `struct task`. Without this, every user-mode program launches with no arguments, no environment, and no way to find executables on disk.

> [!IMPORTANT]
> **Current state:** Confirmed absent in tree: no `src/kernel/env.c`; `include/kernel/sched/task.h` has no `environ` / `argv` / `argc` fields; kernel `*.c` / `*.h` contain no `env_get`, `GetEnvironmentVariable`, or `CreateEnvironmentBlock` definitions.
> **Scope boundary with adjacent TODOs:**
> - `TODO-11-peb-teb-user-abi.md §2` defines `RTL_USER_PROCESS_PARAMETERS.Environment` (type/layout); `TODO-11-peb-teb-user-abi.md §5` allocates PEB/process parameters and the initial env block pointer target.
> - `TODO-11-peb-teb-user-abi.md §7` defines the initial user stack frame. Today ELF uses `envp NULL`; Win32 reads `PEB->ProcessParameters`. When TODO-22 adds `task->argv` / `task->environ`, extend §7 (or the exec path) so the ring-3 frame and `CommandLine` consume those fields; do not assume §7 already pushes a full `envp[]` from `struct task`.
> - **This TODO** owns: the kernel-side `char **environ` storage in `struct task`, `env_get/set/unset/expand`, population of default variables from Registry, `argv[]` preparation in the kernel and shell, `NtSetEnvironmentVariable` / `NtQueryEnvironmentVariable` syscalls, Win32 `GetEnvironmentVariable`/`ExpandEnvironmentStrings` wrappers, PATH lookup, `SET` shell command, and `.profile` startup.

---

## Inputs

- `src/kernel/sched/task.c` -- `struct task` (environ and argv fields must be added)
- `include/kernel/sched/task.h` -- task struct header
- `src/kernel/sched/syscall.c` -- syscall dispatch table
- `src/kernel/registry.c` / `include/registry.h` -- `RegQueryValueEx`, `NtQueryValueKey` / `NtEnumerateValueKey` path (TODO-14 §4) for default env vars at boot
- `src/desktop/terminal.c` -- terminal/shell command dispatch
- → XREF: `TODO-11-peb-teb-user-abi.md §2` -- `RTL_USER_PROCESS_PARAMETERS.Environment` field (layout)
- → XREF: `TODO-11-peb-teb-user-abi.md §5` -- PEB / process-parameters allocation; `Environment` pointer and block backing store
- → XREF: `TODO-11-peb-teb-user-abi.md §7` -- initial user stack; must be updated to consume `task->argv` / `task->environ` when populated (see IMPORTANT above)
- → XREF: `TODO-12-native-api-ssdt.md §5` -- reserve **new** SSDT indices for `NtQueryEnvironmentVariable` / `NtSetEnvironmentVariable` (do not use `0x00D2`..`0x00D6`; those are `NtQuerySystemEnvironmentValue*` UEFI firmware APIs in `service_numbers.h`)
- → XREF: `TODO-12-native-api-ssdt.md §7` -- `NtCreateProcess` path must call `env_copy()` / argv inheritance when TODO-22 §1 lands (child private copies)
- → XREF: `TODO-15-security-reference-monitor.md §7` -- `ACCESS_TOKEN` for `CreateEnvironmentBlock` hToken parameter and env sanitization for elevated processes (TODO-22 §13, §16)
- → XREF: `TODO-14-registry-completion.md §4` -- `NtQueryValueKey` / `NtEnumerateValueKey` (and related) for `Session Manager\Environment`, `HKCU\Environment`, App Paths (§17)
- → XREF: `12-user-platform-sdk/TODO-02-env-vars-process-abi.md` (D12 T02) -- SDK/user-mode contracts, `env_expand_path` (`%1`..`%9`), optional `SYS_GETENV` shims (COMPLEMENT; kernel storage stays in this TODO)
- → XREF: `TODO-01-kernel-init-sequencing.md §4` -- Phase 2 timing for Registry-backed defaults vs bootstrap env (§2)
- → XREF: `TODO-23-exception-dispatch-seh.md §13` -- `ProbeForRead` / `ProbeForWrite` for syscall buffers (§4, §5)

---

## Outcome

- `struct task` carries `char **environ` (UTF-8 key=value array, sorted alphabetically) and `char **argv` (argument array); both are deep-copied on spawn.
- `env_get`, `env_set`, `env_unset`, `env_expand` are available kernel-wide; `env_set` maintains sorted order.
- System defaults (`PATH`, `SYSTEMROOT`, `TEMP`, `USERNAME`, `COMPUTERNAME`, `USERPROFILE`) are populated from Registry at boot.
- `NtSetEnvironmentVariable` and `NtQueryEnvironmentVariable` let user-mode processes read and write their own env block; input validation enforces name/value size limits.
- `GetEnvironmentVariableW/A`, `SetEnvironmentVariableW/A`, `ExpandEnvironmentStringsW/A`, `GetCommandLineW`, `GetEnvironmentStrings` are usable Win32 API functions.
- `SearchPathW/A`, `CommandLineToArgvW`, `CreateEnvironmentBlock`, `DestroyEnvironmentBlock`, `ExpandEnvironmentStringsForUser` provide full Win32 parity.
- Shell PATH lookup respects `PATHEXT` extension search order; `SET` / `ECHO` commands, and `%VAR%` argument expansion all work.
- Hidden drive-letter variables (`=C:`, `=D:`) track per-drive current directories; propagated correctly through `CreateProcess`.
- Environment variables are sanitized for elevated/restricted-token processes to prevent privilege escalation.
- App Paths registry-based executable lookup provides a `PATH`-free alternative for installed applications.
- `.profile` is sourced on shell startup; users can persist env changes across reboots.

---

## Implementation Order

| ⭐   | Order | Deliverable                                        | Depends On                 | Status |
| --- | :---: | -------------------------------------------------- | -------------------------- | :----: |
| 💎   |   1   | Per-process environ storage & kernel API           | --                         |  [x]   |
| 💎   |   2   | System default variables from Registry             | §1, T14 §4                 |  [x]   |
| 💎   |   3   | `%VAR%` expansion (`env_expand`)                   | §1                         |  [x]   |
| 💎   |   4   | argv array: kernel storage & shell parsing         | §1                         |  [x]   |
| 💎   |   5   | Nt/Zw environment variable syscalls                | §1, T11 §2, T11 §5, T12 §4 |  [x]   |
| 💎   |   6   | Win32 API wrappers                                 | §5                         |  [/]   |
| 💎   |   7   | Shell integration (PATH lookup, SET, ECHO)         | §3, §4                     |  [/]   |
| 💎   |   8   | `.profile` startup script                          | §7                         |  [/]   |
| ⭐   |   9   | Environment change notifications & `sysdm.cpl` tab | §6, §8                     |  [/]   |
| 💎   |  10   | Environment block sorting & size limits            | §1                         |  [x]   |
| 💎   |  11   | PATHEXT variable & extension search order          | §7                         |  [/]   |
| 💎   |  12   | Hidden drive-letter variables (`=C:`, `=D:`)       | §1, §10                    |  [/]   |
| 💎   |  13   | CreateEnvironmentBlock / DestroyEnvironmentBlock   | §2, §10, §12, T15 §4       |  [/]   |
| 💎   |  14   | SearchPathW / SearchPathA Win32 API                | §7, §6                     |  [/]   |
| 💎   |  15   | CommandLineToArgvW Win32 API                       | §4, §6                     |  [/]   |
| 💎   |  16   | Environment variable security & sanitization       | §1, T15 §4                 |  [/]   |
| ⭐   |  17   | App Paths registry-based executable lookup         | §7, T14 §4                 |  [/]   |
| 💎   |  18   | cmd.exe dynamic pseudo-vars & delayed `!VAR!`      | §3, §7                     |  [/]   |
| 💎   |  19   | Rtl expansion hardening (count wrap, `%=X:%`)      | §3                         |  [x]   |
| 💎   |  20   | Rtl export prereqs (lookup bound, extent, alloc)   | §19                        |  [x]   |
| 💎   |  21   | ntdll Rtl environment exports                      | §5, §6, §19, §20           |  [/]   |
| 💎   |  22   | env allocator safety + UTF-8 expansion budget      | §1, §3, §21                |  [x]   |
| 💎   |  23   | Live-environment adoption (SetCurrent/Strings/Ex)  | §20, §21, T33 §10          |  [x]   |
| 💎   |  24   | Rtl expansion completeness (probe+copy, budget)    | §3, §20, §21, T23 §13      |  [/]   |
| 💎   |  25   | Counted (non-`_U`) Rtl env read forms              | §20, §21, T33 §10          |  [x]   |

> 💎 = parity work -- matches what Windows 11 and Linux already do.
> ⭐ = exclusive work -- Impossible OS is superior or first.

---

## 1. Per-Process Environ Storage & Kernel API

- [x] Added to `struct task` in `include/kernel/sched/task.h` (after `cwd_lock`):
  ```c
  char      **environ;        /* NULL-terminated "KEY=VALUE" UTF-8 array, or NULL */
  uint32_t    environ_count;  /* live entries (excludes NULL terminator) */
  char      **argv;           /* NULL-terminated argument array, or NULL */
  int         argc;
  mutex_t     environ_lock;   /* serializes environ/argv mutate+read across sibling threads */
  ```
- [x] `environ_lock` is a **mutex, not a spinlock** (design-review adoption): env mutation calls kmalloc/kfree/pmm and copies up to 32 KiB, forbidden under a spinlock; env is thread-context-only. task.h includes `kernel/sched/mutex.h`.
- [x] `environ`/`argv` NULL + counts 0 initially; fields + `mutex_init(&environ_lock,"environ")` inited once in the `task_init` all-slots loop (`src/kernel/sched/task.c`).
- [x] Value strings ≤ `ENV_STR_KMALLOC_MAX` (4096) → `kmalloc`, larger → `pmm_alloc_contiguous` (freed per-frame); allocator kind recovered from `strlen+1`. Array capped at `ENV_MAX_ENTRIES` (511) to stay ≤ 4 KiB.
- [x] Implemented in `src/kernel/env.c`, declared in `include/kernel/env.h`:
  ```c
  int  env_get_copy(struct task *t, const char *name, char *out, uint32_t out_size);
  void env_lock(struct task *t); void env_unlock(struct task *t);
  const char *env_peek_locked(struct task *t, const char *name); /* borrowed, lock-held */
  int  env_set(struct task *t, const char *name, const char *value);
  int  env_unset(struct task *t, const char *name);
  int  env_copy(struct task *dst, const struct task *src);       /* deep copy */
  void env_free(struct task *t);                                 /* at reap barrier */
  ```
- [x] Read API is copy-out (design-review UAF fix): `env_get_copy` copies under the lock (returns full length even when truncated); `env_peek_locked` borrows only between `env_lock`/`env_unlock`. No unlocked `env_get`.
- [x] `env_set`: builds the `"name=value"` entry BEFORE freeing the old (OOM-safe); replaces or `krealloc`s the array +1 slot. Case-insensitive; bounded validation of name (≤256, no `=`) + value (≤ `ENV_VALUE_MAX` 32767).
- [x] `env_unset`: bounded ci name scan, `env_str_free` the string, shift pointers left, re-terminate, decrement count.
- [x] `env_copy`: snapshots src under `src->environ_lock` only (dst unpublished, no dst lock); fresh array + `env_strdup`, unwinds on OOM. Child-creation wiring owned by → XREF `TODO-12-native-api-ssdt.md §7`.
- [x] SMP + lifetime (CLAUDE.md SMP gate; parallels `TODO-21 §1` `cwd_lock`): readers/mutators serialize on `environ_lock`; `env_free` runs lock-free at the `task_cleanup` reap barrier, freeing both arrays.
  - All-CPU reap-barrier re-proof + mutex waiter-queue SMP backfill are single-CPU-scheduler follow-ups (→ XREF `03-memory-concurrency/TODO-07-smp-phase2.md` reap-barrier item; `03-memory-concurrency/TODO-08-advanced-sync.md §11`).

- [x] Commit: `"kernel/env: per-process environ array + lock, env_get_copy/set/unset/copy"`

**Test checkpoint:** `env_set`/`env_get_copy`/`env_unset`/`env_copy` on a test task; case-insensitivity; validation; truncating get returns full length; OOM-during-replace preserves the old value; name-length boundary (256 OK / 257 TOOLONG); >4 KiB value round-trips via PMM; `env_free` leaves no dangling pointers. `bash scripts/test.sh SUITE=abi` green (1193 passed, 0 failed, 0 leaked); `tail -1 build/build.log` is `=== BUILD OK ===`. QEMU WHPX + TCG; VirtualBox; bare metal.

> **Test runner:** `scripts\debug\kernel\run-abi-tests.bat` (SUITE=abi) | 15 env suites, 0 failures
> **Notes:**
> - **What shipped** -- `src/kernel/env.c` + `include/kernel/env.h`: per-task `environ`/`argv` storage + `env_get_copy`/`env_set`/`env_unset`/`env_copy`/`env_free`/`env_lock`/`env_peek_locked`; 15 tests in `src/kernel/test/test_env.c` (TEST_CAT_ABI).
> - **How it integrates** -- fields + `mutex_init` land once in the `task_init` all-slots loop; `env_free` wired at the `task_cleanup` reap barrier; build auto-discovers `env.c`.
> - **Downstream effects** -- unblocks env-copy wiring for `TODO-12-native-api-ssdt.md §7` (child env); storage base for later TODO-22 sections; Codex adoptions in the commit message.
> - **Canonical doc** -- [`include/kernel/env.h`](../../include/kernel/env.h) header contract (lock + reader-lifetime rules).
> - **Scope boundary** -- §1 owns kernel storage + the C API only; `%VAR%` expansion is §3, Nt syscalls §5, Win32 wrappers §6, argv/exec handoff §4, sorting/size-block §10.
> **Verified:** 2026-07-13 | commit `49335ede` | 10/10 items | build OK | tests 1193/1193 PASS
> **Accepted:** [H] task_cleanup reap barrier lacks all-CPU quiescence for env_free (single-CPU scheduler today) -> XREF: 03-memory-concurrency/TODO-07-smp-phase2.md (item: "task_cleanup reap barrier" at line 125)
> **Accepted:** [M] environ_lock inherits the mutex_t waiter-queue SMP race (unreachable on single-CPU) -> XREF: 03-memory-concurrency/TODO-08-advanced-sync.md §11 (item: "Wait-queue protection" at line 275)
> **Accepted:** [M] env names case-folded ASCII-only; non-ASCII compared case-sensitively -> XREF: 02-kernel-core/TODO-22-environment-variables.md §10 (item: "Upgrade env name case-folding" at line 141)
> **Accepted:** [L] env_copy has no live caller yet (§1 is storage+API only) -> XREF: 02-kernel-core/TODO-12-native-api-ssdt.md §7 (item: "wire env_copy() into every child-creation path" at line 383)
> **Quality reviewed:** 2026-07-13 | Codex 9x (design, adversarial, consistency, perf, re-adversarial) | 3M+2L fixed, 1H+2M+1L accepted-XREF | scope: kernel-code-quality

---

## 2. System Default Variables from Registry

- [x] `env_init_defaults(task)` (`src/kernel/env.c`) -- seeds a task's environment in three precedence layers applied via `env_set` (last write wins):
  1. **Synth base** (`env_synth_base`): `COMPUTERNAME` (← `HKLM\SYSTEM\ComputerName\ActiveComputerName\ComputerName`, default `IMPOSSIBLE-PC`); `USERNAME`=`Default`; `USERPROFILE`/`APPDATA`/`LOCALAPPDATA` derived from `USERNAME`; `TEMP`/`TMP`=`C:\Temp`; `PROCESSOR_ARCHITECTURE`=`AMD64`; `NUMBER_OF_PROCESSORS` (← `HKLM\HARDWARE\CPU\Count`, else `smp_cpu_count()`); `OS`=`Impossible_OS`; `WINDIR`/`SYSTEMROOT`=`C:\Impossible`; `SYSTEMDRIVE`=`C:`; `PATH`=`C:\Impossible\Bin;C:\Impossible\System32;C:\Programs`.
  2. **System overlay**: enumerate `HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Environment` (→ XREF `TODO-14-registry-completion.md §4`) via `RegEnumValue`; `env_set` each REG_SZ/REG_EXPAND_SZ value (overrides synth; non-string types skipped).
  3. **User overlay**: enumerate `HKCU\Environment`; `env_set` each (overrides system). `PATH` is APPENDED to the base with `;`, not replaced.
  Value buffers are heap/PMM-sized from `RegQueryInfoKey`, never a 32 KiB stack buffer (design-review adoption). Missing/unreadable keys are skipped so the synth base always lands.
- [x] Lifecycle: `env_init_defaults` seeds the initial system process only; children will inherit via `env_copy` once wired (→ XREF `TODO-12 §7`; no live caller today), not by re-reading the Registry per spawn.
- [x] `env_init_kernel_task()` seeds PID 0 (`task_get_by_pid(0)`) from `boot_phase3` after `task_init()`; applies a hardcoded bootstrap fallback table when `SUBSYS_REGISTRY` is not ready:
  ```c
  static const struct { const char *name, *value; } bootstrap_env[] = {
      { "PATH",       "C:\\Impossible\\Bin" },
      { "SYSTEMROOT", "C:\\Impossible" },
      { "TEMP",       "C:\\Temp" },
  };
  ```
  Boot order: `registry_init()` (Phase 2) precedes `task_init()` (Phase 3), so the Registry is normally up by the time PID 0 exists; the bootstrap table is the Registry-FAILURE fallback, not a phase-ordering gap (corrects the draft "Phase 1 before registry_init" note → XREF `TODO-01-kernel-init-sequencing.md §4`).
- [x] `registry_populate_defaults()` (`src/kernel/registry.c`) creates the three keys the read path consumes: `HARDWARE\CPU\Count`, `ComputerName\ActiveComputerName`, `Session Manager\Environment` (so the overlay is real, not dead).

- [x] Commit: `"kernel/env: system default variables from Registry, bootstrap env"`

**Test checkpoint:** After boot Phase 2+3 the `env_init_defaults` unit tests confirm `PATH`/`SYSTEMROOT`/`TEMP`/`USERNAME`/`COMPUTERNAME` land, the `Session Manager\Environment` overlay applies (`ComSpec`) while non-string values are skipped, a user `HKCU\Environment\PATH` appends, and a user `TEMP` overrides. QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-abi-tests.bat` (SUITE=abi) | 7 env-default suites, 0 failures
> **Notes:**
> - **What shipped** -- `env_init_defaults`/`env_init_kernel_task` in `src/kernel/env.c`: 3-layer (synth base -> HKLM system overlay -> HKCU user overlay, PATH appended) default environment seed; heap/PMM-sized enumeration buffers.
> - **How it integrates** -- `env_init_kernel_task()` seeds PID 0 in `boot_phase3` after `task_init()`; registry reads are best-effort (synth base always lands); descendants will inherit via `env_copy` once wired (TODO-12 §7; not yet).
> - **Downstream effects** -- adds `HARDWARE\CPU\Count` + `ComputerName` + `Session Manager\Environment` Registry defaults; unblocks env-block builders and shell PATH lookup; Codex design adoptions in the commit message.
> - **Canonical doc** -- [`include/kernel/env.h`](../../include/kernel/env.h) `env_init_defaults` contract.
> - **Scope boundary** -- §2 owns the default seed + Registry read; per-child env inheritance is `TODO-12 §7` (`env_copy`); token-derived `USERNAME` is SRM account-name work; `%VAR%` expansion is §3.
> **Verified:** 2026-07-13 | commit `91f1e2ec` | 4/4 items | build OK | tests 1214/1214 PASS, smoke PASS (env: 15 vars for PID 0)
> **Accepted:** [design] child processes receive env via `env_copy`, not `env_init_defaults` -> XREF: 02-kernel-core/TODO-12-native-api-ssdt.md §7 (item: "wire env_copy() into every child-creation path" at line 383)
> **Accepted:** [M] token UserSid -> account-name lookup for `USERNAME` (defaults to "Default" until then) -> XREF: 02-kernel-core/TODO-15-security-reference-monitor.md §10 (item: "LookupAccountSidW / LookupAccountNameW")
> **Accepted:** [design] `boot_phase3` Registry read is unlocked (BSP-only, post-readiness; only a concurrent AP-panic write could race) -> XREF: 02-kernel-core/TODO-14-registry-completion.md §14 (item: "read-path entry points RegQueryValueEx, RegEnumValue, RegQueryInfoKey under the lock")
> **Quality reviewed:** 2026-07-13 | Codex 5x (design, adversarial, consistency, perf, re-adversarial) | 1H+1L fixed, 3 accepted-XREF | scope: kernel-code-quality

---

## 3. `%VAR%` Expansion

- [x] `env_expand(t, input, output, max_len)` (`env.c`) -- single-pass byte walk:
  - On `%`: scan forward for closing `%`; extract `%NAME%`; look it up via `env_peek_locked` (case-insensitive ASCII fold, one `env_lock` held for the whole walk); found -> append value; not found (or name over `ENV_NAME_MAX`) -> append the literal `%NAME%` unchanged
  - `%%`: preserved VERBATIM (empty name = unresolved var), matching Win32/ntdll `ExpandEnvironmentStrings`; cmd.exe's `%%`->`%` batch escape is a distinct shell mode owned by §18. Unmatched trailing `%` copied verbatim; all other chars verbatim
  - Single-pass (matches Win32 `ExpandEnvironmentStrings`): each `%VAR%` replaced exactly once; a value containing `%OTHER%` is NOT re-expanded (no depth limit, no infinite-loop risk). Delayed `!VAR!` re-expansion is a distinct mode owned by §18
  - Returns bytes written excluding NUL; on overflow writes the truncated result + NUL and returns `max_len` (a sentinel); `output` always NUL-terminated when `max_len > 0` EXCEPT the overlap-rejection path (input/output overlap -> returns 0, both buffers left unchanged)
- [x] `env_expand` uses the caller's `task->environ`; kernel-internal calls pass the initial system process (`task_get_by_pid(0)`) as `t`. Caller must NOT already hold `t->environ_lock`

- [x] `RtlExpandEnvironmentStrings_U(Environment, Source, Destination, ReturnedLength)` (`nt/nt_rtlenv.c`, header `nt/nt_rtlenv.h`):
  - `Environment`: MUST be NULL (synthesized from `task_current()`'s UTF-8 environ via `env_build_block_utf16` -- NOT the stale user-mapped PEB block). A non-NULL block is refused with `STATUS_NOT_SUPPORTED` since §20: the ntdll ABI carries no allocation extent, so the extent-taking `rtl_env_expand_block` engine serves every in-kernel caller instead
  - Kernel-resident-input contract: since §20 every scan is bounded by a caller-guaranteed EXTENT (`min(extent, RTL_ENV_BLOCK_MAX_WCHARS)`); a block unterminated inside its extent is `STATUS_INVALID_PARAMETER`. A raw user block must be probed+copied into a kernel snapshot by the Win32 boundary first, which then calls `rtl_env_expand_block` (owned by §21, not §6)
  - `Source`/`Destination`: `UNICODE_STRING` (validated: even byte Length, Length <= MaximumLength, Buffer for non-empty); same single-pass `%VAR%`/`%%` semantics as `env_expand`; case-insensitive ASCII-fold name match (consistent with the UTF-8 store)
  - `ReturnedLength`: required buffer size in bytes including the WCHAR NUL; `STATUS_BUFFER_TOO_SMALL` when too small, with NO partial output and `Destination->Length` unchanged; on success `Destination->Length` = result bytes excluding NUL (required length in a wide accumulator)
  - Used by `ExpandEnvironmentStringsW` (§6, through `rtl_env_expand_block`) and by the shell for Win32-mode argument expansion

- [x] Commit: `"kernel/env: env_expand %VAR% substitution, RtlExpandEnvironmentStrings_U"`

**Test checkpoint:** `env_expand` replaces `%VAR%`; `%%` preserved verbatim (Win32, not a cmd escape); `%A%`=`%B%`, `%B%`=`x`, `env_expand("%A%")` -> literal `%B%` (single-pass, no recursion); `rtl_env_expand_block` expands over an explicit block and returns `STATUS_BUFFER_TOO_SMALL` + required length on an undersized `Destination` (the public `RtlExpandEnvironmentStrings_U` serves the NULL form only since §20). QEMU WHPX + TCG; VirtualBox; bare metal.
> **Test runner:** `scripts\debug\kernel\run-abi-tests.bat` (SUITE=abi) | 27 env_expand/Rtl suites added, 0 failures
> **Notes:**
> - **What shipped:** `env_expand` (UTF-8 single-pass `%VAR%`, Win32 `%%`-verbatim, bounded) in `env.c`; `RtlExpandEnvironmentStrings_U` (UTF-16) in new `nt/nt_rtlenv.c`; `env_build_block_utf16` (nls UTF-8->UTF-16) in `env.c`.
> - **How it integrates:** `env_expand` holds one `env_lock` across the walk; the Rtl core is a bounded kernel-resident two-pass transformer (count then write, no partial output, pass counts verified against between-pass mutation).
> - **Downstream:** §6 `ExpandEnvironmentStrings{W,A}` call `RtlExpandEnvironmentStrings_U`; §6 owns probing+copying a user Environment block into a kernel snapshot first (design + adversarial adoptions in the commit message).
> - **Scope boundary:** §3 owns the two primitives; §5/§6 own the syscall/Win32 boundary; cmd `%%`/`!VAR!` is §18; full Unicode name folding + the PMM allocator SMP lock are tracked elsewhere.
> **Verified:** 2026-07-13 | commit `c89eb4c8` | 3/3 items | build OK | tests 1271/1271 PASS
> **Accepted:** [H] large synth env block (>4 KiB) routes through the unlocked `pmm_alloc_contiguous` (pre-existing kernel-wide gap; env is one of many callers) -> XREF: 03-memory-concurrency/TODO-03-advanced-allocator.md §1 (item: "PMM bitmap SMP locking" at line 103)
> **Quality reviewed:** 2026-07-13 | Codex 12x (design + adversarial + re-adversarial + consistency + perf) | 7H+9M+3L fixed, 1H accepted-XREF, 1H deferred | scope: kernel-code-quality

---

## 4. argv Array: Kernel Storage & Shell Parsing

- [x] `task_set_argv(t, argc, argv)` (`env.c`): deep-copies each string via the env string allocator (so `env_free` reclaims argv), serialized on `environ_lock`, unwinding on failure; caps at `ARG_ARGC_MAX` (511)
- [x] `task_set_argv` builds the `argc+1` pointer array + `env_strdup`'d strings + `argv[argc]=NULL` BEFORE freeing the old argv; SYS_EXEC checks its return
- [x] On task exit / `env_free`: `kfree` each argv string + the pointer array (already in `env_free`)

- [x] `cmd_tokenize(line, argv, max_argc)` (`user/lib/stdlib.c`, libc) -- in-place Windows-quoting tokenizer:
  - whitespace separates tokens; `"quoted"` groups + strips quotes; spaces inside quotes preserved
  - `""` inside a quoted run → one literal `"`; `2n` backslashes + `"` → `n` + toggle-quote, `2n+1` → `n` + literal `"`
  - returns argc; `argv[argc]=NULL`
- [x] `cmd_tokenize` used by the shell (`user/cmd.c` replaced whitespace-only `parse`) before `sys_exec(path, argv, envp)`
- [x] Tokens beyond `max_argc-1` dropped (shell passes `ARGV_MAX`=32); dropped silently (pure libc fn, no logging surface)

- [x] `SYS_EXEC(path, argv[], envp[])` (clean ABI: `arg1=path`, `arg2=argv`, `arg3=envp`; both callers updated, NO probe-based `len`-vs-pointer inference):
  - `ProbeForRead` every `argv[]`/`envp[]` pointer + string; NULL argv/envp allowed (inherit / no-arg)
  - `exec_snapshot_vec`: `copy_from_user` the pointer vectors then each string EXACTLY ONCE into a kernel bounce buffer before any use
  - Bounded ingestion (reject BEFORE allocating): cap `argc` (`ARG_ARGC_MAX`), each string (`ARG_STRING_MAX` 4 KiB), aggregate (`ARG_MAX`); AND the EXACT serialized argv frame (`argv_frame_bytes`, qword-rounded strings + parity pads + ptr array, matching the builder) + `ARGV_FRAME_RESERVE` MUST fit `USER_STACK_SIZE` (16 KiB) or reject
  - Reject exec when `num_threads > 1` (fail-closed; fork-then-exec child is single-threaded) -> XREF `03-memory-concurrency/TODO-02-memory-security.md §4`
  - `task_set_argv` + (if `envp != NULL`) `env_adopt_block`, both return-checked, BEFORE `task_exec` (a later `task_exec` failure returns -1 and the child exits, so no surviving process sees half-updated state); else inherit environ
  - Residual: `copy_from_user` is range-checked but NOT fault-recoverable -- kernel-wide usercopy gap -> XREF `03-memory-concurrency/TODO-02-memory-security.md §4`
  - `task_exec` frame builder reads `task->argv`, pushes argv (16-byte `&argc` alignment), FAILS the exec on unfit/OOM (never silently diverges from the PEB); `crt0` loads `argc`/`argv` into RDI/RSI for `main`
- [x] `argv_to_cmdline(argc, argv, out, max)` **encode** (Windows quoting rules, `src/kernel/env.c`) wired into the PEB `CommandLine` builder (bounded to the RTLPP page) so `GetCommandLineW()` (§6) reflects full argv; decode + round-trip owned by §15
- [ ] Follow-up: make SYS_EXEC argv/env commit transactional -- stage `task_set_argv`/`env_adopt_block` inside `task_exec` at its no-return point (after `exec_load_fmt`) so a loader failure leaves old argv/env intact; terminate on post-commit OOM
- [ ] Follow-up: give `argv_to_cmdline` a Windows program-name encoder for `argv[0]` (today it over-escapes argv[0], so an embedded-quote argv[0] does not round-trip through §15's special argv[0] decode) -> XREF `02-kernel-core/TODO-22 §15`

- [x] Commit: `"kernel/env: argv array in task, shell tokenizer, exec argument handoff"`

**Test checkpoint:** `test_process.exe` execs `hello.exe` with `{alpha,beta}`; hello walks argv and returns `40+argc`=43, proving end-to-end delivery. QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-abi-tests.bat` (SUITE=abi) -- 5 argv/exec cases + `scripts\debug\usermode\run-test_libc.bat` cmd_tokenize + `run-test_process.bat` argv-delivery | 0 failures
> **Notes:**
> - Shipped: `task_set_argv`/`env_adopt_block`/`argv_to_cmdline`/`argv_frame_bytes`/`exec_snapshot_vec`/`SYS_EXEC(path,argv,envp)` (`env.c`,`syscall.c`); argv frame builder + PEB CommandLine encode (`task.c`); `cmd_tokenize`/`crt0` (`user/`).
> - Design + adversarial review adopted: clean SYS_EXEC ABI, exact argv-frame-fits-stack cap, `num_threads>1` reject, crt0 argv delivery, return checks, OOM-fails-not-diverges, page-bounded CommandLine; evidence in commit.
> - Adjacent fork bug fixed: `task_fork` never set `threads[0].base_priority`, so the first `mutex_unlock` a forked-then-exec'd task did (`task_set_argv`) reset priority to 0 and starved it (exec hung); now mirrors `task_create_*`.
> - Canonical doc: [include/kernel/env.h](../../include/kernel/env.h) (argv/env API) + the `task.c` frame builder.
> - Scope boundary: §6 owns `GetCommandLineW/A`; §15 owns `CommandLineToArgvW` decode + round-trip; envp -> PEB Environment PAGE owned by TODO-11 §21; the non-recoverable `copy_from_user` gap by TODO-02 §4.
> **Verified:** 2026-07-13 | commit `bda8324f` | 8/9 items | build OK | 20644 kernel + 16 user tests PASS, smoke PASS
> **Accepted:** [H] SYS_EXEC signature change is invisible to the ABI fingerprint (hashes SYS_* numbers, not signatures; theoretical stale-binary handshake bypass in the monolithic build) -> XREF: 00-infrastructure/TODO-04 §18 (item: "Fold syscall arg counts into the ABI fingerprint" at line 650)
> **Accepted:** [H] fork does not copy the parent environ, so exec(envp==NULL) inherits empty; race-safe env_copy needs atomic slot publication -> XREF: 02-kernel-core/TODO-12-native-api-ssdt.md §7 (item: "Wire `env_copy()` into every child path")
> **Accepted:** [M] PEB CommandLine truncates a >~2 KiB full-argv command line + UTF-8 argv mojibakes (single-page RTLPP, byte-widening) -> XREF: 02-kernel-core/TODO-11 §5 (item: "`CommandLine` fidelity" at line 197)
> **Accepted:** [M] `copy_from_user` is not fault-recoverable (in-range unmapped page faults in kernel) -> XREF: 03-memory-concurrency/TODO-02 §4 (item: "Audit all syscall handlers" at line 132)
> **Deferred:** [M] full exec-commit transactionality (roll back / terminate on a task_exec failure after the argv/env commit) -> XREF: 02-kernel-core/TODO-22 §4 (item: "Follow-up: make SYS_EXEC argv/env commit transactional" at line 233)
> **Quality reviewed:** 2026-07-13 | Codex 5x (adversarial, consistency, perf, re-adversarial) | 1H+1L fixed, 2H+2M accepted-XREF, 1M deferred | scope: kernel-code-quality

---

## 5. Nt/Zw Environment Variable Syscalls

- [x] `NtQueryEnvironmentVariable(Name, Value, ValueLength)` -- `nt_env.c` (SSDT `0x03DD`): reads `task->environ` via one locked `env_get_copy`; `STATUS_VARIABLE_NOT_FOUND` / `STATUS_BUFFER_TOO_SMALL`; sizes in bytes excl NUL (see Notes).
- [x] `NtSetEnvironmentVariable(Name, Value)` -- `nt_env.c` (SSDT `0x03DE`): `Value==NULL` deletes; updates only the kernel-authoritative `task->environ`. Optional PEB-block raw re-sync deferred to §6 (no post-startup reader).
- [x] SSDT wiring -- `0x03DD`/`0x03DE` in `service_numbers.h` (not firmware `0x00D2`..`0x00D6`); `nt_env_register_ssdt()` in the boot-halt gate; `PLEDGE_REQ_CORE`; ABI regenerated. No `Zw*` alias (mode via `ssdt_previous_mode()`).

- [x] Commit: `"kernel/env: NtQueryEnvironmentVariable, NtSetEnvironmentVariable SSDT wiring"`

**Test checkpoint:** `bash scripts/test.sh SUITE=abi` reports every `Env: Nt*` case PASS (roundtrip, not-found, delete, buffer-too-small, exact-fit, embedded-NUL/empty-name reject, overlap+NULL reject, empty-value NULL-buffer reject) via `ssdt_dispatch` (also proves boot-time registration); SSDT uses new indices `0x03DD`/`0x03DE`, not `0xD2`..`0xD6`. QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-abi-tests.bat` (SUITE=abi) | 1343 suites, 0 failures

> **Notes:**
> - **What shipped** -- `src/kernel/nt/nt_env.c` (~410 lines) + `include/kernel/nt/nt_env.h`: `NtQueryEnvironmentVariable` (0x03DD) + `NtSetEnvironmentVariable` (0x03DE) over the kernel-authoritative `task->environ`, plus `nt_env_register_ssdt()`.
> - **How it integrates** -- registered through the `boot_desktop.c` failure-counted boot-halt gate; pledge-classified `PLEDGE_REQ_CORE`; ABI numbers/hash regenerated by `scripts/gen-user-abi.py`.
> - **Boundary hardening** -- embedded-U+0000 reject (Name+Value), single-acquire `env_get_copy` (no size-then-copy race), output-region overlap reject, UTF-8 byte limits, `Length` excl NUL with NUL only when the buffer has spare room, and a NULL output `Buffer` rejected whenever a terminator is still owed (empty stored value: no memcpy-through-NULL on the KernelMode/Zw path -- Codex adversarial fix).
> - **Downstream effects** -- unblocks §6 Win32 wrappers and §19 `RtlQueryEnvironmentVariable_U`; design-review adoptions in the section commit message.
> - **Scope boundary** -- §5 owns the NT-boundary syscalls over `task->environ`; §6 owns the Win32 wrappers + the optional PEB-block raw re-sync; firmware env vars (0x00D2-0x00D6) stay with `uefi_runtime.c`.

> **Verified:** 2026-07-13 | commit `ec1df5dd` | 3/3 items | build OK | tests 1343/1343 PASS, smoke PASS (boot 3.18s)
> **Accepted:** [C] range-only `ProbeForWrite`/`ProbeForRead` + non-fault-recoverable `copy_to_user`/`copy_from_user` is a kernel-crash / kernel-write exposure for the ring-3 path (systemic to every Probe + `copy_*_user` syscall, incl. the reviewed `NtQueryCurrentDirectory`; not new in this class). -> XREF: `02-kernel-core/TODO-23-exception-dispatch-seh.md §13` (item: "`src/kernel/probe.c` -- implementation; `safe_return_rip` slot in CPU-local area" at line 392) -- re-enters when fault-recoverable `try_copy_*_user` lands.
> **Accepted:** [M] name/value size limits are enforced in UTF-8 BYTES (`ENV_NAME_MAX`/`ENV_VALUE_MAX`, matching the UTF-8 storage layer), so a UTF-16 input within the Windows CHARACTER limit but over the byte cap is cleanly rejected (`STATUS_NAME_TOO_LONG`), not corrupted. -> XREF: §10 (item: "reconcile UTF-16 character-count limits with UTF-8 storage byte caps").
> **Accepted:** [H] aggregate 1 MiB per-process env quota not enforced; §5 makes it user-reachable via `NtSetEnvironmentVariable` (env is already bounded to ~16 MiB/process by `ENV_MAX_ENTRIES`, so not unbounded) (RESOLVED 2026-07-14 by §10 commit `90a3b1fa`: `env_set`/`env_adopt_block`/`env_parse_block` reject a total block over `ENV_BLOCK_MAX` (1 MiB) with `ENV_ERR_NOSPACE`, klog warns once past 256 KiB) -> XREF: `02-kernel-core/TODO-22-environment-variables.md` §10 (item: "Per-process block-size DoS cap" at line 419)
> **Accepted:** [M] user-reachable env syscalls add a per-call caller to the unlocked `pmm_alloc_contiguous` for values > 4 KiB (mitigated: query/set now size to the value, so only genuinely-large values hit PMM) -> XREF: `03-memory-concurrency/TODO-03-advanced-allocator.md` §1 (item: "PMM bitmap SMP locking" at line 103)
> **Accepted:** [L] user-mode `ProbeForWrite`/`copy_*_user` branches are unit-tested only via KernelMode `ssdt_dispatch` (user pages are awkward in-kernel) -> XREF: `02-kernel-core/TODO-22-environment-variables.md` §6 (item: "`GetEnvironmentVariableW` ... calls `NtQueryEnvironmentVariable` directly")
> **Quality reviewed:** 2026-07-13 | Codex 4x (adversarial, consistency, perf, re-adversarial) | 2M fixed, 1H+1M+1L accepted-XREF | scope: kernel-code-quality

---

## 6. Win32 API Wrappers

- [/] `GetEnvironmentVariableA(lpName, lpBuffer, nSize)`:
  - Convert `lpName` to UTF-16; call `NtQueryEnvironmentVariable`; convert UTF-16 result back to UTF-8 into `lpBuffer`
  - Return character count on success; if `nSize` too small, return required size and `SetLastError(ERROR_INSUFFICIENT_BUFFER)`
- [/] `GetEnvironmentVariableW(lpName, lpBuffer, nSize)` -- calls `NtQueryEnvironmentVariable` directly with UTF-16 `lpBuffer`
- [/] `SetEnvironmentVariableA/W(lpName, lpValue)`:
  - `lpValue == NULL` → delete the variable
  - Call `NtSetEnvironmentVariable`; map `STATUS_*` to `ERROR_*` via `RtlNtStatusToDosError`; return `TRUE` / `FALSE`
- [/] Optional PEB-block re-sync (← XREF §5 `NtSetEnvironmentVariable`): rebuild `PEB->ProcessParameters->Environment` from `task->environ` after a set/delete for raw-block readers. Compat nicety -- in-tree readers use `task->environ`.

- [/] `ExpandEnvironmentStringsA(lpSrc, lpDst, nSize)`:
  - Convert `lpSrc` to UTF-16; call `RtlExpandEnvironmentStrings_U` (same section, RTL helper above); convert UTF-16 result to UTF-8 into `lpDst`
  - Return bytes written (including null); if `nSize` too small, return required size (caller must retry)
- [/] `ExpandEnvironmentStringsW(lpSrc, lpDst, nSize)` -- calls `RtlExpandEnvironmentStrings_U` directly
- [/] User-supplied UTF-16 `Environment` probe+copy into a kernel snapshot (then `rtl_env_expand_block` with its extent) is owned by §24 (item: "**Boundary probe+copy for a non-NULL `Environment`**"); §6 consumes it. NULL needs no probe
- [ ] Route the env wrappers through §21's Rtl exports (`RtlQueryEnvironmentVariable_U`/`RtlSetEnvironmentVariable`), NOT `Nt*` directly: that is Win11's own kernel32->ntdll layering, and it is what resolves ntdll imports → XREF §21
- [x] Bound RtlExpandEnvironmentStrings_U scan cost before user exposure: closed by §20's per-pass `RTL_ENV_EXPAND_WORK_MAX` work budget (`STATUS_INSUFFICIENT_RESOURCES`), which bounds the O(refs x block) product for every caller → XREF §20

- [/] `GetEnvironmentStringsW()`:
  - Walk `current_task->environ[]`; convert each `"KEY=VALUE"` to UTF-16; pack into a contiguous buffer as null-separated entries with a double-null at the end (matches the Win32 format); allocate with `LocalAlloc`
  - Return pointer; caller must call `FreeEnvironmentStringsW` when done
- [/] `GetEnvironmentStringsA()` -- UTF-8 variant; same format in ANSI
- [/] `FreeEnvironmentStringsW(pEnvBlock)` → `LocalFree(pEnvBlock)`

- [/] `GetCommandLineW()` → returns `PEB->ProcessParameters->CommandLine.Buffer` (UTF-16 command line string, built by `TODO-11-peb-teb-user-abi.md §7` from `task->argv`)
- [/] `GetCommandLineA()` → convert `CommandLine.Buffer` UTF-16 → UTF-8 and cache in a static per-process buffer (allocated on first call)

- [/] Commit: `"kernel/env: GetEnvironmentVariable, SetEnvironmentVariable, ExpandEnvironmentStrings, GetCommandLine Win32 wrappers"`

> **Deferred:** [blocked] §6 Win32 env/command-line wrappers need user-mode kernel32/ntdll runtime primitives that do not exist yet -- `LocalAlloc`/`LocalFree`, `SetLastError`/`GetLastError`, and UTF-8<->UTF-16 conversion (`MultiByteToWideChar`/`WideCharToMultiByte`). The PEB CommandLine prereq is CLOSED (TODO-11 §7 shipped `[x]`; §4 wires `argv_to_cmdline` into the builder at `src/kernel/sched/task.c:2177-2184` via `peb_build_ustr(&pp->CommandLine, ...)`), so `GetCommandLineW` now waits only on the runtime primitives above. The syscall layer (§5) is shipped; these are user-platform runtime prereqs owned elsewhere. -> XREF: `12-user-platform-sdk/TODO-04-ntdll-user-runtime.md` §7 (item: "kernel32 conversion + last-error shims") + §2 (item: "RtlHeap Process Heap Allocator" at line 171).

**Test checkpoint:** `GetEnvironmentVariableW` returns `SYSTEMROOT`; `ExpandEnvironmentStringsW` expands. QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 7. Shell Integration: PATH Lookup & SET/ECHO

- [ ] `shell_find_command(name, out_path, max)` -- one shared extension rule (matches SDK `TODO-02 §5`): a `name` WITH an explicit extension is probed verbatim only; an extensionless `name` iterates `PATHEXT` in order (§11):
  1. If `name` contains `\` or `/`: explicit path; probe verbatim if it has an extension, else iterate `PATHEXT` against it; return first match
  2. Otherwise: retrieve `PATH` value via `env_get(current_task, "PATH")`
  3. Split `PATH` on `;` into directory list; get `PATHEXT` via `env_get(current_task, "PATHEXT")` (split `;`; empty/unset -> `.EXE`)
  4. For each directory: if `name` has an extension probe `dir\name` verbatim, else `dir\name{ext}` for each `PATHEXT` ext in order; `vfs_stat` each; return first hit
  5. If no PATH match: consult `app_paths_lookup(caller, name, ...)` (§17 App Paths fallback, matches `ShellExecute`); return its result before `SHELL_COMMAND_NOT_FOUND` → XREF §17
- [ ] Shell uses `shell_find_command` before any `exec` call; replaces current ad-hoc path construction

- [ ] Before executing any command, the shell calls `env_expand(current_task, raw_cmdline, expanded, sizeof expanded)` on the full command line
- [ ] This makes `echo %SYSTEMROOT%`, `cd %TEMP%`, and path arguments with env vars work transparently before the command is parsed into argv

- [ ] `SET` with no args: print all environment variables, one per line in `KEY=VALUE` format, sorted alphabetically
- [ ] `SET VAR=VALUE`: call `env_set(current_task, "VAR", "VALUE")`; print nothing on success
- [ ] `SET VAR=` (empty value): call `env_set(current_task, "VAR", "")`; effectively creates an empty variable (distinct from unset)
- [ ] `SET /A VAR=EXPR`: arithmetic expansion (integer expressions using `+`, `-`, `*`, `/`, `%`, `<<`, `>>`, `&`, `|`, `^`); evaluate with a simple recursive descent parser; store result as decimal string
- [ ] `ECHO %VAR%`: `env_expand` already handles this before argument tokenization; ensure `ECHO` prints the expanded value
- [ ] `SET /?`: print usage summary

- [ ] Commit: `"shell: PATH lookup, SET command, %VAR% expansion in all commands"`

> **Deferred:** [blocked] §7 shell PATH lookup + SET/ECHO -- the shell is user-mode (`user/cmd.c` cmd.exe), so these builtins need §6 user-mode env wrappers (deferred) or env-syscall stubs; the spec's kernel-side `env_get(current_task)`/`env_expand(current_task)` assume a kernel shell that does not exist (`src/shell/` is empty). -> XREF: §6 (Win32 env wrappers, deferred) + `12-user-platform-sdk/TODO-02-env-vars-process-abi.md` §8 (item: "`set`/`echo`/`env`/`where` commands").

**Test checkpoint:** `shell_find_command` finds binary on PATH; `SET` lists sorted vars. QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 8. `.profile` Startup Script

- [ ] On shell startup (after default env is initialized):
  1. Compute profile path: `env_expand(..., "%USERPROFILE%\\.profile", ...)`
  2. `vfs_stat(profile_path)` -- if not found, skip silently; no error
  3. If found: `vfs_open` + read line-by-line (max 4 KiB buffer per line)
  4. Execute each non-blank, non-comment line as a shell command (same code path as user-typed input); errors are logged to serial but do not abort the rest of the script
- [ ] Comment lines: any line beginning with `#` (after optional leading whitespace) is skipped
- [ ] Maximum lines: 1 000; lines beyond limit are skipped with a warning

- [ ] `resources/defaults/.profile` file included in the ISO/image, copied to `C:\Users\Default\.profile` during OS install / first boot:
  ```
  # Impossible OS default user profile
  # Edit this file to customise your shell environment.

  SET PATH=%PATH%;C:\Users\%USERNAME%\Bin
  SET EDITOR=notepad.exe
  SET PAGER=more.exe
  ```
- [ ] The build system (Makefile / build.sh) copies this file into the IXFS root partition during `make install-userfiles`

- [ ] `source <file>` (or `.  <file>` POSIX synonym) -- execute a script file in the current shell's context (not a child process); env changes in the script affect the current shell session
- [ ] Used to manually reload `.profile` after editing: `source C:\Users\Default\.profile`

- [ ] Commit: `"shell: .profile startup script, source command, default profile in image"`

> **Deferred:** [blocked] §8 `.profile` startup depends on §7 shell integration (deferred) plus the user-mode shell startup hook and a `make install-userfiles` copy of `resources/defaults/.profile` into the IXFS root. -> XREF: §7 (shell integration, deferred) + `12-user-platform-sdk/TODO-02-env-vars-process-abi.md` §8 (item: "`set`/`echo`/`env`/`where` commands" -- same user-mode shell surface).

**Test checkpoint:** `.profile` lines run at shell start; `source` reloads. QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 9. Environment Change Notifications

- [ ] **`SetEnvironmentVariable` is process-local:** modifies ONLY the caller's own block (§5/§6); never writes Registry, never broadcasts -- persistence + notification are exclusive to `setx` / `sysdm.cpl` (MS Learn).
- [ ] After a Registry write via `setx` / `sysdm.cpl` (not `SetEnvironmentVariable`), post `WM_SETTINGCHANGE` (`0x001A`, `lParam=L"Environment"`) to `HWND_BROADCAST`; apps refresh via `SendMessageTimeout` with `SMTO_ABORTIFHUNG`.
- [ ] WM delivery requires the window manager to be running; if WM is not yet started (early boot), skip silently
- [ ] Add "Environment Variables" button to `sysdm.cpl` (System Properties): dialog with System (admin) and User list-views; Edit/New/Delete writes Registry + `WM_SETTINGCHANGE`
- [ ] Dialog layout matches Windows 11's Environment Variables dialog for user familiarity
- [ ] `setx VAR VALUE` -- set an environment variable **persistently** (writes to `HKCU\Environment` via Registry API + triggers `WM_SETTINGCHANGE`); current session not affected (matches Windows `setx.exe` behaviour)
- [ ] `setx VAR VALUE /M` -- write to system-wide `HKLM\...\Session Manager\Environment`; requires admin token (→ XREF `TODO-15-security-reference-monitor.md §9`)
- [ ] `setx /?` -- print usage
- [ ] **`setx` value length (Win11 parity):** Microsoft documents a **1024-character** cap on the value assigned by `setx`; excess is truncated and can corrupt an existing variable; implement the same cap (or emit a hard error instead of silent truncate) and mention it in `setx /?` (see Microsoft Learn `setx` Remarks).

- [ ] Commit: `"kernel/env: WM_SETTINGCHANGE broadcast, sysdm.cpl env tab, setx command"`

> **Deferred:** [blocked] §9 env-change notifications + `sysdm.cpl` tab need §6 (deferred), Registry env persistence (`setx`), the window manager for the `WM_SETTINGCHANGE` `HWND_BROADCAST`, and a System Properties control-panel applet (desktop-shell domain). -> XREF: §6 (Win32 env wrappers, deferred) + §8 (`.profile`, deferred) + `09-desktop-shell` (System Properties / control panel applet).

**Test checkpoint:** Registry write-back + `WM_SETTINGCHANGE` when WM up; `setx` persists. QEMU WHPX, QEMU TCG (headless may skip WM); VirtualBox; bare metal.

---

## 10. Environment Block Sorting & Size Limits

- [x] `environ[]` kept **sorted by name** (case-insensitive) always so `GetEnvironmentStrings` sees alphabetical order; one comparator `env_name_cmp` (`src/kernel/env.c`) is the sole authority for identity AND order
- [/] Full NLS Unicode name upcasing. Deferred: `nls_upcase_char` U+0100+ is disk-backed/ephemeral, corpus unshipped. Stable ASCII fold ships; non-ASCII compares ordinally (→ XREF `TODO-13-atom-nls-locale-subsystem.md` §10 full-BMP corpus)
- [x] `env_set` maintains sorted order via binary-search insert + tail shift (`env_bsearch`); a same-name replace keeps position. `env_find_index`/`get_copy`/`peek`/`unset` route through `env_bsearch`
- [x] `env_build_block(t, out, max_len, is_unicode, *out_len)` -- contiguous block into caller buffer: `name=value\0`...`\0` (ANSI=UTF-8) or UTF-16 `...\0\0`; sorted; `*out_len`=required bytes; `ENV_ERR_NOSPACE` (no partial write) if too small
- [x] CREATE_UNICODE_ENVIRONMENT flag decision stays in the process-creation caller (→ XREF `TODO-12-native-api-ssdt.md §7`, `TODO-21-process-model-extensions.md §2`); env just supplies the block
- [x] `env_parse_block(t, block, len, is_unicode)` -- decode block and REPLACE `environ[]` via atomic `env_adopt_block`; validates + sorts + de-dups (last-wins); ANSI or UTF-16 (converted to UTF-8)
- [x] Max value length `ENV_VALUE_MAX`=32767 bytes; `env_set` returns `ENV_ERR_TOOLONG` (§5 maps `STATUS_NAME_TOO_LONG`)
- [x] Max name length `ENV_NAME_MAX`=256 bytes; `env_name_classify` rejects longer names
- [x] Reconciled UTF-16 char vs UTF-8 byte caps (← XREF §5): documented byte-cap contract in `include/kernel/env.h` (one byte cap, applied after the NT layer's UTF-16→UTF-8 conversion)
- [x] Name validation: `env_name_classify` rejects `=`-containing names EXCEPT the hidden `=X:` drive-cwd shape (shipped §12); empty/NULL rejected
- [x] Per-process block-size DoS cap: `env_set`/`env_adopt_block`/`env_parse_block` reject a total block over `ENV_BLOCK_MAX` (1 MiB) with `ENV_ERR_NOSPACE`; klog warns once past `ENV_BLOCK_WARN` (256 KiB)
- [x] `REG_EXPAND_SZ` stored RAW during overlay then expanded once against the fully-assembled env in a deterministic post-overlay pass (`env_expand_reg_values`), never inline during `RegEnumValue`
- [x] Commit: `"kernel/env: sorted environment block, size limits, name validation"`

**Test checkpoint:** `GetEnvironmentStrings` order AAA before ZZZ; oversize name rejected. QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-abi-tests.bat` (SUITE=abi) | 1653 suites, 0 failures (s10 build/parse/sort/cap/quota cases)

> **Notes:**
> - Shipped in `src/kernel/env.c` + `env.h`: sorted `environ[]` (`env_name_cmp`/`env_bsearch`, merge sort in adopt), caller-buffer `env_build_block`/`env_parse_block` (ANSI+UTF-16), 1 MiB block cap, two-phase `REG_EXPAND_SZ` expansion.
> - Adoptions (detail in commit messages): value pointers derive from the stored `=`; ASCII-only stable fold over ephemeral NLS; O(n log n) adopt sort; O(1) block cap via cached byte total.
> - New `test_env_*` cases (sort, build/parse ANSI+Unicode, dedup, malformed, 1 MiB cap, over-value skip, oversize reject, unset-reclaims, empty-adopt quota) under SUITE=abi; build+test+smoke green.
> - Canonical doc: byte-cap contract + block-cap rationale in the `include/kernel/env.h` header comment.
> - Scope: full NLS folding is TODO-13's (deferred `[/]`); child-env `env_copy` wiring is solely TODO-12 §7 (TODO-21 §2 is standard-handle pre-wiring, not inheritance); the NULL-env RtlExpand 128 KiB cap is §19's.

> **Verified:** 2026-07-14 | commit `90a3b1fa` (+review) | 11/12 items | build OK | tests 1653/1653 abi PASS + smoke PASS
> **Accepted:** [M] NULL-environment `RtlExpandEnvironmentStrings_U` caps at 128 KiB while storage allows 1 MiB (raising it safely needs a uint64 expansion-length count) -> XREF: 02-kernel-core/TODO-22 §19 (item: "Raise the NULL-env expansion cap above 128 KiB" at line 435)
> **Quality reviewed:** 2026-07-14 | Codex 7x (design + adversarial + consistency + perf + re-adversarial) | 2H+4M+1L fixed, 1M accepted-XREF, 1 rejected | scope: kernel-code-quality

---

## 11. PATHEXT Variable & Extension Search Order

- [x] Add `PATHEXT` to system default variables (§2): seeded `.EXE` only (`ENV_DEF_PATHEXT` in `env.c`); design review trimmed `.EXE;.CMD;.BAT` since `exec.c` runs only PE/ELF/EIF (see commit msg)
- [/] Modify `shell_find_command` (§7) to use `PATHEXT`. Deferred: §7 deferred (`src/shell/` empty). -> XREF: §7 + `12-user-platform-sdk/TODO-02-env-vars-process-abi.md` §5 (item: "Extension precedence" at line 183)
- [/] Current search order (before PATHEXT): tries only `.exe`; too restrictive. Deferred with the §7 consumer above (no shell lookup exists to widen yet).
- [/] `PATHEXT` with empty value: fall back to `.EXE` only (Windows behavior). Deferred: empty-value semantics belong to the §7 lookup consumer; the seeded default is already `.EXE`.

- [x] Commit: `"kernel/env: PATHEXT extension search order for PATH-based command lookup"`

**Test checkpoint:** `env_init_defaults` seeds `PATHEXT=.EXE`; the `.cmd`-before-`.exe` ordering test lands with the §7 shell consumer. QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-abi-tests.bat` (SUITE=abi) | 1656 suites, 0 failures, 0 leaked (PATHEXT default assertion in `test_env_defaults_derived` + `test_ntenv_live_env_hwm` prewarm)

> **Notes:**
> - **What shipped:** `ENV_DEF_PATHEXT` (`.EXE`) seeded in `env.c` `env_synth_base`; one `test_env_defaults_derived` assertion; a `test_ntenv_live_env_hwm` prewarm suite absorbing the live-task `environ[]` high-water-mark grow.
> - **How it integrates:** the default lands in PID 0's env today (`env_init_kernel_task`); children reach it only after TODO-12 §7 wires `env_copy` before publish; `env_get(t, "PATHEXT")` returns `.EXE` until the §7 shell consumer iterates it.
> - **Design review:** trimmed value to `.EXE` (Codex, adoptions in commit msg) because `exec.c` has no `.CMD`/`.BAT` loader; fuller Windows default waits on a batch processor.
> - **Scope boundary:** §11 owns only the default var; the `shell_find_command` PATHEXT consumer is owned by §7 (deferred) + `12-user-platform-sdk/TODO-02` §5 (Extension precedence).

> **Verified:** 2026-07-14 | commit `7a8842a8` | 1/4 items | build OK | tests abi 1656/1656 PASS
> **Accepted:** [M] session-cache CWD + VFS-generation coherency + `where` cache-free enumeration (reason: pre-existing SDK cache design, surfaced reviewing PATHEXT invalidation) -> XREF: 12-user-platform-sdk/TODO-02 §5 (item: "Cache coherency" at line 180)
> **Quality reviewed:** 2026-07-14 | Codex 8x (design + adversarial + consistency + perf + re-adversarial) | 7M fixed, 1M accepted-XREF, 1M rejected | scope: kernel-code-quality

---

## 12. Hidden Drive-Letter Variables (`=C:`, `=D:`)

- [x] Hidden `=X:` drive vars live in the sorted `task->environ` (entry `=C:=C:\path`, leading `=` is the name); `env_entry_keylen`/`env_name_classify`/`env_entry_key_span` accept ONLY the 3-byte `=<A-Z>:` shape (`env.c`)
- [x] `env_set_drive_cwd(task, char drive, const char *path)` -- builds `=X:` (uppercased) + `env_set`; non-letter drive / NULL path → `ENV_ERR_INVAL` (`src/kernel/env.c`, `include/kernel/env.h`)
- [x] `env_get_drive_cwd(task, char drive, char *out, uint32_t out_size)` -- COPY-OUT (design review: no borrowed pointer); returns value length, fills `X:\` root and succeeds when the drive is unset
- [x] `NtSetCurrentDirectory_handler` commits `env_set_drive_cwd`+`task_set_cwd` as ONE txn under a per-task sleeping `chdir_lock` (adversarial: closes same-drive divergence); env-first OOM → `STATUS_NO_MEMORY`, cwd unchanged
- [x] Drive-relative resolution: `task_resolve_path_for` resolves `X:tail`/bare `X:` from cwd (drive match), else remembered `=X:`, else `X:\`; oversized/foreign/nested-qualifier (`D:C:\`) tails FAIL CLOSED; `X:\tail` absolute
- [/] Process inheritance: NULL `lpEnvironment` inherits `=X:` ONCE `env_copy` is wired fail-closed pre-publish in every constructor (`task_fork`, `task_create`, `task_create_user`) -- no live caller. Storage READY

- [x] Hidden `=X:` variables sort before regular variables because `=` (0x3D) sorts before any letter (A=0x41); they appear at the front of the environment block (existing sorted store + `env_build_block` emit them first)
- [x] `env_build_block` (§10) includes hidden drive vars in the sorted output (leading-`=` names emitted verbatim; `test_env_drive_cwd_sorts_first` asserts `=C:` before `AAA`)

- [x] Commit: `"kernel/env: hidden =X: drive-letter current directory variables"`

**Test checkpoint:** `env_set_drive_cwd(t,'C',"C:\Users")` round-trips via `env_get_drive_cwd`/`env_get_copy(t,"=C:")`; unset drive → `X:\`; hidden `=C:` sorts before `AAA`; a custom block carrying `=C:` survives `env_parse_block`; only the `=X:` shape is a legal `=`-name; the drive-relative matrix (current/other/unset/absolute/bare/dot-dot) resolves and an oversized `=X:` fails closed. QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-abi-tests.bat` (SUITE=abi) | 8 `Env: *drive*/=X:*` suites, 0 failures

> **Notes:**
> - Shipped: `env_set_drive_cwd`/`env_get_drive_cwd` + `env_name_is_drive_cwd`/`env_entry_key_span` (`env.c`/`env.h`); `=X:` stored in the same sorted `environ`, so sort/block/inherit reuse existing code with no builder change.
> - Consumer: `task_resolve_path_for` (new explicit-task form of `task_resolve_path`) resolves drive-relative `X:tail`/bare `X:`; `NtSetCurrentDirectory` updates `=X:` under a per-task `chdir_lock` transaction (env-first, failure-atomic).
> - Review adoptions (copy-out getter, drive-relative consumer, chdir_lock divergence fix, fail-closed on oversized AND foreign-drive `=X:`) in the commit messages.
> - Scope: §12 owns `=X:` storage + resolution; child-creation auto-inheritance is TODO-12 §7 (env_copy wiring); the `mutex_unlock` SMP orphan race is TODO-08 §11 (pre-existing primitive).
> - Test gap (accepted): the `NtSetCurrentDirectory` handler-level integration (env-OOM injection through the SSDT route + a live VFS dir) is serial-validation only; the pure resolver matrix + adapter boundaries are unit-tested (8 suites).
> **Verified:** 2026-07-14 | commit `0f0dab43` | 7/8 items | build OK | tests 21042/21042 PASS
> **Accepted:** [H] concurrent multi-thread `NtSetCurrentDirectory` is non-linearizable (path resolved against a cwd snapshot outside `chdir_lock`; pre-existing, and Windows documents `SetCurrentDirectory` as not thread-safe). `chdir_lock` keeps cwd + `=X:` mutually consistent -> XREF: `02-kernel-core/TODO-21-process-model-extensions.md` §1 (item: "Concurrent `NtSetCurrentDirectory` non-linearizable").
> **Accepted:** [H] the `chdir_lock` txn relies on `mutex_t`, whose `mutex_unlock` clears ownership after releasing `locked` (SMP orphan race shared by every contended per-task mutex incl. `environ_lock`) -> XREF: `03-memory-concurrency/TODO-08-advanced-sync.md` §11 (item: "Unlock ownership-clear ordering"). Pre-existing primitive defect; §12 uses the mutex identically to shipped `environ_lock`.
> **Accepted:** [H] automatic child inheritance of `=X:` needs `env_copy` wired fail-closed before publish in every child-creation path (owner elsewhere) -> XREF: `02-kernel-core/TODO-12-native-api-ssdt.md` §7 (item: "Wire `env_copy()` into every child path"). Storage is inheritance-ready; custom blocks already carry `=X:`.
> **Accepted:** [H] the legacy `SYS_OPENFILE` passes a RAW user pointer to `task_resolve_path`, so the nested-qualifier guard (and `vfs_resolve_path`'s own drive select) can be defeated by a concurrent user-buffer mutation (double-fetch). Pre-existing; the NT handlers already snapshot. The guard is sound for snapshotted callers -> XREF: `03-memory-concurrency/TODO-02-memory-security.md` §4 (item: "Audit all syscall handlers" at line 132).
> **Quality reviewed:** 2026-07-14 | Codex 18x (design, adversarial, test-coverage, consistency, perf, re-adversarial) | 12H fixed, 4 accepted-XREF | scope: kernel-code-quality

---

## 13. CreateEnvironmentBlock / DestroyEnvironmentBlock

- [/] `env_create_block()` -- Win32 `CreateEnvironmentBlock` (`src/kernel/env.c`): self-describing sorted UTF-16 block; `htoken==NULL && inherit` snapshots the caller env under `environ_lock`. Other branches return `ENV_ERR_UNSUPPORTED`.
- [x] `env_destroy_block()` -- Win32 `DestroyEnvironmentBlock`: pointer-only free via a hidden `{magic,wchars}` header (no kernel `LocalAlloc`); NULL is a no-op; called once with a live `env_create_block` pointer (Win32 contract).
- [/] `ExpandEnvironmentStringsForUser()` -- Win32 `ExpandEnvironmentStringsForUserW` (`nt_rtlenv.c`): `htoken==NULL` expands the caller block via `RtlExpandEnvironmentStrings_U`; `htoken!=NULL` returns `STATUS_NOT_SUPPORTED`.
- [x] **Profile-load policy honored:** a token needing HKCU vars without a loaded profile returns documented failure (`ENV_ERR_UNSUPPORTED`/`STATUS_NOT_SUPPORTED`); no profile path invented (MS Learn: user vars apply only after `LoadUserProfile`).
- [ ] Per-user block/expansion (`htoken!=NULL`): token-SID -> `HKEY_USERS` hive + `LoadUserProfile` + HANDLE `granted_access` check. -> XREF: 02-kernel-core/TODO-15 §5 (item: "Enforce per-handle `granted_access` on token mutation syscalls")
- [ ] `htoken==NULL` system-only / no-inherit fresh block: needs an SMP-safe runtime Registry snapshot (`env_init_defaults` is boot-only). -> XREF: 02-kernel-core/TODO-14 §14 (item: "Registry SMP synchronization")
- [ ] User-mode `userenv.dll` export wiring: kernel returns kernel-resident blocks today (like the unwired `RtlExpandEnvironmentStrings_U`); needs a user heap -> XREF: 12-user-platform-sdk/TODO-04 §2 (item: "`RtlAllocateHeap`")

- [x] Commit: `"kernel/env: CreateEnvironmentBlock, DestroyEnvironmentBlock, ExpandEnvironmentStringsForUser"`

**Test checkpoint:** `CreateEnvironmentBlock` + `DestroyEnvironmentBlock` no leak; sorted block. QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-abi-tests.bat` (SUITE=abi) | 7 new env s13 cases, 0 failures

> **Notes:**
> - **What shipped:** `env_create_block` / `env_destroy_block` (`src/kernel/env.c`, self-describing `{magic,wchars}`-header UTF-16 block) + `ExpandEnvironmentStringsForUser` (`src/kernel/nt/nt_rtlenv.c`); 6 tests in `test_env.c`.
> - **How it runs:** caller-env subset -- snapshots the caller `environ` under its mutex, no runtime Registry walk, no scratch task; a >4 KiB block shares the pre-existing unlocked-PMM exposure (owner: 03-memory-concurrency/TODO-03 PMM bitmap SMP locking). Design + adversarial review adoptions in the commit message.
> - **Downstream:** honors the profile-load policy (documented failure, no invented paths); per-user / registry-fresh / userenv-export paths deferred with XREFs to TODO-15 §5, TODO-14 §14.
> - **Canonical doc:** `include/kernel/env.h` (`env_create_block` contract) + `include/kernel/nt/nt_rtlenv.h`.
> - **Scope boundary:** §13 owns the caller-env block build/free (environ_lock snapshot) + NULL-token expand; per-user token identity is TODO-15 §5 + profile-load infra; runtime Registry snapshot is TODO-14 §14.
> **Verified:** 2026-07-15 | commit `b39fe250` | 2/7 items | build OK | tests 1727/1727 PASS
> **Accepted:** [M] UTF-16 `%=X:%` expansion cannot resolve hidden `=X:` drive vars (Rtl lookup uses the first `=` as separator) -> XREF: 02-kernel-core/TODO-22 §19 (item: "UTF-16 `%=X:%` expansion parity" at line 521)
> **Quality reviewed:** 2026-07-15 | Codex 6x (design, adversarial, re-adversarial, consistency, perf) | 7H+2M fixed, 0 open, 1M accepted-XREF | scope: kernel-code-quality

---

## 14. SearchPathW / SearchPathA Win32 API

- [x] `SearchPathW(caller, lpPath, lpFileName, lpExtension, nBufferLength, lpBuffer, lpFilePart)` -- kernel32 API wrapping a shared UTF-8 core `env_search_path` (`src/kernel/env_searchpath.c`):
  - `lpPath == NULL`: order is (1) current directory, (2) system dir `%SYSTEMROOT%\System32`, (3) Windows dir `%SYSTEMROOT%`, (4) `PATH` dirs. System/Windows dirs derive from the IMMUTABLE `ENV_SYSTEM32_DIR`/`ENV_SYSTEMROOT_DIR` kernel constants, NEVER the caller-mutable `SYSTEMROOT` env var (DLL-hijack hardening). Safe-search mode REORDERS the current directory (safe: after `PATH`; unsafe: before system dirs); it never removes it. A `PATH`-buffer allocation failure fails closed with `ERROR_OUTOFMEMORY` (never falls through to the CWD leg).
  - `lpPath != NULL`: search only its semicolon-delimited dirs.
  - A `lpFileName` that is already qualified (rooted `\`, drive `X:`, or explicitly CWD-relative `.\`/`..\`) bypasses directory iteration and is probed directly. An ORDINARY relative subpath (`plugins\tool.exe`) is searched BENEATH each leg; an unqualified name with a `..` component is REJECTED (`ERROR_INVALID_PARAMETER`), since it would canonicalize out of the selected directory.
  - Probes are caller-aware: each candidate resolves via `task_resolve_path_for(caller,...)` then `unveil_check(caller, ., UNVEIL_R)` under `pledge_user_mode()`; denied paths read as absent; only a regular file (not a directory) matches.
  - `lpExtension`: appended when the final path component has no extension; first char must be `.`; ignored when the name already has one; a separator- or drive-bearing extension is REJECTED (`ERROR_INVALID_PARAMETER`) so it cannot smuggle traversal past qualification.
  - `lpFilePart`: receives a pointer to the file-name component in the returned buffer (target-encoding units; offset 0 when no separator).
  - Return (target-ABI units -- UTF-16 words for W): length excl NUL on exact fit (buffer written); required chars INCL NUL when it will not fit (buffer left untouched); 0 if not found (`ERROR_FILE_NOT_FOUND` on TEB).
- [x] `SearchPathA` -- ANSI wrapper over the same UTF-8 core; lengths + `lpFilePart` in ACP bytes via `nls_cp_*_utf16` / `NLS_CP_ACP`; result identical to widening + `SearchPathW`.
- [x] `NeedCurrentDirectoryForExePathW(caller, ExeName)` -- returns `TRUE` if `ExeName` contains a backslash; else `FALSE` when the `NoDefaultCurrentDirectoryInExePath` env var is present (even empty), else `TRUE` (security hardening).
- [x] `SetSearchPathMode(caller, mode)`: validate `BASE_SEARCH_PATH_*` bits; `PERMANENT` needs `ENABLE`, locks later changes; per-task `search_path_mode` via `__atomic` CAS; governs CWD ORDER only (`ERROR_INVALID_PARAMETER`/`ERROR_ACCESS_DENIED`).
- [ ] Leg-1 (application-load directory) deferred: needs a kernel-owned canonical image path (PEB / `task->name` are caller-writable, unsafe); also unblocks §15 `CommandLineToArgvW("")` argv[0] -> XREF: `TODO-21-process-model-extensions.md §2`.
- [ ] `sp_probe` fails open on a trusted-leg VFS I/O/OOM error (treated as a miss -> falls through to PATH/CWD); once `vfs_stat` returns a tri-state found/absent/error result, make `sp_probe` abort the search on a hard error.
- [ ] Compose all install-path consumers from `ENV_SYSTEMROOT_DIR`/`ENV_SYSTEM32_DIR` (`ENV_DEF_PATH_BASE` done; still literal: bootstrap PATH `env.c` seed row + `registry.c` windir) and move the constants to a neutral install-path header.

- [x] Commit: `"kernel/env: SearchPathW/A Win32 API, NeedCurrentDirectoryForExePathW"`

**Test checkpoint:** `env_search_path` / `SearchPathA` finds a `vfs_create`d binary in `C:\Impossible\System32\`; a missing name returns 0 + `ERROR_FILE_NOT_FOUND`; a `\`-qualified name bypasses iteration; a duplicate name in CWD and System32 returns the System32 path under safe mode and the CWD path under unsafe mode (precedence, not participation); `SetSearchPathMode` rejects bad flags (`ERROR_INVALID_PARAMETER`) and locks after `PERMANENT` (`ERROR_ACCESS_DENIED`); A/W exact-fit returns length excl NUL and one-short returns required incl NUL with the buffer untouched. QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-abi-tests.bat` (SUITE=abi) | 1804 kernel suites, 0 failures (16 SearchPath/`SetSearchPathMode`/`NeedCurrentDir` cases)
> **Notes:**
> - **What shipped** -- `src/kernel/env_searchpath.c` + header: UTF-8 `env_search_path` core behind `SearchPathW`/`A`, `NeedCurrentDirectoryForExePathW`, `SetSearchPathMode`; new `struct task` `search_path_mode` field; 16 ABI tests.
> - **How it integrates** -- plain kernel functions taking `struct task *caller` (like `ExpandEnvironmentStringsForUser`); NOT SSDT/`pe.c` exports; probes reuse `task_resolve_path_for` + `unveil_check`; last-error to the executing thread's TEB.
> - **Security** -- trusted legs from immutable constants (not `%SYSTEMROOT%`); safe-search reorders (never removes) CWD; `..`/separator/OOM fail closed; PERMANENT lock is an `__atomic` CAS.
> - **Scope boundary** -- §14 owns SearchPath ordering; leg-1 (app-load dir) needs a kernel-owned image path owned by `TODO-21 §2`; PATH+PATHEXT shell iteration stays `shell_find_command` (§7).
> **Verified:** 2026-07-15 | commit `2953e5ce` | 4/7 items | build OK | tests 1804/1804 PASS
> **Accepted:** [H] SearchPathW non-ASCII CWD leg limited by the ASCII-only NT-path/cwd narrowing (`nt_process.c`) -> XREF: `02-kernel-core/TODO-21-process-model-extensions.md §1` (item: "Non-ASCII CWD: NtSetCurrentDirectory narrows..." at line 97)
> **Deferred:** [H] `sp_probe` fails open on a trusted-leg VFS I/O/OOM error (needs `vfs_stat` tri-state) -> XREF: `02-kernel-core/TODO-22-environment-variables.md §14` (item: "`sp_probe` fails open on a trusted-leg VFS..." at line 555)
> **Deferred:** [M] install-path consumers (`registry.c` windir, bootstrap PATH) not composed from the constants -> XREF: `02-kernel-core/TODO-22-environment-variables.md §14` (item: "Compose all install-path consumers..." at line 556)
> **Quality reviewed:** 2026-07-15 | Codex 14x (adversarial, consistency, perf, re-adversarial) | 2H+7M fixed, 2H+1M deferred/accepted, 1M rejected | scope: kernel-code-quality

---

## 15. CommandLineToArgvW Win32 API

- [x] `CommandLineToArgvW(caller, lpCmdLine, pNumArgs)` -- kernel primitive in `src/kernel/env.c` (behind the future shell32 export); inverse of §4's `argv_to_cmdline` for argv[1+]:
  - Backslash/quote rules match Windows exactly: 2n backslashes + `"` → n backslashes + toggle "in quotes"; 2n+1 → n backslashes + literal `"`; a `"` inside quotes immediately followed by another `"` emits one literal `"` and CLOSES the quoted region (modulo-3 rule); backslashes not before `"` are verbatim
  - Outside quotes, whitespace (space/tab) ends the argument; inside quotes it is kept
  - `argv[0]` special parse: leading `"` → up to the closing `"`; else up to the first whitespace (backslashes literal) -- NOT a general inverse (§4 follow-up owns a program-name encoder)
  - A shared macro parser body serves both a UTF-8 core (`cmdline_to_argv`) and the wide W form
  - The W form parses UTF-16 code units DIRECTLY (all syntax chars are ASCII, so every other WCHAR is preserved verbatim -- no lossy UTF-16↔UTF-8 transcode); WCHAR cap `CMDL_ARGV_MAX_WCHARS`, byte cap `CMDL_ARGV_MAX_BYTES` for the core
  - Returns one self-describing block ({magic,total} header + `(argc+1)` NUL-terminated pointer array + strings); `*pNumArgs` set; free the whole block with `cmdline_free_argv` (this kernel has no `LocalAlloc` size bookkeeping)
- [x] Edge case: empty string → `*pNumArgs = 1`, `argv[0]` = caller module identity (`caller->name`, widened for the W form), NOT `""`; full kernel-owned ImagePathName deferred with SearchPathW (`env_searchpath.h`)
- [x] Edge case: `NULL` input → return `NULL` (Windows behavior); `*pNumArgs` zeroed
- [x] Round-trip with §4's `argv_to_cmdline`: encode `argv = {"a b", "c\"d", ""}` then `cmdline_to_argv` returns the same three args (`test_env_cmdline_roundtrip`)

- [x] Commit: `"kernel/env: CommandLineToArgvW Win32 API (shell32)"`

**Test checkpoint:** `CommandLineToArgvW` quote rules match Windows samples. QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-abi-tests.bat` (SUITE=abi) | 1860 suites, 0 failures, 0 leaked (9 `test_env_cmdline_*` cases: quotes, 2n+1 backslash, round-trip, empty→module-path core+W, NULL + NULL-pNumArgs, wide lone-surrogate verbatim, wide quotes, consecutive-quote modulo-3, PMM allocator crossover)

> **Notes:**
> - **What shipped** -- `cmdline_to_argv` / `CommandLineToArgvW` / `cmdline_free_argv` in `src/kernel/env.c`: one macro-generated backslash/quote parser over `char`/`uint16_t`, single `{magic,total}`-header block; 9 `test_env.c` cases.
> - **How it runs** -- plain kernel functions taking an explicit `struct task *caller` (like SearchPathW), NOT SSDT syscalls and NOT `pe.c` exports; unit-testable against a fixture task; `caller->name` read lock-free (stable field).
> - **Safety design** -- W form parses UTF-16 directly (no lossy transcode); input snapshotted into kernel memory so both parse passes consume the SAME bytes (no pass-divergence overrun); caller must keep the command line valid + NUL-terminated for the call, as Win32 requires.
> - **Canonical doc** -- the `include/kernel/env.h` contract block above `cmdline_to_argv`.
> - **Scope boundary** -- kernel primitive only; the user-callable shell32 export + LocalFree-compatible stub are deferred (owned by TODO-C / TODO-12-02); §4 owns the `argv_to_cmdline` encoder; the empty-cmdline argv[0] image path is owned by SearchPathW.
> **Verified:** 2026-07-15 | commit `1d3ba8f4` | 4/4 items | build OK | tests 1860/1860 PASS | [/] Win32 export deferred
> **Accepted:** [H] not a user-callable shell32 API -- kernel primitive only (extra `caller` param, `cmdline_free_argv` not `LocalFree`), like SearchPathW §14 -> XREF: 10-platform-services/TODO-C §"Tier 1" (item: "`CommandLineToArgvW`" at line 68)
> **Accepted:** [H] LocalAlloc/LocalFree-compatible user Win32 stub not implemented -> XREF: 12-user-platform-sdk/TODO-02 §6 (item: "`CommandLineToArgvW(lpCmdLine, pNumArgs)` Win32 stub" at line 206)
> **Accepted:** [M] `argv_to_cmdline` is not a general inverse for a special `argv[0]` (encoder over-escapes the program name) -> XREF: 02-kernel-core/TODO-22 §4 (item: "give `argv_to_cmdline` a Windows program-name encoder for `argv[0]`" at line 234)
> **Accepted:** [M] blocks > 4 KiB ride `pmm_alloc_contiguous`, which mutates the frame bitmap unsynchronized (pre-existing exposure, not new to §15) -> XREF: 03-memory-concurrency/TODO-03 §1 (item: "PMM bitmap SMP locking" at line 103)
> **Quality reviewed:** 2026-07-15 | Codex 5x (design, adversarial, consistency, perf, re-adversarial) | 2H+6M fixed, 4M accepted-XREF | scope: kernel-code-quality

---

## 16. Environment Variable Security & Sanitization

- [x] Blocklist `env_name_is_privilege_sensitive` (`env.c`): case-insensitive `LD_PRELOAD`, `LD_LIBRARY_PATH` (Linux loader-hijack -- XREF `TODO-23-exception-dispatch-seh.md`), any `_IMPOSSIBLE_DEBUG_`-prefix name
- [x] `env_sanitize_for_elevation(task)` (`env.c`): physically strips blocklisted vars, audit-logs each NAME at `LOG_WARN`, idempotent; detaches under `environ_lock` then klog+free after unlock
- [/] Elevation-transition invocation DEFERRED: `NtCreateProcess` never raises child IL above parent; owners TODO-15 SRM (UAC in-place token install) + TODO-12 §7 (child env inheritance)
- [x] AT_SECURE read gate `env_is_secure_context` + `env_get_copy`/`env_peek_locked`/block builders: token IL > Medium hides blocklisted names, NULL-token fail-closed; active now, independent of the deferred strip wiring
- [x] `NtSetEnvironmentVariable` validation (§5, `nt/nt_env.c:340`): empty/`=`/length rejects, value over cap `STATUS_NAME_TOO_LONG`, block quota `STATUS_QUOTA_EXCEEDED`, else `STATUS_INVALID_PARAMETER`
- [x] `NtQueryEnvironmentVariable` validation (§5, `nt/nt_env.c:128`): `ProbeForWrite` descriptor+buffer+len, `ProbeForRead` name, `STATUS_BUFFER_TOO_SMALL` w/ required length, no partial write

- [x] Commit: `"kernel/env: security sanitization for elevated processes, input validation"`

**Test checkpoint:** Elevated (High/System) task reads `LD_PRELOAD` as absent and its env block omits it; a Medium task still reads it; a malformed token fails closed; `env_sanitize_for_elevation` strips all blocklisted vars and is idempotent; invalid `NtSetEnvironmentVariable` name returns `STATUS_INVALID_PARAMETER` (§5). QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-abi-tests.bat` (SUITE=abi) | 6 s16 suites (blocklist, secure-context+malformed, read gate, sanitize, block-exclude, env_copy-exclude), 0 failures

> **Notes:**
> - **What shipped** -- `env.c` s16 layer: `env_name_is_privilege_sensitive`, `env_is_secure_context` (token IL > Medium, NULL fail-closed), `env_sanitize_for_elevation`, + AT_SECURE read/serialize gate on the getters and both block builders.
> - **How it runs** -- read gate is ACTIVE now (keyed on live token integrity); `env_sanitize_for_elevation` is the transition primitive whose call site is deferred to the token-install owners. Nt-handler input validation was already delivered by §5.
> - **Downstream effects** -- filed reciprocal sanitize-invocation items in TODO-15 (UAC token install) and TODO-12 §7 (elevated-child env inheritance); Codex design-review adoptions are in the commit message.
> - **Canonical doc** -- `include/kernel/env.h` "s16: elevation security" contract block.
> - **Scope boundary** -- §16 owns the blocklist + read gate + strip primitive; the transition INVOCATION is owned by TODO-15 (UAC) and TODO-12 §7 (child env copy); Linux `LD_*` semantics owned by the compat layer (TODO-23).
> **Verified:** 2026-07-15 | commit `e6c0c067` + review fixes | 5/6 items | build OK | tests 1899 abi + 1197 security PASS
> **Accepted:** [L] `env_is_secure_context` reads `task->token` with an ACQUIRE load but no teardown-safe pin (foreign-task reap could UAF; also a plain-writer/atomic-reader mismatch) -- pre-existing kernel-wide pattern, single-cursor-scheduler-safe today, does not fire (all callers pass a live task) -> XREF: 02-kernel-core/TODO-15 §4 (item: "Teardown-safe primary-token READ pin" at line 333)
> **Quality reviewed:** 2026-07-15 | Codex 9x (design, adversarial, consistency, perf, re-adversarial) | 4H+1M fixed, 1L accepted-XREF | scope: kernel-code-quality + kernel-quality-auditor (no C/H)

---

## 17. App Paths Registry-Based Executable Lookup

- [x] App Paths convention shipped in new `env_apppaths.c` + `env_apppaths.h`: `HKLM|HKCU\...\CurrentVersion\App Paths\{name}` subkey, default value = full exe path, `Path` value = extra launch-PATH dir
- [x] `app_paths_lookup(caller, name, out_path.., out_additional..)` -- `.exe`-appends bare names, rejects `\`/`/`; default -> exe path, optional `Path` -> out_additional; rich Win32 status + required-size (MORE_DATA never reads as not-found)
- [/] Integration: `shell_find_command` (§7) consults App Paths as the fallback after a PATH miss (Windows `ShellExecute` order) -- DEFERRED with §7 (no kernel shell exists yet; the lookup primitive is shipped and ready to call)
- [x] Per-user HKCU checked with precedence but caller-elevation-bound: elevated/System (and identity-less NULL) callers see HKLM ONLY so a user HKCU entry cannot hijack a privileged resolution; a normal caller gets HKCU-first then HKLM
- [x] `app_paths_register(caller, root, name, full_path, additional_path)` -- explicit HKLM/HKCU root; HKLM write needs an elevated caller (else `ERROR_ACCESS_DENIED`); NULL `additional_path` deletes stale `Path` (→ XREF TODO-03)

- [x] Commit: `"kernel/env: App Paths registry-based executable lookup"`

**Test checkpoint:** App Paths resolves `myapp` not on PATH. QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-abi-tests.bat` (SUITE=abi) | 5 App Paths suites, 0 failures.
> **Notes:**
> - **What shipped** -- `src/kernel/env_apppaths.c` + `include/kernel/env_apppaths.h`: `app_paths_lookup` / `app_paths_register` over the registry API; 5 suites in `src/kernel/test/test_env.c` (TEST_CAT_ABI).
> - **How it runs** -- plain kernel-C callables (NOT SSDT syscalls / pe.c exports); read/write the HKLM|HKCU App Paths subkeys; no allocation; consumers of the lock-free registry (registry-wide SMP sync + atomic multi-value writes owned by TODO-14 §14).
> - **Downstream effects** -- provides the App Paths fallback primitive §7's shell wiring calls and the registration TODO-03 consumes; design-review adoptions (elevation-bound precedence, rich status, retry-safe Path) in the commit.
> - **Canonical doc** -- `include/kernel/env_apppaths.h` (full contract + security posture).
> - **Scope boundary** -- §17 owns the lookup/register primitives; §7 owns the shell `shell_find_command` wiring; TODO-03 owns installer callers; TODO-15 §5 owns real registry-DACL authorization (elevation gate stands in until it lands).
> **Verified:** 2026-07-15 | commit `2ca1683f` | 4/5 items | build OK | tests 5 App Paths suites PASS
> **Accepted:** [H] non-atomic default+`Path` pair (concurrent register/lookup can interleave -- lock-free-registry class) -> XREF: 02-kernel-core/TODO-14 §14 (item: "Batched atomic multi-value write under one `reg_lock` hold" at line 663)
> **Deferred:** [M] `shell_find_command` App Paths fallback wiring (no kernel shell exists yet) -> XREF: 02-kernel-core/TODO-22 §7 (item: "`shell_find_command(name, out_path, max)`" at line 323)
> **Quality reviewed:** 2026-07-15 | Codex 9x (design + adversarial + consistency + perf + re-adversarial) | 5H+6M+1L fixed, 1H accepted-XREF | scope: kernel-code-quality

---

## 18. cmd.exe Dynamic Pseudo-Variables & Delayed Expansion

cmd exposes computed-at-expansion pseudo-variables (never stored in `environ[]`) plus an opt-in delayed `!VAR!` mode; both are cmd-parser concerns distinct from §3's generic single-pass `%VAR%`.

- [ ] Dynamic pseudo-vars resolved at expansion time by the shell (§7), never stored, real stored vars take precedence: `%CD%`, `%DATE%`, `%TIME%`, `%RANDOM%` (0-32767), `%ERRORLEVEL%`, `%CMDCMDLINE%`, `%CMDEXTVERSION%`
- [ ] `%ERRORLEVEL%` tracks the last command's exit code; the shell updates it after each command
- [ ] Delayed expansion: `setlocal enabledelayedexpansion` / `cmd /V:ON` enables `!VAR!` re-read at execution time (needed inside `FOR`/`IF` blocks where `%VAR%` is fixed at parse time per §3)
- [ ] `SET` enforces cmd's tighter documented per-var / total caps (fidelity beyond §10's Win32 API-level limits)
- [ ] Batch `%~` modifiers (`%~dp0`, `%~nx1`) extend D12 T02 §8's `%1..%9`; `FOR`/`IF`/`setlocal` batch semantics are owned there (item: "**cmd.exe batch interpreter**") → XREF `12-user-platform-sdk/TODO-02-env-vars-process-abi.md` §8
- [ ] Commit: `"shell: cmd.exe dynamic pseudo-variables + delayed !VAR! expansion"`

**Test checkpoint:** `echo %RANDOM%` varies across calls; `%ERRORLEVEL%` reflects the last exit code; `!VAR!` re-reads a var set earlier in the same `enabledelayedexpansion` block. Serial/klog observable. QEMU WHPX + TCG; VirtualBox; bare metal.

> **Deferred:** [blocked] §18 pseudo-vars + delayed `!VAR!` are cmd-parser features layered on the shell `%VAR%`/`SET` expansion that §7 owns, and §7 is itself deferred-blocked: the shell is user-mode (`user/cmd.c`, which has no `%VAR%` expansion, no `SET` built-in, and no `ERRORLEVEL` tracking), `src/shell/` is empty, and the §6 user-mode env wrappers those built-ins would call are all `[/]`. Every §18 item needs that missing layer: pseudo-vars and `!VAR!` need shell expansion; `%ERRORLEVEL%` needs the shell to write env after each command; "SET enforces cmd's caps" needs the `SET` built-in; `%~` modifiers + `FOR`/`IF`/`setlocal` additionally need a batch interpreter, which had no owner and is now filed as a concrete item. -> XREF: §7 (item: "Shell uses `shell_find_command` before any `exec` call") + §6 (Win32 env wrappers, all `[/]`) + `12-user-platform-sdk/TODO-02-env-vars-process-abi.md` §8 (items: "**`set NAME=VALUE`**", "**`echo`**", "**cmd.exe batch interpreter**").

---

## 19. Rtl Expansion Hardening: Output-Count Wrap & `%=X:%` Parity

Two latent defects in the SHIPPED §3 UTF-16 expansion path (`nt_rtlenv.c`), fixed before §20 layers new exports on it. Split out of the original combined §19 because the block-scan cap, the counter width, and the create-block cap are one coupled change; §20 consumes them but does not move them.

- [x] Raise the NULL-env expansion cap: `rtl_env_expand_pass` counted output in a uint32 (wrapped at 2^32). Now a saturating uint64; `RTL_ENV_BLOCK_MAX_WCHARS` 64 Ki -> 1 MiWCHAR, covering a full `ENV_BLOCK_MAX` environ
- [x] Checked-conversion order in `RtlExpandEnvironmentStrings_U`: reject a saturated count, then a result over `RTL_ENV_MAX_RESULT_WCHARS` (32766) with `STATUS_UNSUCCESSFUL`, and only THEN narrow -- never publish a truncated size
- [x] UTF-16 `%=X:%` parity: `rtl_env_block_lookup` split a hidden `=C:=<path>` entry at the FIRST `=`, yielding an unnameable empty key. Now scans one WCHAR in for a leading `=`, mirroring `env.c` ~line 72
- [x] `_Static_assert` pins `ENV_CREATE_BLOCK_MAX_WCHARS <= RTL_ENV_BLOCK_MAX_WCHARS` (created blocks stay consumable by the expansion scan) + the `RTL_ENV_MAX_RESULT_WCHARS` NUL-room invariant
- [/] `ENV_CREATE_BLOCK_MAX_WCHARS` stays 64 KiWCHAR: raising it needs the ~2 MiB contiguous-PMM path stress-verified under SMP (design review) -> XREF §22 (item: "Raise `ENV_CREATE_BLOCK_MAX_WCHARS`")
- [x] Commit: `"kernel/env: Rtl expansion output-count wrap fix + %=X:% parity"`

**Test checkpoint:** `%=C:%` resolves against a hidden `=C:=C:\dir` entry on the UTF-16 path (it returned the literal before); a result over 32766 WCHARs returns `STATUS_UNSUCCESSFUL` with `ReturnedLength` zeroed (ntdll parity), not a truncated `BUFFER_TOO_SMALL`; a formerly over-cap ~96 KiWCHAR environ now expands. Serial/klog observable. QEMU WHPX + TCG; VirtualBox; bare metal.
> **Test runner:** `scripts\debug\kernel\run-abi-tests.bat` (SUITE=abi) | 4 suites added/reworked, 0 failures (21290 kernel + 16 user-mode green)
> **Notes:**
> - **What shipped:** `nt_rtlenv.c` counter uint32 -> saturating uint64, `RTL_ENV_BLOCK_MAX_WCHARS` 64 Ki -> 1 MiWCHAR, new `RTL_ENV_MAX_RESULT_WCHARS`, hidden-`=C:` separator fix, 2 `_Static_assert`s.
> - **How it integrates:** `RtlExpandEnvironmentStrings_U` gains a checked-conversion order (saturation -> representability -> narrow), so `ReturnedLength` never publishes a truncated size and an unsatisfiable result is `STATUS_UNSUCCESSFUL` with `ReturnedLength` 0 (ntdll parity).
> - **Downstream effects:** every environ the store accepts is now expandable (pinned by `RTL_ENV_BLOCK_MAX_WCHARS >= ENV_BLOCK_MAX`, true today with ZERO margin), so the over-cap-via-environ branch is unreachable and its test became a positive large-environ case.
> - **Canonical doc:** [`include/kernel/nt/nt_rtlenv.h`](../../include/kernel/nt/nt_rtlenv.h) (cap rationale + return set).
> - **Scope boundary:** §20 owns the three BLOCKING gates review filed there (lookup-work bound, foreign-block extent, the expand-path 2 MiB PMM alloc the raise made live) plus the at-cap allocation test; §21 owns the four Rtl exports and stays non-user-reachable until §20 clears -- nothing here is user-reachable until §21 wires an export; §3 owns the expansion primitives.
> **Verified:** 2026-07-15 | commit `b0c184e0` + review fixes | 4/5 items | build OK | tests 21290 kernel + 16 user-mode PASS
> **Deferred:** [H] the raise took the NULL-env path's worst case from 128 KiB to 2 MiB (`pmm_alloc_contiguous(512)`) under the PER-TASK `environ_lock`, which serializes nothing across tasks, so two CPUs can double-allocate the same frames -> XREF: 02-kernel-core/TODO-22 §20 (item: "**BLOCKING before a user-reachable export:** expand path takes 2 MiB" at line 697)
> **Accepted:** [M] the unsynchronized PMM frame bitmap that lets the above double-allocate is a pre-existing kernel-wide gap this section AMPLIFIES (32 -> 512 frames), not originates -> XREF: 03-memory-concurrency/TODO-03 §1 (item: "**PMM bitmap SMP locking**" at line 103)
> **Quality reviewed:** 2026-07-15 | Codex 5x (design, adversarial, consistency, perf, re-adversarial) | 2M+3L fixed, 3H+1M deferred-XREF, 1M accepted-XREF | scope: kernel-code-quality + kernel-quality-auditor

---

## 20. Rtl Export Prerequisites: Lookup Bound, Foreign-Block Extent, At-Cap Allocation

The three BLOCKING gates §19's review filed against the shipped `nt_rtlenv.c` expansion path, plus the at-cap allocation test. Split out of the original combined §20 (which paired them with the exports that consume them): these harden the EXISTING §3 path and are the precondition for §21 making any of it user-reachable, so they land first and independently. Nothing in `nt_rtlenv.c` is user-reachable today (no SSDT row, no kernel32 caller), which is what keeps these deferrable-but-blocking rather than live defects.

- [x] Bounded `rtl_env_block_lookup` work: per-pass `RTL_ENV_EXPAND_WORK_MAX` budget, over-budget -> `STATUS_INSUFFICIENT_RESOURCES` refused before any store. A budget, not a name index: an index over a 1 MiWCHAR block needs ~168 KB of PMM
- [x] Trusted extent: new `rtl_env_expand_block(block, block_extent, ...)` engine bounds the scan by `min(extent, cap)`; the ntdll ABI carries no extent, so `RtlExpandEnvironmentStrings_U` refuses a non-NULL `Environment` (`STATUS_NOT_SUPPORTED`)
- [x] `rtl_env_block_validate` returns the lookup bound AND the verified extent in one call (`extent >= 2`, genuine double NUL), replacing the `(bound == 0) ? 2 : bound + 1` reconstruction
- [/] **BLOCKING before a user-reachable export:** expand path takes 2 MiB via `pmm_alloc_contiguous(512)` on an unlocked PMM, so two tasks can double-allocate → XREF `03-memory-concurrency/TODO-03` §1 (item: "**PMM bitmap SMP locking**" line 103)
- [x] Test the raise at its real boundary: `test_env_expand_at_cap_boundary` fills the environ to `ENV_BLOCK_MAX` and asserts a > 1e6 WCHAR block (~488+ frames vs the ~48 the old case reached)
- [x] Commit: `"kernel/env: bound Rtl block lookup + foreign-block extent before exports"`

**Test checkpoint:** a repeated-miss expansion at the PRODUCTION ceiling is refused with `STATUS_INSUFFICIENT_RESOURCES` publishing no length; against a small synthetic budget the refusal lands exactly at the boundary (the largest affordable miss count succeeds, one more is refused) on both the miss and hit paths, and one full-block miss still succeeds; a block with no terminator inside its extent is refused rather than scanned past; a lone `"\0"` is not accepted as the empty block; an at-cap environ (`> 1e6` WCHAR block, ~488+ frames) EXPANDS a real reference through `ExpandEnvironmentStringsForUser`. Serial/klog observable. QEMU WHPX + TCG; VirtualBox; bare metal.
> **Test runner:** `scripts\debug\kernel\run-abi-tests.bat` (SUITE=abi) | 9 suites added, 0 failures (2109 registered; 21309 kernel + 16 user-mode green)
> **Notes:**
> - **What shipped:** `nt_rtlenv.c` gains `rtl_env_block_validate` (extent-bounded, replacing `rtl_env_block_len`), the `rtl_env_expand_core` engine behind `rtl_env_expand_block`, and a per-pass work budget; 9 new tests.
> - **How it integrates:** builder-produced blocks reach the engine with the length their builder reported (no re-scan); a caller-supplied one is validated within its extent first. The public entry keeps its ntdll signature, NULL form only.
> - **Downstream effects:** unblocks §21. The non-NULL ABI divergence, the unmeasured budget constant, and the lookup cache are each owned by a concrete §24 item.
> - **Canonical doc:** [`include/kernel/nt/nt_rtlenv.h`](../../include/kernel/nt/nt_rtlenv.h) (extent contract, budget rationale, the non-NULL refusal and why).
> - **Scope boundary:** §20 owns the three gates + the at-cap test; §21 owns the exports; §24 owns the boundary probe+copy, the budget measurement, and the cache; the PMM bitmap is `03-memory-concurrency/TODO-03` §1.
> **Verified:** 2026-07-15 | commit `2ea286cc` + review fixes | 5/6 items | build OK | tests 21309 kernel + 16 user-mode PASS
> **Deferred:** [H] the expand path's 2 MiB `pmm_alloc_contiguous(512)` runs on an unsynchronized PMM frame bitmap, so two CPUs can double-allocate the same frames; the root cause is kernel-wide and owned elsewhere, and a local lock here would be a second allocator lock racing the real one (no user-reachable caller exists: `nt_rtlenv.c` has no SSDT row, and §21 carries the gate item that keeps it that way) -> XREF: 03-memory-concurrency/TODO-03 §1 (item: "**PMM bitmap SMP locking**" at line 103)
> **Accepted:** [M] a bounded per-call name cache would cut the repeated-miss constant the work budget merely bounds; rejected here because it cannot bound the worst case (all-distinct misses still thrash) and the budget must exist regardless -> XREF: 02-kernel-core/TODO-22 §24 (item: "Lookup cache for repeated `%NAME%` misses")
> **Accepted:** [M] refusing a non-NULL `Environment` diverges from the documented ntdll explicit-block form; the ABI-preserving shape needs a probe+copy boundary that derives a trusted extent, which is not this section's surface -> XREF: 02-kernel-core/TODO-22 §24 (item: "**Boundary probe+copy for a non-NULL `Environment`**")
> **Accepted:** [M] `RTL_ENV_EXPAND_WORK_MAX` is a reasoned ceiling (~8 ms at ~1e9 compares/s), not one measured on the slowest supported target (reason: not-functional-today -- no user-reachable caller) -> XREF: 02-kernel-core/TODO-22 §24 (item: "**Measure `RTL_ENV_EXPAND_WORK_MAX`**")
> **Quality reviewed:** 2026-07-15 | Codex 5x (design, adversarial, re-adversarial, consistency, perf) | 1H+6M+4L fixed, 4M accepted-XREF, 1H deferred-XREF | scope: kernel-code-quality + kernel-quality-auditor + concurrency-evidence-mapper

---

## 21. ntdll Rtl Environment Exports

Real Win11 resolves env access entirely in user-mode via ntdll `Rtl*Environment*` over the PEB-resident block (no syscall). Impossible OS stores env in the kernel (`task->environ`, §1), so these Rtl functions are thin kernel-side routines over the §5 Nt syscalls -- provide them so ntdll-importing apps and the CRT resolve. Consumes §20's bounded lookup + extent guarantees; builds on §19's hardened expansion path. ABI facts below are sourced from ReactOS `sdk/lib/rtl/env.c` + the Microsoft WRK `base/ntos/rtl/environ.c`.

> [!NOTE]
> Architecture divergence: env lives in the kernel here vs the PEB in Windows; the syscall-backed Rtl layer is the compat bridge. The kernel-vs-PEB storage choice is operator-reserved (security/ABI) -- flag for review, do not silently redesign §1/§5.

- [x] `RtlQueryEnvironmentVariable_U(Environment, Name, Value)` over `env_get_copy`; absent -> `STATUS_VARIABLE_NOT_FOUND`. Only `MaximumLength`/`Buffer` are inputs -- `Length` is an OUT field, so a caller's incoming value is not judged
- [x] `Value->Length` = required CONTENT bytes EXCLUDING the NUL on both paths. Exact fit SUCCEEDS (WRK; ReactOS refuses -- sources conflict, WRK wins). NUL needs a full WCHAR (`>= Length + 2`), closing an odd-`MaximumLength` overflow
- [x] `RtlSetEnvironmentVariable(Environment, Name, Value)` over `env_set`/`env_unset`; NULL `Value` deletes, absent-var delete is a silent success; the supported form touches no pointer storage
- [x] Refuse EVERY non-NULL `Environment` in Set with `STATUS_NOT_SUPPORTED` BEFORE dereferencing `*Environment`: `*Environment == NULL` is a different ntdll form (caller-owned empty env), not the current process
- [x] `=` in a name is `STATUS_INVALID_PARAMETER` except at position 0 (ntdll's rule; admits `=C:`, §12). The store is stricter (exact `=<A-Z>:`), so `=FOO` clears the ABI check and is refused below it
- [x] `RtlCreateEnvironment` / `RtlDestroyEnvironment`; `clone_current == 0` yields EMPTY via a new `env_create_empty_block`, NOT `env_create_block(inherit == 0)` (that means Registry-derived, still refused); one header + free path
- [x] `RtlDestroyEnvironment` returns `NTSTATUS` (always `STATUS_SUCCESS`), not VOID: sources conflict (ReactOS NDK vs WRK) and NTSTATUS is the strict x86-64 binary-compat superset
- [x] Refuse a non-NULL `Environment` in `RtlQueryEnvironmentVariable_U` with `STATUS_NOT_SUPPORTED`: reading a foreign block derefs untrusted memory with no trusted extent, as §20 found for expansion. §24's probe+copy restores it
- [x] Extracted `env_buf_alloc`/`env_buf_free` (env.h/env.c). Inventory was larger than drafted: `sp_alloc` (`env_searchpath.c`) was an uncounted THIRD copy, and all three disagreed on `n == 0` -- Gate 10
- [ ] kernel32 `GetEnvironmentVariable`/`SetEnvironmentVariable` (§6) route through these exports. BLOCKED twice: §6 is deferred (no kernel32 env wrapper exists), and the gate below forbids a kernel32 caller while §20's PMM item is open
- [x] Gate: no export is user-reachable -- no SSDT row, no ntdll export row, pinned by `test_rtlenv_exports_not_user_reachable` (`pe_ntdll_export_ssdt` == -1 x4), so wiring one must first close §20's BLOCKING item
- [x] Commit: `"ntdll: Rtl environment exports over the Nt env syscalls"`

**Test checkpoint:** an app importing `RtlQueryEnvironmentVariable_U` from ntdll resolves and returns the same value as `NtQueryEnvironmentVariable`; `RtlCreateEnvironment(0, &e)` builds an empty block and `(1, &e)` a sorted clone; `RtlSetEnvironmentVariable` and `RtlQueryEnvironmentVariable_U` with a non-NULL Environment each return `STATUS_NOT_SUPPORTED`. Serial/klog observable. QEMU WHPX + TCG; VirtualBox; bare metal.
> **Test runner:** `scripts\debug\kernel\run-abi-tests.bat` (SUITE=abi) | 15 suites added, 0 failures (2046 registered; 21383 kernel + 16 user-mode green)
> **Notes:**
> - **What shipped:** the four ntdll Rtl env exports in `nt_rtlenv.c` (Query/Set/Create/Destroy) over the §5 storage API; `env.c` gains `env_buf_alloc`/`env_buf_free` (one size-class rule, 3 copies retired); 14 suites.
> - **How it integrates:** kernel-side routines on the authoritative `task->environ`, taking no lock (the storage helpers lock internally); a foreign `Environment` is refused uniformly across Query/Set/Expand pending §24's snapshot.
> - **Downstream effects:** deliberately NOT compatibility-complete (created blocks are clone/destroy-only until §23/§24; §23 since shipped the live-adoption ops 2026-07-17, so only export-reachability via §24+§20 remains open) and NOT user-reachable (a test pins the absent ntdll export row); §6 stays blocked. Adoptions in the commit message.
> - **Canonical doc:** [`include/kernel/nt/nt_rtlenv.h`](../../include/kernel/nt/nt_rtlenv.h) (per-export contract, the WRK-vs-ReactOS exact-fit tie-break, the reachability gate).
> - **Scope boundary:** §21 owns the exports; §24 owns the probe+copy boundary, budget measurement, and cache; §6 owns the kernel32 wrappers; the PMM bitmap is `03-memory-concurrency/TODO-03` §1.
> **Verified:** 2026-07-15 | commit `7495aec9` + review fixes | 11/12 items | build OK | tests 21383 kernel + 16 user-mode PASS
> **Deferred:** [blocked] routing kernel32 `GetEnvironmentVariable`/`SetEnvironmentVariable` through these exports is blocked twice over: §6 itself is deferred on user-mode runtime prereqs (no kernel32 env wrapper exists to route), and this section's own reachability gate forbids adding a kernel32 caller while §20's PMM item is open -- doing it now would make the exports user-reachable over an unsynchronized PMM bitmap -> XREF: 02-kernel-core/TODO-22 §6 (item: "Route the env wrappers through §21's Rtl exports") + 03-memory-concurrency/TODO-03 §1 (item: "**PMM bitmap SMP locking**" at line 103)
> **Accepted:** [M] `RtlSetEnvironmentVariable`'s non-NULL `Environment` stays refused after §24 restores Query/Expand: its `void **` ABI may realloc and write back, which a read-only snapshot cannot serve (reason: scope -- needs its own ownership contract, not the same probe+copy) -> XREF: 02-kernel-core/TODO-22 §24 (item: "Extend the boundary to `RtlSetEnvironmentVariable`'s non-NULL `Environment`")
> **Accepted:** [L] every small query pays a 4 KiB heap round-trip before the value size is known (reason: scope -- the kmalloc-first shape is deliberate and shared with `NtQueryEnvironmentVariable_handler`, whose comment records it as the fix for a worse contiguous-PMM bug; changing one entry alone would diverge the two) -> XREF: 02-kernel-core/TODO-22 §24 (item: "Drop the per-query 4 KiB heap round-trip")
> **Quality reviewed:** 2026-07-15 | Codex 9x (design, adversarial x2, test-coverage, consistency x2, perf, re-adversarial x2) | 4H+5M+9L fixed, 4 accepted-XREF, 2 rejected | scope: kernel-code-quality + kernel-quality-auditor + concurrency-evidence-mapper + parity-research-analyst

---

## 22. env Allocator Safety and UTF-8 Expansion Budget

The `env.c` half of the completeness work §20's review deferred, split from the Rtl half (§24) on the subsystem seam: this section hardens the environment store's own allocator and bounds its UTF-8 expansion walk; §24 owns the UTF-16 `nt_rtlenv.c` surface above it. Both were one section until §24's boundary-copy items proved blocked on fault-recoverable usercopy (TODO-23 §13), which left an allocator/budget cluster carrying a different review lens (ownership and DoS, not an untrusted-pointer boundary) and no dependency on the blocked work.

- [x] Fold the alloc size into a self-describing `env_buf` header: a `{magic,total_bytes,payload_bytes}` prefix selects `kfree` vs `pmm_free_frame`; the caller's `n` becomes the independent witness and a free needs both to agree, else it is refused
- [x] Size class moved to the TOTAL (header + payload), keeping `kmalloc` inside its 4 KiB ceiling: `ENV_BUF_PAYLOAD_MAX` bounds the heap class, `ENV_BUF_ALLOC_MAX` refuses a request that would wrap `uint32`
- [x] Magic poisoned on free, so an immediate double free is a logged no-op; a foreign or already-freed pointer is refused rather than handed to a deallocator on a guessed extent
- [x] **Budget the UTF-8 `env_expand` path**: it walked input under `environ_lock` unbounded. `ENV_EXPAND_WORK_MAX` caps it; over-budget returns `ENV_EXPAND_OVER_BUDGET` and never leaves a partial expansion behind
- [x] Every unbounded loop charges the budget (outer step, closing-`%` scan, remainder copy, value copy); one refusal exit releases `environ_lock`. Budgeting only the outer step would leave three uncapped paths
- [x] `env_expand_budget(...)` splits the ceiling out so the refusal is testable with a small budget, not an 8 MiB input -- mirrors `rtl_env_expand_block_budget`; `env_expand` is the wrapper
- [/] Raise `ENV_CREATE_BLOCK_MAX_WCHARS` toward `RTL_ENV_BLOCK_MAX_WCHARS` so `RtlCreateEnvironment` (§21) accepts every environ the store does, once the ~2 MiB PMM path is SMP-stress-verified → XREF `03-memory-concurrency/TODO-03`
- [x] Commit: `"kernel/env: self-describing env_buf header + UTF-8 expansion work budget"`

**Test checkpoint:** an `env_buf` payload round-trips at both size classes and stays 16-byte aligned; a free naming the WRONG size across the 4 KiB class is REFUSED (the caller's size is the independent witness the header cannot corrupt), as is a corrupt header, a coherent paired corruption, and an immediate double free; zero and wrapping requests are refused. `env_expand` refuses over-budget input -- emptying `output` when the walk ran out, leaving BOTH buffers untouched when the pre-lock measure did (overlap is not yet known) -- and is byte-identical to its pre-budget result under the ceiling; a `%NAME%` reference is charged its lookup bound, and the walk stops once `output` is full. Serial/klog observable (error paths use `TEST_KLOG_SUPPRESS`, so a green run stays clean). QEMU WHPX + TCG; VirtualBox; bare metal.

> **Test runner:** `scripts\debug\kernel\run-abi-tests.bat` (SUITE=abi) | 2063 suites, 0 failures

> **Notes:**
> - **What shipped:** a self-describing `{magic,total_bytes,payload_bytes}` header under every `env_buf_alloc` (`env.c`), plus an `ENV_EXPAND_WORK_MAX` budget on the UTF-8 `env_expand` walk with an injectable ceiling; 8 new suites.
> - **How it integrates:** invisible to callers -- the payload stays >= 16-byte aligned on both size classes, so `cmdl_hdr`'s alignment assert and `env_block_hdr` are unaffected; the size class keys off header+payload to stay under kmalloc's 4 KiB cap.
> - **Downstream effects:** closes §21's Accepted `env_buf_free(p, n)` wrong-deallocator finding; the caller's `n` is now a logged cross-check, not a load-bearing input. Adoptions in the commit message.
> - **Canonical doc:** [`include/kernel/env.h`](../../include/kernel/env.h) (env_buf self-describing contract, size-class rule, expansion-budget failure contract).
> - **Scope boundary:** §22 owns the `env.c` allocator + UTF-8 budget; §24 owns the `nt_rtlenv.c` boundary copy, budget measurement, and lookup cache; the ~2 MiB PMM path is `03-memory-concurrency/TODO-03` §1.

> **Verified:** 2026-07-15 | commit `ab60e334` + review fixes | 7/8 items | build OK | tests 21408 kernel + 16 user-mode PASS | smoke PASS (KVM 3.4s)
> **Accepted:** [H] keying the size class on header+payload moves payloads in (4080, 4096] from `kmalloc` to the SMP-unlocked `pmm_alloc_contiguous`; the race is pre-existing and user-reachable for every payload > 4096 (`env_set` via `NtSetEnvironmentVariable`), and this widens it by 16 bytes of payload space (reason: infra -- the 4 KiB kmalloc ceiling forces the header+payload class, and the fix is PMM locking) -> XREF: 03-memory-concurrency/TODO-03 §1 (item: "**PMM bitmap SMP locking**" at line 103)
> **Accepted:** [M] a 32768-byte payload now spans 9 contiguous frames instead of 8; inherent to sub-payload metadata, and the real waste is the fixed `ENV_VALUE_MAX + 1` retry rather than the header (reason: scope -- the query probe already knows the length) -> XREF: 02-kernel-core/TODO-22 §24 (item: "Right-size the grow-once retry to `returned_length + 1`")
> **Quality reviewed:** 2026-07-15 | Codex 11x (design, adversarial x3, re-adversarial x2, consistency x2, perf x2, test-coverage) | 5H+10M fixed, 1H+1M accepted-XREF, 2H split to §24 | scope: kernel-code-quality + kernel-quality-auditor + concurrency-evidence-mapper

---

## 23. Live-Environment Adoption: SetCurrentEnvironment, SetEnvironmentStrings, CreateEnvironmentEx

> [!NOTE]
> **SHIPPED 2026-07-17.** Unparked after `02-kernel-core/TODO-33 §10` retired the BSS ceiling; the parked `git stash s23-wip-bss-check` (env.c substrate) was applied and RE-VERIFIED against the moved HEAD. The re-verify Codex design review found (and this ship fixed) two real defects the pre-park state carried: `env_replace_from_block_utf16` accepted an interior double-NUL with trailing data (silent truncation under an all-or-nothing ABI) and decoded UTF-16 with `NLS_CP_REPLACE` (silently rewriting malformed units). The three Rtl wrappers were then written over the corrected substrate. Exports stay GATED (no pe.c row) until `§24` probe+copy + `§20` PMM close.

§21 shipped the four Rtl env entries the CRT reaches for, but `RtlCreateEnvironment` is still a pure alloc/free round-trip: nothing adopts a created block as the live environment, so §21 + §22 could both close with the create/destroy pair never becoming real. This section closes that with the block-LIFECYCLE family. Split from the counted READ forms (§25) on the seam the design review drew: adoption is a store-ownership + atomicity lens over `env.c`, while the counted forms are an ABI-length + integer-narrowing lens over `nt_rtlenv.c`'s expansion engine. Signatures + flags pinned VERBATIM from winsiderss/phnt `ntrtl.h` (2026-07-15), cross-read against the WRK and ReactOS `sdk/lib/rtl/env.c`.

- [x] `env_exchange_block(t, entries, count, void **out_old)` (env.c): ONE `environ_lock` span snapshots the old store AND swaps the prepared new one; new store prepared before the lock, so any failure leaves the prior environ live
- [x] Split `env_build_block_utf16` into an `_locked` core (caller holds the lock) + a locking wrapper; `env_snapshot_wrapped_locked` builds `PreviousEnvironment` INSIDE the swap span, so an OOM there aborts the whole exchange with no snapshot leaked
- [x] `RtlSetCurrentEnvironment(PVOID Environment, PVOID *PreviousEnvironment)` (nt_rtlenv.c): NTSTATUS + PVOID per phnt/WRK; NULL out-param swaps and builds no old block. NULL Environment -> INVALID_PARAMETER
- [x] REPLACE-whole-store, never per-entry merge (test: adopt drops keys absent from the block). Copies contents internally but CONSUMES the block on SUCCESS (native ownership transfer); caller retains only on failure
- [x] `env_adopt_block` becomes exactly `env_exchange_block(t, e, c, NULL)` over a shared prepare + well-formedness predicate, so the two paths cannot drift on what a storable entry is
- [x] Provenance: BEST-EFFORT `env_block_extent()` check (derefs `block-1`); a non-provenanced pointer -> `STATUS_NOT_SUPPORTED`, live store untouched. NOT a foreign-pointer validator (kernel-resident caller contract); header private to env.c
- [x] `RtlSetEnvironmentStrings(PCWSTR NewEnvironment, SIZE_T NewEnvironmentSize)`: size is BYTES; ODD size and 0 -> INVALID_PARAMETER (never floored), WCHAR count bounded before narrowing. Rides §24-owned kernel-resident input contract
- [x] STRICT counted-block adoption (`env_replace_from_block_utf16`): Codex re-verify hardened it to reject interior-double-NUL + trailing data (was silent truncation) and decode `NLS_CP_STRICT` (was REPLACE); any defect refuses the whole block
- [x] `RtlCreateEnvironmentEx(Source, Environment, Flags)`: TRANSLATE 0x1, TRANSLATE_FROM_OEM 0x2, EMPTY 0x4. Non-NULL source -> `STATUS_NOT_SUPPORTED`; `EMPTY` -> empty; `Flags==0` -> clone; NULL out -> INVALID_PARAMETER
- [x] Flag precedence (Codex): unknown bit + FROM_OEM-without-TRANSLATE -> INVALID_PARAMETER; either TRANSLATE bit -> NOT_SUPPORTED. Validated BEFORE EMPTY so EMPTY masks nothing; every defined bit tested
- [x] Gate (split in two): implementation landed; reachability did NOT -- no export row until BOTH §24's probe+copy and §20's PMM item close. §21's `pe_ntdll_export_ssdt == -1` test extended to pin the 3 new entries absent
- [x] Commit: `"ntdll: live-environment adoption over an atomic env exchange"`

**Test checkpoint:** `RtlSetCurrentEnvironment` installs a `RtlCreateEnvironment(1)` clone and the store then reads a variable only that block carried; `PreviousEnvironment` returns a block whose content is the pre-swap environ and which `RtlDestroyEnvironment` frees; a NULL `PreviousEnvironment` still swaps and allocates no old block; a non-provenanced pointer is refused with `STATUS_NOT_SUPPORTED` and the store is untouched; an adoption failure leaves the prior environ intact and leaks no snapshot. `RtlSetEnvironmentStrings` replaces the store from a counted block and refuses an odd size, an unterminated block, and a size past `ENV_BLOCK_MAX`. `RtlCreateEnvironmentEx` honors EMPTY, clones on 0, and refuses each unknown flag bit and each TRANSLATE bit. Serial/klog observable. QEMU WHPX + TCG; VirtualBox; bare metal.

> **Test runner:** `scripts\debug\kernel\run-abi-tests.bat` (SUITE=abi) | 4 new suites (create-ex flag matrix / exchange secure-snapshot / set-current / set-strings), 2124/2124 kernel + 16/16 user PASS, 0 leaked on KVM

> **Notes:**
> - Shipped: 3 ntdll wrappers in `nt_rtlenv.c` (`RtlSetCurrentEnvironment`, `RtlSetEnvironmentStrings`, `RtlCreateEnvironmentEx`) over the applied env.c substrate (`env_exchange_block`, `env_replace_from_block_utf16`, `env_block_extent`).
> - How it runs: NTSTATUS entries, thread context; whole-store REPLACE under one `environ_lock` span; flag precedence rejects unknown/incompatible bits before EMPTY. Codex re-verify adoptions in the commit message.
> - Downstream: exports stay GATED (no `pe.c` row; §21's `pe_ntdll_export_ssdt == -1` test extended to the 3 new entries) until §24 probe+copy + §20 PMM close; unblocked by `02-kernel-core/TODO-33 §10` BSS headroom.
> - Canonical doc: `include/kernel/nt/nt_rtlenv.h` (per-entry contracts) + `include/kernel/env.h` (substrate contracts).
> - Scope boundary: §23 owns block-LIFECYCLE adoption over `env.c`; §24 owns the counted-block boundary probe+copy + expansion completeness; §25 owns the counted (non-`_U`) Rtl read forms.
> **Verified:** 2026-07-18 | commit `9f43238d` | 12/12 items | build OK | tests 2124/2124 + 16 user PASS, 0 leaked (KVM)
> **Accepted:** [L] restore-token cap 65536 WCHARs < 1 MiB store cap; a 64K-1M-WCHAR env fails `RtlSetCurrentEnvironment` on the old-store serialize (reason: pre-existing env_create_block format cap, gated) -> XREF: 02-kernel-core/TODO-22 §24 (item: "Restore-token cap parity" at line 837)
> **Quality reviewed:** 2026-07-18 | Codex 7x (design, adversarial x2, re-adversarial x2, consistency, perf) | 1H+6M+1L fixed, 1L accepted-XREF, 1L rejected | scope: kernel-code-quality + kernel-quality-auditor + concurrency-evidence-mapper

---

## 24. Rtl Expansion Completeness: Boundary Copy, Measured Budget, Lookup Cache

The `nt_rtlenv.c` half of §20's deferred completeness work (§22 shipped the `env.c` half). The boundary-copy items are BLOCKED: snapshotting a caller `Environment` means scanning an extent-less untrusted block for its double NUL, and this kernel has no fault-recoverable usercopy -- `copy_from_user` (`cpu_security.c:270`) is a raw STAC/CLAC byte loop whose only failure return is a `KERNEL_TESTS` injector, and `ProbeForRead` (`ssdt.c:60`) checks range and alignment, never mapping. A page-presence precheck cannot substitute: no address-space lock exists (`munmap`, `NtFreeVirtualMemory` MEM_RELEASE, and `swap_out` all clear PTEs unsynchronized), and `vmm_query_flags` walks `kernel_pml4` while user unmaps take an explicit `cr3`. Nothing here is user-reachable until §20's BLOCKING PMM item closes (§21 carries that gate).

- [/] **Boundary probe+copy for a non-NULL `Environment`**: copy the caller block into a terminated kernel snapshot, then expand it with a known extent, restoring the ntdll form §20 refuses → XREF `02-kernel-core/TODO-23` §13
- [/] Apply the same probe+copy to `RtlQueryEnvironmentVariable_U`'s non-NULL `Environment` (§21 refuses it): one trusted-snapshot helper serves both the query and expansion paths, so the two cannot drift → XREF `02-kernel-core/TODO-23` §13
- [/] Extend the boundary to `RtlSetEnvironmentVariable`'s non-NULL `Environment` (§21 refuses it). A read-only snapshot is NOT enough: the ABI takes `void **` because ntdll may REALLOC the block and write it back -- needs its own ownership contract
- [ ] Even WITH fault-recoverable usercopy, an unterminated block scanned to a coincidental double NUL reads adjacent memory; user pages share identity-mapped frames today, so refusing it needs per-process isolation
- [ ] Snapshot `Source` into a private kernel buffer before the two-pass expansion once these forms are user-reachable: the two-pass check misses an equal-length, equal-cost concurrent Source mutation. Same probe+copy boundary owns it → XREF §25
- [ ] **Measure `RTL_ENV_EXPAND_WORK_MAX`**: the 8388608 ceiling is reasoned (~8 ms at ~1e9 compares/s), not measured. Derive it from worst-case bare-metal timing and record the method next to the constant
- [ ] Lookup cache for repeated `%NAME%` misses: a bounded (<= 4 KiB `kmalloc`, no PMM) per-call name cache shared by both passes would cut the O(refs x block) constant. The §20 budget is the hard bound; this is the optimization under it
- [ ] Drop the per-query heap round-trip: `RtlQueryEnvironmentVariable_U` + `NtQueryEnvironmentVariable_handler` both `env_buf_alloc(ENV_BUF_PAYLOAD_MAX)` before knowing the size. A 256B stack probe keeps grow-once; fix BOTH or they diverge
- [ ] Right-size the grow-once retry to `returned_length + 1`: both query paths retry at a fixed `ENV_VALUE_MAX + 1` (32768), which with §22's 16-byte header spans 9 frames instead of 8. The probe already knows the length
- [ ] Make the `env_buf` magic poison the serialization point: `env_buf_free` read-checks then writes `magic` non-atomically, so two CPUs freeing the same pointer both pass and both release. `__atomic_compare_exchange_n` makes the catch SMP-real
- [ ] Pin `env_expand`'s truncation sentinel against `ENV_EXPAND_OVER_BUDGET`: a `max_len` of `0xFFFFFFFF` returns `(int)max_len` == -1, indistinguishable from a refusal. No caller comes near it; state a `max_len <= INT32_MAX` contract
- [ ] **Ownership-registry provenance validator**: `env_block_extent()` cannot refuse a foreign pointer (provenance derefs `block-1`). An export row needs a validator keyed on the body pointer -> XREF `02-kernel-core/TODO-22` §23
- [ ] **Restore-token cap parity**: env_create_block caps at 65536 WCHARs but the store accepts 1 MiB, so a 64K-1M-WCHAR env fails RtlSetCurrentEnvironment on the old-store serialize. Raise the cap when the gate opens -> XREF §23
- [ ] Commit: `"ntdll: measured expansion budget + lookup cache for Rtl env"`

**Test checkpoint:** the measured budget constant carries its derivation next to it; a repeated-miss expansion is measurably cheaper than the §20 baseline while still refusing at the budget; a small query no longer allocates a 4 KiB heap block, and the Rtl and Nt query paths agree. When the boundary items unblock: a caller-supplied `Environment` expands and queries through the snapshot helper, and an unterminated/unmapped one is refused rather than scanned past. Serial/klog observable. QEMU WHPX + TCG; VirtualBox; bare metal.

- [ ] **Split the section-level park.** Only the three boundary probe+copy items need fault-recoverable usercopy; the measured-budget, lookup-cache, CAS double-free and sentinel items are independent and shippable, but the terminal stamp parks them
  - The oracle reads a stamped `[/]` section as DONE, so the documented non-atomic `env_buf_free` SMP double-free hardening stays stranded behind an unrelated usercopy project
  - Move the three usercopy-bound items into their own deferred scope (or leave only those `[/]`), then implement the independent remainder -> XREF: `02-kernel-core/TODO-33 §11` (item: "`TODO-22 §24`: KEPT `[/]`")

> **Deferred:** [blocked] blocker (2) of 2 is CLEARED; blocker (1) remains and is sufficient on its own. The three boundary probe+copy items need bounded, fault-recoverable usercopy: `copy_from_user` has no fixup table or SEH, `ProbeForRead` never checks mapping, and a page-presence precheck cannot close it (no address-space lock exists; `vmm_query_flags` walks `kernel_pml4` while user unmaps take a `cr3`) -- so scanning an extent-less untrusted block for its terminator is a kernel-panic primitive reachable from user input -> XREF: `02-kernel-core/TODO-23 §13` (item: "Extend `include/kernel/nt/zw.h` + `src/kernel/nt/ssdt.c` in place: add `try_copy_from_user` / `try_copy_to_user`"). The ceiling half retired 2026-07-17: `02-kernel-core/TODO-33 §10` moved the BSS end to `0x6c2000` (~1272 KiB headroom), so the REMAINING items (measured budget, lookup cache, stack probe, right-sized retry, CAS-serialized free, sentinel contract) are now free to land -- they no longer wait on headroom, only on this section being reachable. Carries a real latent SMP defect (the non-atomic `env_buf_free` magic check, item above) that ships with them; it is NOT user-reachable today (§20's BLOCKING PMM item gates the export row) -> XREF: `02-kernel-core/TODO-33 §11` (item: "`TODO-22 §24`: KEPT `[/]`")

---

## 25. Counted (non-`_U`) Rtl Env Read Forms

The counted `RtlQueryEnvironmentVariable` / `RtlExpandEnvironmentStrings` (raw ptr+len, no `UNICODE_STRING`) that a real PE import table may name instead of the `_U` entries §21 shipped. In ReactOS the `_U` forms are thin wrappers over these, so the counted form is the PRIMITIVE and `_U` should delegate DOWN -- but §23's design review proved the shipped engine cannot become that primitive unchanged, which is why this is its own section rather than a bullet in §23: the lens here is ABI length conventions + integer narrowing, not store ownership. Both entries take `_In_opt_ PVOID Environment`, so they inherit §21's non-NULL refusal until §24 lands the boundary. Signatures pinned verbatim from winsiderss/phnt `ntrtl.h` (2026-07-15).

- [x] Extracted `rtl_env_expand_counted` (nt_rtlenv.c): raw ptr + `size_t` lengths, no `UNICODE_STRING` ceiling. `rtl_env_expand_core` is now its thin US adapter. A `SourceLength` past the cap is refused BEFORE the uint32 pass-index narrowing
- [x] `rtl_env_ranges_overlap` widened to `size_t alen/blen` + an endpoint-wrap guard (a range whose `base + len` wraps is treated as overlapping -> rejected), so a > 4 GiB byte length can neither truncate nor wrap past detection
- [x] `RTL_ENV_SOURCE_MAX_WCHARS` (1 MiWCHAR) is the explicit source bound the SIZE_T form needs; over it is `STATUS_INSUFFICIENT_RESOURCES` (resource-policy, a documented divergence from native's unbounded SIZE_T), not a malformed-input status
- [x] `RtlQueryEnvironmentVariable(Env, Name, NameLength, Value, ValueLength, ReturnLength)`: WCHAR lengths, WRK exact-fit-SUCCEEDS (not ReactOS strict), over a shared `rtl_env_query_value` core; a name past `ENV_NAME_MAX` is `NAME_TOO_LONG`
- [x] Query `ReturnLength` pinned to the ReactOS counted convention (WCHARs EXCLUDING the NUL on success, INCLUDING it on `BUFFER_TOO_SMALL`); the `_U` wrapper applies the `ReturnLength -= 1` correction to reach its bytes-excl-NUL Length
- [x] `RtlExpandEnvironmentStrings(Env, Source, SourceLength, Destination, DestinationLength, ReturnLength)`: `ReturnLength` INCLUDES the NUL on BOTH paths (distinct from Query, not unified); result ceiling is saturation-only, no USHORT limit
- [x] Both `_U` forms delegate DOWN: expand `_U` keeps the USHORT `RTL_ENV_MAX_RESULT_WCHARS` -> `UNSUCCESSFUL` map + descriptor overlaps; query `_U` keeps descriptor validation + the `MaximumLength / 2` floor (== the `>= Length + 2` NUL guard)
- [x] Gate honored: NO ntdll export row added; the counted forms stay non-user-reachable pending §24 probe+copy and §20's PMM item (`03-memory-concurrency/TODO-03 §1`); the §21 `pe_ntdll_export_ssdt == -1` test pins both counted names absent
- [x] Commit: `"ntdll: counted (non-_U) Rtl env read forms over a SIZE_T-safe core"`

**Test checkpoint:** the counted Query and Expand agree value-for-value with their `_U` counterparts on the shared cases; exact fit SUCCEEDS on Query (WRK rule) and the `_U` wrapper still reports Length in bytes excluding the NUL; Expand's `ReturnLength` includes the NUL on both the success and too-small paths; a `SourceLength` past the documented cap is refused rather than narrowed; a result above `RTL_ENV_MAX_RESULT_WCHARS` is `STATUS_UNSUCCESSFUL` through `_U` but expressible through the counted form; a non-NULL `Environment` is refused pending §24. Serial/klog observable. QEMU WHPX + TCG; VirtualBox; bare metal.

> **Test runner:** `scripts\debug\kernel\run-abi-tests.bat` (SUITE=abi) | 8 new suites (counted Expand success/too-small/foreign/boundaries, counted Query roundtrip/too-small/errors/boundaries), 2162/2162 kernel + 16/16 user PASS, 0 leaked on KVM

> **Notes:**
> - **What shipped:** two counted phnt read forms (`RtlExpandEnvironmentStrings`, `RtlQueryEnvironmentVariable`) in `nt_rtlenv.c` over a SIZE_T-safe `rtl_env_expand_counted` engine + a shared query core; both `_U` forms delegate down; 8 new suites.
> - **How it integrates:** counted forms serve only the NULL Environment (foreign -> `STATUS_NOT_SUPPORTED`); Expand `ReturnLength` includes the NUL on both paths, Query excludes it on success -- deliberately NOT unified. Adoptions in the commit message.
> - **Downstream effects:** exports stay GATED (no `pe.c` row; the §21 absence test pins both counted names) until §24 probe+copy + §20 PMM (`03-memory-concurrency/TODO-03 §1`) close.
> - **Canonical doc:** [`include/kernel/nt/nt_rtlenv.h`](../../include/kernel/nt/nt_rtlenv.h) (counted entry contracts, ReturnLength conventions, source-policy ceiling).
> - **Scope boundary:** §25 owns the counted (non-`_U`) READ forms over `nt_rtlenv.c`; §23 owns block-lifecycle adoption; §24 owns the foreign-block probe+copy boundary + expansion completeness.
> **Verified:** 2026-07-18 | commit `c846428f` | 8/8 items | build OK | tests 2162/2162 kernel + 16 user PASS, 0 leaked (KVM)
> **Accepted:** [M] two-pass agreement misses an equal-length equal-cost concurrent `Source` mutation (reason: pre-existing documented limitation shared by all forms, not user-reachable) -> XREF: 02-kernel-core/TODO-22 §24 (item: "Snapshot `Source` into a private kernel buffer" at line 830)
> **Quality reviewed:** 2026-07-18 | Codex 7x (design, adversarial x2, re-adversarial x2, consistency, perf) | 1H+4M+1L fixed, 1M accepted-XREF | scope: kernel-code-quality + kernel-quality-auditor

---

## OS Comparison

| ⭐   | Feature                    | 🪟 Win11               | 🐧 Linux              | 🚀 Impossible OS      |
| --- | -------------------------- | --------------------- | -------------------- | -------------------- |
| 💎   | Per-process env storage    | ✅ PEB UTF-16          | ✅ POSIX environ      | ✅ §1 kernel API      |
| 💎   | `%VAR%` / `$VAR`           | ✅ cmd `%VAR%`         | ✅ bash `$VAR`        | ✅ §3 `env_expand`    |
| 💎   | System defaults            | ✅ Session Manager     | ✅ `/etc/environment` | ✅ §2 Registry+synth  |
| 💎   | argv to child              | ✅ CRT cmdline         | ✅ execve argv        | ✅ §4 crt0->main      |
| 💎   | Env var read/write         | ⚠️ ntdll Rtl usermode | ⚠️ libc only         | ⚠️ §5 Nt✅, §21 Rtl✅  |
| 💎   | Get/Set env Win32          | ✅ kernel32 A/W        | ⚠️ Wine path         | ⬜ §6                 |
| 💎   | Expand env strings         | ✅ A/W                 | ⚠️ Wine path         | ⬜ §6                 |
| 💎   | GetCommandLine             | ✅ A/W                 | ⚠️ Wine path         | ⬜ §6                 |
| 💎   | PATH lookup                | ✅ PATHEXT             | ✅ POSIX PATH         | ⬜ §7                 |
| 💎   | SET shell cmd              | ✅ cmd built-in        | ✅ export/env         | ⬜ §7                 |
| 💎   | Shell startup              | ✅ HKCU at logon       | ✅ profile files      | ⬜ §8                 |
| 💎   | Env change notify          | ✅ WM_SETTINGCHANGE    | ⚠️ inotify etc       | ⬜ §9                 |
| 💎   | Persistent set             | ✅ setx.exe            | ⚠️ edit dotfiles     | ⬜ §9                 |
| ⭐   | sysdm env tab              | ✅ sysdm.cpl           | ❌ no GNOME equiv     | ⬜ §9                 |
| ⭐   | SET /A arith               | ✅ cmd only            | ✅ bash arith         | ⬜ §7                 |
| ⭐   | source / `.`               | ❌ not cmd             | ✅ POSIX              | ⬜ §8                 |
| 💎   | Sorted env block           | ✅ Unicode sort        | ❌ unsorted           | ✅ §10 ASCII sort     |
| 💎   | CREATE_UNICODE_ENVIRONMENT | ✅ CreateProcess docs  | ❌ Win32-only         | ⚠️ §10 blk, T12 §7   |
| 💎   | Env size limits            | ✅ 32K/var             | ⚠️ ARG_MAX           | ✅ §10 32K+1MiB       |
| 💎   | PATHEXT                    | ✅ long default        | ❌ N/A                | ⚠️ §11 `.EXE` def    |
| 💎   | Hidden `=C:` cwd           | ✅ per drive           | ❌ single cwd         | ⚠️ §12 (inherit def) |
| 💎   | CreateEnvBlock             | ✅ userenv             | ❌ none               | ⚠️ §13 caller blk    |
| 💎   | ExpandForUser              | ✅ userenv             | ❌ none               | ⚠️ §13 NULL token    |
| 💎   | SearchPathW                | ✅ kernel32            | ⚠️ execvp libc       | ✅ env_searchpath.c   |
| 💎   | SetSearchPathMode          | ✅ kernel32            | ❌ N/A                | ✅ safe-search CAS    |
| 💎   | CmdLineToArgvW             | ✅ shell32             | ❌ wordexp diff       | ⚠️ §15 kernel prim   |
| 💎   | Elevated env strip         | ✅ restricted          | ✅ AT_SECURE          | ⚠️ §16 read gate     |
| ⭐   | App Paths                  | ✅ HKLM App Paths      | ❌ none               | ⚠️ §17 lookup/reg    |
| 💎   | Dynamic pseudo-vars        | ✅ %CD%/%ERRORLEVEL%   | ⚠️ $PWD/$?/$RANDOM   | ⬜ §18                |
| 💎   | Delayed `!VAR!` expansion  | ✅ cmd /V              | ❌ N/A                | ⬜ §18                |
| 💎   | ntdll Rtl env exports      | ✅ ntdll usermode      | ❌ none               | ⚠️ §21 kernel-side   |
| 💎   | Rtl live-env adoption      | ✅ ntdll usermode      | ❌ none               | ⚠️ §23 kernel-side   |
| 💎   | Counted Rtl env reads      | ✅ phnt ntrtl.h        | ❌ none               | ⚠️ §25 SIZE_T-safe   |
| ⭐   | Rtl expansion hardening    | ⚠️ uint32 len fields  | ❌ n/a                | ✅ §19 u64 + `%=X:%`  |
| ⭐   | Rtl lookup work bound      | ❌ O(n) scan per ref   | ❌ n/a                | ✅ §20 work budget    |
| 💎   | Exec argv+envp size cap    | ⚠️ per-var only       | ✅ E2BIG/ARG_MAX      | ✅ §4 frame+ARG_MAX   |

After §1 through §9, Impossible OS reaches base Windows 11 and Linux parity for core environment variable features: per-process UTF-8 env storage, `%VAR%` expansion, Registry-backed system defaults, Win32 `GetEnvironmentVariable` / `ExpandEnvironmentStrings`, PATH lookup, `SET`, and `.profile` startup.

§10 through §17 close the remaining competitive gaps: sorted environment blocks (a hard Windows contract), `PATHEXT` extension search order, hidden drive-letter CWD variables, `CreateEnvironmentBlock` for service-launched processes, `SearchPathW` / `CommandLineToArgvW` formal Win32 APIs, security sanitization for elevated processes (matching both Windows restricted tokens and Linux `AT_SECURE`), and App Paths registry-based executable discovery (an Impossible OS differentiator: avoids `PATH` pollution). §18 adds cmd.exe dynamic pseudo-variables (`%CD%`, `%ERRORLEVEL%`, `%RANDOM%`) and delayed `!VAR!` expansion; §19 hardens the shipped Rtl expansion path (saturating output count + `%=X:%` parity), §20 closes the three gates that path must clear before anything user-reachable sits on it (bounded block lookup, a trusted extent for foreign blocks, and the at-cap allocation), and §21 provides the ntdll Rtl environment exports real Win32 apps and the CRT import over the §5 syscalls.

The `source` / `.` command (section 8 above) remains a differentiator over Windows cmd.exe.

---

## Unit Tests

> Wire into `test_runner_init()` via `test_register_env()` (see `src/kernel/test/test_runner.c` and `include/kernel/test/test.h`; same pattern as `TODO-11-peb-teb-user-abi.md` Unit Tests).
> Boot tests run with `debug=1` or `test=1` in boot.conf.

- [ ] Create `src/kernel/test/test_env.c` with:
  - `SetEnvironmentVariable("TEST_KEY", "hello")` + `GetEnvironmentVariable("TEST_KEY")` → `"hello"`
  - Delete: `SetEnvironmentVariable("TEST_KEY", NULL)` → subsequent get returns 0
  - `ExpandEnvironmentStrings("%TEST_KEY%")` → expanded value
  - Nonexistent var: `%NOEXIST%` left as-is or empty per Windows semantics
  - `GetEnvironmentStrings()` → block contains `PATH=`, `SystemRoot=`
  - Child inherits parent env: set var, fork, child reads same var
  - `PATH` search: `SearchPath(NULL, "hello.exe")` finds binary in `C:\Impossible\System32\`
  - Case insensitivity: `%path%` and `%PATH%` resolve to same value
  - Sorted order: after setting `ZZZ=1`, `AAA=2`, `MMM=3`, `GetEnvironmentStrings()` returns block in alphabetical order (AAA before MMM before ZZZ)
  - Name validation: `env_set(t, "BAD=NAME", "val")` returns error; `env_set(t, "", "val")` returns error
  - Size limit: `env_set` with 33,000-char value returns `STATUS_NAME_TOO_LONG`
  - Hidden drive var: `env_set_drive_cwd(t, 'C', "C:\\Users")` → `env_get(t, "=C:")` returns `"C:\\Users"`; hidden var sorts before `AAA` in block output
  - `PATHEXT` search: set `PATHEXT=.CMD;.EXE`; place `test.cmd` and `test.exe` in PATH dir; `shell_find_command("test")` returns `.cmd` variant first
  - `CommandLineToArgvW`: `"foo \"bar baz\" qux"` → 3 args: `foo`, `bar baz`, `qux`; `"a\\\\\"b"` → `a\\"b` (2n+1 rule)
  - Security: elevated task with `env_sanitize_for_elevation` strips `LD_PRELOAD` from environ
  - `CREATE_UNICODE_ENVIRONMENT`: spawn with custom UTF-16 env block succeeds only when the flag is set; rejected or mis-decoded when omitted (parity with Microsoft Learn examples)
  - `SetSearchPathMode`: toggling documented `BASE_SEARCH_PATH_*` mode changes whether `SearchPathW` consults the current directory for a probe name
  - Lifetime (§1): `env_get_copy(t, "PATH", buf, n)` returns the value in `buf`; after `env_unset(t, "PATH")` a prior `env_get` borrowed pointer is NOT dereferenced (copy-out path is UAF-safe)
  - Single-pass (§3): `env_set(t, "A", "%B%")`, `env_set(t, "B", "x")`, `env_expand(t, "%A%")` -> `"%B%"` literal, NOT `"x"` (no recursive re-expansion)
  - Exec size cap (§4): a `SYS_EXEC` with aggregate argv+envp over `ARG_MAX` returns `STATUS_QUOTA_EXCEEDED`; per-string over 32 KiB rejected before allocation
  - Round-trip (§4/§15): encode `argv = {"a b", "c\"d", ""}` to a CommandLine, then `CommandLineToArgvW` returns the same three args
  - Empty cmdline (§15): `CommandLineToArgvW(L"", &n)` -> `n==1`, `argv[0]` = module path (not `""`)
  - Pseudo-vars (§18): `%RANDOM%` in 0-32767 and varies; `%ERRORLEVEL%` equals the last command's exit code
  - Delayed expansion (§18): under `enabledelayedexpansion`, `!VAR!` reflects a value set earlier in the same block; `%VAR%` does not
  - Rtl exports (§21): `RtlQueryEnvironmentVariable_U` returns the same value as `NtQueryEnvironmentVariable` for `"PATH"`
- [ ] Register in `test_runner_init()`: `test_register_env()`
- [ ] Commit: `"test: add environment variables test suite"`

**Test checkpoint:** `bash scripts/test.sh SUITE=abi` reports every `test_env_*` case PASS; `tail -1 build/build.log` is `=== BUILD OK ===`. QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## Verification

- [ ] **env_get/set**: kernel unit test: create a task with empty environ; `env_set(t, "GREETING", "hello")`; `env_get(t, "GREETING")` → `"hello"`; `env_unset(t, "GREETING")`; `env_get(t, "GREETING")` → `NULL`.
- [ ] **env_expand**: `env_set(t, "NAME", "World")`; `env_expand(t, "Hello %NAME%!", buf, ...)` → `"Hello World!"`; `env_expand(t, "%%", buf, ...)` → `"%%"` (Win32: empty var preserved, NOT a cmd escape).
- [ ] **Default vars**: boot to shell; run `SET` with no args; output must include `PATH=`, `SYSTEMROOT=`, `TEMP=`, `USERNAME=`, `COMPUTERNAME=`.
- [ ] **PATH lookup**: place a test binary in `C:\Impossible\Bin\`; type just its name without path in the shell; it must launch.
- [ ] **argv round-trip**: run `echo hello world`; child process must receive `argc=3`, `argv=["echo","hello","world"]`; verify via a debug print in the program's `main()`.
- [ ] **NtQueryEnvironmentVariable**: user-mode test calls `NtQueryEnvironmentVariable` with `"PATH"` → returns the PATH string.
- [ ] **GetEnvironmentVariableW**: `GetEnvironmentVariableW(L"SYSTEMROOT", buf, MAX_PATH)` → `L"C:\\Impossible"`.
- [ ] **.profile**: create `C:\Users\Default\.profile` with `SET TEST_VAR=from_profile`; restart shell; run `ECHO %TEST_VAR%` → `"from_profile"`.
- [ ] **Remaining limits**: `ExpandEnvironmentStringsW` with deeply nested `%A%%B%%C%` (> 4 levels) must return the partially expanded string rather than hanging; `WM_SETTINGCHANGE` broadcast requires the window manager to be running (§9): skip in headless QEMU test.
- [ ] **Sorted block**: `GetEnvironmentStrings()` output is alphabetically sorted; insert `ZZZ` then `AAA`; `AAA` must appear before `ZZZ` in the block.
- [ ] **PATHEXT**: set `PATHEXT=.CMD;.EXE`; create both `test.cmd` and `test.exe` in `C:\Impossible\Bin\`; type `test` in shell; `test.cmd` must be selected (`.CMD` first in PATHEXT).
- [ ] **Hidden drive vars**: after `SetCurrentDirectory("D:\\Docs")`, `GetEnvironmentStrings()` block contains `=D:=D:\Docs` before any letter-named variable.
- [ ] **CreateEnvironmentBlock**: `env_create_block(caller, NULL, 1, &blk)` returns a sorted UTF-16 caller-env block; `env_destroy_block(blk)` frees via the pointer alone; non-NULL token / no-inherit returns `ENV_ERR_UNSUPPORTED` (deferred).
- [ ] **SearchPathW**: `SearchPathW(NULL, L"notepad", L".exe", ...)` → finds `C:\Impossible\System32\notepad.exe`; `SearchPathW(NULL, L"nonexistent", L".exe", ...)` → returns 0, `GetLastError() == ERROR_FILE_NOT_FOUND`.
- [ ] **CommandLineToArgvW**: `CommandLineToArgvW(L"a.exe \"hello world\" test", &argc)` → `argc==3`, `argv[0]=="a.exe"`, `argv[1]=="hello world"`, `argv[2]=="test"`.
- [x] **Security sanitization** (§16): a High/System task reads `LD_PRELOAD` as absent and `env_build_block` omits it; `env_sanitize_for_elevation` strips blocklisted vars. Elevated-CHILD strip deferred (TODO-12 §7, TODO-15 §9).
- [ ] **App Paths**: register `myapp.exe` in `HKLM\...\App Paths\myapp.exe` pointing to `C:\Programs\MyApp\myapp.exe`; type `myapp` in shell (not in PATH); App Paths fallback finds and launches it.
- [ ] Commit: `"kernel/env: environment variables, argv, Win32 GetEnvironmentVariable, PATH lookup, .profile"`

**Test checkpoint:** All bullets in this section pass on QEMU WHPX, QEMU TCG, VirtualBox, and bare metal where applicable; `bash scripts/test.sh SUITE=abi` green; `tail -1 build/build.log` is `=== BUILD OK ===`.

**Test runner:** `scripts\debug\kernel\run-abi-tests.bat` (SUITE=abi)

---

## History

| Date       | Action   | Summary                                                                 |
| ---------- | -------- | ----------------------------------------------------------------------- |
| 2026-04-13 | validate | History present; §9 WM bullet colon fix; Verification prose (no `--` clause join); Unit Tests single blank line before **Test checkpoint**. |
| 2026-04-14 | validate | §1 through §17: Commit before **Test checkpoint**; Unit Tests + Verification checkpoints; Impl Order Depends On T11/T05/T11/T13; D12 T04 Inputs path; OS table re-padded; argv row T11 §7; platform tails; History added. |
| 2026-04-15 | gap-analysis | Web: Win32 sorted env block + CREATE_UNICODE (MS Learn), GetEnvironmentStringsW, CreateEnvironmentBlock/LoadUserProfile, SetSearchPathMode, Linux AT_SECURE/systemd user env; code absent; +§10/§13/§14 items; OS rows; T12 SSDT owner fix; T11/T09 doc fixes. |
| 2026-04-16 | validate | IMPORTANT **Current state:** label; prose `--` fixes (IMPORTANT, setx, PATHEXT, §12 summary, OS blurb); §10 CREATE_UNICODE nested under `env_build_block` (10 top-level `- [ ]` before Commit); §14 checkpoint adds `SetSearchPathMode`; Inputs + runner + OS placement rechecked. |

---
