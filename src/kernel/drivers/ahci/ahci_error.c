/* ============================================================================
 * ahci_error.c — AHCI error counter flush to Registry
 * ============================================================================ */

#include "kernel/drivers/ahci_internal.h"
#include "registry.h"

/* ---- Flush per-port error counters to Registry ---- */

static uint32_t ahci_reg_throttle;
static HKEY     ahci_reg_port_keys[32];
static int      ahci_reg_keys_init;

/* One-time setup: open/create all port error keys and keep them cached. */
static void ahci_reg_keys_open(void)
{
    HKEY hAhci;
    long rc;
    int i;

    rc = RegCreateKeyEx(HKEY_LOCAL_MACHINE, "HARDWARE\\AHCI", 0,
                        (char *)0, 0, KEY_ALL_ACCESS, (void *)0,
                        &hAhci, (uint32_t *)0);
    if (rc != 0) return;

    for (i = 0; i < num_ports_total; i++) {
        char port_path[32];
        if (!ports[i].active) continue;

        {
            int n = ports[i].port_num;
            char *p = port_path;
            *p++ = 'P'; *p++ = 'o'; *p++ = 'r'; *p++ = 't';
            if (n >= 10) { *p++ = (char)('0' + n / 10); }
            *p++ = (char)('0' + n % 10);
            *p++ = '\\'; *p++ = 'E'; *p++ = 'r'; *p++ = 'r';
            *p++ = 'o'; *p++ = 'r'; *p++ = 's'; *p = '\0';
        }

        rc = RegCreateKeyEx(hAhci, port_path, 0,
                            (char *)0, 0, KEY_ALL_ACCESS, (void *)0,
                            &ahci_reg_port_keys[i], (uint32_t *)0);
    }

    RegCloseKey(hAhci);
    ahci_reg_keys_init = 1;
}

void ahci_flush_error_counters(void)
{
    int i;

    if (!initialized) return;

    /* Throttle: only flush every 256 iterations (~4s at 60 fps) */
    if ((ahci_reg_throttle++ & 0xFF) != 0) return;

    if (!ahci_reg_keys_init)
        ahci_reg_keys_open();

    for (i = 0; i < num_ports_total; i++) {
        HKEY hPort = ahci_reg_port_keys[i];
        if (!hPort || !ports[i].active) continue;

        RegSetDword(hPort, "FatalErrors",   ports[i].errors.fatal_errors);
        RegSetDword(hPort, "NonfatalErrors", ports[i].errors.nonfatal_errors);
        RegSetDword(hPort, "CrcErrors",     ports[i].errors.crc_errors);
        RegSetDword(hPort, "LinkResets",    ports[i].errors.link_resets);
        RegSetDword(hPort, "CmdFailures",   ports[i].errors.cmd_failures);
    }
}
