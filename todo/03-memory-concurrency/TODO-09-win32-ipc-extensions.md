---
schema_version: 1
id: win32-ipc-extensions
domain: 03-memory-concurrency
status: active
title: "TODO-09 -- Win32 IPC Extensions & Async I/O"
---

# TODO-09 -- Win32 IPC Extensions & Async I/O

> **Goal:** Implement the Win32 IPC layer required before the Win32 subsystem (CSRSS, Win32k) can function: named pipes (NPFS), mailslots (MSFS), I/O completion ports (IOCP) with a DMA-backed worker pool, Object Manager–backed named sync objects, LPC/ALPC ports, and `ImpossibleRing` -- a two-ring async submission interface providing io_uring parity.

> [!IMPORTANT]
> **Already complete:** Anonymous pipes (`pipe.c`), POSIX signals (`signal.c`), and shared memory (`shmem.c`) are done and are the baseline this TODO extends. Ob namespace (→ XREF `02-kernel-core/TODO-32 §4`) must exist before §6 named sync objects and §7 LPC ports can register names; implement §6–§8 only after the Object Manager is up.

## Inputs

- [`src/kernel/ipc/pipe.c`](../../src/kernel/ipc/pipe.c), [`include/kernel/ipc/pipe.h`](../../include/kernel/ipc/pipe.h)
- [`src/kernel/ipc/shmem.c`](../../src/kernel/ipc/shmem.c)
- [`src/kernel/drivers/ahci.c`](../../src/kernel/drivers/ahci.c) -- DMA completion hook for §5 IOCP worker pool
- → XREF: `02-kernel-core/TODO-05-object-manager.md §3` -- Ob namespace (`\Device\NamedPipe\`, `\RPC Control\`) required for §1, §6, §7; named sync object lookup in §6 traverses the Ob directory tree
- → XREF: `02-kernel-core/TODO-12-native-api-ssdt.md` -- all `Nt*` function stubs (`NtCreateNamedPipeFile`, `NtCreateIoCompletion`, `NtCreateEvent`, `NtCreatePort`, `NtAlpcSendWaitReceivePort`, `NtSubmitRing`) are registered as Native API dispatch entries here
- → XREF: `03-memory-concurrency/TODO-08-advanced-sync.md §2` -- `FUTEX_WAIT`/`WAKE` underlies IOCP wait in §2 and ALPC blocking in §8
- → XREF: `03-memory-concurrency/TODO-08-advanced-sync.md §8` -- `waitable_t` vtable used by IOCP completion port and Ob sync objects to integrate with `WaitForMultipleObjects`
- → XREF: `03-memory-concurrency/TODO-05-advanced-virtual-memory.md §5` -- Section Object `NtMapViewOfSection` used by §8 ALPC view attribute and §9 `ImpossibleRing` shared ring buffer
- → XREF: `02-kernel-core/TODO-24-alpc-message-ports.md` -- core ALPC subsystem (`ALPC_PORT`, connection handshake, `NtAlpc*` syscall surface); §8 of this TODO adds only incremental Win32 extensions (handle attribute, direct/indirect mode) on top

## Outcome

- `CreateNamedPipe` / `ConnectNamedPipe` work end-to-end over `\Device\NamedPipe\`; byte-stream and message modes supported; `FSCTL_PIPE_LISTEN` blocks until a client connects.
- Per-instance ring buffers and `FILE_FLAG_OVERLAPPED` IRP struct enable multiple server instances and non-blocking I/O from a single thread.
- Mailslots allow one-way broadcast messaging (`\\.\Mailslot\name`) with read-only server and write-only clients; `GetMailslotInfo`/`SetMailslotInfo` control timeout and size.
- IOCP provides scalable async I/O: `CreateIoCompletionPort` associates a file handle; AHCI/VirtIO DMA completion posts a packet; worker threads drain via `GetQueuedCompletionStatus`.
- All Win32 named sync objects (`CreateEvent`, `CreateMutex`, `CreateSemaphore`) are Object Manager–backed, cross-process accessible by name, and integrate with `WaitForMultipleObjects`.
- LPC ports under `\RPC Control\` enable CSRSS ↔ client 256-byte synchronous request/reply.
- ALPC upgrades LPC with variable-length messages via shared section views, handle marshalling, and direct/indirect message modes.
- `ImpossibleRing` shared SQ+CQ rings allow user-mode programs to submit I/O without a syscall per operation, matching Linux io_uring throughput.

## Implementation Order

| ⭐   | Order | Deliverable                              | Depends On                              | Status |
| --- | :---: | ---------------------------------------- | --------------------------------------- | :----: |
| 💎   |   1   | §1 Named pipes (NPFS) -- server endpoint, byte/message mode | Ob namespace, `pipe.c` baseline         |  [ ]   |
| 💎   |   2   | §2 Named pipe instances + overlapped I/O (IRP) | §1                                      |  [ ]   |
| 💎   |   3   | §3 Mailslots (MSFS) -- broadcast, `GetMailslotInfo` | Ob namespace                            |  [ ]   |
| 💎   |   4   | §4 I/O completion ports (IOCP) -- `NtCreateIoCompletion`, FIFO | TODO-08 §2 (futex wait), TODO-08 §8     |  [ ]   |
| 💎   |   5   | §5 IOCP worker thread pool + DMA completion hook | §4, AHCI/VirtIO DMA callback            |  [ ]   |
| 💎   |   6   | §6 Win32 Ob-backed sync objects -- Event, Mutant, Semaphore | Ob namespace, TODO-08 §8 (`waitable_t`) |  [ ]   |
| 💎   |   7   | §7 LPC basic -- `NtCreatePort`, request/reply, `PORT_MESSAGE` | §6, Ob `\RPC Control\`                  |  [ ]   |
| 💎   |   8   | §8 ALPC extensions -- shared section, handle attrs, direct mode | §7, TODO-05 §5 (Section Object)         |  [ ]   |
| ⭐   |   9   | §9 `ImpossibleRing` SQ+CQ async submission rings | §4, §5, TODO-05 §5 (NtMapViewOfSection) |  [ ]   |
| 💎   |  10   | IPC syscalls wired to SSDT               | §1–§4, D02 T12 §4                       |  [ ]   |

> 💎 = parity -- named pipes, IOCP, named sync objects, LPC, and ALPC are Windows NT core IPC primitives; Impossible OS must match for Win32 compatibility. Mailslots are a Windows-exclusive feature Linux lacks.
> ⭐ = exclusive -- `ImpossibleRing` provides io_uring–style zero-syscall async submission, going beyond classic Windows I/O models.

---

## 1. Named Pipes (NPFS) -- Server Endpoint `[Opus]`

Create the Named Pipe File System (`NPFS`) driver. `NtCreateNamedPipeFile` registers a server endpoint under `\Device\NamedPipe\<name>` in the Ob namespace. `FSCTL_PIPE_LISTEN` blocks the server thread until a client calls `NtOpenFile` on the same path. Supports byte-stream and message read modes; duplex, inbound-only, and outbound-only pipe directions.

**Files:** `src/kernel/ipc/npfs.c` (new), `include/kernel/ipc/npfs.h` (new)

> [!IMPORTANT]
> Named pipe state transitions must be serialised: `DISCONNECTED → LISTENING → CONNECTED → CLOSING`. A pipe handle in the wrong state must return `STATUS_PIPE_DISCONNECTED` or `STATUS_PIPE_LISTENING` as appropriate. Race between `FSCTL_PIPE_LISTEN` and client `NtOpenFile` requires a condvar or event to synchronise without busy-spinning.

- [ ] Define `npfs_pipe_t`: name, `pipe_state_t` enum (`DISCONNECTED`/`LISTENING`/`CONNECTED`/`CLOSING`), `pipe_type_t` (`BYTE_STREAM`/`MESSAGE`), direction, in/out ring buffers, condvar for `FSCTL_PIPE_LISTEN`
- [ ] `NtCreateNamedPipeFile(name, direction, type, max_instances, ...)` -- register under `\Device\NamedPipe\<name>` in Ob; allocate `npfs_pipe_t`; set state `DISCONNECTED`
- [ ] `FSCTL_PIPE_LISTEN`: set state `LISTENING`; `condvar_wait` until client connects; return `STATUS_SUCCESS`
- [ ] Client `NtOpenFile(\Device\NamedPipe\<name>)` -- find `npfs_pipe_t`; if `LISTENING`, set `CONNECTED`; signal server's condvar; return client file handle
- [ ] `NtReadFile` / `NtWriteFile` on named pipe -- byte-stream: copy raw bytes; message mode: prepend `uint32_t` length prefix
- [ ] `FSCTL_PIPE_DISCONNECT` -- set `CLOSING`, drain buffers, wake blocked readers/writers with `STATUS_PIPE_BROKEN`
- [ ] Boot log: `[NPFS] named pipe driver registered under \Device\NamedPipe\`
- [ ] Commit: `"ipc: NPFS named pipe driver -- NtCreateNamedPipeFile, FSCTL_PIPE_LISTEN, byte/message mode"`

## 2. Named Pipe Instances + Overlapped I/O `[Opus]`

Multiple server instances of the same named pipe name allow N simultaneous clients. `FILE_FLAG_OVERLAPPED` issues non-blocking I/O via an IRP (I/O Request Packet) struct; the caller polls or waits on an associated event object. `GetOverlappedResult` queries the IRP state.

**Files:** `src/kernel/ipc/npfs.c`, `include/kernel/ipc/npfs.h`, `include/kernel/ipc/irp.h` (new)

> [!IMPORTANT]
> Per-instance ring buffers must be completely independent -- a slow client on instance 2 must not block writes to instance 1. `dwMaxInstances` counts currently-CONNECTED instances; creating a new instance when `count == max` returns `STATUS_PIPE_BUSY`.

- [ ] Define `irp_t`: `status`, `bytes_transferred`, `event_handle`, `user_buffer`, `user_length`, `irp_flags`
- [ ] `NtCreateNamedPipeFile` with `dwMaxInstances > 1`: each server call allocates a new `npfs_pipe_t` instance sharing the same name; `instance_count` tracked in a shared `npfs_name_t` header
- [ ] `FILE_FLAG_OVERLAPPED` on `NtReadFile`/`NtWriteFile`: allocate `irp_t`; if ring empty/full, queue IRP and return `STATUS_PENDING` immediately; set event when complete
- [ ] IRP completion: when data arrives/drains, complete pending IRPs in FIFO order; set `irp->status = STATUS_SUCCESS`; signal `irp->event_handle`
- [ ] `GetOverlappedResult(handle, &overlapped, &bytes, wait)` → `NtWaitForSingleObject(irp->event_handle)` + read `irp->status`/`irp->bytes_transferred`
- [ ] `STATUS_PIPE_BUSY` when `instance_count >= max_instances` on new `NtCreateNamedPipeFile`
- [ ] Commit: `"ipc: named pipe instances -- dwMaxInstances, IRP overlapped I/O, GetOverlappedResult"`

## 3. Mailslots (MSFS) `[Sonnet]`

One-way broadcast IPC: a server creates `\\.\Mailslot\<name>` (read-only); any number of clients open it for write-only access and post messages. `GetMailslotInfo` queries pending messages and slot size; `SetMailslotInfo` adjusts the read timeout.

**Files:** `src/kernel/ipc/msfs.c` (new), `include/kernel/ipc/msfs.h` (new)

> [!NOTE]
> Mailslots are a Windows-exclusive IPC mechanism; Linux has no direct equivalent. They are simpler than named pipes: no connection handshake, no duplex, no state machine -- server reads, clients write, messages are discrete.

- [ ] Define `mailslot_t`: name, message queue (ring of fixed-size slots), `max_message_size`, `slot_count`, `next_read_timeout_ms`, condvar for reader wait
- [ ] `NtCreateMailslotFile(name, max_msg_size, read_timeout)` -- register under `\Device\Mailslot\<name>`; server has `GENERIC_READ` handle
- [ ] Client `NtOpenFile(\Device\Mailslot\<name>)` -- returns `GENERIC_WRITE`-only handle; multiple clients may open simultaneously
- [ ] `NtWriteFile` (client): enqueue message into mailslot ring; wake server `condvar`; return `STATUS_SUCCESS` or `STATUS_INSUFFICIENT_RESOURCES` if queue full
- [ ] `NtReadFile` (server): dequeue one message; if empty, wait up to `read_timeout_ms` on condvar; return `STATUS_IO_TIMEOUT` if expired
- [ ] `GetMailslotInfo(hMailslot, &max_msg_size, &next_size, &msg_count, &read_timeout)` -- fill from `mailslot_t` fields
- [ ] `SetMailslotInfo(hMailslot, read_timeout_ms)` -- update `next_read_timeout_ms`
- [ ] Commit: `"ipc: MSFS mailslot driver -- NtCreateMailslotFile, broadcast write, GetMailslotInfo"`

## 4. I/O Completion Ports (IOCP) `[Opus]`

`NtCreateIoCompletion` creates a completion port object. `NtSetIoCompletion` enqueues an `IO_COMPLETION_PACKET` onto the port's spinlock-protected FIFO. `NtRemoveIoCompletion` dequeues one packet, blocking if the queue is empty. File handles are associated at open time or via `NtSetInformationFile(FileCompletionInformation)`.

**Files:** `src/kernel/ipc/iocp.c` (new), `include/kernel/ipc/iocp.h` (new)

> [!IMPORTANT]
> The IOCP queue is the kernel's hot I/O path -- every completed DMA transfer posts here. Use `ticket_lock_t` (→ XREF: `TODO-08 §5`) not a mutex for the FIFO spinlock. `NtRemoveIoCompletion` must support a timeout so thread-pool workers can check shutdown flags.

- [ ] Define `io_completion_port_t`: `ticket_lock_t lock`, `waitable_t waitable`, ring buffer of `IO_COMPLETION_PACKET { key, apc_context, status, info }`
- [ ] `NtCreateIoCompletion(port, access, obj_attr, concurrency)` -- allocate port object in Ob; `concurrency` caps simultaneous dequeue threads (0 = CPU count)
- [ ] `NtSetIoCompletion(port, key, apc_ctx, status, info)` -- enqueue packet under `ticket_lock`; signal waitable (→ XREF `TODO-08 §8`)
- [ ] `NtRemoveIoCompletion(port, &key, &apc_ctx, &io_status, timeout)` -- dequeue packet; if empty, block on port's `waitable_t` with timeout; return `STATUS_TIMEOUT` if expired
- [ ] File handle association: `NtSetInformationFile(handle, FileCompletionInformation, &{port, key})` -- stores port reference in the file object
- [ ] On `NtReadFile`/`NtWriteFile` with overlapped + completion port: on I/O completion, call `NtSetIoCompletion` automatically (no explicit user call)
- [ ] Win32 `CreateIoCompletionPort` → `NtCreateIoCompletion`; `GetQueuedCompletionStatus` → `NtRemoveIoCompletion`
- [ ] **Retrofit existing in-tree consumers to the new Ob-backed IOCP.** The current `src/kernel/nt/nt_file.c` IOCP is a 16-slot static array (`s_iocp_pool`) with a numeric `idx + 0x10000` "handle" that has no ownership check -- any task can guess another task's IOCP index and drain / forge completion packets. After this section's Ob-backed redesign ships: (a) replace `iocp_index_from_handle` with an Ob handle-table lookup that enforces per-port access rights; (b) update `io_completion_post` (kernel-internal helper added by TODO-12 §6 for ALPC async notification) to take the validated `io_completion_port_t *` rather than a numeric handle; (c) update `AlpcAssociateCompletionPort` in `src/kernel/ipc/alpc_port.c` to pin the Ob-backed IOCP object instead of storing the raw handle value on the ALPC port. Test: cross-task attempt to `NtSetIoCompletion` / `NtRemoveIoCompletion` a peer's IOCP must fail `STATUS_ACCESS_DENIED`.
- [ ] Commit: `"ipc: IOCP -- NtCreateIoCompletion, IO_COMPLETION_PACKET FIFO, NtRemoveIoCompletion"`

## 5. IOCP Worker Thread Pool + DMA Completion Hook `[Opus]`

AHCI and VirtIO DMA-done ISRs post completion packets to the associated IOCP port. A configurable-size worker thread pool calls `NtRemoveIoCompletion` in a loop, invoking user callbacks. The pool sizes to the IOCP `concurrency` value and expands/shrinks based on active worker count.

**Files:** `src/kernel/ipc/iocp.c`, `src/kernel/drivers/ahci.c`, `src/kernel/drivers/virtio_blk.c`

> [!IMPORTANT]
> DMA completion may fire in an ISR context (IRQL > `DISPATCH_LEVEL`). The IOCP enqueue (`NtSetIoCompletion`) must be ISR-safe: use `ticket_lock_irqsave` when called from interrupt context. Worker threads run at `DISPATCH_LEVEL` or lower -- never in interrupt context.

- [ ] Define `iocp_thread_pool_t`: thread array, `active_count`, `target_count`, `shutdown` flag, associated completion port
- [ ] `iocp_create_thread_pool(port, min_threads, max_threads)` -- spawn `min_threads` kernel threads each calling `NtRemoveIoCompletion` in a loop
- [ ] AHCI DMA done ISR: `iocp_post_from_isr(port, key, status, bytes)` -- `ticket_lock_irqsave` enqueue; does NOT call `schedule()`
- [ ] VirtIO blk vq interrupt: same `iocp_post_from_isr` path
- [ ] Worker thread body: loop `NtRemoveIoCompletion(port, ..., INFINITE)`; on packet: invoke registered callback; on `shutdown` flag: exit
- [ ] Pool scaling: if all `target_count` workers are active, spawn one more (up to `max_threads`); if idle for 30 s, terminate one excess thread
- [ ] `CloseThreadpoolIo` / `SubmitThreadpoolWork` Win32 stubs route to pool management
- [ ] Boot log: `[IOCP] thread pool: %u workers, DMA completion hook active`
- [ ] Commit: `"ipc: IOCP worker pool -- DMA ISR post, pool scaling, AHCI/VirtIO hook"`

## 6. Win32 Ob-Backed Named Sync Objects `[Sonnet]`

`NtCreateEvent`, `NtCreateMutant`, and `NtCreateSemaphore` create kernel sync objects registered in the Object Manager namespace. They can be opened by name across processes, referenced by handle, and waited on via `NtWaitForSingleObject` / `NtWaitForMultipleObjects`. All integrate with the `waitable_t` vtable from `TODO-08 §8`.

**Files:** `src/kernel/sched/ob_sync.c` (new), `include/kernel/sched/ob_sync.h` (new)

> [!IMPORTANT]
> → XREF: `02-kernel-core/TODO-05-object-manager.md §3` -- objects are registered in the Ob directory; `NtOpenEvent(name)` must resolve through the Ob namespace. Handle table reference counting must prevent use-after-free when a process closes a handle while another is blocked on it.

- [ ] `NtCreateEvent(name, type, initial_state)` -- `type` = `NotificationEvent` (manual reset) or `SynchronizationEvent` (auto reset); register in Ob under `name` if provided; return handle
- [ ] `NtSetEvent` / `NtResetEvent` / `NtPulseEvent` -- signal, clear, or signal-then-auto-clear; wake waiters via `waitable_t`
- [ ] `NtCreateMutant(name, initial_owner)` -- kernel mutex with priority inheritance (`PI` -- already in `mutex_t`); register in Ob; owner stored as `task_t *`
- [ ] `NtReleaseMutant(handle)` -- assert caller is owner; release PI mutex; return previous signal count
- [ ] `NtCreateSemaphore(name, initial_count, max_count)` -- wrap existing `semaphore_t`; register in Ob
- [ ] `NtReleaseSemaphore(handle, release_count, &prev_count)` -- call `sem_signal(n)` times
- [ ] All three types implement `waitable_t` vtable for integration with `NtWaitForMultipleObjects`
- [ ] Win32 `CreateEvent` / `CreateMutex` / `CreateSemaphore` / `OpenEvent` / `OpenMutex` → `Nt*` wrappers
- [ ] Commit: `"ipc: Win32 Ob-backed sync objects -- NtCreateEvent/Mutant/Semaphore, named cross-process access"`

## 7. LPC Basic -- Local Procedure Call `[Opus]`

LPC is the synchronous request/reply IPC used by Windows subsystems (CSRSS, Win32k). A server creates a port under `\RPC Control\`; clients connect; each call blocks until the server replies with a 256-byte `PORT_MESSAGE`. The kernel copies the message -- no shared memory needed for small messages.

**Files:** `src/kernel/ipc/lpc.c` (new), `include/kernel/ipc/lpc.h` (new)

> [!IMPORTANT]
> LPC is a rendezvous protocol: `NtRequestWaitReplyPort` atomically enqueues the request, wakes the server, and sleeps. The server's `NtReplyWaitReceivePort` atomically dequeues, processes, and calls `NtReplyPort` which wakes the original caller. There is no buffering -- sender and receiver rendezvous at every message.

- [ ] Define `port_message_t`: `{ uint16_t total_length; uint16_t data_length; uint32_t type; uint32_t client_id; uint64_t msg_id; uint8_t data[256]; }`
- [ ] Define `lpc_port_t`: name, connected client list, request FIFO, condvar pair (server waiting / client waiting)
- [ ] `NtCreatePort(name, max_connect_info_len, max_msg_len)` -- register under `\RPC Control\<name>`; server port
- [ ] `NtConnectPort(name, security_qos, client_view, ...)` -- find server port in Ob; enqueue connect request; block until server calls `NtAcceptConnectPort`; return client connection port handle
- [ ] `NtAcceptConnectPort` -- dequeue pending connect; optionally reject; set up bidirectional channel
- [ ] `NtRequestWaitReplyPort(port, &request, &reply)` -- copy request into port FIFO; wake server; sleep; on `NtReplyPort`, copy reply into `&reply`; return
- [ ] `NtReplyWaitReceivePort(port, &client_id, &reply, &msg)` -- if `reply` non-null, copy to blocked caller and wake; dequeue next `msg`; block if empty
- [ ] **Retrofit the 15 LPC SSDT stubs in `src/kernel/nt/nt_lpc.c` (SSDT 0x0100-0x010E)** to real handlers that call into `lpc.c` instead of returning `STATUS_NOT_IMPLEMENTED`. Covered syscalls: `NtCreatePort` (0x0100), `NtCreateWaitablePort` (0x0101 -- extends §7 port create with `ALPC_PORTFLG_WAITABLE_PORT`), `NtConnectPort` (0x0102), `NtSecureConnectPort` (0x0103 -- adds SID validation on top of `NtConnectPort`; uses `SeAccessCheck` from TODO-11 §9), `NtAcceptConnectPort` (0x0104), `NtCompleteConnectPort` (0x0105 -- two-phase accept handshake), `NtListenPort` (0x0106 -- blocking variant of server-side dequeue), `NtReplyPort` (0x0107 -- fire-and-forget reply without receive), `NtReplyWaitReceivePort` (0x0108), `NtReplyWaitReceivePortEx` (0x0109 -- adds `Timeout` parameter), `NtRequestPort` (0x010A -- fire-and-forget request without reply wait), `NtRequestWaitReplyPort` (0x010B), `NtImpersonateClientOfPort` (0x010C -- uses `SeImpersonate` from TODO-11 §4), `NtReadRequestData` (0x010D -- reads large client buffer referenced by a message), `NtWriteRequestData` (0x010E -- writes into client buffer). Each retrofit drops the `SCOPE-GAP-ALLOWED` sentinel in the stub file and adds a handler body that probes user buffers, resolves the port handle via `ObpReferenceObjectByHandle` (-> XREF TODO-03 §5), and invokes the matching `lpc_*` helper. This auto-closes TODO-06 §20 (15 LPC syscalls deferred here).
- [ ] Boot log: `[LPC] port subsystem active; \RPC Control\ registered`
- [ ] Commit: `"ipc: LPC -- NtCreatePort, NtConnectPort, NtRequestWaitReplyPort, PORT_MESSAGE rendezvous"`

## 8. ALPC Extensions `[Opus]`

ALPC handle-attribute marshalling and direct/indirect mode selection -- the incremental extensions on top of the core ALPC subsystem implemented in `02-kernel-core/TODO-24-alpc-message-ports.md`. TODO-12 provides `ALPC_PORT`, the three-way connection handshake, `NtAlpcSendWaitReceivePort`, and port sections. This section adds only what is deferred from TODO-12 or specific to the Win32 IPC integration layer.

**Files:** `src/kernel/ipc/alpc.c` (extends TODO-12), `include/kernel/ipc/alpc.h` (extends TODO-12)

> [!IMPORTANT]
> → XREF: `02-kernel-core/TODO-24-alpc-message-ports.md` -- prerequisite; `ALPC_PORT`, `NtAlpcCreatePort`, `NtAlpcConnectPort`, `NtAlpcAcceptConnectPort`, `NtAlpcSendWaitReceivePort`, and port sections are implemented there. Do NOT re-implement those primitives here.
> → XREF: `02-kernel-core/TODO-24-alpc-message-ports.md` §8-§9: gap-analysis 2026-04-14; D02 split NtAlpc into §8 (SSDT stub retrofit, OpenSender gap items) and §9 (QueryInformation, SetInformation, CancelMessage). This file §8 adds only handle-attribute and direct/indirect extensions.
> → XREF: `03-memory-concurrency/TODO-05-advanced-virtual-memory.md §5` -- ALPC view attributes work by mapping the same Section Object into both the client and server address spaces. `NtMapViewOfSection` must be fully operational before §8 can be implemented. Handle marshalling requires the Object Manager handle table in §6 to duplicate handles across processes atomically.

- [ ] Handle attribute (`ALPC_HANDLE_ATTR`, bit 0x4 in `ALPC_MESSAGE_ATTRIBUTES`): sender provides a handle array; kernel duplicates each handle into receiver’s handle table atomically via `NtDuplicateObject`; receiver gets the new handle values in the message header (→ XREF `02-kernel-core/TODO-24-alpc-message-ports.md §6` message-attribute dispatch; deferred there pending `NtDuplicateObject` stability)
- [ ] Direct mode: messages ≤ 256 bytes copied by value (LPC-compatible path); add size-threshold check in `AlpcEnqueueMessage` to auto-select inline copy vs. view-attribute path
- [ ] Indirect mode: messages > 256 bytes auto-select view attribute; kernel sets `DataLength = 0`, populates `ALPC_DATA_VIEW_ATTR` from the registered port section
- [ ] Commit: `"ipc/alpc: ALPC extensions -- handle-attribute marshalling, direct/indirect mode selection"`

## 9. `ImpossibleRing` -- Async Submission Rings `[Opus]`

Two memory-mapped rings (submission queue SQ + completion queue CQ) shared between user and kernel via `NtMapViewOfSection`. User writes SQEs (submission queue entries) directly into the SQ ring without a syscall; calls `NtSubmitRing(count)` to notify the kernel; kernel drains SQEs in a workqueue and posts CQEs (completion queue entries) to the CQ ring. Provides io_uring–style zero-per-operation-syscall async I/O.

**Files:** `src/kernel/ipc/impossiblering.c` (new), `include/kernel/ipc/impossiblering.h` (new)

> [!IMPORTANT]
> The SQ head and CQ tail are written by the kernel; SQ tail and CQ head are written by the user. Each must be a single atomic `uint32_t` -- no other synchronisation between kernel and user on the ring indices. Use `WRITE_ONCE`/`READ_ONCE` (→ XREF `TODO-07 §8`) on all index reads/writes to prevent compiler reordering across the ring boundary.

- [ ] Define `sqe_t` (submission entry): `opcode`, `fd`, `buf_ptr`, `len`, `offset`, `user_data`; `cqe_t` (completion entry): `user_data`, `result`, `flags`
- [ ] Define `ring_t`: `head` (`uint32_t`), `tail` (`uint32_t`), `mask` (`= capacity - 1`), `entries[]`; capacity must be a power of 2
- [ ] `NtCreateRing(sq_capacity, cq_capacity)` -- allocate two ring sections; `NtMapViewOfSection` both into caller's address space; return ring handle + user-visible pointers
- [ ] User submission: write SQE at `sq.tail & mask`; `WRITE_ONCE(sq.tail, sq.tail + 1)`; ring is now visible to kernel without a syscall
- [ ] `NtSubmitRing(ring_handle, count)` -- batch doorbell: notify kernel that `count` SQEs are ready; kernel workqueue picks up and processes
- [ ] Kernel drain: read `READ_ONCE(sq.tail)`; process all SQEs from `sq.head` to `sq.tail`; for each completed op, write CQE at `cq.tail & mask`; `WRITE_ONCE(cq.tail, cq.tail + 1)`
- [ ] Supported opcodes (initial): `RING_OP_READ`, `RING_OP_WRITE`, `RING_OP_FSYNC`, `RING_OP_NOP`
- [ ] CQ polling: user spins on `READ_ONCE(cq.tail)` -- no syscall needed for polling completions
- [ ] `NtCloseRing(handle)` -- unmap both sections; drain pending SQEs before return
- [ ] Boot log: `[RING] ImpossibleRing subsystem active (SQ+CQ rings, io_uring compatible)`
- [ ] Commit: `"ipc: ImpossibleRing -- SQ+CQ shared rings, NtSubmitRing, zero-syscall I/O submission"`

---

## 10. IPC Syscalls Wired to SSDT

Wire named pipes, mailslots, and I/O completion port syscalls into the SSDT. (→ XREF: 02-kernel-core/TODO-12-native-api-ssdt.md §5)

- [ ] `NtCreateNamedPipeFile(...)` → SSDT 0x001B: named pipe creation via NPFS (§1)
- [ ] `NtCreateNamedPipeFileEx(...)` → SSDT 0x0398: extended named pipe creation
- [ ] `NtCreateMailslotFile(...)` → SSDT 0x001C: one-way IPC mailslot (§3)
- [ ] `NtCreateMailslotFileEx(...)` → SSDT 0x0399: extended mailslot creation
- [ ] `NtCreateIoCompletion(IoCompletionHandle, DesiredAccess, ObjectAttributes, Count)` → SSDT 0x0088 (§4)
- [ ] `NtSetIoCompletion(IoCompletionHandle, KeyContext, ApcContext, IoStatus, IoStatusInformation)` → SSDT 0x0089
- [ ] `NtRemoveIoCompletion(IoCompletionHandle, KeyContext, ApcContext, IoStatusBlock, Timeout)` → SSDT 0x008A
- [ ] `NtQueryIoCompletion(IoCompletionHandle, InformationClass, Buffer, Length, RetLen)` → SSDT 0x008B
- [ ] `NtSetIoCompletionEx(...)` → SSDT 0x008C / 0x0382
- [ ] `NtRemoveIoCompletionEx(...)` → SSDT 0x008D / 0x0383
- [ ] All functions return `NTSTATUS`
- [ ] Commit: `"ipc: wire named pipe, mailslot, and IOCP syscalls to SSDT"`

**Test checkpoint:** `NtCreateNamedPipeFile` creates `\\.\pipe\test`; reader/writer round-trip. `NtCreateIoCompletion` + `NtSetIoCompletion` + `NtRemoveIoCompletion` post/dequeue round-trip.

---

## OS Comparison


| ⭐   | Feature                                  | 🪟 Win11                                  | 🐧 Linux                                  | 🚀 Impossible OS                          |
| --- | ---------------------------------------- | ---------------------------------------- | ---------------------------------------- | ---------------------------------------- |
| 💎   | Named pipes -- byte/message, duplex      | ✅ NPFS; `CreateNamedPipe`; message + byte | ✅ `mkfifo(3)` / `open(O_RDWR)`; byte only; | ⬜ §1 -- NPFS, `NtCreateNamedPipeFile`, byte + message, |
| 💎   | Overlapped I/O + multiple pipe instances | ✅ `dwMaxInstances`; `FILE_FLAG_OVERLAPPED`; `GetOverlappedResult` | ❌ No named pipe instances; `O_NONBLOCK`  | ⬜ §2 -- `dwMaxInstances`, `irp_t`, `FILE_FLAG_OVERLAPPED` |
| 💎   | Mailslots -- one-way broadcast           | ✅ `CreateMailslot`; broadcast via `\\.\Mailslot\*` | ❌ No equivalent; workarounds use UDP/domain | ⬜ §3 -- MSFS, `NtCreateMailslotFile`, broadcast, `GetMailslotInfo` |
| 💎   | I/O completion ports                     | ✅ `CreateIoCompletionPort`; `GetQueuedCompletionStatus` | ✅ `io_uring`; `epoll` (FD-only)          | ⬜ §4 -- `NtCreateIoCompletion`, FIFO, `ticket_lock_t`, `waitable_t` |
| 💎   | DMA-backed async I/O completion worker pool | ✅ Thread pool API (`CreateThreadpoolIo`) | ✅ `io_uring` sqpoll / libaio thread      | ⬜ §5 -- ISR-safe `iocp_post_from_isr`, pool scaling, AHCI/VirtIO |
| 💎   | Named cross-process sync objects         | ✅ `CreateEvent(name)` / `OpenEvent(name)` via Ob | ❌ No named kernel sync objects;          | ⬜ §6 -- Ob-backed, `NtCreateEvent/Mutant/Semaphore`, cross-process name |
| 💎   | LPC -- synchronous request/reply 256-byte messages | ✅ `NtRequestWaitReplyPort`; used by CSRSS, Win32k | ❌ No equivalent kernel rendezvous IPC;   | ⬜ §7 -- `NtCreatePort`, rendezvous, `PORT_MESSAGE`, `\RPC Control\` |
| 💎   | ALPC                                     | ✅ ALPC in Vista+; `NtAlpcSendWaitReceivePort` | ❌ No equivalent; D-Bus operates in       | ⬜ §8 -- view + handle attributes, direct/indirect, |
| ⭐   | `ImpossibleRing` zero-syscall async submission | ❌ IOCP still requires one `GetQueuedCompletionStatus` | ✅ `io_uring` SQ+CQ rings; `io_uring_enter` doorbell | ⬜ §9 -- SQ+CQ `NtMapViewOfSection`, `NtSubmitRing` batch doorbell |

> **After §1–8:** Impossible OS achieves full Win32 IPC parity required for CSRSS and Win32k -- named pipes, mailslots, IOCP, named sync objects, LPC, and ALPC all present. Mailslots and named sync objects fill gaps that Linux never addressed at the kernel level. `ImpossibleRing` (§9) then surpasses classic Windows IOCP by providing io_uring–style zero-per-operation-syscall async I/O with a shared ring buffer -- the same architecture that made io_uring the Linux storage throughput benchmark leader.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] Named pipe: server calls `NtCreateNamedPipeFile` + `FSCTL_PIPE_LISTEN`; client opens pipe; exchange 4 KB message-mode message; server reads back correctly
- [ ] Multiple instances: 4 server instances, 4 simultaneous clients; verify each gets its own buffer; slow client on instance 2 does not stall instance 1
- [ ] Mailslot: 3 writer clients post messages; server reads all 3 in order; `GetMailslotInfo` returns `msg_count = 3`
- [ ] IOCP: `ReadFile` on AHCI block device with `FILE_FLAG_OVERLAPPED` + completion port; `GetQueuedCompletionStatus` returns completion key and byte count
- [ ] IOCP pool: 8 concurrent async reads; serial log shows all complete without dropped packets
- [ ] Named event: `CreateEvent("TestEvent")`; second process `OpenEvent("TestEvent")` + `WaitForSingleObject`; first process `SetEvent` -- second wakes
- [ ] LPC: mock CSRSS server creates port; client `NtConnectPort` + `NtRequestWaitReplyPort` with test payload; server echoes; client receives correct reply
- [ ] ALPC: >256-byte message (512 B); kernel selects indirect mode; section view mapped into server; server reads data without copy
- [ ] `ImpossibleRing`: user writes 64 SQEs to SQ ring without syscall; single `NtSubmitRing(64)`; reads 64 CQEs from CQ ring; verifies all completions present
- [ ] Commit: `"ipc: Win32 IPC extensions -- NPFS, MSFS, IOCP, ob-sync, LPC, ALPC, ImpossibleRing"`
