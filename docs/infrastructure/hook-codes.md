<!-- docs: covers=todo/00-infrastructure/TODO-08-automation-hardening.md sources=.claude/hooks/MANIFEST.md,.githooks/pre-commit,.githooks/pre-push reviewed=2026-11-09 -->
# Hook Message Codes

Hook block/reminder messages are injected into the model's context every time
they fire; the 2026-07-10 run carried ~500 KB of repeated policy prose from the
Stop loop alone. Hooks therefore emit a SHORT code + the immediate remediation
+ a pointer here; this file holds the full policy text, rationale, and history.
Codes are stable -- docs and transcripts reference them.

## SEQ-* (run_phase_guard.py)

### SEQ-CONTROL-PLANE
The run may edit its own machinery, with a test, since 2026-11-07. Only the frozen files listed in `.claude/hooks/gate.py` stay refused, because they recover from every other mistake. Add the problem to `todo/RUN-ISSUES.md` and continue.

Added 2026-08-07 because the rule had been **documentation only**. It is stated in the sequencer skill, in the doctrine file and in every capture file's header, and nothing enforced it -- an `Edit` on `scripts/overnight/decision-registry.py` returned rc 0 from every guard. The run edited that file for ~90 lines, noticed on its own, reverted, and filed the gap. Prose is not a gate.

`.claude/state/**` is deliberately NOT covered: that is the run's own notebook (gotcha cards, receipts, cursor state), which it writes constantly. The rule is about the machinery that DECIDES, not the notes it keeps. Ordinary work -- `src/`, `user/`, tests, docs, `todo/` -- is unaffected.

Paths are compared repo-relative after resolution, so an absolute path, a `./` prefix and a `../` escape all normalise to the same answer. INERT interactively: an operator repairing the control plane is the intended path, and the guard is silent with no `OVERNIGHT_SEQUENCER_RUN` discriminator.

### SEQ-STOP
The headless unattended run may not stop voluntarily. The work unit is the
entire TODO queue; a blocker, hard failure, or operator-reserved decision is
DEFERRED (`[/]` + Deferred stamp + XREF) and you ADVANCE. The only endings:
oracle-verified `run_phase_guard.py fixpoint`, the operator's `run.sh stop`, or a
bounded run (`run.sh start --ships N`) that shipped its sections. A verified
`rollover` does not end the session: it goes straight on. Reviews are polled IN-SESSION (a blocking Bash `sleep`
loop, ~0 tokens) -- the runner never exits to wait. The heartbeat restarts
any death, so a voluntary exit accomplishes nothing. Remediation: re-invoke
`Skill(overnight-sequencer)`, run `run_phase_guard.py status`, and continue
from the recorded phase. Doctrine: todo/TODO-Claude-Overnight-Runner.md
"Session-exit policy". (The `wait`/`wake` verbs + watcher were removed
2026-07-11; see the doctrine's in-session-poll rule.)

### SEQ-ASK
`AskUserQuestion` is never allowed in the unattended run. Decide with the
conservative/no-op choice and log the assumption, or DEFER the item (`[/]` +
Deferred stamp with `awaiting-answer` + XREF naming the decision) and advance.
Three-tier answer order: todo/answers.md operator answer -> low-risk logged
default -> defer. Doctrine: "Session-exit policy" tier list.

### SEQ-TEARDOWN
Self-teardown blocked: the unattended run cannot stop itself, clear guard state,
fabricate the fixpoint sentinel, or kill its own tmux sessions. Stopping is
human-only: `bash scripts/run.sh stop` from an interactive session. Keep going.

### SEQ-CODEX-WRITE
A write-capable Codex dispatch (`task --write`, `--sandbox workspace-write`,
`--full-auto`, `--dangerously-bypass-approvals-and-sandbox`, or `-s
danger-full-access`) was attempted inside the unattended headless run. Codex
write is INTERACTIVE-ONLY (Option A, 2026-07-11): the autonomous-agent
boundary (CLAUDE.md) forbids an agent mutating the tree with no per-step human
authorship. The runner uses read-only reviews (`adversarial-review`, the
review broker, `task` without `--write`) only. Remediation: implement the
change yourself and dispatch a read-only review, or defer the section for an
operator's interactive rescue (`codex … task --write` or `Skill(codex:rescue)`
from an interactive session, where this guard is inert). The reviewer sandbox
is separately pinned read-only in `codex-companion.mjs`, and
`codex_review_completed.py` firewalls a write dispatch out of the review
receipt state so it can never satisfy or queue a review gate.

### SEQ-WORKTREE

**Emitted by:** `run_phase_guard.py` (PreToolUse Bash), unattended run only.

**Means:** the run attempted a mutating `git worktree` subcommand (`add`, `remove`, `move`, `prune`, `lock`, ...). Read-only `git worktree list` is allowed.

**Why:** the run executes the PRIMARY worktree on `main` and has no legitimate reason to create another. The ONE sanctioned repair worktree exists so an operator can fix the control plane WITHOUT sharing the run's tree, index and `build/` -- the collision class measured on 2026-07-31 (four refused rollovers, one swept index, one invalidated smoke receipt in a single evening). It is created interactively by a human, where this guard is inert. Letting the run add worktrees of its own would reintroduce exactly the ambiguity the single-worktree rule removes, and a worktree the operator did not create is one nobody is watching.

**What to do:** nothing -- continue the pipeline in the primary worktree. If a change genuinely seems to need an isolated checkout, that is an operator decision: file it in the newest `todo/overnight-runner-improvements/` capture file and advance.

**What it matches (since 2026-08-29):** the INVOCATION, not the text. Heredoc bodies are stripped, the command is split on control operators, env-prefix and wrappers (`env`, `timeout`, `xargs`, `exec`, ...) are walked, `bash -c '...'` is descended, and a segment headed by a data consumer (`echo`, `sed`, `grep`, `python3`, a quoted `git commit -m` message) is never a mutation. So writing SOURCE that quotes `git worktree add` (a heredoc patch, a commit message, a fixture) no longer trips it; running one through any wrapper or chain still does, and a scratch repo under `/tmp` is still blocked because the guard has no safe notion of where a path resolves. Controls: `scripts/overnight/tests/test_worktree_guard.py` (18 must-block, 12 must-allow shapes). Before 2026-08-29 it matched the substring anywhere and was routed around in under a minute (split the literal, `-F msgfile`), which is why it changed.


## CACHE-* (agent_result_cache.py)

### CACHE-HIT
An identical analyst dispatch already ran over content-identical inputs; the
cached report follows the code in the block message. Reuse it (verify
load-bearing file:line claims as usual). Force a fresh run by changing the
prompt; kill switch `AGENT_RESULT_CACHE_DISABLE=1`.

## SEARCH-* (search_offload_gate.py) -- RETIRED 2026-07-28

### SEARCH-OFFLOAD (retired -- no longer emitted)
This code is dead: the hook was unwired from `settings.json` on 2026-07-28 and
never fires. Retained so a transcript carrying the code can still be read, and
so the measurement is not lost.

It blocked a leading file search (`grep`/`rg`/`find` family) in the headless
run and directed the caller to the Grep/Glob TOOL, on the premise that the
tool's output does not land in the main context. The live canary run of
2026-07-28 disproved that premise:

  * Every tool result lands in context, the Grep tool's included. The blocked
    `grep -n X file | head -20` and the piped form it was pushed into,
    `cat file | grep -n X | head -20`, return identical output for identical
    context. Observed cost: three tool calls where one would do (block, retry,
    reformulate), no context saved.

A second fault was briefly claimed here and is WRONG -- recorded so it is not
repeated. The first retirement note said the headless run "has no Grep tool",
citing one `Grep is not available in this session` error at 02:03:44. The same
run then made 14 successful Grep tool calls from 02:26 on. Grep is a DEFERRED
tool whose schema loads on demand via ToolSearch; that single error was an
unloaded schema, not an absent tool.

Do not revive it by rewording the message. Bounded output and agent offload are
already owned by other gates, and the premise cannot be repaired in an
environment with no Grep tool.

## EDIT-* (edit_preflight.py)

### EDIT-STALE
The Edit old_string does not occur in the target file (0 matches) or is
ambiguous (2+ matches without replace_all). The hook message includes the
closest actual line -- re-read a slice around it and re-anchor the edit on
current content instead of retrying variants.

### EDIT-LINELEN
A new/edited TODO checklist line exceeds the 250-char cap (OS-Comparison rows
200). The hook names the line and its length. Trim to <= 230 before invoking
Edit; move detail into the commit message instead.
