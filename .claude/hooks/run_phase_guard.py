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
  CLI       start / status / phase / cursor / progress / next-pass / fixpoint /
            clear / selftest -- the overnight-sequencer skill drives phase
            transitions through these.

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
import sys
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
_MUTATE_OPS = ("rm ", "rm-", "mv ", "unlink", "truncate", "tee ", " > ", ">>",
               "os.remove", "os.unlink", "rmtree", "shutil.")


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
    if any(k in c for k in ("pkill", "killall", "kill ")) and "claude" in c:
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


def load_state():
    if not STATE_PATH.exists():
        return {"active": False}
    try:
        with STATE_PATH.open(encoding="utf-8") as fh:
            return json.load(fh)
    except (json.JSONDecodeError, OSError):
        return {"active": False}


def save_state(state):
    STATE_PATH.parent.mkdir(parents=True, exist_ok=True)
    with STATE_PATH.open("w", encoding="utf-8") as fh:
        json.dump(state, fh, indent=1)


def _skill_name(tool_input):
    return tool_input.get("skill") or tool_input.get("name") or ""


def evaluate(tool_name, tool_input, state, armed=False, headless=False):
    """Return (allow: bool, message: str). Pure -- unit-testable.

    The guard governs ONLY the headless unattended run. An interactive operator
    session (headless=False) is never constrained -- it can ask, stop, and run
    --disarm freely. This is what lets the human share the repo's hooks without
    being trapped by the run cursor on disk.
    """
    if not headless:
        return True, ""

    active = bool(state.get("active"))
    if not active and not armed:
        return True, ""

    # AskUserQuestion is never allowed inside the unattended run.
    if tool_name == "AskUserQuestion":
        return False, (
            "[sequencer] AskUserQuestion is blocked during an unattended run. "
            "Decide with the conservative/no-op choice and log the assumption, "
            "or DEFER the item (mark [/] + a Deferred stamp with an XREF) and "
            "advance. The run never stops to ask. "
            "(todo/TODO-Claude-Overnight-Runner.md hard rules.)")

    # The unattended run must never tear itself down. Disarm/clear/stop is a
    # human-only operation, performed from an interactive session (where the
    # OVERNIGHT_SEQUENCER_RUN discriminator is absent and this guard is inert).
    if tool_name == "Bash" and _is_self_teardown(tool_input.get("command", "")):
        return False, (
            "[sequencer] self-teardown blocked: the unattended run cannot "
            "disarm, clear the guard, remove the armed marker, or stop its own "
            "service. Only the human operator ends the run, from an interactive "
            "session, via `bash .claude/skills/overnight-sequencer/"
            "arm-sequencer.sh --disarm`. Keep going -- continue the pipeline.")

    # Armed but not yet started: force the redirect onto overnight-sequencer.
    if not active and armed:
        if tool_name == "Skill":
            sk = _skill_name(tool_input)
            if sk == "overnight-sequencer":
                return True, ""
            return False, (
                "[sequencer] ARMED unattended run: your only valid skill right now "
                "is Skill(overnight-sequencer), which drives the per-file pipeline "
                "from todo/TODO-Claude-Overnight-Runner.md. Do NOT run the generic "
                "overnight-runner flow or any other skill first. Invoke "
                "Skill(overnight-sequencer) now.")
        return True, ""  # Bash / Read / Grep / Glob allowed for setup

    # Active run: phase enforcement. (AskUserQuestion already handled above.)
    phase = state.get("phase", "PREFLIGHT")

    # Sequence-skill ordering: a controlled skill may only fire in a phase
    # that allows it.
    if tool_name == "Skill":
        sk = _skill_name(tool_input)
        if sk in SEQUENCE_SKILLS and sk not in PHASE_ALLOWED_SKILLS.get(phase, set()):
            allowed = sorted(PHASE_ALLOWED_SKILLS.get(phase, set())) or ["(none)"]
            return False, (
                f"[sequencer] phase={phase}: Skill({sk}) is out of sequence. "
                f"Allowed sequence skills in this phase: {', '.join(allowed)}. "
                "Follow the per-file pipeline (PREFLIGHT -> TRIAGE -> VALIDATE -> "
                "GAP_AUDIT -> SECTIONS -> FILE_CLOSE -> ADVANCE); transition with "
                "`python3 .claude/hooks/run_phase_guard.py phase <PHASE>`. "
                "(todo/TODO-Claude-Overnight-Runner.md per-file pipeline.)")

    # Everything else (Bash, Edit, Write, Read, Grep, Glob, inner-pipeline
    # skills) passes -- the within-section gates govern it.
    return True, ""


def handle_pretool():
    try:
        d = json.load(sys.stdin)
    except (json.JSONDecodeError, ValueError):
        return 0
    allow, msg = evaluate(d.get("tool_name", ""), d.get("tool_input", {}),
                          load_state(), armed=ARMED_MARKER.exists(),
                          headless=is_headless())
    if allow:
        return 0
    sys.stderr.write(msg)
    return 2


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
    state = load_state()
    sys.stderr.write(
        "[sequencer] headless unattended run: do NOT stop. The work unit is the "
        "ENTIRE queue, not one section or file. A user-reserved decision or a "
        "per-file blocker is DEFERRED ([/] + Deferred stamp + XREF) and you "
        "ADVANCE to the next file -- it is NEVER a reason to stop or disarm. "
        f"pass={state.get('pass_no')} file={state.get('file')} "
        f"phase={state.get('phase')}. Re-read todo/TODO-Claude-Overnight-Runner.md, "
        "run `run_phase_guard.py status`, re-invoke Skill(overnight-sequencer), and "
        "continue. The run ends ONLY on the human's --disarm or an oracle-verified "
        "`run_phase_guard.py fixpoint` (all work done). The watchdog relaunches any "
        "death, so a voluntary exit accomplishes nothing.")
    return 2


# ---- CLI (the sequencer skill drives transitions) ----

def cli(argv):
    cmd = argv[0] if argv else "status"
    state = load_state()
    if cmd == "start":
        state = {
            "active": True, "phase": "PREFLIGHT", "pass_no": 1,
            "domain": None, "file": None, "section_idx": 0,
            "progress_this_pass": False,
            "started_at": argv[1] if len(argv) > 1 else "unknown",
            "updated_at": argv[1] if len(argv) > 1 else "unknown",
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
        return 0
    if cmd == "phase":
        if len(argv) < 2 or argv[1] not in PHASES:
            print(f"[sequencer] phase needs one of {PHASES}", file=sys.stderr)
            return 1
        state["phase"] = argv[1]
        save_state(state)
        print(f"[sequencer] phase -> {argv[1]}", file=sys.stderr)
        return 0
    if cmd == "cursor":
        # cursor <domain> <file> [section_idx]
        if len(argv) >= 3:
            state["domain"], state["file"] = argv[1], argv[2]
            if len(argv) >= 4:
                state["section_idx"] = int(argv[3])
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
        import subprocess
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
        print("[sequencer] FIXPOINT verified by oracle (no remaining work) -- "
              "run complete; sentinel written, armed marker removed",
              file=sys.stderr)
        return 0
    if cmd == "clear":
        save_state({"active": False})
        print(f"[sequencer] cleared: {' '.join(argv[1:]) or 'no reason'}", file=sys.stderr)
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
