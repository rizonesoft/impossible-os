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

/* --- Boot info structure (must match kernel/boot_info.h exactly) --- */
#define BOOT_MMAP_MAX_ENTRIES 512

struct boot_mmap_entry {
    UINT64 base_addr;
    UINT64 length;
    UINT32 type;              /* simplified: 1=available, 2=reserved, 3=ACPI, 4=NVS, 5=bad */
    UINT32 uefi_memory_type;  /* original EFI_MEMORY_TYPE enum value (0–14) */
    UINT64 attribute;         /* UEFI memory attribute flags */
};

struct boot_framebuffer {
    UINT64  addr;
    UINT32  pitch;
    UINT32  width;
    UINT32  height;
    UINT8   bpp;
    UINT8   type;
    UINT8   pixel_format;
    UINT8   pad0;
};

#define BOOT_GOP_MODE_MAX  32

struct boot_gop_mode {
    UINT32  width;
    UINT32  height;
    UINT32  pixels_per_scanline;
    UINT8   pixel_format;
    UINT8   pad[3];
};

/* Boot configuration from \EFI\ImpossibleOS\boot.conf
 * Must match struct boot_config in kernel/boot_info.h exactly. */
#define BOOT_CONF_CMDLINE_MAX 256

struct boot_config {
    UINT8   debug;
    UINT8   verbose;
    UINT8   serial_debug;
    UINT8   boot_mode;
    UINT16  splash_timeout;
    UINT8   heartbeat;
    UINT8   postcode;
    UINT8   postbars;
    UINT8   test;              /* 1 = run unit tests only, then shutdown */
    UINT8   test_suite;        /* category filter: 0-8 = specific, 0xFF = all */
    UINT8   test_quiet;        /* 1 = suppress PASS lines, show FAIL + summary */
    UINT8   diag_delay;        /* seconds to pause on each diag screen (0 = skip) */
    UINT8   diag_splash;       /* 1 = show diag on splash (bare metal, no serial) */
    UINT8   deferred;          /* 1 = defer non-critical inits (default), 0 = all in-phase */
    UINT8   async_init;        /* 1 = parallel subsystem init on APs, 0 = sequential (default) */
    UINT8   crash_test;        /* 1 = trigger deliberate BSOD after desktop init */
    UINT8   ob_handle_trace;   /* 1 = log every handle alloc/free to klog */
    UINT8   config_version;    /* 0 = legacy, 1+ = versioned (S7) */
    UINT8   error_screen_test; /* 1 = call boot_fatal() before kernel load (S14) */
    UINT8   _reserved[12];     /* future fields -- zero-filled by defaults */
    char    cmdline[BOOT_CONF_CMDLINE_MAX];
    UINT8   config_found;
    UINT8   _pad[223];         /* pad to 512 bytes total (sector-aligned) */
};

/* Mirror of kernel/boot_info.h static asserts -- catches drift between
 * bootloader and kernel struct definitions at compile time. */
_Static_assert(__builtin_offsetof(struct boot_config, cmdline) == 32,
    "cmdline must be at byte offset 32 -- kernel ABI contract");
_Static_assert(sizeof(struct boot_config) == 512,
    "boot_config must be exactly 512 bytes (sector-aligned)");

#define BOOT_CONFIG_TABLE_MAX 32
#define BOOT_RT_MMAP_MAX      64

struct boot_rt_mem_entry {
    UINT64 phys_addr;
    UINT64 num_pages;
    UINT64 attribute;   /* EFI memory attributes (cache type + EFI_MEMORY_RUNTIME) */
    UINT32 type;
    UINT32 reserved;
};

/* UEFI Runtime Service function pointers -- must match kernel/boot_info.h */
struct boot_uefi_runtime {
    UINT64 get_time;
    UINT64 set_time;
    UINT64 get_variable;
    UINT64 set_variable;
    UINT64 get_next_variable_name;
    UINT64 reset_system;
    UINT64 update_capsule;
    UINT64 query_capsule_capabilities;
    UINT64 query_variable_info;
    UINT64 get_wakeup_time;
    UINT64 set_wakeup_time;
    UINT8  svam_called;
    UINT8  pad[7];
};

struct boot_uefi_guid {
    UINT32  data1;
    UINT16  data2;
    UINT16  data3;
    UINT8   data4[8];
};

struct boot_uefi_config_entry {
    struct boot_uefi_guid guid;
    UINT64  table_addr;
};

/* USB device structures -- must match kernel/boot_info.h */
#define BOOT_USB_MAX_DEVICES     16
#define BOOT_USB_MAX_ENDPOINTS    4

struct boot_usb_endpoint {
    UINT8   address;
    UINT8   attributes;
    UINT16  max_packet;
    UINT8   interval;
    UINT8   pad[3];
};

struct boot_usb_device {
    UINT8   active;
    UINT8   port;
    UINT8   speed;
    UINT8   device_class;
    UINT8   iface_class;
    UINT8   iface_subclass;
    UINT8   iface_protocol;
    UINT8   num_endpoints;
    UINT16  vendor_id;
    UINT16  product_id;
    UINT8   is_msc;
    UINT8   is_hid;
    UINT16  pad0;
    UINT32  block_size;
    UINT64  block_count;
    struct boot_usb_endpoint endpoints[BOOT_USB_MAX_ENDPOINTS];
};

/* xHCI controller DMA state -- must match kernel/boot_info.h */
#define BOOT_USB_MAX_SCRATCHPADS 16
#define BOOT_USB_MAX_DMA_PAGES   16

struct boot_usb_controller {
    UINT8   pci_bus;
    UINT8   pci_dev;
    UINT8   pci_func;
    UINT8   active;
    UINT64  mmio_phys;
    UINT32  mmio_size;
    UINT8   cap_length;
    UINT16  hci_version;
    UINT32  max_slots;
    UINT32  max_intrs;
    UINT32  max_ports;
    UINT32  db_offset;
    UINT32  rts_offset;
    UINT8   ac64;
    UINT8   csz;
    UINT32  max_scratchpads;
    UINT64  dcbaa_phys;
    UINT64  scratchpad_array_phys;
    UINT64  scratchpad_base_phys;
    UINT32  scratchpad_page_count;
    UINT32  scratchpad_pad;
    UINT64  cmd_ring_phys;
    UINT64  evt_ring_phys;
    UINT64  erst_phys;
    UINT64  dma_pages[BOOT_USB_MAX_DMA_PAGES];
    UINT32  dma_page_count;
    UINT32  alloc_fail_status;
    UINT32  alloc_fail_page;
};

struct boot_info {
    struct boot_mmap_entry  mmap[BOOT_MMAP_MAX_ENTRIES];
    UINT32  mmap_count;
    UINT8   mmap_truncated;
    UINT8   mmap_quirks;       /* 1 if any descriptors had validation warnings (S12) */
    UINT8   _mmap_pad[2];
    UINT32  mem_lower_kb;
    UINT32  mem_upper_kb;
    struct boot_framebuffer fb;
    UINT8   fb_available;
    UINT8   hidpi;          /* 1 if negotiated GOP width >= 2560 */
    UINT8   fb_pad[2];
    /* GOP mode list */
    struct boot_gop_mode gop_modes[BOOT_GOP_MODE_MAX];
    UINT32  gop_mode_count;
    UINT32  gop_mode_selected;
    UINT64  acpi_rsdp_addr;
    UINT8   acpi_version;
    UINT8   acpi_available;
    UINT64  module_start;
    UINT64  module_end;
    UINT8   module_available;
    struct boot_config config;
    /* UEFI Configuration Table */
    struct boot_uefi_config_entry config_table[BOOT_CONFIG_TABLE_MAX];
    UINT32  config_table_count;
    /* UEFI Runtime Services */
    UINT64  uefi_runtime_services;
    UINT8   uefi_rt_available;
    struct boot_rt_mem_entry rt_mmap[BOOT_RT_MMAP_MAX];
    UINT32  rt_mmap_count;
    UINT32  uefi_mmap_desc_size;
    UINT32  uefi_mmap_desc_version;
    struct boot_uefi_runtime uefi_runtime;
    /* TPM Measured Boot */
    UINT64  tpm_event_log;
    UINT32  tpm_event_log_size;
    UINT8   tpm_available;
    UINT8   tpm_version;
    UINT16  tpm_event_count;
    /* USB devices discovered before ExitBootServices */
    struct boot_usb_device usb_devices[BOOT_USB_MAX_DEVICES];
    UINT32  usb_device_count;
    UINT8   usb_discovery_ok;
    UINT8   usb_handover_complete;
    UINT8   usb_pad[2];
    /* xHCI controller DMA state */
    struct boot_usb_controller usb_controller;
    /* Boot Timing */
    struct {
        UINT64 reset_end;
        UINT64 os_loader_load_start;
        UINT64 os_loader_start_start;
        UINT64 exit_bs_entry;
        UINT64 exit_bs_exit;
        UINT8  fpdt_available;
        UINT64 bl_entry;
        UINT64 gop_start;
        UINT64 gop_end;
        UINT64 conf_start;
        UINT64 conf_end;
        UINT64 kernel_load_start;
        UINT64 kernel_load_end;
        UINT64 splash_start;
        UINT64 splash_end;
        UINT64 exit_bs;
        UINT64 kernel_jump;
        UINT64 tsc_freq;
    } timing;

    /* Serial port (probed by bootloader; 0 = no UART detected) */
    UINT16 serial_port;
    UINT8  serial_source;         /* 0=none, 1=SPCR, 2=I/O-probe */
    UINT8  _serial_pad;
    UINT32 serial_baud;           /* baud rate from SPCR (0 = use default 38400) */

    /* NVRAM boot error from previous boot (S13); 0 = last boot OK */
    UINT32 last_boot_error;

    /* Kernel-populated fields (set after boot) */
    UINT8   secure_boot_enabled;
    UINT8   _kp_pad[3];
    UINT32  degraded_mask;
    UINT32  hv_flags;
    char    hv_vendor[16];
};

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
static UINT32  gFbPitch;  /* in pixels */

/* Requested resolution from boot.conf Resolution=WxH (0 = auto). */
static UINT32  g_conf_res_width  = 0;
static UINT32  g_conf_res_height = 0;

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

        /* S14: Render QR code on framebuffer (direct pixel write, no EFI call) */
        qr_render_error_url(err_code);

        /* Wait for keypress with timeout (10 seconds at ~1ms per iteration).
         * Prevents infinite spin on headless servers with no keyboard. */
        if (gST->ConIn) {
            EFI_INPUT_KEY key;
            UINT32 timeout = 10000000;  /* ~10s at firmware poll speed */
            while (gST->ConIn->ReadKeyStroke(gST->ConIn, &key) != 0 && --timeout)
                ;
        }
    } else if (gFramebuffer && gFbWidth > 0 && gFbHeight > 0) {
        /* EBS in progress or no ConOut -- still render QR to framebuffer
         * since direct pixel writes work without Boot Services */
        qr_render_error_url(err_code);
    }

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
        g_boot_info_ptr->fb_available = 0;
        g_boot_info_ptr->hidpi = 0;
        return EFI_SUCCESS;
    }

    /* Validate framebuffer size against mode dimensions before touching VRAM */
    gFramebuffer = (UINT32 *)(UINTN)gop->Mode->FrameBufferBase;
    gFbWidth  = gop->Mode->Info->HorizontalResolution;
    gFbHeight = gop->Mode->Info->VerticalResolution;
    gFbPitch  = gop->Mode->Info->PixelsPerScanLine;

    {
        UINTN required_bytes = (UINTN)gFbHeight * (UINTN)gFbPitch * 4;
        UINTN fb_size = gop->Mode->FrameBufferSize;
        if (gFbPitch < gFbWidth || required_bytes == 0 ||
            (fb_size > 0 && required_bytes > fb_size)) {
            serial_early_print("[BOOT] GOP: framebuffer size mismatch -- headless\n");
            gFramebuffer = (UINT32 *)0;
            gFbWidth = 0; gFbHeight = 0; gFbPitch = 0;
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
        else                                   cfg->test_suite = 0xFF;
    }
    else if (ascii_streq(key, "test_quiet")) {
        cfg->test_quiet = (UINT8)ascii_atoi(val);
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

    serial_early_print("[BOOT] parse_boot_conf...\n");

    /* Open filesystem */
    status = gBS->LocateProtocol(&fs_guid, (VOID *)0, (VOID **)&fs);
    if (EFI_ERROR(status)) {
        serial_early_print("[BOOT] boot.conf - no filesystem\n");
        return;
    }

    status = fs->OpenVolume(fs, &root_dir);
    if (EFI_ERROR(status)) {
        serial_early_print("[BOOT] boot.conf - cannot open volume\n");
        return;
    }

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

    /* Read entire file (cap at 4096 bytes -- S7 validation) */
    char buf[4096];
    UINTN buf_size = sizeof(buf) - 1;
    status = conf_file->Read(conf_file, &buf_size, buf);
    conf_file->Close(conf_file);
    root_dir->Close(root_dir);

    if (EFI_ERROR(status) || buf_size == 0) {
        serial_early_print("[BOOT] boot.conf read error - using defaults\n");
        return;
    }

    /* Warn if file was larger than buffer (truncated) */
    if (buf_size >= sizeof(buf) - 1) {
        serial_early_print("[WARN] boot.conf too large (");
        serial_early_print_uint((UINT32)buf_size);
        serial_early_print(" bytes), truncating\n");
    }

    buf[buf_size] = '\0';

    serial_early_print("[BOOT] boot.conf loaded\n");

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

        /* Parse this key=value */
        if (ki > 0 && vi > 0)
            parse_conf_kv(cfg, key, val);
    }

    cfg->config_found = 1;
}

/* ============================================================================
 * Step 2: Load kernel ELF from FAT32
 * ============================================================================ */
static EFI_STATUS load_kernel(UINT64 *entry_point)
{
    EFI_GUID fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_GUID li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
    EFI_LOADED_IMAGE_PROTOCOL *loaded_image;
    EFI_FILE_PROTOCOL *root_dir, *kernel_file;
    EFI_STATUS status;
    UINT8 *file_buf;
    UINTN file_size;
    Elf64_Ehdr *ehdr;
    Elf64_Phdr *phdr;
    UINT16 i;

    /* Use LoadedImage->DeviceHandle to get the boot device's filesystem
     * (not LocateProtocol which returns an arbitrary filesystem). */
    status = gBS->HandleProtocol(gImageHandle, &li_guid,
                                  (VOID **)&loaded_image);
    if (!EFI_ERROR(status) && loaded_image && loaded_image->DeviceHandle) {
        status = gBS->HandleProtocol(loaded_image->DeviceHandle,
                                      &fs_guid, (VOID **)&fs);
    } else {
        /* Fallback: locate any filesystem protocol */
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
            serial_early_print(
                "[FAIL] Kernel not found. Searched: "
                "\\boot\\kernel.exe, \\kernel.exe, "
                "\\EFI\\ImpossibleOS\\kernel.exe\n");
            root_dir->Close(root_dir);
            return EFI_NOT_FOUND;
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

        /* Verify buffer doesn't overlap boot_info (0x10000-0x11000) */
        if (buf_addr < 0x11000 && buf_addr + alloc_size > BOOT_INFO_PHYS_ADDR) {
            serial_early_print("[FAIL] Kernel buffer overlaps boot_info region\n");
            return EFI_LOAD_ERROR;
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

        /* Reject segments that overlap boot_info region (0x10000-0x11000) */
        {
            UINT64 seg_start = phdr[i].p_paddr;
            UINT64 seg_end   = seg_start + phdr[i].p_memsz;
            if (seg_start < 0x11000 && seg_end > BOOT_INFO_PHYS_ADDR) {
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

static void fill_memory_map(EFI_MEMORY_DESCRIPTOR *mmap,
                             UINTN map_size, UINTN desc_size)
{
    UINTN offset;
    UINT32 idx = 0;
    UINT64 total_mem = 0;
    /* Guard against malformed descriptor geometry (S12) */
    if (desc_size == 0 || desc_size < sizeof(EFI_MEMORY_DESCRIPTOR)) {
        serial_early_print("[FAIL] Memory map: invalid descriptor size\n");
        g_boot_info_ptr->mmap_count = 0;
        return;
    }

    UINT32 total_descs = (UINT32)(map_size / desc_size);

    /* Detect overflow before filling (S6) */
    if (total_descs > BOOT_MMAP_MAX_ENTRIES) {
        serial_early_print("[WARN] Memory map has ");
        serial_early_print_uint(total_descs);
        serial_early_print(" entries, truncating to ");
        serial_early_print_uint(BOOT_MMAP_MAX_ENTRIES);
        serial_early_print("\n");
        g_boot_info_ptr->mmap_truncated = 1;
    } else {
        g_boot_info_ptr->mmap_truncated = 0;
    }

    g_boot_info_ptr->mmap_quirks = 0;

    for (offset = 0; offset < map_size && idx < BOOT_MMAP_MAX_ENTRIES;
         offset += desc_size) {
        EFI_MEMORY_DESCRIPTOR *desc =
            (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)mmap + offset);
        UINT32 entry_num = (UINT32)(offset / desc_size);

        /* ---- Descriptor validation (S12) ---- */

        /* Range overflow: NumberOfPages * PAGE_SIZE or PhysicalStart + length wraps */
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

        /* Zero-length descriptors are invalid -- skip */
        if (desc->NumberOfPages == 0) {
            serial_early_print("[WARN] Memory map entry ");
            serial_early_print_uint(entry_num);
            serial_early_print(": zero pages -- skipping\n");
            g_boot_info_ptr->mmap_quirks = 1;
            continue;
        }

        /* PhysicalStart must be page-aligned (4 KiB) */
        if (desc->PhysicalStart & 0xFFF) {
            serial_early_print("[WARN] Memory map entry ");
            serial_early_print_uint(entry_num);
            serial_early_print(": unaligned PhysicalStart -- skipping\n");
            g_boot_info_ptr->mmap_quirks = 1;
            continue;
        }

        /* Type must be a valid EFI_MEMORY_TYPE (0-14; EfiMaxMemoryType=15 is sentinel) */
        if (desc->Type >= EfiMaxMemoryType) {
            serial_early_print("[WARN] Memory map entry ");
            serial_early_print_uint(entry_num);
            serial_early_print(": invalid type 0x");
            serial_early_print_hex16((UINT16)desc->Type);
            serial_early_print(" -- skipping\n");
            g_boot_info_ptr->mmap_quirks = 1;
            continue;
        }

        /* Overlap detection: check against all previously accepted entries.
         * O(n^2) but n is typically ~130, runs once at boot -- acceptable. */
        {
            UINT64 new_start = desc->PhysicalStart;
            UINT64 new_end   = new_start + desc->NumberOfPages * EFI_PAGE_SIZE;
            int overlap = 0;
            UINT32 j;
            for (j = 0; j < idx; j++) {
                UINT64 prev_start = g_boot_info_ptr->mmap[j].base_addr;
                UINT64 prev_end   = prev_start + g_boot_info_ptr->mmap[j].length;
                if (new_start < prev_end && new_end > prev_start) {
                    serial_early_print("[WARN] Memory map entry ");
                    serial_early_print_uint(entry_num);
                    serial_early_print(": overlaps entry ");
                    serial_early_print_uint(j);
                    serial_early_print(" -- skipping\n");
                    g_boot_info_ptr->mmap_quirks = 1;
                    overlap = 1;
                    break;
                }
            }
            if (overlap) continue;
        }

        /* ---- Descriptor accepted ---- */

        g_boot_info_ptr->mmap[idx].base_addr = desc->PhysicalStart;
        g_boot_info_ptr->mmap[idx].length =
            desc->NumberOfPages * EFI_PAGE_SIZE;
        g_boot_info_ptr->mmap[idx].type =
            uefi_to_mb2_memtype(desc->Type);
        g_boot_info_ptr->mmap[idx].uefi_memory_type = desc->Type;
        g_boot_info_ptr->mmap[idx].attribute = desc->Attribute;

        /* Count all usable RAM (conventional + reclaimable boot/loader memory) */
        switch (desc->Type) {
        case EfiConventionalMemory:
        case EfiBootServicesCode:
        case EfiBootServicesData:
        case EfiLoaderCode:
        case EfiLoaderData:
            total_mem += desc->NumberOfPages * EFI_PAGE_SIZE;
            break;
        default:
            break;
        }

        idx++;
    }

    g_boot_info_ptr->mmap_count = idx;
    g_boot_info_ptr->mem_lower_kb = 640;   /* conventional: 640 KiB */
    g_boot_info_ptr->mem_upper_kb =
        (UINT32)((total_mem / 1024) - 1024);
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
 * Pre-ExitBootServices USB Device Discovery (TODO-07 §5 Phase A)
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
 * TODO-09 §1: Allocate persistent xHCI DMA structures
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
 * TODO-09 §2: USBLEGSUP handoff + controller takeover
 *
 * Take xHCI ownership from BIOS/firmware, halt the controller, reset it,
 * configure it to use our persistent DMA structures (from §1), and start it.
 * After this, the controller runs with our DCBAA/rings and is ready for §3
 * device enumeration.
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
#define POST16_BL_USB_DISC      0xB080
#define POST16_BL_USB_DISC_OK   0xB081
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

    /* S13: Read previous boot error from NVRAM before zeroing boot_info.
     * RuntimeServices->GetVariable is available before ExitBootServices. */
    {
        UINT32 prev_error = nvram_read_boot_error();
        if (prev_error != 0) {
            serial_early_print("[BOOT] Previous boot failed: code=0x");
            serial_early_print_hex16((UINT16)prev_error);
            serial_early_print("\n");
        }

        /* Point boot_info to a known physical address */
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

    /* Step 1b: Parse boot.conf first -- Resolution= key needed by init_gop */
    g_boot_info_ptr->timing.conf_start = boot_rdtsc();
    parse_boot_conf();
    g_boot_info_ptr->timing.conf_end = boot_rdtsc();

    /* Step 1: Initialize graphics (uses g_conf_res_width/height from boot.conf) */
    post_code16(POST16_BL_GOP);
    watchdog_reset();
    serial_early_print("[BOOT] init_gop...\n");
    g_boot_info_ptr->timing.gop_start = boot_rdtsc();
    status = init_gop();
    if (EFI_ERROR(status)) {
        serial_early_print("[FAIL] init_gop\n");
        efi_print(u"[FAIL] Graphics initialization failed\r\n");
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

    /* Step 4e: Discover USB devices via UEFI firmware (TODO-07 §5 Phase A) */
    post_code16(POST16_BL_USB_DISC);
    watchdog_reset();
    serial_early_print("[BOOT] discover_usb_devices...\n");
    discover_usb_devices();
    post_code16(POST16_BL_USB_DISC_OK);

    /* Step 4f: Allocate persistent xHCI DMA structures (TODO-09 §1) */
    post_code16(0xB082);
    serial_early_print("[BOOT] allocate_xhci_dma...\n");
    allocate_xhci_dma();
    post_code16(0xB083);

    /* Step 4g: Take over xHCI controller (TODO-09 §2) */
    post_code16(0xB084);
    serial_early_print("[BOOT] xhci_controller_takeover...\n");
    xhci_controller_takeover();
    post_code16(0xB085);

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
     * Locked at 4 per TODO-02-bootloader-error-recovery.md S2 agreement. */
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

    /* Step 8: Jump to kernel! */
    post_code16(POST16_BL_KERNEL_JUMP);
    serial_early_print("[BOOT] jumping to kernel_main\n");
    g_boot_info_ptr->timing.kernel_jump = boot_rdtsc();
    jump_to_kernel(kernel_entry);

    /* Never reached */
    return EFI_SUCCESS;
}
