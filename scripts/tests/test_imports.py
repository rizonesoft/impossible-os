#!/usr/bin/env python3
"""scripts/imports.py: nothing that moved out of the roadmap can be forgotten."""
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import imports

FILE = "todo/11-apps/TODO-09-calculator.md"
MOVED, IMPORT = (f"{FILE} {imports.SIGN}{n}" for n in (1, 3))
TEXT = f"# T\n\n## Implementation Order\n\n| Step | Section | Dep | Status |\n| --- | --- | --- | --- |\n| 1 | Engine | {imports.SIGN}3 | [ ] |\n| 2 | Other | -- | [ ] |\n| 3 | Import | -- | [ ] |\n\n## 1. Engine\n\n**Moved:** calculator\n\n## 2. Other\n\ntext\n\n## 3. Import\n\n**Import:** calculator\n\n- [ ] x\n"
PLAN = f"| Done | Section |\n| :-: | --- |\n| [ ]  | `D{11} T{9:02} {imports.SIGN}3`  | Import | none | 1 |\n"


class Imports(unittest.TestCase):
    def setUp(self):
        # A hook exports variables that would point git at the repository running the hook.
        clean = {k: v for k, v in os.environ.items() if not k.startswith("GIT_") and k != "IMPOSSIBLE_REPOS"}
        patch = mock.patch.dict(os.environ, clean, clear=True)
        patch.start()
        self.addCleanup(patch.stop)
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name, "os")
        self.root.mkdir()
        self.start(self.root)
        self.ledger = {"schema": 1, "removed": [], "needs": [], "sources": {"calculator": {
            "repository": "Calculator", "from": ["engine", "engine.txt", "run.sh"], "kind": "app", "path": "src/apps/calculator", "section": IMPORT, "commit": None, "tree": None, "open": None}},
            "moved": [{"section": MOVED, "source": "calculator", "to": "Calculator: engine/"}]}
        self.write(FILE, TEXT)
        self.write("todo/implementation-plan.md", PLAN)
        self.save()
        self.commit()

    def start(self, root):
        self.git("init", "-q", root=root)
        self.git("config", "core.hooksPath", os.devnull, root=root)

    def git(self, *arguments, root=None):
        return subprocess.run(["git", "-C", str(root or self.root), "-c", "user.name=T", "-c", "user.email=t@example.invalid", *arguments],
                              capture_output=True, check=True, timeout=60).stdout.decode().strip()

    def write(self, path, text, root=None):
        path = (root or self.root) / path
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def save(self):
        self.write("todo/imports.json", json.dumps(self.ledger))

    def commit(self, root=None):
        self.git("add", "-A", root=root)
        self.git("commit", "-qm", "x", "--allow-empty", root=root)

    def faults(self):
        """What the check says about everything staged now."""
        self.git("add", "-A")
        return "\n".join(imports.check(self.root))

    def test_a_complete_ledger_passes(self):
        self.assertEqual(self.faults(), "")

    def test_each_side_is_missed(self):
        for old, new, said in (("**Import:** calculator", "", "does not carry the one line **Import:**"),
                               ("**Moved:** calculator", "", "does not carry the one line **Moved:**"),
                               ("## 1. Engine", "## 7. Engine", "keeps its address"),
                               ("## 3. Import", "## 8. Import", "is not a roadmap section"),
                               ("text", "**Moved:** calculator", "has no row"),
                               ("text", "**Moved:** new_app", "has no row"), ("text", "**Moved:**", "has no row"),
                               ("text", "**Import:** notepad", "has no source")):
            self.write(FILE, TEXT.replace(old, new, 1))
            self.assertIn(said, self.faults(), old)

    def test_a_mark_that_is_not_seen_is_not_a_mark(self):
        for hidden in ("```\n**Import:** calculator\n```", "<!--\n**Import:** calculator\n-->", " ## 9. Other\n\n**Import:** calculator"):
            self.write(FILE, TEXT.replace("**Import:** calculator", hidden))
            self.assertIn("does not carry the one line **Import:**", self.faults(), hidden)

    def test_a_section_number_of_three_digits_works(self):
        self.write(FILE, TEXT.replace("## 3. Import", "## 103. Import").replace(f"| Engine | {imports.SIGN}3 |", f"| Engine | {imports.SIGN}103 |"))
        self.ledger["sources"]["calculator"]["section"] = IMPORT[:-1] + "103"
        self.save()
        self.assertEqual(self.faults(), "")

    def drop(self):
        self.ledger["moved"] = []
        self.write(FILE, TEXT.replace("**Moved:** calculator", ""))
        self.save()

    def test_an_entry_cannot_leave_without_a_reason_also_once_committed(self):
        self.drop()
        self.assertIn("left todo/imports.json 1 time(s) and has 0 reason(s)", self.faults())
        self.commit()
        self.assertIn("left todo/imports.json 1 time(s) and has 0 reason(s)", self.faults())
        self.ledger["removed"] = [{"name": f"moved {MOVED}", "reason": "the owner dropped it"}]
        self.save()
        self.assertEqual(self.faults(), "")
        self.commit()
        self.ledger["removed"] = []
        self.save()
        self.assertIn("is kept for good", self.faults())

    def test_one_reason_excuses_one_removal(self):
        first = [dict(r) for r in self.ledger["moved"]]
        self.drop()
        self.ledger["removed"] = [{"name": f"moved {MOVED}", "reason": "one"}]
        self.save()
        self.commit()
        self.ledger["moved"] = first
        self.write(FILE, TEXT)
        self.save()
        self.commit()
        self.drop()
        self.assertIn("left todo/imports.json 2 time(s) and has 1 reason(s)", self.faults())
        self.ledger["removed"].append({"name": f"moved {MOVED}", "reason": "two"})
        self.save()
        self.assertEqual(self.faults(), "")

    def test_the_ledger_itself_cannot_vanish(self):
        (self.root / "todo/imports.json").unlink()
        self.assertIn("is missing", self.faults())
        self.commit()
        self.assertIn("is missing", self.faults())

    def test_a_ledger_that_was_deleted_and_put_back_still_owes_its_reasons(self):
        (self.root / "todo/imports.json").unlink()
        self.commit()
        self.save()
        self.assertIn("left todo/imports.json 1 time(s)", self.faults())

    def test_everything_is_read_from_one_tree(self):
        """A ledger staged while the check runs is not mixed with the older one."""
        self.git("add", "-A")
        real = imports.todo_fence.staged_docs

        def moved(root):
            answer = real(root)
            self.write("todo/imports.json", "{")
            self.git("add", "-A")
            return answer

        with mock.patch.object(imports.todo_fence, "staged_docs", moved):
            self.assertEqual(imports.check(self.root), [])
        self.assertNotEqual(self.faults(), "")

    def test_an_import_is_not_done_before_anything_arrived(self):
        for done in (PLAN.replace("[ ]", "[x]"), PLAN.replace("| [ ]  | ", "|[x]|")):
            self.assertNotEqual(done, PLAN)
            self.write("todo/implementation-plan.md", done)
            self.assertIn("is done, and nothing was imported", self.faults())

    def arrive(self):
        """The program's own repository, and its copy here, pinned."""
        repos = Path(self.temp.name, "repos")
        theirs = repos / "Calculator"
        theirs.mkdir(parents=True)
        self.start(theirs)
        for root, path in ((theirs, "."), (self.root, "src/apps/calculator")):
            self.write(f"{path}/engine/a.c", "int a;\n", root=root)
            self.write(f"{path}/run.sh", "#!/bin/sh\n", root=root)
            self.write(f"{path}/engine.txt", "git sorts it before engine/\n", root=root)
            (root / path / "run.sh").chmod(0o755)
            (root / path / "engine/alias.h").symlink_to("a.c")
        self.write("AGENTS.md", "not imported\n", root=theirs)
        self.commit(root=theirs)
        self.git("add", "-A")
        source = self.ledger["sources"]["calculator"]
        source["commit"] = self.git("rev-parse", "HEAD", root=theirs)
        source["tree"] = self.git("rev-parse", self.git("write-tree") + ":src/apps/calculator")
        self.save()
        return repos, source

    def test_files_without_a_pin_are_refused(self):
        self.write("src/apps/calculator/a.c", "int a;\n")
        self.assertIn("not imported yet", self.faults())

    def test_the_copy_has_the_tree_id_of_its_own_repository(self):
        repos, source = self.arrive()
        self.assertEqual(self.faults(), "")
        with mock.patch.dict(os.environ, {"IMPOSSIBLE_REPOS": str(repos), "GIT_DIR": str(self.root / ".git")}):
            self.assertEqual("\n".join(imports.check(self.root)), "")
        self.commit()
        copy = self.root / "src/apps/calculator"
        for change in (lambda: (copy / "engine/a.c").write_text("int b;\n"), lambda: (copy / "new.c").write_text(""),
                       lambda: (copy / "engine/a.c").rename(copy / "engine/b.c"), lambda: (copy / "run.sh").chmod(0o644),
                       lambda: ((copy / "engine/alias.h").unlink(), (copy / "engine/alias.h").symlink_to("../run.sh"))):
            change()
            self.assertIn("is not the pinned copy", self.faults())
            self.git("reset", "-q", "--hard")
        self.assertEqual(self.faults(), "")

    def test_a_pin_that_is_not_where_the_copy_came_from_is_refused(self):
        repos, source = self.arrive()
        with mock.patch.dict(os.environ, {"IMPOSSIBLE_REPOS": str(repos)}):
            good = source["commit"]
            theirs = repos / "Calculator"
            for wrong in ("a" * 40, "abc", self.git("rev-parse", "HEAD^{tree}", root=theirs)):
                source["commit"] = wrong
                self.save()
                self.assertNotEqual(self.faults(), "", wrong)
            source["commit"], source["repository"] = good, "Calculater"
            self.save()
            self.assertIn("a repository that is not there", self.faults())

    def test_a_bad_ledger_is_refused(self):
        good = json.dumps(self.ledger)
        for bad in ("{", "[]", good.replace('"schema": 1', '"schema": 2'), good.replace('"kind": "app", ', ""), good.replace('["engine", "engine.txt", "run.sh"]', "[]"),
                    good.replace('["engine", "engine.txt", "run.sh"]', '["engine", "engine"]'), good.replace('["engine", "engine.txt", "run.sh"]', '["../x"]'),
                    good.replace('"open": null', '"open": {}'), good.replace('"Calculator"', '" "', 1),
                    good.replace('"Calculator"', "3", 1), good.replace('"calculator": {', '"Cal c": {'), good.replace("src/apps/calculator", "../x"), good.replace("src/apps/calculator", ".git"),
                    good.replace('"Calculator"', '"../Calculator"', 1),
                    good.replace('"moved": [', '"moved": [1, '), good.replace('"removed": []', '"removed": [{"name": "x", "reason": " "}]')):
            self.assertNotEqual(bad, good)
            self.write("todo/imports.json", bad)
            self.assertNotEqual(self.faults(), "", bad)

    def test_a_name_that_is_not_text_does_not_stop_the_check(self):
        with open(os.fsencode(self.root / "todo") + b"/\xff.txt", "w"):
            pass
        self.assertEqual(self.faults(), "")

    def linked(self):
        """The same as arrive(), with a remote that pull and propose reach, and gh standing in."""
        repos, source = self.arrive()
        theirs, remotes = repos / "Calculator", Path(self.temp.name, "remotes")
        self.git("branch", "-M", "main", root=theirs)
        self.git("clone", "-q", "--bare", str(theirs), str(remotes / "Calculator.git"))
        self.git("remote", "add", "origin", str(remotes / "Calculator.git"), root=theirs)
        self.commit()
        self.calls, self.state, self.broken = [], {"state": "OPEN", "mergeCommit": None, "isCrossRepository": False, "commits": []}, False
        real = imports.outside

        def outside(*command, **more):
            if command[0] != "gh":
                return real(*command, **more)
            if command[2] == "create" and self.broken:
                raise SystemExit("imports: gh pr: no network")
            self.calls.append(command[1:3])
            return json.dumps(self.state) if command[2] == "view" else f"https://github.com/rizonesoft/Calculator/pull/{len(self.calls)}"

        identity = {f"GIT_{who}_{what}": value for who in ("AUTHOR", "COMMITTER") for what, value in (("NAME", "T"), ("EMAIL", "t@example.invalid"))}
        home = Path(self.temp.name, "home")
        home.mkdir()
        (home / ".gitconfig").write_text("[user]\nname = T\nemail = t@example.invalid\n")
        for patch in (mock.patch.object(imports, "outside", outside), mock.patch.dict(os.environ, {"IMPOSSIBLE_REMOTE": str(remotes), "HOME": str(home), **identity})):
            patch.start()
            self.addCleanup(patch.stop)
        return theirs, remotes / "Calculator.git"

    def now(self):
        return json.loads((self.root / "todo/imports.json").read_text())["sources"]["calculator"]

    def change(self, text):
        self.write("src/apps/calculator/engine/a.c", text)
        self.git("add", "-A")

    def test_a_change_here_is_proposed_there_and_comes_back(self):
        theirs, remote = self.linked()
        self.write("README.md", "newer\n", root=theirs)  # their main moved on meanwhile
        self.commit(root=theirs)
        self.git("push", "-q", "origin", "main", root=theirs)
        self.change("int changed;\n")
        self.assertIn("propose the change", self.faults())
        imports.propose(self.root, "calculator", "engine: changed here")
        self.assertEqual(self.faults(), "")
        branch = self.git("for-each-ref", "--format=%(refname)", "refs/heads/impossible-os", root=remote)
        self.assertEqual(self.git("show", f"{branch}:engine/a.c", root=remote), "int changed;")
        self.assertEqual(self.git("show", f"{branch}:AGENTS.md", root=remote), "not imported")
        self.assertEqual(self.git("diff", "--name-only", "main", branch, root=remote), "engine/a.c")
        self.commit()
        self.state["commits"] = [{"oid": self.git("rev-parse", branch, root=remote)}]
        with self.assertRaisesRegex(SystemExit, "drop its change"):
            imports.pull(self.root, "calculator", None)
        self.git("update-ref", "refs/heads/main", branch, root=remote)
        head = self.git("rev-parse", branch, root=remote)
        for commits, fork in (([{"oid": "a" * 40}], False), ([{"oid": head}], True)):
            self.state = {"state": "MERGED", "mergeCommit": {"oid": head}, "isCrossRepository": fork, "commits": commits}
            with self.assertRaisesRegex(SystemExit, "does not hold this change"):
                imports.pull(self.root, "calculator", None)
        self.state["isCrossRepository"] = False
        imports.pull(self.root, "calculator", None)
        self.assertEqual((self.now()["open"], self.now()["commit"]), (None, self.state["mergeCommit"]["oid"]))
        self.assertEqual(self.faults(), "")
        self.assertEqual((self.root / "src/apps/calculator/engine/a.c").read_text(), "int changed;\n")
        self.assertFalse((self.root / "src/apps/calculator/AGENTS.md").exists())

    def test_a_second_change_replaces_the_first_proposal(self):
        self.linked()
        self.change("int one;\n")
        imports.propose(self.root, "calculator", "one")
        self.change("int two;\n")
        self.assertIn("propose it again", self.faults())
        imports.propose(self.root, "calculator", "two")  # the first one is not known to be ours: it is left alone
        self.assertNotIn(("pr", "close"), self.calls)
        self.state["commits"] = [{"oid": self.now()["open"]["head"]}]
        self.change("int three;\n")
        imports.propose(self.root, "calculator", "three")
        self.assertEqual(self.calls[-1], ("pr", "close"))
        self.assertEqual(self.faults(), "")
        with self.assertRaisesRegex(SystemExit, "no new change"):
            imports.propose(self.root, "calculator", "again")

    def test_nothing_unstaged_or_unproposed_is_lost(self):
        self.linked()
        path = self.root / "src/apps/calculator/engine/a.c"
        path.write_text("int unstaged;\n")
        for command in (lambda: imports.pull(self.root, "calculator", None), lambda: imports.propose(self.root, "calculator", "x")):
            with self.assertRaisesRegex(SystemExit, "not staged"):
                command()
        self.git("add", "-A")
        with self.assertRaisesRegex(SystemExit, "was not proposed"):
            imports.pull(self.root, "calculator", None)
        self.git("reset", "-q", "--hard")
        for flag, back in (("--assume-unchanged", "--no-assume-unchanged"), ("--skip-worktree", "--no-skip-worktree")):
            self.git("update-index", flag, "src/apps/calculator/engine/a.c")
            path.write_text("int unseen;\n")
            with self.assertRaisesRegex(SystemExit, "not staged"):
                imports.pull(self.root, "calculator", None)
            self.assertEqual(path.read_text(), "int unseen;\n")
            self.git("update-index", back, "src/apps/calculator/engine/a.c")

    def test_a_pull_that_fails_leaves_everything_as_it_was(self):
        theirs, remote = self.linked()
        self.write("engine/a.c", "int newer;\n", root=theirs)
        self.commit(root=theirs)
        self.git("push", "-q", "origin", "main", root=theirs)
        with mock.patch.object(imports, "index", side_effect=OSError("planted")), self.assertRaises(OSError):
            imports.pull(self.root, "calculator", None)
        self.assertEqual(self.git("status", "--porcelain"), "")
        self.git("config", "filter.f.smudge", "false")
        self.git("config", "filter.f.clean", "cat")
        self.git("config", "filter.f.required", "true")
        (self.root / ".git/info/attributes").write_text("src/** filter=f\n")
        imports.pull(self.root, "calculator", None)  # a filter of this repository has no say
        self.assertEqual((self.root / "src/apps/calculator/engine/a.c").read_text(), "int newer;\n")
        self.assertEqual("\n".join(imports.check(self.root)), "")

    def test_a_conflict_proposes_nothing(self):
        theirs, remote = self.linked()
        self.write("engine/a.c", "int theirs;\n", root=theirs)
        self.commit(root=theirs)
        self.git("push", "-q", "origin", "main", root=theirs)
        self.change("int ours;\n")
        with self.assertRaises(SystemExit):
            imports.propose(self.root, "calculator", "x")
        self.assertIsNone(self.now()["open"])
        self.assertEqual(self.git("for-each-ref", "--format=%(refname)", "refs/heads/impossible-os", root=remote), "")

    def test_a_try_that_stopped_after_the_push_goes_on(self):
        theirs, remote = self.linked()
        self.change("int once;\n")
        self.broken = True
        with self.assertRaisesRegex(SystemExit, "no network"):
            imports.propose(self.root, "calculator", "x")
        self.assertIsNone(self.now()["open"])
        self.broken = False
        imports.propose(self.root, "calculator", "x")
        self.assertEqual(self.faults(), "")

    def test_a_folder_missing_on_either_side_proposes_nothing(self):
        theirs, remote = self.linked()
        self.git("rm", "-q", "src/apps/calculator/run.sh")
        with self.assertRaisesRegex(SystemExit, "run.sh is in the copy or under from"):
            imports.propose(self.root, "calculator", "x")
        self.assertEqual(self.git("for-each-ref", "--format=%(refname)", "refs/heads/impossible-os", root=remote), "")

    def test_attributes_neither_drop_nor_change_a_file(self):
        theirs, remote = self.linked()
        self.write(".gitattributes", "src/** text eol=crlf\n")
        self.git("add", ".gitattributes")
        self.git("commit", "-qm", "x")
        self.write("engine/.gitattributes", "hidden.c export-ignore\n", root=theirs)
        self.write("engine/hidden.c", "int hidden;\n", root=theirs)
        self.commit(root=theirs)
        self.git("push", "-q", "origin", "main", root=theirs)
        imports.pull(self.root, "calculator", None)
        self.assertEqual("\n".join(imports.check(self.root)), "")
        self.assertEqual(self.git("show", ":src/apps/calculator/engine/hidden.c"), "int hidden;")
        self.git("commit", "-qm", "x")
        blob = subprocess.run(["git", "-C", str(self.root), "hash-object", "-w", "--stdin"], input=b"int b;\n", capture_output=True, check=True).stdout.decode().strip()
        self.git("update-index", "--cacheinfo", f"100644,{blob},src/apps/calculator/engine/a.c")
        self.git("checkout", "-q", "--", "src")
        imports.propose(self.root, "calculator", "x")
        branch = self.git("for-each-ref", "--format=%(refname)", "refs/heads/impossible-os", root=remote)
        self.assertEqual(self.git("diff", "--name-only", "main", branch, root=remote), "engine/a.c")

    def test_a_staged_ledger_that_differs_from_the_file_is_not_overwritten(self):
        self.linked()
        self.ledger = json.loads((self.root / "todo/imports.json").read_text())
        text = (self.root / "todo/imports.json").read_text()
        self.write("todo/imports.json", text.replace('"kind": "app"', '"kind": "part"'))
        self.git("add", "-A")
        self.write("todo/imports.json", text)
        with self.assertRaisesRegex(SystemExit, "not staged"):
            imports.pull(self.root, "calculator", None)

    def test_a_hook_names_this_repository_and_not_the_other(self):
        theirs, remote = self.linked()
        self.write("engine/new.c", "int n;\n", root=theirs)
        self.commit(root=theirs)
        self.git("push", "-q", "origin", "main", root=theirs)
        with mock.patch.dict(os.environ, {"GIT_DIR": str(self.root / ".git"), "GIT_WORK_TREE": str(self.root)}):
            imports.pull(self.root, "calculator", None)
        self.assertTrue((self.root / "src/apps/calculator/engine/new.c").is_file())

    def test_a_pin_without_its_tree_is_refused_also_with_an_open_change(self):
        self.linked()
        self.change("int a1;\n")
        imports.propose(self.root, "calculator", "x")
        self.ledger = json.loads((self.root / "todo/imports.json").read_text())
        self.ledger["sources"]["calculator"]["tree"] = None
        self.save()
        self.assertIn("are not full hashes", self.faults())

    def test_a_moved_section_waits_for_its_import(self):
        self.write(FILE, TEXT.replace(f"| Engine | {imports.SIGN}3 |", "| Engine | -- |"))
        self.assertIn("does not depend on its import", self.faults())
        self.write(FILE, TEXT)
        row = PLAN.splitlines()[2].replace(f"{imports.SIGN}3", f"{imports.SIGN}1").replace("[ ]", "[x]")
        self.write("todo/implementation-plan.md", PLAN + row + "\n")
        self.assertIn("is done in the plan, and its import", self.faults())

    def test_what_is_new_at_the_top_of_a_repository_is_not_left_out_unseen(self):
        theirs, remote = self.linked()
        for name, said in (("todo", None), ("levels", "levels is new in Calculator")):
            self.write(f"{name}/x", "x", root=theirs)
            self.commit(root=theirs)
            self.git("push", "-q", "origin", "main", root=theirs)
            if said:
                with self.assertRaisesRegex(SystemExit, said):
                    imports.pull(self.root, "calculator", None)
            else:
                imports.pull(self.root, "calculator", None)

    def test_a_need_and_its_section_are_never_without_each_other(self):
        self.ledger["needs"] = [{"id": "calculator-sound", "source": "calculator", "what": "a sound", "section": f"{FILE} {imports.SIGN}2"}]
        self.save()
        self.assertIn("does not carry the line **Need:** calculator-sound", self.faults())
        self.write(FILE, TEXT.replace("text", "**Need:** calculator-sound"))
        self.assertEqual(self.faults(), "")
        self.commit()
        self.ledger["needs"] = []
        self.save()
        self.assertIn("**Need:** calculator-sound has no row", self.faults())
        self.write(FILE, TEXT)
        self.assertIn("need calculator-sound left todo/imports.json", self.faults())

    def test_what_a_program_declares_and_nobody_builds_is_named(self):
        repos, source = self.arrive()
        for name in ("Calculator", "ImpossibleSDK"):
            (repos / name).mkdir(exist_ok=True)
        self.start(repos / "ImpossibleSDK")
        self.write("abi/tiers.json", json.dumps({"tiers": [{"tier": 1, "functions": {"kernel32.dll": ["FunctionNamedHere", "FunctionOfNobody"]}}]}), root=repos / "ImpossibleSDK")
        self.commit(root=repos / "ImpossibleSDK")
        self.write("app.json", json.dumps({"needs": [{"id": "calculator-sound", "what": "a sound"}]}), root=repos / "Calculator")
        self.ledger["sources"]["impossible-sdk"] = {**self.ledger["sources"]["calculator"], "repository": "ImpossibleSDK", "kind": "part", "path": "src/libs/sdk", "commit": None, "tree": None, "section": f"{FILE} {imports.SIGN}2"}
        tier = {"id": "impossible-sdk-tier-1", "source": "impossible-sdk", "what": "tier 1", "section": f"{imports.NEEDS} {imports.SIGN}1"}
        self.ledger["needs"] = [tier]
        self.save()
        self.write(FILE, TEXT.replace("text", "FunctionOfNobody in prose answers nothing"))
        self.write(imports.NEEDS, "# T\n\n## 1. One\n\n**Need:** impossible-sdk-tier-1\n\n- [ ] `kernel32.dll`: `FunctionNamedHere`\n- [ ] `user32.dll`: `FunctionOfNobody`\n```\n- [ ] `kernel32.dll`: `FunctionOfNobody`\n```\n\n## 2. Another tier\n\n- [ ] `kernel32.dll`: `FunctionOfNobody`\n")
        self.git("add", "-A")
        with mock.patch.dict(os.environ, {"IMPOSSIBLE_REPOS": str(repos)}):
            # The program's need is not committed there yet.
            self.assertEqual(imports.wanted(self.root), ["impossible-sdk needs 1 more Windows function(s) of tier 1: FunctionOfNobody"])
            self.commit(root=repos / "Calculator")
            self.assertIn("calculator needs a sound (calculator-sound)", imports.wanted(self.root))
            self.write(imports.NEEDS, "# T\n\n## Implementation Order\n\n| a | Order | Deliverable | Depends On | Status |\n| --- | --- | --- | --- | --- |\n| x | 1 | " + imports.SIGN + "1 one | -- | [ ] |\n\n---\n\n## 1. One\n\n**Need:** impossible-sdk-tier-1\n\n- [ ] `kernel32.dll`: `FunctionNamedHere`\n\n---\n\n## Verification\n")
            self.write(FILE, TEXT.replace("text", "**Import:** impossible-sdk"))
            self.save()
            with self.assertRaisesRegex(SystemExit, "not staged"):
                imports.file_needs(self.root)
            self.git("add", "-A")
            imports.file_needs(self.root)
            self.assertEqual(imports.wanted(self.root), [])
            self.assertEqual(self.faults(), "")
            text = (self.root / imports.NEEDS).read_text()
            rows = [line for line in text.split("\n") if line.startswith("| ")][2:]
            self.assertEqual([r.split("|")[2].strip() for r in rows], ["1", "2", "3"])
            self.assertIn(f"| {imports.SIGN}3, -- |", rows[0])  # the tier waits for what was added to it
            self.assertIn("`TODO-08`", rows[2])
            self.assertIn("- [ ] `FunctionOfNobody` of `kernel32.dll` acts as on Windows", text)
            self.assertEqual(text.count("Name what it stands on"), 2)
            self.assertLess(text.index("## 3. 1 more"), text.index("## Verification"))
            imports.file_needs(self.root)  # nothing new: nothing written
            self.assertEqual((self.root / imports.NEEDS).read_text(), text)
            for bad in ('{"needs": 7}', '{"needs": [{"id": "A b", "what": "a sound"}]}', '{"needs": [{"id": "a", "what": "two\\nlines"}]}'):
                self.write("app.json", bad, root=repos / "Calculator")
                self.commit(root=repos / "Calculator")
                with self.assertRaisesRegex(SystemExit, "cannot be read"):
                    imports.wanted(self.root)
        with mock.patch.dict(os.environ, {"IMPOSSIBLE_REPOS": str(repos / "nowhere")}):
            self.assertEqual(imports.wanted(self.root), [])

    def test_a_new_function_is_planned_before_its_tier(self):
        row = {n: f"| [ ] | `D{10} T{13} {imports.SIGN}{n}` | x |" for n in (4, 7, 47)}
        for lines, then in (([row[4], row[47], row[7], "end"], [row[7], row[4], row[47], "end"]), ([row[7], row[4]], [row[7], row[4]]), ([row[4]], [row[4]])):
            self.assertEqual(imports.ahead("\n".join(lines), 7, 4).split("\n"), then)

    def test_a_false_proposal_is_refused(self):
        self.linked()
        self.change("int a1;\n")
        tree = self.git("rev-parse", self.git("write-tree") + ":src/apps/calculator")
        source = self.ledger["sources"]["calculator"]
        for proposal in ("https://github.com/rizonesoft/Notepad/pull/1", "https://github.com/rizonesoft/Calculator/pull/0", "later"):
            source["open"] = {"tree": tree, "proposal": proposal, "head": "a" * 40}
            self.save()
            self.assertIn("is not a pull request", self.faults(), proposal)
        source["open"] = {"tree": source["tree"], "proposal": "https://github.com/rizonesoft/Calculator/pull/1", "head": "a" * 40}
        self.save()
        self.git("checkout", "-q", "HEAD", "--", "src")
        self.assertIn("nothing is open", self.faults())

    def test_the_real_ledger_passes(self):
        self.assertEqual(imports.check(Path(__file__).resolve().parents[2]), [])


if __name__ == "__main__":
    unittest.main()
