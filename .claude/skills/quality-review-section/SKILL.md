---
name: quality-review-section
description: Deep quality review of an implemented TODO section -- industry standards compliance, optimization opportunities, architectural fitness, and Win11/Linux parity analysis. Unlike verify-todo-section (compliance audit), this skill asks "is it done RIGHT?" not just "is it done?" Use after verification passes, or when you want to improve an already-working section.
---

# Quality Review Section

## Execution Discipline

> This is a QUALITY audit, not a compliance audit. The section already works and is verified. The question is: does it match industry standards, is it optimally implemented, and could it be done better?
> - **Do not downgrade checklist items.** This is not verify-mode. All items stay as-is.
> - **Apply `superpowers:receiving-code-review` to every finding.** Verify before acting -- reject wrong findings with evidence.
> - **Fix all valid findings.** Spec violations, best practices, dead code, parity gaps -- if it's real, fix it. Only Accept findings that are wrong, irrelevant, or need missing infrastructure.
> - **One section = one commit.** All fixes committed together.

## Use This Skill When

- A section passed `/verify-todo-section` and the user wants deeper quality analysis.
- The user asks "can this be done better?", "is this production quality?", or "how does this compare to Windows/Linux?"
- Before bare-metal testing on new hardware -- catch spec violations early.
- After a section has been stable for a while and is ready for hardening.

## Workflow

1. **Read the section + source files** -- full section text, all source files that implement it. Understand not just WHAT it does but HOW it does it.

2. **Industry standards research** -- for each major feature in the section, research how it SHOULD work per the relevant specification:
   - **UEFI code:** UEFI Specification 2.10+ (table formats, calling conventions, memory ownership rules, error handling requirements)
   - **ACPI code:** ACPI Specification 6.5+ (table signatures, checksums, revision handling, GAS parsing rules)
   - **SMBIOS code:** SMBIOS Specification 3.x (structure walking, string extraction, type-specific field encoding)
   - **Win32/NT code:** Windows Internals, ReactOS reference, MSDN documentation (NTSTATUS codes, parameter validation, privilege requirements)
   - **x86-64 code:** Intel SDM / AMD APM (MSR usage, CPUID leaf requirements, CR register rules, interrupt handling)
   - For each spec requirement: does the implementation follow the spec exactly, or does it take shortcuts? Document deviations.

3. **Win11/Linux parity analysis** -- for each feature, how does Windows 11 and Linux handle the same thing?
   - What does Windows bootmgr/hal.dll/ntoskrnl do for this feature?
   - What does Linux efi-stub/kernel do?
   - Are there approaches they use that we should adopt?
   - Are there mistakes they made that we should avoid?
   - Document concrete differences, not vague comparisons.

4. **Codex performance + consistency review** -- dispatch TWO Codex reviews in sequence:

   **4a. Performance review** (codex-perf-review approach):
   ```bash
   node "/home/derickpayne/.claude/plugins/marketplaces/openai-codex/plugins/codex/scripts/codex-companion.mjs" adversarial-review "<perf prompt>"
   ```
   Focus areas:
   - Allocations in hot paths (ISR, per-tick, per-syscall)
   - O(n^2) algorithms where O(n) or O(log n) suffices
   - Spinlock hold times -- how much work under each lock?
   - Cache-hostile access patterns (struct layout vs access order)
   - Redundant computation in loops
   - Byte-at-a-time operations where bulk copies exist
   - klog/printk in hot paths

   **4b. Consistency + dead code review** (codex-consistency-audit + codex-dead-code):
   ```bash
   node "/home/derickpayne/.claude/plugins/marketplaces/openai-codex/plugins/codex/scripts/codex-companion.mjs" adversarial-review "<consistency prompt>"
   ```
   Focus areas:
   - Struct layout matches between header, implementation, bootloader, and test
   - Constants defined in one place, used consistently everywhere
   - Unreachable functions, unused defines, orphaned types
   - API contracts: does the header promise something the implementation doesn't deliver?
   - Error code consistency: same error mapped to same NTSTATUS everywhere

5. **Classify and triage findings** -- apply `superpowers:receiving-code-review` discipline to every finding from steps 2-4. For EACH finding:
   - **Verify technically first.** Read the cited code. Is the finding correct? Codex and spec research can be wrong.
   - **If valid:** classify it with one label below and proceed to fix.
   - **If wrong/misleading:** reject with concrete code evidence. Classify as Accept.
   - **If correct but truly out of scope** (requires missing infrastructure, different subsystem owner): classify as Accept with `-> XREF` to the owner.

   | Label | Meaning | Action |
   |-------|---------|--------|
   | **Spec violation** | Code contradicts a published specification | Fix -- this is a bug |
   | **Optimization** | Measurably better approach exists | Fix if on hot path; Accept if cold path |
   | **Best practice** | Industry convention we should follow | Fix |
   | **Simplification** | Same behavior with less code | Fix |
   | **Parity gap** | Win11/Linux does this better | Fix if feasible; Accept with XREF if large |
   | **Dead code** | Unreachable or unused | Remove |
   | **Accept** | Wrong, irrelevant, or needs missing infrastructure | Document why with code evidence |

6. **Fix loop** -- fix ALL non-Accept findings. Apply in priority order:
   - Spec violations first (bugs)
   - `STATUS_NOT_IMPLEMENTED` stubs: if the function is **standalone** (self-contained, no deep dependency chain), IMPLEMENT it fully -- features should not get lost behind stubs. If it requires **significant new infrastructure**, Accept with a domain-qualified XREF and advise creating a new TODO section (Branch B) or TODO file (Branch C).
   - Best practices and simplifications (low-risk improvements)
   - Dead code removal
   - Parity gaps (new functionality)
   - Optimizations (hot-path only)
   - Build after each batch of related fixes: `bash scripts/build.sh`, confirm `=== BUILD OK ===`.
   - If a fix breaks the build, revert and Accept with justification.

7. **Self-review** -- after all fixes applied:
   - Walk every fix: does it introduce regressions, new races, or new complexity?
   - Confirm no TODO/FIXME/HACK introduced by fixes
   - Final build: `bash scripts/build.sh`, confirm `=== BUILD OK ===`.

8. **Report** -- summarize what was fixed vs accepted:
   ```
   ## Quality Review: TODO-XX §N -- <section name>
   ### Fixed
   - [spec] <what was wrong> -- <what was fixed>
   - [best] ...
   - [dead] ...
   ### Accepted (no action)
   - [accept] <finding> -- <why>
   ```

9. **Quality stamp** -- add or update IMMEDIATELY after the Verified stamp (no blank line between stamps):
   ```
   > **Quality reviewed:** YYYY-MM-DD -- <summary of fixes>. Accepted: <deferred items with domain-qualified XREFs, or "none">.
   ```
   If no fixes were needed (clean review):
   ```
   > **Quality reviewed:** YYYY-MM-DD -- no findings. Industry standards compliant.
   ```
   Rules:
   - If re-reviewing, REPLACE the existing `> **Quality reviewed:**` line
   - **No blank line** between Verified and Quality reviewed stamps
   - All TODO cross-references in Accepted field MUST include domain prefix (e.g., `02-kernel-core/TODO-17`, not just `TODO-17`)

10. **Commit and push**
    - Commit message: `"quality: <TODO file> §N -- <summary of fixes>"`
    - Push to `origin/main` immediately.

## What This Skill Does NOT Do

- Does NOT downgrade checklist items. That's `/verify-todo-section`.
- Does NOT implement new features beyond what the section already covers. That's `/implement-todo-section`.
- Does NOT replace the Codex adversarial review in verify -- this is complementary, not a substitute.

## What This Skill DOES Produce

- A classified quality report comparing implementation against specs, industry standards, and Win11/Linux approaches.
- Fixes for all valid findings (spec violations, best practices, dead code, parity gaps).
- A `> **Quality reviewed:**` stamp with audit trail of what was fixed and what was accepted.

## Guardrails

- Apply `superpowers:receiving-code-review` discipline -- verify every finding before acting. Do not blindly implement Codex suggestions.
- Do not optimize without evidence the path is hot. "Could be faster" without "is measurably slow" is an Accept.
- Do not sacrifice correctness or clarity for micro-optimization.
- Do not add complexity for marginal gains. If the improvement is < 1 us on a cold path, skip it.
- Do not turn this into a rewrite. The code works. The goal is targeted improvements, not a fresh start.
- When researching Win11/Linux approaches, cite the source (function name, file, or documentation section) -- not vague claims.

## Relationship to Other Skills

- **verify-todo-section:** "Is it done?" (compliance) -- run FIRST, then quality-review.
- **quality-review-section:** "Is it done RIGHT?" (quality) -- run AFTER verify passes.
- **codex-perf-review:** Performance subset -- this skill includes perf review as step 4a plus adds standards, parity, and consistency analysis.
- **codex-consistency-audit:** Consistency subset -- this skill includes it as step 4b.
- **codex-dead-code:** Dead code subset -- included in step 4b.
