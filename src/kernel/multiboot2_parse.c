/* ============================================================================
 * multiboot2_parse.c — Parse the Multiboot2 info structure
 *
 * Iterates over all Multiboot2 tags and extracts:
 *   - Memory map (type 6)
 *   - Framebuffer info (type 8)
 *   - ACPI RSDP (type 14/15)
 *   - Basic memory info (type 4)
 *
 * Results are stored in the global g_boot_info structure.
 * ============================================================================ */

#include "types.h"
#include "multiboot2.h"
#include "boot_info.h"

/* Global boot info — zero-initialized by BSS */
struct boot_info g_boot_info;

/* Helper: align address up to 8-byte boundary (Multiboot2 tags are 8-aligned) */
static inline uint32_t align_up_8(uint32_t addr)
{
    return (addr + 7) & ~((uint32_t)7);
}

void multiboot2_parse(uint32_t *mbi_addr)
{
    struct multiboot2_info_header *header;
    struct multiboot2_tag *tag;
    uint32_t mbi = (uint32_t)(uintptr_t)mbi_addr;

    header = (struct multiboot2_info_header *)mbi;

    /* First tag starts 8 bytes after the info header */
    tag = (struct multiboot2_tag *)(mbi + 8);

    while (tag->type != MULTIBOOT2_TAG_TYPE_END) {
        switch (tag->type) {

        /* --- Basic memory info (type 4) --- */
        case MULTIBOOT2_TAG_TYPE_BASIC_MEMINFO: {
            struct multiboot2_tag_basic_meminfo *mem;
            mem = (struct multiboot2_tag_basic_meminfo *)tag;
            g_boot_info.mem_lower_kb = mem->mem_lower;
            g_boot_info.mem_upper_kb = mem->mem_upper;
            break;
        }

        /* --- Memory map (type 6) --- */
        case MULTIBOOT2_TAG_TYPE_MMAP: {
            struct multiboot2_tag_mmap *mmap_tag;
            struct multiboot2_mmap_entry *entry;
            uint32_t offset;

            mmap_tag = (struct multiboot2_tag_mmap *)tag;
            g_boot_info.mmap_count = 0;

            /* Entries start after the tag header (16 bytes) */
            offset = 16;
            while (offset < mmap_tag->size &&
                   g_boot_info.mmap_count < BOOT_MMAP_MAX_ENTRIES) {
                entry = (struct multiboot2_mmap_entry *)
                        ((uint32_t)(uintptr_t)mmap_tag + offset);

                g_boot_info.mmap[g_boot_info.mmap_count].base_addr = entry->addr;
                g_boot_info.mmap[g_boot_info.mmap_count].length = entry->len;
                g_boot_info.mmap[g_boot_info.mmap_count].type = entry->type;
                g_boot_info.mmap_count++;

                offset += mmap_tag->entry_size;
            }
            break;
        }

        /* --- Framebuffer info (type 8) --- */
        case MULTIBOOT2_TAG_TYPE_FRAMEBUFFER: {
            struct multiboot2_tag_framebuffer *fb_tag;
            fb_tag = (struct multiboot2_tag_framebuffer *)tag;

            g_boot_info.fb.addr   = (uint32_t)fb_tag->framebuffer_addr;
            g_boot_info.fb.pitch  = fb_tag->framebuffer_pitch;
            g_boot_info.fb.width  = fb_tag->framebuffer_width;
            g_boot_info.fb.height = fb_tag->framebuffer_height;
            g_boot_info.fb.bpp    = fb_tag->framebuffer_bpp;
            g_boot_info.fb.type   = fb_tag->framebuffer_type;
            g_boot_info.fb_available = 1;
            break;
        }

        /* --- ACPI old RSDP (type 14) --- */
        case MULTIBOOT2_TAG_TYPE_ACPI_OLD: {
            /* RSDP data starts at offset 8 (after type + size) */
            g_boot_info.acpi_rsdp_addr = (uint32_t)(uintptr_t)tag + 8;
            g_boot_info.acpi_version = 1;
            g_boot_info.acpi_available = 1;
            break;
        }

        /* --- ACPI new RSDP (type 15) --- */
        case MULTIBOOT2_TAG_TYPE_ACPI_NEW: {
            g_boot_info.acpi_rsdp_addr = (uint32_t)(uintptr_t)tag + 8;
            g_boot_info.acpi_version = 2;
            g_boot_info.acpi_available = 1;
            break;
        }

        default:
            /* Ignore unknown tags */
            break;
        }

        /* Advance to next tag (aligned to 8 bytes) */
        tag = (struct multiboot2_tag *)
              align_up_8((uint32_t)(uintptr_t)tag + tag->size);
    }

    (void)header; /* suppress unused warning */
}
