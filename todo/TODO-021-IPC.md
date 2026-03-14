# P0103 — Inter-Process Communication (IPC)

> **Goal:** Pipes, signals, and shared memory — the building blocks for process
> communication, shell piping, and Ctrl+C handling.
> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB. `kmalloc` is ONLY for small kernel structs (≤ 4 KB). Violating this crashes the 2 MiB heap silently. See `rules.md` Known Gotchas.

---

## 1. Pipes ✅

**Prompt:** This section is marked complete. Verify the implementation is correct: review `pipe_t` struct (4 KiB ring buffer, read/write positions, mutex, semaphores), confirm `pipe_create`, `pipe_write`, `pipe_read`, `pipe_close` all work, and that `SYS_PIPE` syscall (number 33) is registered. Check that SIGPIPE is sent when writing to a closed pipe. Run `bash scripts/build.sh clean` and test shell piping (e.g., `ls | grep`). Fix any inconsistencies in the TODO items below. After verifying, mark all items as `[x]` and update this prompt to reflect the final verified state for future correctness checks. Update `README.md` if it contains stale or incorrect references to IPC or pipes. Create or update documentation in `docs/` covering the pipe implementation, syscalls, and SIGPIPE behavior.


- [x] Define `pipe_t` struct (4 KiB ring buffer, read/write positions, mutex, semaphores)
- [x] Implement `pipe_create(fds[2])` — allocate pipe, return read/write file descriptors
- [x] Implement `pipe_write(pipe, data, len)` — write bytes, block if full
- [x] Implement `pipe_read(pipe, buf, len)` — read bytes, block if empty
- [x] Implement `pipe_close(pipe, end)` — close one end, SIGPIPE if writing to closed pipe
- [x] Add `SYS_PIPE` syscall (number 33)
- [x] Test: shell commands piping output (e.g., `ls | grep`)
- [x] Commit: `"ipc: pipe implementation"`

---

## 2. Signals ✅

**Prompt:** This section is marked complete. Verify the implementation is correct: confirm signal constants (SIGKILL=9, SIGTERM=15, SIGINT=2, SIGCHLD=17, SIGPIPE=13), `signal_send`, `signal_handler` functions, and `SYS_SIGNAL` syscall (34) all exist. Verify SIGINT is wired from Ctrl+C in the terminal driver, SIGCHLD fires on child exit, SIGPIPE fires on write to closed pipe, and SIGKILL always terminates. Run `bash scripts/build.sh clean` and test Ctrl+C. Fix any inconsistencies in the TODO items below. After verifying, mark all items as `[x]`. Update `README.md` and `docs/` covering signal constants, delivery semantics, and handler registration.


- [x] Define signal constants: `SIGINT(2)`, `SIGKILL(9)`, `SIGPIPE(13)`, `SIGTERM(15)`, `SIGCHLD(17)`
- [x] Implement `signal_send(pid, sig)` — deliver signal to process
- [x] Implement `signal_handler(sig, handler)` — register user-mode handler
- [x] Handle `SIGINT` from Ctrl+C in terminal
- [x] Handle `SIGCHLD` when child process exits
- [x] Handle `SIGPIPE` when writing to closed pipe end
- [x] Implement default handlers (SIGKILL always kills, SIGTERM clean exit)
- [x] Add `SYS_SIGNAL` syscall (number 34)
- [x] Commit: `"ipc: signal delivery"`

---

## 3. Shared Memory ✅

**Prompt:** This section is marked complete. Verify the implementation is correct: confirm `shmem_create`, `shmem_map`, `shmem_unmap` exist with reference counting, and that `SYS_SHMEM_CREATE` (35), `SYS_SHMEM_MAP` (36), and `SYS_SHMEM_UNMAP` (37) syscalls are registered. Run `bash scripts/build.sh clean` and test two processes sharing a counter. Fix any inconsistencies in the TODO items below. After verifying, mark all items as `[x]`. Update `README.md` and `docs/` covering named shared memory regions, syscalls, and reference counting.

> **Note:** Verify syscall numbers — `SYS_SHMEM_UNMAP` may conflict with `SYS_MMAP` (37) in `TODO-022-Virtual-Memory.md`. Check `syscall.h` and resolve any collision.

- [x] Implement `shmem_create(name, size)` — allocate named shared memory region
- [x] Implement `shmem_map(id)` — map shared region into calling process's address space
- [x] Implement `shmem_unmap(id)` — unmap from process
- [x] Reference counting — free physical pages when last process unmaps
- [x] Add `SYS_SHMEM_CREATE` (35), `SYS_SHMEM_MAP` (36), `SYS_SHMEM_UNMAP` (37) syscalls
- [x] Test: two processes sharing a counter via shared memory
- [x] Commit: `"ipc: named shared memory"`

---

## Priority Order

| Priority | Section          | Reason                                            |
|----------|------------------|---------------------------------------------------|
| ✅ Done   | 1. Pipes         | Verified complete — shell piping works            |
| ✅ Done   | 2. Signals       | Verified complete — Ctrl+C, SIGPIPE work          |
| ✅ Done   | 3. Shared Memory | Verified complete — verify `SYS_SHMEM_UNMAP` num |
