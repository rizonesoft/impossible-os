#!/usr/bin/env python3
"""The sections whose review is owed because the reviewer was down when they were built.

Owner: todo/00-infrastructure/TODO-08-automation-hardening.md section 45.

Every section starts with a plan pass, so while the routed reviewer's backend is down the next section can
only be built unreviewed. That is allowed, as a debt: the section is recorded here BEFORE it is built, the run
moves on, nothing is pushed while a debt is open, and the owed sections are reviewed first when the reviewer
answers again. An outage is a delay, never an approval.

    python3 scripts/overnight/review_owed.py add --todo todo/<path>.md --section N
    python3 scripts/overnight/review_owed.py list
    python3 scripts/overnight/review_owed.py gate
    python3 scripts/overnight/review_owed.py clear --todo todo/<path>.md --section N
    python3 scripts/overnight/review_owed.py probe

add     before the section's first edit. Refused unless `probe` reports an outage now. Records the section and
        its base: the commit its checked plan pass was dispatched at when it has one (the outage came at
        review time), else HEAD (the commit before its code). Adding a section already recorded changes
        nothing, so the first base stands and an interrupted run may call it again. The outage is confirmed
        at this moment, so the dispatch record that failed with it (.claude/state/last-codex-review.json,
        unreceived) is marked `outage_debt` here: the rollover then knows it is this debt and not a review
        waiting to be read. When an owed section's review is tried again and fails again, `add` again marks
        that new record the same way.
list    one line a section: `<todo> section <N> base <sha>`.
gate    exit 0 when nothing is owed, 1 naming each owed section, 2 when the record cannot be read (a caller
        that guards a push refuses on both).
clear   after the section's stamp commit. Refused until the COMMITTED TODO carries both stamps: a stamp that
        was written and whose commit the gate then refused has cleared nothing.
probe   exit 0 when the reviewer answers, 75 while it is down: scripts/overnight/codex-outage-check.py, the
        one definition of an outage.

Exit 2 on a bad argument, a refusal, or a record that cannot be read or written.
"""
import argparse
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import time

REPO = pathlib.Path(__file__).resolve().parents[2]
STATE = pathlib.Path(".claude") / "state" / "review-owed.json"
MANIFEST = pathlib.Path(".claude") / "overnight" / "reviews" / "manifest.jsonl"
OUTAGE_CHECK = pathlib.Path(__file__).resolve().parent / "codex-outage-check.py"
OUTAGE_EXIT = 75
VERIFIED, QUALITY = "> **Verified:**", "> **Quality reviewed:**"
GIT_TIMEOUT = 60

sys.path.insert(0, str(REPO / "scripts"))
import todo_fence  # noqa: E402  (the one rule for what a `## N.` heading is)


class Unreadable(Exception):
    """The record exists and cannot be trusted. Nothing may be assumed absent from it."""


def load(root):
    """The owed sections, oldest first: [{'todo', 'section', 'base', 'since'}]. [] when none was ever recorded."""
    path = pathlib.Path(root) / STATE
    try:
        data = json.loads(path.read_text())
    except FileNotFoundError:
        return []
    except (OSError, ValueError) as exc:
        raise Unreadable(f"{STATE} cannot be read: {exc}") from exc
    if not isinstance(data, list) or not all(
            isinstance(e, dict) and isinstance(e.get("todo"), str) and isinstance(e.get("section"), int)
            and isinstance(e.get("base"), str) and e["base"] for e in data):
        raise Unreadable(f"{STATE} is not a list of owed sections")
    return data


def save(root, records):
    path = pathlib.Path(root) / STATE
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=str(path.parent), prefix=".review-owed-")
    try:
        with os.fdopen(fd, "w") as fh:
            fh.write(json.dumps(records, indent=1) + "\n")
        os.replace(tmp, path)
    except BaseException:
        try:
            os.unlink(tmp)
        except FileNotFoundError:
            pass
        raise


REVIEW_STATE = pathlib.Path(".claude") / "state" / "last-codex-review.json"


def _module(name, path):
    import importlib.util
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def mark_failed_dispatch(root):
    """Mark the unreceived dispatch record as the confirmed outage's (see `add`), under the recorder's lock and
    only when that record's own log ended in a backend failure. A record another dispatch published meanwhile,
    or one with no single log, is left pending (it is then received by hand). Nothing to mark is fine."""
    path = pathlib.Path(root) / REVIEW_STATE
    if not path.exists():
        return
    crc = _module("codex_review_completed", REPO / ".claude" / "hooks" / "codex_review_completed.py")
    leg_state = _module("codex_outage_check", OUTAGE_CHECK).leg_state

    def _mark(state):
        if state.get("received") is True or state.get("outage_debt") is True:
            return None
        log = str(state.get("log_file") or "")
        if not log or leg_state(pathlib.Path(log))[0] != "outage":
            return None
        state["outage_debt"] = True
        return state

    crc._update_state(path, _mark)


def base_of(root, todo, section):
    """The recorded base of an owed section, or "" when it is not owed."""
    for e in load(root):
        if e["todo"] == todo and str(e["section"]) == str(section):
            return e["base"]
    return ""


def outage(root, now=None):
    """Whether the reviewer's backend is down, by the existing check over the reviews manifest."""
    cmd = [sys.executable, str(OUTAGE_CHECK), "--manifest", str(pathlib.Path(root) / MANIFEST)]
    if now is not None:
        cmd += ["--now", str(now)]
    done = subprocess.run(cmd, capture_output=True, text=True, timeout=GIT_TIMEOUT)
    if done.returncode not in (0, OUTAGE_EXIT):
        raise Unreadable(f"the outage check failed (exit {done.returncode}): {done.stderr.strip()[:200]}")
    return done.returncode == OUTAGE_EXIT, done.stdout.strip()


def stamps(root, todo, section, committed=False):
    """(the section exists, it carries a Verified stamp, it carries a Quality reviewed stamp).

    `committed` reads the TODO as HEAD records it: the stamp commit is what the review gate judged, so only a
    committed stamp is evidence that the review happened.
    """
    try:
        if committed:
            shown = git(root, "show", f"HEAD:{todo}")
            if shown.returncode != 0:
                return (False, False, False)
            text = shown.stdout
        else:
            text = (pathlib.Path(root) / todo).read_text(encoding="utf-8")
        scan = todo_fence.scan_text(text)
    except (OSError, subprocess.SubprocessError):
        return (False, False, False)
    here, found, verified, quality = None, False, False, False
    for line, masked in zip(scan.lines, scan.mask):
        if masked:
            continue
        if todo_fence.is_h2(line):
            here = todo_fence.classify_heading(line).n
            found = found or here == section
        elif here == section:
            verified = verified or line.startswith(VERIFIED)
            quality = quality or line.startswith(QUALITY)
    return (found, verified, quality)


def plan_head(root, todo, section):
    """The commit of the section's checked plan pass, or "" (read as review_pin.section_status reads it)."""
    try:
        lines = (pathlib.Path(root) / MANIFEST).read_text().splitlines()
    except (OSError, UnicodeDecodeError):
        return ""
    head = ""
    for line in lines:
        try:
            e = json.loads(line)
        except ValueError:
            continue
        if isinstance(e, dict) and e.get("todo") == todo and str(e.get("section", "")) == str(section) \
                and e.get("kind") == "plan" and e.get("pass_checked") is True and e.get("rc") == 0 \
                and isinstance(e.get("head"), str):
            head = e["head"]
    return head


def git(root, *args):
    return subprocess.run(["git", "-C", str(root), *args], capture_output=True, text=True, timeout=GIT_TIMEOUT)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--root", default=str(REPO), help=argparse.SUPPRESS)
    ap.add_argument("--now", type=float, help=argparse.SUPPRESS)
    sub = ap.add_subparsers(dest="verb", required=True)
    for verb in ("add", "clear"):
        p = sub.add_parser(verb)
        p.add_argument("--todo", required=True)
        p.add_argument("--section", type=int, required=True)
    for verb in ("list", "gate", "probe"):
        sub.add_parser(verb)
    args = ap.parse_args(argv)
    root = pathlib.Path(args.root)

    def refuse(message):
        print(f"review-owed: {message}", file=sys.stderr)
        return 2

    try:
        if args.verb == "probe":
            down, why = outage(root, args.now)
            if down:
                print(why)
            return OUTAGE_EXIT if down else 0
        records = load(root)
        if args.verb == "list":
            for e in records:
                print(f"{e['todo']} section {e['section']} base {e['base']}")
            return 0
        if args.verb == "gate":
            for e in records:
                print(f"review owed: {e['todo']} section {e['section']} (built while the reviewer was down, "
                      f"since {time.strftime('%Y-%m-%d %H:%M', time.localtime(e.get('since', 0)))})")
            return 1 if records else 0
        found, verified, quality = stamps(root, args.todo, args.section)
        if not found:
            return refuse(f"{args.todo} has no section {args.section}")
        listed = any(e["todo"] == args.todo and e["section"] == args.section for e in records)
        if args.verb == "add":
            if listed:
                # The first base stands. Asked again, the section's review was retried and failed again, or
                # the run stopped between the two writes below: when the outage is confirmed now, the
                # dispatch record that failed with it is marked as the first one was.
                down, _why = outage(root, args.now)
                if down:
                    mark_failed_dispatch(root)
                return 0
            if verified:
                return refuse(f"{args.todo} section {args.section} is already stamped; it owes no review")
            down, _why = outage(root, args.now)
            if not down:
                return refuse("the reviewer answers (the outage check reports no outage), so the section is "
                              "reviewed now: dispatch its plan pass")
            head = git(root, "rev-parse", "HEAD")
            if head.returncode != 0:
                return refuse("HEAD cannot be read")
            base = plan_head(root, args.todo, args.section) or head.stdout.strip()
            if git(root, "merge-base", "--is-ancestor", base, "HEAD").returncode != 0:
                return refuse(f"the section's plan pass was dispatched at {base[:10]}, which is not in this history")
            records.append({"todo": args.todo, "section": args.section, "base": base, "since": int(time.time())})
            save(root, records)
            mark_failed_dispatch(root)
            print(f"review owed: {args.todo} section {args.section} from {base[:10]}; build it, commit it, move on")
            return 0
        if not listed:
            return 0                    # nothing to clear
        _found, verified, quality = stamps(root, args.todo, args.section, committed=True)
        if not (verified and quality):
            return refuse(f"{args.todo} section {args.section} is not stamped in HEAD yet: its review is still "
                          "owed (a stamp counts once its commit has landed)")
        save(root, [e for e in records if not (e["todo"] == args.todo and e["section"] == args.section)])
        return 0
    except Unreadable as exc:
        return refuse(str(exc))
    except OSError as exc:
        return refuse(f"{STATE} cannot be written: {exc}")


if __name__ == "__main__":
    sys.exit(main())
