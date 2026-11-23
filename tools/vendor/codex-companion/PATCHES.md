# Codex companion: local patches

This tree is OpenAI's Codex plugin for Claude Code (`openai/codex-plugin-cc`, `plugins/codex/`, v1.0.4 at commit `807e03ac9d5aa23bc395fdec8c3767500a86b3cf`, Apache-2.0), vendored so the review dispatcher (`scripts/codex-dispatch.sh`) can set reasoning effort per pass from `.conclave/reviewers.toml`. Everything here is upstream text, byte-exact, except the files and changes this page lists.

## Patch: `--effort` on `adversarial-review`

- `scripts/codex-companion.mjs`: `adversarial-review` accepts `--effort <none|minimal|low|medium|high|xhigh>`, normalised by the upstream `normalizeReasoningEffort`, and forwards it to `runAppServerTurn`, as `task` already does. The native `review` command still refuses the option, because its `review/start` request carries no effort.
- `tests/effort.test.mjs` (ours): four tests on the upstream fake app-server (`tests/fake-codex-fixture.mjs`, `tests/helpers.mjs`, copied from upstream `tests/`). Each test owns its plugin data, inherits no broker endpoint, and stops every process carrying that plugin data (the detached broker and its app-server, whether or not the broker published its record) before it ends.

## Patch: a turn whose app-server dies fails at once

- `scripts/lib/codex.mjs` (`captureTurn`): the turn's completion races the client's connection close, so a turn whose app-server exits after accepting it fails then, instead of waiting for a completion notification that cannot come. Upstream waited for good; our dispatcher's limit (`review_runtime_limit.py`, 1800 s by default) was the only end, about half an hour later.
- `scripts/app-server-broker.mjs`: when the broker's own app-server exits, it ends every client connection and stops, so a brokered client sees the close too. A shutdown the broker started itself (SIGTERM, SIGINT) is not treated as that exit.
- Test: `tests/effort.test.mjs` "a turn whose app-server exits after accepting it fails instead of waiting forever" (the generated fake acknowledges `turn/start` and exits); without both patches it waits until its outer timeout.

## Not patched: the shared job state

Upstream reads, changes and rewrites `state.json` with no lock, so two companion processes sharing one plugin-data directory can delete each other's job records. That is left as upstream wrote it: `scripts/codex-dispatch.sh` gives every review pass its own `CLAUDE_PLUGIN_DATA`, so no two of our passes share that state or a broker, and it runs each pass under `scripts/overnight/review_runtime_limit.py`, the child subreaper the review broker uses, which ends and reaps every process of the pass (the detached broker and its app-server included) within the pass's limit.

Run the tests here: `node --test tools/vendor/codex-companion/tests/effort.test.mjs`.

Upstream: the same change is proposed in openai/codex-plugin-cc pull requests #677 ("Support --effort on adversarial-review", opened 2026-08-23) and #746 (the same plus unrecognised-option errors, opened 2026-09-06), and requested in issues #476 and #651; none was merged as of 2026-11-23. We open no third request (operator decision 2026-11-23). When a release carries either, drop this patch and re-drop the stock tree.

## Re-drop

Copy upstream `plugins/codex/{.claude-plugin,scripts,prompts,schemas,LICENSE,NOTICE}` and `tests/{fake-codex-fixture.mjs,helpers.mjs}` over this tree, re-apply the patch above (or drop it once upstream ships `--effort` on `adversarial-review`), run the tests, update the commit in `src/libs/PROVENANCE.md`, and review the re-drop like any import (Claude read, then the pinned Astra passes).
