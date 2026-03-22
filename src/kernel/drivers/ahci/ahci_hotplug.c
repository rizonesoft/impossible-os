/* ============================================================================
 * ahci_hotplug.c — AHCI Hot-Plug Detection and Management
 *
 * Handles PxIS.PCS (Port Connect Change) and PxIS.PRCS (PhyRdy Change)
 * interrupts for native SATA hot-plug support.
 *
 * On insertion: clear SERR, wait BSY, read SIG, identify, probe partitions.
 * On removal: stop engine, flush, unmount, release resources.
 *
 * CAP.SXS (bit 5) = External SATA support
 * CAP.SMPS (bit 28) = Mechanical presence switch
 * ============================================================================ */

#include "kernel/drivers/ahci_internal.h"
#include "registry.h"

/* Hot-plug statistics */
static uint32_t hotplug_insertions;
static uint32_t hotplug_removals;
static uint32_t hotplug_errors;

/* ---- Hot-plug insertion handler ---- */

/* Called from ISR context when PxSSTS.DET transitions to 3 (device present).
 * Schedules deferred initialization since we can't do heavy work in ISR. */
void ahci_hotplug_insert(int port_idx)
{
    struct ahci_port *p = &ports[port_idx];
    volatile uint8_t *pregs = p->regs;
    uint32_t ssts, sig, tfd;
    uint32_t wait;

    klog(LOG_INFO, "ahci",
           "Port %u: hot-plug insertion detected",
           (uint64_t)p->port_num);

    /* Step 1: Clear SERR to dismiss stale link-up errors */
    port_write(pregs, AHCI_PxSERR, 0xFFFFFFFF);

    /* Step 2: Wait for BSY to clear (device ready after spin-up) */
    wait = 3000000;  /* ~3 seconds for slow drives */
    while (wait--) {
        tfd = port_read(pregs, AHCI_PxTFD);
        if (!(tfd & AHCI_PxTFD_BSY))
            break;
        __asm__ volatile ("inb $0x80, %%al" ::: "al", "memory");
    }

    if (wait == 0) {
        klog(LOG_WARN, "ahci",
               "Port %u: hot-plug device BSY stuck after insertion",
               (uint64_t)p->port_num);
        hotplug_errors++;
        return;
    }

    /* Step 3: Verify device is still present */
    ssts = port_read(pregs, AHCI_PxSSTS);
    if ((ssts & AHCI_SSTS_DET_MASK) != AHCI_SSTS_DET_OK) {
        klog(LOG_DEBUG, "ahci",
               "Port %u: device disappeared before identification",
               (uint64_t)p->port_num);
        hotplug_errors++;
        return;
    }

    /* Step 4: Read device signature */
    sig = port_read(pregs, AHCI_PxSIG);
    p->sig = sig;

    switch (sig) {
    case AHCI_SIG_ATA:
        p->device_type = AHCI_DEV_ATA;
        p->is_atapi = 0;
        p->sector_size = 512;
        break;

    case AHCI_SIG_ATAPI:
        p->device_type = AHCI_DEV_ATAPI;
        p->is_atapi = 1;
        p->sector_size = 2048;
        klog(LOG_INFO, "ahci",
             "Port %u: hot-plug ATAPI device",
             (uint64_t)p->port_num);
        break;

    default:
        klog(LOG_DEBUG, "ahci",
             "Port %u: hot-plug unknown signature 0x%x",
             (uint64_t)p->port_num, (uint64_t)sig);
        hotplug_errors++;
        return;
    }

    /* Step 5: Ensure command engine is running */
    if (!(port_read(pregs, AHCI_PxCMD) & AHCI_PxCMD_ST)) {
        port_start_cmd(pregs);
    }

    /* Step 6: Run IDENTIFY to get model, serial, capacity */
    if (p->is_atapi) {
        if (atapi_do_identify(p) != 0) {
            klog(LOG_WARN, "ahci",
                   "Port %u: hot-plug ATAPI IDENTIFY failed",
                   (uint64_t)p->port_num);
            hotplug_errors++;
            return;
        }
        atapi_read_capacity(p);
    } else {
        if (ahci_do_identify(p) != 0) {
            klog(LOG_WARN, "ahci",
                   "Port %u: hot-plug IDENTIFY failed",
                   (uint64_t)p->port_num);
            hotplug_errors++;
            return;
        }
    }

    p->active = 1;

    {
        uint64_t size_mb = p->sectors / 2048;
        klog(LOG_INFO, "ahci",
               "Port %u: hot-plug \"%s\" detected (%u MiB, %s)",
               (uint64_t)p->port_num,
               p->model,
               size_mb,
               p->is_atapi ? "ATAPI" : "SATA");
    }

    hotplug_insertions++;

    /* Step 7: Expose to Registry */
    {
        HKEY hAhci, hHp;
        long rc;
        char port_path[32];
        int n = (int)p->port_num;
        char *pp = port_path;

        *pp++ = 'P'; *pp++ = 'o'; *pp++ = 'r'; *pp++ = 't';
        if (n >= 10) { *pp++ = (char)('0' + n / 10); }
        *pp++ = (char)('0' + n % 10);
        *pp++ = '\\'; *pp++ = 'H'; *pp++ = 'o'; *pp++ = 't';
        *pp++ = 'P'; *pp++ = 'l'; *pp++ = 'u'; *pp++ = 'g';
        *pp = '\0';

        rc = RegCreateKeyEx(HKEY_LOCAL_MACHINE, "HARDWARE\\AHCI", 0,
                            (char *)0, 0, KEY_ALL_ACCESS, (void *)0,
                            &hAhci, (uint32_t *)0);
        if (rc == 0) {
            rc = RegCreateKeyEx(hAhci, port_path, 0,
                                (char *)0, 0, KEY_ALL_ACCESS, (void *)0,
                                &hHp, (uint32_t *)0);
            if (rc == 0) {
                RegSetString(hHp, "Model", p->model);
                RegSetString(hHp, "Serial", p->serial);
                RegSetDword(hHp, "SizeMB", (uint32_t)(p->sectors / 2048));
                RegSetString(hHp, "Type", p->is_atapi ? "ATAPI" : "SATA");
                RegSetString(hHp, "Event", "Inserted");
                RegCloseKey(hHp);
            }
            RegCloseKey(hAhci);
        }
    }

    /* Step 8: Attempt partition probe (if GPT/MBR probing is available)
     * This would call into the block device layer to enumerate partitions.
     * For now, log that the device is ready for partition probing. */
    klog(LOG_INFO, "ahci",
           "Port %u: device ready for partition probing",
           (uint64_t)p->port_num);
}

/* ---- Hot-plug removal handler ---- */

/* Called when PxSSTS.DET transitions to 0 (no device). */
void ahci_hotplug_remove(int port_idx)
{
    struct ahci_port *p = &ports[port_idx];
    volatile uint8_t *pregs = p->regs;

    klog(LOG_INFO, "ahci",
           "Port %u: hot-plug removal detected (\"%s\")",
           (uint64_t)p->port_num,
           p->active ? p->model : "<unknown>");

    if (!p->active) {
        klog(LOG_DEBUG, "ahci",
               "Port %u: removal on inactive port — ignoring",
               (uint64_t)p->port_num);
        return;
    }

    /* Step 1: Stop command engine */
    port_stop_cmd(pregs);

    /* Step 2: Flush dirty buffers — skip if device is already gone.
     * Check PxSSTS.DET first; if 0, the device is physically absent. */
    {
        uint32_t ssts = port_read(pregs, AHCI_PxSSTS);
        if ((ssts & AHCI_SSTS_DET_MASK) == AHCI_SSTS_DET_OK) {
            /* Device still partially present — try to flush */
            klog(LOG_DEBUG, "ahci",
                   "Port %u: attempting cache flush before removal",
                   (uint64_t)p->port_num);
            /* Best-effort flush: don't block on failure */
            port_start_cmd(pregs);
            ahci_flush(port_idx);
            port_stop_cmd(pregs);
        }
    }

    /* Step 3: Clear all pending interrupts and errors */
    port_write(pregs, AHCI_PxIS, 0xFFFFFFFF);
    port_write(pregs, AHCI_PxSERR, 0xFFFFFFFF);

    /* Step 4: Mark port as inactive */
    p->active = 0;
    p->device_type = AHCI_DEV_NULL;
    p->sectors = 0;

    /* Step 5: Clear NCQ state if any */
    if (p->ncq_supported) {
        int t;
        uint32_t pending = p->tags_pending;
        for (t = 0; t < 32; t++) {
            if (pending & (1U << t)) {
                p->tag_status[t] = -1;
                p->tags_completed |= (1U << t);
                if (p->tag_callbacks[t]) {
                    p->tag_callbacks[t](port_idx, t, -1,
                                        p->tag_cb_ctx[t]);
                    p->tag_callbacks[t] = (ahci_callback_t)0;
                }
            }
        }
        p->tags_pending = 0;
    }

    /* Step 6: Signal completion event for any waiters */
    event_set(&p->completion);

    hotplug_removals++;

    /* Step 7: Update Registry */
    {
        HKEY hAhci, hHp;
        long rc;
        char port_path[32];
        int n = (int)p->port_num;
        char *pp = port_path;

        *pp++ = 'P'; *pp++ = 'o'; *pp++ = 'r'; *pp++ = 't';
        if (n >= 10) { *pp++ = (char)('0' + n / 10); }
        *pp++ = (char)('0' + n % 10);
        *pp++ = '\\'; *pp++ = 'H'; *pp++ = 'o'; *pp++ = 't';
        *pp++ = 'P'; *pp++ = 'l'; *pp++ = 'u'; *pp++ = 'g';
        *pp = '\0';

        rc = RegCreateKeyEx(HKEY_LOCAL_MACHINE, "HARDWARE\\AHCI", 0,
                            (char *)0, 0, KEY_ALL_ACCESS, (void *)0,
                            &hAhci, (uint32_t *)0);
        if (rc == 0) {
            rc = RegCreateKeyEx(hAhci, port_path, 0,
                                (char *)0, 0, KEY_ALL_ACCESS, (void *)0,
                                &hHp, (uint32_t *)0);
            if (rc == 0) {
                RegSetString(hHp, "Event", "Removed");
                RegCloseKey(hHp);
            }
            RegCloseKey(hAhci);
        }
    }

    klog(LOG_INFO, "ahci",
           "Port %u: removal complete — resources released",
           (uint64_t)p->port_num);
}

/* ---- ISR hot-plug event dispatcher ---- */

/* Called from ahci_irq_handler when PxIS.PCS or PxIS.PRCS is set.
 * Examines PxSSTS.DET to determine insertion vs removal. */
void ahci_hotplug_check(int port_idx)
{
    struct ahci_port *p = &ports[port_idx];
    volatile uint8_t *pregs = p->regs;
    uint32_t ssts;

    /* Clear SERR diagnostic bits (PCS/PRCS cause DIAG.X/DIAG.N) */
    port_write(pregs, AHCI_PxSERR,
               AHCI_PxSERR_DIAG_X | AHCI_PxSERR_DIAG_N);

    ssts = port_read(pregs, AHCI_PxSSTS);

    if ((ssts & AHCI_SSTS_DET_MASK) == AHCI_SSTS_DET_OK) {
        /* DET = 3: device present and communication established */
        if (!p->active) {
            ahci_hotplug_insert(port_idx);
        }
    } else {
        /* DET = 0 or other: device removed or link down */
        if (p->active) {
            ahci_hotplug_remove(port_idx);
        }
    }
}

/* ---- Hot-plug statistics ---- */

void ahci_hotplug_flush_stats(void)
{
    HKEY hk;
    if (RegCreateKeyEx(HKEY_LOCAL_MACHINE,
                       "HARDWARE\\AHCI\\HotPlug",
                       0, NULL, 0, 0, NULL, &hk, NULL) == 0) {
        RegSetDword(hk, "Insertions", hotplug_insertions);
        RegSetDword(hk, "Removals",   hotplug_removals);
        RegSetDword(hk, "Errors",     hotplug_errors);
        RegCloseKey(hk);
    }
}
