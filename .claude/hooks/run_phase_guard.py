#!/usr/bin/env python3
"""
run_phase_guard.py -- the overnight sequencer's no-deviation phase machine.

Enforces the per-file pipeline from todo/TODO-Claude-Overnight-Runner.md so an
unattended `bypassPermissions` run physically cannot skip a stage, reorder the
sequence, ask a human, or stop before fixpoint.

Roles (dispatched on argv[1]):
  pretool   PreToolUse hook: reads {tool_name, tool_input} on stdin.
            exit 0 = allow, exit 2 + stderr = BLOCK.
  stop      Stop hook: while a run is active and not at FIXPOINT, exit 2 keeps
            the headless run going (the watchdog handles real death).
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
# armed". Until the run is active (overnight-sequencer called `start`), the only
# valid skill is overnight-sequencer -- this is how the headless launch is
# redirected off the plugin's generic flow without editing the plugin.
ARMED_MARKER = repo_root() / ".claude/state/sequencer-armed"

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


def evaluate(tool_name, tool_input, state, armed=False):
    """Return (allow: bool, message: str). Pure -- unit-testable."""
    active = bool(state.get("active"))
    if not active and not armed:
        return True, ""

    # AskUserQuestion is never allowed while armed or active.
    if tool_name == "AskUserQuestion":
        return False, (
            "[sequencer] AskUserQuestion is blocked during an unattended run. "
            "Decide with the conservative/no-op choice and log the assumption, "
            "or DEFER the item (mark [/] + a Deferred stamp with an XREF) and "
            "advance. The run never stops to ask. "
            "(todo/TODO-Claude-Overnight-Runner.md hard rules.)")

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
                          load_state(), armed=ARMED_MARKER.exists())
    if allow:
        return 0
    sys.stderr.write(msg)
    return 2


def handle_stop():
    state = load_state()
    if not state.get("active"):
        return 0
    if state.get("phase") == "FIXPOINT":
        return 0
    sys.stderr.write(
        "[sequencer] run is active and not at FIXPOINT. Do not stop: continue the "
        "per-file pipeline (the work unit is the ENTIRE queue, not one section). "
        f"pass={state.get('pass_no')} file={state.get('file')} "
        f"phase={state.get('phase')}. Re-read todo/TODO-Claude-Overnight-Runner.md, "
        "run `run_phase_guard.py status`, and continue. The watchdog only relaunches "
        "after real death; a voluntary mid-queue exit defeats the runner.")
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
        # The redirect served its purpose; the active run now governs.
        try:
            ARMED_MARKER.unlink()
        except FileNotFoundError:
            pass
        print("[sequencer] started: pass 1, PREFLIGHT", file=sys.stderr)
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
        print("[sequencer] FIXPOINT verified by oracle (no remaining work) -- "
              "run complete", file=sys.stderr)
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

    base = {"active": True, "phase": "VALIDATE"}
    # AskUserQuestion always blocked when active.
    for ph in PHASES:
        a, _ = evaluate("AskUserQuestion", {}, {"active": True, "phase": ph})
        check(not a, f"AskUserQuestion allowed in {ph}")
    # Inactive run allows everything.
    a, _ = evaluate("AskUserQuestion", {}, {"active": False})
    check(a, "AskUserQuestion blocked while run inactive")
    # implement-todo-section blocked in VALIDATE, allowed in SECTIONS.
    a, _ = evaluate("Skill", {"skill": "implement-todo-section"}, {"active": True, "phase": "VALIDATE"})
    check(not a, "implement-todo-section allowed in VALIDATE")
    a, _ = evaluate("Skill", {"skill": "implement-todo-section"}, {"active": True, "phase": "SECTIONS"})
    check(a, "implement-todo-section blocked in SECTIONS")
    # validate-todo-file allowed in VALIDATE, blocked in SECTIONS.
    a, _ = evaluate("Skill", {"name": "validate-todo-file"}, {"active": True, "phase": "VALIDATE"})
    check(a, "validate-todo-file blocked in VALIDATE")
    a, _ = evaluate("Skill", {"name": "validate-todo-file"}, {"active": True, "phase": "SECTIONS"})
    check(not a, "validate-todo-file allowed in SECTIONS")
    # gap-audit-todo only in GAP_AUDIT.
    a, _ = evaluate("Skill", {"skill": "gap-audit-todo"}, {"active": True, "phase": "GAP_AUDIT"})
    check(a, "gap-audit-todo blocked in GAP_AUDIT")
    a, _ = evaluate("Skill", {"skill": "gap-audit-todo"}, {"active": True, "phase": "SECTIONS"})
    check(not a, "gap-audit-todo allowed in SECTIONS (should be GAP_AUDIT only)")
    # Non-sequence skill + Bash/Edit pass in SECTIONS.
    a, _ = evaluate("Skill", {"skill": "kernel-code-quality"}, {"active": True, "phase": "SECTIONS"})
    check(a, "inner-pipeline skill blocked in SECTIONS")
    a, _ = evaluate("Bash", {"command": "git commit"}, {"active": True, "phase": "SECTIONS"})
    check(a, "Bash blocked in SECTIONS")
    # legacy System A skill blocked everywhere active.
    a, _ = evaluate("Skill", {"skill": "overnight-todo-runner"}, {"active": True, "phase": "SECTIONS"})
    check(not a, "legacy overnight-todo-runner allowed inside a sequencer run")
    # Armed-but-not-started: only overnight-sequencer skill allowed.
    a, _ = evaluate("Skill", {"skill": "overnight-sequencer"}, {"active": False}, armed=True)
    check(a, "overnight-sequencer blocked while armed")
    a, _ = evaluate("Skill", {"name": "overnight-runner:start"}, {"active": False}, armed=True)
    check(not a, "generic overnight-runner:start allowed while armed (should redirect)")
    a, _ = evaluate("Bash", {"command": "python3 .claude/hooks/sequencer_triage.py --next"},
                    {"active": False}, armed=True)
    check(a, "Bash blocked while armed (setup needs it)")
    a, _ = evaluate("AskUserQuestion", {}, {"active": False}, armed=True)
    check(not a, "AskUserQuestion allowed while armed")
    # Not armed, not active: everything passes.
    a, _ = evaluate("Skill", {"skill": "anything"}, {"active": False}, armed=False)
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
