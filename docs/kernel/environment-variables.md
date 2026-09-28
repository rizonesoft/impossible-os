<!-- docs: covers=todo/02-kernel-core/TODO-22-environment-variables.md sources=include/kernel/env.h,src/kernel/env.c,src/kernel/env_searchpath.c,src/kernel/env_apppaths.c,src/kernel/nt/nt_env.c,include/kernel/nt/nt_env.h,src/kernel/nt/nt_rtlenv.c,include/kernel/nt/nt_rtlenv.h,src/kernel/test/test_env.c,src/kernel/main/boot_desktop.c reviewed=2026-09-28 order=22 -->
# Environment Variables

## What is it?

Every task carries a private, sorted array of `"NAME=VALUE"` UTF-8 strings (`task->environ`) plus an argument vector (`task->argv`), the kernel-side storage and API a Win32 process needs for `PATH`, `%VAR%` expansion, and `argv`/`argc` delivery. Unlike Windows, where `GetEnvironmentVariable` resolves entirely in user-mode ntdll over a PEB-resident block, Impossible OS keeps the authoritative copy in the kernel and exposes it through two `Nt*` syscalls; user-mode callers are meant to reach it through Win32 `kernel32` and ntdll `Rtl*Environment*` wrappers, neither of which exists yet (see below).

## How does it work?

`struct task` holds `environ`/`environ_count`, `argv`/`argc`, and a `mutex_t environ_lock` (a sleeping lock, not a spinlock, because mutation calls `kmalloc`/`pmm_alloc_contiguous`) ([`env.h`](../../include/kernel/env.h)). `env_set()`/`env_unset()` keep the array sorted case-insensitively by binary-search insert (`env_bsearch`), so `GetEnvironmentStrings`-shaped block builders see alphabetical order for free. There is deliberately no raw `env_get()`: a borrowed pointer would use-after-free the moment a sibling thread mutates the array, so reads are either `env_get_copy()` (copies under the lock) or a locked `env_lock()`/`env_peek_locked()`/`env_unlock()` batch pair ([`env.h`](../../include/kernel/env.h)).

`env_init_defaults()` (`src/kernel/env.c`) seeds a task in three precedence layers: a synthesised base (`COMPUTERNAME`, `USERNAME`, `PATH`, `SYSTEMROOT`, ...), then an `HKLM\...\Session Manager\Environment` overlay, then an `HKCU\Environment` overlay (`PATH` is appended, not replaced). `env_init_kernel_task()` calls this for PID 0 from `boot_phase3()` right after `task_init()` ([`boot_desktop.c`](../../src/kernel/main/boot_desktop.c)), falling back to a hardcoded `PATH`/`SYSTEMROOT`/`TEMP` table if the Registry is not ready. Child processes are designed to inherit their parent's block through `env_copy()`, but nothing calls it yet, so only PID 0 gets a seeded environment and every other new task starts with an empty one.

`env_expand()` does single-pass `%VAR%` substitution (Win32 `ExpandEnvironmentStrings` semantics: a resolved value is never re-scanned, `%%` is preserved verbatim as an empty variable name rather than treated as a cmd-style escape) under a work budget (`ENV_EXPAND_WORK_MAX`) that charges the outer walk, the closing-`%` scan, and the lookup cost together, so a pathological input cannot hold `environ_lock` unbounded. `task_set_argv()` deep-copies an argv vector using the same allocator `environ` uses, so `env_free()` reclaims both at the `task_cleanup` reap barrier. `SYS_EXEC` snapshots the caller's `argv[]`/`envp[]` into a kernel bounce buffer once, then calls `task_set_argv()` and `env_adopt_block()` before `task_exec()`.

At the syscall boundary, `NtQueryEnvironmentVariable` (SSDT `0x03DD`) and `NtSetEnvironmentVariable` (SSDT `0x03DE`) read and write `task->environ` directly ([`nt_env.c`](../../src/kernel/nt/nt_env.c)). A parallel `nt_rtlenv.c` implements the ntdll `Rtl*Environment*` family (`RtlQueryEnvironmentVariable_U`, `RtlSetEnvironmentVariable`, `RtlCreateEnvironment`, `RtlExpandEnvironmentStrings_U`, and the counted non-`_U` forms) as kernel-side routines over the same syscalls, but a test (`test_rtlenv_exports_not_user_reachable`) pins them absent from any `pe.c` ntdll export row: they exist and are tested, but no PE binary can import them today. `SearchPathW`/`SearchPathA` ([`env_searchpath.c`](../../src/kernel/env_searchpath.c)) and the App Paths registry lookup ([`env_apppaths.c`](../../src/kernel/env_apppaths.c)) are plain kernel-C callables taking an explicit `struct task *caller`, not syscalls, so they are reachable only from other kernel code today.

## What are its interfaces?

| Interface | Purpose |
| --- | --- |
| `env_get_copy()`, `env_set()`, `env_unset()`, `env_copy()`, `env_free()` | Core per-task environment storage ([`env.h`](../../include/kernel/env.h)) |
| `env_lock()`, `env_unlock()`, `env_peek_locked()` | Locked batch-read fast path |
| `env_expand()`, `env_expand_budget()` | `%VAR%` single-pass expansion with a work ceiling |
| `task_set_argv()`, `env_adopt_block()`, `argv_to_cmdline()` | argv storage, envp adoption, `CommandLine` encoding |
| `NtQueryEnvironmentVariable` (`0x03DD`), `NtSetEnvironmentVariable` (`0x03DE`) | The only user-reachable env syscalls ([`nt_env.c`](../../src/kernel/nt/nt_env.c)) |
| `RtlQueryEnvironmentVariable_U`, `RtlSetEnvironmentVariable`, `RtlCreateEnvironment`, `RtlExpandEnvironmentStrings_U` | ntdll-shaped kernel routines, no ntdll export row yet ([`nt_rtlenv.c`](../../src/kernel/nt/nt_rtlenv.c)) |
| `env_create_block()`, `env_destroy_block()`, `env_build_block()`, `env_parse_block()` | `CreateEnvironmentBlock`-shaped contiguous block builders |
| `env_set_drive_cwd()`, `env_get_drive_cwd()` | Hidden `=C:`/`=D:` per-drive current-directory variables |
| `SearchPathW()`, `SearchPathA()`, `SetSearchPathMode()` | Win32 `SearchPath` family ([`env_searchpath.c`](../../src/kernel/env_searchpath.c)) |
| `app_paths_lookup()`, `app_paths_register()` | App Paths registry executable lookup ([`env_apppaths.c`](../../src/kernel/env_apppaths.c)) |
| `env_sanitize_for_elevation()`, `env_is_secure_context()` | Strip `LD_PRELOAD`-class variables at an elevation boundary |

## How do I use it?

The environment subsystem is always on for kernel-resident code; there is no boot setting. From C, call `env_get_copy()`/`env_set()` on a `struct task *`, or reach the same storage from user mode through `NtQueryEnvironmentVariable`/`NtSetEnvironmentVariable`.

```bash
bash scripts/test.sh SUITE=abi    # env, argv, Rtl, SearchPath, App Paths suites
```

The suite lives in [`test_env.c`](../../src/kernel/test/test_env.c) under `TEST_CAT_ABI`. `cmd.exe` has no `SET` command and there is no Win32 `GetEnvironmentVariable`/`SetEnvironmentVariable` to call from a user-mode program yet (see below), so exercising this subsystem today means either a kernel unit test or a direct `NtQueryEnvironmentVariable`/`NtSetEnvironmentVariable` syscall.

## What is not implemented yet?

- **Win32 `GetEnvironmentVariable`/`SetEnvironmentVariable`/`ExpandEnvironmentStrings`/`GetCommandLine`.** These need `kernel32` user-mode runtime primitives (`LocalAlloc`, `SetLastError`, `MultiByteToWideChar`) that do not exist yet ([Win32 API Wrappers](../../todo/02-kernel-core/TODO-22-environment-variables.md#6-win32-api-wrappers)).
- **`SET`, `%VAR%` expansion and `PATH` search in the shell.** `cmd.exe` ([`user/cmd.c`](../../user/cmd.c)) has a literal `ECHO` but no `SET`, no variable expansion and no `PATH` lookup ([Shell Integration: PATH Lookup & SET/ECHO](../../todo/02-kernel-core/TODO-22-environment-variables.md#7-shell-integration-path-lookup--setecho)).
- **`.profile` startup and `source`.** `cmd.exe` runs no startup script ([`.profile` Startup Script](../../todo/02-kernel-core/TODO-22-environment-variables.md#8-profile-startup-script)).
- **`WM_SETTINGCHANGE` broadcast, `setx`, and a `sysdm.cpl` Environment Variables tab.** Need the Win32 wrappers plus a running window manager ([Environment Change Notifications](../../todo/02-kernel-core/TODO-22-environment-variables.md#9-environment-change-notifications)).
- **App Paths as a `PATH`-miss fallback.** `app_paths_lookup()` is implemented and tested, but nothing calls it yet; the shell's command lookup is the intended caller ([App Paths Registry-Based Executable Lookup](../../todo/02-kernel-core/TODO-22-environment-variables.md#17-app-paths-registry-based-executable-lookup)).
- **ntdll `Rtl*Environment*` exports reaching a real PE binary.** The kernel-side implementations are shipped and tested but carry no `pe.c` export row, gated behind an unsynchronized-PMM-bitmap concern in `03-memory-concurrency/TODO-03` ([ntdll Rtl Environment Exports](../../todo/02-kernel-core/TODO-22-environment-variables.md#21-ntdll-rtl-environment-exports)).
- **cmd.exe pseudo-variables (`%CD%`, `%ERRORLEVEL%`, `%RANDOM%`) and delayed `!VAR!` expansion.** These layer on the shell's missing `%VAR%` and `SET` support ([cmd.exe Dynamic Pseudo-Variables & Delayed Expansion](../../todo/02-kernel-core/TODO-22-environment-variables.md#18-cmdexe-dynamic-pseudo-variables--delayed-expansion)).
- **Child processes inheriting their parent's environment.** `env_copy()` exists and is tested but has no live caller; `task_fork`, `task_create` and `task_create_user` leave a new task's `environ` empty, so a program `cmd.exe` launches does not see its `PATH` ([NtCreateProcess / NtCreateThread / Process-Thread Lifecycle](../../todo/02-kernel-core/TODO-12-native-api-ssdt.md#7-ntcreateprocess--ntcreatethread--process-thread-lifecycle)).

## How does it compare with Windows 11 and Linux?

The kernel-side storage model matches both operating systems in shape: a per-process environment block, `%VAR%`/`$VAR` expansion, `argv` delivered to `main`, and a sorted, size-limited block matching Windows' `CreateProcess` contract more closely than Linux's unsorted `ARG_MAX`-bounded one. Where Impossible OS currently falls short of both is the user-visible surface: Windows and Linux processes can call `getenv()`/`GetEnvironmentVariable` and a shell can run `SET`/`export`; here, only kernel-resident code and the two `Nt*` syscalls can read or write an environment, because the `kernel32` wrappers, the ntdll export rows and the shell's `SET` support are unbuilt. The architecture itself diverges from Windows by design: Windows resolves environment access in user-mode ntdll over a PEB-resident block with no syscall, while Impossible OS keeps the authoritative store in the kernel and treats the Rtl layer as a compatibility bridge over two syscalls.

## See also

- [Environment Variables & Process Arguments roadmap](../../todo/02-kernel-core/TODO-22-environment-variables.md)
- [PEB, TEB and the User-Mode ABI](peb-teb-user-abi.md)
- [Native API and SSDT](native-api-ssdt.md)
