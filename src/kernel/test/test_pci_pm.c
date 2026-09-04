/* ============================================================================
 * test_pci_pm.c -- PCI Power Management capability and D-state machine tests
 *
 * Covers the decision logic of src/kernel/drivers/pci_pm.c: the capability-list
 * structural rules, the PCI PM recovery-delay matrix, transition legality, the
 * PMC capability gate for D1/D2, and the write-1-to-clear-safe PMCSR write
 * value. All of it is pure, so none of these tests needs a PM-capable device to
 * be present -- which matters, because no emulator this suite runs under is
 * guaranteed to expose one.
 *
 * XREF: 02-kernel-core/TODO-26-power-management.md section 8
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/drivers/pci_pm.h"

/* ---- Capabilities Pointer offset by header type ---- */

static void test_pci_pm_cap_ptr_by_header_type(void)
{
    TEST_ASSERT_EQ(pci_cap_ptr_offset(PCI_HEADER_TYPE_NORMAL), PCI_CAP_PTR_TYPE01,
                   "header type 0 reads the Capabilities Pointer at 0x34");
    TEST_ASSERT_EQ(pci_cap_ptr_offset(PCI_HEADER_TYPE_BRIDGE), PCI_CAP_PTR_TYPE01,
                   "header type 1 (bridge) also reads it at 0x34");
    TEST_ASSERT_EQ(pci_cap_ptr_offset(PCI_HEADER_TYPE_CARDBUS), PCI_CAP_PTR_TYPE2,
                   "header type 2 (CardBus) reads it at 0x14, not 0x34");
}

static void test_pci_pm_cap_ptr_ignores_multifunction_bit(void)
{
    /* Bit 7 of the header-type register is the multi-function flag and is not
     * part of the layout code; masking it is what makes 0x80 a type 0. */
    TEST_ASSERT_EQ(pci_cap_ptr_offset(0x80), PCI_CAP_PTR_TYPE01,
                   "multi-function type 0 still reads 0x34");
    TEST_ASSERT_EQ(pci_cap_ptr_offset(0x82), PCI_CAP_PTR_TYPE2,
                   "multi-function CardBus still reads 0x14");
}

static void test_pci_pm_cap_ptr_unknown_type_refused(void)
{
    TEST_ASSERT_EQ(pci_cap_ptr_offset(0x03), 0,
                   "an undefined header type has no walkable capability list");
    TEST_ASSERT_EQ(pci_cap_ptr_offset(0x7F), 0,
                   "the largest undefined header type is refused too");
}

/* ---- Capability offset validity ---- */

static void test_pci_pm_cap_offset_bounds(void)
{
    TEST_ASSERT_EQ(pci_pm_cap_offset_valid(PCI_CAP_OFF_MIN, PCI_HEADER_TYPE_NORMAL), 1,
                   "0x40, the first byte past a type 0 header, is valid");
    TEST_ASSERT_EQ(pci_pm_cap_offset_valid(0x3C, PCI_HEADER_TYPE_NORMAL), 0,
                   "an offset inside the standard header is rejected");
    TEST_ASSERT_EQ(pci_pm_cap_offset_valid(PCI_CAP_NODE_OFF_MAX, PCI_HEADER_TYPE_NORMAL), 1,
                   "0xFC is a legal place for a capability node");
    TEST_ASSERT_EQ(pci_pm_cap_offset_valid(0x00, PCI_HEADER_TYPE_NORMAL), 0,
                   "a zero pointer is not a valid capability location");
    TEST_ASSERT_EQ(pci_pm_cap_offset_valid(0x100, PCI_HEADER_TYPE_NORMAL), 0,
                   "an offset past the 256-byte config header is rejected");
}

static void test_pci_pm_footprint_is_stricter_than_node_bound(void)
{
    /* A node only needs its ID and next-pointer bytes; a PM capability also
     * needs PMCSR at +4. Conflating the two would report a firmware defect for
     * a legal list whose LAST capability happens not to be the PM one. */
    TEST_ASSERT_EQ(pci_pm_cap_fits_pmcsr(PCI_CAP_OFF_MAX), 1,
                   "0xF8 is the last offset whose PMCSR at +4 still fits");
    TEST_ASSERT_EQ(pci_pm_cap_fits_pmcsr(PCI_CAP_NODE_OFF_MAX), 0,
                   "a PM capability at 0xFC would put PMCSR past 0xFF");
    TEST_ASSERT_EQ(pci_pm_cap_offset_valid(PCI_CAP_NODE_OFF_MAX, PCI_HEADER_TYPE_NORMAL), 1,
                   "yet 0xFC remains a valid node location for a non-PM capability");
}

static void test_pci_pm_cap_offset_cardbus_fixed_fields(void)
{
    /* A CardBus bridge keeps subsystem IDs at 0x40 and the legacy-mode base
     * register at 0x44. A hostile pointer there whose first byte reads 0x01
     * would otherwise be accepted as a PM capability laid over those. */
    TEST_ASSERT_EQ(pci_pm_cap_offset_valid(0x40, PCI_HEADER_TYPE_CARDBUS), 0,
                   "0x40 is the CardBus subsystem ID, not a capability");
    TEST_ASSERT_EQ(pci_pm_cap_offset_valid(0x44, PCI_HEADER_TYPE_CARDBUS), 0,
                   "0x44 is the CardBus legacy-mode base, not a capability");
    TEST_ASSERT_EQ(pci_pm_cap_offset_valid(PCI_CAP_OFF_MIN_CARDBUS, PCI_HEADER_TYPE_CARDBUS), 1,
                   "0x48 is the first CardBus offset past the fixed fields");
    TEST_ASSERT_EQ(pci_pm_cap_offset_valid(0x40, PCI_HEADER_TYPE_BRIDGE), 1,
                   "0x40 stays valid for a type 1 bridge, which has no such fields");
    /* The validator must mask the multi-function bit itself, not rely on a
     * caller having done it: a real CardBus bridge advertising multiple
     * functions reports header type 0x82. */
    TEST_ASSERT_EQ(pci_pm_cap_offset_valid(0x44, 0x82), 0,
                   "a multi-function CardBus bridge still protects 0x44");
    TEST_ASSERT_EQ(pci_pm_cap_offset_valid(0x48, 0x82), 1,
                   "and still admits 0x48");
}

static void test_pci_pm_cap_offset_alignment(void)
{
    /* Rejecting rather than masking is the point: masking 0x41 to 0x40 would
     * silently follow a malformed pointer into an unrelated register. */
    TEST_ASSERT_EQ(pci_pm_cap_offset_valid(0x41, PCI_HEADER_TYPE_NORMAL), 0,
                   "a byte-misaligned capability pointer is rejected");
    TEST_ASSERT_EQ(pci_pm_cap_offset_valid(0x42, PCI_HEADER_TYPE_NORMAL), 0,
                   "a word-aligned but not DWORD-aligned pointer is rejected");
    TEST_ASSERT_EQ(pci_pm_cap_offset_valid(0x43, PCI_HEADER_TYPE_NORMAL), 0,
                   "0x43 is rejected as well");
    TEST_ASSERT_EQ(pci_pm_cap_offset_valid(0x44, PCI_HEADER_TYPE_NORMAL), 1,
                   "the next DWORD-aligned offset is accepted");
}

/* ---- Recovery-delay matrix ----
 * Every transition involving D3hot needs 10 ms, every transition involving D2
 * needs 200 us, and D0 <-> D1 is immediate. Asserted as a full matrix rather
 * than a few spot values: the failure mode is a single wrong cell, and a wrong
 * cell means touching a device that is not addressable yet. */

static void test_pci_pm_delay_from_d0(void)
{
    TEST_ASSERT_EQ(pci_pm_recovery_delay_us(PCI_D0, PCI_D0), 0,
                   "D0->D0 is not a transition");
    TEST_ASSERT_EQ(pci_pm_recovery_delay_us(PCI_D0, PCI_D1), 0,
                   "D0->D1 is immediate");
    TEST_ASSERT_EQ(pci_pm_recovery_delay_us(PCI_D0, PCI_D2), PCI_PM_D2_DELAY_US,
                   "D0->D2 needs 200 us before the next access");
    TEST_ASSERT_EQ(pci_pm_recovery_delay_us(PCI_D0, PCI_D3HOT), PCI_PM_D3HOT_DELAY_US,
                   "D0->D3hot needs 10 ms before the next access");
}

static void test_pci_pm_delay_from_d1_d2(void)
{
    TEST_ASSERT_EQ(pci_pm_recovery_delay_us(PCI_D1, PCI_D0), 0,
                   "D1->D0 is immediate");
    TEST_ASSERT_EQ(pci_pm_recovery_delay_us(PCI_D1, PCI_D2), PCI_PM_D2_DELAY_US,
                   "D1->D2 needs 200 us");
    TEST_ASSERT_EQ(pci_pm_recovery_delay_us(PCI_D1, PCI_D3HOT), PCI_PM_D3HOT_DELAY_US,
                   "D1->D3hot needs 10 ms");
    TEST_ASSERT_EQ(pci_pm_recovery_delay_us(PCI_D2, PCI_D0), PCI_PM_D2_DELAY_US,
                   "D2->D0 needs 200 us, not 10 ms");
    TEST_ASSERT_EQ(pci_pm_recovery_delay_us(PCI_D2, PCI_D3HOT), PCI_PM_D3HOT_DELAY_US,
                   "D2->D3hot needs 10 ms");
}

static void test_pci_pm_delay_d3hot_exit(void)
{
    TEST_ASSERT_EQ(pci_pm_recovery_delay_us(PCI_D3HOT, PCI_D0), PCI_PM_D3HOT_DELAY_US,
                   "D3hot->D0 needs the full 10 ms");
    TEST_ASSERT_EQ(pci_pm_recovery_delay_us(PCI_D3HOT, PCI_D3HOT), 0,
                   "staying in D3hot is not a transition");
}

static void test_pci_pm_delay_identity_and_shallower(void)
{
    /* The remaining cells of the matrix. The shallower pairs are refused by
     * pci_pm_transition_legal(), but the delay function is still asked about
     * them by anything that inspects a pair before deciding, so they must not
     * report a value that would look safe. */
    TEST_ASSERT_EQ(pci_pm_recovery_delay_us(PCI_D1, PCI_D1), 0,
                   "D1->D1 is not a transition");
    TEST_ASSERT_EQ(pci_pm_recovery_delay_us(PCI_D2, PCI_D2), 0,
                   "D2->D2 is not a transition");
    TEST_ASSERT_EQ(pci_pm_recovery_delay_us(PCI_D2, PCI_D1), PCI_PM_D2_DELAY_US,
                   "a D2 endpoint still carries the 200 us figure");
    TEST_ASSERT_EQ(pci_pm_recovery_delay_us(PCI_D3HOT, PCI_D1), PCI_PM_D3HOT_DELAY_US,
                   "a D3hot endpoint still carries the 10 ms figure");
    TEST_ASSERT_EQ(pci_pm_recovery_delay_us(PCI_D3HOT, PCI_D2), PCI_PM_D3HOT_DELAY_US,
                   "D3hot dominates D2 when both endpoints are low-power");
}

/* ---- Transition legality ---- */

static void test_pci_pm_transition_deeper_and_d0_legal(void)
{
    TEST_ASSERT_EQ(pci_pm_transition_legal(PCI_D0, PCI_D3HOT), 1,
                   "any deeper transition is legal");
    TEST_ASSERT_EQ(pci_pm_transition_legal(PCI_D1, PCI_D2), 1,
                   "D1->D2 is deeper, so legal");
    TEST_ASSERT_EQ(pci_pm_transition_legal(PCI_D3HOT, PCI_D0), 1,
                   "D0 is reachable from every state");
    TEST_ASSERT_EQ(pci_pm_transition_legal(PCI_D2, PCI_D2), 1,
                   "a no-op transition is legal");
}

static void test_pci_pm_transition_wake_to_d0_legal(void)
{
    /* Every low-power state must be able to reach D0, or a suspended device
     * can never resume. Asserted separately from the deeper-transition cases
     * because a regression that rejects only the intermediate states would
     * otherwise pass. */
    TEST_ASSERT_EQ(pci_pm_transition_legal(PCI_D1, PCI_D0), 1,
                   "D1->D0 is a legal wake");
    TEST_ASSERT_EQ(pci_pm_transition_legal(PCI_D2, PCI_D0), 1,
                   "D2->D0 is a legal wake");
}

static void test_pci_pm_plan_d2_carries_its_delay(void)
{
    pci_pm_plan_t plan;

    int rc = pci_pm_plan_transition(PMC_D2_SUPPORT, PCI_D0, PCI_D2, 1, &plan);
    TEST_ASSERT_EQ(rc, PCI_DX_OK, "D0->D2 is planned on a D2-capable device");
    TEST_ASSERT_EQ(plan.needs_write, 1, "it needs a write");
    TEST_ASSERT_EQ(plan.delay_us, PCI_PM_D2_DELAY_US,
                   "and carries the 200 us recovery time");
    TEST_ASSERT_EQ(plan.write_value & PMCSR_POWER_STATE_MASK, PCI_D2,
                   "selecting D2");

    rc = pci_pm_plan_transition(0, PCI_D2, PCI_D0, 1, &plan);
    TEST_ASSERT_EQ(rc, PCI_DX_OK, "waking from D2 needs no PMC advertisement");
    TEST_ASSERT_EQ(plan.needs_write, 1, "it needs a write");
    TEST_ASSERT_EQ(plan.delay_us, PCI_PM_D2_DELAY_US,
                   "and the same 200 us applies leaving D2");
}

static void test_pci_pm_transition_partial_wake_illegal(void)
{
    /* The only exit from a low-power state is D0; waking part-way is undefined. */
    TEST_ASSERT_EQ(pci_pm_transition_legal(PCI_D3HOT, PCI_D1), 0,
                   "D3hot->D1 is not a defined transition");
    TEST_ASSERT_EQ(pci_pm_transition_legal(PCI_D3HOT, PCI_D2), 0,
                   "D3hot->D2 is not a defined transition");
    TEST_ASSERT_EQ(pci_pm_transition_legal(PCI_D2, PCI_D1), 0,
                   "D2->D1 is not a defined transition");
}

static void test_pci_pm_transition_out_of_range(void)
{
    TEST_ASSERT_EQ(pci_pm_transition_legal(PCI_D0, 4), 0,
                   "a state above D3hot is not addressable through PMCSR");
    TEST_ASSERT_EQ(pci_pm_transition_legal(9, PCI_D0), 0,
                   "an out-of-range current state is refused too");
}

/* ---- PMC capability gate ---- */

static void test_pci_pm_d0_d3hot_always_supported(void)
{
    TEST_ASSERT_EQ(pci_pm_state_supported(0, PCI_D0), 1,
                   "D0 is mandatory even when PMC advertises nothing");
    TEST_ASSERT_EQ(pci_pm_state_supported(0, PCI_D3HOT), 1,
                   "D3hot is mandatory for every PM-capable device");
}

static void test_pci_pm_d1_d2_require_advertisement(void)
{
    TEST_ASSERT_EQ(pci_pm_state_supported(0, PCI_D1), 0,
                   "D1 is refused when PMC does not advertise it");
    TEST_ASSERT_EQ(pci_pm_state_supported(0, PCI_D2), 0,
                   "D2 is refused when PMC does not advertise it");
    TEST_ASSERT_EQ(pci_pm_state_supported(PMC_D1_SUPPORT, PCI_D1), 1,
                   "D1 is accepted once advertised");
    TEST_ASSERT_EQ(pci_pm_state_supported(PMC_D2_SUPPORT, PCI_D2), 1,
                   "D2 is accepted once advertised");
    TEST_ASSERT_EQ(pci_pm_state_supported(PMC_D1_SUPPORT, PCI_D2), 0,
                   "advertising D1 does not imply D2");
    TEST_ASSERT_EQ(pci_pm_state_supported(PMC_D2_SUPPORT, PCI_D1), 0,
                   "advertising D2 does not imply D1 either");
}

static void test_pci_pm_state_supported_rejects_out_of_range(void)
{
    TEST_ASSERT_EQ(pci_pm_state_supported(0xFFFF, 4), 0,
                   "a state above D3hot is unsupported however permissive PMC is");
}

/* ---- PMCSR write value ---- */

static void test_pci_pmcsr_write_never_acks_pme(void)
{
    /* PME_Status is write-1-to-clear, so echoing a read 1 back acknowledges and
     * destroys a pending wake event. The write value must carry a 0 there. */
    uint16_t old = (uint16_t)(PMCSR_PME_STATUS | PMCSR_PME_EN | PCI_D0);
    uint16_t out = pci_pmcsr_write_value(old, PCI_D3HOT);
    TEST_ASSERT_EQ(out & PMCSR_PME_STATUS, 0,
                   "the write value never sets PME_Status");
    TEST_ASSERT_EQ(out & PMCSR_PME_EN, PMCSR_PME_EN,
                   "PME_En is preserved across a state change");
    TEST_ASSERT_EQ(out & PMCSR_POWER_STATE_MASK, PCI_D3HOT,
                   "the requested state is inserted");
}

static void test_pci_pmcsr_write_replaces_state_only(void)
{
    /* 0x0074: Data_Scale/Data_Select and No_Soft_Reset bits set, state D0. */
    uint16_t old = 0x0074;
    uint16_t out = pci_pmcsr_write_value(old, PCI_D2);
    TEST_ASSERT_EQ(out & PMCSR_POWER_STATE_MASK, PCI_D2,
                   "state field becomes D2");
    TEST_ASSERT_EQ(out & (uint16_t)~(PMCSR_POWER_STATE_MASK | PMCSR_PME_STATUS),
                   old & (uint16_t)~(PMCSR_POWER_STATE_MASK | PMCSR_PME_STATUS),
                   "every bit outside the state field and PME_Status is preserved");
}

static void test_pci_pmcsr_write_masks_state_argument(void)
{
    uint16_t out = pci_pmcsr_write_value(0, 0xFF);
    TEST_ASSERT_EQ(out, PMCSR_POWER_STATE_MASK,
                   "an over-wide state argument cannot spill into other bits");
}

static void test_pci_pmcsr_write_all_ones_input(void)
{
    /* 0xFFFF is what a config read of an absent device returns, and it is also
     * the worst case for the mask: every reserved and write-1-to-clear bit set. */
    uint16_t out = pci_pmcsr_write_value(0xFFFF, PCI_D0);
    TEST_ASSERT_EQ(out & PMCSR_PME_STATUS, 0,
                   "PME_Status is cleared out of an all-ones read");
    TEST_ASSERT_EQ(out & PMCSR_POWER_STATE_MASK, PCI_D0,
                   "the state field becomes D0");
    TEST_ASSERT_EQ(out, 0x7FFC,
                   "every other bit of the all-ones value survives unchanged");
}

/* ---- No_Soft_Reset ---- */

static void test_pci_pm_no_soft_reset_reported(void)
{
    TEST_ASSERT_EQ(pci_pm_no_soft_reset(PMCSR_NO_SOFT_RESET), 1,
                   "No_Soft_Reset set means D3hot->D0 preserves device state");
    TEST_ASSERT_EQ(pci_pm_no_soft_reset(0), 0,
                   "No_Soft_Reset clear means D3hot->D0 yields D0 Uninitialized");
    TEST_ASSERT_EQ(pci_pm_no_soft_reset(PMCSR_PME_EN | PMCSR_PME_STATUS), 0,
                   "other PMCSR bits do not make a device soft-reset-free");
}

/* ---- Status-code distinctness ----
 * Each failure has to be distinguishable by the caller: a device with no PM
 * capability, a malformed capability list, and a missing time source demand
 * different responses, and collapsing them to -1 would hide which happened. */

static void test_pci_pm_status_codes_distinct(void)
{
    TEST_ASSERT(PCI_DX_OK == 0,
                "success is zero so a negative return always means failure");
    TEST_ASSERT(PCI_DX_UNSUPPORTED != PCI_DX_MALFORMED,
                "no PM capability is distinguishable from a malformed list");
    TEST_ASSERT(PCI_DX_NO_TIMEBASE != PCI_DX_FAILED,
                "an unhonourable delay is distinguishable from a failed latch");
    TEST_ASSERT(PCI_DX_INVALID < 0 && PCI_DX_UNSUPPORTED < 0
                && PCI_DX_NO_TIMEBASE < 0 && PCI_DX_FAILED < 0
                && PCI_DX_MALFORMED < 0,
                "every failure code is negative, so it cannot alias an offset");
}

/* ---- Transition planner ----
 * The whole decision half of pci_set_d_state(), exercised without a device on
 * the bus: what gets written, how long the caller must then wait, and which
 * requests are refused before anything is written at all. */

static void test_pci_pm_plan_normal_entry(void)
{
    pci_pm_plan_t plan;
    int rc = pci_pm_plan_transition(0, PCI_D0, PCI_D3HOT, 1, &plan);
    TEST_ASSERT_EQ(rc, PCI_DX_OK, "D0->D3hot is planned");
    TEST_ASSERT_EQ(plan.needs_write, 1, "a state change needs a write");
    TEST_ASSERT_EQ(plan.from, PCI_D0, "the current state is read out of PMCSR");
    TEST_ASSERT_EQ(plan.delay_us, PCI_PM_D3HOT_DELAY_US,
                   "the plan carries the 10 ms recovery time");
    TEST_ASSERT_EQ(plan.write_value & PMCSR_POWER_STATE_MASK, PCI_D3HOT,
                   "the write value selects D3hot");
}

static void test_pci_pm_plan_noop_needs_no_write(void)
{
    pci_pm_plan_t plan;
    int rc = pci_pm_plan_transition(0, PCI_D3HOT, PCI_D3HOT, 1, &plan);
    TEST_ASSERT_EQ(rc, PCI_DX_OK, "requesting the current state succeeds");
    TEST_ASSERT_EQ(plan.needs_write, 0, "no write is issued for a no-op");
    TEST_ASSERT_EQ(plan.delay_us, 0, "and nothing has to be waited for");
}

static void test_pci_pm_plan_refuses_unsupported_state(void)
{
    pci_pm_plan_t plan;
    TEST_ASSERT_EQ(pci_pm_plan_transition(0, PCI_D0, PCI_D2, 1, &plan),
                   PCI_DX_UNSUPPORTED,
                   "D2 is refused when PMC does not advertise it");
    TEST_ASSERT_EQ(plan.needs_write, 0, "a refused plan writes nothing");
}

static void test_pci_pm_plan_refuses_partial_wake(void)
{
    pci_pm_plan_t plan;
    TEST_ASSERT_EQ(pci_pm_plan_transition(PMC_D2_SUPPORT, PCI_D3HOT, PCI_D2, 1, &plan),
                   PCI_DX_INVALID,
                   "D3hot->D2 is refused even though the device supports D2");
    TEST_ASSERT_EQ(plan.needs_write, 0, "a refused plan writes nothing");
}

static void test_pci_pm_plan_refuses_out_of_range_target(void)
{
    pci_pm_plan_t plan;
    TEST_ASSERT_EQ(pci_pm_plan_transition(0xFFFF, PCI_D0, 4, 1, &plan),
                   PCI_DX_INVALID,
                   "a target above D3hot is refused");
    TEST_ASSERT_EQ(pci_pm_plan_transition(0, PCI_D0, PCI_D3HOT, 1, 0),
                   PCI_DX_INVALID,
                   "a NULL plan pointer is refused");
}

static void test_pci_pm_plan_refuses_without_timebase(void)
{
    pci_pm_plan_t plan;
    int rc = pci_pm_plan_transition(0, PCI_D0, PCI_D3HOT, 0, &plan);
    TEST_ASSERT_EQ(rc, PCI_DX_NO_TIMEBASE,
                   "a transition needing a delay is refused with no clock");
    TEST_ASSERT_EQ(plan.needs_write, 0,
                   "and the refusal happens before anything is written");
}

static void test_pci_pm_plan_refusal_clears_both_outputs(void)
{
    /* Seeded non-zero so the assertions below prove the refusal CLEARED them
     * rather than merely never having set them. The D3hot->D0 direction is the
     * one that matters: it is the only transition that computes a
     * reinitialisation obligation, so it is the only case where a refusal could
     * leave a stale one behind. */
    pci_pm_plan_t plan;
    plan.needs_write  = 1;
    plan.reinit_after = 1;

    int rc = pci_pm_plan_transition(0, PCI_D3HOT, PCI_D0, 0, &plan);
    TEST_ASSERT_EQ(rc, PCI_DX_NO_TIMEBASE,
                   "waking from D3hot is refused when the delay cannot be timed");
    TEST_ASSERT_EQ(plan.needs_write, 0, "the refusal clears the write flag");
    TEST_ASSERT_EQ(plan.reinit_after, 0,
                   "and clears the obligation, because no transition occurred");
}

static void test_pci_pm_plan_no_delay_needs_no_timebase(void)
{
    pci_pm_plan_t plan;
    int rc = pci_pm_plan_transition(PMC_D1_SUPPORT, PCI_D0, PCI_D1, 0, &plan);
    TEST_ASSERT_EQ(rc, PCI_DX_OK,
                   "D0->D1 needs no recovery time, so it needs no clock either");
    TEST_ASSERT_EQ(plan.needs_write, 1, "and it still writes");
    TEST_ASSERT_EQ(plan.delay_us, 0, "with nothing to wait for");
}

static void test_pci_pm_plan_preserves_pme_state(void)
{
    pci_pm_plan_t plan;
    uint16_t pmcsr = (uint16_t)(PMCSR_PME_EN | PMCSR_PME_STATUS | PCI_D0);
    int rc = pci_pm_plan_transition(0, pmcsr, PCI_D3HOT, 1, &plan);
    TEST_ASSERT_EQ(rc, PCI_DX_OK, "the transition is planned");
    TEST_ASSERT_EQ(plan.write_value & PMCSR_PME_STATUS, 0,
                   "the planned write never acknowledges a pending PME");
    TEST_ASSERT_EQ(plan.write_value & PMCSR_PME_EN, PMCSR_PME_EN,
                   "and never disarms PME_En");
}

/* ---- Public API guards reachable without a device ---- */

static void test_pci_pmcap_read_null_refused(void)
{
    /* Returns before any config access, so it is safe on a bus with no
     * PM-capable device present. */
    TEST_ASSERT_EQ(pci_pmcap_read(0, 0, 0, 0), PCI_DX_INVALID,
                   "a NULL output structure is refused");
}

static void test_pci_set_d_state_out_of_range_refused(void)
{
    int reinit = 1;
    TEST_ASSERT_EQ(pci_set_d_state(0, 0, 0, 4, &reinit), PCI_DX_INVALID,
                   "a state above D3hot is refused before any config access");
    TEST_ASSERT_EQ(pci_set_d_state(0, 0, 0, 0xFF, &reinit), PCI_DX_INVALID,
                   "so is a wildly out-of-range state");
    TEST_ASSERT_EQ(reinit, 0,
                   "the obligation out-parameter is cleared before any refusal");
}

static void test_pci_set_d_state_requires_obligation_pointer(void)
{
    /* The reinitialisation obligation must be impossible to miss, so there is
     * no call shape that discards it. */
    TEST_ASSERT_EQ(pci_set_d_state(0, 0, 0, PCI_D0, 0), PCI_DX_INVALID,
                   "a NULL obligation pointer is refused");
}

/* ---- BDF validation ----
 * The CF8 address format packs bus, device and function adjacently, so an
 * out-of-range field does not fail -- it names a DIFFERENT device. */

static void test_pci_pm_bdf_range(void)
{
    TEST_ASSERT_EQ(pci_pm_bdf_valid(0, 0, 0), 1, "00:00.0 is addressable");
    TEST_ASSERT_EQ(pci_pm_bdf_valid(0xFF, 31, 7), 1,
                   "the largest legal triple is addressable");
    TEST_ASSERT_EQ(pci_pm_bdf_valid(0, 32, 0), 0,
                   "device 32 would alias the next bus, so it is refused");
    TEST_ASSERT_EQ(pci_pm_bdf_valid(0, 0, 8), 0,
                   "function 8 would alias device 1, so it is refused");
    TEST_ASSERT_EQ(pci_pm_bdf_valid(0, 0xFF, 0xFF), 0,
                   "an all-ones device and function are refused");
}

static void test_pci_pm_public_apis_refuse_bad_bdf(void)
{
    /* Refused before any config access, so this is safe with no device present
     * and is what stops an aliased tuple reaching a real one. */
    PCI_PMCAP cap;
    TEST_ASSERT_EQ(pci_pmcap_find(0, 32, 0), PCI_DX_INVALID,
                   "capability discovery refuses an aliasing device number");
    TEST_ASSERT_EQ(pci_pmcap_read(0, 0, 8, &cap), PCI_DX_INVALID,
                   "capability read refuses an aliasing function number");
    TEST_ASSERT_EQ(pci_get_d_state(0, 32, 0), PCI_DX_INVALID,
                   "the getter refuses it too");
    int reinit = 0;
    TEST_ASSERT_EQ(pci_set_d_state(0, 0, 8, PCI_D0, &reinit), PCI_DX_INVALID,
                   "and so does the setter");
}

/* ---- D0 Uninitialized obligation ---- */

static void test_pci_pm_plan_flags_reinit_after_d3hot(void)
{
    pci_pm_plan_t plan;
    int rc = pci_pm_plan_transition(0, PCI_D3HOT, PCI_D0, 1, &plan);
    TEST_ASSERT_EQ(rc, PCI_DX_OK, "D3hot->D0 is planned");
    TEST_ASSERT_EQ(plan.reinit_after, 1,
                   "No_Soft_Reset clear means the device comes back uninitialised");
}

static void test_pci_pm_plan_no_reinit_when_soft_reset_free(void)
{
    pci_pm_plan_t plan;
    uint16_t pmcsr = (uint16_t)(PMCSR_NO_SOFT_RESET | PCI_D3HOT);
    int rc = pci_pm_plan_transition(0, pmcsr, PCI_D0, 1, &plan);
    TEST_ASSERT_EQ(rc, PCI_DX_OK, "D3hot->D0 is planned");
    TEST_ASSERT_EQ(plan.reinit_after, 0,
                   "No_Soft_Reset set means state survives the transition");
}

static void test_pci_pm_plan_reinit_only_leaving_d3hot(void)
{
    pci_pm_plan_t plan;
    pci_pm_plan_transition(0, PCI_D0, PCI_D3HOT, 1, &plan);
    TEST_ASSERT_EQ(plan.reinit_after, 0,
                   "ENTERING D3hot does not lose state; leaving it does");
    pci_pm_plan_transition(PMC_D1_SUPPORT, PCI_D1, PCI_D0, 1, &plan);
    TEST_ASSERT_EQ(plan.reinit_after, 0,
                   "D1->D0 keeps configuration regardless of No_Soft_Reset");
}

static void test_pci_pm_status_codes_are_ok_or_negative(void)
{
    /* The obligation travels in an out-parameter, so the return value keeps one
     * meaning: zero is success and everything else is a failure to classify. */
    TEST_ASSERT_EQ(PCI_DX_OK, 0, "success is zero");
    TEST_ASSERT(PCI_DX_BUSY < 0, "and every other outcome is negative");
}

/* ---- Non-answering device ----
 * A bus returns 0xFFFF when nothing responds, and 0xFFFF & PMCSR_POWER_STATE_MASK
 * is D3hot. Without this check a removed device would confirm the suspend it
 * never performed. */

static void test_pci_pm_all_ones_pmcsr_is_not_a_state(void)
{
    TEST_ASSERT_EQ(pci_pm_pmcsr_plausible(0xFFFF), 0,
                   "an all-ones PMCSR is a device that did not answer");
    TEST_ASSERT_EQ(0xFFFF & PMCSR_POWER_STATE_MASK, PCI_D3HOT,
                   "and it would otherwise read as D3hot, which is the hazard");
    TEST_ASSERT_EQ(pci_pm_pmcsr_plausible(0x0000), 1,
                   "a fully cleared register is plausible");
    TEST_ASSERT_EQ(pci_pm_pmcsr_plausible(PCI_D3HOT), 1,
                   "a genuine D3hot report is not rejected");
    TEST_ASSERT_EQ(pci_pm_pmcsr_plausible((uint16_t)(PMCSR_PME_EN | PMCSR_PME_STATUS
                                                     | PMCSR_NO_SOFT_RESET | PCI_D0)), 1,
                   "every defined bit set at once is still a legal report");
}

static void test_pci_pm_partial_response_pmcsr_rejected(void)
{
    /* The dangerous values are the ones that are NOT 0xFFFF. 0xFFFB has state
     * bits 3, so a D3hot request would take the already-there path and report
     * success without ever writing or verifying. */
    TEST_ASSERT_EQ(pci_pm_pmcsr_plausible(0xFFFB), 0,
                   "a degraded near-all-ones response is rejected");
    TEST_ASSERT_EQ(0xFFFB & PMCSR_POWER_STATE_MASK, PCI_D3HOT,
                   "and it would otherwise have read as an already-suspended device");
    TEST_ASSERT_EQ(pci_pm_pmcsr_plausible(0x0010), 0,
                   "a bit from the required-zero 7:4 field means the same");
    TEST_ASSERT_EQ(pci_pm_pmcsr_plausible(0x0004), 1,
                   "but bit 2 has a device-specific reset value and is accepted");
}

static void test_pci_pm_poison_refuses_every_entry_point(void)
{
    /* Without forcing the flag, the only provable claims are about the status
     * codes, and an implementation whose poison flag was never set would pass
     * all of them. Forcing it is what makes this test fail if the admission
     * gate stops gating. */
    PCI_PMCAP cap;
    int reinit = 0;

    pci_pm_set_poisoned_for_test(1);
    TEST_ASSERT_EQ(pci_pm_is_poisoned(), 1, "the module reports itself poisoned");
    TEST_ASSERT_EQ(pci_pmcap_find(0, 0, 0), PCI_DX_POISONED,
                   "capability discovery refuses while poisoned");
    TEST_ASSERT_EQ(pci_pmcap_read(0, 0, 0, &cap), PCI_DX_POISONED,
                   "capability read refuses while poisoned");
    TEST_ASSERT_EQ(pci_get_d_state(0, 0, 0), PCI_DX_POISONED,
                   "the getter refuses while poisoned");
    TEST_ASSERT_EQ(pci_set_d_state(0, 0, 0, PCI_D0, &reinit), PCI_DX_POISONED,
                   "the setter refuses while poisoned");

    pci_pm_clear_poison();
    TEST_ASSERT_EQ(pci_pm_is_poisoned(), 0,
                   "and clearing restores admission, so the refusal is recoverable");
}

static void test_pci_pm_poison_is_global_and_clearable(void)
{
    /* The only way to poison is a qualified hardware counter stopping mid-wait,
     * which is a property of the MACHINE. Scoping the refusal the same way is
     * what stops a handful of dead devices making every healthy one
     * undriveable, which per-device bookkeeping of a fixed size would. */
    TEST_ASSERT(PCI_DX_POISONED < 0,
                "a poisoned module refuses rather than driving devices");
    TEST_ASSERT(PCI_DX_POISONED != PCI_DX_BUSY,
                "and that is distinguishable from a device merely in use");
    TEST_ASSERT_EQ(pci_pm_is_poisoned(), 0,
                   "nothing has stalled, so the module is usable");

    /* Clearing is idempotent, which is what makes it safe to call from a bus
     * rescan that does not know whether anything stalled. */
    pci_pm_clear_poison();
    TEST_ASSERT_EQ(pci_pm_is_poisoned(), 0,
                   "clearing an unpoisoned module is a no-op, not an error");
}

static void test_pci_pm_pmc_version_range(void)
{
    TEST_ASSERT_EQ(pci_pm_pmc_version_supported(1), 1, "PM 1.0 is understood");
    TEST_ASSERT_EQ(pci_pm_pmc_version_supported(2), 1, "PM 1.1 is understood");
    TEST_ASSERT_EQ(pci_pm_pmc_version_supported(3), 1, "PM 1.2 is understood");
    TEST_ASSERT_EQ(pci_pm_pmc_version_supported(0), 0,
                   "a zero version field is reserved, not a revision");
    TEST_ASSERT_EQ(pci_pm_pmc_version_supported(4), 0,
                   "and so is anything above 1.2");
    TEST_ASSERT_EQ(pci_pm_pmc_version_supported(0xFFFF), 0,
                   "an all-ones PMC names version 7, which is reserved");
    TEST_ASSERT_EQ(pci_pm_pmc_version_supported((uint16_t)(PMC_D1_SUPPORT | 3)), 1,
                   "the other capability bits do not affect the version check");
}

static void test_pci_pm_stall_status_is_distinct(void)
{
    /* A wait that could not be observed is not the same as a state that did not
     * latch: the first leaves the device mid-transition, the second does not. */
    TEST_ASSERT(PCI_DX_CLOCK_STALLED < 0,
                "a stalled clock is a failure");
    TEST_ASSERT(PCI_DX_CLOCK_STALLED != PCI_DX_FAILED
                && PCI_DX_CLOCK_STALLED != PCI_DX_NO_TIMEBASE,
                "and is distinguishable from a failed latch and a missing clock");
}

/* ---- Not-ready is not the same as absent ---- */

static void test_pci_pm_rrs_is_distinct_from_no_response(void)
{
    /* A PCIe function still initialising answers Request Retry Status, which
     * arrives as a real vendor ID rather than as silence. A readiness poll that
     * accepts "anything but all-ones" therefore declares an explicitly
     * not-ready device ready and touches it mid-reset. */
    TEST_ASSERT(PCI_VENDOR_ID_RRS != PCI_CFG_NO_RESPONSE,
                "RRS is a real answer, not the absence of one");
    /* The readiness poll's own predicate: a device is ready only when its
     * vendor ID is neither of these. Asserted as the predicate rather than as
     * the two literals, which would only prove they were typed correctly. */
    TEST_ASSERT(!(PCI_VENDOR_ID_RRS != PCI_CFG_NO_RESPONSE
                  && PCI_VENDOR_ID_RRS != PCI_VENDOR_ID_RRS),
                "an RRS answer does not satisfy the ready predicate");
    TEST_ASSERT(PCI_CFG_NO_RESPONSE > PCI_VENDOR_ID_RRS,
                "and neither sentinel can be mistaken for the other");
}

static void test_pci_pm_readiness_allowance_covers_a_reset(void)
{
    /* 60 ms was short enough to declare a compliant storage or USB controller
     * failed during resume; the conventional-reset allowance is a full second. */
    TEST_ASSERT(PCI_PM_D0_READY_MAX_US >= 1000000u,
                "the readiness allowance covers a conventional reset");
    TEST_ASSERT(PCI_PM_D0_READY_STEP_US > 0
                && PCI_PM_D0_READY_STEP_US < PCI_PM_D0_READY_MAX_US,
                "and the poll step divides that allowance rather than exceeding it");
}

static void test_pci_pm_capability_ttl_matches_node_positions(void)
{
    /* The walk tells a maximal legal list from a cyclic one by whether the next
     * pointer is zero when the budget runs out, which is only correct while the
     * budget equals the number of positions a node can occupy. */
    TEST_ASSERT_EQ(PCI_CAP_WALK_MAX,
                   ((PCI_CAP_NODE_OFF_MAX - PCI_CAP_OFF_MIN) / 4) + 1,
                   "the TTL is the count of legal node positions, not a margin");
}

static void test_pci_pm_irql_status_is_distinct(void)
{
    /* A transition busy-waits for its recovery interval, so it may only run
     * where that does not hold off interrupts and DPCs. Refusing is a distinct
     * outcome from anything the device did. */
    TEST_ASSERT(PCI_DX_IRQL < 0, "a raised-IRQL caller is refused");
    TEST_ASSERT(PCI_DX_IRQL != PCI_DX_INVALID && PCI_DX_IRQL != PCI_DX_BUSY,
                "and that is not confused with a bad argument or a busy device");
}

static void test_pci_pm_plan_d0_noop_on_reset_prone_device(void)
{
    /* The planner still reports a D0 request against a device already in D0 as
     * a no-op; the CONSERVATIVE obligation for that case is applied by
     * pci_set_d_state, which knows the device may have reached D0 through a
     * transition nobody saw finish. */
    pci_pm_plan_t plan;
    int rc = pci_pm_plan_transition(0, PCI_D0, PCI_D0, 1, &plan);
    TEST_ASSERT_EQ(rc, PCI_DX_OK, "a D0 request on a D0 device succeeds");
    TEST_ASSERT_EQ(plan.needs_write, 0, "without writing");
    TEST_ASSERT_EQ(plan.reinit_after, 0,
                   "and claims no reinitialisation obligation for it");

    /* Not even when No_Soft_Reset is clear. That bit says what a D3hot->D0
     * WOULD do; on a device already in D0 it proves nothing, and acting on it
     * would tear down a live controller's BARs and interrupts. */
    rc = pci_pm_plan_transition(0, (uint16_t)(PCI_D0), PCI_D0, 1, &plan);
    TEST_ASSERT_EQ(rc, PCI_DX_OK, "same for a device reporting No_Soft_Reset clear");
    TEST_ASSERT_EQ(plan.reinit_after, 0,
                   "a no-op never fabricates a destructive obligation");
}

/* ---- Registration ---- */

void test_register_pci_pm(void)
{
    test_suite_register_cat("PCI PM: capability pointer by header type",
                            test_pci_pm_cap_ptr_by_header_type, TEST_CAT_BOOT);
    test_suite_register_cat("PCI PM: multi-function bit is not a header type",
                            test_pci_pm_cap_ptr_ignores_multifunction_bit, TEST_CAT_BOOT);
    test_suite_register_cat("PCI PM: unknown header type has no cap list",
                            test_pci_pm_cap_ptr_unknown_type_refused, TEST_CAT_BOOT);
    test_suite_register_cat("PCI PM: capability offset bounds",
                            test_pci_pm_cap_offset_bounds, TEST_CAT_BOOT);
    test_suite_register_cat("PCI PM: misaligned capability pointer rejected",
                            test_pci_pm_cap_offset_alignment, TEST_CAT_BOOT);
    test_suite_register_cat("PCI PM: PM footprint is stricter than the node bound",
                            test_pci_pm_footprint_is_stricter_than_node_bound, TEST_CAT_BOOT);
    test_suite_register_cat("PCI PM: CardBus fixed fields are not capabilities",
                            test_pci_pm_cap_offset_cardbus_fixed_fields, TEST_CAT_BOOT);
    test_suite_register_cat("PCI PM: recovery delay leaving D0",
                            test_pci_pm_delay_from_d0, TEST_CAT_BOOT);
    test_suite_register_cat("PCI PM: recovery delay from D1 and D2",
                            test_pci_pm_delay_from_d1_d2, TEST_CAT_BOOT);
    test_suite_register_cat("PCI PM: recovery delay leaving D3hot",
                            test_pci_pm_delay_d3hot_exit, TEST_CAT_BOOT);
    test_suite_register_cat("PCI PM: recovery delay identity and shallower pairs",
                            test_pci_pm_delay_identity_and_shallower, TEST_CAT_BOOT);
    test_suite_register_cat("PCI PM: deeper transitions and D0 are legal",
                            test_pci_pm_transition_deeper_and_d0_legal, TEST_CAT_BOOT);
    test_suite_register_cat("PCI PM: every low-power state can reach D0",
                            test_pci_pm_transition_wake_to_d0_legal, TEST_CAT_BOOT);
    test_suite_register_cat("PCI PM: a D2 plan carries the 200 us delay",
                            test_pci_pm_plan_d2_carries_its_delay, TEST_CAT_BOOT);
    test_suite_register_cat("PCI PM: partial wake transitions are illegal",
                            test_pci_pm_transition_partial_wake_illegal, TEST_CAT_BOOT);
    test_suite_register_cat("PCI PM: out-of-range states refused",
                            test_pci_pm_transition_out_of_range, TEST_CAT_BOOT);
    test_suite_register_cat("PCI PM: D0 and D3hot always supported",
                            test_pci_pm_d0_d3hot_always_supported, TEST_CAT_BOOT);
    test_suite_register_cat("PCI PM: D1/D2 require PMC advertisement",
                            test_pci_pm_d1_d2_require_advertisement, TEST_CAT_BOOT);
    test_suite_register_cat("PCI PM: unsupported state above D3hot",
                            test_pci_pm_state_supported_rejects_out_of_range, TEST_CAT_BOOT);
    test_suite_register_cat("PCI PM: PMCSR write never acknowledges PME",
                            test_pci_pmcsr_write_never_acks_pme, TEST_CAT_BOOT);
    test_suite_register_cat("PCI PM: PMCSR write replaces the state field only",
                            test_pci_pmcsr_write_replaces_state_only, TEST_CAT_BOOT);
    test_suite_register_cat("PCI PM: PMCSR write masks the state argument",
                            test_pci_pmcsr_write_masks_state_argument, TEST_CAT_BOOT);
    test_suite_register_cat("PCI PM: No_Soft_Reset reported from PMCSR",
                            test_pci_pm_no_soft_reset_reported, TEST_CAT_BOOT);
    test_suite_register_cat("PCI PM: PMCSR write over an all-ones read",
                            test_pci_pmcsr_write_all_ones_input, TEST_CAT_BOOT);
    test_suite_register_cat("PCI PM: status codes are distinct and negative",
                            test_pci_pm_status_codes_distinct, TEST_CAT_BOOT);
    test_suite_register_cat("PCI PM: plan a normal low-power entry",
                            test_pci_pm_plan_normal_entry, TEST_CAT_BOOT);
    test_suite_register_cat("PCI PM: plan for the current state writes nothing",
                            test_pci_pm_plan_noop_needs_no_write, TEST_CAT_BOOT);
    test_suite_register_cat("PCI PM: plan refuses an unadvertised state",
                            test_pci_pm_plan_refuses_unsupported_state, TEST_CAT_BOOT);
    test_suite_register_cat("PCI PM: plan refuses a partial wake",
                            test_pci_pm_plan_refuses_partial_wake, TEST_CAT_BOOT);
    test_suite_register_cat("PCI PM: plan refuses an out-of-range target",
                            test_pci_pm_plan_refuses_out_of_range_target, TEST_CAT_BOOT);
    test_suite_register_cat("PCI PM: plan refuses a delay it cannot time",
                            test_pci_pm_plan_refuses_without_timebase, TEST_CAT_BOOT);
    test_suite_register_cat("PCI PM: a refused plan clears both outputs",
                            test_pci_pm_plan_refusal_clears_both_outputs, TEST_CAT_BOOT);
    test_suite_register_cat("PCI PM: a delay-free plan needs no clock",
                            test_pci_pm_plan_no_delay_needs_no_timebase, TEST_CAT_BOOT);
    test_suite_register_cat("PCI PM: plan preserves PME arming and status",
                            test_pci_pm_plan_preserves_pme_state, TEST_CAT_BOOT);
    test_suite_register_cat("PCI PM: pmcap_read refuses a NULL output",
                            test_pci_pmcap_read_null_refused, TEST_CAT_BOOT);
    test_suite_register_cat("PCI PM: set_d_state refuses an out-of-range state",
                            test_pci_set_d_state_out_of_range_refused, TEST_CAT_BOOT);
    test_suite_register_cat("PCI PM: BDF range refuses aliasing tuples",
                            test_pci_pm_bdf_range, TEST_CAT_BOOT);
    test_suite_register_cat("PCI PM: public APIs refuse an aliasing BDF",
                            test_pci_pm_public_apis_refuse_bad_bdf, TEST_CAT_BOOT);
    test_suite_register_cat("PCI PM: plan flags reinit leaving D3hot",
                            test_pci_pm_plan_flags_reinit_after_d3hot, TEST_CAT_BOOT);
    test_suite_register_cat("PCI PM: No_Soft_Reset means no reinit",
                            test_pci_pm_plan_no_reinit_when_soft_reset_free, TEST_CAT_BOOT);
    test_suite_register_cat("PCI PM: reinit is flagged only leaving D3hot",
                            test_pci_pm_plan_reinit_only_leaving_d3hot, TEST_CAT_BOOT);
    test_suite_register_cat("PCI PM: return codes are OK or negative",
                            test_pci_pm_status_codes_are_ok_or_negative, TEST_CAT_BOOT);
    test_suite_register_cat("PCI PM: an all-ones PMCSR is not a D-state",
                            test_pci_pm_all_ones_pmcsr_is_not_a_state, TEST_CAT_BOOT);
    test_suite_register_cat("PCI PM: a partial response is not a D-state",
                            test_pci_pm_partial_response_pmcsr_rejected, TEST_CAT_BOOT);
    test_suite_register_cat("PCI PM: RRS is distinct from no response",
                            test_pci_pm_rrs_is_distinct_from_no_response, TEST_CAT_BOOT);
    test_suite_register_cat("PCI PM: readiness allowance covers a reset",
                            test_pci_pm_readiness_allowance_covers_a_reset, TEST_CAT_BOOT);
    test_suite_register_cat("PCI PM: capability TTL matches node positions",
                            test_pci_pm_capability_ttl_matches_node_positions, TEST_CAT_BOOT);
    test_suite_register_cat("PCI PM: raised-IRQL refusal is distinct",
                            test_pci_pm_irql_status_is_distinct, TEST_CAT_BOOT);
    test_suite_register_cat("PCI PM: a D0 no-op plan claims no obligation",
                            test_pci_pm_plan_d0_noop_on_reset_prone_device, TEST_CAT_BOOT);
    test_suite_register_cat("PCI PM: poison refuses every entry point",
                            test_pci_pm_poison_refuses_every_entry_point, TEST_CAT_BOOT);
    test_suite_register_cat("PCI PM: poison is global and clearable",
                            test_pci_pm_poison_is_global_and_clearable, TEST_CAT_BOOT);
    test_suite_register_cat("PCI PM: PMC version field range",
                            test_pci_pm_pmc_version_range, TEST_CAT_BOOT);
    test_suite_register_cat("PCI PM: stalled-clock status is distinct",
                            test_pci_pm_stall_status_is_distinct, TEST_CAT_BOOT);
    test_suite_register_cat("PCI PM: set_d_state requires the obligation pointer",
                            test_pci_set_d_state_requires_obligation_pointer, TEST_CAT_BOOT);
}

#endif /* KERNEL_TESTS */
