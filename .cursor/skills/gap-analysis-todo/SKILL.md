---
name: gap-analysis-todo
description: Deep gap analysis of a TODO file against all overlapping TODOs — find scope conflicts, stale sections, missing coverage, and unclear ownership. Use after creating or majorly editing a TODO, or when planning cross-domain work.
---

# Gap Analysis TODO

## Use This Skill When

- A TODO file needs to be checked for feature completeness against Windows 11 and Linux.
- The user asks "what's missing?" or "is this TODO complete?" or "does this match Win11/Linux?"
- A new TODO was just created and needs competitive analysis before implementation begins.
- Cross-domain work touches multiple TODOs and ownership or coverage is unclear.

## Workflow

### Phase 1 — Understand the TODO

1. **Read the full TODO file.** Understand its goal, scope, every section, every XREF, and the OS Comparison table.
2. **Extract the domain topic.** Identify the specific kernel/OS area this TODO covers (e.g., "UEFI boot hardening", "Object Manager", "NVMe storage driver", "desktop compositor"). This topic drives all subsequent research.
3. **Read the domain INDEX.md and sibling TODOs.** Understand what's already covered nearby to avoid duplicate work.

### Phase 2 — Internet Research (critical — do not skip)

4. **Research Windows 11 features in this domain.**
   - Use WebSearch with queries like: `"Windows 11 <topic> internals"`, `"Windows NT <topic> architecture"`, `"Win32 <topic> API"`, `"Windows <topic> features 2025 2026"`.
   - Run at least 3 targeted searches. Follow promising results with WebFetch to read documentation pages, blog posts, and Microsoft Learn articles.
   - Build a feature inventory: list every distinct feature, API, behavior, or capability that Win11 provides in this domain.
   - Note implementation details that affect how Impossible OS should implement the same feature.

5. **Research Linux features in this domain.**
   - Use WebSearch with queries like: `"Linux kernel <topic>"`, `"Linux <topic> subsystem"`, `"Linux <topic> implementation"`, `"Linux <topic> features 2025 2026"`.
   - Run at least 3 targeted searches. Follow promising results with WebFetch.
   - Build a feature inventory for Linux — same level of detail as Win11.
   - Note where Linux does something differently or better than Windows.

6. **Research emerging and state-of-art features.**
   - Use WebSearch for: `"modern OS <topic> best practices"`, `"<topic> innovations operating system"`, `"<topic> security hardening OS"`.
   - Look for features that NEITHER Win11 nor Linux implements well — these are competitive edge opportunities for Impossible OS.
   - Look for common pain points developers/users have with Win11 and Linux in this domain.

### Phase 3 — Feature Inventory and Gap Detection

7. **Compile the complete feature inventory.**
   - Merge all research into a single list organized by category.
   - For each feature, record: feature name, Win11 status (✅/⚠️/❌), Linux status (✅/⚠️/❌), and a one-line description.

8. **Compare inventory against the TODO's current sections.**
   - For each feature in the inventory:
     - If covered by an existing section: mark as covered, verify the section is adequate (not just a stub).
     - If covered by a sibling TODO: note with `→ XREF:` and mark as covered-elsewhere.
     - If NOT covered anywhere: this is a **gap**. Classify it:
       - **Parity gap (💎):** Both Win11 and Linux have it. Impossible OS MUST implement it.
       - **Win11-only gap:** Only Win11 has it. Evaluate whether it's important for Win32 API compatibility.
       - **Linux-only gap:** Only Linux has it. Evaluate whether it's relevant for Impossible OS's target audience.
       - **Competitive edge (⭐):** Neither OS does it well. Impossible OS can be first or best.
       - **Not applicable:** The feature doesn't fit Impossible OS's architecture or goals. Note why.

9. **Cross-TODO overlap check.**
   - Use Grep to search ALL domains for keywords from the TODO's section titles and deliverables.
   - For each match, classify the relationship:
     - **SUPERSEDES** — this TODO replaces the other section entirely.
     - **COMPLEMENT** — both contribute different aspects. Ensure bidirectional `→ XREF:`.
     - **CONFLICT** — both claim to implement the same thing. Resolve: pick one owner, add scope boundary note.
     - **FOUNDATION** — the other TODO provides infrastructure this one consumes. Add dependency XREF.
   - Check for stale sections in overlapping TODOs — if a section was made obsolete by recent work, add a supersession note.

### Phase 4 — Add Missing Sections

10. **Draft new sections for each significant gap.**
    - For each parity gap (💎): create a full section with:
      - Section heading: `## N. Feature Name`
      - One-line intro paragraph explaining what this feature does and why it matters.
      - Checklist items (`- [ ]`) with concrete deliverables. Each item must reference specific functions, types, or APIs. No vague "implement X" items.
      - `**Test checkpoint:**` block with concrete pass/fail criteria.
      - Commit message item.
    - For each competitive edge (⭐): same format, but add a `> [!TIP]` callout explaining why this is superior to Win11/Linux.
    - For Win11-only or Linux-only features: create sections only if they're important for compatibility or user experience. Otherwise note them as deferred with a reason.

11. **Renumber all sections.**
    - After inserting new sections, renumber ALL `## N. Title` headings sequentially from 1.
    - Group sections logically: foundational → parity → competitive edge.
    - Update all internal `§N` references in prose, callouts, and checklist items to match new numbers.

12. **Update the Implementation Order table.**
    - Add rows for every new section.
    - Mark each new row `💎` (parity) or `⭐` (exclusive).
    - Set `Depends On` correctly — new sections should depend on existing foundation sections where applicable.
    - Keep existing section dependencies correct after renumbering.
    - Renumber all Order values sequentially.

13. **Update the OS Comparison table.**
    - Add rows for each new feature discovered during research.
    - Use the compact format: `⭐ | Feature | Win11 | Linux | Impossible OS`
    - Keep cells short: status emoji + max 5 words.
    - Add the summary paragraph after the table noting competitive position.

14. **Update the Outcome section.**
    - Add bullet points for any new major deliverables introduced by the new sections.
    - Keep the Outcome section concise — one bullet per major capability, not per section.

15. **Update the Inputs section.**
    - If new sections reference source files not listed in Inputs, add them.
    - If new sections create dependencies on other TODOs, add `→ XREF:` lines.

### Phase 5 — Report

16. **Output a gap analysis report to the user.**
    - List every gap found, with its classification (parity/edge/deferred/N-A).
    - List every new section added, with section number and one-line summary.
    - List every cross-TODO overlap found, with relationship type and action taken.
    - List features explicitly deferred, with reasons.
    - Include the final section count and assertion count delta.

## Research Quality Standards

- **Minimum 6 web searches** per analysis (3 Win11 + 3 Linux). More for complex domains.
- **Follow at least 2 links** with WebFetch to get detailed feature descriptions, not just search summaries.
- **Cite sources** in the report — include URLs for key findings so the user can verify.
- **Distinguish fact from inference.** If a feature's existence is inferred from documentation rather than confirmed, note it as "likely" rather than "confirmed."
- **Current information only.** Search for 2025/2026 content to avoid citing deprecated features. Windows 11 24H2+ and Linux 6.x+ are the comparison baseline.

## Guardrails

- Do NOT implement kernel code — this skill creates TODO sections, not source files.
- Do NOT delete existing sections — only add new ones and renumber.
- Do NOT mark existing checklist items as done.
- Do NOT remove features from the TODO — only add missing ones.
- If a feature is out of scope for this TODO, note it as `→ XREF:` to the appropriate TODO or as explicitly deferred with a reason.
- Keep new sections lean and actionable. No multi-paragraph prose blocks — use checklist items with concrete deliverables.
- When research is ambiguous (e.g., Windows internal feature with no public docs), note the uncertainty rather than guessing.
