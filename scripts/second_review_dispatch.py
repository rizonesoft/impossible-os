#!/usr/bin/env python3
"""Dispatch one pass of the second reviewer (the [second] table of .conclave/reviewers.toml).

Owner: todo/00-infrastructure/TODO-08-automation-hardening.md section 44.

    python3 scripts/second_review_dispatch.py '[review-kind: second] todo/<path>.md section <N>
    <body>'

The second reviewer is another model, read after the routed reviewer's passes are clean, on sections whose
changed files fall under the pin's paths. One argument: the prompt, its first line naming the TODO and section.

What a pass is held to:
  - It reads the whole section: from the commit the section's checked plan pass was dispatched at, to HEAD.
    That base is read from the reviews manifest, never taken from the caller, and goes on the receipt; a
    REVIEW_BASE that names another commit is refused (a later one would show the reviewer only the last repair).
  - One pass a section at a time (a lock held from the cap check to the receipt), so two overlapping passes
    cannot both spend the last one.
  - The candidate is HEAD. A tracked file changed in the working tree refuses the pass: the reviewer reads a
    clone of HEAD and would not see it.
  - The reviewer's CLI has no read-only mode, so it runs in a throwaway clone. After the run the clone must
    still be exactly the candidate (same HEAD, nothing changed, nothing added): a reviewer that repaired a
    defect in its copy and then called it clean has reviewed something else, and that attempt counts for nothing.
  - Each attempt has the pin's time limit and an output limit, and its whole process group is killed at
    either. The reviewer's output goes to a file, never into memory. An attempt that ran out of time is not
    retried: the same work would run out again.
  - A review ends with `HIGHEST: P1`, `P2`, `P3` or `none`, or the pass is incomplete.
  - The candidate is identified before the run and checked again after it.
The receipt goes to the reviews manifest the commit gate reads, with kind `second`.

Exit 0 a checked pass (or the section is outside the second reviewer's paths: nothing dispatched, no receipt),
2 refused before dispatch, 3 incomplete (dispatch it again), 4 the reviewer was unavailable on every attempt
(the stamp may then carry `second: unavailable`).

Environment: REVIEW_BASE (optional, checked against the plan pass); CODEX_REVIEWS_DIR (the reviews directory, as for codex-dispatch.sh);
SECOND_REVIEW_CLI, SECOND_REVIEW_LIMIT, SECOND_REVIEW_GRACE, SECOND_REVIEW_RETRY_WAIT (seconds) and
SECOND_REVIEW_MAX_OUTPUT (bytes) for tests.
"""
import fcntl
import hashlib
import json
import os
import pathlib
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import review_pin  # noqa: E402  (sibling module)

ROUTE = "second_review_dispatch.py"
FIRST_LINE = re.compile(r"^\[review-kind: second\] (todo/\S+\.md) section (\d+)\s*$")
GIT_TIMEOUT = 120
MAX_OUTPUT = 64 * 1024 * 1024      # of the reviewer's event stream; a review is a few kilobytes of it
RULES = """You are the second reviewer of this change. Another reviewer has already passed it; find what it missed.
Rules of this pass:
- Read only. Do not edit, create or delete any file, do not commit, and run no command that changes anything.
  This checkout is a copy; a change to it voids your review.
- The change under review is the output of: git diff {base} {head}
- Report defects only, most severe first, each with file:line and what breaks. No style remarks.
- Tag every finding P1 (wrong result, crash, data loss or a security hole), P2 (a real defect of lesser reach)
  or P3 (minor).
- You have {minutes} minutes. Read the diff first, then the code around what it changes; when time is short,
  report what you have found rather than reading further.
- End with exactly one line: HIGHEST: P1, HIGHEST: P2, HIGHEST: P3 or HIGHEST: none"""


def say(message):
    print(f"[second-review] {message}", file=sys.stderr)


def git(root, *args):
    return subprocess.run(["git", "-C", str(root), *args], capture_output=True, text=True, timeout=GIT_TIMEOUT)


def complete(receipt):
    """Whether a receipt is a checked, returned pass of the second reviewer."""
    return receipt.get("route") == ROUTE and receipt.get("pass_checked") is True and receipt.get("rc") == 0 \
        and receipt.get("unavailable") is not True and isinstance(receipt.get("head"), str)


def spent(manifest, todo, section):
    try:
        lines = manifest.read_text().splitlines()
    except (OSError, UnicodeDecodeError):
        return 0
    count = 0
    for line in lines:
        try:
            e = json.loads(line)
        except ValueError:
            continue
        if isinstance(e, dict) and e.get("kind") == review_pin.SECOND_KIND and e.get("todo") == todo \
                and str(e.get("section", "")) == section and complete(e):
            count += 1
    return count


class Terminated(Exception):
    """The dispatcher was told to stop: unwind through every cleanup, then exit."""


def _stop(signum, _frame):
    raise Terminated(signum)


def attempt(root, head, cli, model, prompt, limit, grace, max_output):
    """One run of the reviewer in a fresh clone of the candidate.

    Returns (response, why): the review text and "", or None and why this attempt gave none
    ("unavailable: ...", "timeout: ..." or "mutated: ...").
    """
    scratch = pathlib.Path(tempfile.mkdtemp(prefix="second-review-"))
    clone, stream = scratch / "repo", scratch / "events"
    try:
        made = subprocess.run(["git", "clone", "--quiet", "--no-hardlinks", str(root), str(clone)],
                              capture_output=True, text=True, timeout=GIT_TIMEOUT)
        if made.returncode != 0 or git(clone, "checkout", "--quiet", "--detach", head).returncode != 0:
            return None, "unavailable: the candidate could not be cloned"
        why = ""
        with stream.open("wb") as out:
            # The reviewer gets a process group of its own so that it and its helpers can be killed together.
            # It is not detached: the dispatcher waits for it, and kills the group at a limit or when stopped.
            proc = subprocess.Popen([cli, "-p", prompt, "--model", model, "--sandbox", "--output-format",
                                     "stream-json", "--print-timeout", f"{limit}s"], cwd=str(clone), stdout=out,
                                    stderr=subprocess.DEVNULL,
                                    start_new_session=True)  # LIFECYCLE-WAIVER: a REVIEW's own group, not the session
            started = time.monotonic()
            try:
                deadline = started + limit + grace
                while proc.poll() is None:
                    if time.monotonic() >= deadline:
                        why = f"timeout: the reviewer did not finish in {limit} s"
                        break
                    if stream.stat().st_size > max_output:
                        why = f"unavailable: the reviewer wrote more than {max_output} bytes of output"
                        break
                    time.sleep(0.2)
            finally:
                # The reviewer starts helpers of its own; none may outlive the attempt.
                try:
                    os.killpg(proc.pid, signal.SIGKILL)
                except (ProcessLookupError, PermissionError):
                    pass
                proc.wait()
        if not why and stream.stat().st_size > max_output:
            why = f"unavailable: the reviewer wrote more than {max_output} bytes of output"   # it ended between two looks
        if why:
            return None, why
        response = None
        with stream.open("r", errors="replace") as events:
            for line in events:         # one event a line; only the last successful result is kept
                try:
                    event = json.loads(line)
                except ValueError:
                    continue
                result = event.get("result") if isinstance(event, dict) else None
                if isinstance(result, dict) and result.get("status") == "SUCCESS" \
                        and isinstance(result.get("response"), str):
                    response = result["response"]
        if response is None or not response.strip():
            # The CLI was given the same limit and ends itself at it. A run that used its time and has no
            # review ran out of time, whoever stopped it: that is not an outage to retry.
            if time.monotonic() - started >= limit * 0.9:
                return None, f"timeout: the reviewer did not finish in {limit} s"
            return None, "unavailable: the reviewer returned no result"
        now = git(clone, "rev-parse", "HEAD")
        status = git(clone, "status", "--porcelain")
        if now.returncode != 0 or now.stdout.strip() != head or status.returncode != 0 or status.stdout.strip():
            return None, "mutated: the reviewer changed its copy of the candidate, so what it judged is not the candidate"
        return response, ""
    except (OSError, subprocess.TimeoutExpired) as exc:
        return None, f"unavailable: {exc}"
    finally:
        shutil.rmtree(scratch, ignore_errors=True)


def main(argv):
    if len(argv) != 1:
        say("expected exactly one argument: the prompt, single-quoted, its first line "
            "`[review-kind: second] todo/<path>.md section <N>`")
        return 2
    prompt = argv[0]
    first = next((line for line in prompt.splitlines() if line.strip()), "")
    m = FIRST_LINE.match(first)
    if not m:
        say("the first line must be exactly `[review-kind: second] todo/<path>.md section <N>`")
        return 2
    todo, section = m.group(1), m.group(2)
    top = subprocess.run(["git", "rev-parse", "--show-toplevel"], capture_output=True, text=True, timeout=GIT_TIMEOUT)
    if top.returncode != 0:
        say("not inside a git repository")
        return 2
    root = pathlib.Path(top.stdout.strip())
    try:
        pin = review_pin.load_pin(root / ".conclave" / "reviewers.toml")
    except review_pin.Refused as exc:
        say(f"the reviewer pin refuses the pass: {exc}")
        return 2
    second = pin["second"]
    if second is None:
        say("the pin names no second reviewer ([second] in .conclave/reviewers.toml)")
        return 2
    try:
        status, base = review_pin.section_status(root, todo, section)
    except review_pin.Refused as exc:
        say(str(exc))
        return 2
    if status != "passes" or not base or git(root, "rev-parse", "--quiet", "--verify", f"{base}^{{commit}}").returncode != 0:
        say("this section has no checked plan pass in this repository, so where it started is unknown; the "
            "second reviewer reads a section after the routed reviewer's passes")
        return 2
    given = os.environ.get("REVIEW_BASE", "")
    if given:
        named = git(root, "rev-parse", "--quiet", "--verify", f"{given}^{{commit}}")
        if named.returncode != 0 or named.stdout.strip() != base:
            say(f"REVIEW_BASE names {given}, but the section's plan pass was dispatched at {base[:10]}: the second "
                "reviewer reads the whole section, so leave REVIEW_BASE unset or give that commit")
            return 2
    head = git(root, "rev-parse", "HEAD").stdout.strip()
    dirty = git(root, "status", "--porcelain", "--untracked-files=no")
    if not head or dirty.returncode != 0 or dirty.stdout.strip():
        say("a tracked file differs from HEAD; the reviewer reads a clone of HEAD, so commit the candidate first")
        return 2
    changed = git(root, "diff", "--no-renames", "--name-only", base, head)
    if changed.returncode != 0:
        say("the changed files of the section could not be listed")
        return 2
    if not review_pin.second_applies(pin, [p for p in changed.stdout.splitlines() if p]):
        print("[second-review] not in scope: no changed file is under the second reviewer's paths; nothing dispatched")
        return 0
    reviews = pathlib.Path(os.environ.get("CODEX_REVIEWS_DIR") or root / ".claude" / "overnight" / "reviews")
    try:
        reviews.mkdir(parents=True, exist_ok=True)
    except OSError as exc:
        say(f"no writable reviews directory ({reviews}): {exc}")
        return 2
    manifest = reviews / "manifest.jsonl"
    # One pass a section at a time, from the cap check to the receipt: two that overlap would both pass the
    # check, and the later one's receipt could then never be replaced.
    lock_name = hashlib.sha256(f"{todo}#{section}".encode()).hexdigest()[:16]
    try:
        lock = (reviews / f"second-{lock_name}.lock").open("w")
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        say("another pass of the second reviewer is running for this section; wait for its receipt")
        return 2
    except OSError as exc:
        say(f"the section's lock could not be taken: {exc}")
        return 2
    try:
        for name in ("SIGTERM", "SIGINT", "SIGHUP"):
            signal.signal(getattr(signal, name), _stop)
        return run(root, reviews, manifest, pin, todo, section, base, head, prompt)
    except Terminated as stop:
        say("stopped before the pass finished: no receipt, the reviewer and its copy are gone")
        return 128 + int(stop.args[0])
    finally:
        lock.close()


def run(root, reviews, manifest, pin, todo, section, base, head, prompt):
    second = pin["second"]
    if spent(manifest, todo, section) >= second["round_cap"]:
        say(f"this section has had its {second['round_cap']} passes of the second reviewer; file what is still "
            "open as a tracked item and ship")
        return 2

    cli = os.environ.get("SECOND_REVIEW_CLI") or second["cli"]
    limit = int(os.environ.get("SECOND_REVIEW_LIMIT") or second["limit_seconds"])
    max_output = int(os.environ.get("SECOND_REVIEW_MAX_OUTPUT") or MAX_OUTPUT)
    wait = float(os.environ.get("SECOND_REVIEW_RETRY_WAIT") or 20)
    grace = float(os.environ.get("SECOND_REVIEW_GRACE") or 30)   # past the CLI's own limit, before it is killed
    full = prompt + "\n\n" + RULES.format(base=base, head=head, minutes=max(1, limit // 60))
    stamp = time.strftime("%Y%m%d-%H%M%S")
    log = reviews / f"{stamp}-{os.getpid()}-second-direct.out"
    notes, response, mutated = [], None, False
    for n in range(1, second["retries"] + 1):
        response, why = attempt(root, head, cli, second["model"], full, limit, grace, max_output)
        if response is not None:
            break
        notes.append(f"attempt {n}: {why}")
        say(notes[-1])
        mutated = mutated or why.startswith("mutated")
        if why.startswith("timeout") or n == second["retries"]:
            break                       # the same work would run out of time again
        time.sleep(wait)

    rc, level = 0, ""
    if response is None:
        # Every attempt that answered had changed its copy: the reviewer is there, the pass is not usable.
        rc = 3 if mutated else 4
    else:
        level = review_pin.highest(response)
        if not level:
            rc = 3
            notes.append("incomplete: the review has no single HIGHEST: line (P1, P2, P3 or none)")
            say(notes[-1])
    after = git(root, "rev-parse", "HEAD").stdout.strip()
    moved = git(root, "status", "--porcelain", "--untracked-files=no")
    if rc == 0 and (after != head or moved.returncode != 0 or moved.stdout.strip()):
        rc, level = 3, ""
        notes.append("incomplete: the candidate changed while it was being reviewed; dispatch the pass again")
        say(notes[-1])

    body = (response or "") + ("\n" if response and not response.endswith("\n") else "")
    log.write_text(body + "".join(f"[second-review] {note}\n" for note in notes) + f"Turn completed (rc={rc})\n")
    if response:
        print(response)
    receipt = {"ts": int(time.time()), "kind": review_pin.SECOND_KIND, "todo": todo, "section": int(section),
               "jobId": f"second:{stamp}-{os.getpid()}", "logFile": str(log),
               "prompt_sha256": hashlib.sha256(prompt.encode()).hexdigest(), "head": head, "base": base,
               "route": ROUTE,
               "rc": rc, "pass_checked": True, "highest": level, "unavailable": rc == 4, "attempts": len(notes) + (rc == 0)}
    try:
        # An append cut short earlier leaves a line with no end; this receipt must not be glued onto it.
        glue = manifest.exists() and manifest.stat().st_size > 0
        if glue:
            with manifest.open("rb") as fh:
                fh.seek(-1, os.SEEK_END)
                glue = fh.read(1) != b"\n"
        with manifest.open("a") as fh:
            fh.write(("\n" if glue else "") + json.dumps(receipt) + "\n")
    except OSError as exc:
        say(f"the receipt could not be written to {manifest}: {exc}; the pass will not be credited")
        return 3
    if rc == 4:
        say("the second reviewer gave no review (unavailable, or out of time); the stamp may say "
            "`second: unavailable` and the section is read later")
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
