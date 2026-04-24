/* ============================================================================
 * bootx64.c -- Custom UEFI Boot Application for Impossible OS
 *
 * Entry point: efi_main(EFI_HANDLE, EFI_SYSTEM_TABLE*)
 * Compiled as a PE/COFF binary, placed at \EFI\BOOT\BOOTX64.EFI
 *
 * Responsibilities:
 *   1. Set GOP video mode (1280×720×32bpp)
 *   2. Draw boot splash (logo + spinner)
 *   3. Load kernel ELF from \boot\kernel.exe
 *   4. Get UEFI memory map → convert to boot_info format
 *   5. Find ACPI RSDP from config tables
 *   6. ExitBootServices()
 *   7. Set up identity-mapped page tables (4 GiB)
 *   8. Jump to kernel entry point
 * ============================================================================ */

#include "efi.h"

/* --- Boot info structure (mirrored from include/kernel/boot_info.h) ---
 * Every mirror struct, constant, and _Static_assert lives in the shared
 * header so tools/boot-info-manifest/ can compile the same definitions for
 * its offset-fingerprint dumper. If this include fails, run the clean-build
 * sequence before editing the mirror. */
#include "boot_info_mirror.h"
#include "boot_proto_mirror.h"
#include "elf_bootproto.h"
#include "boot_proto_sha.h"   /* generated; provides KERNEL_ABI_SHA256 */

/* Inline rdtsc for boot timing */
static inline UINT64 boot_rdtsc(void)
{
    UINT32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((UINT64)hi << 32) | lo;
}

/* --- ELF64 header structures --- */
#define ELF_MAGIC 0x464C457F  /* \x7FELF */

typedef struct {
    UINT32 e_magic;
    UINT8  e_class;       /* 2 = 64-bit */
    UINT8  e_data;        /* 1 = little-endian */
    UINT8  e_version;
    UINT8  e_osabi;
    UINT8  e_pad[8];
    UINT16 e_type;
    UINT16 e_machine;     /* 0x3E = x86-64 */
    UINT32 e_version2;
    UINT64 e_entry;
    UINT64 e_phoff;
    UINT64 e_shoff;
    UINT32 e_flags;
    UINT16 e_ehsize;
    UINT16 e_phentsize;
    UINT16 e_phnum;
    UINT16 e_shentsize;
    UINT16 e_shnum;
    UINT16 e_shstrndx;
} Elf64_Ehdr;

#define PT_LOAD 1

typedef struct {
    UINT32 p_type;
    UINT32 p_flags;
    UINT64 p_offset;
    UINT64 p_vaddr;
    UINT64 p_paddr;
    UINT64 p_filesz;
    UINT64 p_memsz;
    UINT64 p_align;
} Elf64_Phdr;

/* ELF section header */
#define SHT_SYMTAB 2
#define SHT_STRTAB 3

typedef struct {
    UINT32 sh_name;
    UINT32 sh_type;
    UINT64 sh_flags;
    UINT64 sh_addr;
    UINT64 sh_offset;
    UINT64 sh_size;
    UINT32 sh_link;      /* index of associated string table section */
    UINT32 sh_info;
    UINT64 sh_addralign;
    UINT64 sh_entsize;
} Elf64_Shdr;

/* ELF symbol table entry */
typedef struct {
    UINT32 st_name;     /* index into string table */
    UINT8  st_info;
    UINT8  st_other;
    UINT16 st_shndx;
    UINT64 st_value;
    UINT64 st_size;
} Elf64_Sym;

/* --- Globals --- */
static EFI_SYSTEM_TABLE    *gST;
static EFI_BOOT_SERVICES   *gBS;
static EFI_HANDLE           gImageHandle;
static EFI_HANDLE g_boot_device_handle; /*: boot device from LoadedImage */

/* Boot info -- placed at a known physical address (64 KiB) */
#define BOOT_INFO_PHYS_ADDR  0x10000
static struct boot_info    *g_boot_info_ptr;

/* Impossible OS vendor GUID: {6F35D3A4-C0E6-4A82-B5D8-7C9D2E4F8A13}
 * Must match IMPOSSIBLE_OS_VENDOR_GUID_INIT in include/kernel/uefi_vars.h. */
static EFI_GUID g_impossible_os_guid = {
    0x6f35d3a4, 0xc0e6, 0x4a82,
    { 0xb5, 0xd8, 0x7c, 0x9d, 0x2e, 0x4f, 0x8a, 0x13 }
};

/* NVRAM variable name for boot error persistence (UCS-2) */
static CHAR16 g_boot_error_var[] = u"BootError";

/* Framebuffer for splash */
static UINT32 *gFramebuffer;
static UINT32  gFbWidth;
static UINT32  gFbHeight;
static UINT32  gFbPitch;       /* in pixels */
static UINT8   gFbPixelFormat; /* 0=RGBX, 1=BGRX, 2=BitMask -- see gop_pixel_format_code() */

/* Requested resolution from boot.conf Resolution=WxH (0 = auto). */
static UINT32  g_conf_res_width  = 0;
static UINT32  g_conf_res_height = 0;

/* --- Forward declarations for POST16 (used by parse_boot_conf, load_kernel) --- */
static inline void post_code(UINT8 code);
static inline void post_code16(UINT16 code);

/* 16-bit POST codes for boot device filesystem scoping.
 * Placed here so parse_boot_conf() can reference them; the remaining
 * bootloader POST16 codes live near efi_main() where they're used. */
#define POST16_BL_BOOT_FS       0xB092
#define POST16_BL_BOOT_FS_OK    0xB093
#define POST16_BL_FALLBACK 0xB094 /* Device fallback chain */
#define POST16_BL_FALLBACK_OK   0xB095
#define POST16_BL_ROLLBACK_REFUSE 0xB09A  /* Anti-rollback: shipped < required */
#define POST16_BL_ROLLBACK_PASS   0xB09B  /* Anti-rollback: shipped >= required */

/* --- Helper: memory ops --- */
static void efi_memset(void *dst, UINT8 val, UINTN size)
{
    UINT8 *d = (UINT8 *)dst;
    UINTN i;
    for (i = 0; i < size; i++)
        d[i] = val;
}

static void efi_memcpy(void *dst, const void *src, UINTN size)
{
    UINT8 *d = (UINT8 *)dst;
    const UINT8 *s = (const UINT8 *)src;
    UINTN i;
    for (i = 0; i < size; i++)
        d[i] = s[i];
}

/* --- Helper: GUID compare --- */
static BOOLEAN guid_equal(const EFI_GUID *a, const EFI_GUID *b)
{
    const UINT8 *pa = (const UINT8 *)a;
    const UINT8 *pb = (const UINT8 *)b;
    UINTN i;
    for (i = 0; i < 16; i++)
        if (pa[i] != pb[i]) return 0;
    return 1;
}

/* --- Boot debug log buffer (captures all serial output for ESP write) --- */
#define BOOT_LOG_SIZE (32 * 1024)
static char *boot_log_buf = 0;
static UINTN boot_log_pos = 0;

static void boot_log_init(void)
{
    EFI_STATUS status;
    status = gBS->AllocatePool(EfiLoaderData, BOOT_LOG_SIZE, (VOID **)&boot_log_buf);
    if (EFI_ERROR(status))
        boot_log_buf = 0;
}

static void boot_log_append(const char *s)
{
    if (!boot_log_buf) return;
    while (*s && boot_log_pos < BOOT_LOG_SIZE - 1)
        boot_log_buf[boot_log_pos++] = *s++;
}

/* --- Helper: early serial output to COM1 (0x3F8) ---
 * Works before ExitBootServices -- provides diagnostics even when
 * UEFI video console doesn't work (e.g. Hyper-V Gen 2).
 * The 16550 UART is emulated by all major hypervisors. */
#define SERIAL_COM1 0x3F8
#define SERIAL_COM2 0x2F8

/* Active serial port (0 = none detected). Set by serial_early_init(). */
static UINT16 s_serial_port;
static UINT8  s_serial_source;   /* 0=none, 1=SPCR, 2=I/O-probe */
static UINT32 s_serial_baud;     /* detected baud rate */

/* SPCR diagnostic info (for logging after serial is up) */
static UINT8  s_spcr_skipped;   /* 1 if SPCR found but unusable (MMIO/non-standard) */
static UINT64 s_spcr_skip_addr; /* the address that caused the skip */

static inline void outb_early(UINT16 port, UINT8 val)
{
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline UINT8 inb_early(UINT16 port)
{
    UINT8 ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* ---- PCI Configuration Space access (via I/O ports 0xCF8/0xCFC) ---- */

static inline UINT32 bl_pci_addr(UINT8 bus, UINT8 dev, UINT8 func, UINT8 off)
{
    return 0x80000000U | ((UINT32)bus << 16) | ((UINT32)dev << 11)
         | ((UINT32)func << 8) | (off & 0xFC);
}

static inline void outl_early(UINT16 port, UINT32 val)
{
    __asm__ volatile ("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline UINT32 inl_early(UINT16 port)
{
    UINT32 ret;
    __asm__ volatile ("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static UINT32 bl_pci_read32(UINT8 bus, UINT8 dev, UINT8 func, UINT8 off)
{
    outl_early(0xCF8, bl_pci_addr(bus, dev, func, off));
    return inl_early(0xCFC);
}

static UINT16 bl_pci_read16(UINT8 bus, UINT8 dev, UINT8 func, UINT8 off)
{
    outl_early(0xCF8, bl_pci_addr(bus, dev, func, off));
    return (UINT16)(inl_early(0xCFC) >> ((off & 2) * 8));
}

static UINT8 bl_pci_read8(UINT8 bus, UINT8 dev, UINT8 func, UINT8 off)
{
    outl_early(0xCF8, bl_pci_addr(bus, dev, func, off));
    return (UINT8)(inl_early(0xCFC) >> ((off & 3) * 8));
}

/* Probe a 16550 UART at the given I/O base address.
 * Write 0xAE to the scratch register (base+7), read back.
 * If it matches, the UART is present. */
static int serial_probe_port(UINT16 base)
{
    outb_early(base + 7, 0xAE);
    return (inb_early(base + 7) == 0xAE) ? 1 : 0;
}

/* Initialize a 16550 UART with a specific baud rate.
 * If baud == 0, preserve the firmware-configured divisor (SPCR baud code 0). */
static void serial_init_port_baud(UINT16 base, UINT32 baud)
{
    outb_early(base + 1, 0x00);  /* Disable interrupts */
    if (baud > 0) {
        UINT16 divisor = (UINT16)(115200 / baud);
        if (divisor == 0) divisor = 1;
        outb_early(base + 3, 0x80);  /* Enable DLAB */
        outb_early(base + 0, (UINT8)(divisor & 0xFF));
        outb_early(base + 1, (UINT8)((divisor >> 8) & 0xFF));
    }
    outb_early(base + 3, 0x03);  /* 8N1 (also clears DLAB) */
    outb_early(base + 2, 0xC7);  /* Enable FIFO */
    outb_early(base + 4, 0x0B);  /* IRQs, RTS/DSR */
}

/* Initialize the UART at s_serial_port (default 38400 baud). */
static void serial_init_port(UINT16 base)
{
    serial_init_port_baud(base, 38400);
}

/* ---- ACPI SPCR Serial Port Auto-Detection (TODO-02 S10) ---- */

/* Minimal ACPI RSDP for locating RSDT/XSDT */
typedef struct {
    UINT8  signature[8];    /* "RSD PTR " */
    UINT8  checksum;
    UINT8  oem_id[6];
    UINT8  revision;        /* 0=ACPI 1.0, 2=ACPI 2.0+ */
    UINT32 rsdt_addr;
    UINT32 length;
    UINT64 xsdt_addr;
    UINT8  ext_checksum;
    UINT8  reserved[3];
} __attribute__((packed)) BL_ACPI_RSDP;

/* Minimal ACPI SDT header for table scanning */
typedef struct {
    UINT8  signature[4];
    UINT32 length;
} __attribute__((packed)) BL_ACPI_SDT_HDR;

/* ACPI SPCR table (Serial Port Console Redirection) */
typedef struct {
    UINT8  signature[4];       /* 0:  "SPCR" */
    UINT32 length;             /* 4:  table length */
    UINT8  revision;           /* 8 */
    UINT8  checksum;           /* 9 */
    UINT8  oem_id[6];          /* 10 */
    UINT8  oem_table_id[8];    /* 16 */
    UINT32 oem_revision;       /* 24 */
    UINT32 creator_id;         /* 28 */
    UINT32 creator_revision;   /* 32 */
    UINT8  interface_type;     /* 36: 0=16550 compatible */
    UINT8  reserved1[3];       /* 37 */
    /* Generic Address Structure (offset 40) */
    UINT8  base_addr_space;    /* 40: 0=memory, 1=I/O */
    UINT8  base_bit_width;     /* 41 */
    UINT8  base_bit_offset;    /* 42 */
    UINT8  base_access_size;   /* 43 */
    UINT64 base_address;       /* 44 */
    UINT8  interrupt_type;     /* 52 */
    UINT8  irq;                /* 53 */
    UINT32 gsiv;               /* 54 */
    UINT8  baud_rate;          /* 58: encoded (3=9600,4=19200,6=57600,7=115200) */
    UINT8  parity;             /* 59 */
    UINT8  stop_bits;          /* 60 */
    UINT8  flow_control;       /* 61 */
    UINT8  terminal_type;      /* 62 */
    UINT8  language;           /* 63 */
} __attribute__((packed)) BL_ACPI_SPCR;

/* Decode SPCR baud_rate field to actual baud rate.
 * ACPI spec: 0=as-is (firmware-configured), 3-7=specific rates.
 * Unknown non-zero codes fall back to 38400 (not 0/preserve). */
static UINT32 spcr_decode_baud(UINT8 code)
{
    switch (code) {
    case 0:  return 0;     /* as-is: firmware preconfigured the UART */
    case 3:  return 9600;
    case 4:  return 19200;
    case 6:  return 57600;
    case 7:  return 115200;
    default: return 38400; /* unknown code -- fall back to safe default */
    }
}

/* Search ACPI tables for SPCR and extract serial port info.
 * Requires gST to be set (called from serial_early_init).
 * Returns 1 if SPCR provided a usable I/O port, 0 otherwise. */
static int serial_spcr_probe(void)
{
    UINTN i;
    EFI_GUID acpi20_guid = EFI_ACPI_20_TABLE_GUID;
    EFI_GUID acpi10_guid = EFI_ACPI_TABLE_GUID;
    BL_ACPI_RSDP *rsdp = (BL_ACPI_RSDP *)0;

    if (!gST || gST->NumberOfTableEntries == 0)
        return 0;

    /* Step 1: Find ACPI RSDP in UEFI config tables */
    for (i = 0; i < gST->NumberOfTableEntries; i++) {
        EFI_CONFIGURATION_TABLE *entry = &gST->ConfigurationTable[i];
        if (guid_equal(&entry->VendorGuid, &acpi20_guid)) {
            rsdp = (BL_ACPI_RSDP *)entry->VendorTable;
            break;
        }
        if (guid_equal(&entry->VendorGuid, &acpi10_guid) && !rsdp)
            rsdp = (BL_ACPI_RSDP *)entry->VendorTable;
    }
    if (!rsdp)
        return 0;

    /* Validate RSDP signature */
    if (rsdp->signature[0] != 'R' || rsdp->signature[1] != 'S' ||
        rsdp->signature[2] != 'D' || rsdp->signature[3] != ' ' ||
        rsdp->signature[4] != 'P' || rsdp->signature[5] != 'T' ||
        rsdp->signature[6] != 'R' || rsdp->signature[7] != ' ')
        return 0;

    /* Step 2: Get RSDT or XSDT address from RSDP */
    UINT64 sdt_addr = 0;
    int use_xsdt = 0;
    if (rsdp->revision >= 2 && rsdp->xsdt_addr) {
        sdt_addr = rsdp->xsdt_addr;
        use_xsdt = 1;
    } else if (rsdp->rsdt_addr) {
        sdt_addr = rsdp->rsdt_addr;
    }
    if (!sdt_addr)
        return 0;

    /* Step 3: Scan SDT entries for SPCR signature */
    BL_ACPI_SDT_HDR *sdt = (BL_ACPI_SDT_HDR *)(UINTN)sdt_addr;
    UINT32 entry_size = use_xsdt ? 8 : 4;
    UINT32 hdr_size = 36;  /* standard ACPI SDT header size */
    if (sdt->length < hdr_size || sdt->length > 0x100000)
        return 0;  /* reject implausible lengths (< header or > 1 MiB) */
    UINT32 num_entries = (sdt->length - hdr_size) / entry_size;
    UINT8 *entries = (UINT8 *)sdt + hdr_size;

    for (i = 0; i < num_entries; i++) {
        UINT64 table_addr;
        if (use_xsdt)
            table_addr = *(UINT64 *)(entries + i * 8);
        else
            table_addr = *(UINT32 *)(entries + i * 4);
        if (!table_addr)
            continue;

        BL_ACPI_SDT_HDR *hdr = (BL_ACPI_SDT_HDR *)(UINTN)table_addr;

        /* Match "SPCR" signature */
        if (hdr->signature[0] != 'S' || hdr->signature[1] != 'P' ||
            hdr->signature[2] != 'C' || hdr->signature[3] != 'R')
            continue;

        BL_ACPI_SPCR *spcr = (BL_ACPI_SPCR *)(UINTN)table_addr;

        /* Minimum table length to read the fields we need */
        if (spcr->length < 60)
            return 0;

        /* Interface type must be 16550-compatible (ACPI spec Table 5-49) */
        if (spcr->interface_type != 0 && spcr->interface_type != 1)
            return 0;  /* not 16550 -- skip (PL011, ARM SBSA, etc.) */

        /* Must be I/O space (address_space == 1), not MMIO */
        if (spcr->base_addr_space != 1) {
            s_spcr_skipped = 1;
            s_spcr_skip_addr = spcr->base_address;
            return 0;
        }

        /* Must be a standard COM port (COM1 or COM2) */
        UINT16 port = (UINT16)spcr->base_address;
        if (port != SERIAL_COM1 && port != SERIAL_COM2) {
            s_spcr_skipped = 1;
            s_spcr_skip_addr = spcr->base_address;
            return 0;
        }

        /* Verify UART actually exists at this address */
        if (!serial_probe_port(port))
            return 0;

        /* SPCR is valid -- use it */
        s_serial_port = port;
        s_serial_baud = spcr_decode_baud(spcr->baud_rate);
        /* baud == 0 means firmware preconfigured the port (SPCR code 0);
         * serial_init_port_baud() preserves the existing divisor when 0 */
        s_serial_source = 1;  /* SPCR */
        return 1;
    }

    return 0;
}

/* Try ACPI SPCR first, then probe COM1/COM2 via I/O.
 * Sets s_serial_port, s_serial_baud, s_serial_source. */
static void serial_early_init(void)
{
    /* Try ACPI SPCR -- preferred detection method */
    if (serial_spcr_probe()) {
        serial_init_port_baud(s_serial_port, s_serial_baud);
        return;
    }

    /* Fallback: scratch-register I/O probe (S4) */
    if (serial_probe_port(SERIAL_COM1)) {
        s_serial_port = SERIAL_COM1;
    } else if (serial_probe_port(SERIAL_COM2)) {
        s_serial_port = SERIAL_COM2;
    } else {
        s_serial_port = 0;
        return;  /* No UART -- serial output will be silent */
    }
    s_serial_source = 2;  /* I/O probe */
    s_serial_baud = 38400;
    serial_init_port(s_serial_port);
}

static void serial_early_putchar(char c)
{
    UINT32 timeout = 100000;
    if (!s_serial_port) return;  /* No UART available */
    while (!(inb_early(s_serial_port + 5) & 0x20) && --timeout)
        ;
    outb_early(s_serial_port, (UINT8)c);
}

static void serial_early_print(const char *s)
{
    boot_log_append(s);
    const char *p = s;
    while (*p) {
        if (*p == '\n')
            serial_early_putchar('\r');
        serial_early_putchar(*p++);
    }
}

static void serial_early_print_uint(UINT32 val)
{
    char buf[12];
    UINT32 pos = 10;
    buf[11] = '\0';
    if (val == 0) { serial_early_putchar('0'); return; }
    while (val > 0) {
        buf[pos--] = (char)('0' + val % 10);
        val /= 10;
    }
    serial_early_print(buf + pos + 1);
}

static void serial_early_print_hex16(UINT16 val)
{
    static const char hex[] = "0123456789ABCDEF";
    serial_early_putchar(hex[(val >> 12) & 0xF]);
    serial_early_putchar(hex[(val >>  8) & 0xF]);
    serial_early_putchar(hex[(val >>  4) & 0xF]);
    serial_early_putchar(hex[ val        & 0xF]);
}

/* 16-hex-digit printer for UINT64 EFI_STATUS and physical addresses in
 * the early-boot diagnostic path. Same truncation-free shape as
 * serial_early_print_hex16 but walks 16 nibbles. Used by the section 5
 * payload loader's failure messages. */
static void serial_early_print_hex64(UINT64 val)
{
    static const char hex[] = "0123456789ABCDEF";
    int i;
    for (i = 60; i >= 0; i -= 4)
        serial_early_putchar(hex[(val >> i) & 0xFull]);
}

/* Forward declaration */
static void efi_print(CHAR16 *str);

/* ---- Boot fatal error screen (S9) ----------------------------------------
 * Replaces all `for (;;) hlt;` loops with a visible error display.
 * Uses UEFI console (ConOut) for text and ConIn for keypress wait.
 * Falls back to HLT if console is unavailable.
 *
 * After ExitBootServices has been attempted, Boot Services (including ConOut)
 * may be in an undefined state. Set g_ebs_in_progress = 1 before the EBS
 * loop to force serial-only output for EBS-path failures. */
static int g_ebs_in_progress;
static int g_wd_armed;  /* 1 if watchdog was successfully armed (S11) */

/* --- NVRAM boot error persistence (S13) ---
 * Write/read a UINT32 error code to UEFI NVRAM so the next boot knows
 * what happened.  Uses RuntimeServices->GetVariable/SetVariable which
 * are available both before and after ExitBootServices.
 * On failure (NVRAM full, read-only, no RuntimeServices), silently
 * continues -- NVRAM persistence is best-effort. */

static void nvram_write_boot_error(UINT32 code)
{
    if (!gST || !gST->RuntimeServices)
        return;
    EFI_RUNTIME_SERVICES *rt = gST->RuntimeServices;
    if (!rt->SetVariable)
        return;
    UINT32 attrs = EFI_VARIABLE_NON_VOLATILE |
                   EFI_VARIABLE_BOOTSERVICE_ACCESS |
                   EFI_VARIABLE_RUNTIME_ACCESS;
    EFI_STATUS s = rt->SetVariable(
        g_boot_error_var, &g_impossible_os_guid,
        attrs, sizeof(code), &code);
    if (EFI_ERROR(s)) {
        serial_early_print("[WARN] NVRAM: write BootError failed\n");
    }
}

static UINT32 nvram_read_boot_error(void)
{
    if (!gST || !gST->RuntimeServices)
        return 0;
    EFI_RUNTIME_SERVICES *rt = gST->RuntimeServices;
    if (!rt->GetVariable)
        return 0;
    UINT32 code = 0;
    UINTN size = sizeof(code);
    UINT32 attrs = 0;
    EFI_STATUS s = rt->GetVariable(
        g_boot_error_var, &g_impossible_os_guid,
        &attrs, &size, &code);
    if (EFI_ERROR(s) || size != sizeof(code))
        return 0;
    return code;
}

/* Reset the watchdog timer if it was successfully armed.
 * Called before long operations to extend the timeout window. */
static void watchdog_reset(void)
{
    if (!g_wd_armed) return;
    EFI_STATUS s = gBS->SetWatchdogTimer(60, 0x424F4F54, 0, (CHAR16 *)0);
    if (EFI_ERROR(s)) {
        serial_early_print("[WARN] Watchdog: reset failed\n");
        g_wd_armed = 0;  /* lost the watchdog -- don't try again */
    }
}

/* ============================================================================
 * Minimal QR Code Version 3 Encoder (S14: Error Screen QR Code)
 *
 * Encodes a lowercase URL into a 29x29 QR code matrix using byte mode
 * and renders it to the GOP framebuffer.  Self-contained, no dependencies.
 *
 * QR Version 3, Error Correction Level L:
 *   - 29x29 modules
 *   - 70 total codewords (data: 55, EC: 15)
 *   - Byte mode: 1 byte per character, supports full ASCII
 *   - Alignment pattern at (22, 22)
 *
 * URL format: https://impossibleos.co/err/XXXX
 * where XXXX is the 4-digit hex error code from S13.
 * Verified module-for-module against segno (spec-compliant QR library).
 * ============================================================================ */

#define QR_SIZE    29   /* Version 3: 29x29 modules */
#define QR_DATA_CW 55   /* ECL-L: 55 data codewords */
#define QR_EC_LEN  15   /* ECL-L: 15 error correction codewords */
#define QR_TOTAL_CW 70  /* 55 data + 15 EC */

/* GF(256) multiply with QR polynomial 0x11D */
static UINT8 gf_mul(UINT8 a, UINT8 b)
{
    UINT16 p = 0;
    int i;
    for (i = 0; i < 8; i++) {
        if (b & 1) p ^= a;
        b >>= 1;
        if (a & 0x80)
            a = (UINT8)((a << 1) ^ 0x1D);
        else
            a <<= 1;
    }
    return (UINT8)p;
}

/* Reed-Solomon error correction for QR Version 3 ECL-L (15 EC codewords).
 * Generator polynomial coefficients verified via independent computation. */
static void qr_reed_solomon(const UINT8 *data, int data_len, UINT8 *ec_out)
{
    static const UINT8 gen[QR_EC_LEN] = {
        0x1D, 0xC4, 0x6F, 0xA3, 0x70, 0x4A, 0x0A, 0x69,
        0x69, 0x8B, 0x84, 0x97, 0x20, 0x86, 0x1A
    };
    int i, j;
    for (i = 0; i < QR_EC_LEN; i++) ec_out[i] = 0;
    for (i = 0; i < data_len; i++) {
        UINT8 lead = data[i] ^ ec_out[0];
        for (j = 0; j < QR_EC_LEN - 1; j++)
            ec_out[j] = ec_out[j + 1] ^ gf_mul(lead, gen[j]);
        ec_out[QR_EC_LEN - 1] = gf_mul(lead, gen[QR_EC_LEN - 1]);
    }
}

/* Encode a URL string into QR data codewords (byte mode).
 * Returns number of data codewords written (max QR_DATA_CW). */
static int qr_encode_data(const char *text, UINT8 *codewords)
{
    int len = 0, cw_pos = 0, i;
    const char *p;
    for (p = text; *p; p++) len++;

    /* Mode indicator: 0100 (byte mode, 4 bits) +
     * Character count: 8 bits for Version 3 byte mode.
     * Combined first byte: 0100_LLLL where LLLL is upper 4 bits of length.
     * Second byte: lower 4 bits of length + first 4 data bits.
     * Simpler: pack mode(4) + count(8) + data into a bit stream. */
    UINT32 bits = 0x4;  /* 0100 = byte mode */
    int nbits = 4;

    /* Character count: 8 bits for Version 1-9 byte mode */
    bits = (bits << 8) | (UINT32)(len & 0xFF);
    nbits += 8;

    /* Data bytes */
    for (i = 0; i < len; i++) {
        bits = (bits << 8) | (UINT8)text[i];
        nbits += 8;
        while (nbits >= 8) {
            nbits -= 8;
            codewords[cw_pos++] = (UINT8)(bits >> nbits);
            bits &= (1U << nbits) - 1;
        }
    }

    /* Terminator: up to 4 zero bits */
    {
        int term = 4;
        int cap = QR_DATA_CW * 8;
        if (nbits + term > cap) term = cap - nbits;
        if (term < 0) term = 0;
        bits <<= term;
        nbits += term;
    }

    /* Pad to byte boundary (add 0 bits to reach next 8-bit boundary).
     * Per ISO 18004 section 7.4.10, always pad to the next codeword
     * boundary -- if already aligned, add a full zero codeword. */
    {
        int pad_bits = 8 - (nbits % 8);
        bits <<= pad_bits;
        nbits += pad_bits;
    }

    /* Flush remaining */
    while (nbits >= 8) {
        nbits -= 8;
        codewords[cw_pos++] = (UINT8)(bits >> nbits);
        bits &= (1U << nbits) - 1;
    }

    /* Pad to QR_DATA_CW codewords */
    {
        UINT8 pad[2] = { 0xEC, 0x11 };
        int pi = 0;
        while (cw_pos < QR_DATA_CW) {
            codewords[cw_pos++] = pad[pi];
            pi ^= 1;
        }
    }
    return cw_pos;
}

/* Check if a module position is a function pattern (finder, separator,
 * timing, alignment, format info). Used for mask application. */
static int qr_is_function(int row, int col)
{
    /* Finder + separator regions: 9x9 in three corners */
    if (row <= 8 && col <= 8) return 1;          /* Top-left */
    if (row <= 8 && col >= QR_SIZE - 8) return 1; /* Top-right */
    if (row >= QR_SIZE - 8 && col <= 8) return 1; /* Bottom-left */
    /* Timing patterns */
    if (row == 6 || col == 6) return 1;
    /* Alignment pattern for Version 3: center at (22, 22), 5x5 */
    if (row >= 20 && row <= 24 && col >= 20 && col <= 24) return 1;
    return 0;
}

/* Place all QR function patterns into the matrix. */
static void qr_place_patterns(UINT8 matrix[QR_SIZE][QR_SIZE],
                               UINT8 used[QR_SIZE][QR_SIZE])
{
    int r, c;
    for (r = 0; r < QR_SIZE; r++)
        for (c = 0; c < QR_SIZE; c++) {
            matrix[r][c] = 0; used[r][c] = 0;
        }

#define FINDER(sr, sc) do { int _r, _c; \
    for (_r = 0; _r < 7; _r++) for (_c = 0; _c < 7; _c++) { \
        int dark = (_r==0||_r==6||_c==0||_c==6||(_r>=2&&_r<=4&&_c>=2&&_c<=4)); \
        matrix[(sr)+_r][(sc)+_c] = (UINT8)dark; used[(sr)+_r][(sc)+_c] = 1; \
    } } while(0)

    FINDER(0, 0);
    FINDER(0, QR_SIZE - 7);
    FINDER(QR_SIZE - 7, 0);

    /* Separators */
    for (c = 0; c < 8; c++) {
        matrix[7][c] = 0; used[7][c] = 1;
        matrix[c][7] = 0; used[c][7] = 1;
        matrix[7][QR_SIZE-8+c] = 0; used[7][QR_SIZE-8+c] = 1;
        matrix[c][QR_SIZE-8] = 0; used[c][QR_SIZE-8] = 1;
        matrix[QR_SIZE-8][c] = 0; used[QR_SIZE-8][c] = 1;
        matrix[QR_SIZE-8+c][7] = 0; used[QR_SIZE-8+c][7] = 1;
    }

    /* Timing */
    for (c = 8; c < QR_SIZE - 8; c++) {
        matrix[6][c] = (UINT8)(c % 2 == 0); used[6][c] = 1;
        matrix[c][6] = (UINT8)(c % 2 == 0); used[c][6] = 1;
    }

    /* Alignment at (22, 22) for Version 3 */
    { int dr, dc;
      for (dr = -2; dr <= 2; dr++)
          for (dc = -2; dc <= 2; dc++) {
              int dark = (dr==-2||dr==2||dc==-2||dc==2||(dr==0&&dc==0));
              matrix[22+dr][22+dc] = (UINT8)dark;
              used[22+dr][22+dc] = 1;
          }
    }

    /* Dark module */
    matrix[QR_SIZE - 8][8] = 1; used[QR_SIZE - 8][8] = 1;

    /* Reserve format info */
    for (c = 0; c < 9; c++) { used[8][c] = 1; used[c][8] = 1; }
    for (c = QR_SIZE - 8; c < QR_SIZE; c++) { used[8][c] = 1; used[c][8] = 1; }
#undef FINDER
}

/* Place data bits in QR zigzag pattern.
 * Matches segno's implementation of ISO 18004 section 7.7.3:
 * upward = (right & 2) == 0, then XOR with (col < 6) to flip
 * direction for columns left of the timing column. */
static void qr_place_data(UINT8 matrix[QR_SIZE][QR_SIZE],
                           const UINT8 used[QR_SIZE][QR_SIZE],
                           const UINT8 *data, int data_len)
{
    int bit_idx = 0, total_bits = data_len * 8;
    int right, row;
    /* Iterate column pairs: 28,26,...,8,6,4,2 then adjust for timing.
     * Python range(28,0,-2) is not affected by modifying the loop var;
     * in C we use a separate counter to replicate this. */
    int col_iter;
    for (col_iter = QR_SIZE - 1; col_iter >= 1; col_iter -= 2) {
        right = col_iter;
        if (right <= 6) right--;  /* skip timing column: 6->5, 4->3, 2->1 */
        for (row = 0; row < QR_SIZE; row++) {
            int z;
            for (z = 0; z < 2; z++) {
                int c = right - z;
                int upward = (right & 2) == 0;
                if (c < 6) upward = !upward;
                int r = upward ? (QR_SIZE - 1 - row) : row;
                if (c < 0 || used[r][c]) continue;
                if (bit_idx < total_bits) {
                    matrix[r][c] = (UINT8)((data[bit_idx/8] >> (7-(bit_idx%8))) & 1);
                    bit_idx++;
                } else {
                    matrix[r][c] = 0;
                }
            }
        }
    }
}

/* Apply mask 0 and write format info.
 * Format placement matches segno's implementation of ISO 18004 section 7.9:
 * vertical col 8 uses LSB-first, horizontal row 8 uses MSB-first. */
static void qr_apply_mask_and_format(UINT8 matrix[QR_SIZE][QR_SIZE])
{
    int r, c, i;
    /* Format info for ECL-L, mask 0 = 0x77C4 */
    UINT32 fi = 0x77C4;

    for (r = 0; r < QR_SIZE; r++)
        for (c = 0; c < QR_SIZE; c++)
            if (!qr_is_function(r, c))
                if ((r + c) % 2 == 0)
                    matrix[r][c] ^= 1;

    /* Place format info per segno/ISO 18004 section 7.9.
     * For each bit position i (0..7):
     *   vbit = bit i (from LSB)
     *   hbit = bit (14-i) (from MSB)
     *   Vertical col 8 top-left: row i (skip timing at row 6)
     *   Horizontal row 8 top-left: col i (skip timing at col 6)
     *   Horizontal row 8 top-right: col (SIZE-1-i) = vbit
     *   Vertical col 8 bottom-left: row (SIZE-1-i) = hbit */
    {
        int voff = 0, hoff = 0;
        for (i = 0; i < 8; i++) {
            UINT8 vbit = (UINT8)((fi >> i) & 1);
            UINT8 hbit = (UINT8)((fi >> (14 - i)) & 1);
            if (i == 6) { voff = 1; hoff = 1; }  /* skip timing row/col 6 */
            /* Top-left: vertical col 8 */
            matrix[i + voff][8] = vbit;
            /* Top-left: horizontal row 8 */
            matrix[8][i + hoff] = hbit;
            /* Top-right: horizontal row 8, from right */
            matrix[8][QR_SIZE - 1 - i] = vbit;
            /* Bottom-left: vertical col 8, from bottom */
            matrix[QR_SIZE - 1 - i][8] = hbit;
        }
    }

    /* Dark module (always dark, placed after format info) */
    matrix[QR_SIZE - 8][8] = 1;
}

/* Render QR matrix to GOP framebuffer. Black = 0x00000000, White = 0x00FFFFFF. */
static void qr_render_to_fb(const UINT8 matrix[QR_SIZE][QR_SIZE],
                             UINT32 start_x, UINT32 start_y, UINT32 mod)
{
    UINT32 qr_row, qr_col, px, py;
    UINT32 quiet = mod * 4;

    if (!gFramebuffer || gFbWidth == 0 || gFbHeight == 0) return;

    /* Quiet zone (white) */
    { UINT32 total = QR_SIZE * mod + quiet * 2, y, x;
      for (y = 0; y < total && (start_y+y) < gFbHeight; y++)
          for (x = 0; x < total && (start_x+x) < gFbWidth; x++)
              gFramebuffer[(start_y+y) * gFbPitch + (start_x+x)] = 0x00FFFFFF;
    }

    /* QR modules */
    for (qr_row = 0; qr_row < QR_SIZE; qr_row++)
        for (qr_col = 0; qr_col < QR_SIZE; qr_col++) {
            UINT32 color = matrix[qr_row][qr_col] ? 0x00000000 : 0x00FFFFFF;
            UINT32 bx = start_x + quiet + qr_col * mod;
            UINT32 by = start_y + quiet + qr_row * mod;
            for (py = 0; py < mod; py++)
                for (px = 0; px < mod; px++) {
                    UINT32 fx = bx + px, fy = by + py;
                    if (fx < gFbWidth && fy < gFbHeight)
                        gFramebuffer[fy * gFbPitch + fx] = color;
                }
        }
}

/* High-level: encode URL and render QR to framebuffer bottom-right. */
static void qr_render_error_url(UINT32 err_code)
{
    char url[48];
    static const char hex_chars[] = "0123456789abcdef";
    static const char prefix[] = "https://impossibleos.co/err/";
    int i;

    for (i = 0; prefix[i]; i++) url[i] = prefix[i];
    url[i++] = hex_chars[(err_code >> 12) & 0xF];
    url[i++] = hex_chars[(err_code >> 8) & 0xF];
    url[i++] = hex_chars[(err_code >> 4) & 0xF];
    url[i++] = hex_chars[err_code & 0xF];
    url[i] = '\0';

    UINT8 data_cw[QR_DATA_CW];
    UINT8 ec_cw[QR_EC_LEN];
    UINT8 all_cw[QR_TOTAL_CW];

    if (qr_encode_data(url, data_cw) == 0) return;
    qr_reed_solomon(data_cw, QR_DATA_CW, ec_cw);

    for (i = 0; i < QR_DATA_CW; i++) all_cw[i] = data_cw[i];
    for (i = 0; i < QR_EC_LEN; i++) all_cw[QR_DATA_CW + i] = ec_cw[i];

    UINT8 matrix[QR_SIZE][QR_SIZE];
    UINT8 used[QR_SIZE][QR_SIZE];
    qr_place_patterns(matrix, used);
    qr_place_data(matrix, used, all_cw, QR_TOTAL_CW);
    qr_apply_mask_and_format(matrix);

    UINT32 mod = 4;
    if (gFbWidth >= 1920) mod = 6;
    if (gFbWidth >= 2560) mod = 8;

    UINT32 quiet_zone = mod * 4;
    UINT32 qr_total = QR_SIZE * mod + quiet_zone * 2;
    UINT32 margin = 12;

    if (gFbWidth >= qr_total + margin && gFbHeight >= qr_total + margin) {
        UINT32 x = gFbWidth - qr_total - margin;
        UINT32 y = gFbHeight - qr_total - margin;
        qr_render_to_fb(matrix, x, y, mod);
    }
}

/* ============================================================================
 * S18: Graphical Error Screen (ChromeOS/Win11-style BSOD)
 *
 * Renders a pixel-accurate blue-screen layout directly to the GOP
 * framebuffer.  Falls back silently when the framebuffer is headless,
 * too small, or uses a BitMask pixel format that we cannot pack.
 * The ConOut text path in boot_fatal() stays unchanged as the fallback
 * when graphical rendering is not possible (pre-EBS only; post-EBS
 * ConOut is disabled by the g_ebs_in_progress guard).
 *
 * Components:
 *   1. fb_pack_rgb(r,g,b)         -- format-aware pixel packing
 *   2. bsod_font[128][8]          -- 8x8 bitmap font (ported from boot_halt.c)
 *   3. bsod_blit_char / string    -- per-glyph blit with clip
 *   4. bsod_blit_string_scaled()  -- pixel-doubled title/headline text
 *   5. bsod_sad_face[16]          -- 16x16 sad face pixel art
 *   6. bsod_blit_sad_face()       -- scale 4x to 64x64
 *   7. bsod_fill_rect()           -- solid color rect
 *   8. bsod_render_graphical()    -- full layout: fill + icon + text + QR
 * ============================================================================ */

/* Pack (R, G, B) into a 32-bit GOP pixel using the active pixel format.
 * Returns 0 on BitMask format (caller should skip colored rendering). */
static UINT32 fb_pack_rgb(UINT8 r, UINT8 g, UINT8 b)
{
    if (gFbPixelFormat == 0) {
        /* RGBX: byte 0 = R, byte 1 = G, byte 2 = B */
        return ((UINT32)r) | ((UINT32)g << 8) | ((UINT32)b << 16);
    }
    if (gFbPixelFormat == 1) {
        /* BGRX: byte 0 = B, byte 1 = G, byte 2 = R */
        return ((UINT32)b) | ((UINT32)g << 8) | ((UINT32)r << 16);
    }
    /* BitMask or unknown: safe fallback to black (0x00000000 is
     * palindromic and renders as black on every format). */
    return 0x00000000;
}

/* ---- Antialiased font atlas (Selawik Semibold) -------------------------
 *
 * Pre-rendered at build time from resources/fonts/selawksb.ttf via
 * Python/Pillow.  Two sizes: 28px (title/error-code) and 16px (body/
 * hints/URL).  Each glyph is stored as an 8-bit alpha bitmap with
 * variable width and metrics (bearing, advance).  The renderer
 * alpha-composites each pixel against the background color for
 * smooth antialiased text -- the same quality as a desktop TTF
 * renderer but without runtime font parsing.
 *
 * Data files:
 *   bsod_font_title.inc  -- 28px, ~28 KB alpha data
 *   bsod_font_body.inc   -- 16px, ~9 KB alpha data
 * ------------------------------------------------------------------- */

struct bsod_aa_glyph {
    UINT8  width;
    UINT8  height;
    INT8   bearing_x;
    INT8   bearing_y;
    UINT8  advance;
    UINT8  _pad;
    UINT16 data_offset;
};

#include "bsod_font_title.inc"
#include "bsod_font_sub.inc"
#include "bsod_font_body.inc"

/* Render one antialiased glyph at (px, py) where py is the TOP of the
 * line (not the baseline).  bearing_y positions the glyph vertically
 * relative to the ascent line.  Blends fg onto bg per pixel alpha. */
static void bsod_aa_char(UINT32 px, UINT32 py,
                          const struct bsod_aa_glyph *g,
                          const UINT8 *data,
                          UINT32 ascent,
                          UINT8 fg_r, UINT8 fg_g, UINT8 fg_b,
                          UINT8 bg_r, UINT8 bg_g, UINT8 bg_b)
{
    UINT32 row, col;
    const UINT8 *src;
    if (g->width == 0 || g->height == 0) return;
    src = data + g->data_offset;
    for (row = 0; row < g->height; row++) {
        UINT32 y = py + (UINT32)((int)ascent + (int)g->bearing_y) + row;
        if (y >= gFbHeight) break;
        for (col = 0; col < g->width; col++) {
            UINT8 a = src[row * g->width + col];
            UINT32 x;
            if (a == 0) continue;
            x = px + (UINT32)((int)g->bearing_x + (int)col);
            if (x >= gFbWidth) continue;
            if (a == 255) {
                gFramebuffer[y * gFbPitch + x] = fb_pack_rgb(fg_r, fg_g, fg_b);
            } else {
                UINT8 inv = (UINT8)(255 - a);
                UINT8 r = (UINT8)(((UINT32)fg_r * a + (UINT32)bg_r * inv + 127) / 255);
                UINT8 g2 = (UINT8)(((UINT32)fg_g * a + (UINT32)bg_g * inv + 127) / 255);
                UINT8 b = (UINT8)(((UINT32)fg_b * a + (UINT32)bg_b * inv + 127) / 255);
                gFramebuffer[y * gFbPitch + x] = fb_pack_rgb(r, g2, b);
            }
        }
    }
}

/* Extra letter spacing added to each glyph advance for readability.
 * Selawik's default metrics are tight for a BSOD context viewed from
 * a distance; +1px gives the text room to breathe. */
#define BSOD_AA_TRACKING  1

/* Render an antialiased string using the given font atlas. */
static void bsod_aa_string(UINT32 px, UINT32 py, const char *str,
                             const struct bsod_aa_glyph *glyphs,
                             const UINT8 *data, UINT32 ascent,
                             UINT8 fg_r, UINT8 fg_g, UINT8 fg_b,
                             UINT8 bg_r, UINT8 bg_g, UINT8 bg_b)
{
    UINT32 cx = px;
    while (*str) {
        unsigned char ch = (unsigned char)*str;
        if (ch < 0x20 || ch > 0x7E) ch = '?';
        const struct bsod_aa_glyph *g = &glyphs[ch - 0x20];
        if (cx + g->advance > gFbWidth) break;
        bsod_aa_char(cx, py, g, data, ascent,
                      fg_r, fg_g, fg_b, bg_r, bg_g, bg_b);
        cx += g->advance + BSOD_AA_TRACKING;
        str++;
    }
}

/* Compute the pixel width of an AA string (includes tracking). */
static UINT32 bsod_aa_string_width(const char *str,
                                     const struct bsod_aa_glyph *glyphs)
{
    UINT32 w = 0;
    UINT32 n = 0;
    while (*str) {
        unsigned char ch = (unsigned char)*str;
        if (ch < 0x20 || ch > 0x7E) ch = '?';
        w += glyphs[ch - 0x20].advance;
        n++;
        str++;
    }
    if (n > 0) w += (n - 1) * BSOD_AA_TRACKING;
    return w;
}

/* Word-wrap an AA string into lines of at most max_px width. */
static void bsod_aa_wrapped(UINT32 px, UINT32 py, const char *text,
                              const struct bsod_aa_glyph *glyphs,
                              const UINT8 *data, UINT32 ascent,
                              UINT32 line_h, UINT32 max_px,
                              UINT32 max_lines,
                              UINT8 fg_r, UINT8 fg_g, UINT8 fg_b,
                              UINT8 bg_r, UINT8 bg_g, UINT8 bg_b)
{
    UINT32 line = 0;
    UINT32 start = 0;
    char buf[128];

    if (!text) return;

    while (text[start] && line < max_lines) {
        UINT32 end = start;
        UINT32 last_space = start;
        UINT32 have_space = 0;
        UINT32 cur_w = 0;

        while (text[end] && cur_w < max_px) {
            unsigned char ch = (unsigned char)text[end];
            if (ch < 0x20 || ch > 0x7E) ch = '?';
            cur_w += glyphs[ch - 0x20].advance + BSOD_AA_TRACKING;
            if (text[end] == ' ') { last_space = end; have_space = 1; }
            end++;
        }
        if (text[end] != 0 && have_space && cur_w >= max_px)
            end = last_space;

        {
            UINT32 n = end - start;
            UINT32 k;
            if (n >= sizeof(buf)) n = sizeof(buf) - 1;
            for (k = 0; k < n; k++) buf[k] = text[start + k];
            buf[n] = '\0';
        }
        bsod_aa_string(px, py + line * line_h, buf,
                         glyphs, data, ascent,
                         fg_r, fg_g, fg_b, bg_r, bg_g, bg_b);
        line++;
        start = end;
        while (text[start] == ' ') start++;
    }
}

/* Legacy 8x8 bitmap font and its blit helpers removed -- superseded by
 * the antialiased Selawik Semibold atlas above.  The kernel's
 * boot_halt.c retains its own copy of the 8x8 font for the halt
 * screen.  BSOD_FONT_W/H kept for the QR caption_reserve calc. */
#define BSOD_FONT_W 8
#define BSOD_FONT_H 8

/* 100x100 antialiased sad face icon from resources/bsod.png.
 * 8-bit alpha per pixel (0 = transparent, 255 = fully opaque).
 * 10,000 bytes in .rodata.  The renderer alpha-blends each pixel
 * with the background color for smooth edges. */
#define BSOD_ICON_W 100
#define BSOD_ICON_H 100

#include "bsod_icon_aa.inc"

/* Render the 100x100 antialiased icon at (px, py).
 * Alpha-composites each pixel: result = bg + (fg - bg) * alpha / 255
 * using the formula: (fg * a + bg * (255 - a) + 127) / 255
 * which avoids signed subtraction and is always correct. */
static void bsod_blit_icon_aa(UINT32 px, UINT32 py,
                                UINT8 fg_r, UINT8 fg_g, UINT8 fg_b,
                                UINT8 bg_r, UINT8 bg_g, UINT8 bg_b)
{
    UINT32 row, col;
    for (row = 0; row < BSOD_ICON_H; row++) {
        UINT32 y = py + row;
        if (y >= gFbHeight) break;
        for (col = 0; col < BSOD_ICON_W; col++) {
            UINT8 a = bsod_icon_aa[row * BSOD_ICON_W + col];
            UINT32 x;
            if (a == 0) continue;
            x = px + col;
            if (x >= gFbWidth) break;
            if (a == 255) {
                gFramebuffer[y * gFbPitch + x] = fb_pack_rgb(fg_r, fg_g, fg_b);
            } else {
                UINT8 inv = (UINT8)(255 - a);
                UINT8 r = (UINT8)(((UINT32)fg_r * a + (UINT32)bg_r * inv + 127) / 255);
                UINT8 g = (UINT8)(((UINT32)fg_g * a + (UINT32)bg_g * inv + 127) / 255);
                UINT8 b = (UINT8)(((UINT32)fg_b * a + (UINT32)bg_b * inv + 127) / 255);
                gFramebuffer[y * gFbPitch + x] = fb_pack_rgb(r, g, b);
            }
        }
    }
}

/* Fill [x0, x0+w) x [y0, y0+h) with `color`. */
static void bsod_fill_rect(UINT32 x0, UINT32 y0, UINT32 w, UINT32 h, UINT32 color)
{
    UINT32 y, x;
    for (y = 0; y < h; y++) {
        UINT32 fy = y0 + y;
        if (fy >= gFbHeight) break;
        for (x = 0; x < w; x++) {
            UINT32 fx = x0 + x;
            if (fx >= gFbWidth) break;
            gFramebuffer[fy * gFbPitch + fx] = color;
        }
    }
}

/* Full graphical BSOD layout.  Caller MUST have verified:
 *   - gFramebuffer != NULL
 *   - gFbWidth >= 800 && gFbHeight >= 600
 *   - gFbPixelFormat != 2 (RGBX or BGRX)
 * Fills the entire screen and renders every overlay.  Does NOT wait
 * for a keypress -- caller handles that. */
static void bsod_render_graphical(UINT32 err_code, const char *title,
                                    const char *detail)
{
    UINT32 blue   = fb_pack_rgb(0x0A, 0x0A, 0x0A);  /* near-black background */
    UINT32 i;
    char err_buf[32];
    const char hex[] = "0123456789ABCDEF";

    /* 1. Fill entire framebuffer with blue. */
    bsod_fill_rect(0, 0, gFbWidth, gFbHeight, blue);

    /* 2. Sad face icon centered horizontally, 100x100 native pixels
     *    from resources/bsod.png with 8-bit alpha antialiasing.
     *    Blends white foreground onto the blue background. */
    bsod_blit_icon_aa(gFbWidth / 2 - BSOD_ICON_W / 2, 30,
                       0xFF, 0xFF, 0xFF,   /* fg: white */
                       0x0A, 0x0A, 0x0A);  /* bg: near-black */

    /* Shorthand macros for the three AA font sizes.  bg = BSOD blue. */
#define AA_TITLE(px, py, str, r, g, b) \
    bsod_aa_string((px), (py), (str), bsod_aa_TITLE, bsod_aa_TITLE_data, \
                    BSOD_AA_TITLE_ASCENT, (r), (g), (b), 0x0A, 0x0A, 0x0A)
#define AA_SUB(px, py, str, r, g, b) \
    bsod_aa_string((px), (py), (str), bsod_aa_SUB, bsod_aa_SUB_data, \
                    BSOD_AA_SUB_ASCENT, (r), (g), (b), 0x0A, 0x0A, 0x0A)
#define AA_BODY(px, py, str, r, g, b) \
    bsod_aa_string((px), (py), (str), bsod_aa_BODY, bsod_aa_BODY_data, \
                    BSOD_AA_BODY_ASCENT, (r), (g), (b), 0x0A, 0x0A, 0x0A)

    /* 3. Title (36px Selawik Semibold), centered horizontally.
     *    Positioned 20px below the icon bottom (icon at y=30, h=100). */
    {
        const char *t = "This wasn't supposed to happen.";
        UINT32 w = bsod_aa_string_width(t, bsod_aa_TITLE);
        UINT32 x = (gFbWidth > w) ? (gFbWidth - w) / 2 : 8;
        AA_TITLE(x, 150, t, 0xFF, 0xFF, 0xFF);
    }

    /* 4. Error code subtitle (22px Selawik Semibold, left-aligned).
     *    Clear gap below 36px title establishes visual hierarchy:
     *    36px title >> 22px subtitle >> 16px body. */
    {
        i = 0;
        err_buf[i++] = 'C'; err_buf[i++] = 'o'; err_buf[i++] = 'd';
        err_buf[i++] = 'e'; err_buf[i++] = ' ';
        err_buf[i++] = '0'; err_buf[i++] = 'x';
        err_buf[i++] = hex[(err_code >> 12) & 0xF];
        err_buf[i++] = hex[(err_code >>  8) & 0xF];
        err_buf[i++] = hex[(err_code >>  4) & 0xF];
        err_buf[i++] = hex[(err_code >>  0) & 0xF];
        err_buf[i++] = ':';
        err_buf[i++] = ' ';
        err_buf[i] = '\0';
        {
            UINT32 w1 = bsod_aa_string_width(err_buf, bsod_aa_SUB);
            AA_SUB(80, 250, err_buf, 0xB2, 0xD8, 0xFF);
            if (title)
                AA_SUB(80 + w1, 250, title, 0xFF, 0xFF, 0xFF);
        }
    }

    /* 5. Detail text wrapped (16px body font). */
    if (detail) {
        bsod_aa_wrapped(80, 310, detail,
                         bsod_aa_BODY, bsod_aa_BODY_data,
                         BSOD_AA_BODY_ASCENT, BSOD_AA_BODY_LINE_H,
                         gFbWidth - 160, 3,
                         0xA0, 0xC0, 0xE0,   /* dim blue-white */
                         0x0A, 0x0A, 0x0A);
    }

    /* 6. Recovery hint lines (16px body font). */
    AA_BODY(80, 390, "Here's what you can do:", 0xFF, 0xFF, 0xFF);
    AA_BODY(100, 414, "- Make sure your boot drive is connected", 0xB2, 0xD8, 0xFF);
    AA_BODY(100, 438, "- Check that \\boot\\kernel.exe is on the drive", 0xB2, 0xD8, 0xFF);
    AA_BODY(100, 462, "- Scan the QR code below for help", 0xB2, 0xD8, 0xFF);
    AA_BODY(100, 486, "- Press any key to restart", 0xB2, 0xD8, 0xFF);

#undef AA_TITLE
#undef AA_SUB
#undef AA_BODY

    /* 7. QR code + URL caption, bottom-right.
     *
     * The existing qr_render_error_url() (S14) places the QR with a
     * fixed 12-pixel bottom margin, which leaves no room for a caption
     * line below.  For the graphical BSOD we inline the QR encoding
     * here and position the QR higher so the URL text fits in the
     * reserved bottom strip.  qr_render_to_fb is called directly. */
    {
        char url[48];
        static const char prefix[] = "impossibleos.co/err/";
        int k;
        UINT8 data_cw[QR_DATA_CW];
        UINT8 ec_cw[QR_EC_LEN];
        UINT8 all_cw[QR_TOTAL_CW];
        UINT8 matrix[QR_SIZE][QR_SIZE];
        UINT8 used[QR_SIZE][QR_SIZE];
        UINT32 mod;
        UINT32 quiet_zone;
        UINT32 qr_total;
        UINT32 side_margin = 12;
        UINT32 caption_reserve;
        UINT32 url_w;
        UINT32 qr_x, qr_y;
        UINT32 url_x, url_y;

        for (k = 0; prefix[k]; k++) url[k] = prefix[k];
        url[k++] = hex[(err_code >> 12) & 0xF];
        url[k++] = hex[(err_code >>  8) & 0xF];
        url[k++] = hex[(err_code >>  4) & 0xF];
        url[k++] = hex[(err_code >>  0) & 0xF];
        url[k] = '\0';

        /* Match the mod-size selection from qr_render_error_url so the
         * QR has the same visual scale across renderers. */
        mod = 4;
        if (gFbWidth >= 1920) mod = 6;
        if (gFbWidth >= 2560) mod = 8;
        quiet_zone = mod * 4;
        qr_total = (UINT32)(QR_SIZE * mod + quiet_zone * 2);

        /* Reserve space below the QR for the URL caption.  The QR
         * is pushed up by exactly caption_reserve pixels so the
         * URL fits between the QR bottom and the screen edge. */
        caption_reserve = BSOD_AA_BODY_LINE_H + 14;

        if (gFbWidth >= qr_total + side_margin &&
            gFbHeight >= qr_total + side_margin + caption_reserve) {

            if (qr_encode_data(url, data_cw) != 0) {
                qr_reed_solomon(data_cw, QR_DATA_CW, ec_cw);
                for (k = 0; k < QR_DATA_CW; k++) all_cw[k] = data_cw[k];
                for (k = 0; k < QR_EC_LEN; k++)
                    all_cw[QR_DATA_CW + k] = ec_cw[k];
                qr_place_patterns(matrix, used);
                qr_place_data(matrix, used, all_cw, QR_TOTAL_CW);
                qr_apply_mask_and_format(matrix);

                /* QR pushed up from bottom edge by side_margin +
                 * caption_reserve so the URL text fits below it. */
                qr_x = gFbWidth  - qr_total - side_margin;
                qr_y = gFbHeight - qr_total - side_margin - caption_reserve;
                qr_render_to_fb(matrix, qr_x, qr_y, mod);

                /* URL caption centered below the QR with a 4px gap. */
                url_w = bsod_aa_string_width(url, bsod_aa_BODY);
                if (qr_total >= url_w) {
                    url_x = qr_x + (qr_total - url_w) / 2;
                } else if (gFbWidth >= url_w + 2 * side_margin) {
                    url_x = gFbWidth - side_margin - url_w;
                } else {
                    url_x = side_margin;
                    if (url_x + url_w > gFbWidth)
                        url_x = gFbWidth;  /* sentinel: skip */
                }
                url_y = qr_y + qr_total + 1;
                if (url_x + url_w <= gFbWidth &&
                    url_y + BSOD_AA_BODY_LINE_H <= gFbHeight) {
                    bsod_aa_string(url_x, url_y, url,
                                    bsod_aa_BODY, bsod_aa_BODY_data,
                                    BSOD_AA_BODY_ASCENT,
                                    0xB2, 0xD8, 0xFF,
                                    0x0A, 0x0A, 0x0A);
                }
            }
        }
    }
}

/* Gate predicate: is the framebuffer usable for the graphical BSOD? */
static int bsod_can_render_graphical(void)
{
    return gFramebuffer != (UINT32 *)0 &&
           gFbWidth  >= 800 &&
           gFbHeight >= 600 &&
           gFbPixelFormat != 2;  /* skip BitMask format */
}

/* Hold the fatal error screen on-screen long enough for a human to
 * read it and scan the QR code before boot_fatal() calls ResetSystem.
 *
 * Pre-EBS path: uses gBS->Stall() for accurate 10-second wall-clock
 * dwell with 50 ms keypress polling for early exit.
 *
 * Post-EBS path: Boot Services are gone, so use a TSC spin loop.
 * 2 x 10^10 TSC ticks gives ~20 seconds on a 1 GHz CPU and ~4 seconds
 * on a 5 GHz CPU.  Without TSC frequency calibration at this early
 * stage we cannot hit exact seconds, but any dwell is vastly better
 * than the previous "counter decrement with no time base" which
 * finished in milliseconds when ReadKeyStroke returned quickly.
 * Codex S18 post-impl review finding, 2026-04-12. */
static void boot_fatal_dwell(void)
{
    const UINT64 dwell_ms = 10000;

    if (!g_ebs_in_progress && gBS && gBS->Stall) {
        /* Pre-EBS path: gBS->Stall for accurate wall clock.  Flush
         * any stale buffered keystrokes from firmware menus or
         * keyboard init before polling (Codex quality finding: a
         * leftover key event would skip dwell immediately). */
        UINT64 elapsed_ms = 0;
        if (gST && gST->ConIn && gST->ConIn->Reset)
            gST->ConIn->Reset(gST->ConIn, 0);
        while (elapsed_ms < dwell_ms) {
            if (gST && gST->ConIn) {
                EFI_INPUT_KEY key;
                EFI_STATUS rs = gST->ConIn->ReadKeyStroke(gST->ConIn, &key);
                if (!EFI_ERROR(rs)) return;  /* keypress -- bail */
            }
            gBS->Stall(50000);  /* 50 ms per poll */
            elapsed_ms += 50;
        }
        return;
    }

    /* Post-EBS: RuntimeServices->GetTime for accurate wall clock.
     * GetTime survives ExitBootServices (it's a runtime service).
     * EFI_TIME layout: Year(2)+Month(1)+Day(1)+Hour(1)+Minute(1)+Second(1)+...
     * Our efi.h declares GetTime as (VOID*, VOID*), so we use a raw
     * byte buffer and read Hour/Minute/Second at offsets 4/5/6. */
    if (gST && gST->RuntimeServices && gST->RuntimeServices->GetTime) {
        UINT8 tbuf0[20], tbuf[20];
        EFI_STATUS gs = gST->RuntimeServices->GetTime(tbuf0, (void *)0);
        if (!EFI_ERROR(gs)) {
            UINT64 s0 = (UINT64)tbuf0[4] * 3600 + tbuf0[5] * 60 + tbuf0[6];
            for (;;) {
                UINT64 sn;
                __asm__ volatile("pause");
                gs = gST->RuntimeServices->GetTime(tbuf, (void *)0);
                if (EFI_ERROR(gs)) break;
                sn = (UINT64)tbuf[4] * 3600 + tbuf[5] * 60 + tbuf[6];
                if (sn < s0) break;  /* midnight rollover -- bail */
                if (sn >= s0 + 10) return;
            }
        }
    }

    /* Last-resort TSC fallback if GetTime failed or runtime services
     * are unavailable.  20 * 10^9 TSC ticks is ~4-20 seconds depending
     * on CPU frequency -- coarse but better than zero dwell. */
    {
        UINT64 start, now;
        UINT32 lo, hi;
        __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
        start = ((UINT64)hi << 32) | lo;
        for (;;) {
            __asm__ volatile("pause");
            __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
            now = ((UINT64)hi << 32) | lo;
            if (now - start >= 20000000000ULL) return;
        }
    }
}

static void boot_fatal(UINT32 err_code, const char *title, const char *detail)
{
    /* 0. Persist error code in NVRAM for next-boot diagnostics (S13) */
    nvram_write_boot_error(err_code);

    /* 1. Log to serial (include hex error code) */
    serial_early_print("[CRIT] BOOT FATAL (0x");
    serial_early_print_hex16((UINT16)err_code);
    serial_early_print("): ");
    serial_early_print(title);
    serial_early_print("\n");
    if (detail) {
        serial_early_print("[CRIT]   ");
        serial_early_print(detail);
        serial_early_print("\n");
    }

    /* 2. Display on UEFI console if available and Boot Services are intact.
     * Skip ConOut if we're inside the EBS retry loop -- Boot Services
     * may be in an undefined state after a failed ExitBootServices. */
    if (gST && gST->ConOut && !g_ebs_in_progress) {
        EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *con = gST->ConOut;

        /* White text on blue background (BSOD style) */
        if (con->SetAttribute)
            con->SetAttribute(con, EFI_WHITE | EFI_BACKGROUND_BLUE);
        if (con->ClearScreen)
            con->ClearScreen(con);

        efi_print(u"\r\n  Impossible OS -- Boot Error\r\n\r\n");

        /* Error code line */
        efi_print(u"  Error code: 0x");
        {
            CHAR16 hex_buf[5];
            CHAR16 hex_chars[] = u"0123456789ABCDEF";
            hex_buf[0] = hex_chars[(err_code >> 12) & 0xF];
            hex_buf[1] = hex_chars[(err_code >> 8) & 0xF];
            hex_buf[2] = hex_chars[(err_code >> 4) & 0xF];
            hex_buf[3] = hex_chars[err_code & 0xF];
            hex_buf[4] = 0;
            efi_print(hex_buf);
        }
        efi_print(u"\r\n");

        /* Title */
        efi_print(u"  ERROR: ");
        {
            CHAR16 wide[128];
            UINTN wi = 0;
            while (title[wi] && wi < 126) {
                wide[wi] = (CHAR16)(UINT8)title[wi];
                wi++;
            }
            wide[wi] = 0;
            efi_print(wide);
        }
        efi_print(u"\r\n");

        /* Detail */
        if (detail) {
            efi_print(u"  ");
            {
                CHAR16 wide[256];
                UINTN wi = 0;
                while (detail[wi] && wi < 254) {
                    wide[wi] = (CHAR16)(UINT8)detail[wi];
                    wi++;
                }
                wide[wi] = 0;
                efi_print(wide);
            }
            efi_print(u"\r\n");
        }

        /* Recovery steps */
        efi_print(u"\r\n  Recovery:\r\n");
        efi_print(u"  1. Check boot media is inserted\r\n");
        efi_print(u"  2. Verify \\boot\\kernel.exe exists\r\n");
        efi_print(u"  3. Scan QR code for recovery help\r\n");
        efi_print(u"  4. Press any key to reboot or power off\r\n\r\n");

        /* S18: Render full graphical BSOD when the framebuffer is
         * usable.  Covers the blue background, sad face icon, title,
         * error code, description, QR, and URL.  Fallback when the
         * framebuffer is headless / too small / BitMask format: the
         * ConOut text above is the only on-screen signal. */
        if (bsod_can_render_graphical()) {
            bsod_render_graphical(err_code, title, detail);
        } else if (gFramebuffer && gFbWidth > 0 && gFbHeight > 0) {
            /* Low-res or BitMask: only the QR still works reliably. */
            qr_render_error_url(err_code);
        }
    } else if (bsod_can_render_graphical()) {
        /* EBS in progress or no ConOut -- ConOut is not safe, but direct
         * framebuffer writes are.  Render the full graphical BSOD; the
         * serial log above still has the critical details. */
        bsod_render_graphical(err_code, title, detail);
    } else if (gFramebuffer && gFbWidth > 0 && gFbHeight > 0) {
        /* Post-EBS with unusable framebuffer format -- only the QR
         * renderer works because it uses palindromic B/W pixels that
         * render correctly on every format. */
        qr_render_error_url(err_code);
    }

    /* S18: unified dwell.  Hold the fatal screen visible for the user
     * before ResetSystem in all branches (pre-EBS, post-EBS, graphical
     * or QR-only, with or without ConIn).  Uses gBS->Stall pre-EBS and
     * a TSC spin post-EBS so the dwell is time-based, not iteration-
     * based.  Replaces a pre-existing counter-decrement loop that
     * could finish in milliseconds when ReadKeyStroke returned
     * EFI_NOT_READY quickly -- Codex post-impl review round 1. */
    boot_fatal_dwell();

    /* 3. If error_screen_test, skip reboot so the screen stays visible. */
    if (g_boot_info_ptr && g_boot_info_ptr->config.error_screen_test) {
        serial_early_print("[BOOT] error_screen_test: halting (screen stays visible)\n");
        for (;;) __asm__ volatile("hlt");
    }

    /* 4. Attempt cold reboot via RuntimeServices (works even after EBS failure
     * since ResetSystem is a runtime service, not a boot service). */
    if (gST && gST->RuntimeServices)
        gST->RuntimeServices->ResetSystem(
            0 /* EfiResetCold */, 0, 0, (VOID *)0);

    /* 5. Fallback: halt forever */
    for (;;) __asm__ volatile("hlt");
}

/* --- Helper: print to UEFI console (for debug, before ExitBootServices) --- */
static void efi_print(CHAR16 *str)
{
    gST->ConOut->OutputString(gST->ConOut, str);
}

/* Helper: print hex number (used for debugging before ExitBootServices) */
__attribute__((unused))
static void efi_print_hex(UINT64 val)
{
    CHAR16 buf[19];
    CHAR16 hex[] = u"0123456789ABCDEF";
    int i;
    buf[0] = u'0'; buf[1] = u'x';
    for (i = 0; i < 16; i++)
        buf[2 + i] = hex[(val >> (60 - i * 4)) & 0xF];
    buf[18] = 0;
    efi_print(buf);
}

/* ============================================================================
 * Step 1: Initialize GOP (Graphics Output Protocol)
 *
 * Resolution selection strategy (gop_negotiate_mode):
 *
 *   1. boot.conf Resolution=WxH -- scan all 32bpp modes for exact match.
 *      If found, SetMode to that mode.
 *
 *   2. Auto (no boot.conf override or Resolution=auto):
 *      Pick the mode with the highest pixel count (width × height).
 *      On real hardware this naturally selects the panel's native resolution.
 *      On QEMU it picks whatever OVMF offers at the configured -device size.
 *
 *   3. Fallback: if SetMode fails, keep the current firmware mode as-is.
 *      Log "[Boot] GOP: using firmware default WxH".
 *
 * HiDPI: negotiated width >= 2560 sets boot_info.hidpi = 1.
 *        The boot splash and desktop use this flag to scale UI by 2×.
 * ============================================================================ */

/* Map GOP pixel format to boot_info encoding: 0=RGBX, 1=BGRX, 2=BitMask */
static UINT8 gop_pixel_format_code(EFI_GRAPHICS_PIXEL_FORMAT fmt)
{
    if (fmt == PixelRedGreenBlueReserved) return 0;
    if (fmt == PixelBlueGreenRedReserved) return 1;
    return 2;
}

/* Score and select the best 32bpp GOP mode, call SetMode, set hidpi flag. */
static void gop_negotiate_mode(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop)
{
    /* Guard: validate Mode and Info pointers before any dereference */
    if (!gop->Mode || !gop->Mode->Info) {
        serial_early_print("[BOOT] GOP: Mode/Info NULL -- attempting SetMode(0)\n");
        gop->SetMode(gop, 0);
        if (!gop->Mode || !gop->Mode->Info) return;
    }

    UINT32  best_idx = gop->Mode->Mode;
    UINT32  best_w   = gop->Mode->Info->HorizontalResolution;
    UINT32  best_h   = gop->Mode->Info->VerticalResolution;
    int     found    = 0;
    UINT32  i;

    for (i = 0; i < gop->Mode->MaxMode; i++) {
        UINTN info_size;
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info;
        if (EFI_ERROR(gop->QueryMode(gop, i, &info_size, &info))) continue;

        /* Only consider 32bpp modes with valid pitch */
        if (info->PixelFormat != PixelBlueGreenRedReserved &&
            info->PixelFormat != PixelRedGreenBlueReserved)
            continue;
        if (info->PixelsPerScanLine < info->HorizontalResolution)
            continue;  /* corrupt pitch */

        UINT32 w = info->HorizontalResolution;
        UINT32 h = info->VerticalResolution;

        if (g_conf_res_width > 0 && g_conf_res_height > 0) {
            /* boot.conf explicit resolution: require exact match */
            if (w == g_conf_res_width && h == g_conf_res_height) {
                best_idx = i; best_w = w; best_h = h;
                found = 1;
                break;  /* exact match found, no need to scan further */
            }
        } else {
            /* Auto: prefer best fit up to 1920x1080 to avoid
             * VirtualBox VMSVGA selecting 7680x4320 (8K) which
             * allocates a 129 MB framebuffer and crashes on exit.
             * For higher resolutions, use explicit Resolution= in boot.conf. */
            if (w > 1920 || h > 1080) continue;
            if (!found || w * h > best_w * best_h) {
                best_idx = i; best_w = w; best_h = h;
                found = 1;
            }
        }
    }

    /* Apply SetMode if the selected mode differs from the current one.
     * If SetMode fails, try mode 0 as fallback (S5 hardening). */
    if (found && best_idx != gop->Mode->Mode) {
        EFI_STATUS s = gop->SetMode(gop, best_idx);
        if (EFI_ERROR(s) || gop->Mode->FrameBufferBase == 0) {
            serial_early_print("[BOOT] GOP: SetMode(");
            serial_early_print_uint(best_idx);
            serial_early_print(") failed, trying mode 0\n");
            s = gop->SetMode(gop, 0);
            if (EFI_ERROR(s) || gop->Mode->FrameBufferBase == 0) {
                /* All modes failed -- keep current firmware mode */
                serial_early_print("[BOOT] GOP: using firmware default ");
                serial_early_print_uint(gop->Mode->Info->HorizontalResolution);
                serial_early_print("x");
                serial_early_print_uint(gop->Mode->Info->VerticalResolution);
                serial_early_print("\n");
            }
            g_boot_info_ptr->hidpi =
                (gop->Mode->Info->HorizontalResolution >= 2560) ? 1 : 0;
            return;
        }
    }

    /* HiDPI flag: set when negotiated width >= 2560 */
    g_boot_info_ptr->hidpi =
        (gop->Mode->Info->HorizontalResolution >= 2560) ? 1 : 0;

    /* Serial log: [BOOT] GOP: {W}x{H} 32bpp (mode {idx} of {max}) */
    serial_early_print("[BOOT] GOP: ");
    serial_early_print_uint(gop->Mode->Info->HorizontalResolution);
    serial_early_print("x");
    serial_early_print_uint(gop->Mode->Info->VerticalResolution);
    serial_early_print(" 32bpp (mode ");
    serial_early_print_uint(gop->Mode->Mode);
    serial_early_print(" of ");
    serial_early_print_uint(gop->Mode->MaxMode);
    serial_early_print(")\n");
}

static EFI_STATUS init_gop(void)
{
    EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
    EFI_STATUS status;
    UINT32 i;

    /* Locate GOP -- headless fallback if no display available (S5) */
    status = gBS->LocateProtocol(&gop_guid, (VOID *)0, (VOID **)&gop);
    if (EFI_ERROR(status)) {
        serial_early_print("[BOOT] GOP: none found, headless boot\n");
        g_boot_info_ptr->fb.addr = 0;
        g_boot_info_ptr->fb.width = 0;
        g_boot_info_ptr->fb.height = 0;
        g_boot_info_ptr->fb_available = 0;
        return EFI_SUCCESS;  /* Continue boot without display */
    }

    serial_early_print("[BOOT] GOP: 1 handle found\n");

    /* ── Enumerate all available modes into boot_info ──────────────────── */
    g_boot_info_ptr->gop_mode_count = 0;
    g_boot_info_ptr->gop_mode_selected = 0;
    {
        UINT32 query_errors = 0;
        for (i = 0; i < gop->Mode->MaxMode && i < BOOT_GOP_MODE_MAX; i++) {
            UINTN info_size;
            EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info;
            if (EFI_ERROR(gop->QueryMode(gop, i, &info_size, &info))) {
                query_errors++;
                /* Abort mode enumeration after 100 consecutive errors --
                 * firmware may be returning garbage for remaining modes */
                if (query_errors >= 100) {
                    serial_early_print("[BOOT] GOP: mode enum aborted after 100 errors\n");
                    break;
                }
                continue;
            }
            query_errors = 0;  /* Reset on success */

        UINT32 idx = g_boot_info_ptr->gop_mode_count;
        g_boot_info_ptr->gop_modes[idx].width  = info->HorizontalResolution;
        g_boot_info_ptr->gop_modes[idx].height = info->VerticalResolution;
        g_boot_info_ptr->gop_modes[idx].pixels_per_scanline =
            info->PixelsPerScanLine;
        g_boot_info_ptr->gop_modes[idx].pixel_format =
            gop_pixel_format_code(info->PixelFormat);
        g_boot_info_ptr->gop_modes[idx].pad[0] = 0;
        g_boot_info_ptr->gop_modes[idx].pad[1] = 0;
        g_boot_info_ptr->gop_modes[idx].pad[2] = 0;
        g_boot_info_ptr->gop_mode_count++;
        }
    }

    /* ── Negotiate best mode via boot.conf override or highest-res auto ── */
    gop_negotiate_mode(gop);

    /* ── Safety: if firmware framebuffer is still unusable, try mode 0 ── */
    if (gop->Mode->FrameBufferBase == 0) {
        serial_early_print("[BOOT] GOP: FrameBufferBase=0 after negotiate -- trying mode 0\n");
        EFI_STATUS mode0_s = gop->SetMode(gop, 0);
        if (EFI_ERROR(mode0_s))
            serial_early_print("[BOOT] GOP: SetMode(0) failed in recovery\n");
    }

    /* If framebuffer is STILL null after all retries, go headless */
    if (gop->Mode->FrameBufferBase == 0) {
        serial_early_print("[BOOT] GOP: no usable framebuffer -- headless mode\n");
        gFramebuffer = (UINT32 *)0;
        gFbWidth = 0;
        gFbHeight = 0;
        gFbPitch = 0;
        gFbPixelFormat = 2;
        g_boot_info_ptr->fb_available = 0;
        g_boot_info_ptr->hidpi = 0;
        return EFI_SUCCESS;
    }

    /* Validate framebuffer size against mode dimensions before touching VRAM */
    gFramebuffer = (UINT32 *)(UINTN)gop->Mode->FrameBufferBase;
    gFbWidth  = gop->Mode->Info->HorizontalResolution;
    gFbHeight = gop->Mode->Info->VerticalResolution;
    gFbPitch  = gop->Mode->Info->PixelsPerScanLine;
    gFbPixelFormat = gop_pixel_format_code(gop->Mode->Info->PixelFormat);

    {
        UINTN required_bytes = (UINTN)gFbHeight * (UINTN)gFbPitch * 4;
        UINTN fb_size = gop->Mode->FrameBufferSize;
        if (gFbPitch < gFbWidth || required_bytes == 0 ||
            (fb_size > 0 && required_bytes > fb_size)) {
            serial_early_print("[BOOT] GOP: framebuffer size mismatch -- headless\n");
            gFramebuffer = (UINT32 *)0;
            gFbWidth = 0; gFbHeight = 0; gFbPitch = 0;
            gFbPixelFormat = 2;
            g_boot_info_ptr->fb_available = 0;
            g_boot_info_ptr->hidpi = 0;
            return EFI_SUCCESS;
        }
        /* Clear VRAM -- bounded by validated byte count */
        UINTN sz = (UINTN)gFbHeight * (UINTN)gFbPitch;
        UINTN j;
        for (j = 0; j < sz; j++)
            gFramebuffer[j] = 0x00000000;
    }

    g_boot_info_ptr->fb.addr   = (UINT64)gop->Mode->FrameBufferBase;
    g_boot_info_ptr->fb.pitch  = gFbPitch * 4;
    g_boot_info_ptr->fb.width  = gFbWidth;
    g_boot_info_ptr->fb.height = gFbHeight;
    g_boot_info_ptr->fb.bpp    = 32;
    g_boot_info_ptr->fb.type   = 1;
    g_boot_info_ptr->fb.pixel_format =
        gop_pixel_format_code(gop->Mode->Info->PixelFormat);
    g_boot_info_ptr->fb.pad0 = 0;
    g_boot_info_ptr->gop_mode_selected = gop->Mode->Mode;
    g_boot_info_ptr->fb_available = 1;

    return EFI_SUCCESS;
}

/* ============================================================================
 * Step 1b: Parse boot.conf -- key=value ini file from EFI partition
 *
 * Format: key=value, # comments, blank lines ignored.
 * Runs BEFORE load_kernel() -- UEFI Boot Services are still available.
 * ============================================================================ */

/* ASCII string compare (no strcmp in freestanding UEFI) */
static BOOLEAN ascii_streq(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

/* ASCII to unsigned int (no atoi in freestanding UEFI) */
static UINT32 ascii_atoi(const char *s)
{
    UINT32 val = 0;
    while (*s >= '0' && *s <= '9') {
        val = val * 10 + (*s - '0');
        s++;
    }
    return val;
}

/* ============================================================================
 * section 5: typed payload staging for module= / initrd= / recovery_image=
 *
 * parse_conf_kv() routes each occurrence of these keys through
 * stage_payload() into a fixed staging array. After boot.conf parsing
 * completes, load_staged_payloads() opens each staged file, allocates
 * EfiLoaderData pages, reads the file, and populates the matching
 * payload_descriptors[] slot in g_boot_info_ptr. Runs while Boot
 * Services are still alive; consumers (kernel-side boot_payload_find)
 * read the descriptors after ExitBootServices.
 *
 * The staging layer exists because a single boot.conf line cannot
 * directly own file I/O (parse_conf_kv is called once per key=value
 * pair and cannot partially fail without losing the rest of the file's
 * configuration). Separating parse from load keeps error handling
 * concentrated and lets the PATHS list survive into the load phase
 * even if the parser encounters later unrelated errors.
 * ============================================================================ */

/* Max length of a single ESP-relative payload path. Covers realistic
 * trees like \\EFI\\ImpossibleOS\\modules\\subdir\\name.kmod with room
 * for a filename that is a few times the FAT short-name limit. If a
 * future layout needs longer paths, grow this (producer_id bytes-in-path
 * are hashed for sanity logging but never truncated). */
#define BOOT_PAYLOAD_PATH_MAX 192

/* Per-payload size cap. Typical module/initrd/recovery images in real
 * deployments are well under this; anything larger almost certainly
 * indicates FAT metadata corruption, a pointer-turned-size bug, or a
 * deliberately-malicious image. Cap before AllocatePages so we fail
 * cleanly with a specific message instead of attempting a wild
 * allocation that would either OOM or wrap the page-count math. Sized
 * to match the comparable boot.conf 1 MiB sanity cap philosophy: big
 * enough for any legitimate payload but much smaller than host RAM. */
#define BOOT_PAYLOAD_FILE_MAX (256ull * 1024ull * 1024ull)  /* 256 MiB */

struct staged_payload {
    UINT32 type;                          /* BOOT_PAYLOAD_MODULE / _INITRD / _RECOVERY_IMAGE */
    char   path[BOOT_PAYLOAD_PATH_MAX];   /* ASCII; converted to CHAR16 on load */
};

/* Reserve BOOT_PAYLOAD_MAX - 1 slots; the -1 leaves headroom for future
 * implicit payloads (TPM event log copy, random seed, USB handover
 * state) that do not come from boot.conf. Slot 31 stays open. */
#define BOOT_PAYLOAD_STAGE_MAX (BOOT_PAYLOAD_MAX - 1)
static struct staged_payload g_staged_payloads[BOOT_PAYLOAD_STAGE_MAX];
static UINTN g_staged_payload_count = 0;
static UINT32 g_staged_payload_overflow = 0;

/* Append one payload to the staging list. Type is already resolved to
 * BOOT_PAYLOAD_*. Path is ASCII (from boot.conf); empty paths are
 * rejected at the parser level before this is called. Overflow beyond
 * BOOT_PAYLOAD_STAGE_MAX is counted and surfaced via
 * payload_overflow in boot_info -- the validator's existing
 * BOOT_PAYLOAD_ERR_OVERFLOW_TRUNCATED check then refuses to boot
 * with an incomplete payload set. */
static void stage_payload(UINT32 type, const char *path)
{
    if (path == (const char *)0 || path[0] == '\0') {
        /* Defense in depth: parse_conf_kv's payload-key branches
         * already boot_fatal on empty value, so this path is
         * unreachable today. If a future caller forgets that
         * contract, fail hard here too rather than silently losing a
         * payload. */
        boot_fatal(BOOT_ERR_CONF_INVALID,
                   "stage_payload: empty path",
                   "Caller passed a NULL or empty path; see parse_conf_kv.");
    }
    if (g_staged_payload_count >= BOOT_PAYLOAD_STAGE_MAX) {
        g_staged_payload_overflow++;
        serial_early_print("[WARN] boot.conf: payload staging full (max=");
        serial_early_print_uint((UINT32)BOOT_PAYLOAD_STAGE_MAX);
        serial_early_print("); dropping '");
        serial_early_print(path);
        serial_early_print("'\n");
        return;
    }
    struct staged_payload *slot = &g_staged_payloads[g_staged_payload_count];
    slot->type = type;
    UINTN i;
    for (i = 0; i < BOOT_PAYLOAD_PATH_MAX - 1 && path[i] != '\0'; i++)
        slot->path[i] = path[i];
    slot->path[i] = '\0';
    /* Path overflow is a producer bug: the intended file cannot be
     * located by this name. Refuse to continue rather than silently
     * truncating to the wrong ESP file. */
    if (path[i] != '\0') {
        serial_early_print("[FATAL] boot.conf: payload path exceeds ");
        serial_early_print_uint((UINT32)(BOOT_PAYLOAD_PATH_MAX - 1));
        serial_early_print(" chars: '");
        serial_early_print(path);
        serial_early_print("'\n");
        boot_fatal(BOOT_ERR_CONF_INVALID,
                   "boot.conf payload path too long",
                   "Shorten the path or raise BOOT_PAYLOAD_PATH_MAX.");
    }
    g_staged_payload_count++;
}

/* Set boot_config defaults (used when boot.conf is missing or on parse error) */
static void boot_config_defaults(struct boot_config *cfg)
{
    efi_memset(cfg, 0, sizeof(*cfg));
    cfg->debug          = 0;    /* off by default */
    cfg->verbose        = 0;    /* splash mode */
    cfg->serial_debug   = 1;    /* always useful */
    cfg->boot_mode      = 0;    /* normal */
    cfg->splash_timeout = 3;    /* 3 seconds */
    cfg->heartbeat      = 1;    /* auto */
    cfg->postcode       = 1;    /* auto */
    cfg->postbars       = 0;    /* off -- normal splash, no VPD */
    cfg->test_suite     = 0xFF; /* all categories */
    cfg->test_quiet     = 0;    /* verbose (show PASS lines) */
    cfg->deferred       = 1;    /* defer non-critical inits by default */
    cfg->utest_isolation = 1;   /* per-test isolation ON (opt-out only via boot.conf) */
    cfg->cmdline[0]     = '\0';
    cfg->config_found   = 0;
}

/* Parse a single key=value pair into boot_config */
static void parse_conf_kv(struct boot_config *cfg,
                          const char *key, const char *val)
{
    if      (ascii_streq(key, "debug"))          cfg->debug          = (UINT8)ascii_atoi(val);
    else if (ascii_streq(key, "verbose"))        cfg->verbose        = (UINT8)ascii_atoi(val);
    else if (ascii_streq(key, "serial_debug"))   cfg->serial_debug   = (UINT8)ascii_atoi(val);
    else if (ascii_streq(key, "splash_timeout")) cfg->splash_timeout = (UINT16)ascii_atoi(val);
    else if (ascii_streq(key, "heartbeat")) {
        if      (ascii_streq(val, "off"))    cfg->heartbeat = 0;
        else if (ascii_streq(val, "auto"))   cfg->heartbeat = 1;
        else if (ascii_streq(val, "always")) cfg->heartbeat = 2;
        else cfg->heartbeat = (UINT8)ascii_atoi(val);
    }
    else if (ascii_streq(key, "postcode")) {
        if      (ascii_streq(val, "off"))    cfg->postcode = 0;
        else if (ascii_streq(val, "auto"))   cfg->postcode = 1;
        else if (ascii_streq(val, "always")) cfg->postcode = 2;
        else cfg->postcode = (UINT8)ascii_atoi(val);
    }
    else if (ascii_streq(key, "postbars")) {
        if      (ascii_streq(val, "off"))  cfg->postbars = 0;
        else if (ascii_streq(val, "on"))   cfg->postbars = 1;
        else if (ascii_streq(val, "diag")) cfg->postbars = 2;
        else cfg->postbars = (UINT8)ascii_atoi(val);
    }
    else if (ascii_streq(key, "test")) {
        cfg->test = (UINT8)ascii_atoi(val);
    }
    else if (ascii_streq(key, "test_suite")) {
        if      (ascii_streq(val, "mm"))       cfg->test_suite = 0;
        else if (ascii_streq(val, "fs"))       cfg->test_suite = 1;
        else if (ascii_streq(val, "sched"))    cfg->test_suite = 2;
        else if (ascii_streq(val, "ob"))       cfg->test_suite = 3;
        else if (ascii_streq(val, "security")) cfg->test_suite = 4;
        else if (ascii_streq(val, "ipc"))      cfg->test_suite = 5;
        else if (ascii_streq(val, "boot"))     cfg->test_suite = 6;
        else if (ascii_streq(val, "abi"))      cfg->test_suite = 7;
        else if (ascii_streq(val, "storage"))  cfg->test_suite = 8;
        else if (ascii_streq(val, "exec"))     cfg->test_suite = 9;
        else if (ascii_streq(val, "x86"))      cfg->test_suite = 10;
        else if (ascii_streq(val, "desktop"))  cfg->test_suite = 11;
        else                                   cfg->test_suite = 0xFF;
    }
    else if (ascii_streq(key, "test_quiet")) {
        cfg->test_quiet = (UINT8)ascii_atoi(val);
    }
    else if (ascii_streq(key, "compositor")) {
        /* Desktop UI test framework, headless compositor section. */
        if      (ascii_streq(val, "headless")) cfg->compositor = 1;
        else if (ascii_streq(val, "normal"))   cfg->compositor = 0;
        else                                   cfg->compositor = (UINT8)ascii_atoi(val);
    }
    else if (ascii_streq(key, "test_monitors")) {
        /* Desktop UI multi-monitor matrix section. Accepts either a
         * plain integer count (`test_monitors=2` -> count=2) or the
         * full geometry list form (`1920x1080@96,1920x1080@144,...`)
         * which is comma-counted. The geometry form is forward-compat
         * for the virtio-gpu multi-output driver that will consume the
         * per-monitor WxH@DPI metadata; today both forms collapse to a
         * count. Counts > 3 clamp to 0 = "use hardware default".
         * Codex [H] review: the prior implementation comma-counted
         * even pure-integer input, so `test_monitors=2` was parsed as
         * count=1 and silently broke the documented contract. */
        UINT8 count = 0;
        int all_digits = (*val != '\0');
        const char *p = val;
        while (*p) {
            if (*p < '0' || *p > '9') { all_digits = 0; break; }
            p++;
        }
        if (all_digits) {
            UINTN v = ascii_atoi(val);
            count = (v > 3) ? 0 : (UINT8)v;
        } else {
            count = (*val != '\0') ? 1 : 0;
            const char *q = val;
            while (*q) {
                if (*q == ',') count++;
                q++;
            }
            if (count > 3) count = 0;
        }
        cfg->test_monitors_count = count;
    }
    else if (ascii_streq(key, "diag_delay")) {
        cfg->diag_delay = (UINT8)ascii_atoi(val);
    }
    else if (ascii_streq(key, "diag_splash")) {
        cfg->diag_splash = (UINT8)ascii_atoi(val);
    }
    else if (ascii_streq(key, "deferred")) {
        cfg->deferred = (UINT8)ascii_atoi(val);
    }
    else if (ascii_streq(key, "async_init")) {
        cfg->async_init = (UINT8)ascii_atoi(val);
    }
    else if (ascii_streq(key, "crash_test")) {
        cfg->crash_test = (UINT8)ascii_atoi(val);
    }
    else if (ascii_streq(key, "error_screen_test")) {
        cfg->error_screen_test = (UINT8)ascii_atoi(val);
    }
    else if (ascii_streq(key, "ob_handle_trace")) {
        cfg->ob_handle_trace = (UINT8)ascii_atoi(val);
    }
    else if (ascii_streq(key, "tap")) {
        cfg->tap = (UINT8)ascii_atoi(val);
    }
    else if (ascii_streq(key, "utest_timeout_ms")) {
        UINTN ms = (UINTN)ascii_atoi(val);
        if (ms > 0xFFFF) ms = 0xFFFF;  /* clamp to uint16_t */
        cfg->utest_timeout_ms = (UINT16)ms;
    }
    else if (ascii_streq(key, "utest_filter")) {
        UINTN i;
        for (i = 0; i < sizeof(cfg->utest_filter) - 1 && val[i]; i++)
            cfg->utest_filter[i] = val[i];
        cfg->utest_filter[i] = '\0';
    }
    else if (ascii_streq(key, "utest_isolation")) {
        cfg->utest_isolation = (UINT8)ascii_atoi(val);
    }
    else if (ascii_streq(key, "xml")) {
        cfg->xml = (UINT8)ascii_atoi(val);
    }
    else if (ascii_streq(key, "json")) {
        cfg->json = (UINT8)ascii_atoi(val);
    }
    else if (ascii_streq(key, "stress_iters")) {
        UINTN n = (UINTN)ascii_atoi(val);
        if (n > 0xFFFF) n = 0xFFFF;
        cfg->stress_iters = (UINT16)n;
    }
    else if (ascii_streq(key, "test_kernel_skip")) {
        cfg->test_kernel_skip = (UINT8)ascii_atoi(val);
    }
    else if (ascii_streq(key, "test_usermode_skip")) {
        cfg->test_usermode_skip = (UINT8)ascii_atoi(val);
    }
    else if (ascii_streq(key, "boot_mode")) {
        if      (ascii_streq(val, "normal"))   cfg->boot_mode = 0;
        else if (ascii_streq(val, "safe"))     cfg->boot_mode = 1;
        else if (ascii_streq(val, "recovery")) cfg->boot_mode = 2;
        else cfg->boot_mode = (UINT8)ascii_atoi(val);
    }
    else if (ascii_streq(key, "cmdline")) {
        UINTN i;
        for (i = 0; i < BOOT_CONF_CMDLINE_MAX - 1 && val[i]; i++)
            cfg->cmdline[i] = val[i];
        cfg->cmdline[i] = '\0';
    }
    else if (ascii_streq(key, "Resolution")) {
        if (ascii_streq(val, "auto") || val[0] == '\0') {
            g_conf_res_width  = 0;
            g_conf_res_height = 0;
        } else {
            /* Parse "WxH" or "WXH" (case-insensitive 'x') */
            g_conf_res_width = ascii_atoi(val);
            const char *xp = val;
            while (*xp && *xp != 'x' && *xp != 'X') xp++;
            g_conf_res_height = (*xp) ? ascii_atoi(xp + 1) : 0;
        }
    }
    else if (ascii_streq(key, "config_version")) {
        cfg->config_version = (UINT8)ascii_atoi(val);
    }
    else if (ascii_streq(key, "kernel")) {
        /* Recognised but ignored -- kernel path is a hardcoded 3-path
           fallback search (\boot\kernel.exe, \kernel.exe,
           \EFI\ImpossibleOS\kernel.exe).  Accept the key so boot.conf
           can document the default without triggering a warning. */
    }
    else if (ascii_streq(key, "module")) {
        /* section 5 typed payload: early kernel module file. Value is
         * an ESP-relative path like '\\EFI\\ImpossibleOS\\mod\\net.kmod'.
         * Multiple module= lines are allowed; each stages one MODULE
         * descriptor. Empty value is a hard error (test-checkpoint
         * contract: "missing or malformed entries fail"). */
        if (val[0] == '\0') {
            boot_fatal(BOOT_ERR_CONF_INVALID,
                       "boot.conf: module= with empty path",
                       "Remove the line or give it a valid ESP-relative path.");
        }
        stage_payload(BOOT_PAYLOAD_MODULE, val);
    }
    else if (ascii_streq(key, "initrd")) {
        /* section 5 typed payload: initrd/initramfs. One or more
         * initrd= lines stage INITRD descriptors in order. Empty
         * value is a hard error. */
        if (val[0] == '\0') {
            boot_fatal(BOOT_ERR_CONF_INVALID,
                       "boot.conf: initrd= with empty path",
                       "Remove the line or give it a valid ESP-relative path.");
        }
        stage_payload(BOOT_PAYLOAD_INITRD, val);
    }
    else if (ascii_streq(key, "recovery_image")) {
        /* section 5 typed payload: recovery env image. Typically one
         * recovery_image= line, but the staging layer accepts multiple
         * (e.g. recovery + installer bundle). Empty value is a hard
         * error. */
        if (val[0] == '\0') {
            boot_fatal(BOOT_ERR_CONF_INVALID,
                       "boot.conf: recovery_image= with empty path",
                       "Remove the line or give it a valid ESP-relative path.");
        }
        stage_payload(BOOT_PAYLOAD_RECOVERY_IMAGE, val);
    }
    else {
        /* Unknown key -- warn but don't fail (forward compatibility) */
        serial_early_print("[WARN] boot.conf: unknown key '");
        serial_early_print(key);
        serial_early_print("' (ignored)\n");
        return;
    }

    /* --- Range validation (S7) --- */
    /* debug/verbose/serial_debug: must be 0 or 1 */
    if (ascii_streq(key, "debug") && cfg->debug > 1) {
        serial_early_print("[WARN] boot.conf: debug out of range, using 1\n");
        cfg->debug = 1;
    }
    if (ascii_streq(key, "verbose") && cfg->verbose > 1) {
        serial_early_print("[WARN] boot.conf: verbose out of range, using 1\n");
        cfg->verbose = 1;
    }
    /* splash_timeout: 0-60 seconds */
    if (ascii_streq(key, "splash_timeout") && cfg->splash_timeout > 60) {
        serial_early_print("[WARN] boot.conf: splash_timeout out of range, using 3\n");
        cfg->splash_timeout = 3;
    }
    /* test: 0 or 1 */
    if (ascii_streq(key, "test") && cfg->test > 1) {
        serial_early_print("[WARN] boot.conf: test out of range, using 1\n");
        cfg->test = 1;
    }
}

/* Read and parse \EFI\ImpossibleOS\boot.conf */
static void parse_boot_conf(void)
{
    EFI_GUID fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
    EFI_FILE_PROTOCOL *root_dir, *conf_file;
    EFI_STATUS status;
    struct boot_config *cfg = &g_boot_info_ptr->config;

    /* Always start with defaults */
    boot_config_defaults(cfg);

    /* Reset section 5 payload staging explicitly. EDK2 DEBUG builds
     * fill unused pool / BSS memory with 0xAF, and in practice these
     * statics come up with that poison pattern instead of zero --
     * enough to make stage_payload's count >= STAGE_MAX check fail
     * early (0xAFAFAFAF >= 31) and leak payload_overflow=0xAFAFAFAF
     * into the validator. Initializing at parse entry guarantees a
     * clean slate on every boot regardless of PE loader behavior. */
    g_staged_payload_count = 0;
    g_staged_payload_overflow = 0;
    efi_memset(g_staged_payloads, 0, sizeof(g_staged_payloads));

    serial_early_print("[BOOT] parse_boot_conf...\n");

    /* Open filesystem from boot device (scoped to boot volume).
     * Fall back to LocateProtocol if device handle is absent or lacks
     * SimpleFS -- same pattern as load_kernel(). */
    post_code16(POST16_BL_BOOT_FS);
    fs = (EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *)0;
    if (g_boot_device_handle) {
        status = gBS->HandleProtocol(g_boot_device_handle,
                                      &fs_guid, (VOID **)&fs);
        if (!EFI_ERROR(status)) {
            serial_early_print("[BOOT] Using boot device filesystem\n");
        } else {
            serial_early_print("[WARN] Boot device has no filesystem, "
                               "using fallback\n");
            status = gBS->LocateProtocol(&fs_guid, (VOID *)0, (VOID **)&fs);
        }
    } else {
        status = gBS->LocateProtocol(&fs_guid, (VOID *)0, (VOID **)&fs);
    }
    if (EFI_ERROR(status)) {
        serial_early_print("[BOOT] boot.conf - no filesystem\n");
        return;
    }
    status = fs->OpenVolume(fs, &root_dir);
    if (EFI_ERROR(status)) {
        serial_early_print("[BOOT] boot.conf - cannot open volume\n");
        return;
    }
    post_code16(POST16_BL_BOOT_FS_OK);

    /* Open boot.conf */
    status = root_dir->Open(
        root_dir, &conf_file,
        u"\\EFI\\ImpossibleOS\\boot.conf",
        EFI_FILE_MODE_READ, 0
    );
    if (EFI_ERROR(status)) {
        serial_early_print("[BOOT] boot.conf not found - using defaults\n");
        root_dir->Close(root_dir);
        return;
    }

    /* Read entire file into a pool-allocated buffer sized to the file
     * (2026-04-21): the previous fixed stack buffer silently truncated
     * boot.conf when in-tree comments grew past the cap, dropping the
     * patch-appended `test=1` line at end-of-file and making every
     * `test=1` boot a no-op without any visible warning. Now we query
     * the file size via GetInfo first, then AllocatePool exactly that
     * many bytes plus a terminator. The only cap is a sanity ceiling
     * that rejects a corrupt/malicious filesystem claiming a
     * multi-GiB boot.conf. See `boot-code-quality` Gate 14.
     *
     * BOOT_CONF_SANITY_CAP: 1 MiB. Any legitimate boot.conf is under
     * 100 KiB; a file past this cap means fs corruption or a test
     * disk shipping garbage, and we'd rather boot_fatal than burn
     * half of bootloader memory on it. */
    #define BOOT_CONF_SANITY_CAP (1024u * 1024u)

    UINT64 file_size = 0;
    {
        EFI_GUID file_info_guid = EFI_FILE_INFO_ID;
        UINTN info_size = 0;
        /* Size-query probe: GetInfo returns EFI_BUFFER_TOO_SMALL and
         * fills info_size with the canonical EFI_FILE_INFO size
         * (which includes the variable-length FileName tail). Callers
         * MUST allocate exactly info_size -- a smaller buffer yields
         * a truncated FileInfo, a larger one wastes pool but still
         * returns the same fields. */
        status = conf_file->GetInfo(conf_file, &file_info_guid,
                                    &info_size, (VOID *)0);
        if (status != EFI_BUFFER_TOO_SMALL || info_size == 0) {
            serial_early_print("[BOOT] boot.conf GetInfo size-probe failed - using defaults\n");
            conf_file->Close(conf_file);
            root_dir->Close(root_dir);
            return;
        }
        VOID *info_buf = (VOID *)0;
        status = gBS->AllocatePool(EfiLoaderData, info_size, &info_buf);
        if (EFI_ERROR(status) || !info_buf) {
            serial_early_print("[BOOT] boot.conf info AllocatePool failed - using defaults\n");
            conf_file->Close(conf_file);
            root_dir->Close(root_dir);
            return;
        }
        status = conf_file->GetInfo(conf_file, &file_info_guid,
                                    &info_size, info_buf);
        if (EFI_ERROR(status)) {
            serial_early_print("[BOOT] boot.conf GetInfo read failed - using defaults\n");
            gBS->FreePool(info_buf);
            conf_file->Close(conf_file);
            root_dir->Close(root_dir);
            return;
        }
        file_size = ((EFI_FILE_INFO *)info_buf)->FileSize;
        gBS->FreePool(info_buf);
    }

    if (file_size == 0) {
        serial_early_print("[BOOT] boot.conf empty - using defaults\n");
        conf_file->Close(conf_file);
        root_dir->Close(root_dir);
        return;
    }
    if (file_size > BOOT_CONF_SANITY_CAP) {
        serial_early_print("[FATAL] boot.conf implausibly large -- FileSize=");
        serial_early_print_uint((UINT32)file_size);
        serial_early_print(" bytes (cap=1 MiB); filesystem likely corrupt\n");
        conf_file->Close(conf_file);
        root_dir->Close(root_dir);
        boot_fatal(BOOT_ERR_CONF_INVALID,
                   "boot.conf exceeds 1 MiB sanity cap",
                   "Filesystem likely corrupt; boot.conf claimed >1 MiB.");
    }

    char *buf = (char *)0;
    status = gBS->AllocatePool(EfiLoaderData, (UINTN)file_size + 1,
                               (VOID **)&buf);
    if (EFI_ERROR(status) || !buf) {
        serial_early_print("[BOOT] boot.conf AllocatePool failed - using defaults\n");
        conf_file->Close(conf_file);
        root_dir->Close(root_dir);
        return;
    }

    UINTN buf_size = (UINTN)file_size;
    status = conf_file->Read(conf_file, &buf_size, buf);
    conf_file->Close(conf_file);
    root_dir->Close(root_dir);

    if (EFI_ERROR(status) || buf_size == 0) {
        serial_early_print("[BOOT] boot.conf read error - using defaults\n");
        gBS->FreePool(buf);
        return;
    }

    /* EFI_FILE_PROTOCOL.Read may legitimately return fewer bytes than
     * FileSize if the underlying FAT chain truncated -- trim the
     * declared length to whatever actually landed so the parser does
     * not walk off a short read. No boot_fatal: this is a soft error,
     * not a cap overflow. */
    buf[buf_size] = '\0';

    serial_early_print("[BOOT] boot.conf loaded (");
    serial_early_print_uint((UINT32)buf_size);
    serial_early_print(" bytes)\n");

    /* Parse line by line */
    char *pos = buf;
    while (*pos) {
        /* Skip leading whitespace */
        while (*pos == ' ' || *pos == '\t') pos++;

        /* Skip blank lines and comments */
        if (*pos == '\n' || *pos == '\r' || *pos == '#' || *pos == '\0') {
            while (*pos && *pos != '\n') pos++;
            if (*pos == '\n') pos++;
            continue;
        }

        /* Extract key */
        char key[64];
        UINTN ki = 0;
        while (*pos && *pos != '=' && *pos != '\n' && *pos != '#' &&
               ki < sizeof(key) - 1) {
            if (*pos != ' ' && *pos != '\t')  /* skip spaces in key */
                key[ki++] = *pos;
            pos++;
        }
        key[ki] = '\0';

        if (*pos != '=') {
            /* No '=' found -- skip line */
            while (*pos && *pos != '\n') pos++;
            if (*pos == '\n') pos++;
            continue;
        }
        pos++;  /* skip '=' */

        /* Skip leading whitespace in value */
        while (*pos == ' ' || *pos == '\t') pos++;

        /* Extract value (until newline, comment, or EOF) */
        char val[256];
        UINTN vi = 0;
        while (*pos && *pos != '\n' && *pos != '\r' && *pos != '#' &&
               vi < sizeof(val) - 1) {
            val[vi++] = *pos++;
        }
        /* Trim trailing whitespace from value */
        while (vi > 0 && (val[vi - 1] == ' ' || val[vi - 1] == '\t'))
            vi--;
        val[vi] = '\0';

        /* Skip to next line */
        while (*pos && *pos != '\n') pos++;
        if (*pos == '\n') pos++;

        /* Pre-gate empty-value detection for section 5 payload keys.
         * The `ki > 0 && vi > 0` gate below exists to preserve today's
         * behavior for NON-payload keys: a stray `cmdline=` later in
         * boot.conf must not clear an earlier `cmdline=foo`, and a
         * trailing `test_suite=` must not reset to 0xFF. But the
         * section 5 test-checkpoint contract requires `module=` /
         * `initrd=` / `recovery_image=` with empty value to FAIL boot
         * with a specific diagnostic, which means we cannot silently
         * drop empty-value lines for those three keys. Detect them
         * BEFORE the gate, boot_fatal on empty value; everything else
         * goes through the original gate. */
        if (ki > 0 && vi == 0) {
            if (ascii_streq(key, "module") ||
                ascii_streq(key, "initrd") ||
                ascii_streq(key, "recovery_image")) {
                serial_early_print("[FATAL] boot.conf: '");
                serial_early_print(key);
                serial_early_print("' with empty value\n");
                boot_fatal(BOOT_ERR_CONF_INVALID,
                           "boot.conf: payload key with empty value",
                           "module=/initrd=/recovery_image= require a non-empty ESP-relative path.");
            }
        }

        /* Parse this key=value (original gate preserved for non-payload
         * keys so existing empty-value semantics do not regress). */
        if (ki > 0 && vi > 0)
            parse_conf_kv(cfg, key, val);
    }

    cfg->config_found = 1;
    gBS->FreePool(buf);
}

/* ============================================================================
 * section 5: Load staged payloads (module / initrd / recovery_image)
 *
 * Runs AFTER parse_boot_conf (so g_staged_payloads is populated) and
 * BEFORE load_kernel (so descriptor population finishes while Boot
 * Services are live). Any failure aborts the boot via boot_fatal --
 * "missing or malformed entries fail with a specific bootloader error"
 * per section 5 test checkpoint.
 * ============================================================================ */

/* Convert an ASCII path to UTF-16 on a caller-supplied stack buffer.
 * Returns 0 on overflow, 1 on success. The buffer must be sized for at
 * least (strlen(src) + 1) CHAR16 units; caller passes the declared
 * element count. */
static int ascii_to_utf16(const char *src, CHAR16 *dst, UINTN dst_max)
{
    UINTN i;
    for (i = 0; i < dst_max - 1 && src[i] != '\0'; i++) {
        dst[i] = (CHAR16)(UINT8)src[i];  /* ASCII subset is identity in UTF-16 */
    }
    dst[i] = (CHAR16)0;
    return src[i] == '\0';
}

/* Locate SimpleFS on the boot device (same pattern as load_kernel's
 * opening block). Returns EFI_SUCCESS + *out_fs on success; on failure
 * callers must not dereference *out_fs. */
static EFI_STATUS locate_boot_fs(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL **out_fs)
{
    EFI_GUID fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_STATUS status = EFI_NOT_FOUND;
    *out_fs = (EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *)0;
    if (g_boot_device_handle) {
        status = gBS->HandleProtocol(g_boot_device_handle, &fs_guid,
                                     (VOID **)out_fs);
        if (EFI_ERROR(status))
            status = gBS->LocateProtocol(&fs_guid, (VOID *)0, (VOID **)out_fs);
    } else {
        status = gBS->LocateProtocol(&fs_guid, (VOID *)0, (VOID **)out_fs);
    }
    return status;
}

static void load_staged_payloads(void)
{
    if (g_staged_payload_count == 0 && g_staged_payload_overflow == 0) {
        /* Common path: no module/initrd/recovery_image in boot.conf.
         * payload_count / payload_overflow stay at their memset-zero
         * default; validator short-circuits on count=0 with BOOT_OK. */
        return;
    }

    serial_early_print("[BOOT] load_staged_payloads: staging=");
    serial_early_print_uint((UINT32)g_staged_payload_count);
    if (g_staged_payload_overflow) {
        serial_early_print(" overflow=");
        serial_early_print_uint(g_staged_payload_overflow);
    }
    serial_early_print("\n");

    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
    EFI_STATUS status = locate_boot_fs(&fs);
    if (EFI_ERROR(status)) {
        boot_fatal(BOOT_ERR_CONF_INVALID,
                   "boot.conf names module/initrd/recovery_image but no filesystem is available",
                   "Check the boot device partition and SimpleFS driver.");
    }

    EFI_FILE_PROTOCOL *root_dir;
    status = fs->OpenVolume(fs, &root_dir);
    if (EFI_ERROR(status)) {
        boot_fatal(BOOT_ERR_CONF_INVALID,
                   "boot.conf payload load: OpenVolume failed",
                   "Boot device filesystem is unavailable or corrupt.");
    }

    UINT64 total_bytes = 0ull;
    UINTN si;
    for (si = 0; si < g_staged_payload_count; si++) {
        struct staged_payload *stg = &g_staged_payloads[si];

        /* +1 for NUL; path came from an already-bounded ASCII buffer so
         * length fits. */
        CHAR16 wpath[BOOT_PAYLOAD_PATH_MAX];
        if (!ascii_to_utf16(stg->path, wpath, BOOT_PAYLOAD_PATH_MAX)) {
            boot_fatal(BOOT_ERR_CONF_INVALID,
                       "boot.conf payload path UTF-16 conversion overflow",
                       "Payload path length exceeds bootloader buffer.");
        }

        EFI_FILE_PROTOCOL *file;
        status = root_dir->Open(root_dir, &file, wpath,
                                EFI_FILE_MODE_READ, 0);
        if (EFI_ERROR(status)) {
            serial_early_print("[FATAL] boot.conf payload open failed: '");
            serial_early_print(stg->path);
            serial_early_print("' status=0x");
            serial_early_print_hex64((UINT64)status);
            serial_early_print("\n");
            root_dir->Close(root_dir);
            boot_fatal(BOOT_ERR_CONF_INVALID,
                       "boot.conf named payload not found or unreadable",
                       "Check the path spelling and that the file is on the boot ESP.");
        }

        /* FILE_INFO size probe (identical pattern to boot.conf loader). */
        EFI_GUID file_info_guid = EFI_FILE_INFO_ID;
        UINTN info_size = 0;
        status = file->GetInfo(file, &file_info_guid, &info_size, (VOID *)0);
        if (status != EFI_BUFFER_TOO_SMALL || info_size == 0) {
            file->Close(file);
            root_dir->Close(root_dir);
            boot_fatal(BOOT_ERR_CONF_INVALID,
                       "boot.conf payload GetInfo size-probe failed",
                       "Cannot determine payload file size.");
        }
        VOID *info_buf = (VOID *)0;
        status = gBS->AllocatePool(EfiLoaderData, info_size, &info_buf);
        if (EFI_ERROR(status) || !info_buf) {
            file->Close(file);
            root_dir->Close(root_dir);
            boot_fatal(BOOT_ERR_CONF_INVALID,
                       "boot.conf payload AllocatePool(info) failed",
                       "Out of EfiLoaderData memory for FILE_INFO.");
        }
        status = file->GetInfo(file, &file_info_guid, &info_size, info_buf);
        if (EFI_ERROR(status)) {
            gBS->FreePool(info_buf);
            file->Close(file);
            root_dir->Close(root_dir);
            boot_fatal(BOOT_ERR_CONF_INVALID,
                       "boot.conf payload GetInfo read failed",
                       "Payload file metadata unreadable.");
        }
        UINT64 file_size = ((EFI_FILE_INFO *)info_buf)->FileSize;
        gBS->FreePool(info_buf);

        if (file_size == 0) {
            file->Close(file);
            root_dir->Close(root_dir);
            serial_early_print("[FATAL] boot.conf payload is empty: '");
            serial_early_print(stg->path);
            serial_early_print("'\n");
            boot_fatal(BOOT_ERR_CONF_INVALID,
                       "boot.conf payload file is empty",
                       "Remove the entry or replace the file.");
        }

        /* Sanity cap against corrupt / malicious FAT metadata. Below
         * the cap, the (file_size + EFI_PAGE_SIZE - 1) overflow is
         * mathematically impossible (256 MiB + 4 KiB < UINT64_MAX),
         * so a single ceiling check covers both bugs at once. */
        if (file_size > BOOT_PAYLOAD_FILE_MAX) {
            file->Close(file);
            root_dir->Close(root_dir);
            serial_early_print("[FATAL] boot.conf payload implausibly large: '");
            serial_early_print(stg->path);
            serial_early_print("' FileSize=0x");
            serial_early_print_hex64(file_size);
            serial_early_print(" (cap=256 MiB)\n");
            boot_fatal(BOOT_ERR_CONF_INVALID,
                       "boot.conf payload exceeds 256 MiB sanity cap",
                       "Filesystem likely corrupt, or payload is wrong file.");
        }

        /* Page-align the allocation so the resulting phys_start is a
         * multiple of 4 KiB and matches the descriptor's alignment=4096
         * promise. AllocatePages always returns page-aligned addresses.
         * Overflow-safe because file_size <= 256 MiB per the cap above. */
        UINTN pages = (UINTN)((file_size + (UINT64)EFI_PAGE_SIZE - 1ull)
                               / (UINT64)EFI_PAGE_SIZE);
        EFI_PHYSICAL_ADDRESS payload_addr = 0;
        status = gBS->AllocatePages(AllocateAnyPages, EfiLoaderData,
                                    pages, &payload_addr);
        if (EFI_ERROR(status)) {
            file->Close(file);
            root_dir->Close(root_dir);
            boot_fatal(BOOT_ERR_CONF_INVALID,
                       "boot.conf payload AllocatePages failed",
                       "Out of EfiLoaderData memory for payload buffer.");
        }

        UINTN read_size = (UINTN)file_size;
        status = file->Read(file, &read_size, (VOID *)(UINTN)payload_addr);
        file->Close(file);
        if (EFI_ERROR(status)) {
            root_dir->Close(root_dir);
            boot_fatal(BOOT_ERR_CONF_INVALID,
                       "boot.conf payload Read failed",
                       "Short or erroring read from payload file.");
        }
        if ((UINT64)read_size != file_size) {
            serial_early_print("[FATAL] boot.conf payload short read: '");
            serial_early_print(stg->path);
            serial_early_print("' expected=");
            serial_early_print_uint((UINT32)file_size);
            serial_early_print(" got=");
            serial_early_print_uint((UINT32)read_size);
            serial_early_print("\n");
            root_dir->Close(root_dir);
            boot_fatal(BOOT_ERR_CONF_INVALID,
                       "boot.conf payload short read",
                       "File truncated during read.");
        }

        /* Populate the matching payload_descriptors[] slot. Packed
         * prefix: slot index == si, so payload_count becomes
         * g_staged_payload_count at the end. */
        struct boot_payload_desc *d = &g_boot_info_ptr->payload_descriptors[si];
        d->type        = stg->type;
        /* FLAG_RESERVED is mandatory on every bootloader-loaded typed
         * payload: the allocation lives in EfiLoaderData, which PMM
         * would otherwise reclaim to the free-page pool after boot.
         * Without this flag the payload pages can be re-handed out as
         * generic kernel memory, silently corrupting the module /
         * initrd / recovery image AFTER boot_payload_validate already
         * passed. PMM reservation of these ranges is owned by the
         * Handoff Memory Ownership section (see XREF in Notes). */
        d->flags       = BOOT_PAYLOAD_FLAG_VALID | BOOT_PAYLOAD_FLAG_RESERVED;
        d->phys_start  = (UINT64)payload_addr;
        d->length      = file_size;
        d->alignment   = 4096ull;  /* AllocatePages guarantees page align */
        d->checksum    = 0ull;     /* FLAG_CHECKSUMMED not set; reserved */
        d->producer_id = BOOT_PRODUCER_UEFI;
        d->_reserved   = 0u;

        total_bytes += file_size;

        serial_early_print("[BOOT] payload[");
        serial_early_print_uint((UINT32)si);
        serial_early_print("] type=");
        serial_early_print_uint(stg->type);
        serial_early_print(" phys=0x");
        serial_early_print_hex64((UINT64)payload_addr);
        serial_early_print(" len=");
        serial_early_print_uint((UINT32)file_size);
        serial_early_print(" path='");
        serial_early_print(stg->path);
        serial_early_print("'\n");
    }

    root_dir->Close(root_dir);

    g_boot_info_ptr->payload_count       = (UINT32)g_staged_payload_count;
    g_boot_info_ptr->payload_overflow    = g_staged_payload_overflow;
    g_boot_info_ptr->payload_total_bytes = total_bytes;
}

/* ============================================================================
 * Pre-jump ABI mismatch check.
 *
 * Runs BEFORE ExitBootServices, AFTER the kernel image has been loaded
 * into file_buf + copied to its final physical address. Walks the
 * kernel ELF for a `.bootproto` section, compares the 4-tuple
 * { magic, version, struct_size, sha256 } against the bootloader's
 * compile-time expected values, and on mismatch OR parse failure:
 *   1. Writes a boot_version_fault record to UEFI NVRAM
 *      (BootVersionFault variable, IPOS GUID, NV|BS|RT attrs) so the
 *      kernel-side blackbox transcribe pipeline picks it up on the
 *      next successful boot.
 *   2. Renders a UCS-2 error on gST->ConOut showing observed vs
 *      expected for each differing field.
 *   3. Stalls 10 seconds so the operator can read the screen.
 *   4. ResetSystem(EfiResetCold, EFI_ABORTED) to reboot.
 *
 * This function does NOT return on mismatch. On match it returns
 * without side effects and load_kernel continues.
 *
 * The boot_version_fault schema mirrors include/kernel/boot_version.h
 * (48 bytes, pinned). A mismatch is classified by priority:
 *   magic  -> BAD_MAGIC (fault_class=2)
 *   version-> BAD_VERSION (fault_class=3)
 *   size   -> BAD_SIZE (fault_class=4)
 *   sha    -> BAD_VERSION with a SHA-hex hint in the UCS-2 message
 *            (closest analog; sha drift = missed version bump).
 * ============================================================================ */

/* Mirror of struct boot_version_fault from include/kernel/boot_version.h.
 * 48 bytes, pinned. Field order MUST match byte-for-byte. */
struct bl_boot_version_fault {
    UINT32 record_magic;
    UINT32 fault_class;
    UINT32 observed_magic;
    UINT32 expected_magic;
    UINT16 observed_version;
    UINT16 expected_version;
    UINT32 observed_size;
    UINT32 expected_size;
    UINT32 observed_loader_sec_ver;
    UINT32 expected_loader_sec_ver;
    UINT32 reserved_pad[3];
};
_Static_assert(sizeof(struct bl_boot_version_fault) == 48,
    "bl_boot_version_fault layout pinned at 48 bytes");

/* Classification enum values match include/kernel/boot_version.h.
 * Keep values in sync manually; the kernel reader renders the name via
 * boot_version_fault_class_name() which is updated in lockstep. */
#define BL_FAULT_OK            0u
#define BL_FAULT_NULL_HDR      1u
#define BL_FAULT_BAD_MAGIC     2u
#define BL_FAULT_BAD_VERSION   3u
#define BL_FAULT_BAD_SIZE      4u
#define BL_FAULT_SEC_ROLLBACK  5u
#define BL_FAULT_BAD_SHA       6u
#define BL_FAULT_BAD_PARSE     7u

/* NVRAM record layout invariants must match include/kernel/boot_version.h. */
#define BL_BOOT_VERSION_FAULT_MAGIC  0x42565046u

/* UEFI console print helper: emit a hex byte. Uses efi_print under the
 * hood so non-ASCII digits work on every UEFI console. */
static void bpp_print_hex32(UINT32 v)
{
    static const CHAR16 hex[] = u"0123456789abcdef";
    CHAR16 buf[11];
    int i;
    buf[0] = u'0';
    buf[1] = u'x';
    for (i = 0; i < 8; i++)
        buf[2 + i] = hex[(v >> ((7 - i) * 4)) & 0xFu];
    buf[10] = 0;
    efi_print(buf);
}

static void bpp_print_decimal(UINT32 v)
{
    CHAR16 buf[11];
    int i = 10;
    buf[10] = 0;
    if (v == 0) {
        efi_print(u"0");
        return;
    }
    while (v > 0 && i > 0) {
        i--;
        buf[i] = (CHAR16)(u'0' + (v % 10));
        v /= 10;
    }
    efi_print(&buf[i]);
}

/* Persist the fault record. Returns EFI_SUCCESS on confirmed NVRAM
 * write; any other status means the record was NOT durably captured
 * and the caller must decide what to do (for rollback refusals we
 * keep the screen up instead of auto-resetting; for structural ABI
 * drift we still reset because a fresh rebuild often resolves it).
 *
 * CRITICAL: name + GUID MUST match what boot_version_blackbox_transcribe()
 * reads in src/kernel/main/boot_version.c (s_fault_name + s_fault_guid).
 * A mismatch silently drops the fault record on the next successful
 * boot. Duplicated from the kernel side rather than shared because the
 * bootloader cannot include kernel/boot_version.h (pulls kernel types). */
static EFI_STATUS bpp_persist_nvram_fault(
    const struct bl_boot_version_fault *rec)
{
    if (!gST || !gST->RuntimeServices || !gST->RuntimeServices->SetVariable)
        return EFI_UNSUPPORTED;
    /* GUID matches kernel src/kernel/main/boot_version.c s_fault_guid:
     * "IMPOSTFV-PROTOFLT" -- distinct from g_impossible_os_guid so a
     * POST16 overwriter cannot alias the fault record. */
    static EFI_GUID fault_guid = {
        0x494D504F, 0x5354, 0x4656,
        { 0x50, 0x52, 0x4F, 0x54, 0x4F, 0x46, 0x4C, 0x54 }
    };
    /* Name matches kernel s_fault_name: "ImpossibleBootProtoFault". */
    static CHAR16 name[] = u"ImpossibleBootProtoFault";
    UINT32 attrs = EFI_VARIABLE_NON_VOLATILE |
                   EFI_VARIABLE_BOOTSERVICE_ACCESS |
                   EFI_VARIABLE_RUNTIME_ACCESS;
    return gST->RuntimeServices->SetVariable(
        name, &fault_guid, attrs,
        sizeof(*rec), (VOID *)rec);
}

/* Render the observed-vs-expected error, persist NVRAM, stall, reset.
 * Does not return. */
static __attribute__((noreturn)) void bpp_render_and_reset(
    const struct bl_boot_version_fault *rec,
    int parse_result,
    UINT8 sha_mismatch)
{
    if (gST && gST->ConOut) {
        gST->ConOut->ClearScreen(gST->ConOut);
        efi_print(u"\r\n");
        efi_print(u"  Impossible OS -- Boot Protocol Mismatch\r\n");
        efi_print(u"\r\n");
        efi_print(u"  The kernel image does not match this bootloader's\r\n");
        efi_print(u"  compile-time ABI. Rebuild + reflash both halves.\r\n");
        efi_print(u"\r\n");
        if (parse_result != BOOTPROTO_OK) {
            efi_print(u"  .bootproto parse failed: ");
            /* Translate to ASCII via narrow print. */
            const char *n = bootproto_result_name(parse_result);
            CHAR16 wide[32];
            int i = 0;
            while (n[i] && i < 31) {
                wide[i] = (CHAR16)(UINT8)n[i];
                i++;
            }
            wide[i] = 0;
            efi_print(wide);
            efi_print(u"\r\n");
        } else {
            efi_print(u"  fault_class: ");
            bpp_print_decimal(rec->fault_class);
            efi_print(u"\r\n  observed_magic:   ");
            bpp_print_hex32(rec->observed_magic);
            efi_print(u"  expected_magic:   ");
            bpp_print_hex32(rec->expected_magic);
            efi_print(u"\r\n  observed_version: ");
            bpp_print_decimal((UINT32)rec->observed_version);
            efi_print(u"  expected_version: ");
            bpp_print_decimal((UINT32)rec->expected_version);
            efi_print(u"\r\n  observed_size:    ");
            bpp_print_decimal(rec->observed_size);
            efi_print(u"  expected_size:    ");
            bpp_print_decimal(rec->expected_size);
            efi_print(u"\r\n");
            if (sha_mismatch) {
                efi_print(u"  sha256 manifest drift detected\r\n");
                efi_print(u"    expected: ");
                /* Wide-print the expected hex (host string macro). */
                const char *hex = KERNEL_ABI_SHA256_HEX;
                CHAR16 wide[65];
                int i = 0;
                while (hex[i] && i < 64) {
                    wide[i] = (CHAR16)(UINT8)hex[i];
                    i++;
                }
                wide[i] = 0;
                efi_print(wide);
                efi_print(u"\r\n");
            }
        }
        efi_print(u"\r\n  Rebooting in 10 seconds...\r\n");
    }

    serial_early_print("[BOOT] ABI MISMATCH: parse=");
    serial_early_print_uint((UINT32)parse_result);
    serial_early_print(" fault_class=");
    serial_early_print_uint(rec->fault_class);
    serial_early_print(" observed_magic=0x");
    serial_early_print_hex16((UINT16)(rec->observed_magic >> 16));
    serial_early_print_hex16((UINT16)rec->observed_magic);
    serial_early_print("\n");

    EFI_STATUS persist_status = bpp_persist_nvram_fault(rec);
    if (persist_status != 0) {
        /* Print the FULL EFI_STATUS as 64-bit hex: the error bit
         * (0x8000000000000000) must not be truncated by a UINT32
         * cast, otherwise the logged value looks like success when
         * it was a real failure. */
        serial_early_print("[BOOT] ABI mismatch: NVRAM persist failed 0x");
        serial_early_print_hex64((UINT64)persist_status);
        serial_early_print(" -- next boot will not carry the transcript\n");
    }

    if (gBS && gBS->Stall)
        gBS->Stall(10 * 1000 * 1000);

    /* UEFI 2.x Reset types: 0=Cold, 1=Warm, 2=Shutdown. EFI_ABORTED
     * per the UEFI specification = 21 | high-bit. Use literals since
     * efi.h does not expose the enum names. */
    if (gST && gST->RuntimeServices && gST->RuntimeServices->ResetSystem) {
        gST->RuntimeServices->ResetSystem(
            /* EfiResetCold  */ 0,
            /* EFI_ABORTED   */ (21ULL | (1ULL << 63)),
            0, (VOID *)0);
    }

    /* Reset unreachable on well-formed firmware; fall through to HLT
     * as a last resort so the operator is not left with a silent
     * re-jump into a mismatched kernel. */
    for (;;)
        __asm__ volatile ("cli; hlt");
}

/* Render the anti-rollback refusal path. Called from the pre-jump
 * anti-rollback gate when shipped < required. Populates a 48-byte
 * boot_version_fault record with fault_class=SEC_ROLLBACK and
 * {observed,expected}_loader_sec_ver populated, persists to NVRAM via
 * the same helper the structural-mismatch path uses, and HALTS with
 * the operator-visible screen preserved.
 *
 * Distinct from bpp_render_and_reset (which handles structural ABI
 * drift): rollback refusal is NEVER transient. The stored required
 * value does not change between reboots, so auto-resetting would
 * just bring the operator back to the same error. HLT preserves the
 * diagnostic screen until the operator power-cycles and enters the
 * firmware's UEFI shell (via the firmware F12/Escape menu) to either
 * flash a newer signed kernel.exe OR clear the
 * IPOSRequiredSecVersion NVRAM variable under operator consent.
 *
 * Operator response differs from ABI drift. Rollback refusal means
 * "boot a newer signed kernel OR clear IPOSRequiredSecVersion";
 * ABI drift means "rebuild both halves" (where auto-reset after a
 * fresh build is actually helpful).
 *
 * Does not return. */
static __attribute__((noreturn)) void bpp_render_rollback_and_halt(
    UINT32 shipped, UINT32 required)
{
    struct bl_boot_version_fault rec;
    {
        UINT8 *p = (UINT8 *)&rec;
        UINTN i;
        for (i = 0; i < sizeof(rec); i++) p[i] = 0;
    }
    rec.record_magic             = BL_BOOT_VERSION_FAULT_MAGIC;
    rec.fault_class              = BL_FAULT_SEC_ROLLBACK;
    /* observed/expected structural fields stay zero; magic/version/
     * size are not the drift axis here. The rollback-specific fields
     * below carry the operator-actionable numbers. */
    rec.observed_loader_sec_ver  = shipped;
    rec.expected_loader_sec_ver  = required;

    EFI_STATUS persist_status = bpp_persist_nvram_fault(&rec);

    /* UEFI console (gST->ConOut) is ONLY safe pre-EBS. The anti-
     * rollback gate currently runs AFTER ExitBootServices at
     * boot_hw.c's refusal site, so touching ConOut here would drive
     * an undefined service. Gate the rich UCS-2 render on
     * !g_ebs_in_progress and fall back to serial-only when Boot
     * Services have been exited. Serial is post-EBS safe; Runtime
     * Services (used by bpp_persist_nvram_fault above) are too. */
    if (!g_ebs_in_progress && gST && gST->ConOut) {
        gST->ConOut->ClearScreen(gST->ConOut);
        efi_print(u"\r\n");
        efi_print(u"  Impossible OS -- Anti-Rollback Refusal\r\n");
        efi_print(u"\r\n");
        efi_print(u"  Security-version downgrade refused:\r\n");
        efi_print(u"    image    = ");
        bpp_print_decimal(shipped);
        efi_print(u"\r\n    required = ");
        bpp_print_decimal(required);
        efi_print(u"\r\n");
        efi_print(u"\r\n");
        efi_print(u"  Boot a newer signed kernel, or clear the\r\n");
        efi_print(u"  IPOSRequiredSecVersion UEFI NVRAM variable under\r\n");
        efi_print(u"  operator consent (UEFI shell: setvar / dmpstore)\r\n");
        efi_print(u"  and retry.\r\n");
        efi_print(u"\r\n");
        if (persist_status == 0) {
            efi_print(u"  Diagnostic persisted to UEFI NVRAM "
                      u"(ImpossibleBootProtoFault).\r\n");
        } else {
            efi_print(u"  WARNING: NVRAM persist failed; "
                      u"next boot will not have the transcript.\r\n");
            efi_print(u"  See serial log for full EFI_STATUS value.\r\n");
        }
        efi_print(u"\r\n  Power-cycle and enter the UEFI shell to recover.\r\n");
    }

    /* Serial diagnostic (always safe, pre- or post-EBS). Format the
     * full EFI_STATUS as a 64-bit hex value so the top error bit
     * (0x8000000000000000) is preserved -- truncating to UINT32 drops
     * the error flag and makes every failure look like success. */
    serial_early_print("[BOOT] ANTI-ROLLBACK REFUSAL: "
                       "fault_class=SEC_ROLLBACK shipped=");
    serial_early_print_uint(shipped);
    serial_early_print(" required=");
    serial_early_print_uint(required);
    serial_early_print(" persist_status=0x");
    serial_early_print_hex64((UINT64)persist_status);
    if (g_ebs_in_progress)
        serial_early_print(" (post-EBS: ConOut skipped, serial-only)");
    serial_early_print("\n");

    /* Halt with the screen/serial preserved. NO auto-reset: rollback
     * refusal has no path forward without operator intervention, and
     * resetting into the same refusal destroys the only diagnostic the
     * operator has. Power-cycle (operator-triggered) is the correct
     * next step. */
    for (;;)
        __asm__ volatile ("cli; hlt");
}

/* Verify the loaded kernel image carries a matching .bootproto
 * descriptor. Called from load_kernel right before its final success
 * return, with file_buf still valid. */
static void bootproto_verify_or_reset(const UINT8 *kernel_image,
                                      UINT64 kernel_size)
{
    struct boot_proto_descriptor desc;
    int r = bootproto_find(kernel_image, kernel_size, &desc);

    struct bl_boot_version_fault rec;
    {
        UINT8 *p = (UINT8 *)&rec;
        UINTN i;
        for (i = 0; i < sizeof(rec); i++) p[i] = 0;
    }
    rec.record_magic = BL_BOOT_VERSION_FAULT_MAGIC;

    UINT32 expected_magic   = (UINT32)BOOT_PROTO_DESCRIPTOR_MAGIC;
    UINT32 expected_version = (UINT32)BOOT_INFO_VERSION;
    UINT32 expected_size    = (UINT32)sizeof(struct boot_info);
    static const UINT8 expected_sha[32] = KERNEL_ABI_SHA256;

    /* Fill NVRAM record with bootloader-expected values; observed
     * fields will be overwritten if the descriptor parsed OK. */
    rec.expected_magic    = expected_magic;
    rec.expected_version  = (UINT16)expected_version;
    rec.expected_size     = expected_size;

    if (r != BOOTPROTO_OK) {
        /* Parse failure: dedicated BAD_PARSE class so the blackbox
         * transcript can distinguish a missing/truncated/misaligned
         * `.bootproto` section from a structural ABI mismatch. Stash
         * the raw parser error enum in observed_loader_sec_ver so the
         * kernel-side reader can surface the specific step that
         * failed (missing section / bounds / alignment / etc.). */
        rec.fault_class             = BL_FAULT_BAD_PARSE;
        rec.observed_magic          = 0;
        rec.observed_version        = 0;
        rec.observed_size           = 0;
        rec.observed_loader_sec_ver = (UINT32)r;
        bpp_render_and_reset(&rec, r, 0);
    }

    rec.observed_magic   = desc.magic;
    rec.observed_version = (UINT16)desc.version;
    rec.observed_size    = desc.struct_size;

    if (desc.magic != expected_magic) {
        rec.fault_class = BL_FAULT_BAD_MAGIC;
        bpp_render_and_reset(&rec, BOOTPROTO_OK, 0);
    }
    if (desc.version != expected_version) {
        rec.fault_class = BL_FAULT_BAD_VERSION;
        bpp_render_and_reset(&rec, BOOTPROTO_OK, 0);
    }
    if (desc.struct_size != expected_size) {
        rec.fault_class = BL_FAULT_BAD_SIZE;
        bpp_render_and_reset(&rec, BOOTPROTO_OK, 0);
    }

    {
        UINTN i;
        int sha_mismatch = 0;
        for (i = 0; i < 32; i++) {
            if (desc.sha256[i] != expected_sha[i]) {
                sha_mismatch = 1;
                break;
            }
        }
        if (sha_mismatch) {
            /* Magic + version + size all match but manifest hash
             * diverged. Dedicated BAD_SHA class so the blackbox
             * transcript names the real cause instead of a generic
             * version mismatch. */
            rec.fault_class = BL_FAULT_BAD_SHA;
            bpp_render_and_reset(&rec, BOOTPROTO_OK, 1);
        }
    }

    /* All fields match -- log once and return. */
    serial_early_print("[BOOT] .bootproto match: magic=0x");
    serial_early_print_hex16((UINT16)(desc.magic >> 16));
    serial_early_print_hex16((UINT16)desc.magic);
    serial_early_print(" version=");
    serial_early_print_uint(desc.version);
    serial_early_print(" struct_size=");
    serial_early_print_uint(desc.struct_size);
    serial_early_print(" sha256=match\n");
}

/* ============================================================================
 * Step 2: Load kernel ELF from FAT32
 * ============================================================================ */
static EFI_STATUS load_kernel(UINT64 *entry_point)
{
    EFI_GUID fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
    EFI_FILE_PROTOCOL *root_dir, *kernel_file;
    EFI_STATUS status;
    UINT8 *file_buf;
    UINTN file_size;
    Elf64_Ehdr *ehdr;
    Elf64_Phdr *phdr;
    UINT16 i;

    /* Use global g_boot_device_handle (set in efi_main) to get the
     * boot device's filesystem.  Fall back to LocateProtocol if the
     * handle was not resolved OR if the handle lacks SimpleFS (e.g.
     * PXE boot, partition handle without filesystem driver). */
    fs = (EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *)0;
    if (g_boot_device_handle) {
        status = gBS->HandleProtocol(g_boot_device_handle,
                                      &fs_guid, (VOID **)&fs);
        if (EFI_ERROR(status)) {
            serial_early_print("[WARN] Boot device has no filesystem, "
                               "trying LocateProtocol fallback\n");
            status = gBS->LocateProtocol(&fs_guid, (VOID *)0, (VOID **)&fs);
        }
    } else {
        status = gBS->LocateProtocol(&fs_guid, (VOID *)0, (VOID **)&fs);
    }
    if (EFI_ERROR(status)) {
        serial_early_print("[FAIL] File system protocol not found\n");
        return status;
    }

    /* Open root directory */
    status = fs->OpenVolume(fs, &root_dir);
    if (EFI_ERROR(status)) {
        serial_early_print("[FAIL] Cannot open root volume\n");
        return status;
    }

    /* Fallback kernel search: try paths in order (S3) */
    {
        static const CHAR16 *kernel_paths[] = {
            u"\\boot\\kernel.exe",
            u"\\kernel.exe",
            u"\\EFI\\ImpossibleOS\\kernel.exe",
        };
        static const char *kernel_path_names[] = {
            "\\boot\\kernel.exe",
            "\\kernel.exe",
            "\\EFI\\ImpossibleOS\\kernel.exe",
        };
        int found = 0;
        UINT16 pi;
        for (pi = 0; pi < 3; pi++) {
            serial_early_print("[BOOT] Trying ");
            serial_early_print(kernel_path_names[pi]);
            serial_early_print("...\n");
            status = root_dir->Open(root_dir, &kernel_file,
                                     (CHAR16 *)kernel_paths[pi],
                                     EFI_FILE_MODE_READ, 0);
            if (!EFI_ERROR(status)) {
                serial_early_print("[BOOT] Kernel found at ");
                serial_early_print(kernel_path_names[pi]);
                serial_early_print("\n");
                found = 1;
                break;
            }
            /* Only continue searching on EFI_NOT_FOUND; any other error
             * (EFI_DEVICE_ERROR, EFI_VOLUME_CORRUPTED, etc.) is a hard
             * failure -- report and stop. */
            if (status != EFI_NOT_FOUND) {
                serial_early_print("[FAIL] Error opening ");
                serial_early_print(kernel_path_names[pi]);
                serial_early_print(" (device/FS error)\n");
                root_dir->Close(root_dir);
                return status;
            }
        }
        if (!found) {
            /*: Device fallback chain -- kernel not on boot device,
             * try all other filesystems before giving up. */
            root_dir->Close(root_dir);
            post_code16(POST16_BL_FALLBACK);
            serial_early_print("[WARN] Kernel not on boot device, "
                               "searching other volumes...\n");
            {
                EFI_GUID fs_fb_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
                EFI_HANDLE *fs_handles = (EFI_HANDLE *)0;
                UINTN fs_count = 0;
                EFI_STATUS fb_s;

                fb_s = gBS->LocateHandleBuffer(ByProtocol, &fs_fb_guid,
                                                (VOID *)0, &fs_count,
                                                &fs_handles);
                if (EFI_ERROR(fb_s) || !fs_handles || fs_count == 0) {
                    serial_early_print("[WARN] LocateHandleBuffer failed "
                                       "or no filesystems found\n");
                } else {
                    UINTN hi;
                    for (hi = 0; hi < fs_count && !found; hi++) {
                        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fb_fs;
                        EFI_FILE_PROTOCOL *fb_root;

                        /* Skip the boot device -- already tried */
                        if (fs_handles[hi] == g_boot_device_handle)
                            continue;

                        fb_s = gBS->HandleProtocol(fs_handles[hi],
                                                    &fs_fb_guid,
                                                    (VOID **)&fb_fs);
                        if (EFI_ERROR(fb_s)) continue;

                        fb_s = fb_fs->OpenVolume(fb_fs, &fb_root);
                        if (EFI_ERROR(fb_s)) continue;

                        for (pi = 0; pi < 3; pi++) {
                            fb_s = fb_root->Open(fb_root, &kernel_file,
                                                  (CHAR16 *)kernel_paths[pi],
                                                  EFI_FILE_MODE_READ, 0);
                            if (!EFI_ERROR(fb_s)) {
                                serial_early_print("[WARN] Kernel found on "
                                                   "non-boot device at ");
                                serial_early_print(kernel_path_names[pi]);
                                serial_early_print("\n");
                                /* Use this volume's root for the rest
                                 * of load_kernel */
                                root_dir = fb_root;
                                fs = fb_fs;
                                found = 1;
                                break;
                            }
                        }
                        if (!found)
                            fb_root->Close(fb_root);
                    }
                    gBS->FreePool(fs_handles);
                }
            }

            if (!found) {
                serial_early_print(
                    "[FAIL] Kernel not found on any volume. Searched: "
                    "\\boot\\kernel.exe, \\kernel.exe, "
                    "\\EFI\\ImpossibleOS\\kernel.exe\n");
                /* No POST16_BL_FALLBACK_OK -- last POST stays at 0xB094
                 * so a POST card shows fallback failed. */
                return EFI_NOT_FOUND;
            }
            post_code16(POST16_BL_FALLBACK_OK);
        }
    }

    /* Read entire file into memory.
     * Graduated allocation fallback (S8): try 32 -> 16 -> 8 MiB. */
    file_size = 0;
    {
        static const UINTN alloc_sizes[] = {
            32 * 1024 * 1024,
            16 * 1024 * 1024,
             8 * 1024 * 1024,
        };
        EFI_PHYSICAL_ADDRESS buf_addr = 0;
        UINTN alloc_size = 0;
        int ai;
        for (ai = 0; ai < 3; ai++) {
            UINTN pages = alloc_sizes[ai] / EFI_PAGE_SIZE;
            status = gBS->AllocatePages(AllocateAnyPages, EfiLoaderData,
                                         pages, &buf_addr);
            if (!EFI_ERROR(status)) {
                alloc_size = alloc_sizes[ai];
                serial_early_print("[BOOT] Kernel buffer: ");
                serial_early_print_uint((UINT32)(alloc_size / (1024 * 1024)));
                serial_early_print(" MiB allocated\n");
                break;
            }
        }
        if (alloc_size == 0) {
            serial_early_print("[FAIL] Cannot allocate kernel buffer (tried 32/16/8 MiB)\n");
            return EFI_LOAD_ERROR;
        }
        file_buf = (UINT8 *)(UINTN)buf_addr;
        file_size = alloc_size;

        /* Verify buffer doesn't overlap boot_info region.
         * S16: The protected region is the FULL struct boot_info at
         * BOOT_INFO_PHYS_ADDR, not just the first 4 KiB.  sizeof(struct
         * boot_info) is ~22 KiB and the S15 assert caps it at 65535,
         * so any allocation that intersects [0x10000, 0x10000+sizeof)
         * would silently clobber the handoff tail. */
        {
            UINT64 bi_start = (UINT64)BOOT_INFO_PHYS_ADDR;
            UINT64 bi_end   = bi_start + (UINT64)sizeof(struct boot_info);
            if ((UINT64)buf_addr < bi_end &&
                (UINT64)buf_addr + (UINT64)alloc_size > bi_start) {
                serial_early_print("[FAIL] Kernel buffer overlaps boot_info region\n");
                return EFI_LOAD_ERROR;
            }
        }
        /* Verify buffer doesn't overlap framebuffer */
        if (g_boot_info_ptr->fb.addr != 0) {
            UINT64 fb_end = g_boot_info_ptr->fb.addr +
                (UINT64)g_boot_info_ptr->fb.pitch * g_boot_info_ptr->fb.height;
            if (buf_addr < fb_end && buf_addr + alloc_size > g_boot_info_ptr->fb.addr) {
                serial_early_print("[FAIL] Kernel buffer overlaps framebuffer\n");
                return EFI_LOAD_ERROR;
            }
        }
    }

    status = kernel_file->Read(kernel_file, &file_size, file_buf);
    if (EFI_ERROR(status)) {
        efi_print(u"[FAIL] Cannot read kernel file\r\n");
        return status;
    }

    kernel_file->Close(kernel_file);
    root_dir->Close(root_dir);

    /* Parse ELF header */
    ehdr = (Elf64_Ehdr *)file_buf;
    if (ehdr->e_magic != ELF_MAGIC || ehdr->e_class != 2 ||
        ehdr->e_machine != 0x3E) {
        serial_early_print("[FAIL] Kernel ELF corrupt: invalid magic/class/machine\n");
        return EFI_LOAD_ERROR;
    }

    /* --- ELF bounds validation (S1 hardening) --- */

    /* Validate e_phentsize matches expected Elf64_Phdr size.
     * A mismatch means phdr indexing reads wrong offsets -- reject. */
    if (ehdr->e_phentsize != sizeof(Elf64_Phdr)) {
        serial_early_print("[FAIL] Kernel ELF corrupt: e_phentsize mismatch\n");
        return EFI_LOAD_ERROR;
    }

    /* Cap e_phnum to prevent huge loop on corrupt ELF (matches Linux ELF_MAX_SEGMENTS spirit) */
    if (ehdr->e_phnum > 64) {
        serial_early_print("[FAIL] Kernel ELF corrupt: too many program headers\n");
        return EFI_LOAD_ERROR;
    }

    /* Cap total kernel size at 32 MiB */
#define ELF_MAX_KERNEL_SIZE (32ULL * 1024 * 1024)
    if (file_size > ELF_MAX_KERNEL_SIZE) {
        serial_early_print("[FAIL] Kernel ELF corrupt: file exceeds 32 MiB limit\n");
        return EFI_LOAD_ERROR;
    }

    /* Validate program header table is within file bounds.
     * Use subtraction-based check to prevent integer wraparound. */
    if (ehdr->e_phoff > file_size) {
        serial_early_print("[FAIL] Kernel ELF corrupt: phdr offset past EOF\n");
        return EFI_LOAD_ERROR;
    }
    if (ehdr->e_phnum > (file_size - ehdr->e_phoff) / sizeof(Elf64_Phdr)) {
        serial_early_print("[FAIL] Kernel ELF corrupt: phdr table past EOF\n");
        return EFI_LOAD_ERROR;
    }

    /* Load PT_LOAD segments with per-segment validation */
    phdr = (Elf64_Phdr *)(file_buf + ehdr->e_phoff);
    for (i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type != PT_LOAD)
            continue;

        /* Validate segment file data is within file bounds.
         * Subtraction-based: p_offset must fit, then filesz must fit in remainder. */
        if (phdr[i].p_offset > file_size ||
            phdr[i].p_filesz > file_size - phdr[i].p_offset) {
            serial_early_print("[FAIL] Kernel ELF corrupt: segment ");
            serial_early_print_uint(i);
            serial_early_print(" data past EOF\n");
            return EFI_LOAD_ERROR;
        }

        /* Validate memsz >= filesz (ELF spec requirement) */
        if (phdr[i].p_memsz < phdr[i].p_filesz) {
            serial_early_print("[FAIL] Kernel ELF corrupt: segment ");
            serial_early_print_uint(i);
            serial_early_print(" memsz < filesz\n");
            return EFI_LOAD_ERROR;
        }

        /* Reject segments with address wraparound */
        if (phdr[i].p_memsz > 0xFFFFFFFFFFFFFFFFULL - phdr[i].p_paddr) {
            serial_early_print("[FAIL] Kernel ELF corrupt: segment ");
            serial_early_print_uint(i);
            serial_early_print(" address wraparound\n");
            return EFI_LOAD_ERROR;
        }

        /* Reject segments that overlap the FULL boot_info region.
         * S16: Previously guarded only [0x10000, 0x11000) -- but struct
         * boot_info is ~22 KiB so segments loaded at 0x11000..0x155BF
         * would silently clobber the tail (config, framebuffer, timing,
         * etc.) and still pass the post-handoff validator because the
         * header at offset 0 is untouched.  Use sizeof(struct boot_info)
         * as the upper bound -- kept <= 65535 by the S15 static assert. */
        {
            UINT64 seg_start = phdr[i].p_paddr;
            UINT64 seg_end   = seg_start + phdr[i].p_memsz;
            UINT64 bi_start  = (UINT64)BOOT_INFO_PHYS_ADDR;
            UINT64 bi_end    = bi_start + (UINT64)sizeof(struct boot_info);
            if (seg_start < bi_end && seg_end > bi_start) {
                serial_early_print("[FAIL] Kernel ELF corrupt: segment ");
                serial_early_print_uint(i);
                serial_early_print(" overlaps boot_info region\n");
                return EFI_LOAD_ERROR;
            }
        }

        /* Reject segments that overlap framebuffer */
        if (g_boot_info_ptr->fb.addr != 0 &&
            g_boot_info_ptr->fb.height != 0 &&
            g_boot_info_ptr->fb.pitch != 0) {
            UINT64 seg_start = phdr[i].p_paddr;
            UINT64 seg_end   = seg_start + phdr[i].p_memsz;
            UINT64 fb_start  = g_boot_info_ptr->fb.addr;
            UINT64 fb_size   = (UINT64)g_boot_info_ptr->fb.pitch *
                               g_boot_info_ptr->fb.height;
            UINT64 fb_end    = fb_start + fb_size;
            /* Guard against fb overflow (pitch*height) */
            if (fb_size / g_boot_info_ptr->fb.pitch ==
                g_boot_info_ptr->fb.height &&
                fb_end >= fb_start) {
                if (seg_start < fb_end && seg_end > fb_start) {
                    serial_early_print("[FAIL] Kernel ELF corrupt: segment ");
                    serial_early_print_uint(i);
                    serial_early_print(" overlaps framebuffer\n");
                    return EFI_LOAD_ERROR;
                }
            }
        }

        /* Log loaded segment */
        serial_early_print("[BOOT] ELF segment ");
        serial_early_print_uint(i);
        serial_early_print(": paddr=0x");
        serial_early_print_hex16((UINT16)(phdr[i].p_paddr >> 16));
        serial_early_print_hex16((UINT16)(phdr[i].p_paddr));
        serial_early_print(" filesz=");
        serial_early_print_uint((UINT32)phdr[i].p_filesz);
        serial_early_print(" memsz=");
        serial_early_print_uint((UINT32)phdr[i].p_memsz);
        serial_early_print("\n");

        /* Copy segment to its physical address */
        {
            UINT8 *dst = (UINT8 *)(UINTN)phdr[i].p_paddr;
            UINT8 *src = file_buf + phdr[i].p_offset;
            UINTN copy_size = (UINTN)phdr[i].p_filesz;
            UINTN mem_size = (UINTN)phdr[i].p_memsz;

            /* Copy file data */
            efi_memcpy(dst, src, copy_size);

            /* Zero BSS portion (memsz > filesz) */
            if (mem_size > copy_size)
                efi_memset(dst + copy_size, 0, mem_size - copy_size);
        }
    }

    /* Find kernel_main symbol in the ELF symbol table.
     * The ELF entry point (_start) is 32-bit code for GRUB compatibility.
     * Since UEFI is already in 64-bit Long Mode, we must call kernel_main
     * directly, skipping the 32→64 mode transition in entry.asm. */
    {
        Elf64_Shdr *shdr = (Elf64_Shdr *)(file_buf + ehdr->e_shoff);
        UINT16 s;
        UINT64 km_addr = 0;

        for (s = 0; s < ehdr->e_shnum; s++) {
            if (shdr[s].sh_type == SHT_SYMTAB) {
                Elf64_Sym *syms = (Elf64_Sym *)(file_buf + shdr[s].sh_offset);
                UINT64 nsyms = shdr[s].sh_size / shdr[s].sh_entsize;
                /* String table is in section shdr[s].sh_link */
                char *strtab = (char *)(file_buf + shdr[shdr[s].sh_link].sh_offset);
                UINT64 j;

                for (j = 0; j < nsyms; j++) {
                    char *name = strtab + syms[j].st_name;
                    /* Compare with "kernel_main" */
                    if (name[0]=='k' && name[1]=='e' && name[2]=='r' &&
                        name[3]=='n' && name[4]=='e' && name[5]=='l' &&
                        name[6]=='_' && name[7]=='m' && name[8]=='a' &&
                        name[9]=='i' && name[10]=='n' && name[11]=='\0') {
                        km_addr = syms[j].st_value;
                        break;
                    }
                }
                if (km_addr) break;
            }
        }

        if (km_addr == 0) {
            efi_print(u"[FAIL] kernel_main symbol not found in ELF\r\n");
            return EFI_LOAD_ERROR;
        }

        *entry_point = km_addr;
    }

    /* Pre-jump ABI mismatch check: scan the loaded kernel ELF for
     * `.bootproto`, compare 4-tuple against bootloader's compile-time
     * expected bytes. On mismatch the function does not return --
     * it persists a NVRAM fault record, renders a UCS-2 screen, and
     * reboots cold. file_buf + file_size are still valid here. */
    bootproto_verify_or_reset(file_buf, (UINT64)file_size);

    return EFI_SUCCESS;
}

/* ============================================================================
 * Step 4: Get memory map and convert to boot_info format
 * ============================================================================ */
static EFI_STATUS get_memory_map(UINTN *map_key_out,
                                  EFI_MEMORY_DESCRIPTOR **map_out,
                                  UINTN *map_size_out,
                                  UINTN *desc_size_out,
                                  UINT32 *desc_version_out)
{
    EFI_STATUS status;
    UINTN map_size = 0;
    UINTN desc_size;
    UINT32 desc_version;
    UINTN map_key;
    EFI_MEMORY_DESCRIPTOR *mmap = (EFI_MEMORY_DESCRIPTOR *)0;

    /* First call to get required size */
    status = gBS->GetMemoryMap(&map_size, mmap, &map_key, &desc_size,
                                &desc_version);
    /* Expected: EFI_BUFFER_TOO_SMALL, map_size is now set */

    /* Add extra space for the allocation itself */
    map_size += 2 * desc_size;

    status = gBS->AllocatePool(EfiLoaderData, map_size, (VOID **)&mmap);
    if (EFI_ERROR(status))
        return status;

    status = gBS->GetMemoryMap(&map_size, mmap, &map_key, &desc_size,
                                &desc_version);
    if (EFI_ERROR(status))
        return status;

    *map_key_out = map_key;
    *map_out = mmap;
    *map_size_out = map_size;
    *desc_size_out = desc_size;
    *desc_version_out = desc_version;

    return EFI_SUCCESS;
}

/* Convert UEFI memory type to Multiboot2-compatible type */
static UINT32 uefi_to_mb2_memtype(UINT32 efi_type)
{
    switch (efi_type) {
    case EfiConventionalMemory:
    case EfiBootServicesCode:
    case EfiBootServicesData:
    case EfiLoaderCode:
    case EfiLoaderData:
        return 1;  /* Available */
    case EfiACPIReclaimMemory:
        return 3;  /* ACPI reclaimable */
    case EfiACPIMemoryNVS:
        return 4;  /* NVS */
    case EfiUnusableMemory:
        return 5;  /* Bad */
    default:
        return 2;  /* Reserved */
    }
}

/* ============================================================================
 * S17: Memory map normalization -- sort by base address, then carve
 * overlapping descriptors by UEFI memory type priority.
 *
 * Real firmware occasionally emits overlapping descriptors (particularly
 * ACPI Reclaim regions straddling BootServices buffers) that must be
 * resolved, not skipped, before the kernel PMM consumes the map.  The
 * normalization pass runs after fill_memory_map() accepts raw entries
 * and before mmap_count is published to boot_info.
 *
 * Priority order (higher wins the contested range):
 *   100 -- Reserved, MMIO, MMIOPort, PalCode, Unusable, Unknown (can't reclaim)
 *    95 -- RuntimeServicesCode/Data (survives ExitBootServices)
 *    85 -- ACPI NVS (firmware data that must not be overwritten)
 *    80 -- ACPI Reclaim (copied by kernel, then reclaimable later)
 *    70 -- Persistent (NVDIMM)
 *    50 -- BootServicesCode/Data, LoaderCode/Data (reclaimable post-EBS)
 *    40 -- Conventional (lowest priority, freely reclaimable)
 *
 * Runtime is promoted ABOVE ACPI Reclaim because runtime code/data must
 * remain reserved for the firmware even after the kernel copies the
 * ACPI tables out of an overlapping reclaim range.
 * ============================================================================ */

static int mmap_type_priority(UINT32 uefi_type)
{
    switch (uefi_type) {
    case EfiReservedMemoryType:
    case EfiMemoryMappedIO:
    case EfiMemoryMappedIOPortSpace:
    case EfiPalCode:
    case EfiUnusableMemory:
        return 100;
    case EfiRuntimeServicesCode:
    case EfiRuntimeServicesData:
        return 95;
    case EfiACPIMemoryNVS:
        return 85;
    case EfiACPIReclaimMemory:
        return 80;
    case EfiPersistentMemory:
        return 70;
    case EfiBootServicesCode:
    case EfiBootServicesData:
    case EfiLoaderCode:
    case EfiLoaderData:
        return 50;
    case EfiConventionalMemory:
        return 40;
    default:
        return 100;  /* conservative: treat unknown as reserved */
    }
}

/* Shift arr[i+1..count) left by one, decrementing count.  Used to remove
 * the entry at index i. */
static void mmap_remove_at(struct boot_mmap_entry *arr, UINT32 *count, UINT32 i)
{
    UINT32 k;
    if (i >= *count) return;
    for (k = i; k + 1 < *count; k++) arr[k] = arr[k + 1];
    (*count)--;
}

/* Log the canonical overlap-resolved line. */
static void mmap_log_resolved(UINT64 at, UINT32 winner_type, UINT32 loser_type)
{
    serial_early_print("[BOOT] mmap: overlap resolved at 0x");
    serial_early_print_hex16((UINT16)(at >> 48));
    serial_early_print_hex16((UINT16)(at >> 32));
    serial_early_print_hex16((UINT16)(at >> 16));
    serial_early_print_hex16((UINT16)at);
    serial_early_print(" -- type ");
    serial_early_print_uint(winner_type);
    serial_early_print(" wins over type ");
    serial_early_print_uint(loser_type);
    serial_early_print("\n");
}

/* --- Sweep-line normalization (S17) ---------------------------------
 *
 * Codex adversarial round 1 flagged that an iterative pair-resolver
 * with an ad-hoc pass guard could exit before the map stabilized on
 * pathological inputs.  A provably correct O(n^2) sweep-line pass
 * replaces it:
 *
 *   1. Copy input into a work buffer, build 2n endpoint events.
 *   2. Sort events by (addr, end-before-start) so touching ranges do
 *      not create zero-width spurious intervals.
 *   3. Walk events linearly.  Between consecutive events the active
 *      set is fixed; emit one range taking the highest-priority active
 *      descriptor, coalescing with the previous output if adjacent,
 *      same type, and same attribute.
 *
 * Storage: MMAP_NORMALIZE_MAX = BOOT_MMAP_MAX_ENTRIES.  Event, work,
 * and active-set buffers live in BSS (~34 KiB) so the bootloader
 * stack stays shallow.
 * -------------------------------------------------------------------- */

#define MMAP_NORMALIZE_MAX  BOOT_MMAP_MAX_ENTRIES

struct mmap_event {
    UINT64 addr;
    UINT32 desc_idx;
    UINT8  is_start;   /* 1 = descriptor start, 0 = descriptor end */
    UINT8  _pad[3];
};

static struct mmap_event        s_mmap_events[2 * MMAP_NORMALIZE_MAX];
static struct boot_mmap_entry   s_mmap_work[MMAP_NORMALIZE_MAX];
static UINT32                   s_mmap_active[MMAP_NORMALIZE_MAX];

static void mmap_event_sort(struct mmap_event *ev, UINT32 count)
{
    /* Insertion sort: nearly-sorted firmware maps run in ~O(n).
     * Tiebreak end-before-start at equal addr so adjacent ranges
     * [A.end == B.start] close A before opening B. */
    UINT32 i, j;
    for (i = 1; i < count; i++) {
        struct mmap_event key = ev[i];
        j = i;
        while (j > 0) {
            if (ev[j - 1].addr > key.addr ||
                (ev[j - 1].addr == key.addr &&
                 ev[j - 1].is_start > key.is_start)) {
                ev[j] = ev[j - 1];
                j--;
            } else {
                break;
            }
        }
        ev[j] = key;
    }
}

static UINT32 mmap_active_find_winner(UINT32 active_count)
{
    /* Stable tiebreak: when two active descriptors share the same
     * priority, pick the one with the lower original index into
     * s_mmap_work.  Original indices never change, so the winner is
     * independent of the active-set removal order.  Without this
     * tiebreak, mmap_active_remove's swap-remove could shift a
     * different same-priority descriptor to the front and flip the
     * winner based on unrelated event ordering. */
    UINT32 best = s_mmap_active[0];
    int best_pri = mmap_type_priority(s_mmap_work[best].uefi_memory_type);
    UINT32 k;
    for (k = 1; k < active_count; k++) {
        UINT32 idx = s_mmap_active[k];
        int pri = mmap_type_priority(s_mmap_work[idx].uefi_memory_type);
        if (pri > best_pri || (pri == best_pri && idx < best)) {
            best = idx;
            best_pri = pri;
        }
    }
    return best;
}

/* Swap-remove desc_idx from the active set.  Returns new count. */
static UINT32 mmap_active_remove(UINT32 active_count, UINT32 desc_idx)
{
    UINT32 k;
    for (k = 0; k < active_count; k++) {
        if (s_mmap_active[k] == desc_idx) {
            s_mmap_active[k] = s_mmap_active[active_count - 1];
            return active_count - 1;
        }
    }
    return active_count;
}

/* Find the first active descriptor that is not `winner_idx` and return
 * its UEFI memory type -- used only for the overlap-resolved log line. */
static UINT32 mmap_active_first_loser_type(UINT32 active_count, UINT32 winner_idx)
{
    UINT32 k;
    for (k = 0; k < active_count; k++) {
        UINT32 idx = s_mmap_active[k];
        if (idx != winner_idx) return s_mmap_work[idx].uefi_memory_type;
    }
    return 0;
}

/* Append or coalesce one output segment.  Returns 0 if the segment
 * was dropped due to the array cap (caller sets mmap_quirks). */
static int mmap_emit_segment(struct boot_mmap_entry *arr, UINT32 *out_count,
                              UINT32 max_entries, UINT64 seg_start,
                              UINT64 seg_end, UINT32 win_type,
                              UINT32 win_simple, UINT64 win_attr)
{
    if (*out_count > 0) {
        struct boot_mmap_entry *last = &arr[*out_count - 1];
        if (last->base_addr + last->length == seg_start &&
            last->uefi_memory_type == win_type &&
            last->attribute == win_attr) {
            last->length += (seg_end - seg_start);
            return 1;
        }
    }
    if (*out_count >= max_entries) return 0;
    arr[*out_count].base_addr        = seg_start;
    arr[*out_count].length           = seg_end - seg_start;
    arr[*out_count].type             = win_simple;
    arr[*out_count].uefi_memory_type = win_type;
    arr[*out_count].attribute        = win_attr;
    (*out_count)++;
    return 1;
}

static void mmap_normalize(struct boot_mmap_entry *arr, UINT32 *count,
                            UINT32 max_entries)
{
    UINT32 n = *count;
    UINT32 ev_count = 0;
    UINT32 active_count = 0;
    UINT32 out_count = 0;
    UINT32 i;
    UINT64 prev_addr = 0;
    int prev_valid = 0;

    if (n < 2) return;
    if (n > MMAP_NORMALIZE_MAX) n = MMAP_NORMALIZE_MAX;

    /* Snapshot validated input into the work buffer.  The active-set
     * indices reference s_mmap_work, not arr, so we can freely
     * overwrite arr as we emit output. */
    for (i = 0; i < n; i++) s_mmap_work[i] = arr[i];

    for (i = 0; i < n; i++) {
        if (s_mmap_work[i].length == 0) continue;
        s_mmap_events[ev_count].addr     = s_mmap_work[i].base_addr;
        s_mmap_events[ev_count].desc_idx = i;
        s_mmap_events[ev_count].is_start = 1;
        ev_count++;
        s_mmap_events[ev_count].addr     =
            s_mmap_work[i].base_addr + s_mmap_work[i].length;
        s_mmap_events[ev_count].desc_idx = i;
        s_mmap_events[ev_count].is_start = 0;
        ev_count++;
    }

    if (ev_count < 2) { *count = 0; return; }

    mmap_event_sort(s_mmap_events, ev_count);

    for (i = 0; i < ev_count; i++) {
        UINT64 curr_addr = s_mmap_events[i].addr;

        if (prev_valid && curr_addr > prev_addr && active_count > 0) {
            UINT32 winner = mmap_active_find_winner(active_count);
            UINT32 win_type   = s_mmap_work[winner].uefi_memory_type;
            UINT32 win_simple = s_mmap_work[winner].type;
            UINT64 win_attr   = s_mmap_work[winner].attribute;

            if (active_count > 1) {
                UINT32 loser_type =
                    mmap_active_first_loser_type(active_count, winner);
                if (loser_type != win_type)
                    mmap_log_resolved(prev_addr, win_type, loser_type);
            }

            if (!mmap_emit_segment(arr, &out_count, max_entries,
                                    prev_addr, curr_addr, win_type,
                                    win_simple, win_attr)) {
                g_boot_info_ptr->mmap_quirks = 1;
            }
        }

        if (s_mmap_events[i].is_start) {
            if (active_count < MMAP_NORMALIZE_MAX)
                s_mmap_active[active_count++] = s_mmap_events[i].desc_idx;
        } else {
            active_count = mmap_active_remove(active_count,
                                               s_mmap_events[i].desc_idx);
        }

        prev_addr = curr_addr;
        prev_valid = 1;
    }

    *count = out_count;
}

/* When the UEFI descriptor count exceeds BOOT_MMAP_MAX_ENTRIES, preserve
 * high-priority descriptors by evicting a lower-priority already-
 * accepted entry to make room for the incoming one.
 *
 * Eviction policy (Codex S17 post-implementation review rounds 1 and
 * 2, 2026-04-12): cap handling in Phase 1 cannot use the sweep-line
 * carver to preserve non-overlapping fragments -- normalization runs
 * on the already-pruned set.  To avoid pathologically discarding a
 * large RAM block for a small non-overlapping high-priority entry,
 * or a small OVERLAPPING entry that only contests a tiny slice of a
 * large RAM block, rank every lower-priority candidate by the amount
 * of memory that would be lost if it were evicted whole:
 *
 *   loss = victim.length - overlap_length(victim, incoming)
 *
 * Pick the minimum loss.  Ties break to the first-seen candidate
 * (deterministic).  A fully contained victim yields loss == 0 and is
 * always preferred.  If no lower-priority candidate exists, drop the
 * incoming rather than clobber a peer or higher-priority entry.
 *
 * Returns 1 if the incoming descriptor should be inserted at `out_idx`,
 * 0 if the incoming descriptor should be skipped.  Updates *count on
 * successful eviction. */
static int mmap_evict_for_incoming(struct boot_mmap_entry *arr, UINT32 *count,
                                    UINT32 cap, int incoming_prio,
                                    UINT64 incoming_start, UINT64 incoming_end,
                                    UINT32 *out_idx)
{
    UINT32 j;
    int best_idx = -1;
    UINT64 best_loss = 0;

    if (*count < cap) {
        *out_idx = *count;
        return 1;
    }

    for (j = 0; j < *count; j++) {
        UINT64 e_start, e_end, ov_start, ov_end, ov_len, loss;
        int p = mmap_type_priority(arr[j].uefi_memory_type);
        if (p >= incoming_prio) continue;  /* not evictable */

        e_start = arr[j].base_addr;
        e_end   = e_start + arr[j].length;

        /* Compute intersection of [incoming_start, incoming_end) and
         * [e_start, e_end), clamped to zero if disjoint. */
        ov_start = (incoming_start > e_start) ? incoming_start : e_start;
        ov_end   = (incoming_end   < e_end)   ? incoming_end   : e_end;
        ov_len   = (ov_start < ov_end) ? (ov_end - ov_start) : 0;

        /* Memory lost if this victim is evicted whole (non-contested
         * portion of the victim). */
        loss = arr[j].length - ov_len;

        if (best_idx < 0 || loss < best_loss) {
            best_idx  = (int)j;
            best_loss = loss;
        }
    }

    if (best_idx < 0)
        return 0;  /* no evictable victim -- drop incoming */

    mmap_remove_at(arr, count, (UINT32)best_idx);
    g_boot_info_ptr->mmap_quirks = 1;
    serial_early_print("[WARN] mmap: cap reached -- evicted lower-priority entry (min-loss)\n");
    *out_idx = *count;
    return 1;
}

static void fill_memory_map(EFI_MEMORY_DESCRIPTOR *mmap,
                             UINTN map_size, UINTN desc_size)
{
    UINTN offset;
    UINT32 idx = 0;
    UINT64 total_mem = 0;
    UINT32 total_descs;

    /* Guard against malformed descriptor geometry (S12) */
    if (desc_size == 0 || desc_size < sizeof(EFI_MEMORY_DESCRIPTOR)) {
        serial_early_print("[FAIL] Memory map: invalid descriptor size\n");
        g_boot_info_ptr->mmap_count = 0;
        return;
    }

    total_descs = (UINT32)(map_size / desc_size);

    if (total_descs > BOOT_MMAP_MAX_ENTRIES) {
        serial_early_print("[WARN] Memory map has ");
        serial_early_print_uint(total_descs);
        serial_early_print(" entries, capped at ");
        serial_early_print_uint(BOOT_MMAP_MAX_ENTRIES);
        serial_early_print(" (priority eviction active)\n");
        g_boot_info_ptr->mmap_truncated = 1;
    } else {
        g_boot_info_ptr->mmap_truncated = 0;
    }

    g_boot_info_ptr->mmap_quirks = 0;

    /* Phase 1: validate and copy raw descriptors into boot_info->mmap.
     * Overlap resolution is deferred to mmap_normalize() below so the
     * fill loop does not silently drop ACPI Reclaim / BootServices
     * overlaps that firmware can legitimately emit.  If the UEFI map
     * overflows BOOT_MMAP_MAX_ENTRIES, evict the lowest-priority
     * already-accepted entry to keep higher-priority ranges. */
    for (offset = 0; offset < map_size; offset += desc_size) {
        EFI_MEMORY_DESCRIPTOR *desc =
            (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)mmap + offset);
        UINT32 entry_num = (UINT32)(offset / desc_size);
        UINT32 dest_idx;
        int incoming_prio;

        /* ---- Descriptor validation (S12) ---- */

        {
            UINT64 max_pages = 0xFFFFFFFFFFFFFULL;  /* UINT64_MAX / 4096 */
            if (desc->NumberOfPages > max_pages) {
                serial_early_print("[WARN] Memory map entry ");
                serial_early_print_uint(entry_num);
                serial_early_print(": NumberOfPages overflow -- skipping\n");
                g_boot_info_ptr->mmap_quirks = 1;
                continue;
            }
            UINT64 len = desc->NumberOfPages * EFI_PAGE_SIZE;
            if (desc->PhysicalStart > 0xFFFFFFFFFFFFFFFFULL - len) {
                serial_early_print("[WARN] Memory map entry ");
                serial_early_print_uint(entry_num);
                serial_early_print(": address range wraps -- skipping\n");
                g_boot_info_ptr->mmap_quirks = 1;
                continue;
            }
        }

        if (desc->NumberOfPages == 0) {
            serial_early_print("[WARN] Memory map entry ");
            serial_early_print_uint(entry_num);
            serial_early_print(": zero pages -- skipping\n");
            g_boot_info_ptr->mmap_quirks = 1;
            continue;
        }

        if (desc->PhysicalStart & 0xFFF) {
            serial_early_print("[WARN] Memory map entry ");
            serial_early_print_uint(entry_num);
            serial_early_print(": unaligned PhysicalStart -- skipping\n");
            g_boot_info_ptr->mmap_quirks = 1;
            continue;
        }

        if (desc->Type >= EfiMaxMemoryType) {
            serial_early_print("[WARN] Memory map entry ");
            serial_early_print_uint(entry_num);
            serial_early_print(": invalid type 0x");
            serial_early_print_hex16((UINT16)desc->Type);
            serial_early_print(" -- skipping\n");
            g_boot_info_ptr->mmap_quirks = 1;
            continue;
        }

        /* Priority-aware cap handling: at capacity, evict a lower-
         * priority existing entry to make room for the incoming one.
         * Eviction prefers an overlapping victim and falls back to the
         * smallest lower-priority entry so a small Reserved descriptor
         * cannot pathologically discard a large Conventional RAM
         * region that does not overlap it.  At or below cap, append
         * at idx. */
        {
            UINT64 desc_len = desc->NumberOfPages * EFI_PAGE_SIZE;
            UINT64 desc_start = desc->PhysicalStart;
            UINT64 desc_end = desc_start + desc_len;
            incoming_prio = mmap_type_priority(desc->Type);
            if (!mmap_evict_for_incoming(g_boot_info_ptr->mmap, &idx,
                                          BOOT_MMAP_MAX_ENTRIES,
                                          incoming_prio,
                                          desc_start, desc_end,
                                          &dest_idx)) {
                g_boot_info_ptr->mmap_quirks = 1;
                continue;  /* no evictable victim -- drop incoming */
            }
        }

        g_boot_info_ptr->mmap[dest_idx].base_addr = desc->PhysicalStart;
        g_boot_info_ptr->mmap[dest_idx].length =
            desc->NumberOfPages * EFI_PAGE_SIZE;
        g_boot_info_ptr->mmap[dest_idx].type =
            uefi_to_mb2_memtype(desc->Type);
        g_boot_info_ptr->mmap[dest_idx].uefi_memory_type = desc->Type;
        g_boot_info_ptr->mmap[dest_idx].attribute = desc->Attribute;
        idx++;
    }

    /* Phase 2: sort by base address and resolve overlaps by priority. */
    mmap_normalize(g_boot_info_ptr->mmap, &idx, BOOT_MMAP_MAX_ENTRIES);

    /* Phase 3: recompute total_mem from the NORMALIZED map so
     * mem_upper_kb reflects any carving.  A Conventional range that
     * was shrunk or split by higher-priority overlays must not be
     * counted at its pre-normalize length. */
    {
        UINT32 k;
        for (k = 0; k < idx; k++) {
            switch (g_boot_info_ptr->mmap[k].uefi_memory_type) {
            case EfiConventionalMemory:
            case EfiBootServicesCode:
            case EfiBootServicesData:
            case EfiLoaderCode:
            case EfiLoaderData:
                total_mem += g_boot_info_ptr->mmap[k].length;
                break;
            default:
                break;
            }
        }
    }

    g_boot_info_ptr->mmap_count = idx;
    g_boot_info_ptr->mem_lower_kb = 640;   /* conventional: 640 KiB */
    if (total_mem >= (1024ULL * 1024ULL)) {
        g_boot_info_ptr->mem_upper_kb =
            (UINT32)((total_mem / 1024ULL) - 1024ULL);
    } else {
        g_boot_info_ptr->mem_upper_kb = 0;
    }
}

/* ============================================================================
 * Step 4b: Extract runtime memory regions for SetVirtualAddressMap
 *
 * Scans the UEFI memory map for EfiRuntimeServicesCode and
 * EfiRuntimeServicesData regions.  These survive ExitBootServices and must
 * be passed to SetVirtualAddressMap() so the firmware can relocate its
 * internal pointers to match the kernel's virtual address layout.
 * ============================================================================ */
static void fill_runtime_map(EFI_MEMORY_DESCRIPTOR *mmap,
                              UINTN map_size, UINTN desc_size)
{
    UINTN offset;
    UINT32 idx = 0;

    for (offset = 0; offset < map_size && idx < BOOT_RT_MMAP_MAX;
         offset += desc_size) {
        EFI_MEMORY_DESCRIPTOR *desc =
            (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)mmap + offset);

        if (desc->Type == EfiRuntimeServicesCode ||
            desc->Type == EfiRuntimeServicesData) {
            g_boot_info_ptr->rt_mmap[idx].phys_addr  = desc->PhysicalStart;
            g_boot_info_ptr->rt_mmap[idx].num_pages  = desc->NumberOfPages;
            g_boot_info_ptr->rt_mmap[idx].attribute  = desc->Attribute;
            g_boot_info_ptr->rt_mmap[idx].type       = desc->Type;
            g_boot_info_ptr->rt_mmap[idx].reserved   = 0;
            idx++;
        }
    }

    g_boot_info_ptr->rt_mmap_count = idx;
}

/* ============================================================================
 * Step 4c: Retrieve TPM Event Log (if available)
 *
 * Uses EFI_TCG2_PROTOCOL to detect the TPM and retrieve the measured boot
 * event log.  This must happen before ExitBootServices() because the
 * protocol is a boot service.  We copy the event log to a separate buffer
 * since firmware may reclaim the original memory after ExitBootServices.
 * ============================================================================ */
#define TPM_EVENT_LOG_MAX (32 * 1024)  /* 32 KiB max event log */

static void retrieve_tpm_event_log(void)
{
    EFI_STATUS status;
    EFI_GUID tcg2_guid = EFI_TCG2_PROTOCOL_GUID;
    EFI_TCG2_PROTOCOL *tcg2 = (EFI_TCG2_PROTOCOL *)0;

    /* Try to locate the TCG2 protocol */
    status = gBS->LocateProtocol(&tcg2_guid, (VOID *)0, (VOID **)&tcg2);
    if (EFI_ERROR(status) || !tcg2) {
        /* No TPM -- not an error, just unavailable */
        serial_early_print("[BOOT] TPM: not available\n");
        g_boot_info_ptr->tpm_available = 0;
        return;
    }

    /* Get TPM capabilities */
    EFI_TCG2_BOOT_SERVICE_CAPABILITY caps;
    caps.Size = (UINT8)sizeof(caps);

    status = tcg2->GetCapability(tcg2, &caps);
    if (EFI_ERROR(status)) {
        serial_early_print("[BOOT] TPM: GetCapability failed\n");
        g_boot_info_ptr->tpm_available = 0;
        return;
    }

    if (!caps.TPMPresentFlag) {
        serial_early_print("[BOOT] TPM: device not present\n");
        g_boot_info_ptr->tpm_available = 0;
        return;
    }

    /* Determine TPM version from supported event log formats */
    UINT8 tpm_ver = 1;  /* default: TPM 1.2 */
    UINT32 log_format = EFI_TCG2_EVENT_LOG_FORMAT_TCG_1_2;
    if (caps.SupportedEventLogs & EFI_TCG2_EVENT_LOG_FORMAT_TCG_2) {
        tpm_ver = 2;  /* TPM 2.0 with crypto-agile log */
        log_format = EFI_TCG2_EVENT_LOG_FORMAT_TCG_2;
    }

    /* Get event log */
    EFI_PHYSICAL_ADDRESS log_location = 0;
    EFI_PHYSICAL_ADDRESS log_last_entry = 0;
    BOOLEAN log_truncated = 0;

    status = tcg2->GetEventLog(tcg2, log_format,
                                &log_location, &log_last_entry,
                                &log_truncated);
    if (EFI_ERROR(status) || log_location == 0) {
        serial_early_print("[BOOT] TPM: GetEventLog failed\n");
        g_boot_info_ptr->tpm_available = 0;
        return;
    }

    /* Calculate event log size.
     * The last entry pointer points to the start of the last event.
     * We estimate size as (last_entry - log_location + 256) since we
     * don't know the exact size of the last event without parsing.
     * Cap at TPM_EVENT_LOG_MAX. */
    UINT64 log_size;
    if (log_last_entry > log_location) {
        log_size = (log_last_entry - log_location) + 256;
    } else {
        log_size = 4096;  /* fallback: single page */
    }
    if (log_size > TPM_EVENT_LOG_MAX)
        log_size = TPM_EVENT_LOG_MAX;

    /* Allocate buffer and copy event log (firmware may reclaim original) */
    VOID *log_copy = (VOID *)0;
    status = gBS->AllocatePool(EfiLoaderData, (UINTN)log_size, &log_copy);
    if (EFI_ERROR(status) || !log_copy) {
        serial_early_print("[BOOT] TPM: failed to allocate log buffer\n");
        g_boot_info_ptr->tpm_available = 0;
        return;
    }

    /* Copy event log data */
    UINT8 *dst = (UINT8 *)log_copy;
    UINT8 *src = (UINT8 *)(UINTN)log_location;
    UINTN i;
    for (i = 0; i < (UINTN)log_size; i++)
        dst[i] = src[i];

    /* Count events by walking the TCG_PCR_EVENT header (first entry is
     * always a SHA-1 spec ID event in the TCG 1.2 format, even for
     * crypto-agile logs).  For a simple count, scan for 4-byte aligned
     * entries.  This is approximate -- the kernel will do full parsing. */
    UINT16 event_count = 0;
    UINTN offset = 0;
    while (offset + 32 < (UINTN)log_size) {
        /* Each TCG_PCR_EVENT starts with: uint32 pcr_index, uint32 event_type,
         * 20-byte SHA-1 digest, uint32 event_data_size, then event_data[] */
        UINT32 event_data_size = *(UINT32 *)(dst + offset + 28);
        UINTN entry_size = 32 + event_data_size;
        if (entry_size < 32 || offset + entry_size > (UINTN)log_size)
            break;
        event_count++;
        if (event_count == 1 && tpm_ver == 2) {
            /* First entry is spec ID event -- remaining entries use
             * TCG_PCR_EVENT2 format. We can't easily count those without
             * knowing the hash sizes, so break after the first. The kernel
             * will do proper parsing. */
            break;
        }
        offset += entry_size;
    }

    /* Store in boot_info */
    g_boot_info_ptr->tpm_event_log      = (UINT64)(UINTN)log_copy;
    g_boot_info_ptr->tpm_event_log_size = (UINT32)log_size;
    g_boot_info_ptr->tpm_available      = 1;
    g_boot_info_ptr->tpm_version        = tpm_ver;
    g_boot_info_ptr->tpm_event_count    = event_count;

    serial_early_print("[BOOT] TPM: event log retrieved\n");
}

/* ============================================================================
 * Step 4d: Parse Firmware Performance Data Table (FPDT)
 *
 * The FPDT provides precise timestamps for firmware boot phases.
 * It's found in the UEFI Configuration Table.  The table contains
 * performance record pointers; we follow the Firmware Basic Boot
 * Performance Pointer to get the FBPT (Firmware Basic Boot
 * Performance Table), which has ResetEnd, OSLoaderLoad, etc.
 * ============================================================================ */

/* FPDT record types */
#define FPDT_RECORD_TYPE_FIRMWARE_BASIC_BOOT  0x0000
#define FPDT_RECORD_TYPE_S3_PERF              0x0001

/* FPDT header -- at the config table address */
struct fpdt_header {
    UINT32 signature;       /* 'FPDT' */
    UINT32 length;
    UINT8  revision;
    UINT8  checksum;
    UINT8  oem_id[6];
    UINT8  oem_table_id[8];
    UINT32 oem_revision;
    UINT32 creator_id;
    UINT32 creator_revision;
};

/* FPDT performance record header */
struct fpdt_perf_record_hdr {
    UINT16 type;
    UINT8  length;
    UINT8  revision;
};

/* Firmware Basic Boot Performance Pointer Record */
struct fpdt_boot_perf_ptr {
    UINT16 type;            /* 0x0000 */
    UINT8  length;
    UINT8  revision;
    UINT32 reserved;
    UINT64 fbpt_address;    /* physical address of FBPT */
};

/* Firmware Basic Boot Performance Table (FBPT) entry */
struct fbpt_record {
    UINT16 type;            /* 0x0002 = basic boot */
    UINT8  length;
    UINT8  revision;
    UINT32 reserved;
    UINT64 reset_end;                /* SEC phase complete (ns since reset) */
    UINT64 os_loader_load_start;     /* bootloader load began (ns) */
    UINT64 os_loader_start_start;    /* bootloader started executing (ns) */
    UINT64 exit_bs_entry;            /* ExitBootServices called (ns) */
    UINT64 exit_bs_exit;             /* ExitBootServices returned (ns) */
};

static void parse_fpdt(void)
{
    g_boot_info_ptr->timing.fpdt_available = 0;

    /* FPDT can be in UEFI config table or ACPI table.
     * Try config table first (via the GUID the kernel already copied). */
    EFI_GUID fpdt_guid = { 0x564b1aaa, 0xafe3, 0x4b6c,
        { 0x83, 0xa9, 0x27, 0x00, 0x80, 0x50, 0x01, 0x00 } };

    /* Search config table */
    UINTN i;
    UINT64 fpdt_addr = 0;
    for (i = 0; i < gST->NumberOfTableEntries; i++) {
        EFI_GUID *tg = &gST->ConfigurationTable[i].VendorGuid;
        if (tg->Data1 == fpdt_guid.Data1 &&
            tg->Data2 == fpdt_guid.Data2 &&
            tg->Data3 == fpdt_guid.Data3) {
            fpdt_addr = (UINT64)(UINTN)gST->ConfigurationTable[i].VendorTable;
            break;
        }
    }

    if (fpdt_addr == 0) {
        serial_early_print("[BOOT] FPDT: not found\n");
        return;
    }

    /* Walk FPDT records to find the Firmware Basic Boot Perf Pointer */
    const struct fpdt_header *hdr = (const struct fpdt_header *)(UINTN)fpdt_addr;
    UINT32 table_len = hdr->length;
    UINTN offset = sizeof(struct fpdt_header);

    while (offset + sizeof(struct fpdt_perf_record_hdr) < table_len) {
        const struct fpdt_boot_perf_ptr *rec =
            (const struct fpdt_boot_perf_ptr *)((UINTN)fpdt_addr + offset);

        if (rec->type == FPDT_RECORD_TYPE_FIRMWARE_BASIC_BOOT &&
            rec->fbpt_address != 0) {
            /* Follow the pointer to the FBPT */
            const struct fbpt_record *fbpt =
                (const struct fbpt_record *)(UINTN)rec->fbpt_address;

            /* The FBPT starts with an ACPI table header (36 bytes),
             * followed by performance records.  The first record
             * at offset 36 is the basic boot record. */
            const struct fbpt_record *boot_rec =
                (const struct fbpt_record *)((UINTN)rec->fbpt_address + 36);
            (void)fbpt;

            if (boot_rec->type == 0x0002) {
                g_boot_info_ptr->timing.reset_end = boot_rec->reset_end;
                g_boot_info_ptr->timing.os_loader_load_start =
                    boot_rec->os_loader_load_start;
                g_boot_info_ptr->timing.os_loader_start_start =
                    boot_rec->os_loader_start_start;
                g_boot_info_ptr->timing.exit_bs_entry = boot_rec->exit_bs_entry;
                g_boot_info_ptr->timing.exit_bs_exit = boot_rec->exit_bs_exit;
                g_boot_info_ptr->timing.fpdt_available = 1;
                serial_early_print("[BOOT] FPDT: firmware boot record found\n");
                return;
            }
        }

        if (rec->length < 4) break;
        offset += rec->length;
    }

    serial_early_print("[BOOT] FPDT: no basic boot record\n");
}

/* ============================================================================
 * Step 5: Copy UEFI Configuration Table + find ACPI RSDP
 *
 * The EFI_SYSTEM_TABLE.ConfigurationTable[] array contains {GUID, Pointer}
 * pairs for ACPI, SMBIOS, Memory Attributes, and other firmware tables.
 * We copy all entries into boot_info so the kernel can search by GUID.
 * We also extract the ACPI RSDP here (backward compatibility).
 * ============================================================================ */
static void copy_config_tables(void)
{
    EFI_GUID acpi20_guid = EFI_ACPI_20_TABLE_GUID;
    EFI_GUID acpi10_guid = EFI_ACPI_TABLE_GUID;
    UINTN i;
    UINT32 count = 0;

    for (i = 0; i < gST->NumberOfTableEntries && count < BOOT_CONFIG_TABLE_MAX;
         i++) {
        EFI_CONFIGURATION_TABLE *entry = &gST->ConfigurationTable[i];

        /* Copy GUID */
        const UINT8 *src = (const UINT8 *)&entry->VendorGuid;
        UINT8 *dst = (UINT8 *)&g_boot_info_ptr->config_table[count].guid;
        UINTN j;
        for (j = 0; j < 16; j++)
            dst[j] = src[j];

        /* Copy table pointer */
        g_boot_info_ptr->config_table[count].table_addr =
            (UINT64)(UINTN)entry->VendorTable;
        count++;

        /* Also extract ACPI RSDP for backward compatibility */
        if (guid_equal(&entry->VendorGuid, &acpi20_guid)) {
            g_boot_info_ptr->acpi_rsdp_addr = (UINT64)(UINTN)entry->VendorTable;
            g_boot_info_ptr->acpi_version = 2;
            g_boot_info_ptr->acpi_available = 1;
        } else if (guid_equal(&entry->VendorGuid, &acpi10_guid) &&
                   !g_boot_info_ptr->acpi_available) {
            /* Only use ACPI 1.0 if we haven't found 2.0 yet */
            g_boot_info_ptr->acpi_rsdp_addr = (UINT64)(UINTN)entry->VendorTable;
            g_boot_info_ptr->acpi_version = 1;
            g_boot_info_ptr->acpi_available = 1;
        }
    }

    g_boot_info_ptr->config_table_count = count;
}

/* ============================================================================
 * Step 6: Set up page tables (identity map first 4 GiB with 2 MiB pages)
 * ============================================================================ */

/* Page table physical addresses -- allocate 6 pages at 0x70000 */
#define PT_PML4  0x70000
#define PT_PDPT  0x71000
#define PT_PD0   0x72000  /* 4 PDs: 0x72000, 0x73000, 0x74000, 0x75000 */

static void setup_page_tables(void)
{
    UINT64 *pml4 = (UINT64 *)PT_PML4;
    UINT64 *pdpt = (UINT64 *)PT_PDPT;
    UINT64 *pd;
    UINT32 i, pdi;

    /* Zero all page table memory (6 pages) */
    efi_memset((void *)PT_PML4, 0, 6 * 4096);

    /* PML4[0] → PDPT */
    pml4[0] = PT_PDPT | 0x07;  /* Present + Writable + User */

    /* PDPT[0..3] → PD[0..3] */
    for (i = 0; i < 4; i++)
        pdpt[i] = (PT_PD0 + i * 4096) | 0x07;

    /* Fill PDs with 2 MiB identity-mapped pages */
    for (pdi = 0; pdi < 4; pdi++) {
        pd = (UINT64 *)(UINTN)(PT_PD0 + pdi * 4096);
        for (i = 0; i < 512; i++) {
            UINT64 addr = ((UINT64)pdi * 512 + i) * 0x200000;
            pd[i] = addr | 0x87;  /* Present + Writable + User + PageSize(2MiB) */
        }
    }
}

/* ============================================================================
 * Step 7: Jump to kernel -- switch to our page tables and call entry
 * ============================================================================ */

/* Defined in entry64.asm */
typedef void (*kernel_entry_fn)(UINT64 magic, UINT64 boot_info_addr);

static void jump_to_kernel(UINT64 entry_point)
{
    kernel_entry_fn entry = (kernel_entry_fn)entry_point;

    /* Load our page tables into CR3 */
    __asm__ volatile (
        "movq %0, %%cr3"
        :
        : "r"((UINT64)PT_PML4)
        : "memory"
    );

    /* Call kernel -- pass Multiboot2 magic + boot_info address.
     * We pass the UEFI-specific magic 0x55454649 ("UEFI") so the kernel
     * can detect which bootloader was used. */
    entry(0x55454649ULL, (UINT64)(UINTN)g_boot_info_ptr);

    /* Should never return */
    for (;;) __asm__ volatile("hlt");
}

/* ============================================================================
 * Pre-ExitBootServices USB Device Discovery (Phase A)
 *
 * Uses EFI_USB_IO_PROTOCOL to enumerate all USB devices while firmware is
 * active.  For each device, reads the device descriptor (VID, PID, class),
 * interface descriptor (class, subclass, protocol, endpoints), and for MSC
 * devices queries disk geometry via EFI_BLOCK_IO_PROTOCOL.  All info is
 * stored in boot_info.usb_devices[] for the kernel to inherit.
 * ============================================================================ */

static void discover_usb_devices(void)
{
    EFI_GUID usb_io_guid   = EFI_USB_IO_PROTOCOL_GUID;
    EFI_GUID block_io_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
    EFI_STATUS status;
    UINTN handle_count = 0;
    EFI_HANDLE *handle_buf = (EFI_HANDLE *)0;
    UINTN i;
    UINT32 dev_idx = 0;

    g_boot_info_ptr->usb_device_count = 0;
    g_boot_info_ptr->usb_discovery_ok = 0;

    /* Find all handles that expose EFI_USB_IO_PROTOCOL */
    status = gBS->LocateHandleBuffer(ByProtocol, &usb_io_guid,
                                     (VOID *)0, &handle_count, &handle_buf);
    if (EFI_ERROR(status) || handle_count == 0) {
        serial_early_print("[BOOT] USB: no USB devices found (");
        serial_early_print_uint((UINT32)(status & 0xFFFFFFFF));
        serial_early_print(")\n");
        g_boot_info_ptr->usb_discovery_ok = 1;  /* success -- just no devices */
        return;
    }

    serial_early_print("[BOOT] USB: found ");
    serial_early_print_uint((UINT32)handle_count);
    serial_early_print(" USB device handle(s)\n");

    for (i = 0; i < handle_count && dev_idx < BOOT_USB_MAX_DEVICES; i++) {
        EFI_USB_IO_PROTOCOL *usb_io = (EFI_USB_IO_PROTOCOL *)0;
        EFI_USB_DEVICE_DESCRIPTOR dev_desc;
        EFI_USB_INTERFACE_DESCRIPTOR iface_desc;
        struct boot_usb_device *bdev;

        /* Open USB I/O protocol on this handle */
        status = gBS->HandleProtocol(handle_buf[i], &usb_io_guid,
                                     (VOID **)&usb_io);
        if (EFI_ERROR(status) || !usb_io)
            continue;

        /* Read device descriptor */
        status = usb_io->UsbGetDeviceDescriptor(usb_io, &dev_desc);
        if (EFI_ERROR(status))
            continue;

        /* Read interface descriptor */
        status = usb_io->UsbGetInterfaceDescriptor(usb_io, &iface_desc);
        if (EFI_ERROR(status))
            continue;

        /* Populate boot_info entry */
        bdev = &g_boot_info_ptr->usb_devices[dev_idx];
        efi_memset(bdev, 0, sizeof(*bdev));

        bdev->active          = 1;
        bdev->port            = 0;  /* port info not available via USB_IO */
        bdev->speed           = 0;  /* speed not exposed by USB_IO */
        bdev->device_class    = dev_desc.DeviceClass;
        bdev->vendor_id       = dev_desc.IdVendor;
        bdev->product_id      = dev_desc.IdProduct;
        bdev->iface_class     = iface_desc.InterfaceClass;
        bdev->iface_subclass  = iface_desc.InterfaceSubClass;
        bdev->iface_protocol  = iface_desc.InterfaceProtocol;

        /* Collect endpoint descriptors */
        {
            UINT8 ep_idx;
            UINT8 ep_count = 0;
            for (ep_idx = 0; ep_idx < iface_desc.NumEndpoints &&
                             ep_count < BOOT_USB_MAX_ENDPOINTS; ep_idx++) {
                EFI_USB_ENDPOINT_DESCRIPTOR ep_desc;
                status = usb_io->UsbGetEndpointDescriptor(usb_io, ep_idx,
                                                          &ep_desc);
                if (EFI_ERROR(status))
                    continue;
                bdev->endpoints[ep_count].address    = ep_desc.EndpointAddress;
                bdev->endpoints[ep_count].attributes = ep_desc.Attributes;
                bdev->endpoints[ep_count].max_packet = ep_desc.MaxPacketSize;
                bdev->endpoints[ep_count].interval   = ep_desc.Interval;
                ep_count++;
            }
            bdev->num_endpoints = ep_count;
        }

        /* Identify MSC BOT device (class 0x08, subclass 0x06, protocol 0x50) */
        if (iface_desc.InterfaceClass    == 0x08 &&
            iface_desc.InterfaceSubClass == 0x06 &&
            iface_desc.InterfaceProtocol == 0x50) {
            bdev->is_msc = 1;

            /* Try to get disk geometry from EFI_BLOCK_IO_PROTOCOL */
            {
                EFI_BLOCK_IO_PROTOCOL *block_io = (EFI_BLOCK_IO_PROTOCOL *)0;
                status = gBS->HandleProtocol(handle_buf[i], &block_io_guid,
                                             (VOID **)&block_io);
                if (!EFI_ERROR(status) && block_io && block_io->Media) {
                    bdev->block_size  = block_io->Media->BlockSize;
                    bdev->block_count = block_io->Media->LastBlock + 1;
                    serial_early_print("[BOOT] USB MSC: ");
                    serial_early_print_uint(bdev->block_size);
                    serial_early_print("B x ");
                    serial_early_print_uint((UINT32)(bdev->block_count & 0xFFFFFFFF));
                    serial_early_print(" sectors\n");
                }
            }
        }

        /* Identify HID device (class 0x03) */
        if (iface_desc.InterfaceClass == 0x03)
            bdev->is_hid = 1;

        /* Log device */
        serial_early_print("[BOOT] USB dev ");
        serial_early_print_uint(dev_idx);
        serial_early_print(": VID=");
        serial_early_print_hex16(dev_desc.IdVendor);
        serial_early_print(" PID=");
        serial_early_print_hex16(dev_desc.IdProduct);
        serial_early_print(" class=");
        serial_early_print_uint(iface_desc.InterfaceClass);
        serial_early_print("/");
        serial_early_print_uint(iface_desc.InterfaceSubClass);
        serial_early_print("/");
        serial_early_print_uint(iface_desc.InterfaceProtocol);
        if (bdev->is_msc)
            serial_early_print(" [MSC]");
        if (bdev->is_hid)
            serial_early_print(" [HID]");
        serial_early_print("\n");

        dev_idx++;
    }

    g_boot_info_ptr->usb_device_count = dev_idx;
    g_boot_info_ptr->usb_discovery_ok = 1;

    /* Free the handle buffer */
    gBS->FreePool((VOID *)handle_buf);

    serial_early_print("[BOOT] USB discovery: ");
    serial_early_print_uint(dev_idx);
    serial_early_print(" device(s) recorded\n");
}

/* ============================================================================
 * Allocate persistent xHCI DMA structures
 *
 * Find the xHCI controller via PCI scan, read capability registers to
 * determine structure sizes, and allocate DCBAA, scratchpad buffers,
 * command ring, event ring, and ERST using EfiLoaderData pages.
 * These pages survive ExitBootServices; kernel must reserve them in PMM.
 * ============================================================================ */

/* Helper: allocate one 4 KiB page as EfiLoaderData and track it */
static UINT64 bl_alloc_dma_page(struct boot_usb_controller *ctrl)
{
    EFI_PHYSICAL_ADDRESS addr = 0;
    EFI_STATUS status;
    UINTN i;

    /* Use AllocatePages for page-aligned DMA memory */
    status = gBS->AllocatePages(AllocateAnyPages, EfiLoaderData, 1, &addr);
    if (EFI_ERROR(status) || addr == 0) {
        ctrl->alloc_fail_status = (UINT32)(status & 0xFFFFFFFF);
        ctrl->alloc_fail_page   = ctrl->dma_page_count;
        serial_early_print("[BOOT] xHCI DMA: AllocatePages failed status=");
        serial_early_print_uint((UINT32)(status & 0xFFFF));
        serial_early_print(" page#=");
        serial_early_print_uint(ctrl->dma_page_count);
        serial_early_print("\n");
        return 0;
    }

    /* Zero the page using gBS->SetMem if available, otherwise byte-by-byte.
     * Some firmware have memory protection that makes efi_memset fail on
     * newly allocated pages. */
    {
        volatile UINT8 *p = (volatile UINT8 *)(UINTN)addr;
        for (i = 0; i < 4096; i++)
            p[i] = 0;
    }

    /* Track for kernel PMM reservation */
    if (ctrl->dma_page_count < BOOT_USB_MAX_DMA_PAGES)
        ctrl->dma_pages[ctrl->dma_page_count++] = addr;

    serial_early_print("[BOOT] xHCI DMA: page ");
    serial_early_print_uint(ctrl->dma_page_count);
    serial_early_print(" @ 0x");
    serial_early_print_hex16((UINT16)(addr >> 16));
    serial_early_print_hex16((UINT16)addr);
    serial_early_print("\n");

    return addr;
}

/* MMIO read helpers (bootloader identity-maps all memory) */
static inline UINT32 bl_mmio_read32(UINT64 base, UINT32 off)
{
    return *(volatile UINT32 *)((UINTN)base + off);
}

static inline UINT8 bl_mmio_read8(UINT64 base, UINT32 off)
{
    return *(volatile UINT8 *)((UINTN)base + off);
}

static void allocate_xhci_dma(void)
{
    struct boot_usb_controller *ctrl = &g_boot_info_ptr->usb_controller;
    UINT16 bus;
    UINT8 dev, func;
    UINT32 bar0, bar1, hcsparams1, hcsparams2, hccparams1;
    UINT64 mmio;
    UINT32 i;

    efi_memset(ctrl, 0, sizeof(*ctrl));
    g_boot_info_ptr->usb_handover_complete = 0;

    /* ---- Find xHCI controller via PCI scan (class 0x0C/0x03/0x30) ---- */
    for (bus = 0; bus < 256; bus++) {
        for (dev = 0; dev < 32; dev++) {
            for (func = 0; func < 8; func++) {
                UINT16 vid = bl_pci_read16((UINT8)bus, dev, func, 0x00);
                if (vid == 0xFFFF) continue;

                UINT8 cls = bl_pci_read8((UINT8)bus, dev, func, 0x0B);
                UINT8 sub = bl_pci_read8((UINT8)bus, dev, func, 0x0A);
                UINT8 pif = bl_pci_read8((UINT8)bus, dev, func, 0x09);

                if (cls == 0x0C && sub == 0x03 && pif == 0x30)
                    goto found_xhci;
            }
        }
    }
    serial_early_print("[BOOT] xHCI DMA: no controller found\n");
    return;

found_xhci:
    ctrl->pci_bus  = (UINT8)bus;
    ctrl->pci_dev  = dev;
    ctrl->pci_func = func;

    serial_early_print("[BOOT] xHCI DMA: found at PCI ");
    serial_early_print_uint((UINT32)bus);
    serial_early_print(":");
    serial_early_print_uint((UINT32)dev);
    serial_early_print(".");
    serial_early_print_uint((UINT32)func);
    serial_early_print("\n");

    /* ---- Read BAR0/BAR1 to get MMIO base ---- */
    bar0 = bl_pci_read32((UINT8)bus, dev, func, 0x10);
    bar1 = bl_pci_read32((UINT8)bus, dev, func, 0x14);
    if (bar0 & 0x01) {
        serial_early_print("[BOOT] xHCI DMA: BAR0 is I/O -- skipping\n");
        return;
    }
    mmio = (UINT64)(bar0 & 0xFFFFFFF0) | ((UINT64)bar1 << 32);
    if (mmio == 0) {
        serial_early_print("[BOOT] xHCI DMA: BAR0 is zero -- skipping\n");
        return;
    }
    ctrl->mmio_phys = mmio;
    ctrl->mmio_size = 0x10000;  /* 64 KiB min per xHCI spec */

    /* ---- Read capability registers (MMIO is identity-mapped by firmware) ---- */
    ctrl->cap_length  = bl_mmio_read8(mmio, 0x00);
    ctrl->hci_version = (UINT16)(bl_mmio_read32(mmio, 0x00) >> 16);

    hcsparams1 = bl_mmio_read32(mmio, 0x04);
    ctrl->max_slots = hcsparams1 & 0xFF;
    ctrl->max_intrs = (hcsparams1 >> 8) & 0x7FF;
    ctrl->max_ports = (hcsparams1 >> 24) & 0xFF;

    hccparams1 = bl_mmio_read32(mmio, 0x10);
    ctrl->ac64 = (hccparams1 & 1) ? 1 : 0;
    ctrl->csz  = (hccparams1 & 4) ? 1 : 0;

    ctrl->db_offset  = bl_mmio_read32(mmio, 0x14) & ~0x03;
    ctrl->rts_offset = bl_mmio_read32(mmio, 0x18) & ~0x1F;

    hcsparams2 = bl_mmio_read32(mmio, 0x08);
    {
        UINT32 spb_hi = (hcsparams2 >> 27) & 0x1F;
        UINT32 spb_lo = (hcsparams2 >> 21) & 0x1F;
        ctrl->max_scratchpads = (spb_hi << 5) | spb_lo;
    }

    serial_early_print("[BOOT] xHCI DMA: v");
    serial_early_print_uint((ctrl->hci_version >> 8) & 0xFF);
    serial_early_print(".");
    serial_early_print_uint(ctrl->hci_version & 0xFF);
    serial_early_print(", ");
    serial_early_print_uint(ctrl->max_slots);
    serial_early_print(" slots, ");
    serial_early_print_uint(ctrl->max_ports);
    serial_early_print(" ports, ");
    serial_early_print_uint(ctrl->max_scratchpads);
    serial_early_print(" scratchpads, csz=");
    serial_early_print_uint(ctrl->csz);
    serial_early_print("\n");

    /* ---- Allocate DCBAA: (max_slots + 1) × 8 bytes ---- */
    ctrl->dcbaa_phys = bl_alloc_dma_page(ctrl);
    if (!ctrl->dcbaa_phys) {
        serial_early_print("[BOOT] xHCI DMA: DCBAA alloc failed\n");
        return;
    }

    /* ---- Allocate scratchpad buffers ---- */
    if (ctrl->max_scratchpads > 0) {
        EFI_PHYSICAL_ADDRESS sp_array_addr = 0, sp_base_addr = 0;
        EFI_STATUS sp_status;
        UINT32 sp_count = ctrl->max_scratchpads;

        /* Scratchpad array: sp_count × 8 bytes (fits in 1 page for up to 512 entries) */
        sp_status = gBS->AllocatePages(AllocateAnyPages, EfiLoaderData,
                                       (sp_count * 8 + 4095) / 4096, &sp_array_addr);
        if (EFI_ERROR(sp_status) || sp_array_addr == 0) {
            serial_early_print("[BOOT] xHCI DMA: scratchpad array alloc failed\n");
            return;
        }
        efi_memset((void *)(UINTN)sp_array_addr, 0, sp_count * 8);
        ctrl->scratchpad_array_phys = sp_array_addr;
        if (ctrl->dma_page_count < BOOT_USB_MAX_DMA_PAGES)
            ctrl->dma_pages[ctrl->dma_page_count++] = sp_array_addr;

        /* Scratchpad buffer pages: allocate all contiguously */
        sp_status = gBS->AllocatePages(AllocateAnyPages, EfiLoaderData,
                                       sp_count, &sp_base_addr);
        if (EFI_ERROR(sp_status) || sp_base_addr == 0) {
            serial_early_print("[BOOT] xHCI DMA: scratchpad pages alloc failed (");
            serial_early_print_uint(sp_count);
            serial_early_print(" pages)\n");
            ctrl->alloc_fail_status = (UINT32)(sp_status & 0xFFFFFFFF);
            ctrl->alloc_fail_page   = ctrl->dma_page_count;
            return;
        }
        efi_memset((void *)(UINTN)sp_base_addr, 0, (UINTN)sp_count * 4096);
        ctrl->scratchpad_base_phys  = sp_base_addr;
        ctrl->scratchpad_page_count = sp_count;
        if (ctrl->dma_page_count < BOOT_USB_MAX_DMA_PAGES)
            ctrl->dma_pages[ctrl->dma_page_count++] = sp_base_addr;

        /* Fill scratchpad array with individual page addresses */
        {
            volatile UINT64 *arr = (volatile UINT64 *)(UINTN)sp_array_addr;
            for (i = 0; i < sp_count; i++)
                arr[i] = sp_base_addr + (UINT64)i * 4096;
        }

        /* DCBAA[0] = scratchpad array physical address */
        ((UINT64 *)(UINTN)ctrl->dcbaa_phys)[0] = sp_array_addr;

        serial_early_print("[BOOT] xHCI DMA: ");
        serial_early_print_uint(sp_count);
        serial_early_print(" scratchpad pages at 0x");
        serial_early_print_hex16((UINT16)(sp_base_addr >> 16));
        serial_early_print_hex16((UINT16)sp_base_addr);
        serial_early_print("\n");
    }

    /* ---- Allocate Command Ring (256 TRBs × 16B = 4 KiB) ---- */
    ctrl->cmd_ring_phys = bl_alloc_dma_page(ctrl);
    if (!ctrl->cmd_ring_phys) return;

    /* ---- Allocate Event Ring (256 TRBs × 16B = 4 KiB) ---- */
    ctrl->evt_ring_phys = bl_alloc_dma_page(ctrl);
    if (!ctrl->evt_ring_phys) return;

    /* ---- Allocate ERST (1 entry × 16B, 1 page) ---- */
    ctrl->erst_phys = bl_alloc_dma_page(ctrl);
    if (!ctrl->erst_phys) return;

    ctrl->active = 1;

    serial_early_print("[BOOT] xHCI DMA: allocated ");
    serial_early_print_uint(ctrl->dma_page_count);
    serial_early_print(" pages (DCBAA=0x");
    serial_early_print_hex16((UINT16)(ctrl->dcbaa_phys >> 16));
    serial_early_print_hex16((UINT16)(ctrl->dcbaa_phys));
    serial_early_print(", CmdRing=0x");
    serial_early_print_hex16((UINT16)(ctrl->cmd_ring_phys >> 16));
    serial_early_print_hex16((UINT16)(ctrl->cmd_ring_phys));
    serial_early_print(")\n");
}

/* ============================================================================
 * USBLEGSUP handoff + controller takeover
 *
 * Take xHCI ownership from BIOS/firmware, halt the controller, reset it,
 * configure it to use our persistent DMA structures (allocated earlier), and
 * start it. After this, the controller runs with our DCBAA/rings and is ready
 * for device enumeration.
 * ============================================================================ */

static inline void bl_mmio_write32(UINT64 base, UINT32 off, UINT32 val)
{
    *(volatile UINT32 *)((UINTN)base + off) = val;
}

static inline void bl_mmio_write64(UINT64 base, UINT32 off, UINT64 val)
{
    /* Two 32-bit writes -- some xHCI controllers don't support 64-bit MMIO */
    *(volatile UINT32 *)((UINTN)base + off)     = (UINT32)(val & 0xFFFFFFFF);
    *(volatile UINT32 *)((UINTN)base + off + 4) = (UINT32)(val >> 32);
}

/* Busy-wait microseconds (port 0x80 write ≈ 1 µs on x86) */
static void bl_delay_us(UINT32 us)
{
    UINT32 i;
    for (i = 0; i < us; i++)
        __asm__ volatile("outb %%al, $0x80" ::: "memory");
}

static void bl_usblegsup_handoff(UINT64 mmio, UINT32 hccparams1)
{
    UINT32 xecp_off = ((hccparams1 >> 16) & 0xFFFF) << 2;
    UINT32 max_iter = 64;

    if (xecp_off == 0) {
        serial_early_print("[BOOT] xHCI takeover: no extended caps\n");
        return;
    }

    while (xecp_off != 0 && max_iter-- > 0) {
        UINT32 cap = bl_mmio_read32(mmio, xecp_off);
        UINT8  cap_id   = (UINT8)(cap & 0xFF);
        UINT8  next_ptr = (UINT8)((cap >> 8) & 0xFF);

        if (cap_id == 1) {  /* USBLEGSUP */
            if (!(cap & (1 << 16))) {
                serial_early_print("[BOOT] xHCI takeover: BIOS doesn't own controller\n");
                return;
            }

            serial_early_print("[BOOT] xHCI takeover: USBLEGSUP -- requesting ownership\n");
            bl_mmio_write32(mmio, xecp_off, cap | (1 << 24));

            /* Wait up to 1s for BIOS to release */
            {
                UINT32 timeout = 1000;
                while (timeout > 0) {
                    cap = bl_mmio_read32(mmio, xecp_off);
                    if (!(cap & (1 << 16))) {
                        serial_early_print("[BOOT] xHCI takeover: BIOS released\n");
                        bl_mmio_write32(mmio, xecp_off + 4, 0);  /* Clear USBLEGCTLSTS */
                        return;
                    }
                    bl_delay_us(1000);
                    timeout--;
                }
            }

            /* Timeout -- force */
            serial_early_print("[BOOT] xHCI takeover: BIOS timeout -- forcing\n");
            cap |= (1 << 24);
            cap &= ~(1 << 16);
            bl_mmio_write32(mmio, xecp_off, cap);
            bl_mmio_write32(mmio, xecp_off + 4, 0);
            return;
        }

        if (next_ptr == 0) break;
        xecp_off += (UINT32)next_ptr << 2;
    }

    serial_early_print("[BOOT] xHCI takeover: USBLEGSUP not found\n");
}

static void xhci_controller_takeover(void)
{
    struct boot_usb_controller *ctrl = &g_boot_info_ptr->usb_controller;
    UINT64 mmio;
    UINT64 op_base;
    UINT64 rt_base;
    UINT64 ir0;
    UINT32 cmd, sts, timeout;
    UINT32 hccparams1;

    if (!ctrl->active) {
        serial_early_print("[BOOT] xHCI takeover: no controller -- skipping\n");
        return;
    }

    mmio    = ctrl->mmio_phys;
    op_base = mmio + ctrl->cap_length;
    rt_base = mmio + ctrl->rts_offset;
    ir0     = rt_base + 0x20;  /* Interrupter 0 */

    /* ---- Step 1: USBLEGSUP handoff ---- */
    hccparams1 = bl_mmio_read32(mmio, 0x10);
    bl_usblegsup_handoff(mmio, hccparams1);

    /* ---- Step 2: Halt controller (USBCMD.RS=0, wait HCH=1) ---- */
    cmd = bl_mmio_read32(op_base, 0x00);  /* USBCMD */
    cmd &= ~(1 << 0);  /* Clear RS */
    bl_mmio_write32(op_base, 0x00, cmd);

    timeout = 16000;
    while (timeout > 0) {
        sts = bl_mmio_read32(op_base, 0x04);  /* USBSTS */
        if (sts & (1 << 0))  /* HCH */
            break;
        bl_delay_us(100);
        timeout -= 100;
    }
    if (!(bl_mmio_read32(op_base, 0x04) & (1 << 0))) {
        serial_early_print("[BOOT] xHCI takeover: halt timeout -- aborting\n");
        ctrl->active = 0;
        return;
    }
    serial_early_print("[BOOT] xHCI takeover: halted\n");

    /* ---- Step 3: Reset controller (HCRST=1, wait HCRST=0 AND CNR=0) ---- */
    bl_mmio_write32(op_base, 0x00, (1 << 1));  /* USBCMD = HCRST */

    timeout = 100000;
    while (timeout > 0) {
        cmd = bl_mmio_read32(op_base, 0x00);
        sts = bl_mmio_read32(op_base, 0x04);
        if (!(cmd & (1 << 1)) && !(sts & (1 << 11)))
            break;
        bl_delay_us(100);
        timeout -= 100;
    }
    if ((bl_mmio_read32(op_base, 0x00) & (1 << 1)) ||
        (bl_mmio_read32(op_base, 0x04) & (1 << 11))) {
        serial_early_print("[BOOT] xHCI takeover: reset timeout -- aborting\n");
        ctrl->active = 0;
        return;
    }
    serial_early_print("[BOOT] xHCI takeover: reset complete\n");

    /* ---- Step 4: Configure MaxSlotsEn ---- */
    bl_mmio_write32(op_base, 0x38, ctrl->max_slots);  /* CONFIG */

    /* ---- Step 5: Write DCBAAP ---- */
    bl_mmio_write64(op_base, 0x30, ctrl->dcbaa_phys);

    /* ---- Step 6: Set up Command Ring (CRCR) ---- */
    /* Set Link TRB at slot 255 pointing back to start, with toggle cycle */
    {
        volatile UINT32 *link = (volatile UINT32 *)((UINTN)ctrl->cmd_ring_phys + 255 * 16);
        /* parameter = ring base (low 32) */
        link[0] = (UINT32)(ctrl->cmd_ring_phys & 0xFFFFFFFF);
        /* parameter high = ring base (high 32) */
        link[1] = (UINT32)(ctrl->cmd_ring_phys >> 32);
        /* status = 0 */
        link[2] = 0;
        /* control = TRB type 6 (Link) << 10 | Toggle Cycle (bit 1) | Cycle (bit 0) */
        link[3] = (6 << 10) | (1 << 1) | 1;
    }
    bl_mmio_write64(op_base, 0x18, ctrl->cmd_ring_phys | 1);  /* CRCR | cycle=1 */

    /* ---- Step 7: Set up Event Ring (ERST + Interrupter 0) ---- */
    {
        /* ERST entry: ring_base (8B) + ring_size (4B) + reserved (4B) */
        volatile UINT64 *erst = (volatile UINT64 *)(UINTN)ctrl->erst_phys;
        erst[0] = ctrl->evt_ring_phys;         /* ring_base */
        ((volatile UINT32 *)(UINTN)ctrl->erst_phys)[2] = 256; /* ring_size (TRBs) */
        ((volatile UINT32 *)(UINTN)ctrl->erst_phys)[3] = 0;   /* reserved */
    }

    /* Interrupter 0 registers */
    bl_mmio_write32(ir0, 0x08, 1);                       /* ERSTSZ = 1 segment */
    bl_mmio_write64(ir0, 0x18, ctrl->evt_ring_phys);     /* ERDP = ring start */
    bl_mmio_write64(ir0, 0x10, ctrl->erst_phys);         /* ERSTBA (write last per spec) */

    /* ---- Step 8: Enable interrupts + start controller ---- */
    cmd = bl_mmio_read32(op_base, 0x00);
    cmd |= (1 << 0) | (1 << 2);  /* RS + INTE */
    bl_mmio_write32(op_base, 0x00, cmd);

    /* Enable Interrupter 0 */
    bl_mmio_write32(ir0, 0x00, (1 << 1));  /* IMAN.IE = 1 */

    /* Wait for HCH to clear */
    timeout = 16000;
    while (timeout > 0) {
        sts = bl_mmio_read32(op_base, 0x04);
        if (!(sts & (1 << 0)))
            break;
        bl_delay_us(100);
        timeout -= 100;
    }
    if (bl_mmio_read32(op_base, 0x04) & (1 << 0)) {
        serial_early_print("[BOOT] xHCI takeover: start timeout -- aborting\n");
        ctrl->active = 0;
        return;
    }

    serial_early_print("[BOOT] xHCI takeover: controller running with our DMA\n");
    g_boot_info_ptr->usb_handover_complete = 1;
}

/* ============================================================================
 * EFI Application Entry Point
 * ============================================================================ */
/* ---- 16-bit POST code output ----
 * Writes high byte to I/O port 0x80 for hardware POST cards.
 * Also prints to serial for log capture.
 * QEMU silently ignores port 0x80 writes. */
static inline void post_code(UINT8 code)
{
    __asm__ volatile ("outb %0, $0x80" :: "a"(code));
}

static inline void post_code16(UINT16 code)
{
    /* High byte to I/O port 0x80 (hardware POST cards are 8-bit) */
    post_code((UINT8)(code >> 8));
    /* Serial: [BOOT] POST 0xNNNN */
    serial_early_print("[BOOT] POST 0x");
    {
        static const char hex[] = "0123456789ABCDEF";
        serial_early_putchar(hex[(code >> 12) & 0xF]);
        serial_early_putchar(hex[(code >>  8) & 0xF]);
        serial_early_putchar(hex[(code >>  4) & 0xF]);
        serial_early_putchar(hex[ code        & 0xF]);
    }
    serial_early_putchar('\n');
}

/* Legacy 8-bit POST codes (kept for backward compat) */
#define POST_ENTRY              0x01
#define POST_GOP_INIT           0x02
#define POST_KERNEL_OPEN        0x03
#define POST_KERNEL_LOAD        0x04
#define POST_RSDP_FOUND         0x05
#define POST_MEMORY_MAP         0x06
#define POST_EXIT_BOOT_SERVICES 0x07
#define POST_PAGE_TABLES        0x08
#define POST_KERNEL_JUMP        0x09

/* 16-bit POST codes for UEFI bootloader (0xB000 range) */
#define POST16_BL_ENTRY         0xB001
#define POST16_BL_GOP           0xB010
#define POST16_BL_KERNEL_OPEN   0xB020
#define POST16_BL_KERNEL_LOAD   0xB021
#define POST16_BL_RSDP          0xB030
#define POST16_BL_MEMMAP        0xB040
#define POST16_BL_USB_DISC          0xB080
#define POST16_BL_USB_DISC_OK       0xB081
#define POST16_BL_XHCI_DMA          0xB082  /* xHCI DMA alloc (§TODO-20) */
#define POST16_BL_XHCI_DMA_OK       0xB083
#define POST16_BL_XHCI_TAKEOVER     0xB084  /* xHCI controller takeover */
#define POST16_BL_XHCI_TAKEOVER_OK  0xB085
#define POST16_BL_BOOT_DEV 0xB090 /* Boot device identification */
#define POST16_BL_BOOT_DEV_OK       0xB091
/* POST16_BL_BOOT_FS / _OK (0xB092/0xB093) defined near top -- used by parse_boot_conf */
#define POST16_BL_EXIT_BS       0xB050
#define POST16_BL_PAGE_TABLES   0xB060
#define POST16_BL_KERNEL_JUMP   0xB070

EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
    EFI_STATUS status;
    UINT64 kernel_entry;
    UINTN map_key;
    EFI_MEMORY_DESCRIPTOR *mmap;
    UINTN map_size, desc_size;
    UINT32 desc_version;

    /* Save globals */
    gST = SystemTable;
    gBS = SystemTable->BootServices;
    gImageHandle = ImageHandle;

    /* Clear the UEFI text console immediately -- firmware (BdsDxe, QEMU MMIO
     * warnings) may have left text on screen before our image was launched. */
    if (SystemTable->ConOut)
        SystemTable->ConOut->ClearScreen(SystemTable->ConOut);

    /* Initialize early serial for diagnostics (before anything else) */
    serial_early_init();
    boot_log_init();

    /* Log serial detection method (S10: SPCR auto-detection) */
    if (s_serial_source == 1) {
        serial_early_print("[BOOT] Serial: SPCR detected port=0x");
        serial_early_print_hex16(s_serial_port);
        serial_early_print(" baud=");
        serial_early_print_uint(s_serial_baud);
        serial_early_print("\n");
    } else if (s_spcr_skipped) {
        serial_early_print("[BOOT] SPCR: non-standard port at 0x");
        serial_early_print_hex16((UINT16)(s_spcr_skip_addr >> 16));
        serial_early_print_hex16((UINT16)s_spcr_skip_addr);
        serial_early_print(", skipping\n");
        serial_early_print("[BOOT] Serial: SPCR absent, falling back to I/O probe\n");
    } else if (s_serial_port) {
        serial_early_print("[BOOT] Serial: SPCR absent, falling back to I/O probe\n");
    }

    post_code16(POST16_BL_ENTRY);
    serial_early_print("[BOOT] efi_main entered\n");

    /* Re-arm watchdog timer as boot hang safety net (S11).
     * UEFI default is 5 minutes -- too long for debugging.
     * Arm with 60s timeout; disarm before ExitBootServices.
     * Code 0x424F4F54 = "BOOT" in ASCII for firmware logs. */
#define WD_TIMEOUT  60
#define WD_CODE     0x424F4F54  /* "BOOT" */
    g_wd_armed = 0;
    {
        EFI_STATUS wd_s = gBS->SetWatchdogTimer(WD_TIMEOUT, WD_CODE, 0, (CHAR16 *)0);
        if (!EFI_ERROR(wd_s)) {
            serial_early_print("[BOOT] Watchdog: armed (60s)\n");
            g_wd_armed = 1;
        } else {
            serial_early_print("[BOOT] Watchdog: arm failed (unsupported?)\n");
        }
    }

    /* S16: Reserve the full boot_info range with UEFI before any raw
     * write, so later AllocatePages(AllocateAnyPages, ...) calls in
     * load_kernel, allocate_xhci_dma, and scratchpad setup cannot hand
     * back a page overlapping [BOOT_INFO_PHYS_ADDR, +sizeof(struct
     * boot_info)).  Without this, firmware was free to reuse the tail
     * pages of the handoff buffer, silently clobbering config/timing/
     * USB state while the header at offset 0 stayed intact -- the
     * kernel's post-handoff validator would still pass.  Halt early if
     * the range is already in use: the old silent-corruption path is
     * strictly worse than a visible boot_fatal. */
    {
        EFI_PHYSICAL_ADDRESS bi_addr = (EFI_PHYSICAL_ADDRESS)BOOT_INFO_PHYS_ADDR;
        UINTN bi_pages = (sizeof(struct boot_info) + 4095) / 4096;
        EFI_STATUS rs = gBS->AllocatePages(AllocateAddress, EfiLoaderData,
                                           bi_pages, &bi_addr);
        if (EFI_ERROR(rs)) {
            serial_early_print("[FAIL] boot_info reservation at 0x");
            serial_early_print_hex16((UINT16)(BOOT_INFO_PHYS_ADDR >> 16));
            serial_early_print_hex16((UINT16)BOOT_INFO_PHYS_ADDR);
            serial_early_print(" failed -- range already in use by firmware\n");
            boot_fatal(BOOT_ERR_BOOT_INFO_RESERVED,
                       "boot_info range already in use",
                       "Firmware owns the boot_info physical range.");
        }
    }

    /* S13: Read previous boot error from NVRAM before zeroing boot_info.
     * RuntimeServices->GetVariable is available before ExitBootServices. */
    {
        UINT32 prev_error = nvram_read_boot_error();
        if (prev_error != 0) {
            serial_early_print("[BOOT] Previous boot failed: code=0x");
            serial_early_print_hex16((UINT16)prev_error);
            serial_early_print("\n");
        }

        /* Point boot_info to the reserved physical address */
        g_boot_info_ptr = (struct boot_info *)BOOT_INFO_PHYS_ADDR;
        efi_memset(g_boot_info_ptr, 0, sizeof(struct boot_info));

        /* Pass previous error to kernel (S13) */
        g_boot_info_ptr->last_boot_error = prev_error;
    }

    /* Record serial port detection results (S4/S10) */
    g_boot_info_ptr->serial_port = s_serial_port;
    g_boot_info_ptr->serial_baud = s_serial_baud;
    g_boot_info_ptr->serial_source = s_serial_source;

    /* Record bootloader entry time */
    g_boot_info_ptr->timing.bl_entry = boot_rdtsc();

    /* Step 0: Identify boot device via EFI_LOADED_IMAGE_PROTOCOL.
     * Store DeviceHandle globally so parse_boot_conf(), load_kernel(), and
     * all later boot consumers use the same boot device handle. */
    post_code16(POST16_BL_BOOT_DEV);
    {
        EFI_GUID li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
        EFI_LOADED_IMAGE_PROTOCOL *loaded_image = (EFI_LOADED_IMAGE_PROTOCOL *)0;
        EFI_STATUS li_status;

        li_status = gBS->HandleProtocol(gImageHandle, &li_guid,
                                         (VOID **)&loaded_image);
        if (!EFI_ERROR(li_status) && loaded_image && loaded_image->DeviceHandle) {
            g_boot_device_handle = loaded_image->DeviceHandle;
            serial_early_print("[BOOT] Boot device: handle=0x");
            serial_early_print_hex16((UINT16)((UINTN)g_boot_device_handle >> 48));
            serial_early_print_hex16((UINT16)((UINTN)g_boot_device_handle >> 32));
            serial_early_print_hex16((UINT16)((UINTN)g_boot_device_handle >> 16));
            serial_early_print_hex16((UINT16)(UINTN)g_boot_device_handle);
            serial_early_print("\n");
            post_code16(POST16_BL_BOOT_DEV_OK);
        } else {
            g_boot_device_handle = (EFI_HANDLE)0;
            serial_early_print("[WARN] Boot device: LoadedImage unavailable, "
                               "using LocateProtocol fallback\n");
            /* No POST16_BL_BOOT_DEV_OK -- last POST stays at 0xB090
             * so a POST card shows the lookup did not succeed. */
        }
    }

    /*: Populate boot device path from DevicePathToText protocol.
     * Type stays 0 (unknown) until parses the device path nodes. */
    g_boot_info_ptr->boot_device_type = 0;
    g_boot_info_ptr->boot_device_path[0] = '\0';
    if (g_boot_device_handle) {
        EFI_GUID dp_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
        EFI_GUID dptt_guid = EFI_DEVICE_PATH_TO_TEXT_PROTOCOL_GUID;
        EFI_DEVICE_PATH_PROTOCOL *dp = (EFI_DEVICE_PATH_PROTOCOL *)0;
        EFI_DEVICE_PATH_TO_TEXT_PROTOCOL *dptt = (EFI_DEVICE_PATH_TO_TEXT_PROTOCOL *)0;
        EFI_STATUS dp_s;

        dp_s = gBS->HandleProtocol(g_boot_device_handle, &dp_guid, (VOID **)&dp);
        if (!EFI_ERROR(dp_s) && dp) {
            /*: Convert device path to text for boot_info */
            dp_s = gBS->LocateProtocol(&dptt_guid, (VOID *)0, (VOID **)&dptt);
            if (!EFI_ERROR(dp_s) && dptt) {
                CHAR16 *text = dptt->ConvertDevicePathToText(dp, 0, 0);
                if (text) {
                    UINTN j;
                    for (j = 0; j < 127 && text[j]; j++)
                        g_boot_info_ptr->boot_device_path[j] = (char)(text[j] & 0x7F);
                    g_boot_info_ptr->boot_device_path[j] = '\0';
                    gBS->FreePool(text);

                    serial_early_print("[BOOT] Boot device path: ");
                    serial_early_print(g_boot_info_ptr->boot_device_path);
                    serial_early_print("\n");
                }
            } else {
                serial_early_print("[BOOT] DevicePathToText not available\n");
            }

            /*: Classify device type from the TEXT path string.
             * The raw device path from HandleProtocol on a partition handle
             * starts at the HD() node -- it doesn't include parent Messaging
             * nodes (Sata/NVMe/USB). But DevicePathToText returns the full
             * path including parents. Parse the text for known substrings. */
            if (g_boot_info_ptr->boot_device_path[0] != '\0' &&
                g_boot_info_ptr->boot_device_type == 0) {
                const char *p = g_boot_info_ptr->boot_device_path;
                UINTN pi;
                for (pi = 0; p[pi]; pi++) {
                    if (p[pi] == 'S' && p[pi+1] == 'a' && p[pi+2] == 't' &&
                        p[pi+3] == 'a' && p[pi+4] == '(') {
                        g_boot_info_ptr->boot_device_type = 1; break;
                    }
                    if (p[pi] == 'N' && p[pi+1] == 'V' && p[pi+2] == 'M' &&
                        p[pi+3] == 'e' && p[pi+4] == '(') {
                        g_boot_info_ptr->boot_device_type = 2; break;
                    }
                    if (p[pi] == 'U' && p[pi+1] == 'S' && p[pi+2] == 'B' &&
                        p[pi+3] == '(') {
                        g_boot_info_ptr->boot_device_type = 3; break;
                    }
                    if (p[pi] == 'I' && p[pi+1] == 'P' && p[pi+2] == 'v' &&
                        (p[pi+3] == '4' || p[pi+3] == '6') && p[pi+4] == '(') {
                        g_boot_info_ptr->boot_device_type = 4; break;
                    }
                }
            }

            /*: Walk raw device path nodes to extract partition GUID.
             * The HD() node IS present in the partition handle's device path. */
            g_boot_info_ptr->boot_partition_style = 0;
            {
                const EFI_DEVICE_PATH_PROTOCOL *node = dp;
                const UINT8 *base = (const UINT8 *)dp;
                UINT16 node_len;
                UINTN walked = 0;
                #define DP_MAX_WALK 1024

                while (walked + 4 <= DP_MAX_WALK &&
                       !(node->Type == EFI_DP_TYPE_END &&
                         node->SubType == EFI_DP_SUBTYPE_END_ENTIRE)) {
                    node_len = (UINT16)node->Length[0] |
                               ((UINT16)node->Length[1] << 8);
                    if (node_len < 4) break;
                    if (walked + node_len > DP_MAX_WALK) break;

                    /*: Media/HardDrive -> partition GUID + style.
                     * UEFI spec Table 10-58: HardDrive DP node is 42 bytes.
                     * Offset 24: PartitionSignature (16 bytes)
                     * Offset 40: MBRType (0x01=MBR, 0x02=GPT)
                     * Offset 41: SignatureType (0x02=GUID for GPT) */
                    if (node->Type == EFI_DP_TYPE_MEDIA &&
                        node->SubType == EFI_DP_MEDIA_HARDDRIVE &&
                        node_len >= 42 &&
                        g_boot_info_ptr->boot_partition_style == 0) {
                        const UINT8 *nd = (const UINT8 *)node;
                        UINT8 mbr_type = nd[40];
                        UINT8 sig_type = nd[41];
                        UINTN gi;

                        if (mbr_type == 0x02 && sig_type == 0x02) {
                            /* GPT + GUID signature: copy 16 bytes from offset 24 */
                            g_boot_info_ptr->boot_partition_style = 2;
                            for (gi = 0; gi < 16; gi++)
                                g_boot_info_ptr->boot_partition_guid[gi] = nd[24 + gi];
                        } else if (mbr_type == 0x01 && sig_type == 0x01) {
                            /* MBR + 32-bit signature: 4 bytes from offset 24 */
                            g_boot_info_ptr->boot_partition_style = 1;
                            for (gi = 0; gi < 4; gi++)
                                g_boot_info_ptr->boot_partition_guid[gi] = nd[24 + gi];
                        }
                    }

                    node = (const EFI_DEVICE_PATH_PROTOCOL *)
                           ((const UINT8 *)node + node_len);
                    walked = (UINTN)((const UINT8 *)node - base);
                }
                #undef DP_MAX_WALK
            }

            /* Log detected type */
            {
                static const char *type_names[] = {
                    "unknown", "SATA", "NVMe", "USB", "network"
                };
                UINT8 t = g_boot_info_ptr->boot_device_type;
                serial_early_print("[BOOT] Boot device type: ");
                serial_early_print(t <= 4 ? type_names[t] : "invalid");
                serial_early_print("\n");
            }

            /*: Log partition GUID or MBR signature */
            if (g_boot_info_ptr->boot_partition_style == 2) {
                const UINT8 *g = g_boot_info_ptr->boot_partition_guid;
                /* Check for all-zero GUID */
                int zero = 1;
                UINT8 zi;
                for (zi = 0; zi < 16; zi++)
                    if (g[zi]) { zero = 0; break; }
                if (zero) {
                    serial_early_print("[WARN] Boot partition GUID is zero "
                                       "-- firmware may not support GPT\n");
                } else {
                    serial_early_print("[BOOT] Boot partition: GUID=");
                    /* Format: AABBCCDD-EEFF-GGHH-IIJJ-KKLLMMNNOOPP
                     * UEFI GUID layout: Data1(4B LE) Data2(2B LE)
                     * Data3(2B LE) Data4(8B) */
                    serial_early_print_hex16((UINT16)(g[3] << 8 | g[2]));
                    serial_early_print_hex16((UINT16)(g[1] << 8 | g[0]));
                    serial_early_print("-");
                    serial_early_print_hex16((UINT16)(g[5] << 8 | g[4]));
                    serial_early_print("-");
                    serial_early_print_hex16((UINT16)(g[7] << 8 | g[6]));
                    serial_early_print("-");
                    serial_early_print_hex16((UINT16)(g[8] << 8 | g[9]));
                    serial_early_print("-");
                    serial_early_print_hex16((UINT16)(g[10] << 8 | g[11]));
                    serial_early_print_hex16((UINT16)(g[12] << 8 | g[13]));
                    serial_early_print_hex16((UINT16)(g[14] << 8 | g[15]));
                    serial_early_print(" (GPT)\n");
                }
            } else if (g_boot_info_ptr->boot_partition_style == 1) {
                const UINT8 *g = g_boot_info_ptr->boot_partition_guid;
                serial_early_print("[BOOT] Boot partition: MBR sig=0x");
                serial_early_print_hex16((UINT16)(g[3] << 8 | g[2]));
                serial_early_print_hex16((UINT16)(g[1] << 8 | g[0]));
                serial_early_print("\n");
            }
        }
    }

    /* +: Removable media detection AND pre-boot device health check.
     * Single HandleProtocol(BlockIO) call for both. Reads: RemovableMedia,
     * MediaPresent, BlockSize, LastBlock, ReadOnly, LogicalPartition.
     * If BlockIO unavailable, USB defaults to removable (safe for cache policy). */
    g_boot_info_ptr->boot_media_present = 1;
    g_boot_info_ptr->boot_device_removable =
        (g_boot_info_ptr->boot_device_type == 3) ? 1 : 0;
    if (g_boot_device_handle) {
        EFI_GUID bio_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
        EFI_BLOCK_IO_PROTOCOL *bio = (EFI_BLOCK_IO_PROTOCOL *)0;
        EFI_STATUS bio_s;

        bio_s = gBS->HandleProtocol(g_boot_device_handle, &bio_guid,
                                     (VOID **)&bio);
        if (!EFI_ERROR(bio_s) && bio && bio->Media) {
            EFI_BLOCK_IO_MEDIA *m = bio->Media;

            /*: Removable + MediaPresent */
            g_boot_info_ptr->boot_device_removable =
                m->RemovableMedia ? 1 : 0;
            g_boot_info_ptr->boot_media_present =
                m->MediaPresent ? 1 : 0;

            /*: Capacity */
            {
                UINT64 total_bytes = 0;
                UINT32 mib = 0;
                if (m->BlockSize > 0 && m->LastBlock < 0xFFFFFFFFFFFFFFFFULL) {
                    UINT64 blocks = m->LastBlock + 1;
                    if (blocks <= 0xFFFFFFFFFFFFFFFFULL / (UINT64)m->BlockSize)
                        total_bytes = blocks * (UINT64)m->BlockSize;
                    mib = (UINT32)(total_bytes / (1024 * 1024));
                }
                serial_early_print("[BOOT] Boot disk: ");
                {
                    char nb[20];
                    UINT32 v = mib;
                    int n = 0;
                    if (v == 0) nb[n++] = '0';
                    else { while (v > 0 && n < 20) { nb[n++] = '0' + (char)(v % 10); v /= 10; } }
                    while (n > 0) serial_early_putchar(nb[--n]);
                }
                serial_early_print(" MiB (");
                {
                    static const char *tn[] = {"unknown","SATA","NVMe","USB","network"};
                    UINT8 t = g_boot_info_ptr->boot_device_type;
                    serial_early_print(t <= 4 ? tn[t] : "?");
                }
                serial_early_print(")\n");
            }

            /*: ReadOnly warning (abnormal on fixed non-USB disks) */
            if (m->ReadOnly && g_boot_info_ptr->boot_device_type != 3)
                serial_early_print("[WARN] Boot disk is read-only "
                                   "-- possible hardware failure\n");

            /*: LogicalPartition diagnostic */
            if (m->LogicalPartition)
                serial_early_print("[BOOT] Boot device is a logical partition "
                                   "(not whole disk)\n");
        } else {
            serial_early_print("[WARN] BlockIO not available on boot device"
                               " -- using type-based default\n");
        }
    }

    /* logging */
    if (g_boot_info_ptr->boot_device_removable)
        serial_early_print("[BOOT] Boot device is removable "
                           "-- write-caching will be disabled by default\n");
    else
        serial_early_print("[BOOT] Boot device is fixed\n");

    if (!g_boot_info_ptr->boot_media_present)
        serial_early_print("[WARN] Boot media not present "
                           "(reported by firmware -- may be stale)\n");

    /* §6: Read UEFI boot variables (BootCurrent, BootOrder, BootNext).
     * RuntimeServices->GetVariable is available before ExitBootServices. */
    {
        EFI_GUID global_guid = EFI_GLOBAL_VARIABLE_GUID;
        EFI_RUNTIME_SERVICES *rt = gST->RuntimeServices;
        UINT32 attrs = 0;

        /* Defaults */
        g_boot_info_ptr->uefi_boot_current = 0xFFFF;
        g_boot_info_ptr->uefi_boot_next = 0xFFFF;
        g_boot_info_ptr->uefi_boot_next_valid = 0;
        g_boot_info_ptr->uefi_boot_order_count = 0;

        if (rt && rt->GetVariable) {
            /* BootCurrent (UINT16) */
            {
                UINT16 bc = 0;
                UINTN sz = sizeof(bc);
                EFI_STATUS vs = rt->GetVariable(
                    u"BootCurrent", &global_guid, &attrs, &sz, &bc);
                if (!EFI_ERROR(vs) && sz == sizeof(bc)) {
                    g_boot_info_ptr->uefi_boot_current = bc;
                    serial_early_print("[BOOT] BootCurrent=0x");
                    serial_early_print_hex16(bc);
                    serial_early_print("\n");
                }
            }

            /* BootOrder (UINT16 array -- read full, store first 16).
             * Use a 128-entry stack buffer so GetVariable succeeds even
             * on firmware with large boot menus (>16 entries). */
            {
                UINT16 order[128];
                UINTN sz = sizeof(order);
                EFI_STATUS vs = rt->GetVariable(
                    u"BootOrder", &global_guid, &attrs, &sz, order);
                if (!EFI_ERROR(vs) && sz >= 2 && (sz % 2) == 0) {
                    UINTN count = sz / 2;
                    if (count > 16) count = 16;
                    g_boot_info_ptr->uefi_boot_order_count = (UINT8)count;
                    UINTN oi;
                    for (oi = 0; oi < count; oi++)
                        g_boot_info_ptr->uefi_boot_order[oi] = order[oi];

                    serial_early_print("[BOOT] BootOrder=[");
                    for (oi = 0; oi < count; oi++) {
                        if (oi > 0) serial_early_print(",");
                        serial_early_print("0x");
                        serial_early_print_hex16(order[oi]);
                    }
                    serial_early_print("]\n");
                }
            }

            /* BootNext (UINT16, optional one-shot override) */
            {
                UINT16 bn = 0;
                UINTN sz = sizeof(bn);
                EFI_STATUS vs = rt->GetVariable(
                    u"BootNext", &global_guid, &attrs, &sz, &bn);
                if (!EFI_ERROR(vs) && sz == sizeof(bn)) {
                    g_boot_info_ptr->uefi_boot_next = bn;
                    g_boot_info_ptr->uefi_boot_next_valid = 1;
                    serial_early_print("[BOOT] BootNext=0x");
                    serial_early_print_hex16(bn);
                    serial_early_print(" (one-shot override)\n");
                }
            }

            /* S12: Decode Boot#### EFI_LOAD_OPTION for the current boot entry.
             * Format variable name as Boot0000..BootFFFF from BootCurrent. */
            if (g_boot_info_ptr->uefi_boot_current != 0xFFFF) {
                static const char hex[] = "0123456789ABCDEF";
                UINT16 bc = g_boot_info_ptr->uefi_boot_current;
                CHAR16 var_name[] = u"Boot0000";
                /* Fill in the 4 hex digits (UCS-2) */
                var_name[4] = (CHAR16)hex[(bc >> 12) & 0xF];
                var_name[5] = (CHAR16)hex[(bc >> 8) & 0xF];
                var_name[6] = (CHAR16)hex[(bc >> 4) & 0xF];
                var_name[7] = (CHAR16)hex[bc & 0xF];

                UINT8 lo_buf[2048];
                UINTN lo_sz = sizeof(lo_buf);
                EFI_STATUS lo_s = rt->GetVariable(
                    var_name, &global_guid, &attrs, &lo_sz, lo_buf);

                if (lo_s == EFI_BUFFER_TOO_SMALL) {
                    serial_early_print("[BOOT] Boot#### entry too large ("
                                       "skipping decode)\n");
                } else if (!EFI_ERROR(lo_s) && lo_sz > 6) {
                    /* EFI_LOAD_OPTION layout:
                     * [0..3]  Attributes   (UINT32)
                     * [4..5]  FilePathListLength (UINT16)
                     * [6..]   Description  (NUL-terminated CHAR16)
                     * [after NUL] FilePathList (FilePathListLength bytes) */
                    UINT16 fp_len = (UINT16)(lo_buf[4] | ((UINT16)lo_buf[5] << 8));
                    const CHAR16 *desc = (const CHAR16 *)&lo_buf[6];
                    UINTN max_desc_bytes = lo_sz - 6;

                    /* Print description as ASCII (truncate at 80 chars) */
                    serial_early_print("[BOOT] Boot");
                    serial_early_print_hex16(bc);
                    serial_early_print(": ");
                    {
                        UINTN di;
                        for (di = 0; di < 80 && (di * 2 + 1) < max_desc_bytes; di++) {
                            if (desc[di] == 0) break;
                            serial_early_putchar((char)(desc[di] & 0x7F));
                        }
                    }
                    serial_early_print("\n");

                    /* Find end of Description to locate FilePathList */
                    {
                        UINTN di;
                        const UINT8 *fp_start = (const UINT8 *)0;
                        for (di = 0; (di * 2 + 1) < max_desc_bytes; di++) {
                            if (desc[di] == 0) {
                                fp_start = (const UINT8 *)&desc[di + 1];
                                break;
                            }
                        }

                        /* Convert FilePathList to text via DevicePathToText.
                         * First validate: walk nodes within fp_len to confirm
                         * a valid END_ENTIRE node exists (per Gate 13: spec
                         * compliance -- ConvertDevicePathToText doesn't take
                         * a length and would read past the buffer otherwise). */
                        if (fp_start && fp_len >= 4 &&
                            (UINTN)(fp_start - lo_buf) + fp_len <= lo_sz) {
                            int dp_valid = 0;
                            {
                                const UINT8 *vn = fp_start;
                                UINTN vw = 0;
                                while (vw + 4 <= fp_len) {
                                    UINT16 vl = (UINT16)(vn[2] | ((UINT16)vn[3] << 8));
                                    if (vl < 4 || vw + vl > fp_len) break;
                                    if (vn[0] == EFI_DP_TYPE_END &&
                                        vn[1] == EFI_DP_SUBTYPE_END_ENTIRE) {
                                        dp_valid = 1;
                                        break;
                                    }
                                    vn += vl;
                                    vw += vl;
                                }
                            }

                            if (dp_valid) {
                                EFI_GUID dptt_s12 = EFI_DEVICE_PATH_TO_TEXT_PROTOCOL_GUID;
                                EFI_DEVICE_PATH_TO_TEXT_PROTOCOL *dptt12 =
                                    (EFI_DEVICE_PATH_TO_TEXT_PROTOCOL *)0;
                                gBS->LocateProtocol(&dptt_s12, (VOID *)0,
                                                     (VOID **)&dptt12);
                                if (dptt12) {
                                    CHAR16 *fp_txt = dptt12->ConvertDevicePathToText(
                                        (EFI_DEVICE_PATH_PROTOCOL *)fp_start, 0, 0);
                                    if (fp_txt) {
                                        serial_early_print("[BOOT]   Path: ");
                                        UINTN fi;
                                        for (fi = 0; fi < 120 && fp_txt[fi]; fi++)
                                            serial_early_putchar((char)(fp_txt[fi] & 0x7F));
                                        if (fp_txt[fi])
                                            serial_early_print("...");
                                        serial_early_print("\n");
                                        gBS->FreePool(fp_txt);
                                    }
                                }
                            }
                        }
                    }
                }
                /* EFI_NOT_FOUND is silent (some VMs have minimal NVRAM) */
            }
        }
    }

    /* Step 1b: Parse boot.conf first -- Resolution= key needed by init_gop */
    g_boot_info_ptr->timing.conf_start = boot_rdtsc();
    parse_boot_conf();
    g_boot_info_ptr->timing.conf_end = boot_rdtsc();

    /* section 5 typed payload load. Runs right after boot.conf parse so
     * staging is populated, and before load_kernel so the payload
     * allocations do not compete with the 32/16/8 MiB graduated kernel
     * buffer. All failures are fatal: a missing payload == boot fail. */
    load_staged_payloads();

    /*: Boot device enumeration log -- gated behind verbose=1 in boot.conf
     * to avoid per-handle Open/Close overhead on firmware with many devices.
     * Runs after parse_boot_conf so the flag is available. */
    if (g_boot_info_ptr->config.verbose) {
        EFI_GUID fs_enum_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
        EFI_GUID dp_enum_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
        EFI_GUID dptt_enum_guid = EFI_DEVICE_PATH_TO_TEXT_PROTOCOL_GUID;
        EFI_GUID bio_enum_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
        EFI_HANDLE *enum_handles = (EFI_HANDLE *)0;
        UINTN enum_count = 0;
        EFI_STATUS enum_s;
        EFI_DEVICE_PATH_TO_TEXT_PROTOCOL *enum_dptt = (EFI_DEVICE_PATH_TO_TEXT_PROTOCOL *)0;

        gBS->LocateProtocol(&dptt_enum_guid, (VOID *)0, (VOID **)&enum_dptt);

        enum_s = gBS->LocateHandleBuffer(ByProtocol, &fs_enum_guid,
                                          (VOID *)0, &enum_count, &enum_handles);
        if (!EFI_ERROR(enum_s) && enum_handles && enum_count > 0) {
            serial_early_print("[BOOT] --- Device Enumeration (verbose=1) ---\n");
            UINTN ei;
            for (ei = 0; ei < enum_count; ei++) {
                int is_boot = (enum_handles[ei] == g_boot_device_handle);
                int has_kernel = 0;
                int removable = -1;

                /* Check kernel presence */
                {
                    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *efs;
                    EFI_FILE_PROTOCOL *eroot;
                    EFI_FILE_PROTOCOL *efile;
                    EFI_STATUS es = gBS->HandleProtocol(enum_handles[ei],
                                        &fs_enum_guid, (VOID **)&efs);
                    if (!EFI_ERROR(es)) {
                        es = efs->OpenVolume(efs, &eroot);
                        if (!EFI_ERROR(es)) {
                            es = eroot->Open(eroot, &efile,
                                              u"\\boot\\kernel.exe",
                                              EFI_FILE_MODE_READ, 0);
                            if (!EFI_ERROR(es)) {
                                has_kernel = 1;
                                efile->Close(efile);
                            }
                            eroot->Close(eroot);
                        }
                    }
                }

                /* Check removable */
                {
                    EFI_BLOCK_IO_PROTOCOL *ebio;
                    EFI_STATUS es = gBS->HandleProtocol(enum_handles[ei],
                                        &bio_enum_guid, (VOID **)&ebio);
                    if (!EFI_ERROR(es) && ebio && ebio->Media)
                        removable = ebio->Media->RemovableMedia ? 1 : 0;
                }

                /* Log device path + tags */
                {
                    EFI_DEVICE_PATH_PROTOCOL *edp;
                    EFI_STATUS es = gBS->HandleProtocol(enum_handles[ei],
                                        &dp_enum_guid, (VOID **)&edp);

                    serial_early_print("[BOOT]  [");
                    serial_early_print_hex16((UINT16)ei);
                    serial_early_print("] ");
                    if (is_boot) serial_early_print("*BOOT* ");

                    if (!EFI_ERROR(es) && edp && enum_dptt) {
                        CHAR16 *etxt = enum_dptt->ConvertDevicePathToText(edp, 0, 0);
                        if (etxt) {
                            UINTN ej;
                            for (ej = 0; ej < 80 && etxt[ej]; ej++)
                                serial_early_putchar((char)(etxt[ej] & 0x7F));
                            if (etxt[ej]) serial_early_print("...");
                            gBS->FreePool(etxt);
                        } else {
                            serial_early_print("(no path text)");
                        }
                    } else {
                        serial_early_print("(no device path)");
                    }

                    serial_early_print(has_kernel ? " [kernel]" : "");
                    serial_early_print(removable == 1 ? " [removable]"
                                     : removable == 0 ? " [fixed]"
                                     : " [removable=?]");
                    serial_early_print("\n");
                }
            }

            /* Summary line */
            serial_early_print("[BOOT] Device summary: ");
            {
                char nb[20];
                UINTN v = enum_count;
                int n = 0;
                if (v == 0) nb[n++] = '0';
                else { while (v > 0 && n < 20) { nb[n++] = '0' + (char)(v % 10); v /= 10; } }
                while (n > 0) serial_early_putchar(nb[--n]);
            }
            serial_early_print(" devices found, boot=");
            {
                static const char *tn[] = {"unknown","SATA","NVMe","USB","network"};
                UINT8 t = g_boot_info_ptr->boot_device_type;
                serial_early_print(t <= 4 ? tn[t] : "?");
            }
            serial_early_print(g_boot_info_ptr->boot_device_removable
                               ? " (removable)" : " (fixed)");
            serial_early_print("\n");
            serial_early_print("[BOOT] --- End Enumeration ---\n");

            gBS->FreePool(enum_handles);
        }
    }

    /* Step 1: Initialize graphics (uses g_conf_res_width/height from boot.conf) */
    post_code16(POST16_BL_GOP);
    watchdog_reset();
    serial_early_print("[BOOT] init_gop...\n");
    g_boot_info_ptr->timing.gop_start = boot_rdtsc();
    status = init_gop();
    if (EFI_ERROR(status)) {
        serial_early_print("[FAIL] init_gop\n");
        efi_print(u"[FAIL] Graphics initialization failed\r\n");
        if (g_wd_armed && gBS) {
            EFI_STATUS wd_dis = gBS->SetWatchdogTimer(0, 0, 0, (CHAR16 *)0);
            if (!EFI_ERROR(wd_dis))
                g_wd_armed = 0;
        }
        return status;
    }
    if (g_boot_info_ptr->fb.addr == 0)
        serial_early_print("[BOOT] init_gop OK (headless)\n");
    else
        serial_early_print("[BOOT] init_gop OK\n");
    g_boot_info_ptr->timing.gop_end = boot_rdtsc();

    /* Clear screen to black before loading the kernel. */
    {
        UINT32 row, col;
        for (row = 0; row < gFbHeight; row++)
            for (col = 0; col < gFbWidth; col++)
                gFramebuffer[row * gFbPitch + col] = 0x00000000;
    }

    /* S14: error_screen_test=1 in boot.conf triggers boot_fatal for QR/BSOD testing.
     * GOP is initialized, framebuffer is ready for QR code rendering. */
    if (g_boot_info_ptr->config.error_screen_test) {
        serial_early_print("[BOOT] error_screen_test=1 -- triggering boot_fatal\n");
        boot_fatal(BOOT_ERR_KERNEL_NOT_FOUND, "Error screen test",
                   "Deliberate boot failure triggered by error_screen_test=1 in boot.conf.");
    }

    /* Load kernel ELF */
    post_code16(POST16_BL_KERNEL_OPEN);
    watchdog_reset();
    serial_early_print("[BOOT] load_kernel...\n");
    g_boot_info_ptr->timing.kernel_load_start = boot_rdtsc();
    status = load_kernel(&kernel_entry);
    g_boot_info_ptr->timing.kernel_load_end = boot_rdtsc();
    if (EFI_ERROR(status)) {
        boot_fatal(BOOT_ERR_KERNEL_NOT_FOUND, "Kernel load failed",
                   "The kernel ELF could not be loaded from boot media.");
    }
    serial_early_print("[BOOT] load_kernel OK\n");
    post_code16(POST16_BL_KERNEL_LOAD);

    /* Step 4: Copy UEFI Configuration Table + find ACPI RSDP */
    post_code16(POST16_BL_RSDP);
    serial_early_print("[BOOT] copy_config_tables...\n");
    copy_config_tables();

    /* Step 4c: Retrieve TPM event log (if available) */
    retrieve_tpm_event_log();

    /* Step 4d: Parse FPDT for firmware boot timing */
    parse_fpdt();

    /* Estimate TSC frequency using UEFI Stall (1ms) */
    {
        UINT64 t0 = boot_rdtsc();
        gBS->Stall(1000);  /* 1ms */
        UINT64 t1 = boot_rdtsc();
        g_boot_info_ptr->timing.tsc_freq = (t1 - t0) * 1000;  /* Hz */
    }

    /* Step 4e: Discover USB devices via UEFI firmware (Phase A) */
    post_code16(POST16_BL_USB_DISC);
    watchdog_reset();
    serial_early_print("[BOOT] discover_usb_devices...\n");
    discover_usb_devices();
    post_code16(POST16_BL_USB_DISC_OK);

    /* Step 4f: Allocate persistent xHCI DMA structures */
    post_code16(POST16_BL_XHCI_DMA);
    serial_early_print("[BOOT] allocate_xhci_dma...\n");
    allocate_xhci_dma();
    post_code16(POST16_BL_XHCI_DMA_OK);

    /* Step 4g: Take over xHCI controller */
    post_code16(POST16_BL_XHCI_TAKEOVER);
    serial_early_print("[BOOT] xhci_controller_takeover...\n");
    xhci_controller_takeover();
    post_code16(POST16_BL_XHCI_TAKEOVER_OK);

    /* Step 5: Get UEFI memory map */
    post_code16(POST16_BL_MEMMAP);
    serial_early_print("[BOOT] get_memory_map...\n");
    status = get_memory_map(&map_key, &mmap, &map_size, &desc_size,
                            &desc_version);
    if (EFI_ERROR(status)) {
        boot_fatal(BOOT_ERR_MMAP_GETMAP_FAIL, "GetMemoryMap failed",
                   "UEFI firmware could not provide a memory map.");
    }

    /* Validate descriptor geometry before parsing (S12) */
    if (desc_size == 0 || desc_size < sizeof(EFI_MEMORY_DESCRIPTOR) ||
        map_size == 0 || map_size % desc_size != 0) {
        boot_fatal(BOOT_ERR_MMAP_GEOMETRY, "Memory map geometry invalid",
                   "Descriptor size or map size is malformed.");
    }

    fill_memory_map(mmap, map_size, desc_size);
    fill_runtime_map(mmap, map_size, desc_size);

    /* Step 5b: Preserve Runtime Services pointer + descriptor metadata */
    g_boot_info_ptr->uefi_runtime_services = (UINTN)gST->RuntimeServices;
    g_boot_info_ptr->uefi_rt_available     = (gST->RuntimeServices != (void *)0) ? 1 : 0;
    g_boot_info_ptr->uefi_mmap_desc_size    = (UINT32)desc_size;
    g_boot_info_ptr->uefi_mmap_desc_version = desc_version;

    /* Step 5c: Copy RT function pointers into boot_info.
     * SetVirtualAddressMap() MUST be called after ExitBootServices() per UEFI
     * spec §7.4.2.  The kernel owns SVAM; uefi_runtime_init() calls it once
     * boot services are gone.  svam_called stays 0. */
    {
        EFI_RUNTIME_SERVICES *rt = gST->RuntimeServices;
        if (rt != (void *)0) {
            g_boot_info_ptr->uefi_runtime.get_time                   = (UINT64)(UINTN)rt->GetTime;
            g_boot_info_ptr->uefi_runtime.set_time                   = (UINT64)(UINTN)rt->SetTime;
            g_boot_info_ptr->uefi_runtime.get_variable               = (UINT64)(UINTN)rt->GetVariable;
            g_boot_info_ptr->uefi_runtime.set_variable               = (UINT64)(UINTN)rt->SetVariable;
            g_boot_info_ptr->uefi_runtime.get_next_variable_name     = (UINT64)(UINTN)rt->GetNextVariableName;
            g_boot_info_ptr->uefi_runtime.reset_system               = (UINT64)(UINTN)rt->ResetSystem;
            g_boot_info_ptr->uefi_runtime.update_capsule             = (UINT64)(UINTN)rt->UpdateCapsule;
            g_boot_info_ptr->uefi_runtime.query_capsule_capabilities = (UINT64)(UINTN)rt->QueryCapsuleCapabilities;
            g_boot_info_ptr->uefi_runtime.query_variable_info        = (UINT64)(UINTN)rt->QueryVariableInfo;
            g_boot_info_ptr->uefi_runtime.get_wakeup_time            = (UINT64)(UINTN)rt->GetWakeupTime;
            g_boot_info_ptr->uefi_runtime.set_wakeup_time            = (UINT64)(UINTN)rt->SetWakeupTime;
        }
    }
    serial_early_print("[BOOT] runtime services preserved\n");

    /* Step 6: ExitBootServices -- bounded retry loop.
     * The UEFI spec allows the memory map to change between GetMemoryMap
     * and ExitBootServices (timer tick, firmware event). Retry with a
     * fresh map up to EBS_MAX_ATTEMPTS times (typical UEFI practice: 3-5).
     * Locked at 4 per TODO-03-bootloader-error-recovery.md S2 agreement. */
#define EBS_MAX_ATTEMPTS 4
    /* Disarm watchdog before ExitBootServices -- no longer needed (S11) */
    if (g_wd_armed) {
        EFI_STATUS wd_dis = gBS->SetWatchdogTimer(0, 0, 0, (CHAR16 *)0);
        if (!EFI_ERROR(wd_dis)) {
            serial_early_print("[BOOT] Watchdog: disarmed\n");
            g_wd_armed = 0;
        } else {
            serial_early_print("[WARN] Watchdog: disarm FAILED -- 60s timer may still be active\n");
            wd_dis = gBS->SetWatchdogTimer(0, 0, 0, (CHAR16 *)0);
            if (!EFI_ERROR(wd_dis)) {
                serial_early_print("[BOOT] Watchdog: disarmed on retry\n");
                g_wd_armed = 0;
            } else {
                serial_early_print("[WARN] Watchdog: disarm retry FAILED -- proceeding with caution\n");
            }
        }
    }

    post_code16(POST16_BL_EXIT_BS);
    serial_early_print("[BOOT] ExitBootServices...\n");
    g_boot_info_ptr->timing.exit_bs = boot_rdtsc();
    g_ebs_in_progress = 1;  /* Disable ConOut in boot_fatal from here */
    {
        int ebs_attempt;
        EFI_STATUS ebs_status;
        for (ebs_attempt = 0; ebs_attempt < EBS_MAX_ATTEMPTS; ebs_attempt++) {
            ebs_status = gBS->ExitBootServices(gImageHandle, map_key);
            if (!EFI_ERROR(ebs_status))
                break;
            serial_early_print("[BOOT] ExitBootServices retry\n");
            /* Re-fetch memory map for next attempt */
            status = get_memory_map(&map_key, &mmap, &map_size, &desc_size,
                                    &desc_version);
            if (EFI_ERROR(status)) {
                boot_fatal(BOOT_ERR_EBS_MMAP_FAIL, "GetMemoryMap failed on EBS retry",
                           "Memory map refresh failed during ExitBootServices retry.");
            }
            fill_memory_map(mmap, map_size, desc_size);
            fill_runtime_map(mmap, map_size, desc_size);
        }
        if (EFI_ERROR(ebs_status)) {
            boot_fatal(BOOT_ERR_EXIT_BS_FAIL, "ExitBootServices failed",
                       "UEFI ExitBootServices failed after 4 retry attempts.");
        }
    }
    serial_early_print("[BOOT] ExitBootServices OK\n");

    /* === NO MORE UEFI Boot Services CALLS FROM HERE === */

    /* Step 7: Set up page tables */
    post_code16(POST16_BL_PAGE_TABLES);
    serial_early_print("[BOOT] setup_page_tables...\n");
    setup_page_tables();

    /* Capability negotiation: advertise what this bootloader actually
     * produced. caps_required is left at 0 (this bootloader does not
     * demand any kernel-side feature beyond what BOOT_INFO_VERSION
     * already gates). For every known capability bit
     * we either set it in caps_present (feature was populated) or in
     * caps_degraded (producer intentionally skipped or could not
     * supply), but never both -- the kernel validator rejects the
     * overlap. */
    {
        UINT64 present  = 0;
        UINT64 degraded = 0;

        /* Typed payload descriptor array -- always published, even
         * when the packed prefix is empty (count=0 is still valid). */
        present |= BOOT_CAP_PAYLOAD_DESCRIPTORS;

        /* UEFI runtime services */
        if (g_boot_info_ptr->uefi_rt_available)
            present  |= BOOT_CAP_RUNTIME_SERVICES;
        else
            degraded |= BOOT_CAP_RUNTIME_SERVICES;

        /* Secure boot state: `secure_boot_enabled` is kernel-populated
         * AFTER handoff (the kernel queries the UEFI RT variable itself
         * during Phase 0). The bootloader has not yet established the
         * field, so it MUST advertise the capability as degraded here;
         * kernel-side refinement (promoting to caps_present once the
         * variable read succeeds) is a separate follow-up in the
         * capability-consumer path. */
        degraded |= BOOT_CAP_SECURE_BOOT_STATE;

        /* TPM measured-boot event log. Kernel consumers treat the log
         * as valid only when BOTH the pointer AND the size are
         * nonzero -- advertising present on a nonzero pointer alone
         * would let a consumer dereference a zero-length buffer. */
        if (g_boot_info_ptr->tpm_available &&
            g_boot_info_ptr->tpm_event_log != 0 &&
            g_boot_info_ptr->tpm_event_log_size != 0)
            present  |= BOOT_CAP_TPM_EVENT_LOG;
        else
            degraded |= BOOT_CAP_TPM_EVENT_LOG;

        /* USB handover: xHCI BIOS->OS ownership transfer + DMA state */
        if (g_boot_info_ptr->usb_handover_complete)
            present  |= BOOT_CAP_USB_HANDOVER;
        else
            degraded |= BOOT_CAP_USB_HANDOVER;

        /* Media role: boot device type + presence */
        if (g_boot_info_ptr->boot_media_present && g_boot_info_ptr->boot_device_type != 0)
            present  |= BOOT_CAP_MEDIA_ROLE;
        else
            degraded |= BOOT_CAP_MEDIA_ROLE;

        /* Network provenance: not populated by this bootloader yet
         * (PXE/HTTP boot path is future work). Advertise as degraded so
         * downstream consumers can pick a safe fallback. */
        degraded |= BOOT_CAP_NETWORK_PROVENANCE;

        /* Resume metadata (S4 hibernation): not populated yet. */
        degraded |= BOOT_CAP_RESUME_METADATA;

        /* Alternate protocol adapter: the bit advertises presence of a
         * non-native protocol shim (Multiboot2 / PXE / HTTP boot /
         * Secure Launch) between firmware and kernel. This bootloader
         * is native UEFI, so no adapter was interposed -- the feature
         * is absent by design. Classify as degraded; a real adapter
         * (TODO-08) would set this bit in caps_present instead. */
        degraded |= BOOT_CAP_ALT_PROTOCOL_ADAPTER;

        g_boot_info_ptr->caps_required = 0ull;
        g_boot_info_ptr->caps_present  = present;
        g_boot_info_ptr->caps_degraded = degraded;
    }

    /* Boot-path provenance and decision record: one shared answer to
     * "what path did this boot take and why". This bootloader only
     * implements the native UEFI cold-boot path today; installer /
     * recovery / network / resume / fast-startup / diagnostic paths
     * are future work tracked in neighboring domain TODOs. The record
     * still gets populated here so Registry / BlackBox / attestation
     * consumers have a stable contract from day one. */
    {
        UINT32 src_flags = 0;
        if (g_boot_info_ptr->uefi_boot_next_valid)
            src_flags |= BOOT_SOURCE_FLAG_BOOT_NEXT_SET;
        if (g_boot_info_ptr->boot_device_removable)
            src_flags |= BOOT_SOURCE_FLAG_MEDIA_REMOVABLE;
        if (g_boot_info_ptr->boot_media_present)
            src_flags |= BOOT_SOURCE_FLAG_MEDIA_PRESENT;
        /* BOOT_CURRENT_MISMATCH: firmware landed on a non-primary
         * Boot#### entry without an explicit BootNext override. That
         * is exactly the "fallthrough to a secondary entry" condition
         * recovery + attestation consumers need to see; without this
         * flag those cases would be indistinguishable from an ordinary
         * cold boot. Gates:
         *   - BootOrder populated (count > 0). No list -> nothing to
         *     compare against.
         *   - BootCurrent is actually known. uefi_boot_current defaults
         *     to 0xFFFF when GetVariable(BootCurrent) is absent or
         *     fails; comparing 0xFFFF vs BootOrder[0] would false-
         *     positive on firmware that exposes BootOrder but not
         *     BootCurrent (rare but observed on OVMF builds).
         *   - Not an operator BootNext selection. If BootNext was used,
         *     BootCurrent is whatever the operator picked and
         *     "mismatch" is meaningless. */
        if (g_boot_info_ptr->uefi_boot_order_count > 0 &&
            g_boot_info_ptr->uefi_boot_current != 0xFFFF &&
            !g_boot_info_ptr->uefi_boot_next_valid &&
            g_boot_info_ptr->uefi_boot_current !=
                g_boot_info_ptr->uefi_boot_order[0]) {
            src_flags |= BOOT_SOURCE_FLAG_BOOT_CURRENT_MISMATCH;
        }
        /* Rollback / recovery / resume-invalidation / network-insecure /
         * manifest-failed / measured-boot-failed flags stay clear on
         * this loader; the paths that would set them (rollback store,
         * recovery trigger probe, hibernation image validator, PXE
         * adapter, manifest verifier, TPM attestation) are not
         * implemented yet. When they land, the producer adds the flag
         * here and picks the matching boot_reason. */

        g_boot_info_ptr->boot_path = BOOT_PATH_NORMAL;
        /* If BootNext was populated, the operator picked this entry
         * explicitly -- record USER_SELECTED so the decision log
         * carries that nuance even though the resulting path is
         * NORMAL. */
        g_boot_info_ptr->boot_reason =
            (src_flags & BOOT_SOURCE_FLAG_BOOT_NEXT_SET)
                ? BOOT_REASON_USER_SELECTED
                : BOOT_REASON_NORMAL;
        g_boot_info_ptr->boot_source_flags = src_flags;
        /* Fallback depth: 0 on the primary path. The current loader
         * has a fallback chain for boot-device detection but does not
         * count rungs; future work threads a counter through the
         * fallback path and writes the final value here. */
        g_boot_info_ptr->boot_fallback_depth = 0u;
    }

    /* Anti-rollback and security-version binding. Reads UEFI NVRAM
     * variable IPOSRequiredSecVersion (if present), sets shipped +
     * required fields, and HALTS with a fatal UEFI screen on a
     * downgrade. The check lives here (near the end of populate,
     * before the header-magic write) so all other boot_info fields
     * are already populated for a potential post-mortem read.
     *
     * Uses the shared g_impossible_os_guid for the NVRAM namespace.
     * Build-time shipped version is IPOS_KERNEL_SECURITY_VERSION
     * (defaults to 1 until a release policy raises it). */
    {
        static CHAR16 req_name[] = L"IPOSRequiredSecVersion";
        UINT32 required = 0;
        UINTN sz = sizeof(required);
        UINT32 attrs = 0;
        EFI_STATUS st = EFI_UNSUPPORTED;
        /* Expected attributes: NV | BS | RT. Must match what the
         * kernel-side writer uses in uefi_set_variable. */
        const UINT32 EXPECTED_ATTRS =
            EFI_VARIABLE_NON_VOLATILE |
            EFI_VARIABLE_BOOTSERVICE_ACCESS |
            EFI_VARIABLE_RUNTIME_ACCESS;
        int read_failed_closed = 0;

        /* Read required version via UEFI RT GetVariable. Fail-closed
         * policy: only EFI_NOT_FOUND is a legitimate first-ever-boot
         * state. Any other error, wrong size, or wrong attr set is
         * treated as an integrity failure -- we halt rather than
         * silently zeroing `required` (which would disable rollback
         * protection exactly at the trust boundary).
         *
         * Special case: missing runtime services entirely (e.g.
         * non-UEFI handoff) -- treat as EFI_NOT_FOUND; the caps
         * negotiation ABI's RUNTIME_SERVICES degraded bit is the
         * right place to signal that to the kernel. */
        if (!gST || !gST->RuntimeServices ||
            !gST->RuntimeServices->GetVariable) {
            st = EFI_NOT_FOUND;
        } else {
            st = gST->RuntimeServices->GetVariable(
                req_name, &g_impossible_os_guid, &attrs, &sz, &required);
        }

        if (st == EFI_NOT_FOUND) {
            /* First-ever boot: variable absent. Accept any shipped. */
            required = 0;
        } else if (EFI_ERROR(st)) {
            /* Any OTHER error: fail closed. A transient read error
             * or a crafted malformed variable must not disable
             * enforcement. */
            read_failed_closed = 1;
        } else if (sz != sizeof(required)) {
            /* Size mismatch: NVRAM contains something other than a
             * u32. Crafted or corrupted. Fail closed. */
            read_failed_closed = 1;
        } else if ((attrs & EXPECTED_ATTRS) != EXPECTED_ATTRS) {
            /* Attributes don't include NV|BS|RT: variable was written
             * by something that doesn't follow our policy (e.g.
             * volatile write that wouldn't persist, or missing RT so
             * the kernel couldn't advance it). Fail closed. */
            read_failed_closed = 1;
        } else if (required > BOOT_SECURITY_VERSION_MAX) {
            /* Runaway counter: also fail closed. Prevents a
             * corrupted-NVRAM brick by halting with a diagnostic the
             * operator can fix from UEFI shell. */
            read_failed_closed = 1;
        }

        if (read_failed_closed) {
            post_code16(POST16_BL_ROLLBACK_REFUSE);
            serial_early_print(
                "[BOOT] ANTI-ROLLBACK: IPOSRequiredSecVersion read failed "
                "(status/size/attrs invalid) -- failing closed. ");
            /* Log EFI_STATUS as 64-bit hex so the error bit
             * (0x8000000000000000) is preserved -- narrowing to UINT32
             * drops it and makes every failure look like a benign
             * low value. `attrs` is defined as UINT32 per UEFI spec;
             * `sz` (UINTN) is 64-bit on x86_64 but the size fits
             * comfortably in 32 bits for this variable. */
            serial_early_print("status=0x");
            serial_early_print_hex64((UINT64)st);
            serial_early_print(" sz=");
            serial_early_print_uint((UINT32)sz);
            serial_early_print(" attrs=0x");
            serial_early_print_uint(attrs);
            serial_early_print(" value=");
            serial_early_print_uint(required);
            serial_early_print(
                "\n -- operator recovery: clear the variable from UEFI shell "
                "(setvar / dmpstore) or reflash with matching policy.\n");
            g_boot_info_ptr->flags |= BOOT_FLAG_ROLLBACK_READ_FAILED;
            boot_fatal(POST16_BL_ROLLBACK_REFUSE,
                       "Anti-rollback read failure",
                       "IPOSRequiredSecVersion unreadable or malformed");
        }

        UINT32 shipped = (UINT32)IPOS_KERNEL_SECURITY_VERSION;
        g_boot_info_ptr->os_loader_security_version = shipped;
        g_boot_info_ptr->required_security_version  = required;

        if (shipped < required) {
            /* Refusal path: set the telemetry flag, emit POST16 +
             * serial diagnostic, render the UCS-2 rollback-specific
             * screen, persist a SEC_ROLLBACK fault record to NVRAM
             * (consumed by boot_version_blackbox_transcribe on the
             * next successful boot), and cold-reset. Does NOT jump
             * to kernel. */
            g_boot_info_ptr->flags |= BOOT_FLAG_ROLLBACK_REFUSAL;
            post_code16(POST16_BL_ROLLBACK_REFUSE);
            serial_early_print(
                "[BOOT] ANTI-ROLLBACK REFUSAL: os_loader_security_version=");
            serial_early_print_uint(shipped);
            serial_early_print(" < required_security_version=");
            serial_early_print_uint(required);
            serial_early_print(
                " -- kernel.exe is older than policy permits. Refusing to jump.\n");
            bpp_render_rollback_and_halt(shipped, required);
        }

        post_code16(POST16_BL_ROLLBACK_PASS);
    }

    /* S15: Populate ABI header as the final step before kernel handoff.
     * Written last so any late modifications to boot_info don't overwrite it. */
    g_boot_info_ptr->header.magic   = BOOT_INFO_MAGIC;
    g_boot_info_ptr->header.version = BOOT_INFO_VERSION;
    g_boot_info_ptr->header.size    = (UINT16)sizeof(struct boot_info);

    /* Step 8: Jump to kernel! */
    post_code16(POST16_BL_KERNEL_JUMP);
    serial_early_print("[BOOT] jumping to kernel_main\n");
    g_boot_info_ptr->timing.kernel_jump = boot_rdtsc();
    jump_to_kernel(kernel_entry);

    /* Never reached */
    return EFI_SUCCESS;
}
