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
    subprocess.run([sys.executable, str(HOOK)], input=json.dumps(payload), text=True,
                   cwd=root, env=env, capture_output=True, timeout=60)
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
    env = {**os.environ, "CODEX_COMPANION_PATH": str(fake), "CODEX_OUTAGE_OVERRIDE": "1"}
    subprocess.run(["bash", str(REPO / "scripts/codex-dispatch.sh"), prompt], capture_output=True, text=True,
                   timeout=60, cwd=repo, env=env)
    rows = [json.loads(x) for x in (repo / ".claude/overnight/reviews/manifest.jsonl").read_text().splitlines()]
    want = hashlib.sha256(prompt.encode()).hexdigest()
    assert rows and rows[-1]["prompt_sha256"] == want, (rows[-1:], want)
