# P0107 — Environment Variables

> **Goal:** Per-process environment variable storage, `%VAR%` expansion, and PATH-based command lookup.
> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB. `kmalloc` is ONLY for small kernel structs (≤ 4 KB). Violating this crashes the 2 MiB heap silently. See `rules.md` Known Gotchas.

---

## 1. System & User Environment

**Prompt:** Environment variables are key-value string pairs stored per-process, inherited by children, and essential for the shell's PATH-based command lookup. Store as a flat array of `"KEY=VALUE"` strings in each `struct task` (same as POSIX environ). System-wide defaults (PATH, SYSTEMROOT, TEMP, HOME, USERNAME, COMPUTERNAME) should be populated from Registry entries at boot. The `env_expand` function replaces `%VAR%` tokens in strings — this is used by the shell for command expansion and by file associations for argument templates. The shell's command lookup must search each PATH directory in order, trying the command name with and without `.exe` extension. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"kernel: environment variables"`. Update `README.md` if it contains stale or incorrect references to environment variables or PATH. Add notes, gotchas, and design decisions directly in this TODO section covering environment variable storage, per-process inheritance, env_expand, and PATH-based command lookup.

### Notes

**Storage:** Flat `char *environ[]` array in `struct task`, terminated by `NULL`. Inherited via deep copy on fork/exec.

**Default variables:**
- `PATH=C:\Impossible\Bin;C:\Programs\`
- `SYSTEMROOT=C:\Impossible\`
- `TEMP=C:\Temp\`
- `HOME=C:\Users\Default\`
- `USERNAME=Default`
- `COMPUTERNAME=IMPOSSIBLE-PC`

**`%VAR%` expansion:** scanner walks input string, finds `%`, extracts name until next `%`, looks up in environ, substitutes. Handles nested expansion with a depth limit of 4.


- [ ] Implement `env_get(task, name)` — look up variable by name in task environ
- [ ] Implement `env_set(task, name, value)` — set/create variable in task environ
- [ ] Implement `env_unset(task, name)` — remove variable from task environ
- [ ] Implement `env_expand(input, output, max)` — expand `%VAR%` references in-place
- [ ] Define default system variables (populated from Registry at boot):
  - [ ] `PATH` = `C:\Impossible\Bin;C:\Programs\`
  - [ ] `SYSTEMROOT` = `C:\Impossible\`
  - [ ] `TEMP` = `C:\Temp\`
  - [ ] `HOME` = `C:\Users\{username}\`
  - [ ] `USERNAME` = `Default`
  - [ ] `COMPUTERNAME` = `IMPOSSIBLE-PC`
- [ ] Per-process environ array in `struct task` — inherited (deep-copied) on fork/exec
- [ ] Add `SYS_GETENV` and `SYS_SETENV` syscalls (define numbers in `syscall.h`)
- [ ] Update shell to use `PATH` for command lookup (try `cmd` then `cmd.exe` in each dir)
- [ ] Commit: `"kernel: environment variables"`

---

## 2. Shell Config File (`.profile` / `autoexec.conf`)

**Prompt:** On Linux, shell startup sources `~/.profile` or `~/.bashrc` — a script that sets env vars, defines aliases, and runs startup commands. On Windows, `HKCU\Environment` Registry key sets user env vars; `%USERPROFILE%\autoexec.bat` is legacy. Impossible OS should source `C:\Users\Default\.profile` on shell startup: execute each line as a shell command (including `SET VAR=VALUE` lines). This allows users to customise their environment persistently without touching the Registry. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"shell: .profile startup script"`. Add notes directly in this TODO section.


- [ ] On shell startup: check `C:\Users\Default\.profile` exists via VFS
- [ ] If found: read line-by-line, execute each as a shell command (same as typed input)
- [ ] Support `SET VAR=VALUE` lines in `.profile` — call `env_set()`
- [ ] Support `#` comment lines — skip
- [ ] Ship a default `.profile` in the ISO with PATH and TEMP set
- [ ] Commit: `"shell: .profile startup script"`

---

## 3. Process Argument Passing (`argv` / `argc`)

**Prompt:** When the shell launches a program (`notepad.exe hello.txt`), the arguments must be passed to the child process. On POSIX, `execve(path, argv[], envp[])` passes both arguments and environment. On Windows, `CreateProcess` passes a single command-line string that the runtime parses. Impossible OS's `exec` syscall must accept an `argv[]` array and copy it into the child process's user-space stack below the stack pointer at program entry, following the POSIX System V AMD64 ABI stack layout. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"kernel: argv/argc process arguments"`. Add notes directly in this TODO section covering the ABI stack layout.

**ABI stack layout at program entry (System V AMD64):**
```
[RSP]           argc
[RSP+8]         argv[0]   (path to executable)
[RSP+16]        argv[1]   (first argument)
...
[RSP+8*(N+1)]   NULL      (argv terminator)
[RSP+8*(N+2)]   envp[0]
...
[RSP+8*(N+M+2)] NULL      (envp terminator)
```


- [ ] `exec(path, argv[], envp[])` syscall: copy argv strings + envp to child user-space stack
- [ ] Follow System V AMD64 ABI: argc, argv[], NULL, envp[], NULL at bottom of stack
- [ ] Shell: parse command line into `argv[]` array (handle quoted arguments)
- [ ] Child `main(int argc, char *argv[])` receives correct values
- [ ] Test: `echo hello world` → child receives `argc=3, argv=["echo","hello","world"]`
- [ ] Commit: `"kernel: argv/argc process arguments"`

---

## Priority Order

| Priority | Section                     | Reason                                               |
|----------|-----------------------------|------------------------------------------------------|
| 🔴 P0    | 1. env_get/set/unset        | Shell needs PATH lookup to find any command          |
| 🔴 P0    | 1. env_expand               | Shell `%VAR%` expansion used everywhere              |
| 🔴 P0    | 3. argv/argc                | Programs need arguments — basic ELF ABI requirement  |
| 🟠 P1    | 1. Default variables        | Sane environment from first boot                     |
| 🟠 P1    | 1. SYS_GETENV / SYS_SETENV  | User-mode apps need env access                       |
| 🟡 P2    | 2. .profile startup script  | User personalisation; quality of life                |

---

## OS Comparison

| Feature                          | Windows 11                   | Linux / Bash              | Impossible OS                      |
|----------------------------------|------------------------------|---------------------------|------------------------------------|
| Per-process env vars             | ✅ `GetEnvironmentVariable`  | ✅ POSIX `environ[]`      | ⬜ §1 P0                            |
| `%VAR%` / `$VAR` expansion       | ✅ `%VAR%` in cmd.exe        | ✅ `$VAR` in bash         | ⬜ §1 P0 — `%VAR%` Windows-style   |
| PATH-based command lookup        | ✅ `PATH` env var            | ✅ `PATH` env var         | ⬜ §1 P0                            |
| Default system variables         | ✅ Registry + system env     | ✅ `/etc/environment`     | ⬜ §1 P1 — Registry-backed         |
| Shell startup config             | ✅ Registry `HKCU\Env`       | ✅ `~/.profile`/`.bashrc` | ⬜ §2 P2 — `.profile` file         |
| argv/argc to child process       | ✅ Command-line string       | ✅ `execve` argv[]        | ⬜ §3 P0 — System V ABI stack      |
| **Windows-style `%VAR%` syntax** | ✅                           | ❌ (`$VAR` only)          | ⬜ **§1 — familiar for Win users**  |
