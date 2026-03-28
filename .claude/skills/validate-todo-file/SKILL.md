---
name: validate-todo-file
description: Validate a TODO file for structural completeness, Implementation Order accuracy, XREF continuity, cross-TODO scope overlap, Inputs path existence, and gap-free execution coverage without doing code-completion audits. Use when reviewing a TODO after creation or major edits, or before implementation begins.
---

# Validate TODO File

## Workflow

1. Read the full TODO file, not just a selected section.
2. Clean up unnecessary manual line breaks throughout the file.
   - **Before touching anything**, run Grep to find all continuation lines:
     ```
     rg '^ {2,}[a-zA-Z`'"'"'"]' <file>
     ```
     Fix **every** match before moving on. Re-run after fixes — proceed only when it returns zero matches.
   - **List items** (`- [ ]`, `- [x]`, `- bullet`): remove blank lines between consecutive items in the same group.
   - **Prose blocks** (Goal block, Prompt text, XREF notes, callout bodies): join hard-wrapped mid-sentence line breaks into a single flowing line.
   - **Callout blocks** (`> [!NOTE]`, `> [!IMPORTANT]`, etc.): no blank lines inside a single callout.
   - **Tables**: no blank lines between rows.
   - **Preserve** blank lines between: section headings and content, distinct list groups, `---` separators, and code block fences.
3. **Remove model tags from section headings.**
   - Strip `` `[Opus]` `` and `` `[Sonnet]` `` postfixes from all `## N. Title` headings.
   - These tags are legacy — model selection is handled by the harness, not the TODO.
4. Anchor-check the Inputs section.
   - For each file path listed in Inputs, use Glob to confirm it exists on disk.
   - Flag any path that does not exist as a broken anchor; suggest the correct path or note it as planned.
   - Do not read the files — existence check only.
5. Read the domain `INDEX.md` and all other TODO files in the same domain folder.
   - Use Grep to search for deliverable names and feature keywords across domain TODOs to detect scope overlap.
   - Scan for duplicate XREFs pointing at the same target section — two TODOs depending on the same section is fine; two TODOs both *implementing* it is a conflict.
6. Validate the current lean TODO structure.
   - Check required sections, numbering, checklist shape, references, and exit criteria.
   - Use `Implementation Order`, not legacy phase-table rules.
   - **Compact the OS Comparison table:**
     - Header columns: `⭐ | Feature | Win11 | Linux | Impossible OS`
     - Keep cells short: status emoji + max 5 words per cell. No full sentences.
     - Pad columns so pipe characters align vertically within the table.
     - If the table is wider than ~100 characters per row, shorten cell text further.
   - **Parity gap check:** Scan the OS Comparison table for features where BOTH Win11 and Linux show ✅ but Impossible OS shows ⬜ or is missing entirely. These are parity gaps — features competitors have that we don't. For each gap:
     - If covered by a section in this TODO: verify the section exists and is actionable (not deferred indefinitely).
     - If NOT covered by any section: flag it as a missing parity feature. Suggest adding a section or noting it as deferred with a reason.
     - If covered by another TODO: add a `→ XREF:` and note it in the table.
   - **Competitive edge check:** Look for features where Impossible OS could be BETTER than both competitors (⭐ exclusive). Research what Win11 and Linux do poorly in this TODO's domain and suggest exclusive features that would make Impossible OS superior. Add suggested rows to the table marked ⭐ with ⬜ Planned.
7. Validate execution coverage.
   - Check dependency order, `→ XREF:` lines, overlap notes, handoffs, and adjacent-file continuity.
   - For each `→ XREF: TODO-XX §N`, confirm the target TODO file exists **and** the referenced section number is present in that file.
   - Check handoff boundaries: for each deliverable this TODO hands off to another, confirm the receiving TODO has a matching Inputs or XREF entry.
   - Fix stale planning text, broken links, and roadmap inconsistencies.
8. **Self-contained execution check (critical).**
   - The TODO must be executable from §1 to the last section WITHOUT being blocked by unimplemented sections in other TODOs.
   - For each `Depends On` entry in the Implementation Order table that references an EXTERNAL TODO (not a section within this file):
     1. Check if that external section is already implemented (`[x]`). If yes, no action needed.
     2. If NOT implemented (`[ ]`): the TODO is **blocked**. Fix it by adding a new section to THIS TODO that implements the minimal prerequisite — just enough to unblock the dependent section, not the full scope of the other TODO.
     3. The new section should be clearly marked: `> [!NOTE] Minimal prerequisite — full implementation in TODO-XX §N`
     4. Update the Implementation Order to reference the new local section instead of the external one.
   - **Principle:** When you follow a TODO from §1 to the last section, you must have a fully working base system at the end. External TODOs enhance it later, but never block it.
   - Flag any section where the `Depends On` column references something that doesn't exist yet and no local fallback is provided.
9. **Prerequisite code audit.**
   - For each function, type, or API referenced in checklist items (e.g., `vmm_map_mmio()`, `BOOT_TRY()`, `POST16()`), use Grep to check if it exists in the codebase.
   - If it doesn't exist AND is not created by a prior section in this TODO: flag it as a missing prerequisite.
   - If it IS created by a prior section: verify the section order puts the creator before the consumer.
10. **Platform coverage check (bare metal first).**
    - **Bare metal is the acceptance criteria.** "Verified on QEMU" is necessary but NOT sufficient. Every section that touches hardware must include bare-metal verification.
    - Every TODO that touches hardware, interrupts, page tables, timers, or drivers MUST list which platforms each section has been verified on.
    - Required platforms: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.
    - If the Test Checkpoint only mentions VMs, flag it and add: "Verify on bare metal — VM behavior differs."
    - When writing hardware-touching code, ask: "does this work without a hypervisor?"
11. **Regression risk scan.**
    - For each section, identify what existing working functionality it could break.
    - Flag high-risk sections — those that touch:
      - **Interrupt path** (IDT, ISR stubs, LAPIC, IOAPIC, EOI) — one wrong bit = triple fault
      - **Page tables** (PTE flags, CR3, TLB flush) — breaks all memory access
      - **GDT/TSS** (segment selectors, RSP0, IST) — breaks privilege transitions
      - **Timer** (LAPIC timer, PIT, calibration) — breaks scheduler, compositor, sleep
      - **Boot order** (Phase 0/1/2/3 sequencing) — breaks everything downstream
    - For each high-risk section, require:
      - A rollback note: "If this breaks, revert [specific change] and fall back to [known-good behavior]"
      - An incremental test: verify the change works BEFORE proceeding to the next section
12. **Boot-path impact analysis.**
    - Trace the boot execution path from `kernel_main()` through Phase 0 → 1 → 2 → 3 → compositor.
    - For each section in the TODO, identify exactly WHERE in the boot path its changes take effect.
    - Flag any section that modifies code running BEFORE `sti` (interrupts enabled) — these are the most dangerous because errors cause silent triple faults with no diagnostic output.
    - Flag any section that modifies the interrupt handler path — errors here crash on every interrupt, not just the subsystem being changed.
13. **Test checkpoint enforcement.**
    - Every section MUST have a `**Test checkpoint:**` block at the end.
    - Each checkpoint must have concrete pass/fail criteria — not "verify it works" but specific observable outcomes:
      - Serial log output: exact string to grep for
      - POST code: exact hex value
      - Screen output: what the user should see
      - Behavior: what happens when X is triggered
    - Each checkpoint must specify which platforms to test on.
    - Flag any section missing a test checkpoint.
    - Flag any checkpoint with vague criteria ("should work", "verify correct behavior").
14. If the problem is code-truth or completion-state accuracy, hand off to `/verify-todo-section` instead.

## Guardrails

- Do not implement code.
- Do not mark TODO work done based on code inspection alone.
- Do not turn this into a formatting-only cleanup pass; structural clarity is the goal.
- Inputs path checks are existence-only — do not read or analyse the referenced source files.

