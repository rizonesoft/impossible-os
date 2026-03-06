/* ============================================================================
 * multiboot2.h — Multiboot2 tag structures and constants
 *
 * Reference: https://www.gnu.org/software/grub/manual/multiboot2/
 * ============================================================================ */

#pragma once

#include "types.h"

/* Multiboot2 bootloader magic (passed in eax) */
#define MULTIBOOT2_BOOTLOADER_MAGIC  0x36D76289

/* Tag types */
#define MULTIBOOT2_TAG_TYPE_END          0
#define MULTIBOOT2_TAG_TYPE_CMDLINE      1
#define MULTIBOOT2_TAG_TYPE_BOOTLOADER   2
#define MULTIBOOT2_TAG_TYPE_MODULE       3
#define MULTIBOOT2_TAG_TYPE_BASIC_MEMINFO 4
#define MULTIBOOT2_TAG_TYPE_BOOTDEV      5
#define MULTIBOOT2_TAG_TYPE_MMAP         6
#define MULTIBOOT2_TAG_TYPE_FRAMEBUFFER  8
#define MULTIBOOT2_TAG_TYPE_ELF_SECTIONS 9
#define MULTIBOOT2_TAG_TYPE_APM          10
#define MULTIBOOT2_TAG_TYPE_ACPI_OLD     14
#define MULTIBOOT2_TAG_TYPE_ACPI_NEW     15

/* Memory map entry types */
#define MULTIBOOT2_MEMORY_AVAILABLE       1
#define MULTIBOOT2_MEMORY_RESERVED        2
#define MULTIBOOT2_MEMORY_ACPI_RECLAIMABLE 3
#define MULTIBOOT2_MEMORY_NVS             4
#define MULTIBOOT2_MEMORY_BADRAM          5

/* Framebuffer types */
#define MULTIBOOT2_FRAMEBUFFER_TYPE_INDEXED  0
#define MULTIBOOT2_FRAMEBUFFER_TYPE_RGB      1
#define MULTIBOOT2_FRAMEBUFFER_TYPE_EGA_TEXT 2

/* --- Fixed-size info header --- */
struct multiboot2_info_header {
    uint32_t total_size;
    uint32_t reserved;
};

/* --- Generic tag header (all tags start with this) --- */
struct multiboot2_tag {
    uint32_t type;
    uint32_t size;
};

/* --- Memory map tag (type 6) --- */
struct multiboot2_mmap_entry {
    uint64_t addr;
    uint64_t len;
    uint32_t type;
    uint32_t zero;          /* reserved, must be 0 */
};

struct multiboot2_tag_mmap {
    uint32_t type;          /* = 6 */
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
    /* followed by mmap_entry[] */
};

/* --- Framebuffer tag (type 8) --- */
struct multiboot2_tag_framebuffer {
    uint32_t type;          /* = 8 */
    uint32_t size;
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t  framebuffer_bpp;
    uint8_t  framebuffer_type;
    uint8_t  reserved;
    /* color info follows for RGB type */
};

/* --- ACPI old RSDP tag (type 14) --- */
struct multiboot2_tag_acpi_old {
    uint32_t type;          /* = 14 */
    uint32_t size;
    /* followed by RSDP v1 structure */
};

/* --- ACPI new RSDP tag (type 15) --- */
struct multiboot2_tag_acpi_new {
    uint32_t type;          /* = 15 */
    uint32_t size;
    /* followed by RSDP v2 structure */
};

/* --- Basic memory info tag (type 4) --- */
struct multiboot2_tag_basic_meminfo {
    uint32_t type;          /* = 4 */
    uint32_t size;
    uint32_t mem_lower;     /* in KiB, max 640 */
    uint32_t mem_upper;     /* in KiB, starting at 1 MiB */
};
