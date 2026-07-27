# TODO bookkeeping -- the Notes block and the loose-ends sweep

Read on demand from `implement-todo-section` steps 10 and 18. The step text in SKILL.md carries the mandatory actions
(write a Notes block; sweep the file and its XREF targets). This file carries the canonical shapes and the reasons.

Both policies have a hook backstop: `notes_bloat_check.py` BLOCKs an over-long Notes block, and `accepted_xref_block.py`
/ `accepted_xref_warn.py` gate the XREF concreteness rule. The prose here is the explanation, the hooks are the
enforcement.

## The `> **Notes:**` block (step 10)

MANDATORY for sections marked `[x]` or `[/]` that shipped any artifact. Placement: immediately after the pre-stamp
`> **Test runner:**` line, before the `> **Verified:**` stamp.

**HARD RULE: 3-6 bullets, ONE LINE EACH** (one logical line in the markdown source -- a long single bullet is fine, a
wrapped multi-paragraph bullet is not).

**FORBIDDEN patterns (the PreToolUse hook BLOCKs edits that introduce these):**

- More than 6 top-level bullets in the Notes block.
- Indented sub-bullets (`>   - ...`) under a Notes bullet.
- Per-finding adoption sub-blocks like "Design review adoptions:" / "Implementation adversarial adoptions:" / "Latent
  bug fixed:" / "Bundle root discovery:" with their own enumerated children. Adoption details (Codex finding evidence,
  file:line citations, per-dispatch breakdowns, "before this fix / after this fix" prose) belong in **commit
  messages**, NOT in Notes.
- Inventing extra bullets beyond the canonical shape below to capture review-pipeline narrative. If the canonical
  shape can't hold a fact, that fact lives in the commit message or in a concrete `[ ]` checklist item in a dependent
  section.

**Canonical shape (one bullet each, in this order):**

- **What shipped** -- name the concrete artifact (filename + one-phrase purpose) and any key knob/count (line count,
  sentinel count, hook count, etc.).
- **How it runs / integrates** -- invocation path, idempotence claim, how it fires (hook / skill / CI). Skip if the
  artifact is pure docs.
- **Downstream effects** -- other TODOs it satisfies or unblocks, stamp sweeps it enables. May also point at the commit
  message for review-pipeline adoption details (e.g. "Codex 4x review adoptions in commit `<hash>`").
- **Canonical doc** -- single link to the authoritative file for this section's subject (usually
  `docs/<area>/<topic>.md` or a `CLAUDE.md` anchor). One link, not three.
- **Scope boundary** -- name what this section does NOT own, with pointers (e.g. "section 7 owns X; TODO-XX section N
  owns Y").
- For docs-only sections, collapse "How it runs" and "Downstream effects" into one "Structure / consumers" bullet.

**Purpose:** Notes is a 30-second human scan summary, NOT a review-pipeline transcript. The stamps are the
machine-readable audit trail; commit messages are the per-finding evidence trail; Notes is "what shipped, how it
integrates, what's still owed elsewhere." A contributor six months from now should be able to read this block in 30
seconds. If you find yourself drafting a bullet that needs a colon-introduced sub-list or a multi-paragraph
explanation, STOP -- promote the structural fact to a single sentence and move the rest to the commit message. Same
applies if you find yourself reaching for "Latent bug fixed", "Design review adoptions", or per-finding bullets: those
signal Notes is being misused as a review log.

## Inbound `> **Accepted:**` + `> **Deferred:**` sweep (step 18, MANDATORY)

Any `[x]` item this section just closed is almost certainly the target of `> **Accepted:**` stamps (from other TODO
sections pointing at us) or `> **Deferred:**` stamps (from this TODO file pointing at us).

**Fast path:** `python3 scripts/todo-graph/query.py deferred-by <id>` enumerates every inbound Accepted/Deferred stamp
pointing at the target TODO (resolves by frontmatter id / filename stem / slug; one row per stamp with severity + kind
+ section + item_name). Use it to short-circuit the `grep -rn "TODO-XX <section-ref>"` sweep (and, if the item name was
quoted, the quoted item name too) across `todo/`. If the query returns empty, the sweep is done; if it returns rows,
walk each inbound reference.

Note: stamp resolution requires the cache to be current -- a recent
`bash scripts/todo-graph/build-and-validate.sh --keep-cache` run or the PostToolUse auto-rewrite hook keeps the cache
fresh.

For each match:

- If the inbound entry's concern is FULLY resolved by this section's work: DELETE the entire `> **Accepted:**` or
  `> **Deferred:**` line. If it was the only deferred entry on that line, remove the line entirely; keep the Verified +
  Quality reviewed stamps adjacent.
- If the inbound entry is PARTIALLY resolved (you closed one of several concerns on the line): rewrite the line,
  dropping the resolved concern while preserving the remaining XREFs.
- If uncertain, leave the entry and note the ambiguity in chat so the user can decide.

**Why this step exists:** otherwise Accepted/Deferred lines accumulate indefinitely and lose their value as a "what's
still deferred on this section" scan target. The XREF target moving to `[x]` is exactly when the inbound reference
becomes stale.

Semantic reminder: `Accepted:` = out-of-scope for the emitting section, owner is elsewhere; `Deferred:` = in-scope for
the emitting TODO, owner is later. Both need sweeping when their target closes. Full stamp field rules (severity tags, reason parentheticals):
[review-todo-section references/stamp-fields.md](../../review-todo-section/references/stamp-fields.md).

## Accepted-XREF concreteness check (step 18, MANDATORY)

For every Codex finding that step 13 marked "Accepted with XREF" (out-of-scope deferral), open the XREF target and
verify a **concrete `[ ]` checklist item** exists that would close the gap when checked. A section title, an enum
definition, or prose mention is NOT concrete.

If the target lacks such an item, create one NOW: write a checklist item that names the source file/function to fix,
the helper to add (with signature), and the validation behavior. If no owner section exists or fits, follow the
scope-gap protocol Branch C/D to create one BEFORE marking the section complete. Update the Accepted XREF in any chat
output and in TODO stamps to reference the concrete item by name/line.

**Why this matters:** "Accepted with XREF: TODO-XX section N" with no concrete item there is a paper trail that someone
later finds empty. Every accepted finding must be exactly one `[x]` away from being fully closed.

## Filed-in-owner check (step 18)

Any follow-up `[ ]` item that names an owner (e.g., "tracked in TODO-XX section N", "owner: TODO-YY") must ALSO be
filed as a checklist item in that owner section with a reciprocal XREF. If the owner section doesn't exist yet, find or
create one via scope-gap protocol Branch C/D before filing. A note alone is a dead-end paper trail.
