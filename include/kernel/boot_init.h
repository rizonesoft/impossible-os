/* ============================================================================
 * boot_init.h -- Kernel boot infrastructure: result types, subsystem readiness
 *               oracle, POST code constants, and phase macros.
 *
 * Included by every boot phase file and any driver init function that
 * participates in the formal phased init model.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* --- Boolean (freestanding -- no stdbool.h) -------------------------------- */
#ifndef __BOOT_INIT_BOOL_DEFINED
#define __BOOT_INIT_BOOL_DEFINED
typedef uint8_t bool;
#define true  ((bool)1)
#define false ((bool)0)
#endif

/* --- Boot result type ----------------------------------------------------- */

typedef enum {
    BOOT_OK       = 0,   /* Subsystem initialised successfully */
    BOOT_DEGRADED = 1,   /* Initialised with reduced capability; log + continue */
    BOOT_FATAL    = 2,   /* Cannot proceed; caller must halt or BSOD */
    BOOT_DEFERRED = 3,   /* Subsystem skipped; will init after desktop is up */
} boot_result_t;

/* --- Subsystem identifiers ------------------------------------------------ */
/*
 * One entry per logical subsystem. Ordered roughly by init sequence so
 * early-phase subsystems have low numbers -- makes the readiness dump easy
 * to read.  Add new entries *before* SUBSYS_COUNT; never reorder existing
 * entries (stable ABI for crash-dump analysis).
 */
typedef enum {
    SUBSYS_SERIAL   =  0,
    SUBSYS_PMM      =  1,
    SUBSYS_VMM      =  2,
    SUBSYS_HEAP     =  3,
    SUBSYS_KLOG     =  4,
    SUBSYS_GDT      =  5,
    SUBSYS_IDT      =  6,
    SUBSYS_ACPI     =  7,
    SUBSYS_LAPIC    =  8,
    SUBSYS_IOAPIC   =  9,
    SUBSYS_TIMER    = 10,
    SUBSYS_RTC      = 11,
    SUBSYS_FB       = 12,
    SUBSYS_VFS      = 13,
    SUBSYS_REGISTRY = 14,
    SUBSYS_SCHED    = 15,
    SUBSYS_IPC      = 16,
    SUBSYS_SMP      = 17,
    SUBSYS_EXEC     = 18,
    SUBSYS_DESKTOP  = 19,
    SUBSYS_OB       = 20,
    SUBSYS_UEFI_VARS = 21,  /* UEFI variable services (NVRAM enumeration) */
    SUBSYS_UEFI_TIME = 22,  /* UEFI RTC wall-clock seed */
    SUBSYS_SECUREBOOT = 23, /* Secure Boot state detection */
    SUBSYS_TPM      = 24,   /* TPM event log + PCR integrity */
    SUBSYS_XSAVE    = 25,   /* XSAVE/XCR0 Phase 1 finalize (TODO-09-boot S5) */
    SUBSYS_PCID     = 26,   /* CR4.PCIDE activation window (TODO-09-boot S5) */
    SUBSYS_EX       = 27,   /* Executive support runtime (Ex* primitives) */
    SUBSYS_NLS      = 28,   /* NLS table loader + invariant fallback (TODO-13 S4) */
    SUBSYS_KNF      = 29,   /* Kernel Notification Facility namespace + type */
    SUBSYS_COUNT    = 30,  /* sentinel -- keep last */
} kernel_subsys_t;

/* g_boot_info.degraded_mask is a uint32 and BOOT_TRY / kernel_subsystem_
 * apply_result() record failures via `1u << subsys`. If the enum ever grows
 * past 32 slots the shift becomes undefined and degraded subsystems silently
 * drop out of the boot-health summary. Widen degraded_mask to uint64 (and bump
 * the boot_info mirror/version) before adding the 33rd subsystem. */
_Static_assert(SUBSYS_COUNT <= 32,
    "degraded_mask is uint32: SUBSYS_COUNT must stay <= 32 or 1u<<subsys is UB");

/* --- POST code constants --------------------------------------------------
 *
 * One code per major init step. Values match common BIOS diagnostic LED
 * conventions so physical POST-code cards can decode them on real hardware.
 *
 * Phase 0 (0x10–0x2F): critical pre-interrupt init
 * Phase 1 (0x30–0x4F): platform services
 * Phase 2 (0x50–0x5F): system services
 * Phase 3 (0x60–0x6F): user platform
 */
#define POSTCODE_SERIAL_INIT    0x10
#define POSTCODE_PMM_INIT       0x20
#define POSTCODE_VMM_INIT       0x21
#define POSTCODE_HEAP_INIT      0x22
#define POSTCODE_KLOG_INIT      0x23
#define POSTCODE_CPUID_INIT     0x24
#define POSTCODE_SIMD_INIT      0x25
/* CPU security activation completion alias. Same constant as POST16_CPU_HARDEN_OK
 * (the canonical marker emitted from boot_phase0 after cpu_harden() returns);
 * named for clarity in the TODO-09 cpu-boot-sequencing activation-order section. */
#define POSTCODE_CPU_HARDEN_DONE POST16_CPU_HARDEN_OK
#define POSTCODE_BOOT_CFG       0x26
#define POSTCODE_AP_VALIDATE    0x27  /* AP feature consistency validation (TODO-09-boot S6) */
#define POSTCODE_CPU_AUDIT      0x2A  /* per-CPU register audit trail emitted (TODO-09-boot S9) */
#define POSTCODE_GDT_INIT       0x30
#define POSTCODE_IDT_INIT       0x31
#define POSTCODE_ACPI_INIT      0x32
#define POSTCODE_LAPIC_INIT     0x33
#define POSTCODE_IOAPIC_INIT    0x34
#define POSTCODE_TIMER_INIT     0x35
#define POSTCODE_DPC_INIT       0x36
#define POSTCODE_RTC_INIT       0x37
#define POSTCODE_KBD_INIT       0x38
#define POSTCODE_CR_PINNED      0x39  /* CR0/CR4 safety-bit pinning active (TODO-09-boot S7) */
#define POSTCODE_SMBIOS_INIT    0x40
#define POSTCODE_FB_INIT        0x41
#define POSTCODE_XSAVE_ENABLED  0x42  /* XSAVE/XCR0 Phase 1 finalize (TODO-09-boot S5) */
#define POSTCODE_PCID_ENABLED   0x43  /* CR4.PCIDE activation window (TODO-09-boot S5) */
#define POSTCODE_PCI_INIT       0x50
#define POSTCODE_STORAGE_INIT   0x51
#define POSTCODE_VFS_INIT       0x52
#define POSTCODE_REGISTRY_INIT  0x53
#define POSTCODE_SMP_INIT       0x54
#define POSTCODE_NET_INIT       0x55
#define POSTCODE_SCHED_INIT     0x60
#define POSTCODE_IPC_INIT       0x61
#define POSTCODE_EXEC_INIT      0x62
#define POSTCODE_DESKTOP_INIT   0x63

/* Sentinel POST codes written to UEFI NVRAM for post-mortem diagnosis */
#define POSTCODE_HV_DETECT      0x26  /* reuse BOOT_CFG slot */

/* ---- 16-bit POST code system (4-digit hex) --------------------------------
 *
 * Each phase gets 0x1000 codes.  Entry = even, exit = odd.
 * UEFI bootloader uses 0xB000 range (before kernel exists).
 * Hardware POST cards see the high byte on I/O port 0x80.
 * UEFI NVRAM stores the full 16-bit value for post-mortem. */

/* UEFI Bootloader (0xB000-0xBFFF) -- owned by src/boot/uefi/bootx64.c.
 * The bootloader defines its own POST16_BL_* constants locally and is the
 * source of truth for serial-observable [BOOT] POST 0xNNNN emissions. The
 * kernel does not emit codes in this range. See bootx64.c for the full set
 * and tools/post16-manifest/generate.sh for the manifest that feeds the
 * smoke test's required-set assertion. */

/* Kernel POST16 codes (0x0000-0x3FFF) are emitted via boot_post_write16()
 * in src/kernel/main/boot_init.c, which writes to I/O port 0x80 and the
 * framebuffer corner (post_display16()) but NOT to serial. Some of the
 * constants below are reserved for subsystems that exist but have not yet
 * been wired to call boot_post_write16() (for example POST16_WQ work
 * queues, POST16_COMPOSITOR desktop compositor, POST16_WALLPAPER asset
 * loader). They are kept as the stable contract for future wiring rather
 * than removed; add a caller rather than renumbering when the subsystem
 * gains a discrete boot-path init. Grep for the name in src/ to see
 * whether a constant is currently live or reserved. */

/* Phase 0 -- Critical Init (0x0000-0x0FFF) */
#define POST16_SERIAL           0x0010
#define POST16_SERIAL_OK        0x0011
#define POST16_UEFI_RT          0x0012  /* UEFI runtime services init */
#define POST16_UEFI_RT_OK       0x0013
#define POST16_UEFI_VARS        0x0014  /* UEFI variable enumeration */
#define POST16_UEFI_VARS_OK     0x0015
#define POST16_SECUREBOOT       0x0016  /* Secure Boot state detection */
#define POST16_SECUREBOOT_OK    0x0017
#define POST16_TPM              0x0018  /* TPM init + integrity */
#define POST16_TPM_OK           0x0019
#define POST16_PMM              0x0020
#define POST16_PMM_OK           0x0021
#define POST16_VMM              0x0030
#define POST16_VMM_OK           0x0031
#define POST16_HEAP             0x0040
#define POST16_HEAP_OK          0x0041
#define POST16_KLOG             0x0050
#define POST16_KLOG_OK          0x0051
#define POST16_KLOG_DISK        0x0052  /* Phase 2: VFS-backed klog disk enable (can hang on slow media) */
#define POST16_KLOG_DISK_OK     0x0053
#define POST16_CPUID            0x0060
#define POST16_CPUID_OK         0x0061
#define POST16_CPU_HARDEN       0x0070
#define POST16_CPU_HARDEN_OK    0x0071
#define POST16_NX_POLICY        0x0080
#define POST16_NX_POLICY_OK     0x0081
#define POST16_PAT              0x0082  /* PAT MSR init (WC at entry 1) */
#define POST16_PAT_OK           0x0083
#define POST16_SIMD             0x0090
#define POST16_SIMD_OK          0x0091

/* Phase 1 -- Platform Services (0x1000–0x1FFF) */
#define POST16_GDT              0x1000
#define POST16_GDT_OK           0x1001
#define POST16_IDT              0x1010
#define POST16_IDT_OK           0x1011
#define POST16_ACPI             0x1020
#define POST16_ACPI_OK          0x1021
#define POST16_LAPIC            0x1030
#define POST16_LAPIC_OK         0x1031
#define POST16_IOAPIC           0x1034
#define POST16_IOAPIC_OK        0x1035
#define POST16_TIMER            0x1040
#define POST16_TIMER_OK         0x1041
#define POST16_TIMER_CAL        0x1042
#define POST16_TIMER_INIT       0x1044
#define POST16_RTC              0x1050
#define POST16_RTC_OK           0x1051
#define POST16_TPM_TRANSPORT    0x1054  /* TPM2 command transport (TIS/CRB) */
#define POST16_TPM_TRANSPORT_OK 0x1055
#define POST16_CSPRNG           0x1056  /* kernel CSPRNG first seed */
#define POST16_CSPRNG_OK        0x1057
#define POST16_BOOT_SEED        0x1058  /* boot_info seed payload consume */
#define POST16_BOOT_SEED_OK     0x1059
#define POST16_KBD              0x1060
#define POST16_KBD_OK           0x1061
#define POST16_MOUSE            0x1070
#define POST16_MOUSE_OK         0x1071
#define POST16_FB               0x1080
#define POST16_FB_OK            0x1081
#define POST16_SPLASH           0x1090
#define POST16_SPLASH_OK        0x1091
#define POST16_SMBIOS           0x10A0
#define POST16_SMBIOS_OK        0x10A1
#define POST16_BOOT_TIMING      0x10B0
#define POST16_BOOT_TIMING_OK   0x10B1
#define POST16_FW_TABLES        0x10C0
#define POST16_FW_TABLES_OK     0x10C1

/* Phase 2 -- System Services (0x2000–0x2FFF) */
#define POST16_PCI              0x2000
#define POST16_PCI_OK           0x2001
#define POST16_XHCI             0x2010
#define POST16_XHCI_OK          0x2011
#define POST16_NIC              0x2020
#define POST16_NIC_OK           0x2021
#define POST16_NET              0x2030
#define POST16_NET_OK           0x2031
#define POST16_ATA              0x2040
#define POST16_ATA_OK           0x2041
#define POST16_AHCI             0x2050
#define POST16_AHCI_OK          0x2051
#define POST16_AHCI_MSI         0x2052
#define POST16_AHCI_MSI_OK      0x2053
#define POST16_VFS              0x2060
#define POST16_VFS_OK           0x2061
#define POST16_PARTITION        0x2070
#define POST16_PARTITION_OK     0x2071
#define POST16_REGISTRY         0x2080
#define POST16_REGISTRY_OK      0x2081
#define POST16_SMP              0x2090
#define POST16_SMP_OK           0x2091
#define POST16_NVME             0x20A0
#define POST16_NVME_OK          0x20A1
#define POST16_NVME_ADMIN       0x20A2
#define POST16_NVME_ADMIN_OK    0x20A3
#define POST16_NVME_IO          0x20A4
#define POST16_NVME_IO_OK       0x20A5
#define POST16_NVME_BLK         0x20A6
#define POST16_NVME_BLK_OK      0x20A7
#define POST16_OB               0x20B0
#define POST16_OB_OK            0x20B1
#define POST16_EX               0x20C0
#define POST16_EX_OK            0x20C1
#define POST16_NLS              0x20D0
#define POST16_NLS_OK           0x20D1
#define POST16_KNF              0x20E0
#define POST16_KNF_OK           0x20E1
#define POST16_QUOTA            0x20F0
#define POST16_QUOTA_OK         0x20F1

/* Phase 3 -- Desktop (0x3000–0x3FFF) */
#define POST16_SCHED            0x3000
#define POST16_SCHED_OK         0x3001
#define POST16_WQ               0x3010
#define POST16_WQ_OK            0x3011
#define POST16_IPC              0x3012
#define POST16_IPC_OK           0x3013
#define POST16_EXEC             0x3014
#define POST16_EXEC_OK          0x3015
#define POST16_FONTS            0x3020
#define POST16_FONTS_OK         0x3021
#define POST16_ICONS            0x3022
#define POST16_ICONS_OK         0x3023
#define POST16_CURSORS          0x3024
#define POST16_CURSORS_OK       0x3025
#define POST16_WALLPAPER        0x3026
#define POST16_WALLPAPER_OK     0x3027
#define POST16_DESKTOP          0x3030
#define POST16_DESKTOP_OK       0x3031
#define POST16_COMPOSITOR       0x3040

/* Deferred Init (0xD000–0xD00F) -- post-desktop non-critical subsystems */
#define POST16_DEFERRED         0xD000
#define POST16_DEFERRED_OK      0xD001
#define POST16_DEFERRED_NET     0xD002  /* network: rtl8139 + net + dhcp */
#define POST16_DEFERRED_NET_OK  0xD003
#define POST16_DEFERRED_INPUT   0xD004  /* input: virtio_input + vbox_mouse */
#define POST16_DEFERRED_INPUT_OK 0xD005

/* Boot Performance Regression (0xDC00–0xDC03) */
#define POST16_BOOTPERF         0xDC00  /* perf detection entry */
#define POST16_BOOTPERF_READ    0xDC01  /* NVRAM read of prev boot */
#define POST16_BOOTPERF_CMP     0xDC02  /* comparison done */
#define POST16_BOOTPERF_WRITE   0xDC03  /* NVRAM write of current boot */

/* Async Subsystem Init (0xDD00–0xDD03) */
#define POST16_ASYNC            0xDD00  /* async dispatch start */
#define POST16_ASYNC_AP         0xDD01  /* per-AP entry */
#define POST16_ASYNC_BARRIER    0xDD02  /* barrier wait */
#define POST16_ASYNC_DONE       0xDD03  /* all complete */

/* HHDM walker cutover (higher-half kernel relocation): the VMM stops walking
 * page tables through the bootloader identity map and starts walking them
 * through the HHDM direct map. A bad HHDM root here faults on the first walker
 * deref with the exit code as the last-seen POST16. (0xDD10-0xDD11) */
#define POST16_HHDM_WALK        0xDD10  /* kernel root -> HHDM walk pointer */
#define POST16_HHDM_WALK_OK     0xDD11  /* walk pointer installed */

/* IPI vector for async boot init work dispatch */
#define IPI_VECTOR_ASYNC_INIT   0xFC

/* Exception dispatch handler registration (0xDE30-0xDE3F) */
#define POST16_EXCEPT           0xDE30  /* except_init() entry */
#define POST16_EXCEPT_OK        0xDE31  /* all fault handlers registered */

/* Crash Dump Generation (0xDE40-0xDE4F) */
#define POST16_BUGCHECK         0xDE40  /* KeBugCheckEx entry */
#define POST16_FPU_CAPTURE      0xDE42  /* FPU/XSAVE state capture */
#define POST16_MODULE_REGISTRY  0xDE44  /* kernel module self-registration */
#define POST16_MDMP_FORMAT      0xDE46  /* MDMP binary format structs init */

/* TLS Expansion Slots (0xDF10-0xDF13) */
#define POST16_TLS_EXPAND       0xDF10  /* TLS expansion entry */
#define POST16_TLS_EXPAND_ALLOC 0xDF11  /* expansion array demand-allocated */
#define POST16_TLS_EXPAND_TEST  0xDF12  /* expansion slot 1087 test */
#define POST16_TLS_EXPAND_CLEAN 0xDF13  /* expansion cleanup */

/* Crash-Persistent Log (0xDE00-0xDE03) */
#define POST16_CRASHLOG         0xDE00  /* crash persist entry */
#define POST16_CRASHLOG_ALLOC   0xDE01  /* region reserved */
#define POST16_CRASHLOG_CHECK   0xDE02  /* recovery check */
#define POST16_CRASHLOG_DONE    0xDE03  /* recovery complete */

/* Per-Entry Context Metadata (0xDE10-0xDE13) */
#define POST16_KLOG_CTX         0xDE10  /* per-entry context init */
#define POST16_KLOG_CTX_STRUCT  0xDE11  /* struct extended */
#define POST16_KLOG_CTX_SERIAL  0xDE12  /* serial format updated */
#define POST16_KLOG_CTX_JSON    0xDE13  /* JSON format updated */

/* Sentinels (0xF000-0xFFFE) */
#define POST16_BOOT_OK          0xFF00
#define POST16_BOOT_FAILED      0xFFFE

/* Write 16-bit POST code: I/O port 0x80 + on-screen display. No NVRAM. */
void boot_post_write16(uint16_t code);

/* Write 16-bit POST code to UEFI NVRAM only. Called twice per boot:
 * 1. Early boot entry (marks "booting")  2. Boot OK (marks "succeeded").
 * Limited writes protect flash endurance (~100K cycle limit). */
void boot_post_nvram_write16(uint16_t code);

/* Read last 16-bit POST code from NVRAM.  Returns code or -1. */
int boot_post_read16(void);

/* Fault-safe RAM shadow of the last POST code (no NVRAM/firmware). Use this,
 * not boot_post_read16(), from panic/crash-collection context. */
uint16_t boot_post_last_shadow(void);

/* Convenience: write POST16 to I/O 0x80 + NVRAM + serial. */
#define POST16(code) boot_post_write16(code)

/* --- Hypervisor feature flags (stored in g_boot_info.hv_flags) ----------- */
#define HV_FLAG_TSC_ENLIGHTENMENT   (1u << 0)  /* Hyper-V TSC reference counter MSR */
#define HV_FLAG_TLBFLUSH_HYPERCALL  (1u << 1)  /* Hyper-V TLB flush hypercall */
#define HV_FLAG_APIC_FREQ_MSR       (1u << 2)  /* HV provides APIC frequency MSR */
#define HV_FLAG_KVM_STEAL_TIME      (1u << 3)  /* KVM steal time accounting */
#define HV_FLAG_VMWARE_BACKDOOR     (1u << 4)  /* VMware backdoor I/O port */
#define POSTCODE_BOOT_OK        0xFF  /* boot completed successfully */
#define POSTCODE_BOOT_FAILED    0xFE  /* boot_halt() or panic() fired */

/* --- Subsystem readiness oracle ------------------------------------------ */

/* Returns true if the subsystem completed init without BOOT_FATAL. */
bool kernel_subsystem_ready(kernel_subsys_t subsys);

/* Record the readiness state of a subsystem.  Call once per subsystem. */
void kernel_subsystem_set_ready(kernel_subsys_t subsys, bool ok);

/* Dump all subsystem states to klog(LOG_INFO).
 * Call before any halt so the serial log captures full state. */
void kernel_subsystem_dump(void);

/* Return a pointer to the canonical name of a subsystem (e.g. "PMM").
 * Returns "UNKNOWN" if subsys is out of range. Single source of truth --
 * recovery screen, klog dumps, and crash reporters all share this table. */
const char *kernel_subsystem_name(kernel_subsys_t subsys);

/* Apply a boot_result_t to BOTH readiness oracle channels in lockstep:
 *   - kernel_subsystem_set_ready(subsys, result == BOOT_OK || BOOT_DEGRADED)
 *   - g_boot_info.degraded_mask |= (1u << subsys) for any non-OK result
 * Returns true if the subsystem ended up READY (OK or DEGRADED).
 *
 * Use for non-fatal Phase 0/1/2 init wrappers where the function returns
 * a boot_result_t and the caller does NOT halt on failure. The two
 * channels gate downstream BOOT_REQUIRE checks (channel 1) and feed the
 * end-of-boot degraded summary in boot_desktop.c (channel 2).  They MUST
 * agree -- this helper guarantees they do. */
bool kernel_subsystem_apply_result(kernel_subsys_t subsys, boot_result_t r);

/* --- Boot progress tracker ----------------------------------------------- */

/* Emit "[PHASEn] step (0xNN)\n" to serial and record a TSC timestamp.
 * Safe to call from any phase; serial must be initialised (SUBSYS_SERIAL). */
void boot_progress(uint8_t phase, const char *step, uint16_t postcode);

/* --- Macros --------------------------------------------------------------- */

/*
 * BOOT_REQUIRE(subsys) -- prerequisite guard
 *
 * Placed at the top of an init function body.  If the required subsystem is
 * not ready, logs the failure and returns BOOT_FATAL to the caller.
 * Only use after SUBSYS_KLOG is ready (klog_init called).
 */
#define BOOT_REQUIRE(subsys) \
    do { \
        if (!kernel_subsystem_ready(subsys)) { \
            _boot_require_failed(#subsys); \
            return BOOT_FATAL; \
        } \
    } while (0)

/*
 * BOOT_STEP(subsys, fn) -- call an init function and record its result
 *
 * fn must have signature: boot_result_t fn(void)
 * Sets the subsystem ready ONLY for BOOT_OK or BOOT_DEGRADED.
 * BOOT_DEFERRED and BOOT_FATAL leave the subsystem NOT ready.
 * Does NOT call boot_progress -- the caller is responsible for that.
 */
#define BOOT_STEP(subsys, fn) \
    do { \
        boot_result_t _boot_step_r = (fn)(); \
        kernel_subsystem_set_ready((subsys), \
            _boot_step_r == BOOT_OK || _boot_step_r == BOOT_DEGRADED); \
    } while (0)

/*
 * BOOT_TRY(subsys, fn, name) -- non-critical subsystem init wrapper
 *
 * fn must have signature: boot_result_t fn(void) or void fn(void).
 * For void functions, wrap: BOOT_TRY(SUBSYS_X, (fn(), BOOT_OK), "name")
 *
 * On success: sets subsystem ready, logs normally.
 * On BOOT_DEGRADED/BOOT_FATAL: logs warning, sets degraded_mask bit,
 * continues boot -- never halts for non-critical subsystems.
 */
#define BOOT_TRY(subsys, fn_call, name) \
    do { \
        boot_result_t _bt_r = (fn_call); \
        /* Delegate the ready/degraded_mask mapping to the single source of \
         * truth -- it sets ready on BOOT_OK||BOOT_DEGRADED, records \
         * degraded_mask on any non-OK, and bounds-checks subsys (so the \
         * 1u<<subsys shift is never out of range). */ \
        kernel_subsystem_apply_result((subsys), _bt_r); \
        if (_bt_r != BOOT_OK) \
            klog(LOG_WARN, "boot", "%s: init result %d -- degraded", \
                 (name), (int)_bt_r); \
    } while (0)

/*
 * BOOT_ASSERT(condition, msg) -- impossible-state check
 *
 * If condition is false, logs the message and halts. Use for invariants
 * that should never be violated (e.g., "IDT must be ready before timer").
 *
 * The diagnostic uses LOG_ERROR, NOT LOG_FATAL: klog(LOG_FATAL) halts in
 * its own bare for(;;) loop and never returns, which would make the
 * boot_halt() below unreachable -- losing boot_halt's styled halt screen,
 * POST16_BOOT_FAILED write, and subsystem-readiness dump. boot_halt() is
 * the single intended boot-failure renderer for an asserted violation.
 */
#define BOOT_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            klog(LOG_ERROR, "boot", "ASSERT FAILED: %s", (msg)); \
            boot_halt(msg); \
        } \
    } while (0)

/* --- Deferred init -------------------------------------------------------- */

/* Maximum number of deferred init slots.  16 is generous -- currently only
 * network (rtl8139+net+dhcp) and optional input (virtio+vbox) are deferred. */
#define BOOT_DEFERRED_MAX 16

/* Register a subsystem for post-desktop init.  fn must have signature:
 * boot_result_t fn(void).  Returns 0 on success, -1 if array full. */
int boot_defer(const char *name, boot_result_t (*fn)(void));

/* Run all deferred inits sequentially.  Call in Phase 3 after desktop_init()
 * but before compositor_run().  Logs [DEFERRED] name +NNNms for each. */
void boot_run_deferred(void);

/* --- Async subsystem init (SMP parallel) --------------------------------- */

#define BOOT_ASYNC_MAX_STEPS 8

/* How long the group barrier waits for a worker before declaring it overrun. */
#define BOOT_ASYNC_BARRIER_MS 10000u

/* Deadlock backstop for the S27 test-only worker hold. The hold is released by
 * the BSP once it has finished deciding the degraded path, so this bound is
 * only reached when that release never arrives -- a held worker must not be
 * able to hold a boot open forever. Comfortably past barrier + quiesce so it
 * cannot fire during a scenario that IS progressing.
 *
 * It replaced a hold of exactly BOOT_ASYNC_BARRIER_MS measured from the AP's
 * own start, which could expire before the BSP even created its deadline.
 *
 * DEPENDS ON THE BSP'S TICK. The held AP spins with IF clear, so it cannot
 * advance any timer of its own; the backstop works only because
 * system_get_ticks() reads the LAPIC tick counter that the BSP advances (AP
 * LAPIC timers are masked). Arming per-CPU LAPIC timers would both skew every
 * /10 deadline computed in boot_init.c and remove this backstop's tick source,
 * so that change has to revisit this constant rather than inherit it. */
#define BOOT_ASYNC_HOLD_CAP_MS 30000u

/* How long a caller waits for a skipped worker to release its step before it
 * gives up and says so. Deliberately far shorter than the barrier: by this
 * point the worker has ALREADY overrun a 10-second deadline, so this is a grace
 * period for one that is about to finish, not a second full budget. */
#define BOOT_ASYNC_QUIESCE_MS 2000u

/* A step in an async init group */
typedef struct {
    const char    *name;
    boot_result_t (*fn)(void);
} boot_async_step_t;

/* Initialize the async init subsystem: register IPI handler.
 * Call after IDT and LAPIC are ready. */
void boot_async_init(void);

/* Per-step disposition after a group returns (TODO-10 S27). The group's worst
 * result says whether the GROUP failed; it says nothing about which individual
 * steps a caller may safely re-run on the BSP, and that is the distinction a
 * sequential fallback needs. */
typedef enum {
    /* The step ran to completion and delivered its own result -- on the BSP, on
     * an overflow pass, or on a worker that published before the barrier. */
    BOOT_ASYNC_STEP_COMPLETED = 0,
    /* The step overran the barrier deadline and its worker STILL OWNS the slot:
     * that CPU is inside the initializer right now. */
    BOOT_ASYNC_STEP_RUNNING,
    /* The step's worker faulted and parked mid-step (panic.c async isolation).
     * Nobody is executing it, but it was abandoned at an arbitrary instruction. */
    BOOT_ASYNC_STEP_POISONED,
    /* The step was never dispatched or run at all. */
    BOOT_ASYNC_STEP_UNDISPATCHED,
} boot_async_step_state_t;

/* What a sequential fallback may do with one step. */
#define BOOT_ASYNC_FALLBACK_KEEP  0   /* already done -- re-running would redo it */
#define BOOT_ASYNC_FALLBACK_RUN   1   /* safe to run on the BSP now */
#define BOOT_ASYNC_FALLBACK_SKIP  2   /* unsafe to run -- degrade this step */

/* Per-step outcome record, indexed by the caller's step index. */
typedef struct {
    boot_async_step_state_t state;
    boot_result_t           result;      /* meaningful when state == COMPLETED */
    uint32_t                worker_cpu;  /* 0 when the step did not run on an AP */
    uint32_t                dispatch_gen;/* claim generation the worker owns */
} boot_async_step_outcome_t;

typedef struct {
    uint32_t                  count;
    boot_async_step_outcome_t step[BOOT_ASYNC_MAX_STEPS];
} boot_async_outcome_t;

/* Dispatch an async init group across available APs.
 * Steps run in parallel on different CPUs. BSP runs one step too.
 * Blocks until all steps complete. Returns worst boot_result_t.
 *
 * `out` is OPTIONAL; pass NULL when the caller has no fallback path. When
 * supplied it is filled with one outcome per step, which is the ONLY safe basis
 * for deciding what a sequential fallback may re-run. */
boot_result_t boot_async_group_ex(const char *group_name,
                                  boot_async_step_t *steps, uint32_t count,
                                  boot_async_outcome_t *out);

/* Convenience wrapper: boot_async_group_ex() with no outcome record. */
boot_result_t boot_async_group(const char *group_name,
                              boot_async_step_t *steps, uint32_t count);

/* PURE decision: may the BSP run step `st` itself, now?
 *
 * `claim_word` is a LIVE read of the worker's claim taken at fallback time, not
 * at barrier time -- a worker that overran the deadline may have finished since,
 * and re-reading is what turns "we gave up waiting" into "it still owns the
 * slot". `dispatch_gen` is the generation the step was dispatched under.
 *
 *   COMPLETED + OK/DEGRADED   -> KEEP. The initializer already ran; none of the
 *                               storage initializers is idempotent (each resets
 *                               its own module globals and reprograms the
 *                               device), so re-running a success is a fresh
 *                               hazard, not a recovery.
 *   COMPLETED + FATAL/DEFERRED-> RUN. It returned a failure through its own
 *                               control flow, so it holds no lock and no CPU is
 *                               inside it; retrying is the whole point of a
 *                               fallback.
 *   RUNNING + slot still owned -> SKIP. Re-entering means two CPUs in one
 *                               controller reset.
 *   RUNNING + slot released    -> decided as COMPLETED with `result`.
 *   POISONED                   -> SKIP. Absence of a running CPU is not restart
 *                               safety: the worker was cut at an arbitrary
 *                               instruction, so device state is half-programmed
 *                               and any lock it held is abandoned.
 *   UNDISPATCHED               -> RUN.
 */
int boot_async_fallback_decision(boot_async_step_state_t st, boot_result_t result,
                                 uint32_t claim_word, uint32_t dispatch_gen);

/* PURE: 0 while the worker dispatched at `dispatch_gen` still owns the slot
 * (BUSY or RESERVED at that generation), 1 once it does not. Shared by the
 * decision above and by the live callers, so "still inside the step" has ONE
 * definition rather than a copy per caller. */
int boot_async_claim_released(uint32_t claim_word, uint32_t dispatch_gen);

/* PURE: 1 when a RELEASED slot was released by the worker itself -- i.e. its
 * `async_retired` witness is exactly SMP_ASYNC_CLAIM(IDLE, dispatch_gen) and
 * the claim word is still at that generation -- so `async_result` is that
 * worker's own published result. 0 when the release was a panic-path park
 * (OFFLINE with no witness: the worker faulted INSIDE the step and never
 * published) or when the slot has since been re-dispatched at a later
 * generation (any result there belongs to the newer dispatch).
 *
 * Released is NOT finished. The claim word reaches the same OFFLINE(gen) for a
 * worker that completed and whose CPU parked afterwards and for one cut down
 * mid-initializer; only the witness separates them, and only one is safe to
 * credit with a result. */
int boot_async_release_attributable(uint32_t claim_word, uint32_t witness,
                                    uint32_t dispatch_gen);

/* LIVE wrapper around boot_async_fallback_decision(): re-reads the worker's
 * claim and, when that shows the step released at its dispatch generation BY
 * THE WORKER ITSELF (attributable per above), substitutes the worker's
 * PUBLISHED result for the barrier's timeout sentinel before deciding. A
 * release that is NOT attributable is decided as POISONED -- SKIP -- because
 * the worker was parked at an arbitrary instruction inside the initializer.
 * Callers with a fallback path should use this rather than calling the pure
 * decision with a recorded result, which cannot distinguish a step that failed
 * from one that merely finished late, nor either from one that was cut down. */
int boot_async_step_fallback(const boot_async_step_outcome_t *o);

/* Wait, bounded, for every step still recorded RUNNING to release its slot.
 * Returns the number that did NOT, which is the number of drivers whose
 * downstream consumers (interrupt setup, device registration) cannot safely
 * run. SKIPPING a step keeps the BSP out of the initializer; it does NOT stop
 * the worker, so a caller that proceeds to touch the same driver state must
 * quiesce first or degrade. Full cooperative cancellation -- a cancel flag each
 * long initializer polls plus an acknowledgement -- is a driver-owner decision
 * tracked in 02-kernel-core/TODO-01 "Async Subsystem Init (SMP Parallel)". */
uint32_t boot_async_quiesce(const boot_async_outcome_t *out, uint32_t timeout_ms);

#ifdef KERNEL_TESTS
/* Release the worker held by `test_hold_async_cpu`. Called by the BSP once it
 * has recorded the timeout, decided the fallback, and run quiescence to its own
 * deadline -- so every stage of the degraded path observed a genuinely
 * still-running worker rather than one that happened to finish first. Safe to
 * call when nothing is held; it only sets a latch. */
void boot_async_test_release_hold(void);
#endif

/* Internal helper used by BOOT_REQUIRE -- logs via serial (klog optional). */
void _boot_require_failed(const char *subsys_name);
