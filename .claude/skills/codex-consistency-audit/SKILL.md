---
name: codex-consistency-audit
description: Codex-driven cross-file consistency audit. Verifies that constants, struct offsets, API contracts, and registration tables match between header, implementation, test, assembly, and TODO files. Like audit-ssdt but generalized to any subsystem.
---

# Codex Consistency Audit

> **External-Reviewer Contract:** Codex is a subordinate reviewer, not authority. Every finding from this skill goes through `superpowers:receiving-code-review` (verify at file:line, Fix / Reject / Accept, never blind-implement). Canonical contract: [docs/infrastructure/ai-system.md#external-reviewer-contract-codex](../../../docs/infrastructure/ai-system.md#external-reviewer-contract-codex).

## Prompt Shape

Every dispatch from this skill MUST open its prompt with the marker `[review-kind: consistency] <todo-path>` on the first non-blank line. The marker is what `.claude/hooks/skill_step_observer.py` and the section-commit four-dispatch gate use to attribute the dispatch. Un-marked dispatches waste a Codex round and block the next section-commit. Canonical reference for all 8 markers: [.claude/skills/codex-prompt-shape.md](../codex-prompt-shape.md).

## Use This Skill When

- A subsystem has structs shared across C and assembly files.
- Constants are defined in one header and used in 5+ files.
- After refactoring that touched struct layouts or constant values.
- The user asks "is everything consistent?" or "did I miss any callers?"

## Workflow

1. **Identify the consistency domain:**
   - The header file(s) defining the types/constants
   - All `.c` files that include those headers
   - All `.asm` files that reference the same offsets or values
   - All `test_*.c` files that assert on sizes/offsets/values
   - All `_Static_assert` in the codebase for those types

1.5. **Pre-fetch the reference set via `mcp__lsp-bridge__references` (MANDATORY for typed symbols).** Two-step workflow because `references` requires `path/line/character` (cursor position), not a symbol name: (a) call `mcp__lsp-bridge__workspace_symbol <name>` for each named type / constant / function in the consistency domain to resolve the canonical declaration site; (b) call `mcp__lsp-bridge__references` with the resolved cursor to get the AST-resolved use sites. Paste the result into the Codex prompt as the verified reference set so Codex audits actual divergence between known callers, not heuristically-discovered ones. The LSP doesn't index `.asm`, so for assembly-side mirrors fall back to `Bash(grep)` for the offsets / constants and combine the two lists. Doctrine: [docs/infrastructure/mcp-usage.md](../../../docs/infrastructure/mcp-usage.md).

2. **Dispatch to Codex plugin** (with the `mcp__lsp-bridge__references` output + assembly-side grep result pasted into the prompt as the verified consistency-domain set):
   ```bash
   bash scripts/codex-dispatch.sh '[review-kind: consistency] <todo-path> <consistency prompt>'
   ```

3. **Triage findings** -- the PostToolUse hook fires `receiving-code-review` reminder; follow it. **Consistency-audit false-positive watch:** Codex misreads padding and packed/aligned attributes when comparing `.asm` offsets to C struct layouts; flags `#define` drift between files when the drift is deliberately scoped (kernel constant vs. bootloader constant); demands static asserts for invariants that no cross-file code depends on. Open both sides of every claimed mismatch yourself before fixing.
4. **Categorize verified findings:**
   - **Offset mismatch** -- assembly uses hardcoded offset that doesn't match struct layout
   - **Size mismatch** -- `sizeof()` in test doesn't match actual struct size
   - **Constant drift** -- `#define` value changed in header but hardcoded copy exists elsewhere
   - **Missing static assert** -- cross-file invariant without compile-time protection
   - **Registration gap** -- function exists but isn't registered in dispatch/init/test table
   - **API contract drift** -- function signature in header doesn't match implementation

5. **Fix verified inconsistencies** and add missing `_Static_assert` where the 5-layer defense pattern applies. Reject false positives with file:line evidence.

## Prompt Template

```
Cross-file consistency audit for <subsystem>.

Header: <path> (defines types and constants)
Implementation: <paths>
Assembly: <paths> (hardcoded offsets)
Tests: <paths> (static asserts and test assertions)

Check:
1. Every struct field offset used in .asm matches the C struct layout
2. Every sizeof() assertion matches the actual struct size
3. Every #define constant value is consistent across all files
4. Every public function signature in the header matches the .c implementation
5. Every function registered in init/dispatch tables exists and has correct signature
6. Every _Static_assert is still valid (not stale after refactoring)
7. Every cross-file invariant has at least a _Static_assert (Layer 1 of 5-layer defense)

Report mismatches with: file:line, expected value, actual value, risk level.
```

## Guardrails

- This is audit-only. Fix mismatches after the audit, not during.
- Focus on cross-file boundaries -- intra-file consistency is the compiler's job.
- Always check `.asm` files -- they are the highest risk because the assembler doesn't type-check.
- If the audit finds no issues, report what was checked (file count, assert count) so the user knows it ran.
