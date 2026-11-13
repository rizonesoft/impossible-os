<!-- docs: covers=todo/00-infrastructure/TODO-06-todo-metadata-layer.md sources=scripts/todo-graph/build.py,scripts/todo-graph/validate.py,scripts/todo-graph/cache_schema.py,scripts/todo-graph/build-and-validate.sh,scripts/lint.sh,scripts/todo-graph/tests/test_build.sh,scripts/todo-plan.py reviewed=2026-11-11 -->
# TODO Metadata Layer -- Frontmatter Spec

> Canonical spec for the YAML frontmatter block that every TODO file under `todo/**/*.md` carries (post-migration). The frontmatter is the stable identity layer the [`scripts/todo-graph/build.py`](../../scripts/todo-graph/build.py) generator parses into `build/todo-cache.json`, decoupling cross-TODO references from filename-based lookups (which break silently on renumbering).
>
> **Two-layer doctrine:** the canonical authority for what's allowed is this human-readable spec PLUS the machine-readable JSON Schema sidecar at [`docs/infrastructure/todo-metadata.schema.json`](todo-metadata.schema.json). The generator implements both. The `$schema:` field inside each TODO's frontmatter is metadata that programmatic validators (the §2 generator, `remark-lint-frontmatter-schema` in §6 CI, ad-hoc `python3 -c "import jsonschema; jsonschema.validate(...)"` calls) follow to locate the sidecar; it is NOT auto-detected as a schema directive by the stock Red Hat YAML language server (which expects `# yaml-language-server: $schema=...` modeline comments or a workspace `yaml.schemas` mapping, neither of which natively reads markdown-embedded YAML). Editor-time inline diagnostics for TODO frontmatter therefore require either (a) a markdown-frontmatter-aware lint integration like [`remark-lint-frontmatter-schema`](https://github.com/JulianCataldo/remark-lint-frontmatter-schema) wired through `remark-cli` in CI and an editor's "lint on save" hook, or (b) a VS Code extension that hooks the Red Hat YAML server into markdown YAML frontmatter blocks (the one this page used to cite, `joaompinto.vscode-yaml-frontmatter`, was removed from the Visual Studio Marketplace; a 2026-09-29 check returned 404). The §6 CI gate is the authoritative line of defense; editor-time diagnostics are best-effort developer convenience.

## Scope

- **Owns:** the schema specification (this doc) + the JSON Schema sidecar + the canonical authoritative example.
- **Does NOT own:** the generator parser (lives in `scripts/todo-graph/build.py`), the validator's graph-integrity checks (validator section of the TODO-metadata-layer plan), the back-fill migration that adds frontmatter to existing TODOs (migration section of the same plan).

## Frontmatter Block Shape

YAML between the first two `---` fences at the top of every TODO file (Jekyll/Hugo convention). The opening `---` MUST be the first three bytes of the file (after an optional UTF-8 BOM, which the parser tolerates and strips). The closing `---` MUST be on its own line.

```yaml
---
$schema: ../../docs/infrastructure/todo-metadata.schema.json
schema_version: 1
id: todo-metadata-layer
domain: 00-infrastructure
status: active
title: TODO Metadata Layer and Derived Graph
depends_on: []
---
```

Everything between the two fences is parsed as YAML; everything after the closing fence is the human TODO content (markdown body) and is parsed independently for Implementation Order rows, Inputs XREFs, and Accepted/Deferred stamps.

## Required Fields

Every field below is required; absence is a `missing-required` error and the generator refuses to emit a cache entry for the offending file (cache build fails closed).

| Field            | Type    | Constraint                                                                                | Example                              |
| ---------------- | ------- | ----------------------------------------------------------------------------------------- | ------------------------------------ |
| `schema_version` | integer | Starts at `1`. Bumps on a NON-additive change (rename, removal, semantic shift). Adding a new optional field does NOT bump it.                                                                            | `1`                                  |
| `id`             | string  | Globally unique across all domains. Regex `^[a-z][a-z0-9-]{0,58}[a-z0-9]$` (lowercase letter start, kebab-case, 2-60 chars, no trailing dash).                                                            | `kernel-test-harness`                |
| `domain`         | string  | Directory short name (folder under `todo/`). Matches the file's parent directory by convention but is recorded explicitly so the cache schema is self-describing.                                          | `00-infrastructure`                  |
| `status`         | enum    | One of: `draft`, `active`, `blocked`, `done`, `superseded`. Validator gates `status: done` against every Implementation Order row being `[x]`.                                                            | `active`                             |
| `title`          | string  | Human-readable short name. May differ from the H1 heading for length reasons (the H1 is preserved for in-page display).                                                                                    | `TODO Metadata Layer and Derived Graph` |

## Optional Fields

Optional fields surface as cache entries when present and as `unknown-field` warnings when an unrecognized field appears (forward-compat: a newer generator may know fields an older spec doesn't). Unknown optional fields do NOT fail the build.

| Field             | Type      | Purpose                                                                                                                                            |
| ----------------- | --------- | -------------------------------------------------------------------------------------------------------------------------------------------------- |
| `$schema`         | string    | Relative path to the JSON Schema sidecar. Programmatic validators (generator + CI lint + ad-hoc `jsonschema.validate`) follow it to load the sidecar. NOT auto-detected by the stock Red Hat YAML LS in markdown frontmatter (see Two-layer doctrine note above for editor wiring). Recommended to set even though it's optional. |
| `owners`          | string\[] | Free-form list of handles, roles, or team names. NOT enforced; advisory.                                                                            |
| `depends_on`      | string\[] | Structural dependencies on other TODO ids. The validator builds a digraph from this field and rejects cycles.                                       |
| `satisfies`       | string\[] | List of ids that this TODO closes when its `status` flips to `done`. Powers auto-closure propagation in the validator (gated by full acceptance).   |
| `superseded_by`   | string    | Single id. Set when this TODO's `status` is `superseded`. Validator surfaces a chain when traversing supersession.                                  |
| `file_patterns`   | string\[] | Glob patterns marking source paths this TODO owns. Linux MAINTAINERS-style `F:` parity. Powers the `code <id>` query (query-CLI section).            |
| `effort`          | string    | Gantt row duration: whole days or whole weeks, matching `\A[1-9][0-9]*[dw]\Z` (`3d`, `2w`). Absent means the renderer's `1w` default; a **present-but-invalid value fails the build** rather than defaulting, because a silent default hides a typo behind a plausible schedule. Deliberately narrower than Mermaid's full duration grammar -- every unit it rejects is one that cannot reach the emitted `gantt` line. |

### Which dependency evidence the readiness verbs consult

`ready`, `blocked` and `blocking` (and `stats`, which reuses `blocking`) rank against the **file-level `depends_on` frontmatter key above, and nothing else**. That is a pinned decision, not an oversight, and it is worth stating because zero of the 232 live TODOs currently author the field -- so `blocked` and `blocking` return empty for every possible corpus and `stats.avg_dep_depth` is `0.0`. `query.py` prints a stderr note saying so whenever the edge set is empty, so an empty ranking cannot be mistaken for "nothing is blocked". The note is suppressed by `--quiet` and never touches stdout, so `--json` consumers are unaffected.

The cache also carries `sections[].depends_on` -- 2,529 dependency groups parsed from the Implementation Order tables, far richer evidence. Redirecting the verbs at it was evaluated and rejected on two measurements:

- **1,806 of the 2,529 groups target `self`.** A mechanical derivation either creates self-dependencies or, once those are filtered, promotes "depends on section 3 of that file" into "depends on all of that file" -- a widening nobody authored.
- **The two fields are different namespaces.** Frontmatter `depends_on` holds TODO **ids** (`^[a-z][a-z0-9-]{0,58}[a-z0-9]$`); section groups hold unqualified filename stems. Of the 145 distinct cross-file targets, **26 name a bare `TODO-NN` that matches more than one file** (`TODO-01` matches 18, one per domain). So this is not a source swap but an unresolved-reference problem.

Qualified target resolution is owned by [Domain-Code Resolution Picks a Directory by Cache Order](../../todo/00-infrastructure/TODO-06-todo-metadata-layer.md#26-domain-code-resolution-picks-a-directory-by-cache-order). Section-aware readiness verbs, if wanted, belong on top of that as explicitly new commands with stated aggregation rules -- never by reinterpreting what the existing three answer.

That resolution landed, and the verbs below are those new commands. The three above are unchanged and still rank against the file-level field, so the stderr note stays: its subject -- an empty file-level edge set -- is still true. It now names which verb to use instead.

### Section-level readiness: the aggregation rule

`section-ready`, `section-blocked` and `section-blocking` rank against `sections[].depends_on`. They are separate commands rather than a new behaviour for `ready` / `blocked` / `blocking`, because an MCP agent or a `--json` consumer cannot distinguish a semantics change from a corpus change. Every row carries a `dep_source` column naming the graph it came from.

A section-level edge says "this file's section N depends on that file's section M", and compressing that into a verdict is the whole design. The rule, in the order the code applies it:

All counts below were measured on 2026-08-09 against the live corpus (232 nodes, 2,533 dependency groups). They are dated observations that justify a design choice, not invariants -- editing any TODO moves them.

- **`self` groups are excluded from the cross-file edge set at construction, and evaluated separately.** 1,810 of the 2,533 groups target `self`; they are intra-file ordering, so admitting them to the rank would answer a different question. They are still prerequisites, though: `section-ready` requires a section's own `self` dependencies to be satisfied too. Ranking on cross-file edges alone reported 33 ready rows, **20 of which had an unmet prerequisite inside their own file** -- ready by the letter of the edge set and unworkable in fact.
- **A target that resolves back to the source file is treated as `self`, whatever it was called.** The literal token is not the only way to name your own file: 8 live groups write the file's own code (`TODO-04`, `TODO-05`) where they mean `self`. Left in the cross-file set they satisfy `section-ready`'s cross-file eligibility test with a same-file edge and show up in `section-blocking` as pressure no other file applied. So 723 is the count of NON-LITERAL-`self` CANDIDATE groups; **715 are actually cross-file** once the aliases are routed. An alias with an empty section list still means the whole source file, and iterating its section numbers alone silently dropped it -- 3 of the 8 are that shape.
- **Targets resolve through `resolve_xref_target`**, the same resolver the domain-code resolution section shipped. `None` means unknown *or* ambiguous, and both are unmet (`unresolved`). An ambiguous bare stem is never bound to an arbitrary file.
- **Satisfaction is decided by the canonical lifecycle classifier, never by the raw `[x]` / `[/]` marker.** `query.py` imports `classify_section` from `.claude/hooks/sequencer_triage.py` rather than re-deriving it. This is not a refinement: **42 sections carry a raw `[x]` the oracle classifies NEEDS_WORK** (shipped but not yet stamped Verified + Quality-reviewed) and **421 carry a raw `[/]` it classifies DONE** (a Deferred park, or fully stamped). Ranking on the marker would disagree with the runner about what is runnable and what is finished, on 463 sections.
- **A source section must classify NEEDS_WORK to be `section-ready`, not merely "not DONE".** A recoverable `awaiting-*` deferral classifies BLOCKED, and treating every non-DONE class as runnable points the verb at work that is explicitly parked on something external. The live corpus happens to contain no such source today; the state is reachable and fixture-covered rather than hypothetical.
- **A park whose question is answered is open work, and a park on a person or a machine names its Need.** A Deferred stamp tagged `awaiting-answer Qn` whose questions all have an answer in `todo/answers.md` makes its section NEEDS_WORK, even one that carries Verified and Quality-reviewed (`sequencer_triage.classify_section`, the `DA` stamp kind), and the staged-commit check accepts it, reading the answer from the same staged snapshot as the park. A stamp that waits on an operator, real hardware or the receipt-surface guard deleted on 2026-11-09 is listed by `scripts/overnight/stale_parks.py --check`, and the run cannot reach its end state while one has no `**Needs:**` line on its section (the guard's parks are listed even with one). Re-judged 2026-11-13: 38 parks, 27 sections reopened under Q42 and Q43, eight given `**Needs:** bare-metal`.
- **A group with an empty section list names the whole target file**, and only there is a file-level verdict used -- again the classifier's `classify_file`, not the node's `status` field. 279 of the 715 cross-file groups are this shape.
- **A token can resolve to a real path that is not a TODO node.** Every domain carries an `INDEX.md`, and 8 live groups resolve to one. That is its own unmet reason (`non-node`) so the state machine has an outcome instead of crashing on the node lookup, and the label keeps the domain (`08-graphics-ui/INDEX`) because a bare `INDEX` names 15 files.
- **An Implementation Order row with no section number keeps a distinct identity, it is not dropped.** The cache contract permits a null `sections[].n` and does not require nulls to be unique, so keying on `(file_path, n)` alone let two such rows overwrite each other's lifecycle class and merge their edges. They are keyed by row ordinal instead. Excluding them was the first attempt and it was wrong the other way: being unable to NAME a source is no reason to forget what it points at, and dropping it understated `section-blocking` just the same. Such a row reports a null section number and still contributes its outbound edges.
- **`section-blocking` counts distinct `(source_file, source_section, target_file, target_section)` edges.** [Release Artifacts](../../todo/15-installer-release/TODO-01-release-artifacts.md) already declares one target section twice in a single group (`sections: [6, 6]`), which a naive loop scores as two dependents for one dependent.
- **`section-ready` requires at least one cross-file group.** Without that bound the answer is "every open section that declares nothing", which is most of the corpus. The question the verb answers is "what was waiting on another file and no longer is".

On that corpus the 715 cross-file groups expand to 790 deduplicated cross edges, of which 415 are unmet from a non-DONE source; `section-ready` returns 13 rows and `section-blocking` ranks 248 target sections. The file-level `blocking` returns 0 for the same corpus.

The unmet reasons are reported separately rather than collapsed into "blocked", because they are not interchangeable: `open` and `blocked` are ordinary pending work, while `unresolved`, `non-node` and `dangling` are corpus defects wearing the shape of a dependency. Collapsing them would hide those defects exactly as the empty edge set used to.

## Auto-Derived Fields (Cache-Only, NOT Hand-Authored, NOT in Frontmatter)

These fields are computed by the generator from `git log` and emitted into `build/todo-cache.json` ONLY. They MUST NOT appear in hand-authored frontmatter; the JSON Schema sidecar enforces this with a `not/anyOf/required` clause that REJECTS frontmatter containing either field. The fields are documented here so the cache schema is self-describing and downstream consumers (`scripts/todo-graph/validate.py`, `query.py`) know what to expect when reading the cache.

| Field             | Type    | Source                                                                                                                                          |
| ----------------- | ------- | ----------------------------------------------------------------------------------------------------------------------------------------------- |
| `created_at`      | string  | ISO 8601 UTC. First-commit timestamp for the file (`git log --reverse --format=%ct -- <file>`, taking the first hit). Cache-only.                |
| `last_active_at`  | string  | ISO 8601 UTC. Last-commit timestamp for the file (`git log -1 --format=%ct -- <file>`). Cache-only.                                              |

The generator uses a single batched `git log -- todo` call to fill these for every file in one pass; the per-file fork overhead is amortized.

## The Corpus Binding (`build/todo-cache.json.corpus-<digest>.json`)

Every published cache is accompanied by a **corpus binding**: a small JSON sidecar recording the sha256 of each `todo/**/TODO-*.md` the generator consumed, the id of the corpus git history those auto-derived fields came from, and the sha256 of the cache bytes it describes. Readers go through [`scripts/todo-graph/cache_schema.py`](../../scripts/todo-graph/cache_schema.py) `check_freshness`, which locates the binding by the digest of the cache bytes it actually parsed and refuses (`STALE`) unless the recorded corpus and history still match what is on disk.

It replaced an mtime comparison ("is any TODO newer than the cache?"), which was a proxy resolved against the wall clock -- and therefore invertible by a backward clock step, which is what made one identity-gate fixture fail roughly 1 run in 4 on an unchanged tree. The binding also closes the producer's own generation window: `build.py` fingerprints the bytes it parsed, re-verifies them before publishing, and exits **3** without writing anything if the corpus moved underneath it, leaving any previous cache byte-identical.

The binding also carries the **producer contract** the cache was written against, as two fields that answer different questions:

| Field                       | Purpose                                                                                                                                                                                                                                                        |
| --------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `cache_format_version`      | Hand-bumped integer, for a **semantic** change -- a field that keeps its name and type and changes its meaning, which no digest can detect. Distinct from the per-node `schema_version`, which is the TODO frontmatter's own generation.                          |
| `producer_contract_digest`  | sha256 over `cache_schema.EMITTED_NODE_FIELDS` together with the version, so a **structural** change (field added, removed, renamed) invalidates old artifacts even when the version bump is forgotten. Taken over the declaration, never over an observed cache's keys -- hashing output would let a node-shape regression re-certify itself. |

`build.py` refuses to publish a node carrying any key outside `EMITTED_NODE_FIELDS`, so the declaration the digest certifies cannot drift from what is actually emitted. Readers verify both whenever a binding is present, and refuse with `LEGACY_FORMAT` on a mismatch; `validate.py --diff` additionally **requires** the binding, because its baseline is read with freshness disabled and would otherwise be trusted on shape alone -- a field that kept its type and changed its meaning would pass and produce graph deltas nobody authored. A refused baseline is left byte-identical: it is somebody else's artifact, so the recovery path never reaches it.

Operational notes:

- The binding is **immutable and named for the cache digest**, so it can be written before the cache is atomically replaced; whichever generation survives a crash has its own binding beside it. `build.py` prunes the non-current ones after each successful publish.
- A cache **copied without its binding is refused**, not accepted -- absence of evidence is a rebuild condition. Rebuild with `bash scripts/todo-graph/build-and-validate.sh --keep-cache`.
- Both files are derived build artifacts under `build/` and are gitignored.

## Reader Profiles and Fail-Closed Behavior

A reader declares what it CONSUMES via a `cache_schema.Profile`, and is validated for exactly that -- no more (which would advertise an untested contract) and no less (which would be routing in name only). `PROFILE_STAMPED_ITEMS` is the default; `PROFILE_SECTIONS` serves `todo-reachability.py`; `PROFILE_QUERY` serves `query.py` and covers four subtrees (`sections`, `sections.depends_on`, `stamps_xrefs`, and the node-level scalars and edge collections).

**`query.py` fails closed on an unusable cache, through every transport** ([query.py fail-closed through every transport](../../todo/00-infrastructure/TODO-06-todo-metadata-layer.md#22-querypy-fail-closed-through-every-transport)). It previously dropped malformed rows silently, so `stats`, `ready`, `blocked` and `backlinks` could be computed from a SUBSET of the corpus and still look complete -- and a scalar `depends_on` made the unmet-dependency loop iterate zero times, reporting a blocked TODO as **ready**. The exit codes are:

| Code | Meaning                                                                                                                                                                                                                               |
| ---- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 0    | A real answer (an empty result set is a legitimate 0).                                                                                                                                                                                |
| 1    | A verdict -- the graph itself has findings.                                                                                                                                                                                           |
| 2    | **Infrastructure: the cache exists and cannot be trusted.** A machine-readable `{"error": "cache-unusable", "reason", "detail"}` body goes to stdout.                                                                                 |
| 3    | Output-ceiling breach -- a bounded answer, not a refusal.                                                                                                                                                                             |
| 4    | **The request, not the cache** -- an unresolvable or ambiguous target, an unsupported `--scope`, an unknown `--fields` column, a negative `--offset`/`--limit`. A `{"error": "query-input", "reason", "detail"}` body goes to stdout. |

- The refusal body is written to **stdout**, not only stderr, because `mcp_server.py` runs `query.main()` in-process with stdout captured; a stderr-only refusal left that buffer empty and the old `buf.getvalue() or "[]"` turned it into an empty result array an agent could not distinguish from "nothing matched".
- **Freshness is checked for this repo's canonical `build/todo-cache.json`.** A cache supplied with `--cache` pointing elsewhere describes a different corpus by construction, so it is validated for SHAPE (still fail-closed) while freshness is skipped, and the reader says so on stderr. Comparing the RESOLVED path means `--cache build/todo-cache.json` is checked exactly like the default.
- **After a failed `--watch` rebuild the tick is refused, and the watcher stays alive.** Serving the previous cache would publish a graph predating the edit that broke the build. A rebuild that was never ATTEMPTED (out-of-repo `--cache`, absent `build.py`) is distinct from one that FAILED and does not refuse.
- **A request error is rc 4, and it too keeps the watcher alive.** The command handlers used to `sys.exit(2)` for an unresolvable target, so a caller could not tell a bad argument from an untrustworthy cache, and -- because `SystemExit` is not an `Exception` -- renaming the watched target terminated the watcher instead of reporting one bad tick. Both now raise `QueryInputError`, which the same guard that maps `CacheRefused` onto rc 2 maps onto rc 4.
- **The MCP layer's status matches its body.** A refusal or request-error envelope comes back with `isError: true` (the repo-owned JSON-RPC handler classifies with `_looks_like_error_envelope`; the FastMCP handlers raise), so a client that reads the protocol's own error flag is not told a refused query succeeded.
- **Watch events are coalesced.** One filesystem event used to mean one full cache rebuild, so a corpus-wide sweep replayed hundreds of obsolete intermediate states; the loop now drains the pending burst within a short settle window and answers once for the batch.

## Parsing Rules

- **Frontmatter detection.** The opening `---` MUST be the first three bytes (after an optional UTF-8 BOM, which is stripped). The closing `---` MUST be on its own line. CRLF line endings (`\r\n`) are normalized to LF before fence detection so Windows-authored TODOs parse correctly.
- **Absence of frontmatter is a HARD ERROR.** A file with no frontmatter block fails the build with `missing-frontmatter`. This replaced the migration-era rule, under which such a file parsed into a node with `id=null`, `status="no-frontmatter"` and the validator only counted them; the back-fill completed 2026-04-23 and every corpus file carries frontmatter, so the fallback now only hides a regression.
- **A body that never closes an HTML comment is the same hard error.** `<!--` with no `-->` masks to EOF exactly as an unclosed fence does; it is `unclosed-comment` rather than `unclosed-fence` so the message points at the right delimiter.
- **A body that never closes a fence is a hard error too.** Masking to EOF emits no WRONG structure but silently erases real structure: a file with a real Implementation Order, heading and `- [x]` item behind an unclosed opener built at rc 0 with all three empty (measured 2026-08-10). `unclosed-fence` names it instead. This is the BODY; an unclosed frontmatter delimiter is `malformed-yaml`.
- **Malformed YAML is a hard error.** Unclosed fences, tab indentation, duplicate keys, and any PyYAML parse error fail the build with a `file:line: malformed-yaml: <detail>` message. The cache file is NOT written when any file fails (fail-closed) so a partial cache cannot mask a regression.
- **Top-level type.** The frontmatter MUST be a YAML mapping (key: value pairs). Sequences or scalars at the top level are rejected as malformed.
- **Unknown fields.** Top-level fields not listed in this spec emit a `unknown-field` warning but do NOT fail the build. This is forward-compat: a newer generator that adds a field can ship cache files older generators read without breaking.
- **Unknown enum values.** A `status` not in `{draft, active, blocked, done, superseded}` is a hard error (`unknown-status`).

## Error Catalog (Generator-Reported)

The generator's full error catalog (1:1 with the parser branches) is:

| Category             | Trigger                                                                                                              | Severity |
| -------------------- | -------------------------------------------------------------------------------------------------------------------- | -------- |
| `malformed-yaml`     | Unclosed fence, tab indentation, duplicate mapping key (e.g. two `id:` entries), recursive YAML alias (self-referential container via `&a [*a]`-style aliases), any PyYAML parse error | FATAL    |
| `unknown-status`     | `status` not in `{draft, active, blocked, done, superseded}`                                                          | FATAL    |
| `invalid-id`         | `id` fails the kebab-case regex `^[a-z][a-z0-9-]{0,58}[a-z0-9]$`                                                      | FATAL    |
| `invalid-field`      | Optional field present but wrong type / shape / uniqueness: e.g. `owners` is a scalar, `depends_on` has duplicates or contains a non-id string, `$schema` is not a string, `title` / `domain` is empty. Mirrors the JSON Schema sidecar's per-field constraints | FATAL    |
| `missing-required`   | Any of `schema_version`, `id`, `domain`, `status`, `title` absent                                                     | FATAL    |
| `schema-version`     | `schema_version` not int (bool rejected too), OR < 1, OR > supported by current generator (forward-incompatible)      | FATAL    |
| `forbidden-field`    | Hand-authored frontmatter contains `created_at`, `last_active_at` or `stamped_items` (cache-only fields, not allowed in source) | FATAL    |
| `unclosed-comment`   | The BODY ends inside an unclosed HTML block comment. Same erasure as `unclosed-fence` -- everything from the opener is masked, so every body walk reads as empty -- and reported separately so the message names the delimiter the author must close | FATAL    |
| `missing-frontmatter` | The file has no frontmatter block at all. Hard failure since the 2026-04-23 back-fill; there is no `status: no-frontmatter` fallback node any more | FATAL    |
| `emission-drop`      | The producer built a node that DROPPED a field it declares it emits (an authored optional present in frontmatter but absent from the node, or extracted `stamped_items` that never reached it). Catches a copy-list/declared-set disagreement that would otherwise publish under a valid contract identity | FATAL    |
| `unclosed-fence`     | The BODY (not the frontmatter) ends inside an unclosed fenced code block, so every body walk reads as empty. Distinct from `malformed-yaml`, which covers an unclosed frontmatter delimiter | FATAL    |
| `unclosed-html`      | The BODY ends inside one of the OTHER EOF-consuming CommonMark HTML blocks -- type 1 (`<script>` / `<pre>` / `<style>` / `<textarea>`), 3 (processing instruction), 4 (declaration) or 5 (CDATA). Same erasure as `unclosed-fence`. Separate from `unclosed-comment` (type 2) so a category never says "comment" about a construct that is not one; types 6-7 end at a blank line and so can never be unterminated | FATAL    |
| `unknown-field`      | Top-level field not listed in Required + Optional                                                                     | WARNING  |

FATAL categories print `[build.py] FAIL <file>: <category>: <message>` to stderr, exit the build with code 1, and do NOT write the cache file. WARNING categories print `[build.py] WARN ...` (suppressed under `--quiet`) but the build still succeeds.

**The rule is an allowlist of ONE, not an enumerated fatal set** (`build.py:main`): `unknown-field` is downgraded to a warning and *every other category string is fatal by the `else` branch*. A category added later is therefore fatal by default -- which is the safe direction, and the reason this table can drift without anything failing open.

**Separately, some fatal conditions carry no category at all** and so never appear in this table: a per-file read error, an empty corpus, a corpus whose git history moved mid-build, a shallow-repo timestamp walk, and the two whole-run emission-contract assertions. Those exit 1 or 3 directly rather than through the per-node `errors` list.

## ID Naming Rules

- Lowercase, kebab-case (`a-z`, `0-9`, `-`), 2-60 characters total.
- MUST start with a letter.
- MUST NOT end with a dash.
- Globally unique across every domain (validator gates).
- Stable across renumbering: renaming a file from `TODO-05-old.md` to `TODO-12-new.md` does NOT change its `id`. Inbound `depends_on` / `satisfies` references keep working.
- Renaming an id IS allowed, but requires a grep sweep across `todo/` to rewrite every inbound `depends_on:` / `satisfies:` reference (and the validator catches dangling refs that the sweep missed).

Examples:
- `ai-development-system`
- `kernel-test-harness`
- `todo-metadata-layer` (this TODO)
- `desktop-test-late-phase-harness`

Counter-examples (reject):
- `Ai-Dev-System` (uppercase)
- `_kernel-test` (leading underscore, not a letter)
- `kernel test` (space)
- `todo-` (trailing dash)
- `1-foo` (leading digit)

## XREF-in-Prose Forms (Human-Authored, Survive)

The frontmatter `id` references are the machine-readable graph. Two human-authored XREF forms remain allowed in the markdown body and are parsed by the generator into the cache:

1. **Compact `D<dom>T<num>` shorthand** for cross-domain references and `T<num>` for same-domain, both followed by an optional section marker. Scannable in tight Implementation Order `Depends On` table cells. Lives only inside `todo/**/*.md` and `.claude/skills/**` per [`scripts/lint.sh` Check 4](../../scripts/lint.sh) (which forbids the bare numeric shorthand outside those scopes to keep cross-doc references stable across renumbering). The generator captures these as `target_path` strings (e.g. `02-kernel-core/TODO-19`); the validator section resolves them to stable ids by lookup once the migration has shipped.
2. **`(item: "NAME" at line N)` parenthetical in stamps.** Used inside `> **Accepted:**` and `> **Deferred:**` lines to name the specific checklist item the XREF target item resolves to. The generator captures `item_name` from this. The validator's `--fix-line-numbers` mode re-resolves the line number when the named item moves.

Both forms stay human-authored. The validator looks them up by file-and-section to detect drift; the cache treats them as graph edges alongside the structured `depends_on` / `satisfies` fields.

## Authoritative Example

The frontmatter that this TODO (`todo-metadata-layer`) carries once the migration ships:

```yaml
---
$schema: ../../docs/infrastructure/todo-metadata.schema.json
schema_version: 1
id: todo-metadata-layer
domain: 00-infrastructure
status: active
title: TODO Metadata Layer and Derived Graph
depends_on: []
---
```

This block validates cleanly against the JSON Schema sidecar:

```bash
python3 - <<'PY'
import json, jsonschema, yaml
schema = json.load(open("docs/infrastructure/todo-metadata.schema.json"))
jsonschema.Draft202012Validator.check_schema(schema)
sample = yaml.safe_load("""
$schema: ../../docs/infrastructure/todo-metadata.schema.json
schema_version: 1
id: todo-metadata-layer
domain: 00-infrastructure
status: active
title: TODO Metadata Layer and Derived Graph
depends_on: []
""")
jsonschema.validate(sample, schema)
print("OK: schema valid + sample validates against it")
PY
```

## The Implementation Plan

[`todo/implementation-plan.md`](../../todo/implementation-plan.md) is the graph laid out as one ordered list: every numbered section of every TODO file appears in exactly one row, grouped into phases, in the order it should be built. `scripts/todo-plan.py` keeps it current. The script is plain Python over a privately rebuilt graph cache (about 2.5 seconds for 2,591 sections), so keeping the plan current spends no model.

A row is a box, an address in backticks, a title, the section's Needs and its count of checklist items. The address is `D` and the two digits of the domain directory, then `T` and the file's number (or letter, for a master-table file), then the section sign and the section number.

| Command                                          | What it does                                                                                                                                                                                                                                                                             |
| ------------------------------------------------ | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `python3 scripts/todo-plan.py --sync`            | Rewrites each row's box, title, Needs and Items, places new sections, removes rows whose section is gone, updates the progress line.                                                                                                                                                     |
| `python3 scripts/todo-plan.py --check`           | Exits 1 when a sync would change the file, an open section sits before one it depends on, a dependency cycle exists, a section depends on itself or on something no work can finish, a row is mislabelled, or a `**Needs:**` line names a value outside the list. Lint Check 37 runs it. |
| `python3 scripts/todo-plan.py --next 5`          | The first five rows that can be built now, in plan order. `--next 0` lists all of them. Add `--json` for a tool.                                                                                                                                                                         |
| `python3 scripts/todo-plan.py --resolve '<row>'` | The file and section a row names, from a pasted row in any shape.                                                                                                                                                                                                                        |
| `python3 scripts/todo-plan.py --cycles`          | Open sections whose `Depends On` cells form a cycle.                                                                                                                                                                                                                                     |
| `python3 scripts/todo-plan.py --seed`            | Writes a first plan from the roadmap domains. Refuses when a plan exists.                                                                                                                                                                                                                |

**What is derived and what is written by hand.** The box is the section's lifecycle class from `sequencer_triage.py`, the classifier the run obeys: `[x]` done (shipped with both stamps, or parked by a terminal deferral), `[/]` shipped or in progress and still owed work or review, `[ ]` not started. The title is the section heading without appended notes or model tags, cut at 120 characters. Items is the count of the section's top-level checklist items (`- [ ]`, `- [x]`, `- [/]`), outside code samples. Phase headings, the paragraph under each, and the order of rows are written by hand: a sync never moves a row and never edits prose.

**Needs.** A section that cannot finish without something outside the run says so on one line under its heading: `**Needs:** bare-metal` (comma-separated for more than one). The list is closed and lives in `scripts/todo-plan.py` (`NEEDS`): `bare-metal`, `whpx-host`, `display-session`, `attended`, `owner-decision`, `third-party`, `operator`. A section with no such line shows `none`. A Need is a label, never a gate: `--next` and the run treat the section as any other. The run builds it, proves everything the emulators and the host can prove, and parks only the one check that needs the named thing as a `- [/]` item. Write the line only when no substitute exists: a check that an emulator, a host tool or a fixture can make is written that way and carries no Need. The format follows the Conclave-SDK todo kit, whose plan table has the same two columns.

**Where a new section lands.** After the last row of its own file, or in a later phase when it depends on a section there. Move it by hand when it belongs elsewhere; the check holds the result.

**The order rule.** An open section never sits before an open section it depends on. Finished work constrains nothing: many done sections still name prerequisites that were never formally closed, and holding them to order would push shipped boot work behind the installer. A dependency cycle fails `--check`, because none of its members can ever be built; `--cycles` lists its edges, and the fix belongs in the `Depends On` cells of the Implementation Order tables. The corpus has had none since 2026-11-11.

**A row's section number.** The graph takes it from the table's Section column when there is one, else from Order. A table that keeps Order as a build position therefore needs a Section column; one that writes the real `§N` at the start of the Deliverable instead is read with every section shifted, and `--check` refuses it. Ten tables had that shape, and three of the corpus's cycles were nothing else.

**The run follows the plan.** `sequencer_triage.py --next` keeps the run's current file while it has a runnable section (shipped and owing review, or buildable now), then opens the owner of the first row `--next` prints, then the first file in domain order with a runnable section; when nothing can start it names the first open file with `"waiting": true`, and it falls back to domain order when the plan cannot answer. `--classify` marks every other open section `waiting`, and `advance-work.py` skips those, so the run never starts a section before its dependencies while anything else can start. When every open section waits, the packet still names the first one (`next_section` carries `waiting`): work it anyway, build what its finished dependencies allow, and defer the rest with an XREF to the owning section. Readiness is computed in-process from the oracle's cache file re-read under the strict freshness check (the planner rebuilds its own graph when that fails), only when a file has to be chosen, so a finished or blocked queue never pays for it. The rule and its fallback are in [the runner doctrine](../../todo/TODO-Claude-Overnight-Runner.md#traversal-order).

**When to sync.** After any TODO edit that ships, stamps, renames or files a section, before the commit. Stage the plan with the TODO edit: under the commit gate the check reads the git index (`--check --staged`), which is what the commit carries, so a plan synced but left unstaged is refused and a TODO edit outside the commit changes nothing. Syncs take a lock (`build/.todo-plan.lock`), so two at once run one after the other. A hand edit made while a sync runs does not take that lock: the sync notices the file changed under it, writes nothing, and is run again.

**A merge conflict in the plan.** The rows are derived, so a plan git could not merge (a rebase over a commit that also synced it) is repaired by a sync: `python3 scripts/todo-plan.py --sync` removes the conflict markers, keeps one copy of each row and rewrites every cell from the TODO files. Then `git add todo/implementation-plan.md` and continue the rebase. Never pick a side by hand.

**Do not read the plan into a session.** It is about 2,700 lines. Ask for `--next`.

## See Also

- [`docs/infrastructure/todo-metadata.schema.json`](todo-metadata.schema.json) -- machine-readable JSON Schema Draft 2020-12 sidecar
- [`scripts/todo-graph/build.py`](../../scripts/todo-graph/build.py) -- generator that parses frontmatter into `build/todo-cache.json`
- [`scripts/todo-graph/tests/test_build.sh`](../../scripts/todo-graph/tests/test_build.sh) -- regression suite covering the parser surfaces named in this spec, split since 2026-11-11 into four shards that share [`test_build_lib.sh`](../../scripts/todo-graph/tests/test_build_lib.sh): this core shard, `test_build_gate.sh` (the identity gate), `test_build_sections_a.sh` and `test_build_sections_b.sh`. The sub-test count is not hand-tracked here (it grows with the surface and was 844 at the 2026-08-28 TODO-06 close-out); the runner prints its own `[test_build] N/N passed` summary.
- [`todo/00-infrastructure/TODO-06-todo-metadata-layer.md`](../../todo/00-infrastructure/TODO-06-todo-metadata-layer.md) -- the TODO-metadata-layer plan (this spec is shipped under the schema-and-spec section; the generator under the next)
