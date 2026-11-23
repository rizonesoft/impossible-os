#!/usr/bin/env python3
"""What Impossible OS imports from the repositories beside it (todo/imports.json).

    python3 scripts/imports.py --check    lint Check 42: judges the git index
    python3 scripts/imports.py --list     every source and where it is imported
    python3 scripts/imports.py --pull <source> [--to <commit>]
                                          bring the copy to that commit (default: its main)
    python3 scripts/imports.py --propose <source> --title <text>
                                          send the staged change to its repository as a pull request

A source is a part or a program developed in its own repository (Impossible SDK,
Calculator and so on). One roadmap section imports it and carries the line
`**Import:** <source>`. A section that is built elsewhere keeps its address,
carries `**Moved:** <source>` and has a row in the ledger. The check refuses:

- one of the three without the others;
- a copy that is neither the pinned one nor the one that was proposed: its
  identity is git's tree id, which is the same in the repository it came from,
  so names, bytes, modes and links count. With IMPOSSIBLE_REPOS naming the
  folder of those repositories, the pinned commit is asked too;
- an import section marked done with nothing imported;
- an entry that ever left the ledger, in its committed history or now, without
  its own reason under "removed". A shallow clone is judged on what it has.

It cannot see work that is described as moved in prose and never declared: the
staged TODO checks guard the checklist items themselves. Decision:
todo/answers.md Q56 and Q57.
"""
import argparse
import hashlib
import json
import os
import re
import secrets
import shutil
import subprocess
import sys
import tempfile
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
SOURCE = {"repository": str, "from": list, "kind": str, "path": str, "section": str, "commit": (str, type(None)), "tree": (str, type(None)),
          "open": (dict, type(None))}
OPEN = {"tree": str, "proposal": str, "head": str}
FOLDER = re.compile(r"src(/[a-z0-9][a-z0-9-]*)+")
NAME = re.compile(r"[A-Za-z0-9_][A-Za-z0-9._-]*")
REMOTE = "https://github.com/rizonesoft"
ROW = {"section": str, "source": str, "to": str}
GONE = {"name": str, "reason": str}
EMPTY = {"schema": 1, "sources": {}, "moved": [], "removed": []}


def git(root, *arguments, check=True, **more):
    return subprocess.run(["git", "-C", str(root), *arguments], capture_output=True, check=check, timeout=120, **more)


def typed(value, shape, where):
    if not isinstance(value, dict) or set(value) != set(shape):
        return [f"{where}: needs exactly {', '.join(sorted(shape))}"]
    return [f"{where}: {key} has the wrong type or is empty" for key, kind in shape.items()
            if not isinstance(value[key], kind) or (isinstance(value[key], str) and not value[key].strip())]


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
        wrong = typed(source, SOURCE, f"source {name}")
        if not wrong:
            listed = source["from"]
            if not listed or not all(isinstance(n, str) and NAME.fullmatch(n) for n in listed) or len(set(listed)) != len(listed):
                wrong.append(f"source {name}: from lists files and folders at the top of its repository, each once")
            if not FOLDER.fullmatch(source["path"]):
                wrong.append(f"source {name}: {source['path']} is not a folder under src/")
            if not NAME.fullmatch(source["repository"]):
                wrong.append(f"source {name}: {source['repository']} is not a repository's name")
            if source["open"] is not None:
                shape = typed(source["open"], OPEN, f"source {name}: open")
                if not shape and not re.fullmatch(re.escape(f"{REMOTE}/{source['repository']}/pull/") + r"[1-9]\d*", source["open"]["proposal"]):
                    shape.append(f"source {name}: the proposal is not a pull request of {source['repository']}")
                wrong += shape
        faults += wrong
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


def picked(root, commit, names, **more):
    """git's id for a tree of these entries of a commit's top, or None when one is not there."""
    listed = git(root, "ls-tree", "-z", "--full-tree", commit, check=False, **more)
    found = [line.split(b"\t", 1) for line in listed.stdout.split(b"\0") if line] if listed.returncode == 0 else []
    chosen = [(head.split(), name) for head, name in found if name.decode("utf-8", "replace") in names]
    if len(chosen) != len(names):
        return None
    chosen.sort(key=lambda entry: entry[1] + (b"/" if entry[0][1] == b"tree" else b""))  # git's own order
    body = b"".join(mode.lstrip(b"0") + b" " + name + b"\0" + bytes.fromhex(sha.decode()) for (mode, _, sha), name in chosen)
    return hashlib.sha1(b"tree %d\0" % len(body) + body).hexdigest()


def upstream(source):
    """The tree id the pinned commit has; None when nobody said where to ask."""
    folder = os.environ.get("IMPOSSIBLE_REPOS")
    if not folder:
        return None
    if not (Path(folder) / source["repository"]).is_dir():
        return "of a repository that is not there"
    def ask(what):
        asked = git(Path(folder) / source["repository"], "rev-parse", "--verify", "-q", what, check=False, env=clean())
        return asked.stdout.decode().strip() if asked.returncode == 0 else "unknown"

    if ask(source["commit"] + "^{commit}") != source["commit"]:  # a tree or a tag has files too
        return "of something that is not a commit"
    return picked(Path(folder) / source["repository"], source["commit"], source["from"], env=clean()) or "unknown"


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
        copy = copy_of(root, tree, path)
        if source["commit"] is None:
            if copy or source["tree"] is not None:
                faults.append(f"{where}: {path} is not imported yet, but it has files or a tree id")
        elif not HASH.fullmatch(source["commit"]) or not HASH.fullmatch(source["tree"] or ""):
            faults.append(f"{where}: the commit and the tree are not full hashes")
        elif copy is None:
            faults.append(f"{where}: pinned at {source['commit'][:12]}, but {path} has no files")
        elif source["open"] is None and copy != source["tree"]:
            faults.append(f"{where}: {path} is not the pinned copy (its tree is {copy}): propose the change with --propose {name}")
        elif source["open"] is not None and copy != source["open"]["tree"]:
            faults.append(f"{where}: {path} is not the proposed copy (its tree is {copy}): propose it again with --propose {name}")
        elif source["open"] is not None and copy == source["tree"]:
            faults.append(f"{where}: {path} is the pinned copy, so nothing is open")
        elif (theirs := upstream(source)) not in (None, source["tree"]):
            faults.append(f"{where}: {source['repository']} at {source['commit'][:12]} has the tree {theirs}, and the ledger has {source['tree']}")
        if source["open"] is not None and source["commit"] is None:
            faults.append(f"{where}: a change is open and nothing was imported")
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


def clean():
    """A hook's git variables name this repository and not another one."""
    return {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}


def outside(*command, data=None, **more):
    """A command about another repository; its text answer."""
    done = subprocess.run([str(c) for c in command], input=data, capture_output=True, timeout=600, env={**clean(), **more.pop("env", {})}, **more)
    if done.returncode:
        raise SystemExit(f"imports: {command[0]} {command[1]}: {(done.stderr or done.stdout).decode(errors='replace').strip()}")
    return done.stdout.decode().strip()


def cloned(source, folder):
    there = Path(folder, source["repository"])
    outside("git", "clone", "-q", f"{os.environ.get('IMPOSSIBLE_REMOTE', REMOTE)}/{source['repository']}.git", there)
    return there


def files(root, tree, names, **more):
    """[(mode, blob, path)] of these entries of a tree: git's own objects, so no attribute or filter can change or leave out one."""
    listed = git(root, "ls-tree", "-r", "-z", "--full-tree", tree, **more).stdout.split(b"\0")
    found = [(head.split()[0].decode(), head.split()[2].decode(), path) for head, path in (line.split(b"\t", 1) for line in listed if line)]
    if any(mode == "160000" for mode, _, _ in found):
        raise SystemExit("imports: a submodule cannot be imported")
    return [entry for entry in found if entry[2].split(b"/")[0].decode("utf-8", "replace") in names]


def holds(proposal, head):
    """(the pull request, whether it is this repository's own and holds that commit)."""
    seen = json.loads(outside("gh", "pr", "view", proposal, "--json", "state,mergeCommit,isCrossRepository,commits"))
    return seen, not seen["isCrossRepository"] and any(c["oid"] == head for c in seen["commits"])


def staged(root, name):
    """(ledger, source, tree id of the staged copy); what is not staged would be lost or left out."""
    shown = git(root, "show", f":{LEDGER}", check=False)
    ledger, faults = load(shown.stdout) if shown.returncode == 0 else (None, [f"{LEDGER} is not there"])
    if faults or name not in ledger["sources"]:
        raise SystemExit("\n".join(f"imports: {f}" for f in faults or [f"there is no source {name}"]))
    source = ledger["sources"][name]
    for path in (source["path"], LEDGER):
        # A lower-case letter or S: git was told not to look at the file, so it cannot say that nothing changed.
        unseen = any(line[:1] == b"S" or line[:1].islower() for line in git(root, "ls-files", "-v", "-z", "--", path).stdout.split(b"\0"))
        if unseen or git(root, "diff", "--name-only", "--", path).stdout or git(root, "ls-files", "-o", "--", path).stdout:
            raise SystemExit(f"imports: {path} has changes that are not staged")
    return ledger, source, copy_of(root, git(root, "write-tree").stdout.decode().strip(), source["path"])


def written(ledger):
    return (json.dumps(ledger, indent=2, ensure_ascii=False) + "\n").encode()


def blob(root, content):
    return git(root, "hash-object", "-w", "--stdin", input=content).stdout.decode().strip()


def index(root, lines):
    git(root, "update-index", "-z", "--index-info", input=b"\0".join(lines) + b"\0")  # one step: all of it or none


def keep(root, ledger, lines=()):
    """The ledger, and what belongs to it, in the index in one step; then the file."""
    index(root, [*lines, f"100644 {blob(root, written(ledger))}\t{LEDGER}".encode()])
    (root / LEDGER).write_bytes(written(ledger))


def pull(root, name, to):
    ledger, source, copy = staged(root, name)
    known = source["open"]["tree"] if source["open"] else source["tree"]
    if copy != known:
        raise SystemExit(f"imports: {source['path']} has a change that was not proposed")
    with tempfile.TemporaryDirectory() as folder:
        there = cloned(source, folder)
        commit = outside("git", "-C", there, "rev-parse", "--verify", (to or "origin/main") + "^{commit}")
        outside("git", "-C", there, "merge-base", "--is-ancestor", commit, "origin/main")
        if source["open"]:
            proposal = source["open"]["proposal"]
            state, ours = holds(proposal, source["open"]["head"])
            if not ours:
                raise SystemExit(f"imports: {proposal} does not hold this change")
            if state["state"] != "MERGED":
                raise SystemExit(f"imports: {proposal} is {state['state'].lower()}, and pulling now would drop its change")
            outside("git", "-C", there, "merge-base", "--is-ancestor", state["mergeCommit"]["oid"], commit)
        tree = picked(there, commit, source["from"], env=clean())
        if tree is None:
            raise SystemExit(f"imports: {source['repository']} at {commit[:12]} does not have all of {', '.join(source['from'])}")
        arriving = files(there, commit, source["from"], env=clean())
        fresh = Path(tempfile.mkdtemp(dir=(root / "todo"), prefix=".import-"))  # beside the copy, so that it can be renamed
        try:
            for mode, sha, path in arriving:
                content = git(there, "cat-file", "blob", sha, env=clean()).stdout
                if blob(root, content) != sha:
                    raise SystemExit("imports: the two repositories name their files differently")
                target = fresh / "copy" / os.fsdecode(path)
                target.parent.mkdir(parents=True, exist_ok=True)
                if mode == "120000":
                    target.symlink_to(os.fsdecode(content))
                else:
                    target.write_bytes(content)  # as it is there: no filter of this repository
                    target.chmod(0o755 if mode == "100755" else 0o644)
            prefix = os.fsencode(source["path"]) + b"/"
            leaving = [p for p in git(root, "ls-files", "-z", "--", source["path"]).stdout.split(b"\0") if p]
            lines = [b"0 " + b"0" * 40 + b"\t" + p for p in leaving]
            lines += [f"{mode} {sha}\t".encode() + prefix + path for mode, sha, path in arriving]
            source.update(commit=commit, tree=tree, open=None)
            target = root / source["path"]
            target.parent.mkdir(parents=True, exist_ok=True)
            if target.exists():
                target.rename(fresh / "before")
            (fresh / "copy").rename(target)
            try:
                keep(root, ledger, lines)
            except BaseException:  # the index is as it was, so the files are too
                target.rename(fresh / "copy")
                if (fresh / "before").exists():
                    (fresh / "before").rename(target)
                raise
        finally:
            shutil.rmtree(fresh)
    print(f"{name}: {source['repository']} at {commit[:12]}, staged")


def propose(root, name, title):
    ledger, source, copy = staged(root, name)
    before = source["open"]
    if source["commit"] is None or not HASH.fullmatch(source["tree"] or "") or copy is None or copy in (source["tree"], before and before["tree"]):
        raise SystemExit(f"imports: {name} has no new change to propose")
    top = {n for n in git(root, "ls-tree", "--name-only", "-z", copy).stdout.decode().split("\0") if n}
    if top != set(source["from"]):
        raise SystemExit(f"imports: {', '.join(sorted(top ^ set(source['from'])))} is in the copy or under from, and not in both")
    with tempfile.TemporaryDirectory() as folder:
        # A name of its own for every try: a branch that is there may hold something else.
        there, branch = cloned(source, folder), f"impossible-os/{name}-{copy[:12]}-{secrets.token_hex(4)}"
        # Reads this repository's objects beside its own, for as long as the branch is made and pushed.
        objects = Path(root, git(root, "rev-parse", "--git-path", "objects").stdout.decode().strip()).resolve()
        here = {"GIT_ALTERNATE_OBJECT_DIRECTORIES": str(objects)}
        outside("git", "-C", there, "checkout", "-q", "-b", branch, source["commit"])
        lines = [f"{mode} {sha}\t".encode() + path for mode, sha, path in files(root, copy, top)]
        outside("git", "-C", there, "rm", "-r", "-q", "--cached", "--", *source["from"])
        outside("git", "-C", there, "update-index", "-z", "--index-info", data=b"\0".join(lines) + b"\0", env=here)
        outside("git", "-C", there, "commit", "-q", "-m", title, env=here)
        outside("git", "-C", there, "reset", "-q", "--hard", env=here)
        outside("git", "-C", there, "rebase", "-q", "origin/main", env=here)  # a conflict stops here: pull what is new first
        head = outside("git", "-C", there, "rev-parse", "HEAD")
        outside("git", "-C", there, "push", "-q", "origin", branch, env=here)
        proposal = outside("gh", "pr", "create", "--repo", f"rizonesoft/{source['repository']}", "--head", branch, "--base", "main", "--title", title,
                           "--body", f"Changed in Impossible OS ({source['path']}), from {source['commit'][:12]}.", cwd=there).splitlines()[-1]
        source["open"] = {"tree": copy, "proposal": proposal, "head": head}
        keep(root, ledger)
        if before:
            try:
                if not holds(before["proposal"], before["head"])[1]:
                    raise SystemExit("it does not hold the earlier change")
                outside("gh", "pr", "close", before["proposal"], "--comment", f"Replaced by {proposal}", cwd=there)
            except SystemExit as error:
                print(f"{name}: {before['proposal']} is still open and is replaced ({error})", file=sys.stderr)
    print(f"{name}: {proposal}, recorded and staged")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--pull", metavar="SOURCE")
    parser.add_argument("--to", metavar="COMMIT")
    parser.add_argument("--propose", metavar="SOURCE")
    parser.add_argument("--title")
    args = parser.parse_args()
    if bool(args.propose) != bool(args.title) or (args.to and not args.pull):
        parser.error("--propose needs --title, and --to belongs to --pull")
    faults = []
    if args.pull:
        pull(args.root, args.pull, args.to)
    elif args.propose:
        propose(args.root, args.propose, args.title)
    elif args.list:
        ledger, faults = load((args.root / LEDGER).read_bytes())
        for name, source in (ledger or {"sources": {}})["sources"].items():
            print(f"{name}: {source['repository']} at {(source['commit'] or 'not imported')[:12]}, {source['section']}"
                  + (f", open: {source['open']['proposal']}" if source["open"] else ""))
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
