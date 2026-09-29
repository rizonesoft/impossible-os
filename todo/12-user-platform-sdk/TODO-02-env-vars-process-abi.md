---
schema_version: 1
id: env-vars-process-abi
domain: 12-user-platform-sdk
status: active
title: "TODO-02 -- Environment Variables & Process ABI"
---

# TODO-02 -- Environment Variables & Process ABI

> **Goal:** Wire the per-process environment, `%VAR%` expansion, file-association command
> template substitution, and the argv/argc ABI that every user-mode program depends on.
> The kernel-side implementation lives in `02-kernel-core/TODO-22`; this TODO is the
> **SDK/user-mode companion** -- it owns the developer-facing API contract, `env_expand_path`
> for shell/exec command templates, `cmd_tokenize` quoted-arg rules, and the Win32 function
> prototypes apps call.

> [!IMPORTANT]
> **Canonical implementation is `02-kernel-core/TODO-22-environment-variables.md`.**
> That TODO fully specifies: `struct task` environ/argv fields, `env_get/set/unset/copy`,
> system default variable population from Registry, `%VAR%` depth-limited expansion,
> `NtQuery/SetEnvironmentVariable` SSDT wiring, all Win32 API wrappers
> (`GetEnvironmentVariableA/W`, `SetEnvironmentVariableA/W`, `ExpandEnvironmentStringsA/W`,
> `GetEnvironmentStringsW`, `GetCommandLineA/W`), PATH lookup, `SET`/`ECHO` shell commands,
> and `.profile` startup. **Do not re-implement any of those in this TODO.**
>
> **Initial user stack frame** (`exec_build_stack` -- argc/argv/envp pushed before `iretq`) is
> specified in `02-kernel-core/TODO-11-peb-teb-user-abi.md §2`. Implement that first.
>
> This TODO adds the parts unique to the **SDK/user-platform domain:**
> - `env_expand_path(template, argv0, filepath, output)` for `%1`–`%9` file-association templates
> - `cmd_tokenize` quoted-argument and escape rules (the contract SDK apps can rely on)
> - Shell session lookup cache and `where` command
> - `set`/`echo`/`env`/`where` command implementations in the shell

---

## Inputs

- `include/kernel/env.h` (→ XREF `02-kernel-core/TODO-22 §1`) -- `env_get`, `env_set`, `env_unset`, `env_expand`, `env_init_defaults`
- `include/kernel/sched/task.h` (→ XREF `02-kernel-core/TODO-22 §1`) -- `struct task { char **environ; char **argv; int argc; }`
- `include/registry.h` -- `reg_get_string`, `reg_enum_values` -- §2 system defaults, §7 profile bypass flag
- `include/kernel/fs/vfs.h` -- `vfs_open`, `vfs_read`, `vfs_stat` -- §5 PATH stat, §7 `.profile` read
- `src/desktop/terminal.c` -- shell command dispatch; `cmd_tokenize` lives here (→ §6)
- `02-kernel-core/TODO-22-environment-variables.md` (→ XREF) -- full kernel env implementation
- `02-kernel-core/TODO-11-peb-teb-user-abi.md §2` (→ XREF) -- `exec_build_stack`, PEB layout, initial ring-3 stack
- `09-desktop-shell/TODO-02-file-associations-resources.md §1` (→ XREF) -- `%1` command template consumer
- `include/kernel/syscall.h` -- syscall number table; `SYS_GETENV`/`SYS_SETENV`/`SYS_UNSETENV` numbers

---

## Outcome

Every user-mode program launched via `task_exec` receives correct `argc`, `argv[]`, and a
populated environment block. `%VAR%` references in shell command lines and file-association
command templates expand correctly. `GetEnvironmentVariableA` and `ExpandEnvironmentStrings`
work for third-party app code. The shell commands `set`, `echo`, `env`, and `where` are
available. `.profile` is sourced on startup and persists env changes across reboots.

---

## Implementation Order

| Step | Section                                    | 💎/⭐ | Dependency                             |
| ---- | ------------------------------------------ | ----- | -------------------------------------- |
| 1    | Per-process environ storage (SDK contract) | 💎    | `D02T22 §1` complete                   |
| 2    | System default variables                   | 💎    | `D02T22 §2` complete; Registry mounted |
| 3    | `%VAR%` expansion + `env_expand_path`      | 💎    | `D02T22 §3` complete                   |
| 4    | SYS_GETENV / SYS_SETENV / SYS_UNSETENV     | 💎    | `D02T22 §5` complete                   |
| 5    | PATH-based command lookup + session cache  | 💎    | `D02T22 §7`; `vfs_stat`                |
| 6    | argv / argc process ABI + `cmd_tokenize`   | 💎    | `TODO-04 §1` complete                  |
| 7    | Shell `.profile` startup script            | 💎    | §5 PATH lookup, §6 cmd_tokenize        |
| 8    | `set` / `echo` / `env` / `where` commands  | 💎    | §1–§5 all complete                     |

---

## 1. Per-Process Environ Storage (SDK Contract) `[Sonnet]`

> Implementation: `02-kernel-core/TODO-22 §1`. This section defines the **SDK contract** --
> the stable API surface that apps and SDK headers depend on.

**Header `include/kernel/env.h`** -- public API contract:

```c
const char *env_get(struct task *t, const char *name);          /* case-insensitive key match */
int         env_set(struct task *t, const char *name,
                    const char *value);                         /* NULL value → unset */
int         env_unset(struct task *t, const char *name);
int         env_list(struct task *t, char *buf, size_t max);    /* all KEY=VALUE lines, \n separated */
int         env_copy(struct task *dst, const struct task *src); /* deep-copy for fork/spawn */
void        env_free(struct task *t);                           /* on task exit */
int         env_expand(struct task *t, const char *in,
                       char *out, size_t max);                  /* %VAR% substitution, depth 4 */
int         env_expand_path(struct task *t, const char *templ,
                             const char *argv0, const char *filepath,
                             char *out, size_t max);            /* %1–%9 file-assoc substitution */
```

- [ ] Rescope against `02-kernel-core/TODO-22-environment-variables.md`, which shipped the storage, defaults, expansion, `NtQueryEnvironmentVariable`/`NtSetEnvironmentVariable`, search path and `CommandLineToArgvW` this file restates
  - Real names: `env_get_copy()` (no public `env_get()`), SSDT `NtQueryEnvironmentVariable` (no `SYS_GETENV`), `cmd_tokenize()` in `user/lib/stdlib.c` and the shell in `user/cmd.c`; keep `env_expand_path`, the PATH cache, `.profile` and the shell commands.
- [ ] Verify `include/kernel/env.h` exports all the above after TODO-14 §2 is complete
- [ ] `env_get` is **case-insensitive** (`PATH` == `path`); this is the Windows contract
- [ ] `env_list` is the backing function for `GetEnvironmentStrings` and the `set` command

---

## 2. System Default Variables `[Sonnet]`

> Implementation: `02-kernel-core/TODO-22 §2`. This section documents the **developer-visible
> defaults** every app can rely on.

- [ ] Confirm all the following are populated by `env_init_defaults()` before any user-mode app starts:

| Variable                 | Default value                                           | Source                                    |
| ------------------------ | ------------------------------------------------------- | ----------------------------------------- |
| `PATH`                   | `C:\Impossible\Bin;C:\Impossible\System32;C:\Programs\` | Registry + user `HKCU\Environment` append |
| `SYSTEMROOT` / `WINDIR`  | `C:\Impossible\`                                        | Registry                                  |
| `SYSTEMDRIVE`            | `C:\`                                                   | Hardcoded                                 |
| `TEMP` / `TMP`           | `C:\Temp\`                                              | Registry                                  |
| `USERPROFILE`            | `C:\Users\{USERNAME}\`                                  | Computed from token                       |
| `APPDATA`                | `C:\Users\{USERNAME}\AppData\Roaming\`                  | Computed                                  |
| `LOCALAPPDATA`           | `C:\Users\{USERNAME}\AppData\Local\`                    | Computed                                  |
| `USERNAME`               | current user account name                               | Token lookup                              |
| `COMPUTERNAME`           | `HKLM\SYSTEM\ComputerName\ActiveComputerName`           | Registry                                  |
| `OS`                     | `"Impossible_OS"`                                       | Hardcoded                                 |
| `PROCESSOR_ARCHITECTURE` | `"AMD64"`                                               | Hardcoded                                 |
| `NUMBER_OF_PROCESSORS`   | CPU count string                                        | `acpi_get_cpu_count()`                    |

- [ ] Per-user overrides from `HKCU\Environment` are applied **after** system defaults; user's `PATH` extension is **appended** with `;`, not replaced

---

## 3. `%VAR%` Expansion + `env_expand_path` `[Sonnet]`

> `env_expand` (depth-limited `%VAR%` substitution) is implemented in `02-kernel-core/TODO-22 §3`.
> `env_expand_path` is **new** -- not in TODO-14 -- and is specified here.

**Source file:** `src/kernel/env_path.c` (extends `src/kernel/env.c`)

- [ ] **`env_expand_path(task, template, argv0, filepath, out, max)`**:
  - Used by `file_assoc_open()` (→ `09-desktop-shell/TODO-02 §1`) to build the command line from a Registry command template (`"notepad.exe %1"` → `"notepad.exe C:\file.txt"`)
  - Substitution table:
    - `%1` → full quoted filepath: `"C:\path\to\file.txt"` (double-quote wrapped)
    - `%~1` → unquoted filepath (no quotes)
    - `%0` → `argv0` (the program path itself)
    - `%2`–`%9` → additional argument slots (empty string if not provided)
    - `%*` → all arguments concatenated with spaces
    - `%%` → literal `%`
  - After `%1`–`%9` substitution, run `env_expand()` on the result to also resolve `%VAR%` references in the template
  - Return bytes written (not including null); if output would overflow `max`, truncate + null
- [ ] **`%%` rule**: single `%` escape; works in both `env_expand` and `env_expand_path`
- [ ] Depth limit 4: if a substituted value itself contains `%VAR%`, recursion capped at 4 passes; return partial on depth 5 + `klog(WARN, "env", "expand depth limit")`
- [ ] Write `env_expand_path` test: template `"C:\\Impossible\\System32\\notepad.exe %1"`, filepath `"C:\\Users\\Default\\doc.txt"` → output `"C:\\Impossible\\System32\\notepad.exe \"C:\\Users\\Default\\doc.txt\""`

---

## 4. SYS_GETENV / SYS_SETENV / SYS_UNSETENV `[Sonnet]`

> Syscall implementation: `02-kernel-core/TODO-22 §5` (`NtQuery/SetEnvironmentVariable`).
> This section documents the **user-mode syscall numbers** and the user32/kernel32 wrappers.

- [ ] Confirm syscall numbers added to `include/kernel/syscall.h`:
  - `SYS_GETENV` = `{N}` -- `sys_getenv(const char *name, char *buf, size_t max)` → returns length or -1
  - `SYS_SETENV` = `{N+1}` -- `sys_setenv(const char *name, const char *value)` → 0 on success
  - `SYS_UNSETENV` = `{N+2}` -- `sys_unsetenv(const char *name)` → 0 on success
- [ ] User-mode `GetEnvironmentVariableA(name, buf, size)` → calls `NtQueryEnvironmentVariable` (which internally calls `env_get`)
- [ ] User-mode `SetEnvironmentVariableA(name, value)` → calls `NtSetEnvironmentVariable`; `value == NULL` → unset
- [ ] `ExpandEnvironmentStringsA(src, dst, size)` → calls `RtlExpandEnvironmentStrings_U` → `env_expand`
- [ ] All three `SYS_*` variants validate the user-mode buffer pointer is within process address space before dereferencing
- [ ] POSIX libc env surface (Linux-compat; → XREF `02-kernel-core/TODO-22-environment-variables.md §1`):
  - `extern char **environ` global synced to `task->environ` (read directly by `env` / `printenv` / coreutils)
  - `clearenv(3)` -- wraps an `env_unset` sweep
  - `putenv(3)` NON-copy aliasing vs `setenv(3)` copy semantics: document the divergence or provide a no-copy path (TODO-22 §1 `env_set` always `kstrdup`s)

---

## 5. PATH-Based Command Lookup + Session Cache `[Sonnet]`

> Core PATH iteration: `02-kernel-core/TODO-22 §7`. This section adds the **session cache**.

- [ ] **Session cache**: `cmd_cache[8]` in `shell.c` -- cache a hit ONLY if `name` is bare AND every searched PATH entry is drive-qualified (fully CWD-independent); LRU evict; invalidate on `PATH`/`PATHEXT`
- [ ] **Cache coherency**: a bare-name hit can still go stale via VFS mutation (create/delete/rename an earlier PATH candidate); bind entries to a PATH/PATHEXT/VFS generation counter or bypass. `where` stays cache-free (enumerates ALL matches)
- [ ] **`where <command>` command**: iterate all PATH directories (no cache shortcut); print every matching path (multiple hits possible); format: one `full_path\n` per match; no match → print `"INFO: Could not find files for the given pattern(s)."` (Windows `where.exe` phrasing)
- [ ] **Executable check**: when probing `dir\name.exe`, use `vfs_stat(path)` -- only report match if file exists and has nonzero size; never execute a 0-byte file
- [ ] **Extension precedence**: no-extension `name` iterates `PATHEXT` (`env_get`, split `;`, in order) not hardcoded `.exe`; empty/unset -> `.EXE`; default `.EXE` (-> XREF `02-kernel-core/TODO-22 §11`) until a batch processor adds `.CMD`/`.BAT`

---

## 6. argv / argc Process ABI + `cmd_tokenize` `[Sonnet]`

> Initial user stack layout (argc / argv / envp pushed before `iretq`): `02-kernel-core/TODO-11 §7`.
> This section specifies **`cmd_tokenize`** -- the command-line tokenizer that produces the
> `argv[]` the kernel pushes, and the Win32 contract apps read via `GetCommandLineA`.

**Source file:** `src/desktop/terminal.c` → `cmd_tokenize()`

- [ ] **`cmd_tokenize(const char *cmdline, char **argv_out, int max_argc)`**:
  - Rule set (Win32 `CommandLineToArgvW` compatible):
    - Whitespace (space, tab) outside quotes → token boundary
    - `"..."` -- double-quoted region; spaces inside do not split; quotes stripped from token
    - `\"` inside a quoted region → literal `"`
    - `\\` → literal `\` (only when preceding a `"` or `\\`)
    - `%VAR%` references: call `env_expand(current_task, token, expanded)` on each token after splitting; tokens that are entirely `%MISSING%` remain as `%MISSING%` literal (Windows behavior)
  - Returns `argc` (number of tokens written to `argv_out[]`); always NULL-terminates `argv_out`
  - Limits: max 256 tokens; each token ≤ 4095 chars; silently truncates if exceeded
- [ ] **`argv[0]` convention**: first token is always the command name / executable path (same as POSIX `argv[0]`); shell prepends the resolved full path from `shell_find_command()` if not an absolute path
- [ ] **`GetCommandLineA()` / `GetCommandLineW()`**: returns the **original unexpanded** flat command-line string from `PEB->ProcessParameters->CommandLine.Buffer`; apps that want tokens must call `CommandLineToArgvW` (stub → `cmd_tokenize` adapter) or parse themselves
- [ ] **`CommandLineToArgvW(lpCmdLine, pNumArgs)`** Win32 stub: allocates `char **argv` via `LocalAlloc`; calls `cmd_tokenize`; caller must `LocalFree` the result

---

## 7. Shell `.profile` Startup Script `[Sonnet]`

> Implementation: `02-kernel-core/TODO-22 §8`. This section adds the **default `.profile`**
> content shipped in the ISO.

**Source file:** `src/desktop/shell.c` → `shell_run_profile()`

- [ ] On shell startup: check `HKCU\Software\Impossible\Shell\SkipProfile` (DWORD 1 = skip); if not set, open `C:\Users\{name}\.profile` via `vfs_open`
- [ ] Parse line-by-line (max 4096 chars/line):
  - Lines starting with `#` → skip (comments)
  - `SET NAME=VALUE` → `env_set(current_task, name, value)`
  - `SET NAME=` → `env_unset(current_task, name)`
  - All other non-empty lines → `shell_execute_command(line)` (same as interactive input)
- [ ] **Default `.profile`** content shipped in ISO (written by installer to new user home):
  ```
  # Impossible OS default user profile
  SET EDITOR=notepad.exe
  SET PAGER=more.exe
  ```
- [ ] If `.profile` does not exist → silently skip (first-run is normal)
- [ ] Profile errors (command not found) → print `"[profile] error: {line}"` to stderr; do not abort remaining lines

---

## 8. `set` / `echo` / `env` / `where` Shell Commands `[Sonnet]`

> Core dispatch: `02-kernel-core/TODO-22 §7`. This section consolidates the exact
> **command contract** for each shell built-in.

- [ ] **`set` (no args)**: call `env_list(current_task, buf, sizeof buf)`; print each `KEY=VALUE` line; sort alphabetically (simple qsort on lines array)
- [ ] **`set NAME=VALUE`**: parse at first `=`; `env_set(current_task, name, value)`; print nothing on success (Windows `SET` behavior)
- [ ] **`set NAME=`** (empty value): `env_unset(current_task, name)`
- [ ] **`set /P NAME=PROMPT`** (stretch): print PROMPT, read a line from stdin, `env_set` the result
- [ ] **`echo`**: call `env_expand(current_task, args, expanded, max)` first; then print `expanded\n`; `echo` with no args → print blank line; `echo.` → blank line (Windows idiom)
- [ ] **`echo off` / `echo on`**: set shell `g_echo_mode` flag; when off, commands are not echoed before execution (used in batch scripts)
- [ ] **`env`** (alias for `set` no args): identical output; added for Unix familiarity
- [ ] **`where <name>`**: delegates to `shell_find_command` (§5) with "print all matches" mode; exit code 0 if found, 1 if not found
- [ ] **cmd.exe batch interpreter**: `FOR`/`IF`/`setlocal enabledelayedexpansion`/`CALL`/`GOTO`, `%~dp0` modifiers, delayed `!VAR!`, `.CMD`/`.BAT` exec; needs its own TODO when picked up (← XREF `02-kernel-core/TODO-22 §18`)

---

## OS Comparison


| ⭐  | Feature                                                      | 🪟 Win11                                                       | 🐧 Linux                                   | 🚀 Impossible OS                                 |
| --- | ------------------------------------------------------------ | -------------------------------------------------------------- | ------------------------------------------ | ------------------------------------------------ |
| 💎  | Per-process `KEY=VALUE` environ array                        | ✅ `PEB->ProcessParameters->Environment` UTF-16 block          | ✅ `execve` `envp[]`; `environ` global     | ⬜ §1 -- `D02T22 `; `struct task` environ        |
| 💎  | System default variables                                     | ✅ Registry `HKLM\SYSTEM\...\Environment` + `HKCU\Environment` | ✅ `/etc/environment` + PAM + `~/.profile` | ⬜ §2 -- `D02T22 `; same dual-hive Registry      |
| 💎  | `%VAR%` expansion                                            | ✅ CMD `%VAR%` + `ExpandEnvironmentStrings`                    | ✅ `$VAR` / `${VAR}` (shell-level)         | ⬜ §3 -- `D02T22 `; depth-4 cap prevents         |
| ⭐  | `env_expand_path` `%1`–`%9` file-assoc template substitution | ✅ `ShellExecute` HKCR command template (`%1`                  | ⚠️ `xdg-open` delegates to desktop; no     | ⬜ §3 -- (this TODO); quote-wraps filepath; also |
| 💎  | `SYS_GETENV` / `SYS_SETENV` syscalls                         | ✅ `NtQueryEnvironmentVariable` / `NtSetEnvironmentVariable`   | ✅ `getenv`/`setenv` via CRT (no direct    | ⬜ §5 -- `D02T22 `; SSDT + user-mode             |
| 💎  | PATH lookup + executable-not-found error                     | ✅ `SearchPath`; `where.exe` utility                           | ✅ `execvp` + shell `type`/`which`         | ⬜ §7 -- `D02T22 ` + §6 (session                 |
| 💎  | `cmd_tokenize` with Win32 quote/escape rules                 | ✅ `CommandLineToArgvW`                                        | ✅ POSIX shell word-splitting              | ⬜ §6 -- `\"` inside quotes, `\\` before         |
| 💎  | `CommandLineToArgvW` Win32 stub                              | ✅ `shell32.dll` `CommandLineToArgvW`                          | ❌ Not applicable (different model)        | ⬜ §6 -- adapter over `cmd_tokenize`             |
| 💎  | `.profile` sourced on shell startup                          | ✅ `HKCU\...\Run` / PowerShell profile                         | ✅ `~/.profile` / `~/.bashrc`              | ⬜ §8 -- `D02T22 ` + §8; default                 |
| 💎  | `set`/`echo`/`env`/`where` shell built-ins                   | ✅ CMD `set`/`echo`/`where`                                    | ✅ `export`/`echo`/`env`/`which`           | ⬜ §8 -- `echo.` blank-line idiom + `set         |

Impossible OS merges the Windows `%1`–`%9` file-association template model with `env_expand`
into a single `env_expand_path` pass -- so a command template like `"player.exe %1 /fullscreen"`
both substitutes the filepath **and** resolves any `%VAR%` references in the path, in one
function call, without the shell or file manager needing separate expansion steps.

---

## Verification

Run `bash scripts/build.sh run` for each verification step.

- [ ] **env_get/set**: `env_set(task, "FOO", "bar")` → `env_get(task, "FOO") == "bar"`; `env_get(task, "foo") == "bar"` (case-insensitive); `env_unset` → `env_get` returns NULL
- [ ] **env_init_defaults**: fresh `task_exec` → `env_get("PATH")` non-null, contains `C:\Impossible\Bin`; `env_get("TEMP")` = `C:\Temp\`; `env_get("USERNAME")` = `"Default"` (or current user)
- [ ] **`%VAR%` expansion**: shell `echo %PATH%` → prints PATH value; `echo %%` → prints `%`; undefined `echo %NOPE%` → prints `%NOPE%` literal; depth-limit: 5-level self-referencing var → partial result without hang
- [ ] **`env_expand_path`**: template `"notepad.exe %1"`, filepath `"C:\file.txt"` → `"notepad.exe \"C:\\file.txt\""`; `%0` → argv0; `%2` with only one arg → empty string substitution
- [ ] **`SYS_SETENV`**: user-mode `SetEnvironmentVariableA("X", "42")` → `GetEnvironmentVariableA("X", buf, 8)` → `buf == "42"`, return 2; `SetEnvironmentVariableA("X", NULL)` → subsequent `GetEnvironmentVariableA` returns 0 + `ERROR_ENVVAR_NOT_FOUND`
- [ ] **PATH lookup**: `where notepad` → prints `C:\Impossible\System32\notepad.exe`; `where nonexistent_cmd` → exit code 1; session cache: second `where notepad` uses cache (serial log shows "cache hit")
- [ ] **`cmd_tokenize`**: `cmd_tokenize("foo \"bar baz\" qux")` → 3 tokens: `foo`, `bar baz`, `qux`; `"a\"b"` → token `a"b`; `%%1` → literal `%1`
- [ ] **argv in process**: launch `hello.exe foo bar` → `argc == 3`; `argv[0]` = full path; `argv[1]` = `"foo"`; `argv[2]` = `"bar"`; `GetCommandLineA()` returns `"hello.exe foo bar"` flat string
- [ ] **`.profile`**: create `C:\Users\Default\.profile` with `SET MYVAR=hello`; start shell → `echo %MYVAR%` → prints `hello`; `SkipProfile=1` in Registry → var not set
- [ ] **Shell commands**: `set` → lists all env vars alphabetically; `set PATH=X:\new` → `echo %PATH%` → `X:\new`; `echo hello world` → prints `hello world`; `env` → same output as `set`
- [ ] Commit: `"env: per-process environ, %VAR% expansion, env_expand_path, cmd_tokenize, set/echo/where, .profile"`
