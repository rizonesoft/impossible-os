/* ============================================================================
 * bootx64.c — Custom UEFI Boot Application for Impossible OS
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
 * ============================================================================ */
static EFI_STATUS init_gop(void)
{
    EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
    EFI_STATUS status;
    UINT32 i, best_mode = 0;
    BOOLEAN found = 0;

    status = gBS->LocateProtocol(&gop_guid, (VOID *)0, (VOID **)&gop);
    if (EFI_ERROR(status)) {
        efi_print(u"[FAIL] GOP not found\r\n");
        return status;
    }

    /* Find 1280×720 mode (or closest match) */
    for (i = 0; i < gop->Mode->MaxMode; i++) {
        UINTN info_size;
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info;

        status = gop->QueryMode(gop, i, &info_size, &info);
        if (EFI_ERROR(status)) continue;

        if (info->HorizontalResolution == 1280 &&
            info->VerticalResolution == 720 &&
            (info->PixelFormat == PixelBlueGreenRedReserved ||
             info->PixelFormat == PixelRedGreenBlueReserved)) {
            best_mode = i;
            found = 1;
            break;
        }
    }

    if (!found) {
        /* Fall back to current mode */
        best_mode = gop->Mode->Mode;
        efi_print(u"[--] 1280x720 not found, using current mode\r\n");
    }

    /* Set the mode */
    status = gop->SetMode(gop, best_mode);
    if (EFI_ERROR(status)) {
        efi_print(u"[FAIL] GOP SetMode failed\r\n");
        return status;
    }

    /* Store framebuffer info */
    gFramebuffer = (UINT32 *)(UINTN)gop->Mode->FrameBufferBase;
    gFbWidth  = gop->Mode->Info->HorizontalResolution;
    gFbHeight = gop->Mode->Info->VerticalResolution;
    gFbPitch  = gop->Mode->Info->PixelsPerScanLine;

    /* Fill boot_info framebuffer */
    g_boot_info_ptr->fb.addr   = (UINT64)gop->Mode->FrameBufferBase;
    g_boot_info_ptr->fb.pitch  = gop->Mode->Info->PixelsPerScanLine * 4;
    g_boot_info_ptr->fb.width  = gFbWidth;
    g_boot_info_ptr->fb.height = gFbHeight;
    g_boot_info_ptr->fb.bpp    = 32;
    g_boot_info_ptr->fb.type   = 1;  /* RGB direct color */
    g_boot_info_ptr->fb_available = 1;

    return EFI_SUCCESS;
}

/* ============================================================================
 * Step 2: Boot Splash — Render logo + spinner
 * ============================================================================ */

/* Draw a filled rectangle */
static void draw_rect(UINT32 x, UINT32 y, UINT32 w, UINT32 h, UINT32 color)
{
    UINT32 row, col;
    for (row = y; row < y + h && row < gFbHeight; row++)
        for (col = x; col < x + w && col < gFbWidth; col++)
            gFramebuffer[row * gFbPitch + col] = color;
}

/* Draw a filled circle (Bresenham) */
static void draw_circle(INT32 cx, INT32 cy, INT32 r, UINT32 color)
{
    INT32 x, y;
    for (y = -r; y <= r; y++)
        for (x = -r; x <= r; x++)
            if (x * x + y * y <= r * r) {
                INT32 px = cx + x, py = cy + y;
                if (px >= 0 && px < (INT32)gFbWidth &&
                    py >= 0 && py < (INT32)gFbHeight)
                    gFramebuffer[py * gFbPitch + px] = color;
            }
}

/* Simple "Impossible OS" text renderer (8×16 bitmap font embedded) */
/* We'll draw a simple centered text using box characters instead */
static void draw_text_centered(const char *text, UINT32 y, UINT32 color)
{
    /* Simple 5×7 pixel font for basic ASCII rendering */
    UINT32 len = 0;
    const char *p = text;
    while (*p++) len++;

    UINT32 char_w = 8;
    UINT32 total_w = len * char_w;
    UINT32 start_x = (gFbWidth - total_w) / 2;
    UINT32 i;

    /* We don't have a real font — just draw a subtle bar to indicate text
     * location. The real text will appear once kernel takes over. */
    (void)color;
    (void)text;
    draw_rect(start_x, y, total_w, 2, 0x404060);

    /* Suppress unused warnings */
    (void)i;
}

/* Render the boot splash screen */
static void render_splash(UINT32 spinner_frame)
{
    UINT32 cx = gFbWidth / 2;
    UINT32 cy = gFbHeight / 2;
    UINT32 i;

    /* Background: dark navy */
    draw_rect(0, 0, gFbWidth, gFbHeight, 0x1a1a2e);

    /* Spinning dots ring — 8 dots in a circle, radius 30px */
    /* The "active" dot is brighter, others dim */
    for (i = 0; i < 8; i++) {
        /* Angle: i * 45 degrees (precomputed sin/cos for 8 positions) */
        /* sin/cos * 30 for each of 8 positions */
        static const INT32 dx[8] = { 0, 21, 30, 21, 0, -21, -30, -21 };
        static const INT32 dy[8] = { -30, -21, 0, 21, 30, 21, 0, -21 };

        INT32 px = (INT32)cx + dx[i];
        INT32 py = (INT32)cy + dy[i];

        /* Active dot is bright white, others fade based on distance from active */
        UINT32 dist = ((i - spinner_frame % 8) + 8) % 8;
        UINT32 brightness;
        if (dist == 0)      brightness = 0xFF;
        else if (dist == 1) brightness = 0xC0;
        else if (dist == 2) brightness = 0x80;
        else if (dist == 3) brightness = 0x50;
        else                brightness = 0x30;

        UINT32 color = (brightness << 16) | (brightness << 8) | brightness;
        draw_circle(px, py, 4, color);
    }

    /* "Impossible OS" text below spinner */
    draw_text_centered("Impossible OS", cy + 60, 0xCCCCCC);
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

    *entry_point = ehdr->e_entry;
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
EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
    EFI_STATUS status;
    UINT64 kernel_entry;
    UINTN map_key;
    EFI_MEMORY_DESCRIPTOR *mmap;
    UINTN map_size, desc_size;
    UINT32 spinner_frame = 0;

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

    /* Step 2: Draw initial boot splash */
    render_splash(0);

    /* Step 3: Load kernel ELF */
    efi_print(u"Loading kernel...\r\n");
    status = load_kernel(&kernel_entry);
    if (EFI_ERROR(status)) {
        efi_print(u"[FAIL] Kernel load failed\r\n");
        return status;
    }

    /* Animate spinner a few frames while we set up */
    for (spinner_frame = 1; spinner_frame < 8; spinner_frame++) {
        render_splash(spinner_frame);
        gBS->Stall(80000);  /* 80ms per frame */
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
