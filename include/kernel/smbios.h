/* ============================================================================
 * smbios.h -- SMBIOS System Information Parser
 *
 * Parses SMBIOS tables found via UEFI Configuration Table.
 * Extracts system manufacturer, product name, BIOS version, CPU info,
 * and memory information for the "About This PC" dialog.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* Maximum string length for SMBIOS fields */
#define SMBIOS_STRING_MAX  64

/* Per-socket CPU info (Type 4, up to 4 sockets) */
#define SMBIOS_CPU_MAX  4
struct smbios_cpu_info {
    char     socket[SMBIOS_STRING_MAX];
    char     manufacturer[SMBIOS_STRING_MAX];
    uint16_t max_speed_mhz;
    uint16_t core_count;
    uint16_t thread_count;
    uint8_t  family;
    uint8_t  valid;
};

/* Per-DIMM memory info (Type 17, up to 16 slots) */
#define SMBIOS_DIMM_MAX  16
struct smbios_dimm_info {
    char     manufacturer[SMBIOS_STRING_MAX];
    char     part_number[SMBIOS_STRING_MAX];
    char     bank_locator[SMBIOS_STRING_MAX];
    char     device_locator[SMBIOS_STRING_MAX];
    uint32_t size_mb;
    uint16_t speed_mhz;
    uint8_t  mem_type;   /* SMBIOS_MEM_* */
    uint8_t  valid;
};

/* System information extracted from SMBIOS tables */
struct smbios_system_info {
    /* Type 0 -- BIOS Information */
    char bios_vendor[SMBIOS_STRING_MAX];
    char bios_version[SMBIOS_STRING_MAX];
    char bios_date[SMBIOS_STRING_MAX];

    /* Type 1 -- System Information */
    char    sys_manufacturer[SMBIOS_STRING_MAX];
    char    sys_product[SMBIOS_STRING_MAX];
    char    sys_version[SMBIOS_STRING_MAX];
    char    sys_serial[SMBIOS_STRING_MAX];
    uint8_t sys_uuid[16];   /* raw UUID bytes in SMBIOS wire order */

    /* Type 2 -- Baseboard */
    char board_manufacturer[SMBIOS_STRING_MAX];
    char board_product[SMBIOS_STRING_MAX];
    char board_version[SMBIOS_STRING_MAX];
    char board_serial[SMBIOS_STRING_MAX];
    char board_asset[SMBIOS_STRING_MAX];

    /* Type 3 -- Chassis / System Enclosure */
    char    chassis_manufacturer[SMBIOS_STRING_MAX];
    char    chassis_serial[SMBIOS_STRING_MAX];
    char    chassis_asset[SMBIOS_STRING_MAX];
    uint8_t chassis_type;   /* enclosure type, bit7 (lock) stripped */
    uint8_t chassis_valid;

    /* Type 16 -- Physical Memory Array (first array only) */
    uint64_t mem_array_max_capacity_bytes; /* 0 = absent/unknown */
    uint16_t mem_array_handle;   /* structure handle; Type 19 ranges match this */
    uint16_t mem_array_device_count;
    uint8_t  mem_array_location;
    uint8_t  mem_array_use;
    uint8_t  mem_array_error_correction; /* SMBIOS_ECC_* (offset 0x06) */
    uint8_t  mem_array_valid;

    /* Type 19 -- Memory Array Mapped Address (first range only) */
    uint64_t mem_mapped_start;  /* byte address; 0 = absent */
    uint64_t mem_mapped_end;    /* byte address (inclusive end) */
    uint8_t  mem_mapped_valid;

    /* Type 4 -- Processor (per socket; legacy single-socket summary kept) */
    char     cpu_manufacturer[SMBIOS_STRING_MAX];
    char     cpu_socket[SMBIOS_STRING_MAX];
    uint16_t cpu_max_speed_mhz;
    uint16_t cpu_core_count;
    uint16_t cpu_thread_count;

    /* Per-socket array */
    struct smbios_cpu_info  cpus[SMBIOS_CPU_MAX];
    uint8_t                 cpu_count;

    /* Type 17 -- Memory (aggregate summary kept for legacy callers) */
    uint32_t ram_total_mb;
    uint16_t ram_speed_mhz;
    uint8_t  ram_type;
    uint8_t  ram_dimm_count;

    /* Per-DIMM array */
    struct smbios_dimm_info dimms[SMBIOS_DIMM_MAX];
    uint8_t                 dimm_count;

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

/* Memory error-correction constants (SMBIOS Type 16 offset 0x06) */
#define SMBIOS_ECC_OTHER       0x01
#define SMBIOS_ECC_UNKNOWN     0x02
#define SMBIOS_ECC_NONE        0x03
#define SMBIOS_ECC_PARITY      0x04
#define SMBIOS_ECC_SINGLE_BIT  0x05  /* Single-bit ECC */
#define SMBIOS_ECC_MULTI_BIT   0x06  /* Multi-bit ECC */
#define SMBIOS_ECC_CRC         0x07

/* Initialize SMBIOS -- find tables via config table, parse, log summary.
 * Must be called after uefi_config_init(). */
void smbios_init(void);

/* Returns pointer to the global system info struct (valid after smbios_init). */
const struct smbios_system_info *smbios_get_info(void);

/* Copy the raw 16-byte system UUID into uuid[16].
 * Returns 1 if SMBIOS was found and UUID is non-zero, 0 otherwise. */
int smbios_get_system_uuid(uint8_t uuid[16]);

/* Return the SMBIOS Type 3 chassis/enclosure type code (bit7 lock stripped),
 * or -1 if no Type 3 record was parsed. Codes per DMTF SMBIOS 3.x Table 17. */
int smbios_get_chassis_type(void);

/* Return 1 if the chassis type indicates a mobile/portable form factor
 * (Portable/Laptop/Notebook/Hand-Held/Sub-Notebook/Tablet/Convertible/
 * Detachable), 0 for desktop/server/unknown. Drives power + UI defaults. */
int smbios_chassis_is_laptop(void);

/* Pure classifier: 1 if a SMBIOS Type 3 chassis code (bit7 already stripped)
 * is a mobile/portable form factor, else 0. No global state; testable. */
int smbios_chassis_type_is_mobile(uint8_t code);

/* Pure helper: convert a SMBIOS kilobyte count to bytes in 64-bit, avoiding
 * the 32-bit-multiply wrap that truncates addresses above 4 GiB. */
uint64_t smbios_kb_to_bytes(uint32_t kb);

/* Pure decoder for a SMBIOS Type 16 (Physical Memory Array) formatted record.
 * `rec` points at the structure start (rec[0]=type, rec[1]=length, ...).
 * Writes maximum capacity in bytes (0 = unknown/absent; handles the
 * 0x80000000 sentinel -> 8-byte Extended Maximum Capacity) and the 2-byte
 * device count. Returns 1 if the record was long enough to decode, else 0.
 * No global state; testable from synthetic byte arrays. */
int smbios_type16_decode(const uint8_t *rec, uint8_t length,
                         uint64_t *out_capacity_bytes,
                         uint16_t *out_device_count);

/* Pure decoder for a SMBIOS Type 19 (Memory Array Mapped Address) record.
 * Writes the inclusive byte range [*out_start, *out_end] (4-byte KB fields,
 * or the 0xFFFFFFFF sentinel -> 8-byte Extended byte addresses at 0x0F/0x17).
 * Also writes the Memory Array Handle (offset 0x0C) the range belongs to, so
 * callers can associate ranges with the correct Type 16 array on multi-array
 * firmware. Returns 1 on a valid range, 0 if the record is too short or
 * malformed (end < start). No global state; testable from synthetic arrays. */
int smbios_type19_decode(const uint8_t *rec, uint8_t length,
                         uint64_t *out_start, uint64_t *out_end,
                         uint16_t *out_array_handle);

/* Write SMBIOS hardware data into the Registry.
 * Creates HKLM\HARDWARE\BIOS\*, HKLM\HARDWARE\System\*,
 * HKLM\HARDWARE\CPU\{idx}\*, and HKLM\HARDWARE\Memory\{idx}\*.
 * Must be called after smbios_init() and registry_init(). */
void smbios_populate_registry(void);

/* Return the firmware-mapped raw SMBIOS structure-table base + length
 * captured at smbios_init() time. Used by the Win32 GetSystemFirmwareTable
 * facade to return raw RSMB bytes -- caller must memcpy into its own
 * buffer before exposing to user-mode (the firmware pointer never crosses
 * the syscall boundary). Returns 1 on success, 0 if SMBIOS was never
 * successfully parsed (s_info.valid == 0) or the table bounds are zero.
 *
 * Lifetime: pointer remains valid for the kernel's lifetime (UEFI marks
 * SMBIOS pages as Reserved, never reclaimed). Read-only.
 *
 * The public accessor hands out validated bounds (table_addr != 0,
 * table_size > 0) so a caller cannot dereference an unparsed firmware
 * region. */
int smbios_get_raw_table(const uint8_t **out_addr, uint32_t *out_size);
