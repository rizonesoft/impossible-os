/* ============================================================================
 * acpi.h -- ACPI table parsing and power management
 *
 * Parses the RSDP → RSDT/XSDT → FADT chain to locate the PM1a control
 * register needed for clean shutdown.  Also supports reboot via the
 * keyboard controller reset or ACPI reset register.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* ---- ACPI table structures ---- */

/* Root System Description Pointer (RSDP) -- ACPI 1.0 */
struct acpi_rsdp {
    char     signature[8];   /* "RSD PTR " */
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;       /* 0 = ACPI 1.0, 2 = ACPI 2.0+ */
    uint32_t rsdt_addr;      /* Physical address of RSDT */
} __attribute__((packed));

/* Extended RSDP -- ACPI 2.0+ */
struct acpi_rsdp2 {
    struct acpi_rsdp v1;
    uint32_t length;
    uint64_t xsdt_addr;     /* Physical address of XSDT */
    uint8_t  ext_checksum;
    uint8_t  reserved[3];
} __attribute__((packed));

/* Standard ACPI table header (shared by RSDT, FADT, etc.) */
struct acpi_sdt_header {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed));

/* Root System Description Table (RSDT) -- 32-bit pointers */
struct acpi_rsdt {
    struct acpi_sdt_header header;
    uint32_t entries[];      /* Array of 32-bit physical addresses */
} __attribute__((packed));

/* Extended System Description Table (XSDT) -- 64-bit pointers */
struct acpi_xsdt {
    struct acpi_sdt_header header;
    uint64_t entries[];
} __attribute__((packed));

/* Generic Address Structure (GAS) -- ACPI 2.0+ */
struct acpi_gas {
    uint8_t  address_space;  /* 0=system memory, 1=system I/O */
    uint8_t  bit_width;
    uint8_t  bit_offset;
    uint8_t  access_size;
    uint64_t address;
} __attribute__((packed));

/* Fixed ACPI Description Table (FADT / FACP) */
struct acpi_fadt {
    struct acpi_sdt_header header;
    uint32_t firmware_ctrl;
    uint32_t dsdt;
    uint8_t  reserved1;
    uint8_t  preferred_pm_profile;
    uint16_t sci_interrupt;
    uint32_t smi_commandport;
    uint8_t  acpi_enable;
    uint8_t  acpi_disable;
    uint8_t  s4bios_req;
    uint8_t  pstate_control;
    uint32_t pm1a_event_block;
    uint32_t pm1b_event_block;
    uint32_t pm1a_control_block;   /* PM1a_CNT -- used for shutdown */
    uint32_t pm1b_control_block;
    uint32_t pm2_control_block;
    uint32_t pm_timer_block;
    uint32_t gpe0_block;
    uint32_t gpe1_block;
    uint8_t  pm1_event_length;
    uint8_t  pm1_control_length;
    uint8_t  pm2_control_length;
    uint8_t  pm_timer_length;
    uint8_t  gpe0_length;
    uint8_t  gpe1_length;
    uint8_t  gpe1_base;
    uint8_t  cstate_control;
    uint16_t worst_c2_latency;
    uint16_t worst_c3_latency;
    uint16_t flush_size;
    uint16_t flush_stride;
    uint8_t  duty_offset;
    uint8_t  duty_width;
    uint8_t  day_alarm;
    uint8_t  month_alarm;
    uint8_t  century;
    uint16_t boot_arch_flags;
    uint8_t  reserved2;
    uint32_t flags;
    struct acpi_gas reset_reg;     /* ACPI 2.0+ reset register */
    uint8_t  reset_value;
    uint16_t arm_boot_arch;
    uint8_t  fadt_minor_version;
    /* ACPI 2.0+ extended fields. OSPM must PREFER these over their 32-bit
     * counterparts whenever they are non-zero (ACPI 6.5 section 5.2.9): a DSDT
     * placed above 4 GiB is representable only here, and firmware that
     * publishes only the extended field leaves the 32-bit `dsdt` at 0. Read
     * them only after checking the FADT's declared length covers them. */
    uint64_t x_firmware_ctrl;      /* FADT offset 132 */
    uint64_t x_dsdt;               /* FADT offset 140 */
} __attribute__((packed));

/* The extended-field offsets are fixed by the spec, so pin them rather than
 * trusting that every preceding field in this mirror carries the right width. */
_Static_assert(__builtin_offsetof(struct acpi_fadt, x_firmware_ctrl) == 132,
               "FADT X_FIRMWARE_CTRL must sit at offset 132 (ACPI 6.5 table 5.9)");
_Static_assert(__builtin_offsetof(struct acpi_fadt, x_dsdt) == 140,
               "FADT X_DSDT must sit at offset 140 (ACPI 6.5 table 5.9)");

/* Minimum `header.length` a FADT must report before a given field may be read.
 * Firmware is free to publish a table shorter than the struct above (older
 * revisions genuinely are), so every accessor gates on these before touching
 * the field -- a short table is treated as "capability unknown", never read
 * past its end.
 *
 * ACPI_FADT_LEN_FLAGS is exactly the structural minimum: flags is the last
 * byte-range either bound covers. ACPI_FADT_LEN_BOOT_ARCH is deliberately two
 * bytes MORE conservative than boot_arch_flags itself needs (the field ends at
 * offset 111); that floor is the one the accessors have always enforced and it
 * is kept as-is, with the assert below pinning it to never drop BELOW the
 * structural minimum if the layout moves. */
#define ACPI_FADT_LEN_BOOT_ARCH  113
#define ACPI_FADT_LEN_FLAGS \
    (__builtin_offsetof(struct acpi_fadt, flags) + 4)

_Static_assert(ACPI_FADT_LEN_BOOT_ARCH
                   >= __builtin_offsetof(struct acpi_fadt, boot_arch_flags) + 2,
               "ACPI_FADT_LEN_BOOT_ARCH would allow a read past a short FADT");
_Static_assert(ACPI_FADT_LEN_FLAGS
                   >= __builtin_offsetof(struct acpi_fadt, flags) + 4,
               "ACPI_FADT_LEN_FLAGS would allow a read past a short FADT");

/* ---- MADT (Multiple APIC Description Table) -- for SMP ---- */

#define MAX_CPUS  16   /* maximum supported CPUs */

/* MADT header -- signature "APIC" */
struct acpi_madt {
    struct acpi_sdt_header header;
    uint32_t lapic_addr;        /* Physical address of Local APIC */
    uint32_t flags;             /* bit 0: dual-8259 legacy PICs installed */
} __attribute__((packed));

/* MADT entry types */
#define MADT_TYPE_LAPIC          0   /* Processor Local APIC */
#define MADT_TYPE_IOAPIC         1   /* I/O APIC */
#define MADT_TYPE_INT_OVERRIDE   2   /* Interrupt Source Override */
#define MADT_TYPE_NMI_SOURCE     3   /* NMI Source */
#define MADT_TYPE_LAPIC_NMI      4   /* Local APIC NMI */
#define MADT_TYPE_LAPIC_OVERRIDE 5   /* Local APIC Address Override */
#define MADT_TYPE_X2APIC         9   /* Processor Local x2APIC */

/* Common MADT entry header */
struct madt_entry_header {
    uint8_t  type;
    uint8_t  length;
} __attribute__((packed));

/* Type 0: Processor Local APIC */
struct madt_lapic {
    struct madt_entry_header header;
    uint8_t  acpi_processor_id;
    uint8_t  apic_id;           /* LAPIC ID for this CPU */
    uint32_t flags;             /* bit 0: enabled, bit 1: online-capable */
} __attribute__((packed));

/* Type 1: I/O APIC */
struct madt_ioapic {
    struct madt_entry_header header;
    uint8_t  ioapic_id;
    uint8_t  reserved;
    uint32_t ioapic_addr;       /* Physical address of I/O APIC MMIO */
    uint32_t gsi_base;          /* Global System Interrupt base */
} __attribute__((packed));

/* Type 2: Interrupt Source Override (ISA IRQ remapping) */
struct madt_int_override {
    struct madt_entry_header header;
    uint8_t  bus;               /* always 0 = ISA */
    uint8_t  source;            /* ISA IRQ number (e.g. 0 = PIT) */
    uint32_t gsi;               /* GSI that this IRQ maps to */
    uint16_t flags;             /* polarity + trigger mode */
} __attribute__((packed));

/* Type 4: Local APIC NMI */
struct madt_lapic_nmi {
    struct madt_entry_header header;
    uint8_t  acpi_processor_id; /* 0xFF = all processors */
    uint16_t flags;
    uint8_t  lint;              /* LINT# (0 or 1) */
} __attribute__((packed));

/* Per-CPU info discovered from MADT */
struct cpu_info {
    uint8_t  apic_id;
    uint8_t  acpi_id;
    uint8_t  is_bsp;            /* 1 for the bootstrap processor */
    uint8_t  enabled;           /* 1 if CPU is usable */
};

/* ---- API ---- */

/* Initialize ACPI -- parse RSDP → RSDT → FADT → MADT.
 * Returns 0 on success, -1 if ACPI tables not found. */
int  acpi_init(void);

/* Phase 2: parse \_S1_, \_S3_, \_S4_ sleep objects from DSDT.
 * Call after acpi_init() (Phase 1) has located the DSDT. */
void acpi_power_init(void);

/* Enable ACPI fixed events (power button, sleep button).
 * Call after acpi_power_init(). */
void acpi_enable_fixed_events(void);

/* Register SCI (System Control Interrupt) handler for ACPI events.
 * Call after IDT and IOAPIC are ready. Idempotent: a repeat call is ignored
 * rather than appending a second handler to the shared GSI chain. */
void acpi_register_sci(void);

/* Enter a sleep state. TODAY THIS MEANS S1 ONLY.
 *
 * S1 keeps processor context: returns 0 once the wake is confirmed through
 * WAK_STS, or -1 if the halt was released without a real sleep-state exit.
 *
 * Every other state returns -1 without touching the hardware:
 *   - S3/S4 are discovered but not enterable -- entering either would lose
 *     processor or DRAM state with no AP shutdown, device quiesce, waking
 *     vector, or hibernation image in place. Those pipelines own the lift.
 *   - S5 is soft-off and must go through acpi_shutdown(), which runs the
 *     storage durability barrier first and never returns.
 *   - an unsupported or invalid state number, or ACPI not ready.
 *
 * Also returns -1 if the firmware did not publish the state's \_Sx object. */
int acpi_enter_sleep_state(uint8_t state);

/* Returns 1 if sleep state N (1, 3, 4, or 5) is supported by the firmware,
 * meaning its \_Sx object parsed out of the DSDT. Support is a statement about
 * FIRMWARE, not about acpi_enter_sleep_state() accepting the state: S3/S4/S5
 * can report supported and still be refused by that call (see above). */
int acpi_sleep_supported(uint8_t state);

/* Get the SLP_TYPa value for a sleep state. Returns 0xFFFF if the state's
 * \_Sx object was absent or did not decode -- including for S5, which is NOT
 * given a fabricated type 0 when the firmware never declared it. */
uint16_t acpi_get_slp_typa(uint8_t state);

/* Fixed-event counts recorded by the SCI ISR. The ISR cannot log or dispatch
 * policy (it would reach disk I/O in hard-IRQ context), so it acknowledges the
 * hardware and counts; these are how a thread-level consumer learns what
 * happened. Monotonic, never reset.
 *
 * 64-bit deliberately: the consumer drains by comparing these against its own
 * watermark, and a 32-bit counter that wraps back onto a stale watermark loses
 * the whole 2^32 interval. A stuck or repeatedly reasserted SCI is exactly the
 * failure that reaches such a count, and it is the case where silently losing
 * every button press is least acceptable. */
uint64_t acpi_power_button_count(void);
uint64_t acpi_sleep_button_count(void);
uint64_t acpi_wake_event_count(void);

/* ---- Power / sleep button action policy ---------------------------------- */

/* Action codes stored in HKLM\SYSTEM\PowerControl\PowerButtonAction and
 * SleepButtonAction, matching the Windows power-button action encoding. */
#define ACPI_BTN_ACTION_IGNORE      0u
#define ACPI_BTN_ACTION_SLEEP       1u   /* S3 suspend to RAM */
#define ACPI_BTN_ACTION_HIBERNATE   2u   /* S4 suspend to disk */
#define ACPI_BTN_ACTION_SHUTDOWN    3u   /* S5 soft off */
#define ACPI_BTN_ACTION_LOCK        4u   /* lock the interactive session */
#define ACPI_BTN_ACTION_MAX         ACPI_BTN_ACTION_LOCK

/* Defaults, used ONLY when the stored value is absent, unreadable, or out of
 * range. A value that is in range but currently unactionable is NOT redirected
 * here: promoting a configured "sleep" to a shutdown would destroy the session
 * the setting exists to preserve, and for the sleep button the redirect would
 * be circular, since its own default is the S3 that was just refused. An
 * unactionable action is refused with one log line and nothing else happens. */
#define ACPI_BTN_DEFAULT_POWER      ACPI_BTN_ACTION_SHUTDOWN
#define ACPI_BTN_DEFAULT_SLEEP      ACPI_BTN_ACTION_SLEEP

/* Resolve a raw registry value to an action. `present` is zero when the value
 * was absent or unreadable. Absent and out-of-range both resolve to `fallback`;
 * every in-range value is returned unchanged. Pure. */
uint32_t acpi_btn_resolve_action(uint32_t raw, int present, uint32_t fallback);

/* Nonzero when `action` can actually be carried out on this tree. The single
 * place that decision is made, so the consumer and its tests agree. Pure. */
int acpi_btn_action_available(uint32_t action);

/* Returned by acpi_btn_plan() for a button that had no unseen events. Outside
 * the ACPI_BTN_ACTION_* range on purpose, so it can never be mistaken for one. */
#define ACPI_BTN_ACTION_NONE        0xFFFFFFFFu

/* One drain-and-decide pass over both buttons. Writes the action each button
 * should perform into *out_pwr / *out_slp, or ACPI_BTN_ACTION_NONE when that
 * button saw nothing, and advances both watermarks.
 *
 * This is the WHOLE dispatcher decision, including burst collapsing: the
 * threaded DPC is this function plus the gate plus the actuator. Split out so
 * the decision can be driven end to end from a test, which cannot raise a real
 * PM1 event or run the real actuator. Pure apart from the watermark stores. */
void acpi_btn_plan(uint64_t pwr_count, uint64_t slp_count,
                   uint64_t *pwr_seen, uint64_t *slp_seen,
                   uint32_t pwr_action, uint32_t slp_action,
                   uint32_t *out_pwr, uint32_t *out_slp);

/* Edge drain: returns how many events happened since *seen, and advances *seen
 * to `count`. Unsigned arithmetic, so it stays correct across a 64-bit wrap.
 * Pure apart from the *seen store. */
uint64_t acpi_btn_drain(uint64_t count, uint64_t *seen);

/* Single-entry gate. take() returns 1 exactly once until release() is called,
 * so a second dispatch cannot stack on an action that is already running. */
int  acpi_btn_gate_take(volatile uint32_t *gate);
void acpi_btn_gate_release(volatile uint32_t *gate);

/* The resolved actions, cached once at acpi_enable_fixed_events() time.
 *
 * They are cached rather than read per dispatch because the registry has no SMP
 * lock (src/kernel/main/boot_storage.c records this, and RegSetValueEx publishes
 * type, size and data through separate unsynchronized stores). A safety-critical
 * decision must not be made through a value that can be read torn, so the policy
 * is sampled once before the SCI is enabled and treated as immutable until a
 * synchronized registry exists. */
uint32_t acpi_power_button_action(void);
uint32_t acpi_sleep_button_action(void);

/* Carry out the configured action for one button press. PASSIVE_LEVEL only:
 * these run from the threaded DPC the SCI ISR queues, never from the ISR.
 * acpi_power_button_event() does not return when the action is shutdown. */
void acpi_power_button_event(void);
void acpi_sleep_button_event(void);

/* Test/diagnostic: button dispatches that reached an action decision. */
uint32_t acpi_btn_dispatch_count(void);

/* Test-only: run a caller-supplied table image through the real \_Sx parser /
 * the real table validator, so malformed-firmware handling is testable without
 * live firmware. Returns the same values the internal functions do. */
int acpi_parse_sleep_type_test(const void *table, char state_digit,
                               uint16_t *out_typa, uint16_t *out_typb);
int acpi_table_valid_test(const void *table, const char *sig);


/* Power off the machine via ACPI S5 sleep state.
 * Falls back to QEMU-specific port if FADT is unavailable.
 * Does not return on success. */
void acpi_shutdown(void);

/* Power off WITHOUT the (interrupt-requiring) storage quiesce -- for callers
 * already running with interrupts disabled. Does not return on success. */
void acpi_poweroff_now(void);

/* Reboot the machine via ACPI reset register or keyboard controller.
 * Runs the storage quiesce first (needs interrupts enabled). Does not return. */
void acpi_reboot(void);

/* Reset the machine WITHOUT the (interrupt-requiring) storage quiesce -- for
 * callers already running with interrupts disabled (e.g. the degraded-boot
 * recovery screen). Does not return on success. */
void acpi_reset_now(void);

/* ---- Consolidated MADT info ---- */

struct acpi_irq_override {
    uint8_t  bus_irq;       /* ISA IRQ number */
    uint32_t gsi;           /* Global System Interrupt */
    uint16_t flags;         /* polarity + trigger mode */
};

struct acpi_madt_info {
    uint32_t lapic_base;        /* LAPIC physical base (default 0xFEE00000) */
    uint32_t ioapic_base;       /* I/O APIC physical base (0 if not found) */
    uint32_t ioapic_gsi_base;   /* GSI base for this IOAPIC */
    uint32_t flags;             /* MADT flags (bit 0 = PCAT_COMPAT) */
    struct acpi_irq_override overrides[24];
    uint32_t override_count;
    uint8_t  cpu_lapic_ids[64];
    uint32_t cpu_count;
};

/* Get consolidated MADT info (populated after acpi_init). */
const struct acpi_madt_info *acpi_madt_info(void);

/* ---- SMP discovery API ---- */

/* Number of CPUs discovered in the MADT (1 = single-core / no MADT) */
uint32_t acpi_get_cpu_count(void);

/* Get CPU info by index (0 .. acpi_get_cpu_count()-1). Returns NULL if invalid. */
const struct cpu_info *acpi_get_cpu_info(uint32_t index);

/* LAPIC physical base address from MADT (default 0xFEE00000) */
uint32_t acpi_get_lapic_base(void);

/* I/O APIC physical base address (0 if not found) */
uint32_t acpi_get_ioapic_base(void);

/* Number of interrupt source overrides found */
uint32_t acpi_get_override_count(void);

/* Get an interrupt source override by index. Returns NULL if invalid. */
const struct madt_int_override *acpi_get_override(uint32_t index);

/* Returns 1 if MADT reports dual-8259 legacy PICs (PCAT_COMPAT=1).
 * Returns 0 for APIC-only platforms (Hyper-V Gen 2, hardware-reduced ACPI). */
uint8_t acpi_pcat_compat(void);

/* ---- Timer calibration helpers ---- */

/* HPET MMIO base address from the ACPI "HPET" table.
 * Returns 0 if no HPET table found. */
uint64_t acpi_get_hpet_base(void);

/* PM Timer I/O port from FADT PM_TMR_BLK (offset 76).
 * Returns 0 if FADT not available or PM Timer block not set. */
uint16_t acpi_get_pmtimer_port(void);

/* Returns 1 if PM Timer is 32-bit (FADT flags bit 8: TMR_VAL_EXT).
 * Returns 0 for 24-bit PM Timer. */
int acpi_pmtimer_is_32bit(void);

/* Current PM Timer counter value, masked to its real width (24 or 32 bits),
 * glitch-filtered (median of 3 back-to-back reads -- some broken chipsets latch
 * a wrong value on a single read). Returns 0 when no PM Timer block exists
 * (HW-reduced ACPI). Use for one-shot / low-rate reads (LAPIC calibration,
 * CSPRNG entropy, the mono_clock PMTMR epoch advance + precise mono_ns). */
uint32_t acpi_pmtimer_read_value(void);

/* Returns 1 if FADT flags bit 20 (HW_REDUCED_ACPI) is set.
 * When set, legacy devices (PIT, PIC, RTC) do NOT exist. */
int acpi_hw_reduced(void);

/* ---- FADT IAPC_BOOT_ARCH flags (ACPI 6.0, Table 5-11) ---- */

/* Returns 1 if FADT IAPC_BOOT_ARCH indicates an i8042 controller is present.
 * Check before ANY port 0x60/0x64 access (keyboard, mouse). */
int acpi_has_8042(void);

/* Returns 1 if CMOS RTC is present (bit 5 NOT set).
 * Check before any port 0x70/0x71 access. */
int acpi_has_cmos_rtc(void);

/* Returns the CMOS register index that holds the RTC century byte, or 0 when
 * the FADT advertises no century register (do NOT read a hardcoded 0x32 then). */
uint8_t acpi_rtc_century_index(void);

/* Returns 1 if MSI is supported (bit 3 NOT set).
 * Check before enabling MSI on PCI devices. */
int acpi_msi_supported(void);

/* Returns 1 if VGA is present (bit 2 NOT set). */
int acpi_has_vga(void);

/* ---- Pure evaluators over an explicit FADT ----
 *
 * Each accessor above is a one-line wrapper that passes the FADT the ACPI
 * parse latched at init. These take the table explicitly and hold ALL of the
 * decision logic: the hardware-reduced override, the short-table bounds, and
 * the inverted-bit conventions. Every one is a pure function of its argument
 * with no global reads, so a caller (notably the bare-metal test suite) can
 * evaluate the policy against a synthetic FADT -- a hardware-reduced table, a
 * truncated table -- which is otherwise unreachable, because the real table
 * pointer is latched only inside acpi_init() and no platform this runs on
 * publishes a hardware-reduced FADT.
 *
 * A NULL `fadt` means "no FADT was found" and yields the same legacy-present
 * defaults the accessors return before ACPI parsing. */
/* The FADT the ACPI parse latched at init, or NULL when none was accepted.
 * Read-only: the returned table is firmware memory the kernel never writes.
 * Exists so a caller can evaluate the pure predicates below against exactly
 * the table the public accessors use -- notably to pin that each accessor
 * still forwards to ITS OWN evaluator. */
const struct acpi_fadt *acpi_get_fadt(void);

int acpi_fadt_hw_reduced(const struct acpi_fadt *fadt);
int acpi_fadt_has_8042(const struct acpi_fadt *fadt);
int acpi_fadt_has_cmos_rtc(const struct acpi_fadt *fadt);
int acpi_fadt_msi_supported(const struct acpi_fadt *fadt);
int acpi_fadt_has_vga(const struct acpi_fadt *fadt);

/* ---- Win32 GetSystemFirmwareTable / EnumSystemFirmwareTables surface ----
 *
 * The Win32 facade in NtQuerySystemInformation(SystemFirmwareTableInformation)
 * exposes ACPI tables to user-mode, so every read MUST validate
 * checksums and lengths. The kernel's internal find_acpi_table walks
 * pointers without per-entry validation -- fine for one-shot boot
 * parsing, NOT fine for repeatable user-triggered reads.
 *
 * Both accessors return 0 on failure (table not found, checksum
 * mismatch, geometry rejected) so the syscall path can return
 * STATUS_NOT_FOUND cleanly. Pointers handed to the caller are
 * firmware-mapped; the caller MUST memcpy into its own buffer before
 * exposing to user-mode (the firmware pointer never crosses the
 * syscall boundary).
 */

/* Enumerate the 4-byte signatures of every validated ACPI SDT.
 * out_sigs[] receives up to max_count uint32_t signatures (little-endian
 * encoded "FACP", "APIC", etc.). out_total receives the actual count
 * found (may exceed max_count -- caller can re-call with a larger
 * buffer). Returns 1 on success, 0 if ACPI was not parsed or the
 * RSDT/XSDT root is invalid. Duplicates (multiple SSDT) ARE included
 * to match Win32 EnumSystemFirmwareTables semantics. */
int acpi_enumerate_signatures(uint32_t *out_sigs, uint32_t max_count,
                               uint32_t *out_total);

/* Look up a single ACPI SDT by signature (4-byte ASCII packed
 * little-endian, e.g. 'FACP' as 0x50434146). Validates the table
 * header length AND checksum before returning. out_addr / out_size
 * are the firmware-mapped pointer + length. Returns 1 on success,
 * 0 if not found or any validation failed. */
int acpi_get_raw_table(uint32_t signature, const uint8_t **out_addr,
                        uint32_t *out_size);

/* Returns 1 if acpi_init() succeeded (RSDP checksum + FADT located), 0
 * otherwise. Use this instead of `g_boot_info.acpi_available` when a
 * caller must avoid touching ACPI roots after a failed acpi_init -- the
 * boot_info bit only reflects "RSDP pointer present" not "validated". */
int acpi_is_ready(void);

/* Retrieve the n-th validated ACPI SDT in XSDT/RSDT order. Unlike the
 * signature-keyed accessor above, this returns each duplicate-signature
 * table separately (e.g. multiple SSDTs are distinct entries) -- the
 * firmware table catalog uses this to preserve full ACPI inventory.
 * Indexing matches the order of signatures emitted by
 * `acpi_enumerate_signatures`. out_sig may be NULL.
 * Returns 1 on success, 0 if index is out of range or validation fails.
 *
 * NOTE: Each call rewalks the root and re-validates every preceding SDT;
 * callers iterating the full list should prefer `acpi_for_each_record`
 * instead, which performs a single validated pass. */
int acpi_get_record(uint32_t index, uint32_t *out_sig,
                    const uint8_t **out_addr, uint32_t *out_size);

/* Single-pass walk over every validated ACPI SDT. The callback receives
 * each record's signature, firmware-mapped pointer, and length-validated
 * size; the user `ctx` is forwarded unchanged. The callback returns 1 to
 * continue or 0 to stop the walk early. Returns the number of records
 * passed to the callback (0 if ACPI was not parsed or the root failed
 * validation). Preserves duplicate-signature tables as distinct callbacks.
 *
 * Use this for catalog-style enumeration so the root walk + per-child
 * checksum/length validation runs once across the whole list, not once
 * per ordinal. */
typedef int (*acpi_record_cb_t)(uint32_t index, uint32_t signature,
                                 const uint8_t *addr, uint32_t size,
                                 void *ctx);
uint32_t acpi_for_each_record(acpi_record_cb_t cb, void *ctx);
