#!/usr/bin/env python3
"""What Impossible OS imports from the repositories beside it (todo/imports.json).

    python3 scripts/imports.py --check    lint Check 42: judges the git index
    python3 scripts/imports.py --list     every source and where it is imported
    python3 scripts/imports.py --needs    what the programs beside this repository need and no section builds yet
    python3 scripts/imports.py --file-needs   write a section for each of them, and stage it
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
- a need without its section: what a program needs of this OS is a row under
  "needs" and the line `**Need:** <id>` in the section that builds it;
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
NEEDS = "todo/10-platform-services/TODO-13-program-needs.md"
PLAN = "todo/implementation-plan.md"
SIGN = "§"
MARK = re.compile(r"^\*\*(Import|Moved|Need):\*\*[ \t]*(.*?)[ \t]*$")  # any name: a wrong one has no row and is refused
ADDRESS = re.compile(r"^(todo/(\d\d)-[a-z0-9-]+/TODO-(\d\d)-[a-z0-9-]+\.md) " + SIGN + r"(\d{1,5})$")
HASH = re.compile(r"[0-9a-f]{40}")
SOURCE = {"repository": str, "from": list, "kind": str, "path": str, "section": str, "commit": (str, type(None)), "tree": (str, type(None)),
          "open": (dict, type(None))}
OPEN = {"tree": str, "proposal": str, "head": str}
FOLDER = re.compile(r"src(/[a-z0-9][a-z0-9-]*)+")
NAME = re.compile(r"[A-Za-z0-9_][A-Za-z0-9._-]*")
# Agent kits, lists and documents of a repository: never imported.
LEFT = {".agents", ".claude", ".codex", ".conclave", ".gitattributes", ".gitignore", ".github", "AGENTS.md", "CLAUDE.md", "CODEX.md", "GEMINI.md",
        "README.md", "docs", "helio.conf", "scripts", "todo"}
# What the functions of a DLL stand on, as the dependencies of a new section; a DLL that is not here waits for the tier alone.
UNDER = {"winmm.dll": f"`TODO-01` {SIGN}1, {SIGN}2", "ws2_32.dll": f"`07-networking/TODO-02` {SIGN}5, {SIGN}8", "user32.dll": f"`08-graphics-ui/TODO-14` {SIGN}5, {SIGN}6", "gdi32.dll": f"`08-graphics-ui/TODO-14` {SIGN}2, {SIGN}3",
         "dwmapi.dll": f"`08-graphics-ui/TODO-08` {SIGN}8", "kernel32.dll": f"`TODO-08` {SIGN}2, {SIGN}5, {SIGN}6", "shell32.dll": f"`TODO-08` {SIGN}12", "ntdll.dll": f"`TODO-08` {SIGN}4"}
REMOTE = "https://github.com/rizonesoft"
ROW = {"section": str, "source": str, "to": str}
GONE = {"name": str, "reason": str}
NEED = {"id": str, "source": str, "what": str, "section": str}
EMPTY = {"schema": 1, "sources": {}, "moved": [], "needs": [], "removed": []}


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
    if isinstance(ledger, dict):
        ledger.setdefault("needs", [])  # ledgers of before 2026-11-23 have none
    if not isinstance(ledger, dict) or set(ledger) != set(EMPTY) or ledger["schema"] != 1:
        return None, [f"{LEDGER}: needs schema 1, sources, moved, needs and removed"]
    if not isinstance(ledger["sources"], dict) or not all(isinstance(ledger[key], list) for key in ("moved", "needs", "removed")):
        return None, [f"{LEDGER}: sources is an object, moved, needs and removed are lists"]
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
    for key, shape in (("moved", ROW), ("needs", NEED), ("removed", GONE)):
        for number, row in enumerate(ledger[key], 1):
            faults += typed(row, shape, f"{key} row {number}")
    return (None, faults) if faults else (ledger, [])


def entries(ledger):
    return {f"source {n}" for n in ledger["sources"]} | {f"moved {r['section']}" for r in ledger["moved"]} | {f"need {r['id']}" for r in ledger["needs"]}


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
    """{number: {"Import": [names], "Moved": [names], "Need": [ids]}} as the roadmap's own parser sees the file."""
    found, current = {}, None
    for line in todo_fence.mask_text(text).split("\n"):
        heading = todo_fence.classify_heading(line)
        if heading.kind == "ok":
            current = found.setdefault(heading.n, {"Import": [], "Moved": [], "Need": []})
        elif todo_fence.is_h2(line):
            current = None
        elif current is not None and (mark := MARK.match(line)):
            current[mark[1]].append(mark[2])
    return found


def order(text):
    """{section number: what its row in the Implementation Order table depends on}."""
    start = text.find("## Implementation Order")
    table = re.search(r"(?:^\|.*\n)+", text[start:], re.M) if start >= 0 else None
    rows = [re.split(r"(?<!\\)\|", line)[1:-1] for line in (table[0].splitlines() if table else [])]
    head = [cell.strip() for cell in rows[0]] if rows else []
    end = next((k for k, name in enumerate(head) if name in ("Depends On", "Dep", "Dependency")), len(head))
    named = {int(m[1]): row for row in rows for cell in row[:end] if (m := re.match(rf"\s*{SIGN}(\d+)\b", cell))}
    named = named or {int(row[0]): row for row in rows if row and row[0].strip().isdigit()}
    return {number: row[end] if end < len(row) else "" for number, row in named.items()}


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
    there = checkout(folder, source["repository"])
    if not there.is_dir():
        return "of a repository that is not there"
    def ask(what):
        asked = git(there, "rev-parse", "--verify", "-q", what, check=False, env=clean())
        return asked.stdout.decode().strip() if asked.returncode == 0 else "unknown"

    if ask(source["commit"] + "^{commit}") != source["commit"]:  # a tree or a tag has files too
        return "of something that is not a commit"
    return picked(there, source["commit"], source["from"], env=clean()) or "unknown"


def checkout(folder, repository):
    """The checkout of that repository in folder: the folder of its name, else the one folder whose origin is it.

    A checkout may be named apart from its repository (SDK holds rizonesoft/ImpossibleSDK). When none or
    several match, the folder of its name is returned, and it is not there."""
    named = Path(folder) / repository
    if named.is_dir():
        return named
    host, owner = REMOTE.split("://", 1)[1].split("/", 1)
    ends = re.compile(rf"(?:^|[@/]){re.escape(host)}[/:]{re.escape(owner)}/{re.escape(repository)}(?:\.git)?/?$", re.IGNORECASE)  # GitHub's names ignore case
    folders = sorted(Path(folder).iterdir()) if Path(folder).is_dir() else []
    found = [there for there in folders if os.path.exists(there / ".git")  # False, not an error, for a folder it may not read
             and ends.search(git(there, "remote", "get-url", "origin", check=False, env=clean()).stdout.decode().strip())]
    return found[0] if len(found) == 1 else named


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

    parsed, marks = {}, {"Import": {}, "Moved": {}, "Need": {}}
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
        else:
            home = ledger["sources"].get(row["source"], {}).get("section", "")
            if done(at) and not done(home):
                faults.append(f"{at}: is done in the plan, and its import {home} is not")
            mine, theirs = ADDRESS.match(at), ADDRESS.match(home)
            if mine and theirs:
                # The way this roadmap names a section: in its own file, its own domain, or any.
                told, token = order(docs.get(mine[1], "")).get(int(mine[4]), ""), rf"{theirs[1].split('/')[1]}/TODO-{theirs[3]}\b|D{theirs[2]} T{theirs[3]}\b"
                if mine[2] == theirs[2]:
                    token += rf"|(?<![/\d])TODO-{theirs[3]}\b|(?<!D\d\d )\bT{theirs[3]}\b"
                tail = rf"[^A-Z]*?{SIGN}{theirs[4]}(?!\d)"
                if not re.search(rf"^[^A-Za-z]*?{SIGN}{theirs[4]}(?!\d)" if mine[1] == theirs[1] else rf"(?:{token}){tail}", told):
                    faults.append(f"{at}: does not depend on its import {home}")
    faults += [f"{at}: **Moved:** {found[0]} has no row in {LEDGER}" for at, found in marks["Moved"].items() if at not in rows]
    needs = {row["id"]: row for row in ledger["needs"]}
    if len(needs) != len(ledger["needs"]):
        faults.append("a need is there twice")
    for name, row in needs.items():
        if row["source"] not in ledger["sources"]:
            faults.append(f"need {name}: there is no source {row['source']}")
        if name not in marks["Need"].get(row["section"], []):
            faults.append(f"need {name}: {row['section']} does not carry the line **Need:** {name}")
    faults += [f"{at}: **Need:** {name} has no row in {LEDGER}" for at, found in marks["Need"].items() for name in found
               if needs.get(name, {}).get("section") != at]
    return faults


def functions_of(line):
    """(dll, function) pairs of a checklist item: `dll`: `f`, `g` or `f` of `dll`."""
    names = re.findall(r"`([^`]+)`", line)
    dlls = [n for n in names if n.endswith(".dll")]
    return [(dlls[0], n) for n in names if n not in dlls] if len(dlls) == 1 else []


def unanswered(root):
    """[(source, id, what, [(dll, function)])] that the programs beside this repository declare, committed there, and no row answers yet."""
    ledger, faults = load(git(root, "show", f":{LEDGER}").stdout)  # what would be committed
    if faults:
        raise SystemExit("\n".join(f"imports: {f}" for f in faults))
    beside = Path(os.environ.get("IMPOSSIBLE_REPOS") or Path(root).resolve().parent)  # the folder this checkout sits in
    known, missing = {row["id"] for row in ledger["needs"]}, []
    for name, source in ledger["sources"].items():
        shown = git(checkout(beside, source["repository"]), "show", "HEAD:app.json", check=False, env=clean())
        try:
            needs = json.loads(shown.stdout)["needs"] if shown.returncode == 0 and source["kind"] == "app" else []
            for need in needs:
                if not re.fullmatch(r"[a-z0-9-]+", need["id"]) or not re.fullmatch(r"[^\n|`]{4,120}", need["what"]):
                    raise ValueError(need)
                if need["id"] not in known:
                    missing.append((name, need["id"], need["what"], []))
        except (ValueError, KeyError, TypeError):
            raise SystemExit(f"imports: {name}: the needs in its app.json cannot be read") from None
    tiers = git(checkout(beside, "ImpossibleSDK"), "show", "HEAD:abi/tiers.json", check=False, env=clean())
    if tiers.returncode == 0:
        shown = git(root, "show", f":{NEEDS}", check=False)
        items, current = {}, None
        for line in todo_fence.mask_text(shown.stdout.decode()).split("\n"):
            heading = todo_fence.classify_heading(line)
            current = heading.n if heading.kind == "ok" else None if todo_fence.is_h2(line) else current
            if line.startswith("- ["):
                items.setdefault(current, set()).update(functions_of(line))
        for tier in json.loads(tiers.stdout)["tiers"]:
            # A function is answered by its own tier's section or by one that was added for that tier.
            own = [int(ADDRESS.match(row["section"])[4]) for row in ledger["needs"] if row["section"].startswith(NEEDS)
                   and re.match(rf"impossible-sdk-tier-{tier['tier']}(-|$)", row["id"])]
            answered = set().union(*(items.get(number, set()) for number in own))
            new = [(dll, f) for dll, found in sorted(tier["functions"].items()) for f in sorted(found) if (dll, f) not in answered]
            if new:
                first = re.sub(r"[^a-z0-9]", "", new[0][1].lower())
                missing.append(("impossible-sdk", f"impossible-sdk-tier-{tier['tier']}-{first}", f"{len(new)} more Windows function(s) of tier {tier['tier']}", new))
    return missing


def wanted(root):
    return [f"{source} needs {what}" + (": " + ", ".join(f for _, f in new) if new else f" ({name})") for source, name, what, new in unanswered(root)]


def ahead(plan, number, home):
    """The plan with the row of one section of NEEDS put before another's, where it is after it."""
    lines, (domain, file) = plan.split("\n"), re.search(r"/?(\d\d)-[^/]+/TODO-(\d\d)", NEEDS).groups()
    at = {n: next((k for k, line in enumerate(lines) if re.match(rf"\|[^|]*\| *`D{domain} T{file} {SIGN}{n}` *\|", line)), None) for n in (number, home)}
    if None not in at.values() and at[number] > at[home]:
        lines.insert(at[home], lines.pop(at[number]))
    return "\n".join(lines)


def file_needs(root):
    """One section in NEEDS for each, with its row there, in the ledger and in the plan; staged."""
    found = unanswered(root)
    path = root / NEEDS
    if git(root, "diff", "--name-only", "--", "todo").stdout or git(root, "ls-files", "-o", "--exclude-standard", "--", "todo").stdout:
        raise SystemExit("imports: todo has changes that are not staged, and the plan is made from all of it")
    text = path.read_text(encoding="utf-8")
    ledger = load(git(root, "show", f":{LEDGER}").stdout)[0]
    before = []
    for source, name, what, new in found:
        number = max(sections(text)) + 1
        tier = re.match(r"impossible-sdk-tier-(\d+)-", name)
        home = next((int(ADDRESS.match(n["section"])[4]) for n in ledger["needs"] if tier and n["id"] == f"impossible-sdk-tier-{tier[1]}"), None)
        own = [f"{SIGN}1"] if home != 1 else []
        stands = ", ".join(dict.fromkeys([*own, *(UNDER[dll] for dll, _ in new if dll in UNDER)])) or "--"
        if home:  # the tier is not complete without it
            before.append((number, home))
            text = re.sub(rf"^(\|[^|]*\| *\d+ *\| {SIGN}{home} [^|]*\| *)([^|]*?)( *\|)", lambda m: f"{m[1]}{SIGN}{number}, {m[2]}{m[3]}", text, count=1, flags=re.M)
        items = ["- [ ] Name what it stands on (a driver, a protocol, an engine, a file system) and the section that builds that; add it to this row's dependencies, and file a section where there is none"]
        items += [f"- [ ] `{f}` of `{dll}` acts as on Windows, whichever section builds it" for dll, f in new] or [f"- [ ] {what[0].upper()}{what[1:]}, as {source} declares it"]
        table = list(re.finditer(r"^\|[^\n]*\n", text[text.index("## Implementation Order"):], re.M))
        rows = text.index("## Implementation Order") + next(m.end() for k, m in enumerate(table) if k + 1 == len(table) or table[k + 1].start() != m.end())
        row = f"| 💎 | {number} | {SIGN}{number} {what[0].upper()}{what[1:]} | {stands} | [ ] |\n"
        body = "\n".join([f"## {number}. {what[0].upper()}{what[1:]}", "", "> **Spawned-by:** root", "", "**Design:** n/a -- declared by a program; the session that builds it names its design", "",
                          f"**Need:** {name}", "", *items, f"- [ ] Show it on this OS and on Windows with {source}'s own test, and compare", f'- [ ] Commit: `"needs: {name}"`', ""])
        text = text[:rows] + row + text[rows:]
        end = text.index("\n---\n", text.index(f"## {number - 1}. "))
        text = text[:end].rstrip("\n") + "\n\n" + body + text[end:]
        ledger["needs"].append({"id": name, "source": source, "what": what, "section": f"{NEEDS} {SIGN}{number}"})
        print(f"{NEEDS} {SIGN}{number}: {what}")
    if found:
        path.write_text(text, encoding="utf-8")
        (root / LEDGER).write_bytes(written(ledger))
        try:
            if (root / "scripts/todo-plan.py").is_file():
                sync = ["python3", "scripts/todo-plan.py", "--sync"]
                subprocess.run(["python3", "scripts/format-md-tables.py", NEEDS], cwd=root, check=True, stdout=subprocess.DEVNULL, timeout=60)
                subprocess.run(["bash", "scripts/todo-graph/build-and-validate.sh"], cwd=root, check=True, stdout=subprocess.DEVNULL, timeout=600)
                subprocess.run(sync, cwd=root, capture_output=True, timeout=600)  # places the new rows, last in their phase
                for number, home in before:
                    (root / PLAN).write_text(ahead((root / PLAN).read_text(encoding="utf-8"), number, home), encoding="utf-8")
                subprocess.run(sync, cwd=root, check=True, stdout=subprocess.DEVNULL, timeout=600)
        except BaseException:  # nothing is half filed: the files are as they were staged
            git(root, "checkout-index", "-f", "--", NEEDS, LEDGER, *([PLAN] if (root / PLAN).is_file() else []))
            raise
        git(root, "add", "--", NEEDS, LEDGER, *([PLAN] if (root / PLAN).is_file() else []))


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
        top = set(git(there, "ls-tree", "--name-only", commit, env=clean()).stdout.decode().split("\n")) - {""}
        if (extra := top - set(source["from"]) - LEFT):
            raise SystemExit(f"imports: {', '.join(sorted(extra))} is new in {source['repository']}: list it under from")
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
    parser.add_argument("--needs", action="store_true")
    parser.add_argument("--file-needs", action="store_true")
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
    elif args.file_needs:
        file_needs(args.root)
    elif args.needs:
        faults = wanted(args.root)
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
