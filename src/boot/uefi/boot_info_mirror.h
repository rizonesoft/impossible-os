/* ============================================================================
 * boot_info_mirror.h -- UEFI bootloader view of struct boot_info
 *
 * MIRROR of include/kernel/boot_info.h. Must match the kernel layout byte-
 * for-byte. Every field name, order, and padding stays aligned with the
 * kernel view; compile-time _Static_asserts at the bottom pin the count-
 * field offsets and size invariants.
 *
 * Includers:
 *   - src/boot/uefi/bootx64.c (via efi.h which typedefs UINT8/16/32/64)
 *   - tools/boot-info-manifest/dump-mirror.c (typedefs UINT* from stdint
 *     then includes this header) -- generates build/boot-info-abi.mirror.json
 *
 * Canonical field ownership matrix: docs/boot/boot-info-fields.md.
 * Offset/SHA-256 fingerprint manifest: build/boot-info-abi.mirror.json
 * (regenerated on every build; diffed against the kernel view).
 * ============================================================================ */

#pragma once

/* --- Boot info structure (must match kernel/boot_info.h exactly) --- */
#define BOOT_MMAP_MAX_ENTRIES 512

struct boot_mmap_entry {
    UINT64 base_addr;
    UINT64 length;
    UINT32 type;              /* simplified: 1=available, 2=reserved, 3=ACPI, 4=NVS, 5=bad */
    UINT32 uefi_memory_type;  /* original EFI_MEMORY_TYPE enum value (0 to 14) */
    UINT64 attribute;         /* UEFI memory attribute flags */
};

struct boot_framebuffer {
    UINT64  addr;
    UINT32  pitch;
    UINT32  width;
    UINT32  height;
    UINT8   bpp;
    UINT8   type;
    UINT8   pixel_format;
    UINT8   pad0;
};

#define BOOT_GOP_MODE_MAX  32

struct boot_gop_mode {
    UINT32  width;
    UINT32  height;
    UINT32  pixels_per_scanline;
    UINT8   pixel_format;
    UINT8   pad[3];
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
    UINT8   postbars;
    UINT8   test;              /* 1 = run unit tests only, then shutdown */
    UINT8   test_suite;        /* category filter: 0-8 = specific, 0xFF = all */
    UINT8   test_quiet;        /* 1 = suppress PASS lines, show FAIL + summary */
    UINT8   diag_delay;        /* seconds to pause on each diag screen (0 = skip) */
    UINT8   diag_splash;       /* 1 = show diag on splash (bare metal, no serial) */
    UINT8   deferred;          /* 1 = defer non-critical inits (default), 0 = all in-phase */
    UINT8   async_init;        /* 1 = parallel subsystem init on APs, 0 = sequential (default) */
    UINT8   crash_test;        /* 1 = trigger deliberate BSOD after desktop init */
    UINT8   ob_handle_trace;   /* 1 = log every handle alloc/free to klog */
    UINT8   config_version;    /* 0 = legacy, 1+ = versioned (S7) */
    UINT8   error_screen_test; /* 1 = call boot_fatal() before kernel load (S14) */
    /* Desktop UI test framework, headless compositor section:
     * 0 = normal display (default), 1 = headless. */
    UINT8   compositor;
    /* Multi-monitor test-matrix expected output count:
     * 0 = whatever the hardware offers, 1..3 = hard-assert exact count. */
    UINT8   test_monitors_count;
    UINT8   anti_rollback_raise;    /* anti-rollback opt-in policy */
    UINT8   _reserved[9];           /* future fields -- zero-filled by defaults */
    char    cmdline[BOOT_CONF_CMDLINE_MAX];
    UINT8   config_found;
    /* User-mode test launcher knobs (S4 of TODO-04). Mirror of kernel
     * struct boot_config; zero-init = pre-S4 behavior. */
    UINT8   tap;
    UINT8   _pad_utest[2];     /* align the UINT16 below */
    UINT16  utest_timeout_ms;
    char    utest_filter[64];
    /* Per-test isolation knob (S6 of TODO-04). */
    UINT8   utest_isolation;
    UINT8   _pad_utest2[3];
    /* CI output format knobs (S7 of TODO-04). */
    UINT8   xml;
    UINT8   json;
    /* Test-type taxonomy (S8 of TODO-04). */
    UINT16  stress_iters;
    /* Test-runner skip knobs (S10 follow-up). */
    UINT8   test_kernel_skip;
    UINT8   test_usermode_skip;
    UINT8   _pad_test_skip[2];
    UINT8   _pad[142];         /* pad to 512 bytes total (sector-aligned) */
};

/* Mirror of kernel/boot_info.h static asserts -- catches drift between
 * bootloader and kernel struct definitions at compile time. */
_Static_assert(__builtin_offsetof(struct boot_config, cmdline) == 32,
    "cmdline must be at byte offset 32 -- kernel ABI contract");
_Static_assert(__builtin_offsetof(struct boot_config, config_found) == 288,
    "config_found must be at byte offset 288 -- after cmdline[256]");
_Static_assert(__builtin_offsetof(struct boot_config, tap) == 289,
    "tap must be at byte offset 289 -- TODO-04 S4 stable ABI");
_Static_assert(__builtin_offsetof(struct boot_config, utest_timeout_ms) == 292,
    "utest_timeout_ms must be at byte offset 292 -- TODO-04 S4 stable ABI");
_Static_assert(__builtin_offsetof(struct boot_config, utest_filter) == 294,
    "utest_filter must be at byte offset 294 -- TODO-04 S4 stable ABI");
_Static_assert(__builtin_offsetof(struct boot_config, utest_isolation) == 358,
    "utest_isolation must be at byte offset 358 -- TODO-04 S6 stable ABI");
_Static_assert(__builtin_offsetof(struct boot_config, xml) == 362,
    "xml must be at byte offset 362 -- TODO-04 S7 stable ABI");
_Static_assert(__builtin_offsetof(struct boot_config, json) == 363,
    "json must be at byte offset 363 -- TODO-04 S7 stable ABI");
_Static_assert(__builtin_offsetof(struct boot_config, stress_iters) == 364,
    "stress_iters must be at byte offset 364 -- TODO-04 S8 stable ABI");
_Static_assert(__builtin_offsetof(struct boot_config, test_kernel_skip) == 366,
    "test_kernel_skip must be at byte offset 366 -- TODO-04 S10 follow-up stable ABI");
_Static_assert(__builtin_offsetof(struct boot_config, test_usermode_skip) == 367,
    "test_usermode_skip must be at byte offset 367 -- TODO-04 S10 follow-up stable ABI");
_Static_assert(sizeof(struct boot_config) == 512,
    "boot_config must be exactly 512 bytes (sector-aligned)");

#define BOOT_CONFIG_TABLE_MAX 32
#define BOOT_RT_MMAP_MAX      64

struct boot_rt_mem_entry {
    UINT64 phys_addr;
    UINT64 num_pages;
    UINT64 attribute;   /* EFI memory attributes (cache type + EFI_MEMORY_RUNTIME) */
    UINT32 type;
    UINT32 reserved;
};

/* UEFI Runtime Service function pointers -- must match kernel/boot_info.h */
struct boot_uefi_runtime {
    UINT64 get_time;
    UINT64 set_time;
    UINT64 get_variable;
    UINT64 set_variable;
    UINT64 get_next_variable_name;
    UINT64 reset_system;
    UINT64 update_capsule;
    UINT64 query_capsule_capabilities;
    UINT64 query_variable_info;
    UINT64 get_wakeup_time;
    UINT64 set_wakeup_time;
    UINT8  svam_called;
    UINT8  pad[7];
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

/* USB device structures -- must match kernel/boot_info.h */
#define BOOT_USB_MAX_DEVICES     16
#define BOOT_USB_MAX_ENDPOINTS    4

struct boot_usb_endpoint {
    UINT8   address;
    UINT8   attributes;
    UINT16  max_packet;
    UINT8   interval;
    UINT8   pad[3];
};

struct boot_usb_device {
    UINT8   active;
    UINT8   port;
    UINT8   speed;
    UINT8   device_class;
    UINT8   iface_class;
    UINT8   iface_subclass;
    UINT8   iface_protocol;
    UINT8   num_endpoints;
    UINT16  vendor_id;
    UINT16  product_id;
    UINT8   is_msc;
    UINT8   is_hid;
    UINT16  pad0;
    UINT32  block_size;
    UINT64  block_count;
    struct boot_usb_endpoint endpoints[BOOT_USB_MAX_ENDPOINTS];
};

/* xHCI controller DMA state -- must match kernel/boot_info.h */
#define BOOT_USB_MAX_SCRATCHPADS 16
#define BOOT_USB_MAX_DMA_PAGES   16

struct boot_usb_controller {
    UINT8   pci_bus;
    UINT8   pci_dev;
    UINT8   pci_func;
    UINT8   active;
    UINT64  mmio_phys;
    UINT32  mmio_size;
    UINT8   cap_length;
    UINT16  hci_version;
    UINT32  max_slots;
    UINT32  max_intrs;
    UINT32  max_ports;
    UINT32  db_offset;
    UINT32  rts_offset;
    UINT8   ac64;
    UINT8   csz;
    UINT32  max_scratchpads;
    UINT64  dcbaa_phys;
    UINT64  scratchpad_array_phys;
    UINT64  scratchpad_base_phys;
    UINT32  scratchpad_page_count;
    UINT32  scratchpad_pad;
    UINT64  cmd_ring_phys;
    UINT64  evt_ring_phys;
    UINT64  erst_phys;
    UINT64  dma_pages[BOOT_USB_MAX_DMA_PAGES];
    UINT32  dma_page_count;
    UINT32  alloc_fail_status;
    UINT32  alloc_fail_page;
};

/* ABI header -- must match kernel/boot_info.h */
/* Guarded so the stale-ABI fixture harness can override via
 * `-DBOOT_INFO_VERSION=N` / `-DBOOT_INFO_MAGIC=...` on the command
 * line. Must match the kernel-side guards in
 * include/kernel/boot_info.h. */
#ifndef BOOT_INFO_MAGIC
#define BOOT_INFO_MAGIC    0x49504F53  /* "IPOS" */
#endif
#ifndef BOOT_INFO_VERSION
/* v11 adds ESP integrity fields populated by esp_integrity_check();
 * v10 added BOOT_FLAG_INVOKED_VIA_UKI;
 * v9 added flags + os_loader/required_security_version */
#define BOOT_INFO_VERSION  11
#endif

/* Typed payload descriptor array -- must match kernel/boot_info.h. */
#define BOOT_PAYLOAD_MAX  32

/* enum boot_payload_type mirror (value-for-value identical to
 * include/kernel/boot_info.h). C enums carry host ABI; we use plain
 * #defines here so bootloader UEFI code can reference the values. */
#define BOOT_PAYLOAD_NONE              0u
#define BOOT_PAYLOAD_MODULE            1u
#define BOOT_PAYLOAD_INITRD            2u
#define BOOT_PAYLOAD_RECOVERY_IMAGE    3u
#define BOOT_PAYLOAD_HIBERNATION_META  4u
#define BOOT_PAYLOAD_TPM_EVENT_LOG     5u
#define BOOT_PAYLOAD_NETWORK_CONFIG    6u
#define BOOT_PAYLOAD_RANDOM_SEED       7u
#define BOOT_PAYLOAD_USB_HANDOVER      8u

/* BOOT_PAYLOAD_FLAG_* mirror */
#define BOOT_PAYLOAD_FLAG_VALID        (1u << 0)
#define BOOT_PAYLOAD_FLAG_CHECKSUMMED  (1u << 1)
#define BOOT_PAYLOAD_FLAG_REQUIRED     (1u << 2)
#define BOOT_PAYLOAD_FLAG_RESERVED     (1u << 3)

/* enum boot_payload_producer mirror */
#define BOOT_PRODUCER_NONE          0u
#define BOOT_PRODUCER_UEFI          1u
#define BOOT_PRODUCER_MULTIBOOT2    2u
#define BOOT_PRODUCER_KERNEL_TEST   3u

/* BOOT_CAP_* mirror -- bitmasks value-for-value identical to
 * include/kernel/boot_info.h. Bootloader populates caps_required /
 * caps_present / caps_degraded using these. */
#define BOOT_CAP_PAYLOAD_DESCRIPTORS  (1ull << 0)
#define BOOT_CAP_RUNTIME_SERVICES     (1ull << 1)
#define BOOT_CAP_SECURE_BOOT_STATE    (1ull << 2)
#define BOOT_CAP_TPM_EVENT_LOG        (1ull << 3)
#define BOOT_CAP_USB_HANDOVER         (1ull << 4)
#define BOOT_CAP_MEDIA_ROLE           (1ull << 5)
#define BOOT_CAP_NETWORK_PROVENANCE   (1ull << 6)
#define BOOT_CAP_RESUME_METADATA      (1ull << 7)
#define BOOT_CAP_ALT_PROTOCOL_ADAPTER (1ull << 8)

/* Boot-path provenance enums and flag bits (v8). Mirror of the
 * enum boot_path_type / boot_reason_code / BOOT_SOURCE_FLAG_* block in
 * include/kernel/boot_info.h. Bootloader populates boot_path /
 * boot_reason / boot_source_flags / boot_fallback_depth before
 * kernel handoff. */
#define BOOT_PATH_NORMAL        0u
#define BOOT_PATH_INSTALLER     1u
#define BOOT_PATH_RECOVERY      2u
#define BOOT_PATH_NETWORK       3u
#define BOOT_PATH_RESUME        4u
#define BOOT_PATH_FAST_STARTUP  5u
#define BOOT_PATH_DIAGNOSTIC    6u

#define BOOT_REASON_NORMAL              0u
#define BOOT_REASON_USER_SELECTED       1u
#define BOOT_REASON_ROLLBACK            2u
#define BOOT_REASON_RESUME_VALIDATED    3u
#define BOOT_REASON_RESUME_INVALIDATED  4u
#define BOOT_REASON_NETWORK_INSECURE    5u
#define BOOT_REASON_MANIFEST_FAILURE    6u
#define BOOT_REASON_MEASURED_BOOT_FAIL  7u
#define BOOT_REASON_RECOVERY_TRIGGER    8u
#define BOOT_REASON_FAST_STARTUP_HIT    9u
#define BOOT_REASON_DIAGNOSTIC_REQUEST  10u
#define BOOT_REASON_FALLBACK            11u

#define BOOT_SOURCE_FLAG_BOOT_NEXT_SET            (1u << 0)
#define BOOT_SOURCE_FLAG_BOOT_CURRENT_MISMATCH    (1u << 1)
#define BOOT_SOURCE_FLAG_MEDIA_REMOVABLE          (1u << 2)
#define BOOT_SOURCE_FLAG_MEDIA_PRESENT            (1u << 3)
#define BOOT_SOURCE_FLAG_ROLLBACK_TRIGGERED       (1u << 4)
#define BOOT_SOURCE_FLAG_RECOVERY_TRIGGERED       (1u << 5)
#define BOOT_SOURCE_FLAG_RESUME_INVALIDATED       (1u << 6)
#define BOOT_SOURCE_FLAG_NETWORK_INSECURE         (1u << 7)
#define BOOT_SOURCE_FLAG_MANIFEST_FAILED          (1u << 8)
#define BOOT_SOURCE_FLAG_MEASURED_BOOT_FAILED     (1u << 9)

#define BOOT_FALLBACK_DEPTH_MAX  16

/* Anti-rollback and security-version binding (v9). Mirror of the
 * BOOT_FLAG_* bits + the IPOSRequiredSecVersion NVRAM variable
 * contract in include/kernel/boot_info.h. */
#define BOOT_FLAG_ROLLBACK_REFUSAL    (1u << 0)
#define BOOT_FLAG_ROLLBACK_READ_FAILED (1u << 1)
#define BOOT_FLAG_WARM_UPDATE         (1u << 2)
/* BOOT_FLAG_INVOKED_VIA_UKI (v10): bootloader walked LoadedImage's PE
 * section table, found a `.linux` section, and used the embedded
 * kernel image instead of loading `\\kernel.exe` from the ESP. UKI
 * format per UAPI Group spec; whole-chain Secure Boot signature. */
#define BOOT_FLAG_INVOKED_VIA_UKI     (1u << 3)
#define BOOT_FLAG_MASK_KNOWN \
    (BOOT_FLAG_ROLLBACK_REFUSAL | BOOT_FLAG_ROLLBACK_READ_FAILED | \
     BOOT_FLAG_WARM_UPDATE | BOOT_FLAG_INVOKED_VIA_UKI)

/* Warm-kernel-update (section 14). Mirror of the continuation-flag
 * bits + discriminator. Bootloader today never writes
 * BOOT_PAYLOAD_WARM_UPDATE_STATE descriptors (the producer is the
 * outgoing kernel during a live update, not the UEFI bootloader).
 * Defines live here for ABI-surface completeness so a future outgoing-
 * kernel payload producer can include this header. */
#define BOOT_MMAP_WARM_UPDATE                   15u
#define BOOT_PAYLOAD_WARM_UPDATE_STATE          9u
#define BOOT_WARM_UPDATE_CONT_PAGE_TABLES       (1u << 8)
#define BOOT_WARM_UPDATE_CONT_SCHEDULER_QUIESCED (1u << 9)
#define BOOT_WARM_UPDATE_CONT_VFS_WRITEBACK      (1u << 10)
#define BOOT_WARM_UPDATE_CONT_FD_TABLE           (1u << 11)
#define BOOT_WARM_UPDATE_CONT_OBJECT_HANDLES     (1u << 12)
#define BOOT_WARM_UPDATE_CONT_HW_QUEUES          (1u << 13)
#define BOOT_WARM_UPDATE_CONT_MASK_KNOWN                                   \
    (BOOT_WARM_UPDATE_CONT_PAGE_TABLES       |                             \
     BOOT_WARM_UPDATE_CONT_SCHEDULER_QUIESCED |                            \
     BOOT_WARM_UPDATE_CONT_VFS_WRITEBACK      |                            \
     BOOT_WARM_UPDATE_CONT_FD_TABLE           |                            \
     BOOT_WARM_UPDATE_CONT_OBJECT_HANDLES     |                            \
     BOOT_WARM_UPDATE_CONT_HW_QUEUES)
#define BOOT_SECURITY_VERSION_MAX   0x7FFFFFFFu

/* Build-time security version baked into the shipping kernel.exe.
 * Override via Makefile -DIPOS_KERNEL_SECURITY_VERSION=N when cutting
 * a release that requires the NVRAM counter to advance. Default 1 so
 * an absent NVRAM variable (first-ever boot, required=0) still passes
 * the shipped >= required gate. */
#ifndef IPOS_KERNEL_SECURITY_VERSION
#define IPOS_KERNEL_SECURITY_VERSION  1u
#endif

struct boot_payload_desc {
    UINT32 type;         /* enum boot_payload_type */
    UINT32 flags;        /* BOOT_PAYLOAD_FLAG_* bitmask */
    UINT64 phys_start;
    UINT64 length;
    UINT64 alignment;
    UINT64 checksum;
    UINT32 producer_id;  /* enum boot_payload_producer */
    UINT32 _reserved;
};

_Static_assert(sizeof(struct boot_payload_desc) == 48,
    "boot_payload_desc ABI size pinned at 48 bytes -- kernel mirror");

struct boot_info_header {
    UINT32 magic;
    UINT16 version;
    UINT16 size;
};

struct boot_info {
    struct boot_info_header header;   /* must be at offset 0 */
    struct boot_mmap_entry  mmap[BOOT_MMAP_MAX_ENTRIES];
    UINT32  mmap_count;
    UINT8   mmap_truncated;
    UINT8   mmap_quirks;       /* 1 if any descriptors had validation warnings */
    UINT8   _mmap_pad[2];
    UINT32  mem_lower_kb;
    UINT32  mem_upper_kb;
    struct boot_framebuffer fb;
    UINT8   fb_available;
    UINT8   hidpi;          /* 1 if negotiated GOP width >= 2560 */
    UINT8   fb_pad[2];
    /* GOP mode list */
    struct boot_gop_mode gop_modes[BOOT_GOP_MODE_MAX];
    UINT32  gop_mode_count;
    UINT32  gop_mode_selected;
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
    struct boot_uefi_runtime uefi_runtime;
    /* TPM Measured Boot */
    UINT64  tpm_event_log;
    UINT32  tpm_event_log_size;
    UINT8   tpm_available;
    UINT8   tpm_version;
    UINT16  tpm_event_count;
    /* USB devices discovered before ExitBootServices */
    struct boot_usb_device usb_devices[BOOT_USB_MAX_DEVICES];
    UINT32  usb_device_count;
    UINT8   usb_discovery_ok;
    UINT8   usb_handover_complete;
    UINT8   usb_pad[2];
    /* xHCI controller DMA state */
    struct boot_usb_controller usb_controller;
    /* Boot Timing */
    struct {
        UINT64 reset_end;
        UINT64 os_loader_load_start;
        UINT64 os_loader_start_start;
        UINT64 exit_bs_entry;
        UINT64 exit_bs_exit;
        UINT8  fpdt_available;
        UINT64 bl_entry;
        UINT64 gop_start;
        UINT64 gop_end;
        UINT64 conf_start;
        UINT64 conf_end;
        UINT64 kernel_load_start;
        UINT64 kernel_load_end;
        UINT64 splash_start;
        UINT64 splash_end;
        UINT64 exit_bs;
        UINT64 kernel_jump;
        UINT64 tsc_freq;
    } timing;

    /* Serial port (probed by bootloader; 0 = no UART detected) */
    UINT16 serial_port;
    UINT8  serial_source;         /* 0=none, 1=SPCR, 2=I/O-probe */
    UINT8  _serial_pad;
    UINT32 serial_baud;           /* baud rate from SPCR (0 = use default 38400) */

    /* NVRAM boot error from previous boot; 0 = last boot OK */
    UINT32 last_boot_error;

    /* Boot device info */
    UINT8   boot_device_type;       /* 0=unknown, 1=SATA, 2=NVMe, 3=USB, 4=network */
    UINT8   _boot_dev_pad[3];
    char    boot_device_path[128];  /* UEFI device path text */

    /* UEFI boot variables */
    UINT16  uefi_boot_current;
    UINT16  uefi_boot_next;
    UINT8   uefi_boot_next_valid;
    UINT8   uefi_boot_order_count;
    UINT8   _boot_var_pad[2];
    UINT16  uefi_boot_order[16];

    /* Boot partition info */
    UINT8   boot_partition_guid[16];
    UINT8   boot_partition_style;     /* 0=unknown, 1=MBR, 2=GPT */

    /* Removable media info */
    UINT8   boot_device_removable;   /* 0=fixed, 1=removable */
    UINT8   boot_media_present;      /* 0=no, 1=yes */
    UINT8   _rem_pad;

    /* Kernel-populated fields (set after boot) */
    UINT8   secure_boot_enabled;
    UINT8   _kp_pad[3];
    UINT32  degraded_mask;
    UINT32  hv_flags;
    char    hv_vendor[16];

    /* Typed payload descriptor array. Two invariants enforced by
     * the kernel validator (see boot_payload_validate() in
     * src/kernel/main/boot_payload.c; contract lives in the kernel
     * header):
     *   (1) Packed-prefix: type != BOOT_PAYLOAD_NONE only inside
     *       [0, payload_count). An occupied slot past payload_count is
     *       rejected (BOOT_PAYLOAD_ERR_PREFIX_VIOLATED).
     *   (2) NONE-is-empty: type == BOOT_PAYLOAD_NONE MUST have every
     *       other field zero (flags, phys_start, length, alignment,
     *       checksum, producer_id, _reserved). Residual metadata in a
     *       NONE slot is rejected (BOOT_PAYLOAD_ERR_NONE_NOT_EMPTY).
     * The current UEFI bootloader satisfies both trivially: it emits
     * payload_count=0 after the full efi_memset(g_boot_info_ptr, 0,
     * sizeof(struct boot_info)) at handoff alloc time. Any future
     * producer must uphold both invariants before publishing. */
    struct boot_payload_desc payload_descriptors[BOOT_PAYLOAD_MAX];
    UINT32  payload_count;
    UINT32  payload_overflow;
    UINT64  payload_total_bytes;

    /* Capability negotiation. Contract lives in the kernel header;
     * the loader populates caps_present (bits for fields it actually
     * wrote), caps_required (bits it asserts the kernel must support),
     * and caps_degraded (bits for known capabilities it could not
     * provide). Kernel's boot_caps_validate() rejects unknown-required
     * and contradictory combinations. */
    UINT64  caps_required;
    UINT64  caps_present;
    UINT64  caps_degraded;

    /* Boot-path provenance and decision record. Contract lives in the
     * kernel header; loader populates all four before header write. */
    UINT32  boot_path;
    UINT32  boot_reason;
    UINT32  boot_source_flags;
    UINT32  boot_fallback_depth;

    /* Anti-rollback and security-version binding. Contract lives in
     * the kernel header; loader populates flags / os_loader_security
     * _version / required_security_version before header write. */
    UINT32  flags;
    UINT32  os_loader_security_version;
    UINT32  required_security_version;
    UINT32  _rollback_pad;

    /* EFI System Partition integrity gate (v11). Populated by
     * esp_integrity_check() in bootx64.c BEFORE parse_boot_conf and
     * load_kernel. The unique partition GUID lives in
     * boot_partition_guid above; here we record the size + filesystem
     * type + type-GUID match result so post-boot tools (registry seed
     * at HKLM\HARDWARE\BOOT\ESP) can identify the boot disk without
     * re-reading firmware. UKI mode populates esp_size_mb but leaves
     * the validity bits zero (split-path-only gate; UKI's trust anchor
     * is the signed PE image itself). */
    UINT32  esp_size_mb;            /* ESP partition size in MiB; 0 if unknown */
    UINT8   esp_filesystem_type;    /* 0=unknown, 1=FAT16, 2=FAT32 */
    UINT8   esp_type_guid_valid;    /* 1 if GPT type GUID matched ESP type GUID; 0 otherwise/UKI/non-GPT */
    UINT8   _esp_pad[2];            /* alignment; reserved zero */
};

/* ABI compile-time guards -- catch bootloader/kernel struct drift at build */
_Static_assert(__builtin_offsetof(struct boot_info, header) == 0,
    "boot_info_header must be at offset 0");
_Static_assert(sizeof(struct boot_info_header) == 8,
    "boot_info_header must be 8 bytes");
_Static_assert(sizeof(struct boot_info) <= 65535,
    "boot_info too large for uint16_t size field");

/* Cross-struct layout fingerprint -- MUST match include/kernel/boot_info.h.
 * If the bootloader or kernel reorders a field without the other side doing
 * the same change, the build fails on whichever side drifted. This catches
 * same-size reorders that the total-size check alone misses. Keep these in
 * sync with the kernel header; update BOTH sides atomically when adding or
 * moving fields. Per-field offset manifest (build/boot-info-abi.mirror.json)
 * catches any reorder, not just the six pinned count-field offsets. */
_Static_assert(__builtin_offsetof(struct boot_info, mmap_count) == 16392,
    "boot_info.mmap_count offset drift -- kernel + bootloader mirror out of sync");
_Static_assert(__builtin_offsetof(struct boot_info, gop_mode_count) == 16948,
    "boot_info.gop_mode_count offset drift -- kernel + bootloader mirror out of sync");
_Static_assert(__builtin_offsetof(struct boot_info, config_table_count) == 18280,
    "boot_info.config_table_count offset drift -- kernel + bootloader mirror out of sync");
_Static_assert(__builtin_offsetof(struct boot_info, rt_mmap_count) == 20352,
    "boot_info.rt_mmap_count offset drift -- kernel + bootloader mirror out of sync");
_Static_assert(__builtin_offsetof(struct boot_info, usb_device_count) == 21504,
    "boot_info.usb_device_count offset drift -- kernel + bootloader mirror out of sync");
_Static_assert(__builtin_offsetof(struct boot_info, uefi_boot_current) == 22056,
    "boot_info.uefi_boot_current offset drift -- kernel + bootloader mirror out of sync");
