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

3. **Evaluate findings by category:**
   - **Unreachable functions** -- `static` functions with zero callers in the same file
   - **Unused `#define`** -- constants defined but never referenced
   - **Orphaned types** -- `typedef struct` with no variables, parameters, or casts using it
   - **Stale declarations** -- function declared in `.h` but never defined or called
   - **Commented-out code** -- `#if 0` blocks, `/* disabled */` sections
   - **Unused parameters** -- function parameters that are never read
   - **Stale `extern`** -- `extern` declaration for a symbol that no longer exists

4. **Classify each finding:**
   - **Safe to remove** -- no callers, no assembly references, no external linkage
   - **Verify first** -- might be called via function pointer, assembly, or macro expansion
   - **Keep (planned)** -- referenced by an unimplemented TODO section (note the XREF)

5. **Remove safe-to-remove items** and rebuild.

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
