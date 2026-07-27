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

## SEARCH-* (search_offload_gate.py)

### SEARCH-OFFLOAD
A leading file search (`grep`/`rg`/`find` family) run through Bash in the
headless run. Its whole result lands in context and is re-charged as cache-read
on every later turn; the Grep/Glob TOOL returns the same matches without that,
and supports `-n`, `-A`/`-B`/`-C`, `-c`, `-l`, `head_limit` and glob/type
filters.

NOT gated, by design: a search after a `|` (it filters another command's
stdout, which the Grep tool cannot do), a command this hook cannot tokenize, a
grep with no path operand (reads stdin), a compound command whose first real
segment is other work (`sed -i ... && grep ...` -- blocking it would reject the
write), subagent searches, and every interactive session.

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
