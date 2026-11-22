#!/usr/bin/env python3
"""The review recorder credits only a review a wrapper actually launched.

A launch is proven by the wrapper's own launch line (the broker's {logFile}
line per registered job, the direct dispatcher's transcript line printed once
nothing can refuse any more), never by the absence of a refusal banner. Every
writer of last-codex-review.json goes through one locked update, and a trigger
is published once and never over a newer one.

Run: python3 -m pytest -q scripts/overnight/tests/test_launch_evidence.py
"""
import importlib
import json
import os
import pathlib
import shutil
import pytest
import signal
import time
import shlex
import subprocess
import sys

REPO = pathlib.Path(__file__).resolve().parents[3]
HOOK = REPO / ".claude/hooks/codex_review_completed.py"
sys.path.insert(0, str(REPO / ".claude/hooks"))
crc = importlib.import_module("codex_review_completed")

BROKER = "bash scripts/overnight/review-broker-codex-dispatch.sh"
TODO = "todo/00-x/TODO-01-x.md"


def _repo(tmp):
    root = pathlib.Path(tmp)
    subprocess.run(["git", "init", "-q", str(root)], check=True)
    (root / ".claude/state").mkdir(parents=True)
    (root / TODO).parent.mkdir(parents=True)
    (root / TODO).write_text("# x\n## 1. One\n")
    subprocess.run(["git", "-C", str(root), "add", "-A"], check=True)
    subprocess.run(["git", "-C", str(root), "-c", "user.name=t", "-c", "user.email=t@t",
                    "commit", "-qm", "x"], check=True)
    return root


def _dispatch(kind):
    return f"{BROKER} '[review-kind: {kind}] {TODO} section 1 body'"


def _hook(root, command, stdout="", stderr=""):
    payload = {"tool_name": "Bash", "session_id": "s", "tool_input": {"command": command},
               "tool_response": {"stdout": stdout, "stderr": stderr}}
    env = {k: v for k, v in os.environ.items() if not k.startswith("SKIP_")}
    r = subprocess.run([sys.executable, str(HOOK)], input=json.dumps(payload), text=True,
                       cwd=root, env=env, capture_output=True, timeout=60)
    assert r.returncode == 0, r.stderr          # a refusal is handled, never a crash
    state = root / ".claude/state/last-codex-review.json"
    stamps = root / ".claude/state/last-review-stamps.json"
    return (json.loads(state.read_text()) if state.exists() else None,
            json.loads(stamps.read_text()) if stamps.exists() else {})


def _job(kind, log="/x/a.out"):
    return json.dumps({"kind": kind, "jobId": "detached:j", "logFile": log})


def _prompt(kind, todo=TODO, section=1):
    return f"[review-kind: {kind}] {todo} section {section} body"


def _row(root, kind, log, section=1, todo=TODO, prompt=None, head=None):
    """The receipt row a dispatcher writes for each job, with its prompt hash."""
    import hashlib
    body = prompt if prompt is not None else _prompt(kind, todo, section)
    m = root / ".claude/overnight/reviews/manifest.jsonl"
    m.parent.mkdir(parents=True, exist_ok=True)
    import time
    import uuid
    row = {"ts": int(time.time()), "jobId": f"detached:{uuid.uuid4().hex}", "kind": kind, "todo": todo,
           "section": section, "logFile": log,
           "prompt_sha256": hashlib.sha256(body.encode()).hexdigest()}
    if head:
        row["head"] = head
    with m.open("a") as fh:
        fh.write(json.dumps(row) + "\n")


def test_a_registered_broker_job_is_recorded_and_stamped(tmp_path):
    root = _repo(tmp_path)
    _row(root, "adversarial", "/x/a.out", prompt=f"[review-kind: adversarial] {TODO} section 1 body")
    state, stamps = _hook(root, _dispatch("adversarial"), stdout=_job("adversarial") + "\n")
    assert state and state["review_kind"] == "adversarial" and state["log_file"] == "/x/a.out"
    assert isinstance(stamps.get(TODO, {}).get("adversarial"), int), stamps


# ---- the class: printed text is never evidence of a launch ----
# Every case below once fooled a rule that read the tool output (a refusal
# banner on either stream, a banner or launch line quoted by a review, a
# transcript path, a script named in a prompt). Evidence is now only a receipt
# row the dispatcher wrote, claimed once; so any text, with no row, records
# nothing, and the same row is never credited to a second call.
TEXTS = [
    ("", "[review-broker] BLOCK -- expected exactly one prompt argument.\n"),
    ("[review-broker] BLOCK -- expected exactly one prompt argument.\n", ""),
    (json.dumps({"kind": "adversarial", "jobId": "detached:q", "logFile": "/x/q.out"}) + "\n", ""),
    ("review text quoting [codex-dispatch] transcript: /r/1-1-adversarial-direct.out\n", ""),
    ("", "[codex-dispatch] transcript: /tmp/review logs/1-1-adversarial-direct.out\n"),
    ("finding: [codex-bg-dispatch] BLOCK -- x\n", "[codex-dispatch] launched: adversarial\n"),
]


def test_printed_text_never_credits_a_launch_and_a_receipt_credits_once(tmp_path):
    root = _repo(tmp_path)
    for out, err in TEXTS:
        for cmd in (_dispatch("adversarial"), "bash scripts/codex-dispatch.sh " + shlex.quote(_prompt("adversarial"))):
            state, stamps = _hook(root, cmd, stdout=out, stderr=err)
            assert state is None and stamps == {}, (out, err, state, stamps)
    _row(root, "adversarial", "/x/a.out")
    state, stamps = _hook(root, _dispatch("adversarial"))
    assert state and state["log_file"] == "/x/a.out" and isinstance(stamps[TODO]["adversarial"], int)
    (root / ".claude/state/last-codex-review.json").unlink()
    (root / ".claude/state/last-review-stamps.json").unlink()
    for out, err in TEXTS + [("", "")]:                     # the same prompt again, refused: its row is spent
        state, stamps = _hook(root, _dispatch("adversarial"), stdout=out, stderr=err)
        assert state is None and stamps == {}, (out, err, state, stamps)


def test_a_broker_refusal_records_nothing_whatever_stream_it_is_on(tmp_path):
    root = _repo(tmp_path)
    banner = "[review-broker] BLOCK -- expected exactly one prompt argument.\n"
    for out, err in (("", banner), (banner, "")):          # stderr, or redirected 2>&1
        state, stamps = _hook(root, _dispatch("adversarial") + " extra", stdout=out, stderr=err)
        assert state is None and stamps == {}, (state, stamps)


def test_a_refused_leg_beside_a_launched_one_earns_no_stamp(tmp_path):
    cmd = _dispatch("adversarial") + " extra; " + _dispatch("perf")
    root = _repo(tmp_path)
    _row(root, "perf", "/x/p.out")
    _state, stamps = _hook(root, cmd, stdout=_job("perf", "/x/p.out") + "\n",
                           stderr="[review-broker] BLOCK -- expected exactly one prompt argument.\n")
    entry = stamps.get(TODO, {})
    assert isinstance(entry.get("perf"), int) and not entry.get("adversarial"), entry
    assert _state["review_kind"] == "perf" and _state["log_file"] == "/x/p.out", _state


def test_a_refused_section_never_takes_its_same_kind_siblings_stamp(tmp_path):
    root = _repo(tmp_path)
    (root / "todo/00-x/TODO-02-y.md").write_text("# y\n## 35. Thirty-five\n")
    other = "todo/00-x/TODO-02-y.md"
    cmd = (_dispatch("adversarial") + " extra; "
           + f"{BROKER} " + shlex.quote(_prompt("adversarial", other, 35)))
    _row(root, "adversarial", "/x/b.out", section=35, todo=other)
    _state, stamps = _hook(root, cmd, stdout=_job("adversarial", "/x/b.out") + "\n",
                           stderr="[review-broker] BLOCK -- expected exactly one prompt argument.\n")
    assert not stamps.get(TODO, {}).get("adversarial"), stamps
    assert isinstance(stamps.get(other, {}).get("adversarial"), int), stamps
    assert stamps[other].get("adversarial_section") == "35", stamps[other]


def test_a_quoted_launch_line_in_a_review_stamps_nothing_of_its_own(tmp_path):
    # A direct review streams its text to stdout; it quotes a real broker job of
    # another TODO. Only the direct review's own receipt earns a stamp.
    root = _repo(tmp_path)
    other = "todo/00-x/TODO-02-y.md"
    (root / other).write_text("# y\n## 9. Nine\n")
    direct_log = "/r/20261121-1-1-adversarial-direct.out"
    _row(root, "adversarial", direct_log, head="a" * 40)
    _row(root, "adversarial", "/x/q.out", section=9, todo=other)       # a same-kind job the review quotes
    cmd = "bash scripts/codex-dispatch.sh " + shlex.quote(_prompt("adversarial"))
    state, stamps = _hook(root, cmd, stdout="review text\n" + _job("adversarial", "/x/q.out") + "\nmore\n",
                          stderr=f"[codex-dispatch] transcript: {direct_log}\n")
    assert isinstance(stamps.get(TODO, {}).get("adversarial"), int), stamps
    assert other not in stamps, stamps
    # Auto-receive is bound to the review's own log, never the quoted job's.
    assert state["log_file"] == direct_log, state
    # The stamp names the commit the review launched on, not the hook's HEAD.
    assert stamps[TODO].get("adversarial_head") == "a" * 40, stamps[TODO]


def test_a_lone_launch_claimed_only_on_stdout_is_not_enough_without_a_receipt(tmp_path):
    cmd = "bash scripts/codex-dispatch.sh " + shlex.quote(_prompt("adversarial"))
    _state, stamps = _hook(_repo(tmp_path), cmd, stdout="[codex-dispatch] transcript: /r/x-adversarial-direct.out\n")
    assert stamps == {}, stamps


def test_a_prompt_naming_the_background_wrapper_still_needs_a_launch_line(tmp_path):
    cmd = f"{BROKER} '[review-kind: adversarial] {TODO} section 1 review scripts/codex-bg-dispatch.sh' extra"
    state, stamps = _hook(_repo(tmp_path), cmd, stdout="[review-broker] BLOCK -- expected exactly one prompt argument.\n")
    assert state is None and stamps == {}, (state, stamps)


def test_a_receive_leaves_another_sessions_review_pending(tmp_path):
    root = _repo(tmp_path)
    state = root / ".claude/state/last-codex-review.json"
    state.write_text(json.dumps({"timestamp_ns": 1, "received": False, "session_id": "B"}))
    payload = {"tool_name": "Skill", "session_id": "A",
               "tool_input": {"skill": "superpowers:receiving-code-review"}}
    subprocess.run([sys.executable, str(HOOK)], input=json.dumps(payload), text=True, cwd=root,
                   capture_output=True, timeout=60)
    assert json.loads(state.read_text())["received"] is False


def _locked(path):
    import fcntl
    fd = os.open(str(path) + ".lock", os.O_RDWR | os.O_CREAT, 0o644)
    fcntl.flock(fd, fcntl.LOCK_EX)
    return fd


def test_a_trigger_is_not_published_without_the_lock(tmp_path):
    path = tmp_path / "last-codex-review.json"
    path.write_text(json.dumps({"timestamp_ns": 1, "review_kind": "round"}))
    fd = _locked(path)
    try:
        ok, _err = crc._publish_trigger(path, {"timestamp_ns": 5, "review_kind": "perf"})
    finally:
        os.close(fd)
    assert not ok and json.loads(path.read_text())["review_kind"] == "round"


def test_a_skip_whose_reset_cannot_be_made_is_reported(tmp_path):
    scg = importlib.import_module("section_commit_gate")
    state = tmp_path / ".claude/state/last-codex-review.json"
    state.parent.mkdir(parents=True)
    state.write_text(json.dumps({"timestamp_ns": 1, "received": True}))
    fd = _locked(state)
    try:
        assert scg._reset_review_state(tmp_path) is False
    finally:
        os.close(fd)
    assert scg._reset_review_state(tmp_path) is True
    assert json.loads(state.read_text())["received"] is False
    assert scg._reset_review_state(tmp_path) is True          # already reset


def test_the_broker_ends_a_review_past_its_runtime_limit(tmp_path):
    import time
    repo = _repo(tmp_path / "r")
    fake = tmp_path / "companion.js"
    # Prints and does not end on its own within the limit; bounded at 20 s so a
    # broken limit cannot leave it running.
    fake.write_text("setInterval(() => process.stdout.write('.'), 200);\n"
                    "setTimeout(() => process.exit(0), 20000);\n")
    env = {**os.environ, "CODEX_COMPANION_PATH": str(fake), "CODEX_OUTAGE_OVERRIDE": "1",
           "CODEX_REVIEW_MAX_SECS": "2"}
    r = subprocess.run(["bash", str(REPO / "scripts/overnight/review-broker-codex-dispatch.sh"),
                        f"[review-kind: adversarial] {TODO} section 1 body"],
                       capture_output=True, text=True, timeout=60, cwd=repo, env=env)
    assert r.returncode == 0, r.stderr
    log = pathlib.Path(json.loads(r.stdout.strip().splitlines()[-1])["logFile"])
    done = pathlib.Path(str(log) + ".done")
    t = time.monotonic()
    while not done.exists() and time.monotonic() - t < 45:
        time.sleep(0.5)
    assert done.exists() and done.read_text().strip() == "124", (done.exists(), log.read_text()[-200:])


LIMIT = REPO / "scripts/overnight/review-runtime-limit.sh"


def test_the_runtime_limit_ends_a_descendant_that_ignores_term(tmp_path):
    import signal
    import time
    out = tmp_path / "ticks"
    pg = tmp_path / "pgid"
    # The leader exits on TERM; its child ignores TERM and keeps writing
    # (bounded at 60 ticks so a broken limit cannot run on).
    cmd = (f"echo $$ > {pg}; ( trap '' TERM; for i in $(seq 60); do echo $i >> {out}; sleep 0.2; done ) & "
           "wait")
    try:
        r = subprocess.run(["bash", str(LIMIT), "2", cmd], capture_output=True, text=True, timeout=30)
        assert r.returncode == 124, (r.returncode, r.stderr)
        size = out.stat().st_size
        time.sleep(1.2)
        assert out.stat().st_size == size, "a process of the attempt wrote after the limit returned"
    finally:
        try:
            os.killpg(int(pg.read_text().strip()), signal.SIGKILL)
        except (OSError, ValueError):  # the group is already gone: the expected case
            pass


def test_the_runtime_limit_passes_a_normal_status_through(tmp_path):
    r = subprocess.run(["bash", str(LIMIT), "30", "exit 3"], capture_output=True, text=True, timeout=30)
    assert r.returncode == 3, r
    assert subprocess.run(["bash", str(LIMIT), "0", "true"], capture_output=True, timeout=10).returncode == 2


def test_a_bad_runtime_limit_is_refused(tmp_path):
    env = {**os.environ, "CODEX_OUTAGE_OVERRIDE": "1", "CODEX_REVIEW_MAX_SECS": "soon",
           "CODEX_COMPANION_PATH": str(tmp_path / "c.js")}
    (tmp_path / "c.js").write_text("")
    r = subprocess.run(["bash", str(REPO / "scripts/overnight/review-broker-codex-dispatch.sh"),
                        f"[review-kind: adversarial] {TODO} section 1 body"],
                       capture_output=True, text=True, timeout=60, cwd=_repo(tmp_path / "r"), env=env)
    assert r.returncode == 2 and "[review-broker] BLOCK" in r.stderr, r.stderr


def test_a_trigger_never_replaces_a_newer_one(tmp_path):
    path = tmp_path / "last-codex-review.json"
    path.write_text(json.dumps({"timestamp_ns": 20, "review_kind": "round"}))
    crc._publish_trigger(path, {"timestamp_ns": 10, "review_kind": "adversarial"})
    assert json.loads(path.read_text())["review_kind"] == "round"
    crc._publish_trigger(path, {"timestamp_ns": 30, "review_kind": "perf"})
    assert json.loads(path.read_text())["review_kind"] == "perf"


def test_a_receive_that_cannot_take_the_lock_leaves_the_review_pending(tmp_path):
    import fcntl
    path = tmp_path / "last-codex-review.json"
    path.write_text(json.dumps({"timestamp_ns": 1, "received": False}))
    fd = os.open(str(path) + ".lock", os.O_RDWR | os.O_CREAT, 0o644)
    fcntl.flock(fd, fcntl.LOCK_EX)
    try:
        got = crc._update_state(path, lambda cur: {**cur, "received": True})
    finally:
        os.close(fd)
    assert got is None and json.loads(path.read_text())["received"] is False


def test_a_wrapper_call_beside_a_background_fallback_still_needs_its_receipt(tmp_path):
    root = _repo(tmp_path)
    bg = json.dumps({"kind": "adversarial", "jobId": "bg:1", "logFile": "/x/bg.out"})
    for cmd in (_dispatch("adversarial") + " || bash scripts/codex-bg-dispatch.sh " + shlex.quote(_prompt("adversarial")),
                "test -f scripts/codex-bg-dispatch.sh && " + _dispatch("adversarial")):
        state, stamps = _hook(root, cmd, stdout=bg + "\n")
        assert state is None and stamps == {}, (cmd, state, stamps)


def test_the_direct_receipt_hashes_the_callers_prompt(tmp_path):
    import hashlib
    import time
    repo = _repo(tmp_path / "r")
    fake = tmp_path / "companion.js"
    fake.write_text("process.stdout.write('Verdict: approve\\nHIGHEST: none\\n');\n")
    prompt = _prompt("round")                      # a pinned pass: the dispatcher appends its lenses
    env = {**os.environ, "CODEX_COMPANION_PATH": str(fake), "CODEX_OUTAGE_OVERRIDE": "1",
           "REVIEW_BASE": "HEAD"}                  # a later pass names its section start
    subprocess.run(["bash", str(REPO / "scripts/codex-dispatch.sh"), prompt], capture_output=True, text=True,
                   timeout=60, cwd=repo, env=env)
    rows = [json.loads(x) for x in (repo / ".claude/overnight/reviews/manifest.jsonl").read_text().splitlines()]
    want = hashlib.sha256(prompt.encode()).hexdigest()
    assert rows and rows[-1]["prompt_sha256"] == want, (rows[-1:], want)


def test_a_quoted_wrapper_path_with_a_space_still_needs_its_receipt(tmp_path):
    cmd = "false && bash '/opt/my tools/scripts/codex-dispatch.sh' " + shlex.quote(_prompt("adversarial"))
    state, stamps = _hook(_repo(tmp_path), cmd)
    assert state is None and stamps == {}, (state, stamps)


def test_the_heredoc_template_claims_its_receipt(tmp_path):
    root = _repo(tmp_path)
    body = _prompt("adversarial")
    _row(root, "adversarial", "/x/h.out", prompt=body)        # the dispatcher hashed $PROMPT, newline stripped
    cmd = "PROMPT=$(cat <<'EOF'\n" + body + "\nEOF\n)\nbash scripts/codex-dispatch.sh \"$PROMPT\""
    state, stamps = _hook(root, cmd)
    assert state and state["log_file"] == "/x/h.out", state


def test_the_limit_reports_124_even_when_it_missed_the_term_window(tmp_path):
    import signal
    import time
    pg = tmp_path / "pgid"
    proc = subprocess.Popen(["bash", str(LIMIT), "2", f"echo $$ > {pg}; sleep 30"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        time.sleep(0.5)
        os.kill(proc.pid, signal.SIGSTOP)          # the supervisor sleeps through TERM and the deadline
        time.sleep(3.5)
        os.kill(proc.pid, signal.SIGCONT)
        assert proc.wait(timeout=20) == 124
    finally:
        if proc.poll() is None:
            proc.kill()
        try:
            os.killpg(int(pg.read_text().strip()), signal.SIGKILL)
        except (OSError, ValueError):  # the group is already gone: the expected case
            pass



def test_an_unreadable_ledger_credits_nothing(tmp_path):
    root = _repo(tmp_path)
    _row(root, "adversarial", "/x/a.out")
    (root / ".claude/state/credited-receipts.json").write_text("{not json")
    state, stamps = _hook(root, _dispatch("adversarial"))
    assert state is None and stamps == {}, (state, stamps)
    assert (root / ".claude/state/credited-receipts.json").read_text() == "{not json"


def test_two_launched_legs_after_a_refused_one_are_each_stamped_and_name_no_single_target(tmp_path):
    root = _repo(tmp_path)
    cmd = (_dispatch("round") + " extra; " + _dispatch("adversarial") + "; " + _dispatch("perf"))
    _row(root, "adversarial", "/x/a.out")
    _row(root, "perf", "/x/p.out")
    state, stamps = _hook(root, cmd)
    assert state["review_kind"] == "bundle" and state["log_file"] == "", state
    e = stamps.get(TODO, {})
    assert isinstance(e.get("adversarial"), int) and isinstance(e.get("perf"), int), e


def test_the_limit_polls_without_starting_a_process(tmp_path):
    bindir = tmp_path / "bin"
    bindir.mkdir()
    used = tmp_path / "used"
    (bindir / "sleep").write_text(f'#!/bin/sh\n: > "{used}"\nexec "{shutil.which("sleep")}" "$@"\n')
    (bindir / "sleep").chmod(0o755)
    done = subprocess.run(["bash", str(LIMIT), "5", f"{sys.executable} -c 'import time; time.sleep(0.6)'"],
                          capture_output=True, text=True, timeout=30,
                          env={**os.environ, "PATH": f"{bindir}:{os.environ['PATH']}"})
    assert done.returncode == 0 and not used.exists(), done


def test_the_limit_reads_the_state_after_a_name_holding_spaces(tmp_path):
    named = tmp_path / "review Z job"
    shutil.copy(shutil.which("sleep"), named)          # the process name is the executable's: "review Z job"
    began = time.monotonic()
    done = subprocess.run(["bash", str(LIMIT), "1", f"exec {shlex.quote(str(named))} 30"], capture_output=True,
                          text=True, timeout=20)
    assert done.returncode == 124 and time.monotonic() - began < 2.5, done


def test_a_command_seen_ending_after_the_deadline_is_the_limits(tmp_path):
    """The supervisor is not scheduled across the deadline and the command ends meanwhile: still 124."""
    proc = subprocess.Popen(["bash", str(LIMIT), "1", f"{sys.executable} -c 'import time; time.sleep(1.15)'"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        time.sleep(0.3)
        os.kill(proc.pid, signal.SIGSTOP)
        time.sleep(1.5)
        os.kill(proc.pid, signal.SIGCONT)
        assert proc.wait(timeout=10) == 124
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()


def _gone(pid, within=3.0):
    """Whether `pid` is gone (or a zombie no one can signal into running again) within `within` seconds."""
    end = time.monotonic() + within
    while True:                                        # checked at least once, even with no time to wait
        try:
            with open(f"/proc/{pid}/stat") as fh:
                if fh.read().rsplit(") ", 1)[1].startswith("Z"):
                    return True
        except (FileNotFoundError, ProcessLookupError):
            return True
        if time.monotonic() >= end:
            return False
        time.sleep(0.05)


def test_cancelling_the_limit_ends_its_command(tmp_path):
    pidfile = tmp_path / "pid"
    proc = subprocess.Popen(["bash", str(LIMIT), "30", f"echo $$ > {pidfile}; exec sleep 30"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        end = time.monotonic() + 5
        while not pidfile.exists() or not pidfile.read_text().strip():
            assert time.monotonic() < end
            time.sleep(0.05)
        proc.terminate()
        assert proc.wait(timeout=10) == 143
        assert _gone(int(pidfile.read_text()))
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
        if pidfile.exists() and not _gone(int(pidfile.read_text()), 0):
            os.kill(int(pidfile.read_text()), signal.SIGKILL)


def test_the_limit_ends_a_session_its_command_started(tmp_path):
    """A step that starts its own session (git under the candidate reader) is ended with the attempt, even
    when the step's parent ignores TERM and only the KILL at the deadline ends it."""
    pidfile = tmp_path / "pid"
    cmd = f'trap "" TERM; setsid sh -c "echo \\$\\$ > {pidfile}; exec sleep 30" & wait'
    try:
        done = subprocess.run(["bash", str(LIMIT), "1", cmd], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                              timeout=20)
        assert done.returncode == 124
        assert _gone(int(pidfile.read_text()))
    finally:
        if pidfile.exists() and not _gone(int(pidfile.read_text()), 0):
            os.kill(int(pidfile.read_text()), signal.SIGKILL)


def test_a_process_name_holding_a_newline_does_not_stop_the_limit(tmp_path):
    """The supervisor reads every process on the host: an unrelated one named across lines must not end it."""
    odd = tmp_path / "x\ny) Z 1"
    shutil.copy(shutil.which("sleep"), odd)
    other = subprocess.Popen([str(odd), "30"])
    pidfile = tmp_path / "pid"
    try:
        proc = subprocess.Popen(["bash", str(LIMIT), "30", f"echo $$ > {pidfile}; exec sleep 30"],
                                stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        end = time.monotonic() + 5
        while not pidfile.exists() or not pidfile.read_text().strip():
            assert time.monotonic() < end
            time.sleep(0.05)
        proc.terminate()
        assert proc.wait(timeout=10) == 143, proc.stderr.read()
        assert _gone(int(pidfile.read_text()))
    finally:
        other.kill()
        other.wait()


def test_a_session_started_by_a_term_handler_is_ended_with_the_attempt(tmp_path):
    """The handler starts its cleanup in a session of its own and exits at once: it was not there when the
    TERM went out, and its parent is gone, so only a supervisor its orphans return to can still end it."""
    pidfile = tmp_path / "pid"
    cmd = (f'trap \'setsid sh -c "echo \\$\\$ > {pidfile}; exec sleep 30" & exit 0\' TERM; '
           'sleep 30 & wait')
    try:
        done = subprocess.run(["bash", str(LIMIT), "2", cmd], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                              timeout=20)
        assert done.returncode == 124
        end = time.monotonic() + 3
        while not pidfile.exists() or not pidfile.read_text().strip():
            assert time.monotonic() < end
            time.sleep(0.05)
        assert _gone(int(pidfile.read_text()))
    finally:
        if pidfile.exists() and pidfile.read_text().strip() and not _gone(int(pidfile.read_text()), 0):
            os.kill(int(pidfile.read_text()), signal.SIGKILL)


def _limiter_in_process(tmp_path, body):
    """Runs the limiter's main() in a child python with `body` patching it first; returns (rc, spawned)."""
    import sys
    script = tmp_path / "drive.py"
    script.write_text("import importlib.util, os, sys\n"
                      f"spec = importlib.util.spec_from_file_location('rl', {str(LIMIT.parent / 'review_runtime_limit.py')!r})\n"
                      "rl = importlib.util.module_from_spec(spec); spec.loader.exec_module(rl)\n"
                      "spawned = []\n"
                      "real_spawn = os.posix_spawnp\n"
                      "def spawn(*a, **k):\n    spawned.append(a[1]); return real_spawn(*a, **k)\n"
                      "os.posix_spawnp = spawn\n" + body +
                      "rc = rl.main(sys.argv[1:])\nprint('spawned', len(spawned)); sys.exit(rc)\n")
    return subprocess.run([sys.executable, str(script), "30", "true"], capture_output=True, text=True, timeout=30)


def test_a_deadline_that_passes_while_the_supervisor_sets_up_runs_nothing(tmp_path):
    # The parse reads the clock once; every later read is past the deadline (a host that stalled the setup).
    done = _limiter_in_process(tmp_path, "calls = [0]\nreal = rl.now_cs\n"
                                         "def late():\n    calls[0] += 1\n"
                                         "    return real() if calls[0] == 1 else real() + 10**6\n"
                                         "rl.now_cs = late\n")
    assert done.returncode == 124 and "spawned 0" in done.stdout, done


def test_a_cancellation_before_the_start_runs_nothing(tmp_path):
    done = _limiter_in_process(tmp_path, "import signal\nreal_mask = signal.pthread_sigmask\n"
                                         "def mask(how, sigs):\n"
                                         "    if how == signal.SIG_BLOCK:\n        os.kill(os.getpid(), signal.SIGTERM)\n"
                                         "    return real_mask(how, sigs)\n"
                                         "signal.pthread_sigmask = mask\n")
    assert done.returncode == 143 and "spawned 0" in done.stdout, done


def test_a_number_given_up_and_reused_is_never_signalled(tmp_path):
    """A member's number freed and taken by an unrelated process between the walk and the signal: only a
    process held by a pidfd while proved a member is signalled, so the stranger lives. Run in a child that
    has no other children, so nothing of the test's own is in reach."""
    import sys
    script = tmp_path / "reuse.py"
    script.write_text("import importlib.util, os, signal, subprocess, time\n"
                      f"spec = importlib.util.spec_from_file_location('rl', {str(LIMIT.parent / 'review_runtime_limit.py')!r})\n"
                      "rl = importlib.util.module_from_spec(spec); spec.loader.exec_module(rl)\n"
                      # Not this process's child: started by a shell that exits at once.
                      "pid = int(subprocess.run(['sh', '-c', 'sleep 30 >/dev/null 2>&1 & echo $!'], capture_output=True,"
                      " text=True, timeout=10).stdout)\n"
                      "real, told = rl._stat, []\n"
                      "def lying(p):\n"
                      "    if int(p) == pid and not told:\n        told.append(p); return (b'S', os.getpid())\n"
                      "    return real(p)\n"
                      "rl._stat = lying\n"
                      "n = rl.signal_all(signal.SIGTERM)[0]\n"
                      "time.sleep(0.2)\n"
                      "alive = os.path.exists(f'/proc/{pid}') and "
                      "not open(f'/proc/{pid}/stat').read().rsplit(') ', 1)[1].startswith('Z')\n"
                      "os.kill(pid, signal.SIGTERM)\n"
                      "print('told' if told else 'untold', n, alive)\n")
    done = subprocess.run([sys.executable, str(script)], capture_output=True, text=True, timeout=30)
    assert done.stdout.split() == ["told", "0", "True"], done

def test_the_teardown_ends_by_the_deadline(tmp_path):
    """TERM ignored by a descendant: it is killed before the deadline, and the answer comes by it."""
    began = time.monotonic()
    done = subprocess.run(["bash", str(LIMIT), "2", 'trap "" TERM; sh -c \'trap "" TERM; sleep 30\' & wait'],
                          stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True, timeout=20)
    took = time.monotonic() - began
    assert done.returncode == 124 and took < 1.95, (took, done.stderr)   # KILLed before the end


def test_an_attempt_that_never_finishes_ending_is_answered_by_the_deadline(tmp_path):
    """A process the kernel keeps after KILL (uninterruptible I/O): no fresh budget is started after the
    deadline, the limit answers by it. Modelled in a child: no signal is delivered and no child is ever
    reaped."""
    import sys
    script = tmp_path / "stuck.py"
    script.write_text("import importlib.util, os, sys, time\n"
                      f"spec = importlib.util.spec_from_file_location('rl', {str(LIMIT.parent / 'review_runtime_limit.py')!r})\n"
                      "rl = importlib.util.module_from_spec(spec); spec.loader.exec_module(rl)\n"
                      "rl.signal_all = lambda sig, sent=None: (0, 0)\n"
                      "os.waitpid = lambda pid, flags: (0, 0)\n"
                      "began = time.monotonic()\n"
                      "rc = rl.main(['2', 'true'])   # exits at once; the model never reaps it\n"
                      "print(rc, round(time.monotonic() - began, 2), flush=True)\n"
                      "os._exit(0)\n")
    proc = subprocess.Popen([sys.executable, str(script)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                            start_new_session=True)
    try:
        out, err = proc.communicate(timeout=30)
    finally:
        if proc.poll() is None:
            os.killpg(proc.pid, signal.SIGKILL)
            proc.wait()
    rc, took = out.split()
    assert rc == "124" and float(took) < 2.5, (out, err)


def test_a_cancellation_held_behind_the_mask_runs_nothing(tmp_path):
    # Delivered after the mask is taken: pending in the kernel, never seen by the handler before the start.
    done = _limiter_in_process(tmp_path, "import signal\nreal_mask = signal.pthread_sigmask\n"
                                         "def mask(how, sigs):\n    old = real_mask(how, sigs)\n"
                                         "    if how == signal.SIG_BLOCK:\n        os.kill(os.getpid(), signal.SIGTERM)\n"
                                         "    return old\n"
                                         "signal.pthread_sigmask = mask\n")
    assert done.returncode == 143 and "spawned 0" in done.stdout, done


def test_a_member_whose_first_thread_ended_is_still_ended(tmp_path):
    """A process whose leader thread exited while another runs shows Z in its stat record; it is still a
    member and is ended with the attempt."""
    import sys
    pidfile = tmp_path / "member.pid"
    member = ("import ctypes, os, threading, time\n"
              f"open({str(pidfile)!r} + '.part', 'w').write(str(os.getpid()))\n"
              f"os.rename({str(pidfile)!r} + '.part', {str(pidfile)!r})\n"
              "threading.Thread(target=time.sleep, args=(60,)).start()\n"
              "ctypes.CDLL(None).pthread_exit(None)\n")
    script = tmp_path / "member.py"
    script.write_text(member)
    cmd = f"{sys.executable} {script} & wait"
    try:
        done = subprocess.run([str(LIMIT), "2", cmd], capture_output=True, text=True, timeout=30)
        assert done.returncode == 124, done
        pid = int(pidfile.read_text())
        assert not os.path.exists(f"/proc/{pid}"), "a member outlived the attempt"
    finally:
        try:
            os.kill(int(pidfile.read_text()), signal.SIGKILL)
        except (FileNotFoundError, ValueError, ProcessLookupError):   # never started, or already gone
            pass


def test_a_member_that_cannot_be_held_is_counted_not_skipped(tmp_path):
    """pidfd_open failing for a reason other than the process's exit (descriptor pressure) is reported as
    unheld, never taken for a finished member."""
    import sys
    script = tmp_path / "unheld.py"
    script.write_text("import errno, importlib.util, os, signal, subprocess\n"
                      f"spec = importlib.util.spec_from_file_location('rl', {str(LIMIT.parent / 'review_runtime_limit.py')!r})\n"
                      "rl = importlib.util.module_from_spec(spec); spec.loader.exec_module(rl)\n"
                      "child = subprocess.Popen(['sleep', '30'])\n"
                      "real = os.pidfd_open\n"
                      "def full(pid, flags=0):\n"
                      "    if pid == child.pid:\n        raise OSError(errno.EMFILE, 'Too many open files')\n"
                      "    return real(pid, flags)\n"
                      "os.pidfd_open = full\n"
                      "print(*rl.signal_all(signal.SIGTERM))\n"
                      "child.terminate(); child.wait()\n")
    done = subprocess.run([sys.executable, str(script)], capture_output=True, text=True, timeout=30)
    assert done.stdout.split() == ["0", "1"], done


def _uptime_cs():
    with open("/proc/uptime") as f:
        up = f.read().split()[0]
    whole, frac = up.split(".")
    return int(whole) * 100 + int(frac[:2].ljust(2, "0"))


def test_the_job_supervisor_publishes_a_review_its_deadline_ended(tmp_path):
    """One supervisor for the detached job: the review it ends at the steps' deadline is published as 124 by
    the final one (a limiter nested in another had its grace end the job before it could publish)."""
    log, prompt = tmp_path / "r.out", tmp_path / "r.prompt"
    prompt.write_text("p")
    end = _uptime_cs() + 150
    r = subprocess.run(["python3", str(REPO / "scripts/overnight/review_runtime_limit.py"), "--job", str(log),
                        str(prompt), "--publish-by", str(end + 20), "--until-cs", str(end),
                        f"echo started >> {log}; sleep 30"], capture_output=True, text=True, timeout=30)
    done = pathlib.Path(str(log) + ".done")
    assert r.returncode == 124 and done.read_text().strip() == "124", (r, done.exists())
    assert "started" in log.read_text() and not prompt.exists()


def test_the_job_supervisor_leaves_a_claimed_result_alone(tmp_path):
    log, prompt = tmp_path / "r.out", tmp_path / "r.prompt"
    log.write_text("owned elsewhere\n")
    prompt.write_text("p")
    mark = tmp_path / "ran"
    r = subprocess.run(["python3", str(REPO / "scripts/overnight/review_runtime_limit.py"), "--job", str(log),
                        str(prompt), "--no-deadline", f"touch {mark}"], capture_output=True, text=True, timeout=30)
    assert r.returncode == 0 and not mark.exists() and log.read_text() == "owned elsewhere\n", r
    assert not pathlib.Path(str(log) + ".done").exists()


def test_a_stuck_teardown_with_no_deadline_ends_within_its_own_budget(tmp_path):
    """No deadline: a process the kernel keeps after KILL is reported once the teardown's own budget is spent,
    never waited for until the 'no deadline' horizon (centuries) with a /proc walk every few milliseconds."""
    import sys
    script = tmp_path / "stuck.py"
    script.write_text("import importlib.util, os, sys, time\n"
                      f"spec = importlib.util.spec_from_file_location('rl', {str(LIMIT.parent / 'review_runtime_limit.py')!r})\n"
                      "rl = importlib.util.module_from_spec(spec); spec.loader.exec_module(rl)\n"
                      "rl.signal_all = lambda sig, sent=None: (0, 0)\n"
                      "real = os.waitpid\n"
                      "seen = []\n"
                      "def waitpid(pid, flags):\n"
                      "    if not seen:\n"
                      "        got = real(pid, flags)\n"
                      "        if got[0]:\n"
                      "            seen.append(got)\n"
                      "        return got\n"
                      "    return (0, 0)\n"
                      "os.waitpid = waitpid\n"
                      "began = time.monotonic()\n"
                      "rc = rl.main(['--no-deadline', 'true'])\n"
                      "print(rc, round(time.monotonic() - began, 2), flush=True)\n"
                      "os._exit(0)\n")
    proc = subprocess.Popen([sys.executable, str(script)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                            start_new_session=True)
    try:
        out, err = proc.communicate(timeout=30)
    finally:
        if proc.poll() is None:
            os.killpg(proc.pid, signal.SIGKILL)
            proc.wait()
    rc, took = out.split()
    assert rc == "0" and float(took) < 5, (out, err)


def test_a_job_first_scheduled_after_its_deadline_publishes_the_limit(tmp_path):
    """A detached job the broker handed off but which first ran after the steps' deadline: it still claims the
    result and publishes 124, so the waiter sees a finished attempt, never a review pending for ever."""
    log, prompt = tmp_path / "r.out", tmp_path / "r.prompt"
    prompt.write_text("p")
    mark = tmp_path / "ran"
    end = _uptime_cs() - 50                    # the steps' deadline has passed; the final one has not
    r = subprocess.run(["python3", str(REPO / "scripts/overnight/review_runtime_limit.py"), "--job", str(log),
                        str(prompt), "--publish-by", str(end + 1000), "--until-cs", str(end), f"touch {mark}"],
                       capture_output=True, text=True, timeout=30)
    assert r.returncode == 124 and not mark.exists(), r
    assert pathlib.Path(str(log) + ".done").read_text().strip() == "124" and not prompt.exists()
    # A result another owner holds is left alone, late or not.
    other = tmp_path / "o.out"
    other.write_text("owned\n")
    r = subprocess.run(["python3", str(REPO / "scripts/overnight/review_runtime_limit.py"), "--job", str(other),
                        str(prompt), "--publish-by", str(end + 1000), "--until-cs", str(end), f"touch {mark}"],
                       capture_output=True, text=True, timeout=30)
    assert r.returncode == 0 and other.read_text() == "owned\n" and not pathlib.Path(str(other) + ".done").exists()


def test_a_cancellation_during_the_claim_is_published(tmp_path):
    """TERM to the job's supervisor while its claim runs: the cancellation is watched from the start, so the
    claimed result is published with the cancellation's status and nothing is run."""
    import sys
    log, prompt = tmp_path / "r.out", tmp_path / "r.prompt"
    prompt.write_text("p")
    mark = tmp_path / "ran"
    script = tmp_path / "cancel.py"
    script.write_text("import importlib.util, os, signal, sys\n"
                      f"spec = importlib.util.spec_from_file_location('rl', {str(LIMIT.parent / 'review_runtime_limit.py')!r})\n"
                      "rl = importlib.util.module_from_spec(spec); spec.loader.exec_module(rl)\n"
                      "real = rl._result.claim\n"
                      "def claim(log):\n"
                      "    got = real(log)\n"
                      "    os.kill(os.getpid(), signal.SIGTERM)   # lands after the claim, before the review\n"
                      "    return got\n"
                      "rl._result.claim = claim\n"
                      f"sys.exit(rl.main(['--job', {str(log)!r}, {str(prompt)!r}, '--no-deadline', 'touch {mark}']))\n")
    r = subprocess.run([sys.executable, str(script)], capture_output=True, text=True, timeout=30)
    assert r.returncode == 128 + signal.SIGTERM and not mark.exists(), r
    assert pathlib.Path(str(log) + ".done").read_text().strip() == str(128 + signal.SIGTERM)


def test_a_handoff_larger_than_the_pipe_does_not_stall_the_broker(tmp_path):
    """The fallback job's command can outgrow the pipe (a long checkout path, quoted several times): the
    supervisor reads the hand-off while the broker runs, so the broker's write completes and it exits."""
    log, prompt = tmp_path / "r.out", tmp_path / "r.prompt"
    prompt.write_text("p")
    mark = tmp_path / "launched"
    record = b'response {"logFile": 1}\njob touch ' + str(mark).encode() + b' #' + b'x' * 100000 + b'\nstep answered\n'
    writer = f"import os; os.write(int(os.environ['REVIEW_BROKER_HANDOFF_FD']), {record!r})"
    r = subprocess.run(["python3", str(REPO / "scripts/overnight/review_runtime_limit.py"), "--broker", str(log),
                        str(prompt), "--no-deadline", f"python3 -c {shlex.quote(writer)}"],
                       capture_output=True, text=True, timeout=30)
    assert r.returncode == 0 and r.stdout == '{"logFile": 1}\n', r
    t = time.monotonic()
    while not mark.exists() and time.monotonic() - t < 10:
        time.sleep(0.1)
    assert mark.exists()


def _rl_script(tmp_path, body):
    """A child that loads the limiter, applies `body` (patches), and runs the rest of `body`."""
    script = tmp_path / "rl_child.py"
    script.write_text("import importlib.util, os, signal, sys, time\n"
                      f"spec = importlib.util.spec_from_file_location('rl', {str(LIMIT.parent / 'review_runtime_limit.py')!r})\n"
                      "rl = importlib.util.module_from_spec(spec); spec.loader.exec_module(rl)\n" + body)
    return script


def test_an_answer_the_caller_cannot_receive_is_a_failed_dispatch(tmp_path):
    """The supervisor delivers the broker's answer itself and checks the write: an answer that could not be
    written is exit 1, never a success with no job handle."""
    log, prompt = tmp_path / "r.out", tmp_path / "r.prompt"
    prompt.write_text("p")
    record = b'response {"logFile": 1}\nstep answered\n'
    writer = f"import os; os.write(int(os.environ['REVIEW_BROKER_HANDOFF_FD']), {record!r})"
    with open("/dev/full", "w") as full:
        r = subprocess.run(["python3", str(REPO / "scripts/overnight/review_runtime_limit.py"), "--broker", str(log),
                            str(prompt), "--no-deadline", f"python3 -c {shlex.quote(writer)}"],
                           stdout=full, stderr=subprocess.PIPE, text=True, timeout=30)
    assert r.returncode == 1 and "could not be delivered" in r.stderr, r
    r = subprocess.run(["python3", str(REPO / "scripts/overnight/review_runtime_limit.py"), "--broker", str(log),
                        str(prompt), "--no-deadline", f"python3 -c {shlex.quote(writer)}"],
                       capture_output=True, text=True, timeout=30)
    assert r.returncode == 0 and r.stdout == '{"logFile": 1}\n', r


def test_a_review_that_cannot_be_started_after_the_claim_is_published_as_failed(tmp_path):
    """A process limit at the review's start, after the job owns the result: the owner publishes 1."""
    import sys
    log, prompt = tmp_path / "r.out", tmp_path / "r.prompt"
    prompt.write_text("p")
    script = _rl_script(tmp_path, "import errno\n"
                        "def refuse(*a, **k):\n"
                        "    raise OSError(errno.EAGAIN, 'Resource temporarily unavailable')\n"
                        "os.posix_spawnp = refuse\n"
                        f"sys.exit(rl.main(['--job', {str(log)!r}, {str(prompt)!r}, '--no-deadline', 'true']))\n")
    r = subprocess.run([sys.executable, str(script)], capture_output=True, text=True, timeout=30)
    assert r.returncode == 1 and "could not be started" in r.stderr, r
    assert pathlib.Path(str(log) + ".done").read_text().strip() == "1"


def test_the_job_claims_and_publishes_in_one_process(tmp_path):
    """The class behind three late-claim findings: a claim handed to a helper whose wait can time out. The
    job's supervisor makes both calls itself, so no helper is started for its result at all."""
    import sys
    log, prompt = tmp_path / "r.out", tmp_path / "r.prompt"
    prompt.write_text("p")
    script = _rl_script(tmp_path, "def no_helper(*a, **k):\n"
                        "    raise AssertionError('the job handed its result to a helper')\n"
                        "rl._result_step = no_helper\n"
                        f"sys.exit(rl.main(['--job', {str(log)!r}, {str(prompt)!r}, '--no-deadline', 'exit 3']))\n")
    r = subprocess.run([sys.executable, str(script)], capture_output=True, text=True, timeout=30)
    assert r.returncode == 3, r
    assert pathlib.Path(str(log) + ".done").read_text().strip() == "3" and not prompt.exists()


def test_a_cancelled_publication_wait_ends_and_the_helper_holds_no_caller_stream(tmp_path):
    """A publication helper stalled in I/O: a cancellation ends the supervisor's wait within a second, and
    the helper has streams of its own, so a caller capturing the output sees EOF when the supervisor ends."""
    import sys
    here = tmp_path / "here"
    here.mkdir()
    (here / "review_result.py").write_text("import sys, time\nprint('helper says', file=sys.stderr, flush=True)\n"
                                           "time.sleep(4)\n")
    script = _rl_script(tmp_path, f"rl.HERE = {str(here)!r}\n"
                        "class C:\n    sig = 143\n"
                        "began = time.monotonic()\n"
                        "rc = rl._result_step(['publish', 'x', '1'], rl.now_cs() + 100_000, C())\n"
                        "print(rc, round(time.monotonic() - began, 2), flush=True)\n")
    began = time.monotonic()
    r = subprocess.run([sys.executable, str(script)], capture_output=True, text=True, timeout=30)
    took = time.monotonic() - began
    rc, waited = r.stdout.split()
    assert rc == "None" and float(waited) < 1.6 and took < 3, (r, took)
    assert "helper says" in r.stderr, r


def _full_pipe():
    """A pipe whose buffer is full; the caller keeps the read end open and never drains it."""
    rd, wr = os.pipe()
    os.set_blocking(wr, False)
    try:
        while True:
            os.write(wr, b"x" * 65536)
    except BlockingIOError:
        pass
    os.set_blocking(wr, True)
    return rd, wr


def test_an_answer_to_a_reader_that_stopped_reading_ends_at_the_deadline(tmp_path):
    """The caller's stdout pipe is full and its reader still open: the answer waits for room only until the
    deadline, so the supervisor exits (1, the answer undelivered) instead of hanging."""
    import sys
    log, prompt = tmp_path / "r.out", tmp_path / "r.prompt"
    prompt.write_text("p")
    record = b'response {"logFile": 1}\nstep answered\n'
    writer = f"import os; os.write(int(os.environ['REVIEW_BROKER_HANDOFF_FD']), {record!r})"
    rd, wr = _full_pipe()
    end = _uptime_cs() + 150
    began = time.monotonic()
    try:
        r = subprocess.run([sys.executable, str(REPO / "scripts/overnight/review_runtime_limit.py"), "--broker",
                            str(log), str(prompt), "--publish-by", str(end + 50), "--until-cs", str(end),
                            f"python3 -c {shlex.quote(writer)}"], stdout=wr, stderr=subprocess.PIPE, text=True,
                           timeout=30)
    finally:
        os.close(wr)
        os.close(rd)
    assert r.returncode == 1 and "could not be delivered" in r.stderr and time.monotonic() - began < 6, r


def test_diagnostics_to_a_full_stderr_end_at_the_deadline(tmp_path):
    """A caller that waits for the supervisor before draining stderr: a diagnostic waits for room only until
    the deadline, and everything after it is dropped rather than waited on."""
    import sys
    rd, wr = _full_pipe()
    script = _rl_script(tmp_path, "os.dup2(int(sys.argv[1]), 2)\n"
                        "sys.stderr = rl.Bounded(2, rl.now_cs() + 50)\n"
                        "began = time.monotonic()\n"
                        "for _ in range(5):\n"
                        "    print('x' * 10000, file=sys.stderr)\n"
                        "print(round(time.monotonic() - began, 2), flush=True)\n")
    try:
        r = subprocess.run([sys.executable, str(script), str(wr)], capture_output=True, text=True, timeout=30,
                           pass_fds=(wr,))
    finally:
        os.close(wr)
        os.close(rd)
    assert r.returncode == 0 and float(r.stdout) < 1.5, r


def test_a_job_whose_supervisor_died_takes_its_review_with_it_and_is_recovered(tmp_path):
    """The job's supervisor ends without publishing (out of memory): the attempt's warden ends its whole group
    the moment the supervisor's lifeline closes, so nothing it started keeps running; the owner lock is free,
    so the waiter's recovery publishes 1. A live owner's result is never touched."""
    import sys
    log, prompt = tmp_path / "r.out", tmp_path / "r.prompt"
    prompt.write_text("p")
    result = REPO / "scripts/overnight/review_result.py"
    pids = tmp_path / "pids"
    cmd = f"sleep 60 & echo $$ $! > {pids}; wait"
    sup = subprocess.Popen([sys.executable, str(REPO / "scripts/overnight/review_runtime_limit.py"), "--job", str(log),
                            str(prompt), "--no-deadline", cmd], stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, start_new_session=True)
    live = []
    try:
        t = time.monotonic()
        while time.monotonic() - t < 10 and len(live) < 2:
            live = [int(x) for x in pids.read_text().split()] if pids.exists() else []
            time.sleep(0.05)
        assert len(live) == 2
        r = subprocess.run([sys.executable, str(result), "recover", str(log)], capture_output=True, text=True,
                           timeout=30)
        assert r.returncode == 0 and not pathlib.Path(str(log) + ".done").exists(), r   # alive: untouched
        os.kill(sup.pid, signal.SIGKILL)
        sup.wait()
        t = time.monotonic()
        while any(os.path.exists(f"/proc/{p}") for p in live) and time.monotonic() - t < 5:
            time.sleep(0.05)
        assert not any(os.path.exists(f"/proc/{p}") for p in live), "the attempt outlived its supervisor"
        r = subprocess.run([sys.executable, str(result), "recover", str(log)], capture_output=True, text=True,
                           timeout=30)
        assert r.returncode == 0 and pathlib.Path(str(log) + ".done").read_text().strip() == "1", r
    finally:
        if sup.poll() is None:
            os.kill(sup.pid, signal.SIGKILL)
            sup.wait()
        for p in live:
            if os.path.exists(f"/proc/{p}"):
                os.kill(p, signal.SIGKILL)


def test_recovery_never_takes_a_claim_in_progress(tmp_path):
    """A claimant has opened LOG.owner but not yet locked it, and the waiter recovers then: recovery acts only
    once LOG exists (a claim creates it only after holding the lock), so it never holds the lock a claim is
    reaching for; the claim succeeds and nothing is published for it."""
    import sys
    log = tmp_path / "r.out"
    pathlib.Path(str(log) + ".owner").write_text("")
    rr_path = REPO / "scripts/overnight/review_result.py"
    claimer = tmp_path / "claim.py"
    claimer.write_text("import importlib.util, sys\n"
                       f"spec = importlib.util.spec_from_file_location('rr', {str(rr_path)!r})\n"
                       "rr = importlib.util.module_from_spec(spec); spec.loader.exec_module(rr)\n"
                       f"sys.exit(rr.claim({str(log)!r}))\n")
    during = tmp_path / "claim-during-recovery"
    recoverer = tmp_path / "recover.py"
    # The claim runs at the worst moment: while recovery holds the owner lock, if it ever takes it.
    recoverer.write_text("import importlib.util, subprocess, sys\n"
                         f"spec = importlib.util.spec_from_file_location('rr', {str(rr_path)!r})\n"
                         "rr = importlib.util.module_from_spec(spec); spec.loader.exec_module(rr)\n"
                         "real = rr.fcntl.flock\n"
                         "def flock(fd, op):\n"
                         "    real(fd, op)\n"
                         f"    rc = subprocess.run([sys.executable, {str(claimer)!r}], timeout=30).returncode\n"
                         f"    open({str(during)!r}, 'w').write(str(rc))\n"
                         "rr.fcntl.flock = flock\n"
                         f"sys.exit(rr.recover({str(log)!r}))\n")
    r = subprocess.run([sys.executable, str(recoverer)], capture_output=True, text=True, timeout=60)
    assert r.returncode == 0 and not pathlib.Path(str(log) + ".done").exists(), r
    assert not during.exists() or during.read_text() == "0", "recovery took the lock from a claim in progress"
    if not during.exists():
        assert subprocess.run([sys.executable, str(claimer)], timeout=30).returncode == 0 and log.exists()


def test_a_writer_that_loses_the_room_to_another_never_blocks(tmp_path):
    """Two writers share a full pipe: poll reports room, the other writer takes it, and this one's write
    would block. Writes go through a private nonblocking descriptor, so the loop only waits again, until the
    deadline; the inherited descriptor's flags are left alone."""
    import sys
    rd, wr = _full_pipe()
    script = _rl_script(tmp_path, "import select\n"
                        "fd = int(sys.argv[1])\n"
                        "class Always:\n"
                        "    def register(self, *a): pass\n"
                        "    def poll(self, ms): return [(0, select.POLLOUT)]   # room, taken before the write\n"
                        "rl.select.poll = Always\n"
                        "began = time.monotonic()\n"
                        "ok = rl.emit(fd, b'answer\\n', rl.now_cs() + 30)\n"
                        "print(ok, round(time.monotonic() - began, 2), os.get_blocking(fd), flush=True)\n")
    try:
        r = subprocess.run([sys.executable, str(script), str(wr)], capture_output=True, text=True, timeout=30,
                           pass_fds=(wr,))
    finally:
        os.close(wr)
        os.close(rd)
    ok, took, blocking = r.stdout.split()
    assert ok == "False" and float(took) < 1.5 and blocking == "True", r


def test_a_prompt_that_is_not_the_registered_one_runs_nothing(tmp_path):
    """The job reads the prompt once and checks it against the digest the broker registered: a short or
    changed read is a failed attempt (1) and the reviewer is never started."""
    import hashlib
    import sys
    log, prompt = tmp_path / "r.out", tmp_path / "r.prompt"
    prompt.write_text("[review-kind: round] todo/x.md section 1")   # the first line only: the rest was lost
    mark = tmp_path / "ran"
    whole = hashlib.sha256(b"[review-kind: round] todo/x.md section 1\nthe rest of the scope\n").hexdigest()
    r = subprocess.run([sys.executable, str(REPO / "scripts/overnight/review_runtime_limit.py"), "--job", str(log),
                        str(prompt), "--prompt-sha", whole, "--no-deadline", f"touch {mark}"],
                       capture_output=True, text=True, timeout=30)
    assert r.returncode == 1 and not mark.exists(), r
    assert pathlib.Path(str(log) + ".done").read_text().strip() == "1" and "digest differs" in log.read_text()
    # The registered prompt reaches the reviewer whole, trailing newline included.
    log2 = tmp_path / "s.out"
    prompt.write_text("p\n")
    r = subprocess.run([sys.executable, str(REPO / "scripts/overnight/review_runtime_limit.py"), "--job", str(log2),
                        str(prompt), "--prompt-sha", hashlib.sha256(b"p\n").hexdigest(), "--no-deadline",
                        f"printf %s \"$REVIEW_PROMPT_TEXT\" > {mark}"], capture_output=True, text=True, timeout=30)
    assert r.returncode == 0 and mark.read_text() == "p\n", r
