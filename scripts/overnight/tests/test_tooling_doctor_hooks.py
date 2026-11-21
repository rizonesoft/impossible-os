"""tooling-doctor.sh reads install-hooks.sh --status by its labels; a renamed label must not drop a row."""
import json
import shutil
import subprocess
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
HOOKS = ("pre-commit", "prepare-commit-msg", "commit-msg", "post-commit", "pre-push")


def _env():
    """The caller's git settings never reach a fixture repository."""
    import os
    env = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}
    env.update({"GIT_CONFIG_GLOBAL": os.devnull, "GIT_CONFIG_NOSYSTEM": "1"})
    return env


def _hooks(root):
    r = subprocess.run(["bash", str(root / "scripts/tooling-doctor.sh"), "--json", "--group=hooks"], cwd=root,
                       capture_output=True, text=True, timeout=120)
    return {c["name"]: c for c in json.loads(r.stdout)["checks"] if c["group"] == "hooks"}


def test_the_doctor_reports_the_pre_push_build_step_and_every_hook():
    checks = _hooks(REPO)
    step = checks.get("pre-push build/test step")
    assert step and step["status"] == "pass" and step["detail"].startswith(("ENABLED", "disabled")), checks
    shipped = {p.name for p in (REPO / ".githooks").iterdir() if p.is_file()}
    assert shipped == set(HOOKS), shipped
    for hook in HOOKS:
        assert checks[f".githooks/{hook}"]["status"] == "pass", hook


def test_a_hook_that_cannot_run_is_reported(tmp_path):
    root = tmp_path / "r"
    (root / "scripts").mkdir(parents=True)
    for name in ("tooling-doctor.sh", "install-hooks.sh"):
        shutil.copy(REPO / "scripts" / name, root / "scripts" / name)
    shutil.copytree(REPO / ".githooks", root / ".githooks")
    (root / ".githooks/prepare-commit-msg").chmod(0o644)
    assert _hooks(root)[".githooks/prepare-commit-msg"]["status"] != "pass"


def test_the_build_step_sentinel_does_not_depend_on_the_callers_directory(tmp_path):
    root = tmp_path / "r"
    (root / "scripts").mkdir(parents=True)
    (root / "docs").mkdir()
    shutil.copy(REPO / "scripts/install-hooks.sh", root / "scripts/install-hooks.sh")
    shutil.copytree(REPO / ".githooks", root / ".githooks")
    env = _env()
    subprocess.run(["git", "init", "-q"], cwd=root, check=True, timeout=30, env=env)
    run = subprocess.run(["bash", "../scripts/install-hooks.sh", "--enable-pre-push"], cwd=root / "docs",
                         capture_output=True, text=True, timeout=60, env=env)
    assert run.returncode == 0, run
    assert (root / ".git/.impossible-os-prepush").is_file() and not (root / "docs/.git").exists()
    status = subprocess.run(["bash", "scripts/install-hooks.sh", "--status"], cwd=root, capture_output=True,
                            text=True, timeout=60, env=env)
    assert "Pre-push build/test step: ENABLED" in status.stdout, status.stdout
    shutil.copy(REPO / "scripts/tooling-doctor.sh", root / "scripts/tooling-doctor.sh")
    step = _hooks(root)["pre-push build/test step"]
    assert step["detail"] == f"ENABLED (sentinel: {root / '.git/.impossible-os-prepush'})", step


def test_remove_prunes_a_legacy_symlink_while_hooks_path_is_set(tmp_path):
    root = tmp_path / "r"
    (root / "scripts/hooks").mkdir(parents=True)
    shutil.copy(REPO / "scripts/install-hooks.sh", root / "scripts/install-hooks.sh")
    (root / "scripts/hooks/pre-push").write_text("#!/bin/sh\n")
    shutil.copytree(REPO / ".githooks", root / ".githooks")
    env = _env()
    subprocess.run(["git", "init", "-q"], cwd=root, check=True, timeout=30, env=env)
    subprocess.run(["git", "config", "core.hooksPath", ".githooks"], cwd=root, check=True, timeout=30, env=env)
    (root / "scripts/hooks/heavy.sh").write_text("#!/bin/sh\n")
    legacy = root / ".git/hooks/pre-push"
    legacy.parent.mkdir(exist_ok=True)
    legacy.symlink_to(root / "scripts/hooks/pre-push")
    theirs = root / ".git/hooks/post-commit"
    (tmp_path / "operator-hook").write_text("#!/bin/sh\n")
    theirs.symlink_to(tmp_path / "operator-hook")
    helper = root / ".git/hooks/heavy.sh"            # a link to a current helper the legacy installer never shipped
    helper.symlink_to(root / "scripts/hooks/heavy.sh")
    r = subprocess.run(["bash", "scripts/install-hooks.sh", "--remove"], cwd=root, capture_output=True, text=True,
                       timeout=60, env=env)
    assert r.returncode == 0, r
    assert not legacy.is_symlink(), r.stdout
    assert theirs.is_symlink(), "a link to the operator's own hook is not ours to remove"
    assert helper.is_symlink(), "the legacy installer linked pre-push only"


def test_remove_from_a_linked_worktree_prunes_the_primary_checkouts_legacy_link(tmp_path):
    root = tmp_path / "r"
    (root / "scripts/hooks").mkdir(parents=True)
    shutil.copy(REPO / "scripts/install-hooks.sh", root / "scripts/install-hooks.sh")
    (root / "scripts/hooks/pre-push").write_text("#!/bin/sh\n")
    shutil.copytree(REPO / ".githooks", root / ".githooks")
    env = _env()
    for cmd in (["init", "-q"], ["add", "-A"], ["-c", "user.name=t", "-c", "user.email=t@t", "commit", "-q",
                "-m", "x"], ["worktree", "add", "-q", str(tmp_path / "wt")],
                ["config", "core.hooksPath", ".githooks"]):
        subprocess.run(["git", *cmd], cwd=root, check=True, capture_output=True, timeout=30, env=env)
    legacy = root / ".git/hooks/pre-push"
    legacy.parent.mkdir(exist_ok=True)
    legacy.symlink_to(root / "scripts/hooks/pre-push")
    r = subprocess.run(["bash", "scripts/install-hooks.sh", "--remove"], cwd=tmp_path / "wt", capture_output=True,
                       text=True, timeout=60, env=env)
    assert r.returncode == 0, r
    assert not legacy.is_symlink(), r.stdout
    assert (root / ".githooks/pre-push").is_file()
