---
schema_version: 1
id: linux-compat
domain: 10-platform-services
status: active
title: "TODO-10 -- Linux ELF Compatibility Layer"
---

# TODO-10 -- Linux ELF Compatibility Layer

**Domain:** `10-platform-services`
**Goal:** Add a secondary ELF/POSIX compatibility layer so statically-linked Linux x86-64 binaries (busybox, coreutils, musl-static apps) run unmodified on Impossible OS -- "WSL in reverse", without modifying the host binary.

> [!IMPORTANT]
> **Depends on:** `TODO-07 §1–8` -- ring-3 execution, `SYSCALL`/`SYSRET`, `exec_load()` PE-first format probe. The Linux compat layer registers as the ELF fallback path in `exec_load()`.
> **Not a prerequisite for Win32:** This layer is purely additive. PE/Win32 remains the native format; Linux ELF support is a secondary compatibility shim.
> **Continues from:** `todo-old/510-Long-Term-Stretch/TODO-540-Linux.md` (migrated).

---

## Important Notes

- `elf_load()` already exists in `src/kernel/elf.c` and parses ELF64 headers + maps `PT_LOAD` segments (`include/kernel/elf.h`). It **does not** set up the SYSV user stack (auxv, `argv`, `envp`) required for a Linux process start -- that is new work in §2.
- The existing `elf_load()` is also used for the **kernel ELF** (`kernel.exe`) loaded by the bootloader; modifications must not break kernel loading. The Linux compat layer needs a separate `elf_linux_load()` path.
- `exec_load()` (defined in `TODO-07 §8`) already probes PE first, ELF second -- the Linux handler hooks into the ELF branch by setting a `task->is_linux_elf` flag.
- `SYS_FORK=5`, `SYS_EXEC=6`, `SYS_MMAP=37` are already defined in `include/kernel/sched/syscall.h`. Linux syscall numbers are entirely different (Linux `read=0`, `write=1`, etc.) -- the compat layer maintains its own translation table.
- The Linux SYSV calling convention uses `RDI`, `RSI`, `RDX`, `R10`, `R8`, `R9` for args -- the **opposite** register order from the Win32 ABI (`RCX`, `RDX`, `R8`, `R9`). The syscall entry dispatcher must branch on `task->is_linux_elf` before touching registers.
- No dynamic ELF support in this TODO -- static binaries only. `PT_INTERP` (dynamic linker) → return `ENOEXEC`. This is tracked in §8 as a future stretch.
- Per-process Linux fd table (int → `HANDLE`) is distinct from the Win32 `handle_table[]` in `struct task` -- it is a parallel structure only present when `is_linux_elf` is set.

---

## Inputs

| Path | Purpose |
|------|---------|
| `include/kernel/elf.h` | Existing ELF types: `ET_EXEC`, `ET_DYN`, `EM_X86_64`, `PT_LOAD`, `elf_load()`, `elf_load_result` |
| `src/kernel/elf.c` | Existing ELF loader (extend for SYSV stack setup) |
| `include/kernel/sched/task.h` | `task_t`, `task_create_user()` -- add `is_linux_elf` flag + `fd_table[]` |
| `include/kernel/sched/syscall.h` | `SYS_FORK`, `SYS_EXEC`, `SYS_MMAP` numbers; existing syscall dispatch |
| `include/kernel/mm/mmap.h` | `mmap_region_t`, kernel `mmap()`/`munmap()` -- back `sys_mmap`/`sys_munmap` |
| `include/kernel/mm/heap.h` | `kmalloc`/`kfree` for compat structs |
| `include/kernel/fs/vfs.h` | `vfs_open`, `vfs_read`, `vfs_stat`, `vfs_readdir` -- back POSIX file ops |
| → XREF: `TODO-07 §1,8` | Ring-3 `SYSCALL`/`SYSRET` entry; `exec_load()` format probe (ELF branch) |
| → XREF: `TODO-08 §4,5` | `CreateFile`/`ReadFile`/`WriteFile` Win32 wrappers; `VirtualAlloc`/`VirtualFree` |
| → XREF: `02-kernel-core/TODO-17-threads-sched.md` | Task flags, scheduler integration for `SIGCHLD` delivery |
| → XREF: `02-kernel-core/TODO-23-exception-dispatch-seh.md §15` | Fault-to-signal mapping and signal frame setup for Linux compat processes; §8 of this TODO registers signal handlers via `rt_sigaction`, TODO-10 §15 delivers faults as POSIX signals |

---

## Outcome

- Statically-linked Linux x86-64 ELF binaries execute on Impossible OS via `exec_load()`.
- ~30 core Linux syscalls (`read`, `write`, `open`, `close`, `mmap`, `brk`, `exit`, `getpid`, `uname`, etc.) translate to native Impossible OS equivalents.
- Linux path strings (`/home/user/file`) are translated to Windows paths (`C:\Users\Default\file`) transparently.
- Per-process integer file descriptor table maps Linux fds (0/1/2 = stdin/stdout/stderr) to native HANDLEs.
- Basic signal stubs (`SIGINT`, `SIGCHLD`, `SIGKILL`) work for foreground process control.
- Busybox static binary runs: `busybox ls /`, `busybox cat`, `busybox echo`, `busybox sh` function.
- ELF processes are labeled `[Linux]` in the process list.

---

## Implementation Order

| #   | Section                                                              | Tag        | Dep            | Mark |
| --- | -------------------------------------------------------------------- | ---------- | -------------- | ---- |
| 1   | `task_t` Linux flags + fd table                                      | `[Sonnet]` | TODO-07 §1     | ⭐   |
| 2   | ELF Linux loader (SYSV stack + auxv)                                 | `[Opus]`   | §1, TODO-07 §8 | ⭐   |
| 3   | Syscall entry dispatch (`is_linux_elf` branch)                       | `[Opus]`   | §2             | ⭐   |
| 4   | 🐧 Linux syscall translation table (file I/O, process, memory, misc) | `[Sonnet]` | §3             | ⭐   |
| 5   | Path translation (`/` → `C:\`, `/tmp`, `/proc` stubs)                | `[Sonnet]` | §4             | ⭐   |
| 6   | 🐧 Linux file descriptor table                                       | `[Sonnet]` | §4             | ⭐   |
| 7   | POSIX filesystem stubs (`opendir`/`readdir`/`getdents`)              | `[Sonnet]` | §6             | 💎   |
| 8   | Signal stubs (`rt_sigaction`, `SIGINT`, `SIGCHLD`, `SIGKILL`)        | `[Opus]`   | §3             | 💎   |
| 9   | Shell ELF integration + `[Linux]` process tag                        | `[Sonnet]` | §2             | ⭐   |
| 10  | Test: static hello + busybox                                         | `[Sonnet]` | §4–7           | 💎   |
| 11  | Future: dynamic ELF (stretch)                                        | `[Opus]`   | §2–8           | 💎   |

---

## 1. `task_t` Linux Flags + FD Table `[Sonnet]`

Extend `struct task` in `include/kernel/sched/task.h` with Linux compat fields. These fields are only populated when `is_linux_elf = 1`.

- [ ] Add `uint8_t is_linux_elf` flag to `struct task`; initialize to 0 for all Win32 tasks
- [ ] Add per-process Linux fd table: `HANDLE linux_fd_table[LINUX_MAX_FDS]` (256 entries); `-1`/`INVALID_HANDLE_VALUE` = free slot
- [ ] Add `uint64_t linux_brk_base` and `uint64_t linux_brk_current` for `sys_brk()` heap tracking
- [ ] Add `struct linux_signal_handler linux_sighandlers[32]`:
  ```
  struct linux_signal_handler { uint64_t handler_fn; uint64_t sa_mask; uint32_t sa_flags; };
  ```
- [ ] Pre-populate `linux_fd_table[0/1/2]` with stdin/stdout/stderr HANDLEs when task is created as `is_linux_elf`
- [ ] `fd_alloc(task, handle)` → return lowest free fd slot; store `handle`
- [ ] `fd_to_handle(task, fd)` → return `HANDLE` or `INVALID_HANDLE_VALUE`
- [ ] `fd_close(task, fd)` → `CloseHandle()` + clear slot
- [ ] `fd_dup2(task, oldfd, newfd)` → copy handle, close old newfd if open
- [ ] Commit: `"compat: Linux task flags and fd table"`

---

## 2. ELF Linux Loader (SYSV Stack + auxv) `[Opus]`

Create `src/compat/linux/elf_linux.c` with `elf_linux_load()`. Unlike the kernel `elf_load()` (used for `kernel.exe`), this path sets up a full Linux user-mode process stack with the SYSV ABI initial stack layout required by `_start` in musl/glibc.

- [ ] `elf_linux_load(data, size, path, argc, argv, envp)`:
  - Validate ELF magic (`\x7FELF`), class (64-bit), machine (`EM_X86_64`), type (`ET_EXEC` or `ET_DYN`)
  - Reject `PT_INTERP` (dynamic linker) with `klog_warn("compat: dynamic ELF not supported: %s")` → return `ENOEXEC`
  - Map all `PT_LOAD` segments via `vmm_alloc_user()` at their specified `p_vaddr`; `memcpy` file data; zero BSS (`p_memsz > p_filesz`)
  - Set `task->is_linux_elf = 1`, `task->linux_brk_base = highest_mapped_vaddr + PAGE_SIZE`
- [ ] Build SYSV initial stack (grows down from top of user stack, typically `0x7FFFFFFFE000`):
  - `[rsp]` = `argc` (uint64_t)
  - `[rsp+8]` = `argv[0]` ... `argv[argc-1]` (pointers to strings), then NULL
  - Then `envp[0]` ... `envp[n]` NULL-terminated (at minimum: `PATH=C:\\Impossible\\Bin`, `HOME=C:\\Users\\Default`, `TERM=xterm`)
  - Then auxiliary vector (auxv) `AT_key, AT_val` pairs:
    - `AT_PHDR` = ELF `e_phoff` + image base
    - `AT_PHENT` = `sizeof(Elf64_Phdr)`
    - `AT_PHNUM` = `e_phnum`
    - `AT_PAGESZ` = 4096
    - `AT_ENTRY` = ELF `e_entry`
    - `AT_UID = AT_EUID = AT_GID = AT_EGID` = 1000
    - `AT_SECURE` = 0
    - `AT_NULL` terminator
- [ ] Set `task->linux_fd_table[0/1/2]` = stdin/stdout/stderr
- [ ] Launch ring-3 at `e_entry` with RSP pointing to the constructed stack frame (via `iretq`, same as `pe_exec()`)
- [ ] Update `exec_load()` (TODO-07 §8): on ELF detection, call `elf_linux_load()` instead of bare `elf_load()`, passing `argc`/`argv` from shell
- [ ] Commit: `"compat: ELF Linux loader with SYSV stack and auxv"`

---

## 3. Syscall Entry Dispatch (`is_linux_elf` Branch) `[Opus]`

Extend the `syscall_entry.asm` handler from `TODO-07 §1` to branch to the Linux translation layer when the current task has `is_linux_elf = 1`. This is a security-critical dispatch path -- the wrong branch would allow Linux binaries to call Win32 syscalls with SYSV register layout.

- [ ] In `syscall_entry.asm`, after SWAPGS and saving registers:
  - Read `current_task->is_linux_elf` (GS-relative access or kernel stack pointer dereference)
  - If set: jump to `linux_syscall_dispatch` (separate C function)
  - If clear: fall through to existing Win32 handler
- [ ] Create `src/compat/linux/linux_syscall.c`:
  - `linux_syscall_dispatch(uint64_t nr, uint64_t rdi, uint64_t rsi, uint64_t rdx, uint64_t r10, uint64_t r8, uint64_t r9)` → `int64_t` return
  - Dispatch table: `linux_syscall_table[512]` of function pointers indexed by Linux syscall number
  - Unknown number → `klog_warn("compat: unhandled linux syscall %llu", nr)` → return `-ENOSYS`
  - Errors translate: Win32 failures → negative Linux errno values (`-ENOENT`, `-EACCES`, `-EINVAL`, etc.)
- [ ] Return value convention: Linux expects negative errno in RAX on failure; positive value on success
- [ ] Commit: `"compat: Linux syscall entry dispatch"`

---

## 4. Linux Syscall Translation Table `[Sonnet]`

Populate `linux_syscall_table[]`. All translations go through the fd table (§1) and path translator (§5).

**File I/O (Linux syscall numbers):**

- [ ] `read(0)` → `fd_to_handle(fd)` → `ReadFile(handle, buf, count, &read, NULL)` → return bytes read or `-errno`
- [ ] `write(1)` → `fd_to_handle(fd)` → `WriteFile(handle, buf, count, &written, NULL)` → return bytes written
- [ ] `open(2, path, flags, mode)` → `linux_path_to_win32(path)` → `CreateFileA` with flags translated (`O_RDONLY`→`GENERIC_READ`, `O_WRONLY`→`GENERIC_WRITE`, `O_RDWR`→both, `O_CREAT`→`OPEN_ALWAYS`, `O_TRUNC`→`CREATE_ALWAYS`, `O_APPEND` → seek to end after open) → `fd_alloc(task, handle)` → return fd
- [ ] `close(3)` → `fd_close(task, fd)`
- [ ] `stat(4, path, buf)` / `lstat(6)` → `GetFileAttributesA` + `GetFileSize` → populate `struct stat` (`st_size`, `st_mode`, `st_mtime` from `GetFileTime`)
- [ ] `fstat(5, fd, buf)` → `fd_to_handle(fd)` → same as `stat`
- [ ] `lseek(8)` → `SetFilePointer(handle, offset, whence_map)`; `whence`: `SEEK_SET=0`, `SEEK_CUR=1`, `SEEK_END=2`
- [ ] `pread64(17)` → `SetFilePointer` + `ReadFile`
- [ ] `pwrite64(18)` → `SetFilePointer` + `WriteFile`
- [ ] `access(21, path, mode)` → `GetFileAttributesA` → existence check for `F_OK`; always allow for `R_OK`/`W_OK`
- [ ] `dup(32)` / `dup2(33)` → `fd_dup2(task, oldfd, newfd)`
- [ ] `rename(82)` → `MoveFileA`
- [ ] `mkdir(83)` → `CreateDirectoryA`
- [ ] `rmdir(84)` → `RemoveDirectoryA`
- [ ] `unlink(87)` → `DeleteFileA`
- [ ] `readlink(89)` → return `-EINVAL` (no symlinks)
- [ ] `getdents64(217)` → `FindFirstFileA`/`FindNextFileA` + format into `linux_dirent64` structs

**Process:**

- [ ] `exit(60)` / `exit_group(231)` → `ExitProcess(code)`
- [ ] `getpid(39)` → return task PID
- [ ] `getppid(110)` → return parent task PID (0 if none)
- [ ] `getuid(102)` / `geteuid(107)` / `getgid(104)` / `getegid(108)` → return 1000 (unprivileged user)
- [ ] `set_tid_address(218)` → store pointer, return TID (no-op for static apps)
- [ ] `arch_prctl(158, code, addr)` → `ARCH_SET_FS (0x1002)` → write `MSR_FS_BASE` (TLS); `ARCH_GET_FS (0x1003)` → read `MSR_FS_BASE`
- [ ] `uname(63, buf)` → fill `struct utsname`: `sysname="Linux"`, `nodename="impossible"`, `release="5.15.0"`, `version="#1"`, `machine="x86_64"` (returns "Linux" to satisfy apps that check kernel identity)
- [ ] `getrusage(98, who, usage)` → `RUSAGE_SELF` fills `struct rusage` from the §8 task accounting fields (→ XREF `02-kernel-core/TODO-21-process-model-extensions.md §8`); `RUSAGE_CHILDREN` zeros until §21 reap lands
- [ ] `times(100, buf)` → fill `struct tms` (`tms_utime`/`tms_stime` in clock ticks) from the same §8 `user_time_ns`/`kernel_time_ns`; return elapsed ticks since boot

**Memory:**

- [ ] `brk(12, addr)` → if `addr == 0`: return `linux_brk_current`; if `addr > linux_brk_current`: `VirtualAlloc` the range → update `linux_brk_current`; return new brk
- [ ] `mmap(9, addr, len, prot, flags, fd, off)` → `MAP_ANON|MAP_PRIVATE` → `VirtualAlloc(addr, len, MEM_COMMIT, PAGE_*)` at `addr` (or any if 0); `MAP_FIXED` → fail if `addr` unavailable; file-backed → `fd_to_handle` + `ReadFile` into allocated region
- [ ] `munmap(11, addr, len)` → `VirtualFree(addr, len, MEM_RELEASE)`
- [ ] `mprotect(10, addr, len, prot)` → `VirtualProtect(addr, len, page_prot_map[prot])`
- [ ] `mremap(25)` → stub `-ENOSYS` (no remap needed for typical static apps)

**Misc:**

- [ ] `ioctl(16, fd, cmd, arg)` → `TIOCGWINSZ` → return `{rows=25, cols=80}`; others → `-ENOTTY`
- [ ] `writev(20, fd, iov, iovcnt)` → iterate `iov[]`; `WriteFile()` each; return total bytes
- [ ] `readv(19)` → iterate `iov[]`; `ReadFile()` each
- [ ] `rt_sigaction(13)` → store handler in `task->linux_sighandlers[sig]` (§8)
- [ ] `rt_sigprocmask(14)` → no-op (no blocking in static single-threaded apps)
- [ ] `clock_gettime(228, clockid, tp)` → `GetTickCount()` → fill `struct timespec {tv_sec, tv_nsec}`
- [ ] `gettimeofday(96)` → same, fill `struct timeval`
- [ ] `nanosleep(35, req, rem)` → `Sleep(req->tv_sec * 1000 + req->tv_nsec / 1000000)`
- [ ] `getrandom(318, buf, buflen, flags)` → fill with CSPRNG bytes from kernel RNG
- [ ] Commit: `"compat: Linux syscall translation table"`

---

## 5. Path Translation `[Sonnet]`

Create `src/compat/linux/linux_path.c`. All file-related Linux syscalls call `linux_path_to_win32()` before touching the VFS.

- [ ] `linux_path_to_win32(src, dst, dstlen)` → translate and write to `dst`:
  - `/` (exact) → `C:\`
  - `/home/<user>/` → `C:\Users\Default\`
  - `/tmp/` → `C:\Temp\`
  - `/bin/`, `/usr/bin/`, `/usr/local/bin/` → `C:\Impossible\Bin\`
  - `/lib/`, `/usr/lib/` → `C:\Impossible\Lib\`
  - `/etc/` → `C:\Impossible\System\Config\`
  - `/proc/self/exe` → current process image path
  - `/proc/self/maps` → synthesize from VMM page table (format: `addr-addr rwxp ...`)
  - `/proc/self/fd/N` → stub → `-ENOENT`
  - `/proc/<pid>/` → `-ENOENT` for other PIDs
  - `/dev/null` → route to a null-device handle (reads return 0 bytes, writes succeed silently)
  - `/dev/zero` → zero-fill reads; discard writes
  - `/dev/urandom` / `/dev/random` → kernel CSPRNG bytes
  - Any other absolute path starting with `/` → prepend `C:\` and convert `/` → `\`
  - Relative paths → prepend CWD from `GetCurrentDirectoryA()` + `\`
- [ ] `win32_path_to_linux(src, dst, dstlen)` → reverse: `C:\Users\Default\` → `/home/default/`; `C:\` → `/`; backslash → slash (for `getcwd()` return values)
- [ ] Commit: `"compat: Linux-to-Win32 path translation"`

---

## 6. Linux File Descriptor Table `[Sonnet]`

> The data structures are defined in §1. This section implements the full fd lifecycle wiring in `src/compat/linux/linux_fd.c`.

- [ ] `linux_fd_init(task)` → zero `linux_fd_table[]`; set `fd_table[0]` = `GetStdHandle(STD_INPUT_HANDLE)`, `[1]` = `STD_OUTPUT_HANDLE`, `[2]` = `STD_ERROR_HANDLE`
- [ ] `linux_fd_alloc(task, handle)` → scan `[3..MAX_FDS-1]` for first free slot; return fd or `-EMFILE` if full
- [ ] `linux_fd_close(task, fd)` → bounds-check; if `fd < 3` skip `CloseHandle` (don't close stdio); clear slot
- [ ] `linux_fd_dup(task, oldfd)` → `DuplicateHandle(current_process, handle, current_process, &new_handle, 0, FALSE, DUPLICATE_SAME_ACCESS)` → `linux_fd_alloc(new_handle)`
- [ ] `linux_fd_dup2(task, oldfd, newfd)` → close `newfd` if open; assign `fd_table[newfd] = fd_table[oldfd]`
- [ ] Commit: `"compat: Linux fd table lifecycle"`

---

## 7. POSIX Filesystem Stubs `[Sonnet]`

Add `opendir`/`readdir`/`closedir`/`getdents64` emulation using `FindFirstFile`/`FindNextFile` under the hood.

- [ ] `struct linux_dir` = `{ HANDLE find_handle; char search_path[MAX_PATH]; WIN32_FIND_DATA find_data; int first; }`
- [ ] `sys_opendir_internal(path)` → `linux_path_to_win32()` + append `\*`; `FindFirstFileA()` → allocate `linux_dir`; return opaque pointer (stored in fd table with a special `LINUX_DIR_FD` marker)
- [ ] `getdents64(217)` → fill `linux_dirent64` structs from `FindNextFileA()`; `d_type`: `DT_DIR` or `DT_REG`; `d_name` from `cFileName`; return bytes filled
- [ ] `getcwd(79, buf, size)` → `GetCurrentDirectoryA(size, win32_buf)` → `win32_path_to_linux(win32_buf, buf)` → return buf pointer in RAX
- [ ] `chdir(80, path)` → `linux_path_to_win32(path)` → `SetCurrentDirectoryA()` → 0 on success
- [ ] `mkdir(83)` / `rmdir(84)` / `unlink(87)` / `rename(82)` already implemented in §4 -- verify VFS backing works for translated paths
- [ ] Commit: `"compat: POSIX opendir/readdir/getcwd/chdir stubs"`

---

## 8. Signal Stubs `[Opus]`

Basic signal delivery for foreground process control. Full POSIX signal semantics are not required -- only the subset needed by static Linux apps (busybox, musl programs).

- [ ] `rt_sigaction(13, sig, act, oldact, sigsetsize)` → if `act != NULL`: copy `sa_handler`, `sa_mask`, `sa_flags` into `task->linux_sighandlers[sig]`; if `oldact != NULL`: copy previous handler out; return 0
- [ ] `rt_sigprocmask(14)` → store `sigset_t` in task; no actual blocking of kernel-level interrupts needed for static single-threaded apps; return 0
- [ ] `SIGINT` delivery (signal 2): keyboard ISR detects Ctrl+C; if foreground task has `is_linux_elf=1` and `linux_sighandlers[SIGINT].handler_fn != SIG_DFL`: inject user-mode signal frame (push return address + signum, redirect RIP to handler); if `SIG_DFL`: terminate process
- [ ] `SIGCHLD` (signal 17): when a child task exits: if parent has `is_linux_elf=1` and registered `SIGCHLD` handler: deliver signal (wake parent if waiting in `waitpid`)
- [ ] `SIGKILL` (signal 9) and `SIGTERM` (signal 15) via `kill(62, pid, sig)` → `TerminateProcess(OpenProcess(pid))` for SIGKILL; deliver via signal frame for SIGTERM
- [ ] `waitpid(61, pid, status, options)` → block on child task exit; populate `*status` with exit code in `WIFEXITED` format
- [ ] Decide how a native kernel exit REASON translates into the Linux status word, which has no lossless slot for one
      - Native `task_waitpid` (reached from ring 3 via `SYS_WAITPID`, `src/kernel/sched/syscall.c`) returns ONE raw `int32_t` shared by application codes, `-(signum)`, and the reserved kernel-reason block at `TASK_EXIT_REASON_BASE` (-1000) in `include/kernel/sched/task.h`. Linux's status word has only two shapes and `WEXITSTATUS` carries just 8 bits, so a value like `TASK_EXIT_UTEST_TIMEOUT` (-1003) has NO faithful target
      - Both obvious translations lose the thing the reserved block exists to preserve: clipping into the 8-bit `WEXITSTATUS` field collides distinct reasons back together (exactly the ambiguity TODO-04 §47 removed), and synthesizing `WIFSIGNALED` + `SIGKILL` discards the cause entirely. Use `TASK_EXIT_IS_KERNEL_REASON()` (same header) to detect the case rather than re-deriving the boundary -- it has been hand-rebuilt wrong twice
      - Decide explicitly and record the stance: a reserved `WEXITSTATUS` value, a synthesized signal plus an out-of-band cause channel, or a documented lossy mapping. A silent clip is the one option that must not ship -> XREF: `00-infrastructure/TODO-04-usermode-test-framework.md` §47 (item: "Moved the timeout marker out of the `-(signum)` range: `UTEST_EXIT_TIMEOUT` (-6) is DELETED and replaced by `TASK_EXIT_UTEST_TIMEOUT`")
- [ ] User-mode signal frame: push `rip`, `rflags`, `rsp`, signal number onto user stack; set `rip = handler_fn`; on `sigreturn(15)` → restore saved frame
- [ ] Commit: `"compat: Linux signal stubs (SIGINT, SIGCHLD, SIGKILL, rt_sigaction)"`

> **Note (foreground-group source):** the Ctrl+C `SIGINT` fan-out is now group-aware -- `02-kernel-core/TODO-21-process-model-extensions.md §17` sets pending SIGINT on every foreground process-group member (`signal_send_group` / `signal_ctrl_c`). This §8 delivery boundary must drain that pending set per member (inject the ring-3 frame or apply the `SIG_DFL` terminate), not just the single leader; there is currently NO `signal_check` call site, so §17's queued SIGINT is not yet acted on until this ships.

---

## 9. Shell ELF Integration + `[Linux]` Process Tag `[Sonnet]`

- [ ] `exec_load()` (TODO-07 §8): on `\x7FELF` magic, call `elf_linux_load()` (§2) and set `task->is_linux_elf = 1`; fall-through to ELF error only for `ENOEXEC` (e.g. dynamic binary)
- [ ] Shell (`cmd.exe`) process list (`tasklist` built-in) → append `[Linux]` tag to processes where `is_linux_elf = 1`
- [ ] Shell recognizes ELF by magic bytes or `.elf` extension; no `.exe` extension required to run ELF binaries
- [ ] Serial log on ELF process start: `klog_info("compat: launching Linux ELF: %s at entry %p", path, entry)`
- [ ] Commit: `"shell: Linux ELF integration + [Linux] process tag"`

---

## 10. Test: Static Hello + Busybox `[Sonnet]`

- [ ] Cross-compile static hello: `x86_64-linux-gnu-gcc -static -o hello_linux hello.c`; place at `C:\` on disk image
- [ ] QEMU: `hello_linux` in shell → prints "Hello, World!" via `sys_write(1, ...)`
- [ ] Cross-compile static busybox: `make defconfig && make LDFLAGS="--static" CROSS_COMPILE=x86_64-linux-gnu-`; place `busybox` at `C:\Impossible\Bin\busybox`
- [ ] `busybox ls /` → lists root-translated directory contents
- [ ] `busybox cat C:\test.txt` (or `/tmp/test.txt`) → prints file contents
- [ ] `busybox echo hello world` → writes to stdout via `sys_write`
- [ ] `busybox grep pattern /tmp/file.txt` → reads file + pattern match
- [ ] `busybox sh` → launches interactive shell (limited: no fork/exec for scripts)
- [ ] `busybox find / -name "*.txt"` → traverses translated path tree
- [ ] Document known limitations in serial log: no dynamic linking, no `/proc` content, `fork`/`exec` stubs return `-ENOSYS`
- [ ] Commit: `"compat: static hello + busybox milestone"`

---

## 11. Future: Dynamic ELF (Stretch) `[Opus]`

> Out of scope for initial implementation. Tracked here for planning. Do not start until §2–8 are complete and busybox runs.

- `PT_INTERP` handling: detect dynamic linker path, load `ld-linux.so` or an Impossible OS equivalent
- Shared library loader: `dlopen()`/`dlclose()`/`dlsym()` backed by PE export directory re-use
- `LD_PRELOAD` environment variable support
- `libc.so.6` emulation shim (musl-based) that redirects to native Impossible OS kernel calls
- `libm.so`, `libpthread.so` stub libraries
- Position-independent executables (`ET_DYN`): load at randomized base, apply `R_X86_64_*` relocations
- Commit: `"compat: dynamic ELF loader + shared library support"`

---

## OS Comparison


| ⭐  | Feature                                          | 🪟 Win11                 | 🐧 Linux      | 🚀 Impossible OS                 |
| --- | ------------------------------------------------ | ------------------------ | ------------- | -------------------------------- |
| 💎  | Run static Linux ELF binaries                    | ✅ WSL2 (full VM)        | ✅ Native     | ⬜ in-kernel compat, no VM       |
| 💎  | 🐧 Linux syscall translation                     | ✅ WSL2 NT layer         | ✅ Native     | ⬜ `linux_syscall_table[]`       |
| 💎  | POSIX path model                                 | ✅ WSL2 VirtIO-FS        | ✅ Native     | ⬜ `linux_path_to_win32()`       |
| 💎  | Integer file descriptor table                    | ✅ WSL2                  | ✅ Native     | ⬜ `linux_fd_table[]` in task    |
| 💎  | Signal delivery                                  | ✅ WSL2                  | ✅ Native     | ⬜ user-mode signal frames       |
| 💎  | `busybox` runs                                   | ✅ WSL2                  | ✅ Native     | ⬜ static busybox milestone      |
| ⭐  | Zero-VM Linux compat                             | ❌ WSL2 requires Hyper-V | ❌ N/A        | ⬜ compat layer in kernel, ~500  |
| ⭐  | PE-native + ELF-compat in same process namespace | ❌ Separate WSL env      | ❌ N/A        | ⬜ both formats in `exec_load()` |
| ⭐  | `[Linux]` tag in process list for ELF processes  | ❌ No tagging            | ❌ No tagging | ⬜ `tasklist` shows format       |
| ❌  | Dynamic ELF / `dlopen` / `libc.so`               | ✅ WSL2                  | ✅ Native     | ⬜ §11 -- Future only            |

**Impossible OS advantage:** Linux ELF compat runs in-kernel with no hypervisor, no separate VHD, and no process namespace boundary -- a static Linux binary simply runs in ring-3 alongside PE binaries using the same scheduler, memory manager, and VFS. This is architecturally lighter than WSL2 and unique among OS designs.

---

## Verification

**§2: ELF Linux load**
- QEMU serial: `elf_linux_load()` logs entry point + `argc`/`argv` for a static ELF binary; process reaches `_start`

**§4: Syscall translation**
- `strace`-equivalent serial output: each translated syscall logs `[Linux] read(0, ...) → 5 bytes`
- Static `hello`: `sys_write(1, "Hello, World!\n", 14)` → console output visible

**§5: Path translation**
- `open("/tmp/test.txt", O_RDWR|O_CREAT)` → translated to `C:\Temp\test.txt` → file appears on IXFS
- `open("/bin/sh", O_RDONLY)` → translated to `C:\Impossible\Bin\sh`, correct HANDLE or `ENOENT`

**§8: Signals**
- Ctrl+C in terminal while Linux ELF is running → `SIGINT` delivered; default handler terminates process
- `kill(pid, SIGKILL)` from another shell process → target ELF process terminates

**§10: Busybox milestone**
- `busybox ls /` → lists `Impossible\`, `Users\`, `Temp\` (translated from `C:\`)
- `busybox echo test | busybox cat` → pipe between commands (or `-ENOSYS` with graceful message)
- `busybox sh` → interactive shell prompt appears; `echo hello` → output
