# TODO-14 — Environment Variables & Process Arguments

> **Goal:** Implement per-process environment variable storage, `%VAR%`
> expansion, `PATH`-based command lookup, `argv`/`argc` kernel preparation, the Win32 `GetEnvironmentVariable`/`SetEnvironmentVariable` API surface, and the `.profile` shell startup script. No env API exists at all today: there is no `env_get`, no `SYS_GETENV`, no `PATH` lookup, and no argv array in `struct task`. Without this, every user-mode program launches with no arguments, no environment, and no way to find executables on disk.

> [!IMPORTANT]
> **Scope boundary with adjacent TODOs:**
> - `TODO-04-peb-teb-user-abi.md §2` defines `RTL_USER_PROCESS_PARAMETERS.Environment` (the UTF-16 null-terminated env block layout in user address space).
> - `TODO-04-peb-teb-user-abi.md §7` covers pushing `argc`/`argv`/`envp` onto the initial ring-3 stack frame immediately before `iretq`.
> - **This TODO** owns: the kernel-side `char **environ` storage in   `struct task`, `env_get/set/unset/expand`, population of default   variables from Registry, `argv[]` preparation in the kernel and shell,   `NtSetEnvironmentVariable` / `NtQueryEnvironmentVariable` syscalls,   Win32 `GetEnvironmentVariable`/`ExpandEnvironmentStrings` wrappers,   PATH lookup, `SET` shell command, and `.profile` startup.

---

## Inputs

- `src/kernel/sched/task.c` — `struct task` (environ and argv fields must be added)
- `include/kernel/sched/task.h` — task struct header
- `src/kernel/sched/syscall.c` — syscall dispatch table
- `src/kernel/registry.c` — `reg_get_dword`, `reg_get_string` (used to read default env vars at boot)
- `src/desktop/terminal.c` — terminal/shell command dispatch
- → XREF: `TODO-04-peb-teb-user-abi.md §2` — `RTL_USER_PROCESS_PARAMETERS.Environment` points to the UTF-16 env block built by §5 of this TODO
- → XREF: `TODO-04-peb-teb-user-abi.md §7` — initial stack frame pushes `argc`/`argv[]`/`envp[]`; requires `task->argv` and `task->environ` to be populated first
- → XREF: `TODO-05-native-api-ssdt.md §4` — SSDT slots for `NtSetEnvironmentVariable` and `NtQueryEnvironmentVariable`
- → XREF: `TODO-09-process-model-extensions.md §1` — `NtCreateProcess` must deep-copy `task->environ` and `task->argv` from parent to child
- → XREF: `TODO-13-registry-completion.md §1` — system defaults read from `HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Environment` and `HKCU\Environment`

---

## Outcome

- `struct task` carries `char **environ` (UTF-8 key=value array) and `char **argv` (argument array); both are deep-copied on spawn.
- `env_get`, `env_set`, `env_unset`, `env_expand` are available kernel-wide.
- System defaults (`PATH`, `SYSTEMROOT`, `TEMP`, `USERNAME`, `COMPUTERNAME`, `USERPROFILE`) are populated from Registry at boot.
- `NtSetEnvironmentVariable` and `NtQueryEnvironmentVariable` let user-mode processes read and write their own env block.
- `GetEnvironmentVariableW/A`, `SetEnvironmentVariableW/A`, `ExpandEnvironmentStringsW/A`, `GetCommandLineW`, `GetEnvironmentStrings` are usable Win32 API functions.
- Shell PATH lookup, `SET` / `ECHO` commands, and `%VAR%` argument expansion all work.
- `.profile` is sourced on shell startup; users can persist env changes across reboots.

---

## Implementation Order

| ⭐  | Order | Deliverable                                         | Depends On          | Status |
| --- | :---: | --------------------------------------------------- | ------------------- | :----: |
| 💎  |   1   | Per-process environ storage & kernel API            | TODO-09 §1          |  [ ]   |
| 💎  |   2   | System default variables from Registry              | 1, TODO-13 §1       |  [ ]   |
| 💎  |   3   | `%VAR%` expansion (`env_expand`)                    | 1                   |  [ ]   |
| 💎  |   4   | argv array: kernel storage & shell parsing          | 1                   |  [ ]   |
| 💎  |   5   | Nt/Zw environment variable syscalls                 | 1, TODO-04 §2, TODO-05 §4 | [ ] |
| 💎  |   6   | Win32 API wrappers                                  | 5                   |  [ ]   |
| 💎  |   7   | Shell integration (PATH lookup, SET, ECHO)          | 3, 4                |  [ ]   |
| 💎  |   8   | `.profile` startup script                           | 7                   |  [ ]   |
| ⭐  |   9   | Environment change notifications & `sysdm.cpl` tab  | 6, 8                |  [ ]   |

> 💎 = parity work — matches what Windows 11 and Linux already do.
> ⭐ = exclusive work — Impossible OS is superior or first.

---

## 1. Per-Process Environ Storage & Kernel API `[Sonnet]`

### 1.1 environ field in struct task

- [ ] Add to `struct task` in `include/kernel/sched/task.h`:
  ```c
  char   **environ;      /* NULL-terminated array of "KEY=VALUE" UTF-8 strings */
  uint32_t environ_count;
  char   **argv;         /* NULL-terminated argument array */
  int      argc;
  ```
- [ ] `environ` and `argv` are `NULL` initially; populated by `env_init_defaults(task)` and the exec path respectively
- [ ] Memory: each `"KEY=VALUE"` string ≤ 32 KiB → `kmalloc`; the pointer arrays themselves: ≤ 4 KiB → `kmalloc`; large `REG_EXPAND_SZ` values may exceed 4 KiB → `pmm_alloc_contiguous`

### 1.2 Core env API

- [ ] Implement in `src/kernel/env.c`, declare in `include/kernel/env.h`:
  ```c
  const char *env_get(struct task *t, const char *name);
  int         env_set(struct task *t, const char *name, const char *value);
  int         env_unset(struct task *t, const char *name);
  int         env_copy(struct task *dst, const struct task *src); /* deep copy */
  void        env_free(struct task *t);                           /* on task exit */
  ```
- [ ] `env_get`: linear scan of `t->environ[]` for `"name="` prefix match (case-insensitive on Windows-style names); return pointer to value portion or `NULL`
- [ ] `env_set`: search for existing entry; if found, replace string in-place (kfree old, kmalloc new); if not found, `krealloc` the pointer array to add one slot + NULL terminator
- [ ] `env_unset`: find entry, `kfree` its string, shift remaining pointers left, update `environ_count`
- [ ] `env_copy`: `kmalloc` a new pointer array of `src->environ_count + 1` entries; `kstrdup` each string; called from `NtCreateProcess` (→ XREF `TODO-09-process-model-extensions.md §1`) to give child its own private copy

### 1.3 Commit

- [ ] Commit: `"kernel/env: per-process environ array, env_get/set/unset/copy"`

---

## 2. System Default Variables from Registry `[Sonnet]`

### 2.1 System-wide defaults

- [ ] `env_init_defaults(task)` — called once for every newly created process:
  1. Read system env vars from Registry key `HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Environment` (→ XREF `TODO-13-registry-completion.md §1`); enumerate all values; call `env_set` for each
  2. Read user env vars from `HKCU\Environment`; set for each (user vars override system vars with the same name)
  3. Synthesise computed variables that cannot come from Registry:
     - `COMPUTERNAME` ← `HKLM\SYSTEM\ComputerName\ActiveComputerName\ComputerName` (default `"IMPOSSIBLE-PC"`)
     - `USERNAME` ← from the process's primary token UserSid → account name lookup (→ XREF `TODO-11-security-reference-monitor.md §4`); default `"Default"`
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

### 2.2 Bootstrap env before Registry is mounted

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

### 2.3 Commit

- [ ] Commit: `"kernel/env: system default variables from Registry, bootstrap env"`

---

## 3. `%VAR%` Expansion `[Sonnet]`

### 3.1 env_expand

- [ ] `env_expand(task, input, output, max_len)` — walk `input` byte by byte:
  - On `%`: record start; scan forward for closing `%`; if found, extract name (`%NAME%`); call `env_get(task, name)`; if found, append value to output; if not found, append the literal `%NAME%` unchanged
  - On `%%`: emit a single literal `%` (Windows escape)
  - All other chars: copy verbatim
  - Depth limit: track nesting depth; if result contains `%VAR%` references, re-expand up to 4 levels deep; abort and return partial result at depth 5 to prevent infinite expansion loops
  - Return number of bytes written (not including null terminator); if output would overflow `max_len`, write truncated result + null and return `max_len`
- [ ] `env_expand` uses the caller's `task->environ`; for kernel-internal calls pass `PsInitialSystemProcess` as the task

### 3.2 RtlExpandEnvironmentStrings_U (RTL layer)

- [ ] `RtlExpandEnvironmentStrings_U(Environment, Source, Destination, ReturnedLength)`:
  - `Environment`: pointer to the UTF-16 env block (from `PEB->ProcessParameters->Environment`); if NULL, use calling process's own block
  - `Source`: `UNICODE_STRING` with `%VAR%` references
  - `Destination`: `UNICODE_STRING` output buffer
  - Parse the UTF-16 env block for each `%VAR%` match (UTF-16 `%` = `0x0025`); substitute in-place
  - `ReturnedLength`: set to required buffer length if `Destination` too small; return `STATUS_BUFFER_TOO_SMALL`
  - Used by `ExpandEnvironmentStringsW` (§6) and by the shell for Win32-mode argument expansion

### 3.3 Commit

- [ ] Commit: `"kernel/env: env_expand %VAR% substitution, RtlExpandEnvironmentStrings_U"`

---

## 4. argv Array: Kernel Storage & Shell Parsing `[Sonnet]`

### 4.1 argv in struct task

- [ ] `task->argv` is set by the kernel exec path before calling `TODO-04-peb-teb-user-abi.md §7` (which reads `task->argv` to build the stack frame and `CommandLine`):
  ```c
  int task_set_argv(struct task *t, int argc, const char *const *argv);
  /* deep-copies argv strings; sets t->argc, t->argv */
  ```
- [ ] `task_set_argv`: `kmalloc` pointer array of `argc + 1` entries; `kstrdup` each string; set `t->argv[argc] = NULL` terminator
- [ ] On task exit / `env_free`: `kfree` each argv string, `kfree` pointer array

### 4.2 Shell command-line tokenizer

- [ ] `cmd_tokenize(cmdline, argv_out, max_argc)` — split a shell command line into argv tokens:
  - Split on whitespace (space, tab)
  - `"quoted argument"` → single token with quotes stripped; spaces inside quotes are preserved
  - `"embedded ""double"" quotes"` → produce a single `"` character
  - Backslash before `"` → literal `"` (Windows convention)
  - Return token count; `argv_out[count] = NULL`
- [ ] `cmd_tokenize` used by the shell before calling `exec(path, argv, envp)`
- [ ] Maximum 128 tokens per command; tokens beyond limit are silently dropped with a `[WARN] too many arguments` log message

### 4.3 exec argument handoff

- [ ] `SYS_EXEC(path, argv[], envp[])` syscall (extends existing exec syscall):
  - Validate `argv[]` pointer array with `ProbeForRead` (→ XREF `TODO-10-exception-dispatch-seh.md §10`)
  - Validate each `argv[i]` string pointer
  - Call `task_set_argv(new_task, argc, argv)` — deep copy into kernel
  - Call `env_copy(new_task, ...)` from `envp[]` — deep copy env
  - Proceed to binary loader → `TODO-04-peb-teb-user-abi.md §7` reads `task->argv` and `task->environ` to build the stack frame
- [ ] `GetCommandLineW()` Win32 wrapper (§6): returns `PEB->ProcessParameters->CommandLine`, which TODO-04 §7 builds from `task->argv[0]` + the joined argv string

### 4.4 Commit

- [ ] Commit: `"kernel/env: argv array in task, shell tokenizer, exec argument handoff"`

---

## 5. Nt/Zw Environment Variable Syscalls `[Opus]`

### 5.1 NtQueryEnvironmentVariable

- [ ] `NtQueryEnvironmentVariable(Name, Value, ValueLength)`:
  - `Name`: `UNICODE_STRING` (UTF-16 variable name, case-insensitive)
  - `Value`: `UNICODE_STRING` output buffer
  - `ValueLength`: `PULONG` receiving required size if buffer too small
  - Implementation: `ProbeForRead(Name->Buffer, Name->Length, 2)`; `ProbeForWrite(Value->Buffer, Value->MaximumLength, 2)`; convert `Name` to UTF-8 (`RtlUnicodeToUTF8`); call `env_get(current_task, name_utf8)`; convert result back to UTF-16 into `Value->Buffer`
  - Return `STATUS_VARIABLE_NOT_FOUND` if not found; `STATUS_BUFFER_TOO_SMALL` if value too long

### 5.2 NtSetEnvironmentVariable

- [ ] `NtSetEnvironmentVariable(Name, Value)`:
  - `Value` may be `NULL` → delete the variable (calls `env_unset`)
  - Validate both `UNICODE_STRING` buffers with `ProbeForRead`
  - Convert name and value to UTF-8; call `env_set(current_task, ...)` or `env_unset(current_task, ...)`
  - Also update the UTF-16 env block in `PEB->ProcessParameters->Environment` (→ XREF `TODO-04-peb-teb-user-abi.md §2`):
    1. `ProbeForWrite(PEB->ProcessParameters->Environment, block_size, 2)`
    2. Scan the null-terminated UTF-16 block for `name=` prefix
    3. If found: replace the value portion by moving the tail of the block and inserting the new value; if the new value is longer, reallocate the block with `NtAllocateVirtualMemory` and update the pointer in `RTL_USER_PROCESS_PARAMETERS`
    4. If not found: extend the block (realloc) and append `name=value\0` before the final `\0`
  - Concurrency note: only the owning process can call this for its own block; no cross-process env modification is supported without `NtWriteVirtualMemory` + `SeDebugPrivilege`
- [ ] Add `NtQueryEnvironmentVariable` and `NtSetEnvironmentVariable` to the SSDT (→ XREF `TODO-05-native-api-ssdt.md §4`); add corresponding `ZwXxx` aliases

### 5.3 Commit

- [ ] Commit: `"kernel/env: NtQueryEnvironmentVariable, NtSetEnvironmentVariable SSDT wiring"`

---

## 6. Win32 API Wrappers `[Sonnet]`

### 6.1 GetEnvironmentVariable / SetEnvironmentVariable

- [ ] `GetEnvironmentVariableA(lpName, lpBuffer, nSize)`:
  - Convert `lpName` to UTF-16; call `NtQueryEnvironmentVariable`; convert UTF-16 result back to UTF-8 into `lpBuffer`
  - Return character count on success; if `nSize` too small, return required size and `SetLastError(ERROR_INSUFFICIENT_BUFFER)`
- [ ] `GetEnvironmentVariableW(lpName, lpBuffer, nSize)` — calls `NtQueryEnvironmentVariable` directly with UTF-16 `lpBuffer`
- [ ] `SetEnvironmentVariableA/W(lpName, lpValue)`:
  - `lpValue == NULL` → delete the variable
  - Call `NtSetEnvironmentVariable`; map `STATUS_*` to `ERROR_*` via `RtlNtStatusToDosError`; return `TRUE` / `FALSE`

### 6.2 ExpandEnvironmentStrings

- [ ] `ExpandEnvironmentStringsA(lpSrc, lpDst, nSize)`:
  - Convert `lpSrc` to UTF-16; call `RtlExpandEnvironmentStrings_U` (§3.2); convert UTF-16 result to UTF-8 into `lpDst`
  - Return bytes written (including null); if `nSize` too small, return required size (caller must retry)
- [ ] `ExpandEnvironmentStringsW(lpSrc, lpDst, nSize)` — calls `RtlExpandEnvironmentStrings_U` directly

### 6.3 GetEnvironmentStrings / FreeEnvironmentStrings

- [ ] `GetEnvironmentStringsW()`:
  - Walk `current_task->environ[]`; convert each `"KEY=VALUE"` to UTF-16; pack into a contiguous buffer as null-separated entries with a double-null at the end (matches the Win32 format); allocate with `LocalAlloc`
  - Return pointer; caller must call `FreeEnvironmentStringsW` when done
- [ ] `GetEnvironmentStringsA()` — UTF-8 variant; same format in ANSI
- [ ] `FreeEnvironmentStringsW(pEnvBlock)` → `LocalFree(pEnvBlock)`

### 6.4 GetCommandLine

- [ ] `GetCommandLineW()` → returns `PEB->ProcessParameters->CommandLine.Buffer` (UTF-16 command line string, built by `TODO-04-peb-teb-user-abi.md §7` from `task->argv`)
- [ ] `GetCommandLineA()` → convert `CommandLine.Buffer` UTF-16 → UTF-8 and cache in a static per-process buffer (allocated on first call)

### 6.5 Commit

- [ ] Commit: `"kernel/env: GetEnvironmentVariable, SetEnvironmentVariable, ExpandEnvironmentStrings, GetCommandLine Win32 wrappers"`

---

## 7. Shell Integration: PATH Lookup & SET/ECHO `[Sonnet]`

### 7.1 PATH-based command lookup

- [ ] `shell_find_command(name, out_path, max)`:
  1. If `name` contains `\` or `/`: treat as an explicit path; try verbatim, then with `.exe` appended; return first match
  2. Otherwise: retrieve `PATH` value via `env_get(current_task, "PATH")`
  3. Split `PATH` on `;` into directory list
  4. For each directory: try `dir\name` (exact), then `dir\name.exe`; call `vfs_stat(path)` to check existence; return first hit
  5. If no match: return `SHELL_COMMAND_NOT_FOUND`
- [ ] Shell uses `shell_find_command` before any `exec` call; replaces current ad-hoc path construction

### 7.2 `%VAR%` expansion in shell command arguments

- [ ] Before executing any command, the shell calls `env_expand(current_task, raw_cmdline, expanded, sizeof expanded)` on the full command line
- [ ] This makes `echo %SYSTEMROOT%`, `cd %TEMP%`, and path arguments with env vars work transparently before the command is parsed into argv

### 7.3 SET and ECHO shell commands

- [ ] `SET` with no args: print all environment variables, one per line in `KEY=VALUE` format, sorted alphabetically
- [ ] `SET VAR=VALUE`: call `env_set(current_task, "VAR", "VALUE")`; print nothing on success
- [ ] `SET VAR=` (empty value): call `env_set(current_task, "VAR", "")`; effectively creates an empty variable (distinct from unset)
- [ ] `SET /A VAR=EXPR`: arithmetic expansion (integer expressions using `+`, `-`, `*`, `/`, `%`, `<<`, `>>`, `&`, `|`, `^`); evaluate with a simple recursive descent parser; store result as decimal string
- [ ] `ECHO %VAR%`: `env_expand` already handles this before argument tokenization; ensure `ECHO` prints the expanded value
- [ ] `SET /?`: print usage summary

### 7.4 Commit

- [ ] Commit: `"shell: PATH lookup, SET command, %VAR% expansion in all commands"`

---

## 8. `.profile` Startup Script `[Sonnet]`

### 8.1 Shell startup sourcing

- [ ] On shell startup (after default env is initialized):
  1. Compute profile path: `env_expand(..., "%USERPROFILE%\\.profile", ...)`
  2. `vfs_stat(profile_path)` — if not found, skip silently; no error
  3. If found: `vfs_open` + read line-by-line (max 4 KiB buffer per line)
  4. Execute each non-blank, non-comment line as a shell command (same code path as user-typed input); errors are logged to serial but do not abort the rest of the script
- [ ] Comment lines: any line beginning with `#` (after optional leading whitespace) is skipped
- [ ] Maximum lines: 1 000; lines beyond limit are skipped with a warning

### 8.2 Default .profile shipped in the image

- [ ] `resources/defaults/.profile` file included in the ISO/image, copied to `C:\Users\Default\.profile` during OS install / first boot:
  ```
  # Impossible OS default user profile
  # Edit this file to customise your shell environment.

  SET PATH=%PATH%;C:\Users\%USERNAME%\Bin
  SET EDITOR=notepad.exe
  SET PAGER=more.exe
  ```
- [ ] The build system (Makefile / build.sh) copies this file into the IXFS root partition during `make install-userfiles`

### 8.3 `source` shell command

- [ ] `source <file>` (or `.  <file>` POSIX synonym) — execute a script file in the current shell's context (not a child process); env changes in the script affect the current shell session
- [ ] Used to manually reload `.profile` after editing: `source C:\Users\Default\.profile`

### 8.4 Commit

- [ ] Commit: `"shell: .profile startup script, source command, default profile in image"`

---

## 9. Environment Change Notifications `[Sonnet]`

### 9.1 WM_SETTINGCHANGE broadcast

- [ ] When `SetEnvironmentVariableW` modifies a variable that originated from `HKCU\Environment` or `HKLM\...\Session Manager\Environment`, also write the change back to the Registry key
- [ ] After writing to Registry, post `WM_SETTINGCHANGE` (Win32 message `0x001A`) with `lParam = (LPARAM)L"Environment"` to `HWND_BROADCAST` (all top-level windows); this matches Windows 11 behaviour so that applications receive the standard notification and call `SendMessageTimeout(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"Environment", SMTO_ABORTIFHUNG, 5000, NULL)`
- [ ] WM delivery requires the window manager to be running; if WM is not yet started (early boot), skip silently

### 9.2 System Properties Environment tab (`sysdm.cpl`)

- [ ] Add "Environment Variables" button to `sysdm.cpl` (System Properties)
  → opens a dialog with two list-views: System variables (requires admin) and User variables
- [ ] Edit/New/Delete buttons per list; changes go directly to Registry + call `WM_SETTINGCHANGE`
- [ ] Dialog layout matches Windows 11's Environment Variables dialog for user familiarity

### 9.3 `setx` shell command ⭐

- [ ] `setx VAR VALUE` — set an environment variable **persistently** (writes to `HKCU\Environment` via Registry API + triggers `WM_SETTINGCHANGE`); current session not affected (matches Windows `setx.exe` behaviour)
- [ ] `setx VAR VALUE /M` — write to system-wide `HKLM\...\Session Manager\Environment`; requires admin token (→ XREF `TODO-11-security-reference-monitor.md §8`)
- [ ] `setx /?` — print usage

### 9.4 Commit

- [ ] Commit: `"kernel/env: WM_SETTINGCHANGE broadcast, sysdm.cpl env tab, setx command"`

---

## OS Comparison


| ⭐ | Feature                                 | 🪟 Win11                                  | 🐧 Linux                              | 🚀 Impossible OS                     |
|----|-----------------------------------------|----------------------------------------|------------------------------------|-----------------------------------|
| 💎 | Per-process env var storage             | ✅ UTF-16 env block in PEB             | ✅ POSIX `environ[]` (UTF-8)       | ⬜ §1                             |
| 💎 | `%VAR%` / `$VAR` expansion              | ✅ `%VAR%` in cmd.exe                  | ✅ `$VAR` in bash                  | ⬜ §3 — (`%VAR%` Win-style)       |
| 💎 | System defaults from persistent store   | ✅ Registry `Session Manager\Env`      | ✅ `/etc/environment`              | ⬜ §2 — (Registry-backed)         |
| 💎 | argv / argc to child process            | ✅ command-line string → CRT parses    | ✅ `execve` argv[]                 | ⬜ §4 — (System V ABI via TODO-04 |
| 💎 | `NtSetEnvironmentVariable` syscall      | ✅ Native                              | ✅ `SYS_setenv` (glibc internal)   | ⬜ §5                             |
| 💎 | `GetEnvironmentVariable` Win32 API      | ✅ Full A/W                            | ⚠️ Via Wine only                   | ⬜ §6                             |
| 💎 | `ExpandEnvironmentStrings` Win32 API    | ✅ Full A/W                            | ⚠️ Via Wine only                   | ⬜ §6                             |
| 💎 | `GetCommandLine` Win32 API              | ✅ Full A/W                            | ⚠️ Via Wine only                   | ⬜ §6                             |
| 💎 | PATH-based command lookup               | ✅ `PATHEXT` + `.exe` fallback         | ✅ POSIX PATH                      | ⬜ §7                             |
| 💎 | `SET` interactive shell command         | ✅ cmd.exe built-in                    | ✅ `export` / `env`                | ⬜ §7                             |
| 💎 | Shell startup config                    | ✅ `HKCU\Environment` at logon         | ✅ `~/.profile` / `~/.bashrc`      | ⬜ §8                             |
| 💎 | Env change notification to running apps | ✅ `WM_SETTINGCHANGE` broadcast        | ⚠️ `inotify` on `/etc/environment` | ⬜ §9                             |
| 💎 | Persistent env var writer               | ✅ `setx.exe` built-in                 | ⚠️ Manual `.bashrc` edit           | ⬜ §9 — .3                        |
| ⭐ | System Properties Environment tab       | ✅ `sysdm.cpl` → Environment Variables | ❌ Not available (GNOME Settings)  | ⬜ §9 — .2                        |
| ⭐ | `SET /A` arithmetic expansion           | ✅ cmd.exe only                        | ✅ `$((expr))` bash                | ⬜ §7 — .3                        |
| ⭐ | `source` / `.` command                  | ❌ Not in cmd.exe (PowerShell only)    | ✅ POSIX `.`                       | ⬜ §8 — .3 🚀                     |

After §1–8, Impossible OS reaches full Windows 11 and Linux parity for every environment variable feature: per-process UTF-8 env storage, `%VAR%` expansion, Registry-backed system defaults, Win32 `GetEnvironmentVariable` /
`ExpandEnvironmentStrings`, PATH lookup, `SET`, and `.profile` startup.
The `source` / `.` command (§8.3) is a genuine differentiator over Windows cmd.exe. The `SET /A` arithmetic expansion and `setx` persistent setter are standard Windows features that are also included for full shell parity.

---

## Unit Tests

> Wire into `test_runner_init()` via `test_register_env()` (→ XREF: `00-infrastructure/TODO-03-kernel-test-framework.md`).
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
- [ ] Register in `test_runner_init()`: `test_register_env()`
- [ ] Commit: `"test: add environment variables test suite"`

---

## Verification

- [ ] **env_get/set**: kernel unit test — create a task with empty environ; `env_set(t, "GREETING", "hello")`; `env_get(t, "GREETING")` → `"hello"`; `env_unset(t, "GREETING")`; `env_get(t, "GREETING")` → `NULL`.
- [ ] **env_expand**: `env_set(t, "NAME", "World")`; `env_expand(t, "Hello %NAME%!", buf, ...)` → `"Hello World!"`; `env_expand(t, "%%", buf, ...)` → `"%"`.
- [ ] **Default vars**: boot to shell; run `SET` with no args; output must include `PATH=`, `SYSTEMROOT=`, `TEMP=`, `USERNAME=`, `COMPUTERNAME=`.
- [ ] **PATH lookup**: place a test binary in `C:\Impossible\Bin\`; type just its name without path in the shell; it must launch.
- [ ] **argv round-trip**: run `echo hello world`; child process must receive `argc=3`, `argv=["echo","hello","world"]`; verify via a debug print in the program's `main()`.
- [ ] **NtQueryEnvironmentVariable**: user-mode test calls `NtQueryEnvironmentVariable` with `"PATH"` → returns the PATH string.
- [ ] **GetEnvironmentVariableW**: `GetEnvironmentVariableW(L"SYSTEMROOT", buf, MAX_PATH)` → `L"C:\\Impossible"`.
- [ ] **.profile**: create `C:\Users\Default\.profile` with `SET TEST_VAR=from_profile`; restart shell; run `ECHO %TEST_VAR%` → `"from_profile"`.
- [ ] **Remaining limits**: `ExpandEnvironmentStringsW` with deeply nested `%A%%B%%C%` (> 4 levels) must return the partially expanded string rather than hanging; `WM_SETTINGCHANGE` broadcast requires the window manager to be running (§9.1) — skip in headless QEMU test.
- [ ] Commit: `"kernel/env: environment variables, argv, Win32 GetEnvironmentVariable, PATH lookup, .profile"`
