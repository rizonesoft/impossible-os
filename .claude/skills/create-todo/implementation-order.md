# Implementation Order, XREF, And Index Rules

## Naming And Placement

- Choose the domain from `todo/TODO-00-INDEX.md` and the domain `INDEX.md`.
- Use the domain's live local naming pattern, such as `TODO-03-short-name.md`.
- Keep the TODO in one canonical home and cross-link instead of cloning scope elsewhere.

## Implementation Order Table

Use this format:

```markdown
| ⭐  | Order | Deliverable              | Depends On   | Status |
| --- | :---: | ------------------------ | ------------ | :----: |
| 💎  |   1   | Foundation / parity      | --           |  [ ]   |
| ⭐  |   2   | Exclusive differentiator | §1           |  [ ]   |
| 💎  |   3   | Cross-dep parity work    | §1, T11 §3   |  [ ]   |
```

- `💎` = parity work -- this is what Windows 11 and Linux already do; Impossible OS must match it.
- `⭐` = exclusive work -- Impossible OS is superior or first; this is a differentiating feature.
- Every row must carry one of these two markers. Never leave the first column blank or use numbers there.
- **Every `## N.` body section MUST have a corresponding table row, and vice versa.** No orphan rows or missing rows.

### Compact Dependency Notation (mandatory)

| Dependency type | Format | Example |
|-----------------|--------|---------|
| No dependency | `--` | `--` |
| Internal section | `§N` | `§2` |
| Multiple internal | `§N, §M` | `§1, §3` |
| Same-domain TODO | `TNN §N` | `T17 §3` |
| Cross-domain TODO | `DNN TNN §N` | `D02 T19 §1` |
| Section range | `§N-N` | `§1-7` |

- **Always use `§` prefix for section references.** Never bare numbers (`1, 2` -> `§1, §2`).
- **Cross-TODO references MUST include specific section numbers** (e.g., `T11 §1,§3`). Never bare TODO numbers -- ambiguous and blocks planning.
- Domain numbers are the two-digit prefix of the folder (e.g., `02` for `02-kernel-core`). Omit when the referenced TODO is in the same domain.
- Do not create a later formatting-only cleanup step; the table must be correct at creation time.

## Section Structure Requirements

Every numbered implementation section (`## N. Title`) MUST include:

1. **Checklist items** (`- [ ]`) with concrete deliverables referencing specific functions, types, or files.
2. **Commit line** (`- [ ] Commit: "scope: short description"`) as the last checklist item.
3. **Test checkpoint** (`**Test checkpoint:**`) with:
   - Concrete pass/fail criteria (exact serial strings, POST16 codes, expected behavior).
   - Platform list: `Test on: QEMU WHPX + TCG; bare metal.`
4. **Regression risk note** (for high-risk sections touching interrupt path, page tables, GDT/TSS, timers, boot order):
   - `**Regression risk:**` identifying what could break.
   - Rollback strategy: "If this breaks, revert [change] and fall back to [behavior]."

## OS Comparison Table

Every TODO must include an `## OS Comparison` section. Use this format:

```markdown
## OS Comparison

| ⭐ | Feature           | 🪟 Win11          | 🐧 Linux            | 🚀 Impossible OS            |
|----|-------------------|--------------------|---------------------|------------------------------|
| 💎 | Core feature      | ✅ Full support    | ✅ Full support    | ⬜ Planned -- §1            |
| ⭐ | Exclusive feature | ❌ Not available   | ❌ Not available   | ⬜ Planned -- world-first   |
```

- `💎` rows = parity features; Impossible OS must reach the same level as Windows and Linux.
- `⭐` rows = exclusive features; Impossible OS goes further than either competitor.
- Status cells: ✅ = done/shipping, ⚠️ = partial/limited, ❌ = not available, ⬜ = planned (link the section).
- Keep cells short: status emoji + brief phrase (add words when needed for a fair comparison). **Hard cap: 200 characters per raw markdown table row** (including pipes and alignment padding). Tighten wording, shorten header column names (`🪟 Win11 projects` -> `🪟 Win11`), or use a `§N` pointer; do not strip spaces that align columns. Exceptions require an inline HTML comment on the line above: `<!-- row-length-exempt: <reason> -->`. Pad all rows (separator uses `-` per column width, min 3).
- **Populate from actual research** (web searches for Win11/Linux features), not guesswork.
- Add a short paragraph after the table summarizing the competitive position at each milestone.

## Unit Tests Section

Every TODO must include a `## Unit Tests` section. Use this format:

```markdown
## Unit Tests

> Wire into `test_runner_init()` via `test_register_<feature>()` -- register in `src/kernel/test/test_runner.c`.
> Tests run with `debug=1` or `test=1` in boot.conf.

- [ ] Create `src/kernel/test/test_<feature>.c` with:
  - `function_a()` returns expected value for valid input
  - `function_a(NULL)` returns error code, not crash
  - Struct size: `sizeof(struct_t)` == expected bytes
- [ ] Register in `test_runner_init()`: `test_register_<feature>()`
- [ ] Commit: `"test: add <feature> test suite"`
```

- Each test case must be a specific assertion -- function call + expected return value or observable state.
- No vague "test that X works" items.
- Derive test cases from the implementation sections' deliverables.

## XREF And Handoff Rules

- Add `-> XREF:` only when another TODO section is a real dependency or overlap.
- If two TODOs touch the same boundary, add a short overlap or handoff note near the affected section.
- Keep cross-file dependencies explicit enough that another agent can follow the order without guessing.

## Index Updates

- Update the domain `INDEX.md` whenever the new TODO changes active scope in that domain.
- Update `todo/TODO-00-INDEX.md` only when the new TODO changes root-level epic visibility.
- Do not leave numbering or index updates for a later sync pass.
