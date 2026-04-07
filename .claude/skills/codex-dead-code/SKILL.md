---
name: codex-dead-code
description: Codex-driven dead code scanner. Finds unreachable functions, unused defines, orphaned types, and stale declarations after TODO sections are completed or code is refactored. Use periodically or after completing a TODO to clean up the codebase.
---

# Codex Dead Code Scanner

## Use This Skill When

- A TODO has been completed and you want to clean up leftover scaffolding.
- After a refactor that removed callers or changed APIs.
- Periodically to keep the codebase lean.
- The user asks "is there dead code?" or "what can I remove?"

## Workflow

1. **Scope the scan:**
   - Single file: scan one `.c` + its `.h` for unused static functions and local defines
   - Subsystem: scan all files under a directory (e.g., `src/kernel/mm/`)
   - Full: scan entire `src/kernel/` (use background mode for large scans)

2. **Dispatch to Codex plugin:**
   ```bash
   node "/home/derickpayne/.claude/plugins/marketplaces/openai-codex/plugins/codex/scripts/codex-companion.mjs" adversarial-review "<dead code prompt>"
   ```

3. **Evaluate findings with `superpowers:receiving-code-review` discipline** -- this skill has the highest false-positive risk. Removing live code based on a wrong "dead" verdict is a regression. For every finding, before treating it as dead:
   - **Grep yourself.** Run a project-wide Grep for the symbol name. Codex can miss callers reached via function pointers, macros, and assembly.
   - **Check `.asm` files explicitly.** A function called from assembly looks dead from C alone. Always grep `*.asm` for the symbol before removing.
   - **Check TODO files.** A symbol referenced by an unimplemented TODO section is *planned*, not dead.
   - **Check external linkage.** Symbols exported via the SSDT, syscall table, dispatch table, or driver registration are reachable through indirect paths Codex won't trace.
   - **YAGNI inversion.** Codex's "dead code" finding is actually the *opposite* of the typical YAGNI case -- it's saying "no caller exists, so remove it." Confirm there's truly no caller before agreeing.
4. **Classify each verified finding:**
   - **Safe to remove** -- verified no callers, no assembly references, no external linkage, no TODO references
   - **Verify first** -- might be called via function pointer, assembly, or macro expansion
   - **Reject** -- finding is wrong; symbol IS used at file:line (provide evidence)
   - **Keep (planned)** -- referenced by an unimplemented TODO section (note the XREF)

5. **Remove safe-to-remove items only** and rebuild after each batch -- if the build breaks, the "dead" symbol wasn't actually dead. Revert and reclassify.

## Prompt Template

```
Dead code scan for <scope>.

Files:
<list .c and .h files>

Find:
1. Static functions with zero callers in their file
2. #define constants never referenced in any .c or .h file
3. typedef/struct definitions never used as variable type, parameter, or cast
4. Function declarations in .h without a corresponding definition in .c
5. Commented-out code blocks (#if 0, /* disabled */)
6. Function parameters that are never read in the function body
7. extern declarations for symbols that don't exist

For each finding: file:line, symbol name, category, confidence (certain/likely/check).
Mark as "safe to remove" or "verify first" based on whether it could be reached via function pointer or assembly.
```

## Guardrails

- Never remove symbols that are referenced by `.asm` files -- check first.
- Never remove symbols that are part of a public API in a `.h` file unless the `.h` is also being cleaned up.
- Never remove symbols referenced by TODO checklist items (they're planned, not dead).
- Rebuild after every removal to confirm nothing broke.
- If unsure whether something is dead, leave it and note it as "verify first."
- Apply `superpowers:receiving-code-review` discipline aggressively here -- this skill has the highest false-positive risk of any codex-* skill. Codex's "dead code" verdict is a hypothesis, not a fact. Verify with project-wide Grep (including `.asm`, TODO files, registration tables) before removing anything.
