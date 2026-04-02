# TODO-07 — Desktop & UI Test Framework

> **Goal:** Automated testing for the graphical desktop: window creation, compositor rendering, input event processing, widget controls, terminal output, and visual regression detection. UI bugs are the hardest to catch because they require eyes — this framework replaces eyes with framebuffer snapshots and pixel-level comparison. A window that renders wrong, a button that doesn't respond, or a terminal that drops characters gets caught automatically.

> [!IMPORTANT]
> **Current state:** The desktop (compositor, window manager, terminal, gallery) is tested entirely by manual visual inspection — launch QEMU, look at the screen, move the mouse, type commands. No automated testing. No screenshot capture. No input event injection. No render verification. If a compositor change silently breaks window decorations or the terminal, nobody knows until someone looks.

---

## Inputs

- `src/desktop/wm.c` — window manager (create, move, resize, focus)
- `src/desktop/terminal.c` — command prompt window
- `src/desktop/controls.c` — UI controls (buttons, text, gallery)
- `src/desktop/desktop.c` — desktop surface, taskbar, wallpaper
- `src/kernel/main/compositor.c` — render loop, framebuffer compositing
- `src/kernel/drivers/framebuffer.c` — framebuffer access, page flip
- → XREF: `TODO-03-kernel-test-framework.md §2` — `make test` target (local headless QEMU, no CI QEMU)
- → XREF: `TODO-02-usermode-test-framework.md §2` — test launcher mechanism

---

## Outcome

- Framebuffer snapshot API: capture the current screen as a raw bitmap from within the kernel.
- Reference screenshot comparison: `scripts/compare-screenshot.sh` diffs two framebuffer dumps.
- Input event injection: simulate key presses and mouse clicks from kernel test code.
- Desktop smoke test: boot → verify desktop rendered (non-black framebuffer, taskbar present).
- Visual regression test: compare screenshots against known-good references.
- `make test-ui` runs desktop tests in QEMU headless with VNC or framebuffer dump.

---

## Implementation Order

| ⭐  | Order | Deliverable                                       | Depends On    | Status |
| --- | :---: | ------------------------------------------------- | ------------- | :----: |
| 💎  |   1   | Framebuffer snapshot API (kernel-side capture)      | —             |  [ ]   |
| 💎  |   2   | QEMU framebuffer dump (screendump via monitor)      | —             |  [ ]   |
| 💎  |   3   | Desktop smoke test (non-black screen after boot)    | §1 or §2      |  [ ]   |
| 💎  |   4   | Input event injection (key press, mouse click)      | —             |  [ ]   |
| 💎  |   5   | Terminal output verification                        | §4            |  [ ]   |
| ⭐  |   6   | Reference screenshot comparison                    | §2            |  [ ]   |
| ⭐  |   7   | Visual regression CI pipeline                       | §3, §6        |  [ ]   |
| ⭐  |   8   | Window manager state verification                   | §4            |  [ ]   |

> 💎 = parity — Windows has the Windows App Certification Kit (WACK) and UI Automation; Linux has dogtail and LDTP.
> ⭐ = exclusive — pixel-level visual regression in CI for an OS-level compositor.

---

## 1. Framebuffer Snapshot API

Capture the current framebuffer contents as a raw bitmap for comparison.

- [ ] `fb_snapshot(void *dest_buf, uint32_t *width, uint32_t *height)` — copies back buffer to caller buffer
- [ ] Output format: raw BGRA pixels (same as framebuffer format)
- [ ] Size: `width * height * 4` bytes (e.g., 1280×720 = 3.5 MiB)
- [ ] Can be called from kernel test code after compositor has rendered
- [ ] Commit: `"test: framebuffer snapshot API for UI verification"`

**Test checkpoint:** Call `fb_snapshot()` after boot complete → buffer is non-zero (not all black).

---

## 2. QEMU Framebuffer Dump

Use QEMU's monitor interface to capture screenshots from outside the VM.

- [ ] QEMU flag: `-monitor telnet:127.0.0.1:4444,server,nowait`
- [ ] Script: `scripts/qemu-screenshot.sh` connects to monitor, sends `screendump build/screenshot.ppm`
- [ ] Converts PPM to PNG: `convert build/screenshot.ppm build/screenshot.png` (ImageMagick)
- [ ] Alternative: use `-chardev file` to dump framebuffer directly
- [ ] Commit: `"test: QEMU screendump capture via monitor protocol"`

**Test checkpoint:** Boot QEMU → run script → `build/screenshot.png` shows the desktop.

---

## 3. Desktop Smoke Test

Verify the desktop actually rendered after boot — not a black screen or crash.

- [ ] After boot completes (serial shows `DESKTOP_READY`): capture screenshot
- [ ] Verify: at least 10% of pixels are non-black (desktop rendered)
- [ ] Verify: top-left region has taskbar colors (not wallpaper-only)
- [ ] Script: `scripts/test-desktop.sh` — boot QEMU, wait for DESKTOP_READY, screendump, analyze
- [ ] Pass: `"Desktop smoke test: PASS (rendered, %u%% non-black)"`
- [ ] Fail: `"Desktop smoke test: FAIL (screen is black or >90% single color)"`
- [ ] Commit: `"test: desktop smoke test — verify screen renders after boot"`

**Test checkpoint:** Normal boot → desktop smoke test passes. Break compositor → test catches black screen.

---

## 4. Input Event Injection

Simulate keyboard and mouse events from kernel test code or QEMU monitor.

- [ ] Kernel-side: `test_inject_keypress(uint8_t scancode)` — push scancode into keyboard buffer
- [ ] Kernel-side: `test_inject_mouse_click(int32_t x, int32_t y, uint8_t button)` — push mouse event
- [ ] QEMU-side: `sendkey` monitor command for keyboard, `mouse_move`/`mouse_button` for mouse
- [ ] Script wrapper: `scripts/qemu-input.sh sendkey ret` → sends Enter key via monitor
- [ ] Commit: `"test: input event injection — simulate keyboard and mouse from tests"`

**Test checkpoint:** Inject `dir` + Enter → terminal shows directory listing.

---

## 5. Terminal Output Verification

Verify the terminal window displays correct text after commands.

- [ ] After injecting keystrokes (§4): wait 500ms for rendering
- [ ] Capture screenshot → crop terminal window region
- [ ] Option A: OCR the terminal region (heavyweight, complex)
- [ ] Option B: kernel-side `terminal_get_buffer()` → read the terminal's character buffer directly
- [ ] Compare character buffer against expected output: `"C:\>"` prompt present, `dir` output matches files
- [ ] Commit: `"test: terminal output verification — inject command, verify response"`

**Test checkpoint:** Inject `dir` command → terminal buffer contains file listing from C:\.

---

## 6. Reference Screenshot Comparison

Compare current screenshots against known-good reference images.

- [ ] Store reference screenshots in `tests/references/` (PNG format)
- [ ] `scripts/compare-screenshot.sh reference.png current.png` → pixel diff
- [ ] Tolerance: allow up to 5% pixel difference (anti-aliasing, timing variations)
- [ ] Output: diff image highlighting changed pixels + percentage difference
- [ ] Pass: `"Visual match: 99.2% identical (threshold: 95%)"`
- [ ] Fail: `"Visual MISMATCH: 72% identical (expected 95%+)"` + upload diff image
- [ ] Commit: `"test: reference screenshot comparison with tolerance"`

**Test checkpoint:** Boot, capture screenshot, save as reference. Boot again → comparison passes. Change wallpaper → comparison shows diff.

---

## 7. Visual Regression CI Pipeline

Run desktop tests in CI and catch visual regressions.

- [ ] GitHub Actions step: boot QEMU headless with VNC or `-display none`
- [ ] After DESKTOP_READY: capture screenshot via monitor
- [ ] Compare against stored reference images
- [ ] Upload diff images as artifacts on failure
- [ ] Add to step summary: `### UI Tests: 3/3 visual checks passed ✅`
- [ ] Update reference images: `make update-ui-refs` captures new baselines
- [ ] Commit: `"ci: visual regression testing — screenshot comparison in GitHub Actions"`

**Test checkpoint:** PR that changes compositor → CI shows visual diff → reviewer sees what changed.

**Regression risk:** MEDIUM — reference images are environment-dependent (QEMU version, font rendering). Use generous tolerance (95%) and update refs when intentional changes land.

---

## 8. Window Manager State Verification

Verify WM state without screenshots — pure data inspection.

- [ ] `wm_get_window_count()` → number of open windows
- [ ] `wm_get_focused_window()` → handle of focused window
- [ ] `wm_get_window_rect(handle)` → x, y, width, height
- [ ] Test: after boot, verify: terminal window exists, gallery window exists, terminal is focused
- [ ] Test: inject Alt+F4 → window count decreases by 1
- [ ] Kernel test code: call WM introspection API directly
- [ ] Commit: `"test: WM state verification — window count, focus, rect inspection"`

**Test checkpoint:** After boot: 2 windows (terminal + gallery), terminal focused, both at expected positions.

---

## OS Comparison

| ⭐ | Feature                    | 🪟 Win11                   | 🐧 Linux                   | 🚀 Impossible OS            |
|----|----------------------------|-------------------------|-------------------------|--------------------------|
| 💎 | UI automation framework    | ✅ UI Automation + WACK | ⚠️ dogtail/LDTP         | ⬜ §4–§5                 |
| 💎 | Automated boot UI test     | ✅ Internal CI          | ⚠️ OpenQA (SUSE)        | ⬜ §3                    |
| ⭐ | Pixel-level visual CI      | ❌ Not in public CI     | ❌ Not standard          | ⬜ §6–§7 🚀              |
| ⭐ | WM state introspection     | ⚠️ spy++ (manual)      | ⚠️ xdotool              | ⬜ §8 🚀                 |

After §1–§5, Impossible OS has basic automated desktop testing (smoke test + input injection + terminal verification). §6–§8 add visual regression and WM introspection that neither Windows nor Linux provides in public CI.

---

## Verification

- [ ] `make test-ui` → desktop boots, screenshot captured, comparison passes.
- [ ] Break compositor (change background color) → visual regression caught in CI.
- [ ] Inject `dir` command → terminal shows correct output.
- [ ] WM state: correct window count and focus after boot.
- [ ] Commit: `"test: desktop UI test framework complete"`
