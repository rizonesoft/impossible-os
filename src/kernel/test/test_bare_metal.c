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
#include "kernel/idt.h"
#include "kernel/mm/pmm.h"
#include "kernel/mm/boot_stack.h"
#include "kernel/mm/user_range.h"
#include "kernel/drivers/lapic.h"

/* strcmp lives in the kernel libc, not the freestanding headers (see snprintf
 * above). The guard-label assertion below compares the exact registered text. */
extern int strcmp(const char *a, const char *b);

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

/* ---- BSP ring-0 entry stack guard ----------------------------------------
 *
 * The stack TSS.rsp0 holds on a ring-3 -> ring-0 transition before the
 * scheduler installs a thread's own kernel_rsp. It was the one stack in the
 * kernel without a guard page and is now a PMM-backed guarded run.
 *
 * What is testable here is the STRUCTURE -- page alignment, the guard sitting
 * exactly one page below the usable base, and the guard being registered under
 * its label. The behaviour on an actual overflow (escalation to #DF and the
 * label appearing on serial) is a bare-metal validation item, not a unit test:
 * provoking it deliberately would take the machine down mid-suite.
 */

static void test_bm_bsp_entry_stack_is_page_aligned(void)
{
    uintptr_t base  = bsp_entry_stack_base();
    uintptr_t guard = bsp_entry_stack_guard();
    uint64_t  top   = bsp_entry_stack_top();

    TEST_ASSERT(base != 0, "BSP entry stack was never allocated");
    TEST_ASSERT((base & 0xFFFu) == 0, "BSP entry stack base is not page-aligned");
    TEST_ASSERT((guard & 0xFFFu) == 0, "BSP entry stack guard is not page-aligned");
    TEST_ASSERT_EQ(top, (uint64_t)(base + BSP_ENTRY_STACK_SIZE),
                   "stack top must be base + BSP_ENTRY_STACK_SIZE");
    TEST_ASSERT_EQ((uint64_t)base, (uint64_t)(guard + 4096u),
                   "the guard page must sit immediately below the usable base");
}

static void test_bm_bsp_entry_stack_guard_is_registered(void)
{
    const char *label;

    TEST_ASSERT(bsp_entry_stack_guarded() != 0,
                "BSP entry stack shipped UNGUARDED -- guard table full or split failed");

    label = vmm_guard_page_label(bsp_entry_stack_guard());
    TEST_ASSERT(label != (const char *)0,
                "no guard-table entry for the BSP entry stack guard page");

    /* TEST_ASSERT records and RETURNS TO THE CALLER -- it does not abort the
     * test function -- so the NULL case has to be bailed out of explicitly.
     * Falling through would hand strcmp a null pointer on exactly the registry
     * failure this test exists to catch, turning a reported failure into a
     * kernel panic that truncates the suite. */
    if (!label)
        return;

    /* The label is what a #DF on this stack prints, so pin the text, not just
     * its presence. */
    TEST_ASSERT(strcmp(label, "GUARD: BSP kernel entry stack overflow") == 0,
                "BSP entry stack guard is registered under the wrong label");

    /* The usable run itself must NOT be a guard: a lookup that matched it
     * would mean the stack's own pages are unmapped. */
    TEST_ASSERT(vmm_guard_page_label(bsp_entry_stack_base()) == (const char *)0,
                "the usable BSP entry stack base must not be a registered guard");
    TEST_ASSERT(vmm_guard_page_label(bsp_entry_stack_top() - 8) == (const char *)0,
                "the BSP entry stack top must not be a registered guard");
}

static void test_bm_bsp_entry_stack_peak_is_bounded(void)
{
    uint32_t peak = bsp_entry_stack_peak_used();

    /* The poison watermark turns the 16 KiB size into a measured margin. A
     * peak at or above the full size means the run was exhausted, which the
     * guard page below would have caught first -- so reaching here with that
     * value means the watermark itself is wrong. Zero is legitimate and is the
     * expected reading while no ring-3 entry has occurred yet. */
    TEST_ASSERT(peak < BSP_ENTRY_STACK_SIZE,
                "BSP entry stack watermark reports the whole run as used");

    /* The watermark must be qword-granular: it is derived by scanning 8-byte
     * poison cells, so any other value means the scan arithmetic is wrong. */
    TEST_ASSERT((peak & 7u) == 0,
                "BSP entry stack watermark is not a whole number of qwords");
}

/* The #DF stack-overflow classifier. This is the ONLY reachable test of the
 * attribution path: a real hit requires overflowing a kernel stack, which takes
 * the machine down. idt_df_guard_reason() is pure over its arguments plus two
 * read-only globals, so the four shapes that matter are checkable directly. */
static void test_bm_df_classifier_names_the_guard(void)
{
    uintptr_t guard = bsp_entry_stack_guard();
    uintptr_t usable = bsp_entry_stack_base();
    const char *bsp_label = "GUARD: BSP kernel entry stack overflow";
    const char *possible = "Double Fault (possible kernel stack guard overflow)";
    const char *fallback = "Double Fault";
    const char *r;

    TEST_ASSERT(bsp_entry_stack_guarded() != 0,
                "classifier test needs the BSP guard installed");

    /* The commonest REAL shape, and the one an earlier corroboration rule got
     * wrong: a faulting PUSH/CALL reports the pre-instruction RSP, which is
     * still on the usable page while CR2 is already in the guard. This must
     * still name the stack. */
    r = idt_df_guard_reason(8, guard + 0x40, usable + 0x10, fallback);
    TEST_ASSERT(r != (const char *)0 && strcmp(r, bsp_label) == 0,
                "CR2 in the guard with RSP on the usable page must name the stack");

    /* Both keys in the guard -- also a real overflow, same answer. */
    r = idt_df_guard_reason(8, guard + 0x40, guard + 0x8, fallback);
    TEST_ASSERT(r != (const char *)0 && strcmp(r, bsp_label) == 0,
                "both keys in the guard must name the stack");

    /* RSP-only match: the saved #DF state is not architecturally reliable, so
     * this is reported as a possibility rather than as a specific stack. */
    r = idt_df_guard_reason(8, usable + 0x10, guard + 0x8, fallback);
    TEST_ASSERT(r != (const char *)0 && strcmp(r, possible) == 0,
                "an RSP-only match must be reported as possible, not definitive");

    /* Neither key in any guard -- a #DF that is not a stack overflow keeps the
     * vector name. This is the stale-CR2-elsewhere case. */
    r = idt_df_guard_reason(8, usable + 0x10, usable + 0x20, fallback);
    TEST_ASSERT(r == fallback,
                "a #DF with no guard match must keep the vector name");

    /* Every other vector is untouched, whatever CR2 holds. */
    r = idt_df_guard_reason(14, guard + 0x40, guard + 0x8, fallback);
    TEST_ASSERT(r == fallback,
                "the classifier must only act on #DF");
}

/* The BSP guard short-circuits ahead of the registry, so the cases above never
 * reach the registry branches. Without this, deleting those branches would
 * leave the suite green while task, AP and IST overflows lost their reports.
 * Install a real guard on a scratch frame to drive them. */
static void test_bm_df_classifier_uses_the_guard_registry(void)
{
    const char *possible = "Double Fault (possible kernel stack guard overflow)";
    const char *fallback = "Double Fault";
    uintptr_t frame = pmm_alloc_frame();
    const char *r;
    int rc;

    if (!frame) {
        TEST_SKIP("no free frame for a scratch guard");
        return;
    }

    rc = vmm_install_guard_page(frame, "GUARD: classifier scratch");
    if (rc != VMM_GUARD_OK) {
        /* VMM_GUARD_VA_UNSAFE means the frame is not PMM-safe: quarantine it
         * rather than freeing it, exactly as the production callers do. */
        if (rc != VMM_GUARD_VA_UNSAFE)
            pmm_free_frame(frame);
        TEST_SKIP("scratch guard could not be installed");
        return;
    }

    /* CR2 in a REGISTRY guard: reported as a possibility, not as a named stack.
     * The registry stores no run extent, so there is nothing to corroborate
     * against and a confident name would be unearned. */
    r = idt_df_guard_reason(8, frame + 0x40, frame + 0x1000, fallback);
    TEST_ASSERT(r != (const char *)0 && strcmp(r, possible) == 0,
                "a registry CR2 hit must report a possible guard overflow");

    /* RSP-only through the registry reaches the last branch. */
    r = idt_df_guard_reason(8, frame + 0x8000, frame + 0x40, fallback);
    TEST_ASSERT(r != (const char *)0 && strcmp(r, possible) == 0,
                "a registry RSP hit must report a possible guard overflow");

    /* Neither key in the scratch guard: unchanged verdict. */
    r = idt_df_guard_reason(8, frame + 0x8000, frame + 0x9000, fallback);
    TEST_ASSERT(r == fallback,
                "addresses outside every guard must keep the vector name");

    /* Teardown is ASSERTED, not merely attempted. vmm_uninstall_guard_page
     * deliberately keeps the registry entry on failure, so a silent failure
     * here would leave this test's guard live in the boot-time address space
     * for every later test and for the rest of boot, while the suite stayed
     * green. Free only on success, per the vmm.h contract. */
    rc = vmm_uninstall_guard_page(frame);
    TEST_ASSERT_EQ(rc, 0, "scratch guard must uninstall cleanly");
    if (rc == 0)
        pmm_free_frame(frame);
}

/* ---- Registration ---- */


/* ---- Loader-owned kernel boot stack (section 32) ---------------------------
 *
 * boot_stack_validate() is the fail-closed gate on the one handoff field that,
 * if wrong, lets the PMM hand out the memory the kernel is standing on. It is
 * a PURE function over the three published values precisely so every refusal
 * branch is reachable from a test -- the live path halts the boot, which a
 * test may not do.
 *
 * The accepted geometry below deliberately does NOT hardcode what the
 * bootloader currently allocates; it drives the CONTRACT (page grain, size
 * band, guard strictly inside, identity-map ceiling). A test that echoed
 * BL_KSTACK_SIZE would only assert that the constant was typed correctly.
 */

#define BM_KS_BASE   0x00200000ull
#define BM_KS_SIZE   0x00040000ull   /* 256 KiB -- above BOOT_KSTACK_MIN_SIZE */
#define BM_KS_GUARD  0x00001000ull   /* 4 KiB  */

static void test_bm_kstack_accepts_a_well_formed_handoff(void)
{
    struct boot_stack_info si;
    enum boot_stack_error err = BOOT_STACK_ERR_WRAP;  /* poisoned, must be overwritten */

    TEST_ASSERT(boot_stack_validate(BM_KS_BASE, BM_KS_SIZE, BM_KS_GUARD,
                                    &si, &err) != 0,
                "a page-aligned in-band run must be accepted");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_STACK_ERR_OK,
                   "accept must report BOOT_STACK_ERR_OK");
    TEST_ASSERT_EQ(si.base, BM_KS_BASE, "base must round-trip unchanged");
    TEST_ASSERT_EQ(si.size, BM_KS_SIZE, "size must round-trip unchanged");
    TEST_ASSERT_EQ(si.guard_size, BM_KS_GUARD, "guard must round-trip unchanged");
    TEST_ASSERT(si.valid != 0, "accepted handoff must be marked valid");
}

static void test_bm_kstack_refuses_each_malformed_shape(void)
{
    struct boot_stack_info si;
    enum boot_stack_error err;

    /* Absent beats every other verdict: a zero base must NOT be reported as a
     * misalignment of address 0, or a bootloader that published nothing looks
     * like one that published something broken. */
    TEST_ASSERT(boot_stack_validate(0, BM_KS_SIZE, BM_KS_GUARD, &si, &err) == 0,
                "a zero base must be refused");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_STACK_ERR_ABSENT,
                   "a zero base must be reported as ABSENT, not as misaligned");

    TEST_ASSERT(boot_stack_validate(BM_KS_BASE + 8, BM_KS_SIZE, BM_KS_GUARD,
                                    &si, &err) == 0,
                "a non-page-aligned base must be refused");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_STACK_ERR_UNALIGNED,
                   "misaligned base must report UNALIGNED");

    TEST_ASSERT(boot_stack_validate(BM_KS_BASE, 0, BM_KS_GUARD, &si, &err) == 0,
                "a zero-size run must be refused");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_STACK_ERR_SIZE_RANGE,
                   "zero-size run must report SIZE_RANGE");

    /* A run smaller than its own guard would underflow size - guard_size and
     * wrap to a huge usable span that clears the floor. */
    TEST_ASSERT(boot_stack_validate(BM_KS_BASE, BM_KS_GUARD, BM_KS_GUARD,
                                    &si, &err) == 0,
                "a run no larger than its guard must be refused");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_STACK_ERR_USABLE_SHORT,
                   "a guard-sized run must report USABLE_SHORT, not wrap into "
                   "a passing usable span");

    TEST_ASSERT(boot_stack_validate(BM_KS_BASE,
                                    (uint64_t)BOOT_KSTACK_MAX_SIZE + 4096u,
                                    BM_KS_GUARD, &si, &err) == 0,
                "a run above the size ceiling must be refused");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_STACK_ERR_SIZE_RANGE,
                   "oversized run must report SIZE_RANGE");

    TEST_ASSERT(boot_stack_validate(BM_KS_BASE, BM_KS_SIZE + 8, BM_KS_GUARD,
                                    &si, &err) == 0,
                "a size that is not a page multiple must be refused");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_STACK_ERR_SIZE_GRAIN,
                   "non-page-multiple size must report SIZE_GRAIN");

    TEST_ASSERT(boot_stack_validate(BM_KS_BASE, BM_KS_SIZE, BM_KS_GUARD + 8,
                                    &si, &err) == 0,
                "a guard that is not a page multiple must be refused");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_STACK_ERR_GUARD_GRAIN,
                   "non-page-multiple guard must report GUARD_GRAIN");

    /* A zero guard passes grain and range, then ships a run with no overflow
     * detection at all. Accepting it would make the guard guarantee
     * unfalsifiable, so it is refused with its own verdict rather than folded
     * into GUARD_RANGE. */
    TEST_ASSERT(boot_stack_validate(BM_KS_BASE, BM_KS_SIZE, 0, &si, &err) == 0,
                "a zero guard must be refused");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_STACK_ERR_GUARD_ABSENT,
                   "zero guard must report GUARD_ABSENT, not GUARD_GRAIN");

    /* A structurally PERFECT run that happens to sit in the user page-table
     * window must still be refused. vmm_create_user_pml4() re-points that PD
     * entry per process, so a stack here stops being mapped at the first user
     * exec -- and this run is PID 0's permanent stack. The loader avoids the
     * window, but AllocateMaxAddress bounds only the top of the allocation
     * and firmware need not allocate top-down, so this consumer-side refusal
     * is the actual guarantee. */
    TEST_ASSERT(boot_stack_validate((uint64_t)USER_PT_WINDOW_BASE, BM_KS_SIZE,
                                    BM_KS_GUARD, &si, &err) == 0,
                "a run based inside the user PT window must be refused");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_STACK_ERR_USER_WINDOW,
                   "a run in the user PT window must report USER_WINDOW");
    /* Partial overlap from below counts too -- the window is not entered only
     * by starting in it. */
    TEST_ASSERT(boot_stack_validate((uint64_t)USER_PT_WINDOW_BASE - 0x1000ull,
                                    BM_KS_SIZE, BM_KS_GUARD, &si, &err) == 0,
                "a run overlapping the window's low edge must be refused");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_STACK_ERR_USER_WINDOW,
                   "a low-edge overlap must report USER_WINDOW");
    /* Below 1 MiB the loader writes the boot page tables (0x70000-0x75fff),
     * boot_info (0x10000) and the AP trampoline at fixed addresses with no
     * firmware claim, so a run there can contain the live PML4: 0x60000
     * passed every check above and setup_page_tables() then wrote inside the
     * poisoned run. */
    TEST_ASSERT(boot_stack_validate(0x60000ull, BM_KS_SIZE, BM_KS_GUARD,
                                    &si, &err) == 0,
                "a run starting below 1 MiB must be refused");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_STACK_ERR_LOW_MEM,
                   "a run below 1 MiB must report LOW_MEM");
    /* Refusal direction control: the LOW_MEM rule's first legal base is
     * exactly 1 MiB. That is a statement about this rule only -- 1 MiB is
     * where the kernel image itself is loaded (src/boot/linker.ld), so a run
     * based there is refused by boot_stack_init's image-envelope rule below,
     * which is a different check with its own error code. Keeping the two
     * assertions apart is deliberate: a single "is 0x100000 legal" question
     * has two different right answers depending on which rule is asking. */
    TEST_ASSERT(boot_stack_validate(0x100000ull, BM_KS_SIZE, BM_KS_GUARD,
                                    &si, &err) == 1,
                "a run based at exactly 1 MiB is not low memory");
    /* Consumer-side disjointness against the kernel image and the PMM bitmap
     * extent, which the PURE validator cannot know because it holds no linker
     * symbols. boot_stack_init applies it in Phase 0 over the envelope below,
     * and pmm_init re-asks it over the VALIDATED run (the reserved-table
     * payload predicate skips every kind but PAYLOAD, so nothing else
     * compares the run against them). */
    TEST_ASSERT(boot_stack_validate(BM_KS_BASE, BM_KS_SIZE, BM_KS_GUARD,
                                    &si, &err) == 1, "reference run validates");
    TEST_ASSERT(boot_stack_overlaps(&si, BM_KS_BASE + 0x1000ull, 0x1000ull) == 1,
                "a range inside the run overlaps it");
    TEST_ASSERT(boot_stack_overlaps(&si, BM_KS_BASE - 0x1000ull, 0x2000ull) == 1,
                "a range straddling the run's base overlaps it");
    TEST_ASSERT(boot_stack_overlaps(&si, BM_KS_BASE + BM_KS_SIZE - 8ull, 0x10ull) == 1,
                "a range straddling the run's top overlaps it");
    TEST_ASSERT(boot_stack_overlaps(&si, BM_KS_BASE + BM_KS_SIZE, 0x1000ull) == 0,
                "a range starting at the run's end is disjoint");
    TEST_ASSERT(boot_stack_overlaps(&si, BM_KS_BASE - 0x1000ull, 0x1000ull) == 0,
                "a range ending at the run's base is disjoint");
    TEST_ASSERT(boot_stack_overlaps(&si, BM_KS_BASE, 0ull) == 0,
                "a zero-length range never overlaps");
    TEST_ASSERT(boot_stack_overlaps(&si, ~0ull - 0x100ull, 0x1000ull) == 0,
                "a wrapping range never overlaps");
    TEST_ASSERT(boot_stack_overlaps((const struct boot_stack_info *)0,
                                    BM_KS_BASE, 0x1000ull) == 0,
                "a NULL info never overlaps");
    /* And a run ending exactly at the window base is disjoint, so it must NOT
     * be refused -- an off-by-one here would reject legitimate placements. */
    TEST_ASSERT(boot_stack_validate((uint64_t)USER_PT_WINDOW_BASE - BM_KS_SIZE,
                                    BM_KS_SIZE, BM_KS_GUARD, &si, &err) != 0,
                "a run ending exactly at the window base must be accepted");

    /* base + size overflowing UINT64_MAX would otherwise compute an end BELOW
     * the base and satisfy the identity-map ceiling test by wrapping under
     * it. WRAP is checked before ABOVE_MAP precisely so this reports the
     * cause rather than the symptom. */
    TEST_ASSERT(boot_stack_validate(0xFFFFFFFFFFFC0000ull, BM_KS_SIZE,
                                    BM_KS_GUARD, &si, &err) == 0,
                "a base + size that wraps must be refused");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_STACK_ERR_WRAP,
                   "wrapping run must report WRAP, not ABOVE_MAP");

    /* A page-MULTIPLE guard larger than one page is refused. It would move
     * the boundary the stack grows through to the page below
     * base + guard_size while the installer unmaps base, leaving the real
     * boundary mapped -- an overflow that does not fault. */
    TEST_ASSERT(boot_stack_validate(BM_KS_BASE, BM_KS_SIZE, 0x8000ull,
                                    &si, &err) == 0,
                "a multi-page guard must be refused");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_STACK_ERR_GUARD_GRAIN,
                   "a multi-page guard must report GUARD_GRAIN");

    /* THE geometry the round-2 review named: usable span one page short of
     * the floor. A total of exactly BOOT_KSTACK_MIN_USABLE leaves
     * MIN_USABLE - 4096 usable once the guard is taken out. Under a
     * total-size floor this whole class was accepted. */
    TEST_ASSERT(boot_stack_validate(BM_KS_BASE,
                                    (uint64_t)BOOT_KSTACK_MIN_USABLE,
                                    BM_KS_GUARD, &si, &err) == 0,
                "a run whose USABLE span is below the floor must be refused");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_STACK_ERR_USABLE_SHORT,
                   "a short usable span must report USABLE_SHORT");

    /* The kernel reads this run through the boot identity map, which stops at
     * 4 GiB. A run ending above it is unreadable here whatever the producer
     * meant, so it is refused rather than faulted on later. */
    TEST_ASSERT(boot_stack_validate(BOOT_INFO_EARLY_MAP_END - 4096ull,
                                    BM_KS_SIZE, BM_KS_GUARD, &si, &err) == 0,
                "a run ending above the identity map must be refused");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_STACK_ERR_ABOVE_MAP,
                   "above-map run must report ABOVE_MAP");
}

static void test_bm_kstack_refusal_leaves_the_output_untouched(void)
{
    struct boot_stack_info si;
    enum boot_stack_error err;

    si.base = 0xDEADBEEFull;
    si.size = 0xDEADBEEFull;
    si.guard_size = 0xDEADBEEFull;
    si.valid = 0;

    TEST_ASSERT(boot_stack_validate(0, BM_KS_SIZE, BM_KS_GUARD, &si, &err) == 0,
                "sanity: the refusal under test must actually refuse");
    /* A refusal that half-populated `si` would let a caller that checked the
     * struct instead of the return value proceed on garbage bounds. ALL FOUR
     * fields are checked: asserting only base and valid would miss a partial
     * write that left a plausible size behind. */
    TEST_ASSERT_EQ(si.base, 0xDEADBEEFull,
                   "a refused handoff must not write out->base");
    TEST_ASSERT_EQ(si.size, 0xDEADBEEFull,
                   "a refused handoff must not write out->size");
    TEST_ASSERT_EQ(si.guard_size, 0xDEADBEEFull,
                   "a refused handoff must not write out->guard_size");
    TEST_ASSERT(si.valid == 0,
                "a refused handoff must never set out->valid");
}

static void test_bm_kstack_accepts_at_the_exact_contract_bounds(void)
{
    struct boot_stack_info si;
    enum boot_stack_error err;

    /* The floor and ceiling are INCLUSIVE. An off-by-one that made either
     * exclusive would reject a producer publishing exactly the documented
     * geometry, and no other test in this file would notice. */
    TEST_ASSERT(boot_stack_validate(BM_KS_BASE, (uint64_t)BOOT_KSTACK_MIN_SIZE,
                                    BM_KS_GUARD, &si, &err) != 0,
                "a run at exactly BOOT_KSTACK_MIN_SIZE must be accepted");
    /* One page below it must NOT be, or the floor is off by a page and the
     * smallest accepted run is short of the measured peak. */
    TEST_ASSERT(boot_stack_validate(BM_KS_BASE,
                                    (uint64_t)BOOT_KSTACK_MIN_SIZE - 4096ull,
                                    BM_KS_GUARD, &si, &err) == 0,
                "one page below BOOT_KSTACK_MIN_SIZE must be refused");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_STACK_ERR_USABLE_SHORT,
                   "and refused for the USABLE reason -- BOOT_KSTACK_MIN_SIZE "
                   "is a derived total, not a second gate");
    TEST_ASSERT_EQ((uint64_t)(BOOT_KSTACK_MIN_SIZE - BOOT_KSTACK_GUARD_BYTES),
                   (uint64_t)BOOT_KSTACK_MIN_USABLE,
                   "the total floor must be exactly the usable floor plus one "
                   "guard page -- a drift here silently shortens every "
                   "accepted run");
    TEST_ASSERT(boot_stack_validate(BM_KS_BASE, (uint64_t)BOOT_KSTACK_MAX_SIZE,
                                    BM_KS_GUARD, &si, &err) != 0,
                "a run at exactly BOOT_KSTACK_MAX_SIZE must be accepted");

    /* base + size == BOOT_INFO_EARLY_MAP_END is the last address the boot
     * identity map covers, so it is inside the map, not above it. The
     * ceiling check is `>`, and this is what pins that. */
    TEST_ASSERT(boot_stack_validate(BOOT_INFO_EARLY_MAP_END - BM_KS_SIZE,
                                    BM_KS_SIZE, BM_KS_GUARD, &si, &err) != 0,
                "a run ending exactly at the identity-map end must be accepted");
    TEST_ASSERT_EQ((uint64_t)err, (uint64_t)BOOT_STACK_ERR_OK,
                   "the exact-ceiling run must report OK");
}

static void test_bm_kstack_scan_finds_the_lowest_touched_qword(void)
{
    static uint64_t buf[16];
    uint32_t i;

    for (i = 0u; i < 16u; i++)
        buf[i] = BOOT_KSTACK_POISON;

    /* All poison: the scan reports the full length, which is what makes
     * "nothing ran on this stack" distinguishable from "peak 0". */
    TEST_ASSERT_EQ(boot_stack_scan_first_touched(buf, sizeof(buf)),
                   (uint64_t)sizeof(buf),
                   "an untouched span must report its own length");

    /* Two writes with poison BETWEEN them. Scanning down from the top would
     * stop at index 12 and report a peak less than half the truth; the rule
     * is the LOWEST touched qword, not the first one found. */
    buf[12] = 0x1111111111111111ull;
    buf[4]  = 0x2222222222222222ull;
    TEST_ASSERT_EQ(boot_stack_scan_first_touched(buf, sizeof(buf)),
                   (uint64_t)(4u * sizeof(uint64_t)),
                   "the scan must report the LOWEST touched qword");

    buf[0] = 0x3333333333333333ull;
    TEST_ASSERT_EQ(boot_stack_scan_first_touched(buf, sizeof(buf)), 0ull,
                   "a write to the first qword must report offset 0");

    /* Caller bugs report "nothing touched" rather than dereferencing or
     * reading a partial qword past the end. */
    TEST_ASSERT_EQ(boot_stack_scan_first_touched((const void *)0, sizeof(buf)),
                   (uint64_t)sizeof(buf), "NULL must not be dereferenced");
    TEST_ASSERT_EQ(boot_stack_scan_first_touched(buf, 12ull), 12ull,
                   "a non-qword-multiple length must be refused, not rounded");
}

static void test_bm_kstack_contains_bounds_the_usable_span(void)
{
    struct boot_stack_info si;
    enum boot_stack_error err;
    uint64_t lo;
    uint64_t hi;

    TEST_ASSERT(boot_stack_validate(BM_KS_BASE, BM_KS_SIZE, BM_KS_GUARD,
                                    &si, &err) != 0,
                "fixture handoff must validate");
    lo = BM_KS_BASE + BM_KS_GUARD;
    hi = BM_KS_BASE + BM_KS_SIZE;

    TEST_ASSERT(boot_stack_contains(&si, lo, 8) != 0,
                "the first usable qword must be inside the span");
    TEST_ASSERT(boot_stack_contains(&si, hi - 8, 8) != 0,
                "the last usable qword must be inside the span");

    /* The guard is deliberately OUTSIDE the usable span: this predicate is
     * what the live RSP check uses, and an RSP inside the guard means the
     * stack has already overflowed, not that it is healthy. */
    TEST_ASSERT(boot_stack_contains(&si, BM_KS_BASE, 8) == 0,
                "the guard page must not count as usable stack");
    TEST_ASSERT(boot_stack_contains(&si, lo - 8, 8) == 0,
                "the qword below the usable floor must be outside");
    TEST_ASSERT(boot_stack_contains(&si, hi, 8) == 0,
                "the qword at the top must be outside (exclusive end)");
    TEST_ASSERT(boot_stack_contains(&si, hi - 4, 8) == 0,
                "a range straddling the top must be outside");
    TEST_ASSERT(boot_stack_contains(&si, lo, 0) == 0,
                "a zero-length range must never be reported as contained");
}

static void test_bm_kstack_scan_first_poison_is_the_mirror_scan(void)
{
    /* The pattern-mismatch discriminator. `first_touched` and `first_poison`
     * are NOT complements, and this test is what pins that: a used stack is a
     * MIX, so both scans return a small offset on the same buffer and neither
     * answer implies the other. */
    static uint64_t buf[8];
    uint64_t i;

    for (i = 0; i < 8; i++)
        buf[i] = BOOT_KSTACK_POISON;
    TEST_ASSERT_EQ(boot_stack_scan_first_poison(buf, sizeof(buf)), 0ull,
                   "an all-poison buffer reports poison at offset 0");
    TEST_ASSERT_EQ(boot_stack_scan_first_touched(buf, sizeof(buf)),
                   (uint64_t)sizeof(buf),
                   "an all-poison buffer reports nothing touched");

    /* A pattern mismatch: every qword holds a DIFFERENT fill, so no poison
     * survives anywhere. This is the case that used to be misreported as a
     * stack overflow. */
    for (i = 0; i < 8; i++)
        buf[i] = ~BOOT_KSTACK_POISON;
    TEST_ASSERT_EQ(boot_stack_scan_first_poison(buf, sizeof(buf)),
                   (uint64_t)sizeof(buf),
                   "a differently-filled buffer reports NO poison anywhere");
    TEST_ASSERT_EQ(boot_stack_scan_first_touched(buf, sizeof(buf)), 0ull,
                   "a differently-filled buffer looks entirely touched");

    /* A genuinely used stack: written at both ends, poison surviving in the
     * middle. Both scans find something, which is exactly why the halt path
     * asks the poison question and not the touched one. */
    for (i = 0; i < 8; i++)
        buf[i] = BOOT_KSTACK_POISON;
    buf[0] = 0xDEADBEEFull;
    buf[7] = 0xFEEDFACEull;
    TEST_ASSERT_EQ(boot_stack_scan_first_touched(buf, sizeof(buf)), 0ull,
                   "a used buffer is touched at its first qword");
    TEST_ASSERT_EQ(boot_stack_scan_first_poison(buf, sizeof(buf)), 8ull,
                   "a used buffer still holds poison at the second qword");

    /* THE AMBIGUOUS STATE, pinned as ambiguous. An overflow dense enough to
     * write every usable qword -- a large local buffer, deep recursion --
     * leaves exactly the same "no poison anywhere" reading as a producer
     * whose fill pattern is not this kernel's. The scan cannot separate them
     * and is not asked to; boot_stack_install_guard reports BOTH causes on
     * this reading rather than naming one. This case is here so that a later
     * change which starts treating no-poison as proof of a producer mismatch
     * has to delete a test that says otherwise. */
    for (i = 0; i < 8; i++)
        buf[i] = 0x1111111111111111ull + i;   /* dense writes, no poison left */
    TEST_ASSERT_EQ(boot_stack_scan_first_poison(buf, sizeof(buf)),
                   (uint64_t)sizeof(buf),
                   "a fully overwritten buffer is indistinguishable from a "
                   "pattern mismatch: both report NO poison");
    TEST_ASSERT_EQ(boot_stack_scan_first_touched(buf, sizeof(buf)), 0ull,
                   "a fully overwritten buffer is touched from its first qword");

    /* Same refusal shape as the sibling scan: a NULL pointer or a length that
     * is not a whole number of qwords reports "nothing found" rather than
     * reading off the end. */
    TEST_ASSERT_EQ(boot_stack_scan_first_poison((const void *)0, 64ull), 64ull,
                   "a NULL buffer reports nothing found");
    TEST_ASSERT_EQ(boot_stack_scan_first_poison(buf, 7ull), 7ull,
                   "a non-qword length reports nothing found");
}

static void test_bm_kstack_image_envelope_covers_image_and_bitmap(void)
{
    /* The envelope boot_stack_init refuses a run inside. It mirrors the
     * loader's bl_kstack_placement_ok kguard_hi arithmetic exactly, so this
     * test is what keeps the two halves of one rule from drifting apart:
     * image end rounded UP to a page, plus the bitmap for the whole capped
     * physical range (one bit per 4 KiB frame == cap / 32768 bytes), plus one
     * page of linker padding. */
    const uint64_t cap = PMM_PHYS_ADDR_CAP;
    const uint64_t bitmap_bytes = cap / 32768u;

    TEST_ASSERT_EQ(boot_stack_image_envelope_end(0x800000ull, cap),
                   0x800000ull + bitmap_bytes + 4096ull,
                   "a page-aligned image end needs no rounding");

    /* Rounding is UP, not down: an image ending mid-page still owns that
     * page, so truncating would place the bitmap on top of the image tail. */
    TEST_ASSERT_EQ(boot_stack_image_envelope_end(0x7ffa54ull, cap),
                   0x800000ull + bitmap_bytes + 4096ull,
                   "an unaligned image end rounds up to the next page");
    TEST_ASSERT_EQ(boot_stack_image_envelope_end(0x800001ull, cap),
                   0x801000ull + bitmap_bytes + 4096ull,
                   "one byte past a page boundary consumes a whole page");

    /* The 4 GiB cap yields a 128 KiB bitmap. Pinned as a number rather than
     * recomputed, so a change to either the cap or the frame size has to be
     * stated here rather than silently tracked. */
    TEST_ASSERT_EQ(bitmap_bytes, 131072ull,
                   "the capped physical range needs a 128 KiB bitmap");

    /* Direction control: the envelope must strictly EXCEED the image end,
     * otherwise the whole rule degrades to a no-op that still reads as a
     * check. */
    TEST_ASSERT(boot_stack_image_envelope_end(0x800000ull, cap) > 0x800000ull,
                "the envelope extends past the image end");
}

static void test_bm_kstack_image_envelope_rejects_the_load_address(void)
{
    struct boot_stack_info si;
    enum boot_stack_error err = BOOT_STACK_ERR_OK;
    uint64_t img_hi;

    /* THE case this rule exists for. src/boot/linker.ld loads the kernel at
     * exactly 1 MiB, and boot_stack_validate accepts that base because its
     * LOW_MEM rule is satisfied there. Until this envelope existed the only
     * net was in pmm_init, which runs after Phase 0 has already been pushing
     * frames into the kernel's own image. */
    TEST_ASSERT(boot_stack_validate(0x100000ull, BM_KS_SIZE, BM_KS_GUARD,
                                    &si, &err) == 1,
                "the pure validator still accepts the load address");

    /* A SYNTHETIC image end, deliberately far below the real one. The
     * envelope's top must stay clear of USER_PT_WINDOW_BASE (0x800000), or
     * the control case below would be refused by the user-window rule instead
     * of validating -- which is how the first draft of this test failed: it
     * used 0x800000 as the synthetic image end, putting its "just above the
     * envelope" base squarely inside the window. The arithmetic under test is
     * parameterised precisely so it can be exercised without depending on
     * where this kernel happens to end. */
    img_hi = boot_stack_image_envelope_end(0x300000ull, PMM_PHYS_ADDR_CAP);
    TEST_ASSERT(img_hi < (uint64_t)USER_PT_WINDOW_BASE,
                "the synthetic envelope stays clear of the user PT window");
    TEST_ASSERT(boot_stack_overlaps(&si, 0x100000ull, img_hi - 0x100000ull) == 1,
                "a run based at the kernel load address is inside the envelope");

    /* Refusal direction control: a run placed above the envelope is NOT
     * caught, so the check discriminates rather than always firing. */
    TEST_ASSERT(boot_stack_validate(img_hi, BM_KS_SIZE, BM_KS_GUARD,
                                    &si, &err) == 1,
                "a run just above the envelope validates");
    TEST_ASSERT(boot_stack_overlaps(&si, 0x100000ull, img_hi - 0x100000ull) == 0,
                "a run above the envelope is outside it");
}

static void test_bm_kstack_contains_rejects_wrap_and_invalid(void)
{
    struct boot_stack_info si;
    struct boot_stack_info invalid;
    enum boot_stack_error err;

    invalid.base = BM_KS_BASE;
    invalid.size = BM_KS_SIZE;
    invalid.guard_size = BM_KS_GUARD;
    invalid.valid = 0;
    TEST_ASSERT(boot_stack_contains(&invalid, BM_KS_BASE + BM_KS_GUARD, 8) == 0,
                "an unvalidated struct must never report containment");
    TEST_ASSERT(boot_stack_contains((const struct boot_stack_info *)0,
                                    BM_KS_BASE, 8) == 0,
                "NULL must not be dereferenced");

    TEST_ASSERT(boot_stack_validate(BM_KS_BASE, BM_KS_SIZE, BM_KS_GUARD,
                                    &si, &err) != 0,
                "fixture handoff must validate");
    /* addr + len wrapping past UINT64_MAX would otherwise compute a hi bound
     * BELOW lo and satisfy the range test for an address nowhere near the
     * stack. */
    TEST_ASSERT(boot_stack_contains(&si, 0xFFFFFFFFFFFFFFF8ull, 16) == 0,
                "a wrapping range must be refused, not wrapped into the span");
}

static void test_bm_kstack_live_run_is_reserved_and_measured(void)
{
    const struct boot_stack_info *si = boot_stack_get();
    uint64_t lo;
    uint64_t hi;
    uint64_t frame;

    TEST_ASSERT(si != (const struct boot_stack_info *)0,
                "boot_stack_get must never return NULL");
    TEST_ASSERT(si->valid != 0,
                "the live boot ran without a validated kernel stack");
    TEST_ASSERT((si->base & 0xFFFu) == 0, "live stack base must be page-aligned");
    TEST_ASSERT(si->guard_size < si->size,
                "live guard must leave usable stack");

    /* THE acceptance property of section 32, asserted against the live boot
     * rather than a fixture: every frame of the run the kernel executes on
     * must be unavailable to the allocator. pmm_init frees all LoaderCode/Data
     * moments before reserving this run, so a pass here cannot be an accident
     * of the free walk having skipped it. */
    lo = si->base;
    hi = si->base + si->size;
    for (frame = lo; frame < hi; frame += 4096ull) {
        if (pmm_frame_is_free((uintptr_t)frame)) {
            TEST_ASSERT(0,
                        "a frame of the live kernel boot stack is ALLOCATABLE");
            return;
        }
    }

    TEST_ASSERT(boot_stack_guarded() != 0,
                "the live boot stack shipped UNGUARDED -- guard table full "
                "or huge-page split failed");
}

static void test_bm_pmm_frame_is_free_is_calibrated(void)
{
    uintptr_t f;

    /* CONTROL for the assertion above. Without this, an implementation of
     * pmm_frame_is_free() that returned 0 unconditionally would make the
     * whole-run sweep pass while proving nothing at all -- the sweep only
     * ever asks about frames it expects to be reserved, so it can never
     * observe the oracle saying "free". Both answers must be reachable
     * before either is evidence. */
    f = pmm_alloc_frame();
    TEST_ASSERT(f != 0, "fixture: a frame must be allocatable");
    TEST_ASSERT(pmm_frame_is_free(f) == 0,
                "an ALLOCATED frame must not be reported free");

    pmm_free_frame(f);
    TEST_ASSERT(pmm_frame_is_free(f) != 0,
                "a FREED frame must be reported free -- the oracle is stuck "
                "at 'used' and every reservation claim built on it is void");

    /* Re-take it so the suite leaves the allocator as it found it, and so the
     * transition is observed in both directions. */
    TEST_ASSERT_EQ((uint64_t)pmm_alloc_frame(), (uint64_t)f,
                   "the just-freed frame must be handed back next");
    TEST_ASSERT(pmm_frame_is_free(f) == 0,
                "the re-allocated frame must not be reported free");
    pmm_free_frame(f);

    /* Out of range is conservatively NOT free: a frame the allocator does not
     * know about is one it will never hand out. */
    TEST_ASSERT(pmm_frame_is_free((uintptr_t)(pmm_get_total_frames() * 4096ull)) == 0,
                "an out-of-range address must never be reported free");
}

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
    test_suite_register_cat("BM: BSP entry stack is page-aligned",
                            test_bm_bsp_entry_stack_is_page_aligned,
                            TEST_CAT_BOOT);
    test_suite_register_cat("BM: BSP entry stack guard is registered",
                            test_bm_bsp_entry_stack_guard_is_registered,
                            TEST_CAT_BOOT);
    test_suite_register_cat("BM: BSP entry stack peak is bounded",
                            test_bm_bsp_entry_stack_peak_is_bounded,
                            TEST_CAT_BOOT);
    test_suite_register_cat("BM: #DF classifier names the guard",
                            test_bm_df_classifier_names_the_guard,
                            TEST_CAT_BOOT);
    test_suite_register_cat("BM: #DF classifier uses the registry",
                            test_bm_df_classifier_uses_the_guard_registry,
                            TEST_CAT_BOOT);
    test_suite_register_cat("BM: kstack accepts a well-formed handoff",
                            test_bm_kstack_accepts_a_well_formed_handoff,
                            TEST_CAT_BOOT);
    test_suite_register_cat("BM: kstack refuses each malformed shape",
                            test_bm_kstack_refuses_each_malformed_shape,
                            TEST_CAT_BOOT);
    test_suite_register_cat("BM: kstack refusal leaves output untouched",
                            test_bm_kstack_refusal_leaves_the_output_untouched,
                            TEST_CAT_BOOT);
    test_suite_register_cat("BM: kstack contains bounds the usable span",
                            test_bm_kstack_contains_bounds_the_usable_span,
                            TEST_CAT_BOOT);
    test_suite_register_cat("BM: kstack first-poison scan mirrors first-touched",
                            test_bm_kstack_scan_first_poison_is_the_mirror_scan,
                            TEST_CAT_BOOT);
    test_suite_register_cat("BM: kstack image envelope covers image + bitmap",
                            test_bm_kstack_image_envelope_covers_image_and_bitmap,
                            TEST_CAT_BOOT);
    test_suite_register_cat("BM: kstack image envelope rejects load address",
                            test_bm_kstack_image_envelope_rejects_the_load_address,
                            TEST_CAT_BOOT);
    test_suite_register_cat("BM: kstack contains rejects wrap and invalid",
                            test_bm_kstack_contains_rejects_wrap_and_invalid,
                            TEST_CAT_BOOT);
    test_suite_register_cat("BM: live kernel stack run is reserved",
                            test_bm_kstack_live_run_is_reserved_and_measured,
                            TEST_CAT_BOOT);
    test_suite_register_cat("BM: kstack accepts at exact contract bounds",
                            test_bm_kstack_accepts_at_the_exact_contract_bounds,
                            TEST_CAT_BOOT);
    test_suite_register_cat("BM: kstack scan finds lowest touched qword",
                            test_bm_kstack_scan_finds_the_lowest_touched_qword,
                            TEST_CAT_BOOT);
    test_suite_register_cat("BM: pmm_frame_is_free oracle is calibrated",
                            test_bm_pmm_frame_is_free_is_calibrated,
                            TEST_CAT_BOOT);
}

#endif /* KERNEL_TESTS */
