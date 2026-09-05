/*
 * src/boot/uefi/boot_policy.c -- Boot policy merge order
 *
 * Pure-C ladder + BLS-style counter filename parser/formatter +
 * conservative duplicate dedupe helper. UEFI-side glue (Boot####
 * GetVariable, ESP directory scan, write-new + Flush() + Close(success) +
 * delete-old crash-tolerant decrement) lives in bootx64.c; this file
 * is freestanding and cross-includable from the kernel test runner
 * the same way boot_entries_parser.c is.
 */

#ifndef BOOT_POLICY_NO_HEADER
#include "../../../include/boot/boot_policy.h"
#endif

/* Local typedefs match boot_entries_parser.c so behavior is consistent
 * across the bootloader pure-C surface. */
typedef unsigned char       u8;
typedef unsigned short      u16;
typedef unsigned int        u32;

/* ---- Tiny string helpers (heap-free, freestanding) ----------------------- */

static u32 sp_strlen(const char *s)
{
    u32 n = 0;
    while (s && s[n]) n++;
    return n;
}

static int sp_str_eq(const char *a, const char *b)
{
    if (!a || !b) return 0;
    u32 i = 0;
    while (a[i] && b[i]) {
        if (a[i] != b[i]) return 0;
        i++;
    }
    return a[i] == b[i];
}

static void sp_zero(void *p, u32 n)
{
    u8 *b = (u8 *)p;
    for (u32 i = 0; i < n; i++) b[i] = 0;
}

static void sp_copy_clamped(char *dst, u32 dst_cap, const char *src, u32 src_len)
{
    if (dst_cap == 0) return;
    u32 n = src_len < dst_cap - 1u ? src_len : dst_cap - 1u;
    for (u32 i = 0; i < n; i++) dst[i] = src[i];
    dst[n] = '\0';
}

/* ASCII case-insensitive equality on UUID textual form. */
static int sp_uuid_eq_ci(const char *a, const char *b)
{
    u32 i;
    for (i = 0; i < 36u; i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + ('a' - 'A'));
        if (ca != cb) return 0;
    }
    return a[36] == '\0' && b[36] == '\0';
}

static int sp_is_ascii_digit(char c) { return c >= '0' && c <= '9'; }

static int sp_is_kebab_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
}

/* ---- Counter filename parser/formatter ---------------------------------- */

int boot_counter_parse_filename(const char *name, unsigned int name_len,
                                boot_counter_t *out)
{
    if (!name || !out || name_len == 0u) return 0;
    sp_zero(out, sizeof(*out));

    /* Locate the last '+' (separator before tries_left); the id may not
     * contain '+' per kebab grammar so any '+' is the separator. */
    u32 plus = 0xFFFFFFFFu;
    for (u32 i = 0; i < name_len; i++) {
        if (name[i] == '+') { plus = i; break; }
    }
    if (plus == 0xFFFFFFFFu || plus == 0u) return 0;
    if (plus > BOOT_ENTRIES_MAX_ID_LEN) return 0;

    /* Validate id grammar: kebab-case, no leading/trailing/consecutive '-'. */
    if (name[0] == '-' || name[plus - 1u] == '-') return 0;
    for (u32 i = 0; i < plus; i++) {
        char c = name[i];
        if (!sp_is_kebab_char(c)) return 0;
        if (c == '-' && i + 1u < plus && name[i + 1u] == '-') return 0;
    }

    /* After '+': L digits, '-', D digits. L is 1 digit, D is 1..2 digits. */
    u32 li = plus + 1u;
    if (li >= name_len || !sp_is_ascii_digit(name[li])) return 0;
    u32 left = (u32)(name[li] - '0');
    if (left > BOOT_COUNTER_TRIES_LEFT_MAX) return 0;
    li++;
    if (li >= name_len || name[li] != '-') return 0;
    li++;
    if (li >= name_len || !sp_is_ascii_digit(name[li])) return 0;
    u32 done = (u32)(name[li] - '0');
    li++;
    if (li < name_len && sp_is_ascii_digit(name[li])) {
        done = done * 10u + (u32)(name[li] - '0');
        li++;
    }
    if (li != name_len) return 0;          /* trailing junk */
    if (done > BOOT_COUNTER_TRIES_DONE_MAX) return 0;

    sp_copy_clamped(out->id, sizeof(out->id), name, plus);
    out->tries_left = left;
    out->tries_done = done;
    return 1;
}

unsigned int boot_counter_format_filename(const boot_counter_t *c,
                                          char *out, unsigned int out_cap)
{
    if (!c || !out || out_cap < 8u) return 0;
    if (c->tries_left > BOOT_COUNTER_TRIES_LEFT_MAX) return 0;
    if (c->tries_done > BOOT_COUNTER_TRIES_DONE_MAX) return 0;

    u32 idl = sp_strlen(c->id);
    if (idl == 0u || idl > BOOT_ENTRIES_MAX_ID_LEN) return 0;

    /* Validate id grammar before writing -- caller may have a stale id. */
    if (c->id[0] == '-' || c->id[idl - 1u] == '-') return 0;
    for (u32 i = 0; i < idl; i++) {
        if (!sp_is_kebab_char(c->id[i])) return 0;
        if (c->id[i] == '-' && i + 1u < idl && c->id[i + 1u] == '-') return 0;
    }

    /* idl + '+' + 1 digit + '-' + (1..2 digits) + NUL */
    u32 need = idl + 1u + 1u + 1u + (c->tries_done >= 10u ? 2u : 1u) + 1u;
    if (need > out_cap) return 0;

    u32 n = 0;
    for (u32 i = 0; i < idl; i++) out[n++] = c->id[i];
    out[n++] = '+';
    out[n++] = (char)('0' + c->tries_left);
    out[n++] = '-';
    if (c->tries_done >= 10u) out[n++] = (char)('0' + c->tries_done / 10u);
    out[n++] = (char)('0' + c->tries_done % 10u);
    out[n] = '\0';
    return n;
}

/* ---- Ladder helpers ----------------------------------------------------- */

static int counter_lookup(const boot_counter_t *counters, unsigned int n,
                          const char *id, boot_counter_t *out)
{
    for (unsigned int i = 0; i < n; i++) {
        if (sp_str_eq(counters[i].id, id)) {
            if (out) *out = counters[i];
            return 1;
        }
    }
    return 0;
}

static int machine_id_matches_local(const boot_entry_envelope_t *e,
                                    const char *local)
{
    /* Empty machine_id on the entry means "match any machine". Empty local
     * UUID means "machine has no SMBIOS UUID" -- in that case, only entries
     * with empty machine_id can match (avoid mis-routing builds). */
    if (e->machine_id[0] == '\0') return 1;
    if (local[0] == '\0') return 0;
    if (sp_strlen(e->machine_id) != 36u || sp_strlen(local) != 36u) return 0;
    return sp_uuid_eq_ci(e->machine_id, local);
}

static void record_reject(boot_policy_decision_t *out, const char *id,
                          unsigned int reason)
{
    if (out->rejected_count >= BOOT_ENTRIES_MAX_ENTRIES) {
        out->rejected_overflow = 1;
        return;
    }
    sp_copy_clamped(out->rejected[out->rejected_count].id,
                    sizeof(out->rejected[out->rejected_count].id),
                    id, sp_strlen(id));
    out->rejected[out->rejected_count].reason = reason;
    out->rejected_count++;
}

/* Returns 1 if the entry passes the policy filter (active, not hidden, not
 * kind-skipped, machine matches, tries_left > 0, no path-escape under
 * Secure Boot). On a filter miss, records the reject reason in `out`. */
static int entry_passes_filter(const boot_entry_envelope_t *e,
                               const boot_policy_inputs_t *inputs,
                               const boot_counter_t *counters,
                               unsigned int counter_count,
                               boot_policy_decision_t *out)
{
    if (e->kind_skipped) {
        record_reject(out, e->id, BOOT_REJECT_REASON_KIND_SKIPPED);
        return 0;
    }
    if (!(e->flags & BOOT_ENTRY_FLAG_ACTIVE)) {
        record_reject(out, e->id, BOOT_REJECT_REASON_NOT_ACTIVE);
        return 0;
    }
    if (e->flags & BOOT_ENTRY_FLAG_HIDDEN) {
        record_reject(out, e->id, BOOT_REJECT_REASON_HIDDEN);
        return 0;
    }
    /* Caller-side capability gate: kinds the bootloader cannot actually
     * execute (network/resume/chainload until per-kind handlers ship)
     * are filtered here so the ladder never selects an entry whose
     * handoff the caller cannot finish. mask=0 disables the gate.
     *
     * Closed-mask for kind >= 32: the mask is 32 bits so any kind that
     * cannot be represented (vendor 100..199, reserved-future >=200)
     * fails closed under any non-zero mask. Today the parser already
     * sets kind_skipped on those ranges and the earlier branch filters
     * them out, but defense-in-depth: if a future parser change stops
     * setting kind_skipped for some range, the mask gate STILL refuses
     * to promote a kind whose loader the caller cannot vouch for. */
    if (inputs->supported_kinds_mask != 0u &&
        (e->kind >= 32u ||
         (inputs->supported_kinds_mask & (1u << e->kind)) == 0u)) {
        record_reject(out, e->id, BOOT_REJECT_REASON_KIND_UNAVAILABLE);
        return 0;
    }
    if (!machine_id_matches_local(e, inputs->local_machine_id)) {
        record_reject(out, e->id, BOOT_REJECT_REASON_MACHINE_ID_MISMATCH);
        return 0;
    }
    boot_counter_t c;
    if (counter_lookup(counters, counter_count, e->id, &c) && c.tries_left == 0u) {
        record_reject(out, e->id, BOOT_REJECT_REASON_TRIES_EXHAUSTED);
        return 0;
    }
    if (inputs->secure_boot_active && e->kind == BOOT_ENTRY_KIND_CHAINLOAD &&
        !(e->flags & BOOT_ENTRY_FLAG_TRUSTED_CHAINLOAD)) {
        record_reject(out, e->id, BOOT_REJECT_REASON_PATH_ESCAPE);
        return 0;
    }
    return 1;
}

/* Lexicographic compare -- shorter wins on tie (BLS sort_key semantics). */
static int sort_key_less(const char *a, const char *b)
{
    u32 i = 0;
    while (a[i] && b[i]) {
        if ((unsigned char)a[i] != (unsigned char)b[i]) {
            return (unsigned char)a[i] < (unsigned char)b[i];
        }
        i++;
    }
    return a[i] == '\0' && b[i] != '\0';
}

/* ---- Selected-kind path override (pure mapping) ------------------------- */

int boot_policy_kind_to_path(unsigned int kind,
                             boot_policy_path_override_t *out)
{
    if (!out) return 0;
    out->boot_path = 0u;
    out->boot_reason = 0u;
    out->src_flag_add = 0u;
    switch (kind) {
        case BOOT_ENTRY_KIND_RECOVERY:
            out->boot_path    = BOOT_POLICY_PATH_RECOVERY;
            out->boot_reason  = BOOT_POLICY_REASON_RECOVERY_TRIGGER;
            out->src_flag_add = BOOT_POLICY_SRC_FLAG_RECOVERY_TRIGGERED;
            return 1;
        case BOOT_ENTRY_KIND_DIAGNOSTICS:
            out->boot_path   = BOOT_POLICY_PATH_DIAGNOSTIC;
            out->boot_reason = BOOT_POLICY_REASON_DIAGNOSTIC_REQUEST;
            return 1;
        case BOOT_ENTRY_KIND_NETWORK:
            out->boot_path   = BOOT_POLICY_PATH_NETWORK;
            out->boot_reason = BOOT_POLICY_REASON_USER_SELECTED;
            return 1;
        case BOOT_ENTRY_KIND_RESUME:
            out->boot_path   = BOOT_POLICY_PATH_RESUME;
            out->boot_reason = BOOT_POLICY_REASON_USER_SELECTED;
            return 1;
        default:
            return 0;
    }
}

/* ---- Ladder entrypoint -------------------------------------------------- */

void boot_policy_decide(const boot_policy_inputs_t *inputs,
                        const boot_entries_parse_result_t *parse,
                        const boot_counter_t *counters,
                        unsigned int counter_count,
                        boot_policy_decision_t *out)
{
    if (!inputs || !parse || !out) return;
    sp_zero(out, sizeof(*out));
    out->reason = BOOT_SELECTION_UNSET;

    /* Normalize the optional counter array: NULL means "no counters tracked",
     * but a stale or buggy caller could pass NULL with a nonzero count and
     * crash counter_lookup() below. Treat NULL as zero count regardless.
     * Also cap counter_count at BOOT_ENTRIES_MAX_ENTRIES so a hostile or
     * stale ESP that scanned thousands of `<id>+<L>-<D>` files cannot turn
     * a single pre-EBS policy decision into O(entries x counters) work
     * with both bounds unbounded. The bootloader-side counter scan caps
     * at MAX_ENTRIES too, but enforcing here keeps the contract
     * load-bearing regardless of caller. */
    if (!counters) counter_count = 0u;
    if (counter_count > BOOT_ENTRIES_MAX_ENTRIES) counter_count = BOOT_ENTRIES_MAX_ENTRIES;

    /* Counter-scan overflow: a hostile/stale ESP could flood the counter
     * directory with thousands of files; the bootloader-side scan caps at
     * MAX_ENTRIES, but a tries-exhausted record beyond that cap would be
     * silently dropped, allowing an exhausted entry to boot un-gated.
     * Fail closed: when the caller signals overflow, fall straight to the
     * fallback envelope so the operator can recover via Branch B-wired
     * counter cleanup. */
    if (inputs->counters_overflow) {
        out->reason = BOOT_SELECTION_FALLBACK_NO_VIABLE;
        boot_entries_synthesize_fallback(inputs->invoked_via_uki ? 1 : 0,
                                         &out->selected);
        sp_copy_clamped(out->selected_entry_id, sizeof(out->selected_entry_id),
                        out->selected.id, sp_strlen(out->selected.id));
        return;
    }

    /* STORE_INVALID short-circuit: the parser failed, the caller will use
     * boot_entries_synthesize_fallback(). Nothing to filter or pick. */
    if (parse->reject_code != BOOT_ENTRIES_OK) {
        out->reason = BOOT_SELECTION_FALLBACK_STORE_INVALID;
        return;
    }

    /* First pass: filter the entries array. The records that miss go into
     * rejected[]; the survivors stay candidates for the precedence ladder. */
    int candidate[BOOT_ENTRIES_MAX_ENTRIES];
    for (unsigned int i = 0; i < parse->entry_count; i++) {
        candidate[i] = entry_passes_filter(&parse->entries[i], inputs,
                                           counters, counter_count, out);
    }

    /* --- Ladder priority 1: hotkey override. Operator picks by 1-based
     * menu index in the ORIGINAL entry order; the index is post-filter
     * (the menu only shows non-hidden non-skipped active entries). */
    if (inputs->hotkey_override_index > 0u) {
        unsigned int seen = 0;
        for (unsigned int i = 0; i < parse->entry_count; i++) {
            if (!candidate[i]) continue;
            seen++;
            if (seen == inputs->hotkey_override_index) {
                sp_copy_clamped(out->selected_entry_id,
                                sizeof(out->selected_entry_id),
                                parse->entries[i].id,
                                sp_strlen(parse->entries[i].id));
                out->selected = parse->entries[i];
                out->reason = BOOT_SELECTION_HOTKEY;
                return;
            }
        }
        /* Hotkey index out of range -> fall through to next priority. */
    }

    /* --- Ladder priority 2: watchdog rollback. Skip the most-recently-
     * attempted entry (max tries_done among candidates) and pick the next
     * viable. If all candidates have tries_done = 0, watchdog has nothing
     * to roll back FROM; the request falls through. */
    if (inputs->watchdog_rollback) {
        unsigned int peak_idx = 0xFFFFFFFFu;
        unsigned int peak_done = 0u;
        for (unsigned int i = 0; i < parse->entry_count; i++) {
            if (!candidate[i]) continue;
            boot_counter_t c;
            unsigned int done = 0u;
            if (counter_lookup(counters, counter_count,
                               parse->entries[i].id, &c)) {
                done = c.tries_done;
            }
            if (done > peak_done) {
                peak_done = done;
                peak_idx = i;
            }
        }
        if (peak_idx != 0xFFFFFFFFu && peak_done > 0u) {
            /* Suppress the peak from ALL subsequent ladder priorities so a
             * fall-through cannot reselect the failing entry that watchdog
             * is rolling back from. If no alternate viable candidate
             * exists, the ladder must reach FALLBACK_NO_VIABLE. */
            candidate[peak_idx] = 0;
            record_reject(out, parse->entries[peak_idx].id,
                          BOOT_REJECT_REASON_TRIES_EXHAUSTED);

            /* Pick the first other candidate by sort_key. */
            unsigned int chosen = 0xFFFFFFFFu;
            for (unsigned int i = 0; i < parse->entry_count; i++) {
                if (!candidate[i]) continue;
                if (chosen == 0xFFFFFFFFu ||
                    sort_key_less(parse->entries[i].sort_key,
                                  parse->entries[chosen].sort_key)) {
                    chosen = i;
                }
            }
            if (chosen != 0xFFFFFFFFu) {
                sp_copy_clamped(out->selected_entry_id,
                                sizeof(out->selected_entry_id),
                                parse->entries[chosen].id,
                                sp_strlen(parse->entries[chosen].id));
                out->selected = parse->entries[chosen];
                out->reason = BOOT_SELECTION_WATCHDOG_ROLLBACK;
                return;
            }
            /* Peak was the only viable entry. Fall through, but the peak is
             * now suppressed so the rest of the ladder cannot pick it. */
        }
    }

    /* --- Ladder priority 3: A/B try-state. The slot index maps to the
     * Nth candidate (0-based). A/B integration owns the policy that maps
     * slot index to a specific entry pair; pre-integration, we accept
     * whatever index the prober supplies. `ab_slot_valid` MUST be set --
     * a zero-initialized inputs struct (BSS / AllocateZeroPool) has
     * valid=0 and skips this priority, preventing accidental slot-0
     * selection that would override recovery + store-default. */
    if (inputs->ab_slot_valid &&
        inputs->ab_slot_index != BOOT_POLICY_NO_AB_SLOT) {
        unsigned int seen = 0;
        for (unsigned int i = 0; i < parse->entry_count; i++) {
            if (!candidate[i]) continue;
            if (seen == inputs->ab_slot_index) {
                sp_copy_clamped(out->selected_entry_id,
                                sizeof(out->selected_entry_id),
                                parse->entries[i].id,
                                sp_strlen(parse->entries[i].id));
                out->selected = parse->entries[i];
                out->reason = BOOT_SELECTION_AB_TRY_STATE;
                return;
            }
            seen++;
        }
    }

    /* --- Ladder priority 4: recovery request. Pick the first KIND_RECOVERY
     * candidate by sort_key. */
    if (inputs->recovery_requested) {
        unsigned int chosen = 0xFFFFFFFFu;
        for (unsigned int i = 0; i < parse->entry_count; i++) {
            if (!candidate[i] || parse->entries[i].kind != BOOT_ENTRY_KIND_RECOVERY) continue;
            if (chosen == 0xFFFFFFFFu ||
                sort_key_less(parse->entries[i].sort_key,
                              parse->entries[chosen].sort_key)) {
                chosen = i;
            }
        }
        if (chosen != 0xFFFFFFFFu) {
            sp_copy_clamped(out->selected_entry_id,
                            sizeof(out->selected_entry_id),
                            parse->entries[chosen].id,
                            sp_strlen(parse->entries[chosen].id));
            out->selected = parse->entries[chosen];
            out->reason = BOOT_SELECTION_RECOVERY_REQUEST;
            return;
        }
    }

    /* --- Ladder priority 5: store default. BootCurrent is a SOFT HINT --
     * if the BootCurrent-resolved id is among the candidates, prefer it;
     * otherwise pick the candidate with the lowest sort_key. */
    if (inputs->bootcurrent_known && inputs->bootcurrent_entry_id[0] != '\0') {
        for (unsigned int i = 0; i < parse->entry_count; i++) {
            if (!candidate[i]) continue;
            if (sp_str_eq(parse->entries[i].id, inputs->bootcurrent_entry_id)) {
                sp_copy_clamped(out->selected_entry_id,
                                sizeof(out->selected_entry_id),
                                parse->entries[i].id,
                                sp_strlen(parse->entries[i].id));
                out->selected = parse->entries[i];
                out->reason = BOOT_SELECTION_BOOTNEXT_HINT;
                return;
            }
        }
        /* BootCurrent did not map to any candidate. Record the diagnostic
         * but keep walking -- the store default still applies. */
        record_reject(out, inputs->bootcurrent_entry_id,
                      BOOT_REJECT_REASON_KIND_UNAVAILABLE);
    }

    /* Lowest sort_key wins; tiebreak by array order (first occurrence). */
    unsigned int chosen = 0xFFFFFFFFu;
    for (unsigned int i = 0; i < parse->entry_count; i++) {
        if (!candidate[i]) continue;
        if (chosen == 0xFFFFFFFFu ||
            sort_key_less(parse->entries[i].sort_key,
                          parse->entries[chosen].sort_key)) {
            chosen = i;
        }
    }
    if (chosen != 0xFFFFFFFFu) {
        sp_copy_clamped(out->selected_entry_id,
                        sizeof(out->selected_entry_id),
                        parse->entries[chosen].id,
                        sp_strlen(parse->entries[chosen].id));
        out->selected = parse->entries[chosen];
        if (inputs->bootcurrent_known &&
            inputs->bootcurrent_entry_id[0] == '\0') {
            /* BootCurrent was advertised but its OptionalData lacked our
             * tag -- we record the diagnostic via UNKNOWN_BOOTCURRENT. */
            out->reason = BOOT_SELECTION_UNKNOWN_BOOTCURRENT;
        } else {
            out->reason = BOOT_SELECTION_STORE_DEFAULT;
        }
        return;
    }

    /* --- Ladder priority 6: fallback (no viable candidate). The public
     * contract states that `selected` is valid for every reason except
     * UNSET and FALLBACK_STORE_INVALID, so we synthesize the fallback
     * envelope inline here. The caller still needs to handle
     * STORE_INVALID separately because the parser result is unusable in
     * that case (no store to read uki_mode from). */
    out->reason = BOOT_SELECTION_FALLBACK_NO_VIABLE;
    /* Honor the caller's UKI invocation context so a UKI boot that
     * exhausts all viable entries gets a kind=uki fallback envelope, not
     * a split-path one. invoked_via_uki=0 (default) -> kind=split. */
    boot_entries_synthesize_fallback(inputs->invoked_via_uki ? 1 : 0,
                                     &out->selected);
    sp_copy_clamped(out->selected_entry_id, sizeof(out->selected_entry_id),
                    out->selected.id, sp_strlen(out->selected.id));
}

/* ---- Conservative duplicate dedupe ------------------------------------- */

int boot_policy_counter_dedup_insert(boot_counter_t *out,
                                     unsigned int *count,
                                     unsigned int cap,
                                     const boot_counter_t *cand)
{
    if (!out || !count || !cand || cap == 0u) return -1;
    /* Look for existing entry with the same id. Merge ALWAYS wins over
     * cap-full -- a duplicate id is just an update of a slot already
     * occupied, so the array does not grow and the dedupe must be
     * allowed to apply worst-case demotion. */
    for (unsigned int i = 0; i < *count; i++) {
        const char *a = out[i].id;
        const char *b = cand->id;
        unsigned int j;
        int eq = 1;
        for (j = 0; j < sizeof(out[i].id); j++) {
            if (a[j] != b[j]) { eq = 0; break; }
            if (a[j] == 0) break;
        }
        if (eq) {
            /* Conservative: lowest tries_left wins; highest tries_done wins.
             * Worst-case demotion -- never silently un-demote across a torn
             * rename. */
            if (cand->tries_left < out[i].tries_left)
                out[i].tries_left = cand->tries_left;
            if (cand->tries_done > out[i].tries_done)
                out[i].tries_done = cand->tries_done;
            return 0;  /* merged */
        }
    }
    if (*count >= cap) return -1;  /* cap-full, NEW id, reject */
    out[*count] = *cand;
    (*count)++;
    return 1;  /* appended */
}

/* ---- Boot menu pure logic helpers -------------------------------------- */

int boot_policy_menu_should_show(const boot_policy_decision_t *decision,
                                 const boot_entries_parse_result_t *parse,
                                 unsigned int viable_count)
{
    if (!decision || !parse) return 0;
    if (viable_count < 2u) return 0;
    switch ((unsigned int)decision->reason) {
        case BOOT_SELECTION_STORE_DEFAULT:
        case BOOT_SELECTION_BOOTNEXT_HINT:
        case BOOT_SELECTION_UNKNOWN_BOOTCURRENT:
            return 1;
        default:
            return 0;
    }
}

unsigned int boot_policy_menu_collect(
    const boot_entries_parse_result_t *parse,
    const boot_policy_decision_t *decision,
    unsigned int *out_idx, unsigned int cap,
    unsigned int *out_default_idx)
{
    unsigned int n = 0;
    int default_found = 0;
    if (out_default_idx) *out_default_idx = BOOT_POLICY_MENU_DEFAULT_NOT_FOUND;
    if (!parse || !decision || !out_idx || cap == 0u) return 0;
    /* A REJECTED store offers no candidates. boot_policy_decide() already
     * short-circuits to FALLBACK_STORE_INVALID without filtering, which means
     * `decision->rejected[]` is EMPTY -- so the hard-hide filters below have
     * nothing to match on and every entry the parser happened to retain before
     * it hit the rejection would be listed and selectable. entry_count is
     * documented meaningful on BOOT_ENTRIES_OK only (boot_entries_parser.h);
     * reading it otherwise is reading indeterminate state.
     *
     * That mattered concretely: a store rejected for a repeated `entries` key
     * still exposed the entries parsed before the repeat, and picking one
     * through the F11 menu replaced the fallback selection with it -- bypassing
     * the active/machine_id filtering the ladder would have applied. The
     * rejection is only a rejection if every consumer honours it. */
    if (parse->reject_code != BOOT_ENTRIES_OK) return 0;
    /* If the decision did not name a selected entry (empty id, e.g.
     * STORE_INVALID), there is nothing to find; treat as
     * default-not-applicable rather than not-found so the caller
     * still gets visible candidates without artificially suppressing
     * the menu. */
    int decision_has_selected = (decision->selected_entry_id[0] != 0);
    for (unsigned int i = 0; i < parse->entry_count && n < cap; i++) {
        const boot_entry_envelope_t *e = &parse->entries[i];
        if (e->kind_skipped) continue;
        if (e->flags & BOOT_ENTRY_FLAG_HIDDEN) continue;
        /* Filter all HARD-HIDE reasons -- structural exclusions where
         * the loader cannot or will not boot the entry. The single
         * exception is TRIES_EXHAUSTED, which is a demote signal:
         * the entry CAN still be manually retried, so it stays
         * visible in the menu (the greyed-out + last_failure_reason
         * label UX is owned by section 9, A/B and Recovery
         * integration). All other reasons (KIND_SKIPPED,
         * KIND_UNAVAILABLE, NOT_ACTIVE, HIDDEN, MACHINE_ID_MISMATCH,
         * PATH_ESCAPE) hide the entry: showing them as selectable
         * would let the operator pick something the loader cannot
         * actually execute, falsifying the v19 audit trail. */
        int rejected = 0;
        for (unsigned int r = 0; r < decision->rejected_count; r++) {
            unsigned int reason = decision->rejected[r].reason;
            if (reason == (unsigned int)BOOT_REJECT_REASON_TRIES_EXHAUSTED)
                continue;
            if (reason == (unsigned int)BOOT_REJECT_REASON_NONE)
                continue;
            const char *a = e->id;
            const char *b = decision->rejected[r].id;
            unsigned int j;
            int eq = 1;
            for (j = 0; j < sizeof(e->id); j++) {
                if (a[j] != b[j]) { eq = 0; break; }
                if (a[j] == 0) break;
            }
            if (eq) { rejected = 1; break; }
        }
        if (rejected) continue;
        out_idx[n] = i;
        if (out_default_idx && decision_has_selected) {
            const char *a = e->id;
            const char *b = decision->selected_entry_id;
            unsigned int j;
            int eq = 1;
            for (j = 0; j < sizeof(e->id); j++) {
                if (a[j] != b[j]) { eq = 0; break; }
                if (a[j] == 0) break;
            }
            if (eq) {
                *out_default_idx = n;
                default_found = 1;
            }
        }
        n++;
    }
    /* Sentinel resolution:
     *   - decision had no selected id: leave NOT_FOUND. Caller
     *     interprets as "no anchor"; menu can render with index 0
     *     highlighted (boot_policy_invoke picks index 0 if NOT_FOUND).
     *   - decision had a selected id AND we found it within cap:
     *     out_default_idx already set above.
     *   - decision had a selected id AND we did NOT find it (cap-
     *     truncation OR filtered out OR id no longer in store):
     *     leave NOT_FOUND. Caller MUST suppress the menu otherwise
     *     the visible default diverges from the actual decision. */
    if (!decision_has_selected || default_found) {
        /* nothing to do; either NOT_FOUND-as-no-anchor or set above */
        (void)0;
    }
    return n;
}
