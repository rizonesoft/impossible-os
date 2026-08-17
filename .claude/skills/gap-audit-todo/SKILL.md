---
name: gap-audit-todo
description: Deep gap audit of a TODO file against all overlapping TODOs -- find scope conflicts, stale sections, missing coverage, false completeness, and unclear ownership. Mandatory secondary Codex pass via codex-gap-audit red-teams the inventory before the result is accepted. Use after creating or majorly editing a TODO, or when planning cross-domain work.
---

# Gap Audit TODO

> **External-Reviewer Contract:** This skill dispatches Codex through its mandatory Phase 3.5 `codex-gap-audit` pass. Every finding goes through `superpowers:receiving-code-review` (verify at file:line or source section, Fix / Reject / Accept, never blind-implement). Canonical contract: [docs/infrastructure/ai-system.md#external-reviewer-contract-codex](../../../docs/infrastructure/ai-system.md#external-reviewer-contract-codex).

## Execution Discipline

> Gap analysis is not a formatting pass and not a parity-only pass. The job is to make the TODO competitively complete, execution-ready, and resistant to paper completion.
> - **Completion-first.** A section is not "good enough" just because the happy path is listed. If nearby work is required for the feature to feel real, wired, and credible, the TODO must name that work or explicitly file the owner elsewhere.
> - **No ownerless gaps.** Every deferred or adjacent gap must end with a concrete owner section or TODO XREF.
> - **Think beyond parity.** Win11/Linux parity is the floor. Competitive edge and refinement opportunities belong in the TODO when they are feasible and relevant.

## Vendor-vs-build gap

A gap audit asks what is MISSING. It must also ask what is being BUILT that need not be. For each subsystem this TODO implements from scratch, check whether a mature, license-compatible upstream exists, and flag it when one does.

- License first, and it disqualifies more than it admits: this project is GPL-3.0-only, so **GPL-2.0-only upstreams are a hard stop** (the Linux kernel and its drivers, NTFS-3G, lwext4). Read the upstream LICENSE file; a README or a wiki page is not evidence.
- Compatible and already proven in this tree: ACPICA (ACPI/AML), Mbed TLS (crypto/TLS), Monocypher, LZ4, miniz, cJSON. Compatible and verified but not yet adopted: lwIP (BSD-3, TCP/IP), HarfBuzz (MIT, text shaping), FatFs (permissive, FAT/exFAT), litehtml (BSD-3, HTML/CSS layout), EDK2 (BSD-2-Clause-Patent, used as an oracle rather than vendored).
- A finding here is "this section reimplements X, which <upstream> provides under <license>", with the upstream named and its license verified. It is NOT an instruction to rewrite a shipped section; existing work stays unless the section is still open.
- The reverse finding counts too: a plan that says "port <upstream>" where that upstream is GPL-2.0-only is a licensing defect, and a more urgent one than a missing feature.


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
6. **Define the completion boundary for this TODO.**
   - What would make this feature feel real to a user, caller, or subsystem consumer?
   - What adjacent capabilities, wiring steps, exports, tests, or follow-up ownership would obviously still be missing if the current sections were implemented literally?
   - Keep this boundary in view for the rest of the analysis. The goal is not "does the TODO mention something"; the goal is "would implementing this TODO produce a credible result?"

### Phase 2 -- Internet Research (critical -- do not skip)

**Phase 2.0 -- Maturity gate (set research DEPTH before you dispatch).** Deep parity research is the right tool for a greenfield or partially-built TODO; it is the WRONG tool for one already shipped and validated, where an unbounded survey just burns tokens chasing tangents. (Incident 2026-06-21: a parity dispatch on a mature kernel-libraries TODO ran 90+ web fetches into network-stack hash-table history before it was stopped.) Classify this TODO from concrete signals BEFORE choosing a dispatch:

- **MATURE / multiply-validated** = every `## N.` section's Implementation Order row is `[x]` DONE (or the only open rows are `[/]` + Deferred-stamped), AND the file carries a recent close-for-pass / completion run-log entry or sits in documentation/maintenance state.
- **GREENFIELD / partial** = anything else (open `[ ]` sections, no close-out, active build-out).

Then choose the dispatch by verdict:

- **GREENFIELD / partial -> FULL research** (the blockquote below): the `parity-research-analyst` todo-plan dispatch performing steps 7-11 as written.
- **MATURE -> CONFIRMATORY only.** Do NOT run the full Win11 / Linux / emerging triple-survey. Either spot-check inline (a few targeted lookups confirming no NEW baseline capability has appeared since the TODO closed), or dispatch the agent with `[maturity: mature -- confirmatory-only, cap ~6 lookups]` leading the prompt so its mature-bias research budget binds. The goal is to catch a genuinely new gap, not to re-derive an inventory the closed sections already cover. If the confirmatory pass surfaces nothing new, record "mature TODO, confirmatory parity pass, no new baseline gaps" in the Phase 5 report and proceed -- a near-empty parity result on a closed TODO is the expected, correct outcome, not a reason to widen the search.

> **Delegate the research + inventory to a read-only agent (the biggest token + context offload in the pipeline).** Dispatch `Agent(subagent_type="parity-research-analyst", ...)` in **todo-plan mode** to perform steps 7-11 (Win11 / Linux / emerging research + merged feature inventory + gap classification + compare-against-current-sections) in a separate Sonnet context, returning the structured inventory + coverage-gap list + sources. (Sonnet is the frontmatter default; gap mode is backstopped by the MANDATORY Phase 3.5 `codex-gap-audit` red-team.) You (main session) then do step 12 (cross-TODO overlap), apply all resulting TODO edits, and run Phase 3.5 -- the agent never edits the TODO. If you prefer to run the research inline, follow steps 7-11 directly instead.

7. **Research Windows 11 features in this domain.**
   - Use WebSearch with queries like: `"Windows 11 <topic> internals"`, `"Windows NT <topic> architecture"`, `"Win32 <topic> API"`, `"Windows <topic> features 2025 2026"`.
   - Run at least 3 targeted searches. Follow promising results with WebFetch to read documentation pages, blog posts, and Microsoft Learn articles.
   - Build a feature inventory: list every distinct feature, API, behavior, or capability that Win11 provides in this domain.
   - Note implementation details that affect how Impossible OS should implement the same feature.
   - **Save URLs** for key findings -- cite them in the final **report to the user** (schema item 22), not in the TODO file.

8. **Research Linux features in this domain.**
   - Use WebSearch with queries like: `"Linux kernel <topic>"`, `"Linux <topic> subsystem"`, `"Linux <topic> implementation"`, `"Linux <topic> features 2025 2026"`.
   - Run at least 3 targeted searches. Follow promising results with WebFetch.
   - Build a feature inventory for Linux -- same level of detail as Win11.
   - Note where Linux does something differently or better than Windows.
   - **Save URLs.**

9. **Research emerging and state-of-art features.**
    - Use WebSearch for: `"modern OS <topic> best practices"`, `"<topic> innovations operating system"`, `"<topic> security hardening OS"`.
    - Look for features that NEITHER Win11 nor Linux implements well -- these are competitive edge opportunities for Impossible OS.
    - Look for common pain points developers/users have with Win11 and Linux in this domain.
    - Build a **refinement inventory**: cleaner APIs, better defaults, reduced wiring friction, stronger safety checks, simpler deployment, better observability, or less legacy complexity than either baseline.

### Phase 3 -- Feature Inventory and Gap Detection

10. **Compile the complete feature inventory.**
    - Merge all research into a single list organized by category.
    - For each feature, record: feature name, Win11 status (✅/⚠️/❌), Linux status (✅/⚠️/❌), and a one-line description.
    - Also record whether the feature is:
      - **core parity**
      - **adjacent completeness**
      - **wiring/integration**
      - **competitive refinement**

11. **Compare inventory against the TODO's current sections.**
     - For each feature in the inventory:
       - If covered by an existing section: mark as covered, verify the section is adequate (not just a stub).
       - If covered by a sibling TODO: note with `-> XREF:` and mark as covered-elsewhere.
       - If NOT covered anywhere: this is a **gap**. Classify it:
        - **Parity gap (💎):** Both Win11 and Linux have it. Impossible OS MUST implement it.
        - **Win11-only gap:** Only Win11 has it. Evaluate whether it's important for Win32 API compatibility.
        - **Linux-only gap:** Only Linux has it. Evaluate whether it's relevant for Impossible OS's target audience.
         - **Competitive edge (⭐):** Neither OS does it well. Impossible OS can be first or best.
         - **False-completeness gap:** The TODO lists the happy-path work, but still omits an adjacent piece needed for the feature to feel real, integrated, or properly owned.
         - **Wiring gap:** Implementation work is listed, but exports, registrations, tables, tests, docs, or reciprocal XREFs are missing.
         - **Refinement gap:** The TODO reaches parity but misses an obvious cleaner/faster/safer/more elegant design the project should pursue.
         - **Not applicable:** The feature doesn't fit Impossible OS's architecture or goals. Note why.
         - **Blocked by gotcha:** The feature hits a documented CLAUDE.md bare-metal gotcha. Note the blocker.

12. **Cross-TODO overlap check.**
    - Use Grep to search ALL domains for keywords from the TODO's section titles and deliverables.
    - **Query wiring:** `python3 scripts/todo-graph/query.py backlinks <id>` enumerates inbound XREFs programmatically (depends_on, satisfies, Inputs XREF, Accepted/Deferred stamps, Implementation Order dep groups). Use this to short-circuit the keyword grep when the target already carries a stable slug (pre-§5: filename slug; post-§5: frontmatter id).
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

13. **Code-truth completion audit (required, full-file).**
     - Scan EVERY checklist item in the target TODO (`[x]` and `[ ]`) against the live codebase.
     - Never trust checkbox state alone; validate by evidence (symbols, call sites, wiring/registration paths, normal-path behavior).
     - Treat a claim as complete only when implemented and wired. Stubs/placeholders/`STATUS_NOT_IMPLEMENTED` normal paths are not complete.
     - Apply the **completion radar** to the TODO itself:
       1. **Correctness:** does the plan cover normal and failure paths?
       2. **Completeness:** what nearby piece is still missing for the feature to feel real?
       3. **Wiring:** exports, registrations, tables, tests, docs, TODO/XREF sync?
       4. **Parity:** what does Win11/Linux do here that the TODO still does not cover?
       5. **Superiority:** what cleaner, faster, safer, or more elegant design should Impossible OS claim?
       6. **Ownership:** if any work is deferred, where is the exact tracked item that closes it?
     - If a section is marked complete but has gaps, add missing unchecked items in that same section in prerequisite-first logical order.
     - **Strict evidence gate (non-optional):** every status change or newly added TODO item must cite proof in `path:line + symbol + reason` form.
     - Required evidence classes for completion claims:
       1. **Implementation evidence:** concrete symbol/body exists in non-test code.
       2. **Wiring evidence:** registration/dispatch/call path reaches the implementation.
       3. **Behavior evidence:** normal path is not stub/placeholder/`STATUS_NOT_IMPLEMENTED`.
       4. **Ownership evidence:** if work is deferred, a concrete owner section/TODO item exists with reciprocal XREF.
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

14. **Audit section sizes.** For each existing section, count checklist items. Flag any section with > 10 items -- split into **two new top-level `## N.` sections** (renumber), not into `N.M` sublabels inside one section.

### Phase 3.5 -- Codex Gap Audit (MANDATORY -- NO EXCEPTIONS)

14.5. **Dispatch `codex-gap-audit`** to red-team the gap inventory before Phase 4 turns it into TODO edits. This catches Win11/Linux features the agent missed, parity claims that don't hold under scrutiny, "covered elsewhere" XREFs that point at items which won't actually close the gap, ownerless adjacent work, and refinement opportunities the research phase glossed over.

   ```bash
   bash scripts/codex-dispatch.sh '[review-kind: gap-audit] <todo-path> <prompt with feature inventory + classified gaps>'
   ```

   Follow the [`codex-gap-audit`](../codex-gap-audit/SKILL.md) skill for the prompt template. The dispatch is mandatory in every gap-audit run -- planning text is exactly the surface where blind spots calcify into "shipped" before code starts. Skip ONLY when the gap analysis is a docs-only sweep with zero new sections / ownership changes / parity claims (rare); document the skip in the Phase 5 report.

   For each finding:
   - **Verify against actual Win11/Linux source-of-truth** (Microsoft Learn, kernel.org docs, official spec). Codex sometimes hallucinates feature names or attributes them to the wrong OS; the receiving rule (Fix / Reject with evidence / Accept-XREF) applies.
   - **Cross-TODO duplicate check (MANDATORY before adopting any finding as a NEW section/item).** Before turning a finding into a new `## N.` section or a new `[ ]` item in this TODO, grep across `todo/` for the feature name AND any obvious synonyms / variant phrasings: `mcp__todo-graph__by-domain`, `mcp__todo-graph__code <symbol>`, plus a plain `Bash(grep -rni "<feature>" todo/)`. Read every hit. If the gap is already owned by another TODO section: do NOT create a duplicate -- instead retarget as an Accepted-XREF (with the concrete `[ ]` item name from the existing section) or, if the existing item is too vague to actually close the gap, sharpen that target item in place rather than spawning a new one. Same rule for refinement / competitive-edge findings: scan for prior decisions in adjacent TODOs, `docs/infrastructure/`, and `CLAUDE.md` before claiming "we should do X" -- the project may have already evaluated and rejected X. **Why this step exists:** Codex audits one TODO at a time; cross-TODO ownership is the agent's job. A finding that's right "in isolation" but ends up duplicating an existing section is worse than a missed finding -- it forks ownership and breaks the single-source-of-truth contract.
   - **Valid finding (after duplicate check) -> fold into the inventory** before Phase 4: add the missed feature, correct the misclassified gap, retarget the dead-end XREF at a concrete `[ ]` item.
   - **Reject false positive -> note in chat with citation** (the same kind of file:line evidence the code-review reception requires).
   - **Out-of-scope -> Accept with concrete XREF** to the owner section/TODO, never a section title alone.

   Receiving the response goes through `superpowers:receiving-code-review` like every other Codex dispatch.

### Phase 3.6 -- Filing Triage Gate (MANDATORY -- runs BEFORE any TODO edit)

> **Why this gate exists:** the most common gap-audit failure mode is shipping the right findings into the wrong shape. Specifically:
> 1. A finding that is a substantive new feature gets appended as a `[ ]` sub-bullet under a closed `[x]` section, bloating that section's checklist and breaking the "closed sections are stable" contract.
> 2. A finding that is already owned by another TODO/section gets re-filed here as a fresh `[ ]` instead of cross-referenced, creating duplicate ownership and drift potential.
>
> Both failures look like progress in the moment and only surface as drift weeks later. This gate makes the filing decision an explicit, classified step before Phase 4 runs.

14.6. **Classify EVERY accepted finding into one of four branches before the Phase 4 edit pass.** No finding may bypass this gate; the classification is an entry in the gap-analysis report (schema item 14, added below) and drives Phase 4 placement.

   Branches (mirrors the [scope-gap protocol](../implement-todo-section/scope-gap-protocol.md) used by `/implement-todo-section`):

   - **Branch A -- Inline expansion in an OPEN section (`[/]` or `[ ]`).** The finding is small, same-subsystem, fits naturally inside a section that's still in progress. Add as a `- [ ]` checklist item with concrete file:function evidence. **Forbidden when the target section's IO row is `[x]`** -- a closed section's checklist must not grow new mandatory work; if you're tempted to file in a closed section, the finding is wrong-shape and belongs in B / C / D instead. Adding a follow-up `[ ]` to a closed section silently downgrades the IO row to `[/]` (per Phase 4 step 17 status-consistency rule), which usually surprises the agent that closed it.
   - **Branch B -- New top-level `## N.` section in THIS TODO.** Default for non-trivial new features that fit the TODO's scope. Add a full section with intro paragraph, checklist, Test checkpoint, Test runner, and an Implementation Order row. **This is the default for findings that name a substantive capability, a new subsystem boundary, or work that needs its own commit message**; do not collapse such findings into Branch A bullets even when a closed section's topic is "adjacent."
   - **Branch C -- Existing concrete `[ ]` item in another TODO (XREF only).** The finding is already owned. Add a `-> XREF:` cross-reference (or, if the target item is too vague to actually close the gap, sharpen its wording in place) and reciprocal back-link in the OWNER TODO. Never spawn a duplicate `[ ]` here. **Both directions of the XREF must land in the same edit pass** (per Phase 3 step 12 reciprocal-patch rule).
   - **Branch D -- New TODO file in the appropriate domain.** The finding is a substantive scope this TODO does not own and no existing TODO covers. Use `/create-todo` (or document the recommendation for the user) before filing the gap.

   **Cross-TODO duplicate sweep is a HARD precondition for Branch B / D.** Step 14.5 already mandates the sweep; this step formalizes it as a gate -- if the sweep finds an existing owner, the finding becomes Branch C and Branch B / D are forbidden. Do NOT rely on the gut feel that "it would be cleaner to own this here." The sweep query set is fixed:

   1. `mcp__todo-graph__code <symbol>` -- LSP-grade by-symbol lookup (only when the finding names a concrete C symbol, file path, or struct).
   2. `mcp__todo-graph__by-domain <domain>` -- enumerate every TODO in the affected domain and its siblings.
   3. `Bash(grep -rni "<feature>" todo/)` -- prose / synonym hits across the whole TODO tree. Try at least 2 phrasings per finding (the registered name plus an obvious synonym).

   If ANY of the three queries returns a hit that names the same capability, the finding routes to Branch C. If you choose to override the sweep (rare; finding genuinely needs a new owner despite an existing partial item), state the override in chat with the file:line of the existing item AND why retargeting / sharpening would not close the gap.

   **Output of this gate:** a per-finding classification table the report's schema item 14 will record. Until every accepted finding has a Branch A/B/C/D label AND, for B/D, evidence that the duplicate sweep was empty, the agent does NOT begin Phase 4.

### Phase 4 -- Add Missing Sections

15. **Draft new sections or owner items for each significant gap.**
     - For each parity gap (💎): create a full section with:
       - Section heading: `## N. Feature Name`
       - One-line intro paragraph explaining what this feature does and why it matters.
       - Checklist items (`- [ ]`) with concrete deliverables (max 8-10 per section). Each item must reference specific functions, types, or APIs. No vague "implement X" items.
      - **No N.M subnumbering:** never add `### N.M`, `**N.M Title**`, or split the work into `17.1` / `17.2` style blocks. One `## N.` section = one continuous `- [ ]` list (nested detail bullets under a checkbox allowed). End with `- [ ] Commit:` as the last checklist line, then `**Test checkpoint:**` last in the section.
      - `**Test checkpoint:**` block with concrete pass/fail criteria and platforms (QEMU WHPX + TCG; bare metal). Mention POST16 codes ONLY if the section is boot-path (Phase 0/1/2); for post-boot sections use a `klog(LOG_INFO, ...)` line as the observable instead.
      - Commit message item.
      - For BOOT-PATH sections (Phase 0/1/2 init, hardware bring-up, page tables, GDT/IDT, APIC, ACPI, SMP AP startup): `**Regression risk:**` note and `POST16(0xDDNN)` codes (check `boot_init.h` for conflicts).
       - For POST-BOOT sections (scheduler, syscall, exec, file I/O, IPC, network, runtime drivers): NO POST16. klog is fully working at this point. POST16 was designed for pre-`sti` triple-fault diagnostics; using it post-boot is cargo-culted noise.
       - For high-risk sections: `> [!WARNING]` callout noting SMP safety or bare-metal concerns.
     - For each competitive edge (⭐): same format, but add a `> [!TIP]` callout explaining why this is superior to Win11/Linux.
     - For each false-completeness, wiring, or refinement gap, follow the Phase 3.6 Branch classification:
       - **Branch A only:** the target section's IO row is `[/]` or `[ ]` AND the gap fits naturally inside its checklist. **NEVER append a follow-up bullet to a section whose IO row is `[x]`** -- that bloats closed sections and silently re-opens them. If the natural-target section is closed, the finding is wrong-shape: classify as Branch B (new section in this TODO) or Branch C (XREF in another TODO).
       - **Branch B only:** new top-level `## N.` section in THIS TODO (full intro + checklist + Test checkpoint + Test runner + IO row). This is the default for non-trivial findings -- if you find yourself drafting a paragraph-long Branch A bullet, the finding has already grown past "inline expansion" and needs Branch B.
       - **Branch C only:** the duplicate sweep in Phase 3.6 found an existing concrete `[ ]` item that owns the gap. Add reciprocal `-> XREF:` cross-refs (or sharpen the target item's wording). Do NOT spawn a duplicate `[ ]` here.
       - **Branch D only:** the gap belongs to a domain this TODO does not own AND no existing TODO covers it. Use `/create-todo` (or document the recommendation in the report) before filing.
       - **Forbidden:** "If it fits the current TODO's natural scope, add a checklist item or a new section now" was the prior wording; that ambiguity caused most of the Branch A misuses. The Branch A vs B decision is now bound by the Phase 3.6 classification, not by gut feel.
     - For Win11-only or Linux-only features: create sections only if they're important for compatibility or user experience. Otherwise note them as deferred with a reason.

16. **Renumber all sections.**
    - After inserting new sections, renumber ALL `## N. Title` headings sequentially from 1.
    - Group sections logically: foundational -> parity -> competitive edge.
    - Update all internal `§N` references in prose, callouts, and checklist items to match new numbers.

17. **Update the Implementation Order table.**
    - Add rows for every new section.
    - Mark each new row `💎` (parity) or `⭐` (exclusive).
    - Set `Depends On` correctly -- new sections should depend on existing foundation sections where applicable.
    - Keep existing section dependencies correct after renumbering.
    - Renumber all Order values sequentially.
      - **Status consistency rule:** if a section has unresolved required checklist items or newly discovered prerequisite gaps, its Implementation Order status cannot stay `[x]`; downgrade to `[/]` (or `[ ]` when appropriate).
      - If a section depends on adjacent completeness work or owner work newly added in this analysis, its status cannot stay "done on paper."

18. **Update the OS Comparison table.**
    - Add rows for each new feature discovered during research.
    - Use the standard five-column layout with icon labels.
    - Header format: `⭐ | Feature | 🪟 Win11 | 🐧 Linux | 🚀 Impossible OS`
    - Keep cells short: status emoji + max 5 words.
    - **Source alignment:** pad columns so pipes line up in the raw file; separator row uses `-` per column (min 3) matching column widths. If you edit one row, re-pad the entire table. Do not strip trailing spaces to shorten lines.
    - Add the summary paragraph after the table noting competitive position.
    - **Do not** add `<!-- Sources: ... -->` HTML comments to the TODO. Put research URLs in the **gap-analysis report to the user** (report schema item 22), not in tracked markdown.

19. **Update the Unit Tests section.**
     - Add test cases for new sections' deliverables.
     - Check if a new `TEST_CAT_*` category is needed for the new functionality. If so, note it.
     - Reserve POST16 debug codes ONLY for new boot-path sections (check `boot_init.h` for conflicts). Do not generate "test POST16 constant equals 0xDDNN" cases -- those are tautological (the compiler enforces the literal, the boot-time uniqueness check is the real protection).
     - If a feature is deferred, ensure the Unit Tests section reflects whether it should land as a real assertion later or as a temporary `TEST_PENDING` shape once the stub exists.

20. **Update the Outcome section.**
     - Add bullet points for any new major deliverables introduced by the new sections.
     - Keep the Outcome section concise -- one bullet per major capability, not per section.

21. **Update the Inputs section.**
     - If new sections reference source files not listed in Inputs, add them.
     - If new sections create dependencies on other TODOs, add `-> XREF:` lines (Inputs level = structural).
     - For missing items added into existing sections, add/verify ownership and prerequisite XREF entries as well.
     - For each new external dependency, patch bidirectional awareness immediately: add the missing back-reference and mirrored unchecked item to the target TODO section in the same run.

### Phase 5 -- Report

22. **Output a gap analysis report to the user.**
     - Use this fixed schema (always):
       1. **Demoted items** (previously `[x]`, now `[/]` or `[ ]` -- with evidence for each demotion)
       2. **Implemented but not marked** (with evidence)
       3. **Marked but not implemented/wired** (with evidence)
       4. **Missing prerequisites inserted** (ordered list + ownership + XREF)
       5. **Cross-TODO ownership decisions** (SUPERSEDES/COMPLEMENT/CONFLICT/FOUNDATION)
       6. **False-completeness and wiring gaps** (fact vs inference labeled)
       7. **Parity/edge/refinement gaps** (fact vs inference labeled)
       8. **Oversized sections flagged** (> 10 items, recommend split)
       9. **Bare-metal gotcha blockers** (features blocked by documented CLAUDE.md gotchas)
       10. **Edits summary** (sections touched, status downgrades, section/order deltas)
       11. **Cross-file patches applied** (target file, section, inserted/downgraded items, reason)
       12. **Auto-closure decisions** (source ID, target ID, closure result, proof or missing criteria)
       13. **Estimated scope** (new sections added, estimated complexity: low/medium/high, test categories needed)
       14. **Filing triage table** (Phase 3.6 output): one row per accepted finding with columns `finding`, `branch` (A/B/C/D), `target` (section number for A/B, TODO+section for C, new TODO name for D), `duplicate-sweep` (queries run + result, OR "n/a" for Branch A/C). A Branch B / D row without a "duplicate-sweep: empty" entry is invalid; surface it as a defect in the report.
     - Every finding line must include a confidence tag: `confirmed` or `inferred`.
     - Include the final section count and checklist delta.

23. **Do NOT add a `## History` section or row.** Gap-analysis activity is already captured by the written report (step 22), the git commit, and any section stamps that get added. A separate History table just duplicates that trail and grows without bound across review passes. Leave existing History sections alone (don't delete prior entries) but don't append new ones.

24. **Write the file-level Gap-audited stamp (lifecycle marker).** Once Phase 4 filing and the mandatory `codex-gap-audit` pass are complete, write/refresh the file-preamble stamp immediately under the H1 and above `> **Goal:**`:
    `> **Gap-audited:** YYYY-MM-DD | gap-audit + codex-gap-audit; N findings filed (or none)`.
    For a Phase 2.0 confirmatory pass on a mature TODO, the basis line says so (e.g. "confirmatory pass, no new baseline gaps"). One stamp line, not a History block; refresh the date each run. The oracle `.claude/hooks/sequencer_triage.py` reads this stamp as the other half of `stages_1_2_done`, and Stage 0 of the per-file pipeline uses both to skip a redundant re-audit.

## Research Quality Standards

- **Depth follows the Phase 2.0 maturity gate, within its ~15-lookup budget.** For a FULL (greenfield / partial) analysis, aim for roughly 3 targeted searches each across Win11 / Linux / emerging. A MATURE-TODO confirmatory pass is explicitly exempt from any search floor -- a handful of targeted lookups is the correct depth, not a quota to clear.
- **Follow at least 2 links** with WebFetch to get detailed feature descriptions, not just search summaries.
- **Save source URLs** for the written report (schema item 22) -- not as `<!-- Sources: ... -->` in the TODO file.
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
- Do NOT leave paper completion in place. If a section would still feel obviously incomplete after literal implementation, add the adjacent work or file the exact owner now.
- Do NOT leave deferred work ownerless. Every deferral needs a concrete checklist item in the owning section or TODO.
- Keep new sections lean and actionable. No multi-paragraph prose blocks -- use checklist items with concrete deliverables.
- Max 8-10 checklist items per new section. Split if larger.
- When research is ambiguous (e.g., Windows internal feature with no public docs), note the uncertainty rather than guessing.
- Always scan the codebase for completion claims; TODO text is never source-of-truth by itself.
- Do not change visual formatting conventions (icons/table headers/callout style) unless the user explicitly asks for format changes.
- Do not leave a section marked complete when newly found prerequisite gaps remain unresolved.
- If confidence is below strict-proof threshold, choose conservative output: keep unchecked and add a concrete follow-up item with owner/XREF.
