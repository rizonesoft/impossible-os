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

/* ---- MADT (Multiple APIC Description Table) — for SMP ---- */

#define MAX_CPUS  16   /* maximum supported CPUs */

/* MADT header — signature "APIC" */
struct acpi_madt {
    struct acpi_sdt_header header;
    uint32_t lapic_addr;        /* Physical address of Local APIC */
    uint32_t flags;             /* bit 0: dual-8259 legacy PICs installed */
} __attribute__((packed));

/* MADT entry types */
#define MADT_TYPE_LAPIC          0   /* Processor Local APIC */
#define MADT_TYPE_IOAPIC         1   /* I/O APIC */
#define MADT_TYPE_INT_OVERRIDE   2   /* Interrupt Source Override */
#define MADT_TYPE_NMI_SOURCE     3   /* NMI Source */
#define MADT_TYPE_LAPIC_NMI      4   /* Local APIC NMI */
#define MADT_TYPE_LAPIC_OVERRIDE 5   /* Local APIC Address Override */

/* Common MADT entry header */
struct madt_entry_header {
    uint8_t  type;
    uint8_t  length;
} __attribute__((packed));

/* Type 0: Processor Local APIC */
struct madt_lapic {
    struct madt_entry_header header;
    uint8_t  acpi_processor_id;
    uint8_t  apic_id;           /* LAPIC ID for this CPU */
    uint32_t flags;             /* bit 0: enabled, bit 1: online-capable */
} __attribute__((packed));

/* Type 1: I/O APIC */
struct madt_ioapic {
    struct madt_entry_header header;
    uint8_t  ioapic_id;
    uint8_t  reserved;
    uint32_t ioapic_addr;       /* Physical address of I/O APIC MMIO */
    uint32_t gsi_base;          /* Global System Interrupt base */
} __attribute__((packed));

/* Type 2: Interrupt Source Override (ISA IRQ remapping) */
struct madt_int_override {
    struct madt_entry_header header;
    uint8_t  bus;               /* always 0 = ISA */
    uint8_t  source;            /* ISA IRQ number (e.g. 0 = PIT) */
    uint32_t gsi;               /* GSI that this IRQ maps to */
    uint16_t flags;             /* polarity + trigger mode */
} __attribute__((packed));

/* Type 4: Local APIC NMI */
struct madt_lapic_nmi {
    struct madt_entry_header header;
    uint8_t  acpi_processor_id; /* 0xFF = all processors */
    uint16_t flags;
    uint8_t  lint;              /* LINT# (0 or 1) */
} __attribute__((packed));

/* Per-CPU info discovered from MADT */
struct cpu_info {
    uint8_t  apic_id;
    uint8_t  acpi_id;
    uint8_t  is_bsp;            /* 1 for the bootstrap processor */
    uint8_t  enabled;           /* 1 if CPU is usable */
};

/* ---- API ---- */

/* Initialize ACPI — parse RSDP → RSDT → FADT → MADT.
 * Returns 0 on success, -1 if ACPI tables not found. */
int  acpi_init(void);

/* Power off the machine via ACPI S5 sleep state.
 * Falls back to QEMU-specific port if FADT is unavailable.
 * Does not return on success. */
void acpi_shutdown(void);

/* Reboot the machine via ACPI reset register or keyboard controller.
 * Does not return on success. */
void acpi_reboot(void);

/* ---- Consolidated MADT info (TODO-03 §1) ---- */

struct acpi_irq_override {
    uint8_t  bus_irq;       /* ISA IRQ number */
    uint32_t gsi;           /* Global System Interrupt */
    uint16_t flags;         /* polarity + trigger mode */
};

struct acpi_madt_info {
    uint32_t lapic_base;        /* LAPIC physical base (default 0xFEE00000) */
    uint32_t ioapic_base;       /* I/O APIC physical base (0 if not found) */
    uint32_t ioapic_gsi_base;   /* GSI base for this IOAPIC */
    uint32_t flags;             /* MADT flags (bit 0 = PCAT_COMPAT) */
    struct acpi_irq_override overrides[24];
    uint32_t override_count;
    uint8_t  cpu_lapic_ids[64];
    uint32_t cpu_count;
};

/* Get consolidated MADT info (populated after acpi_init). */
const struct acpi_madt_info *acpi_madt_info(void);

/* ---- SMP discovery API ---- */

/* Number of CPUs discovered in the MADT (1 = single-core / no MADT) */
uint32_t acpi_get_cpu_count(void);

/* Get CPU info by index (0 .. acpi_get_cpu_count()-1). Returns NULL if invalid. */
const struct cpu_info *acpi_get_cpu_info(uint32_t index);

/* LAPIC physical base address from MADT (default 0xFEE00000) */
uint32_t acpi_get_lapic_base(void);

/* I/O APIC physical base address (0 if not found) */
uint32_t acpi_get_ioapic_base(void);

/* Number of interrupt source overrides found */
uint32_t acpi_get_override_count(void);

/* Get an interrupt source override by index. Returns NULL if invalid. */
const struct madt_int_override *acpi_get_override(uint32_t index);

/* Returns 1 if MADT reports dual-8259 legacy PICs (PCAT_COMPAT=1).
 * Returns 0 for APIC-only platforms (Hyper-V Gen 2, hardware-reduced ACPI). */
uint8_t acpi_pcat_compat(void);

/* ---- Timer calibration helpers ---- */

/* HPET MMIO base address from the ACPI "HPET" table.
 * Returns 0 if no HPET table found. */
uint64_t acpi_get_hpet_base(void);

/* PM Timer I/O port from FADT PM_TMR_BLK (offset 76).
 * Returns 0 if FADT not available or PM Timer block not set. */
uint16_t acpi_get_pmtimer_port(void);

/* Returns 1 if PM Timer is 32-bit (FADT flags bit 8: TMR_VAL_EXT).
 * Returns 0 for 24-bit PM Timer. */
int acpi_pmtimer_is_32bit(void);

/* Returns 1 if FADT flags bit 20 (HW_REDUCED_ACPI) is set.
 * When set, legacy devices (PIT, PIC, RTC) do NOT exist. */
int acpi_hw_reduced(void);
