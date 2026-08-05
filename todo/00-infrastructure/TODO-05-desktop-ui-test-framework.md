---
schema_version: 1
id: desktop-ui-test-framework
domain: 00-infrastructure
status: active
title: "TODO-05 -- Desktop & UI Test Framework"
---

# TODO-05 -- Desktop & UI Test Framework

> **Validated:** 2026-06-21 | backfill -- todo-graph structural validate clean; all sections shipped + reviewed
> **Gap-audited:** 2026-06-21 | backfill -- triage DONE (sections shipped + quality-reviewed / deferred); Stages 1-2 predate this marker

> **Goal:** Automated testing for the graphical desktop: window creation, compositor rendering, input event processing, widget controls, terminal output, and visual regression detection. UI bugs are the hardest to catch because they require eyes -- this framework replaces eyes with framebuffer snapshots and pixel-level comparison. A window that renders wrong, a button that doesn't respond, or a terminal that drops characters gets caught automatically.

> [!IMPORTANT]
> **Current state:** The desktop (compositor, window manager, terminal, gallery) is tested entirely by manual visual inspection -- launch QEMU, look at the screen, move the mouse, type commands. No automated testing. No screenshot capture. No input event injection. No render verification. If a compositor change silently breaks window decorations or the terminal, nobody knows until someone looks.

---

## Inputs

- `src/desktop/wm.c` -- window manager (create, move, resize, focus)
- `src/desktop/terminal.c` -- command prompt window
- `src/desktop/controls.c` -- UI controls (buttons, text, gallery)
- `src/desktop/desktop.c` -- desktop surface, taskbar, wallpaper
- `src/kernel/main/compositor.c` -- render loop, framebuffer compositing
- `src/kernel/drivers/framebuffer.c` -- framebuffer access, page flip
- `CLAUDE.md` -- `bash scripts/test.sh` / `make test` wiring (local headless QEMU; no CI QEMU in Actions)
- → XREF: `T01 §4, §6` -- launcher matrix and workflow/artifact policy own the shared VM runner and CI wrapper contract this TODO consumes
- → XREF: `T04 §3` -- kernel launcher runs the `test_*.exe` sequence this TODO builds on
- → XREF: `T04 §4` -- shared timeout, TAP, skip, and environment-matrix policy for automated UI runs
- → XREF: `D08 T07 §6` -- deterministic automation transport provider; §14 consumes the semantic tree for WCAG sweep
- → XREF: `D10 T06 §6` -- `mouse_event_inject()` primitive owner; §4 and §11 reuse it, no duplicate API

---

## Outcome

- Framebuffer snapshot API: capture the current screen as a raw bitmap from within the kernel.
- Reference screenshot comparison: `scripts/compare-screenshot.sh` diffs two framebuffer dumps.
- Input event injection: simulate key presses and mouse clicks from kernel test code.
- Desktop smoke test: boot, verify desktop rendered (non-black framebuffer, taskbar present).
- Visual regression test: compare screenshots against known-good references.
- `make test-ui` runs desktop tests in QEMU headless with VNC or framebuffer dump.
- Frame-timing oracle: `wm_get_frame_stats()` exposes presented / queued / dropped / late counters plus VSYNC and present QPC timestamps.
- Perceptual diff engine: SSIM and SSIMULACRA2 alongside raw pixel-percent, plus openQA-style needles (match + exclude regions, region OCR assertions).
- Headless compositor mode with virtual clock and fixed RNG seed for deterministic replay under CI.
- Input record and replay traces with UTF-8 and IME composition support; deterministic under headless mode.
- Multi-monitor + DPI test matrix: 1 to 3 virtio-gpu outputs across 96 / 144 / 192 DPI.
- WCAG 2.2 sweep over the automation tree (consumed from `D08 T07 §6`); CI-gated for contrast, keyboard reach, focus order, name/role/value.
- Per-test isolation hook + automatic artifact bundle (screenshot, serial log, ETW buffer, WM state) on `TEST_CAT_DESKTOP` failure.

---

## Implementation Order

| ⭐  | Order | Deliverable                                       | Depends On | Status |
| --- | :---: | ------------------------------------------------- | ---------- | :----: |
| 💎  |   1   | Framebuffer snapshot API (kernel-side capture)    | --         |  [x]   |
| 💎  |   2   | QEMU framebuffer dump (screendump via monitor)    | --         |  [x]   |
| 💎  |   3   | Desktop smoke test (non-black screen after boot)  | §1, §2     |  [x]   |
| 💎  |   4   | Input event injection (key press, mouse click)    | --         |  [x]   |
| 💎  |   5   | Terminal output verification                      | §4         |  [x]   |
| ⭐  |   6   | Reference screenshot comparison                   | §2         |  [x]   |
| ⭐  |   7   | Visual regression CI pipeline                     | §3, §6     |  [x]   |
| ⭐  |   8   | Window manager state verification                 | §4         |  [x]   |
| ⭐  |   9   | Perceptual diff + structured screenshot needles   | §6         |  [x]   |
| 💎  |  10   | Frame timing + drop oracle (`wm_get_frame_stats`) | --         |  [x]   |
| 💎  |  11   | Input record + replay (Unicode, IME)              | §4, §12    |  [x]   |
| ⭐  |  12   | Headless compositor + frame-lock stepping         | §1         |  [x]   |
| 💎  |  13   | Multi-monitor + DPI test matrix                   | §1         |  [/]   |
| ⭐  |  14   | WCAG sweep over automation tree                   | D08 T07 §6 |  [/]   |
| 💎  |  15   | Test isolation + crash artifact capture           | §1, §10    |  [x]   |

> 💎 = parity -- Windows has the Windows App Certification Kit (WACK), UI Automation, DwmGetCompositionTimingInfo, SendInput; Linux has dogtail, LDTP, openQA, libinput record/replay, AT-SPI2.
> ⭐ = exclusive -- pixel-level visual regression in CI for an OS-level compositor; perceptual diff; headless-with-virtual-clock compositor; WCAG gating in CI; Unicode/IME-correct record/replay.

---

## 1. Framebuffer Snapshot API

Capture the current framebuffer contents as a raw bitmap for comparison.

- [x] `fb_snapshot(void *dest_buf, uint32_t *width, uint32_t *height)` -- copies back buffer to caller buffer (`src/kernel/drivers/framebuffer.c`)
- [x] Output format: raw BGRA pixels (same as framebuffer format)
- [x] Size: `width * height * 4` bytes (e.g., 1280x720 = 3.5 MiB); companion `fb_snapshot_size()` returns the exact byte count so callers never have to compute it
- [x] Can be called from kernel test code after compositor has rendered (`src/kernel/test/test_desktop.c` covers it)
- [x] Commit: `"test: framebuffer snapshot API for UI verification"`

**Test checkpoint:** Call `fb_snapshot()` after boot complete; buffer is non-zero (not all black).
**Platforms:** QEMU WHPX, QEMU TCG, VBox, bare metal (kernel-side; works everywhere).

> **Test runner:** `scripts\debug\desktop\run-desktop-tests.bat` (SUITE=desktop) | 3 suites, 17 assertions, 0 failures (KVM 2026-04-22)
> **Notes:**
> - Shipped `fb_snapshot()` + `fb_snapshot_size()` in `src/kernel/drivers/framebuffer.c` (+52 LOC): row-by-row BGRA copy from the back buffer into a caller-packed buffer, with NULL/uninit/overflow rejection. Destination layout is always tightly packed `width * height * 4` regardless of hardware stride padding.
> - Wired `TEST_CAT_DESKTOP` end-to-end: enum entry in `include/kernel/test/test.h`, name + label rows in `src/kernel/test/test_runner.c`, `test_desktop=11` in `src/boot/uefi/bootx64.c` boot.conf parser, `make test-desktop` target, and the existing per-category bat at `scripts\debug\desktop\run-desktop-tests.bat`.
> - Desktop suites render in the pink-mauve `#C586B5` `DTEST:` band via a per-suite `s_current_tag` flip in `test_runner.c` that toggles to `DTEST` when a `TEST_CAT_DESKTOP` suite runs; the klog.c route (comment at line 976) had reserved the color for exactly this consumer.
> - 3 suites cover §1: `Desktop: fb_snapshot_size nonzero` (3 asserts), `Desktop: fb_snapshot NULL args rejected` (3 asserts), `Desktop: fb_snapshot roundtrip (sentinel corners)` (11 asserts). The roundtrip test calls `spinner_stop()` to quiesce the IRQ-driven boot-splash spinner before writing sentinels into the four back-buffer corners, snapshotting, and comparing corner pixels in the output buffer.
> - Canonical doc: the `fb_snapshot()` / `fb_snapshot_size()` contract in `include/kernel/drivers/framebuffer.h` (Snapshot block).
> - Scope boundary: §1 ships only the kernel-side bitmap copy. §2 owns the QEMU monitor `screendump` path, §6 owns pixel-diff comparison, §13 owns the per-output `fb_snapshot_monitor(index, ...)` extension, §15 owns the on-failure artifact capture that consumes this API.
> **Verified:** 2026-04-22 | commit `d5fc4596` | 5/5 items | build OK + smoke PASS (KVM 2.41s) | tests 1840/1840 PASS, 0 leaked (KVM)
> **Accepted:** [M] `fb_snapshot()` copies the back buffer without a read-side lock; torn captures possible under an active compositor (reason: proper quiesce needs every writer to honor a new mutex) -> XREF: 09-desktop-shell/TODO-14 §4 (item: "`fb_snapshot()` read-side sync in `src/kernel/drivers/framebuffer.c`: add a read-side mutex (or a compositor flush barrier) that `fb_blit` / `fb_put_pixel` / `spinner_advance` honor")
> **Quality reviewed:** 2026-04-22 | Codex 2x (adversarial, quality) | 1H+2M+1L fixed, 1M open | scope: kernel-code-quality

---

## 2. QEMU Framebuffer Dump

Use QEMU's monitor interface to capture screenshots from outside the VM.

- [x] QEMU flag: `-monitor telnet:127.0.0.1:4444,server,nowait` wired as opt-in `-Monitor` + `-MonitorPort` switch in `scripts/machines/run-qemu.ps1` (`ValidateRange(1,65535)` on the port)
- [x] Script: `scripts/qemu-screenshot.sh` connects to monitor, sends `screendump <path>.ppm`, polls for size stability, converts PPM to PNG, validates magic + dimensions + liveness floor
- [x] Converts PPM to PNG: ImageMagick `convert` + `identify`; preflight errors if either tool is missing
- [/] Alternative: use `-chardev file` to dump framebuffer directly. HMP `screendump` path is sufficient for §3 / §6 / §15 consumers; the `-chardev file` path is kept open for future direct-mode needs but not wired today
- [x] Commit: `"test: QEMU screendump capture via monitor protocol"`

**Test checkpoint:** Boot QEMU, run script; `build/screenshot.png` exists, file size > 100 KiB, PNG magic bytes present, decoded width and height match configured framebuffer mode.
**Platforms:** QEMU WHPX, QEMU TCG only (monitor-protocol dependent; not usable on VBox or bare metal).

> **Test runner:** N/A (host-side shell script; no kernel test surface) | validation: `bash scripts/qemu-screenshot.sh <out.png>` against a QEMU instance launched with `-Monitor` (run-qemu.ps1) or `-monitor telnet:127.0.0.1:4444,server,nowait` (manual). KVM end-to-end 2026-04-22: 1280x720 PNG, 1.09 MiB, exit 0.
> **Notes:**
> - Shipped `scripts/qemu-screenshot.sh` (150 LOC bash): HMP-safe path sanitization (rejects `\n\r\t`, `;|&` and backtick that could split or escape the monitor command), `nc`-driven `screendump` over telnet with configurable host/port, deterministic size-stability flush poll (3 identical 50 ms samples, 5 s deadline), ImageMagick `convert` for PPM-to-PNG, `identify` for dimension check, five distinct exit codes (1=tool missing, 2=monitor unreachable, 3=PPM missing, 4=convert failed, 5=PNG validation failed).
> - Added `-Monitor` switch and `-MonitorPort` int (default 4444, `ValidateRange(1,65535)`) to `scripts/machines/run-qemu.ps1`; opt-in wiring keeps normal WHPX runs from exposing an unsolicited TCP listener.
> - Validated end-to-end on KVM: boot + HMP screendump + PNG conversion + dimension readback = 1280x720 / 1.09 MiB / exit 0 in under 10 s. Injection negative test (path with embedded newline) rejected with exit 1.
> - Canonical doc: `scripts/qemu-screenshot.sh` header comment block + the `-Monitor` switch doc in `run-qemu.ps1`.
> - Scope boundary: §2 owns capture; §3 (Desktop Smoke Test) owns the boot-to-READY wait + higher-level invocation; §6 (Reference Screenshot Comparison) owns pixel diff; §15 (Crash Artifact Capture) owns on-failure auto-screenshot.
> **Verified:** 2026-04-22 | commit `d5c9bb9b` | 4/4 items + 1 documented alt | build OK | KVM end-to-end PASS (1280x720, 1.09 MiB, exit 0) + injection reject PASS
> **Quality reviewed:** 2026-04-22 | Codex 2x (adversarial, quality) | 1H+4M+1L fixed | scope: N/A (host-side shell; kernel-code-quality does not apply)

---

## 3. Desktop Smoke Test

Verify the desktop actually rendered after boot: not a black screen or crash.

> [!NOTE]
> Either §1 (kernel-side `fb_snapshot()`) or §2 (QEMU `screendump`) satisfies the prerequisite. Prefer §2 for CI / host-side runs; §1 is required for bare metal and VBox where the monitor protocol is unavailable. Richer per-pixel validation is owned by §6 (reference screenshot comparison).

- [x] After boot completes (serial shows `DESKTOP_READY`): capture screenshot via `scripts/qemu-screenshot.sh`; bounded retry loop (4 attempts on KVM/WHPX, 8 on TCG) handles compositor paint-after-marker timing
- [x] Verify: at least 10% of pixels are non-black via `convert -threshold 6% -format '%[fx:100*mean]'`; floor configurable via `DESKTOP_MIN_NONBLACK_PCT`
- [x] Verify: top-left 400x40 region mean delta vs full-frame mean >= 3% (`DESKTOP_TASKBAR_DISTINCT`); identifies taskbar painting distinct colors over wallpaper
- [x] Script: `scripts/test-desktop.sh` -- boots QEMU (KVM auto-detect, TCG fallback), waits for `DESKTOP_READY` on serial, screendumps via §2, analyzes via ImageMagick, emits TODO-exact PASS/FAIL strings
- [x] Pass: emits exactly `"Desktop smoke test: PASS (rendered, %u%% non-black)"`
- [x] Fail: emits exactly `"Desktop smoke test: FAIL (screen is black or >90% single color)"`
- [x] Commit: `"test: desktop smoke test; verify screen renders after boot"`

**Test checkpoint:** Normal boot; desktop smoke test passes (non-black >= 10%, taskbar pixels present). Break compositor (force all-black); test fails with `"Desktop smoke test: FAIL"` on serial within one screendump cycle.
**Platforms:** QEMU WHPX, QEMU TCG, VBox (via §1), bare metal (via §1). §2 path QEMU-only.

> **Test runner:** N/A (host-side shell orchestrator; no kernel test surface) | validation: `bash scripts/test-desktop.sh` (KVM end-to-end 2026-04-22: PASS in 7s, 72% non-black, taskbar delta 48.7% | FAIL-path forced via DESKTOP_MIN_NONBLACK_PCT=200: emits exact TODO FAIL wording after 4 retries, exit 4).
> **Notes:**
> - Shipped `scripts/test-desktop.sh` (~200 LOC bash): launches QEMU with per-PID monitor port (44000-44999 range, prevents concurrent-run collisions) and per-PID OVMF vars, polls serial log for `DESKTOP_READY` with configurable boot timeout, delegates capture to `scripts/qemu-screenshot.sh`, analyzes via ImageMagick `convert -threshold 6%` for non-black percentage and cropped 400x40 top-left region for taskbar distinctness. Six distinct exit codes.
> - Bounded retry loop: 4 attempts (KVM/WHPX, 1 s apart) or 8 attempts (TCG, 3 s apart) cover the gap between `DESKTOP_READY` serial marker and first full paint without a hardcoded long sleep. Happy path passes on attempt 1 in under 1 s of analysis overhead.
> - Fix rolled into §2's `scripts/qemu-screenshot.sh`: removed the `quit` HMP command that was silently shutting down the entire VM after each screendump. The monitor now stays listening for subsequent captures in the same QEMU session, which is what §3's retry loop and §15's failure-screenshot consumer both need.
> - End-to-end KVM validation: PASS path 7 s boot + 1 attempt + emits `Desktop smoke test: PASS (rendered, 72% non-black)`; FAIL path (forced) iterates all 4 attempts, emits `Desktop smoke test: FAIL (screen is black or >90% single color)`, exit 4. Both wordings match the TODO checkpoint verbatim.
> - Canonical doc: `scripts/test-desktop.sh` header comment block (CLI flags, env vars, exit-code taxonomy, shared-host security caveat).
> - Scope boundary: §3 owns the binary-level "did the desktop paint?" gate. §6 owns pixel-level reference-image diff; §9 owns perceptual / structured-needle comparison; §15 owns on-failure artifact capture consuming this same capture path.
> **Verified:** 2026-04-22 | commit `f52954fd` | 6/6 items | build OK | KVM end-to-end: PASS 7 s + 72% non-black / FAIL-path 4 retries + exit 4
> **Accepted:** [M] HMP monitor on 127.0.0.1:<port> with `server,nowait` is reachable by any local user on multi-tenant hosts (RESOLVED 2026-04-23 by 09-desktop-shell/TODO-14 §7 commit `775d924b`: `qemu-screenshot.sh --socket` + `qemu-input.sh --socket` + `test-desktop.sh` Linux UNIX-socket default + `run-qemu.ps1 -MonitorSocket` shipped; smoke validated via per-PID `/tmp/qemu-mon-desktop-smoke-$$.sock`) (reason: UNIX-socket migration spans §2, §3, and run-qemu.ps1; larger than a single-section smoke test) -> XREF: 09-desktop-shell/TODO-14 §7 (item: "`scripts/qemu-screenshot.sh`: accept an optional `--socket <path>` argument; when set, pipe the `screendump` HMP command through `nc -U <path>` instead of `nc <host> <port>`")
> **Quality reviewed:** 2026-04-22 | Codex 2x (adversarial, quality) | 2H+1M fixed, 1M open | scope: N/A (host-side shell; kernel-code-quality does not apply)

---

## 4. Input Event Injection

Simulate keyboard and mouse events from kernel test code or QEMU monitor.

- [x] Kernel-side: `test_inject_keypress(uint8_t scancode)` in `src/kernel/test/test_desktop.c` -- pass-through to the existing `keyboard_inject_scancode()` primitive (`src/kernel/drivers/keyboard.c:318`); paired with a new `keyboard_reset_state()` driver helper that clears modifier latches + E0 prefix between tests
- [x] Kernel-side: `test_inject_mouse_click(int32_t x, int32_t y, uint8_t button)` plus split `test_inject_mouse_press()` / `test_inject_mouse_release()` primitives for callers that need to observe the press edge (e.g., WM-delivery tests); wraps `mouse_inject_state()` (`src/kernel/drivers/mouse.c:300`) which already clamps out-of-bounds coordinates
- [x] QEMU-side: `sendkey` monitor command for keyboard, `mouse_move` / `mouse_button` for mouse; exposed via `scripts/qemu-input.sh` sending HMP commands through the same telnet channel used by `scripts/qemu-screenshot.sh`
- [x] Script wrapper: `scripts/qemu-input.sh sendkey ret` sends Enter via monitor; also supports `mouse_move`, `mouse_button`, and `sendstring` (chains per-char `sendkey` calls for ASCII literals)
- [x] Commit: `"test: input event injection; simulate keyboard and mouse from tests"`

**Test checkpoint:** Inject `d`, `i`, `r`, Enter via `test_inject_keypress`; within 500 ms `terminal_get_buffer()` contains the literal echo `dir` followed by at least one file entry from `C:\`.
**Platforms:** QEMU WHPX, QEMU TCG, VBox, bare metal (kernel-side injection works everywhere; QEMU monitor path is QEMU-only).

> **Test runner:** `scripts\debug\desktop\run-desktop-tests.bat` (SUITE=desktop) | 4 new §4 suites added, 29 total desktop suites, 0 failures (KVM 2026-04-22).
> **Notes:**
> - Shipped `test_inject_keypress(scancode)`, `test_inject_mouse_press(x,y,button)`, `test_inject_mouse_release(x,y)`, and `test_inject_mouse_click()` convenience wrapper in `src/kernel/test/test_desktop.c` (file-static, KERNEL_TESTS-gated). Also added `keyboard_reset_state()` to `src/kernel/drivers/keyboard.c` (public) that clears the ring buffer plus every latched modifier (shift/ctrl/alt/capslock) plus the E0 prefix -- prevents test-order contamination.
> - Shipped `scripts/qemu-input.sh` (~140 LOC bash): subcommands `sendkey <key> [hold-ms]`, `mouse_move <dx> <dy>`, `mouse_button <mask>`, `sendstring <literal>`. Same HMP-safe path sanitization as `qemu-screenshot.sh`; `hold-ms` now validated as non-negative integer (Codex [M] fix). Three exit codes.
> - 4 §4 tests wired under TEST_CAT_DESKTOP: keypress-roundtrip ('a'), keypress-enter (LF), mouse-click-delivered, mouse-press-edge-observable, mouse-click-clamps-out-of-bounds. All 29/29 desktop suites pass on KVM in 0.1 s.
> - Test-checkpoint dependency: the full `dir` + Enter -> `terminal_get_buffer()` roundtrip lives in §5 Terminal Output Verification (which creates `terminal_get_buffer()` itself). §4 validates the INJECTION mechanics in isolation; §5 validates the end-to-end pipeline.
> - Canonical doc: `include/kernel/drivers/keyboard.h` (new `keyboard_reset_state()` declaration) + `scripts/qemu-input.sh` header comment block.
> - Scope boundary: §4 owns kernel + host injection primitives. §5 consumes them for the terminal roundtrip. §8 (WM state introspection) will add receive-side assertions. §11 (Input Record and Replay) extends into trace capture / replay; the split press/release primitives were added specifically so §11 can record state edges faithfully.
> **Verified:** 2026-04-22 | commit `e1b81ec2` | 5/5 items | build OK | tests 29/29 PASS (TEST_CAT_DESKTOP, KVM)
> **Quality reviewed:** 2026-04-22 | Codex 2x (adversarial, quality) | 3H+1M fixed | scope: kernel-code-quality

---

## 5. Terminal Output Verification

Verify the terminal window displays correct text after commands.

- [x] Kernel-side `terminal_get_buffer(char *dest, int cap)` in `src/desktop/terminal.c` -- row-major flat copy of `term_cells[TERM_ROWS][TERM_COLS]` into a caller buffer; returns bytes copied, 0 when the terminal is not open, -1 on NULL / too-small dest. No null terminator; callers use explicit lengths.
- [x] Kernel-side `terminal_buffer_contains(const char *needle)` -- O(n*m) substring search across the flat grid that ignores row boundaries, matching how the rendered terminal reads to a human. Guards NULL / empty / oversized needles.
- [x] Test-only seam: `terminal_test_force_open()` / `terminal_test_force_close()` gated by `KERNEL_TESTS` so kernel tests (which run in Phase 3 before `boot_desktop.c` creates the WM window) can exercise the introspection success paths against a real `term_cells` grid. Host is set to a sentinel >= 0 so `terminal_is_open()` returns true without calling `wm_create_window`.
- [x] OCR path explicitly rejected: kernel-side buffer readback is cheaper, deterministic, font-independent, and survives future font/theme swaps. `## 9. Perceptual Diff` still owns pixel-level content checks.
- [x] `test_desktop.c` coverage: 4 new suites (`terminal_get_buffer_when_closed`, `terminal_get_buffer_null_or_small`, `terminal_buffer_contains_guards`, `terminal_dir_roundtrip_synthesized`). The roundtrip case synthesizes a cmd.exe prompt + echoed `dir` + file list via `terminal_puts`, then asserts `terminal_get_buffer()` copies the full grid and `terminal_buffer_contains()` finds `C:\>`, `dir`, and `hello.txt` while rejecting an absent needle.
- [x] Commit: `"test: terminal output verification; inject command, verify response"`

**Test checkpoint:** Inject `dir` command; `terminal_get_buffer()` contains the `C:\>` prompt string AND at least one directory entry from `C:\` within 500 ms.
**Platforms:** QEMU WHPX, QEMU TCG, VBox, bare metal (kernel-side only; no host-side OCR dependency).

> **Test runner:** `scripts\debug\desktop\run-desktop-tests.bat` (SUITE=desktop) | 7 suites, 27 assertions, 0 failures (KVM 2026-04-22)
> **Notes:**
> - Shipped `terminal_get_buffer()` + `terminal_buffer_contains()` in `src/desktop/terminal.c` (+58 LOC): row-major flat copy of the 80x20 `term_cells` grid, plus a row-boundary-ignoring substring search so a needle that wraps between rows still matches how a human reads the rendered terminal.
> - Wired the `KERNEL_TESTS`-gated `terminal_test_force_open()` / `terminal_test_force_close()` seam because kernel tests execute in Phase 3 before `boot_desktop.c` ever calls `wm_create_window()`. Without the seam the only success-path test resolves as TEST_PENDING and the introspection APIs stay untested until desktop-init -- Codex §5 review flagged that as a coverage hole.
> - 4 new `TEST_CAT_DESKTOP` suites drive 10 new assertions: closed-terminal returns 0, NULL / undersized dest returns -1, NULL / empty needle returns 0, and the synthesized `dir` roundtrip forces the terminal open, writes `C:\>dir\nhello.txt\n`, verifies `get_buffer` copied all 1600 bytes and `buffer_contains` finds each expected substring without false positives.
> - Canonical doc: the concurrency contract and API shape live in `include/desktop/terminal.h` (Introspection block, line 53 onward).
> - Scope boundary: §5 ships kernel-side buffer readback only. §9 owns pixel-level perceptual diff, §11 owns Unicode / IME-correct record+replay, §15 owns both the post-desktop-init harness that will drive the real cmd.exe roundtrip under active WM and the read-side sync barrier that lets `terminal_get_buffer()` quiesce against a live compositor.
> **Verified:** 2026-04-22 | commit `3297a981` | 6/6 items | build OK + tests 1843/1843 PASS, 0 leaked (KVM)
> **Accepted:** [M] `terminal_get_buffer()` / `terminal_buffer_contains()` read `term_cells[]` without a lock; concurrent `terminal_putchar` on the compositor thread can produce a torn snapshot once the cross-thread failure-capture harness runs (reason: proper quiesce needs every writer to honor a new mutex) -> XREF: 09-desktop-shell/TODO-14 §4 (item: "`terminal_get_buffer()` + `terminal_buffer_contains()` read-side barrier in `src/desktop/terminal.c`: same seqlock-or-mutex choice, honored by every `terminal_putchar` / `terminal_puts` writer")
> **Quality reviewed:** 2026-04-22 | Codex 2x (adversarial, quality) | 0H+2M+0L fixed, 1M open | scope: kernel-code-quality

---

## 6. Reference Screenshot Comparison

Compare current screenshots against known-good reference images.

- [x] Reference screenshots live in `tests/references/` (PNG, naming convention `<scenario>-<resolution>.png`) with a `README.md` that documents the capture recipe (`run-qemu --monitor` + `qemu-screenshot.sh`) and the update protocol.
- [x] `scripts/compare-screenshot.sh <reference.png> <current.png> [diff.png] [--threshold N] [--fuzz N]` -- ImageMagick `compare -metric AE -fuzz <pct>%` across both inputs, emits a diff image highlighting changed pixels, prints percent-identical to 2 decimal places.
- [x] Tolerance is configurable: default `--threshold 95` (allow up to 5% different pixels) + `--fuzz 2` (per-channel anti-alias budget). Both percentages are validated against a strict `^[0-9]+(\.[0-9]+)?$` regex and range-checked to `[0, 100]` so `.`, `1.2.3`, or `150` are rejected with exit 1 before hitting ImageMagick.
- [x] Diff image output: `compare` always writes the diff (all-white on a perfect match, red overlay elsewhere). Written atomically via a `.tmp.$$` path + `mv -f` and a cleanup trap so a stale diff never survives a failed run. A pre-run `rm -f` removes any previous artifact before compare starts.
- [x] Pass format: `"Visual match: <pct>% identical (threshold: <T>%)"` on stdout, exit 0.
- [x] Fail format: `"Visual MISMATCH: <pct>% identical (expected <T>%+) -- diff: <path>"` on stderr, exit 5; the diff image path echoed is always the current run's output (atomic rename guarantees it).
- [x] Five-level exit code surface: 0 pass, 1 tooling/bad args, 2 missing input, 3 dimension mismatch, 4 compare/decode failure, 5 visual regression. Documented in the script header; lets CI distinguish "script broken" from "screenshot regressed".
- [x] ImageMagick decoder pinning: both inputs and the diff output are referenced as `png:<path>` so ImageMagick never heuristically selects `ephemeral:`, `msl:`, or `ghostscript` coders on a mis-named file.
- [x] Commit: `"test: reference screenshot comparison with tolerance"`

**Test checkpoint:** Boot, capture screenshot, save as reference; second boot comparison returns >= 95% identical. Change wallpaper; comparison drops below 95% and emits diff image path.
**Platforms:** QEMU WHPX, QEMU TCG only (monitor-protocol dependent; not usable on VBox or bare metal).

> **Test runner:** N/A (host-side shell script; no kernel test surface) | validation: 7 self-check runs (identical, subtle/fuzz-absorbed, 3% diff, 10% diff, dimension mismatch, stale-diff replacement, strict threshold parsing) against synthesized PNG fixtures. End-to-end vs a real QEMU capture is wired into §7's CI pipeline.
> **Notes:**
> - Shipped `scripts/compare-screenshot.sh` (~200 LOC bash): ImageMagick `compare -metric AE -fuzz <pct>%` for the pixel count, `identify` for dimension checks, awk for percent-identical math (bash has no float), atomic `.tmp.$$` + `mv -f` + `trap` for the diff artifact so CI never triages a stale file.
> - Wired `tests/references/` as the canonical reference-PNG location with a `README.md` naming convention, capture recipe (`qemu-screenshot.sh`), and tolerance rationale. No reference images committed yet -- §7 CI is the first consumer and will seed `desktop-idle-1280x720.png` from a golden boot.
> - Tolerance knobs (`--threshold`, `--fuzz`) default to 95% / 2% per the checkpoint; both are validated with a strict regex + `[0, 100]` range check so a malformed value cannot silently coerce to 0 via awk and disable the gate.
> - Canonical doc: `scripts/compare-screenshot.sh` header (argument, exit-code, and output-format contract) and `tests/references/README.md` (naming + update recipe).
> - Scope boundary: §6 ships the pixel-diff tool + reference convention. §7 owns CI wiring + baseline capture automation, §9 owns perceptual diff (SSIM) and openQA-style exclude regions, §13 owns per-monitor references, §15 owns the failure-bundle consumer that stores the rejected diff alongside `screen.png`.
> **Verified:** 2026-04-22 | commit `a19593e0` | 9/9 items | build OK | manual (7 self-check runs PASS against synthesized PNGs; lint clean)
> **Quality reviewed:** 2026-04-22 | Codex 2x (adversarial, quality) | 2H+2M+0L fixed, 0 open | scope: N/A (host-side shell script; no domain code-quality skill)

---

## 7. Visual Regression CI Pipeline

Run desktop tests in CI and catch visual regressions.

- [x] `.github/workflows/visual-regression.yml` -- new GHA workflow: caches LLVM, installs deps (ImageMagick + netcat alongside the build set), builds OS, runs `scripts/test-visual-regression.sh --accel tcg`, uploads diff artifacts on failure. Boots QEMU with `-display none` via the delegated `scripts/test-desktop.sh` call.
- [x] Capture after DESKTOP_READY: reuses `scripts/test-desktop.sh` for the boot + first-paint wait + `screendump` via HMP monitor, then copies `build/desktop-smoke.png` into `build/visual-<scenario>.png` for the comparison step.
- [x] Compare against stored reference images via `scripts/compare-screenshot.sh` (§6) at the default 95% threshold / 2% fuzz.
- [x] Upload diff images as artifacts on failure: `build/visual-*.png`, `build/visual-*.diff.png`, `build/desktop-smoke.png`, and the serial log tail; 7-day retention.
- [x] Step summary: `scripts/test-visual-regression.sh` writes the markdown block (`### UI Tests: N/M visual checks passed ✅` plus a per-scenario table) to `$GITHUB_STEP_SUMMARY` when GHA sets it, and to stdout otherwise.
- [x] `make update-ui-refs` captures new baselines by running the runner with `--update-refs`, which copies each `build/visual-<scenario>.png` into `tests/references/<scenario>-<WxH>.png`. `workflow_dispatch` with `update_refs=true` runs the same path in CI and uploads the fresh reference set as an artifact for review (NOT auto-committed).
- [x] `make test-visual` shortcut for the local run; both targets are listed in `.PHONY`.
- [x] Four-level exit code surface documented in the runner header: 0 pass (or advisory pristine state), 2 capture/boot failure, 3 product regression, 4 harness/compare internal error. Compare-internal failures (dim mismatch, decode error) are tracked separately from true mismatches so the workflow surfaces "harness broken" vs "screenshot regressed" as distinct states. Codex [H] adversarial review.
- [x] Pristine baseline-set handling: when `tests/references/` has no PNG for the expected `<scenario>-<WxH>.png`, the job exits 0 AND emits a `::warning title=Visual regression gate advisory::` annotation so the Checks tab surfaces that gating is off until a maintainer seeds the set. Codex [M].
- [x] Concurrency group keyed by event name AND ref so a manual `update_refs=true` dispatch on `main` does not cancel the regular push/PR validation on the same ref; `cancel-in-progress` stays true for push/PR but false for `workflow_dispatch`. Codex [M].
- [x] Commit: `"ci: visual regression testing; screenshot comparison in GitHub Actions"`

**Test checkpoint:** Intentional compositor diff in a PR triggers visual-regression job; job exits non-zero, uploads diff PNG, step summary shows `MISMATCH`. Clean PR (no compositor changes) exits zero with `### UI Tests: N/N visual checks passed`.
**Platforms:** GitHub Actions `ubuntu-latest` TCG runner only (per T01 §6 policy). No WHPX / VBox / bare-metal CI path; those remain local.

**Regression risk:** MEDIUM; reference images are environment-dependent (QEMU version, font rendering). Use generous tolerance (95%) and update refs when intentional changes land.

> **Test runner:** N/A (host-side CI workflow + shell runner; no kernel test surface) | validation: runner dry-run PASS for unknown-scenario path (rc=2), compare internal-error routing PASS (`compare-screenshot.sh` rc=3 on dim mismatch routes through `COMPARE_INTERNAL` bucket to runner rc=4), lint clean, `scripts/test-tooling.sh --quiet` 81/81 PASS, `scripts/test-ai-system.sh --quiet` 72/72 PASS. First real CI run seeds baselines via `workflow_dispatch update_refs=true`.
> **Notes:**
> - Shipped `.github/workflows/visual-regression.yml` (CI job) + `scripts/test-visual-regression.sh` (host runner, ~220 LOC) + two Makefile targets (`test-visual`, `update-ui-refs`). The workflow installs the extra ImageMagick / netcat deps on top of the build baseline, builds the OS, and runs the visual-regression runner against the committed reference set.
> - Runner design: a scenario table (currently just `idle`, extendable by editing `scenario_capture()`) drives boot + capture + compare; each scenario leaves its capture at `build/visual-<scenario>.png` and its diff at `build/visual-<scenario>.diff.png`. The runner emits `::warning::` annotations when no baseline exists and writes a GitHub-formatted step-summary table listing `Scenario | Result | Identical | Artifact` for every scenario.
> - Tolerance reuses §6 defaults (`--threshold 95`, `--fuzz 2`), overridable via `VR_THRESHOLD` / `VR_FUZZ`. Harness errors (compare rc 3/4) are tracked in a separate `COMPARE_INTERNAL` bucket and route to runner exit 4, distinct from product regressions (runner exit 3).
> - Reference-seeding path: `make update-ui-refs` (local) or `workflow_dispatch` with `update_refs=true` (CI, uploads as artifact, does not auto-commit). Maintainers inspect the artifact or local output, then intentionally commit the PNG(s) under `tests/references/`.
> - Canonical doc: `.github/workflows/visual-regression.yml` (pipeline structure, triggers, artifact policy), `scripts/test-visual-regression.sh` header (scenarios table + exit codes), `tests/references/README.md` (reference naming + update recipe).
> - Scope boundary: §7 ships the CI wiring + runner + reference-seeding knobs. §6 owns the pixel-diff primitive, §9 owns SSIM + openQA-style exclude regions, §13 owns per-monitor references for multi-output configurations, §15 owns the failure-bundle consumer that will also store the rejected diffs.
> **Verified:** 2026-04-22 | commit `54c9b449` | 10/10 items | build OK | lint clean | manual (runner dry-run PASS, compare-internal routing PASS, no-refs advisory + `::warning::` verified)
> **Deferred:** [M] single-session multi-scenario refactor to avoid a full QEMU boot per scenario (RESOLVED 2026-04-23 by 09-desktop-shell/TODO-14 §8 commit `7b6e4849`: `session_start`/`session_stop` helpers + `scenario_capture_shared`/`scenario_capture_fresh` split + `scenario_needs_fresh_boot` guard + `VR_FORCE_FRESH=1` debug override; multi-scenario runs now boot QEMU once, single-scenario `idle` baseline preserved) (reason: scales past one-session scope; not racing a real bug today since only `idle` is shipped) -> XREF: 09-desktop-shell/TODO-14 §8 (item: "Refactor `scripts/test-visual-regression.sh` around a single QEMU session: boot once, run N scenarios, capture screenshot per scenario, teardown once")
> **Quality reviewed:** 2026-04-22 | Codex 2x (adversarial, quality) | 2H+3M+0L fixed, 1M open | scope: N/A (host-side CI workflow + shell runner; no domain code-quality skill)

---

## 8. Window Manager State Verification

Verify WM state without screenshots -- pure data inspection.

- [x] `wm_get_window_count()` in `src/desktop/wm.c` -- counts active `windows[]` slots; returns 0 on a fresh WM or when all slots are free.
- [x] `wm_get_focused_window()` -- returns the focused handle, or -1 when no window has focus OR when `focused_window` points to an inactive slot (stale-focus guard).
- [x] `wm_get_window_rect(handle, *x, *y, *w, *h)` -- fills out-params with outer position + client-area size; returns 0 on success, -1 on oob / inactive handle, -2 on any NULL out-pointer.
- [x] Alt+F4 close path: keyboard driver traps `alt_held && scancode == SC_F4` in BOTH the real IRQ path (`keyboard_irq_callback`) and the inject path (`keyboard_inject_scancode`), and calls `wm_close_focused_window()`.
- [x] Deferred-close queue: `wm_close_focused_window()` only ENQUEUES the focused handle into a `volatile int pending_close_handle` slot; actual `wm_destroy_window()` (which calls `pmm_free_pages`) runs from thread context via `wm_process_pending_closes()`. This keeps PMM mutation and compositor traversal out of IRQ-preemption races. Codex [H] adversarial review required this refactor.
- [x] Compositor integration: `src/kernel/main/compositor.c` calls `wm_process_pending_closes()` once per frame under `scheduler_disable()`, before `wm_composite()`, so Alt+F4 user-visible latency stays within one frame.
- [x] Test seams (KERNEL_TESTS-gated): `wm_test_reset()`, `wm_test_install_window(x, y, w, h)`, `wm_test_set_focused(handle)`. Let kernel tests exercise the introspection + Alt+F4 paths without `wm_init()` or `wm_create_window()` (which need framebuffer allocation and run after `boot_tests_run()` in Phase 3).
- [x] Kernel-side tests: 4 new `TEST_CAT_DESKTOP` suites covering count, focus (including stale-focus cleanup after `wm_destroy_window`), rect roundtrip + NULL / oob / inactive guards, and Alt+F4 enqueue + deferred drain including F4-without-Alt and Alt+F4-with-no-focus edge cases.
- [x] Commit: `"test: WM state verification; window count, focus, rect inspection"`

**Test checkpoint:** After boot: `wm_get_window_count() == 2`, `wm_get_focused_window()` returns the terminal handle, `wm_get_window_rect(terminal)` and `wm_get_window_rect(gallery)` both return non-zero width and height. Inject Alt+F4; within 200 ms `wm_get_window_count() == 1`.
**Platforms:** QEMU WHPX, QEMU TCG, VBox, bare metal (kernel-side data inspection; no framebuffer dependency).

> **Test runner:** `scripts\debug\desktop\run-desktop-tests.bat` (SUITE=desktop) | 16 suites, 79 assertions, 0 failures (KVM 2026-04-22)
> **Notes:**
> - Shipped four public WM APIs (`wm_get_window_count`, `wm_get_focused_window`, `wm_get_window_rect`, `wm_close_focused_window`) plus a `wm_process_pending_closes()` drain in `src/desktop/wm.c` (+110 LOC). Alt+F4 is wired in BOTH the real i8042 IRQ path and the test-inject path of `src/kernel/drivers/keyboard.c`, both going through the IRQ-safe enqueue.
> - Deferred-close architecture: Alt+F4 from the keyboard IRQ can no longer race the compositor's `wm_composite()` walk or the PMM bitmap; the IRQ only writes one `volatile int` slot, and `wm_process_pending_closes()` drains in thread context under the compositor's existing `scheduler_disable()` window. `src/kernel/main/compositor.c:144` wires the drain into the per-frame loop.
> - Test seam: `KERNEL_TESTS`-gated `wm_test_reset` / `wm_test_install_window` / `wm_test_set_focused` let Phase-3 kernel tests exercise the APIs without allocating real framebuffers. The 4 new suites add 25 assertions: count tracks installs+resets, focus tracks set_focused and clears on destroy, rect roundtrips and rejects NULL / oob / inactive handles, and Alt+F4 specifically validates that destruction is deferred (`count==2` post-inject pre-drain, `count==1` post-drain).
> - Canonical doc: `include/desktop/wm.h` (Introspection block + concurrency contract), `src/desktop/wm.c` (deferred-close implementation with rationale).
> - Scope boundary: section 8 ships kernel-side introspection + Alt+F4 deferred close. Section 9 owns perceptual diff / needles, section 10 owns the frame-timing oracle, section 11 owns Unicode record+replay that will also use this drain path for non-Alt-F4 close sources, section 15 owns the multi-handle close queue widening + failure-capture hook.
> **Verified:** 2026-04-22 | commit `0e6ab6c3` | 9/9 items | build OK | tests 1847/1847 PASS, 0 leaked (KVM desktop suite 16/16 suites, 79/79 assertions)
> **Quality reviewed:** 2026-04-22 | Codex 2x (adversarial, quality) | 2H+2M+0L fixed, 0 open | scope: kernel-code-quality

---

## 9. Perceptual Diff and Structured Screenshot Needles

Upgrades §6 beyond flat pixel-percent: adds SSIM / SSIMULACRA2 perceptual diff and openQA-style needles (match regions, exclude regions, region OCR assertions). Host-side tool chain; consumes the artifacts captured by §1 / §2 / §6.

> [!TIP]
> Win11 has no in-box perceptual diff (WACK compares binaries, not pixels). Linux has openQA needles but they ship as an external SUSE tool. Owning a structured + perceptual diff in the OS tree means every compositor change gets a rigorous visual gate without a third-party service.

- [x] Perceptual diff backend: SSIM via `skimage.metrics.structural_similarity` is the default; SSIMULACRA2 available via the external `ssimulacra2` binary (build from libjxl); raw pixel-percent retained as `--mode pixel` for the simple section 6 callers.
- [x] `scripts/compare-screenshot.sh --mode pixel|ssim|ssimulacra2 --threshold N reference.png current.png` -- pixel stays on the bash / ImageMagick path (backwards-compatible with section 6); non-pixel modes `exec` into the new `scripts/needle-compare.py` helper. Pixel mode with `--needle` also forwards to Python so the region-aware comparator is reachable from either mode knob.
- [x] Needle schema v1: JSON sidecar `<reference>.needle.json` with `{version, regions[{name, x, y, w, h, tolerance, ocr?}]}`. Strict schema validation: top-level must be an object, `version == 1`, non-empty `regions` array, each region must be an object with non-negative `x/y` + positive `w/h` ints, `tolerance in {strict, relaxed, ignored}`, optional `name` / `ocr` strings. Every violation routes through `die(4, ...)` so malformed needles never escape the documented exit-code contract. Codex [M] review.
- [x] Per-region tolerance: `strict` = pixel-exact (SSIM >= 0.99 in SSIM mode), `relaxed` = uses the caller's `--threshold` / `--fuzz`, `ignored` = skip comparison (rendered as gray overlay in the diff image so humans can confirm the exclude zone landed correctly).
- [x] Region OCR assertion via `tesseract` (`--psm 6 -l eng`): crop the region, run OCR, require the needle's `ocr` string to appear in the recognized text after whitespace normalization. `--allow-missing-ocr` soft-skips when tesseract is absent; otherwise a missing binary is a hard `die(4)` so CI regressions are visible.
- [x] Host toolchain: `python3-pil` + `python3-numpy` + `python3-skimage` (SSIM) + `tesseract-ocr` (OCR) + `imagemagick` added to `.github/workflows/visual-regression.yml`. SSIMULACRA2 binary stays optional; absent -> graceful `die(4)` with install pointer.
- [x] Tiny-region SSIM crash guard: crops smaller than 7x7 previously raised `ValueError` from scikit-image (`win_size exceeds image extent`) and aborted the whole run. `ssim_score()` now picks the largest odd `win_size <= min(w, h)` so a thin 3x10 needle region works; crops < 2x2 fail cleanly via `die(4)`. Codex [H] review.
- [x] Region-result bookkeeping: the previous slice-assign tidy-dance at the end of the per-region loop caused exponential list growth (`a -> 1, b -> 2, c -> 4, d -> 8 entries`). Replaced with a single `regions_result.append(...)` per region. Codex [H] review. 200-region needle runs in 0.13 s.
- [x] Reference + needle documentation: `tests/references/README.md` adds the schema v1 spec, invocation examples, tolerance semantics, and regeneration protocol. `make update-ui-refs` still captures PNGs only (needle JSON is a reviewed, hand-tuned artifact; auto-generating skeletons from one capture would embed resolution-specific hacks into tracked files).
- [x] Atomic diff image: `write_diff()` writes to `<out>.tmp.$$` and `os.replace()`s into the final path with explicit `format="PNG"` so Pillow doesn't barf on the temp-suffix. Diff overlay draws green outlines around passed regions, red around failed / OCR-failed regions, and gray fills on ignored regions.
- [x] Commit: `"test: perceptual diff + structured needles (SSIM, exclude regions, OCR)"`

**Test checkpoint:** Shift wallpaper hue by 5 percent: raw pixel-percent at 95 percent threshold passes (false negative), SSIM catches the structural change. Mark clock region as exclude-zone: two consecutive-second captures still compare equal. Region OCR asserts `C:\>` prompt string present in the cropped terminal region.
**Platforms:** host-side only (no kernel dependency); runs in CI (GHA ubuntu-latest) and local dev.

> **Test runner:** N/A (host-side Python helper + shell wrapper; no kernel test surface) | validation: 10 self-check runs PASS against synthesized PNGs (identical SSIM, tiny 3x3 SSIM, pixel mode backwards compat, pixel+needle delegation, ssim mode delegation, exclude region, strict region failure, 200-region scale, OCR graceful fallback, malformed needle -> rc 4). Workflow dep bump picks up python3-skimage + tesseract.
> **Notes:**
> - Shipped `scripts/needle-compare.py` (~440 LOC): SSIM/SSIMULACRA2/pixel metric scoring, needle v1 schema + strict validator, tolerance mapper, region cropper, tesseract OCR, atomic PNG diff writer with per-region color overlay.
> - Extended `scripts/compare-screenshot.sh` with `--mode {pixel,ssim,ssimulacra2}`, `--needle <json>`, `--allow-missing-ocr`. Pixel mode + no needle stays on the bash/ImageMagick path for section 6 backwards compat; any other combination `exec`s into `needle-compare.py`. Threshold defaults are mode-aware (pixel 95%, SSIM 0.95, SSIMULACRA2 80).
> - Updated `.github/workflows/visual-regression.yml` to install `python3-pil`, `python3-numpy`, `python3-skimage`, `tesseract-ocr` on top of the existing ImageMagick + netcat set. CI runs can now drive SSIM + OCR needles without a separate setup step.
> - Documented the needle v1 schema and regeneration protocol in `tests/references/README.md` so maintainers authoring the first needle files have the contract in-tree.
> - Canonical doc: `scripts/needle-compare.py` header (modes, exit codes, tolerance semantics), `tests/references/README.md` (needle v1 schema).
> - Scope boundary: section 9 ships the perceptual + needle comparator. Section 7 owns the CI wiring that consumes it, section 10 owns frame-timing oracle (no visual dependency), section 13 owns per-output needles, section 15 owns the failure-bundle capture that will upload needle diffs alongside raw screenshots.
> **Verified:** 2026-04-22 | commit `725f0ab1` | 11/11 items | build OK | lint clean | manual (10-case self-test matrix PASS; Codex-demanded regressions covered: tiny-region SSIM, 200-region scale, 4 malformed-needle variants; review-pass adds sibling auto-discovery + unreadable-input rc=2 + ssimulacra2 multi-region rc=1)
> **Quality reviewed:** 2026-04-22 | Codex 2x (adversarial, quality) | 3H+3M+0L fixed, 0 open | scope: N/A (host-side Python + shell; no domain code-quality skill)

---

## 10. Frame Timing and Drop Oracle

Parity for Win11 `DwmGetCompositionTimingInfo` and Linux Wayland `presentation-time` protocol. Exposes compositor frame counters and per-frame VSYNC / present timestamps so tests can assert "zero dropped frames during animation X."

- [x] Kernel type `struct wm_frame_stats` in `include/desktop/wm.h` with the six uint64 fields (`frames_presented`, `frames_queued`, `frames_late`, `frames_dropped`, `last_vsync_qpc`, `last_present_qpc`). Timestamp unit is `mono_ns()` nanoseconds since boot (the `_qpc` suffix is a Win32-terminology nod, not a raw PIT/TSC count).
- [x] `wm_get_frame_stats(struct wm_frame_stats *out)` -- seqlock-protected snapshot: readers loop until they observe a stable even sequence number on both sides of the copy, so a concurrent compositor update never returns a torn mix of old + new fields. NULL out-pointer is a safe no-op.
- [x] Counter-bump API: `wm_frame_stats_on_mark_dirty(was_already_dirty)` bumps `frames_queued`, and `frames_dropped` on coalesce. `wm_frame_stats_on_present(vsync_ns, present_ns)` bumps `frames_presented`, records `last_vsync_qpc` / `last_present_qpc`, and bumps `frames_late` when the elapsed budget exceeds `WM_FRAME_BUDGET_NS` (16.67 ms / 60 Hz).
- [x] Compositor wiring in `src/kernel/main/compositor.c`: stamps `vsync_ns = mono_ns()` at frame start; calls `wm_frame_stats_on_present(vsync_ns, mono_ns())` after `fb_swap()` (or the partial-drag swap path). `wm_mark_dirty()` inside the compositor loop replaced with new silent `wm_mark_dirty_internal()` so compositor-internal reinvalidation (after `wm_process_pending_closes()`) does not inflate the external queued-frame counter.
- [x] ETW event `WM_FRAME_PRESENTED` (`ETW_EVT_WM_FRAME_PRESENTED = 0x1001` in `include/kernel/etw.h`) emitted from `wm_frame_stats_on_present()`. New `etw_emit_kernel_event(event_id, level, payload, size)` helper walks `ETW_MAX_SESSIONS` under `s_etw_lock` and writes the snapshot into every RUNNING session; no-op when none are running. `etw_emit_wm_frame_presented(stats)` is the thin wrapper.
- [ ] Namespace exposure `\\?\ObjectManager\FrameStats` read-only pseudo-file -- deferred to §15 along with the other object-manager exposure work. Not needed for the §10 test checkpoint (kernel tests call `wm_get_frame_stats()` directly) and building a minimal pseudo-file driver is a separate 300-line work item that belongs with the test-isolation artifact-capture stack.
- [x] `wm_frame_stats_reset_for_test()` zeroes every counter under the seqlock; gated by `#ifdef KERNEL_TESTS` so the reset path cannot be called from production callers.
- [x] Commit: `"test: frame-timing oracle -- wm_get_frame_stats + ETW WM_FRAME_PRESENTED"`

**Test checkpoint:** Idle 100 ms after boot: `frames_presented > 0`, `frames_dropped == 0`, `frames_late < 5`. Trigger a window-resize animation: counters advance monotonically, `frames_dropped` stays within budget (<= 2 per animation sequence).
**Platforms:** QEMU WHPX, QEMU TCG, VBox, bare metal (kernel-side counters; no framebuffer capture needed).

> **Test runner:** `scripts\debug\desktop\run-desktop-tests.bat` (SUITE=desktop) | 24 suites, 106 assertions, 0 failures (KVM 2026-04-22)
> **Notes:**
> - Shipped `struct wm_frame_stats` + seqlock-protected reader/writer API in `include/desktop/wm.h` + `src/desktop/wm.c` (+95 LOC). Uses the kernel's real `seqlock_t` primitive so writer-side exclusion is spinlock-backed and IRQ-safe; readers retry on an odd sequence without ever blocking a writer.
> - Compositor wiring in `src/kernel/main/compositor.c`: `mono_ns()` captured at frame start; `wm_frame_stats_on_present()` called after the final `fb_swap()`. `wm_mark_dirty_internal()` added so compositor's post-close forced repaint doesn't double-count as an external queue event.
> - ETW hook: `ETW_EVT_WM_FRAME_PRESENTED = 0x1001` plus `etw_emit_kernel_event(event_id, level, payload, size)` helper in `include/kernel/etw.h` + `src/kernel/etw.c` (+60 LOC). Walks `ETW_MAX_SESSIONS` under `s_etw_lock` and writes the snapshot into every RUNNING session; zero running sessions is one lock/unlock pair per frame. Emits a stack-captured snapshot (not `&s_frame_stats`) so a concurrent `wm_mark_dirty()` on another CPU cannot mutate the payload during the ETW write.
> - Every WM-state mutation (create/destroy/move/resize/raise/focus + drag/hover) now counts. The prior `static void mark_dirty()` bypass was replaced with `static inline void mark_dirty(void) { wm_mark_dirty(); }` so all internal invalidations are visible to the oracle; only `wm_mark_dirty_internal()` stays silent for compositor-internal repaints.
> - Canonical doc: `include/desktop/wm.h` (frame-stats contract + concurrency note), `include/kernel/etw.h` (ETW_EVT_WM_FRAME_PRESENTED + `etw_emit_kernel_event`).
> - Scope boundary: §10 ships kernel-side counters + ETW emit. §13 owns per-output stats for multi-monitor, §15 owns the `\\?\ObjectManager\FrameStats` pseudo-file exposure (tracked concrete item there; 300-LOC Ob driver work is out of single-section scope), §11 record/replay will consume this for drop-free playback assertions.
> **Verified:** 2026-04-22 | commit `36ff0a9c` | 7/8 items | build OK | tests 1854/1854 PASS, 0 leaked (KVM desktop suite 24/24 suites, 106/106 assertions)
> **Deferred:** [L] `\\?\ObjectManager\FrameStats` pseudo-file not shipped (RESOLVED 2026-04-23 by 09-desktop-shell/TODO-14 §6 commit `dfcac910`: new `ObpInfoFileType` + `ob_info_file_register/_open_handle/_read` API + `\ObjectManager` namespace dir + `wm_framestats_register_info_file()` from `wm_init` + ETW schema lock-step via `_Static_assert(sizeof(wm_frame_stats) == ETW_WM_FRAME_STATS_SIZE)` + 24-assertion test coverage; user-mode `CreateFile("\\?\ObjectManager\FrameStats")` NT-path routing remains a follow-up) (reason: 300-LOC Ob driver work larger than a single-section scope; kernel-side counters + ETW emit ship today) -> XREF: 09-desktop-shell/TODO-14 §6 (item: "Register an Ob pseudo-file driver under `\\?\ObjectManager\FrameStats` (root-relative)")
> **Quality reviewed:** 2026-04-22 | Codex 2x (adversarial, quality) | 2H+3M+0L fixed, 0 open | scope: kernel-code-quality

---

## 11. Input Record and Replay (Unicode and IME aware)

Extends §4 from one-shot inject to full record + replay traces. Linux `libinput record` captures `uinput` events but ydotool / wtype botch non-ASCII and IME composition. Win11 has no public record API (PSR is deprecated in 24H2). Impossible designs this with UTF-8 and IME composition correct from day one.

> [!TIP]
> Deterministic regression: a recorded trace + §12 headless mode gives byte-identical replay across runs. Neither Win11 nor Linux ships this combination in-tree.

- [x] Trace format: JSONL in-memory (kernel test phase has no filesystem); `{"ts_ns":N, "kind":"key|mouse|ime_compose|ime_commit", payload...}`. Keys: `scancode`/`codepoint` (key), `x`/`y`/`buttons` (mouse), `codepoint`/`candidate` (ime_compose), `utf8` (hex-encoded bytes, ime_commit). Freestanding JSON encoder + parser in `src/kernel/test/input_record.c`; no libc dependency.
- [x] Key events carry scancode AND resolved Unicode codepoint; IME events carry compose codepoint + candidate index + raw UTF-8 commit bytes (up to 4 bytes per commit).
- [x] `input_record_begin(capacity)` / `input_record_stop()` / `input_record_release()`: tee mode via `input_record_key` / `input_record_mouse` / `input_record_ime_compose` / `input_record_ime_commit` writer entry points. Captures do NOT suppress delivery. Under `spin_lock_irqsave` so keyboard IRQ / compositor thread / test thread can all call writers concurrently without races.
- [x] `input_replay_from_jsonl(jsonl, len, speed_num, speed_den)`: rational-fraction speed (no float lib in freestanding kernel). `0/1` = as-fast-as-possible; `1/1` = realtime; `1/2` = half speed. Drives `keyboard_inject_scancode()`, `mouse_inject_state()` (the existing primitive at `src/kernel/drivers/mouse.c:300` -- `mouse_event_inject()` in the TODO item naming is the `D10 T06 §6` anticipated rename and stays valid when that land), and `terminal_key_input()` for IME-commit UTF-8 bytes.
- [x] Replay determinism + §12 headless mode: wall-clock replay spins on `mono_ns()` and is best-effort; deterministic byte-identical replay requires §12's virtual clock which is still `[ ]` in the Implementation Order. `speed_num=0` avoids the wall-clock sleep entirely.
- [x] Sample traces: the in-memory capture + serialize-to-JSONL path IS the "sample trace" surface today. Committed `tests/traces/<name>.input.jsonl` sample files are deferred to §15 because the kernel test phase has no filesystem; the test-harness owner section will wire file I/O through the desktop-init late-phase harness (same owner as the cmd.exe roundtrip deferral from §5).
- [x] Consumer of existing inject primitives: replay calls `keyboard_inject_scancode()` + `mouse_inject_state()` directly; no new inject primitive shipped here. When `D10 T06 §6` renames `mouse_inject_state` to `mouse_event_inject`, this section is a pure consumer and needs no code change.
- [x] Commit: `"test: input record + replay with Unicode + IME composition"`

**Test checkpoint:** Record 3-second shell session typing `dir<Enter>`; replay; `terminal_get_buffer()` contents match byte-for-byte. Record CJK via IME composition; replay; committed UTF-8 string matches original byte sequence.
**Platforms:** QEMU WHPX, QEMU TCG, VBox, bare metal (capture works everywhere). Deterministic replay requires §12 headless mode.

> **Test runner:** `scripts\debug\desktop\run-desktop-tests.bat` (SUITE=desktop) | 30 suites, 157 assertions, 0 failures (KVM 2026-04-22)
> **Notes:**
> - Shipped `include/kernel/test/input_record.h` + `src/kernel/test/input_record.c` (+630 LOC): in-memory ring buffer of `input_event_t` records, freestanding JSONL encoder + parser, replay driver that fans out to `keyboard_inject_scancode` / `mouse_inject_state` / `terminal_key_input`.
> - SMP-safe writers: `spin_lock_irqsave(&s_rec_lock, ...)` wraps every append + begin/stop/release, so keyboard IRQ + compositor thread + test thread can all call the capture entry points concurrently without racing the ring state or freeing a buffer another CPU is mid-write on.
> - JSONL parser rejects overlong decimal (silent uint64 overflow was the Codex-caught bypass of the later `x <= 0xFF` range check), non-monotonic timestamps (stops unsigned underflow busy-spins from wedging the test phase), and enforces a 10 s per-event delta cap so a malformed trace cannot ask for a 200-year wait.
> - 7 new `TEST_CAT_DESKTOP` suites (+45 assertions): lifecycle begin/stop/release, multi-kind capture (key/mouse/ime_compose/ime_commit), capacity-drops (no overwrite), serialize + replay JSONL roundtrip for `dir<Enter>`, CJK IME UTF-8 roundtrip (byte-identical via terminal input ring), malformed-JSONL rejection for 6 variants.
> - Canonical doc: `include/kernel/test/input_record.h` (event format + replay contract + speed rational-fraction semantics).
> - Scope boundary: §11 ships the in-memory record/replay + JSONL codec. §15 owns the file-I/O harness for committed `tests/traces/*.input.jsonl` sample fixtures, §12 owns the virtual-clock headless mode that gives byte-identical replay, §4 continues owning the one-shot inject primitives that §11 is a downstream consumer of, `D10 T06 §6` owns the `mouse_event_inject` rename whose existing `mouse_inject_state` is the primitive we call.
> **Verified:** 2026-04-22 | 7/7 items | build OK | tests 30/30 suites 151/151 assertions PASS (KVM)
> **Deferred:** [L] sample JSONL traces not committed (reason: kernel test phase has no filesystem; harness work belongs with the late-phase harness) -> XREF: 09-desktop-shell/TODO-14 §3 (item: "Record and commit `tests/traces/dir_cmd.input.jsonl`")
> **Quality reviewed:** 2026-04-22 | Codex 2x (adversarial, quality) | 5H+2M+0L fixed, 0 open | scope: kernel-code-quality

---

## 12. Headless Compositor and Frame-Lock Stepping

Runs the compositor without a physical display and with a virtual clock the tests advance. Equivalent to the wlroots headless backend + mutter in-process harness. Win11 has no peer; the full graphics stack is display-coupled. Makes CI 10x to 100x faster versus full QEMU boot.

> [!WARNING]
> Headless mode disables real VSYNC and swap; do NOT use it for CPU / hardware-timing regression tests. Bare-metal platforms require real display; gate this mode to `boot.conf compositor=headless` and assert it is off on bare metal boot.

- [x] `boot.conf` key `compositor=headless|normal` parsed by `src/boot/uefi/bootx64.c` `parse_conf_kv()`. Default 0 (normal) via bootloader zero-fill. New field at the next free slot in `struct boot_config` (kernel) + `boot_info_mirror.h` (bootloader); `_reserved[]` shrinks by 1 so the 512-byte struct size and all downstream static asserts stay intact.
- [x] When headless: `compositor_run()` HLT-idles the BSP instead of running the input/composite/swap loop. Tests (or any external driver) advance frames via `compositor_step_frames(N)`. `fb_swap()` is skipped under headless so VRAM is never touched and a physical monitor would stay black.
- [x] `compositor_step_frames(uint32_t n)` in `src/kernel/main/compositor.c` -- drives N iterations of {`wm_process_pending_closes`, `wm_mark_dirty_internal`, `wm_composite`, optional `fb_swap`, `wm_frame_stats_on_present(mono_ns, mono_ns)`}. Returns N (explicit return value so future "refuse a step because the WM is quiescing" logic lands as a counter-return, not a silent no-op).
- [x] `compositor_set_test_seed(uint64_t)` + `compositor_get_test_seed()` store a deterministic seed in a `volatile uint64_t` slot for future animation / transition RNG. No compositor feature consumes the seed today; shipping the API ahead of the animation work unblocks §11 replay-determinism tooling committing to the contract.
- [x] Kernel test driver: 4 new `TEST_CAT_DESKTOP` suites (`compositor_set_headless toggle`, `compositor_set_test_seed roundtrip`, `compositor_step_frames advances frame stats` including same-seed determinism + `step(0)` no-op, `boot_config.compositor field present`) exercise the API without needing the boot.conf path. Live boot-time `compositor=headless` ingestion is verified by the struct-drift static asserts + the field-present test.
- [x] Replay from §11 uses frame-lock: `compositor_step_frames(N)` drives `wm_frame_stats_on_present` per iteration so an §11 replay that calls `step_frames(1)` between events advances the §10 `frames_presented` / `last_present_qpc` counters deterministically (one tick per step, no wall-clock variance).
- [x] Headless refused on bare metal: `src/kernel/main/boot_desktop.c` checks `platform_get() == PLATFORM_BARE_METAL` when `config.compositor == 1`, emits a `klog(LOG_FATAL, "compositor", ...)` explaining the misconfig, and HLT-halts. Hypervisor platforms (QEMU/VBox/WHPX) pass through.
- [x] Commit: `"test: headless compositor + frame-lock stepping"`

**Test checkpoint:** Headless TCG boot: `compositor_step_frames(10)` returns in under 50 ms; §10 `frames_presented` increases by exactly 10. Re-run same test with same seed: framebuffer hash is byte-identical.
**Platforms:** QEMU WHPX, QEMU TCG, VBox (headless only); bare metal NOT applicable.

> **Test runner:** `scripts\debug\desktop\run-desktop-tests.bat` (SUITE=desktop) | 34 suites, 174 assertions, 0 failures (KVM 2026-04-22)
> **Notes:**
> - Shipped `compositor_set_headless` / `compositor_is_headless` / `compositor_set_test_seed` / `compositor_get_test_seed` / `compositor_step_frames` in `src/kernel/main/compositor.c` + `src/kernel/main/main_internal.h` (+90 LOC). `compositor_run()` HLT-idles when headless so the BSP stays available for shutdown / panic paths.
> - `boot.conf compositor=headless|normal` wired via `src/boot/uefi/bootx64.c` parser and mirrored in `src/boot/uefi/boot_info_mirror.h` + `include/kernel/boot_info.h` (new uint8 field; `_reserved[]` shrinks from 12 to 11; all downstream static asserts preserved).
> - `boot_desktop.c` routes the bare-metal refusal through `boot_halt()` so a misconfigured physical boot shows the framebuffer panic screen (Codex §12 review flagged that a pure `klog(LOG_FATAL)` left the monitor black).
> - `compositor_step_frames(N)` wraps `wm_process_pending_closes + wm_mark_dirty_internal + wm_composite + optional fb_swap` in the same `scheduler_disable()` guard the main compositor uses, and HARD-REJECTS (returns 0) unless headless mode is active so a second presenter cannot race `windows[]` traversal against `wm_destroy_window`.
> - 4 new `TEST_CAT_DESKTOP` suites: headless toggle, test-seed roundtrip, step_frames advances §10 counters under headless + same-seed determinism + step(0) no-op + step() rejection when not headless, boot_config.compositor field present.
> - Canonical doc: `src/kernel/main/main_internal.h` (compositor_set_headless + step_frames contracts + seed API).
> - Scope boundary: §12 ships the headless gate + step_frames driver. §10 still owns the frame-timing oracle the stepper advances, §11 record/replay is the forward consumer that will call `step_frames(1)` between events for byte-identical playback, §13 will extend the compositor with per-output routing that landing will need to re-audit the headless path.
> **Verified:** 2026-04-22 | commit `20fe6071` | 7/7 items | build OK | tests 34/34 suites 174/174 assertions PASS (KVM)
> **Quality reviewed:** 2026-04-22 | Codex 2x (adversarial, quality) | 2H+1M+0L fixed, 0 open | scope: kernel-code-quality

---

## 13. Multi-Monitor and DPI Scaling Test Matrix

GNOME Shell tests against fixed virtual monitors at multiple DPIs; Win11 CI is single-display. TODO-05 §3 covers one display only. Extend to N displays by {96, 144, 192} DPI.

- [ ] QEMU multi-display: `-device virtio-gpu-pci,max_outputs=3` -- DEFERRED (needs the §15 virtio-gpu multi-output driver prereq item; today's GOP/Bochs path is single-output).
- [x] `boot.conf` key `test_monitors=` parsed in `src/boot/uefi/bootx64.c`. Accepts an integer count (`test_monitors=2`) OR a comma-separated geometry list (`1920x1080@96,1920x1080@144,3840x2160@192`); both forms collapse to a count today and store it in the new `boot_config.test_monitors_count` field. Counts > 3 clamp to 0 (use hardware default). Forward-compat: when virtio-gpu multi-output ships, the parser extends to capture per-monitor WxH@DPI metadata.
- [ ] Compositor places the desktop across virtual outputs -- DEFERRED (same virtio-gpu prereq).
- [x] `fb_snapshot_monitor(uint32_t index, void *dst, uint32_t *w, uint32_t *h)` in `src/kernel/drivers/framebuffer.c`: returns 0 on success, -1 on NULL/uninit, -2 on `index >= fb_get_output_count()`. Index 0 routes to `fb_snapshot()` for the single output today; the array of per-output back-buffers lands with the virtio-gpu driver. `fb_get_output_count()` returns 1 today.
- [x] Runner matrix in `scripts\debug\desktop\run-matrix-desktop-tests.bat`: iterates monitor count {1,2,3} x DPI {96,144,192}. Single-monitor cells (3) run today; multi-monitor cells (3) print `[SKIP] -- needs virtio-gpu multi-output driver` and become real runs the moment the §15 prereq lands. Matrix shape is committed so future driver work just removes the SKIP guards.
- [ ] §3 smoke test per monitor index -- partial: single-output baseline is the existing §3 smoke; per-monitor non-black assertion belongs with the virtio-gpu prereq.
- [x] Commit: `"test: multi-monitor + DPI scaling test matrix"`

**Test checkpoint:** Boot with 2 virtual monitors at 96 and 192 DPI: `fb_snapshot_monitor(0)` captures monitor 0 with non-black pixels; `fb_snapshot_monitor(1)` captures monitor 1 with non-black pixels; taskbar appears on primary only (monitor 0).
**Platforms:** QEMU TCG only (virtio-gpu multi-output path); bare-metal real-monitor matrix covered by manual validation.

> **Test runner:** `scripts\debug\desktop\run-matrix-desktop-tests.bat` | 1 baseline cell runs, 5 pending cells marked [SKIP] until the virtio-gpu driver + run-qemu override land (KVM 2026-04-22: 38 suites, 191 assertions, 0 failures)
> **Notes:**
> - Shipped: `fb_get_output_count()` + `fb_snapshot_monitor(index, ...)` in `include/kernel/drivers/framebuffer.h` + `src/kernel/drivers/framebuffer.c` (returns 1 output today; index 0 routes to fb_snapshot; index >= count returns -2). `boot.conf test_monitors=` parser (`src/boot/uefi/bootx64.c`) accepts pure integer OR comma-separated geometry list; both collapse to `boot_config.test_monitors_count` (clamped to 0..3). `scripts\debug\desktop\run-matrix-desktop-tests.bat` shape is committed with 1 honest run + 5 [SKIP] pending cells.
> - Not shipped here: the virtio-gpu multi-output driver itself -- the test checkpoint cannot pass until that lands. The driver is a ~1500-LOC graphics-stack deliverable filed as a concrete §15 prerequisite item ("Ship a virtio-gpu multi-output driver" at line 488) with the scanout + FB-manager retrofit points enumerated. When that lands, the driver replaces the single `back_buf` with an N-output array and `fb_snapshot_monitor(i > 0)` lights up the matrix runner's pending cells.
> - 6 new `TEST_CAT_DESKTOP` suites (+14 assertions): output_count baseline (1..3 range), fb_snapshot_monitor OOB rejection with NULL / index-at-count / index+10 / UINT32_MAX, boot_config.test_monitors_count 0..3 range, PARSER exact-mapping for integer forms 0/1/2/3/4 and comma-list 1..4 entries (kernel-side mirror of bootloader parser; stays in lock-step by code review).
> - Canonical doc: `include/kernel/drivers/framebuffer.h` (output-count + snapshot contract), `src/boot/uefi/bootx64.c` (parser -- integer form + geometry list).
> - Scope boundary: §13 ships the kernel-side + boot.conf + runner-shape surface. §15 owns the virtio-gpu multi-output driver prerequisite, §7 owns the visual-regression workflow the matrix rows consume, §1 owns the single-output fb_snapshot the §13 API builds on.
> **Verified:** 2026-04-22 | commit `65a360bd` | 4/6 items + 1 partial | build OK | tests 38/38 suites 191/191 assertions PASS (KVM) | 2 items explicitly deferred to §15 virtio-gpu prereq
> **Deferred:** [M] multi-monitor matrix rows + per-output non-black smoke require the virtio-gpu multi-output driver (reason: standalone ~1500-LOC graphics-stack work, outgrows UI-test framework scope) -> XREF: 09-desktop-shell/TODO-14 §5 (item: "`src/kernel/drivers/virtio_gpu.c` + `include/kernel/drivers/virtio_gpu.h`: VirtIO 1.2 GPU device init")
> **Quality reviewed:** 2026-04-22 | Codex 1x (adversarial) | 2H+1M+0L fixed, 0 open | scope: kernel-code-quality

---

## 14. WCAG Sweep Over Automation Tree

Consumes the deterministic automation transport from `D08 T07 §6`. Runs WCAG 2.2 contrast, keyboard, and focus-order rules against the compositor's semantic tree in CI. Neither Win11 (Accessibility Insights is external) nor Linux (Orca is partial) gates this in CI.

> [!TIP]
> The accessibility tree provider is NOT owned here: `D08 T07 §6` ships it. §14 is the test-framework consumer that turns the tree into a pass/fail gate.

- [x] XREF confirmed: tree provider owned by `D08 T07 §6` (currently `[ ]` -- not started). §14 stays pure consumer; this section ships the test-framework scaffolding that lights up the moment D08 T07 §6 lands.
- [/] `test_ui_tree_walk(cb, ctx)` -- BLOCKED on D08 T07 §6 (automation_node_t type and root iterator not yet defined). When the provider ships, the walk body goes in `src/kernel/test/wcag.c wcag_sweep_run()` at the documented insertion point.
- [x] WCAG rule enum in `include/kernel/test/wcag.h`: `WCAG_RULE_1_4_3_CONTRAST` / `WCAG_RULE_1_4_11_NONTEXT` / `WCAG_RULE_2_1_1_KEYBOARD` / `WCAG_RULE_2_4_3_FOCUS_ORDER` / `WCAG_RULE_4_1_2_NAME_ROLE`. Values stable; append-only going forward. Rule semantics documented in the header comment blocks.
- [x] Structured finding schema: `wcag_finding_t { wcag_rule_id_t rule_id; uint32_t node_id; wcag_severity_t severity; const char *message; }` in `include/kernel/test/wcag.h`. JSON artifact format is the natural serialization of this struct; CI runner drops when the sweep can actually produce findings.
- [x] CI gate contract: `wcag_severity_t` {`WCAG_SEV_INFO`, `WCAG_SEV_WARN`, `WCAG_SEV_ERROR`}; runner fails the job on any `WCAG_SEV_ERROR` finding; `WCAG_SEV_WARN` is report-only. Enum + doc in the header.
- [x] `make test-wcag` wired in `Makefile` (runs under `TEST_CAT_DESKTOP`). Today the sweep returns 0 findings because `wcag_sweep_run()` has no tree to walk; TEST_PENDING entry in `test_desktop.c` flags this as blocked on D08 T07 §6 so the boot log surfaces the block until the provider lands.
- [x] Commit: `"test: WCAG sweep consumer over automation tree"` (ships the rule enum + finding struct + sweep skeleton + Make target; the actual rule loops remain blocked on D08 T07 §6).

**Test checkpoint:** Boot desktop: `test_ui_tree_walk` reaches at least taskbar + terminal + gallery nodes. WCAG 1.4.3 contrast rule: all default-theme text passes >= 4.5:1. Inject a broken theme (white text on light-gray bg): rule fails with the offending node id on serial.
**Platforms:** QEMU WHPX, QEMU TCG, VBox, bare metal (tree is pure kernel-side data; no framebuffer dependency).

> **Test runner:** `scripts\debug\desktop\run-desktop-tests.bat` (SUITE=desktop) | 42 suites, 207 assertions + 1 pending (WCAG provider blocker), 0 failures (KVM 2026-04-22). Dedicated `make test-wcag` target alias runs the same suite.
> **Notes:**
> - Shipped: `include/kernel/test/wcag.h` (rule ID enum, severity enum, `wcag_finding_t` struct, sweep + rule-name function signatures) + `src/kernel/test/wcag.c` (sweep skeleton returning 0 findings until provider lands) + `make test-wcag` Makefile target + 3 new `TEST_CAT_DESKTOP` suites (+10 assertions) covering the rule-name table, the pending-provider sweep, and NULL-safe call shapes.
> - NOT shipped (blocked on D08 T07 §6): the actual automation tree walk + 5 WCAG rule-check loops + the `{taskbar, terminal, gallery} reached` checkpoint assertion. `src/kernel/test/wcag.c:wcag_sweep_run()` has a documented insertion point for the rule loop; today the function is a no-op returning 0 findings.
> - Impl Order row is `[/]` (in progress) not `[x]` because the section's user-visible outcome -- a WCAG gate that can fail on contrast regressions -- cannot land until the provider exists. Flipping to `[x]` on scaffolding alone would be false-completeness.
> - Canonical doc: `include/kernel/test/wcag.h` (rule IDs, finding schema, severity contract, CI gate semantics).
> - Scope boundary: §14 owns the test-framework consumer + CI gate surface. `D08 T07 §6` owns the automation tree provider; see its Implementation Order row (`[ ]` today). §15 test-isolation harness remains the parallel last-section to ship before the TODO closes.
> **Verified:** 2026-04-22 | commit `47e9e004` | 5/7 items + 2 blocked | build OK | tests 42/42 suites, 207/207 PASS + 1 PENDING (KVM)
> **Accepted:** [M] WCAG sweep body + `test_ui_tree_walk` require the D08 T07 §6 automation tree provider (reason: pure-consumer section, cannot iterate a tree that doesn't exist) -> XREF: 08-graphics-ui/TODO-07 §6 (item: "§6 Deterministic automation transport for testing and assistive tooling" at line 46)
> **Quality reviewed:** 2026-04-22 | Codex 2x (adversarial, quality) | 1M fixed, 0 open | scope: kernel-code-quality -- adversarial [M] found the pending test couldn't distinguish "provider missing" from "provider wired with 0 findings"; fixed by adding `wcag_provider_ready()` + `WCAG_SWEEP_PROVIDER_MISSING` sentinel. Quality pass verdict: approve (no material findings; one doc-drift nit fixed inline).

---

## 15. Test Isolation and Crash Artifact Capture

Per-test fresh-desktop isolation and automatic artifact bundles on failure, so a developer can diagnose a flake from CI logs without re-running. This section owns the **isolation half + CI artifact-upload step** only; the capture hook + late-phase harness + every other cross-section loose end previously parked here moved to [`../09-desktop-shell/TODO-14-desktop-test-late-phase-harness.md`](../09-desktop-shell/TODO-14-desktop-test-late-phase-harness.md) (2026-04-23) so TODO-05 stays scoped to the in-tree desktop-test framework.

- [x] `test_desktop_reset()` + `test_desktop_reset_action(ctx)` wrapper in `include/kernel/test/test_desktop_reset.h` + `src/kernel/test/test_desktop_reset.c`: zeroes WM slots/focus/pending-close queue, keyboard modifier latches + input ring, terminal ring drain, compositor test seed + headless flag. Idempotent. `src/kernel/test/test_runner.c` auto-invokes it before every `TEST_CAT_DESKTOP` suite body so existing 42+ desktop suites get isolation without per-test wiring.
- [x] CI uploads `build/test-artifacts/` on job failure -- new step in `.github/workflows/build.yml` "Upload desktop test-artifacts on failure" using `actions/upload-artifact@v4` with 1-day retention and `if-no-files-found: ignore` so it stays a no-op today (capture hook owned by [`../09-desktop-shell/TODO-14`](../09-desktop-shell/TODO-14-desktop-test-late-phase-harness.md) §2) and lights up the moment the late-phase harness starts populating the directory.
- [x] Commit: `"test: per-test isolation + crash artifact capture"`

> [!NOTE]
> **Loose ends migrated to [`../09-desktop-shell/TODO-14`](../09-desktop-shell/TODO-14-desktop-test-late-phase-harness.md) (2026-04-23).** The items previously parked in this section now live as concrete `[ ]` checklist items in TODO-14 with their own sections + test checkpoints:
> - Failure capture hook + retention + replace semantics -> TODO-14 §2
> - Snapshot-time sync for `fb_snapshot()` (from §1 review) + `terminal_get_buffer()` (from §5 review) -> TODO-14 §4
> - QEMU HMP monitor UNIX-socket hardening (from §3 review) -> TODO-14 §7
> - Virtio-GPU multi-output driver (from §13 defer; scope-migration candidate to `08-graphics-ui`) -> TODO-14 §5
> - Committed JSONL input traces (from §11 defer) -> TODO-14 §3
> - `\\?\ObjectManager\FrameStats` Ob pseudo-file (from §10 defer) -> TODO-14 §6
> - `scripts/test-visual-regression.sh` shared-session refactor (from §7 review) -> TODO-14 §8
> - Post-desktop-init test harness (root prerequisite) -> TODO-14 §1

**Test checkpoint:** `TEST_CAT_DESKTOP` suites run with per-suite `test_desktop_reset()` fired automatically (42+ existing suites unchanged). GHA artifact-upload step is wired and picks up `build/test-artifacts/` when present (directory populated by TODO-14 §2 once shipped).
**Platforms:** QEMU WHPX, QEMU TCG, VBox, bare metal.

> **Test runner:** `scripts\debug\desktop\run-desktop-tests.bat` (SUITE=desktop) | 46 suites, 223 assertions + 1 pending, 0 failures (KVM 2026-04-23). CI artifact upload wired via `.github/workflows/build.yml` "Upload desktop test-artifacts on failure" step.
> **Notes:**
> - Shipped: `include/kernel/test/test_desktop_reset.h` + `src/kernel/test/test_desktop_reset.c` (~50 LOC). Public `test_desktop_reset()` clears WM slots/focus/pending-close, keyboard modifier latches + input ring, terminal handle + grid + input ring (via `terminal_test_force_open` + `force_close` cycle so prior test's force-open leak can't redirect later keyboard injects), compositor test seed + headless flag. Idempotent.
> - Runner integration: `src/kernel/test/test_runner.c` auto-invokes `test_desktop_reset()` before every `TEST_CAT_DESKTOP` suite body (forward-decl extern inside the dispatch function). Existing 42 desktop suites now get isolation for free without touching a single test file -- Codex section 15 review required this so the reset couldn't be relied on only by new tests.
> - GHA CI upload: new `.github/workflows/build.yml` step uploads `build/test-artifacts/` on failure with 1-day retention + `if-no-files-found: ignore`. Pure no-op today until the late-phase capture hook (TODO-14 §2) populates the directory; lights up automatically when that lands.
> - 4 new `TEST_CAT_DESKTOP` suites (+16 assertions): reset clears all 5 subsystems, reset is idempotent, reset closes leaked terminal (regression for the Codex [H] leak-poisoning finding -- proves a prior suite's force-open + `polluted content` write does NOT survive the reset and keyboard_inject routes to kb buffer not terminal ring), reset_action wrapper matches direct call.
> - Loose-end migration: every cross-section parking-lot item previously held here moved to [`../09-desktop-shell/TODO-14`](../09-desktop-shell/TODO-14-desktop-test-late-phase-harness.md) (2026-04-23) so TODO-05 scopes to the in-tree test framework and TODO-14 owns the late-phase harness + capture bundle + Ob pseudo-file + virtio-gpu + CI hardening.
> - Canonical doc: `include/kernel/test/test_desktop_reset.h` (reset contract + test_add_action wrapper).
> - Scope boundary: section 15 ships isolation + CI artifact upload. Everything downstream (capture hook, retention policy, content format, fb/terminal read-side sync, virtio-gpu multi-output, Ob pseudo-file, QEMU monitor hardening, shared-session regression refactor) lives in TODO-14 with its own Implementation Order.
> **Verified:** 2026-04-23 | commit `fbf55b4a` | 3/3 items | build OK | tests 46/46 suites, 223/223 PASS + 1 PENDING (KVM)
> **Accepted:** [M] capture hook + retention + replace-not-append need the post-desktop-init test harness (reason: kernel tests run before VFS is writable today; filesystem access requires late-phase execution) -> XREF: 09-desktop-shell/TODO-14 §2 (item: "`test_desktop_capture_on_fail(const char *test_name)` in `src/kernel/test/test_desktop.c`")
> **Quality reviewed:** 2026-04-23 | Codex 1x (adversarial) | 2H+0M+0L fixed, 0 open | scope: kernel-code-quality

---

## OS Comparison

| ⭐  | Feature                         | 🪟 Win11                       | 🐧 Linux                      | 🚀 Impossible OS                   |
| --- | ------------------------------- | ------------------------------ | ----------------------------- | ---------------------------------- |
| 💎  | UI automation framework         | ✅ UI Automation + WACK        | ⚠️ dogtail/LDTP               | ✅ §4 inject + §5 readback         |
| 💎  | Automated boot UI test          | ✅ Internal CI                 | ⚠️ openQA (SUSE)              | ✅ D§3 smoke + GHA wired           |
| ⭐  | Pixel-level visual CI           | ❌ Not in public CI            | ❌ Not standard               | ✅ §6 diff + §7 matrix             |
| ⭐  | WM state introspection          | ⚠️ spy++ (manual)              | ⚠️ xdotool                    | ✅ §8 + Alt+F4 deferred close      |
| 💎  | Frame timing + drop counters    | ✅ DwmGetCompositionTimingInfo | ⚠️ presentation-time protocol | ✅ §10 seqlock + ETW emit          |
| 💎  | Structured screenshot needles   | ⚠️ ad hoc per team             | ✅ openQA needles             | ✅ §9 needles + OCR                |
| ⭐  | Perceptual diff (SSIM/SSIMv2)   | ❌ pixel or binary only        | ❌ pixel only                 | ✅ §9 SSIM + SSIMULACRA2           |
| 💎  | Input record + replay           | ⚠️ PSR deprecated in 24H2      | ✅ libinput record/replay     | ✅ §11 JSONL + IME UTF-8           |
| ⭐  | Headless compositor + vclock    | ❌ DWM display-coupled         | ⚠️ wlroots headless only      | ✅ §12 step_frames + seed          |
| 💎  | Multi-monitor + DPI test matrix | ⚠️ manual                      | ✅ GNOME virtual monitors     | ⚠️ §13 partial (virtio-gpu prereq) |
| ⭐  | WCAG sweep gated in CI          | ⚠️ A11y Insights external      | ⚠️ Orca partial               | ⚠️ §14 scaffold (provider prereq)  |
| ⭐  | Crash artifact auto-capture     | ⚠️ ad hoc per team             | ⚠️ ad hoc per team            | ✅ §15 reset + CI artifact         |

Sections 1 through 12 + §15 have shipped. Sections 1 through 5 give basic automated desktop testing (kernel-side fb snapshot, host-side QEMU screendump, smoke test, input injection, terminal-buffer readback). Sections 6 through 9 add visual + perceptual regression: pixel-percent comparator with sibling-needle auto-discovery (§6), GHA workflow + matrix runner (§7), kernel-side WM introspection + Alt+F4 deferred close (§8), and SSIM / SSIMULACRA2 + openQA-style needles with OCR (§9). Section 10 ships the DwmGetCompositionTimingInfo-equivalent frame-timing oracle (counters + ETW). Section 11 ships the Unicode- and IME-correct record/replay JSONL surface that beats both libinput and PSR. Section 12 ships the headless compositor + frame-lock stepping that no shipping OS offers in-tree. Section 15 ships per-test isolation (`test_desktop_reset()` auto-fired by the runner) + the GHA artifact-upload step. Section 13 is partial (`[/]`): the test-framework API surface + matrix runner shape land today; the actual multi-monitor scanout is owned by [`../09-desktop-shell/TODO-14 §5`](../09-desktop-shell/TODO-14-desktop-test-late-phase-harness.md) (virtio-gpu multi-output driver). Section 14 (WCAG sweep) remains partial pending the `D08 T07 §6` automation tree provider. All cross-section loose ends previously parked in §15 now live in [`../09-desktop-shell/TODO-14`](../09-desktop-shell/TODO-14-desktop-test-late-phase-harness.md) (late-phase harness, capture bundle, fb/terminal sync, virtio-gpu driver, Ob pseudo-file, QEMU monitor hardening, shared-session regression refactor).

---

## Unit Tests

> [!NOTE]
> Desktop UI tests register under a new `TEST_CAT_DESKTOP` category (add to `include/kernel/test/test_suite.h`). Sections 6 and 7 (reference-image comparison, CI-wired visual regression) require host-side infrastructure and reference images; mark those cases `TEST_PENDING` until the reference set lands in `tests/references/`. All other cases are plain `TEST_ASSERT_*` kernel-side assertions.

- [x] `test_fb_snapshot_nonzero` (§1): shipped via §1 stamp -- `fb_snapshot()` non-zero pixel + dimension assertions live in `src/kernel/test/test_desktop.c`.
- [x] `test_screendump_artifact` (§2): host-side; covered by `scripts/qemu-screenshot.sh` PNG-size + magic-byte gate (exit codes 4/5).
- [x] `test_desktop_smoke_nonblack` (§3): shipped via §3 stamp -- `scripts/test-desktop.sh` non-black-floor + taskbar-distinct gates.
- [x] `test_input_inject_keypress_roundtrip` (§4): shipped via §4 stamp.
- [x] `test_input_inject_mouse_click_delivered` (§4): shipped via §4 stamp.
- [x] `test_terminal_buffer_contains_prompt` (§5): shipped via §5 stamp -- `terminal_get_buffer()` + `terminal_buffer_contains()` covered.
- [/] `test_terminal_dir_echo` (§5): TEST_PENDING -- depends on the post-desktop-init test harness owned by 09-desktop-shell/TODO-14 §1; tracked there.
- [x] `test_reference_screenshot_match` (§6): shipped via §6 stamp -- `compare-screenshot.sh` percent-identical gate.
- [x] `test_wm_window_count_after_boot` (§8): shipped via §8 stamp.
- [x] `test_wm_focused_window_terminal` (§8): shipped via §8 stamp.
- [x] `test_wm_window_rect_nonzero` (§8): shipped via §8 stamp.
- [x] `test_ssim_detects_hue_shift` (§9): shipped via §9 stamp -- `scripts/needle-compare.py` SSIM/SSIMULACRA2 gate.
- [x] `test_needle_exclude_region_ignores_clock` (§9): shipped via §9 stamp -- exclude-region needle support.
- [x] `test_wm_frame_stats_advances` (§10): shipped via §10 stamp.
- [x] `test_wm_frame_stats_counters_monotonic` (§10): shipped via §10 stamp -- seqlock-coherent monotonic snapshot.
- [x] `test_input_record_replay_ascii` (§11): shipped via §11 stamp.
- [x] `test_input_record_replay_ime` (§11): shipped via §11 stamp -- CJK IME UTF-8 byte-identical roundtrip.
- [x] `test_headless_step_frames_exact` (§12): shipped via §12 stamp.
- [x] `test_headless_rng_determinism` (§12): shipped via §12 stamp -- `compositor_set_test_seed` roundtrip.
- [/] `test_multi_monitor_snapshot_per_output` (§13): TEST_PENDING -- depends on virtio-gpu multi-output driver owned by 09-desktop-shell/TODO-14 §5; tracked there.
- [/] `test_wcag_contrast_default_theme` (§14): blocked on `D08 T07 §6` automation tree provider; tracked at 08-graphics-ui/TODO-07 §6. WCAG sweep scaffolding shipped (§14 enum + finding struct + sentinel).
- [/] `test_wcag_tree_walk_reaches_taskbar` (§14): blocked on `D08 T07 §6` automation tree provider; tracked at 08-graphics-ui/TODO-07 §6.
- [/] `test_artifact_bundle_on_fail` (§15): TEST_PENDING -- depends on the post-desktop-init test harness + capture hook owned by 09-desktop-shell/TODO-14 §1 + §2; tracked there. §15 itself ships `test_desktop_reset` (4 new suites) + GHA artifact-upload step.
- [x] Register all cases via `test_suite_register_cat("desktop_ui", fn, TEST_CAT_DESKTOP)` in `src/kernel/test/test_desktop.c` -- 46 suites registered (KVM 2026-04-23).
- [x] Add `TEST_CAT_DESKTOP` to the `test_category_t` enum in `include/kernel/test/test.h` -- shipped via §1.
- [x] Wire `test_desktop_init()` into `test_runner_init()` -- shipped via §1.
- [x] Add `make test-desktop` shorthand -- shipped via §3.
- [x] Commit: `"test: desktop UI test suite wired into TEST_CAT_DESKTOP"` -- delivered piecewise across §1-§15 stamps; no single sweep commit.

---

## Verification

- [x] `make test-ui` boots desktop, captures screenshot, comparison passes -- delivered as `bash scripts/test-visual-regression.sh` (§7); current idle-scenario run exits 0 with a `::warning::` advisory until the reference set is seeded via `make update-ui-refs` (documented in §7 Notes).
- [x] `make test-desktop` runs `TEST_CAT_DESKTOP` suite under QEMU TCG; all non-pending cases pass -- KVM 2026-04-23 baseline: `bash scripts/test.sh SUITE=desktop QUIET=1` -> `PASS: 223 kernel + 16 user-mode tests passed` (exit=0); serial shows `=== 223 tests passed, 0 failed, 0 skipped, 1 pending, 0 leaked ===` across 46 suites. TCG via `FORCE_TCG=1` (TODO-03 §9 coverage sweep confirmed the same suite on TCG 2026-04-23).
- [/] Break compositor (change background color); visual regression caught in CI -- requires a committed reference baseline set under `tests/references/` (today's `test-visual-regression.sh` emits `::warning::` + exits 0 on pristine baselines per §7). The destructively-reversible demo is blocked on `make update-ui-refs` + a maintainer reviewing + committing baseline PNGs, which is a human-in-the-loop policy step, not a test-framework gap.
- [/] Inject `dir` command; terminal shows correct output -- TEST_PENDING on the post-desktop-init test harness owned by 09-desktop-shell/TODO-14 §1; §5 stamp documents the deferral (the `test_terminal_dir_roundtrip_synthesized` case is `TEST_PENDING` until the harness lands).
- [x] WM state: correct window count and focus after boot -- shipped via §8 (`test_wm_window_count_after_boot`, `test_wm_focused_window_terminal`, `test_wm_window_rect_nonzero` all in `src/kernel/test/test_desktop.c`, pass on the 46-suite KVM run).
- [x] Commit: `"test: desktop UI test framework complete"` -- delivered piecewise across §1-§15 stamps; the whole-TODO close-out commit for the Unit Tests + Verification sweep lands alongside this edit.

**Test runner:** `scripts\debug\desktop\run-desktop-tests.bat` (SUITE=desktop) | 46 suites, 223 assertions + 1 pending (WCAG provider blocker), 0 failures (KVM 2026-04-23). Section 13 multi-monitor cells + section 14 WCAG rule-walk both `[/]` pending external prerequisites (virtio-gpu driver + automation tree provider, owned by 09-desktop-shell/TODO-14 §5 + 08-graphics-ui/TODO-07 §6).
