/* ============================================================================
 * boot_info.h — Parsed boot information passed to the kernel
 * ============================================================================ */

#pragma once

#include "types.h"

/* Maximum number of memory map entries we store */
#define BOOT_MMAP_MAX_ENTRIES 64

/* A single memory region from the bootloader */
struct boot_mmap_entry {
    uint64_t base_addr;
    uint64_t length;
    uint32_t type;          /* 1=available, 2=reserved, 3=ACPI, 4=NVS, 5=bad */
};

/* Framebuffer information from the bootloader */
struct boot_framebuffer {
    uint32_t addr;          /* physical address (32-bit for now) */
    uint32_t pitch;         /* bytes per scanline */
    uint32_t width;         /* pixels */
    uint32_t height;        /* pixels */
    uint8_t  bpp;           /* bits per pixel */
    uint8_t  type;          /* 0=indexed, 1=RGB, 2=EGA text */
};

/* All boot info collected from Multiboot2 */
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
    uint32_t acpi_rsdp_addr;    /* physical address of RSDP */
    uint8_t  acpi_version;      /* 1 = RSDP v1, 2 = RSDP v2 */
    uint8_t  acpi_available;    /* 1 if ACPI tag was found */
};

/* Global boot info — populated by multiboot2_parse() */
extern struct boot_info g_boot_info;
