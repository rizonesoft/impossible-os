# P0110 — Process Model & Syscall Layer

> **Goal:** Complete the kernel's per-process abstractions — file descriptors, working
> directory, user-mode heap, and timer syscalls.
> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB. `kmalloc` is ONLY for small kernel structs (≤ 4 KB). Violating this crashes the 2 MiB heap silently. See `rules.md` Known Gotchas.

---

## 1. File Descriptors & I/O Abstraction

**Prompt:** File descriptors are the POSIX abstraction that unifies files, pipes, sockets, and devices behind integer handles. Each process gets an FD table (array of `TASK_MAX_FDS=256` pointers to `struct file`). `struct file` has a type enum (FILE, PIPE, SOCKET, DEVICE), a VFS node pointer, a read/write offset, reference count, and type-specific callbacks for read/write/close/seek. FDs 0/1/2 are stdin/stdout/stderr (wired to the terminal). `dup`/`dup2` enables I/O redirection (`cmd > file`). `fcntl(fd, F_SETFL, flags)` sets non-blocking mode. This is a prerequisite for pipes and the shell's I/O redirection. Study the existing VFS in `src/kernel/vfs.c` to understand how `vfs_open`/`vfs_read`/`vfs_write` work, then wrap them in the FD layer. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"kernel: file descriptor table"`. Add notes, gotchas, and design decisions directly in this TODO section covering the FD table, struct file, type dispatch, and dup/dup2 semantics.


- [ ] Define `TASK_MAX_FDS 256` — maximum open file descriptors per process
- [ ] Implement per-process FD table in `struct task` (array of `struct file *`)
- [ ] Pre-wire FDs 0/1/2 → stdin/stdout/stderr at task creation
- [ ] `open(path, flags)` → allocate `struct file`, insert into FD table, return fd
- [ ] `read(fd, buf, size)` / `write(fd, buf, size)` — dispatch via `struct file` callbacks
- [ ] `close(fd)` — decrement ref count, free `struct file` when count reaches 0
- [ ] `seek(fd, offset, whence)` — for file-type FDs only
- [ ] `dup(fd)` / `dup2(old, new)` — duplicate FD slot, increment ref count
- [ ] `fcntl(fd, cmd, arg)` — at minimum: `F_GETFL`, `F_SETFL` (non-blocking flag)
- [ ] Add `SYS_OPEN`, `SYS_READ`, `SYS_WRITE`, `SYS_CLOSE`, `SYS_DUP`, `SYS_DUP2`, `SYS_SEEK`, `SYS_FCNTL` syscalls
- [ ] Commit: `"kernel: file descriptor table"`

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

**Prompt:** User-mode programs need their own heap separate from the kernel heap. `brk(addr)` sets the program break (top of data segment), `sbrk(increment)` extends it by N bytes. The kernel maps new pages on demand as the break increases. User-mode `malloc` implementations (dlmalloc, musl's allocator) call `sbrk` internally, so once this works, any standard allocator can be ported. The program break starts at the end of the BSS section (read from the ELF/PE loader). This is needed before any non-trivial user programs can run. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"kernel: brk/sbrk heap for user processes"`. Add notes, gotchas, and design decisions directly in this TODO section covering brk/sbrk, program break, and user-mode malloc integration.


- [ ] Add `brk` (program break pointer) field to `struct task`
- [ ] Init `brk` to end of BSS at exec time (read from ELF/PE loader)
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

## Priority Order

| Priority | Section                       | Reason                                           |
|----------|-------------------------------|--------------------------------------------------|
| 🔴 P0     | 1. File Descriptors           | Foundation for pipes, I/O redirection, sockets   |
| 🔴 P0     | 2. Working Directory          | Shell navigation, relative path VFS resolution   |
| 🔴 P0     | 3. brk/sbrk                   | User-mode malloc doesn't work without this       |
| 🟠 P1     | 4. Timer API                  | vsync, animations, NTP need sleep/gettimeofday   |
