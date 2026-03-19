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
};

/* Global boot info — populated by multiboot2_parse() or UEFI bootloader */
extern struct boot_info g_boot_info;
