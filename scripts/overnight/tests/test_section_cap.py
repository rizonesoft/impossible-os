#!/usr/bin/env python3
"""The TODO section-count cap: bound a file's growth without suppressing work.

Measured across 232 TODO files: median 9 sections, p90 16, largest-other 32 --
against TODO-04 at 63. A cap of 99 was considered and rejected for sitting
above every file in the repo, where it could never bind.

THE PROPERTY THAT MATTERS MOST is the one these tests exist to protect: the cap
must change WHERE a discovered gap is filed, never WHETHER it is filed.
"Finishing the listed checklist items is not enough if the feature is still
obviously incomplete" is what makes this runner produce real completeness; a
cap that tempted the run to swallow a gap would trade a visible large file for
invisible missing work. So the block message must name the destination, and the
cap must fire on GROWTH rather than on existence -- an already-oversized file
has to be able to finish what it has.
"""
from __future__ import annotations

import os
import pathlib
import subprocess
import tempfile

CHECK = pathlib.Path(__file__).resolve().parents[2] / "todo-staged-check.py"

# CAPS READ FROM THE CHECK ITSELF, never restated. These fixtures hardcoded 41
# and 61 against a soft cap of 40, so raising it to 50 on 2026-08-10 broke a test
# that was pinning the number rather than the BEHAVIOUR -- exactly the failure
# this repo already learned about measurement probes ("reuse the tool's own
# machinery, never reimplement its rule"). Sized relative to the constants, the
# cases survive the next change and keep asserting what they mean: one section
# past soft warns, past hard refuses.
def _caps():
    import importlib.util
    spec = importlib.util.spec_from_file_location("_tsc", str(CHECK))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod.SECTION_SOFT_CAP, mod.SECTION_HARD_CAP


SOFT_CAP, HARD_CAP = _caps()


def _git(*a, cwd):
    return subprocess.run(["git", *a], cwd=cwd, capture_output=True,
                          text=True, timeout=60)


def _repo(td, sections):
    root = pathlib.Path(td)
    _git("init", "-b", "main", ".", cwd=root)
    _git("config", "user.email", "t@t", cwd=root)
    _git("config", "user.name", "t", cwd=root)
    d = root / "todo" / "00-infrastructure"
    d.mkdir(parents=True)
    f = d / "TODO-99-fixture.md"
    body = "# Fixture\n\n" + "".join(
        f"## {i}. Section {i}\n\n- [x] item\n\n" for i in range(1, sections + 1))
    f.write_text(body)
    _git("add", "-A", cwd=root)
    _git("commit", "-m", "seed", cwd=root)
    return root, f


def _run(root):
    env = dict(os.environ)
    env.pop("SKIP_TODO_STAGED_CHECK", None)
    r = subprocess.run(["python3", str(CHECK)], cwd=root, capture_output=True,
                       text=True, timeout=120, env=env)
    return r.returncode, r.stdout + r.stderr


def _append_section(f, n, root):
    # PROVENANCE IS REQUIRED on a new section as of 2026-08-09, so a fixture
    # that omits it is refused for a reason this file is not about -- the soft-
    # cap case then reads as a block instead of a warning. `root` is the honest
    # declaration here: these fixtures invent a section from nothing.
    f.write_text(f.read_text()
                 + f"## {n}. Newly discovered gap\n\n"
                 + "> **Spawned-by:** root\n\n- [ ] work\n\n")
    _git("add", "-A", cwd=root)


def test_growth_past_the_hard_cap_is_blocked():
    with tempfile.TemporaryDirectory() as td:
        root, f = _repo(td, HARD_CAP + 1)
        _append_section(f, 62, root)
        rc, out = _run(root)
        assert rc == 1, f"expected a block, got rc={rc}\n{out}"
        assert "hard cap" in out, out


def test_the_block_names_the_destination_and_forbids_dropping_the_work():
    """The cap must never read as 'stop filing gaps'."""
    with tempfile.TemporaryDirectory() as td:
        root, f = _repo(td, HARD_CAP + 1)
        _append_section(f, 62, root)
        _, out = _run(root)
        assert "DO NOT DROP THE WORK" in out, out
        assert "domain-correct TODO" in out, out
        assert "reciprocal XREF" in out, out
        assert "part-2" in out, "must reject the part-2 escape hatch"
        # The row requirement is load-bearing: measured 2026-08-02, a body-only
        # filing left a completed TODO classified DONE and the work invisible.
        assert "IMPLEMENTATION ORDER ROW" in out, out
        assert "black hole" in out, out


def test_an_oversized_file_may_still_finish_its_existing_work():
    """Existence above the cap is NOT an error -- only growth is. An oversized
    file must be able to tick items, add stamps and close."""
    with tempfile.TemporaryDirectory() as td:
        root, f = _repo(td, HARD_CAP + 3)
        old_h = f"## {HARD_CAP + 3}. Section {HARD_CAP + 3}\n\n- [x] item"
        f.write_text(f.read_text().replace(old_h, old_h + "\n- [x] more work"))
        _git("add", "-A", cwd=root)
        rc, out = _run(root)
        assert rc == 0, f"an oversized file must still be able to progress\n{out}"


def test_soft_cap_warns_without_blocking():
    with tempfile.TemporaryDirectory() as td:
        root, f = _repo(td, SOFT_CAP + 1)
        _append_section(f, SOFT_CAP + 2, root)
        rc, out = _run(root)
        assert rc == 0, f"soft cap must not block, got rc={rc}\n{out}"
        assert "soft cap" in out, out


def test_a_small_file_is_untouched():
    with tempfile.TemporaryDirectory() as td:
        root, f = _repo(td, 8)
        _append_section(f, 9, root)
        rc, out = _run(root)
        assert rc == 0, out
        assert "cap" not in out.lower(), f"must be silent on normal files:\n{out}"


if __name__ == "__main__":
    test_growth_past_the_hard_cap_is_blocked()
    test_the_block_names_the_destination_and_forbids_dropping_the_work()
    test_an_oversized_file_may_still_finish_its_existing_work()
    test_soft_cap_warns_without_blocking()
    test_a_small_file_is_untouched()
    print(f"PASS: TODO section-count cap ({SOFT_CAP} soft / {HARD_CAP} hard)")
