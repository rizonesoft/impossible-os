/* ============================================================================
 * wcag.c -- WCAG 2.2 sweep driver skeleton (the desktop UI test framework WCAG section).
 *
 * Consumer of the accessibility automation tree. The tree provider is
 * still `[ ]`; until it ships, `wcag_sweep_run()` has no nodes to
 * walk and returns 0 findings. This file exists so the test harness
 * can wire `make test-wcag` end-to-end today -- the moment the tree
 * lands, the walk loop inside this function lights up without
 * changing a single call site.
 * ============================================================================ */

#include "kernel/test/wcag.h"
#include "kernel/types.h"

static const char *const s_rule_names[WCAG_RULE_COUNT] = {
    [WCAG_RULE_NONE]              = "none",
    [WCAG_RULE_1_4_3_CONTRAST]    = "1.4.3 contrast",
    [WCAG_RULE_1_4_11_NONTEXT]    = "1.4.11 non-text contrast",
    [WCAG_RULE_2_1_1_KEYBOARD]    = "2.1.1 keyboard",
    [WCAG_RULE_2_4_3_FOCUS_ORDER] = "2.4.3 focus order",
    [WCAG_RULE_4_1_2_NAME_ROLE]   = "4.1.2 name/role/value",
};

const char *wcag_rule_name(wcag_rule_id_t id)
{
    if ((int)id < 0 || (int)id >= WCAG_RULE_COUNT)
        return "unknown";
    return s_rule_names[id] ? s_rule_names[id] : "unknown";
}

int wcag_provider_ready(void)
{
    /* Flips to 1 once the accessibility automation tree provider exposes `automation_root()` and
     * friends. Today it is hard-coded 0 -- when that section ships,
     * update both this predicate AND wcag_sweep_run() in lock-step.
     * Codex [M] section 14 review required a distinct signal so test code
     * can tell "provider wired" from "provider wired + zero findings". */
    return 0;
}

int wcag_sweep_run(wcag_finding_t *out_findings, int max_findings)
{
    /* Argument guard -- the sweep path is always safe to call with
     * a NULL buffer so callers can "dry-run" the finding count. */
    (void)out_findings;
    (void)max_findings;

    if (!wcag_provider_ready())
        return WCAG_SWEEP_PROVIDER_MISSING;

    /* the accessibility automation tree provider provides:
     *   automation_root() -- iterate from tree root
     *   automation_node_t fields: role, name, children, fg_color, bg_color,
     *                             tab_index, is_actionable, etc.
     *
     * Replace this block with the real rule loop when the provider is
     * wired:
     *
     *   automation_walk(check_contrast, &ctx);
     *   automation_walk(check_non_text_contrast, &ctx);
     *   automation_walk(check_keyboard_reachable, &ctx);
     *   check_focus_order(automation_root());
     *   automation_walk(check_name_role_value, &ctx);
     *
     * Each `check_*` helper lives below this function (all stubs today;
     * see wcag_provider_ready() above for the single flip point). */
    return 0;
}
