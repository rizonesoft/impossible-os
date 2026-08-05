---
schema_version: 1
id: kernel-debugger-kd-protocol
domain: 02-kernel-core
status: active
title: "TODO-29 -- Kernel Debugger (KD Protocol)"
---

# TODO-29 -- Kernel Debugger (KD Protocol)

> **Goal:** Implement a WinDbg-compatible kernel debug stub over the existing COM1 serial port. The KD protocol is the wire format that WinDbg uses to control a target kernel: it allows a connected debugger to set software and hardware breakpoints, read and write memory and registers, single-step, enumerate loaded modules, and analyze crashes on live hardware. Today the serial port outputs text at 38400 baud with no receive capability beyond a single `serial_trygetchar`; there is no breakpoint infrastructure, no `#DB`/`#BP` routing to a debugger, and no packet framing. Without KD, every crash on real hardware requires a reboot-and-guess cycle; with KD, a developer attaches WinDbg from a second machine and gets live `!analyze -v`, symbol-resolved stack traces, and memory inspection in real time.

> [!IMPORTANT]
> **Current state:** `src/kernel/drivers/serial.c` (~110 lines today) initialises COM1 at 38400 baud, 8N1, with `serial_putchar` / `serial_write` / `serial_trygetchar` (polling only). `idt.c` has `idt_register_handler(n, fn)` for custom IDT slot registration but no `#DB` (vector 1) or `#BP` (vector 3) handlers; both panic on the default path. No KD packet structures, no connection state, no breakpoint table, no debug register management exist anywhere.

> [!NOTE]
> The KD serial stub is independent of the text-based `printk` path.
> When KD is not connected (`kd_connected == false`), all serial output continues as plain text. KD and text coexist by multiplexing: KD packet bytes start with a 4-byte leader (0x30303030 or 0x62626262), which is impossible to appear in normal UTF-8 text.

---

## Inputs

- `src/kernel/drivers/serial.c` -- COM1 init + polling TX/RX; extend to 115200 baud + IRQ-driven RX
- `include/kernel/drivers/serial.h` -- `serial_putchar`, `serial_trygetchar`
- `include/kernel/mm/vmm.h` -- `vmm_map_page`, `vmm_get_physical` for §7 physical-memory KD paths (no `phys_to_virt()` in tree)
- `include/kernel/boot_info.h` -- early identity map extent for §7 `DbgKdReadPhysicalMemoryApi` bounds
- `include/kernel/kd/kd_protocol.h` -- `KD_PACKET`, leaders, packet types (created in §3; not in tree yet)
- `src/kernel/kd/kd.c` -- KD state machine, handshake, command loop (created in §4 onward; not in tree yet)
- `src/kernel/drivers/keyboard.c` -- F12 breakin path in §13
- `src/kernel/idt.c` -- `idt_register_handler(n, fn)` for `#DB` and `#BP`
- `include/kernel/idt.h` -- `struct interrupt_frame` (full x64 register save)
- `src/kernel/smp/smp.c` -- `wrmsr`/`rdmsr`; per-CPU data (AP freeze in §5)
- → XREF: `TODO-23-exception-dispatch-seh.md §1` -- `EXCEPTION_RECORD` and `CONTEXT` types defined there; §6 of this TODO serialises them into `KD_STATE_CHANGE64`
- → XREF: `TODO-23-exception-dispatch-seh.md §4` -- `ki_dispatch_exception()` calls `KiDebugRoutine` for first/second-chance debugger notification; §5 of this TODO provides the `KiDebugRoutine` implementation that enters the KD command loop
- → XREF: `TODO-27-crash-dump-generation.md §3` -- `g_module_list` / `g_module_count` from the module registry fed into `DbgKdGetVersionApi` response (§11)
- → XREF: `TODO-27-crash-dump-generation.md §2` -- `CONTEXT` record layout must match exactly what §6 of this TODO sends to WinDbg over the wire
- → XREF: `TODO-07-irql-model-dpcs.md §3` -- IRQL must be at `HIGH_LEVEL` while the kernel is frozen in the debugger; DPC timer must not fire during the debug loop
- → XREF: `TODO-12-native-api-ssdt.md §5` -- SSDT indices 0x0132-0x0137 reserved for debug syscalls; §14 of this TODO wires NtDebugActiveProcess, NtWaitForDebugEvent, etc. into the SSDT

> [!NOTE]
> **Planned doc (not in tree yet):** §13 creates `docs/guides/windbg-kd-setup.md` on first guide commit. Do not list it as a required Inputs anchor until the file exists.

---

## Outcome

- COM1/COM2 KD line operates at 115200 baud (§1) with IRQ-driven receive (§2); a 256-byte RX ring buffer feeds the KD packet reader without polling.
- `KD_PACKET` framing layer (§3) sends and receives packets with correct leaders, checksums, packet IDs, and ACK/RESEND handshaking.
- WinDbg connects via `windbg -k com:port=COM1,baud=115200` and receives a `PACKET_TYPE_KD_RESET` on the first breakin byte (§4).
- Any `INT3` / `#BP` in the kernel or any explicit `DbgBreakPoint()` call freezes all APs, sends `KD_STATE_CHANGE64` to WinDbg, and enters the KD command loop; WinDbg shows the source line and register state.
- `#DB` handles both single-step (TF flag) and hardware watchpoints (DR0-DR3).
- WinDbg can: `db / dd / dq` (read memory), `eb / ed / eq` (write memory), `r` (registers), `bp`/`bu` (software breakpoints), `ba` (hardware access breakpoints), `p`/`t` (step over/into), `lm` (module list), `g` (go).
- `kd_break()` callable from any kernel code to enter the debugger programmatically; `F12` on the target keyboard triggers a breakin too.
- Documented KDNET/USB3 and idle/BSOD coexistence policy (§15) so later milestones stay aligned with Win11 and Linux kgdb/kdb operator expectations.

---

## Implementation Order

| ⭐  | Order | Deliverable                              | Depends On                  | Status |
| --- | :---: | ---------------------------------------- | --------------------------- | :----: |
| 💎  |   1   | Serial line: 115200 baud, 8N1, FIFO      | --                          |  [ ]   |
| 💎  |   2   | IRQ RX ring + COM1/COM2 KD port selection | §1                          |  [ ]   |
| 💎  |   3   | KD packet framing: send/receive, checksum, ACK/RESEND | §1, §2                      |  [ ]   |
| 💎  |   4   | KD connection handshake & breakin detection | §3                          |  [ ]   |
| 💎  |   5   | `#DB` / `#BP` exception routing to KD, AP freeze/thaw | §4, T07 §3                  |  [ ]   |
| 💎  |   6   | Context get/set (DbgKdGetContextApi / SetContextApi) | §5, T27 §2                  |  [ ]   |
| 💎  |   7   | Memory read/write (DbgKdReadVirtualMemoryApi / Write) | §5                          |  [ ]   |
| 💎  |   8   | Software breakpoint management (Write / RestoreApi) | §5, §7                      |  [ ]   |
| 💎  |   9   | Hardware breakpoints (DR0--DR3, DR7) + `#DB` reporting | §5, §6                      |  [ ]   |
| 💎  |  10   | Single-step (RFLAGS.TF) + continue / continue-step | §5, §6                      |  [ ]   |
| 💎  |  11   | DbgKdGetVersionApi + module list         | §4, T27 §3                  |  [ ]   |
| 💎  |  12   | I/O port & MSR read/write (DbgKdReadIoSpace / MSR apis) | §5                          |  [ ]   |
| ⭐  |  13   | `kd_break()` + keyboard F12 breakin + QEMU pipe guide | §4                          |  [ ]   |
| 💎  |  14   | Debug syscalls wired to SSDT             | §1, §2, §5, T12 §4, T12 §21 |  [ ]   |
| 💎  |  15   | KD transport roadmap + idle/BSOD coexistence | §3, §5, T26 §2, T28 §1      |  [ ]   |

> 💎 = parity work: matches what Windows 11 and Linux already do.
> ⭐ = exclusive work: Impossible OS is superior or first.

---

## 1. Serial Line Setup: 115200 Baud, 8N1, FIFO

- [ ] Update `serial_init()` in `src/kernel/drivers/serial.c`:
  ```c
  /* Divisor for 115200 baud: 115200 = 1843200 / (16 * divisor) -> divisor=1 */
  outb(SERIAL_PORT + 3, 0x80);  /* enable DLAB */
  outb(SERIAL_PORT + 0, 0x01);  /* DLL=1 -> 115200 baud */
  outb(SERIAL_PORT + 1, 0x00);  /* DLH=0 */
  outb(SERIAL_PORT + 3, 0x03);  /* 8N1, DLAB off */
  outb(SERIAL_PORT + 2, 0xC7);  /* FIFO: enable, 14-byte trigger level */
  outb(SERIAL_PORT + 4, 0x0B);  /* RTS+DTR+OUT2 (OUT2 enables IRQ to PIC) */
  ```
- [ ] After init, enable RX interrupts: `outb(SERIAL_PORT + 1, 0x01)` (IER bit 0 = Received Data Available Interrupt; KD uses same line when IRQ path is primary)
- [ ] Commit: `"kernel/serial: 115200 baud 8N1 FIFO COM1 baseline"`

**Test checkpoint:** Serial log readable at 115200; line control matches divisor 1 for 115200 with 8N1. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 2. IRQ-Driven RX Ring Buffer and KD COM Port Selection

- [ ] `#define SERIAL_RX_BUF_SIZE 256` -- power-of-two, static allocation
- [ ] `static uint8_t serial_rx_buf[SERIAL_RX_BUF_SIZE]`
- [ ] `static volatile uint32_t rx_head, rx_tail` -- producer (ISR) and consumer (KD reader); no lock needed because ISR is single-producer
- [ ] Register COM1 interrupt handler (IRQ 4, vector `IRQ_BASE + 4 = 0x24`):
  - Read all available bytes from DATA port while `LSR & 0x01` is set; each byte: if `(rx_head - rx_tail) < SERIAL_RX_BUF_SIZE` -> enqueue into `serial_rx_buf[rx_head++ & (SIZE-1)]`; else drop (overflow)
- [ ] `serial_getchar()` -- blocking read: spin until `rx_head != rx_tail`; return `serial_rx_buf[rx_tail++ & (SIZE-1)]`
- [ ] `serial_trygetchar()` -- updated to use the ring buffer (non-blocking; return -1 if empty); retains backward compatibility

- [ ] `SERIAL_PORT_KD` -- compile-time or Registry-selectable: `0x3F8` (COM1, default) or `0x2F8` (COM2); store in `g_kd_com_port`
- [ ] `kd_serial_init()` calls `serial_init_port(g_kd_com_port)` which is a generalisation of the above; the existing `printk` path stays on COM1
- [ ] Commit: `"kernel/serial: IRQ RX ring, COM1/COM2 KD port selection, kd_serial_init"`

**Test checkpoint:** IRQ RX ring `rx_head != rx_tail` advances under byte flood from host; no boot hang when KD disabled. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 3. KD Packet Framing: Send / Receive, Checksum, ACK/RESEND

- [ ] Define in `include/kernel/kd/kd_protocol.h`:
  ```c
  #define PACKET_LEADER           0x30303030UL
  #define BREAKIN_PACKET_LEADER   0x62626262UL  /* 'bbbb' breakin leader */
  #define CONTROL_PACKET_LEADER   0x69696969UL  /* 'iiii' */

  #define PACKET_TYPE_KD_STATE_CHANGE64    2
  #define PACKET_TYPE_KD_MANIPULATE_STATE64 8
  #define PACKET_TYPE_KD_DEBUG_IO          3
  #define PACKET_TYPE_KD_ACKNOWLEDGE       4
  #define PACKET_TYPE_KD_RESEND            5
  #define PACKET_TYPE_KD_RESET             6
  #define PACKET_TYPE_KD_BREAKIN           7

  typedef struct {
      uint32_t PacketLeader;  /* PACKET_LEADER or CONTROL_PACKET_LEADER */
      uint16_t PacketType;
      uint16_t ByteCount;     /* payload bytes following this header */
      uint32_t PacketId;      /* monotonically increasing, alternates 0/1 for retries */
      uint32_t Checksum;      /* sum of all payload bytes (uint32_t wrapping) */
  } KD_PACKET;  /* 16 bytes */
  ```

- [ ] `kd_checksum(buf, len)` -- simple 32-bit wrapping sum of all bytes:
  ```c
  uint32_t kd_checksum(const uint8_t *buf, uint32_t len) {
      uint32_t sum = 0;
      while (len--) sum += *buf++;
      return sum;
  }
  ```

- [ ] `kd_send_packet(type, payload, len)`:
  1. Build `KD_PACKET` header: `PacketLeader = PACKET_LEADER`, `PacketType = type`, `ByteCount = len`, `PacketId = g_kd_packet_id`, `Checksum = kd_checksum(payload, len)`
  2. `serial_putchar` each byte of header then each byte of payload
  3. Append one `0xAA` trailing byte (packet separator)
  4. Increment `g_kd_packet_id`; if the packet requires an ACK, wait for `PACKET_TYPE_KD_ACKNOWLEDGE` (retry up to 8 times with RESEND)
- [ ] `kd_send_control(type)` -- send a control packet (0-byte payload, `PacketLeader = CONTROL_PACKET_LEADER`); used for ACK, RESEND, RESET

- [ ] `kd_receive_packet(out_header, out_payload, max_payload_len)`:
  1. Read bytes until a 4-byte leader matches `PACKET_LEADER`, `BREAKIN_PACKET_LEADER`, or `CONTROL_PACKET_LEADER`
  2. Read 12 remaining header bytes into `KD_PACKET`
  3. Read `header.ByteCount` bytes into `out_payload`; refuse if `ByteCount > max_payload_len` (send RESEND, try again)
  4. Verify checksum; if mismatch: `kd_send_control(PACKET_TYPE_KD_RESEND)` and retry
  5. Send `kd_send_control(PACKET_TYPE_KD_ACKNOWLEDGE)` on success
  6. Return `KD_STATUS_SUCCESS`
- [ ] Return immediately with `KD_STATUS_BREAKIN` if `BREAKIN_PACKET_LEADER` received (start of breakin sequence)

- [ ] Commit: `"kernel/kd: KD packet framing, checksum, kd_send_packet, kd_receive_packet"`

**Test checkpoint:** Unit test `kd_checksum()` matches hand-computed sum on fixed buffer; `kd_send_packet` mock emits leader `0x30303030` then 16-byte header on wire buffer. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 4. KD Connection Handshake & Breakin Detection

- [ ] In `src/kernel/kd/kd.c`:
  ```c
  static bool kd_present    = false; /* connection attempted (boot flag) */
  static bool kd_connected  = false; /* WinDbg acknowledged */
  static bool kd_active     = false; /* currently in KD command loop */
  static uint32_t g_kd_packet_id = 0;
  ```
- [ ] `kd_init()` -- called from Phase 1 kernel init (-> XREF `T01 §3`); sets `kd_present = true` if the boot command line contains `kddebug=serial` or `HKLM\SYSTEM\KernelDebugger\Enabled = 1`; calls `kd_serial_init()` (§1, §2)

- [ ] The serial RX ISR checks: if the received byte is `0x62` ('b') and the 4-byte accumulator becomes `0x62626262` (BREAKIN_PACKET_LEADER): call `kd_handle_breakin()` -- sets a flag `kd_breakin_requested`; the next call to `kd_poll()` or any `#DB`/`#BP` handler picks it up
- [ ] `kd_poll()` -- called from the idle thread (`T26 §2`) and from the timer ISR every 100 ms when `kd_present`; if `kd_breakin_requested`: clear flag, invoke `kd_enter_command_loop(NULL)` (§5)

- [ ] When the first BREAKIN bytes arrive:
  1. `kd_send_control(PACKET_TYPE_KD_RESET)` -- tells WinDbg "I am here"
  2. WinDbg responds with its own `PACKET_TYPE_KD_RESET`
  3. Send `PACKET_TYPE_KD_ACKNOWLEDGE` to confirm
  4. Set `kd_connected = true`; log `[KD] WinDbg connected`
  5. Enter `kd_enter_command_loop(NULL)` so WinDbg can inspect state before continuing
- [ ] If `kd_present` but WinDbg never connects: the kernel boots normally; no performance penalty (the serial RX ISR costs ~100 ns per character)

- [ ] Commit: `"kernel/kd: KD connection state, breakin detection, WinDbg handshake"`

**Test checkpoint:** With `-serial pipe:kd`, serial shows `[KD] WinDbg connected` within 5 s of host attach; `kd_connected` true only after RESET exchange. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 5. `#DB` / `#BP` Exception Routing to KD, AP Freeze/Thaw

- [ ] In `kd_init()`, register:
  ```c
  idt_register_handler(1, kd_debug_exception_handler);  /* #DB */
  idt_register_handler(3, kd_breakpoint_exception_handler); /* #BP */
  ```
- [ ] `kd_debug_exception_handler(frame)` -- handles `#DB` (single-step, hardware watchpoint); calls `kd_enter_command_loop(frame)` if KD is present; returns `panic_screen` otherwise
- [ ] `kd_breakpoint_exception_handler(frame)` -- handles `INT3`; adjusts `frame->rip -= 1` to rewind past the `0xCC` byte before entering the command loop (so `g` resumes from the original instruction)
- [ ] KiDebugRoutine lifetime: `ki_dispatch_exception()` (TODO-23 §4) snapshots the routine with one atomic load and may call it after a detach -- keep it resident (permanent kernel text or quiescence before unmap); freeing on detach is UAF.

- [ ] `kd_freeze_aps()` -- send `KD_IPI_FREEZE` IPI to all APs (use LAPIC broadcast IPI `NMI` or a dedicated synthetic vector); each AP's IPI handler sets `cpu_data[this_cpu].frozen = true` and busy-waits
- [ ] `kd_thaw_aps()` -- clear `frozen` flag on all CPUs; they resume executing their previous tasks
- [ ] The BSP holds `kd_active = true` and runs `kd_enter_command_loop` (see checklist below) while APs are frozen; at no point can a frozen AP deliver a DPC or preempt the debug session (-> XREF `TODO-07-irql-model-dpcs.md §3`)

- [ ] `kd_enter_command_loop(frame)`:
  1. `kd_freeze_aps()` if SMP enabled
  2. Send `PACKET_TYPE_KD_STATE_CHANGE64` (§6) to WinDbg
  3. Loop: `kd_receive_packet(...)` -> dispatch to handler by `ApiNumber`:
     - `0x3131` -> `kd_handle_read_virt_mem()`   (§7)
     - `0x3132` -> `kd_handle_write_virt_mem()`  (§7)
     - `0x3133` -> `kd_handle_get_context()`     (§6)
     - `0x3134` -> `kd_handle_set_context()`     (§6)
     - `0x3135` -> `kd_handle_write_breakpoint()` (§8)
     - `0x3136` -> `kd_handle_restore_breakpoint()` (§8)
     - `0x3137` -> `kd_handle_continue()` -> break loop
     - `0x313D` -> `kd_handle_continue2()` -> break loop (with step flag)
     - `0x314A` -> `kd_handle_get_version()` (§11)
     - `0x3152` -> `kd_handle_read_msr()` (§12)
     - `0x3153` -> `kd_handle_write_msr()` (§12)
     - `0x313A`/`0x313B` -> `kd_handle_io_space_rw()` (§12)
     - Unknown -> send error response
  4. On continue: `kd_thaw_aps()`; set `kd_active = false`; return frame

- [ ] Commit: `"kernel/kd: #DB/#BP IDT routing, AP freeze/thaw IPI, KD command dispatch loop"`

**Test checkpoint:** `int3` in kernel text breaks to KD loop (not panic); APs idle in freeze loop while BSP in KD; `g` returns to running threads. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 6. Context Get / Set

- [ ] `DBGKD_WAIT_STATE_CHANGE64` payload for `PACKET_TYPE_KD_STATE_CHANGE64`:
  ```c
  typedef struct {
      uint32_t NewState;          /* DbgKdExceptionStateChange=0x30020003 */
      uint16_t ProcessorLevel;    /* 0x0006 (AMD64) */
      uint16_t Processor;         /* CPU number */
      uint32_t NumberProcessors;
      uint64_t Thread;            /* current task pointer */
      uint64_t ProgramCounter;    /* RIP */
      struct {
          EXCEPTION_RECORD64 ExceptionRecord; /* (-> XREF TODO-23-exception-dispatch-seh.md §1) */
          uint64_t FirstChance;
      } Exception;
      uint8_t  ControlReport[56]; /* processor-specific control report */
  } DBGKD_WAIT_STATE_CHANGE64;
  ```
- [ ] Populate from `interrupt_frame`: `ProgramCounter = frame->rip`; `ExceptionRecord.ExceptionCode = kd_exception_code(frame->int_no)`; `ExceptionRecord.ExceptionAddress = frame->rip`
- [ ] `ControlReport` (AMD64 format): contains CS, DS, ES, FS, DR6, DR7, `EFlags`, 4 bytes at RIP (instruction preview), and `SegSs`

- [ ] Build a full `CONTEXT` record (-> XREF `TODO-27-crash-dump-generation.md §2`) from the saved `interrupt_frame`:
  - Map `frame->rip` -> `ctx->Rip`, `frame->rsp` -> `ctx->Rsp`, etc.
  - DR0-DR7 values read from hardware (`mov rax, dr0`, etc.)
  - `FltSave`: copy `g_panic_xsave_buf` (or live `FXSAVE` if available)
  - Send as payload of `PACKET_TYPE_KD_MANIPULATE_STATE64` response

- [ ] Receive `CONTEXT` record from WinDbg; apply to the saved `frame`:
  - `frame->rip = ctx->Rip`, `frame->rsp = ctx->Rsp`, etc.
  - Restore DR0-DR3, DR7 via `mov dr0, rax` etc. (§9)
  - Restore `frame->rflags = ctx->EFlags` (preserves single-step TF if WinDbg set it)
  - Send success response

- [ ] Commit: `"kernel/kd: KD_STATE_CHANGE64, DbgKdGetContextApi/SetContextApi"`

**Test checkpoint:** WinDbg `r` shows RIP matching faulting `interrupt_frame`; `r rip=<va>` changes next continue. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 7. Memory Read / Write

- [ ] **Virtual read (`DbgKdReadVirtualMemoryApi`):** request `{BaseAddress (uint64_t), TransferCount (uint32_t)}`; response `{ActualBytesRead (uint32_t)}` plus raw bytes; walk page tables to verify each page is mapped and present before `memcpy`; on absent page return partial read; cap `TransferCount` at 4096 bytes per KD request
- [ ] **Virtual write (`DbgKdWriteVirtualMemoryApi`):** request `{BaseAddress, TransferCount}` plus raw payload; same page-walk validation as read; if PTE is read-only (NX or RO), temporarily set `PTE_WRITE`, perform write, restore original PTE (supports kernel `INT3` patches under `TODO-10-kernel-security-hardening.md §1`); return `ActualBytesWritten`

- [ ] Implement `kd_phys_to_kva(uintptr_t pa, size_t *max_contiguous)` (or reuse an existing MM helper) that maps `pa` only inside the boot-time identity window described in `include/kernel/boot_info.h` (and/or a small fixed low-memory policy); reject `pa` outside allowed ranges; do not add a Linux-style `phys_to_virt()` name (tree has none: `rg phys_to_virt` only hits this TODO)
- [ ] `DbgKdReadPhysicalMemoryApi (0x313F)` -- read using `kd_phys_to_kva()` + bounds; verify each mapped host VA with `vmm_get_physical()` round-trip where applicable
- [ ] `DbgKdWritePhysicalMemoryApi (0x3140)` -- same for write with identical bounds rules

- [ ] **Search memory:** request `{SearchAddress, SearchLength, PatternLength}` plus pattern bytes; scan `[SearchAddress, SearchAddress + SearchLength)`; return first matching VA or `0xFFFFFFFFFFFFFFFF`; used by WinDbg `s` for pool tags and magic values

- [ ] Commit: `"kernel/kd: DbgKdReadVirtualMemory, WriteVirtualMemory, Physical variants, SearchMemory"`

**Test checkpoint:** WinDbg `db` on kernel `.text` returns non-zero bytes; unmapped VA returns partial read without crash. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 8. Software Breakpoint Management

- [ ] Static table of 64 slots:
  ```c
  typedef struct {
      uint64_t address;    /* VA of patched byte */
      uint8_t  orig_byte;  /* saved byte (before 0xCC patch) */
      bool     active;
  } kd_sw_bp_t;

  static kd_sw_bp_t kd_sw_bp_table[64];
  ```

- [ ] Request: `{BreakPointAddress (uint64_t), BreakPointHandle (uint32_t)}`
- [ ] Find or allocate a slot; save `orig_byte = *(uint8_t*)address`; use `kd_handle_write_virt_mem()` (§7) to patch the byte to `0xCC` (must use the write-protected-bypass path)
- [ ] Response: `{BreakPointHandle}` (index + 1)

- [ ] Request: `{BreakPointHandle}`
- [ ] Restore `*(uint8_t*)address = orig_byte` (using the same write bypass); mark slot `active = false`

- [ ] In `kd_breakpoint_exception_handler` (§5): scan the breakpoint table for an entry matching `frame->rip - 1` (the `INT3` is one byte); if found: set `ExceptionRecord.ExceptionInformation[0] = handle` in the `KD_STATE_CHANGE64` so WinDbg can match the breakpoint to its `bp` command

- [ ] Commit: `"kernel/kd: software breakpoint table, WriteBP/RestoreBP, INT3 hit identification"`

**Test checkpoint:** After `bp` on symbol, hit shows INT3 at patched VA; `bc` restores original opcode byte. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 9. Hardware Breakpoints: DR0--DR3, DR7, `#DB` Reporting

- [ ] `kd_dr_read(n)` / `kd_dr_write(n, val)` for n in {0,1,2,3,6,7}:
  ```asm
  mov rax, dr0   ; kd_dr_read(0)
  mov dr0, rax   ; kd_dr_write(0, val)
  ```
- [ ] `kd_hw_bp_set(slot, addr, type, len)`:
  - Write `addr` to DR[slot]
  - DR7 encoding: `LE = 1` (local enable for slot), `G0/G1 = 0` (global disabled), condition bits (type: `00`=exec, `01`=write, `11`=rw) and length (`00`=1, `01`=2, `11`=4, `10`=8 bytes) in DR7 bits [17:16+slot*4]
  - Set `DR7.LE[slot]` bit (bits 0,2,4,6 for slots 0--3)
- [ ] `kd_hw_bp_clear(slot)` -- clear DR7 enable bits for slot

- [ ] `kd_debug_exception_handler(frame)`:
  1. Read DR6 status: `__asm__ volatile("mov %%dr6, %0" : "=r"(dr6))`
  2. DR6 bits [3:0]: which DR triggered; DR6 bit 14: single-step (BS flag)
  3. Distinguish:
     - `dr6 & 0x0F` non-zero -> hardware breakpoint hit; report to WinDbg with `ExceptionCode = STATUS_SINGLE_STEP (0x80000004)` and indicate the DR that fired in `ExceptionInformation[0]`
     - `dr6 & (1 << 14)` -> single-step: clear `RFLAGS.TF` in frame, set `ExceptionCode = STATUS_SINGLE_STEP`
  4. Clear DR6: `__asm__ volatile("mov %0, %%dr6" : : "r"(0ULL))`
  5. Call `kd_enter_command_loop(frame)`

- [ ] Commit: `"kernel/kd: hardware breakpoints DR0-DR3/DR7, #DB handler, DR6 status decode"`

**Test checkpoint:** `ba e1 <kernel_va>` breaks on execute; DR6 shows correct Bn bit; `bc` clears DR7 enable. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 10. Single-Step (RFLAGS.TF) + Continue / Step

- [ ] Request: `{ContinueStatus (uint32_t)}` -- `DBG_CONTINUE (0x10002)` or `DBG_EXCEPTION_NOT_HANDLED (0x80010001)`
- [ ] Set `kd_continue = true`; exit the command loop; do not set `RFLAGS.TF` -- execution resumes normally

- [ ] Request: `{ContinueStatus, AnyActiveHwBps, TraceFlag}`:
  - `TraceFlag = 1` -> set `frame->rflags |= 0x100` (TF bit) -- single step mode; the CPU fires `#DB` after the next instruction
  - `TraceFlag = 0` -> clear `frame->rflags &= ~0x100`
- [ ] Used by WinDbg's `p` (step over) and `t` (step into) commands; for `p` (step over CALL), WinDbg installs a temporary software breakpoint at the instruction following the CALL -- no kernel-side step-over logic needed

- [ ] Commit: `"kernel/kd: single-step via RFLAGS.TF, ContinueApi/ContinueApi2"`

**Test checkpoint:** WinDbg `t` advances RIP by one instruction; TF cleared after #DB report unless stepping again. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 11. DbgKdGetVersionApi + Module List

- [ ] `DbgKdGetVersionApi (0x314A)` response payload:
  ```c
  typedef struct {
      uint16_t MajorVersion;    /* 0x0F = free build, 0x0C = checked */
      uint16_t MinorVersion;    /* OS build number */
      uint8_t  ProtocolVersion; /* 6 = KD protocol version 6 */
      uint8_t  KdSecondaryVersion; /* 0 */
      uint16_t Flags;           /* DBGKD_VERS_FLAG_MP (0x0004) if SMP */
      uint16_t MachineType;     /* IMAGE_FILE_MACHINE_AMD64 (0x8664) */
      uint8_t  MaxPacketType;
      uint8_t  MaxStateChange;
      uint8_t  MaxManipulate;
      uint8_t  Simulation;      /* 0 = real hardware */
      uint16_t __Unused;
      uint64_t KernBase;        /* kernel load base VA */
      uint64_t PsLoadedModuleList; /* VA of g_module_list (-> XREF TODO-27-crash-dump-generation.md §3) */
      uint64_t DebuggerDataList;   /* VA of KdDebuggerDataBlock */
  } DBGKD_GET_VERSION64;
  ```
- [ ] `KernBase` = `KERNEL_IMAGE_BASE + g_kaslr_slide` (-> XREF `TODO-10-kernel-security-hardening.md §13`)
- [ ] `PsLoadedModuleList` = `(uint64_t)(uintptr_t)&g_module_list[0]`
- [ ] Unloaded-module (`lm`-style) query: expose the recently-unloaded tombstone ring so WinDbg can resolve a PC into a recently-unloaded image. -> XREF: TODO-18 §8 (tombstone ring owner).

- [ ] `KDDEBUGGER_DATA64` structure at a known VA; WinDbg uses offsets from this to locate internal kernel data:
  - `KernBase`, `BreakpointWithStatus`, `SavedContext`
  - `KiCallUserMode` (for user-mode breakin)
  - `PsActiveProcessHead` (kernel process list)
  - Offsets that are zero-filled may cause WinDbg to skip features gracefully -- fill in what is available, leave the rest zero

- [ ] Commit: `"kernel/kd: DbgKdGetVersionApi, DBGKD_GET_VERSION64, KdDebuggerDataBlock"`

**Test checkpoint:** `lm` lists `kernel.exe` load address equal to `KernBase` from version packet; `!lmi kernel` succeeds. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 12. I/O Port & MSR Read / Write

- [ ] Request: `{IoAddress (uint64_t), DataSize (uint32_t)}`:
  - `DataSize = 1` -> `inb(IoAddress)` / `outb(IoAddress, data)`
  - `DataSize = 2` -> `inw` / `outw`
  - `DataSize = 4` -> `inl` / `outl`
- [ ] Used by WinDbg for low-level port I/O diagnostics and ACPI PM register inspection
- [ ] Bounds check: refuse I/O to KD serial port (0x3F8--0x3FF) to prevent self-disruption

- [ ] Request: `{MsrAddress (uint32_t)}`
- [ ] Response: `{DataValueLow, DataValueHigh}` -- result of `rdmsr(MsrAddress)`
- [ ] Whitelist for safety: allow only well-known debug-relevant MSRs (IA32_EFER, IA32_SPEC_CTRL, IA32_LSTAR, IA32_GS_BASE, IA32_KERNEL_GS_BASE, IA32_S_CET, IA32_PL0_SSP); return error for others in non-debug builds

- [ ] Same as read but `wrmsr(MsrAddress, value)`; same whitelist enforced
- [ ] Useful for WinDbg's `!sysinfo` and experimental testing of IBRS/CET toggles from the debugger

- [ ] Commit: `"kernel/kd: DbgKdReadIoSpaceApi, WriteIoSpaceApi, ReadMSR, WriteMSR"`

**Test checkpoint:** `!msr 0xC0000082` (LSTAR) returns expected 64-bit value; write to COM1 I/O range rejected with error. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 13. `kd_break()`, Keyboard F12 Breakin & QEMU Guide

- [ ] `void kd_break(void)` -- callable from any kernel code to programmatically enter the debugger:
  ```c
  void kd_break(void) {
      if (kd_present)
          __asm__ volatile ("int $3"); /* triggers §5 */
      /* else: no-op if KD not configured */
  }
  ```
- [ ] `void DbgBreakPoint(void)` -- Win32-compatible alias; allows porting existing Windows driver code that calls `DbgBreakPoint()` to break in

- [ ] In `keyboard.c` ISR: if KD is present and `kd_connected` and the scancode for F12 is received: set `kd_breakin_requested = true`; the KD poll path picks it up (§4)
- [ ] Allows a developer sitting at the target machine to break in without needing the debugger host; mirrors the `SysRq+g` mechanism on Linux

- [ ] Document in `docs/guides/windbg-kd-setup.md`:
  ```
  QEMU flags:
    -serial pipe:kd              (Windows host -- named pipe \\.\pipe\kd)
    -serial tcp::1234,server,nowait  (cross-platform -- KD over TCP)

  WinDbg command:
    windbg -k com:pipe,resets=0,reconnect,port=\\.\pipe\kd
    windbg -k com:port=com1,baud=115200   (real hardware via USB-to-serial)

  KD boot flag for Impossible OS:
    Add 'kddebug=serial' to boot parameters in UEFI boot entry, or:
    Registry: HKLM\SYSTEM\KernelDebugger\Enabled = 1
              HKLM\SYSTEM\KernelDebugger\Port    = "COM1"
              HKLM\SYSTEM\KernelDebugger\BaudRate = 115200
  ```
- [ ] `src/kernel/kd/kd.c` reads the Registry key at Phase 2 init to allow disabling KD for production/release builds (-> XREF `TODO-14-registry-completion.md §5`)

- [ ] Commit: `"kernel/kd: kd_break, DbgBreakPoint, F12 keyboard breakin, QEMU KD setup guide"`

**Test checkpoint:** `kd_break()` from code path hits KD when enabled; F12 sets breakin flag when `kd_connected`; `docs/guides/windbg-kd-setup.md` committed. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 14. Debug Syscalls Wired to SSDT

Register user-mode debug API entry points in the SSDT so debuggers can attach/detach/wait via `syscall`. (-> XREF: `TODO-12-native-api-ssdt.md` §5 SSDT indices, §21 Exception/Debug syscalls)

- [ ] `NtCreateDebugObject(DebugObjectHandle, DesiredAccess, ObjectAttributes, Flags)` -> SSDT 0x0135: allocate debug port object; register as ObpDebugType
- [ ] `NtDebugActiveProcess(ProcessHandle, DebugObjectHandle)` -> SSDT 0x0132: attach debug port to target process; all exceptions route to debugger first
- [ ] `NtRemoveProcessDebug(ProcessHandle, DebugObjectHandle)` -> SSDT 0x0134: detach debugger from process
- [ ] `NtWaitForDebugEvent(DebugObjectHandle, Alertable, Timeout, WaitStateChange)` -> SSDT 0x0136: wait for breakpoint, exception, thread create/exit, process exit, module load events
- [ ] `NtDebugContinue(DebugObjectHandle, ClientId, ContinueStatus)` -> SSDT 0x0133: continue after debug event with `DBG_CONTINUE` or `DBG_EXCEPTION_NOT_HANDLED`
- [ ] `NtSetInformationDebugObject(DebugObjectHandle, DebugObjectInformationClass, Buffer, Length, ReturnLength)` -> SSDT 0x0137: control debug object behavior
- [ ] All functions return `NTSTATUS`; use codes from `include/kernel/nt/ntstatus.h` (TODO-12 §1)
- [ ] Commit: `"kernel/kd: wire debug syscalls to SSDT (0x0132-0x0137)"`

**Test checkpoint:** `NtCreateDebugObject` returns valid handle. `NtDebugActiveProcess` on child process captures INT3 breakpoint via `NtWaitForDebugEvent`. `NtDebugContinue(DBG_CONTINUE)` resumes debuggee. `NtRemoveProcessDebug` detaches cleanly. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 15. KD Transport Roadmap, SMP Policy, and Coexistence

> [!WARNING]
> `kd_freeze_aps()` must not resurrect deprecated broadcast Init Level De-Assert IPI sequences; follow `CLAUDE.md` AP bring-up guidance and use only supported per-CPU freeze vectors plus LAPIC rules already used for SMP stop-machine class work.

- [ ] In `docs/guides/windbg-kd-setup.md` (first commit of that file), document milestone order: (1) COM1/COM2 serial + IRQ path per §1-§2, (2) QEMU `-serial pipe` / TCP-pty bridge examples from §13, (3) defer Win11-style KDNET (`windbg -k net:port=...,key=...`, host `kdnet.exe` flow) until a verified-NIC allowlist story exists for Impossible OS (-> XREF Microsoft Learn "Set up KDNET network kernel debugging automatically")
- [ ] Extend the same guide with a short "Linux operator cross-walk" covering `kgdboc`, `kgdbwait`, SysRq-G, and kdb versus kgdb so developers know Impossible OS matches WinDbg wire format, not GDB remote (-> XREF Linux kernel.org kgdb documentation v6.12)
- [ ] Mirror `TODO-26-power-management.md §2`: when `kd_present` is true, `pm_deep_idle_allowed()` (or `sched_idle_cpu()` prolog) must not enter S1/deep idle while `kd_breakin_requested` is set; optionally invoke `kd_poll()` before halting (-> XREF `TODO-26-power-management.md §2`)
- [ ] Mirror `T28 §1`: when `kd_active`, defer `panic_screen()` until the debugger continues or a documented timeout elapses; keep first-chance path in `ki_dispatch_exception` ahead of BSOD (-> XREF `TODO-28-bsod-ux-enhancements.md` §1)
- [ ] Mirror `TODO-01-kernel-init-sequencing.md §3`: call `kd_init()` only after COM IRQ registration and IDT slots for vectors used by KD are live; document ordering relative to `sti` (-> XREF `TODO-01-kernel-init-sequencing.md §3`)
- [ ] Add release-build policy notes: compile-time `KERNEL_KD` (name TBD) plus registry must both allow KD in production-equivalent images, analogous to disabling `bcdedit /debug` on shipping Windows SKUs (-> XREF `TODO-10-kernel-security-hardening.md` hardening themes)
- [ ] Commit: `"docs/kernel: KD transport roadmap + idle/BSOD coexistence"`

**Test checkpoint:** No kernel binary requirement yet; verify markdown links and mirrored TODO bullets exist. After `kd_init` lands, expect `klog(LOG_INFO, "[KD] transport policy ...")` once at boot when `kd_present` is true. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## OS Comparison

| ⭐  | Feature                  | 🪟 Win11  | 🐧 Linux        | 🚀 Impossible OS  |
| --- | ------------------------ | --------- | --------------- | ----------------- |
| 💎  | KD serial stub           | ✅ kdcom  | ✅ KGDB serial  | ⬜ §1-§5          |
| 💎  | WinDbg wire format       | ✅ Native | ❌ GDB only     | ⬜ §3-§4          |
| 💎  | Live SW breakpoints      | ✅ Full   | ✅ GDB break    | ⬜ §8             |
| 💎  | HW DR breakpoints        | ✅ ba     | ✅ watch        | ⬜ §9             |
| 💎  | Single step t p          | ✅ Full   | ✅ stepi        | ⬜ §10            |
| 💎  | Live mem r w             | ✅ db eb  | ✅ x set        | ⬜ §7             |
| 💎  | Register r w             | ✅ r      | ✅ info reg     | ⬜ §6             |
| 💎  | Module list lm           | ✅ lm     | ✅ shared       | ⬜ §11            |
| 💎  | MSR from debugger        | ✅ !msr   | ✅ msr sysfs    | ⬜ §12            |
| 💎  | DbgBreak kd_break        | ✅ Yes    | ✅ KGDB BP      | ⬜ §13            |
| 💎  | I/O from debugger        | ✅ ioctl  | ⚠️ driver        | ⬜ §12            |
| ⭐  | F12 target breakin       | ❌ SysRq  | ⚠️ SysRq+g       | ⬜ §13            |
| ⭐  | KD via Registry          | ⚠️ bcdedit | ⚠️ cmdline       | ⬜ §13            |
| ⭐  | QEMU TCP pipe KD         | ⚠️ pipe    | ⚠️ gdb remote    | ⬜ §13            |
| 💎  | KDNET Ethernet debug     | ✅ kdnet  | ❌ N/A          | ⬜ §15 defer      |
| 💎  | USB3 kernel debug        | ✅ Yes    | ⚠️ platform      | ⬜ §15 defer      |
| 💎  | kdb shell + kgdb gdbstub | ❌ WinDbg | ✅ kdb + kgdb   | ⬜ WinDbg KD only |
| 💎  | SMP freeze other CPUs    | ✅ Yes    | ✅ kgdb roundup | ⬜ §5 §15         |

After §1-§14, WinDbg-compatible KD on COM1 plus user-mode debug SSDT surface. KGDB stays GDB-only. §15 tracks KDNET/USB deferrals, idle/BSOD coexistence, and Linux kgdb operator cross-walk. Extras: Registry KD toggle, F12 breakin, QEMU pipe doc.

---

## Unit Tests

> Wire into `test_runner_init()` via `test_register_kd()` (see `src/kernel/test/test_runner.c` and `include/kernel/test/test.h`). Use `test_suite_register_cat(..., TEST_CAT_BOOT)` for each case (KD ships with boot-time init).
> Boot tests run with `debug=1` or `test=1` in `boot.conf`.

- [ ] Create `src/kernel/test/test_kd.c` with:
  - KD packet framing: `kd_send_packet` (or pure header builder under test) emits `PACKET_LEADER`, correct `ByteCount`, and checksum matching `kd_checksum(payload, len)`
  - KD checksum: `kd_checksum(data, len)` matches hand-calculated 32-bit wrapping sum on fixed buffer
  - Memory read: `kd_read_memory(kernel_main_addr, 4)` returns first 4 bytes of kernel text (non-zero)
  - Register read: `kd_get_context()` fills `CONTEXT.Rip` in kernel text range for synthetic `interrupt_frame`
  - Breakpoint: `kd_set_breakpoint(addr)` writes `0xCC`; `kd_clear_breakpoint(addr)` restores saved opcode
  - Symbol oracle: `symtab_resolve(kernel_main_va, &off)` returns non-NULL name (no separate `kd_lookup_symbol` until KD exports one)
  - Serial transport: `kd_serial_send(buf, len)` (or mock backend) records exact byte sequence including `0xAA` trailer
  - Physical read helper: `kd_phys_to_kva()` rejects addresses above the documented identity window and accepts a page inside low RAM per `boot_info` constants
  - Policy strings: build-time test or static assert that `docs/guides/windbg-kd-setup.md` (once added) contains required section headings listed in §15 (optional doc lint hook)
- [ ] Add `extern void test_register_kd(void);` in `test_runner.c`, call `test_register_kd()` from `test_runner_init()`
- [ ] Commit: `"test: add kernel debugger (KD protocol) test suite"`

---

## Verification

- [ ] **115200 baud**: connect a terminal emulator (PuTTY / minicom) at 115200 baud; kernel boot log must appear correctly (no garbled output); verify `printk` still works as before.
- [ ] **WinDbg connection**: launch QEMU with `-serial pipe:kd`; run `windbg -k com:pipe,resets=0,reconnect,port=\\.\pipe\kd`; within 5 s WinDbg must show `[KD] WinDbg connected` in the kernel serial log.
- [ ] **`g` continue**: after connection, type `g` in WinDbg; kernel must resume executing (boot continues); WinDbg shows `Broken into`.
- [ ] **Software breakpoint**: `bp kernel!pmm_alloc_frame`; trigger a kernel malloc; WinDbg must halt with source line and `r` must show correct RIP pointing to `pmm_alloc_frame`.
- [ ] **Memory read**: `db fffff80000000000 L 10` in WinDbg -- must return the first 16 bytes of kernel text without error.
- [ ] **Hardware watchpoint**: `ba w4 fffff80000100000`; write to that VA from kernel code; WinDbg must break at the store instruction.
- [ ] **Single-step**: `t` command -- must execute exactly one instruction and break again; `r rip` increments by the instruction length.
- [ ] **Module list**: `lm` in WinDbg -- must show at least `kernel.exe` with its load address matching `KernBase` in `DbgKdGetVersionApi`.
- [ ] **Remaining limits**: KGDB-style reverse debugging (record & replay) is out of scope; the `KdDebuggerDataBlock` offsets enable WinDbg commands like `!process` only after the process list (-> `TODO-21-process-model-extensions.md`) is wired in; the `!analyze -v` for live crashes requires the crash dump path (-> `TODO-27-crash-dump-generation.md §5`) to have run first.
- [ ] **Platforms:** repeat WinDbg serial checks on QEMU WHPX, QEMU TCG, VirtualBox, and bare metal (VM serial timing can hide IRQ races).
- [ ] Commit: `"kernel/kd: WinDbg-compatible KD stub -- serial, packet framing, breakpoints, context, module list"`

**Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot)

---

## History

| Date       | Action   | Summary |
| ---------- | -------- | ------- |
| 2026-04-13 | validate | Full-file validate: moved OS Comparison after §13; Impl Order `T07`/`T27`/`T12`/`§3` deps; `→ XREF` Inputs; IMPORTANT serial.c line count; OS table § ranges; legend colons; SSDT range hyphen; planned-doc NOTE; Verification `---` before History. |
| 2026-04-13 | gap-analysis | WebSearch x6 + WebFetch x4 (WinDbg kernel-mode, KDNET auto, KDNET-USB, Linux kgdb v6.12, SMP freeze); split §1 serial; add §15 transport/coexistence; OS table + Impl Order row 15; §7 phys helper; cross-patched TODO-01 §3, TODO-26 §2, TODO-28 Inputs+§1; History validate row preserved. |
| 2026-04-13 | validate | Full-file validate: Impl Order row 15 `T26`/`T28 §1`; Inputs `kd_protocol.h`, `kd.c`, `keyboard.c`; OS row kdb/kgdb clarity; §1/§2 list spacing; XREF section targets verified (TODO-32/06/10/16). |
