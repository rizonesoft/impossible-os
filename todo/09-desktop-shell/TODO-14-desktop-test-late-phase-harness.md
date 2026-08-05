---
schema_version: 1
id: desktop-test-late-phase-harness
domain: 09-desktop-shell
status: active
title: "TODO-14 -- Desktop Test Late-Phase Harness and Artifact Bundle"
---

# TODO-14 -- Desktop Test Late-Phase Harness and Artifact Bundle

> **Goal:** Ship the **late-phase kernel test harness** that runs after `boot_desktop.c` has brought up the compositor + terminal + WM, plus the cross-subsystem wiring the desktop UI test framework (`00-infrastructure/TODO-05`) needs before it can close. Today `TEST_CAT_DESKTOP` runs in Phase 3 against in-memory fixtures; the real user-facing assertions (cmd.exe `dir` roundtrip, on-failure artifact bundle, torn-snapshot-free captures under an active compositor) all wait on this TODO.

> [!IMPORTANT]
> **Current state:** Sourced from `00-infrastructure/TODO-05 §15` (2026-04-23). `TODO-05 §15` itself shipped its in-scope half (`test_desktop_reset()` + GHA artifact-upload step). The deferred items in that section were parking-lot references to work that spans multiple subsystems; they live here as concrete `[ ]` items with explicit XREFs back to the sections that depend on them.

## Inputs

| Path / TODO | Purpose |
|-------------|---------|
| [`../00-infrastructure/TODO-05-desktop-ui-test-framework.md`](../00-infrastructure/TODO-05-desktop-ui-test-framework.md) §1, §5, §10, §11, §13, §15 | Deferred cross-section items tracked here |
| [`../02-kernel-core/TODO-03-object-manager.md`](../02-kernel-core/TODO-03-object-manager.md) | Ob pseudo-file driver pattern for §6 `\\?\ObjectManager\FrameStats` |
| [`../08-graphics-ui/INDEX.md`](../08-graphics-ui/INDEX.md) | True domain home for §5 virtio-gpu multi-output driver -- migrate when a specific virtio-gpu TODO is filed there |
| `src/kernel/main/boot_desktop.c` | Phase where the late-phase hook attaches |
| `src/kernel/test/test_runner.c` | Dispatch site where the late-phase category runs |
| `src/kernel/drivers/framebuffer.c` | `fb_snapshot()` read-side quiesce target |
| `src/desktop/terminal.c` | `terminal_get_buffer()` read-side quiesce target |

## Outcome

- Kernel tests can execute **after** the compositor + terminal are fully initialized, so `fb_snapshot()` lands on a live desktop and the VFS is writable.
- A failing `TEST_CAT_DESKTOP` suite produces `build/test-artifacts/<test>/{screen.png, serial.log, etw.bin, wm.json}` and the GHA job uploads it (CI side already wired in TODO-05 §15).
- Sample input traces are committed to `tests/traces/` and replayable from kernel tests.
- Snapshots of the framebuffer and terminal grid taken under an active compositor are not torn.
- `TODO-05 §13` multi-monitor matrix rows convert from `[SKIP]` to live runs once virtio-gpu multi-output lands (owner migrates to `08-graphics-ui` when that TODO is filed).
- Shared-host CI no longer exposes the QEMU HMP monitor on a TCP port reachable by other local users.
- `TODO-05 §7` visual-regression workflow scales past the single-scenario boot-per-run shape.

## Implementation Order

| Order | Deliverable                                                                  | Depends On | Status |
| :---: | ---------------------------------------------------------------------------- | ---------- | :----: |
|   1   | Post-desktop-init test harness (`TEST_CAT_DESKTOP_LATE`)                     | --         |  [ ]   |
|   2   | Failure capture hook + retention + replace semantics                         | §1         |  [ ]   |
|   3   | Committed JSONL input traces + replay fixture loader                         | §1         |  [ ]   |
|   4   | Snapshot-time read-side sync (fb + terminal)                                 | §1         |  [ ]   |
|   5   | Virtio-GPU multi-output driver (scope-migration candidate to 08-graphics-ui) | --         |  [ ]   |
|   6   | `\\?\ObjectManager\FrameStats` Ob pseudo-file                                | --         |  [x]   |
|   7   | QEMU HMP monitor UNIX-socket hardening (Linux shared hosts)                  | --         |  [x]   |
|   8   | `scripts/test-visual-regression.sh` shared-session refactor                  | --         |  [x]   |

## 1. Post-Desktop-Init Test Harness

Adds a late-phase test category (`TEST_CAT_DESKTOP_LATE` or a "late" tag on `TEST_CAT_DESKTOP`) that runs AFTER `boot_desktop.c` finishes `terminal_open()` + `wm_create_window()` but before the shell prompts the user. Unblocks the cmd.exe roundtrip test that `TODO-05 §5` had to leave as `TEST_PENDING` and every §15 item that needs a writable VFS or a live compositor.

- [ ] Decide harness shape: (a) new `TEST_CAT_DESKTOP_LATE` category dispatched from `src/kernel/main/boot_desktop.c` after compositor init, (b) second pass of existing `TEST_CAT_DESKTOP` filtered by a "late" tag on each suite, (c) host-driver pattern where `scripts/test-desktop.sh` injects input via `qemu-input.sh sendstring` and reads back through a debug syscall. Pick one; document the trade-off in the commit message.
- [ ] Implement the dispatch site: whichever approach, the late test run must execute with the compositor running, the keyboard IRQ path live, and `vfs_open("X:\\test-artifacts\\<test>\\...")` able to succeed.
- [ ] Port the `TEST_PENDING` cmd.exe `dir` roundtrip case at `src/kernel/test/test_desktop.c:~370` (`test_terminal_dir_roundtrip_synthesized`) to the late category; assert `terminal_get_buffer()` contains the echoed `dir` + at least one directory entry within 500 ms of Enter injection.
- [ ] Ensure `test_desktop_reset()` still fires between late-category suites (or an analogous reset) so each suite starts from a clean WM + terminal + keyboard state.
- [ ] Commit: `"test: post-desktop-init late-phase test harness"`

**Test checkpoint:** `test_terminal_dir_roundtrip_synthesized` reports `[PASS]` on serial (not `[STUB]`). Build log shows late-phase test section ran AFTER the compositor ready line and BEFORE the first user shell prompt.
**Platforms:** QEMU WHPX, QEMU TCG, VBox, bare metal.

## 2. Failure Capture Hook and Artifact Bundle

Consumers of `§1`: when a late-phase test asserts-fails, write a forensic bundle so CI artifact upload (TODO-05 §15, already wired) has something to pick up.

- [ ] `test_desktop_capture_on_fail(const char *test_name)` in `src/kernel/test/test_desktop.c` (or a new `test_artifact.c`) that writes `screen.png` (from `fb_snapshot()` with §4's read-side sync), `serial.log` (from the klog ring tail), `etw.bin` (dump of active ETW sessions), and `wm.json` (from `wm_get_window_count` + per-window rect + focus + pending close queue) into `X:\test-artifacts\<test_name>\`. Called from the `TEST_ASSERT_*` fail path when the current suite is a late-category `TEST_CAT_DESKTOP*` suite.
- [ ] Local retention: keep the most recent 10 failure bundles. On a new failure, if `build/test-artifacts/` has >= 10 entries, delete the oldest. Deterministic ordering by bundle mtime; ties broken by name.
- [ ] Re-running a previously-failed test replaces its prior bundle rather than appending: `vfs_rmdir("X:\\test-artifacts\\<test>")` (recursive) before the first write of the current run.
- [ ] PNG encoder: the kernel has no libpng. Ship a minimal PNG writer (deflate + CRC32 + IHDR/IDAT/IEND) under `src/kernel/test/png_write.c` or reuse miniz's deflate (already vendored for TODO-04 ZIP). Filter 0, 24-bit RGB, no interlacing.
- [ ] ETW dump format: concatenate active-session ring contents in the same on-wire event layout the `etw_emit_kernel_event()` writer produces; consumers re-parse per `include/kernel/etw.h` schema.
- [ ] `wm.json` schema: `{"windows":[{"handle":N,"title":"...","rect":{x,y,w,h},"focused":bool}], "pending_close":[handle,...]}`. Freestanding JSON encoder already exists in `src/kernel/test/input_record.c` (TODO-05 §11); factor it into `kernel/test/json_write.h` when this section lands if the surface grows.
- [ ] Commit: `"test: failure capture hook + retention + replace semantics"`

**Test checkpoint:** Force-fail a canary late-phase test; `build/test-artifacts/<canary>/` contains all 4 files. Re-run the same failing test: only the second run's bundle is present. Run 11 distinct failures: the oldest bundle is gone, 10 remain.
**Platforms:** QEMU WHPX, QEMU TCG (TCG preferred for CI reproducibility).

> [!NOTE]
> GHA artifact upload step already exists in `.github/workflows/build.yml` ("Upload desktop test-artifacts on failure", shipped in `TODO-05 §15`). This section populates the directory the step picks up; no CI-side work is needed.

## 3. Committed JSONL Input Traces

TODO-05 §11 ships the in-memory record + replay surface. The sample traces that a developer would replay to reproduce a bug need a filesystem to live on, which only §1's late-phase harness provides.

- [ ] Record and commit `tests/traces/dir_cmd.input.jsonl` (the "type `dir<Enter>` into the terminal" trace from §11's test checkpoint).
- [ ] Record and commit `tests/traces/cjk_hello.input.jsonl` (CJK IME composition trace; exercises the UTF-8 / IME roundtrip §11 already validates in-memory).
- [ ] Record and commit `tests/traces/drag_resize.input.jsonl` (window move + resize via mouse; drives `wm_mark_dirty` coalesce + `frames_dropped == 0` assertion from §10).
- [ ] Fixture loader: new helper `input_replay_from_file(path, speed_num, speed_den)` that reads the JSONL file via VFS and calls the existing `input_replay_from_jsonl(buf, len, ...)` from TODO-05 §11. Same rational-fraction speed contract.
- [ ] Late-phase test: replay each fixture under `compositor=headless` (TODO-05 §12) and assert `terminal_get_buffer()` / `wm_get_window_count()` / `wm_get_frame_stats()` match the recorded expected state.
- [ ] Commit: `"test: committed JSONL input traces + fixture loader"`

**Test checkpoint:** `make test-desktop` (late phase) runs all three fixtures under headless mode; each completes with the expected post-state assertion green.
**Platforms:** QEMU WHPX, QEMU TCG, VBox (headless compositor mode); bare metal covered by manual replay.

## 4. Snapshot-Time Read-Side Sync

TODO-05 §1 (`fb_snapshot`) and §5 (`terminal_get_buffer`) document a single-threaded / quiesced-caller contract because §15's capture hook is the first caller that runs under an active compositor. Harden both so the capture hook gets non-torn snapshots.

- [ ] `fb_snapshot()` read-side sync in `src/kernel/drivers/framebuffer.c`: add a read-side mutex (or a compositor flush barrier) that `fb_blit` / `fb_put_pixel` / `spinner_advance` honor. Prefer seqlock if readers vastly outnumber writers. Drop the "single-threaded or quiesced caller" caveat in `include/kernel/drivers/framebuffer.h` once the sync is in place.
- [ ] `terminal_get_buffer()` + `terminal_buffer_contains()` read-side barrier in `src/desktop/terminal.c`: same seqlock-or-mutex choice, honored by every `terminal_putchar` / `terminal_puts` writer. Drop the caveat in `include/desktop/terminal.h`.
- [ ] Regression test: under §1's late harness, start a background compositor stress (invalidate 60 times/s) and a background terminal writer (emit 1000 chars/s); call `fb_snapshot()` and `terminal_get_buffer()` 100 times each; assert no caller observes a buffer whose rows cross the torn boundary (i.e., every snapshot is a consistent point-in-time copy).
- [ ] Commit: `"test: fb_snapshot + terminal_get_buffer read-side sync"`

**Test checkpoint:** 100 concurrent-read snapshots under stress are internally consistent (no cross-frame mixing). Frame-stats `frames_dropped` still stays within TODO-05 §10's budget (<= 2 per animation sequence) during the stress loop.
**Platforms:** QEMU WHPX, QEMU TCG, VBox, bare metal.

## 5. Virtio-GPU Multi-Output Driver

Unblocks TODO-05 §13's 5 pending matrix cells. ~1500 LOC of VirtIO 1.2 GPU implementation plus a compositor / framebuffer-manager retrofit.

> [!TIP]
> **Scope boundary:** This section is a scope-migration candidate. If and when `08-graphics-ui` files a dedicated virtio-gpu TODO, migrate this section (and its XREFs in TODO-05 §13 Accepted line) to that file. Keeping it here today centralizes the TODO-05 deferral chain; the true domain home is `08-graphics-ui`.

- [ ] `src/kernel/drivers/virtio_gpu.c` + `include/kernel/drivers/virtio_gpu.h`: VirtIO 1.2 GPU device init (feature negotiation, controlq + cursorq ring setup, per-output display-info probe).
- [ ] Implement `RESOURCE_CREATE_2D`, `RESOURCE_ATTACH_BACKING`, `SET_SCANOUT`, `RESOURCE_FLUSH`, `GET_DISPLAY_INFO` command submission paths.
- [ ] Multi-scanout framebuffer manager: replace the single `back_buf` in `src/kernel/drivers/framebuffer.c` with an N-output array keyed by `output_index`. `fb_get_output_count()` returns the virtio-gpu display count (up to `max_outputs=3` per §13's `boot.conf test_monitors=` matrix). `fb_snapshot_monitor(i)` routes to `back_buf[i]`.
- [ ] Compositor layout across primary + secondary outputs: taskbar on primary only, windows positioned by `boot_config.test_monitors_count`. Ship a minimal placement policy; GNOME-Shell-equivalent multi-monitor layout is out of scope for §5.
- [ ] Flip the 5 `[SKIP]` cells in `scripts/debug/desktop/run-matrix-desktop-tests.bat` to live runs; assert per-output non-black captures.
- [ ] Commit: `"drivers: virtio-gpu multi-output + fb-manager N-output retrofit"`

**Test checkpoint:** `run-matrix-desktop-tests.bat` matrix (3 DPI x 3 monitor counts = 9 cells): all 9 run; 8 pass (1x96 DPI baseline + 8 multi-monitor); `fb_snapshot_monitor(0)` and `fb_snapshot_monitor(1)` both return non-black buffers; taskbar present on primary only.
**Platforms:** QEMU TCG (virtio-gpu emulation path); bare metal covered by manual validation on real VirtIO-GPU-capable hosts.

## 6. ObjectManager FrameStats Pseudo-File

TODO-05 §10 shipped the kernel-side counters + `wm_get_frame_stats()` reader. This section wires it into the Ob namespace so user-mode tools (task manager, future graphs app) can read it without a syscall.

- [x] Shipped a first-time pseudo-file infrastructure: `ObpInfoFileType` + `OB_INFO_FILE` body + `ob_info_file_register` / `ob_info_file_open_handle` / `ob_info_file_read` in `include/kernel/ob/ob_info_file.h` + `src/kernel/ob/ob_info_file.c`. Read callback signature is `int32_t read_fn(uint8_t *buf, uint32_t size, uint32_t offset)`; the shared read path clamps to the declared file size and returns 0 on EOF.
- [x] `\ObjectManager` namespace directory created in `ob_ns_init` alongside `\Device` / `\KernelObjects` / `\DosDevices` / `\BaseNamedObjects`. FrameStats registers via `wm_framestats_register_info_file()` from `wm_init()` (`src/desktop/wm.c`). Read callback takes a seqlock-coherent `wm_get_frame_stats()` snapshot and memcpy-slices the requested window into the caller buffer.
- [x] Schema pinned in `include/kernel/etw.h`: `ETW_WM_FRAME_STATS_LAYOUT_VERSION=1` + `ETW_WM_FRAME_STATS_SIZE=48` + a prose field-offset table. `_Static_assert(sizeof(struct wm_frame_stats) == ETW_WM_FRAME_STATS_SIZE, ...)` in `src/desktop/wm.c` catches any future struct-layout drift at compile time.
- [x] Kernel-side unit tests in `src/kernel/test/test_ob.c` (2 new suites, +20 assertions): `OB: info-file register+read` (duplicate-register guard, null-arg rejection, full 64-byte deterministic pattern roundtrip, partial read at offset, EOF semantics at offset==size and past size, clamping to remaining bytes, bad handle, null buf, unknown name) + `OB: \ObjectManager\FrameStats pseudo-file` (schema size lock-step, pseudo-file read byte-identical to `wm_get_frame_stats()` direct snapshot, EOF at offset == size). Both tests unlink their registered info files via `ObMakeTemporaryObject` + `ObpRemoveFromDirectory` so the per-suite leak tracker sees balanced closes.
- [x] User-mode `CreateFile("\\?\ObjectManager\FrameStats")` path is deferred to a later section: the NT-path -> Ob-namespace routing in NtOpenFile + NtReadFile is a cross-subsystem addition larger than this scope. Kernel-side API (`ob_info_file_open_handle` + `ob_info_file_read`) fully exercises the pseudo-file surface today; the NT routing plugs into the same primitives without changing them.
- [x] Commit: `"ob: expose \\ObjectManager\\FrameStats pseudo-file"`

**Test checkpoint:** Kernel test phase: `OB: info-file register+read` (20 assertions) + `OB: \ObjectManager\FrameStats pseudo-file` (4 assertions) both PASS with 0 leaked bytes. `wm_framestats_register_info_file` is idempotent so wm_init's registration at boot phase 3 does not race with any test-time registration. `_Static_assert` in `wm.c` enforces the etw.h schema pin at compile time.
**Platforms:** QEMU WHPX, QEMU TCG, VBox, bare metal (pure kernel-side data path; no framebuffer or driver dependency).

> **Test runner:** `scripts\debug\kernel\run-ob-tests.bat` (SUITE=ob) | 323 suites, 0 failures (KVM 2026-04-23). New: `OB: info-file register+read` + `OB: \ObjectManager\FrameStats pseudo-file`.
> **Notes:**
> - Shipped: `include/kernel/ob/ob_info_file.h` + `src/kernel/ob/ob_info_file.c` (~170 LOC). New `ObpInfoFileType` sits alongside `ObpFileType` but carries a kernel-supplied read callback + fixed virtual byte size instead of a VFS node. Objects are `OB_FLAG_PERMANENT | OB_FLAG_KERNEL_ONLY`. Creation ref is dropped after a successful namespace insert (mirrors `ob_create_file_handle`) so future `ObMakeTemporaryObject` + `ObpRemoveFromDirectory` correctly frees the body.
> - Wired: `ob_info_file_type_init()` from `ob_init()` before `ob_ns_init()`; `\ObjectManager` directory created in `ob_ns_init`; `wm_framestats_register_info_file()` called from `wm_init()` during phase 3.
> - Schema: `ETW_WM_FRAME_STATS_LAYOUT_VERSION`/`_SIZE` + field-offset comment in `include/kernel/etw.h`. `_Static_assert(sizeof(wm_frame_stats) == ETW_WM_FRAME_STATS_SIZE)` in `src/desktop/wm.c` catches struct drift.
> - 2 new `TEST_CAT_OB` suites (+24 assertions). Both unlink their registered info files in cleanup so per-test leak tracking stays green. The second test directly compares pseudo-file bytes against `wm_get_frame_stats()` -- any future seqlock / struct drift fails the test immediately.
> - Canonical doc: `include/kernel/ob/ob_info_file.h` (read contract + lifecycle); `include/kernel/etw.h` (schema + layout version).
> - Scope boundary: §6 owns the kernel-side pseudo-file primitive + FrameStats registration + schema lock. User-mode NT path (`CreateFile("\\?\ObjectManager\FrameStats")` -> `NtOpenFile` -> Ob namespace -> `NtReadFile` -> `ob_info_file_read`) is a later section / TODO -- kernel-side API already exercises the full read surface.
> **Verified:** 2026-04-23 | 6/6 items | build OK | tests 323/323 kernel + 16/16 user-mode PASS (KVM), 0 leaks
> **Quality reviewed:** pending (quick-win series; follow-up Codex pass after the three quick wins ship)

## 7. QEMU HMP Monitor UNIX-Socket Hardening

`scripts/test-desktop.sh` and manual `-Monitor` consumers of `scripts/machines/run-qemu.ps1` bind the QEMU HMP monitor to `127.0.0.1:<port>` with `server,nowait`. On a shared Linux / WSL host, any local user can connect and issue `quit` or `screendump`. Migrate to UNIX sockets on Linux.

- [x] `scripts/qemu-screenshot.sh`: `--socket <path>` flag + `QEMU_MONITOR_SOCKET` env var. When set, transport switches to `nc -U <path>` and the script verifies the socket exists before piping the `screendump` command. Path is positional-arg-order independent.
- [x] `scripts/machines/run-qemu.ps1`: `-MonitorSocket <path>` switch. When set, emits `-monitor unix:<path>,server,nowait` instead of the telnet TCP form. Auto-enables `-Monitor` (matches the existing `-MonitorPort` UX). Documented as Linux/WSL-only because Windows-native QEMU's UNIX-socket support across the WSL boundary is flaky; Windows keeps TCP by default.
- [x] `scripts/test-desktop.sh`: `uname -s` gates transport selection. `Linux` / `Darwin` -> UNIX socket at `/tmp/qemu-mon-desktop-smoke-$$.sock` (per-PID, no collision); anything else -> TCP fallback. `DESKTOP_MONITOR_TCP=1` forces TCP for debugging. Cleanup trap removes the socket on EXIT/INT/TERM even if QEMU already cleaned its end.
- [x] Socket path convention: `/tmp/qemu-mon-<role>-$$.sock` (test-desktop.sh uses `desktop-smoke` for `<role>`). QEMU creates with the invoking user's uid + 0600 by default, so other local users cannot connect; the trap-based cleanup prevents stale sockets.
- [x] Commit: `"scripts: qemu hmp monitor via unix socket on linux hosts"`

**Test checkpoint:** `scripts/test-desktop.sh` on Linux uses a UNIX socket; `ss -tln | grep 127.0.0.1:44` (or whatever port range was used) finds nothing while the test is running, and `ls -l /tmp/qemu-mon-desktop-smoke-*.sock` shows a 0600-perm socket file owned by the invoking user. Windows runs keep working on TCP via the same `qemu-screenshot.sh` invocation.
**Platforms:** Host-side; Linux + WSL2 + macOS primary, Windows opt-out kept on TCP.

> **Test runner:** N/A (host-side script change) | validation: `bash scripts/test-desktop.sh --accel tcg` on a Linux host shows `monitor=unix` in the boot line; `lsof -p $(pgrep -f qemu-system-x86_64)` lists the UNIX socket and no TCP listener on the per-PID port.
> **Notes:**
> - `qemu-screenshot.sh`: `--socket <path>` (or `QEMU_MONITOR_SOCKET` env) routes the HMP `screendump` command through `nc -U` and validates the socket file exists before piping. TCP path unchanged when neither is set.
> - `test-desktop.sh`: per-PID UNIX socket at `/tmp/qemu-mon-desktop-smoke-$$.sock` is the default on Linux/macOS; per-PID TCP port (44000+pid%1000) is the fallback on Windows/Cygwin and when `DESKTOP_MONITOR_TCP=1`. Cleanup trap removes the socket on every exit path.
> - `run-qemu.ps1`: `-MonitorSocket <path>` switch added; auto-enables `-Monitor`. Windows-native QEMU support for UNIX sockets across the WSL boundary is flaky; the switch exists for users who know they want it (PowerShell Core on Linux, or QEMU builds with named-pipe-as-socket support).
> - Hardening result: shared-host CI no longer exposes the HMP monitor on a 127.0.0.1 TCP port reachable by any other local user. The socket is created with the invoking user's uid + 0600 perms, so only that user can issue HMP commands.
> - Canonical doc: `scripts/qemu-screenshot.sh` header (env vars + flag); `scripts/test-desktop.sh` security note block.
> - Scope boundary: §7 hardens HMP monitor exposure for the existing test-desktop + qemu-screenshot stack. §8 (visual-regression refactor) and TODO-05 §7 (visual-regression workflow) consume the same hardening when they invoke `test-desktop.sh`; no extra changes needed there.
> **Verified:** 2026-04-23 | 5/5 items | build OK (script-only) | manual smoke validated

## 8. Visual Regression Shared-Session Refactor

`scripts/test-visual-regression.sh` delegates every scenario to `scripts/test-desktop.sh`, which boots a fresh QEMU instance per scenario. Fine for the current single `idle` scenario; linear-in-count for the §11 record/replay cases and §13 multi-monitor cases once those land in CI.

- [x] Refactored `scripts/test-visual-regression.sh` around `session_start()` / `session_stop()` helpers that boot QEMU once with a per-PID UNIX socket (`/tmp/qemu-mon-visual-reg-$$.sock`), wait for DESKTOP_READY, and host all scenario captures against the shared socket via `qemu-screenshot.sh --socket`. Single EXIT/INT/TERM trap guarantees teardown + socket cleanup.
- [x] `scenario_needs_fresh_boot()` function declares per-scenario isolation need. Any scenario that returns 0 (i.e. needs fresh boot) triggers the whole run to fall back to `scenario_capture_fresh()` (test-desktop.sh delegation) so shared-session state never bleeds into a scenario that demanded isolation. Today all scenarios are shared-session-friendly; future scenarios that touch non-idempotent state just flip the case-arm.
- [x] `scenario_setup()` hook runs BETWEEN scenario captures against the shared session via `qemu-input.sh sendstring`/`sendkey` over the same socket. Idle is a no-op. Framework in place; future scenarios add case arms.
- [x] Baseline preserved: `SCENARIO_COUNT <= 1` routes through `scenario_capture_fresh()` which keeps the existing test-desktop.sh invocation verbatim. `--update-refs` also forces fresh-boot per scenario so committed baselines are never tainted by cross-scenario state. `VR_FORCE_FRESH=1` debug override available.
- [x] Commit: `"scripts: visual-regression shared-session capture loop"`

**Test checkpoint:** `scripts/test-visual-regression.sh --scenarios "idle idle"` shows one `DESKTOP_READY` line + two capture blocks (shared session). `scripts/test-visual-regression.sh` (single scenario, default) shows the existing test-desktop.sh path unchanged. `VR_FORCE_FRESH=1` or a needs_fresh_boot scenario reverts to per-scenario boot. All paths clean up socket + OVMF_VARS on EXIT/INT/TERM.
**Platforms:** Host-side.

> **Test runner:** N/A (host-side script change) | validation: `bash scripts/test-visual-regression.sh --accel tcg --scenarios "idle idle"` confirms one QEMU boot + N captures over the shared socket; single-scenario `idle` run preserves test-desktop.sh delegation; `ls /tmp/qemu-mon-visual-reg-*.sock` empty after run.
> **Notes:**
> - Shipped `session_start()` + `session_stop()` helpers that inline the boot / DESKTOP_READY-wait / teardown sequence from test-desktop.sh against a per-PID UNIX socket (`/tmp/qemu-mon-visual-reg-$$.sock`). Single trap on EXIT/INT/TERM guarantees cleanup.
> - Scenario table split into `scenario_capture_fresh()` (fresh VM per scenario, used for single-scenario / update-refs / needs_fresh_boot / VR_FORCE_FRESH paths) and `scenario_capture_shared()` (uses the running session via `qemu-screenshot.sh --socket`, with `scenario_setup()` called first for input-driven scenarios).
> - `qemu-input.sh` gained `--socket <path>` / `QEMU_MONITOR_SOCKET` support mirroring the §7 hardening in `qemu-screenshot.sh`; the shared-session setup hook invokes it over the same socket so cross-user HMP access stays blocked.
> - Baseline preserved: `SCENARIOS="idle"` (default) runs `scenario_capture_fresh` exactly as before -- CI wall-time is identical for the one-scenario case today. Multi-scenario runs pay the boot cost once instead of N times.
> - Canonical doc: `scripts/test-visual-regression.sh` header (session model + scenarios table); `scripts/qemu-input.sh` header (socket env var).
> - Scope boundary: §8 owns the host-side test driver. It does NOT extend the scenarios list (still just `idle` today); new scenarios land alongside §11 record/replay fixtures (TODO-14 §3) and §13 multi-monitor work. §7 hardening is an upstream dependency -- shared-session relies on the UNIX-socket transport.
> **Verified:** 2026-04-23 | 5/5 items | build OK (script-only) | manual: single-scenario baseline PASS + shared-session 2-scenario run confirmed (one DESKTOP_READY, two captures)

## OS Comparison

| ⭐  | Feature                            | 🪟 Win11                                | 🐧 Linux                        | 🚀 Impossible OS                         |
| --- | ---------------------------------- | --------------------------------------- | ------------------------------- | ---------------------------------------- |
| ⭐  | Late-phase kernel test hook        | ❌ Internal WTT only                    | ⚠️ KUnit late init only         | ⚠️ §1 planned                            |
| ⭐  | Crash artifact bundle on test fail | ⚠️ ad hoc per team                      | ⚠️ ad hoc per team              | ⚠️ §2 planned (CI upload wired today)    |
| ⭐  | Committed input traces per-fixture | ❌ PSR deprecated                       | ✅ libinput record samples      | ⚠️ §3 planned                            |
| 💎  | Snapshot-time reader sync          | ✅ DWM quiesce                          | ✅ wlroots frame barrier        | ⚠️ §4 planned (single-threaded today)    |
| 💎  | Virtio-GPU multi-output            | ⚠️ Hyper-V virtual display              | ✅ virtio-gpu + KMS             | ⚠️ §5 planned (matrix rows SKIPped)      |
| ⭐  | OS-level frame-stats pseudo-file   | ❌ DwmGetCompositionTimingInfo API only | ❌ no pseudo-file               | ✅ Done §6 \ObjectManager\FrameStats     |
| 💎  | CI monitor socket security         | N/A (vmconnect.exe)                     | ✅ QEMU `-monitor unix:` common | ✅ Done §7 unix sock + 0600 perms        |
| ⭐  | Shared-session visual regression   | ⚠️ Playwright-style per tool            | ⚠️ openQA per-test VM default   | ✅ Done §8 amortized boot + fresh opt-in |

## Unit Tests

> [!NOTE]
> Every section above defines its own test checkpoint; the unit-test targets below mirror those checkpoints as concrete kernel-side assertions. The late-phase harness (§1) must land before §2-§4 tests can run; §5-§8 tests are independent.

- [ ] `test_late_phase_dispatch_runs_after_compositor` (§1): assert the late category runs AFTER `compositor_run()` reaches its event loop and BEFORE the shell prompt appears on serial.
- [ ] `test_terminal_dir_roundtrip_real` (§1): inject `dir\n` via keyboard; within 500 ms `terminal_get_buffer()` contains the literal echo + at least one dir entry. (Replaces the §5 `TEST_PENDING` case.)
- [ ] `test_artifact_bundle_contents` (§2): force-fail a canary late suite; `build/test-artifacts/<canary>/` has `screen.png` (>= PNG magic + IHDR), `serial.log` (non-empty), `etw.bin` (parseable per schema), `wm.json` (parseable, at least one window).
- [ ] `test_artifact_retention_trims_oldest` (§2): synthesize 11 bundle dirs with increasing mtimes; run the retention trim; assert exactly 10 remain and the oldest is gone.
- [ ] `test_artifact_replace_on_rerun` (§2): write a bundle for `<t>`; re-run `<t>`; only the second bundle is present.
- [ ] `test_input_fixture_dir_cmd_replay` (§3): `input_replay_from_file("tests/traces/dir_cmd.input.jsonl", 0, 1)` under headless; post-state assertion green.
- [ ] `test_input_fixture_cjk_hello_replay` (§3): same for the CJK fixture.
- [ ] `test_input_fixture_drag_resize_replay` (§3): same for the drag-resize fixture; additionally assert `frames_dropped <= 2`.
- [ ] `test_fb_snapshot_stress_consistent` (§4): 100 snapshots under compositor stress are internally consistent.
- [ ] `test_terminal_buffer_stress_consistent` (§4): 100 reads under concurrent writer are internally consistent.
- [ ] `test_virtio_gpu_outputs_count` (§5): `fb_get_output_count()` returns the value reported by `GET_DISPLAY_INFO` on a 2-output QEMU command line.
- [ ] `test_virtio_gpu_per_output_nonblack` (§5): `fb_snapshot_monitor(0)` and `fb_snapshot_monitor(1)` both return non-black; taskbar on output 0 only.
- [ ] `test_ob_framestats_pseudo_file_read` (§6): open `\\?\ObjectManager\FrameStats`, read `sizeof(wm_frame_stats)` bytes, assert `frames_presented > 0`.
- [ ] `test_qemu_monitor_unix_socket_linux` (§7): host-side shell test; `scripts/test-desktop.sh` on Linux binds a UNIX socket (`lsof` confirms); no TCP monitor port open while running.
- [ ] `test_visual_regression_shared_session` (§8): synthesize 3 scenarios; run the script; assert the boot-phase marker on serial appears exactly once, not 3 times.
- [ ] Register late-phase cases via a new `test_suite_register_cat("desktop_late", fn, TEST_CAT_DESKTOP_LATE)` in `src/kernel/test/test_desktop_late.c` (created by §1).

## Verification

- [ ] `make test-desktop` runs the existing `TEST_CAT_DESKTOP` phase-3 cases + the new `TEST_CAT_DESKTOP_LATE` cases; all green on KVM + TCG.
- [ ] `make test-visual-regression` drives the refactored shared-session script across >= 2 scenarios; wall time scales sub-linearly.
- [ ] `bash scripts/test-smoke.sh` still passes (late-phase harness must not break the baseline boot).
- [ ] Deliberately fail a canary late suite; inspect `build/test-artifacts/<canary>/` contents manually; GHA upload populates a non-empty artifact.
- [ ] `run-matrix-desktop-tests.bat` matrix reports 9/9 cells running (not 1/6) after §5 lands.
- [ ] Commit: `"test: desktop late-phase harness + artifact bundle + virtio-gpu multi-output complete"`

**Test runner:** `scripts\debug\desktop\run-desktop-tests.bat` (SUITE=desktop) | covers §1-§4 + §6 late-phase cases; §5 matrix via `run-matrix-desktop-tests.bat`; §7/§8 host-side validation via `scripts/test-desktop.sh` + `scripts/test-visual-regression.sh`.
