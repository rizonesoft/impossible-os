/* ============================================================================
 * boot_info.h -- Parsed boot information passed to the kernel
 *
 * Field ownership (producer / consumer / first valid phase / lifetime / owning
 * roadmap / validation rule) lives in the canonical matrix at
 *   docs/boot/boot-info-fields.md
 * That doc is the single source of truth; this header keeps only the C-level
 * policy strictly required by the compiler (ABI header offsets, the static
 * asserts that pin count-field offsets, and the `struct boot_config` size
 * contract).
 *
 * Bootloader mirror of this struct lives in src/boot/uefi/boot_info_mirror.h
 * (same layout, UEFI UINT* types instead of uint*_t). Adding, removing, or
 * reordering a field requires updating BOTH headers in the same commit and
 * adding a matching F(...) line in tools/boot-info-manifest/dump-fields.inc
 * so the build-time ABI manifest catches the drift.
 *
 * Manifest: build/boot-info-abi.{kernel,mirror}.json is regenerated on every
 * `bash scripts/build.sh` and diffed by tools/boot-info-manifest/compare.sh;
 * any field / offset / size disagreement fails the build with the first
 * mismatching field named. Static asserts below remain the compile-time
 * first line of defense.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/boot_init.h"   /* boot_result_t */

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
#define BOOT_INFO_VERSION  6           /* §4: typed payload descriptor array */

/* Upper bound for pre-copy address validation: the UEFI bootloader
 * identity-maps [0, 4 GiB) with 2 MiB pages in setup_page_tables()
 * before jumping to the kernel.  Any handoff pointer above this limit
 * cannot be safely dereferenced during Phase 0 and must be rejected
 * before boot_info_validate_header() reads the magic/version/size. */
#define BOOT_INFO_EARLY_MAP_END  0x100000000ULL

struct boot_info_header {
    uint32_t magic;     /* must be BOOT_INFO_MAGIC */
    uint16_t version;   /* BOOT_INFO_VERSION */
    uint16_t size;      /* sizeof(struct boot_info) as seen by the bootloader */
};

/* S16: boot_info handoff validators.
 *
 * Split into address and header phases so pre-copy failure paths can
 * decide whether dereferencing the handoff pointer is safe before
 * logging observed header values.
 *
 * boot_info_validate_addr(p, size, max_addr) -- pure, no dereferences.
 *   Rejects NULL, addresses below 0x1000 (NULL page / BDA), misaligned
 *   pointers, sizes that cannot hold the header or exceed uint16_t, and
 *   any [p, p + size) range that wraps around or crosses max_addr.
 *   Early boot passes BOOT_INFO_EARLY_MAP_END; tests pass (uintptr_t)-1.
 *
 * boot_info_validate_header(hdr, kernel_struct_size) -- dereferences
 *   the header.  Caller MUST have confirmed the pointer is safe via
 *   boot_info_validate_addr() first.  Rejects bad magic, version not
 *   equal to BOOT_INFO_VERSION, or size != kernel_struct_size.
 *
 * boot_info_validate(p, kernel_struct_size) -- convenience for unit
 *   tests: runs boot_info_validate_addr() with max = (uintptr_t)-1 then
 *   boot_info_validate_header().  Production code in boot_phase0()
 *   calls the two phases separately so it can bound addresses to the
 *   4 GiB early identity map.
 *
 * All three functions return BOOT_OK on success, BOOT_FATAL on rejection.
 */
boot_result_t boot_info_validate_addr(const void *p,
                                      size_t kernel_struct_size,
                                      uintptr_t max_addr);

boot_result_t boot_info_validate_header(const struct boot_info_header *hdr,
                                        size_t kernel_struct_size);

boot_result_t boot_info_validate(const void *p,
                                 size_t kernel_struct_size);

/* §4 payload descriptor validator error classes. Kept near the
 * validator prototype so callers and tests do not have to forward into
 * the struct boot_info region of the header. */
enum boot_payload_error {
    BOOT_PAYLOAD_ERR_OK                  = 0,
    BOOT_PAYLOAD_ERR_COUNT_OOR           = 1,  /* payload_count > BOOT_PAYLOAD_MAX */
    BOOT_PAYLOAD_ERR_PREFIX_VIOLATED     = 2,  /* occupied slot at index >= payload_count */
    BOOT_PAYLOAD_ERR_RANGE_WRAP          = 3,  /* phys_start + length overflows uint64_t */
    BOOT_PAYLOAD_ERR_OVERLAP_BOOT_INFO   = 4,  /* overlaps the struct boot_info handoff region */
    BOOT_PAYLOAD_ERR_OVERLAP_RT_MMAP     = 5,  /* overlaps a UEFI runtime memory region */
    BOOT_PAYLOAD_ERR_OVERLAP_USB_DMA     = 6,  /* overlaps usb_controller.dma_pages[] */
    BOOT_PAYLOAD_ERR_OVERLAP_FB          = 7,  /* overlaps the linear framebuffer */
    BOOT_PAYLOAD_ERR_ALIGNMENT           = 8,  /* alignment not a power of 2 */
    BOOT_PAYLOAD_ERR_UNKNOWN_REQUIRED    = 9,  /* unknown type with FLAG_REQUIRED set */
    BOOT_PAYLOAD_ERR_UNKNOWN_FLAGS       = 10, /* unknown flag bits with FLAG_REQUIRED set */
    BOOT_PAYLOAD_ERR_TOTAL_MISMATCH      = 11, /* payload_total_bytes != recomputed sum */
    BOOT_PAYLOAD_ERR_OVERFLOW_TRUNCATED  = 12, /* payload_overflow != 0 -- producer dropped payloads */
};

/* Forward declaration so the validator prototype can reference
 * struct boot_info before its full definition appears below. */
struct boot_info;

/* Validate the typed payload descriptor array after the header copy is
 * complete and g_boot_info is populated. Runs every overlap, range,
 * packed-prefix, total-bytes, and unknown-required check documented in
 * the boot_payload_desc contract. On BOOT_FATAL the specific failure
 * class is written to *out_error (when non-NULL) and a LOG_ERROR klog
 * entry names the offending descriptor index + retained region. */
boot_result_t boot_payload_validate(const struct boot_info *info,
                                    enum boot_payload_error *out_error);

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
 *   289       1   tap                (S4: TODO-04 user-mode TAP mode)
 *   290       2   _pad_align         (align uint16_t)
 *   292       2   utest_timeout_ms
 *   294      64   utest_filter[]
 *   358     154   _pad[]
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
    /* User-mode test launcher knobs (S4 of TODO-04). All fields zero by
     * default -- a boot.conf without these keys matches the pre-S4
     * behavior exactly. */
    uint8_t  tap;              /* 1 = emit TAP (ok/not ok/1..N) lines around each binary */
    uint8_t  _pad_utest[2];    /* align the uint16_t below */
    uint16_t utest_timeout_ms; /* per-binary wall-clock timeout; 0 = default (10s) */
    char     utest_filter[64]; /* glob/literal filter; empty = run every test_*.exe */
    /* Per-test isolation (S6 of TODO-04). Default 1 (on); set to 0 in
     * boot.conf for debugging a broken cleanup hook. Production test
     * runs always isolate. */
    uint8_t  utest_isolation;  /* 0 = disable scratch-dir + Registry wipe + handle-leak */
    uint8_t  _pad_utest2[3];   /* align following struct fields */
    /* CI-friendly output formats (S7 of TODO-04). Both default 0;
     * orthogonal to tap= so a single run with tap=1 xml=1 json=1
     * emits all three formats interleaved on serial and scripts/
     * test.sh splits them into separate artifacts. */
    uint8_t  xml;              /* 1 = emit [UTEST-XML] JUnit XML lines */
    uint8_t  json;             /* 1 = emit [UTEST-JSON] lines (per-binary + summary) */
    uint8_t  _pad_utest3[2];   /* align future fields */
    /* Pad to 512 bytes total (sector-aligned). */
    uint8_t  _pad[146];
};

/* Compile-time enforcement of bootloader ABI contract.
 * If you add a field, shrink _reserved[] / _pad[] to keep stable offsets. */
_Static_assert(__builtin_offsetof(struct boot_config, cmdline) == 32,
    "cmdline must be at byte offset 32 -- bootloader ABI contract");
_Static_assert(__builtin_offsetof(struct boot_config, config_found) == 288,
    "config_found must be at byte offset 288 -- after cmdline[256]");
_Static_assert(__builtin_offsetof(struct boot_config, tap) == 289,
    "tap must be at byte offset 289 (TODO-04 S4 stable ABI)");
_Static_assert(__builtin_offsetof(struct boot_config, utest_timeout_ms) == 292,
    "utest_timeout_ms must be at byte offset 292 (TODO-04 S4 stable ABI)");
_Static_assert(__builtin_offsetof(struct boot_config, utest_filter) == 294,
    "utest_filter must be at byte offset 294 (TODO-04 S4 stable ABI)");
_Static_assert(__builtin_offsetof(struct boot_config, utest_isolation) == 358,
    "utest_isolation must be at byte offset 358 (TODO-04 S6 stable ABI)");
_Static_assert(__builtin_offsetof(struct boot_config, xml) == 362,
    "xml must be at byte offset 362 (TODO-04 S7 stable ABI)");
_Static_assert(__builtin_offsetof(struct boot_config, json) == 363,
    "json must be at byte offset 363 (TODO-04 S7 stable ABI)");
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

/* --- Typed payload descriptor array (§4) ---
 * Bootloader enumerates optional physical payloads (modules, initrd,
 * recovery image, hibernation metadata, TPM log copy, network config,
 * random seed, USB handover state) and publishes them here for kernel
 * consumers in §5, §6, §11, §13, §20, §25, §26.
 *
 * Contract:
 *   - payload_descriptors is a fixed-size array; payload_count is the
 *     packed-prefix length (a bootloader MUST NOT leave an occupied slot
 *     at index >= payload_count).  The kernel validator enforces this
 *     invariant so future consumers scanning the full array can trust
 *     that index < payload_count covers every real payload.
 *   - Unknown type enums are SKIPPED for type-specific validation so
 *     older kernels can ignore future payloads -- EXCEPT when the
 *     descriptor sets BOOT_PAYLOAD_FLAG_REQUIRED, which forces a fatal
 *     boot failure.
 *   - Every descriptor is overlap-checked against boot_info itself, the
 *     runtime memory map, USB DMA pages, and the framebuffer regardless
 *     of type -- unknown or future types still cannot stomp retained
 *     boot regions.
 */
#define BOOT_PAYLOAD_MAX  32

/* Well-known payload types. New values are safe to add; the kernel's
 * unknown-type skip rule keeps older kernels booting against newer
 * bootloaders that emit payloads this kernel does not recognize. */
enum boot_payload_type {
    BOOT_PAYLOAD_NONE              = 0,   /* empty slot */
    BOOT_PAYLOAD_MODULE            = 1,   /* generic kernel module (owner: module and initrd handoff contract) */
    BOOT_PAYLOAD_INITRD            = 2,   /* initrd / initramfs (owner: module and initrd handoff contract) */
    BOOT_PAYLOAD_RECOVERY_IMAGE    = 3,   /* recovery env image (owner: recovery partition bootloader) */
    BOOT_PAYLOAD_HIBERNATION_META  = 4,   /* hibernation metadata (owner: hibernation/resume handoff) */
    BOOT_PAYLOAD_TPM_EVENT_LOG     = 5,   /* TPM TCG event log copy (owner: TPM measured boot event log parser) */
    BOOT_PAYLOAD_NETWORK_CONFIG    = 6,   /* network boot config blob (owner: network boot provenance) */
    BOOT_PAYLOAD_RANDOM_SEED       = 7,   /* bootloader RNG seed (owner: early entropy seed handoff) */
    BOOT_PAYLOAD_USB_HANDOVER      = 8,   /* xHCI DMA state blob (owner: USB zero-delay handover DMA state) */
};

/* Descriptor flags (bitmask). New bits are ignored by older kernels if
 * not listed in BOOT_PAYLOAD_FLAG_MASK_KNOWN, which is how capability
 * negotiation (§11) will extend this surface without a version bump. */
#define BOOT_PAYLOAD_FLAG_VALID        (1u << 0)  /* 1 = descriptor is valid and should be processed */
#define BOOT_PAYLOAD_FLAG_CHECKSUMMED  (1u << 1)  /* 1 = checksum low 32 bits carry CRC-32C of payload */
#define BOOT_PAYLOAD_FLAG_REQUIRED     (1u << 2)  /* 1 = unknown type aborts boot instead of skipping */
#define BOOT_PAYLOAD_FLAG_RESERVED     (1u << 3)  /* 1 = PMM must mark this region reserved */
#define BOOT_PAYLOAD_FLAG_MASK_KNOWN   \
    (BOOT_PAYLOAD_FLAG_VALID | BOOT_PAYLOAD_FLAG_CHECKSUMMED | \
     BOOT_PAYLOAD_FLAG_REQUIRED | BOOT_PAYLOAD_FLAG_RESERVED)

/* Producer identity -- which subsystem populated this descriptor. Lets
 * consumers attribute ownership without re-deriving it from type. */
enum boot_payload_producer {
    BOOT_PRODUCER_NONE          = 0,
    BOOT_PRODUCER_UEFI          = 1,   /* src/boot/uefi/bootx64.c */
    BOOT_PRODUCER_MULTIBOOT2    = 2,   /* src/kernel/multiboot2_parse.c */
    BOOT_PRODUCER_KERNEL_TEST   = 3,   /* src/kernel/test/test_boot_info.c fixture */
};

struct boot_payload_desc {
    uint32_t type;         /* enum boot_payload_type (u32 for ABI stability) */
    uint32_t flags;        /* BOOT_PAYLOAD_FLAG_* bitmask */
    uint64_t phys_start;   /* physical address of payload */
    uint64_t length;       /* size in bytes; 0 or type=BOOT_PAYLOAD_NONE = empty slot */
    uint64_t alignment;    /* required natural alignment (power of 2); 0 = none */
    uint64_t checksum;     /* CRC-32C in low 32 bits if FLAG_CHECKSUMMED; else 0 */
    uint32_t producer_id;  /* enum boot_payload_producer */
    uint32_t _reserved;    /* pad to 48 bytes */
};

_Static_assert(sizeof(struct boot_payload_desc) == 48,
    "boot_payload_desc ABI size pinned at 48 bytes -- update mirror + manifest on change");

/* enum boot_payload_error is declared earlier alongside the validator
 * prototype so callers and tests can include this header without
 * needing the full struct boot_info definition in scope. */

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

    /* Boot device info (§3: populated by bootloader from LoadedImage) */
    uint8_t  boot_device_type;      /* 0=unknown, 1=SATA, 2=NVMe, 3=USB, 4=network */
    uint8_t  _boot_dev_pad[3];      /* alignment */
    char     boot_device_path[128]; /* UEFI device path text (DevicePathToText) */

    /* UEFI boot variables (§6: read pre-ExitBootServices) */
    uint16_t uefi_boot_current;     /* BootCurrent: firmware-selected Boot#### entry */
    uint16_t uefi_boot_next;        /* BootNext: one-shot override (0xFFFF = not set) */
    uint8_t  uefi_boot_next_valid;  /* 1 if BootNext was present */
    uint8_t  uefi_boot_order_count; /* number of valid entries in uefi_boot_order[] */
    uint8_t  _boot_var_pad[2];      /* alignment */
    uint16_t uefi_boot_order[16];   /* first 16 entries of BootOrder variable */

    /* Boot partition info (§7: extracted from device path HardDrive node) */
    uint8_t  boot_partition_guid[16]; /* raw GUID bytes (GPT) or 4-byte MBR sig in [0..3] */
    uint8_t  boot_partition_style;    /* 0=unknown, 1=MBR, 2=GPT */

    /* Removable media info (§8: from EFI_BLOCK_IO_PROTOCOL.Media) */
    uint8_t  boot_device_removable;  /* 0=fixed, 1=removable (USB/SD/external) */
    uint8_t  boot_media_present;     /* 0=no media, 1=media inserted */
    uint8_t  _rem_pad;               /* alignment */

    /* Kernel-populated fields (set after boot; never written by the bootloader) */
    uint8_t  secure_boot_enabled;   /* 1 if Secure Boot is active (uefi_secureboot_init) */
    uint8_t  _kp_pad[3];            /* alignment */
    uint32_t degraded_mask;         /* bitmask of non-critical subsystems that failed init */
    uint32_t hv_flags;              /* hypervisor feature flags (HV_FLAG_*) */
    char     hv_vendor[16];         /* hypervisor vendor string (null-terminated) */

    /* Typed payload descriptor array (§4). See boot_payload_desc above.
     * Bootloader MUST populate payload_count as the packed-prefix length;
     * the kernel validator rejects any occupied slot at index >=
     * payload_count. payload_total_bytes MUST equal the sum of length for
     * all occupied slots; the kernel validator rejects a mismatch. */
    struct boot_payload_desc payload_descriptors[BOOT_PAYLOAD_MAX];
    uint32_t payload_count;         /* 0..BOOT_PAYLOAD_MAX; packed-prefix length */
    uint32_t payload_overflow;      /* 1 if bootloader had > BOOT_PAYLOAD_MAX payloads */
    uint64_t payload_total_bytes;   /* sum of length for all occupied slots */
};

/* Compile-time enforcement of ABI header layout (S15) */
_Static_assert(__builtin_offsetof(struct boot_info, header) == 0,
    "boot_info_header must be at offset 0 -- ABI contract");
_Static_assert(sizeof(struct boot_info_header) == 8,
    "boot_info_header must be exactly 8 bytes -- ABI contract");
/* header.size is uint16_t; struct must fit so the runtime size check cannot
 * wrap.  If this fails, widen boot_info_header.size to uint32_t and bump
 * BOOT_INFO_VERSION. */
_Static_assert(sizeof(struct boot_info) <= 65535,
    "boot_info too large for uint16_t header.size field -- widen size field or trim struct");

/* S16: Cross-struct layout fingerprint.  The bootloader at
 * src/boot/uefi/bootx64.c keeps a mirror definition of struct boot_info.
 * Total-size matching alone cannot catch same-size field reorders that
 * would otherwise silently diverge the two interpretations.  Both the
 * kernel header and the bootloader mirror pin the byte offsets of the
 * five critical count fields below.  A build failure on EITHER side
 * flags drift; a cross-build mismatch of sizes still triggers the
 * runtime header.size check.  When adding fields, update BOTH sides
 * atomically.  If an offset shifts intentionally, update both asserts
 * in the same commit.  Computed once from the canonical kernel layout:
 *   mmap_count           = 16392  (after header + 512 mmap entries)
 *   gop_mode_count       = 16948
 *   config_table_count   = 18280
 *   rt_mmap_count        = 20352
 *   usb_device_count     = 21504
 */
_Static_assert(__builtin_offsetof(struct boot_info, mmap_count) == 16392,
    "boot_info.mmap_count offset drift -- update kernel + bootloader mirror");
_Static_assert(__builtin_offsetof(struct boot_info, gop_mode_count) == 16948,
    "boot_info.gop_mode_count offset drift -- update kernel + bootloader mirror");
_Static_assert(__builtin_offsetof(struct boot_info, config_table_count) == 18280,
    "boot_info.config_table_count offset drift -- update kernel + bootloader mirror");
_Static_assert(__builtin_offsetof(struct boot_info, rt_mmap_count) == 20352,
    "boot_info.rt_mmap_count offset drift -- update kernel + bootloader mirror");
_Static_assert(__builtin_offsetof(struct boot_info, usb_device_count) == 21504,
    "boot_info.usb_device_count offset drift -- update kernel + bootloader mirror");
_Static_assert(__builtin_offsetof(struct boot_info, uefi_boot_current) == 22056,
    "boot_info.uefi_boot_current offset drift -- update kernel + bootloader mirror");

/* Global boot info -- populated by multiboot2_parse() or UEFI bootloader */
extern struct boot_info g_boot_info;

/* §9: Populate HKLM\SYSTEM\Boot\Device\ from g_boot_info.
 * Called from registry_populate_defaults() after registry_init(). */
void boot_device_populate_registry(void);
