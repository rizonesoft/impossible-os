---
name: gap-analysis-todo
description: Deep gap analysis of a TODO file against all overlapping TODOs -- find scope conflicts, stale sections, missing coverage, and unclear ownership. Use after creating or majorly editing a TODO, or when planning cross-domain work.
---

# Gap Analysis TODO

## Use This Skill When

- A TODO file needs to be checked for feature completeness against Windows 11 and Linux.
- The user asks "what's missing?" or "is this TODO complete?" or "does this match Win11/Linux?"
- A new TODO was just created and needs competitive analysis before implementation begins.
- Cross-domain work touches multiple TODOs and ownership or coverage is unclear.

## Workflow

### Phase 1 -- Understand the TODO

1. **Read the full TODO file.** Understand its goal, scope, every section, every XREF, and the OS Comparison table.
2. **Verify the `> [!IMPORTANT] Current state:` callout exists** after the Goal paragraph. If missing, flag it -- the gap analysis is harder without a clear starting point. Add one based on codebase exploration.
3. **Extract the domain topic.** Identify the specific kernel/OS area this TODO covers (e.g., "UEFI boot hardening", "Object Manager", "NVMe storage driver", "desktop compositor"). This topic drives all subsequent research.
4. **Read the domain INDEX.md and sibling TODOs.** Understand what's already covered nearby to avoid duplicate work.
5. **Check CLAUDE.md "Bare Metal Gotchas" and "Safety Gates"** for any documented issues that affect this domain. Flag features that would be blocked by known gotchas (e.g., SMEP requires per-process page tables, NVMe is unreliable on WHPX).

### Phase 2 -- Internet Research (critical -- do not skip)

6. **Research Windows 11 features in this domain.**
   - Use WebSearch with queries like: `"Windows 11 <topic> internals"`, `"Windows NT <topic> architecture"`, `"Win32 <topic> API"`, `"Windows <topic> features 2025 2026"`.
   - Run at least 3 targeted searches. Follow promising results with WebFetch to read documentation pages, blog posts, and Microsoft Learn articles.
   - Build a feature inventory: list every distinct feature, API, behavior, or capability that Win11 provides in this domain.
   - Note implementation details that affect how Impossible OS should implement the same feature.
   - **Save URLs** for key findings -- these go into the `<!-- Sources -->` comment later.

7. **Research Linux features in this domain.**
   - Use WebSearch with queries like: `"Linux kernel <topic>"`, `"Linux <topic> subsystem"`, `"Linux <topic> implementation"`, `"Linux <topic> features 2025 2026"`.
   - Run at least 3 targeted searches. Follow promising results with WebFetch.
   - Build a feature inventory for Linux -- same level of detail as Win11.
   - Note where Linux does something differently or better than Windows.
   - **Save URLs.**

8. **Research emerging and state-of-art features.**
   - Use WebSearch for: `"modern OS <topic> best practices"`, `"<topic> innovations operating system"`, `"<topic> security hardening OS"`.
   - Look for features that NEITHER Win11 nor Linux implements well -- these are competitive edge opportunities for Impossible OS.
   - Look for common pain points developers/users have with Win11 and Linux in this domain.

### Phase 3 -- Feature Inventory and Gap Detection

9. **Compile the complete feature inventory.**
   - Merge all research into a single list organized by category.
   - For each feature, record: feature name, Win11 status (✅/⚠️/❌), Linux status (✅/⚠️/❌), and a one-line description.

10. **Compare inventory against the TODO's current sections.**
    - For each feature in the inventory:
      - If covered by an existing section: mark as covered, verify the section is adequate (not just a stub).
      - If covered by a sibling TODO: note with `-> XREF:` and mark as covered-elsewhere.
      - If NOT covered anywhere: this is a **gap**. Classify it:
        - **Parity gap (💎):** Both Win11 and Linux have it. Impossible OS MUST implement it.
        - **Win11-only gap:** Only Win11 has it. Evaluate whether it's important for Win32 API compatibility.
        - **Linux-only gap:** Only Linux has it. Evaluate whether it's relevant for Impossible OS's target audience.
        - **Competitive edge (⭐):** Neither OS does it well. Impossible OS can be first or best.
        - **Not applicable:** The feature doesn't fit Impossible OS's architecture or goals. Note why.
        - **Blocked by gotcha:** The feature hits a documented CLAUDE.md bare-metal gotcha. Note the blocker.

11. **Cross-TODO overlap check.**
    - Use Grep to search ALL domains for keywords from the TODO's section titles and deliverables.
    - For each match, classify the relationship:
      - **SUPERSEDES** -- this TODO replaces the other section entirely.
      - **COMPLEMENT** -- both contribute different aspects. Ensure bidirectional `-> XREF:`.
      - **CONFLICT** -- both claim to implement the same thing. Resolve: pick one owner, add scope boundary note.
      - **FOUNDATION** -- the other TODO provides infrastructure this one consumes. Add dependency XREF.
    - Check for stale sections in overlapping TODOs -- if a section was made obsolete by recent work, add a supersession note.
    - **Patch both sides in the same run (required):** for every SUPERSEDES/COMPLEMENT/CONFLICT/FOUNDATION decision, update the referenced TODO section too:
      - Add reciprocal `-> XREF:` links.
      - Add mirrored unchecked prerequisite/ownership items where needed.
      - If a referenced section/order row is marked complete but depends on unresolved required work, downgrade status there as well.
      - Keep edits limited to TODO files directly referenced by the finding.
    - Normalize dependency metadata when possible:
      - Add stable source item `ID:` tags.
      - Add target `SATISFIES:` links to those IDs for closure propagation.
      - Do not treat plain prose XREF text as sufficient for automatic completion propagation.
    - **XREF placement:** Inputs-level XREFs = structural dependencies. Section-level XREFs = implementation-time cross-references. Place each in the correct location.

12. **Code-truth completion audit (required, full-file).**
    - Scan EVERY checklist item in the target TODO (`[x]` and `[ ]`) against the live codebase.
    - Never trust checkbox state alone; validate by evidence (symbols, call sites, wiring/registration paths, normal-path behavior).
    - Treat a claim as complete only when implemented and wired. Stubs/placeholders/`STATUS_NOT_IMPLEMENTED` normal paths are not complete.
    - If a section is marked complete but has gaps, add missing unchecked items in that same section in prerequisite-first logical order.
    - **Strict evidence gate (non-optional):** every status change or newly added TODO item must cite proof in `path:line + symbol + reason` form.
    - Required evidence classes for completion claims:
      1. **Implementation evidence:** concrete symbol/body exists in non-test code.
      2. **Wiring evidence:** registration/dispatch/call path reaches the implementation.
      3. **Behavior evidence:** normal path is not stub/placeholder/`STATUS_NOT_IMPLEMENTED`.
    - If any evidence class is missing or ambiguous, keep/revert to `[ ]` and add a concrete follow-up unchecked item.
    - **Dependency-aware auto-closure gate (required):**
      1. Auto-close referenced target items only when explicit `ID` -> `SATISFIES` mapping exists.
      2. Require full target acceptance-criteria coverage by source evidence before `[x]` propagation.
      3. If partially satisfied, use `[/]` or `[ ]` and insert ordered missing items with ownership/XREF.
      4. Never auto-close from inferred relationship alone.
    - Every newly added missing item must include ownership and references:
      - owner file/function and/or owning TODO section
      - `-> XREF: TODO-XX §N` for external dependencies
      - placement before dependent tasks so execution order is valid
    - **Deterministic insertion order algorithm (when adding missing unchecked items):**
      1. Prerequisite type/struct/constants
      2. Core implementation
      3. Wiring/registration/dispatcher
      4. Error-path and privilege/validation checks
      5. Tests and verification hooks
      6. Commit/checkpoint item
    - Preserve existing style/layout while editing: keep table structure/icons/headers and edit only required status/content cells.
    - Include factual evidence in the report for each mismatch (path + symbol + why).

13. **Audit section sizes.** For each existing section, count checklist items. Flag any section with > 10 items -- it should be split into two sections. This applies to both existing sections and any new sections being added.

### Phase 4 -- Add Missing Sections

14. **Draft new sections for each significant gap.**
    - For each parity gap (💎): create a full section with:
      - Section heading: `## N. Feature Name`
      - One-line intro paragraph explaining what this feature does and why it matters.
      - Checklist items (`- [ ]`) with concrete deliverables (max 8-10 per section). Each item must reference specific functions, types, or APIs. No vague "implement X" items.
      - `**Test checkpoint:**` block with concrete pass/fail criteria, POST16 codes, and platforms.
      - Commit message item.
      - For boot-path/interrupt/page-table sections: `**Regression risk:**` note and `POST16(0xDDNN)` codes (check `boot_init.h` for conflicts).
      - For high-risk sections: `> [!WARNING]` callout noting SMP safety or bare-metal concerns.
    - For each competitive edge (⭐): same format, but add a `> [!TIP]` callout explaining why this is superior to Win11/Linux.
    - For Win11-only or Linux-only features: create sections only if they're important for compatibility or user experience. Otherwise note them as deferred with a reason.

15. **Renumber all sections.**
    - After inserting new sections, renumber ALL `## N. Title` headings sequentially from 1.
    - Group sections logically: foundational -> parity -> competitive edge.
    - Update all internal `§N` references in prose, callouts, and checklist items to match new numbers.

16. **Update the Implementation Order table.**
    - Add rows for every new section.
    - Mark each new row `💎` (parity) or `⭐` (exclusive).
    - Set `Depends On` correctly -- new sections should depend on existing foundation sections where applicable.
    - Keep existing section dependencies correct after renumbering.
    - Renumber all Order values sequentially.
    - **Status consistency rule:** if a section has unresolved required checklist items or newly discovered prerequisite gaps, its Implementation Order status cannot stay `[x]`; downgrade to `[/]` (or `[ ]` when appropriate).

17. **Update the OS Comparison table.**
    - Add rows for each new feature discovered during research.
    - Use a compact 5-column format with icon labels.
    - Header format: `⭐ | Feature | 🪟 Win11 | 🐧 Linux | 🚀 Impossible OS`
    - Keep cells short: status emoji + max 5 words.
    - Add the summary paragraph after the table noting competitive position.
    - **Add `<!-- Sources: URL1, URL2, ... -->` comment** after the summary with research URLs from steps 6-8.

18. **Update the Unit Tests section.**
    - Add test cases for new sections' deliverables.
    - Check if a new `TEST_CAT_*` category is needed for the new functionality. If so, note it.
    - Reserve POST16 debug codes for new boot-path sections (check `boot_init.h` for conflicts).

19. **Update the Outcome section.**
    - Add bullet points for any new major deliverables introduced by the new sections.
    - Keep the Outcome section concise -- one bullet per major capability, not per section.

20. **Update the Inputs section.**
    - If new sections reference source files not listed in Inputs, add them.
    - If new sections create dependencies on other TODOs, add `-> XREF:` lines (Inputs level = structural).
    - For missing items added into existing sections, add/verify ownership and prerequisite XREF entries as well.
    - For each new external dependency, patch bidirectional awareness immediately: add the missing back-reference and mirrored unchecked item to the target TODO section in the same run.

### Phase 5 -- Report

21. **Output a gap analysis report to the user.**
    - Use this fixed schema (always):
      1. **Demoted items** (previously `[x]`, now `[/]` or `[ ]` -- with evidence for each demotion)
      2. **Implemented but not marked** (with evidence)
      3. **Marked but not implemented/wired** (with evidence)
      4. **Missing prerequisites inserted** (ordered list + ownership + XREF)
      5. **Cross-TODO ownership decisions** (SUPERSEDES/COMPLEMENT/CONFLICT/FOUNDATION)
      6. **Parity/edge gaps** (fact vs inference labeled)
      7. **Oversized sections flagged** (> 10 items, recommend split)
      8. **Bare-metal gotcha blockers** (features blocked by documented CLAUDE.md gotchas)
      9. **Edits summary** (sections touched, status downgrades, section/order deltas)
      10. **Cross-file patches applied** (target file, section, inserted/downgraded items, reason)
      11. **Auto-closure decisions** (source ID, target ID, closure result, proof or missing criteria)
      12. **Estimated scope** (new sections added, estimated complexity: low/medium/high, test categories needed)
    - Every finding line must include a confidence tag: `confirmed` or `inferred`.
    - Include the final section count and checklist delta.

## Research Quality Standards

- **Minimum 6 web searches** per analysis (3 Win11 + 3 Linux). More for complex domains.
- **Follow at least 2 links** with WebFetch to get detailed feature descriptions, not just search summaries.
- **Save source URLs** -- add `<!-- Sources: ... -->` to the OS Comparison table.
- **Distinguish fact from inference.** If a feature's existence is inferred from documentation rather than confirmed, note it as "likely" rather than "confirmed."
- **Current information only.** Search for 2025/2026 content to avoid citing deprecated features. Windows 11 24H2+ and Linux 6.x+ are the comparison baseline.

## Guardrails

- Do NOT implement kernel code -- this skill creates TODO sections, not source files.
- Do NOT delete existing sections -- only add new ones and renumber.
- Do NOT mark existing checklist items as done without strict proof from code-truth evidence.
- You may downgrade stale `[x]` to `[ ]` and add missing unchecked items when evidence shows incompleteness. **Demoted items get their own prominent report section.**
- Apply the same strict-proof gate to referenced TODO files before any `[x]` status change.
- For cross-file dependency findings, update directly referenced target TODO sections in the same run; do not leave them as report-only follow-ups unless blocked.
- Never auto-close referenced TODO items without explicit `ID`/`SATISFIES` mapping and full target-criteria proof.
- If mapping metadata is absent, add linkage metadata and conservative follow-up checklist items instead of propagating `[x]`.
- Do NOT remove features from the TODO -- only add missing ones.
- If a feature is out of scope for this TODO, note it as `-> XREF:` to the appropriate TODO or as explicitly deferred with a reason.
- Keep new sections lean and actionable. No multi-paragraph prose blocks -- use checklist items with concrete deliverables.
- Max 8-10 checklist items per new section. Split if larger.
- When research is ambiguous (e.g., Windows internal feature with no public docs), note the uncertainty rather than guessing.
- Always scan the codebase for completion claims; TODO text is never source-of-truth by itself.
- Do not change visual formatting conventions (icons/table headers/callout style) unless the user explicitly asks for format changes.
- Do not leave a section marked complete when newly found prerequisite gaps remain unresolved.
- If confidence is below strict-proof threshold, choose conservative output: keep unchecked and add a concrete follow-up item with owner/XREF.
