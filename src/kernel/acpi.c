/* ============================================================================
 * acpi.c -- ACPI table parsing and power management
 *
 * Walks the RSDP → RSDT/XSDT → FADT chain to discover the PM1a control
 * block port, which is used to initiate an S5 (soft-off) shutdown.
 *
 * The RSDP physical address is provided by the UEFI bootloader and
 * stored in g_boot_info.acpi_rsdp_addr.
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
#include "kernel/mm/vmm.h"
#include "kernel/idt.h"
#include "kernel/klog.h"
#include "kernel/cache.h"
#include "kernel/printk.h"
#include "kernel/fs/fat32.h"
#include "kernel/irq.h"
#include "kernel/drivers/pic.h"
#include "kernel/drivers/ioapic.h"

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

static struct cpu_info        cpus[MAX_CPUS] = {
    { 0, 0, 1, 1 }  /* BSP placeholder until MADT parse */
};
static uint32_t               cpu_count = 1;  /* BSP-only until MADT parse */
static uint32_t               lapic_base_addr = 0xFEE00000; /* default */
static uint32_t               ioapic_base_addr = 0;
static struct madt_int_override int_overrides[24]; /* ISA only has 16, extra room */
static uint32_t               override_count = 0;
static uint32_t               ioapic_gsi_base = 0;

/* Consolidated MADT info struct (populated by parse_madt). Static defaults
 * mirror the legacy accessors so the consolidated getter agrees with them
 * even when acpi_init() fails before the MADT is reached. */
static struct acpi_madt_info  s_madt_info = {
    .lapic_base = 0xFEE00000,
    .flags      = 0x1,          /* PCAT assumed present */
    .cpu_count  = 1,
};

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

/* Read the BSP's LAPIC ID register through a UC mapping (device MMIO must
 * never be read through the WB identity map). The mapping is cached per
 * physical base so a reparse does not leak mapping pages. */
static uint8_t read_bsp_lapic_id(void)
{
    static volatile uint32_t *s_lapic_map = (volatile uint32_t *)0;
    static uint32_t s_lapic_map_phys = 0;

    if (!s_lapic_map || s_lapic_map_phys != lapic_base_addr) {
        s_lapic_map = (volatile uint32_t *)
            vmm_map_mmio_uc(lapic_base_addr, 4096);
        s_lapic_map_phys = lapic_base_addr;
        if (!s_lapic_map) {
            klog(LOG_WARN, "acpi",
                 "UC map of LAPIC at 0x%x failed -- assuming BSP id 0",
                 (uint64_t)lapic_base_addr);
            return 0;
        }
    }
    /* LAPIC ID register at offset 0x20, id in bits 24-31 */
    return (uint8_t)((s_lapic_map[0x20 / 4] >> 24) & 0xFF);
}

/* Typed MADT record sizes (ACPI 6.x Table 5-21 ff.) for length validation.
 * Firmware-controlled entry->length must cover the full typed payload
 * before the cast -- a short record would read past the declared table. */
#define MADT_X2APIC_ENTRY_LEN        16  /* type 9: hdr + rsvd + id + flags + uid */
#define MADT_LAPIC_OVERRIDE_LEN      12  /* type 5: hdr + rsvd(2) + addr64 */

/* Reset every MADT-derived global to the BSP-only legacy defaults. Runs
 * before EVERY parse outcome (valid table, short table, no MADT) so a
 * reparse can never blend state from two tables. */
static void madt_reset_state(void)
{
    cpu_count = 0;
    override_count = 0;
    lapic_base_addr = 0xFEE00000;
    ioapic_base_addr = 0;
    ioapic_gsi_base = 0;
    pcat_compat = 1;
}

/* Publish the consolidated MADT view consumed by acpi_madt_info().
 * Runs on every parse outcome (full table, short table, no MADT) so the
 * consolidated getter never disagrees with the legacy accessors. */
static void madt_publish_info(uint32_t flags)
{
    uint32_t j;

    s_madt_info.lapic_base       = lapic_base_addr;
    s_madt_info.ioapic_base      = ioapic_base_addr;
    s_madt_info.ioapic_gsi_base  = ioapic_gsi_base;
    s_madt_info.flags            = flags;
    s_madt_info.override_count   = override_count;
    s_madt_info.cpu_count        = cpu_count;
    for (j = 0; j < override_count &&
                j < sizeof(s_madt_info.overrides) / sizeof(s_madt_info.overrides[0]); j++) {
        s_madt_info.overrides[j].bus_irq = int_overrides[j].source;
        s_madt_info.overrides[j].gsi     = int_overrides[j].gsi;
        s_madt_info.overrides[j].flags   = int_overrides[j].flags;
    }
    for (j = 0; j < cpu_count && j < sizeof(s_madt_info.cpu_lapic_ids); j++)
        s_madt_info.cpu_lapic_ids[j] = cpus[j].apic_id;
}

/* Parse the MADT to discover CPUs, I/O APIC, and interrupt overrides */
static void parse_madt(const struct acpi_madt *madt)
{
    const uint8_t *data = (const uint8_t *)madt;
    uint32_t length = madt->header.length;
    uint32_t offset;
    uint8_t bsp_lapic_id;
    uint32_t x2apic_skipped = 0, x2apic_skip_max = 0;

    madt_reset_state();

    /* The fixed MADT body (SDT header + lapic_addr + flags) must be present
     * before any field past the SDT header is trusted. */
    if (length < sizeof(struct acpi_madt)) {
        klog(LOG_ERROR, "acpi",
             "MADT body too short (%u bytes, need %u) -- table ignored, single-core fallback",
             (uint64_t)length, (uint64_t)sizeof(struct acpi_madt));
        cpu_count = 1;
        cpus[0].apic_id = 0;
        cpus[0].acpi_id = 0;
        cpus[0].is_bsp  = 1;
        cpus[0].enabled = 1;
        madt_publish_info(0x1);  /* PCAT assumed present, matches default */
        return;
    }

    /* Read LAPIC base from MADT header */
    lapic_base_addr = madt->lapic_addr;

    /* Read PCAT_COMPAT flag (bit 0): 1 = dual-8259 PICs installed, 0 = APIC-only */
    pcat_compat = (madt->flags & 1) ? 1 : 0;
    klog(LOG_INFO, "acpi",
         "MADT PCAT_COMPAT=%u%s",
         (uint64_t)pcat_compat,
         pcat_compat ? "" : " -- PIC absent, APIC-only mode");

    /* Pass 1: apply the 64-bit LAPIC address override (type 5) BEFORE the
     * first LAPIC MMIO access -- the BSP ID read below must use the real
     * base when firmware relocated it. */
    offset = sizeof(struct acpi_madt);
    while (offset + sizeof(struct madt_entry_header) <= length) {
        const struct madt_entry_header *entry =
            (const struct madt_entry_header *)(data + offset);

        if (entry->length < sizeof(struct madt_entry_header) ||
            offset + entry->length > length)
            break;

        if (entry->type == MADT_TYPE_LAPIC_OVERRIDE) {
            if (entry->length < MADT_LAPIC_OVERRIDE_LEN) {
                klog(LOG_WARN, "acpi",
                     "MADT[%u]: short LAPIC address override (len=%u) -- skipped",
                     (uint64_t)offset, (uint64_t)entry->length);
            } else {
                uint64_t addr64 =
                    *(const uint64_t *)(const void *)(data + offset + 4);
                if (addr64 > 0xFFFFFFFFull) {
                    /* LAPIC bases above 4 GiB never occur on shipping
                     * hardware; reject loudly instead of truncating. */
                    klog(LOG_ERROR, "acpi",
                         "LAPIC address override above 4 GiB unsupported -- keeping 0x%x",
                         (uint64_t)lapic_base_addr);
                } else if (addr64 != 0) {
                    lapic_base_addr = (uint32_t)addr64;
                }
            }
        }
        offset += entry->length;
    }

    /* Get BSP LAPIC ID so we can mark it (base is final after pass 1) */
    bsp_lapic_id = read_bsp_lapic_id();

    /* Pass 2: walk variable-length MADT entries starting after the fixed
     * header. Every typed record validates entry->length against the full
     * typed payload before the cast -- firmware data is untrusted input. */
    offset = sizeof(struct acpi_madt);

    while (offset + sizeof(struct madt_entry_header) <= length) {
        const struct madt_entry_header *entry =
            (const struct madt_entry_header *)(data + offset);

        if (entry->length < sizeof(struct madt_entry_header) ||
            offset + entry->length > length)
            break;

        switch (entry->type) {
        case MADT_TYPE_LAPIC: {
            const struct madt_lapic *lapic =
                (const struct madt_lapic *)entry;

            if (entry->length < sizeof(struct madt_lapic)) {
                klog(LOG_WARN, "acpi",
                     "MADT[%u]: short LAPIC record (len=%u) -- skipped",
                     (uint64_t)offset, (uint64_t)entry->length);
                break;
            }

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

        case MADT_TYPE_X2APIC: {
            /* Processor Local x2APIC (ACPI 6.x): rsvd(2) at offset 2,
             * X2APIC ID (4) at offset 4, Flags (4) at offset 8,
             * ACPI Processor UID (4) at offset 12. 16 bytes total. */
            const uint8_t *e = data + offset;
            uint32_t x2_uid, x2_flags, x2_id;

            if (entry->length < MADT_X2APIC_ENTRY_LEN) {
                klog(LOG_WARN, "acpi",
                     "MADT[%u]: short x2APIC record (len=%u) -- skipped",
                     (uint64_t)offset, (uint64_t)entry->length);
                break;
            }
            x2_id    = *(const uint32_t *)(e + 4);
            x2_flags = *(const uint32_t *)(e + 8);
            x2_uid   = *(const uint32_t *)(e + 12);

            klog(LOG_DEBUG, "acpi",
                 "  MADT[%u]: x2APIC uid=%u id=%u flags=0x%x",
                 (uint64_t)offset, (uint64_t)x2_uid,
                 (uint64_t)x2_id, (uint64_t)x2_flags);

            if (x2_id > 0xFF) {
                /* IDs above 255 cannot be addressed in xAPIC mode; the
                 * MSR-based x2APIC send path (APIC interrupt routing TODO)
                 * is required before these CPUs can be started. One summary
                 * WARN after the walk -- large servers can have hundreds. */
                x2apic_skipped++;
                if (x2_id > x2apic_skip_max)
                    x2apic_skip_max = x2_id;
                break;
            }

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

            if (entry->length < sizeof(struct madt_ioapic)) {
                klog(LOG_WARN, "acpi",
                     "MADT[%u]: short IOAPIC record (len=%u) -- skipped",
                     (uint64_t)offset, (uint64_t)entry->length);
                break;
            }

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

            if (entry->length < sizeof(struct madt_int_override)) {
                klog(LOG_WARN, "acpi",
                     "MADT[%u]: short interrupt override (len=%u) -- skipped",
                     (uint64_t)offset, (uint64_t)entry->length);
                break;
            }

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

        case MADT_TYPE_LAPIC_OVERRIDE:
            /* Applied in pass 1 (before the BSP LAPIC ID read) */
            break;

        default:
            klog(LOG_DEBUG, "acpi",
                 "  MADT[%u]: type=%u len=%u (skipped)",
                 (uint64_t)offset, (uint64_t)entry->type,
                 (uint64_t)entry->length);
            break; /* skip unknown entry types */
        }

        offset += entry->length;
    }

    if (x2apic_skipped) {
        klog(LOG_WARN, "acpi",
             "%u x2APIC CPUs skipped (ids up to %u exceed xAPIC 8-bit addressing)",
             (uint64_t)x2apic_skipped, (uint64_t)x2apic_skip_max);
    }

    /* The running BSP must be represented in the CPU set. If it is missing
     * (no records, or the BSP's own x2APIC id was skipped above), fail
     * closed to BSP-only mode: no AP is ever started, so 8-bit physical
     * routing to the BSP's xAPIC id stays unambiguous. */
    {
        uint32_t j;
        int bsp_found = 0;
        for (j = 0; j < cpu_count; j++) {
            if (cpus[j].is_bsp) {
                bsp_found = 1;
                break;
            }
        }
        if (!bsp_found) {
            if (x2apic_skipped) {
                klog(LOG_ERROR, "acpi",
                     "BSP not in usable MADT CPU set (x2APIC ids skipped) -- "
                     "x2APIC mode required; forcing BSP-only, no AP bringup");
            } else if (cpu_count > 0) {
                klog(LOG_ERROR, "acpi",
                     "BSP apic_id=%u missing from MADT CPU set -- forcing BSP-only",
                     (uint64_t)bsp_lapic_id);
            } else {
                klog(LOG_WARN, "acpi",
                     "MADT listed no usable CPUs -- BSP-only fallback");
            }
            cpus[0].apic_id = bsp_lapic_id;
            cpus[0].acpi_id = 0;
            cpus[0].is_bsp  = 1;
            cpus[0].enabled = 1;
            cpu_count = 1;
        }
    }

    /* Populate consolidated MADT info struct */
    madt_publish_info(madt->flags);

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

    /* FADT fixed fields read below (pm1a_control_block @64, dsdt @40, and the
     * flags/boot_arch_flags fields the capability accessors read at @109/@112)
     * require the ACPI 1.0 minimum FADT length of 116 bytes. The table is
     * firmware-supplied and the checksum does not bound its length, so reject
     * a truncated FADT before dereferencing past its declared end -- leaving
     * fadt_ptr NULL makes the accessors safe-default to legacy-present. */
    if (fadt_hdr->length < 116) {
        printk("[ACPI] FADT too short (%u < 116) -- ignoring\n",
               (uint32_t)fadt_hdr->length);
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
             (uint64_t)acpi_hw_reduced());  /* length-guarded flags read */
    }

    /* ---- Parse MADT for SMP discovery ---- */

    madt_hdr = find_acpi_table(rsdp, "APIC");

    if (madt_hdr) {
        parse_madt((const struct acpi_madt *)madt_hdr);
        /* parse_madt() logs the full "MADT: ..." summary line */
    } else {
        /* No MADT -- single CPU, no APIC routing */
        madt_reset_state();
        cpu_count = 1;
        cpus[0].apic_id = 0;
        cpus[0].acpi_id = 0;
        cpus[0].is_bsp  = 1;
        cpus[0].enabled  = 1;
        madt_publish_info(0x1);  /* PCAT assumed present, matches default */

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

/* SCI body -- services PM1a fixed events and reports whether THIS
 * controller raised the interrupt (the SCI line may be shared) */
static int acpi_sci_process(void)
{
    uint16_t sts;
    int handled = 0;

    if (!s_pm1a_sts_port) return 0;

    sts = inw_acpi(s_pm1a_sts_port);

    if (sts & (1u << 8)) {
        /* Power button pressed */
        outw_acpi(s_pm1a_sts_port, (1u << 8));  /* clear PWRBTN_STS */
        klog(LOG_INFO, "acpi", "Power button pressed (SCI)");
        /* Power button handler dispatch pending -- see power management roadmap. */
        handled = 1;
    }

    if (sts & (1u << 9)) {
        /* Sleep button pressed */
        outw_acpi(s_pm1a_sts_port, (1u << 9));  /* clear SLPBTN_STS */
        klog(LOG_INFO, "acpi", "Sleep button pressed (SCI)");
        handled = 1;
    }

    if (sts & (1u << 15)) {
        /* WAK_STS -- system just woke from sleep */
        outw_acpi(s_pm1a_sts_port, (1u << 15));  /* clear WAK_STS */
        klog(LOG_INFO, "acpi", "Wake event detected (WAK_STS)");
        handled = 1;
    }

    return handled;
}

/* Shared-chain registrant for the IOAPIC GSI path (EOI owned by the
 * irq dispatch wrapper) */
static int acpi_sci_shared(uint8_t vector, void *ctx)
{
    (void)vector; (void)ctx;
    return acpi_sci_process() ? IRQ_HANDLED : IRQ_NONE;
}

/* IDT-level handler for the PIC-only fallback path; EOI is
 * controller-aware (PIC-delivered ISA vector needs the PIC EOI) */
static uint64_t acpi_sci_handler(struct interrupt_frame *frame)
{
    acpi_sci_process();
    irq_eoi(irq_vector_to_isa((uint8_t)frame->int_no));
    return (uint64_t)frame;
}

void acpi_register_sci(void)
{
    uint8_t sci_vec;

    if (!fadt_ptr) return;

    if (ioapic_available()) {
        /* FADT SCI_INT below 16 is an ISA IRQ (translate through MADT
         * overrides to its GSI); 16 and above it is already a GSI
         * (ACPI 6.x FADT SCI_INT definition). Route through the GSI API
         * so the IOAPIC redirection entry actually exists -- the old
         * bare IDT install at 32+n received nothing on IOAPIC systems
         * and used the pre-remap slave-PIC vector math (IRQ9 -> 0x29,
         * while ISA IRQ9 delivers at 0x71 since the S4 remap). */
        uint32_t gsi = (fadt_ptr->sci_interrupt < 16)
                           ? ioapic_isa_to_gsi((uint8_t)fadt_ptr->sci_interrupt)
                           : (uint32_t)fadt_ptr->sci_interrupt;
        /* SCI is level-triggered active-low per the ACPI spec (0x0F);
         * irq_request_gsi_ex itself treats a MADT override naming this
         * GSI as authoritative (including flags 0 = conforms-to-bus),
         * so no pre-resolution is needed here */
        sci_vec = irq_request_gsi_ex(gsi, acpi_sci_shared, (void *)0,
                                     "acpi-sci", 0x0F, 1);
        if (!sci_vec) {
            klog(LOG_ERROR, "acpi",
                 "SCI GSI %u not routable -- SCI not registered",
                 (uint64_t)gsi);
            return;
        }
        klog(LOG_INFO, "acpi", "SCI registered (SCI_INT %u, GSI %u, vec 0x%x)",
             (uint64_t)fadt_ptr->sci_interrupt, (uint64_t)gsi,
             (uint64_t)sci_vec);
        return;
    }

    /* PIC-only fallback: SCI must be an ISA IRQ, delivered at the
     * canonical ISA vector (0x20-0x27 master, 0x70-0x77 slave remap) */
    if (fadt_ptr->sci_interrupt >= 16) {
        klog(LOG_ERROR, "acpi",
             "SCI_INT %u is a GSI but no IOAPIC -- SCI not registered",
             (uint64_t)fadt_ptr->sci_interrupt);
        return;
    }
    sci_vec = isa_irq_to_vector((uint8_t)fadt_ptr->sci_interrupt);
    if (!sci_vec) {
        klog(LOG_ERROR, "acpi", "SCI ISA IRQ %u has no vector -- not registered",
             (uint64_t)fadt_ptr->sci_interrupt);
        return;
    }
    idt_register_handler(sci_vec, acpi_sci_handler);
    pic_unmask_irq((uint8_t)fadt_ptr->sci_interrupt);

    klog(LOG_INFO, "acpi", "SCI handler registered (ISA IRQ %u, vec 0x%x, PIC)",
         (uint64_t)fadt_ptr->sci_interrupt, (uint64_t)sci_vec);
}

/* ---- Generic sleep state entry ------------------------------------------ */

int acpi_enter_sleep_state(uint8_t state)
{
    uint16_t typa = acpi_get_slp_typa(state);
    uint16_t val;

    /* S5 (soft-off) is NOT a resumable sleep state: it must run the storage
     * durability barrier and then power off without ever returning. That is
     * acpi_shutdown()'s job (quiesce + PM1 + fallback ports + halt forever).
     * Reject S5 here so the generic S1-S4 resume tail (sti; hlt; return) can
     * never run after storage has been quiesced, leaving a live kernel with
     * shut-down storage. */
    if (state == 5) {
        klog(LOG_WARN, "acpi", "S5 must use acpi_shutdown(), not the sleep API");
        return -1;
    }

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

/* Pre-storage-quiesce shared by poweroff + reboot: flush the X: filesystem
 * sector cache to the block device (so the clean bit + pending writes reach the
 * controller cache), then flush + cleanly shut down every storage controller
 * (NVMe CC.SHN). MUST run with interrupts enabled (driver shutdown paths poll
 * via hlt). Multi-volume FS flush/unmount is the clean-shutdown orchestrator's
 * job; this covers the BlackBox X: volume that ACPI itself marks clean. */
static void acpi_storage_quiesce(void)
{
    extern int vfs_is_mounted(char letter);
    extern struct vfs_node *vfs_get_drive_root(char letter);
    extern int vfs_flush(struct vfs_node *node);
    extern int blkdev_shutdown_all(void);

    if (vfs_is_mounted('X')) {
        struct vfs_node *x_root = vfs_get_drive_root('X');
        if (x_root) {
            struct fat32_volume *vol = fat32_volume_from_root(x_root);
            if (vol) fat32_mark_clean(vol);
            if (vfs_flush(x_root) != 0)
                printk("[ACPI] WARNING: X: cache flush failed -- "
                       "clean bit may not be durable\n");
        }
    }
    if (blkdev_shutdown_all() != 0)
        printk("[ACPI] WARNING: storage did not cleanly quiesce\n");
}

/* Power off WITHOUT the (interrupt-requiring) storage quiesce -- IF-off-safe,
 * for callers already running under cli (the degraded-boot recovery screen).
 * Everything below is port writes. Does not return on success. */
void acpi_poweroff_now(void)
{
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

void acpi_shutdown(void)
{
    printk("[ACPI] Initiating shutdown...\n");
    /* Flush X: + cleanly shut down storage controllers BEFORE the poweroff (the
     * quiesce sleeps, so it needs interrupts enabled). The recovery screen runs
     * under cli and uses acpi_poweroff_now() directly instead. */
    acpi_storage_quiesce();
    acpi_poweroff_now();
}

/* Reset the machine WITHOUT the storage quiesce. acpi_storage_quiesce() sleeps
 * via sleep_ms()/hlt and so requires interrupts enabled; the degraded-boot
 * recovery screen reaches a reset with interrupts already disabled (the keypress
 * poll runs under cli), so it calls this directly. Everything below is port
 * writes + triple-fault -- all IF-off-safe. */
void acpi_reset_now(void)
{
    __asm__ volatile("cli");

    /* Commit dirty lines before ANY of the three reset methods below, because
     * a reset invalidates the caches without writing them back (kernel/cache.h)
     * and every method here ends in one. Filesystem state does recover on its
     * own -- the A/B slot tries-counter and the journal exist for that -- but
     * the cross-boot crash record has no journal behind it, and losing it
     * costs the user the explanation for the reboot they are about to see.
     * Defence in depth only: this runs on the calling CPU, does not quiesce
     * the others, and does not wait for external caches, so a writer that
     * needs its own record durable persists it at publication time instead of
     * relying on reaching here. */
    cache_writeback_all();

    /* Method 1: ACPI reset register (FADT 2.0+).
     * revision >= 2 alone does not prove the table is long enough -- reset_reg
     * (GAS) and reset_value sit at offset 116+, past the 116-byte init floor,
     * so a malformed short rev-2 FADT would overread. Require the declared
     * length to cover reset_value before touching either field. */
    if (acpi_ready && fadt_ptr &&
        fadt_ptr->header.length >= (__builtin_offsetof(struct acpi_fadt, reset_value)
                                    + sizeof(fadt_ptr->reset_value)) &&
        fadt_ptr->header.revision >= 2 &&
        fadt_ptr->reset_reg.address != 0) {

        if (fadt_ptr->reset_reg.address_space == 1) {
            /* System I/O space -- a plain port write, always safe. */
            outb_acpi((uint16_t)fadt_ptr->reset_reg.address,
                      fadt_ptr->reset_value);
        }
        /* SystemMemory (address_space == 0) reset registers are intentionally
         * NOT used here: writing one means a raw MMIO store through the
         * firmware physical address, which is not mapped UC at this point (and
         * this path runs with interrupts disabled, e.g. the degraded-boot
         * recovery [R] escape) -- a #PF/wedge risk that violates the "no MMIO
         * through unmapped/WB pages" rule. Fall through to the keyboard-
         * controller reset + triple-fault below, which need no mapping. */

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

void acpi_reboot(void)
{
    printk("[ACPI] Initiating reboot...\n");

    /* Flush X: + shut down storage controllers before the reset (the quiesce
     * sleeps, so it needs interrupts enabled -- callers in the normal,
     * interrupts-enabled path use this; the degraded-boot recovery screen,
     * which runs under cli, calls acpi_reset_now() directly instead). */
    acpi_storage_quiesce();
    acpi_reset_now();
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
    /* pm_timer_block is at FADT offset 76 (4 bytes) -- needs length >= 80. */
    if (fadt_ptr->header.length < 80)
        return 0;
    return (uint16_t)fadt_ptr->pm_timer_block;
}

int acpi_pmtimer_is_32bit(void)
{
    if (!fadt_ptr)
        return 0;
    /* flags is at FADT offset 112 (4 bytes) -- needs length >= 116 (same
     * bounds rule as acpi_hw_reduced); a short table cannot prove TMR_VAL_EXT. */
    if (fadt_ptr->header.length < 116)
        return 0;
    /* FADT flags bit 8: TMR_VAL_EXT -- 1 = 32-bit PM Timer */
    return (fadt_ptr->flags & (1u << 8)) ? 1 : 0;
}

static inline uint32_t pmtimer_inl(uint16_t port)
{
    uint32_t v;
    __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

/* Median of three (the middle value). With two close real samples and one
 * glitched outlier, returns a real sample. */
static inline uint32_t pmtimer_median3(uint32_t a, uint32_t b, uint32_t c)
{
    uint32_t lo = a < b ? a : b;
    uint32_t hi = a < b ? b : a;
    if (c < lo) return lo;
    if (c > hi) return hi;
    return c;
}

/* Centralized, glitch-filtered PM timer read (all consumers route here:
 * mono_clock PMTMR source, LAPIC calibration, csprng entropy). Some broken
 * chipsets latch a wrong value on a single read of the ACPI PM timer; read
 * three times back-to-back (~us total, far under the 24-bit wrap of ~4.7 s,
 * so the true sequence is monotonic) and take the median to reject a lone
 * outlier. */
uint32_t acpi_pmtimer_read_value(void)
{
    uint16_t port = acpi_get_pmtimer_port();
    uint32_t mask, a, b, c;

    if (port == 0)
        return 0;
    mask = acpi_pmtimer_is_32bit() ? 0xFFFFFFFFu : 0x00FFFFFFu;
    a = pmtimer_inl(port) & mask;
    b = pmtimer_inl(port) & mask;
    c = pmtimer_inl(port) & mask;
    return pmtimer_median3(a, b, c);
}

int acpi_hw_reduced(void)
{
    if (!fadt_ptr)
        return 0;
    /* flags is at FADT offset 112 (4 bytes) -- reading it needs length >= 116
     * (ACPI 1.0 minimum FADT). A truncated/malformed table cannot be declared
     * hardware-reduced; treat it as legacy so the bounds check is not bypassed
     * by the capability accessors that call this first. */
    if (fadt_ptr->header.length < 116)
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
    /* Hardware-reduced ACPI platforms have no legacy fixed hardware (no
     * i8042/CMOS/VGA legacy ports), regardless of the IAPC_BOOT_ARCH bits,
     * which are reserved/ignored under the hardware-reduced model. Override
     * to absent so the PS/2 path is never probed there. */
    if (acpi_hw_reduced())
        return 0;
    if (!fadt_ptr)
        return 1;  /* assume present if no FADT */
    /* FADT length must be >= 113 for boot_arch_flags to be valid */
    if (fadt_ptr->header.length < 113)
        return 1;  /* too short -- assume present */
    return (fadt_ptr->boot_arch_flags & (1u << 1)) ? 1 : 0;
}

int acpi_has_cmos_rtc(void)
{
    if (acpi_hw_reduced())
        return 0;  /* hardware-reduced: no CMOS RTC */
    if (!fadt_ptr)
        return 1;
    if (fadt_ptr->header.length < 113)
        return 1;
    /* Bit 5: CMOS_RTC_NOT_PRESENT -- inverted: 0=present, 1=absent */
    return (fadt_ptr->boot_arch_flags & (1u << 5)) ? 0 : 1;
}

uint8_t acpi_rtc_century_index(void)
{
    /* The FADT "century" field names the CMOS register that holds the century
     * byte, or 0 when the platform has no century register. Hardcoding 0x32 is
     * wrong: on firmware without one, that register reads bus-float and yields
     * a wild year. Return 0 (no century register) unless the FADT advertises a
     * valid one. */
    if (acpi_hw_reduced())
        return 0;
    if (!fadt_ptr)
        return 0;
    if (fadt_ptr->header.length < __builtin_offsetof(struct acpi_fadt, century) + 1)
        return 0;
    return fadt_ptr->century;
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
    if (acpi_hw_reduced())
        return 0;  /* hardware-reduced: no legacy VGA */
    if (!fadt_ptr)
        return 1;
    if (fadt_ptr->header.length < 113)
        return 1;
    /* Bit 2: VGA_NOT_PRESENT -- inverted: 0=present, 1=absent */
    return (fadt_ptr->boot_arch_flags & (1u << 2)) ? 0 : 1;
}

/* ---- Win32 GetSystemFirmwareTable surface ---------------------------------
 *
 * Hostile-input contract: every accessor below treats firmware-supplied
 * pointers, lengths, and counts as untrusted. The internal
 * find_acpi_table is single-shot boot-time code; these public accessors
 * are reachable from user-mode via NtQuerySystemInformation(
 * SystemFirmwareTableInformation) and therefore must reject every
 * malformed shape before dereferencing.
 */

/* Conservative cap on the SDT root entry count. ACPI 6.4 doesn't
 * specify an upper bound, but real systems carry < 64 entries; a 1024
 * cap rejects malicious headers without blocking legitimate platforms. */
#define ACPI_ROOT_ENTRY_MAX 1024

/* Validate the root SDT header (RSDT or XSDT): signature, length range,
 * length covers an entry-count integer multiple, and full checksum.
 * Returns the validated entry count on success, 0 on rejection. The
 * caller derives entry count from header.length and entry stride. */
static uint32_t acpi_validate_root(const struct acpi_sdt_header *root,
                                    uint32_t entry_stride,
                                    const char *expected_sig)
{
    if (!root || !sig_match(root->signature, expected_sig))
        return 0;
    /* length must cover the header plus at least zero entries and not
     * exceed a sane upper bound (1 MiB). */
    if (root->length < sizeof(struct acpi_sdt_header) ||
        root->length > 1024U * 1024U)
        return 0;
    uint32_t entries_bytes = root->length - sizeof(struct acpi_sdt_header);
    if (entries_bytes % entry_stride != 0)
        return 0;
    uint32_t count = entries_bytes / entry_stride;
    if (count > ACPI_ROOT_ENTRY_MAX)
        return 0;
    if (!acpi_checksum(root, root->length))
        return 0;
    return count;
}

/* Validate a child SDT pointed to by RSDT/XSDT. Returns the SDT length
 * on success, 0 on rejection. Caller passes the firmware-mapped
 * pointer; we read header.length and full-table checksum, both of
 * which the firmware controls. */
static uint32_t acpi_validate_child(const struct acpi_sdt_header *hdr)
{
    if (!hdr) return 0;
    /* Header.length must cover the SDT header itself and not exceed a
     * 16 MiB sanity cap (largest legitimate ACPI table is < 1 MiB). */
    if (hdr->length < sizeof(struct acpi_sdt_header) ||
        hdr->length > 16U * 1024U * 1024U)
        return 0;
    if (!acpi_checksum(hdr, hdr->length))
        return 0;
    return hdr->length;
}

int acpi_enumerate_signatures(uint32_t *out_sigs, uint32_t max_count,
                               uint32_t *out_total)
{
    if (!out_total)
        return 0;
    *out_total = 0;

    if (!g_boot_info.acpi_available || !g_boot_info.acpi_rsdp_addr)
        return 0;

    const struct acpi_rsdp *rsdp =
        (const struct acpi_rsdp *)g_boot_info.acpi_rsdp_addr;

    /* Prefer XSDT on ACPI 2.0+; fall back to RSDT. */
    if (g_boot_info.acpi_version >= 2) {
        const struct acpi_rsdp2 *rsdp2 = (const struct acpi_rsdp2 *)rsdp;
        if (rsdp2->xsdt_addr) {
            const struct acpi_xsdt *xsdt =
                (const struct acpi_xsdt *)(uintptr_t)rsdp2->xsdt_addr;
            uint32_t count = acpi_validate_root(&xsdt->header,
                                                 sizeof(uint64_t), "XSDT");
            if (count == 0)
                return 0;
            uint32_t i, written = 0;
            for (i = 0; i < count; i++) {
                const struct acpi_sdt_header *hdr =
                    (const struct acpi_sdt_header *)(uintptr_t)xsdt->entries[i];
                if (acpi_validate_child(hdr) == 0)
                    continue;
                if (out_sigs && written < max_count) {
                    uint32_t sig = (uint32_t)(uint8_t)hdr->signature[0] |
                                   ((uint32_t)(uint8_t)hdr->signature[1] << 8) |
                                   ((uint32_t)(uint8_t)hdr->signature[2] << 16) |
                                   ((uint32_t)(uint8_t)hdr->signature[3] << 24);
                    out_sigs[written++] = sig;
                }
                (*out_total)++;
            }
            return 1;
        }
    }

    /* RSDT fallback */
    if (!rsdp->rsdt_addr)
        return 0;
    const struct acpi_rsdt *rsdt =
        (const struct acpi_rsdt *)(uintptr_t)rsdp->rsdt_addr;
    uint32_t count = acpi_validate_root(&rsdt->header,
                                         sizeof(uint32_t), "RSDT");
    if (count == 0)
        return 0;
    uint32_t i, written = 0;
    for (i = 0; i < count; i++) {
        const struct acpi_sdt_header *hdr =
            (const struct acpi_sdt_header *)(uintptr_t)rsdt->entries[i];
        if (acpi_validate_child(hdr) == 0)
            continue;
        if (out_sigs && written < max_count) {
            uint32_t sig = (uint32_t)(uint8_t)hdr->signature[0] |
                           ((uint32_t)(uint8_t)hdr->signature[1] << 8) |
                           ((uint32_t)(uint8_t)hdr->signature[2] << 16) |
                           ((uint32_t)(uint8_t)hdr->signature[3] << 24);
            out_sigs[written++] = sig;
        }
        (*out_total)++;
    }
    return 1;
}

int acpi_get_raw_table(uint32_t signature, const uint8_t **out_addr,
                        uint32_t *out_size)
{
    if (!out_addr || !out_size)
        return 0;
    *out_addr = (const uint8_t *)0;
    *out_size = 0;

    if (!g_boot_info.acpi_available || !g_boot_info.acpi_rsdp_addr)
        return 0;

    char want[4];
    want[0] = (char)(signature & 0xFF);
    want[1] = (char)((signature >> 8) & 0xFF);
    want[2] = (char)((signature >> 16) & 0xFF);
    want[3] = (char)((signature >> 24) & 0xFF);

    const struct acpi_rsdp *rsdp =
        (const struct acpi_rsdp *)g_boot_info.acpi_rsdp_addr;

    if (g_boot_info.acpi_version >= 2) {
        const struct acpi_rsdp2 *rsdp2 = (const struct acpi_rsdp2 *)rsdp;
        if (rsdp2->xsdt_addr) {
            const struct acpi_xsdt *xsdt =
                (const struct acpi_xsdt *)(uintptr_t)rsdp2->xsdt_addr;
            uint32_t count = acpi_validate_root(&xsdt->header,
                                                 sizeof(uint64_t), "XSDT");
            if (count == 0) return 0;
            uint32_t i;
            for (i = 0; i < count; i++) {
                const struct acpi_sdt_header *hdr =
                    (const struct acpi_sdt_header *)(uintptr_t)xsdt->entries[i];
                uint32_t hdr_len = acpi_validate_child(hdr);
                if (hdr_len == 0) continue;
                if (sig_match(hdr->signature, want)) {
                    *out_addr = (const uint8_t *)hdr;
                    *out_size = hdr_len;
                    return 1;
                }
            }
            return 0;
        }
    }

    if (!rsdp->rsdt_addr) return 0;
    const struct acpi_rsdt *rsdt =
        (const struct acpi_rsdt *)(uintptr_t)rsdp->rsdt_addr;
    uint32_t count = acpi_validate_root(&rsdt->header,
                                         sizeof(uint32_t), "RSDT");
    if (count == 0) return 0;
    uint32_t i;
    for (i = 0; i < count; i++) {
        const struct acpi_sdt_header *hdr =
            (const struct acpi_sdt_header *)(uintptr_t)rsdt->entries[i];
        uint32_t hdr_len = acpi_validate_child(hdr);
        if (hdr_len == 0) continue;
        if (sig_match(hdr->signature, want)) {
            *out_addr = (const uint8_t *)hdr;
            *out_size = hdr_len;
            return 1;
        }
    }
    return 0;
}

int acpi_is_ready(void)
{
    return acpi_ready != 0;
}

uint32_t acpi_for_each_record(acpi_record_cb_t cb, void *ctx)
{
    if (!cb) return 0;
    if (!acpi_ready) return 0;
    if (!g_boot_info.acpi_available || !g_boot_info.acpi_rsdp_addr) return 0;

    const struct acpi_rsdp *rsdp =
        (const struct acpi_rsdp *)g_boot_info.acpi_rsdp_addr;
    uint32_t emitted = 0;

    if (g_boot_info.acpi_version >= 2) {
        const struct acpi_rsdp2 *rsdp2 = (const struct acpi_rsdp2 *)rsdp;
        if (rsdp2->xsdt_addr) {
            const struct acpi_xsdt *xsdt =
                (const struct acpi_xsdt *)(uintptr_t)rsdp2->xsdt_addr;
            uint32_t count = acpi_validate_root(&xsdt->header,
                                                 sizeof(uint64_t), "XSDT");
            if (count == 0) return 0;
            for (uint32_t i = 0; i < count; i++) {
                const struct acpi_sdt_header *hdr =
                    (const struct acpi_sdt_header *)(uintptr_t)xsdt->entries[i];
                uint32_t hdr_len = acpi_validate_child(hdr);
                if (hdr_len == 0) continue;
                uint32_t sig =
                    (uint32_t)(uint8_t)hdr->signature[0] |
                    ((uint32_t)(uint8_t)hdr->signature[1] << 8) |
                    ((uint32_t)(uint8_t)hdr->signature[2] << 16) |
                    ((uint32_t)(uint8_t)hdr->signature[3] << 24);
                if (!cb(emitted, sig, (const uint8_t *)hdr, hdr_len, ctx))
                    return emitted + 1;
                emitted++;
            }
            return emitted;
        }
    }

    if (!rsdp->rsdt_addr) return 0;
    const struct acpi_rsdt *rsdt =
        (const struct acpi_rsdt *)(uintptr_t)rsdp->rsdt_addr;
    uint32_t count = acpi_validate_root(&rsdt->header,
                                         sizeof(uint32_t), "RSDT");
    if (count == 0) return 0;
    for (uint32_t i = 0; i < count; i++) {
        const struct acpi_sdt_header *hdr =
            (const struct acpi_sdt_header *)(uintptr_t)rsdt->entries[i];
        uint32_t hdr_len = acpi_validate_child(hdr);
        if (hdr_len == 0) continue;
        uint32_t sig =
            (uint32_t)(uint8_t)hdr->signature[0] |
            ((uint32_t)(uint8_t)hdr->signature[1] << 8) |
            ((uint32_t)(uint8_t)hdr->signature[2] << 16) |
            ((uint32_t)(uint8_t)hdr->signature[3] << 24);
        if (!cb(emitted, sig, (const uint8_t *)hdr, hdr_len, ctx))
            return emitted + 1;
        emitted++;
    }
    return emitted;
}

int acpi_get_record(uint32_t index, uint32_t *out_sig,
                    const uint8_t **out_addr, uint32_t *out_size)
{
    if (!out_addr || !out_size) return 0;
    *out_addr = (const uint8_t *)0;
    *out_size = 0;
    if (out_sig) *out_sig = 0;

    if (!acpi_ready) return 0;
    if (!g_boot_info.acpi_available || !g_boot_info.acpi_rsdp_addr)
        return 0;

    const struct acpi_rsdp *rsdp =
        (const struct acpi_rsdp *)g_boot_info.acpi_rsdp_addr;

    /* Iterate validated SDTs in the same order as acpi_enumerate_signatures
     * so duplicate-signature tables (multiple SSDTs) get distinct ordinals. */
    uint32_t seen = 0;

    if (g_boot_info.acpi_version >= 2) {
        const struct acpi_rsdp2 *rsdp2 = (const struct acpi_rsdp2 *)rsdp;
        if (rsdp2->xsdt_addr) {
            const struct acpi_xsdt *xsdt =
                (const struct acpi_xsdt *)(uintptr_t)rsdp2->xsdt_addr;
            uint32_t count = acpi_validate_root(&xsdt->header,
                                                 sizeof(uint64_t), "XSDT");
            if (count == 0) return 0;
            for (uint32_t i = 0; i < count; i++) {
                const struct acpi_sdt_header *hdr =
                    (const struct acpi_sdt_header *)(uintptr_t)xsdt->entries[i];
                uint32_t hdr_len = acpi_validate_child(hdr);
                if (hdr_len == 0) continue;
                if (seen == index) {
                    if (out_sig) {
                        *out_sig =
                            (uint32_t)(uint8_t)hdr->signature[0] |
                            ((uint32_t)(uint8_t)hdr->signature[1] << 8) |
                            ((uint32_t)(uint8_t)hdr->signature[2] << 16) |
                            ((uint32_t)(uint8_t)hdr->signature[3] << 24);
                    }
                    *out_addr = (const uint8_t *)hdr;
                    *out_size = hdr_len;
                    return 1;
                }
                seen++;
            }
            return 0;
        }
    }

    if (!rsdp->rsdt_addr) return 0;
    const struct acpi_rsdt *rsdt =
        (const struct acpi_rsdt *)(uintptr_t)rsdp->rsdt_addr;
    uint32_t count = acpi_validate_root(&rsdt->header,
                                         sizeof(uint32_t), "RSDT");
    if (count == 0) return 0;
    for (uint32_t i = 0; i < count; i++) {
        const struct acpi_sdt_header *hdr =
            (const struct acpi_sdt_header *)(uintptr_t)rsdt->entries[i];
        uint32_t hdr_len = acpi_validate_child(hdr);
        if (hdr_len == 0) continue;
        if (seen == index) {
            if (out_sig) {
                *out_sig =
                    (uint32_t)(uint8_t)hdr->signature[0] |
                    ((uint32_t)(uint8_t)hdr->signature[1] << 8) |
                    ((uint32_t)(uint8_t)hdr->signature[2] << 16) |
                    ((uint32_t)(uint8_t)hdr->signature[3] << 24);
            }
            *out_addr = (const uint8_t *)hdr;
            *out_size = hdr_len;
            return 1;
        }
        seen++;
    }
    return 0;
}
