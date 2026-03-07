# Phase 13 — Long-Term Features

> **Goal:** Add polish and advanced features that make Impossible OS a mature,
> inclusive, and developer-friendly platform: accessibility for all users, developer
> debugging tools, touch/gamepad input support, printer output, text-to-speech,
> multi-user sessions, telemetry, and parental controls — features that elevate the
> OS from a hobby project to a production-grade experience.

---

## 1. Accessibility
> *Research: [01_accessibility.md](research/phase_13_longterm/01_accessibility.md)*

### 1.1 High Contrast Mode

- [ ] Create `src/desktop/accessibility.c` and `include/accessibility.h`
- [ ] Define high contrast theme in Codex:
  - [ ] Background: `#000000`, Foreground: `#FFFFFF`, Accent: `#FFFF00`, Links: `#00FFFF`, Errors: `#FF0000`
- [ ] `accessibility_set_high_contrast(enabled)` — swap entire UI color palette
- [ ] All widgets (buttons, text, menus) use theme colors when high contrast is active
- [ ] Codex: `System\Accessibility\HighContrast = 0`
- [ ] Commit: `"desktop: high contrast mode"`

### 1.2 Large Text / DPI Scaling

- [ ] DPI override: force 150%, 200%, 250%, 300% scale
- [ ] Scale all font rendering by DPI factor
- [ ] Scale window chrome, icons, cursor, widgets
- [ ] Codex: `System\Accessibility\DPIScale = 100`
- [ ] Commit: `"desktop: DPI scaling override"`

### 1.3 Sticky Keys

- [ ] 5× Shift → toggle Sticky Keys on/off
- [ ] When active: modifier keys (Shift, Ctrl, Alt) stay pressed until next key
- [ ] Visual indicator: modifier key status in system tray
- [ ] Codex: `System\Accessibility\StickyKeys = 0`
- [ ] Commit: `"desktop: sticky keys"`

### 1.4 Cursor Accessibility

- [ ] Large cursor option: 48px, 64px (default 32px)
- [ ] Cursor color: white with black border (default) or inverse
- [ ] Codex: `System\Accessibility\CursorSize = 32`
- [ ] Commit: `"desktop: large cursor option"`

### 1.5 Magnifier

- [ ] Win+Plus → zoom in (2×, 4×, 6×, 8×)
- [ ] Win+Minus → zoom out
- [ ] Win+Escape → close magnifier
- [ ] Magnifier follows cursor: zoom area centered on mouse position
- [ ] Render magnified region from compositor backbuffer
- [ ] Commit: `"desktop: screen magnifier"`

### 1.6 Mouse Keys

- [ ] Numpad keys move cursor when activated:
  - [ ] 4/6 — left/right, 2/8 — down/up, 7/9/1/3 — diagonal
  - [ ] 5 — click, + — double-click, 0 — press, . — release
- [ ] Codex: `System\Accessibility\MouseKeys = 0`
- [ ] Commit: `"desktop: mouse keys (numpad cursor control)"`

### 1.7 Reduced Motion & Color Blind Mode

- [ ] Reduced motion: disable all UI animations (window open/close/minimize transitions)
- [ ] Codex: `System\Accessibility\ReducedMotion = 0`
- [ ] *(Stretch)* Color blind simulation modes:
  - [ ] Deuteranopia (red-green), Protanopia (red-green), Tritanopia (blue-yellow)
  - [ ] Apply color matrix transform at compositor level
- [ ] Commit: `"desktop: reduced motion + color blind modes"`

### 1.8 Accessibility Settings Applet

- [ ] `accessibility.spl` in Settings Panel:
  - [ ] High contrast toggle
  - [ ] DPI scale slider
  - [ ] Sticky keys toggle
  - [ ] Mouse keys toggle
  - [ ] Cursor size selector
  - [ ] Reduced motion toggle
  - [ ] Magnifier toggle
  - [ ] *(Stretch)* Color blind mode selector
  - [ ] *(Stretch)* Screen reader toggle
- [ ] Commit: `"apps: accessibility settings applet"`

---

## 2. Developer Tools
> *Research: [02_developer_tools.md](research/phase_13_longterm/02_developer_tools.md)*

### 2.1 Debug Console (F12)

- [ ] Create `src/kernel/debug/debug_console.c`
- [ ] F12 → toggle semi-transparent overlay panel at bottom of screen
- [ ] Tabs: [Kernel] [Memory] [Network] [Syscalls]
- [ ] **Kernel tab**: real-time stream of `printk()` messages with timestamps
- [ ] Scrollable log, newest at bottom
- [ ] Filter by subsystem: wm, fs, net, mem, drv
- [ ] Command input line at bottom: type commands (e.g., `mem`, `ps`, `net`)
- [ ] Codex: `System\Developer\DebugConsole = 0`
- [ ] Commit: `"debug: F12 debug console overlay"`

### 2.2 Memory Inspector

- [ ] **Memory tab** in debug console:
  - [ ] Physical memory map: used/free ranges
  - [ ] Per-process virtual memory usage
  - [ ] Heap fragmentation visualization (bar chart)
  - [ ] Total used / total free / largest free block
- [ ] Shell command: `memmap` — dump physical memory layout
- [ ] Commit: `"debug: memory inspector"`

### 2.3 Syscall Tracer (strace)

- [ ] **Syscalls tab** in debug console:
  - [ ] Live stream of syscalls: name, args, return value, timing
  - [ ] Filter by process
- [ ] Shell command: `strace <program>` — trace all syscalls of a process
  - [ ] Output: `SYS_OPEN("readme.txt", READ) = 3 (0.2ms)`
- [ ] Commit: `"debug: syscall tracer (strace)"`

### 2.4 Performance Profiler

- [ ] FPS counter overlay (Codex: `System\Developer\ShowFPS = 0`)
  - [ ] Show in corner: `60 FPS | 16.7ms`
- [ ] Frame time graph: last 120 frames as bar chart
- [ ] Syscall timing: average time per syscall type
- [ ] IPC throughput: messages/second
- [ ] Commit: `"debug: performance profiler + FPS overlay"`

### 2.5 Pixel Inspector

- [ ] *(Stretch)* Alt+hover → overlay showing:
  - [ ] RGBA color value under cursor (hex + decimal)
  - [ ] Window ID and title at that position
  - [ ] Z-order / compositor layer
- [ ] Commit: `"debug: pixel inspector"`

---

## 3. Bluetooth
> *Research: [03_bluetooth.md](research/phase_13_longterm/03_bluetooth.md)*

### 3.1 Bluetooth Stack

- [ ] *(Stretch)* Evaluate **BlueKitchen BTstack** (MIT, ~15K lines, embedded-friendly)
- [ ] *(Stretch)* Port HCI transport layer (USB-BT adapter via USB host stack)
- [ ] *(Stretch)* L2CAP implementation: packet multiplexing
- [ ] *(Stretch)* SDP: service discovery (find nearby devices)
- [ ] *(Stretch)* RFCOMM: serial port profile (serial-over-BT)
- [ ] *(Stretch)* A2DP: audio streaming to BT headphones
- [ ] *(Stretch)* BLE: Low Energy for modern IoT devices
- [ ] *(Stretch)* Note: QEMU has no BT emulation — requires real hardware + USB passthrough
- [ ] Commit: `"drivers: Bluetooth stack (BTstack port)"`

### 3.2 Bluetooth Settings

- [ ] *(Stretch)* `bluetooth.spl` settings applet:
  - [ ] Enable/disable Bluetooth
  - [ ] Scan for devices
  - [ ] Pair/unpair devices
  - [ ] Connected devices list
- [ ] *(Stretch)* System tray: BT icon (on/off/connected)

---

## 4. Touch & Gesture Support
> *Research: [04_touch_gestures.md](research/phase_13_longterm/04_touch_gestures.md)*

### 4.1 Touch Input Driver

- [ ] Create `src/kernel/drivers/touch.c` and `include/touch.h`
- [ ] Define `struct touch_event` (type, finger_id, x, y, pressure)
- [ ] Touch event types: `TOUCH_DOWN`, `TOUCH_MOVE`, `TOUCH_UP`
- [ ] Multi-touch: track up to 10 simultaneous finger IDs
- [ ] QEMU: `-device usb-tablet` sends absolute coordinates (already partially handled)
- [ ] Real hardware: USB HID touchscreen or I2C touch controller
- [ ] Commit: `"drivers: touchscreen input"`

### 4.2 Gesture Recognition

- [ ] Create `src/kernel/gesture.c` and `include/gesture.h`
- [ ] Recognize gestures from touch event streams:
  | Gesture | Fingers | Action |
  |---------|---------|--------|
  | Tap | 1 | Click |
  | Double tap | 1 | Double-click |
  | Long press (>500ms) | 1 | Right-click |
  | Swipe left/right | 1 | Back/forward |
  | Swipe from edge | 1 | Open action center |
  | Pinch in/out | 2 | Zoom in/out |
  | Two-finger scroll | 2 | Scroll up/down |
  | Three-finger swipe | 3 | Switch virtual desktop |
- [ ] Emit gesture events to WM → route to focused window
- [ ] Commit: `"kernel: gesture recognition engine"`

---

## 5. Printer Support
> *Research: [05_printer_support.md](research/phase_13_longterm/05_printer_support.md)*

### 5.1 PDF Export (Print-to-PDF)

- [ ] Create `src/kernel/print.c` and `include/print.h`
- [ ] Implement basic PDF writer (~500 lines):
  - [ ] PDF header, pages, content streams
  - [ ] Text rendering (font references + positioning)
  - [ ] Basic images (JPEG embedded)
- [ ] "Print" → "Save as PDF" in any application
- [ ] Save to user-chosen path
- [ ] Commit: `"kernel: print-to-PDF"`

### 5.2 Network Printing (IPP)

- [ ] *(Stretch)* IPP (Internet Printing Protocol) client over HTTP (port 631)
- [ ] *(Stretch)* `print_enum_printers(names, max)` — discover IPP printers on network
- [ ] *(Stretch)* `print_document(printer, data, size, format)` — submit print job
- [ ] *(Stretch)* Support PostScript or PCL page description
- [ ] *(Stretch)* Print dialog: select printer, copies, page range
- [ ] Commit: `"kernel: IPP network printing"`

### 5.3 USB Printing

- [ ] *(Stretch)* USB Printer Class driver
- [ ] *(Stretch)* Raw data over USB bulk transfers
- [ ] *(Stretch)* Requires PostScript/PCL rendering
- [ ] Commit: `"drivers: USB printer support"`

---

## 6. Gamepad / Joystick Support
> *Research: [06_gamepad_joystick.md](research/phase_13_longterm/06_gamepad_joystick.md)*

### 6.1 Gamepad Driver

- [ ] Create `src/kernel/drivers/usb/usb_gamepad.c` and `include/gamepad.h`
- [ ] Define `struct gamepad_state`:
  - [ ] buttons (bitfield: A, B, X, Y, LB, RB, Start, Select, L3, R3)
  - [ ] left_x, left_y, right_x, right_y (analog sticks, -32768 to +32767)
  - [ ] left_trigger, right_trigger (0–255)
  - [ ] dpad (UP, DOWN, LEFT, RIGHT)
- [ ] Match USB HID device with gamepad usage page
- [ ] Parse HID report for Xbox-style layout
- [ ] Implement `gamepad_count()` — number of connected controllers
- [ ] Implement `gamepad_poll(index, state)` — read current state
- [ ] Depends on: USB HID driver (Phase 08)
- [ ] QEMU: requires USB passthrough for real gamepad
- [ ] Commit: `"drivers: USB gamepad/joystick"`

### 6.2 Gamepad API for Apps

- [ ] Syscall: `SYS_GAMEPAD_POLL(index, state_buf)` — user-mode access
- [ ] Rumble/vibration: `gamepad_rumble(index, left_motor, right_motor)` (if supported)
- [ ] Gamepad → keyboard mapping (optional): map buttons to key events for apps without gamepad support
- [ ] Commit: `"kernel: gamepad API"`

---

## 7. Text-to-Speech
> *Research: [07_text_to_speech.md](research/phase_13_longterm/07_text_to_speech.md)*

### 7.1 TTS Engine

- [ ] Create `src/kernel/tts.c` and `include/tts.h`
- [ ] Port **SAM** (Software Automatic Mouth, public domain, ~2K lines):
  - [ ] Phoneme-based speech synthesis
  - [ ] Generate PCM audio from text
  - [ ] Redirect audio output to `audio_play()`
- [ ] Implement API:
  - [ ] `tts_speak(text)` — blocking: synthesize and play
  - [ ] `tts_speak_async(text)` — non-blocking
  - [ ] `tts_set_rate(rate)` — speech speed (0=slow, 100=fast)
  - [ ] `tts_set_voice(voice)` — select voice preset
  - [ ] `tts_stop()` — cancel current speech
- [ ] Depends on: audio system (Phase 08)
- [ ] Commit: `"kernel: text-to-speech (SAM port)"`

### 7.2 Screen Reader

- [ ] *(Stretch)* Integrate TTS with UI focus system:
  - [ ] Read focused widget label/text aloud
  - [ ] Read window title on focus change
  - [ ] Read menu items on navigation
  - [ ] Read notification toasts
- [ ] *(Stretch)* Toggle: Codex `System\Accessibility\ScreenReader = 0`
- [ ] Commit: `"desktop: screen reader"`

### 7.3 Upgrade to espeak-ng (Future)

- [ ] *(Stretch)* Port **espeak-ng** (GPL-3, ~200K lines) for higher quality
- [ ] *(Stretch)* Multiple language support
- [ ] *(Stretch)* SSML markup for fine control

---

## 8. Multi-User Sessions
> *Research: [08_multi_user_sessions.md](research/phase_13_longterm/08_multi_user_sessions.md)*

### 8.1 Session Management

- [ ] Create `src/kernel/session.c` and `include/session.h`
- [ ] Define `struct user_session`:
  - [ ] uid, username
  - [ ] `desktop_surface` — per-session compositor backbuffer
  - [ ] `window_list` — separate window list per user
  - [ ] `processes` — processes owned by this session
  - [ ] `active` — flag for currently displayed session
- [ ] `session_create(uid)` — allocate session resources
- [ ] `session_destroy(uid)` — free all resources + kill processes
- [ ] Commit: `"kernel: user session management"`

### 8.2 Fast User Switching

- [ ] Start → Switch User → lock screen appears
- [ ] User A's apps continue running in background (not visible)
- [ ] User B logs in → gets own desktop + compositor + window list
- [ ] Switch back → User A's desktop restored instantly (no reloading)
- [ ] Memory cost: each session duplicates compositor state
- [ ] Limit: max 3 simultaneous sessions (configurable)
- [ ] Depends on: user accounts (Phase 09)
- [ ] Commit: `"desktop: fast user switching"`

---

## 9. Telemetry & Diagnostics
> *Research: [09_telemetry.md](research/phase_13_longterm/09_telemetry.md)*

### 9.1 Telemetry Collection

- [ ] Create `src/kernel/telemetry.c` and `include/telemetry.h`
- [ ] **OFF by default** — user must opt in explicitly
- [ ] Collect (when opted in):
  - [ ] OS version + architecture
  - [ ] Crash dumps (anonymized)
  - [ ] Boot time measurement
  - [ ] Hardware info: CPU model, RAM size, GPU, NIC
  - [ ] Feature usage counts (which apps launched, which settings changed)
- [ ] **No personal data**: no files, passwords, browsing history, usernames
- [ ] Store locally: `C:\Impossible\System\Diagnostics\telemetry.log`
- [ ] Codex: `System\Privacy\Telemetry = 0` (0=off, 1=basic, 2=full)
- [ ] Commit: `"kernel: telemetry collection (opt-in)"`

### 9.2 Diagnostic Report

- [ ] `telemetry_generate_report(path)` — export diagnostics to file
- [ ] Upload only on explicit user action: "Send diagnostic report" button
- [ ] *(Stretch)* HTTP POST to `https://impossible-os.dev/api/telemetry`
- [ ] Privacy settings: `privacy.spl` applet — telemetry level, view collected data, clear data
- [ ] Commit: `"kernel: diagnostic report generation"`

---

## 10. Parental Controls
> *Research: [10_parental_controls.md](research/phase_13_longterm/10_parental_controls.md)*

### 10.1 Screen Time Limits

- [ ] Create `src/kernel/parental.c` and `include/parental.h`
- [ ] Set max daily usage (minutes): Codex `User\{child}\ParentalControls\ScreenTimeLimit = 120`
- [ ] Track active time per user session
- [ ] 15-minute warning before limit reached
- [ ] Auto-lock when limit hit: "Screen time limit reached. Ask a parent to extend."
- [ ] Commit: `"kernel: screen time limits"`

### 10.2 Time Scheduling

- [ ] Set allowed hours: e.g., only 3 PM – 8 PM on weekdays
- [ ] Codex: `User\{child}\ParentalControls\AllowedStart = 15:00`
- [ ] Codex: `User\{child}\ParentalControls\AllowedEnd = 20:00`
- [ ] Auto-lock outside allowed hours
- [ ] Commit: `"kernel: time scheduling"`

### 10.3 App Blocking

- [ ] Block list of specific apps by executable name
- [ ] Codex: `User\{child}\ParentalControls\BlockedApps = "browser.exe,ssh.exe"`
- [ ] Check on process exec: if app is blocked → deny + show message
- [ ] Commit: `"kernel: app blocking"`

### 10.4 Activity Report

- [ ] Log apps launched by child account: app name + duration
- [ ] Store: `C:\Users\{child}\AppData\ParentalLogs\{date}.log`
- [ ] Admin can view activity in parental settings
- [ ] Commit: `"kernel: activity logging"`

### 10.5 Parental Controls Settings

- [ ] `parental.spl` settings applet (**admin-only** access):
  - [ ] Select child user account
  - [ ] Screen time limit slider (30 min – 8 hours)
  - [ ] Time schedule (start/end hours per day)
  - [ ] App block list (add/remove apps)
  - [ ] View activity report
  - [ ] *(Stretch)* Website block list (after browser exists)
- [ ] Commit: `"apps: parental controls settings"`

---

## 11. Agent-Recommended Additions

> Items not in the research files but valuable for a mature OS.

### 11.1 Keyboard Shortcuts Reference

- [ ] Win+F1 → show keyboard shortcuts overlay (full-screen cheat sheet)
- [ ] List all global shortcuts: window management, accessibility, developer, navigation
- [ ] Dismiss with Escape or any key
- [ ] Commit: `"desktop: keyboard shortcuts reference overlay"`

### 11.2 Power Management Profiles

- [ ] Power profiles: Balanced (default), Performance (no sleep), Power Saver (aggressive sleep)
- [ ] Profiles control: display sleep timeout, CPU throttling (if applicable)
- [ ] `power.spl` settings applet
- [ ] Codex: `System\Power\Profile = "Balanced"`
- [ ] Commit: `"kernel: power management profiles"`

### 11.3 Scripting Engine (Macro/Automation)

- [ ] *(Stretch)* Simple scripting language for automation:
  - [ ] Execute shell commands
  - [ ] Schedule actions
  - [ ] GUI automation (click, type, wait)
- [ ] `.iscript` files: run from shell or double-click
- [ ] Commit: `"kernel: scripting engine"`

### 11.4 Help System

- [ ] Built-in help viewer: `help.exe`
- [ ] Browse `.hlp` files stored in `C:\Impossible\System\Help\`
- [ ] F1 in any app → open context-sensitive help
- [ ] Contents: getting started guide, keyboard shortcuts, settings reference
- [ ] Commit: `"apps: help system"`

---

## Priority Order

| Priority | Section | Reason |
|----------|---------|--------|
| 🟠 P1 | 1.1–1.4 Core Accessibility | Inclusion — high contrast, large text, sticky keys, cursor |
| 🟠 P1 | 2.1 Debug Console (F12) | Essential for development |
| 🟡 P2 | 1.5–1.6 Magnifier + Mouse Keys | Additional accessibility |
| 🟡 P2 | 2.2–2.3 Memory Inspector + strace | Debugging tools |
| 🟡 P2 | 5.1 Print-to-PDF | Basic print support |
| 🟡 P2 | 11.1 Keyboard Shortcuts | UX discoverability |
| 🟢 P3 | 4.1–4.2 Touch + Gestures | Tablet/touch devices |
| 🟢 P3 | 6.1 Gamepad Driver | Gaming input |
| 🟢 P3 | 7.1 TTS (SAM) | Accessibility audio |
| 🟢 P3 | 9.1 Telemetry | Usage analytics |
| 🟢 P3 | 10.1–10.5 Parental Controls | Family safety |
| 🟢 P3 | 2.4 Performance Profiler | Optimization |
| 🟢 P3 | 1.7–1.8 Motion + Color Blind + Settings | Polish |
| 🔵 P4 | 8. Multi-User Sessions | Fast user switching |
| 🔵 P4 | 3. Bluetooth | Hardware-dependent |
| 🔵 P4 | 5.2–5.3 Network/USB Printing | Complex hardware |
| 🔵 P4 | 7.2–7.3 Screen Reader + espeak | Advanced TTS |
| 🔵 P4 | 11.2–11.4 Power Profiles + Scripting + Help | Future polish |
