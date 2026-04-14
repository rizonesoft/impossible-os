# TODO-12 -- ALPC / Message Ports

> **Goal:** Implement Advanced Local Procedure Call (ALPC), the kernel's connection-oriented message-passing substrate. ALPC gives every process pair a typed, reference-counted port object, a three-way connection handshake (server create → client connect → server accept), synchronous send+wait+reply semantics, asynchronous delivery via completion lists, and optional large-data transfer through mapped port sections. The Win32 subsystem server (CSRSS), the RPC local transport, COM local activation, and every NT service that talks back to a client process are all built on top of ALPC. The existing IPC layer (pipes, shared memory, signals) cannot substitute for it because it has no connection-oriented reply semantics: a server cannot wait for exactly one client's reply and route it back to the right caller. Without ALPC, the Win32 subsystem server model is impossible to build.

> [!IMPORTANT]
> **Current state:** `src/kernel/ipc/` has pipes (`pipe.c`), shared memory (`shmem.c`), and signal delivery (`signal.c`), none of which are managed by the Object Manager. There is **no** `ALPC_PORT` object, **no** connection queues, and **no** synchronous request/reply engine yet. **`src/kernel/nt/nt_alpc.c`** already registers SSDT slots **0x010F-0x011E** with **`STATUS_NOT_IMPLEMENTED`** stub bodies and a one-time klog warning per syscall (`ALPC_STUB_BODY`); `src/kernel/test/test_ob.c` asserts those slots exist and return the deferred status. CSRSS cannot be started until this TODO replaces stubs with the real ALPC engine and Ob-backed port objects.

---

## Inputs

- `src/kernel/ipc/` -- existing pipe, shm, signal primitives (context only)
- `include/kernel/ipc/` -- current IPC headers
- `src/kernel/nt/nt_alpc.c` -- SSDT stub table for `NtAlpc*` (deferred-status placeholders until §8-§9 retrofit)
- `include/kernel/nt/nt_alpc.h` -- syscall indices / prototypes for ALPC handlers
- `src/kernel/test/test_ob.c` -- `TEST_CAT_OB` tests that ALPC SSDT slots register and return `STATUS_NOT_IMPLEMENTED`
- `src/kernel/sched/task.c` -- `struct task`, wait/wake primitives
- `include/kernel/ipc/alpc.h` (planned; first created in §1)
- `include/kernel/ipc/alpc_port.h` (planned; first created in §2)
- `include/kernel/ipc/alpc_syscalls.h` (planned; first created in §8)
- `src/apps/csrss/csrss.c` (planned; §10 kernel-side bootstrap contract)
- `src/apps/alpcmon/alpcmon.c` (planned; §12 profiler UI)
- → XREF: `TODO-03-object-manager.md §1-§4`: ALPC ports are `OBJECT_TYPE` kernel objects with handles, reference counts, and named entries in `\RPC Control\`
- → XREF: `TODO-03-object-manager.md §7`: `NtAlpcCreatePortSection` registers a Section object with the port; section creation depends on the Section object type
- → XREF: `TODO-05-native-api-ssdt.md §4`: all `NtAlpc*` entry points are SSDT slots; wiring happens after the SSDT exists
- → XREF: `TODO-06-irql-model-dpcs.md §3`: message delivery runs at `DISPATCH_LEVEL` briefly when queuing to the server's message queue; IRQL discipline applies
- → XREF: `TODO-11-security-reference-monitor.md §4-§7`: client security context capture requires `ACCESS_TOKEN`; server impersonating a client requires `SeImpersonatePrivilege`
- → XREF: `12-user-platform-sdk/TODO-05-win32-subsystem.md`: CSRSS (Win32 subsystem server) creates its `ApiPort` using the bootstrap path defined in §10 of this TODO
- → XREF: `03-memory-concurrency/TODO-08-win32-ipc-extensions.md §7`: LPC compatibility syscalls (`NtCreatePort`, `NtConnectPort`, `NtReplyWaitReceivePort`) redirect to ALPC internally; depends on this TODO's `ALPC_PORT` and `NtAlpc*` surface
- → XREF: `03-memory-concurrency/TODO-08-win32-ipc-extensions.md §8`: incremental Win32 ALPC extensions (handle-attribute cross-process dup, direct/indirect mode) build on top of this TODO's `ALPC_PORT` object and `NtAlpc*` syscall surface; TODO-08 §8 must not re-implement core ALPC primitives

---

## Outcome

- `ALPC_PORT` kernel object type registered via Object Manager with three subtypes: server connection port, client port, server communication port.
- Named server ports appear in `\RPC Control\<name>` in the kernel namespace.
- `NtAlpcCreatePort` / `NtAlpcConnectPort` / `NtAlpcAcceptConnectPort` implement the full three-way connection handshake.
- `NtAlpcSendWaitReceivePort` handles synchronous send+wait, reply, and receive-only in a single entry point.
- Asynchronous message delivery via `ALPC_COMPLETION_LIST` integrates with `NtWaitForSingleObject` and I/O completion ports.
- Large data (> 512 bytes) transfers through port sections without copying.
- Server can capture and impersonate the client's security token.
- Per-message sender SID and token-modified-ID queries via `NtAlpcQueryInformationMessage`.
- Optional Win11 parity exports: `NtAlpcOpenSenderProcess`, `NtAlpcOpenSenderThread`, and `NtAlpcRevokeSecurityContext` (or documented alias) for CSRSS and security tooling parity.
- Message zones provide pre-allocated buffer pools for high-throughput ports, eliminating per-message `kmalloc` on the hot path.
- CSRSS `ApiPort` boots and accepts the first Win32 subsystem connections.
- `alpcmon.exe` provides a live port monitor with per-port latency histograms (P50/P99); a developer-experience exclusive.

---

## Implementation Order

| ⭐  | Order | Deliverable                                       | Depends On         | Status |
| --- | :---: | ------------------------------------------------- | ------------------ | :----: |
| 💎  |   1   | Message header, port attributes, type codes       | (none)              |  [x]   |
| 💎  |   2   | ALPC_PORT object & Object Manager registration    | §1, T03 §1-§4     |  [x]   |
| 💎  |   3   | Connection state machine (create/connect/accept)  | §2, T05 §4        |  [ ]   |
| 💎  |   4   | Synchronous send+wait+receive engine              | §3, T06 §3        |  [ ]   |
| 💎  |   5   | Asynchronous delivery & completion list           | §4                 |  [ ]   |
| 💎  |   6   | Large data: port sections & view mapping          | §2, T03 §7        |  [ ]   |
| 💎  |   7   | Security: client token capture & impersonation    | §4, T11 §4-§7     |  [ ]   |
| 💎  |   8   | NtAlpc* SSDT registration & stub retrofit           | §1-§7, T05 §4     |  [ ]   |
| 💎  |   9   | NtAlpc QueryInformation / SetInformation / CancelMessage | §8, T05 §4          |  [ ]   |
| 💎  |  10   | CSRSS ApiPort bootstrap                             | §3-§9             |  [ ]   |
| 💎  |  11   | Message zones (pre-allocated message buffers)     | §4, §8-§9         |  [ ]   |
| ⭐  |  12   | Live port monitor & IPC latency profiler          | §8-§9             |  [ ]   |

> 💎 = parity work; matches what Windows 11 and Linux already do.
> ⭐ = exclusive work; Impossible OS is superior or first.

---

## 1. Message Header, Port Attributes & Type Codes

Define the on-the-wire ALPC ABI in `include/kernel/ipc/alpc.h`. Field names follow the Windows ALPC convention (winternl.h `PORT_MESSAGE`, MSDN ALPC reference) for source-level familiarity; wire layout uses Impossible OS canonical types: 16-byte `CLIENT_ID` sourced from `kernel/ob/teb.h` (two uint64 UniqueProcess/UniqueThread), `SECURITY_IMPERSONATION_LEVEL` sourced from `kernel/security/token.h`. Not binary-compatible with Windows x64 (which has a 48-byte `PORT_MESSAGE`), but structurally shared types reuse the kernel's canonical definitions to avoid type-collision traps across translation units. Compile-time `_Static_assert` lines lock every ABI-visible sizeof and field offset.

- [x] Reuse canonical `CLIENT_ID` (16 bytes, `UniqueProcess`/`UniqueThread` uint64) from `kernel/ob/teb.h`. Re-assert sizeof + field offsets in `alpc.h` so any drift in the canonical header trips here too.
- [x] Define `PORT_MESSAGE` (40 bytes): `TotalLength`/`DataLength`/`Type`/`DataInfoOffset` (uint16 packed at offset 0..7), `ClientId` (CLIENT_ID at offset 8, 16 bytes), `MessageId` (uint64 at offset 24), `CallbackId` (uint64 at offset 32). `_Static_assert(sizeof(PORT_MESSAGE) == 40)` + per-field offset asserts.
- [x] Define `ALPC_MESSAGE` wrapper: `{PORT_MESSAGE Header; uint8_t Body[];}` -- flexible array; body length carried by `Header.DataLength`. Offset assert: `Body == sizeof(PORT_MESSAGE)`.
- [x] Define 10 `ALPC_MSG_TYPE_*` constants: `REQUEST=1`, `REPLY=2`, `DATAGRAM=3`, `LOST_REPLY=4`, `PORT_CLOSED=5`, `CLIENT_DIED=6`, `EXCEPTION=7`, `DEBUG_EVENT=8`, `ERROR_EVENT=9`, `CONNECTION_REQUEST=10`.
- [x] Define `ALPC_MAX_ALLOWED_MESSAGE_LENGTH = 65528` (kernel rejects larger inline messages). Recommended port-section threshold = 512 bytes documented in header comment.
- [x] Define `SECURITY_QUALITY_OF_SERVICE` (12 bytes, sizeof asserted): `Length` (uint32), `ImpersonationLevel` (canonical `SECURITY_IMPERSONATION_LEVEL` from `kernel/security/token.h`, re-used not redefined), `ContextTrackingMode` (uint8 STATIC=0/DYNAMIC=1), `EffectiveOnly` (uint8), 2-byte trailing pad.
- [x] Define `ALPC_PORT_ATTRIBUTES` (80 bytes, sizeof + every external offset asserted): `Flags` (uint32), explicit 4-byte `Pad0`, `SecurityQos` (SECURITY_QUALITY_OF_SERVICE), explicit 4-byte `Pad1`, six uint64 fields (MaxMessageLength/MemoryBandwidth/MaxPoolUsage/MaxSectionSize/MaxViewSize/MaxTotalSectionSize), `DupObjectTypes` (uint32), trailing `Pad2`. Explicit pad fields make layout drift-proof.
- [x] Define 4 `ALPC_PORTFLG_*` constants: `LPC_MODE=0x20000`, `WAITABLE_PORT=0x40000`, `ALLOW_DUP_OBJECT=0x80000`, `SYSTEM_PROCESS=0x100000`. `_Static_assert` pairwise non-overlap (OR == SUM).
- [x] Define 6 `ALPC_MSGFLG_*` send-flag constants for `NtAlpcSendWaitReceivePort`: `REPLY_MESSAGE=0x1`, `LPC_MODE=0x2`, `RELEASE_MESSAGE=0x10`, `SYNC_REQUEST=0x20000`, `WAIT_USER_MODE=0x100000`, `WAIT_PENDING_CALLBACKS=0x200000`. `_Static_assert` pairwise non-overlap.
- [x] Update `pipe_init()` to return `boot_result_t` instead of `void` -- migration item carried from TODO-01 §8. Idempotent: returns `BOOT_OK`; all callers converted to `(void)pipe_init()`.
- [x] Add `alpc_init()` (in `src/kernel/ipc/alpc.c`) emitting `"[alpc] header constants defined"` to klog. Wire into `boot_phase3()` alongside explicit `pipe_init()` call.
- [x] Commit: `"kernel/ipc/alpc: message header, port attributes, type codes"`

**Test checkpoint:** `_Static_assert` lines compile (sizeof PORT_MESSAGE == 40, sizeof SECURITY_QUALITY_OF_SERVICE == 12, sizeof ALPC_PORT_ATTRIBUTES == 80, every field offset correct). Unit test `test_alpc_*` (7 suites under `TEST_CAT_IPC` in `src/kernel/test/test_alpc.c`) verifies: PORT_MESSAGE layout, message-type uniqueness (1..10), `ALPC_MAX_ALLOWED_MESSAGE_LENGTH == 65528`, `ALPC_PORTFLG_*` non-overlap, `ALPC_MSGFLG_*` non-overlap, SQOS layout, PORT_ATTRIBUTES layout. Serial log on init: `[alpc] header constants defined`. Platforms: QEMU WHPX + TCG (header compiles + unit tests run); bare metal + VirtualBox sweep: header is pure compile-time, runtime risk nil.

> **Test runner:** `scripts\debug\run-ipc-tests.bat` (SUITE=ipc)
> **Expected:** 7 alpc suites pass, 0 failures

> **Verified:** 2026-04-14 -- Evidence mapped: every `[x]` item backed by `include/kernel/ipc/alpc.h` (CLIENT_ID asserts, PORT_MESSAGE 40B, SQOS 12B, ALPC_PORT_ATTRIBUTES 80B, 10 msg-type codes, 4 PORTFLG + 6 MSGFLG non-overlap asserts, `alpc_init` proto), `src/kernel/ipc/alpc.c` (klog line), `src/kernel/test/test_alpc.c` (7 suites TEST_CAT_IPC), `src/kernel/main/boot_desktop.c:101-102` (wiring). No scope gaps; no `TODO/FIXME/HACK/STATUS_NOT_IMPLEMENTED` in delta. Build clean (`=== BUILD OK ===`). Test checkpoint serial string `[alpc] header constants defined` confirmed at `src/kernel/ipc/alpc.c:18`.
> **Quality reviewed:** 2026-04-14 -- Two Codex dispatches (step-5 adversarial + step-8 quality/dead-code/consistency/perf). Four Highs fixed: (1) ABI-comment overclaim of Windows winternl.h binary compat softened to explicit Impossible-OS-native layout; (2) `ALPC_PORT_ATTRIBUTES` sizeof + every external offset locked with explicit `Pad0/Pad1/Pad2` fields (80B); (3) duplicate `CLIENT_ID` collision with `kernel/ob/teb.h` fixed by reusing the canonical 16-byte type + re-asserting sizeof/offsets locally; (4) duplicate `SECURITY_IMPERSONATION_LEVEL` collision with `kernel/security/token.h` fixed by reusing canonical enum. PORT_MESSAGE offsets shifted: MessageId 16 -> 24, CallbackId 24 -> 32; trailing Reserved u64 removed (16-byte CLIENT_ID fills the gap). Coexistence compile-proof: `alpc.h` now includes both `ob/teb.h` and `security/token.h`; full-tree build passes. Tests + TODO spec + kernel-code-quality gates (Gates 1-11) all walked. No dead code. No perf concerns (header-only + one klog line).
> **Test runner:** `scripts\debug\run-ipc-tests.bat` (SUITE=ipc), 7 alpc suites, 0 failures expected


## 2. ALPC_PORT Object & Object Manager Registration

Register the `ALPC Port` kernel object type with the Object Manager, create the `\RPC Control` namespace directory, and expose a kernel-side `AlpcCreatePort()` helper that the NT syscall dispatcher and in-kernel servers build on top of. The port body is Ob-prefix-conformant -- it ships as a plain body, and the `OBJECT_HEADER` sits at a negative offset (`OB_HEADER_FROM_BODY`). All queue and sync state is initialised empty; real message flow arrives in §3-§5.

- [x] Define `ALPC_PORT_TYPE` enum in `include/kernel/ipc/alpc_port.h` (3 values): `AlpcServerConnectionPort` (named, listens), `AlpcClientCommunicationPort` (unnamed, client's end after connect), `AlpcServerCommunicationPort` (unnamed, server's end after accept).
- [x] Define `struct alpc_port` body in `include/kernel/ipc/alpc_port.h`. Fields: `PortType` (`ALPC_PORT_TYPE`), `ConnectedPort`/`ConnectionPort` (`struct alpc_port *`), `Lock` (`spinlock_t`), three intrusive singly-linked queue heads (`MessageQueue`, `PendingQueue`, `ConnectionQueue` -- each an `ALPC_MSG_QUEUE` `{head, tail, count}` of `PORT_MESSAGE_ENTRY *`; the kernel has no generic `list_head_t`), `NextMessageId` (u64), `PortContext` (`void *`), `Attributes` (`ALPC_PORT_ATTRIBUTES` by value, 80B), `OwnerTask` (`struct task *`), `WaitQueue` (`condvar_t`, unused until §4), `ClientToken` (`struct access_token *`, NULL until §7), `SectionList` head/tail/count (empty until §6), `MessageZone` (forward-declared `struct alpc_message_zone *`, NULL until §11), `Stats` (`ALPC_PORT_STATS` inline stub, zero-init until §12), `Disconnected` (u8).
- [x] Define `PORT_MESSAGE_ENTRY` in `include/kernel/ipc/alpc_port.h`. Fields: `Link_next` (`struct port_message_entry *`, intrusive singly-linked-list forward pointer), `Header` (`PORT_MESSAGE` by value, 40B), `ReplyPort` (`struct alpc_port *`), `ReplyMessageId` (u64), `WaitingForReply` (u8), `ReplySyncWait` (`condvar_t`, unused until §4). Inline body follows the struct via hand-laid-out flexible allocation; body length lives in `Header.DataLength`.
- [x] Implement `alpc_port_init()` in `src/kernel/ipc/alpc_port.c`. Registers `ObpAlpcPortType` via `ob_create_type(&(OBJECT_TYPE){.name="ALPC Port", .body_size=sizeof(ALPC_PORT), .on_delete=alpc_port_on_delete})`. Creates `\RPC Control` directory under `ObpRootDirectory` via `ob_ns_create_directory` + `ObInsertObject`. Logs `[alpc] ALPC Port type registered` and `[alpc] \RPC Control directory created`. Returns `BOOT_FATAL` on registration failure. Idempotent.
- [x] Implement `alpc_port_on_delete(void *body)` as the type DeleteProcedure. Snapshots queues under the port's own `Lock`, releases lock, then drains (kfree may sleep in debug builds -- never free under a spinlock). Drops ref on `ConnectedPort` if non-NULL. `ClientToken` is NULL today and process-lifetime-owned per token.h; no drop path yet (adds in §7 when token capture ships). `MessageZone` NULL until §11. Idempotent: safe to call on an already-drained port body.
- [x] Implement `HANDLE AlpcCreatePort(HANDLE_TABLE *ht, const char *name, const ALPC_PORT_ATTRIBUTES *attrs)` in `src/kernel/ipc/alpc_port.c`. Kernel-side helper used by both the NT syscall retrofit and in-kernel servers (CSRSS in §10). Path: validate leaf name (rejects `\\`, `/`, empty, > `OB_NAME_MAX`) -> `ob_alloc_object(ObpAlpcPortType)` -> initialise (`PortType=AlpcServerConnectionPort`, `Lock=SPINLOCK_INIT`, queues empty, `OwnerTask=task_current()`, `NextMessageId=1`, `cond_init(WaitQueue)`, `Attributes=*attrs` or zero-init when `attrs==NULL`) -> **allocate handle FIRST** (mirrors `NtCreateDirectoryObject` rollback idiom in `nt_namespace.c`) -> if `name`, resolve `\RPC Control` + `ObInsertObject`. On any failure path: `ObpFreeHandle` (if handle allocated) then drop creation ref. Handle-first ordering guarantees no half-created namespace entries.
- [x] Retrofit `NtAlpcCreatePort_handler` in `src/kernel/nt/nt_alpc.c` to call `AlpcCreatePort`. Interprets `a1` as out-`HANDLE *`, `a2` as `OBJECT_ATTRIBUTES *`, `a3` as `ALPC_PORT_ATTRIBUTES *`. User-mode previous-mode: `ProbeForWriteIfUser` on out-handle, `ProbeForReadIfUser` on port_attrs (single-shot copy-by-value) and via `alpc_probe_and_split` on OA + UNICODE_STRING + bounded buffer. `alpc_probe_and_split` verifies path is rooted at `\RPC Control\`, splits the leaf, and **copies it into a kernel-owned 64-byte buffer** before dereferencing (avoids TOCTOU). Rejects embedded `\\`/`/` in leaf with `STATUS_OBJECT_NAME_INVALID`. Slot `0x010F` skipped in `test_nt_alpc_pending_features` -- it is no longer pending.
- [x] Wire `alpc_port_init()` into `alpc_init()` in `src/kernel/ipc/alpc.c` so the `\RPC Control` directory and `ObpAlpcPortType` come up in the same `boot_phase3` pass. `alpc_init` propagates the `boot_result_t` from `alpc_port_init`.
- [x] Commit: `"kernel/ipc/alpc: ALPC_PORT object, ObCreateObjectType, port init"`

**Test checkpoint:** Unit tests (under `TEST_CAT_IPC` in `src/kernel/test/test_alpc.c`): (a) `ObpAlpcPortType` non-NULL after `alpc_init`; (b) `\RPC Control` directory resolvable via `ObLookupObjectByName("\\RPC Control", ObpDirectoryType, ...)`; (c) `AlpcCreatePort(ht, "TestPort", NULL)` returns a valid non-zero handle; (d) the port is findable via `ObLookupObjectByName("\\RPC Control\\TestPort", ObpAlpcPortType, ...)`; (e) `NtClose` on the handle plus a matching deref drops `ref_count` to 0 and invokes `alpc_port_on_delete`; (f) `AlpcCreatePort(ht, NULL, NULL)` creates an unnamed port (handle-only, no namespace entry). Serial log on init: `[alpc] ALPC Port type registered`, `[alpc] \RPC Control directory created`. Platforms: QEMU WHPX + TCG; bare metal + VirtualBox sweep via the IPC test suite (pure kernel-heap + Ob, no hardware path).

> **Test runner:** `scripts\debug\run-ipc-tests.bat` (SUITE=ipc)
> **Expected:** 12 new alpc-port suites pass, 0 failures

> **Verified:** 2026-04-15 -- Evidence mapped: every `[x]` item backed by `include/kernel/ipc/alpc_port.h` (ALPC_PORT_TYPE, ALPC_PORT body, PORT_MESSAGE_ENTRY, ALPC_MSG_QUEUE, ALPC_PORT_STATS, public API), `src/kernel/ipc/alpc_port.c` (`alpc_port_init`, `alpc_port_on_delete`, `alpc_validate_attrs`, `AlpcCreatePort`), `src/kernel/nt/nt_alpc.c` (`alpc_probe_and_split` + retrofit), `src/kernel/ipc/alpc.c` (alpc_init chain), `src/kernel/test/test_alpc.c` (12 suites). No scope gaps (only `SCOPE-GAP-ALLOWED` on the 15 remaining NtAlpc* stubs, which TODO-12 §8 owns). Build clean (`=== BUILD OK ===`). Test checkpoint strings `[alpc] ALPC Port type registered` and `[alpc] \RPC Control directory created` confirmed at `src/kernel/ipc/alpc_port.c:122` and `:142`.
> **Quality reviewed:** 2026-04-15 -- kernel-code-quality Gates 1-11 walked clean. Codex quality dispatch (dead-code + consistency + perf) returned 4 findings: (1) HIGH rejected -- probe+direct-deref is the kernel-wide pattern (`nt_namespace.c:37`, `nt_section.c:39`, `nt_timer.c`, `nt_file.c`) and SMAP is intentionally disabled per `CLAUDE.md "No SMEP/SMAP until per-process page tables"`; (2) MEDIUM fixed -- added `alpc_validate_attrs` in `alpc_port.c:138` that rejects unknown `ALPC_PORTFLG_*` bits, `ALPC_PORTFLG_SYSTEM_PROCESS` (pending SeAccessCheck at §7), and non-zero reserved pads; (3) MEDIUM fixed -- `AlpcCreatePort` now returns `NTSTATUS` + out-HANDLE so the syscall propagates `STATUS_OBJECT_NAME_COLLISION` for duplicates, `STATUS_OBJECT_NAME_INVALID` for bad leaves, `STATUS_OBJECT_NAME_NOT_FOUND` for missing `\RPC Control`, `STATUS_INVALID_PARAMETER` for bad attrs, `STATUS_INSUFFICIENT_RESOURCES` only for real allocation failure; (4) LOW fixed -- removed the over-promising "static assert on sizeof(alpc_port) catches drift" claim from `alpc_port.h` (internal layout, not ABI). Added 2 new tests (privileged flag rejected + reserved pad rejected) to exercise validation. Accepted: copy_from_user retrofit deferred per CLAUDE.md SMAP gate -> XREF: `03-memory-concurrency/TODO-02 §4` (item: "Audit all syscall handlers" at line 117 -- now names `nt_alpc.c`'s `alpc_probe_and_split` + `NtAlpcCreatePort_handler` as explicit retrofit consumers).
> **Test runner:** `scripts\debug\run-ipc-tests.bat` (SUITE=ipc), 12 alpc-port suites + 7 §1 ABI suites, 0 failures expected

---

## 3. Connection State Machine

- [ ] (3.1 NtAlpcCreatePort -- server side) `NtAlpcCreatePort(PortHandle, ObjectAttributes, PortAttributes)`:
  1. `ObCreateObject(AlpcPortObjectType, attrs, &port)` -- allocate + insert in `\RPC Control\` if `ObjectName` non-NULL
  2. Set `port->PortType = AlpcServerConnectionPort`
  3. Copy `PortAttributes` (if non-NULL; defaults if NULL)
  4. Initialise `ConnectionQueue`, `MessageQueue`, `WaitQueue`, `Lock`
  5. `ObInsertObject → handle table → *PortHandle`
- [ ] Security: the DACL on the port object controls which processes may connect; default SD = `(A;;0x1F0001;;;SY)(A;;0x00120001;;;WD)` (Everyone=connect only, System=full)
- [ ] (3.2 NtAlpcConnectPort -- client side) `NtAlpcConnectPort(PortHandle, PortName, ObjAttr, PortAttr, Flags, RequiredServerSid, ConnMsg, BufferLen, SendMsgAttr, RecvMsgAttr, Timeout)`:
  1. `ObReferenceObjectByName(PortName, ...)` → server connection port; returns `STATUS_OBJECT_NAME_NOT_FOUND` if not found
  2. `SeAccessCheck` on server port with `PORT_CONNECT (0x1)` access -- deny if client IL < server port IL
  3. If `RequiredServerSid` non-NULL: compare to server port owner SID; deny if mismatch (prevents client from accidentally connecting to a hijacked port)
  4. Allocate `ConnMsg` payload, set `Type=ALPC_MSG_TYPE_CONNECTION_REQUEST`
  5. Allocate client communication port (`AlpcClientCommunicationPort`); set `ConnectionPort` pointer to the server port
  6. Queue connection request entry on `server->ConnectionQueue`; wake server `WaitQueue`
  7. Client blocks on `port->WaitQueue` (interruptible, honours `Timeout`)
  8. Server's `NtAlpcAcceptConnectPort` resumes client (§3 accept path)
  9. On accept: `*PortHandle` = client comm port handle; return `STATUS_SUCCESS`
  10. On reject: return `STATUS_PORT_CONNECTION_REFUSED`

- [ ] (3.3 NtAlpcAcceptConnectPort -- server side) `NtAlpcAcceptConnectPort(PortHandle, ConnectionPortHandle, Flags, ObjAttr, PortAttr, PortContext, ConnectionRequest, ConnMsgAttr, AcceptConnection)`:
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
     - Capture client security token if `SecurityQos.ImpersonationLevel >= SecurityIdentification` (§7)
     - `ObInsertObject(server_comm)` → `*PortHandle` (server's new handle)
     - Wake client's `ReplySyncWait` with `STATUS_SUCCESS`
- [ ] Connection queue is FIFO; server processes one connection at a time unless it uses async mode (§5) to drain multiple pending connections
- [ ] (3.4 NtAlpcDisconnectPort) `NtAlpcDisconnectPort(PortHandle, Flags)`:
  - Sets `port->Disconnected = true`
  - If `ConnectedPort` non-NULL: queue `ALPC_MSG_TYPE_PORT_CLOSED` message to the peer's `MessageQueue`; wake peer's `WaitQueue`
  - Release `ConnectedPort` reference
  - Any threads blocked in `NtAlpcSendWaitReceivePort` on this port are woken with `STATUS_PORT_DISCONNECTED`

- [ ] (3.5 Self-contained execution note) Follow the callout on the next lines.
> [!NOTE]
> `SeAccessCheck` (T11 §5) is not yet implemented. §3 (NtAlpcConnectPort) step 2 should stub the access check as always-grant until T11 §5 is complete. Add `// TODO: SeAccessCheck always-grant stub` with a compile-time `#warning` so it's not forgotten.

- [ ] (3.6 Commit) Commit: `"kernel/ipc/alpc: connection state machine, NtAlpcCreatePort, Connect, Accept, Disconnect"`

**Test checkpoint:** Task A creates `\RPC Control\TestPort`. Task B calls `NtAlpcConnectPort("\\RPC Control\\TestPort")`; connection request queued. Task A calls `NtAlpcAcceptConnectPort(TRUE)`; both tasks hold valid port handles. Task A calls `NtAlpcAcceptConnectPort(FALSE)` on a second connection; client gets `STATUS_PORT_CONNECTION_REFUSED`. `NtAlpcDisconnectPort` on server → client receives `ALPC_MSG_TYPE_PORT_CLOSED`. Connecting to non-existent port → `STATUS_OBJECT_NAME_NOT_FOUND`. Serial log: `"[ALPC] connection accepted: client=%u server=%u"`. Test on: QEMU WHPX + TCG.

---

## 4. Synchronous Send+Wait+Receive Engine

- [ ] (4.1 Message pool allocator) `AlpcAllocateMessage(DataLength)` → `PORT_MESSAGE_ENTRY*`:
  - Body immediately follows the `PORT_MESSAGE_ENTRY` struct in a single `kmalloc` allocation: `sizeof(PORT_MESSAGE_ENTRY) + DataLength`
  - Enforces `ALPC_MAX_ALLOWED_MESSAGE_LENGTH` limit
  - Enforces per-port `Attributes.MaxPoolUsage` accounting; returns `STATUS_INSUFFICIENT_RESOURCES` if exceeded
- [ ] `AlpcFreeMessage(entry)` -- decrements pool accounting, calls `kfree`
- [ ] (4.2 Core engine: NtAlpcSendWaitReceivePort) `NtAlpcSendWaitReceivePort(PortHandle, Flags, SendMsg, SendMsgAttr, RecvMsg, BufferLen, RecvMsgAttr, Timeout)`:

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
  1. Steps 1-4 from datagram path, but `entry->WaitingForReply = true`
  2. Init `entry->ReplySyncWait`; add `entry` to `port->PendingQueue` keyed on `MessageId`
  3. Enqueue on `ConnectedPort->MessageQueue`; wake server
  4. `wait_event_interruptible(&entry->ReplySyncWait, entry->WaitingForReply == false)`
     -- caller blocks until server replies or port is disconnected
  5. On wake: check `entry->Header.Type` -- if `PORT_CLOSED`/`CLIENT_DIED` → return `STATUS_PORT_DISCONNECTED`; otherwise copy reply body into `RecvMsg`; `AlpcFreeMessage(entry)`

  **Reply** (`ALPC_MSGFLG_REPLY_MESSAGE` set):
  1. Look up `PendingQueue` entry by `SendMsg->Header.MessageId` on the client's port; return `STATUS_REPLY_MESSAGE_MISMATCH` if not found
  2. Copy reply body into the entry
  3. `entry->WaitingForReply = false`; set `entry->Header.Type = ALPC_MSG_TYPE_REPLY`
  4. `wake_up(&entry->ReplySyncWait)` -- unblocks the waiting client

- [ ] (4.3 Concurrency guarantees) `port->Lock` is a spinlock held only for queue manipulation (enqueue, dequeue, list walks); never held across a sleep -- the waiting happens outside the lock
- [ ] `PendingQueue` is per-port and protected by `port->Lock`; entries are uniquely identified by `MessageId` (monotonic u64, never reused within a port's lifetime)
- [ ] Multiple threads may call `NtAlpcSendWaitReceivePort` on the same server port concurrently -- each dequeues one message; FIFO order is preserved within the spinlock window; no message is delivered to two threads
- [ ] Port destruction race: `AlpcPortDelete` acquires `port->Lock`, sets `Disconnected=true`, wakes all `WaitQueue` sleepers with `STATUS_PORT_DISCONNECTED` before freeing any memory; sleepers check `Disconnected` flag on wake
- [ ] (4.4 Commit) Commit: `"kernel/ipc/alpc: synchronous send+wait+receive engine with reply routing"`

**Test checkpoint:** Client sends 64-byte request with `ALPC_MSGFLG_SYNC_REQUEST` → client blocks. Server calls `NtAlpcSendWaitReceivePort` (receive-only) → dequeues request with correct `ClientId` and body. Server sends `ALPC_MSGFLG_REPLY_MESSAGE` → client unblocks with reply body intact and matching `MessageId`. Datagram send (no `SYNC_REQUEST`) → message queued, sender returns immediately. Message exceeding `MaxMessageLength` → `STATUS_BUFFER_TOO_SMALL`. Reply with wrong `MessageId` → `STATUS_REPLY_MESSAGE_MISMATCH`. Port disconnected during wait → `STATUS_PORT_DISCONNECTED`. Serial log: `"[ALPC] msg sent: id=%llu type=%u len=%u"`. Test on: QEMU WHPX + TCG.

---

## 5. Asynchronous Delivery & Completion List

- [ ] (5.1 ALPC completion list) `ALPC_COMPLETION_LIST` -- lock-free single-producer / multi-consumer ring buffer for async message delivery:
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
  {CompletionPort, CompletionKey}, len)` -- associates an `IO_COMPLETION_OBJECT` with the port; incoming async messages post a completion packet instead of queuing on `MessageQueue`; integrates with `NtWaitForSingleObject` on the completion port

- [ ] (5.2 Async message delivery) When the port has an associated completion port: on `AlpcEnqueueMessage`, instead of pushing to `MessageQueue` + waking `WaitQueue`, post a completion packet to the `IO_COMPLETION_OBJECT`: `IoSetIoCompletion(completion_port, completion_key, message_ptr, STATUS_SUCCESS, 0)`
- [ ] Server consumes messages from the completion port via `NtRemoveIoCompletion` / `NtRemoveIoCompletionEx` (standard I/O completion port API, → future TODO for I/O completion ports in `05-storage-filesystems`)
- [ ] Fallback: if no completion port is associated, use the synchronous `MessageQueue` + `WaitQueue` path from §4
- [ ] (5.3 Waitable port) When `ALPC_PORTFLG_WAITABLE_PORT` is set: port itself is a waitable kernel object; `ObSetObjectWaitable(port)` makes it compatible with `NtWaitForSingleObject`; port is signalled when `MessageQueue` becomes non-empty, cleared when queue drains to empty
- [ ] (5.4 Commit) Commit: `"kernel/ipc/alpc: async completion list, waitable port, IO_COMPLETION integration"`

**Test checkpoint:** Create port with `ALPC_PORTFLG_WAITABLE_PORT`. `NtWaitForSingleObject(port)` blocks when queue is empty. Send a message → port becomes signalled, wait returns `STATUS_SUCCESS`. Drain queue → port unsignalled. If I/O completion ports are available: associate completion port with ALPC port, send message, verify completion packet posted. Serial log: `"[ALPC] waitable port signalled: %s"`. Test on: QEMU WHPX + TCG (pure kernel logic, no hardware).

---

## 6. Large Data: Port Sections & View Mapping

- [ ] (6.1 ALPC_PORT_SECTION) `ALPC_PORT_SECTION` node appended to `port->SectionList`:
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
  - `ObReferenceObjectByHandle(SectionHandle)` -- get existing Section object, OR if `SectionHandle == NULL`: `NtCreateSection` internally to create an anonymous shared-memory section of `SectionSize`
  - Allocate `ALPC_PORT_SECTION`, append to `port->SectionList`, assign unique `AlpcSectionHandle`
- [ ] `NtAlpcDeletePortSection(PortHandle, Flags, SectionHandle)` -- remove from list, dereference section object
- [ ] (6.2 View mapping) `ALPC_DATA_VIEW_ATTR` embedded in send/receive attributes:
  ```c
  typedef struct {
      uint32_t Flags;
      ALPC_SECTION_HANDLE SectionHandle;
      void     *ViewBase;   /* base address in caller's VA space */
      uint64_t  ViewSize;
  } ALPC_DATA_VIEW_ATTR;
  ```
- [ ] `NtAlpcCreateSectionView(PortHandle, Flags, DataView)` -- maps the registered section into the calling process's VA space (`NtMapViewOfSection`); `DataView->ViewBase` and `DataView->ViewSize` are filled in
- [ ] `NtAlpcDeleteSectionView(PortHandle, Flags, ViewBase)` -- unmaps the view
- [ ] Send path with view: caller fills `ALPC_DATA_VIEW_ATTR`, includes it as a message attribute; kernel copies the `ALPC_DATA_VIEW_ATTR` metadata into the queued message entry; receiver maps the same section on its side using `NtAlpcCreateSectionView` with the received `SectionHandle`
- [ ] (6.3 Message attributes dispatcher) `ALPC_MESSAGE_ATTRIBUTES` struct passed to send/receive:
  - Bitfield `ValidAttributes` selects which attribute structs are present
  - `ALPC_DATA_VIEW_ATTR` (bit 0x1) -- view for large data
  - `ALPC_CONTEXT_ATTR` (bit 0x2) -- port/message context values
  - `ALPC_HANDLE_ATTR` (bit 0x4) -- handle duplication across the port
  - `ALPC_SECURITY_ATTR` (bit 0x8) -- security context (for §7)
- [ ] `AlpcpValidateMessageAttributes(attrs, buffer_len)` -- bounds-check all present attribute structs against `buffer_len` before use
- [ ] (6.4 Commit) Commit: `"kernel/ipc/alpc: port sections, view mapping, message attributes dispatch"`

**Test checkpoint:** `NtAlpcCreatePortSection(NULL, 64*1024)` creates anonymous 64 KiB section. `NtAlpcCreateSectionView` maps it into caller's VA; writing a pattern to `ViewBase` succeeds. Send message with `ALPC_DATA_VIEW_ATTR`; receiver calls `NtAlpcCreateSectionView` with received `SectionHandle`; reads back matching pattern; zero copies. `NtAlpcDeleteSectionView` unmaps without crash. `NtAlpcDeletePortSection` dereferences section. `AlpcpValidateMessageAttributes` rejects invalid `buffer_len`. Serial log: `"[ALPC] section view mapped: base=%p size=%llu"`. Test on: QEMU WHPX + TCG.

---

## 7. Security: Client Token Capture & Impersonation

- [ ] (7.1 Security context capture at accept) In `NtAlpcAcceptConnectPort` (§3), if `AcceptConnection == TRUE` AND `port->Attributes.SecurityQos.ImpersonationLevel >= SecurityIdentification`:
  1. Locate the client task from `connection_request->Header.ClientId`
  2. `PsReferencePrimaryToken(client_task)` → `client_token`
  3. `NtDuplicateToken(client_token, TOKEN_QUERY | TOKEN_IMPERSONATE, NULL, SecurityQos.EffectiveOnly, TokenImpersonation, &captured_token)` -- duplicate at the requested impersonation level; `EffectiveOnly` strips disabled attrs
  4. Store `captured_token` in `server_comm_port->ClientToken`
  5. `PsDereferencePrimaryToken(client_token)`

- [ ] (7.2 Server impersonation) `NtAlpcImpersonateClientOfPort(PortHandle, Message, Reserved)`:
  1. `ObReferenceObjectByHandle(PortHandle)` → must be a server communication port
  2. Retrieve `port->ClientToken`; check it is not NULL
  3. `SeImpersonateClientEx(port->ClientToken, current_task)` sets `current_task->ImpersonationToken = port->ClientToken` (ref-counted copy) (→ XREF `TODO-11-security-reference-monitor.md §7.3`)
  4. Requires `SeImpersonatePrivilege` on the server's token if the client's IL is higher than the server's IL (→ XREF `TODO-11-security-reference-monitor.md §8`)

- [ ] (7.3 Security attribute in message) `ALPC_SECURITY_ATTR` (bit 0x8 in `ValidAttributes`):
  - On send: client includes its `SECURITY_QUALITY_OF_SERVICE` preferences
  - On receive: server reads the `ContextHandle` to call `NtAlpcImpersonateClientOfPort` without needing a separate call
- [ ] `NtAlpcCreateSecurityContext(PortHandle, Flags, SecurityAttribute)` -- pre-creates a security context handle to avoid repeated per-message token capture; stored in `ALPC_SECURITY_ATTR.ContextHandle`
- [ ] `NtAlpcDeleteSecurityContext(PortHandle, Flags, ContextHandle)` -- revokes and frees the captured context
- [ ] (7.4 Client connection SID verification) `RequiredServerSid` parameter to `NtAlpcConnectPort`: if non-NULL, kernel reads the server port owner's `UserSid` from `port->ClientToken` (set when server was created) and compares it using `RtlEqualSid`; returns `STATUS_SERVER_SID_MISMATCH` if different; prevents clients from connecting to hijacked ports
- [ ] (7.5 Self-contained execution note) Follow the callout on the next lines.
> [!NOTE]
> `SeImpersonateClientEx` (T11 §7) is not yet implemented. §7 (NtAlpcImpersonateClientOfPort) should implement a minimal inline version: `task->ImpersonationToken = ObReferenceObject(port->ClientToken)` with refcount management. Full privilege checks (`SeImpersonatePrivilege`, IL comparison) are deferred to T11 §7-§8; add `// TODO: SeImpersonatePrivilege check T11 §8` stub.

- [ ] (7.6 Commit) Commit: `"kernel/ipc/alpc: client token capture, NtAlpcImpersonateClientOfPort, SID verification"`

**Test checkpoint:** Server accepts connection from client task. `server_comm->ClientToken` is non-NULL and points to a duplicate of client's primary token. `NtAlpcImpersonateClientOfPort` sets `task_current()->ImpersonationToken` to the captured token. `RequiredServerSid` mismatch → `STATUS_SERVER_SID_MISMATCH`. `NtAlpcCreateSecurityContext` returns valid `ContextHandle`. `NtAlpcDeleteSecurityContext` revokes it. Serial log: `"[ALPC] client token captured: SID=%s"`, `"[ALPC] impersonating client: SID=%s"`. Test on: QEMU WHPX + TCG.

---

## 8. NtAlpc* SSDT Registration & Stub Retrofit

- [ ] (8.1 SSDT entries) Add the following NtAlpc* entry points to the SSDT (→ XREF `TODO-05 §4`):
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
  NtAlpcCreateSectionView
  NtAlpcCreateResourceReserve (pre-allocate resources for guaranteed delivery; related to §11 message zones)
  NtAlpcDeleteSectionView
  NtAlpcCreateSecurityContext
  NtAlpcDeleteSecurityContext
  NtAlpcImpersonateClientOfPort
  NtAlpcCancelMessage
  NtAlpcQueryInformationMessage
  ```
- [ ] **Gap (Win11 parity, gap-analysis 2026-04-14):** add SSDT slots + kernel helpers for **`NtAlpcOpenSenderProcess`** and **`NtAlpcOpenSenderThread`** so a server can obtain audited handles to the sender of a queued message (CSRSS-style diagnostics, sandbox introspection). Validate parameters against `PORT_MESSAGE.ClientId`; return `STATUS_INVALID_HANDLE` when the sender thread has exited.
- [ ] **Gap (Win11 parity, gap-analysis 2026-04-14):** implement **`NtAlpcRevokeSecurityContext`** if it remains a distinct export from `NtAlpcDeleteSecurityContext` on a pinned `ntdll.dll` / public symbol list; otherwise document a thin alias and close the gap with one implementation path.
- [ ] Add corresponding `ZwAlpc*` aliases in `include/kernel/ipc/alpc_syscalls.h`
- [ ] **Retrofit the 16 ALPC SSDT stubs in `src/kernel/nt/nt_alpc.c` (SSDT 0x010F-0x011E)** to real handlers that call into the ALPC engine instead of returning `STATUS_NOT_IMPLEMENTED`. Covered syscalls: `NtAlpcCreatePort` (0x010F), `NtAlpcConnectPort` (0x0110), `NtAlpcConnectPortEx` (0x0111), `NtAlpcAcceptConnectPort` (0x0112), `NtAlpcSendWaitReceivePort` (0x0113; canonical message rendezvous; needs `ALPC_MESSAGE_ATTRIBUTES` decode for handle/context/view attrs), `NtAlpcDisconnectPort` (0x0114), `NtAlpcCancelMessage` (0x0115), `NtAlpcCreatePortSection` (0x0116; wraps `NtCreateSection` with port association), `NtAlpcDeletePortSection` (0x0117), `NtAlpcCreateSectionView` (0x0118), `NtAlpcDeleteSectionView` (0x0119), `NtAlpcCreateResourceReserve` (0x011A; pre-allocated message zones from §11), `NtAlpcDeleteResourceReserve` (0x011B), `NtAlpcQueryInformation` (0x011C; all 11 `ALPC_PORT_INFORMATION_CLASS` values from §9), `NtAlpcSetInformation` (0x011D), `NtAlpcQueryInformationMessage` (0x011E; 2 `ALPC_MESSAGE_INFORMATION_CLASS` values from §9). Each retrofit drops the `SCOPE-GAP-ALLOWED` sentinel in `nt_alpc.c`, adds a handler body that probes user buffers via `ProbeForReadIfUser`/`ProbeForWriteIfUser`, resolves the port handle via `ObpReferenceObjectByHandle` (-> XREF TODO-03 §3), and dispatches to the matching ALPC engine helper (`alpc_port_create`, `alpc_port_connect`, `alpc_send_wait_receive`, `alpc_disconnect`, `alpc_query_info`, etc.). This auto-closes TODO-05 §31 (16 ALPC syscalls deferred here). Additional Vista+ ALPC syscalls listed in the §8 syscall list above (`NtAlpcCreateSecurityContext`, `NtAlpcDeleteSecurityContext`, `NtAlpcImpersonateClientOfPort`) require fresh SSDT numbers in `service_numbers.h` and are not covered by this retrofit.

---

## 9. NtAlpc QueryInformation, SetInformation & CancelMessage

- [ ] (9.1 NtAlpcQueryInformation) `ALPC_PORT_INFORMATION_CLASS` values (full enumeration):
  - `AlpcBasicInformation` (0) → `{Flags, SequenceNo, PortContext}`
  - `AlpcPortInformation` (1) → full `ALPC_PORT_ATTRIBUTES` readback
  - `AlpcAssociateCompletionPortInformation` (2) → read back associated completion port (writeable via `NtAlpcSetInformation` in this section)
  - `AlpcConnectedSIDInformation` (3) → SID of the connected peer process
  - `AlpcServerInformation` (4) → `{ThreadBlocked, ConnectedProcessId, ConnectionNtPath}`
  - `AlpcMessageZoneInformation` (5) → message zone status (→ §11)
  - `AlpcRegisterCompletionListInformation` (6) → completion list registration status (→ §5)
  - `AlpcUnregisterCompletionListInformation` (7) → unregister completion list
  - `AlpcAdjustCompletionListConcurrencyCountInformation` (8) → concurrency count for completion list drain threads
  - `AlpcRegisterCallbackInformation` (9) → callback function registered for async event notification
  - `AlpcCompletionListRundownInformation` (10) → drain and tear down completion list
- [ ] Return `STATUS_INVALID_INFO_CLASS` for unknown classes
- [ ] (9.2 NtAlpcQueryInformationMessage) `NtAlpcQueryInformationMessage(PortHandle, Flags, Message, MessageInformationClass, Buffer, Length, ReturnLength)`:
  - `AlpcMessageSidInformation` (0) → returns the SID of the sender of the specified message (derived from captured client token at send time)
  - `AlpcMessageTokenModifiedIdInformation` (1) → returns the `ModifiedId` LUID of the sender's token at send time; allows detecting if the token changed between send and receive

- [ ] (9.3 NtAlpcSetInformation) `AlpcPortAssociateCompletionPortInformation` (2); set completion port (§5 completion list)
- [ ] `AlpcBasicInformation` (0); update `MaxMessageLength`, `MemoryBandwidth` (only if no messages are pending)
- [ ] `AlpcDirectMessageAttribute` (3); set default message type for datagrams
- [ ] (9.4 NtAlpcCancelMessage) `NtAlpcCancelMessage(PortHandle, Flags, MessageContext)`:
  - Find entry on `port->PendingQueue` or `port->MessageQueue` matching `MessageContext->MessageId`
  - Remove from queue; wake any blocked sender with `STATUS_CANCELLED`
  - Used by timeout path in `NtAlpcConnectPort` and `NtAlpcSendWaitReceivePort`

- [ ] (9.5 Commit) Commit: `"kernel/ipc/alpc: NtAlpc* SSDT wiring, QueryInformation, SetInformation, CancelMessage"`

**Test checkpoint:** Every `NtAlpc*` entry point listed in the §8 syscall list plus any §8 gap items (OpenSender*, RevokeSecurityContext) is registered in the SSDT and callable from user mode (or kernel-mode `Zw*` alias). `NtAlpcQueryInformation(AlpcBasicInformation)` returns valid `{Flags, SequenceNo, PortContext}`. `NtAlpcQueryInformation` with unknown class → `STATUS_INVALID_INFO_CLASS`. `NtAlpcQueryInformationMessage(AlpcMessageSidInformation)` returns sender SID. `NtAlpcCancelMessage` cancels a pending request → blocked sender wakes with `STATUS_CANCELLED`. Serial log: `"[ALPC] SSDT wired: %u NtAlpc* entries"`. Test on: QEMU WHPX + TCG.

---

## 10. CSRSS ApiPort Bootstrap

- [ ] (10.1 CSRSS as the first ALPC server) CSRSS (`src/apps/csrss/csrss.c`): the Win32 subsystem server process (→ XREF `12-user-platform-sdk/TODO-05-win32-subsystem.md`); it is the first real user-mode process that uses ALPC; this section defines only the kernel-side bootstrap contract
- [ ] On kernel init Phase 3 (→ XREF `TODO-01-kernel-init-sequencing.md §5`): spawn CSRSS as a `SYSTEM`-token process before any other user processes; CSRSS calls:
  ```c
  NtAlpcCreatePort(&ApiPort,
      &ObjAttr(L"\\Windows\\ApiPort"),
      &PortAttrs{ .MaxMessageLength = 512,
                  .Flags = ALPC_PORTFLG_SYSTEM_PROCESS });
  ```
- [ ] `\Windows\ApiPort` must be resolvable in the Ob namespace (→ XREF `TODO-03-object-manager.md §4`); add `\Windows\` directory creation to Phase 1 Ob init
- [ ] (10.2 Win32 process creation notification) Every new process calls `NtAlpcConnectPort(L"\\Windows\\ApiPort", ...)` at startup in its CRT0 path (→ XREF `12-user-platform-sdk/TODO-04-ntdll-user-runtime.md §5` *(planned)*); sends `CsrClientConnectToServer` message with `{ProcessId, ThreadId, WindowsVersion, SubsystemType=IMAGE_SUBSYSTEM_WINDOWS_GUI/CUI}`
- [ ] CSRSS accepts, allocates a `CSR_PROCESS` record, stores `PortContext`, and replies with a session ID and the address of the CSR shared section
- [ ] From this point, Win32 console I/O, `CreateProcess`, `CreateThread`, and exception notification all flow through this ALPC connection
- [ ] (10.3 Message format compatibility) CSRSS message format uses a fixed 32-byte opcode area before the variable payload; matches the NT 5.x LPC message format for compatibility with the existing ntdll stubs that will be ported from the Win32 layer:
  ```c
  typedef struct {
      PORT_MESSAGE Header;
      uint32_t     ApiNumber;    /* CSRSS API index */
      NTSTATUS     ReturnValue;
      uint32_t     Reserved;
      uint8_t      Payload[CSRSS_MAX_PAYLOAD]; /* up to 476 bytes */
  } CSRSS_API_MSG;
  ```

- [ ] (10.4 Commit) Commit: `"kernel/ipc/alpc: CSRSS ApiPort bootstrap, Win32 process registration protocol"`

**Test checkpoint:** After Phase 3 init, `\Windows\ApiPort` is resolvable in the Ob namespace. Test `hello.exe` calls `NtAlpcConnectPort("\\Windows\\ApiPort")` → connection accepted by CSRSS. CSRSS replies with session ID. `hello.exe` receives the reply with valid `CSRSS_API_MSG.ReturnValue == STATUS_SUCCESS`. Multiple test processes connect → each gets independent session. Serial log: `"[CSRSS] ApiPort created"`, `"[CSRSS] client connected: PID=%u SessionId=%u"`. Test on: QEMU WHPX + TCG.

---

## 11. Message Zones (Pre-Allocated Message Buffers)

High-throughput ports like `\Windows\ApiPort` (CSRSS) process thousands of messages per second. The default per-message `kmalloc`/`kfree` cycle adds allocation pressure and cache misses. A message zone pre-allocates a contiguous buffer pool per port, and message allocations come from the zone via bump-pointer until full, falling back to `kmalloc` only when the zone is exhausted. Win11 uses `AlpcMessageZoneInformation` for this; Linux has no equivalent; Unix sockets allocate per-message `sk_buff` structs from the slab.

- [ ] Define `ALPC_MESSAGE_ZONE` structure:
  ```c
  typedef struct {
      void    *ZoneBase;       /* contiguous allocation from pool */
      uint64_t ZoneSize;       /* total size in bytes */
      uint64_t UsedBytes;      /* current bump-pointer offset */
      uint32_t ActiveMessages; /* messages currently in-flight from this zone */
      spinlock_t Lock;
  } ALPC_MESSAGE_ZONE;
  ```
- [ ] `AlpcCreateMessageZone(port, ZoneSize)` -- allocate a contiguous buffer of `ZoneSize` bytes via `pmm_alloc_contiguous()` (for zones > 4 KiB) or `kmalloc` (for smaller zones); attach to `port->MessageZone`
- [ ] `AlpcAllocateFromZone(zone, DataLength)` → `PORT_MESSAGE_ENTRY*`:
  - If `zone != NULL` and `zone->UsedBytes + alloc_size ≤ zone->ZoneSize`: bump-allocate from zone, increment `ActiveMessages`
  - Else: fall back to `AlpcAllocateMessage()` (§4 message pool kmalloc path)
- [ ] `AlpcFreeToZone(zone, entry)`; decrement `ActiveMessages`; when `ActiveMessages == 0`, reset `UsedBytes = 0` (zone becomes fully reusable)
- [ ] Wire `AlpcMessageZoneInformation` (info class 5) into `NtAlpcSetInformation`: set zone size; `NtAlpcQueryInformation`: read zone status
- [ ] CSRSS bootstrap (§10): create `\Windows\ApiPort` with a 64 KiB message zone by default
- [ ] Commit: `"kernel/ipc/alpc: message zone pre-allocated per-port message buffer pool"`

**Test checkpoint:** Create port with 16 KiB message zone. Send 100 messages of 128 bytes each. Verify all allocate from zone (no `kmalloc` calls). Send messages until zone is full. Verify fallback to `kmalloc` succeeds. After all messages are freed, verify zone resets (`UsedBytes == 0`). `NtAlpcQueryInformation(AlpcMessageZoneInformation)` returns correct `ZoneSize` and `UsedBytes`. Serial log: `"[ALPC] Zone alloc: port=%s size=%u used=%u"`. Test on: QEMU WHPX + TCG.

---

## 12. Live Port Monitor & IPC Profiler

- [ ] (12.1 Per-port message latency histogram) Follow the callout on the next lines.
> [!TIP]
> Neither Windows (no built-in per-port latency tracking) nor Linux (no kernel-level IPC profiling beyond `perf`/`ftrace`) provides a first-class, always-on message latency profiler for IPC. Impossible OS embeds a lightweight histogram directly in every `ALPC_PORT`, recording send-to-reply round-trip time in microsecond buckets. Developers see per-port P50/P95/P99 latencies in `alpcmon.exe` without attaching a debugger or running an ETW trace.

- [ ] Add to `ALPC_PORT`:
  ```c
  typedef struct {
      _Atomic(uint64_t) TotalSent;       /* messages sent through this port */
      _Atomic(uint64_t) TotalReceived;   /* messages received on this port */
      _Atomic(uint64_t) TotalBytesSent;
      _Atomic(uint64_t) TotalBytesRecvd;
      _Atomic(uint64_t) LatencyBuckets[8]; /* us: <10, <50, <100, <500, <1000, <5000, <10000, >=10000 */
  } ALPC_PORT_STATS;
  ```
- [ ] On `NtAlpcSendWaitReceivePort` with `ALPC_MSGFLG_SYNC_REQUEST`: record `tsc_start` at send, `tsc_end` at reply wake; delta → microseconds → increment appropriate `LatencyBuckets[i]`
- [ ] Expose via `NtQuerySystemInformation(SystemAlpcPortInformation)` alongside existing per-port stats
- [ ] (12.2 Kernel query API) `NtQuerySystemInformation(SystemAlpcPortInformation, ...)`:
  - Walk all named ports in `\RPC Control\` and `\Windows\` Ob directories
  - Per-port: name, owner PID, pending message count, pending connection count, connected ports count, max message length, stats (total sent/received/bytes, latency histogram)
  - Return as array of `SYSTEM_ALPC_PORT_INFORMATION` structs

- [ ] (12.3 alpcmon.exe app) `src/apps/alpcmon/alpcmon.c`: live view of active ALPC ports:
  - Refreshes every 1 s via `NtQuerySystemInformation` poll
  - Table view: Port Name | Owner PID | Owner Image | Pending Msgs | Clients | Msg/s | Bytes/s | P50 us | P99 us
  - Click row → detail pane: full path, creation time, security descriptor (SDDL string), message history (last 16 message IDs + types), latency histogram bar chart
  - Right-click → "Disconnect port" (requires `SeDebugPrivilege`)
  - Right-click → "Reset stats" (zero counters for this port)

- [ ] (12.4 Commit) Commit: `"kernel/ipc/alpc: alpcmon.exe live port monitor with per-port latency profiler"`

**Test checkpoint:** Create port, send 50 synchronous request+reply messages. Query `SystemAlpcPortInformation`. Verify `TotalSent == 50`, `TotalReceived == 50`, at least one `LatencyBuckets` entry > 0. Run `alpcmon.exe`. Verify `\Windows\ApiPort` shows in the table with non-zero Msg/s after CSRSS boot test. Serial log: `"[ALPC] Port stats: %s sent=%llu recv=%llu P50=%lluus"`. Test on: QEMU WHPX + TCG.

---

## OS Comparison

| ⭐ | Feature                      | 🪟 Win11               | 🐧 Linux                 | 🚀 Impossible OS                      |
| --- | ------------------------------ | ----------------------- | ------------------------- | -------------------------------------- |
| 💎 | Connection-oriented ports    | ✅ ALPC                | ⚠️ SOCK_SEQPACKET       | ⬜ §2-§3                              |
| 💎 | Sync send+wait+reply         | ✅ Full                | ⚠️ No typed reply       | ⬜ §4                                 |
| 💎 | Async completion delivery    | ✅ Full                | ⚠️ io_uring (RFC 2026)  | ⬜ §5                                 |
| 💎 | Large data via section       | ✅ Port sections       | ⚠️ Manual mmap          | ⬜ §6                                 |
| 💎 | Client identity capture      | ✅ Full                | ⚠️ SCM_CREDENTIALS      | ⬜ §7                                 |
| 💎 | Named port namespace         | ✅ `\\RPC Control\\`     | ⚠️ Abstract sockets     | ⬜ §2                                 |
| 💎 | CSRSS subsystem server       | ✅ Full                | ❌ N/A                   | ⬜ §10                                |
| 💎 | Handle dup across port       | ✅ ALPC_HANDLE_ATTR    | ⚠️ SCM_RIGHTS           | ⬜ D10 T08 §8                         |
| 💎 | Connection SID verification  | ✅ Full                | ⚠️ SO_PEERPIDFD (2023+) | ⬜ §7 (SID verify)                    |
| 💎 | Per-message SID query        | ✅ Full                | ❌ N/A                   | ⬜ §9 (NtAlpcQueryInformationMessage) |
| 💎 | Open sender proc/thread      | ✅ NtAlpcOpenSender*   | ⚠️ peer creds / pidfd   | ⬜ §8 gap items                       |
| 💎 | Revoke security context      | ✅ NtAlpcRevoke*       | ❌ N/A                   | ⬜ §8 gap items                       |
| 💎 | Message zones                | ✅ AlpcMessageZone     | ❌ Per-msg sk_buff       | ⬜ §11                                |
| 💎 | Completion list lifecycle    | ✅ Register/Unregister | ❌ N/A                   | ⬜ §9                                 |
| ⭐ | Per-port latency histogram   | ❌ ETW only            | ❌ ftrace only           | ⬜ §12 (histogram)                    |
| ⭐ | Live port monitor + profiler | ❌ WinObj read-only    | ❌ N/A                   | ⬜ §12 (alpcmon)                      |

> **Deferred features (→ other TODOs):**
> - Handle attribute marshalling and direct/indirect mode → `03-memory-concurrency/TODO-08-win32-ipc-extensions.md §8`
> - LPC compatibility syscalls (`NtCreatePort`, `NtConnectPort`, etc.) → `03-memory-concurrency/TODO-08-win32-ipc-extensions.md §7`

After §1-10, Impossible OS reaches full Windows 11 ALPC parity for hosting CSRSS, RPC local transport, COM local activation, and the Win32 subsystem server ecosystem. §11 adds message zones for high-throughput ports. Linux's closest equivalent (Unix domain sockets with `SOCK_SEQPACKET`) lacks typed reply routing, integrated impersonation, and section-based zero-copy data transfer. The live port monitor with latency profiler (§12) gives developers a real-time view of all active message ports with P50/P99 latency stats; a developer-experience exclusive that neither Windows (WinObj is read-only, ETW requires separate trace capture) nor Linux ship in their default tooling.

---

## Unit Tests

> Boot tests run with `test=1` in `boot.conf`.

- [ ] Create `src/kernel/test/test_alpc.c` with:
  - Port create: `NtAlpcCreatePort` returns valid handle, port object in Ob namespace
  - Port connect: client `NtAlpcConnectPort` to server → connection handle valid on both sides
  - Send/receive: client sends 64-byte message → server receives matching data via `NtAlpcSendWaitReceivePort`
  - Reply: server replies → client receives reply with correct `MessageId` round-trip
  - Max message size enforced: message exceeding `MaxMessageLength` rejected with `STATUS_BUFFER_TOO_SMALL`
  - Port close: closing server port → pending client receives `STATUS_PORT_DISCONNECTED`
  - Multiple clients: 3 clients connect to 1 server, each gets independent connection
  - View section: `NtAlpcCreatePortSection` + `NtAlpcCreateSectionView` → shared memory accessible from both sides
  - Connection reject: server calls `NtAlpcAcceptConnectPort` with `AcceptConnection=FALSE` → client gets `STATUS_PORT_CONNECTION_REFUSED`
  - SID verification: client connects with wrong `RequiredServerSid` → `STATUS_SERVER_SID_MISMATCH`
  - Message zone: create port with 16 KiB zone → 100 messages allocate from zone without `kmalloc`
  - Per-message query: `NtAlpcQueryInformationMessage(AlpcMessageSidInformation)` returns sender SID
  - Open sender: after a queued message, `NtAlpcOpenSenderProcess` / `NtAlpcOpenSenderThread` return handles matching `PORT_MESSAGE.ClientId`; dead thread returns `STATUS_INVALID_HANDLE`
  - Port stats: send 50 messages → `ALPC_PORT_STATS.TotalSent == 50`
- [ ] Register in `test_runner_init()`: `test_suite_register_cat("alpc", test_register_alpc, TEST_CAT_IPC)`
- [ ] Commit: `"test: add ALPC message port test suite"`

---

## Verification

- [ ] **Unit test: connection handshake**: two kernel tasks; task A creates `\RPC Control\TestPort`; task B calls `NtAlpcConnectPort`; task A calls `NtAlpcAcceptConnectPort`; verify both ends hold valid port handles; verify `NtAlpcDisconnectPort` sends `PORT_CLOSED` to peer.
- [ ] **Unit test: sync send+reply**: task B sends a 64-byte request and blocks; task A receives it, sends reply; task B unblocks with the reply body intact; verify `MessageId` round-trip and `ClientId` is correctly filled.
- [ ] **Unit test: concurrent receivers**: 4 server threads all call `NtAlpcSendWaitReceivePort`; send 100 messages from one client; verify each message is delivered exactly once and total received count == 100.
- [ ] **Unit test: port section large transfer**: register a 4 MiB section, map it, write a pattern, send; receiver maps same section, reads back pattern; verify byte-for-byte match without any `kmalloc` for the message body.
- [ ] **Unit test: message zone**: create port with 16 KiB zone, send 100 × 128 B messages; verify all allocate from zone; exhaust zone, verify fallback to `kmalloc`; free all, verify zone reset.
- [ ] **Unit test: per-message SID query**: send message from client, call `NtAlpcQueryInformationMessage(AlpcMessageSidInformation)` on server side; verify returned SID matches client's user SID.
- [ ] **Unit test: port stats & latency**: send 50 sync request+reply messages; query `SystemAlpcPortInformation`; verify `TotalSent >= 50`, at least one latency bucket > 0.
- [ ] **CSRSS boot test in QEMU**: kernel spawns CSRSS; a test `hello.exe` connects to `\Windows\ApiPort`; CSRSS logs the connection and replies; verify `hello.exe` receives session ID; `alpcmon.exe` shows `\Windows\ApiPort` with 1 connected client and non-zero Msg/s.
- [ ] **Platform sweep:** repeat high-value ALPC checkpoints (handshake, sync reply, port section, message zone) on QEMU TCG, VirtualBox NEM, QEMU WHPX, and bare metal. Serial strings must match WHPX runs; treat any VM-only timing or IRQL quirks as defects, not baselines.
- [ ] **Remaining limits**: I/O completion port integration (§5) requires `NtCreateIoCompletion` / `NtRemoveIoCompletion` from `05-storage-filesystems`; handle duplication attribute (`ALPC_HANDLE_ATTR`) and LPC compat wrappers deferred to `03-memory-concurrency/TODO-08-win32-ipc-extensions.md §7-§8`; ALPC debugging/tracing via ETW: no owning TODO yet; track under `10-platform-services/TODO-12-long-term-features.md` when telemetry or ETW-like tracing is scoped (or add a subsection in `TODO-08-win32-api-surface.md` if user-mode only).
- [ ] Commit: `"kernel/ipc/alpc: ALPC complete -- port objects, connection handshake, sync send+reply, async completion, port sections, security, message zones, CSRSS ApiPort, alpcmon"`

**Test runner:** `scripts/debug/run-ipc-tests.bat` (SUITE=ipc)

---

## History

| Date | Action | Summary |
|------|--------|---------|
| 2026-04-14 | validate | Structural validate-todo-file pass: folded ### N.M into checklist lines, ASCII range hyphens, TEST_CAT_IPC, padded OS Comparison, ETW deferral pointer, `run-ipc-tests.bat` runner line, bare-metal + VBox platform sweep bullet, History section. |
| 2026-04-14 | gap-analysis | Win11/Linux IPC research (6+ web searches); code-truth: `nt_alpc.c` stubs + `test_ob.c`; IMPORTANT + Inputs refresh; §8 split into §8 SSDT + §9 Query/Set (12 sections); OpenSender* + RevokeSecurityContext gap items; OS rows; XREF updates in TODO-05, TODO-A, TODO-08, `nt_alpc`/`nt_lpc` headers. |
| 2026-04-14 | validate | validate-todo-file: planned Inputs paths; flat § refs (no §N.M); XREF colons; removed blanks between adjacent checkboxes; ASCII latency comment; OS §12 row text; padded Impl Order row 9. |
| 2026-04-14 | implement | §1 Message header / port attributes / type codes: `include/kernel/ipc/alpc.h` (PORT_MESSAGE 40B + SQOS 12B + ALPC_PORT_ATTRIBUTES 80B with sizeof + per-field offset `_Static_assert`, 10 msg-type codes, 4 PORTFLG + 6 MSGFLG non-overlap asserts), `src/kernel/ipc/alpc.c` (`alpc_init` klog), `src/kernel/test/test_alpc.c` (7 suites under `TEST_CAT_IPC`), pipe_init -> boot_result_t; Codex adversarial review (2 highs: ABI-claim softened to explicit native-layout + PORT_ATTRIBUTES locked); `boot_phase3` wiring + TODO-01 §8 pipe_init checked. |
| 2026-04-14 | review | §1 review-todo-section: Phase-1 evidence + scope-gap + build clean; Phase-2 adversarial already ran during implement; Phase-3 kernel-code-quality Gates 1-11 walked clean + second Codex quality dispatch (dead-code + consistency + perf) found 2 highs: duplicate `CLIENT_ID` (fixed by reusing canonical 16B type from `ob/teb.h` + re-asserting locally) and duplicate `SECURITY_IMPERSONATION_LEVEL` (fixed by reusing canonical enum from `security/token.h`); PORT_MESSAGE offsets re-shifted MessageId 16->24 and CallbackId 24->32; trailing Reserved u64 dropped; test layout + TODO spec synced; full build passes proving coexistence with both consumer headers. |
| 2026-04-15 | implement | §2 ALPC_PORT object + registration: new `include/kernel/ipc/alpc_port.h` (ALPC_PORT body, PORT_MESSAGE_ENTRY, ALPC_MSG_QUEUE, ALPC_PORT_STATS stub), new `src/kernel/ipc/alpc_port.c` (`alpc_port_init` registers ObpAlpcPortType + creates `\RPC Control`, `alpc_port_on_delete` drains queues under lock/free outside, `AlpcCreatePort` kernel helper with **handle-first rollback** ordering), NtAlpcCreatePort retrofitted with ProbeForRead/WriteIfUser + `alpc_probe_and_split` that **copies leaf into kernel-owned 64-byte buffer** (TOCTOU fix); slot 0x010F removed from pending sweep in test_ob.c; 10 new IPC tests (port type + \RPC Control + named/unnamed create + duplicate reject + NULL ht + embedded slash reject + syscall full-path + syscall bad-prefix reject). Codex adversarial review -- 3 highs (full-path-as-leaf, missing user-pointer probes, half-created namespace entry on handle-fail) all fixed; re-dispatch found 1 high TOCTOU (leaf still pointed into user buf), fixed with kernel-owned copy. Added `STATUS_OBJECT_NAME_INVALID = 0xC0000033` to ntstatus.h. |
| 2026-04-15 | review | §2 review-todo-section: Phase-1 evidence + scope-gap + build clean; Phase-2 adversarial already ran 2x during implement; Phase-3 kernel-code-quality Gates 1-11 walked clean + Codex quality dispatch (dead-code + consistency + perf) found 4 more: 1 HIGH rejected (probe+direct-deref is kernel-wide precedent, SMAP disabled per CLAUDE.md), 2 MEDIUM fixed (`alpc_validate_attrs` rejects unknown Flags + privileged SYSTEM_PROCESS + non-zero reserved pads; `AlpcCreatePort` now returns NTSTATUS+out-HANDLE so syscall propagates OBJECT_NAME_COLLISION/INVALID/NOT_FOUND accurately), 1 LOW fixed (removed over-promising sizeof-drift-check comment from alpc_port.h). 2 new tests (privileged flag + reserved pad rejection). TODO-03/TODO-02 §4 "Audit all syscall handlers" checklist item extended to explicitly name `nt_alpc.c`'s `alpc_probe_and_split` + `NtAlpcCreatePort_handler` as retrofit consumers (concrete XREF for the Accepted finding). |
