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
#include "kernel/smp.h"

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

/* A legacy ACPI System-I/O address is a 16-bit port. Firmware fields carrying
 * one are 32 bits wide, so a malformed value survives a nonzero test and then
 * NARROWS to something else entirely -- 0x10000 becomes port 0. Return 0 for
 * anything that does not fit, so "nonzero" and "usable" mean the same thing
 * everywhere downstream. */
static uint16_t acpi_io_port(uint32_t addr)
{
    if (!addr || addr > 0xFFFFu)
        return 0;
    return (uint16_t)addr;
}

static inline uint32_t inl_acpi(uint16_t port)
{
    uint32_t ret;
    __asm__ volatile("inl %1, %0" : "=a"(ret) : "Nd"(port));
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

/* Sleep type values parsed from the DSDT \_Sx_ objects. INVALID means the
 * object was absent or did not decode -- it is never a usable SLP_TYP. S5 in
 * particular must NOT fall back to an invented type 0: acpi_poweroff_now()
 * runs after storage is quiesced, so writing an arbitrary sleep type there
 * can request a state that is not soft-off. ACPI 6.5 section 7.4.2 makes
 * SLP_TYPa and SLP_TYPb independent per-register values, so each state
 * carries both. */
#define ACPI_SLP_TYPE_INVALID 0xFFFF
static uint16_t slp_typa = ACPI_SLP_TYPE_INVALID;    /* S5 SLP_TYPa */
static uint16_t slp_typb = ACPI_SLP_TYPE_INVALID;    /* S5 SLP_TYPb */
static uint8_t  acpi_ready = 0;

static uint16_t slp_typa_s1 = ACPI_SLP_TYPE_INVALID;
static uint16_t slp_typb_s1 = ACPI_SLP_TYPE_INVALID;
static uint16_t slp_typa_s3 = ACPI_SLP_TYPE_INVALID;
static uint16_t slp_typb_s3 = ACPI_SLP_TYPE_INVALID;
static uint16_t slp_typa_s4 = ACPI_SLP_TYPE_INVALID;
static uint16_t slp_typb_s4 = ACPI_SLP_TYPE_INVALID;
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

/* Upper bound on a firmware-declared ACPI table length. An ACPI header's
 * checksum does not bound its own `length` field, so a forged or corrupt
 * length would otherwise drive every table walk past the mapped extent.
 * 16 MiB is far above any real DSDT (tens of KB) and small enough that the
 * `i + 3` reach of the scanners below provably cannot wrap uint32_t. */
#define ACPI_MAX_TABLE_LENGTH (16u * 1024u * 1024u)

/* Validate a firmware-supplied table BEFORE any code trusts its length:
 * non-NULL, expected signature, a length that covers at least the header and
 * at most ACPI_MAX_TABLE_LENGTH, and a correct checksum over exactly that
 * length. find_table_rsdt()/find_table_xsdt() already apply the checksum half
 * to tables they return; the DSDT is reached through the FADT's `dsdt` field
 * instead and so was never checked at all. */
/* Is [phys, phys + len) contained in a SINGLE firmware memory-map descriptor of
 * a type that can hold an ACPI table?
 *
 * The numeric length cap prevents unsigned wrap but proves nothing about
 * mapping: a corrupt-but-sub-cap length can still run off the end of its
 * descriptor into reclaimed memory, MMIO, or a hole, and acpi_checksum()
 * dereferences the WHOLE declared span before anything can reject it. So the
 * extent is checked against the UEFI map the loader handed us BEFORE the first
 * read of the table body.
 *
 * Returns 1 when contained, and also 1 when the map cannot answer -- a
 * truncated or absent map is missing evidence, and refusing on it would drop
 * sleep-state support on machines whose firmware simply has more descriptors
 * than the handoff carries. */
static int acpi_extent_mapped(uint64_t phys, uint64_t len)
{
    uint32_t i;

    /* ALWAYS scan the descriptors we were given. A truncated map still holds
     * usable ones, and skipping the scan outright turned validation into
     * unconditional approval on exactly the machines with the most complex
     * memory layouts. */
    for (i = 0; i < g_boot_info.mmap_count && i < BOOT_MMAP_MAX_ENTRIES; i++) {
        const struct boot_mmap_entry *e = &g_boot_info.mmap[i];

        /* Admit only classes that are readable RAM holding firmware tables. The
         * SIMPLIFIED type is not enough: the loader folds EfiMemoryMappedIO,
         * EfiMemoryMappedIOPortSpace and other reserved classes into type 2, and
         * checksumming a "table" in one of those performs device-register reads.
         * The original UEFI type is carried per descriptor for this reason. */
        /* Does the extent lie in THIS descriptor at all? Established before the
         * type test, so a hit on a rejected class is recorded as a rejection
         * rather than silently skipped. */
        if (phys < e->base_addr)
            continue;
        if (len > e->length)
            continue;
        if (phys - e->base_addr > e->length - len)
            continue;              /* runs past this descriptor's end */

        /* Admit ONLY the classes an ACPI table actually lives in. Conventional,
         * loader and boot-services memory are excluded deliberately: pmm_init
         * returns those to the allocator, so a table "validated" there can be
         * overwritten afterwards. MMIO classes are excluded because
         * checksumming across them performs device-register reads. */
        switch (e->uefi_memory_type) {
        case UEFI_MMAP_ACPI_RECLAIM:
        case UEFI_MMAP_ACPI_NVS:
        case UEFI_MMAP_RESERVED:
            return 1;
        default:
            /* Contained, but in a class a table must not be in. This is
             * POSITIVE evidence the pointer is wrong, so it outranks the
             * truncated-map benefit of the doubt below. */
            klog(LOG_WARN, "acpi",
                 "table extent lies in unusable memory class %u -- rejected",
                 (uint64_t)e->uefi_memory_type);
            return 0;
        }
    }

    /* Outside every descriptor we were given. Only a map we KNOW is incomplete
     * earns the benefit of the doubt; a complete map that does not contain the
     * table is positive evidence the extent is wrong. */
    if (!g_boot_info.mmap_count || g_boot_info.mmap_truncated) {
        klog(LOG_WARN, "acpi",
             "table extent unprovable (memory map absent or truncated)");
        return 1;
    }

    return 0;
}

/* STRUCTURAL validity: signature, a length that covers the header and stays
 * under the cap, and a correct checksum over exactly that length. Split out
 * from the containment policy below so it can be exercised against synthetic
 * tables -- a unit test's table lives in ordinary test memory, which is
 * correctly NOT an ACPI-class descriptor, so a combined function could only
 * ever be tested by refusing it, and every structural case would then pass for
 * the wrong reason. Assumes hdr's declared extent is already known readable. */
static int acpi_table_struct_valid(const struct acpi_sdt_header *hdr,
                                   const char *sig)
{
    if (!hdr || !sig_match(hdr->signature, sig))
        return 0;
    if (hdr->length < sizeof(struct acpi_sdt_header) ||
        hdr->length > ACPI_MAX_TABLE_LENGTH)
        return 0;
    return acpi_checksum(hdr, hdr->length);
}

static int acpi_table_valid(const struct acpi_sdt_header *hdr, const char *sig)
{
    if (!hdr)
        return 0;

    /* The HEADER must be proven mapped before it is read at all: sig_match()
     * and the length field are themselves dereferences, so checking containment
     * after them would let a corrupt pointer into an unmapped hole or a device
     * register fault first. Header containment, THEN the structural read, THEN
     * full-extent containment before the checksum walks the whole span. */
    if (!acpi_extent_mapped((uint64_t)(uintptr_t)hdr,
                            (uint64_t)sizeof(struct acpi_sdt_header)))
        return 0;

    if (!sig_match(hdr->signature, sig))
        return 0;
    if (hdr->length < sizeof(struct acpi_sdt_header) ||
        hdr->length > ACPI_MAX_TABLE_LENGTH)
        return 0;

    if (!acpi_extent_mapped((uint64_t)(uintptr_t)hdr, (uint64_t)hdr->length))
        return 0;

    return acpi_table_struct_valid(hdr, sig);
}

/* Decode one AML integer data object at data[*i], bounded by `length`.
 * On success advances *i past the term and returns 1; on a malformed or
 * unsupported encoding returns 0 and leaves *i untouched.
 *
 * ACPI 6.5 section 20.2.3 (Data Objects). This exists because the previous
 * decoder understood only BytePrefix and returned every OTHER opcode byte AS
 * the value -- a WordPrefix-encoded sleep type yielded 0x0B rather than the
 * type it encodes.
 */
static int aml_read_integer(const uint8_t *data, uint32_t length,
                            uint32_t *i, uint64_t *out)
{
    uint32_t p = *i;
    uint32_t n;
    uint32_t k;
    uint64_t v = 0;

    if (p >= length)
        return 0;

    switch (data[p]) {
    case 0x00: *out = 0;              *i = p + 1; return 1;  /* ZeroOp  */
    case 0x01: *out = 1;              *i = p + 1; return 1;  /* OneOp   */
    case 0xFF: *out = 0xFFFFFFFFFFFFFFFFull; *i = p + 1; return 1; /* OnesOp */
    case 0x0A: n = 1; break;   /* BytePrefix  */
    case 0x0B: n = 2; break;   /* WordPrefix  */
    case 0x0C: n = 4; break;   /* DWordPrefix */
    case 0x0E: n = 8; break;   /* QWordPrefix */
    default:   return 0;
    }

    p++;
    if (n > length - p)        /* p <= length here, so this cannot wrap */
        return 0;

    for (k = 0; k < n; k++)
        v |= (uint64_t)data[p + k] << (8u * k);

    *out = v;
    *i = p + n;
    return 1;
}

/* Decode an AML PkgLength at data[*i] (ACPI 6.5 section 20.2.4). The top two bits
 * of the lead byte give the count of following bytes; with none, the low SIX
 * bits are the length, otherwise the low FOUR bits are its least-significant
 * nibble. The encoded length INCLUDES the encoding itself, so the package ends
 * at (lead position + length). Advances *i past the encoding and writes the
 * package end offset to *pkg_end; returns 0 if the encoding or the resulting
 * extent runs past `length`.
 *
 * Skipping this -- advancing over the encoding without decoding it -- let the
 * second package element be read from whatever AML happened to follow a SHORT
 * package whose NumElements over-claimed: in bounds for the table, but not the
 * value firmware published. */
static int aml_read_pkglength(const uint8_t *data, uint32_t length,
                              uint32_t *i, uint32_t *pkg_end)
{
    uint32_t p = *i;
    uint32_t lead_pos = p;
    uint32_t n;
    uint32_t v;
    uint32_t k;

    if (p >= length)
        return 0;

    n = (uint32_t)((data[p] >> 6) & 0x03);
    if (n == 0) {
        v = (uint32_t)(data[p] & 0x3F);
    } else {
        v = (uint32_t)(data[p] & 0x0F);
        if (n > length - p - 1u)
            return 0;
        for (k = 0; k < n; k++)
            v |= (uint32_t)data[p + 1u + k] << (4u + 8u * k);
    }

    p += 1u + n;
    if (v < 1u + n || v > length - lead_pos)
        return 0;          /* length shorter than its own encoding, or overruns */

    *pkg_end = lead_pos + v;
    *i = p;
    return 1;
}

/* Parse a \_Sx_ package from the DSDT and extract SLP_TYPa + SLP_TYPb.
 *
 * The object is located by scanning the AML byte stream for the NameString
 * '_' 'S' <digit> '_' followed by a PackageOp. Works on QEMU, Bochs,
 * VirtualBox, and most real firmware.
 *
 * AML encoding of \_Sx_:
 *   NameOp (0x08) + '_Sx_' + PackageOp (0x12) + PkgLength + NumElements
 *   + <integer SLP_TYPa> + <integer SLP_TYPb> + ...
 *
 * ACPI 6.5 section 7.4.2: SLP_TYPa and SLP_TYPb are INDEPENDENT values written
 * to PM1a_CNT and PM1b_CNT; firmware is not required to make them equal, so
 * the PM1b write cannot reuse SLP_TYPa.
 *
 * `dsdt` MUST already have passed acpi_table_valid() -- this function trusts
 * dsdt->length as the mapped extent of the table.
 *
 * Returns 1 with *out_typa set on success; *out_typb is set only when the
 * package actually carries a valid second element. Returns 0 (leaving both
 * INVALID) when the object is absent or does not decode.
 */
static int parse_sleep_type(const struct acpi_sdt_header *dsdt,
                            char state_digit,
                            uint16_t *out_typa, uint16_t *out_typb)
{
    const uint8_t *data = (const uint8_t *)dsdt;
    uint32_t length = dsdt->length;
    uint32_t i;

    *out_typa = ACPI_SLP_TYPE_INVALID;
    *out_typb = ACPI_SLP_TYPE_INVALID;

    if (length < sizeof(struct acpi_sdt_header) + 4u)
        return 0;

    /* Bound written as a subtraction rather than `i + 4 < length`: the old
     * form wraps for a length near UINT32_MAX and turns the loop bound into a
     * constant true. acpi_table_valid() already caps length, so this is belt
     * and braces -- but it is the form that stays correct if the cap moves. */
    for (i = sizeof(struct acpi_sdt_header); i <= length - 4u; i++) {
        uint32_t p;
        uint32_t pkg_end;
        uint8_t num_elements;
        uint64_t v;
        uint16_t ta;
        uint16_t tb;

        if (data[i] != '_' || data[i + 1] != 'S' ||
            data[i + 2] != (uint8_t)state_digit || data[i + 3] != '_')
            continue;

        p = i + 4u;

        /* PackageOp */
        if (p >= length || data[p] != 0x12)
            continue;
        p++;

        /* PkgLength -- DECODED, not merely skipped, so both element reads are
         * bounded by the PACKAGE and not just by the table. */
        if (!aml_read_pkglength(data, length, &p, &pkg_end))
            continue;

        /* NumElements */
        if (p >= pkg_end)
            continue;
        num_elements = data[p];
        p++;

        if (num_elements < 1)
            continue;

        /* Both shapes below mirror AcpiGetSleepTypeData()
         * (src/kernel/acpica/components/hardware/hwxface.c), the reference
         * implementation vendored in this tree. Decode into LOCALS and publish
         * only once every element has validated: publishing SLP_TYPa and then
         * failing on the second element would advertise a state whose PM1b half
         * is silently skipped, which on a dual-PM1 platform programs half a
         * transition. */
        if (num_elements >= 2) {
            uint64_t vb;

            /* ACPICA's `case 2: default:` fails the WHOLE call unless both
             * elements are integers, then takes `(UINT8) Integer.Value` of each.
             * It does NOT reject values above 7 -- the register writer masks to
             * the 3-bit field instead, which pm1_write_sleep() also does. An
             * extra range rejection here would report a state UNSUPPORTED that
             * the reference implementation accepts, which on S5 means a machine
             * that cannot power off through ACPI at all. */
            if (!aml_read_integer(data, pkg_end, &p, &v))
                continue;
            if (!aml_read_integer(data, pkg_end, &p, &vb))
                continue;
            ta = (uint16_t)(v & 0xFFu);
            tb = (uint16_t)(vb & 0xFFu);
        } else {
            /* A ONE-element package is legal and is not a truncated two-element
             * one: the single integer PACKS both types. ACPICA's `case 1:` takes
             * `(UINT8) value` and `(UINT8)(value >> 8)` -- it MASKS to the two
             * low bytes and ignores everything above bit 15, so a value like
             * 0x00010102 is valid there and must be valid here. */
            if (!aml_read_integer(data, pkg_end, &p, &v))
                continue;
            ta = (uint16_t)(v & 0xFFu);
            tb = (uint16_t)((v >> 8) & 0xFFu);
        }

        *out_typa = ta;
        *out_typb = tb;
        return 1;
    }

    return 0;
}

/* S5 (soft-off) discovery. A parse failure is PRESERVED as INVALID so callers
 * can distinguish "firmware never published \_S5" from "\_S5 is type 0" --
 * type 0 is QEMU's real, parsed value, which is why inventing 0 on failure
 * looked correct for so long. It made acpi_sleep_supported(5) claim support
 * unconditionally and handed acpi_poweroff_now() a fabricated sleep type. */
static void parse_s5_from_dsdt(const struct acpi_sdt_header *dsdt)
{
    (void)parse_sleep_type(dsdt, '5', &slp_typa, &slp_typb);
}

/* Test-only entry points into the two firmware-input guards above. The unit
 * tests build synthetic (and deliberately malformed) table images in their own
 * memory and run them through the REAL parser and the REAL validator -- the
 * bounds and AML-decoding behaviour cannot otherwise be exercised, because live
 * firmware supplies exactly one well-formed DSDT. No live state is touched. */
int acpi_parse_sleep_type_test(const void *table, char state_digit,
                               uint16_t *out_typa, uint16_t *out_typb)
{
    return parse_sleep_type((const struct acpi_sdt_header *)table,
                            state_digit, out_typa, out_typb);
}

int acpi_table_valid_test(const void *table, const char *sig)
{
    /* The STRUCTURAL half only. The memory-map containment policy cannot be
     * exercised from a unit test -- a synthetic table necessarily sits in
     * ordinary kernel memory, which acpi_extent_mapped() correctly refuses --
     * so testing the combined function would assert nothing about signature,
     * length or checksum handling. Containment is exercised on every real boot
     * instead: the smoke log carries a diagnostic whenever it refuses. */
    return acpi_table_struct_valid((const struct acpi_sdt_header *)table, sig);
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

    /* Extract PM1a control block port. A legacy System-I/O address must fit in
     * 16 bits: narrowing a malformed value such as 0x10000 to uint16_t yields
     * port 0, which passes every later nonzero check and then does inw/outw on
     * unrelated legacy hardware. Reject rather than narrow. */
    pm1a_cnt_port = acpi_io_port(fadt->pm1a_control_block);

    /* Parse DSDT for \_S5 sleep type. The DSDT is firmware-supplied and its
     * declared length drives every AML scan in this file, so validate it ONCE
     * here (signature + bounded length + checksum) and cache ONLY a table that
     * passed -- acpi_power_init() then inherits the same guarantee. Unlike the
     * RSDT/XSDT tables, the DSDT is reached through the FADT's `dsdt` field and
     * so never went through find_table_*()'s checksum. */
    dsdt_hdr = (const struct acpi_sdt_header *)(uintptr_t)fadt->dsdt;
    if (acpi_table_valid(dsdt_hdr, "DSDT")) {
        s_dsdt_hdr = dsdt_hdr;  /* cache for acpi_power_init() */
        parse_s5_from_dsdt(dsdt_hdr);
    } else if (dsdt_hdr) {
        printk("[ACPI] DSDT failed validation -- sleep states unavailable\n");
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
    /* s_dsdt_hdr is set only after acpi_table_valid() passed in acpi_init(),
     * so its length is already bounded and checksum-verified here. */
    if (!s_dsdt_hdr) {
        klog(LOG_WARN, "acpi", "ACPI power: no valid DSDT -- S1/S3/S4 unavailable");
        return;
    }

    (void)parse_sleep_type(s_dsdt_hdr, '1', &slp_typa_s1, &slp_typb_s1);
    (void)parse_sleep_type(s_dsdt_hdr, '3', &slp_typa_s3, &slp_typb_s3);
    (void)parse_sleep_type(s_dsdt_hdr, '4', &slp_typa_s4, &slp_typb_s4);

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
    /* S5 is reported supported only when \_S5 actually parsed. Returning 1
     * unconditionally made this API disagree with acpi_get_slp_typa(5), which
     * hands back INVALID on firmware that never published the object. */
    case 5: return slp_typa != ACPI_SLP_TYPE_INVALID;
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
static uint16_t s_pm1b_sts_port;   /* PM1b_EVT status register (optional) */
static uint16_t s_pm1b_en_port;    /* PM1b_EVT enable register (optional) */

/* Bits this driver enables and services in the PM1 event registers. */
#define PM1_STS_PWRBTN  (1u << 8)
#define PM1_STS_SLPBTN  (1u << 9)
#define PM1_STS_WAK     (1u << 15)
#define PM1_SERVICED    (PM1_STS_PWRBTN | PM1_STS_SLPBTN | PM1_STS_WAK)

/* Fixed-event counts, published from the SCI ISR and read at thread level.
 * The ISR must not log (see acpi_sci_process), so these ARE the record of what
 * the interrupt saw until a deferred dispatcher consumes them. */
static volatile uint32_t s_pwrbtn_events;
static volatile uint32_t s_slpbtn_events;
static volatile uint32_t s_wake_events;

/* Compute the enable-register port for one PM1 event block. ACPI 6.5 section
 * 4.8.3.1: the block is PM1_EVT_LEN bytes, split into equal status and enable
 * halves, so PM1_EVT_LEN must be at least 4 (two 16-bit halves) for the
 * derived offset to mean anything. Returns 0 when the block is absent or the
 * declared length is unusable. */
static uint16_t pm1_en_port(uint32_t block)
{
    if (!block || fadt_ptr->pm1_event_length < 4)
        return 0;
    /* Validate the RAW block first, then widen before adding: block + half is a
     * uint32_t expression, so 0xFFFFFFFF + 2 wraps to 1 and would validate as a
     * perfectly good low port. Compute in 64 bits and reject anything that
     * leaves the legacy I/O range. */
    {
        uint64_t derived;

        if (!acpi_io_port(block))
            return 0;
        derived = (uint64_t)block + (uint64_t)(fadt_ptr->pm1_event_length / 2);
        if (derived > 0xFFFFu)
            return 0;
        return acpi_io_port((uint32_t)derived);
    }
}

/* Read PM1_CNT as ONE logical register. ACPI 6.5 section 4.8.3 groups PM1a and
 * PM1b: a bit may be implemented in either block, and an unimplemented bit
 * reads zero, so the effective value is the OR of both. The vendored reference
 * does exactly this -- AcpiHwRegisterRead reaches PM1_CONTROL through
 * AcpiHwReadMultiple(&Value, &XPm1aControlBlock, &XPm1bControlBlock)
 * (src/kernel/acpica/components/hardware/hwregs.c). Reading only PM1a would
 * misread SCI_EN as clear on hardware that implements it in PM1b. */
static uint16_t pm1_control_read_merged(void)
{
    uint16_t v = 0;

    uint16_t pm1b = fadt_ptr ? acpi_io_port(fadt_ptr->pm1b_control_block) : 0;

    if (pm1a_cnt_port)
        v |= inw_acpi(pm1a_cnt_port);
    if (pm1b)
        v |= inw_acpi(pm1b);
    return v;
}

/* ACPI PM timer frequency, fixed by the spec at 3.579545 MHz. */
#define ACPI_PMTMR_HZ 3579545u

/* One in-flight mode change at a time (see acpi_pm1_control_owned). */
static volatile uint32_t s_mode_change_inflight;

/* Wait for SCI_EN with a real ~3-second deadline rather than a spin count.
 *
 * The budget is the vendored reference's: AcpiHwSetMode
 * (src/kernel/acpica/components/hardware/hwacpi.c) polls with `Retry = 3000`
 * and a 1 ms stall each pass, and its own comment says real firmware may
 * transition slowly. An uncalibrated spin count expires in whatever time the
 * host happens to take, which on a fast machine can be milliseconds -- and a
 * premature give-up here means acpi_poweroff_now() skips S5 and halts.
 *
 * The ACPI PM timer is the clock because it is a port read: no interrupts, no
 * calibration, and usable on the interrupts-disabled recovery path. Deltas are
 * accumulated under the counter's own width so a wrap (every ~4.7 s at 24 bits)
 * cannot end the wait early. */
static int pm1_wait_for_sci_en(void)
{
    uint16_t tmr_port = acpi_get_pmtimer_port();
    uint32_t mask;
    uint32_t prev;
    uint32_t iters = 0;
    uint64_t elapsed = 0;
    const uint64_t limit = (uint64_t)ACPI_PMTMR_HZ * 3u;

    if (!tmr_port) {
        /* No PM timer to measure with. Spin long rather than short: the cost of
         * over-waiting on a machine that will never answer is a slow boot, and
         * the cost of under-waiting is a machine that cannot power off. */
        uint32_t i;
        for (i = 0; i < 50000000u; i++) {
            if (pm1_control_read_merged() & 1u)
                return 1;
        }
        return 0;
    }

    mask = acpi_pmtimer_is_32bit() ? 0xFFFFFFFFu : 0x00FFFFFFu;
    prev = inl_acpi(tmr_port) & mask;

    /* An INDEPENDENT iteration ceiling, because the deadline is only as good as
     * the clock behind it: a PM timer that reads a constant (stopped, or a port
     * that answers 0xFF..) never advances `elapsed`, and the loop would spin
     * forever waiting for a deadline that cannot arrive. The ceiling is far
     * above the iteration count 3 s of real polling needs, so it never ends a
     * legitimate wait -- it only bounds a broken one. */
    while (elapsed < limit && iters < 100000000u) {
        uint32_t now;

        if (pm1_control_read_merged() & 1u)
            return 1;

        now = inl_acpi(tmr_port) & mask;
        elapsed += (uint64_t)((now - prev) & mask);
        prev = now;
        iters++;
    }

    return 0;
}

/* Is PM1_CNT ours to write -- and if not, TAKE it.
 *
 * SCI_EN (PM1_CNT bit 0) is the ownership bit: while it is clear the PM1
 * registers belong to firmware and SMI. ACPI 6.5 section 4.8.3.2 / 5.2.9 gives
 * OSPM exactly one way to claim them -- write FADT.ACPI_ENABLE to FADT.SMI_CMD
 * and poll SCI_EN until firmware sets it. OSPM never writes SCI_EN directly.
 *
 * This both ESTABLISHES and REPORTS ownership, deliberately, because splitting
 * the two produced a shutdown regression twice over: a cached "handshake
 * failed" flag defaults to permit on every path that never ran the handshake,
 * while a report-only live read REFUSES on every path that runs before it --
 * and `boot_storage.c:587-590` reaches boot_recovery_act() -> acpi_poweroff_now()
 * during Phase 2, long before the handoff at :1154. A machine that boots with
 * SCI_EN clear would then have been denied the S5 write it previously got. With
 * establish-and-report there is no path that can refuse without having tried.
 *
 * Everything here is port I/O with a bounded poll, so it is safe on the
 * interrupts-disabled recovery path acpi_poweroff_now() documents. */
static int acpi_pm1_control_owned(void)
{
    uint16_t smi_port;
    int owned;

    /* No PM1 control block in EITHER position: nothing to own and nothing to
     * write. Not the hardware-reduced case -- that platform has no PM1 fixed
     * registers at all and never reaches the callers of this. */
    if (!pm1a_cnt_port && !(fadt_ptr && acpi_io_port(fadt_ptr->pm1b_control_block)))
        return 0;

    if (pm1_control_read_merged() & 1u)      /* SCI_EN already set */
        return 1;

    smi_port = fadt_ptr ? acpi_io_port(fadt_ptr->smi_commandport) : 0;
    if (!smi_port || !fadt_ptr->acpi_enable)
        return 0;                            /* no way to ask for ownership */

    /* Do not re-issue ACPI_ENABLE while a previous transition may still be in
     * flight -- firmware can take seconds, and a second write during the
     * transition is not a retry, it is a second request. */
    if (__atomic_exchange_n(&s_mode_change_inflight, 1u, __ATOMIC_ACQUIRE)) {
        /* Someone else already asked. JOIN their transition with the same
         * bounded wait rather than sampling SCI_EN once -- firmware may set it
         * moments later, and a single sample would report "firmware owns it" to
         * a caller that is about to skip the PM1 shutdown path over it. */
        return pm1_wait_for_sci_en();
    }

    outb_acpi(smi_port, fadt_ptr->acpi_enable);
    owned = pm1_wait_for_sci_en();
    __atomic_store_n(&s_mode_change_inflight, 0u, __ATOMIC_RELEASE);
    return owned;
}

/* Init-time wrapper: same handshake, but it says what happened. Kept separate
 * only so the boot log carries one line about ACPI mode rather than one per
 * later poweroff attempt. */
static int acpi_enter_acpi_mode(void)
{
    if (acpi_pm1_control_owned()) {
        klog(LOG_INFO, "acpi", "ACPI mode established (SCI_EN set)");
        return 1;
    }

    klog(LOG_WARN, "acpi",
         "SCI_EN clear and the SMI_CMD handshake did not take -- "
         "PM1 registers remain firmware-owned");
    return 0;
}

void acpi_enable_fixed_events(void)
{
    uint16_t en;

    if (!fadt_ptr || !fadt_ptr->pm1a_event_block) {
        klog(LOG_WARN, "acpi", "No PM1a event block -- fixed events unavailable");
        return;
    }

    /* PM1a_STS is at pm1a_event_block, PM1a_EN is at pm1a_event_block + half */
    s_pm1a_sts_port = acpi_io_port(fadt_ptr->pm1a_event_block);
    s_pm1a_en_port  = pm1_en_port(fadt_ptr->pm1a_event_block);
    if (!s_pm1a_en_port) {
        s_pm1a_sts_port = 0;
        klog(LOG_WARN, "acpi", "PM1_EVT_LEN %u unusable -- fixed events disabled",
             (uint64_t)fadt_ptr->pm1_event_length);
        return;
    }

    /* PM1b is OPTIONAL but not decorative: ACPI 6.5 section 4.8.3.1 permits
     * fixed-event bits to be implemented in EITHER block, so hardware that puts
     * the power button in PM1b would leave it disabled and, worse, never
     * acknowledged -- a level-triggered SCI that nothing clears. */
    s_pm1b_sts_port = acpi_io_port(fadt_ptr->pm1b_event_block);
    s_pm1b_en_port  = pm1_en_port(fadt_ptr->pm1b_event_block);
    if (!s_pm1b_en_port)
        s_pm1b_sts_port = 0;

    /* SCI_EN is an OWNERSHIP bit, not a preference: while it is clear the PM1
     * registers belong to SMI, so programming them would write under firmware
     * and leave power/sleep events routed away from the handler we are about to
     * install. Abort instead, and clear the cached ports so acpi_sci_process()
     * (which returns early on a zero status port) and acpi_register_sci() both
     * see an unavailable subsystem rather than a half-configured one. */
    if (!acpi_enter_acpi_mode()) {
        s_pm1a_sts_port = 0;
        s_pm1a_en_port  = 0;
        s_pm1b_sts_port = 0;
        s_pm1b_en_port  = 0;
        klog(LOG_WARN, "acpi",
             "ACPI mode not established -- fixed events unavailable");
        return;
    }

    /* Clear pending status for the bits we service. The previous 0xFFFF wrote
     * a 1 into bits ACPI 6.5 defines as reserved -- harmless today, but it
     * would blind-clear a bit that later gains a meaning, and it contradicts
     * the careful masking the ISR's write-1-to-clear does. */
    outw_acpi(s_pm1a_sts_port, PM1_SERVICED);
    if (s_pm1b_sts_port)
        outw_acpi(s_pm1b_sts_port, PM1_SERVICED);

    /* Enable EXACTLY the events this driver services, and no others. An OR
     * against the current register would preserve whatever firmware left
     * enabled -- TMR_EN, GBL_EN, RTC_EN, PCIEXP_WAKE_EN -- none of which
     * acpi_sci_process() acknowledges. On a level-triggered SCI an enabled but
     * never-acknowledged event holds the line asserted forever, which the
     * shared-IRQ layer eventually answers by quarantining the GSI. Enabling
     * less is the safe direction: an event we do not service is an event we
     * must not ask for. */
    en = PM1_STS_PWRBTN | PM1_STS_SLPBTN;  /* PWRBTN_EN | SLPBTN_EN */
    outw_acpi(s_pm1a_en_port, en);
    if (s_pm1b_en_port)
        outw_acpi(s_pm1b_en_port, en);

    klog(LOG_INFO, "acpi",
         "Fixed events: PWRBTN_EN + SLPBTN_EN on PM1a 0x%x/0x%x PM1b 0x%x/0x%x",
         (uint64_t)s_pm1a_sts_port, (uint64_t)s_pm1a_en_port,
         (uint64_t)s_pm1b_sts_port, (uint64_t)s_pm1b_en_port);
}

/* SCI body -- services the PM1a/PM1b fixed events and reports whether THIS
 * controller raised the interrupt (the SCI line may be shared).
 *
 * HARD-IRQ CONTEXT. This function must NOT log. klog() reaches
 * klog_disk_flush() whenever live disk logging is armed (klog.c:1881), which
 * performs vfs_create/vfs_open/vfs_write (klog_disk.c:1267-1299) -- and
 * klog_disk_enable() runs at boot_storage.c:940, BEFORE acpi_register_sci() at
 * :1151, so the very first SCI can already take that path. Blocking there
 * strands a level-triggered SCI before its EOI and can deadlock against the
 * storage completion interrupt the write is waiting on. So the ISR does the
 * two things an ISR must do -- acknowledge the source and record what it saw --
 * and leaves logging plus policy dispatch to a thread-level consumer.
 * -> XREF: the power-button/lid-close event section of
 * todo/02-kernel-core/TODO-26-power-management.md (item:
 * "acpi_power_button_event()").
 */
static int acpi_sci_process(void)
{
    uint16_t sts = 0;
    uint16_t sts_b = 0;
    uint16_t asserted;
    uint16_t ack;

    if (!s_pm1a_sts_port) return 0;

    /* ACPI 6.5 section 4.8.3.1.1: a fixed-event bit may be implemented in
     * either block, so the effective status is the OR of both. */
    sts = inw_acpi(s_pm1a_sts_port);
    if (s_pm1b_sts_port)
        sts_b = inw_acpi(s_pm1b_sts_port);
    asserted = (uint16_t)((sts | sts_b) & PM1_SERVICED);

    if (!asserted)
        return 0;

    /* Acknowledge every serviced bit in ONE write-1-to-clear per block, before
     * any bookkeeping. Clearing bit by bit left the line asserted for as long
     * as the work between the writes took. Each block is cleared only for the
     * bits IT actually asserted -- writing a 1 to a bit a block never raised is
     * harmless but pointless, and masking keeps the two blocks independent. */
    ack = (uint16_t)(sts & PM1_SERVICED);
    if (ack)
        outw_acpi(s_pm1a_sts_port, ack);
    ack = (uint16_t)(sts_b & PM1_SERVICED);
    if (ack && s_pm1b_sts_port)
        outw_acpi(s_pm1b_sts_port, ack);

    if (asserted & PM1_STS_PWRBTN)
        __atomic_fetch_add(&s_pwrbtn_events, 1u, __ATOMIC_RELAXED);
    if (asserted & PM1_STS_SLPBTN)
        __atomic_fetch_add(&s_slpbtn_events, 1u, __ATOMIC_RELAXED);
    if (asserted & PM1_STS_WAK)
        __atomic_fetch_add(&s_wake_events, 1u, __ATOMIC_RELAXED);

    return 1;
}

/* Thread-level readers of what the SCI ISR recorded. These are the deferred
 * half of the split above: the ISR counts, a thread-level consumer reports and
 * acts. Counts are monotonic and never cleared here. */
uint32_t acpi_power_button_count(void) {
    return __atomic_load_n(&s_pwrbtn_events, __ATOMIC_RELAXED);
}
uint32_t acpi_sleep_button_count(void) {
    return __atomic_load_n(&s_slpbtn_events, __ATOMIC_RELAXED);
}
uint32_t acpi_wake_event_count(void) {
    return __atomic_load_n(&s_wake_events, __ATOMIC_RELAXED);
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
    static uint8_t registered;
    uint8_t sci_vec;

    if (!fadt_ptr) return;

    /* A second call would append a SECOND acpi_sci_shared node to the same GSI
     * chain (irq_request_gsi_ex does not de-duplicate on handler+ctx), running
     * the handler twice per interrupt and double-counting every event. */
    if (registered) {
        klog(LOG_WARN, "acpi", "SCI already registered -- ignoring repeat call");
        return;
    }

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
        registered = 1;
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
    /* Refuse rather than steal the vector. idt_register_handler() only WARNs
     * and then overwrites, and the SCI is typically the shared ISA IRQ 9, so a
     * silent overwrite would unhook whatever already owns that line (or be
     * unhooked by it later). This path deliberately does NOT go through
     * irq_register(): that dispatcher EOIs BEFORE calling the handler, which is
     * wrong for a level-triggered SCI -- the line is still asserted at EOI time
     * and the interrupt re-fires immediately. acpi_sci_handler() EOIs after
     * servicing, which is the correct order here. */
    if (idt_get_handler(sci_vec)) {
        klog(LOG_ERROR, "acpi",
             "vector 0x%x already claimed -- SCI not registered",
             (uint64_t)sci_vec);
        return;
    }

    idt_register_handler(sci_vec, acpi_sci_handler);
    pic_unmask_irq((uint8_t)fadt_ptr->sci_interrupt);
    registered = 1;

    klog(LOG_INFO, "acpi", "SCI handler registered (ISA IRQ %u, vec 0x%x, PIC)",
         (uint64_t)fadt_ptr->sci_interrupt, (uint64_t)sci_vec);
}

/* ---- Generic sleep state entry ------------------------------------------ */

/* Get the SLP_TYPb companion of acpi_get_slp_typa(). Internal: PM1b is a
 * register-level detail, not part of the public S-state query surface. */
static uint16_t acpi_get_slp_typb(uint8_t state)
{
    switch (state) {
    case 1: return slp_typb_s1;
    case 3: return slp_typb_s3;
    case 4: return slp_typb_s4;
    case 5: return slp_typb;
    default: return ACPI_SLP_TYPE_INVALID;
    }
}

/* Write SLP_TYP + SLP_EN into one PM1 control register WITHOUT disturbing the
 * rest of it. ACPI 6.5 section 4.8.3.2: PM1_CNT also carries SCI_EN (bit 0) and
 * BM_RLD (bit 1); the previous code wrote the register wholesale as
 * (SLP_TYP << 10) | SLP_EN, which cleared SCI_EN and dropped the machine out of
 * ACPI mode at the exact moment it was asked to sleep. */
static void pm1_write_sleep(uint16_t port, uint16_t typ)
{
    uint16_t cur = inw_acpi(port);
    uint16_t val = (uint16_t)((cur & (uint16_t)~((7u << 10) | (1u << 13)))
                              | ((uint16_t)(typ & 7u) << 10)
                              | (uint16_t)(1u << 13));
    outw_acpi(port, val);
}

/* One sleep attempt at a time. The wake confirmation compares a GLOBAL event
 * counter against a snapshot, so two concurrent attempts (or an attempt racing
 * an unrelated wake) could let one validate the other's event. Single-flight
 * makes the counter attempt-scoped by construction. */
static volatile uint32_t s_sleep_inflight;

int acpi_enter_sleep_state(uint8_t state)
{
    uint16_t typa = acpi_get_slp_typa(state);
    uint16_t typb = acpi_get_slp_typb(state);
    uint16_t sts;
    uint16_t pm1b_cnt = fadt_ptr ? acpi_io_port(fadt_ptr->pm1b_control_block) : 0;
    uint32_t wake_before;
    uint64_t saved_flags;
    int woke = 0;
    int spins;

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

    /* S3/S4 are DISCOVERED by this section but not enterable by it. Entering
     * either loses processor state (S3) or DRAM (S4), and none of the machinery
     * that makes that survivable exists yet: no APs are stopped, no devices are
     * quiesced, no caches are flushed, no firmware waking vector is installed,
     * and no hibernation image is written. QEMU reports S3 and S4 supported, so
     * without this refusal a caller gets a hung or reset machine rather than an
     * error. The suspend/hibernate pipelines own the lift.
     * -> XREF: the "S3: Suspend to RAM" and "S4: Hibernate to Disk" sections
     * of todo/02-kernel-core/TODO-26-power-management.md. */
    if (state == 3 || state == 4) {
        klog(LOG_WARN, "acpi",
             "S%u entry refused: suspend/resume machinery not implemented",
             (uint64_t)state);
        return -1;
    }

    if (typa == ACPI_SLP_TYPE_INVALID) {
        klog(LOG_DEBUG, "acpi", "S%u not supported by firmware", (uint64_t)state);
        return -1;
    }

    /* PM1a is REQUIRED, deliberately. A PM1b-only platform could reach the
     * control write, but acpi_enable_fixed_events() returns early without a
     * PM1a EVENT block and acpi_sci_process() returns without a PM1a status
     * port, so such a machine could enter S1 and then never acknowledge or
     * count its wake -- a false resume failure with the level-triggered SCI
     * left asserted. Supporting PM1b-only END TO END is a feature, not a
     * refusal to relax here.
     * -> XREF: the "ACPI general-purpose event (GPE) blocks" section of
     * todo/02-kernel-core/TODO-26-power-management.md carries the PM1b-only
     * item alongside the other dual-block work. */
    if (!acpi_ready || !pm1a_cnt_port) {
        klog(LOG_ERROR, "acpi", "ACPI not ready -- cannot enter S%u",
             (uint64_t)state);
        return -1;
    }

    /* Clearing the cached EVENT ports is not enough on its own: PM1_CNT is a
     * separate register with its own ownership bit. */
    if (!acpi_pm1_control_owned()) {
        klog(LOG_ERROR, "acpi",
             "SCI_EN clear -- refusing to write firmware-owned PM1_CNT for S%u",
             (uint64_t)state);
        return -1;
    }

    /* Sleep entry is a BSP operation. An AP calling this would halt itself
     * while the BSP kept running, and its wake confirmation would be reading a
     * counter another CPU's ISR is driving. */
    if (smp_cpu_id() != 0) {
        klog(LOG_ERROR, "acpi", "S%u entry attempted off the BSP (cpu %u)",
             (uint64_t)state, (uint64_t)smp_cpu_id());
        return -1;
    }

    if (__atomic_exchange_n(&s_sleep_inflight, 1u, __ATOMIC_ACQUIRE)) {
        klog(LOG_WARN, "acpi", "S%u entry already in flight", (uint64_t)state);
        return -1;
    }

    klog(LOG_INFO, "acpi", "Entering S%u (SLP_TYPa=%u SLP_TYPb=%u)...",
         (uint64_t)state, (uint64_t)typa,
         (uint64_t)(typb == ACPI_SLP_TYPE_INVALID ? 0xFFFFu : typb));

    /* Step 1: Disable interrupts on this CPU, SAVING the caller's flags. A bare
     * cli/sti pair hands every return path back with IF=1, silently re-enabling
     * interrupts under a caller that had deliberately turned them off. */
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(saved_flags) : : "memory");

    /* Step 2: Clear a stale WAK_STS, and snapshot the ISR's wake counter. The
     * counter is what the confirmation below actually reads: the wake SCI is
     * delivered at the `sti` further down, so acpi_sci_process() runs and
     * write-1-to-clears WAK_STS BEFORE this function regains control. Polling
     * the register directly would therefore report a genuine resume as a
     * failure -- on a single CPU, every time. The ISR publishes the fact
     * atomically precisely so a thread-level waiter need not win that race. */
    if (s_pm1a_sts_port)
        outw_acpi(s_pm1a_sts_port, PM1_STS_WAK);
    if (s_pm1b_sts_port)
        outw_acpi(s_pm1b_sts_port, PM1_STS_WAK);
    wake_before = acpi_wake_event_count();

    /* Step 3: Write SLP_TYP + SLP_EN, preserving the rest of PM1_CNT. PM1b gets
     * its OWN sleep type: ACPI 6.5 section 7.4.2 does not require the two to match,
     * and reusing SLP_TYPa there can select a different hardware state. */
    /* Each block is written only if it EXISTS. pm1_write_sleep() does a
     * read-modify-write, so calling it with a zero port would do inw/outw on
     * I/O port 0 -- unrelated legacy hardware -- on a PM1b-only platform. */
    if (pm1a_cnt_port)
        pm1_write_sleep(pm1a_cnt_port, typa);
    if (pm1b_cnt && typb != ACPI_SLP_TYPE_INVALID)
        pm1_write_sleep(pm1b_cnt, typb);

    /* Step 4: S1 keeps processor context, so the CPU resumes here on a wakeup
     * interrupt. Any interrupt releases hlt, which is why the wake is confirmed
     * through WAK_STS below rather than assumed from reaching this line. */
    __asm__ volatile ("sti; hlt");

    /* Step 5: Confirm the wake. WAK_STS is set by hardware on a genuine
     * sleep-state exit; a timer tick that merely broke the halt does not set it.
     * Prefer the ISR's counter (it saw and consumed the bit); fall back to
     * reading the register for the case where no SCI is routed at all, where
     * nothing cleared it and the bit is still there to be read. */
    for (spins = 0; spins < 1000 && !woke; spins++) {
        if (acpi_wake_event_count() != wake_before) {
            woke = 1;
            break;
        }
        if (s_pm1a_sts_port) {
            sts = inw_acpi(s_pm1a_sts_port);
            if (sts & PM1_STS_WAK) woke = 1;
        }
        if (!woke && s_pm1b_sts_port) {
            sts = inw_acpi(s_pm1b_sts_port);
            if (sts & PM1_STS_WAK) woke = 1;
        }
    }

    /* Restore the caller's interrupt state instead of leaving IF=1 from the
     * `sti` above. Both exits below pass through here. */
    if (!(saved_flags & (1ull << 9)))
        __asm__ volatile ("cli" ::: "memory");

    __atomic_store_n(&s_sleep_inflight, 0u, __ATOMIC_RELEASE);

    if (!woke) {
        klog(LOG_WARN, "acpi",
             "S%u: halt released without a wake event -- sleep not entered",
             (uint64_t)state);
        return -1;
    }

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

    /* Only write PM1 when \_S5 actually parsed. On firmware that never
     * published it, slp_typa is INVALID and writing a fabricated type here --
     * after acpi_storage_quiesce() has already shut storage down -- could
     * request a state that is not soft-off. The platform poweroff ports below
     * are the correct fallback for exactly that case. */
    if (acpi_ready && acpi_pm1_control_owned() &&
        slp_typa != ACPI_SLP_TYPE_INVALID) {
        uint16_t pm1b_cnt = fadt_ptr
                                ? acpi_io_port(fadt_ptr->pm1b_control_block)
                                : 0;

        if (pm1a_cnt_port)
            pm1_write_sleep(pm1a_cnt_port, slp_typa);

        /* PM1b gets its own SLP_TYPb (ACPI 6.5 section 7.4.2), and only when the
         * address VALIDATED -- a raw-nonzero field such as 0x10000 resolves to
         * port 0, and writing that here would hit unrelated legacy hardware
         * after storage has already been quiesced. */
        if (pm1b_cnt && slp_typb != ACPI_SLP_TYPE_INVALID)
            pm1_write_sleep(pm1b_cnt, slp_typb);
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

const struct acpi_fadt *acpi_get_fadt(void)
{
    return fadt_ptr;
}

int acpi_fadt_hw_reduced(const struct acpi_fadt *fadt)
{
    if (!fadt)
        return 0;
    /* flags is at FADT offset 112 (4 bytes) -- reading it needs length >= 116
     * (ACPI 1.0 minimum FADT). A truncated/malformed table cannot be declared
     * hardware-reduced; treat it as legacy so the bounds check is not bypassed
     * by the capability accessors that call this first. */
    if (fadt->header.length < ACPI_FADT_LEN_FLAGS)
        return 0;
    /* FADT flags bit 20: HW_REDUCED_ACPI -- legacy devices absent */
    return (fadt->flags & (1u << 20)) ? 1 : 0;
}

int acpi_hw_reduced(void)
{
    return acpi_fadt_hw_reduced(fadt_ptr);
}

/* ---- FADT IAPC_BOOT_ARCH flags (offset 109, 16-bit) ----
 * Bit 0: LEGACY_DEVICES -- 8042 required
 * Bit 1: 8042 -- i8042 controller present
 * Bit 2: VGA_NOT_PRESENT -- do not probe VGA
 * Bit 3: MSI_NOT_SUPPORTED -- do not enable MSI
 * Bit 4: PCIe_ASPM -- PCIe ASPM must not be disabled
 * Bit 5: CMOS_RTC_NOT_PRESENT -- do not access CMOS RTC */

int acpi_fadt_has_8042(const struct acpi_fadt *fadt)
{
    /* Hardware-reduced ACPI platforms have no legacy fixed hardware (no
     * i8042/CMOS/VGA legacy ports), regardless of the IAPC_BOOT_ARCH bits,
     * which are reserved/ignored under the hardware-reduced model. Override
     * to absent so the PS/2 path is never probed there. */
    if (acpi_fadt_hw_reduced(fadt))
        return 0;
    if (!fadt)
        return 1;  /* assume present if no FADT */
    /* FADT length must reach boot_arch_flags for the bits to be valid */
    if (fadt->header.length < ACPI_FADT_LEN_BOOT_ARCH)
        return 1;  /* too short -- assume present */
    return (fadt->boot_arch_flags & (1u << 1)) ? 1 : 0;
}

int acpi_has_8042(void)
{
    return acpi_fadt_has_8042(fadt_ptr);
}

int acpi_fadt_has_cmos_rtc(const struct acpi_fadt *fadt)
{
    if (acpi_fadt_hw_reduced(fadt))
        return 0;  /* hardware-reduced: no CMOS RTC */
    if (!fadt)
        return 1;
    if (fadt->header.length < ACPI_FADT_LEN_BOOT_ARCH)
        return 1;
    /* Bit 5: CMOS_RTC_NOT_PRESENT -- inverted: 0=present, 1=absent */
    return (fadt->boot_arch_flags & (1u << 5)) ? 0 : 1;
}

int acpi_has_cmos_rtc(void)
{
    return acpi_fadt_has_cmos_rtc(fadt_ptr);
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

int acpi_fadt_msi_supported(const struct acpi_fadt *fadt)
{
    /* No hardware-reduced override here, deliberately: MSI is a PCI/PCIe
     * capability, not legacy fixed hardware, and a hardware-reduced platform
     * is the LAST place to conclude MSI is unavailable. Only the explicit
     * MSI_NOT_SUPPORTED bit turns it off. */
    if (!fadt)
        return 1;
    if (fadt->header.length < ACPI_FADT_LEN_BOOT_ARCH)
        return 1;
    /* Bit 3: MSI_NOT_SUPPORTED -- inverted: 0=supported, 1=not supported */
    return (fadt->boot_arch_flags & (1u << 3)) ? 0 : 1;
}

int acpi_msi_supported(void)
{
    return acpi_fadt_msi_supported(fadt_ptr);
}

int acpi_fadt_has_vga(const struct acpi_fadt *fadt)
{
    if (acpi_fadt_hw_reduced(fadt))
        return 0;  /* hardware-reduced: no legacy VGA */
    if (!fadt)
        return 1;
    if (fadt->header.length < ACPI_FADT_LEN_BOOT_ARCH)
        return 1;
    /* Bit 2: VGA_NOT_PRESENT -- inverted: 0=present, 1=absent */
    return (fadt->boot_arch_flags & (1u << 2)) ? 0 : 1;
}

int acpi_has_vga(void)
{
    return acpi_fadt_has_vga(fadt_ptr);
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
