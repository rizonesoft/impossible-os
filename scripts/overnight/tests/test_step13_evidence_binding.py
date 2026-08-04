#!/usr/bin/env python3
"""section_commit_gate step-13: evidence validity is CONTENT-bound, not aged out.

The module header states the policy for every other evidence check in this
gate -- "these wall-clock TTLs apply ONLY when the content binding is absent
... a slow review over unchanged content never expires" -- and
`_dispatch_content_fresh` implements it ("age is irrelevant by design"). The
step-13 check never adopted either; it aged out head-bound evidence
unconditionally at 30 minutes.

MEASURED 2026-08-04 (v08, the LSP-bridge TODO's kernel-SEH section): a correctly
section-bound and HEAD-bound stamp recorded at 07:32 was refused at 08:05,
because `implement-todo-section` step 16's OWN mandatory chain -- build 2m +
test.sh 5m + test-tooling.sh 18m, before TODO bookkeeping, the loose-end sweep
and the todo-graph rebuild -- does not fit in 30 minutes. The gate was refusing
evidence for getting old while the run did the work the skill required, and the
refusal named the SECTION BINDING, so it read as "you never dispatched": it cost
a full re-dispatch carrying no new information, and was then filed a SECOND time
as a review-kind (`adversarial` vs `adversarial-impl`) mismatch, which does not
reproduce -- both tags have been accepted here since 2026-06-30.

The change is a net STRENGTHENING, which is what these tests pin: a 5-minute-old
stamp over DRIFTED source used to pass and now refuses.
"""
from __future__ import annotations

import importlib.util
import json
import pathlib
import subprocess
import sys
import tempfile
import time

REPO = pathlib.Path(__file__).resolve().parents[3]
HOOK = REPO / ".claude/hooks/section_commit_gate.py"
SEC = "§"          # the gate's own section glyph, built rather than typed


def _load():
    if str(HOOK.parent) not in sys.path:
        sys.path.insert(0, str(HOOK.parent))
    spec = importlib.util.spec_from_file_location("section_commit_gate", HOOK)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _fixture(tmp, tag="adversarial", age_min=5, section="22",
             record_head=True, drift=False):
    d = pathlib.Path(tmp)
    g = ["git", "-C", str(d)]
    subprocess.run(["git", "init", "-q", str(d)], check=True)
    (d / "src").mkdir()
    (d / "src/a.c").write_text("int x;\n")
    subprocess.run(g + ["add", "-A"], check=True)
    subprocess.run(g + ["-c", "user.email=t@t", "-c", "user.name=t",
                        "commit", "-qm", "base"], check=True)
    disp_head = subprocess.run(g + ["rev-parse", "HEAD"],
                               capture_output=True, text=True).stdout.strip()
    if drift:
        (d / "src/a.c").write_text("int y;\n")
        subprocess.run(g + ["add", "-A"], check=True)
        subprocess.run(g + ["-c", "user.email=t@t", "-c", "user.name=t",
                            "commit", "-qm", "drift"], check=True)
    st = d / ".claude/state"
    st.mkdir(parents=True)
    (st / "skill-progress.json").write_text(
        json.dumps({"implement-todo-section": {"step": 13}}))
    entry = {tag: time.time_ns() - int(age_min * 60 * 1e9),
             f"{tag}_section": section}
    if record_head:
        entry[f"{tag}_head"] = disp_head
    (st / "last-review-stamps.json").write_text(
        json.dumps({"todo/x.md": entry}))
    return d


def _check(mod, d):
    return mod._step13_impl_adversarial_check(
        d, ["src/a.c"], ["todo/x.md"], {"todo/x.md": ["22"]})


def test_head_bound_evidence_does_not_age_out():
    """The defect: the mandatory gate chain is longer than the old TTL."""
    mod = _load()
    for tag in ("adversarial", "adversarial-impl"):
        for age in (5, 33, 120):
            with tempfile.TemporaryDirectory() as t:
                ok, msg = _check(mod, _fixture(t, tag=tag, age_min=age))
                assert ok, (tag, age, msg)


def test_both_review_kind_tags_are_accepted():
    """Pins the NOT-REPRODUCED half of the v08 filing.

    The second filing blamed the gate for looking up `adversarial-impl` while
    the skill documents `adversarial`. Both are accepted, and were at the time
    of the observation -- the refusal was the TTL above.
    """
    mod = _load()
    for tag in ("adversarial", "adversarial-impl"):
        with tempfile.TemporaryDirectory() as t:
            ok, msg = _check(mod, _fixture(t, tag=tag, age_min=5))
            assert ok, (tag, msg)


def test_drifted_source_now_refuses_however_fresh():
    """The strengthening. Age never invalidated the right thing; drift does."""
    mod = _load()
    with tempfile.TemporaryDirectory() as t:
        ok, msg = _check(mod, _fixture(t, age_min=1, drift=True))
        assert not ok, "a stamp over CHANGED source must not satisfy step 13"
        assert "content-STALE" in msg and "source changed" in msg, msg


def test_legacy_entry_without_a_head_still_uses_the_wall_clock():
    """No head recorded -> nothing to bind to -> the TTL is all there is."""
    mod = _load()
    with tempfile.TemporaryDirectory() as t:
        ok, _ = _check(mod, _fixture(t, age_min=5, record_head=False))
        assert ok
    with tempfile.TemporaryDirectory() as t:
        ok, msg = _check(mod, _fixture(t, age_min=33, record_head=False))
        assert not ok
        assert "no recorded head" in msg and "33 min old" in msg, msg


def test_section_binding_still_fails_closed():
    mod = _load()
    with tempfile.TemporaryDirectory() as t:
        ok, msg = _check(mod, _fixture(t, age_min=5, section="19"))
        assert not ok
        assert ("bound to " + SEC + "19") in msg, msg


def test_refusal_names_the_actual_reason():
    """The misdirection is the expensive part, not the refusal.

    Every rejection cause used to print the same "missing <todo> <section>", so
    an expired-but-present stamp read as "you never dispatched" -- which is what
    sent the run re-dispatching a closed review and produced the wrong second
    filing. Each cause must now be distinguishable in the text.
    """
    mod = _load()
    seen = []
    for kw in ({"drift": True}, {"record_head": False, "age_min": 33},
               {"section": "19"}):
        with tempfile.TemporaryDirectory() as t:
            ok, msg = _check(mod, _fixture(t, **kw))
            assert not ok, kw
            seen.append(msg)
    assert len({m.split(SEC + "22: ")[-1] for m in seen}) == 3, seen


if __name__ == "__main__":
    test_head_bound_evidence_does_not_age_out()
    test_both_review_kind_tags_are_accepted()
    test_drifted_source_now_refuses_however_fresh()
    test_legacy_entry_without_a_head_still_uses_the_wall_clock()
    test_section_binding_still_fails_closed()
    test_refusal_names_the_actual_reason()
    print("PASS: step-13 evidence is content-bound, and the refusal says why")
