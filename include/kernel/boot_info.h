/* ============================================================================
 * boot_info.h — Parsed boot information passed to the kernel
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* Maximum number of memory map entries we store.
 * Real hardware (especially laptops with NVRAM, MMIO, etc.) can have 100+
 * descriptors.  256 is generous and still fits comfortably at 0x10000. */
#define BOOT_MMAP_MAX_ENTRIES 256

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

/* UEFI memory attribute flag — marks regions that survive ExitBootServices */
#define UEFI_MEMORY_ATTR_RUNTIME  0x8000000000000000ULL

/* Runtime memory region descriptor (for SetVirtualAddressMap) */
#define BOOT_RT_MMAP_MAX 64

struct boot_rt_mem_entry {
    uint64_t phys_addr;   /* physical start address */
    uint64_t num_pages;   /* number of 4 KiB pages */
    uint32_t type;        /* EFI_MEMORY_TYPE (UEFI_MMAP_RUNTIME_CODE or _DATA) */
    uint32_t reserved;    /* alignment padding */
};

/* Framebuffer information from the bootloader */
struct boot_framebuffer {
    uintptr_t addr;         /* physical address */
    uint32_t pitch;         /* bytes per scanline */
    uint32_t width;         /* pixels */
    uint32_t height;        /* pixels */
    uint8_t  bpp;           /* bits per pixel */
    uint8_t  type;          /* 0=indexed, 1=RGB, 2=EGA text */
};

/* Boot configuration from \EFI\ImpossibleOS\boot.conf */
#define BOOT_CONF_CMDLINE_MAX 256

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
    /* Command line */
    char     cmdline[BOOT_CONF_CMDLINE_MAX];
    /* Status */
    uint8_t  config_found;     /* 1 if boot.conf was successfully parsed */
};

/* All boot info collected from UEFI bootloader */
struct boot_info {
    /* Memory map */
    struct boot_mmap_entry mmap[BOOT_MMAP_MAX_ENTRIES];
    uint32_t mmap_count;

    /* Basic memory (from tag type 4) */
    uint32_t mem_lower_kb;  /* conventional memory in KiB */
    uint32_t mem_upper_kb;  /* extended memory in KiB */

    /* Framebuffer */
    struct boot_framebuffer fb;
    uint8_t  fb_available;  /* 1 if framebuffer tag was found */

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
};

/* Global boot info — populated by multiboot2_parse() or UEFI bootloader */
extern struct boot_info g_boot_info;
