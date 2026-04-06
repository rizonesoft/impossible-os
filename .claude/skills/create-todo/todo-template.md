# TODO Template

Use this as the default starting point for a new leaf TODO.

```markdown
# TODO-NN -- Short Name

> **Goal:** One short paragraph describing the current state, the change to make, and the end state.

> [!IMPORTANT]
> **Current state:** Describe what exists NOW -- which files, functions, and infrastructure are already in place. What works. What doesn't. Without this, the implementer has to grep the codebase to understand the starting point.

## Inputs

- [`src/kernel/relevant.c`](../../src/kernel/relevant.c) -- existing implementation
- [`include/kernel/relevant.h`](../../include/kernel/relevant.h) -- public API
- -> XREF: `TODO-XX-dependency.md §N` -- structural dependency (must exist before this TODO)
- -> XREF: `TODO-YY-related.md §M` -- related work (complement, not blocker)

## Outcome

- Clear result 1 with specific deliverable.
- Clear result 2 with specific deliverable.

## Implementation Order

| ⭐  | Order | Deliverable                  | Depends On   | Status |
| --- | :---: | ---------------------------- | ------------ | :----: |
| 💎  |   1   | Foundation / parity          | --           |  [ ]   |
| 💎  |   2   | More parity work             | §1           |  [ ]   |
| ⭐  |   3   | Exclusive differentiator     | §1, §2       |  [ ]   |

> 💎 = parity work -- matches what Windows 11 and Linux already do.
> ⭐ = exclusive work -- Impossible OS is superior or first.

---

## 1. Foundation Section

One-line intro explaining what this section does and why it matters.

- [ ] Define `relevant_struct_t` in `include/kernel/relevant.h`
- [ ] Implement `relevant_init()` in `src/kernel/relevant.c`
- [ ] Wire into boot path: call from `boot_desktop.c` Phase 3
- [ ] Commit: `"kernel: relevant -- foundation implementation"`

**Test checkpoint:** Serial log shows `"relevant: initialized (N items)"`. `POST16(0xD801)` on entry, `POST16(0xD802)` after init. Test on: QEMU WHPX + TCG; bare metal.

## 2. Parity Feature

One-line intro explaining what this section does and why it matters.

> [!NOTE]
> **Resolved:** Brief note about a design question that was answered or a prerequisite that is now available.

- [ ] Implement `relevant_process()` with full error handling
- [ ] Register in dispatcher / SSDT / hook table as needed
- [ ] Commit: `"kernel: relevant -- parity feature"`

**Test checkpoint:** `relevant_process(valid_input)` returns expected result. `relevant_process(NULL)` returns error without crash. Serial log shows `"relevant: processed <N> items"`. Test on: QEMU WHPX + TCG; bare metal.

## 3. Exclusive Differentiator

One-line intro explaining what this section does and why it matters.

> [!TIP]
> Neither Win11 nor Linux implements this -- Impossible OS is first.

> [!WARNING]
> **SMP safety:** This section introduces shared mutable state (`g_relevant_table`). Must use spinlock or atomics. See `kernel-code-quality` Gate 2.

- [ ] Implement exclusive feature with concrete deliverable
- [ ] Commit: `"kernel: relevant -- exclusive feature"`

**Test checkpoint:** Exclusive feature produces expected observable behavior. Serial log shows `"relevant: exclusive feature active"`. Test on: QEMU WHPX + TCG; bare metal.

**Regression risk:** This section touches [interrupt path / page tables / etc]. If this breaks, revert [specific change] and fall back to [known-good behavior].

---

## OS Comparison

| ⭐ | Feature              | 🪟 Win11              | 🐧 Linux              | 🚀 Impossible OS          |
|----|----------------------|----------------------|----------------------|----------------------------|
| 💎 | Core feature         | ✅ Full support      | ✅ Full support      | ⬜ Planned -- §1           |
| 💎 | Parity feature       | ✅ Supported         | ✅ Supported         | ⬜ Planned -- §2           |
| ⭐ | Exclusive feature    | ❌ Not available     | ❌ Not available     | ⬜ Planned -- world-first  |

> **After §1-§2:** Impossible OS matches Windows 11 and Linux for core functionality.
> **After §3:** Impossible OS surpasses both with exclusive feature.

<!-- Sources: https://learn.microsoft.com/..., https://docs.kernel.org/... -->

## Unit Tests

> Wire into `test_runner_init()` via `test_register_relevant()` -- register in `src/kernel/test/test_runner.c`.
> Tests run with `debug=1` or `test=1` in boot.conf. Use `TEST_CAT_<CATEGORY>`.

- [ ] Create `src/kernel/test/test_relevant.c` with:
  - `relevant_init()` returns success on first call
  - `relevant_process(valid_input)` returns expected value
  - `relevant_process(NULL)` returns error code, not crash
  - Struct size: `sizeof(relevant_struct_t)` == expected bytes
  - Constants: `RELEVANT_MAX` == expected value
- [ ] Register in `test_runner_init()`: `test_register_relevant()`
- [ ] Commit: `"test: add relevant test suite"`

## Verification

- [ ] `bash scripts/build.sh clean` -> `tail -1 build/build.log` -> `=== BUILD OK ===`
- [ ] `relevant_init()` logs `"relevant: initialized (N items)"` on serial
- [ ] `relevant_process(valid_input)` returns 0 (success)
- [ ] POST16 codes 0xD801/0xD802 appear in correct order on serial
- [ ] Unit tests pass: `make test-relevant` shows all PASS
- [ ] Verify on: QEMU WHPX (2 CPUs), QEMU TCG, VirtualBox, bare metal
- [ ] Commit: `"kernel: relevant -- complete"`
```

## Notes

- Default to a leaf TODO.
- Add child-TODO sections only when the topic truly needs multiple files or shared verification.
- Use flat numbered sections (`## 1. Title`) -- not sub-sections (`### 1.1 Title`).
- Every section needs: checklist items (max 8-10), Commit line, Test checkpoint.
- `Depends On` column always uses `§N` notation, never bare numbers.
- The `> [!IMPORTANT] Current state:` callout after Goal is mandatory -- it's the implementer's starting context.
- The OS Comparison table is **mandatory** -- populate from actual Win11/Linux research, add `<!-- Sources -->` comment.
- The Unit Tests section is **mandatory** -- derive test cases from implementation deliverables.
- Keep tables compact. The `💎`/`⭐` column must always be present in both tables.
- Use ✅ = done, ⚠️ = partial, ⬜ = planned, ❌ = not available/not applicable.
- Use callouts: `> [!NOTE]` resolved, `> [!WARNING]` risks, `> [!IMPORTANT]` blockers, `> [!TIP]` advantages.
- **XREFs in Inputs** = structural deps (must exist before this TODO). **XREFs in sections** = implementation-time cross-refs.
- If a new `TEST_CAT_*` is needed, note it in the Unit Tests section.
