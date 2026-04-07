---
name: codex-consistency-audit
description: Codex-driven cross-file consistency audit. Verifies that constants, struct offsets, API contracts, and registration tables match between header, implementation, test, assembly, and TODO files. Like audit-ssdt but generalized to any subsystem.
---

# Codex Consistency Audit

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

2. **Dispatch to Codex plugin:**
   ```bash
   node "/home/derickpayne/.claude/plugins/marketplaces/openai-codex/plugins/codex/scripts/codex-companion.mjs" adversarial-review "<consistency prompt>"
   ```

3. **Evaluate findings with `superpowers:receiving-code-review` discipline** -- verify each mismatch before "fixing" it:
   - **Read both sides of the claimed mismatch.** If Codex says "asm offset 0x18 doesn't match struct field at 0x20," open the .asm file and the C struct yourself. Codex sometimes counts padding wrong or misreads packed/aligned attributes.
   - **Check whether the "drift" is intentional.** A `#define` that differs between two files may be deliberately scoped (e.g., kernel constant vs. bootloader constant). Don't unify them blindly.
   - **Don't auto-add static asserts.** If Codex says "missing static assert," verify the invariant is actually load-bearing (does cross-file code depend on it?) before adding compile-time checks.
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
- Apply `superpowers:receiving-code-review` discipline -- read both sides of every claimed mismatch before fixing. Codex misreads padding, packed attributes, and intentionally-scoped constants.
