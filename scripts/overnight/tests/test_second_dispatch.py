#!/usr/bin/env python3
"""scripts/second_review_dispatch.py: one pass of the second reviewer, driven with a fake reviewer CLI.

The fake prints the stream the real CLI prints and does what FAKE_MODE says: answer, answer without a verdict,
fail, hang (with a child of its own), or change its copy of the candidate before answering.
"""
import json
import os
import pathlib
import subprocess
import sys
import time

REPO = pathlib.Path(__file__).resolve().parents[3]
SCRIPT = REPO / "scripts" / "second_review_dispatch.py"
TODO = "todo/00-x/TODO-01-x.md"
PROMPT = f"[review-kind: second] {TODO} section 7\nRead the change."

FAKE = r'''#!/usr/bin/env python3
import json, os, pathlib, subprocess, sys, time
mode = os.environ.get("FAKE_MODE", "ok")
log = pathlib.Path(os.environ["FAKE_LOG"])
with log.open("a") as fh:
    fh.write(json.dumps({"cwd": os.getcwd(), "argv": sys.argv[1:], "pid": os.getpid()}) + "\n")
calls = len(log.read_text().splitlines())
def answer(text):
    print(json.dumps({"event": "progress"}))
    print(json.dumps({"event": "result", "result": {"status": "SUCCESS", "response": text}}))
if mode == "fail" or (mode == "fail-then-ok" and calls == 1):
    sys.exit(1)
if mode == "hang":
    # A helper of its own, which must not outlive the attempt. Bounded: it ends by itself after a minute.
    child = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)"])
    pathlib.Path(os.environ["FAKE_CHILD"]).write_text(str(child.pid))
    time.sleep(60)
    sys.exit(0)
if mode == "noisy":
    # Bounded: about 40 MB at most, then it ends by itself.
    chunk = json.dumps({"event": "progress", "text": "x" * 4000})
    for _ in range(10000):
        print(chunk, flush=True)
    answer("HIGHEST: none\n")
    sys.exit(0)
if mode == "edit":
    pathlib.Path("src/kernel/a.c").write_text("int a = 2; /* repaired by the reviewer */\n")
if mode == "add":
    pathlib.Path("notes.txt").write_text("scratch\n")
if mode == "commit":
    pathlib.Path("src/kernel/a.c").write_text("int a = 2;\n")
    subprocess.run(["git", "-c", "user.name=r", "-c", "user.email=r@example.invalid", "commit", "-qam", "repair"], check=True)
if mode == "nohighest":
    answer("P2 src/kernel/a.c:1 a is never read.\n")
elif mode == "two":
    answer("HIGHEST: P1\nOn reflection.\nHIGHEST: none\n")
elif mode == "blocking":
    answer("P2 src/kernel/a.c:1 a is never read.\nHIGHEST: P2\n")
elif mode == "empty":
    print(json.dumps({"event": "result", "result": {"status": "SUCCESS", "response": "  "}}))
elif mode == "refused":
    print(json.dumps({"event": "result", "result": {"status": "ERROR", "response": "HIGHEST: none"}}))
else:
    answer("No defects found.\nHIGHEST: none\n")
'''


def _git(root, *args):
    done = subprocess.run(["git", "-C", str(root), "-c", "user.name=t", "-c", "user.email=t@example.invalid",
                           "-c", "core.hooksPath=/dev/null", *args], capture_output=True, text=True, timeout=60)
    assert done.returncode == 0, done.stderr
    return done.stdout.strip()


def _repo(tmp_path, source="src/kernel/a.c"):
    root = tmp_path / "repo"
    root.mkdir(parents=True)
    _git(root, "init", "-q", "-b", "main")
    (root / ".conclave").mkdir()
    (root / ".conclave/reviewers.toml").write_text((REPO / ".conclave/reviewers.toml").read_text())
    (root / ".gitignore").write_text(".claude/\n")
    (root / TODO).parent.mkdir(parents=True)
    (root / TODO).write_text("# X\n\n## 7. Seven\n")
    _git(root, "add", "-A")
    _git(root, "commit", "-q", "-m", "plan")
    base = _git(root, "rev-parse", "HEAD")
    (root / source).parent.mkdir(parents=True, exist_ok=True)
    (root / source).write_text("int a = 1;\n")
    _git(root, "add", "-A")
    _git(root, "commit", "-q", "-m", "build")
    fake = tmp_path / "fake-reviewer"
    fake.write_text(FAKE)
    fake.chmod(0o755)
    # The routed reviewer's checked plan pass: where the section started.
    manifest = root / ".claude/overnight/reviews/manifest.jsonl"
    manifest.parent.mkdir(parents=True)
    manifest.write_text(json.dumps({"kind": "plan", "todo": TODO, "section": 7, "head": base, "rc": 0,
                                    "route": "codex-dispatch.sh", "pass_checked": True, "highest": "none"}) + "\n")
    return root, base, fake


def _run(root, base, fake, mode="ok", prompt=PROMPT, tmp=None, **env):
    tmp = tmp or root.parent
    full = dict(os.environ, SECOND_REVIEW_CLI=str(fake), SECOND_REVIEW_LIMIT="2",
                SECOND_REVIEW_GRACE="1", SECOND_REVIEW_RETRY_WAIT="0", FAKE_MODE=mode,
                FAKE_LOG=str(tmp / "fake.log"), FAKE_CHILD=str(tmp / "fake.child"))
    full.pop("CODEX_REVIEWS_DIR", None)
    full.pop("REVIEW_BASE", None)
    full.update(env)
    return subprocess.run([sys.executable, str(SCRIPT), prompt], cwd=str(root), env=full, capture_output=True,
                          text=True, timeout=120)


def _receipts(root):
    path = root / ".claude/overnight/reviews/manifest.jsonl"
    lines = [json.loads(line) for line in path.read_text().splitlines()] if path.exists() else []
    return [e for e in lines if e["kind"] == "second"]


def _calls(root):
    path = root.parent / "fake.log"
    return [json.loads(line) for line in path.read_text().splitlines()] if path.exists() else []


def test_a_clean_pass_is_receipted_and_the_tree_is_untouched(tmp_path):
    root, base, fake = _repo(tmp_path)
    head = _git(root, "rev-parse", "HEAD")
    r = _run(root, base, fake)
    assert r.returncode == 0, r.stderr
    assert "HIGHEST: none" in r.stdout
    (receipt,) = _receipts(root)
    assert receipt["kind"] == "second" and receipt["route"] == "second_review_dispatch.py"
    assert receipt["head"] == head and receipt["base"] == base and receipt["rc"] == 0 and receipt["pass_checked"] is True
    assert receipt["highest"] == "none" and receipt["unavailable"] is False
    assert receipt["todo"] == TODO and receipt["section"] == 7
    assert "HIGHEST: none" in pathlib.Path(receipt["logFile"]).read_text()
    # The reviewer ran in a copy, with the pinned model, and was told the range to read.
    (call,) = _calls(root)
    assert pathlib.Path(call["cwd"]).resolve() != root.resolve() and not pathlib.Path(call["cwd"]).exists()
    assert call["argv"][call["argv"].index("--model") + 1] == "gemini-4-carbon-medium"
    assert f"git diff {base} {head}" in call["argv"][1] and "Read the change." in call["argv"][1]
    assert _git(root, "rev-parse", "HEAD") == head
    assert _git(root, "status", "--porcelain") == ""      # only the ignored reviews directory was written


def test_a_blocking_review_is_a_checked_pass_at_its_level(tmp_path):
    root, base, fake = _repo(tmp_path)
    assert _run(root, base, fake, "blocking").returncode == 0
    assert _receipts(root)[-1]["highest"] == "P2"


def test_a_review_without_one_verdict_line_is_incomplete(tmp_path):
    root, base, fake = _repo(tmp_path)
    for mode in ("nohighest", "two"):
        r = _run(root, base, fake, mode)
        assert r.returncode == 3, (mode, r.stderr)
        receipt = _receipts(root)[-1]
        assert receipt["rc"] == 3 and receipt["highest"] == "" and receipt["unavailable"] is False


def test_a_reviewer_that_changes_its_copy_has_reviewed_something_else(tmp_path):
    root, base, fake = _repo(tmp_path)
    for mode in ("edit", "add", "commit"):
        r = _run(root, base, fake, mode)
        assert r.returncode == 3, (mode, r.stderr)
        assert "changed its copy" in r.stderr
        receipt = _receipts(root)[-1]
        assert receipt["rc"] == 3 and receipt["unavailable"] is False and receipt["highest"] == ""
    # Every attempt started from a fresh copy, and none reached the real tree.
    assert len({c["cwd"] for c in _calls(root)}) == 9
    assert (root / "src/kernel/a.c").read_text() == "int a = 1;\n" and not (root / "notes.txt").exists()


def test_an_unavailable_reviewer_is_retried_then_recorded_and_never_a_clean_pass(tmp_path):
    root, base, fake = _repo(tmp_path)
    for mode in ("fail", "empty", "refused"):
        r = _run(root, base, fake, mode)
        assert r.returncode == 4, (mode, r.stderr)
        receipt = _receipts(root)[-1]
        assert receipt["rc"] == 4 and receipt["unavailable"] is True and receipt["highest"] == ""
    assert len(_calls(root)) == 9          # three attempts each
    # One failure, then an answer: a pass.
    (root.parent / "fake.log").unlink()
    r = _run(root, base, fake, "fail-then-ok")
    assert r.returncode == 0, r.stderr
    assert len(_calls(root)) == 2 and _receipts(root)[-1]["rc"] == 0


def test_a_reviewer_that_floods_its_output_is_stopped_at_the_cap(tmp_path):
    root, base, fake = _repo(tmp_path)
    r = _run(root, base, fake, "noisy", SECOND_REVIEW_MAX_OUTPUT="200000", SECOND_REVIEW_LIMIT="30")
    assert r.returncode == 4, r.stderr
    assert "more than 200000 bytes of output" in r.stderr
    assert _receipts(root)[-1]["unavailable"] is True
    for call in _calls(root):
        assert not pathlib.Path(call["cwd"]).exists()


def test_a_hung_reviewer_is_killed_with_its_children(tmp_path):
    root, base, fake = _repo(tmp_path)
    start = time.monotonic()
    r = _run(root, base, fake, "hang")
    assert r.returncode == 4, r.stderr
    assert time.monotonic() - start < 30
    # Out of time is not retried: the same work would run out again.
    assert "did not finish in 2 s" in r.stderr and len(_calls(root)) == 1
    for pid in [c["pid"] for c in _calls(root)] + [int((root.parent / "fake.child").read_text())]:
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            continue
        raise AssertionError(f"process {pid} outlived its attempt")


def test_refusals_before_dispatch(tmp_path):
    root, base, fake = _repo(tmp_path)
    for prompt in ("Read the change.", f"[review-kind: round] {TODO} section 7\nx", f"[review-kind: second] {TODO}\nx",
                   f" [review-kind: second] {TODO} section 7 and more\nx"):
        r = _run(root, base, fake, prompt=prompt)
        assert r.returncode == 2 and "first line" in r.stderr, (prompt, r.stderr)
    # The range is the section's, read from its plan pass. A base that names a later commit (the last repair's
    # parent) or nothing is refused; the plan pass's own commit is accepted.
    for wrong in (_git(root, "rev-parse", "HEAD"), "not-a-commit"):
        r = _run(root, base, fake, REVIEW_BASE=wrong)
        assert r.returncode == 2 and "REVIEW_BASE names" in r.stderr, r.stderr
    assert not _calls(root)
    assert _run(root, base, fake, REVIEW_BASE=base).returncode == 0
    (root.parent / "fake.log").unlink()
    (root / ".claude/overnight/reviews/manifest.jsonl").write_text(json.dumps(
        {"kind": "plan", "todo": TODO, "section": 7, "head": base, "rc": 0, "route": "codex-dispatch.sh",
         "pass_checked": True, "highest": "none"}) + "\n")
    # A tracked change the clone would not hold.
    (root / "src/kernel/a.c").write_text("int a = 3;\n")
    r = _run(root, base, fake)
    assert r.returncode == 2 and "commit the candidate first" in r.stderr
    _git(root, "checkout", "--", "src/kernel/a.c")
    # No second reviewer pinned.
    pin = root / ".conclave/reviewers.toml"
    text = pin.read_text()
    keep, skipping = [], False
    for line in text.splitlines(keepends=True):
        if line.startswith("["):
            skipping = line.strip() in ("[reviewers.carbon]", "[second]")
        if not skipping:
            keep.append(line)
    pin.write_text("".join(keep))
    _git(root, "commit", "-qam", "no second reviewer")
    r = _run(root, base, fake)
    assert r.returncode == 2 and "names no second reviewer" in r.stderr
    assert not _calls(root) and not _receipts(root)


def test_refusal_a_section_with_no_plan_pass_has_no_known_start(tmp_path):
    root, base, fake = _repo(tmp_path)
    (root / ".claude/overnight/reviews/manifest.jsonl").write_text("")
    r = _run(root, base, fake, REVIEW_BASE=base)
    assert r.returncode == 2 and "no checked plan pass" in r.stderr
    assert not _calls(root)


def _wait_for(path, seconds=20):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        if path.exists() and path.read_text().strip():
            return
        time.sleep(0.05)
    raise AssertionError(f"{path} never appeared")


def _alive(pid):
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    # A killed child that nobody reaped yet is not running.
    try:
        return pathlib.Path(f"/proc/{pid}/stat").read_text().split(")")[-1].split()[0] != "Z"
    except OSError:
        return False


def _start(root, fake, mode, **env):
    tmp = root.parent
    full = dict(os.environ, SECOND_REVIEW_CLI=str(fake), SECOND_REVIEW_LIMIT="40", SECOND_REVIEW_GRACE="1",
                SECOND_REVIEW_RETRY_WAIT="0", FAKE_MODE=mode, FAKE_LOG=str(tmp / "fake.log"),
                FAKE_CHILD=str(tmp / "fake.child"))
    full.pop("CODEX_REVIEWS_DIR", None)
    full.pop("REVIEW_BASE", None)
    full.update(env)
    return subprocess.Popen([sys.executable, str(SCRIPT), PROMPT], cwd=str(root), env=full, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, text=True, start_new_session=True)


def test_a_stopped_dispatcher_takes_the_reviewer_and_its_copy_with_it(tmp_path):
    root, base, fake = _repo(tmp_path)
    proc = _start(root, fake, "hang")
    try:
        _wait_for(root.parent / "fake.child")
        (call,) = _calls(root)
        child = int((root.parent / "fake.child").read_text())
        assert _alive(call["pid"]) and _alive(child) and pathlib.Path(call["cwd"]).exists()
        proc.terminate()
        _out, err = proc.communicate(timeout=30)
    finally:
        if proc.poll() is None:
            os.killpg(proc.pid, 9)
            proc.wait()
    assert proc.returncode == 143 and "stopped before the pass finished" in err, err
    end = time.monotonic() + 10
    while time.monotonic() < end and (_alive(call["pid"]) or _alive(child)):
        time.sleep(0.05)
    assert not _alive(call["pid"]) and not _alive(child), "the reviewer outlived its dispatcher"
    assert not pathlib.Path(call["cwd"]).exists(), "the copy of the candidate was left behind"
    assert not _receipts(root)


def test_refusal_two_passes_for_one_section_do_not_overlap(tmp_path):
    root, base, fake = _repo(tmp_path)
    proc = _start(root, fake, "hang")
    try:
        _wait_for(root.parent / "fake.child")
        r = _run(root, base, fake)
        assert r.returncode == 2 and "another pass of the second reviewer is running" in r.stderr, r.stderr
        assert len(_calls(root)) == 1 and not _receipts(root)
    finally:
        proc.terminate()
        proc.communicate(timeout=30)
    # Once it is gone the section can be dispatched again.
    assert _run(root, base, fake).returncode == 0


def test_a_section_outside_the_paths_dispatches_nothing(tmp_path):
    root, base, fake = _repo(tmp_path, source="scripts/a.sh")
    r = _run(root, base, fake)
    assert r.returncode == 0 and "not in scope" in r.stdout
    assert not _calls(root) and not _receipts(root)


def test_the_round_cap_refuses_a_further_pass(tmp_path):
    root, base, fake = _repo(tmp_path)
    for _ in range(3):
        assert _run(root, base, fake, "blocking").returncode == 0
    r = _run(root, base, fake, "blocking")
    assert r.returncode == 2 and "file what is still open" in r.stderr
    assert len(_receipts(root)) == 3
    # Passes that did not return a review do not spend the cap.
    other, base2, fake2 = _repo(tmp_path / "b")
    for _ in range(4):
        assert _run(other, base2, fake2, "fail", tmp=other.parent).returncode == 4
