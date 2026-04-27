---
name: codex-test-coverage
description: Codex-driven test coverage gap analysis. Given a source file or subsystem, Codex identifies untested code paths, missing boundary tests, error paths without assertions, and uncovered branches. Use after implementing a section to strengthen the test suite, or when auditing test quality.
---

# Codex Test Coverage Analysis

> **External-Reviewer Contract:** Codex is a subordinate reviewer, not authority. Every finding from this skill goes through `superpowers:receiving-code-review` (verify at file:line, Fix / Reject / Accept, never blind-implement). Canonical contract: [docs/infrastructure/ai-system.md#external-reviewer-contract-codex-copilot](../../../docs/infrastructure/ai-system.md#external-reviewer-contract-codex-copilot).

## Use This Skill When

- A section has been implemented and tests written, but you want to verify nothing was missed.
- The user asks "are my tests sufficient?" or "what test cases am I missing?"
- Auditing test quality for a subsystem before marking a TODO complete.

## Workflow

1. **Identify the source files and test file:**
   - Source: the `.c` file(s) implementing the feature
   - Tests: the corresponding `test_*.c` file
   - Header: the public API in the `.h` file

2. **Build a coverage analysis prompt:**
   - List every public function in the header
   - List every test assertion in the test file
   - Ask Codex to identify gaps

3. **Dispatch to Codex plugin:**
   ```bash
   node "$HOME/.claude/plugins/marketplaces/openai-codex/plugins/codex/scripts/codex-companion.mjs" adversarial-review "<coverage prompt>"
   ```

4. **Triage findings** -- the PostToolUse hook fires `receiving-code-review` reminder; follow it. **Test-coverage false-positive watch:** Codex misses indirect assertions (a higher-level test that exercises the path); demands tests for unreachable error paths (`if (size > SIZE_MAX)` on a `size_t` -- delete the dead branch instead); demands tests for hypothetical NULL inputs that no caller produces. Verify reachability and real-caller behavior before adding any test.
5. **Classify each verified gap:**
   - **Untested public functions** -- public API without any test assertion
   - **Missing boundary tests** -- off-by-one, exact limits, overflow, zero-length
   - **Untested error paths** -- allocation failure, NULL input, invalid args, overflow
   - **Missing negative tests** -- inputs that should be rejected
   - **Missing concurrency tests** -- if the code has spinlocks or atomics, are there concurrent-access tests?
   - **Missing integration tests** -- function works in isolation but not wired into the real call path

6. **Add tests for verified gaps only** to the test file.

7. **Rebuild** and confirm `=== BUILD OK ===`.

## Prompt Template

```
Test coverage gap analysis for <subsystem>.

Source files:
<list .c files with line counts>

Public API (from header):
<list every function signature>

Existing tests (from test file):
<list every test function name and what it asserts>

Find:
1. Public functions with NO test coverage
2. Error paths (NULL input, allocation failure, overflow) without assertions
3. Boundary conditions not tested (exact limits, off-by-one, zero-length)
4. Negative tests missing (inputs that should be rejected)
5. Code paths that are only tested indirectly (no direct assertion)

For each gap, suggest a concrete test: function call + expected result.
```

## Guardrails

- Do not delete or weaken existing tests. Only add new ones.
- Every suggested test must have a concrete expected value -- no "verify it works."
- Register new tests in the correct `test_register_*()` function and `TEST_CAT_*` category.
- This skill analyzes coverage, not correctness. Use `codex-adversarial-review-section` for correctness review.
