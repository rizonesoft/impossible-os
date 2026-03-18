# P0110 — Process Model & Syscall Layer

> **Goal:** Complete the kernel's per-process abstractions — file descriptors, working
> directory, user-mode heap, and timer syscalls.
> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB. `kmalloc` is ONLY for small kernel structs (≤ 4 KB). Violating this crashes the 2 MiB heap silently. See `rules.md` Known Gotchas.

---

## 1. Handle Table & I/O Abstraction

> [!IMPORTANT]
> **Impossible OS is natively Win32.** The primary I/O abstraction is the Win32
> **HANDLE table** (opaque `void*` handles via `CreateFile`/`ReadFile`/`WriteFile`/
> `CloseHandle`). POSIX-style integer file descriptors (0/1/2) are provided as
> an internal convenience and for the Linux compatibility layer (TODO-540).

**Prompt:** Implement a per-process handle table that unifies files, pipes, sockets, and devices behind opaque `HANDLE` values (matching the Win32 `CreateFile`/`CloseHandle` model). Each process gets a handle table (array of `TASK_MAX_HANDLES=256` pointers to `struct file`). `struct file` has a type enum (FILE, PIPE, SOCKET, DEVICE), a VFS node pointer, a read/write offset, reference count, and type-specific callbacks for read/write/close/seek. Standard handles: `STD_INPUT_HANDLE`, `STD_OUTPUT_HANDLE`, `STD_ERROR_HANDLE` (wired to the terminal). `DuplicateHandle` enables I/O redirection (`cmd > file`). Internally, integer indices map to handles for POSIX compat (`dup`/`dup2`/`fcntl` for Linux compat layer). This is a prerequisite for pipes and the shell's I/O redirection. Study the existing VFS in `src/kernel/vfs.c` to understand how `vfs_open`/`vfs_read`/`vfs_write` work, then wrap them in the handle table. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"kernel: per-process handle table"`. Add notes directly in this TODO section.


- [ ] Define `TASK_MAX_HANDLES 256` — maximum open handles per process
- [ ] Implement per-process handle table in `struct task` (array of `struct file *`)
- [ ] Pre-wire standard handles: `STD_INPUT_HANDLE`, `STD_OUTPUT_HANDLE`, `STD_ERROR_HANDLE` at task creation
- [ ] Win32 API surface: `CreateFile` → allocate handle, `ReadFile`/`WriteFile` → dispatch via callbacks, `CloseHandle` → release
- [ ] `SetFilePointer(handle, offset, method)` — for file-type handles only
- [ ] `DuplicateHandle` — duplicate handle slot, increment ref count
- [ ] POSIX compat (internal + Linux compat layer): map integer FDs 0/1/2 to standard handles
- [ ] POSIX compat: `dup(fd)` / `dup2(old, new)` — for Linux compat layer (TODO-540)
- [ ] POSIX compat: `fcntl(fd, cmd, arg)` — `F_GETFL`, `F_SETFL` for Linux compat layer
- [ ] Add Win32 syscalls: `SYS_CREATEFILE`, `SYS_READFILE`, `SYS_WRITEFILE`, `SYS_CLOSEHANDLE`, `SYS_DUPLICATEHANDLE`
- [ ] Add POSIX compat syscalls: `SYS_OPEN`, `SYS_READ`, `SYS_WRITE`, `SYS_CLOSE`, `SYS_DUP`, `SYS_DUP2` (for Linux compat)
- [ ] Commit: `"kernel: per-process handle table"`

---

## 2. Process Working Directory

**Prompt:** Every process needs a current working directory (CWD) stored as an absolute path string in `struct task`. All relative paths in VFS calls must be resolved against CWD by prepending it. The shell's `cd` command calls `chdir()` which validates the path exists (via VFS) then updates the task's CWD string. `getcwd()` returns the current string. Children inherit the parent's CWD on fork/exec. The shell prompt should display the CWD. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"kernel: per-process working directory"`. Add notes, gotchas, and design decisions directly in this TODO section covering CWD storage, chdir/getcwd, relative path resolution, and child inheritance.


- [ ] Add `cwd[MAX_PATH]` field to `struct task` (default: `C:\`)
- [ ] Implement `chdir(path)` syscall — validate path exists via VFS, update task CWD
- [ ] Implement `getcwd(buf, size)` syscall — copy CWD string to user buffer
- [ ] Resolve relative paths in VFS: prepend `task->cwd` if path does not start with drive letter
- [ ] Inherit CWD from parent on fork/exec (copy string)
- [ ] Update shell prompt to show CWD
- [ ] Add `SYS_CHDIR` and `SYS_GETCWD` syscalls
- [ ] Commit: `"kernel: per-process working directory"`

---

## 3. User-Mode Heap (`brk`/`sbrk`)

**Prompt:** User-mode programs need their own heap separate from the kernel heap. The native Win32 API uses `VirtualAlloc`/`HeapAlloc` (implemented in `TODO-510-Native-Win32.md §6`). For the Linux compatibility layer, `brk(addr)` sets the program break (top of data segment), `sbrk(increment)` extends it by N bytes. The kernel maps new pages on demand as the break increases. User-mode `malloc` implementations (dlmalloc, musl's allocator) call `sbrk` internally. The program break starts at the end of the BSS section (read from the PE/ELF loader). This is needed before any non-trivial user programs can run. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"kernel: brk/sbrk heap for user processes"`. Add notes directly in this TODO section.


- [ ] Add `brk` (program break pointer) field to `struct task`
- [ ] Init `brk` to end of BSS at exec time (read from PE loader, or ELF for Linux compat)
- [ ] Implement `brk(addr)` syscall — validate addr > BSS end, map pages to cover range
- [ ] Implement `sbrk(increment)` syscall — return old break, advance break by `increment` bytes, map new pages
- [ ] Wire user-mode `malloc`/`free` to `sbrk`-backed dlmalloc or musl allocator
- [ ] Add `SYS_BRK` syscall
- [ ] Test: user program allocates > 1 page via malloc, write/read verifies heap works
- [ ] Commit: `"kernel: brk/sbrk heap for user processes"`

---

## 4. Timer API for User Programs

**Prompt:** User programs need `sleep(seconds)` and `usleep(microseconds)` for delays, and `gettimeofday()` for wall-clock time. `sleep` adds the calling thread to a timer queue sorted by wake time, then yields — the PIT IRQ handler checks the queue each tick and wakes expired entries. `gettimeofday` combines the RTC (for calendar time) with the PIT tick counter (for sub-second precision). These syscalls are needed by: compositor vsync timing, animation loops in GUI apps, and the NTP client. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"kernel: user-mode timer syscalls"`. Add notes, gotchas, and design decisions directly in this TODO section covering sleep/usleep implementation, timer queue, and gettimeofday precision.


- [ ] Implement timer queue in scheduler: sorted linked list of `(task, wake_tick)` pairs
- [ ] Hook timer queue check into PIT IRQ handler — O(1) head check per tick
- [ ] Implement `sleep(seconds)` syscall — insert into timer queue, yield
- [ ] Implement `usleep(microseconds)` syscall — same mechanism, finer granularity
- [ ] Implement `gettimeofday(tv)` syscall — `tv.tv_sec` from RTC, `tv.tv_usec` from PIT tick counter
- [ ] Add `SYS_SLEEP`, `SYS_USLEEP`, `SYS_GETTIMEOFDAY` syscalls
- [ ] Test: `sleep(1)` — verify ~1 second delay measured by tick counter
- [ ] Commit: `"kernel: user-mode timer syscalls"`

---

## 5. Process Priority & Scheduling Policy

**Prompt:** The current scheduler uses a round-robin policy with no priority differentiation. Processes should be assignable to priority classes: `REALTIME > HIGH > NORMAL > IDLE`. Windows uses `SetPriorityClass`; Linux uses `nice()`/`setpriority()`. Add a `priority` field to `struct task`, a `sched_policy` field (`SCHED_RR`, `SCHED_FIFO`, `SCHED_IDLE`), and update the scheduler to select the highest-priority runnable task first. `SYS_SETPRIORITY` allows user-mode priority changes (with privilege check). The compositor and audio mixer should run at `HIGH`; background tasks at `IDLE`. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"sched: process priority classes"`. Add notes directly in this TODO section.


- [ ] Add `priority` (0–3: IDLE/NORMAL/HIGH/REALTIME) and `sched_policy` fields to `struct task`
- [ ] Update scheduler: always pick highest-priority runnable task; round-robin within same priority
- [ ] `SCHED_FIFO`: run until block or yield (no time-slicing) — for realtime tasks
- [ ] `SCHED_IDLE`: only run when no other tasks are runnable
- [ ] Add `SYS_SETPRIORITY(pid, priority)` syscall — kernel tasks require privilege
- [ ] Compositor and audio mixer: spawn at `PRIORITY_HIGH`
- [ ] Background fetch/index tasks: spawn at `PRIORITY_IDLE`
- [ ] Commit: `"sched: process priority classes"`

---

## 6. Process Capabilities & Privilege Separation

**Prompt:** Currently the kernel treats all processes equally — any process can do anything. A privilege model is needed for security: some operations (raw disk I/O, loading drivers, changing system time) should require elevated privilege. Windows uses tokens + privileges (`SeShutdownPrivilege`, etc.); Linux uses capabilities (`CAP_SYS_ADMIN`, `CAP_NET_ADMIN`). Add a `capabilities` bitmask to `struct task`. System processes (init, compositor) get full capabilities; user processes get a restricted set. `capability_check(cap)` in the kernel validates before privileged operations. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"kernel: process capabilities"`. Add notes directly in this TODO section.


- [ ] Define capability bitmask constants: `CAP_SYS_ADMIN`, `CAP_RAW_IO`, `CAP_NET_ADMIN`, `CAP_LOAD_DRIVER`, `CAP_KILL_ALL`, `CAP_SET_TIME`
- [ ] Add `capabilities` field to `struct task` (uint64_t bitmask)
- [ ] System processes: granted full capability set at spawn
- [ ] User processes: granted restricted set (no raw I/O, no driver load)
- [ ] Add `capability_check(cap)` — returns 0 if current task has cap, -EPERM if not
- [ ] Gate privileged syscalls behind capability_check
- [ ] `SYS_DROPCAP(cap)` — reduce own capabilities (principle of least privilege)
- [ ] Commit: `"kernel: process capabilities"`

---

## Priority Order

| Priority | Section                          | Reason                                                  |
|----------|----------------------------------|---------------------------------------------------------|
| 🔴 P0    | 1. Handle Table                  | Foundation for pipes, I/O redirection, sockets (Win32 HANDLE model) |
| 🔴 P0    | 2. Working Directory             | Shell navigation, relative path VFS resolution           |
| 🔴 P0    | 3. brk/sbrk                      | User-mode malloc doesn't work without this              |
| 🟠 P1    | 4. Timer API                     | vsync, animations, NTP need sleep/gettimeofday          |
| 🟠 P1    | 5. Process Priority              | Compositor/audio needs high priority; background idle   |
| 🟡 P2    | 6. Process Capabilities          | Security hardening — privilege separation               |

---

## OS Comparison

| Feature                         | 🪟 Windows 11                | 🐧 Linux Kernel           | 🚀 Impossible OS                           |
| ------------------------------- | --------------------------- | ------------------------ | ----------------------------------------- |
| Handle table (per-process)      | ✅ Handle table (HANDLE)     | ✅ POSIX FD table         | ⬜ §1 P0 — **Win32 HANDLE model (native)** |
| Per-process working directory   | ✅ `SetCurrentDirectory`     | ✅ `chdir(2)`             | ⬜ §2 P0                                   |
| User-mode heap (brk/sbrk)       | ✅ `VirtualAlloc` / heap     | ✅ `brk(2)` / `sbrk(2)`   | ⬜ §3 P0                                   |
| Timer syscalls (sleep/time)     | ✅ `Sleep` / `GetSystemTime` | ✅ `sleep`/`gettimeofday` | ⬜ §4 P1                                   |
| Process priority classes        | ✅ `SetPriorityClass`        | ✅ `nice`/`setpriority`   | ⬜ §5 P1                                   |
| Process capabilities            | ✅ Privileges + tokens       | ✅ `CAP_*` capabilities   | ⬜ §6 P2                                   |
| **Unified handles for all I/O** | ✅ HANDLE for everything     | ✅ Everything is an FD    | ⬜ **§1 — Win32 HANDLE model (native)**    |
