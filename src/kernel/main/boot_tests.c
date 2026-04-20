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
#include "kernel/test/test.h"
#include "kernel/test/test_usermode.h"
#include "main/main_internal.h"

void boot_tests_run(void)
{
    /* test=1 or debug=1: run unit tests, then continue booting to desktop. */
    if (g_boot_info.config.test || g_boot_info.config.debug) {
        /* Apply category filter and quiet mode from boot.conf */
        uint8_t suite_val = g_boot_info.config.test_suite;
        if (suite_val < TEST_CAT_COUNT)
            test_runner_set_filter((test_category_t)suite_val);

        if (g_boot_info.config.test_quiet)
            test_runner_set_quiet(1);

        /* Suppress non-TEST boot chatter during test mode */
        if (g_boot_info.config.test) {
            klog_set_level((const char *)0, LOG_WARN);
            klog_set_level("TEST", LOG_DEBUG);
        }

        test_runner_init();
        test_runner_run();

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
        if (g_boot_info.config.test) {
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

    /* IXFS CRUD demo */
    if (vfs_is_mounted('C')) {
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

    /* IXFS performance features test */
    ixfs_test_performance();

    /* Directory tree dump */
    if (klog_disk_live_active()) {
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
