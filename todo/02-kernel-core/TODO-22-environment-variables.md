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
| 💎   |  13   | CreateEnvironmentBlock / DestroyEnvironmentBlock   | §2, §10, §12, T15 §4       |  [ ]   |
| 💎   |  14   | SearchPathW / SearchPathA Win32 API                | §7, §6                     |  [ ]   |
| 💎   |  15   | CommandLineToArgvW Win32 API                       | §4, §6                     |  [ ]   |
| 💎   |  16   | Environment variable security & sanitization       | §1, T15 §4                 |  [ ]   |
| ⭐   |  17   | App Paths registry-based executable lookup         | §7, T14 §4                 |  [ ]   |
| 💎   |  18   | cmd.exe dynamic pseudo-vars & delayed `!VAR!`      | §3, §7                     |  [ ]   |
| 💎   |  19   | ntdll Rtl environment layer                        | §5, §6                     |  [ ]   |

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
> **Accepted:** [H] task_cleanup reap barrier lacks all-CPU quiescence for env_free (single-CPU scheduler today) -> XREF: 03-memory-concurrency/TODO-07-smp-phase2.md (item: "task_cleanup reap barrier" at line 122)
> **Accepted:** [M] environ_lock inherits the mutex_t waiter-queue SMP race (unreachable on single-CPU) -> XREF: 03-memory-concurrency/TODO-08-advanced-sync.md §11 (item: "Wait-queue protection" at line 275)
> **Accepted:** [M] env names case-folded ASCII-only; non-ASCII compared case-sensitively -> XREF: 02-kernel-core/TODO-22-environment-variables.md §10 (item: "Upgrade env name case-folding" at line 135)
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
  - `Environment`: kernel-resident UTF-16 block; if NULL, synthesized from `task_current()`'s UTF-8 environ (authoritative store) via `env_build_block_utf16` -- NOT the stale user-mapped PEB block, honoring the documented NULL "calling process's own block" form from kernel-resident memory
  - Kernel-resident-input contract: the double-NUL scan is HARD-BOUNDED by `RTL_ENV_BLOCK_MAX_WCHARS` (non-terminating block -> `STATUS_INVALID_PARAMETER`); a raw user block must be probed+copied into a kernel snapshot by the Win32 boundary first (owned by §6 `ExpandEnvironmentStringsW`)
  - `Source`/`Destination`: `UNICODE_STRING` (validated: even byte Length, Length <= MaximumLength, Buffer for non-empty); same single-pass `%VAR%`/`%%` semantics as `env_expand`; case-insensitive ASCII-fold name match (consistent with the UTF-8 store)
  - `ReturnedLength`: required buffer size in bytes including the WCHAR NUL; `STATUS_BUFFER_TOO_SMALL` when too small, with NO partial output and `Destination->Length` unchanged; on success `Destination->Length` = result bytes excluding NUL (required length in a wide accumulator)
  - Used by `ExpandEnvironmentStringsW` (§6) and by the shell for Win32-mode argument expansion

- [x] Commit: `"kernel/env: env_expand %VAR% substitution, RtlExpandEnvironmentStrings_U"`

**Test checkpoint:** `env_expand` replaces `%VAR%`; `%%` preserved verbatim (Win32, not a cmd escape); `%A%`=`%B%`, `%B%`=`x`, `env_expand("%A%")` -> literal `%B%` (single-pass, no recursion); `RtlExpandEnvironmentStrings_U` expands over an explicit block and returns `STATUS_BUFFER_TOO_SMALL` + required length on an undersized `Destination`. QEMU WHPX + TCG; VirtualBox; bare metal.
> **Test runner:** `scripts\debug\kernel\run-abi-tests.bat` (SUITE=abi) | 27 env_expand/Rtl suites added, 0 failures
> **Notes:**
> - **What shipped:** `env_expand` (UTF-8 single-pass `%VAR%`, Win32 `%%`-verbatim, bounded) in `env.c`; `RtlExpandEnvironmentStrings_U` (UTF-16) in new `nt/nt_rtlenv.c`; `env_build_block_utf16` (nls UTF-8->UTF-16) in `env.c`.
> - **How it integrates:** `env_expand` holds one `env_lock` across the walk; the Rtl core is a bounded kernel-resident two-pass transformer (count then write, no partial output, pass counts verified against between-pass mutation).
> - **Downstream:** §6 `ExpandEnvironmentStrings{W,A}` call `RtlExpandEnvironmentStrings_U`; §6 owns probing+copying a user Environment block into a kernel snapshot first (design + adversarial adoptions in the commit message).
> - **Scope boundary:** §3 owns the two primitives; §5/§6 own the syscall/Win32 boundary; cmd `%%`/`!VAR!` is §18; full Unicode name folding + the PMM allocator SMP lock are tracked elsewhere.
> **Verified:** 2026-07-13 | commit `c89eb4c8` | 3/3 items | build OK | tests 1271/1271 PASS
> **Accepted:** [H] large synth env block (>4 KiB) routes through the unlocked `pmm_alloc_contiguous` (pre-existing kernel-wide gap; env is one of many callers) -> XREF: 03-memory-concurrency/TODO-03-advanced-allocator.md §1 (item: "PMM bitmap SMP locking" at line 103)
> **Deferred:** [H] two-pass linear block scan per `%VAR%` (~1.4e9 WCHARs worst case) is a DoS once user-exposed -> XREF: 02-kernel-core/TODO-22 §6 (item: "Bound RtlExpandEnvironmentStrings_U scan cost before user exposure" at line 287)
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
> **Accepted:** [H] SYS_EXEC signature change is invisible to the ABI fingerprint (hashes SYS_* numbers, not signatures; theoretical stale-binary handshake bypass in the monolithic build) -> XREF: 00-infrastructure/TODO-04 §18 (item: "Fold syscall arg counts into the ABI fingerprint" at line 632)
> **Accepted:** [H] fork does not copy the parent environ, so exec(envp==NULL) inherits empty; race-safe env_copy needs atomic slot publication -> XREF: 02-kernel-core/TODO-12-native-api-ssdt.md §7 (item: "Wire `env_copy()` into every child path")
> **Accepted:** [M] PEB CommandLine truncates a >~2 KiB full-argv command line + UTF-8 argv mojibakes (single-page RTLPP, byte-widening) -> XREF: 02-kernel-core/TODO-11 §5 (item: "`CommandLine` fidelity" at line 196)
> **Accepted:** [M] `copy_from_user` is not fault-recoverable (in-range unmapped page faults in kernel) -> XREF: 03-memory-concurrency/TODO-02 §4 (item: "Audit all syscall handlers" at line 126)
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
> **Accepted:** [C] range-only `ProbeForWrite`/`ProbeForRead` + non-fault-recoverable `copy_to_user`/`copy_from_user` is a kernel-crash / kernel-write exposure for the ring-3 path (systemic to every Probe + `copy_*_user` syscall, incl. the reviewed `NtQueryCurrentDirectory`; not new in this class). -> XREF: `02-kernel-core/TODO-23-exception-dispatch-seh.md §13` (item: "`src/kernel/probe.c` -- implementation; `safe_return_rip` slot in CPU-local area" at line 378) -- re-enters when fault-recoverable `try_copy_*_user` lands.
> **Accepted:** [M] name/value size limits are enforced in UTF-8 BYTES (`ENV_NAME_MAX`/`ENV_VALUE_MAX`, matching the UTF-8 storage layer), so a UTF-16 input within the Windows CHARACTER limit but over the byte cap is cleanly rejected (`STATUS_NAME_TOO_LONG`), not corrupted. -> XREF: §10 (item: "reconcile UTF-16 character-count limits with UTF-8 storage byte caps").
> **Accepted:** [H] aggregate 1 MiB per-process env quota not enforced; §5 makes it user-reachable via `NtSetEnvironmentVariable` (env is already bounded to ~16 MiB/process by `ENV_MAX_ENTRIES`, so not unbounded) -> XREF: `02-kernel-core/TODO-22-environment-variables.md` §10 (item: "enforce a 1 MiB per-process sanity cap" at line 278)
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
- [/] For a user-supplied UTF-16 `Environment` block, `ProbeForRead` + copy into a kernel snapshot and verify its double-NUL terminator is within the copied length before the Rtl call (`nt/nt_rtlenv.h`). NULL needs no probe
- [/] Bound RtlExpandEnvironmentStrings_U scan cost before user exposure: two passes linear-scan the block per %VAR% (~1.4e9 WCHARs worst case). Add a one-time block index, or cap Source at the ExpandEnvironmentStrings boundary

- [/] `GetEnvironmentStringsW()`:
  - Walk `current_task->environ[]`; convert each `"KEY=VALUE"` to UTF-16; pack into a contiguous buffer as null-separated entries with a double-null at the end (matches the Win32 format); allocate with `LocalAlloc`
  - Return pointer; caller must call `FreeEnvironmentStringsW` when done
- [/] `GetEnvironmentStringsA()` -- UTF-8 variant; same format in ANSI
- [/] `FreeEnvironmentStringsW(pEnvBlock)` → `LocalFree(pEnvBlock)`

- [/] `GetCommandLineW()` → returns `PEB->ProcessParameters->CommandLine.Buffer` (UTF-16 command line string, built by `TODO-11-peb-teb-user-abi.md §7` from `task->argv`)
- [/] `GetCommandLineA()` → convert `CommandLine.Buffer` UTF-16 → UTF-8 and cache in a static per-process buffer (allocated on first call)

- [/] Commit: `"kernel/env: GetEnvironmentVariable, SetEnvironmentVariable, ExpandEnvironmentStrings, GetCommandLine Win32 wrappers"`

> **Deferred:** [blocked] §6 Win32 env/command-line wrappers need user-mode kernel32/ntdll runtime primitives that do not exist yet -- `LocalAlloc`/`LocalFree`, `SetLastError`/`GetLastError`, and UTF-8<->UTF-16 conversion (`MultiByteToWideChar`/`WideCharToMultiByte`); `GetCommandLineW` also needs the PEB CommandLine. The syscall layer (§5) is shipped; these are user-platform runtime prereqs owned elsewhere. -> XREF: `12-user-platform-sdk/TODO-04-ntdll-user-runtime.md` §7 (item: "kernel32 conversion + last-error shims") + §2 (item: "RtlHeap Process Heap Allocator" at line 169); `02-kernel-core/TODO-11-peb-teb-user-abi.md` §7 for the PEB CommandLine.

**Test checkpoint:** `GetEnvironmentVariableW` returns `SYSTEMROOT`; `ExpandEnvironmentStringsW` expands. QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 7. Shell Integration: PATH Lookup & SET/ECHO

- [ ] `shell_find_command(name, out_path, max)` -- one shared extension rule (matches SDK `TODO-02 §5`): a `name` WITH an explicit extension is probed verbatim only; an extensionless `name` iterates `PATHEXT` in order (§11):
  1. If `name` contains `\` or `/`: explicit path; probe verbatim if it has an extension, else iterate `PATHEXT` against it; return first match
  2. Otherwise: retrieve `PATH` value via `env_get(current_task, "PATH")`
  3. Split `PATH` on `;` into directory list; get `PATHEXT` via `env_get(current_task, "PATHEXT")` (split `;`; empty/unset -> `.EXE`)
  4. For each directory: if `name` has an extension probe `dir\name` verbatim, else `dir\name{ext}` for each `PATHEXT` ext in order; `vfs_stat` each; return first hit
  5. If no match: return `SHELL_COMMAND_NOT_FOUND`
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
> **Accepted:** [M] NULL-environment `RtlExpandEnvironmentStrings_U` caps at 128 KiB while storage allows 1 MiB (raising it safely needs a uint64 expansion-length count) -> XREF: 02-kernel-core/TODO-22 §19 (item: "Raise the NULL-env expansion cap above 128 KiB" at line 600)
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
> **Accepted:** [H] the legacy `SYS_OPENFILE` passes a RAW user pointer to `task_resolve_path`, so the nested-qualifier guard (and `vfs_resolve_path`'s own drive select) can be defeated by a concurrent user-buffer mutation (double-fetch). Pre-existing; the NT handlers already snapshot. The guard is sound for snapshotted callers -> XREF: `03-memory-concurrency/TODO-02-memory-security.md` §4 (item: "Audit all syscall handlers" at line 126).
> **Quality reviewed:** 2026-07-14 | Codex 18x (design, adversarial, test-coverage, consistency, perf, re-adversarial) | 12H fixed, 4 accepted-XREF | scope: kernel-code-quality

---

## 13. CreateEnvironmentBlock / DestroyEnvironmentBlock

- [ ] `CreateEnvironmentBlock(LPVOID *lpEnvironment, HANDLE hToken, BOOL bInherit)` -- Win32 API from `userenv.dll`:
  - If `hToken == NULL`: build an environment block containing **system variables only** (from `HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Environment`)
  - If `hToken != NULL`: merge system variables + user variables from `HKCU\Environment` for the user identified by the token; token must have `TOKEN_QUERY` and `TOKEN_DUPLICATE` access
  - If `bInherit == TRUE`: start with the calling process's environment, then overlay system and user variables from the Registry
  - If `bInherit == FALSE`: build a fresh block from Registry only (no inheritance)
  - Output: a contiguous UTF-16 sorted environment block (double-NUL terminated), allocated with `LocalAlloc`; caller must free with `DestroyEnvironmentBlock`
- [ ] `DestroyEnvironmentBlock(LPVOID lpEnvironment)` → `LocalFree(lpEnvironment)`
- [ ] `ExpandEnvironmentStringsForUserW(HANDLE hToken, LPCWSTR lpSrc, LPWSTR lpDst, DWORD dwSize)` -- expand `%VAR%` using the environment of the user identified by `hToken`:
  - Build a temporary env block via `CreateEnvironmentBlock(NULL, hToken, FALSE)`
  - Parse the block, match `%VAR%` references against it
  - Token requires `TOKEN_QUERY`, `TOKEN_DUPLICATE`, and `TOKEN_IMPERSONATE`
  - If `hToken == NULL`, expand using system variables only
- [ ] These APIs are used by services (e.g. Task Scheduler, logon service) that launch processes on behalf of other users
- [ ] **Profile load vs user vars:** Per Microsoft Learn `CreateEnvironmentBlock` remarks, user-specific variables such as `%USERPROFILE%` apply only after the user's profile is loaded (`LoadUserProfile` on Win32). Until D02 T21 or D02 T15 defines an equivalent profile-load sequence, return documented failure (or system-only block) when a token requires HKCU-backed vars without a loaded profile; do not invent profile paths.

- [ ] Commit: `"kernel/env: CreateEnvironmentBlock, DestroyEnvironmentBlock, ExpandEnvironmentStringsForUser"`

**Test checkpoint:** `CreateEnvironmentBlock` + `DestroyEnvironmentBlock` no leak; sorted block. QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 14. SearchPathW / SearchPathA Win32 API

- [ ] `SearchPathW(lpPath, lpFileName, lpExtension, nBufferLength, lpBuffer, lpFilePart)` -- Win32 API from `kernel32.dll`:
  - If `lpPath == NULL`: search order is (1) the directory from which the application loaded, (2) the current directory, (3) the system directory (`GetSystemDirectory`), (4) the Windows directory (`GetWindowsDirectory`), (5) directories listed in `PATH`
  - If `lpPath != NULL`: search only the directories in `lpPath` (semicolon-delimited)
  - `lpExtension`: extension to append if `lpFileName` has none; first character must be `.` (e.g. `".exe"`); if `lpFileName` already has an extension, this parameter is ignored
  - `lpFilePart`: receives pointer to the file-name component in the returned path
  - Return: length of the path found (in characters, not including NUL); 0 if not found (`GetLastError` returns `ERROR_FILE_NOT_FOUND`)
  - Security note: Windows provides `SetSearchPathMode` to control whether the current directory is searched before or after system directories; default is before (risky for DLL hijacking)
- [ ] `SearchPathA` -- ANSI wrapper; converts to UTF-16, calls `SearchPathW`, converts result back
- [ ] `NeedCurrentDirectoryForExePathW(ExeName)` -- returns `TRUE` if the current directory should be included in the search path for the given executable; returns `FALSE` if `NoDefaultCurrentDirectoryInExePath` environment variable is set (security hardening)
- [ ] `SetSearchPathMode(DWORD BaseSearchPathMode)` (kernel32): persist `BASE_SEARCH_PATH_*` bits that govern `SearchPathW` / CWD inclusion; honor the same semantics Microsoft documents for `SetSearchPathMode` and `NeedCurrentDirectoryForExePath*`.

- [ ] Commit: `"kernel/env: SearchPathW/A Win32 API, NeedCurrentDirectoryForExePathW"`

**Test checkpoint:** `SearchPathW` finds System32 binary; missing returns 0 + `ERROR_FILE_NOT_FOUND`; `SetSearchPathMode` toggles CWD participation per `NeedCurrentDirectoryForExePathW` / documented `BASE_SEARCH_PATH_*` behavior. QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 15. CommandLineToArgvW Win32 API

- [ ] `CommandLineToArgvW(LPCWSTR lpCmdLine, int *pNumArgs)` -- Win32 API from `shell32.dll`:
  - Parses a Unicode command-line string into an argv-style array of pointers
  - Backslash/quote escaping rules (must match Windows exactly):
    - 2n backslashes + `"` → n backslashes + begin/end quote (toggle "in quotes" mode)
    - (2n+1) backslashes + `"` → n backslashes + literal `"` (no toggle)
    - n backslashes not followed by `"` → n backslashes verbatim
  - Outside quotes: whitespace (space/tab) terminates the current argument
  - Inside quotes: whitespace is part of the argument
  - `argv[0]` has special parsing: if it starts with `"`, everything up to the closing `"` is argv[0]; otherwise, everything up to the first whitespace
  - Return: pointer to `LPWSTR *` array, with `*pNumArgs` set to the count; the array and all strings are allocated as a single `LocalAlloc` block; caller must `LocalFree` the returned pointer
- [ ] Edge case: empty string input → `*pNumArgs = 1`, `argv[0]` = path to the CURRENT EXECUTABLE (module path), NOT `""` (MS Learn `CommandLineToArgvW`)
- [ ] Edge case: `NULL` input → return `NULL` (Windows behavior)
- [ ] Round-trip test with §4's `argv_to_cmdline` encoder: encode `argv = {"a b", "c\"d", ""}` to a CommandLine, then `CommandLineToArgvW` returns the same three args (encoder owned by §4; §15 owns decode + the round-trip test)

- [ ] Commit: `"kernel/env: CommandLineToArgvW Win32 API (shell32)"`

**Test checkpoint:** `CommandLineToArgvW` quote rules match Windows samples. QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 16. Environment Variable Security & Sanitization

- [ ] When a process runs with elevated privileges (e.g. admin token from UAC elevation, or a service), sanitize the inherited environment by stripping variables that could enable privilege escalation:
  - Strip `LD_PRELOAD`, `LD_LIBRARY_PATH` (Linux compat layer only -- XREF `TODO-23-exception-dispatch-seh.md`)
  - Strip any variable whose name starts with `_IMPOSSIBLE_DEBUG_` in non-debug builds (prevent debug knobs from being inherited into privileged processes)
- [ ] `env_sanitize_for_elevation(task)` -- called by `NtCreateProcess` when the child process has a higher integrity level than the parent:
  - Walk `task->environ[]`; remove blocklisted variables
  - Log removed variables to security audit log at `LOG_NOTICE` level
- [ ] Parallel to Linux `secure_getenv()` / glibc `AT_SECURE`: if `task->token` has setuid-equivalent elevation, `env_get` for blocklisted names returns `NULL` even if the variable is present

- [ ] `NtSetEnvironmentVariable` must validate:
  - Name does not contain `=` (except hidden `=X:` drive vars)
  - Name is not empty (zero-length)
  - Name length ≤ 256 characters
  - Value length ≤ 32,767 characters
  - Total environment block size after insertion ≤ 1 MiB
  - Return `STATUS_INVALID_PARAMETER` for invalid names, `STATUS_NAME_TOO_LONG` for oversized values, `STATUS_QUOTA_EXCEEDED` for block overflow
- [ ] `NtQueryEnvironmentVariable` must validate:
  - Output buffer is properly `ProbeForWrite`-validated before any kernel data is written
  - Name string is `ProbeForRead`-validated before dereferencing
  - Return `STATUS_BUFFER_TOO_SMALL` with required length if buffer is insufficient (do not truncate silently)

- [ ] Commit: `"kernel/env: security sanitization for elevated processes, input validation"`

**Test checkpoint:** Elevated child strips `LD_PRELOAD`; invalid name returns `STATUS_INVALID_PARAMETER`. QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 17. App Paths Registry-Based Executable Lookup

- [ ] Windows provides an alternative to the `PATH` variable: the `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\` registry key, where each subkey is named after an executable (e.g. `notepad.exe`) and the default value is the full path
- [ ] `app_paths_lookup(name, out_path, max)` -- check `App Paths\{name}` subkey:
  1. Open `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\{name}` (if name has no extension, append `.exe`)
  2. Read default value → full executable path
  3. Read `Path` value (optional) → additional directory to add to the process's PATH at launch time
  4. Return the full path or `NULL` if not found
- [ ] Integration: `shell_find_command` (§7) checks App Paths **after** the standard PATH search fails; this matches Windows `ShellExecute` behavior where App Paths is a fallback
- [ ] Per-user App Paths: also check `HKCU\SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\{name}` (HKCU takes precedence over HKLM)
- [ ] `app_paths_register(name, full_path, additional_path)` -- API for installers to register executables without modifying PATH (→ XREF `10-platform-services/TODO-03-updates-packages.md`)

- [ ] Commit: `"kernel/env: App Paths registry-based executable lookup"`

**Test checkpoint:** App Paths resolves `myapp` not on PATH. QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 18. cmd.exe Dynamic Pseudo-Variables & Delayed Expansion

cmd exposes computed-at-expansion pseudo-variables (never stored in `environ[]`) plus an opt-in delayed `!VAR!` mode; both are cmd-parser concerns distinct from §3's generic single-pass `%VAR%`.

- [ ] Dynamic pseudo-vars resolved at expansion time by the shell (§7), never stored, real stored vars take precedence: `%CD%`, `%DATE%`, `%TIME%`, `%RANDOM%` (0-32767), `%ERRORLEVEL%`, `%CMDCMDLINE%`, `%CMDEXTVERSION%`
- [ ] `%ERRORLEVEL%` tracks the last command's exit code; the shell updates it after each command
- [ ] Delayed expansion: `setlocal enabledelayedexpansion` / `cmd /V:ON` enables `!VAR!` re-read at execution time (needed inside `FOR`/`IF` blocks where `%VAR%` is fixed at parse time per §3)
- [ ] `SET` enforces cmd's tighter documented per-var / total caps (fidelity beyond §10's Win32 API-level limits)
- [ ] Batch `%~` modifiers (`%~dp0`, `%~nx1`) extend D12 T02's `%1..%9` (→ XREF `12-user-platform-sdk/TODO-02-env-vars-process-abi.md`); `FOR`/`IF`/`setlocal` batch semantics need a batch-processor TODO
- [ ] Commit: `"shell: cmd.exe dynamic pseudo-variables + delayed !VAR! expansion"`

**Test checkpoint:** `echo %RANDOM%` varies across calls; `%ERRORLEVEL%` reflects the last exit code; `!VAR!` re-reads a var set earlier in the same `enabledelayedexpansion` block. Serial/klog observable. QEMU WHPX + TCG; VirtualBox; bare metal.

---

## 19. ntdll Rtl Environment Layer

Real Win11 resolves env access entirely in user-mode via ntdll `Rtl*Environment*` over the PEB-resident block (no syscall). Impossible OS stores env in the kernel (`task->environ`, §1), so these Rtl functions are thin ntdll wrappers over the §5 Nt syscalls -- provide them so ntdll-importing apps and the CRT resolve.

> [!NOTE]
> Architecture divergence: env lives in the kernel here vs the PEB in Windows; the syscall-backed Rtl layer is the compat bridge. The kernel-vs-PEB storage choice is operator-reserved (security/ABI) -- flag for review, do not silently redesign §1/§5.

- [ ] `RtlQueryEnvironmentVariable_U(Environment, Name, Value)` -- ntdll export routing to §5 `NtQueryEnvironmentVariable` under the process-env lock
- [ ] `RtlSetEnvironmentVariable(Environment, Name, Value)` -- routes to §5 `NtSetEnvironmentVariable`; NULL value deletes
- [ ] `RtlCreateEnvironment` / `RtlDestroyEnvironment` -- allocate/free a standalone UTF-16 env block (for `CreateProcess lpEnvironment`, §10/§13)
- [ ] kernel32 `GetEnvironmentVariable`/`SetEnvironmentVariable` (§6) route through these Rtl exports so ntdll imports resolve; `RtlExpandEnvironmentStrings_U` already lives in §3
- [ ] Raise the NULL-env expansion cap above 128 KiB: `rtl_env_expand_pass` (`nt_rtlenv.c`) counts output in a uint32 (wraps at 2^32), so `RTL_ENV_BLOCK_MAX_WCHARS` stays 64 KiWCHAR; switch to a saturating uint64 count then raise the cap
- [ ] Commit: `"ntdll: Rtl environment layer over the Nt env syscalls"`

**Test checkpoint:** an app importing `RtlQueryEnvironmentVariable_U` from ntdll resolves and returns the same value as `NtQueryEnvironmentVariable`; `RtlCreateEnvironment` builds a sorted block. Serial/klog observable. QEMU WHPX + TCG; VirtualBox; bare metal.

---

## OS Comparison

| ⭐   | Feature                    | 🪟 Win11               | 🐧 Linux              | 🚀 Impossible OS      |
| --- | -------------------------- | --------------------- | -------------------- | -------------------- |
| 💎   | Per-process env storage    | ✅ PEB UTF-16          | ✅ POSIX environ      | ✅ §1 kernel API      |
| 💎   | `%VAR%` / `$VAR`           | ✅ cmd `%VAR%`         | ✅ bash `$VAR`        | ✅ §3 `env_expand`    |
| 💎   | System defaults            | ✅ Session Manager     | ✅ `/etc/environment` | ✅ §2 Registry+synth  |
| 💎   | argv to child              | ✅ CRT cmdline         | ✅ execve argv        | ✅ §4 crt0->main      |
| 💎   | Env var read/write         | ⚠️ ntdll Rtl usermode | ⚠️ libc only         | ⚠️ §5 Nt✅, §19 Rtl   |
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
| 💎   | CreateEnvBlock             | ✅ userenv             | ❌ none               | ⬜ §13                |
| 💎   | ExpandForUser              | ✅ userenv             | ❌ none               | ⬜ §13                |
| 💎   | SearchPathW                | ✅ kernel32            | ⚠️ execvp libc       | ⬜ §14                |
| 💎   | SetSearchPathMode          | ✅ kernel32            | ❌ N/A                | ⬜ §14                |
| 💎   | CmdLineToArgvW             | ✅ shell32             | ❌ wordexp diff       | ⬜ §15                |
| 💎   | Elevated env strip         | ✅ restricted          | ✅ AT_SECURE          | ⬜ §16                |
| ⭐   | App Paths                  | ✅ HKLM App Paths      | ❌ none               | ⬜ §17                |
| 💎   | Dynamic pseudo-vars        | ✅ %CD%/%ERRORLEVEL%   | ⚠️ $PWD/$?/$RANDOM   | ⬜ §18                |
| 💎   | Delayed `!VAR!` expansion  | ✅ cmd /V              | ❌ N/A                | ⬜ §18                |
| 💎   | ntdll Rtl env layer        | ✅ ntdll usermode      | ❌ none               | ⬜ §19                |
| 💎   | Exec argv+envp size cap    | ⚠️ per-var only       | ✅ E2BIG/ARG_MAX      | ✅ §4 frame+ARG_MAX   |

After §1 through §9, Impossible OS reaches base Windows 11 and Linux parity for core environment variable features: per-process UTF-8 env storage, `%VAR%` expansion, Registry-backed system defaults, Win32 `GetEnvironmentVariable` / `ExpandEnvironmentStrings`, PATH lookup, `SET`, and `.profile` startup.

§10 through §17 close the remaining competitive gaps: sorted environment blocks (a hard Windows contract), `PATHEXT` extension search order, hidden drive-letter CWD variables, `CreateEnvironmentBlock` for service-launched processes, `SearchPathW` / `CommandLineToArgvW` formal Win32 APIs, security sanitization for elevated processes (matching both Windows restricted tokens and Linux `AT_SECURE`), and App Paths registry-based executable discovery (an Impossible OS differentiator: avoids `PATH` pollution). §18 adds cmd.exe dynamic pseudo-variables (`%CD%`, `%ERRORLEVEL%`, `%RANDOM%`) and delayed `!VAR!` expansion; §19 provides the ntdll Rtl environment layer real Win32 apps and the CRT import over the §5 syscalls.

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
  - Rtl layer (§19): `RtlQueryEnvironmentVariable_U` returns the same value as `NtQueryEnvironmentVariable` for `"PATH"`
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
- [ ] **CreateEnvironmentBlock**: call with a user token; returned block includes both `HKLM` system vars and `HKCU` user vars; block is sorted; `DestroyEnvironmentBlock` frees without leak.
- [ ] **SearchPathW**: `SearchPathW(NULL, L"notepad", L".exe", ...)` → finds `C:\Impossible\System32\notepad.exe`; `SearchPathW(NULL, L"nonexistent", L".exe", ...)` → returns 0, `GetLastError() == ERROR_FILE_NOT_FOUND`.
- [ ] **CommandLineToArgvW**: `CommandLineToArgvW(L"a.exe \"hello world\" test", &argc)` → `argc==3`, `argv[0]=="a.exe"`, `argv[1]=="hello world"`, `argv[2]=="test"`.
- [ ] **Security sanitization**: create elevated child process; parent sets `LD_PRELOAD=/evil.so`; child's `env_get("LD_PRELOAD")` returns `NULL` (stripped by `env_sanitize_for_elevation`).
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
