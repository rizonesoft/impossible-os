# Codex companion: local patches

This tree is OpenAI's Codex plugin for Claude Code (`openai/codex-plugin-cc`, `plugins/codex/`, v1.0.4 at commit `807e03ac9d5aa23bc395fdec8c3767500a86b3cf`, Apache-2.0), vendored so the review dispatcher (`scripts/codex-dispatch.sh`) can set reasoning effort per pass from `.conclave/reviewers.toml`. Everything here is upstream text, byte-exact, except the files this page lists.

## Patch: `--effort` on `adversarial-review`

- `scripts/codex-companion.mjs`: `adversarial-review` accepts `--effort <none|minimal|low|medium|high|xhigh>`, normalised by the upstream `normalizeReasoningEffort`, and forwards it to `runAppServerTurn`, as `task` already does. The native `review` command still refuses the option, because its `review/start` request carries no effort.
- `tests/effort.test.mjs` (ours): four tests on the upstream fake app-server (`tests/fake-codex-fixture.mjs`, `tests/helpers.mjs`, copied from upstream `tests/`). Run: `node --test tools/vendor/codex-companion/tests/`.

Upstream: the same change is proposed in openai/codex-plugin-cc pull requests #677 ("Support --effort on adversarial-review", opened 2026-08-23) and #746 (the same plus unrecognised-option errors, opened 2026-09-06), and requested in issues #476 and #651; none was merged as of 2026-11-23. We open no third request (operator decision 2026-11-23). When a release carries either, drop this patch and re-drop the stock tree.

## Re-drop

Copy upstream `plugins/codex/{.claude-plugin,scripts,prompts,schemas,LICENSE,NOTICE}` and `tests/{fake-codex-fixture.mjs,helpers.mjs}` over this tree, re-apply the patch above (or drop it once upstream ships `--effort` on `adversarial-review`), run the tests, update the commit in `src/libs/PROVENANCE.md`, and review the re-drop like any import (Claude read, then the pinned Astra passes).
