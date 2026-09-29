<!-- docs: covers=todo/09-desktop-shell/TODO-14-desktop-test-late-phase-harness.md sources=src/kernel/test/test_desktop.c,include/kernel/drivers/framebuffer.h,src/kernel/drivers/framebuffer.c,include/desktop/terminal.h,src/desktop/wm.c,src/kernel/ob/ob_info_file.c,scripts/test-desktop.sh,scripts/test-visual-regression.sh,.github/workflows/visual-regression.yml,.github/workflows/build.yml reviewed=2026-09-29T19:00 order=20 -->
# Desktop Test Late-Phase Harness

## What is it?

This roadmap extends the desktop test framework so tests can run against the live desktop, after the compositor, terminal and shell have started, instead of only in the kernel test phase before them. It also plans a bundle of artifacts saved when a desktop test fails, recorded input traces, lock-safe screen snapshots, a multi-output virtio-gpu driver for multi-monitor tests, and three CI and tooling pieces. Three of its eight sections have shipped: the frame statistics pseudo-file, the QEMU monitor over a UNIX socket, and a shared-session visual regression run.

## How does it work?

**Today: tests run before the desktop.** The desktop suites in [`test_desktop.c`](../../src/kernel/test/test_desktop.c) run under `TEST_CAT_DESKTOP` in the kernel test phase, before the window manager starts `cmd.exe`. The terminal `dir` test therefore types `dir` into a forced-open terminal and feeds the expected echo itself, rather than driving the real shell. The framework is described in [Desktop and UI Test Framework](../infrastructure/desktop-ui-test-framework.md).

**Shipped from this roadmap.**

- **Frame statistics pseudo-file (section 6).** The window manager registers `\ObjectManager\FrameStats`, a read-only file that returns the 48-byte `struct wm_frame_stats` ([`wm.c`](../../src/desktop/wm.c), on the info-file helper in [`ob_info_file.c`](../../src/kernel/ob/ob_info_file.c)). Tests read it through the object manager; reading it from user mode with `CreateFile` is not wired yet.
- **Monitor socket (section 7).** [`test-desktop.sh`](../../scripts/test-desktop.sh) talks to the QEMU monitor over a per-run UNIX socket instead of a TCP port on Linux hosts (`DESKTOP_MONITOR_TCP=1` restores TCP), and the screenshot, input and QEMU launch scripts accept a socket path.
- **Shared-session visual regression (section 8).** [`test-visual-regression.sh`](../../scripts/test-visual-regression.sh) boots once and captures every scenario in that session, booting fresh only for scenarios that need it or when `VR_FORCE_FRESH` is set. Only the idle desktop scenario exists, and no reference images are committed yet, so the [Visual Regression workflow](../../.github/workflows/visual-regression.yml) is advisory.

**What exists toward the open sections.** `fb_snapshot()` copies the framebuffer without a lock and documents that it can tear, and the terminal's buffer reads carry the same caveat ([`framebuffer.h`](../../include/kernel/drivers/framebuffer.h), [`terminal.h`](../../include/desktop/terminal.h)). `fb_get_output_count()` always returns 1 and `fb_snapshot_monitor()` serves that one output ([`framebuffer.c`](../../src/kernel/drivers/framebuffer.c)). The [build workflow](../../.github/workflows/build.yml) already uploads `build/test-artifacts/` when a run fails, but nothing writes to that folder yet.

**Planned design.**

1. **Late-phase harness.** A test category dispatched after the compositor, terminal and shell are up, starting with a real `cmd.exe` `dir` round trip.
2. **Failure capture.** On failure, save `screen.png`, the serial log, the event trace and the window list under `X:\test-artifacts\<test>\`, keeping the last ten.
3. **Input traces.** Committed JSONL traces (typing `dir`, CJK input, drag and resize) replayed from a file.
4. **Snapshot sync.** A lock or sequence counter so snapshots and terminal reads never tear, with a stress test.
5. **Virtio-GPU multi-output.** A driver with several outputs, so the multi-monitor test matrix stops skipping.

## What are its interfaces?

| Interface | Status |
| --- | --- |
| `\ObjectManager\FrameStats` | Shipped |
| `test-desktop.sh` UNIX-socket monitor, `--socket` in the QEMU helper scripts | Shipped |
| `test-visual-regression.sh` shared session, `VR_FORCE_FRESH` | Shipped |
| `fb_snapshot()`, `fb_snapshot_monitor()`, `fb_get_output_count()` | Shipped; one output, no lock |
| Late-phase test category, artifact bundle, trace replay from file, virtio-gpu | Planned |

## How do I use it?

Run the desktop smoke test with `bash scripts/test-desktop.sh`; it prints `monitor=unix` or `monitor=tcp` for the transport it chose. Run the visual regression pass with `make test-visual` and seed reference images with `make update-ui-refs`. The kernel desktop suites, which run before the desktop starts, are `make test-desktop` (the same as `bash scripts/test.sh SUITE=desktop`).

## What is not implemented yet?

- [Post-Desktop-Init Test Harness](../../todo/09-desktop-shell/TODO-14-desktop-test-late-phase-harness.md#1-post-desktop-init-test-harness)
- [Failure Capture Hook and Artifact Bundle](../../todo/09-desktop-shell/TODO-14-desktop-test-late-phase-harness.md#2-failure-capture-hook-and-artifact-bundle)
- [Committed JSONL Input Traces](../../todo/09-desktop-shell/TODO-14-desktop-test-late-phase-harness.md#3-committed-jsonl-input-traces)
- [Snapshot-Time Read-Side Sync](../../todo/09-desktop-shell/TODO-14-desktop-test-late-phase-harness.md#4-snapshot-time-read-side-sync)
- [Virtio-GPU Multi-Output Driver](../../todo/09-desktop-shell/TODO-14-desktop-test-late-phase-harness.md#5-virtio-gpu-multi-output-driver)
- Reference images for visual regression, and scenarios beyond the idle desktop, from the [Desktop and UI Test Framework roadmap](../../todo/00-infrastructure/TODO-05-desktop-ui-test-framework.md)

## How does it compare with Windows 11 and Linux?

Windows has no public late-boot kernel test hook, and its frame timing is exposed only through the `DwmGetCompositionTimingInfo` API; the Desktop Window Manager quiesces before snapshots. Linux has KUnit for late init tests, libinput recordings as input traces, virtio-gpu with KMS for multiple virtual outputs, and QEMU's UNIX monitor sockets are common in CI; openQA boots a VM per test by default. Impossible OS already exposes frame statistics as a file and shares one boot across visual scenarios. Running tests against the live shell and capturing failure artifacts are still planned.

## See also

- [Desktop Test Late-Phase Harness roadmap](../../todo/09-desktop-shell/TODO-14-desktop-test-late-phase-harness.md)
- [Desktop and UI Test Framework](../infrastructure/desktop-ui-test-framework.md)
- [Object Manager](../kernel/object-manager.md)
- [Desktop Compositor](compositor.md)
- [Test coverage](../test-coverage/coverage.md)
