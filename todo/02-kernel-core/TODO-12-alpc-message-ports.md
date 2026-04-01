# TODO-12 — ALPC / Message Ports

> **Goal:** Implement Advanced Local Procedure Call (ALPC) — the kernel's connection-oriented message-passing substrate. ALPC gives every process pair a typed, reference-counted port object, a three-way connection handshake (server create → client connect → server accept), synchronous send+wait+reply semantics, asynchronous delivery via completion lists, and optional large-data transfer through mapped port sections. The Win32 subsystem server (CSRSS), the RPC local transport, COM local activation, and every NT service that talks back to a client process are all built on top of ALPC. The existing IPC layer (pipes, shared memory, signals) cannot substitute for it because it has no connection-oriented reply semantics — a server cannot wait for exactly one client's reply and route it back to the right caller. Without ALPC, the Win32 subsystem server model is impossible to build.

> [!IMPORTANT]
> **Current state:** `src/kernel/ipc/` has pipes (`pipe.c`), shared memory (`shmem.c`), and signal delivery (`signal.c`), none of which are managed by the Object Manager. There are no port objects, no connection queues, no synchronous request/reply channels, and no `NtAlpc*` syscalls. CSRSS cannot be started without this subsystem.

---

## Inputs

- `src/kernel/ipc/` — existing pipe, shm, signal primitives (context only)
- `include/kernel/ipc/` — current IPC headers
- `src/kernel/sched/task.c` — `struct task`, wait/wake primitives
- → XREF: `TODO-03-object-manager.md §1–§4` — ALPC ports are `OBJECT_TYPE` kernel objects with handles, reference counts, and named entries in `\RPC Control\`
- → XREF: `TODO-03-object-manager.md §7` — `NtAlpcCreatePortSection` registers a Section object with the port; section creation depends on the Section object type
- → XREF: `TODO-05-native-api-layer.md §4` — all `NtAlpc*` entry points are SSDT slots; wiring happens after the SSDT exists
- → XREF: `TODO-06-irql-model-dpcs.md §3` — message delivery runs at `DISPATCH_LEVEL` briefly when queuing to the server's message queue; IRQL discipline applies
- → XREF: `TODO-11-security-reference-monitor.md §4–§7` — client security context capture requires `ACCESS_TOKEN`; server impersonating a client requires `SeImpersonatePrivilege`
- → XREF: `11-user-platform-sdk/TODO-05` *(planned)* — CSRSS (Win32 subsystem server) creates its `ApiPort` using the bootstrap path defined in §9 of this TODO
- → XREF: `03-memory-concurrency/TODO-08-win32-ipc-extensions.md §8` — incremental Win32 ALPC extensions (handle-attribute cross-process dup, direct/indirect mode) build on top of this TODO's `ALPC_PORT` object and `NtAlpc*` syscall surface; TODO-08 §8 must not re-implement core ALPC primitives

---

## Outcome

- `ALPC_PORT` kernel object type registered via Object Manager with three subtypes: server connection port, client port, server communication port.
- Named server ports appear in `\RPC Control\<name>` in the kernel namespace.
- `NtAlpcCreatePort` / `NtAlpcConnectPort` / `NtAlpcAcceptConnectPort` implement the full three-way connection handshake.
- `NtAlpcSendWaitReceivePort` handles synchronous send+wait, reply, and receive-only in a single entry point.
- Asynchronous message delivery via `ALPC_COMPLETION_LIST` integrates with `NtWaitForSingleObject` and I/O completion ports.
- Large data (> 512 bytes) transfers through port sections without copying.
- Server can capture and impersonate the client's security token.
- CSRSS `ApiPort` boots and accepts the first Win32 subsystem connections.

---

## Implementation Order

| ⭐  | Order | Deliverable                                      | Depends On             | Status |
| --- | :---: | ------------------------------------------------ | ---------------------- | :----: |
| 💎  |   1   | Message header, port attributes, type codes      | —                      |  [ ]   |
| 💎  |   2   | ALPC_PORT object & Object Manager registration   | 1, TODO-03 §1–§4       |  [ ]   |
| 💎  |   3   | Connection state machine (create/connect/accept) | 2, TODO-05 §4          |  [ ]   |
| 💎  |   4   | Synchronous send+wait+receive engine             | 3, TODO-06 §3          |  [ ]   |
| 💎  |   5   | Asynchronous delivery & completion list          | 4                      |  [ ]   |
| 💎  |   6   | Large data: port sections & view mapping         | 2, TODO-03 §7          |  [ ]   |
| 💎  |   7   | Security: client token capture & impersonation   | 4, TODO-11 §4–§7       |  [ ]   |
| 💎  |   8   | NtAlpc* syscall table wiring & query/set         | 1–7, TODO-05 §4        |  [ ]   |
| 💎  |   9   | CSRSS ApiPort bootstrap                          | 3–8                    |  [ ]   |
| ⭐  |  10   | Live port monitor (`alpcmon.exe`)                | 8                      |  [ ]   |

> 💎 = parity work — matches what Windows 11 and Linux already do.
> ⭐ = exclusive work — Impossible OS is superior or first.

---

## 1. Message Header, Port Attributes & Type Codes `[Sonnet]`

### 1.1 PORT_MESSAGE header

- [ ] Define in `include/kernel/ipc/alpc.h`:
  ```c
  typedef struct {
      uint16_t TotalLength;    /* sizeof header + sizeof body */
      uint16_t DataLength;     /* sizeof body only */
      uint16_t Type;           /* message type — see below */
      uint16_t DataInfoOffset; /* offset from end of header to ALPC_DATA_INFO,
                                  0 if no data info entries */
      CLIENT_ID ClientId;      /* filled by kernel on send: {ProcessId, ThreadId} */
      uint64_t MessageId;      /* monotonic ID assigned by kernel */
      uint64_t CallbackId;     /* for async replies */
  } PORT_MESSAGE;

  typedef struct { uint32_t ProcessId; uint32_t ThreadId; } CLIENT_ID;

  /* Message body follows immediately after PORT_MESSAGE in the same allocation */
  typedef struct {
      PORT_MESSAGE Header;
      /* uint8_t Body[DataLength]; */
  } ALPC_MESSAGE;
  ```
- [ ] Message type constants:
  ```c
  #define ALPC_MSG_TYPE_REQUEST         1  /* client → server request */
  #define ALPC_MSG_TYPE_REPLY           2  /* server → client reply */
  #define ALPC_MSG_TYPE_DATAGRAM        3  /* fire-and-forget, no reply */
  #define ALPC_MSG_TYPE_LOST_REPLY      4  /* server side closed before reply */
  #define ALPC_MSG_TYPE_PORT_CLOSED     5  /* peer port destroyed */
  #define ALPC_MSG_TYPE_CLIENT_DIED     6  /* client process terminated */
  #define ALPC_MSG_TYPE_EXCEPTION       7  /* exception in client */
  #define ALPC_MSG_TYPE_DEBUG_EVENT     8  /* debug event */
  #define ALPC_MSG_TYPE_ERROR_EVENT     9  /* kernel error notification */
  #define ALPC_MSG_TYPE_CONNECTION_REQUEST 10 /* sent on NtAlpcConnectPort */
  ```
- [ ] Maximum inline message body: `ALPC_MAX_ALLOWED_MESSAGE_LENGTH = 65528` bytes; kernel rejects messages that exceed this; recommended threshold for using port sections instead: 512 bytes

### 1.2 Port attributes

- [ ] `ALPC_PORT_ATTRIBUTES`:
  ```c
  typedef struct {
      uint32_t Flags;            /* ALPC_PORTFLG_* below */
      SECURITY_QUALITY_OF_SERVICE SecurityQos; /* impersonation level for server */
      uint64_t MaxMessageLength; /* max inline message body; 0 = default 512 */
      uint64_t MemoryBandwidth;  /* memory-bandwidth accounting (0 = unlimited) */
      uint64_t MaxPoolUsage;     /* max pool memory for queued messages */
      uint64_t MaxSectionSize;   /* max size of a single port section */
      uint64_t MaxViewSize;      /* max size of a single mapped view */
      uint64_t MaxTotalSectionSize;
      uint32_t DupObjectTypes;   /* bitmask of object types that can be duped */
  } ALPC_PORT_ATTRIBUTES;
  ```
- [ ] `ALPC_PORTFLG_*` constants:
  - `ALPC_PORTFLG_LPC_MODE (0x20000)` — compatibility with old LPC style
  - `ALPC_PORTFLG_ALLOW_DUP_OBJECT (0x80000)` — permit handle duplication
  - `ALPC_PORTFLG_WAITABLE_PORT (0x40000)` — port acts as waitable object; signalled when a message is queued
  - `ALPC_PORTFLG_SYSTEM_PROCESS (0x100000)` — port owned by kernel/SYSTEM
- [ ] `SECURITY_QUALITY_OF_SERVICE`:
  ```c
  typedef struct {
      uint32_t Length;
      SECURITY_IMPERSONATION_LEVEL ImpersonationLevel; /* Anonymous/Identification/Impersonation/Delegation */
      uint8_t  ContextTrackingMode; /* SECURITY_STATIC_TRACKING (0) or DYNAMIC (1) */
      bool     EffectiveOnly;       /* strip disabled privileges/groups from captured token */
  } SECURITY_QUALITY_OF_SERVICE;
  ```

### 1.3 Message send flags

- [ ] `NtAlpcSendWaitReceivePort` flags:
  ```c
  #define ALPC_MSGFLG_REPLY_MESSAGE       0x1  /* this message is a reply */
  #define ALPC_MSGFLG_LPC_MODE            0x2  /* LPC compat — sync call+reply */
  #define ALPC_MSGFLG_RELEASE_MESSAGE     0x10 /* release message back to pool */
  #define ALPC_MSGFLG_SYNC_REQUEST        0x20000 /* block until reply received */
  #define ALPC_MSGFLG_WAIT_USER_MODE      0x100000 /* alertable wait */
  #define ALPC_MSGFLG_WAIT_PENDING_CALLBACKS 0x200000 /* wait for async completions */
  ```

### 1.4 Commit

- [ ] Commit: `"kernel/ipc/alpc: message header, port attributes, type codes"`

---

## 2. ALPC_PORT Object & Object Manager Registration `[Sonnet]`

### 2.1 ALPC_PORT struct

- [ ] Define in `include/kernel/ipc/alpc_port.h`:
  ```c
  typedef enum {
      AlpcServerConnectionPort,   /* named, listens for connections */
      AlpcClientCommunicationPort,/* unnamed, client's end after connect */
      AlpcServerCommunicationPort /* unnamed, server's end after accept */
  } ALPC_PORT_TYPE;

  typedef struct ALPC_PORT {
      OBJECT_HEADER         Header;       /* MUST be first — Ob prefix */
      ALPC_PORT_TYPE        PortType;
      struct ALPC_PORT     *ConnectedPort; /* peer port (for comm ports) */
      struct ALPC_PORT     *ConnectionPort;/* server connection port (for comm ports) */
      spinlock_t            Lock;
      list_head_t           MessageQueue;  /* pending PORT_MESSAGE_ENTRY items */
      list_head_t           PendingQueue;  /* requests waiting for reply */
      list_head_t           ConnectionQueue; /* connection requests (server port only) */
      uint32_t              QueuedMessages;
      uint64_t              NextMessageId; /* monotonically increasing */
      void                 *PortContext;   /* opaque user data */
      ALPC_PORT_ATTRIBUTES  Attributes;
      struct task          *OwnerTask;     /* process that created/connected */
      wait_queue_t          WaitQueue;     /* threads waiting to receive */
      ACCESS_TOKEN         *ClientToken;   /* captured on accept (server comm port) */
      list_head_t           SectionList;   /* registered ALPC_PORT_SECTION entries */
      bool                  Disconnected;
  } ALPC_PORT;
  ```
- [ ] `PORT_MESSAGE_ENTRY` — message node allocated from pool:
  ```c
  typedef struct {
      list_head_t       Link;
      PORT_MESSAGE      Header;
      struct ALPC_PORT *ReplyPort;  /* which port to send reply to */
      uint64_t          ReplyMessageId; /* MessageId this is a reply for */
      bool              WaitingForReply;/* sender blocked waiting for reply */
      wait_queue_t      ReplySyncWait;  /* sender sleeps here until reply arrives */
      /* uint8_t Body[]; */
  } PORT_MESSAGE_ENTRY;
  ```

### 2.2 Object type registration

- [ ] `AlpcInitialize()` called from Phase 1 kernel init (→ XREF `TODO-01-kernel-init-sequencing.md §3`):
  - `ObCreateObjectType("ALPC Port", sizeof(ALPC_PORT), AlpcPortDelete, ...)`
  - Registers `AlpcPortDelete` as the `DeleteProcedure`; drains queues, disconnects linked port, frees all message entries
- [ ] Named server connection ports live in `\RPC Control\<name>` in the Ob namespace (→ XREF `TODO-03-object-manager.md §4`); `NtAlpcCreatePort` with non-NULL `ObjectAttributes->ObjectName` inserts there
- [ ] Unnamed ports (client + server communication ports) have no namespace entry; accessed only via handle

### 2.3 Commit

- [ ] Commit: `"kernel/ipc/alpc: ALPC_PORT object, ObCreateObjectType, port init"`

---

## 3. Connection State Machine `[Opus]`

### 3.1 NtAlpcCreatePort — server side

- [ ] `NtAlpcCreatePort(PortHandle, ObjectAttributes, PortAttributes)`:
  1. `ObCreateObject(AlpcPortObjectType, attrs, &port)` — allocate + insert in `\RPC Control\` if `ObjectName` non-NULL
  2. Set `port->PortType = AlpcServerConnectionPort`
  3. Copy `PortAttributes` (if non-NULL; defaults if NULL)
  4. Initialise `ConnectionQueue`, `MessageQueue`, `WaitQueue`, `Lock`
  5. `ObInsertObject → handle table → *PortHandle`
- [ ] Security: the DACL on the port object controls which processes may connect; default SD = `(A;;0x1F0001;;;SY)(A;;0x00120001;;;WD)` (Everyone=connect only, System=full)

### 3.2 NtAlpcConnectPort — client side

- [ ] `NtAlpcConnectPort(PortHandle, PortName, ObjAttr, PortAttr, Flags, RequiredServerSid, ConnMsg, BufferLen, SendMsgAttr, RecvMsgAttr, Timeout)`:
  1. `ObReferenceObjectByName(PortName, ...)` → server connection port; returns `STATUS_OBJECT_NAME_NOT_FOUND` if not found
  2. `SeAccessCheck` on server port with `PORT_CONNECT (0x1)` access — deny if client IL < server port IL
  3. If `RequiredServerSid` non-NULL: compare to server port owner SID; deny if mismatch (prevents client from accidentally connecting to a hijacked port)
  4. Allocate `ConnMsg` payload, set `Type=ALPC_MSG_TYPE_CONNECTION_REQUEST`
  5. Allocate client communication port (`AlpcClientCommunicationPort`); set `ConnectionPort` pointer to the server port
  6. Queue connection request entry on `server->ConnectionQueue`; wake server `WaitQueue`
  7. Client blocks on `port->WaitQueue` (interruptible, honours `Timeout`)
  8. Server's `NtAlpcAcceptConnectPort` resumes client (§3.3)
  9. On accept: `*PortHandle` = client comm port handle; return `STATUS_SUCCESS`
  10. On reject: return `STATUS_PORT_CONNECTION_REFUSED`

### 3.3 NtAlpcAcceptConnectPort — server side

- [ ] `NtAlpcAcceptConnectPort(PortHandle, ConnectionPortHandle, Flags, ObjAttr, PortAttr, PortContext, ConnectionRequest, ConnMsgAttr, AcceptConnection)`:
  1. `ObReferenceObjectByHandle(ConnectionPortHandle)` → server connection port
  2. Dequeue the first entry from `server->ConnectionQueue` (or the specific request matched by `ConnectionRequest->MessageId`)
  3. If `AcceptConnection == FALSE`:
     - Send `ALPC_MSG_TYPE_LOST_REPLY` back to the waiting client's `ReplySyncWait`
     - Client's `NtAlpcConnectPort` returns `STATUS_PORT_CONNECTION_REFUSED`
     - Return `STATUS_SUCCESS` (server's decision, not an error)
  4. If `AcceptConnection == TRUE`:
     - Allocate server communication port (`AlpcServerCommunicationPort`)
     - Set `server_comm->ConnectedPort = client_comm_port` (cross-link)
     - Set `client_comm->ConnectedPort = server_comm_port`
     - If `PortContext` non-NULL: `server_comm->PortContext = PortContext`
     - Capture client security token if `SecurityQos.ImpersonationLevel ≥ SecurityIdentification` (§7)
     - `ObInsertObject(server_comm)` → `*PortHandle` (server's new handle)
     - Wake client's `ReplySyncWait` with `STATUS_SUCCESS`
- [ ] Connection queue is FIFO; server processes one connection at a time unless it uses async mode (§5) to drain multiple pending connections

### 3.4 NtAlpcDisconnectPort

- [ ] `NtAlpcDisconnectPort(PortHandle, Flags)`:
  - Sets `port->Disconnected = true`
  - If `ConnectedPort` non-NULL: queue `ALPC_MSG_TYPE_PORT_CLOSED` message to the peer's `MessageQueue`; wake peer's `WaitQueue`
  - Release `ConnectedPort` reference
  - Any threads blocked in `NtAlpcSendWaitReceivePort` on this port are woken with `STATUS_PORT_DISCONNECTED`

### 3.5 Commit

- [ ] Commit: `"kernel/ipc/alpc: connection state machine, NtAlpcCreatePort, Connect, Accept, Disconnect"`

---

## 4. Synchronous Send+Wait+Receive Engine `[Opus]`

### 4.1 Message pool allocator

- [ ] `AlpcAllocateMessage(DataLength)` → `PORT_MESSAGE_ENTRY*`:
  - Body immediately follows the `PORT_MESSAGE_ENTRY` struct in a single `kmalloc` allocation: `sizeof(PORT_MESSAGE_ENTRY) + DataLength`
  - Enforces `ALPC_MAX_ALLOWED_MESSAGE_LENGTH` limit
  - Enforces per-port `Attributes.MaxPoolUsage` accounting; returns `STATUS_INSUFFICIENT_RESOURCES` if exceeded
- [ ] `AlpcFreeMessage(entry)` — decrements pool accounting, calls `kfree`

### 4.2 Core engine: NtAlpcSendWaitReceivePort

- [ ] `NtAlpcSendWaitReceivePort(PortHandle, Flags, SendMsg, SendMsgAttr, RecvMsg, BufferLen, RecvMsgAttr, Timeout)`:

  **Receive-only path** (`SendMsg == NULL` or `ALPC_MSGFLG_SYNC_REQUEST` not set):
  1. Lock `port->Lock`
  2. If `MessageQueue` non-empty: dequeue front entry, copy into `RecvMsg` buffer (bounded by `BufferLen`), fill `Header.ClientId` from `entry->ReplyPort->OwnerTask`, unlock, return `STATUS_SUCCESS`
  3. If empty: unlock; `wait_event_interruptible(&port->WaitQueue,
     !list_empty(&port->MessageQueue))` with `Timeout`; retry from step 1
  4. On `Timeout == 0`: return `STATUS_TIMEOUT` immediately instead of sleeping

  **Send-only datagram** (`ALPC_MSGFLG_SYNC_REQUEST` not set, `SendMsg != NULL`):
  1. Validate `SendMsg->Header.TotalLength ≤ port->Attributes.MaxMessageLength`
  2. `AlpcAllocateMessage(DataLength)` → `entry`
  3. Copy `SendMsg` body into `entry`; assign `entry->Header.MessageId = atomic_inc(&port->NextMessageId)`
  4. Set `entry->WaitingForReply = false`
  5. `spin_lock(&ConnectedPort->Lock)` → enqueue on `ConnectedPort->MessageQueue`
     → `wake_up(&ConnectedPort->WaitQueue)` → `spin_unlock`

  **Synchronous request+wait** (`ALPC_MSGFLG_SYNC_REQUEST` set, `SendMsg != NULL`):
  1. Steps 1–4 from datagram path, but `entry->WaitingForReply = true`
  2. Init `entry->ReplySyncWait`; add `entry` to `port->PendingQueue` keyed on `MessageId`
  3. Enqueue on `ConnectedPort->MessageQueue`; wake server
  4. `wait_event_interruptible(&entry->ReplySyncWait, entry->WaitingForReply == false)`
     — caller blocks until server replies or port is disconnected
  5. On wake: check `entry->Header.Type` — if `PORT_CLOSED`/`CLIENT_DIED` → return `STATUS_PORT_DISCONNECTED`; otherwise copy reply body into `RecvMsg`; `AlpcFreeMessage(entry)`

  **Reply** (`ALPC_MSGFLG_REPLY_MESSAGE` set):
  1. Look up `PendingQueue` entry by `SendMsg->Header.MessageId` on the client's port; return `STATUS_REPLY_MESSAGE_MISMATCH` if not found
  2. Copy reply body into the entry
  3. `entry->WaitingForReply = false`; set `entry->Header.Type = ALPC_MSG_TYPE_REPLY`
  4. `wake_up(&entry->ReplySyncWait)` — unblocks the waiting client

### 4.3 Concurrency guarantees

- [ ] `port->Lock` is a spinlock held only for queue manipulation (enqueue, dequeue, list walks); never held across a sleep — the waiting happens outside the lock
- [ ] `PendingQueue` is per-port and protected by `port->Lock`; entries are uniquely identified by `MessageId` (monotonic u64, never reused within a port's lifetime)
- [ ] Multiple threads may call `NtAlpcSendWaitReceivePort` on the same server port concurrently — each dequeues one message; FIFO order is preserved within the spinlock window; no message is delivered to two threads
- [ ] Port destruction race: `AlpcPortDelete` acquires `port->Lock`, sets `Disconnected=true`, wakes all `WaitQueue` sleepers with `STATUS_PORT_DISCONNECTED` before freeing any memory; sleepers check `Disconnected` flag on wake

### 4.4 Commit

- [ ] Commit: `"kernel/ipc/alpc: synchronous send+wait+receive engine with reply routing"`

---

## 5. Asynchronous Delivery & Completion List `[Opus]`

### 5.1 ALPC completion list

- [ ] `ALPC_COMPLETION_LIST` — lock-free single-producer / multi-consumer ring buffer for async message delivery:
  ```c
  typedef struct {
      uint32_t  TotalSize;
      uint32_t  UserVirtualAddr; /* mapped into server's address space */
      uint32_t  KernelOffset;
      uint32_t  Capacity;        /* number of entries */
      _Atomic(uint32_t) ProducerHead;
      _Atomic(uint32_t) ConsumerHead;
      ALPC_COMPLETION_LIST_ITEM Items[]; /* variable length */
  } ALPC_COMPLETION_LIST;

  typedef struct {
      PORT_MESSAGE *Message;   /* pointer into shared mapping */
      uint64_t      PortContext;
      uint32_t      MessageFlags;
  } ALPC_COMPLETION_LIST_ITEM;
  ```
- [ ] `NtAlpcSetInformation(port, AlpcAssociateCompletionPortInformation,
  {CompletionPort, CompletionKey}, len)` — associates an `IO_COMPLETION_OBJECT` with the port; incoming async messages post a completion packet instead of queuing on `MessageQueue`; integrates with `NtWaitForSingleObject` on the completion port

### 5.2 Async message delivery

- [ ] When the port has an associated completion port: on `AlpcEnqueueMessage`, instead of pushing to `MessageQueue` + waking `WaitQueue`, post a completion packet to the `IO_COMPLETION_OBJECT`: `IoSetIoCompletion(completion_port, completion_key, message_ptr, STATUS_SUCCESS, 0)`
- [ ] Server consumes messages from the completion port via `NtRemoveIoCompletion` / `NtRemoveIoCompletionEx` (standard I/O completion port API, → future TODO for I/O completion ports in `05-storage-filesystems`)
- [ ] Fallback: if no completion port is associated, use the synchronous `MessageQueue` + `WaitQueue` path from §4

### 5.3 Waitable port

- [ ] When `ALPC_PORTFLG_WAITABLE_PORT` is set: port itself is a waitable kernel object; `ObSetObjectWaitable(port)` makes it compatible with `NtWaitForSingleObject`; port is signalled when `MessageQueue` becomes non-empty, cleared when queue drains to empty

### 5.4 Commit

- [ ] Commit: `"kernel/ipc/alpc: async completion list, waitable port, IO_COMPLETION integration"`

---

## 6. Large Data: Port Sections & View Mapping `[Sonnet]`

### 6.1 ALPC_PORT_SECTION

- [ ] `ALPC_PORT_SECTION` node appended to `port->SectionList`:
  ```c
  typedef struct {
      list_head_t           Link;
      ALPC_SECTION_HANDLE   Handle;   /* opaque 64-bit ID, not a kernel HANDLE */
      struct section_object *Section; /* kernel Section object (TODO-03 §7) */
      uint64_t              Size;
      bool                  DeleteOnClose;
  } ALPC_PORT_SECTION;
  ```
- [ ] `NtAlpcCreatePortSection(PortHandle, Flags, SectionHandle, SectionSize, AlpcSectionHandle, ActualSectionSize)`:
  - `ObReferenceObjectByHandle(SectionHandle)` — get existing Section object, OR if `SectionHandle == NULL`: `NtCreateSection` internally to create an anonymous shared-memory section of `SectionSize`
  - Allocate `ALPC_PORT_SECTION`, append to `port->SectionList`, assign unique `AlpcSectionHandle`
- [ ] `NtAlpcDeletePortSection(PortHandle, Flags, SectionHandle)` — remove from list, dereference section object

### 6.2 View mapping

- [ ] `ALPC_DATA_VIEW_ATTR` embedded in send/receive attributes:
  ```c
  typedef struct {
      uint32_t Flags;
      ALPC_SECTION_HANDLE SectionHandle;
      void     *ViewBase;   /* base address in caller's VA space */
      uint64_t  ViewSize;
  } ALPC_DATA_VIEW_ATTR;
  ```
- [ ] `NtAlpcCreateSectionView(PortHandle, Flags, DataView)` — maps the registered section into the calling process's VA space (`NtMapViewOfSection`); `DataView->ViewBase` and `DataView->ViewSize` are filled in
- [ ] `NtAlpcDeleteSectionView(PortHandle, Flags, ViewBase)` — unmaps the view
- [ ] Send path with view: caller fills `ALPC_DATA_VIEW_ATTR`, includes it as a message attribute; kernel copies the `ALPC_DATA_VIEW_ATTR` metadata into the queued message entry; receiver maps the same section on its side using `NtAlpcCreateSectionView` with the received `SectionHandle`

### 6.3 Message attributes dispatcher

- [ ] `ALPC_MESSAGE_ATTRIBUTES` struct passed to send/receive:
  - Bitfield `ValidAttributes` selects which attribute structs are present
  - `ALPC_DATA_VIEW_ATTR` (bit 0x1) — view for large data
  - `ALPC_CONTEXT_ATTR` (bit 0x2) — port/message context values
  - `ALPC_HANDLE_ATTR` (bit 0x4) — handle duplication across the port
  - `ALPC_SECURITY_ATTR` (bit 0x8) — security context (for §7)
- [ ] `AlpcpValidateMessageAttributes(attrs, buffer_len)` — bounds-check all present attribute structs against `buffer_len` before use

### 6.4 Commit

- [ ] Commit: `"kernel/ipc/alpc: port sections, view mapping, message attributes dispatch"`

---

## 7. Security: Client Token Capture & Impersonation `[Opus]`

### 7.1 Security context capture at accept

- [ ] In `NtAlpcAcceptConnectPort` (§3.3), if `AcceptConnection == TRUE` AND `port->Attributes.SecurityQos.ImpersonationLevel ≥ SecurityIdentification`:
  1. Locate the client task from `connection_request->Header.ClientId`
  2. `PsReferencePrimaryToken(client_task)` → `client_token`
  3. `NtDuplicateToken(client_token, TOKEN_QUERY | TOKEN_IMPERSONATE, NULL, SecurityQos.EffectiveOnly, TokenImpersonation, &captured_token)` — duplicate at the requested impersonation level; `EffectiveOnly` strips disabled attrs
  4. Store `captured_token` in `server_comm_port->ClientToken`
  5. `PsDereferencePrimaryToken(client_token)`

### 7.2 Server impersonation

- [ ] `NtAlpcImpersonateClientOfPort(PortHandle, Message, Reserved)`:
  1. `ObReferenceObjectByHandle(PortHandle)` → must be a server communication port
  2. Retrieve `port->ClientToken`; check it is not NULL
  3. `SeImpersonateClientEx(port->ClientToken, current_task)` — sets `current_task->ImpersonationToken = port->ClientToken` (ref-counted copy) (→ XREF `TODO-11-security-reference-monitor.md §7.3`)
  4. Requires `SeImpersonatePrivilege` on the server's token if the client's IL is higher than the server's IL (→ XREF `TODO-11-security-reference-monitor.md §8`)

### 7.3 Security attribute in message

- [ ] `ALPC_SECURITY_ATTR` (bit 0x8 in `ValidAttributes`):
  - On send: client includes its `SECURITY_QUALITY_OF_SERVICE` preferences
  - On receive: server reads the `ContextHandle` to call `NtAlpcImpersonateClientOfPort` without needing a separate call
- [ ] `NtAlpcCreateSecurityContext(PortHandle, Flags, SecurityAttribute)` — pre-creates a security context handle to avoid repeated per-message token capture; stored in `ALPC_SECURITY_ATTR.ContextHandle`
- [ ] `NtAlpcDeleteSecurityContext(PortHandle, Flags, ContextHandle)` — revokes and frees the captured context

### 7.4 Client connection SID verification

- [ ] `RequiredServerSid` parameter to `NtAlpcConnectPort`: if non-NULL, kernel reads the server port owner's `UserSid` from `port->ClientToken` (set when server was created) and compares it using `RtlEqualSid`; returns `STATUS_SERVER_SID_MISMATCH` if different; prevents clients from connecting to hijacked ports

### 7.5 Commit

- [ ] Commit: `"kernel/ipc/alpc: client token capture, NtAlpcImpersonateClientOfPort, SID verification"`

---

## 8. NtAlpc* Syscall Table Wiring & Query/Set `[Sonnet]`

### 8.1 SSDT entries

- [ ] Add the following NtAlpc* entry points to the SSDT (→ XREF `TODO-05 §4`):
  ```
  NtAlpcCreatePort
  NtAlpcConnectPort / NtAlpcConnectPortEx (extended variant)
  NtAlpcAcceptConnectPort
  NtAlpcSendWaitReceivePort
  NtAlpcDisconnectPort
  NtAlpcQueryInformation
  NtAlpcSetInformation
  NtAlpcCreatePortSection
  NtAlpcDeletePortSection
  NtAlpcCreateSectionView     (NtAlpcCreateResourceReserve in newer NT)
  NtAlpcDeleteSectionView
  NtAlpcCreateSecurityContext
  NtAlpcDeleteSecurityContext
  NtAlpcImpersonateClientOfPort
  NtAlpcCancelMessage
  NtAlpcQueryInformationMessage
  ```
- [ ] Add corresponding `ZwAlpc*` aliases in `include/kernel/ipc/alpc_syscalls.h`

### 8.2 NtAlpcQueryInformation

- [ ] `AlpcPortInformationClass` values:
  - `AlpcBasicInformation` (0) → `{Flags, SequenceNo, PortContext}`
  - `AlpcPortAssociateCompletionPort` (4) → read back associated completion port
  - `AlpcServerInformation` (8) → `{ThreadBlocked, ConnectedProcessId, ConnectionNtPath}`
  - `AlpcMessagePendingInformation` (9) → pending message count + attributes
- [ ] Return `STATUS_INVALID_INFO_CLASS` for unknown classes

### 8.3 NtAlpcSetInformation

- [ ] `AlpcPortAssociateCompletionPortInformation` (2) — set completion port (§5.1)
- [ ] `AlpcBasicInformation` (0) — update `MaxMessageLength`, `MemoryBandwidth` (only if no messages are pending)
- [ ] `AlpcDirectMessageAttribute` (3) — set default message type for datagrams

### 8.4 NtAlpcCancelMessage

- [ ] `NtAlpcCancelMessage(PortHandle, Flags, MessageContext)`:
  - Find entry on `port->PendingQueue` or `port->MessageQueue` matching `MessageContext->MessageId`
  - Remove from queue; wake any blocked sender with `STATUS_CANCELLED`
  - Used by timeout path in `NtAlpcConnectPort` and `NtAlpcSendWaitReceivePort`

### 8.5 Commit

- [ ] Commit: `"kernel/ipc/alpc: NtAlpc* SSDT wiring, QueryInformation, SetInformation, CancelMessage"`

---

## 9. CSRSS ApiPort Bootstrap `[Opus]`

### 9.1 CSRSS as the first ALPC server

- [ ] CSRSS (`src/apps/csrss/csrss.c`) — the Win32 subsystem server process (→ XREF `11-user-platform-sdk/TODO-05` *(planned)*); it is the first real user-mode process that uses ALPC; this section defines only the kernel-side bootstrap contract
- [ ] On kernel init Phase 3 (→ XREF `TODO-01-kernel-init-sequencing.md §5`): spawn CSRSS as a `SYSTEM`-token process before any other user processes; CSRSS calls:
  ```c
  NtAlpcCreatePort(&ApiPort,
      &ObjAttr(L"\\Windows\\ApiPort"),
      &PortAttrs{ .MaxMessageLength = 512,
                  .Flags = ALPC_PORTFLG_SYSTEM_PROCESS });
  ```
- [ ] `\Windows\ApiPort` must be resolvable in the Ob namespace (→ XREF `TODO-03-object-manager.md §4`); add `\Windows\` directory creation to Phase 1 Ob init

### 9.2 Win32 process creation notification

- [ ] Every new process calls `NtAlpcConnectPort(L"\\Windows\\ApiPort", ...)` at startup in its CRT0 path (→ XREF `11-user-platform-sdk/TODO-04 §5` *(planned)*); sends `CsrClientConnectToServer` message with `{ProcessId, ThreadId, WindowsVersion, SubsystemType=IMAGE_SUBSYSTEM_WINDOWS_GUI/CUI}`
- [ ] CSRSS accepts, allocates a `CSR_PROCESS` record, stores `PortContext`, and replies with a session ID and the address of the CSR shared section
- [ ] From this point, Win32 console I/O, `CreateProcess`, `CreateThread`, and exception notification all flow through this ALPC connection

### 9.3 Message format compatibility

- [ ] CSRSS message format uses a fixed 32-byte opcode area before the variable payload — matches the NT 5.x LPC message format for compatibility with the existing ntdll stubs that will be ported from the Win32 layer:
  ```c
  typedef struct {
      PORT_MESSAGE Header;
      uint32_t     ApiNumber;    /* CSRSS API index */
      NTSTATUS     ReturnValue;
      uint32_t     Reserved;
      uint8_t      Payload[CSRSS_MAX_PAYLOAD]; /* up to 476 bytes */
  } CSRSS_API_MSG;
  ```

### 9.4 Commit

- [ ] Commit: `"kernel/ipc/alpc: CSRSS ApiPort bootstrap, Win32 process registration protocol"`

---

## 10. Live Port Monitor (`alpcmon.exe`) `[Sonnet]`

### 10.1 Kernel query API

- [ ] `NtQuerySystemInformation(SystemAlpcPortInformation, ...)`:
  - Walk all named ports in `\RPC Control\` and `\Windows\` Ob directories
  - Per-port: name, owner PID, pending message count, pending connection count, connected ports count, max message length, total bytes sent/received
  - Return as array of `SYSTEM_ALPC_PORT_INFORMATION` structs

### 10.2 alpcmon.exe app

- [ ] `src/apps/alpcmon/alpcmon.c` — live view of active ALPC ports:
  - Refreshes every 1 s via `NtQuerySystemInformation` poll
  - Table view: Port Name | Owner PID | Owner Image | Pending Messages | Connected Clients | Msg/s | Bytes/s
  - Click row → detail pane: full path, creation time, security descriptor (SDDL string), message history (last 16 message IDs + types)
  - Right-click → "Disconnect port" (requires `SeDebugPrivilege`)
  - Useful during CSRSS and RPC service development to verify ports are created and messages are flowing

### 10.3 Commit

- [ ] Commit: `"kernel/ipc/alpc: alpcmon.exe live port monitor"`

---

## OS Comparison


| ⭐ | Feature                              | Win11              | Linux                                       | Impossible OS                 |
|----|--------------------------------------|--------------------|---------------------------------------------|-------------------------------|
| 💎 | Connection-oriented message ports    | ✅ ALPC            | ⚠️ Unix sockets (SOCK_SEQPACKET)            | ⬜ §2–§3                      |
| 💎 | Synchronous send+wait+reply          | ✅ Full            | ⚠️ SOCK_SEQPACKET (no typed reply)          | ⬜ §4                         |
| 💎 | Async delivery with completion ports | ✅ Full            | ⚠️ `io_uring` (kernel version only)         | ⬜ §5                         |
| 💎 | Large data via shared section        | ✅ Port sections   | ⚠️ Manual `mmap` (no transport integration) | ⬜ §6                         |
| 💎 | Client identity capture              | ✅ Full            | ❌ Not available                            | ⬜ §7                         |
| 💎 | Named port namespace                 | ✅ Full            | ⚠️ Abstract socket namespace                | ⬜ §2                         |
| 💎 | Win32 CSRSS subsystem server         | ✅ Full            | ❌ Not applicable                           | ⬜ §9                         |
| 💎 | Handle duplication across port       | ✅ Full            | ❌ Not available                            | ⬜ §6 — .3 (ALPC_HANDLE_ATTR) |
| 💎 | Connection SID verification          | ✅ Full            | ❌ Not available                            | ⬜ §7 — .4                    |
| ⭐ | Live port monitor with msg/s stats   | ❌ WinObj/WPA only | ❌ Not available                            | ⬜ §10 — 🚀                   |

After §1–9, Impossible OS reaches full Windows 11 ALPC parity — the only kernel IPC mechanism capable of hosting CSRSS, RPC local transport, COM local activation, and the rest of the Win32 subsystem server ecosystem. Linux's closest equivalent (Unix domain sockets with `SOCK_SEQPACKET`) lacks typed reply routing, integrated impersonation, and section-based zero-copy data transfer. The live port monitor (§10) gives developers a real-time view of all active message ports with throughput stats — a developer-experience exclusive that neither Windows (WinObj is read-only) nor Linux ship in their default tooling.

---

## Unit Tests

> Wire into `test_runner_init()` via `test_register_alpc()` (→ XREF: `00-infrastructure/TODO-03 §1`).
> Boot tests run with `debug=1` or `test=1` in boot.conf.

- [ ] Create `src/kernel/test/test_alpc.c` with:
  - Port create: `NtCreatePort` returns valid handle, port object in namespace
  - Port connect: client `NtConnectPort` to server → connection handle valid on both sides
  - Send/receive: client sends 64-byte message → server `NtReplyWaitReceivePort` receives matching data
  - Reply: server replies → client receives reply with correct data
  - Max message size enforced: message exceeding `MaxMessageLength` rejected
  - Port close: closing server port → pending client receives `STATUS_PORT_DISCONNECTED`
  - Multiple clients: 3 clients connect to 1 server, each gets independent connection
  - View section: `NtCreateSection` + attach to port → shared memory accessible from both sides
- [ ] Register in `test_runner_init()`: `test_register_alpc()`
- [ ] Commit: `"test: add ALPC message port test suite"`

---

## Verification

- [ ] **Unit test — connection handshake**: two kernel tasks; task A creates `\RPC Control\TestPort`; task B calls `NtAlpcConnectPort`; task A calls `NtAlpcAcceptConnectPort`; verify both ends hold valid port handles; verify `NtAlpcDisconnectPort` sends `PORT_CLOSED` to peer.
- [ ] **Unit test — sync send+reply**: task B sends a 64-byte request and blocks; task A receives it, sends reply; task B unblocks with the reply body intact; verify `MessageId` round-trip and `ClientId` is correctly filled.
- [ ] **Unit test — concurrent receivers**: 4 server threads all call `NtAlpcSendWaitReceivePort`; send 100 messages from one client; verify each message is delivered exactly once and total received count == 100.
- [ ] **Unit test — port section large transfer**: register a 4 MiB section, map it, write a pattern, send; receiver maps same section, reads back pattern; verify byte-for-byte match without any `kmalloc` for the message body.
- [ ] **CSRSS boot test in QEMU**: kernel spawns CSRSS; a test `hello.exe` connects to `\Windows\ApiPort`; CSRSS logs the connection and replies; verify `hello.exe` receives session ID; `alpcmon.exe` shows `\Windows\ApiPort` with 1 connected client.
- [ ] **Remaining limits**: I/O completion port integration (§5) requires `NtCreateIoCompletion` / `NtRemoveIoCompletion` from `05-storage-filesystems`; handle duplication attribute (`ALPC_HANDLE_ATTR`) deferred to after `NtDuplicateObject` is stable (→ `TODO-03-object-manager.md §9`); ALPC debugging/tracing via ETW deferred to `09-services-security`.
- [ ] Commit: `"kernel/ipc/alpc: ALPC complete — port objects, connection handshake, sync send+reply, async completion, port sections, security impersonation, CSRSS ApiPort, alpcmon"`
