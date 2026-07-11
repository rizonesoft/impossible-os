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
oracle-verified `run_phase_guard.py fixpoint`, the operator's `--disarm`, a
declared structural wait (`wait` verb), or a verified `rollover`. The watchdog
relaunches any death, so a voluntary exit accomplishes nothing. Remediation:
re-invoke `Skill(overnight-sequencer)`, run `run_phase_guard.py status`, and
continue from the recorded phase. Doctrine: todo/TODO-Claude-Overnight-Runner.md
"Session-exit policy".

### SEQ-WAIT-READY
A declared wait's artifacts are already complete. Do not stop -- read the
verdict(s), receive the review (`superpowers:receiving-code-review`), run
`run_phase_guard.py wake`, continue.

### SEQ-WAIT-EXPIRED
The declared wait timed out without the artifacts completing. Do not stop --
handle the timeout (re-dispatch the review, or defer the section with the
captured diagnostic), `run_phase_guard.py wake`, continue.

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

## CACHE-* (agent_result_cache.py)

### CACHE-HIT
An identical analyst dispatch already ran over content-identical inputs; the
cached report follows the code in the block message. Reuse it (verify
load-bearing file:line claims as usual). Force a fresh run by changing the
prompt; kill switch `AGENT_RESULT_CACHE_DISABLE=1`.

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
