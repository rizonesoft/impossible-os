/* ============================================================================
 * test_bare_metal.c -- Bare-metal hardening infrastructure unit tests
 *
 * Covers the APIs the bare-metal boot path depends on to degrade instead of
 * crashing on real hardware: uncacheable MMIO mapping, IST stacks for the
 * critical exceptions, and the ACPI FADT capability gates that decide whether
 * a legacy device is probed at all.
 *
 * Multi-platform boot (QEMU WHPX/TCG, VirtualBox, bare metal) remains the
 * primary validation for this TODO; these assertions cover the infrastructure
 * APIs underneath it.
 *
 * XREF: 01-boot-platform/TODO-10-bare-metal-hardening.md §Unit Tests
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/acpi.h"
#include "kernel/boot_init.h"
#include "kernel/boot_info.h"
#include "kernel/gdt.h"
#include "kernel/mm/vmm.h"
#include "kernel/drivers/lapic.h"

/* snprintf is not in the freestanding kernel headers (see ob_section.c). */
extern int snprintf(char *buf, size_t size, const char *fmt, ...);

/* ---- UC MMIO mapping -------------------------------------------------------
 *
 * vmm_map_mmio_uc() is the mechanism the whole TODO rests on: device registers
 * reached through a write-back page machine-check on real hardware. The
 * rejection paths are already covered by test_vmm_map_mmio_reject
 * (test_vmm.c); what is NOT covered anywhere is a SUCCESSFUL uncacheable map,
 * which is the case that has to be right on bare metal.
 *
 * The LAPIC register page is the subject because it is the one device page
 * guaranteed to exist on every platform that can run this kernel, and it has a
 * register whose value is independently knowable (the local APIC ID).
 */

static void test_bm_mmio_uc_maps_lapic(void)
{
    uint64_t phys = (uint64_t)acpi_get_lapic_base();
    volatile uint32_t *uc;
    uint64_t flags;
    uint32_t id_via_uc;

    if (!phys || !lapic_available()) {
        TEST_SKIP("no LAPIC base published by ACPI");
        return;
    }

    uc = (volatile uint32_t *)vmm_map_mmio_uc(phys, VMM_PAGE_SIZE);
    TEST_ASSERT_NOT_NULL((void *)uc, "vmm_map_mmio_uc maps the LAPIC page");
    if (!uc)
        return;

    /* The mapping must actually be UNCACHEABLE. Comparing the register value
     * alone would pass just as happily through a write-back mapping, so the
     * PTE attributes are the assertion that distinguishes a correct UC map
     * from a mapping that merely works under an emulator. PCD+PWT with PAT
     * bit clear selects UC; NX because a device page is never code. */
    flags = vmm_query_flags((uintptr_t)uc);
    TEST_ASSERT((flags & VMM_FLAG_PRESENT) != 0,
                "UC MMIO PTE is present");
    TEST_ASSERT((flags & VMM_FLAG_NOCACHE) != 0,
                "UC MMIO PTE has PCD set (cache disabled)");
    TEST_ASSERT((flags & VMM_FLAG_WRITETHROUGH) != 0,
                "UC MMIO PTE has PWT set (UC, not UC-minus)");
    TEST_ASSERT((flags & VMM_FLAG_NX) != 0,
                "UC MMIO PTE is NX (device page is never code)");

    /* Read the local APIC ID through the fresh UC mapping and compare against
     * the driver's own read. They address the same register on the same CPU,
     * so a mismatch means the mapping landed on the wrong frame. */
    id_via_uc = (uc[LAPIC_REG_ID / 4] >> 24) & 0xFF;
    TEST_ASSERT_EQ(id_via_uc, lapic_id(),
                   "LAPIC ID read through the UC mapping matches lapic_id()");

    vmm_unmap_mmio((void *)uc, VMM_PAGE_SIZE);
    TEST_ASSERT_EQ(vmm_query_flags((uintptr_t)uc) & VMM_FLAG_PRESENT, 0,
                   "vmm_unmap_mmio clears the present bit");
}

/* ---- IST stacks ------------------------------------------------------------
 *
 * #DF, NMI and #MCE are delivered on their own stacks precisely because the
 * faulting stack may be the thing that is broken. A zero IST entry means the
 * CPU loads a null stack pointer on delivery and triple-faults instead of
 * reaching the BSOD path, which is unobservable after the fact -- the machine
 * simply resets. GDT setup halts the boot on an allocation failure, so
 * reaching the test runner at all implies the slots were filled; this pins
 * that they stayed filled and are the distinct, page-aligned stacks they are
 * meant to be.
 */

static void test_bm_ist_stacks_allocated(void)
{
    static const struct { unsigned slot; const char *what; } ist[] = {
        { 1, "#DF" },
        { 2, "NMI" },
        { 3, "MCE" },
    };
    uint64_t top[3];
    char msg[80];
    unsigned i, j;

    for (i = 0; i < 3; i++) {
        top[i] = gdt_get_ist(ist[i].slot);

        snprintf(msg, sizeof(msg), "IST%u (%s) stack top is allocated",
                 (uint64_t)ist[i].slot, ist[i].what);
        TEST_ASSERT(top[i] != 0, msg);

        snprintf(msg, sizeof(msg), "IST%u (%s) stack top is page-aligned",
                 (uint64_t)ist[i].slot, ist[i].what);
        TEST_ASSERT((top[i] & (VMM_PAGE_SIZE - 1)) == 0, msg);
    }

    /* Distinct stacks: a shared IST between #DF and NMI is a real nesting
     * hazard, not a cosmetic duplicate. */
    for (i = 0; i < 3; i++) {
        for (j = i + 1; j < 3; j++) {
            snprintf(msg, sizeof(msg), "IST%u (%s) and IST%u (%s) are distinct",
                     (uint64_t)ist[i].slot, ist[i].what,
                     (uint64_t)ist[j].slot, ist[j].what);
            TEST_ASSERT(top[i] != top[j], msg);
        }
    }
}

static void test_bm_ist_index_maps_to_the_right_field(void)
{
    /* Against the live TSS the index-to-field mapping cannot be falsified:
     * only ist1..ist3 are assigned and the rest read as 0, which is exactly
     * what an out-of-range index returns, so a mapping that dropped or
     * misrouted a case would look identical to a correct one. Evaluate the
     * pure half against a TSS whose seven slots carry DISTINCT sentinels, and
     * every case is individually pinned. */
    struct tss t;
    uint8_t *raw = (uint8_t *)&t;
    char msg[80];
    unsigned i;

    for (i = 0; i < sizeof(t); i++)
        raw[i] = 0;

    t.ist1 = 0x1111000000000001ull;
    t.ist2 = 0x2222000000000002ull;
    t.ist3 = 0x3333000000000003ull;
    t.ist4 = 0x4444000000000004ull;
    t.ist5 = 0x5555000000000005ull;
    t.ist6 = 0x6666000000000006ull;
    t.ist7 = 0x7777000000000007ull;

    for (i = 1; i <= TSS_IST_COUNT; i++) {
        /* Sentinels are built so slot i ends in i -- an off-by-one or a
         * swapped case lands on a different value, not merely a nonzero one. */
        uint64_t want = ((uint64_t)i * 0x1111ull) << 48 | (uint64_t)i;
        snprintf(msg, sizeof(msg), "IST index %u selects the ist%u field",
                 (uint64_t)i, (uint64_t)i);
        TEST_ASSERT_EQ(tss_get_ist(&t, i), want, msg);
    }

    /* The IDT encodes IST as a 1-based index, so 0 is "no IST" and must not
     * alias slot 1 -- which the sentinels now make visible, because aliasing
     * would return ist1's value instead of 0. */
    TEST_ASSERT_EQ(tss_get_ist(&t, 0), 0, "IST index 0 is not a slot");
    TEST_ASSERT_EQ(tss_get_ist(&t, TSS_IST_COUNT + 1), 0,
                   "IST index past ist7 is not a slot");
    TEST_ASSERT_EQ(tss_get_ist(&t, 0xFFFFFFFFu), 0,
                   "huge IST index is not a slot");
    TEST_ASSERT_EQ(tss_get_ist((const struct tss *)0, 1), 0,
                   "NULL tss yields no slot");

    /* The live accessor applies the same bounds. Its POSITIVE slots are
     * covered by test_bm_ist_stacks_allocated against the real TSS, which is
     * the only thing that can see the real stacks; the live TSS is file-local
     * to gdt.c by design, so there is deliberately no assertion here that
     * gdt_get_ist reads that specific object. */
    TEST_ASSERT_EQ(gdt_get_ist(0), 0, "gdt_get_ist rejects index 0");
    TEST_ASSERT_EQ(gdt_get_ist(TSS_IST_COUNT + 1), 0,
                   "gdt_get_ist rejects an index past ist7");
}

/* ---- ACPI FADT capability gates (live table) -------------------------------
 *
 * These run against whatever FADT the platform published. The value is not
 * knowable in advance, so the assertion is the contract every caller relies
 * on: a strict 0-or-1 answer, and no crash on a platform with no FADT at all.
 */

static void test_bm_acpi_capability_gates_are_boolean(void)
{
    TEST_ASSERT(acpi_hw_reduced() == 0 || acpi_hw_reduced() == 1,
                "acpi_hw_reduced returns strictly 0 or 1");
    TEST_ASSERT(acpi_has_8042() == 0 || acpi_has_8042() == 1,
                "acpi_has_8042 returns strictly 0 or 1");
    TEST_ASSERT(acpi_has_cmos_rtc() == 0 || acpi_has_cmos_rtc() == 1,
                "acpi_has_cmos_rtc returns strictly 0 or 1");
    TEST_ASSERT(acpi_msi_supported() == 0 || acpi_msi_supported() == 1,
                "acpi_msi_supported returns strictly 0 or 1");
    TEST_ASSERT(acpi_has_vga() == 0 || acpi_has_vga() == 1,
                "acpi_has_vga returns strictly 0 or 1");
}

static void test_bm_acpi_wrappers_delegate_to_their_own_evaluator(void)
{
    /* Each public accessor must forward to ITS OWN pure evaluator over the
     * latched table. Without this, acpi_has_cmos_rtc() rewritten to call the
     * VGA evaluator would pass every other test in this file while telling a
     * caller to touch CMOS ports that are not there.
     *
     * LABEL: this is a delegation PIN, not a live guard. While the accessors
     * remain one-line forwards it compares a function against itself and
     * CANNOT fail. It becomes discriminating the moment an accessor grows a
     * body of its own, which is exactly the edit it exists to catch. */
    const struct acpi_fadt *f = acpi_get_fadt();

    TEST_ASSERT_EQ(acpi_hw_reduced(), acpi_fadt_hw_reduced(f),
                   "acpi_hw_reduced forwards to the hw_reduced evaluator");
    TEST_ASSERT_EQ(acpi_has_8042(), acpi_fadt_has_8042(f),
                   "acpi_has_8042 forwards to the 8042 evaluator");
    TEST_ASSERT_EQ(acpi_has_cmos_rtc(), acpi_fadt_has_cmos_rtc(f),
                   "acpi_has_cmos_rtc forwards to the CMOS RTC evaluator");
    TEST_ASSERT_EQ(acpi_msi_supported(), acpi_fadt_msi_supported(f),
                   "acpi_msi_supported forwards to the MSI evaluator");
    TEST_ASSERT_EQ(acpi_has_vga(), acpi_fadt_has_vga(f),
                   "acpi_has_vga forwards to the VGA evaluator");
}

static void test_bm_acpi_no_fadt_defaults_to_legacy_present(void)
{
    /* Before ACPI is parsed -- and on a platform with no FADT -- the gates
     * must assume legacy hardware is PRESENT. Assuming absent would skip the
     * PS/2 and RTC probes on exactly the old machines that need them. */
    TEST_ASSERT_EQ(acpi_fadt_hw_reduced((const struct acpi_fadt *)0), 0,
                   "no FADT is not hardware-reduced");
    TEST_ASSERT_EQ(acpi_fadt_has_8042((const struct acpi_fadt *)0), 1,
                   "no FADT assumes i8042 present");
    TEST_ASSERT_EQ(acpi_fadt_has_cmos_rtc((const struct acpi_fadt *)0), 1,
                   "no FADT assumes CMOS RTC present");
    TEST_ASSERT_EQ(acpi_fadt_msi_supported((const struct acpi_fadt *)0), 1,
                   "no FADT assumes MSI supported");
    TEST_ASSERT_EQ(acpi_fadt_has_vga((const struct acpi_fadt *)0), 1,
                   "no FADT assumes VGA present");
}

/* ---- ACPI hardware-reduced override (synthetic table) ----------------------
 *
 * No platform this kernel runs on publishes a hardware-reduced FADT, so the
 * override branch is unreachable from the live table. The pure evaluators take
 * the FADT explicitly for exactly this reason: the policy can be exercised
 * against a table built here.
 */

/* Build a FADT that is long enough for every gated field and asserts nothing
 * about the platform. `hw_reduced` sets flags bit 20; `boot_arch` is the
 * IAPC_BOOT_ARCH word verbatim. */
static void bm_make_fadt(struct acpi_fadt *f, int hw_reduced, uint16_t boot_arch)
{
    uint8_t *raw = (uint8_t *)f;
    unsigned i;

    for (i = 0; i < sizeof(*f); i++)
        raw[i] = 0;

    f->header.length = (uint32_t)sizeof(*f);
    f->boot_arch_flags = boot_arch;
    f->flags = hw_reduced ? (1u << 20) : 0;
}

static void test_bm_acpi_hw_reduced_overrides_legacy_bits(void)
{
    struct acpi_fadt f;

    /* IAPC_BOOT_ARCH advertising every legacy device as PRESENT: bit 1 (8042)
     * set, bit 5 (CMOS_RTC_NOT_PRESENT) clear, bit 2 (VGA_NOT_PRESENT) clear.
     * A hardware-reduced platform has none of them regardless. */
    bm_make_fadt(&f, 1, (uint16_t)(1u << 1));

    TEST_ASSERT_EQ(acpi_fadt_hw_reduced(&f), 1,
                   "flags bit 20 reports hardware-reduced");
    TEST_ASSERT_EQ(acpi_fadt_has_8042(&f), 0,
                   "hw-reduced overrides the 8042-present bit to absent");
    TEST_ASSERT_EQ(acpi_fadt_has_cmos_rtc(&f), 0,
                   "hw-reduced overrides CMOS RTC to absent");
    TEST_ASSERT_EQ(acpi_fadt_has_vga(&f), 0,
                   "hw-reduced overrides VGA to absent");

    /* MSI is a PCI capability, not legacy fixed hardware, so hardware-reduced
     * must NOT suppress it -- concluding "no MSI" there would push a modern
     * machine onto a legacy interrupt path it may not even have. */
    TEST_ASSERT_EQ(acpi_fadt_msi_supported(&f), 1,
                   "hw-reduced does not suppress MSI");

    /* ...but bit 3 stays authoritative on a hardware-reduced table. Without
     * this case an evaluator that returned 1 unconditionally for hw-reduced
     * would pass the assertion above. */
    bm_make_fadt(&f, 1, (uint16_t)(1u << 3));
    TEST_ASSERT_EQ(acpi_fadt_msi_supported(&f), 0,
                   "hw-reduced still honors MSI_NOT_SUPPORTED");
}

static void test_bm_acpi_unrelated_flags_are_not_hw_reduced(void)
{
    struct acpi_fadt f;

    /* Only flags bit 20 means hardware-reduced. Every other FADT flag bit is
     * an unrelated platform property (WBINVD, power button style, docking
     * capability and so on), and a great many real tables set several. An
     * evaluator that tested `flags != 0`, or masked the wrong bit, would pass
     * every other test in this file, then silently suppress the PS/2, RTC and
     * VGA probes on ordinary legacy hardware. */
    bm_make_fadt(&f, 0, (uint16_t)(1u << 1));
    f.flags = ~(uint32_t)(1u << 20);   /* every flag EXCEPT hardware-reduced */

    TEST_ASSERT_EQ(acpi_fadt_hw_reduced(&f), 0,
                   "flags with every bit but 20 is not hardware-reduced");
    TEST_ASSERT_EQ(acpi_fadt_has_8042(&f), 1,
                   "unrelated flags do not suppress the 8042 probe");
    TEST_ASSERT_EQ(acpi_fadt_has_cmos_rtc(&f), 1,
                   "unrelated flags do not suppress the CMOS RTC probe");
    TEST_ASSERT_EQ(acpi_fadt_has_vga(&f), 1,
                   "unrelated flags do not suppress the VGA probe");

    /* Control: adding bit 20 to that same word DOES flip it, so the assertions
     * above rejected on the bit and not on the table being unreadable. */
    f.flags = 0xFFFFFFFFu;
    TEST_ASSERT_EQ(acpi_fadt_hw_reduced(&f), 1,
                   "bit 20 set among every other flag is hardware-reduced");
}

static void test_bm_acpi_legacy_table_honors_boot_arch_bits(void)
{
    struct acpi_fadt f;

    /* Control for the test above: the SAME boot_arch word on a NON-hw-reduced
     * table must report the legacy devices as present. Without this, an
     * evaluator that returned 0 unconditionally would pass the override test. */
    bm_make_fadt(&f, 0, (uint16_t)(1u << 1));

    TEST_ASSERT_EQ(acpi_fadt_hw_reduced(&f), 0,
                   "flags bit 20 clear is not hardware-reduced");
    TEST_ASSERT_EQ(acpi_fadt_has_8042(&f), 1,
                   "legacy table honors the 8042-present bit");
    TEST_ASSERT_EQ(acpi_fadt_has_cmos_rtc(&f), 1,
                   "legacy table reports CMOS RTC present (bit 5 clear)");
    TEST_ASSERT_EQ(acpi_fadt_has_vga(&f), 1,
                   "legacy table reports VGA present (bit 2 clear)");
    TEST_ASSERT_EQ(acpi_fadt_msi_supported(&f), 1,
                   "legacy table reports MSI supported (bit 3 clear)");

    /* Inverted-sense bits: set means ABSENT / NOT SUPPORTED. */
    bm_make_fadt(&f, 0, (uint16_t)((1u << 5) | (1u << 2) | (1u << 3)));
    TEST_ASSERT_EQ(acpi_fadt_has_8042(&f), 0,
                   "8042 bit clear reports i8042 absent");
    TEST_ASSERT_EQ(acpi_fadt_has_cmos_rtc(&f), 0,
                   "CMOS_RTC_NOT_PRESENT set reports RTC absent");
    TEST_ASSERT_EQ(acpi_fadt_has_vga(&f), 0,
                   "VGA_NOT_PRESENT set reports VGA absent");
    TEST_ASSERT_EQ(acpi_fadt_msi_supported(&f), 0,
                   "MSI_NOT_SUPPORTED set reports MSI unsupported");
}

static void test_bm_acpi_short_table_is_not_read_past(void)
{
    struct acpi_fadt f;

    /* A truncated FADT cannot prove a capability is absent, so both bounds
     * fail SAFE: legacy present, not hardware-reduced.
     *
     * SCOPE: this pins the DECISION, not memory safety. The fixture is a full
     * struct whose fields stay addressable after header.length is shortened,
     * so an evaluator that read the field first and only then consulted the
     * length would still pass. Catching a genuine overread needs a guard-page
     * or sanitizer harness around the evaluators, which this freestanding
     * kernel target has no equivalent of. */
    bm_make_fadt(&f, 1, (uint16_t)((1u << 5) | (1u << 2) | (1u << 3)));

    f.header.length = ACPI_FADT_LEN_FLAGS - 1;
    TEST_ASSERT_EQ(acpi_fadt_hw_reduced(&f), 0,
                   "FADT too short for flags is not hardware-reduced");

    f.header.length = ACPI_FADT_LEN_BOOT_ARCH - 1;
    TEST_ASSERT_EQ(acpi_fadt_has_8042(&f), 1,
                   "FADT too short for boot_arch assumes i8042 present");
    TEST_ASSERT_EQ(acpi_fadt_has_cmos_rtc(&f), 1,
                   "FADT too short for boot_arch assumes CMOS RTC present");
    TEST_ASSERT_EQ(acpi_fadt_msi_supported(&f), 1,
                   "FADT too short for boot_arch assumes MSI supported");
    TEST_ASSERT_EQ(acpi_fadt_has_vga(&f), 1,
                   "FADT too short for boot_arch assumes VGA present");

    /* Exactly at each bound the field IS readable, so the gates must engage.
     * This is the control that shows the checks above rejected on LENGTH and
     * not simply on the zeroed table. */
    /* Each evaluator carries its OWN length check, so the exact bound is
     * asserted for each of them -- a `<` silently becoming `<=` in only the
     * CMOS, MSI or VGA gate would otherwise pass, and misclassify every
     * 113-byte table. */
    f.header.length = ACPI_FADT_LEN_BOOT_ARCH;
    TEST_ASSERT_EQ(acpi_fadt_has_8042(&f), 0,
                   "at the boot_arch bound the 8042 bit is honored");
    TEST_ASSERT_EQ(acpi_fadt_has_cmos_rtc(&f), 0,
                   "at the boot_arch bound the CMOS RTC bit is honored");
    TEST_ASSERT_EQ(acpi_fadt_msi_supported(&f), 0,
                   "at the boot_arch bound the MSI bit is honored");
    TEST_ASSERT_EQ(acpi_fadt_has_vga(&f), 0,
                   "at the boot_arch bound the VGA bit is honored");

    f.header.length = ACPI_FADT_LEN_FLAGS;
    TEST_ASSERT_EQ(acpi_fadt_hw_reduced(&f), 1,
                   "at the flags bound the hw-reduced bit is honored");
}

/* ---- Degraded-subsystem bookkeeping ---------------------------------------
 *
 * The dual-channel apply_result logic is already covered by
 * test_subsys_apply_result_dual_channel (test_boot_init.c) and is deliberately
 * not duplicated. What is covered here is the structural invariant on the mask
 * itself, which holds on EVERY platform -- including one that legitimately
 * degraded a subsystem, where asserting the mask is zero would fail on correct
 * behavior rather than on a defect.
 */

static void test_bm_degraded_mask_is_well_formed(void)
{
    uint32_t mask = g_boot_info.degraded_mask;
    uint32_t stray = mask & ~((uint32_t)((1ull << SUBSYS_COUNT) - 1));
    char msg[80];
    uint32_t i;

    snprintf(msg, sizeof(msg),
             "degraded_mask has no bits outside SUBSYS_COUNT (stray 0x%x)",
             (uint64_t)stray);
    TEST_ASSERT_EQ(stray, 0, msg);

    /* Every set bit must name a real subsystem: the desktop summary walks the
     * mask and prints kernel_subsystem_name() per bit, which is the exact
     * drift a new SUBSYS_* slot without a name entry would produce. */
    for (i = 0; i < SUBSYS_COUNT; i++) {
        if (!(mask & (1u << i)))
            continue;
        snprintf(msg, sizeof(msg), "degraded subsystem %u has a name",
                 (uint64_t)i);
        TEST_ASSERT_NOT_NULL((void *)kernel_subsystem_name((kernel_subsys_t)i),
                             msg);
    }
}

/* ---- CPU hardening verification -------------------------------------------
 *
 * cpu_verify_hardening cannot be called from a test: on an NX readback failure
 * it halts the boot rather than returning, which would take the whole
 * test-runner boot down instead of failing one assertion. That call is
 * transitive, so the pre-commit forbidden-call scan (which reads this file's
 * own text) would not catch it -- this note is the record of why the call is
 * deliberately absent.
 *
 * Its output is verified end-to-end instead: scripts/test-smoke.sh asserts the
 * "Verify: NX enabled (EFER.NXE set)" serial line on every boot.
 */

static void test_bm_cpu_hardening_verified_by_smoke(void)
{
    TEST_SKIP("cpu_verify_hardening halts on NX failure; smoke asserts its line");
}

/* ---- Registration ---- */

void test_register_bare_metal(void)
{
    test_suite_register_cat("BM: UC MMIO maps the LAPIC page",
                            test_bm_mmio_uc_maps_lapic, TEST_CAT_BOOT);
    test_suite_register_cat("BM: IST stacks allocated",
                            test_bm_ist_stacks_allocated, TEST_CAT_BOOT);
    test_suite_register_cat("BM: IST index maps to the right field",
                            test_bm_ist_index_maps_to_the_right_field,
                            TEST_CAT_BOOT);
    test_suite_register_cat("BM: ACPI capability gates are boolean",
                            test_bm_acpi_capability_gates_are_boolean,
                            TEST_CAT_BOOT);
    test_suite_register_cat("BM: ACPI wrappers delegate correctly",
                            test_bm_acpi_wrappers_delegate_to_their_own_evaluator,
                            TEST_CAT_BOOT);
    test_suite_register_cat("BM: ACPI defaults with no FADT",
                            test_bm_acpi_no_fadt_defaults_to_legacy_present,
                            TEST_CAT_BOOT);
    test_suite_register_cat("BM: ACPI hw-reduced overrides legacy bits",
                            test_bm_acpi_hw_reduced_overrides_legacy_bits,
                            TEST_CAT_BOOT);
    test_suite_register_cat("BM: ACPI unrelated flags are not hw-reduced",
                            test_bm_acpi_unrelated_flags_are_not_hw_reduced,
                            TEST_CAT_BOOT);
    test_suite_register_cat("BM: ACPI legacy table honors boot_arch bits",
                            test_bm_acpi_legacy_table_honors_boot_arch_bits,
                            TEST_CAT_BOOT);
    test_suite_register_cat("BM: ACPI short table is not read past",
                            test_bm_acpi_short_table_is_not_read_past,
                            TEST_CAT_BOOT);
    test_suite_register_cat("BM: degraded_mask is well formed",
                            test_bm_degraded_mask_is_well_formed, TEST_CAT_BOOT);
    test_suite_register_cat("BM: CPU hardening verified by smoke",
                            test_bm_cpu_hardening_verified_by_smoke,
                            TEST_CAT_BOOT);
}

#endif /* KERNEL_TESTS */
