# P0103 — Inter-Process Communication (IPC)

> **Goal:** Pipes, signals, shared memory, and per-thread signal masks — the
> building blocks for process communication, shell piping, Ctrl+C handling,
> and POSIX-compliant multithreaded signal delivery.
>
> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB. `kmalloc` is ONLY for small kernel structs (≤ 4 KB). Violating this crashes the 2 MiB heap silently. See `rules.md` Known Gotchas.

---

## 1. Pipes ✅

<details>
<summary>✅ 1. Pipes — completed</summary>


**Prompt:** This section is marked complete. Verify the implementation is correct: review `pipe_t` struct (4 KiB ring buffer, read/write positions, mutex, semaphores), confirm `pipe_create`, `pipe_write`, `pipe_read`, `pipe_close` all work, and that `SYS_PIPE` syscall (number 33) is registered. Check that SIGPIPE is sent when writing to a closed pipe. Run `bash scripts/build.sh clean` and test shell piping (e.g., `ls | grep`). Fix any inconsistencies in the TODO items below. After verifying, mark all items as `[x]` and update this prompt to reflect the final verified state for future correctness checks. Add notes, gotchas, and design decisions directly in this TODO section covering the pipe implementation, syscalls, and SIGPIPE behavior.


- [x] Define `pipe_t` struct (4 KiB ring buffer, read/write positions, mutex, semaphores)
- [x] Implement `pipe_create(fds[2])` — allocate pipe, return read/write file descriptors
- [x] Implement `pipe_write(pipe, data, len)` — write bytes, block if full
- [x] Implement `pipe_read(pipe, buf, len)` — read bytes, block if empty
- [x] Implement `pipe_close(pipe, end)` — close one end, SIGPIPE if writing to closed pipe
- [x] Add `SYS_PIPE` syscall (number 33)
- [x] Test: shell commands piping output (e.g., `ls | grep`)
- [x] Commit: `"ipc: pipe implementation"`


</details>

---
## 2. Signals ✅

<details>
<summary>✅ 2. Signals — completed</summary>


**Prompt:** This section is marked complete. Verify the implementation is correct: confirm signal constants (SIGKILL=9, SIGTERM=15, SIGINT=2, SIGCHLD=17, SIGPIPE=13), `signal_send`, `signal_handler` functions, and `SYS_SIGNAL` syscall (34) all exist. Verify SIGINT is wired from Ctrl+C in the terminal driver, SIGCHLD fires on child exit, SIGPIPE fires on write to closed pipe, and SIGKILL always terminates. Run `bash scripts/build.sh clean` and test Ctrl+C. Fix any inconsistencies in the TODO items below. After verifying, mark all items as `[x]`. Add notes directly in this TODO section covering signal constants, delivery semantics, and handler registration.


- [x] Define signal constants: `SIGINT(2)`, `SIGKILL(9)`, `SIGPIPE(13)`, `SIGTERM(15)`, `SIGCHLD(17)`
- [x] Implement `signal_send(pid, sig)` — deliver signal to process
- [x] Implement `signal_handler(sig, handler)` — register user-mode handler
- [x] Handle `SIGINT` from Ctrl+C in terminal
- [x] Handle `SIGCHLD` when child process exits
- [x] Handle `SIGPIPE` when writing to closed pipe end
- [x] Implement default handlers (SIGKILL always kills, SIGTERM clean exit)
- [x] Add `SYS_SIGNAL` syscall (number 34)
- [x] Commit: `"ipc: signal delivery"`


</details>

---
## 3. Shared Memory ✅

<details>
<summary>✅ 3. Shared Memory — completed</summary>


**Prompt:** This section is marked complete. Verify the implementation is correct: confirm `shmem_create`, `shmem_map`, `shmem_unmap` exist with reference counting, and that `SYS_SHMEM_CREATE` (35), `SYS_SHMEM_MAP` (36), and `SYS_SHMEM_UNMAP` (37) syscalls are registered. Run `bash scripts/build.sh clean` and test two processes sharing a counter. Fix any inconsistencies in the TODO items below. After verifying, mark all items as `[x]`. Add notes directly in this TODO section covering named shared memory regions, syscalls, and reference counting.

> **Note:** Verify syscall numbers — `SYS_SHMEM_UNMAP` may conflict with `SYS_MMAP` (37) in `TODO-023-Virtual-Memory.md`. Check `syscall.h` and resolve any collision.

- [x] Implement `shmem_create(name, size)` — allocate named shared memory region
- [x] Implement `shmem_map(id)` — map shared region into calling process's address space
- [x] Implement `shmem_unmap(id)` — unmap from process
- [x] Reference counting — free physical pages when last process unmaps
- [x] Add `SYS_SHMEM_CREATE` (35), `SYS_SHMEM_MAP` (36), `SYS_SHMEM_UNMAP` (37) syscalls
- [x] Test: two processes sharing a counter via shared memory
- [x] Commit: `"ipc: named shared memory"`


</details>

---
## 4. Message Queues

**Prompt:** Message queues are a structured IPC mechanism: typed messages are placed into a kernel-managed FIFO queue, retrieved by type or in order, without a persistent open channel (unlike pipes). Windows uses `PostMessage`/`SendMessage` (window messages) and MSMQ. Linux has POSIX `mq_open`/`mq_send`/`mq_receive`. Impossible OS needs message queues for: the compositor event system (input events from drivers → window manager), the shell's async notification system, and driver-to-kernel communication. A message queue is a ring of fixed-size `mq_msg_t` entries (`type` + `data[MQ_MAX_DATA]`, typically 64 bytes). After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"ipc: message queues"`. Add notes directly in this TODO section covering the message queue API and compositor usage.


- [ ] Define `mq_msg_t` struct: `{ uint32_t type; uint8_t data[60]; }` (64 bytes per message)
- [ ] Define `msgqueue_t` struct: ring buffer of `mq_msg_t`, capacity, read/write pos, mutex, semaphore
- [ ] Implement `mq_create(name, capacity)` — create named message queue
- [ ] Implement `mq_send(mq, type, data, len)` — enqueue message, block if full
- [ ] Implement `mq_recv(mq, out_msg)` — dequeue oldest message, block if empty
- [ ] Implement `mq_tryrecv(mq, out_msg)` — non-blocking recv, returns 0 if empty
- [ ] Implement `mq_destroy(mq)` — free queue and wake all blocked senders/receivers
- [ ] Add `SYS_MQ_CREATE`, `SYS_MQ_SEND`, `SYS_MQ_RECV`, `SYS_MQ_DESTROY` syscalls
- [ ] Use for: compositor input event delivery (keyboard/mouse → WM)
- [ ] Commit: `"ipc: message queues"`

---

## 5. Unix-Domain Sockets (Local IPC)

**Prompt:** Unix-domain sockets (`AF_UNIX`) provide a socket-like API for local IPC — bidirectional, stream-oriented, with no network overhead. Both Windows (named pipes in `\\.\pipe\`) and Linux (`AF_UNIX`) have this. Impossible OS needs local sockets for: IxUI widget toolkit ↔ compositor protocol, future Wayland-style display server, and inter-process RPC without networking. Implement as a pair of in-kernel ring buffers with `connect`/`accept`/`send`/`recv` semantics. The socket is identified by a VFS path (e.g., `/tmp/compositor.sock`). After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"ipc: unix-domain sockets"`. Add notes directly in this TODO section.

> **Prerequisite:** File Descriptors (§1 of TODO-028) must exist — sockets are FD-backed.


- [ ] Define `unix_socket_t` struct: two ring buffers (A→B, B→A), mutex, backlog queue
- [ ] Implement `socket(AF_UNIX, SOCK_STREAM, 0)` — create socket FD
- [ ] Implement `bind(fd, path)` — register socket at VFS path
- [ ] Implement `listen(fd, backlog)` — mark socket as server
- [ ] Implement `accept(fd)` — return new client FD from backlog queue
- [ ] Implement `connect(fd, path)` — connect to server socket at VFS path
- [ ] Implement `send(fd, buf, len)` / `recv(fd, buf, len)` — bidirectional I/O via ring buffers
- [ ] Add `SYS_SOCKET`, `SYS_BIND`, `SYS_LISTEN`, `SYS_ACCEPT`, `SYS_CONNECT` syscalls
- [ ] Use for: IxUI ↔ compositor protocol
- [ ] Commit: `"ipc: unix-domain sockets"`

---

## 6. Process Events (waitpid / exit status)

**Prompt:** `waitpid(pid, &status)` blocks the calling process until the target child exits and retrieves its exit code. Without this, parent processes cannot determine if a child succeeded or failed — the shell cannot report exit codes, and process cleanup leaks zombie entries. Linux uses `waitpid()`; Windows uses `WaitForSingleObject` on a process handle. Impossible OS already delivers `SIGCHLD` but has no way to retrieve the exit status. `exit(code)` must store the code in the task struct before terminating; `waitpid` reads it. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"kernel: waitpid and exit status"`. Add notes directly in this TODO section.


- [ ] Add `exit_code` field to `struct task`
- [ ] `exit(code)` stores `exit_code`, sets task state to `TASK_ZOMBIE` (don't free yet)
- [ ] Implement `waitpid(pid, &status)` — block until child task is ZOMBIE, read exit_code, free child
- [ ] `SIGCHLD` is still delivered to parent on child exit (already done §2) — waitpid clears the zombie
- [ ] Add `SYS_WAITPID` and `SYS_EXIT` syscalls
- [ ] Shell: print `"Process exited with code N"` using waitpid on background jobs
- [ ] Commit: `"kernel: waitpid and exit status"`

---

## 7. Per-Thread Signal Masks (`pthread_sigmask`)

**Prompt:** The current signal implementation (§2) delivers signals to the
process as a whole — there is no notion of which thread receives a signal.
POSIX requires per-thread signal masks: each thread independently blocks or
unblocks signals using `pthread_sigmask(SIG_BLOCK / SIG_UNBLOCK / SIG_SETMASK,
&sigset, &oldset)`. When a signal is delivered, the kernel picks the first
thread that does not have that signal blocked. `SIGKILL` and `SIGSTOP` can
never be blocked. This is required for correct multithreaded programs: without
it, any thread may receive (and mishandle) a signal intended for a specific
thread — a common source of crashes in multi-threaded servers.

Implement by adding a `signal_mask` bitmask (`uint64_t`) to `thread_t`.
On signal delivery in `signal_send()`, iterate threads in the target process
and dispatch to the first thread where `!(signal_mask & (1 << sig))`.

> **Cross-reference:** Thread-Local Storage (§22 of TODO-020) stores the
> current thread's `signal_mask` via the `FS`-base TLS block, making it
> accessible from user-space as `pthread_sigmask()` without a syscall.

> **Prerequisite:** §2 Signals must be complete (✅ Done) and §22 TLS
> (TODO-020) should be done first for clean user-space access.

- [ ] Add `signal_mask` (`uint64_t` bitmask) field to `thread_t` (0 = all signals unblocked)
- [ ] `SIGKILL (9)` and `SIGSTOP (19)` bits are ignored in mask — always deliverable
- [ ] Update `signal_send(pid, sig)` to find first unblocked thread in process
- [ ] Implement `SYS_SIGPROCMASK(how, &set, &oldset)` syscall
  - `SIG_BLOCK`: `mask |= set`
  - `SIG_UNBLOCK`: `mask &= ~set`
  - `SIG_SETMASK`: `mask = set`
- [ ] Implement `SYS_PTHREAD_SIGMASK` alias (same as `SYS_SIGPROCMASK` but per-thread)
- [ ] `sigpending()` syscall: return bitmask of signals pending + blocked for current thread
- [ ] `sigsuspend(mask)` syscall: atomically set mask and sleep until signal arrives
- [ ] Test: two threads, one masks SIGINT — verify only unmasked thread receives it
- [ ] Commit: `"ipc: per-thread signal masks (pthread_sigmask)"`

---

## Priority Order

| Priority | Section                    | Reason                                                      |
| -------- | -------------------------- | ----------------------------------------------------------- |
| ✅ Done  | 1. Pipes                   | Verified complete — shell piping works                     |
| ✅ Done  | 2. Signals                 | Verified complete — Ctrl+C, SIGPIPE work                   |
| ✅ Done  | 3. Shared Memory           | Verified complete — verify `SYS_SHMEM_UNMAP` num           |
| 🔴 P0    | 6. waitpid / exit status   | Shell can’t report exit codes without this                 |
| 🟠 P1    | 4. Message Queues          | Compositor event delivery needs typed message passing       |
| 🟠 P1    | 5. Unix-Domain Sockets     | IxUI ↔ compositor protocol; requires §1 TODO-028 FDs first |
| 🟠 P1    | 7. Per-Thread Signal Masks | Required for correct POSIX multithreaded signal delivery    |

---

## OS Comparison

| Feature                         | 🪟 Windows IPC                   | 🐧 Linux IPC              | 🚀 Impossible OS                   |
| ------------------------------- | ------------------------------- | ------------------------ | --------------------------------- |
| Pipes (anonymous)               | ✅ `CreatePipe`                  | ✅ `pipe(2)`              | ✅ §1 Done                         |
| Signals                         | ⚠️ Basic (Ctrl+C only)          | ✅ Full POSIX signals     | ✅ §2 Done                         |
| Shared Memory                   | ✅ `CreateFileMapping`           | ✅ `shmget` / `mmap`      | ✅ §3 Done                         |
| Message Queues                  | ✅ Window messages / MSMQ        | ✅ `mq_open` (POSIX)      | ⬜ §4 P1                           |
| Unix-Domain Sockets             | ✅ Named pipes `\\.\pipe\`       | ✅ `AF_UNIX`              | ⬜ §5 P1                           |
| waitpid / exit status           | ✅ `WaitForSingleObject`         | ✅ `waitpid(2)`           | ⬜ §6 P0                           |
| **Named pipes (bidirectional)** | ✅ Full duplex named pipes       | ✅ `mkfifo` (half-duplex) | ⬜ §5 covers this via unix sockets |
| Broadcast IPC                   | ✅ `SendMessage(HWND_BROADCAST)` | ⚠️ Signals only          | ⬜ §4 message queues (typed msgs)  |
| **Per-thread signal masks**     | ✅ Per-thread (Win32 threads)    | ✅ `pthread_sigmask`      | ⬜ §7 P1 — POSIX compliant         |

> **After §4–6:** Impossible OS matches Linux’s IPC feature set and exceeds basic Windows IPC in signal richness.
> **After §7:** Full POSIX multithreaded signal delivery — required for any serious user-space threading library.
