/* ============================================================================
 * boot_info.h -- Parsed boot information passed to the kernel
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* Maximum number of memory map entries we store.
 * Real hardware (especially laptops with NVRAM, MMIO, etc.) can have 100+
 * descriptors.  256 is generous and still fits comfortably at 0x10000. */
#define BOOT_MMAP_MAX_ENTRIES 512

/* UEFI memory type constants (matches EFI_MEMORY_TYPE enum 0–14).
 * Defined here so kernel code can reference them without UEFI headers. */
#define UEFI_MMAP_RESERVED              0   /* EfiReservedMemoryType */
#define UEFI_MMAP_LOADER_CODE           1   /* EfiLoaderCode */
#define UEFI_MMAP_LOADER_DATA           2   /* EfiLoaderData */
#define UEFI_MMAP_BOOT_SERVICES_CODE    3   /* EfiBootServicesCode */
#define UEFI_MMAP_BOOT_SERVICES_DATA    4   /* EfiBootServicesData */
#define UEFI_MMAP_RUNTIME_CODE          5   /* EfiRuntimeServicesCode */
#define UEFI_MMAP_RUNTIME_DATA          6   /* EfiRuntimeServicesData */
#define UEFI_MMAP_CONVENTIONAL          7   /* EfiConventionalMemory */
#define UEFI_MMAP_UNUSABLE              8   /* EfiUnusableMemory */
#define UEFI_MMAP_ACPI_RECLAIM          9   /* EfiACPIReclaimMemory */
#define UEFI_MMAP_ACPI_NVS             10   /* EfiACPIMemoryNVS */
#define UEFI_MMAP_MMIO                 11   /* EfiMemoryMappedIO */
#define UEFI_MMAP_MMIO_PORT            12   /* EfiMemoryMappedIOPortSpace */
#define UEFI_MMAP_PAL_CODE             13   /* EfiPalCode */
#define UEFI_MMAP_PERSISTENT           14   /* EfiPersistentMemory */

/* ---- boot_info ABI header (S15) ----
 * Fixed at offset 0 of struct boot_info. The bootloader fills magic, version,
 * and size before kernel handoff. The kernel validates these before memcpy
 * to detect stale BOOTX64.EFI / kernel.exe mismatches.
 *
 * ABI rules:
 *   - Never reorder fields in boot_info_header.
 *   - Bump BOOT_INFO_VERSION when adding/removing/reordering fields in
 *     struct boot_info (not for adding fields to _reserved regions).
 *   - Always rebuild BOOTX64.EFI and kernel.exe together after a bump.
 *   - Bootloader writes sizeof(struct boot_info) into header.size at
 *     compile time -- a size mismatch means the structs diverged. */
#define BOOT_INFO_MAGIC    0x49504F53  /* "IPOS" (Impossible OS) */
#define BOOT_INFO_VERSION  1           /* bump on struct layout changes */

struct boot_info_header {
    uint32_t magic;     /* must be BOOT_INFO_MAGIC */
    uint16_t version;   /* BOOT_INFO_VERSION */
    uint16_t size;      /* sizeof(struct boot_info) as seen by the bootloader */
};

/* A single memory region from the bootloader */
struct boot_mmap_entry {
    uint64_t base_addr;
    uint64_t length;
    uint32_t type;              /* simplified: 1=available, 2=reserved, 3=ACPI, 4=NVS, 5=bad */
    uint32_t uefi_memory_type;  /* original EFI_MEMORY_TYPE (UEFI_MMAP_* constants above) */
    uint64_t attribute;         /* UEFI memory attribute flags (EFI_MEMORY_RUNTIME, etc.) */
};

/* --- UEFI Configuration Table ---
 * Copied from EFI_SYSTEM_TABLE.ConfigurationTable[] before ExitBootServices.
 * Each entry is a {GUID, VendorTable} pair pointing to platform data. */
#define BOOT_CONFIG_TABLE_MAX 32

struct boot_uefi_guid {
    uint32_t data1;
    uint16_t data2;
    uint16_t data3;
    uint8_t  data4[8];
};

struct boot_uefi_config_entry {
    struct boot_uefi_guid guid;
    uintptr_t             table_addr;   /* physical address of vendor table */
};

/* Well-known Configuration Table GUIDs (for kernel-side lookup) */
#define UEFI_GUID_ACPI_20 \
    ((struct boot_uefi_guid){ 0x8868e871, 0xe4f1, 0x11d3, \
        { 0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81 } })
#define UEFI_GUID_ACPI_10 \
    ((struct boot_uefi_guid){ 0xeb9d2d30, 0x2d88, 0x11d3, \
        { 0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d } })
#define UEFI_GUID_SMBIOS3 \
    ((struct boot_uefi_guid){ 0xf2fd1544, 0x9794, 0x4a2c, \
        { 0x99, 0x2e, 0xe5, 0xbb, 0xcf, 0x20, 0xe3, 0x94 } })
#define UEFI_GUID_SMBIOS \
    ((struct boot_uefi_guid){ 0xeb9d2d31, 0x2d88, 0x11d3, \
        { 0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d } })
#define UEFI_GUID_MEM_ATTR \
    ((struct boot_uefi_guid){ 0xdcfa911d, 0x26eb, 0x469f, \
        { 0xa2, 0x20, 0x38, 0xb7, 0xdc, 0x46, 0x12, 0x20 } })
#define UEFI_GUID_RT_PROPS \
    ((struct boot_uefi_guid){ 0xeb66918a, 0x7eef, 0x402a, \
        { 0x84, 0x2e, 0x93, 0x1d, 0x21, 0xc3, 0x8a, 0xe9 } })
#define UEFI_GUID_CONFORMANCE \
    ((struct boot_uefi_guid){ 0x36122546, 0xf7e7, 0x4c8f, \
        { 0xbd, 0x9b, 0xeb, 0x85, 0x25, 0xb5, 0x0c, 0x0b } })
#define UEFI_GUID_DTB \
    ((struct boot_uefi_guid){ 0xb1b621d5, 0xf19c, 0x41a5, \
        { 0x83, 0x0b, 0xd9, 0x15, 0x2c, 0x69, 0xaa, 0xe0 } })
#define UEFI_GUID_FPDT \
    ((struct boot_uefi_guid){ 0x564b1aaa, 0xafe3, 0x4b6c, \
        { 0x83, 0xa9, 0x27, 0x00, 0x80, 0x50, 0x01, 0x00 } })

/* UEFI memory attribute flag -- marks regions that survive ExitBootServices */
#define UEFI_MEMORY_ATTR_RUNTIME  0x8000000000000000ULL

/* Runtime memory region descriptor (for SetVirtualAddressMap) */
#define BOOT_RT_MMAP_MAX 64

struct boot_rt_mem_entry {
    uint64_t phys_addr;   /* physical start address */
    uint64_t num_pages;   /* number of 4 KiB pages */
    uint64_t attribute;   /* EFI memory attributes (cache type + EFI_MEMORY_RUNTIME) */
    uint32_t type;        /* EFI_MEMORY_TYPE (UEFI_MMAP_RUNTIME_CODE or _DATA) */
    uint32_t reserved;    /* alignment padding */
};

/* UEFI Runtime Service function pointers -- copied individually from
 * EFI_RUNTIME_SERVICES before ExitBootServices.  Stored as uintptr_t
 * because the bootloader (PE/COFF ms_abi) and kernel (ELF sysv) use
 * different calling conventions; the kernel casts at use time. */
struct boot_uefi_runtime {
    uintptr_t get_time;                   /* EFI_GET_TIME */
    uintptr_t set_time;                   /* EFI_SET_TIME */
    uintptr_t get_variable;               /* EFI_GET_VARIABLE */
    uintptr_t set_variable;               /* EFI_SET_VARIABLE */
    uintptr_t get_next_variable_name;     /* EFI_GET_NEXT_VARIABLE_NAME */
    uintptr_t reset_system;               /* EFI_RESET_SYSTEM */
    uintptr_t update_capsule;             /* EFI_UPDATE_CAPSULE */
    uintptr_t query_capsule_capabilities; /* EFI_QUERY_CAPSULE_CAPABILITIES */
    uintptr_t query_variable_info;        /* EFI_QUERY_VARIABLE_INFO */
    uintptr_t get_wakeup_time;            /* EFI_GET_WAKEUP_TIME */
    uintptr_t set_wakeup_time;            /* EFI_SET_WAKEUP_TIME */
    uint8_t   svam_called;                /* 1 if bootloader called SetVirtualAddressMap */
    uint8_t   pad[7];
};

/* GOP pixel format constants */
#define GOP_PIXEL_RGBX   0   /* PixelRedGreenBlueReserved8BitPerColor */
#define GOP_PIXEL_BGRX   1   /* PixelBlueGreenRedReserved8BitPerColor */
#define GOP_PIXEL_BITMASK 2  /* PixelBitMask (custom channel masks) */

/* Framebuffer information from the bootloader */
struct boot_framebuffer {
    uintptr_t addr;         /* physical address */
    uint32_t pitch;         /* bytes per scanline */
    uint32_t width;         /* pixels */
    uint32_t height;        /* pixels */
    uint8_t  bpp;           /* bits per pixel */
    uint8_t  type;          /* 0=indexed, 1=RGB, 2=EGA text */
    uint8_t  pixel_format;  /* GOP_PIXEL_RGBX/BGRX/BITMASK */
    uint8_t  pad0;
};

/* GOP mode entry (enumerated before ExitBootServices) */
#define BOOT_GOP_MODE_MAX  32

struct boot_gop_mode {
    uint32_t width;
    uint32_t height;
    uint32_t pixels_per_scanline;
    uint8_t  pixel_format;  /* GOP_PIXEL_* */
    uint8_t  pad[3];
};

/* Boot configuration from \EFI\ImpossibleOS\boot.conf */
#define BOOT_CONF_CMDLINE_MAX 256

/* Boot configuration layout (shared between UEFI bootloader and kernel).
 *
 * WARNING: The UEFI bootloader (bootx64.c) has a MIRROR of this struct
 * using UEFI types. Both definitions MUST stay in sync field-for-field.
 *
 * Field offset table (assembly/ABI contract):
 *   Offset  Size  Field
 *   ------  ----  ---------------
 *     0       1   debug
 *     1       1   verbose
 *     2       1   serial_debug
 *     3       1   boot_mode
 *     4       2   splash_timeout
 *     6       1   heartbeat
 *     7       1   postcode
 *     8       1   postbars
 *     9       1   test
 *    10       1   test_suite
 *    11       1   test_quiet
 *    12       1   diag_delay
 *    13       1   diag_splash
 *    14       1   deferred
 *    15       1   async_init
 *    16       1   crash_test
 *    17      15   _reserved[]
 *    32     256   cmdline          <-- STABLE ABI offset
 *   288       1   config_found
 *   289     223   _pad[]
 *   ---     ---
 *   512 total (sector-aligned)
 */
struct boot_config {
    /* Core */
    uint8_t  debug;            /* 1 = debug mode on (live flush to B:\) */
    uint8_t  verbose;          /* 1 = text mode, skip splash */
    uint8_t  serial_debug;     /* 1 = serial COM1 output */
    uint8_t  boot_mode;        /* 0=normal, 1=safe, 2=recovery */
    uint16_t splash_timeout;   /* seconds (0 = no timeout) */
    /* Kernel Heartbeat */
    uint8_t  heartbeat;        /* 0=off, 1=auto, 2=always */
    uint8_t  postcode;         /* 0=off, 1=auto, 2=always */
    /* VPD (Visual POST Display) */
    uint8_t  postbars;         /* 0=off, 1=on (integrated), 2=diag (full) */
    /* Test mode */
    uint8_t  test;             /* 1 = run unit tests only, then shutdown */
    uint8_t  test_suite;       /* category filter: 0-8 = specific, 0xFF = all (default) */
    uint8_t  test_quiet;       /* 1 = suppress PASS lines, show FAIL + summary only */
    /* Debug diagnostics */
    uint8_t  diag_delay;       /* seconds to pause on each diag screen (0 = skip) */
    uint8_t  diag_splash;      /* 1 = show diag on splash (bare metal, no serial) */
    /* Deferred init */
    uint8_t  deferred;         /* 1 = defer non-critical inits (default), 0 = all in-phase */
    /* Async init */
    uint8_t  async_init;       /* 1 = parallel subsystem init on APs, 0 = sequential (default) */
    /* Crash test */
    uint8_t  crash_test;       /* 1 = trigger deliberate BSOD after desktop init */
    /* Object Manager tracing (S15) */
    uint8_t  ob_handle_trace;  /* 1 = log every handle alloc/free to klog */
    /* Config version (S7) -- future boot.conf changes can key on this */
    uint8_t  config_version;   /* 0 = unversioned (legacy), 1+ = versioned */
    /* Error screen test (S14) -- trigger boot_fatal from bootloader for QR/BSOD testing */
    uint8_t  error_screen_test; /* 1 = call boot_fatal() before kernel load */
    /* Reserved -- new config fields go here without shifting cmdline.
     * Bootloader zero-fills the entire struct, so new fields default to 0
     * in older bootloaders that don't know about them. */
    uint8_t  _reserved[12];
    /* Command line (offset 32 -- stable across versions) */
    char     cmdline[BOOT_CONF_CMDLINE_MAX];
    /* Status */
    uint8_t  config_found;     /* 1 if boot.conf was successfully parsed */
    /* Pad to 512 bytes total (sector-aligned). */
    uint8_t  _pad[223];
};

/* Compile-time enforcement of bootloader ABI contract.
 * If you add a field, shrink _reserved[] to keep cmdline at offset 32. */
_Static_assert(__builtin_offsetof(struct boot_config, cmdline) == 32,
    "cmdline must be at byte offset 32 -- bootloader ABI contract");
_Static_assert(__builtin_offsetof(struct boot_config, config_found) == 288,
    "config_found must be at byte offset 288 -- after cmdline[256]");
_Static_assert(sizeof(struct boot_config) == 512,
    "boot_config must be exactly 512 bytes (sector-aligned)");

/* USB device discovered by UEFI firmware before ExitBootServices.
 * The bootloader uses EFI_USB_IO_PROTOCOL to enumerate all USB devices while
 * firmware is active, then passes this inventory to the kernel so it can skip
 * re-enumeration after taking over the xHCI controller. */
#define BOOT_USB_MAX_DEVICES     16
#define BOOT_USB_MAX_ENDPOINTS    4

struct boot_usb_endpoint {
    uint8_t  address;       /* bEndpointAddress (bit 7 = direction: 1=IN) */
    uint8_t  attributes;    /* bmAttributes (bits 1:0 = transfer type) */
    uint16_t max_packet;    /* wMaxPacketSize */
    uint8_t  interval;      /* bInterval (polling interval) */
    uint8_t  pad[3];
};

struct boot_usb_device {
    uint8_t  active;            /* 1 if entry is valid */
    uint8_t  port;              /* root hub port (1-based, from device path) */
    uint8_t  speed;             /* USB_SPEED_*: 1=FS, 2=LS, 3=HS, 4=SS */
    uint8_t  device_class;      /* bDeviceClass from device descriptor */
    uint8_t  iface_class;       /* bInterfaceClass of primary interface */
    uint8_t  iface_subclass;    /* bInterfaceSubClass */
    uint8_t  iface_protocol;    /* bInterfaceProtocol */
    uint8_t  num_endpoints;     /* number of populated entries in endpoints[] */
    uint16_t vendor_id;         /* idVendor */
    uint16_t product_id;        /* idProduct */
    /* MSC geometry (populated if iface_class == 0x08) */
    uint8_t  is_msc;            /* 1 if MSC BOT interface found */
    uint8_t  is_hid;            /* 1 if HID interface found */
    uint16_t pad0;
    uint32_t block_size;        /* bytes per sector (from EFI_BLOCK_IO_MEDIA) */
    uint64_t block_count;       /* total sectors (LastBlock + 1) */
    /* Endpoints (bulk-in, bulk-out, interrupt-in, etc.) */
    struct boot_usb_endpoint endpoints[BOOT_USB_MAX_ENDPOINTS];
};

/* xHCI controller state allocated by bootloader in EfiLoaderData memory.
 * Survives ExitBootServices.  Kernel must call pmm_mark_region_used() for
 * each non-zero physical address to prevent PMM from reclaiming them.
 * Set usb_handover_complete = 1 when all structures are valid. */
#define BOOT_USB_MAX_SCRATCHPADS 16
#define BOOT_USB_MAX_DMA_PAGES   16  /* max pages to reserve in PMM */

struct boot_usb_controller {
    /* PCI identity */
    uint8_t  pci_bus;
    uint8_t  pci_dev;
    uint8_t  pci_func;
    uint8_t  active;             /* 1 if controller was found and configured */

    /* MMIO base (physical -- kernel must remap via vmm_map_mmio_uc) */
    uint64_t mmio_phys;
    uint32_t mmio_size;

    /* Capability register cache */
    uint8_t  cap_length;
    uint16_t hci_version;
    uint32_t max_slots;
    uint32_t max_intrs;
    uint32_t max_ports;
    uint32_t db_offset;
    uint32_t rts_offset;
    uint8_t  ac64;               /* 64-bit addressing */
    uint8_t  csz;                /* context size: 0=32B, 1=64B */
    uint32_t max_scratchpads;

    /* DMA structure physical addresses (allocated as EfiLoaderData pages) */
    uint64_t dcbaa_phys;         /* DCBAA: (max_slots+1) × 8B, 64B aligned */
    uint64_t scratchpad_array_phys;
    uint64_t scratchpad_base_phys; /* contiguous scratchpad pages base */
    uint32_t scratchpad_page_count; /* actual pages allocated */
    uint32_t scratchpad_pad;
    uint64_t cmd_ring_phys;      /* Command Ring: 256 TRBs × 16B = 4 KiB */
    uint64_t evt_ring_phys;      /* Event Ring: 256 TRBs × 16B = 4 KiB */
    uint64_t erst_phys;          /* ERST: 1 entry × 16B (1 page) */

    /* Page tracking for kernel PMM reservation */
    uint64_t dma_pages[BOOT_USB_MAX_DMA_PAGES];
    uint32_t dma_page_count;     /* number of valid entries in dma_pages[] */
    uint32_t alloc_fail_status;  /* DEBUG: last AllocatePages failure EFI_STATUS (low 32 bits) */
    uint32_t alloc_fail_page;    /* DEBUG: which page# failed */
};

/* All boot info collected from UEFI bootloader */
struct boot_info {
    /* ABI header -- must be at offset 0 (S15) */
    struct boot_info_header header;

    /* Memory map */
    struct boot_mmap_entry mmap[BOOT_MMAP_MAX_ENTRIES];
    uint32_t mmap_count;
    uint8_t  mmap_truncated;    /* 1 if firmware had more entries than BOOT_MMAP_MAX_ENTRIES */
    uint8_t  mmap_quirks;       /* 1 if any descriptors had validation warnings (S12) */
    uint8_t  _mmap_pad[2];

    /* Basic memory (from tag type 4) */
    uint32_t mem_lower_kb;  /* conventional memory in KiB */
    uint32_t mem_upper_kb;  /* extended memory in KiB */

    /* Framebuffer */
    struct boot_framebuffer fb;
    uint8_t  fb_available;  /* 1 if framebuffer tag was found */
    uint8_t  hidpi;         /* 1 if negotiated GOP width >= 2560 (set by bootloader) */
    uint8_t  fb_pad[2];     /* alignment padding */

    /* GOP mode list (enumerated by bootloader) */
    struct boot_gop_mode gop_modes[BOOT_GOP_MODE_MAX];
    uint32_t gop_mode_count;     /* number of valid entries */
    uint32_t gop_mode_selected;  /* index of currently active mode */

    /* ACPI */
    uintptr_t acpi_rsdp_addr;   /* physical address of RSDP */
    uint8_t  acpi_version;      /* 1 = RSDP v1, 2 = RSDP v2 */
    uint8_t  acpi_available;    /* 1 if ACPI tag was found */

    /* Module (GRUB module) */
    uintptr_t module_start;     /* physical address of first module */
    uintptr_t module_end;       /* physical address of end of module */
    uint8_t   module_available; /* 1 if a module was loaded */

    /* Boot configuration (parsed from boot.conf) */
    struct boot_config config;

    /* UEFI Configuration Table (copied from EFI_SYSTEM_TABLE) */
    struct boot_uefi_config_entry config_table[BOOT_CONFIG_TABLE_MAX];
    uint32_t config_table_count;

    /* UEFI Runtime Services (for kernel to call after ExitBootServices) */
    uintptr_t uefi_runtime_services;  /* phys addr of EFI_RUNTIME_SERVICES table */
    uint8_t   uefi_rt_available;      /* 1 if runtime services pointer was saved */
    struct boot_rt_mem_entry rt_mmap[BOOT_RT_MMAP_MAX]; /* runtime memory regions */
    uint32_t rt_mmap_count;           /* number of runtime regions */
    uint32_t uefi_mmap_desc_size;     /* UEFI descriptor size (for SVAM) */
    uint32_t uefi_mmap_desc_version;  /* UEFI descriptor version (for SVAM) */
    struct boot_uefi_runtime uefi_runtime; /* individual RT function pointers */

    /* TPM Measured Boot (event log from EFI_TCG2_PROTOCOL) */
    uintptr_t tpm_event_log;          /* phys addr of copied event log buffer */
    uint32_t  tpm_event_log_size;     /* size of event log in bytes */
    uint8_t   tpm_available;          /* 1 if TPM was detected */
    uint8_t   tpm_version;            /* 0=none, 1=TPM 1.2, 2=TPM 2.0 */
    uint16_t  tpm_event_count;        /* number of events in log */

    /* USB devices discovered by UEFI firmware before ExitBootServices */
    struct boot_usb_device usb_devices[BOOT_USB_MAX_DEVICES];
    uint32_t usb_device_count;      /* number of valid entries */
    uint8_t  usb_discovery_ok;      /* 1 if USB discovery completed successfully */
    uint8_t  usb_handover_complete; /* 1 if bootloader allocated DMA + configured controller */
    uint8_t  usb_pad[2];

    /* xHCI controller DMA state (allocated by bootloader in EfiLoaderData) */
    struct boot_usb_controller usb_controller;

    /* Boot Timing (TSC timestamps from bootloader + FPDT) */
    struct {
        /* FPDT firmware performance record (nanoseconds, from firmware) */
        uint64_t reset_end;               /* SEC phase complete */
        uint64_t os_loader_load_start;    /* bootloader load began */
        uint64_t os_loader_start_start;   /* bootloader started executing */
        uint64_t exit_bs_entry;           /* ExitBootServices called */
        uint64_t exit_bs_exit;            /* ExitBootServices returned */
        uint8_t  fpdt_available;          /* 1 if FPDT was found */
        /* Bootloader phase timestamps (TSC ticks via rdtsc) */
        uint64_t bl_entry;                /* efi_main entered */
        uint64_t gop_start;               /* init_gop start */
        uint64_t gop_end;                 /* init_gop end */
        uint64_t conf_start;              /* parse_boot_conf start */
        uint64_t conf_end;                /* parse_boot_conf end */
        uint64_t kernel_load_start;       /* load_kernel start */
        uint64_t kernel_load_end;         /* load_kernel end */
        uint64_t splash_start;            /* boot_splash start */
        uint64_t splash_end;              /* boot_splash end */
        uint64_t exit_bs;                 /* just before ExitBootServices */
        uint64_t kernel_jump;             /* just before jumping to kernel */
        uint64_t tsc_freq;                /* TSC frequency in Hz (0 = unknown) */
    } timing;

    /* Serial port (probed by bootloader; 0 = no UART detected) */
    uint16_t serial_port;           /* I/O base: 0x3F8 (COM1), 0x2F8 (COM2), or 0 */
    uint8_t  serial_source;         /* 0=none, 1=SPCR, 2=I/O-probe */
    uint8_t  _serial_pad;
    uint32_t serial_baud;           /* baud rate from SPCR (0 = use default 38400) */

    /* NVRAM boot error from previous boot (S13); 0 = last boot OK */
    uint32_t last_boot_error;

    /* Kernel-populated fields (set after boot; never written by the bootloader) */
    uint8_t  secure_boot_enabled;   /* 1 if Secure Boot is active (uefi_secureboot_init) */
    uint8_t  _kp_pad[3];            /* alignment */
    uint32_t degraded_mask;         /* bitmask of non-critical subsystems that failed init */
    uint32_t hv_flags;              /* hypervisor feature flags (HV_FLAG_*) */
    char     hv_vendor[16];         /* hypervisor vendor string (null-terminated) */
};

/* Compile-time enforcement of ABI header layout (S15) */
_Static_assert(__builtin_offsetof(struct boot_info, header) == 0,
    "boot_info_header must be at offset 0 -- ABI contract");
_Static_assert(sizeof(struct boot_info_header) == 8,
    "boot_info_header must be exactly 8 bytes -- ABI contract");

/* Global boot info -- populated by multiboot2_parse() or UEFI bootloader */
extern struct boot_info g_boot_info;
