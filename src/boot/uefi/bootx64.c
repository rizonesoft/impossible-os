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
#define BOOT_MMAP_MAX_ENTRIES 256

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
    char    cmdline[BOOT_CONF_CMDLINE_MAX];
    UINT8   config_found;
};

#define BOOT_CONFIG_TABLE_MAX 32
#define BOOT_RT_MMAP_MAX      64

struct boot_rt_mem_entry {
    UINT64 phys_addr;
    UINT64 num_pages;
    UINT32 type;
    UINT32 reserved;
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
    /* TPM Measured Boot */
    UINT64  tpm_event_log;
    UINT32  tpm_event_log_size;
    UINT8   tpm_available;
    UINT8   tpm_version;
    UINT16  tpm_event_count;
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

/* --- Helper: early serial output to COM1 (0x3F8) ---
 * Works before ExitBootServices — provides diagnostics even when
 * UEFI video console doesn't work (e.g. Hyper-V Gen 2).
 * The 16550 UART is emulated by all major hypervisors. */
#define SERIAL_COM1 0x3F8

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

static void serial_early_init(void)
{
    outb_early(SERIAL_COM1 + 1, 0x00);  /* Disable interrupts */
    outb_early(SERIAL_COM1 + 3, 0x80);  /* Enable DLAB */
    outb_early(SERIAL_COM1 + 0, 0x03);  /* 38400 baud (divisor=3) */
    outb_early(SERIAL_COM1 + 1, 0x00);
    outb_early(SERIAL_COM1 + 3, 0x03);  /* 8N1 */
    outb_early(SERIAL_COM1 + 2, 0xC7);  /* Enable FIFO */
    outb_early(SERIAL_COM1 + 4, 0x0B);  /* IRQs, RTS/DSR */
}

static void serial_early_putchar(char c)
{
    UINT32 timeout = 100000;
    while (!(inb_early(SERIAL_COM1 + 5) & 0x20) && --timeout)
        ;
    outb_early(SERIAL_COM1, (UINT8)c);
}

static void serial_early_print(const char *s)
{
    while (*s) {
        if (*s == '\n')
            serial_early_putchar('\r');
        serial_early_putchar(*s++);
    }
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
 * Resolution selection strategy:
 *
 *   A. EDID present and valid:
 *      Scan GOP modes for exact W x H 32bpp match -> SetMode -> done.
 *      Real hardware only: panels report native resolution via EDID.
 *
 *   B. No EDID (QEMU, VirtualBox, most emulators):
 *      If the current mode is already 32bpp with FrameBufferBase != 0,
 *      USE IT AS-IS with no SetMode call.
 *      This honors -device VGA,xres=N,yres=M — the whole point of the
 *      HiDPI test targets is that OVMF already set the right mode.
 *
 *   C. Current mode unusable (FrameBufferBase==0 or non-32bpp):
 *      Search for 1280x720 -> SetMode.  Final fallback: SetMode(0).
 * ============================================================================ */
static EFI_STATUS init_gop(void)
{
    EFI_GUID gop_guid  = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_GUID edid_guid = EFI_EDID_ACTIVE_PROTOCOL_GUID;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
    EFI_EDID_ACTIVE_PROTOCOL     *edid_proto;
    EFI_STATUS status;
    UINT32 i;

    /* Locate GOP */
    status = gBS->LocateProtocol(&gop_guid, (VOID *)0, (VOID **)&gop);
    if (EFI_ERROR(status)) {
        efi_print(u"[FAIL] GOP not found\r\n");
        return status;
    }

    /* Is the current firmware mode already usable? */
    BOOLEAN cur_ok =
        (gop->Mode->FrameBufferBase != 0) &&
        (gop->Mode->Info->PixelFormat == PixelBlueGreenRedReserved ||
         gop->Mode->Info->PixelFormat == PixelRedGreenBlueReserved);

    /* ── A. Try EDID for the panel's native resolution ────────────────── */
    UINT32 edid_w = 0, edid_h = 0;
    status = gBS->LocateProtocol(&edid_guid, (VOID *)0, (VOID **)&edid_proto);
    if (!EFI_ERROR(status) &&
        edid_proto->SizeOfEdid >= 72 &&
        edid_proto->Edid != (VOID *)0) {

        const UINT8 *e = edid_proto->Edid;
        /* Preferred Timing Descriptor at byte 54.
         * H-active: byte[56] | (byte[58]>>4)<<8
         * V-active: byte[59] | (byte[61]>>4)<<8 */
        UINT32 hw = e[56] | (((UINT32)(e[58] >> 4) & 0x0F) << 8);
        UINT32 hh = e[59] | (((UINT32)(e[61] >> 4) & 0x0F) << 8);
        if (hw > 0 && hh > 0) { edid_w = hw; edid_h = hh; }
    }

    if (edid_w > 0 && edid_h > 0) {
        /* Search for matching GOP mode and switch to it */
        for (i = 0; i < gop->Mode->MaxMode; i++) {
            UINTN info_size;
            EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info;
            if (EFI_ERROR(gop->QueryMode(gop, i, &info_size, &info))) continue;

            if (info->HorizontalResolution == edid_w &&
                info->VerticalResolution   == edid_h &&
                (info->PixelFormat == PixelBlueGreenRedReserved ||
                 info->PixelFormat == PixelRedGreenBlueReserved)) {
                status = gop->SetMode(gop, i);
                if (!EFI_ERROR(status) && gop->Mode->FrameBufferBase != 0) {
                    efi_print(u"[GOP] EDID native mode set\r\n");
                    goto store_fb;
                }
                break;  /* SetMode failed or bad FB — fall through */
            }
        }
        efi_print(u"[GOP] EDID match failed — using firmware mode\r\n");
    }

    /* ── B. No EDID: keep OVMF's current mode ───────────────────────────
     * OVMF initialises the display at the resolution given by xres/yres on
     * the QEMU device (-device VGA,xres=N,yres=M or bochs-display,xres=N).
     * By the time our bootloader runs, gop->Mode already reflects that
     * resolution.  We just honour it — no SetMode needed.
     *
     * Fallback: current mode is genuinely unusable (FrameBufferBase=0 or
     * non-32bpp format).  In that case try mode 0 (always valid on OVMF). */
    if (cur_ok) {
        efi_print(u"[GOP] Keeping firmware mode\r\n");
        goto store_fb;
    }

    /* cur_ok=false → last resort: SetMode(0) */
    efi_print(u"[GOP] Current mode unusable — SetMode(0)\r\n");
    gop->SetMode(gop, 0);


store_fb:
    /* Store framebuffer info and zero VRAM */
    gFramebuffer = (UINT32 *)(UINTN)gop->Mode->FrameBufferBase;
    gFbWidth  = gop->Mode->Info->HorizontalResolution;
    gFbHeight = gop->Mode->Info->VerticalResolution;
    gFbPitch  = gop->Mode->Info->PixelsPerScanLine;

    {
        UINTN sz = (UINTN)gFbHeight * (UINTN)gFbPitch;
        UINTN j;
        for (j = 0; j < sz; j++)
            gFramebuffer[j] = 0x00000000;
    }

    g_boot_info_ptr->fb.addr   = (UINT64)gop->Mode->FrameBufferBase;
    g_boot_info_ptr->fb.pitch  = gop->Mode->Info->PixelsPerScanLine * 4;
    g_boot_info_ptr->fb.width  = gFbWidth;
    g_boot_info_ptr->fb.height = gFbHeight;
    g_boot_info_ptr->fb.bpp    = 32;
    g_boot_info_ptr->fb.type   = 1;
    g_boot_info_ptr->fb_available = 1;

    return EFI_SUCCESS;
}

/* ============================================================================
 * Step 1b: Parse boot.conf — key=value ini file from EFI partition
 *
 * Format: key=value, # comments, blank lines ignored.
 * Runs BEFORE load_kernel() — UEFI Boot Services are still available.
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
    /* Unknown keys are silently ignored — forward compatibility */
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

    serial_early_print("BOOT: parse_boot_conf...\n");

    /* Open filesystem */
    status = gBS->LocateProtocol(&fs_guid, (VOID *)0, (VOID **)&fs);
    if (EFI_ERROR(status)) {
        serial_early_print("BOOT: boot.conf - no filesystem\n");
        return;
    }

    status = fs->OpenVolume(fs, &root_dir);
    if (EFI_ERROR(status)) {
        serial_early_print("BOOT: boot.conf - cannot open volume\n");
        return;
    }

    /* Open boot.conf */
    status = root_dir->Open(
        root_dir, &conf_file,
        u"\\EFI\\ImpossibleOS\\boot.conf",
        EFI_FILE_MODE_READ, 0
    );
    if (EFI_ERROR(status)) {
        serial_early_print("BOOT: boot.conf not found - using defaults\n");
        root_dir->Close(root_dir);
        return;
    }

    /* Read entire file (boot.conf should be < 1 KB) */
    char buf[1024];
    UINTN buf_size = sizeof(buf) - 1;
    status = conf_file->Read(conf_file, &buf_size, buf);
    conf_file->Close(conf_file);
    root_dir->Close(root_dir);

    if (EFI_ERROR(status) || buf_size == 0) {
        serial_early_print("BOOT: boot.conf read error - using defaults\n");
        return;
    }
    buf[buf_size] = '\0';

    serial_early_print("BOOT: boot.conf loaded\n");

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
            /* No '=' found — skip line */
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

    for (offset = 0; offset < map_size && idx < BOOT_MMAP_MAX_ENTRIES;
         offset += desc_size) {
        EFI_MEMORY_DESCRIPTOR *desc =
            (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)mmap + offset);

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
            g_boot_info_ptr->rt_mmap[idx].phys_addr = desc->PhysicalStart;
            g_boot_info_ptr->rt_mmap[idx].num_pages = desc->NumberOfPages;
            g_boot_info_ptr->rt_mmap[idx].type      = desc->Type;
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
        /* No TPM — not an error, just unavailable */
        serial_early_print("BOOT: TPM: not available\n");
        g_boot_info_ptr->tpm_available = 0;
        return;
    }

    /* Get TPM capabilities */
    EFI_TCG2_BOOT_SERVICE_CAPABILITY caps;
    caps.Size = (UINT8)sizeof(caps);

    status = tcg2->GetCapability(tcg2, &caps);
    if (EFI_ERROR(status)) {
        serial_early_print("BOOT: TPM: GetCapability failed\n");
        g_boot_info_ptr->tpm_available = 0;
        return;
    }

    if (!caps.TPMPresentFlag) {
        serial_early_print("BOOT: TPM: device not present\n");
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
        serial_early_print("BOOT: TPM: GetEventLog failed\n");
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
        serial_early_print("BOOT: TPM: failed to allocate log buffer\n");
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
     * entries.  This is approximate — the kernel will do full parsing. */
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
            /* First entry is spec ID event — remaining entries use
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

    serial_early_print("BOOT: TPM: event log retrieved\n");
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
    UINT32 desc_version;

    /* Save globals */
    gST = SystemTable;
    gBS = SystemTable->BootServices;
    gImageHandle = ImageHandle;

    /* Initialize early serial for diagnostics (before anything else) */
    serial_early_init();
    serial_early_print("BOOT: efi_main entered\n");

    /* Disable watchdog timer (UEFI default: 5 min timeout) */
    gBS->SetWatchdogTimer(0, 0, 0, (CHAR16 *)0);

    /* Point boot_info to a known physical address */
    g_boot_info_ptr = (struct boot_info *)BOOT_INFO_PHYS_ADDR;
    efi_memset(g_boot_info_ptr, 0, sizeof(struct boot_info));

    /* Step 1: Initialize graphics */
    serial_early_print("BOOT: init_gop...\n");
    status = init_gop();
    if (EFI_ERROR(status)) {
        serial_early_print("BOOT: FAIL init_gop\n");
        efi_print(u"[FAIL] Graphics initialization failed\r\n");
        return status;
    }
    serial_early_print("BOOT: init_gop OK\n");

    /* Step 1b: Parse boot.conf (must run while UEFI Boot Services available) */
    parse_boot_conf();

    /* Clear screen to black before loading the kernel. */
    {
        UINT32 row, col;
        for (row = 0; row < gFbHeight; row++)
            for (col = 0; col < gFbWidth; col++)
                gFramebuffer[row * gFbPitch + col] = 0x00000000;
    }

    /* Debug indicator: draw a bright red bar at top-left (100x10px).
     * If this bar is visible on Hyper-V, it proves the UEFI bootloader
     * loaded and GOP initialized successfully.
     * Each subsequent stage adds a new colored bar below:
     *   Row 0:  RED    = GOP init OK
     *   Row 12: GREEN  = kernel loaded OK
     *   Row 24: BLUE   = ExitBootServices OK
     *   Row 36: YELLOW = page tables OK
     *   Row 48: WHITE  = about to jump to kernel */
#define DRAW_BAR(y_off, color) do { \
    UINT32 _r, _c; \
    for (_r = (y_off); _r < (y_off) + 10 && _r < gFbHeight; _r++) \
        for (_c = 0; _c < 100 && _c < gFbWidth; _c++) \
            gFramebuffer[_r * gFbPitch + _c] = (color); \
} while (0)

    DRAW_BAR(0, 0x00FF0000);  /* RED = GOP OK */

    /* Load kernel ELF */
    serial_early_print("BOOT: load_kernel...\n");
    status = load_kernel(&kernel_entry);
    if (EFI_ERROR(status)) {
        serial_early_print("BOOT: FAIL load_kernel\n");
        efi_print(u"[FAIL] Kernel load failed\r\n");
        /* Draw orange bar to indicate kernel load failure */
        DRAW_BAR(12, 0x00FF8000);
        for (;;) __asm__ volatile("hlt");
    }
    serial_early_print("BOOT: load_kernel OK\n");
    DRAW_BAR(12, 0x0000FF00);  /* GREEN = kernel loaded */

    /* Step 4: Copy UEFI Configuration Table + find ACPI RSDP */
    serial_early_print("BOOT: copy_config_tables...\n");
    copy_config_tables();

    /* Step 4c: Retrieve TPM event log (if available) */
    retrieve_tpm_event_log();

    /* Step 5: Get UEFI memory map */
    serial_early_print("BOOT: get_memory_map...\n");
    status = get_memory_map(&map_key, &mmap, &map_size, &desc_size,
                            &desc_version);
    if (EFI_ERROR(status)) {
        serial_early_print("BOOT: FAIL get_memory_map\n");
        efi_print(u"[FAIL] GetMemoryMap failed\r\n");
        DRAW_BAR(24, 0x00FF8000);
        for (;;) __asm__ volatile("hlt");
    }

    fill_memory_map(mmap, map_size, desc_size);
    fill_runtime_map(mmap, map_size, desc_size);

    /* Step 5b: Preserve Runtime Services pointer + descriptor metadata */
    g_boot_info_ptr->uefi_runtime_services = (UINTN)gST->RuntimeServices;
    g_boot_info_ptr->uefi_rt_available     = (gST->RuntimeServices != (void *)0) ? 1 : 0;
    g_boot_info_ptr->uefi_mmap_desc_size    = (UINT32)desc_size;
    g_boot_info_ptr->uefi_mmap_desc_version = desc_version;
    serial_early_print("BOOT: runtime services preserved\n");

    /* Step 6: ExitBootServices */
    serial_early_print("BOOT: ExitBootServices...\n");
    status = gBS->ExitBootServices(gImageHandle, map_key);
    if (EFI_ERROR(status)) {
        status = get_memory_map(&map_key, &mmap, &map_size, &desc_size,
                                &desc_version);
        if (!EFI_ERROR(status)) {
            fill_memory_map(mmap, map_size, desc_size);
            fill_runtime_map(mmap, map_size, desc_size);
            status = gBS->ExitBootServices(gImageHandle, map_key);
        }
        if (EFI_ERROR(status)) {
            serial_early_print("BOOT: FATAL ExitBootServices failed\n");
            DRAW_BAR(24, 0x00FF8000);
            for (;;) __asm__ volatile("hlt");
        }
    }
    serial_early_print("BOOT: ExitBootServices OK\n");
    DRAW_BAR(24, 0x000000FF);  /* BLUE = ExitBootServices OK */

    /* === NO MORE UEFI CALLS FROM HERE === */

    /* Step 7: Set up page tables */
    serial_early_print("BOOT: setup_page_tables...\n");
    setup_page_tables();
    DRAW_BAR(36, 0x00FFFF00);  /* YELLOW = page tables OK */

    /* Step 8: Jump to kernel! */
    serial_early_print("BOOT: jumping to kernel_main\n");
    DRAW_BAR(48, 0x00FFFFFF);  /* WHITE = about to jump */
    jump_to_kernel(kernel_entry);

    /* Never reached */
    return EFI_SUCCESS;
}
