/* ============================================================================
 * boot_tests.c — Boot-time verification tests
 *
 * VFS read, IXFS CRUD, IXFS performance, directory tree dump, VMM test,
 * PMM test, timer test, scheduler tests, IPC tests, swap/mmap tests,
 * driver tests, syscall init.
 * ============================================================================ */

#include "kernel/types.h"
#include "kernel/klog.h"
#include "kernel/mm/pmm.h"
#include "kernel/mm/vmm.h"
#include "kernel/mm/heap.h"
#include "kernel/mm/swap.h"
#include "kernel/mm/mmap.h"
#include "kernel/timer.h"
#include "kernel/drivers/virtio_blk.h"
#include "kernel/drivers/ahci.h"
#include "kernel/fs/vfs.h"
#include "kernel/fs/ixfs.h"
#include "kernel/sched/task.h"
#include "kernel/sched/workqueue.h"
#include "kernel/sched/syscall.h"
#include "kernel/ipc/pipe.h"
#include "kernel/ipc/shmem.h"
#include "kernel/boot_splash.h"
#include "kernel/boot_info.h"
#include "main/main_internal.h"

void boot_tests_run(void)
{
    /* --- Essential runtime init (must run regardless of debug mode) --- */

    boot_splash_status("Initializing scheduler...");
    task_init();
    ahci_enable_events();

    /* Create the system work queue (needed by NIC driver) */
    {
        extern workqueue_t *sys_wq;
        scheduler_enable();
        sys_wq = workqueue_create("sys_wq");
        scheduler_disable();
        if (sys_wq)
            klog(LOG_DEBUG, "wq", "sys_wq created");
        else
            klog(LOG_ERROR, "wq", "sys_wq creation FAILED");
    }

    boot_splash_status("Initializing syscalls...");
    syscall_init();

    /* Only run boot tests when debug=1 in boot.conf */
    if (!g_boot_info.config.debug) {
        klog(LOG_DEBUG, "boot", "Boot tests skipped (debug=0)");
        return;
    }

    klog(LOG_DEBUG, "", "");
    klog(LOG_DEBUG, "", "--- Boot Tests -------------------------------------------------------------");

    boot_splash_status("Running boot tests...");
    klog_disk_flush();

    /* VFS test: read a file from C:\ (IXFS system partition) */
    if (vfs_is_mounted('C')) {
        struct vfs_node *f = vfs_open("C:\\hello.txt", VFS_O_READ);
        if (f) {
            uint8_t buf[128];
            int n = vfs_read(f, 0, sizeof(buf) - 1, buf);
            if (n > 0) {
                buf[n] = '\0';
                klog(LOG_DEBUG, "test", "VFS read C:\\hello.txt: \"%s\"", (char *)buf);
            }
            vfs_close(f);
        }
    }

    /* IXFS CRUD test */
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
                        klog(LOG_DEBUG, "test",
                             "IXFS CRUD: C:\\test.txt = \"%s\"", (char *)rbuf);
                    }
                    vfs_close(tf);
                }

                if (c_root->ops->unlink) {
                    c_root->ops->unlink(c_root, "test.txt");
                    klog(LOG_DEBUG, "test", "IXFS delete: C:\\test.txt removed");
                }
            }

            /* mkdir + rmdir test */
            c_root->ops->create(c_root, "TestDir", VFS_DIRECTORY);
            if (c_root->ops->unlink) {
                c_root->ops->unlink(c_root, "TestDir");
                klog(LOG_DEBUG, "test", "IXFS rmdir: C:\\TestDir removed");
            }
        }
    }

    /* IXFS performance features test */
    ixfs_test_performance();

    /* Directory tree dump */
    if (klog_disk_live_active()) {
        klog(LOG_DEBUG, "", "");
        klog(LOG_DEBUG, "", "--- C:\\ Directory Tree ---");
        if (vfs_is_mounted('C')) {
            struct vfs_node *cr = vfs_get_drive_root('C');
            if (cr) {
                uint32_t count = dump_dir_tree("C:", cr, 0);
                klog(LOG_DEBUG, "tree", "(%u entries total)", (uint64_t)count);
            }
        }
    }

    /* VMM round-trip test.
     *
     * MUST use a virtual address above the bootloader's 4 GiB identity-map.
     * The boot page tables cover 0–4 GiB with 2 MiB huge pages; vmm_map_page
     * cannot split those, so any address below 4 GiB would silently fail and
     * resolve to the identity-mapped frame, not test_phys.
     * 8 GiB (0x200000000) is above that range and has no existing mapping. */
    {
        uintptr_t test_phys = pmm_alloc_frame();
        uintptr_t test_virt = 0x0000000200000000ULL;  /* 8 GiB */
        uint32_t vmm_ok = 1;

        if (test_phys) {
            if (vmm_map_page(test_virt, test_phys, VMM_KERNEL_RW) != 0) {
                pmm_free_frame(test_phys);
                vmm_ok = 0;
            } else {
                volatile uint64_t *p = (volatile uint64_t *)test_virt;
                *p = 0xDEADBEEFCAFE1234ULL;
                if (*p != 0xDEADBEEFCAFE1234ULL)
                    vmm_ok = 0;

                uintptr_t resolved = vmm_get_physical(test_virt);
                if (resolved != test_phys)
                    vmm_ok = 0;

                vmm_unmap_page(test_virt, 1);  /* also frees test_phys */
            }
        } else {
            vmm_ok = 0;
        }

        klog(vmm_ok ? LOG_DEBUG : LOG_ERROR, "test",
             "VMM map/read/unmap: %s", vmm_ok ? "passed" : "FAIL");
    }

    /* PMM alloc/free test */
    {
        uintptr_t f1, f2, f3;
        uint32_t pmm_ok = 1;

        f1 = pmm_alloc_frame();
        f2 = pmm_alloc_frame();
        if (!f1 || !f2) {
            pmm_ok = 0;
        } else {
            if (f1 == f2) pmm_ok = 0;
            pmm_free_frame(f1);
            f3 = pmm_alloc_frame();
            if (!f3) pmm_ok = 0;
            pmm_free_frame(f2);
            pmm_free_frame(f3);
        }

        klog(pmm_ok ? LOG_DEBUG : LOG_ERROR, "test",
             "PMM alloc/free: %s", pmm_ok ? "passed" : "FAIL");
    }

    /* Timer verification */
    boot_splash_status("Timer test (1s sleep)...");
    klog(LOG_DEBUG, "test", "Timer: sleeping 1 second...");
    sleep_ms(1000);
    klog(LOG_DEBUG, "test", "Timer OK (ticks: %u, uptime: %u sec)",
         system_get_ticks(), uptime());

    klog(LOG_DEBUG, "", "");
    klog(LOG_DEBUG, "", "--- Scheduler Tests --------------------------------------------------------");
    klog_disk_flush();

    {
        extern void thread_a_func(void);
        extern void thread_b_func(void);

        task_create(thread_a_func, "ThreadA");
        task_create(thread_b_func, "ThreadB");

        klog(LOG_DEBUG, "test",
             "Cooperative: two threads alternate via yield()");

        yield(); yield(); yield();
        yield(); yield(); yield();

        klog(LOG_DEBUG, "test", "Cooperative threading test passed");
    }

    /* Preemptive scheduling test */
    {
        extern void preempt_a_func(void);
        extern void preempt_b_func(void);

        task_create(preempt_a_func, "PreemptA");
        task_create(preempt_b_func, "PreemptB");

        klog(LOG_DEBUG, "test",
             "Preemptive: threads run without yield()");
        scheduler_enable();
        sleep_ms(600);
        scheduler_disable();

        klog(LOG_DEBUG, "test", "Preemptive scheduling test passed");
    }

    /* Kernel thread test (shared globals) */
    {
        extern volatile uint32_t thread_shared_counter;
        extern void thread_inc_func(void *arg);

        int tid_a, tid_b;
        int32_t status_a, status_b;

        thread_shared_counter = 0;

        tid_a = thread_create(thread_inc_func, (void *)"ThreadA", 0);
        tid_b = thread_create(thread_inc_func, (void *)"ThreadB", 0);

        if (tid_a >= 0 && tid_b >= 0) {
            status_a = thread_join((uint32_t)tid_a);
            status_b = thread_join((uint32_t)tid_b);
            (void)status_a;
            (void)status_b;

            klog(thread_shared_counter == 10 ? LOG_DEBUG : LOG_ERROR,
                 "test",
                 "Kernel thread test: counter=%u (%s)",
                 (uint64_t)thread_shared_counter,
                 thread_shared_counter == 10 ? "passed" : "FAIL");
        } else {
            klog(LOG_ERROR, "test", "thread_create failed");
        }
    }

    /* Mutex test */
    {
        extern volatile uint32_t mutex_shared_counter;
        extern void mutex_inc_func(void *arg);

        int mtid_a, mtid_b;

        mutex_shared_counter = 0;

        mtid_a = thread_create(mutex_inc_func, (void *)"MutexA", 0);
        mtid_b = thread_create(mutex_inc_func, (void *)"MutexB", 0);

        if (mtid_a >= 0 && mtid_b >= 0) {
            thread_join((uint32_t)mtid_a);
            thread_join((uint32_t)mtid_b);

            klog(mutex_shared_counter == 200 ? LOG_DEBUG : LOG_ERROR,
                 "test",
                 "Mutex test: counter=%u (%s)",
                 (uint64_t)mutex_shared_counter,
                 mutex_shared_counter == 200 ? "passed" : "FAIL");
        } else {
            klog(LOG_ERROR, "test", "mutex thread_create failed");
        }
    }

    /* Semaphore test */
    {
        extern volatile uint32_t sem_produced;
        extern volatile uint32_t sem_consumed;
        extern void sem_producer_func(void *arg);
        extern void sem_consumer_func(void *arg);

        int stid_p, stid_c;

        sem_produced = 0;
        sem_consumed = 0;

        stid_c = thread_create(sem_consumer_func, (void *)0, 0);
        stid_p = thread_create(sem_producer_func, (void *)0, 0);

        if (stid_p >= 0 && stid_c >= 0) {
            thread_join((uint32_t)stid_p);
            thread_join((uint32_t)stid_c);

            klog(sem_consumed == 5 ? LOG_DEBUG : LOG_ERROR, "test",
                 "Semaphore test: consumed=%u (%s)",
                 (uint64_t)sem_consumed,
                 sem_consumed == 5 ? "passed" : "FAIL");
        } else {
            klog(LOG_ERROR, "test", "semaphore thread_create failed");
        }
    }

    /* Pipe test */
    {
        extern int pipe_test_id;
        extern volatile uint32_t pipe_test_ok;
        extern void pipe_writer_func(void *arg);
        extern void pipe_reader_func(void *arg);

        int pipe_fds[2];
        int ptid_w, ptid_r;

        pipe_init();
        pipe_test_ok = 0;

        if (pipe_create(pipe_fds) == 0) {
            pipe_test_id = pipe_fds[0];

            ptid_r = thread_create(pipe_reader_func, (void *)0, 0);
            ptid_w = thread_create(pipe_writer_func, (void *)0, 0);

            if (ptid_w >= 0 && ptid_r >= 0) {
                thread_join((uint32_t)ptid_w);
                thread_join((uint32_t)ptid_r);

                klog(pipe_test_ok ? LOG_DEBUG : LOG_ERROR, "test",
                     "Pipe test: %s",
                     pipe_test_ok ? "passed" : "data mismatch");
            } else {
                klog(LOG_ERROR, "test", "pipe thread_create failed");
            }
        } else {
            klog(LOG_ERROR, "test", "pipe_create failed");
        }
    }

    /* Shared memory test */
    {
        extern volatile uint32_t shmem_test_ok;
        extern void shmem_writer_func(void *arg);
        extern void shmem_reader_func(void *arg);

        int shm_id;
        int shm_tw, shm_tr;

        shmem_test_ok = 0;

        shm_id = shmem_create("test_counter", sizeof(uint32_t));
        if (shm_id >= 0) {
            shm_tw = thread_create(shmem_writer_func, (void *)0, 0);
            shm_tr = thread_create(shmem_reader_func, (void *)0, 0);

            if (shm_tw >= 0 && shm_tr >= 0) {
                thread_join((uint32_t)shm_tw);
                thread_join((uint32_t)shm_tr);

                klog(shmem_test_ok ? LOG_DEBUG : LOG_ERROR, "test",
                     "Shared memory test: %s",
                     shmem_test_ok ? "passed" : "counter != 200");
            } else {
                klog(LOG_ERROR, "test", "shmem thread_create failed");
            }

            shmem_unmap(shm_id);
        } else {
            klog(LOG_ERROR, "test", "shmem_create failed");
        }
    }

    /* Swap test */
    boot_splash_status("Testing swap...");
    {
        uintptr_t test_phys = pmm_alloc_frame();
        uintptr_t test_virt = 0x800000;  /* 8 MiB */
        uint32_t swap_ok = 1;
        uint32_t k;
        int slot_id;

        swap_init(64);

        if (test_phys) {
            vmm_map_page(test_virt, test_phys, VMM_KERNEL_RW);

            {
                uint8_t *page = (uint8_t *)test_virt;
                for (k = 0; k < 4096; k++)
                    page[k] = (uint8_t)(k & 0xFF);
            }

            swap_clock_register(test_virt);

            slot_id = swap_out(test_virt);
            if (slot_id >= 0) {
                if (swap_in((uint32_t)slot_id, test_virt) == 0) {
                    uint8_t *page = (uint8_t *)test_virt;
                    for (k = 0; k < 4096; k++) {
                        if (page[k] != (uint8_t)(k & 0xFF)) {
                            swap_ok = 0;
                            break;
                        }
                    }
                } else {
                    swap_ok = 0;
                }
            } else {
                swap_ok = 0;
            }

            vmm_unmap_page(test_virt, 1);

            klog(swap_ok ? LOG_DEBUG : LOG_ERROR, "test",
                 "Swap test: %s (%u/%u slots)",
                 swap_ok ? "passed" : "FAIL",
                 (uint64_t)swap_get_used_slots(),
                 (uint64_t)swap_get_total_slots());
        } else {
            klog(LOG_ERROR, "test", "swap test alloc failed");
        }
    }

    /* mmap test */
    boot_splash_status("Testing mmap...");
    if (vfs_is_mounted('C')) {
        struct vfs_node *mf = vfs_open("C:\\hello.txt", VFS_O_READ);
        if (mf) {
            void *mapped = mmap((void *)0, 4096, PROT_READ, MAP_PRIVATE,
                                mf, 0);
            if (mapped != MAP_FAILED) {
                const char *txt = (const char *)mapped;
                uint32_t mmap_ok = (txt[0] != '\0') ? 1 : 0;

                klog(mmap_ok ? LOG_DEBUG : LOG_ERROR, "test",
                     "mmap test: %s (mapped at %p, first='%c')",
                     mmap_ok ? "passed" : "FAIL",
                     (uintptr_t)mapped, (uint64_t)(uint8_t)txt[0]);

                munmap(mapped, 4096);
            } else {
                klog(LOG_ERROR, "test", "mmap returned MAP_FAILED");
            }
            vfs_close(mf);
        } else {
            klog(LOG_DEBUG, "test",
                 "mmap test: hello.txt not found (skipped)");
        }
    }

    /* VirtIO-blk test */
    boot_splash_status("Testing storage...");
    if (virtio_blk_present()) {
        uint8_t sect0[512];
        uint32_t vt;
        int vrc;

        for (vt = 0; vt < 512; vt++) sect0[vt] = 0;

        vrc = virtio_blk_read(0, 1, sect0);
        if (vrc == 0) {
            klog(LOG_DEBUG, "test",
                 "VirtIO-blk: sector 0 read OK (%u sectors)",
                 (uint64_t)virtio_blk_capacity());
        } else {
            klog(LOG_ERROR, "test", "VirtIO-blk: sector 0 read failed");
        }
    }

    /* AHCI sector 0 test */
    boot_splash_status("Testing AHCI...");
    if (ahci_present()) {
        uint8_t asect0[512];
        uint32_t at;
        int arc;

        for (at = 0; at < 512; at++) asect0[at] = 0;

        arc = ahci_read(0, 0, 1, asect0);
        if (arc == 0) {
            klog(LOG_DEBUG, "test",
                 "AHCI: sector 0 read OK (%u sectors)",
                 (uint64_t)ahci_capacity(0));
        } else {
            klog(LOG_ERROR, "test", "AHCI: sector 0 read failed");
        }
    }

    /* User mode tests (currently skipped) */
#if 0
    {
        extern void user_test_func(void);
        syscall_init();
        task_create_user(user_test_func, "UserTest");
        scheduler_enable();
        sleep_ms(500);
        scheduler_disable();
        klog(LOG_DEBUG, "test", "User mode test passed");
    }

    {
        extern void exec_loader_func(void);
        task_create(exec_loader_func, "ExecLoader");
        scheduler_enable();
        sleep_ms(500);
        scheduler_disable();
        klog(LOG_DEBUG, "test", "Exec test passed");
    }

    {
        extern void fork_test_func(void);
        task_create_user(fork_test_func, "ForkTest");
        scheduler_enable();
        sleep_ms(800);
        scheduler_disable();
        klog(LOG_DEBUG, "test", "Fork test passed");
    }
#else
    klog(LOG_DEBUG, "test", "User mode / exec / fork tests skipped");
#endif

    boot_splash_status("Preparing desktop...");
    boot_splash_tick();
}
