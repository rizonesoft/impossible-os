---
name: codex-test-coverage
description: Codex-driven test coverage gap analysis. Given a source file or subsystem, Codex identifies untested code paths, missing boundary tests, error paths without assertions, and uncovered branches. Use after implementing a section to strengthen the test suite, or when auditing test quality.
---

# Codex Test Coverage Analysis

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
   node "/home/derickpayne/.claude/plugins/marketplaces/openai-codex/plugins/codex/scripts/codex-companion.mjs" adversarial-review "<coverage prompt>"
   ```

4. **Evaluate findings with `superpowers:receiving-code-review` discipline** -- Codex's gap list is suggestions, not orders. For each gap before adding a test:
   - **Verify the gap is real.** Read the existing test file at the cited section. Is the path actually untested, or did Codex miss an indirect assertion (e.g., a higher-level test that exercises the path)?
   - **Verify the path is reachable.** Check the source file. If the "untested error path" is unreachable (e.g., `if (size > SIZE_MAX)` on a `size_t`), don't add a test for dead code -- delete the dead branch instead.
   - **YAGNI check.** If Codex demands tests for hypothetical inputs that no caller produces (e.g., NULL on a function whose only callers always pass an Object Manager handle), the test would assert behavior that doesn't matter. Reject with reasoning.
   - **No performative agreement.** Reject wrong gaps with code evidence; add real ones without commentary.
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
- Apply `superpowers:receiving-code-review` discipline -- verify each gap is real and reachable before adding a test. Don't add tests for dead code, unreachable branches, or hypothetical inputs that no caller produces.
