#!/usr/bin/env python3
"""What the second reviewer is worth, and which sections it still owes a read.

Owner: todo/00-infrastructure/TODO-08-automation-hardening.md section 44.

The second reviewer was wired on real sections rather than replayed first (operator decision 2026-11-14), so
its record on those sections is the evidence for keeping it, narrowing its paths or dropping it.

    python3 scripts/overnight/second-review-log.py record --todo todo/<path>.md --section N \\
        --finding 'fixed: <what it found>' --finding 'rejected: <what it found> | <the code evidence>' \\
        --finding 'filed: <what it found> | <the tracked item that owns it>'
    python3 scripts/overnight/second-review-log.py stats [--findings]
    python3 scripts/overnight/second-review-log.py unavailable

record       one line per section, once its second review is finished, holding every finding it raised and
             what became of it: `fixed`, `rejected` (with the code evidence after ` | `) or `filed` (with the
             tracked item after ` | `). No --finding records a section it found nothing in. The totals are
             worked out from the findings: real is fixed plus filed. A section recorded twice counts once, as
             its latest line.
stats        per section and in total: raised, real, rejected; and how many sections of the ten are in.
             --findings also prints each finding, so the judgments behind the totals can be checked.
unavailable  every stamp under todo/ that says `second: unavailable`, as `<todo> section <N>`: the sections to
             read when the reviewer answers again. Exit 1 when there is one, 0 when none.

Exit 2 on a bad argument or a log that cannot be read or written.
"""
import argparse
import json
import pathlib
import sys
import time

REPO = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))
import todo_fence  # noqa: E402  (the one rule for what a `## N.` heading is, and what is inside a fence)
LOG = pathlib.Path(".claude") / "overnight" / "second-review-log.jsonl"
MARK = "second: unavailable"
STAMP = "> **Quality reviewed:**"      # the line the stamp gate accepts the mark on (section_commit_gate.py)
TRIAL = 10
VERDICTS = ("fixed", "rejected", "filed")
NEEDS_NOTE = {"rejected": "the code evidence", "filed": "the tracked item that owns it"}


def parse_finding(text):
    """{'verdict', 'finding', 'note'} from `verdict: finding | note`, or a message saying what is wrong."""
    verdict, sep, rest = text.partition(":")
    verdict = verdict.strip()
    if not sep or verdict not in VERDICTS:
        return f"a finding starts with {', '.join(VERDICTS[:-1])} or {VERDICTS[-1]} and a colon: {text!r}"
    finding, _, note = (part.strip() for part in rest.partition("|"))
    if not finding:
        return f"a finding says what was found: {text!r}"
    if verdict in NEEDS_NOTE and not note:
        return f"a {verdict} finding gives {NEEDS_NOTE[verdict]} after ` | `: {text!r}"
    return {"verdict": verdict, "finding": finding, "note": note}


def totals(record):
    found = record["findings"]
    rejected = sum(f["verdict"] == "rejected" for f in found)
    return {"raised": len(found), "real": len(found) - rejected, "rejected": rejected}


def load(path):
    """{(todo, section): latest line}, in first-recorded order."""
    out = {}
    try:
        lines = path.read_text().splitlines()
    except FileNotFoundError:
        return out
    for n, line in enumerate(lines, 1):
        if not line.strip():
            continue
        e = json.loads(line)        # a line that is not JSON is a damaged record: say so, do not skip it
        if not isinstance(e, dict) or not isinstance(e.get("section"), int) or not isinstance(e.get("findings"), list) \
                or not all(isinstance(f, dict) and f.get("verdict") in VERDICTS and isinstance(f.get("finding"), str)
                           for f in e["findings"]):
            raise ValueError(f"line {n} is not a section record")
        out[(e.get("todo"), e["section"])] = e
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="verb", required=True)
    rec = sub.add_parser("record")
    rec.add_argument("--todo", required=True)
    rec.add_argument("--section", type=int, required=True)
    rec.add_argument("--finding", action="append", default=[])
    sub.add_parser("stats").add_argument("--findings", action="store_true")
    sub.add_parser("unavailable")
    ap.add_argument("--root", default=str(REPO), help=argparse.SUPPRESS)
    args = ap.parse_args(argv)
    root = pathlib.Path(args.root)
    log = root / LOG

    if args.verb == "unavailable":
        owed = []
        for path in sorted((root / "todo").rglob("*.md")):
            scan = todo_fence.scan_text(path.read_text(encoding="utf-8", errors="replace"))
            section = None
            for line, masked in zip(scan.lines, scan.mask):
                if masked:
                    continue        # inside a fence or a comment: an example, not a heading or a stamp
                if todo_fence.is_h2(line):
                    section = todo_fence.classify_heading(line).n      # None for a heading with no number
                elif MARK in line and line.startswith(STAMP) and section is not None:
                    owed.append(f"{path.relative_to(root).as_posix()} section {section}")
        print("\n".join(owed))
        return 1 if owed else 0

    try:
        records = load(log)
    except (OSError, ValueError) as exc:
        print(f"second-review-log: {log} cannot be read: {exc}", file=sys.stderr)
        return 2

    if args.verb == "record":
        if not (root / args.todo).is_file():
            print(f"second-review-log: no such TODO: {args.todo}", file=sys.stderr)
            return 2
        found = [parse_finding(text) for text in args.finding]
        wrong = [f for f in found if isinstance(f, str)]
        if wrong:
            print("second-review-log: " + "; ".join(wrong), file=sys.stderr)
            return 2
        line = {"ts": int(time.time()), "todo": args.todo, "section": args.section, "findings": found}
        try:
            log.parent.mkdir(parents=True, exist_ok=True)
            with log.open("a") as fh:
                fh.write(json.dumps(line) + "\n")
        except OSError as exc:
            print(f"second-review-log: {log} cannot be written: {exc}", file=sys.stderr)
            return 2
        records[(args.todo, args.section)] = line

    total = {"raised": 0, "real": 0, "rejected": 0}
    for (todo, section), e in records.items():
        counts = totals(e)
        print(f"{todo} section {section}: raised {counts['raised']}, real {counts['real']}, rejected {counts['rejected']}")
        if getattr(args, "findings", False):
            for f in e["findings"]:
                print(f"  {f['verdict']}: {f['finding']}" + (f" | {f['note']}" if f["note"] else ""))
        for key in total:
            total[key] += counts[key]
    done = len(records)
    verdict = "decide: keep, narrow or drop" if done >= TRIAL else f"{TRIAL - done} more before deciding"
    print(f"second reviewer: {done} of {TRIAL} sections, raised {total['raised']}, real {total['real']}, "
          f"rejected {total['rejected']} ({verdict})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
