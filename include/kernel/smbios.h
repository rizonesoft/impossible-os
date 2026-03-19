/* ============================================================================
 * smbios.h — SMBIOS System Information Parser
 *
 * Parses SMBIOS tables found via UEFI Configuration Table.
 * Extracts system manufacturer, product name, BIOS version, CPU info,
 * and memory information for the "About This PC" dialog.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* Maximum string length for SMBIOS fields */
#define SMBIOS_STRING_MAX  64

/* System information extracted from SMBIOS tables */
struct smbios_system_info {
    /* Type 0 — BIOS Information */
    char bios_vendor[SMBIOS_STRING_MAX];
    char bios_version[SMBIOS_STRING_MAX];
    char bios_date[SMBIOS_STRING_MAX];

    /* Type 1 — System Information */
    char sys_manufacturer[SMBIOS_STRING_MAX];
    char sys_product[SMBIOS_STRING_MAX];
    char sys_serial[SMBIOS_STRING_MAX];

    /* Type 2 — Baseboard */
    char board_manufacturer[SMBIOS_STRING_MAX];
    char board_product[SMBIOS_STRING_MAX];

    /* Type 4 — Processor (first socket) */
    char cpu_manufacturer[SMBIOS_STRING_MAX];
    char cpu_socket[SMBIOS_STRING_MAX];
    uint16_t cpu_max_speed_mhz;
    uint16_t cpu_core_count;
    uint16_t cpu_thread_count;

    /* Type 17 — Memory (aggregate) */
    uint32_t ram_total_mb;
    uint16_t ram_speed_mhz;     /* speed of first populated DIMM */
    uint8_t  ram_type;          /* DDR type of first populated DIMM */
    uint8_t  ram_dimm_count;    /* number of populated DIMMs */

    /* Metadata */
    uint8_t  smbios_major;
    uint8_t  smbios_minor;
    uint8_t  valid;             /* 1 if SMBIOS was found and parsed */
};

/* Memory type constants (SMBIOS Type 17 field 18) */
#define SMBIOS_MEM_DDR3   24
#define SMBIOS_MEM_DDR4   26
#define SMBIOS_MEM_LPDDR4 27
#define SMBIOS_MEM_DDR5   34
#define SMBIOS_MEM_LPDDR5 35

/* Initialize SMBIOS — find tables via config table, parse, log summary.
 * Must be called after uefi_config_init(). */
void smbios_init(void);

/* Returns pointer to the global system info struct (valid after smbios_init). */
const struct smbios_system_info *smbios_get_info(void);
