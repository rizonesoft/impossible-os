# TODO Metadata Layer -- Frontmatter Spec

> Canonical spec for the YAML frontmatter block that every TODO file under `todo/**/*.md` carries (post-migration). The frontmatter is the stable identity layer the [`scripts/todo-graph/build.py`](../../scripts/todo-graph/build.py) generator parses into `build/todo-cache.json`, decoupling cross-TODO references from filename-based lookups (which break silently on renumbering).
>
> **Two-layer doctrine:** the canonical authority for what's allowed is this human-readable spec PLUS the machine-readable JSON Schema sidecar at [`docs/infrastructure/todo-metadata.schema.json`](todo-metadata.schema.json). The generator implements both. The `$schema:` field inside each TODO's frontmatter is metadata that programmatic validators (the §2 generator, `remark-lint-frontmatter-schema` in §6 CI, ad-hoc `python3 -c "import jsonschema; jsonschema.validate(...)"` calls) follow to locate the sidecar; it is NOT auto-detected as a schema directive by the stock Red Hat YAML language server (which expects `# yaml-language-server: $schema=...` modeline comments or a workspace `yaml.schemas` mapping, neither of which natively reads markdown-embedded YAML). Editor-time inline diagnostics for TODO frontmatter therefore require either (a) a markdown-frontmatter-aware lint integration like [`remark-lint-frontmatter-schema`](https://github.com/JulianCataldo/remark-lint-frontmatter-schema) wired through `remark-cli` in CI and an editor's "lint on save" hook, or (b) the [`markdown-yaml-embedded-langservers`](https://marketplace.visualstudio.com/items?itemName=joaompinto.vscode-yaml-frontmatter) VS Code extension that hooks the Red Hat YAML server into markdown YAML frontmatter blocks. The §6 CI gate is the authoritative line of defense; editor-time diagnostics are best-effort developer convenience.

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

Operational notes:

- The binding is **immutable and named for the cache digest**, so it can be written before the cache is atomically replaced; whichever generation survives a crash has its own binding beside it. `build.py` prunes the non-current ones after each successful publish.
- A cache **copied without its binding is refused**, not accepted -- absence of evidence is a rebuild condition. Rebuild with `bash scripts/todo-graph/build-and-validate.sh --keep-cache`.
- Both files are derived build artifacts under `build/` and are gitignored.

## Reader Profiles and Fail-Closed Behavior

A reader declares what it CONSUMES via a `cache_schema.Profile`, and is validated for exactly that -- no more (which would advertise an untested contract) and no less (which would be routing in name only). `PROFILE_STAMPED_ITEMS` is the default; `PROFILE_SECTIONS` serves `todo-reachability.py`; `PROFILE_QUERY` serves `query.py` and covers four subtrees (`sections`, `sections.depends_on`, `stamps_xrefs`, and the node-level scalars and edge collections).

**`query.py` fails closed on an unusable cache, through every transport** ([query.py fail-closed through every transport](../../todo/00-infrastructure/TODO-06-todo-metadata-layer.md#22-querypy-fail-closed-through-every-transport)). It previously dropped malformed rows silently, so `stats`, `ready`, `blocked` and `backlinks` could be computed from a SUBSET of the corpus and still look complete -- and a scalar `depends_on` made the unmet-dependency loop iterate zero times, reporting a blocked TODO as **ready**. The exit codes are:

| Code | Meaning |
|---|---|
| 0 | A real answer (an empty result set is a legitimate 0). |
| 1 | A verdict -- the graph itself has findings. |
| 2 | **Infrastructure: the cache exists and cannot be trusted.** A machine-readable `{"error": "cache-unusable", "reason", "detail"}` body goes to stdout. |
| 3 | Output-ceiling breach -- a bounded answer, not a refusal. |

- The refusal body is written to **stdout**, not only stderr, because `mcp_server.py` runs `query.main()` in-process with stdout captured; a stderr-only refusal left that buffer empty and the old `buf.getvalue() or "[]"` turned it into an empty result array an agent could not distinguish from "nothing matched".
- **Freshness is checked for this repo's canonical `build/todo-cache.json`.** A cache supplied with `--cache` pointing elsewhere describes a different corpus by construction, so it is validated for SHAPE (still fail-closed) while freshness is skipped, and the reader says so on stderr. Comparing the RESOLVED path means `--cache build/todo-cache.json` is checked exactly like the default.
- **After a failed `--watch` rebuild the tick is refused, and the watcher stays alive.** Serving the previous cache would publish a graph predating the edit that broke the build. A rebuild that was never ATTEMPTED (out-of-repo `--cache`, absent `build.py`) is distinct from one that FAILED and does not refuse.

## Parsing Rules

- **Frontmatter detection.** The opening `---` MUST be the first three bytes (after an optional UTF-8 BOM, which is stripped). The closing `---` MUST be on its own line. CRLF line endings (`\r\n`) are normalized to LF before fence detection so Windows-authored TODOs parse correctly.
- **Absence is not an error during migration.** Files without frontmatter parse into a node with `id=null`, `status="no-frontmatter"`. The validator surfaces these counts but does not fail the build until the migration completes.
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
| `forbidden-field`    | Hand-authored frontmatter contains `created_at` or `last_active_at` (cache-only fields, not allowed in source)        | FATAL    |
| `unknown-field`      | Top-level field not listed in Required + Optional                                                                     | WARNING  |

FATAL categories print `[build.py] FAIL <file>: <category>: <message>` to stderr, exit the build with code 1, and do NOT write the cache file. WARNING categories print `[build.py] WARN ...` (suppressed under `--quiet`) but the build still succeeds.

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

## See Also

- [`docs/infrastructure/todo-metadata.schema.json`](todo-metadata.schema.json) -- machine-readable JSON Schema Draft 2020-12 sidecar
- [`scripts/todo-graph/build.py`](../../scripts/todo-graph/build.py) -- generator that parses frontmatter into `build/todo-cache.json`
- [`scripts/todo-graph/tests/test_build.sh`](../../scripts/todo-graph/tests/test_build.sh) -- regression suite (12 sub-tests covering the parser surfaces named in this spec)
- [`todo/00-infrastructure/TODO-06-todo-metadata-layer.md`](../../todo/00-infrastructure/TODO-06-todo-metadata-layer.md) -- the TODO-metadata-layer plan (this spec is shipped under the schema-and-spec section; the generator under the next)
