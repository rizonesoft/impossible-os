# P1503 — Linux ELF Compatibility Layer

> **Goal:** Run simple Linux ELF binaries on Impossible OS via a POSIX/Linux
> syscall translation layer. This is a **compatibility layer** — PE/Win32 is the
> native and default binary format (see `TODO-510-Native-Win32.md`). ELF
> support allows running statically-linked Linux CLI tools (busybox, coreutils)
> and opens the door to porting Linux software.

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB (fonts, images, file data). `kmalloc` is ONLY for small kernel structs (≤ 4 KB). Violating this crashes the 2 MiB heap silently. See `rules.md` Known Gotchas and `/add-asset` workflow.

> [!IMPORTANT]
> **PE is native.** Impossible OS is a Win32-native OS with Windows 11
> compatible API. This Linux compatibility layer is secondary — it translates
> Linux syscalls into native Win32/kernel calls. Think of it like WSL in
> reverse: Windows is native, Linux runs on top.

**Related TODOs:**
- **P0105** — Native Win32 PE programs & SDK (the primary binary format)
- **P0103 §3.6** — Native file I/O API (CreateFile, ReadFile, etc.)

---

## 1. ELF Loader ✅ (Partial)

> **Status:** Basic `elf_load()` exists in `src/kernel/elf.c`.

**Prompt:** The ELF loader already parses ELF64 headers and loads segments. Verify it handles: ELF magic validation, ET_EXEC type check, EM_X86_64 machine check, PT_LOAD segment mapping, entry point extraction. Extend if needed: PT_INTERP for dynamic linking (future), proper BSS zero-fill, alignment. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: ELF loader enhancements"`.


- [x] Parse ELF64 header: magic, class (64-bit), machine (x86-64)
- [x] Map PT_LOAD segments into process address space
- [x] Extract entry point address
- [ ] Verify BSS zero-fill (VirtualSize > FileSize segments)
- [ ] Handle non-page-aligned segments
- [ ] Reject ELF32, non-x86_64, or shared objects (ET_DYN) with clear error
- [ ] Commit: `"kernel: ELF loader enhancements"`

---

## 2. Binary Format Detection

> **Depends on:** §1 (ELF loader), P0105 §3.4 (PE `load_binary()`)

**Prompt:** Extend `load_binary(data, size)` from P0105 to detect both formats: `"MZ"` → `pe_load()` (native), `"\x7fELF"` → `elf_load()` (compat layer). PE is always tried first. ELF binaries automatically get the Linux syscall handler attached. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: dual PE+ELF binary detection"`.


- [ ] Extend `load_binary()` in `src/kernel/task.c`:
  - [ ] `"MZ"` (0x5A4D) → `pe_load()` — native PE (checked first)
  - [ ] `"\x7fELF"` → `elf_load()` — Linux compatibility
  - [ ] Unknown → return error
- [ ] ELF processes get Linux syscall translation layer (§3) attached
- [ ] Commit: `"kernel: dual PE+ELF binary detection"`

---

## 3. Linux Syscall Translation Layer

> **Depends on:** §1 (ELF loader), ring 3 user-mode (P0105 §1)

**Prompt:** Linux x86-64 uses `syscall` with number in RAX, args in RDI, RSI, RDX, R10, R8, R9 (System V convention). Implement a syscall handler that translates Linux syscall numbers into native Impossible OS kernel calls. Start with the ~20 most essential syscalls for a static busybox binary. The translation layer converts POSIX paths (`/home/user/file`) to Windows paths (`C:\Users\Default\file`) and Linux file descriptors to native HANDLEs. After completing all items, create `docs/architecture/linux-compat.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: Linux syscall translation layer"`.


- [ ] Create `src/compat/linux/linux_syscall.c`
- [ ] Detect ELF process → route `syscall` to Linux handler (not Win32 handler)
- [ ] Linux x86-64 calling convention: RAX=number, RDI, RSI, RDX, R10, R8, R9

### 3.1 File I/O Syscalls

- [ ] `sys_read(fd, buf, count)` → translate fd to HANDLE → `ReadFile()`
- [ ] `sys_write(fd, buf, count)` → translate fd to HANDLE → `WriteFile()`
- [ ] `sys_open(path, flags, mode)` → translate path → `CreateFile()`
- [ ] `sys_close(fd)` → `CloseHandle()`
- [ ] `sys_lseek(fd, offset, whence)` → `SetFilePointer()`
- [ ] `sys_stat(path, buf)` / `sys_fstat(fd, buf)` → `GetFileAttributes()` + `GetFileSize()`
- [ ] `sys_access(path, mode)` → `GetFileAttributes()`
- [ ] `sys_unlink(path)` → `DeleteFile()`
- [ ] `sys_rename(old, new)` → `MoveFile()`
- [ ] `sys_mkdir(path, mode)` → `CreateDirectory()`
- [ ] `sys_rmdir(path)` → `RemoveDirectory()`
- [ ] `sys_getcwd(buf, size)` → `GetCurrentDirectory()`
- [ ] `sys_chdir(path)` → `SetCurrentDirectory()`

### 3.2 Process Syscalls

- [ ] `sys_exit(code)` → `ExitProcess()`
- [ ] `sys_exit_group(code)` → `ExitProcess()`
- [ ] `sys_getpid()` → return process ID
- [ ] `sys_getppid()` → return parent PID
- [ ] *(Stretch)* `sys_fork()` → `CreateProcess()` (complex — may stub initially)
- [ ] *(Stretch)* `sys_execve(path, argv, envp)` → `CreateProcess()`
- [ ] *(Stretch)* `sys_wait4(pid, ...)` → wait for child

### 3.3 Memory Syscalls

- [ ] `sys_brk(addr)` → adjust heap via `VirtualAlloc()`
- [ ] `sys_mmap(addr, len, prot, flags, fd, off)` → `VirtualAlloc()`
- [ ] `sys_munmap(addr, len)` → `VirtualFree()`

### 3.4 Misc Syscalls

- [ ] `sys_ioctl(fd, cmd, arg)` → stub (return -ENOTTY for most)
- [ ] `sys_writev(fd, iov, iovcnt)` → scatter-gather write
- [ ] `sys_uname(buf)` → return "Impossible" as sysname, "1.0" as release
- [ ] `sys_getuid()` / `sys_geteuid()` → return 0 (root)
- [ ] `sys_clock_gettime()` → PIT-based time

- [ ] Commit: `"kernel: Linux syscall translation layer"`

---

## 4. Path Translation

> **Depends on:** §3 (syscall layer)

**Prompt:** Linux uses forward-slash paths rooted at `/`. Translate: `/` → `C:\`, `/home/<user>` → `C:\Users\Default`, `/tmp` → `C:\Impossible\Temp`, `/dev/null` → NUL, `/proc/self/...` → stub. Handle relative paths using the process CWD. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"compat: Linux path translation"`.


- [ ] Create `src/compat/linux/linux_path.c`
- [ ] `/` → `C:\`
- [ ] `/home/<user>` → `C:\Users\Default`
- [ ] `/tmp` → `C:\Impossible\Temp`
- [ ] `/dev/null` → NUL device
- [ ] `/dev/zero` → zero-fill reads, discard writes
- [ ] `/proc/self/exe` → return ELF path
- [ ] `/proc/self/fd/N` → stub
- [ ] Relative paths use process CWD
- [ ] Convert `/forward/slashes` → `C:\back\slashes`
- [ ] Commit: `"compat: Linux path translation"`

---

## 5. File Descriptor Table

> **Depends on:** §3 (syscall layer)

**Prompt:** Linux uses integer file descriptors (0=stdin, 1=stdout, 2=stderr). Maintain a per-process fd→HANDLE mapping table. `fd_alloc()` returns the lowest available fd. `fd_to_handle(fd)` returns the native HANDLE. Pre-populate fd 0/1/2 with stdin/stdout/stderr HANDLEs. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"compat: Linux file descriptor table"`.


- [ ] Create `src/compat/linux/linux_fd.c`
- [ ] Per-process fd table: `HANDLE fd_table[MAX_FDS]` (MAX_FDS = 256)
- [ ] Pre-populate: fd 0 = stdin, fd 1 = stdout, fd 2 = stderr
- [ ] `fd_alloc(handle)` → return lowest available fd
- [ ] `fd_to_handle(fd)` → return native HANDLE
- [ ] `fd_close(fd)` → close HANDLE + free slot
- [ ] `dup2(oldfd, newfd)` → duplicate fd mapping
- [ ] Commit: `"compat: Linux file descriptor table"`

---

## 6. ELF Shell Integration

- [ ] Shell recognizes ELF binaries by `\x7fELF` magic (in addition to `MZ`)
- [ ] File extension `.elf` or no extension → try ELF loader
- [ ] Display `[Linux]` tag in process list for ELF processes
- [ ] Commit: `"shell: Linux ELF binary support"`

---

## 7. Test: Run Static Linux Binary

**Prompt:** Cross-compile a static Linux x86-64 "Hello World" (`gcc -static -o hello hello.c`), include on C:\. Execute in shell — should print "Hello, World!" via `sys_write(1, ...)`. This proves ELF + Linux syscall translation works. Next milestone: run a static busybox binary. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"compat: run first Linux ELF binary"`.


- [ ] Cross-compile: `x86_64-linux-gnu-gcc -static -o hello hello.c`
- [ ] Include on C:\
- [ ] Execute: `hello` in shell → prints "Hello, World!" via sys_write
- [ ] Commit: `"compat: run first Linux ELF binary"`

---

## 8. Busybox (Stretch)

> **Depends on:** §3–5 (syscall layer, path translation, fd table)

- [ ] *(Stretch)* Compile static busybox for x86-64
- [ ] *(Stretch)* `busybox ls` → directory listing via `sys_getdents`
- [ ] *(Stretch)* `busybox cat file` → read + print file
- [ ] *(Stretch)* `busybox echo hello` → write to stdout
- [ ] *(Stretch)* Add missing syscalls as busybox reveals them

---

## 9. Documentation

- [ ] Create `docs/architecture/linux-compat.md`
- [ ] Document: ELF loader, syscall translation, path mapping, fd table
- [ ] Document supported syscalls with native mapping table
- [ ] Document limitations (no dynamic linking, no signals, no ptrace)
- [ ] Commit: `"docs: Linux ELF compatibility layer"`

---

## Priority Order

| Priority | Section | Reason |
|----------|---------|--------|
| 🔴 P0 | §1 ELF Loader (verify) | Already partial — verify and extend |
| 🔴 P0 | §2 Binary Format Detection | PE + ELF dual support |
| 🔴 P0 | §3.1–3.2 Core Syscalls (I/O + process) | Minimum viable syscall set |
| 🟠 P1 | §4 Path Translation | `/` → `C:\` mapping |
| 🟠 P1 | §5 File Descriptor Table | Linux fd → native HANDLE |
| 🟠 P1 | §7 Test: Static Hello World | Prove it works |
| 🟡 P2 | §3.3–3.4 Memory + Misc Syscalls | Broader program support |
| 🟡 P2 | §6 Shell Integration | UX polish |
| 🟢 P3 | §8 Busybox | Real-world Linux binary |
| 🔵 P4 | §9 Documentation | Reference docs |

---

## Key Files

| File | Purpose |
|------|---------|
| `src/kernel/elf.c` | [EXISTS] ELF64 loader |
| `src/kernel/sched/task.c` | [MODIFY] Binary format detection |
| `src/compat/linux/linux_syscall.c` | [NEW] Linux syscall handler |
| `src/compat/linux/linux_path.c` | [NEW] POSIX → Win32 path translation |
| `src/compat/linux/linux_fd.c` | [NEW] File descriptor table |
| `docs/architecture/linux-compat.md` | [NEW] Compatibility layer docs |

---

## Effort Estimates

| Component | Effort | Dependencies |
|-----------|--------|-------------|
| ELF loader verify | Days | Already exists |
| Binary detection | Days | PE loader (P0105) |
| ~20 core syscalls | Weeks | Ring 3, native file API |
| Path translation | Days | VFS paths |
| FD table | Days | Native HANDLE system |
| Static hello world | Days | Syscalls working |
| Busybox | Weeks | Broad syscall coverage |
| **Total (core)** | **~2–3 weeks** | |
| **Total (busybox)** | **~4–6 weeks** | |
