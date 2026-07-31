# Hook Message Codes

Hook block/reminder messages are injected into the model's context every time
they fire; the 2026-07-10 run carried ~500 KB of repeated policy prose from the
Stop loop alone. Hooks therefore emit a SHORT code + the immediate remediation
+ a pointer here; this file holds the full policy text, rationale, and history.
Codes are stable -- docs and transcripts reference them.

## SEQ-* (run_phase_guard.py)

### SEQ-STOP
The headless unattended run may not stop voluntarily. The work unit is the
entire TODO queue; a blocker, hard failure, or operator-reserved decision is
DEFERRED (`[/]` + Deferred stamp + XREF) and you ADVANCE. The only endings:
oracle-verified `run_phase_guard.py fixpoint`, the operator's `--disarm`, or a
verified `rollover`. Reviews are polled IN-SESSION (a blocking Bash `sleep`
loop, ~0 tokens) -- the runner never exits to wait. The watchdog relaunches
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
Self-teardown blocked: the unattended run cannot disarm, clear guard state,
fabricate the fixpoint sentinel, or stop its own services. Disarm is
human-only: `bash .claude/skills/overnight-sequencer/arm-sequencer.sh --disarm`
from an interactive session. Keep going.

### SEQ-PHASE
A sequence skill fired outside its phase. The per-file pipeline is
PREFLIGHT -> TRIAGE -> VALIDATE -> GAP_AUDIT -> SECTIONS -> FILE_CLOSE ->
ADVANCE; transition with `run_phase_guard.py phase <PHASE>` and re-invoke.
Never bypass -- set the correct phase.

### SEQ-LIFECYCLE
A Stage 1-2 skill (validate-todo-file / gap-audit-todo / codex-gap-audit)
fired on a mature cursor file (both `Validated:` + `Gap-audited:` preamble
stamps; oracle `stages_1_2_done: true`). Doctrine Stage 0: skip Stages 1-2 --
`run_phase_guard.py phase SECTIONS` and proceed. Exception (genuinely NEW
`## N.` section since the stamps): `run_phase_guard.py relifecycle "<which>"`
then re-invoke.

### SEQ-REDIRECT
Armed unattended run, not yet started: the only valid skill is
`Skill(overnight-sequencer)`. Invoke it now.

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


## CACHE-* (agent_result_cache.py)

### CACHE-HIT
An identical analyst dispatch already ran over content-identical inputs; the
cached report follows the code in the block message. Reuse it (verify
load-bearing file:line claims as usual). Force a fresh run by changing the
prompt; kill switch `AGENT_RESULT_CACHE_DISABLE=1`.

## READ-* (read_cache_block.py)

### READ-CACHED
The requested line range of this file is already covered by an earlier read in
this session, and the file is byte-identical since (sha256 match) -- so the
content is still in context. Scroll up and reuse it; do not pay for a second
copy, which is charged again in the cached prefix on every later turn.

This does NOT fire for a re-read you actually need. Content changed, a failed
Edit on the file, a compaction, a subagent's own read, a new session, a wider
or non-overlapping slice, files under 2 KB, and image/PDF/notebook reads all
pass untouched. If you need a different region, pass `offset`/`limit` for it.

If the block is wrong anyway, it releases itself: the same request is blocked
at most twice, then allowed. Operator escapes are `READ_CACHE_DISABLE=1`
(session kill switch) and the one-shot file `.claude/state/read-cache-override`.

## AGENT-* (agent_coverage_gate.py)

### AGENT-COVERED
A read-only agent already mapped this file earlier in the session and its report
covers it, so reading the whole file again is the "dispatch ADDS a layer" pattern
CLAUDE.md forbids. Reuse the report.

WARNs twice, then BLOCKs. A **slice read is always allowed** -- `Read(offset,
limit)` is the trust-contract verification read (confirming one finding at
file:line before acting), which this gate deliberately protects; only unbounded
whole-file re-reads are gated. Subagents, other sessions, non-covering agent
types, and any path the agent never touched are all exempt.

Kill switch: `AGENT_COVERAGE_DISABLE=1`.

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
