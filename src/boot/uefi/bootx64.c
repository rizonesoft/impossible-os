/* ============================================================================
 * bootx64.c — Custom UEFI Boot Application for Impossible OS
 *
 * Entry point: efi_main(EFI_HANDLE, EFI_SYSTEM_TABLE*)
 * Compiled as a PE/COFF binary, placed at \EFI\BOOT\BOOTX64.EFI
 *
 * Responsibilities:
 *   1. Enumerate GOP modes and select highest-resolution 32bpp mode
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
#define BOOT_MMAP_MAX_ENTRIES 64

struct boot_mmap_entry {
    UINT64 base_addr;
    UINT64 length;
    UINT32 type;    /* 1=available, 2=reserved, 3=ACPI, 4=NVS, 5=bad */
};

struct boot_framebuffer {
    UINT64  addr;
    UINT32  pitch;
    UINT32  width;
    UINT32  height;
    UINT8   bpp;
    UINT8   type;
};

struct boot_info {
    struct boot_mmap_entry  mmap[BOOT_MMAP_MAX_ENTRIES];
    UINT32  mmap_count;
    UINT32  mem_lower_kb;
    UINT32  mem_upper_kb;
    struct boot_framebuffer fb;
    UINT8   fb_available;
    UINT64  acpi_rsdp_addr;
    UINT8   acpi_version;
    UINT8   acpi_available;
    UINT64  module_start;
    UINT64  module_end;
    UINT8   module_available;
};

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

/* Boot info — placed at a known physical address (64 KiB) */
#define BOOT_INFO_PHYS_ADDR  0x10000
static struct boot_info    *g_boot_info_ptr;

/* Framebuffer for splash */
static UINT32 *gFramebuffer;
static UINT32  gFbWidth;
static UINT32  gFbHeight;
static UINT32  gFbPitch;  /* in pixels */

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
 * Enumerate all available modes, filter for 32bpp BGRA/RGBA formats, and select
 * the highest resolution (largest pixel count = width × height) that produces a
 * valid (non-zero) framebuffer after SetMode().
 *
 * Why the fallback loop?  Some UEFI firmware (OVMF in QEMU/VirtualBox) reports
 * very large GOP modes but returns a zero FrameBufferBase after SetMode — i.e.
 * the mode is logically valid but the virtual GPU can't back it with real VRAM.
 * We sort modes by pixel count descending and try each in turn until we get a
 * non-zero framebuffer address.  The first working mode wins.
 * ============================================================================ */

/* Helper: efi_print a decimal number (used for resolution logging) */
static void efi_print_dec(UINT32 val)
{
    CHAR16 buf[12];
    int i = 10;
    buf[11] = 0;
    if (val == 0) {
        efi_print(u"0");
        return;
    }
    while (val > 0 && i >= 0) {
        buf[i--] = (CHAR16)('0' + (val % 10));
        val /= 10;
    }
    efi_print(&buf[i + 1]);
}

/* Simple insertion-sort: sort mode_ids by pixel_counts descending (N ≤ 64) */
#define MAX_CANDIDATE_MODES 64

static void sort_modes_desc(UINT32 *mode_ids, UINT32 *pixel_counts, UINT32 n)
{
    UINT32 i, j, tmp_id, tmp_px;
    for (i = 1; i < n; i++) {
        tmp_id = mode_ids[i];
        tmp_px = pixel_counts[i];
        j = i;
        while (j > 0 && pixel_counts[j - 1] < tmp_px) {
            mode_ids[j]    = mode_ids[j - 1];
            pixel_counts[j] = pixel_counts[j - 1];
            j--;
        }
        mode_ids[j]    = tmp_id;
        pixel_counts[j] = tmp_px;
    }
}

static EFI_STATUS init_gop(void)
{
    EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
    EFI_STATUS status;
    UINT32 i, n_candidates;
    UINT32 candidate_ids[MAX_CANDIDATE_MODES];
    UINT32 candidate_px[MAX_CANDIDATE_MODES];

    status = gBS->LocateProtocol(&gop_guid, (VOID *)0, (VOID **)&gop);
    if (EFI_ERROR(status)) {
        efi_print(u"[FAIL] GOP not found\r\n");
        return status;
    }

    /* Pass 1: collect all 32bpp packed-pixel modes into a candidate list */
    n_candidates = 0;
    for (i = 0; i < gop->Mode->MaxMode && n_candidates < MAX_CANDIDATE_MODES; i++) {
        UINTN info_size;
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info;

        status = gop->QueryMode(gop, i, &info_size, &info);
        if (EFI_ERROR(status))
            continue;

        /* Only accept 32bpp packed-pixel layouts the kernel can write directly */
        if (info->PixelFormat != PixelBlueGreenRedReserved &&
            info->PixelFormat != PixelRedGreenBlueReserved)
            continue;

        candidate_ids[n_candidates] = i;
        candidate_px[n_candidates]  = info->HorizontalResolution *
                                      info->VerticalResolution;
        n_candidates++;
    }

    /* Sort candidates highest-resolution first */
    if (n_candidates > 1)
        sort_modes_desc(candidate_ids, candidate_px, n_candidates);

    /* Pass 2: try each candidate from highest to lowest until we get a
     * non-zero FrameBufferBase.  Some OVMF builds claim huge modes but
     * return base=0 after SetMode (VirtualBox, certain QEMU configs). */
    for (i = 0; i < n_candidates; i++) {
        status = gop->SetMode(gop, candidate_ids[i]);
        if (EFI_ERROR(status))
            continue;  /* mode failed — try next */

        if (gop->Mode->FrameBufferBase == 0)
            continue;  /* invalid framebuffer — try next */

        /* Working mode found */
        goto mode_set;
    }

    /* No candidate worked (or no 32bpp modes at all) — use current mode
     * as-is.  If FrameBufferBase is still 0 here, GOP is fundamentally
     * broken on this firmware and we cannot continue. */
    if (n_candidates == 0) {
        efi_print(u"[WARN] No 32bpp GOP mode found, using current mode\r\n");
        if (gop->Mode->FrameBufferBase == 0) {
            efi_print(u"[FAIL] GOP framebuffer base is 0 — cannot boot\r\n");
            return EFI_UNSUPPORTED;
        }
        goto mode_set;
    }

    /* All candidates had base=0 — try setting the current mode explicitly */
    efi_print(u"[WARN] All 32bpp modes returned base=0, using current mode\r\n");
    status = gop->SetMode(gop, gop->Mode->Mode);
    if (EFI_ERROR(status) || gop->Mode->FrameBufferBase == 0) {
        efi_print(u"[FAIL] GOP framebuffer base is 0 — cannot boot\r\n");
        return EFI_UNSUPPORTED;
    }

mode_set:
    /* Log chosen resolution to UEFI console */
    efi_print(u"[GOP] Mode ");
    efi_print_dec(gop->Mode->Mode);
    efi_print(u": ");
    efi_print_dec(gop->Mode->Info->HorizontalResolution);
    efi_print(u"x");
    efi_print_dec(gop->Mode->Info->VerticalResolution);
    efi_print(u" 32bpp selected\r\n");

    /* Cache framebuffer globals (used by boot splash drawing) */
    gFramebuffer = (UINT32 *)(UINTN)gop->Mode->FrameBufferBase;
    gFbWidth  = gop->Mode->Info->HorizontalResolution;
    gFbHeight = gop->Mode->Info->VerticalResolution;
    gFbPitch  = gop->Mode->Info->PixelsPerScanLine;   /* in pixels */

    /* Fill boot_info framebuffer — kernel uses these exclusively */
    g_boot_info_ptr->fb.addr   = (UINT64)gop->Mode->FrameBufferBase;
    g_boot_info_ptr->fb.pitch  = gop->Mode->Info->PixelsPerScanLine * 4; /* bytes */
    g_boot_info_ptr->fb.width  = gFbWidth;
    g_boot_info_ptr->fb.height = gFbHeight;
    g_boot_info_ptr->fb.bpp    = 32;
    g_boot_info_ptr->fb.type   = 1;    /* RGB direct colour */
    g_boot_info_ptr->fb_available = 1;

    return EFI_SUCCESS;
}

/* ============================================================================
 * Step 2: Fill screen black (kernel handles the real boot splash)
 * ============================================================================ */

/* Draw a filled rectangle */
static void draw_rect(UINT32 x, UINT32 y, UINT32 w, UINT32 h, UINT32 color)
{
    UINT32 row, col;
    for (row = y; row < y + h && row < gFbHeight; row++)
        for (col = x; col < x + w && col < gFbWidth; col++)
            gFramebuffer[row * gFbPitch + col] = color;
}

static void fill_screen_black(void)
{
    draw_rect(0, 0, gFbWidth, gFbHeight, 0x000000);
}

/* ============================================================================
 * Step 3: Load kernel ELF from FAT32
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

    /* Locate file system protocol */
    status = gBS->LocateProtocol(&fs_guid, (VOID *)0, (VOID **)&fs);
    if (EFI_ERROR(status)) {
        efi_print(u"[FAIL] File system protocol not found\r\n");
        return status;
    }

    /* Open root directory */
    status = fs->OpenVolume(fs, &root_dir);
    if (EFI_ERROR(status)) {
        efi_print(u"[FAIL] Cannot open root volume\r\n");
        return status;
    }

    /* Open kernel file */
    status = root_dir->Open(
        root_dir, &kernel_file,
        u"\\boot\\kernel.exe",
        EFI_FILE_MODE_READ, 0
    );
    if (EFI_ERROR(status)) {
        efi_print(u"[FAIL] Cannot open \\boot\\kernel.exe\r\n");
        return status;
    }

    /* Read entire file into memory */
    /* First read: get file size by reading a large chunk */
    file_size = 0;

    /* Allocate generous buffer for kernel (16 MiB should be plenty) */
    {
        UINTN pages = (16 * 1024 * 1024) / EFI_PAGE_SIZE;
        EFI_PHYSICAL_ADDRESS buf_addr;
        status = gBS->AllocatePages(AllocateAnyPages, EfiLoaderData,
                                     pages, &buf_addr);
        if (EFI_ERROR(status)) {
            efi_print(u"[FAIL] Cannot allocate memory for kernel\r\n");
            return status;
        }
        file_buf = (UINT8 *)(UINTN)buf_addr;
        file_size = 16 * 1024 * 1024;
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
        efi_print(u"[FAIL] Invalid ELF64 kernel\r\n");
        return EFI_LOAD_ERROR;
    }

    /* Load PT_LOAD segments */
    phdr = (Elf64_Phdr *)(file_buf + ehdr->e_phoff);
    for (i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type != PT_LOAD)
            continue;

        /* Copy segment to its physical address */
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
                                  UINTN *desc_size_out)
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

    for (offset = 0; offset < map_size && idx < BOOT_MMAP_MAX_ENTRIES;
         offset += desc_size) {
        EFI_MEMORY_DESCRIPTOR *desc =
            (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)mmap + offset);

        g_boot_info_ptr->mmap[idx].base_addr = desc->PhysicalStart;
        g_boot_info_ptr->mmap[idx].length =
            desc->NumberOfPages * EFI_PAGE_SIZE;
        g_boot_info_ptr->mmap[idx].type =
            uefi_to_mb2_memtype(desc->Type);

        if (g_boot_info_ptr->mmap[idx].type == 1)
            total_mem += g_boot_info_ptr->mmap[idx].length;

        idx++;
    }

    g_boot_info_ptr->mmap_count = idx;
    g_boot_info_ptr->mem_lower_kb = 640;   /* conventional: 640 KiB */
    g_boot_info_ptr->mem_upper_kb =
        (UINT32)((total_mem / 1024) - 1024);
}

/* ============================================================================
 * Step 5: Find ACPI RSDP from UEFI configuration tables
 * ============================================================================ */
static void find_acpi_rsdp(void)
{
    EFI_GUID acpi20_guid = EFI_ACPI_20_TABLE_GUID;
    EFI_GUID acpi10_guid = EFI_ACPI_TABLE_GUID;
    UINTN i;

    for (i = 0; i < gST->NumberOfTableEntries; i++) {
        EFI_CONFIGURATION_TABLE *entry = &gST->ConfigurationTable[i];

        if (guid_equal(&entry->VendorGuid, &acpi20_guid)) {
            g_boot_info_ptr->acpi_rsdp_addr = (UINT64)(UINTN)entry->VendorTable;
            g_boot_info_ptr->acpi_version = 2;
            g_boot_info_ptr->acpi_available = 1;
            return;  /* Prefer ACPI 2.0 */
        }

        if (guid_equal(&entry->VendorGuid, &acpi10_guid)) {
            g_boot_info_ptr->acpi_rsdp_addr = (UINT64)(UINTN)entry->VendorTable;
            g_boot_info_ptr->acpi_version = 1;
            g_boot_info_ptr->acpi_available = 1;
            /* Don't return — keep looking for ACPI 2.0 */
        }
    }
}

/* ============================================================================
 * Step 6: Set up page tables (identity map first 4 GiB with 2 MiB pages)
 * ============================================================================ */

/* Page table physical addresses — allocate 6 pages at 0x70000 */
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
 * Step 7: Jump to kernel — switch to our page tables and call entry
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

    /* Call kernel — pass Multiboot2 magic + boot_info address.
     * We pass the UEFI-specific magic 0x55454649 ("UEFI") so the kernel
     * can detect which bootloader was used. */
    entry(0x55454649ULL, (UINT64)(UINTN)g_boot_info_ptr);

    /* Should never return */
    for (;;) __asm__ volatile("hlt");
}

/* ============================================================================
 * EFI Application Entry Point
 * ============================================================================ */
EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
    EFI_STATUS status;
    UINT64 kernel_entry;
    UINTN map_key;
    EFI_MEMORY_DESCRIPTOR *mmap;
    UINTN map_size, desc_size;

    /* Save globals */
    gST = SystemTable;
    gBS = SystemTable->BootServices;
    gImageHandle = ImageHandle;

    /* Disable watchdog timer (UEFI default: 5 min timeout) */
    gBS->SetWatchdogTimer(0, 0, 0, (CHAR16 *)0);

    /* Point boot_info to a known physical address */
    g_boot_info_ptr = (struct boot_info *)BOOT_INFO_PHYS_ADDR;
    efi_memset(g_boot_info_ptr, 0, sizeof(struct boot_info));

    /* Step 1: Initialize graphics */
    status = init_gop();
    if (EFI_ERROR(status)) {
        efi_print(u"[FAIL] Graphics initialization failed\r\n");
        return status;
    }

    /* Step 2: Fill screen black (kernel draws the real splash after boot) */
    fill_screen_black();

    /* Step 3: Load kernel ELF */
    /* (No screen text — kernel boot splash handles all visuals) */
    status = load_kernel(&kernel_entry);
    if (EFI_ERROR(status)) {
        efi_print(u"[FAIL] Kernel load failed\r\n");
        return status;
    }

    /* Step 4: Find ACPI RSDP */
    find_acpi_rsdp();

    /* Step 5: Get UEFI memory map (must be done LAST before ExitBootServices) */
    status = get_memory_map(&map_key, &mmap, &map_size, &desc_size);
    if (EFI_ERROR(status)) {
        efi_print(u"[FAIL] GetMemoryMap failed\r\n");
        return status;
    }

    /* Convert UEFI memory map to boot_info format */
    fill_memory_map(mmap, map_size, desc_size);

    /* Step 6: ExitBootServices — no more UEFI calls after this! */
    status = gBS->ExitBootServices(gImageHandle, map_key);
    if (EFI_ERROR(status)) {
        /* Memory map may have changed — retry once */
        status = get_memory_map(&map_key, &mmap, &map_size, &desc_size);
        if (!EFI_ERROR(status)) {
            fill_memory_map(mmap, map_size, desc_size);
            status = gBS->ExitBootServices(gImageHandle, map_key);
        }
        if (EFI_ERROR(status)) {
            /* Fatal — can't exit boot services */
            for (;;) __asm__ volatile("hlt");
        }
    }

    /* === NO MORE UEFI CALLS FROM HERE === */

    /* Step 7: Set up our own identity-mapped page tables */
    setup_page_tables();

    /* Step 8: Jump to kernel! */
    jump_to_kernel(kernel_entry);

    /* Never reached */
    return EFI_SUCCESS;
}
