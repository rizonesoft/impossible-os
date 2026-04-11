---
name: review-pipeline
description: Full verify + quality review pipeline for one TODO section. Runs both reviews back-to-back with MANDATORY Codex adversarial dispatch at each stage. No skipping, no self-review shortcuts. Use when you want the complete audit in one pass.
---

# Review Pipeline

> **This is the "no shortcuts" skill.** It runs verify-todo-section then quality-review-section back-to-back on the same section, with MANDATORY Codex adversarial review at each stage. Every Codex dispatch is required -- no "Codex can't see the diff" or "already reviewed" exemptions.

## When to Use

- The user says "review this section" or "full review"
- Batch-reviewing multiple sections in a TODO file
- Any time you want both compliance + quality in one pass
- When you suspect you've been skipping Codex dispatches

## Pipeline Steps

### Phase 1: Verify (compliance audit -- "is it done?")

1. **Read section** -- full text, notes, XREFs. Inventory all `[x]` items.

2. **Explore codebase** -- Grep/Read every symbol. Build evidence map with file:line proof for each claim.

3. **Domain quality gates** -- walk the domain-appropriate code quality skill against the implementation:
   - `src/boot/` -> `boot-code-quality`
   - `src/kernel/`, `include/kernel/` -> `kernel-code-quality`
   - `src/desktop/` -> `desktop-code-quality`
   - `src/shell/` -> `shell-code-quality`
   - `user/`, `src/apps/` -> `userland-code-quality`

4. **Verify each `[x]` item** -- prove via evidence. Downgrade to `[/]` or `[ ]` on regression. Scope-gap audit for TODO/FIXME/HACK markers.

5. **Build** -- `bash scripts/build.sh`, confirm `=== BUILD OK ===`.

6. **MANDATORY Codex adversarial review** -- dispatch to Codex plugin. NO EXCEPTIONS. If Codex says "no diff available", provide file content in the prompt. If Codex returns a shallow response, re-prompt with specific adversarial angles.
   ```bash
   node "/home/derickpayne/.claude/plugins/marketplaces/openai-codex/plugins/codex/scripts/codex-companion.mjs" adversarial-review "<prompt with specific adversarial angles>"
   ```
   **Adversarial angles to ALWAYS include:**
   - Integer overflow / underflow in arithmetic
   - Buffer overread / overwrite on untrusted input
   - Missing NULL checks before dereference
   - SMP race conditions on shared mutable state
   - Error paths that leak resources (handles, memory, locks)
   - ABI mismatch between bootloader and kernel structs
   - Missing bounds checks on firmware-provided data

7. **Fix loop** -- apply `superpowers:receiving-code-review` to EVERY finding. Fix valid ones, reject with code evidence, accept out-of-scope with XREF. Rebuild after fixes.

8. **Reconcile + Verified stamp** -- if all items survived, add stamp. If downgrades, no stamp.

### Phase 2: Quality review (industry audit -- "is it done RIGHT?")

9. **Industry standards research** -- what does the relevant spec say? UEFI, ACPI, SMBIOS, ELF, Win32, x86-64 as applicable.

10. **Win11/Linux parity** -- how do Windows and Linux handle the same feature? Concrete function/file references, not vague claims.

11. **MANDATORY Codex performance + consistency review** -- dispatch to Codex plugin. NO EXCEPTIONS. Same rule as step 6 -- if no diff, provide file content.
    ```bash
    node "/home/derickpayne/.claude/plugins/marketplaces/openai-codex/plugins/codex/scripts/codex-companion.mjs" adversarial-review "<perf + consistency prompt>"
    ```
    **Performance angles to ALWAYS include:**
    - Allocations in hot paths
    - O(n^2) algorithms
    - Lock hold times
    - Byte-at-a-time operations where bulk exists
    
    **Consistency angles to ALWAYS include:**
    - Struct layout matches between header/impl/bootloader/test
    - Constants defined in one place
    - Unused functions/defines/types
    - Error code mapping consistency

12. **Fix loop** -- same discipline as step 7. Fix all valid findings. Rebuild.

13. **Quality stamp** -- add immediately after Verified stamp (no blank line). Include what was fixed and what was accepted with domain-qualified XREFs.

### Phase 3: Commit

14. **Commit and push** -- single commit with both stamps + any fixes.
    - If only stamps (no code changes): `"review: <TODO> §N -- verified + quality reviewed clean"`
    - If fixes applied: `"review: <TODO> §N -- <summary of fixes>"`
    - Push to `origin/main` immediately.

## What Makes This Different from Running Verify + Quality Separately

- **Codex is MANDATORY at both stages** -- no "already reviewed" or "no diff" exemptions
- **Single commit** for both stamps -- less commit noise
- **Adversarial angles are prescribed** -- no lazy "review this code" prompts
- **No self-review substitution** -- Codex must be dispatched even for "simple" code

## Guardrails

- Both Codex dispatches MUST happen before commit. If either is skipped, the pipeline is invalid.
- Apply `superpowers:receiving-code-review` to every finding from both dispatches.
- Do not weaken tests to make findings go away.
- Do not skip Phase 2 because Phase 1 was clean. Clean compliance doesn't mean clean quality.
- All stamps use domain-qualified XREFs in Accepted fields.
- No blank line between Verified and Quality reviewed stamps.
