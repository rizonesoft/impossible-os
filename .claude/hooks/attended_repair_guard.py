#!/usr/bin/env python3
# block-via: exit 2 (interactive session only; the unattended run is never gated)
"""PreToolUse(Bash): protect a LIVE unattended run's working tree from the
attended session repairing alongside it.

The overnight run executes the LIVE WORKING TREE, so an interactive operator
session and the run share one tree and one index. That sharing is deliberate --
it is what makes attended repair of the control plane possible at all, since the
run is forbidden from editing `.claude/hooks/**`, `.claude/skills/**`,
`scripts/overnight/**`, `.githooks/**`, `.claude/settings.json` and the receipt
surface itself and can only FILE those findings into the newest
`todo/overnight-runner-improvements/` and `todo/token-saver/` capture files. The
attended session is the half that applies them.

What it must never do is reach for a whole-tree git verb. Observed live
2026-07-28 (recorded in `.claude/state/live-gotchas.md`): a `git add -A` swept
the OPERATOR's uncommitted `token-saver-v03.md` into the RUN's index. The
converse is worse and is what this hook exists for -- an operator-side `git add
-A` / `git commit -a` sweeps the run's half-finished section into an unrelated
commit, and `git stash` / `git reset --hard` / `git checkout .` / `git clean -fd`
DESTROY a section's uncommitted work outright, with no reviewer and no receipt.

So: while a run is live, whole-tree staging and tree-destroying verbs are
blocked in the attended session. Explicit paths (`git add path/a path/b`,
`git add -u todo/`) always pass -- they are the prescribed shape, not a
grudging exception.

STAGING EXPLICIT PATHS IS NOT ENOUGH, and this hook's first version wrongly
implied it was. The INDEX is shared too: on 2026-07-31 an attended commit that
had `git add`-ed only its own four files still swept the run's already-staged
TODO-04 section deferral into an unrelated commit, because `git commit` takes
the whole index and nothing in `git add a b c` un-stages what another session
put there. The immune shape is a PATH-LIMITED commit (`git commit -m ... --
<paths>`), which ignores the index for every path it does not name, so that is
what this hook requires while a run is live.

Scope, deliberately narrow:
  - INTERACTIVE ONLY. `OVERNIGHT_SEQUENCER_RUN=1` returns 0 immediately, so this
    hook is structurally incapable of wedging the unattended run it protects.
    (The run has its own boundary: run_phase_guard + receipt_surface_guard.)
  - LIVE RUN ONLY. With no active `.claude/state/sequencer-run.json`, ordinary
    solo work is untouched -- `git add -A` on a quiet tree is fine and common.

Detection reuses the shlex tokenizer + control-operator segmentation from
`_codex_dispatch` (NOT raw regex), so `git -C /path add -A`, an env prefix, and
`cmd && git stash` are all seen. Fail-open on any error.

Opt-out: `ATTENDED_REPAIR_OVERRIDE=1` (process env, or as the env prefix of the
command -- it must prefix the command it applies to, not follow an `&&`).
"""
from __future__ import annotations

import json
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _codex_dispatch as _cd  # noqa: E402

_MAX_DEPTH = 5
_SHELLS = {"bash", "sh", "zsh", "dash", "ksh"}
_GIT_VALUE_OPTS = {"-C", "-c", "--config-env", "--git-dir", "--work-tree",
                   "--namespace", "--exec-path"}
# Pathspecs that mean "the whole tree" regardless of cwd.
_WHOLE_TREE = {".", "-A", "--all", "-a", ":/", "*", "./"}


def _repo_root() -> str:
    return os.environ.get("CLAUDE_PROJECT_DIR") or os.getcwd()


def _canonical_run_root() -> str:
    """The PRIMARY worktree, which is where the run always lives.

    Required once a second worktree exists (2026-07-31). `.claude/state/*` is
    gitignored and therefore PER-WORKTREE, so an operator session working from
    a repair worktree would find no `sequencer-run.json` in its own root, this
    guard would silently conclude "no run is active", and it would go inert
    exactly when it is needed. Resolving the primary worktree instead keeps the
    guard bound to the run wherever the operator happens to be standing.

    `--git-common-dir` is the shared `.git` of the primary worktree (a linked
    worktree's own `.git` is a FILE pointing there), so its parent is the
    primary checkout. Falls back to this root on any error -- the pre-worktree
    behaviour, which is correct when there is only one worktree.
    """
    try:
        out = subprocess.run(
            ["git", "-C", _repo_root(), "rev-parse", "--path-format=absolute",
             "--git-common-dir"],
            capture_output=True, text=True, timeout=5)
        common = (out.stdout or "").strip()
        if out.returncode == 0 and common:
            root = os.path.dirname(common.rstrip("/"))
            if root and os.path.isdir(root):
                return root
    except Exception:
        pass
    return _repo_root()


def _run_state() -> dict | None:
    """The live run's phase-guard state, or None when no run is active.

    A run is live only while BOTH its state says `active` AND the armed marker
    exists. The marker is written at arm and removed by every stop path
    (--disarm, the launcher's deadline and fixpoint stops), whereas `active`
    outlived a finished run: observed 2026-09-29, the deadline stop left it
    true and this guard kept blocking git verbs for hours with nothing running.
    """
    state_dir = os.path.join(_canonical_run_root(), ".claude", "state")
    if not os.path.exists(os.path.join(state_dir, "sequencer-armed")):
        return None
    try:
        with open(os.path.join(state_dir, "sequencer-run.json"), "r",
                  encoding="utf-8") as fh:
            d = json.load(fh)
    except Exception:
        return None
    if isinstance(d, dict) and d.get("active") is True:
        return d
    return None


def _base(tok: str) -> str:
    return os.path.basename(tok.rstrip("/"))


def _git_verdict(seg: list[str]) -> str:
    """seg[0] is git. Walk global options, then judge the subcommand."""
    i = 1
    while i < len(seg):
        tok = seg[i]
        if tok in _GIT_VALUE_OPTS:
            i += 2
            continue
        if tok.startswith("-"):
            i += 1
            continue
        break
    if i >= len(seg):
        return ""
    sub = seg[i]
    rest = seg[i + 1:]

    if sub == "add":
        # Explicit paths are the prescribed shape. Block only the whole-tree
        # forms -- including a BARE `git add -u` (every tracked file), while
        # `git add -u todo/` is path-scoped and passes.
        flags = [r for r in rest if r.startswith("-")]
        paths = [r for r in rest if not r.startswith("-")]
        if any(f in _WHOLE_TREE for f in flags):
            return "git add -A/--all (stages the run's in-flight section too)"
        if any(p in _WHOLE_TREE for p in paths):
            return "git add . (stages the run's in-flight section too)"
        if not paths:
            return "git add with no pathspec (stages the whole tree)"
        return ""

    if sub == "commit":
        for r in rest:
            if r in ("-a", "--all") or (
                r.startswith("-") and not r.startswith("--")
                and "a" in r.lstrip("-") and "=" not in r
            ):
                return "git commit -a (commits the run's unstaged work)"
        # A commit takes whatever is in the INDEX, and the index is shared. On
        # 2026-07-31 an attended commit that had `git add`-ed only its own four
        # files still swept the run's already-staged TODO-04 edit (a section
        # deferral: an IO-table [ ]->[/] flip plus a Deferred stamp) into an
        # unrelated commit message. Staging explicit paths is NOT sufficient
        # protection -- the run may have staged its own work first, and nothing
        # in `git add a b c` un-stages it.
        #
        # The shape that IS immune is a path-limited commit: `git commit -m ...
        # -- <paths>` ignores the index for every path not named. So while a run
        # is live, require it.
        if "--" not in rest:
            return ("git commit without a `-- <paths>` limiter (commits whatever "
                    "the run has staged, not just your files)")
        return ""

    if sub == "stash":
        if rest and rest[0] in ("list", "show"):
            return ""
        return "git stash (removes the run's uncommitted section work)"

    if sub == "reset":
        if any(r in ("--hard", "--merge", "--keep") for r in rest):
            return "git reset --hard (destroys the run's uncommitted work)"
        return ""

    if sub in ("checkout", "restore"):
        if any(p in _WHOLE_TREE for p in rest if not p.startswith("-")):
            return f"git {sub} . (discards the run's uncommitted work)"
        return ""

    if sub == "clean":
        if any(r.startswith("-") and ("f" in r.lstrip("-") or r == "--force")
               for r in rest):
            return "git clean -f (deletes the run's untracked artifacts)"
        return ""

    return ""


def _scan(cmd: str, depth: int = 0) -> str:
    if depth > _MAX_DEPTH:
        return ""
    try:
        tokens = _cd._tokenize(cmd)
    except Exception:
        tokens = None
    if tokens is None:
        return ""
    for seg in _cd._segment_by_separators(tokens):
        for tok in seg:
            if tok == "ATTENDED_REPAIR_OVERRIDE=1":
                return ""
        seg = _cd._strip_env_and_wrappers(list(seg))
        if not seg:
            continue
        head = _base(seg[0])
        if head in _SHELLS:
            for j, tok in enumerate(seg[1:], start=1):
                if tok.startswith("-") and "c" in tok.lstrip("-") and j + 1 <= len(seg) - 1:
                    label = _scan(seg[j + 1], depth + 1)
                    if label:
                        return label
                    break
            continue
        if head == "git":
            label = _git_verdict(seg)
            if label:
                return label
    return ""


def main() -> int:
    # Structurally inert inside the unattended run: it has its own boundary,
    # and a guard that could block the run it protects is a wedge waiting to
    # happen.
    if os.environ.get("OVERNIGHT_SEQUENCER_RUN") == "1":
        return 0
    if os.environ.get("ATTENDED_REPAIR_OVERRIDE") == "1":
        return 0
    try:
        d = json.load(sys.stdin)
    except Exception:
        return 0
    if not isinstance(d, dict) or d.get("tool_name") != "Bash":
        return 0
    ti = d.get("tool_input")
    if not isinstance(ti, dict):
        return 0
    cmd = str(ti.get("command") or "")
    if not cmd:
        return 0
    state = _run_state()
    if state is None:
        return 0          # no live run: ordinary solo work, never gated
    label = _scan(cmd)
    if not label:
        return 0
    where = state.get("file") or "(unknown file)"
    sec = state.get("section_idx")
    phase = state.get("phase") or "?"
    sys.stderr.write(
        f"[attended-repair BLOCK] {label}.\n"
        f"  An unattended run is LIVE and shares this working tree "
        f"(phase {phase}, {where}"
        + (f" section {sec}" if sec is not None else "")
        + ").\n"
        "  Stage EXPLICIT paths instead: `git add <path> [<path> ...]` or "
        "`git add -u <dir>`.\n"
        "  Land attended repairs at a rollover boundary, commit + push them "
        "promptly, and leave no dirt behind -- uncommitted dirt blocks the "
        "run's next ship rollover.\n"
        "  A change you cannot make safely goes into the newest "
        "todo/overnight-runner-improvements/ (flow, gates) or "
        "todo/token-saver/ (cost) file instead.\n"
        "  Override for this call: ATTENDED_REPAIR_OVERRIDE=1 <command>\n"
    )
    return 2


def _selftest() -> int:
    cases = [
        ("git add -A", True),
        ("git add --all", True),
        ("git add .", True),
        ("git add -u", True),
        ("git -C /repo add -A", True),
        ("git add -u todo/", False),
        ("git add todo/a.md todo/b.md", False),
        ("git add -- todo/a.md", False),
        ("git commit -am 'x'", True),
        ("git commit -a", True),
        # index-sweep: a commit with no path limiter takes the run's staged work
        ("git commit -m 'x'", True),
        ("git commit --amend -m 'x'", True),
        ("git commit -m 'x' -- a.py b.py", False),
        ("git commit -F - -- .claude/hooks/x.py", False),
        ("git stash", True),
        ("git stash push -m wip", True),
        ("git stash list", False),
        ("git reset --hard origin/main", True),
        ("git reset HEAD~1", False),
        ("git checkout .", True),
        ("git restore .", True),
        ("git checkout -b feature", False),
        ("git clean -fd", True),
        ("git clean -n", False),
        ("git status --short", False),
        ("git push origin main", False),
        ("echo hi && git add -A", True),
        ("bash -c 'git add -A'", True),
        ("ATTENDED_REPAIR_OVERRIDE=1 git add -A", False),
        ("ls -la", False),
    ]
    bad = 0
    for cmd, want_block in cases:
        got = bool(_scan(cmd))
        if got != want_block:
            bad += 1
            print(f"FAIL: {cmd!r} -> block={got}, want={want_block}")
    print(f"{len(cases) - bad}/{len(cases)} cases passed")
    return 1 if bad else 0


if __name__ == "__main__":
    if "--selftest" in sys.argv:
        sys.exit(_selftest())
    try:
        sys.exit(main())
    except Exception:
        sys.exit(0)  # fail-open: a broken guard must never block real work
