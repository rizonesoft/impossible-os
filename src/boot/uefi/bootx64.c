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

/* --- Boot info structure (mirrored from include/kernel/boot_info.h) ---
 * Every mirror struct, constant, and _Static_assert lives in the shared
 * header so tools/boot-info-manifest/ can compile the same definitions for
 * its offset-fingerprint dumper. If this include fails, run the clean-build
 * sequence before editing the mirror. */
#include "boot_info_mirror.h"
#include "boot_proto_mirror.h"
#include "elf_bootproto.h"
#include "boot_proto_sha.h"   /* generated; provides KERNEL_ABI_SHA256 */
#include "boot_loader_identity.h" /* generated; provides BOOT_LOADER_GIT_SHA + ..._BUILD_TIME + ..._BUILD_LABEL */
#include "../../../include/boot/sha256_boot.h"     /* SHA-256 for the loader self-measurement */
#include "../../../include/boot/devpath_filepath.h" /* bounded LoadedImage FilePath parser */
#include "../../../include/boot/pe_authenticode.h" /* Authenticode PE image hash, as firmware computes it */
#include "../../../include/boot/tcg_evlog.h"       /* strict TCG event-log walk + image-load event identity */
#include "../../../include/boot/uki_cmdline_check.h" /* uki_find_disk_override_token() shared with kernel test */
#include "../../../include/boot/uki_cmdline_media_role.h" /* uki_cmdline_extract_media_role() shared with kernel test */
#include "../../../include/boot/boot_entries_parser.h"   /* boot entries parser + envelope */
#include "../../../include/boot/boot_policy.h"           /* boot policy ladder + decision */
#include "../../../include/boot/boot_entry_kind.h"        /* per-kind payload validators */
#include "../../../include/boot/boot_implicit_payload.h"  /* implicit-payload reservation + claim */
#include "../../../include/boot/ab_boot_metadata.h"        /* A/B dual-slot metadata wire ABI + validators (TODO-21) */
#include "../../../include/kernel/mm/memmap_boot.h"          /* single-sourced HHDM constants (no drift vs memmap.h) */
#include "../../../include/kernel/boot_version_constants.h"  /* BOOT_INFO_PHYS_ADDR + fault-class constants */
#include "../../../include/kernel/firmware_quirks_parse.inc" /* shared firmware_quirk_disable= tokenizer */

/* Inline rdtsc for boot timing */
static inline UINT64 boot_rdtsc(void)
{
    UINT32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((UINT64)hi << 32) | lo;
}

/* --- ELF64 header structures ---
 * Single source of truth shared with elf_bootproto.c (.bootproto walker)
 * and any future ELF consumer in the bootloader. The _Static_assert block
 * below pins every byte offset load_kernel() reads, so a future change to
 * elf_types.h that shifts a field breaks the build instead of silently
 * miscomputing destination paddrs / segment bounds at boot. */
#include "elf_types.h"

_Static_assert(__builtin_offsetof(Elf64_Ehdr, e_magic)     ==  0, "Ehdr e_magic");
_Static_assert(__builtin_offsetof(Elf64_Ehdr, e_class)     ==  4, "Ehdr e_class");
_Static_assert(__builtin_offsetof(Elf64_Ehdr, e_machine)   == 18, "Ehdr e_machine");
_Static_assert(__builtin_offsetof(Elf64_Ehdr, e_phoff)     == 32, "Ehdr e_phoff");
_Static_assert(__builtin_offsetof(Elf64_Ehdr, e_phentsize) == 54, "Ehdr e_phentsize");
_Static_assert(__builtin_offsetof(Elf64_Ehdr, e_phnum)     == 56, "Ehdr e_phnum");
_Static_assert(sizeof(Elf64_Ehdr) == 64, "Elf64_Ehdr size mismatch (ELF64 spec)");

_Static_assert(__builtin_offsetof(Elf64_Phdr, p_type)   ==  0, "Phdr p_type");
_Static_assert(__builtin_offsetof(Elf64_Phdr, p_flags)  ==  4, "Phdr p_flags");
_Static_assert(__builtin_offsetof(Elf64_Phdr, p_offset) ==  8, "Phdr p_offset");
_Static_assert(__builtin_offsetof(Elf64_Phdr, p_paddr)  == 24, "Phdr p_paddr");
_Static_assert(__builtin_offsetof(Elf64_Phdr, p_filesz) == 32, "Phdr p_filesz");
_Static_assert(__builtin_offsetof(Elf64_Phdr, p_memsz)  == 40, "Phdr p_memsz");
_Static_assert(sizeof(Elf64_Phdr) == 56, "Elf64_Phdr size mismatch (ELF64 spec)");

/* --- Globals --- */
EFI_SYSTEM_TABLE    *gST;
static EFI_BOOT_SERVICES   *gBS;
static EFI_HANDLE           gImageHandle;
static EFI_HANDLE g_boot_device_handle; /*: boot device from LoadedImage */
/* LoadedImage->FilePath captured in Step 0; network_boot_discover() walks it in
 * addition to the DeviceHandle path because HTTP/PXE URI/MAC/IP messaging nodes
 * can live in the image file path, not the controller handle path (Codex design
 * review D1). */
static EFI_DEVICE_PATH_PROTOCOL *g_boot_image_file_path;

/* Firmware network-boot discovery result. Seeds the DHCP-provenance capture
 * and the boot_info network handoff. booted_from_network is driven by network
 * messaging
 * nodes in OUR boot path (authoritative), NOT by mere SNP presence (a NIC can
 * exist on a disk boot). MAC prefers the boot-path MAC node, falling back to the
 * SNP CurrentAddress. */
struct net_boot_discovery {
    int   booted_from_network;
    int   http_boot;        /* URI node present -> UEFI HTTP Boot */
    int   snp_available;    /* EFI_SIMPLE_NETWORK_PROTOCOL handle found */
    int   pxe_available;    /* EFI_PXE_BASE_CODE_PROTOCOL handle found */
    int   link_up;          /* SNP MediaPresent (valid only when link_known) */
    int   link_known;       /* boot NIC identified -> link_up is meaningful */
    UINT8 mac[6];
    UINT8 mac_len;
};
static struct net_boot_discovery g_net_discovery;

/* DHCP/PXE provenance captured from the firmware PXE Base Code cached DhcpAck.
 * Parsed fields seed the boot_info network handoff; the raw packet is preserved
 * for offline diagnostics. Sensitive DHCP options (43 vendor, 17 root-path) are
 * preserved in the raw copy but never printed to the boot log. */
struct net_dhcp_provenance {
    UINT8 client_ip[4];      /* BOOTP yiaddr */
    UINT8 next_server_ip[4]; /* BOOTP siaddr */
    UINT8 gateway_ip[4];     /* DHCP option 3 router, BOOTP giaddr fallback */
    UINT8 dhcp_server_ip[4]; /* DHCP option 54 server id */
    UINT8 boot_file[128];    /* BOOTP bootfile */
    int   valid;
};
static struct net_dhcp_provenance g_net_dhcp;
static UINT8 g_net_dhcp_raw[1472];   /* preserved raw DhcpAck for the boot_info handoff */
static int   g_net_dhcp_raw_valid;


/* Boot info -- placed at the known physical address (64 KiB) defined once
 * in include/kernel/boot_version_constants.h, included at the top of this
 * file. The kernel's PMM reservation and payload overlap validators read
 * that same macro, so this producer cannot drift from those consumers. */
static struct boot_info    *g_boot_info_ptr;

/* Higher-half direct map (HHDM) construction state -- higher-half relocation section 2.
 * The kernel-image PT_LOAD physical envelope [lo, hi) is captured by
 * load_kernel() (loaded ELF p_paddr, NOT kernel symbols) so the writable
 * HHDM alias can exclude it (W^X: a writable low alias of kernel text would
 * defeat kernel_wx_protect()). The arena holds the extra PDPT/PD/PT frames
 * the HHDM leaves need; it is AllocatePages'd pre-EBS (< 4 GiB) and filled
 * post-EBS by setup_page_tables(). See docs/infrastructure/kernel-address-space.md.
 *
 * These are RESET AT RUNTIME (load_kernel for the envelope, bl_hhdm_reserve_arena
 * for the arena) -- the bootloader does not zero .bss, so a static `= 0` would
 * start at firmware pool-poison (0xAF...). The static initializers below are
 * documentation only; correctness relies on the runtime resets. */
static UINT64 g_kernel_img_lo    = ~0ULL;  /* min PT_LOAD p_paddr (page-aligned down at use) */
static UINT64 g_kernel_img_hi    = 0;      /* max PT_LOAD p_paddr + p_memsz */
static UINT64 g_hhdm_arena_base  = 0;      /* physical base of the HHDM page-table arena */
static UINT64 g_hhdm_arena_pages = 0;      /* arena size in 4 KiB pages (0 = HHDM disabled) */

/* UKI (Unified Kernel Image) embedded-section pointers per the UAPI Group
 * UKI specification. Populated by detect_uki_sections() if our LoadedImage's
 * PE/COFF section table carries a `.linux` section. When set, load_kernel()
 * uses the embedded buffer instead of reading `\\kernel.exe` from the ESP,
 * and BOOT_FLAG_INVOKED_VIA_UKI is recorded in boot_info->flags. The
 * whole-chain Secure Boot signature over the PE then covers kernel +
 * cmdline + osrel as one signed unit. */
static UINT8 *g_uki_kernel_ptr;
static UINTN  g_uki_kernel_size;
static UINT8 *g_uki_cmdline_ptr;     /* boot.conf-equivalent; may be NULL */
static UINTN  g_uki_cmdline_size;
static UINT8 *g_uki_osrel_ptr;       /* os-release info; may be NULL */
static UINTN  g_uki_osrel_size;

/* UKI v14 signed payloads. Populated by detect_uki_sections() with
 * pointers into LoadedImage memory; uki_copy_payloads_to_loader_data()
 * later allocates EfiLoaderData pages and copies each payload so the
 * kernel-physical addresses survive ExitBootServices. The post-copy
 * addresses are what gets published into boot_info. Zero means the
 * section was absent in the UKI (legacy back-compat). */
static UINT8 *g_uki_initrd_ptr;       /* LoadedImage-relative pointer; may be NULL */
static UINTN  g_uki_initrd_size;
static UINT64 g_uki_initrd_phys;      /* post-copy kernel-physical addr; 0 if absent */
static UINT8 *g_uki_recovery_ptr;
static UINTN  g_uki_recovery_size;
static UINT64 g_uki_recovery_phys;
static UINT8 *g_uki_modules_ptr;
static UINTN  g_uki_modules_size;
static UINT64 g_uki_modules_phys;

/* Impossible OS vendor GUID: {6F35D3A4-C0E6-4A82-B5D8-7C9D2E4F8A13}
 * Must match IMPOSSIBLE_OS_VENDOR_GUID_INIT in include/kernel/uefi_vars.h. */
EFI_GUID g_impossible_os_guid = {
    0x6f35d3a4, 0xc0e6, 0x4a82,
    { 0xb5, 0xd8, 0x7c, 0x9d, 0x2e, 0x4f, 0x8a, 0x13 }
};

/* NVRAM variable name for boot error persistence (UCS-2) */
static CHAR16 g_boot_error_var[] = u"BootError";

/* Boot-error history source-section hint.  Set via boot_set_section()
 * at the entry of each major bootloader code path; read by boot_fatal()
 * when appending to the history ring.  Default UNKNOWN value surfaces
 * any path that forgot to call the setter (logged as a [WARN] line at
 * fatal-time so the gap is operator-visible).  Fatal path uses the
 * static rather than threading a parameter through every call site.
 *
 * Section codes use the 0x01NN range so they cannot collide with
 * BOOT_ERR_* (0x000N..0x0013), the kernel-Phase-3 sentinel (0xFFFF),
 * the EBS-success sentinel (0xFFFE), or the UNKNOWN sentinel (0xFFFD).
 * The consumer-side renderer decodes the sentinels by name and shows
 * remaining values as raw 0xNNNN; a future renderer extension can
 * promote these to symbolic phase names. */
#define BOOT_SECTION_BL_INIT        0x0101u  /* efi_main entry */
#define BOOT_SECTION_BL_CONF        0x0102u  /* parse_boot_conf */
#define BOOT_SECTION_BL_KERNEL      0x0103u  /* load_kernel */
#define BOOT_SECTION_BL_PAGETABLES  0x0104u  /* setup_kernel_pages */
#define BOOT_SECTION_BL_EBS         0x0105u  /* ExitBootServices retry loop */
#define BOOT_SECTION_BL_POLICY      0x0106u  /* boot_policy_invoke */
#define BOOT_SECTION_BL_MENU        0x0107u  /* boot_menu_run */

static UINT16 g_boot_section = BOOT_SECTION_UNKNOWN;

/* Boot-policy decision artifacts captured pre-EBS for the post-EBS
 * boot-decision populate block to read. Selected kind is preserved as
 * a sentinel-bearing UINT32: 0xFFFFFFFF means "no policy decision ran"
 * (BSS-zero would collide with BOOT_ENTRY_KIND_SPLIT == 0). The populate
 * block applies the path-changing kind override (recovery / diagnostics
 * / network / resume) AFTER the media-role switch so policy intent
 * trumps a NORMAL media role but still falls back to an installer /
 * recovery / diagnostics media role when no path-changing kind fired.
 *
 * The boot_info v19 fields (selected_entry_id / selection_reason /
 * rejected_entries) are written directly into g_boot_info_ptr by the
 * boot-policy invoke step pre-EBS; they survive EBS as part of the
 * boot_info struct at BOOT_INFO_PHYS_ADDR. This static is a separate
 * carry-channel for the kind because the kind is needed to map to
 * boot_path / boot_reason but the post-EBS populate block lives in a
 * different scope. */
#define G_POLICY_KIND_UNSET 0xFFFFFFFFu
static UINT32 g_policy_selected_kind = G_POLICY_KIND_UNSET;

/* Outcome of pinning the panic-evidence page at PANIC_EVIDENCE_PHYS_ADDR
 * (TODO-14 sec14). The pin runs at the very top of efi_main -- before
 * ClearScreen, before serial_early_init, before boot_log_init -- because it
 * must precede every allocation and every firmware protocol call this loader
 * makes. Serial is not up yet at that point, so the status is parked here and
 * reported once boot_log_init has run and the line can be captured.
 *
 * EFI_SUCCESS means this loader owns the page for the rest of the boot; any
 * error means firmware already owns it or will not hand it over. Neither is
 * a statement about the page's CONTENTS: a refusal does not prove a crash
 * record was destroyed, and a success does not prove one survived. */
static EFI_STATUS g_panic_page_status = EFI_SUCCESS;
static BOOLEAN    g_panic_page_attempted = 0;

/* Per-kind decoded payload from the policy-selected envelope. Populated
 * by boot_policy_invoke() AFTER menu override + SAFE materialization
 * but BEFORE counter decrement -- so the validation runs on the FINAL
 * pick, not a pre-menu candidate. Consumed by load_kernel(): when
 * g_policy_decoded.valid && g_policy_decoded.kind == SPLIT, the
 * decoded kernel path is the SOLE candidate (no fallback to ambient
 * search) so the ladder cannot report `selected=X` while a different
 * kernel actually loaded. Default-zero state means "no authoritative
 * payload" and load_kernel falls back to its existing kernel_paths[]
 * search. */
static boot_entry_decoded_t g_policy_decoded;

/* Forward declaration of the history-ring writer (definition lives in
 * src/boot/uefi/boot_history.c).  Three call sites: boot_fatal(),
 * the EBS retry success branch, and the kernel side via the
 * uefi_var_* helpers.  Failure WARNs to serial; never blocks. */
extern void boot_history_append(UINT16 source_section, UINT16 err_code);

/* Forward declaration of serial_early_print -- the static definition
 * lives further down (the original code orders helpers by phase, and
 * the boot_set_section setter introduces the first early-serial use
 * before the definition appears). */
void serial_early_print(const char *s);

static void boot_set_section(UINT16 section)
{
    if (g_boot_section == section)
        return;
    /* Single-line transition log so reviewers can scan a serial
     * capture for path-attribution coverage.  The hex format keeps
     * the sentinels (0xFFFD/0xFFFE/0xFFFF) and section numbers in a
     * uniform shape. */
    serial_early_print("[BOOT] section transition: 0x");
    {
        const char *hex = "0123456789ABCDEF";
        char buf[5];
        buf[0] = hex[(g_boot_section >> 12) & 0xF];
        buf[1] = hex[(g_boot_section >> 8) & 0xF];
        buf[2] = hex[(g_boot_section >> 4) & 0xF];
        buf[3] = hex[g_boot_section & 0xF];
        buf[4] = 0;
        serial_early_print(buf);
    }
    serial_early_print(" -> 0x");
    {
        const char *hex = "0123456789ABCDEF";
        char buf[5];
        buf[0] = hex[(section >> 12) & 0xF];
        buf[1] = hex[(section >> 8) & 0xF];
        buf[2] = hex[(section >> 4) & 0xF];
        buf[3] = hex[section & 0xF];
        buf[4] = 0;
        serial_early_print(buf);
    }
    serial_early_print("\n");
    g_boot_section = section;
}

/* Framebuffer for splash */
static UINT32 *gFramebuffer;
static UINT32  gFbWidth;
static UINT32  gFbHeight;
static UINT32  gFbPitch;       /* in pixels */
static UINT8   gFbPixelFormat; /* 0=RGBX, 1=BGRX, 2=BitMask -- see gop_pixel_format_code() */

/* Requested resolution from boot.conf Resolution=WxH (0 = auto). */
static UINT32  g_conf_res_width  = 0;
static UINT32  g_conf_res_height = 0;

/* --- Forward declarations for POST16 (used by parse_boot_conf, load_kernel) --- */
static inline void post_code(UINT8 code);
static inline void post_code16(UINT16 code);

/* 16-bit POST codes for boot device filesystem scoping.
 * Placed here so parse_boot_conf() can reference them; the remaining
 * bootloader POST16 codes live near efi_main() where they're used. */
#define POST16_BL_BOOT_FS       0xB092
#define POST16_BL_BOOT_FS_OK    0xB093
#define POST16_BL_FALLBACK 0xB094 /* Device fallback chain */
#define POST16_BL_FALLBACK_OK   0xB095
#define POST16_BL_ESP_INTEGRITY 0xB096  /* ESP integrity check entry */
#define POST16_BL_ESP_GPT       0xB097  /* ESP GPT type-GUID probe */
#define POST16_BL_ESP_BPB       0xB098  /* ESP FAT BPB sanity */
#define POST16_BL_ESP_FILES     0xB099  /* ESP required-files batch */
#define POST16_BL_ESP_INTEGRITY_OK 0xB09C  /* ESP integrity all gates passed */
#define POST16_BL_ROLLBACK_REFUSE 0xB09A  /* Anti-rollback: shipped < required */
#define POST16_BL_ROLLBACK_PASS   0xB09B  /* Anti-rollback: shipped >= required */
#define POST16_BL_BOOT_POLICY     0xB0B0  /* Boot policy invocation entry */
#define POST16_BL_BOOT_POLICY_PARSE 0xB0B1 /* Boot entries store parsed */
#define POST16_BL_BOOT_POLICY_DECIDE 0xB0B2 /* Policy ladder decided */
#define POST16_BL_BOOT_POLICY_OK  0xB0B3  /* Policy decision copied to boot_info */
#define POST16_BL_COUNTER_SCAN    0xB0B4  /* Counter directory scan entry */
#define POST16_BL_COUNTER_DECR    0xB0B5  /* Counter decrement (write-new + Flush() + Close(success) + delete-old) */
#define POST16_BL_MENU            0xB0B6  /* Boot menu rendered (interactive selector) */
#define POST16_BL_KIND_VALIDATE   0xB0B7  /* Per-kind payload validation entry */
#define POST16_BL_KIND_VALIDATE_OK 0xB0B8 /* Per-kind validation accepted (or demoted to fallback) */
#define POST16_BL_AB_SELECT       0xB0B9  /* A/B dual-slot selection entry (TODO-21) */
#define POST16_BL_AB_SELECT_OK    0xB0BA  /* A/B slot selected + active_slot published */

/* --- Helper: memory ops ---
 * x86-64 `rep stosb` / `rep movsb` -- modern microarchitectures
 * (Ivy Bridge+ via ERMSB; Ice Lake+ via FSRM) fast-path these to
 * cache-line-wide stores/loads.  Both work pre- and post-
 * ExitBootServices (no gBS dependency).  load_kernel() calls these
 * across multi-MiB PT_LOAD segments and BSS clears; the prior
 * byte-loop implementation executed millions of byte stores per
 * boot.  Direction flag is cleared on entry per the System V AMD64
 * ABI's stable-DF assumption (UEFI firmware does not guarantee
 * DF=0 on entry to the loaded image, so be explicit). */
static void efi_memset(void *dst, UINT8 val, UINTN size)
{
    __asm__ volatile (
        "cld\n\t"
        "rep stosb"
        : "+D"(dst), "+c"(size)
        : "a"(val)
        : "memory", "cc"
    );
}

static void efi_memcpy(void *dst, const void *src, UINTN size)
{
    __asm__ volatile (
        "cld\n\t"
        "rep movsb"
        : "+D"(dst), "+S"(src), "+c"(size)
        :
        : "memory", "cc"
    );
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

/* ACPI checksum gate -- byte sum of [base, base+len) MUST equal 0
 * mod 256 per ACPI 6.5 §5.2.5.3 (RSDP), §5.2.6 (DESCRIPTION_HEADER).
 * Used by serial_spcr_probe() to reject corrupt firmware tables
 * BEFORE dereferencing untrusted base_address fields.  Length is
 * caller-bounded; this helper trusts the caller to have already
 * rejected implausible lengths. */
static int acpi_checksum_ok(const void *base, UINTN len)
{
    const UINT8 *p = (const UINT8 *)base;
    UINT8 sum = 0;
    UINTN i;
    for (i = 0; i < len; i++)
        sum = (UINT8)(sum + p[i]);
    return sum == 0;
}

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

    /* Validate RSDP v1 checksum (first 20 bytes per ACPI 6.5
     * specification section 5.2.5.3). */
    if (!acpi_checksum_ok(rsdp, 20))
        return 0;
    /* For revision >= 2, the v2 RSDP MUST have length covering the
     * full 36-byte struct (signature thru ext_checksum + 3-byte
     * reserved tail per ACPI specification).  Reject malformed
     * length OUTRIGHT -- a skip-on-bad-length policy would let
     * firmware bypass the extended checksum gate while still
     * steering xsdt_addr.  Cap
     * at sizeof for future-proofing against larger declared
     * lengths we cannot validate.  Length must also pass the
     * extended checksum. */
    if (rsdp->revision >= 2) {
        if (rsdp->length < sizeof(BL_ACPI_RSDP) ||
            rsdp->length > sizeof(BL_ACPI_RSDP))
            return 0;
        if (!acpi_checksum_ok(rsdp, rsdp->length))
            return 0;
    }

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

    /* Validate root signature ("XSDT" if use_xsdt else "RSDT")
     * + full-table checksum BEFORE treating payload bytes as
     * entry pointers.  Without this, a corrupt RSDP that
     * survived the v1/v2 checksum gate could redirect the walk
     * into stale memory; a chance match on "SPCR" four bytes in
     * would let the bogus base_address through to
     * serial_probe_port. */
    {
        const UINT8 sig_xsdt[4] = {'X', 'S', 'D', 'T'};
        const UINT8 sig_rsdt[4] = {'R', 'S', 'D', 'T'};
        const UINT8 *want = use_xsdt ? sig_xsdt : sig_rsdt;
        if (sdt->signature[0] != want[0] ||
            sdt->signature[1] != want[1] ||
            sdt->signature[2] != want[2] ||
            sdt->signature[3] != want[3])
            return 0;
        if (!acpi_checksum_ok(sdt, sdt->length))
            return 0;
    }

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
        /* Reject implausible declared length (caller-bounded sanity
         * cap before checksum walk) -- 1 MiB matches the root SDT
         * cap above.  ACPI 6.5 SPCR has a max of ~80 bytes today;
         * future revisions are bounded. */
        if (spcr->length > 0x100000)
            return 0;
        /* Validate full-SPCR-table checksum BEFORE reading
         * base_address / interface_type / baud_rate.  A SPCR with
         * a corrupt length-bounded payload cannot reach
         * serial_probe_port without the byte sum gating it out
         *. */
        if (!acpi_checksum_ok(spcr, spcr->length))
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

        /* SPCR is firmware-authoritative on this address.  Accept any
         * 16-bit I/O base (COM1/COM2/COM3/COM4 or vendor-custom) so we
         * do not silently lose diagnostics on hardware that declares a
         * 16550-compatible UART outside the legacy COM1/COM2 pair.
         * Reject only if base_address would not fit in a UINT16 -- the
         * I/O port space is 16 bits wide on x86 and base_addr_space==1
         * already guaranteed I/O above; firmware advertising an I/O
         * address > 0xFFFF is malformed.  The fallback probe in
         * serial_early_init() keeps the narrower COM1/COM2-only policy
         * because there is no firmware-provided base to trust. */
        /* The 16550 register block uses offsets through base+7, so
         * any base above 0xFFF8 would wrap the 16-bit I/O port space
         * when probing or initializing.  Reject those plus zero. */
        if (spcr->base_address == 0 || spcr->base_address > 0xFFF8ULL) {
            s_spcr_skipped = 1;
            s_spcr_skip_addr = spcr->base_address;
            return 0;
        }
        UINT16 port = (UINT16)spcr->base_address;

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

void serial_early_print(const char *s)
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

/* 16-hex-digit printer for UINT64 EFI_STATUS and physical addresses in
 * the early-boot diagnostic path. Same truncation-free shape as
 * serial_early_print_hex16 but walks 16 nibbles. Used by the section 5
 * payload loader's failure messages. */
static void serial_early_print_hex64(UINT64 val)
{
    static const char hex[] = "0123456789ABCDEF";
    int i;
    for (i = 60; i >= 0; i -= 4)
        serial_early_putchar(hex[(val >> i) & 0xFull]);
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
/* Watchdog state.  TWO flags, not one -- "is the firmware watchdog
 * counting?" and "can we still talk to SetWatchdogTimer?" are
 * different questions.  Conflating them lets a refresh failure
 * suppress the pre-EBS disarm and ship a live 60s timer into the
 * kernel handoff. */
static int g_wd_armed;            /* 1 = firmware watchdog is counting; cleared
                                   * ONLY on successful disarm. */
static int g_wd_refresh_disabled; /* 1 = stop calling watchdog_reset()
                                   * because a prior refresh failed.
                                   * Pre-EBS disarm is still attempted
                                   * because g_wd_armed stays 1. */

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

/* BootError values must be a known BOOT_ERR_* code from efi.h.  Reject
 * anything else as untrusted input (a pre-OS UEFI app can write under
 * the public Impossible OS GUID).  Keep this in sync with the registry
 * in efi.h: highest currently-defined code is BOOT_ERR_HHDM_FAIL (0x0019). */
#define BOOT_ERR_REGISTRY_MAX 0x0019  /* highest BOOT_ERR_* in efi.h (BOOT_ERR_HHDM_FAIL) */
/* Pin the duplicate to the actual highest efi.h code so a new BOOT_ERR_* that
 * forgets to bump this fails the build instead of silently rejecting a valid
 * new error as untrusted. */
_Static_assert(BOOT_ERR_REGISTRY_MAX == BOOT_ERR_HHDM_FAIL,
    "BOOT_ERR_REGISTRY_MAX must track the highest BOOT_ERR_* in efi.h");

static UINT32 nvram_read_boot_error(void)
{
    if (!gST || !gST->RuntimeServices)
        return 0;
    EFI_RUNTIME_SERVICES *rt = gST->RuntimeServices;
    if (!rt->GetVariable || !rt->SetVariable)
        return 0;
    UINT32 code = 0;
    UINTN size = sizeof(code);
    UINT32 attrs = 0;
    EFI_STATUS s = rt->GetVariable(
        g_boot_error_var, &g_impossible_os_guid,
        &attrs, &size, &code);

    /* Per UEFI 2.10 7.2.1, GetVariable populates DataSize and
     * Attributes on success AND on EFI_BUFFER_TOO_SMALL.  Any record
     * found (size != 4, oversized, or undersized) under our public
     * GUID came from a pre-OS app, not the writer side -- canonicalize
     * it via the delete-then-create repair so the writer-side
     * SetVariable does not later trip EFI_INVALID_PARAMETER on attr
     * change.  Only absent-or-no-perms errors (NOT_FOUND, security
     * violation, etc.) mean "no previous boot error". */
    int malformed = 0;
    if (s == EFI_BUFFER_TOO_SMALL) {
        malformed = 1;
    } else if (EFI_ERROR(s)) {
        return 0;
    } else if (size != sizeof(code)) {
        malformed = 1;
    }

    /* Treat any value the writer-side contract would not have produced
     * as untrusted: attrs must be exactly NV|BS|RT, and the value must
     * be inside the BOOT_ERR_* registry.  Malformed entries are
     * overwritten with the writer's canonical attrs+0 so the kernel's
     * later clear path sees a well-formed record. */
    const UINT32 expected_attrs = EFI_VARIABLE_NON_VOLATILE |
                                  EFI_VARIABLE_BOOTSERVICE_ACCESS |
                                  EFI_VARIABLE_RUNTIME_ACCESS;
    if (malformed || attrs != expected_attrs || code > BOOT_ERR_REGISTRY_MAX) {
        serial_early_print("[WARN] NVRAM: BootError untrusted "
                           "(attrs/value mismatch); repairing\n");
        /* UEFI 2.10 7.2.1: SetVariable cannot change attributes on an
         * existing variable -- it returns EFI_INVALID_PARAMETER.  To
         * promote a poisoned BootError back to the canonical NV|BS|RT
         * shape we must delete-then-create: SetVariable with size=0 and
         * the variable's CURRENT attrs deletes it, after which a fresh
         * SetVariable with canonical attrs and value=0 establishes the
         * well-formed record the kernel-side clear path expects. */
        EFI_STATUS d = rt->SetVariable(g_boot_error_var,
                                       &g_impossible_os_guid,
                                       attrs, 0, (void *)0);
        if (EFI_ERROR(d)) {
            serial_early_print("[WARN] NVRAM: BootError repair delete "
                               "failed; persistence channel poisoned "
                               "until manual NVRAM cleanup\n");
            return 0;
        }
        UINT32 zero = 0;
        EFI_STATUS w = rt->SetVariable(g_boot_error_var,
                                       &g_impossible_os_guid,
                                       expected_attrs, sizeof(zero),
                                       &zero);
        if (EFI_ERROR(w)) {
            serial_early_print("[WARN] NVRAM: BootError repair "
                               "recreate failed; channel cleared but "
                               "next fatal write may also fail\n");
        }
        return 0;
    }
    return code;
}

/* Reset the watchdog timer if it was successfully armed AND a prior
 * refresh has not failed.  On refresh failure we set
 * g_wd_refresh_disabled=1 to stop spamming the failing call -- but
 * we DO NOT clear g_wd_armed, because the firmware watchdog from
 * the initial successful arm may still be counting and the pre-EBS
 * disarm path must still run. */
static void watchdog_reset(void)
{
    if (!g_wd_armed || g_wd_refresh_disabled) return;
    EFI_STATUS s = gBS->SetWatchdogTimer(60, 0x424F4F54, 0, (CHAR16 *)0);
    if (EFI_ERROR(s)) {
        serial_early_print("[WARN] Watchdog: reset failed -- stopping "
                           "refresh attempts; pre-EBS disarm will "
                           "still fire to clear the live timer\n");
        g_wd_refresh_disabled = 1;
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
/* Canonical recovery URL: scheme + host + path + 4 lowercase hex.
 * Both QR-only (qr_render_error_url) and graphical BSOD paths must
 * encode the same string -- otherwise users scan one URL and read
 * another. The 4-hex width matches the gh-pages/err/<code>/ static
 * page layout AND the BOOT_ERR_REGISTRY_MAX domain (currently
 * 0x0013, well under 16 bits); the upper 16 bits of err_code carry
 * no information today. Buffer must be at least 33 bytes (28-byte
 * prefix + 4 hex + NUL). Returns the URL length excluding NUL. */
#define RECOVERY_URL_MIN_BUF 33
static UINTN format_recovery_url(char *buf, UINTN size, UINT32 err_code)
{
    static const char hex_lc[] = "0123456789abcdef";
    static const char prefix[] = "https://impossibleos.co/err/";
    UINTN i;

    if (size < RECOVERY_URL_MIN_BUF) {
        if (size > 0) buf[0] = '\0';
        return 0;
    }
    for (i = 0; prefix[i]; i++) buf[i] = prefix[i];
    /* 4 lowercase hex digits.  Matches the published recovery pages
     * under gh-pages/err/0000../<code>/.  RFC 3986 canonical URL
     * form for the host + path. */
    buf[i++] = hex_lc[(err_code >> 12) & 0xF];
    buf[i++] = hex_lc[(err_code >>  8) & 0xF];
    buf[i++] = hex_lc[(err_code >>  4) & 0xF];
    buf[i++] = hex_lc[ err_code        & 0xF];
    buf[i] = '\0';
    return i;
}

static void qr_render_error_url(UINT32 err_code)
{
    char url[48];
    int i;

    if (format_recovery_url(url, sizeof url, err_code) == 0) return;

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

/* ============================================================================
 * S18: Graphical Error Screen (ChromeOS/Win11-style BSOD)
 *
 * Renders a pixel-accurate blue-screen layout directly to the GOP
 * framebuffer.  Falls back silently when the framebuffer is headless,
 * too small, or uses a BitMask pixel format that we cannot pack.
 * The ConOut text path in boot_fatal() stays unchanged as the fallback
 * when graphical rendering is not possible (pre-EBS only; post-EBS
 * ConOut is disabled by the g_ebs_in_progress guard).
 *
 * Components:
 *   1. fb_pack_rgb(r,g,b)         -- format-aware pixel packing
 *   2. bsod_font[128][8]          -- 8x8 bitmap font (ported from boot_halt.c)
 *   3. bsod_blit_char / string    -- per-glyph blit with clip
 *   4. bsod_blit_string_scaled()  -- pixel-doubled title/headline text
 *   5. bsod_sad_face[16]          -- 16x16 sad face pixel art
 *   6. bsod_blit_sad_face()       -- scale 4x to 64x64
 *   7. bsod_fill_rect()           -- solid color rect
 *   8. bsod_render_graphical()    -- full layout: fill + icon + text + QR
 * ============================================================================ */

/* Pack (R, G, B) into a 32-bit GOP pixel using the active pixel format.
 * Returns 0 on BitMask format (caller should skip colored rendering). */
static UINT32 fb_pack_rgb(UINT8 r, UINT8 g, UINT8 b)
{
    if (gFbPixelFormat == 0) {
        /* RGBX: byte 0 = R, byte 1 = G, byte 2 = B */
        return ((UINT32)r) | ((UINT32)g << 8) | ((UINT32)b << 16);
    }
    if (gFbPixelFormat == 1) {
        /* BGRX: byte 0 = B, byte 1 = G, byte 2 = R */
        return ((UINT32)b) | ((UINT32)g << 8) | ((UINT32)r << 16);
    }
    /* BitMask or unknown: safe fallback to black (0x00000000 is
     * palindromic and renders as black on every format). */
    return 0x00000000;
}

/* ---- Antialiased font atlas (Selawik Semibold) -------------------------
 *
 * Pre-rendered at build time from resources/fonts/selawksb.ttf via
 * Python/Pillow.  Two sizes: 28px (title/error-code) and 16px (body/
 * hints/URL).  Each glyph is stored as an 8-bit alpha bitmap with
 * variable width and metrics (bearing, advance).  The renderer
 * alpha-composites each pixel against the background color for
 * smooth antialiased text -- the same quality as a desktop TTF
 * renderer but without runtime font parsing.
 *
 * Data files:
 *   bsod_font_title.inc  -- 28px, ~28 KB alpha data
 *   bsod_font_body.inc   -- 16px, ~9 KB alpha data
 * ------------------------------------------------------------------- */

struct bsod_aa_glyph {
    UINT8  width;
    UINT8  height;
    INT8   bearing_x;
    INT8   bearing_y;
    UINT8  advance;
    UINT8  _pad;
    UINT16 data_offset;
};

#include "bsod_font_title.inc"
#include "bsod_font_sub.inc"
#include "bsod_font_body.inc"

/* Render one antialiased glyph at (px, py) where py is the TOP of the
 * line (not the baseline).  bearing_y positions the glyph vertically
 * relative to the ascent line.  Blends fg onto bg per pixel alpha. */
static void bsod_aa_char(UINT32 px, UINT32 py,
                          const struct bsod_aa_glyph *g,
                          const UINT8 *data,
                          UINT32 ascent,
                          UINT8 fg_r, UINT8 fg_g, UINT8 fg_b,
                          UINT8 bg_r, UINT8 bg_g, UINT8 bg_b)
{
    UINT32 row, col;
    const UINT8 *src;
    if (g->width == 0 || g->height == 0) return;
    src = data + g->data_offset;
    for (row = 0; row < g->height; row++) {
        UINT32 y = py + (UINT32)((int)ascent + (int)g->bearing_y) + row;
        if (y >= gFbHeight) break;
        for (col = 0; col < g->width; col++) {
            UINT8 a = src[row * g->width + col];
            UINT32 x;
            if (a == 0) continue;
            x = px + (UINT32)((int)g->bearing_x + (int)col);
            if (x >= gFbWidth) continue;
            if (a == 255) {
                gFramebuffer[y * gFbPitch + x] = fb_pack_rgb(fg_r, fg_g, fg_b);
            } else {
                UINT8 inv = (UINT8)(255 - a);
                UINT8 r = (UINT8)(((UINT32)fg_r * a + (UINT32)bg_r * inv + 127) / 255);
                UINT8 g2 = (UINT8)(((UINT32)fg_g * a + (UINT32)bg_g * inv + 127) / 255);
                UINT8 b = (UINT8)(((UINT32)fg_b * a + (UINT32)bg_b * inv + 127) / 255);
                gFramebuffer[y * gFbPitch + x] = fb_pack_rgb(r, g2, b);
            }
        }
    }
}

/* Extra letter spacing added to each glyph advance for readability.
 * Selawik's default metrics are tight for a BSOD context viewed from
 * a distance; +1px gives the text room to breathe. */
#define BSOD_AA_TRACKING  1

/* Render an antialiased string using the given font atlas. */
static void bsod_aa_string(UINT32 px, UINT32 py, const char *str,
                             const struct bsod_aa_glyph *glyphs,
                             const UINT8 *data, UINT32 ascent,
                             UINT8 fg_r, UINT8 fg_g, UINT8 fg_b,
                             UINT8 bg_r, UINT8 bg_g, UINT8 bg_b)
{
    UINT32 cx = px;
    while (*str) {
        unsigned char ch = (unsigned char)*str;
        if (ch < 0x20 || ch > 0x7E) ch = '?';
        const struct bsod_aa_glyph *g = &glyphs[ch - 0x20];
        if (cx + g->advance > gFbWidth) break;
        bsod_aa_char(cx, py, g, data, ascent,
                      fg_r, fg_g, fg_b, bg_r, bg_g, bg_b);
        cx += g->advance + BSOD_AA_TRACKING;
        str++;
    }
}

/* Compute the pixel width of an AA string (includes tracking). */
static UINT32 bsod_aa_string_width(const char *str,
                                     const struct bsod_aa_glyph *glyphs)
{
    UINT32 w = 0;
    UINT32 n = 0;
    while (*str) {
        unsigned char ch = (unsigned char)*str;
        if (ch < 0x20 || ch > 0x7E) ch = '?';
        w += glyphs[ch - 0x20].advance;
        n++;
        str++;
    }
    if (n > 0) w += (n - 1) * BSOD_AA_TRACKING;
    return w;
}

/* Word-wrap an AA string into lines of at most max_px width. */
static void bsod_aa_wrapped(UINT32 px, UINT32 py, const char *text,
                              const struct bsod_aa_glyph *glyphs,
                              const UINT8 *data, UINT32 ascent,
                              UINT32 line_h, UINT32 max_px,
                              UINT32 max_lines,
                              UINT8 fg_r, UINT8 fg_g, UINT8 fg_b,
                              UINT8 bg_r, UINT8 bg_g, UINT8 bg_b)
{
    UINT32 line = 0;
    UINT32 start = 0;
    char buf[128];

    if (!text) return;

    while (text[start] && line < max_lines) {
        UINT32 end = start;
        UINT32 last_space = start;
        UINT32 have_space = 0;
        UINT32 cur_w = 0;

        while (text[end] && cur_w < max_px) {
            unsigned char ch = (unsigned char)text[end];
            if (ch < 0x20 || ch > 0x7E) ch = '?';
            cur_w += glyphs[ch - 0x20].advance + BSOD_AA_TRACKING;
            if (text[end] == ' ') { last_space = end; have_space = 1; }
            end++;
        }
        if (text[end] != 0 && have_space && cur_w >= max_px)
            end = last_space;

        {
            UINT32 n = end - start;
            UINT32 k;
            if (n >= sizeof(buf)) n = sizeof(buf) - 1;
            for (k = 0; k < n; k++) buf[k] = text[start + k];
            buf[n] = '\0';
        }
        bsod_aa_string(px, py + line * line_h, buf,
                         glyphs, data, ascent,
                         fg_r, fg_g, fg_b, bg_r, bg_g, bg_b);
        line++;
        start = end;
        while (text[start] == ' ') start++;
    }
}

/* Legacy 8x8 bitmap font and its blit helpers removed -- superseded by
 * the antialiased Selawik Semibold atlas above.  The kernel's
 * boot_halt.c retains its own copy of the 8x8 font for the halt
 * screen.  BSOD_FONT_W/H kept for the QR caption_reserve calc. */
#define BSOD_FONT_W 8
#define BSOD_FONT_H 8

/* 100x100 antialiased sad face icon from resources/bsod.png.
 * 8-bit alpha per pixel (0 = transparent, 255 = fully opaque).
 * 10,000 bytes in .rodata.  The renderer alpha-blends each pixel
 * with the background color for smooth edges. */
#define BSOD_ICON_W 100
#define BSOD_ICON_H 100

#include "bsod_icon_aa.inc"

/* Render the 100x100 antialiased icon at (px, py).
 * Alpha-composites each pixel: result = bg + (fg - bg) * alpha / 255
 * using the formula: (fg * a + bg * (255 - a) + 127) / 255
 * which avoids signed subtraction and is always correct. */
static void bsod_blit_icon_aa(UINT32 px, UINT32 py,
                                UINT8 fg_r, UINT8 fg_g, UINT8 fg_b,
                                UINT8 bg_r, UINT8 bg_g, UINT8 bg_b)
{
    UINT32 row, col;
    for (row = 0; row < BSOD_ICON_H; row++) {
        UINT32 y = py + row;
        if (y >= gFbHeight) break;
        for (col = 0; col < BSOD_ICON_W; col++) {
            UINT8 a = bsod_icon_aa[row * BSOD_ICON_W + col];
            UINT32 x;
            if (a == 0) continue;
            x = px + col;
            if (x >= gFbWidth) break;
            if (a == 255) {
                gFramebuffer[y * gFbPitch + x] = fb_pack_rgb(fg_r, fg_g, fg_b);
            } else {
                UINT8 inv = (UINT8)(255 - a);
                UINT8 r = (UINT8)(((UINT32)fg_r * a + (UINT32)bg_r * inv + 127) / 255);
                UINT8 g = (UINT8)(((UINT32)fg_g * a + (UINT32)bg_g * inv + 127) / 255);
                UINT8 b = (UINT8)(((UINT32)fg_b * a + (UINT32)bg_b * inv + 127) / 255);
                gFramebuffer[y * gFbPitch + x] = fb_pack_rgb(r, g, b);
            }
        }
    }
}

/* Fill [x0, x0+w) x [y0, y0+h) with `color`. */
static void bsod_fill_rect(UINT32 x0, UINT32 y0, UINT32 w, UINT32 h, UINT32 color)
{
    UINT32 y, x;
    for (y = 0; y < h; y++) {
        UINT32 fy = y0 + y;
        if (fy >= gFbHeight) break;
        for (x = 0; x < w; x++) {
            UINT32 fx = x0 + x;
            if (fx >= gFbWidth) break;
            gFramebuffer[fy * gFbPitch + fx] = color;
        }
    }
}

/* Full graphical BSOD layout.  Caller MUST have verified:
 *   - gFramebuffer != NULL
 *   - gFbWidth >= 800 && gFbHeight >= 600
 *   - gFbPixelFormat != 2 (RGBX or BGRX)
 * Fills the entire screen and renders every overlay.  Does NOT wait
 * for a keypress -- caller handles that. */
static void bsod_render_graphical(UINT32 err_code, const char *title,
                                    const char *detail)
{
    UINT32 blue   = fb_pack_rgb(0x0A, 0x0A, 0x0A);  /* near-black background */
    UINT32 i;
    char err_buf[32];
    const char hex[] = "0123456789ABCDEF";

    /* 1. Fill entire framebuffer with blue. */
    bsod_fill_rect(0, 0, gFbWidth, gFbHeight, blue);

    /* 2. Sad face icon centered horizontally, 100x100 native pixels
     *    from resources/bsod.png with 8-bit alpha antialiasing.
     *    Blends white foreground onto the blue background. */
    bsod_blit_icon_aa(gFbWidth / 2 - BSOD_ICON_W / 2, 30,
                       0xFF, 0xFF, 0xFF,   /* fg: white */
                       0x0A, 0x0A, 0x0A);  /* bg: near-black */

    /* Shorthand macros for the three AA font sizes.  bg = BSOD blue. */
#define AA_TITLE(px, py, str, r, g, b) \
    bsod_aa_string((px), (py), (str), bsod_aa_TITLE, bsod_aa_TITLE_data, \
                    BSOD_AA_TITLE_ASCENT, (r), (g), (b), 0x0A, 0x0A, 0x0A)
#define AA_SUB(px, py, str, r, g, b) \
    bsod_aa_string((px), (py), (str), bsod_aa_SUB, bsod_aa_SUB_data, \
                    BSOD_AA_SUB_ASCENT, (r), (g), (b), 0x0A, 0x0A, 0x0A)
#define AA_BODY(px, py, str, r, g, b) \
    bsod_aa_string((px), (py), (str), bsod_aa_BODY, bsod_aa_BODY_data, \
                    BSOD_AA_BODY_ASCENT, (r), (g), (b), 0x0A, 0x0A, 0x0A)

    /* 3. Title (36px Selawik Semibold), centered horizontally.
     *    Positioned 20px below the icon bottom (icon at y=30, h=100). */
    {
        const char *t = "This wasn't supposed to happen.";
        UINT32 w = bsod_aa_string_width(t, bsod_aa_TITLE);
        UINT32 x = (gFbWidth > w) ? (gFbWidth - w) / 2 : 8;
        AA_TITLE(x, 150, t, 0xFF, 0xFF, 0xFF);
    }

    /* 4. Error code subtitle (22px Selawik Semibold, left-aligned).
     *    Clear gap below 36px title establishes visual hierarchy:
     *    36px title >> 22px subtitle >> 16px body. */
    {
        i = 0;
        err_buf[i++] = 'C'; err_buf[i++] = 'o'; err_buf[i++] = 'd';
        err_buf[i++] = 'e'; err_buf[i++] = ' ';
        err_buf[i++] = '0'; err_buf[i++] = 'x';
        /* 8 hex digits -- UINT32 contract; matches serial + ConOut
         * fatal lines. */
        err_buf[i++] = hex[(err_code >> 28) & 0xF];
        err_buf[i++] = hex[(err_code >> 24) & 0xF];
        err_buf[i++] = hex[(err_code >> 20) & 0xF];
        err_buf[i++] = hex[(err_code >> 16) & 0xF];
        err_buf[i++] = hex[(err_code >> 12) & 0xF];
        err_buf[i++] = hex[(err_code >>  8) & 0xF];
        err_buf[i++] = hex[(err_code >>  4) & 0xF];
        err_buf[i++] = hex[(err_code >>  0) & 0xF];
        err_buf[i++] = ':';
        err_buf[i++] = ' ';
        err_buf[i] = '\0';
        {
            UINT32 w1 = bsod_aa_string_width(err_buf, bsod_aa_SUB);
            AA_SUB(80, 250, err_buf, 0xB2, 0xD8, 0xFF);
            if (title)
                AA_SUB(80 + w1, 250, title, 0xFF, 0xFF, 0xFF);
        }
    }

    /* 5. Detail text wrapped (16px body font). */
    if (detail) {
        bsod_aa_wrapped(80, 310, detail,
                         bsod_aa_BODY, bsod_aa_BODY_data,
                         BSOD_AA_BODY_ASCENT, BSOD_AA_BODY_LINE_H,
                         gFbWidth - 160, 3,
                         0xA0, 0xC0, 0xE0,   /* dim blue-white */
                         0x0A, 0x0A, 0x0A);
    }

    /* 6. Recovery hint lines (16px body font). */
    AA_BODY(80, 390, "Here's what you can do:", 0xFF, 0xFF, 0xFF);
    AA_BODY(100, 414, "- Make sure your boot drive is connected", 0xB2, 0xD8, 0xFF);
    AA_BODY(100, 438, "- Check that \\boot\\kernel.exe is on the drive", 0xB2, 0xD8, 0xFF);
    AA_BODY(100, 462, "- Scan the QR code below for help", 0xB2, 0xD8, 0xFF);
    AA_BODY(100, 486, "- Press any key to restart", 0xB2, 0xD8, 0xFF);

#undef AA_TITLE
#undef AA_SUB
#undef AA_BODY

    /* 7. QR code + URL caption, bottom-right.
     *
     * The existing qr_render_error_url() places the QR with a fixed
     * 12-pixel bottom margin, which leaves no room for a caption
     * line below.  For the graphical BSOD we inline the QR encoding
     * here and position the QR higher so the URL text fits in the
     * reserved bottom strip.  qr_render_to_fb is called directly.
     * The URL itself is built by format_recovery_url() so QR payload
     * and caption text match qr_render_error_url's contract exactly. */
    {
        char url[48];
        int k;
        UINT8 data_cw[QR_DATA_CW];
        UINT8 ec_cw[QR_EC_LEN];
        UINT8 all_cw[QR_TOTAL_CW];
        UINT8 matrix[QR_SIZE][QR_SIZE];
        UINT8 used[QR_SIZE][QR_SIZE];
        UINT32 mod;
        UINT32 quiet_zone;
        UINT32 qr_total;
        UINT32 side_margin = 12;
        UINT32 caption_reserve;
        UINT32 url_w;
        UINT32 qr_x, qr_y;
        UINT32 url_x, url_y;

        /* format_recovery_url is structurally defensive (returns 0
         * only when the buffer is < RECOVERY_URL_MIN_BUF; we pass 48
         * which is comfortably above 37). On the impossible failure
         * path, skip QR rendering rather than aborting the rest of
         * the BSOD -- title/error code/caption already drew. */
        if (format_recovery_url(url, sizeof url, err_code) == 0)
            goto skip_qr;

        /* Match the mod-size selection from qr_render_error_url so the
         * QR has the same visual scale across renderers. */
        mod = 4;
        if (gFbWidth >= 1920) mod = 6;
        if (gFbWidth >= 2560) mod = 8;
        quiet_zone = mod * 4;
        qr_total = (UINT32)(QR_SIZE * mod + quiet_zone * 2);

        /* Reserve space below the QR for the URL caption.  The QR
         * is pushed up by exactly caption_reserve pixels so the
         * URL fits between the QR bottom and the screen edge. */
        caption_reserve = BSOD_AA_BODY_LINE_H + 14;

        if (gFbWidth >= qr_total + side_margin &&
            gFbHeight >= qr_total + side_margin + caption_reserve) {

            if (qr_encode_data(url, data_cw) != 0) {
                qr_reed_solomon(data_cw, QR_DATA_CW, ec_cw);
                for (k = 0; k < QR_DATA_CW; k++) all_cw[k] = data_cw[k];
                for (k = 0; k < QR_EC_LEN; k++)
                    all_cw[QR_DATA_CW + k] = ec_cw[k];
                qr_place_patterns(matrix, used);
                qr_place_data(matrix, used, all_cw, QR_TOTAL_CW);
                qr_apply_mask_and_format(matrix);

                /* QR pushed up from bottom edge by side_margin +
                 * caption_reserve so the URL text fits below it. */
                qr_x = gFbWidth  - qr_total - side_margin;
                qr_y = gFbHeight - qr_total - side_margin - caption_reserve;
                qr_render_to_fb(matrix, qr_x, qr_y, mod);

                /* URL caption centered below the QR with a 4px gap. */
                url_w = bsod_aa_string_width(url, bsod_aa_BODY);
                if (qr_total >= url_w) {
                    url_x = qr_x + (qr_total - url_w) / 2;
                } else if (gFbWidth >= url_w + 2 * side_margin) {
                    url_x = gFbWidth - side_margin - url_w;
                } else {
                    url_x = side_margin;
                    if (url_x + url_w > gFbWidth)
                        url_x = gFbWidth;  /* sentinel: skip */
                }
                url_y = qr_y + qr_total + 1;
                if (url_x + url_w <= gFbWidth &&
                    url_y + BSOD_AA_BODY_LINE_H <= gFbHeight) {
                    bsod_aa_string(url_x, url_y, url,
                                    bsod_aa_BODY, bsod_aa_BODY_data,
                                    BSOD_AA_BODY_ASCENT,
                                    0xB2, 0xD8, 0xFF,
                                    0x0A, 0x0A, 0x0A);
                }
            }
        }
    skip_qr:;
    }
}

/* Gate predicate: is the framebuffer usable for the graphical BSOD? */
static int bsod_can_render_graphical(void)
{
    return gFramebuffer != (UINT32 *)0 &&
           gFbWidth  >= 800 &&
           gFbHeight >= 600 &&
           gFbPixelFormat != 2;  /* skip BitMask format */
}

/* Hold the fatal error screen on-screen long enough for a human to
 * read it and scan the QR code before boot_fatal() calls ResetSystem.
 *
 * Pre-EBS path: uses gBS->Stall() for accurate 10-second wall-clock
 * dwell with 50 ms keypress polling for early exit.  ConIn->Reset
 * runs once before polling to flush any stale buffered keystrokes
 * that would otherwise skip the dwell immediately.
 *
 * Post-EBS path: RuntimeServices->GetTime is the wall-clock primary
 * (survives ExitBootServices).  TSC is the inner throttle so each
 * outer iteration calls GetTime at most ~10 times per second --
 * GetTime is firmware/SMM-backed and a tight loop would issue a
 * runtime-services storm.  TSC-only fallback applies when GetTime
 * is unavailable or returns EFI_ERROR. */
static void boot_fatal_dwell(void)
{
    const UINT64 dwell_ms = 10000;

    if (!g_ebs_in_progress && gBS && gBS->Stall) {
        /* Pre-EBS path: gBS->Stall for accurate wall clock.  Flush
         * any stale buffered keystrokes from firmware menus or
         * keyboard init before polling (Codex quality finding: a
         * leftover key event would skip dwell immediately). */
        UINT64 elapsed_ms = 0;
        if (gST && gST->ConIn && gST->ConIn->Reset)
            gST->ConIn->Reset(gST->ConIn, 0);
        while (elapsed_ms < dwell_ms) {
            if (gST && gST->ConIn) {
                EFI_INPUT_KEY key;
                EFI_STATUS rs = gST->ConIn->ReadKeyStroke(gST->ConIn, &key);
                if (!EFI_ERROR(rs)) return;  /* keypress -- bail */
            }
            gBS->Stall(50000);  /* 50 ms per poll */
            elapsed_ms += 50;
        }
        return;
    }

    /* Post-EBS: RuntimeServices->GetTime for accurate wall clock.
     * GetTime survives ExitBootServices (it's a runtime service).
     * EFI_TIME layout: Year(2)+Month(1)+Day(1)+Hour(1)+Minute(1)+Second(1)+...
     * Our efi.h declares GetTime as (VOID*, VOID*), so we use a raw
     * byte buffer and read Hour/Minute/Second at offsets 4/5/6.
     *
     * GetTime is firmware/SMM-backed on real hardware; calling it in
     * a tight loop would issue a runtime-services storm for the full
     * 10s dwell.  Throttle calls to ~10/sec via an inner TSC spin
     * (~100ms per outer iteration), keeping wall-clock accuracy
     * while reducing GetTime invocations by ~5 orders of magnitude. */
    if (gST && gST->RuntimeServices && gST->RuntimeServices->GetTime) {
        UINT8 tbuf0[20], tbuf[20];
        EFI_STATUS gs = gST->RuntimeServices->GetTime(tbuf0, (void *)0);
        if (!EFI_ERROR(gs)) {
            UINT64 s0 = (UINT64)tbuf0[4] * 3600 + tbuf0[5] * 60 + tbuf0[6];
            for (;;) {
                UINT64 sn;
                /* TSC-based ~100ms throttle.  300M ticks at 3 GHz =
                 * ~100ms; on slower CPUs the throttle is longer
                 * which only reduces firmware-call rate further. */
                UINT64 throttle_start, throttle_now;
                UINT32 lo, hi;
                __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
                throttle_start = ((UINT64)hi << 32) | lo;
                for (;;) {
                    __asm__ volatile("pause");
                    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
                    throttle_now = ((UINT64)hi << 32) | lo;
                    if (throttle_now - throttle_start >= 300000000ULL)
                        break;
                }
                gs = gST->RuntimeServices->GetTime(tbuf, (void *)0);
                if (EFI_ERROR(gs)) break;
                sn = (UINT64)tbuf[4] * 3600 + tbuf[5] * 60 + tbuf[6];
                if (sn < s0) break;  /* midnight rollover -- bail */
                if (sn >= s0 + 10) return;
            }
        }
    }

    /* Last-resort TSC fallback if GetTime failed or runtime services
     * are unavailable.  20 * 10^9 TSC ticks is ~4-20 seconds depending
     * on CPU frequency -- coarse but better than zero dwell. */
    {
        UINT64 start, now;
        UINT32 lo, hi;
        __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
        start = ((UINT64)hi << 32) | lo;
        for (;;) {
            __asm__ volatile("pause");
            __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
            now = ((UINT64)hi << 32) | lo;
            if (now - start >= 20000000000ULL) return;
        }
    }
}

static __attribute__((noreturn)) void boot_fatal(UINT32 err_code,
                                                  const char *title,
                                                  const char *detail)
{
    /* 1. Log to serial FIRST.  The full 8-hex-digit error code matches
     * the UINT32 contract on boot_fatal / nvram_write_boot_error /
     * boot_info.last_boot_error; truncating to 16 bits diverges the
     * displayed code from the persisted code.
     *
     * Serial precedes the NVRAM write so a slow / wedged firmware
     * SetVariable cannot block the only synchronous diagnostic the
     * operator has during a boot-services-era fatal. */
    serial_early_print("[CRIT] BOOT FATAL (0x");
    serial_early_print_hex16((UINT16)(err_code >> 16));
    serial_early_print_hex16((UINT16)err_code);
    serial_early_print("): ");
    serial_early_print(title);
    serial_early_print("\n");
    if (detail) {
        serial_early_print("[CRIT]   ");
        serial_early_print(detail);
        serial_early_print("\n");
    }

    /* 1b. Persist error code in NVRAM for next-boot diagnostics (S13).
     * Runs after serial so a slow/wedged SetVariable cannot starve
     * the operator's only fatal-path diagnostic. */
    nvram_write_boot_error(err_code);

    /* 1c. Append a history-ring entry so multi-attempt diagnostics
     * (BootHistorySeq cookie + BootErrorHistory ring) capture
     * ordering across cascading boot failures.  err_code already fits
     * UINT16 (registry max 0x0013); cast is safe.  An UNKNOWN
     * source_section means the failing path forgot to call
     * boot_set_section() -- WARN so the operator can grep coverage. */
    if (g_boot_section == BOOT_SECTION_UNKNOWN) {
        serial_early_print("[WARN] boot_history: source_section UNKNOWN at fatal\n");
    }
    boot_history_append(g_boot_section, (UINT16)err_code);

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

        /* Error code line -- 8 hex digits (UINT32 width matches the
         * boot_fatal/nvram/boot_info contract). */
        efi_print(u"  Error code: 0x");
        {
            CHAR16 hex_buf[9];
            CHAR16 hex_chars[] = u"0123456789ABCDEF";
            hex_buf[0] = hex_chars[(err_code >> 28) & 0xF];
            hex_buf[1] = hex_chars[(err_code >> 24) & 0xF];
            hex_buf[2] = hex_chars[(err_code >> 20) & 0xF];
            hex_buf[3] = hex_chars[(err_code >> 16) & 0xF];
            hex_buf[4] = hex_chars[(err_code >> 12) & 0xF];
            hex_buf[5] = hex_chars[(err_code >> 8) & 0xF];
            hex_buf[6] = hex_chars[(err_code >> 4) & 0xF];
            hex_buf[7] = hex_chars[err_code & 0xF];
            hex_buf[8] = 0;
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

        /* S18: Render full graphical BSOD when the framebuffer is
         * usable.  Covers the blue background, sad face icon, title,
         * error code, description, QR, and URL.  Fallback when the
         * framebuffer is headless / too small / BitMask format: the
         * ConOut text above is the only on-screen signal. */
        if (bsod_can_render_graphical()) {
            bsod_render_graphical(err_code, title, detail);
        } else if (gFramebuffer && gFbWidth > 0 && gFbHeight > 0) {
            /* Low-res or BitMask: only the QR still works reliably. */
            qr_render_error_url(err_code);
        }
    } else if (bsod_can_render_graphical()) {
        /* EBS in progress or no ConOut -- ConOut is not safe, but direct
         * framebuffer writes are.  Render the full graphical BSOD; the
         * serial log above still has the critical details. */
        bsod_render_graphical(err_code, title, detail);
    } else if (gFramebuffer && gFbWidth > 0 && gFbHeight > 0) {
        /* Post-EBS with unusable framebuffer format -- only the QR
         * renderer works because it uses palindromic B/W pixels that
         * render correctly on every format. */
        qr_render_error_url(err_code);
    }

    /* S18: unified dwell.  Hold the fatal screen visible for the user
     * before ResetSystem in all branches (pre-EBS, post-EBS, graphical
     * or QR-only, with or without ConIn).  Uses gBS->Stall pre-EBS and
     * a TSC spin post-EBS so the dwell is time-based, not iteration-
     * based.  A naive counter-decrement loop can finish in
     * milliseconds when ReadKeyStroke returns EFI_NOT_READY
     * quickly. */
    boot_fatal_dwell();

    /* 3. If error_screen_test, skip reboot so the screen stays visible. */
    if (g_boot_info_ptr && g_boot_info_ptr->config.error_screen_test) {
        serial_early_print("[BOOT] error_screen_test: halting (screen stays visible)\n");
        for (;;) __asm__ volatile("hlt");
    }

    /* 4. Attempt cold reboot via RuntimeServices (works even after EBS failure
     * since ResetSystem is a runtime service, not a boot service). */
    if (gST && gST->RuntimeServices)
        gST->RuntimeServices->ResetSystem(
            EFI_RESET_COLD, 0, 0, (VOID *)0);

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

/* Score and select the best 32bpp GOP mode, call SetMode, set hidpi flag.
 *
 * Hostile-firmware hardening:
 *   - Loop is bounded by BOOT_GOP_MODE_MAX (matches init_gop's enumeration
 *     cap) AND by a consecutive-error counter, so a firmware that reports
 *     MaxMode = 0xFFFFFFFF or that returns garbage from QueryMode cannot
 *     hang the bootloader.
 *   - Every successful QueryMode buffer is FreePool'd (UEFI 2.10 spec
 *     12.9.2.4: caller releases callee-allocated info).
 *   - info pointer + info_size are validated before dereference.
 */
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
    UINT32  query_errors = 0;
    UINT32  scan_cap = gop->Mode->MaxMode;
    if (scan_cap > BOOT_GOP_MODE_MAX) scan_cap = BOOT_GOP_MODE_MAX;

    for (i = 0; i < scan_cap; i++) {
        UINTN info_size = 0;
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = (EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *)0;
        if (EFI_ERROR(gop->QueryMode(gop, i, &info_size, &info))) {
            query_errors++;
            if (query_errors >= 100) {
                serial_early_print("[BOOT] GOP: negotiate aborted after 100 errors\n");
                break;
            }
            continue;
        }
        query_errors = 0;
        /* UEFI spec: SUCCESS always populates info; defense-in-depth check */
        if (!info || info_size < sizeof(*info)) {
            if (info) gBS->FreePool(info);
            continue;
        }

        EFI_GRAPHICS_PIXEL_FORMAT pf = info->PixelFormat;
        UINT32 w = info->HorizontalResolution;
        UINT32 h = info->VerticalResolution;
        UINT32 pps = info->PixelsPerScanLine;
        gBS->FreePool(info);

        /* Only consider 32bpp modes with valid pitch */
        if (pf != PixelBlueGreenRedReserved && pf != PixelRedGreenBlueReserved)
            continue;
        if (pps < w)
            continue;  /* corrupt pitch */

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

/* ---- Multi-GPU GOP enumeration (TODO-27 sec4) -----------------------------
 * Firmware can expose more than one EFI_GRAPHICS_OUTPUT_PROTOCOL handle (iGPU +
 * dGPU, or several connected panels). init_gop enumerates every GOP handle,
 * records per-handle geometry into boot_info.gop_handles[] for the kernel
 * multi-head driver (04-drivers-hardware/TODO-17 sec6), and drives the
 * framebuffer publish on the single PRIMARY handle so boot_info.fb stays
 * byte-identical to the single-GPU case. Primary selection prefers a
 * device-path match against the firmware's gST->ConsoleOutHandle (the
 * authoritative active console), then the EFI_CONSOLE_OUT_DEVICE_GUID marker as
 * a tie-breaker, then largest resolution. ------------------------------------*/

/* Bounded byte-equality (no CompareMem in freestanding UEFI). */
static BOOLEAN gop_bytes_equal(const UINT8 *a, const UINT8 *b, UINTN n)
{
    UINTN i;
    for (i = 0; i < n; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

/* TRUE when the device path of `h` is a prefix of (or equal to) the ConsoleOut
 * device path -- i.e. `h` is the firmware's active console graphics device.
 * Every access stays within the GetDevicePathSize-measured object; a degenerate
 * or oversized path yields FALSE rather than a self-walk past the object. */
static BOOLEAN gop_handle_is_conout(EFI_HANDLE h,
                                    const EFI_DEVICE_PATH_PROTOCOL *conout_dp,
                                    UINTN conout_sz,
                                    EFI_DEVICE_PATH_UTILITIES_PROTOCOL *dpu)
{
    EFI_GUID dp_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
    EFI_DEVICE_PATH_PROTOCOL *hdp = (EFI_DEVICE_PATH_PROTOCOL *)0;
    UINTN hsz, off = 0, prefix_len = 0;

    if (!h || !conout_dp || conout_sz < 4u || !dpu || !dpu->GetDevicePathSize)
        return 0;
    if (EFI_ERROR(gBS->HandleProtocol(h, &dp_guid, (VOID **)&hdp)) || !hdp)
        return 0;
    hsz = dpu->GetDevicePathSize(hdp);
    if (hsz < 4u || hsz > 65536u)
        return 0;
    /* Find this handle's END node strictly inside [0, hsz); prefix_len is the
     * span of real nodes before it (UEFI spec Table 10-1: END is Type 0x7F
     * SubType 0xFF -- both checked, never Type alone). */
    for (;;) {
        UINTN nlen;
        const EFI_DEVICE_PATH_PROTOCOL *node;
        if (off + 4u > hsz)
            return 0;
        node = (const EFI_DEVICE_PATH_PROTOCOL *)((const UINT8 *)hdp + off);
        nlen = (UINTN)node->Length[0] | ((UINTN)node->Length[1] << 8);
        if (nlen < 4u || off + nlen > hsz)
            return 0;
        if (node->Type == EFI_DP_TYPE_END &&
            node->SubType == EFI_DP_SUBTYPE_END_ENTIRE) {
            prefix_len = off;
            break;
        }
        off += nlen;
    }
    if (prefix_len == 0u || prefix_len > conout_sz)
        return 0;
    return gop_bytes_equal((const UINT8 *)hdp, (const UINT8 *)conout_dp, prefix_len);
}

/* Read one GOP handle's current mode into a boot_info.gop_handles[] entry.
 * Geometry is always recorded when Mode->Info is present; fb_addr/fb_size/
 * fb_valid are set ONLY when the framebuffer passes the SAME validation the
 * primary publish path enforces -- a secondary head whose mode was never
 * SetMode-configured must never hand the kernel a stale or non-display MMIO
 * range to map. */
static void gop_record_handle(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop,
                              struct boot_gop_handle *out)
{
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info;
    out->fb_addr = 0; out->fb_size = 0;
    out->width = 0; out->height = 0; out->pitch = 0;
    out->pixel_format = 2; out->is_primary = 0; out->fb_valid = 0; out->pad = 0;
    if (!gop || !gop->Mode || !gop->Mode->Info)
        return;
    info = gop->Mode->Info;
    out->width  = info->HorizontalResolution;
    out->height = info->VerticalResolution;
    out->pitch  = info->PixelsPerScanLine * 4u;
    out->pixel_format = gop_pixel_format_code(info->PixelFormat);
    /* Validation gauntlet -- mirror of init_gop's primary publish checks. */
    if (gop->Mode->FrameBufferBase == 0)
        return;
    if (info->PixelFormat != PixelRedGreenBlueReserved &&
        info->PixelFormat != PixelBlueGreenRedReserved)
        return;
    if (info->PixelsPerScanLine < info->HorizontalResolution ||
        info->VerticalResolution == 0 || info->PixelsPerScanLine == 0 ||
        info->VerticalResolution > (0xFFFFFFFFu / 4u) ||
        info->PixelsPerScanLine > (0xFFFFFFFFu / 4u) ||
        (UINTN)info->VerticalResolution >
            ((UINTN)~(UINTN)0 / (UINTN)info->PixelsPerScanLine) ||
        ((UINTN)info->VerticalResolution * (UINTN)info->PixelsPerScanLine) >
            ((UINTN)~(UINTN)0 / 4u))
        return;
    {
        UINTN required = (UINTN)info->VerticalResolution *
                         (UINTN)info->PixelsPerScanLine * 4u;
        UINTN fb_size = gop->Mode->FrameBufferSize;
        /* Require a firmware-reported FrameBufferSize that covers the surface.
         * fb_size==0 (firmware did not report a size) is treated as unusable:
         * the dimension checks above are internally consistent but cannot bound
         * the geometry against actual VRAM, so fb_size is the only VRAM-extent
         * signal. UEFI 2.10 requires FrameBufferSize to be valid. */
        if (required == 0 || fb_size < required)
            return;
        out->fb_addr  = (UINT64)gop->Mode->FrameBufferBase;
        out->fb_size  = (UINT64)fb_size;
        out->fb_valid = 1;
    }
}

/* Enumerate all GOP handles, populate boot_info.gop_handles[], pick the primary.
 * Returns the primary handle's GOP protocol via *out_primary and its index via
 * *out_primary_idx; EFI_NOT_FOUND when no GOP handle exists (caller goes
 * headless). Sets boot_info.gop_handle_count. */
static EFI_STATUS gop_enumerate_and_select(EFI_GRAPHICS_OUTPUT_PROTOCOL *out_gops[],
                                           UINT32 *out_count)
{
    EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_GUID dp_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
    EFI_GUID dpu_guid = EFI_DEVICE_PATH_UTILITIES_PROTOCOL_GUID;
    EFI_GUID con_guid = EFI_CONSOLE_OUT_DEVICE_GUID;
    EFI_HANDLE *handles = (EFI_HANDLE *)0;
    EFI_DEVICE_PATH_PROTOCOL *conout_dp = (EFI_DEVICE_PATH_PROTOCOL *)0;
    EFI_DEVICE_PATH_UTILITIES_PROTOCOL *dpu = (EFI_DEVICE_PATH_UTILITIES_PROTOCOL *)0;
    UINTN count = 0, conout_sz = 0, i;
    UINTN primary_i, conout_i, guidvalid_i, valid_i, any_i;
    UINT64 guidvalid_area = 0, valid_area = 0, any_area = 0;
    UINT32 n = 0;
    EFI_STATUS s;

    *out_count = 0;
    g_boot_info_ptr->gop_handle_count = 0;

    s = gBS->LocateHandleBuffer(ByProtocol, &gop_guid, (VOID *)0, &count, &handles);
    if (EFI_ERROR(s) || !handles || count == 0) {
        if (handles) gBS->FreePool(handles);
        return EFI_NOT_FOUND;
    }

    /* ConsoleOut device path + DevicePathUtilities for the authoritative primary
     * key. Both optional -- absence drops to the GUID / largest-resolution
     * passes. */
    if (gST->ConsoleOutHandle &&
        !EFI_ERROR(gBS->HandleProtocol(gST->ConsoleOutHandle, &dp_guid,
                                       (VOID **)&conout_dp)) && conout_dp) {
        (void)gBS->LocateProtocol(&dpu_guid, (VOID *)0, (VOID **)&dpu);
        if (dpu && dpu->GetDevicePathSize) {
            UINTN sz = dpu->GetDevicePathSize(conout_dp);
            if (sz >= 4u && sz <= 65536u) conout_sz = sz;
        }
    }

    /* Select the primary across ALL handles BEFORE truncating to the exported
     * BOOT_GOP_HANDLE_MAX slots (a ConsoleOut GOP past the first four must still
     * win). Priority, highest first:
     *   1. ConsoleOut device-path match (firmware's active console),
     *   2. EFI_CONSOLE_OUT_DEVICE_GUID marker AND a valid framebuffer,
     *   3. valid framebuffer, largest area,
     *   4. any GOP, largest area (last resort).
     * Validity-awareness keeps a disconnected high-res GOP from beating a usable
     * lower-res display. */
    conout_i = count; guidvalid_i = count; valid_i = count; any_i = count;
    for (i = 0; i < count; i++) {
        EFI_GRAPHICS_OUTPUT_PROTOCOL *g = (EFI_GRAPHICS_OUTPUT_PROTOCOL *)0;
        struct boot_gop_handle rec;
        UINT64 area;
        VOID *dummy = (VOID *)0;
        if (EFI_ERROR(gBS->HandleProtocol(handles[i], &gop_guid, (VOID **)&g)) || !g)
            continue;
        gop_record_handle(g, &rec);
        area = (UINT64)rec.width * (UINT64)rec.height;
        if (any_i == count || area > any_area) { any_area = area; any_i = i; }
        if (rec.fb_valid && (valid_i == count || area > valid_area)) {
            valid_area = area; valid_i = i;
        }
        if (rec.fb_valid &&
            !EFI_ERROR(gBS->HandleProtocol(handles[i], &con_guid, &dummy)) &&
            (guidvalid_i == count || area > guidvalid_area)) {
            guidvalid_area = area; guidvalid_i = i;
        }
        if (conout_i == count && conout_sz >= 4u &&
            gop_handle_is_conout(handles[i], conout_dp, conout_sz, dpu)) {
            conout_i = i;
        }
    }
    if (any_i == count) {            /* no handle exposed a usable GOP protocol */
        gBS->FreePool(handles);
        return EFI_NOT_FOUND;
    }
    primary_i = (conout_i != count) ? conout_i
              : (guidvalid_i != count) ? guidvalid_i
              : (valid_i != count) ? valid_i
              : any_i;

    /* Export the primary at slot 0, then fill the remaining slots with the other
     * GOP handles up to BOOT_GOP_HANDLE_MAX. is_primary is set by init_gop on the
     * slot that actually publishes a framebuffer. */
    {
        EFI_GRAPHICS_OUTPUT_PROTOCOL *pg = (EFI_GRAPHICS_OUTPUT_PROTOCOL *)0;
        if (!EFI_ERROR(gBS->HandleProtocol(handles[primary_i], &gop_guid,
                                           (VOID **)&pg)) && pg) {
            gop_record_handle(pg, &g_boot_info_ptr->gop_handles[0]);
            out_gops[0] = pg;
            n = 1;
        }
    }
    if (n == 0) {                    /* primary handle lost its GOP (racy) */
        gBS->FreePool(handles);
        return EFI_NOT_FOUND;
    }
    /* Fill the remaining export/retry slots valid-candidates-first so a usable
     * secondary is never truncated by an unusable handle earlier in the firmware
     * list (Codex re-adversarial R1). pass 0 takes fb_valid handles, pass 1 takes
     * the rest, both capped at BOOT_GOP_HANDLE_MAX. */
    {
        UINT32 pass;
        for (pass = 0; pass < 2u && n < BOOT_GOP_HANDLE_MAX; pass++) {
            for (i = 0; i < count && n < BOOT_GOP_HANDLE_MAX; i++) {
                EFI_GRAPHICS_OUTPUT_PROTOCOL *g = (EFI_GRAPHICS_OUTPUT_PROTOCOL *)0;
                struct boot_gop_handle rec;
                if (i == primary_i)
                    continue;
                if (EFI_ERROR(gBS->HandleProtocol(handles[i], &gop_guid, (VOID **)&g)) || !g)
                    continue;
                gop_record_handle(g, &rec);
                if ((pass == 0u) != (rec.fb_valid != 0u))
                    continue;     /* pass 0 = valid only; pass 1 = the rest */
                g_boot_info_ptr->gop_handles[n] = rec;
                out_gops[n] = g;
                n++;
            }
        }
    }

    g_boot_info_ptr->gop_handle_count = n;
    *out_count = n;
    gBS->FreePool(handles);
    return EFI_SUCCESS;
}

/* Negotiate + publish the framebuffer for ONE candidate GOP into boot_info.fb +
 * the gFb* globals. Returns EFI_SUCCESS when a usable framebuffer was published
 * (fb_available set to 1), or EFI_UNSUPPORTED when this candidate is unusable
 * (Mode/Info NULL, no framebuffer, unsupported format, bad dimensions) so the
 * caller can try the next candidate before declaring headless. Failure paths do
 * NOT commit headless state -- the caller owns the final headless decision. */
static EFI_STATUS gop_publish_primary(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop)
{
    UINT32 i;

    /* Mode NULL guard: coax firmware with SetMode(0); still NULL = unusable. */
    if (!gop || !gop->Mode) {
        serial_early_print("[BOOT] GOP: Mode NULL post-locate -- "
                           "attempting SetMode(0)\n");
        if (gop) gop->SetMode(gop, 0);
        if (!gop || !gop->Mode) {
            serial_early_print("[BOOT] GOP: Mode still NULL -- candidate unusable\n");
            return EFI_UNSUPPORTED;
        }
    }

    /* Enumerate all available modes into boot_info. */
    g_boot_info_ptr->gop_mode_count = 0;
    g_boot_info_ptr->gop_mode_selected = 0;
    {
        UINT32 query_errors = 0;
        for (i = 0; i < gop->Mode->MaxMode && i < BOOT_GOP_MODE_MAX; i++) {
            UINTN info_size = 0;
            EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = (EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *)0;
            if (EFI_ERROR(gop->QueryMode(gop, i, &info_size, &info))) {
                query_errors++;
                if (query_errors >= 100) {
                    serial_early_print("[BOOT] GOP: mode enum aborted after 100 errors\n");
                    break;
                }
                continue;
            }
            query_errors = 0;

            if (!info || info_size < sizeof(*info)) {
                if (info) gBS->FreePool(info);
                continue;
            }

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
            gBS->FreePool(info);
        }
    }

    /* Negotiate best mode via boot.conf override or highest-res auto. */
    gop_negotiate_mode(gop);

    /* Safety: if firmware framebuffer is still unusable, try mode 0. */
    if (gop->Mode->FrameBufferBase == 0) {
        serial_early_print("[BOOT] GOP: FrameBufferBase=0 after negotiate -- trying mode 0\n");
        EFI_STATUS mode0_s = gop->SetMode(gop, 0);
        if (EFI_ERROR(mode0_s))
            serial_early_print("[BOOT] GOP: SetMode(0) failed in recovery\n");
    }
    if (gop->Mode->FrameBufferBase == 0) {
        serial_early_print("[BOOT] GOP: no usable framebuffer -- candidate unusable\n");
        return EFI_UNSUPPORTED;
    }
    if (!gop->Mode->Info) {
        serial_early_print("[BOOT] GOP: Mode->Info NULL after negotiate -- candidate unusable\n");
        return EFI_UNSUPPORTED;
    }
    if (gop->Mode->Info->PixelFormat != PixelRedGreenBlueReserved &&
        gop->Mode->Info->PixelFormat != PixelBlueGreenRedReserved) {
        serial_early_print("[BOOT] GOP: unsupported pixel format -- candidate unusable\n");
        return EFI_UNSUPPORTED;
    }

    gFramebuffer = (UINT32 *)(UINTN)gop->Mode->FrameBufferBase;
    gFbWidth  = gop->Mode->Info->HorizontalResolution;
    gFbHeight = gop->Mode->Info->VerticalResolution;
    gFbPitch  = gop->Mode->Info->PixelsPerScanLine;
    gFbPixelFormat = gop_pixel_format_code(gop->Mode->Info->PixelFormat);

    {
        UINTN fb_size = gop->Mode->FrameBufferSize;
        /* Hostile-firmware overflow guard before any height*pitch*4 mul. */
        if (gFbPitch < gFbWidth || gFbHeight == 0 || gFbPitch == 0 ||
            gFbHeight > (0xFFFFFFFFu / 4) ||
            gFbPitch  > (0xFFFFFFFFu / 4) ||
            (UINTN)gFbHeight > ((UINTN)~(UINTN)0 / (UINTN)gFbPitch) ||
            ((UINTN)gFbHeight * (UINTN)gFbPitch) > ((UINTN)~(UINTN)0 / 4)) {
            serial_early_print("[BOOT] GOP: dimensions out of range -- candidate unusable\n");
            gFramebuffer = (UINT32 *)0;
            gFbWidth = 0; gFbHeight = 0; gFbPitch = 0;
            gFbPixelFormat = 2;
            return EFI_UNSUPPORTED;
        }
        UINTN required_bytes = (UINTN)gFbHeight * (UINTN)gFbPitch * 4;
        /* Require FrameBufferSize to cover the surface. fb_size==0 (firmware did
         * not report a size) is unusable: the bootloader clears
         * gFbHeight*gFbPitch pixels and the kernel maps the same extent, so an
         * unbounded geometry would overwrite past actual VRAM. UEFI 2.10
         * requires FrameBufferSize to be valid. */
        if (required_bytes == 0 || fb_size < required_bytes) {
            serial_early_print("[BOOT] GOP: framebuffer size mismatch -- candidate unusable\n");
            gFramebuffer = (UINT32 *)0;
            gFbWidth = 0; gFbHeight = 0; gFbPitch = 0;
            gFbPixelFormat = 2;
            return EFI_UNSUPPORTED;
        }
        /* VRAM clear is owned by the caller (efi_main, after init_gop returns). */
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
    /* gop_mode_selected: ordinal into gop_modes[] (sentinel = gop_mode_count when
     * the active raw mode was not enumerated). Consumers MUST check
     * selected < count before indexing. */
    {
        UINT32 active_w   = gFbWidth;
        UINT32 active_h   = gFbHeight;
        UINT32 active_pps = gFbPitch;
        UINT32 ord = g_boot_info_ptr->gop_mode_count;
        UINT32 k;
        for (k = 0; k < g_boot_info_ptr->gop_mode_count; k++) {
            if (g_boot_info_ptr->gop_modes[k].width  == active_w &&
                g_boot_info_ptr->gop_modes[k].height == active_h &&
                g_boot_info_ptr->gop_modes[k].pixels_per_scanline == active_pps) {
                ord = k;
                break;
            }
        }
        g_boot_info_ptr->gop_mode_selected = ord;
    }
    g_boot_info_ptr->fb_available = 1;
    return EFI_SUCCESS;
}

static EFI_STATUS init_gop(void)
{
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gops[BOOT_GOP_HANDLE_MAX];
    UINT32 count = 0, order[BOOT_GOP_HANDLE_MAX], om = 0, t, j, c;
    EFI_STATUS status;

    /* Enumerate every GOP handle (multi-GPU), record per-handle geometry into
     * boot_info.gop_handles[], and pick the primary (ConsoleOut-attached display,
     * validity-aware, else largest resolution). Headless when no GOP handle
     * exists (S5). */
    status = gop_enumerate_and_select(gops, &count);
    if (EFI_ERROR(status) || count == 0) {
        serial_early_print("[BOOT] GOP: none found, headless boot\n");
        g_boot_info_ptr->fb.addr = 0;
        g_boot_info_ptr->fb.width = 0;
        g_boot_info_ptr->fb.height = 0;
        g_boot_info_ptr->fb_available = 0;
        return EFI_SUCCESS;  /* Continue boot without display */
    }

    {
        char nbuf[2];
        nbuf[0] = (char)('0' + (count <= 9u ? count : 9u));
        nbuf[1] = 0;
        serial_early_print("[BOOT] GOP: ");
        serial_early_print(nbuf);
        serial_early_print(count == 1u ? " handle found\n" : " handles found\n");
    }

    /* Try the selected primary (slot 0), then the remaining valid candidates,
     * then any remaining candidate, until one publishes a framebuffer -- never go
     * headless while a usable GOP is still untried. */
    order[om++] = 0;
    for (j = 1; j < count; j++)
        if (g_boot_info_ptr->gop_handles[j].fb_valid) order[om++] = j;
    for (j = 1; j < count; j++)
        if (!g_boot_info_ptr->gop_handles[j].fb_valid) order[om++] = j;

    for (t = 0; t < om; t++) {
        j = order[t];
        if (gop_publish_primary(gops[j]) == EFI_SUCCESS) {
            for (c = 0; c < count; c++)
                g_boot_info_ptr->gop_handles[c].is_primary = (c == j) ? 1u : 0u;
            /* Refresh the winning head from the published framebuffer (post
             * negotiate). Other heads keep their gop_record_handle geometry. */
            {
                struct boot_gop_handle *ph = &g_boot_info_ptr->gop_handles[j];
                ph->fb_addr      = g_boot_info_ptr->fb.addr;
                ph->fb_size      = (UINT64)gops[j]->Mode->FrameBufferSize;
                ph->width        = g_boot_info_ptr->fb.width;
                ph->height       = g_boot_info_ptr->fb.height;
                ph->pitch        = g_boot_info_ptr->fb.pitch;
                ph->pixel_format = g_boot_info_ptr->fb.pixel_format;
                ph->fb_valid     = 1;
            }
            return EFI_SUCCESS;
        }
        /* This candidate proved unusable after negotiate -- invalidate its
         * advertised framebuffer so the kernel never maps a head we just failed
         * to bring up (Codex re-adversarial R2). Geometry stays for diagnostics;
         * fb_addr/fb_valid are cleared. */
        g_boot_info_ptr->gop_handles[j].fb_addr = 0;
        g_boot_info_ptr->gop_handles[j].fb_size = 0;
        g_boot_info_ptr->gop_handles[j].fb_valid = 0;
        g_boot_info_ptr->gop_handles[j].is_primary = 0;
    }

    /* Every candidate failed to publish -- headless. Clear ALL per-head state so
     * a headless boot is unambiguous (Codex re-adversarial R2): no stale fb_valid
     * entry survives and gop_handle_count drops to 0. */
    serial_early_print("[BOOT] GOP: no candidate published a framebuffer -- headless boot\n");
    gFramebuffer = (UINT32 *)0;
    gFbWidth = 0; gFbHeight = 0; gFbPitch = 0;
    gFbPixelFormat = 2;
    g_boot_info_ptr->fb.addr = 0;
    g_boot_info_ptr->fb.width = 0;
    g_boot_info_ptr->fb.height = 0;
    g_boot_info_ptr->fb_available = 0;
    g_boot_info_ptr->hidpi = 0;
    for (c = 0; c < BOOT_GOP_HANDLE_MAX; c++) {
        g_boot_info_ptr->gop_handles[c].fb_addr = 0;
        g_boot_info_ptr->gop_handles[c].fb_size = 0;
        g_boot_info_ptr->gop_handles[c].width = 0;
        g_boot_info_ptr->gop_handles[c].height = 0;
        g_boot_info_ptr->gop_handles[c].pitch = 0;
        g_boot_info_ptr->gop_handles[c].pixel_format = 2;
        g_boot_info_ptr->gop_handles[c].is_primary = 0;
        g_boot_info_ptr->gop_handles[c].fb_valid = 0;
        g_boot_info_ptr->gop_handles[c].pad = 0;
    }
    g_boot_info_ptr->gop_handle_count = 0;
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

/* ============================================================================
 * section 5: typed payload staging for module= / initrd= / recovery_image=
 *
 * parse_conf_kv() routes each occurrence of these keys through
 * stage_payload() into a fixed staging array. After boot.conf parsing
 * completes, load_staged_payloads() opens each staged file, allocates
 * EfiLoaderData pages, reads the file, and populates the matching
 * payload_descriptors[] slot in g_boot_info_ptr. Runs while Boot
 * Services are still alive; consumers (kernel-side boot_payload_find)
 * read the descriptors after ExitBootServices.
 *
 * The staging layer exists because a single boot.conf line cannot
 * directly own file I/O (parse_conf_kv is called once per key=value
 * pair and cannot partially fail without losing the rest of the file's
 * configuration). Separating parse from load keeps error handling
 * concentrated and lets the PATHS list survive into the load phase
 * even if the parser encounters later unrelated errors.
 * ============================================================================ */

/* Max length of a single ESP-relative payload path. Covers realistic
 * trees like \\EFI\\ImpossibleOS\\modules\\subdir\\name.kmod with room
 * for a filename that is a few times the FAT short-name limit. If a
 * future layout needs longer paths, grow this (producer_id bytes-in-path
 * are hashed for sanity logging but never truncated). */
#define BOOT_PAYLOAD_PATH_MAX 192

/* Per-payload size cap. Typical module/initrd/recovery images in real
 * deployments are well under this; anything larger almost certainly
 * indicates FAT metadata corruption, a pointer-turned-size bug, or a
 * deliberately-malicious image. Cap before AllocatePages so we fail
 * cleanly with a specific message instead of attempting a wild
 * allocation that would either OOM or wrap the page-count math. Sized
 * to match the comparable boot.conf 1 MiB sanity cap philosophy: big
 * enough for any legitimate payload but much smaller than host RAM. */
#define BOOT_PAYLOAD_FILE_MAX (256ull * 1024ull * 1024ull)  /* 256 MiB */

/* The kernel refuses to PIN a MODULE, INITRD or RECOVERY_IMAGE payload past
 * its own declared maximum, and that maximum is written to match the cap
 * above. Include the shared contract and assert the two agree HERE, on the
 * producer side, because a divergence is otherwise invisible: this loader
 * would allocate and publish a payload as VALID|RESERVED that the kernel
 * then silently declines to pin, and the descriptor would look retained.
 * The header is designed for exactly this (plain types, literals, no UEFI
 * or libc dependencies), so the assertion belongs here rather than only on
 * the kernel side. */
#include "../../../include/boot/boot_payload_limits.h"

struct staged_payload {
    UINT32 type;                          /* BOOT_PAYLOAD_MODULE / _INITRD / _RECOVERY_IMAGE */
    char   path[BOOT_PAYLOAD_PATH_MAX];   /* ASCII; converted to CHAR16 on load */
};

/* How many descriptor slots boot.conf staging may occupy. The remainder is
 * reserved for the IMPLICIT publishers -- payloads the loader synthesizes
 * itself, which have no boot.conf entry and so nobody notices when one goes
 * missing. The reservation is COMPUTED from the publisher list in
 * include/boot/boot_implicit_payload.h rather than written as a constant,
 * because the constant is what went wrong: it read `- 1` against a comment
 * naming three implicit payloads, so at the boundary the publishers competed
 * for one slot and the loser was whichever ran later.
 *
 * Adding a publisher is now one entry in that list. The reservation grows with
 * it and the assertions below re-check the arithmetic; no call site changes. */
#define BOOT_PAYLOAD_STAGE_MAX BOOT_PAYLOAD_STAGE_MAX_FOR(BOOT_PAYLOAD_MAX)

/* The list spells its payload types as literals, because the kernel enum and
 * this mirror spell the same constants differently and the header is included
 * by both. These assertions are how a divergence becomes a build failure on the
 * side that drifted instead of a silent mismatch. */
_Static_assert(BOOT_PAYLOAD_RANDOM_SEED == 7u,
               "implicit-payload list literal 7 must stay BOOT_PAYLOAD_RANDOM_SEED");
_Static_assert(BOOT_PAYLOAD_HEADLESS_AUTHZ == 10u,
               "implicit-payload list literal 10 must stay BOOT_PAYLOAD_HEADLESS_AUTHZ");
_Static_assert(BOOT_PAYLOAD_STAGE_MAX > 0,
               "implicit reservation must leave room for at least one boot.conf payload");
_Static_assert(BOOT_PAYLOAD_STAGE_MAX + BOOT_IMPLICIT_PAYLOAD_COUNT == BOOT_PAYLOAD_MAX,
               "staging bound plus implicit reservation must exactly fill the descriptor table");
static struct staged_payload g_staged_payloads[BOOT_PAYLOAD_STAGE_MAX];
static UINTN g_staged_payload_count = 0;
static UINT32 g_staged_payload_overflow = 0;

/* Which implicit publishers have already taken their reserved slot. One bit
 * per entry in BOOT_IMPLICIT_PAYLOAD_LIST.
 *
 * The `= 0` here is DOCUMENTATION, NOT INITIALIZATION. This loader does not
 * zero `.bss` and firmware poisons it with 0xAF (see the note near the top of
 * this file), so at entry this word reads 0xAFAFAFAF -- both publisher bits
 * already set, both publishers refused as duplicates, and the section that
 * exists to stop an entropy seed being dropped would drop it on every boot.
 * The staged-payload counters below carry the same hazard and are reset the
 * same way; `reset_implicit_payload_claims()` is called from
 * `parse_boot_conf()`, which runs before `load_staged_payloads()` and before
 * either publisher. */
static UINT32 g_implicit_payload_claimed = 0;

static void reset_implicit_payload_claims(void)
{
    g_implicit_payload_claimed = 0;
}

/* Publish one implicit payload, or refuse LOUDLY and change nothing.
 *
 * `desc` must be COMPLETE before this is called: the copy plus the two counter
 * updates are the commit, and every fallible step (allocation, file read,
 * length and checksum work) belongs before it. A publisher that claimed a slot
 * and then failed its I/O would leave an empty descriptor inside the packed
 * prefix, which the kernel validator rejects outright -- an unbootable machine,
 * strictly worse than the dropped payload the reservation exists to prevent.
 *
 * Returns TRUE when the descriptor was published. On FALSE the table is
 * untouched and the caller should release whatever it allocated.
 *
 * The refusal line names the payload AND what the machine loses. The lines this
 * replaced ("payload table full -- seed dropped") named neither the consequence
 * nor, in the authorization's case, which of the two publishers had won, so an
 * operator reading a boot log could not tell a degraded machine from a healthy
 * one. */
static BOOLEAN publish_implicit_payload(const struct boot_payload_desc *desc)
{
    enum boot_implicit_claim_result r;

    if (g_boot_info_ptr == (struct boot_info *)0 || desc == (const struct boot_payload_desc *)0) {
        serial_early_print("[WARN] implicit payload: no boot_info or no descriptor -- not published\n");
        return 0;
    }

    r = boot_implicit_payload_claim((unsigned int)desc->type,
                                    (unsigned int)g_boot_info_ptr->payload_count,
                                    (unsigned int)BOOT_PAYLOAD_MAX,
                                    (unsigned int *)&g_implicit_payload_claimed);
    if (r != BOOT_IMPLICIT_CLAIM_OK) {
        serial_early_print("[WARN] implicit payload REFUSED: ");
        serial_early_print(boot_implicit_payload_label((unsigned int)desc->type));
        serial_early_print(" -- ");
        switch (r) {
        case BOOT_IMPLICIT_CLAIM_NOT_IMPLICIT:
            serial_early_print("type is not a declared implicit publisher");
            break;
        case BOOT_IMPLICIT_CLAIM_DUPLICATE:
            serial_early_print("this publisher already took its reserved slot");
            break;
        case BOOT_IMPLICIT_CLAIM_TABLE_FULL:
            serial_early_print("descriptor table full");
            break;
        case BOOT_IMPLICIT_CLAIM_NULL_STATE:
            serial_early_print("claim state unavailable");
            break;
        default:
            serial_early_print("unknown refusal");
            break;
        }
        serial_early_print("; machine is DEGRADED: ");
        serial_early_print(boot_implicit_payload_degradation((unsigned int)desc->type));
        serial_early_print("\n");
        return 0;
    }

    /* Commit. The slot write and both counter updates happen together, with
     * nothing between them that can fail. The macro lives in the shared header
     * so the kernel-side tests execute these exact lines rather than a fixture
     * that re-implements them. */
    BOOT_IMPLICIT_PAYLOAD_COMMIT(g_boot_info_ptr, desc);
    return 1;
}

/* Append one payload to the staging list. Type is already resolved to
 * BOOT_PAYLOAD_*. Path is ASCII (from boot.conf); empty paths are
 * rejected at the parser level before this is called. Overflow beyond
 * BOOT_PAYLOAD_STAGE_MAX is counted and surfaced via
 * payload_overflow in boot_info -- the validator's existing
 * BOOT_PAYLOAD_ERR_OVERFLOW_TRUNCATED check then refuses to boot
 * with an incomplete payload set. */
static void stage_payload(UINT32 type, const char *path)
{
    if (path == (const char *)0 || path[0] == '\0') {
        /* Defense in depth: parse_conf_kv's payload-key branches
         * already boot_fatal on empty value, so this path is
         * unreachable today. If a future caller forgets that
         * contract, fail hard here too rather than silently losing a
         * payload. */
        boot_fatal(BOOT_ERR_CONF_INVALID,
                   "stage_payload: empty path",
                   "Caller passed a NULL or empty path; see parse_conf_kv.");
    }
    if (g_staged_payload_count >= BOOT_PAYLOAD_STAGE_MAX) {
        g_staged_payload_overflow++;
        serial_early_print("[WARN] boot.conf: payload staging full (max=");
        serial_early_print_uint((UINT32)BOOT_PAYLOAD_STAGE_MAX);
        serial_early_print("); dropping '");
        serial_early_print(path);
        serial_early_print("'\n");
        return;
    }
    struct staged_payload *slot = &g_staged_payloads[g_staged_payload_count];
    slot->type = type;
    UINTN i;
    for (i = 0; i < BOOT_PAYLOAD_PATH_MAX - 1 && path[i] != '\0'; i++)
        slot->path[i] = path[i];
    slot->path[i] = '\0';
    /* Path overflow is a producer bug: the intended file cannot be
     * located by this name. Refuse to continue rather than silently
     * truncating to the wrong ESP file. */
    if (path[i] != '\0') {
        serial_early_print("[FATAL] boot.conf: payload path exceeds ");
        serial_early_print_uint((UINT32)(BOOT_PAYLOAD_PATH_MAX - 1));
        serial_early_print(" chars: '");
        serial_early_print(path);
        serial_early_print("'\n");
        boot_fatal(BOOT_ERR_CONF_INVALID,
                   "boot.conf payload path too long",
                   "Shorten the path or raise BOOT_PAYLOAD_PATH_MAX.");
    }
    g_staged_payload_count++;
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
    cfg->utest_isolation = 1;   /* per-test isolation ON (opt-out only via boot.conf) */
    cfg->firmware_rng   = 1;    /* collect EFI_RNG_PROTOCOL entropy by default */
    cfg->seed_file      = 1;    /* kernel seed-file carryover lifecycle on by default */
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
        else if (ascii_streq(val, "exec"))     cfg->test_suite = 9;
        else if (ascii_streq(val, "x86"))      cfg->test_suite = 10;
        else if (ascii_streq(val, "desktop"))  cfg->test_suite = 11;
        else if (ascii_streq(val, "ex"))       cfg->test_suite = 12;
        else if (ascii_streq(val, "nls"))      cfg->test_suite = 13;
        else if (ascii_streq(val, "knf"))      cfg->test_suite = 14;
        else if (ascii_streq(val, "except"))   cfg->test_suite = 15;
        else if (ascii_streq(val, "quota"))    cfg->test_suite = 16;
        else                                   cfg->test_suite = 0xFF;
    }
    else if (ascii_streq(key, "test_quiet")) {
        cfg->test_quiet = (UINT8)ascii_atoi(val);
    }
    else if (ascii_streq(key, "firmware_rng")) {
        /* Escape hatch for firmware whose RNG hangs inside GetRNG. */
        if      (ascii_streq(val, "off")) cfg->firmware_rng = 0;
        else if (ascii_streq(val, "on"))  cfg->firmware_rng = 1;
        else                              cfg->firmware_rng = (UINT8)ascii_atoi(val);
    }
    else if (ascii_streq(key, "seed_file")) {
        /* Escape hatch: skip the kernel's random-seed carryover file
         * read/rotate (e.g. damaged or read-only BlackBox media). */
        if      (ascii_streq(val, "off")) cfg->seed_file = 0;
        else if (ascii_streq(val, "on"))  cfg->seed_file = 1;
        else                              cfg->seed_file = (UINT8)ascii_atoi(val);
    }
    else if (ascii_streq(key, "tpm_enroll")) {
        /* Measured-boot baseline enrollment opt-in; the kernel honors it only
         * in recovery mode (boot_mode=recovery) so a normal boot never silently
         * becomes the golden baseline. */
        if      (ascii_streq(val, "off")) cfg->tpm_enroll = 0;
        else if (ascii_streq(val, "on"))  cfg->tpm_enroll = 1;
        else                              cfg->tpm_enroll = (UINT8)ascii_atoi(val);
    }
    else if (ascii_streq(key, "anti_rollback_raise")) {
        /* Anti-rollback opt-in: when set, a boot that reaches the
         * compositor's first stable frame advances the
         * IPOSRequiredSecVersion NVRAM floor to the shipped version.
         * Default 0 (memset by boot_config_defaults) is the safe
         * state -- the floor never moves, so every older signed image
         * stays bootable. Consumed by the kernel at
         * boot_rollback_raise_if_steady() in
         * src/kernel/main/boot_rollback.c; without this branch the
         * field stayed 0 forever and the steady-gated raise was
         * unreachable in production. */
        /* STRICT, unlike the sibling knobs above. Those are benign
         * toggles where a typo costs a feature; this one advances an
         * IRREVERSIBLE security floor, and `ascii_atoi` + a UINT8
         * truncation would let `2`, `1garbage`, or an overflowing
         * decimal that wraps to a nonzero byte all read as opt-IN,
         * because the kernel treats any nonzero value as enabled.
         * A corrupted or mistyped boot.conf must never be able to
         * strand older signed images. Anything unrecognized falls back
         * to the safe 0 and says so on serial. */
        if      (ascii_streq(val, "off") || ascii_streq(val, "0"))
            cfg->anti_rollback_raise = 0;
        else if (ascii_streq(val, "on")  || ascii_streq(val, "1"))
            cfg->anti_rollback_raise = 1;
        else {
            cfg->anti_rollback_raise = 0;
            serial_early_print("[WARN] boot.conf: anti_rollback_raise=");
            serial_early_print(val);
            serial_early_print(" is not 0/1/on/off -- treating as OFF\r\n");
        }
    }
    else if (ascii_streq(key, "compositor")) {
        /* Desktop UI test framework, headless compositor section. */
        if      (ascii_streq(val, "headless")) cfg->compositor = 1;
        else if (ascii_streq(val, "normal"))   cfg->compositor = 0;
        else                                   cfg->compositor = (UINT8)ascii_atoi(val);
    }
    else if (ascii_streq(key, "test_monitors")) {
        /* Desktop UI multi-monitor matrix section. Accepts either a
         * plain integer count (`test_monitors=2` -> count=2) or the
         * full geometry list form (`1920x1080@96,1920x1080@144,...`)
         * which is comma-counted. The geometry form is forward-compat
         * for the virtio-gpu multi-output driver that will consume the
         * per-monitor WxH@DPI metadata; today both forms collapse to a
         * count. Counts > 3 clamp to 0 = "use hardware default".
         * Codex [H] review: the prior implementation comma-counted
         * even pure-integer input, so `test_monitors=2` was parsed as
         * count=1 and silently broke the documented contract. */
        UINT8 count = 0;
        int all_digits = (*val != '\0');
        const char *p = val;
        while (*p) {
            if (*p < '0' || *p > '9') { all_digits = 0; break; }
            p++;
        }
        if (all_digits) {
            UINTN v = ascii_atoi(val);
            count = (v > 3) ? 0 : (UINT8)v;
        } else {
            count = (*val != '\0') ? 1 : 0;
            const char *q = val;
            while (*q) {
                if (*q == ',') count++;
                q++;
            }
            if (count > 3) count = 0;
        }
        cfg->test_monitors_count = count;
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
    else if (ascii_streq(key, "tap")) {
        cfg->tap = (UINT8)ascii_atoi(val);
    }
    else if (ascii_streq(key, "utest_timeout_ms")) {
        UINTN ms = (UINTN)ascii_atoi(val);
        if (ms > 0xFFFF) ms = 0xFFFF;  /* clamp to uint16_t */
        cfg->utest_timeout_ms = (UINT16)ms;
    }
    else if (ascii_streq(key, "utest_filter")) {
        UINTN i;
        for (i = 0; i < sizeof(cfg->utest_filter) - 1 && val[i]; i++)
            cfg->utest_filter[i] = val[i];
        cfg->utest_filter[i] = '\0';
    }
    else if (ascii_streq(key, "utest_isolation")) {
        cfg->utest_isolation = (UINT8)ascii_atoi(val);
    }
    else if (ascii_streq(key, "xml")) {
        cfg->xml = (UINT8)ascii_atoi(val);
    }
    else if (ascii_streq(key, "json")) {
        cfg->json = (UINT8)ascii_atoi(val);
    }
    else if (ascii_streq(key, "stress_iters")) {
        UINTN n = (UINTN)ascii_atoi(val);
        if (n > 0xFFFF) n = 0xFFFF;
        cfg->stress_iters = (UINT16)n;
    }
    else if (ascii_streq(key, "test_kernel_skip")) {
        cfg->test_kernel_skip = (UINT8)ascii_atoi(val);
    }
    else if (ascii_streq(key, "test_usermode_skip")) {
        cfg->test_usermode_skip = (UINT8)ascii_atoi(val);
    }
    else if (ascii_streq(key, "firmware_quirk_disable")) {
        /* Tokenizer + qmap[] live in the shared static-inline at
         * include/kernel/firmware_quirks_parse.inc -- both kernel and
         * bootloader call it so tokenization rules cannot drift. */
        cfg->firmware_quirk_disable =
            (UINT8)firmware_quirks_parse_disable_inline(val);
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
    else if (ascii_streq(key, "kernel")) {
        /* Recognised but ignored -- kernel path is a hardcoded 3-path
           fallback search (\boot\kernel.exe, \kernel.exe,
           \EFI\ImpossibleOS\kernel.exe).  Accept the key so boot.conf
           can document the default without triggering a warning. */
    }
    else if (ascii_streq(key, "module")) {
        /* section 5 typed payload: early kernel module file. Value is
         * an ESP-relative path like '\\EFI\\ImpossibleOS\\mod\\net.kmod'.
         * Multiple module= lines are allowed; each stages one MODULE
         * descriptor. Empty value is a hard error (test-checkpoint
         * contract: "missing or malformed entries fail"). */
        if (val[0] == '\0') {
            boot_fatal(BOOT_ERR_CONF_INVALID,
                       "boot.conf: module= with empty path",
                       "Remove the line or give it a valid ESP-relative path.");
        }
        stage_payload(BOOT_PAYLOAD_MODULE, val);
    }
    else if (ascii_streq(key, "initrd")) {
        /* section 5 typed payload: initrd/initramfs. One or more
         * initrd= lines stage INITRD descriptors in order. Empty
         * value is a hard error. */
        if (val[0] == '\0') {
            boot_fatal(BOOT_ERR_CONF_INVALID,
                       "boot.conf: initrd= with empty path",
                       "Remove the line or give it a valid ESP-relative path.");
        }
        stage_payload(BOOT_PAYLOAD_INITRD, val);
    }
    else if (ascii_streq(key, "recovery_image")) {
        /* section 5 typed payload: recovery env image. Typically one
         * recovery_image= line, but the staging layer accepts multiple
         * (e.g. recovery + installer bundle). Empty value is a hard
         * error. */
        if (val[0] == '\0') {
            boot_fatal(BOOT_ERR_CONF_INVALID,
                       "boot.conf: recovery_image= with empty path",
                       "Remove the line or give it a valid ESP-relative path.");
        }
        stage_payload(BOOT_PAYLOAD_RECOVERY_IMAGE, val);
    }
    else {
        /* Unknown key -- warn but don't fail (forward compatibility) */
        serial_early_print("[WARN] boot.conf: unknown key '");
        serial_early_print(key);
        serial_early_print("' (ignored)\n");
        return;
    }

    /* --- Range validation ---
     * Out-of-range boolean values clamp to the SAFE DEFAULT (0), not
     * to the max (1) -- a malformed `test=255` or `debug=2` must NOT
     * silently enable test/debug mode, which gates fault-injection
     * syscalls and bypasses normal boot-perf collection.  Defaults
     * are populated by boot_config_defaults() (debug=0, verbose=0,
     * test=0, splash_timeout=3) before this parser runs. */
    if (ascii_streq(key, "debug") && cfg->debug > 1) {
        serial_early_print("[WARN] boot.conf: debug out of range, using 0\n");
        cfg->debug = 0;
    }
    if (ascii_streq(key, "verbose") && cfg->verbose > 1) {
        serial_early_print("[WARN] boot.conf: verbose out of range, using 0\n");
        cfg->verbose = 0;
    }
    /* splash_timeout: 0-60 seconds */
    if (ascii_streq(key, "splash_timeout") && cfg->splash_timeout > 60) {
        serial_early_print("[WARN] boot.conf: splash_timeout out of range, using 3\n");
        cfg->splash_timeout = 3;
    }
    /* test: 0 or 1 */
    if (ascii_streq(key, "test") && cfg->test > 1) {
        serial_early_print("[WARN] boot.conf: test out of range, using 0\n");
        cfg->test = 0;
    }
}

/* Read and parse \EFI\ImpossibleOS\boot.conf
 *
 * UKI mode: when
 * detect_uki_sections() captured a `.cmdline` PE section, the
 * embedded buffer is the firmware-Secure-Boot-verified config.
 * Reading boot.conf from the ESP in UKI mode would defeat the
 * whole-chain signature claim because the disk file is unsigned
 * relative to the UKI artifact. The short-circuit below allocates a
 * writable copy of the embedded section, parses it via the same
 * loop the disk path uses, and returns -- never opening the
 * filesystem. */
static void parse_boot_conf(void)
{
    boot_set_section(BOOT_SECTION_BL_CONF);
    EFI_GUID fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
    EFI_FILE_PROTOCOL *root_dir, *conf_file;
    EFI_STATUS status;
    struct boot_config *cfg = &g_boot_info_ptr->config;
    /* Lifted to function scope so the UKI fast-path can fill the
     * same buffer the disk path fills, and both converge at the
     * `parse_loop` label below. */
    char *buf = (char *)0;
    UINTN buf_size = 0;

    /* Always start with defaults */
    boot_config_defaults(cfg);

    /* Reset section 5 payload staging explicitly. EDK2 DEBUG builds
     * fill unused pool / BSS memory with 0xAF, and in practice these
     * statics come up with that poison pattern instead of zero --
     * enough to make stage_payload's count >= STAGE_MAX check fail
     * early (0xAFAFAFAF >= BOOT_PAYLOAD_STAGE_MAX) and leak payload_overflow=0xAFAFAFAF
     * into the validator. Initializing at parse entry guarantees a
     * clean slate on every boot regardless of PE loader behavior. */
    g_staged_payload_count = 0;
    g_staged_payload_overflow = 0;
    efi_memset(g_staged_payloads, 0, sizeof(g_staged_payloads));

    /* Same hazard, same remedy: the implicit-publisher claim mask is a static
     * in the same non-zeroed `.bss`, and 0xAFAFAFAF has every publisher bit
     * set. Left poisoned it refuses BOTH implicit payloads as duplicates. */
    reset_implicit_payload_claims();

    serial_early_print("[BOOT] parse_boot_conf...\n");

    /* UKI mode: the disk boot.conf MUST NOT be consulted. The
     * whole-chain Secure Boot signature claim only holds if the
     * runtime config came from the firmware-verified .cmdline
     * section (or defaults). Codex re-adversarial H1 fix 2026-04-29:
     * gate the fallback on g_uki_kernel_ptr (the UKI-mode indicator)
     * rather than g_uki_cmdline_ptr -- an absent or empty .cmdline
     * section would otherwise let execution fall through to the ESP
     * filesystem, defeating the signature semantics. */
    #ifndef BOOT_CONF_SANITY_CAP
    #define BOOT_CONF_SANITY_CAP (1024u * 1024u)
    #endif
    if (g_uki_kernel_ptr) {
        if (g_uki_cmdline_ptr && g_uki_cmdline_size > 0) {
            if (g_uki_cmdline_size > BOOT_CONF_SANITY_CAP) {
                serial_early_print("[FATAL] UKI .cmdline section exceeds 1 MiB sanity cap\n");
                boot_fatal(BOOT_ERR_CONF_INVALID,
                           "UKI .cmdline exceeds 1 MiB sanity cap",
                           "Generated UKI artifact is malformed; rebuild and re-sign.");
            }
            status = gBS->AllocatePool(EfiLoaderData,
                                        g_uki_cmdline_size + 1,
                                        (VOID **)&buf);
            if (EFI_ERROR(status) || !buf) {
                serial_early_print("[BOOT] UKI .cmdline AllocatePool failed - using defaults\n");
                return;
            }
            efi_memcpy(buf, g_uki_cmdline_ptr, g_uki_cmdline_size);
            buf[g_uki_cmdline_size] = '\0';
            buf_size = g_uki_cmdline_size;
            serial_early_print("[BOOT] UKI .cmdline loaded (");
            serial_early_print_uint((UINT32)buf_size);
            serial_early_print(" bytes; firmware-Secure-Boot-verified)\n");
            /* Reject any disk-side payload override token in the
             * active cmdline. Helper is in a separate translation
             * unit so unit tests can exercise it against synthetic
             * fixtures without booting QEMU. */
            const char *reject_token = uki_find_disk_override_token(
                (const UINT8 *)buf, buf_size);
            if (reject_token) {
                serial_early_print("[FATAL] UKI mode rejects out-of-UKI ");
                serial_early_print(reject_token);
                serial_early_print(" override; disk payload is unsigned\n");
                boot_fatal(BOOT_ERR_UKI_DISK_OVERRIDE,
                           "UKI cmdline contains disk-payload override token",
                           "Rebuild the UKI without initrd= / module= / recovery_image= "
                           "tokens in resources/boot/boot.conf; load-bearing payloads "
                           "must use the .initrd / .recovery / .modules PE sections "
                           "embedded by scripts/build.sh.");
            }
            goto parse_loop;
        }
        /* UKI mode but no usable .cmdline section: keep boot_config
         * defaults from boot_config_defaults() and return without
         * touching the filesystem. The whole-chain signature still
         * holds because no unsigned disk content was consulted. */
        serial_early_print("[BOOT] UKI mode without .cmdline -- using boot_config "
                           "defaults (disk boot.conf NOT read; whole-chain signature "
                           "preserved)\n");
        return;
    }

    /* Split-path fallback only -- no UKI invocation detected. Open
     * filesystem from boot device (scoped to boot volume).  When the
     * boot DeviceHandle is absent or lacks SimpleFS, do NOT silently
     * call LocateProtocol -- it returns the first SimpleFS firmware
     * enumerates, which on multi-disk systems may not be the boot
     * volume and could let an arbitrary ESP provide boot.conf
     * (defeating the trust boundary the fallback kernel-search path
     * established).  Skip the parse instead and keep boot_config
     * defaults already populated by boot_config_defaults(); a
     * degraded boot device still boots because the kernel-search
     * fallback chain separately locates kernel.exe with explicit
     * per-volume diagnostics. */
    post_code16(POST16_BL_BOOT_FS);
    fs = (EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *)0;
    if (g_boot_device_handle) {
        status = gBS->HandleProtocol(g_boot_device_handle,
                                      &fs_guid, (VOID **)&fs);
        /* Treat EFI_SUCCESS + NULL interface as failure -- nearby
         * code (esp_check_required_files, load_kernel) already gates
         * on (status && fs); match the postcondition here so a
         * misbehaving firmware cannot crash OpenVolume below. */
        if (!EFI_ERROR(status) && fs) {
            serial_early_print("[BOOT] Using boot device filesystem\n");
        } else {
            serial_early_print("[WARN] Boot device has no SimpleFS for "
                               "boot.conf -- skipping parse, using "
                               "boot_config defaults\n");
            return;
        }
    } else {
        serial_early_print("[WARN] No boot device handle for boot.conf "
                           "-- skipping parse, using boot_config "
                           "defaults\n");
        return;
    }
    status = fs->OpenVolume(fs, &root_dir);
    if (EFI_ERROR(status)) {
        serial_early_print("[BOOT] boot.conf - cannot open volume\n");
        return;
    }
    post_code16(POST16_BL_BOOT_FS_OK);

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

    /* Read entire file into a pool-allocated buffer sized to the file
     * (2026-04-21): the previous fixed stack buffer silently truncated
     * boot.conf when in-tree comments grew past the cap, dropping the
     * patch-appended `test=1` line at end-of-file and making every
     * `test=1` boot a no-op without any visible warning. Now we query
     * the file size via GetInfo first, then AllocatePool exactly that
     * many bytes plus a terminator. The only cap is a sanity ceiling
     * that rejects a corrupt/malicious filesystem claiming a
     * multi-GiB boot.conf. See `boot-code-quality` Gate 14.
     *
     * BOOT_CONF_SANITY_CAP: 1 MiB. Any legitimate boot.conf is under
     * 100 KiB; a file past this cap means fs corruption or a test
     * disk shipping garbage, and we'd rather boot_fatal than burn
     * half of bootloader memory on it. */
    #define BOOT_CONF_SANITY_CAP (1024u * 1024u)

    UINT64 file_size = 0;
    {
        EFI_GUID file_info_guid = EFI_FILE_INFO_ID;
        UINTN info_size = 0;
        /* Size-query probe: GetInfo returns EFI_BUFFER_TOO_SMALL and
         * fills info_size with the canonical EFI_FILE_INFO size
         * (which includes the variable-length FileName tail). Callers
         * MUST allocate exactly info_size -- a smaller buffer yields
         * a truncated FileInfo, a larger one wastes pool but still
         * returns the same fields. */
        status = conf_file->GetInfo(conf_file, &file_info_guid,
                                    &info_size, (VOID *)0);
        if (status != EFI_BUFFER_TOO_SMALL || info_size == 0) {
            serial_early_print("[BOOT] boot.conf GetInfo size-probe failed - using defaults\n");
            conf_file->Close(conf_file);
            root_dir->Close(root_dir);
            return;
        }
        VOID *info_buf = (VOID *)0;
        status = gBS->AllocatePool(EfiLoaderData, info_size, &info_buf);
        if (EFI_ERROR(status) || !info_buf) {
            serial_early_print("[BOOT] boot.conf info AllocatePool failed - using defaults\n");
            conf_file->Close(conf_file);
            root_dir->Close(root_dir);
            return;
        }
        status = conf_file->GetInfo(conf_file, &file_info_guid,
                                    &info_size, info_buf);
        if (EFI_ERROR(status)) {
            serial_early_print("[BOOT] boot.conf GetInfo read failed - using defaults\n");
            gBS->FreePool(info_buf);
            conf_file->Close(conf_file);
            root_dir->Close(root_dir);
            return;
        }
        file_size = ((EFI_FILE_INFO *)info_buf)->FileSize;
        gBS->FreePool(info_buf);
    }

    if (file_size == 0) {
        serial_early_print("[BOOT] boot.conf empty - using defaults\n");
        conf_file->Close(conf_file);
        root_dir->Close(root_dir);
        return;
    }
    if (file_size > BOOT_CONF_SANITY_CAP) {
        serial_early_print("[FATAL] boot.conf implausibly large -- FileSize=");
        serial_early_print_uint((UINT32)file_size);
        serial_early_print(" bytes (cap=1 MiB); filesystem likely corrupt\n");
        conf_file->Close(conf_file);
        root_dir->Close(root_dir);
        boot_fatal(BOOT_ERR_CONF_INVALID,
                   "boot.conf exceeds 1 MiB sanity cap",
                   "Filesystem likely corrupt; boot.conf claimed >1 MiB.");
    }

    status = gBS->AllocatePool(EfiLoaderData, (UINTN)file_size + 1,
                               (VOID **)&buf);
    if (EFI_ERROR(status) || !buf) {
        serial_early_print("[BOOT] boot.conf AllocatePool failed - using defaults\n");
        conf_file->Close(conf_file);
        root_dir->Close(root_dir);
        return;
    }

    buf_size = (UINTN)file_size;
    status = conf_file->Read(conf_file, &buf_size, buf);
    conf_file->Close(conf_file);
    root_dir->Close(root_dir);

    if (EFI_ERROR(status) || buf_size == 0) {
        serial_early_print("[BOOT] boot.conf read error - using defaults\n");
        gBS->FreePool(buf);
        return;
    }

    /* Reject short reads.  Parsing a prefix when the underlying FAT
     * chain truncated would silently drop trailing keys (test=1,
     * module=, recovery_image=) -- the same silent-truncation class
     * the dynamic-buffer hardening eliminated.  Any mismatch between
     * the declared FileSize and the actual bytes returned is a
     * filesystem / media error: log declared vs actual and fall
     * through to boot_config defaults rather than parsing the
     * truncated prefix. */
    if (buf_size != (UINTN)file_size) {
        serial_early_print("[WARN] boot.conf short read: declared=");
        serial_early_print_uint((UINT32)file_size);
        serial_early_print(" actual=");
        serial_early_print_uint((UINT32)buf_size);
        serial_early_print(" bytes -- using boot_config defaults\n");
        gBS->FreePool(buf);
        return;
    }
    buf[buf_size] = '\0';

    serial_early_print("[BOOT] boot.conf loaded (");
    serial_early_print_uint((UINT32)buf_size);
    serial_early_print(" bytes)\n");

parse_loop:
    ;  /* C99: a label must be followed by a statement, not a declaration */
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

        /* UKI whole-chain Secure Boot: reject any disk-payload key
         * staged by parse_conf_kv when invoked under UKI mode. This
         * is the AUTHORITATIVE rejection gate -- it runs AFTER the
         * key has been normalized (whitespace stripped at line 2642)
         * so a UKI cmdline like `mod ule = foo` (parser normalizes
         * to `module=foo`) cannot bypass detection. The static-inline
         * helper in include/boot/uki_cmdline_check.h fired earlier
         * on literal-byte forms as defense-in-depth; this check
         * covers the parser's actual decision boundary. Codex
         * re-adversarial 2026-05-01 round-3 fix: literal-byte helper
         * missed whitespace-stripped key forms. */
        if (g_uki_kernel_ptr && ki > 0) {
            if (ascii_streq(key, "module") ||
                ascii_streq(key, "initrd") ||
                ascii_streq(key, "recovery_image")) {
                serial_early_print("[FATAL] UKI mode rejects out-of-UKI ");
                serial_early_print(key);
                serial_early_print("= override; disk payload is unsigned\n");
                boot_fatal(BOOT_ERR_UKI_DISK_OVERRIDE,
                           "UKI cmdline contains disk-payload override key",
                           "Rebuild the UKI without initrd= / module= / recovery_image= "
                           "lines in resources/boot/boot.conf; load-bearing payloads "
                           "must use the .initrd / .recovery / .modules PE sections "
                           "embedded by scripts/build.sh.");
            }
        }

        /* Pre-gate empty-value detection for the section 5 payload keys.
         * The `ki > 0 && vi > 0` gate below exists to preserve today's
         * behavior for NON-payload keys: a stray `cmdline=` later in
         * boot.conf must not clear an earlier `cmdline=foo`, and a
         * trailing `test_suite=` must not reset to 0xFF. But the
         * payload-key contract requires `module=` / `initrd=` /
         * `recovery_image=` with empty value to FAIL boot with a
         * specific diagnostic, which means we cannot silently drop
         * empty-value lines for those three keys. Detect them BEFORE
         * the gate, boot_fatal on empty value; everything else goes
         * through the original gate. */
        if (ki > 0 && vi == 0) {
            if (ascii_streq(key, "module") ||
                ascii_streq(key, "initrd") ||
                ascii_streq(key, "recovery_image")) {
                serial_early_print("[FATAL] boot.conf: '");
                serial_early_print(key);
                serial_early_print("' with empty value\n");
                boot_fatal(BOOT_ERR_CONF_INVALID,
                           "boot.conf: payload key with empty value",
                           "module=/initrd=/recovery_image= require a non-empty ESP-relative path.");
            }
        }

        /* Parse this key=value (original gate preserved for non-payload
         * keys so existing empty-value semantics do not regress). */
        if (ki > 0 && vi > 0)
            parse_conf_kv(cfg, key, val);
    }

    cfg->config_found = 1;
    gBS->FreePool(buf);
}

/* ============================================================================
 * section 5: Load staged payloads (module / initrd / recovery_image)
 *
 * Runs AFTER parse_boot_conf (so g_staged_payloads is populated) and
 * BEFORE load_kernel (so descriptor population finishes while Boot
 * Services are live). Any failure aborts the boot via boot_fatal --
 * "missing or malformed entries fail with a specific bootloader error"
 * per section 5 test checkpoint.
 * ============================================================================ */

/* Convert an ASCII path to UTF-16 on a caller-supplied stack buffer.
 * Returns 0 on overflow, 1 on success. The buffer must be sized for at
 * least (strlen(src) + 1) CHAR16 units; caller passes the declared
 * element count. */
static int ascii_to_utf16(const char *src, CHAR16 *dst, UINTN dst_max)
{
    UINTN i;
    for (i = 0; i < dst_max - 1 && src[i] != '\0'; i++) {
        dst[i] = (CHAR16)(UINT8)src[i];  /* ASCII subset is identity in UTF-16 */
    }
    dst[i] = (CHAR16)0;
    return src[i] == '\0';
}

/* Locate SimpleFS on the boot device (same pattern as load_kernel's
 * opening block). Returns EFI_SUCCESS + *out_fs on success; on failure
 * callers must not dereference *out_fs. */
static EFI_STATUS locate_boot_fs(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL **out_fs)
{
    EFI_GUID fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_STATUS status;
    *out_fs = (EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *)0;
    /* Fail closed: trust ONLY the explicit boot-device handle. The prior
     * gBS->LocateProtocol fallback could return ANY SimpleFS volume in the
     * system, letting split-path module=/initrd=/recovery_image= payloads
     * be sourced from a different ESP than the boot device -- a trust-
     * boundary leak. This mirrors load_kernel() (HandleProtocol-only on
     * g_boot_device_handle) and media_role_detect_and_record(), both of
     * which already refuse the ambient-volume fallback. Split-path payload
     * provenance hardening. */
    if (!g_boot_device_handle) {
        serial_early_print("[WARN] No boot-device handle for staged payloads\n");
        return EFI_NOT_FOUND;
    }
    status = gBS->HandleProtocol(g_boot_device_handle, &fs_guid,
                                 (VOID **)out_fs);
    /* Also guard against firmware returning EFI_SUCCESS with a NULL
     * interface pointer -- callers call (*out_fs)->OpenVolume directly. */
    if (EFI_ERROR(status) || *out_fs == (EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *)0) {
        serial_early_print("[WARN] No SimpleFS on boot device for staged payloads\n");
        *out_fs = (EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *)0;
        return EFI_NOT_FOUND;
    }
    return EFI_SUCCESS;
}

/* === Multi-OS chainload detection (TODO-27 sec1) =========================
 * Enumerate every SimpleFileSystem volume except our own boot ESP, probe the
 * well-known foreign UEFI bootloader paths, and record each as a chainload
 * target. The TODO-07 menu renders them; chainload_exec() LoadImage+StartImage
 * the selection (Secure Boot, when on, makes the firmware verify the foreign
 * loader against db/dbx -- EFI_SECURITY_VIOLATION is skipped). The probe is
 * read-only: nothing executes until the user selects an entry. */
#define CHAINLOAD_MAX 8u

struct chainload_target {
    EFI_HANDLE    volume_handle;   /* SFS handle the loader lives on */
    const CHAR16 *file_path;       /* \EFI\...\xxx.efi on that volume */
    const char   *os_name;         /* menu title */
};
static struct chainload_target g_chainload_targets[CHAINLOAD_MAX];
static UINTN g_chainload_count;

/* Well-known foreign UEFI bootloader probe table. shim precedes grub (shim is
 * the Secure-Boot first stage). The first hit on a volume wins. */
struct chainload_probe { const CHAR16 *path; const char *os; };
static const struct chainload_probe g_chainload_probes[] = {
    { u"\\EFI\\Microsoft\\Boot\\bootmgfw.efi", "Windows Boot Manager" },
    { u"\\EFI\\ubuntu\\shimx64.efi",             "Ubuntu" },
    { u"\\EFI\\ubuntu\\grubx64.efi",             "Ubuntu" },
    { u"\\EFI\\fedora\\shimx64.efi",             "Fedora" },
    { u"\\EFI\\fedora\\grubx64.efi",             "Fedora" },
    { u"\\EFI\\debian\\grubx64.efi",             "Debian" },
    { u"\\EFI\\opensuse\\shim.efi",              "openSUSE" },
};
#define CHAINLOAD_PROBE_COUNT (sizeof(g_chainload_probes) / sizeof(g_chainload_probes[0]))

/* Probe one volume's root for the first matching foreign bootloader. Returns
 * the probe index on a hit, -1 on none / open failure. Read-only: opens the
 * candidate READ then closes it immediately. */
static int chainload_probe_volume(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs)
{
    EFI_FILE_PROTOCOL *root = (EFI_FILE_PROTOCOL *)0;
    UINTN i;
    if (!fs || EFI_ERROR(fs->OpenVolume(fs, &root)) || !root)
        return -1;
    for (i = 0; i < CHAINLOAD_PROBE_COUNT; i++) {
        EFI_FILE_PROTOCOL *f = (EFI_FILE_PROTOCOL *)0;
        EFI_STATUS s = root->Open(root, &f, (CHAR16 *)g_chainload_probes[i].path,
                                  EFI_FILE_MODE_READ, 0);
        if (!EFI_ERROR(s) && f) {
            f->Close(f);
            root->Close(root);
            return (int)i;
        }
    }
    root->Close(root);
    return -1;
}

/* Enumerate all SimpleFileSystem volumes (except our boot ESP) and record up to
 * CHAINLOAD_MAX foreign bootloaders into g_chainload_targets. Logs each on
 * serial. Called pre-EBS once the boot device is known; clean no-op when no
 * foreign loaders exist. */
static void chainload_detect(void)
{
    EFI_GUID sfs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_HANDLE *handles = (EFI_HANDLE *)0;
    UINTN count = 0, i;
    EFI_STATUS status;

    g_chainload_count = 0;
    status = gBS->LocateHandleBuffer(ByProtocol, &sfs_guid, (VOID *)0,
                                     &count, &handles);
    if (EFI_ERROR(status) || !handles || count == 0)
        return;

    for (i = 0; i < count && g_chainload_count < CHAINLOAD_MAX; i++) {
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = (EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *)0;
        int pi;
        if (handles[i] == g_boot_device_handle)
            continue;   /* our own boot ESP is not a foreign chainload target */
        if (EFI_ERROR(gBS->HandleProtocol(handles[i], &sfs_guid, (VOID **)&fs)) || !fs)
            continue;
        pi = chainload_probe_volume(fs);
        if (pi < 0)
            continue;
        g_chainload_targets[g_chainload_count].volume_handle = handles[i];
        g_chainload_targets[g_chainload_count].file_path = g_chainload_probes[pi].path;
        g_chainload_targets[g_chainload_count].os_name = g_chainload_probes[pi].os;
        g_chainload_count++;
        serial_early_print("[MULTIBOOT] detected ");
        serial_early_print(g_chainload_probes[pi].os);
        serial_early_print("\n");
    }
    gBS->FreePool(handles);
    if (g_chainload_count == 0)
        serial_early_print("[MULTIBOOT] no foreign OS bootloaders found\n");
}

/* MEDIA_FILEPATH device-path subtype (UEFI 2.10 Table 10-58). */
#define EFI_DP_MEDIA_FILEPATH 0x04

/* Build a full device path = the volume's device path (END node dropped) + a
 * FILEPATH node for `file_path` + END_ENTIRE. Caller FreePool()s *out_dp. Every
 * size is bounded; *out_dp is NULL on failure. */
static EFI_STATUS chainload_build_devpath(EFI_HANDLE vol_handle,
                                          const CHAR16 *file_path,
                                          EFI_DEVICE_PATH_PROTOCOL **out_dp)
{
    EFI_GUID dp_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
    EFI_GUID dpu_guid = EFI_DEVICE_PATH_UTILITIES_PROTOCOL_GUID;
    EFI_DEVICE_PATH_PROTOCOL *vol_dp = (EFI_DEVICE_PATH_PROTOCOL *)0;
    EFI_DEVICE_PATH_UTILITIES_PROTOCOL *dpu = (EFI_DEVICE_PATH_UTILITIES_PROTOCOL *)0;
    EFI_DEVICE_PATH_PROTOCOL *node;
    UINTN measured, off = 0, vol_len = 0, name_chars = 0, fp_node_len, total, i;
    UINT8 *out;
    EFI_STATUS s;

    *out_dp = (EFI_DEVICE_PATH_PROTOCOL *)0;
    if (!vol_handle || !file_path)
        return EFI_INVALID_PARAMETER;
    s = gBS->HandleProtocol(vol_handle, &dp_guid, (VOID **)&vol_dp);
    if (EFI_ERROR(s) || !vol_dp)
        return EFI_NOT_FOUND;

    /* Measure the firmware-owned device path via GetDevicePathSize -- never
     * self-walk past the object on a heuristic cap -- a cap is not an object
     * bound. 0 = utility absent or degenerate -> refuse. */
    (void)gBS->LocateProtocol(&dpu_guid, (VOID *)0, (VOID **)&dpu);
    measured = 0u;
    if (dpu && dpu->GetDevicePathSize) {
        UINTN sz = dpu->GetDevicePathSize(vol_dp);
        if (sz >= 4u && sz <= 65536u)        /* >64 KiB device path is absurd */
            measured = sz;
    }
    if (measured == 0u)
        return EFI_NOT_FOUND;

    /* Find the END node strictly within the measured size; vol_len is the
     * prefix before it. Every header/body access stays inside [0, measured). */
    for (;;) {
        UINTN nlen;
        if (off + 4u > measured)
            return EFI_INVALID_PARAMETER;          /* header would cross object */
        node = (EFI_DEVICE_PATH_PROTOCOL *)((UINT8 *)vol_dp + off);
        nlen = (UINTN)node->Length[0] | ((UINTN)node->Length[1] << 8);
        if (nlen < 4u || off + nlen > measured)
            return EFI_INVALID_PARAMETER;          /* node crosses object */
        if (node->Type == EFI_DP_TYPE_END) {
            vol_len = off;                          /* copy everything before END */
            break;
        }
        off += nlen;
    }

    while (file_path[name_chars])
        name_chars++;
    if (name_chars == 0u || name_chars > 1024u)
        return EFI_INVALID_PARAMETER;
    fp_node_len = 4u + (name_chars + 1u) * 2u;      /* header + CHAR16 path + NUL */
    total = vol_len + fp_node_len + 4u;             /* + END_ENTIRE */

    s = gBS->AllocatePool(EfiLoaderData, total, (VOID **)&out);
    if (EFI_ERROR(s) || !out)
        return EFI_OUT_OF_RESOURCES;

    for (i = 0; i < vol_len; i++)                   /* volume path, END dropped */
        out[i] = ((UINT8 *)vol_dp)[i];
    {                                               /* FILEPATH node */
        UINT8 *fp = out + vol_len;
        CHAR16 *pn = (CHAR16 *)(fp + 4);
        fp[0] = EFI_DP_TYPE_MEDIA;
        fp[1] = EFI_DP_MEDIA_FILEPATH;
        fp[2] = (UINT8)(fp_node_len & 0xFFu);
        fp[3] = (UINT8)((fp_node_len >> 8) & 0xFFu);
        for (i = 0; i <= name_chars; i++)           /* include the NUL */
            pn[i] = file_path[i];
    }
    {                                               /* END_ENTIRE node */
        UINT8 *e = out + vol_len + fp_node_len;
        e[0] = EFI_DP_TYPE_END;
        e[1] = EFI_DP_SUBTYPE_END_ENTIRE;
        e[2] = 4u;
        e[3] = 0u;
    }
    *out_dp = (EFI_DEVICE_PATH_PROTOCOL *)out;
    return EFI_SUCCESS;
}

/* Chainload a detected foreign UEFI bootloader. LoadImage with BootPolicy=FALSE
 * so the firmware Secure-Boot-verifies the image against db/dbx
 * (EFI_SECURITY_VIOLATION == dbx-revoked -> refuse + skip). On a clean StartImage
 * control transfers to the foreign loader and does not return; a return means it
 * exited without booting, so we reclaim the image and the caller falls back to
 * the normal IPOS path. */
static EFI_STATUS chainload_exec(const struct chainload_target *t)
{
    EFI_DEVICE_PATH_PROTOCOL *dp = (EFI_DEVICE_PATH_PROTOCOL *)0;
    EFI_HANDLE img = (EFI_HANDLE)0;
    UINTN exit_data_size = 0;
    CHAR16 *exit_data = (CHAR16 *)0;
    EFI_STATUS s;

    if (!t || !t->volume_handle || !t->file_path)
        return EFI_INVALID_PARAMETER;
    s = chainload_build_devpath(t->volume_handle, t->file_path, &dp);
    if (EFI_ERROR(s) || !dp) {
        serial_early_print("[MULTIBOOT] chainload: device-path build failed\n");
        return EFI_ERROR(s) ? s : EFI_LOAD_ERROR;
    }

    serial_early_print("[MULTIBOOT] chainloading ");
    serial_early_print(t->os_name);
    serial_early_print("\n");

    s = gBS->LoadImage((BOOLEAN)0, gImageHandle, (VOID *)dp, (VOID *)0, 0, &img);
    gBS->FreePool(dp);
    if (EFI_ERROR(s) || !img) {
        if (s == EFI_SECURITY_VIOLATION)
            serial_early_print("[MULTIBOOT] chainload REFUSED -- Secure Boot dbx\n");
        else
            serial_early_print("[MULTIBOOT] chainload LoadImage failed\n");
        if (img)
            gBS->UnloadImage(img);
        return EFI_ERROR(s) ? s : EFI_LOAD_ERROR;
    }

    s = gBS->StartImage(img, &exit_data_size, &exit_data);
    /* StartImage returned: the chainloaded loader exited without booting. */
    serial_early_print("[MULTIBOOT] chainload target exited; falling back\n");
    if (exit_data)
        gBS->FreePool(exit_data);
    gBS->UnloadImage(img);
    return s;
}

/* Append each detected foreign-OS bootloader as a CHAINLOAD boot entry so the
 * TODO-07 menu renders it. id "chainload-N" lets the post-menu dispatch recover
 * the g_chainload_targets[] index; foreign OSes sort after IPOS entries. No-op
 * when there are no targets or the entry array is full. */
static void chainload_synthesize(boot_entries_parse_result_t *parse)
{
    UINTN i;
    if (!parse)
        return;
    /* Hide store-provided chainload entries: TODO-07 sec13's
     * JSON chainload validator/dispatcher is deferred, so only synthesized
     * entries are functional. Marking the rest kind_skipped keeps them out of
     * the menu instead of admitting-then-demoting them on selection. */
    for (i = 0; i < (UINTN)parse->entry_count; i++) {
        if (parse->entries[i].kind == BOOT_ENTRY_KIND_CHAINLOAD &&
            !(parse->entries[i].flags & BOOT_ENTRY_FLAG_SYNTHESIZED))
            parse->entries[i].kind_skipped = 1;
    }
    for (i = 0; i < g_chainload_count; i++) {
        boot_entry_envelope_t *e;
        const char *os;
        UINTN j, k;
        if ((UINTN)parse->entry_count >= BOOT_ENTRIES_MAX_ENTRIES)
            break;
        e = &parse->entries[parse->entry_count];
        for (k = 0; k < sizeof(*e); k++) ((UINT8 *)e)[k] = 0;
        /* id = "chainload-N" (i < CHAINLOAD_MAX == 8, single digit) */
        {
            const char *pfx = "chainload-";
            j = 0;
            while (pfx[j] && j < sizeof(e->id) - 2u) { e->id[j] = pfx[j]; j++; }
            e->id[j++] = (char)('0' + (int)i);
            e->id[j] = 0;
        }
        os = g_chainload_targets[i].os_name;
        j = 0;
        while (os[j] && j < sizeof(e->title) - 1u) { e->title[j] = os[j]; j++; }
        e->title[j] = 0;
        {
            const char *sk = "zzz-chainload-";
            j = 0;
            while (sk[j] && j < sizeof(e->sort_key) - 2u) { e->sort_key[j] = sk[j]; j++; }
            e->sort_key[j++] = (char)('0' + (int)i);
            e->sort_key[j] = 0;
        }
        e->kind = BOOT_ENTRY_KIND_CHAINLOAD;
        /* TRUSTED_CHAINLOAD: synthesized entries pass the Secure-Boot chainload
         * gate (boot_policy.c PATH_ESCAPE) because the firmware verifies the
         * loader at LoadImage; SYNTHESIZED marks them as our own provenance. */
        e->flags = BOOT_ENTRY_FLAG_ACTIVE | BOOT_ENTRY_FLAG_SYNTHESIZED |
                   BOOT_ENTRY_FLAG_TRUSTED_CHAINLOAD;
        e->payload_present = 0;
        e->kind_skipped = 0;
        parse->entry_count++;
    }
}

static void load_staged_payloads(void)
{
    if (g_staged_payload_count == 0 && g_staged_payload_overflow == 0) {
        /* Common path: no module/initrd/recovery_image in boot.conf.
         * payload_count / payload_overflow stay at their memset-zero
         * default; validator short-circuits on count=0 with BOOT_OK. */
        return;
    }

    /* Fail-closed BEFORE any file I/O when boot.conf overflowed the
     * staging table. Without this early gate, the loader would open the
     * filesystem and allocate up to BOOT_PAYLOAD_STAGE_MAX payloads
     * despite already knowing the handoff is incomplete. The kernel's
     * Phase-0 validator does reject with OVERFLOW_TRUNCATED later, but
     * by then the operator may have seen a misleading open/OOM/short-
     * read diagnostic from a mid-loop failure that masks the real
     * cause. Halt here with the actual reason so the user knows to
     * trim boot.conf instead of chasing a phantom file error. */
    if (g_staged_payload_overflow != 0) {
        serial_early_print("[FATAL] boot.conf: staged ");
        serial_early_print_uint((UINT32)g_staged_payload_count);
        serial_early_print(" + dropped ");
        serial_early_print_uint(g_staged_payload_overflow);
        serial_early_print(" payload entries; max allowed=");
        serial_early_print_uint((UINT32)BOOT_PAYLOAD_STAGE_MAX);
        serial_early_print("\n");
        boot_fatal(BOOT_ERR_CONF_INVALID,
                   "boot.conf has too many payload entries",
                   "module/initrd/recovery_image total exceeds the "
                   "fixed staging table; trim entries or raise "
                   "BOOT_PAYLOAD_STAGE_MAX.");
    }

    serial_early_print("[BOOT] load_staged_payloads: staging=");
    serial_early_print_uint((UINT32)g_staged_payload_count);
    serial_early_print("\n");

    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
    EFI_STATUS status = locate_boot_fs(&fs);
    if (EFI_ERROR(status)) {
        boot_fatal(BOOT_ERR_CONF_INVALID,
                   "boot.conf names module/initrd/recovery_image but no filesystem is available",
                   "Check the boot device partition and SimpleFS driver.");
    }

    EFI_FILE_PROTOCOL *root_dir;
    status = fs->OpenVolume(fs, &root_dir);
    if (EFI_ERROR(status)) {
        boot_fatal(BOOT_ERR_CONF_INVALID,
                   "boot.conf payload load: OpenVolume failed",
                   "Boot device filesystem is unavailable or corrupt.");
    }

    UINT64 total_bytes = 0ull;
    UINTN si;
    for (si = 0; si < g_staged_payload_count; si++) {
        struct staged_payload *stg = &g_staged_payloads[si];

        /* +1 for NUL; path came from an already-bounded ASCII buffer so
         * length fits. */
        CHAR16 wpath[BOOT_PAYLOAD_PATH_MAX];
        if (!ascii_to_utf16(stg->path, wpath, BOOT_PAYLOAD_PATH_MAX)) {
            boot_fatal(BOOT_ERR_CONF_INVALID,
                       "boot.conf payload path UTF-16 conversion overflow",
                       "Payload path length exceeds bootloader buffer.");
        }

        EFI_FILE_PROTOCOL *file;
        status = root_dir->Open(root_dir, &file, wpath,
                                EFI_FILE_MODE_READ, 0);
        if (EFI_ERROR(status)) {
            serial_early_print("[FATAL] boot.conf payload open failed: '");
            serial_early_print(stg->path);
            serial_early_print("' status=0x");
            serial_early_print_hex64((UINT64)status);
            serial_early_print("\n");
            root_dir->Close(root_dir);
            boot_fatal(BOOT_ERR_CONF_INVALID,
                       "boot.conf named payload not found or unreadable",
                       "Check the path spelling and that the file is on the boot ESP.");
        }

        /* FILE_INFO size probe (identical pattern to boot.conf loader). */
        EFI_GUID file_info_guid = EFI_FILE_INFO_ID;
        UINTN info_size = 0;
        status = file->GetInfo(file, &file_info_guid, &info_size, (VOID *)0);
        if (status != EFI_BUFFER_TOO_SMALL || info_size == 0) {
            file->Close(file);
            root_dir->Close(root_dir);
            boot_fatal(BOOT_ERR_CONF_INVALID,
                       "boot.conf payload GetInfo size-probe failed",
                       "Cannot determine payload file size.");
        }
        VOID *info_buf = (VOID *)0;
        status = gBS->AllocatePool(EfiLoaderData, info_size, &info_buf);
        if (EFI_ERROR(status) || !info_buf) {
            file->Close(file);
            root_dir->Close(root_dir);
            boot_fatal(BOOT_ERR_CONF_INVALID,
                       "boot.conf payload AllocatePool(info) failed",
                       "Out of EfiLoaderData memory for FILE_INFO.");
        }
        status = file->GetInfo(file, &file_info_guid, &info_size, info_buf);
        if (EFI_ERROR(status)) {
            gBS->FreePool(info_buf);
            file->Close(file);
            root_dir->Close(root_dir);
            boot_fatal(BOOT_ERR_CONF_INVALID,
                       "boot.conf payload GetInfo read failed",
                       "Payload file metadata unreadable.");
        }
        UINT64 file_size = ((EFI_FILE_INFO *)info_buf)->FileSize;
        gBS->FreePool(info_buf);

        if (file_size == 0) {
            file->Close(file);
            root_dir->Close(root_dir);
            serial_early_print("[FATAL] boot.conf payload is empty: '");
            serial_early_print(stg->path);
            serial_early_print("'\n");
            boot_fatal(BOOT_ERR_CONF_INVALID,
                       "boot.conf payload file is empty",
                       "Remove the entry or replace the file.");
        }

        /* Sanity cap against corrupt / malicious FAT metadata. Below
         * the cap, the (file_size + EFI_PAGE_SIZE - 1) overflow is
         * mathematically impossible (256 MiB + 4 KiB < UINT64_MAX),
         * so a single ceiling check covers both bugs at once. */
        if (file_size > BOOT_PAYLOAD_FILE_MAX) {
            file->Close(file);
            root_dir->Close(root_dir);
            serial_early_print("[FATAL] boot.conf payload implausibly large: '");
            serial_early_print(stg->path);
            serial_early_print("' FileSize=0x");
            serial_early_print_hex64(file_size);
            serial_early_print(" (cap=256 MiB)\n");
            boot_fatal(BOOT_ERR_CONF_INVALID,
                       "boot.conf payload exceeds 256 MiB sanity cap",
                       "Filesystem likely corrupt, or payload is wrong file.");
        }

        /* Page-align the allocation so the resulting phys_start is a
         * multiple of 4 KiB and matches the descriptor's alignment=4096
         * promise. AllocatePages always returns page-aligned addresses.
         * Overflow-safe because file_size <= 256 MiB per the cap above. */
        UINTN pages = (UINTN)((file_size + (UINT64)EFI_PAGE_SIZE - 1ull)
                               / (UINT64)EFI_PAGE_SIZE);
        EFI_PHYSICAL_ADDRESS payload_addr = 0;
        status = gBS->AllocatePages(AllocateAnyPages, EfiLoaderData,
                                    pages, &payload_addr);
        if (EFI_ERROR(status)) {
            file->Close(file);
            root_dir->Close(root_dir);
            boot_fatal(BOOT_ERR_CONF_INVALID,
                       "boot.conf payload AllocatePages failed",
                       "Out of EfiLoaderData memory for payload buffer.");
        }

        UINTN read_size = (UINTN)file_size;
        status = file->Read(file, &read_size, (VOID *)(UINTN)payload_addr);
        file->Close(file);
        if (EFI_ERROR(status)) {
            root_dir->Close(root_dir);
            boot_fatal(BOOT_ERR_CONF_INVALID,
                       "boot.conf payload Read failed",
                       "Short or erroring read from payload file.");
        }
        if ((UINT64)read_size != file_size) {
            serial_early_print("[FATAL] boot.conf payload short read: '");
            serial_early_print(stg->path);
            serial_early_print("' expected=");
            serial_early_print_uint((UINT32)file_size);
            serial_early_print(" got=");
            serial_early_print_uint((UINT32)read_size);
            serial_early_print("\n");
            root_dir->Close(root_dir);
            boot_fatal(BOOT_ERR_CONF_INVALID,
                       "boot.conf payload short read",
                       "File truncated during read.");
        }

        /* Populate the matching payload_descriptors[] slot. Packed
         * prefix: slot index == si, so payload_count becomes
         * g_staged_payload_count at the end. */
        struct boot_payload_desc *d = &g_boot_info_ptr->payload_descriptors[si];
        d->type        = stg->type;
        /* FLAG_RESERVED is mandatory on every bootloader-loaded typed
         * payload: the allocation lives in EfiLoaderData, which PMM
         * would otherwise reclaim to the free-page pool after boot.
         * Without this flag the payload pages can be re-handed out as
         * generic kernel memory, silently corrupting the module /
         * initrd / recovery image AFTER boot_payload_validate already
         * passed. PMM reservation of these ranges is owned by the
         * Handoff Memory Ownership section (see XREF in Notes). */
        d->flags       = BOOT_PAYLOAD_FLAG_VALID | BOOT_PAYLOAD_FLAG_RESERVED;
        d->phys_start  = (UINT64)payload_addr;
        d->length      = file_size;
        d->alignment   = 4096ull;  /* AllocatePages guarantees page align */
        d->checksum    = 0ull;     /* FLAG_CHECKSUMMED not set; reserved */
        d->producer_id = BOOT_PRODUCER_UEFI;
        d->_reserved   = 0u;

        total_bytes += file_size;

        serial_early_print("[BOOT] payload[");
        serial_early_print_uint((UINT32)si);
        serial_early_print("] type=");
        serial_early_print_uint(stg->type);
        serial_early_print(" phys=0x");
        serial_early_print_hex64((UINT64)payload_addr);
        serial_early_print(" len=");
        serial_early_print_uint((UINT32)file_size);
        serial_early_print(" path='");
        serial_early_print(stg->path);
        serial_early_print("'\n");
    }

    root_dir->Close(root_dir);

    g_boot_info_ptr->payload_count       = (UINT32)g_staged_payload_count;
    g_boot_info_ptr->payload_overflow    = g_staged_payload_overflow;
    g_boot_info_ptr->payload_total_bytes = total_bytes;
}

/* ============================================================================
 * Boot policy invocation -- ESP store + Boot####.OptionalData + ladder.
 *
 * Runs pre-EBS, between init_gop OK and load_kernel. Reads
 * \EFI\ImpossibleOS\bootentries.json from the boot device, decodes
 * Boot####.OptionalData for the BootCurrent->internal-id mapping, runs
 * the pure-C boot_policy_decide() ladder, and copies the decision into
 * the boot_info v19 selection ABI. The chosen entry's kind is held in
 * g_policy_selected_kind so the post-EBS boot-decision populate block
 * can apply the path-changing kind override AFTER the media-role
 * switch. The counter directory scan + crash-tolerant decrement
 * (write-new + Flush() + Close(success) + delete-old) ship in this
 * same invocation; the kernel-side Phase-0 v19 ABI validator lives
 * in src/kernel/main/boot_decision.c.
 * ============================================================================ */

/* Read SecureBoot + SetupMode + AuditMode pre-EBS so the policy filter's
 * path-escape gate (chainload + Secure Boot + missing trusted_chainload
 * flag) can fire. boot_info.secure_boot_enabled is kernel-populated
 * AFTER ExitBootServices (the kernel re-reads SecureBoot via UEFI RT)
 * and is not trustworthy here. UEFI 2.10 specification section 32.3:
 * SB is active iff SecureBoot == 1 AND SetupMode == 0. AuditMode == 1
 * means SB is in audit-only mode (signature verification logged but
 * not enforced) -- treat as not-enforcing for the path-escape gate.
 * The boot-menu feature reuses this helper to drive the menu indicator. */
static int bootloader_secureboot_active(void)
{
    EFI_GUID global_guid = EFI_GLOBAL_VARIABLE_GUID;
    EFI_RUNTIME_SERVICES *rt = gST ? gST->RuntimeServices : (EFI_RUNTIME_SERVICES *)0;
    if (!rt || !rt->GetVariable) return 0;

    UINT8 sb = 0, setup = 0, audit = 0;
    UINT32 attrs = 0;
    UINTN sz;

    sz = sizeof(sb);
    if (EFI_ERROR(rt->GetVariable(u"SecureBoot", &global_guid, &attrs, &sz, &sb))
        || sz != sizeof(sb))
        return 0;
    if (sb != 1) return 0;

    sz = sizeof(setup);
    if (!EFI_ERROR(rt->GetVariable(u"SetupMode", &global_guid, &attrs, &sz, &setup))
        && sz == sizeof(setup) && setup != 0)
        return 0;

    sz = sizeof(audit);
    if (!EFI_ERROR(rt->GetVariable(u"AuditMode", &global_guid, &attrs, &sz, &audit))
        && sz == sizeof(audit) && audit != 0)
        return 0;

    return 1;
}

/* Decode Boot####.OptionalData where #### == BootCurrent. The optional
 * data trails the EFI_LOAD_OPTION header + Description (UCS-2 NUL-term)
 * + FilePathList (FilePathListLength bytes). When the trailing blob
 * starts with the 5-byte tag "IPOS\x01", the next bytes up to a NUL or
 * end-of-blob are the kebab-case internal entry id; copy NUL-terminated
 * into out_id (max 47 chars + NUL) and set *out_known = 1.
 *
 * Untrusted firmware input: every offset is bounded against lo_sz; a
 * truncated Description (no NUL within max_desc_bytes) leaves
 * *out_known = 0 without dereferencing past the buffer. Same for an
 * over-long FilePathList. */
static void read_boot_optionaldata_id(UINT16 boot_current,
                                      char out_id[64],
                                      int *out_known)
{
    out_id[0] = 0;
    *out_known = 0;
    if (boot_current == 0xFFFFu) return;

    EFI_GUID global_guid = EFI_GLOBAL_VARIABLE_GUID;
    EFI_RUNTIME_SERVICES *rt = gST ? gST->RuntimeServices : (EFI_RUNTIME_SERVICES *)0;
    if (!rt || !rt->GetVariable) return;

    static const char hex[] = "0123456789ABCDEF";
    CHAR16 var_name[] = u"Boot0000";
    var_name[4] = (CHAR16)hex[(boot_current >> 12) & 0xF];
    var_name[5] = (CHAR16)hex[(boot_current >> 8) & 0xF];
    var_name[6] = (CHAR16)hex[(boot_current >> 4) & 0xF];
    var_name[7] = (CHAR16)hex[boot_current & 0xF];

    UINT8 lo_buf[2048];
    UINTN lo_sz = sizeof(lo_buf);
    UINT32 attrs = 0;
    EFI_STATUS s = rt->GetVariable(var_name, &global_guid, &attrs, &lo_sz, lo_buf);
    if (EFI_ERROR(s) || lo_sz <= 6u) return;

    UINT16 fp_len = (UINT16)(lo_buf[4] | ((UINT16)lo_buf[5] << 8));
    const CHAR16 *desc = (const CHAR16 *)&lo_buf[6];
    UINTN max_desc_bytes = lo_sz - 6u;

    /* Walk Description until NUL CHAR16. */
    UINTN di;
    UINTN desc_end_byte = (UINTN)-1;
    for (di = 0; (di * 2u + 1u) < max_desc_bytes; di++) {
        if (desc[di] == 0) {
            desc_end_byte = 6u + (di + 1u) * 2u;  /* byte after NUL CHAR16 */
            break;
        }
    }
    if (desc_end_byte == (UINTN)-1) return;
    if (desc_end_byte + (UINTN)fp_len > lo_sz) return;

    UINTN od_off = desc_end_byte + (UINTN)fp_len;
    UINTN od_len = lo_sz - od_off;
    if (od_len < 6u) return;  /* IPOS\x01 + at least 1 id byte */

    const UINT8 *od = lo_buf + od_off;
    if (od[0] != 'I' || od[1] != 'P' || od[2] != 'O' || od[3] != 'S' || od[4] != 0x01)
        return;

    /* Copy ASCII id NUL-terminated; reject non-kebab chars defensively
     * to keep a malformed firmware variable from poisoning the ladder
     * input. id grammar = [a-z0-9-] (kebab-case). The terminator MUST
     * be a NUL byte (or the buffer's natural end-of-blob); if the loop
     * ran out the 47-char cap WITHOUT seeing one, the firmware id was
     * longer than the cap and any prefix we'd publish is a lie -- a
     * 47-char-without-NUL "default-os-with-extra-tail" would silently
     * promote to "default-os" and falsely match an existing entry.
     * Reject the OptionalData mapping outright in that case. */
    UINTN i;
    int saw_terminator = 0;
    for (i = 0; i < 47u && (5u + i) < od_len; i++) {
        UINT8 c = od[5u + i];
        if (c == 0) { saw_terminator = 1; break; }
        int kebab = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        if (!kebab) {
            out_id[0] = 0;
            return;
        }
        out_id[i] = (char)c;
    }
    /* Termination matrix. UEFI variable length is authoritative
     * (the firmware does not truncate OptionalData), so end-of-blob
     * IS a valid terminator even at the 47-char cap.
     *   - in-loop NUL break: saw_terminator already set inside the
     *     loop, post-loop guard is no-op.
     *   - end-of-blob ((5+i) >= od_len): id ended exactly there;
     *     accept regardless of i.
     *   - i == 47 with byte at (5+i) == 0: explicit NUL just past
     *     the max-length id; accept.
     *   - i == 47 with byte at (5+i) != 0 AND (5+i) < od_len: cap
     *     hit AND the firmware id continues past 47 chars; the
     *     47-char prefix could silently promote to a real entry id,
     *     so reject. */
    if (!saw_terminator) {
        if ((5u + i) >= od_len) {
            saw_terminator = 1;  /* end-of-blob */
        } else if (od[5u + i] == 0) {
            saw_terminator = 1;  /* explicit NUL just past copied region */
        }
        /* else: i == 47 with non-NUL data following = reject */
    }
    out_id[i] = 0;
    /* Full kebab-case grammar (matches the boot-entry parser at
     * boot_entries_parser.c sp_is_kebab_char + the leading/trailing/
     * consecutive-dash gates): reject ids that start or end with '-',
     * or contain '--'. The per-char gate above already rejected
     * non-kebab bytes. Accepting a malformed id here would pollute
     * boot_info.rejected_entries[] with noise from untrusted firmware
     * even though the malformed id can never match a valid store
     * entry. Defense-in-depth keeps the audit surface clean. */
    if (i > 0 && saw_terminator) {
        if (out_id[0] == '-' || out_id[i - 1u] == '-') {
            out_id[0] = 0;
            return;
        }
        UINTN j;
        for (j = 1; j < i; j++) {
            if (out_id[j] == '-' && out_id[j - 1u] == '-') {
                out_id[0] = 0;
                return;
            }
        }
        *out_known = 1;
    }
}

/* Read \EFI\ImpossibleOS\bootentries.json into a freshly AllocatePool'd
 * buffer. Mirror parse_boot_conf's size-probe + AllocatePool shape.
 * Returns EFI_SUCCESS + (*out_buf, *out_len) on success; EFI_NOT_FOUND
 * when the file is absent (ladder treats this as STORE_INVALID and
 * synthesizes the fallback envelope). hard-fails via boot_fatal() when
 * the file exceeds BOOT_ENTRIES_MAX_TOTAL_BYTES (16 KiB) per the boot
 * entry file format spec -- anything larger means filesystem corruption
 * or hostile input, never silent truncation. */
static EFI_STATUS load_bootentries_json(unsigned char **out_buf, UINTN *out_len)
{
    *out_buf = (unsigned char *)0;
    *out_len = 0;

    if (!g_boot_device_handle) return EFI_NOT_FOUND;

    EFI_GUID fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = (EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *)0;
    EFI_STATUS status = gBS->HandleProtocol(g_boot_device_handle, &fs_guid,
                                            (VOID **)&fs);
    if (EFI_ERROR(status) || !fs) return EFI_NOT_FOUND;

    EFI_FILE_PROTOCOL *root_dir = (EFI_FILE_PROTOCOL *)0;
    status = fs->OpenVolume(fs, &root_dir);
    if (EFI_ERROR(status) || !root_dir) return EFI_NOT_FOUND;

    EFI_FILE_PROTOCOL *json_file = (EFI_FILE_PROTOCOL *)0;
    status = root_dir->Open(root_dir, &json_file,
                            u"\\EFI\\ImpossibleOS\\bootentries.json",
                            EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status) || !json_file) {
        root_dir->Close(root_dir);
        return EFI_NOT_FOUND;
    }

    UINT64 file_size = 0;
    {
        EFI_GUID file_info_guid = EFI_FILE_INFO_ID;
        UINTN info_size = 0;
        status = json_file->GetInfo(json_file, &file_info_guid, &info_size, (VOID *)0);
        if (status != EFI_BUFFER_TOO_SMALL || info_size == 0) {
            json_file->Close(json_file);
            root_dir->Close(root_dir);
            return EFI_NOT_FOUND;
        }
        VOID *info_buf = (VOID *)0;
        status = gBS->AllocatePool(EfiLoaderData, info_size, &info_buf);
        if (EFI_ERROR(status) || !info_buf) {
            json_file->Close(json_file);
            root_dir->Close(root_dir);
            serial_early_print("[BOOT] policy: AllocatePool exhaustion -- treating store as absent\n");
            return EFI_NOT_FOUND;
        }
        status = json_file->GetInfo(json_file, &file_info_guid, &info_size, info_buf);
        if (EFI_ERROR(status)) {
            gBS->FreePool(info_buf);
            json_file->Close(json_file);
            root_dir->Close(root_dir);
            return EFI_NOT_FOUND;
        }
        file_size = ((EFI_FILE_INFO *)info_buf)->FileSize;
        gBS->FreePool(info_buf);
    }

    if (file_size == 0) {
        json_file->Close(json_file);
        root_dir->Close(root_dir);
        return EFI_NOT_FOUND;
    }
    if (file_size > (UINT64)BOOT_ENTRIES_MAX_TOTAL_BYTES) {
        /* Per the boot entry schema fallback contract, an oversize
         * store is an INVALID store, not a fatal bootloader error --
         * boot_policy_invoke() routes EFI_NOT_FOUND through to the
         * FALLBACK_STORE_INVALID synthesis path so the operator gets
         * a usable fallback envelope instead of a halt. A hostile or
         * corrupt ESP cannot stop the machine just by inflating the
         * policy file. */
        json_file->Close(json_file);
        root_dir->Close(root_dir);
        serial_early_print("[BOOT] policy: bootentries.json oversize (");
        serial_early_print_uint((UINT32)file_size);
        serial_early_print(" bytes > 16 KiB cap) -- treating as invalid store\n");
        return EFI_NOT_FOUND;
    }

    unsigned char *buf = (unsigned char *)0;
    status = gBS->AllocatePool(EfiLoaderData, (UINTN)file_size + 1u, (VOID **)&buf);
    if (EFI_ERROR(status) || !buf) {
        json_file->Close(json_file);
        root_dir->Close(root_dir);
        serial_early_print("[BOOT] policy: AllocatePool exhaustion -- treating store as absent\n");
        return EFI_NOT_FOUND;
    }

    UINTN read_len = (UINTN)file_size;
    status = json_file->Read(json_file, &read_len, buf);
    json_file->Close(json_file);
    root_dir->Close(root_dir);

    if (EFI_ERROR(status) || read_len == 0) {
        gBS->FreePool(buf);
        return EFI_NOT_FOUND;
    }
    /* Reject short reads. UEFI Read() may legally return EFI_SUCCESS
     * with fewer bytes than requested on a flaky controller; feeding a
     * truncated prefix to the parser would let a syntactically-valid
     * JSON prefix poison the policy decision. boot-code-quality Gate 14
     * forbids silent truncation -- the corruption rule is "all or
     * nothing" for parsed config blobs. */
    if (read_len != (UINTN)file_size) {
        serial_early_print("[BOOT] policy: bootentries.json short read (");
        serial_early_print_uint((UINT32)read_len);
        serial_early_print(" of ");
        serial_early_print_uint((UINT32)file_size);
        serial_early_print(" bytes) -- rejecting as unreadable\n");
        gBS->FreePool(buf);
        return EFI_NOT_FOUND;
    }
    buf[read_len] = 0;
    *out_buf = buf;
    *out_len = read_len;
    return EFI_SUCCESS;
}

/* Counter directory path. UEFI Open() takes a backslash-rooted path
 * relative to volume root; CHAR16 form. */
static const CHAR16 g_counter_dir_path[] = u"\\EFI\\ImpossibleOS\\counters";

/* Open the boot device's root volume. Returns EFI_SUCCESS + (*out_root)
 * on success; caller MUST Close *out_root when done. */
static EFI_STATUS counters_open_volume_root(EFI_FILE_PROTOCOL **out_root)
{
    *out_root = (EFI_FILE_PROTOCOL *)0;
    if (!g_boot_device_handle) return EFI_NOT_FOUND;
    EFI_GUID fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = (EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *)0;
    EFI_STATUS s = gBS->HandleProtocol(g_boot_device_handle, &fs_guid, (VOID **)&fs);
    if (EFI_ERROR(s) || !fs) return EFI_NOT_FOUND;
    return fs->OpenVolume(fs, out_root);
}

/* Scan \EFI\ImpossibleOS\counters\ and parse each <id>+<L>-<D>
 * filename. Caps at BOOT_ENTRIES_MAX_ENTRIES; sets *out_overflow=1
 * if more files exist than the cap. Directory missing -> *out_count=0
 * (treated as no counters, ladder applies no gate). */
static void policy_scan_counters(boot_counter_t *out, unsigned int cap,
                                 unsigned int *out_count, int *out_overflow)
{
    *out_count = 0;
    *out_overflow = 0;
    post_code16(POST16_BL_COUNTER_SCAN);

    EFI_FILE_PROTOCOL *root = (EFI_FILE_PROTOCOL *)0;
    if (EFI_ERROR(counters_open_volume_root(&root)) || !root) return;

    EFI_FILE_PROTOCOL *dir = (EFI_FILE_PROTOCOL *)0;
    EFI_STATUS s = root->Open(root, &dir, (CHAR16 *)g_counter_dir_path,
                              EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(s) || !dir) {
        root->Close(root);
        return;
    }

    /* Loop over directory entries via repeated Read() until 0 bytes. */
    /* EFI_FILE_INFO + filename slack. UEFI spec section 13.5: Read on a
     * directory returns one EFI_FILE_INFO per call; the FileName field
     * is variable-length. 1024 bytes covers an ASCII filename up to
     * roughly 470 CHAR16 chars -- way more than the 64-char counter
     * filename cap. */
    UINT8 info_buf[1024];
    for (;;) {
        UINTN sz = sizeof(info_buf);
        s = dir->Read(dir, &sz, info_buf);
        /* Distinguish EOF (sz==0 + EFI_SUCCESS) from scan failure. Any
         * EFI_ERROR -- corrupt directory, transient media error, or
         * EFI_BUFFER_TOO_SMALL on a pathologically long filename --
         * means we cannot trust the partial set we have already read.
         * A malicious or stale ESP could otherwise hide an exhausted
         * record by injecting an early read error: the selected entry
         * would then reach policy_counter_decrement() with
         * counter_existed=0, get re-bootstrapped to +2-1, and
         * effectively un-demote. Fail closed by setting
         * counters_overflow=1 (the ladder treats it the same as the
         * cap-exceeded case: FALLBACK_NO_VIABLE). */
        if (EFI_ERROR(s)) {
            *out_overflow = 1;
            serial_early_print("[BOOT] policy: counter dir Read failed -- fail-closed\n");
            break;
        }
        if (sz == 0) break;  /* clean EOF */
        EFI_FILE_INFO *fi = (EFI_FILE_INFO *)info_buf;
        /* Skip directories ("." / ".." / nested) -- counters dir is flat. */
        if (fi->Attribute & EFI_FILE_DIRECTORY) continue;
        /* Convert UCS-2 filename to ASCII into a small stack buffer. */
        char ascii_name[BOOT_COUNTER_FILENAME_MAX];
        UINTN i;
        for (i = 0; i < sizeof(ascii_name) - 1u; i++) {
            CHAR16 c = fi->FileName[i];
            if (c == 0) break;
            if (c > 0x7F) { i = 0; break; }  /* non-ASCII: reject */
            ascii_name[i] = (char)c;
        }
        ascii_name[i] = 0;
        if (i == 0) continue;  /* empty or non-ASCII filename */

        boot_counter_t parsed;
        if (!boot_counter_parse_filename(ascii_name, (unsigned int)i, &parsed))
            continue;
        /* Always call the dedup helper -- a duplicate id at cap-full
         * MUST be allowed to merge (worst-case demotion of an existing
         * slot, not a new slot). Only -1 (NEW id rejected because cap
         * was already full) is genuine overflow. */
        int ins = boot_policy_counter_dedup_insert(out, out_count, cap, &parsed);
        if (ins < 0) {
            *out_overflow = 1;
            /* Stop scanning on first cap-overflow. The ladder fail-
             * closes (FALLBACK_NO_VIABLE) regardless of further
             * counter content once counters_overflow is set, so any
             * additional Read+parse work is wasted. Stopping here
             * also caps the worst-case pre-EBS time on a hostile or
             * stale ESP that contains thousands of parseable counter
             * filenames -- without this break, each spurious file
             * would force a firmware Read + UCS-2 conversion +
             * parse + dedup-scan with no policy benefit. */
            serial_early_print("[BOOT] policy: counter dir cap exceeded -- stopping scan (fail-closed)\n");
            break;
        }
    }
    /* seen > cap is no longer the trigger -- duplicates of existing
     * ids inflate `seen` past `cap` without representing overflow.
     * The per-insert -1 above is the authoritative overflow signal. */

    dir->Close(dir);
    root->Close(root);

    serial_early_print("[BOOT] policy: counter scan found ");
    serial_early_print_uint((UINT32)*out_count);
    serial_early_print(" counter(s)");
    if (*out_overflow) serial_early_print(" (overflow)");
    serial_early_print("\n");
}

/* Crash-tolerant decrement. NOT atomic rename. Per UEFI spec section
 * 13.5 the EFI_FILE_PROTOCOL.SetInfo rename is NOT power-fail-atomic on
 * FAT32: LFN entries can span multiple directory entries and a reset
 * mid-rename can leave torn names. The protocol is:
 *   1. Open(new_filename, CREATE) -> Flush -> Close. Both Flush and
 *      Close must return EFI_SUCCESS for the new file to be considered
 *      durably committed; if EITHER fails we skip step 2 and preserve
 *      the old file (next-boot scan dedupes).
 *   2. Open(old_filename) + Delete().
 * A reset between (1) and (2) leaves both files; the next-boot scan
 * resolves duplicates with worst-case semantics (min tries_left, max
 * tries_done) so a torn rename can never silently un-demote an entry.
 *
 * First-boot bootstrap (counter_existed=0): write `<id>+2-1` per the BLS
 * 3-try semantic (3 tries default, one already done). Without this
 * positive-budget bootstrap, single-entry installs would brick after one
 * boot before the health-gated mark-good feature ships. mark-good will
 * later DELETE the counter on success, returning the entry to the
 * "no counter, no gate" state. */
static void policy_counter_decrement(const char *id,
                                     unsigned int cur_left,
                                     unsigned int cur_done,
                                     int counter_existed)
{
    if (!id || id[0] == 0) return;
    post_code16(POST16_BL_COUNTER_DECR);

    /* Compute post-decrement state. First-boot bootstrap: jump to BLS-3
     * default (2 left, 1 done) -- treats the absent-counter case as
     * "fresh entry, default 3 tries, one consumed by this boot". */
    unsigned int new_left, new_done;
    if (counter_existed) {
        new_left = (cur_left > 0u) ? (cur_left - 1u) : 0u;
        new_done = (cur_done < BOOT_COUNTER_TRIES_DONE_MAX)
                       ? (cur_done + 1u) : BOOT_COUNTER_TRIES_DONE_MAX;
    } else {
        new_left = 2u;
        new_done = 1u;
    }

    EFI_FILE_PROTOCOL *root = (EFI_FILE_PROTOCOL *)0;
    if (EFI_ERROR(counters_open_volume_root(&root)) || !root) {
        serial_early_print("[BOOT] policy: counter decrement skipped (volume open failed)\n");
        return;
    }

    /* Open (or create) the counters directory. UEFI Open with CREATE
     * + EFI_FILE_DIRECTORY auto-mkdirs if absent. */
    EFI_FILE_PROTOCOL *dir = (EFI_FILE_PROTOCOL *)0;
    EFI_STATUS s = root->Open(root, &dir, (CHAR16 *)g_counter_dir_path,
                              EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE
                                | EFI_FILE_MODE_CREATE,
                              EFI_FILE_DIRECTORY);
    if (EFI_ERROR(s) || !dir) {
        /* Read-only or full filesystem: log + skip. The ladder treats
         * absent counter as "no gate" so boot continues; the entry just
         * isn't tracked. */
        serial_early_print("[BOOT] policy: counter dir open(CREATE) failed -- read-only ESP? skipping decrement\n");
        root->Close(root);
        return;
    }

    /* Build new + old filenames. */
    boot_counter_t new_c;
    char new_name[BOOT_COUNTER_FILENAME_MAX];
    {
        unsigned int idl = 0;
        while (id[idl] && idl < BOOT_ENTRIES_MAX_ID_LEN) idl++;
        for (unsigned int j = 0; j < idl; j++) new_c.id[j] = id[j];
        new_c.id[idl] = 0;
        new_c.tries_left = new_left;
        new_c.tries_done = new_done;
    }
    if (boot_counter_format_filename(&new_c, new_name, sizeof(new_name)) == 0) {
        serial_early_print("[BOOT] policy: counter format failed (id too long?)\n");
        dir->Close(dir);
        root->Close(root);
        return;
    }

    /* CHAR16-ify the new filename. */
    CHAR16 new_name_w[BOOT_COUNTER_FILENAME_MAX];
    {
        UINTN j;
        for (j = 0; j < sizeof(new_name_w) / sizeof(new_name_w[0]) - 1u; j++) {
            char c = new_name[j];
            if (c == 0) break;
            new_name_w[j] = (CHAR16)(unsigned char)c;
        }
        new_name_w[j] = 0;
    }

    /* Step 1: Open(new, CREATE|WRITE), Flush durability, Close.
     * The replacement MUST be durably committed before we touch the
     * old file. UEFI Close() is documented to flush (UEFI 2.10 spec
     * section 13.5) but firmware can still return failure on either
     * Flush or Close; if we ignore those statuses and proceed to
     * delete-old, a flush failure plus a reset can leave NEITHER the
     * old nor the new counter -- the next-boot scan would see the
     * entry as untracked, bootstrap to +2-1, and defeat the demotion
     * logic this section exists to provide.
     *
     * Safer ordering: Flush + Close on the new file; if EITHER fails
     * we log and SKIP the delete-old. Worst case we then have BOTH
     * files (the old is durable from a prior boot; the new may or
     * may not be durable). The next-boot scan resolves duplicates
     * conservatively (worst-case demotion); a torn replacement still
     * yields a coherent fail-closed state. */
    EFI_FILE_PROTOCOL *newf = (EFI_FILE_PROTOCOL *)0;
    s = dir->Open(dir, &newf, new_name_w,
                  EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
                  0);
    if (EFI_ERROR(s) || !newf) {
        serial_early_print("[BOOT] policy: counter Open(new, CREATE) failed for ");
        serial_early_print(new_name);
        serial_early_print("\n");
        dir->Close(dir);
        root->Close(root);
        return;
    }
    /* Flush explicitly. If Flush fails the new file is not durable,
     * so we MUST keep the old file intact. */
    EFI_STATUS flush_st = EFI_SUCCESS;
    if (newf->Flush) flush_st = newf->Flush(newf);
    EFI_STATUS close_st = newf->Close(newf);
    int new_durable = (!EFI_ERROR(flush_st) && !EFI_ERROR(close_st));
    if (!new_durable) {
        serial_early_print(
            "[BOOT] policy: counter new-file flush/close failed -- "
            "skipping delete-old (preserves old, scan resolves dup conservatively)\n");
        dir->Close(dir);
        root->Close(root);
        return;
    }

    /* Step 2: Open(old) + Delete(), iff there was an old. The old
     * filename is reconstructed from current state. Skip on first-boot
     * bootstrap (counter_existed=0). */
    if (counter_existed) {
        boot_counter_t old_c = new_c;
        old_c.tries_left = cur_left;
        old_c.tries_done = cur_done;
        char old_name[BOOT_COUNTER_FILENAME_MAX];
        if (boot_counter_format_filename(&old_c, old_name, sizeof(old_name)) > 0) {
            CHAR16 old_name_w[BOOT_COUNTER_FILENAME_MAX];
            UINTN j;
            for (j = 0; j < sizeof(old_name_w) / sizeof(old_name_w[0]) - 1u; j++) {
                char c = old_name[j];
                if (c == 0) break;
                old_name_w[j] = (CHAR16)(unsigned char)c;
            }
            old_name_w[j] = 0;

            /* If old == new (cur was already at the bootstrap state),
             * skip Open+Delete to avoid removing the file we just
             * created. */
            int same = 1;
            for (j = 0; j < sizeof(new_name); j++) {
                if (new_name[j] != old_name[j]) { same = 0; break; }
                if (new_name[j] == 0) break;
            }
            if (!same) {
                EFI_FILE_PROTOCOL *oldf = (EFI_FILE_PROTOCOL *)0;
                EFI_STATUS os = dir->Open(dir, &oldf, old_name_w,
                                          EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE,
                                          0);
                if (!EFI_ERROR(os) && oldf) {
                    oldf->Delete(oldf);  /* Delete also closes the handle. */
                }
            }
        }
    }

    serial_early_print("[BOOT] policy: counter decrement OK -> ");
    serial_early_print(new_name);
    if (!counter_existed) serial_early_print(" (first-boot bootstrap)");
    serial_early_print("\n");

    dir->Close(dir);
    root->Close(root);
}

/* serial_early_print signature for the parser logger callback. */
static void boot_policy_log_cb(const char *s) { serial_early_print(s); }

/* Trampoline so the ladder log callback formats numeric values without
 * relying on libc. Used to print selection_reason name + selected id. */
static const char *selection_reason_name(unsigned int r)
{
    switch (r) {
        case BOOT_SELECTION_UNSET:                 return "UNSET";
        case BOOT_SELECTION_STORE_DEFAULT:         return "STORE_DEFAULT";
        case BOOT_SELECTION_BOOTNEXT_HINT:         return "BOOTNEXT_HINT";
        case BOOT_SELECTION_HOTKEY:                return "HOTKEY";
        case BOOT_SELECTION_WATCHDOG_ROLLBACK:     return "WATCHDOG_ROLLBACK";
        case BOOT_SELECTION_AB_TRY_STATE:          return "AB_TRY_STATE";
        case BOOT_SELECTION_RECOVERY_REQUEST:      return "RECOVERY_REQUEST";
        case BOOT_SELECTION_FALLBACK_NO_VIABLE:    return "FALLBACK_NO_VIABLE";
        case BOOT_SELECTION_FALLBACK_STORE_INVALID:return "FALLBACK_STORE_INVALID";
        case BOOT_SELECTION_UNKNOWN_BOOTCURRENT:   return "UNKNOWN_BOOTCURRENT";
        default:                                   return "?";
    }
}

/* Write a minimal STORE_INVALID decision directly to boot_info v19
 * without going through the parser/decision heap path. Used when the
 * pre-EBS AllocatePool budget is exhausted -- as the live producer for
 * the v19 selection ABI we MUST publish a coherent decision instead of
 * leaving the fields at BSS-zero (BOOT_SELECTION_UNSET) which would
 * confuse the kernel-side Phase-0 ABI validator that lands in the next
 * section. The fallback envelope's id ("fallback") is the canonical
 * synth id; capture its kind so the populate block sees a sane
 * non-path-changing kind. */
static void boot_policy_publish_alloc_failure_fallback(const char *what)
{
    boot_entry_envelope_t fb;
    boot_entries_synthesize_fallback((g_uki_kernel_ptr != (UINT8 *)0) ? 1 : 0, &fb);

    /* Per the boot_info v19 ABI, FALLBACK_STORE_INVALID publishes an
     * EMPTY selected_entry_id; the synthesized "fallback" name is
     * the load-time identifier, NOT the policy decision. Capture the
     * kind for the post-EBS populate block override but leave the id
     * field at its BSS-zero (empty NUL-terminated) state. */
    g_boot_info_ptr->selected_entry_id[0] = 0;
    g_boot_info_ptr->selection_reason = (UINT32)BOOT_SELECTION_FALLBACK_STORE_INVALID;
    g_boot_info_ptr->rejected_entry_count = 0u;
    g_boot_info_ptr->rejected_entry_overflow = 0u;
    g_policy_selected_kind = fb.kind;

    serial_early_print("[BOOT] policy: ");
    serial_early_print(what);
    serial_early_print(" AllocatePool failed -- publishing FALLBACK_STORE_INVALID\n");
}

/* Boot menu cap. UEFI spec section 13.5 supports far more entries, but
 * pragmatic UX caps at 16 visible rows -- matches systemd-boot. */
#define BOOT_MENU_MAX_VISIBLE 16

/* Default countdown when no per-entry override is set. 5 seconds per
 * the boot menu spec. */
#define BOOT_MENU_DEFAULT_TIMEOUT_S 5u

/* Effective countdown cap. The firmware watchdog at watchdog_reset()
 * runs on a 60s schedule (WD_TIMEOUT in efi_main); the menu loop calls
 * watchdog_reset() each tick to keep the watchdog fed, but a sanity
 * cap on the per-entry timeout_override prevents pathological values
 * (parser allows up to 600) from staying in interactive mode for far
 * longer than any kiosk operator expects. */
#define BOOT_MENU_TIMEOUT_CAP_S 60u

/* Render one frame of the menu. GOP path uses the existing Selawik AA
 * font infrastructure (bsod_aa_string + bsod_aa_TITLE / bsod_aa_SUB
 * tables); when the framebuffer is unavailable we fall back to
 * gST->ConOut->OutputString. Serial mirror always logs the current
 * highlighted index for diagnostic visibility. */
/* Compose ASCII indicator tags for one entry into a fixed buffer.
 * ASCII-only because the Selawik AA atlas covers 0x20..0x7E and
 * Windows serial consoles garble multi-byte UTF-8.
 *
 *   [SB]   Secure Boot active (firmware reports SB enabled)
 *   [REC]  recovery entry (kind == RECOVERY)
 *   [NET]  network entry (kind == NETWORK)
 *   [FAIL] last-failure recorded for this id in decision->rejected[]
 *
 * [MB] (measured boot) is reserved for the TPM event-log integration
 * and stays out of menu-indicators ship -- event-log producer is in
 * the measured-boot TODO domain.
 *
 * out_buf is a >= 32-byte caller-owned buffer; returns out_buf. */
static const char *boot_menu_indicators(const boot_entry_envelope_t *e,
                                        const boot_policy_decision_t *decision,
                                        int sb_active,
                                        char *out_buf,
                                        unsigned int out_cap)
{
    unsigned int p = 0;
    if (out_cap == 0u) return out_buf;
    out_buf[0] = 0;

    if (sb_active && p + 5u < out_cap) {
        out_buf[p++] = '['; out_buf[p++] = 'S'; out_buf[p++] = 'B'; out_buf[p++] = ']';
        out_buf[p++] = ' ';
    }
    if (e->kind == BOOT_ENTRY_KIND_RECOVERY && p + 6u < out_cap) {
        out_buf[p++] = '['; out_buf[p++] = 'R'; out_buf[p++] = 'E'; out_buf[p++] = 'C';
        out_buf[p++] = ']'; out_buf[p++] = ' ';
    }
    if (e->kind == BOOT_ENTRY_KIND_NETWORK && p + 6u < out_cap) {
        out_buf[p++] = '['; out_buf[p++] = 'N'; out_buf[p++] = 'E'; out_buf[p++] = 'T';
        out_buf[p++] = ']'; out_buf[p++] = ' ';
    }
    if (decision != (const boot_policy_decision_t *)0) {
        for (unsigned int r = 0; r < decision->rejected_count; r++) {
            unsigned int reason = decision->rejected[r].reason;
            if (reason == (unsigned int)BOOT_REJECT_REASON_NONE) continue;
            const char *a = e->id;
            const char *b = decision->rejected[r].id;
            unsigned int j;
            int eq = 1;
            for (j = 0; j < sizeof(e->id); j++) {
                if (a[j] != b[j]) { eq = 0; break; }
                if (a[j] == 0) break;
            }
            if (!eq) continue;
            if (p + 7u < out_cap) {
                out_buf[p++] = '['; out_buf[p++] = 'F'; out_buf[p++] = 'A';
                out_buf[p++] = 'I'; out_buf[p++] = 'L'; out_buf[p++] = ']';
                out_buf[p++] = ' ';
            }
            break;
        }
    }
    out_buf[p] = 0;
    return out_buf;
}

static void boot_menu_render(const boot_entries_parse_result_t *parse,
                             const unsigned int *cand_idx,
                             unsigned int cand_count,
                             unsigned int selected_idx,
                             unsigned int seconds_left,
                             int gop_available,
                             const boot_policy_decision_t *decision,
                             int sb_active)
{
    /* Serial mirror -- always, regardless of GOP availability. The
     * smoke test asserts this line appears so headless operators see
     * the same selection state as graphical ones. */
    serial_early_print("[BOOT] menu: ");
    serial_early_print_uint((UINT32)cand_count);
    serial_early_print(" entries, selected=");
    if (selected_idx < cand_count) {
        const boot_entry_envelope_t *e = &parse->entries[cand_idx[selected_idx]];
        serial_early_print("\"");
        serial_early_print(e->id);
        serial_early_print("\"");
    } else {
        serial_early_print("(none)");
    }
    serial_early_print(" timeout=");
    serial_early_print_uint(seconds_left);
    serial_early_print("s\n");

    if (gop_available && gFramebuffer && gFbPitch > 0u && gFbHeight > 0u) {
        /* GOP path: clear top portion, render title + entry rows.
         * Use bsod_aa_TITLE for the heading and bsod_aa_SUB for entries. */
        const UINT32 row_h = 32;
        const UINT32 title_y = 60;
        const UINT32 list_y0 = 130;
        const UINT32 col_x = 80;
        UINT32 bg = fb_pack_rgb(0x0A, 0x0A, 0x0A);
        UINT32 hi_bg = fb_pack_rgb(0x40, 0x80, 0xC0);

        /* Clear background. */
        for (UINT32 y = 0; y < gFbHeight && y < (list_y0 + row_h * BOOT_MENU_MAX_VISIBLE + row_h); y++)
            for (UINT32 x = 0; x < gFbWidth; x++)
                gFramebuffer[y * gFbPitch + x] = bg;

        /* Title. */
        bsod_aa_string(col_x, title_y, "Impossible OS Boot Menu",
                       bsod_aa_TITLE, bsod_aa_TITLE_data, 28u,
                       0xE0, 0xE0, 0xE0, 0x0A, 0x0A, 0x0A);

        /* Entry rows. */
        for (unsigned int i = 0; i < cand_count && i < BOOT_MENU_MAX_VISIBLE; i++) {
            UINT32 y = list_y0 + (UINT32)i * row_h;
            const boot_entry_envelope_t *e = &parse->entries[cand_idx[i]];
            UINT8 fg_r = 0xC8, fg_g = 0xC8, fg_b = 0xC8;
            UINT8 bg_r = 0x0A, bg_g = 0x0A, bg_b = 0x0A;
            if (i == selected_idx) {
                /* Highlight: blue bar across the row. */
                for (UINT32 yi = y; yi < y + row_h && yi < gFbHeight; yi++)
                    for (UINT32 xi = col_x - 8; xi < gFbWidth && xi < col_x + 800; xi++)
                        gFramebuffer[yi * gFbPitch + xi] = hi_bg;
                fg_r = 0xFF; fg_g = 0xFF; fg_b = 0xFF;
                bg_r = 0x40; bg_g = 0x80; bg_b = 0xC0;
            }
            /* Indicator badges first (ASCII, atlas-safe), then title. */
            char ind[40];
            (void)boot_menu_indicators(e, decision, sb_active,
                                       ind, (unsigned int)sizeof(ind));
            const char *label = e->title[0] ? e->title : e->id;
            UINT32 lx = col_x;
            if (ind[0]) {
                bsod_aa_string(lx, y + 4u, ind,
                               bsod_aa_SUB, bsod_aa_SUB_data, 18u,
                               fg_r, fg_g, fg_b, bg_r, bg_g, bg_b);
                /* Approximate AA width per char: SUB at 18px ~ 11px/char. */
                unsigned int ind_len = 0;
                while (ind[ind_len]) ind_len++;
                lx += (UINT32)ind_len * 11u;
            }
            bsod_aa_string(lx, y + 4u, label,
                           bsod_aa_SUB, bsod_aa_SUB_data, 18u,
                           fg_r, fg_g, fg_b, bg_r, bg_g, bg_b);
        }

        /* Footer: countdown. */
        UINT32 footer_y = list_y0 + (UINT32)BOOT_MENU_MAX_VISIBLE * row_h + 30;
        if (footer_y < gFbHeight) {
            char foot[64];
            const char prefix[] = "Auto-boot in ";
            unsigned int p = 0;
            while (prefix[p] && p < sizeof(foot) - 8u) { foot[p] = prefix[p]; p++; }
            unsigned int sl = seconds_left;
            char buf[12];
            unsigned int n = 0;
            if (sl == 0) buf[n++] = '0';
            else {
                char rev[12];
                unsigned int rn = 0;
                while (sl > 0 && rn < 11) { rev[rn++] = (char)('0' + (sl % 10)); sl /= 10; }
                while (rn > 0) buf[n++] = rev[--rn];
            }
            for (unsigned int j = 0; j < n && p < sizeof(foot) - 4u; j++) foot[p++] = buf[j];
            const char suffix[] = "s. Use arrows + Enter.";
            for (unsigned int j = 0; suffix[j] && p < sizeof(foot) - 1u; j++) foot[p++] = suffix[j];
            foot[p] = 0;
            bsod_aa_string(col_x, footer_y, foot,
                           bsod_aa_SUB, bsod_aa_SUB_data, 18u,
                           0x80, 0x80, 0x80, 0x0A, 0x0A, 0x0A);
        }
    } else if (gST && gST->ConOut && gST->ConOut->OutputString) {
        /* ConOut text fallback. UEFI text console only -- no
         * fancy formatting. */
        gST->ConOut->OutputString(gST->ConOut, u"\r\n=== Impossible OS Boot Menu ===\r\n");
        for (unsigned int i = 0; i < cand_count && i < BOOT_MENU_MAX_VISIBLE; i++) {
            const boot_entry_envelope_t *e = &parse->entries[cand_idx[i]];
            char ind[40];
            (void)boot_menu_indicators(e, decision, sb_active,
                                       ind, (unsigned int)sizeof(ind));
            const char *label = e->title[0] ? e->title : e->id;
            CHAR16 line[200];
            unsigned int p = 0;
            line[p++] = (i == selected_idx) ? u'>' : u' ';
            line[p++] = u' ';
            for (unsigned int j = 0; ind[j] && p < sizeof(line)/sizeof(line[0]) - 4u; j++)
                line[p++] = (CHAR16)(unsigned char)ind[j];
            for (unsigned int j = 0; label[j] && p < sizeof(line)/sizeof(line[0]) - 4u; j++)
                line[p++] = (CHAR16)(unsigned char)label[j];
            line[p++] = u'\r';
            line[p++] = u'\n';
            line[p] = 0;
            gST->ConOut->OutputString(gST->ConOut, line);
        }
    }
}

/* Run the menu countdown + input loop. Returns the index in cand_idx[]
 * of the chosen entry; on countdown expiry, returns default_idx (the
 * ladder pick). The watchdog is reset every iteration so a long
 * timeout_override does not trigger a firmware reset. */
/* Hotkey result codes returned in *out_hotkey by boot_menu_run().
 * Negative values are sentinels (none / firmware-setup-pending);
 * non-negative values index into cand_idx[] like the return value. */
#define BOOT_MENU_HOTKEY_NONE       0  /* no hotkey side-effect */
#define BOOT_MENU_HOTKEY_SAFE_MODE  1  /* F8 pressed */
#define BOOT_MENU_HOTKEY_FW_SETUP   2  /* F10 -> firmware setup (handled in invoke) */

/* Per-menu-session F10 lockout: once retries to the firmware-setup
 * transition are exhausted, further F10 keypresses are logged and
 * ignored so the menu stays interactive instead of falling through
 * to a normal boot the operator did not request. Reset by every
 * boot_policy_invoke() entry. */
static int g_menu_f10_disabled;

/* Per-menu-session "no auto-boot" gate: when set, boot_menu_run()
 * disables the countdown expiry path so that the menu stays up
 * indefinitely and only returns when the operator presses Enter,
 * Esc, or another action key. Used after F10 retry exhaustion --
 * a normal boot after a failed firmware-setup request would be a
 * stranded boot the operator did not authorize. Reset on every
 * boot_policy_invoke() entry. */
static int g_menu_no_autoboot;

static unsigned int boot_menu_run(const boot_entries_parse_result_t *parse,
                                  const unsigned int *cand_idx,
                                  unsigned int cand_count,
                                  unsigned int default_idx,
                                  unsigned int timeout_s,
                                  const boot_policy_decision_t *decision,
                                  int sb_active,
                                  int allow_skip_when_alone,
                                  int *out_hotkey)
{
    boot_set_section(BOOT_SECTION_BL_MENU);
    post_code16(POST16_BL_MENU);

    if (out_hotkey) *out_hotkey = BOOT_MENU_HOTKEY_NONE;

    if (cand_count == 0u) return 0;
    if (cand_count == 1u && allow_skip_when_alone) {
        serial_early_print("[BOOT] menu: 1 candidate + hide_when_alone -- skipping\n");
        return 0;
    }

    /* Cap the countdown at BOOT_MENU_TIMEOUT_CAP_S (60s). We call
     * watchdog_reset() each tick, but on a SetWatchdogTimer refresh
     * failure watchdog_reset() leaves g_wd_armed set (the firmware timer
     * may still be live) and stops refreshing, so a countdown longer than
     * the ~60s firmware watchdog window could be reset by firmware mid-
     * dwell. The cap keeps the interactive countdown inside one watchdog
     * window; longer LoaderConfigTimeoutOneShot requests are clamped to
     * 60 with a [WARN] at the apply site. timeout_s == 0 falls back to the
     * default here (one-shot 0 takes the no-countdown path separately). */
    if (timeout_s == 0u || timeout_s > BOOT_MENU_TIMEOUT_CAP_S)
        timeout_s = BOOT_MENU_DEFAULT_TIMEOUT_S;

    int gop_available = (gFramebuffer != (UINT32 *)0);
    unsigned int selected = (default_idx < cand_count) ? default_idx : 0;
    unsigned int elapsed_ms = 0;
    unsigned int countdown_ms = timeout_s * 1000u;
    int counting_down = g_menu_no_autoboot ? 0 : 1;
    if (g_menu_no_autoboot)
        serial_early_print("[BOOT] menu: countdown disabled "
                           "(awaiting explicit Enter/Esc)\n");

    /* Flush stale keystrokes from firmware menus / boot-time keypress
     * buffer. Mirrors the boot_fatal_dwell pattern. */
    if (gST && gST->ConIn && gST->ConIn->Reset)
        gST->ConIn->Reset(gST->ConIn, 0);

    /* Initial render. */
    boot_menu_render(parse, cand_idx, cand_count, selected,
                     timeout_s, gop_available, decision, sb_active);
    int dirty = 0;
    unsigned int last_seconds_logged = timeout_s;
    /* Throttle watchdog refresh: the firmware WD_TIMEOUT in efi_main
     * is 60s, so 50ms-tick refreshes (20Hz, 1200 SetWatchdogTimer
     * calls per minute) are wasteful. Refresh once per 10s of menu
     * dwell instead -- well under the 60s budget but cheap on
     * firmware-service churn. The initial reset before the loop
     * keeps the boot-side watchdog window from trailing in. */
    watchdog_reset();
    unsigned int last_wd_reset_ms = 0;

    while (counting_down ? (elapsed_ms < countdown_ms) : 1) {
        if (elapsed_ms - last_wd_reset_ms >= 10000u) {
            watchdog_reset();
            last_wd_reset_ms = elapsed_ms;
        }

        /* Poll for a keystroke. */
        if (gST && gST->ConIn && gST->ConIn->ReadKeyStroke) {
            EFI_INPUT_KEY key;
            EFI_STATUS rs = gST->ConIn->ReadKeyStroke(gST->ConIn, &key);
            if (!EFI_ERROR(rs)) {
                /* Only ACTIONABLE keys cancel the countdown. Stray
                 * non-action keys (printable chars, modifier ghost
                 * events, firmware console noise after reset) are
                 * IGNORED so they cannot strand the boot in an
                 * indefinite menu dwell -- watchdog_reset() runs
                 * every tick, so a non-action keypress would
                 * otherwise sit forever. Codex re-adversarial
                 * caught this regression in the menu fix loop. */
                int is_action = 0;
                if (key.ScanCode == EFI_SCAN_UP) {
                    if (selected > 0) selected--;
                    dirty = 1;
                    is_action = 1;
                } else if (key.ScanCode == EFI_SCAN_DOWN) {
                    if (selected + 1u < cand_count) selected++;
                    dirty = 1;
                    is_action = 1;
                } else if (key.ScanCode == EFI_SCAN_HOME) {
                    selected = 0;
                    dirty = 1;
                    is_action = 1;
                } else if (key.ScanCode == EFI_SCAN_END) {
                    selected = cand_count - 1u;
                    dirty = 1;
                    is_action = 1;
                } else if (key.ScanCode == EFI_SCAN_ESC) {
                    /* Esc boots the highlighted entry without further
                     * interaction (per the menu spec). */
                    serial_early_print("[BOOT] menu: Esc -- boot highlighted\n");
                    return selected;
                } else if (key.UnicodeChar == EFI_CHAR_CR
                           || key.UnicodeChar == EFI_CHAR_LF) {
                    serial_early_print("[BOOT] menu: Enter -- boot selected\n");
                    return selected;
                } else if (key.ScanCode == EFI_SCAN_F8) {
                    /* F8 = safe-mode override hotkey. The caller
                     * (boot_policy_invoke) sets boot_config.boot_mode
                     * = 1 in response to BOOT_MENU_HOTKEY_SAFE_MODE
                     * so kernel-side observers (KUSD SafeBootMode,
                     * bare-metal hardening guards) see the operator
                     * intent. */
                    serial_early_print("[BOOT] menu: F8 -- safe-mode requested (boot_config update in caller)\n");
                    if (out_hotkey) *out_hotkey = BOOT_MENU_HOTKEY_SAFE_MODE;
                    return selected;
                } else if (key.ScanCode == EFI_SCAN_F10) {
                    /* F10 = firmware setup. The actual reset-into-
                     * setup transition is owned by the caller in
                     * boot_policy_invoke() because it touches NV
                     * UEFI variables (OsIndications) and gRT->
                     * ResetSystem. Signal via out_hotkey so the
                     * caller can do the gated setvar+reset. */
                    if (g_menu_f10_disabled) {
                        /* Prior F10 attempt exhausted retries
                         * (firmware reports BOOT_TO_FW_UI
                         * unsupported or SetVariable persistently
                         * failed). Stay in menu; require explicit
                         * Enter/Esc instead of triggering more
                         * SetVariable churn. */
                        serial_early_print("[BOOT] menu: F10 ignored "
                                           "(retries exhausted this session)\n");
                        is_action = 1;
                        dirty = 1;
                    } else {
                        serial_early_print("[BOOT] menu: F10 -- firmware setup requested\n");
                        if (out_hotkey) *out_hotkey = BOOT_MENU_HOTKEY_FW_SETUP;
                        return selected;
                    }
                }
                /* F11 is handled BEFORE this loop is entered (pre-
                 * menu probe in boot_policy_invoke); inside the loop
                 * the menu is already showing, so F11 is a no-op. */
                if (is_action && counting_down) {
                    counting_down = 0;
                    serial_early_print("[BOOT] menu: countdown cancelled by keypress\n");
                }
            }
        }

        if (gBS && gBS->Stall) gBS->Stall(50000);  /* 50 ms tick */
        elapsed_ms += 50u;

        if (counting_down) {
            unsigned int seconds_left = (countdown_ms > elapsed_ms)
                ? (countdown_ms - elapsed_ms) / 1000u
                : 0u;
            if (seconds_left != last_seconds_logged) {
                last_seconds_logged = seconds_left;
                dirty = 1;
            }
        }

        if (dirty) {
            unsigned int seconds_left = counting_down
                ? ((countdown_ms > elapsed_ms) ? (countdown_ms - elapsed_ms) / 1000u : 0u)
                : 0u;
            boot_menu_render(parse, cand_idx, cand_count, selected,
                             seconds_left, gop_available, decision, sb_active);
            dirty = 0;
        }
    }

    serial_early_print("[BOOT] menu: countdown expired -- auto-selecting default\n");
    return selected;
}

/* Pre-menu F11 probe: brief 100ms ConIn poll for F11 so the operator
 * can force the menu to show even when the ladder picked a forced-
 * selection path (HOTKEY, WATCHDOG, AB_TRY_STATE, RECOVERY_REQUEST,
 * FALLBACK_*). Returns 1 if F11 was buffered, 0 otherwise. Drains
 * any other pending keys (they would just race the menu loop's
 * own ReadKeyStroke). */
/* Drain cap per probe slot. A held key generating repeats at typical
 * USB/PS2 rates produces tens of events per slot; 32 covers that
 * comfortably while bounding the worst case if firmware misbehaves
 * (returns EFI_SUCCESS forever). */
#define BOOT_F11_PROBE_DRAIN_CAP  32u

static int boot_menu_probe_f11(void)
{
    if (!gST || !gBS || !gBS->Stall) return 0;

    /* Prefer Simple Text Input Ex per UEFI 2.10 spec 12.2 + Appendix
     * B Table B-1: F11 (scan 0x15) is in the Ex table only, not the
     * plain Simple Text Input table. Plain ConIn->ReadKeyStroke is
     * only required to deliver scan codes 0x00..0x14, so F11 may not
     * be reported there on conforming firmware. Try Ex first via
     * gBS->LocateProtocol; fall back to ConIn for permissive firmware
     * that aggregates Ex scan codes into the plain protocol (most
     * desktop UEFI does, but the spec does not require it). */
    EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *cin_ex = (EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *)0;
    EFI_GUID ex_guid = EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL_GUID;
    if (gBS && gBS->LocateProtocol)
        (void)gBS->LocateProtocol(&ex_guid, (VOID *)0, (VOID **)&cin_ex);

    int seen = 0;
    for (unsigned int slot = 0; slot < 2u; slot++) {
        unsigned int drained = 0;
        if (cin_ex && cin_ex->ReadKeyStrokeEx) {
            EFI_KEY_DATA kd;
            EFI_STATUS rs;
            while (drained < BOOT_F11_PROBE_DRAIN_CAP
                   && !EFI_ERROR((rs = cin_ex->ReadKeyStrokeEx(cin_ex, &kd)))) {
                if (kd.Key.ScanCode == EFI_SCAN_F11) seen = 1;
                drained++;
            }
        } else if (gST->ConIn && gST->ConIn->ReadKeyStroke) {
            EFI_INPUT_KEY key;
            EFI_STATUS rs;
            while (drained < BOOT_F11_PROBE_DRAIN_CAP
                   && !EFI_ERROR((rs = gST->ConIn->ReadKeyStroke(gST->ConIn, &key)))) {
                if (key.ScanCode == EFI_SCAN_F11) seen = 1;
                drained++;
            }
        }
        gBS->Stall(50000);
    }
    return seen;
}

/* F10 firmware-setup transition. Reads OsIndicationsSupported; if
 * BOOT_TO_FW_UI bit is supported, OR-in only that bit on
 * OsIndications and ResetSystem. Capsule trigger bits stay banned.
 * Returns 0 on unsupported / failure (caller stays in menu and
 * renders an unsupported message); does not return on success
 * (ResetSystem is supposed to terminate). */
#define BOOT_OS_INDICATIONS_BOOT_TO_FW_UI  (1ull << 0)
static int boot_menu_enter_fw_setup(void)
{
    if (!gST || !gST->RuntimeServices)
        return 0;
    EFI_RUNTIME_SERVICES *rt = gST->RuntimeServices;
    if (!rt->GetVariable || !rt->SetVariable || !rt->ResetSystem)
        return 0;

    EFI_GUID global_guid = EFI_GLOBAL_VARIABLE_GUID;
    UINT64 supported = 0;
    UINTN sz = sizeof(supported);
    UINT32 attrs = 0;
    EFI_STATUS st = rt->GetVariable(u"OsIndicationsSupported",
                                    &global_guid, &attrs, &sz, &supported);
    if (EFI_ERROR(st) || sz != sizeof(supported)) {
        serial_early_print("[BOOT] menu: F10 -- OsIndicationsSupported absent (firmware unsupported)\n");
        return 0;
    }
    if ((supported & BOOT_OS_INDICATIONS_BOOT_TO_FW_UI) == 0) {
        serial_early_print("[BOOT] menu: F10 -- BOOT_TO_FW_UI not in OsIndicationsSupported\n");
        return 0;
    }

    UINT64 indications = 0;
    sz = sizeof(indications);
    attrs = 0;
    st = rt->GetVariable(u"OsIndications", &global_guid, &attrs, &sz, &indications);
    if (EFI_ERROR(st)) {
        /* EFI_NOT_FOUND -> current value is 0; that is normal on a
         * clean system. Any other error is a real read failure that
         * should leave the operator in the menu rather than gambling
         * on a write. */
        if (st != EFI_NOT_FOUND) {
            serial_early_print("[BOOT] menu: F10 -- OsIndications read failed\n");
            return 0;
        }
        indications = 0;
        attrs = 0x07; /* NV | BS | RT */
    } else if (sz != sizeof(indications)) {
        serial_early_print("[BOOT] menu: F10 -- OsIndications has unexpected size\n");
        return 0;
    }

    /* OR in ONLY the firmware-setup bit. Any capsule-trigger bits
     * already in the variable stay (we are not authoring them, just
     * preserving the firmware's state). */
    indications |= BOOT_OS_INDICATIONS_BOOT_TO_FW_UI;

    /* Always rewrite with the spec-mandated NV | BS | RT attrs. */
    st = rt->SetVariable(u"OsIndications", &global_guid,
                         (UINT32)0x07, sizeof(indications), &indications);
    if (EFI_ERROR(st)) {
        serial_early_print("[BOOT] menu: F10 -- SetVariable failed; staying in menu\n");
        return 0;
    }

    serial_early_print("[BOOT] menu: F10 -- OsIndications written; resetting into firmware setup\n");
    rt->ResetSystem(EFI_RESET_COLD, 0, 0, (CHAR16 *)0);
    /* UEFI 2.10 spec contract: ResetSystem MUST NOT return. If it
     * does, the firmware is misbehaving AND OsIndications is now
     * persisted with BOOT_TO_FW_UI set -- on the next reboot the
     * firmware would unexpectedly enter setup. Halt rather than
     * fall through to a normal boot, which would proceed with the
     * pending firmware-setup indication still set. */
    boot_fatal(BOOT_ERR_FW_SETUP_RESET_RET,
               "F10 firmware setup transition failed",
               "ResetSystem returned (UEFI 2.10 spec violation; "
               "OsIndications was already written so a normal boot "
               "is unsafe -- next reboot would unexpectedly enter "
               "firmware setup).");
    return 1;
}

/* ----- per-entry health-gate handoff helpers -------------------------- */

#include "../../../include/boot/boot_health_handoff.h"

/* Variable names + attribute sets for the cross-boot health handoff.
 * Names mirror BOOT_HEALTH_VAR_*_ASCII in boot_health_handoff.h. */
static const CHAR16 g_health_mark_good_var[]    = u"ImpossibleOS-MarkGood";
static const CHAR16 g_health_cur_boot_ctr_var[] = u"ImpossibleOS-CurBootCtr";
static const CHAR16 g_health_subset_var[]       = u"ImpossibleOS-HealthSubset";

/* Read the MarkGood UEFI variable (NV+BS+RT) into a kernel-validated
 * record. Returns 1 if a usable record was found, 0 on absent / wrong
 * size / wrong attrs / invalid (magic / version / CRC / id grammar).
 *
 * Attribute binding: a pre-OS tool (ESP UEFI shell or separate boot
 * tool) could preseed a CRC-valid record with non-canonical attrs to
 * steer the gate. Enforce attrs == NV|BS|RT exactly so the only
 * legitimate producer is the kernel's mark_entry_successful path,
 * which always writes UEFI_VAR_NV_BOOT_RUNTIME. */
static int health_read_mark_good(struct boot_health_mark_good_record *out)
{
    if (!gST || !gST->RuntimeServices || !gST->RuntimeServices->GetVariable)
        return 0;
    if (!out) return 0;
    for (unsigned int i = 0; i < sizeof(*out); i++)
        ((UINT8 *)out)[i] = 0;
    UINTN sz = sizeof(*out);
    UINT32 attrs = 0;
    EFI_STATUS s = gST->RuntimeServices->GetVariable(
        (CHAR16 *)g_health_mark_good_var, &g_impossible_os_guid,
        &attrs, &sz, (VOID *)out);
    if (EFI_ERROR(s)) return 0;
    if (sz != sizeof(*out)) return 0;
    {
        const UINT32 expected = EFI_VARIABLE_NON_VOLATILE
                              | EFI_VARIABLE_BOOTSERVICE_ACCESS
                              | EFI_VARIABLE_RUNTIME_ACCESS;
        if (attrs != expected) {
            serial_early_print("[BOOT] policy: MarkGood attrs mismatch -- ignoring\n");
            return 0;
        }
    }
    if (!boot_health_mark_good_is_valid(out)) return 0;
    return 1;
}

/* Delete the MarkGood variable. Best-effort; failure is logged WARN. */
static void health_clear_mark_good(void)
{
    if (!gST || !gST->RuntimeServices || !gST->RuntimeServices->SetVariable)
        return;
    UINT32 attrs = EFI_VARIABLE_NON_VOLATILE
                 | EFI_VARIABLE_BOOTSERVICE_ACCESS
                 | EFI_VARIABLE_RUNTIME_ACCESS;
    EFI_STATUS s = gST->RuntimeServices->SetVariable(
        (CHAR16 *)g_health_mark_good_var, &g_impossible_os_guid,
        attrs, 0, (VOID *)0);
    if (EFI_ERROR(s)) {
        serial_early_print("[BOOT] policy: MarkGood clear failed\n");
    }
}

/* Consume an outstanding MarkGood record at the start of boot_policy_
 * invoke. State-bound authorization: the exact {entry_id, tries_left,
 * tries_done} triple file must exist for the consume to fire. Once
 * authorized, ALL counter files matching entry_id are deleted -- the
 * crash-tolerant counter protocol intentionally leaves duplicate
 * <id>+L-D files for the same entry id after a torn rename, dedupes
 * them at scan time with worst-case semantics, and only the dedupe
 * winner gets the per-boot decrement. If MarkGood deleted only the
 * exact bound filename, an orphan duplicate (e.g. <id>+2-1) would
 * survive the consume and re-tracking would resume next boot. The
 * sweep keeps the bound triple as the authorization gate but
 * guarantees the entry is fully returned to the "no counter"
 * good state. */
static void policy_consume_mark_good_var(void)
{
    struct boot_health_mark_good_record rec;
    if (!health_read_mark_good(&rec)) {
        /* No record (absent or invalid) -- nothing to do. We still
         * clear the variable on invalid so a corrupt record cannot
         * persist indefinitely. */
        return;
    }

    /* Build the authorization filename for the bound state. */
    boot_counter_t bound;
    for (unsigned int j = 0; j < sizeof(bound.id); j++) bound.id[j] = 0;
    unsigned int idlen = 0;
    while (rec.entry_id[idlen] != 0 && idlen < BOOT_ENTRIES_MAX_ID_LEN) {
        bound.id[idlen] = rec.entry_id[idlen];
        idlen++;
    }
    if (idlen == 0u) {
        health_clear_mark_good();
        return;
    }
    bound.tries_left = rec.tries_left;
    bound.tries_done = rec.tries_done;
    char bound_name[BOOT_COUNTER_FILENAME_MAX];
    int bound_len = boot_counter_format_filename(&bound, bound_name,
                                                  sizeof(bound_name));
    if (bound_len == 0) {
        serial_early_print("[BOOT] policy: MarkGood id format failed -- clearing var\n");
        health_clear_mark_good();
        return;
    }

    EFI_FILE_PROTOCOL *root = (EFI_FILE_PROTOCOL *)0;
    if (EFI_ERROR(counters_open_volume_root(&root)) || !root) {
        health_clear_mark_good();
        return;
    }
    EFI_FILE_PROTOCOL *dir = (EFI_FILE_PROTOCOL *)0;
    EFI_STATUS ds = root->Open(root, &dir, (CHAR16 *)g_counter_dir_path,
                                EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE,
                                EFI_FILE_DIRECTORY);
    if (EFI_ERROR(ds) || !dir) {
        root->Close(root);
        health_clear_mark_good();
        return;
    }

    /* Step 1: state-bound authorization. The exact bound filename
     * MUST exist for the sweep to fire -- otherwise this is a stale /
     * replayed record (or the counter was already cleaned up). */
    CHAR16 bound_w[BOOT_COUNTER_FILENAME_MAX];
    {
        UINTN j;
        for (j = 0; j < sizeof(bound_w)/sizeof(bound_w[0]) - 1u; j++) {
            char c = bound_name[j];
            if (c == 0) break;
            bound_w[j] = (CHAR16)(unsigned char)c;
        }
        bound_w[j] = 0;
    }

    EFI_FILE_PROTOCOL *probe = (EFI_FILE_PROTOCOL *)0;
    EFI_STATUS ps = dir->Open(dir, &probe, bound_w,
                               EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(ps) || !probe) {
        /* Authorization failed -- stale / replayed. Clear and move on. */
        serial_early_print("[BOOT] policy: MarkGood stale (no counter ");
        serial_early_print(bound_name);
        serial_early_print(") -- clearing var\n");
        dir->Close(dir);
        root->Close(root);
        health_clear_mark_good();
        return;
    }
    probe->Close(probe);

    /* Step 2: authorized sweep. Walk the counter directory; for every
     * file whose parsed id matches rec.entry_id, delete it. Same
     * walker shape as policy_scan_counters but the action is delete
     * instead of dedup-insert. The boot_counter_parse_filename grammar
     * rejects non-counter files (skip-with-noop), so non-counter
     * files in the directory are left alone. Note: the dir's Read
     * position is already at 0 (we have not Read this handle yet --
     * the Step 1 probe opened a separate file handle), so no
     * SetPosition is needed. */
    UINT8 info_buf[1024];
    unsigned int deleted = 0;
    int sweep_complete = 0;
    for (;;) {
        UINTN sz = sizeof(info_buf);
        EFI_STATUS s = dir->Read(dir, &sz, info_buf);
        if (EFI_ERROR(s)) {
            /* Partial directory view -- duplicate counter files for
             * this entry_id may survive. Do NOT clear MarkGood: leave
             * the authorization in NV so the next boot retries. The
             * counter that survives keeps the entry tracked, which is
             * the conservative outcome. */
            serial_early_print("[BOOT] policy: MarkGood sweep Read failed -- keeping MarkGood for retry\n");
            dir->Close(dir);
            root->Close(root);
            return;
        }
        if (sz == 0) { sweep_complete = 1; break; }  /* clean EOF */
        EFI_FILE_INFO *fi = (EFI_FILE_INFO *)info_buf;
        if (fi->Attribute & EFI_FILE_DIRECTORY) continue;
        char ascii_name[BOOT_COUNTER_FILENAME_MAX];
        UINTN i;
        for (i = 0; i < sizeof(ascii_name) - 1u; i++) {
            CHAR16 c = fi->FileName[i];
            if (c == 0) break;
            if (c > 0x7F) { i = 0; break; }
            ascii_name[i] = (char)c;
        }
        ascii_name[i] = 0;
        if (i == 0) continue;
        boot_counter_t parsed;
        if (!boot_counter_parse_filename(ascii_name, (unsigned int)i, &parsed))
            continue;
        /* Match the bound entry_id exactly (NUL-terminated, ASCII
         * already validated by parse_filename). */
        int eq = 1;
        for (unsigned int j = 0; j < sizeof(parsed.id); j++) {
            if (parsed.id[j] != bound.id[j]) { eq = 0; break; }
            if (parsed.id[j] == 0) break;
        }
        if (!eq) continue;

        /* Open + Delete this counter file. */
        CHAR16 w[BOOT_COUNTER_FILENAME_MAX];
        UINTN j;
        for (j = 0; j < sizeof(w)/sizeof(w[0]) - 1u; j++) {
            char c = ascii_name[j];
            if (c == 0) break;
            w[j] = (CHAR16)(unsigned char)c;
        }
        w[j] = 0;
        EFI_FILE_PROTOCOL *fh = (EFI_FILE_PROTOCOL *)0;
        EFI_STATUS os = dir->Open(dir, &fh, w,
                                   EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
        if (EFI_ERROR(os) || !fh) continue;
        EFI_STATUS xs = fh->Delete(fh);
        if (!EFI_ERROR(xs)) deleted++;
    }

    /* Sweep reached clean EOF: every same-id counter we observed has
     * been opened + Deleted (or skipped on Open failure -- those are
     * surviving counters the next boot's scan still sees, which means
     * MarkGood acts as a strict guarantee against the bound state but
     * a best-effort guarantee against transient firmware errors on
     * sibling counters. The next boot's scan would then re-decrement
     * any survivor and a future health pass would retry). Clearing
     * MarkGood is safe because the authorization triple is now
     * spent. */
    (void)sweep_complete;
    serial_early_print("[BOOT] policy: MarkGood consumed (id=");
    serial_early_print(bound.id);
    serial_early_print(", deleted ");
    serial_early_print_uint((UINT32)deleted);
    serial_early_print(" counter file(s))\n");

    dir->Close(dir);
    root->Close(root);
    health_clear_mark_good();
}

/* Write the CurBootCtr UEFI variable (BS+RT, no NV) with the post-
 * decrement counter state for the selected entry. Kernel reads it to
 * compose the state-bound MarkGood record on health pass. Failure is
 * a LOG_WARN -- the kernel falls back to "no mark-good available". */
static void policy_write_cur_boot_ctr(const char *entry_id,
                                       unsigned int tries_left,
                                       unsigned int tries_done)
{
    if (!gST || !gST->RuntimeServices || !gST->RuntimeServices->SetVariable)
        return;
    if (!entry_id || entry_id[0] == 0) return;
    struct boot_health_cur_boot_ctr_record rec;
    for (unsigned int i = 0; i < sizeof(rec); i++) ((UINT8 *)&rec)[i] = 0;
    rec.magic   = BOOT_HEALTH_CUR_BOOT_CTR_MAGIC;
    rec.version = BOOT_HEALTH_VAR_VERSION;
    {
        unsigned int idl = 0;
        while (entry_id[idl] != 0 && idl < BOOT_HEALTH_HANDOFF_ID_LEN - 1u) {
            rec.entry_id[idl] = entry_id[idl];
            idl++;
        }
        rec.entry_id[idl] = 0;
    }
    rec.tries_left = tries_left;
    rec.tries_done = tries_done;
    rec.reserved   = 0u;
    rec.crc32      = boot_health_handoff_compute_crc(&rec,
                        (unsigned int)sizeof(rec));
    UINT32 attrs = EFI_VARIABLE_BOOTSERVICE_ACCESS
                 | EFI_VARIABLE_RUNTIME_ACCESS;
    EFI_STATUS s = gST->RuntimeServices->SetVariable(
        (CHAR16 *)g_health_cur_boot_ctr_var, &g_impossible_os_guid,
        attrs, sizeof(rec), (VOID *)&rec);
    if (EFI_ERROR(s)) {
        serial_early_print("[BOOT] policy: CurBootCtr SetVariable failed\n");
    }
}

/* Write the HealthSubset UEFI variable (BS+RT) iff the selected entry
 * envelope carries a non-empty health_check_subset. Absent / empty
 * subset -> kernel runs the full default check set. */
static void policy_write_health_subset(const boot_entry_envelope_t *env)
{
    if (!gST || !gST->RuntimeServices || !gST->RuntimeServices->SetVariable)
        return;
    if (!env || env->health_check_subset_count == 0u) return;
    struct boot_health_subset_record rec;
    for (unsigned int i = 0; i < sizeof(rec); i++) ((UINT8 *)&rec)[i] = 0;
    rec.magic   = BOOT_HEALTH_SUBSET_MAGIC;
    rec.version = BOOT_HEALTH_VAR_VERSION;
    rec.count   = env->health_check_subset_count;
    rec.reserved = 0u;
    unsigned int cap = BOOT_HEALTH_SUBSET_MAX_NAMES;
    if (rec.count > cap) rec.count = cap;
    for (unsigned int i = 0; i < rec.count; i++) {
        unsigned int j;
        for (j = 0; j < BOOT_HEALTH_SUBSET_NAME_LEN - 1u; j++) {
            char c = env->health_check_subset[i][j];
            if (c == 0) break;
            rec.names[i][j] = c;
        }
        rec.names[i][j] = 0;
    }
    rec.crc32 = boot_health_handoff_compute_crc(&rec,
                  (unsigned int)sizeof(rec));
    UINT32 attrs = EFI_VARIABLE_BOOTSERVICE_ACCESS
                 | EFI_VARIABLE_RUNTIME_ACCESS;
    EFI_STATUS s = gST->RuntimeServices->SetVariable(
        (CHAR16 *)g_health_subset_var, &g_impossible_os_guid,
        attrs, sizeof(rec), (VOID *)&rec);
    if (EFI_ERROR(s)) {
        serial_early_print("[BOOT] policy: HealthSubset SetVariable failed\n");
    }
}

/* Forward declarations for helpers defined later in the file but
 * called from the boot-policy invoke path. */
static int smbios_extract_system_uuid(char out[37]);
static int smbios_format_uuid(const UINT8 src[16], char out[37]);

/* ----- systemd-boot Boot Loader Interface (LoaderXxx vars) -----------
 *
 * systemd-boot defines a set of UEFI variables under vendor GUID
 * 4a67b082-0a4c-41cf-b6c7-440b29bb8c4f that Linux user-mode tools
 * (bootctl, systemctl reboot --boot-loader-entry) read to query boot
 * loader state and write to override the next boot. Impossible OS
 * publishes the same surface so those tools work unmodified.
 *
 * Read-only (bootloader -> OS, NV+BS+RT):
 *   LoaderInfo               -- "ImpossibleOS bootloader vX"
 *   LoaderFirmwareInfo       -- gST->FirmwareVendor + revision
 *   LoaderFirmwareType       -- "UEFI <major>.<minor>"
 *   LoaderImageIdentifier    -- canonical loader path on ESP
 *   LoaderDevicePartUUID     -- ESP GPT partition GUID (lowercase 36-char)
 *   LoaderEntries            -- NUL-separated UCS-2 entry ids, double-NUL term
 *   LoaderEntryDefault       -- default entry id (lowest sort_key)
 *   LoaderEntrySelected      -- this boot's selected entry id
 *   LoaderConfigTimeout      -- decimal seconds string
 *   LoaderTimeInitUSec       -- TSC-derived boot loader start in usec
 *   LoaderTimeExecUSec       -- TSC-derived handoff usec
 *   LoaderFeatures           -- 64-bit LE bitmap (canonical bit positions)
 *
 * One-shot (OS -> bootloader, NV+BS+RT, deleted after consumption):
 *   LoaderEntryOneShot       -- next boot's entry id
 *   LoaderConfigTimeoutOneShot -- next boot's timeout
 *
 * LoaderFeatures bit positions follow the upstream
 * https://systemd.io/BOOT_LOADER_INTERFACE/ spec verbatim. Advertising
 * a bit means we implement the documented semantic; not advertising
 * means the capability is absent. Impossible OS-specific extensions
 * (entry kinds, audit JSONL, health gate) live in a SEPARATE variable
 * `ImpossibleOSLoaderFeaturesExt` under our own vendor GUID so we
 * never lie about systemd-boot capability bits.
 *
 * Failure model: every gRT->SetVariable() call is wrapped in
 * loader_set_var() which sets boot_info.loader_vars_degraded on any
 * error and continues. Userland still boots; the kernel surfaces
 * degraded publication state in the audit JSONL. */

/* systemd-boot vendor GUID (Linux compat -- DO NOT change). */
static const EFI_GUID g_loader_systemd_guid =
    { 0x4a67b082, 0x0a4c, 0x41cf,
      { 0xb6, 0xc7, 0x44, 0x0b, 0x29, 0xbb, 0x8c, 0x4f } };

/* LoaderFeatures bits per upstream Boot Loader Interface spec. Only
 * advertise bits the bootloader actually implements; advertising a bit
 * we do not honor is a compatibility lie. */
#define LOADER_FEATURE_CONFIG_TIMEOUT          (1ULL << 0)  /* writable LoaderConfigTimeout -- NOT advertised */
#define LOADER_FEATURE_CONFIG_TIMEOUT_ONESHOT  (1ULL << 1)
#define LOADER_FEATURE_ENTRY_DEFAULT           (1ULL << 2)  /* writable LoaderEntryDefault -- NOT advertised */
#define LOADER_FEATURE_ENTRY_ONESHOT           (1ULL << 3)
#define LOADER_FEATURE_BOOT_COUNTING           (1ULL << 4)
#define LOADER_FEATURE_XBOOTLDR                (1ULL << 5)  /* XBOOTLDR partition -- NOT advertised */
/* We advertise: ConfigTimeoutOneShot + EntryOneShot + BootCounting */
#define LOADER_FEATURES_PUBLISHED \
    (LOADER_FEATURE_CONFIG_TIMEOUT_ONESHOT \
     | LOADER_FEATURE_ENTRY_ONESHOT \
     | LOADER_FEATURE_BOOT_COUNTING)

/* Canonical loader install path on the ESP (Impossible OS UEFI
 * bootloader). systemd-boot publishes the actual loaded path via
 * device-path walk; we publish the canonical install location since
 * it is what operators expect to see and what bootctl displays. */
#define LOADER_IMAGE_IDENTIFIER_PATH u"\\EFI\\ImpossibleOS\\BOOTX64.EFI"

/* Bound on any LoaderXxx variable payload. systemd-boot caps at ~4 KiB
 * per var; our LoaderEntries is the largest payload (up to 64 ids * 48
 * chars * 2 bytes UCS-2 + terminators = ~6 KiB). Allocate 8 KiB on the
 * stack-frame inside the publisher. */
#define LOADER_VAR_PAYLOAD_MAX  8192u

/* Convert an ASCII NUL-terminated string to UCS-2 (UTF-16LE) into the
 * caller buffer; returns the byte count including the trailing
 * UCS-2 NUL (2 bytes). Caller-buffer must hold at least
 * (strlen+1)*2 bytes. */
static UINTN loader_ascii_to_ucs2(const char *src, CHAR16 *dst, UINTN dst_cap)
{
    UINTN i = 0;
    if (!src || !dst || dst_cap < 2) return 0;
    while (src[i] && i < (dst_cap / 2u) - 1u) {
        dst[i] = (CHAR16)(unsigned char)src[i];
        i++;
    }
    dst[i] = 0;
    return (i + 1u) * 2u;
}

/* Convert UINT64 decimal to ASCII into out[buf_cap]. Returns the
 * length (excluding NUL). Always NUL-terminates. */
static UINTN loader_u64_to_decimal(UINT64 v, char *out, UINTN cap)
{
    char tmp[24];
    UINTN tlen = 0;
    if (!out || cap == 0) return 0;
    if (v == 0) tmp[tlen++] = '0';
    while (v) {
        tmp[tlen++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    UINTN out_pos = 0;
    while (tlen > 0 && out_pos < cap - 1u) {
        out[out_pos++] = tmp[--tlen];
    }
    out[out_pos] = 0;
    return out_pos;
}

/* Write a UEFI variable with NV+BS+RT attrs. On failure, log + set
 * loader_vars_degraded=1 (best-effort). Never halts. */
static void loader_set_var(const CHAR16 *name, UINTN payload_size,
                            const void *payload)
{
    if (!gST || !gST->RuntimeServices || !gST->RuntimeServices->SetVariable) {
        g_boot_info_ptr->loader_vars_degraded = 1;
        return;
    }
    UINT32 attrs = EFI_VARIABLE_NON_VOLATILE
                 | EFI_VARIABLE_BOOTSERVICE_ACCESS
                 | EFI_VARIABLE_RUNTIME_ACCESS;
    EFI_STATUS s = gST->RuntimeServices->SetVariable(
        (CHAR16 *)name, (EFI_GUID *)&g_loader_systemd_guid,
        attrs, payload_size, (VOID *)payload);
    if (EFI_ERROR(s)) {
        g_boot_info_ptr->loader_vars_degraded = 1;
        serial_early_print("[BOOT] loader-vars: SetVariable failed -- degraded mode\n");
    }
}

/* Delete a Loader* variable so a stale NON_VOLATILE value from a prior boot does
 * not linger for systemd-BLI consumers when there is nothing to publish.
 * EFI_NOT_FOUND (already absent) is success, not a degradation. */
static void loader_clear_var(const CHAR16 *name)
{
    if (!gST || !gST->RuntimeServices || !gST->RuntimeServices->SetVariable)
        return;
    UINT32 attrs = EFI_VARIABLE_NON_VOLATILE
                 | EFI_VARIABLE_BOOTSERVICE_ACCESS
                 | EFI_VARIABLE_RUNTIME_ACCESS;
    EFI_STATUS s = gST->RuntimeServices->SetVariable(
        (CHAR16 *)name, (EFI_GUID *)&g_loader_systemd_guid, attrs, 0, (VOID *)0);
    if (EFI_ERROR(s) && s != EFI_NOT_FOUND) {
        g_boot_info_ptr->loader_vars_degraded = 1;
        serial_early_print("[BOOT] loader-vars: clear failed -- degraded mode\n");
    }
}

/* Write a UEFI variable from an ASCII string. */
static void loader_set_var_ascii(const CHAR16 *name, const char *value)
{
    CHAR16 buf[512];
    UINTN n = loader_ascii_to_ucs2(value, buf, sizeof(buf));
    if (n == 0) return;
    /* systemd-boot interface convention: string vars are UCS-2 WITHOUT
     * the trailing NUL counted (so userspace tools treat the bytes as
     * the exact string). We follow the same convention: size = n - 2. */
    if (n < 2) return;
    loader_set_var(name, n - 2u, buf);
}

/* Format the GPT partition GUID at boot_info.boot_partition_guid into
 * RFC 4122 textual form (lowercase) and write LoaderDevicePartUUID.
 * GPT GUIDs use the same little-endian-on-wire convention as SMBIOS. */
static void loader_set_var_part_uuid(void)
{
    const UINT8 *raw = (const UINT8 *)g_boot_info_ptr->boot_partition_guid;
    /* Skip if we never populated a real GPT GUID (zero or MBR fallback
     * in [0..3] only). systemd-boot spec: var absent => unknown. */
    int all_zero = 1;
    for (UINTN i = 0; i < 16; i++) if (raw[i]) { all_zero = 0; break; }
    if (all_zero) return;
    char ascii[37];
    if (!smbios_format_uuid(raw, ascii)) return;
    loader_set_var_ascii(u"LoaderDevicePartUUID", ascii);
}

/* Format gST->FirmwareVendor + FirmwareRevision and write LoaderFirmware*. */
static void loader_set_firmware_info(void)
{
    CHAR16 buf[256];
    UINTN p = 0;
    if (gST && gST->FirmwareVendor) {
        const CHAR16 *fv = gST->FirmwareVendor;
        while (fv[p] != 0 && p < (sizeof(buf) / sizeof(buf[0])) - 16u) {
            buf[p] = fv[p];
            p++;
        }
    }
    if (gST) {
        /* Append " " + revision hex */
        const char *suffix = " ";
        UINTN si = 0;
        while (suffix[si] && p < (sizeof(buf) / sizeof(buf[0])) - 12u) {
            buf[p++] = (CHAR16)(unsigned char)suffix[si++];
        }
        UINT32 rev = gST->FirmwareRevision;
        char dec[16];
        UINTN dl = loader_u64_to_decimal((UINT64)rev, dec, sizeof(dec));
        for (UINTN i = 0; i < dl && p < (sizeof(buf) / sizeof(buf[0])) - 1u; i++) {
            buf[p++] = (CHAR16)(unsigned char)dec[i];
        }
    }
    buf[p] = 0;
    if (p > 0)
        loader_set_var(u"LoaderFirmwareInfo", p * 2u, buf);

    /* Firmware type: "UEFI <major>.<minor>" from gST->Hdr.Revision. */
    if (gST) {
        char ft[32] = { 'U','E','F','I',' ', 0 };
        UINTN tp = 5;
        UINT32 ver = gST->Hdr.Revision;
        UINT32 maj = (ver >> 16) & 0xFFFF;
        UINT32 min = ver & 0xFFFF;
        char dbuf[8];
        UINTN dn = loader_u64_to_decimal((UINT64)maj, dbuf, sizeof(dbuf));
        for (UINTN i = 0; i < dn; i++) ft[tp++] = dbuf[i];
        ft[tp++] = '.';
        dn = loader_u64_to_decimal((UINT64)min, dbuf, sizeof(dbuf));
        for (UINTN i = 0; i < dn; i++) ft[tp++] = dbuf[i];
        ft[tp] = 0;
        loader_set_var_ascii(u"LoaderFirmwareType", ft);
    }
}

/* Lexical compare of two NUL-terminated ASCII strings (unsigned byte
 * order). Returns <0, 0, >0 like strcmp. */
/* BLS display-order comparator (boot_entry_bls_less) + stable sort
 * (boot_entries_bls_sort) live in boot_entries_parser.c -- pure C over the
 * envelope struct, shared with the kernel-side unit tests. Declared in
 * boot/boot_entries_parser.h. */

/* Compose + write LoaderEntries from the parser result, in BLS display
 * order (matching the menu). UCS-2, NUL-separated, trailing double-NUL
 * terminator. systemd-boot's bootctl reads this and lists each id. */
static void loader_set_entries(const boot_entries_parse_result_t *parse)
{
    if (!parse || parse->entry_count == 0) return;
    /* Build a BLS-sorted index permutation of all entries. */
    unsigned int order[BOOT_ENTRIES_MAX_ENTRIES];
    unsigned int n = (unsigned int)parse->entry_count;
    if (n > BOOT_ENTRIES_MAX_ENTRIES) n = BOOT_ENTRIES_MAX_ENTRIES;
    for (unsigned int i = 0; i < n; i++) order[i] = i;
    boot_entries_bls_sort(parse, order, n);
    /* Stack-allocate the payload buffer. */
    static CHAR16 buf[LOADER_VAR_PAYLOAD_MAX / 2u];
    UINTN p = 0;
    UINTN cap = sizeof(buf) / sizeof(buf[0]);
    for (unsigned int oi = 0; oi < n; oi++) {
        const boot_entry_envelope_t *e = &parse->entries[order[oi]];
        const char *id;
        UINTN j = 0;
        /* Skip menu-hidden entries (kind_skipped / HIDDEN) so LoaderEntries
         * mirrors the visible menu: a deferred-kind or store-provided hidden
         * entry must not leak to systemd-BLI consumers (Codex re-adversarial). */
        if (e->kind_skipped || (e->flags & BOOT_ENTRY_FLAG_HIDDEN))
            continue;
        id = e->id;
        while (id[j] && p < cap - 2u) {
            buf[p++] = (CHAR16)(unsigned char)id[j++];
        }
        if (p < cap - 1u) buf[p++] = 0;  /* NUL separator */
    }
    /* Trailing double-NUL: one was emitted by the loop for the last
     * entry; add the second. */
    if (p < cap) buf[p++] = 0;
    loader_set_var(u"LoaderEntries", p * 2u, buf);
}

/* Publish the read-only Loader* variables. Called after the policy
 * decision lands in boot_info and after the per-entry counter
 * decrement, BEFORE the kernel handoff. Failure is best-effort: each
 * SetVariable independently flips loader_vars_degraded but never
 * blocks boot. */
static void loader_publish_readonly_vars(
    const boot_entries_parse_result_t *parse,
    const boot_policy_decision_t *decision,
    UINT64 init_usec, UINT64 exec_usec, UINT32 timeout_seconds)
{
    /* LoaderInfo */
    loader_set_var_ascii(u"LoaderInfo", "Impossible OS bootloader 1.0");

    /* LoaderFirmwareInfo + LoaderFirmwareType */
    loader_set_firmware_info();

    /* LoaderImageIdentifier (canonical install path). UCS-2 literal. */
    {
        const CHAR16 *p = LOADER_IMAGE_IDENTIFIER_PATH;
        UINTN n = 0;
        while (p[n] != 0) n++;
        loader_set_var(u"LoaderImageIdentifier", n * 2u, p);
    }

    /* LoaderDevicePartUUID */
    loader_set_var_part_uuid();

    /* LoaderEntries (NUL-separated ids, double-NUL terminator). */
    loader_set_entries(parse);

    /* LoaderEntryDefault -- the entry with the lowest sort_key.
     * The policy ladder evaluates this at boot time; for the published
     * default we use the parsed entry whose sort_key sorts first. */
    if (parse && parse->entry_count > 0) {
        UINTN best = (UINTN)-1;
        for (UINTN i = 0; i < parse->entry_count; i++) {
            const boot_entry_envelope_t *e = &parse->entries[i];
            /* Never publish a menu-hidden entry as the default (Codex
             * re-adversarial): a kind_skipped/HIDDEN store entry must not be
             * advertised as LoaderEntryDefault to systemd-BLI consumers. */
            if (e->kind_skipped || (e->flags & BOOT_ENTRY_FLAG_HIDDEN))
                continue;
            if (best == (UINTN)-1) { best = i; continue; }
            {
                const char *a = parse->entries[best].sort_key;
                const char *b = e->sort_key;
                UINTN k = 0;
                while (a[k] == b[k] && a[k] != 0) k++;
                if ((unsigned char)b[k] < (unsigned char)a[k]) best = i;
            }
        }
        if (best != (UINTN)-1)
            loader_set_var_ascii(u"LoaderEntryDefault", parse->entries[best].id);
        else
            loader_clear_var(u"LoaderEntryDefault");  /* nothing visible -> drop stale */
    }

    /* LoaderEntrySelected -- this boot's selected id (already in
     * boot_info v19). Skipped on FALLBACK_STORE_INVALID where the
     * selected_entry_id is empty by ABI. */
    if (decision && decision->selected_entry_id[0] != 0)
        loader_set_var_ascii(u"LoaderEntrySelected", decision->selected_entry_id);

    /* LoaderConfigTimeout -- current effective timeout in seconds. */
    {
        char tbuf[16];
        loader_u64_to_decimal((UINT64)timeout_seconds, tbuf, sizeof(tbuf));
        loader_set_var_ascii(u"LoaderConfigTimeout", tbuf);
    }

    /* LoaderTimeInitUSec / LoaderTimeExecUSec -- TSC-derived
     * microseconds since bootloader entry / kernel handoff. */
    {
        char tbuf[24];
        loader_u64_to_decimal(init_usec, tbuf, sizeof(tbuf));
        loader_set_var_ascii(u"LoaderTimeInitUSec", tbuf);
        loader_u64_to_decimal(exec_usec, tbuf, sizeof(tbuf));
        loader_set_var_ascii(u"LoaderTimeExecUSec", tbuf);
    }

    /* LoaderFeatures -- canonical systemd-boot bit positions; only
     * the bits we honor are set. */
    {
        UINT64 features = LOADER_FEATURES_PUBLISHED;
        loader_set_var(u"LoaderFeatures", sizeof(features), &features);
    }
}

/* Read and consume LoaderEntryOneShot + LoaderConfigTimeoutOneShot.
 * Both are deleted after consumption so the next boot reverts to
 * the configured default. Out-params are written only when a valid
 * value was consumed. */
static void loader_consume_one_shot_vars(
    char *out_entry_id, UINTN entry_id_cap,
    int *out_have_entry,
    UINT32 *out_timeout_seconds,
    int *out_have_timeout)
{
    *out_have_entry = 0;
    *out_have_timeout = 0;
    if (!gST || !gST->RuntimeServices ||
        !gST->RuntimeServices->GetVariable ||
        !gST->RuntimeServices->SetVariable) {
        return;
    }
    UINT32 attrs = 0;
    UINTN sz;

    /* LoaderEntryOneShot: UCS-2 string of the desired entry id. The
     * delete path MUST fire on every observed-present case (success,
     * EFI_BUFFER_TOO_SMALL on an oversize value, exact-buffer-size
     * value), not just on the parse-success branch. A user-writable
     * malformed value that escapes the delete would sit in NVRAM
     * across boots and block clean recovery. */
    {
        CHAR16 buf[64];
        int present = 0;
        sz = sizeof(buf);
        EFI_STATUS s = gST->RuntimeServices->GetVariable(
            u"LoaderEntryOneShot", (EFI_GUID *)&g_loader_systemd_guid,
            &attrs, &sz, buf);
        if (!EFI_ERROR(s) && sz > 0 && sz < sizeof(buf)) {
            present = 1;
            UINTN i = 0;
            UINTN clen = sz / 2u;
            while (i < clen && buf[i] != 0 &&
                   i < entry_id_cap - 1u) {
                CHAR16 c = buf[i];
                if (c < 0x20 || c > 0x7E) break;
                out_entry_id[i] = (char)c;
                i++;
            }
            out_entry_id[i] = 0;
            if (i > 0) *out_have_entry = 1;
        } else if (s == EFI_BUFFER_TOO_SMALL || (!EFI_ERROR(s) && sz == sizeof(buf))) {
            /* Malformed (oversize) but present -- delete without
             * honoring so a hostile or stale value cannot persist
             * indefinitely. Buffer too small means the firmware
             * reported the variable exists but is larger than 64
             * CHAR16; sz == buf_size is the boundary case where the
             * value happens to fill the buffer with no NUL room. */
            present = 1;
            serial_early_print("[BOOT] loader-vars: LoaderEntryOneShot malformed/oversize -- deleting\n");
        }
        if (present) {
            (void)gST->RuntimeServices->SetVariable(
                u"LoaderEntryOneShot",
                (EFI_GUID *)&g_loader_systemd_guid,
                attrs, 0, (VOID *)0);
        }
    }

    /* LoaderConfigTimeoutOneShot: decimal UCS-2 seconds. Same
     * delete-on-malformed contract as LoaderEntryOneShot above. */
    {
        CHAR16 buf[16];
        int present = 0;
        sz = sizeof(buf);
        EFI_STATUS s = gST->RuntimeServices->GetVariable(
            u"LoaderConfigTimeoutOneShot",
            (EFI_GUID *)&g_loader_systemd_guid,
            &attrs, &sz, buf);
        if (!EFI_ERROR(s) && sz > 0 && sz < sizeof(buf)) {
            present = 1;
            UINTN i = 0, clen = sz / 2u;
            UINT32 acc = 0;
            int saw_digit = 0;
            int rejected = 0;
            while (i < clen && buf[i] != 0) {
                CHAR16 c = buf[i];
                if (c < '0' || c > '9') { rejected = 1; break; }
                UINT32 digit = (UINT32)(c - '0');
                /* Overflow-safe cap check: reject BEFORE the multiply.
                 * Cap is 3600 (one hour). A pre-multiply check defeats
                 * a hostile UCS-2 value like "999999999999999" that
                 * would wrap UINT32 and slip past a post-arithmetic
                 * check. */
                if (acc > 360u || (acc == 360u && digit > 0u)) {
                    rejected = 1;
                    break;
                }
                acc = acc * 10u + digit;
                saw_digit = 1;
                i++;
            }
            if (saw_digit && !rejected) {
                *out_timeout_seconds = acc;
                *out_have_timeout = 1;
            }
        } else if (s == EFI_BUFFER_TOO_SMALL || (!EFI_ERROR(s) && sz == sizeof(buf))) {
            present = 1;
            serial_early_print("[BOOT] loader-vars: LoaderConfigTimeoutOneShot malformed/oversize -- deleting\n");
        }
        if (present) {
            (void)gST->RuntimeServices->SetVariable(
                u"LoaderConfigTimeoutOneShot",
                (EFI_GUID *)&g_loader_systemd_guid,
                attrs, 0, (VOID *)0);
        }
    }
}

/* Impossible-OS-specific feature bitmap (under our own vendor GUID,
 * NOT systemd-boot's GUID) advertising extension capabilities. Bit
 * positions are project-local; documented in docs/boot/loader-vars.md.
 * Linux tools never read this variable; Impossible-OS-aware userspace
 * (bootcfg, sysinfo) does. */
#define IMPOSSIBLE_FEATURE_AUDIT_JSONL    (1ULL << 0)
#define IMPOSSIBLE_FEATURE_ENTRY_KINDS    (1ULL << 1)
#define IMPOSSIBLE_FEATURE_HEALTH_GATE    (1ULL << 2)
#define IMPOSSIBLE_FEATURE_DEMOTE_NOT_DROP (1ULL << 3)
#define IMPOSSIBLE_FEATURES_PUBLISHED \
    (IMPOSSIBLE_FEATURE_AUDIT_JSONL \
     | IMPOSSIBLE_FEATURE_ENTRY_KINDS \
     | IMPOSSIBLE_FEATURE_HEALTH_GATE \
     | IMPOSSIBLE_FEATURE_DEMOTE_NOT_DROP)

static void loader_publish_impossible_ext(void)
{
    if (!gST || !gST->RuntimeServices || !gST->RuntimeServices->SetVariable)
        return;
    UINT64 features = IMPOSSIBLE_FEATURES_PUBLISHED;
    UINT32 attrs = EFI_VARIABLE_NON_VOLATILE
                 | EFI_VARIABLE_BOOTSERVICE_ACCESS
                 | EFI_VARIABLE_RUNTIME_ACCESS;
    EFI_STATUS s = gST->RuntimeServices->SetVariable(
        u"ImpossibleOSLoaderFeaturesExt",
        (EFI_GUID *)&g_impossible_os_guid,
        attrs, sizeof(features), &features);
    if (EFI_ERROR(s)) {
        g_boot_info_ptr->loader_vars_degraded = 1;
        serial_early_print("[BOOT] loader-vars: Impossible ext SetVariable failed\n");
    }
}

/* The boot-policy invoke step. Owns ESP read, OptionalData decode,
 * ladder dispatch, and boot_info v19 population. Counter scan / crash-
 * tolerant rename / kernel-side Phase-0 ABI validator are owned by the
 * next section. */
static void boot_policy_invoke(void)
{
    boot_set_section(BOOT_SECTION_BL_POLICY);
    post_code16(POST16_BL_BOOT_POLICY);
    serial_early_print("[BOOT] boot_policy_invoke...\n");

    /* Allocate the result + decision + inputs structs in pool. The
     * decision is ~3 KiB and the parser result is ~3.5 KiB so keeping
     * them off the pre-EBS stack (~64 KiB on most firmware) is the
     * right call -- the include/boot/boot_policy.h static_assert pins
     * decision <= 8 KiB precisely so this allocation discipline is
     * load-bearing. */
    boot_entries_parse_result_t *parse =
        (boot_entries_parse_result_t *)0;
    boot_policy_inputs_t *inputs = (boot_policy_inputs_t *)0;
    boot_policy_decision_t *decision = (boot_policy_decision_t *)0;
    unsigned char *json_buf = (unsigned char *)0;
    UINTN json_len = 0;
    EFI_STATUS s;

    s = gBS->AllocatePool(EfiLoaderData, sizeof(*parse), (VOID **)&parse);
    if (EFI_ERROR(s) || !parse) {
        boot_policy_publish_alloc_failure_fallback("parse_result");
        return;
    }
    s = gBS->AllocatePool(EfiLoaderData, sizeof(*inputs), (VOID **)&inputs);
    if (EFI_ERROR(s) || !inputs) {
        gBS->FreePool(parse);
        boot_policy_publish_alloc_failure_fallback("inputs");
        return;
    }
    s = gBS->AllocatePool(EfiLoaderData, sizeof(*decision), (VOID **)&decision);
    if (EFI_ERROR(s) || !decision) {
        gBS->FreePool(inputs);
        gBS->FreePool(parse);
        boot_policy_publish_alloc_failure_fallback("decision");
        return;
    }

    /* Zero everything; UEFI AllocatePool returns uninitialized pool. */
    for (UINTN i = 0; i < sizeof(*parse); i++) ((UINT8 *)parse)[i] = 0;
    for (UINTN i = 0; i < sizeof(*inputs); i++) ((UINT8 *)inputs)[i] = 0;
    for (UINTN i = 0; i < sizeof(*decision); i++) ((UINT8 *)decision)[i] = 0;

    /* Conservative input defaults for branches the ladder reads pre-set. */
    inputs->boot_current = 0xFFFFu;
    inputs->ab_slot_index = BOOT_POLICY_NO_AB_SLOT;

    /* Cache SecureBoot state once -- bootloader_secureboot_active()
     * does 3 gRT->GetVariable reads which can stall ~5 ms each on
     * AMI/Phoenix firmware. The parser currently treats this
     * argument as a no-op (path-escape gating is owned by the
     * policy filter) but pass the same cached value so a future
     * parser-layer gate sees the matching value. */
    int sb_active = bootloader_secureboot_active();

    /* ---- Parse the ESP store (or synthesize a STORE_INVALID result). */
    s = load_bootentries_json(&json_buf, &json_len);
    if (s == EFI_SUCCESS && json_buf && json_len > 0) {
        int rc = boot_entries_parse(json_buf, (unsigned int)json_len,
                                    sb_active, boot_policy_log_cb, parse);
        (void)rc;
        post_code16(POST16_BL_BOOT_POLICY_PARSE);
        serial_early_print("[BOOT] policy: bootentries.json parsed reject_code=");
        serial_early_print_uint((UINT32)parse->reject_code);
        serial_early_print(" entries=");
        serial_early_print_uint((UINT32)parse->entry_count);
        serial_early_print("\n");
        /* Append detected foreign-OS bootloaders as CHAINLOAD entries
         * (TODO-27 sec1) so they render alongside the IPOS store entries. */
        chainload_synthesize(parse);
    } else {
        /* Missing or unreadable -- synthesize a STORE_INVALID result so
         * the ladder writes FALLBACK_STORE_INVALID and the caller below
         * synthesizes the in-firmware fallback envelope. Use the
         * existing JSON_PARSE reject code with a clear reject_msg; the
         * code distinguishes "store unreadable" from "store rejected"
         * via the message shown on serial. */
        parse->reject_code = BOOT_ENTRIES_REJECT_JSON_PARSE;
        const char *m = "bootentries.json absent or unreadable";
        UINTN mi;
        for (mi = 0; mi < BOOT_ENTRIES_REJECT_MSG_LEN - 1u && m[mi]; mi++)
            parse->reject_msg[mi] = m[mi];
        parse->reject_msg[mi] = 0;
        serial_early_print("[BOOT] policy: bootentries.json absent/unreadable -- fallback\n");
    }

    /* ---- Fill ladder inputs. ----------------------------------------- */
    inputs->boot_current     = g_boot_info_ptr->uefi_boot_current;
    inputs->boot_next        = g_boot_info_ptr->uefi_boot_next;
    inputs->boot_next_valid  = g_boot_info_ptr->uefi_boot_next_valid ? 1 : 0;
    inputs->secure_boot_active = sb_active;  /* cached above */
    inputs->invoked_via_uki  = (g_uki_kernel_ptr != (UINT8 *)0) ? 1 : 0;

    /* Local machine UUID feeds the machine_id filter. Extract from
     * SMBIOS Type 1 (System Information) via the ConfigurationTable
     * entry-point lookup. The schema doc treats local_machine_id="" as
     * the "machine has no UUID -- match only entries with an empty
     * machine_id (wildcard)" sentinel, so any failure (no SMBIOS,
     * anchor mismatch, Type 1 absent, UUID not-specified) leaves the
     * field empty and machine-pinned entries simply do not match. */
    {
        char uuid_buf[37];
        uuid_buf[0] = 0;
        (void)smbios_extract_system_uuid(uuid_buf);
        UINTN k = 0;
        while (uuid_buf[k] && k < sizeof(inputs->local_machine_id) - 1u) {
            inputs->local_machine_id[k] = uuid_buf[k];
            k++;
        }
        inputs->local_machine_id[k] = 0;
    }

    /* Resolve BootCurrent -> internal id via Boot####.OptionalData. */
    read_boot_optionaldata_id(g_boot_info_ptr->uefi_boot_current,
                              inputs->bootcurrent_entry_id,
                              &inputs->bootcurrent_known);

    /* Consume systemd-boot one-shot vars. LoaderEntryOneShot overlays
     * the firmware-resolved bootcurrent hint (the policy ladder treats
     * bootcurrent_entry_id as a preferred-default bias, which is the
     * correct semantic for "next boot only"). LoaderConfigTimeoutOneShot
     * is captured into a local for the menu timeout below. Both vars
     * are deleted after read so they fire exactly once. */
    static char s_loader_oneshot_entry[64];
    int loader_have_entry_oneshot = 0;
    UINT32 loader_oneshot_timeout = 0;
    int loader_have_timeout_oneshot = 0;
    loader_consume_one_shot_vars(
        s_loader_oneshot_entry, sizeof(s_loader_oneshot_entry),
        &loader_have_entry_oneshot,
        &loader_oneshot_timeout,
        &loader_have_timeout_oneshot);
    if (loader_have_entry_oneshot) {
        UINTN k = 0;
        while (s_loader_oneshot_entry[k] &&
               k < sizeof(inputs->bootcurrent_entry_id) - 1u) {
            inputs->bootcurrent_entry_id[k] = s_loader_oneshot_entry[k];
            k++;
        }
        inputs->bootcurrent_entry_id[k] = 0;
        inputs->bootcurrent_known = 1;
        serial_early_print("[BOOT] loader-vars: LoaderEntryOneShot consumed -> ");
        serial_early_print(s_loader_oneshot_entry);
        serial_early_print("\n");
    }

    /* Caller-side capability gate. Mode-locked: today's load_kernel
     * uses the embedded UKI payload OR the disk kernel.exe based on
     * detect_uki_sections() before policy runs; the policy ladder
     * cannot switch modes. Cross-mode selection (kind=uki under
     * split, kind=split under UKI) would write a lying
     * selected_entry_id to boot_info v19 -- the ladder picked a
     * payload the loader will not actually load.
     *
     * UKI mode   -> admit only KIND_UKI.
     * Split mode -> admit KIND_SPLIT and KIND_SAFE. SAFE shares
     *               SPLIT's payload shape and load path; the
     *               safe-mode subset is materialized post-policy
     *               from the selected entry's flag set into
     *               boot_config.boot_mode.
     *
     * Every other kind needs follow-up wiring in a later section
     * before it can be selected without lying to consumers:
     *   - RECOVERY: needs recovery-partition load path (recovery
     *     integration feature + recovery partition feature).
     *   - TEST / DIAGNOSTICS: need boot_config materialization from
     *     the entry flag set so the kernel actually enters the
     *     matching mode instead of running the normal default.
     *   - INSTALLER: needs a distinct installer-image load path plus offline + first-install seeding.
     *   - CHAINLOAD / NETWORK / RESUME: need the per-entry-kind handler
     *     table (LoadImage / fetch+digest / hibernation validation).
     * Each owner section widens this mask as it ships. KIND_UNAVAILABLE
     * is the right reject reason while the loader cannot finish the
     * handoff a chosen entry would imply. Note that even within the
     * current mode, the selected entry's payload metadata (kernel/
     * cmdline/root for split, .linux section path for UKI) is
     * informational only until per-kind dispatchers land -- the loader
     * still loads the ambient payload. The mask gate is the floor;
     * selecting same-mode entries is the policy decision the kernel
     * sees recorded, not an instruction the loader follows yet. */
    if (inputs->invoked_via_uki) {
        inputs->supported_kinds_mask = (1u << BOOT_ENTRY_KIND_UKI);
    } else {
        inputs->supported_kinds_mask = (1u << BOOT_ENTRY_KIND_SPLIT);
    }
    /* SAFE entries are bootable in SPLIT mode only -- the loader
     * still loads the ambient kernel.exe, but boot_config.boot_mode
     * is materialized from envelope.kind == SAFE post-policy so the
     * kernel sees safe-mode requested. The kernel-side observers
     * (KUSD SafeBootMode, bare-metal hardening guards) read
     * boot_mode unchanged. Under UKI mode (invoked_via_uki),
     * admitting SAFE would let a SAFE entry's validated kernel path
     * be reported in audit while the running BOOTX64.UKI.efi's
     * embedded .linux kernel actually loads -- the loader's UKI
     * fast path returns before the authoritative SPLIT/SAFE branch
     * runs. Mode-locking SAFE to non-UKI mode kills that
     * misleading-state class entirely; UKI mode admits only UKI.
     * TEST and DIAGNOSTICS stay filtered until per-kind payload
     * parsing lands -- TEST without test_suite payload would run
     * all categories, and DIAGNOSTICS verbose flag must be honored
     * BEFORE policy decide to affect early POST. */
    if (!inputs->invoked_via_uki) {
        inputs->supported_kinds_mask |= (1u << BOOT_ENTRY_KIND_SAFE);
    }
    /* Multi-OS chainload (TODO-27 sec1): admit CHAINLOAD in BOTH modes when
     * chainload_detect() found foreign loaders -- the loader chainloads them via
     * pre-EBS LoadImage/StartImage regardless of UKI/split. Gated on a real
     * detection so no stray chainload entry is admitted when none were found;
     * the synthesized foreign-OS entries are the only functional chainload
     * source (the JSON-store chainload validator is deferred to TODO-07 sec13). */
    if (g_chainload_count > 0)
        inputs->supported_kinds_mask |= (1u << BOOT_ENTRY_KIND_CHAINLOAD);

    /* hotkey / watchdog / A/B / recovery_requested are owned by
     * neighboring features (boot menu, watchdog audit, A/B integration,
     * recovery integration). All zero today -- the BSS-zero default is
     * the documented "no override" semantics for each axis. */

    /* Boot policy NVRAM sticky read. The bootloader reads
     * `ImpossibleOS-BootSticky` and surfaces the trigger bits to the
     * ladder; the kernel acks consumed triggers post-publish via
     * uefi_var_set. Bootloader is read-only -- this is what the
     * exceptional-only NVRAM contract demands. A reset between read
     * and ack leaves the trigger pending, which is the correct sticky
     * semantic (a recovery request must persist across crashes until
     * the recovery boot durably records its outcome). */
    extern void boot_sticky_read_into_boot_info(struct boot_info *bi);
    boot_sticky_read_into_boot_info(g_boot_info_ptr);
    if (g_boot_info_ptr->sticky_present) {
        if (g_boot_info_ptr->sticky_recovery_trigger)
            inputs->recovery_requested = 1;
        if (g_boot_info_ptr->sticky_watchdog_rollback_request)
            inputs->watchdog_rollback = 1;
    }

    /* ---- Consume any outstanding MarkGood handoff. -----------------
     * State-bound: only deletes the counter whose filename matches the
     * {entry_id, tries_left, tries_done} triple recorded by the kernel
     * that wrote the var. Runs BEFORE the counter scan so the scan sees
     * the post-consume state -- the entry that was marked good appears
     * as "no counter, no gate" exactly like a fresh, untracked entry. */
    policy_consume_mark_good_var();

    /* ---- Counter directory scan. ------------------------------------ */
    boot_counter_t counters[BOOT_ENTRIES_MAX_ENTRIES];
    unsigned int counter_count = 0;
    int counters_overflow = 0;
    policy_scan_counters(counters, BOOT_ENTRIES_MAX_ENTRIES,
                         &counter_count, &counters_overflow);
    inputs->counters_overflow = counters_overflow;

    /* ---- Run the ladder. -------------------------------------------- */
    boot_policy_decide(inputs, parse, counters, counter_count, decision);
    post_code16(POST16_BL_BOOT_POLICY_DECIDE);

    /* On STORE_INVALID the ladder leaves selected_entry_id empty per
     * the file-format fallback contract; the caller synthesizes the
     * fallback envelope from the parser's helper. */
    if (decision->reason == BOOT_SELECTION_FALLBACK_STORE_INVALID) {
        /* Synthesize the fallback envelope so g_policy_selected_kind
         * captures the right kind for the post-EBS populate block,
         * but leave decision->selected_entry_id empty per the
         * boot_info v19 ABI: empty is the documented sentinel for
         * FALLBACK_STORE_INVALID (see include/kernel/boot_info.h
         * "selected_entry_id is empty in that case"). The fallback
         * envelope's "fallback" id is the LOAD-time identifier used
         * by load_kernel; the v19 selected_entry_id is the POLICY
         * decision and the policy did NOT pick an entry from the
         * (rejected) store. */
        boot_entries_synthesize_fallback(inputs->invoked_via_uki ? 1 : 0,
                                         &decision->selected);
        decision->selected_entry_id[0] = 0;
    }

    /* ---- Copy decision into boot_info v19 selection fields. ---------- */
    {
        UINTN i;
        for (i = 0; i < sizeof(g_boot_info_ptr->selected_entry_id) - 1u
                    && decision->selected_entry_id[i]; i++)
            g_boot_info_ptr->selected_entry_id[i] = decision->selected_entry_id[i];
        g_boot_info_ptr->selected_entry_id[i] = 0;
    }
    g_boot_info_ptr->selection_reason = (UINT32)decision->reason;
    g_boot_info_ptr->rejected_entry_count = decision->rejected_count;
    g_boot_info_ptr->rejected_entry_overflow = decision->rejected_overflow;
    {
        UINTN ri;
        UINTN n = decision->rejected_count;
        if (n > BOOT_ENTRIES_MAX_ENTRIES) n = BOOT_ENTRIES_MAX_ENTRIES;
        for (ri = 0; ri < n; ri++) {
            UINTN i;
            for (i = 0; i < sizeof(g_boot_info_ptr->rejected_entries[0].id) - 1u
                        && decision->rejected[ri].id[i]; i++)
                g_boot_info_ptr->rejected_entries[ri].id[i] = decision->rejected[ri].id[i];
            g_boot_info_ptr->rejected_entries[ri].id[i] = 0;
            g_boot_info_ptr->rejected_entries[ri].reason = decision->rejected[ri].reason;
        }
    }

    /* ---- Boot menu (interactive selection). ----------------------- */
    /* Build the candidate list from parsed entries minus rejected[]
     * minus HIDDEN/kind_skipped. Cap at BOOT_MENU_MAX_VISIBLE. */
    unsigned int cand_idx[BOOT_MENU_MAX_VISIBLE];
    unsigned int cand_default_idx = 0;
    unsigned int cand_count = boot_policy_menu_collect(parse, decision,
                                                cand_idx,
                                                BOOT_MENU_MAX_VISIBLE,
                                                &cand_default_idx);
    /* BLS display order: sort the visible candidate list so the on-
     * screen menu matches LoaderEntries (which loader_set_entries emits
     * in the same order). The policy ladder already chose the default
     * ENTRY; sorting only reorders the menu rows, so re-find the
     * default's new position in cand_idx after the sort. */
    if (cand_count > 1u) {
        unsigned int default_entry = (cand_default_idx < cand_count)
            ? cand_idx[cand_default_idx] : 0xFFFFFFFFu;
        boot_entries_bls_sort(parse, cand_idx, cand_count);
        if (default_entry != 0xFFFFFFFFu) {
            for (unsigned int i = 0; i < cand_count; i++) {
                if (cand_idx[i] == default_entry) {
                    cand_default_idx = i;
                    break;
                }
            }
        }
    }
    /* Default-not-found gate: when the policy ladder picked an entry
     * that is NOT representable in the visible candidate window
     * (cap-truncated past BOOT_MENU_MAX_VISIBLE, filtered out by
     * HIDDEN/kind_skipped/rejected, or otherwise missing from the
     * collected list), suppress the menu. Rendering it would
     * highlight a different entry than the one that actually boots
     * on Enter / timeout -- a user-visible boot-selection integrity
     * failure. The ladder pick still applies; only the interactive
     * UI is suppressed. */
    int default_visible = (cand_default_idx != BOOT_POLICY_MENU_DEFAULT_NOT_FOUND);
    if (!default_visible && decision->selected_entry_id[0] == 0) {
        /* Decision had no selected id (empty STORE_INVALID sentinel);
         * NOT_FOUND just means "no anchor". Fall back to highlighting
         * candidate 0; menu still safe to render. */
        cand_default_idx = 0;
        default_visible = 1;
    }
    /* F11 force-show probe: read keys briefly BEFORE the should_show
     * gate so an operator can force the menu to render on paths that
     * would otherwise auto-pick (forced-selection / single-entry
     * with hide_when_alone / etc.). The flag also clears the watchdog
     * once so the dwell does not start with a stale window. */
    int force_show = boot_menu_probe_f11();
    if (force_show) watchdog_reset();
    /* HIDE_WHEN_ALONE: when the visible candidate list has exactly
     * one entry AND it carries the flag, skip the interactive menu
     * (Win11 single-OS / systemd-boot default-pattern parity).
     * Without the flag, render briefly even for one entry so the
     * operator gets a chance to hit F8/F10/F11. F11 force_show
     * overrides the flag -- the operator's emergency override must
     * work on the quiet single-OS path too. */
    int allow_skip_when_alone = 0;
    if (cand_count == 1u && cand_default_idx < cand_count && !force_show
        && !loader_have_timeout_oneshot) {
        const boot_entry_envelope_t *lone =
            &parse->entries[cand_idx[cand_default_idx]];
        if (lone->flags & BOOT_ENTRY_FLAG_HIDE_WHEN_ALONE)
            allow_skip_when_alone = 1;
    }
    /* show_menu fires when:
     *   1. force_show (F11 buffered); OR
     *   2. should_show returns 1 (>= 2 viable candidates AND soft
     *      ladder reason); OR
     *   3. exactly 1 candidate AND the flag is NOT set (so the lone
     *      entry still gets a brief countdown for F8/F10/F11
     *      access).
     */
    int single_entry_render = (cand_count == 1u && !allow_skip_when_alone);
    /* A present LoaderConfigTimeoutOneShot forces the menu to render
     * (like F11 force_show): the operator/OS asked to see the menu this
     * boot (value 0 = no-timeout menu; 1..3600 = countdown). Without
     * this, a quiet HIDE_WHEN_ALONE / auto-pick path would consume +
     * delete the one-shot and silently boot through. */
    int show_menu = default_visible &&
        (force_show || loader_have_timeout_oneshot ||
         boot_policy_menu_should_show(decision, parse, cand_count) ||
         single_entry_render);
    if (show_menu) {
        /* Honor per-entry timeout_override on the default candidate.
         * Schema range is 0..600 (inclusive). 0 = "immediate auto-
         * boot, no menu UI" -- a kiosk preference; the lookup must
         * preserve that explicit zero rather than rewriting it to
         * BOOT_MENU_DEFAULT_TIMEOUT_S. NONE (sentinel) means "no
         * override set" and falls back to the default. */
        unsigned int timeout_s = BOOT_MENU_DEFAULT_TIMEOUT_S;
        int explicit_zero = 0;
        if (cand_default_idx < cand_count) {
            const boot_entry_envelope_t *def_e =
                &parse->entries[cand_idx[cand_default_idx]];
            if (def_e->timeout_override != BOOT_ENTRIES_TIMEOUT_OVERRIDE_NONE
                && def_e->timeout_override <= BOOT_MENU_TIMEOUT_CAP_S) {
                timeout_s = def_e->timeout_override;
                if (timeout_s == 0u) explicit_zero = 1;
            }
        }
        /* LoaderConfigTimeoutOneShot (systemd Boot Loader Interface,
         * one-shot) overrides the per-entry timeout_override for this
         * boot only. The var was consumed + deleted earlier so it fires
         * exactly once. systemd semantics: value 0 = show the menu with
         * NO timeout (no auto-boot); 1..N = countdown seconds. Value 0 is
         * routed to the no-countdown menu path (NOT the per-entry kiosk
         * path). A non-zero value above the 60s watchdog window is clamped
         * to 60 with a [WARN] -- a documented Impossible divergence from
         * systemd's 3600s range, because the interactive countdown cannot
         * safely outlive one firmware watchdog window (see boot_menu_run).
         * The prior <=60s gate instead silently dropped 61..3600 to the
         * default; clamp + warn replaces that silent drop. */
        int oneshot_no_timeout = 0;
        if (loader_have_timeout_oneshot) {
            if (loader_oneshot_timeout == 0u) {
                oneshot_no_timeout = 1;   /* menu, no countdown */
                explicit_zero = 0;        /* override any kiosk default */
                serial_early_print("[BOOT] loader-vars: LoaderConfigTimeoutOneShot=0 -- menu shown, no timeout\n");
            } else if (loader_oneshot_timeout > BOOT_MENU_TIMEOUT_CAP_S) {
                timeout_s = BOOT_MENU_TIMEOUT_CAP_S;
                explicit_zero = 0;
                serial_early_print("[WARN] loader-vars: "
                                   "LoaderConfigTimeoutOneShot > 60s "
                                   "clamped to 60s (watchdog window)\n");
            } else {
                timeout_s = loader_oneshot_timeout;   /* 1..60 */
                explicit_zero = 0;
                serial_early_print("[BOOT] loader-vars: LoaderConfigTimeoutOneShot applied\n");
            }
        }
        unsigned int chosen;
        int hotkey = BOOT_MENU_HOTKEY_NONE;
        int sb_active = bootloader_secureboot_active();
        if (explicit_zero && !force_show) {
            /* Skip menu UI entirely: kiosk/automated boot path. The
             * default candidate auto-boots without rendering or
             * polling for keys. F11 force_show overrides the kiosk
             * suppression so the operator's emergency override
             * works on any single-OS path. */
            serial_early_print("[BOOT] menu: timeout_override=0 -- "
                               "immediate auto-boot, no UI\n");
            chosen = cand_default_idx;
        } else {
            /* If force_show overrode an explicit-zero kiosk
             * timeout, render with the default countdown so the
             * operator gets a usable F8/F10/F11 window. */
            if (explicit_zero && force_show)
                timeout_s = BOOT_MENU_DEFAULT_TIMEOUT_S;
            /* F10 unsupported / SetVariable failure must return the
             * operator to the menu, not silently fall through to
             * boot. Re-enter the menu loop on every F10 failure.
             * After the cap, set g_menu_f10_disabled so subsequent
             * F10 keystrokes are ignored (the operator must press
             * Enter/Esc to boot); this avoids both an infinite
             * SetVariable spin AND a stranded normal boot the
             * operator did not request. */
            const unsigned int F10_MAX_RETRIES = 8u;
            unsigned int f10_retries = 0;
            g_menu_f10_disabled = 0;
            /* LoaderConfigTimeoutOneShot=0 means "show the menu with no
             * timeout" -- suppress the countdown so the menu stays up
             * until the operator picks (systemd one-shot semantics). */
            g_menu_no_autoboot = oneshot_no_timeout;
            for (;;) {
                hotkey = BOOT_MENU_HOTKEY_NONE;
                chosen = boot_menu_run(parse, cand_idx, cand_count,
                                       cand_default_idx, timeout_s,
                                       decision, sb_active,
                                       allow_skip_when_alone, &hotkey);
                if (hotkey != BOOT_MENU_HOTKEY_FW_SETUP) break;
                /* F10 pressed: try the firmware-setup transition.
                 * boot_menu_enter_fw_setup() does not return on
                 * success (ResetSystem terminates execution). If
                 * it returns, the transition was unsupported or
                 * SetVariable failed -- log and re-render the menu
                 * so the operator gets another shot at picking. */
                (void)boot_menu_enter_fw_setup();
                if (++f10_retries >= F10_MAX_RETRIES) {
                    serial_early_print("[BOOT] menu: F10 retries "
                                       "exhausted -- F10 disabled "
                                       "this session, countdown "
                                       "disabled, press Enter/Esc "
                                       "to boot\n");
                    g_menu_f10_disabled = 1;
                    g_menu_no_autoboot = 1;
                    /* Re-enter menu one more time with F10 disabled
                     * AND countdown disabled so the menu stays up
                     * indefinitely until the operator picks
                     * Enter/Esc. A failed firmware-setup request
                     * must not convert into a normal boot via
                     * countdown expiry. */
                    continue;
                }
                serial_early_print("[BOOT] menu: F10 unsupported -- "
                                   "re-entering menu\n");
            }
            g_menu_f10_disabled = 0;
            g_menu_no_autoboot = 0;
        }
        /* F8 -> safe-mode signal. Wire the operator's safe-mode
         * intent into boot_config.boot_mode. Kernel-side consumers
         * (KUSD SafeBootMode population, bare-metal hardening
         * guards) read boot_config.boot_mode unchanged. */
        if (hotkey == BOOT_MENU_HOTKEY_SAFE_MODE) {
            /* F8 wires the operator's safe-mode request into
             * boot_config.boot_mode regardless of which entry was
             * highlighted -- the operator's intent is "boot in
             * safe mode" independent of kind. Cross-domain wire-up:
             * KUSER_SHARED_DATA.SafeBootMode is populated from this
             * value by the kernel configuration policy. */
            g_boot_info_ptr->config.boot_mode = 1u;
            serial_early_print("[BOOT] menu: F8 -- boot_config.boot_mode=1 "
                               "(safe-mode override)\n");
        }
        if (chosen != cand_default_idx && chosen < cand_count) {
            /* User picked a DIFFERENT entry -- operator override.
             * Replace the decision id + envelope so counter
             * decrement targets the user's choice; flip the reason
             * to HOTKEY so v19 audit consumers (Registry,
             * policy-audit, LoaderXxx) see the operator action.
             * Default-Enter (chosen == default) keeps the original
             * ladder reason because the operator only confirmed
             * without changing the choice. */
            const boot_entry_envelope_t *picked =
                &parse->entries[cand_idx[chosen]];
            decision->selected = *picked;
            decision->reason = BOOT_SELECTION_HOTKEY;
            UINTN i;
            for (i = 0; i < sizeof(decision->selected_entry_id) - 1u
                        && picked->id[i]; i++)
                decision->selected_entry_id[i] = picked->id[i];
            decision->selected_entry_id[i] = 0;
            /* Mirror into boot_info v19 fields too. */
            for (i = 0; i < sizeof(g_boot_info_ptr->selected_entry_id) - 1u
                        && picked->id[i]; i++)
                g_boot_info_ptr->selected_entry_id[i] = picked->id[i];
            g_boot_info_ptr->selected_entry_id[i] = 0;
            g_boot_info_ptr->selection_reason = (UINT32)BOOT_SELECTION_HOTKEY;
            serial_early_print("[BOOT] menu: user override -- chosen=\"");
            serial_early_print(picked->id);
            serial_early_print("\" reason=HOTKEY\n");
        }
    } else {
        serial_early_print("[BOOT] menu: skipped (");
        if (!default_visible)
            serial_early_print("ladder pick not in visible window)");
        else if (cand_count == 0u) serial_early_print("no viable candidates)");
        else if (cand_count == 1u && allow_skip_when_alone)
            serial_early_print("hide_when_alone)");
        else serial_early_print("forced selection path)");
        serial_early_print("\n");
    }

    /* Capture the chosen kind for the post-EBS populate block. The
     * fallback envelope's kind is the right answer for STORE_INVALID
     * (boot_entries_synthesize_fallback set it to SPLIT or UKI). For
     * a menu-overridden pick, decision->selected was already updated
     * above so the kind reflects the user's choice. */
    g_policy_selected_kind = decision->selected.kind;

    /* Multi-OS chainload dispatch (TODO-27 sec1): if the operator picked a
     * synthesized foreign-OS entry, chainload it now (pre-EBS, gBS live).
     * chainload_exec() transfers control on success and does not return; a
     * return means LoadImage/StartImage failed (or Secure Boot dbx refused),
     * so demote to the IPOS fallback envelope and keep booting IPOS. */
    if (decision->selected.kind == BOOT_ENTRY_KIND_CHAINLOAD) {
        const char *cid = decision->selected.id;
        UINTN cidx = (UINTN)CHAINLOAD_MAX;
        if (cid[0] == 'c' && cid[9] == '-' &&
            cid[10] >= '0' && cid[10] <= '9' && cid[11] == 0)
            cidx = (UINTN)(cid[10] - '0');
        /* Trust gate (Codex re-adversarial): execute ONLY entries that
         * chainload_synthesize() built -- a bootentries.json entry with a
         * forged "chainload-N" id lacks BOOT_ENTRY_FLAG_SYNTHESIZED (no JSON
         * flag maps to it), so it falls through to the IPOS fallback instead of
         * hijacking a detected foreign loader. */
        if ((decision->selected.flags & BOOT_ENTRY_FLAG_SYNTHESIZED) &&
            cidx < g_chainload_count)
            (void)chainload_exec(&g_chainload_targets[cidx]);
        boot_entries_synthesize_fallback((g_uki_kernel_ptr != (UINT8 *)0) ? 1 : 0,
                                         &decision->selected);
        decision->reason = BOOT_SELECTION_FALLBACK_NO_VIABLE;
        g_policy_selected_kind = decision->selected.kind;
        /* Mirror the FALLBACK_NO_VIABLE audit contract (Codex re-adversarial):
         * the boot_info v19 selection fields were copied from the original
         * chainload pick earlier, so without this the registry / policy audit /
         * LoaderEntrySelected would report the (possibly forged) chainload id +
         * stale reason while IPOS actually booted. Rewrite id + reason to the
         * synthesized fallback so audit consumers see the truth. */
        {
            UINTN i;
            for (i = 0; i < sizeof(decision->selected_entry_id) - 1u
                        && decision->selected.id[i]; i++)
                decision->selected_entry_id[i] = decision->selected.id[i];
            decision->selected_entry_id[i] = 0;
            for (i = 0; i < sizeof(g_boot_info_ptr->selected_entry_id) - 1u
                        && decision->selected.id[i]; i++)
                g_boot_info_ptr->selected_entry_id[i] = decision->selected.id[i];
            g_boot_info_ptr->selected_entry_id[i] = 0;
            g_boot_info_ptr->selection_reason =
                (UINT32)BOOT_SELECTION_FALLBACK_NO_VIABLE;
        }
    }

    /* Per-kind boot_config materialization. After the entry is
     * picked (post-menu, pre-counter-decrement), translate the
     * envelope kind into boot_config so kernel-side consumers see
     * the operator's selection without consumer churn. SAFE is the
     * only kind shipped here -- it sets boot_mode=1 which the
     * kernel configuration policy then mirrors into KUSER_SHARED_
     * DATA.SafeBootMode. TEST + DIAGNOSTICS materialization needs
     * per-kind payload parsing (test_suite filter, diag verbosity
     * subset) which is owned by the entry-kind handler table; the
     * mask gate keeps them filtered until that work ships.
     *
     * Precedence: a structured boot-entry pick is the AUTHORITATIVE
     * source of boot_mode and overrides any boot.conf legacy value.
     * Otherwise an old `boot_mode=recovery` line in boot.conf would
     * silently defeat a fresh `kind: safe` entry pick (operator
     * picks SAFE, kernel boots recovery -- a precedence bug). The
     * F8 hotkey path above already wrote boot_mode=1 by the time
     * this runs, and SAFE materialization is idempotent (writing
     * 1 over 1 is a no-op). */
    /* Per-kind payload validation. Runs AFTER menu but BEFORE any
     * per-kind materialization (SAFE boot_mode flip) and BEFORE
     * counter decrement -- so a rejected SAFE entry does NOT force
     * boot_mode=1 on the fallback kernel that loads instead. The
     * earlier ordering (SAFE materialization first, validate
     * second) let a malformed SAFE entry change the kernel's boot
     * mode while audit said FALLBACK_NO_VIABLE.
     *
     * Skip on fallback paths: STORE_INVALID and NO_VIABLE both
     * synthesize an envelope with no payload object; running per-
     * kind validation against a synthesized fallback would always
     * fail SPLIT (payload-missing) and force a second-order
     * fallback. The fallback path's load_kernel already uses the
     * ambient kernel_paths[] search.
     *
     * On reject: flip reason to FALLBACK_NO_VIABLE AND clear
     * g_policy_selected_kind so the post-EBS kind->path mapping
     * does not still see SAFE/RECOVERY/etc. Counter decrement
     * skips on the demoted reason; selected_entry_id stays
     * populated so the audit trail records which entry was rejected.
     * On accept: stash decoded payload for the loader. */
    post_code16(POST16_BL_KIND_VALIDATE);
    if (decision->reason != BOOT_SELECTION_FALLBACK_STORE_INVALID &&
        decision->reason != BOOT_SELECTION_FALLBACK_NO_VIABLE) {
        const unsigned char *payload_bytes = (const unsigned char *)0;
        unsigned int payload_len = 0;
        if (json_buf && decision->selected.payload_present &&
            decision->selected.payload_offset + decision->selected.payload_length
                <= (unsigned int)json_len) {
            payload_bytes = json_buf + decision->selected.payload_offset;
            payload_len = decision->selected.payload_length;
        }
        int vrc = boot_entry_kind_validate(decision->selected.kind,
                                            decision->selected.flags,
                                            sb_active,
                                            payload_bytes, payload_len,
                                            &g_policy_decoded);
        if (vrc != BOOT_ENTRY_KIND_OK) {
            serial_early_print("[BOOT] policy: kind validate REJECT (");
            serial_early_print(boot_entry_kind_reject_name(vrc));
            serial_early_print(") -- demoting to fallback\n");

            /* Append the rejected entry to decision->rejected[] AND
             * boot_info.rejected_entries[] so structured audit
             * consumers (registry, policy-audit, loader vars) can
             * see the per-entry KIND_UNAVAILABLE reason. Without
             * this, post-boot consumers see retained selected id +
             * FALLBACK_NO_VIABLE but no structured reject row
             * explaining the demotion -- only serial text.
             * Skip when the array is at the cap to preserve
             * ladder-level rejects (set overflow=1 instead). */
            if (decision->rejected_count < BOOT_ENTRIES_MAX_ENTRIES) {
                unsigned int ri = decision->rejected_count;
                UINTN i;
                for (i = 0; i < sizeof(decision->rejected[0].id) - 1u
                            && decision->selected_entry_id[i]; i++)
                    decision->rejected[ri].id[i] = decision->selected_entry_id[i];
                decision->rejected[ri].id[i] = 0;
                decision->rejected[ri].reason =
                    (unsigned int)BOOT_REJECT_REASON_KIND_UNAVAILABLE;
                decision->rejected_count++;

                /* Mirror into boot_info v19 rejected_entries[]. */
                if (g_boot_info_ptr->rejected_entry_count <
                    BOOT_ENTRIES_MAX_ENTRIES) {
                    UINT32 bi = g_boot_info_ptr->rejected_entry_count;
                    for (i = 0;
                         i < sizeof(g_boot_info_ptr->rejected_entries[0].id) - 1u
                         && decision->selected_entry_id[i]; i++)
                        g_boot_info_ptr->rejected_entries[bi].id[i] =
                            decision->selected_entry_id[i];
                    g_boot_info_ptr->rejected_entries[bi].id[i] = 0;
                    g_boot_info_ptr->rejected_entries[bi].reason =
                        (UINT32)BOOT_REJECT_REASON_KIND_UNAVAILABLE;
                    g_boot_info_ptr->rejected_entry_count = bi + 1;
                } else {
                    g_boot_info_ptr->rejected_entry_overflow = 1u;
                }
            } else {
                decision->rejected_overflow = 1u;
                g_boot_info_ptr->rejected_entry_overflow = 1u;
            }

            decision->reason = BOOT_SELECTION_FALLBACK_NO_VIABLE;
            g_boot_info_ptr->selection_reason =
                (UINT32)BOOT_SELECTION_FALLBACK_NO_VIABLE;
            g_policy_decoded.valid = 0;
            /* Clear the captured kind so the post-EBS kind->path
             * mapping does not still see SAFE/RECOVERY/etc.
             * Otherwise a rejected SAFE entry would cause the
             * fallback kernel to inherit boot_path/boot_reason
             * mappings the rejected entry implied. */
            g_policy_selected_kind = G_POLICY_KIND_UNSET;
        } else {
            serial_early_print("[BOOT] policy: kind validate OK kind=");
            serial_early_print_uint((UINT32)g_policy_decoded.kind);
            if (g_policy_decoded.kind == BOOT_ENTRY_KIND_SPLIT &&
                g_policy_decoded.u.split.has_kernel) {
                serial_early_print(" kernel=\"");
                serial_early_print(g_policy_decoded.u.split.kernel);
                serial_early_print("\"");
            }
            serial_early_print("\n");
        }
    } else {
        /* Fallback path: synthesized envelope, no per-kind validation.
         * Leave g_policy_decoded zero (valid=0), load_kernel uses
         * ambient kernel_paths[]. */
        serial_early_print("[BOOT] policy: kind validate skipped (fallback path)\n");
    }
    post_code16(POST16_BL_KIND_VALIDATE_OK);

    /* SAFE materialization: post-validate so a malformed SAFE entry
     * does not flip boot_mode=1 before its rejection demotes it.
     * Gated on g_policy_decoded.valid so a demoted-to-FALLBACK_
     * NO_VIABLE entry leaves boot_mode at whatever boot.conf
     * provided (the fallback kernel then runs with boot.conf
     * defaults, not the rejected entry's intended mode).
     *
     * Precedence remains: a successfully-validated SAFE entry is
     * the AUTHORITATIVE source of boot_mode and overrides
     * boot.conf. F8 hotkey wrote boot_mode=1 earlier; SAFE
     * materialization here is idempotent in that case. */
    if (g_policy_decoded.valid &&
        g_policy_selected_kind == BOOT_ENTRY_KIND_SAFE) {
        unsigned int prior = g_boot_info_ptr->config.boot_mode;
        g_boot_info_ptr->config.boot_mode = 1u;
        if (prior != 1u) {
            serial_early_print("[BOOT] policy: kind=SAFE -- "
                               "boot_config.boot_mode=1 materialized "
                               "(overrode boot.conf value=");
            serial_early_print_uint((UINT32)prior);
            serial_early_print(")\n");
        } else {
            serial_early_print("[BOOT] policy: kind=SAFE -- "
                               "boot_config.boot_mode=1 (already set)\n");
        }
    }

    serial_early_print("[BOOT] policy: selection_reason=");
    serial_early_print_uint((UINT32)decision->reason);
    serial_early_print(" (");
    serial_early_print(selection_reason_name((unsigned int)decision->reason));
    serial_early_print(") selected=\"");
    serial_early_print(g_boot_info_ptr->selected_entry_id);
    serial_early_print("\" kind=");
    serial_early_print_uint((UINT32)decision->selected.kind);
    serial_early_print(" rejected=");
    serial_early_print_uint((UINT32)decision->rejected_count);
    if (decision->rejected_overflow) serial_early_print(" (overflow)");
    serial_early_print("\n");

    /* ---- Crash-tolerant counter decrement for the chosen entry. ------
     * Only decrement when the ladder selected an entry FROM THE STORE.
     * Fallback paths (STORE_INVALID, NO_VIABLE) chose a synthesized
     * fallback envelope, not a parsed-store entry, so there is no real
     * counter to track. */
    if (decision->reason != BOOT_SELECTION_FALLBACK_STORE_INVALID &&
        decision->reason != BOOT_SELECTION_FALLBACK_NO_VIABLE &&
        decision->selected_entry_id[0] != 0) {
        /* Look up the chosen entry's current counter state, if any. */
        boot_counter_t cur;
        int existed = 0;
        unsigned int idl = 0;
        while (decision->selected_entry_id[idl]
               && idl < sizeof(decision->selected_entry_id))
            idl++;
        for (unsigned int ci = 0; ci < counter_count; ci++) {
            unsigned int j;
            int eq = 1;
            for (j = 0; j < sizeof(counters[ci].id); j++) {
                if (counters[ci].id[j] != decision->selected_entry_id[j]) {
                    eq = 0; break;
                }
                if (counters[ci].id[j] == 0) break;
            }
            if (eq) {
                cur = counters[ci];
                existed = 1;
                break;
            }
        }
        unsigned int cur_left = existed ? cur.tries_left : 0u;
        unsigned int cur_done = existed ? cur.tries_done : 0u;
        policy_counter_decrement(decision->selected_entry_id,
                                 cur_left, cur_done, existed);

        /* Post-decrement state for the kernel handoff. First-boot
         * bootstrap path: counter_existed==0 -> policy_counter_decrement
         * wrote the BLS-3 bootstrap (tries_left=2, tries_done=1).
         * Subsequent boots write (cur_left-1, cur_done+1) with the
         * saturation logic from policy_counter_decrement. */
        unsigned int new_left, new_done;
        if (existed) {
            new_left = (cur_left > 0u) ? (cur_left - 1u) : 0u;
            new_done = (cur_done < BOOT_COUNTER_TRIES_DONE_MAX)
                          ? (cur_done + 1u) : BOOT_COUNTER_TRIES_DONE_MAX;
        } else {
            new_left = 2u;
            new_done = 1u;
        }
        policy_write_cur_boot_ctr(decision->selected_entry_id,
                                  new_left, new_done);

        /* Propagate per-entry health_check_subset if present. The
         * decision carries the parsed envelope of the selected entry
         * in decision->selected (boot_entries_parser_t shape). */
        policy_write_health_subset(&decision->selected);
        (void)idl;
    }

    post_code16(POST16_BL_BOOT_POLICY_OK);

    /* Publish systemd-boot-compatible LoaderXxx UEFI variables AFTER
     * the policy decision lands in boot_info and AFTER the per-entry
     * counter decrement, BEFORE the kernel handoff. Every SetVariable
     * failure flips boot_info.loader_vars_degraded but never blocks
     * boot. Init/exec usec timestamps come from boot_info.timing if
     * tsc_freq is calibrated; otherwise 0 is published (advisory).
     *
     * Timeout published: must match the EFFECTIVE timeout the menu
     * actually used. Precedence (highest wins):
     *   1. LoaderConfigTimeoutOneShot (already consumed above)
     *   2. selected entry envelope.timeout_override (if in range)
     *   3. BOOT_MENU_DEFAULT_TIMEOUT_S
     * If we just published BOOT_MENU_DEFAULT_TIMEOUT_S when the
     * actual menu used a per-entry override, Linux tools would see a
     * stale value and report the wrong effective timeout. */
    {
        UINT32 published_timeout = (UINT32)BOOT_MENU_DEFAULT_TIMEOUT_S;
        /* Per-entry override from the SELECTED entry. The decision's
         * envelope is populated even on FALLBACK paths (synthesized
         * fallback envelope has timeout_override == NONE). */
        if (decision &&
            decision->selected.timeout_override != BOOT_ENTRIES_TIMEOUT_OVERRIDE_NONE
            && decision->selected.timeout_override <= BOOT_MENU_TIMEOUT_CAP_S) {
            published_timeout = decision->selected.timeout_override;
        }
        /* One-shot wins over per-entry override (consumed above).
         * Publish what the menu actually honored: 0 (menu no-timeout) or
         * the value clamped to the 60s watchdog window, so bootctl and the
         * on-screen countdown agree (a request above 60 was clamped to 60
         * at the apply site). */
        if (loader_have_timeout_oneshot) {
            published_timeout = (loader_oneshot_timeout > BOOT_MENU_TIMEOUT_CAP_S)
                ? BOOT_MENU_TIMEOUT_CAP_S : loader_oneshot_timeout;
        }
        UINT64 init_us = 0, exec_us = 0;
        UINT64 tsc_freq = g_boot_info_ptr->timing.tsc_freq;
        if (tsc_freq > 0) {
            UINT64 now = boot_rdtsc();
            UINT64 entry = g_boot_info_ptr->timing.bl_entry;
            if (now > entry) {
                /* Convert TSC delta to microseconds. tsc_freq is Hz;
                 * (delta * 1e6) / tsc_freq fits in u64 for any plausible
                 * bootloader uptime. NOTE: timing.tsc_freq is calibrated
                 * AFTER load_kernel, so it is 0 at this pre-kernel publish
                 * point and this branch does not run today -- both
                 * LoaderTime vars publish 0. Producing meaningful distinct
                 * Init (loader entry) and Exec (about to hand off) values
                 * requires relocating this publish to a point after TSC
                 * calibration; that is the open follow-up item in this
                 * section. Left as equal timestamps until then. */
                init_us = (now - entry) / (tsc_freq / 1000000ULL);
                exec_us = init_us;
            }
        }
        loader_publish_readonly_vars(parse, decision,
                                      init_us, exec_us, published_timeout);
        loader_publish_impossible_ext();
    }

    /* FreePool the heap structures. The boot_info copies survive in
     * BOOT_INFO_PHYS_ADDR. The JSON buffer can also be released --
     * the parser deep-copied every needed string into the envelope
     * arrays inside parse_result, but parse_result itself is being
     * freed too. The kind static was already captured. */
    if (json_buf) gBS->FreePool(json_buf);
    gBS->FreePool(decision);
    gBS->FreePool(inputs);
    gBS->FreePool(parse);
}

/* ============================================================================
 * Pre-jump ABI mismatch check.
 *
 * Runs BEFORE ExitBootServices, AFTER the kernel image has been loaded
 * into file_buf + copied to its final physical address. Walks the
 * kernel ELF for a `.bootproto` section, compares the 4-tuple
 * { magic, version, struct_size, sha256 } against the bootloader's
 * compile-time expected values, and on mismatch OR parse failure:
 *   1. Writes a boot_version_fault record to UEFI NVRAM
 *      (BootVersionFault variable, IPOS GUID, NV|BS|RT attrs) so the
 *      kernel-side blackbox transcribe pipeline picks it up on the
 *      next successful boot.
 *   2. Renders a UCS-2 error on gST->ConOut showing observed vs
 *      expected for each differing field.
 *   3. Stalls 10 seconds so the operator can read the screen.
 *   4. ResetSystem(EfiResetCold, EFI_ABORTED) to reboot.
 *
 * This function does NOT return on mismatch. On match it returns
 * without side effects and load_kernel continues.
 *
 * The boot_version_fault schema mirrors include/kernel/boot_version.h
 * (48 bytes, pinned). A mismatch is classified by priority:
 *   magic  -> BAD_MAGIC (fault_class=2)
 *   version-> BAD_VERSION (fault_class=3)
 *   size   -> BAD_SIZE (fault_class=4)
 *   sha    -> BAD_VERSION with a SHA-hex hint in the UCS-2 message
 *            (closest analog; sha drift = missed version bump).
 * ============================================================================ */

/* Mirror of struct boot_version_fault from include/kernel/boot_version.h.
 * 48 bytes, pinned. Field order MUST match byte-for-byte. */
struct bl_boot_version_fault {
    UINT32 record_magic;
    UINT32 fault_class;
    UINT32 observed_magic;
    UINT32 expected_magic;
    UINT16 observed_version;
    UINT16 expected_version;
    UINT32 observed_size;
    UINT32 expected_size;
    UINT32 observed_loader_sec_ver;
    UINT32 expected_loader_sec_ver;
    UINT32 reserved_pad[3];
    /* v2 extension (Bootloader Build Identity): bootloader's compile-
     * time identity captured at fault time so the kernel transcribe
     * can attribute prior-boot faults to the loader that emitted
     * them, not to the (different) recovery loader on next boot.
     * Old kernel sees size mismatch and clears -- one-way upgrade
     * break, documented. */
    struct boot_loader_identity loader_identity;
};
_Static_assert(sizeof(struct bl_boot_version_fault) == 112,
    "bl_boot_version_fault layout pinned at 112 bytes (v2: includes loader identity)");

/* Stamp the bootloader's compile-time build identity into a fault
 * record. Called from every site that builds a bl_boot_version_fault
 * before persisting via bpp_persist_nvram_fault, so the kernel
 * transcribe path on the NEXT boot can attribute the fault to THIS
 * bootloader image (not the recovery loader that emits the next
 * successful handoff). */
static void bpp_stamp_loader_identity(struct bl_boot_version_fault *rec)
{
    static const UINT8 _git_sha[20] = BOOT_LOADER_GIT_SHA;
    static const char _label[] = BOOT_LOADER_BUILD_LABEL;
    UINT32 i;
    for (i = 0; i < 20; i++)
        rec->loader_identity.git_sha[i] = _git_sha[i];
    rec->loader_identity.build_unix_time = (UINT64)BOOT_LOADER_BUILD_TIME;
    for (i = 0; i < sizeof(rec->loader_identity.build_label); i++)
        rec->loader_identity.build_label[i] = 0;
    for (i = 0; i < sizeof(_label) - 1 &&
                i < sizeof(rec->loader_identity.build_label) - 1;
         i++)
        rec->loader_identity.build_label[i] = _label[i];
    for (i = 0; i < sizeof(rec->loader_identity._pad); i++)
        rec->loader_identity._pad[i] = 0;
}

/* Per-field offset asserts mirroring include/kernel/boot_version.h.
 * Same-size field reorders would silently break the rollback
 * payload (observed/expected_loader_sec_ver) the kernel reader
 * consumes; pin offsets explicitly so the producer side fails to
 * compile if either header drifts from the contract. */
_Static_assert(__builtin_offsetof(struct bl_boot_version_fault, record_magic) == 0,
    "bl_boot_version_fault.record_magic at offset 0");
_Static_assert(__builtin_offsetof(struct bl_boot_version_fault, fault_class) == 4,
    "bl_boot_version_fault.fault_class at offset 4");
_Static_assert(__builtin_offsetof(struct bl_boot_version_fault, observed_loader_sec_ver) == 28,
    "bl_boot_version_fault.observed_loader_sec_ver at offset 28 (rollback payload)");
_Static_assert(__builtin_offsetof(struct bl_boot_version_fault, expected_loader_sec_ver) == 32,
    "bl_boot_version_fault.expected_loader_sec_ver at offset 32 (rollback payload)");

/* Classification enum values mirror include/kernel/boot_version.h
 * (the bootloader cannot include kernel headers because they pull
 * kernel types). The kernel header is the canonical source; the
 * bootloader-side numeric `#define`s below are a UEFI-safe mirror,
 * and the parity asserts further down catch drift at compile time
 * if either side renumbers. */
#define BL_FAULT_OK            0u
#define BL_FAULT_NULL_HDR      1u
#define BL_FAULT_BAD_MAGIC     2u
#define BL_FAULT_BAD_VERSION   3u
#define BL_FAULT_BAD_SIZE      4u
#define BL_FAULT_SEC_ROLLBACK  5u
#define BL_FAULT_BAD_SHA       6u
#define BL_FAULT_BAD_PARSE     7u
#define BL_FAULT_PT_LOAD_FORBIDDEN 8u

/* Compile-time parity: pin the bootloader's mirrored constants
 * against the canonical kernel constants header so a drift on either
 * side fails the build. The full kernel header
 * (include/kernel/boot_version.h) pulls kernel-only types and cannot
 * be used here; the constants sub-header is UEFI-safe and is included
 * at the top of this file. */
_Static_assert(BL_FAULT_OK            == BOOT_VERSION_FAULT_VAL_OK,
    "BL_FAULT_OK drift vs kernel enum");
_Static_assert(BL_FAULT_NULL_HDR      == BOOT_VERSION_FAULT_VAL_NULL_HDR,
    "BL_FAULT_NULL_HDR drift vs kernel enum");
_Static_assert(BL_FAULT_BAD_MAGIC     == BOOT_VERSION_FAULT_VAL_BAD_MAGIC,
    "BL_FAULT_BAD_MAGIC drift vs kernel enum");
_Static_assert(BL_FAULT_BAD_VERSION   == BOOT_VERSION_FAULT_VAL_BAD_VERSION,
    "BL_FAULT_BAD_VERSION drift vs kernel enum");
_Static_assert(BL_FAULT_BAD_SIZE      == BOOT_VERSION_FAULT_VAL_BAD_SIZE,
    "BL_FAULT_BAD_SIZE drift vs kernel enum");
_Static_assert(BL_FAULT_SEC_ROLLBACK  == BOOT_VERSION_FAULT_VAL_SEC_ROLLBACK,
    "BL_FAULT_SEC_ROLLBACK drift vs kernel enum");
_Static_assert(BL_FAULT_BAD_SHA       == BOOT_VERSION_FAULT_VAL_BAD_SHA,
    "BL_FAULT_BAD_SHA drift vs kernel enum");
_Static_assert(BL_FAULT_BAD_PARSE     == BOOT_VERSION_FAULT_VAL_BAD_PARSE,
    "BL_FAULT_BAD_PARSE drift vs kernel enum");
_Static_assert(BL_FAULT_PT_LOAD_FORBIDDEN == BOOT_VERSION_FAULT_VAL_PT_LOAD_FORBIDDEN,
    "BL_FAULT_PT_LOAD_FORBIDDEN drift vs kernel enum");

/* NVRAM record layout invariants must match include/kernel/boot_version.h. */
#define BL_BOOT_VERSION_FAULT_MAGIC  0x42565046u
_Static_assert(BL_BOOT_VERSION_FAULT_MAGIC == BOOT_VERSION_FAULT_MAGIC,
    "BL_BOOT_VERSION_FAULT_MAGIC drift vs kernel constant");

/* UEFI console print helper: emit a hex byte. Uses efi_print under the
 * hood so non-ASCII digits work on every UEFI console. */
static void bpp_print_hex32(UINT32 v)
{
    static const CHAR16 hex[] = u"0123456789abcdef";
    CHAR16 buf[11];
    int i;
    buf[0] = u'0';
    buf[1] = u'x';
    for (i = 0; i < 8; i++)
        buf[2 + i] = hex[(v >> ((7 - i) * 4)) & 0xFu];
    buf[10] = 0;
    efi_print(buf);
}

static void bpp_print_decimal(UINT32 v)
{
    CHAR16 buf[11];
    int i = 10;
    buf[10] = 0;
    if (v == 0) {
        efi_print(u"0");
        return;
    }
    while (v > 0 && i > 0) {
        i--;
        buf[i] = (CHAR16)(u'0' + (v % 10));
        v /= 10;
    }
    efi_print(&buf[i]);
}

/* Persist the fault record. Returns EFI_SUCCESS on confirmed NVRAM
 * write; any other status means the record was NOT durably captured
 * and the caller must decide what to do (for rollback refusals we
 * keep the screen up instead of auto-resetting; for structural ABI
 * drift we still reset because a fresh rebuild often resolves it).
 *
 * CRITICAL: name + GUID MUST match what boot_version_blackbox_transcribe()
 * reads in src/kernel/main/boot_version.c (s_fault_name + s_fault_guid).
 * A mismatch silently drops the fault record on the next successful
 * boot. Duplicated from the kernel side rather than shared because the
 * bootloader cannot include kernel/boot_version.h (pulls kernel types). */
static EFI_STATUS bpp_persist_nvram_fault(
    const struct bl_boot_version_fault *rec)
{
    if (!gST || !gST->RuntimeServices || !gST->RuntimeServices->SetVariable)
        return EFI_UNSUPPORTED;
    /* GUID matches kernel src/kernel/main/boot_version.c s_fault_guid:
     * "IMPOSTFV-PROTOFLT" -- distinct from g_impossible_os_guid so a
     * POST16 overwriter cannot alias the fault record. */
    static EFI_GUID fault_guid = {
        0x494D504F, 0x5354, 0x4656,
        { 0x50, 0x52, 0x4F, 0x54, 0x4F, 0x46, 0x4C, 0x54 }
    };
    /* Name matches kernel s_fault_name: "ImpossibleBootProtoFault". */
    static CHAR16 name[] = u"ImpossibleBootProtoFault";
    UINT32 attrs = EFI_VARIABLE_NON_VOLATILE |
                   EFI_VARIABLE_BOOTSERVICE_ACCESS |
                   EFI_VARIABLE_RUNTIME_ACCESS;
    return gST->RuntimeServices->SetVariable(
        name, &fault_guid, attrs,
        sizeof(*rec), (VOID *)rec);
}

/* Render the observed-vs-expected error, persist NVRAM, stall, reset.
 * Does not return. */
static __attribute__((noreturn)) void bpp_render_and_reset(
    const struct bl_boot_version_fault *rec,
    int parse_result,
    UINT8 sha_mismatch)
{
    if (gST && gST->ConOut) {
        gST->ConOut->ClearScreen(gST->ConOut);
        efi_print(u"\r\n");
        efi_print(u"  Impossible OS -- Boot Protocol Mismatch\r\n");
        efi_print(u"\r\n");
        efi_print(u"  The kernel image does not match this bootloader's\r\n");
        efi_print(u"  compile-time ABI. Rebuild + reflash both halves.\r\n");
        efi_print(u"\r\n");
        if (parse_result != BOOTPROTO_OK) {
            efi_print(u"  .bootproto parse failed: ");
            /* Translate to ASCII via narrow print. */
            const char *n = bootproto_result_name(parse_result);
            CHAR16 wide[32];
            int i = 0;
            while (n[i] && i < 31) {
                wide[i] = (CHAR16)(UINT8)n[i];
                i++;
            }
            wide[i] = 0;
            efi_print(wide);
            efi_print(u"\r\n");
        } else {
            efi_print(u"  fault_class: ");
            bpp_print_decimal(rec->fault_class);
            efi_print(u"\r\n  observed_magic:   ");
            bpp_print_hex32(rec->observed_magic);
            efi_print(u"  expected_magic:   ");
            bpp_print_hex32(rec->expected_magic);
            efi_print(u"\r\n  observed_version: ");
            bpp_print_decimal((UINT32)rec->observed_version);
            efi_print(u"  expected_version: ");
            bpp_print_decimal((UINT32)rec->expected_version);
            efi_print(u"\r\n  observed_size:    ");
            bpp_print_decimal(rec->observed_size);
            efi_print(u"  expected_size:    ");
            bpp_print_decimal(rec->expected_size);
            efi_print(u"\r\n");
            if (sha_mismatch) {
                efi_print(u"  sha256 manifest drift detected\r\n");
                efi_print(u"    expected: ");
                /* Wide-print the expected hex (host string macro). */
                const char *hex = KERNEL_ABI_SHA256_HEX;
                CHAR16 wide[65];
                int i = 0;
                while (hex[i] && i < 64) {
                    wide[i] = (CHAR16)(UINT8)hex[i];
                    i++;
                }
                wide[i] = 0;
                efi_print(wide);
                efi_print(u"\r\n");
            }
        }
        efi_print(u"\r\n  Rebooting in 10 seconds...\r\n");
    }

    serial_early_print("[BOOT] ABI MISMATCH: parse=");
    serial_early_print_uint((UINT32)parse_result);
    serial_early_print(" fault_class=");
    serial_early_print_uint(rec->fault_class);
    serial_early_print(" observed_magic=0x");
    serial_early_print_hex16((UINT16)(rec->observed_magic >> 16));
    serial_early_print_hex16((UINT16)rec->observed_magic);
    serial_early_print("\n");

    /* Stamp the bootloader's compile-time build identity into a
     * local copy of the fault record before persist. The caller built
     * `rec` with const-pointer semantics; we own the copy lifetime
     * here. NVRAM consumers (next-boot kernel transcribe) read
     * loader_identity to attribute the fault to THIS bootloader. */
    struct bl_boot_version_fault rec_with_id = *rec;
    bpp_stamp_loader_identity(&rec_with_id);
    EFI_STATUS persist_status = bpp_persist_nvram_fault(&rec_with_id);
    if (persist_status != 0) {
        /* Print the FULL EFI_STATUS as 64-bit hex: the error bit
         * (0x8000000000000000) must not be truncated by a UINT32
         * cast, otherwise the logged value looks like success when
         * it was a real failure. */
        serial_early_print("[BOOT] ABI mismatch: NVRAM persist failed 0x");
        serial_early_print_hex64((UINT64)persist_status);
        serial_early_print(" -- next boot will not carry the transcript\n");
        /* On real hardware with NVRAM full or locked, the operator
         * never sees the serial log -- only the on-screen mismatch
         * banner. Without this on-screen warning they would expect
         * a transcript at X:\Diag\boot-proto-fault.txt on the next
         * boot and find none. Make the missing transcript visible. */
        if (gST && gST->ConOut) {
            efi_print(u"\r\n  WARNING: fault record could NOT be saved to NVRAM\r\n");
            efi_print(u"  The next boot will NOT carry a BlackBox transcript.\r\n");
            efi_print(u"  Capture this screen before reboot.\r\n");
        }
    }

    if (gBS && gBS->Stall)
        gBS->Stall(10 * 1000 * 1000);

    /* UEFI 2.x Reset types: 0=Cold, 1=Warm, 2=Shutdown. EFI_ABORTED
     * per the UEFI specification = 21 | high-bit. Use literals since
     * efi.h does not expose the enum names. */
    if (gST && gST->RuntimeServices && gST->RuntimeServices->ResetSystem) {
        gST->RuntimeServices->ResetSystem(
            /* EfiResetCold  */ 0,
            /* EFI_ABORTED   */ (21ULL | (1ULL << 63)),
            0, (VOID *)0);
    }

    /* Reset unreachable on well-formed firmware; fall through to HLT
     * as a last resort so the operator is not left with a silent
     * re-jump into a mismatched kernel. */
    for (;;)
        __asm__ volatile ("cli; hlt");
}

/* Render the anti-rollback refusal path. Called from the pre-jump
 * anti-rollback gate when shipped < required. Populates a 48-byte
 * boot_version_fault record with fault_class=SEC_ROLLBACK and
 * {observed,expected}_loader_sec_ver populated, persists to NVRAM via
 * the same helper the structural-mismatch path uses, and HALTS with
 * the operator-visible screen preserved.
 *
 * Distinct from bpp_render_and_reset (which handles structural ABI
 * drift): rollback refusal is NEVER transient. The stored required
 * value does not change between reboots, so auto-resetting would
 * just bring the operator back to the same error. HLT preserves the
 * diagnostic screen until the operator power-cycles and enters the
 * firmware's UEFI shell (via the firmware F12/Escape menu) to either
 * flash a newer signed kernel.exe OR clear the
 * IPOSRequiredSecVersion NVRAM variable under operator consent.
 *
 * Operator response differs from ABI drift. Rollback refusal means
 * "boot a newer signed kernel OR clear IPOSRequiredSecVersion";
 * ABI drift means "rebuild both halves" (where auto-reset after a
 * fresh build is actually helpful).
 *
 * Does not return. */
static __attribute__((noreturn)) void bpp_render_rollback_and_halt(
    UINT32 shipped, UINT32 required)
{
    struct bl_boot_version_fault rec;
    {
        UINT8 *p = (UINT8 *)&rec;
        UINTN i;
        for (i = 0; i < sizeof(rec); i++) p[i] = 0;
    }
    rec.record_magic             = BL_BOOT_VERSION_FAULT_MAGIC;
    rec.fault_class              = BL_FAULT_SEC_ROLLBACK;
    /* observed/expected structural fields stay zero; magic/version/
     * size are not the drift axis here. The rollback-specific fields
     * below carry the operator-actionable numbers. */
    rec.observed_loader_sec_ver  = shipped;
    rec.expected_loader_sec_ver  = required;
    bpp_stamp_loader_identity(&rec);

    EFI_STATUS persist_status = bpp_persist_nvram_fault(&rec);

    /* Persist the rollback refusal in the BootError NVRAM channel
     * too. last_boot_error consumers expect BOOT_ERR_* codes; the
     * richer ImpossibleBootProtoFault record carries the structured
     * detail (observed/expected versions + class) but the simple
     * BootError code lets next-boot diagnostics surface "rollback
     * refusal happened" without parsing the full record. */
    nvram_write_boot_error(BOOT_ERR_ROLLBACK_REFUSE);

    /* UEFI console (gST->ConOut) is ONLY safe pre-EBS. The anti-
     * rollback gate currently runs AFTER ExitBootServices at
     * boot_hw.c's refusal site, so touching ConOut here would drive
     * an undefined service. Gate the rich UCS-2 render on
     * !g_ebs_in_progress and fall back to serial-only when Boot
     * Services have been exited. Serial is post-EBS safe; Runtime
     * Services (used by bpp_persist_nvram_fault above) are too. */
    if (!g_ebs_in_progress && gST && gST->ConOut) {
        gST->ConOut->ClearScreen(gST->ConOut);
        efi_print(u"\r\n");
        efi_print(u"  Impossible OS -- Anti-Rollback Refusal\r\n");
        efi_print(u"\r\n");
        efi_print(u"  Security-version downgrade refused:\r\n");
        efi_print(u"    image    = ");
        bpp_print_decimal(shipped);
        efi_print(u"\r\n    required = ");
        bpp_print_decimal(required);
        efi_print(u"\r\n");
        efi_print(u"\r\n");
        efi_print(u"  Boot a newer signed kernel, or clear the\r\n");
        efi_print(u"  IPOSRequiredSecVersion UEFI NVRAM variable under\r\n");
        efi_print(u"  operator consent (UEFI shell: setvar / dmpstore)\r\n");
        efi_print(u"  and retry.\r\n");
        efi_print(u"\r\n");
        if (persist_status == 0) {
            efi_print(u"  Diagnostic persisted to UEFI NVRAM "
                      u"(ImpossibleBootProtoFault).\r\n");
        } else {
            efi_print(u"  WARNING: NVRAM persist failed; "
                      u"next boot will not have the transcript.\r\n");
            efi_print(u"  See serial log for full EFI_STATUS value.\r\n");
        }
        efi_print(u"\r\n  Power-cycle and enter the UEFI shell to recover.\r\n");
    } else if (bsod_can_render_graphical()) {
        /* Post-EBS path (the normal call site for the rollback gate)
         * cannot use ConOut. Render a ROLLBACK-SPECIFIC graphical
         * screen instead of the generic bsod_render_graphical: that
         * generic renderer hardcodes "Press any key to restart" + boot-
         * media advice ("Check boot media is inserted", "Verify
         * \\boot\\kernel.exe exists") which are wrong for the rollback
         * halt path -- there is no key-to-restart and the boot media
         * is fine. Build the screen here from the lower-level bsod_aa_*
         * primitives directly. The framebuffer remains valid post-EBS
         * (GOP backing memory survives ExitBootServices). */
        UINT32 bg = fb_pack_rgb(0x0A, 0x0A, 0x0A);
        bsod_fill_rect(0, 0, gFbWidth, gFbHeight, bg);
        bsod_blit_icon_aa(gFbWidth / 2 - BSOD_ICON_W / 2, 30,
                          0xFF, 0xFF, 0xFF, 0x0A, 0x0A, 0x0A);

        /* Title -- mirrors the formatting of the structural-mismatch
         * screen but with rollback wording. */
        {
            const char *t = "Security-version downgrade refused.";
            UINT32 w = bsod_aa_string_width(t, bsod_aa_TITLE);
            UINT32 x = (gFbWidth > w) ? (gFbWidth - w) / 2 : 8;
            bsod_aa_string(x, 150, t, bsod_aa_TITLE, bsod_aa_TITLE_data,
                           BSOD_AA_TITLE_ASCENT,
                           0xFF, 0xFF, 0xFF, 0x0A, 0x0A, 0x0A);
        }

        /* Build "image=N required=M" line; append it as a SUB-size
         * line. UINT32 -> decimal in a stack buffer; max 11 digits + NUL. */
        char vbuf[64];
        UINT32 vp = 0;
        const char *prefix = "image=";
        for (UINT32 i = 0; prefix[i] && vp < sizeof(vbuf) - 1; i++)
            vbuf[vp++] = prefix[i];
        {
            char tmp[16]; UINT32 ti = 0;
            UINT32 v = shipped;
            if (v == 0) tmp[ti++] = '0';
            while (v) { tmp[ti++] = (char)('0' + (v % 10)); v /= 10; }
            while (ti && vp < sizeof(vbuf) - 1) vbuf[vp++] = tmp[--ti];
        }
        const char *mid = "    required=";
        for (UINT32 i = 0; mid[i] && vp < sizeof(vbuf) - 1; i++)
            vbuf[vp++] = mid[i];
        {
            char tmp[16]; UINT32 ti = 0;
            UINT32 v = required;
            if (v == 0) tmp[ti++] = '0';
            while (v) { tmp[ti++] = (char)('0' + (v % 10)); v /= 10; }
            while (ti && vp < sizeof(vbuf) - 1) vbuf[vp++] = tmp[--ti];
        }
        vbuf[vp] = '\0';
        bsod_aa_string(80, 230, vbuf, bsod_aa_SUB, bsod_aa_SUB_data,
                       BSOD_AA_SUB_ASCENT,
                       0xFF, 0xFF, 0xFF, 0x0A, 0x0A, 0x0A);

        /* Recovery instructions -- ROLLBACK-SPECIFIC; no "press any
         * key" prompt because this path halts forever, and no
         * boot-media advice because the media is fine. */
        UINT32 ly = 290;
        const char *lines[] = {
            "What to do:",
            "  - Boot a newer signed kernel image,",
            "  - or clear IPOSRequiredSecVersion via the firmware UEFI",
            "    shell (setvar / dmpstore) under operator consent,",
            "  - then power-cycle.",
            "",
            "This machine will NOT auto-restart. Power-cycle to recover.",
            (const char *)0
        };
        for (UINT32 i = 0; lines[i]; i++) {
            bsod_aa_string(80, ly, lines[i],
                           bsod_aa_BODY, bsod_aa_BODY_data,
                           BSOD_AA_BODY_ASCENT,
                           0xFF, 0xFF, 0xFF, 0x0A, 0x0A, 0x0A);
            ly += BSOD_AA_BODY_ASCENT + 8;
        }
    }

    /* Serial diagnostic (always safe, pre- or post-EBS). Format the
     * full EFI_STATUS as a 64-bit hex value so the top error bit
     * (0x8000000000000000) is preserved -- truncating to UINT32 drops
     * the error flag and makes every failure look like success. */
    serial_early_print("[BOOT] ANTI-ROLLBACK REFUSAL: "
                       "fault_class=SEC_ROLLBACK shipped=");
    serial_early_print_uint(shipped);
    serial_early_print(" required=");
    serial_early_print_uint(required);
    serial_early_print(" persist_status=0x");
    serial_early_print_hex64((UINT64)persist_status);
    if (g_ebs_in_progress)
        serial_early_print(" (post-EBS: ConOut skipped, serial-only)");
    serial_early_print("\n");

    /* Halt with the screen/serial preserved. NO auto-reset: rollback
     * refusal has no path forward without operator intervention, and
     * resetting into the same refusal destroys the only diagnostic the
     * operator has. Power-cycle (operator-triggered) is the correct
     * next step. */
    for (;;)
        __asm__ volatile ("cli; hlt");
}

/* Verify the loaded kernel image carries a matching .bootproto
 * descriptor. Called from load_kernel right before its final success
 * return, with file_buf still valid. */
static void bootproto_verify_or_reset(const UINT8 *kernel_image,
                                      UINT64 kernel_size)
{
    struct boot_proto_descriptor desc;
    int r = bootproto_find(kernel_image, kernel_size, &desc);

    struct bl_boot_version_fault rec;
    {
        UINT8 *p = (UINT8 *)&rec;
        UINTN i;
        for (i = 0; i < sizeof(rec); i++) p[i] = 0;
    }
    rec.record_magic = BL_BOOT_VERSION_FAULT_MAGIC;

    UINT32 expected_magic   = (UINT32)BOOT_PROTO_DESCRIPTOR_MAGIC;
    UINT32 expected_version = (UINT32)BOOT_INFO_VERSION;
    UINT32 expected_size    = (UINT32)sizeof(struct boot_info);
    static const UINT8 expected_sha[32] = KERNEL_ABI_SHA256;

    /* Fill NVRAM record with bootloader-expected values; observed
     * fields will be overwritten if the descriptor parsed OK. */
    rec.expected_magic    = expected_magic;
    rec.expected_version  = (UINT16)expected_version;
    rec.expected_size     = expected_size;

    if (r != BOOTPROTO_OK) {
        /* Parse failure: dedicated BAD_PARSE class so the blackbox
         * transcript can distinguish a missing/truncated/misaligned
         * `.bootproto` section from a structural ABI mismatch. Stash
         * the raw parser error enum in observed_loader_sec_ver so the
         * kernel-side reader can surface the specific step that
         * failed (missing section / bounds / alignment / etc.). */
        rec.fault_class             = BL_FAULT_BAD_PARSE;
        rec.observed_magic          = 0;
        rec.observed_version        = 0;
        rec.observed_size           = 0;
        rec.observed_loader_sec_ver = (UINT32)r;
        bpp_render_and_reset(&rec, r, 0);
    }

    rec.observed_magic   = desc.magic;
    rec.observed_version = (UINT16)desc.version;
    rec.observed_size    = desc.struct_size;

    if (desc.magic != expected_magic) {
        rec.fault_class = BL_FAULT_BAD_MAGIC;
        bpp_render_and_reset(&rec, BOOTPROTO_OK, 0);
    }
    if (desc.version != expected_version) {
        rec.fault_class = BL_FAULT_BAD_VERSION;
        bpp_render_and_reset(&rec, BOOTPROTO_OK, 0);
    }
    if (desc.struct_size != expected_size) {
        rec.fault_class = BL_FAULT_BAD_SIZE;
        bpp_render_and_reset(&rec, BOOTPROTO_OK, 0);
    }

    {
        UINTN i;
        int sha_mismatch = 0;
        for (i = 0; i < 32; i++) {
            if (desc.sha256[i] != expected_sha[i]) {
                sha_mismatch = 1;
                break;
            }
        }
        if (sha_mismatch) {
            /* Magic + version + size all match but manifest hash
             * diverged. Dedicated BAD_SHA class so the blackbox
             * transcript names the real cause instead of a generic
             * version mismatch. */
            rec.fault_class = BL_FAULT_BAD_SHA;
            bpp_render_and_reset(&rec, BOOTPROTO_OK, 1);
        }
    }

    /* All fields match -- log once and return. */
    serial_early_print("[BOOT] .bootproto match: magic=0x");
    serial_early_print_hex16((UINT16)(desc.magic >> 16));
    serial_early_print_hex16((UINT16)desc.magic);
    serial_early_print(" version=");
    serial_early_print_uint(desc.version);
    serial_early_print(" struct_size=");
    serial_early_print_uint(desc.struct_size);
    serial_early_print(" sha256=match\n");
}

/* ============================================================================
 * PT_LOAD destination policy
 * ----------------------------------------------------------------------------
 * Defense-in-depth gate on the kernel ELF copy site. Even after Secure
 * Boot proves the bytes were not tampered with, a misbehaving or
 * malicious kernel image can declare any p_paddr it wants -- including
 * UEFI tables (gST/gRT), Runtime Services regions, the bootloader
 * image itself, ACPI tables, or firmware-reserved memory. Without this
 * gate, efi_memcpy(p_paddr, ...) would corrupt firmware state before
 * any later check runs.
 *
 * The policy walks the live UEFI memory map (snapshot taken just
 * before the PT_LOAD copy loop) and asserts every byte of every
 * PT_LOAD destination range falls inside an ALLOWED descriptor:
 *   EfiConventionalMemory  (only)
 *
 * FORBIDDEN descriptor types (any overlap fails the check):
 *   EfiReservedMemoryType, EfiLoaderCode, EfiLoaderData (bootloader
 *   allocations: file_buf, xHCI DMA, UKI payloads -- a kernel
 *   segment overwriting any of these would corrupt loader state
 *   before handoff), EfiBootServicesCode/Data,
 *   EfiRuntimeServicesCode/Data, EfiUnusableMemory, EfiACPIReclaimMemory,
 *   EfiACPIMemoryNVS, EfiMemoryMappedIO, EfiMemoryMappedIOPortSpace,
 *   EfiPalCode, EfiPersistentMemory, plus any descriptor whose Type is
 *   above EfiMaxMemoryType (forward-spec types we do not understand).
 *
 * Why Conventional only: the kernel ELF's standard load address is
 * 0x100000 upward, which UEFI firmware reports as
 * EfiConventionalMemory. Allowing LoaderData would let a malformed
 * or hostile ELF land in the bootloader's own heap.
 * ============================================================================ */

/* 128 KiB scratch for the snapshot. Real UEFI memory maps are
 * typically 50-200 descriptors at 48 bytes each (~10 KiB). The
 * doubled buffer covers descriptor-heavy firmware (large NVDIMM
 * configurations, Splice-style memory tiering, BIOS-emit-many-tiny
 * Reserved-region quirks) without an EFI_BUFFER_TOO_SMALL fail
 * (Codex post-commit adversarial M). The pre-snapshot path
 * cannot easily fall back to AllocatePool because firmware may
 * fragment the map between the size-probe call and the data call;
 * a static doubled buffer is simpler and matches the existing
 * BOOT_MMAP_MAX_ENTRIES = 512 design upper bound. */
#define PT_LOAD_POLICY_MMAP_BUF_PAGES 32  /* 128 KiB */

static const char *pt_load_mem_type_name(UINT32 type)
{
    switch (type) {
    case EfiReservedMemoryType:     return "Reserved";
    case EfiLoaderCode:             return "LoaderCode";
    case EfiLoaderData:             return "LoaderData";
    case EfiBootServicesCode:       return "BootServicesCode";
    case EfiBootServicesData:       return "BootServicesData";
    case EfiRuntimeServicesCode:    return "RuntimeServicesCode";
    case EfiRuntimeServicesData:    return "RuntimeServicesData";
    case EfiConventionalMemory:     return "Conventional";
    case EfiUnusableMemory:         return "Unusable";
    case EfiACPIReclaimMemory:      return "ACPIReclaim";
    case EfiACPIMemoryNVS:          return "ACPIMemoryNVS";
    case EfiMemoryMappedIO:         return "MMIO";
    case EfiMemoryMappedIOPortSpace:return "MMIOPort";
    case EfiPalCode:                return "PalCode";
    case EfiPersistentMemory:       return "Persistent";
    case EfiUnacceptedMemoryType:   return "Unaccepted";
    default:                        return "Unknown";
    }
}

/* True iff every byte of [dst_start, dst_end) falls inside a
 * descriptor of EfiConventionalMemory (the only allowed type;
 * EfiLoaderData is FORBIDDEN, see header above).
 *
 * On false return, *bad_type_out (if non-NULL) is set to the type of
 * the first forbidden / unmapped descriptor that overlaps the range,
 * so the caller can name it on the failure screen. dst_start MUST be
 * < dst_end (caller checked the wraparound case before calling).
 *
 * O(n) walk of the descriptor array; n is typically 50-200 for real
 * firmware. Per-segment cost is sub-millisecond. */
static int pt_load_destination_allowed(UINT64 dst_start, UINT64 dst_end,
                                       const UINT8 *map, UINTN map_size,
                                       UINTN desc_size,
                                       UINT32 *bad_type_out,
                                       UINT64 *bad_addr_out)
{
    UINTN offset;

    /* desc_size MUST be at least sizeof(EFI_MEMORY_DESCRIPTOR) before
     * casting; UEFI spec allows DescriptorSize >= sizeof(struct) for
     * forward compatibility but never less. map_size must be a whole
     * multiple of desc_size or the trailing partial entry is malformed.
     * Conditions match mmap_geometry_validate elsewhere in this file. */
    if (desc_size < sizeof(EFI_MEMORY_DESCRIPTOR) ||
        map_size == 0 ||
        (map_size % desc_size) != 0) {
        if (bad_type_out) *bad_type_out = (UINT32)-1;
        return 0;
    }
    if (dst_start >= dst_end) {
        if (bad_type_out) *bad_type_out = (UINT32)-1;
        return 0;
    }

    /* Sweep the destination range; for every byte that is covered by
     * a descriptor, the descriptor's type must be allowed. The walk
     * advances `cursor` to the next byte after the most recent
     * allowed descriptor that started covering the range. If we
     * complete the range without a forbidden hit AND the cursor
     * reached dst_end, every byte was allowed. */
    UINT64 cursor = dst_start;
    /* First pass: find any descriptor that overlaps and is forbidden;
     * report it. Second pass: if no forbidden overlap, walk allowed
     * descriptors and confirm full coverage. Two passes keep the
     * logic simple at the cost of 2*N descriptor reads (still O(n)). */

    /* Pass 1: forbidden overlap = immediate fail. */
    for (offset = 0; offset + desc_size <= map_size; offset += desc_size) {
        const EFI_MEMORY_DESCRIPTOR *d =
            (const EFI_MEMORY_DESCRIPTOR *)(map + offset);
        UINT64 d_start = (UINT64)d->PhysicalStart;
        UINT64 d_pages = d->NumberOfPages;
        if (d_pages == 0) continue;
        /* Wraparound guard: pages * EFI_PAGE_SIZE must fit. */
        if (d_pages > 0xFFFFFFFFFFFFFFFFULL / EFI_PAGE_SIZE) continue;
        UINT64 d_size = d_pages * EFI_PAGE_SIZE;
        if (d_start + d_size < d_start) continue;  /* descriptor wraps */
        UINT64 d_end = d_start + d_size;
        if (d_start >= dst_end || d_end <= dst_start) continue;  /* no overlap */

        UINT32 t = d->Type;
        if (t == EfiConventionalMemory)
            continue;  /* allowed; pass 2 will confirm full coverage */
        if (bad_type_out) *bad_type_out = t;
        /* Report WHERE, not just what: a segment spanning megabytes gives
         * no clue which end collided, and "shrink the kernel by how much"
         * is exactly the number the operator needs. */
        if (bad_addr_out) *bad_addr_out = d_start;
        return 0;
    }

    /* Pass 2: confirm every byte of [dst_start, dst_end) is covered
     * by an allowed descriptor. Iterate the cursor across allowed
     * descriptors; gaps in the map (no descriptor covers a byte) are
     * a fail because the kernel cannot land in unmapped firmware
     * space. */
    while (cursor < dst_end) {
        UINT64 best_end = cursor;
        for (offset = 0; offset + desc_size <= map_size; offset += desc_size) {
            const EFI_MEMORY_DESCRIPTOR *d =
                (const EFI_MEMORY_DESCRIPTOR *)(map + offset);
            UINT64 d_start = (UINT64)d->PhysicalStart;
            UINT64 d_pages = d->NumberOfPages;
            if (d_pages == 0) continue;
            if (d_pages > 0xFFFFFFFFFFFFFFFFULL / EFI_PAGE_SIZE) continue;
            UINT64 d_size = d_pages * EFI_PAGE_SIZE;
            if (d_start + d_size < d_start) continue;
            UINT64 d_end = d_start + d_size;
            UINT32 t = d->Type;
            if (t != EfiConventionalMemory) continue;
            /* Allowed descriptor that includes the cursor byte? */
            if (d_start <= cursor && d_end > cursor) {
                if (d_end > best_end) best_end = d_end;
            }
        }
        if (best_end == cursor) {
            /* No allowed descriptor covers the cursor byte -- gap. */
            if (bad_type_out) *bad_type_out = (UINT32)-1;
            return 0;
        }
        cursor = best_end;
    }

    return 1;
}

/* Snapshot the live UEFI memory map into the caller-provided buffer.
 * Returns EFI_SUCCESS on success with *map_size_out and *desc_size_out
 * populated. On failure (firmware error, buffer too small) returns
 * the EFI_STATUS so the caller can fail-closed. */
static EFI_STATUS pt_load_snapshot_mmap(UINT8 *buf, UINTN buf_size,
                                        UINTN *map_size_out,
                                        UINTN *desc_size_out)
{
    UINTN map_size = buf_size;
    UINTN map_key  = 0;
    UINTN desc_size = 0;
    UINT32 desc_version = 0;
    EFI_STATUS s;

    if (!gBS || !gBS->GetMemoryMap || !buf || buf_size == 0)
        return EFI_INVALID_PARAMETER;

    s = gBS->GetMemoryMap(&map_size, (EFI_MEMORY_DESCRIPTOR *)buf,
                          &map_key, &desc_size, &desc_version);
    if (EFI_ERROR(s))
        return s;
    /* Match the predicate's geometry guard so a malformed map is
     * caught at snapshot time rather than during the walk.
     * Conditions match mmap_geometry_validate elsewhere in this file. */
    if (desc_size < sizeof(EFI_MEMORY_DESCRIPTOR) ||
        map_size == 0 ||
        (map_size % desc_size) != 0)
        return EFI_INVALID_PARAMETER;
    if (map_size > buf_size)
        return EFI_BUFFER_TOO_SMALL;

    *map_size_out  = map_size;
    *desc_size_out = desc_size;
    return EFI_SUCCESS;
}

/* ============================================================================
 * Step 2: Load kernel ELF from FAT32
 * ============================================================================ */
static EFI_STATUS load_kernel(UINT64 *entry_point)
{
    boot_set_section(BOOT_SECTION_BL_KERNEL);
    EFI_GUID fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
    EFI_FILE_PROTOCOL *root_dir, *kernel_file;
    EFI_STATUS status;
    UINT8 *file_buf;
    UINTN file_size;
    Elf64_Ehdr *ehdr;
    Elf64_Phdr *phdr;
    UINT16 i;
    /* Disk-path-allocated kernel buffer ownership.  Set ONLY by the
     * AllocatePages block in the disk path; left zero on the UKI fast
     * path (which uses LoadedImage memory it does not own).  The
     * `load_error` cleanup label uses the page count to decide whether
     * to FreePages, so the UKI path's failure returns are no-ops here. */
    EFI_PHYSICAL_ADDRESS load_buf_addr  = 0;
    UINTN                load_buf_pages = 0;
    EFI_STATUS           load_err_status;

    /* UKI fast path: when the bootloader was invoked through a Unified
     * Kernel Image, detect_uki_sections() captured the embedded kernel
     * pointer + size from the LoadedImage's `.linux` PE section. Use
     * that buffer directly and skip the disk-load path entirely. The
     * whole-chain Secure Boot signature on the PE already vouched for
     * the kernel content; reading `\\kernel.exe` from the ESP would
     * give us an unsigned copy. */
    if (g_uki_kernel_ptr && g_uki_kernel_size > 0) {
        serial_early_print("[BOOT] load_kernel: using UKI embedded "
                           "`.linux` section\n");
        file_buf = g_uki_kernel_ptr;
        file_size = g_uki_kernel_size;
        /* Record the path taken in boot_info.flags so the kernel knows
         * its content came from a whole-chain-signed PE rather than
         * the per-file split path. */
        g_boot_info_ptr->flags |= BOOT_FLAG_INVOKED_VIA_UKI;
        /* Publish UKI signed-payload addresses (v14). The post-copy
         * physical addresses populated by uki_copy_payloads_to_loader_data()
         * survive ExitBootServices in EfiLoaderData. Zero stays in
         * the field when the corresponding section was absent in the
         * UKI -- legacy UKIs without payload sections still work. */
        g_boot_info_ptr->uki_initrd_addr   = g_uki_initrd_phys;
        g_boot_info_ptr->uki_initrd_size   = (UINT64)g_uki_initrd_size;
        g_boot_info_ptr->uki_recovery_addr = g_uki_recovery_phys;
        g_boot_info_ptr->uki_recovery_size = (UINT64)g_uki_recovery_size;
        g_boot_info_ptr->uki_modules_addr  = g_uki_modules_phys;
        g_boot_info_ptr->uki_modules_size  = (UINT64)g_uki_modules_size;
        goto kernel_loaded;
    }

    /* Use global g_boot_device_handle (set in efi_main) to get the
     * boot device's filesystem.  When the handle is unresolved or
     * lacks SimpleFS (PXE boot, partition handle without filesystem
     * driver), do NOT silently call LocateProtocol -- it returns the
     * first SimpleFS firmware enumerates, which on multi-disk systems
     * may not be the boot volume and could let an arbitrary ESP
     * provide kernel.exe. Leave fs/root_dir unset so the explicit
     * all-volumes fallback below runs with [WARN] diagnostics and the
     * operator can see which volume was selected. */
    fs = (EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *)0;
    root_dir = (EFI_FILE_PROTOCOL *)0;
    if (g_boot_device_handle) {
        status = gBS->HandleProtocol(g_boot_device_handle,
                                      &fs_guid, (VOID **)&fs);
        if (EFI_ERROR(status)) {
            serial_early_print("[WARN] Boot device has no SimpleFS; "
                               "deferring to explicit all-volumes "
                               "fallback search\n");
            fs = (EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *)0;
        }
    } else {
        serial_early_print("[WARN] No boot device handle; deferring to "
                           "explicit all-volumes fallback search\n");
    }

    /* Open root directory on the boot device when available; if fs is
     * NULL the all-volumes fallback below populates root_dir. */
    if (fs) {
        status = fs->OpenVolume(fs, &root_dir);
        if (EFI_ERROR(status)) {
            serial_early_print("[FAIL] Cannot open root volume\n");
            return status;
        }
    }

    /* Authoritative SPLIT path from policy ladder (when validated). When
     * g_policy_decoded carries a validated SPLIT kernel path, that path
     * is the SOLE candidate -- no fallback to ambient kernel_paths[]
     * search. Otherwise the ladder reports `selected=X` while a
     * different kernel actually loaded, which is the exact misleading-
     * state the entry-kind validator pipeline is supposed to prevent.
     * The validator already checked the path is ASCII + under an
     * allowed prefix + no `..` traversal, so the UCS-2 conversion below
     * is a direct byte-to-CHAR16 widen. */
    int kernel_loaded_from_authoritative = 0;
    /* SAFE entries share the SPLIT payload shape (same kernel/cmdline/
     * root/initrd[] fields, validated by the same code path). When a
     * SAFE entry carries a non-default kernel path, the loader MUST
     * honor it -- otherwise audit reports `selected=safe-entry-id`
     * while the ambient kernel boots, recreating the same
     * misleading-state failure the entry-kind validator pipeline
     * was meant to eliminate. The decoded union for SAFE lives at
     * u.split (validate_safe == validate_split), so the
     * authoritative-path branch reads the same fields for both. */
    int policy_owns_kernel_path =
        g_policy_decoded.valid &&
        (g_policy_decoded.kind == BOOT_ENTRY_KIND_SPLIT ||
         g_policy_decoded.kind == BOOT_ENTRY_KIND_SAFE) &&
        g_policy_decoded.u.split.has_kernel;

    /* Degraded boot guard: when the policy ladder owns the kernel path
     * but root_dir is NULL (boot device handle missing or lacks
     * SimpleFS), the ambient all-volumes fallback would silently load
     * an arbitrary kernel.exe from any volume while audit still names
     * the policy entry. Refuse to enter the fallback -- the
     * authoritative path either runs against the boot device or fails
     * closed. Operator sees the explicit reason on serial. */
    if (policy_owns_kernel_path && !root_dir) {
        serial_early_print("[FAIL] load_kernel: policy selected an "
                           "authoritative kernel path but boot device "
                           "is unavailable; refusing ambient fallback "
                           "(would mislead audit)\n");
        return EFI_NOT_FOUND;
    }

    if (policy_owns_kernel_path && root_dir) {
        const char *ap = g_policy_decoded.u.split.kernel;
        CHAR16 ucs[BOOT_ENTRIES_MAX_PATH_LEN + 1u];
        UINTN k = 0;
        while (ap[k] && k < BOOT_ENTRIES_MAX_PATH_LEN) {
            ucs[k] = (CHAR16)(unsigned char)ap[k];
            k++;
        }
        ucs[k] = 0;
        serial_early_print("[BOOT] load_kernel: authoritative SPLIT path \"");
        serial_early_print(ap);
        serial_early_print("\"\n");
        status = root_dir->Open(root_dir, &kernel_file, ucs,
                                 EFI_FILE_MODE_READ, 0);
        if (EFI_ERROR(status)) {
            serial_early_print("[FAIL] Authoritative kernel path not "
                               "openable; refusing fallback to ambient "
                               "(would mislead audit)\n");
            root_dir->Close(root_dir);
            return status;
        }
        serial_early_print("[BOOT] Kernel found at authoritative path\n");
        kernel_loaded_from_authoritative = 1;
    }

    /* Fallback kernel search: try paths in order. Runs ONLY when the
     * policy ladder did not produce an authoritative SPLIT path
     * (synthesized fallback envelope, FALLBACK_STORE_INVALID, or
     * validation rejected the chosen entry). */
    if (!kernel_loaded_from_authoritative) {
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
        /* Skip the boot-device search entirely when root_dir is NULL
         * (degraded boot path: no DeviceHandle or no SimpleFS on it).
         * The all-volumes fallback below handles those cases with
         * explicit [WARN] logging. */
        if (root_dir) {
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
                /* Only continue searching on EFI_NOT_FOUND; any other
                 * error (EFI_DEVICE_ERROR, EFI_VOLUME_CORRUPTED, etc.)
                 * is a hard failure -- report and stop. */
                if (status != EFI_NOT_FOUND) {
                    serial_early_print("[FAIL] Error opening ");
                    serial_early_print(kernel_path_names[pi]);
                    serial_early_print(" (device/FS error)\n");
                    root_dir->Close(root_dir);
                    return status;
                }
            }
        }
        if (!found) {
            /* Same-provenance gate: if boot.conf staged module/initrd/
             * recovery_image payloads (g_staged_payload_count > 0, loaded
             * from the boot device by load_staged_payloads() earlier in
             * efi_main), refuse to source the kernel from a DIFFERENT
             * volume. The all-volumes fallback below is the recovery chain
             * for a PAYLOAD-FREE boot; pairing it with boot-device-sourced
             * payloads would split the trust chain (kernel from volume A,
             * initrd/modules from volume B). Fail closed -- the operator
             * must keep kernel + staged payloads on one volume. Split-path
             * payload provenance hardening. */
            if (g_staged_payload_count > 0) {
                serial_early_print("[FATAL] staged payloads sourced from the "
                                   "boot device but kernel.exe is absent "
                                   "there; refusing a non-boot-volume kernel "
                                   "(provenance mismatch)\n");
                boot_fatal(BOOT_ERR_CONF_INVALID,
                           "split-path payloads require the kernel on the same volume",
                           "boot.conf names module/initrd/recovery_image but "
                           "kernel.exe is not on the boot device; put the "
                           "kernel on the boot ESP or remove the staged "
                           "payload entries.");
            }
            /*: Device fallback chain -- kernel not on boot device,
             * try all other filesystems before giving up. */
            if (root_dir) root_dir->Close(root_dir);
            root_dir = (EFI_FILE_PROTOCOL *)0;
            post_code16(POST16_BL_FALLBACK);
            serial_early_print("[WARN] Kernel not on boot device, "
                               "searching other volumes...\n");
            {
                EFI_GUID fs_fb_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
                EFI_HANDLE *fs_handles = (EFI_HANDLE *)0;
                UINTN fs_count = 0;
                EFI_STATUS fb_s;

                fb_s = gBS->LocateHandleBuffer(ByProtocol, &fs_fb_guid,
                                                (VOID *)0, &fs_count,
                                                &fs_handles);
                if (EFI_ERROR(fb_s) || !fs_handles || fs_count == 0) {
                    serial_early_print("[WARN] LocateHandleBuffer failed "
                                       "or no filesystems found\n");
                } else {
                    UINTN hi;
                    for (hi = 0; hi < fs_count && !found; hi++) {
                        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fb_fs =
                            (EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *)0;
                        EFI_FILE_PROTOCOL *fb_root;

                        /* Skip the boot device -- already tried */
                        if (fs_handles[hi] == g_boot_device_handle)
                            continue;

                        fb_s = gBS->HandleProtocol(fs_handles[hi],
                                                    &fs_fb_guid,
                                                    (VOID **)&fb_fs);
                        /* Defend against firmware returning EFI_SUCCESS
                         * with a NULL interface pointer -- mirrors the
                         * locate_boot_fs postcondition (boot filesystem
                         * scoping). The fallback path is the recovery
                         * chain users depend on when the boot device
                         * kernel search fails; do not crash here. */
                        if (EFI_ERROR(fb_s) || !fb_fs) continue;

                        fb_s = fb_fs->OpenVolume(fb_fs, &fb_root);
                        if (EFI_ERROR(fb_s)) continue;

                        for (pi = 0; pi < 3; pi++) {
                            fb_s = fb_root->Open(fb_root, &kernel_file,
                                                  (CHAR16 *)kernel_paths[pi],
                                                  EFI_FILE_MODE_READ, 0);
                            if (!EFI_ERROR(fb_s)) {
                                serial_early_print("[WARN] Kernel found on "
                                                   "non-boot device at ");
                                serial_early_print(kernel_path_names[pi]);
                                serial_early_print("\n");
                                /* Use this volume's root for the rest
                                 * of load_kernel */
                                root_dir = fb_root;
                                fs = fb_fs;
                                found = 1;
                                break;
                            }
                            /* Split per-volume hard-error policy:
                             *
                             *   - EFI_SECURITY_VIOLATION /
                             *     EFI_ACCESS_DENIED on a candidate
                             *     kernel.exe are firmware-detected
                             *     security/policy events.  Halt the
                             *     whole search so the operator sees
                             *     the failure instead of silently
                             *     booting from a different volume.
                             *
                             *   - All other non-EFI_NOT_FOUND statuses
                             *     (EFI_DEVICE_ERROR /
                             *     EFI_VOLUME_CORRUPTED / EFI_NO_MEDIA
                             *     / EFI_MEDIA_CHANGED / etc.) are
                             *     media or filesystem degradation on
                             *     this candidate volume only.  Log
                             *     and SKIP this volume -- continuing
                             *     to the next handle so one bad
                             *     removable/encrypted ESP cannot deny
                             *     boot recovery purely by
                             *     enumeration order.
                             *
                             * Strict semantics stay on the boot-device
                             * primary loop above (which owns kernel.exe
                             * trust). */
                            if (fb_s == EFI_SECURITY_VIOLATION ||
                                fb_s == EFI_ACCESS_DENIED) {
                                serial_early_print(
                                    "[FAIL] Security/policy denial "
                                    "opening ");
                                serial_early_print(kernel_path_names[pi]);
                                serial_early_print(
                                    " on non-boot volume\n");
                                fb_root->Close(fb_root);
                                gBS->FreePool(fs_handles);
                                return fb_s;
                            }
                            if (fb_s != EFI_NOT_FOUND) {
                                serial_early_print("[WARN] Skipping "
                                                   "non-boot volume "
                                                   "(media/FS error "
                                                   "opening ");
                                serial_early_print(kernel_path_names[pi]);
                                serial_early_print(")\n");
                                break;
                            }
                        }
                        if (!found)
                            fb_root->Close(fb_root);
                    }
                    gBS->FreePool(fs_handles);
                }
            }

            if (!found) {
                serial_early_print(
                    "[FAIL] Kernel not found on any volume. Searched: "
                    "\\boot\\kernel.exe, \\kernel.exe, "
                    "\\EFI\\ImpossibleOS\\kernel.exe\n");
                /* No POST16_BL_FALLBACK_OK -- last POST stays at 0xB094
                 * so a POST card shows fallback failed. */
                return EFI_NOT_FOUND;
            }
            post_code16(POST16_BL_FALLBACK_OK);
        }
    }

    /* Read entire file into memory.
     * Graduated allocation fallback: try 32 -> 16 -> 8 MiB.  When a
     * tier succeeds AT AN OVERLAPPING ADDRESS (boot_info region or
     * framebuffer), FreePages it and continue to the next smaller
     * tier rather than aborting -- a 32 MiB request might land on
     * top of boot_info, but a 16 MiB or 8 MiB request might be
     * placed elsewhere.  Only when every tier either fails to
     * allocate or is overlap-rejected do we return EFI_LOAD_ERROR. */
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
            EFI_PHYSICAL_ADDRESS try_addr = 0;
            status = gBS->AllocatePages(AllocateAnyPages, EfiLoaderData,
                                         pages, &try_addr);
            if (EFI_ERROR(status))
                continue;

            /* Overlap rejection: boot_info region.  Protected
             * region is the FULL struct boot_info at
             * BOOT_INFO_PHYS_ADDR (~22 KiB; capped at 65535 by the
             * static assert), not just the first 4 KiB. */
            UINT64 bi_start = (UINT64)BOOT_INFO_PHYS_ADDR;
            UINT64 bi_end   = bi_start + (UINT64)sizeof(struct boot_info);
            if ((UINT64)try_addr < bi_end &&
                (UINT64)try_addr + (UINT64)alloc_sizes[ai] > bi_start) {
                serial_early_print("[BOOT] Kernel buffer ");
                serial_early_print_uint((UINT32)(alloc_sizes[ai] / (1024 * 1024)));
                serial_early_print(" MiB overlaps boot_info -- "
                                   "trying smaller tier\n");
                gBS->FreePages(try_addr, pages);
                continue;
            }

            /* Overlap rejection: framebuffer region. */
            if (g_boot_info_ptr->fb.addr != 0) {
                UINT64 fb_end = g_boot_info_ptr->fb.addr +
                    (UINT64)g_boot_info_ptr->fb.pitch *
                    g_boot_info_ptr->fb.height;
                if (try_addr < fb_end &&
                    try_addr + alloc_sizes[ai] > g_boot_info_ptr->fb.addr) {
                    serial_early_print("[BOOT] Kernel buffer ");
                    serial_early_print_uint((UINT32)(alloc_sizes[ai] / (1024 * 1024)));
                    serial_early_print(" MiB overlaps framebuffer -- "
                                       "trying smaller tier\n");
                    gBS->FreePages(try_addr, pages);
                    continue;
                }
            }

            /* This tier succeeded AND is non-overlapping.  Commit. */
            buf_addr   = try_addr;
            alloc_size = alloc_sizes[ai];
            serial_early_print("[BOOT] Kernel buffer: ");
            serial_early_print_uint((UINT32)(alloc_size / (1024 * 1024)));
            serial_early_print(" MiB allocated\n");
            break;
        }
        if (alloc_size == 0) {
            serial_early_print("[FAIL] Cannot allocate non-overlapping "
                               "kernel buffer (tried 32/16/8 MiB)\n");
            kernel_file->Close(kernel_file);
            root_dir->Close(root_dir);
            return EFI_LOAD_ERROR;
        }
        file_buf = (UINT8 *)(UINTN)buf_addr;
        file_size = alloc_size;
        /* Record ownership for the load_error cleanup label.  The UKI
         * fast path leaves these zero so the cleanup is a no-op when
         * the kernel buffer came from LoadedImage memory. */
        load_buf_addr  = buf_addr;
        load_buf_pages = alloc_size / EFI_PAGE_SIZE;
    }

    status = kernel_file->Read(kernel_file, &file_size, file_buf);
    if (EFI_ERROR(status)) {
        efi_print(u"[FAIL] Cannot read kernel file\r\n");
        kernel_file->Close(kernel_file);
        root_dir->Close(root_dir);
        load_err_status = status;
        goto load_error;
    }

    kernel_file->Close(kernel_file);
    root_dir->Close(root_dir);

kernel_loaded:
    /* Codex audit-mode review 2026-04-29: validate file_size covers a
     * full Elf64_Ehdr BEFORE any header field deref. The disk path's
     * AllocatePages buffer is uninitialized past file_size; without
     * this guard a truncated kernel (file_size < 64) would let stale
     * buffer bytes feed e_phentsize / e_phnum / e_phoff and possibly
     * pass the existing ELF-bounds checks. Matches the additive-bounds
     * hardening pattern of the rest of load_kernel. */
    if (file_size < sizeof(Elf64_Ehdr)) {
        serial_early_print("[FAIL] Kernel ELF corrupt: file < Elf64_Ehdr\n");
        load_err_status = EFI_LOAD_ERROR;
        goto load_error;
    }

    /* Parse ELF header */
    ehdr = (Elf64_Ehdr *)file_buf;
    if (ehdr->e_magic != ELF_MAGIC || ehdr->e_class != 2 ||
        ehdr->e_machine != 0x3E) {
        serial_early_print("[FAIL] Kernel ELF corrupt: invalid magic/class/machine\n");
        load_err_status = EFI_LOAD_ERROR;
        goto load_error;
    }

    /* --- ELF bounds validation (S1 hardening) --- */

    /* Validate e_phentsize matches expected Elf64_Phdr size.
     * A mismatch means phdr indexing reads wrong offsets -- reject. */
    if (ehdr->e_phentsize != sizeof(Elf64_Phdr)) {
        serial_early_print("[FAIL] Kernel ELF corrupt: e_phentsize mismatch\n");
        load_err_status = EFI_LOAD_ERROR;
        goto load_error;
    }

    /* Cap e_phnum to prevent huge loop on corrupt ELF (matches Linux ELF_MAX_SEGMENTS spirit) */
    if (ehdr->e_phnum > 64) {
        serial_early_print("[FAIL] Kernel ELF corrupt: too many program headers\n");
        load_err_status = EFI_LOAD_ERROR;
        goto load_error;
    }

    /* Cap total kernel size at 32 MiB */
#define ELF_MAX_KERNEL_SIZE (32ULL * 1024 * 1024)
    if (file_size > ELF_MAX_KERNEL_SIZE) {
        serial_early_print("[FAIL] Kernel ELF corrupt: file exceeds 32 MiB limit\n");
        load_err_status = EFI_LOAD_ERROR;
        goto load_error;
    }

    /* Validate program header table is within file bounds.
     * Use subtraction-based check to prevent integer wraparound. */
    if (ehdr->e_phoff > file_size) {
        serial_early_print("[FAIL] Kernel ELF corrupt: phdr offset past EOF\n");
        load_err_status = EFI_LOAD_ERROR;
        goto load_error;
    }
    if (ehdr->e_phnum > (file_size - ehdr->e_phoff) / sizeof(Elf64_Phdr)) {
        serial_early_print("[FAIL] Kernel ELF corrupt: phdr table past EOF\n");
        load_err_status = EFI_LOAD_ERROR;
        goto load_error;
    }

    /* Pre-jump ABI mismatch check MUST run BEFORE the PT_LOAD copy
     * loop. A stale or malicious kernel image can declare any
     * p_paddr it wants; copying segments before validating the
     * .bootproto descriptor would let the bad image perform
     * attacker-controlled physical writes (UEFI tables, RuntimeServices
     * pointers, the bootloader itself) before the mismatch screen
     * ever fires. The parser walks file_buf, not the to-be-loaded
     * image, so it is safe to run here -- ehdr has been bounds-
     * validated above and file_buf + file_size are still authoritative
     * for the on-disk image. On mismatch the function does not return:
     * it persists a NVRAM fault record, renders a UCS-2 screen, and
     * reboots cold. */
    bootproto_verify_or_reset(file_buf, (UINT64)file_size);

    /* Snapshot the live UEFI memory map for PT_LOAD destination
     * policy. This snapshot is consumed by pt_load_destination_allowed
     * inside the segment loop below; it is independent of the
     * GetMemoryMap call that supplies the ExitBootServices map_key
     * later (that map_key has atomicity semantics; this snapshot
     * does not). The buffer is static-class so it lives in BSS, not
     * on the load_kernel stack. */
    static UINT8 s_pt_load_mmap_buf[
        PT_LOAD_POLICY_MMAP_BUF_PAGES * EFI_PAGE_SIZE];
    UINTN pt_mmap_size = 0;
    UINTN pt_desc_size = 0;
    {
        EFI_STATUS ms = pt_load_snapshot_mmap(
            s_pt_load_mmap_buf, sizeof s_pt_load_mmap_buf,
            &pt_mmap_size, &pt_desc_size);
        if (EFI_ERROR(ms)) {
            serial_early_print("[FAIL] Kernel ELF corrupt: PT_LOAD policy "
                               "GetMemoryMap failed status=0x");
            serial_early_print_hex16((UINT16)((UINT64)ms >> 16));
            serial_early_print_hex16((UINT16)ms);
            serial_early_print("\n");
            load_err_status = EFI_LOAD_ERROR;
            goto load_error;
        }
    }

    /* Load PT_LOAD segments with per-segment validation */
    phdr = (Elf64_Phdr *)(file_buf + ehdr->e_phoff);
    /* Initialize the kernel-image envelope accumulators at RUNTIME. The
     * bootloader does not zero .bss (firmware pool-poisons it with 0xAF), so a
     * static `= 0` on g_kernel_img_hi would start at poison and the max below
     * would never lower it -- the same 0xAF poison class the UKI/DHCP paths
     * defend against. Reset both here so the sweep is deterministic. */
    g_kernel_img_lo = ~0ULL;
    g_kernel_img_hi = 0;
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
            load_err_status = EFI_LOAD_ERROR;
            goto load_error;
        }

        /* Validate memsz >= filesz (ELF spec requirement) */
        if (phdr[i].p_memsz < phdr[i].p_filesz) {
            serial_early_print("[FAIL] Kernel ELF corrupt: segment ");
            serial_early_print_uint(i);
            serial_early_print(" memsz < filesz\n");
            load_err_status = EFI_LOAD_ERROR;
            goto load_error;
        }

        /* Reject segments with address wraparound */
        if (phdr[i].p_memsz > 0xFFFFFFFFFFFFFFFFULL - phdr[i].p_paddr) {
            serial_early_print("[FAIL] Kernel ELF corrupt: segment ");
            serial_early_print_uint(i);
            serial_early_print(" address wraparound\n");
            load_err_status = EFI_LOAD_ERROR;
            goto load_error;
        }

        /* Reject segments that overlap the FULL boot_info region.
         * S16: Previously guarded only [0x10000, 0x11000) -- but struct
         * boot_info is ~22 KiB so segments loaded at 0x11000..0x155BF
         * would silently clobber the tail (config, framebuffer, timing,
         * etc.) and still pass the post-handoff validator because the
         * header at offset 0 is untouched.  Use sizeof(struct boot_info)
         * as the upper bound -- kept <= 65535 by the S15 static assert. */
        {
            UINT64 seg_start = phdr[i].p_paddr;
            UINT64 seg_end   = seg_start + phdr[i].p_memsz;
            UINT64 bi_start  = (UINT64)BOOT_INFO_PHYS_ADDR;
            UINT64 bi_end    = bi_start + (UINT64)sizeof(struct boot_info);
            if (seg_start < bi_end && seg_end > bi_start) {
                serial_early_print("[FAIL] Kernel ELF corrupt: segment ");
                serial_early_print_uint(i);
                serial_early_print(" overlaps boot_info region\n");
                load_err_status = EFI_LOAD_ERROR;
                goto load_error;
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
                    load_err_status = EFI_LOAD_ERROR;
                    goto load_error;
                }
            }
        }

        /* PT_LOAD destination policy: reject any segment whose
         * physical destination overlaps firmware-owned, bootloader-
         * owned, or handoff-reserved memory. The predicate walks
         * the UEFI memory map snapshot taken above. Allowed type:
         * EfiConventionalMemory ONLY (EfiLoaderData is forbidden
         * because it holds bootloader scratch -- file_buf, xHCI
         * DMA, UKI payloads -- during load_kernel). p_memsz == 0
         * segments are degenerate and skipped (no bytes copied). */
        if (phdr[i].p_memsz > 0) {
            UINT64 dst_start = phdr[i].p_paddr;
            UINT64 dst_end   = dst_start + phdr[i].p_memsz;
            UINT32 bad_type = (UINT32)-1;
            UINT64 bad_addr = 0;
            if (!pt_load_destination_allowed(
                    dst_start, dst_end,
                    s_pt_load_mmap_buf, pt_mmap_size, pt_desc_size,
                    &bad_type, &bad_addr)) {
                /* Match the neighboring "[FAIL] Kernel ELF corrupt: segment N ..."
                 * shape so log scrapers and tests keying on that
                 * prefix see this rejection too. */
                serial_early_print("[FAIL] Kernel ELF corrupt: segment ");
                serial_early_print_uint(i);
                serial_early_print(" PT_LOAD destination forbidden "
                                   "paddr=0x");
                serial_early_print_hex16((UINT16)(dst_start >> 16));
                serial_early_print_hex16((UINT16)dst_start);
                serial_early_print(" memsz=");
                serial_early_print_uint((UINT32)phdr[i].p_memsz);
                serial_early_print(" type=");
                serial_early_print(pt_load_mem_type_name(bad_type));
                serial_early_print(" at=0x");
                serial_early_print_hex16((UINT16)(bad_addr >> 16));
                serial_early_print_hex16((UINT16)bad_addr);
                serial_early_print("\n");

                /* Persist a PT_LOAD_FORBIDDEN fault record so the
                 * next successful boot transcribes it to
                 * X:\Diag\boot-proto-fault.txt for operators with
                 * no serial console. observed_loader_sec_ver carries
                 * the offending EFI_MEMORY_TYPE per the v1 schema
                 * convention (BAD_PARSE reuses the same slot for the
                 * parser-error enum). Persist failure is non-fatal
                 * for this path -- the existing serial diagnostic +
                 * graphical BSOD already reach the operator. */
                struct bl_boot_version_fault rec;
                {
                    UINT8 *p = (UINT8 *)&rec;
                    UINTN k;
                    for (k = 0; k < sizeof(rec); k++) p[k] = 0;
                }
                rec.record_magic             = BL_BOOT_VERSION_FAULT_MAGIC;
                rec.fault_class              = BL_FAULT_PT_LOAD_FORBIDDEN;
                rec.observed_loader_sec_ver  = bad_type;
                bpp_stamp_loader_identity(&rec);
                (void)bpp_persist_nvram_fault(&rec);

                load_err_status = EFI_LOAD_ERROR;
                goto load_error;
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

        /* Higher-half section 2: record the loaded kernel-image physical
         * envelope from the ACTUAL loaded ELF p_paddr (design finding F2:
         * use loaded p_paddr, never kernel symbols). The HHDM writable
         * alias excludes [g_kernel_img_lo, g_kernel_img_hi) so it cannot
         * defeat W^X, and the arena disjointness check rejects an arena
         * that would overlap it. */
        if (phdr[i].p_memsz > 0) {
            UINT64 seg_lo = phdr[i].p_paddr;
            UINT64 seg_hi = phdr[i].p_paddr + phdr[i].p_memsz;
            if (seg_lo < g_kernel_img_lo) g_kernel_img_lo = seg_lo;
            if (seg_hi > g_kernel_img_hi) g_kernel_img_hi = seg_hi;
        }
    }

    /* Find kernel_main symbol in the ELF symbol table.
     * The ELF entry point (_start) is 32-bit code for GRUB compatibility.
     * Since UEFI is already in 64-bit Long Mode, we must call kernel_main
     * directly, skipping the 32->64 mode transition in entry.asm.
     *
     * Bound the section-header walk against file_size so a malformed
     * signed UKI .linux section cannot make the bootloader read past
     * the embedded kernel buffer or divide by zero.  The Secure Boot
     * signature only proves the bytes were not tampered with -- it
     * does NOT prove the ELF is well-formed. */
    {
        Elf64_Shdr *shdr = (Elf64_Shdr *)0;
        UINT16 s;
        UINT64 km_addr = 0;

        /* Section-table envelope: e_shoff + e_shnum * sh_size_of_entry
         * must fit within file_size; e_shentsize must equal sizeof. */
        if (ehdr->e_shentsize != sizeof(Elf64_Shdr) ||
            ehdr->e_shnum == 0 ||
            ehdr->e_shoff > file_size ||
            (UINT64)ehdr->e_shnum * sizeof(Elf64_Shdr) > file_size - ehdr->e_shoff) {
            efi_print(u"[FAIL] Kernel ELF: malformed section header table\r\n");
            load_err_status = EFI_LOAD_ERROR;
            goto load_error;
        }
        shdr = (Elf64_Shdr *)(file_buf + ehdr->e_shoff);

        for (s = 0; s < ehdr->e_shnum; s++) {
            if (shdr[s].sh_type == SHT_SYMTAB) {
                /* sh_entsize MUST be > 0 and match Elf64_Sym; sh_link
                 * MUST be a valid section index; symtab and strtab
                 * data MUST fit within file_size. */
                if (shdr[s].sh_entsize == 0 ||
                    shdr[s].sh_entsize != sizeof(Elf64_Sym) ||
                    shdr[s].sh_link >= ehdr->e_shnum ||
                    shdr[s].sh_offset > file_size ||
                    shdr[s].sh_size > file_size - shdr[s].sh_offset) {
                    continue;  /* malformed symtab; try next section */
                }
                UINT32 strtab_idx = shdr[s].sh_link;
                if (shdr[strtab_idx].sh_offset > file_size ||
                    shdr[strtab_idx].sh_size > file_size - shdr[strtab_idx].sh_offset) {
                    continue;  /* malformed strtab; try next */
                }
                UINT64 strtab_size = shdr[strtab_idx].sh_size;
                Elf64_Sym *syms = (Elf64_Sym *)(file_buf + shdr[s].sh_offset);
                UINT64 nsyms = shdr[s].sh_size / shdr[s].sh_entsize;
                char *strtab = (char *)(file_buf + shdr[strtab_idx].sh_offset);
                UINT64 j;

                for (j = 0; j < nsyms; j++) {
                    /* st_name must be within the strtab bounds; the
                     * strtab MUST end with NUL so name traversal
                     * cannot run off the end. Conservative cap: any
                     * st_name >= strtab_size is malformed. */
                    if (syms[j].st_name >= strtab_size)
                        continue;
                    char *name = strtab + syms[j].st_name;
                    UINT64 name_room = strtab_size - syms[j].st_name;
                    /* "kernel_main" + NUL == 12 bytes; require room. */
                    if (name_room < 12)
                        continue;
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
            load_err_status = EFI_LOAD_ERROR;
            goto load_error;
        }

        *entry_point = km_addr;
    }

    return EFI_SUCCESS;

load_error:
    /* Disk-path failure cleanup: free the kernel buffer if we own it.
     * UKI fast path leaves load_buf_pages == 0 so the FreePages call
     * is skipped (the LoadedImage memory is firmware-managed and we
     * never allocated it).  kernel_file / root_dir are closed at each
     * call site before jumping here because not all errors have them
     * open (e.g. post-Read errors close before validation; UKI path
     * never opened them). */
    if (load_buf_pages != 0)
        gBS->FreePages(load_buf_addr, load_buf_pages);
    return load_err_status;
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
    if (EFI_ERROR(status)) {
        /* The snapshot buffer is already allocated; a failed second GetMemoryMap
         * (e.g. the map changed under us) must free it -- the caller never receives
         * the pointer (*map_out unset), so it cannot. */
        gBS->FreePool(mmap);
        return status;
    }

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

/* ============================================================================
 * S17: Memory map normalization -- sort by base address, then carve
 * overlapping descriptors by UEFI memory type priority.
 *
 * Real firmware occasionally emits overlapping descriptors (particularly
 * ACPI Reclaim regions straddling BootServices buffers) that must be
 * resolved, not skipped, before the kernel PMM consumes the map.  The
 * normalization pass runs after fill_memory_map() accepts raw entries
 * and before mmap_count is published to boot_info.
 *
 * Priority order (higher wins the contested range):
 *   100 -- Reserved, MMIO, MMIOPort, PalCode, Unusable, Unknown (can't reclaim)
 *    95 -- RuntimeServicesCode/Data (survives ExitBootServices)
 *    85 -- ACPI NVS (firmware data that must not be overwritten)
 *    80 -- ACPI Reclaim (copied by kernel, then reclaimable later)
 *    70 -- Persistent (NVDIMM)
 *    50 -- BootServicesCode/Data, LoaderCode/Data (reclaimable post-EBS)
 *    40 -- Conventional (lowest priority, freely reclaimable)
 *
 * Runtime is promoted ABOVE ACPI Reclaim because runtime code/data must
 * remain reserved for the firmware even after the kernel copies the
 * ACPI tables out of an overlapping reclaim range.
 * ============================================================================ */

static int mmap_type_priority(UINT32 uefi_type)
{
    switch (uefi_type) {
    case EfiReservedMemoryType:
    case EfiMemoryMappedIO:
    case EfiMemoryMappedIOPortSpace:
    case EfiPalCode:
    case EfiUnusableMemory:
        return 100;
    case EfiRuntimeServicesCode:
    case EfiRuntimeServicesData:
        return 95;
    case EfiACPIMemoryNVS:
        return 85;
    case EfiACPIReclaimMemory:
        return 80;
    case EfiPersistentMemory:
        return 70;
    case EfiBootServicesCode:
    case EfiBootServicesData:
    case EfiLoaderCode:
    case EfiLoaderData:
        return 50;
    case EfiConventionalMemory:
        return 40;
    default:
        return 100;  /* conservative: treat unknown as reserved */
    }
}

/* Shift arr[i+1..count) left by one, decrementing count.  Used to remove
 * the entry at index i. */
static void mmap_remove_at(struct boot_mmap_entry *arr, UINT32 *count, UINT32 i)
{
    UINT32 k;
    if (i >= *count) return;
    for (k = i; k + 1 < *count; k++) arr[k] = arr[k + 1];
    (*count)--;
}

/* Log the canonical overlap-resolved line. */
static void mmap_log_resolved(UINT64 at, UINT32 winner_type, UINT32 loser_type)
{
    serial_early_print("[BOOT] mmap: overlap resolved at 0x");
    serial_early_print_hex16((UINT16)(at >> 48));
    serial_early_print_hex16((UINT16)(at >> 32));
    serial_early_print_hex16((UINT16)(at >> 16));
    serial_early_print_hex16((UINT16)at);
    serial_early_print(" -- type ");
    serial_early_print_uint(winner_type);
    serial_early_print(" wins over type ");
    serial_early_print_uint(loser_type);
    serial_early_print("\n");
}

/* --- Sweep-line normalization (S17) ---------------------------------
 *
 * An iterative pair-resolver with an ad-hoc pass guard can exit
 * before the map stabilizes on pathological inputs.  A provably
 * correct O(n^2) sweep-line pass:
 *
 *   1. Copy input into a work buffer, build 2n endpoint events.
 *   2. Sort events by (addr, end-before-start) so touching ranges do
 *      not create zero-width spurious intervals.
 *   3. Walk events linearly.  Between consecutive events the active
 *      set is fixed; emit one range taking the highest-priority active
 *      descriptor, coalescing with the previous output if adjacent,
 *      same type, and same attribute.
 *
 * Storage: MMAP_NORMALIZE_MAX = BOOT_MMAP_MAX_ENTRIES.  Event, work,
 * and active-set buffers live in BSS (~34 KiB) so the bootloader
 * stack stays shallow.
 * -------------------------------------------------------------------- */

#define MMAP_NORMALIZE_MAX  BOOT_MMAP_MAX_ENTRIES

struct mmap_event {
    UINT64 addr;
    UINT32 desc_idx;
    UINT8  is_start;   /* 1 = descriptor start, 0 = descriptor end */
    UINT8  _pad[3];
};

static struct mmap_event        s_mmap_events[2 * MMAP_NORMALIZE_MAX];
static struct boot_mmap_entry   s_mmap_work[MMAP_NORMALIZE_MAX];
static UINT32                   s_mmap_active[MMAP_NORMALIZE_MAX];

static void mmap_event_sort(struct mmap_event *ev, UINT32 count)
{
    /* Insertion sort: nearly-sorted firmware maps run in ~O(n).
     * Tiebreak end-before-start at equal addr so adjacent ranges
     * [A.end == B.start] close A before opening B. */
    UINT32 i, j;
    for (i = 1; i < count; i++) {
        struct mmap_event key = ev[i];
        j = i;
        while (j > 0) {
            if (ev[j - 1].addr > key.addr ||
                (ev[j - 1].addr == key.addr &&
                 ev[j - 1].is_start > key.is_start)) {
                ev[j] = ev[j - 1];
                j--;
            } else {
                break;
            }
        }
        ev[j] = key;
    }
}

static UINT32 mmap_active_find_winner(UINT32 active_count)
{
    /* Stable tiebreak: when two active descriptors share the same
     * priority, pick the one with the lower original index into
     * s_mmap_work.  Original indices never change, so the winner is
     * independent of the active-set removal order.  Without this
     * tiebreak, mmap_active_remove's swap-remove could shift a
     * different same-priority descriptor to the front and flip the
     * winner based on unrelated event ordering. */
    UINT32 best = s_mmap_active[0];
    int best_pri = mmap_type_priority(s_mmap_work[best].uefi_memory_type);
    UINT32 k;
    for (k = 1; k < active_count; k++) {
        UINT32 idx = s_mmap_active[k];
        int pri = mmap_type_priority(s_mmap_work[idx].uefi_memory_type);
        if (pri > best_pri || (pri == best_pri && idx < best)) {
            best = idx;
            best_pri = pri;
        }
    }
    return best;
}

/* Swap-remove desc_idx from the active set.  Returns new count. */
static UINT32 mmap_active_remove(UINT32 active_count, UINT32 desc_idx)
{
    UINT32 k;
    for (k = 0; k < active_count; k++) {
        if (s_mmap_active[k] == desc_idx) {
            s_mmap_active[k] = s_mmap_active[active_count - 1];
            return active_count - 1;
        }
    }
    return active_count;
}

/* Find the first active descriptor that is not `winner_idx` and return
 * its UEFI memory type -- used only for the overlap-resolved log line. */
static UINT32 mmap_active_first_loser_type(UINT32 active_count, UINT32 winner_idx)
{
    UINT32 k;
    for (k = 0; k < active_count; k++) {
        UINT32 idx = s_mmap_active[k];
        if (idx != winner_idx) return s_mmap_work[idx].uefi_memory_type;
    }
    return 0;
}

/* Append or coalesce one output segment.  Returns 0 if the segment
 * was dropped due to the array cap (caller sets mmap_quirks). */
static int mmap_emit_segment(struct boot_mmap_entry *arr, UINT32 *out_count,
                              UINT32 max_entries, UINT64 seg_start,
                              UINT64 seg_end, UINT32 win_type,
                              UINT32 win_simple, UINT64 win_attr)
{
    if (*out_count > 0) {
        struct boot_mmap_entry *last = &arr[*out_count - 1];
        if (last->base_addr + last->length == seg_start &&
            last->uefi_memory_type == win_type &&
            last->attribute == win_attr) {
            last->length += (seg_end - seg_start);
            return 1;
        }
    }
    if (*out_count >= max_entries) return 0;
    arr[*out_count].base_addr        = seg_start;
    arr[*out_count].length           = seg_end - seg_start;
    arr[*out_count].type             = win_simple;
    arr[*out_count].uefi_memory_type = win_type;
    arr[*out_count].attribute        = win_attr;
    (*out_count)++;
    return 1;
}

static void mmap_normalize(struct boot_mmap_entry *arr, UINT32 *count,
                            UINT32 max_entries)
{
    UINT32 n = *count;
    UINT32 ev_count = 0;
    UINT32 active_count = 0;
    UINT32 out_count = 0;
    UINT32 i;
    UINT64 prev_addr = 0;
    int prev_valid = 0;
    /* Local log-once guard for the normalize-overflow [WARN].
     * Distinct from g_boot_info_ptr->mmap_truncated so the normalize
     * warning still fires when the raw-UEFI-overflow path already set
     * the flag.  Both producers must be visible on combined raw +
     * normalize overflows for diagnostic completeness. */
    int normalize_overflow_logged = 0;

    if (n < 2) return;
    if (n > MMAP_NORMALIZE_MAX) n = MMAP_NORMALIZE_MAX;

    /* Snapshot validated input into the work buffer.  The active-set
     * indices reference s_mmap_work, not arr, so we can freely
     * overwrite arr as we emit output. */
    for (i = 0; i < n; i++) s_mmap_work[i] = arr[i];

    for (i = 0; i < n; i++) {
        if (s_mmap_work[i].length == 0) continue;
        s_mmap_events[ev_count].addr     = s_mmap_work[i].base_addr;
        s_mmap_events[ev_count].desc_idx = i;
        s_mmap_events[ev_count].is_start = 1;
        ev_count++;
        s_mmap_events[ev_count].addr     =
            s_mmap_work[i].base_addr + s_mmap_work[i].length;
        s_mmap_events[ev_count].desc_idx = i;
        s_mmap_events[ev_count].is_start = 0;
        ev_count++;
    }

    if (ev_count < 2) { *count = 0; return; }

    mmap_event_sort(s_mmap_events, ev_count);

    for (i = 0; i < ev_count; i++) {
        UINT64 curr_addr = s_mmap_events[i].addr;

        if (prev_valid && curr_addr > prev_addr && active_count > 0) {
            UINT32 winner = mmap_active_find_winner(active_count);
            UINT32 win_type   = s_mmap_work[winner].uefi_memory_type;
            UINT32 win_simple = s_mmap_work[winner].type;
            UINT64 win_attr   = s_mmap_work[winner].attribute;

            if (active_count > 1) {
                UINT32 loser_type =
                    mmap_active_first_loser_type(active_count, winner);
                if (loser_type != win_type)
                    mmap_log_resolved(prev_addr, win_type, loser_type);
            }

            if (!mmap_emit_segment(arr, &out_count, max_entries,
                                    prev_addr, curr_addr, win_type,
                                    win_simple, win_attr)) {
                /* Sweep-line emitter dropped a normalized segment
                 * because BOOT_MMAP_MAX_ENTRIES was reached.  This is
                 * a kernel-visible truncation -- raw UEFI descriptor
                 * count <= 512 can still expand past 512 after
                 * overlap carving.  Set mmap_truncated so the PMM
                 * init warning at pmm.c fires; mmap_quirks captures
                 * the same condition for diagnostic traces.  Use a
                 * normalize-local log-once guard distinct from
                 * mmap_truncated so the warning fires even when the
                 * raw-overflow path already set the flag. */
                if (!normalize_overflow_logged) {
                    serial_early_print("[WARN] Memory map normalize "
                                       "exceeds 512 entries -- "
                                       "truncating tail\n");
                    normalize_overflow_logged = 1;
                }
                g_boot_info_ptr->mmap_truncated = 1;
                g_boot_info_ptr->mmap_quirks = 1;
            }
        }

        if (s_mmap_events[i].is_start) {
            if (active_count < MMAP_NORMALIZE_MAX)
                s_mmap_active[active_count++] = s_mmap_events[i].desc_idx;
        } else {
            active_count = mmap_active_remove(active_count,
                                               s_mmap_events[i].desc_idx);
        }

        prev_addr = curr_addr;
        prev_valid = 1;
    }

    *count = out_count;
}

/* When the UEFI descriptor count exceeds BOOT_MMAP_MAX_ENTRIES, preserve
 * high-priority descriptors by evicting a lower-priority already-
 * accepted entry to make room for the incoming one.
 *
 * Eviction policy: cap handling in Phase 1 cannot use the sweep-line
 * carver to preserve non-overlapping fragments -- normalization runs
 * on the already-pruned set.  To avoid pathologically discarding a
 * large RAM block for a small non-overlapping high-priority entry,
 * or a small OVERLAPPING entry that only contests a tiny slice of a
 * large RAM block, rank every lower-priority candidate by the amount
 * of memory that would be lost if it were evicted whole:
 *
 *   loss = victim.length - overlap_length(victim, incoming)
 *
 * Pick the minimum loss.  Ties break to the first-seen candidate
 * (deterministic).  A fully contained victim yields loss == 0 and is
 * always preferred.  If no lower-priority candidate exists, drop the
 * incoming rather than clobber a peer or higher-priority entry.
 *
 * Returns 1 if the incoming descriptor should be inserted at `out_idx`,
 * 0 if the incoming descriptor should be skipped.  Updates *count on
 * successful eviction. */
static int mmap_evict_for_incoming(struct boot_mmap_entry *arr, UINT32 *count,
                                    UINT32 cap, int incoming_prio,
                                    UINT64 incoming_start, UINT64 incoming_end,
                                    UINT32 *out_idx)
{
    UINT32 j;
    int best_idx = -1;
    UINT64 best_loss = 0;

    if (*count < cap) {
        *out_idx = *count;
        return 1;
    }

    for (j = 0; j < *count; j++) {
        UINT64 e_start, e_end, ov_start, ov_end, ov_len, loss;
        int p = mmap_type_priority(arr[j].uefi_memory_type);
        if (p >= incoming_prio) continue;  /* not evictable */

        e_start = arr[j].base_addr;
        e_end   = e_start + arr[j].length;

        /* Compute intersection of [incoming_start, incoming_end) and
         * [e_start, e_end), clamped to zero if disjoint. */
        ov_start = (incoming_start > e_start) ? incoming_start : e_start;
        ov_end   = (incoming_end   < e_end)   ? incoming_end   : e_end;
        ov_len   = (ov_start < ov_end) ? (ov_end - ov_start) : 0;

        /* Memory lost if this victim is evicted whole (non-contested
         * portion of the victim). */
        loss = arr[j].length - ov_len;

        if (best_idx < 0 || loss < best_loss) {
            best_idx  = (int)j;
            best_loss = loss;
        }
    }

    if (best_idx < 0)
        return 0;  /* no evictable victim -- drop incoming */

    mmap_remove_at(arr, count, (UINT32)best_idx);
    g_boot_info_ptr->mmap_quirks = 1;
    serial_early_print("[WARN] mmap: cap reached -- evicted lower-priority entry (min-loss)\n");
    *out_idx = *count;
    return 1;
}

/* Memory-map geometry guard.  Both the initial GetMemoryMap consumer
 * AND the ExitBootServices-retry consumer must call this BEFORE
 * fill_memory_map() / fill_runtime_map() runs; without it,
 * desc_size=0 with map_size>0 in fill_runtime_map's loop spins
 * forever. */
static void mmap_geometry_validate(UINTN map_size, UINTN desc_size,
                                    const char *site)
{
    if (desc_size == 0 || desc_size < sizeof(EFI_MEMORY_DESCRIPTOR) ||
        map_size == 0 || map_size % desc_size != 0) {
        serial_early_print("[CRIT] mmap geometry invalid at ");
        serial_early_print(site);
        serial_early_print(" -- aborting\n");
        boot_fatal(BOOT_ERR_MMAP_GEOMETRY,
                   "Memory map geometry invalid",
                   "Descriptor size or map size is malformed.");
    }
}

/* Per-descriptor validation -- 5 checks shared between
 * fill_memory_map() and fill_runtime_map() so the runtime-services
 * handoff cannot consume firmware quirks that the regular mmap
 * already strips.  Returns 1 if
 * the descriptor is valid, 0 if invalid (and sets mmap_quirks=1 +
 * emits a [WARN] line tagged with `site`). */
static int mmap_descriptor_valid(EFI_MEMORY_DESCRIPTOR *desc,
                                  UINT32 entry_num,
                                  const char *site)
{
    UINT64 max_pages = 0xFFFFFFFFFFFFFULL;  /* UINT64_MAX / 4096 */
    if (desc->NumberOfPages > max_pages) {
        serial_early_print("[WARN] ");
        serial_early_print(site);
        serial_early_print(" entry ");
        serial_early_print_uint(entry_num);
        serial_early_print(": NumberOfPages overflow -- skipping\n");
        g_boot_info_ptr->mmap_quirks = 1;
        return 0;
    }
    UINT64 len = desc->NumberOfPages * EFI_PAGE_SIZE;
    if (desc->PhysicalStart > 0xFFFFFFFFFFFFFFFFULL - len) {
        serial_early_print("[WARN] ");
        serial_early_print(site);
        serial_early_print(" entry ");
        serial_early_print_uint(entry_num);
        serial_early_print(": address range wraps -- skipping\n");
        g_boot_info_ptr->mmap_quirks = 1;
        return 0;
    }
    if (desc->NumberOfPages == 0) {
        serial_early_print("[WARN] ");
        serial_early_print(site);
        serial_early_print(" entry ");
        serial_early_print_uint(entry_num);
        serial_early_print(": zero pages -- skipping\n");
        g_boot_info_ptr->mmap_quirks = 1;
        return 0;
    }
    if (desc->PhysicalStart & 0xFFF) {
        serial_early_print("[WARN] ");
        serial_early_print(site);
        serial_early_print(" entry ");
        serial_early_print_uint(entry_num);
        serial_early_print(": unaligned PhysicalStart -- skipping\n");
        g_boot_info_ptr->mmap_quirks = 1;
        return 0;
    }
    if (desc->Type >= EfiMaxMemoryType) {
        serial_early_print("[WARN] ");
        serial_early_print(site);
        serial_early_print(" entry ");
        serial_early_print_uint(entry_num);
        serial_early_print(": invalid type 0x");
        serial_early_print_hex16((UINT16)desc->Type);
        serial_early_print(" -- skipping\n");
        g_boot_info_ptr->mmap_quirks = 1;
        return 0;
    }
    return 1;
}

static void fill_memory_map(EFI_MEMORY_DESCRIPTOR *mmap,
                             UINTN map_size, UINTN desc_size)
{
    UINTN offset;
    UINT32 idx = 0;
    UINT64 total_mem = 0;
    UINT32 total_descs;

    /* Guard against malformed descriptor geometry (S12) */
    if (desc_size == 0 || desc_size < sizeof(EFI_MEMORY_DESCRIPTOR)) {
        serial_early_print("[FAIL] Memory map: invalid descriptor size\n");
        g_boot_info_ptr->mmap_count = 0;
        return;
    }

    total_descs = (UINT32)(map_size / desc_size);

    if (total_descs > BOOT_MMAP_MAX_ENTRIES) {
        serial_early_print("[WARN] Memory map has ");
        serial_early_print_uint(total_descs);
        serial_early_print(" entries, capped at ");
        serial_early_print_uint(BOOT_MMAP_MAX_ENTRIES);
        serial_early_print(" (priority eviction active)\n");
        g_boot_info_ptr->mmap_truncated = 1;
    } else {
        g_boot_info_ptr->mmap_truncated = 0;
    }

    g_boot_info_ptr->mmap_quirks = 0;

    /* Phase 1: validate and copy raw descriptors into boot_info->mmap.
     * Overlap resolution is deferred to mmap_normalize() below so the
     * fill loop does not silently drop ACPI Reclaim / BootServices
     * overlaps that firmware can legitimately emit.  If the UEFI map
     * overflows BOOT_MMAP_MAX_ENTRIES, evict the lowest-priority
     * already-accepted entry to keep higher-priority ranges. */
    for (offset = 0; offset < map_size; offset += desc_size) {
        EFI_MEMORY_DESCRIPTOR *desc =
            (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)mmap + offset);
        UINT32 entry_num = (UINT32)(offset / desc_size);
        UINT32 dest_idx;
        int incoming_prio;

        /* Per-descriptor validation -- shared with fill_runtime_map
         * via mmap_descriptor_valid() so both passes strip the same
         * firmware quirks. */
        if (!mmap_descriptor_valid(desc, entry_num, "Memory map"))
            continue;

        /* Priority-aware cap handling: at capacity, evict a lower-
         * priority existing entry to make room for the incoming one.
         * Eviction prefers an overlapping victim and falls back to the
         * smallest lower-priority entry so a small Reserved descriptor
         * cannot pathologically discard a large Conventional RAM
         * region that does not overlap it.  At or below cap, append
         * at idx. */
        {
            UINT64 desc_len = desc->NumberOfPages * EFI_PAGE_SIZE;
            UINT64 desc_start = desc->PhysicalStart;
            UINT64 desc_end = desc_start + desc_len;
            incoming_prio = mmap_type_priority(desc->Type);
            if (!mmap_evict_for_incoming(g_boot_info_ptr->mmap, &idx,
                                          BOOT_MMAP_MAX_ENTRIES,
                                          incoming_prio,
                                          desc_start, desc_end,
                                          &dest_idx)) {
                g_boot_info_ptr->mmap_quirks = 1;
                continue;  /* no evictable victim -- drop incoming */
            }
        }

        g_boot_info_ptr->mmap[dest_idx].base_addr = desc->PhysicalStart;
        g_boot_info_ptr->mmap[dest_idx].length =
            desc->NumberOfPages * EFI_PAGE_SIZE;
        g_boot_info_ptr->mmap[dest_idx].type =
            uefi_to_mb2_memtype(desc->Type);
        g_boot_info_ptr->mmap[dest_idx].uefi_memory_type = desc->Type;
        g_boot_info_ptr->mmap[dest_idx].attribute = desc->Attribute;
        idx++;
    }

    /* Phase 2: sort by base address and resolve overlaps by priority. */
    mmap_normalize(g_boot_info_ptr->mmap, &idx, BOOT_MMAP_MAX_ENTRIES);

    /* Phase 3: recompute total_mem from the NORMALIZED map so
     * mem_upper_kb reflects any carving.  A Conventional range that
     * was shrunk or split by higher-priority overlays must not be
     * counted at its pre-normalize length. */
    {
        UINT32 k;
        for (k = 0; k < idx; k++) {
            switch (g_boot_info_ptr->mmap[k].uefi_memory_type) {
            case EfiConventionalMemory:
            case EfiBootServicesCode:
            case EfiBootServicesData:
            case EfiLoaderCode:
            case EfiLoaderData:
                total_mem += g_boot_info_ptr->mmap[k].length;
                break;
            default:
                break;
            }
        }
    }

    g_boot_info_ptr->mmap_count = idx;
    g_boot_info_ptr->mem_lower_kb = 640;   /* conventional: 640 KiB */
    if (total_mem >= (1024ULL * 1024ULL)) {
        g_boot_info_ptr->mem_upper_kb =
            (UINT32)((total_mem / 1024ULL) - 1024ULL);
    } else {
        g_boot_info_ptr->mem_upper_kb = 0;
    }
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

    /* Internal geometry guard -- callers in efi_main and the EBS
     * retry path are expected to call mmap_geometry_validate() too,
     * but a defense-in-depth check here means desc_size==0 cannot
     * spin this loop forever even if a future call site forgets
     * the upstream guard. */
    if (desc_size == 0 || desc_size < sizeof(EFI_MEMORY_DESCRIPTOR) ||
        map_size == 0 || map_size % desc_size != 0) {
        boot_fatal(BOOT_ERR_MMAP_GEOMETRY,
                   "Runtime map geometry invalid",
                   "Descriptor size or map size is malformed.");
    }

    int rt_cap_warned = 0;
    /* Scan ALL descriptors -- do NOT stop at idx < BOOT_RT_MMAP_MAX.
     * Stopping early would skip validation on remaining descriptors
     * (later quirks would not set mmap_quirks or produce warnings)
     * and would silently truncate the runtime view.  When the cap is
     * reached, log once, set mmap_quirks, and keep scanning for
     * validation purposes. */
    for (offset = 0; offset < map_size; offset += desc_size) {
        EFI_MEMORY_DESCRIPTOR *desc =
            (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)mmap + offset);
        UINT32 entry_num = (UINT32)(offset / desc_size);

        if (desc->Type != EfiRuntimeServicesCode &&
            desc->Type != EfiRuntimeServicesData)
            continue;

        /* Apply the SAME validation the regular mmap pass uses --
         * otherwise rt_mmap could keep firmware quirks (zero-page,
         * overflow, wrap, unaligned, invalid-type) that mmap[]
         * stripped, leaving the kernel UEFI runtime setup with
         * inconsistent views. */
        if (!mmap_descriptor_valid(desc, entry_num, "Runtime map"))
            continue;

        if (idx >= BOOT_RT_MMAP_MAX) {
            if (!rt_cap_warned) {
                serial_early_print("[WARN] Runtime map cap reached "
                                   "-- dropping remaining runtime "
                                   "entries; SetVirtualAddressMap "
                                   "input incomplete\n");
                g_boot_info_ptr->mmap_quirks = 1;
                rt_cap_warned = 1;
            }
            continue;  /* keep scanning so later quirks still warn */
        }

        g_boot_info_ptr->rt_mmap[idx].phys_addr  = desc->PhysicalStart;
        g_boot_info_ptr->rt_mmap[idx].num_pages  = desc->NumberOfPages;
        g_boot_info_ptr->rt_mmap[idx].attribute  = desc->Attribute;
        g_boot_info_ptr->rt_mmap[idx].type       = desc->Type;
        g_boot_info_ptr->rt_mmap[idx].reserved   = 0;
        idx++;
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

/* Unaligned-safe little-endian byte loads for the firmware event-log buffer. */
static UINT16 tpm_bl_le16(const UINT8 *p)
{
    return (UINT16)((UINT16)p[0] | ((UINT16)p[1] << 8));
}
static UINT32 tpm_bl_le32(const UINT8 *p)
{
    return (UINT32)p[0] | ((UINT32)p[1] << 8)
         | ((UINT32)p[2] << 16) | ((UINT32)p[3] << 24);
}

/* TCG algorithm id -> digest length in bytes; 0 = unknown. */
static UINT16 tpm_bl_alg_len(UINT16 alg_id)
{
    switch (alg_id) {
        case 0x0004: return 20;  /* SHA-1   */
        case 0x000B: return 32;  /* SHA-256 */
        case 0x000C: return 48;  /* SHA-384 */
        case 0x000D: return 64;  /* SHA-512 */
        default:     return 0;
    }
}

/* Compute the EXACT total event-log size by sizing the final event. The TCG2
 * protocol gives the final event's START offset but not the buffer end, so the
 * legacy code padded the copy with a 256-byte guess -- which truncates a large
 * final event (e.g. a big EV_EFI_VARIABLE db/dbx record). Parse the final
 * event's own length instead. Returns the byte length from log start through
 * the end of the final event, or 0 when it cannot be sized within `cap` (the
 * caller then degrades the capability rather than shipping a guessed copy).
 * Every read is bounded by `cap`, and reads only touch the final event's small
 * header fields (count / alg ids / data-size), all within the firmware buffer
 * when the firmware did not report truncation. */
static UINT64 tpm_compute_log_size(const UINT8 *log, UINT64 last_off,
                                   UINT8 tpm_ver, UINT64 cap)
{
    if (last_off >= cap) return 0;

    if (tpm_ver == 2 && last_off != 0) {
        /* Final entry is TCG_PCR_EVENT2: pcr(4) event_type(4)
         * TPML_DIGEST_VALUES{ count(4) [alg(2) digest[]]* }
         * event_data_size(4) event_data[]. */
        UINT64 p = last_off;
        if (p + 12ull > cap) return 0;
        UINT32 count = tpm_bl_le32(log + p + 8ull);
        if (count > 8u) return 0;
        p += 12ull;
        UINT32 c;
        for (c = 0; c < count; c++) {
            if (p + 2ull > cap) return 0;
            UINT16 alg = tpm_bl_le16(log + p);
            UINT16 dsz = tpm_bl_alg_len(alg);
            if (dsz == 0) return 0;
            if (p + 2ull + (UINT64)dsz > cap) return 0;
            p += 2ull + (UINT64)dsz;
        }
        if (p + 4ull > cap) return 0;
        UINT32 dsize = tpm_bl_le32(log + p);
        p += 4ull + (UINT64)dsize;
        if (p > cap) return 0;
        return p;
    }

    /* Final entry is TCG_PCR_EVENT (TPM 1.2, or the lone first event):
     * pcr(4) event_type(4) digest[20] event_data_size(4) event_data[]. */
    if (last_off + 32ull > cap) return 0;
    UINT32 dsize = tpm_bl_le32(log + last_off + 28ull);
    UINT64 end = last_off + 32ull + (UINT64)dsize;
    if (end > cap) return 0;
    return end;
}

/* Return a hard CEILING for event-log reads: the byte count from `addr` to the end
 * of the UEFI memory-map descriptor that contains it, capped at TPM_EVENT_LOG_MAX.
 * This is a region-boundary guard, NOT a buffer-size record -- the descriptor is a
 * memory-type classification and a short log pool can sit inside a larger one, so
 * the caller must still derive the real length from the event records and use this
 * only to keep the parser from crossing into a different region. Returns 0 when no
 * descriptor covers `addr` (caller fails closed). The map is snapshotted + freed
 * here; this runs well before the ExitBootServices map_key is acquired, so it does
 * not disturb that path. */
static UINT64 tpm_log_mmap_extent(EFI_PHYSICAL_ADDRESS addr)
{
    UINTN  map_key = 0, map_size = 0, desc_size = 0, off;
    UINT32 desc_version = 0;
    EFI_MEMORY_DESCRIPTOR *map = (EFI_MEMORY_DESCRIPTOR *)0;
    UINT64 extent = 0;

    if (EFI_ERROR(get_memory_map(&map_key, &map, &map_size, &desc_size,
                                 &desc_version)) || !map)
        return 0;
    if (desc_size >= sizeof(EFI_MEMORY_DESCRIPTOR) && map_size != 0) {
        for (off = 0; off + desc_size <= map_size; off += desc_size) {
            const EFI_MEMORY_DESCRIPTOR *d =
                (const EFI_MEMORY_DESCRIPTOR *)((const UINT8 *)map + off);
            UINT64 start  = d->PhysicalStart;
            UINT64 npages = d->NumberOfPages;
            /* Trust the descriptor only when npages*PAGE_SIZE cannot overflow. */
            if (npages == 0 || npages > (0xFFFFFFFFFFFFFFFFull / EFI_PAGE_SIZE))
                continue;
            UINT64 end = start + npages * (UINT64)EFI_PAGE_SIZE;
            if (end <= start) continue;                      /* wrap guard */
            if ((UINT64)addr >= start && (UINT64)addr < end) {
                extent = end - (UINT64)addr;
                if (extent > (UINT64)TPM_EVENT_LOG_MAX)
                    extent = (UINT64)TPM_EVENT_LOG_MAX;
                break;
            }
        }
    }
    gBS->FreePool(map);
    return extent;
}

/* TPM is present but its event log is unavailable (retrieval failed, unsizable, or
 * the copy buffer could not be allocated): publish a present-but-degraded state so
 * the kernel still runs TPM-present paths while skipping the absent event-log parse.
 * tpm_available stays 0 ONLY for an actually-absent TPM (no protocol / capability
 * probe failed / !TPMPresentFlag), which the callers below set directly. */
static void tpm_publish_log_degraded(UINT8 tpm_ver)
{
    g_boot_info_ptr->tpm_available      = 1;
    g_boot_info_ptr->tpm_version        = tpm_ver;
    g_boot_info_ptr->tpm_event_log      = 0;
    g_boot_info_ptr->tpm_event_log_size = 0;
    g_boot_info_ptr->tpm_event_count    = 0;
}

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
        serial_early_print("[BOOT] TPM: GetEventLog failed; log degraded\n");
        tpm_publish_log_degraded(tpm_ver);
        return;
    }

    /* Size the copy from the EVENT RECORDS themselves, never from a guess. The
     * TCG2 protocol gives the final event's start offset but not the buffer end or
     * the allocation length, so the ONLY trustworthy size is the one
     * tpm_compute_log_size() derives by parsing the final event's own length
     * fields. The memory-map extent (cap) is a secondary hard stop on those parse
     * reads -- a firmware that reports !truncated can still place a short log pool
     * inside a larger memory descriptor, so the descriptor extent is NOT proof of
     * the buffer length, only a ceiling that keeps the parser from crossing into a
     * different region. When the firmware reports truncation, or the final event
     * cannot be sized within that ceiling, the true length is unknown: publish NO
     * copied buffer (a guessed copy would read adjacent firmware memory) and degrade
     * BOOT_CAP_TPM_EVENT_LOG so the kernel skips the parse. */
    UINT64 cap = tpm_log_mmap_extent(log_location);
    UINT64 log_size = 0;
    /* log_last_entry < log_location is a malformed firmware response (the final
     * entry cannot precede the buffer start); fail closed rather than sizing the
     * first record as if it were the whole log. log_last_entry == log_location is
     * the legitimate one-record case (last_off == 0). */
    if (cap >= 32ull && !log_truncated && log_last_entry >= log_location) {
        UINT64 last_off = log_last_entry - log_location;
        UINT64 exact = tpm_compute_log_size((const UINT8 *)(UINTN)log_location,
                                            last_off, tpm_ver, cap);
        if (exact >= 32ull) log_size = exact;   /* record-derived, within the ceiling */
    }
    if (log_size == 0ull) {
        serial_early_print("[WARN] TPM: event log unsizable from records; "
                           "capability degraded, no buffer published\n");
        tpm_publish_log_degraded(tpm_ver);
        return;
    }

    /* Allocate a PAGE-ALIGNED copy (firmware may reclaim the original after
     * ExitBootServices). Page alignment lets the reserved payload descriptor
     * retain exactly the log's pages in the kernel PMM. */
    UINTN log_pages = ((UINTN)log_size + EFI_PAGE_SIZE - 1u) / EFI_PAGE_SIZE;
    EFI_PHYSICAL_ADDRESS log_addr = 0;
    status = gBS->AllocatePages(AllocateAnyPages, EfiLoaderData,
                                log_pages, &log_addr);
    if (EFI_ERROR(status) || !log_addr) {
        serial_early_print("[BOOT] TPM: failed to allocate log buffer; log degraded\n");
        tpm_publish_log_degraded(tpm_ver);
        return;
    }
    VOID *log_copy = (VOID *)(UINTN)log_addr;

    /* Copy event log data */
    UINT8 *dst = (UINT8 *)log_copy;
    UINT8 *src = (UINT8 *)(UINTN)log_location;
    UINTN i;
    for (i = 0; i < (UINTN)log_size; i++)
        dst[i] = src[i];

    /* Approximate event count for early serial diagnostics only; the kernel
     * does the authoritative crypto-agile parse + count. */
    UINT16 event_count = 0;
    UINTN offset = 0;
    while (offset + 32 < (UINTN)log_size) {
        UINT32 event_data_size = tpm_bl_le32(dst + offset + 28);
        UINTN entry_size = 32 + event_data_size;
        if (entry_size < 32 || offset + entry_size > (UINTN)log_size)
            break;
        event_count++;
        if (event_count == 1 && tpm_ver == 2)
            break;  /* remaining entries are EVENT2; kernel parses those */
        offset += entry_size;
    }

    /* Store in boot_info. log_size is record-derived (the unsizable path returned
     * already), so the published size always matches the copied bytes exactly. */
    g_boot_info_ptr->tpm_event_log      = (UINT64)(UINTN)log_copy;
    g_boot_info_ptr->tpm_event_log_size = (UINT32)log_size;
    g_boot_info_ptr->tpm_available      = 1;
    g_boot_info_ptr->tpm_version        = tpm_ver;
    g_boot_info_ptr->tpm_event_count    = event_count;

    /* Retention: the kernel's boot_reserved path already pins the
     * [tpm_event_log, +tpm_event_log_size) range from PMM whenever
     * BOOT_CAP_TPM_EVENT_LOG is present (which is exactly the exact-size case
     * above). Publishing a second BOOT_PAYLOAD_TPM_EVENT_LOG descriptor with
     * BOOT_PAYLOAD_FLAG_RESERVED would reserve the same range twice and fatal
     * in boot_reserved overlap detection, so the typed descriptor is
     * intentionally NOT emitted -- the legacy fields are the single retention
     * path. */
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
 * Early entropy collection (TODO-12 early-entropy S2)
 *
 * Collects firmware entropy pre-ExitBootServices into a page-aligned
 * EfiLoaderData buffer and publishes it as a BOOT_PAYLOAD_RANDOM_SEED
 * descriptor with FLAG_RESERVED (same memory-ownership convention as the
 * boot.conf payloads -- without the descriptor the kernel PMM would
 * reclaim the page with raw seed bytes still in it).
 *
 * Payload layout (boot_info seed handoff section): a 32-byte header
 * (struct bl_seed_header -- mirror of struct entropy_seed_header in
 * include/kernel/entropy.h, layout pinned by static asserts on both
 * sides) followed by transcript_len bytes of framed records. The
 * descriptor carries FLAG_CHECKSUMMED with CRC-32C of the WHOLE payload
 * in the checksum low 32 bits. mask/quality in the header are ADVISORY;
 * the kernel re-derives credit from the records it accepts.
 *
 * Transcript format mirrors include/kernel/entropy.h framing:
 *   u8 src_id | u32 len (LE) | payload
 * src ids: 0 = firmware RNG (EFI_RNG_PROTOCOL, 64 bytes),
 *          1 = CPU RDSEED/RDRAND (64 RNG bytes + 16 personalization
 *              bytes: CPUID vendor EBX,EDX,ECX order + FMS dword),
 *          3 = ACPI OEM0 table payload (up to 512 bytes),
 *          4 = seed-file carryover (raw 80-byte X:\Boot\random-seed.*
 *              blob, read pre-EBS from the same-disk BLACKBOX volume;
 *              NOT verified here -- the kernel checks MAC + counter
 *              against the NVRAM token and fails closed),
 *          7 = boot timing personalization (bl_entry, reset_end,
 *              tsc_freq, rdtsc-now -- 32 bytes, LOW/uncredited).
 *
 * Earlier-stage concatenation (Linux EFI config-table parity): an
 * already-published RANDOM_SEED descriptor from a prior chain stage is
 * left untouched; ours is appended as an additional descriptor and the
 * kernel mixes every one.
 *
 * Hang contract: a firmware GetRNG that never returns cannot be recovered
 * pre-EBS (no preemption). The boot.conf escape hatch firmware_rng=off
 * skips the protocol entirely; GetInfo is skipped as nonessential and
 * GetRNG is called exactly once with the firmware-default algorithm
 * (NULL) per UEFI 2.10 37.5.2.
 * ============================================================================ */
#define BL_ENTROPY_SRC_FW_RNG    0u
#define BL_ENTROPY_SRC_CPU_RNG   1u
#define BL_ENTROPY_SRC_ACPI_OEM0 3u
#define BL_ENTROPY_SRC_SEED_FILE 4u
#define BL_ENTROPY_SRC_TIME      7u
#define BL_ENTROPY_FW_BYTES      64u
#define BL_ENTROPY_CPU_QWORDS    8u    /* 64 RNG bytes */
#define BL_ENTROPY_OEM0_CAP      512u
#define BL_SEED_FILE_SIZE        80u   /* on-disk blob, kernel seed_file.h */

/* Advisory per-source quality classes (mirror of entropy_quality_t). */
#define BL_ENTROPY_Q_LOW         1u
#define BL_ENTROPY_Q_HIGH        2u

/* In-payload seed header -- mirror of struct entropy_seed_header in
 * include/kernel/entropy.h. Layout pinned on BOTH sides; bump
 * BL_SEED_VERSION together with ENTROPY_SEED_VERSION. */
#define BL_SEED_MAGIC            0x53525049u  /* "IPRS" little-endian */
#define BL_SEED_VERSION          1u

struct bl_seed_header {
    UINT32 magic;
    UINT32 version;
    UINT32 source_mask;
    UINT32 quality;          /* 2 bits per source id */
    UINT32 transcript_len;   /* framed bytes after this header */
    UINT32 reserved[3];
};

_Static_assert(sizeof(struct bl_seed_header) == 32,
    "seed header is a bootloader-kernel handoff -- 32 bytes exactly");
_Static_assert(__builtin_offsetof(struct bl_seed_header, transcript_len) == 16,
    "transcript_len at offset 16 -- kernel entropy_seed_header twin");
/* Per-qword RDSEED retry budget. RDSEED legitimately underflows when the
 * DRNG conditioner is drained; Intel DRG guide recommends pause+retry.
 * 1024 spins of pause is far below 1ms even on slow cores, so the
 * pre-EBS latency is bounded while still riding out normal underflow. */
#define BL_RDSEED_RETRIES        1024u
#define BL_RDRAND_RETRIES        10u
#define POST16_BL_ENTROPY        0xB034
#define POST16_BL_ENTROPY_OK     0xB035

static UINTN bl_entropy_frame(UINT8 *buf, UINTN cap, UINTN pos,
                              UINT8 src, const UINT8 *data, UINT32 len);
/* Same-disk BLACKBOX volume locator (defined with the media-role block
 * below; reused here for the pre-EBS seed-file carryover read). */
static EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *
media_role_locate_blackbox_fs(EFI_HANDLE boot_part_handle);

/* CPUID with subleaf -- max-leaf gating is the CALLER's job. */
static void bl_cpuid(UINT32 leaf, UINT32 subleaf,
                     UINT32 *eax, UINT32 *ebx, UINT32 *ecx, UINT32 *edx)
{
    UINT32 a, b, c, d;
    __asm__ volatile ("cpuid"
                      : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                      : "a"(leaf), "c"(subleaf));
    if (eax) *eax = a;
    if (ebx) *ebx = b;
    if (ecx) *ecx = c;
    if (edx) *edx = d;
}

/* One RDSEED attempt with bounded pause-retry. Returns 1 on success. */
static int bl_rdseed64(UINT64 *out)
{
    UINT32 tries = BL_RDSEED_RETRIES;
    while (tries-- > 0) {
        UINT64 v;
        UINT8 ok;
        __asm__ volatile ("rdseed %0; setc %1"
                          : "=r"(v), "=qm"(ok) : : "cc");
        if (ok) { *out = v; return 1; }
        __asm__ volatile ("pause");
    }
    return 0;
}

/* RDRAND with the Intel-recommended 10-retry loop. Returns 1 on success. */
static int bl_rdrand64(UINT64 *out)
{
    UINT32 tries = BL_RDRAND_RETRIES;
    while (tries-- > 0) {
        UINT64 v;
        UINT8 ok;
        __asm__ volatile ("rdrand %0; setc %1"
                          : "=r"(v), "=qm"(ok) : : "cc");
        if (ok) { *out = v; return 1; }
        __asm__ volatile ("pause");
    }
    return 0;
}

/* Collect CPU RNG entropy: RDSEED preferred (fully conditioned seed
 * grade), RDRAND fallback. CPUID gates follow the kernel cpuid.c
 * discipline: leaf 0 max-basic first, leaf absent = feature absent.
 * The whole sample is rejected when the DRNG looks stuck (all-zero or
 * all qwords identical). On acceptance, the CPU identity (vendor string
 * + family/model/stepping dword) is APPENDED to the same record as
 * non-secret personalization -- never emitted standalone, so a framed
 * src-1 record always implies real RNG output came with it. Returns the
 * new transcript position, or pos unchanged on degrade. */
static UINTN bl_collect_cpu_rng(UINT8 *seed, UINTN cap, UINTN pos)
{
    UINT32 max_basic, ebx, ecx, edx;
    int has_rdseed = 0, has_rdrand = 0;

    bl_cpuid(0, 0, &max_basic, &ebx, &ecx, &edx);
    if (max_basic >= 7) {
        UINT32 f_ebx;
        bl_cpuid(7, 0, (UINT32 *)0, &f_ebx, (UINT32 *)0, (UINT32 *)0);
        has_rdseed = (f_ebx >> 18) & 1u;
    }
    if (max_basic >= 1) {
        UINT32 f_ecx;
        bl_cpuid(1, 0, (UINT32 *)0, (UINT32 *)0, &f_ecx, (UINT32 *)0);
        has_rdrand = (f_ecx >> 30) & 1u;
    }
    if (!has_rdseed && !has_rdrand) {
        serial_early_print("[BOOT] RNG: no RDSEED/RDRAND (CPUID)\n");
        return pos;
    }

    UINT64 q[BL_ENTROPY_CPU_QWORDS];
    UINTN i;
    int used_rdseed = has_rdseed;
    for (i = 0; i < BL_ENTROPY_CPU_QWORDS; i++) {
        int ok = 0;
        if (has_rdseed)
            ok = bl_rdseed64(&q[i]);
        if (!ok && has_rdrand) {
            ok = bl_rdrand64(&q[i]);
            if (ok)
                used_rdseed = 0;   /* ANY fallback makes the sample RDRAND-grade */
        }
        if (!ok) {
            serial_early_print("[BOOT] RNG: CPU DRNG exhausted -- degraded\n");
            efi_memset(q, 0, sizeof(q));
            return pos;
        }
    }

    /* Stuck-DRNG heuristics: all-zero output or every qword identical
     * (catches 0xFF..FF fill too, since all 8 would match). */
    {
        int all_zero = 1, all_same = 1;
        for (i = 0; i < BL_ENTROPY_CPU_QWORDS; i++) {
            if (q[i] != 0) all_zero = 0;
            if (q[i] != q[0]) all_same = 0;
        }
        if (all_zero || all_same) {
            serial_early_print("[BOOT] RNG: CPU DRNG output rejected (stuck)\n");
            efi_memset(q, 0, sizeof(q));
            return pos;
        }
    }

    /* Payload = 64 RNG bytes + 16 bytes non-secret personalization
     * (CPUID vendor string + family/model/stepping dword). */
    UINT8 payload[BL_ENTROPY_CPU_QWORDS * 8 + 16];
    for (i = 0; i < BL_ENTROPY_CPU_QWORDS; i++) {
        UINTN b;
        for (b = 0; b < 8; b++)
            payload[i * 8 + b] = (UINT8)(q[i] >> (b * 8));
    }
    {
        UINT32 v_ebx, v_ecx, v_edx, fms;
        bl_cpuid(0, 0, (UINT32 *)0, &v_ebx, &v_ecx, &v_edx);
        bl_cpuid(1, 0, &fms, (UINT32 *)0, (UINT32 *)0, (UINT32 *)0);
        UINT8 *p = payload + BL_ENTROPY_CPU_QWORDS * 8;
        UINTN b;
        for (b = 0; b < 4; b++) p[b]      = (UINT8)(v_ebx >> (b * 8));
        for (b = 0; b < 4; b++) p[4 + b]  = (UINT8)(v_edx >> (b * 8));
        for (b = 0; b < 4; b++) p[8 + b]  = (UINT8)(v_ecx >> (b * 8));
        for (b = 0; b < 4; b++) p[12 + b] = (UINT8)(fms >> (b * 8));
    }

    UINTN np = bl_entropy_frame(seed, cap, pos, BL_ENTROPY_SRC_CPU_RNG,
                                payload, (UINT32)sizeof(payload));
    efi_memset(q, 0, sizeof(q));
    efi_memset(payload, 0, sizeof(payload));
    if (np == 0)
        return pos;
    serial_early_print(used_rdseed ? "[BOOT] RNG: RDSEED 64 bytes\n"
                                   : "[BOOT] RNG: RDRAND 64 bytes\n");
    return np;
}

/* Find an ACPI SDT by 4-char signature via RSDP -> XSDT/RSDT walk.
 * Same validation discipline as serial_spcr_probe(): signature +
 * checksum gates before trusting any pointer, length bounds before
 * dereference. Returns NULL when absent or malformed. */
static const BL_ACPI_SDT_HDR *bl_find_acpi_table(const char sig[4])
{
    UINTN i;
    EFI_GUID acpi20_guid = EFI_ACPI_20_TABLE_GUID;
    EFI_GUID acpi10_guid = EFI_ACPI_TABLE_GUID;
    BL_ACPI_RSDP *rsdp = (BL_ACPI_RSDP *)0;

    if (!gST || gST->NumberOfTableEntries == 0)
        return (const BL_ACPI_SDT_HDR *)0;

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
        return (const BL_ACPI_SDT_HDR *)0;
    if (rsdp->signature[0] != 'R' || rsdp->signature[1] != 'S' ||
        rsdp->signature[2] != 'D' || rsdp->signature[3] != ' ' ||
        rsdp->signature[4] != 'P' || rsdp->signature[5] != 'T' ||
        rsdp->signature[6] != 'R' || rsdp->signature[7] != ' ')
        return (const BL_ACPI_SDT_HDR *)0;
    if (!acpi_checksum_ok(rsdp, 20))
        return (const BL_ACPI_SDT_HDR *)0;

    /* Prefer XSDT (64-bit entries) when revision >= 2 and the extended
     * checksum holds; otherwise fall back to RSDT (32-bit entries). */
    UINT64 sdt_addr = 0;
    UINTN entry_size = 4;
    if (rsdp->revision >= 2 &&
        rsdp->length == sizeof(BL_ACPI_RSDP) &&
        acpi_checksum_ok(rsdp, rsdp->length) &&
        rsdp->xsdt_addr != 0) {
        sdt_addr = rsdp->xsdt_addr;
        entry_size = 8;
    } else if (rsdp->rsdt_addr != 0) {
        sdt_addr = rsdp->rsdt_addr;
        entry_size = 4;
    }
    if (sdt_addr == 0)
        return (const BL_ACPI_SDT_HDR *)0;

    const BL_ACPI_SDT_HDR *root = (const BL_ACPI_SDT_HDR *)(UINTN)sdt_addr;
    /* The root MUST actually be an XSDT/RSDT before its body is treated
     * as a pointer array -- a checksum-valid but wrong SDT here would
     * have its payload dereferenced as addresses. */
    if (entry_size == 8) {
        if (root->signature[0] != 'X' || root->signature[1] != 'S' ||
            root->signature[2] != 'D' || root->signature[3] != 'T')
            return (const BL_ACPI_SDT_HDR *)0;
    } else {
        if (root->signature[0] != 'R' || root->signature[1] != 'S' ||
            root->signature[2] != 'D' || root->signature[3] != 'T')
            return (const BL_ACPI_SDT_HDR *)0;
    }
    if (root->length < 36 || root->length > (1024u * 1024u))
        return (const BL_ACPI_SDT_HDR *)0;
    if (!acpi_checksum_ok(root, root->length))
        return (const BL_ACPI_SDT_HDR *)0;

    UINTN count = (root->length - 36) / entry_size;
    const UINT8 *entries = (const UINT8 *)root + 36;
    for (i = 0; i < count; i++) {
        UINT64 addr = 0;
        UINTN b;
        /* unaligned-safe little-endian entry read (4 or 8 bytes) */
        for (b = 0; b < entry_size; b++)
            addr |= (UINT64)entries[i * entry_size + b] << (b * 8);
        if (addr == 0)
            continue;
        const BL_ACPI_SDT_HDR *t = (const BL_ACPI_SDT_HDR *)(UINTN)addr;
        if (t->signature[0] != sig[0] || t->signature[1] != sig[1] ||
            t->signature[2] != sig[2] || t->signature[3] != sig[3])
            continue;
        if (t->length < 36 || t->length > (1024u * 1024u))
            continue;
        if (!acpi_checksum_ok(t, t->length))
            continue;
        return t;
    }
    return (const BL_ACPI_SDT_HDR *)0;
}

/* Append one framed record (u8 src | u32 len LE | payload) to the seed
 * transcript. Returns new position, 0 when it would not fit (caller
 * treats as hard failure -- truncated seed records are forbidden). */
static UINTN bl_entropy_frame(UINT8 *buf, UINTN cap, UINTN pos,
                              UINT8 src, const UINT8 *data, UINT32 len)
{
    UINTN need = 1u + 4u + (UINTN)len;
    UINTN k;
    /* Same refusal contract as the kernel framing twin
     * (entropy_frame_source): NULL pointers and out-of-vocabulary
     * source ids are refused before any byte is written. 8 = source
     * class count in include/kernel/entropy.h. */
    if (!buf || !data || src >= 8u)
        return 0;
    if (len == 0 || pos > cap || need > cap - pos)
        return 0;
    buf[pos++] = src;
    buf[pos++] = (UINT8)(len & 0xFF);
    buf[pos++] = (UINT8)((len >> 8) & 0xFF);
    buf[pos++] = (UINT8)((len >> 16) & 0xFF);
    buf[pos++] = (UINT8)((len >> 24) & 0xFF);
    for (k = 0; k < len; k++)
        buf[pos++] = data[k];
    return pos;
}

/* CRC-32C (Castagnoli, reflected 0x82F63B78), bitwise. Twin of the
 * kernel's entropy_crc32c() in src/kernel/entropy.c (mirror pattern --
 * the bootloader cannot link kernel objects). One-shot use on a <= 4 KiB
 * payload; no table needed pre-EBS. */
static UINT32 bl_crc32c(const UINT8 *data, UINTN len)
{
    UINT32 crc = 0xFFFFFFFFu;
    UINTN i;
    int b;

    for (i = 0; i < len; i++) {
        crc ^= data[i];
        for (b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0x82F63B78u & (0u - (crc & 1u)));
    }
    return crc ^ 0xFFFFFFFFu;
}

/* Read one seed-file candidate from an already-open volume root. Frames
 * the raw blob as a src-4 record ONLY when the file is exactly
 * BL_SEED_FILE_SIZE bytes (anything else is torn/corrupt -- the kernel
 * would reject it on length anyway, so skip the framing). Returns the
 * new transcript position, or 'pos' unchanged when absent/short/oversize.
 * The blob is NOT verified here: MAC + anti-replay need the NVRAM token
 * secret, and that check (plus the fail-closed policy) is kernel-side. */
static UINTN bl_seed_read_candidate(EFI_FILE_PROTOCOL *root,
                                    const CHAR16 *path,
                                    UINT8 *seed, UINTN cap, UINTN pos)
{
    EFI_FILE_PROTOCOL *file = (EFI_FILE_PROTOCOL *)0;
    UINT8 blob[BL_SEED_FILE_SIZE + 1u];  /* +1 detects oversize */
    UINTN read_len = sizeof(blob);
    EFI_STATUS status;

    status = root->Open(root, &file, (CHAR16 *)path, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status) || !file)
        return pos;

    status = file->Read(file, &read_len, blob);
    file->Close(file);
    if (EFI_ERROR(status) || read_len != (UINTN)BL_SEED_FILE_SIZE) {
        /* Short read = torn write or wrong file; oversize read (cap+1
         * filled) = not our format. Either way: no record. */
        efi_memset(blob, 0, sizeof(blob));
        serial_early_print("[BOOT] RNG: seed file unreadable or wrong size -- skipped\n");
        return pos;
    }

    {
        UINTN np = bl_entropy_frame(seed, cap, pos, BL_ENTROPY_SRC_SEED_FILE,
                                    blob, BL_SEED_FILE_SIZE);
        efi_memset(blob, 0, sizeof(blob));
        if (np != 0) {
            serial_early_print("[BOOT] RNG: seed file carryover 80 bytes (kernel verifies)\n");
            return np;
        }
    }
    return pos;
}

/* Pre-EBS read of the kernel's seed carryover files from the same-disk
 * BLACKBOX volume (early first-seed boundary: the seed-file section owns
 * format/token/rotation, THIS path owns getting the bytes to
 * csprng_init at Phase 1, long before X: mounts). Both rotation names
 * are read -- a crash mid-rotation can leave either valid; the kernel
 * accepts the highest fresh counter. Gated on the same boot.conf
 * seed_file knob as the kernel lifecycle (one escape hatch, both ends). */
static UINTN bl_collect_seed_carryover(UINT8 *seed, UINTN cap, UINTN pos)
{
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
    EFI_FILE_PROTOCOL *root = (EFI_FILE_PROTOCOL *)0;

    if (!g_boot_info_ptr->config.seed_file) {
        serial_early_print("[BOOT] RNG: seed file disabled by boot.conf\n");
        return pos;
    }
    fs = media_role_locate_blackbox_fs(g_boot_device_handle);
    if (!fs) {
        serial_early_print("[BOOT] RNG: BLACKBOX volume not found -- no seed carryover\n");
        return pos;
    }
    if (EFI_ERROR(fs->OpenVolume(fs, &root)) || !root) {
        serial_early_print("[BOOT] RNG: BLACKBOX OpenVolume failed -- no seed carryover\n");
        return pos;
    }
    pos = bl_seed_read_candidate(root, u"\\Boot\\random-seed.bin",
                                 seed, cap, pos);
    pos = bl_seed_read_candidate(root, u"\\Boot\\random-seed.new",
                                 seed, cap, pos);
    root->Close(root);
    return pos;
}

static void collect_boot_entropy(void)
{
    post_code16(POST16_BL_ENTROPY);

    /* Below 4 GiB: the kernel consumer dereferences phys_start through
     * the boot identity map, which covers the first 4 GiB only --
     * AllocateAny on high-memory firmware can return frames above that. */
    EFI_PHYSICAL_ADDRESS seed_addr = 0xFFFFFFFFull;
    EFI_STATUS status = gBS->AllocatePages(AllocateMaxAddress, EfiLoaderData,
                                           1, &seed_addr);
    if (EFI_ERROR(status)) {
        serial_early_print("[BOOT] RNG: seed page AllocatePages failed -- degraded\n");
        post_code16(POST16_BL_ENTROPY_OK);
        return;
    }
    UINT8 *seed = (UINT8 *)(UINTN)seed_addr;
    efi_memset(seed, 0, EFI_PAGE_SIZE);
    /* Reserve the 32-byte in-payload header; framed records follow it.
     * The header is filled in LAST -- mask/quality/transcript_len are
     * not known until every source has run. */
    UINTN pos = sizeof(struct bl_seed_header);
    UINT32 src_mask = 0;
    UINT32 src_quality = 0;
    UINTN prev;

    /* --- Source: EFI_RNG_PROTOCOL (UEFI 2.10 37.5) ---
     * The firmware_rng gate covers ONLY this block: the escape hatch
     * exists for hanging GetRNG implementations, and disabling it must
     * not also drop the independent ACPI OEM0 source below. */
    if (!g_boot_info_ptr->config.firmware_rng) {
        serial_early_print("[BOOT] RNG: EFI_RNG_PROTOCOL disabled by boot.conf\n");
    } else {
        EFI_GUID rng_guid = EFI_RNG_PROTOCOL_GUID;
        EFI_RNG_PROTOCOL *rng = (EFI_RNG_PROTOCOL *)0;
        status = gBS->LocateProtocol(&rng_guid, (VOID *)0, (VOID **)&rng);
        if (EFI_ERROR(status) || !rng) {
            serial_early_print("[BOOT] RNG: EFI_RNG_PROTOCOL absent\n");
        } else {
            UINT8 fw_bytes[BL_ENTROPY_FW_BYTES];
            /* Single GetRNG with the firmware-default algorithm (NULL).
             * GetInfo is skipped as nonessential: one fewer firmware
             * call that could hang, and the default algorithm is what
             * we consume either way. */
            status = rng->GetRNG(rng, (EFI_RNG_ALGORITHM *)0,
                                 BL_ENTROPY_FW_BYTES, fw_bytes);
            if (EFI_ERROR(status)) {
                serial_early_print("[BOOT] RNG: GetRNG failed -- degraded\n");
            } else {
                UINTN np = bl_entropy_frame(seed, EFI_PAGE_SIZE, pos,
                                            BL_ENTROPY_SRC_FW_RNG,
                                            fw_bytes, BL_ENTROPY_FW_BYTES);
                if (np != 0) {
                    pos = np;
                    src_mask    |= 1u << BL_ENTROPY_SRC_FW_RNG;
                    src_quality |= BL_ENTROPY_Q_HIGH
                                   << (2u * BL_ENTROPY_SRC_FW_RNG);
                    serial_early_print("[BOOT] RNG: EFI_RNG_PROTOCOL 64 bytes\n");
                }
            }
            /* Wipe unconditionally: UEFI gives no guarantee the buffer
             * is untouched on a failed GetRNG, and partial seed bytes
             * must not linger on the stack. */
            efi_memset(fw_bytes, 0, sizeof(fw_bytes));
        }
    }

    /* --- Source: CPU RDSEED/RDRAND --- */
    prev = pos;
    pos = bl_collect_cpu_rng(seed, EFI_PAGE_SIZE, pos);
    if (pos != prev) {
        src_mask    |= 1u << BL_ENTROPY_SRC_CPU_RNG;
        src_quality |= BL_ENTROPY_Q_HIGH << (2u * BL_ENTROPY_SRC_CPU_RNG);
    }

    /* --- Source: ACPI OEM0 entropy table (Win11 winload parity) --- */
    {
        const BL_ACPI_SDT_HDR *oem0 = bl_find_acpi_table("OEM0");
        if (!oem0) {
            serial_early_print("[BOOT] RNG: ACPI OEM0 absent\n");
        } else {
            UINT32 payload_len = oem0->length - 36u;
            if (payload_len > BL_ENTROPY_OEM0_CAP)
                payload_len = BL_ENTROPY_OEM0_CAP;
            if (payload_len > 0) {
                UINTN np = bl_entropy_frame(seed, EFI_PAGE_SIZE, pos,
                                            BL_ENTROPY_SRC_ACPI_OEM0,
                                            (const UINT8 *)oem0 + 36,
                                            payload_len);
                if (np != 0) {
                    pos = np;
                    src_mask    |= 1u << BL_ENTROPY_SRC_ACPI_OEM0;
                    src_quality |= BL_ENTROPY_Q_HIGH
                                   << (2u * BL_ENTROPY_SRC_ACPI_OEM0);
                    serial_early_print("[BOOT] RNG: ACPI OEM0 ");
                    serial_early_print_uint(payload_len);
                    serial_early_print(" bytes\n");
                }
            }
        }
    }

    /* --- Source: seed-file carryover (pre-EBS read; kernel verifies) --- */
    prev = pos;
    pos = bl_collect_seed_carryover(seed, EFI_PAGE_SIZE, pos);
    if (pos != prev) {
        /* LOW advisory: HIGH credit for carryover belongs to the kernel's
         * Phase-3 commit point, never the unverified pre-EBS read. */
        src_mask    |= 1u << BL_ENTROPY_SRC_SEED_FILE;
        src_quality |= BL_ENTROPY_Q_LOW << (2u * BL_ENTROPY_SRC_SEED_FILE);
    }

    /* --- Source: boot timing personalization (TIME -- LOW, uncredited) --- */
    {
        UINT64 vals[4];
        vals[0] = g_boot_info_ptr->timing.bl_entry;
        vals[1] = g_boot_info_ptr->timing.reset_end;
        vals[2] = g_boot_info_ptr->timing.tsc_freq;
        vals[3] = boot_rdtsc();
        if (vals[2] == 0) {
            /* The caller orders the TSC measurement before this; a zero
             * here means that contract broke -- refuse the record rather
             * than framing a misleading placeholder. */
            serial_early_print("[BOOT] RNG: TIME record refused (tsc_freq=0)\n");
        } else {
            UINT8 tbytes[32];
            UINTN vi, b;
            for (vi = 0; vi < 4; vi++)
                for (b = 0; b < 8; b++)
                    tbytes[vi * 8 + b] = (UINT8)(vals[vi] >> (b * 8));
            UINTN np = bl_entropy_frame(seed, EFI_PAGE_SIZE, pos,
                                        BL_ENTROPY_SRC_TIME,
                                        tbytes, (UINT32)sizeof(tbytes));
            if (np != 0) {
                pos = np;
                src_mask    |= 1u << BL_ENTROPY_SRC_TIME;
                src_quality |= BL_ENTROPY_Q_LOW << (2u * BL_ENTROPY_SRC_TIME);
                serial_early_print("[BOOT] RNG: boot timing 32 bytes (personalization)\n");
            }
        }
    }

    if (pos == sizeof(struct bl_seed_header)) {
        /* Nothing collected (header slot is all that's filled): zero the
         * page and return it -- an unpublished EfiLoaderData page would
         * be reclaimed by the kernel PMM with raw loader bytes still in
         * it, and stale memory must never masquerade as seed material. */
        efi_memset(seed, 0, EFI_PAGE_SIZE);
        gBS->FreePages(seed_addr, 1);
        serial_early_print("[BOOT] RNG: no firmware entropy collected -- degraded\n");
        post_code16(POST16_BL_ENTROPY_OK);
        return;
    }

    /* Fill the in-payload header (kernel entropy_seed_parse contract:
     * IPRS magic, version, advisory mask/quality, exact transcript_len). */
    {
        struct bl_seed_header *hdr = (struct bl_seed_header *)seed;
        hdr->magic          = BL_SEED_MAGIC;
        hdr->version        = BL_SEED_VERSION;
        hdr->source_mask    = src_mask;
        hdr->quality        = src_quality;
        hdr->transcript_len = (UINT32)(pos - sizeof(struct bl_seed_header));
        hdr->reserved[0]    = 0;
        hdr->reserved[1]    = 0;
        hdr->reserved[2]    = 0;
    }

    /* Publish as a typed payload so the kernel reserves + consumes +
     * zeroes the page (descriptor validation owned by the boot protocol;
     * kernel-side consumption owned by the early-entropy seed handoff
     * section). */
    /* Build the descriptor COMPLETE on the stack, then publish. The table is
     * not touched until every fallible step above has already succeeded; see
     * publish_implicit_payload for why claiming earlier would be worse than
     * dropping the seed. */
    struct boot_payload_desc seed_desc;
    efi_memset(&seed_desc, 0, sizeof(seed_desc));
    seed_desc.type        = BOOT_PAYLOAD_RANDOM_SEED;
    seed_desc.flags       = BOOT_PAYLOAD_FLAG_VALID | BOOT_PAYLOAD_FLAG_RESERVED |
                            BOOT_PAYLOAD_FLAG_CHECKSUMMED;
    seed_desc.phys_start  = (UINT64)seed_addr;
    seed_desc.length      = (UINT64)pos;
    seed_desc.alignment   = 4096ull;
    /* CRC-32C of the WHOLE payload (header included), low 32 bits;
     * the kernel parser rejects on mismatch before reading a record. */
    seed_desc.checksum    = (UINT64)bl_crc32c(seed, pos);
    seed_desc.producer_id = BOOT_PRODUCER_UEFI;
    seed_desc._reserved   = 0u;

    if (!publish_implicit_payload(&seed_desc)) {
        efi_memset(seed, 0, EFI_PAGE_SIZE);
        gBS->FreePages(seed_addr, 1);
        post_code16(POST16_BL_ENTROPY_OK);
        return;
    }

    serial_early_print("[BOOT] RNG: seed payload ");
    serial_early_print_uint((UINT32)pos);
    serial_early_print(" bytes at 0x");
    serial_early_print_hex64((UINT64)seed_addr);
    serial_early_print("\n");
    post_code16(POST16_BL_ENTROPY_OK);
}

/* ============================================================================
 * Headless enrollment authorization transport (owner: TPM measured-boot
 * attestation roadmap, section 30)
 *
 * Carries an OFFLINE-SIGNED authorization from the ESP to the kernel as a
 * BOOT_PAYLOAD_HEADLESS_AUTHZ descriptor. The blob is not authority in itself:
 * it is Ed25519-signed under a key the kernel already holds, bound to this
 * machine's EK identity, bound to the exact measured-state transition it
 * authorizes, and spent against a monotonic TPM counter. That is what
 * separates it from the plain ESP-file authority the enrollment gate
 * deliberately does not honor -- anyone can write this file and it buys them
 * nothing without the private half.
 *
 * The transport therefore carries bytes, never trust, and every failure
 * degrades to NO authorization rather than a partial one: absent file, wrong
 * size, unreadable, or a full descriptor table all leave the kernel exactly
 * where it is today, which is a machine that refuses a headless enrollment.
 * Nothing here is fatal for the same reason -- a machine with no authorization
 * file is the normal case, not a broken boot.
 *
 * Runs AFTER load_staged_payloads() so the packed-prefix invariant the kernel
 * validator enforces (no occupied slot at index >= payload_count) still holds:
 * this appends one slot at the current count.
 * ============================================================================ */

#define POST16_BL_HL_AUTHZ        0xB0A6
#define POST16_BL_HL_AUTHZ_OK     0xB0A7

/* The exact on-wire size of struct tpm_headless_authz_blob
 * (include/kernel/tpm_headless_authz.h, TPM_HEADLESS_BLOB_LEN). Mirrored
 * rather than included because the bootloader cannot pull kernel headers; the
 * kernel-side consumer re-checks the length against its own definition, so a
 * drift here refuses the payload instead of handing over short bytes. */
#define BL_HEADLESS_AUTHZ_LEN     152u

/* PRODUCER/KERNEL DRIFT GATE, expanded from the shared table.
 *
 * Two earlier versions of this block were wrong in instructive ways and both
 * were caught in review. The first restated the table's numbers as literals,
 * so editing a row still compiled and the include bought nothing. The second
 * expanded the table but compared against mirror constants invented HERE --
 * a third copy -- so changing the real producer constant left the assertions
 * green. This one compares each expanded row against the constant or struct
 * the producer actually uses, which is why it sits down here rather than
 * beside the include: BL_HEADLESS_AUTHZ_LEN and struct bl_seed_header are
 * defined further up the file than that include.
 *
 * Only the types this loader PUBLISHES are asserted. It owns no constant to
 * compare the others against, and asserting a number against itself is the
 * mistake above wearing a third hat. */
#define BOOT_PAYLOAD_LIMIT_LOADER_ASSERT(type_value, label, min_b, max_b, why) \
    _Static_assert((type_value) != (unsigned long long)BOOT_PAYLOAD_MODULE  || ((max_b) == BOOT_PAYLOAD_FILE_MAX    \
                                           && (min_b) == 1ull),                \
                   "MODULE row must stay the loader's file cap, min 1");       \
    _Static_assert((type_value) != (unsigned long long)BOOT_PAYLOAD_INITRD  || ((max_b) == BOOT_PAYLOAD_FILE_MAX    \
                                           && (min_b) == 1ull),                \
                   "INITRD row must stay the loader's file cap, min 1");       \
    _Static_assert((type_value) != (unsigned long long)BOOT_PAYLOAD_RECOVERY_IMAGE \
                       || ((max_b) == BOOT_PAYLOAD_FILE_MAX                     \
                                           && (min_b) == 1ull),                \
                   "RECOVERY_IMAGE row must stay the loader's file cap, min 1"); \
    _Static_assert((type_value) != (unsigned long long)BOOT_PAYLOAD_RANDOM_SEED \
                       || (min_b) == sizeof(struct bl_seed_header),            \
                   "RANDOM_SEED minimum must stay the seed header this loader writes"); \
    _Static_assert((type_value) != (unsigned long long)BOOT_PAYLOAD_HEADLESS_AUTHZ                 \
                       || ((min_b) == (max_b)                                                  \
                                           && (max_b) == BL_HEADLESS_AUTHZ_LEN), \
                   "HEADLESS_AUTHZ row must stay the EXACT blob length this loader publishes");
BOOT_PAYLOAD_LIMIT_LIST(BOOT_PAYLOAD_LIMIT_LOADER_ASSERT)
#undef BOOT_PAYLOAD_LIMIT_LOADER_ASSERT

static void publish_headless_authz_payload(void)
{
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
    EFI_FILE_PROTOCOL *root_dir;
    EFI_FILE_PROTOCOL *file;
    EFI_STATUS status;
    EFI_GUID file_info_guid = EFI_FILE_INFO_ID;
    UINTN info_size = 0;
    VOID *info_buf = (VOID *)0;
    UINT64 file_size = 0;
    EFI_PHYSICAL_ADDRESS blob_addr = 0;
    UINTN read_size;

    /* NOTHING AT ALL ON A BOOT THAT NEVER ASKED TO ENROLL -- not the
     * filesystem work, and not the POST markers either.
     *
     * `tpm_enroll` defaults to 0, so without this gate every ordinary boot on
     * every machine paid two synchronous firmware Open() attempts (the vendor
     * directory and the volume root) for a file that is almost never there,
     * pre-ExitBootServices where the cost is boot latency the operator sees.
     *
     * The gate is ahead of post_code16 rather than after it because this
     * bootloader's post_code16 is not just an I/O-port write: it also emits
     * "[BOOT] POST 0xNNNN" through the POLLED UART, so an entry/exit pair on
     * the inactive path is real serial time on every boot -- around 10 ms at
     * the 38400-baud fallback. A marker for a feature the boot did not request
     * is exactly the kind of unconditional cost this gate exists to remove,
     * which is why the two codes are OPTIONAL in the POST16 manifest rather
     * than required. */
    if (!g_boot_info_ptr->config.tpm_enroll)
        return;

    post_code16(POST16_BL_HL_AUTHZ);

    status = locate_boot_fs(&fs);
    if (EFI_ERROR(status)) {
        /* Silence here read as "no authorization present", which is a
         * different thing from "could not look". */
        serial_early_print("[WARN] headless authz: no boot filesystem -- not published\n");
        post_code16(POST16_BL_HL_AUTHZ_OK);
        return;
    }
    status = fs->OpenVolume(fs, &root_dir);
    if (EFI_ERROR(status)) {
        serial_early_print("[WARN] headless authz: OpenVolume failed -- not published\n");
        post_code16(POST16_BL_HL_AUTHZ_OK);
        return;
    }

    status = root_dir->Open(root_dir, &file,
                            u"\\EFI\\ImpossibleOS\\tpm-authz.bin",
                            EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status)) {
        /* Same two-location search boot.conf uses, so an authorization can be
         * dropped at the volume root on firmware that cannot create the
         * vendor directory. */
        status = root_dir->Open(root_dir, &file, u"\\tpm-authz.bin",
                                EFI_FILE_MODE_READ, 0);
    }
    if (EFI_ERROR(status)) {
        /* SAY SO. This is only reached on a boot that ASKED to enroll, so an
         * absent authorization is a fact the operator needs rather than noise
         * on every machine -- it is the difference between "this boot will
         * wait for a console" and "the file I placed was not found". The
         * enrollment gate refuses either way; this line is what tells an
         * operator which of the two happened. */
        serial_early_print("[BOOT] headless authz: no authorization file on "
                           "the ESP (tpm_enroll is set; enrollment will need "
                           "a console)\n");
        root_dir->Close(root_dir);
        post_code16(POST16_BL_HL_AUTHZ_OK);
        return;
    }

    status = file->GetInfo(file, &file_info_guid, &info_size, (VOID *)0);
    if (status != EFI_BUFFER_TOO_SMALL || info_size == 0) {
        serial_early_print("[WARN] headless authz: GetInfo size probe failed\n");
        goto done_file;
    }
    status = gBS->AllocatePool(EfiLoaderData, info_size, &info_buf);
    if (EFI_ERROR(status) || !info_buf) {
        serial_early_print("[WARN] headless authz: AllocatePool(info) failed\n");
        goto done_file;
    }
    status = file->GetInfo(file, &file_info_guid, &info_size, info_buf);
    if (EFI_ERROR(status)) {
        gBS->FreePool(info_buf);
        serial_early_print("[WARN] headless authz: GetInfo read failed\n");
        goto done_file;
    }
    file_size = ((EFI_FILE_INFO *)info_buf)->FileSize;
    gBS->FreePool(info_buf);

    /* EXACT size, not a ceiling. The blob is a fixed-layout record whose
     * signature covers a fixed byte range; a short file cannot be a truncated
     * authorization that is merely weaker, and a long one is not this format.
     * Refusing both here means the kernel never sees bytes it would have to
     * decide about. */
    if (file_size != (UINT64)BL_HEADLESS_AUTHZ_LEN) {
        serial_early_print("[WARN] headless authz: file is ");
        serial_early_print_uint((UINT32)file_size);
        serial_early_print(" bytes, expected ");
        serial_early_print_uint((UINT32)BL_HEADLESS_AUTHZ_LEN);
        serial_early_print(" -- not published\n");
        goto done_file;
    }

    /* THE YIELD IS GONE, and its absence is the point. This function used to
     * check `idx + 1 >= BOOT_PAYLOAD_MAX` rather than the ordinary bound,
     * refusing the last slot by hand so the entropy seed -- published later in
     * the boot, with no way to signal a reservation backwards -- would still
     * get one. That worked only because there happened to be exactly two
     * implicit publishers and this one happened to run first; a third would
     * have re-opened the race silently.
     *
     * The staging table now reserves one slot PER declared implicit publisher
     * (BOOT_PAYLOAD_STAGE_MAX above), so this function has a slot by
     * construction and does not need to know that entropy exists. The capacity
     * decision moved to publish_implicit_payload, below, at the point where the
     * descriptor is complete. */

    /* Below 4 GiB, exactly as the seed transport does and for the same reason:
     * the kernel consumer dereferences phys_start through the boot identity
     * map, which covers the first 4 GiB only. AllocateAnyPages on high-memory
     * firmware returns frames above that, and the kernel would then REFUSE a
     * perfectly valid signed authorization -- a headless machine that cannot
     * enroll, with the cause visible only as an out-of-map log line. */
    blob_addr = 0xFFFFFFFFull;
    status = gBS->AllocatePages(AllocateMaxAddress, EfiLoaderData, 1, &blob_addr);
    if (EFI_ERROR(status)) {
        serial_early_print("[WARN] headless authz: AllocatePages failed\n");
        goto done_file;
    }
    /* Zero the WHOLE page before the read. The descriptor's length is 152, but
     * the page is RESERVED and handed to the kernel, so the 3944 bytes past
     * the blob would otherwise be whatever EfiLoaderData held -- loader stack
     * or an earlier file's contents. */
    efi_memset((VOID *)(UINTN)blob_addr, 0, EFI_PAGE_SIZE);

    read_size = (UINTN)BL_HEADLESS_AUTHZ_LEN;
    status = file->Read(file, &read_size, (VOID *)(UINTN)blob_addr);
    if (EFI_ERROR(status) || read_size != (UINTN)BL_HEADLESS_AUTHZ_LEN) {
        efi_memset((VOID *)(UINTN)blob_addr, 0, EFI_PAGE_SIZE);
        gBS->FreePages(blob_addr, 1);
        serial_early_print("[WARN] headless authz: short read -- not published\n");
        goto done_file;
    }

    /* Build the descriptor COMPLETE on the stack, then publish. Every step that
     * could still fail -- AllocatePages, the file read, the length check -- is
     * already behind us, so the commit inside publish_implicit_payload cannot
     * leave a half-written slot in the packed prefix. */
    {
        struct boot_payload_desc authz_desc;
        efi_memset(&authz_desc, 0, sizeof(authz_desc));
        authz_desc.type        = BOOT_PAYLOAD_HEADLESS_AUTHZ;
        authz_desc.flags       = BOOT_PAYLOAD_FLAG_VALID | BOOT_PAYLOAD_FLAG_RESERVED |
                                 BOOT_PAYLOAD_FLAG_CHECKSUMMED;
        authz_desc.phys_start  = (UINT64)blob_addr;
        authz_desc.length      = (UINT64)BL_HEADLESS_AUTHZ_LEN;
        authz_desc.alignment   = 4096ull;
        /* CRC-32C detects a corrupt transfer, and that is ALL it is for: the
         * authorization's integrity comes from its Ed25519 signature, which an
         * attacker who can rewrite the file can also re-CRC. Naming that here
         * stops a later reader from mistaking the checksum for a security
         * property. */
        authz_desc.checksum    = (UINT64)bl_crc32c((const UINT8 *)(UINTN)blob_addr,
                                                   (UINTN)BL_HEADLESS_AUTHZ_LEN);
        authz_desc.producer_id = BOOT_PRODUCER_UEFI;
        authz_desc._reserved   = 0u;

        if (!publish_implicit_payload(&authz_desc)) {
            efi_memset((VOID *)(UINTN)blob_addr, 0, EFI_PAGE_SIZE);
            gBS->FreePages(blob_addr, 1);
            blob_addr = 0;
            goto done_file;
        }
    }

    serial_early_print("[BOOT] headless authz payload published at 0x");
    serial_early_print_hex64((UINT64)blob_addr);
    serial_early_print("\n");

done_file:
    file->Close(file);
    root_dir->Close(root_dir);
    post_code16(POST16_BL_HL_AUTHZ_OK);
}

/* ----- SMBIOS Type 1 (System Information) UUID extraction --------------
 *
 * SMBIOS UUID feeds the boot policy ladder's machine_id filter (the
 * envelope's machine_id field uses RFC 4122 textual UUID, OR empty for
 * the "match any machine" wildcard) and the LoaderXxx variable feature
 * (publication only; the kernel's own SMBIOS walker also extracts the
 * UUID post-EBS for the kernel-side surface).
 *
 * Anchor lookup: prefer the SMBIOS3 64-bit entry point (SMBIOS3 GUID)
 * which has anchor string "_SM3_" and a 64-bit TableAddress, then fall
 * back to SMBIOS 2.x with "_SM_" anchor and 32-bit TableAddress.
 * Per the SMBIOS spec sections 5.2.1 / 5.2.2 we validate the anchor
 * string + checksum before trusting any field.
 *
 * Type 1 UUID format: per SMBIOS 2.6+ the first three fields of the
 * 16-byte UUID at offset 8 are little-endian-on-wire and must be
 * byte-swapped to RFC 4122 textual form. UUID "00000000-0000-0000-..."
 * or "FF...FF" is the "not-specified" sentinel and is left as the
 * empty wildcard. */

/* SMBIOS entry-point anchors (UEFI-specific layouts; the shared
 * header in include/boot/boot_smbios_parse.h owns the pure data
 * walker for Type 1 lookup + the UUID formatter). */
struct smbios_ep32 {
    char     anchor[4];          /* "_SM_" */
    UINT8    checksum;
    UINT8    length;
    UINT8    major_version;
    UINT8    minor_version;
    UINT16   max_structure_size;
    UINT8    entry_point_revision;
    UINT8    formatted_area[5];
    char     dmi_anchor[5];      /* "_DMI_" */
    UINT8    dmi_checksum;
    UINT16   structure_table_length;
    UINT32   structure_table_address;
    UINT16   structure_count;
    UINT8    bcd_revision;
} __attribute__((packed));

struct smbios_ep64 {
    char     anchor[5];          /* "_SM3_" */
    UINT8    checksum;
    UINT8    length;
    UINT8    major_version;
    UINT8    minor_version;
    UINT8    docrev;
    UINT8    entry_point_revision;
    UINT8    reserved;
    UINT32   structure_table_max_size;
    UINT64   structure_table_address;
} __attribute__((packed));

#include "../../../include/boot/boot_smbios_parse.h"

/* Thin UEFI-typed adapters around the pure header helpers. */
static int smbios_format_uuid(const UINT8 src[16], char out[37])
{
    return boot_smbios_format_uuid((const unsigned char *)src, out);
}

/* SMBIOS entry-point checksum: sum of all bytes from base..base+len
 * modulo 256 must be 0 per spec section 5.2.1 / 5.2.2. */
static int smbios_ep_checksum_ok(const UINT8 *base, UINTN len)
{
    UINT8 sum = 0;
    if (!base || len == 0 || len > 256) return 0;
    for (UINTN i = 0; i < len; i++) sum = (UINT8)(sum + base[i]);
    return sum == 0;
}

/* Extract the SMBIOS Type 1 UUID into RFC 4122 textual form, written
 * into `out[37]` (caller buffer). Returns 1 on success (out is a
 * valid 36-char UUID), 0 otherwise (out is set to empty string).
 * Prefers SMBIOS3 entry point if both are advertised. */
static int smbios_extract_system_uuid(char out[37])
{
    out[0] = 0;
    if (!gST) return 0;
    /* gST->ConfigurationTable must be non-NULL before indexing.
     * Firmware that advertises a nonzero NumberOfTableEntries with a
     * NULL table pointer is malformed but seen in the wild on some
     * legacy systems; bail safely. */
    if (!gST->ConfigurationTable || gST->NumberOfTableEntries == 0) {
        serial_early_print("[BOOT] SMBIOS: ConfigurationTable absent\n");
        return 0;
    }
    EFI_GUID g3 = EFI_SMBIOS3_TABLE_GUID;
    EFI_GUID g2 = EFI_SMBIOS_TABLE_GUID;
    const void *ep_addr = (const void *)0;
    int ep_is_3 = 0;
    for (UINTN i = 0; i < gST->NumberOfTableEntries; i++) {
        EFI_CONFIGURATION_TABLE *entry = &gST->ConfigurationTable[i];
        if (guid_equal(&entry->VendorGuid, &g3)) {
            ep_addr = entry->VendorTable;
            ep_is_3 = 1;
            break;   /* SMBIOS3 wins -- 64-bit address space */
        }
        if (guid_equal(&entry->VendorGuid, &g2) && !ep_addr) {
            ep_addr = entry->VendorTable;
            ep_is_3 = 0;
            /* keep scanning in case SMBIOS3 is advertised later */
        }
    }
    if (!ep_addr) {
        serial_early_print("[BOOT] SMBIOS: no entry point in ConfigurationTable\n");
        return 0;
    }

    const UINT8 *table_base = (const UINT8 *)0;
    UINTN table_len = 0;
    if (ep_is_3) {
        const struct smbios_ep64 *e = (const struct smbios_ep64 *)ep_addr;
        /* Anchor: must be "_SM3_" + minimum length per SMBIOS spec
         * section 5.2.2. */
        if (e->anchor[0] != '_' || e->anchor[1] != 'S' || e->anchor[2] != 'M' ||
            e->anchor[3] != '3' || e->anchor[4] != '_') {
            serial_early_print("[BOOT] SMBIOS3: anchor mismatch\n");
            return 0;
        }
        if (e->length < sizeof(*e)) {
            serial_early_print("[BOOT] SMBIOS3: length too small\n");
            return 0;
        }
        /* Checksum: 8-bit sum of all bytes in the entry point must be
         * zero per SMBIOS spec section 5.2.2. A failed checksum means
         * a corrupt or hostile firmware table; do NOT trust the
         * structure_table_address field. */
        if (!smbios_ep_checksum_ok((const UINT8 *)e, e->length)) {
            serial_early_print("[BOOT] SMBIOS3: entry-point checksum failed\n");
            return 0;
        }
        table_base = (const UINT8 *)(UINTN)e->structure_table_address;
        table_len = (UINTN)e->structure_table_max_size;
    } else {
        const struct smbios_ep32 *e = (const struct smbios_ep32 *)ep_addr;
        if (e->anchor[0] != '_' || e->anchor[1] != 'S' ||
            e->anchor[2] != 'M' || e->anchor[3] != '_') {
            serial_early_print("[BOOT] SMBIOS2: anchor mismatch\n");
            return 0;
        }
        if (e->length < sizeof(*e)) {
            serial_early_print("[BOOT] SMBIOS2: length too small\n");
            return 0;
        }
        /* Both checksums required per SMBIOS spec section 5.2.1: the
         * 8-bit sum of the first `length` bytes (entry point checksum),
         * AND the 5-byte _DMI_ subsection has its own checksum byte at
         * offset 16. */
        if (!smbios_ep_checksum_ok((const UINT8 *)e, e->length)) {
            serial_early_print("[BOOT] SMBIOS2: entry-point checksum failed\n");
            return 0;
        }
        /* DMI subsection checksum: sum of bytes at offsets 16..30
         * (length 15: dmi_anchor[5] + dmi_checksum + structure_table_*
         * + structure_count + bcd_revision) must be zero. */
        if (!smbios_ep_checksum_ok((const UINT8 *)e + 16, 15)) {
            serial_early_print("[BOOT] SMBIOS2: _DMI_ checksum failed\n");
            return 0;
        }
        table_base = (const UINT8 *)(UINTN)e->structure_table_address;
        table_len = (UINTN)e->structure_table_length;
    }
    if (!table_base || table_len == 0) {
        serial_early_print("[BOOT] SMBIOS: NULL table base or zero length\n");
        return 0;
    }

    const struct boot_smbios_header *h =
        boot_smbios_find_type1((const unsigned char *)table_base,
                                (unsigned int)table_len);
    if (!h) {
        serial_early_print("[BOOT] SMBIOS: Type 1 not found\n");
        return 0;
    }
    /* UUID at offset 8 within the Type 1 structure; requires length
     * >= 25 per spec section 7.2. */
    if (h->length < 25) {
        serial_early_print("[BOOT] SMBIOS: Type 1 too small for UUID\n");
        return 0;
    }
    const UINT8 *raw = (const UINT8 *)h + 8;
    if (!smbios_format_uuid(raw, out)) {
        serial_early_print("[BOOT] SMBIOS: Type 1 UUID is not-specified sentinel\n");
        return 0;
    }
    serial_early_print("[BOOT] SMBIOS: system UUID=");
    serial_early_print(out);
    serial_early_print("\n");
    return 1;
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

/* ============================================================================
 * Higher-half direct map (HHDM) construction -- higher-half relocation
 * section 2 (todo/02-kernel-core/TODO-33). Installs a fixed-offset,
 * sparse, RAM-only alias of physical memory into PML4 slots 273-400 while
 * the kernel still links and boots LOW and still walks page tables via the
 * identity map. Purely ADDITIVE: it changes no address the kernel executes
 * from and touches no existing walker (walker conversion is section 9).
 *
 * Split across ExitBootServices (EBS): the arena that holds the extra
 * PDPT/PD/PT frames is AllocatePages'd PRE-EBS (bl_hhdm_reserve_arena), and
 * the leaf install runs POST-EBS inside setup_page_tables (memory writes
 * only). Design rationale: docs/infrastructure/kernel-address-space.md.
 * ========================================================================== */

/* POST16 codes (0xBxxx bootloader range) around the HHDM steps, before
 * jump_to_kernel. Defined here (ahead of the 0xB060 block) because the HHDM
 * functions below reference them. */
#define POST16_BL_HHDM_PLAN     0xB061  /* pre-EBS: sized + reserved HHDM arena */
#define POST16_BL_HHDM_ARENA_OK 0xB062  /* pre-EBS: arena AllocatePages + disjoint OK */
#define POST16_BL_HHDM_NXE      0xB063  /* post-EBS: EFER.NXE enabled (CPUID-gated) */
#define POST16_BL_HHDM_INSTALL  0xB064  /* post-EBS: installing HHDM leaves (PML4 273-400) */
#define POST16_BL_HHDM_OK       0xB065  /* post-EBS: HHDM direct map complete */

/* The HHDM constants are SINGLE-SOURCED from include/kernel/mm/memmap_boot.h
 * (included above) -- MM_HHDM_BASE / MM_HHDM_SIZE / MM_PML4_PHYS_LIMIT /
 * MM_HHDM_PML4_SLOT / MM_HHDM_PML4_SLOT_LAST. Both this bootloader and the
 * kernel's memmap.h include that one file, so there is no separate literal
 * that can drift. The BL_ aliases keep the local names below unchanged. */
#define BL_MM_HHDM_BASE        MM_HHDM_BASE
#define BL_MM_HHDM_SIZE        MM_HHDM_SIZE
#define BL_MM_PML4_PHYS_LIMIT  MM_PML4_PHYS_LIMIT
#define BL_HHDM_PML4_SLOT      MM_HHDM_PML4_SLOT
#define BL_HHDM_PML4_SLOT_LAST MM_HHDM_PML4_SLOT_LAST

/* pmm_init() clamps its bitmap coverage at 4 GiB (src/kernel/mm/pmm.c
 * highest_addr cap). This is a DISTINCT constant from MM_PML4_PHYS_LIMIT (the
 * AP 32-bit CR3 ceiling) that happens to share the 4 GiB value; keep them
 * separate so a change to one never silently resizes the bitmap guard. */
#define BL_PMM_BITMAP_PHYS_CAP 0x0000000100000000ULL  /* pmm.c 4 GiB highest_addr cap */

/* Page-table entry flags + sizes. */
#define BL_PTE_P      (1ULL << 0)
#define BL_PTE_RW     (1ULL << 1)
#define BL_PTE_PS     (1ULL << 7)   /* 2 MiB leaf when set in a PDE */
#define BL_PTE_NX     (1ULL << 63)  /* requires EFER.NXE; walk faults otherwise */
#define BL_PAGE_4K    0x1000ULL
#define BL_PAGE_2M    0x200000ULL
#define BL_PADDR_MASK 0x000ffffffffff000ULL  /* 52-bit phys, 4 KiB aligned */

/* Closes section 1's deferred item: the kernel PML4 frame (loaded into CR3 by
 * the AP's 32-bit `mov cr3, eax`) MUST live below 4 GiB or the load truncates
 * the root silently. PT_PML4 is a fixed low literal; pin it at compile time. */
_Static_assert((UINT64)PT_PML4 < BL_MM_PML4_PHYS_LIMIT,
    "kernel PML4 frame must load below 4 GiB (AP 32-bit CR3 load)");
_Static_assert(((UINT64)PT_PML4 & (BL_PAGE_4K - 1ULL)) == 0ULL,
    "PT_PML4 must be 4 KiB aligned");

/* --- MSR / CPUID primitives. The bootloader has none (the kernel MSR
 * helpers are kernel-only per the MSR-probe gotcha); long mode is already
 * active here, so rdmsr/wrmsr/cpuid are safe. --- */
static inline UINT64 bl_rdmsr(UINT32 msr)
{
    UINT32 lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((UINT64)hi << 32) | lo;
}
static inline void bl_wrmsr(UINT32 msr, UINT64 val)
{
    __asm__ volatile ("wrmsr" : : "c"(msr), "a"((UINT32)val),
                       "d"((UINT32)(val >> 32)));
}
/* CPUID uses the existing bl_cpuid(leaf, subleaf, &a, &b, &c, &d) helper
 * defined earlier in this file. */

#define BL_MSR_EFER   0xC0000080u
#define BL_EFER_NXE   (1ULL << 11)
#define BL_UEFI_MMAP_RESERVED 0u  /* EfiReservedMemoryType (kernel UEFI_MMAP_RESERVED) */

/* Enable EFER.NXE on the BSP: CPUID-gated read-modify-write with read-back,
 * FATAL on failure. Must run before the kernel WALKS an NX HHDM leaf; writing
 * the NX bit into a PTE is a plain memory store and needs no NXE, but any walk
 * (incl. a data read) of an NX PTE with NXE=0 raises a reserved-bit #PF. NXE
 * persists BSP -> kernel; cpu_enable_nx() re-asserts it idempotently, and APs
 * enable NXE in the trampoline before paging. */
static void bl_enable_nxe(void)
{
    UINT32 a = 0, b = 0, c = 0, d = 0;
    bl_cpuid(0x80000001u, 0, &a, &b, &c, &d);
    if (!(d & (1u << 20))) {              /* CPUID.80000001h:EDX.NX[20] */
        boot_fatal(BOOT_ERR_HHDM_FAIL, "CPU lacks NX support",
                   "CPUID.80000001h:EDX.NX is clear; the HHDM requires NX "
                   "leaves. Cannot construct the higher-half direct map.");
    }
    UINT64 efer = bl_rdmsr(BL_MSR_EFER);
    if (!(efer & BL_EFER_NXE)) {
        bl_wrmsr(BL_MSR_EFER, efer | BL_EFER_NXE);
        efer = bl_rdmsr(BL_MSR_EFER);
        if (!(efer & BL_EFER_NXE)) {
            boot_fatal(BOOT_ERR_HHDM_FAIL, "EFER.NXE did not latch",
                       "wrmsr(EFER |= NXE) read back with NXE still clear.");
        }
    }
}

/* --- Usable-RAM interval model. Mirrors the memory types pmm_init() frees
 * (Conventional, Loader{Code,Data}, BootServices{Code,Data}); everything
 * else stays reserved and is never aliased into the writable direct map
 * (Decision 5: sparse, RAM-only). --- */
#define BL_HHDM_MAX_IV 640   /* >= BOOT_MMAP_MAX_ENTRIES(512) + arena; fail-closed */

struct bl_hhdm_iv { UINT64 base; UINT64 end; };  /* [base,end) 4 KiB-aligned */
static struct bl_hhdm_iv s_hhdm_iv[BL_HHDM_MAX_IV];
static UINT32 s_hhdm_iv_n;
static UINT64 s_hhdm_arena_next;   /* next free arena page index (FILL only) */

static int bl_hhdm_type_usable(UINT32 uefi_type)
{
    return uefi_type == 7 ||   /* EfiConventionalMemory */
           uefi_type == 1 ||   /* EfiLoaderCode */
           uefi_type == 2 ||   /* EfiLoaderData */
           uefi_type == 3 ||   /* EfiBootServicesCode */
           uefi_type == 4;     /* EfiBootServicesData */
}

/* Collect page-aligned usable-RAM intervals from boot_info.mmap into
 * s_hhdm_iv, clamped to the 64 TiB HHDM window, optionally splicing in the
 * arena interval (used POST-EBS after the arena is retagged RESERVED so it
 * is no longer a "usable" type -- F1/F2 "plus retained arena frames"). Sorts
 * by base and coalesces adjacent/overlapping runs. Returns 0, or -1 on
 * capacity overflow (caller FATALs). */
static int bl_hhdm_collect_usable(int include_arena)
{
    UINT32 i;
    s_hhdm_iv_n = 0;
    for (i = 0; i < g_boot_info_ptr->mmap_count; i++) {
        if (!bl_hhdm_type_usable(g_boot_info_ptr->mmap[i].uefi_memory_type))
            continue;
        UINT64 base = g_boot_info_ptr->mmap[i].base_addr;
        UINT64 end  = base + g_boot_info_ptr->mmap[i].length;
        base = (base + BL_PAGE_4K - 1) & ~(BL_PAGE_4K - 1);   /* align inward */
        end &= ~(BL_PAGE_4K - 1);
        if (end <= base) continue;
        if (base >= BL_MM_HHDM_SIZE) continue;                /* beyond window */
        if (end > BL_MM_HHDM_SIZE) end = BL_MM_HHDM_SIZE;
        if (s_hhdm_iv_n >= BL_HHDM_MAX_IV) return -1;
        s_hhdm_iv[s_hhdm_iv_n].base = base;
        s_hhdm_iv[s_hhdm_iv_n].end  = end;
        s_hhdm_iv_n++;
    }
    /* Retained fixed page tables (PT_PML4..PT_PD3 at 0x70000-0x75fff): the
     * kernel keeps walking these as its live PML4 root, and section 9 will
     * dereference them through the HHDM alias -- so they need a leaf even if
     * bare-metal firmware labels the low range Reserved rather than
     * Conventional (QEMU marks it Conventional, so this is bare-metal
     * insurance). Added unconditionally; coalesces away when already covered. */
    if (s_hhdm_iv_n >= BL_HHDM_MAX_IV) return -1;
    s_hhdm_iv[s_hhdm_iv_n].base = (UINT64)PT_PML4 & ~(BL_PAGE_4K - 1);
    s_hhdm_iv[s_hhdm_iv_n].end  =
        (((UINT64)PT_PML4 + 6ULL * BL_PAGE_4K) + BL_PAGE_4K - 1) & ~(BL_PAGE_4K - 1);
    s_hhdm_iv_n++;
    if (include_arena && g_hhdm_arena_pages) {
        UINT64 abase = g_hhdm_arena_base;
        UINT64 aend  = g_hhdm_arena_base + g_hhdm_arena_pages * BL_PAGE_4K;
        if (abase < BL_MM_HHDM_SIZE) {
            if (aend > BL_MM_HHDM_SIZE) aend = BL_MM_HHDM_SIZE;
            if (s_hhdm_iv_n >= BL_HHDM_MAX_IV) return -1;
            s_hhdm_iv[s_hhdm_iv_n].base = abase;
            s_hhdm_iv[s_hhdm_iv_n].end  = aend;
            s_hhdm_iv_n++;
        }
    }
    /* insertion sort by base (n small: <= mmap entries + 1) */
    for (i = 1; i < s_hhdm_iv_n; i++) {
        struct bl_hhdm_iv key = s_hhdm_iv[i];
        UINT32 j = i;
        while (j > 0 && s_hhdm_iv[j - 1].base > key.base) {
            s_hhdm_iv[j] = s_hhdm_iv[j - 1];
            j--;
        }
        s_hhdm_iv[j] = key;
    }
    /* coalesce adjacent/overlapping */
    {
        UINT32 w = 0;
        for (i = 0; i < s_hhdm_iv_n; i++) {
            if (w > 0 && s_hhdm_iv[i].base <= s_hhdm_iv[w - 1].end) {
                if (s_hhdm_iv[i].end > s_hhdm_iv[w - 1].end)
                    s_hhdm_iv[w - 1].end = s_hhdm_iv[i].end;
            } else {
                s_hhdm_iv[w++] = s_hhdm_iv[i];
            }
        }
        s_hhdm_iv_n = w;
    }
    return 0;
}

/* True when phys page (4 KiB) is mappable into the WRITABLE HHDM: inside a
 * collected usable interval AND outside the loaded kernel-image envelope
 * (H3 / Decision 5 -- a writable alias of kernel text defeats W^X). */
static int bl_hhdm_page_mapped(UINT64 phys)
{
    UINT32 i;
    if (g_kernel_img_hi > g_kernel_img_lo) {
        UINT64 elo = g_kernel_img_lo & ~(BL_PAGE_4K - 1);
        UINT64 ehi = (g_kernel_img_hi + BL_PAGE_4K - 1) & ~(BL_PAGE_4K - 1);
        if (phys >= elo && phys < ehi) return 0;
    }
    for (i = 0; i < s_hhdm_iv_n; i++)
        if (phys >= s_hhdm_iv[i].base && phys < s_hhdm_iv[i].end)
            return 1;
    return 0;
}

/* True when the whole 2 MiB bucket [base, base+2M) maps as one PS=1 leaf:
 * fully inside the CURRENT usable interval [iv_base, iv_end) AND disjoint from
 * the kernel-image envelope. O(1): the caller sweeps buckets within one sorted
 * + coalesced interval, so "fully usable" == "fully inside this interval" (a
 * bucket straddling a coalescing gap extends past iv_end and is handled as a
 * partial). This replaces an O(intervals)-per-bucket rescan that made the
 * install O((RAM / 2 MiB) * intervals) -- ~billions of comparisons on a large
 * fragmented map. */
static int bl_hhdm_bucket_whole(UINT64 base, UINT64 iv_base, UINT64 iv_end)
{
    UINT64 bend = base + BL_PAGE_2M;
    if (base < iv_base || bend > iv_end) return 0;   /* not fully in this interval */
    if (g_kernel_img_hi > g_kernel_img_lo) {
        UINT64 elo = g_kernel_img_lo & ~(BL_PAGE_4K - 1);
        UINT64 ehi = (g_kernel_img_hi + BL_PAGE_4K - 1) & ~(BL_PAGE_4K - 1);
        if (base < ehi && bend > elo) return 0;
    }
    return 1;
}

/* Allocate one zeroed page-table frame from the arena. Bounds-checked: FATAL
 * on overflow (the worst-case sizing must have undercounted). This is the hard
 * safety net that makes any count/fill mismatch memory-safe, never a silent
 * corruption. */
static UINT64 bl_hhdm_arena_alloc(void)
{
    if (s_hhdm_arena_next >= g_hhdm_arena_pages) {
        boot_fatal(BOOT_ERR_HHDM_FAIL, "HHDM arena exhausted",
                   "leaf install needed more page-table frames than the "
                   "pre-EBS worst-case sizing reserved.");
    }
    UINT64 p = g_hhdm_arena_base + s_hhdm_arena_next * BL_PAGE_4K;
    s_hhdm_arena_next++;
    efi_memset((void *)(UINTN)p, 0, (UINTN)BL_PAGE_4K);
    return p;
}

/* Worst-case arena page-table frame count for the current usable map,
 * independent of exact bucket geometry: the EBS retry can re-snapshot the map
 * between sizing and fill, so the arena must be provably large enough for ANY
 * usable map, not exactly the counted one:
 *   PDPT frames <= occupied 512 GiB slots
 *   PD   frames <= occupied 1 GiB regions
 *   PT   frames <= 2 partial edge buckets per interval + envelope + slack. */
static UINT64 bl_hhdm_worstcase_pages(EFI_MEMORY_DESCRIPTOR *raw,
                                      UINTN raw_size, UINTN raw_dsize)
{
    UINT64 pdpt = 0, pd = 0, pt, env_buckets = 0;
    UINTN off;
    if (s_hhdm_iv_n == 0) return 0;
    /* PDPT/PD count 512 GiB / 1 GiB region occupancy from the UNCAPPED RAW EFI
     * descriptor array -- NOT boot_info.mmap. fill_memory_map() caps/evicts
     * descriptors above BOOT_MMAP_MAX_ENTRIES (bootx64.c), so a region dropped
     * from boot_info during sizing could reappear in the refreshed fill map and
     * demand a PDPT/PD this snapshot never counted. The raw firmware map is the
     * FULL, churn-stable physical layout (a type flip only relabels a
     * descriptor in place; it never moves RAM into an uncounted region).
     * Summing per-descriptor region spans over-counts shared boundaries (safe)
     * and can never undercount the install's DISTINCT-USABLE-region demand,
     * which is a subset of these descriptor-backed regions. */
    for (off = 0; raw_dsize && off + raw_dsize <= raw_size; off += raw_dsize) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)raw + off);
        UINT64 base = d->PhysicalStart;
        UINT64 end  = base + d->NumberOfPages * BL_PAGE_4K;
        if (base >= BL_MM_HHDM_SIZE) continue;
        if (end > BL_MM_HHDM_SIZE) end = BL_MM_HHDM_SIZE;
        if (end <= base) continue;
        pdpt += ((end - 1) >> 39) - (base >> 39) + 1;
        pd   += ((end - 1) >> 30) - (base >> 30) + 1;
    }
    if (g_kernel_img_hi > g_kernel_img_lo)
        env_buckets = ((g_kernel_img_hi - g_kernel_img_lo) >> 21) + 2;
    /* PT frames are MAP-INDEPENDENT. The install recollects intervals from the
     * FINAL post-EBS map, which the pre-EBS refresh makes fresher and possibly
     * MORE fragmented than this snapshot -- new type-flip holes create new
     * partial buckets. Any UEFI map carries at most BOOT_MMAP_MAX_ENTRIES
     * entries, so it can express at most 2 partial edge buckets per entry (plus
     * the retained-PT + arena splices); bound PT by that maximum so a refreshed
     * fragmented fill can never exceed the reserved arena. */
    pt = 2ULL * ((UINT64)BOOT_MMAP_MAX_ENTRIES + 2) + env_buckets + 8;
    return pdpt + pd + pt + 16;                  /* +16 splice/rounding slack */
}

/* PRE-EBS (boot services still available): size + AllocatePages the HHDM
 * arena. AllocateMaxAddress < 4 GiB keeps the arena firmware-identity-mapped
 * (writable under the current CR3) and away from the low loaded kernel. */
static void bl_hhdm_reserve_arena(EFI_MEMORY_DESCRIPTOR *raw,
                                  UINTN raw_size, UINTN raw_dsize)
{
    /* Initialize the arena globals at RUNTIME: the bootloader does not zero
     * .bss, so a static `= 0` would start at firmware poison (0xAF...). Leaving
     * them poison would make setup_page_tables()'s `if (g_hhdm_arena_pages)`
     * true on the no-usable-RAM early-return path and install from a garbage
     * base. Default to "HHDM disabled" until AllocatePages succeeds below. */
    g_hhdm_arena_base  = 0;
    g_hhdm_arena_pages = 0;
    post_code16(POST16_BL_HHDM_PLAN);
    if (bl_hhdm_collect_usable(0) != 0) {
        boot_fatal(BOOT_ERR_HHDM_FAIL, "HHDM interval overflow",
                   "too many usable-RAM intervals to plan the direct map.");
    }
    UINT64 pages = bl_hhdm_worstcase_pages(raw, raw_size, raw_dsize);
    if (pages == 0) {
        serial_early_print("[BOOT] HHDM: no usable RAM to map (skipping)\n");
        return;   /* g_hhdm_arena_pages stays 0 -> install is a no-op */
    }

    /* Low guard the arena must NOT overlap: the loaded kernel image envelope
     * PLUS the PMM frame bitmap. pmm_init() writes that bitmap at a FIXED
     * kernel_end_phys (= __kernel_end, just above the image), sized
     * ceil(highest_usable_phys / 32768) with highest capped at 4 GiB
     * (src/kernel/mm/pmm.c); neither the image (loaded to unclaimed
     * EfiConventionalMemory by load_kernel) nor the bitmap is claimed. */
    UINT64 kguard_lo = g_kernel_img_lo & ~(BL_PAGE_4K - 1);
    /* pmm caps its bitmap coverage at 4 GiB (src/kernel/mm/pmm.c: highest_addr
     * clamped to 0x100000000), so the frame bitmap is NEVER larger than
     * 4 GiB / 4 KiB / 8 = 128 KiB, regardless of how large or fragmented the
     * map is. Use that FIXED maximum -- truncation-proof and churn-proof, no
     * dependence on the sizing snapshot. Guard = kernel image + max bitmap +
     * 1 page for any linker padding between page_up(image end) and the real
     * __kernel_end where pmm actually places the bitmap. */
    UINT64 bitmap_max = BL_PMM_BITMAP_PHYS_CAP / 32768ULL;   /* 128 KiB, page-aligned */
    UINT64 kguard_hi = ((g_kernel_img_hi + BL_PAGE_4K - 1) & ~(BL_PAGE_4K - 1))
                       + bitmap_max + BL_PAGE_4K;

    /* Claim the FREE (EfiConventionalMemory) pages inside the guard so firmware
     * cannot hand them back for the arena REGARDLESS of its AllocatePages
     * placement policy -- the UEFI spec does not require AllocateMaxAddress to
     * be top-down, so relying on a high placement to dodge the low kernel is
     * not portable. The free pages inside the guard are exactly the loaded-
     * kernel pages load_kernel copied into UNCLAIMED conventional memory (the
     * only guard pages an arena could legally land on; firmware-reserved pages
     * it already will not reallocate). Claimed per-conventional-range because
     * AllocateAddress is all-or-nothing and the guard spans mixed types. pmm
     * re-reserves the image + bitmap, so the EfiLoaderData tag is harmless. */
    if (g_kernel_img_hi > g_kernel_img_lo && kguard_hi > kguard_lo) {
        UINTN goff;
        for (goff = 0; raw_dsize && goff + raw_dsize <= raw_size; goff += raw_dsize) {
            EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)raw + goff);
            UINT64 db = d->PhysicalStart;
            UINT64 de = db + d->NumberOfPages * BL_PAGE_4K;
            UINT64 clo = db > kguard_lo ? db : kguard_lo;
            UINT64 che = de < kguard_hi ? de : kguard_hi;
            if (d->Type != EfiConventionalMemory || che <= clo) continue;
            {
                EFI_PHYSICAL_ADDRESS g = clo;
                UINTN gp = (UINTN)((che - clo) / BL_PAGE_4K);
                (void)gBS->AllocatePages(AllocateAddress, EfiLoaderData, gp, &g);
            }
        }
    }

    EFI_PHYSICAL_ADDRESS arena = BL_MM_PML4_PHYS_LIMIT - 1;  /* max addr < 4 GiB */
    EFI_STATUS st = gBS->AllocatePages(AllocateMaxAddress, EfiLoaderData,
                                       (UINTN)pages, &arena);
    if (EFI_ERROR(st)) {
        boot_fatal(BOOT_ERR_ALLOC_FAIL, "HHDM arena AllocatePages failed",
                   "could not reserve page-table frames for the direct map.");
    }

    /* Disjointness safety net: with the guard claimed above, the arena cannot
     * overlap the kernel image or the PMM bitmap; this catches the residual
     * case where the guard claim failed. Also rejects the low 1 MiB (IVT/BDA,
     * PT_PML4 at 0x70000, boot_info at 0x10000, the AP envelope). */
    {
        UINT64 alo = (UINT64)arena;
        UINT64 ahi = alo + pages * BL_PAGE_4K;
        int overlap = (kguard_hi > kguard_lo &&
                       alo < kguard_hi && ahi > kguard_lo) ||
                      (alo < 0x100000ULL);
        if (overlap) {
            boot_fatal(BOOT_ERR_HHDM_FAIL, "HHDM arena overlaps reserved memory",
                       "AllocatePages returned an arena overlapping the loaded "
                       "kernel image, the PMM bitmap region, or the low "
                       "bring-up region.");
        }
        g_hhdm_arena_base  = alo;
        g_hhdm_arena_pages = pages;
    }
    post_code16(POST16_BL_HHDM_ARENA_OK);
    serial_early_print("[BOOT] HHDM: arena reserved\n");
}

/* Retag the arena interval to EfiReservedMemoryType (UEFI_MMAP_RESERVED) in
 * boot_info.mmap so pmm_init() keeps it USED (it frees Loader/BootServices).
 * POST-EBS memory-only edit: a pre-EBS retag would be clobbered by the
 * EBS-retry loop's fill_memory_map() re-snapshot. Splits the covering entry
 * into up to three; capacity-checked, fail-closed. */
static void bl_hhdm_retag_arena_reserved(void)
{
    struct boot_mmap_entry *m = g_boot_info_ptr->mmap;
    UINT64 alo, ahi;
    UINT32 i;
    if (!g_hhdm_arena_pages) return;
    alo = g_hhdm_arena_base;
    ahi = g_hhdm_arena_base + g_hhdm_arena_pages * BL_PAGE_4K;
    for (i = 0; i < g_boot_info_ptr->mmap_count; i++) {
        UINT64 ebase = m[i].base_addr;
        UINT64 eend  = ebase + m[i].length;
        UINT64 olo, ohi;
        UINT32 want_left, want_right, extra, idx;
        UINT32 orig_type, orig_utype;
        UINT64 orig_attr;
        if (ahi <= ebase || alo >= eend) continue;   /* disjoint */
        olo = alo > ebase ? alo : ebase;
        ohi = ahi < eend ? ahi : eend;
        want_left  = (olo > ebase) ? 1u : 0u;
        want_right = (ohi < eend) ? 1u : 0u;
        extra = want_left + want_right;
        if (g_boot_info_ptr->mmap_count + extra > BOOT_MMAP_MAX_ENTRIES) {
            boot_fatal(BOOT_ERR_MMAP_OVERFLOW, "HHDM retag overflow",
                       "no room to split the mmap for the arena reservation.");
        }
        orig_type  = m[i].type;
        orig_utype = m[i].uefi_memory_type;
        orig_attr  = m[i].attribute;
        if (extra) {                 /* open a gap of `extra` after entry i */
            UINT32 k;
            for (k = g_boot_info_ptr->mmap_count; k > i + 1; k--)
                m[k + extra - 1] = m[k - 1];
        }
        idx = i;
        if (want_left) {
            m[idx].base_addr        = ebase;
            m[idx].length           = olo - ebase;
            m[idx].type             = orig_type;
            m[idx].uefi_memory_type = orig_utype;
            m[idx].attribute        = orig_attr;
            idx++;
        }
        m[idx].base_addr        = olo;                /* reserved middle */
        m[idx].length           = ohi - olo;
        m[idx].type             = 2;                  /* simplified: reserved */
        m[idx].uefi_memory_type = BL_UEFI_MMAP_RESERVED;
        m[idx].attribute        = orig_attr;
        idx++;
        if (want_right) {
            m[idx].base_addr        = ohi;
            m[idx].length           = eend - ohi;
            m[idx].type             = orig_type;
            m[idx].uefi_memory_type = orig_utype;
            m[idx].attribute        = orig_attr;
            idx++;
        }
        g_boot_info_ptr->mmap_count += extra;
        i = idx - 1;   /* resume after the inserted block (defensive: arena
                        * may span a coalesced boundary) */
    }
}

/* Install HHDM leaves into PT_PML4 slots 273..400. Sweeps the collected
 * usable intervals in ascending physical order, emitting a PS=1 2 MiB PDE for
 * a whole bucket and a 4 KiB PT for a partial one, NX + Writable + supervisor
 * (never User). PDPT/PD/PT frames come from the arena. */
static void bl_hhdm_install_leaves(void)
{
    UINT64 *pml4 = (UINT64 *)(UINTN)PT_PML4;
    UINT64 cur_slot = ~0ULL;
    UINT64 cur_gib  = ~0ULL;
    UINT64 last_bucket = ~0ULL;
    UINT64 *cur_pdpt = (void *)0;
    UINT64 *cur_pd   = (void *)0;
    UINT32 iv;

    for (iv = 0; iv < s_hhdm_iv_n; iv++) {
        UINT64 ivend = s_hhdm_iv[iv].end;
        UINT64 b;
        for (b = s_hhdm_iv[iv].base & ~(BL_PAGE_2M - 1); b < ivend; b += BL_PAGE_2M) {
            UINT64 slot = BL_HHDM_PML4_SLOT + (b >> 39);
            UINT64 pdpt_i = (b >> 30) & 0x1ffULL;
            UINT64 pd_i   = (b >> 21) & 0x1ffULL;
            if (b == last_bucket) continue;   /* bucket shared by two intervals */

            if (slot > BL_HHDM_PML4_SLOT_LAST) {
                boot_fatal(BOOT_ERR_HHDM_FAIL, "HHDM slot out of window",
                           "physical RAM exceeds the 64 TiB HHDM window.");
            }
            if (slot != cur_slot) {
                UINT64 pdpt_phys = (pml4[slot] & BL_PTE_P)
                    ? (pml4[slot] & BL_PADDR_MASK) : bl_hhdm_arena_alloc();
                if (!(pml4[slot] & BL_PTE_P))
                    pml4[slot] = pdpt_phys | BL_PTE_P | BL_PTE_RW;  /* table: no NX/User */
                cur_pdpt = (UINT64 *)(UINTN)pdpt_phys;
                cur_slot = slot;
                cur_gib  = ~0ULL;
            }
            if ((b >> 30) != cur_gib) {
                UINT64 pd_phys = (cur_pdpt[pdpt_i] & BL_PTE_P)
                    ? (cur_pdpt[pdpt_i] & BL_PADDR_MASK) : bl_hhdm_arena_alloc();
                if (!(cur_pdpt[pdpt_i] & BL_PTE_P))
                    cur_pdpt[pdpt_i] = pd_phys | BL_PTE_P | BL_PTE_RW;
                cur_pd  = (UINT64 *)(UINTN)pd_phys;
                cur_gib = b >> 30;
            }
            last_bucket = b;

            if (bl_hhdm_bucket_whole(b, s_hhdm_iv[iv].base, ivend)) {
                cur_pd[pd_i] = b | BL_PTE_P | BL_PTE_RW | BL_PTE_PS | BL_PTE_NX;
            } else {
                UINT64 pt_phys;
                UINT64 pg;
                if ((cur_pd[pd_i] & BL_PTE_P) && !(cur_pd[pd_i] & BL_PTE_PS)) {
                    pt_phys = cur_pd[pd_i] & BL_PADDR_MASK;
                } else {
                    pt_phys = bl_hhdm_arena_alloc();
                    cur_pd[pd_i] = pt_phys | BL_PTE_P | BL_PTE_RW;
                }
                UINT64 *pt = (UINT64 *)(UINTN)pt_phys;
                for (pg = 0; pg < 512; pg++) {
                    UINT64 phys = b + pg * BL_PAGE_4K;
                    if (bl_hhdm_page_mapped(phys))
                        pt[pg] = phys | BL_PTE_P | BL_PTE_RW | BL_PTE_NX;
                }
            }
        }
    }
}

/* ============================================================================
 * Kernel boot stack (TODO-10 sec32)
 *
 * Until this existed, jump_to_kernel loaded CR3 and CALLED the kernel entry
 * without ever writing RSP, so Phase 0 and Phase 1 ran on the firmware's own
 * EfiLoaderData stack. pmm_init frees every LoaderCode/Data and
 * BootServicesCode/Data region into the free pool, and nothing reserved that
 * stack -- vmm_init and heap_init then allocated from a pool containing the
 * run the kernel was standing on. That it booted was placement luck, not a
 * property anything enforced, and it is invisible under an emulator that
 * happens to place the loader stack away from the first free frames.
 *
 * A memory-map descriptor is NOT usable as the stack's boundary: mmap_emit_
 * segment coalesces adjacent segments with the same UEFI type and attributes,
 * so the EfiLoaderData descriptor holding RSP can span several unrelated
 * firmware allocations. Only an allocation the loader makes itself has bounds
 * it can honestly publish, which is why this is an explicit page run rather
 * than an inferred window around the firmware's RSP.
 *
 * Below 4 GiB for the same reason as every other kernel-consumed allocation
 * here: the kernel dereferences it through the boot identity map, which
 * covers the first 4 GiB only.
 * ============================================================================ */

/* Placement retries before giving up (see bl_kstack_reserve). Small on
 * purpose: each rejected run stays allocated for the duration, and a firmware
 * that hands back the user PT window several times running is not going to
 * stop. */
#define BL_KSTACK_PLACEMENT_TRIES 8

static UINT64 g_kstack_base;   /* 0 until reserved; page-aligned when set */

/* Where the kernel boot stack may NOT be. AllocateMaxAddress bounds only the
 * TOP of a run and firmware need not allocate top-down, so every fixed boot
 * structure the loader writes WITHOUT an AllocatePages claim is a candidate
 * collision, not just the user page-table window: a base such as 0x60000
 * passed the window-only check, and setup_page_tables() then zeroed and
 * wrote the live
 * PML4..PD3 at 0x70000-0x75fff INSIDE the poisoned run the kernel was about
 * to grow its stack through.
 *   - the low 1 MiB: IVT/BDA, PT_PML4..PT_PD3 at 0x70000, boot_info at
 *     0x10000, the AP trampoline envelope (same exclusion the HHDM arena uses);
 *   - the kernel image plus the PMM bitmap pmm_init() writes right above it
 *     (same envelope arithmetic as bl_hhdm_reserve: image + 128 KiB max
 *     bitmap + one page of linker padding), which load_kernel() loads into
 *     unclaimed EfiConventionalMemory and never claims;
 *   - the user page-table window, re-pointed per process by the kernel.
 * The kernel mirrors the low-memory refusal in boot_stack_validate() and
 * boot_reserved's overlap boot_fatal covers the rest consumer-side. */
static int bl_kstack_placement_ok(UINT64 addr)
{
    UINT64 lo = addr;
    UINT64 hi = addr + BL_KSTACK_SIZE;
    UINT64 kguard_lo = g_kernel_img_lo & ~(BL_PAGE_4K - 1);
    UINT64 kguard_hi = ((g_kernel_img_hi + BL_PAGE_4K - 1) & ~(BL_PAGE_4K - 1))
                       + (BL_PMM_BITMAP_PHYS_CAP / 32768ULL) + BL_PAGE_4K;

    if (lo < 0x100000ULL)
        return 0;
    if (g_kernel_img_hi > g_kernel_img_lo &&
        lo < kguard_hi && hi > kguard_lo)
        return 0;
    if (lo < BL_USER_PT_WINDOW_END && hi > BL_USER_PT_WINDOW_BASE)
        return 0;
    return 1;
}

static void bl_kstack_reserve(void)
{
    EFI_PHYSICAL_ADDRESS addr = 0xFFFFFFFFull;
    UINTN pages = BL_KSTACK_SIZE / EFI_PAGE_SIZE;
    EFI_STATUS status;
    UINT64 *p;
    UINT64 *end;
    EFI_PHYSICAL_ADDRESS rejected[BL_KSTACK_PLACEMENT_TRIES];
    UINTN rejected_count = 0;
    UINTN attempt;

    /* Placement matters as much as size. AllocateMaxAddress bounds only the
     * TOP of the run, and the UEFI spec does not require firmware to allocate
     * top-down, so a conforming firmware may hand back memory inside the
     * kernel's user page-table window (re-pointed per process, so a stack
     * there would vanish under the first user CR3), the low 1 MiB where the
     * fixed page tables and boot_info live, or the kernel image envelope --
     * see bl_kstack_placement_ok for the full exclusion set.
     *
     * Retry rather than fail: hold each rejected run ALLOCATED so firmware
     * cannot return the same pages again, then free them all once a good
     * placement is found. Freeing before retrying would let the allocator
     * hand back the identical range forever. */
    for (attempt = 0; attempt < BL_KSTACK_PLACEMENT_TRIES; attempt++) {
        addr = 0xFFFFFFFFull;
        status = gBS->AllocatePages(AllocateMaxAddress, EfiLoaderData, pages, &addr);
        if (EFI_ERROR(status))
            break;
        if (bl_kstack_placement_ok((UINT64)addr))
            break;  /* disjoint from every fixed boot structure -- accept */
        rejected[rejected_count++] = addr;
        status = EFI_NOT_FOUND;  /* so a loop that runs out reports failure */
    }
    while (rejected_count > 0)
        gBS->FreePages(rejected[--rejected_count], pages);

    if (EFI_ERROR(status)) {
        /* Fail HERE, pre-EBS, where boot_fatal can still reach the console,
         * NVRAM history and ResetSystem. The alternative -- carrying on and
         * letting jump_to_kernel skip the switch -- would silently restore the
         * exact defect this run removes, on the machines least able to report
         * it. */
        boot_fatal(BOOT_ERR_ALLOC_FAIL, "kernel stack AllocatePages failed",
                   "could not allocate a kernel boot stack below 4 GiB and "
                   "outside the low 1 MiB, the kernel image envelope and the "
                   "user page-table window.");
    }

    /* Poison the WHOLE run, guard page included. The kernel scans upward from
     * the first usable byte for the lowest non-poison qword to recover how
     * deep Phase 0/1 actually went; poisoning the guard too means a scan that
     * reports a hit inside the guard is unambiguous evidence of an overflow
     * that predates the guard-page install. */
    p = (UINT64 *)(UINTN)addr;
    end = (UINT64 *)(UINTN)(addr + BL_KSTACK_SIZE);
    while (p < end)
        *p++ = BOOT_KSTACK_POISON;

    g_kstack_base = (UINT64)addr;
    g_boot_info_ptr->kstack_base       = (UINT64)addr;
    g_boot_info_ptr->kstack_size       = BL_KSTACK_SIZE;
    g_boot_info_ptr->kstack_guard_size = BL_KSTACK_GUARD_SIZE;

    serial_early_print("[BOOT] kernel stack: reserved and poisoned\n");
}

static void setup_page_tables(void)
{
    boot_set_section(BOOT_SECTION_BL_PAGETABLES);
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

    /* Higher-half direct map (section 2): additive install into PML4 slots
     * 273-400 while the kernel still runs LOW from the identity map above.
     * POST-EBS: memory writes only (no boot services). Enable EFER.NXE first
     * (NX leaves), retag the pre-EBS arena RESERVED, then install the leaves. */
    bl_enable_nxe();
    post_code16(POST16_BL_HHDM_NXE);
    bl_hhdm_retag_arena_reserved();
    post_code16(POST16_BL_HHDM_INSTALL);
    if (g_hhdm_arena_pages) {
        s_hhdm_arena_next = 0;
        if (bl_hhdm_collect_usable(1) != 0) {
            boot_fatal(BOOT_ERR_HHDM_FAIL, "HHDM interval overflow (install)",
                       "too many usable-RAM intervals to install the direct map.");
        }
        bl_hhdm_install_leaves();
    }
    post_code16(POST16_BL_HHDM_OK);
    serial_early_print("[BOOT] HHDM: direct map installed\n");
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

    /* Higher-half direct map verify (section 2): now that CR3 carries the HHDM
     * (PML4 slots 273-400), prove it RESOLVES to the same physical bytes as the
     * identity map before handoff -- section 9 will route every kernel
     * page-table dereference through it. Read-only (mutates nothing): compare
     * the arena's first word read via the identity map (slot 0) against the
     * same frame read through its HHDM alias. The arena is < 4 GiB
     * (identity-mapped) and HHDM-mapped by construction, and its first page
     * holds a live PDPT (non-zero). The HHDM leaf is NX, so a successful read
     * also proves EFER.NXE latched. A misinstalled map either mismatches (halt
     * below) or #PFs with no IDT -> reset -> the smoke test's missing C:\>
     * prompt catches it. */
    if (g_hhdm_arena_pages) {
        volatile UINT64 *id = (volatile UINT64 *)(UINTN)g_hhdm_arena_base;
        volatile UINT64 *hh =
            (volatile UINT64 *)(UINTN)(BL_MM_HHDM_BASE + g_hhdm_arena_base);
        if (*id != *hh) {
            /* Deliberately NOT boot_fatal here: this runs under the freshly
             * loaded PT_PML4, which maps only the low 4 GiB identity + the HHDM
             * (RAM). boot_fatal dereferences gST and calls RuntimeServices
             * (SetVariable/GetTime/ResetSystem) + boot_history_append and may
             * touch the GOP framebuffer -- any of which can live ABOVE 4 GiB and
             * is unmapped here, turning a controlled halt into a triple fault.
             * A bare serial write + cli;hlt has no post-CR3 mapping dependency. */
            serial_early_print("[FAIL] HHDM: identity vs direct-map readback "
                               "mismatch -- halting\n");
            for (;;) __asm__ volatile ("cli; hlt");
        }
        serial_early_print("[BOOT] HHDM: direct map verified (identity == alias)\n");
    }

    /* Kernel boot stack handoff (TODO-10 sec32). bl_kstack_reserve() ran
     * pre-EBS and boot_fatal'd on failure, so a zero base here means the
     * publication itself was corrupted after the fact. Refuse rather than
     * fall back to the firmware stack: the fallback is the defect. Same bare
     * serial + cli;hlt shape as the HHDM check above and for the same reason
     * -- we are past EBS and running under PT_PML4, where boot_fatal's gST /
     * RuntimeServices / framebuffer dereferences may be unmapped. */
    if (g_kstack_base == 0 || (g_kstack_base & (EFI_PAGE_SIZE - 1)) != 0) {
        serial_early_print("[FAIL] kernel stack: unpublished or misaligned "
                           "-- halting\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }

    /* Switch RSP to the TOP of that run and call the kernel entry from it.
     * This deliberately abandons every loader frame below us: the kernel
     * never returns (entry is followed by an unreachable halt on both sides),
     * so nothing above the new stack is live, and the firmware's stack
     * becomes ordinary reclaimable memory the moment we leave it.
     *
     * One asm block, because no C statement may execute between the RSP write
     * and the call -- the compiler's frame for this function lives on the OLD
     * stack, so any spill or reload after the switch would read memory the new
     * RSP does not describe. RBP is zeroed so a frame-pointer unwinder walking
     * out of kernel_main terminates instead of chasing loader frames.
     *
     * The top is 16-byte aligned by construction (page-aligned base plus a
     * 4 KiB-multiple size), so the CALL's pushed return address leaves
     * RSP % 16 == 8 at the callee's first instruction, which is what the
     * SysV AMD64 ABI requires. The bootloader is built --target=x86_64-elf,
     * so this indirect call is SysV: magic in RDI, boot_info in RSI.
     *
     * EVERY operand is pinned to a NAMED register, and that is a correctness
     * requirement rather than a style choice. With generic "r" operands the
     * compiler is free to place the entry pointer or the stack top in RBP,
     * and the `xorl %ebp, %ebp` above executes BETWEEN the operand being
     * materialised and being used -- so a `callq *%rbp` would jump to address
     * 0. This loader is compiled without -fno-omit-frame-pointer
     * (src/boot/uefi/Makefile), so RBP is genuinely allocatable and only the
     * compiler's current choice was keeping this correct: the shipped clang-19
     * artifact happens to pick RAX and RBX, which is exactly what is pinned
     * here, so the emitted code is unchanged and a version, flag or LTO change
     * can no longer silently move it. RBP is named in the clobber list for the
     * same reason: the compiler is now told the register is destroyed instead
     * of it being an undeclared write. */
    __asm__ volatile (
        "movq %0, %%rsp\n\t"
        "xorl %%ebp, %%ebp\n\t"
        "callq *%1\n\t"
        "1: hlt\n\t"
        "jmp 1b\n\t"
        :
        : "a"(g_kstack_base + BL_KSTACK_SIZE),
          "b"((UINT64)(UINTN)entry),
          "D"(0x55454649ULL),
          "S"((UINT64)(UINTN)g_boot_info_ptr)
        : "memory", "rbp"
    );

    /* Unreachable -- the asm above never falls through. */
    for (;;) __asm__ volatile("hlt");
}

/* ============================================================================
 * Pre-ExitBootServices USB Device Discovery (Phase A)
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
 * Allocate persistent xHCI DMA structures
 *
 * Find the xHCI controller via PCI scan, read capability registers to
 * determine structure sizes, and allocate DCBAA, scratchpad buffers,
 * command ring, event ring, and ERST using EfiLoaderData pages.
 * These pages survive ExitBootServices; kernel must reserve them in PMM.
 * ============================================================================ */

/* Helper: allocate one 4 KiB page as EfiLoaderData and track it */
static UINT64 bl_alloc_dma_page(struct boot_usb_controller *ctrl)
{
    EFI_PHYSICAL_ADDRESS addr = 0xFFFFFFFF;  /* ceiling: keep DMA below 4 GiB */
    EFI_STATUS status;
    UINTN i;

    /* Page-aligned DMA memory, capped below 4 GiB. The bootloader identity
     * map only covers the low 4 GiB and a 32-bit-only xHCI cannot address
     * above it, so AllocateMaxAddress keeps every inherited DMA page reachable
     * by both the controller and the pre-EBS identity map. */
    status = gBS->AllocatePages(AllocateMaxAddress, EfiLoaderData, 1, &addr);
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

    /* ---- Read BAR0 to get MMIO base. Decode the BAR width before touching
     *      BAR1: per PCI, memory-BAR bits [2:1] select width -- 00 = 32-bit
     *      (BAR1 is an unrelated BAR), 10 = 64-bit (BAR1 is the high dword).
     *      Treating a 32-bit BAR1 as the high half fabricates an MMIO base,
     *      and the takeover then issues register writes at a bogus address. */
    bar0 = bl_pci_read32((UINT8)bus, dev, func, 0x10);
    if (bar0 & 0x01) {
        serial_early_print("[BOOT] xHCI DMA: BAR0 is I/O -- skipping\n");
        return;
    }
    {
        UINT32 bar_type = (bar0 >> 1) & 0x03;
        if (bar_type == 0x02) {
            /* 64-bit memory BAR: BAR1 holds the upper 32 bits */
            bar1 = bl_pci_read32((UINT8)bus, dev, func, 0x14);
            mmio = (UINT64)(bar0 & 0xFFFFFFF0) | ((UINT64)bar1 << 32);
        } else if (bar_type == 0x00) {
            /* 32-bit memory BAR: high half is zero; BAR1 is a separate BAR */
            bar1 = 0;
            mmio = (UINT64)(bar0 & 0xFFFFFFF0);
        } else {
            serial_early_print("[BOOT] xHCI DMA: BAR0 reserved type -- skipping\n");
            return;
        }
    }
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
        sp_array_addr = 0xFFFFFFFF;  /* ceiling: keep DMA below 4 GiB */
        sp_status = gBS->AllocatePages(AllocateMaxAddress, EfiLoaderData,
                                       (sp_count * 8 + 4095) / 4096, &sp_array_addr);
        if (EFI_ERROR(sp_status) || sp_array_addr == 0) {
            serial_early_print("[BOOT] xHCI DMA: scratchpad array alloc failed\n");
            ctrl->alloc_fail_status = (UINT32)(sp_status & 0xFFFFFFFF);
            ctrl->alloc_fail_page   = ctrl->dma_page_count;
            return;
        }
        efi_memset((void *)(UINTN)sp_array_addr, 0, sp_count * 8);
        ctrl->scratchpad_array_phys = sp_array_addr;
        if (ctrl->dma_page_count < BOOT_USB_MAX_DMA_PAGES)
            ctrl->dma_pages[ctrl->dma_page_count++] = sp_array_addr;

        /* Scratchpad buffer pages: allocate all contiguously */
        sp_base_addr = 0xFFFFFFFF;  /* ceiling: keep DMA below 4 GiB */
        sp_status = gBS->AllocatePages(AllocateMaxAddress, EfiLoaderData,
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
        serial_early_print_hex64(sp_base_addr);
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
    serial_early_print_hex64(ctrl->dcbaa_phys);
    serial_early_print(", CmdRing=0x");
    serial_early_print_hex64(ctrl->cmd_ring_phys);
    serial_early_print(")\n");
}

/* ============================================================================
 * USBLEGSUP handoff + controller takeover
 *
 * Take xHCI ownership from BIOS/firmware, halt the controller, reset it,
 * configure it to use our persistent DMA structures (allocated earlier), and
 * start it. After this, the controller runs with our DCBAA/rings and is ready
 * for device enumeration.
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
/* Pre-serial breadcrumb for the panic-evidence pin. 8-bit on purpose: it runs
 * before serial_early_init, and post_code16 would drag serial_early_print in
 * with it. This loader does NOT zero .bss (see the note near the top of the
 * file -- firmware pool-poisons it with 0xAF), so s_serial_port can hold
 * 0xAFAF there, which is NON-ZERO and sails past serial_early_putchar's
 * `if (!s_serial_port) return;` guard straight into inb/outb on an arbitrary
 * I/O port. post_code is a bare `outb $0x80` and touches no static state. */
#define POST_PANIC_PAGE         0x0A

/* 16-bit POST codes for UEFI bootloader (0xB000 range) */
#define POST16_BL_ENTRY         0xB001
#define POST16_BL_PANIC_PAGE    0xB002  /* panic-evidence page pin attempted */
#define POST16_BL_GOP           0xB010
#define POST16_BL_KERNEL_OPEN   0xB020
#define POST16_BL_KERNEL_LOAD   0xB021
#define POST16_BL_RSDP          0xB030
#define POST16_BL_MEMMAP        0xB040
#define POST16_BL_USB_DISC          0xB080
#define POST16_BL_USB_DISC_OK       0xB081
#define POST16_BL_XHCI_DMA          0xB082  /* xHCI DMA alloc (§TODO-20) */
#define POST16_BL_XHCI_DMA_OK       0xB083
#define POST16_BL_XHCI_TAKEOVER     0xB084  /* xHCI controller takeover */
#define POST16_BL_XHCI_TAKEOVER_OK  0xB085
#define POST16_BL_BOOT_DEV 0xB090 /* Boot device identification */
#define POST16_BL_BOOT_DEV_OK       0xB091
/* POST16_BL_BOOT_FS / _OK (0xB092/0xB093) defined near top -- used by parse_boot_conf */
#define POST16_BL_EXIT_BS       0xB050
#define POST16_BL_PAGE_TABLES   0xB060
#define POST16_BL_KERNEL_JUMP   0xB070
#define POST16_BL_UKI_DETECT    0xB0A0  /* UKI .linux section probe */
#define POST16_BL_UKI_DETECT_OK 0xB0A1  /* UKI sections found and pinned */
/* Extended boot vars: BootCurrent attrs + BootOptionSupport
 * + OsIndicationsSupported (UEFI 2.10 spec 3.1.3 / 3.1.4 / 8.5.4). */
#define POST16_BL_BOOT_VAR_EXT    0xB0A2
#define POST16_BL_BOOT_VAR_EXT_OK 0xB0A3
/* BOOTX64.EFI on-disk self-measurement (measured-boot loader attribution). */
#define POST16_BL_SELF_MEASURE    0xB0A4
#define POST16_BL_SELF_MEASURE_OK 0xB0A5
/* PCR 4 correlation: entry, and the log-match verdict specifically. A boot that
 * halts between them is one that reached the event-log walk and died there.
 * The second is deliberately not named _AGREE: no PCR is read or replayed on
 * this path, so the marker records a log match, not TPM-confirmed agreement. */
#define POST16_BL_PCR4_CORRELATE    0xB0A8
#define POST16_BL_PCR4_LOG_MATCH    0xB0A9

/* PE/COFF structures for UKI section walk.
 * Reference: Microsoft PE/COFF Specification, MS-DOS stub at offset 0,
 * e_lfanew at offset 0x3C, then "PE\0\0" + IMAGE_FILE_HEADER +
 * IMAGE_OPTIONAL_HEADER (variable size) + IMAGE_SECTION_HEADER[]. */
#define PE_DOS_MAGIC   0x5A4D       /* "MZ" */
#define PE_NT_MAGIC    0x00004550   /* "PE\0\0" */
#define PE_LFANEW_OFF  0x3C
#define PE_SECTION_NAME_LEN 8

struct pe_coff_header {
    UINT16 machine;
    UINT16 num_sections;
    UINT32 timestamp;
    UINT32 sym_table_ptr;
    UINT32 num_symbols;
    UINT16 opt_header_size;
    UINT16 characteristics;
} __attribute__((packed));

struct pe_section_header {
    UINT8  name[PE_SECTION_NAME_LEN];
    UINT32 virtual_size;
    UINT32 virtual_address;
    UINT32 raw_size;
    UINT32 raw_ptr;
    UINT32 reloc_ptr;
    UINT32 linenum_ptr;
    UINT16 num_reloc;
    UINT16 num_linenum;
    UINT32 characteristics;
} __attribute__((packed));

/* Compare 8-byte PE section name (NUL-padded, not NUL-terminated)
 * against a literal. Returns 1 on match. */
static int pe_section_name_eq(const UINT8 *sec_name, const char *want)
{
    UINTN i;
    for (i = 0; i < PE_SECTION_NAME_LEN; i++) {
        UINT8 wc = (UINT8)want[i];
        if (sec_name[i] != wc)
            return 0;
        if (wc == 0)
            return 1;  /* matched the literal NUL terminator */
    }
    return 1;  /* name fills all 8 bytes */
}

/* Walk the LoadedImage's PE section table for UKI sections per the
 * UAPI Group Unified Kernel Image specification. If a `.linux`
 * section is found, populate g_uki_kernel_ptr/_size; populate
 * g_uki_cmdline (ptr+size) and g_uki_osrel (ptr+size) if their
 * sections are present too.
 * Strict bounds-checking: e_lfanew within image bounds, optional
 * header size within remaining bounds, section table fully contained,
 * each section's data fully contained. UEFI guarantees ImageBase
 * remains valid until ExitBootServices, so this scan is safe pre-EBS.
 * Failure modes (header out of bounds, no `.linux`, etc.) leave
 * g_uki_* NULL and the caller falls back to the split path. */
/* Reset all UKI globals to NULL/0. EDK2 DEBUG fills uninitialized BSS
 * with 0xAF; without this, a non-UKI boot can read poison pointers as
 * if a `.linux` section had been found, and load_kernel can follow
 * the UKI fast path with g_uki_kernel_ptr=0xAFAFAFAFAFAFAFAF.  The
 * caller must invoke this UNCONDITIONALLY at entry --
 * detect_uki_sections() is gated on LoadedImage->DeviceHandle, so a
 * fallback path that lacks DeviceHandle would otherwise leave the
 * statics at the poison pattern. */
static void reset_uki_sections(void)
{
    g_uki_kernel_ptr = (UINT8 *)0;
    g_uki_kernel_size = 0;
    g_uki_cmdline_ptr = (UINT8 *)0;
    g_uki_cmdline_size = 0;
    g_uki_osrel_ptr = (UINT8 *)0;
    g_uki_osrel_size = 0;
    g_uki_initrd_ptr = (UINT8 *)0;
    g_uki_initrd_size = 0;
    g_uki_initrd_phys = 0;
    g_uki_recovery_ptr = (UINT8 *)0;
    g_uki_recovery_size = 0;
    g_uki_recovery_phys = 0;
    g_uki_modules_ptr = (UINT8 *)0;
    g_uki_modules_size = 0;
    g_uki_modules_phys = 0;
}

static void detect_uki_sections(EFI_LOADED_IMAGE_PROTOCOL *li)
{
    /* The reset is also called unconditionally at efi_main entry; this
     * second call is a defense-in-depth so callers that re-invoke
     * detect_uki_sections() never observe partial state. */
    reset_uki_sections();

    if (!li || !li->ImageBase || li->ImageSize < 0x100)
        return;
    UINT8 *base = (UINT8 *)li->ImageBase;
    UINTN size = (UINTN)li->ImageSize;

    /* MS-DOS magic at offset 0. */
    if (base[0] != 'M' || base[1] != 'Z')
        return;

    /* e_lfanew at offset 0x3C; 4-byte LE; bounded by image size. */
    if (size < PE_LFANEW_OFF + 4)
        return;
    UINT32 e_lfanew = *(UINT32 *)(base + PE_LFANEW_OFF);
    if (e_lfanew + 4 + sizeof(struct pe_coff_header) > size)
        return;

    /* "PE\0\0" signature. */
    UINT32 pe_sig = *(UINT32 *)(base + e_lfanew);
    if (pe_sig != PE_NT_MAGIC)
        return;

    /* COFF FileHeader follows the PE signature. */
    struct pe_coff_header *coff =
        (struct pe_coff_header *)(base + e_lfanew + 4);
    UINTN sec_count = coff->num_sections;
    UINTN opt_size = coff->opt_header_size;
    if (sec_count == 0 || sec_count > 96)  /* PE spec caps; sanity */
        return;

    UINTN sec_table_off = e_lfanew + 4 + sizeof(struct pe_coff_header) + opt_size;
    UINTN sec_table_size = sec_count * sizeof(struct pe_section_header);
    if (sec_table_off + sec_table_size > size)
        return;

    struct pe_section_header *sections =
        (struct pe_section_header *)(base + sec_table_off);
    UINTN i;
    for (i = 0; i < sec_count; i++) {
        struct pe_section_header *sec = &sections[i];
        UINT32 va = sec->virtual_address;
        UINT32 vsize = sec->virtual_size;
        if (vsize == 0)
            continue;
        /* Section data must fall within the loaded image. */
        if (va >= size || vsize > size || (UINTN)va + (UINTN)vsize > size)
            continue;
        UINT8 *data = base + va;

        if (pe_section_name_eq(sec->name, ".linux")) {
            g_uki_kernel_ptr = data;
            g_uki_kernel_size = vsize;
        } else if (pe_section_name_eq(sec->name, ".cmdline")) {
            g_uki_cmdline_ptr = data;
            g_uki_cmdline_size = vsize;
        } else if (pe_section_name_eq(sec->name, ".osrel")) {
            g_uki_osrel_ptr = data;
            g_uki_osrel_size = vsize;
        } else if (pe_section_name_eq(sec->name, ".initrd")) {
            g_uki_initrd_ptr = data;
            g_uki_initrd_size = vsize;
        } else if (pe_section_name_eq(sec->name, ".recovery")) {
            g_uki_recovery_ptr = data;
            g_uki_recovery_size = vsize;
        } else if (pe_section_name_eq(sec->name, ".modules")) {
            g_uki_modules_ptr = data;
            g_uki_modules_size = vsize;
        }
    }
}

/* Copy a single UKI payload section out of the LoadedImage region
 * into AllocatePages-allocated EfiLoaderData pages. The returned
 * physical address survives ExitBootServices (EfiLoaderData is
 * reclaimable by the kernel PMM but persists through EBS); the
 * LoadedImage region itself is technically EfiBootServicesCode +
 * EfiLoaderData depending on the loader, so copying out is the safe
 * pattern that matches how load_kernel handles g_uki_kernel_ptr.
 *
 * Returns 0 on success, EFI_STATUS error code on failure. On
 * failure, *out_phys is left zero so the boot_info field stays at
 * the "section absent" sentinel rather than reporting a bogus
 * pointer (UEFI design F3 fail-closed pattern).
 *
 * Sections must be > 0 bytes and < 256 MiB (sanity ceiling -- a
 * legitimate initrd is typically a few MiB; a multi-hundred-MiB
 * payload is either an attack or a build mistake; either way the
 * boot must abort visibly rather than try to allocate it). */
#define UKI_PAYLOAD_MAX_BYTES (256ULL * 1024 * 1024)

static EFI_STATUS uki_copy_payload(const char *name,
                                   const UINT8 *src, UINTN src_size,
                                   UINT64 *out_phys)
{
    *out_phys = 0;
    if (!src || src_size == 0)
        return EFI_SUCCESS;  /* absent section -- not an error */
    if (src_size > UKI_PAYLOAD_MAX_BYTES) {
        serial_early_print("[BOOT] UKI ");
        serial_early_print(name);
        serial_early_print(" payload exceeds 256 MiB sanity cap; refusing\r\n");
        return EFI_INVALID_PARAMETER;
    }

    UINTN pages = (src_size + 0xFFF) >> 12;
    EFI_PHYSICAL_ADDRESS phys = 0;
    EFI_STATUS status = gBS->AllocatePages(AllocateAnyPages,
                                           EfiLoaderData,
                                           pages, &phys);
    if (EFI_ERROR(status)) {
        serial_early_print("[BOOT] UKI ");
        serial_early_print(name);
        serial_early_print(": AllocatePages failed\r\n");
        return status;
    }
    efi_memcpy((void *)(UINTN)phys, src, src_size);
    *out_phys = (UINT64)phys;
    return EFI_SUCCESS;
}

/* Copy all present UKI payload sections (.initrd / .recovery /
 * .modules) out of LoadedImage into AllocatePages-allocated
 * EfiLoaderData. Called once after detect_uki_sections() and BEFORE
 * ExitBootServices so the post-EBS kernel-physical addresses stay
 * valid. A failed copy aborts the boot via boot_fatal so a UKI that
 * ships a payload too large to allocate cannot silently lose the
 * payload (which would defeat the whole-chain Secure Boot
 * signature claim). */
static void uki_copy_payloads_to_loader_data(void)
{
    EFI_STATUS s;
    static const char hint[] =
        "AllocatePages(EfiLoaderData) failed for a UKI signed payload. "
        "Check firmware free memory or rebuild the UKI without the "
        "oversized payload section.";
    s = uki_copy_payload(".initrd", g_uki_initrd_ptr,
                         g_uki_initrd_size, &g_uki_initrd_phys);
    if (EFI_ERROR(s))
        boot_fatal(BOOT_ERR_UKI_PAYLOAD,
                   "UKI .initrd payload copy failed", hint);
    s = uki_copy_payload(".recovery", g_uki_recovery_ptr,
                         g_uki_recovery_size, &g_uki_recovery_phys);
    if (EFI_ERROR(s))
        boot_fatal(BOOT_ERR_UKI_PAYLOAD,
                   "UKI .recovery payload copy failed", hint);
    s = uki_copy_payload(".modules", g_uki_modules_ptr,
                         g_uki_modules_size, &g_uki_modules_phys);
    if (EFI_ERROR(s))
        boot_fatal(BOOT_ERR_UKI_PAYLOAD,
                   "UKI .modules payload copy failed", hint);
}

/* --- EFI System Partition integrity check ---------------------------------
 *
 * Pre-load sanity gate: catches obvious corruption / wrong-partition cases
 * BEFORE we trust the disk for kernel.exe, BOOTX64.EFI, or boot.conf.
 * This is NOT cryptographic verification -- the trust anchor for the
 * legacy split path is the Secure Boot signature on BOOTX64.EFI itself,
 * and the trust anchor for UKI is the whole-PE signature. This gate
 * matches Win11 BootMgr (FAT32 + correct partition GUID) and surfaces
 * the ESP UUID + size into HKLM\HARDWARE\BOOT\ESP via boot_info.
 *
 * Three checks, plus telemetry surfacing:
 *   (a) GPT partition type GUID == EFI_PARTITION_TYPE_SYSTEM_PARTITION_GUID
 *       (read from the PARENT disk's partition entry table; the partition
 *       handle's HardDrive DP node only carries the UNIQUE GUID).
 *   (b) FAT32 BPB sanity: BS_FilSysType at offset 0x52 == "FAT32   ", or
 *       BS_FilSysType at offset 0x36 == "FAT16   " for tiny test ESPs
 *       (< 16 MiB) with a WARN.
 *   (c) Required boot files present: BOOTX64.EFI plus one of the three
 *       kernel.exe fallback paths. boot.conf is diagnostic-only because
 *       parse_boot_conf treats missing as "use default config";
 *       making it required would change boot semantics.
 *
 * Three structural decisions:
 *   - UKI fast-skip: in UKI mode the disk identity is not load-bearing,
 *       so all integrity validation is skipped. esp_size_mb is still
 *       populated from the existing BlockIO probe so post-boot tools see
 *       a non-zero size; esp_type_guid_valid stays 0 to indicate "not
 *       checked".
 *   - GPT parser bounds: the corruption gate must treat all on-disk
 *       fields as hostile. We validate signature, header_size,
 *       NumberOfPartitionEntries, SizeOfPartitionEntry, table-byte
 *       overflow, PartitionEntryLBA range, and partition_number range
 *       BEFORE allocating, reading, or indexing.
 *   - boot.conf diagnostic-only: see (c) above.
 *
 * Failure mode: type-GUID mismatch / corrupt BPB / missing required file
 * all halt via boot_fatal() with a specific error code so the on-screen
 * BSOD identifies which gate failed. Missing parent disk handle (PXE,
 * RAM-disk, firmware quirks) is WARN-and-continue -- no GPT to check.
 */

/* GPT header layout per UEFI 2.10 Table 5-5. Read from LBA 1 of the
 * parent disk. Total header size is at least 92 bytes; bytes beyond
 * HeaderSize must read as zero per spec but we never look past HeaderSize. */
struct esp_gpt_header {
    UINT8   signature[8];           /* "EFI PART" -- 0x5452415020494645 */
    UINT32  revision;               /* 0x00010000 for v1.0 */
    UINT32  header_size;            /* 92 .. block_size; bytes covered by CRC */
    UINT32  header_crc32;           /* CRC32 of header_size bytes with this field zeroed */
    UINT32  reserved;
    UINT64  current_lba;            /* LBA of this header */
    UINT64  backup_lba;
    UINT64  first_usable_lba;
    UINT64  last_usable_lba;
    UINT8   disk_guid[16];
    UINT64  partition_entry_lba;    /* LBA where partition entry array starts */
    UINT32  num_partition_entries;
    UINT32  size_of_partition_entry; /* must be >= 128 and a power of 2 per UEFI spec */
    UINT32  partition_entry_array_crc32;
} __attribute__((packed));

/* GPT partition entry per UEFI 2.10 Table 5-6. First 16 bytes are
 * the partition type GUID -- that is all this gate needs. */
struct esp_gpt_partition_entry_head {
    UINT8   partition_type_guid[16];
    UINT8   unique_partition_guid[16];
    UINT64  starting_lba;
    UINT64  ending_lba;
    UINT64  attributes;
    /* + 72 bytes partition name (CHAR16) */
} __attribute__((packed));

/* C12A7328-F81F-11D2-BA4B-00A0C93EC93B in mixed-endian on-disk layout
 * (Data1/Data2/Data3 little-endian, Data4 big-endian) per UEFI 2.10
 * Appendix A.2 "EFI System Partition" partition type. */
static const UINT8 g_esp_type_guid[16] = {
    0x28, 0x73, 0x2A, 0xC1,                         /* Data1 LE: 0xC12A7328 */
    0x1F, 0xF8,                                     /* Data2 LE: 0xF81F */
    0xD2, 0x11,                                     /* Data3 LE: 0x11D2 */
    0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B  /* Data4 BE */
};

/* Locate the parent (whole-disk) BlockIO handle for the partition handle
 * `part_handle`. The standard EDK2 pattern: copy the partition's device
 * path, walk to the LAST node, replace it with END_ENTIRE, then call
 * gBS->LocateDevicePath(&BlockIo, &dp_remaining, &parent_handle).
 * On entry the partition's last node is HardDrive (UEFI 2.10 Table
 * 10-58); after truncation the path describes the parent media
 * (Sata/NVMe/USB/...) and LocateDevicePath finds its handle. Returns
 * EFI_SUCCESS + parent_handle on success, EFI_NOT_FOUND on PXE/RAM
 * boots that have no parent disk. */
static EFI_STATUS esp_find_parent_disk(EFI_HANDLE part_handle,
                                        EFI_HANDLE *out_parent)
{
    EFI_GUID dp_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
    EFI_GUID bio_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
    EFI_DEVICE_PATH_PROTOCOL *part_dp = (EFI_DEVICE_PATH_PROTOCOL *)0;
    EFI_STATUS status;

    *out_parent = (EFI_HANDLE)0;

    status = gBS->HandleProtocol(part_handle, &dp_guid, (VOID **)&part_dp);
    if (EFI_ERROR(status) || !part_dp)
        return EFI_NOT_FOUND;

    UINTN total_bytes = 0;
    {
        const EFI_DEVICE_PATH_PROTOCOL *node = part_dp;
        UINTN walked = 0;
        while (1) {
            UINT16 nlen;
            if (walked + 4 > 4096)
                return EFI_NOT_FOUND;
            nlen = (UINT16)node->Length[0] | ((UINT16)node->Length[1] << 8);
            if (nlen < 4)
                return EFI_NOT_FOUND;
            /* Require the full node body to fit within the cap BEFORE
             * advancing -- the header bound alone is insufficient for
             * malformed firmware paths. */
            if (walked + nlen > 4096)
                return EFI_NOT_FOUND;
            walked += nlen;
            if (node->Type == EFI_DP_TYPE_END &&
                node->SubType == EFI_DP_SUBTYPE_END_ENTIRE)
                break;
            node = (const EFI_DEVICE_PATH_PROTOCOL *)((const UINT8 *)node + nlen);
        }
        total_bytes = walked;
    }

    UINT8 dp_copy[1024];
    if (total_bytes > sizeof(dp_copy))
        return EFI_NOT_FOUND;
    efi_memcpy(dp_copy, part_dp, total_bytes);

    /* Walk the copy and find the last non-END node. Replace it with
     * END_ENTIRE in place, removing the trailing HD()/CDROM() media
     * node and leaving the parent (Pci(...)/Sata(...)/Nvme(...)) path. */
    {
        EFI_DEVICE_PATH_PROTOCOL *node = (EFI_DEVICE_PATH_PROTOCOL *)dp_copy;
        EFI_DEVICE_PATH_PROTOCOL *prev = (EFI_DEVICE_PATH_PROTOCOL *)0;
        UINTN walked = 0;
        while (walked < total_bytes) {
            UINT16 nlen = (UINT16)node->Length[0] |
                          ((UINT16)node->Length[1] << 8);
            if (node->Type == EFI_DP_TYPE_END &&
                node->SubType == EFI_DP_SUBTYPE_END_ENTIRE)
                break;
            prev = node;
            node = (EFI_DEVICE_PATH_PROTOCOL *)((UINT8 *)node + nlen);
            walked += nlen;
        }
        if (!prev)
            return EFI_NOT_FOUND;
        prev->Type = EFI_DP_TYPE_END;
        prev->SubType = EFI_DP_SUBTYPE_END_ENTIRE;
        prev->Length[0] = 4;
        prev->Length[1] = 0;
    }

    /* LocateDevicePath consumes a pointer-to-pointer and advances it
     * past matched nodes; pass our own pointer so its mutation does
     * not damage the original copy. EFI Boot Services entry 20 per
     * UEFI 2.10 -- typed VOID* in efi.h, cast at the call site so
     * we do not pollute the shared header. */
    typedef EFI_STATUS (EFIAPI *LOCATE_DEVICE_PATH_FN)(
        EFI_GUID *Protocol,
        EFI_DEVICE_PATH_PROTOCOL **DevicePath,
        EFI_HANDLE *Device);
    LOCATE_DEVICE_PATH_FN locate_dp =
        (LOCATE_DEVICE_PATH_FN)gBS->LocateDevicePath;
    EFI_DEVICE_PATH_PROTOCOL *probe = (EFI_DEVICE_PATH_PROTOCOL *)dp_copy;
    EFI_HANDLE parent = (EFI_HANDLE)0;
    status = locate_dp(&bio_guid, &probe, &parent);
    if (EFI_ERROR(status) || !parent)
        return EFI_NOT_FOUND;

    *out_parent = parent;
    return EFI_SUCCESS;
}

/* Extract HardDrive DP node fields from the partition's device path.
 * Returns EFI_SUCCESS + populated outputs when an HD() node is present
 * and is the last data node (always true for a partition handle). */
static EFI_STATUS esp_read_harddrive_node(EFI_HANDLE part_handle,
                                           UINT32 *out_part_number,
                                           UINT64 *out_part_start_lba,
                                           UINT64 *out_part_size_lba)
{
    EFI_GUID dp_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
    EFI_DEVICE_PATH_PROTOCOL *dp = (EFI_DEVICE_PATH_PROTOCOL *)0;
    EFI_STATUS status;

    *out_part_number = 0;
    *out_part_start_lba = 0;
    *out_part_size_lba = 0;

    status = gBS->HandleProtocol(part_handle, &dp_guid, (VOID **)&dp);
    if (EFI_ERROR(status) || !dp)
        return EFI_NOT_FOUND;

    const EFI_DEVICE_PATH_PROTOCOL *node = dp;
    UINTN walked = 0;
    while (walked + 4 <= 4096) {
        UINT16 nlen = (UINT16)node->Length[0] |
                      ((UINT16)node->Length[1] << 8);
        if (nlen < 4) return EFI_NOT_FOUND;
        /* Require the FULL node body to fit within the 4096 sanity cap
         * before reading any payload fields.  A `walked + 4 <= 4096`
         * bound only proves the header is in range; a malformed
         * firmware DP with HardDrive nlen >= 42 starting near the cap
         * would otherwise read past the bounded window. */
        if (walked + nlen > 4096) return EFI_NOT_FOUND;
        if (node->Type == EFI_DP_TYPE_END &&
            node->SubType == EFI_DP_SUBTYPE_END_ENTIRE)
            return EFI_NOT_FOUND;
        if (node->Type == EFI_DP_TYPE_MEDIA &&
            node->SubType == EFI_DP_MEDIA_HARDDRIVE && nlen >= 42) {
            const UINT8 *nd = (const UINT8 *)node;
            UINT32 pn = (UINT32)nd[4] | ((UINT32)nd[5] << 8) |
                        ((UINT32)nd[6] << 16) | ((UINT32)nd[7] << 24);
            UINT64 ps = 0, pz = 0;
            UINTN bi;
            for (bi = 0; bi < 8; bi++) {
                ps |= (UINT64)nd[8 + bi] << (bi * 8);
                pz |= (UINT64)nd[16 + bi] << (bi * 8);
            }
            *out_part_number = pn;
            *out_part_start_lba = ps;
            *out_part_size_lba = pz;
            return EFI_SUCCESS;
        }
        node = (const EFI_DEVICE_PATH_PROTOCOL *)((const UINT8 *)node + nlen);
        walked += nlen;
    }
    return EFI_NOT_FOUND;
}

/* IoAlign-compliant block read (UEFI 2.10 spec 13.9: the ReadBlocks data buffer
 * must be aligned to Media->IoAlign). Most firmware reports IoAlign <= 1, where
 * any buffer is legal -- the fast path reads directly into `out`, byte-identical
 * to a bare ReadBlocks. When IoAlign > 1, read into an aligned bounce buffer and
 * copy to `out` so a caller's naturally-aligned stack/pool buffer still complies
 * on large-sector NVMe / RAID HBAs. `byte_count` must be a whole number of
 * blocks (ReadBlocks requirement). The IoAlign sanity bound mirrors the
 * ab_bl_increment_tries A/B-metadata write path: cap at 4096, reject
 * non-power-of-two, and guard the byte_count + align allocation against overflow
 * -- buggy firmware advertising an implausible IoAlign is rejected
 * (EFI_UNSUPPORTED), never used to size an overflowing AllocatePool. Returns the
 * ReadBlocks status, or EFI_OUT_OF_RESOURCES / EFI_UNSUPPORTED before the read. */
static EFI_STATUS bl_read_blocks_aligned(EFI_BLOCK_IO_PROTOCOL *bio,
                                         UINT32 media_id, UINT64 lba,
                                         UINTN byte_count, VOID *out)
{
    UINT32 align = bio->Media->IoAlign;

    if (align <= 1u)
        return bio->ReadBlocks(bio, media_id, lba, byte_count, out);

    /* Reject implausible / non-power-of-two alignment before sizing the alloc. */
    if (align > 4096u || (align & (align - 1u)) != 0u)
        return EFI_UNSUPPORTED;
    if (byte_count > (UINTN)(~(UINTN)0) - (UINTN)align)
        return EFI_UNSUPPORTED;   /* byte_count + align would overflow */

    UINT8 *raw = (UINT8 *)0;   /* AllocatePool pointer (for FreePool) */
    EFI_STATUS status = gBS->AllocatePool(EfiLoaderData, byte_count + align,
                                          (VOID **)&raw);
    if (EFI_ERROR(status) || !raw)
        return EFI_OUT_OF_RESOURCES;

    UINT8 *aligned = (UINT8 *)(((UINTN)raw + (align - 1u)) & ~((UINTN)align - 1u));
    status = bio->ReadBlocks(bio, media_id, lba, byte_count, aligned);
    if (!EFI_ERROR(status))
        efi_memcpy(out, aligned, byte_count);
    gBS->FreePool(raw);
    return status;
}

/* Validate the GPT type GUID for our partition by reading the parent
 * disk's GPT header + partition entry table. Sets
 * g_boot_info_ptr->esp_type_guid_valid on success. Returns EFI_SUCCESS
 * on validation pass, an error status on missing-parent / read-failure
 * (warn-skip), or calls boot_fatal() and never returns on actual
 * type-GUID mismatch / corrupt header.
 *
 * Hostile-field treatment: every on-disk field is validated --
 * signature, header_size, num_entries, entry_size, table byte count,
 * entry LBA range, and partition_number range -- BEFORE allocation,
 * read, or indexing. */
static EFI_STATUS esp_check_gpt_type_guid(EFI_HANDLE part_handle)
{
    EFI_GUID bio_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
    EFI_HANDLE parent_handle = (EFI_HANDLE)0;
    EFI_BLOCK_IO_PROTOCOL *parent_bio = (EFI_BLOCK_IO_PROTOCOL *)0;
    UINT32 part_number = 0;
    UINT64 part_start_lba = 0;
    UINT64 part_size_lba = 0;
    EFI_STATUS status;

    status = esp_read_harddrive_node(part_handle, &part_number,
                                      &part_start_lba, &part_size_lba);
    if (EFI_ERROR(status) || part_number == 0) {
        serial_early_print("[BOOT] ESP integrity: no HardDrive DP node "
                           "(non-GPT boot path) -- skipping GPT check\n");
        return EFI_NOT_FOUND;
    }

    /* The caller already gated on boot_partition_style == 2 before
     * calling this function, so every read failure below is on a
     * GPT-CLASSIFIED boot device.  Treat those as fail-closed
     * (boot_fatal) rather than warn-skip -- a hostile or degraded
     * parent-disk read path must not be allowed to disable the
     * type-GUID gate.  The only legitimate warn-skip remains in
     * esp_read_harddrive_node when no HD() node exists (true non-GPT
     * path) and is reachable BEFORE this function. */
    status = esp_find_parent_disk(part_handle, &parent_handle);
    if (EFI_ERROR(status)) {
        boot_fatal(BOOT_ERR_ESP_TYPE_GUID,
                   "ESP integrity: parent disk handle not found",
                   "Boot device is GPT-classified but the parent "
                   "(whole-disk) handle is unreachable -- cannot "
                   "verify partition type GUID.");
    }

    status = gBS->HandleProtocol(parent_handle, &bio_guid,
                                  (VOID **)&parent_bio);
    if (EFI_ERROR(status) || !parent_bio || !parent_bio->Media) {
        boot_fatal(BOOT_ERR_ESP_TYPE_GUID,
                   "ESP integrity: parent disk BlockIO missing",
                   "Cannot read GPT header on a GPT-classified boot "
                   "device -- ESP identity is unverifiable.");
    }

    EFI_BLOCK_IO_MEDIA *pm = parent_bio->Media;
    /* Require BlockSize covers the full GPT header footprint (92
     * bytes per UEFI 2.10) before reading. */
    if (!pm->MediaPresent || pm->BlockSize < sizeof(struct esp_gpt_header) ||
        pm->BlockSize > 4096 || pm->LastBlock < 2) {
        boot_fatal(BOOT_ERR_ESP_TYPE_GUID,
                   "ESP integrity: parent media geometry unusable",
                   "BlockSize must be 92..4096 with media present and "
                   "LastBlock >= 2 to host a GPT header.");
    }

    UINT8 hdr_buf[4096];
    if (pm->BlockSize > sizeof(hdr_buf)) {
        boot_fatal(BOOT_ERR_ESP_TYPE_GUID,
                   "ESP integrity: parent BlockSize exceeds 4096",
                   "Cannot allocate stack buffer to hold one block; "
                   "GPT header read aborted.");
    }
    status = bl_read_blocks_aligned(parent_bio, pm->MediaId, 1,
                                    pm->BlockSize, hdr_buf);
    if (EFI_ERROR(status)) {
        boot_fatal(BOOT_ERR_ESP_TYPE_GUID,
                   "ESP integrity: GPT header read failed",
                   "Cannot read LBA 1 of the parent disk to verify "
                   "the GPT header.");
    }

    struct esp_gpt_header hdr;
    efi_memcpy(&hdr, hdr_buf, sizeof(hdr));

    static const UINT8 expected_sig[8] = {
        'E','F','I',' ','P','A','R','T'
    };
    UINTN i;
    for (i = 0; i < 8; i++) {
        if (hdr.signature[i] != expected_sig[i]) {
            boot_fatal(BOOT_ERR_ESP_TYPE_GUID,
                       "GPT signature mismatch on parent disk",
                       "Expected 'EFI PART' at LBA 1; ESP cannot be "
                       "verified against a corrupt or non-GPT disk.");
        }
    }
    if (hdr.header_size < 92 || hdr.header_size > pm->BlockSize) {
        boot_fatal(BOOT_ERR_ESP_TYPE_GUID,
                   "GPT header_size out of bounds",
                   "GPT header_size must be 92..BlockSize per UEFI spec.");
    }
    if (hdr.size_of_partition_entry < 128 ||
        hdr.size_of_partition_entry > 4096 ||
        (hdr.size_of_partition_entry & (hdr.size_of_partition_entry - 1)) != 0) {
        boot_fatal(BOOT_ERR_ESP_TYPE_GUID,
                   "GPT size_of_partition_entry invalid",
                   "Must be >= 128 and a power of 2 per UEFI spec.");
    }
    if (hdr.num_partition_entries == 0 ||
        hdr.num_partition_entries > 1024) {
        boot_fatal(BOOT_ERR_ESP_TYPE_GUID,
                   "GPT num_partition_entries out of bounds",
                   "Plausible range is 1..1024 (typical is 128).");
    }
    if (part_number > hdr.num_partition_entries) {
        boot_fatal(BOOT_ERR_ESP_TYPE_GUID,
                   "GPT partition_number exceeds num_partition_entries",
                   "Boot partition's HD() node references an entry "
                   "past the end of the partition table.");
    }
    UINT64 table_bytes = (UINT64)hdr.num_partition_entries *
                         (UINT64)hdr.size_of_partition_entry;
    if (table_bytes > 1024ULL * 1024ULL) {
        boot_fatal(BOOT_ERR_ESP_TYPE_GUID,
                   "GPT partition table byte count exceeds 1 MiB cap",
                   "Sanity cap rejects malformed disks claiming an "
                   "absurdly large entry table.");
    }
    if (hdr.partition_entry_lba < 2 ||
        hdr.partition_entry_lba > pm->LastBlock) {
        boot_fatal(BOOT_ERR_ESP_TYPE_GUID,
                   "GPT partition_entry_lba out of disk range",
                   "Entry table LBA must be within the parent disk.");
    }
    UINT64 entry_blocks = (table_bytes + (UINT64)pm->BlockSize - 1) /
                          (UINT64)pm->BlockSize;
    if (entry_blocks == 0 ||
        hdr.partition_entry_lba + entry_blocks > pm->LastBlock + 1) {
        boot_fatal(BOOT_ERR_ESP_TYPE_GUID,
                   "GPT partition entry array exceeds disk size",
                   "Computed table footprint walks off the end of the "
                   "parent disk -- header is corrupt or hostile.");
    }

    /* Read the single block containing our partition's entry rather
     * than the whole table -- minimizes alloc and read pressure on
     * slow firmware. */
    UINT64 byte_offset = (UINT64)(part_number - 1) *
                          (UINT64)hdr.size_of_partition_entry;
    UINT64 block_index = byte_offset / (UINT64)pm->BlockSize;
    UINT32 byte_in_block = (UINT32)(byte_offset % (UINT64)pm->BlockSize);
    if (byte_in_block + 16 > (UINT32)pm->BlockSize) {
        boot_fatal(BOOT_ERR_ESP_TYPE_GUID,
                   "GPT partition entry straddles block boundary",
                   "Type GUID would span two LBAs -- header is corrupt.");
    }

    UINT8 entry_blk[4096];
    status = bl_read_blocks_aligned(parent_bio, pm->MediaId,
                                    hdr.partition_entry_lba + block_index,
                                    pm->BlockSize, entry_blk);
    if (EFI_ERROR(status)) {
        boot_fatal(BOOT_ERR_ESP_TYPE_GUID,
                   "ESP integrity: GPT partition entry read failed",
                   "GPT header parsed cleanly but the entry table LBA "
                   "could not be read -- disk is corrupt or hostile.");
    }

    const UINT8 *type_guid = &entry_blk[byte_in_block];
    for (i = 0; i < 16; i++) {
        if (type_guid[i] != g_esp_type_guid[i]) {
            char detail[96];
            const char hex[] = "0123456789ABCDEF";
            UINTN p = 0;
            const char *prefix = "Observed type GUID first byte: 0x";
            UINTN pi;
            for (pi = 0; prefix[pi] && p < sizeof(detail) - 4; pi++)
                detail[p++] = prefix[pi];
            detail[p++] = hex[(type_guid[0] >> 4) & 0xF];
            detail[p++] = hex[type_guid[0] & 0xF];
            detail[p] = '\0';
            boot_fatal(BOOT_ERR_ESP_TYPE_GUID,
                       "ESP type GUID mismatch",
                       detail);
        }
    }

    g_boot_info_ptr->esp_type_guid_valid = 1;
    serial_early_print("[BOOT] ESP integrity: GPT type GUID matches "
                       "EFI System Partition\n");
    return EFI_SUCCESS;
}

/* Read LBA 0 of the partition (already-open BlockIO `bio`) and verify
 * the FAT BPB filesystem-type field. Sets esp_filesystem_type. FAT32
 * is mandatory; FAT16 is tolerated only on tiny test ESPs (< 16 MiB)
 * with a WARN. Calls boot_fatal on a clearly invalid BPB. */
static void esp_check_fat_bpb(EFI_BLOCK_IO_PROTOCOL *bio,
                               UINT32 partition_size_mib)
{
    EFI_BLOCK_IO_MEDIA *m = bio->Media;
    UINT8 lba0[4096];
    EFI_STATUS status;

    /* BPB check is a fail-closed gate.  Geometry out of range or
     * ReadBlocks failure on LBA 0 of the partition we are about to
     * load kernel.exe from is a hard error -- not a skip.  Leaving
     * esp_filesystem_type=0 while continuing to required-files +
     * load_kernel would let a degraded or hostile BlockIO path
     * bypass the FAT32 policy entirely. */
    if (m->BlockSize < 512 || m->BlockSize > sizeof(lba0)) {
        boot_fatal(BOOT_ERR_ESP_BPB,
                   "ESP integrity: BlockSize geometry unusable for BPB",
                   "Partition BlockSize must be 512..4096 to host a "
                   "FAT BPB at LBA 0; ESP cannot be verified.");
    }
    status = bl_read_blocks_aligned(bio, m->MediaId, 0, m->BlockSize, lba0);
    if (EFI_ERROR(status)) {
        boot_fatal(BOOT_ERR_ESP_BPB,
                   "ESP integrity: BPB read failed at LBA 0",
                   "Cannot read the partition's boot sector to verify "
                   "FAT32 / FAT16 filesystem signature.");
    }

    /* Boot signature 0x55AA at offset 510 is mandatory for any FAT
     * volume per Microsoft FAT specification; absence indicates the
     * volume was never formatted. */
    if (lba0[510] != 0x55 || lba0[511] != 0xAA) {
        boot_fatal(BOOT_ERR_ESP_BPB,
                   "ESP BPB boot signature missing",
                   "Bytes 510/511 of LBA 0 must be 0x55 0xAA per "
                   "Microsoft FAT spec; ESP appears unformatted.");
    }

    static const UINT8 fat32_marker[8] = {'F','A','T','3','2',' ',' ',' '};
    static const UINT8 fat16_marker[8] = {'F','A','T','1','6',' ',' ',' '};
    int is_fat32 = 1, is_fat16_legacy = 1;
    UINTN i;
    for (i = 0; i < 8; i++) {
        if (lba0[0x52 + i] != fat32_marker[i]) is_fat32 = 0;
        if (lba0[0x36 + i] != fat16_marker[i]) is_fat16_legacy = 0;
    }
    if (is_fat32) {
        g_boot_info_ptr->esp_filesystem_type = 2;
        serial_early_print("[BOOT] ESP integrity: BS_FilSysType FAT32 OK\n");
        return;
    }
    if (is_fat16_legacy) {
        if (partition_size_mib > 0 && partition_size_mib < 16) {
            g_boot_info_ptr->esp_filesystem_type = 1;
            serial_early_print("[WARN] ESP integrity: FAT16 ESP tolerated "
                               "(partition < 16 MiB; likely test image)\n");
            return;
        }
        boot_fatal(BOOT_ERR_ESP_BPB,
                   "ESP filesystem is FAT16 on a non-tiny ESP",
                   "FAT16 only allowed on test ESPs under 16 MiB; "
                   "production ESPs must be FAT32 per UEFI spec.");
    }
    boot_fatal(BOOT_ERR_ESP_BPB,
               "ESP filesystem type unrecognized",
               "BS_FilSysType at offset 0x52 is not 'FAT32   '; "
               "ESP must be FAT32 per UEFI spec or tiny FAT16.");
}

/* Probe the boot device's filesystem for required boot files and
 * report ALL missing files in a single batched boot_fatal call
 * instead of cascading through later load failures. boot.conf is
 * NOT required (parse_boot_conf treats missing as default-config) --
 * we probe it for diagnostic-only logging per Codex design F3. */
static void esp_check_required_files(EFI_HANDLE part_handle)
{
    EFI_GUID fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = (EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *)0;
    EFI_FILE_PROTOCOL *root = (EFI_FILE_PROTOCOL *)0;
    EFI_STATUS status;

    status = gBS->HandleProtocol(part_handle, &fs_guid, (VOID **)&fs);
    if (EFI_ERROR(status) || !fs) {
        serial_early_print("[WARN] ESP integrity: boot device has no "
                           "SimpleFS -- required-files batch skipped\n");
        return;
    }
    status = fs->OpenVolume(fs, &root);
    if (EFI_ERROR(status) || !root) {
        serial_early_print("[WARN] ESP integrity: OpenVolume failed -- "
                           "required-files batch skipped\n");
        return;
    }

    int missing_bootx64 = 0;
    int missing_kernel = 0;
    int missing_bootconf = 0;

    EFI_FILE_PROTOCOL *fh = (EFI_FILE_PROTOCOL *)0;
    status = root->Open(root, &fh, u"\\EFI\\BOOT\\BOOTX64.EFI",
                         EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status) || !fh) {
        missing_bootx64 = 1;
    } else {
        fh->Close(fh);
    }

    /* kernel.exe: matches the existing fallback list in load_kernel
     * (\boot\kernel.exe, \kernel.exe, \EFI\ImpossibleOS\kernel.exe).
     * Any one being present satisfies the batch. */
    fh = (EFI_FILE_PROTOCOL *)0;
    status = root->Open(root, &fh, u"\\boot\\kernel.exe",
                         EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status) || !fh) {
        fh = (EFI_FILE_PROTOCOL *)0;
        status = root->Open(root, &fh, u"\\kernel.exe",
                             EFI_FILE_MODE_READ, 0);
        if (EFI_ERROR(status) || !fh) {
            fh = (EFI_FILE_PROTOCOL *)0;
            status = root->Open(root, &fh,
                                 u"\\EFI\\ImpossibleOS\\kernel.exe",
                                 EFI_FILE_MODE_READ, 0);
            if (EFI_ERROR(status) || !fh) {
                missing_kernel = 1;
            } else {
                fh->Close(fh);
            }
        } else {
            fh->Close(fh);
        }
    } else {
        fh->Close(fh);
    }

    fh = (EFI_FILE_PROTOCOL *)0;
    status = root->Open(root, &fh, u"\\EFI\\ImpossibleOS\\boot.conf",
                         EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status) || !fh) {
        fh = (EFI_FILE_PROTOCOL *)0;
        status = root->Open(root, &fh, u"\\boot.conf",
                             EFI_FILE_MODE_READ, 0);
        if (EFI_ERROR(status) || !fh) {
            missing_bootconf = 1;
        } else {
            fh->Close(fh);
        }
    } else {
        fh->Close(fh);
    }
    if (missing_bootconf) {
        serial_early_print("[BOOT] ESP integrity: boot.conf not present "
                           "(default-config boot path will be used)\n");
    }

    root->Close(root);

    /* Only BOOTX64.EFI is fatal here.  kernel.exe missing on the boot
     * device is NOT fatal because load_kernel() has a fallback
     * (bootx64.c:3443-3500) that searches every other SimpleFS volume;
     * making this gate fatal would block that recovery path.  UEFI
     * launched us from BOOTX64.EFI, so it MUST be on this volume -- a
     * missing copy means the volume itself is corrupt regardless of
     * what load_kernel finds elsewhere. */
    if (missing_bootx64) {
        boot_fatal(BOOT_ERR_ESP_MISSING_FILES,
                   "BOOTX64.EFI missing from boot volume",
                   "UEFI launched us from this volume, but the file is "
                   "no longer present -- ESP appears corrupt.");
    }
    if (missing_kernel) {
        serial_early_print("[BOOT] ESP integrity: kernel.exe absent on "
                           "boot device -- load_kernel() will search "
                           "non-boot volumes\n");
    } else {
        serial_early_print("[BOOT] ESP integrity: required files present "
                           "(BOOTX64.EFI + kernel.exe)\n");
    }
}

/* ---- Media role detection (boot-media role-detection feature) -------------
 *
 * Reads /IPOS/role.txt from the ESP and from the BlackBox service partition
 * on the SAME physical disk as the ESP. Cross-checks: mismatch falls back to
 * BOOT_MEDIA_ROLE_NORMAL with mismatch=1. BlackBox absent is not a mismatch.
 * Updates boot_info->boot_media_role + (for installer/recovery/diagnostics)
 * the boot_path/boot_reason decision record so consumers see a coherent
 * answer.
 *
 * Same-disk BlackBox identity: a label-only "BLACKBOX" lookup is unsafe --
 * on a host with both an internal Impossible OS disk and an installer USB,
 * label-only lookup might hit the internal disk's stale role marker. Same-
 * disk binding pins BlackBox to the partition that shares a parent BlockIO
 * handle with the ESP. */

#define MEDIA_ROLE_FILE_PATH        L"\\IPOS\\role.txt"
#define MEDIA_ROLE_MAX_BYTES        64u   /* role names <= 14 chars + slack */

static UINT32 media_role_parse(const char *buf, UINTN len)
{
    /* Trim leading/trailing ASCII whitespace + \r\n. The on-disk file is
     * operator-friendly (text editor output often has trailing newline). */
    UINTN start = 0, end = len;
    while (start < end &&
           (buf[start] == ' '  || buf[start] == '\t' ||
            buf[start] == '\r' || buf[start] == '\n')) {
        start++;
    }
    while (end > start &&
           (buf[end - 1] == ' '  || buf[end - 1] == '\t' ||
            buf[end - 1] == '\r' || buf[end - 1] == '\n')) {
        end--;
    }
    UINTN n = end - start;
    if (n == 0 || n > 14)
        return BOOT_MEDIA_ROLE_NORMAL;

    /* Lower-case + literal compare. ASCII only by spec; non-ASCII bytes
     * fall through to NORMAL. */
    char lc[16];
    UINTN i;
    for (i = 0; i < n; i++) {
        char c = buf[start + i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        lc[i] = c;
    }
    lc[n] = '\0';

    if (n == 6  && lc[0]=='n' && lc[1]=='o' && lc[2]=='r' && lc[3]=='m' &&
                   lc[4]=='a' && lc[5]=='l')
        return BOOT_MEDIA_ROLE_NORMAL;
    if (n == 9  && lc[0]=='i' && lc[1]=='n' && lc[2]=='s' && lc[3]=='t' &&
                   lc[4]=='a' && lc[5]=='l' && lc[6]=='l' && lc[7]=='e' &&
                   lc[8]=='r')
        return BOOT_MEDIA_ROLE_INSTALLER;
    if (n == 4  && lc[0]=='l' && lc[1]=='i' && lc[2]=='v' && lc[3]=='e')
        return BOOT_MEDIA_ROLE_LIVE;
    if (n == 8  && lc[0]=='r' && lc[1]=='e' && lc[2]=='c' && lc[3]=='o' &&
                   lc[4]=='v' && lc[5]=='e' && lc[6]=='r' && lc[7]=='y')
        return BOOT_MEDIA_ROLE_RECOVERY;
    if (n == 13 && lc[0]=='m' && lc[1]=='a' && lc[2]=='n' && lc[3]=='u' &&
                   lc[4]=='f' && lc[5]=='a' && lc[6]=='c' && lc[7]=='t' &&
                   lc[8]=='u' && lc[9]=='r' && lc[10]=='i'&& lc[11]=='n'&&
                   lc[12]=='g')
        return BOOT_MEDIA_ROLE_MANUFACTURING;
    if (n == 11 && lc[0]=='d' && lc[1]=='i' && lc[2]=='a' && lc[3]=='g' &&
                   lc[4]=='n' && lc[5]=='o' && lc[6]=='s' && lc[7]=='t' &&
                   lc[8]=='i' && lc[9]=='c' && lc[10]=='s')
        return BOOT_MEDIA_ROLE_DIAGNOSTICS;
    return BOOT_MEDIA_ROLE_NORMAL;
}

static const char *media_role_name(UINT32 role)
{
    switch (role) {
        case BOOT_MEDIA_ROLE_NORMAL:        return "normal";
        case BOOT_MEDIA_ROLE_INSTALLER:     return "installer";
        case BOOT_MEDIA_ROLE_LIVE:          return "live";
        case BOOT_MEDIA_ROLE_RECOVERY:      return "recovery";
        case BOOT_MEDIA_ROLE_MANUFACTURING: return "manufacturing";
        case BOOT_MEDIA_ROLE_DIAGNOSTICS:   return "diagnostics";
        default:                            return "unset";
    }
}

/* Read \IPOS\role.txt from a SimpleFileSystem volume. Returns the parsed
 * role enum value, or BOOT_MEDIA_ROLE_UNSET if the file is absent /
 * unreadable / over-sized. Does NOT fall back to NORMAL on absent --
 * caller distinguishes absent (UNSET) from unrecognized (NORMAL). */
static UINT32 media_role_read_from_fs(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs)
{
    if (!fs) return BOOT_MEDIA_ROLE_UNSET;

    EFI_FILE_PROTOCOL *root = (EFI_FILE_PROTOCOL *)0;
    EFI_STATUS status = fs->OpenVolume(fs, &root);
    if (EFI_ERROR(status) || !root) return BOOT_MEDIA_ROLE_UNSET;

    EFI_FILE_PROTOCOL *file = (EFI_FILE_PROTOCOL *)0;
    status = root->Open(root, &file, (CHAR16 *)MEDIA_ROLE_FILE_PATH,
                        EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status) || !file) {
        root->Close(root);
        return BOOT_MEDIA_ROLE_UNSET;
    }

    /* Hard cap at MEDIA_ROLE_MAX_BYTES per the parse-buffers gate (gate
     * 14): role.txt must be tiny; refuse oversize inputs rather than
     * truncate. */
    char buf[MEDIA_ROLE_MAX_BYTES + 1];
    UINTN read_size = MEDIA_ROLE_MAX_BYTES;
    status = file->Read(file, &read_size, (VOID *)buf);
    file->Close(file);
    root->Close(root);
    if (EFI_ERROR(status))
        return BOOT_MEDIA_ROLE_UNSET;
    /* If the read filled the entire buffer, the file is over-sized
     * (pathological / wrong). Don't accept it. */
    if (read_size >= MEDIA_ROLE_MAX_BYTES)
        return BOOT_MEDIA_ROLE_UNSET;
    return media_role_parse(buf, read_size);
}

/* Find a SimpleFileSystem volume that lives on the same parent BlockIO as
 * `boot_part_handle` AND has volume label "BLACKBOX". On failure returns
 * NULL (BlackBox absent or not on same disk -- treated as "no marker",
 * not mismatch). */
static EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *
media_role_locate_blackbox_fs(EFI_HANDLE boot_part_handle)
{
    if (!boot_part_handle) return (EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *)0;

    EFI_HANDLE boot_parent = (EFI_HANDLE)0;
    if (EFI_ERROR(esp_find_parent_disk(boot_part_handle, &boot_parent))
        || !boot_parent)
        return (EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *)0;

    EFI_GUID fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_HANDLE *handles = (EFI_HANDLE *)0;
    UINTN nh = 0;
    EFI_STATUS status = gBS->LocateHandleBuffer(ByProtocol, &fs_guid, (VOID *)0,
                                                &nh, &handles);
    if (EFI_ERROR(status) || !handles || nh == 0)
        return (EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *)0;

    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *match = (EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *)0;
    UINTN hi;
    for (hi = 0; hi < nh; hi++) {
        EFI_HANDLE h = handles[hi];
        if (h == boot_part_handle) continue;  /* ESP itself */

        /* Pin candidate to same parent disk as ESP. */
        EFI_HANDLE parent = (EFI_HANDLE)0;
        if (EFI_ERROR(esp_find_parent_disk(h, &parent)) || parent != boot_parent)
            continue;

        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = (EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *)0;
        status = gBS->HandleProtocol(h, &fs_guid, (VOID **)&fs);
        if (EFI_ERROR(status) || !fs) continue;

        EFI_FILE_PROTOCOL *root = (EFI_FILE_PROTOCOL *)0;
        if (EFI_ERROR(fs->OpenVolume(fs, &root)) || !root) continue;

        /* EFI_FILE_SYSTEM_INFO GUID per UEFI 2.10 Table 13.4 -- defined
         * inline here because the shared efi.h does not surface it. */
        EFI_GUID fsi_guid = { 0x09576e93, 0x6d3f, 0x11d2,
                              { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } };
        UINTN info_size = 0;
        status = root->GetInfo(root, &fsi_guid, &info_size, (VOID *)0);
        if (status != EFI_BUFFER_TOO_SMALL || info_size == 0 ||
            info_size > 4096) {
            root->Close(root);
            continue;
        }
        VOID *info = (VOID *)0;
        if (EFI_ERROR(gBS->AllocatePool(EfiLoaderData, info_size, &info)) || !info) {
            root->Close(root);
            continue;
        }
        status = root->GetInfo(root, &fsi_guid, &info_size, info);
        root->Close(root);
        if (EFI_ERROR(status)) {
            gBS->FreePool(info);
            continue;
        }
        /* EFI_FILE_SYSTEM_INFO.VolumeLabel is at offset 36 (8+8+8+8+1+3 pad)
         * per UEFI 2.10 Table 13.4. The CHAR16 label is variable-length and
         * NUL-terminated. Bounds-check info_size before reading 9 CHAR16
         * slots ("BLACKBOX" + terminator) so a same-disk volume with a
         * short label cannot drag the label compare past the AllocatePool
         * buffer. 9 * sizeof(CHAR16) = 18 bytes minimum from offset 36. */
        UINTN label_off = 36;
        UINTN min_label_bytes = 9u * sizeof(CHAR16);
        if (info_size < label_off + min_label_bytes) {
            gBS->FreePool(info);
            continue;
        }
        const CHAR16 *label = (const CHAR16 *)((UINT8 *)info + label_off);
        const CHAR16 want[] = L"BLACKBOX";
        UINTN li = 0;
        BOOLEAN ok = 1;
        for (li = 0; li < 8; li++) {
            CHAR16 ch = label[li];
            if (ch >= L'a' && ch <= L'z') ch = (CHAR16)(ch - L'a' + L'A');
            if (ch != want[li]) { ok = 0; break; }
        }
        if (ok && (label[8] == 0 || label[8] == L' '))
            match = fs;
        gBS->FreePool(info);
        if (match) break;
    }
    gBS->FreePool(handles);
    return match;
}

/* Top-level: read role.txt from ESP + BlackBox (same disk), cross-check,
 * populate boot_info media-role fields, and (for installer/recovery/
 * diagnostics) update boot_path / boot_reason. Emits one [BOOT] Media
 * role: ... line on serial; mismatch emits an extra [WARN] line. */
static void media_role_detect_and_record(EFI_HANDLE boot_part_handle)
{
    UINT32 esp_role = BOOT_MEDIA_ROLE_UNSET;
    UINT32 bb_role  = BOOT_MEDIA_ROLE_UNSET;

    /* ESP read: HandleProtocol on the boot partition directly -- NO
     * LocateProtocol fallback. An ambient global LocateProtocol scan can
     * return any SimpleFS volume in the system. For media-role detection
     * that fallback is a trust-boundary leak: the loader could read
     * /IPOS/role.txt from an unrelated disk and treat it as authoritative.
     * Trust only the explicit boot-device handle (load_kernel() and
     * locate_boot_fs() are HandleProtocol-only for the same reason).
     * Failure -> esp_role stays UNSET, caller emits the default-normal
     * line and (if BlackBox is also UNSET) takes the same path as a
     * marker-absent boot. */
    if (boot_part_handle) {
        EFI_GUID fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *esp_fs = (EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *)0;
        EFI_STATUS s = gBS->HandleProtocol(boot_part_handle, &fs_guid,
                                           (VOID **)&esp_fs);
        if (!EFI_ERROR(s) && esp_fs)
            esp_role = media_role_read_from_fs(esp_fs);
    }

    /* BlackBox read: same-disk binding via parent-BlockIO match. */
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *bb_fs =
        media_role_locate_blackbox_fs(boot_part_handle);
    if (bb_fs)
        bb_role = media_role_read_from_fs(bb_fs);

    /* Resolve final role with the cross-check rules: ESP authoritative,
     * BlackBox cross-checks, mismatch -> NORMAL + warn, both UNSET ->
     * NORMAL (default). */
    UINT32 final_role = BOOT_MEDIA_ROLE_NORMAL;
    UINT8  mismatch   = 0;
    if (esp_role == BOOT_MEDIA_ROLE_UNSET && bb_role == BOOT_MEDIA_ROLE_UNSET) {
        serial_early_print("[BOOT] Media role: normal (default)\n");
    } else if (esp_role != BOOT_MEDIA_ROLE_UNSET &&
               bb_role  != BOOT_MEDIA_ROLE_UNSET &&
               esp_role != bb_role) {
        mismatch = 1;
        serial_early_print("[WARN] Media role mismatch (esp=");
        serial_early_print(media_role_name(esp_role));
        serial_early_print(" blackbox=");
        serial_early_print(media_role_name(bb_role));
        serial_early_print("); falling back to normal\n");
        serial_early_print("[BOOT] Media role: normal (default)\n");
    } else {
        final_role = (esp_role != BOOT_MEDIA_ROLE_UNSET) ? esp_role : bb_role;
        serial_early_print("[BOOT] Media role: ");
        serial_early_print(media_role_name(final_role));
        serial_early_print("\n");
    }

    /* UKI .cmdline override (3-source media-role precedence: UKI
     * cmdline > DHCP option > ESP /IPOS/role.txt > default normal).
     * When the UKI fast path supplied a signed .cmdline carrying
     * `media_role=NAME`, it wins over the disk-side answer because
     * the UKI cmdline is covered by the firmware-Secure-Boot
     * signature (whole-chain) while role.txt is staged at image-
     * build time and not part of the signature. We override CLEANLY
     * (no mismatch flag) -- a UKI legitimately staged on top of an
     * existing on-disk media-role marker (e.g., recovery UKI on a
     * normal install) is the EXPECTED case, not a producer error.
     * UNSET from the parser means "no media_role= token in cmdline";
     * fall through to the disk answer in that case. */
    if (g_uki_kernel_ptr && g_uki_cmdline_ptr && g_uki_cmdline_size > 0) {
        /* Cap the cmdline scan at the same 1 MiB sanity limit that
         * parse_boot_conf() applies in BOOT_CONF_SANITY_CAP. parse_
         * boot_conf runs LATER in efi_main, so without this cap a
         * malformed-but-signed UKI with a multi-GiB .cmdline section
         * would force the parser to scan unbounded bytes before the
         * boot.conf path's hard-fail can fire. Realistic UKI cmdlines
         * are <1 KiB; 1 MiB matches the existing producer sanity
         * contract. */
        #ifndef BOOT_CONF_SANITY_CAP
        #define BOOT_CONF_SANITY_CAP (1024u * 1024u)
        #endif
        UINTN scan_size = g_uki_cmdline_size;
        if (scan_size > BOOT_CONF_SANITY_CAP)
            scan_size = BOOT_CONF_SANITY_CAP;
        UINT32 uki_role = (UINT32)uki_cmdline_extract_media_role(
            (const unsigned char *)g_uki_cmdline_ptr,
            (__SIZE_TYPE__)scan_size);
        if (uki_role != BOOT_MEDIA_ROLE_UNSET) {
            if (uki_role != final_role) {
                serial_early_print("[BOOT] Media role: UKI cmdline override "
                                   "(disk=");
                serial_early_print(media_role_name(final_role));
                serial_early_print(" -> uki=");
                serial_early_print(media_role_name(uki_role));
                serial_early_print(")\n");
            } else {
                serial_early_print("[BOOT] Media role: UKI cmdline confirms "
                                   "disk value\n");
            }
            final_role = uki_role;
            mismatch = 0;  /* UKI override resets cross-check flag. */
        }
    }

    g_boot_info_ptr->boot_media_role = final_role;
    g_boot_info_ptr->boot_media_role_mismatch = mismatch;
    /* boot_path / boot_reason coupling lives in the populate block
     * later in efi_main: that block reads g_boot_info_ptr->
     * boot_media_role to decide whether to override its NORMAL +
     * USER_SELECTED/NORMAL defaults with INSTALLER / RECOVERY /
     * DIAGNOSTIC + MEDIA_ROLE_MARKER. Doing the override here would
     * be silently undone when the populate block writes its defaults. */
}

/* Top-level ESP integrity check entry. Runs after the BlockIO probe
 * in efi_main and BEFORE parse_boot_conf. UKI mode skips identity
 * validation (the trust anchor is the signed PE) but still surfaces
 * esp_size_mb so post-boot tools see the partition size. */
static void esp_integrity_check(EFI_HANDLE part_handle,
                                 EFI_BLOCK_IO_PROTOCOL *part_bio,
                                 UINT32 partition_size_mib)
{
    post_code16(POST16_BL_ESP_INTEGRITY);

    g_boot_info_ptr->esp_size_mb = partition_size_mib;
    g_boot_info_ptr->esp_filesystem_type = 0;
    g_boot_info_ptr->esp_type_guid_valid = 0;

    /* UKI fast-skip: in UKI mode the disk is not load-bearing for
     * kernel.exe / boot.conf, and a valid signed UKI launched from
     * PXE / RAM-disk / non-GPT media must not fail this gate. */
    if (g_uki_kernel_ptr && g_uki_kernel_size > 0) {
        serial_early_print("[BOOT] ESP integrity: skipped (UKI mode -- "
                           "trust anchor is signed PE image)\n");
        post_code16(POST16_BL_ESP_INTEGRITY_OK);
        return;
    }

    if (!part_handle || !part_bio || !part_bio->Media) {
        serial_early_print("[WARN] ESP integrity: no boot device handle / "
                           "BlockIO -- skipping\n");
        return;
    }

    /* Non-GPT boot path (network, MBR-only test image): no GPT type
     * GUID to check, but the BPB and required-files gates still
     * apply for any partitioned FAT volume. */
    if (g_boot_info_ptr->boot_partition_style != 2) {
        serial_early_print("[BOOT] ESP integrity: non-GPT boot device "
                           "-- skipping GPT type-GUID check\n");
    } else {
        post_code16(POST16_BL_ESP_GPT);
        (void)esp_check_gpt_type_guid(part_handle);
    }

    post_code16(POST16_BL_ESP_BPB);
    esp_check_fat_bpb(part_bio, partition_size_mib);

    post_code16(POST16_BL_ESP_FILES);
    esp_check_required_files(part_handle);

    post_code16(POST16_BL_ESP_INTEGRITY_OK);
}

/* ===================================================================
 * TODO-21 A/B dual-slot boot: pre-ExitBootServices slot selection.
 *
 * select_active_slot() reads the on-disk ab_boot_metadata record from the
 * A/B-metadata GPT partition (type GUID below) and publishes the chosen
 * root slot in g_boot_info_ptr->active_slot (0=A, 1=B). It is the SOLE
 * authority for slot choice -- the kernel mounts EXACTLY this slot and
 * refuses to mark a mismatched slot good. READ-ONLY: the tries increment
 * and the power-fail-atomic metadata write are owned by the failure-counting
 * and metadata-integrity work, not this section.
 *
 * Fail posture: a CORRUPT GPT structure or an unreadable located metadata
 * partition on a GPT disk is fail-closed (boot_fatal), matching
 * esp_check_gpt_type_guid -- guessing on a damaged A/B disk is worse than a
 * clear operator error. A non-A/B disk (no metadata partition), a non-GPT /
 * non-disk boot, or content-invalid metadata on a READABLE partition all
 * resolve to Slot A so the common case always boots.
 * =================================================================== */

/* Metadata partition type GUID, written by tools/make-system-disk.c --ab.
 * Raw on-disk byte order: Data1/Data2/Data3 little-endian, Data4 big-endian
 * (same convention as g_esp_type_guid). DA000000-0000-4978-4D44-000000000001:
 * "MD" in Data4[0..1] distinguishes it from the IXFS slot roots ("FS",
 * ...4653...), so this scan never matches a bootable root slot. */
static const UINT8 g_ab_meta_type_guid[16] = {
    0x00, 0x00, 0x00, 0xDA,    /* Data1 LE: 0xDA000000 */
    0x00, 0x00,                /* Data2 LE: 0x0000     */
    0x78, 0x49,                /* Data3 LE: 0x4978     */
    0x4D, 0x44, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01   /* Data4 BE */
};

/* The two redundant ab_boot_metadata copies live at fixed byte offsets
 * AB_META_COPY0_BYTE_OFF / AB_META_COPY1_BYTE_OFF (shared read/write contract,
 * defined in ab_boot_metadata.h). This section only reads. */

/* Map the 0/1 slot encoding to a display character for serial logs. */
static char ab_slot_char(unsigned int slot)
{
    return (slot == AB_BOOT_SLOT_B) ? 'B' : 'A';
}

/* The successful/pending/unbootable selection state model lives in the shared
 * header (ab_boot_meta_choose_slot) so the bootloader's choice and the
 * kernel/test view are byte-identical. */

/* Standard GPT CRC-32 (IEEE 802.3, polynomial 0xEDB88320, init/final-xor
 * 0xFFFFFFFF) over `len` bytes. This is NOT bl_crc32c() -- that is Castagnoli
 * (CRC-32C) for a different purpose; GPT mandates the IEEE polynomial. */
static UINT32 bl_gpt_crc32(const UINT8 *data, UINTN len)
{
    UINT32 crc = 0xFFFFFFFFu;
    UINTN i;
    int b;
    for (i = 0; i < len; i++) {
        crc ^= (UINT32)data[i];
        for (b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (UINT32)(-(INT32)(crc & 1u)));
    }
    return crc ^ 0xFFFFFFFFu;
}

/* Read + fully validate the GPT whose header sits at `header_lba` (primary=1 or
 * backup=LastBlock). Validates signature, self-location (`current_lba`), all
 * bounded shape fields, the header CRC (header_crc32 zeroed over header_size
 * bytes), AND the partition-entry-array CRC over the exact declared table
 * bytes. On success, AllocatePool()s the entry table, fills it (CRC-verified),
 * and returns EFI_SUCCESS with *out_table (caller FreePool()s), *out_num,
 * *out_entsize. On ANY failure returns an error WITHOUT a fatal and WITHOUT a
 * leaked allocation, so the caller can try the alternate GPT before failing
 * closed. */
static EFI_STATUS ab_load_gpt_table(EFI_BLOCK_IO_PROTOCOL *bio, UINT64 header_lba,
                                    UINT8 **out_table, UINT32 *out_num,
                                    UINT32 *out_entsize)
{
    EFI_BLOCK_IO_MEDIA *pm = bio->Media;
    UINT8 hdr_buf[4096];
    EFI_STATUS status;
    UINTN i;

    *out_table = (UINT8 *)0;
    *out_num = 0;
    *out_entsize = 0;

    if (header_lba > pm->LastBlock)
        return EFI_NOT_FOUND;
    status = bl_read_blocks_aligned(bio, pm->MediaId, header_lba, pm->BlockSize, hdr_buf);
    if (EFI_ERROR(status))
        return status;

    struct esp_gpt_header hdr;
    efi_memcpy(&hdr, hdr_buf, sizeof(hdr));

    static const UINT8 expected_sig[8] = { 'E','F','I',' ','P','A','R','T' };
    for (i = 0; i < 8; i++)
        if (hdr.signature[i] != expected_sig[i])
            return EFI_NOT_FOUND;
    if (hdr.current_lba != header_lba)
        return EFI_NOT_FOUND;  /* a misplaced / mirror header -- not authoritative here */
    if (hdr.header_size < 92 || hdr.header_size > pm->BlockSize)
        return EFI_NOT_FOUND;

    /* Header CRC: zero the header_crc32 field (offset 16, 4 bytes) over a copy
     * of the first header_size bytes, then compare. */
    {
        UINT8 hc[4096];
        efi_memcpy(hc, hdr_buf, hdr.header_size);
        hc[16] = 0; hc[17] = 0; hc[18] = 0; hc[19] = 0;
        if (bl_gpt_crc32(hc, hdr.header_size) != hdr.header_crc32)
            return EFI_NOT_FOUND;
    }

    if (hdr.size_of_partition_entry < 128 || hdr.size_of_partition_entry > 4096 ||
        (hdr.size_of_partition_entry & (hdr.size_of_partition_entry - 1)) != 0)
        return EFI_NOT_FOUND;
    if (hdr.num_partition_entries == 0 || hdr.num_partition_entries > 1024)
        return EFI_NOT_FOUND;
    UINT64 table_bytes = (UINT64)hdr.num_partition_entries *
                         (UINT64)hdr.size_of_partition_entry;
    if (table_bytes > 1024ULL * 1024ULL)
        return EFI_NOT_FOUND;
    if (hdr.partition_entry_lba < 2 || hdr.partition_entry_lba > pm->LastBlock)
        return EFI_NOT_FOUND;
    UINT64 entry_blocks = (table_bytes + (UINT64)pm->BlockSize - 1) /
                          (UINT64)pm->BlockSize;
    if (entry_blocks == 0 ||
        hdr.partition_entry_lba + entry_blocks > pm->LastBlock + 1)
        return EFI_NOT_FOUND;

    UINT64 alloc_bytes = entry_blocks * (UINT64)pm->BlockSize;
    UINT8 *table = (UINT8 *)0;
    status = gBS->AllocatePool(EfiLoaderData, (UINTN)alloc_bytes, (VOID **)&table);
    if (EFI_ERROR(status) || !table)
        return EFI_NOT_FOUND;
    /* AllocatePool returns 8-byte-aligned memory (UEFI 2.10 spec 7.2), so the
     * table satisfies any POWER-OF-TWO IoAlign <= 8 (0/1/2/4/8) -- read the
     * whole array directly. A non-power-of-two IoAlign (3/5/6/7) is NOT
     * satisfied by 8-byte alignment and is rejected by bl_read_blocks_aligned;
     * route those (and IoAlign > 8) through the helper block-by-block so the
     * single enforcement point fails closed (EFI_UNSUPPORTED) on bad alignment.
     * Block-by-block also avoids a second full-size table allocation (peak
     * memory matters on the A/B primary+backup path that holds both tables). */
    if (pm->IoAlign <= 8u && (pm->IoAlign & (pm->IoAlign - 1u)) == 0u) {
        status = bio->ReadBlocks(bio, pm->MediaId, hdr.partition_entry_lba,
                                 (UINTN)alloc_bytes, table);
    } else {
        status = EFI_SUCCESS;
        for (UINT64 b = 0; b < entry_blocks; b++) {
            status = bl_read_blocks_aligned(bio, pm->MediaId,
                         hdr.partition_entry_lba + b, pm->BlockSize,
                         table + b * (UINT64)pm->BlockSize);
            if (EFI_ERROR(status))
                break;
        }
    }
    if (EFI_ERROR(status)) {
        gBS->FreePool(table);
        return status;
    }
    /* Entry-array CRC is over the EXACT declared table bytes, not the block-
     * rounded read. A mismatch means a torn/hostile table -- reject. */
    if (bl_gpt_crc32(table, (UINTN)table_bytes) != hdr.partition_entry_array_crc32) {
        gBS->FreePool(table);
        return EFI_NOT_FOUND;
    }

    *out_table = table;
    *out_num = hdr.num_partition_entries;
    *out_entsize = hdr.size_of_partition_entry;
    return EFI_SUCCESS;
}

/* Scan a CRC-validated, in-memory GPT entry table for the A/B-metadata type
 * GUID. Returns 1 and fills out_lba + out_blocks when found, 0 when absent, and
 * calls boot_fatal() (never returns) if the located entry's LBA range is
 * outside the disk (the table is CRC-valid, so a bad range is real corruption). */
static int ab_scan_table_for_md(const UINT8 *table, UINT32 num, UINT32 entsize,
                                UINT64 last_block, UINT64 *out_lba,
                                UINT64 *out_blocks)
{
    UINT32 e;
    UINTN i;
    for (e = 0; e < num; e++) {
        const UINT8 *ent = table + (UINTN)e * (UINTN)entsize;
        int match = 1;
        for (i = 0; i < 16; i++)
            if (ent[i] != g_ab_meta_type_guid[i]) { match = 0; break; }
        if (!match)
            continue;
        UINT64 first_lba = 0, last_lba = 0;
        for (i = 0; i < 8; i++) {
            first_lba |= (UINT64)ent[32 + i] << (i * 8);
            last_lba  |= (UINT64)ent[40 + i] << (i * 8);
        }
        if (first_lba < 2 || last_lba < first_lba || last_lba > last_block) {
            boot_fatal(BOOT_ERR_ESP_TYPE_GUID,
                       "A/B select: metadata partition LBA range invalid",
                       "Located the A/B metadata entry but its LBA range is "
                       "outside the parent disk.");
        }
        *out_lba = first_lba;
        *out_blocks = last_lba - first_lba + 1;
        return 1;
    }
    return 0;
}

/* Locate the A/B-metadata partition on the parent disk and return its first
 * LBA + size (in blocks) plus the open BlockIO. Returns EFI_SUCCESS when
 * found, EFI_NOT_FOUND when the disk genuinely has no metadata partition (both
 * the authoritative GPT and the redundant copy agree), and calls boot_fatal()
 * (never returns) when BOTH GPTs fail validation OR the two CRC-valid GPTs
 * DISAGREE on the metadata partition (split-brain corruption) -- guessing a
 * slot from an inconsistent partition table could boot the wrong (or
 * rolled-back) root. Each entry table is CRC-validated before the type-GUID
 * scan, so a torn GPT cannot masquerade as a clean non-A/B disk, and a
 * valid-but-stale primary cannot silently hide the metadata the backup holds. */
static EFI_STATUS ab_find_meta_partition(EFI_HANDLE part_handle,
                                          EFI_BLOCK_IO_PROTOCOL **out_bio,
                                          UINT64 *out_md_lba,
                                          UINT64 *out_md_blocks)
{
    EFI_GUID bio_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
    EFI_HANDLE parent_handle = (EFI_HANDLE)0;
    EFI_BLOCK_IO_PROTOCOL *bio = (EFI_BLOCK_IO_PROTOCOL *)0;
    EFI_STATUS status;

    *out_bio = (EFI_BLOCK_IO_PROTOCOL *)0;
    *out_md_lba = 0;
    *out_md_blocks = 0;

    status = esp_find_parent_disk(part_handle, &parent_handle);
    if (EFI_ERROR(status))
        return EFI_NOT_FOUND;  /* PXE / RAM-disk / no parent -- not an A/B disk */

    status = gBS->HandleProtocol(parent_handle, &bio_guid, (VOID **)&bio);
    if (EFI_ERROR(status) || !bio || !bio->Media) {
        boot_fatal(BOOT_ERR_ESP_TYPE_GUID,
                   "A/B select: parent disk BlockIO missing",
                   "Cannot read the GPT to locate the A/B metadata "
                   "partition on a GPT-classified disk.");
    }

    EFI_BLOCK_IO_MEDIA *pm = bio->Media;
    if (!pm->MediaPresent || pm->BlockSize < sizeof(struct esp_gpt_header) ||
        pm->BlockSize > 4096 || pm->LastBlock < 2) {
        boot_fatal(BOOT_ERR_ESP_TYPE_GUID,
                   "A/B select: parent media geometry unusable",
                   "BlockSize must be 92..4096 with media present and "
                   "LastBlock >= 2 to host a GPT header.");
    }

    /* Load + CRC-validate BOTH the primary GPT (LBA 1) and the backup GPT
     * (LastBlock) independently, then reconcile. Reading both unconditionally
     * (even when the primary is fine) is what lets a CRC-valid-but-stale or
     * contradictory backup be detected instead of silently trusting a primary
     * that disagrees with its redundant copy. */
    UINT8 *ptab = (UINT8 *)0, *btab = (UINT8 *)0;
    UINT32 pnum = 0, pent = 0, bnum = 0, bent = 0;
    int pvalid = !EFI_ERROR(ab_load_gpt_table(bio, 1, &ptab, &pnum, &pent));
    int p_has = 0;
    UINT64 p_lba = 0, p_blk = 0;
    if (pvalid)
        p_has = ab_scan_table_for_md(ptab, pnum, pent, pm->LastBlock, &p_lba, &p_blk);

    int bvalid = !EFI_ERROR(ab_load_gpt_table(bio, pm->LastBlock, &btab, &bnum, &bent));
    int b_has = 0;
    UINT64 b_lba = 0, b_blk = 0;
    if (bvalid)
        b_has = ab_scan_table_for_md(btab, bnum, bent, pm->LastBlock, &b_lba, &b_blk);

    if (pvalid) gBS->FreePool(ptab);
    if (bvalid) gBS->FreePool(btab);

    /* Reconcile the two copies (fail-closed). The only non-fatal "no A/B"
     * answer is BOTH-valid-and-both-no-MD; any unrecoverable invalid copy or a
     * presence disagreement is fatal. */
    int decision = ab_boot_meta_reconcile_gpt(pvalid, p_has, bvalid, b_has);
    if (decision == AB_GPT_USE_PRIMARY) {
        /* If the backup ALSO carries the metadata, the two copies must point at
         * the identical partition or the table is split-brained. */
        if (bvalid && b_has && (b_lba != p_lba || b_blk != p_blk)) {
            boot_fatal(BOOT_ERR_ESP_TYPE_GUID,
                       "A/B select: primary/backup GPT metadata range mismatch",
                       "Both GPTs carry the A/B metadata partition but at "
                       "different LBA ranges -- the partition table is "
                       "inconsistent. Reflash or boot recovery media.");
        }
        *out_bio = bio;
        *out_md_lba = p_lba;
        *out_md_blocks = p_blk;
        return EFI_SUCCESS;
    }
    if (decision == AB_GPT_USE_BACKUP) {
        *out_bio = bio;
        *out_md_lba = b_lba;
        *out_md_blocks = b_blk;
        return EFI_SUCCESS;
    }
    if (decision == AB_GPT_NO_AB)
        return EFI_NOT_FOUND;  /* both copies validate and agree: non-A/B disk */

    boot_fatal(BOOT_ERR_ESP_TYPE_GUID,
               "A/B select: GPT corrupt or inconsistent",
               "The primary and backup GPT are not both valid-and-agreeing on "
               "the A/B metadata partition (a corrupt copy or a primary/backup "
               "disagreement) -- cannot trust slot selection. Reflash or boot "
               "recovery media.");
    /* boot_fatal never returns; keeps the compiler happy about the return. */
    return EFI_NOT_FOUND;
}

/* Read one ab_boot_metadata copy from the metadata partition at the given
 * byte offset into *out. Returns 1 on a successful block read (the record
 * still has to pass ab_boot_meta_is_valid), 0 on a read/geometry error. */
static int ab_read_meta_copy(EFI_BLOCK_IO_PROTOCOL *bio, UINT64 md_lba,
                             UINT64 md_blocks, UINT32 byte_off,
                             struct ab_boot_metadata *out)
{
    EFI_BLOCK_IO_MEDIA *pm = bio->Media;
    UINT32 bs = pm->BlockSize;
    UINT64 lba = md_lba + (UINT64)(byte_off / bs);
    UINT32 in_blk = byte_off % bs;
    UINT8 blk[4096];
    EFI_STATUS status;

    /* The copy's record must fit entirely within the partition and within one
     * block (AB_BOOT_META_SIZE is 60 bytes; both copies sit block-aligned). */
    if (in_blk + AB_BOOT_META_SIZE > bs)
        return 0;
    if ((UINT64)(byte_off / bs) >= md_blocks)
        return 0;
    if (lba > pm->LastBlock)
        return 0;

    status = bl_read_blocks_aligned(bio, pm->MediaId, lba, bs, blk);
    if (EFI_ERROR(status))
        return 0;
    efi_memcpy(out, &blk[in_blk], AB_BOOT_META_SIZE);
    return 1;
}

/* Append a NUL-terminated string to buf at *p, bounded by cap, advancing *p.
 * serial_early_print has no formatter, so the A/B diagnostics assemble lines by
 * parts; this keeps that bounded-copy idiom in one place. */
static void ab_append(char *buf, UINTN *p, UINTN cap, const char *s)
{
    UINTN i;
    for (i = 0; s[i] && *p < cap - 1; i++) buf[(*p)++] = s[i];
}

/* On-screen rollback notice: a self-contained banner (own dark background, so
 * it is legible over any firmware-logo / splash state -- the AA text blends
 * against the banner fill, not an unknown backdrop) drawn near the bottom of
 * the framebuffer when a slot rollback or the both-exhausted stopgap fires.
 * No-op when there is no usable framebuffer (headless). The kernel repaints
 * shortly after, so this is the brief boot-window notice; the persistent
 * surface is the kernel boot-diagnostics slot-status view. */
static void ab_draw_rollback_banner(unsigned int slot, int both_exhausted)
{
    if (!gFramebuffer || gFbWidth < 200 || gFbHeight < 120) return;
    UINT32 bh = 40;
    UINT32 by = (gFbHeight > bh + 24) ? (gFbHeight - bh - 24) : 0;
    UINT32 bg = fb_pack_rgb(0x1F, 0x1F, 0x28);  /* dark slate banner */
    bsod_fill_rect(0, by, gFbWidth, bh, bg);

    char msg[64];
    UINTN p = 0;
    if (both_exhausted)
        ab_append(msg, &p, sizeof msg,
                  "Recovery: no verified slot -- attempting Slot ");
    else
        ab_append(msg, &p, sizeof msg, "Reverting to previous version (Slot ");
    if (p < sizeof msg - 1) msg[p++] = ab_slot_char(slot);
    if (!both_exhausted && p < sizeof msg - 1) msg[p++] = ')';
    msg[p] = '\0';

    UINT32 tw = bsod_aa_string_width(msg, bsod_aa_BODY);
    UINT32 tx = (gFbWidth > tw) ? (gFbWidth - tw) / 2 : 8;
    UINT32 ty = by + (bh > 18 ? (bh - 18) / 2 : 0);
    bsod_aa_string(tx, ty, msg, bsod_aa_BODY, bsod_aa_BODY_data,
                   BSOD_AA_BODY_ASCENT, 0xFF, 0xD0, 0x60,  /* amber text */
                   0x1F, 0x1F, 0x28);                       /* match banner bg */
}

static void select_active_slot(EFI_HANDLE part_handle)
{
    post_code16(POST16_BL_AB_SELECT);

    /* Default Slot A: every early-return path below leaves this in place so a
     * non-A/B disk, a non-disk boot, or an older zero-filling bootloader all
     * boot the original root. */
    g_boot_info_ptr->active_slot = AB_BOOT_SLOT_A;

    if (!part_handle) {
        serial_early_print("[BOOT] A/B select: no boot device handle -- "
                           "Slot A\n");
        post_code16(POST16_BL_AB_SELECT_OK);
        return;
    }
    if (g_boot_info_ptr->boot_partition_style != 2) {
        serial_early_print("[BOOT] A/B select: non-GPT boot device -- "
                           "Slot A\n");
        post_code16(POST16_BL_AB_SELECT_OK);
        return;
    }

    EFI_BLOCK_IO_PROTOCOL *bio = (EFI_BLOCK_IO_PROTOCOL *)0;
    UINT64 md_lba = 0, md_blocks = 0;
    EFI_STATUS status = ab_find_meta_partition(part_handle, &bio,
                                               &md_lba, &md_blocks);
    if (status == EFI_NOT_FOUND) {
        serial_early_print("[BOOT] A/B select: no metadata partition "
                           "(single-slot disk) -- Slot A\n");
        post_code16(POST16_BL_AB_SELECT_OK);
        return;
    }
    /* ab_find_meta_partition either returns EFI_SUCCESS or boot_fatal()s on a
     * corrupt GPT; EFI_NOT_FOUND was handled above. */

    /* Publish the reconciled MD-partition range so the kernel write path
     * (mark-boot-successful) locates the partition without re-deriving GPT
     * state via the weaker kernel gpt_parse. The location is authoritative even
     * if the metadata record itself is uninitialized/corrupt. */
    g_boot_info_ptr->ab_meta_lba = md_lba;
    g_boot_info_ptr->ab_meta_block_count = (md_blocks > 0xFFFFFFFFull)
                                           ? 0xFFFFFFFFu : (UINT32)md_blocks;

    struct ab_boot_metadata copy0, copy1;
    int got0 = ab_read_meta_copy(bio, md_lba, md_blocks,
                                 AB_META_COPY0_BYTE_OFF, &copy0);
    int got1 = ab_read_meta_copy(bio, md_lba, md_blocks,
                                 AB_META_COPY1_BYTE_OFF, &copy1);
    if (!got0 || !got1) {
        /* BOTH redundant copies must be READABLE to trust slot selection. A
         * torn write leaves one copy CRC-invalid but still readable (got==1)
         * -> select_newest correctly picks the valid one (the redundant-copy
         * integrity resilience path is preserved). But a BLOCK-unreadable copy
         * (got==0) means we cannot see whether it held a NEWER generation than
         * the readable copy -- selecting the lone readable record could boot a
         * stale, exhausted, or rolled-back slot. Fail closed instead. */
        boot_fatal(BOOT_ERR_ESP_TYPE_GUID,
                   "A/B select: metadata block unreadable",
                   "A redundant A/B metadata copy could not be read -- cannot "
                   "prove which copy is newest, so slot selection is not "
                   "trustworthy. Reflash or boot recovery media.");
    }

    /* Both copies are readable here; select_newest compares both generations. */
    const struct ab_boot_metadata *winner = (const struct ab_boot_metadata *)0;
    int valid = ab_boot_meta_select_newest(
                    got0 ? &copy0 : (const struct ab_boot_metadata *)0,
                    got1 ? &copy1 : (const struct ab_boot_metadata *)0,
                    &winner);
    if (!valid || !winner) {
        /* Both readable copies failed validation. A clearly UNINITIALIZED
         * partition (first boot) factory-defaults to Slot A -- there is no
         * prior state to roll back to. But a previously-written record that
         * has since corrupted must NOT default to Slot A: that could bypass a
         * legitimate rollback (Slot B may be the intended/good slot) and boot a
         * stale, exhausted, or vulnerable root. Fail closed instead.
         *
         * "Uninitialized" requires BOTH copies readable AND every byte zero --
         * a zeroed magic word with any other nonzero byte is a damaged record
         * (not first boot), and an unreadable copy on a located partition is a
         * fault, not a pristine disk. Either case -> fail closed. */
        int uninit = got0 && got1;
        if (uninit) {
            const unsigned char *b0 = (const unsigned char *)&copy0;
            const unsigned char *b1 = (const unsigned char *)&copy1;
            UINTN z;
            for (z = 0; z < AB_BOOT_META_SIZE; z++) {
                if (b0[z] != 0u || b1[z] != 0u) { uninit = 0; break; }
            }
        }
        if (uninit) {
            serial_early_print("[BOOT] A/B select: metadata uninitialized "
                               "(first boot) -- Slot A\n");
            g_boot_info_ptr->active_slot = AB_BOOT_SLOT_A;
            /* Publish a factory-default snapshot + set the producer-valid
             * marker so the kernel VPD renders the real first-boot Slot A
             * state (tries=0, pending) -- NOT "snapshot unavailable", which
             * is reserved for an older loader that never wrote these bytes.
             * The remaining fields stay 0 (= factory default: reason normal,
             * from Slot A, both tries 0, neither successful). */
            g_boot_info_ptr->ab_select_reason = (UINT8)AB_BOOT_SEL_NORMAL;
            g_boot_info_ptr->ab_from_slot = AB_BOOT_SLOT_A;
            g_boot_info_ptr->ab_slot_tries[AB_BOOT_SLOT_A] = 0;
            g_boot_info_ptr->ab_slot_tries[AB_BOOT_SLOT_B] = 0;
            g_boot_info_ptr->ab_slot_flags = 0;
            g_boot_info_ptr->ab_status_valid = AB_STATUS_VALID_MAGIC;
            post_code16(POST16_BL_AB_SELECT_OK);
            return;
        }
        boot_fatal(BOOT_ERR_ESP_TYPE_GUID,
                   "A/B select: metadata corrupt on both copies",
                   "An initialized A/B metadata record failed validation on "
                   "both redundant copies -- selecting a default slot could "
                   "bypass rollback. Reflash or boot recovery media.");
    }

    struct ab_boot_decision dec = ab_boot_meta_decide(winner);
    unsigned int slot = dec.slot;
    g_boot_info_ptr->active_slot = (UINT8)slot;

    /* Publish the as-selected slot-status snapshot for the kernel VPD (sec6).
     * winner is the record read at selection time (pre tries-increment), so
     * tries here is the accumulated prior-boot failure count -- the meaningful
     * diagnostic value. Both-exhausted/rollback reason + from_slot let the kernel
     * render the terminal vs rollback message without re-reading mutating disk. */
    g_boot_info_ptr->ab_select_reason = (UINT8)dec.reason;
    g_boot_info_ptr->ab_from_slot = (UINT8)dec.from_slot;
    g_boot_info_ptr->ab_slot_tries[AB_BOOT_SLOT_A] =
        (UINT8)winner->slot[AB_BOOT_SLOT_A].tries;
    g_boot_info_ptr->ab_slot_tries[AB_BOOT_SLOT_B] =
        (UINT8)winner->slot[AB_BOOT_SLOT_B].tries;
    g_boot_info_ptr->ab_slot_flags = (UINT8)(
        (winner->slot[AB_BOOT_SLOT_A].successful
             ? ab_boot_slot_flag(AB_BOOT_SLOT_A) : 0u) |
        (winner->slot[AB_BOOT_SLOT_B].successful
             ? ab_boot_slot_flag(AB_BOOT_SLOT_B) : 0u));
    /* Producer-valid marker: tell the kernel this snapshot was actually
     * published (an older same-version loader leaves it zero). */
    g_boot_info_ptr->ab_status_valid = AB_STATUS_VALID_MAGIC;

    /* Rollback / both-exhausted diagnostics: serial always, on-screen banner
     * when a framebuffer is available. tries <= AB_BOOT_MAX_TRIES so a single
     * digit covers the count. */
    if (dec.reason == AB_BOOT_SEL_ROLLBACK) {
        char line[96];
        UINTN p = 0;
        ab_append(line, &p, sizeof line, "[BOOT] A/B: Slot ");
        if (p < sizeof line - 1) line[p++] = ab_slot_char(dec.from_slot);
        ab_append(line, &p, sizeof line, " failed ");
        if (p < sizeof line - 1)
            line[p++] = (char)('0' + (dec.from_tries % 10u));
        ab_append(line, &p, sizeof line, " times -- rolling back to Slot ");
        if (p < sizeof line - 1) line[p++] = ab_slot_char(slot);
        if (p < sizeof line - 1) line[p++] = '\n';
        line[p] = '\0';
        serial_early_print(line);
        ab_draw_rollback_banner(slot, 0);
    } else if (dec.reason == AB_BOOT_SEL_BOTH_EXHAUSTED) {
        char line[112];
        UINTN p = 0;
        ab_append(line, &p, sizeof line,
                  "[BOOT] A/B: WARNING both slots exhausted (no verified slot) "
                  "-- booting least-bad Slot ");
        if (p < sizeof line - 1) line[p++] = ab_slot_char(slot);
        if (p < sizeof line - 1) line[p++] = '\n';
        line[p] = '\0';
        serial_early_print(line);
        ab_draw_rollback_banner(slot, 1);
    }

    /* "[BOOT] Booting Slot %c (tries=%u, successful=%u)" -- serial_early_print
     * has no formatter, so assemble the line by parts. */
    {
        char line[80];
        UINTN p = 0;
        const char *pfx = "[BOOT] Booting Slot ";
        UINTN k;
        for (k = 0; pfx[k] && p < sizeof(line) - 1; k++) line[p++] = pfx[k];
        if (p < sizeof(line) - 1) line[p++] = ab_slot_char(slot);
        const char *mid = " (tries=";
        for (k = 0; mid[k] && p < sizeof(line) - 1; k++) line[p++] = mid[k];
        if (p < sizeof(line) - 1)
            line[p++] = (char)('0' + (winner->slot[slot].tries % 10u));
        const char *mid2 = ", successful=";
        for (k = 0; mid2[k] && p < sizeof(line) - 1; k++) line[p++] = mid2[k];
        if (p < sizeof(line) - 1)
            line[p++] = (char)('0' + (winner->slot[slot].successful ? 1u : 0u));
        if (p < sizeof(line) - 1) line[p++] = ')';
        if (p < sizeof(line) - 1) line[p++] = '\n';
        line[p] = '\0';
        serial_early_print(line);
    }

    post_code16(POST16_BL_AB_SELECT_OK);
}

/* A/B tries-increment (TODO-21 sec4): increment the selected slot's tries
 * pre-EBS, AFTER load_kernel succeeds (so a kernel-LOAD/firmware failure is not
 * charged to the slot) and before ExitBootServices (EFI_BLOCK_IO still live). A
 * crash before the kernel reaches mark-boot-successful leaves tries
 * incremented; after AB_BOOT_MAX_TRIES the NEXT boot's selection rolls back.
 * Power-fail-atomic single-copy WriteBlocks read-modify-write (sec7 helpers:
 * overwrite the lower-generation copy with a strictly-greater generation, so a
 * crash mid-write leaves the other copy valid). No-op when there is no A/B
 * metadata partition; smoke-validated + reset by the kernel mark-good on every
 * successful boot. */
static void ab_bl_increment_tries(EFI_HANDLE part_handle)
{
    EFI_GUID bio_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
    EFI_HANDLE parent = (EFI_HANDLE)0;
    EFI_BLOCK_IO_PROTOCOL *bio = (EFI_BLOCK_IO_PROTOCOL *)0;
    UINT64 md_lba = g_boot_info_ptr->ab_meta_lba;
    unsigned int slot = g_boot_info_ptr->active_slot;
    EFI_BLOCK_IO_MEDIA *pm;
    UINT32 bs;
    UINT8 *raw = (UINT8 *)0;   /* AllocatePool pointer (for FreePool) */
    UINT8 *buf = (UINT8 *)0;   /* IoAlign-aligned I/O buffer within raw */
    UINTN align;
    struct ab_boot_metadata c0, c1, rec;
    const struct ab_boot_metadata *win = (const struct ab_boot_metadata *)0;
    int got0 = 0, got1 = 0, v0 = 0, v1 = 0;
    unsigned int next_gen, target;
    UINT64 wlba;
    UINT32 off;

    if (md_lba == 0 || slot >= AB_BOOT_SLOT_COUNT)
        return;   /* no A/B metadata partition / bad slot -- nothing to count */
    if (esp_find_parent_disk(part_handle, &parent) != EFI_SUCCESS)
        return;
    if (EFI_ERROR(gBS->HandleProtocol(parent, &bio_guid, (VOID **)&bio)) ||
        !bio || !bio->Media)
        return;
    pm = bio->Media;
    bs = pm->BlockSize;
    if (!pm->MediaPresent || bs < AB_BOOT_META_SIZE || bs > 4096)
        return;
    /* Both copies must be block-aligned for this single-block RMW (COPY0 at 0
     * is always aligned; COPY1 at 4096 requires bs to divide 4096 -- true for
     * every power-of-2 sector size, but reject anything else rather than write
     * to the wrong offset). Mirrors the reader's in-block-offset guard. */
    if (AB_META_COPY1_BYTE_OFF % bs != 0)
        return;
    /* IoAlign-compliant bounce buffer (UEFI 2.10: WriteBlocks buffers must meet
     * Media->IoAlign). Real storage commonly advertises 512/1024/4096-byte I/O
     * alignment; skipping those would silently disable try-counting (rollback
     * never fires) -- exactly the bare-metal behavior the feature must survive.
     * Over-allocate by `align` and align the I/O pointer up within it, keeping
     * `raw` for FreePool. IoAlign 0/1 = no requirement. */
    align = (pm->IoAlign > 1u) ? (UINTN)pm->IoAlign : 1u;
    if (align > 4096u || (align & (align - 1u)) != 0u)
        return;   /* implausible / non-power-of-2 alignment -- bound the alloc */
    if (EFI_ERROR(gBS->AllocatePool(EfiLoaderData, (UINTN)bs + align,
                                    (VOID **)&raw)) || !raw)
        return;
    buf = raw;
    if (align > 1u)
        buf = (UINT8 *)(((UINTN)raw + (align - 1u)) & ~(align - 1u));

    if (bio->ReadBlocks(bio, pm->MediaId, md_lba + AB_META_COPY0_BYTE_OFF / bs,
                        bs, buf) == EFI_SUCCESS) {
        efi_memcpy(&c0, buf, AB_BOOT_META_SIZE); got0 = 1;
        v0 = ab_boot_meta_is_valid(&c0);
    }
    if (bio->ReadBlocks(bio, pm->MediaId, md_lba + AB_META_COPY1_BYTE_OFF / bs,
                        bs, buf) == EFI_SUCCESS) {
        efi_memcpy(&c1, buf, AB_BOOT_META_SIZE); got1 = 1;
        v1 = ab_boot_meta_is_valid(&c1);
    }
    if (!got0 || !got1) {            /* a copy block unreadable -- do not guess */
        gBS->FreePool(raw);
        return;
    }

    if (ab_boot_meta_select_newest(v0 ? &c0 : (const struct ab_boot_metadata *)0,
                                   v1 ? &c1 : (const struct ab_boot_metadata *)0,
                                   &win) && win)
        rec = *win;
    else
        ab_boot_meta_default(&rec);
    next_gen = ab_boot_meta_next_generation(v0, c0.generation, v1, c1.generation);
    if (next_gen == 0u) {            /* generation exhausted -- refuse */
        gBS->FreePool(raw);
        return;
    }
    target = ab_boot_meta_write_target(v0, c0.generation, v1, c1.generation);

    rec.active_slot = slot;
    if (rec.slot[slot].tries < AB_BOOT_MAX_TRIES)
        rec.slot[slot].tries += 1u;   /* saturate at MAX; never wrap */
    rec.generation = next_gen;
    ab_boot_meta_finalize(&rec);

    off = (target == 0u) ? AB_META_COPY0_BYTE_OFF : AB_META_COPY1_BYTE_OFF;
    wlba = md_lba + off / bs;
    if (bio->ReadBlocks(bio, pm->MediaId, wlba, bs, buf) != EFI_SUCCESS) {
        gBS->FreePool(raw);
        return;
    }
    efi_memcpy(buf, &rec, AB_BOOT_META_SIZE);
    if (bio->WriteBlocks(bio, pm->MediaId, wlba, bs, buf) != EFI_SUCCESS) {
        serial_early_print("[WARN] A/B: tries-increment write failed\n");
        gBS->FreePool(raw);
        return;
    }
    /* Durability matters for rollback: if the write lives only in a volatile
     * cache and the flush fails, a crashing slot may never consume a try.
     * Treat a flush failure as a failed increment -- do not report success. */
    if (bio->FlushBlocks && EFI_ERROR(bio->FlushBlocks(bio))) {
        serial_early_print("[WARN] A/B: tries-increment flush failed "
                           "(not durable)\n");
        gBS->FreePool(raw);
        return;
    }
    gBS->FreePool(raw);
    serial_early_print("[BOOT] A/B: tries incremented for selected slot "
                       "(reset by kernel mark-good on a successful boot)\n");
}

/* Validated length of a firmware device path. Prefers the canonical
 * EFI_DEVICE_PATH_UTILITIES_PROTOCOL->GetDevicePathSize (firmware measures its
 * own path); falls back to a self-bounded walk that returns 0 unless a real
 * END_ENTIRE node is found within NET_DP_MAX_WALK. A return of 0 means "do not
 * trust this path" -- the scanner then skips it rather than reading heuristically
 * past the allocation (a heuristic cap is not an object
 * bound). */
#define NET_DP_MAX_WALK 8192
static UINTN net_dp_validated_size(const EFI_DEVICE_PATH_PROTOCOL *dp,
                                   EFI_DEVICE_PATH_UTILITIES_PROTOCOL *utils)
{
    UINTN sz;

    /* Only the firmware can safely measure its own device-path object, so we
     * delegate to GetDevicePathSize and never self-walk firmware-owned memory.
     * When the utility is absent (no UEFI core support) or returns a degenerate
     * or implausibly large measure, return 0 so the caller SKIPS path-based
     * classification rather than dereferencing past the allocation (Codex
     * adversarial: a heuristic cap is not an object bound). SNP/PXE enumeration
     * still classifies availability independently of the device path. */
    if (!dp || !utils || !utils->GetDevicePathSize)
        return 0;
    sz = utils->GetDevicePathSize(dp);
    if (sz < 4 || sz > NET_DP_MAX_WALK)
        return 0;
    return sz;
}

/* Scan a VALIDATED device-path buffer (dp through dp+size) for network
 * messaging nodes. MAC / IPv4 / IPv6 mark a PXE-style launch; URI marks UEFI
 * HTTP Boot. Every read is bounded by `size` (the validated object length), so
 * a malformed node Length can never advance past the allocation. */
static void net_scan_device_path(const EFI_DEVICE_PATH_PROTOCOL *dp, UINTN size)
{
    const UINT8 *p = (const UINT8 *)dp;
    UINTN walked = 0;

    if (!dp || size < 4)
        return;
    while (walked + 4 <= size) {
        const EFI_DEVICE_PATH_PROTOCOL *node =
            (const EFI_DEVICE_PATH_PROTOCOL *)(p + walked);
        UINT16 len;

        if (node->Type == EFI_DP_TYPE_END &&
            node->SubType == EFI_DP_SUBTYPE_END_ENTIRE)
            break;
        len = (UINT16)node->Length[0] | ((UINT16)node->Length[1] << 8);
        if (len < 4 || walked + len > size)
            break;

        if (node->Type == EFI_DP_TYPE_MESSAGING) {
            switch (node->SubType) {
            case EFI_DP_MSG_MAC:
                g_net_discovery.booted_from_network = 1;
                /* MAC node: header(4) + MacAddress(32) + IfType(1). Copy the
                 * first 6 bytes (Ethernet) -- guarded by len so the read stays
                 * within the validated buffer. */
                if (len >= 4 + 6 && !g_net_discovery.mac_len) {
                    UINT32 i;
                    for (i = 0; i < 6; i++)
                        g_net_discovery.mac[i] = p[walked + 4 + i];
                    g_net_discovery.mac_len = 6;
                }
                break;
            case EFI_DP_MSG_IPV4:
            case EFI_DP_MSG_IPV6:
                g_net_discovery.booted_from_network = 1;
                break;
            case EFI_DP_MSG_URI:
                g_net_discovery.booted_from_network = 1;
                g_net_discovery.http_boot = 1;
                break;
            default:
                break;
            }
        }
        walked += len;
    }
}

/* Discover firmware network-boot provenance before ExitBootServices.
 * Classifies booted-from-network from OUR boot paths (DeviceHandle device path
 * + LoadedImage FilePath), then records SNP/PXE protocol availability and reads
 * the NIC MAC + link state from the Simple Network Protocol Mode (read-only --
 * no Start/Initialize needed; Mode is valid once the protocol is installed).
 * Logs the result; a local-media launch is a clean no-network skip, not an
 * error. */
static void network_boot_discover(EFI_HANDLE dev_handle,
                                  EFI_DEVICE_PATH_PROTOCOL *file_path)
{
    EFI_GUID snp_guid  = EFI_SIMPLE_NETWORK_PROTOCOL_GUID;
    EFI_GUID pxe_guid  = EFI_PXE_BASE_CODE_PROTOCOL_GUID;
    EFI_GUID dp_guid   = EFI_DEVICE_PATH_PROTOCOL_GUID;
    EFI_GUID dpu_guid  = EFI_DEVICE_PATH_UTILITIES_PROTOCOL_GUID;
    EFI_DEVICE_PATH_UTILITIES_PROTOCOL *dpu = (EFI_DEVICE_PATH_UTILITIES_PROTOCOL *)0;
    EFI_HANDLE *handles = (EFI_HANDLE *)0;
    UINTN nhandles = 0;
    EFI_STATUS s;
    UINT32 mi;

    /* Zero the result explicitly -- do NOT rely on .bss being cleared. The
     * classification is conditional (net_scan only writes on a network node;
     * the SNP MAC capture is gated on mac_len==0), so uninitialised fields would
     * be read as garbage. OVMF pool-poisons freed pages with 0xAF, which a local
     * SATA boot otherwise surfaced as a bogus "boot=network MAC=af:af:.." line. */
    g_net_discovery.booted_from_network = 0;
    g_net_discovery.http_boot = 0;
    g_net_discovery.snp_available = 0;
    g_net_discovery.pxe_available = 0;
    g_net_discovery.link_up = 0;
    g_net_discovery.link_known = 0;
    g_net_discovery.mac_len = 0;
    for (mi = 0; mi < 6; mi++)
        g_net_discovery.mac[mi] = 0;

    /* DevicePathUtilities measures each path so the scanner stays inside the
     * real allocation; NULL is tolerated (the scanner falls back to a bounded
     * END-finding walk and skips paths without a valid END). */
    (void)gBS->LocateProtocol(&dpu_guid, (VOID *)0, (VOID **)&dpu);

    /* 1. Authoritative source classification: walk both boot-path components,
     * each bounded by its validated length. */
    if (dev_handle) {
        EFI_DEVICE_PATH_PROTOCOL *dev_path = (EFI_DEVICE_PATH_PROTOCOL *)0;
        if (!EFI_ERROR(gBS->HandleProtocol(dev_handle, &dp_guid,
                                           (VOID **)&dev_path)) && dev_path)
            net_scan_device_path(dev_path,
                                 net_dp_validated_size(dev_path, dpu));
    }
    if (file_path)
        net_scan_device_path(file_path,
                             net_dp_validated_size(file_path, dpu));

    /* 2. SNP availability + MAC/link, bound to the BOOT NIC. */
    s = gBS->LocateHandleBuffer(ByProtocol, &snp_guid, (VOID *)0,
                                &nhandles, &handles);
    if (!EFI_ERROR(s) && handles && nhandles) {
        EFI_SIMPLE_NETWORK_PROTOCOL *snp = (EFI_SIMPLE_NETWORK_PROTOCOL *)0;
        g_net_discovery.snp_available = 1;
        /* Prefer the SNP on OUR boot device handle: LocateHandleBuffer order is
         * unspecified, so on a multi-NIC system handles[0] may be the wrong
         * adapter. Fall back to the sole handle only when there is exactly one
         * NIC (unambiguous); with several NICs and no boot-handle match, leave
         * MAC/link unidentified rather than seed provenance from a guess. */
        if (dev_handle &&
            EFI_ERROR(gBS->HandleProtocol(dev_handle, &snp_guid, (VOID **)&snp)))
            snp = (EFI_SIMPLE_NETWORK_PROTOCOL *)0;
        if (!snp && nhandles == 1 &&
            EFI_ERROR(gBS->HandleProtocol(handles[0], &snp_guid, (VOID **)&snp)))
            snp = (EFI_SIMPLE_NETWORK_PROTOCOL *)0;
        if (snp && snp->Mode) {
            EFI_SIMPLE_NETWORK_MODE *m = snp->Mode;
            /* Link state is reportable only when the NIC supports media-present
             * detection; otherwise leave it unknown rather than claiming "up". */
            if (m->MediaPresentSupported) {
                g_net_discovery.link_up = m->MediaPresent ? 1 : 0;
                g_net_discovery.link_known = 1;
            }
            /* MAC capture is independent of link-state availability: record the
             * boot-NIC MAC even when link state is unknown. Cap to the local
             * 6-byte buffer regardless of HwAddressSize. */
            if (!g_net_discovery.mac_len) {
                UINT32 n = m->HwAddressSize, i;
                if (n > 6)
                    n = 6;
                for (i = 0; i < n; i++)
                    g_net_discovery.mac[i] = m->CurrentAddress.Addr[i];
                g_net_discovery.mac_len = (UINT8)n;
            }
        }
    }
    if (handles) {
        gBS->FreePool(handles);
        handles = (EFI_HANDLE *)0;
    }

    /* 3. PXE Base Code availability. */
    nhandles = 0;
    s = gBS->LocateHandleBuffer(ByProtocol, &pxe_guid, (VOID *)0,
                                &nhandles, &handles);
    if (!EFI_ERROR(s) && handles && nhandles)
        g_net_discovery.pxe_available = 1;
    if (handles)
        gBS->FreePool(handles);

    /* 4. Log. A pure local boot with no NIC stays silent except one line. */
    if (g_net_discovery.booted_from_network || g_net_discovery.snp_available) {
        serial_early_print("[NET] boot=");
        serial_early_print(g_net_discovery.booted_from_network ? "network" : "local");
        if (g_net_discovery.mac_len) {
            UINT32 i;
            serial_early_print(" MAC=");
            for (i = 0; i < g_net_discovery.mac_len; i++) {
                /* 2 hex digits per byte -> standard "00:11:22:.." MAC form. */
                static const char hx[] = "0123456789abcdef";
                UINT8 b = g_net_discovery.mac[i];
                if (i)
                    serial_early_print(":");
                serial_early_putchar(hx[(b >> 4) & 0xF]);
                serial_early_putchar(hx[b & 0xF]);
            }
        }
        serial_early_print(" link=");
        serial_early_print(g_net_discovery.link_known
                           ? (g_net_discovery.link_up ? "up" : "down")
                           : "unknown");
        serial_early_print(" SNP=");
        serial_early_print(g_net_discovery.snp_available ? "y" : "n");
        serial_early_print(" PXE=");
        serial_early_print(g_net_discovery.pxe_available ? "y" : "n");
        serial_early_print(" HTTPBoot=");
        serial_early_print(g_net_discovery.http_boot ? "y" : "n");
        serial_early_print("\n");
        boot_log_append("[NET] network-boot discovery complete\n");
    } else {
        serial_early_print("[NET] no network boot path (local media)\n");
    }
}

/* Log a dotted IPv4 address (4 bytes) to serial. */
static void net_log_ipv4(const UINT8 *ip)
{
    UINT32 i;
    for (i = 0; i < 4; i++) {
        if (i)
            serial_early_print(".");
        serial_early_print_uint((UINT32)ip[i]);
    }
}

/* Find DHCP option `tag` in the option TLV buffer [opts, opts+len), copying up
 * to `cap` value bytes into `out`. Returns the option value length (0 if not
 * found). Every access is bounded by `len` so a malformed firmware packet cannot
 * drive a read past the buffer. */
static UINT32 net_dhcp_find_option(const UINT8 *opts, UINT32 len, UINT8 tag,
                                   UINT8 *out, UINT32 cap)
{
    UINT32 i = 0;
    while (i < len) {
        UINT8 t = opts[i];
        UINT8 olen;
        if (t == 0) { i++; continue; }   /* pad option, no length */
        if (t == 255) break;             /* end option */
        if (i + 1 >= len) break;         /* truncated -- no length byte */
        olen = opts[i + 1];
        if (i + 2u + olen > len) break;  /* value runs past the buffer */
        if (t == tag) {
            UINT32 n = (olen < cap) ? olen : cap;
            UINT32 j;
            for (j = 0; j < n; j++)
                out[j] = opts[i + 2u + j];
            return olen;
        }
        i += 2u + olen;
    }
    return 0;
}

/* Capture DHCP/PXE provenance from the firmware PXE Base Code
 * cached DhcpAck. Binds to the BOOT NIC PXE handle (boot-handle preferred,
 * sole-handle fallback -- the wrong-NIC lesson from discovery), parses the BOOTP
 * fixed fields + DHCP options 3/54, preserves the raw packet, and logs redacted
 * provenance. Runs pre-ExitBootServices; a local boot or a NIC without a cached
 * DhcpAck is a clean no-op. */
static EFI_PXE_BASE_CODE_PROTOCOL *net_resolve_boot_pxe(EFI_HANDLE dev_handle);

static void net_dhcp_capture(EFI_HANDLE dev_handle)
{
    EFI_PXE_BASE_CODE_PROTOCOL *pxe;
    UINT32 i;

    /* Zero the result explicitly -- do not rely on .bss being cleared. */
    g_net_dhcp.valid = 0;
    g_net_dhcp_raw_valid = 0;
    for (i = 0; i < 4; i++) {
        g_net_dhcp.client_ip[i] = 0;
        g_net_dhcp.next_server_ip[i] = 0;
        g_net_dhcp.gateway_ip[i] = 0;
        g_net_dhcp.dhcp_server_ip[i] = 0;
    }
    g_net_dhcp.boot_file[0] = '\0';

    /* Resolve the boot-NIC PXE protocol (shared with the TFTP client). The boot device
     * handle is authoritative; the sole-handle fallback applies only on a
     * network-classified boot. Best-effort residual when the boot handle is a
     * child without PXE and DevicePathUtilities is absent -- tracked. */
    pxe = net_resolve_boot_pxe(dev_handle);
    if (!pxe || !pxe->Mode || !pxe->Mode->DhcpAckReceived)
        return;   /* no DHCP provenance available */

    {
        const EFI_PXE_BASE_CODE_PACKET *pkt = &pxe->Mode->DhcpAck;
        const EFI_PXE_BASE_CODE_DHCPV4_PACKET *v4 = &pkt->Dhcpv4;
        const UINT8 *raw = pkt->Raw;
        const UINT32 opts_off = 240;   /* after the 4-byte magic cookie at 236 */
        UINT8 opt[4];

        /* BOOTP fixed fields are always within the 1472-byte packet. */
        for (i = 0; i < 4; i++) {
            g_net_dhcp.client_ip[i] = v4->BootpYiAddr[i];
            g_net_dhcp.next_server_ip[i] = v4->BootpSiAddr[i];
            g_net_dhcp.gateway_ip[i] = v4->BootpGiAddr[i];
        }
        /* Copy + sanitize the boot filename to printable ASCII: DHCP data is
         * attacker-controlled, so a CR/LF/control byte could forge boot-log
         * lines. The true bytes survive in g_net_dhcp_raw for forensics. */
        for (i = 0; i < 127; i++) {
            UINT8 c = v4->BootpBootFile[i];
            if (c == 0)
                break;
            g_net_dhcp.boot_file[i] = (c >= 0x20 && c <= 0x7E) ? c : (UINT8)'?';
        }
        g_net_dhcp.boot_file[i] = '\0';

        /* Parse DHCP options only when the BOOTP magic cookie (99.130.83.99,
         * RFC 2132) is present at offset 236 -- otherwise the trailing bytes are
         * not a DHCP option field and must not be walked as one. Options can
         * extend past DhcpOptions[56] into the packet body, so walk from the
         * cookie boundary to the full 1472 bytes (bounded). */
        if (raw[236] == 0x63 && raw[237] == 0x82 &&
            raw[238] == 0x53 && raw[239] == 0x63) {
            if (net_dhcp_find_option(raw + opts_off, 1472u - opts_off, 3, opt, 4) >= 4)
                for (i = 0; i < 4; i++) g_net_dhcp.gateway_ip[i] = opt[i];
            if (net_dhcp_find_option(raw + opts_off, 1472u - opts_off, 54, opt, 4) >= 4)
                for (i = 0; i < 4; i++) g_net_dhcp.dhcp_server_ip[i] = opt[i];
        }

        for (i = 0; i < 1472; i++)
            g_net_dhcp_raw[i] = raw[i];
        g_net_dhcp_raw_valid = 1;
        g_net_dhcp.valid = 1;
    }

    /* Redacted log: standard provenance only. Vendor option 43 + root-path
     * option 17 are preserved in g_net_dhcp_raw but NOT printed (credentials). */
    serial_early_print("[NET] DHCP: client=");
    net_log_ipv4(g_net_dhcp.client_ip);
    serial_early_print(" gw=");
    net_log_ipv4(g_net_dhcp.gateway_ip);
    serial_early_print(" server=");
    net_log_ipv4(g_net_dhcp.dhcp_server_ip);
    serial_early_print(" file=");
    serial_early_print(g_net_dhcp.boot_file[0]
                       ? (const char *)g_net_dhcp.boot_file : "(none)");
    serial_early_print("\n");
    boot_log_append("[NET] DHCP provenance captured\n");
}

#define NET_TFTP_MAX_FILE      (64u * 1024u * 1024u)   /* kernel cap */
#define NET_TFTP_BOOTCONF_CAP  (1u * 1024u * 1024u)        /* boot.conf cap */
#define NET_TFTP_RETRIES       4
#define NET_TFTP_BACKOFF_MS    250

/* Shared boot-NIC PXE Base Code resolver (DHCP capture + TFTP client). Prefers the boot
 * device handle's PXE protocol (authoritative); falls back to the sole NIC only
 * when discovery classified a network boot. Returns NULL when no boot-NIC PXE
 * can be identified (caller stays local rather than failing fatally). */
static EFI_PXE_BASE_CODE_PROTOCOL *net_resolve_boot_pxe(EFI_HANDLE dev_handle)
{
    EFI_GUID pxe_guid = EFI_PXE_BASE_CODE_PROTOCOL_GUID;
    EFI_PXE_BASE_CODE_PROTOCOL *pxe = (EFI_PXE_BASE_CODE_PROTOCOL *)0;
    EFI_HANDLE *handles = (EFI_HANDLE *)0;
    UINTN nhandles = 0;
    EFI_STATUS s;

    if (dev_handle &&
        EFI_ERROR(gBS->HandleProtocol(dev_handle, &pxe_guid, (VOID **)&pxe)))
        pxe = (EFI_PXE_BASE_CODE_PROTOCOL *)0;
    if (!pxe && g_net_discovery.booted_from_network) {
        s = gBS->LocateHandleBuffer(ByProtocol, &pxe_guid, (VOID *)0,
                                    &nhandles, &handles);
        if (!EFI_ERROR(s) && handles && nhandles == 1 &&
            EFI_ERROR(gBS->HandleProtocol(handles[0], &pxe_guid, (VOID **)&pxe)))
            pxe = (EFI_PXE_BASE_CODE_PROTOCOL *)0;
        if (handles)
            gBS->FreePool(handles);
    }
    return pxe;
}

/* True if a Mtftp status is a transient error worth retrying. */
static int net_tftp_retryable(EFI_STATUS s)
{
    /* EFI_TFTP_ERROR is a server ERROR packet (missing-file/access-violation):
     * deterministic, NOT transient -- excluded so it is not retried. */
    return s == EFI_TIMEOUT || s == EFI_DEVICE_ERROR ||
           s == EFI_NO_RESPONSE || s == EFI_PROTOCOL_ERROR;
}

/* Map a fatal Mtftp/transfer status to a BOOT_ERR_TFTP_* code (for a caller
 * that decides to boot_fatal when there is no local fallback -- fallback-ordering policy). */
static UINT32 net_tftp_boot_err(EFI_STATUS s)
{
    /* A server TFTP ERROR packet during a fixed-path boot-asset fetch is
     * overwhelmingly file-not-found (TFTP has no auth); the precise
     * Mode->TftpError.ErrorCode split is a refinement owned downstream. */
    if (s == EFI_NOT_FOUND || s == EFI_TFTP_ERROR) return BOOT_ERR_TFTP_NOT_FOUND;
    if (s == EFI_TIMEOUT || s == EFI_NO_RESPONSE) return BOOT_ERR_TFTP_TIMEOUT;
    if (s == EFI_BUFFER_TOO_SMALL)  return BOOT_ERR_TFTP_OVER_CAP;
    return BOOT_ERR_TFTP_DEVICE;
}

/* Download `filename` over TFTP into a freshly allocated EfiLoaderData buffer
 * (sized from GET_FILE_SIZE, capped at `cap`). On success returns EFI_SUCCESS,
 * sets *out_buf (caller FreePages) + *out_size. GET_FILE_SIZE is an early-refusal
 * diagnostic only; READ_FILE still passes a capped BufferSize and treats a
 * returned size > cap / EFI_BUFFER_TOO_SMALL as over-cap (the object can change
 * between requests). Retries transient errors with backoff; returns a typed
 * status on hard failure -- the CALLER decides boot_fatal vs local fallback. */
static EFI_STATUS net_tftp_download(EFI_PXE_BASE_CODE_PROTOCOL *pxe,
                                    const char *filename, UINT64 cap,
                                    VOID **out_buf, UINT64 *out_size,
                                    UINTN *out_pages)
{
    EFI_IP_ADDRESS server;
    UINT8 *sb = (UINT8 *)server.Addr;
    UINT64 size = 0;
    UINTN block = 512, pages;
    EFI_PHYSICAL_ADDRESS buf_phys = 0;
    VOID *buf;
    EFI_STATUS s;
    int attempt, i;
    const UINT8 *sip;

    *out_buf = (VOID *)0;
    *out_size = 0;
    *out_pages = 0;
    if (!pxe || !pxe->Mtftp || !cap)
        return EFI_INVALID_PARAMETER;

    /* Server IP: next-server (siaddr) preferred, DHCP server id fallback. */
    server.Addr[0] = server.Addr[1] = server.Addr[2] = server.Addr[3] = 0;
    sip = g_net_dhcp.next_server_ip;
    {
        int have = 0;
        for (i = 0; i < 4; i++) if (sip[i]) have = 1;
        if (!have) sip = g_net_dhcp.dhcp_server_ip;
    }
    for (i = 0; i < 4; i++) sb[i] = sip[i];

    /* Allocate a cap-sized read buffer up front: this buffer IS the first and
     * only transfer boundary against an untrusted server. We deliberately do
     * NOT call GET_FILE_SIZE -- under the PXE Base Code contract it can download
     * the whole object into a bit-bucket when the server lacks the tsize option,
     * letting the server push past the cap before any refusal. cap is a page
     * multiple, so BufferSize == cap exactly. */
    pages = (UINTN)((cap + 0xFFFu) / 0x1000u);
    s = gBS->AllocatePages(AllocateAnyPages, EfiLoaderData, pages, &buf_phys);
    if (EFI_ERROR(s) || !buf_phys)
        return EFI_OUT_OF_RESOURCES;
    buf = (VOID *)(UINTN)buf_phys;

    /* READ_FILE bounded by the cap-sized buffer + retry/backoff on transient
     * errors. EFI_BUFFER_TOO_SMALL means the object exceeds the cap; a server
     * ERROR packet (EFI_TFTP_ERROR) is returned directly, not retried. */
    for (attempt = 0; attempt < NET_TFTP_RETRIES; attempt++) {
        size = (UINT64)pages * 0x1000u;    /* in: capacity (== cap); out: actual */
        block = 512;
        s = pxe->Mtftp(pxe, EFI_PXE_TFTP_READ_FILE, buf, 0,
                       &size, &block, &server, (UINT8 *)filename,
                       (EFI_PXE_BASE_CODE_MTFTP_INFO *)0, 0);
        if (!EFI_ERROR(s)) {
            if (size > cap) {              /* defensive: cannot exceed the buffer */
                gBS->FreePages(buf_phys, pages);
                return EFI_BUFFER_TOO_SMALL;
            }
            *out_buf = buf;
            *out_size = size;
            *out_pages = pages;            /* free exactly this allocated count */
            return EFI_SUCCESS;
        }
        if (s == EFI_BUFFER_TOO_SMALL) {   /* object is larger than the cap */
            gBS->FreePages(buf_phys, pages);
            return EFI_BUFFER_TOO_SMALL;
        }
        if (!net_tftp_retryable(s)) {
            gBS->FreePages(buf_phys, pages);
            return s;                      /* deterministic failure -- caller maps */
        }
        if (gBS->Stall)
            gBS->Stall((UINTN)NET_TFTP_BACKOFF_MS * 1000u * (UINTN)(attempt + 1));
    }
    gBS->FreePages(buf_phys, pages);
    return EFI_TIMEOUT;                     /* transient retries exhausted */
}

/* Network-boot connectivity proof: on a network boot, download boot.conf over TFTP to
 * verify the client works end-to-end, log reachability, then FREE it. The
 * kernel.exe fetch + staging + retention is owned by the network-vs-local boot
 * selection (the consumer): retaining a 64 MiB untrusted payload here with no
 * consumer would pin pre-EBS memory and could starve the local fallback. */
static void net_tftp_probe(EFI_HANDLE dev_handle)
{
    EFI_PXE_BASE_CODE_PROTOCOL *pxe;
    VOID *cbuf = (VOID *)0;
    UINT64 csize = 0;
    UINTN cpages = 0;
    EFI_STATUS s;

    if (!g_net_discovery.booted_from_network || !g_net_dhcp.valid)
        return;
    pxe = net_resolve_boot_pxe(dev_handle);
    if (!pxe) {
        serial_early_print("[NET] TFTP: no boot-NIC PXE protocol; staying local\n");
        return;
    }
    s = net_tftp_download(pxe, "\\EFI\\ImpossibleOS\\boot.conf",
                          NET_TFTP_BOOTCONF_CAP, &cbuf, &csize, &cpages);
    if (EFI_ERROR(s)) {
        serial_early_print("[NET] TFTP: boot.conf probe failed (err 0x");
        serial_early_print_hex16((UINT16)net_tftp_boot_err(s));
        serial_early_print("); staying local\n");
        return;
    }
    serial_early_print("[NET] TFTP: boot.conf reachable (");
    serial_early_print_uint((UINT32)csize);
    serial_early_print(" B); kernel staging owned by network-boot selection\n");
    boot_log_append("[NET] TFTP client verified (boot.conf)\n");
    gBS->FreePages((EFI_PHYSICAL_ADDRESS)(UINTN)cbuf, cpages);  /* no consumer yet */
}

#define NET_HTTP_MAX_REDIRECTS 4
#define NET_HTTP_TOTAL_MS      30000u   /* whole-transfer (all waits) deadline */
#define NET_HTTP_WAIT_US       100000u  /* 100 ms poll granularity */
#define NET_HTTP_MAX_CHUNKS    8192u    /* max body Response calls (tiny-chunk DoS) */

/* No-op notify: EFI_HTTP tokens require an EVT_NOTIFY_SIGNAL event (the HTTP
 * driver signals it on completion); the body is empty -- CheckEvent drives the
 * wait. */
static VOID EFIAPI net_http_notify(EFI_EVENT ev, VOID *ctx)
{
    (void)ev;
    if (ctx)
        *(volatile int *)ctx = 1;   /* token completed */
}

/* Bounded wait for an HTTP token: the EVT_NOTIFY_SIGNAL callback sets *done; we
 * poll it while driving http->Poll() + Stall, drawing from the shared
 * whole-transfer budget (CheckEvent is invalid for NOTIFY_SIGNAL events).
 * Returns EFI_SUCCESS when the token completes, EFI_TIMEOUT on deadline. */
static EFI_STATUS net_http_wait(EFI_HTTP_PROTOCOL *http,
                                volatile int *done, UINTN *budget_us)
{
    for (;;) {
        UINTN step;
        if (*done)
            return EFI_SUCCESS;
        if (http && http->Poll)
            http->Poll(http);          /* drive the network stack to progress */
        if (*done)
            return EFI_SUCCESS;
        if (*budget_us == 0)
            return EFI_TIMEOUT;         /* whole-transfer deadline exhausted */
        step = (*budget_us < NET_HTTP_WAIT_US) ? *budget_us : NET_HTTP_WAIT_US;
        if (gBS->Stall)
            gBS->Stall(step);
        *budget_us -= step;
    }
}

/* ASCII -> CHAR16 URL (caller FreePool). */
static CHAR16 *net_ascii_to_wide(const char *a)
{
    UINTN n = 0, i;
    CHAR16 *w = (CHAR16 *)0;
    while (a[n]) n++;
    if (EFI_ERROR(gBS->AllocatePool(EfiLoaderData, (n + 1) * 2, (VOID **)&w)) || !w)
        return (CHAR16 *)0;
    for (i = 0; i < n; i++)
        w[i] = (CHAR16)(UINT8)a[i];
    w[n] = 0;
    return w;
}

/* True if an HTTP status code is a redirect (3xx with a Location). */
static int net_http_is_redirect(EFI_HTTP_STATUS_CODE c)
{
    return (c >= HTTP_STATUS_300_MULTIPLE_CHOICES &&
            c <= HTTP_STATUS_307_TEMPORARY_REDIRECT) ||
           c == HTTP_STATUS_308_PERMANENT_REDIRECT;
}

/* Find a response header value by (case-insensitive) field name; returns the
 * firmware-owned value pointer (valid until the caller frees Headers), else 0. */
static const char *net_http_header(const EFI_HTTP_HEADER *hdrs, UINTN n,
                                   const char *name)
{
    UINTN i, j;
    for (i = 0; i < n; i++) {
        const CHAR8 *fn = hdrs[i].FieldName;
        if (!fn)
            continue;
        for (j = 0; name[j] && fn[j]; j++) {
            char a = name[j];
            char b = (char)fn[j];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            if (a != b)
                break;
        }
        if (!name[j] && !fn[j])
            return (const char *)hdrs[i].FieldValue;
    }
    return (const char *)0;
}

/* Resolve a (possibly relative) Location against the absolute base URL of the
 * request that produced it, into out[outsz]. Handles three forms:
 *   - absolute ("scheme://...")      -> copied verbatim
 *   - root-relative ("/path")        -> scheme://authority + loc
 *   - path-relative ("p" / "a/b")    -> base up to its last '/', then loc
 * Bounds every write; returns 0 on success, -1 on parse failure or overflow
 * (never truncates). RFC 3986 dot-segment ("../") collapsing is left to the
 * firmware HTTP stack -- asset servers use absolute/root-relative redirects. */
static int net_http_resolve_redirect(const char *base, const char *loc,
                                     char *out, UINTN outsz)
{
    UINTN i, n = 0, sa, p, prefix;
    int has_scheme = 0, found_slash = 0;

    if (!base || !loc || !out || outsz < 2)
        return -1;

    /* Absolute Location ("scheme://...") -> copy verbatim. Stop at the first
     * ':' (scheme delimiter) or '/'; bound every lookahead so a value ending in
     * ':' (e.g. "http:") cannot read past the firmware-owned NUL terminator:
     * loc[i+1] is valid because loc[i]==':' is non-NUL, and loc[i+2] is read
     * only after loc[i+1]=='/' proves another byte exists. */
    for (i = 0; loc[i]; i++) {
        if (loc[i] == '/')            /* path char before any ':' -> relative */
            break;
        if (loc[i] == ':') {
            has_scheme = (loc[i + 1] == '/' && loc[i + 2] == '/');
            break;
        }
    }
    if (has_scheme) {
        for (i = 0; loc[i]; i++) { if (n >= outsz - 1) return -1; out[n++] = loc[i]; }
        out[n] = '\0';
        return 0;
    }

    /* Relative: locate base "scheme://authority" (up to first '/' after "://").
     * Same bounded-lookahead discipline as the loc scan above. */
    p = 0;
    while (base[p]) {
        if (base[p] == ':' && base[p + 1] == '/' && base[p + 2] == '/')
            break;
        p++;
    }
    if (!base[p]) return -1;          /* base must itself be absolute */
    p += 3;
    while (base[p] && base[p] != '/') p++;
    sa = p;                            /* base[0..sa) == scheme://authority */

    if (loc[0] == '/') {               /* root-relative */
        for (i = 0; i < sa; i++) { if (n >= outsz - 1) return -1; out[n++] = base[i]; }
        for (i = 0; loc[i]; i++) { if (n >= outsz - 1) return -1; out[n++] = loc[i]; }
        out[n] = '\0';
        return 0;
    }

    /* path-relative: base up to and including its last '/', then loc. */
    prefix = sa;
    for (p = sa; base[p]; p++) if (base[p] == '/') { prefix = p + 1; found_slash = 1; }
    for (i = 0; i < prefix; i++) { if (n >= outsz - 1) return -1; out[n++] = base[i]; }
    if (!found_slash) { if (n >= outsz - 1) return -1; out[n++] = '/'; }
    for (i = 0; loc[i]; i++) { if (n >= outsz - 1) return -1; out[n++] = loc[i]; }
    out[n] = '\0';
    return 0;
}

/* Download `url` over HTTP via EFI_HTTP_PROTOCOL on the firmware HTTP Boot
 * service-binding handle, into a freshly allocated cap-sized EfiLoaderData
 * buffer (the buffer is the transfer bound; Content-Length is not trusted).
 * Follows up to NET_HTTP_MAX_REDIRECTS 3xx Location hops. On success returns
 * EFI_SUCCESS + sets *out_buf (caller FreePages) + *out_size + *out_pages.
 * Returns a typed status on failure -- caller decides fallback (no boot_fatal
 * here). */
static EFI_STATUS net_http_download(EFI_HANDLE dev_handle, const char *url,
                                    UINT64 cap, VOID **out_buf,
                                    UINT64 *out_size, UINTN *out_pages)
{
    EFI_GUID sb_guid  = EFI_HTTP_SERVICE_BINDING_PROTOCOL_GUID;
    EFI_GUID http_guid = EFI_HTTP_PROTOCOL_GUID;
    EFI_SERVICE_BINDING_PROTOCOL *sb = (EFI_SERVICE_BINDING_PROTOCOL *)0;
    EFI_HTTP_PROTOCOL *http = (EFI_HTTP_PROTOCOL *)0;
    EFI_HANDLE child = (EFI_HANDLE)0;
    EFI_HTTPv4_ACCESS_POINT ap;
    EFI_HTTP_CONFIG_DATA cfg;
    EFI_EVENT req_ev = (EFI_EVENT)0, resp_ev = (EFI_EVENT)0;
    EFI_PHYSICAL_ADDRESS buf_phys = 0;
    VOID *buf = (VOID *)0;
    UINTN pages;
    CHAR16 *wurl = (CHAR16 *)0;
    EFI_STATUS s;
    int redirects = 0;
    char redir_buf[1024];
    char redir_buf2[1024];   /* resolve target before aliasing redir_buf */
    UINTN budget = NET_HTTP_TOTAL_MS * 1000u;   /* shared across all waits */
    volatile int req_done = 0, resp_done = 0;   /* set by net_http_notify */

    *out_buf = (VOID *)0;
    *out_size = 0;
    *out_pages = 0;
    if (!dev_handle || !url || !cap)
        return EFI_INVALID_PARAMETER;

    /* The firmware HTTP Boot installs the HTTP service binding on the boot NIC
     * handle; without it we cannot fetch (caller stays local). */
    if (EFI_ERROR(gBS->HandleProtocol(dev_handle, &sb_guid, (VOID **)&sb)) || !sb)
        return EFI_UNSUPPORTED;
    if (EFI_ERROR(sb->CreateChild(sb, &child)) || !child)
        return EFI_OUT_OF_RESOURCES;
    if (EFI_ERROR(gBS->HandleProtocol(child, &http_guid, (VOID **)&http)) || !http) {
        sb->DestroyChild(sb, child);
        return EFI_UNSUPPORTED;
    }

    /* Configure HTTPv4 using the firmware-assigned address (HTTP Boot already
     * ran DHCP). */
    ap.UseDefaultAddress = 1;
    ap.LocalAddress.Addr[0] = ap.LocalAddress.Addr[1] = 0;
    ap.LocalAddress.Addr[2] = ap.LocalAddress.Addr[3] = 0;
    ap.LocalSubnet.Addr[0] = ap.LocalSubnet.Addr[1] = 0;
    ap.LocalSubnet.Addr[2] = ap.LocalSubnet.Addr[3] = 0;
    ap.LocalPort = 0;
    cfg.HttpVersion = HttpVersion11;
    cfg.TimeOutMillisec = NET_HTTP_TOTAL_MS;
    cfg.LocalAddressIsIPv6 = 0;
    cfg.AccessPoint.IPv4Node = &ap;
    s = http->Configure(http, &cfg);
    if (EFI_ERROR(s))
        goto cleanup;

    /* Allocate the cap-sized body buffer: it is the transfer bound. */
    pages = (UINTN)((cap + 0xFFFu) / 0x1000u);
    if (EFI_ERROR(gBS->AllocatePages(AllocateAnyPages, EfiLoaderData, pages,
                                     &buf_phys)) || !buf_phys) {
        s = EFI_OUT_OF_RESOURCES;
        goto cleanup;
    }
    buf = (VOID *)(UINTN)buf_phys;

    if (gBS->CreateEvent(EVT_NOTIFY_SIGNAL, TPL_CALLBACK, net_http_notify,
                         (VOID *)&req_done, &req_ev) != EFI_SUCCESS ||
        gBS->CreateEvent(EVT_NOTIFY_SIGNAL, TPL_CALLBACK, net_http_notify,
                         (VOID *)&resp_done, &resp_ev) != EFI_SUCCESS) {
        s = EFI_OUT_OF_RESOURCES;
        goto cleanup;
    }

    for (;;) {
        EFI_HTTP_REQUEST_DATA reqd;
        EFI_HTTP_HEADER req_hdr;
        EFI_HTTP_MESSAGE reqm;
        EFI_HTTP_TOKEN reqt;
        EFI_HTTP_RESPONSE_DATA respd;
        EFI_HTTP_MESSAGE respm;
        EFI_HTTP_TOKEN respt;
        UINT64 total = 0;
        const char *loc;

        wurl = net_ascii_to_wide(url);
        if (!wurl) { s = EFI_OUT_OF_RESOURCES; goto cleanup; }

        /* GET request (Host header omitted: the firmware HTTP stack derives it
         * from the absolute URL). */
        reqd.Method = HttpMethodGet;
        reqd.Url = wurl;
        req_hdr.FieldName = (CHAR8 *)"Accept";
        req_hdr.FieldValue = (CHAR8 *)"*/*";
        reqm.Data.Request = &reqd;
        reqm.HeaderCount = 1;
        reqm.Headers = &req_hdr;
        reqm.BodyLength = 0;
        reqm.Body = (VOID *)0;
        reqt.Event = req_ev;
        reqt.Status = EFI_SUCCESS;
        reqt.Message = &reqm;
        req_done = 0;
        s = http->Request(http, &reqt);
        if (EFI_ERROR(s)) goto free_wurl;
        s = net_http_wait(http, &req_done, &budget);
        if (EFI_ERROR(s) || EFI_ERROR(reqt.Status)) {
            /* On timeout the request is still in flight referencing wurl -- abort
             * it (synchronous) before freeing, or the firmware could write to
             * freed memory on a late async completion. */
            http->Cancel(http, &reqt);
            s = EFI_ERROR(s) ? s : reqt.Status;
            goto free_wurl;
        }

        /* First Response: status line + headers + first body chunk. */
        respd.StatusCode = HTTP_STATUS_UNSUPPORTED_STATUS;
        respm.Data.Response = &respd;
        respm.HeaderCount = 0;
        respm.Headers = (EFI_HTTP_HEADER *)0;
        respm.BodyLength = (UINTN)cap;
        respm.Body = buf;
        respt.Event = resp_ev;
        respt.Status = EFI_SUCCESS;
        respt.Message = &respm;
        resp_done = 0;
        s = http->Response(http, &respt);
        if (EFI_ERROR(s)) goto free_wurl;
        s = net_http_wait(http, &resp_done, &budget);
        if (EFI_ERROR(s) || EFI_ERROR(respt.Status)) {
            http->Cancel(http, &respt);   /* abort in-flight before freeing buf */
            s = EFI_ERROR(s) ? s : respt.Status;
            if (respm.Headers) gBS->FreePool(respm.Headers);
            goto free_wurl;
        }

        if (net_http_is_redirect(respd.StatusCode)) {
            loc = net_http_header(respm.Headers, respm.HeaderCount, "Location");
            if (loc && ++redirects <= NET_HTTP_MAX_REDIRECTS) {
                /* Resolve Location (absolute / root-relative / path-relative)
                 * against the current absolute URL BEFORE freeing Headers --
                 * loc points into the firmware-owned header block. Resolve into
                 * redir_buf2 (distinct from base==url, which may be redir_buf),
                 * then copy into the stable redir_buf. */
                int rr = net_http_resolve_redirect(url, loc, redir_buf2,
                                                   sizeof(redir_buf2));
                if (respm.Headers) gBS->FreePool(respm.Headers);
                if (rr != 0) { s = EFI_NO_MAPPING; goto free_wurl; }
                {
                    UINTN k = 0;
                    while (redir_buf2[k] && k < sizeof(redir_buf) - 1) {
                        redir_buf[k] = redir_buf2[k]; k++;
                    }
                    redir_buf[k] = '\0';
                }
                url = redir_buf;
                gBS->FreePool(wurl); wurl = (CHAR16 *)0;
                continue;   /* re-issue GET against the redirect target */
            }
            if (respm.Headers) gBS->FreePool(respm.Headers);
            s = EFI_NO_MAPPING;   /* too many redirects / no Location */
            goto free_wurl;
        }

        if (respd.StatusCode != HTTP_STATUS_200_OK) {
            if (respm.Headers) gBS->FreePool(respm.Headers);
            s = EFI_NOT_FOUND;
            goto free_wurl;
        }
        if (respm.Headers) gBS->FreePool(respm.Headers);
        if (respm.BodyLength > cap) {   /* firmware overran the buffer */
            s = EFI_BUFFER_TOO_SMALL;
            goto free_wurl;
        }
        total = respm.BodyLength;   /* first chunk already in buf */

        /* Read remaining body chunks until a proven EOF (0-length or NOT_FOUND).
         * The cap-sized buffer is the bound: if it fills before EOF the body is
         * over-cap (a 1-byte scratch read distinguishes exact-cap EOF). Bounded
         * by NET_HTTP_MAX_CHUNKS (tiny-chunk DoS) and the shared time budget. */
        {
            int eof = 0;
            UINTN chunks = 0;
            UINT8 scratch;
            while (!eof) {
                if (total >= cap) {
                    /* Buffer full -- scratch-read one byte: 0/NOT_FOUND => the
                     * body was exactly cap (EOF); any byte => over-cap. */
                    respm.Data.Response = (EFI_HTTP_RESPONSE_DATA *)0;
                    respm.HeaderCount = 0;
                    respm.Headers = (EFI_HTTP_HEADER *)0;
                    respm.BodyLength = 1;
                    respm.Body = &scratch;
                    respt.Status = EFI_SUCCESS;
                    resp_done = 0;
                    s = http->Response(http, &respt);
                    if (s == EFI_NOT_FOUND) { eof = 1; break; }
                    if (EFI_ERROR(s)) goto free_wurl;
                    s = net_http_wait(http, &resp_done, &budget);
                    if (EFI_ERROR(s)) { http->Cancel(http, &respt); goto free_wurl; }
                    if (respm.BodyLength == 0) { eof = 1; break; }
                    s = EFI_BUFFER_TOO_SMALL;   /* over-cap */
                    goto free_wurl;
                }
                if (++chunks > NET_HTTP_MAX_CHUNKS) {
                    s = EFI_TIMEOUT;            /* too many tiny chunks */
                    goto free_wurl;
                }
                respm.Data.Response = (EFI_HTTP_RESPONSE_DATA *)0;
                respm.HeaderCount = 0;
                respm.Headers = (EFI_HTTP_HEADER *)0;
                respm.BodyLength = (UINTN)(cap - total);
                respm.Body = (UINT8 *)buf + total;
                respt.Status = EFI_SUCCESS;
                resp_done = 0;
                s = http->Response(http, &respt);
                if (EFI_ERROR(s)) {
                    if (s == EFI_NOT_FOUND) { eof = 1; break; }   /* body done */
                    goto free_wurl;
                }
                s = net_http_wait(http, &resp_done, &budget);
                if (EFI_ERROR(s)) {
                    http->Cancel(http, &respt);  /* abort in-flight body read */
                    goto free_wurl;
                }
                if (respm.BodyLength == 0) { eof = 1; break; }
                if (respm.BodyLength > cap - total) {   /* firmware overran */
                    s = EFI_BUFFER_TOO_SMALL;
                    goto free_wurl;
                }
                total += respm.BodyLength;
            }
        }
        gBS->FreePool(wurl); wurl = (CHAR16 *)0;
        *out_buf = buf;
        *out_size = total;
        *out_pages = pages;
        buf = (VOID *)0;   /* ownership transferred */
        s = EFI_SUCCESS;
        goto cleanup;

    free_wurl:
        if (wurl) { gBS->FreePool(wurl); wurl = (CHAR16 *)0; }
        goto cleanup;
    }

cleanup:
    if (wurl) gBS->FreePool(wurl);
    if (req_ev) gBS->CloseEvent(req_ev);
    if (resp_ev) gBS->CloseEvent(resp_ev);
    if (buf) gBS->FreePages(buf_phys, pages);
    if (http) http->Configure(http, (EFI_HTTP_CONFIG_DATA *)0);  /* reset */
    if (sb && child) sb->DestroyChild(sb, child);
    return s;
}

/* Connectivity proof: on an HTTP boot, fetch boot.conf over HTTP to verify
 * the client end-to-end, log it, then FREE it. kernel fetch/stage/retention is by
 * the network-boot selection, final-URL persistence by the boot_info handoff
 * -- no retention here (the TFTP-probe lesson). */
static void net_http_probe(EFI_HANDLE dev_handle)
{
    VOID *cbuf = (VOID *)0;
    UINT64 csize = 0;
    UINTN cpages = 0;
    EFI_STATUS s;

    if (!g_net_discovery.booted_from_network || !g_net_discovery.http_boot)
        return;
    s = net_http_download(dev_handle, "http://boot/EFI/ImpossibleOS/boot.conf",
                          NET_TFTP_BOOTCONF_CAP, &cbuf, &csize, &cpages);
    if (EFI_ERROR(s)) {
        serial_early_print("[NET] HTTP: boot.conf probe failed; staying local\n");
        return;
    }
    serial_early_print("[NET] HTTP: boot.conf reachable (");
    serial_early_print_uint((UINT32)csize);
    serial_early_print(" B); kernel staging owned by network-boot selection\n");
    boot_log_append("[NET] HTTP client verified (boot.conf)\n");
    gBS->FreePages((EFI_PHYSICAL_ADDRESS)(UINTN)cbuf, cpages);
}

/* === BOOTX64.EFI on-disk self-measurement ================================
 * WHAT IS MEASURED, AND WHAT THAT CLAIM IS WORTH. This hashes the ESP FILE the
 * running loader was launched from, reopened through the loaded-image
 * DeviceHandle + FilePath. It is NOT proof of the bytes the firmware executed:
 * the file can be replaced between LoadImage and this reopen, and a volume can
 * be remounted so the same path resolves elsewhere. Binding a digest to the
 * EXECUTED image needs the firmware measurement (PCR 4, EV_EFI_BOOT_SERVICES_
 * APPLICATION), which is separate work; every name here says FILE for that
 * reason, and a consumer must not upgrade the claim.
 *
 * WHY NOT THE RESIDENT IMAGE RANGE. EFI_LOADED_IMAGE_PROTOCOL hands back a
 * RELOCATED ImageBase whose relocations are already applied and whose data
 * sections mutate as the loader runs, so hashing it yields a different digest
 * for the same binary on every boot and destroys the attribution entirely.
 *
 * FAILURE IS ABSENCE, NEVER A SUBSTITUTE. Every path that cannot produce the
 * real digest reports ABSENT with a distinct status and boots on. A loader that
 * refused to boot because it could not measure itself would convert an
 * observability feature into a brick, and a fallback digest (resident range, a
 * partial read) would be worse than absence: it looks exactly like the real
 * answer. */

/* STAYS 8 MiB. Since the whole file is now held in one pool allocation (the
 * Authenticode hash needs the section table before it knows which bytes to hash,
 * so a single forward pass is not available) this ceiling is also the worst-case
 * peak pre-EBS allocation, and it was briefly lowered to 2 MiB on exactly that
 * reasoning. Reverted: section 20 shipped, verified and documented an 8 MiB
 * bound, so narrowing it here would silently refuse a loader that grew into the
 * 2-8 MiB range the contract still promises -- publishing no digest at all and
 * failing the smoke oracle, for a machine doing nothing wrong. The peak
 * allocation is a real concern, and it is filed as an item rather than paid for
 * with a contract nobody was told had changed. */
#define SELF_MEASURE_SIZE_CAP   (8u * 1024u * 1024u) /* the shipped image is ~350 KB */
#define SELF_MEASURE_CHUNK      (64u * 1024u)
/* 256 CHAR16 covers every ESP path a loader is launched from with room to spare
 * (`\EFI\BOOT\BOOTX64.EFI` is 21). It is a hard bound, not a truncation point:
 * dpfp_extract returns DPFP_OVERFLOW and the measurement reports ABSENT, per the
 * parse-buffer rule that a silently shortened path is worse than no path. */
#define SELF_MEASURE_PATH_CHARS 256u
#define SELF_MEASURE_DP_CAP     8192u   /* same bound net_dp_validated_size uses */

/* The pure parser spells its node constants out (it also compiles in kernel test
 * context, where efi.h must not be included). Pin them against efi.h here, which
 * is the one place both are visible. */
_Static_assert(DPFP_TYPE_MEDIA == EFI_DP_TYPE_MEDIA,
    "devpath_filepath MEDIA type must match efi.h");
_Static_assert(DPFP_TYPE_END == EFI_DP_TYPE_END,
    "devpath_filepath END type must match efi.h");
_Static_assert(DPFP_SUBTYPE_END_ENTIRE == EFI_DP_SUBTYPE_END_ENTIRE,
    "devpath_filepath END_ENTIRE subtype must match efi.h");
_Static_assert(DPFP_SUBTYPE_END_INSTANCE == EFI_DP_SUBTYPE_END_INSTANCE,
    "devpath_filepath END_INSTANCE subtype must match efi.h");
_Static_assert(DPFP_SUBTYPE_FILEPATH == EFI_DP_MEDIA_FILEPATH,
    "devpath_filepath FILEPATH subtype must match efi.h");

enum self_measure_status {
    /* Zero is NOT success. The record is a zero-initialized global, and a boot
     * path that never reaches the measurement would otherwise present as
     * status=ok with present=0 -- two fields disagreeing, which is exactly the
     * contradiction a consumer reading one of them would resolve wrongly. */
    SELF_MEASURE_NOT_RUN = 0,
    SELF_MEASURE_OK,
    SELF_MEASURE_NO_DEVICE,     /* NULL DeviceHandle (pure HTTP/PXE boot) */
    SELF_MEASURE_NO_PATH,       /* FilePath absent, unmeasurable or not a file path */
    SELF_MEASURE_NO_FS,         /* no SimpleFS on the boot device */
    SELF_MEASURE_OPEN_FAILED,
    SELF_MEASURE_INFO_FAILED,
    SELF_MEASURE_EMPTY,         /* zero-length file: nothing honest to hash */
    SELF_MEASURE_TOO_LARGE,     /* above SELF_MEASURE_SIZE_CAP */
    SELF_MEASURE_NO_MEMORY,
    SELF_MEASURE_READ_FAILED,
    SELF_MEASURE_SHORT_READ,    /* file ended before FileSize bytes were read */
    SELF_MEASURE_TRAILING_BYTES,/* data remained after the declared FileSize */
    SELF_MEASURE_INFO_MALFORMED,/* GetInfo succeeded into a buffer too small to hold FileSize */
    SELF_MEASURE_SELFTEST_FAILED, /* the hash itself is wrong; publish nothing */
};

/* Distinct from every enum peac_status value (which start at PEAC_OK == 0), so
 * "never computed" can never be mistaken for "computed successfully". */
#define SELF_MEASURE_PE_NOT_RUN 0xFFu

/* Published for the kernel handoff. The boot_info carriage is NOT wired yet:
 * adding a boot_info field requires an F() row in
 * tools/boot-info-manifest/dump-fields.inc (its completeness check refuses a
 * field without one) and that file is operator-only machinery. Until then this
 * record is the loader-side source of truth and the serial line below is how it
 * leaves the machine. */
/* WHAT THIS DIGEST CLAIMS, AND WHAT NO CONSUMER MAY UPGRADE IT TO.
 *
 * These are the bytes of the ESP FILE reached through the loaded-image
 * DeviceHandle + FilePath. They are NOT the bytes firmware executed. Firmware
 * measured the executed image itself during LoadImage, as an
 * EV_EFI_BOOT_SERVICES_APPLICATION event into PCR 4 (TCG PC Client Platform
 * Firmware Profile), and that measurement is an Authenticode PE hash over the
 * loaded image rather than a flat file hash -- a different value over different
 * bytes, so the two are not interchangeable and cannot be compared without a PE
 * hasher.
 *
 * The gap is real, not theoretical: the file can be replaced between LoadImage
 * and this reopen, and a remount can resolve the same path elsewhere. Either
 * yields two internally consistent digests with nothing to notice the swap.
 * Correlating this value with the firmware event is a tracked capability of its
 * own; until it lands, this record is evidence about a FILE on the ESP, and
 * every field name and serial token says so. */
static struct {
    UINT8  digest[SHA256B_DIGEST_LEN];  /* SHA-256 over the ESP FILE bytes */
    UINT8  present;                 /* 1 = digest holds a real measurement */
    UINT8  status;                  /* enum self_measure_status; 0 = never ran */
    UINT64 measured_bytes;
    UINT64 tsc_delta;
    /* The Authenticode PE hash of the SAME snapshot, which is the only digest
     * firmware's PCR 4 measurement can ever equal. Kept beside the flat digest
     * rather than replacing it: they answer different questions, and section 20
     * pinned the flat one as evidence about an ESP FILE. Both are derived from
     * ONE read of the file -- two reads would let the two digests describe two
     * different snapshots of a file an attacker is free to rewrite between
     * them, which is the substitution this correlation exists to catch. */
    UINT8  pe_digest[SHA256B_DIGEST_LEN];
    UINT8  pe_present;              /* 1 = pe_digest holds a real measurement */
    /* enum peac_status once the hash has RUN, otherwise SELF_MEASURE_PE_NOT_RUN.
     * PEAC_OK is 0, so a record that never ran would otherwise report `ok`
     * beside an absent digest -- two fields disagreeing, which is the same trap
     * SELF_MEASURE_NOT_RUN exists to close for the flat status above. */
    UINT8  pe_status;
} g_self_measure;

/* peac_status_name() plus the not-run sentinel, so every value the record can
 * hold has a name and none of them reads as a success it is not. */
static const char *self_measure_pe_status_name(UINT8 st)
{
    if (st == SELF_MEASURE_PE_NOT_RUN)
        return "not-run";
    return peac_status_name((int)st);
}

static const char *self_measure_status_name(UINT8 st)
{
    switch (st) {
    case SELF_MEASURE_NOT_RUN:         return "not-run";
    case SELF_MEASURE_OK:              return "ok";
    case SELF_MEASURE_NO_DEVICE:       return "no-device-handle";
    case SELF_MEASURE_NO_PATH:         return "no-file-path";
    case SELF_MEASURE_NO_FS:           return "no-filesystem";
    case SELF_MEASURE_OPEN_FAILED:     return "open-failed";
    case SELF_MEASURE_INFO_FAILED:     return "getinfo-failed";
    case SELF_MEASURE_EMPTY:           return "empty-file";
    case SELF_MEASURE_TOO_LARGE:       return "over-size-cap";
    case SELF_MEASURE_NO_MEMORY:       return "alloc-failed";
    case SELF_MEASURE_READ_FAILED:     return "read-failed";
    case SELF_MEASURE_SHORT_READ:      return "short-read";
    case SELF_MEASURE_TRAILING_BYTES:  return "trailing-bytes";
    case SELF_MEASURE_INFO_MALFORMED:  return "getinfo-malformed";
    case SELF_MEASURE_SELFTEST_FAILED: return "sha256-selftest-failed";
    default:                           return "unknown";
    }
}

/* Resolve the ESP-relative path of the running loader. The device path is
 * firmware-owned, so its length comes from GetDevicePathSize and never from a
 * self-walk (bootx64.c net_dp_validated_size records why); a missing utilities
 * protocol or a degenerate measure means "do not trust this path" and the
 * measurement reports ABSENT rather than parsing heuristically. */
static enum self_measure_status self_measure_resolve_path(CHAR16 *out, UINTN out_chars)
{
    EFI_GUID dpu_guid = EFI_DEVICE_PATH_UTILITIES_PROTOCOL_GUID;
    EFI_DEVICE_PATH_UTILITIES_PROTOCOL *dpu = (EFI_DEVICE_PATH_UTILITIES_PROTOCOL *)0;
    EFI_STATUS status;
    UINTN dp_size;

    if (!g_boot_image_file_path)
        return SELF_MEASURE_NO_PATH;

    status = gBS->LocateProtocol(&dpu_guid, (VOID *)0, (VOID **)&dpu);
    if (EFI_ERROR(status) || !dpu || !dpu->GetDevicePathSize)
        return SELF_MEASURE_NO_PATH;

    dp_size = dpu->GetDevicePathSize(g_boot_image_file_path);
    if (dp_size < 4u || dp_size > SELF_MEASURE_DP_CAP)
        return SELF_MEASURE_NO_PATH;

    if (dpfp_extract(g_boot_image_file_path, (__SIZE_TYPE__)dp_size,
                     out, (__SIZE_TYPE__)out_chars) != DPFP_OK)
        return SELF_MEASURE_NO_PATH;

    return SELF_MEASURE_OK;
}

/* Hash the loader file in one forward pass over bounded chunks. `file` is
 * already open and positioned at 0. */
/* Read the whole file into `buf[0 .. file_size)` -- ONE snapshot, from which
 * every digest of this image is then derived. This used to hash as it streamed;
 * it fills a buffer instead because the Authenticode hash needs the section
 * table before it knows which bytes to hash in which order, and because two
 * passes over a file are two SNAPSHOTS: the flat digest and the PE digest could
 * describe different bytes, which is precisely the substitution the PCR 4
 * correlation exists to detect. Every refusal below is unchanged from the
 * streaming version -- they are what make a published digest mean the whole
 * file. */
static enum self_measure_status self_measure_read_file(EFI_FILE_PROTOCOL *file,
                                                       UINT64 file_size,
                                                       UINT8 *buf,
                                                       UINT64 *out_bytes)
{
    EFI_STATUS status;
    UINT64 remaining = file_size;
    UINT64 done = 0;

    if (!buf)
        return SELF_MEASURE_NO_MEMORY;

    while (remaining > 0u) {
        UINTN want = (remaining < (UINT64)SELF_MEASURE_CHUNK)
                   ? (UINTN)remaining : (UINTN)SELF_MEASURE_CHUNK;
        UINTN got = want;

        /* EXACT EFI_SUCCESS, not !EFI_ERROR: EFI_ERROR tests the high error bit
         * only, so a positive WARNING status would pass it and let degraded
         * firmware contribute bytes to a digest published as trustworthy. Every
         * non-success here is a refusal instead. */
        status = file->Read(file, &got, buf + done);
        if (status != EFI_SUCCESS)
            return SELF_MEASURE_READ_FAILED;
        /* UEFI 2.10 spec 13.5.2 says Read never returns more than requested, but
         * `got` is firmware-written and this is a bare-metal target: taking it on
         * trust would write past the allocation and underflow `remaining`.
         * A firmware that violates the contract gets a refusal, not a read. */
        if (got > want)
            return SELF_MEASURE_READ_FAILED;
        if (got == 0u) {
            /* FileSize said there were more bytes. Hashing what we got would
             * publish a prefix digest that is indistinguishable from the real
             * one, so this is a refusal. */
            return SELF_MEASURE_SHORT_READ;
        }
        done      += (UINT64)got;
        remaining -= (UINT64)got;
    }
    /* FileSize is metadata, not proof of EOF. A stale or hostile under-report
     * would let the loop finish "successfully" over a PREFIX of the real file,
     * and a prefix digest is indistinguishable from the whole-file one. One
     * bounded probe past the declared end settles it: any byte still readable
     * there means the declared length was not the file. */
    {
        UINTN probe_len = 1u;
        UINT8 probe_byte = 0u;

        status = file->Read(file, &probe_len, &probe_byte);
        if (status != EFI_SUCCESS) {
            /* The probe is the ONLY evidence that FileSize was not
             * under-reported, so a failed probe leaves EOF UNVERIFIED. Reading
             * that as EOF would publish a possible prefix digest as complete,
             * which is the single outcome this whole path exists to prevent. */
            return SELF_MEASURE_READ_FAILED;
        }
        if (probe_len != 0u)
            return SELF_MEASURE_TRAILING_BYTES;
    }

    *out_bytes = file_size;
    return SELF_MEASURE_OK;
}

/* Measure the running loader and record the result in g_self_measure. Must run
 * pre-ExitBootServices: every call below is a Boot Service or a protocol. */
static void self_measure_run(void)
{
    EFI_GUID fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_GUID file_info_guid = EFI_FILE_INFO_ID;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = (EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *)0;
    EFI_FILE_PROTOCOL *root_dir = (EFI_FILE_PROTOCOL *)0;
    EFI_FILE_PROTOCOL *file = (EFI_FILE_PROTOCOL *)0;
    CHAR16 path[SELF_MEASURE_PATH_CHARS];
    enum self_measure_status st;
    EFI_STATUS status;
    UINT64 file_size = 0;
    UINT64 tsc_start;
    UINTN i;

    post_code16(POST16_BL_SELF_MEASURE);
    tsc_start = boot_rdtsc();
    g_self_measure.present = 0;
    g_self_measure.measured_bytes = 0;
    g_self_measure.tsc_delta = 0;
    for (i = 0; i < SHA256B_DIGEST_LEN; i++)
        g_self_measure.digest[i] = 0;
    /* The PE fields need the SAME treatment, and for a reason specific to this
     * loader: .bss is NOT zeroed here -- firmware pool-poisons it with 0xAF
     * (bootx64.c:8425). Leaving pe_present unwritten therefore does not mean
     * "false", it means 0xAF, and a boot that failed before computing the
     * digest would print status=ok over uninitialized bytes and sail past the
     * no-local-digest guard into a fabricated DISAGREE. Clearing an absence is
     * not defensive here; it is the difference between an honest absence and
     * an invented measurement. */
    g_self_measure.pe_present = 0;
    g_self_measure.pe_status = (UINT8)SELF_MEASURE_PE_NOT_RUN;
    for (i = 0; i < SHA256B_DIGEST_LEN; i++)
        g_self_measure.pe_digest[i] = 0;

    /* The hash itself is verified before anything it produces is believed. */
    if (!sha256b_selftest()) {
        g_self_measure.status = (UINT8)SELF_MEASURE_SELFTEST_FAILED;
        goto report;
    }
    if (!g_boot_device_handle) {
        g_self_measure.status = (UINT8)SELF_MEASURE_NO_DEVICE;
        goto report;
    }

    st = self_measure_resolve_path(path, (UINTN)SELF_MEASURE_PATH_CHARS);
    if (st != SELF_MEASURE_OK) {
        g_self_measure.status = (UINT8)st;
        goto report;
    }

    status = gBS->HandleProtocol(g_boot_device_handle, &fs_guid, (VOID **)&fs);
    if (EFI_ERROR(status) || !fs) {
        g_self_measure.status = (UINT8)SELF_MEASURE_NO_FS;
        goto report;
    }
    status = fs->OpenVolume(fs, &root_dir);
    if (EFI_ERROR(status) || !root_dir) {
        g_self_measure.status = (UINT8)SELF_MEASURE_NO_FS;
        goto report;
    }
    status = root_dir->Open(root_dir, &file, path, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status) || !file) {
        root_dir->Close(root_dir);
        g_self_measure.status = (UINT8)SELF_MEASURE_OPEN_FAILED;
        goto report;
    }

    {
        /* FileSize lives at offset 8, so the buffer must be able to HOLD it
         * before it is read. Both sizes here are firmware-written: the probe
         * result decides the allocation and the second call may lower it. A
         * provider returning success into a 4-byte buffer would otherwise have
         * its neighbouring pool bytes read as a file size, and a plausible one
         * under the 8 MiB cap would drive a false successful measurement. */
        const UINTN info_min = (UINTN)__builtin_offsetof(EFI_FILE_INFO, FileSize)
                             + (UINTN)sizeof(UINT64);
        UINTN info_cap = 0;
        UINTN info_size = 0;
        VOID *info_buf = (VOID *)0;

        status = file->GetInfo(file, &file_info_guid, &info_size, (VOID *)0);
        if (status != EFI_BUFFER_TOO_SMALL || info_size < info_min) {
            g_self_measure.status = (UINT8)SELF_MEASURE_INFO_FAILED;
            goto close_and_report;
        }
        info_cap = info_size;
        status = gBS->AllocatePool(EfiLoaderData, info_cap, &info_buf);
        if (EFI_ERROR(status) || !info_buf) {
            g_self_measure.status = (UINT8)SELF_MEASURE_NO_MEMORY;
            goto close_and_report;
        }
        status = file->GetInfo(file, &file_info_guid, &info_size, info_buf);
        if (EFI_ERROR(status)) {
            gBS->FreePool(info_buf);
            g_self_measure.status = (UINT8)SELF_MEASURE_INFO_FAILED;
            goto close_and_report;
        }
        if (info_size < info_min || info_size > info_cap) {
            gBS->FreePool(info_buf);
            g_self_measure.status = (UINT8)SELF_MEASURE_INFO_MALFORMED;
            goto close_and_report;
        }
        file_size = ((EFI_FILE_INFO *)info_buf)->FileSize;
        gBS->FreePool(info_buf);
    }

    if (file_size == 0u) {
        g_self_measure.status = (UINT8)SELF_MEASURE_EMPTY;
        goto close_and_report;
    }
    /* The cap is a bound on boot latency, not a correctness check: an image
     * this large is not one we built. Refusing beats silently hashing a prefix
     * or lengthening every boot as the binary grows. */
    if (file_size > (UINT64)SELF_MEASURE_SIZE_CAP) {
        g_self_measure.status = (UINT8)SELF_MEASURE_TOO_LARGE;
        goto close_and_report;
    }

    {
        /* Exactly FileSize, under the sanity cap enforced above -- a dynamic
         * allocation that hard-fails rather than a fixed buffer that truncates.
         * Freed before ExitBootServices; nothing here outlives the loader. */
        VOID *snap = (VOID *)0;

        status = gBS->AllocatePool(EfiLoaderData, (UINTN)file_size, &snap);
        if (EFI_ERROR(status) || !snap) {
            g_self_measure.status = (UINT8)SELF_MEASURE_NO_MEMORY;
            goto close_and_report;
        }

        st = self_measure_read_file(file, file_size, (UINT8 *)snap,
                                    &g_self_measure.measured_bytes);
        g_self_measure.status = (UINT8)st;
        if (st == SELF_MEASURE_OK) {
            int pst;

            /* Both digests, one snapshot. The flat hash stays byte-identical to
             * what section 20 published -- same bytes, same algorithm -- so the
             * smoke test's independent cross-check still binds. */
            sha256b((const void *)snap, file_size, g_self_measure.digest);
            g_self_measure.present = 1;

            pst = peac_hash((const unsigned char *)snap,
                            (unsigned long long)file_size,
                            g_self_measure.pe_digest);
            g_self_measure.pe_status = (UINT8)pst;
            if (pst == PEAC_OK)
                g_self_measure.pe_present = 1;
        }
        gBS->FreePool(snap);
    }

close_and_report:
    if (file)
        file->Close(file);
    if (root_dir)
        root_dir->Close(root_dir);

report:
    g_self_measure.tsc_delta = boot_rdtsc() - tsc_start;

    /* One greppable line, pre-ExitBootServices. scripts/test-smoke.sh compares
     * the digest and byte count against an independently computed hash of the
     * staged BOOTX64.EFI, which is what turns this from a determinism check
     * into proof that the right file was hashed in full. */
    serial_early_print("[BOOT] self-measure: status=");
    serial_early_print(self_measure_status_name(g_self_measure.status));
    if (g_self_measure.present) {
        serial_early_print(" bytes=");
        serial_early_print_uint((UINT32)g_self_measure.measured_bytes);
        serial_early_print(" tsc=");
        serial_early_print_hex64(g_self_measure.tsc_delta);
        /* The token NAMES the subject: esp-file-sha256, never a bare sha256.
         * A bare one invites a reader (or a later parser) to treat it as the
         * executed image's digest, which it is not. */
        serial_early_print(" esp-file-sha256=");
        for (i = 0; i < SHA256B_DIGEST_LEN; i += 2u) {
            serial_early_print_hex16((UINT16)(((UINT16)g_self_measure.digest[i] << 8)
                                            | (UINT16)g_self_measure.digest[i + 1u]));
        }
        serial_early_print("\n");
        post_code16(POST16_BL_SELF_MEASURE_OK);

        /* A SEPARATE line, deliberately. Section 20's line is pinned by an
         * ANCHORED schema in scripts/test-smoke.sh, and that anchor is what
         * makes the smoke test's independent cross-check bind; appending a
         * token to it would have to loosen the anchor, trading a real assertion
         * for a cosmetic saving. The two digests also answer different
         * questions -- one about an ESP file, one about what firmware can have
         * executed -- so they read better apart than run together.
         *
         * The name is chosen just as carefully as `esp-file-sha256`: this
         * digest CAN equal a firmware PCR 4 measurement, which is exactly why
         * it must never be confused with the flat one above. */
        serial_early_print("[BOOT] self-measure-pe: ");
        if (g_self_measure.pe_present) {
            serial_early_print("status=ok pe-authenticode-sha256=");
            for (i = 0; i < SHA256B_DIGEST_LEN; i += 2u) {
                serial_early_print_hex16((UINT16)(((UINT16)g_self_measure.pe_digest[i] << 8)
                                                | (UINT16)g_self_measure.pe_digest[i + 1u]));
            }
        } else {
            serial_early_print("status=");
            serial_early_print(self_measure_pe_status_name(g_self_measure.pe_status));
            serial_early_print(" pe-authenticode-sha256=ABSENT");
        }
        serial_early_print("\n");
    } else {
        serial_early_print(" digest=ABSENT\n");
    }
}

/* Close every correlate line with its own elapsed cost. The self-measurement's
 * tsc_delta is finalized before this work begins, so without this the section's
 * boot-latency contribution -- an event-log walk plus polled-UART output -- was
 * invisible to the one timer that exists. A cost nobody measures is a cost
 * nobody notices growing. */
static void correlate_tsc_tail(UINT64 start)
{
    serial_early_print(" tsc=");
    serial_early_print_hex64(boot_rdtsc() - start);
    serial_early_print("\n");
}

/* ---------------------------------------------------------------------------
 * PCR 4 correlation.
 *
 * The digest above is evidence about a FILE on the ESP. Firmware separately
 * measured the image it actually executed into PCR 4, Authenticode-hashed,
 * during LoadImage. Between those two moments the file can be swapped, or the
 * volume remounted so the same path resolves elsewhere, and both digests stay
 * internally consistent while describing different bytes. Comparing them is
 * what turns a file observation into evidence about executed code.
 *
 * WHAT THIS IS NOT, TWICE OVER. First, the verdict leaves on the serial line
 * only. It is a local diagnostic, not an attestation: nothing here is extended
 * into a PCR, bound to a verifier nonce, or carried anywhere a remote party can
 * read it. Second, and less obvious: the event log is validated for internal
 * consistency, not authenticated. Only replaying it into the PCRs and comparing
 * against the TPM's own values would prove the log true, and nothing here does
 * that. So AGREE means "the log says firmware measured the bytes we hashed",
 * which is strictly weaker than "the TPM confirms it did". Both gaps are
 * tracked; neither is claimed by this code.
 *
 * ABSENCE IS NOT DISAGREEMENT, and keeping the two apart is the whole design.
 * A machine with no TPM, no event log, or no matching entry has nothing to
 * compare, and the honest report there is a weaker claim by name. Collapsing it
 * into a mismatch would manufacture a security finding out of ordinary
 * hardware, and an operator who learns to ignore one would ignore the real one.
 * ------------------------------------------------------------------------- */
static void correlate_pcr4_measurement(void)
{
    EFI_GUID lidp_guid = EFI_LOADED_IMAGE_DEVICE_PATH_PROTOCOL_GUID;
    EFI_GUID dpu_guid = EFI_DEVICE_PATH_UTILITIES_PROTOCOL_GUID;
    EFI_DEVICE_PATH_UTILITIES_PROTOCOL *dpu = (EFI_DEVICE_PATH_UTILITIES_PROTOCOL *)0;
    EFI_DEVICE_PATH_PROTOCOL *self_dp = (EFI_DEVICE_PATH_PROTOCOL *)0;
    struct tcgl_log lg;
    struct tcgl_match m;
    unsigned int candidates = 0;
    UINTN dp_size = 0;
    EFI_STATUS status;
    int rc;
    UINTN i;

    UINT64 corr_tsc_start = boot_rdtsc();

    post_code16(POST16_BL_PCR4_CORRELATE);
    serial_early_print("[BOOT] pcr4-correlate: ");

    /* Without our own Authenticode digest there is nothing to compare AGAINST,
     * which is an absence on our side rather than a claim about the platform. */
    if (!g_self_measure.pe_present) {
        serial_early_print("result=no-local-digest status=");
        serial_early_print(self_measure_pe_status_name(g_self_measure.pe_status));
        correlate_tsc_tail(corr_tsc_start);
        return;
    }

    /* Identity comes from the COMPLETE device path, never LoadedImage.FilePath:
     * FilePath is only the portion specific to DeviceHandle, so matching it
     * against an event's full path compares a suffix with a whole. */
    status = gBS->HandleProtocol(gImageHandle, &lidp_guid, (VOID **)&self_dp);
    if (EFI_ERROR(status) || !self_dp) {
        serial_early_print("result=no-image-device-path");
        correlate_tsc_tail(corr_tsc_start);
        return;
    }
    /* The object is measured by firmware, never self-walked: a device path is
     * hostile input and a heuristic cap is not an object bound. */
    status = gBS->LocateProtocol(&dpu_guid, (VOID *)0, (VOID **)&dpu);
    if (EFI_ERROR(status) || !dpu || !dpu->GetDevicePathSize) {
        serial_early_print("result=no-devpath-utilities");
        correlate_tsc_tail(corr_tsc_start);
        return;
    }
    dp_size = dpu->GetDevicePathSize(self_dp);
    if (dp_size < 4u) {
        serial_early_print("result=degenerate-device-path");
        correlate_tsc_tail(corr_tsc_start);
        return;
    }

    if (!g_boot_info_ptr->tpm_available || !g_boot_info_ptr->tpm_event_log
        || g_boot_info_ptr->tpm_event_log_size == 0u) {
        serial_early_print("result=no-event-log");
        correlate_tsc_tail(corr_tsc_start);
        return;
    }

    rc = tcgl_open((const unsigned char *)(UINTN)g_boot_info_ptr->tpm_event_log,
                   (unsigned long long)g_boot_info_ptr->tpm_event_log_size, &lg);
    if (rc != TCGL_OK) {
        /* A log we cannot validate is reported as such. Walking it anyway to
         * "try for a match" is how a corrupt log becomes an AGREE. */
        serial_early_print("result=log-unusable status=");
        serial_early_print(tcgl_status_name(rc));
        correlate_tsc_tail(corr_tsc_start);
        return;
    }

    rc = tcgl_find_image_load(&lg, 4u, (const unsigned char *)self_dp,
                              (unsigned long long)dp_size, &m, &candidates);
    if (rc != TCGL_OK) {
        serial_early_print("result=no-comparison status=");
        serial_early_print(tcgl_status_name(rc));
        if (rc == TCGL_AMBIGUOUS) {
            serial_early_print(" candidates=");
            serial_early_print_uint((UINT32)candidates);
        }
        correlate_tsc_tail(corr_tsc_start);
        return;
    }

    /* Only now, with exactly one identified entry, does the digest decide
     * anything. Comparing digests first would let a byte-identical image loaded
     * from a different path answer for this one. */
    for (i = 0; i < SHA256B_DIGEST_LEN; i++) {
        if (m.sha256[i] != g_self_measure.pe_digest[i]) {
            serial_early_print("result=DISAGREE firmware-sha256=");
            {
                UINTN j;
                for (j = 0; j < SHA256B_DIGEST_LEN; j += 2u) {
                    serial_early_print_hex16((UINT16)(((UINT16)m.sha256[j] << 8)
                                                    | (UINT16)m.sha256[j + 1u]));
                }
            }
            correlate_tsc_tail(corr_tsc_start);
            return;
        }
    }

    /* NOT "AGREE". Nothing here read or replayed a PCR, so the strongest honest
     * claim is that an internally-consistent log RECORDS this digest. A bare
     * AGREE on the wire would be read as TPM-backed by a consumer that never
     * sees the qualification in these comments, and the whole point of the
     * absence taxonomy above is that the wire words mean what they say. */
    serial_early_print("result=UNAUTHENTICATED-LOG-MATCH image-base=");
    serial_early_print_hex64((UINT64)m.image_base);
    serial_early_print(" image-len=");
    serial_early_print_uint((UINT32)m.image_len);
    correlate_tsc_tail(corr_tsc_start);
    post_code16(POST16_BL_PCR4_LOG_MATCH);
}

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

    /* Mark the source-section hint as soon as gST/gBS are wired, so any
     * fatal during the very first phase records BL_INIT (not UNKNOWN). */
    boot_set_section(BOOT_SECTION_BL_INIT);

    /* TODO-14 sec14: pin the panic-evidence page BEFORE anything else this
     * image does. The kernel's PMM keeps the page out of the allocator from
     * kernel entry onward (the blanket first-MiB reservation), but nothing
     * protects it across the window between the reset and that reservation:
     * firmware is free to satisfy any allocation from it, and so is every
     * AllocateAnyPages call this loader makes later. Losing that race
     * overwrites the previous boot's crash record with no diagnostic at all.
     *
     * This must precede ClearScreen too, not merely the first allocation: a
     * firmware protocol implementation may allocate internally, so the only
     * safe position is ahead of every firmware call the image issues.
     *
     * EfiLoaderData, matching the boot_info pin below. UEFI reserves
     * EfiReservedMemoryType for firmware and forbids a loader from allocating
     * it, and EfiACPIMemoryNVS would misdescribe loader-owned evidence as ACPI
     * state. LoaderData being kernel-reclaimable costs nothing here: the page
     * is inside the first MiB the PMM reserves wholesale regardless of type.
     *
     * The pin claims the page; it does NOT preserve what is in it. UEFI
     * guarantees allocation and memory-map reclassification, not that the
     * prior bytes survive, so a conforming allocator could scrub the page and
     * still return EFI_SUCCESS. That is the same non-contractual platform
     * behaviour the whole cross-boot record already rests on (RAM contents
     * surviving a reset and firmware init), and it fails SAFE either way: the
     * record is magic- and CRC-validated on restore (src/kernel/panic.c), so a
     * scrubbed page reads as "no record", never as a false one.
     *
     * NOT fatal on failure, unlike the boot_info pin: a machine that boots
     * without crash forensics is strictly better than one that refuses to
     * boot.
     *
     * A breadcrumb first because this is the image's FIRST firmware call, so
     * without one a fault inside the firmware allocator is indistinguishable
     * from never reaching our code at all. It MUST be the 8-bit post_code,
     * not post_code16: post_code16 also prints through serial_early_print,
     * and at this point s_serial_port is uninitialised .bss that firmware has
     * poisoned to 0xAF -- non-zero, so it passes serial_early_putchar's
     * `if (!s_serial_port) return;` guard and drives inb/outb on an arbitrary
     * I/O port. post_code is a bare `outb $0x80` with no static state.
     * POST16_BL_PANIC_PAGE is emitted later, from the report block, once
     * serial is genuinely up. */
    post_code(POST_PANIC_PAGE);
    {
        EFI_PHYSICAL_ADDRESS pe_addr =
            (EFI_PHYSICAL_ADDRESS)PANIC_EVIDENCE_PHYS_ADDR;
        g_panic_page_status = gBS->AllocatePages(AllocateAddress, EfiLoaderData,
                                                 1, &pe_addr);
        g_panic_page_attempted = 1;
    }

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

    /* TODO-14 sec14: report the panic-evidence pin attempted at the very top
     * of efi_main. Deferred to here because serial was not initialised and
     * boot_log_init had not run when the pin happened, so a line printed then
     * would have gone nowhere and been absent from the ESP boot log.
     *
     * Both outcomes are logged explicitly. A silent success would leave no
     * evidence that the protection is in force on a given machine, and an
     * unlogged failure is exactly the silent-corruption path this pin exists
     * to close -- the reader must be able to tell "protected" from
     * "unprotected" without guessing. Deliberately not "firmware owns it":
     * a refusal narrows the page's status, it does not name an owner.
     *
     * The success path also carries POST16_BL_PANIC_PAGE, which is this
     * section's smoke-test oracle. The pin itself left only the 8-bit
     * POST_PANIC_PAGE on port 0x80, which no serial capture can see. */
    if (g_panic_page_attempted) {
        if (!EFI_ERROR(g_panic_page_status)) {
            /* POST16 on the SUCCESS path only, which is what makes it a
             * meaningful required-code assertion: emitted unconditionally it
             * would prove the code RAN, not that the page was PINNED, and the
             * smoke gate would stay green on a boot that lost the race. */
            post_code16(POST16_BL_PANIC_PAGE);
            serial_early_print("[BOOT] Panic evidence: 0x");
            serial_early_print_hex16(
                (UINT16)(PANIC_EVIDENCE_PHYS_ADDR >> 16));
            serial_early_print_hex16((UINT16)PANIC_EVIDENCE_PHYS_ADDR);
            serial_early_print(" pinned\n");
        } else {
            serial_early_print("[WARN] Panic evidence: page 0x");
            serial_early_print_hex16(
                (UINT16)(PANIC_EVIDENCE_PHYS_ADDR >> 16));
            serial_early_print_hex16((UINT16)PANIC_EVIDENCE_PHYS_ADDR);
            serial_early_print(" not pinned, status=0x");
            serial_early_print_hex16((UINT16)g_panic_page_status);
            /* Deliberately NOT "firmware owns it": AllocatePages can refuse
             * with OUT_OF_RESOURCES or INVALID_PARAMETER, neither of which
             * establishes an owner. All this boot knows is that the page is
             * unprotected. */
            serial_early_print(" -- unprotected this boot; a prior crash"
                               " record may be overwritten\n");
        }
    }

    /* UKI globals reset BEFORE any path that consults them. Must run
     * unconditionally because detect_uki_sections() is gated on
     * LoadedImage->DeviceHandle; a fallback path that lacks
     * DeviceHandle would otherwise read EDK2 0xAF poison as a fake
     * .linux pointer. */
    reset_uki_sections();

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

    /* S16: Reserve the full boot_info range with UEFI before any raw
     * write, so later AllocatePages(AllocateAnyPages, ...) calls in
     * load_kernel, allocate_xhci_dma, and scratchpad setup cannot hand
     * back a page overlapping [BOOT_INFO_PHYS_ADDR, +sizeof(struct
     * boot_info)).  Without this, firmware was free to reuse the tail
     * pages of the handoff buffer, silently clobbering config/timing/
     * USB state while the header at offset 0 stayed intact -- the
     * kernel's post-handoff validator would still pass.  Halt early if
     * the range is already in use: the old silent-corruption path is
     * strictly worse than a visible boot_fatal. */
    {
        EFI_PHYSICAL_ADDRESS bi_addr = (EFI_PHYSICAL_ADDRESS)BOOT_INFO_PHYS_ADDR;
        UINTN bi_pages = (sizeof(struct boot_info) + 4095) / 4096;
        EFI_STATUS rs = gBS->AllocatePages(AllocateAddress, EfiLoaderData,
                                           bi_pages, &bi_addr);
        if (EFI_ERROR(rs)) {
            serial_early_print("[FAIL] boot_info reservation at 0x");
            serial_early_print_hex16((UINT16)(BOOT_INFO_PHYS_ADDR >> 16));
            serial_early_print_hex16((UINT16)BOOT_INFO_PHYS_ADDR);
            serial_early_print(" failed -- range already in use by firmware\n");
            boot_fatal(BOOT_ERR_BOOT_INFO_RESERVED,
                       "boot_info range already in use",
                       "Firmware owns the boot_info physical range.");
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

        /* Point boot_info to the reserved physical address */
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

    /* Step 0: Identify boot device via EFI_LOADED_IMAGE_PROTOCOL.
     * Store DeviceHandle globally so parse_boot_conf(), load_kernel(), and
     * all later boot consumers use the same boot device handle. */
    post_code16(POST16_BL_BOOT_DEV);
    {
        EFI_GUID li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
        EFI_LOADED_IMAGE_PROTOCOL *loaded_image = (EFI_LOADED_IMAGE_PROTOCOL *)0;
        EFI_STATUS li_status;

        li_status = gBS->HandleProtocol(gImageHandle, &li_guid,
                                         (VOID **)&loaded_image);
        if (!EFI_ERROR(li_status) && loaded_image) {
            /* Capture the image file path for network_boot_discover(): HTTP/PXE
             * URI/MAC/IP messaging nodes can live here rather than on the
             * DeviceHandle path (HTTP/PXE nodes can live in the image file path). Valid even when
             * DeviceHandle is NULL (pure HTTP Boot). */
            g_boot_image_file_path =
                (EFI_DEVICE_PATH_PROTOCOL *)loaded_image->FilePath;
            /* DeviceHandle is OPTIONAL on the LoadedImage; UKI
             * detection is independent.  Gating UKI detection on
             * DeviceHandle would let a UKI invocation with NULL
             * DeviceHandle silently degrade to split-path boot
             * without setting BOOT_FLAG_INVOKED_VIA_UKI -- breaking
             * the whole-chain Secure Boot guarantee on degraded
             * LoadedImage paths. */
            if (loaded_image->DeviceHandle) {
                g_boot_device_handle = loaded_image->DeviceHandle;
                serial_early_print("[BOOT] Boot device: handle=0x");
                serial_early_print_hex16((UINT16)((UINTN)g_boot_device_handle >> 48));
                serial_early_print_hex16((UINT16)((UINTN)g_boot_device_handle >> 32));
                serial_early_print_hex16((UINT16)((UINTN)g_boot_device_handle >> 16));
                serial_early_print_hex16((UINT16)(UINTN)g_boot_device_handle);
                serial_early_print("\n");
                post_code16(POST16_BL_BOOT_DEV_OK);
            } else {
                g_boot_device_handle = (EFI_HANDLE)0;
                serial_early_print("[WARN] Boot device: LoadedImage has NULL "
                                   "DeviceHandle -- UKI detection still runs\n");
            }

            /* UKI detection: walk our own PE section table for `.linux`
             * (UAPI Group Unified Kernel Image spec). When invoked
             * through a UKI artifact, the embedded kernel + cmdline +
             * osrel are covered by the same firmware-verified Secure
             * Boot signature as BOOTX64.EFI itself. Runs regardless
             * of DeviceHandle availability. */
            post_code16(POST16_BL_UKI_DETECT);
            detect_uki_sections(loaded_image);
            if (g_uki_kernel_ptr && g_uki_kernel_size > 0) {
                serial_early_print("[BOOT] UKI: .linux section found at 0x");
                serial_early_print_hex16((UINT16)((UINTN)g_uki_kernel_ptr >> 48));
                serial_early_print_hex16((UINT16)((UINTN)g_uki_kernel_ptr >> 32));
                serial_early_print_hex16((UINT16)((UINTN)g_uki_kernel_ptr >> 16));
                serial_early_print_hex16((UINT16)(UINTN)g_uki_kernel_ptr);
                serial_early_print(" size=");
                serial_early_print_uint((UINT32)g_uki_kernel_size);
                serial_early_print(" bytes\n");
                if (g_uki_cmdline_ptr)
                    serial_early_print("[BOOT] UKI: .cmdline embedded\n");
                if (g_uki_osrel_ptr)
                    serial_early_print("[BOOT] UKI: .osrel embedded\n");
                if (g_uki_initrd_ptr) {
                    serial_early_print("[BOOT] UKI: .initrd embedded size=");
                    serial_early_print_uint((UINT32)g_uki_initrd_size);
                    serial_early_print(" bytes\n");
                }
                if (g_uki_recovery_ptr) {
                    serial_early_print("[BOOT] UKI: .recovery embedded size=");
                    serial_early_print_uint((UINT32)g_uki_recovery_size);
                    serial_early_print(" bytes\n");
                }
                if (g_uki_modules_ptr) {
                    serial_early_print("[BOOT] UKI: .modules embedded size=");
                    serial_early_print_uint((UINT32)g_uki_modules_size);
                    serial_early_print(" bytes\n");
                }
                /* Copy each UKI signed payload out of LoadedImage
                 * memory into AllocatePages-allocated EfiLoaderData
                 * pages so the kernel-physical addresses stay valid
                 * after ExitBootServices. Failure is fatal: silently
                 * losing a signed payload would break the whole-chain
                 * Secure Boot signature claim. */
                uki_copy_payloads_to_loader_data();
                post_code16(POST16_BL_UKI_DETECT_OK);
            } else {
                serial_early_print("[BOOT] UKI: no .linux section -- "
                                   "split-path boot (BOOTX64.EFI + "
                                   "\\kernel.exe + boot.conf)\n");
            }
        } else {
            g_boot_device_handle = (EFI_HANDLE)0;
            serial_early_print("[WARN] Boot device: LoadedImage unavailable, "
                               "using LocateProtocol fallback\n");
            /* No POST16_BL_BOOT_DEV_OK -- last POST stays at 0xB090
             * so a POST card shows the lookup did not succeed. */
        }

        /* Measure the loader file while DeviceHandle and FilePath are in hand
         * and Boot Services are live. UNCONDITIONAL, including the branch where
         * LoadedImage itself was unavailable: that path leaves both globals
         * cleared, so the measurement reports an explicit ABSENT with a named
         * reason instead of leaving the record silently untouched. Never fatal;
         * see self_measure_run(). */
        self_measure_run();
    }

    /*: Populate boot device path from DevicePathToText protocol.
     * Type stays 0 (unknown) until parses the device path nodes. */
    g_boot_info_ptr->boot_device_type = 0;
    /* v16: defaults for boot device path detail (TODO-05 boot-device-detail). */
    g_boot_info_ptr->boot_nvme_nsid = 0;
    {
        UINTN ei;
        for (ei = 0; ei < 8; ei++)
            g_boot_info_ptr->boot_nvme_eui64[ei] = 0;
    }
    g_boot_info_ptr->boot_pci_device = 0xFF;
    g_boot_info_ptr->boot_pci_function = 0xFF;
    g_boot_info_ptr->boot_device_path[0] = '\0';
    if (g_boot_device_handle) {
        EFI_GUID dp_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
        EFI_GUID dptt_guid = EFI_DEVICE_PATH_TO_TEXT_PROTOCOL_GUID;
        EFI_DEVICE_PATH_PROTOCOL *dp = (EFI_DEVICE_PATH_PROTOCOL *)0;
        EFI_DEVICE_PATH_TO_TEXT_PROTOCOL *dptt = (EFI_DEVICE_PATH_TO_TEXT_PROTOCOL *)0;
        EFI_STATUS dp_s;

        dp_s = gBS->HandleProtocol(g_boot_device_handle, &dp_guid, (VOID **)&dp);
        if (!EFI_ERROR(dp_s) && dp) {
            /*: Convert device path to text for boot_info */
            dp_s = gBS->LocateProtocol(&dptt_guid, (VOID *)0, (VOID **)&dptt);
            if (!EFI_ERROR(dp_s) && dptt) {
                CHAR16 *text = dptt->ConvertDevicePathToText(dp, 0, 0);
                if (text) {
                    /* Sanitize CHAR16 -> ASCII: copy printable ASCII
                     * (0x20..0x7E) unchanged, replace anything else
                     * (control chars, non-ASCII codepoints, NUL) with
                     * '?'. The previous (char)(text[j] & 0x7F) mask
                     * could turn U+0100 into an embedded NUL and
                     * truncate the path mid-string before any vendor
                     * Unicode segment. */
                    UINTN j;
                    for (j = 0; j < 127 && text[j]; j++) {
                        CHAR16 c = text[j];
                        g_boot_info_ptr->boot_device_path[j] =
                            (c >= 0x20 && c <= 0x7E) ? (char)c : '?';
                    }
                    g_boot_info_ptr->boot_device_path[j] = '\0';
                    gBS->FreePool(text);

                    serial_early_print("[BOOT] Boot device path: ");
                    serial_early_print(g_boot_info_ptr->boot_device_path);
                    serial_early_print("\n");
                }
            } else {
                serial_early_print("[BOOT] DevicePathToText not available\n");
            }

            /*: Classify device type from the TEXT path string.
             * The raw device path from HandleProtocol on a partition handle
             * starts at the HD() node -- it doesn't include parent Messaging
             * nodes (Sata/NVMe/USB). But DevicePathToText returns the full
             * path including parents. Parse the text for known substrings. */
            if (g_boot_info_ptr->boot_device_path[0] != '\0' &&
                g_boot_info_ptr->boot_device_type == 0) {
                const char *p = g_boot_info_ptr->boot_device_path;
                /* sizeof yields the array byte count (128); subtract 5
                 * so p[pi+4] never crosses the buffer end. The trailing
                 * NUL termination from the device-path-text sanitizer
                 * also stops the loop early on shorter paths. */
                UINTN cap = sizeof(g_boot_info_ptr->boot_device_path);
                UINTN pi;
                for (pi = 0; pi + 5 <= cap && p[pi]; pi++) {
                    if (p[pi] == 'S' && p[pi+1] == 'a' && p[pi+2] == 't' &&
                        p[pi+3] == 'a' && p[pi+4] == '(') {
                        g_boot_info_ptr->boot_device_type = 1; break;
                    }
                    if (p[pi] == 'N' && p[pi+1] == 'V' && p[pi+2] == 'M' &&
                        p[pi+3] == 'e' && p[pi+4] == '(') {
                        g_boot_info_ptr->boot_device_type = 2; break;
                    }
                    if (p[pi] == 'U' && p[pi+1] == 'S' && p[pi+2] == 'B' &&
                        p[pi+3] == '(') {
                        g_boot_info_ptr->boot_device_type = 3; break;
                    }
                    if (p[pi] == 'I' && p[pi+1] == 'P' && p[pi+2] == 'v' &&
                        (p[pi+3] == '4' || p[pi+3] == '6') && p[pi+4] == '(') {
                        g_boot_info_ptr->boot_device_type = 4; break;
                    }
                    /* SD / eMMC -- modern laptops + tablets per UEFI 2.10 spec
                     * 10.3.4.24 / 10.3.4.27. SD before eMMC because "SD(" is
                     * a strict prefix of "SDmmc(" in some firmware variants. */
                    if (p[pi] == 'S' && p[pi+1] == 'D' && p[pi+2] == '(') {
                        g_boot_info_ptr->boot_device_type = 5; break;
                    }
                    if (p[pi] == 'e' && p[pi+1] == 'M' && p[pi+2] == 'M' &&
                        p[pi+3] == 'C' && p[pi+4] == '(') {
                        g_boot_info_ptr->boot_device_type = 6; break;
                    }
                }
            }

            /*: Walk raw device path nodes to extract partition GUID.
             * The HD() node IS present in the partition handle's device path. */
            g_boot_info_ptr->boot_partition_style = 0;
            {
                const EFI_DEVICE_PATH_PROTOCOL *node = dp;
                const UINT8 *base = (const UINT8 *)dp;
                UINT16 node_len;
                UINTN walked = 0;
                UINTN pci_oor_count = 0;  /* v16: count of out-of-spec PCI
                                           * nodes seen during walk;
                                           * single summary warn after */
                /* 4096 covers realistic vendor + NVMe namespace + GPT
                 * paths; 1024 was tight enough to silently drop GPT
                 * identity on a benign-but-long firmware path. */
                #define DP_MAX_WALK 4096

                while (walked + 4 <= DP_MAX_WALK &&
                       !(node->Type == EFI_DP_TYPE_END &&
                         node->SubType == EFI_DP_SUBTYPE_END_ENTIRE)) {
                    node_len = (UINT16)node->Length[0] |
                               ((UINT16)node->Length[1] << 8);
                    if (node_len < 4) break;
                    if (walked + node_len > DP_MAX_WALK) break;

                    /*: v16: Hardware/PCI -> leaf PCI Device + Function.
                     * UEFI 2.10 spec 10.3.2.1: Type=0x01 SubType=0x01,
                     * Length=6, Function at offset 4 (UINT8), Device at
                     * offset 5 (UINT8). Walk all PCI nodes; LAST one wins
                     * (paths can have PciRoot/PciBridge/.../Pci(leaf)). */
                    if (node->Type == EFI_DP_TYPE_HW &&
                        node->SubType == EFI_DP_HW_PCI &&
                        node_len >= 6) {
                        const UINT8 *nd = (const UINT8 *)node;
                        UINT8 pci_func = nd[4];
                        UINT8 pci_dev = nd[5];
                        /* Enforce the public 0..31 / 0..7 contract per
                         * UEFI 2.10 spec 10.3.2.1 + PCI Local Bus 3.0:
                         * Device is 5-bit, Function is 3-bit. Out-of-spec
                         * values are firmware corruption -- count them
                         * and emit ONE summary warn after the loop, not
                         * a per-node spam line (slow pre-EBS serial). */
                        if (pci_dev <= 31 && pci_func <= 7) {
                            g_boot_info_ptr->boot_pci_function = pci_func;
                            g_boot_info_ptr->boot_pci_device = pci_dev;
                        } else {
                            pci_oor_count++;
                        }
                    }

                    /*: v16: Messaging/NVMe -> NSID + EUI-64.
                     * UEFI 2.10 spec 10.3.4.21: Type=0x03 SubType=0x17,
                     * Length=16, NamespaceId UINT32 LE at offset 4,
                     * NamespaceUuid 8 bytes at offset 8 (EUI-64 byte order).
                     * Raw-node evidence is more trustworthy than the
                     * truncated text classifier: if we found an NVMe DP
                     * node here but the text classifier missed it (long
                     * vendor prefix truncated boot_device_path), set
                     * boot_device_type=2 from the raw node so registry
                     * provenance stays self-consistent. */
                    if (node->Type == EFI_DP_TYPE_MESSAGING &&
                        node->SubType == EFI_DP_MSG_NVME &&
                        node_len >= 16) {
                        const UINT8 *nd = (const UINT8 *)node;
                        g_boot_info_ptr->boot_nvme_nsid =
                            (UINT32)nd[4]
                            | ((UINT32)nd[5] << 8)
                            | ((UINT32)nd[6] << 16)
                            | ((UINT32)nd[7] << 24);
                        UINTN ei;
                        for (ei = 0; ei < 8; ei++)
                            g_boot_info_ptr->boot_nvme_eui64[ei] = nd[8 + ei];
                        if (g_boot_info_ptr->boot_device_type == 0)
                            g_boot_info_ptr->boot_device_type = 2;
                    }

                    /*: Media/HardDrive -> partition GUID + style.
                     * UEFI spec Table 10-58: HardDrive DP node is 42 bytes.
                     * Offset 24: PartitionSignature (16 bytes)
                     * Offset 40: MBRType (0x01=MBR, 0x02=GPT)
                     * Offset 41: SignatureType (0x02=GUID for GPT) */
                    if (node->Type == EFI_DP_TYPE_MEDIA &&
                        node->SubType == EFI_DP_MEDIA_HARDDRIVE &&
                        node_len >= 42 &&
                        g_boot_info_ptr->boot_partition_style == 0) {
                        const UINT8 *nd = (const UINT8 *)node;
                        UINT8 mbr_type = nd[40];
                        UINT8 sig_type = nd[41];
                        UINTN gi;

                        if (mbr_type == 0x02 && sig_type == 0x02) {
                            /* GPT + GUID signature: copy 16 bytes from offset 24 */
                            g_boot_info_ptr->boot_partition_style = 2;
                            for (gi = 0; gi < 16; gi++)
                                g_boot_info_ptr->boot_partition_guid[gi] = nd[24 + gi];
                        } else if (mbr_type == 0x01 && sig_type == 0x01) {
                            /* MBR + 32-bit signature: 4 bytes from offset 24 */
                            g_boot_info_ptr->boot_partition_style = 1;
                            for (gi = 0; gi < 4; gi++)
                                g_boot_info_ptr->boot_partition_guid[gi] = nd[24 + gi];
                        } else if (mbr_type == 0x02 || mbr_type == 0x01) {
                            /* Spec-violating firmware: MBRType says GPT/MBR
                             * but SignatureType disagrees. Refuse to trust
                             * partial data; log so firmware bugs are visible. */
                            static const char hex_lut[] = "0123456789ABCDEF";
                            char nb[3] = { 0, 0, 0 };
                            serial_early_print("[WARN] HD() inconsistent: mbr_type=0x");
                            nb[0] = hex_lut[(mbr_type >> 4) & 0xF];
                            nb[1] = hex_lut[mbr_type & 0xF];
                            serial_early_print(nb);
                            serial_early_print(" sig_type=0x");
                            nb[0] = hex_lut[(sig_type >> 4) & 0xF];
                            nb[1] = hex_lut[sig_type & 0xF];
                            serial_early_print(nb);
                            serial_early_print(" -- partition style left unknown\n");
                        }
                    }

                    node = (const EFI_DEVICE_PATH_PROTOCOL *)
                           ((const UINT8 *)node + node_len);
                    walked = (UINTN)((const UINT8 *)node - base);
                }
                #undef DP_MAX_WALK

                /* v16: post-walk summary warn for out-of-spec PCI nodes
                 * (rate-limited to one log line regardless of count). */
                if (pci_oor_count > 0) {
                    serial_early_print("[WARN] PCI DP node(s) out of spec "
                        "range -- skipped\n");
                }
            }

            /* Log detected type */
            {
                static const char *type_names[] = {
                    "unknown", "SATA", "NVMe", "USB", "network", "SD", "eMMC"
                };
                UINT8 t = g_boot_info_ptr->boot_device_type;
                serial_early_print("[BOOT] Boot device type: ");
                serial_early_print(t <= 6 ? type_names[t] : "invalid");
                serial_early_print("\n");
            }

            /*: v16: Log NVMe NSID + EUI-64 when boot device is NVMe.
             * UEFI 2.10 spec 10.3.4.21. NSID=0 = no NVMe DP node found. */
            if (g_boot_info_ptr->boot_nvme_nsid != 0) {
                static const char nvme_hex[] = "0123456789ABCDEF";
                serial_early_print("[BOOT] NVMe NSID=0x");
                {
                    UINT32 ns = g_boot_info_ptr->boot_nvme_nsid;
                    serial_early_print_hex16((UINT16)(ns >> 16));
                    serial_early_print_hex16((UINT16)ns);
                }
                serial_early_print(" EUI-64=");
                {
                    UINTN ei;
                    char nb[3] = { 0, 0, 0 };
                    const UINT8 *eu = g_boot_info_ptr->boot_nvme_eui64;
                    for (ei = 0; ei < 8; ei++) {
                        if (ei > 0) serial_early_print(":");
                        nb[0] = nvme_hex[(eu[ei] >> 4) & 0xF];
                        nb[1] = nvme_hex[eu[ei] & 0xF];
                        serial_early_print(nb);
                    }
                }
                serial_early_print("\n");
            }

            /*: v16: Log leaf PCI device + function when on PCI.
             * UEFI 2.10 spec 10.3.2.1. 0xFF sentinel = not on PCI bus. */
            if (g_boot_info_ptr->boot_pci_device != 0xFF) {
                static const char pci_hex[] = "0123456789ABCDEF";
                char nb[3] = { 0, 0, 0 };
                serial_early_print("[BOOT] Boot device PCI: dev=0x");
                nb[0] = pci_hex[(g_boot_info_ptr->boot_pci_device >> 4) & 0xF];
                nb[1] = pci_hex[g_boot_info_ptr->boot_pci_device & 0xF];
                serial_early_print(nb);
                serial_early_print(" func=0x");
                nb[0] = pci_hex[(g_boot_info_ptr->boot_pci_function >> 4) & 0xF];
                nb[1] = pci_hex[g_boot_info_ptr->boot_pci_function & 0xF];
                serial_early_print(nb);
                serial_early_print("\n");
            }

            /*: Log partition GUID or MBR signature */
            if (g_boot_info_ptr->boot_partition_style == 2) {
                const UINT8 *g = g_boot_info_ptr->boot_partition_guid;
                /* Check for all-zero GUID */
                int zero = 1;
                UINT8 zi;
                for (zi = 0; zi < 16; zi++)
                    if (g[zi]) { zero = 0; break; }
                if (zero) {
                    serial_early_print("[WARN] Boot partition GUID is zero "
                                       "-- firmware may not support GPT\n");
                } else {
                    serial_early_print("[BOOT] Boot partition: GUID=");
                    /* Format: AABBCCDD-EEFF-GGHH-IIJJ-KKLLMMNNOOPP
                     * UEFI GUID layout: Data1(4B LE) Data2(2B LE)
                     * Data3(2B LE) Data4(8B) */
                    serial_early_print_hex16((UINT16)(g[3] << 8 | g[2]));
                    serial_early_print_hex16((UINT16)(g[1] << 8 | g[0]));
                    serial_early_print("-");
                    serial_early_print_hex16((UINT16)(g[5] << 8 | g[4]));
                    serial_early_print("-");
                    serial_early_print_hex16((UINT16)(g[7] << 8 | g[6]));
                    serial_early_print("-");
                    serial_early_print_hex16((UINT16)(g[8] << 8 | g[9]));
                    serial_early_print("-");
                    serial_early_print_hex16((UINT16)(g[10] << 8 | g[11]));
                    serial_early_print_hex16((UINT16)(g[12] << 8 | g[13]));
                    serial_early_print_hex16((UINT16)(g[14] << 8 | g[15]));
                    serial_early_print(" (GPT)\n");
                }
            } else if (g_boot_info_ptr->boot_partition_style == 1) {
                const UINT8 *g = g_boot_info_ptr->boot_partition_guid;
                serial_early_print("[BOOT] Boot partition: MBR sig=0x");
                serial_early_print_hex16((UINT16)(g[3] << 8 | g[2]));
                serial_early_print_hex16((UINT16)(g[1] << 8 | g[0]));
                serial_early_print("\n");
            }
        }
    }

    /* +: Removable media detection AND pre-boot device health check.
     * Single HandleProtocol(BlockIO) call for both. Reads: RemovableMedia,
     * MediaPresent, BlockSize, LastBlock, ReadOnly, LogicalPartition.
     * If BlockIO unavailable, USB defaults to removable (safe for cache policy). */
    g_boot_info_ptr->boot_media_present = 1;
    g_boot_info_ptr->boot_device_removable =
        (g_boot_info_ptr->boot_device_type == 3) ? 1 : 0;
    {
        EFI_BLOCK_IO_PROTOCOL *esp_bio_for_integrity = (EFI_BLOCK_IO_PROTOCOL *)0;
        UINT32 esp_partition_mib = 0;
        if (g_boot_device_handle) {
            EFI_GUID bio_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
            EFI_BLOCK_IO_PROTOCOL *bio = (EFI_BLOCK_IO_PROTOCOL *)0;
            EFI_STATUS bio_s;

            bio_s = gBS->HandleProtocol(g_boot_device_handle, &bio_guid,
                                         (VOID **)&bio);
            if (!EFI_ERROR(bio_s) && bio && bio->Media) {
                EFI_BLOCK_IO_MEDIA *m = bio->Media;

                /*: Removable + MediaPresent */
                g_boot_info_ptr->boot_device_removable =
                    m->RemovableMedia ? 1 : 0;
                g_boot_info_ptr->boot_media_present =
                    m->MediaPresent ? 1 : 0;

                /*: Capacity */
                {
                    UINT64 total_bytes = 0;
                    UINT32 mib = 0;
                    if (m->BlockSize > 0 && m->LastBlock < 0xFFFFFFFFFFFFFFFFULL) {
                        UINT64 blocks = m->LastBlock + 1;
                        if (blocks <= 0xFFFFFFFFFFFFFFFFULL / (UINT64)m->BlockSize)
                            total_bytes = blocks * (UINT64)m->BlockSize;
                        /* Saturate to UINT32_MAX rather than wrap: a 4+ PiB
                         * partition would wrap to 1..15 MiB and bypass the
                         * "FAT16 only on tiny ESPs" gate in esp_check_fat_bpb. */
                        UINT64 mib64 = total_bytes / (1024 * 1024);
                        mib = (mib64 > 0xFFFFFFFFULL)
                              ? 0xFFFFFFFFU
                              : (UINT32)mib64;
                    }
                    esp_partition_mib = mib;
                    serial_early_print("[BOOT] Boot disk: ");
                    {
                        char nb[20];
                        UINT32 v = mib;
                        int n = 0;
                        if (v == 0) nb[n++] = '0';
                        else { while (v > 0 && n < 20) { nb[n++] = '0' + (char)(v % 10); v /= 10; } }
                        while (n > 0) serial_early_putchar(nb[--n]);
                    }
                    serial_early_print(" MiB (");
                    {
                        static const char *tn[] = {"unknown","SATA","NVMe","USB","network","SD","eMMC"};
                        UINT8 t = g_boot_info_ptr->boot_device_type;
                        serial_early_print(t <= 6 ? tn[t] : "?");
                    }
                    serial_early_print(")\n");
                }

                /*: ReadOnly warning (abnormal on fixed non-USB disks) */
                if (m->ReadOnly && g_boot_info_ptr->boot_device_type != 3)
                    serial_early_print("[WARN] Boot disk is read-only "
                                       "-- possible hardware failure\n");

                /*: LogicalPartition diagnostic */
                if (m->LogicalPartition)
                    serial_early_print("[BOOT] Boot device is a logical partition "
                                       "(not whole disk)\n");

                esp_bio_for_integrity = bio;
            } else {
                serial_early_print("[WARN] BlockIO not available on boot device"
                               " -- using type-based default\n");
            }
        }

        /* ESP integrity gate (TODO-02 part 13). Runs after the BlockIO
         * probe so esp_size_mb is populated, and BEFORE parse_boot_conf
         * so corruption / wrong-partition cases halt before any disk
         * content is trusted. UKI mode skips identity validation; the
         * GPT type GUID, FAT32 BPB, and required-files checks are
         * split-path-only gates. */
        esp_integrity_check(g_boot_device_handle, esp_bio_for_integrity,
                             esp_partition_mib);

        /* Media role detection: read /IPOS/role.txt from ESP and from
         * the BlackBox service partition on the SAME physical disk,
         * cross-check, and (for installer/recovery/diagnostics) update
         * boot_info->boot_path + boot_reason. Runs HERE (post-ESP-
         * integrity, pre-parse_boot_conf, pre-ExitBootServices) so
         * gBS->LocateHandleBuffer / AllocatePool used by the BlackBox
         * sibling lookup are still valid. The boot-decision populate
         * block farther down only sets defaults; this call provides the
         * marker-derived overrides BEFORE the populate block runs, then
         * the populate block reads back the values we wrote. */
        media_role_detect_and_record(g_boot_device_handle);
    }

    /* logging */
    if (g_boot_info_ptr->boot_device_removable)
        serial_early_print("[BOOT] Boot device is removable "
                           "-- write-caching will be disabled by default\n");
    else
        serial_early_print("[BOOT] Boot device is fixed\n");

    if (!g_boot_info_ptr->boot_media_present)
        serial_early_print("[WARN] Boot media not present "
                           "(reported by firmware -- may be stale)\n");

    /* Read UEFI boot variables (BootCurrent, BootOrder, BootNext).
     * RuntimeServices->GetVariable is available before ExitBootServices. */
    {
        EFI_GUID global_guid = EFI_GLOBAL_VARIABLE_GUID;
        EFI_RUNTIME_SERVICES *rt = gST->RuntimeServices;
        UINT32 attrs = 0;

        /* Defaults */
        g_boot_info_ptr->uefi_boot_current = 0xFFFF;
        g_boot_info_ptr->uefi_boot_next = 0xFFFF;
        g_boot_info_ptr->uefi_boot_next_valid = 0;
        g_boot_info_ptr->uefi_boot_order_count = 0;
        /* v15: Extended boot-var capability surface defaults. */
        g_boot_info_ptr->boot_current_attrs = 0;
        g_boot_info_ptr->boot_option_support = 0;
        g_boot_info_ptr->os_indications_supported = 0;
        g_boot_info_ptr->boot_description[0] = '\0';

        if (rt && rt->GetVariable) {
            /* BootCurrent (UINT16) */
            {
                UINT16 bc = 0;
                UINTN sz = sizeof(bc);
                EFI_STATUS vs = rt->GetVariable(
                    u"BootCurrent", &global_guid, &attrs, &sz, &bc);
                if (!EFI_ERROR(vs) && sz == sizeof(bc)) {
                    g_boot_info_ptr->uefi_boot_current = bc;
                    serial_early_print("[BOOT] BootCurrent=0x");
                    serial_early_print_hex16(bc);
                    serial_early_print("\n");
                }
            }

            /* BootOrder (UINT16 array -- read full, store first 16).
             * Use a 128-entry stack buffer so GetVariable succeeds even
             * on firmware with large boot menus (>16 entries). */
            {
                UINT16 order[128];
                UINTN sz = sizeof(order);
                EFI_STATUS vs = rt->GetVariable(
                    u"BootOrder", &global_guid, &attrs, &sz, order);
                if (!EFI_ERROR(vs) && sz >= 2 && (sz % 2) == 0) {
                    UINTN count = sz / 2;
                    if (count > 16) count = 16;
                    g_boot_info_ptr->uefi_boot_order_count = (UINT8)count;
                    UINTN oi;
                    for (oi = 0; oi < count; oi++)
                        g_boot_info_ptr->uefi_boot_order[oi] = order[oi];

                    serial_early_print("[BOOT] BootOrder=[");
                    for (oi = 0; oi < count; oi++) {
                        if (oi > 0) serial_early_print(",");
                        serial_early_print("0x");
                        serial_early_print_hex16(order[oi]);
                    }
                    serial_early_print("]\n");
                }
            }

            /* BootNext (UINT16, optional one-shot override) */
            {
                UINT16 bn = 0;
                UINTN sz = sizeof(bn);
                EFI_STATUS vs = rt->GetVariable(
                    u"BootNext", &global_guid, &attrs, &sz, &bn);
                if (!EFI_ERROR(vs) && sz == sizeof(bn)) {
                    g_boot_info_ptr->uefi_boot_next = bn;
                    g_boot_info_ptr->uefi_boot_next_valid = 1;
                    serial_early_print("[BOOT] BootNext=0x");
                    serial_early_print_hex16(bn);
                    serial_early_print(" (one-shot override)\n");
                }
            }

            /* S12: Decode Boot#### EFI_LOAD_OPTION for the current boot entry.
             * Format variable name as Boot0000..BootFFFF from BootCurrent. */
            if (g_boot_info_ptr->uefi_boot_current != 0xFFFF) {
                static const char hex[] = "0123456789ABCDEF";
                UINT16 bc = g_boot_info_ptr->uefi_boot_current;
                CHAR16 var_name[] = u"Boot0000";
                /* Fill in the 4 hex digits (UCS-2) */
                var_name[4] = (CHAR16)hex[(bc >> 12) & 0xF];
                var_name[5] = (CHAR16)hex[(bc >> 8) & 0xF];
                var_name[6] = (CHAR16)hex[(bc >> 4) & 0xF];
                var_name[7] = (CHAR16)hex[bc & 0xF];

                UINT8 lo_buf[2048];
                UINTN lo_sz = sizeof(lo_buf);
                EFI_STATUS lo_s = rt->GetVariable(
                    var_name, &global_guid, &attrs, &lo_sz, lo_buf);

                if (lo_s == EFI_BUFFER_TOO_SMALL) {
                    serial_early_print("[BOOT] Boot#### entry too large ("
                                       "skipping decode)\n");
                } else if (!EFI_ERROR(lo_s) && lo_sz > 6) {
                    /* EFI_LOAD_OPTION layout:
                     * [0..3]  Attributes   (UINT32)
                     * [4..5]  FilePathListLength (UINT16)
                     * [6..]   Description  (NUL-terminated CHAR16)
                     * [after NUL] FilePathList (FilePathListLength bytes) */
                    UINT32 lo_attrs = (UINT32)lo_buf[0]
                                    | ((UINT32)lo_buf[1] << 8)
                                    | ((UINT32)lo_buf[2] << 16)
                                    | ((UINT32)lo_buf[3] << 24);
                    UINT16 fp_len = (UINT16)(lo_buf[4] | ((UINT16)lo_buf[5] << 8));
                    const CHAR16 *desc = (const CHAR16 *)&lo_buf[6];
                    UINTN max_desc_bytes = lo_sz - 6;

                    /* v15: Capture Attributes for boot_info / registry. */
                    g_boot_info_ptr->boot_current_attrs = lo_attrs;

                    /* v15: Capture Description ASCII into boot_info (63 chars + NUL).
                     * Same trunc rule as the serial log below. */
                    {
                        UINTN ci;
                        for (ci = 0; ci < 63 && (ci * 2 + 1) < max_desc_bytes; ci++) {
                            if (desc[ci] == 0) break;
                            g_boot_info_ptr->boot_description[ci] = (char)(desc[ci] & 0x7F);
                        }
                        g_boot_info_ptr->boot_description[ci] = '\0';
                    }

                    /* Print description as ASCII (truncate at 80 chars) */
                    serial_early_print("[BOOT] Boot");
                    serial_early_print_hex16(bc);
                    serial_early_print(": ");
                    {
                        UINTN di;
                        for (di = 0; di < 80 && (di * 2 + 1) < max_desc_bytes; di++) {
                            if (desc[di] == 0) break;
                            serial_early_putchar((char)(desc[di] & 0x7F));
                        }
                    }
                    serial_early_print("\n");

                    /* Find end of Description to locate FilePathList */
                    {
                        UINTN di;
                        const UINT8 *fp_start = (const UINT8 *)0;
                        for (di = 0; (di * 2 + 1) < max_desc_bytes; di++) {
                            if (desc[di] == 0) {
                                fp_start = (const UINT8 *)&desc[di + 1];
                                break;
                            }
                        }

                        /* Convert FilePathList to text via DevicePathToText.
                         * First validate: walk nodes within fp_len to confirm
                         * a valid END_ENTIRE node exists (per Gate 13: spec
                         * compliance -- ConvertDevicePathToText doesn't take
                         * a length and would read past the buffer otherwise). */
                        if (fp_start && fp_len >= 4 &&
                            (UINTN)(fp_start - lo_buf) + fp_len <= lo_sz) {
                            int dp_valid = 0;
                            {
                                const UINT8 *vn = fp_start;
                                UINTN vw = 0;
                                while (vw + 4 <= fp_len) {
                                    UINT16 vl = (UINT16)(vn[2] | ((UINT16)vn[3] << 8));
                                    if (vl < 4 || vw + vl > fp_len) break;
                                    if (vn[0] == EFI_DP_TYPE_END &&
                                        vn[1] == EFI_DP_SUBTYPE_END_ENTIRE) {
                                        dp_valid = 1;
                                        break;
                                    }
                                    vn += vl;
                                    vw += vl;
                                }
                            }

                            if (dp_valid) {
                                EFI_GUID dptt_s12 = EFI_DEVICE_PATH_TO_TEXT_PROTOCOL_GUID;
                                EFI_DEVICE_PATH_TO_TEXT_PROTOCOL *dptt12 =
                                    (EFI_DEVICE_PATH_TO_TEXT_PROTOCOL *)0;
                                gBS->LocateProtocol(&dptt_s12, (VOID *)0,
                                                     (VOID **)&dptt12);
                                if (dptt12) {
                                    CHAR16 *fp_txt = dptt12->ConvertDevicePathToText(
                                        (EFI_DEVICE_PATH_PROTOCOL *)fp_start, 0, 0);
                                    if (fp_txt) {
                                        serial_early_print("[BOOT]   Path: ");
                                        UINTN fi;
                                        for (fi = 0; fi < 120 && fp_txt[fi]; fi++)
                                            serial_early_putchar((char)(fp_txt[fi] & 0x7F));
                                        if (fp_txt[fi])
                                            serial_early_print("...");
                                        serial_early_print("\n");
                                        gBS->FreePool(fp_txt);
                                    }
                                }
                            }
                        }
                    }
                }
                /* EFI_NOT_FOUND is silent (some VMs have minimal NVRAM) */
            }

            /* v15: Extended boot-variable capability surface -- read the two
             * firmware-side capability globals. EFI_NOT_FOUND is silent
             * (defaults stay 0); only successful exact-size reads populate. */
            post_code16(POST16_BL_BOOT_VAR_EXT);

            /* BootOptionSupport (UINT32) -- UEFI 2.10 spec 3.1.4 */
            {
                UINT32 bos = 0;
                UINTN sz = sizeof(bos);
                EFI_STATUS vs = rt->GetVariable(
                    u"BootOptionSupport", &global_guid, &attrs, &sz, &bos);
                if (!EFI_ERROR(vs) && sz == sizeof(bos))
                    g_boot_info_ptr->boot_option_support = bos;
            }

            /* OsIndicationsSupported (UINT64) -- UEFI 2.10 spec 8.5.4.
             * READ-ONLY firmware capability variable, distinct from the
             * OsIndications write-path capsule trigger banned by the UEFI
             * hardening TODO. The read carries no capsule risk. */
            {
                UINT64 ois = 0;
                UINTN sz = sizeof(ois);
                EFI_STATUS vs = rt->GetVariable(
                    u"OsIndicationsSupported", &global_guid, &attrs, &sz, &ois);
                if (!EFI_ERROR(vs) && sz == sizeof(ois))
                    g_boot_info_ptr->os_indications_supported = ois;
            }

            /* Combined diagnostic log: BootCurrent attrs + capability bits */
            serial_early_print("[BOOT] Boot var caps: attrs=0x");
            serial_early_print_hex16(
                (UINT16)(g_boot_info_ptr->boot_current_attrs >> 16));
            serial_early_print_hex16(
                (UINT16)g_boot_info_ptr->boot_current_attrs);
            serial_early_print(
                (g_boot_info_ptr->boot_current_attrs & 0x1)
                    ? " (active" : " (inactive");
            if (g_boot_info_ptr->boot_current_attrs & 0x8)
                serial_early_print(" hidden");
            serial_early_print(") BootOptionSupport=0x");
            serial_early_print_hex16(
                (UINT16)(g_boot_info_ptr->boot_option_support >> 16));
            serial_early_print_hex16(
                (UINT16)g_boot_info_ptr->boot_option_support);
            serial_early_print(" OsIndicationsSupported=0x");
            serial_early_print_hex16(
                (UINT16)(g_boot_info_ptr->os_indications_supported >> 48));
            serial_early_print_hex16(
                (UINT16)(g_boot_info_ptr->os_indications_supported >> 32));
            serial_early_print_hex16(
                (UINT16)(g_boot_info_ptr->os_indications_supported >> 16));
            serial_early_print_hex16(
                (UINT16)g_boot_info_ptr->os_indications_supported);
            serial_early_print("\n");

            post_code16(POST16_BL_BOOT_VAR_EXT_OK);
        }
    }

    /* Step 1a: discover firmware network-boot provenance (SNP/PXE/HTTP Boot).
     * Runs after boot-device identification (g_boot_device_handle +
     * g_boot_image_file_path set) and BEFORE ExitBootServices so the firmware
     * network protocols are still live (network-boot discovery). */
    network_boot_discover(g_boot_device_handle, g_boot_image_file_path);
    net_dhcp_capture(g_boot_device_handle);
    net_tftp_probe(g_boot_device_handle);
    net_http_probe(g_boot_device_handle);

    /* Step 1a': detect foreign-OS UEFI bootloaders on other volumes for the
     * multi-OS chainload menu (TODO-27 sec1). Pre-EBS so SimpleFileSystem
     * handles are live; read-only probe (records g_chainload_targets, logs). */
    chainload_detect();

    /* Step 1b: Parse boot.conf first -- Resolution= key needed by init_gop */
    g_boot_info_ptr->timing.conf_start = boot_rdtsc();
    parse_boot_conf();
    g_boot_info_ptr->timing.conf_end = boot_rdtsc();

    /* section 5 typed payload load. Runs right after boot.conf parse so
     * staging is populated, and before load_kernel so the payload
     * allocations do not compete with the 32/16/8 MiB graduated kernel
     * buffer. All failures are fatal: a missing payload == boot fail. */
    load_staged_payloads();

    /* Headless enrollment authorization, appended AFTER the staged payloads so
     * the packed-prefix invariant holds. Non-fatal on every path: an absent or
     * malformed authorization means this machine simply cannot enroll a
     * baseline without a console, which is the pre-existing behavior. */
    publish_headless_authz_payload();

    /*: Boot device enumeration log -- gated behind verbose=1 in boot.conf
     * to avoid per-handle Open/Close overhead on firmware with many devices.
     * Runs after parse_boot_conf so the flag is available. */
    if (g_boot_info_ptr->config.verbose) {
        EFI_GUID fs_enum_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
        EFI_GUID dp_enum_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
        EFI_GUID dptt_enum_guid = EFI_DEVICE_PATH_TO_TEXT_PROTOCOL_GUID;
        EFI_GUID bio_enum_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
        EFI_HANDLE *enum_handles = (EFI_HANDLE *)0;
        UINTN enum_count = 0;
        EFI_STATUS enum_s;
        EFI_DEVICE_PATH_TO_TEXT_PROTOCOL *enum_dptt = (EFI_DEVICE_PATH_TO_TEXT_PROTOCOL *)0;

        gBS->LocateProtocol(&dptt_enum_guid, (VOID *)0, (VOID **)&enum_dptt);

        enum_s = gBS->LocateHandleBuffer(ByProtocol, &fs_enum_guid,
                                          (VOID *)0, &enum_count, &enum_handles);
        if (!EFI_ERROR(enum_s) && enum_handles && enum_count > 0) {
            serial_early_print("[BOOT] --- Device Enumeration (verbose=1) ---\n");
            /* Cap diagnostic enumeration; large multi-disk / SAN / USB
             * topologies can otherwise stall the firmware watchdog while
             * we burn ~10 ms per handle on Open/Close probes. */
            #define DIAG_ENUM_MAX 64
            UINTN logged = enum_count > DIAG_ENUM_MAX ? DIAG_ENUM_MAX : enum_count;
            /* Pre-scan ALL enum entries (cheap identity compare, no firmware
             * calls) to find the boot handle's index. Distinguishes
             * "boot in skipped tail" from "boot not in SimpleFS enum at
             * all" (e.g. block-device boot without a SimpleFS handle). */
            int boot_found_in_enum = 0;
            UINTN boot_enum_index = 0;
            UINTN ei;
            if (g_boot_device_handle) {
                for (ei = 0; ei < enum_count; ei++) {
                    if (enum_handles[ei] == g_boot_device_handle) {
                        boot_found_in_enum = 1;
                        boot_enum_index = ei;
                        break;
                    }
                }
            }
            for (ei = 0; ei < logged; ei++) {
                /* Refresh firmware watchdog every 8 handles -- per-handle
                 * Open/Close can take ~10 ms on real firmware. */
                if ((ei & 0x7) == 0) watchdog_reset();

                int is_boot = (enum_handles[ei] == g_boot_device_handle);
                int has_kernel = 0;
                int removable = -1;

                /* Check kernel presence */
                {
                    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *efs =
                        (EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *)0;
                    EFI_FILE_PROTOCOL *eroot;
                    EFI_FILE_PROTOCOL *efile;
                    EFI_STATUS es = gBS->HandleProtocol(enum_handles[ei],
                                        &fs_enum_guid, (VOID **)&efs);
                    if (!EFI_ERROR(es) && efs) {
                        es = efs->OpenVolume(efs, &eroot);
                        if (!EFI_ERROR(es)) {
                            es = eroot->Open(eroot, &efile,
                                              u"\\boot\\kernel.exe",
                                              EFI_FILE_MODE_READ, 0);
                            if (!EFI_ERROR(es)) {
                                has_kernel = 1;
                                efile->Close(efile);
                            }
                            eroot->Close(eroot);
                        }
                    }
                }

                /* Check removable */
                {
                    EFI_BLOCK_IO_PROTOCOL *ebio;
                    EFI_STATUS es = gBS->HandleProtocol(enum_handles[ei],
                                        &bio_enum_guid, (VOID **)&ebio);
                    if (!EFI_ERROR(es) && ebio && ebio->Media)
                        removable = ebio->Media->RemovableMedia ? 1 : 0;
                }

                /* Log device path + tags */
                {
                    EFI_DEVICE_PATH_PROTOCOL *edp;
                    EFI_STATUS es = gBS->HandleProtocol(enum_handles[ei],
                                        &dp_enum_guid, (VOID **)&edp);

                    serial_early_print("[BOOT]  [");
                    serial_early_print_hex16((UINT16)ei);
                    serial_early_print("] ");
                    if (is_boot) serial_early_print("*BOOT* ");

                    if (!EFI_ERROR(es) && edp && enum_dptt) {
                        CHAR16 *etxt = enum_dptt->ConvertDevicePathToText(edp, 0, 0);
                        if (etxt) {
                            UINTN ej;
                            for (ej = 0; ej < 80 && etxt[ej]; ej++)
                                serial_early_putchar((char)(etxt[ej] & 0x7F));
                            if (etxt[ej]) serial_early_print("...");
                            gBS->FreePool(etxt);
                        } else {
                            serial_early_print("(no path text)");
                        }
                    } else {
                        serial_early_print("(no device path)");
                    }

                    serial_early_print(has_kernel ? " [kernel]" : "");
                    serial_early_print(removable == 1 ? " [removable]"
                                     : removable == 0 ? " [fixed]"
                                     : " [removable=?]");
                    serial_early_print("\n");
                }
            }

            /* If the boot device IS in the SimpleFS enum but landed
             * outside the capped range, emit a minimal *BOOT* line so
             * the diagnostic always names it. (If boot is not in the
             * enum at all -- e.g. block-device boot without SimpleFS
             * -- partition-GUID and boot-variable logging already
             * cover it.) */
            if (boot_found_in_enum && boot_enum_index >= logged) {
                EFI_DEVICE_PATH_PROTOCOL *bdp =
                    (EFI_DEVICE_PATH_PROTOCOL *)0;
                EFI_STATUS bes = gBS->HandleProtocol(g_boot_device_handle,
                                    &dp_enum_guid, (VOID **)&bdp);
                serial_early_print("[BOOT]  [..] *BOOT* ");
                if (!EFI_ERROR(bes) && bdp && enum_dptt) {
                    CHAR16 *btxt = enum_dptt->ConvertDevicePathToText(bdp, 0, 0);
                    if (btxt) {
                        UINTN bj;
                        for (bj = 0; bj < 80 && btxt[bj]; bj++)
                            serial_early_putchar((char)(btxt[bj] & 0x7F));
                        if (btxt[bj]) serial_early_print("...");
                        gBS->FreePool(btxt);
                    } else {
                        serial_early_print("(no path text)");
                    }
                } else {
                    serial_early_print("(no device path)");
                }
                serial_early_print(" (outside capped range)\n");
            }

            /* Summary line */
            serial_early_print("[BOOT] Device summary: ");
            {
                char nb[20];
                UINTN v = enum_count;
                int n = 0;
                if (v == 0) nb[n++] = '0';
                else { while (v > 0 && n < 20) { nb[n++] = '0' + (char)(v % 10); v /= 10; } }
                while (n > 0) serial_early_putchar(nb[--n]);
            }
            if (enum_count > DIAG_ENUM_MAX) {
                serial_early_print(" (logged first ");
                {
                    char nb[8];
                    UINTN v = DIAG_ENUM_MAX;
                    int n = 0;
                    while (v > 0 && n < 8) { nb[n++] = '0' + (char)(v % 10); v /= 10; }
                    while (n > 0) serial_early_putchar(nb[--n]);
                }
                serial_early_print(")");
            }
            #undef DIAG_ENUM_MAX
            serial_early_print(" devices found, boot=");
            {
                static const char *tn[] = {"unknown","SATA","NVMe","USB","network","SD","eMMC"};
                UINT8 t = g_boot_info_ptr->boot_device_type;
                serial_early_print(t <= 6 ? tn[t] : "?");
            }
            serial_early_print(g_boot_info_ptr->boot_device_removable
                               ? " (removable)" : " (fixed)");
            serial_early_print("\n");
            serial_early_print("[BOOT] --- End Enumeration ---\n");

            gBS->FreePool(enum_handles);
        }
    }

    /* Step 1: Initialize graphics (uses g_conf_res_width/height from boot.conf) */
    post_code16(POST16_BL_GOP);
    watchdog_reset();
    serial_early_print("[BOOT] init_gop...\n");
    g_boot_info_ptr->timing.gop_start = boot_rdtsc();
    status = init_gop();
    if (EFI_ERROR(status)) {
        serial_early_print("[FAIL] init_gop\n");
        efi_print(u"[FAIL] Graphics initialization failed\r\n");
        if (g_wd_armed && gBS) {
            EFI_STATUS wd_dis = gBS->SetWatchdogTimer(0, 0, 0, (CHAR16 *)0);
            if (!EFI_ERROR(wd_dis))
                g_wd_armed = 0;
        }
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

    /* Boot-policy invocation: read \EFI\ImpossibleOS\bootentries.json,
     * decode Boot####.OptionalData, run the precedence ladder, and
     * publish the decision into boot_info v19. Pre-EBS only -- counter
     * mutation and gRT->SetVariable calls happen here while Boot
     * Services + Runtime Services are still callable. The path-changing
     * kind override (recovery / diagnostics / network / resume) lives
     * in the post-EBS boot-decision populate block where the rest of
     * the path/reason wiring is centralized. */
    watchdog_reset();
    boot_policy_invoke();

    /* A/B dual-slot selection: read the on-disk metadata and publish the
     * chosen root slot in boot_info.active_slot before handing off. Pre-EBS
     * (BlockIO + Boot Services still live). READ-ONLY -- the tries write is
     * owned by the failure-counting + atomic-write work. */
    watchdog_reset();
    select_active_slot(g_boot_device_handle);

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

    /* A/B failure counting (TODO-21 sec4): now that the kernel is loaded and we
     * are committed to booting the selected slot, charge one boot attempt to it
     * (pre-EBS, BlockIO still live). The kernel resets this to 0 at acceptance
     * (mark-boot-successful); a crash before then leaves it counted, so the next
     * boot rolls back after AB_BOOT_MAX_TRIES. No-op on non-A/B disks. */
    ab_bl_increment_tries(g_boot_device_handle);

    /* Step 4: Copy UEFI Configuration Table + find ACPI RSDP */
    post_code16(POST16_BL_RSDP);
    serial_early_print("[BOOT] copy_config_tables...\n");
    copy_config_tables();

    /* Step 4c: Retrieve TPM event log (if available) */
    retrieve_tpm_event_log();

    /* Step 4c-2: correlate our own on-disk digest with what firmware measured
     * into PCR 4. Must follow the retrieval (it reads the copied log) and stay
     * ahead of ExitBootServices (it calls HandleProtocol/LocateProtocol). */
    correlate_pcr4_measurement();

    /* Step 4d: Parse FPDT for firmware boot timing */
    parse_fpdt();

    /* Estimate TSC frequency using UEFI Stall (1ms) */
    {
        UINT64 t0 = boot_rdtsc();
        gBS->Stall(1000);  /* 1ms */
        UINT64 t1 = boot_rdtsc();
        UINT64 delta = t1 - t0;
        /* Non-monotonic rdtsc or an implausible rate (outside 100 MHz
         * through 10 GHz) publishes 0 so consumers (TIME entropy
         * record, init_us diagnostics) see unknown, not garbage. */
        if (t1 <= t0 || delta < 100000ULL || delta > 10000000ULL) {
            g_boot_info_ptr->timing.tsc_freq = 0;
            serial_early_print("[BOOT] TSC calibration rejected (implausible delta)\n");
        } else {
            g_boot_info_ptr->timing.tsc_freq = delta * 1000;  /* Hz */
        }
    }

    /* Step 4d2: Collect early firmware entropy (EFI RNG + CPU + OEM0 +
     * boot timing). Runs AFTER the TSC measurement so the TIME record
     * carries a real tsc_freq, never a zero placeholder. */
    collect_boot_entropy();

    /* Step 4e: Discover USB devices via UEFI firmware (Phase A) */
    post_code16(POST16_BL_USB_DISC);
    watchdog_reset();
    serial_early_print("[BOOT] discover_usb_devices...\n");
    discover_usb_devices();
    post_code16(POST16_BL_USB_DISC_OK);

    /* Step 4f: Allocate persistent xHCI DMA structures */
    post_code16(POST16_BL_XHCI_DMA);
    serial_early_print("[BOOT] allocate_xhci_dma...\n");
    allocate_xhci_dma();
    post_code16(POST16_BL_XHCI_DMA_OK);

    /* Step 4g: Take over xHCI controller */
    post_code16(POST16_BL_XHCI_TAKEOVER);
    serial_early_print("[BOOT] xhci_controller_takeover...\n");
    xhci_controller_takeover();
    post_code16(POST16_BL_XHCI_TAKEOVER_OK);

    /* Step 5: Get UEFI memory map */
    post_code16(POST16_BL_MEMMAP);
    serial_early_print("[BOOT] get_memory_map...\n");
    status = get_memory_map(&map_key, &mmap, &map_size, &desc_size,
                            &desc_version);
    if (EFI_ERROR(status)) {
        boot_fatal(BOOT_ERR_MMAP_GETMAP_FAIL, "GetMemoryMap failed",
                   "UEFI firmware could not provide a memory map.");
    }

    /* Validate descriptor geometry before parsing (shared helper
     * is called from BOTH this initial path AND the EBS-retry
     * path so neither can skip the guard). */
    mmap_geometry_validate(map_size, desc_size, "initial GetMemoryMap");

    fill_memory_map(mmap, map_size, desc_size);
    fill_runtime_map(mmap, map_size, desc_size);

    /* Higher-half direct map (section 2): PRE-EBS, size + AllocatePages the
     * HHDM page-table arena from the just-normalized usable map. This
     * allocation stales map_key (handled by the ExitBootServices retry loop
     * below); the leaves themselves are installed POST-EBS in
     * setup_page_tables(). Skips cleanly (arena_pages stays 0) on a machine
     * with no usable RAM to map. Sizes PDPT/PD from the UNCAPPED raw firmware
     * descriptor array (mmap), not the capped boot_info map. */
    bl_hhdm_reserve_arena(mmap, map_size, desc_size);

    /* Kernel boot stack (TODO-10 sec32): allocate the run Phase 0 and Phase 1
     * will execute on. Same pre-EBS window and the same map_key-staling
     * consequence as the HHDM arena above, which the ExitBootServices retry
     * loop below already handles. */
    bl_kstack_reserve();

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
     * Locked at 4 per TODO-03-bootloader-error-recovery.md S2 agreement. */
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

    /* Refresh the memory map immediately before the first ExitBootServices
     * attempt. bl_hhdm_reserve_arena() AllocatePages'd the arena AFTER the
     * snapshot above, which deterministically stales map_key; without this
     * refresh the first EBS attempt would ALWAYS fail on the stale key and burn
     * one of the EBS_MAX_ATTEMPTS retries meant for genuine firmware map churn.
     * No boot-services allocation occurs between here and the call below, so
     * this key stays valid for attempt 1. A refresh failure is non-fatal -- the
     * retry loop re-acquires on its own -- so fall through with the old key. */
    {
        EFI_STATUS rk = get_memory_map(&map_key, &mmap, &map_size, &desc_size,
                                       &desc_version);
        if (!EFI_ERROR(rk)) {
            mmap_geometry_validate(map_size, desc_size, "pre-EBS arena refresh");
            /* Propagate the refreshed map into boot_info: without this the
             * kernel would receive the stale pre-arena snapshot and could
             * reclaim or HHDM-map a region that became reserved/runtime since.
             * fill_memory_map/fill_runtime_map do NOT allocate (the EBS retry
             * loop below runs them between its own get_memory_map and the next
             * ExitBootServices with the same key), so map_key stays valid for
             * attempt 1. The arena is retagged RESERVED post-EBS regardless of
             * how the refreshed map now labels it. */
            fill_memory_map(mmap, map_size, desc_size);
            fill_runtime_map(mmap, map_size, desc_size);
            g_boot_info_ptr->uefi_mmap_desc_size    = (UINT32)desc_size;
            g_boot_info_ptr->uefi_mmap_desc_version = desc_version;
        } else {
            serial_early_print("[WARN] pre-EBS map refresh failed; "
                               "relying on EBS retry loop\n");
        }
    }

    post_code16(POST16_BL_EXIT_BS);
    serial_early_print("[BOOT] ExitBootServices...\n");
    g_boot_info_ptr->timing.exit_bs = boot_rdtsc();
    g_ebs_in_progress = 1;  /* Disable ConOut in boot_fatal from here */
    boot_set_section(BOOT_SECTION_BL_EBS);
    {
        int ebs_attempt;
        EFI_STATUS ebs_status = EFI_SUCCESS;
        for (ebs_attempt = 0; ebs_attempt < EBS_MAX_ATTEMPTS; ebs_attempt++) {
            ebs_status = gBS->ExitBootServices(gImageHandle, map_key);
            if (!EFI_ERROR(ebs_status))
                break;
            /* Per-attempt log captures attempt number (1-based for
             * humans), the EBS_MAX_ATTEMPTS denominator (so the log
             * tracks the constant if the policy ever changes), AND
             * the firmware status hex so a triple-retry triage can
             * distinguish "stale map_key" from "firmware unhappy
             * with this address space" without rerunning. */
            serial_early_print("[BOOT] ExitBootServices attempt ");
            serial_early_print_uint((UINT32)(ebs_attempt + 1));
            serial_early_print("/");
            serial_early_print_uint((UINT32)EBS_MAX_ATTEMPTS);
            serial_early_print(" failed: status=0x");
            serial_early_print_hex16((UINT16)((UINTN)ebs_status >> 48));
            serial_early_print_hex16((UINT16)((UINTN)ebs_status >> 32));
            serial_early_print_hex16((UINT16)((UINTN)ebs_status >> 16));
            serial_early_print_hex16((UINT16)(UINTN)ebs_status);
            serial_early_print("\n");
            /* Skip the memory-map refresh on the FINAL iteration.
             * The for-loop exits after attempt N=EBS_MAX_ATTEMPTS, so
             * a refresh after the last failed EBS does no useful work
             * and, if it fails, masks the real BOOT_ERR_EXIT_BS_FAIL
             * with a misleading BOOT_ERR_EBS_MMAP_FAIL.  Preserve
             * ebs_status for the exhaustion-fatal path below.
             *
             * Free the prior mmap pool before allocating the next
             * one.  get_memory_map() calls gBS->AllocatePool, so
             * each retry without a free would leak the previous mmap
             * buffer.  Bounded to 4 ~16 KB buffers max (~64 KB worst
             * case) but unclean -- free explicitly so post-EBS
             * firmware accounting matches the kernel's PMM
             * reservation table. */
            if (ebs_attempt + 1 < EBS_MAX_ATTEMPTS) {
                if (mmap)
                    gBS->FreePool(mmap);
                status = get_memory_map(&map_key, &mmap, &map_size,
                                        &desc_size, &desc_version);
                if (EFI_ERROR(status)) {
                    boot_fatal(BOOT_ERR_EBS_MMAP_FAIL,
                               "GetMemoryMap failed on EBS retry",
                               "Memory map refresh failed during ExitBootServices retry.");
                }
                /* Re-apply geometry guard on the refreshed map --
                 * the initial guard does NOT carry over. */
                mmap_geometry_validate(map_size, desc_size,
                                        "EBS-retry GetMemoryMap");
                fill_memory_map(mmap, map_size, desc_size);
                fill_runtime_map(mmap, map_size, desc_size);
            }
        }
        if (EFI_ERROR(ebs_status)) {
            /* Detail string uses static text rather than a runtime
             * format because boot_fatal renders a fixed buffer; the
             * actual attempt count is in the per-attempt serial log
             * above (which uses EBS_MAX_ATTEMPTS literally). */
            boot_fatal(BOOT_ERR_EXIT_BS_FAIL,
                       "ExitBootServices failed",
                       "UEFI ExitBootServices exhausted EBS_MAX_ATTEMPTS "
                       "retries; see serial log for per-attempt status.");
        }
    }
    serial_early_print("[BOOT] ExitBootServices OK\n");

    /* Append the EBS-success sentinel BEFORE the Boot-Services-gone
     * watershed.  RuntimeServices->SetVariable survives EBS, but
     * keeping the call here means the producer-side smoke pattern
     * (serial line "[BOOT] history: append seq=K src=0xFFFE") fires
     * deterministically every clean boot, so a missing line on the
     * NEXT boot means EBS itself failed (not the history write). */
    boot_history_append(BOOT_SECTION_EBS_OK, 0);

    /* === NO MORE UEFI Boot Services CALLS FROM HERE === */

    /* Step 7: Set up page tables */
    post_code16(POST16_BL_PAGE_TABLES);
    serial_early_print("[BOOT] setup_page_tables...\n");
    setup_page_tables();

    /* Capability negotiation: advertise what this bootloader actually
     * produced. caps_required is left at 0 (this bootloader does not
     * demand any kernel-side feature beyond what BOOT_INFO_VERSION
     * already gates). For every known capability bit
     * we either set it in caps_present (feature was populated) or in
     * caps_degraded (producer intentionally skipped or could not
     * supply), but never both -- the kernel validator rejects the
     * overlap. */
    {
        UINT64 present  = 0;
        UINT64 degraded = 0;

        /* Typed payload descriptor array -- always published, even
         * when the packed prefix is empty (count=0 is still valid). */
        present |= BOOT_CAP_PAYLOAD_DESCRIPTORS;

        /* UEFI runtime services */
        if (g_boot_info_ptr->uefi_rt_available)
            present  |= BOOT_CAP_RUNTIME_SERVICES;
        else
            degraded |= BOOT_CAP_RUNTIME_SERVICES;

        /* Secure boot state: `secure_boot_enabled` is kernel-populated
         * AFTER handoff (the kernel queries the UEFI RT variable itself
         * during Phase 0). The bootloader has not yet established the
         * field, so it MUST advertise the capability as degraded here;
         * kernel-side refinement (promoting to caps_present once the
         * variable read succeeds) is a separate follow-up in the
         * capability-consumer path. */
        degraded |= BOOT_CAP_SECURE_BOOT_STATE;

        /* TPM measured-boot event log. Kernel consumers treat the log
         * as valid only when BOTH the pointer AND the size are
         * nonzero -- advertising present on a nonzero pointer alone
         * would let a consumer dereference a zero-length buffer. */
        if (g_boot_info_ptr->tpm_available &&
            g_boot_info_ptr->tpm_event_log != 0 &&
            g_boot_info_ptr->tpm_event_log_size != 0)
            present  |= BOOT_CAP_TPM_EVENT_LOG;
        else
            degraded |= BOOT_CAP_TPM_EVENT_LOG;

        /* USB handover: xHCI BIOS->OS ownership transfer + DMA state */
        if (g_boot_info_ptr->usb_handover_complete)
            present  |= BOOT_CAP_USB_HANDOVER;
        else
            degraded |= BOOT_CAP_USB_HANDOVER;

        /* Media role: boot device type + presence */
        if (g_boot_info_ptr->boot_media_present && g_boot_info_ptr->boot_device_type != 0)
            present  |= BOOT_CAP_MEDIA_ROLE;
        else
            degraded |= BOOT_CAP_MEDIA_ROLE;

        /* Network provenance: not populated by this bootloader yet
         * (PXE/HTTP boot path is future work). Advertise as degraded so
         * downstream consumers can pick a safe fallback. */
        degraded |= BOOT_CAP_NETWORK_PROVENANCE;

        /* Resume metadata (S4 hibernation): not populated yet. */
        degraded |= BOOT_CAP_RESUME_METADATA;

        /* Alternate protocol adapter: the bit advertises presence of a
         * non-native protocol shim (Multiboot2 / PXE / HTTP boot /
         * Secure Launch) between firmware and kernel. This bootloader
         * is native UEFI, so no adapter was interposed -- the feature
         * is absent by design. Classify as degraded; a real adapter
         * (TODO-08) would set this bit in caps_present instead. */
        degraded |= BOOT_CAP_ALT_PROTOCOL_ADAPTER;

        g_boot_info_ptr->caps_required = 0ull;
        g_boot_info_ptr->caps_present  = present;
        g_boot_info_ptr->caps_degraded = degraded;
    }

    /* Boot-path provenance and decision record: one shared answer to
     * "what path did this boot take and why". This bootloader only
     * implements the native UEFI cold-boot path today; installer /
     * recovery / network / resume / fast-startup / diagnostic paths
     * are future work tracked in neighboring domain TODOs. The record
     * still gets populated here so Registry / BlackBox / attestation
     * consumers have a stable contract from day one. */
    {
        UINT32 src_flags = 0;
        if (g_boot_info_ptr->uefi_boot_next_valid)
            src_flags |= BOOT_SOURCE_FLAG_BOOT_NEXT_SET;
        if (g_boot_info_ptr->boot_device_removable)
            src_flags |= BOOT_SOURCE_FLAG_MEDIA_REMOVABLE;
        if (g_boot_info_ptr->boot_media_present)
            src_flags |= BOOT_SOURCE_FLAG_MEDIA_PRESENT;
        /* BOOT_CURRENT_MISMATCH: firmware landed on a non-primary
         * Boot#### entry without an explicit BootNext override. That
         * is exactly the "fallthrough to a secondary entry" condition
         * recovery + attestation consumers need to see; without this
         * flag those cases would be indistinguishable from an ordinary
         * cold boot. Gates:
         *   - BootOrder populated (count > 0). No list -> nothing to
         *     compare against.
         *   - BootCurrent is actually known. uefi_boot_current defaults
         *     to 0xFFFF when GetVariable(BootCurrent) is absent or
         *     fails; comparing 0xFFFF vs BootOrder[0] would false-
         *     positive on firmware that exposes BootOrder but not
         *     BootCurrent (rare but observed on OVMF builds).
         *   - Not an operator BootNext selection. If BootNext was used,
         *     BootCurrent is whatever the operator picked and
         *     "mismatch" is meaningless. */
        if (g_boot_info_ptr->uefi_boot_order_count > 0 &&
            g_boot_info_ptr->uefi_boot_current != 0xFFFF &&
            !g_boot_info_ptr->uefi_boot_next_valid &&
            g_boot_info_ptr->uefi_boot_current !=
                g_boot_info_ptr->uefi_boot_order[0]) {
            src_flags |= BOOT_SOURCE_FLAG_BOOT_CURRENT_MISMATCH;
        }
        /* Rollback / recovery / resume-invalidation / network-insecure /
         * manifest-failed / measured-boot-failed flags stay clear on
         * this loader; the paths that would set them (rollback store,
         * recovery trigger probe, hibernation image validator, PXE
         * adapter, manifest verifier, TPM attestation) are not
         * implemented yet. When they land, the producer adds the flag
         * here and picks the matching boot_reason. */

        /* Default boot decision: NORMAL + USER_SELECTED-or-NORMAL.
         * Overridden below when the media role marker selected the
         * path -- installer / recovery / diagnostics media types
         * imply a non-normal flow even on a cold boot. */
        g_boot_info_ptr->boot_path = BOOT_PATH_NORMAL;
        /* If BootNext was populated, the operator picked this entry
         * explicitly -- record USER_SELECTED so the decision log
         * carries that nuance even though the resulting path is
         * NORMAL. */
        g_boot_info_ptr->boot_reason =
            (src_flags & BOOT_SOURCE_FLAG_BOOT_NEXT_SET)
                ? BOOT_REASON_USER_SELECTED
                : BOOT_REASON_NORMAL;
        /* Media role coupling: media_role_detect_and_record() ran
         * earlier and populated boot_media_role + mismatch flag. For
         * installer / recovery / diagnostics roles the media itself
         * selected the boot flow -- override boot_path + boot_reason
         * so Registry / recovery / attestation consumers see a
         * coherent decision record. live + manufacturing keep
         * boot_path = NORMAL: their handoff IS a normal cold boot,
         * just flagged by the medium type for post-boot policy. */
        switch (g_boot_info_ptr->boot_media_role) {
            case BOOT_MEDIA_ROLE_INSTALLER:
                g_boot_info_ptr->boot_path = BOOT_PATH_INSTALLER;
                g_boot_info_ptr->boot_reason = BOOT_REASON_MEDIA_ROLE_MARKER;
                break;
            case BOOT_MEDIA_ROLE_RECOVERY:
                g_boot_info_ptr->boot_path = BOOT_PATH_RECOVERY;
                g_boot_info_ptr->boot_reason = BOOT_REASON_MEDIA_ROLE_MARKER;
                break;
            case BOOT_MEDIA_ROLE_DIAGNOSTICS:
                g_boot_info_ptr->boot_path = BOOT_PATH_DIAGNOSTIC;
                g_boot_info_ptr->boot_reason = BOOT_REASON_MEDIA_ROLE_MARKER;
                break;
            default:
                break;
        }
        /* Boot-policy ladder kind override. Runs AFTER the media-role
         * switch so a path-changing entry kind (recovery / diagnostics
         * / network / resume) wins over a NORMAL media role but a
         * non-path-changing kind (split / uki / installer / safe /
         * test) leaves the media-role decision in place. The mapping
         * is documented in docs/boot/boot-policy.md and lives as a
         * pure helper in boot_policy.c so it is unit-testable from the
         * kernel test runner. The static_asserts pin the helper's
         * raw-value outputs to the kernel boot_path_type /
         * boot_reason_code / BOOT_SOURCE_FLAG_* enums; a future enum
         * reorder triggers a build error here before the mapping can
         * silently drift. */
        _Static_assert(BOOT_POLICY_PATH_RECOVERY == BOOT_PATH_RECOVERY,
                       "boot_policy raw constant drift: PATH_RECOVERY");
        _Static_assert(BOOT_POLICY_PATH_NETWORK == BOOT_PATH_NETWORK,
                       "boot_policy raw constant drift: PATH_NETWORK");
        _Static_assert(BOOT_POLICY_PATH_RESUME == BOOT_PATH_RESUME,
                       "boot_policy raw constant drift: PATH_RESUME");
        _Static_assert(BOOT_POLICY_PATH_DIAGNOSTIC == BOOT_PATH_DIAGNOSTIC,
                       "boot_policy raw constant drift: PATH_DIAGNOSTIC");
        _Static_assert(BOOT_POLICY_REASON_USER_SELECTED == BOOT_REASON_USER_SELECTED,
                       "boot_policy raw constant drift: REASON_USER_SELECTED");
        _Static_assert(BOOT_POLICY_REASON_RECOVERY_TRIGGER == BOOT_REASON_RECOVERY_TRIGGER,
                       "boot_policy raw constant drift: REASON_RECOVERY_TRIGGER");
        _Static_assert(BOOT_POLICY_REASON_DIAGNOSTIC_REQUEST == BOOT_REASON_DIAGNOSTIC_REQUEST,
                       "boot_policy raw constant drift: REASON_DIAGNOSTIC_REQUEST");
        _Static_assert(BOOT_POLICY_SRC_FLAG_RECOVERY_TRIGGERED == BOOT_SOURCE_FLAG_RECOVERY_TRIGGERED,
                       "boot_policy raw constant drift: SRC_FLAG_RECOVERY_TRIGGERED");
        if (g_policy_selected_kind != G_POLICY_KIND_UNSET) {
            boot_policy_path_override_t ovr;
            if (boot_policy_kind_to_path(g_policy_selected_kind, &ovr)) {
                g_boot_info_ptr->boot_path = ovr.boot_path;
                g_boot_info_ptr->boot_reason = ovr.boot_reason;
                src_flags |= ovr.src_flag_add;
            }
            /* Non-path-changing kind: helper returned 0; media-role
             * override (if any) stands. */
        }
        g_boot_info_ptr->boot_source_flags = src_flags;
        /* Fallback depth: 0 on the primary path. The current loader
         * has a fallback chain for boot-device detection but does not
         * count rungs; future work threads a counter through the
         * fallback path and writes the final value here. */
        g_boot_info_ptr->boot_fallback_depth = 0u;

        /* Media role: NOTE -- the actual media_role_detect_and_record()
         * call lives EARLIER in efi_main, before ExitBootServices, so
         * gBS->LocateHandleBuffer/AllocatePool used by the BlackBox
         * sibling lookup are still valid. The default boot_media_role =
         * BOOT_MEDIA_ROLE_NORMAL was already set there; if the marker
         * was non-normal, boot_path/boot_reason were also overwritten
         * with the marker-derived decision. We document the path here
         * for the reader following the populate block top-to-bottom. */
    }

    /* Anti-rollback and security-version binding. Reads UEFI NVRAM
     * variable IPOSRequiredSecVersion (if present), sets shipped +
     * required fields, and HALTS with a fatal UEFI screen on a
     * downgrade. The check lives here (near the end of populate,
     * before the header-magic write) so all other boot_info fields
     * are already populated for a potential post-mortem read.
     *
     * Uses the shared g_impossible_os_guid for the NVRAM namespace.
     * Build-time shipped version is IPOS_KERNEL_SECURITY_VERSION
     * (defaults to 1 until a release policy raises it). */
    {
        static CHAR16 req_name[] = L"IPOSRequiredSecVersion";
        UINT32 required = 0;
        UINTN sz = sizeof(required);
        UINT32 attrs = 0;
        EFI_STATUS st = EFI_UNSUPPORTED;
        /* Expected attributes: NV | BS | RT. Must match what the
         * kernel-side writer uses in uefi_set_variable. */
        const UINT32 EXPECTED_ATTRS =
            EFI_VARIABLE_NON_VOLATILE |
            EFI_VARIABLE_BOOTSERVICE_ACCESS |
            EFI_VARIABLE_RUNTIME_ACCESS;
        int read_failed_closed = 0;

        /* Read required version via UEFI RT GetVariable. Fail-closed
         * policy: only EFI_NOT_FOUND is a legitimate first-ever-boot
         * state. Any other error, wrong size, or wrong attr set is
         * treated as an integrity failure -- we halt rather than
         * silently zeroing `required` (which would disable rollback
         * protection exactly at the trust boundary).
         *
         * BOOTX64.EFI is by definition a native UEFI bootloader; if
         * RuntimeServices or GetVariable is unavailable here, the
         * firmware is in a degraded / corrupted state we cannot trust
         * for an anti-rollback decision. Fail closed instead of
         * collapsing into the first-ever-boot path (which would let a
         * crafted RuntimeServices suppression bypass the rollback
         * floor on a machine that previously advanced it). The caps-
         * negotiation RUNTIME_SERVICES degraded bit reports the
         * runtime gap to the kernel separately; for the rollback
         * gate, missing RuntimeServices is an integrity failure. */
        if (!gST || !gST->RuntimeServices ||
            !gST->RuntimeServices->GetVariable) {
            /* Synthesize a non-NOT_FOUND error; the EFI_ERROR(st)
             * branch below latches read_failed_closed so the bootloader
             * halts rather than treating this as first-ever-boot. */
            st = EFI_INVALID_PARAMETER;
        } else {
            st = gST->RuntimeServices->GetVariable(
                req_name, &g_impossible_os_guid, &attrs, &sz, &required);
        }

        if (st == EFI_NOT_FOUND) {
            /* First-ever boot: variable absent. Accept any shipped. */
            required = 0;
        } else if (EFI_ERROR(st)) {
            /* Any OTHER error: fail closed. A transient read error
             * or a crafted malformed variable must not disable
             * enforcement. */
            read_failed_closed = 1;
        } else if (sz != sizeof(required)) {
            /* Size mismatch: NVRAM contains something other than a
             * u32. Crafted or corrupted. Fail closed. */
            read_failed_closed = 1;
        } else if ((attrs & EXPECTED_ATTRS) != EXPECTED_ATTRS) {
            /* Attributes don't include NV|BS|RT: variable was written
             * by something that doesn't follow our policy (e.g.
             * volatile write that wouldn't persist, or missing RT so
             * the kernel couldn't advance it). Fail closed. */
            read_failed_closed = 1;
        } else if (required > BOOT_SECURITY_VERSION_MAX) {
            /* Runaway counter: also fail closed. Prevents a
             * corrupted-NVRAM brick by halting with a diagnostic the
             * operator can fix from UEFI shell. */
            read_failed_closed = 1;
        }

        if (read_failed_closed) {
            post_code16(POST16_BL_ROLLBACK_REFUSE);
            serial_early_print(
                "[BOOT] ANTI-ROLLBACK: IPOSRequiredSecVersion read failed "
                "(status/size/attrs invalid) -- failing closed. ");
            /* Log EFI_STATUS as 64-bit hex so the error bit
             * (0x8000000000000000) is preserved -- narrowing to UINT32
             * drops it and makes every failure look like a benign
             * low value. `attrs` is defined as UINT32 per UEFI spec;
             * `sz` (UINTN) is 64-bit on x86_64 but the size fits
             * comfortably in 32 bits for this variable. */
            serial_early_print("status=0x");
            serial_early_print_hex64((UINT64)st);
            serial_early_print(" sz=");
            serial_early_print_uint((UINT32)sz);
            serial_early_print(" attrs=0x");
            serial_early_print_uint(attrs);
            serial_early_print(" value=");
            serial_early_print_uint(required);
            serial_early_print(
                "\n -- operator recovery: clear the variable from UEFI shell "
                "(setvar / dmpstore) or reflash with matching policy.\n");
            g_boot_info_ptr->flags |= BOOT_FLAG_ROLLBACK_READ_FAILED;
            /* boot_fatal first arg is the BOOT_ERR_* code persisted
             * to BootError NVRAM (last_boot_error consumers expect
             * BOOT_ERR_* values, NOT POST16 milestones). The POST16
             * code was emitted above via post_code16; here we record
             * the operator-visible category. */
            boot_fatal(BOOT_ERR_ROLLBACK_REFUSE,
                       "Anti-rollback read failure",
                       "IPOSRequiredSecVersion unreadable or malformed");
        }

        UINT32 shipped = (UINT32)IPOS_KERNEL_SECURITY_VERSION;
        g_boot_info_ptr->os_loader_security_version = shipped;
        g_boot_info_ptr->required_security_version  = required;

        if (shipped < required) {
            /* Refusal path: set the telemetry flag, emit POST16 +
             * serial diagnostic, render the UCS-2 rollback-specific
             * screen, persist a SEC_ROLLBACK fault record to NVRAM
             * (consumed by boot_version_blackbox_transcribe on the
             * next successful boot), and cold-reset. Does NOT jump
             * to kernel. */
            g_boot_info_ptr->flags |= BOOT_FLAG_ROLLBACK_REFUSAL;
            post_code16(POST16_BL_ROLLBACK_REFUSE);
            serial_early_print(
                "[BOOT] ANTI-ROLLBACK REFUSAL: os_loader_security_version=");
            serial_early_print_uint(shipped);
            serial_early_print(" < required_security_version=");
            serial_early_print_uint(required);
            serial_early_print(
                " -- kernel.exe is older than policy permits. Refusing to jump.\n");
            bpp_render_rollback_and_halt(shipped, required);
        }

        post_code16(POST16_BL_ROLLBACK_PASS);
    }

    /* Bootloader build identity: bake the compile-time {git_sha,
     * build_unix_time, build_label} triple into boot_info. The
     * generator at tools/boot-info-manifest/gen-loader-identity.sh
     * regenerates build/boot_loader_identity.h every build, so these
     * values reflect the exact bootloader image the firmware loaded.
     * Identity bytes are static -- never set from runtime input,
     * never modified post-write. Populated BEFORE the header magic
     * write so a stale-bootloader detection against this field is
     * race-free. */
    {
        static const UINT8 _git_sha[20] = BOOT_LOADER_GIT_SHA;
        static const char _label[] = BOOT_LOADER_BUILD_LABEL;
        UINT32 i;
        for (i = 0; i < 20; i++)
            g_boot_info_ptr->loader_identity.git_sha[i] = _git_sha[i];
        g_boot_info_ptr->loader_identity.build_unix_time =
            (UINT64)BOOT_LOADER_BUILD_TIME;
        for (i = 0; i < sizeof(g_boot_info_ptr->loader_identity.build_label); i++)
            g_boot_info_ptr->loader_identity.build_label[i] = 0;
        for (i = 0; i < sizeof(_label) - 1 &&
                    i < sizeof(g_boot_info_ptr->loader_identity.build_label) - 1;
             i++)
            g_boot_info_ptr->loader_identity.build_label[i] = _label[i];
        for (i = 0; i < sizeof(g_boot_info_ptr->loader_identity._pad); i++)
            g_boot_info_ptr->loader_identity._pad[i] = 0;
    }

    /* S15: Populate ABI header as the final step before kernel handoff.
     * Written last so any late modifications to boot_info don't overwrite it. */
    g_boot_info_ptr->header.magic   = BOOT_INFO_MAGIC;
    g_boot_info_ptr->header.version = BOOT_INFO_VERSION;
    g_boot_info_ptr->header.size    = (UINT16)sizeof(struct boot_info);

    /* Step 8: Jump to kernel! */
    post_code16(POST16_BL_KERNEL_JUMP);
    serial_early_print("[BOOT] jumping to kernel_main\n");
    g_boot_info_ptr->timing.kernel_jump = boot_rdtsc();
    jump_to_kernel(kernel_entry);

    /* Never reached */
    return EFI_SUCCESS;
}
