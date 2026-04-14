/* ============================================================================
 * alpc.c -- ALPC ABI init (header-only contract)
 *
 * Section 1 of TODO-12 defines only the on-the-wire header, port
 * attributes, and flag bitmasks. The static asserts in alpc.h catch any
 * layout drift at compile time. This file carries a single init entry
 * point that emits one klog line so the ABI's presence is visible on
 * the serial log during boot. Future sections add port object
 * registration (section 2), syscalls (section 8), etc.
 * ============================================================================ */

#include "kernel/ipc/alpc.h"
#include "kernel/ipc/alpc_port.h"
#include "kernel/boot_init.h"
#include "kernel/klog.h"

boot_result_t alpc_init(void)
{
    boot_result_t r;

    klog(LOG_INFO, "alpc", "header constants defined");

    r = alpc_port_init();
    if (r != BOOT_OK)
        return r;

    return BOOT_OK;
}
