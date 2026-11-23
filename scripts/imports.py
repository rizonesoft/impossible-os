#!/usr/bin/env python3
"""What Impossible OS imports from the repositories beside it (todo/imports.json).

    python3 scripts/imports.py --check    lint Check 42: judges the git index
    python3 scripts/imports.py --list     every source and where it is imported

A source is a part or a program developed in its own repository (Impossible SDK,
Calculator and so on). One roadmap section imports it and carries the line
`**Import:** <source>`. A section that is built elsewhere keeps its address,
carries `**Moved:** <source>` and has a row in the ledger. The check refuses:

- one of the three without the others;
- a copy that is not the pinned one: its identity is git's tree id, which is the
  same in the repository it came from, so names, bytes, modes and links count.
  With IMPOSSIBLE_REPOS naming the folder of those repositories, the pinned
  commit is asked too;
- an import section marked done with nothing imported;
- an entry that ever left the ledger, in its committed history or now, without
  its own reason under "removed". A shallow clone is judged on what it has.

It cannot see work that is described as moved in prose and never declared: the
staged TODO checks guard the checklist items themselves. Decision:
todo/answers.md Q56.
"""
import argparse
import json
import os
import re
import subprocess
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import todo_fence

LEDGER = "todo/imports.json"
PLAN = "todo/implementation-plan.md"
SIGN = "§"
MARK = re.compile(r"^\*\*(Import|Moved):\*\*[ \t]*(.*?)[ \t]*$")  # any name: a wrong one has no row and is refused
ADDRESS = re.compile(r"^(todo/(\d\d)-[a-z0-9-]+/TODO-(\d\d)-[a-z0-9-]+\.md) " + SIGN + r"(\d{1,5})$")
HASH = re.compile(r"[0-9a-f]{40}")
SOURCE = {"repository": str, "from": str, "kind": str, "path": str, "section": str, "commit": (str, type(None)), "tree": (str, type(None))}
ROW = {"section": str, "source": str, "to": str}
GONE = {"name": str, "reason": str}
EMPTY = {"schema": 1, "sources": {}, "moved": [], "removed": []}


def git(root, *arguments, check=True, **more):
    return subprocess.run(["git", "-C", str(root), *arguments], capture_output=True, check=check, timeout=120, **more)


def typed(value, shape, where):
    if not isinstance(value, dict) or set(value) != set(shape):
        return [f"{where}: needs exactly {', '.join(sorted(shape))}"]
    return [f"{where}: {key} has the wrong type or is empty" for key, kind in shape.items()
            if not isinstance(value[key], kind) or (isinstance(value[key], str) and not value[key].strip() and key != "from")]


def load(data):
    """(ledger, faults): never a ledger with a fault in its shape."""
    try:
        ledger = json.loads(data)
    except (ValueError, UnicodeDecodeError, RecursionError) as error:
        return None, [f"{LEDGER}: not JSON ({error})"]
    if not isinstance(ledger, dict) or set(ledger) != {"schema", "sources", "moved", "removed"} or ledger["schema"] != 1:
        return None, [f"{LEDGER}: needs schema 1, sources, moved and removed"]
    if not isinstance(ledger["sources"], dict) or not isinstance(ledger["moved"], list) or not isinstance(ledger["removed"], list):
        return None, [f"{LEDGER}: sources is an object, moved and removed are lists"]
    faults = []
    for name, source in ledger["sources"].items():
        if not re.fullmatch(r"[a-z0-9-]+", name):
            faults.append(f"source {name}: a name is small letters, digits and dashes")
        faults += typed(source, SOURCE, f"source {name}")
    for key, shape in (("moved", ROW), ("removed", GONE)):
        for number, row in enumerate(ledger[key], 1):
            faults += typed(row, shape, f"{key} row {number}")
    return (None, faults) if faults else (ledger, [])


def entries(ledger):
    return {f"source {n}" for n in ledger["sources"]} | {f"moved {r['section']}" for r in ledger["moved"]}


def history(root):
    """Every ledger that was committed on this line of history, oldest first. A
    commit that deleted it had an empty one; one that cannot be read is None."""
    if git(root, "rev-parse", "--verify", "-q", "HEAD", check=False).returncode:
        return []
    commits = git(root, "log", "--first-parent", "--reverse", "--format=%H", "--", LEDGER).stdout.decode().split()
    ledgers = []
    for commit in commits:
        shown = git(root, "show", f"{commit}:{LEDGER}", check=False)
        ledgers.append(EMPTY if shown.returncode else load(shown.stdout)[0])
    return ledgers


def lost(past, now):
    """Faults for entries that left without a reason of their own."""
    faults, left, reasons, last = [], Counter(), Counter(), set()
    for ledger in [*[p for p in past if p is not None], now]:
        here = entries(ledger)
        left.update(last - here)
        last = here
        reasons |= Counter((r["name"], r["reason"]) for r in ledger["removed"])
    kept = Counter((r["name"], r["reason"]) for r in now["removed"])
    if reasons - kept:
        faults.append("a reason under removed is kept for good")
    given = Counter(r["name"] for r in now["removed"])
    for name, times in sorted(left.items()):
        if given[name] < times:
            faults.append(f"{name} left {LEDGER} {times} time(s) and has {given[name]} reason(s) under removed")
    return faults


def sections(text):
    """{number: {"Import": [names], "Moved": [names]}} as the roadmap's own parser sees the file."""
    found, current = {}, None
    for line in todo_fence.mask_text(text).split("\n"):
        heading = todo_fence.classify_heading(line)
        if heading.kind == "ok":
            current = found.setdefault(heading.n, {"Import": [], "Moved": []})
        elif todo_fence.is_h2(line):
            current = None
        elif current is not None and (mark := MARK.match(line)):
            current[mark[1]].append(mark[2])
    return found


def copy_of(root, tree, path):
    """git's tree id for a folder, or None when it has no files."""
    asked = git(root, "rev-parse", "--verify", "-q", f"{tree}:{path}", check=False)
    return asked.stdout.decode().strip() if asked.returncode == 0 else None


def upstream(source):
    """The tree id the pinned commit has; None when nobody said where to ask."""
    folder = os.environ.get("IMPOSSIBLE_REPOS")
    if not folder:
        return None
    if not (Path(folder) / source["repository"]).is_dir():
        return "of a repository that is not there"
    # The caller's git variables name this repository and not that one.
    clean = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}
    def ask(what):
        asked = git(Path(folder) / source["repository"], "rev-parse", "--verify", "-q", what, check=False, env=clean)
        return asked.stdout.decode().strip() if asked.returncode == 0 else "unknown"

    if ask(source["commit"] + "^{commit}") != source["commit"]:  # a tree or a tag has files too
        return "of something that is not a commit"
    return ask(f"{source['commit']}:{source['from']}")


def check(root):
    docs, tree = todo_fence.staged_docs(root)  # everything below is read from this one tree
    past = history(root)
    staged = git(root, "show", f"{tree}:{LEDGER}", check=False)
    if staged.returncode:
        return [f"{LEDGER} is missing"] if past else []
    ledger, faults = load(staged.stdout)
    if faults:
        return faults
    faults += lost(past, ledger)

    parsed, marks = {}, {"Import": {}, "Moved": {}}
    for path, text in docs.items():
        if not re.fullmatch(r"todo/\d\d-[^/]+/TODO-[^/]+\.md", path):
            continue
        parsed[path] = sections(text)
        for number, section in parsed[path].items():
            for kind, found in section.items():
                if found:
                    marks[kind][f"{path} {SIGN}{number}"] = found

    def exists(address):
        match = ADDRESS.match(address)
        return bool(match) and int(match[4]) in parsed.get(match[1], {})

    def done(address):
        match = ADDRESS.match(address)
        row = re.compile(rf"^\|[ \t]*\[x\][ \t]*\|[ \t]*`D{match[2]} T{match[3]} {SIGN}{match[4]}`[ \t]*\|", re.M)
        return bool(row.search(docs.get(PLAN, "")))

    for name, source in ledger["sources"].items():
        where, at, path = f"source {name}", source["section"], source["path"]
        if not exists(at):
            faults.append(f"{where}: {at} is not a roadmap section")
        elif marks["Import"].get(at) != [name]:
            faults.append(f"{where}: {at} does not carry the one line **Import:** {name}")
        elif source["commit"] is None and done(at):
            faults.append(f"{where}: {at} is done, and nothing was imported")
        if not re.fullmatch(r"src(/[a-z0-9][a-z0-9-]*)+", path):
            faults.append(f"{where}: {path} is not a folder under src/")
            continue
        copy = copy_of(root, tree, path)
        if source["commit"] is None:
            if copy or source["tree"] is not None:
                faults.append(f"{where}: {path} is not imported yet, but it has files or a tree id")
        elif not HASH.fullmatch(source["commit"]):
            faults.append(f"{where}: the commit is not a full hash")
        elif copy is None:
            faults.append(f"{where}: pinned at {source['commit'][:12]}, but {path} has no files")
        elif copy != source["tree"]:
            faults.append(f"{where}: {path} is not the pinned copy (its tree is {copy})")
        elif (theirs := upstream(source)) not in (None, copy):
            faults.append(f"{where}: {source['repository']} at {source['commit'][:12]} has the tree {theirs}, and the copy is {copy}")
    for at, found in marks["Import"].items():
        for name in found:
            if ledger["sources"].get(name, {}).get("section") != at:
                faults.append(f"{at}: **Import:** {name} has no source with this section")

    rows = Counter(row["section"] for row in ledger["moved"])
    faults += [f"{at}: moved twice" for at, times in rows.items() if times > 1]
    for row in ledger["moved"]:
        at = row["section"]
        if row["source"] not in ledger["sources"]:
            faults.append(f"{at}: there is no source {row['source']}")
        if not exists(at):
            faults.append(f"{at}: a moved section keeps its address, and this one is gone")
        elif marks["Moved"].get(at) != [row["source"]]:
            faults.append(f"{at}: does not carry the one line **Moved:** {row['source']}")
    faults += [f"{at}: **Moved:** {found[0]} has no row in {LEDGER}" for at, found in marks["Moved"].items() if at not in rows]
    return faults


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    if args.list:
        ledger, faults = load((args.root / LEDGER).read_bytes())
        for name, source in (ledger or {"sources": {}})["sources"].items():
            print(f"{name}: {source['repository']} at {(source['commit'] or 'not imported')[:12]}, {source['section']}")
    else:
        faults = check(args.root)
    for fault in faults:
        print(f"imports: {fault}")
    return 1 if faults else 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, UnicodeError, subprocess.SubprocessError, todo_fence.StagedSnapshotError) as error:
        print(f"imports: the tree cannot be read ({error})")
        raise SystemExit(2)
