/* ============================================================================
 * acpi.c — ACPI table parsing and power management
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
#include "kernel/printk.h"

/* ---- I/O helpers ---- */

static inline void outb_acpi(uint16_t port, uint8_t val)
{
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline void outw_acpi(uint16_t port, uint16_t val)
{
    __asm__ volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint16_t inw_acpi(uint16_t port)
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

/* ---- Helpers ---- */

/* Validate ACPI table checksum — all bytes must sum to 0 */
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

/* Parse \_S5 object from the DSDT to extract SLP_TYPa.
 *
 * The \_S5 object in the DSDT AML bytecode contains the sleep type values.
 * We search for the byte pattern:  '_' 'S' '5' '_' followed by a package
 * encoding.  This is a simplified parser that works on QEMU, Bochs, and
 * most real BIOS implementations.
 *
 * AML encoding of \_S5:
 *   NameOp (0x08) + '_S5_' + PackageOp (0x12) + PkgLength + NumElements
 *   + BytePrefix (0x0A) + SLP_TYPa + ...
 */
static uint16_t parse_s5_from_dsdt(const struct acpi_sdt_header *dsdt)
{
    const uint8_t *data = (const uint8_t *)dsdt;
    uint32_t length = dsdt->length;
    uint32_t i;

    for (i = sizeof(struct acpi_sdt_header); i + 4 < length; i++) {
        /* Look for "_S5_" */
        if (data[i] == '_' && data[i + 1] == 'S' &&
            data[i + 2] == '5' && data[i + 3] == '_') {

            /* Verify it's preceded by a NameOp (0x08) or is part of a scope */
            /* Skip to the package data after "_S5_" */
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

            /* Now we should be at the first element: SLP_TYPa */
            if (i >= length)
                continue;

            if (data[i] == 0x0A) {
                /* BytePrefix — next byte is the value */
                i++;
                if (i >= length) continue;
                return (uint16_t)data[i];
            } else {
                /* Direct byte value (some BIOSes omit the prefix) */
                return (uint16_t)data[i];
            }
        }
    }

    /* Default: QEMU uses SLP_TYPa = 0 for S5 */
    return 0;
}

/* ---- Public API ---- */

int acpi_init(void)
{
    const struct acpi_rsdp *rsdp;
    const struct acpi_sdt_header *fadt_hdr;
    const struct acpi_sdt_header *dsdt_hdr;
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
    if (g_boot_info.acpi_version >= 2) {
        /* Try XSDT first (64-bit addresses) */
        const struct acpi_rsdp2 *rsdp2 = (const struct acpi_rsdp2 *)rsdp;
        if (rsdp2->xsdt_addr) {
            const struct acpi_xsdt *xsdt =
                (const struct acpi_xsdt *)(uintptr_t)rsdp2->xsdt_addr;
            fadt_hdr = find_table_xsdt(xsdt, "FACP");
        } else {
            const struct acpi_rsdt *rsdt =
                (const struct acpi_rsdt *)(uintptr_t)rsdp->rsdt_addr;
            fadt_hdr = find_table_rsdt(rsdt, "FACP");
        }
    } else {
        const struct acpi_rsdt *rsdt =
            (const struct acpi_rsdt *)(uintptr_t)rsdp->rsdt_addr;

        if (!acpi_checksum(&rsdt->header, rsdt->header.length)) {
            printk("[ACPI] RSDT checksum invalid\n");
            return -1;
        }

        fadt_hdr = find_table_rsdt(rsdt, "FACP");
    }

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
    if (dsdt_hdr && sig_match(dsdt_hdr->signature, "DSDT")) {
        slp_typa = parse_s5_from_dsdt(dsdt_hdr);
    }

    acpi_ready = 1;

    printk("[OK] ACPI: FADT at %p, PM1a_CNT=%x, SLP_TYPa=%u\n",
           (uint64_t)(uintptr_t)fadt, (uint64_t)pm1a_cnt_port,
           (uint64_t)slp_typa);

    return 0;
}

void acpi_shutdown(void)
{
    printk("[ACPI] Initiating shutdown...\n");

    /* Disable interrupts — we're going down */
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
    printk("[ACPI] Shutdown failed — halting\n");
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
