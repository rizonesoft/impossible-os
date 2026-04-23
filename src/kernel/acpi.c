/* ============================================================================
 * acpi.c -- ACPI table parsing and power management
 *
 * Walks the RSDP → RSDT/XSDT → FADT chain to discover the PM1a control
 * block port, which is used to initiate an S5 (soft-off) shutdown.
 *
 * The RSDP physical address is provided by the Multiboot2 bootloader and
 * stored in g_boot_info.acpi_rsdp_addr by multiboot2_parse.c.
 *
 * Shutdown sequence:
 *   1. Parse DSDT/SSDT for \_S5 sleep type value (SLP_TYPa)
 *   2. Write (SLP_TYPa | SLP_EN) to PM1a_CNT_BLK
 *   3. If that fails, fall back to QEMU-specific port 0x604
 *
 * Reboot:
 *   1. Try ACPI reset register (FADT 2.0+)
 *   2. Fall back to keyboard controller pulse (port 0x64)
 *   3. Fall back to triple fault
 * ============================================================================ */

#include "kernel/acpi.h"
#include "kernel/boot_info.h"
#include "kernel/idt.h"
#include "kernel/klog.h"
#include "kernel/printk.h"
#include "kernel/fs/fat32.h"

/* ---- I/O helpers ---- */

static inline void outb_acpi(uint16_t port, uint8_t val)
{
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline void outw_acpi(uint16_t port, uint16_t val)
{
    __asm__ volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline __attribute__((unused)) uint16_t inw_acpi(uint16_t port)
{
    uint16_t ret;
    __asm__ volatile("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline uint8_t inb_acpi(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* ---- Internal state ---- */

static const struct acpi_fadt *fadt_ptr = (const struct acpi_fadt *)0;
static uint16_t pm1a_cnt_port = 0;
static uint16_t slp_typa = 0;       /* S5 sleep type value */
static uint8_t  acpi_ready = 0;

/* Sleep type values for S1-S4 (parsed from DSDT \_Sx_ objects) */
#define ACPI_SLP_TYPE_INVALID 0xFFFF
static uint16_t slp_typa_s1 = ACPI_SLP_TYPE_INVALID;
static uint16_t slp_typa_s3 = ACPI_SLP_TYPE_INVALID;
static uint16_t slp_typa_s4 = ACPI_SLP_TYPE_INVALID;
static const struct acpi_sdt_header *s_dsdt_hdr;  /* cached for acpi_power_init */
static uint8_t  pcat_compat = 1;    /* MADT bit 0: 1=legacy PIC present, 0=APIC-only */

/* ---- SMP discovery state ---- */

static struct cpu_info        cpus[MAX_CPUS];
static uint32_t               cpu_count = 0;
static uint32_t               lapic_base_addr = 0xFEE00000; /* default */
static uint32_t               ioapic_base_addr = 0;
static struct madt_int_override int_overrides[24]; /* ISA only has 16, extra room */
static uint32_t               override_count = 0;
static uint32_t               ioapic_gsi_base = 0;

/* Consolidated MADT info struct (populated by parse_madt) */
static struct acpi_madt_info  s_madt_info;

/* ---- Helpers ---- */

/* Validate ACPI table checksum -- all bytes must sum to 0 */
static int acpi_checksum(const void *ptr, uint32_t length)
{
    const uint8_t *bytes = (const uint8_t *)ptr;
    uint8_t sum = 0;
    uint32_t i;

    for (i = 0; i < length; i++)
        sum += bytes[i];

    return sum == 0;
}

/* Compare 4-byte signature */
static int sig_match(const char *sig, const char *target)
{
    return sig[0] == target[0] && sig[1] == target[1] &&
           sig[2] == target[2] && sig[3] == target[3];
}

/* Find a table with the given 4-byte signature in the RSDT */
static const struct acpi_sdt_header *find_table_rsdt(
    const struct acpi_rsdt *rsdt, const char *sig)
{
    uint32_t entries;
    uint32_t i;

    entries = (rsdt->header.length - sizeof(struct acpi_sdt_header)) /
              sizeof(uint32_t);

    for (i = 0; i < entries; i++) {
        const struct acpi_sdt_header *hdr =
            (const struct acpi_sdt_header *)(uintptr_t)rsdt->entries[i];

        if (sig_match(hdr->signature, sig) &&
            acpi_checksum(hdr, hdr->length))
            return hdr;
    }

    return (const struct acpi_sdt_header *)0;
}

/* Find a table with the given 4-byte signature in the XSDT */
static const struct acpi_sdt_header *find_table_xsdt(
    const struct acpi_xsdt *xsdt, const char *sig)
{
    uint32_t entries;
    uint32_t i;

    entries = (xsdt->header.length - sizeof(struct acpi_sdt_header)) /
              sizeof(uint64_t);

    for (i = 0; i < entries; i++) {
        const struct acpi_sdt_header *hdr =
            (const struct acpi_sdt_header *)(uintptr_t)xsdt->entries[i];

        if (sig_match(hdr->signature, sig) &&
            acpi_checksum(hdr, hdr->length))
            return hdr;
    }

    return (const struct acpi_sdt_header *)0;
}

/* Find a table by signature using either RSDT or XSDT depending on ACPI version */
static const struct acpi_sdt_header *find_acpi_table(
    const struct acpi_rsdp *rsdp, const char *sig)
{
    if (g_boot_info.acpi_version >= 2) {
        const struct acpi_rsdp2 *rsdp2 = (const struct acpi_rsdp2 *)rsdp;
        if (rsdp2->xsdt_addr) {
            const struct acpi_xsdt *xsdt =
                (const struct acpi_xsdt *)(uintptr_t)rsdp2->xsdt_addr;
            return find_table_xsdt(xsdt, sig);
        }
    }
    /* Fall back to RSDT (32-bit) */
    const struct acpi_rsdt *rsdt =
        (const struct acpi_rsdt *)(uintptr_t)rsdp->rsdt_addr;
    return find_table_rsdt(rsdt, sig);
}

/* Parse \_Sx_ object from the DSDT to extract SLP_TYPa.
 *
 * The \_Sx_ object in the DSDT AML bytecode contains the sleep type values.
 * We search for the byte pattern:  '_' 'S' <digit> '_' followed by a package
 * encoding. Works on QEMU, Bochs, VirtualBox, and most real firmware.
 *
 * AML encoding of \_Sx_:
 *   NameOp (0x08) + '_Sx_' + PackageOp (0x12) + PkgLength + NumElements
 *   + BytePrefix (0x0A) + SLP_TYPa + ...
 *
 * Returns SLP_TYPa value, or ACPI_SLP_TYPE_INVALID if not found.
 */
static uint16_t parse_sleep_type(const struct acpi_sdt_header *dsdt,
                                  char state_digit)
{
    const uint8_t *data = (const uint8_t *)dsdt;
    uint32_t length = dsdt->length;
    uint32_t i;

    for (i = sizeof(struct acpi_sdt_header); i + 4 < length; i++) {
        if (data[i] == '_' && data[i + 1] == 'S' &&
            data[i + 2] == (uint8_t)state_digit && data[i + 3] == '_') {

            i += 4;

            /* Expect PackageOp (0x12) */
            if (i >= length || data[i] != 0x12)
                continue;
            i++;

            /* Skip PkgLength (variable-length encoding) */
            if (i >= length)
                continue;
            uint8_t pkg_lead = data[i];
            uint8_t pkg_len_bytes = (uint8_t)((pkg_lead >> 6) & 0x03);
            i += 1 + pkg_len_bytes;

            /* Skip NumElements byte */
            if (i >= length)
                continue;
            i++;

            /* First element: SLP_TYPa */
            if (i >= length)
                continue;

            if (data[i] == 0x0A) {
                i++;
                if (i >= length) continue;
                return (uint16_t)data[i];
            } else {
                return (uint16_t)data[i];
            }
        }
    }

    return ACPI_SLP_TYPE_INVALID;
}

/* Legacy wrapper for S5 -- returns 0 as default (QEMU compatible) */
static uint16_t parse_s5_from_dsdt(const struct acpi_sdt_header *dsdt)
{
    uint16_t val = parse_sleep_type(dsdt, '5');
    return (val == ACPI_SLP_TYPE_INVALID) ? 0 : val;
}

/* ---- MADT parsing ---- */

/* Read the BSP's LAPIC ID from the APIC base MSR */
static uint8_t read_bsp_lapic_id(void)
{
    /* Read from LAPIC ID register at offset 0x20 (identity-mapped) */
    volatile uint32_t *lapic_id_reg =
        (volatile uint32_t *)(uintptr_t)(lapic_base_addr + 0x20);
    return (uint8_t)((*lapic_id_reg >> 24) & 0xFF);
}

/* Parse the MADT to discover CPUs, I/O APIC, and interrupt overrides */
static void parse_madt(const struct acpi_madt *madt)
{
    const uint8_t *data = (const uint8_t *)madt;
    uint32_t length = madt->header.length;
    uint32_t offset;
    uint8_t bsp_lapic_id;

    /* Read LAPIC base from MADT header */
    lapic_base_addr = madt->lapic_addr;

    /* Read PCAT_COMPAT flag (bit 0): 1 = dual-8259 PICs installed, 0 = APIC-only */
    pcat_compat = (madt->flags & 1) ? 1 : 0;
    klog(LOG_INFO, "acpi",
         "MADT PCAT_COMPAT=%u%s",
         (uint64_t)pcat_compat,
         pcat_compat ? "" : " -- PIC absent, APIC-only mode");

    /* Get BSP LAPIC ID so we can mark it */
    bsp_lapic_id = read_bsp_lapic_id();

    /* Walk variable-length MADT entries starting after the fixed header */
    offset = sizeof(struct acpi_madt);

    while (offset + 2 <= length) {
        const struct madt_entry_header *entry =
            (const struct madt_entry_header *)(data + offset);

        if (entry->length < 2 || offset + entry->length > length)
            break;

        switch (entry->type) {
        case MADT_TYPE_LAPIC: {
            const struct madt_lapic *lapic =
                (const struct madt_lapic *)entry;

            klog(LOG_DEBUG, "acpi",
                 "  MADT[%u]: LAPIC acpi_id=%u apic_id=%u flags=0x%x",
                 (uint64_t)offset, (uint64_t)lapic->acpi_processor_id,
                 (uint64_t)lapic->apic_id, (uint64_t)lapic->flags);

            /* Only count enabled or online-capable CPUs */
            if ((lapic->flags & 0x01) || (lapic->flags & 0x02)) {
                if (cpu_count < MAX_CPUS) {
                    cpus[cpu_count].apic_id  = lapic->apic_id;
                    cpus[cpu_count].acpi_id  = lapic->acpi_processor_id;
                    cpus[cpu_count].is_bsp   = (lapic->apic_id == bsp_lapic_id) ? 1 : 0;
                    cpus[cpu_count].enabled   = (lapic->flags & 0x01) ? 1 : 0;
                    cpu_count++;
                }
            }
            break;
        }

        case 9: /* MADT_TYPE_X2APIC */ {
            /* x2APIC entry: 16 bytes total
             * offset 4: acpi_uid (uint32_t)
             * offset 8: flags (uint32_t)
             * offset 12: x2apic_id (uint32_t) */
            const uint8_t *e = data + offset;
            uint32_t x2_uid   = *(const uint32_t *)(e + 4);
            uint32_t x2_flags = *(const uint32_t *)(e + 8);
            uint32_t x2_id    = *(const uint32_t *)(e + 12);

            klog(LOG_DEBUG, "acpi",
                 "  MADT[%u]: x2APIC uid=%u id=%u flags=0x%x",
                 (uint64_t)offset, (uint64_t)x2_uid,
                 (uint64_t)x2_id, (uint64_t)x2_flags);

            if ((x2_flags & 0x01) || (x2_flags & 0x02)) {
                if (cpu_count < MAX_CPUS) {
                    cpus[cpu_count].apic_id  = (uint8_t)(x2_id & 0xFF);
                    cpus[cpu_count].acpi_id  = (uint8_t)(x2_uid & 0xFF);
                    cpus[cpu_count].is_bsp   = (x2_id == bsp_lapic_id) ? 1 : 0;
                    cpus[cpu_count].enabled   = (x2_flags & 0x01) ? 1 : 0;
                    cpu_count++;
                }
            }
            break;
        }

        case MADT_TYPE_IOAPIC: {
            const struct madt_ioapic *ioapic =
                (const struct madt_ioapic *)entry;

            /* Use the first I/O APIC found */
            if (ioapic_base_addr == 0) {
                ioapic_base_addr = ioapic->ioapic_addr;
                ioapic_gsi_base  = ioapic->gsi_base;
            }
            break;
        }

        case MADT_TYPE_INT_OVERRIDE: {
            const struct madt_int_override *ovr =
                (const struct madt_int_override *)entry;

            if (override_count < 24) {
                int_overrides[override_count] = *ovr;
                override_count++;
                klog(LOG_DEBUG, "acpi",
                     "  MADT override: IRQ %u -> GSI %u (flags=0x%x)",
                     (uint64_t)ovr->source, (uint64_t)ovr->gsi,
                     (uint64_t)ovr->flags);
            }
            break;
        }

        case MADT_TYPE_LAPIC_OVERRIDE: {
            /* 64-bit LAPIC address override -- update base */
            const uint64_t *addr64 =
                (const uint64_t *)(data + offset + 4);
            lapic_base_addr = (uint32_t)(*addr64);
            break;
        }

        default:
            klog(LOG_DEBUG, "acpi",
                 "  MADT[%u]: type=%u len=%u (skipped)",
                 (uint64_t)offset, (uint64_t)entry->type,
                 (uint64_t)entry->length);
            break; /* skip unknown entry types */
        }

        offset += entry->length;
    }

    /* Populate consolidated MADT info struct */
    s_madt_info.lapic_base       = lapic_base_addr;
    s_madt_info.ioapic_base      = ioapic_base_addr;
    s_madt_info.ioapic_gsi_base  = ioapic_gsi_base;
    s_madt_info.flags            = madt->flags;
    s_madt_info.override_count   = override_count;
    s_madt_info.cpu_count        = cpu_count;
    {
        uint32_t j;
        for (j = 0; j < override_count && j < 24; j++) {
            s_madt_info.overrides[j].bus_irq = int_overrides[j].source;
            s_madt_info.overrides[j].gsi     = int_overrides[j].gsi;
            s_madt_info.overrides[j].flags   = int_overrides[j].flags;
        }
        for (j = 0; j < cpu_count && j < 64; j++)
            s_madt_info.cpu_lapic_ids[j] = cpus[j].apic_id;
    }

    klog(LOG_INFO, "acpi",
         "MADT: %u CPUs, LAPIC=0x%x, IOAPIC=0x%x GSI=%u, %u overrides, PCAT_COMPAT=%u",
         (uint64_t)cpu_count, (uint64_t)lapic_base_addr,
         (uint64_t)ioapic_base_addr, (uint64_t)ioapic_gsi_base,
         (uint64_t)override_count, (uint64_t)pcat_compat);
}

const struct acpi_madt_info *acpi_madt_info(void)
{
    return &s_madt_info;
}

/* ---- Public API ---- */

int acpi_init(void)
{
    const struct acpi_rsdp *rsdp;
    const struct acpi_sdt_header *fadt_hdr;
    const struct acpi_sdt_header *dsdt_hdr;
    const struct acpi_sdt_header *madt_hdr;
    const struct acpi_fadt *fadt;

    if (!g_boot_info.acpi_available || !g_boot_info.acpi_rsdp_addr) {
        printk("[ACPI] No RSDP found\n");
        return -1;
    }

    rsdp = (const struct acpi_rsdp *)g_boot_info.acpi_rsdp_addr;

    /* Validate RSDP checksum */
    if (!acpi_checksum(rsdp, 20)) {
        printk("[ACPI] RSDP checksum invalid\n");
        return -1;
    }

    /* Find FADT ("FACP") via RSDT or XSDT */
    fadt_hdr = find_acpi_table(rsdp, "FACP");

    if (!fadt_hdr) {
        printk("[ACPI] FADT not found\n");
        return -1;
    }

    fadt = (const struct acpi_fadt *)fadt_hdr;
    fadt_ptr = fadt;

    /* Extract PM1a control block port */
    pm1a_cnt_port = (uint16_t)fadt->pm1a_control_block;

    /* Parse DSDT for \_S5 sleep type */
    dsdt_hdr = (const struct acpi_sdt_header *)(uintptr_t)fadt->dsdt;
    s_dsdt_hdr = dsdt_hdr;  /* cache for acpi_power_init() */
    if (dsdt_hdr && sig_match(dsdt_hdr->signature, "DSDT")) {
        slp_typa = parse_s5_from_dsdt(dsdt_hdr);
    }

    acpi_ready = 1;

    klog(LOG_INFO, "acpi", "ACPI: FADT at %p, PM1a_CNT=0x%x, SLP_TYPa=%u",
           (uint64_t)(uintptr_t)fadt, (uint64_t)pm1a_cnt_port,
           (uint64_t)slp_typa);

    /* Log IAPC_BOOT_ARCH flags for hardware detection */
    if (fadt->header.length >= 113) {
        klog(LOG_INFO, "acpi", "IAPC_BOOT_ARCH: 8042=%d RTC=%d MSI=%d VGA=%d HW_REDUCED=%d",
             (uint64_t)((fadt->boot_arch_flags >> 1) & 1),
             (uint64_t)((fadt->boot_arch_flags & (1u << 5)) ? 0 : 1),
             (uint64_t)((fadt->boot_arch_flags & (1u << 3)) ? 0 : 1),
             (uint64_t)((fadt->boot_arch_flags & (1u << 2)) ? 0 : 1),
             (uint64_t)((fadt->flags >> 20) & 1));
    }

    /* ---- Parse MADT for SMP discovery ---- */

    madt_hdr = find_acpi_table(rsdp, "APIC");

    if (madt_hdr) {
        parse_madt((const struct acpi_madt *)madt_hdr);

        klog(LOG_INFO, "acpi",
             "MADT: %u CPUs, LAPIC=0x%x, IOAPIC=0x%x, %u overrides",
             (uint64_t)cpu_count, (uint64_t)lapic_base_addr,
             (uint64_t)ioapic_base_addr, (uint64_t)override_count);
    } else {
        /* No MADT -- single CPU, no APIC routing */
        cpu_count = 1;
        cpus[0].apic_id = 0;
        cpus[0].acpi_id = 0;
        cpus[0].is_bsp  = 1;
        cpus[0].enabled  = 1;

        klog(LOG_WARN, "acpi", "No MADT found -- single-core mode");
    }

    return 0;
}

/* ---- Power init (Phase 2) -- S-state discovery -------------------------- */

void acpi_power_init(void)
{
    if (!s_dsdt_hdr || !sig_match(s_dsdt_hdr->signature, "DSDT")) {
        klog(LOG_WARN, "acpi", "ACPI power: no DSDT -- S1/S3/S4 unavailable");
        return;
    }

    slp_typa_s1 = parse_sleep_type(s_dsdt_hdr, '1');
    slp_typa_s3 = parse_sleep_type(s_dsdt_hdr, '3');
    slp_typa_s4 = parse_sleep_type(s_dsdt_hdr, '4');

    klog(LOG_INFO, "acpi", "Sleep states: S1=%s S3=%s S4=%s S5=yes",
         slp_typa_s1 != ACPI_SLP_TYPE_INVALID ? "yes" : "no",
         slp_typa_s3 != ACPI_SLP_TYPE_INVALID ? "yes" : "no",
         slp_typa_s4 != ACPI_SLP_TYPE_INVALID ? "yes" : "no");

    if (slp_typa_s3 != ACPI_SLP_TYPE_INVALID)
        klog(LOG_INFO, "acpi", "S3 suspend-to-RAM: SLP_TYPa=%u",
             (uint64_t)slp_typa_s3);
}

int acpi_sleep_supported(uint8_t state)
{
    switch (state) {
    case 1: return slp_typa_s1 != ACPI_SLP_TYPE_INVALID;
    case 3: return slp_typa_s3 != ACPI_SLP_TYPE_INVALID;
    case 4: return slp_typa_s4 != ACPI_SLP_TYPE_INVALID;
    case 5: return 1;  /* S5 always supported (shutdown) */
    default: return 0;
    }
}

uint16_t acpi_get_slp_typa(uint8_t state)
{
    switch (state) {
    case 1: return slp_typa_s1;
    case 3: return slp_typa_s3;
    case 4: return slp_typa_s4;
    case 5: return slp_typa;
    default: return ACPI_SLP_TYPE_INVALID;
    }
}

/* ---- ACPI fixed event enable + SCI ISR ---------------------------------- */

static uint16_t s_pm1a_sts_port;   /* PM1a_EVT status register */
static uint16_t s_pm1a_en_port;    /* PM1a_EVT enable register */

void acpi_enable_fixed_events(void)
{
    uint16_t en;

    if (!fadt_ptr || !fadt_ptr->pm1a_event_block) {
        klog(LOG_WARN, "acpi", "No PM1a event block -- fixed events unavailable");
        return;
    }

    /* PM1a_STS is at pm1a_event_block, PM1a_EN is at pm1a_event_block + half */
    s_pm1a_sts_port = (uint16_t)fadt_ptr->pm1a_event_block;
    s_pm1a_en_port  = (uint16_t)(fadt_ptr->pm1a_event_block
                                 + fadt_ptr->pm1_event_length / 2);

    /* Clear any pending status bits first */
    outw_acpi(s_pm1a_sts_port, 0xFFFF);

    /* Enable power button (bit 8) and sleep button (bit 9) events */
    en = inw_acpi(s_pm1a_en_port);
    en |= (1u << 8) | (1u << 9);  /* PWRBTN_EN | SLPBTN_EN */
    outw_acpi(s_pm1a_en_port, en);

    klog(LOG_INFO, "acpi", "Fixed events: PWRBTN_EN + SLPBTN_EN on PM1a 0x%x/0x%x",
         (uint64_t)s_pm1a_sts_port, (uint64_t)s_pm1a_en_port);
}

/* SCI interrupt handler -- dispatches ACPI fixed events */
static uint64_t acpi_sci_handler(struct interrupt_frame *frame)
{
    uint16_t sts;

    if (!s_pm1a_sts_port) goto eoi;

    sts = inw_acpi(s_pm1a_sts_port);

    if (sts & (1u << 8)) {
        /* Power button pressed */
        outw_acpi(s_pm1a_sts_port, (1u << 8));  /* clear PWRBTN_STS */
        klog(LOG_INFO, "acpi", "Power button pressed (SCI)");
        /* Power button handler dispatch pending -- see power management roadmap. */
    }

    if (sts & (1u << 9)) {
        /* Sleep button pressed */
        outw_acpi(s_pm1a_sts_port, (1u << 9));  /* clear SLPBTN_STS */
        klog(LOG_INFO, "acpi", "Sleep button pressed (SCI)");
    }

    if (sts & (1u << 15)) {
        /* WAK_STS -- system just woke from sleep */
        outw_acpi(s_pm1a_sts_port, (1u << 15));  /* clear WAK_STS */
        klog(LOG_INFO, "acpi", "Wake event detected (WAK_STS)");
    }

eoi:
    {
        extern void lapic_eoi(void);
        lapic_eoi();
    }
    return (uint64_t)frame;
}

void acpi_register_sci(void)
{
    uint8_t sci_vec;

    if (!fadt_ptr) return;

    /* SCI interrupt number from FADT */
    sci_vec = (uint8_t)(32 + fadt_ptr->sci_interrupt);

    idt_register_handler(sci_vec, acpi_sci_handler);

    klog(LOG_INFO, "acpi", "SCI handler registered (IRQ %u, vec %u)",
         (uint64_t)fadt_ptr->sci_interrupt, (uint64_t)sci_vec);
}

/* ---- Generic sleep state entry ------------------------------------------ */

int acpi_enter_sleep_state(uint8_t state)
{
    uint16_t typa = acpi_get_slp_typa(state);
    uint16_t val;

    if (typa == ACPI_SLP_TYPE_INVALID) {
        klog(LOG_DEBUG, "acpi", "S%u not supported by firmware", (uint64_t)state);
        return -1;
    }

    if (!acpi_ready || !pm1a_cnt_port) {
        klog(LOG_ERROR, "acpi", "ACPI not ready -- cannot enter S%u",
             (uint64_t)state);
        return -1;
    }

    klog(LOG_INFO, "acpi", "Entering S%u (SLP_TYPa=%u)...",
         (uint64_t)state, (uint64_t)typa);

    /* Step 1: Disable interrupts */
    __asm__ volatile ("cli");

    /* Step 2: Clear SLP_EN before writing SLP_TYP (ACPI spec requirement) */
    {
        uint16_t cur = inw_acpi(pm1a_cnt_port);
        outw_acpi(pm1a_cnt_port, (uint16_t)(cur & ~(1u << 13)));
    }

    /* Step 3: Write (SLP_TYPa << 10) | SLP_EN to PM1a_CNT */
    val = (uint16_t)(typa << 10) | (1u << 13);
    outw_acpi(pm1a_cnt_port, val);

    /* Write PM1b_CNT if present */
    if (fadt_ptr && fadt_ptr->pm1b_control_block)
        outw_acpi((uint16_t)fadt_ptr->pm1b_control_block, val);

    /* Step 4: For S1, CPU halts here and resumes on wakeup interrupt.
     * For S3/S4, CPU loses context -- resume is via wakeup vector (not
     * implemented yet; requires CPU state save from the power-management
     * sleep/resume roadmap). */
    __asm__ volatile ("sti; hlt");

    /* If we reach here, we woke up from S1 (or S3 resume vector jumped here) */
    klog(LOG_INFO, "acpi", "Resumed from S%u", (uint64_t)state);
    return 0;
}

void acpi_shutdown(void)
{
    printk("[ACPI] Initiating shutdown...\n");

    /* Clean unmount BlackBox (X:\) -- clears dirty bit */
    {
        extern int vfs_is_mounted(char letter);
        extern struct vfs_node *vfs_get_drive_root(char letter);
        if (vfs_is_mounted('X')) {
            struct vfs_node *x_root = vfs_get_drive_root('X');
            if (x_root) {
                struct fat32_volume *vol = fat32_volume_from_root(x_root);
                if (vol) fat32_mark_clean(vol);
            }
        }
    }

    /* Disable interrupts -- we're going down */
    __asm__ volatile("cli");

    if (acpi_ready && pm1a_cnt_port) {
        /* Write SLP_TYPa | SLP_EN (bit 13) to PM1a_CNT */
        uint16_t val = (uint16_t)(slp_typa << 10) | (1 << 13);
        outw_acpi(pm1a_cnt_port, val);

        /* If PM1b exists, write there too */
        if (fadt_ptr && fadt_ptr->pm1b_control_block) {
            outw_acpi((uint16_t)fadt_ptr->pm1b_control_block, val);
        }
    }

    /* Fallback: QEMU-specific ACPI power-off port */
    outw_acpi(0x604, 0x2000);

    /* Fallback: Bochs/older QEMU */
    outw_acpi(0xB004, 0x2000);

    /* If we're still here, halt */
    printk("[ACPI] Shutdown failed -- halting\n");
    for (;;)
        __asm__ volatile("hlt");
}

void acpi_reboot(void)
{
    printk("[ACPI] Initiating reboot...\n");

    __asm__ volatile("cli");

    /* Method 1: ACPI reset register (FADT 2.0+) */
    if (acpi_ready && fadt_ptr &&
        fadt_ptr->header.revision >= 2 &&
        fadt_ptr->reset_reg.address != 0) {

        if (fadt_ptr->reset_reg.address_space == 1) {
            /* System I/O space */
            outb_acpi((uint16_t)fadt_ptr->reset_reg.address,
                      fadt_ptr->reset_value);
        } else if (fadt_ptr->reset_reg.address_space == 0) {
            /* System memory */
            volatile uint8_t *addr =
                (volatile uint8_t *)(uintptr_t)fadt_ptr->reset_reg.address;
            *addr = fadt_ptr->reset_value;
        }

        /* Give it a moment */
        {
            volatile uint32_t i;
            for (i = 0; i < 10000000; i++)
                ;
        }
    }

    /* Method 2: Keyboard controller reset (classic x86) */
    {
        /* Wait for keyboard controller input buffer to be empty */
        uint32_t timeout = 100000;
        while ((inb_acpi(0x64) & 0x02) && timeout > 0)
            timeout--;

        /* Send reset command: pulse CPU reset line */
        outb_acpi(0x64, 0xFE);

        /* Give it a moment */
        {
            volatile uint32_t i;
            for (i = 0; i < 10000000; i++)
                ;
        }
    }

    /* Method 3: Triple fault (last resort) */
    {
        /* Load a null IDT and trigger an interrupt → triple fault → reset */
        struct {
            uint16_t limit;
            uint64_t base;
        } __attribute__((packed)) null_idt = { 0, 0 };

        __asm__ volatile("lidt %0" : : "m"(null_idt));
        __asm__ volatile("int $0x03");
    }

    /* Should never reach here */
    for (;;)
        __asm__ volatile("hlt");
}

/* ---- SMP discovery API ---- */

uint32_t acpi_get_cpu_count(void)
{
    return cpu_count;
}

const struct cpu_info *acpi_get_cpu_info(uint32_t index)
{
    if (index >= cpu_count)
        return (const struct cpu_info *)0;
    return &cpus[index];
}

uint32_t acpi_get_lapic_base(void)
{
    return lapic_base_addr;
}

uint32_t acpi_get_ioapic_base(void)
{
    return ioapic_base_addr;
}

uint32_t acpi_get_override_count(void)
{
    return override_count;
}

const struct madt_int_override *acpi_get_override(uint32_t index)
{
    if (index >= override_count)
        return (const struct madt_int_override *)0;
    return &int_overrides[index];
}

uint8_t acpi_pcat_compat(void)
{
    return pcat_compat;
}

/* ---- Timer calibration helpers ---- */

/* HPET table structure (ACPI spec: Table 5-37)
 * Offset 0:  standard header (36 bytes)
 * Offset 36: event_timer_block_id (uint32_t)
 * Offset 40: base_address (GAS -- 12 bytes: space=0, width=64, offset=0, size=0, addr)
 * The MMIO base is at GAS.address (offset 44 in the table). */

uint64_t acpi_get_hpet_base(void)
{
    const struct acpi_rsdp *rsdp;
    const struct acpi_sdt_header *hpet_hdr;
    const uint8_t *data;

    if (!g_boot_info.acpi_available || !g_boot_info.acpi_rsdp_addr)
        return 0;

    rsdp = (const struct acpi_rsdp *)g_boot_info.acpi_rsdp_addr;
    hpet_hdr = find_acpi_table(rsdp, "HPET");
    if (!hpet_hdr)
        return 0;

    /* HPET table must be at least 56 bytes (header + block_id + GAS) */
    if (hpet_hdr->length < 56)
        return 0;

    data = (const uint8_t *)hpet_hdr;

    /* Base address is at offset 44 (GAS.address field) */
    uint64_t base = *(const uint64_t *)(data + 44);
    return base;
}

uint16_t acpi_get_pmtimer_port(void)
{
    if (!fadt_ptr)
        return 0;
    return (uint16_t)fadt_ptr->pm_timer_block;
}

int acpi_pmtimer_is_32bit(void)
{
    if (!fadt_ptr)
        return 0;
    /* FADT flags bit 8: TMR_VAL_EXT -- 1 = 32-bit PM Timer */
    return (fadt_ptr->flags & (1u << 8)) ? 1 : 0;
}

int acpi_hw_reduced(void)
{
    if (!fadt_ptr)
        return 0;
    /* FADT flags bit 20: HW_REDUCED_ACPI -- legacy devices absent */
    return (fadt_ptr->flags & (1u << 20)) ? 1 : 0;
}

/* ---- FADT IAPC_BOOT_ARCH flags (offset 109, 16-bit) ----
 * Bit 0: LEGACY_DEVICES -- 8042 required
 * Bit 1: 8042 -- i8042 controller present
 * Bit 2: VGA_NOT_PRESENT -- do not probe VGA
 * Bit 3: MSI_NOT_SUPPORTED -- do not enable MSI
 * Bit 4: PCIe_ASPM -- PCIe ASPM must not be disabled
 * Bit 5: CMOS_RTC_NOT_PRESENT -- do not access CMOS RTC */

int acpi_has_8042(void)
{
    if (!fadt_ptr)
        return 1;  /* assume present if no FADT */
    /* FADT length must be >= 113 for boot_arch_flags to be valid */
    if (fadt_ptr->header.length < 113)
        return 1;  /* too short -- assume present */
    return (fadt_ptr->boot_arch_flags & (1u << 1)) ? 1 : 0;
}

int acpi_has_cmos_rtc(void)
{
    if (!fadt_ptr)
        return 1;
    if (fadt_ptr->header.length < 113)
        return 1;
    /* Bit 5: CMOS_RTC_NOT_PRESENT -- inverted: 0=present, 1=absent */
    return (fadt_ptr->boot_arch_flags & (1u << 5)) ? 0 : 1;
}

int acpi_msi_supported(void)
{
    if (!fadt_ptr)
        return 1;
    if (fadt_ptr->header.length < 113)
        return 1;
    /* Bit 3: MSI_NOT_SUPPORTED -- inverted: 0=supported, 1=not supported */
    return (fadt_ptr->boot_arch_flags & (1u << 3)) ? 0 : 1;
}

int acpi_has_vga(void)
{
    if (!fadt_ptr)
        return 1;
    if (fadt_ptr->header.length < 113)
        return 1;
    /* Bit 2: VGA_NOT_PRESENT -- inverted: 0=present, 1=absent */
    return (fadt_ptr->boot_arch_flags & (1u << 2)) ? 0 : 1;
}
