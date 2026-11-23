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
TEXT = "# T\n\n## 1. Engine\n\n**Moved:** calculator\n\n## 2. Other\n\ntext\n\n## 3. Import\n\n**Import:** calculator\n\n- [ ] x\n"
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
        self.ledger = {"schema": 1, "removed": [], "sources": {"calculator": {
            "repository": "Calculator", "from": "engine", "kind": "app", "path": "src/apps/calculator", "section": IMPORT, "commit": None, "tree": None}},
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
        self.write(FILE, TEXT.replace("## 3. Import", "## 103. Import"))
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
        for name, text in (("a.c", "int a;\n"), ("deep/run.sh", "#!/bin/sh\n")):
            self.write(f"engine/{name}", text, root=theirs)
            self.write(f"src/apps/calculator/{name}", text)
        for root, path in ((theirs, "engine"), (self.root, "src/apps/calculator")):
            (root / path / "deep/run.sh").chmod(0o755)
            (root / path / "alias.h").symlink_to("a.c")
        self.commit(root=theirs)
        source = self.ledger["sources"]["calculator"]
        source["commit"] = self.git("rev-parse", "HEAD", root=theirs)
        source["tree"] = self.git("rev-parse", "HEAD:engine", root=theirs)
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
        for change in (lambda: (copy / "a.c").write_text("int b;\n"), lambda: (copy / "new.c").write_text(""), lambda: (copy / "a.c").rename(copy / "b.c"),
                       lambda: (copy / "deep/run.sh").chmod(0o644), lambda: ((copy / "alias.h").unlink(), (copy / "alias.h").symlink_to("deep/run.sh"))):
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
        for bad in ("{", "[]", good.replace('"schema": 1', '"schema": 2'), good.replace('"kind": "app", ', ""), good.replace('"Calculator"', '" "', 1),
                    good.replace('"Calculator"', "3", 1), good.replace('"calculator": {', '"Cal c": {'), good.replace("src/apps/calculator", "../x"),
                    good.replace('"moved": [', '"moved": [1, '), good.replace('"removed": []', '"removed": [{"name": "x", "reason": " "}]')):
            self.assertNotEqual(bad, good)
            self.write("todo/imports.json", bad)
            self.assertNotEqual(self.faults(), "", bad)

    def test_a_name_that_is_not_text_does_not_stop_the_check(self):
        with open(os.fsencode(self.root / "todo") + b"/\xff.txt", "w"):
            pass
        self.assertEqual(self.faults(), "")

    def test_the_real_ledger_passes(self):
        self.assertEqual(imports.check(Path(__file__).resolve().parents[2]), [])


if __name__ == "__main__":
    unittest.main()
