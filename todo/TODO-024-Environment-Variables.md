# P0107 — Environment Variables

> **Goal:** Per-process environment variable storage, `%VAR%` expansion, and PATH-based command lookup.
> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB. `kmalloc` is ONLY for small kernel structs (≤ 4 KB). Violating this crashes the 2 MiB heap silently. See `rules.md` Known Gotchas.

---

## 1. System & User Environment

**Prompt:** Environment variables are key-value string pairs stored per-process, inherited by children, and essential for the shell's PATH-based command lookup. Store as a flat array of `"KEY=VALUE"` strings in each `struct task` (same as POSIX environ). System-wide defaults (PATH, SYSTEMROOT, TEMP, HOME, USERNAME, COMPUTERNAME) should be populated from Registry entries at boot. The `env_expand` function replaces `%VAR%` tokens in strings — this is used by the shell for command expansion and by file associations for argument templates. The shell's command lookup must search each PATH directory in order, trying the command name with and without `.exe` extension. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"kernel: environment variables"`. Update `README.md` if it contains stale or incorrect references to environment variables or PATH. Add notes, gotchas, and design decisions directly in this TODO section covering environment variable storage, per-process inheritance, env_expand, and PATH-based command lookup.


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

## Priority Order

| Priority | Section                       | Reason                                      |
|----------|-------------------------------|---------------------------------------------|
| 🔴 P0     | 1. env_get/set/unset          | Shell needs PATH lookup to find any command |
| 🔴 P0     | 1. env_expand                 | Shell `%VAR%` expansion used everywhere     |
| 🟠 P1     | 1. Default variables          | Sane environment from first boot            |
| 🟠 P1     | 1. SYS_GETENV / SYS_SETENV   | User-mode apps need env access              |
