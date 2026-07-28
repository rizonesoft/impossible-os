#!/usr/bin/env python3
"""
run_phase_guard.py -- the overnight sequencer's no-deviation phase machine.

Enforces the per-file pipeline from todo/TODO-Claude-Overnight-Runner.md so an
unattended `bypassPermissions` run physically cannot skip a stage, reorder the
sequence, ask a human, or stop before fixpoint.

Roles (dispatched on argv[1]):
  pretool   PreToolUse hook: reads {tool_name, tool_input} on stdin.
            exit 0 = allow, exit 2 + stderr = BLOCK.
  stop      Stop hook: for the headless unattended run (env discriminator
            OVERNIGHT_SEQUENCER_RUN=1) while ARMED and not at FIXPOINT, exit 2
            keeps it going so it physically cannot voluntarily stop -- ONLY the
            human's `--disarm` (which removes the armed marker) ends it. The
            watchdog handles real death. Interactive sessions (env unset) are
            never blocked, so the operator is never trapped in the repo.
  CLI      start / status / phase / cursor / progress / next-pass / fixpoint /
            clear / selftest / relifecycle / rollover -- the
            overnight-sequencer skill drives phase transitions (and verified
            context rotations) through these.

State (run cursor only; deferrals live in the TODO files, not here):
  .claude/state/sequencer-run.json
    {active, phase, pass_no, domain, file, section_idx, progress_this_pass,
     started_at, updated_at}

The phase-guard governs the OUTER sequence (you cannot implement/review a
section until the file passed VALIDATE + GAP_AUDIT; you cannot re-validate
mid-section; AskUserQuestion is never allowed). The INNER per-section pipeline
(Codex dispatches, build/test, commit gates) is governed by the existing
within-section hooks -- this guard does not micro-manage it.
"""
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

HOOK_DIR = Path(__file__).resolve().parent


def repo_root():
    d = HOOK_DIR
    while d != d.parent:
        if (d / ".git").is_dir():
            return d
        d = d.parent
    return Path.cwd()


STATE_PATH = repo_root() / ".claude/state/sequencer-run.json"
# Written by arm-sequencer.sh; present means "an unattended sequencer run is
# armed". This is the PERSISTENT MASTER SWITCH: it stays on disk for the entire
# run (it is NOT removed when the run goes active) and is cleared ONLY by the
# human's `--disarm` or by an oracle-verified FIXPOINT. While armed, every fresh
# watchdog-spawned agent is redirected onto overnight-sequencer and cannot
# voluntarily stop. This is what makes the death-thrash impossible: even if the
# run cursor (sequencer-run.json) is reset to inactive out from under the guard,
# the marker keeps the headless run alive and guarded until the operator disarms.
ARMED_MARKER = repo_root() / ".claude/state/sequencer-armed"
# Written ONLY by `run_phase_guard.py fixpoint` AFTER the triage oracle confirms
# zero remaining work. The headless run's Stop is blocked unless this exists, so
# the ONLY graceful self-stop is a machine-verified completion. The agent cannot
# fabricate it: creating/editing it (or the cursor, or the marker) is a blocked
# self-teardown, and `phase FIXPOINT` / `clear` are blocked too. The run cannot
# talk itself into stopping on a reserved decision or a per-file blocker.
FIXPOINT_SENTINEL = repo_root() / ".claude/state/sequencer-fixpoint"


def is_headless():
    """True only inside the unattended systemd-launched run.

    arm-sequencer.sh writes Environment=OVERNIGHT_SEQUENCER_RUN=1 into the
    systemd unit drop-in, so the headless `claude -p` process and all its hook
    subprocesses see it; an interactive operator session in the same repo does
    NOT. This is the load-bearing discriminator: the guard governs ONLY the
    headless run, so it can be absolute there (no stopping, no self-disarm)
    while never trapping the human who shares the repo's settings.json hooks.
    """
    return os.environ.get("OVERNIGHT_SEQUENCER_RUN") == "1"


# Bash command shapes that would let the headless run tear itself down or fake a
# completion. Only the human (interactive session, env discriminator absent) may
# run these. NOTE: `run_phase_guard.py fixpoint` is intentionally NOT blocked --
# it is the legitimate completion path and is itself oracle-gated (it REFUSES
# unless the triage oracle reports zero remaining work), so it cannot be abused.
_STATE_FILES = ("sequencer-armed", "sequencer-run.json", "sequencer-fixpoint")
# touch/cp/dd close the fabricate-the-fixpoint-sentinel bypass (runner-kit
# 2026-07-03: touch/cp/dd wrote the sentinel without tripping the old list).
_MUTATE_OPS = ("rm ", "rm-", "mv ", "unlink", "truncate", "tee ", " > ", ">>",
               "touch ", "cp ", "dd ", "os.remove", "os.unlink", "rmtree",
               "shutil.")


# Codex write/sandbox-escalation markers (mirrors
# codex_review_completed._CODEX_WRITE_FLAGS). A dispatch carrying any of these
# is Codex making CHANGES, not reviewing -- interactive-only per Option A.
_CODEX_WRITE_MARKERS = ("--write", "--full-auto", "--yolo",
                        "--dangerously-bypass-approvals-and-sandbox")
_CODEX_WRITE_SANDBOX = ("workspace-write", "danger-full-access")


def _is_codex_write_dispatch(cmd):
    """True iff cmd is a Codex dispatch AND carries a write marker. Kept
    self-contained (a frozenset/shlex scan) so the guard has no heavy import
    on its hot pretool path."""
    c = cmd or ""
    if not ("codex" in c):  # cheap prefilter; not a codex surface -> not write
        return False
    import shlex
    try:
        toks = shlex.split(c)
    except ValueError:
        toks = c.split()
    # confirm it's actually a codex dispatch surface, not prose mentioning it
    is_codex = any(t == "codex" or t.endswith("codex-companion.mjs")
                   or t.endswith("codex-dispatch.sh")
                   or t.endswith("codex-bg-dispatch.sh")
                   or t.endswith("codex-dispatch-with-files.sh") for t in toks)
    if not is_codex:
        return False
    for i, t in enumerate(toks):
        if t in _CODEX_WRITE_MARKERS:
            return True
        if t in ("--sandbox", "-s"):
            if (toks[i + 1] if i + 1 < len(toks) else "") in _CODEX_WRITE_SANDBOX:
                return True
        if t.startswith("--sandbox=") and t.split("=", 1)[1] in _CODEX_WRITE_SANDBOX:
            return True
    return False


def _is_self_teardown(cmd):
    c = " ".join((cmd or "").split())
    # Disarm via either wrapper.
    if ("arm-sequencer.sh" in c or "overnight-arm.sh" in c) and "--disarm" in c:
        return True
    # Guard CLI used to fake completion or wipe the cursor (fixpoint excluded).
    if "run_phase_guard.py" in c and (" clear" in c or "phase FIXPOINT" in c):
        return True
    # Direct tampering with any guard state file.
    if any(f in c for f in _STATE_FILES) and any(op in c for op in _MUTATE_OPS):
        return True
    # Stopping/disabling the systemd units or killing the headless claude.
    if "systemctl" in c and any(op in c for op in ("stop", "disable", "kill")) and "overnight" in c:
        return True
    if re.search(r"\b(pkill|killall|kill)\b", c) and "claude" in c:
        return True
    return False

# Ordered phases of the per-file pipeline.
PHASES = ["PREFLIGHT", "TRIAGE", "VALIDATE", "GAP_AUDIT", "SECTIONS",
          "FILE_CLOSE", "ADVANCE", "FIXPOINT"]

# Skills the sequencer's OUTER pipeline controls. A controlled skill may only
# run in a phase whose allow-set lists it; any other controlled skill in the
# wrong phase is a deviation -> BLOCK. Skills NOT in this set (the inner-pipeline
# helpers implement/review invoke) pass freely -- the within-section gates own
# them.
SEQUENCE_SKILLS = {
    "validate-todo-file", "gap-audit-todo", "codex-gap-audit",
    "implement-todo-section", "implement-todo-item", "implement-ssdt-range",
    "review-todo-section", "verify-todo-section", "complete-todo-file",
    "overnight-todo-runner",  # legacy System A: never inside a sequencer run
}

PHASE_ALLOWED_SKILLS = {
    "PREFLIGHT": set(),
    "TRIAGE": set(),
    "VALIDATE": {"validate-todo-file"},
    "GAP_AUDIT": {"gap-audit-todo", "codex-gap-audit"},
    # SECTIONS allows the per-section drivers; their internal pipeline skills
    # are not in SEQUENCE_SKILLS so they pass freely.
    "SECTIONS": {"implement-todo-section", "implement-todo-item",
                 "implement-ssdt-range", "review-todo-section",
                 "verify-todo-section"},
    "FILE_CLOSE": {"complete-todo-file", "review-todo-section",
                   "verify-todo-section"},
    "ADVANCE": set(),
    "FIXPOINT": set(),
}

# Stage 1-2 skills, blocked on a cursor file that already carries BOTH
# file-preamble stamps (`> **Validated:**` + `> **Gap-audited:**`). The oracle
# reports this as `stages_1_2_done: true`; doctrine Stage 0 says SKIP straight
# to SECTIONS. Re-running them (each with a Codex pass) on a mature file was
# the single largest source of unnecessary phases. Escape hatch for the
# genuinely-new-section case: `run_phase_guard.py relifecycle <reason>` sets a
# one-shot override consumed by the next Stage 1-2 skill invocation.
LIFECYCLE_SKILLS = {"validate-todo-file", "gap-audit-todo", "codex-gap-audit"}

# R1 (2026-07-19): the skills that START a new section work unit. When a
# ship-stamp commit postdates the last verified rotation, invoking any of
# these in the SAME worker is the un-rotated section-to-section advance the
# flow invariant forbids -- the only legal next step is `rollover`. The
# review/close skills are deliberately NOT here: review-todo-section MUST run
# post-ship (it writes the stamps), and complete-todo-file closes the file
# in-session (the cursor gate still forces rotation before the next file).
R1_SECTION_STARTERS = {"implement-todo-section", "implement-todo-item",
                       "implement-ssdt-range"}


def load_state():
    if not STATE_PATH.exists():
        return {"active": False}
    try:
        with STATE_PATH.open(encoding="utf-8") as fh:
            return json.load(fh)
    except (json.JSONDecodeError, OSError):
        return {"active": False}


def save_state(state):
    """ATOMIC write (tmp + os.replace). Truncate-and-write let a concurrent
    reader (watcher poll, launcher gate) see partial JSON and misread it as
    'no active wait' -- which bypasses an unfinished review (2026-07-11)."""
    STATE_PATH.parent.mkdir(parents=True, exist_ok=True)
    tmp = STATE_PATH.with_suffix(f".{os.getpid()}.tmp")
    with tmp.open("w", encoding="utf-8") as fh:
        json.dump(state, fh, indent=1)
        fh.flush()
        os.fsync(fh.fileno())
    os.replace(str(tmp), str(STATE_PATH))


def _skill_name(tool_input):
    return tool_input.get("skill") or tool_input.get("name") or ""


def evaluate(tool_name, tool_input, state, armed=False, headless=False,
             stages_done=False, ship_pending=""):
    """Return (allow: bool, message: str). Pure -- unit-testable.

    The guard governs ONLY the headless unattended run. An interactive operator
    session (headless=False) is never constrained -- it can ask, stop, and run
    --disarm freely. This is what lets the human share the repo's hooks without
    being trapped by the run cursor on disk.

    `stages_done` is the cursor file's `stages_1_2_done` verdict (computed by
    the caller only when the tool is a Stage 1-2 skill; False otherwise).
    `ship_pending` is the sha of a ship-stamp commit newer than the last
    verified rotation ("" if none; computed by the caller only when the tool
    is an R1 section-starter skill).
    """
    if not headless:
        return True, ""

    active = bool(state.get("active"))
    if not active and not armed:
        return True, ""

    # AskUserQuestion is never allowed inside the unattended run.
    if tool_name == "AskUserQuestion":
        return False, (
            "[SEQ-ASK] blocked: decide conservatively + log the assumption, or "
            "defer ([/] + Deferred awaiting-answer + XREF) and advance. "
            "Details: docs/infrastructure/hook-codes.md#seq-ask")

    # The unattended run must never tear itself down. Disarm/clear/stop is a
    # human-only operation, performed from an interactive session (where the
    # OVERNIGHT_SEQUENCER_RUN discriminator is absent and this guard is inert).
    if tool_name == "Bash" and _is_self_teardown(tool_input.get("command", "")):
        return False, (
            "[SEQ-TEARDOWN] blocked: disarm/clear/stop is human-only. Keep "
            "going -- continue the pipeline. "
            "Details: docs/infrastructure/hook-codes.md#seq-teardown")

    # Codex WRITE is interactive-only (Option A, 2026-07-11). The unattended
    # run dispatches READ-ONLY reviews; a write-capable Codex mutating the
    # tree with no per-step human authorship crosses the autonomous-agent
    # boundary (CLAUDE.md). The headless main session is physically blocked
    # from write dispatches; interactive sessions (guard inert) are not.
    if tool_name == "Bash" and _is_codex_write_dispatch(tool_input.get("command", "")):
        return False, (
            "[SEQ-CODEX-WRITE] blocked: Codex write (`task --write` / sandbox "
            "override) is INTERACTIVE-ONLY (autonomous-agent boundary). The "
            "unattended run uses read-only reviews only. Implement the change "
            "yourself and dispatch a read-only review, or defer for an "
            "operator's interactive rescue. "
            "Details: docs/infrastructure/hook-codes.md#seq-codex-write")

    # Armed but not yet started: force the redirect onto overnight-sequencer.
    if not active and armed:
        if tool_name == "Skill":
            sk = _skill_name(tool_input)
            if sk == "overnight-sequencer":
                return True, ""
            return False, (
                "[SEQ-REDIRECT] armed run: invoke Skill(overnight-sequencer) "
                "now (no other skill first). "
                "Details: docs/infrastructure/hook-codes.md#seq-redirect")
        return True, ""  # Bash / Read / Grep / Glob allowed for setup

    # Active run: phase enforcement. (AskUserQuestion already handled above.)
    phase = state.get("phase", "PREFLIGHT")

    # Lifecycle routing (Stage 0): a mature file (both preamble stamps) never
    # re-runs Stage 1-2 -- the oracle already said so via stages_1_2_done, and
    # honoring it removes whole VALIDATE/GAP_AUDIT phases (each with a Codex
    # pass). One-shot escape: `relifecycle <reason>` for a genuinely new
    # `## N.` section (doctrine Stage 0 exception).
    if tool_name == "Skill":
        sk = _skill_name(tool_input)
        if (sk in LIFECYCLE_SKILLS and stages_done
                and not state.get("lifecycle_override")):
            return False, (
                f"[SEQ-LIFECYCLE] Skill({sk}) blocked: cursor file is mature "
                "(both preamble stamps). Run `run_phase_guard.py phase "
                "SECTIONS` and proceed; for a genuinely NEW section, "
                "`relifecycle \"<which>\"` first. "
                "Details: docs/infrastructure/hook-codes.md#seq-lifecycle")

    # Sequence-skill ordering: a controlled skill may only fire in a phase
    # that allows it.
    if tool_name == "Skill":
        sk = _skill_name(tool_input)
        if sk in SEQUENCE_SKILLS and sk not in PHASE_ALLOWED_SKILLS.get(phase, set()):
            allowed = sorted(PHASE_ALLOWED_SKILLS.get(phase, set())) or ["(none)"]
            return False, (
                f"[SEQ-PHASE] Skill({sk}) out of sequence in {phase} "
                f"(allowed: {', '.join(allowed)}). Set the correct phase via "
                "`run_phase_guard.py phase <PHASE>` and re-invoke. "
                "Details: docs/infrastructure/hook-codes.md#seq-phase")

    # R1 (2026-07-19): rollover-required-after-ship, at the SECTION-START
    # chokepoint. The cursor verb only sees FILE-to-file moves (the SECTIONS
    # loop never re-calls `cursor` between sections of one file -- review
    # 2026-07-19 finder C), so the enforcement lives here: starting a NEW
    # section work unit after an un-rotated ship is blocked. Deferrals leave
    # no ship stamp; the fresh post-rotation worker's ship predates its
    # last_rollover_epoch -- both pass.
    if tool_name == "Skill" and ship_pending:
        sk = _skill_name(tool_input)
        if sk in R1_SECTION_STARTERS:
            return False, (
                f"[SEQ-R1] Skill({sk}) blocked: ship-stamp commit "
                f"{ship_pending[:12]} landed after the last verified rotation. "
                "A fully-shipped section ENDS the worker context: run "
                "`python3 .claude/hooks/run_phase_guard.py rollover` and END "
                "the turn -- the watchdog relaunches a fresh worker that "
                "resumes at the next section. Operator override: "
                "`run_phase_guard.py clear`.")

    # Everything else (Bash, Edit, Write, Read, Grep, Glob, inner-pipeline
    # skills) passes -- the within-section gates govern it.
    return True, ""


def _cursor_stages_done(state):
    """Live stages_1_2_done verdict for the cursor file, via the triage
    oracle's file_lifecycle. Fail-open (False) on any error -- a broken
    oracle must not block Stage 1-2 work."""
    rel = state.get("file")
    if not rel:
        return False
    try:
        sys.path.insert(0, str(HOOK_DIR))
        import sequencer_triage
        lc = sequencer_triage.file_lifecycle(str(repo_root() / rel))
        return bool(lc.get("validated") and lc.get("gap_audited"))
    except Exception:
        return False


def handle_pretool():
    try:
        d = json.load(sys.stdin)
    except (json.JSONDecodeError, ValueError):
        return 0
    tool_name = d.get("tool_name", "")
    tool_input = d.get("tool_input", {})
    state = load_state()
    stages_done = False
    if (is_headless() and state.get("active") and tool_name == "Skill"
            and _skill_name(tool_input) in LIFECYCLE_SKILLS):
        stages_done = _cursor_stages_done(state)
    ship_pending = ""
    if (is_headless() and state.get("active") and tool_name == "Skill"
            and _skill_name(tool_input) in R1_SECTION_STARTERS):
        ship_pending = _ship_stamp_since(repo_root(),
                                         state.get("last_rollover_epoch"))
    allow, msg = evaluate(tool_name, tool_input, state,
                          armed=ARMED_MARKER.exists(),
                          headless=is_headless(), stages_done=stages_done,
                          ship_pending=ship_pending)
    if allow:
        # Consume the one-shot relifecycle override when a Stage 1-2 skill
        # actually fires against a mature file under it.
        if (stages_done and state.get("lifecycle_override")
                and tool_name == "Skill"
                and _skill_name(tool_input) in LIFECYCLE_SKILLS):
            state.pop("lifecycle_override", None)
            save_state(state)
        return 0
    sys.stderr.write(msg)
    return 2


# ---- verified rollover (context rotation) ----------------------------------
#
# ROLLOVER: after a section is shipped+reviewed+pushed+stamped, a fresh worker
# context is cheaper than a long-tail one. `rollover` machine-verifies the
# checkpoint (clean tree, pushed, graph rebuild OK, content-bound receipts, no
# outstanding jobs) and only then permits ONE clean session exit; the *:0/10
# watchdog relaunches a fresh session on its next tick. The RUN stays active --
# only the worker context rotates (doctrine: run-active vs context-rotates).
# There is NO custom relaunch watcher: the watchdog is the sole, systemd-owned
# relaunch mechanism (the structural-wait watcher apparatus was removed
# 2026-07-11 after it deadlocked the runner -- a session polling a review
# in-session in a single blocking Bash call costs ~0 tokens, so exit-and-wake
# was solving a non-problem while adding a fatal process-lifecycle bug class).

ROLLOVER_PENDING_FRESH_S = 1800  # stale pending flags stop permitting stops


# B4/F2: deterministically-regenerated auto-gen docs. The runner's own
# full-suite / graph runs rewrite these, dirtying the tree; blocking the rollover
# on them forced a separate commit + receipt re-record every time (F2), and the
# bare "N change(s)" message mis-attributed them to the operator (B4). They carry
# no uncommitted WORK a fresh worker must not inherit -- the next run regenerates
# them identically -- so a delta consisting ONLY of these does not block rollover.
_ROLLOVER_AUTOGEN = frozenset({
    "docs/test-coverage/coverage.json",
    "docs/test-coverage/coverage.md",
    "COUNT.md",
    "docs/infrastructure/todo-graph.md",
})


def _dirty_path(porcelain_line: str) -> str:
    """Path from a `git status --porcelain` line ('XY <path>' / '?? <path>',
    rename 'XY old -> new')."""
    p = porcelain_line[3:].strip() if len(porcelain_line) > 3 else porcelain_line
    return p.split(" -> ")[-1].strip().strip('"')


# An XREF stamp's line-number tail: `... (item: "NAME" at line 467)`.
_XREF_LINENO_RE = re.compile(r"\bat line \d+\b")
# Owners a rollover may proceed past. Everything else is real uncommitted work.
_TOLERATED_OWNERS = ("auto-gen", "xref-repair")


def _is_xref_lineno_repair(root: Path, path: str) -> bool:
    """True iff `path`'s UNSTAGED diff changes NOTHING but `at line N` integers
    inside XREF stamps.

    WHY (live wedge, 2026-07-28 03:42). The run refused a verified rollover with
    "blocked by the operator's in-flight TODO-25 edit". No operator had touched
    TODO-25: the dirty content was four XREF line-number bumps (467->468,
    302->303, 463->464, 464->465), every one pointing into TODO-21 -- the file
    the run had just edited, shifting those very lines. The run's OWN tooling
    (`stranded_deferrals.py --owner`, `todo-graph/build-and-validate.sh`) had
    emitted them minutes earlier, and the rollover check read the result as a
    human's work and declined to touch it. That misattribution is SELF-
    SUSTAINING: every section that shifts a cross-referenced line re-dirties a
    TODO the run will again refuse to own, so the rollover never fires without a
    human committing for it (it took operator commit `0f4d6995` to unblock).

    This is the item's blunter fallback -- classify by CONTENT rather than by
    tracking which script wrote what -- and it is deliberately strict: every
    removed line must pair with an added line that is byte-identical once the
    `at line N` integer is normalised, AND both must be XREF stamps. A hunk that
    adds or removes lines, touches anything outside an XREF, or changes a word
    fails the test and still blocks. Real section work does not have this shape.

    Tolerating (not committing) matches the existing auto-gen precedent: the
    repair stays unstaged, the next session regenerates it identically, and a
    later real commit sweeps it in. A rollover GATE that commits on a run's
    behalf would be a far larger behaviour change than the wedge warrants.
    """
    try:
        out = subprocess.run(["git", "diff", "--unified=0", "--", path],
                             cwd=str(root), capture_output=True, text=True,
                             timeout=30)
        if out.returncode != 0:
            return False
        removed, added = [], []
        for ln in out.stdout.splitlines():
            if ln.startswith(("+++", "---", "@@", "diff ", "index ")):
                continue
            if ln.startswith("-"):
                removed.append(ln[1:])
            elif ln.startswith("+"):
                added.append(ln[1:])
        if not removed or len(removed) != len(added):
            return False
        for old, new in zip(removed, added):
            if old == new:
                return False              # a pair that did not change: not a repair
            if "XREF:" not in old or "XREF:" not in new:
                return False
            if not _XREF_LINENO_RE.search(old) or not _XREF_LINENO_RE.search(new):
                return False
            if (_XREF_LINENO_RE.sub("at line N", old)
                    != _XREF_LINENO_RE.sub("at line N", new)):
                return False
        return True
    except Exception:
        return False                      # fail-closed: unsure -> keep blocking


def _dirty_owner(porcelain_line: str, root: Path | None = None) -> str:
    """Classify a `git status --porcelain` line by its XY status (F4, C-RECV-class
    review 2026-07-14). ONLY an UNSTAGED modification (` M`) of an allowlisted
    auto-gen file -- or of a todo/*.md whose diff is pure XREF line-number repair
    (2026-07-28) -- is tolerated. A STAGED change (index column set), a delete /
    rename / copy, an unmerged/conflicted entry (UU/AA/DD/AU/UA/UD/DU), or an
    untracked file is NEVER tolerated -- even on an allowlisted path -- because
    each is uncommitted index state or new content a rotation could strand or
    misinterpret. The old pathname-only test tolerated all of these on
    coverage.*/COUNT.md/todo-graph.md.

    `root` is optional so existing callers and tests keep working; without it the
    content-based XREF check is skipped and behaviour is exactly as before."""
    if porcelain_line.startswith("??"):
        return "untracked"
    xy = (porcelain_line[:2] + "  ")[:2]
    x, y = xy[0], xy[1]
    if "U" in xy or xy in ("AA", "DD"):
        return "conflict"
    # anything staged (X is a real op) or an unstaged delete/rename/copy blocks.
    if x != " " or y in ("D", "R", "C"):
        return "tracked-source"
    if y == "M":
        path = _dirty_path(porcelain_line)
        # the ONLY tolerated cases: an unstaged modification of a tracked
        # generated file, or a TODO dirtied solely by the run's own XREF
        # line-number repairs.
        if path in _ROLLOVER_AUTOGEN:
            return "auto-gen"
        if (root is not None and path.startswith("todo/")
                and path.endswith(".md") and _is_xref_lineno_repair(root, path)):
            return "xref-repair"
    return "tracked-source"


def _rollover_failures(root: Path, state: dict) -> list:
    """Machine gates for a verified rollover checkpoint. Every failure is a
    reason the worker context may NOT rotate yet."""
    fails = []
    try:
        # UNTRACKED FILES COUNT (-uall): a stranded new file is uncommitted
        # state a fresh worker must not inherit silently. The old `-uno`
        # called a tree with untracked junk "clean" (2026-07-11).
        out = subprocess.run(["git", "status", "--porcelain", "-uall"],
                             cwd=str(root), capture_output=True, text=True,
                             timeout=30)
        if out.returncode != 0:
            fails.append("git status unavailable")
        elif out.stdout.strip():
            dirty = [ln for ln in out.stdout.splitlines() if ln.strip()]
            # B4/F2: tolerate a delta that is ONLY auto-gen docs; block on any
            # real change and NAME the paths + probable owner so the diagnostic
            # never mis-attributes the runner's own coverage.* to the operator.
            owners = {ln: _dirty_owner(ln, root) for ln in dirty}
            blocking = [ln for ln in dirty
                        if owners[ln] not in _TOLERATED_OWNERS]
            if blocking:
                named = ", ".join(f"{_dirty_path(ln)} [{owners[ln]}]"
                                  for ln in blocking[:6])
                more = f" (+{len(blocking) - 6} more)" if len(blocking) > 6 else ""
                tolerated = len(dirty) - len(blocking)
                autogen_note = (f"; tolerating {tolerated} pipeline-output "
                                f"delta(s)" if tolerated else "")
                fails.append(f"tree not clean ({len(blocking)} blocking "
                             f"change(s): {named}{more}{autogen_note})")
            # else: only pipeline output dirty -> tolerated, rollover may proceed.
    except Exception:
        fails.append("git status unavailable")
    try:
        out = subprocess.run(["git", "rev-list", "--count", "@{u}..HEAD"],
                             cwd=str(root), capture_output=True, text=True,
                             timeout=30)
        if out.returncode != 0:
            fails.append("no upstream configured (cannot verify pushed)")
        elif out.stdout.strip() != "0":
            fails.append(f"{out.stdout.strip()} commit(s) not pushed")
    except Exception:
        fails.append("git rev-list unavailable")
    try:
        out = subprocess.run(
            ["bash", "scripts/todo-graph/build-and-validate.sh", "--keep-cache"],
            cwd=str(root), capture_output=True, text=True, timeout=300)
        if out.returncode != 0:
            fails.append("todo-graph build-and-validate failed")
    except Exception:
        fails.append("todo-graph rebuild unavailable")
    # Content-bound build receipt: green build over the CURRENT build inputs.
    try:
        import importlib.util
        spec = importlib.util.spec_from_file_location(
            "overnight_receipts", str(root / "scripts/overnight/receipts.py"))
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        ok, why = mod.check_build(root)
        if not ok:
            fails.append(f"build receipt not content-valid ({why})")
        # Test + smoke receipts: a rollover asserts a green checkpoint, so the
        # test suite (and, for boot-path work, the smoke test) must be
        # content-valid over the current tree, not just the build.
        try:
            t_ok, t_why = mod.check_suite(root, "all")
            if not t_ok:
                fails.append(f"test receipt not content-valid ({t_why})")
        except Exception:
            fails.append("test receipt missing (record with receipts.py "
                         "record-suite . all after a green test.sh)")
        try:
            # Prefer the image-bound smoke receipt; accept the legacy suite
            # "smoke" receipt during migration. Fail-closed if neither is valid.
            s_ok, s_why = mod.check_smoke(root)
            if not s_ok:
                l_ok, _ = mod.check_suite(root, "smoke")
                if not l_ok:
                    fails.append(f"smoke receipt not content-valid ({s_why})")
        except Exception:
            fails.append("smoke receipt missing (record with receipts.py "
                         "record-smoke . after a green test-smoke.sh)")
    except Exception as exc:  # noqa: BLE001
        fails.append(f"build/test receipt check unavailable ({exc})")
    # No outstanding review obligation. Fail CLOSED on unreadable state.
    review_state = root / ".claude/state/last-codex-review.json"
    if review_state.exists():
        try:
            rs = json.loads(review_state.read_text())
            if rs.get("received") is not True:
                fails.append("outstanding Codex review not yet received")
        except (OSError, ValueError):
            fails.append("last-codex-review.json unreadable (fail-closed)")
    # No outstanding background jobs: a running codex-review or seq-watcher
    # unit means work is in flight the rollover would strand.
    try:
        out = subprocess.run(
            ["systemctl", "--user", "list-units", "--state=active",
             "--no-legend", "codex-rev-*", "seq-watcher-*"],
            capture_output=True, text=True, timeout=10)
        jobs = [ln.split()[0] for ln in out.stdout.splitlines() if ln.strip()]
        if jobs:
            fails.append(f"outstanding background job(s) still active: "
                         f"{', '.join(jobs[:4])}")
    except Exception:
        pass  # systemctl absent -> cannot enumerate; not a hard fail
    return fails


def _unpushed_count(root: Path):
    """A1 (review 2026-07-14): number of local commits ahead of the upstream, or
    None if it cannot be determined (no upstream / git error). Used to separate a
    genuine MID-section WIP boundary (committed-but-unpushed local work -- the WIP
    rotation does not push) from a POST-ship boundary (the ship path pushes, so
    everything is upstream). A WIP rotation is only meaningful in the former; in
    the latter the full `rollover` (with its receipt/graph verification) is the
    correct exit. None -> caller treats as 'cannot prove unshipped WIP' (refuse)."""
    try:
        r = subprocess.run(["git", "-C", str(root), "rev-list", "--count",
                            "@{u}..HEAD"], capture_output=True, text=True, timeout=10)
    except Exception:
        return None
    if r.returncode != 0:
        return None
    try:
        return int(r.stdout.strip())
    except (ValueError, TypeError):
        return None


# A4 (review 2026-07-14 round 2): the review-gate's own definition of a ship stamp
# (kept in sync with section_commit_gate._STAMP_ADDED_RE). A section-SHIP/stamp
# commit adds a `**Verified:**` / `**Quality reviewed:**` line to a todo/ file.
_SHIP_STAMP_RE = re.compile(r"^\+\s{0,3}(?:>\s*)+\*\*(Verified|Quality reviewed):\*\*")


def _head_adds_ship_stamp(root: Path) -> bool:
    """A4: True if HEAD's commit touches a review ship-stamp in a todo/ file -- i.e.
    HEAD is a section-SHIP/stamp commit, not mid-section WIP. Robust vs the loose
    section_idx (which is only a display value): keys on the definitive stamp the
    review-gate recognizes. This closes the stamped-but-not-yet-pushed window the
    unpushed>0 guard cannot see (progress/section_shipped is only set post-push).
    Fail-open (False) on a git error -- the unpushed guard fail-CLOSES on the same
    error, so the pair stays safe."""
    try:
        r = subprocess.run(["git", "-C", str(root), "show", "--format=", "HEAD",
                            "--", "todo"], capture_output=True, text=True, timeout=10)
    except Exception:
        return False
    if r.returncode != 0:
        return False
    return any(_SHIP_STAMP_RE.match(ln) for ln in r.stdout.splitlines())


def _ship_stamp_since(root: Path, epoch) -> str:
    """R1 (2026-07-19): sha of a commit STRICTLY NEWER than `epoch` that adds a
    review ship-stamp to a todo/ file, or '' if none. A ship-stamp commit after
    the last verified rotation means the worker is about to advance un-rotated
    (P3.2 only catches a REFUSED rollover; a never-ATTEMPTED one sailed through
    -- measured 2026-07-19: multi-section sessions reached ~600K context and
    cache reads were 75% of the night's cost). Bounded to the newest 30 commits
    (a single worker session never ships more). Fail-open ('') on missing epoch
    or any git error: a broken git / mid-migration state must not wedge the
    cursor -- the Stop hook still holds the session either way."""
    if not isinstance(epoch, (int, float)) or epoch <= 0:
        return ""
    try:
        # One bounded walk over todo/-touching commits newer than the rotation
        # (--since bounds the traversal itself, so this is cheap on the linear
        # runner history; the python-side ct compare stays as the exact belt).
        # Review 2026-07-19: the first cut walked `git log -30` + one `git
        # show` per commit -- a fixed window unrelated commits could push the
        # stamp out of, at up to 31 subprocess spawns per check.
        since = time.strftime("%Y-%m-%dT%H:%M:%S +0000",
                              time.gmtime(int(epoch)))
        r = subprocess.run(["git", "-C", str(root), "log", "--since", since,
                            "-n", "50", "--format=%H %ct", "--", "todo"],
                           capture_output=True, text=True, timeout=10)
        if r.returncode != 0:
            return ""
        for ln in r.stdout.splitlines():
            parts = ln.split()
            if len(parts) != 2:
                continue
            sha, ct = parts[0], int(parts[1])
            if ct <= epoch:
                continue  # same-second/older commit slipped the --since bound
            show = subprocess.run(["git", "-C", str(root), "show", "--format=",
                                   sha, "--", "todo"],
                                  capture_output=True, text=True, timeout=10)
            if show.returncode == 0 and any(
                    _SHIP_STAMP_RE.match(l) for l in show.stdout.splitlines()):
                return sha
        return ""
    except Exception:
        return ""


_REVIEW_RESOLUTION_REL = ".claude/state/last-review-resolution.json"


def _build_suite_receipts_ok(root: Path):
    """A5 (review 2026-07-14 round 2): (ok, why) for content-valid build + test
    receipts over the CURRENT tree -- the 'owning green verification' a resolved
    review boundary requires. Smoke is NOT required (a mid-section rotation is not
    a ship; smoke stays on the full rollover). Fail-CLOSED."""
    try:
        import importlib.util
        spec = importlib.util.spec_from_file_location(
            "overnight_receipts", str(root / "scripts/overnight/receipts.py"))
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
    except Exception as exc:  # noqa: BLE001
        return False, f"receipts.py unavailable ({exc})"
    try:
        ok, why = mod.check_build(root)
        if not ok:
            return False, f"build receipt not content-valid ({why})"
        ok, why = mod.check_suite(root, "all")
        if not ok:
            return False, f"test receipt not content-valid ({why})"
    except Exception as exc:  # noqa: BLE001
        return False, ("build/test receipt missing (record with receipts.py after "
                       f"a green build + test.sh) ({exc})")
    return True, ""


def _review_resolution_valid(root: Path):
    """A5: (ok, why). A mid-section WIP rotation must stand on a RESOLVED review
    cycle proven green at THIS HEAD, not merely a received:true bit. The
    `review-resolved` verb writes a content-bound receipt {head, ...}; this requires
    it to match the current HEAD. Fail-CLOSED (absent/stale/unreadable -> refuse)."""
    try:
        head = subprocess.run(["git", "-C", str(root), "rev-parse", "HEAD"],
                              capture_output=True, text=True,
                              timeout=10).stdout.strip()
    except Exception:
        return False, "cannot read HEAD (fail-closed)"
    p = root / _REVIEW_RESOLUTION_REL
    if not p.exists():
        return False, ("no review-resolution receipt -- run `review-resolved` to "
                       "prove the review cycle is resolved + green at this HEAD")
    try:
        rr = json.loads(p.read_text())
    except (OSError, ValueError):
        return False, "review-resolution receipt unreadable (fail-closed)"
    if not head or rr.get("head") != head:
        return False, (f"review-resolution receipt is for "
                       f"{str(rr.get('head'))[:12]}, HEAD is {head[:12]} -- re-run "
                       "`review-resolved` at HEAD after resolving the review cycle")
    return True, ""


def _write_section_checkpoint(root: Path) -> bool:
    """Write the durable section checkpoint; return True iff it succeeded (rc==0).
    Extracted so the F2 fail-closed behavior in rollover-wip is unit-testable."""
    try:
        cp = subprocess.run(
            ["python3", str(root / "scripts/overnight/section-checkpoint.py"),
             "write"], cwd=str(root), capture_output=True, timeout=30)
        return cp.returncode == 0
    except Exception:
        return False


def _review_not_binding_head(root: Path, rs: dict) -> str:
    """F3 (C-RECV-class review 2026-07-14): return a failure string if the
    received review's `trigger_blobs` do NOT match the current HEAD source, so a
    stale received:true from a prior round cannot pass a WIP rotation as a
    'reviewed' boundary. Every reviewed file must be present in the committed HEAD
    with the exact blob the reviewer saw. Fail-CLOSED: empty blobs or a git error
    is a failure."""
    blobs = rs.get("trigger_blobs")
    if not isinstance(blobs, dict) or not blobs:
        return ("received review has no content binding (trigger_blobs) -- cannot "
                "prove the committed WIP was reviewed")
    for path, recorded in blobs.items():
        try:
            r = subprocess.run(["git", "-C", str(root), "rev-parse", f"HEAD:{path}"],
                               capture_output=True, text=True, timeout=10)
        except Exception:
            return f"cannot verify review binding for {path} (git error, fail-closed)"
        if r.returncode != 0:
            return (f"reviewed file {path} is not in the committed HEAD -- the WIP "
                    f"was not committed as reviewed")
        head_sha = r.stdout.strip()
        rec = str(recorded)
        # trigger_blobs may be a bare sha or a `mode:sha` (B3); compare the sha.
        rec_sha = (rec.split(":", 1)[1]
                   if ":" in rec and rec.split(":", 1)[0].isdigit() else rec)
        if head_sha != rec_sha:
            return (f"reviewed file {path} differs from the committed HEAD -- the "
                    f"received review is stale relative to the WIP commit")
    return ""


def _rollover_failures_wip(root: Path, state: dict) -> list:
    """P4.5: gate for a MID-SECTION (context-cap) rotation. The shipped
    _rollover_failures() above -- which governs the SECTION-BOUNDARY ship rollover
    -- is deliberately NOT touched. This PARALLEL gate accepts a WIP tree
    (committed but UNPUSHED, UNSTAMPED, no green receipts: the section is still in
    progress) and keeps ONLY the checks that protect against LOSING or CORRUPTING
    work across a rotation:

      (1) the working tree is CLEAN (auto-gen tolerated, same B4/F2 classifier) --
          the WIP MUST be committed, else the rotation strands uncommitted edits;
      (2) NO outstanding Codex review -- a pending review must be received first,
          else the fresh worker inherits an un-triaged review;
      (3) NO active background job -- a running codex-review would be stranded.

    It intentionally DROPS the pushed / receipt / todo-graph checks (a mid-section
    WIP is not ship-ready and must not be forced to be). The checks are DUPLICATED
    from the ship gate on purpose -- refactoring the ship gate is the one thing
    P4.5 must not risk. Fail-CLOSED: unreadable review state is a failure."""
    fails = []
    # (1) clean tree (WIP committed), auto-gen tolerated.
    try:
        out = subprocess.run(["git", "status", "--porcelain", "-uall"],
                             cwd=str(root), capture_output=True, text=True,
                             timeout=30)
        if out.returncode != 0:
            fails.append("git status unavailable")
        elif out.stdout.strip():
            dirty = [ln for ln in out.stdout.splitlines() if ln.strip()]
            blocking = [ln for ln in dirty if _dirty_owner(ln) != "auto-gen"]
            if blocking:
                named = ", ".join(f"{_dirty_path(ln)} [{_dirty_owner(ln)}]"
                                  for ln in blocking[:6])
                fails.append(f"WIP not committed -- tree not clean "
                             f"({len(blocking)} change(s): {named}); commit the "
                             f"WIP before a mid-section rotation")
    except Exception:
        fails.append("git status unavailable")
    # (2) no outstanding review (fail-CLOSED on unreadable), AND (F3) a received
    # review must actually BIND the committed HEAD content -- a stale global
    # received:true from a prior fix-loop round does not prove the committed WIP
    # was reviewed.
    review_state = root / ".claude/state/last-codex-review.json"
    if review_state.exists():
        try:
            rs = json.loads(review_state.read_text())
        except (OSError, ValueError):
            rs = None
            fails.append("last-codex-review.json unreadable (fail-closed)")
        if isinstance(rs, dict):
            if rs.get("received") is not True:
                fails.append("outstanding Codex review not yet received")
            else:
                bad = _review_not_binding_head(root, rs)
                if bad:
                    fails.append(bad)
    # (3) no active background job (a running codex-review / watcher).
    try:
        out = subprocess.run(
            ["systemctl", "--user", "list-units", "--state=active",
             "--no-legend", "codex-rev-*", "seq-watcher-*"],
            capture_output=True, text=True, timeout=10)
        jobs = [ln.split()[0] for ln in out.stdout.splitlines() if ln.strip()]
        if jobs:
            fails.append(f"outstanding background job(s) still active: "
                         f"{', '.join(jobs[:4])}")
    except Exception:
        pass  # systemctl absent -> cannot enumerate; not a hard fail
    return fails


def _live_oracle_status():
    """Best-effort live triage-oracle verdict for the Stop fallback.

    Returns the oracle's `status` string (DONE / BLOCKED / NEEDS_WORK) or None
    on any failure -- callers treat None as "keep blocking" (fail-closed).
    """
    try:
        out = subprocess.run(
            [sys.executable, str(repo_root() / ".claude/hooks/sequencer_triage.py"),
             "--next"],
            capture_output=True, text=True, timeout=60, cwd=str(repo_root()))
        if out.returncode != 0:
            return None
        return json.loads(out.stdout.strip()).get("status")
    except Exception:
        return None


def _derive_section_idx(todo_path):
    """Section number the cursor is moving to, or None if it cannot be derived.

    The current section is the first one the triage oracle still classes
    NEEDS_WORK -- the same ordering the sequencer itself routes on, so the
    recorded index matches the section the worker is about to implement.

    Purely for metrics attribution: EVERY failure path returns None so the
    caller keeps the previous value. A cost-reporting field must never be able
    to fail a `cursor` move and wedge a run.
    """
    try:
        out = subprocess.run(
            [sys.executable, str(repo_root() / ".claude/hooks/sequencer_triage.py"),
             "--classify", str(todo_path)],
            capture_output=True, text=True, timeout=60, cwd=str(repo_root()))
        if out.returncode != 0:
            return None
        secs = json.loads(out.stdout.strip()).get("sections")
        if not isinstance(secs, list):
            return None
        for s in secs:
            if isinstance(s, dict) and s.get("class") == "NEEDS_WORK":
                n = s.get("n")
                return n if isinstance(n, int) else None
        return None                    # every section done: leave the cursor be
    except Exception:
        return None


def handle_stop():
    # The guard governs ONLY the headless unattended run -- an interactive
    # operator session is never trapped (it can stop and disarm freely).
    if not is_headless():
        return 0
    # The ONLY graceful self-stop is an oracle-verified FIXPOINT, recorded by an
    # exclusive sentinel that `run_phase_guard.py fixpoint` writes only after the
    # triage oracle confirms zero remaining work. We key on the sentinel (and the
    # immutable env discriminator), NOT on any value the agent can edit: it
    # cannot fake the sentinel (creating it is a blocked self-teardown), cannot
    # disarm itself, and cannot clear the cursor. So no reserved decision, stale
    # blocker, or per-file block can ever talk the run into stopping.
    if FIXPOINT_SENTINEL.exists():
        return 0
    # The human's --disarm removes the master switch (and kills the service); if
    # the marker is already gone, the run has been disarmed -- allow the stop.
    if not ARMED_MARKER.exists():
        return 0
    state_early = load_state()
    # VERIFIED ROLLOVER: the `rollover` verb machine-verified the checkpoint
    # (clean tree, pushed, graph OK, content-bound receipts, no outstanding
    # jobs). Permit exactly this one stop; the *:0/10 watchdog relaunches a
    # fresh session on its next tick. The flag is one-shot (cleared on the next
    # launch's `start`) and honored only while fresh. The RUN stays active.
    ro = state_early.get("rollover") or {}
    if (isinstance(ro, dict) and ro.get("pending")
            and time.time() - (ro.get("epoch") or 0) < ROLLOVER_PENDING_FRESH_S):
        sys.stderr.write(
            "[sequencer] stop allowed: verified rollover checkpoint -- the "
            "watchdog relaunches a fresh worker context on its next tick; the "
            "run stays ARMED and the cursor carries the state.\n")
        return 0
    # Oracle-consulting fallback (runner-kit law 2, adopted 2026-07-03): a
    # session at a TRUE lifecycle end (oracle DONE, or BLOCKED once the 3-state
    # split reports only recoverable deferrals) may end WITHOUT the sentinel --
    # the sentinel is written by the fixpoint CLI, and a session that cannot
    # reach it (lock, confusion, truncation) would otherwise idle until
    # external reap. Ending here does NOT disarm: the marker persists, the
    # watchdog relaunches, and the next session (or the operator) finalizes
    # via `run_phase_guard.py fixpoint`. Fail-closed: any oracle error keeps
    # the hard block below.
    verdict = _live_oracle_status()
    if verdict in ("DONE", "BLOCKED"):
        sys.stderr.write(
            f"[sequencer] stop allowed: live triage oracle reports {verdict} "
            "(no implementable work right now). The run stays ARMED -- "
            "finalize with `python3 .claude/hooks/run_phase_guard.py fixpoint` "
            "(DONE) or let the watchdog retry (BLOCKED).\n")
        return 0
    state = load_state()
    sys.stderr.write(
        f"[SEQ-STOP] do not stop (pass={state.get('pass_no')} "
        f"file={state.get('file')} phase={state.get('phase')}). Defer-and-"
        "advance, poll any in-flight review IN-SESSION (blocking sleep loop), "
        "or END on a verified `rollover`. Re-invoke Skill(overnight-sequencer) "
        "and continue. Details: docs/infrastructure/hook-codes.md#seq-stop")
    return 2


# ---- CLI (the sequencer skill drives transitions) ----

def _emit_anchor(state) -> None:
    """Best-effort one-line situational anchor on stderr; never raises.

    Only fires for an active run so interactive `status` calls stay quiet.
    """
    try:
        if not state.get("active"):
            return
        import runner_status
        sys.stderr.write("[sequencer] " + runner_status.anchor_line(repo_root()) + "\n")
    except Exception:
        pass


def cli(argv):
    cmd = argv[0] if argv else "status"
    state = load_state()
    if cmd == "start":
        state = {
            "active": True, "phase": "PREFLIGHT", "pass_no": 1,
            "domain": None, "file": None, "section_idx": 0,
            # How section_idx was last set: explicit | derived | stale. `unset`
            # is the start value -- section_idx is 0 and means nothing yet, so
            # a metrics row carrying it must be excluded from any calibration
            # rather than read as "section 0".
            "section_source": "unset",
            "progress_this_pass": False,
            "started_at": argv[1] if len(argv) > 1 else "unknown",
            "updated_at": argv[1] if len(argv) > 1 else "unknown",
            # R1 baseline: ships from BEFORE this run must not block the first
            # cursor move; only a ship inside the run demands a rotation.
            "last_rollover_epoch": int(time.time()),
        }
        save_state(state)
        # NOTE: the armed marker is intentionally NOT removed here. It is the
        # persistent master switch -- it must outlive every watchdog relaunch so
        # a fresh agent is always redirected back onto overnight-sequencer and
        # can never voluntarily stop. Only `--disarm` or an oracle-verified
        # FIXPOINT removes it. (Removing it on start was the death-thrash bug:
        # once gone, a reset cursor left the guard fully inert.)
        # Clear any stale FIXPOINT sentinel from a prior run: a starting run is by
        # definition not complete, and a leftover sentinel would let Stop succeed.
        try:
            FIXPOINT_SENTINEL.unlink()
        except FileNotFoundError:
            pass
        print("[sequencer] started: pass 1, PREFLIGHT (armed marker kept)",
              file=sys.stderr)
        return 0
    if cmd == "status":
        print(json.dumps(state, indent=1))
        _emit_anchor(state)
        return 0
    if cmd == "phase":
        if len(argv) < 2 or argv[1] not in PHASES:
            print(f"[sequencer] phase needs one of {PHASES}", file=sys.stderr)
            return 1
        state["phase"] = argv[1]
        save_state(state)
        print(f"[sequencer] phase -> {argv[1]}", file=sys.stderr)
        _emit_anchor(state)
        return 0
    if cmd == "cursor":
        # cursor <domain> <file> [section_idx]
        if len(argv) >= 3:
            # P3.2: a REFUSED rollover may repair ONLY the current checkpoint; it
            # must NOT advance into the next section un-rotated (the un-rotated
            # section-to-section advance the flow invariant forbids). Block a
            # cursor move to a DIFFERENT section while a rollover is refused --
            # until a `rollover` VERIFIES (which clears the flag) or the operator
            # `clear`s. Safe because B4/F2 stop benign coverage.* from refusing
            # the rollover forever.
            new_file = argv[2]
            new_idx = int(argv[3]) if len(argv) >= 4 else state.get("section_idx")
            rr = state.get("rollover_refused")
            if isinstance(rr, dict) and state.get("active"):
                if new_file != rr.get("file") or new_idx != rr.get("section_idx"):
                    print("[sequencer] cursor BLOCKED (P3.2): a rollover was "
                          f"REFUSED at {rr.get('file')} section "
                          f"{rr.get('section_idx')}. Repair that checkpoint "
                          "(commit/clean/push + receipts) and re-run `rollover` -- "
                          "do NOT start the next section un-rotated. Operator "
                          "override: `run_phase_guard.py clear`.", file=sys.stderr)
                    return 1
            # R1 (2026-07-19): rollover-required-after-ship. P3.2 above only
            # traps a REFUSED rollover; a worker that never ATTEMPTS one could
            # advance into the next section un-rotated. If any commit newer
            # than the last verified rotation adds a ship stamp, the only
            # legal next step is `rollover`. Deferrals add no stamp and
            # advance freely; the fresh post-rotation worker passes because
            # the ship predates its last_rollover_epoch (set on `start` and
            # on every verified `rollover`).
            if state.get("active") and (new_file != state.get("file")
                                        or new_idx != state.get("section_idx")):
                sha = _ship_stamp_since(repo_root(),
                                        state.get("last_rollover_epoch"))
                if sha:
                    print("[sequencer] cursor BLOCKED (R1): ship-stamp commit "
                          f"{sha[:12]} landed after the last verified rotation. "
                          "A fully-shipped section ENDS the worker context: run "
                          "`python3 .claude/hooks/run_phase_guard.py rollover` "
                          "and END the turn -- the watchdog relaunches a fresh "
                          "worker that resumes at the next section. Do NOT "
                          "advance un-rotated. Operator override: "
                          "`run_phase_guard.py clear`.", file=sys.stderr)
                    return 1
            state["domain"], state["file"] = argv[1], argv[2]
            if len(argv) >= 4:
                state["section_idx"] = int(argv[3])
                state["section_source"] = "explicit"
            else:
                # COST INSTRUMENTATION (token-saver v02, 2026-07-28). The
                # documented call shape is `cursor <domain> <file>` with NO
                # index, so `section_idx` never left its `start` value of 0 and
                # stream-report.py stamped `"section": 0` on EVERY metrics
                # record ever written -- all 56 of them, across every run. That
                # left no way to correlate a section with its turn count, which
                # is the dataset the split-predictor calibration needs (cost is
                # quadratic in segment length, so knowing WHICH sections run
                # long is the whole input). Derive it from the same oracle the
                # sequencer routes on rather than adding an argument the model
                # must remember: the current section is the first one still
                # NEEDS_WORK. Fail-open in every direction -- on any error the
                # previous value stands, exactly as before, because a metrics
                # attribution must never be able to block a run.
                derived = _derive_section_idx(argv[2])
                if derived is not None:
                    state["section_idx"] = derived
                    state["section_source"] = "derived"
                else:
                    # The oracle was unavailable (its cache lives under build/
                    # and build-and-validate.sh deletes it unless --keep-cache;
                    # observed absent at 16:47 and rebuilt at 16:48 on the
                    # 2026-07-28 canary). The cursor keeps its PREVIOUS value,
                    # which is the right safety choice -- a metrics field must
                    # never be able to fail a cursor move and wedge a run --
                    # but the number is now carried over, not measured. Mark it
                    # so the split-predictor calibration can EXCLUDE the row
                    # instead of averaging a stale section in as if it were
                    # fresh. A dataset whose gaps are invisible is worse than
                    # one with holes it can see.
                    state["section_source"] = "stale"
            save_state(state)
            print(f"[sequencer] cursor -> {argv[2]}", file=sys.stderr)
            return 0
        print("[sequencer] cursor needs <domain> <file> [idx]", file=sys.stderr)
        return 1
    if cmd == "progress":
        state["progress_this_pass"] = True
        save_state(state)
        return 0
    if cmd == "next-pass":
        state["pass_no"] = int(state.get("pass_no", 1)) + 1
        state["progress_this_pass"] = False
        state["phase"] = "TRIAGE"
        save_state(state)
        print(f"[sequencer] -> pass {state['pass_no']}", file=sys.stderr)
        return 0
    if cmd == "fixpoint":
        # FIXPOINT is the ONLY path to a permanent stop (it ends the run and the
        # watchdog auto-disarms). It must NOT be fakeable: machine-verify via the
        # triage oracle (graph-truth, freshly rebuilt) that ZERO work remains.
        # If anything remains, REFUSE -- the run stays active and keeps looping.
        # (subprocess is imported at module scope; a redundant `import subprocess`
        # here previously made the name function-local across ALL of cli(), so any
        # bare subprocess.run reached before this line -- e.g. the rollover inline
        # checkpoint write and the review-resolved verb -- raised UnboundLocalError.)
        root = repo_root()
        try:
            subprocess.run(
                ["bash", "scripts/todo-graph/build-and-validate.sh", "--keep-cache"],
                cwd=root, capture_output=True, text=True, timeout=300)
            out = subprocess.run(
                [sys.executable, str(HOOK_DIR / "sequencer_triage.py"), "--next"],
                cwd=root, capture_output=True, text=True, timeout=120)
            nxt = json.loads(out.stdout or "{}")
        except Exception as e:  # noqa: BLE001
            print(f"[sequencer] fixpoint REFUSED: oracle check failed ({e}). "
                  "Fail safe -- keep running, do NOT finish.", file=sys.stderr)
            return 1
        if nxt.get("status") != "DONE":
            print(f"[sequencer] fixpoint REFUSED: the oracle still reports work "
                  f"(status={nxt.get('status')}, file={nxt.get('file')}). The run "
                  "is NOT done -- continue the loop. FIXPOINT is only valid when "
                  "`sequencer_triage.py --next` returns DONE.", file=sys.stderr)
            return 1
        # P6.3 -- STRANDED-DEFERRAL GATE (promoted from advisory 2026-07-27).
        #
        # The section oracle above classifies on the Implementation Order row +
        # SECTION stamps only, so a `[/]` checklist item INSIDE a shipped
        # section is invisible to it: the section carries Verified +
        # Quality-reviewed, classifies DONE, and fixpoint never revisits it.
        # That is the one way this runner could declare the OS complete with
        # real, now-runnable work parked -- exactly the completeness hole the
        # fixpoint loop exists to close.
        #
        # This ran advisory-only while the audit's precision was unmeasured (an
        # early naive signal over-matched ~6x). A 10-item hand sample on the
        # live tree (2026-07-27) found 0 false positives, 6 items verified
        # genuinely unblocked at file:line, so it now GATES.
        #
        # It cannot wedge an unattended run: `park` is always a legal
        # disposition (with a reason), so there is a way forward for every
        # item. The gate forces a DECISION, never a particular decision.
        #
        # Fail-OPEN on infrastructure error (missing script, timeout, crash):
        # a broken audit must not strand a genuinely-complete run forever. Only
        # a clean exit 1 -- the audit ran and found undispositioned items --
        # refuses.
        try:
            aud = subprocess.run(
                [sys.executable,
                 str(repo_root() / "scripts/overnight/stranded_deferrals.py"),
                 "--gate"],
                cwd=str(repo_root()), capture_output=True, text=True, timeout=120)
            gate_rc = aud.returncode
        except Exception as e:  # noqa: BLE001
            sys.stderr.write(f"[sequencer] stranded-deferral gate could not run "
                             f"({e}) -- failing OPEN, completion not blocked\n")
            gate_rc = 0
        if gate_rc == 1:
            sys.stderr.write(aud.stderr or "")
            print("[sequencer] fixpoint REFUSED: unblocked-but-parked work "
                  "remains (stranded-deferral gate). Disposition each item "
                  "listed above (reopen / done / park with a reason), then "
                  "re-run fixpoint. The run is NOT done.", file=sys.stderr)
            return 1

        state["phase"] = "FIXPOINT"
        state["active"] = False
        save_state(state)
        # Genuine, oracle-verified completion is the one self-terminating exit:
        # write the exclusive sentinel (this is the ONLY place it is created) so
        # handle_stop will permit the stop, and remove the master switch so the
        # next watchdog tick does not relaunch into a re-entry loop.
        FIXPOINT_SENTINEL.parent.mkdir(parents=True, exist_ok=True)
        FIXPOINT_SENTINEL.write_text("oracle-verified: no remaining work\n",
                                     encoding="utf-8")
        try:
            ARMED_MARKER.unlink()
        except FileNotFoundError:
            pass
        print("[sequencer] FIXPOINT verified by oracle (no remaining work, no "
              "undispositioned stranded deferrals) -- run complete; sentinel "
              "written, armed marker removed", file=sys.stderr)
        return 0
    if cmd == "clear":
        save_state({"active": False})
        print(f"[sequencer] cleared: {' '.join(argv[1:]) or 'no reason'}", file=sys.stderr)
        return 0
    if cmd == "mark-rotation":
        # R1 (2026-07-19): called by overnight-launch.sh at every spawn. A
        # (re)launch IS a fresh worker context, so it counts as a rotation
        # boundary -- without this, a crash / usage-limit relaunch (which
        # never ran the `rollover` verb) would leave last_rollover_epoch
        # pointing before the dead session's ship and force the fresh worker
        # through a redundant rollover (plus a 10-min watchdog wait) before
        # its first section. Also consumes a leftover rollover-pending flag
        # (its one permitted stop already happened -- the session is gone).
        if not state.get("active"):
            return 0  # not-yet-started run: `start` sets its own epoch
        state["last_rollover_epoch"] = int(time.time())
        state.pop("rollover", None)
        save_state(state)
        print("[sequencer] rotation boundary stamped (fresh worker context)",
              file=sys.stderr)
        return 0
    if cmd == "relifecycle":
        # One-shot Stage 1-2 override for a mature file that grew a genuinely
        # NEW section. Consumed by the next Stage 1-2 skill invocation.
        reason = " ".join(argv[1:]).strip()
        if len(reason) < 12:
            print("[sequencer] relifecycle needs a reason (>= 12 chars) naming "
                  "the new section", file=sys.stderr)
            return 1
        state["lifecycle_override"] = reason
        save_state(state)
        print(f"[sequencer] lifecycle override recorded: {reason}", file=sys.stderr)
        return 0
    if cmd == "rollover":
        # Verified worker-context rotation. Machine gates decide; on success
        # the Stop hook permits exactly one stop and the *:0/10 watchdog
        # relaunches a fresh session on its next tick.
        if not state.get("active"):
            print("[sequencer] rollover REFUSED: no active run", file=sys.stderr)
            return 1
        fails = _rollover_failures(repo_root(), state)
        if fails:
            # P3.2: record the refusal + the section it happened at, so `cursor`
            # blocks an un-rotated advance into the next section until a rollover
            # verifies. Repair the current checkpoint and retry -- do not proceed.
            state["rollover_refused"] = {
                "file": state.get("file"),
                "section_idx": state.get("section_idx"),
                "epoch": int(time.time()),
            }
            save_state(state)
            print("[sequencer] rollover REFUSED (checkpoint not verified):\n"
                  + "\n".join(f"  - {f}" for f in fails)
                  + "\n(receipt failures: run build -> test -> smoke with SMOKE "
                    "LAST, then `python3 scripts/overnight/receipts.py "
                    "record-rollover .` in ONE shot -- J1 -- so the image binding "
                    "stays valid instead of a refuse-fix-refuse cascade.)"
                  + "\nRepair these on the CURRENT section and RE-RUN `rollover` "
                    "-- do NOT start the next section un-rotated (P3.2).",
                  file=sys.stderr)
            return 1
        # Atomic pending flag; the Stop hook permits ONE exit and the *:0/10
        # watchdog relaunches a fresh session. No custom watcher.
        now = int(time.time())
        state["rollover"] = {"pending": True, "epoch": now}
        state.pop("rollover_refused", None)  # P3.2: a verified rollover clears it
        # R1: the rotation boundary -- ship stamps at or before this instant no
        # longer block a section start (the fresh worker starts clean). Same
        # `now` as the pending flag so the two windows can never desync.
        state["last_rollover_epoch"] = now
        save_state(state)
        # P4.1: a verified rollover resets the context-rotation turn counter so
        # the fresh worker counts from zero (best-effort).
        try:
            (repo_root() / ".claude/state/rotate-hint.json").unlink()
        except Exception:
            pass
        # Durable checkpoint so the relaunched session loads settled facts
        # (section-pack digest, receipts, review status) instead of
        # rediscovering them. Best-effort -- never blocks the rollover.
        try:
            subprocess.run(
                ["python3", str(repo_root()
                                / "scripts/overnight/section-checkpoint.py"),
                 "write"], cwd=str(repo_root()), capture_output=True, timeout=30)
        except Exception:
            pass
        print("[sequencer] rollover VERIFIED: clean tree, pushed, graph OK, "
              "receipts content-valid, no outstanding jobs. Final-answer now "
              "with a one-line checkpoint summary and END the turn -- the "
              "watchdog relaunches a fresh worker context on its next tick; "
              "the run stays armed and the cursor carries the state.",
              file=sys.stderr)
        return 0
    if cmd == "review-resolved":
        # A5 (review 2026-07-14 round 2): record a content-bound review-RESOLUTION
        # receipt {head, review_run_id, ts} attesting the current section's review
        # cycle is COMPLETE and green at HEAD -- the boundary a mid-section
        # rollover-wip must stand on (received:true alone does not prove findings
        # were fixed + verified). Requires: active + SECTIONS; the last review
        # received AND bound to HEAD (F3: fixes committed + re-reviewed); and
        # content-valid build + test receipts. Fail-CLOSED; writes nothing unless
        # all hold.
        if not state.get("active") or state.get("phase") != "SECTIONS":
            print("[sequencer] review-resolved REFUSED: only valid mid-section "
                  "(active run, phase == SECTIONS).", file=sys.stderr)
            return 1
        root = repo_root()
        rs_path = root / ".claude/state/last-codex-review.json"
        rs = {}
        if rs_path.exists():
            try:
                rs = json.loads(rs_path.read_text())
            except (OSError, ValueError):
                print("[sequencer] review-resolved REFUSED: last-codex-review.json "
                      "unreadable (fail-closed).", file=sys.stderr)
                return 1
        if rs and rs.get("received") is not True:
            print("[sequencer] review-resolved REFUSED: the last Codex review is "
                  "not yet received -- receive it (Skill "
                  "superpowers:receiving-code-review) and fix its findings first.",
                  file=sys.stderr)
            return 1
        bind_fail = _review_not_binding_head(root, rs) if rs else ""
        if bind_fail:
            print(f"[sequencer] review-resolved REFUSED: {bind_fail}. The received "
                  "review must bind the CURRENT HEAD (fixes committed + "
                  "re-reviewed).", file=sys.stderr)
            return 1
        ok, why = _build_suite_receipts_ok(root)
        if not ok:
            print(f"[sequencer] review-resolved REFUSED: {why}. Run a green build + "
                  "test.sh, record receipts, then retry.", file=sys.stderr)
            return 1
        try:
            head = subprocess.run(["git", "-C", str(root), "rev-parse", "HEAD"],
                                  capture_output=True, text=True,
                                  timeout=10).stdout.strip()
        except Exception:
            head = ""
        if not head:
            print("[sequencer] review-resolved REFUSED: cannot read HEAD.",
                  file=sys.stderr)
            return 1
        rec = {"head": head, "review_run_id": rs.get("review_run_id", ""),
               "ts": int(time.time())}
        try:
            p = root / _REVIEW_RESOLUTION_REL
            p.parent.mkdir(parents=True, exist_ok=True)
            tmp = p.with_suffix(p.suffix + ".tmp")
            tmp.write_text(json.dumps(rec))
            os.replace(tmp, p)
        except Exception as exc:  # noqa: BLE001
            print("[sequencer] review-resolved REFUSED: could not write receipt "
                  f"({exc}).", file=sys.stderr)
            return 1
        print(f"[sequencer] review-resolved VERIFIED: review cycle resolved + green "
              f"at {head[:12]}. A mid-section `rollover-wip` may now proceed at this "
              "HEAD.", file=sys.stderr)
        return 0
    if cmd == "rollover-wip":
        # P4.6: MID-SECTION context-cap rotation (safe-boundary firing). Fires
        # ONLY when BOTH hold: (a) the P4.1 rotate_hint is set (context actually
        # grew past the band -- do NOT rotate mid-section on a whim), and (b) the
        # P4.5 WIP gate passes (committed-clean tree, no pending review, no bg
        # job). Rotation is HARD-FORBIDDEN mid-fix-loop -- an uncommitted edit or
        # an outstanding review both FAIL the WIP gate -- and during a review wait
        # (an active codex-rev-* unit fails the WIP gate). On success it uses the
        # SAME rotation mechanism as `rollover`; the enriched section-checkpoint
        # (P4.2/P4.3) carries next_action + open_findings so the fresh worker
        # resumes the SAME section without re-deriving.
        if not state.get("active"):
            print("[sequencer] rollover-wip REFUSED: no active run", file=sys.stderr)
            return 1
        # F1 (review 2026-07-14): a WIP rotation is ONLY a mid-section move. It
        # must NOT be an alternate ship path. Require phase == SECTIONS, and
        # REFUSE outright if a SHIP rollover was refused (rollover_refused set) --
        # a failed ship gate (unpushed / missing receipts / bad graph) must be
        # fixed, never laundered into a WIP rotation. And it must NEVER clear
        # rollover_refused (the P3.2 advance-block stays until a real ship rollover
        # verifies).
        if state.get("phase") != "SECTIONS":
            print("[sequencer] rollover-wip REFUSED: only valid mid-section "
                  f"(phase == SECTIONS), current phase is {state.get('phase')!r}.",
                  file=sys.stderr)
            return 1
        if state.get("rollover_refused"):
            print("[sequencer] rollover-wip REFUSED: a ship rollover was refused "
                  "(rollover_refused set). Fix the ship-gate failures and re-run "
                  "`rollover` -- a WIP rotation must not bypass a failed ship gate.",
                  file=sys.stderr)
            return 1
        try:
            hint = json.loads(
                (repo_root() / ".claude/state/rotate-hint.json").read_text())
        except Exception:
            hint = {}
        if not (isinstance(hint, dict) and hint.get("hint")):
            print("[sequencer] rollover-wip REFUSED: no rotate_hint set -- context "
                  "has not crossed the size band, so a mid-section rotation is not "
                  "warranted. Continue in-session.", file=sys.stderr)
            return 1
        # A1 (review 2026-07-14): a WIP rotation must NOT substitute for the full
        # ship rollover. After a section ships (commit + PUSH) + `progress`, the
        # phase is still SECTIONS and the tree is clean, so the WIP gate alone
        # would pass -- letting a shipped section rotate via the weaker gate,
        # skipping the full rollover's receipt + todo-graph verification. A genuine
        # mid-section boundary always has committed-but-unpushed local work (the
        # WIP rotation deliberately does not push); a post-ship boundary has none.
        # Refuse when nothing is unpushed (or it cannot be determined): use the
        # full `rollover` there. This is the successful-ship analogue of the P4.5
        # F1 alternate-ship-path guard.
        ahead = _unpushed_count(repo_root())
        if not ahead:  # 0 or None
            reason = ("everything is pushed -- no unshipped WIP to rotate"
                      if ahead == 0 else
                      "cannot determine unpushed state (no upstream / git error)")
            print("[sequencer] rollover-wip REFUSED: " + reason + ". A mid-section "
                  "rotation is only for committed-but-unpushed in-progress work; if "
                  "the section has SHIPPED, run the full `rollover` (it verifies "
                  "receipts + todo-graph). Otherwise continue in-session.",
                  file=sys.stderr)
            return 1
        # A4 (round 2): reliably refuse a SHIP/stamp commit even in the pre-push
        # window the unpushed guard cannot see -- key on the definitive **Verified:**
        # stamp, not the loose section_idx.
        if _head_adds_ship_stamp(repo_root()):
            print("[sequencer] rollover-wip REFUSED: HEAD is a section-SHIP/stamp "
                  "commit (adds a **Verified:** stamp to a todo/ file) -- a shipped "
                  "section uses the full `rollover` (receipts + todo-graph), never a "
                  "WIP rotation. Push, then run `rollover`.", file=sys.stderr)
            return 1
        fails = _rollover_failures_wip(repo_root(), state)
        if fails:
            print("[sequencer] rollover-wip REFUSED (not at a safe WIP boundary):\n"
                  + "\n".join(f"  - {f}" for f in fails)
                  + "\nA mid-section rotation is allowed ONLY at a WIP-commit-clean "
                    "tree with NO pending review and NO background job (never "
                    "mid-fix-loop, never during a review wait). Commit the WIP / "
                    "receive the review / wait for the job, then retry -- or just "
                    "continue in-session.", file=sys.stderr)
            return 1
        # A5 (round 2): the review cycle must be RESOLVED + green at THIS HEAD, not
        # merely received. `review-resolved` writes a content-bound receipt; require
        # it to match HEAD. Fail-CLOSED (absent/stale -> refuse, run review-resolved).
        rr_ok, rr_why = _review_resolution_valid(repo_root())
        if not rr_ok:
            print(f"[sequencer] rollover-wip REFUSED: {rr_why}.", file=sys.stderr)
            return 1
        # F2 (review 2026-07-14): write the DURABLE checkpoint FIRST and
        # fail-CLOSED if it fails -- never authorize a Stop/rotation without a
        # checkpoint the fresh worker can resume from. Only after a successful
        # write do we set the rollover-pending authorization.
        if not _write_section_checkpoint(repo_root()):
            print("[sequencer] rollover-wip REFUSED: could not write a durable "
                  "section-checkpoint -- refusing to authorize a rotation the "
                  "fresh worker cannot resume from.", file=sys.stderr)
            return 1
        state["rollover"] = {"pending": True, "epoch": int(time.time()),
                             "wip": True}
        # NB: rollover_refused is intentionally NOT cleared here (F1) -- a WIP
        # rotation resumes the SAME section and must not lift the advance-block.
        save_state(state)
        # Reset the turn counter for the fresh worker (the checkpoint is already
        # written above). Best-effort.
        try:
            (repo_root() / ".claude/state/rotate-hint.json").unlink()
        except Exception:
            pass
        print("[sequencer] rollover-wip VERIFIED: WIP committed, no pending review, "
              "no background job. This is a MID-SECTION context rotation -- "
              "final-answer with a one-line checkpoint and END the turn; the "
              "watchdog relaunches a fresh worker that resumes the SAME section "
              "from the cursor (section-checkpoint carries next_action + "
              "open_findings). The run stays armed.", file=sys.stderr)
        return 0
    if cmd == "selftest":
        return selftest()
    print(f"[sequencer] unknown command: {cmd}", file=sys.stderr)
    return 1


def selftest():
    fails = []

    def check(cond, msg):
        if not cond:
            fails.append(msg)

    H = True  # headless: the guard only governs the unattended run
    # Interactive sessions (headless=False) are NEVER constrained -- this is the
    # anti-trap invariant that lets the operator share the repo hooks.
    a, _ = evaluate("AskUserQuestion", {}, {"active": True, "phase": "SECTIONS"}, headless=False)
    check(a, "interactive AskUserQuestion blocked (must never trap the operator)")
    a, _ = evaluate("Skill", {"skill": "implement-todo-section"}, {"active": True, "phase": "VALIDATE"}, headless=False)
    check(a, "interactive Skill phase-blocked (must never trap the operator)")
    a, _ = evaluate("Bash", {"command": "bash .claude/skills/overnight-sequencer/arm-sequencer.sh --disarm"},
                    {"active": True, "phase": "SECTIONS"}, headless=False)
    check(a, "interactive --disarm blocked (the human MUST be able to disarm)")

    # AskUserQuestion always blocked in the headless run.
    for ph in PHASES:
        a, _ = evaluate("AskUserQuestion", {}, {"active": True, "phase": ph}, headless=H)
        check(not a, f"AskUserQuestion allowed in {ph}")
    # Inactive + unarmed headless allows everything.
    a, _ = evaluate("AskUserQuestion", {}, {"active": False}, headless=H)
    check(a, "AskUserQuestion blocked while headless run inactive+unarmed")
    # Headless self-teardown is blocked across all known shapes; the legit
    # oracle-gated `fixpoint` and ordinary phase transitions are NOT.
    for cmd in ("bash .claude/skills/overnight-sequencer/arm-sequencer.sh --disarm",
                "bash scripts/overnight-arm.sh todo/x.md --disarm",
                "python3 .claude/hooks/run_phase_guard.py clear oops",
                "python3 .claude/hooks/run_phase_guard.py phase FIXPOINT",
                "rm -f .claude/state/sequencer-armed",
                "rm .claude/state/sequencer-run.json",
                "echo '{}' > .claude/state/sequencer-run.json",
                "python3 -c \"import os; os.remove('.claude/state/sequencer-fixpoint')\"",
                "touch .claude/state/sequencer-fixpoint",
                "cp /tmp/fake .claude/state/sequencer-fixpoint",
                "dd if=/dev/zero of=.claude/state/sequencer-fixpoint bs=1 count=1",
                "systemctl --user stop overnight-impossible-os.service",
                "systemctl --user disable overnight-impossible-os-watchdog.timer",
                "pkill -f 'claude -p'"):
        a, _ = evaluate("Bash", {"command": cmd}, {"active": True, "phase": "SECTIONS"}, headless=H)
        check(not a, f"headless self-teardown allowed: {cmd}")
    for okcmd in ("python3 .claude/hooks/run_phase_guard.py fixpoint",
                  "python3 .claude/hooks/run_phase_guard.py phase SECTIONS",
                  "python3 .claude/hooks/run_phase_guard.py next-pass",
                  "git commit -m 'x' && cat .claude/state/sequencer-run.json"):
        a, _ = evaluate("Bash", {"command": okcmd}, {"active": True, "phase": "SECTIONS"}, headless=H)
        check(a, f"legit guard/bash command blocked as self-teardown: {okcmd}")
    # implement-todo-section blocked in VALIDATE, allowed in SECTIONS.
    a, _ = evaluate("Skill", {"skill": "implement-todo-section"}, {"active": True, "phase": "VALIDATE"}, headless=H)
    check(not a, "implement-todo-section allowed in VALIDATE")
    a, _ = evaluate("Skill", {"skill": "implement-todo-section"}, {"active": True, "phase": "SECTIONS"}, headless=H)
    check(a, "implement-todo-section blocked in SECTIONS")
    # validate-todo-file allowed in VALIDATE, blocked in SECTIONS.
    a, _ = evaluate("Skill", {"name": "validate-todo-file"}, {"active": True, "phase": "VALIDATE"}, headless=H)
    check(a, "validate-todo-file blocked in VALIDATE")
    a, _ = evaluate("Skill", {"name": "validate-todo-file"}, {"active": True, "phase": "SECTIONS"}, headless=H)
    check(not a, "validate-todo-file allowed in SECTIONS")
    # gap-audit-todo only in GAP_AUDIT.
    a, _ = evaluate("Skill", {"skill": "gap-audit-todo"}, {"active": True, "phase": "GAP_AUDIT"}, headless=H)
    check(a, "gap-audit-todo blocked in GAP_AUDIT")
    a, _ = evaluate("Skill", {"skill": "gap-audit-todo"}, {"active": True, "phase": "SECTIONS"}, headless=H)
    check(not a, "gap-audit-todo allowed in SECTIONS (should be GAP_AUDIT only)")
    # Non-sequence skill + Bash/Edit pass in SECTIONS.
    a, _ = evaluate("Skill", {"skill": "kernel-code-quality"}, {"active": True, "phase": "SECTIONS"}, headless=H)
    check(a, "inner-pipeline skill blocked in SECTIONS")
    a, _ = evaluate("Bash", {"command": "git commit"}, {"active": True, "phase": "SECTIONS"}, headless=H)
    check(a, "Bash blocked in SECTIONS")
    # legacy System A skill blocked everywhere active.
    a, _ = evaluate("Skill", {"skill": "overnight-todo-runner"}, {"active": True, "phase": "SECTIONS"}, headless=H)
    check(not a, "legacy overnight-todo-runner allowed inside a sequencer run")
    # Armed-but-not-started: only overnight-sequencer skill allowed.
    a, _ = evaluate("Skill", {"skill": "overnight-sequencer"}, {"active": False}, armed=True, headless=H)
    check(a, "overnight-sequencer blocked while armed")
    a, _ = evaluate("Skill", {"name": "overnight-runner:start"}, {"active": False}, armed=True, headless=H)
    check(not a, "generic overnight-runner:start allowed while armed (should redirect)")
    a, _ = evaluate("Bash", {"command": "python3 .claude/hooks/sequencer_triage.py --next"},
                    {"active": False}, armed=True, headless=H)
    check(a, "Bash blocked while armed (setup needs it)")
    a, _ = evaluate("AskUserQuestion", {}, {"active": False}, armed=True, headless=H)
    check(not a, "AskUserQuestion allowed while armed")
    # Armed but cursor reset to inactive (the death-thrash condition): the run is
    # still governed -- the redirect still fires off the persistent marker.
    a, _ = evaluate("Skill", {"name": "overnight-runner:start"}, {"active": False}, armed=True, headless=H)
    check(not a, "armed+inactive run went ungoverned (death-thrash regression)")
    # Not armed, not active (headless): everything passes.
    a, _ = evaluate("Skill", {"skill": "anything"}, {"active": False}, armed=False, headless=H)
    check(a, "skill blocked while neither armed nor active")

    # Codex WRITE is interactive-only: blocked in the headless active run,
    # never blocked interactively; read-only reviews always pass.
    for wcmd in ("node /x/codex-companion.mjs task --write 'fix it'",
                 "codex exec --sandbox workspace-write 'do it'",
                 "codex --full-auto 'go'",
                 "bash scripts/codex-dispatch.sh 'x' && codex task --write 'y'"):
        a, _ = evaluate("Bash", {"command": wcmd}, {"active": True, "phase": "SECTIONS"}, headless=H)
        check(not a, f"headless Codex write allowed: {wcmd[:40]}")
        a, _ = evaluate("Bash", {"command": wcmd}, {"active": True, "phase": "SECTIONS"}, headless=False)
        check(a, f"interactive Codex write blocked (must not): {wcmd[:40]}")
    for rcmd in ("node /x/codex-companion.mjs adversarial-review 'review'",
                 "bash scripts/overnight/review-broker-codex-dispatch.sh '[review-kind: design] todo/x.md b'",
                 "node /x/codex-companion.mjs task --background 'review-kind design'"):
        a, _ = evaluate("Bash", {"command": rcmd}, {"active": True, "phase": "SECTIONS"}, headless=H)
        check(a, f"headless read-only review blocked: {rcmd[:40]}")

    # Lifecycle routing (Stage 0): Stage 1-2 skills blocked on a mature file.
    a, _ = evaluate("Skill", {"skill": "validate-todo-file"},
                    {"active": True, "phase": "VALIDATE"}, headless=H,
                    stages_done=True)
    check(not a, "validate-todo-file allowed on stages_1_2_done file")
    a, _ = evaluate("Skill", {"skill": "gap-audit-todo"},
                    {"active": True, "phase": "GAP_AUDIT"}, headless=H,
                    stages_done=True)
    check(not a, "gap-audit-todo allowed on stages_1_2_done file")
    a, _ = evaluate("Skill", {"skill": "validate-todo-file"},
                    {"active": True, "phase": "VALIDATE",
                     "lifecycle_override": "new section 21 appeared"},
                    headless=H, stages_done=True)
    check(a, "relifecycle override did not unblock validate-todo-file")
    a, _ = evaluate("Skill", {"skill": "validate-todo-file"},
                    {"active": True, "phase": "VALIDATE"}, headless=H,
                    stages_done=False)
    check(a, "validate-todo-file blocked on an immature file")
    a, _ = evaluate("Skill", {"skill": "validate-todo-file"},
                    {"active": True, "phase": "VALIDATE"}, headless=False,
                    stages_done=True)
    check(a, "interactive validate-todo-file blocked (must never trap operator)")

    if fails:
        for f in fails:
            print("FAIL:", f, file=sys.stderr)
        print(f"run_phase_guard selftest: {len(fails)} failure(s)", file=sys.stderr)
        return 1
    print("run_phase_guard selftest OK")
    return 0


def main(argv):
    mode = argv[1] if len(argv) > 1 else "pretool"
    if mode == "pretool":
        return handle_pretool()
    if mode == "stop":
        return handle_stop()
    return cli(argv[1:])


if __name__ == "__main__":
    sys.exit(main(sys.argv))
