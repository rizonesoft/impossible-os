#!/usr/bin/env python3
"""What the second reviewer is worth, and which sections it still owes a read.

Owner: todo/00-infrastructure/TODO-08-automation-hardening.md section 44.

The second reviewer was wired on real sections rather than replayed first (operator decision 2026-11-14), so
its record on those sections is the evidence for keeping it, narrowing its paths or dropping it.

    python3 scripts/overnight/second-review-log.py record --todo todo/<path>.md --section N --raised A --real B --rejected C
    python3 scripts/overnight/second-review-log.py stats
    python3 scripts/overnight/second-review-log.py unavailable

record       one line per section, once its second review is finished: the findings it raised, how many were
             real (fixed, or filed as a tracked item) and how many were rejected with code evidence. real plus
             rejected must equal raised. A section recorded twice counts once, as its latest line.
stats        per section and in total: raised, real, rejected; and how many sections of the ten are in.
unavailable  every stamp under todo/ that says `second: unavailable`, as `<todo> section <N>`: the sections to
             read when the reviewer answers again. Exit 1 when there is one, 0 when none.

Exit 2 on a bad argument or a log that cannot be read or written.
"""
import argparse
import json
import pathlib
import re
import sys
import time

REPO = pathlib.Path(__file__).resolve().parents[2]
LOG = pathlib.Path(".claude") / "overnight" / "second-review-log.jsonl"
MARK = "second: unavailable"
STAMP = "> **Quality reviewed:**"      # the line the stamp gate accepts the mark on (section_commit_gate.py)
HEADING = re.compile(r"^## (\d+)\.")
TRIAL = 10


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
        if not isinstance(e, dict) or not all(isinstance(e.get(k), int) for k in ("section", "raised", "real", "rejected")):
            raise ValueError(f"line {n} is not a section record")
        out[(e.get("todo"), e["section"])] = e
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="verb", required=True)
    rec = sub.add_parser("record")
    rec.add_argument("--todo", required=True)
    rec.add_argument("--section", type=int, required=True)
    for name in ("raised", "real", "rejected"):
        rec.add_argument(f"--{name}", type=int, required=True)
    sub.add_parser("stats")
    sub.add_parser("unavailable")
    ap.add_argument("--root", default=str(REPO), help=argparse.SUPPRESS)
    args = ap.parse_args(argv)
    root = pathlib.Path(args.root)
    log = root / LOG

    if args.verb == "unavailable":
        owed = []
        for path in sorted((root / "todo").rglob("*.md")):
            section = None
            for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
                m = HEADING.match(line)
                if m:
                    section = m.group(1)
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
        if min(args.raised, args.real, args.rejected) < 0 or args.real + args.rejected != args.raised:
            print("second-review-log: real plus rejected must equal raised, none below 0", file=sys.stderr)
            return 2
        line = {"ts": int(time.time()), "todo": args.todo, "section": args.section, "raised": args.raised,
                "real": args.real, "rejected": args.rejected}
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
        print(f"{todo} section {section}: raised {e['raised']}, real {e['real']}, rejected {e['rejected']}")
        for key in total:
            total[key] += e[key]
    done = len(records)
    verdict = "decide: keep, narrow or drop" if done >= TRIAL else f"{TRIAL - done} more before deciding"
    print(f"second reviewer: {done} of {TRIAL} sections, raised {total['raised']}, real {total['real']}, "
          f"rejected {total['rejected']} ({verdict})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
