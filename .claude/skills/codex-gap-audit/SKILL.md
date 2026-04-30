---
name: codex-gap-audit
description: Codex-driven adversarial audit of a TODO gap-analysis result. Red-teams the feature inventory and gap classification produced by `gap-audit-todo` -- catches missed Win11/Linux features, parity claims that don't hold, dead-end XREFs, ownerless adjacent work, and refinement blind spots BEFORE the gaps turn into TODO edits. Mandatory secondary pass in every gap-audit run.
---

# Codex Gap Audit

> **External-Reviewer Contract:** Codex is a subordinate reviewer, not authority. Every finding from this skill goes through `superpowers:receiving-code-review` (verify against actual Win11/Linux source-of-truth at file:line / spec section, Fix / Reject / Accept, never blind-implement). Canonical contract: [docs/infrastructure/ai-system.md#external-reviewer-contract-codex](../../../docs/infrastructure/ai-system.md#external-reviewer-contract-codex).

## Prompt Shape

Every dispatch from this skill MUST open its prompt with the marker `[review-kind: gap-audit] <todo-path>` on the first non-blank line. The marker is what `.claude/hooks/skill_step_observer.py` and any future gap-audit gates use to attribute the dispatch. Canonical reference for all review-kind markers: [.claude/skills/codex-prompt-shape.md](../codex-prompt-shape.md).

## Use This Skill When

- `gap-audit-todo` Phase 3.5 invokes it (mandatory).
- A TODO file's gap analysis output (feature inventory + classified gaps) needs an independent red-team pass before Phase 4 turns it into TODO edits.
- The user asks "did the gap analysis miss anything?" or "is this competitive picture honest?"
- Cross-domain TODO planning where multiple sibling TODOs claim coverage and the ownership lines need adversarial scrutiny.

## What This Skill Audits

Unlike `codex-adversarial-review-section` (reviews shipped code) or `codex-design-review` (reviews pre-implementation design for one section), this skill audits **planning text**:

- **Feature inventory completeness:** what Win11/Linux features did the analysis miss?
- **Parity classification accuracy:** does Win11 actually have this? Does Linux? Are the ✅/⚠️/❌ marks honest?
- **Gap classification correctness:** is this really a "covered elsewhere" via XREF, or does the XREF target lack a concrete `[ ]` item that would close the gap?
- **Ownerless adjacent work:** what nearby capability is required for the feature to feel real that the analysis didn't name an owner for?
- **Refinement / competitive-edge realism:** are the ⭐ "we can be first or best" claims supported by an actual gap in Win11/Linux, or wishful thinking?
- **False completeness:** does any existing section claim adequate coverage when the implementation underneath would obviously still leave a real user/caller hitting a wall?
- **Stale OS-comparison rows:** do any rows still say "Planned" or carry placeholder text when the underlying section has shipped?

## Workflow

1. **Pre-fetch the inventory** from the calling skill. The Phase 3 output of `gap-audit-todo` is:
   - The full feature inventory list (per-feature: name, Win11 status, Linux status, classification).
   - The list of detected gaps grouped by classification (parity / Win11-only / Linux-only / competitive-edge / false-completeness / wiring / refinement).
   - The list of "covered elsewhere" XREFs with their target sections.
   - The proposed new sections / ownership changes / parity-row updates.

2. **Pre-fetch repo-side facts the auditor needs.** Codex cannot fully verify XREFs without seeing the target sections. Before the dispatch:
   - For every "covered elsewhere" XREF, read the target TODO section and capture whether a concrete `[ ]` item addresses the cited gap.
   - For every "shipping in this TODO" claim, capture the relevant section's status (`[ ]` / `[/]` / `[x]`) and Notes block (if any).
   - Paste the captured snippets into the Codex prompt as the verified context set so Codex audits actual divergence, not heuristically-discovered claims.

3. **Dispatch to Codex via the canonical wrapper:**
   ```bash
   bash scripts/codex-dispatch.sh '[review-kind: gap-audit] <todo-path> <prompt>'
   ```

4. **Triage findings** -- the PostToolUse hook fires `receiving-code-review` reminder; follow it on every finding. Gap-audit false-positive watch:
   - Codex sometimes invents Win11 features that don't exist (or attributes Linux features to Windows).
   - Codex flags "covered elsewhere" as missing when the XREF target IS concrete -- always re-check the target section before fixing.
   - Codex pushes for parity with experimental/research-only OS features the project explicitly chose not to track.
   - Codex flags refinement opportunities that are already on the TODO under different wording.
   - Open both sides of every claimed gap yourself before changing the inventory.

5. **Categorize verified findings:**
   - **Inventory miss** -- Win11/Linux feature the analysis didn't list. Add to inventory + classify the gap.
   - **Parity-claim drift** -- ✅ / ⚠️ / ❌ status mark contradicts the actual Win11/Linux state. Correct the row.
   - **Dead-end XREF** -- "covered elsewhere" target lacks a concrete `[ ]` item closing the gap. Either retarget at a real item or create a concrete item in the target section.
   - **Ownerless adjacent work** -- nearby capability the section needs to feel real, with no owner. Add as a concrete `[ ]` item in this TODO or file with reciprocal XREF in the owner TODO.
   - **Stale OS-comparison row** -- row says ⬜ / "Planned" but the underlying section has shipped. Update to ✅ + concrete description.
   - **Wishful competitive edge** -- ⭐ "we can be first or best" without a concrete gap in Win11/Linux. Demote to parity (💎) or remove.
   - **False completeness** -- existing section technically lists the work but underneath leaves a real caller hitting a wall. Add the missing piece as a concrete `[ ]` item.

6. **Fold verified findings into the gap inventory** BEFORE the calling skill enters Phase 4 (Add Missing Sections). The whole point of the secondary pass is to correct the inventory before it turns into TODO edits.

7. **Reject false positives with evidence** -- name the rejection in chat (one line per rejected finding) so the user sees the rationale: "F2 Codex claimed Linux has X; verified absent at <kernel.org URL or man page>." Same evidence-first contract as code review.

8. **Cross-TODO duplicate check before adopting a finding as a new section/item (MANDATORY).** Codex audits this TODO in isolation; cross-TODO ownership is Claude's job, not Codex's. For every finding that would create a new `## N.` section or new `[ ]` item in this TODO:
   - Grep across `todo/` for the feature name AND likely synonyms / variant phrasings.
   - Use `mcp__todo-graph__by-domain` to enumerate the relevant domain's sibling TODOs and `mcp__todo-graph__code <symbol>` for any cited function/struct.
   - Read every hit. If the gap is already owned elsewhere: do NOT create a duplicate. Either accept-XREF (citing the existing concrete `[ ]` item by name) or sharpen the existing target item in place.
   - For refinement / competitive-edge findings: also scan `docs/infrastructure/` and `CLAUDE.md` for prior decisions. The project may have already evaluated and rejected the proposed approach.
   - A finding that survives the duplicate check is genuinely new and safe to fold into the inventory; a finding that fails the check is filed as a sharpening of the existing item, never a new section.

9. **Accept out-of-scope findings with concrete XREF** -- if the finding is genuinely valid but belongs to a different TODO, follow the same rule the section-stamp Accepted-XREF check enforces: the XREF target MUST be a concrete `[ ]` item, not a section title.

## Prompt Template

```
[review-kind: gap-audit] todo/<domain>/TODO-XX-<slug>.md -- gap-analysis red-team

CONTEXT
=======
TODO file: todo/<domain>/TODO-XX-<slug>.md (paste full file or relevant sections).
Sibling TODOs that overlap: <list with one-line scope each>.

FEATURE INVENTORY (as classified by gap-audit-todo Phase 3)
==========================================================
<paste the merged Win11/Linux/state-of-art feature list with status marks>

DETECTED GAPS (proposed for Phase 4 action)
===========================================
- Parity gaps (💎): <list>
- Win11-only gaps: <list>
- Linux-only gaps: <list>
- Competitive-edge gaps (⭐): <list>
- False-completeness gaps: <list>
- Wiring gaps: <list>
- Refinement gaps: <list>

COVERED-ELSEWHERE XREFS (verified target snippets)
=================================================
<for each, paste target section text + the specific [ ] item claimed to close the gap>

CURRENT OS COMPARISON TABLE
===========================
<paste the table>

AUDIT ANGLES (mandatory)
========================
1. Inventory completeness: what Win11/Linux feature in this domain is missing from the inventory?
2. Parity-claim accuracy: does any ✅ / ⚠️ / ❌ status mark contradict actual Win11 (Microsoft Learn / Windows Internals 7e) or Linux (kernel.org Documentation/, man pages, source) reality?
3. XREF concreteness: does any "covered elsewhere" XREF target lack a concrete `[ ]` item that would close the gap when checked?
4. Ownerless adjacent work: what nearby capability is required for the feature to feel real that has no owner section/TODO?
5. Wishful competitive edge: is any ⭐ "we can be first or best" claim unsupported by an actual gap in Win11/Linux?
6. False completeness: does any existing section claim adequate coverage when the underlying implementation would obviously still leave a real user/caller hitting a wall?
7. Stale OS-comparison rows: do any rows still say ⬜ / "Planned" / placeholder when the underlying section has shipped?

Verdict + per-finding severity (H / M / L) + recommendation per finding.
Reject false positives with citation (URL or section number from official source).
```

## Guardrails

- **Mandatory in `gap-audit-todo`.** The skill is wired into Phase 3.5 of `gap-audit-todo`. Skipping requires the same scope-gap-protocol-style justification as skipping any other mandatory step (docs-only sweep with zero new sections / ownership changes / parity claims).
- **Prompt shape is non-negotiable.** Single-quoted argv via the canonical Codex dispatch wrapper (see the Workflow step 3 example). The `[review-kind: gap-audit]` marker on the first non-blank line is what telemetry attributes against. Multi-argv misuse fails loud at the wrapper.
- **Verify against source-of-truth, not Codex's word.** Microsoft Learn, kernel.org Documentation, official specs. Codex hallucinates feature names; the receiving rule applies.
- **Findings flow back into the inventory BEFORE Phase 4.** The whole point is to correct the inventory before it turns into TODO edits. Folding findings AFTER Phase 4 means re-doing the section drafts.
- **Accepted-XREF concreteness rule applies here too.** "Accepted with XREF: TODO-XX §N" is rejected; "Accepted with XREF: TODO-XX §N (item: ...at line N)" is the bar.
