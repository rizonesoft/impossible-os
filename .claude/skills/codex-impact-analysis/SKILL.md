---
name: codex-impact-analysis
description: Codex-driven dependency impact analysis. Before changing a function signature, struct layout, or constant, Codex scans all callers and consumers to identify what would break. Use before refactoring, renaming, or modifying any cross-file API.
---

# Codex Impact Analysis

> **External-Reviewer Contract:** Codex is a subordinate reviewer, not authority. Every finding from this skill goes through `superpowers:receiving-code-review` (verify at file:line, Fix / Reject / Accept, never blind-implement). Canonical contract: [docs/infrastructure/ai-system.md#external-reviewer-contract-codex-copilot](../../../docs/infrastructure/ai-system.md#external-reviewer-contract-codex-copilot).

## Use This Skill When

- About to change a function signature (add/remove/reorder parameters).
- About to modify a struct layout (reorder fields, change sizes, add/remove fields).
- About to rename or remove a public symbol.
- About to change a `#define` constant value that other files depend on.
- The user asks "what would break if I change X?"

## Workflow

1. **Identify the change target:**
   - Function name, struct name, constant name, or file path
   - The specific change planned (new parameter, removed field, renamed symbol, etc.)

2. **Dispatch to Codex plugin:**
   ```bash
   node "/home/derickpayne/.claude/plugins/marketplaces/openai-codex/plugins/codex/scripts/codex-companion.mjs" adversarial-review "<impact analysis prompt>"
   ```

3. **Evaluate findings with `superpowers:receiving-code-review` discipline** -- spot-check Codex's dependent list before trusting it:
   - **Verify high-risk hits.** For every assembly file Codex flags, open it and confirm the symbol/offset is actually referenced. Codex can hallucinate dependencies that don't exist, especially in `.asm` files.
   - **Verify the absence claim is real.** If Codex returns "no callers found," grep the codebase yourself before acting on the assumption. A missed caller in impact analysis becomes a runtime crash.
   - **Don't accept "safe to change" without proof.** If Codex says a change is safe, your job is still to spot-check the riskiest call sites.
4. **Categorize verified dependents:**
   - **Direct callers** -- functions that call the target directly
   - **Indirect consumers** -- code that uses the target's output (return value, struct field)
   - **Assembly dependents** -- `.asm` files that reference the symbol or depend on struct offsets
   - **Test dependents** -- tests that assert on the target's behavior
   - **Static asserts** -- `_Static_assert` that would fail after the change
   - **Cross-TODO impact** -- TODO checklist items that reference the target

5. **Report impact:**
   - List every file + line that needs updating
   - Classify each as: must-update, should-verify, or no-change-needed
   - Flag any assembly or ABI dependents (highest risk -- no compiler error if wrong)
   - Note any Codex claims you spot-checked and any you couldn't verify

## Prompt Template

```
Impact analysis: I plan to change <target> in <file>.

Planned change: <describe the change>

Find ALL dependents:
1. Direct callers of this function / users of this struct / references to this constant
2. Indirect consumers (code that uses the return value or reads struct fields)
3. Assembly files (.asm) that reference this symbol or depend on its layout
4. Test files that assert on this function/struct/constant
5. Static asserts that would break
6. TODO files that reference this symbol

For each dependent, state: file:line, what it does with the target, and whether it needs updating.
```

## Guardrails

- This is analysis only -- do not make changes. Report what would break.
- If the change has > 20 dependents, suggest whether a wrapper/compatibility shim is worth the cost vs updating all callers.
- Always check `.asm` files -- they won't produce compiler errors on wrong offsets.
- If the target is in a header included by 50+ files, note the blast radius explicitly.
- Apply `superpowers:receiving-code-review` discipline -- spot-check Codex's dependent list. A missed dependency in impact analysis becomes a runtime crash; an "all clear" verdict is not a guarantee, it's a hypothesis to verify on the highest-risk files.
