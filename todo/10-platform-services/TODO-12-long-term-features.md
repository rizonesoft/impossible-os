---
schema_version: 1
id: long-term-features
domain: 10-platform-services
status: active
title: "TODO-12 -- Long-Term Features"
---

# TODO-12 -- Long-Term Features

**Domain:** `10-platform-services`
**Goal:** Track the advanced features that elevate Impossible OS to a mature production platform -- developer tools, kernel/user-mode debugger, touch/gamepad input, print, TTS, software OpenGL, multi-user sessions, telemetry, and parental controls.

> [!IMPORTANT]
> **Depends on:** Core OS complete -- ring-3 PE execution (`TODO-07`), Win32 API surface (`TODO-08`), audio system (`10-platform-services/TODO-01`), user accounts (`10-platform-services/TODO-03 §*`), IxUI (`TODO-08 §13`). No section here is a prerequisite for any other active TODO.
> **Long-term scope:** Nothing in this TODO is P0 or P1. All sections are P2–P4 power-user and ecosystem features. Each section is independently implementable.

---

## Important Notes

- `serial_write()` / `serial_putchar()` exist in `include/kernel/drivers/serial.h` -- the GDB remote stub (§1) and debug console (§2) both output over COM1.
- No existing `kdb_*`, `tts_*`, `session_*`, or `parental_*` APIs -- all new.
- The GDB remote stub (§1) uses the **GDB RSP (Remote Serial Protocol)** -- a well-documented text protocol; QEMU connects with `-gdb tcp::1234 -S`.
- The user-mode debugger (§2) uses hardware debug registers (`DR0`–`DR7`) for hardware breakpoints and `INT3` patching for software breakpoints -- both require ring-0 kernel support with a `DR`-read/write syscall.
- TTS engine: prompt specifies **eSpeak-NG** (LGPL); the old TODO used **SAM** (public domain, ~2 K lines). Use SAM as the initial port (simpler), with eSpeak-NG as the upgrade path.
- OpenGL: prompt specifies **TinyGL** (~5 K lines, zlib license) -- already named in `todo-old`. No existing OpenGL infrastructure.
- Multi-user sessions (§8) depend on user accounts and a per-session compositor surface; overlaps with `10-platform-services/TODO-03` (accounts) and `08-graphics-ui/TODO-02` (compositor) -- cross-link, don't duplicate.
- Telemetry (§9) is **opt-in only**, zero by default. `HKLM\SYSTEM\Privacy\Telemetry = 0`.
- Parental controls (§10) hook into `SYS_CREATEPROCESS` to block apps -- that hook point is in the kernel process creation path (`TODO-07 §7`).

---

## Inputs

| Path | Purpose |
|------|---------|
| `include/kernel/drivers/serial.h` | `serial_write()`, `serial_putchar()` -- GDB stub + debug console output |
| `include/kernel/sched/task.h` | `task_t`, `task_create_user()`, `thread_create()` -- debugger + session attach |
| `include/kernel/sched/syscall.h` | Syscall dispatch -- add `SYS_TTS_SPEAK`, `SYS_GAMEPAD_POLL`, debug regs |
| `include/kernel/mm/vmm.h` | `vmm_read_user()`, `vmm_write_user()` -- `ReadProcessMemory`/`WriteProcessMemory` |
| `include/registry.h` | `registry_get/set()` -- debug console, telemetry, parental controls flags |
| `include/desktop/wm.h` | `wm_create_window()` -- debug overlay, session compositor surface |
| → XREF: `10-platform-services/TODO-01` | `audio_play()` -- TTS PCM output |
| → XREF: `10-platform-services/TODO-08 §10–13` | IxUI windows -- debug console, parcon.cpl, gamepad settings |
| → XREF: `04-drivers-hardware/TODO-10-usb-stack.md` | USB HID -- touch digitizer and gamepad hardware input |
| → XREF: `08-graphics-ui/TODO-02` | Compositor -- per-session backbuffer for multi-user |
| → XREF: `10-platform-services/TODO-03 §*` | User accounts and per-user registry hives (parental controls) |

---

## Outcome

- GDB remote stub on COM1: `gdb kernel.exe` + `target remote :1234` → breakpoints, register dump, stack trace.
- `debugger.exe` attaches to running processes, sets INT3 breakpoints, single-steps, dumps registers.
- Multi-touch input events and gesture recognizer (tap/swipe/pinch) feed `WM_TOUCH` to windows.
- USB gamepads register state via `gamepad_poll()`; XInput API stubs work for games.
- Print-to-PDF from any app; IPP network printing (stretch).
- `tts_speak(text)` produces audible speech via SAM + audio mixer.
- TinyGL software rasterizer: `glBegin`/`glEnd` + rotating cube demo runs.
- Fast user switching: up to 3 simultaneous sessions; each gets own compositor surface.
- Opt-in telemetry with `privacy.cpl` viewer; no data leaves without explicit consent.
- `parcon.cpl`: screen time limits, time scheduling, app block list, activity log.

---

## Implementation Order

| #   | Section                                  | Tag        | Dep                                      | Mark |
| --- | ---------------------------------------- | ---------- | ---------------------------------------- | ---- |
| 1   | Kernel debugger (GDB RSP stub)           | `[Opus]`   | serial                                   | 💎   |
| 2   | User-mode debugger (`debugger.exe`, Win32 debug API) | `[Opus]`   | §1, TODO-07 §7                           | 💎   |
| 3   | Developer tools (F12 console, memmap, strace, FPS overlay) | `[Sonnet]` | TODO-08 §2                               | ⭐   |
| 4   | Touch input + gesture recognizer         | `[Opus]`   | XREF: 04-drivers/TODO-09                 | 💎   |
| 5   | Gamepad / controller input + XInput stubs | `[Sonnet]` | XREF: 04-drivers/TODO-09                 | 💎   |
| 6   | Print support (PDF export + IPP stretch) | `[Sonnet]` | TODO-08 §11                              | 💎   |
| 7   | Text-to-speech (SAM port → eSpeak-NG upgrade) | `[Sonnet]` | TODO-01 (audio)                          | 💎   |
| 8   | Software OpenGL (TinyGL port)            | `[Opus]`   | TODO-08 §11 (GDI)                        | 💎   |
| 9   | Multi-user session management + fast switching | `[Opus]`   | TODO-03 (accounts), XREF: 07-gfx/TODO-02 | 💎   |
| 10  | Telemetry (opt-in, anonymous)            | `[Sonnet]` | TODO-08 §2                               | 💎   |
| 11  | Parental controls (`parcon.cpl`)         | `[Sonnet]` | TODO-03 (accounts), TODO-07 §7           | 💎   |

---

## 1. Kernel Debugger (GDB RSP Stub) `[Opus]`

Implement the GDB Remote Serial Protocol over COM1 so a host GDB session can attach to the live kernel.

- [ ] Create `src/kernel/debug/gdb_stub.c` + `include/kernel/debug/gdb_stub.h`
- [ ] GDB RSP framing: `+`/`-` acks, `$packet#checksum` format; `serial_write()`/`serial_putchar()` as transport
- [ ] `gdb_stub_init()` → called from kernel init; installs `SIGTRAP` + `SIGFPE` exception handlers that enter the stub
- [ ] Packet handlers:
  - `?` → reply `S05` (stopped with SIGTRAP)
  - `g` / `G` → read/write all registers (RAX…R15, RIP, RFLAGS, CS, SS, DS)
  - `p N` / `P N=v` → read/write single register by index
  - `m addr,len` / `M addr,len:data` → read/write memory (kernel virtual address)
  - `c [addr]` → continue execution
  - `s [addr]` → single-step (set Trap Flag in RFLAGS, return from stub)
  - `z0,addr,1` / `Z0,addr,1` → remove/insert software breakpoint (patch INT3 at `addr`)
  - `z1,addr,1` / `Z1,addr,1` → remove/insert hardware breakpoint (write `DR0`–`DR3`, configure `DR7`)
  - `q` queries: `qSupported`, `qAttached`
- [ ] `kdb_stack_trace()` → walk RBP chain, emit `klog_info` with symbol names from `symtab` (if available)
- [ ] `kdb_breakpoint(addr)` → insert INT3 patch at `addr` + add to breakpoint list; restore on hit
- [ ] QEMU: `bash scripts/build.sh run` already passes `-serial stdio`; add `-gdb tcp::1234 -S` to enable GDB attach
- [ ] Test: `gdb build/kernel.elf` → `target remote :1234` → `info registers` → correct values
- [ ] Commit: `"debug: GDB remote serial protocol stub (kdb)"`

---

## 2. User-Mode Debugger (`debugger.exe`) `[Opus]`

Win32-compatible debug API + `debugger.exe` GUI application. Hooks into the kernel via privilege-controlled debug syscalls.

**Kernel side (new syscalls):**
- [ ] `SYS_DEBUG_ATTACH(pid)` → mark target `task_t` as `debugged`; pause task
- [ ] `SYS_DEBUG_DETACH(pid)` → un-mark; resume
- [ ] `SYS_READ_PROCESS_MEMORY(pid, addr, buf, size)` → copy from target VMM address space to calling process buffer
- [ ] `SYS_WRITE_PROCESS_MEMORY(pid, addr, buf, size)` → write to target VMM address space; flush instruction cache
- [ ] `SYS_GET_THREAD_CONTEXT(tid, ctx)` / `SYS_SET_THREAD_CONTEXT(tid, ctx)` → read/write `CONTEXT` struct (registers of paused thread)
- [ ] `SYS_DEBUG_WAIT_EVENT(pid, event_buf, timeout)` → block until debuggee raises an event (exception, process/thread create/exit); populate `DEBUG_EVENT`
- [ ] `SYS_DEBUG_CONTINUE(pid, thread_id, status)` → resume after debug event (`DBG_CONTINUE` or `DBG_EXCEPTION_NOT_HANDLED`)
- [ ] Hardware debug register control: `DR0`–`DR3` (address), `DR7` (enable/condition) -- write via privileged kernel call only

**Win32 debug API wrappers (in `src/win32/kernel32.c`):**
- [ ] `DebugActiveProcess(pid)` → `SYS_DEBUG_ATTACH`
- [ ] `DebugActiveProcessStop(pid)` → `SYS_DEBUG_DETACH`
- [ ] `WaitForDebugEvent(lpEvent, timeout)` → `SYS_DEBUG_WAIT_EVENT`; populate `DEBUG_EVENT` struct
- [ ] `ContinueDebugEvent(pid, tid, status)` → `SYS_DEBUG_CONTINUE`
- [ ] `ReadProcessMemory(hProcess, addr, buf, size, read)` → `SYS_READ_PROCESS_MEMORY`
- [ ] `WriteProcessMemory(hProcess, addr, buf, size, written)` → `SYS_WRITE_PROCESS_MEMORY`
- [ ] `GetThreadContext(hThread, ctx)` / `SetThreadContext(hThread, ctx)` → `SYS_GET/SET_THREAD_CONTEXT`

**`debugger.exe` application (`src/apps/debugger/`):**
- [ ] IxUI window: register list (`CTRL_LISTVIEW`), disassembly pane (`CTRL_SCROLLBAR` + text), memory dump pane, breakpoint list
- [ ] `[Attach PID]` → `DebugActiveProcess`; starts debug event loop in background thread
- [ ] Software breakpoint: double-click address in disassembly → `WriteProcessMemory` INT3 patch; on `EXCEPTION_BREAKPOINT` event → restore original byte + single-step + re-patch
- [ ] Single-step: `[Step]` button → set Trap Flag in `CONTEXT.EFlags` → `SetThreadContext` + `ContinueDebugEvent`
- [ ] Register dump updated on each break event
- [ ] Commit: `"debug: user-mode debugger (debugger.exe + Win32 debug API)"`

---

## 3. Developer Tools `[Sonnet]`

F12 debug overlay and shell diagnostics. All gated by registry flags (disabled by default).

- [ ] `HKLM\SYSTEM\Developer\DebugConsole = 0` -- F12 console toggle
- [ ] `HKLM\SYSTEM\Developer\ShowFPS = 0` -- FPS overlay toggle
- [ ] **F12 debug console overlay** (`src/kernel/debug/debug_console.c`):
  - Semi-transparent panel (bottom 30% of screen); `gfx_fill_rect` + `gfx_blend_alpha`
  - Four tabs: **[Kernel]** (live `klog` stream + subsystem filter), **[Memory]** (PMM map), **[Network]** (packet counters), **[Syscalls]** (live syscall stream)
  - Command input line: `ps` → process list, `memmap` → physical map, `netstat` → open sockets, `strace <pid>` → enable syscall trace for PID
- [ ] **Memory tab**: PMM used/free bar, per-process virtual memory `CTRL_LISTVIEW`, heap fragmentation (filled rect proportional to fragmentation ratio), largest free contiguous block
- [ ] **Syscall tracer**: when `strace <pid>` active, each kernel syscall dispatch logs `SYS_NAME(args) = retval [Nms]` to Syscalls tab + serial
- [ ] **FPS overlay**: rendered as a 2×16 pixel watermark in top-right corner; updated every 60 frames; `60 FPS | 16.7 ms`; frame time bar chart (last 120 frames) on hover
- [ ] **Pixel inspector** (stretch): Alt+hover → tooltip with RGBA hex, window title at cursor, compositor z-order layer
- [ ] Commit: `"debug: F12 developer console + FPS overlay + syscall tracer"`

---

## 4. Touch Input + Gesture Recognizer `[Opus]`

Multi-touch input pipeline from USB HID Digitizer to WM events. Depends on USB HID driver (XREF: `04-drivers-hardware/TODO-10-usb-stack.md`).

- [ ] Create `include/kernel/drivers/touch.h`:
  - `struct touch_point { uint8_t id; int32_t x, y; uint16_t pressure; uint8_t phase; /* TOUCH_DOWN/MOVE/UP */ }`
  - `struct touch_event { uint8_t count; struct touch_point points[10]; uint64_t timestamp_ms; }`
  - `touch_event_push(ev)` → enqueue to WM event queue
- [ ] USB HID Digitizer: parse `HID_USAGE_DIGITIZER_CONTACT_ID`, `X`, `Y`, `TIP_SWITCH`, `IN_RANGE` usages; map to `touch_point`
- [ ] QEMU: `-device usb-tablet` sends absolute pointer events → translate to single-finger `touch_point`
- [ ] Gesture recognizer (`src/kernel/gesture.c`):
  - **Tap** (1 finger, contact < 200 ms, displacement < 10 px) → `WM_LBUTTONDOWN` + `WM_LBUTTONUP`
  - **Double-tap** (2 taps < 300 ms apart) → `WM_LBUTTONDBLCLK`
  - **Long-press** (1 finger, > 500 ms, stationary) → `WM_RBUTTONDOWN`
  - **Swipe** (1 finger, > 50 px in < 300 ms) → directional `WM_SWIPE` custom message
  - **Two-finger scroll** → `WM_MOUSEWHEEL` (Y-axis delta)
  - **Pinch** (2-finger distance delta > 20 px) → `WM_GESTURE_ZOOM` with scale factor
  - **Three-finger swipe** → `WM_GESTURE_3FINGER` for virtual desktop switch
- [ ] WM dispatches `WM_TOUCH` (raw touch data) + synthesized mouse events to focused window
- [ ] Commit: `"drivers: touch input + gesture recognizer (tap/swipe/pinch)"`

---

## 5. Gamepad / Controller Input + XInput Stubs `[Sonnet]`

USB HID gamepad driver + XInput-compatible API. Requires USB HID (XREF: `04-drivers-hardware/TODO-10-usb-stack.md`).

- [ ] Create `include/gamepad.h`:
  - `struct gamepad_state { uint32_t buttons; int16_t left_x, left_y, right_x, right_y; uint8_t left_trigger, right_trigger; uint8_t dpad; }`
  - Button constants: `GAMEPAD_A`, `GAMEPAD_B`, `GAMEPAD_X`, `GAMEPAD_Y`, `GAMEPAD_LB`, `GAMEPAD_RB`, `GAMEPAD_START`, `GAMEPAD_SELECT`, `GAMEPAD_L3`, `GAMEPAD_R3`
- [ ] USB HID parser for Xbox-style HID report descriptor (usage page 0x01, generic desktop)
- [ ] `gamepad_count()` → number of connected controllers (max 4)
- [ ] `gamepad_poll(index, state)` → copy latest state; return 0 if disconnected
- [ ] `gamepad_rumble(index, left_motor, right_motor)` → USB HID output report for force feedback (if supported)
- [ ] `SYS_GAMEPAD_POLL(index, state_buf)` syscall for user-mode access
- [ ] WM: `WM_GAMEPAD_BUTTON(button_id, pressed)` events posted to focused window
- [ ] XInput API stubs (`src/win32/xinput.c`):
  - `XInputGetState(dwUserIndex, pState)` → `gamepad_poll()` + map to `XINPUT_STATE`
  - `XInputSetState(dwUserIndex, pVibration)` → `gamepad_rumble()`
  - `XInputEnable(enable)` → no-op
- [ ] Button-to-key mapping table: `GAMEPAD_START` → `VK_ESCAPE`, D-pad → arrow keys (for apps without gamepad support)
- [ ] Commit: `"drivers: USB gamepad + XInput API stubs"`

---

## 6. Print Support `[Sonnet]`

PDF export as the primary print target. IPP network printing and USB printing as stretch goals.

- [ ] **PDF export** (`src/kernel/print.c` + `include/print.h`):
  - `pdf_begin(path, page_width_pt, page_height_pt)` → open file, write PDF header + catalog
  - `pdf_begin_page()` / `pdf_end_page()` → page stream open/close
  - `pdf_draw_text(x, y, font, size, color, str)` → `BT ... Tf Tj ET` content stream
  - `pdf_draw_rect(x, y, w, h, color, filled)` → `re f` or `re S`
  - `pdf_draw_image(x, y, w, h, jpeg_data, size)` → inline JPEG `XObject`
  - `pdf_end(path)` → write xref table + trailer; close file
  - `print_surface_to_pdf(surface, path)` → rasterize `gfx_surface_t` as full-page JPEG → embed in PDF
- [ ] **Print dialog** (IxUI): printer selector (`CTRL_LISTBOX` with "Save as PDF" always present), copies, page range, portrait/landscape toggle
- [ ] `PrintDocument(surface, printer_name)` Win32 API stub → routes to PDF export or IPP (§6.2)
- [ ] **IPP network printing** (stretch): `print_enum_printers(names, max)` via mDNS/DNS-SD; `print_ipp_job(printer_url, pdf_data, size)` via `http_post()` to port 631
- [ ] **USB printer class** (stretch): bulk write to printer endpoint; raw PostScript/PCL passthrough
- [ ] Commit: `"kernel: print-to-PDF + print dialog"`

---

## 7. Text-to-Speech (SAM → eSpeak-NG) `[Sonnet]`

Port SAM (Software Automatic Mouth, public domain, ~2 K lines) as the initial TTS engine. eSpeak-NG is the upgrade path.

- [ ] Create `src/kernel/tts.c` + `include/tts.h`
- [ ] Port SAM source: replace `malloc`/`free` → `kmalloc`/`kfree`; replace POSIX audio output → `audio_play(pcm_buf, size, sample_rate)` from `TODO-01`
- [ ] API:
  - `tts_init()` → load phoneme tables (from `C:\Impossible\System\tts\sam_phonemes.dat`)
  - `tts_speak(text)` → blocking: synthesize text → PCM → `audio_play()`
  - `tts_speak_async(text)` → non-blocking (queue to TTS thread)
  - `tts_set_rate(rate)` → 0 (slow) – 100 (fast); default 50
  - `tts_set_pitch(pitch)` → 0–100; default 50
  - `tts_stop()` → cancel queued speech
- [ ] `SYS_TTS_SPEAK(text_ptr, len)` syscall → user-mode access from PE apps
- [ ] Screen reader mode (stretch): `ctrl_get_accessible_name(ctrl)` → narrate focused widget label on focus change via `tts_speak_async()`
- [ ] eSpeak-NG upgrade path (long-term): replace SAM engine with eSpeak-NG (LGPL, ~100 K lines) for multilingual + higher quality; same `tts_speak()` API
- [ ] Commit: `"kernel: text-to-speech (SAM port + SYS_TTS_SPEAK)"`

---

## 8. Software OpenGL (TinyGL Port) `[Opus]`

Port TinyGL (~5 K lines, zlib license) to render to the Impossible OS framebuffer. Enables 3D apps and games.

- [ ] Obtain TinyGL source (`github.com/C-Chads/tinygl` or similar zlib-licensed fork)
- [ ] Replace platform I/O: `malloc`/`free` → `kmalloc`/`kfree`; `memcpy`/`memset` → kernel versions
- [ ] Framebuffer target: `glFlush()` → `gfx_blit(tinygl_color_buf, x, y, w, h)` to compositor surface
- [ ] Create `include/gl/gl.h`: subset of OpenGL 1.1 -- `glBegin`/`glEnd`, `glVertex3f`, `glColor3f`, `glTexImage2D`, `glBindTexture`, `glEnable`/`glDisable`, `glMatrixMode`, `glLoadIdentity`, `glTranslatef`, `glRotatef`, `glScalef`, `glFrustum`, `glOrtho`, `glViewport`, `glClear`, `glClearColor`, `glFlush`
- [ ] Z-buffer: 16-bit or 32-bit float depth buffer allocated via `vmm_alloc_user()` or `kmalloc`
- [ ] `gl_init(width, height)` → allocate color + depth buffers; set default viewport
- [ ] Test: rotating textured cube at 30+ FPS in QEMU (software rasterizer expected to be slow)
- [ ] `opengl32.dll` import stub → `GetProcAddress()` resolves TinyGL function pointers
- [ ] Enables basic 3D game ports and CAD viewer applications
- [ ] Commit: `"gfx: TinyGL software OpenGL port"`

---

## 9. Multi-User Session Management + Fast Switching `[Opus]`

Per-user sessions with isolated compositor surfaces. Depends on user accounts (`TODO-03`) and compositor (`08-graphics-ui/TODO-02`).

- [ ] Create `include/kernel/session.h`:
  - `struct user_session { uint32_t uid; char username[64]; gfx_surface_t *desktop_surface; int *window_list; uint32_t process_count; uint8_t active; }`
  - `session_create(uid)` → allocate per-session compositor backbuffer (`pmm_alloc_contiguous`), window list, session registry hive mount
  - `session_destroy(uid)` → kill all session processes, free compositor surface, unmount hive
- [ ] `session_switch(uid)` → save current active session state (pause compositor rendering), load target session; swap `fb_get_backbuffer()` pointer to target `desktop_surface`; resume
- [ ] Max 3 simultaneous sessions (configurable via `HKLM\SYSTEM\Session\MaxSessions`)
- [ ] Fast switch UX: Start → "Switch User" → lock screen + user selector; User B logs in → new session; Win+L → lock current, return to session list
- [ ] Session 0 isolation: background services (audio mixer, print spooler, telemetry) run in Session 0 with no interactive desktop; user sessions start at Session 1+
- [ ] `SYS_GET_SESSION_ID()` syscall → return calling process's session ID
- [ ] VNC-like remote session (stretch): stream compositor surface over TCP to a VNC client
- [ ] Commit: `"kernel: multi-user session management + fast user switching"`

---

## 10. Telemetry (Opt-In, Anonymous) `[Sonnet]`

Privacy-first, zero-by-default telemetry. All collection requires explicit user consent.

- [ ] `HKLM\SYSTEM\Privacy\Telemetry = 0` (0 = off, 1 = basic, 2 = full) -- set during OOBE; default 0
- [ ] Create `src/kernel/telemetry.c` + `include/telemetry.h`:
  - `telemetry_init()` → read registry; if 0 → no-op
  - `telemetry_record_event(type, data)` → if enabled: append JSON line to `C:\Impossible\System\Diagnostics\telemetry.log` (rotating, max 1 MiB)
  - Event types: `BOOT_TIME(ms)`, `CRASH_DUMP(anonymized_hash)`, `FEATURE_USE(feature_name)`, `HW_INFO(cpu_model, ram_gb)` -- no filenames, passwords, or user-identifying data ever
  - `telemetry_generate_report(path)` → export collected log as structured JSON
- [ ] Upload (stretch): HTTP POST to `https://telemetry.impossible-os.dev/api/v1/report` -- only on explicit `[Send Report]` button click; never automatic
- [ ] `privacy.cpl` Control Panel applet:
  - Telemetry level radio group (Off / Basic / Full)
  - `[View collected data]` → opens `telemetry.log` in Notepad
  - `[Clear all data]` → delete `telemetry.log`
  - `[Send diagnostic report]` → triggers HTTP upload (opt-in, one-time)
- [ ] Commit: `"kernel: opt-in telemetry + privacy.cpl applet"`

---

## 11. Parental Controls (`parcon.cpl`) `[Sonnet]`

Per-user time limits, app blocking, and activity logging. Admin password required for all changes.

**Kernel enforcement hook:**
- [ ] In `SYS_CREATEPROCESS` handler: read `HKU\{uid}\ParentalControls\BlockedApps`; if process name in list → deny with `ERROR_ACCESS_DENIED` + notify child
- [ ] Screen time tracking: per-user active-session tick counter (incremented by PIT/APIC timer ISR when session is active); check against `ScreenTimeLimit` registry key

**Per-child registry keys (under `HKU\{child_uid}\ParentalControls\`):**
- [ ] `ScreenTimeLimit` (REG_DWORD, minutes per day; 0 = unlimited)
- [ ] `AllowedStart` (REG_SZ, `"HH:MM"`) + `AllowedEnd` (REG_SZ, `"HH:MM"`)
- [ ] `BlockedApps` (REG_MULTI_SZ, list of executable names)
- [ ] `EnableLogging` (REG_DWORD, 0/1)

**Runtime behavior:**
- [ ] 15-minute warning: toast notification "15 minutes of screen time remaining"
- [ ] Limit reached: auto-lock session → overlay "Screen time limit reached. Ask a parent to extend." + `[Parent Override]` button (prompts admin password)
- [ ] Outside allowed hours: same lock + overlay
- [ ] Activity log: on process exit, append `{timestamp, exe_name, duration_sec}` line to `C:\Users\{child}\AppData\ParentalLogs\{YYYY-MM-DD}.log`

**`parcon.cpl` applet (admin-only):**
- [ ] User selector (`CTRL_LISTBOX` of child accounts)
- [ ] Screen time slider (0–480 min, `CTRL_SCROLLBAR` + label)
- [ ] Allowed hours: start/end time pickers (hour/minute dropdowns)
- [ ] App block list: `CTRL_LISTBOX` + `[Add]` (file picker) + `[Remove]`
- [ ] Activity report: `CTRL_LISTBOX` of log entries per date
- [ ] Website block list (stretch, post-browser): domain deny list → injected into DNS resolver
- [ ] `[Apply]` button writes all changes to `HKU\{child}\ParentalControls\` via `registry_set()`
- [ ] Commit: `"apps: parental controls (parcon.cpl + SYS_CREATEPROCESS hook)"`

---

## OS Comparison


| ⭐  | Feature                                  | 🪟 Win11             | 🐧 Linux               | 🚀 Impossible OS                         |
| --- | ---------------------------------------- | -------------------- | ---------------------- | ---------------------------------------- |
| 💎  | Kernel debugger                          | ✅ WinDbg KD         | ✅ KGDB (GDB RSP)      | ⬜ GDB RSP on COM1                       |
| 💎  | User-mode debugger with Win32 debug API  | ✅ WinDbg user       | ✅ GDB/ptrace          | ⬜ `debugger.exe` + INT3/DR*             |
| 💎  | Developer console overlay                | ✅ DevHome           | ✅ Various             | ⬜ F12 overlay (⭐ in-kernel, not        |
| 💎  | Multi-touch input + gestures             | ✅ WM_TOUCH          | ✅ libinput            | ⬜ `touch_point` + gesture engine        |
| 💎  | Gamepad / XInput API                     | ✅ XInput            | ✅ SDL2/evdev          | ⬜ `gamepad_poll()` + XInput stubs       |
| 💎  | Print-to-PDF                             | ✅ MS Print to PDF   | ✅ CUPS/PDF            | ⬜ native PDF writer                     |
| 💎  | Text-to-speech                           | ✅ SAPI/Narrator     | ✅ eSpeak              | ⬜ SAM port + `SYS_TTS_SPEAK`            |
| 💎  | Software OpenGL                          | ✅ WARP d3d11        | ✅ Mesa llvmpipe       | ⬜ TinyGL port                           |
| 💎  | Multi-user fast switching                | ✅ Win11 switch      | ✅ DM sessions         | ⬜ per-session compositor                |
| 💎  | Opt-in telemetry                         | ✅ Windows telemetry | ✅ Ubuntu opt-in       | ⬜ zero by default                       |
| 💎  | Parental controls                        | ✅ Family Safety     | ✅ Various             | ⬜ kernel-enforced                       |
| ⭐  | F12 in-kernel debug console              | ❌ DevTools are apps | ❌ External tools      | ⬜ composited overlay, zero process overhead |
| ⭐  | Kernel GDB stub + user debugger in same OS | ❌ Separate KD + VS  | ❌ KGDB + GDB separate | ⬜ unified debug story                   |

**Impossible OS advantage:** The F12 debug console is implemented in-kernel with zero process overhead -- it reads live kernel state without IPC, making it faster than any userland tool. The GDB stub and user-mode debugger share the same OS, giving a unified debugging story from kernel panic to user-mode INT3 that no other OS provides out of the box.

---

## Verification

**§1: GDB stub**
- `bash scripts/build.sh run -- -gdb tcp::1234 -S` → QEMU waits; `gdb build/kernel.elf` + `target remote :1234` + `info registers` → RAX/RIP read correctly
- `break kernel_main` → `continue` → breakpoint hit; `bt` shows stack trace

**§2: User debugger**
- `debugger.exe` → `[Attach PID]` on `hello.exe` → registers shown; `[Step]` advances RIP by one instruction
- INT3 breakpoint at address → execution pauses; memory read panel shows bytes around RIP

**§3: Developer console**
- F12 → overlay visible at bottom of screen; Kernel tab shows live `klog` lines with timestamps
- `strace 3` in command line → Syscalls tab shows each syscall name + return value for PID 3
- `ShowFPS=1` in registry → FPS counter appears in top-right corner

**§7: TTS**
- `tts_speak("Hello from Impossible OS")` → audible speech via speakers in QEMU (requires `-soundhw hda`)
- `SYS_TTS_SPEAK` from a PE process → same output

**§8: OpenGL**
- Rotating cube demo PE app → visible 3D cube at 15+ FPS in QEMU software mode
- `opengl32.dll` loaded; `GetProcAddress(hmod, "glBegin")` returns non-NULL

**§10: Telemetry**
- Boot with `Telemetry=0` → no `telemetry.log` created; no HTTP traffic
- Set `Telemetry=1` → `telemetry.log` appears after reboot; `privacy.cpl` shows collected events; `[Clear all data]` deletes file

**§11: Parental controls**
- Set `BlockedApps = "notepad.exe"` for child user; log in as child → run `notepad.exe` → denied with notification
- Screen time = 5 min → after 5 minutes → session locks; parent password dialog appears
