# Codex companion: local patches

This tree is OpenAI's Codex plugin for Claude Code (`openai/codex-plugin-cc`, `plugins/codex/`, v1.0.4 at commit `807e03ac9d5aa23bc395fdec8c3767500a86b3cf`, Apache-2.0), vendored so the review dispatcher (`scripts/codex-dispatch.sh`) can set reasoning effort per pass from `.conclave/reviewers.toml`. Everything here is upstream text, byte-exact, except the files and changes this page lists.

## Patch: `--effort` on `adversarial-review`

- `scripts/codex-companion.mjs`: `adversarial-review` accepts `--effort <none|minimal|low|medium|high|xhigh>`, normalised by the upstream `normalizeReasoningEffort`, and forwards it to `runAppServerTurn`, as `task` already does. The native `review` command still refuses the option, because its `review/start` request carries no effort.
- `tests/effort.test.mjs` (ours): four tests on the upstream fake app-server (`tests/fake-codex-fixture.mjs`, `tests/helpers.mjs`, copied from upstream `tests/`). Each test owns its plugin data, inherits no broker endpoint, and stops every process carrying that plugin data (the detached broker and its app-server, whether or not the broker published its record) before it ends.

## Patch: a turn whose app-server dies fails at once

- `scripts/lib/codex.mjs` (`captureTurn`): the turn's completion races the client's connection close, so a turn whose app-server exits after accepting it fails then, instead of waiting for a completion notification that cannot come. Upstream waited for good; our dispatcher's limit (`review_runtime_limit.py`, 1800 s by default) was the only end, about half an hour later.
- `scripts/app-server-broker.mjs`: when the broker's own app-server exits, it ends every client connection and stops, so a brokered client sees the close too. A shutdown the broker started itself (SIGTERM, SIGINT) is not treated as that exit.
- Test: `tests/effort.test.mjs` "a turn whose app-server exits after accepting it fails instead of waiting forever" (the generated fake acknowledges `turn/start` and exits); without both patches it waits until its outer timeout.

## Patch: an untracked special file never blocks the review

- `scripts/lib/git.mjs` (`formatUntrackedFile`): an untracked path is `lstat`ed, never followed: a symlink is shown by its link text (as the dispatcher's candidate reader keys it), anything but a regular file is skipped, and the content is read through a no-follow, non-blocking descriptor checked to be a regular file and capped at the byte limit. Upstream followed links with `statSync`, so a symlink to a FIFO with no writer (which reports 0 bytes) blocked `readFileSync` before the reviewer started.
- Every component is opened through a directory descriptor with no-follow (`openParentBeneath`), so a parent directory swapped for a symlink after git listed a file cannot lead the read outside the checkout. Test: "an untracked file whose parent was swapped for a link is never read outside the checkout" (`formatUntrackedFile` is exported for it).
- Test: `tests/effort.test.mjs` "an untracked symlink to an idle FIFO never blocks the review"; without the patch it blocks until its timeout.
- `scripts/lib/git.mjs` (`getWorkingTreeState`): the staged, unstaged and untracked lists are read NUL-delimited (`-z`) and untrimmed. Upstream split git's display names, which quote a tab or a non-ASCII byte, so an untracked file with such a name was reported unreadable. The untracked names are also kept as the bytes git printed (`untrackedRaw`) and each file is opened by them: a name that is not UTF-8 decodes to the same string as another, so a decoded name could open the wrong file. All three lists are read as bytes; files are counted by their bytes for the inline-review limit, and a name that is not UTF-8 is displayed with its bytes escaped. Tests: "an untracked file whose name git quotes still reaches the review with its contents", "two untracked names that decode alike each bring their own contents", "three names that decode alike are three files for the inline limit".
- `scripts/lib/git.mjs` (`getRepoRoot`, `ensureGitRepository`): the root loses only git's newline. `trim()` also removed trailing spaces, so a worktree named `repair ` was reviewed as its sibling `repair`. Test: "a worktree whose name ends in a space is reviewed, not its trimmed sibling".
- `scripts/lib/git.mjs` (`collectWorkingTreeContext`, self-collection mode): untracked files are counted once, with the command that lists them, and left out of the status and changed-file sections, instead of being read into the prompt (128 files of 24 KiB were 3 MiB of model input) or named in three sections (an untracked import of 3,527 files was about 548 KB). Test: "in self-collection mode untracked files are listed, not read into the prompt".

## Patch: the working directory is the one this process is in

- `scripts/codex-companion.mjs` (`resolveCommandCwd`): with no `--cwd`, the decoded `process.cwd()` must name the directory the process is in (same device and inode as `.`), or the command refuses. A directory name that is not UTF-8 decodes with U+FFFD, and that path can name a sibling, which git and the reviewer would then use. Test: "a working directory whose path cannot be named exactly is refused, never replaced by a sibling".
- `scripts/lib/git.mjs`: one reversible display for file names (shown as is unless not UTF-8, starting with a quote or byte-order mark, or holding a backslash or control character; then quoted with `\xNN` escapes), so distinct files never share a heading. Test: "distinct names never share a heading".

## Not patched: the shared job state

Upstream reads, changes and rewrites `state.json` with no lock, so two companion processes sharing one plugin-data directory can delete each other's job records. That is left as upstream wrote it: `scripts/codex-dispatch.sh` gives every review pass its own `CLAUDE_PLUGIN_DATA`, so no two of our passes share that state or a broker, and it runs each pass under `scripts/overnight/review_runtime_limit.py`, the child subreaper the review broker uses, which ends and reaps every process of the pass (the detached broker and its app-server included) within the pass's limit.

Run the tests here: `node --test tools/vendor/codex-companion/tests/effort.test.mjs`.

Upstream: the same change is proposed in openai/codex-plugin-cc pull requests #677 ("Support --effort on adversarial-review", opened 2026-08-23) and #746 (the same plus unrecognised-option errors, opened 2026-09-06), and requested in issues #476 and #651; none was merged as of 2026-11-23. We open no third request (operator decision 2026-11-23). When a release carries either, the effort patch is retired; the other patches on this page stay until their tests pass against the stock tree.

## Re-drop

Copy upstream `plugins/codex/{.claude-plugin,scripts,prompts,schemas,LICENSE,NOTICE}` and `tests/{fake-codex-fixture.mjs,helpers.mjs}` over this tree, re-apply the patch above (or drop it once upstream ships `--effort` on `adversarial-review`), run the tests, update the commit in `src/libs/PROVENANCE.md`, and review the re-drop like any import (Claude read, then the pinned Astra passes).
