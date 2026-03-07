/* ============================================================================
 * acpi.h — ACPI table parsing and power management
 *
 * Parses the RSDP → RSDT/XSDT → FADT chain to locate the PM1a control
 * register needed for clean shutdown.  Also supports reboot via the
 * keyboard controller reset or ACPI reset register.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* ---- ACPI table structures ---- */

/* Root System Description Pointer (RSDP) — ACPI 1.0 */
struct acpi_rsdp {
    char     signature[8];   /* "RSD PTR " */
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;       /* 0 = ACPI 1.0, 2 = ACPI 2.0+ */
    uint32_t rsdt_addr;      /* Physical address of RSDT */
} __attribute__((packed));

/* Extended RSDP — ACPI 2.0+ */
struct acpi_rsdp2 {
    struct acpi_rsdp v1;
    uint32_t length;
    uint64_t xsdt_addr;     /* Physical address of XSDT */
    uint8_t  ext_checksum;
    uint8_t  reserved[3];
} __attribute__((packed));

/* Standard ACPI table header (shared by RSDT, FADT, etc.) */
struct acpi_sdt_header {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed));

/* Root System Description Table (RSDT) — 32-bit pointers */
struct acpi_rsdt {
    struct acpi_sdt_header header;
    uint32_t entries[];      /* Array of 32-bit physical addresses */
} __attribute__((packed));

/* Extended System Description Table (XSDT) — 64-bit pointers */
struct acpi_xsdt {
    struct acpi_sdt_header header;
    uint64_t entries[];
} __attribute__((packed));

/* Generic Address Structure (GAS) — ACPI 2.0+ */
struct acpi_gas {
    uint8_t  address_space;  /* 0=system memory, 1=system I/O */
    uint8_t  bit_width;
    uint8_t  bit_offset;
    uint8_t  access_size;
    uint64_t address;
} __attribute__((packed));

/* Fixed ACPI Description Table (FADT / FACP) */
struct acpi_fadt {
    struct acpi_sdt_header header;
    uint32_t firmware_ctrl;
    uint32_t dsdt;
    uint8_t  reserved1;
    uint8_t  preferred_pm_profile;
    uint16_t sci_interrupt;
    uint32_t smi_commandport;
    uint8_t  acpi_enable;
    uint8_t  acpi_disable;
    uint8_t  s4bios_req;
    uint8_t  pstate_control;
    uint32_t pm1a_event_block;
    uint32_t pm1b_event_block;
    uint32_t pm1a_control_block;   /* PM1a_CNT — used for shutdown */
    uint32_t pm1b_control_block;
    uint32_t pm2_control_block;
    uint32_t pm_timer_block;
    uint32_t gpe0_block;
    uint32_t gpe1_block;
    uint8_t  pm1_event_length;
    uint8_t  pm1_control_length;
    uint8_t  pm2_control_length;
    uint8_t  pm_timer_length;
    uint8_t  gpe0_length;
    uint8_t  gpe1_length;
    uint8_t  gpe1_base;
    uint8_t  cstate_control;
    uint16_t worst_c2_latency;
    uint16_t worst_c3_latency;
    uint16_t flush_size;
    uint16_t flush_stride;
    uint8_t  duty_offset;
    uint8_t  duty_width;
    uint8_t  day_alarm;
    uint8_t  month_alarm;
    uint8_t  century;
    uint16_t boot_arch_flags;
    uint8_t  reserved2;
    uint32_t flags;
    struct acpi_gas reset_reg;     /* ACPI 2.0+ reset register */
    uint8_t  reset_value;
    uint16_t arm_boot_arch;
    uint8_t  fadt_minor_version;
} __attribute__((packed));

/* ---- API ---- */

/* Initialize ACPI — parse RSDP → RSDT → FADT.
 * Returns 0 on success, -1 if ACPI tables not found. */
int  acpi_init(void);

/* Power off the machine via ACPI S5 sleep state.
 * Falls back to QEMU-specific port if FADT is unavailable.
 * Does not return on success. */
void acpi_shutdown(void);

/* Reboot the machine via ACPI reset register or keyboard controller.
 * Does not return on success. */
void acpi_reboot(void);
