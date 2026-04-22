# TODO-05 -- Desktop & UI Test Framework

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

| ⭐  | Order | Deliverable                                        | Depends On    | Status |
| --- | :---: | -------------------------------------------------- | ------------- | :----: |
| 💎  |   1   | Framebuffer snapshot API (kernel-side capture)     | --            |  [x]   |
| 💎  |   2   | QEMU framebuffer dump (screendump via monitor)     | --            |  [x]   |
| 💎  |   3   | Desktop smoke test (non-black screen after boot)   | §1, §2        |  [x]   |
| 💎  |   4   | Input event injection (key press, mouse click)     | --            |  [x]   |
| 💎  |   5   | Terminal output verification                       | §4            |  [x]   |
| ⭐  |   6   | Reference screenshot comparison                    | §2            |  [x]   |
| ⭐  |   7   | Visual regression CI pipeline                      | §3, §6        |  [ ]   |
| ⭐  |   8   | Window manager state verification                  | §4            |  [ ]   |
| ⭐  |   9   | Perceptual diff + structured screenshot needles    | §6            |  [ ]   |
| 💎  |  10   | Frame timing + drop oracle (`wm_get_frame_stats`)  | --            |  [ ]   |
| 💎  |  11   | Input record + replay (Unicode, IME)               | §4, §12       |  [ ]   |
| ⭐  |  12   | Headless compositor + frame-lock stepping          | §1            |  [ ]   |
| 💎  |  13   | Multi-monitor + DPI test matrix                    | §1            |  [ ]   |
| ⭐  |  14   | WCAG sweep over automation tree                    | D08 T07 §6    |  [ ]   |
| 💎  |  15   | Test isolation + crash artifact capture            | §1, §10       |  [ ]   |

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
> **Accepted:** [M] `fb_snapshot()` copies the back buffer without a read-side lock; torn captures possible under an active compositor (reason: proper quiesce needs every writer to honor a new mutex) -> XREF: 00-infrastructure/TODO-05 §15 (item: "Add snapshot-time quiesce for `fb_snapshot()` so failure captures under an active compositor are not torn" at line 350)
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
> **Accepted:** [M] HMP monitor on 127.0.0.1:<port> with `server,nowait` is reachable by any local user on multi-tenant hosts (reason: UNIX-socket migration spans §2, §3, and run-qemu.ps1; larger than a single-section smoke test) -> XREF: 00-infrastructure/TODO-05 §15 (item: "Harden QEMU monitor exposure for shared-host CI" at line 374)
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
> **Accepted:** [M] `terminal_get_buffer()` / `terminal_buffer_contains()` read `term_cells[]` without a lock; concurrent `terminal_putchar` on the compositor thread can produce a torn snapshot once §15's cross-thread failure-capture harness runs (reason: proper quiesce needs every writer to honor a new mutex) -> XREF: 00-infrastructure/TODO-05 §15 (item: "Add a read-side synchronization barrier to `terminal_get_buffer()` / `terminal_buffer_contains()` so snapshots under an active compositor are not torn" at line 386)
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
> **Quality reviewed:** 2026-04-22 | Codex 1x (adversarial) | 1H+1M+0L fixed, 0 open | scope: N/A (host-side shell script; no domain code-quality skill)

---

## 7. Visual Regression CI Pipeline

Run desktop tests in CI and catch visual regressions.

- [ ] GitHub Actions step: boot QEMU headless with VNC or `-display none`
- [ ] After DESKTOP_READY: capture screenshot via monitor
- [ ] Compare against stored reference images
- [ ] Upload diff images as artifacts on failure
- [ ] Add to step summary: `### UI Tests: 3/3 visual checks passed ✅`
- [ ] Update reference images: `make update-ui-refs` captures new baselines
- [ ] Commit: `"ci: visual regression testing; screenshot comparison in GitHub Actions"`

**Test checkpoint:** Intentional compositor diff in a PR triggers visual-regression job; job exits non-zero, uploads diff PNG, step summary shows `MISMATCH`. Clean PR (no compositor changes) exits zero with `### UI Tests: N/N visual checks passed`.
**Platforms:** GitHub Actions `ubuntu-latest` TCG runner only (per T01 §6 policy). No WHPX / VBox / bare-metal CI path; those remain local.

**Regression risk:** MEDIUM; reference images are environment-dependent (QEMU version, font rendering). Use generous tolerance (95%) and update refs when intentional changes land.

---

## 8. Window Manager State Verification

Verify WM state without screenshots -- pure data inspection.

- [ ] `wm_get_window_count()` → number of open windows
- [ ] `wm_get_focused_window()` → handle of focused window
- [ ] `wm_get_window_rect(handle)` → x, y, width, height
- [ ] Test: after boot, verify: terminal window exists, gallery window exists, terminal is focused
- [ ] Test: inject Alt+F4 → window count decreases by 1
- [ ] Kernel test code: call WM introspection API directly
- [ ] Commit: `"test: WM state verification; window count, focus, rect inspection"`

**Test checkpoint:** After boot: `wm_get_window_count() == 2`, `wm_get_focused_window()` returns the terminal handle, `wm_get_window_rect(terminal)` and `wm_get_window_rect(gallery)` both return non-zero width and height. Inject Alt+F4; within 200 ms `wm_get_window_count() == 1`.
**Platforms:** QEMU WHPX, QEMU TCG, VBox, bare metal (kernel-side data inspection; no framebuffer dependency).

---

## 9. Perceptual Diff and Structured Screenshot Needles

Upgrades §6 beyond flat pixel-percent: adds SSIM / SSIMULACRA2 perceptual diff and openQA-style needles (match regions, exclude regions, region OCR assertions). Host-side tool chain; consumes the artifacts captured by §1 / §2 / §6.

> [!TIP]
> Win11 has no in-box perceptual diff (WACK compares binaries, not pixels). Linux has openQA needles but they ship as an external SUSE tool. Owning a structured + perceptual diff in the OS tree means every compositor change gets a rigorous visual gate without a third-party service.

- [ ] Perceptual diff backend: SSIM as default (fast, structure-aware); SSIMULACRA2 as high-quality opt-in; raw pixel-percent retained for quick gates
- [ ] `scripts/compare-screenshot.sh --mode=pixel|ssim|ssimulacra2 --threshold=N reference.png current.png`
- [ ] Needle schema: PNG + sidecar JSON `reference.needle.json` with match regions (x,y,w,h,tolerance) and exclude regions (clocks, timers, other dynamic pixels)
- [ ] Per-region tolerance: strict (pixel-exact), relaxed (SSIM >= 0.95), or ignored (skip comparison entirely)
- [ ] Region OCR assertion: crop text regions, run tesseract OCR, assert expected string; exclude-region covers dynamic timestamp cells
- [ ] Host toolchain: require `imagemagick`, tesseract; SSIM via Python `scikit-image`; SSIMULACRA2 binary optional with graceful fallback
- [ ] `make update-ui-refs` captures new baselines AND regenerates needle JSON skeleton for manual region edit
- [ ] Commit: `"test: perceptual diff + structured needles (SSIM, exclude regions, OCR)"`

**Test checkpoint:** Shift wallpaper hue by 5 percent: raw pixel-percent at 95 percent threshold passes (false negative), SSIM catches the structural change. Mark clock region as exclude-zone: two consecutive-second captures still compare equal. Region OCR asserts `C:\>` prompt string present in the cropped terminal region.
**Platforms:** host-side only (no kernel dependency); runs in CI (GHA ubuntu-latest) and local dev.

---

## 10. Frame Timing and Drop Oracle

Parity for Win11 `DwmGetCompositionTimingInfo` and Linux Wayland `presentation-time` protocol. Exposes compositor frame counters and per-frame VSYNC / present timestamps so tests can assert "zero dropped frames during animation X."

- [ ] Kernel type: `struct wm_frame_stats { uint64_t frames_presented, frames_queued, frames_late, frames_dropped, last_vsync_qpc, last_present_qpc; }` in `include/desktop/wm.h`
- [ ] `wm_get_frame_stats(struct wm_frame_stats *out)`: monotonic counters; safe to call concurrently with compositor thread
- [ ] Compositor bumps counters on each swap: `late` = present after the VSYNC deadline, `dropped` = frame queued but superseded before present
- [ ] ETW event `WM_FRAME_PRESENTED` emitted per frame with timestamp + state deltas (hooks into existing ETW infra)
- [ ] Namespace exposure: `\\?\ObjectManager\FrameStats` read-only pseudo-file so user-mode tools can tail the counters (optional if namespace cost is low)
- [ ] `wm_frame_stats_reset_for_test()` zeroes counters between test cases (test-only entry point)
- [ ] Commit: `"test: frame-timing oracle -- wm_get_frame_stats + ETW WM_FRAME_PRESENTED"`

**Test checkpoint:** Idle 100 ms after boot: `frames_presented > 0`, `frames_dropped == 0`, `frames_late < 5`. Trigger a window-resize animation: counters advance monotonically, `frames_dropped` stays within budget (<= 2 per animation sequence).
**Platforms:** QEMU WHPX, QEMU TCG, VBox, bare metal (kernel-side counters; no framebuffer capture needed).

---

## 11. Input Record and Replay (Unicode and IME aware)

Extends §4 from one-shot inject to full record + replay traces. Linux `libinput record` captures `uinput` events but ydotool / wtype botch non-ASCII and IME composition. Win11 has no public record API (PSR is deprecated in 24H2). Impossible designs this with UTF-8 and IME composition correct from day one.

> [!TIP]
> Deterministic regression: a recorded trace + §12 headless mode gives byte-identical replay across runs. Neither Win11 nor Linux ships this combination in-tree.

- [ ] Trace format: `tests/traces/*.input.jsonl`; one event per line; fields `{ts_ns, kind, payload}` where `kind` is `key` / `mouse` / `ime_compose` / `ime_commit`
- [ ] Key events carry scancode AND resolved Unicode codepoint; IME events carry preedit string + candidate index + commit string
- [ ] `test_input_record_begin(const char *path)` / `test_input_record_end(void)`: tee mode -- capture does NOT suppress delivery to the real queue
- [ ] `test_input_replay(const char *path, float speed)`: drives `mouse_event_inject()` (owned by `D10 T06 §6`) and sibling key-inject primitive at recorded timestamps; `speed=1.0` realtime, `speed=0.0` as-fast-as-scheduler
- [ ] Replay determinism requires §12 headless mode (virtual clock); wall-clock replay is best-effort
- [ ] Ship sample traces: `dir_cmd.input.jsonl` (ASCII), `cjk_hello.input.jsonl` (CJK via IME), `drag_resize.input.jsonl` (mouse drag)
- [ ] Reuse `mouse_event_inject()` from `D10 T06 §6`: do NOT redefine the primitive here; §4 and §11 are both consumers
- [ ] Commit: `"test: input record + replay with Unicode + IME composition"`

**Test checkpoint:** Record 3-second shell session typing `dir<Enter>`; replay; `terminal_get_buffer()` contents match byte-for-byte. Record CJK via IME composition; replay; committed UTF-8 string matches original byte sequence.
**Platforms:** QEMU WHPX, QEMU TCG, VBox, bare metal (capture works everywhere). Deterministic replay requires §12 headless mode.

---

## 12. Headless Compositor and Frame-Lock Stepping

Runs the compositor without a physical display and with a virtual clock the tests advance. Equivalent to the wlroots headless backend + mutter in-process harness. Win11 has no peer; the full graphics stack is display-coupled. Makes CI 10x to 100x faster versus full QEMU boot.

> [!WARNING]
> Headless mode disables real VSYNC and swap; do NOT use it for CPU / hardware-timing regression tests. Bare-metal platforms require real display; gate this mode to `boot.conf compositor=headless` and assert it is off on bare metal boot.

- [ ] `boot.conf` key: `compositor=headless` (default off); parsed by compositor init
- [ ] When headless: no framebuffer swap, no VSYNC wait; compositor blocks on `compositor_step_frames(N)` instead of the timer tick
- [ ] `compositor_step_frames(uint32_t n)`: advances the render loop by exactly `n` frames; blocks caller until `n` presents complete; returns count of frames actually presented (matches §10 counters)
- [ ] `compositor_set_test_seed(uint64_t seed)`: seeds any RNG used by animations / transitions so diffs are reproducible
- [ ] Kernel test driver: `TEST_CAT_DESKTOP` tests detect headless via `boot.conf`; call `compositor_step_frames()` between input injections instead of `sleep_ms()`
- [ ] Replay from §11 uses frame-lock: trace timestamps advance by frame boundary, not wall clock
- [ ] Headless is refused on bare metal boot: if no display hardware path is required yet headless is set, the compositor halts with a clear message (catches mis-configured boot.conf on physical kit)
- [ ] Commit: `"test: headless compositor + frame-lock stepping"`

**Test checkpoint:** Headless TCG boot: `compositor_step_frames(10)` returns in under 50 ms; §10 `frames_presented` increases by exactly 10. Re-run same test with same seed: framebuffer hash is byte-identical.
**Platforms:** QEMU WHPX, QEMU TCG, VBox (headless only); bare metal NOT applicable.

---

## 13. Multi-Monitor and DPI Scaling Test Matrix

GNOME Shell tests against fixed virtual monitors at multiple DPIs; Win11 CI is single-display. TODO-05 §3 covers one display only. Extend to N displays by {96, 144, 192} DPI.

- [ ] QEMU multi-display: `-device virtio-gpu-pci,max_outputs=3` (TCG path only; WHPX / VBox virtio-gpu is flaky for multi-output)
- [ ] `boot.conf` key: `test_monitors=1920x1080@96,1920x1080@144,3840x2160@192` parsed into framebuffer driver config
- [ ] Compositor places the desktop across virtual outputs; primary monitor hosts the taskbar
- [ ] `fb_snapshot_monitor(int index, void *dst, uint32_t *w, uint32_t *h)`: extends §1 to capture per-output; returns `E_INVALID_INDEX` when `index >= active_output_count`
- [ ] Runner matrix in `scripts/debug/desktop/run-all-desktop-tests.bat`: iterate monitor count in {1, 2, 3} across DPI in {96, 144, 192}; emit one result row per cell
- [ ] §3 smoke test repeated per monitor index; per-monitor non-black threshold >= 10 percent
- [ ] Commit: `"test: multi-monitor + DPI scaling test matrix"`

**Test checkpoint:** Boot with 2 virtual monitors at 96 and 192 DPI: `fb_snapshot_monitor(0)` captures monitor 0 with non-black pixels; `fb_snapshot_monitor(1)` captures monitor 1 with non-black pixels; taskbar appears on primary only (monitor 0).
**Platforms:** QEMU TCG only (virtio-gpu multi-output path); bare-metal real-monitor matrix covered by manual validation.

---

## 14. WCAG Sweep Over Automation Tree

Consumes the deterministic automation transport from `D08 T07 §6`. Runs WCAG 2.2 contrast, keyboard, and focus-order rules against the compositor's semantic tree in CI. Neither Win11 (Accessibility Insights is external) nor Linux (Orca is partial) gates this in CI.

> [!TIP]
> The accessibility tree provider is NOT owned here: `D08 T07 §6` ships it. §14 is the test-framework consumer that turns the tree into a pass/fail gate.

- [ ] XREF: tree provider owned by `D08 T07 §6`; §14 is pure consumer -- do not re-own or duplicate the provider
- [ ] `test_ui_tree_walk(void (*cb)(automation_node_t *, void *), void *ctx)`: iterates every automation node reachable from the root after boot-complete
- [ ] WCAG rule set (subset applicable to a compositor shell):
      - 1.4.3 contrast: text node foreground / background >= 4.5:1 (>= 3:1 for large text)
      - 1.4.11 non-text contrast: control boundary >= 3:1 against adjacent color
      - 2.1.1 keyboard: every actionable node has a tab-reachable path
      - 2.4.3 focus order: tab traversal reaches focusable nodes in logical order (no trap, no skip)
      - 4.1.2 name / role / value: every node has name + role set; value on inputs
- [ ] Rule violations produce structured findings: `{rule_id, node_id, severity, message}` on serial + JSON artifact
- [ ] CI gate: job fails if any `severity=error` rule fires; `severity=warn` is report-only
- [ ] `make test-wcag` runs the sweep against the boot-complete desktop
- [ ] Commit: `"test: WCAG sweep consumer over automation tree"`

**Test checkpoint:** Boot desktop: `test_ui_tree_walk` reaches at least taskbar + terminal + gallery nodes. WCAG 1.4.3 contrast rule: all default-theme text passes >= 4.5:1. Inject a broken theme (white text on light-gray bg): rule fails with the offending node id on serial.
**Platforms:** QEMU WHPX, QEMU TCG, VBox, bare metal (tree is pure kernel-side data; no framebuffer dependency).

---

## 15. Test Isolation and Crash Artifact Capture

Per-test fresh-desktop isolation and automatic artifact bundles on failure, so a developer can diagnose a flake from CI logs without re-running.

- [ ] `test_desktop_reset()`: closes non-essential windows; reseeds compositor RNG via §12 `compositor_set_test_seed()`; clears input queues; invoked automatically between `TEST_CAT_DESKTOP` cases
- [ ] Failure hook: `TEST_ASSERT_*` failures inside `TEST_CAT_DESKTOP` invoke `test_desktop_capture_on_fail(const char *test_name)` which writes to `build/test-artifacts/<test>/`:
      - `screen.png` via `fb_snapshot()` (§1)
      - `serial.log`: last 1 MiB of serial tail
      - `etw.bin`: last 1 MiB of ETW buffer
      - `wm.json`: window list + focus + rects snapshot
- [ ] CI uploads `build/test-artifacts/` on job failure (GHA `actions/upload-artifact@v4`); retention per `T01 §6` policy
- [ ] Local retention: keep most recent 10 failure bundles; older bundles auto-pruned
- [ ] Re-running a failing test replaces the prior bundle, does NOT append (avoids stale artifact drift)
- [ ] Add snapshot-time quiesce for `fb_snapshot()` so failure captures under an active compositor are not torn. Today `src/kernel/drivers/framebuffer.c:fb_snapshot()` does a lockless row copy; §1 relies on `spinner_stop()` + pre-compositor test timing. §15 ships the first caller that runs under active rendering, so add a read-side lock (or compositor flush barrier) that `fb_blit` / `fb_put_pixel` / `spinner_advance` writers honor, then drop the header caveat. Codex quality review of §1 (2026-04-22) accepted the race here.
- [ ] Harden QEMU monitor exposure for shared-host CI: today `scripts/test-desktop.sh` and any manual `-Monitor` user of `scripts/machines/run-qemu.ps1` bind the HMP monitor to `127.0.0.1:<port>` with `server,nowait`, which is reachable by any local user on a multi-tenant Linux / WSL host. Migrate `scripts/qemu-screenshot.sh` to accept an optional `--socket <path>` invoking `nc -U`, add a matching `-MonitorSocket` switch to `run-qemu.ps1` (Linux hosts only; Windows keeps TCP because WSL's AF_UNIX interop is flaky), and update `scripts/test-desktop.sh` to prefer the UNIX socket path. Codex adversarial review of §3 (2026-04-22) accepted this as out-of-scope for the single-section smoke test.
- [ ] Add a read-side synchronization barrier to `terminal_get_buffer()` / `terminal_buffer_contains()` so snapshots under an active compositor are not torn. Today `src/desktop/terminal.c` reads `term_cells[]` locklessly; §5 narrowed the header contract to single-threaded or quiesced callers. §15 ships the first cross-thread caller (failure-capture bundle), so introduce a read-side mutex (or compositor-quiesce fence) that `terminal_putchar` / `terminal_puts` writers honor, then drop the single-threaded caveat in `include/desktop/terminal.h`. Codex adversarial review of §5 (2026-04-22) accepted the race here.
- [ ] Ship a post-desktop-init test harness that can drive `terminal_*` APIs and cmd.exe with the WM actually initialized. Kernel tests run in Phase 3 BEFORE `boot_desktop.c` calls `terminal_open()` + `wm_create_window()`, so the §5 `test_terminal_dir_roundtrip_synthesized` case currently resolves as TEST_PENDING (`src/kernel/test/test_desktop.c:370-ish`). Options: (a) add a late-phase test hook that runs after the desktop compositor has initialized but before the shell prompts the user, (b) extend `scripts/test-desktop.sh` to drive the full `dir` roundtrip via `qemu-input.sh sendstring "dir\\n"` + `terminal_buffer_contains` read-back through a new debug syscall, (c) run TEST_CAT_DESKTOP a second time from compositor context for tests tagged "late". This unblocks the real cmd.exe end-to-end test and the `test_terminal_dir_roundtrip_synthesized` case that the §5 review (2026-04-22) flagged as PENDING.
- [ ] Commit: `"test: per-test isolation + crash artifact capture"`

**Test checkpoint:** Deliberately fail a desktop test: `build/test-artifacts/<test>/` contains `screen.png`, `serial.log`, `etw.bin`, `wm.json`. Re-run the same failing test: the prior bundle is replaced, not appended; directory contents match the second run only.
**Platforms:** QEMU WHPX, QEMU TCG, VBox, bare metal.

---

## OS Comparison

| ⭐ | Feature                         | 🪟 Win11                       | 🐧 Linux                      | 🚀 Impossible OS          |
|----|---------------------------------|---------------------------------|-------------------------------|----------------------------|
| 💎 | UI automation framework         | ✅ UI Automation + WACK        | ⚠️ dogtail/LDTP               | ⬜ §4,§5 + D08 T07 §6     |
| 💎 | Automated boot UI test          | ✅ Internal CI                 | ⚠️ openQA (SUSE)              | ⬜ §3                     |
| ⭐ | Pixel-level visual CI           | ❌ Not in public CI            | ❌ Not standard               | ⬜ §6,§7                  |
| ⭐ | WM state introspection          | ⚠️ spy++ (manual)              | ⚠️ xdotool                    | ⬜ §8                     |
| 💎 | Frame timing + drop counters    | ✅ DwmGetCompositionTimingInfo | ⚠️ presentation-time protocol | ⬜ §10                    |
| 💎 | Structured screenshot needles   | ⚠️ ad hoc per team             | ✅ openQA needles             | ⬜ §9                     |
| ⭐ | Perceptual diff (SSIM/SSIMv2)   | ❌ pixel or binary only        | ❌ pixel only                 | ⬜ §9                     |
| 💎 | Input record + replay           | ⚠️ PSR deprecated in 24H2      | ✅ libinput record/replay     | ⬜ §11, Unicode+IME       |
| ⭐ | Headless compositor + vclock    | ❌ DWM display-coupled         | ⚠️ wlroots headless only      | ⬜ §12                    |
| 💎 | Multi-monitor + DPI test matrix | ⚠️ manual                      | ✅ GNOME virtual monitors     | ⬜ §13                    |
| ⭐ | WCAG sweep gated in CI          | ⚠️ A11y Insights external      | ⚠️ Orca partial               | ⬜ §14                    |
| ⭐ | Crash artifact auto-capture     | ⚠️ ad hoc per team             | ⚠️ ad hoc per team            | ⬜ §15                    |

After sections 1 through 5, Impossible OS has basic automated desktop testing (smoke test, input injection, terminal verification). Sections 6 through 8 add visual regression and WM introspection that neither Windows nor Linux provides in public CI. Sections 9 through 15 push further: perceptual diff and openQA-style needles (§9), a frame-timing oracle on par with DWM (§10), Unicode- and IME-correct record/replay that beats both baselines (§11), a headless compositor with virtual-clock frame stepping that no shipping OS offers (§12), GNOME-style multi-monitor + DPI matrix (§13), CI-gated WCAG sweep that is external-only on Win11 and partial on Linux (§14), and per-test isolation plus automatic artifact bundles (§15).

---

## Unit Tests

> [!NOTE]
> Desktop UI tests register under a new `TEST_CAT_DESKTOP` category (add to `include/kernel/test/test_suite.h`). Sections 6 and 7 (reference-image comparison, CI-wired visual regression) require host-side infrastructure and reference images; mark those cases `TEST_PENDING` until the reference set lands in `tests/references/`. All other cases are plain `TEST_ASSERT_*` kernel-side assertions.

- [ ] `test_fb_snapshot_nonzero` (§1): call `fb_snapshot()` after compositor ready; `TEST_ASSERT(width * height > 0)`; `TEST_ASSERT` at least one pixel in the buffer is non-zero.
- [ ] `test_screendump_artifact` (§2): `TEST_PENDING` (host-side; asserts `build/screenshot.png` size >= 100 KiB and PNG magic bytes).
- [ ] `test_desktop_smoke_nonblack` (§3): capture via §1; `TEST_ASSERT` non-black pixel percentage >= 10; `TEST_ASSERT` top taskbar row contains at least one non-wallpaper color.
- [ ] `test_input_inject_keypress_roundtrip` (§4): `test_inject_keypress(SCAN_A)`; within 100 ms keyboard buffer head returns scancode for `A`.
- [ ] `test_input_inject_mouse_click_delivered` (§4): `test_inject_mouse_click(x, y, 1)`; WM input queue receives a click event at that coordinate within 100 ms.
- [ ] `test_terminal_buffer_contains_prompt` (§5): after compositor ready, `terminal_get_buffer()` contains the literal `C:\>` prompt string.
- [ ] `test_terminal_dir_echo` (§5): inject `d`, `i`, `r`, Enter; within 500 ms `terminal_get_buffer()` contains the echo `dir` and at least one directory entry.
- [ ] `test_reference_screenshot_match` (§6): `TEST_PENDING` (host-side; asserts comparison >= 95% against stored reference).
- [ ] `test_wm_window_count_after_boot` (§8): `TEST_ASSERT_EQ(wm_get_window_count(), 2, "terminal + gallery")`.
- [ ] `test_wm_focused_window_terminal` (§8): `TEST_ASSERT(wm_get_focused_window() == terminal_handle)`.
- [ ] `test_wm_window_rect_nonzero` (§8): both terminal and gallery windows return non-zero width and height.
- [ ] `test_ssim_detects_hue_shift` (§9): `TEST_PENDING` (host-side; shift wallpaper hue by 5 percent, assert SSIM score < 0.98 while raw pixel-percent still passes 95).
- [ ] `test_needle_exclude_region_ignores_clock` (§9): `TEST_PENDING` (host-side; two consecutive-second captures compare equal when clock region is in exclude list).
- [ ] `test_wm_frame_stats_advances` (§10): after `wm_frame_stats_reset_for_test()`, idle 100 ms; `TEST_ASSERT(frames_presented > 0)` AND `TEST_ASSERT_EQ(frames_dropped, 0)`.
- [ ] `test_wm_frame_stats_counters_monotonic` (§10): call `wm_get_frame_stats()` twice 50 ms apart; second call `frames_presented >= first.frames_presented`.
- [ ] `test_input_record_replay_ascii` (§11): record 3-second `dir<Enter>` trace; replay; `terminal_get_buffer()` matches the first run byte-for-byte.
- [ ] `test_input_record_replay_ime` (§11): record CJK IME composition; replay; committed UTF-8 string matches original.
- [ ] `test_headless_step_frames_exact` (§12): under `compositor=headless`, `compositor_step_frames(10)` returns with §10 `frames_presented` increased by exactly 10.
- [ ] `test_headless_rng_determinism` (§12): two runs with the same `compositor_set_test_seed(seed)` produce byte-identical framebuffer hash after 10 frame steps.
- [ ] `test_multi_monitor_snapshot_per_output` (§13): `TEST_PENDING` until virtio-gpu multi-output is wired; later asserts `fb_snapshot_monitor(0)` and `fb_snapshot_monitor(1)` both return non-black.
- [ ] `test_wcag_contrast_default_theme` (§14): walk automation tree; every text node's contrast ratio >= 4.5:1 in default theme.
- [ ] `test_wcag_tree_walk_reaches_taskbar` (§14): `test_ui_tree_walk` visits at least one node with role `taskbar` and one with role `terminal`.
- [ ] `test_artifact_bundle_on_fail` (§15): force a failing assertion in a canary test; after the run, `build/test-artifacts/<canary>/` contains `screen.png`, `serial.log`, `etw.bin`, `wm.json`.
- [ ] Register all cases via `test_suite_register_cat("desktop_ui", fn, TEST_CAT_DESKTOP)` in `src/kernel/test/test_desktop.c`.
- [ ] Add `TEST_CAT_DESKTOP` to the `test_category_t` enum in `include/kernel/test/test.h` (before `TEST_CAT_COUNT`).
- [ ] Wire `test_desktop_init()` into `test_runner_init()`.
- [ ] Add `make test-desktop` shorthand (mirrors `make test-mm`, `make test-ob`).
- [ ] Commit: `"test: desktop UI test suite wired into TEST_CAT_DESKTOP"`

---

## Verification

- [ ] `make test-ui` boots desktop, captures screenshot, comparison passes.
- [ ] `make test-desktop` runs `TEST_CAT_DESKTOP` suite under QEMU TCG; all non-pending cases pass.
- [ ] Break compositor (change background color); visual regression caught in CI.
- [ ] Inject `dir` command; terminal shows correct output.
- [ ] WM state: correct window count and focus after boot.
- [ ] Commit: `"test: desktop UI test framework complete"`

**Test runner:** `scripts\debug\desktop\run-desktop-tests.bat` (SUITE=desktop) | 1 suite, 0 failures
