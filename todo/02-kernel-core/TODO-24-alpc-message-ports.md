---
schema_version: 1
id: alpc-message-ports
domain: 02-kernel-core
status: active
title: "TODO-24 -- ALPC / Message Ports"
---

# TODO-24 -- ALPC / Message Ports

> **Validated:** 2026-07-17 | validate-todo-file clean (structure / IO table / XREF / test wiring)

> **Gap-audited:** 2026-07-17 | gap-audit + codex-gap-audit (needs-attention, 5 findings verified + filed); 6 gaps -> Branch A into open §6-§11 (msg-identity attrs, resource reserves, completion-list not-impl, connection-message ABI, namespace generalization, ConnectPortEx)

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
- `include/kernel/ipc/alpc.h` (created in §1)
- `include/kernel/ipc/alpc_port.h` (created in §2)
- `include/kernel/ipc/alpc_syscalls.h` (planned; first created in §8)
- `src/apps/csrss/csrss.c` (planned; §10 kernel-side bootstrap contract)
- `src/apps/alpcmon/alpcmon.c` (planned; §12 profiler UI)
- → XREF: `TODO-05-object-manager.md §1-§3`: ALPC ports are `OBJECT_TYPE` kernel objects with handles, reference counts, and named entries in `\RPC Control\`
- → XREF: `TODO-05-object-manager.md §7`: `NtAlpcCreatePortSection` registers a Section object with the port; section creation depends on the Section object type
- → XREF: `TODO-12-native-api-ssdt.md §5`: all `NtAlpc*` entry points are SSDT slots; wiring happens after the SSDT exists
- → XREF: `TODO-07-irql-model-dpcs.md §3`: message delivery runs at `DISPATCH_LEVEL` briefly when queuing to the server's message queue; IRQL discipline applies
- → XREF: `TODO-15-security-reference-monitor.md §2-§7`: client security context capture requires `ACCESS_TOKEN`; server impersonating a client requires `SeImpersonatePrivilege`
- → XREF: `12-user-platform-sdk/TODO-05-win32-subsystem.md`: CSRSS (Win32 subsystem server) creates its `ApiPort` using the bootstrap path defined in §10 of this TODO
- → XREF: `03-memory-concurrency/TODO-09-win32-ipc-extensions.md §8`: LPC compatibility syscalls (`NtCreatePort`, `NtConnectPort`, `NtReplyWaitReceivePort`) redirect to ALPC internally; depends on this TODO's `ALPC_PORT` and `NtAlpc*` surface
- → XREF: `03-memory-concurrency/TODO-09-win32-ipc-extensions.md §9`: incremental Win32 ALPC extensions (handle-attribute cross-process dup, direct/indirect mode) build on top of this TODO's `ALPC_PORT` object and `NtAlpc*` syscall surface; 03-memory-concurrency/TODO-09 §8 must not re-implement core ALPC primitives

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
- Message zones provide pre-allocated buffer pools for high-throughput ports, eliminating per-message `kmalloc` on the hot path; resource reserves add per-message reusable reservations for guaranteed delivery under memory pressure.
- Per-message TOKEN and WORK_ON_BEHALF_OF identity attributes propagate sender / RPC-caller provenance across chained ALPC hops.
- Connection-time message and attribute negotiation (full-width connect/accept ABI) carries `PortContext` and connection payloads for CSRSS/RPC.
- CSRSS `ApiPort` boots and accepts the first Win32 subsystem connections.
- `alpcmon.exe` provides a live port monitor with per-port latency histograms (P50/P99); a developer-experience exclusive.

---

## Implementation Order

| ⭐   | Order | Deliverable                                              | Depends On    | Status |
| --- | :---: | -------------------------------------------------------- | ------------- | :----: |
| 💎   |   1   | Message header, port attributes, type codes              | (none)        |  [x]   |
| 💎   |   2   | ALPC_PORT object & Object Manager registration           | §1, T05 §1-§4 |  [x]   |
| 💎   |   3   | Connection state machine (create/connect/accept)         | §2, T12 §4    |  [x]   |
| 💎   |   4   | Synchronous send+wait+receive engine                     | §3, T07 §3    |  [x]   |
| 💎   |   5   | Asynchronous delivery & completion list                  | §4            |  [x]   |
| 💎   |   6   | Large data: port sections & view mapping                 | §2, T05 §3,§7 |  [/]   |
| 💎   |   7   | Security: client token capture & impersonation           | §4, T15 §4-§7 |  [ ]   |
| 💎   |   8   | NtAlpc* SSDT registration & stub retrofit                | §1-§7, T12 §4 |  [ ]   |
| 💎   |   9   | NtAlpc QueryInformation / SetInformation / CancelMessage | §8, T12 §4    |  [ ]   |
| 💎   |  10   | CSRSS ApiPort bootstrap                                  | §3-§9         |  [ ]   |
| 💎   |  11   | Message zones (pre-allocated message buffers)            | §4, §8-§9     |  [ ]   |
| ⭐   |  12   | Live port monitor & IPC latency profiler                 | §8-§9         |  [ ]   |

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

> **Test runner:** `scripts\debug\kernel\run-ipc-tests.bat` (SUITE=ipc)
> **Expected:** 7 alpc suites pass, 0 failures

> **Verified:** 2026-04-14 -- Evidence mapped: every `[x]` item backed by `include/kernel/ipc/alpc.h` (CLIENT_ID asserts, PORT_MESSAGE 40B, SQOS 12B, ALPC_PORT_ATTRIBUTES 80B, 10 msg-type codes, 4 PORTFLG + 6 MSGFLG non-overlap asserts, `alpc_init` proto), `src/kernel/ipc/alpc.c` (klog line), `src/kernel/test/test_alpc.c` (7 suites TEST_CAT_IPC), `src/kernel/main/boot_desktop.c:101-102` (wiring). No scope gaps; no `TODO/FIXME/HACK/STATUS_NOT_IMPLEMENTED` in delta. Build clean (`=== BUILD OK ===`). Test checkpoint serial string `[alpc] header constants defined` confirmed at `src/kernel/ipc/alpc.c:18`.
> **Quality reviewed:** 2026-04-14 -- Two Codex dispatches (step-5 adversarial + step-8 quality/dead-code/consistency/perf). Four Highs fixed: (1) ABI-comment overclaim of Windows winternl.h binary compat softened to explicit Impossible-OS-native layout; (2) `ALPC_PORT_ATTRIBUTES` sizeof + every external offset locked with explicit `Pad0/Pad1/Pad2` fields (80B); (3) duplicate `CLIENT_ID` collision with `kernel/ob/teb.h` fixed by reusing the canonical 16-byte type + re-asserting sizeof/offsets locally; (4) duplicate `SECURITY_IMPERSONATION_LEVEL` collision with `kernel/security/token.h` fixed by reusing canonical enum. PORT_MESSAGE offsets shifted: MessageId 16 -> 24, CallbackId 24 -> 32; trailing Reserved u64 removed (16-byte CLIENT_ID fills the gap). Coexistence compile-proof: `alpc.h` now includes both `ob/teb.h` and `security/token.h`; full-tree build passes. Tests + TODO spec + kernel-code-quality gates (Gates 1-11) all walked. No dead code. No perf concerns (header-only + one klog line).

---

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

> **Test runner:** `scripts\debug\kernel\run-ipc-tests.bat` (SUITE=ipc)
> **Expected:** 12 new alpc-port suites + 7 §1 ABI suites pass, 0 failures

> **Verified:** 2026-04-15 -- Evidence mapped: every `[x]` item backed by `include/kernel/ipc/alpc_port.h` (ALPC_PORT_TYPE, ALPC_PORT body, PORT_MESSAGE_ENTRY, ALPC_MSG_QUEUE, ALPC_PORT_STATS, public API), `src/kernel/ipc/alpc_port.c` (`alpc_port_init`, `alpc_port_on_delete`, `alpc_validate_attrs`, `AlpcCreatePort`), `src/kernel/nt/nt_alpc.c` (`alpc_probe_and_split` + retrofit), `src/kernel/ipc/alpc.c` (alpc_init chain), `src/kernel/test/test_alpc.c` (12 suites). No scope gaps (only `SCOPE-GAP-ALLOWED` on the 15 remaining NtAlpc* stubs, which TODO-12 §31 owns). Build clean (`=== BUILD OK ===`). Test checkpoint strings `[alpc] ALPC Port type registered` and `[alpc] \RPC Control directory created` confirmed at `src/kernel/ipc/alpc_port.c:122` and `:142`.
> **Accepted:** copy_from_user retrofit deferred per CLAUDE.md SMAP gate -> XREF: `03-memory-concurrency/TODO-02 §4` (item: "Audit all syscall handlers" at line 132 -- now names `nt_alpc.c`'s `alpc_probe_and_split` + `NtAlpcCreatePort_handler` as explicit retrofit consumers).
> **Quality reviewed:** 2026-04-15 -- kernel-code-quality Gates 1-11 walked clean. Codex quality dispatch (dead-code + consistency + perf) returned 4 findings: (1) HIGH rejected -- probe+direct-deref is the kernel-wide pattern (`nt_namespace.c:37`, `nt_section.c:39`, `nt_timer.c`, `nt_file.c`) and SMAP is intentionally disabled per `CLAUDE.md "No SMEP/SMAP until per-process page tables"`; (2) MEDIUM fixed -- added `alpc_validate_attrs` in `alpc_port.c:138` that rejects unknown `ALPC_PORTFLG_*` bits, `ALPC_PORTFLG_SYSTEM_PROCESS` (pending SeAccessCheck at §7), and non-zero reserved pads; (3) MEDIUM fixed -- `AlpcCreatePort` now returns `NTSTATUS` + out-HANDLE so the syscall propagates `STATUS_OBJECT_NAME_COLLISION` for duplicates, `STATUS_OBJECT_NAME_INVALID` for bad leaves, `STATUS_OBJECT_NAME_NOT_FOUND` for missing `\RPC Control`, `STATUS_INVALID_PARAMETER` for bad attrs, `STATUS_INSUFFICIENT_RESOURCES` only for real allocation failure; (4) LOW fixed -- removed the over-promising "static assert on sizeof(alpc_port) catches drift" claim from `alpc_port.h` (internal layout, not ABI). Added 2 new tests (privileged flag rejected + reserved pad rejected) to exercise validation.

---

## 3. Connection State Machine

The three-way handshake: client connects by name, server accepts/rejects, both sides hold unnamed communication-port handles cross-linked via `ConnectedPort`. `NtAlpcCreatePort` already ships from §2, so §3 delivers the remaining three syscalls (`NtAlpcConnectPort`, `NtAlpcAcceptConnectPort`, `NtAlpcDisconnectPort`) plus the cross-task wake path.

> [!IMPORTANT]
> **Primitive swap:** §2's `WaitQueue` was declared as `condvar_t`, but `condvar_t` requires a held `mutex_t` partner and this kernel's ports lock with `spinlock_t`. `event_t` is the right cross-task wakeup primitive here -- it carries a timeout (`event_wait_timeout`, see `include/kernel/sched/event.h`) and does not require a matched mutex. §3 changes `ALPC_PORT.WaitQueue` and `PORT_MESSAGE_ENTRY.ReplySyncWait` from `condvar_t` to `event_t`. No §2 code path exercised the condvar, so the swap is behaviour-neutral on the §2 test suite.

- [x] Switch `ALPC_PORT.WaitQueue` and `PORT_MESSAGE_ENTRY.ReplySyncWait` from `condvar_t` to `event_t` (`include/kernel/sched/event.h`). Update `alpc_port_on_delete` + `AlpcCreatePort` initialisation. No cross-ref changes in tests.
- [x] Add `ALPC_PORT_CONNECT = 0x0001` and `ALPC_PORT_ALL_ACCESS = 0x001F` to `include/kernel/ipc/alpc_port.h`. Added `STATUS_INVALID_PORT_HANDLE` (0xC0000042) to `include/kernel/nt/ntstatus.h`.
- [x] Define file-local `ALPC_CONNECTION_REQUEST` in `src/kernel/ipc/alpc_port.c` with `Link_next` + `ClientCommPort` + `ServerCommPort` + `RequesterTask` + `ReplyStatus` + `ReplyEvent` (auto-reset). Reuse `ALPC_MSG_QUEUE` head/tail slots via the shared `Link_next` prefix through `conn_queue_enqueue`/`conn_queue_dequeue` helpers.
- [x] `AlpcConnectPort(ht, port_name, timeout_ms, out_handle)` implemented. Lookup server_conn by name + subtype check; allocate client_comm unnamed; allocate handle; build request; enqueue under server_conn->Lock; `event_set(WaitQueue)`; `event_wait_timeout(ReplyEvent, timeout_ms)` (0 = block forever). Timeout path re-locks server_conn, unlinks request, handles the race where the server dequeued but has not yet signalled. Drops creation ref on success (finding fix). Rolls back client_comm + handle + server_body refs on every failure path.
- [x] `AlpcAcceptConnectPort(ht, conn_port_handle, accept, timeout_ms, out)` implemented. ObpLookupHandle + ObReferenceObject pin + PortType == AlpcServerConnectionPort check. Dequeue loop with `event_wait_timeout(WaitQueue)` when queue empty. Reject: `ReplyStatus=STATUS_PORT_CONNECTION_REFUSED`, `event_set`, server return SUCCESS. Accept: allocate server_comm unnamed, cross-link (server_comm->ConnectedPort <-> client_comm) with ref+1 each, pin ConnectionPort, allocate server handle, snapshot `RequesterTask->pid` BEFORE `event_set` (UAF fix).
- [x] `AlpcDisconnectPort(ht, port_handle)` implemented. Lock port, set Disconnected, snapshot peer + pin; if server_conn, also detach the whole ConnectionQueue so pending clients unblock with `STATUS_PORT_DISCONNECTED` instead of their per-request timeout (finding fix). Outside lock: wake every pending connect with DISCONNECTED status; queue `ALPC_MSG_TYPE_PORT_CLOSED` on peer's MessageQueue + signal peer WaitQueue; drop peer pin + peer ConnectedPort ref.
- [x] `alpc_port_on_delete` now releases `ConnectionPort` as well as `ConnectedPort` (finding fix) -- comm ports no longer leak the listener port reference.
- [x] Retrofit `NtAlpcConnectPort_handler` -- `alpc_probe_and_split` leaf extraction, rebuild full `\RPC Control\<leaf>` path, `AlpcConnectPort` call. Port attrs probed + validated (discarded for §3 scope).
- [x] Retrofit `NtAlpcAcceptConnectPort_handler` -- probe out_handle, call `AlpcAcceptConnectPort`. PortContext / ConnectionMessage / ConnMsgAttr deferred with `SCOPE-GAP-ALLOWED` to §8 (connection-message negotiation).
- [x] Retrofit `NtAlpcDisconnectPort_handler` -- direct call to `AlpcDisconnectPort`; Flags deferred.
- [x] Slots `0x0110`, `0x0112`, `0x0114` dropped from the pending-features sweep in `test_ob.c` alongside `0x010F` (§2).
- [x] **SeAccessCheck (deferred):** on_open stays NULL per §2 precedent; tracked at `02-kernel-core/TODO-15 §5` (SeAccessCheck Engine) which owns the retrofit. `SCOPE-GAP-ALLOWED: pending T15 §5` sentinel in source.
- [x] **RequiredServerSid (deferred):** skipped per §3 minimum-scope; §7 (token capture) will add the check alongside `ClientToken` population. `SCOPE-GAP-ALLOWED: pending §7` sentinel.
- [x] Unit tests under `TEST_CAT_IPC`: `connect_nonexistent` (STATUS_OBJECT_NAME_NOT_FOUND), `connect_type_mismatch`, cross-thread `accept_handshake` (ConnectedPort cross-link verified), cross-thread `reject_handshake` (client CONNECTION_REFUSED + server SUCCESS), `disconnect_queues_close_msg` (PORT_CLOSED in peer's MessageQueue), `disconnect_unconnected` (no-op success). Uses `kthread_create`/`thread_join` pattern from `test_ipc.c`.
- [x] Commit: `"kernel/ipc/alpc: connection state machine (Connect/Accept/Disconnect)"`

**Test checkpoint:** 6 new `alpc:` suites under `TEST_CAT_IPC`: (a) connect-to-nonexistent returns `STATUS_OBJECT_NAME_NOT_FOUND`; (b) connect-to-non-connection-port rejects with `STATUS_INVALID_PORT_HANDLE`; (c) two-thread accept handshake succeeds and both sides have non-NULL `ConnectedPort`; (d) reject handshake returns `CONNECTION_REFUSED` to client and `SUCCESS` to server; (e) disconnect enqueues `PORT_CLOSED` on peer's `MessageQueue`; (f) disconnect on unconnected port is a no-op success. Platforms: QEMU WHPX + TCG (SMP cross-task required); bare metal + VirtualBox sweep by running the IPC test suite.

> **Test runner:** `scripts\debug\kernel\run-ipc-tests.bat` (SUITE=ipc)
> **Expected:** 8 new alpc-connection suites pass, 0 failures

> **Verified:** 2026-04-15 -- Evidence mapped: every `[x]` item backed by `include/kernel/ipc/alpc_port.h` (event_t swap + ALPC_PORT_CONNECT/ALL_ACCESS + 3 API prototypes) + `src/kernel/ipc/alpc_port.c` (ALPC_CONNECTION_REQUEST, conn_queue_{enqueue,dequeue}, alpc_alloc_comm_port, AlpcConnectPort/AcceptConnectPort/DisconnectPort) + `src/kernel/nt/nt_alpc.c` (3 retrofits) + `src/kernel/test/test_alpc.c` (8 new TEST_CAT_IPC suites). Scope-gap audit: only `SCOPE-GAP-ALLOWED` sentinels for SeAccessCheck (T15 §5) and RequiredServerSid (§7). Test checkpoint klog strings `[alpc] connect: server=`, `[alpc] accept:`, `[alpc] disconnect:` all confirmed in source. Build clean (`=== BUILD OK ===`).
> **Accepted:** `ObpLookupHandle` close-race -> XREF: 02-kernel-core/TODO-05 §3 (item: "Add `ObpReferenceObjectByHandle(table, handle, required_type, required_access, out_body, out_granted)` primitive" at line 148 -- retrofit list now names `src/kernel/ipc/alpc_port.c`'s `AlpcAcceptConnectPort` + `AlpcDisconnectPort` as consumers).
> **Quality reviewed:** 2026-04-15 -- kernel-code-quality Gates 1-11 walked clean. Codex quality dispatch (dead-code + consistency + perf) found 3 more: 2 HIGH fixed -- `AlpcConnectPort`/`AlpcAcceptConnectPort` + NT handlers used `NT_SUCCESS(st)` to gate the happy path, but `STATUS_TIMEOUT = 0x00000102` has severity 0 so timeouts silently published valid unconnected handles; switched to strict `st == STATUS_SUCCESS` check. 1 MEDIUM fixed -- `alpc_port_on_delete` queue_drain on ConnectionQueue would kfree pending ALPC_CONNECTION_REQUEST nodes (client-owned memory) without signalling waiters, risking UAF if contract is violated; replaced with a defensive detach-and-event_set loop that warns on contract violation and lets the client free the node. Removed dead `ALPC_CONN_Q` typedef. Added 2 timeout tests (`connect_timeout`, `accept_timeout`) that exercise the fix.

---

## 4. Synchronous Send+Wait+Receive Engine

- [x] Message pool allocator: `AlpcAllocateMessage(DataLength)` → `PORT_MESSAGE_ENTRY*`:
  - Body immediately follows the `PORT_MESSAGE_ENTRY` struct in a single `kmalloc` allocation: `sizeof(PORT_MESSAGE_ENTRY) + DataLength`
  - Enforces `ALPC_MAX_ALLOWED_MESSAGE_LENGTH` limit
  - Enforces per-port `Attributes.MaxPoolUsage` accounting; returns `STATUS_INSUFFICIENT_RESOURCES` if exceeded
- [x] `AlpcFreeMessage(entry)` -- decrements pool accounting, calls `kfree`
- [x] Core engine: `NtAlpcSendWaitReceivePort(PortHandle, Flags, SendMsg, SendMsgAttr, RecvMsg, BufferLen, RecvMsgAttr, Timeout)`:

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

- [x] Concurrency guarantees: `port->Lock` is a spinlock held only for queue manipulation, allocator-quota updates, AND completion-signalling (replier and disconnect call `event_set` while still holding the port lock so a sender that takes the same lock on wake can never observe a freed pending record); long blocking waits happen outside the lock via `event_wait_timeout`. Two-port enqueue (sync request) holds both port locks in **address order** to prevent deadlock with concurrent disconnect.
- [x] `PendingQueue` is per-port (now typed as `ALPC_PENDING_QUEUE` of sender-owned `ALPC_PENDING_REPLY` records, separate from `ALPC_MSG_QUEUE`); entries are looked up by `MessageId` (monotonic u64) and removed by pointer identity to defeat any later MessageId reuse in disconnect/timeout races.
- [x] Multiple threads may call `NtAlpcSendWaitReceivePort` on the same server port concurrently -- each dequeues one message; FIFO order is preserved within the spinlock window; no message is delivered to two threads (covered by `alpc: receive FIFO + truncation`).
- [x] Port destruction race: `AlpcDisconnectPort` acquires `port->Lock`, sets `Disconnected=1`, drains MY `PendingQueue` AND peer's `PendingQueue` (each under its own port's lock), signals every blocked sync waiter with `STATUS_PORT_DISCONNECTED` BEFORE returning. `alpc_port_on_delete` performs the same drain defensively. Sender always frees its own `ALPC_PENDING_REPLY` after observing `Completed=1`, so no path frees memory another path is still writing to.
- [x] Commit `"kernel/ipc/alpc: synchronous send+wait+receive engine with reply routing"` (b8054b5e, pushed)

**Test checkpoint:** Client sends 64-byte request with `ALPC_MSGFLG_SYNC_REQUEST` → client blocks. Server calls `NtAlpcSendWaitReceivePort` (receive-only) → dequeues request with correct `ClientId` and body. Server sends `ALPC_MSGFLG_REPLY_MESSAGE` → client unblocks with reply body intact and matching `MessageId`. Datagram send (no `SYNC_REQUEST`) → message queued, sender returns immediately. Message exceeding `MaxMessageLength` → `STATUS_BUFFER_TOO_SMALL`. Reply with wrong `MessageId` → `STATUS_REPLY_MESSAGE_MISMATCH`. Port disconnected during wait → `STATUS_PORT_DISCONNECTED`. FIFO order + body truncation when caller buffer is smaller than queued message. Late reply after sender timeout → `STATUS_REPLY_MESSAGE_MISMATCH`. Pool quota exceeded → `STATUS_INSUFFICIENT_RESOURCES`. Sync request with no peer → `STATUS_PORT_DISCONNECTED`. Serial log: `"alpc: msg sent: id=N type=DATAGRAM|REQUEST|REPLY len=N"`. Test on: QEMU WHPX + TCG.

> **Test runner:** `scripts\debug\kernel\run-ipc-tests.bat` (SUITE=ipc)
> **Expected:** 17 new alpc §4 suites + 8 §3 suites + 18 §1-§2 suites pass (datagram delivery, sync request+reply, reply mismatch, send oversized, receive empty timeout, disconnect wakes sync waiter, pool quota exceeded, receive FIFO+truncation, sync timeout+late reply, disconnect-then-send, remote disconnect-then-send, PORT_CLOSED uncharged, sync no peer, syscall validation, kmalloc-fail PoolUsageBytes rollback, ReplyBodyCap clamp >65528, two-port lock-order race-barrier stress), 0 failures

> **Verified:** 2026-04-15 -- Evidence mapped: every `[x]` item in §4 backed by `include/kernel/ipc/alpc_port.h` (`ALPC_PENDING_REPLY` + `ALPC_PENDING_QUEUE` + `ChargedSize` on `PORT_MESSAGE_ENTRY` + `PoolUsageBytes` on `ALPC_PORT` + `AlpcAllocateMessage` / `AlpcFreeMessage` / `AlpcSendWaitReceivePort` API), `src/kernel/ipc/alpc_port.c` (engine: `alpc_receive_only`, `alpc_datagram_send`, `alpc_sync_request`, `alpc_reply`, `AlpcSendWaitReceivePort` dispatcher; extended `AlpcDisconnectPort` drains BOTH sides' PendingQueues; extended `alpc_port_on_delete` signals leftover pending records under lock without freeing), `src/kernel/nt/nt_alpc.c` (real `NtAlpcSendWaitReceivePort_handler` with `ProbeForReadIfUser` / `ProbeForWriteIfUser`), `src/kernel/test/test_alpc.c` (14 original §4 suites + 3 retrofit suites added 2026-04-19 closing the original "Test gaps (deferred)" block via TODO-03 §1/§2/§3/§5 primitives). Slot 0x0113 dropped from `test_ob.c` pending sweep. Build clean (`=== BUILD OK ===`). Test checkpoint klog string `alpc: msg sent: id=N type=...` confirmed at 3 sites.
> **Quality reviewed:** 2026-04-15 -- kernel-code-quality Gates 1-11 walked clean. Two Codex dispatches: step-13 adversarial (1 High UAF in datagram klog after publish + 1 Medium status alias `STATUS_INSUFFICIENT_RESOURCES` masking `STATUS_PORT_DISCONNECTED` -- both fixed pre-commit, regression test added) and step-8 quality dispatch (dead-code + consistency + perf): 2 MEDIUM fixed -- (1) PORT_CLOSED marker enqueued via raw `kmalloc` would cause the receiver's `AlpcFreeMessage` fallback to subtract `sizeof(PORT_MESSAGE_ENTRY)` from `peer->PoolUsageBytes` and corrupt charged quota for unrelated entries; fix changed `AlpcFreeMessage` to skip the decrement when `ChargedSize == 0`, with a regression test (`alpc: PORT_CLOSED marker uncharged`) that snapshots `PoolUsageBytes` before/after disconnect; (2) `PORT_MESSAGE_ENTRY` carried four dead v1 sync-wait fields (`ReplyPort`, `ReplyMessageId`, `WaitingForReply`, `ReplySyncWait`) -- the v3 design moved sync state into `ALPC_PENDING_REPLY` but the fields were never deleted, inflating every send-side allocation by ~120 bytes (event_t embeds waiter arrays) and distorting `MaxPoolUsage` accounting; removed all four fields and the `event_init` calls that touched them. 1 LOW improved -- `alpc_memcpy` byte-by-byte loop replaced with the canonical kernel `libc/string.h` `memcpy`, removing duplicate copy primitive. Codex consistency angle: PORT_MESSAGE write order verified (Type/MessageId/CallbackId/ClientId/DataInfoOffset all correct); ChargedSize accounting symmetric across all alloc/free pairs; signal-under-lock invariant holds in `alpc_reply`, `AlpcDisconnectPort`, and `alpc_port_on_delete`; `alpc_lock_two` / `alpc_unlock_two` symmetric in address order; strict `st == STATUS_SUCCESS` (not loose `NT_SUCCESS`) on every TIMEOUT-returning path. Codex perf angle: hot-path allocations (1 or 2 kmalloc per send) acceptable for §4 base case (§11 message zones eliminate them later); `pending_queue_find` O(N) lookup acceptable for typical concurrent-sync-request counts.

---

## 5. Asynchronous Delivery & Completion List

- [x] ALPC completion list: `ALPC_COMPLETION_LIST` + `ALPC_COMPLETION_LIST_ITEM` ABI types defined in `include/kernel/ipc/alpc.h` with `_Static_assert(sizeof(ALPC_COMPLETION_LIST_ITEM) == 24)`. The user-VA-mapped ring-buffer mechanism lands with §6 port sections; §5 implements the notification channel via the existing IOCP infrastructure.
- [x] `NtAlpcSetInformation(port, AlpcAssociateCompletionPortInformation, {CompletionPort, CompletionKey}, len)` -- associates a user-space `IO_COMPLETION_PORT` (the `s_iocp_pool[]` entry opened by `NtCreateIoCompletion`) with the ALPC port. Incoming messages post a notification packet to that IOCP (KeyContext=CompletionKey, ApcContext=MessageId, Information=DataLength) IN ADDITION TO enqueueing on `MessageQueue`. The body remains on MessageQueue so receivers still pull it via `NtAlpcSendWaitReceivePort`; the IOCP is a wake/notification channel, not a replacement delivery vehicle.
- [x] Async message delivery: when the port has an associated completion port, datagram and sync-request sends call `io_completion_post()` after the normal MessageQueue enqueue. A failed post (IOCP full / stale handle) is logged and ignored -- the message is still on MessageQueue and will be delivered via normal receive. Exclusive delivery models were rejected in design review because they would either discard payload (IOCP entries cannot carry variable-length bodies) or duplicate it.
- [x] Server consumes notifications from the completion port via `NtRemoveIoCompletion` (existing handler). Full blocking removal and `NtRemoveIoCompletionEx` remain in the IOCP backlog TODO; the non-blocking path is enough to drive §5 tests and a real-world poll loop.
- [x] Fallback: if no completion port is associated (`port->CompletionPortHandle == 0`), `alpc_notify_completion_port()` early-returns and the send path uses only the synchronous `MessageQueue` + `WaitQueue` path from §4.
- [x] Waitable port: when `ALPC_PORTFLG_WAITABLE_PORT` is set in the port's `Attributes.Flags`, the port caches `IsWaitable=1` and initializes a MANUAL_RESET `SignalledEvent`. `msg_queue_enqueue_locked` sets the event on the 0→1 Count transition; `msg_queue_dequeue_locked` clears it on the 1→0 transition -- both under `port->Lock`, so the event state can never disagree with the queue state a waiter observes. `wait_on_handle` (src/kernel/nt/nt_sync.c) grew an `ObpAlpcPortType` branch that dispatches on `IsWaitable`, returning `STATUS_OBJECT_TYPE_MISMATCH` for non-waitable ports.
- [x] (Bonus fix) `wait_on_handle` Event and Timer branches had an inverted `event_wait_timeout` result mapping (`== 0 ? SUCCESS : TIMEOUT`) that would have reported timeouts as SUCCESS and successes as TIMEOUT. Caught during §5 design review; corrected to `? SUCCESS : TIMEOUT`.
- [x] (Bonus fix) `IO_COMPLETION_PORT` gained a per-port `spinlock_t lock` plus `s_iocp_allocator_lock` guarding `s_iocp_allocated`. The previous "single-threaded today" assumption ceased to hold as soon as kernel ALPC sends started posting completion packets concurrently with user-mode `NtSet/Remove` handlers. All four IOCP handlers (`NtCreate/Set/Remove` + the new `io_completion_post`) now run under the port lock.
- [x] Disconnect's PORT_CLOSED marker is now routed through `msg_queue_enqueue_locked` (was a manual append) so SignalledEvent tracks MessageQueue.Count transitions across every producer, including teardown.
- [x] ~~Commit:~~ `"kernel/ipc/alpc: async completion list, waitable port, IO_COMPLETION integration"`

**Test checkpoint:** Create port with `ALPC_PORTFLG_WAITABLE_PORT`. `NtWaitForSingleObject(port, timeout=0)` returns `STATUS_TIMEOUT` when queue is empty. Send a message → wait returns `STATUS_SUCCESS`. Drain queue → next wait returns `STATUS_TIMEOUT` again. Non-waitable port → `STATUS_OBJECT_TYPE_MISMATCH`. Associate IOCP via `NtAlpcSetInformation(AlpcAssociateCompletionPortInformation)`; send datagram; `NtRemoveIoCompletion` returns a packet with `KeyContext == CompletionKey` and `Information == DataLength`; the message body still dequeues from `MessageQueue` via `NtAlpcSendWaitReceivePort`. Associate with a bogus IOCP handle → `STATUS_INVALID_HANDLE`. Unknown info class → `STATUS_NOT_IMPLEMENTED` (pending §9). Serial log: `"alpc: port associated with IOCP: port_type=N key=0x..."`. Test on: QEMU WHPX + TCG.

> **Test runner:** `scripts\debug\kernel\run-ipc-tests.bat` (SUITE=ipc)
> **Expected:** 6 new alpc §5 suites pass (ALPC_COMPLETION_LIST_ITEM layout, waitable port signal/drain, non-waitable port rejects wait, associate completion port + notification, bad IOCP handle, unknown info class) -- 46 total alpc suites across §1-§5, 0 failures

> **Verified:** 2026-04-15 -- Evidence mapped: every `[x]` item backed by `include/kernel/ipc/alpc.h` (`ALPC_COMPLETION_LIST` + `ALPC_COMPLETION_LIST_ITEM` + static_assert), `include/kernel/ipc/alpc_port.h` (`ALPC_PORT_INFORMATION_CLASS`, `ALPC_PORT_ASSOCIATE_COMPLETION_PORT`, `ALPC_PORT.{CompletionPortHandle, CompletionKey, IsWaitable, SignalledEvent}`, `AlpcAssociateCompletionPort` proto), `include/kernel/nt/nt_file.h` (`IO_COMPLETION_PORT.lock`, `io_completion_post`, `io_completion_validate_handle` protos), `src/kernel/ipc/alpc_port.c` (SignalledEvent init in both port-alloc paths, edge hooks in `msg_queue_enqueue_locked` / `msg_queue_dequeue_locked`, `alpc_notify_completion_port` invoked from datagram + sync-request after enqueue, `AlpcAssociateCompletionPort` body, disconnect PORT_CLOSED routed through helper), `src/kernel/nt/nt_file.c` (per-port + allocator spinlock, init-then-publish in NtCreateIoCompletion, `iocp_index_from_handle`, `iocp_enqueue_locked_call`, `io_completion_post`, `io_completion_validate_handle`), `src/kernel/nt/nt_sync.c` (ObpAlpcPortType branch in `wait_on_handle` + pre-existing inverted timeout mapping fixed), `src/kernel/nt/nt_alpc.c` (real `NtAlpcSetInformation_handler`), `src/kernel/test/test_alpc.c` (6 new TEST_CAT_IPC suites). Slot 0x011D dropped from `test_ob.c` pending sweep. Build clean (`=== BUILD OK ===`).
> **Accepted:** IOCP handle cross-task theft (numeric `idx + 0x10000` with no ownership check -- any task can guess a peer's IOCP index) -> XREF: 03-memory-concurrency/TODO-09 §4 (item: "Retrofit existing in-tree consumers to the new Ob-backed IOCP" at line 128 -- names `iocp_index_from_handle`, `io_completion_post`, and `AlpcAssociateCompletionPort` as the three retrofit consumers plus the cross-task `STATUS_ACCESS_DENIED` test requirement). This is a pre-existing IOCP design limitation not introduced by §3; the §3 spec text itself defers the full Ob-backed IOCP rewrite.
> **Quality reviewed:** 2026-04-15 -- kernel-code-quality Gates 1-11 walked clean. Codex design review caught 1 Critical + 3 High + 1 Medium in the INITIAL plan (payload-discarding IOCP "exclusive delivery", IOCP ring race, disconnect bypassing SignalledEvent helper, inverted timeout mapping) -- all redesigned before a line of code was written: IOCP became a notification channel (body stays on MessageQueue), IOCP got its own spinlock, disconnect now routes through `msg_queue_enqueue_locked`, and the `wait_on_handle` inversion was fixed as an in-scope bonus. Step-13 adversarial review on the implemented code found 1 High (Accepted above) + 1 Medium fixed: `NtCreateIoCompletion_handler` published `s_iocp_allocated` before initializing the slot's head/tail/count/lock, exposing the slot to a concurrent NtSet/Remove/io_completion_post walking uninitialized fields; fix holds the allocator lock through init and publishes last. Codex quality dispatch (post-impl) returned 1 Medium (IOCP backpressure log flood -- added per-port `DroppedNotifications` counter; log only on first failure per port) + 1 Low (ALPC_COMPLETION_LIST full struct layout locked with per-field static_asserts). No dead code; no consistency drift; no perf regressions (two new spinlocks both <100 ns per op).

---

## 6. Large Data: Port Sections & View Mapping

> [!IMPORTANT]
> **Design review 2026-07-17 (Codex needs-attention, 5 findings verified against the codebase) -- implement to THIS contract; the naive plan is NO-SHIP:**
> - **Handle safety (D1):** resolve an existing `SectionHandle` via `ObpLookupHandle` + `OB_HEADER_FROM_BODY(...)->type == ObpSectionType` check + `ObReferenceObject` (there is NO `ObReferenceObjectByHandle`), holding the ref until DeletePortSection. The residual lookup-then-ref race + `SECTION_MAP_*` granted-access enforcement are the repo-wide gap -> XREF: `02-kernel-core/TODO-05 §3` (item: "Add `ObpReferenceObjectByHandle` primitive" at line 148).
> - **Object-direct mapping (D3):** `ObMapViewOfSectionFull` takes a kernel HANDLE, but the port stores a `SECTION_OBJECT*`; add an object-direct `ObMapViewOfSectionObject(SECTION_OBJECT*, pid, offset, size)` + `ObUnmapViewOfSectionObject` (or per-task view-base index) so a registered section still maps after the creator closes its handle.
> - **Cross-port capability (D2):** a port-local section ID is NOT resolvable on the peer's port. The send-path-with-view must pin the sender's `SECTION_OBJECT` into the queued message entry (own a ref), translate to a receiver-scoped ID under address-ordered endpoint locks on receive, and release the ref on every consume / drop / teardown path.
> - **Attribute ABI + TOCTOU (D5):** the 6-arg `NtAlpcSendWaitReceivePort` SSDT slot is already full; define a probed packed extension carrying `SendMsgAttr` / `RecvMsgAttr` + explicit lengths. `AlpcpValidateMessageAttributes` must COPY each buffer once into kernel memory and validate the snapshot (subtraction-based bounds, reject unknown bits) -- never validate-then-deref a live user buffer.
> - **Identity safety (D4):** REJECT `ALPC_TOKEN_ATTR` / `ALPC_WORK_ON_BEHALF_ATTR` bits in §6 (define layout only); their provenance + reference lifetime is owned by §7. Marshalling spoofable identity before §7 lands is a trust-boundary hole.
> - **Lock order:** take `port->Lock` ONLY for `SectionList` insert/remove; call `ObCreateSectionEx` / `ObMapViewOfSectionObject` / unmap OUTSIDE any port lock (no `ALPC_PORT.Lock` -> `SECTION_OBJECT.lk` nesting).

- [ ] `ALPC_PORT_SECTION` node on `port->SectionList` (`{Head,Tail,Count}` + `Link_next` idiom, NOT `list_head_t`):
  - `Link_next` (self ptr); `AlpcSectionHandle` (u64 opaque ID, per-port monotonic counter, reserve 0, fail at `UINT64_MAX`)
  - `Section` (`SECTION_OBJECT*`, ref-held until delete); `Size`; `DeleteOnClose`
- [ ] Prerequisite: object-direct `ObMapViewOfSectionObject` / `ObUnmapViewOfSectionObject` primitives in `ob_section.c` (D3) -- refactor `ObMapViewOfSectionFull` to split handle-resolution from object-map so ALPC can map a stored `SECTION_OBJECT*`.
- [ ] Prerequisite: probed packed attribute-carrying syscall ABI for `NtAlpcSendWaitReceivePort` (D5) -- the 6-arg slot is full; carry `SendMsgAttr`/`RecvMsgAttr` + lengths, copy-once snapshot validation.
- [ ] `NtAlpcCreatePortSection(PortHandle, Flags, SectionHandle, SectionSize, AlpcSectionHandle, ActualSectionSize)`:
  - Resolve an existing `SectionHandle` via `ObpLookupHandle` + `ObpSectionType` check + `ObReferenceObject` (D1), OR if `SectionHandle == NULL` call `ObCreateSectionEx` for an anonymous section of `SectionSize`
  - Allocate `ALPC_PORT_SECTION`, append to `port->SectionList` under `port->Lock`, assign a unique per-port `AlpcSectionHandle`
- [ ] `NtAlpcDeletePortSection(PortHandle, Flags, SectionHandle)` -- remove from list, dereference section object
- [ ] View mapping: `ALPC_DATA_VIEW_ATTR` message attribute -- `Flags`, `SectionHandle` (`ALPC_SECTION_HANDLE` opaque ID), `ViewBase` (out, caller VA), `ViewSize`.
- [ ] `NtAlpcCreateSectionView(PortHandle, Flags, DataView)` -- object-direct map (D3) into caller VA, outside the port lock; fills `ViewBase` / `ViewSize`.
- [ ] `NtAlpcDeleteSectionView(PortHandle, Flags, ViewBase)` -- unmap via per-task view-base index; the view holds its own section ref (survives port-section delete).
- [ ] Send path with view (connection-scoped capability, D2):
  - Pin the sender's `SECTION_OBJECT` into the queued message entry (own a ref)
  - On receive, translate to a receiver-scoped `AlpcSectionHandle` under address-ordered endpoint locks
  - Release the message-held ref on every consume / drop / disconnect / teardown path (a port-local ID is NOT valid on the peer)
- [ ] Message attributes dispatcher: `ALPC_MESSAGE_ATTRIBUTES` struct passed to send/receive:
  - Bitfield `ValidAttributes` selects which attribute structs are present
  - `ALPC_DATA_VIEW_ATTR` (bit 0x1) -- view for large data
  - `ALPC_CONTEXT_ATTR` (bit 0x2) -- port/message context values
  - `ALPC_HANDLE_ATTR` (bit 0x4) -- handle duplication across the port
  - `ALPC_SECURITY_ATTR` (bit 0x8) -- security context handle / QoS (for §7)
  - `ALPC_TOKEN_ATTR` (TOKEN) -- per-message sender token identity; define layout + marshalling here, token reference lifetime / impersonation / teardown owned by §7 (a ValidAttributes bit alone is NOT sufficient -- trust boundary)
  - `ALPC_WORK_ON_BEHALF_ATTR` (WORK_ON_BEHALF_OF) -- RPC caller work-ticket identity propagated across chained ALPC hops (priority-boost / deadlock-avoidance); layout here, provenance validation owned by §7
- [ ] `AlpcpValidateMessageAttributes(attrs, buffer_len)` (D4/D5):
  - COPY attrs once into kernel memory; validate the SNAPSHOT (subtraction-based bounds), never a live user buffer
  - Reject unknown bits; reject `ALPC_TOKEN_ATTR` / `ALPC_WORK_ON_BEHALF` until §7 provenance lands
- [ ] Commit: `"kernel/ipc/alpc: port sections, view mapping, message attributes dispatch"`

**Test checkpoint:** `NtAlpcCreatePortSection(NULL, 64*1024)` creates anonymous 64 KiB section. `NtAlpcCreateSectionView` maps it into caller's VA; writing a pattern to `ViewBase` succeeds. Send message with `ALPC_DATA_VIEW_ATTR`; receiver calls `NtAlpcCreateSectionView` with received `SectionHandle`; reads back matching pattern; zero copies. `NtAlpcDeleteSectionView` unmaps without crash. `NtAlpcDeletePortSection` dereferences section. `AlpcpValidateMessageAttributes` rejects invalid `buffer_len`. Serial log: `"[ALPC] section view mapped: base=%p size=%llu"`. Test on: QEMU WHPX + TCG.

> **Deferred:** [Critical] naive §6 plan is NO-SHIP per Codex design review 2026-07-17 (5 findings verified) -- needs an object-direct section-map primitive (D3), an attribute-carrying syscall ABI (D5), connection-scoped capability transfer with message-held refs (D2), spoof-safe identity handling (D4), and safe handle-ref -> XREF: `02-kernel-core/TODO-05 §3` (item: "Add `ObpReferenceObjectByHandle` primitive" at line 148). Full design contract captured in the §6 `[!IMPORTANT]` callout; implement in a focused session (design done, code deferred).

---

## 7. Security: Client Token Capture & Impersonation

- [ ] Security context capture at accept: in `NtAlpcAcceptConnectPort` (§3), if `AcceptConnection == TRUE` AND `port->Attributes.SecurityQos.ImpersonationLevel >= SecurityIdentification`:
  1. Locate the client task from `connection_request->Header.ClientId`
  2. `PsReferencePrimaryToken(client_task)` → `client_token`
  3. `NtDuplicateToken(client_token, TOKEN_QUERY | TOKEN_IMPERSONATE, NULL, SecurityQos.EffectiveOnly, TokenImpersonation, &captured_token)` -- duplicate at the requested impersonation level; `EffectiveOnly` strips disabled attrs
  4. Store `captured_token` in `server_comm_port->ClientToken`
  5. `PsDereferencePrimaryToken(client_token)`

- [ ] Server impersonation: `NtAlpcImpersonateClientOfPort(PortHandle, Message, Reserved)`:
  1. `ObReferenceObjectByHandle(PortHandle)` → must be a server communication port
  2. Retrieve `port->ClientToken`; check it is not NULL
  3. `SeImpersonateClientEx(port->ClientToken, current_task)` sets `current_task->ImpersonationToken = port->ClientToken` (ref-counted copy) (→ XREF `TODO-15-security-reference-monitor.md §7`)
  4. Requires `SeImpersonatePrivilege` on the server's token if the client's IL is higher than the server's IL (→ XREF `TODO-15-security-reference-monitor.md §3`)

- [ ] Security attribute in message: `ALPC_SECURITY_ATTR` (bit 0x8 in `ValidAttributes`):
  - On send: client includes its `SECURITY_QUALITY_OF_SERVICE` preferences
  - On receive: server reads the `ContextHandle` to call `NtAlpcImpersonateClientOfPort` without needing a separate call
- [ ] `NtAlpcCreateSecurityContext(PortHandle, Flags, SecurityAttribute)` -- pre-creates a security context handle to avoid repeated per-message token capture; stored in `ALPC_SECURITY_ATTR.ContextHandle`
- [ ] `NtAlpcDeleteSecurityContext(PortHandle, Flags, ContextHandle)` -- revokes and frees the captured context
- [ ] Client connection SID verification: `RequiredServerSid` parameter to `NtAlpcConnectPort`: if non-NULL, kernel reads the server port owner's `UserSid` from `port->ClientToken` (set when server was created) and compares it using `RtlEqualSid`; returns `STATUS_SERVER_SID_MISMATCH` if different; prevents clients from connecting to hijacked ports
- [ ] Token / work-on-behalf provenance ownership (backs the §6 `ALPC_TOKEN_ATTR` / `ALPC_WORK_ON_BEHALF_ATTR` layouts):
  - Snapshot + reference the sender's token at send time; release the reference on message free and on port teardown
  - Reject impersonation changes after enqueue; validate carried thread identity against `PORT_MESSAGE.ClientId` (anti-spoof at the trust boundary)
  - Tests: impersonating-sender + stale-thread rejection
- [ ] `NtAlpcConnectPortEx` (SSDT 0x0111): security-descriptor variant of the `RequiredServerSid` check:
  - Import a bounded `PSECURITY_DESCRIPTOR ServerSecurityRequirements` and call `SeAccessCheck` (→ XREF `TODO-15-security-reference-monitor.md §5`) instead of simple SID equality
  - Lower priority than the base SID check; handler wired by the §8 retrofit
- [ ] Self-contained execution note (see callout below).
> [!NOTE]
> `SeImpersonateClientEx` (T15 §7) is not yet implemented. §7 (NtAlpcImpersonateClientOfPort) should implement a minimal inline version: `task->ImpersonationToken = ObReferenceObject(port->ClientToken)` with refcount management. Full privilege checks (`SeImpersonatePrivilege`, IL comparison) are deferred to T15 §7-§8; add `// TODO: SeImpersonatePrivilege check T15 §8` stub.

- [ ] Commit: `"kernel/ipc/alpc: client token capture, NtAlpcImpersonateClientOfPort, SID verification"`

**Test checkpoint:** Server accepts connection from client task. `server_comm->ClientToken` is non-NULL and points to a duplicate of client's primary token. `NtAlpcImpersonateClientOfPort` sets `task_current()->ImpersonationToken` to the captured token. `RequiredServerSid` mismatch → `STATUS_SERVER_SID_MISMATCH`. `NtAlpcCreateSecurityContext` returns valid `ContextHandle`. `NtAlpcDeleteSecurityContext` revokes it. Serial log: `"[ALPC] client token captured: SID=%s"`, `"[ALPC] impersonating client: SID=%s"`. Test on: QEMU WHPX + TCG.

---

## 8. NtAlpc* SSDT Registration & Stub Retrofit

- [ ] SSDT entries: add the following NtAlpc* entry points to the SSDT (→ XREF `TODO-12 §5`):
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
  NtAlpcCreateResourceReserve (per-message reusable reservation for guaranteed delivery; DISTINCT mechanism owned by §11, not the zone pool)
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
- [ ] **Retrofit the 16 ALPC SSDT stubs in `src/kernel/nt/nt_alpc.c` (SSDT 0x010F-0x011E)** to real handlers that call into the ALPC engine instead of returning `STATUS_NOT_IMPLEMENTED`. Covered syscalls: `NtAlpcCreatePort` (0x010F), `NtAlpcConnectPort` (0x0110), `NtAlpcConnectPortEx` (0x0111), `NtAlpcAcceptConnectPort` (0x0112), `NtAlpcSendWaitReceivePort` (0x0113; canonical message rendezvous; needs `ALPC_MESSAGE_ATTRIBUTES` decode for handle/context/view attrs), `NtAlpcDisconnectPort` (0x0114), `NtAlpcCancelMessage` (0x0115), `NtAlpcCreatePortSection` (0x0116; wraps `NtCreateSection` with port association), `NtAlpcDeletePortSection` (0x0117), `NtAlpcCreateSectionView` (0x0118), `NtAlpcDeleteSectionView` (0x0119), `NtAlpcCreateResourceReserve` (0x011A; per-message reusable reservation mechanism owned by §11, distinct from the zone pool), `NtAlpcDeleteResourceReserve` (0x011B), `NtAlpcQueryInformation` (0x011C; all 11 `ALPC_PORT_INFORMATION_CLASS` values from §9), `NtAlpcSetInformation` (0x011D), `NtAlpcQueryInformationMessage` (0x011E; 2 `ALPC_MESSAGE_INFORMATION_CLASS` values from §9). Each retrofit drops the `SCOPE-GAP-ALLOWED` sentinel in `nt_alpc.c`, adds a handler body that probes user buffers via `ProbeForReadIfUser`/`ProbeForWriteIfUser`, resolves the port handle via `ObpReferenceObjectByHandle` (-> XREF TODO-05 §3), and dispatches to the matching ALPC engine helper (`alpc_port_create`, `alpc_port_connect`, `alpc_send_wait_receive`, `alpc_disconnect`, `alpc_query_info`, etc.). This auto-closes TODO-05 §31 (16 ALPC syscalls deferred here). Additional Vista+ ALPC syscalls listed in the §8 syscall list above (`NtAlpcCreateSecurityContext`, `NtAlpcDeleteSecurityContext`, `NtAlpcImpersonateClientOfPort`) require fresh SSDT numbers in `service_numbers.h` and are not covered by this retrofit.
- [ ] Connection-message negotiation (full-width connect/accept ABI):
  - The compact 6-arg SSDT handlers for `NtAlpcConnectPort` / `NtAlpcAcceptConnectPort` currently discard `PortContext`, `ConnectionMessage`, and `ConnMsgAttr` (`nt_alpc.c` `(void)a5; (void)a6;`; §3 deferred these here to nowhere)
  - Wire the full-width ABI: bounded copy of the connection-request `PORT_MESSAGE` + `ALPC_MESSAGE_ATTRIBUTES` through connect → accept/reject → timeout → cancel; propagate `PortContext`; return the accept-side reply message
  - Required for CSRSS/RPC connect-time negotiation. Test: connect-time payload round-trip + context propagation
- [ ] Commit: `"kernel/ipc/alpc: NtAlpc* SSDT registration, stub retrofit, OpenSender/RevokeSecurityContext gap items"`

---

## 9. NtAlpc QueryInformation, SetInformation & CancelMessage

- [ ] NtAlpcQueryInformation: `ALPC_PORT_INFORMATION_CLASS` values (full enumeration):
  - `AlpcBasicInformation` (0) → `{Flags, SequenceNo, PortContext}`
  - `AlpcPortInformation` (1) → full `ALPC_PORT_ATTRIBUTES` readback
  - `AlpcAssociateCompletionPortInformation` (2) → read back associated completion port (writeable via `NtAlpcSetInformation` in this section)
  - `AlpcConnectedSIDInformation` (3) → SID of the connected peer process
  - `AlpcServerInformation` (4) → `{ThreadBlocked, ConnectedProcessId, ConnectionNtPath}`
  - `AlpcMessageZoneInformation` (5) → message zone status (→ §11)
  - `AlpcRegisterCompletionListInformation` (6) / `AlpcUnregisterCompletionListInformation` (7) / `AlpcAdjustCompletionListConcurrencyCountInformation` (8) / `AlpcRegisterCallbackInformation` (9) / `AlpcCompletionListRundownInformation` (10) -- these are completion-LIST lifecycle operations issued via `NtAlpcSetInformation` (NOT queries). The user-VA mapped completion-list ring was NOT built (§5 chose the IOCP-notify channel instead). Return `STATUS_NOT_IMPLEMENTED` with rationale -- do NOT alias to IOCP: IOCP posts metadata while the payload stays on `MessageQueue` and has no shared-list registration / concurrency / rundown semantics, so aliasing would silently break async consumers. If a real mapped completion-list ring is ever needed, it lands as a new section, not an alias.
- [ ] Return `STATUS_INVALID_INFO_CLASS` for unknown classes
- [ ] `NtAlpcQueryInformationMessage(PortHandle, Flags, Message, MessageInformationClass, Buffer, Length, ReturnLength)`:
  - `AlpcMessageSidInformation` (0) → returns the SID of the sender of the specified message (derived from captured client token at send time)
  - `AlpcMessageTokenModifiedIdInformation` (1) → returns the `ModifiedId` LUID of the sender's token at send time; allows detecting if the token changed between send and receive

- [ ] NtAlpcSetInformation: `AlpcPortAssociateCompletionPortInformation` (2); set completion port (§5 completion list)
- [ ] `AlpcBasicInformation` (0); update `MaxMessageLength`, `MemoryBandwidth` (only if no messages are pending)
- [ ] `AlpcDirectMessageAttribute` (3); set default message type for datagrams
- [ ] `NtAlpcCancelMessage(PortHandle, Flags, MessageContext)`:
  - Find entry on `port->PendingQueue` or `port->MessageQueue` matching `MessageContext->MessageId`
  - Remove from queue; wake any blocked sender with `STATUS_CANCELLED`
  - Used by timeout path in `NtAlpcConnectPort` and `NtAlpcSendWaitReceivePort`

- [ ] Commit: `"kernel/ipc/alpc: NtAlpc* SSDT wiring, QueryInformation, SetInformation, CancelMessage"`

**Test checkpoint:** Every `NtAlpc*` entry point listed in the §8 syscall list plus any §8 gap items (OpenSender*, RevokeSecurityContext) is registered in the SSDT and callable from user mode (or kernel-mode `Zw*` alias). `NtAlpcQueryInformation(AlpcBasicInformation)` returns valid `{Flags, SequenceNo, PortContext}`. `NtAlpcQueryInformation` with unknown class → `STATUS_INVALID_INFO_CLASS`. `NtAlpcQueryInformationMessage(AlpcMessageSidInformation)` returns sender SID. `NtAlpcCancelMessage` cancels a pending request → blocked sender wakes with `STATUS_CANCELLED`. Serial log: `"[ALPC] SSDT wired: %u NtAlpc* entries"`. Test on: QEMU WHPX + TCG.

---

## 10. CSRSS ApiPort Bootstrap

- [ ] CSRSS as the first ALPC server: CSRSS (`src/apps/csrss/csrss.c`): the Win32 subsystem server process (→ XREF `12-user-platform-sdk/TODO-05-win32-subsystem.md`); it is the first real user-mode process that uses ALPC; this section defines only the kernel-side bootstrap contract
- [ ] On kernel init Phase 3 (→ XREF `TODO-01-kernel-init-sequencing.md §5`): spawn CSRSS as a `SYSTEM`-token process before any other user processes; CSRSS calls:
  ```c
  NtAlpcCreatePort(&ApiPort,
      &ObjAttr(L"\\Windows\\ApiPort"),
      &PortAttrs{ .MaxMessageLength = 512,
                  .Flags = ALPC_PORTFLG_SYSTEM_PROCESS });
  ```
- [ ] `\Windows\ApiPort` must be resolvable in the Ob namespace (→ XREF `TODO-05-object-manager.md §3`); add `\Windows\` directory creation to Phase 1 Ob init
- [ ] Generalize named-port path resolution beyond `\RPC Control\` (create + connect):
  - `alpc_probe_and_split` (`nt_alpc.c`) + `AlpcCreatePort` (`alpc_port.c`) hard-code the `\RPC Control` root, so `\Windows\ApiPort` is rejected `STATUS_OBJECT_NAME_INVALID`
  - Accept an allowlist of authorized absolute roots (`\RPC Control\`, `\Windows\`), resolving the parent directory generically via `ObLookupObjectByName`
  - Test: user-mode create AND connect to `\Windows\ApiPort`
- [ ] Win32 process creation notification: every new process calls `NtAlpcConnectPort(L"\\Windows\\ApiPort", ...)` at startup in its CRT0 path (→ XREF `12-user-platform-sdk/TODO-04-ntdll-user-runtime.md §5` *(planned)*); sends `CsrClientConnectToServer` message with `{ProcessId, ThreadId, WindowsVersion, SubsystemType=IMAGE_SUBSYSTEM_WINDOWS_GUI/CUI}`
- [ ] CSRSS accepts, allocates a `CSR_PROCESS` record, stores `PortContext`, and replies with a session ID and the address of the CSR shared section
- [ ] From this point, Win32 console I/O, `CreateProcess`, `CreateThread`, and exception notification all flow through this ALPC connection
- [ ] Message format compatibility: CSRSS message format uses a fixed 32-byte opcode area before the variable payload; matches the NT 5.x LPC message format for compatibility with the existing ntdll stubs that will be ported from the Win32 layer:
  ```c
  typedef struct {
      PORT_MESSAGE Header;
      uint32_t     ApiNumber;    /* CSRSS API index */
      NTSTATUS     ReturnValue;
      uint32_t     Reserved;
      uint8_t      Payload[CSRSS_MAX_PAYLOAD]; /* up to 476 bytes */
  } CSRSS_API_MSG;
  ```

- [ ] Commit: `"kernel/ipc/alpc: CSRSS ApiPort bootstrap, Win32 process registration protocol"`

**Test checkpoint:** After Phase 3 init, `\Windows\ApiPort` is resolvable in the Ob namespace. Test `hello.exe` calls `NtAlpcConnectPort("\\Windows\\ApiPort")` → connection accepted by CSRSS. CSRSS replies with session ID. `hello.exe` receives the reply with valid `CSRSS_API_MSG.ReturnValue == STATUS_SUCCESS`. Multiple test processes connect → each gets independent session. Serial log: `"[CSRSS] ApiPort created"`, `"[CSRSS] client connected: PID=%u SessionId=%u"`. Test on: QEMU WHPX + TCG.

---

## 11. Message Zones & Resource Reserves (Pre-Allocated Buffers)

High-throughput ports like `\Windows\ApiPort` (CSRSS) process thousands of messages per second. The default per-message `kmalloc`/`kfree` cycle adds allocation pressure and cache misses. A message zone pre-allocates a contiguous buffer pool per port, and message allocations come from the zone via bump-pointer until full, falling back to `kmalloc` only when the zone is exhausted. Win11 uses `AlpcMessageZoneInformation` for this; Linux has no equivalent; Unix sockets allocate per-message `sk_buff` structs from the slab. Resource reserves (`NtAlpcCreateResourceReserve`) are a DISTINCT mechanism this section also owns: a per-creation, reference-counted, reusable single-message reservation a client pins by `ResourceId` for guaranteed-delivery repeated same-size sends, surviving the memory pressure that would starve the zone/kmalloc paths. Not an alias of the zone pool: the zone is one shared bump-allocator, a reserve is one dedicated reusable buffer with independent lifetime.

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
- [ ] Define `ALPC_RESERVE` (DISTINCT from `ALPC_MESSAGE_ZONE`) + create/delete syscalls:
  - Reference-counted, keyed by a per-creation `ResourceId`; holds `OwnerPort` / `HandleTable` / cached `PORT_MESSAGE_ENTRY` / `Size` / `Active` flag
  - `NtAlpcCreateResourceReserve(PortHandle, Flags, MessageSize, ResourceId)` pins a reusable single-message buffer; `NtAlpcDeleteResourceReserve(PortHandle, Flags, ResourceId)` releases it
- [ ] Reserve send-path + teardown:
  - A send referencing a valid `ResourceId` reuses the pinned reserve (no allocation, survives a `kmalloc` failure) instead of the zone / kmalloc path
  - On disconnect / port teardown, tear down all reserves; a refcount prevents freeing an in-flight reserve
  - Test: create a reserve, send repeatedly under a forced `kmalloc` failure -- delivery still succeeds; delete frees it
- [ ] Commit: `"kernel/ipc/alpc: message zones + resource reserves (pre-allocated per-port buffers)"`

**Test checkpoint:** Create port with 16 KiB message zone. Send 100 messages of 128 bytes each. Verify all allocate from zone (no `kmalloc` calls). Send messages until zone is full. Verify fallback to `kmalloc` succeeds. After all messages are freed, verify zone resets (`UsedBytes == 0`). `NtAlpcQueryInformation(AlpcMessageZoneInformation)` returns correct `ZoneSize` and `UsedBytes`. Resource reserve: `NtAlpcCreateResourceReserve` pins a buffer; repeated sends succeed under a forced `kmalloc` failure; `NtAlpcDeleteResourceReserve` frees it. Serial log: `"[ALPC] Zone alloc: port=%s size=%u used=%u"`. Test on: QEMU WHPX + TCG.

---

## 12. Live Port Monitor & IPC Profiler

- [ ] Per-port message latency histogram (see callout below).
> [!TIP]
> Neither Windows (no built-in per-port latency tracking) nor Linux (no kernel-level IPC profiling beyond `perf`/`ftrace`) provides a first-class, always-on message latency profiler for IPC. Impossible OS embeds a lightweight histogram directly in every `ALPC_PORT`, recording send-to-reply round-trip time in microsecond buckets. Developers see per-port P50/P95/P99 latencies in `alpcmon.exe` without attaching a debugger or running an ETW trace.

> [!NOTE]
> The histogram is designed for sub-1% steady-state overhead (per the 2026 low-overhead ring-tracer research bar): fixed-bucket atomic increments only, no per-message allocation, no lock on the record path.

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
- [ ] Kernel query API: `NtQuerySystemInformation(SystemAlpcPortInformation, ...)`:
  - Walk all named ports in `\RPC Control\` and `\Windows\` Ob directories
  - Per-port: name, owner PID, pending message count, pending connection count, connected ports count, max message length, stats (total sent/received/bytes, latency histogram)
  - Return as array of `SYSTEM_ALPC_PORT_INFORMATION` structs

- [ ] alpcmon.exe app `src/apps/alpcmon/alpcmon.c`: live view of active ALPC ports:
  - Refreshes every 1 s via `NtQuerySystemInformation` poll
  - Table view: Port Name | Owner PID | Owner Image | Pending Msgs | Clients | Msg/s | Bytes/s | P50 us | P99 us
  - Click row → detail pane: full path, creation time, security descriptor (SDDL string), message history (last 16 message IDs + types), latency histogram bar chart
  - Right-click → "Disconnect port" (requires `SeDebugPrivilege`)
  - Right-click → "Reset stats" (zero counters for this port)

- [ ] Commit: `"kernel/ipc/alpc: alpcmon.exe live port monitor with per-port latency profiler"`

**Test checkpoint:** Create port, send 50 synchronous request+reply messages. Query `SystemAlpcPortInformation`. Verify `TotalSent == 50`, `TotalReceived == 50`, at least one `LatencyBuckets` entry > 0. Run `alpcmon.exe`. Verify `\Windows\ApiPort` shows in the table with non-zero Msg/s after CSRSS boot test. Serial log: `"[ALPC] Port stats: %s sent=%llu recv=%llu P50=%lluus"`. Test on: QEMU WHPX + TCG.

---

## OS Comparison

| ⭐   | Feature                      | 🪟 Win11               | 🐧 Linux                 | 🚀 Impossible OS                      |
| --- | ---------------------------- | --------------------- | ----------------------- | ------------------------------------ |
| 💎   | Connection-oriented ports    | ✅ ALPC                | ⚠️ SOCK_SEQPACKET       | ⬜ §2-§3                              |
| 💎   | Sync send+wait+reply         | ✅ Full                | ⚠️ No typed reply       | ✅ §4 done (datagram + sync + reply)  |
| 💎   | Async completion delivery    | ✅ Full                | ⚠️ io_uring (RFC 2026)  | ✅ §5 (IOCP notify + waitable port)   |
| 💎   | Large data via section       | ✅ Port sections       | ⚠️ Manual mmap          | ⬜ §6                                 |
| 💎   | Client identity capture      | ✅ Full                | ⚠️ SCM_CREDENTIALS      | ⬜ §7                                 |
| 💎   | Named port namespace         | ✅ `\\RPC Control\\`   | ⚠️ Abstract sockets     | ⬜ §2                                 |
| 💎   | CSRSS subsystem server       | ✅ Full                | ❌ N/A                   | ⬜ §10                                |
| 💎   | Handle dup across port       | ✅ ALPC_HANDLE_ATTR    | ⚠️ SCM_RIGHTS           | ⬜ D03 T09 §8                         |
| 💎   | Connection SID verification  | ✅ Full                | ⚠️ SO_PEERPIDFD (2023+) | ⬜ §7 (SID verify)                    |
| 💎   | Per-message SID query        | ✅ Full                | ❌ N/A                   | ⬜ §9 (NtAlpcQueryInformationMessage) |
| 💎   | Open sender proc/thread      | ✅ NtAlpcOpenSender*   | ⚠️ peer creds / pidfd   | ⬜ §8 gap items                       |
| 💎   | Revoke security context      | ✅ NtAlpcRevoke*       | ❌ N/A                   | ⬜ §8 gap items                       |
| 💎   | Message zones                | ✅ AlpcMessageZone     | ❌ Per-msg sk_buff       | ⬜ §11                                |
| 💎   | Completion list lifecycle    | ✅ Register/Unregister | ❌ N/A                   | ⬜ §9 not-impl (IOCP subst.)          |
| 💎   | Message identity attrs       | ✅ TOKEN + WoB attr    | ⚠️ SCM_CREDENTIALS      | ⬜ §6 layout + §7 provenance          |
| 💎   | Connection-msg negotiation   | ✅ Connect/accept msg  | ⚠️ connect() payload    | ⬜ §8 (full-width ABI)                |
| 💎   | Resource reserves (per-msg)  | ✅ NtAlpcCreateReserve | ❌ N/A                   | ⬜ §11 (distinct from zones)          |
| ⭐   | Per-port latency histogram   | ❌ ETW only            | ❌ ftrace only           | ⬜ §12 (histogram)                    |
| ⭐   | Live port monitor + profiler | ❌ WinObj read-only    | ❌ N/A                   | ⬜ §12 (alpcmon)                      |

> **Deferred features (→ other TODOs):**
> - Handle attribute marshalling and direct/indirect mode → `03-memory-concurrency/TODO-09-win32-ipc-extensions.md §9`
> - LPC compatibility syscalls (`NtCreatePort`, `NtConnectPort`, etc.) → `03-memory-concurrency/TODO-09-win32-ipc-extensions.md §8`

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
  - Resource reserve: create a reserve, repeated sends succeed under a forced `kmalloc` failure; delete frees it
  - Message identity attrs: send with `ALPC_TOKEN_ATTR` / `ALPC_WORK_ON_BEHALF_ATTR`; receiver reads sender provenance; stale-thread + post-enqueue impersonation rejected
  - Connection-message negotiation: connect with a connection `PORT_MESSAGE` + attrs; server receives the payload + `PortContext` on accept; round-trip intact
  - Namespace generalization: create AND connect `\Windows\ApiPort` succeeds (not only `\RPC Control\`)
  - Completion-list lifecycle: `NtAlpcSetInformation(AlpcRegisterCompletionListInformation)` returns `STATUS_NOT_IMPLEMENTED` (IOCP-notify substitute, not aliased)
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
- [ ] **Remaining limits**: I/O completion port integration (§5) requires `NtCreateIoCompletion` / `NtRemoveIoCompletion` from `05-storage-filesystems`; handle duplication attribute (`ALPC_HANDLE_ATTR`) and LPC compat wrappers deferred to `03-memory-concurrency/TODO-09-win32-ipc-extensions.md §8-§9`; ALPC debugging/tracing via ETW: no owning TODO yet; track under `10-platform-services/TODO-12-long-term-features.md` when telemetry or ETW-like tracing is scoped (or add a subsection in `TODO-08-win32-api-surface.md` if user-mode only).
- [ ] Commit: `"kernel/ipc/alpc: ALPC complete -- port objects, connection handshake, sync send+reply, async completion, port sections, security, message zones, CSRSS ApiPort, alpcmon"`

**Test runner:** `scripts/debug/kernel/run-ipc-tests.bat` (SUITE=ipc)

---

## History

| Date | Action | Summary |
|------|--------|---------|
| 2026-04-14 | validate | Structural validate-todo-file pass: folded ### N.M into checklist lines, ASCII range hyphens, TEST_CAT_IPC, padded OS Comparison, ETW deferral pointer, `run-ipc-tests.bat` runner line, bare-metal + VBox platform sweep bullet, History section. |
| 2026-04-14 | gap-analysis | Win11/Linux IPC research (6+ web searches); code-truth: `nt_alpc.c` stubs + `test_ob.c`; IMPORTANT + Inputs refresh; §8 split into §8 SSDT + §9 Query/Set (12 sections); OpenSender* + RevokeSecurityContext gap items; OS rows; XREF updates in TODO-12, TODO-A, TODO-17, `nt_alpc`/`nt_lpc` headers. |
| 2026-04-14 | validate | validate-todo-file: planned Inputs paths; flat § refs (no §N.M); XREF colons; removed blanks between adjacent checkboxes; ASCII latency comment; OS §12 row text; padded Impl Order row 9. |
| 2026-04-14 | implement | §1 Message header / port attributes / type codes: `include/kernel/ipc/alpc.h` (PORT_MESSAGE 40B + SQOS 12B + ALPC_PORT_ATTRIBUTES 80B with sizeof + per-field offset `_Static_assert`, 10 msg-type codes, 4 PORTFLG + 6 MSGFLG non-overlap asserts), `src/kernel/ipc/alpc.c` (`alpc_init` klog), `src/kernel/test/test_alpc.c` (7 suites under `TEST_CAT_IPC`), pipe_init -> boot_result_t; Codex adversarial review (2 highs: ABI-claim softened to explicit native-layout + PORT_ATTRIBUTES locked); `boot_phase3` wiring + TODO-01 §8 pipe_init checked. |
| 2026-04-14 | review | §1 review-todo-section: Phase-1 evidence + scope-gap + build clean; Phase-2 adversarial already ran during implement; Phase-3 kernel-code-quality Gates 1-11 walked clean + second Codex quality dispatch (dead-code + consistency + perf) found 2 highs: duplicate `CLIENT_ID` (fixed by reusing canonical 16B type from `ob/teb.h` + re-asserting locally) and duplicate `SECURITY_IMPERSONATION_LEVEL` (fixed by reusing canonical enum from `security/token.h`); PORT_MESSAGE offsets re-shifted MessageId 16->24 and CallbackId 24->32; trailing Reserved u64 dropped; test layout + TODO spec synced; full build passes proving coexistence with both consumer headers. |
| 2026-04-15 | implement | §2 ALPC_PORT object + registration: new `include/kernel/ipc/alpc_port.h` (ALPC_PORT body, PORT_MESSAGE_ENTRY, ALPC_MSG_QUEUE, ALPC_PORT_STATS stub), new `src/kernel/ipc/alpc_port.c` (`alpc_port_init` registers ObpAlpcPortType + creates `\RPC Control`, `alpc_port_on_delete` drains queues under lock/free outside, `AlpcCreatePort` kernel helper with **handle-first rollback** ordering), NtAlpcCreatePort retrofitted with ProbeForRead/WriteIfUser + `alpc_probe_and_split` that **copies leaf into kernel-owned 64-byte buffer** (TOCTOU fix); slot 0x010F removed from pending sweep in test_ob.c; 10 new IPC tests (port type + \RPC Control + named/unnamed create + duplicate reject + NULL ht + embedded slash reject + syscall full-path + syscall bad-prefix reject). Codex adversarial review -- 3 highs (full-path-as-leaf, missing user-pointer probes, half-created namespace entry on handle-fail) all fixed; re-dispatch found 1 high TOCTOU (leaf still pointed into user buf), fixed with kernel-owned copy. Added `STATUS_OBJECT_NAME_INVALID = 0xC0000033` to ntstatus.h. |
| 2026-04-15 | review | §3 review-todo-section: Phase-1 evidence + scope-gap + build clean; Phase-2 adversarial already ran during implement; Phase-3 kernel-code-quality Gates 1-11 walked + Codex quality dispatch (dead-code + consistency + perf) found 3 more: 2 HIGH fixed -- STATUS_TIMEOUT passes NT_SUCCESS (severity 0), so both `Alpc*` helpers and Nt handlers silently published handles on timeout; switched to strict `st == STATUS_SUCCESS` gates. 1 MEDIUM fixed -- `alpc_port_on_delete` kfreed pending ALPC_CONNECTION_REQUEST nodes (client-owned memory) without signalling waiters; replaced with defensive detach-and-event_set that warns on contract violation and lets clients free. Removed dead `ALPC_CONN_Q` typedef. 2 new timeout tests (connect_timeout + accept_timeout) exercise the fix. Concrete-XREF accepted: `ObpLookupHandle` race at `02-kernel-core/TODO-05 §2` item line 112 (retrofit list now names nt_alpc.c / alpc_port.c consumers). |
| 2026-04-15 | implement | §3 Connection state machine: swapped `ALPC_PORT.WaitQueue` + `PORT_MESSAGE_ENTRY.ReplySyncWait` from `condvar_t` to `event_t` (condvar needed mutex partner; port uses spinlock). Added `ALPC_PORT_CONNECT`/`ALL_ACCESS` + `STATUS_INVALID_PORT_HANDLE`. New kernel helpers `AlpcConnectPort`/`AcceptConnectPort`/`DisconnectPort` in alpc_port.c (~370 lines): file-local `ALPC_CONNECTION_REQUEST` with `event_t ReplyEvent`, cross-linked comm ports with proper ref accounting. Retrofitted 3 Nt handlers; dropped slots `0x0110`/`0x0112`/`0x0114` from the pending sweep. 6 new IPC tests (cross-thread handshake uses `kthread_create`/`thread_join`). Codex adversarial review found 4 Highs + 1 kernel-wide Medium: UAF after `event_set` (fixed: snapshot pid before signal), client_comm creation-ref leak on success (fixed), `on_delete` missing ConnectionPort deref (fixed: listen port no longer leaks), disconnect didn't drain pending connects (fixed: server disconnect now wakes everyone with STATUS_PORT_DISCONNECTED); the `ObpLookupHandle` close-race is kernel-wide and tracked at `02-kernel-core/TODO-05 §2` ObpReferenceObjectByHandle retrofit (added `nt_alpc.c` to its consumer list). |
| 2026-04-15 | review | §2 review-todo-section: Phase-1 evidence + scope-gap + build clean; Phase-2 adversarial already ran 2x during implement; Phase-3 kernel-code-quality Gates 1-11 walked clean + Codex quality dispatch (dead-code + consistency + perf) found 4 more: 1 HIGH rejected (probe+direct-deref is kernel-wide precedent, SMAP disabled per CLAUDE.md), 2 MEDIUM fixed (`alpc_validate_attrs` rejects unknown Flags + privileged SYSTEM_PROCESS + non-zero reserved pads; `AlpcCreatePort` now returns NTSTATUS+out-HANDLE so syscall propagates OBJECT_NAME_COLLISION/INVALID/NOT_FOUND accurately), 1 LOW fixed (removed over-promising sizeof-drift-check comment from alpc_port.h). 2 new tests (privileged flag + reserved pad rejection). TODO-12/TODO-02 §5 "Audit all syscall handlers" checklist item extended to explicitly name `nt_alpc.c`'s `alpc_probe_and_split` + `NtAlpcCreatePort_handler` as retrofit consumers (concrete XREF for the Accepted finding). |
