/* ============================================================================
 * boot_tests.c -- Boot-time verification tests
 *
 * Dispatches to the test_runner framework for unit tests, then runs
 * debug=1-only integration tests (IXFS perf, directory dump, timer).
 * ============================================================================ */

#include "kernel/types.h"
#include "kernel/klog.h"
#include "kernel/timer.h"
#include "kernel/fs/vfs.h"
#include "kernel/fs/ixfs.h"
#include "kernel/boot_splash.h"
#include "kernel/boot_info.h"
#include "kernel/boot_media.h"
#include "kernel/test/test.h"
#include "kernel/test/test_usermode.h"
#include "main/main_internal.h"

void boot_tests_run(void)
{
    /* test=1 or debug=1: run unit tests, then continue booting to desktop. */
    if (g_boot_info.config.test || g_boot_info.config.debug) {
        /* Per-aggregate test_*_skip knobs follow-up (see TODO-04):
         * test_kernel_skip=1   skips the kernel TEST_CAT_* sweep so a
         *                      usermode-only iteration bat (e.g.
         *                      run-all-usermode-tests.bat or any
         *                      run-test_<name>.bat) does not pay for it.
         * test_usermode_skip=1 skips test_usermode_run() so the kernel
         *                      aggregate (run-all-kernel-tests.bat) is
         *                      genuinely kernel-only and the cross-layer
         *                      sweep (run-all-tests.bat) does not run
         *                      every test_*.exe twice.
         * Default 0/0 = back-compat (every `test=1` run does both halves
         * sequentially). debug=1 always runs both halves regardless --
         * that mode is the human "show me everything" path. */
        int run_kernel_tests   = g_boot_info.config.debug ||
                                 !g_boot_info.config.test_kernel_skip;
        int run_usermode_tests = g_boot_info.config.debug ||
                                 !g_boot_info.config.test_usermode_skip;

        /* On slow USB media, skip the (long) kernel test sweep in debug mode to keep
         * the boot usable -- but HONOR an explicit `test=1` (the user is testing on
         * the slow target on purpose). Two independent slow-media signals are honored:
         * the proactive probe (boot_media_speed) and the reactive klog detector
         * (klog_slow_media_detected, armed when a disk-log flush exceeds the slow
         * threshold). The reactive path covers boots where the probe never landed a
         * full 4 KiB read and stayed UNKNOWN, so a slow USB target still skips. */
        if (run_kernel_tests && !g_boot_info.config.test &&
            (boot_media_speed() == BOOT_MEDIA_SLOW || klog_slow_media_detected())) {
            klog(LOG_INFO, "boot",
                 "Slow boot media -- skipping kernel test sweep (debug mode)");
            run_kernel_tests = 0;
        }

        /* Apply category filter and quiet mode from boot.conf */
        uint8_t suite_val = g_boot_info.config.test_suite;
        if (suite_val < TEST_CAT_COUNT)
            test_runner_set_filter((test_category_t)suite_val);

        /* test_quiet is a small enum, not a flag -- the bootloader has always
         * read it with ascii_atoi(), so TEST_QUIET_COUNT_TRACE (2) adds a mode
         * without adding a field or touching BOOT_INFO_VERSION. 2 means "quiet
         * PASS lines AND emit the per-suite [COUNT] trace": the trace costs
         * real serial I/O, so it is opt-in, and it rides the QUIET path
         * because a verbose run does not finish inside the harness timeout. */
        if (g_boot_info.config.test_quiet >= TEST_QUIET_ON)
            test_runner_set_quiet(1);
        if (g_boot_info.config.test_quiet >= TEST_QUIET_COUNT_TRACE)
            test_runner_set_count_trace(1);

        /* Surface skip decisions BEFORE the LOG_WARN filter clamps
         * non-TEST output -- otherwise the LOG_INFO klog below is
         * suppressed exactly when the operator most needs to see why
         * a `test=1` boot ran zero suites. */
        if (g_boot_info.config.test) {
#ifndef KERNEL_TESTS
            /* Third zero-suite reason, and the only COMPILE-time one: the
             * release flavor pruned src/kernel/test/ and stubbed the runner
             * entry points, so every call below is a no-op. Without this the
             * operator gets a silent `test=1` boot -- the same signature as
             * the 2026-04-21 disk-sourced-config incident. */
            klog(LOG_WARN, "TEST",
                 "Test surface compiled out of this image (KERNEL_TESTS=off)");
#endif
            if (!run_kernel_tests)
                klog(LOG_WARN, "TEST",
                     "Kernel TEST_CAT_* sweep skipped (test_kernel_skip=1)");
            if (!run_usermode_tests)
                klog(LOG_WARN, "TEST",
                     "User-mode launcher skipped (test_usermode_skip=1)");
        }

        /* Suppress non-TEST boot chatter during test mode.
         * All three test-runner subsystems (kernel TEST, user-mode UTEST,
         * desktop DTEST) need the LOG_DEBUG override so their LOG_INFO
         * per-assertion lines are not dropped by the global LOG_WARN
         * ceiling. Missing DTEST override was a real bug surfaced when
         * `SUITE=desktop` filtered the run: 17 assertion lines emitted
         * silently because only TEST had the override. */
        if (g_boot_info.config.test) {
            klog_set_level((const char *)0, LOG_WARN);
            klog_set_level("TEST",  LOG_DEBUG);
            klog_set_level("UTEST", LOG_DEBUG);
            klog_set_level("DTEST", LOG_DEBUG);
        }

        if (run_kernel_tests) {
            test_runner_init();
            test_runner_run();
        }

        /* Restore normal logging so remaining boot output is visible */
        klog_set_level((const char *)0, LOG_DEBUG);

        /* User-mode test launcher: runs every test_*.exe deployed at
         * C:\ root sequentially in its own task. No-op if C:\ is not
         * mounted or no matching binaries exist. Runs AFTER the
         * kernel test runner so the [TEST] summary lands before
         * [UTEST] lines on serial -- matches scripts/test.sh parser
         * expectations (kernel TEST first, user-mode UTEST second).
         *
         * S4 knobs from boot.conf get pushed into the launcher here
         * so the launcher itself stays decoupled from g_boot_info --
         * the kernel tests in test_boot_init can drive the API
         * directly without setting up a fake config. */
        if (g_boot_info.config.test && run_usermode_tests) {
            if (g_boot_info.config.utest_filter[0])
                test_usermode_set_filter(g_boot_info.config.utest_filter);
            if (g_boot_info.config.utest_timeout_ms)
                test_usermode_set_timeout_ms(
                    g_boot_info.config.utest_timeout_ms);
            if (g_boot_info.config.tap)
                test_usermode_set_tap(1);
            /* Per-test isolation defaults ON; honour an explicit 0 in
             * boot.conf as the only way to disable it. Matches the
             * user-visible "production runs always isolate" contract. */
            if (g_boot_info.config.utest_isolation == 0)
                test_usermode_set_isolation(0);
            if (g_boot_info.config.xml)
                test_usermode_set_xml(1);
            if (g_boot_info.config.json)
                test_usermode_set_json(1);
            if (g_boot_info.config.stress_iters)
                test_usermode_set_stress_iters(
                    g_boot_info.config.stress_iters);
            test_usermode_run();
        }
    }

    /* debug=0 and test=0: skip everything below */
    if (!g_boot_info.config.debug && !g_boot_info.config.test) {
        klog(LOG_DEBUG, "boot", "Boot tests skipped (debug=0)");
        return;
    }

    /* test=1 without debug=1: skip integration tests, continue to desktop */
    if (g_boot_info.config.test && !g_boot_info.config.debug) {
        boot_splash_status("Preparing desktop...");
        return;
    }

    boot_splash_status("Running boot tests...");
    klog_disk_flush();

    /* ---- debug=1-only integration tests below ---- */

    /* On USB boot media the I/O-heavy IXFS tests (CRUD delete + the
     * hundreds-of-writes performance suite) can stall the boot for many seconds,
     * so reduce them to the read-path subset. Proactive signal: the boot_media
     * speed probe classified MEDIUM (USB 3.0) or SLOW (USB 2.0). Reactive signal:
     * klog_slow_media_detected() trips when a disk-log flush exceeded the slow
     * threshold -- covers boots where the probe never landed a full 4 KiB read and
     * stayed UNKNOWN. FAST (SSD/NVMe) runs the full suite. */
    int reduced_io = boot_media_is_usb_class(boot_media_speed()) ||
                     klog_slow_media_detected();
    klog(LOG_DEBUG, "TEST", reduced_io
         ? "test: IXFS tests: reduced (USB boot media)"
         : "test: IXFS tests: full suite (fast media)");

    /* VFS smoke test: read a known file */
    if (vfs_is_mounted('C')) {
        struct vfs_node *f = vfs_open("C:\\hello.txt", VFS_O_READ);
        if (f) {
            uint8_t buf[128];
            int n = vfs_read(f, 0, sizeof(buf) - 1, buf);
            if (n > 0) {
                buf[n] = '\0';
                klog(LOG_DEBUG, "TEST", "VFS read C:\\hello.txt: \"%s\"", (char *)buf);
            }
            vfs_close(f);
        }
    }

    /* IXFS CRUD demo -- create/write/read/delete + directory create/rmdir, the
     * write-heavy part of the boot tests (each step is a cache-deferred IXFS write).
     * On USB boot media the WHOLE block is skipped so a reduced boot avoids the
     * multi-second write stall and leaves no test artifact behind. The VFS read
     * smoke test above + the directory dump below still run as the read-path checks;
     * a read touches i_atime (one inode write), far below the slow-media freeze
     * threshold the bulk-write tests hit.
     * KERNEL_TESTS-gated: this block creates/overwrites/unlinks C:\test.txt
     * and C:\TestDir gated only on debug=1 (boot.conf), which can originate
     * from a mutable ESP on some boot-media layouts -- a release image must
     * not run destructive filesystem mutation from that signal alone. */
#ifdef KERNEL_TESTS
    if (!reduced_io && vfs_is_mounted('C')) {
        struct vfs_node *c_root = vfs_get_drive_root('C');
        if (c_root && c_root->ops && c_root->ops->create) {
            c_root->ops->create(c_root, "test.txt", 0);
            struct vfs_node *tf = vfs_open("C:\\test.txt", VFS_O_WRITE);
            if (tf) {
                const char *tdata = "IXFS is working!";
                vfs_write(tf, 0, 16, (const uint8_t *)tdata);
                vfs_close(tf);

                tf = vfs_open("C:\\test.txt", VFS_O_READ);
                if (tf) {
                    uint8_t rbuf[64];
                    int n = vfs_read(tf, 0, 63, rbuf);
                    if (n > 0) {
                        rbuf[n] = '\0';
                        klog(LOG_DEBUG, "TEST",
                             "IXFS CRUD: C:\\test.txt = \"%s\"", (char *)rbuf);
                    }
                    vfs_close(tf);
                }

                if (c_root->ops->unlink) {
                    c_root->ops->unlink(c_root, "test.txt");
                    klog(LOG_DEBUG, "TEST", "IXFS delete: C:\\test.txt removed");
                }
            }

            c_root->ops->create(c_root, "TestDir", VFS_DIRECTORY);
            if (c_root->ops->unlink) {
                c_root->ops->unlink(c_root, "TestDir");
                klog(LOG_DEBUG, "TEST", "IXFS rmdir: C:\\TestDir removed");
            }
        }
    }
#endif /* KERNEL_TESTS */

    /* IXFS performance features test (hash index + snapshot + scrub = hundreds of
     * writes) -- skipped on USB media to avoid a multi-second boot stall.
     * KERNEL_TESTS-gated (release-flavor test-surface exclusion): defined in ixfs_test.c,
     * pruned from the release flavor. This whole function only reaches
     * here under debug=1 (boot.conf, admin-controlled), so the guard is
     * purely about not linking a dropped TU's symbol, not attacker reach. */
#ifdef KERNEL_TESTS
    if (!reduced_io)
        ixfs_test_performance();
#endif

    /* Directory tree dump -- dump_dir_tree() logs per entry, and in live-log mode
     * each klog flushes to disk, so a populated volume is O(entries) small writes.
     * Skip on USB media (reduced_io) so the dump's write cost cannot reintroduce the
     * boot stall the CRUD/perf skips just removed. */
    if (!reduced_io && klog_disk_live_active()) {
        klog(LOG_DEBUG, "TEST", "------------------------------------------------------------------------");
        klog(LOG_DEBUG, "TEST", "--- C:\\ Directory Tree ---");
        if (vfs_is_mounted('C')) {
            struct vfs_node *cr = vfs_get_drive_root('C');
            if (cr) {
                uint32_t count = dump_dir_tree("C:", cr, 0);
                klog(LOG_DEBUG, "TEST", "(%u entries total)", (uint64_t)count);
            }
        }
    }

    /* Timer verification */
    boot_splash_status("Timer test (1s sleep)...");
    klog(LOG_DEBUG, "TEST", "Timer: sleeping 1 second...");
    sleep_ms(1000);
    klog(LOG_DEBUG, "TEST", "Timer OK (ticks: %u, uptime: %u sec)",
         system_get_ticks(), uptime());

    klog(LOG_DEBUG, "TEST", "User mode / exec / fork tests skipped");

    boot_splash_status("Preparing desktop...");
    boot_splash_tick();
}
