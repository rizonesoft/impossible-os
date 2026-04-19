# TODO-06 -- TODO Metadata Layer and Derived Graph

> **Goal:** Keep markdown TODO files canonical, but add a stable-ID frontmatter layer + a derived index (JSON cache) so cross-TODO dependencies, backlinks, and "ready / blocked / orphaned" queries work without line-number fragility or manual grep sweeps. Markdown stays git-friendly and editor-native; the cache is a read-only projection that the generator rebuilds from the source files.

> [!IMPORTANT]
> **Current state:** The repo has ~86 TODO files under `todo/` organized into 16 domains. Cross-TODO references use a mix of `TODO-NN §N` path refs and a compact `TNN §N` / `DNN TNN §N` shorthand. Semantic identity lives in the filename ("TODO-02-ai-development-system.md"); renumbering or renaming a TODO breaks every inbound XREF silently. The `Deferred:` / `Accepted:` stamp grammar relies on `(item: "NAME" at line N)` parentheticals -- line numbers volatile, stamps decay. `validate-todo-file` and `gap-analysis-todo` skills walk the graph in prose at audit time but nothing caches the relationships. The pre-commit hook (`.claude/settings.json` hook 5) blocks bare XREFs but cannot detect broken ones across files. Two incidents in April 2026: TODO-02 §1 stamp needed post-commit correction ("false README link claim"), and §3 Deferred line shipped pointing at line 142 when the real item was at line 127.

## Inputs

- [`todo/`](../../todo/) -- every `.md` file under this tree is a TODO and becomes a node in the graph
- [`todo/TODO-00-INDEX.md`](../TODO-00-INDEX.md) -- root roadmap index; generator reads it to cross-check domain ordering
- [`todo/*/INDEX.md`](../) -- per-domain indexes; each TODO must appear in exactly one
- [`scripts/`](../../scripts/) -- host tooling home for the new generator/validator/query scripts
- [`.github/workflows/build.yml`](../../.github/workflows/build.yml) -- CI hook goes here (§6)
- [`.claude/skills/validate-todo-file/SKILL.md`](../../.claude/skills/validate-todo-file/SKILL.md) -- existing structural validator; §3 extends it with graph checks it cannot do alone
- [`.claude/skills/gap-analysis-todo/SKILL.md`](../../.claude/skills/gap-analysis-todo/SKILL.md) -- existing graph walker in prose; §4 gives it a machine-readable backend
- [`.claude/settings.json`](../../.claude/settings.json) -- hook 5 blocks bare Accepted/Deferred XREFs at commit; §3 validator extends the check to cross-file XREF integrity
- [`docs/infrastructure/ai-system.md`](../../docs/infrastructure/ai-system.md) -- architectural parallel: settings.json canonical, Hook Routing Matrix derived. §1 applies the same invariant to TODO files.
- -> XREF: [`00-infrastructure/TODO-01 §7 Tooling Doctor and Regression Pack`](TODO-01-developer-tooling-stack.md#7-tooling-doctor-and-regression-pack) -- host-side tooling regression pattern to mirror for §3 validator
- -> XREF: [`00-infrastructure/TODO-02 §9 AI Workflow Regression Suite`](TODO-02-ai-development-system.md#9-ai-workflow-regression-suite) -- complement (checks doctrine presence; this TODO checks cross-TODO graph integrity). Both ship as part of a repo-wide TODO/AI regression story.

## Outcome

- Stable semantic ID in every TODO file's YAML frontmatter: `id`, `domain`, `status`, `owners`, `depends_on`, optional `satisfies`. File renumbering / renaming stops breaking inbound XREFs.
- `scripts/todo-graph/build.py` generator: parses every `todo/**/*.md`, extracts frontmatter + Implementation Order rows + Accepted/Deferred XREFs + Inputs XREFs, emits `build/todo-cache.json` (derived, gitignored). Fast (under 2s for ~86 files).
- `scripts/todo-graph/validate.py` validator: reports stale XREFs (target id doesn't exist), dangling `depends_on`, orphaned Implementation-Order rows (table entry but no body section), and dependency cycles. Same `t_pass` / `t_fail` pattern as `scripts/test-tooling.sh`.
- `scripts/todo-graph/query.py` CLI: `todo-graph ready` (no blockers, not in progress), `todo-graph blocked`, `todo-graph by-domain`, `todo-graph backlinks <id>`, `todo-graph orphans`, `todo-graph diff <id>` (what a stamp actually references).
- CI gate wired into `.github/workflows/build.yml`: validator runs on every push / PR that touches `todo/`; stale-XREF or cycle fails the build with actionable output.
- Migration complete: all ~86 existing TODOs carry frontmatter; existing XREFs rewritten to use stable IDs where it shortens (`DNN TNN §N` still allowed for in-prose brevity).

## Implementation Order

| ⭐  | Order | Section | Deliverable                                              | Depends On      | Status |
| --- | :---: | :-----: | -------------------------------------------------------- | --------------- | :----: |
| 💎  |   1   |  §1     | Frontmatter schema + spec doc                            | --              |  [ ]   |
| 💎  |   2   |  §2     | `scripts/todo-graph/build.py` generator + cache format   | §1              |  [ ]   |
| 💎  |   3   |  §5     | Back-fill frontmatter across existing 86 TODO files      | §1, §2          |  [ ]   |
| ⭐  |   4   |  §3     | Validator: stale XREF / dangling dep / orphan / cycle    | §2              |  [ ]   |
| ⭐  |   5   |  §4     | Query CLI: ready / blocked / by-domain / backlinks       | §2              |  [ ]   |
| ⭐  |   6   |  §6     | CI gate on `todo/` commits + repo-local make target      | §3, §4          |  [ ]   |

> 💎 = parity work -- Linux kernel has MAINTAINERS + get_maintainer.pl (person-ownership mapping without a dep graph); Windows has no public equivalent. §1-§3 (frontmatter + generator + migration) bring us to partial Linux parity plus graph metadata neither OS ships.
> ⭐ = competitive edge -- neither Win11 nor mainline Linux ships a first-class TODO dependency graph. §3-§6 are new ground; the surface has direct value for any contributor scanning "what can I work on next?".
> **Order vs section-number:** Implementation Order is the execution sequence; file-order numbering (§1 schema, §2 generator, §3 validator, §4 query, §5 migration, §6 CI) is grouped by kind so readers of the file can scan logically. §5 (migration) ships at Order 3 because §3 (validator) is easier to debug against a repo where all files already carry frontmatter.

---

## 1. Frontmatter Schema and Spec

Define the YAML frontmatter block that every TODO file MUST carry going forward. This is the foundation: every other section depends on the schema being stable and minimal.

- [ ] Draft the schema in [`docs/infrastructure/todo-metadata.md`](../../docs/infrastructure/todo-metadata.md) (new doc). Required fields: `id` (kebab-case, globally unique across all domains), `domain` (directory short name, e.g. `00-infrastructure`), `status` (`draft` / `active` / `blocked` / `done` / `superseded`), `title` (human-readable short name). Optional: `owners` (list of handles or roles), `depends_on` (list of ids; structural deps only), `satisfies` (list of ids; closes-when-done mapping for auto-closure propagation), `superseded_by` (single id; set when status is `superseded`).
- [ ] Specify parsing rules: YAML between first two `---` fences at the top of the file (standard Jekyll/Hugo pattern); parser MUST tolerate absence (back-compat during migration) and MUST reject malformed YAML with a clear error naming the file + line. File is still a valid TODO without frontmatter; just doesn't participate in the graph until migrated.
- [ ] Specify id naming rules: lowercase, `[a-z0-9-]+`, max 60 chars, must start with a letter, must not end with `-`. Examples: `ai-development-system`, `kernel-test-harness`, `todo-metadata-layer` (this file). Renaming an id is allowed but requires a grep sweep to rewrite inbound `depends_on:` / `satisfies:`; validator catches dangling refs.
- [ ] Specify the two XREF-in-prose forms that remain allowed and how the parser handles them: (a) `DNN TNN §N` compact form (scannable in tables); (b) `(item: "NAME" at line N)` parenthetical in stamps. Both stay human-authored; the machine graph uses `id` references. The validator looks up file-by-number for the prose forms to detect drift.
- [ ] Example frontmatter block for this TODO itself (authoritative sample):
  ```yaml
  ---
  id: todo-metadata-layer
  domain: 00-infrastructure
  status: active
  title: TODO Metadata Layer and Derived Graph
  depends_on: []
  ---
  ```
- [ ] Commit: `"docs/todo: define frontmatter schema for TODO graph metadata"`

**Test checkpoint:** A human reading `docs/infrastructure/todo-metadata.md` can author a new TODO's frontmatter without guessing. The schema doc names all required + optional fields with examples, parsing rules, and id naming constraints. No code yet -- pure spec.

## 2. Generator and Cache Format

Build the parser that reads every TODO, extracts graph data, and emits a read-only cache. Markdown remains canonical; the cache is regenerated on demand and gitignored.

- [ ] Create `scripts/todo-graph/build.py`: walks `todo/**/*.md`, parses frontmatter + Implementation Order table rows (section number, deliverable, depends-on, status) + Inputs XREFs (`-> XREF: ...`) + Accepted/Deferred stamp XREFs. Emits `build/todo-cache.json` (schema below).
- [ ] Cache schema: JSON array of node objects. Each node: `{id, domain, status, title, file_path, sections: [{n, deliverable, depends_on: ["§N"...], status}], inputs_xrefs: [{target_id, target_section}], stamps_xrefs: [{kind: "accepted"|"deferred", severity, target_id, target_section, item_name?}]}`.
- [ ] Add `scripts/todo-graph/` to `.gitignore` for the `build/todo-cache.json` output only; keep the scripts themselves tracked.
- [ ] Build must be fast: under 2s wall-clock for the current ~86 TODO files on a dev machine. Use stdlib-only Python (no yaml package dep; hand-roll a YAML parser for the limited frontmatter subset we support, or require `python3-yaml` in `scripts/setup-deps.sh`).
- [ ] Back-compat: files without frontmatter parse into a node with `id = null`, `status = "no-frontmatter"`; the validator surfaces these but doesn't fail on them during the migration window (§5).
- [ ] Exit code: 0 on clean parse of all files, 1 on any file's frontmatter malformed (with file + line). Generator never edits TODO files.
- [ ] Hand-verify the cache against the repo: count of nodes == count of `todo/**/*.md` (excluding `INDEX.md`, `TODO-00-INDEX.md`); every node has a non-empty `sections` list (every TODO has at least §1 or a top-level Commit).
- [ ] Commit: `"scripts/todo-graph: add cache generator (frontmatter + XREF extraction)"`

**Test checkpoint:** `python3 scripts/todo-graph/build.py` succeeds in under 2s, emits `build/todo-cache.json` parseable by `python3 -c 'import json; json.load(open("build/todo-cache.json"))'`. Node count matches `find todo -name 'TODO-*.md' | wc -l`. Running twice with no source changes produces byte-identical output.

## 3. Validator (stale XREF / dangling dep / orphan / cycle)

The validator consumes the cache and reports graph-integrity violations. This is what closes the gap the pre-commit hook 5 cannot: cross-file XREF integrity.

- [ ] Create `scripts/todo-graph/validate.py`: loads `build/todo-cache.json` (or rebuilds if missing), runs four checks:
  1. **Stale XREF**: every `depends_on`, `satisfies`, Inputs XREF, and Accepted/Deferred XREF must resolve to an existing `id` in the cache. Flag `file_path + kind + bad_ref` for each failure.
  2. **Dangling section ref**: every `§N` in an Implementation Order `Depends On` column must resolve to an actual `## N.` heading in the same file (catches renumbering drift).
  3. **Orphaned Implementation Order row**: every row must have a matching `## N.` body section; every body section must have a matching row. Missing = drift on either side.
  4. **Dependency cycle**: build the `depends_on` digraph; run DFS; report any cycle found. A cycle indicates misplanned deps and must be broken manually.
- [ ] Output format: `t_pass` / `t_fail` pattern matching `scripts/test-tooling.sh`. Each failure prints `[FAIL] <check_name>: <file>: <detail>`. Trailing summary line: `N passed, M failed`.
- [ ] Exit code: 0 if M == 0; 1 otherwise (so CI can gate on it). `--warnings-only` flag flips stale-XREF failures to warnings during migration.
- [ ] Pair with `scripts/todo-graph/validate.py --fix-line-numbers`: re-resolves `(item: "NAME" at line N)` parentheticals in stamps by looking up the current line of the named item. Only touches stamps where the ID + item-name still match; never invents a new item.
- [ ] Include a smoke run against the current repo state: document the expected failure set post-migration (should be 0 stale XREFs, 0 cycles, known-count of legacy line-number drift in old stamps).
- [ ] Commit: `"scripts/todo-graph: add validator (stale XREF, dangling dep, orphan, cycle)"`

**Test checkpoint:** `python3 scripts/todo-graph/validate.py` on a clean repo exits 0 with `0 failures` summary. Artificially break an XREF (edit one `depends_on:` to reference a nonexistent id), validator exits 1 and prints the exact file + bad ref. `--fix-line-numbers` corrects a deliberately stale `(item: "..." at line N)` parenthetical back to the real line without changing any item names.

## 4. Query CLI (ready / blocked / by-domain / backlinks / orphans)

Human-facing commands that answer "what can I work on?" and "what references this?" without manual grep sweeps.

- [ ] Create `scripts/todo-graph/query.py` with subcommands:
  - `todo-graph ready`: prints every TODO whose `status == draft` AND all `depends_on` are `status == done`. Sorted by domain. Each line: `<domain> <id> <title>`.
  - `todo-graph blocked`: prints every TODO whose `status == active` with at least one `depends_on` that is not `done`. Same format plus `blocked by: <ids>`.
  - `todo-graph by-domain [<domain>]`: lists all TODOs grouped by domain; each entry shows status + first unfinished section number.
  - `todo-graph backlinks <id>`: prints every TODO that references `<id>` via `depends_on`, `satisfies`, Inputs XREF, or Accepted/Deferred stamp. Key signal for "if I change this TODO, what else is affected?"
  - `todo-graph orphans`: prints every TODO that no other TODO references (no inbound edges of any kind). Candidates for retirement or for missing-XREF audit.
- [ ] `--json` flag on every subcommand emits structured output for scripting; default is compact tab-separated human output.
- [ ] `--format markdown` on `by-domain` emits a copy-pasteable markdown table; useful for PR descriptions.
- [ ] Keep stdlib-only; query scripts must run on a dev machine without extra Python installs.
- [ ] Link from [`.claude/skills/gap-analysis-todo/SKILL.md`](../../.claude/skills/gap-analysis-todo/SKILL.md): cross-TODO overlap scan (step 12) can consult `todo-graph backlinks` instead of hand-grepping. Add a one-line pointer in the gap-analysis skill.
- [ ] Commit: `"scripts/todo-graph: add query CLI (ready, blocked, by-domain, backlinks, orphans)"`

**Test checkpoint:** `todo-graph ready` prints a non-empty list on a repo with at least one ready TODO. `todo-graph backlinks ai-development-system` lists every TODO that references it (expected: at least this TODO via `depends_on`, plus any other explicit XREFs). `todo-graph orphans` returns files that are genuinely unreferenced (should be a short list on a well-linked repo).

## 5. Migration: Back-Fill Frontmatter on Existing TODOs

One-time sweep that adds frontmatter to every existing TODO file without disturbing the content below. Mechanical, scripted, reviewable.

- [ ] Create `scripts/todo-graph/migrate-add-frontmatter.sh`: walks `todo/**/*.md`, for each file missing frontmatter inserts a stub block at the top derived from the existing filename (`TODO-02-ai-development-system.md` -> `id: ai-development-system`), the first `## ` heading (`title`), and the parent directory (`domain`). Sets `status: active` by default; doesn't infer `depends_on` (humans do that in a follow-up pass).
- [ ] Dry-run mode: `--dry-run` prints the proposed diff without writing. Review-first workflow.
- [ ] Idempotent: running twice is safe; skips files that already have frontmatter.
- [ ] Coverage matrix: land the migration in logical batches by domain (14 domains). Each batch = one commit. Commit message names the domain + file count. Makes review-on-PR tractable.
- [ ] Post-migration audit: `todo-graph validate` must report 0 "no-frontmatter" nodes once the migration completes. If any file intentionally stays frontmatter-less (e.g. `INDEX.md` files), explicitly opt it out via a pattern list in the migration script.
- [ ] After migration completes, drop the `no-frontmatter` back-compat path in the generator (§2); future writes must carry frontmatter. The `create-todo` skill already uses `TEMPLATE.md`; extend the template with the frontmatter block.
- [ ] Commit: `"todo: back-fill frontmatter across 14 domains (migration complete)"` (final commit; individual batches get per-domain commit messages)

**Test checkpoint:** After migration, `find todo -name 'TODO-*.md' -not -name 'TODO-00-INDEX.md' | xargs -I{} head -1 {} | sort -u` shows only `---` (every TODO starts with frontmatter). `todo-graph validate` reports 0 "no-frontmatter" failures. Running the migration script a second time produces zero diff (idempotent).

## 6. CI Gate and Make Target

Wire the validator into the repo's CI surface so graph drift is caught before humans hit it manually. Mirrors the `scripts/test-tooling.sh` pattern per [TODO-01 §7](TODO-01-developer-tooling-stack.md#7-tooling-doctor-and-regression-pack).

- [ ] Add `scripts/todo-graph/build-and-validate.sh`: one-liner that runs `build.py` then `validate.py`. Exit code is the validator's; the intermediate `build/todo-cache.json` is consumed then optionally deleted (`--keep-cache` flag).
- [ ] Add `make todo-graph` Makefile target that invokes the above. `make todo-graph-ready` / `make todo-graph-blocked` for the common queries.
- [ ] Extend `.github/workflows/build.yml` with a new job step: runs `bash scripts/todo-graph/build-and-validate.sh` on every push / PR that touches `todo/**/*.md`. Uses `paths:` filter so unrelated commits don't burn CI minutes.
- [ ] Job output: pass/fail summary in the GitHub Actions log; the validator's `t_fail` lines surface directly. PR comment (optional, via a small action): posts the failing XREFs inline on PRs that introduce them.
- [ ] Wire a pre-push opt-in in [`.githooks/pre-push`](../../.githooks/pre-push) (owned by TODO-01 §5): run `bash scripts/todo-graph/build-and-validate.sh` alongside the existing build+test gate. Document in `scripts/install-hooks.sh --status`.
- [ ] Commit: `"ci/todo-graph: gate todo/ commits on graph validation"`

**Test checkpoint:** `make todo-graph` on a clean repo exits 0. A PR that introduces a stale XREF fails the CI `todo-graph` step with a clear failure line naming the file and bad ref. Disabling the pre-push opt-in and pushing a bad ref still fails at PR time via the Actions workflow.

---

## OS Comparison

| ⭐ | Feature                              | 🪟 Win11                          | 🐧 Linux                                 | 🚀 Impossible OS                           |
| --- | ------------------------------------ | --------------------------------- | ---------------------------------------- | ------------------------------------------ |
| 💎 | Structured ownership metadata        | ⚠️ CODEOWNERS (GitHub)           | ✅ MAINTAINERS + get_maintainer.pl       | ⬜ §1 frontmatter + §5 migration            |
| ⭐ | Cross-file dependency graph          | ❌ GitHub Projects (external DB) | ❌ Ad hoc (e.g. patch series cover)      | ⬜ §2 generator + JSON cache                |
| ⭐ | Stale-XREF / cycle validator         | ❌ None                           | ❌ None                                  | ⬜ §3 validator (four checks)              |
| ⭐ | "Ready to work" / backlinks queries  | ❌ Project board filters (manual) | ❌ None in-tree                          | ⬜ §4 query CLI                             |
| 💎 | CI gate on dep-graph integrity       | ⚠️ Varies by repo                | ❌ Rare                                  | ⬜ §6 GitHub Actions step                   |
| ⭐ | Canonical-markdown + derived-cache invariant | ❌ DB-first (Project boards)      | ❌ Flat MAINTAINERS (no derived view)    | ⬜ §1-§2 mirrors settings.json vs ai-system.md pattern |

> **After §1-§3:** Impossible OS has full Linux-parity ownership metadata plus the dep-graph that neither OS ships, plus automated cross-file XREF integrity checks.
> **After §4-§6:** "what should I work on next?" is a one-command query, and graph drift is caught at PR time instead of at next-reviewer-sweep time. The canonical-markdown / derived-cache invariant matches the existing [Hook Routing Matrix](../../docs/infrastructure/ai-system.md#hook-routing-matrix) architecture, so contributors already understand the mental model.

## Unit Tests

> Host-side tooling -- no kernel `test_runner_init()` wiring. Tests are bash + python scripts co-located with the generator/validator under `scripts/todo-graph/tests/`.

- [ ] Create `scripts/todo-graph/tests/test_build.sh`:
  - Generator exits 0 on a repo with valid frontmatter across all files.
  - Generator exits 1 with a clear file + line message on a single deliberately-malformed frontmatter.
  - Running twice produces byte-identical cache (determinism).
  - Cache node count matches `find todo -name 'TODO-*.md' -not -name 'TODO-00-INDEX.md' | wc -l`.
- [ ] Create `scripts/todo-graph/tests/test_validate.sh`:
  - Clean repo produces `0 failures`.
  - Artificially broken XREF (sed a `depends_on:` to a nonexistent id) produces exactly 1 stale-XREF failure.
  - Artificially created cycle (A depends on B depends on A) produces a cycle failure naming both ids.
  - `--fix-line-numbers` on a deliberately drifted parenthetical corrects to the real line and does not touch any other content.
- [ ] Create `scripts/todo-graph/tests/test_query.sh`:
  - `ready` returns a non-empty list on a fixture repo with at least one ready TODO.
  - `backlinks <id>` returns the expected set for a fixture with known cross-refs.
  - `orphans` returns the expected set (single known-unreferenced fixture).
- [ ] Wire tests into [`scripts/test-tooling.sh`](../../scripts/test-tooling.sh) via a new `test_todo_graph()` function; runs under `make test-tooling`.
- [ ] Commit: `"test/todo-graph: add generator + validator + query tests"`

> **Test runner:** `bash scripts/test-tooling.sh` (group: `test_todo_graph`) | expected: all `t_pass`, 0 `t_fail`. For host-side tests this replaces the kernel `scripts/debug/run-<cat>-tests.bat` pattern; the test surface is shell/python, not kernel C.

## Verification

- [ ] `bash scripts/todo-graph/build-and-validate.sh` on a clean repo exits 0.
- [ ] `make todo-graph-ready` prints a non-empty list on a repo where at least one TODO is in `draft` state with satisfied deps.
- [ ] `python3 scripts/todo-graph/query.py backlinks ai-development-system` returns this TODO (because it lists `ai-development-system` in `depends_on`), plus any other inbound XREFs.
- [ ] A PR that introduces a stale `depends_on:` value fails the CI `todo-graph` workflow step with a clear `[FAIL]` line.
- [ ] Running the migration script after migration completes produces zero diff (idempotent).
- [ ] Host-side only -- no kernel boot impact. Not applicable for QEMU WHPX / TCG / VirtualBox / bare metal verification.
- [ ] Commit: `"todo-graph: complete"` (only after every section ships and CI is green for a week)
