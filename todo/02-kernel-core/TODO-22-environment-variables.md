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
| 💎   |   2   | System default variables from Registry             | §1, T14 §4                 |  [ ]   |
| 💎   |   3   | `%VAR%` expansion (`env_expand`)                   | §1                         |  [ ]   |
| 💎   |   4   | argv array: kernel storage & shell parsing         | §1                         |  [ ]   |
| 💎   |   5   | Nt/Zw environment variable syscalls                | §1, T11 §2, T11 §5, T12 §4 |  [ ]   |
| 💎   |   6   | Win32 API wrappers                                 | §5                         |  [ ]   |
| 💎   |   7   | Shell integration (PATH lookup, SET, ECHO)         | §3, §4                     |  [ ]   |
| 💎   |   8   | `.profile` startup script                          | §7                         |  [ ]   |
| ⭐   |   9   | Environment change notifications & `sysdm.cpl` tab | §6, §8                     |  [ ]   |
| 💎   |  10   | Environment block sorting & size limits            | §1                         |  [ ]   |
| 💎   |  11   | PATHEXT variable & extension search order          | §7                         |  [ ]   |
| 💎   |  12   | Hidden drive-letter variables (`=C:`, `=D:`)       | §1, §10                    |  [ ]   |
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

**Test checkpoint:** `env_set`/`env_get_copy`/`env_unset`/`env_copy` on a test task; case-insensitivity; validation; truncating get returns full length; OOM-during-replace preserves the old value; >4 KiB value round-trips via PMM; `env_free` leaves no dangling pointers. `bash scripts/test.sh SUITE=abi` green (1188 passed, 0 failed, 0 leaked); `tail -1 build/build.log` is `=== BUILD OK ===`. QEMU WHPX + TCG; VirtualBox; bare metal.

> **Test runner:** `scripts\debug\kernel\run-abi-tests.bat` (SUITE=abi) | 14 env suites, 0 failures
> **Notes:**
> - **What shipped** -- `src/kernel/env.c` + `include/kernel/env.h`: per-task `environ`/`argv` storage + `env_get_copy`/`env_set`/`env_unset`/`env_copy`/`env_free`/`env_lock`/`env_peek_locked`; 14 tests in `src/kernel/test/test_env.c` (TEST_CAT_ABI).
> - **How it integrates** -- fields + `mutex_init` land once in the `task_init` all-slots loop; `env_free` wired at the `task_cleanup` reap barrier; build auto-discovers `env.c`.
> - **Downstream effects** -- unblocks env-copy wiring for `TODO-12-native-api-ssdt.md §7` (child env); storage base for later TODO-22 sections; Codex adoptions in the commit message.
> - **Canonical doc** -- [`include/kernel/env.h`](../../include/kernel/env.h) header contract (lock + reader-lifetime rules).
> - **Scope boundary** -- §1 owns kernel storage + the C API only; `%VAR%` expansion is §3, Nt syscalls §5, Win32 wrappers §6, argv/exec handoff §4, sorting/size-block §10.

---

## 2. System Default Variables from Registry

- [ ] `env_init_defaults(task)` -- called once for every newly created process:
  1. Read system env vars from Registry key `HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Environment` (→ XREF `TODO-14-registry-completion.md §4`); enumerate all values; call `env_set` for each
  2. Read user env vars from `HKCU\Environment`; set for each (user vars override system vars with the same name)
  3. Synthesise computed variables that cannot come from Registry:
     - `COMPUTERNAME` ← `HKLM\SYSTEM\ComputerName\ActiveComputerName\ComputerName` (default `"IMPOSSIBLE-PC"`)
     - `USERNAME` ← from the process's primary token UserSid → account name lookup (→ XREF `TODO-15-security-reference-monitor.md §7`); default `"Default"`
     - `USERPROFILE` ← `C:\Users\{USERNAME}\`
     - `APPDATA` ← `C:\Users\{USERNAME}\AppData\Roaming\`
     - `LOCALAPPDATA` ← `C:\Users\{USERNAME}\AppData\Local\`
     - `TEMP` / `TMP` ← `C:\Temp\` (also readable from Registry)
     - `PROCESSOR_ARCHITECTURE` ← `"AMD64"`
     - `NUMBER_OF_PROCESSORS` ← `HKLM\HARDWARE\CPU\Count` (default `"1"`)
     - `OS` ← `"Impossible_OS"`
     - `WINDIR` / `SYSTEMROOT` ← `C:\Impossible\`
     - `SYSTEMDRIVE` ← `C:\`
     - `PATH` ← `C:\Impossible\Bin;C:\Impossible\System32;C:\Programs\` (base; user's `HKCU\Environment\PATH` is appended with `;`)

- [ ] A minimal hardcoded fallback is used during kernel init before `registry_init()` completes (Phase 1); replace with Registry values during Phase 2 (→ XREF `TODO-01-kernel-init-sequencing.md §4`):
  ```c
  static const char *bootstrap_env[] = {
      "PATH=C:\\Impossible\\Bin",
      "SYSTEMROOT=C:\\Impossible",
      "TEMP=C:\\Temp",
      NULL
  };
  ```
- [ ] `env_init_kernel_task()` applies bootstrap env to `PsInitialSystemProcess`

- [ ] Commit: `"kernel/env: system default variables from Registry, bootstrap env"`

**Test checkpoint:** After boot Phase 2, `SET` shows `PATH`, `SYSTEMROOT`, `TEMP`. Bootstrap Phase 1 uses fallback table only. QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 3. `%VAR%` Expansion

- [ ] `env_expand(task, input, output, max_len)` -- walk `input` byte by byte:
  - On `%`: record start; scan forward for closing `%`; if found, extract name (`%NAME%`); call `env_get(task, name)`; if found, append value to output; if not found, append the literal `%NAME%` unchanged
  - On `%%`: emit a single literal `%` (Windows escape)
  - All other chars: copy verbatim
  - Single-pass substitution (matches cmd.exe and Win32 `ExpandEnvironmentStrings`): each `%VAR%` is replaced exactly once; a value that itself contains `%OTHER%` is NOT recursively re-expanded (no depth limit, no infinite-loop risk). cmd's delayed `!VAR!` re-expansion is a distinct mode owned by §18
  - Return number of bytes written (not including null terminator); if output would overflow `max_len`, write truncated result + null and return `max_len`
- [ ] `env_expand` uses the caller's `task->environ`; for kernel-internal calls pass `PsInitialSystemProcess` as the task

- [ ] `RtlExpandEnvironmentStrings_U(Environment, Source, Destination, ReturnedLength)`:
  - `Environment`: pointer to the UTF-16 env block (from `PEB->ProcessParameters->Environment`); if NULL, use calling process's own block
  - `Source`: `UNICODE_STRING` with `%VAR%` references
  - `Destination`: `UNICODE_STRING` output buffer
  - Parse the UTF-16 env block for each `%VAR%` match (UTF-16 `%` = `0x0025`); substitute in-place
  - `ReturnedLength`: set to required buffer length if `Destination` too small; return `STATUS_BUFFER_TOO_SMALL`
  - Used by `ExpandEnvironmentStringsW` (§6) and by the shell for Win32-mode argument expansion

- [ ] Commit: `"kernel/env: env_expand %VAR% substitution, RtlExpandEnvironmentStrings_U"`

**Test checkpoint:** `env_expand` replaces `%VAR%`; `%%` -> `%`; depth limit stops at 5. QEMU WHPX + TCG; VirtualBox; bare metal.

---

## 4. argv Array: Kernel Storage & Shell Parsing

- [ ] `task->argv` is set by the kernel exec path before calling `TODO-11-peb-teb-user-abi.md §7` (which reads `task->argv` to build the stack frame and `CommandLine`):
  ```c
  int task_set_argv(struct task *t, int argc, const char *const *argv);
  /* deep-copies argv strings; sets t->argc, t->argv */
  ```
- [ ] `task_set_argv`: `kmalloc` pointer array of `argc + 1` entries; `kstrdup` each string; set `t->argv[argc] = NULL` terminator
- [ ] On task exit / `env_free`: `kfree` each argv string, `kfree` pointer array

- [ ] `cmd_tokenize(cmdline, argv_out, max_argc)` -- split a shell command line into argv tokens:
  - Split on whitespace (space, tab)
  - `"quoted argument"` → single token with quotes stripped; spaces inside quotes are preserved
  - `"embedded ""double"" quotes"` → produce a single `"` character
  - Backslash before `"` → literal `"` (Windows convention)
  - Return token count; `argv_out[count] = NULL`
- [ ] `cmd_tokenize` used by the shell before calling `exec(path, argv, envp)`
- [ ] Maximum 128 tokens per command; tokens beyond limit are silently dropped with a `[WARN] too many arguments` log message

- [ ] `SYS_EXEC(path, argv[], envp[])` syscall (extends existing exec syscall):
  - Validate `argv[]` pointer array with `ProbeForRead` (→ XREF `TODO-23-exception-dispatch-seh.md §13`)
  - Validate each `argv[i]` string pointer
  - Bounded ingestion (reject with `STATUS_QUOTA_EXCEEDED` = Linux `E2BIG` BEFORE allocating): cap `argc` (~4096), each string (`ARG_STRING_MAX` 32 KiB), and the AGGREGATE argv+envp byte budget (`ARG_MAX` ~256 KiB incl. NULs)
  - Single fault-safe snapshot (no TOCTOU): copy the pointer vectors and strings through usercopy EXACTLY ONCE so a sibling thread cannot swap pointers or unmap strings between probe and copy; unwind partial allocations on any failure
  - Call `task_set_argv(new_task, argc, argv)` -- deep copy into kernel (from the snapshot, never re-reading raw user pointers)
  - Call `env_copy(new_task, ...)` from `envp[]` -- deep copy env (from the same snapshot)
  - Proceed to binary loader → `TODO-11-peb-teb-user-abi.md §7` reads `task->argv` and `task->environ` to build the stack frame
- [ ] `GetCommandLineW()` Win32 wrapper (§6): returns `PEB->ProcessParameters->CommandLine`, which TODO-11 §7 builds from `task->argv[0]` + the joined argv string
- [ ] `task->argv` -> `CommandLine` **encode** = exact inverse of §15 decode (so `GetCommandLineW()` then `CommandLineToArgvW()` round-trips):
  - quote any arg with space/tab/quote or an empty arg; emit `2n` backslashes before an interior quote and `2n+1` for a literal `"`; special-case `argv[0]`
  - owned jointly with `TODO-11-peb-teb-user-abi.md §7` (allocates the `CommandLine` UNICODE_STRING); add encode/decode round-trip tests

- [ ] Commit: `"kernel/env: argv array in task, shell tokenizer, exec argument handoff"`

**Test checkpoint:** Shell tokenizer + `task_set_argv`; child sees expected argc/argv in klog or test harness. QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 5. Nt/Zw Environment Variable Syscalls

- [ ] `NtQueryEnvironmentVariable(Name, Value, ValueLength)`:
  - `Name`: `UNICODE_STRING` (UTF-16 variable name, case-insensitive)
  - `Value`: `UNICODE_STRING` output buffer
  - `ValueLength`: `PULONG` receiving required size if buffer too small
  - Implementation: `ProbeForRead(Name->Buffer, Name->Length, 2)`; `ProbeForWrite(Value->Buffer, Value->MaximumLength, 2)`; convert `Name` to UTF-8 (`RtlUnicodeToUTF8`); call `env_get(current_task, name_utf8)`; convert result back to UTF-16 into `Value->Buffer`
  - Return `STATUS_VARIABLE_NOT_FOUND` if not found; `STATUS_BUFFER_TOO_SMALL` if value too long

- [ ] `NtSetEnvironmentVariable(Name, Value)`:
  - `Value` may be `NULL` → delete the variable (calls `env_unset`)
  - Validate both `UNICODE_STRING` buffers with `ProbeForRead`
  - Convert name and value to UTF-8; call `env_set(current_task, ...)` or `env_unset(current_task, ...)`
  - Also update the UTF-16 env block in `PEB->ProcessParameters->Environment` (→ XREF `TODO-11-peb-teb-user-abi.md §7`):
    1. `ProbeForWrite(PEB->ProcessParameters->Environment, block_size, 2)`
    2. Scan the null-terminated UTF-16 block for `name=` prefix
    3. If found: replace the value portion by moving the tail of the block and inserting the new value; if the new value is longer, reallocate the block with `NtAllocateVirtualMemory` and update the pointer in `RTL_USER_PROCESS_PARAMETERS`
    4. If not found: extend the block (realloc) and append `name=value\0` before the final `\0`
  - Concurrency note: only the owning process can call this for its own block; no cross-process env modification is supported without `NtWriteVirtualMemory` + `SeDebugPrivilege`
- [ ] Add `NtQueryEnvironmentVariable` and `NtSetEnvironmentVariable` to the SSDT (→ XREF `TODO-12-native-api-ssdt.md §5`); add corresponding `ZwXxx` aliases; assign **new** service numbers in `include/kernel/nt/service_numbers.h` (never reuse `0x00D2`..`0x00D6` firmware env slots)

- [ ] Commit: `"kernel/env: NtQueryEnvironmentVariable, NtSetEnvironmentVariable SSDT wiring"`

**Test checkpoint:** `NtQueryEnvironmentVariable`/`NtSetEnvironmentVariable` from ring-3 test; SSDT uses new indices not 0xD2..0xD6. QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 6. Win32 API Wrappers

- [ ] `GetEnvironmentVariableA(lpName, lpBuffer, nSize)`:
  - Convert `lpName` to UTF-16; call `NtQueryEnvironmentVariable`; convert UTF-16 result back to UTF-8 into `lpBuffer`
  - Return character count on success; if `nSize` too small, return required size and `SetLastError(ERROR_INSUFFICIENT_BUFFER)`
- [ ] `GetEnvironmentVariableW(lpName, lpBuffer, nSize)` -- calls `NtQueryEnvironmentVariable` directly with UTF-16 `lpBuffer`
- [ ] `SetEnvironmentVariableA/W(lpName, lpValue)`:
  - `lpValue == NULL` → delete the variable
  - Call `NtSetEnvironmentVariable`; map `STATUS_*` to `ERROR_*` via `RtlNtStatusToDosError`; return `TRUE` / `FALSE`

- [ ] `ExpandEnvironmentStringsA(lpSrc, lpDst, nSize)`:
  - Convert `lpSrc` to UTF-16; call `RtlExpandEnvironmentStrings_U` (same section, RTL helper above); convert UTF-16 result to UTF-8 into `lpDst`
  - Return bytes written (including null); if `nSize` too small, return required size (caller must retry)
- [ ] `ExpandEnvironmentStringsW(lpSrc, lpDst, nSize)` -- calls `RtlExpandEnvironmentStrings_U` directly

- [ ] `GetEnvironmentStringsW()`:
  - Walk `current_task->environ[]`; convert each `"KEY=VALUE"` to UTF-16; pack into a contiguous buffer as null-separated entries with a double-null at the end (matches the Win32 format); allocate with `LocalAlloc`
  - Return pointer; caller must call `FreeEnvironmentStringsW` when done
- [ ] `GetEnvironmentStringsA()` -- UTF-8 variant; same format in ANSI
- [ ] `FreeEnvironmentStringsW(pEnvBlock)` → `LocalFree(pEnvBlock)`

- [ ] `GetCommandLineW()` → returns `PEB->ProcessParameters->CommandLine.Buffer` (UTF-16 command line string, built by `TODO-11-peb-teb-user-abi.md §7` from `task->argv`)
- [ ] `GetCommandLineA()` → convert `CommandLine.Buffer` UTF-16 → UTF-8 and cache in a static per-process buffer (allocated on first call)

- [ ] Commit: `"kernel/env: GetEnvironmentVariable, SetEnvironmentVariable, ExpandEnvironmentStrings, GetCommandLine Win32 wrappers"`

**Test checkpoint:** `GetEnvironmentVariableW` returns `SYSTEMROOT`; `ExpandEnvironmentStringsW` expands. QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 7. Shell Integration: PATH Lookup & SET/ECHO

- [ ] `shell_find_command(name, out_path, max)`:
  1. If `name` contains `\` or `/`: treat as an explicit path; try verbatim, then with `.exe` appended; return first match
  2. Otherwise: retrieve `PATH` value via `env_get(current_task, "PATH")`
  3. Split `PATH` on `;` into directory list
  4. For each directory: try `dir\name` (exact), then `dir\name.exe`; call `vfs_stat(path)` to check existence; return first hit
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

**Test checkpoint:** Registry write-back + `WM_SETTINGCHANGE` when WM up; `setx` persists. QEMU WHPX, QEMU TCG (headless may skip WM); VirtualBox; bare metal.

---

## 10. Environment Block Sorting & Size Limits

- [ ] Windows requires all strings in the environment block to be **sorted alphabetically by name** (case-insensitive, Unicode order, locale-independent); `CreateProcess` and `GetEnvironmentStrings` both depend on this invariant
- [ ] `env_set` must maintain sorted order: on insert, binary-search the `environ[]` array for the correct position and shift entries to keep alphabetical order; on replace, check whether the new name changes sort position
- [ ] `env_build_block(task, out_buf, max_len, is_unicode)` -- build a contiguous env block suitable for `CreateProcess` `lpEnvironment`:
  - Each entry: `name=value\0` (or UTF-16 equivalent)
  - Block terminated by extra `\0` (ANSI) or `\0\0` (Unicode / 4 zero bytes)
  - Entries must be in sorted order
  - Return total block size in bytes; return `STATUS_BUFFER_TOO_SMALL` if `max_len` exceeded
  - **CREATE_UNICODE_ENVIRONMENT:** When `NtCreateProcess` / Win32 `CreateProcess*` receives a non-NULL caller-built UTF-16 `lpEnvironment`, set `CREATE_UNICODE_ENVIRONMENT` in creation flags; omit for inherited default env or ANSI blocks (Microsoft Learn: "Changing Environment Variables"; `CreateEnvironmentBlock` remarks). Wire via → XREF `TODO-12-native-api-ssdt.md §7` and `TODO-21-process-model-extensions.md §2`.
- [ ] `env_parse_block(task, block, len, is_unicode)` -- parse a contiguous env block (from `lpEnvironment`) into the `task->environ[]` array; validate: no name contains `=` (except hidden `=X:` drive vars), no empty names, sorted order
- [ ] Maximum single variable value length: 32,767 characters; `env_set` returns `STATUS_NAME_TOO_LONG` if exceeded
- [ ] Maximum variable name length: 256 characters (practical Windows limit); reject names > 256 chars
- [ ] Variable name validation: name must not contain `=` (the separator); names starting with `=` are reserved for hidden drive-letter variables (§12); reject all other `=`-prefixed names
- [ ] No technical limit on environment block size (Windows Vista+); however, enforce a sanity cap of 1 MiB per process to prevent DoS; log warning at 256 KiB
- [ ] `REG_EXPAND_SZ` values from Registry: expand `%VAR%` references at read time using `env_expand` before storing; raw unexpanded values are never stored in `task->environ[]`
- [ ] Commit: `"kernel/env: sorted environment block, size limits, name validation"`

**Test checkpoint:** `GetEnvironmentStrings` order AAA before ZZZ; oversize name rejected. QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 11. PATHEXT Variable & Extension Search Order

- [ ] Add `PATHEXT` to system default variables (§2): default value `".EXE;.CMD;.BAT"` (subset of Windows default `.COM;.EXE;.BAT;.CMD;.VBS;.VBE;.JS;.JSE;.WSF;.WSH;.MSC` -- only the extensions Impossible OS can execute)
- [ ] Modify `shell_find_command` (§7) to use `PATHEXT`:
  1. If `name` has an explicit extension: try as-is, no PATHEXT iteration
  2. If `name` has no extension: retrieve `PATHEXT` via `env_get(current_task, "PATHEXT")`
  3. Split `PATHEXT` on `;` into extension list
  4. For each PATH directory, try `dir\name{ext}` for each extension in PATHEXT order
  5. First hit wins; this makes extension priority configurable by the user
- [ ] Current search order (before PATHEXT): tries only `.exe`; too restrictive
- [ ] `PATHEXT` with empty value: fall back to `.EXE` only (Windows behavior)

- [ ] Commit: `"kernel/env: PATHEXT extension search order for PATH-based command lookup"`

**Test checkpoint:** `PATHEXT` order selects `.cmd` before `.exe` when configured. QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 12. Hidden Drive-Letter Variables (`=C:`, `=D:`)

- [ ] Windows tracks the current directory for each drive letter via hidden environment variables named `=C:`, `=D:`, etc. (the name starts with `=`); these are **not** visible in `SET` output or user-facing enumerations but are present in the environment block
- [ ] `env_set_drive_cwd(task, drive_letter, path)` -- set `=X:` where X is the uppercase drive letter; value is the full path (e.g. `=C:` → `C:\Users\Default`)
- [ ] `env_get_drive_cwd(task, drive_letter)` -- return the current directory for the given drive, or `X:\` root if not set
- [ ] When `SetCurrentDirectory` changes drives, also call `env_set_drive_cwd` to update the hidden variable
- [ ] On process creation: if `lpEnvironment` is NULL (inherit parent), the hidden drive vars are copied automatically; if `lpEnvironment` is non-NULL (custom block), the caller must include them manually (Windows contract: `CreateProcess` does not inject them)

- [ ] Hidden `=X:` variables sort before regular variables because `=` (0x3D) sorts before any letter (A=0x41); they appear at the front of the environment block
- [ ] `env_build_block` (§10) must include hidden drive vars in the sorted output

- [ ] Commit: `"kernel/env: hidden =X: drive-letter current directory variables"`

**Test checkpoint:** Hidden `=C:` sorts before `AAA`; drive CWD round-trip. QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

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
- [ ] Commit: `"ntdll: Rtl environment layer over the Nt env syscalls"`

**Test checkpoint:** an app importing `RtlQueryEnvironmentVariable_U` from ntdll resolves and returns the same value as `NtQueryEnvironmentVariable`; `RtlCreateEnvironment` builds a sorted block. Serial/klog observable. QEMU WHPX + TCG; VirtualBox; bare metal.

---

## OS Comparison

| ⭐   | Feature                    | 🪟 Win11               | 🐧 Linux              | 🚀 Impossible OS |
| --- | -------------------------- | --------------------- | -------------------- | --------------- |
| 💎   | Per-process env storage    | ✅ PEB UTF-16          | ✅ POSIX environ      | ✅ §1 kernel API |
| 💎   | `%VAR%` / `$VAR`           | ✅ cmd `%VAR%`         | ✅ bash `$VAR`        | ⬜ §3 Win `%`    |
| 💎   | System defaults            | ✅ Session Manager     | ✅ `/etc/environment` | ⬜ §2 Registry   |
| 💎   | argv to child              | ✅ CRT cmdline         | ✅ execve argv        | ⬜ §4 + T11 §7   |
| 💎   | Env var read/write         | ⚠️ ntdll Rtl usermode | ⚠️ libc only         | ⬜ §5 + §19      |
| 💎   | Get/Set env Win32          | ✅ kernel32 A/W        | ⚠️ Wine path         | ⬜ §6            |
| 💎   | Expand env strings         | ✅ A/W                 | ⚠️ Wine path         | ⬜ §6            |
| 💎   | GetCommandLine             | ✅ A/W                 | ⚠️ Wine path         | ⬜ §6            |
| 💎   | PATH lookup                | ✅ PATHEXT             | ✅ POSIX PATH         | ⬜ §7            |
| 💎   | SET shell cmd              | ✅ cmd built-in        | ✅ export/env         | ⬜ §7            |
| 💎   | Shell startup              | ✅ HKCU at logon       | ✅ profile files      | ⬜ §8            |
| 💎   | Env change notify          | ✅ WM_SETTINGCHANGE    | ⚠️ inotify etc       | ⬜ §9            |
| 💎   | Persistent set             | ✅ setx.exe            | ⚠️ edit dotfiles     | ⬜ §9            |
| ⭐   | sysdm env tab              | ✅ sysdm.cpl           | ❌ no GNOME equiv     | ⬜ §9            |
| ⭐   | SET /A arith               | ✅ cmd only            | ✅ bash arith         | ⬜ §7            |
| ⭐   | source / `.`               | ❌ not cmd             | ✅ POSIX              | ⬜ §8            |
| 💎   | Sorted env block           | ✅ Unicode sort        | ❌ unsorted           | ⬜ §10           |
| 💎   | CREATE_UNICODE_ENVIRONMENT | ✅ CreateProcess docs  | ❌ Win32-only         | ⬜ §10, T12 §7   |
| 💎   | Env size limits            | ✅ 32K/var             | ⚠️ ARG_MAX           | ⬜ §10           |
| 💎   | PATHEXT                    | ✅ long default        | ❌ N/A                | ⬜ §11           |
| 💎   | Hidden `=C:` cwd           | ✅ per drive           | ❌ single cwd         | ⬜ §12           |
| 💎   | CreateEnvBlock             | ✅ userenv             | ❌ none               | ⬜ §13           |
| 💎   | ExpandForUser              | ✅ userenv             | ❌ none               | ⬜ §13           |
| 💎   | SearchPathW                | ✅ kernel32            | ⚠️ execvp libc       | ⬜ §14           |
| 💎   | SetSearchPathMode          | ✅ kernel32            | ❌ N/A                | ⬜ §14           |
| 💎   | CmdLineToArgvW             | ✅ shell32             | ❌ wordexp diff       | ⬜ §15           |
| 💎   | Elevated env strip         | ✅ restricted          | ✅ AT_SECURE          | ⬜ §16           |
| ⭐   | App Paths                  | ✅ HKLM App Paths      | ❌ none               | ⬜ §17           |
| 💎   | Dynamic pseudo-vars        | ✅ %CD%/%ERRORLEVEL%   | ⚠️ $PWD/$?/$RANDOM   | ⬜ §18           |
| 💎   | Delayed `!VAR!` expansion  | ✅ cmd /V              | ❌ N/A                | ⬜ §18           |
| 💎   | ntdll Rtl env layer        | ✅ ntdll usermode      | ❌ none               | ⬜ §19           |
| 💎   | Exec argv+envp size cap    | ⚠️ per-var only       | ✅ E2BIG/ARG_MAX      | ⬜ §4            |

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
- [ ] **env_expand**: `env_set(t, "NAME", "World")`; `env_expand(t, "Hello %NAME%!", buf, ...)` → `"Hello World!"`; `env_expand(t, "%%", buf, ...)` → `"%"`.
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
