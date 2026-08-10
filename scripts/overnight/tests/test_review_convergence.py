#!/usr/bin/env python3
"""P2.1/P2.2 convergence gate: never redispatch an unchanged kind on unchanged
relevant inputs, scoped per kind. Runs the hook's own selftest plus CLI-level
exit-code checks (0 = REDISPATCH, 1 = CONVERGED)."""
import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent.parent.parent
HOOK = REPO / ".claude/hooks/review_convergence.py"


def test_selftest_passes():
    r = subprocess.run([sys.executable, str(HOOK), "--selftest"],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr


def _repo(d):
    root = pathlib.Path(d)
    (root / "src/kernel").mkdir(parents=True)
    (root / "todo").mkdir()
    subprocess.run(["git", "init", "-q", str(root)], check=True)
    (root / "src/kernel/a.c").write_text("int a(void){return 1;}\n")
    (root / "todo/TODO-01.md").write_text("# t\n")
    subprocess.run(["git", "-C", str(root), "add", "-A"], check=True)
    subprocess.run(["git", "-C", str(root), "-c", "user.email=t@t",
                    "-c", "user.name=t", "commit", "-qm", "i"],
                   check=True, capture_output=True)
    return root


def _cli(root, *args):
    return subprocess.run([sys.executable, str(HOOK), *args],
                          cwd=str(root), capture_output=True, text=True)


def test_cli_record_then_converged_then_redispatch():
    with tempfile.TemporaryDirectory() as d:
        root = _repo(d)
        sl = "todo/TODO-01.md#1"
        # First: no prior verdict -> REDISPATCH (exit 0).
        assert _cli(root, "should-redispatch", sl, "adversarial").returncode == 0
        # Record the verdict, then unchanged inputs -> CONVERGED (exit 1).
        assert _cli(root, "record", sl, "adversarial").returncode == 0
        assert _cli(root, "should-redispatch", sl, "adversarial").returncode == 1
        # Edit source -> REDISPATCH (exit 0).
        (root / "src/kernel/a.c").write_text("int a(void){return 2;}\n")
        assert _cli(root, "should-redispatch", sl, "adversarial").returncode == 0


def test_every_review_kind_the_repo_uses_is_scoped():
    """token-saver v02 (2026-07-28): the canary suppressed 0 of 2 rounds, and one
    of the four all-time redispatch records reads "unknown kind 'test-coverage'
    -- fail-open to redispatch". The gate was not mis-keyed; the kind was simply
    absent from KIND_SCOPES, and an absent kind fails open FOREVER (it can never
    be fingerprinted, so it always redispatches).

    Re-derive the live vocabulary from the tree rather than hard-coding it, so
    the next kind someone introduces fails HERE instead of silently disabling
    convergence for itself."""
    import importlib.util
    import re
    repo = HOOK.parents[2]
    # Metasyntactic stand-ins from usage strings and templates, e.g. the
    # broker's `Usage: $0 '<[review-kind: X] todo-path body>'`. These are not
    # kinds; everything else the scan finds must be scoped.
    PLACEHOLDERS = {"x", "kind", "n"}
    used = set()
    for sub in (".claude/skills", ".claude/hooks", "scripts"):
        for p in (repo / sub).rglob("*"):
            if p.suffix not in (".md", ".py", ".sh") or not p.is_file():
                continue
            try:
                text = p.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            used.update(m.lower() for m in
                        re.findall(r"review-kind:\s*([a-zA-Z-]+)", text))
    spec = importlib.util.spec_from_file_location("rc_kinds", HOOK)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    missing = sorted(used - set(mod.KIND_SCOPES) - PLACEHOLDERS)
    assert not missing, (
        f"review kinds used in the repo but absent from KIND_SCOPES: {missing}. "
        "An unscoped kind fails open forever -- the gate can never suppress it. "
        "Add it with a scope that errs WIDE (a wider scope only causes more "
        "redispatch; a narrow one can skip a review that was needed).")
    assert used, "found no `review-kind:` usages -- the scan is broken, not clean"


def _rc_mod():
    import importlib.util
    spec = importlib.util.spec_from_file_location("rc_mod", HOOK)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def test_tooling_sections_are_visible_to_every_kind():
    """v13 carry, fixed 2026-08-10. A section whose whole surface is
    `scripts/**`, `.claude/**` or `docs/**` used to fingerprint identically no
    matter what it changed, so the gate answered CONVERGED on round one and
    every round after -- not because the review found nothing but because it
    never looked. Measured cost on the metadata-layer TODO's
    shared-cache-schema section: five valid [medium]
    findings that only appeared because the run dispatched AGAINST the advice."""
    m = _rc_mod()
    assert all("tooling" in v for v in m.KIND_SCOPES.values()), \
        "a kind that cannot see host-side tooling converges such sections forever"
    probe = REPO / "scripts/.conv-scope-probe.tmp"
    before = m._fingerprint(REPO, ("src", "tooling"))
    probe.write_text("probe")
    try:
        after = m._fingerprint(REPO, ("src", "tooling"))
    finally:
        probe.unlink(missing_ok=True)
    assert before and after and before != after, \
        "a scripts/ change must move the fingerprint, or the gate is blind to it"


def test_unchanged_tree_still_converges():
    """REFUSAL CONTROL. Widening the scope must not make the gate unable to
    converge -- a gate that always redispatches is as useless as one that never
    does, just in the expensive direction."""
    m = _rc_mod()
    a = m._fingerprint(REPO, ("src", "tooling"))
    b = m._fingerprint(REPO, ("src", "tooling"))
    assert a is not None and a == b, "an unchanged tree must fingerprint stably"


def test_unknown_scope_fails_open():
    """REFUSAL CONTROL, and a latent bug found while fixing the filed one. The
    resolver was `SRC_PATHS if s == "src" else TODO_PATHS`, so ANY unrecognised
    scope name silently fingerprinted against the TODO corpus -- converging a
    kind on grounds unrelated to it. Unknown scopes must return None, which the
    caller reads as REDISPATCH."""
    m = _rc_mod()
    assert m._fingerprint(REPO, ("no-such-scope",)) is None
    assert m._fingerprint(REPO, ("src", "no-such-scope")) is None
    # And every scope actually referenced by KIND_SCOPES must resolve.
    for kind, scopes in m.KIND_SCOPES.items():
        for s in scopes:
            assert s in m.SCOPE_PATHS, f"{kind} names unresolvable scope {s!r}"


if __name__ == "__main__":
    test_selftest_passes()
    test_cli_record_then_converged_then_redispatch()
    test_every_review_kind_the_repo_uses_is_scoped()
    test_tooling_sections_are_visible_to_every_kind()
    test_unchanged_tree_still_converges()
    test_unknown_scope_fails_open()
    print("PASS: review-convergence P2.1/P2.2 + kind coverage")
