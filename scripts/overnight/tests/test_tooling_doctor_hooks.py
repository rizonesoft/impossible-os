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


def _plain_repo(tmp_path):
    root = tmp_path / "p"
    (root / "scripts/hooks").mkdir(parents=True)
    shutil.copy(REPO / "scripts/install-hooks.sh", root / "scripts/install-hooks.sh")
    (root / "scripts/hooks/pre-push").write_text("#!/bin/sh\n")
    shutil.copytree(REPO / ".githooks", root / ".githooks")
    subprocess.run(["git", "init", "-q"], cwd=root, check=True, capture_output=True, timeout=30, env=_env())
    return root


def test_remove_keeps_a_dangling_operator_link_and_finishes(tmp_path):
    root = _plain_repo(tmp_path)
    subprocess.run(["git", "config", "core.hooksPath", ".githooks"], cwd=root, check=True, timeout=30, env=_env())
    link = root / ".git/hooks/pre-push"
    link.parent.mkdir(exist_ok=True)
    link.symlink_to(tmp_path / "gone/dir/pre-push")
    r = subprocess.run(["bash", "scripts/install-hooks.sh", "--remove"], cwd=root, capture_output=True, text=True,
                       timeout=60, env=_env())
    assert r.returncode == 0 and "Git hooks removed" in r.stdout, r
    assert link.is_symlink()


def test_remove_leaves_a_hooks_path_it_did_not_write(tmp_path):
    root = _plain_repo(tmp_path)
    subprocess.run(["git", "config", "core.hooksPath", "my-hooks"], cwd=root, check=True, timeout=30, env=_env())
    r = subprocess.run(["bash", "scripts/install-hooks.sh", "--remove"], cwd=root, capture_output=True, text=True,
                       timeout=60, env=_env())
    assert r.returncode == 0 and "left as it is" in r.stdout, r
    got = subprocess.run(["git", "config", "--get", "core.hooksPath"], cwd=root, capture_output=True, text=True,
                         timeout=30, env=_env()).stdout.strip()
    assert got == "my-hooks"


def _cfg(root, *a, check=True):
    return subprocess.run(["git", "config", *a], cwd=root, capture_output=True, text=True, timeout=30, env=_env(),
                          check=check)


def test_remove_keeps_a_link_and_a_value_that_only_read_alike(tmp_path):
    """A target or value ending in a newline is another path or value: shell stripping must not make it ours."""
    root = _plain_repo(tmp_path)
    wt = subprocess.run(["git", "rev-parse", "--show-toplevel"], cwd=root, capture_output=True, text=True,
                        timeout=30, env=_env()).stdout.strip()
    link = root / ".git/hooks/pre-push"
    link.parent.mkdir(exist_ok=True)
    link.symlink_to(wt + "/scripts/hooks/pre-push\n")
    _cfg(root, "core.hooksPath", ".githooks\n")
    r = subprocess.run(["bash", "scripts/install-hooks.sh", "--remove"], cwd=root, capture_output=True, text=True,
                       timeout=60, env=_env())
    assert r.returncode == 0, r
    assert link.is_symlink() and _cfg(root, "--get", "core.hooksPath").stdout == ".githooks\n\n"


def test_remove_says_when_an_inherited_value_still_runs_the_hooks(tmp_path):
    root = _plain_repo(tmp_path)
    glob = tmp_path / "global.cfg"
    glob.write_text("[core]\n\thooksPath = .githooks\n")
    env = dict(_env(), GIT_CONFIG_GLOBAL=str(glob))
    r = subprocess.run(["bash", "scripts/install-hooks.sh", "--remove"], cwd=root, capture_output=True, text=True,
                       timeout=60, env=env)
    assert r.returncode == 0 and "still runs .githooks/" in r.stdout and "global" in r.stdout, r
    assert "inert" not in r.stdout


def test_install_writes_the_scope_git_reads_the_hooks_path_from(tmp_path):
    """A worktree-scoped value outranks the repository's: install writes that one and checks the result."""
    root = _plain_repo(tmp_path)
    _cfg(root, "extensions.worktreeConfig", "true")
    _cfg(root, "--worktree", "core.hooksPath", "other-hooks")
    r = subprocess.run(["bash", "scripts/install-hooks.sh"], cwd=root, capture_output=True, text=True, timeout=60,
                       env=_env())
    assert r.returncode == 0 and "Configured: core.hooksPath = .githooks" in r.stdout, r
    assert _cfg(root, "--get", "core.hooksPath").stdout.strip() == ".githooks"
    assert _cfg(root, "--worktree", "--get", "core.hooksPath").stdout.strip() == ".githooks"


def test_install_is_judged_by_the_value_git_uses_not_any_value_it_holds(tmp_path):
    """.githooks in the repository's config under a worktree override: git runs the override, so install acts."""
    root = _plain_repo(tmp_path)
    _cfg(root, "core.hooksPath", ".githooks")
    _cfg(root, "extensions.worktreeConfig", "true")
    _cfg(root, "--worktree", "core.hooksPath", "/custom")
    r = subprocess.run(["bash", "scripts/install-hooks.sh"], cwd=root, capture_output=True, text=True, timeout=60,
                       env=_env())
    assert r.returncode == 0 and "Configured: core.hooksPath = .githooks" in r.stdout, r
    hooks = subprocess.run(["git", "rev-parse", "--git-path", "hooks"], cwd=root, capture_output=True, text=True,
                           timeout=30, env=_env()).stdout.strip()
    assert hooks == ".githooks", hooks


def test_remove_undoes_a_worktree_scoped_install(tmp_path):
    root = _plain_repo(tmp_path)
    _cfg(root, "extensions.worktreeConfig", "true")
    _cfg(root, "--worktree", "core.hooksPath", "/custom")
    subprocess.run(["bash", "scripts/install-hooks.sh"], cwd=root, check=True, capture_output=True, timeout=60,
                   env=_env())
    assert _cfg(root, "--worktree", "--get", "core.hooksPath").stdout.strip() == ".githooks"
    r = subprocess.run(["bash", "scripts/install-hooks.sh", "--remove"], cwd=root, capture_output=True, text=True,
                       timeout=60, env=_env())
    assert r.returncode == 0 and "inert" in r.stdout, r
    assert _cfg(root, "--get", "core.hooksPath", check=False).returncode == 1


def test_status_and_the_doctor_show_a_value_that_only_reads_as_githooks(tmp_path):
    root = _plain_repo(tmp_path)
    shutil.copy(REPO / "scripts/tooling-doctor.sh", root / "scripts/tooling-doctor.sh")
    _cfg(root, "core.hooksPath", ".githooks\n")
    status = subprocess.run(["bash", "scripts/install-hooks.sh", "--status"], cwd=root, capture_output=True,
                            text=True, timeout=60, env=_env())
    assert "Hooks path     : $'.githooks\\n' (expected: .githooks)" in status.stdout, status.stdout
    r = subprocess.run(["bash", str(root / "scripts/tooling-doctor.sh"), "--json", "--group=hooks"], cwd=root,
                       capture_output=True, text=True, timeout=120, env=_env())
    check = {c["name"]: c for c in json.loads(r.stdout)["checks"]}["core.hooksPath"]
    assert check["status"] != "pass", check


def test_remove_prunes_the_legacy_link_of_a_worktree_whose_path_holds_a_newline(tmp_path):
    root = _plain_repo(tmp_path)
    env = _env()
    for a in (["config", "user.name", "t"], ["config", "user.email", "t@t"], ["add", "-A"], ["commit", "-qm", "x"]):
        subprocess.run(["git", *a], cwd=root, check=True, capture_output=True, timeout=30, env=env)
    wt = tmp_path / "w\nt"
    subprocess.run(["git", "worktree", "add", "-q", "--detach", str(wt)], cwd=root, check=True, capture_output=True, timeout=30,
                   env=env)
    link = root / ".git/hooks/pre-push"
    link.parent.mkdir(exist_ok=True)
    link.symlink_to(wt / "scripts/hooks/pre-push")
    r = subprocess.run(["bash", "scripts/install-hooks.sh", "--remove"], cwd=root, capture_output=True, text=True,
                       timeout=60, env=env)
    assert r.returncode == 0 and not link.is_symlink(), r
