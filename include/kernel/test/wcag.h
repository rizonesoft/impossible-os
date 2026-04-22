/* ============================================================================
 * wcag.h -- WCAG 2.2 sweep rule IDs + finding schema (the desktop UI test framework WCAG section).
 *
 * section 14 is the test-framework consumer of the deterministic automation
 * tree provided by the accessibility automation tree provider. The provider is NOT shipped yet; this
 * header ships the rule-ID enum + finding struct so when the tree
 * lands, the sweep driver can flip on without touching this schema.
 *
 * Reference: WCAG 2.2 (https://www.w3.org/TR/WCAG22/) -- rule numbers
 * below use the WCAG success-criterion IDs verbatim.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* ---- Rule IDs --------------------------------------------------------- */

/* Subset of WCAG 2.2 rules applicable to a compositor shell.
 * Numeric values are stable once shipped so test output logs can be
 * diff'd across runs. Extending the enum: append only. */
typedef enum {
    WCAG_RULE_NONE               = 0,
    WCAG_RULE_1_4_3_CONTRAST     = 1,   /* text FG/BG >= 4.5:1 (3:1 large) */
    WCAG_RULE_1_4_11_NONTEXT     = 2,   /* control boundary >= 3:1        */
    WCAG_RULE_2_1_1_KEYBOARD     = 3,   /* actionable node tab-reachable  */
    WCAG_RULE_2_4_3_FOCUS_ORDER  = 4,   /* tab traversal logical order    */
    WCAG_RULE_4_1_2_NAME_ROLE    = 5,   /* name + role set; value on inputs */
    WCAG_RULE_COUNT              = 6,
} wcag_rule_id_t;

/* Finding severity. CI gate fails on `error`; `warn` is report-only. */
typedef enum {
    WCAG_SEV_INFO = 0,
    WCAG_SEV_WARN = 1,
    WCAG_SEV_ERROR = 2,
} wcag_severity_t;

/* Single structured finding. `node_id` comes from the accessibility automation tree provider
 * automation tree provider's stable-ID scheme; 0 means "no specific
 * node" (tree-wide finding). `message` is an ASCII diagnostic the
 * CI runner echoes on serial + the JSON artifact. */
typedef struct {
    wcag_rule_id_t  rule_id;
    uint32_t        node_id;
    wcag_severity_t severity;
    const char     *message;
} wcag_finding_t;

/* ---- Sweep driver (provider-backed; stub returns 0 findings today) ---- */

/* Walk every automation node reachable from the root and apply the
 * configured WCAG rule set. Each rule that fires appends a finding
 * to `out_findings[]` up to `max_findings`.
 *
 * Returns: >= 0 = finding count when the provider is ready (may be 0
 * for a clean desktop); WCAG_SWEEP_PROVIDER_MISSING (negative) when
 * the accessibility automation tree provider is still `[ ]`. Callers distinguish "provider wired,
 * zero rule violations" from "provider absent" via the negative
 * sentinel -- Codex [M] section 14 review required this split so
 * `make test-wcag` does not stay permanently indistinguishable
 * between stub and green-sweep states.
 *
 * Callable from TEST_CAT_DESKTOP test cases; not part of the stable
 * kernel API and may change until the accessibility automation tree provider lands. */
#define WCAG_SWEEP_PROVIDER_MISSING (-1)
int wcag_sweep_run(wcag_finding_t *out_findings, int max_findings);

/* Report whether the automation tree provider is wired. Returns 1 when
 * wcag_sweep_run() will run the real rule loop, 0 when it returns
 * WCAG_SWEEP_PROVIDER_MISSING. Splitting this from the sweep lets
 * tests gate on "is the gate active" independently of finding count
 * (a clean desktop produces 0 findings, same as a stub). */
int wcag_provider_ready(void);

/* Return a short ASCII description of the rule for log formatting. */
const char *wcag_rule_name(wcag_rule_id_t id);
