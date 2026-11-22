"""tooling-doctor.sh reads install-hooks.sh --status by its labels; a renamed label must not drop a row."""
import json
import os
import shutil
import signal
import subprocess
import time
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
    """A global value is beneath the repository's: install writes the repository's, git reads it, and removing
    it gives the global value back untouched."""
    root = _plain_repo(tmp_path)
    glob = tmp_path / "global.cfg"
    glob.write_text("[core]\n\thooksPath = /global\n")
    env = {**_env(), "GIT_CONFIG_GLOBAL": str(glob)}
    r = subprocess.run(["bash", "scripts/install-hooks.sh"], cwd=root, capture_output=True, text=True, timeout=60,
                       env=env)
    assert r.returncode == 0 and "Configured: core.hooksPath = .githooks" in r.stdout, r
    assert _cfg(root, "--local", "--get", "core.hooksPath").stdout == ".githooks\n"
    r = subprocess.run(["bash", "scripts/install-hooks.sh", "--remove"], cwd=root, capture_output=True, text=True,
                       timeout=60, env=env)
    assert r.returncode == 0, r
    assert glob.read_text() == "[core]\n\thooksPath = /global\n"
    assert _cfg(root, "--local", "--get", "core.hooksPath", check=False).returncode == 1


def test_install_is_judged_by_the_value_git_uses_not_any_value_it_holds(tmp_path):
    """.githooks in the repository's config under a worktree override: git runs the override, so install does not
    report it configured; the override is the operator's, so it refuses and writes nothing."""
    root = _plain_repo(tmp_path)
    _cfg(root, "core.hooksPath", ".githooks")
    _cfg(root, "extensions.worktreeConfig", "true")
    _cfg(root, "--worktree", "core.hooksPath", "/custom")
    r = subprocess.run(["bash", "scripts/install-hooks.sh"], cwd=root, capture_output=True, text=True, timeout=60,
                       env=_env())
    assert r.returncode != 0 and "Already configured" not in r.stdout and "already set in worktree config" in r.stderr, r
    hooks = subprocess.run(["git", "rev-parse", "--git-path", "hooks"], cwd=root, capture_output=True, text=True,
                           timeout=30, env=_env()).stdout.strip()
    assert hooks == "/custom", hooks


def test_remove_undoes_a_worktree_scoped_install(tmp_path):
    root = _plain_repo(tmp_path)
    _cfg(root, "extensions.worktreeConfig", "true")
    _cfg(root, "--worktree", "core.hooksPath", "/custom")
    # The worktree's own value is the operator's: the install refuses rather than write beside it.
    r = subprocess.run(["bash", "scripts/install-hooks.sh"], cwd=root, capture_output=True, text=True, timeout=60,
                       env=_env())
    assert r.returncode != 0 and "already set in worktree config" in r.stderr, r
    assert _cfg(root, "--worktree", "--get-all", "core.hooksPath").stdout == "/custom\n"
    _cfg(root, "--worktree", "--unset", "core.hooksPath")
    _cfg(root, "--local", "core.hooksPath", "/shared")
    _cfg(root, "--worktree", "core.hooksPath", ".githooks")
    r = subprocess.run(["bash", "scripts/install-hooks.sh", "--remove"], cwd=root, capture_output=True, text=True,
                       timeout=60, env=_env())
    assert r.returncode == 0 and "inert" in r.stdout, r
    assert _cfg(root, "--worktree", "--get", "core.hooksPath", check=False).returncode == 1
    assert _cfg(root, "--local", "--get", "core.hooksPath").stdout == "/shared\n"


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


def test_remove_in_one_worktree_keeps_the_shared_value_its_siblings_use(tmp_path):
    """A shared .githooks beneath this worktree's own custom setting is the siblings' gate: it stays."""
    root = _plain_repo(tmp_path)
    env = _env()
    for a in (["config", "user.name", "t"], ["config", "user.email", "t@t"], ["add", "-A"], ["commit", "-qm", "x"]):
        subprocess.run(["git", *a], cwd=root, check=True, capture_output=True, timeout=30, env=env)
    _cfg(root, "core.hooksPath", ".githooks")
    _cfg(root, "extensions.worktreeConfig", "true")
    wt = tmp_path / "wt"
    subprocess.run(["git", "worktree", "add", "-q", "--detach", str(wt)], cwd=root, check=True,
                   capture_output=True, timeout=30, env=env)
    _cfg(wt, "--worktree", "core.hooksPath", "/custom")
    r = subprocess.run(["bash", "scripts/install-hooks.sh", "--remove"], cwd=wt, capture_output=True, text=True,
                       timeout=60, env=env)
    assert r.returncode == 0 and "left as it is" in r.stdout, r
    assert _cfg(root, "--local", "--get", "core.hooksPath").stdout.strip() == ".githooks"
    assert _cfg(wt, "--worktree", "--get", "core.hooksPath").stdout.strip() == "/custom"


def test_status_shows_an_empty_hooks_path_as_set_not_unset(tmp_path):
    root = _plain_repo(tmp_path)
    _cfg(root, "core.hooksPath", "")
    status = subprocess.run(["bash", "scripts/install-hooks.sh", "--status"], cwd=root, capture_output=True,
                            text=True, timeout=60, env=_env())
    assert "Hooks path     : '' (expected: .githooks)" in status.stdout, status.stdout
    shutil.copy(REPO / "scripts/tooling-doctor.sh", root / "scripts/tooling-doctor.sh")
    r = subprocess.run(["bash", str(root / "scripts/tooling-doctor.sh"), "--json", "--group=hooks"], cwd=root,
                       capture_output=True, text=True, timeout=120, env=_env())
    check = {c["name"]: c for c in json.loads(r.stdout)["checks"]}["core.hooksPath"]
    assert "Unset" not in json.dumps(check), check                       # set to empty is not a fresh clone
    _cfg(root, "--unset", "core.hooksPath")
    status = subprocess.run(["bash", "scripts/install-hooks.sh", "--status"], cwd=root, capture_output=True,
                            text=True, timeout=60, env=_env())
    assert "Hooks path     : <unset> (expected: .githooks)" in status.stdout, status.stdout


def test_install_refuses_a_command_line_value_before_writing_any_file(tmp_path):
    root = _plain_repo(tmp_path)
    env = {**_env(), "GIT_CONFIG_COUNT": "1", "GIT_CONFIG_KEY_0": "core.hooksPath", "GIT_CONFIG_VALUE_0": "/custom"}
    r = subprocess.run(["bash", "scripts/install-hooks.sh"], cwd=root, capture_output=True, text=True, timeout=60,
                       env=env)
    assert r.returncode != 0 and "outranks every config file" in r.stderr, r
    assert _cfg(root, "--local", "--get", "core.hooksPath", check=False).returncode == 1   # nothing written


def test_install_refuses_a_command_line_githooks_over_an_unset_value(tmp_path):
    root = _plain_repo(tmp_path)
    env = {**_env(), "GIT_CONFIG_COUNT": "1", "GIT_CONFIG_KEY_0": "core.hooksPath", "GIT_CONFIG_VALUE_0": ".githooks"}
    r = subprocess.run(["bash", "scripts/install-hooks.sh", "--with-pre-push"], cwd=root, capture_output=True,
                       text=True, timeout=60, env=env)
    assert r.returncode != 0 and "outranks every config file" in r.stderr, r
    assert _cfg(root, "--local", "--get", "core.hooksPath", check=False).returncode == 1
    assert not list(root.joinpath(".git").glob(".impossible-os-prepush"))          # no sentinel either


def test_remove_under_a_command_line_value_changes_nothing(tmp_path):
    root = _plain_repo(tmp_path)
    assert subprocess.run(["bash", "scripts/install-hooks.sh", "--with-pre-push"], cwd=root, capture_output=True,
                          text=True, timeout=60, env=_env()).returncode == 0
    sentinel = subprocess.run(["git", "rev-parse", "--path-format=absolute", "--git-path", ".impossible-os-prepush"],
                              cwd=root, capture_output=True, text=True, timeout=30, env=_env()).stdout.strip()
    env = {**_env(), "GIT_CONFIG_COUNT": "1", "GIT_CONFIG_KEY_0": "core.hooksPath", "GIT_CONFIG_VALUE_0": "/custom"}
    r = subprocess.run(["bash", "scripts/install-hooks.sh", "--remove"], cwd=root, capture_output=True, text=True,
                       timeout=60, env=env)
    assert r.returncode != 0 and "outranks every config file" in r.stderr, r
    assert _cfg(root, "--local", "--get", "core.hooksPath").stdout == ".githooks\n"
    assert os.path.exists(sentinel)


def _config_bytes(root):
    return (root / ".git/config").read_bytes()


def test_install_refuses_a_scope_that_already_holds_values(tmp_path):
    root = _plain_repo(tmp_path)
    _cfg(root, "--local", "--add", "core.hooksPath", "first")
    _cfg(root, "--local", "--add", "core.hooksPath", "second")
    before = _config_bytes(root)
    r = subprocess.run(["bash", "scripts/install-hooks.sh"], cwd=root, capture_output=True, text=True, timeout=60,
                       env=_env())
    assert r.returncode != 0 and "already set in local config" in r.stderr and "first, second" in r.stderr, r
    assert _config_bytes(root) == before


def test_remove_keeps_entries_it_did_not_write(tmp_path):
    """.githooks among several entries is not this installer's single entry: remove leaves every one."""
    root = _plain_repo(tmp_path)
    _cfg(root, "--local", "--add", "core.hooksPath", "custom")
    _cfg(root, "--local", "--add", "core.hooksPath", ".githooks")
    before = _config_bytes(root)
    r = subprocess.run(["bash", "scripts/install-hooks.sh", "--remove"], cwd=root, capture_output=True, text=True,
                       timeout=60, env=_env())
    assert r.returncode == 0 and "not as this installer's single entry" in r.stdout, r
    assert _config_bytes(root) == before


def test_an_included_value_refuses_install_and_writes_nothing(tmp_path):
    root = _plain_repo(tmp_path)
    (root / ".git/inc").write_text("[core]\n\thooksPath = elsewhere\n")
    with open(root / ".git/config", "a") as f:
        f.write("[include]\n\tpath = inc\n")
    before = _config_bytes(root)
    for _attempt in (1, 2):                                 # a retry is refused the same way and adds nothing
        r = subprocess.run(["bash", "scripts/install-hooks.sh"], cwd=root, capture_output=True, text=True,
                           timeout=60, env=_env())
        assert r.returncode != 0 and "or a file it includes" in r.stderr, r
        assert _config_bytes(root) == before


def test_an_install_git_does_not_read_is_undone(tmp_path):
    """A write that a higher-priority value hides is taken out again, so nothing latent is left."""
    root = _plain_repo(tmp_path)
    _cfg(root, "extensions.worktreeConfig", "true")
    bindir = tmp_path / "bin"
    bindir.mkdir()
    real = shutil.which("git")
    # After the install writes the local value, a worktree value appears (as a racing writer would leave it).
    (bindir / "git").write_text(
        f'#!/bin/sh\n"{real}" "$@"; rc=$?\n'
        f'case " $* " in *" --local --add core.hooksPath .githooks "*) "{real}" -C "{root}" config --worktree core.hooksPath /other ;; esac\n'
        'exit $rc\n')
    (bindir / "git").chmod(0o755)
    env = {**_env(), "PATH": f"{bindir}:{os.environ['PATH']}"}
    r = subprocess.run(["bash", "scripts/install-hooks.sh"], cwd=root, capture_output=True, text=True, timeout=60,
                       env=env)
    assert r.returncode != 0 and "the write was undone" in r.stderr, r
    assert _cfg(root, "--local", "--get", "core.hooksPath", check=False).returncode == 1


def _racing_git(tmp_path, root, when, then):
    """git that, on a call whose arguments contain `when`, first runs `git config <then>` (another writer)."""
    bindir = tmp_path / "bin"
    bindir.mkdir()
    real = shutil.which("git")
    (bindir / "git").write_text(f'#!/bin/sh\ncase " $* " in *"{when}"*) "{real}" -C "{root}" config {then} ;; esac\n'
                                f'exec "{real}" "$@"\n')
    (bindir / "git").chmod(0o755)
    return {**_env(), "PATH": f"{bindir}:{os.environ['PATH']}"}


def test_an_install_racing_another_writer_loses_neither_value(tmp_path):
    """Another writer sets the value between the installer's check and its write: theirs stays, ours goes."""
    root = _plain_repo(tmp_path)
    env = _racing_git(tmp_path, root, " core.hooksPath .githooks", "--local core.hooksPath /custom")
    r = subprocess.run(["bash", "scripts/install-hooks.sh"], cwd=root, capture_output=True, text=True, timeout=60,
                       env=env)
    assert r.returncode != 0 and "the write was undone" in r.stderr, r
    assert _cfg(root, "--local", "--get-all", "core.hooksPath").stdout == "/custom\n"


def test_a_removal_racing_another_writer_keeps_their_value(tmp_path):
    root = _plain_repo(tmp_path)
    _cfg(root, "--local", "core.hooksPath", ".githooks")
    env = _racing_git(tmp_path, root, "--unset core.hooksPath", "--local core.hooksPath /custom")
    r = subprocess.run(["bash", "scripts/install-hooks.sh", "--remove"], cwd=root, capture_output=True, text=True,
                       timeout=60, env=env)
    assert r.returncode != 0 and "left as it is" in r.stderr, r
    assert _cfg(root, "--local", "--get-all", "core.hooksPath").stdout == "/custom\n"


def test_remove_reports_hooks_that_still_run_under_another_spelling(tmp_path):
    for spelling in ("{root}/.githooks", "./.githooks"):
        root = _plain_repo(tmp_path / spelling.replace("/", "_").replace("{", "").replace("}", ""))
        _cfg(root, "--local", "core.hooksPath", spelling.format(root=root))
        r = subprocess.run(["bash", "scripts/install-hooks.sh", "--remove"], cwd=root, capture_output=True,
                           text=True, timeout=60, env=_env())
        assert r.returncode == 0 and "still runs .githooks/" in r.stdout and "inert" not in r.stdout, (spelling, r)


def test_the_doctor_names_a_remedy_that_works_for_a_custom_local_value(tmp_path):
    root = _plain_repo(tmp_path)
    shutil.copy(REPO / "scripts/tooling-doctor.sh", root / "scripts/tooling-doctor.sh")
    _cfg(root, "--local", "core.hooksPath", "my-hooks")
    r = subprocess.run(["bash", str(root / "scripts/tooling-doctor.sh"), "--json", "--group=hooks"], cwd=root,
                       capture_output=True, text=True, timeout=120, env=_env())
    check = {c["name"]: c for c in json.loads(r.stdout)["checks"]}["core.hooksPath"]
    fix = check["fix"]
    assert "local config" in check["detail"] and fix.startswith("Check what that value is for; then run: "), check
    done = subprocess.run(["bash", "-c", fix.split("then run: ", 1)[1]], cwd=root, capture_output=True, text=True,
                          timeout=60, env=_env())
    assert done.returncode == 0 and "Configured: core.hooksPath = .githooks" in done.stdout, done


def test_two_installs_at_once_leave_one_entry_that_removal_takes_out(tmp_path):
    root = _plain_repo(tmp_path)
    bindir = tmp_path / "bin"
    bindir.mkdir()
    real = shutil.which("git")
    # Each write waits a moment, so without a lock both installers pass the empty-scope check first.
    (bindir / "git").write_text(f'#!/bin/sh\ncase " $* " in *" --add core.hooksPath "*) sleep 1 ;; esac\n'
                                f'exec "{real}" "$@"\n')
    (bindir / "git").chmod(0o755)
    env = {**_env(), "PATH": f"{bindir}:{os.environ['PATH']}"}
    runs = [subprocess.Popen(["bash", "scripts/install-hooks.sh"], cwd=root, env=env, stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, text=True, start_new_session=True) for _ in range(2)]
    outs, deadline = [], time.monotonic() + 60
    try:
        for p in runs:
            outs.append(p.communicate(timeout=max(1.0, deadline - time.monotonic())))
    finally:
        for p in runs:                                       # each installer's whole group, whatever happened
            try:
                os.killpg(p.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            p.wait()
    assert all(p.returncode == 0 for p in runs), outs
    assert _cfg(root, "--local", "--get-all", "core.hooksPath").stdout == ".githooks\n", outs
    r = subprocess.run(["bash", "scripts/install-hooks.sh", "--remove"], cwd=root, capture_output=True, text=True,
                       timeout=60, env=_env())
    assert r.returncode == 0 and _cfg(root, "--local", "--get-all", "core.hooksPath", check=False).returncode == 1


def test_a_hook_put_in_place_of_the_legacy_link_during_cleanup_survives(tmp_path):
    root = _plain_repo(tmp_path)
    link = root / ".git/hooks/pre-push"
    link.parent.mkdir(exist_ok=True)
    link.symlink_to(root / "scripts/hooks/pre-push")
    bindir = tmp_path / "bin"
    bindir.mkdir()
    flag = tmp_path / "raced"
    # Another installer replaces the link with its own hook at the first mv or rm of that name.
    for tool in ("mv", "rm"):
        real = shutil.which(tool)
        (bindir / tool).write_text(
            f'#!/bin/sh\ncase " $* " in *"{link} "*|*"{link}") [ -e "{flag}" ] || {{ : > "{flag}"; '
            f'"{shutil.which("rm")}" -f "{link}"; echo replacement > "{link}"; }} ;; esac\nexec "{real}" "$@"\n')
        (bindir / tool).chmod(0o755)
    env = {**_env(), "PATH": f"{bindir}:{os.environ['PATH']}"}
    r = subprocess.run(["bash", "scripts/install-hooks.sh", "--remove"], cwd=root, capture_output=True, text=True,
                       timeout=60, env=env)
    assert flag.exists() and not link.is_symlink() and link.exists() and link.read_text() == "replacement\n", r


def test_the_doctor_leaves_an_inherited_global_value_alone(tmp_path):
    root = _plain_repo(tmp_path)
    shutil.copy(REPO / "scripts/tooling-doctor.sh", root / "scripts/tooling-doctor.sh")
    glob = tmp_path / "global.cfg"
    glob.write_text("[core]\n\thooksPath = /shared\n")
    env = {**_env(), "GIT_CONFIG_GLOBAL": str(glob)}
    r = subprocess.run(["bash", str(root / "scripts/tooling-doctor.sh"), "--json", "--group=hooks"], cwd=root,
                       capture_output=True, text=True, timeout=120, env=env)
    fix = {c["name"]: c for c in json.loads(r.stdout)["checks"]}["core.hooksPath"]["fix"]
    assert fix == "Run: bash scripts/install-hooks.sh", fix
    done = subprocess.run(["bash", "-c", fix.split("Run: ", 1)[1]], cwd=root, capture_output=True, text=True,
                          timeout=60, env=env)
    assert done.returncode == 0 and glob.read_text() == "[core]\n\thooksPath = /shared\n", done


def test_the_doctor_names_the_included_file_a_value_comes_from(tmp_path):
    root = _plain_repo(tmp_path)
    shutil.copy(REPO / "scripts/tooling-doctor.sh", root / "scripts/tooling-doctor.sh")
    inc = tmp_path / "inc.cfg"
    inc.write_text("[core]\n\thooksPath = /included\n")
    _cfg(root, "--local", "include.path", str(inc))
    r = subprocess.run(["bash", str(root / "scripts/tooling-doctor.sh"), "--json", "--group=hooks"], cwd=root,
                       capture_output=True, text=True, timeout=120, env=_env())
    fix = {c["name"]: c for c in json.loads(r.stdout)["checks"]}["core.hooksPath"]["fix"]
    assert str(inc) in fix and "--unset-all" not in fix, fix


def test_a_removal_never_overwrites_an_entry_an_earlier_removal_kept(tmp_path):
    """The old aside name held the process id: a later remover with that id renamed over the kept hook."""
    root = _plain_repo(tmp_path)
    hooks = root / ".git/hooks"
    hooks.mkdir(exist_ok=True)
    (hooks / "pre-push").symlink_to(root / "scripts/hooks/pre-push")

    def keep_one():
        (hooks / f".pre-push.install-hooks.{os.getpid()}").write_text("kept\n")
    r = subprocess.run(["bash", "scripts/install-hooks.sh", "--remove"], cwd=root, capture_output=True, text=True,
                       timeout=60, env=_env(), preexec_fn=keep_one)
    kept = list(hooks.glob(".pre-push.install-hooks.*"))
    assert r.returncode == 0 and len(kept) == 1 and kept[0].read_text() == "kept\n", r
    assert not (hooks / "pre-push").exists() and not (hooks / "pre-push").is_symlink()


def test_a_hook_that_is_not_ours_stays_in_place_while_it_is_judged(tmp_path):
    root = _plain_repo(tmp_path)
    hooks = root / ".git/hooks"
    hooks.mkdir(exist_ok=True)
    theirs = tmp_path / "their-pre-push"
    theirs.write_text("#!/bin/sh\nexit 0\n")
    (hooks / "pre-push").symlink_to(theirs)
    bindir = tmp_path / "bin"
    bindir.mkdir()
    seen = tmp_path / "seen"
    real = shutil.which("git")
    # Records, at every worktree scan, whether a push at that moment would find the hook.
    (bindir / "git").write_text(f'#!/bin/sh\ncase " $* " in *" worktree list "*) [ -L "{hooks}/pre-push" ] '
                                f'&& echo present >> "{seen}" || echo missing >> "{seen}" ;; esac\n'
                                f'exec "{real}" "$@"\n')
    (bindir / "git").chmod(0o755)
    env = {**_env(), "PATH": f"{bindir}:{os.environ['PATH']}"}
    r = subprocess.run(["bash", "scripts/install-hooks.sh", "--remove"], cwd=root, capture_output=True, text=True,
                       timeout=60, env=env)
    assert r.returncode == 0 and seen.read_text().split() and "missing" not in seen.read_text(), r
    assert (hooks / "pre-push").resolve() == theirs.resolve()


def test_the_doctor_names_every_file_when_the_scope_and_an_include_both_set_it(tmp_path):
    root = _plain_repo(tmp_path)
    shutil.copy(REPO / "scripts/tooling-doctor.sh", root / "scripts/tooling-doctor.sh")
    inc = tmp_path / "inc.cfg"
    inc.write_text("[core]\n\thooksPath = /included\n")
    _cfg(root, "--local", "core.hooksPath", "/direct")
    _cfg(root, "--local", "include.path", str(inc))
    r = subprocess.run(["bash", str(root / "scripts/tooling-doctor.sh"), "--json", "--group=hooks"], cwd=root,
                       capture_output=True, text=True, timeout=120, env=_env())
    fix = {c["name"]: c for c in json.loads(r.stdout)["checks"]}["core.hooksPath"]["fix"]
    assert str(inc) in fix and ".git/config" in fix and "--unset-all" not in fix, fix


def _doctor(root, env):
    shutil.copy(REPO / "scripts/tooling-doctor.sh", root / "scripts/tooling-doctor.sh")
    r = subprocess.run(["bash", str(root / "scripts/tooling-doctor.sh"), "--json", "--group=hooks"], cwd=root,
                       capture_output=True, text=True, timeout=120, env=env)
    return {c["name"]: c for c in json.loads(r.stdout)["checks"]}["core.hooksPath"]


def _command_value(value):
    return {**_env(), "GIT_CONFIG_COUNT": "1", "GIT_CONFIG_KEY_0": "core.hooksPath", "GIT_CONFIG_VALUE_0": value}


def test_the_doctor_does_not_pass_a_hooks_path_given_for_one_command(tmp_path):
    root = _plain_repo(tmp_path)
    check = _doctor(root, _command_value(".githooks"))
    assert check["status"] == "warn" and "command line" in check["detail"], check


def test_the_doctor_reads_values_that_look_like_its_own_words(tmp_path):
    root = _plain_repo(tmp_path)
    for value in ("<unset>", "foo\nbar"):
        check = _doctor(root, _command_value(value))
        assert check["status"] == "warn" and "command line" in check["detail"], (value, check)
        assert "Unset" not in check["detail"] and "bar config" not in check["detail"], (value, check)


def test_the_doctor_remedy_for_a_worktree_value_over_a_local_one_works(tmp_path):
    root = _plain_repo(tmp_path)
    _cfg(root, "extensions.worktreeConfig", "true")
    _cfg(root, "--local", "core.hooksPath", "/shared")
    _cfg(root, "--worktree", "core.hooksPath", "/mine")
    fix = _doctor(root, _env())["fix"]
    assert "--replace-all core.hooksPath .githooks" in fix, fix
    done = subprocess.run(["bash", "-c", fix.split("then run: ", 1)[1]], cwd=root, capture_output=True, text=True,
                          timeout=60, env=_env())
    assert done.returncode == 0 and _doctor(root, _env())["status"] == "pass", done
    assert _cfg(root, "--local", "--get-all", "core.hooksPath").stdout == "/shared\n"


def test_a_legacy_link_that_cannot_be_moved_fails_the_removal_and_stays(tmp_path):
    root = _plain_repo(tmp_path)
    link = root / ".git/hooks/pre-push"
    link.parent.mkdir(exist_ok=True)
    link.symlink_to(root / "scripts/hooks/pre-push")
    bindir = tmp_path / "bin"
    bindir.mkdir()
    real = shutil.which("mv")
    (bindir / "mv").write_text(f'#!/bin/sh\ncase " $* " in *"{link} "*) echo "mv: refused" >&2; exit 1 ;; esac\n'
                               f'exec "{real}" "$@"\n')
    (bindir / "mv").chmod(0o755)
    r = subprocess.run(["bash", "scripts/install-hooks.sh", "--remove"], cwd=root, capture_output=True, text=True,
                       timeout=60, env={**_env(), "PATH": f"{bindir}:{os.environ['PATH']}"})
    assert r.returncode != 0 and "left in place" in r.stderr and link.is_symlink(), r
    assert not list(link.parent.glob(".install-hooks.*"))


def test_a_relative_legacy_link_is_removed(tmp_path):
    """A relative link resolves elsewhere once moved, so the entry moved aside is known by its own identity."""
    root = _plain_repo(tmp_path)
    link = root / ".git/hooks/pre-push"
    link.parent.mkdir(exist_ok=True)
    link.symlink_to("../../scripts/hooks/pre-push")
    r = subprocess.run(["bash", "scripts/install-hooks.sh", "--remove"], cwd=root, capture_output=True, text=True,
                       timeout=60, env=_env())
    assert r.returncode == 0 and not link.is_symlink() and not link.exists(), r
    assert not list(link.parent.glob(".install-hooks.*"))


def test_a_legacy_link_that_cannot_be_deleted_fails_the_removal(tmp_path):
    root = _plain_repo(tmp_path)
    link = root / ".git/hooks/pre-push"
    link.parent.mkdir(exist_ok=True)
    link.symlink_to(root / "scripts/hooks/pre-push")
    bindir = tmp_path / "bin"
    bindir.mkdir()
    real = shutil.which("rm")
    (bindir / "rm").write_text(f'#!/bin/sh\ncase " $* " in *"/.install-hooks."*"/pre-push"*) exit 1 ;; esac\n'
                               f'exec "{real}" "$@"\n')
    (bindir / "rm").chmod(0o755)
    r = subprocess.run(["bash", "scripts/install-hooks.sh", "--remove"], cwd=root, capture_output=True, text=True,
                       timeout=60, env={**_env(), "PATH": f"{bindir}:{os.environ['PATH']}"})
    assert r.returncode != 0 and "could not be deleted" in r.stderr and "Pruned" not in r.stdout, r


def test_the_doctor_names_every_file_when_one_name_holds_another(tmp_path):
    root = _plain_repo(tmp_path)
    plain = tmp_path / "e"
    holding = Path(f"{tmp_path}/d, {plain}")              # its name ends with ", " and then the other's name
    holding.parent.mkdir(parents=True)
    for f in (holding, plain):
        f.write_text("[core]\n\thooksPath = /custom\n")
    _cfg(root, "--local", "--add", "include.path", str(holding))
    _cfg(root, "--local", "--add", "include.path", str(plain))
    fix = _doctor(root, _env())["fix"]
    assert fix.count(str(plain)) == 2, fix                # once inside the longer name, once on its own


def _legacy_link(tmp_path):
    root = _plain_repo(tmp_path)
    link = root / ".git/hooks/pre-push"
    link.parent.mkdir(exist_ok=True)
    link.symlink_to(root / "scripts/hooks/pre-push")
    bindir = tmp_path / "bin"
    bindir.mkdir()
    return root, link, bindir


def _remove_with(root, bindir, tool, body):
    (bindir / tool).write_text(f'#!/bin/sh\n{body}\nexec "{shutil.which(tool)}" "$@"\n')
    (bindir / tool).chmod(0o755)
    return subprocess.run(["bash", "scripts/install-hooks.sh", "--remove"], cwd=root, capture_output=True, text=True,
                          timeout=60, env={**_env(), "PATH": f"{bindir}:{os.environ['PATH']}"})


def test_a_listing_cut_short_after_a_match_fails_the_removal(tmp_path):
    root, link, bindir = _legacy_link(tmp_path)
    r = _remove_with(root, bindir, "git", f'case " $* " in *" worktree list "*) "{shutil.which("git")}" "$@"; '
                                          'exit 128 ;; esac')
    assert r.returncode != 0 and "could not be listed" in r.stderr and link.is_symlink(), r


def test_a_link_whose_identity_cannot_be_read_is_never_deleted(tmp_path):
    root, link, bindir = _legacy_link(tmp_path)
    r = _remove_with(root, bindir, "stat", f'case " $* " in *"%d:%i -- {link}"*) exit 1 ;; esac')
    assert r.returncode != 0 and "could not be read: left in place" in r.stderr and link.is_symlink(), r
    (tmp_path / "again").mkdir()
    root, link, bindir = _legacy_link(tmp_path / "again")
    r = _remove_with(root, bindir, "stat", 'case " $* " in *"/.install-hooks."*) exit 1 ;; esac')
    assert r.returncode != 0 and "could not be read again" in r.stderr and link.is_symlink(), r
    assert not list(link.parent.glob(".install-hooks.*/pre-push"))


def test_a_failed_worktree_listing_fails_the_removal(tmp_path):
    root = _plain_repo(tmp_path)
    link = root / ".git/hooks/pre-push"
    link.parent.mkdir(exist_ok=True)
    link.symlink_to(root / "scripts/hooks/pre-push")
    bindir = tmp_path / "bin"
    bindir.mkdir()
    real = shutil.which("git")
    (bindir / "git").write_text(f'#!/bin/sh\ncase " $* " in *" worktree list "*) exit 128 ;; esac\n'
                                f'exec "{real}" "$@"\n')
    (bindir / "git").chmod(0o755)
    r = subprocess.run(["bash", "scripts/install-hooks.sh", "--remove"], cwd=root, capture_output=True, text=True,
                       timeout=60, env={**_env(), "PATH": f"{bindir}:{os.environ['PATH']}"})
    assert r.returncode != 0 and "could not be listed" in r.stderr and link.is_symlink(), r


def test_the_doctor_repairs_a_worktree_value_without_touching_its_siblings(tmp_path):
    root = _plain_repo(tmp_path)
    _cfg(root, "extensions.worktreeConfig", "true")
    sibling = tmp_path / "sibling"
    subprocess.run(["git", "-C", str(root), "-c", "user.name=t", "-c", "user.email=t@example.invalid", "commit",
                    "-q", "--allow-empty", "-m", "base"], check=True, timeout=60, env=_env())
    subprocess.run(["git", "-C", str(root), "worktree", "add", "-q", "--detach", str(sibling)], check=True,
                   timeout=60, env=_env())
    _cfg(root, "--worktree", "core.hooksPath", "/mine")
    fix = _doctor(root, _env())["fix"]
    assert "--worktree --replace-all core.hooksPath .githooks" in fix, fix
    subprocess.run(["bash", "-c", fix.split("then run: ", 1)[1]], cwd=root, check=True, timeout=60, env=_env())
    assert _doctor(root, _env())["status"] == "pass"
    assert _cfg(sibling, "--get", "core.hooksPath", check=False).returncode == 1


def test_an_install_shadowed_by_a_worktree_githooks_is_undone(tmp_path):
    """Even a competing .githooks in another scope leaves the repository-wide write latent: it is undone."""
    root = _plain_repo(tmp_path)
    _cfg(root, "extensions.worktreeConfig", "true")
    bindir = tmp_path / "bin"
    bindir.mkdir()
    real = shutil.which("git")
    (bindir / "git").write_text(
        f'#!/bin/sh\n"{real}" "$@"; rc=$?\n'
        f'case " $* " in *" --local --add core.hooksPath .githooks "*) "{real}" -C "{root}" config --worktree core.hooksPath .githooks ;; esac\n'
        'exit $rc\n')
    (bindir / "git").chmod(0o755)
    r = subprocess.run(["bash", "scripts/install-hooks.sh"], cwd=root, capture_output=True, text=True, timeout=60,
                       env={**_env(), "PATH": f"{bindir}:{os.environ['PATH']}"})
    assert r.returncode != 0 and "the write was undone" in r.stderr, r
    assert _cfg(root, "--local", "--get", "core.hooksPath", check=False).returncode == 1


def test_a_legacy_hooks_directory_that_cannot_be_searched_fails_the_removal(tmp_path):
    root = _plain_repo(tmp_path)
    hooks = root / ".git/hooks"
    hooks.mkdir(exist_ok=True)
    (hooks / "pre-push").symlink_to(root / "scripts/hooks/pre-push")
    hooks.chmod(0o600)
    try:
        r = subprocess.run(["bash", "scripts/install-hooks.sh", "--remove"], cwd=root, capture_output=True,
                           text=True, timeout=60, env=_env())
    finally:
        hooks.chmod(0o755)
    assert r.returncode != 0 and "could not be looked up" in r.stderr and (hooks / "pre-push").is_symlink(), r


def test_a_legacy_link_into_a_worktree_that_cannot_be_searched_fails_the_removal(tmp_path):
    root = _plain_repo(tmp_path)
    git = ["git", "-C", str(root), "-c", "user.name=t", "-c", "user.email=t@example.invalid"]
    subprocess.run([*git, "add", "scripts", ".githooks"], check=True, timeout=60, env=_env())
    subprocess.run([*git, "commit", "-q", "-m", "base"], check=True, timeout=60, env=_env())
    wt = tmp_path / "wt"
    subprocess.run([*git, "worktree", "add", "-q", "--detach", str(wt)], check=True, timeout=60, env=_env())
    hooks = root / ".git/hooks"
    hooks.mkdir(exist_ok=True)
    (hooks / "pre-push").symlink_to(wt / "scripts/hooks/pre-push")
    (wt / "scripts").chmod(0o600)                      # the link cannot be followed, nor the hook it names
    try:
        r = subprocess.run(["bash", "scripts/install-hooks.sh", "--remove"], cwd=root, capture_output=True,
                           text=True, timeout=60, env=_env())
    finally:
        (wt / "scripts").chmod(0o755)
    assert r.returncode != 0 and "could not be read, so it was not judged" in r.stderr, r
    assert (hooks / "pre-push").is_symlink()


def test_an_unreadable_sibling_worktree_outranks_a_match(tmp_path):
    """The link matches this checkout's hook, but a sibling not read could hold the same file: not judged."""
    root = _plain_repo(tmp_path)
    git = ["git", "-C", str(root), "-c", "user.name=t", "-c", "user.email=t@example.invalid"]
    subprocess.run([*git, "add", "scripts", ".githooks"], check=True, timeout=60, env=_env())
    subprocess.run([*git, "commit", "-q", "-m", "base"], check=True, timeout=60, env=_env())
    wt = tmp_path / "wt"
    subprocess.run([*git, "worktree", "add", "-q", "--detach", str(wt)], check=True, timeout=60, env=_env())
    hooks = root / ".git/hooks"
    hooks.mkdir(exist_ok=True)
    (hooks / "pre-push").symlink_to(root / "scripts/hooks/pre-push")
    (wt / "scripts").chmod(0o600)
    try:
        r = subprocess.run(["bash", "scripts/install-hooks.sh", "--remove"], cwd=root, capture_output=True,
                           text=True, timeout=60, env=_env())
    finally:
        (wt / "scripts").chmod(0o755)
    assert r.returncode != 0 and "could not be read, so it was not judged" in r.stderr, r
    assert (hooks / "pre-push").is_symlink()


def test_a_legacy_entry_that_cannot_be_looked_up_fails_the_removal(tmp_path):
    root, link, bindir = _legacy_link(tmp_path)
    r = _remove_with(root, bindir, "stat", 'case " $* " in *"%F -- "*) echo "stat: Input/output error" >&2; '
                                           'exit 1 ;; esac')
    assert r.returncode != 0 and "could not be looked up" in r.stderr and link.is_symlink(), r


def test_an_io_error_naming_a_path_that_holds_the_absence_words_is_not_absence(tmp_path):
    root, link, bindir = _legacy_link(tmp_path)
    msg = "stat: cannot statx '/x/No such file or directory/y': Input/output error"
    for probe in ("%F -- ", "%d:%i -- "):
        r = _remove_with(root, bindir, "stat", f'case " $* " in *"{probe}"*) echo "{msg}" >&2; exit 1 ;; esac')
        assert r.returncode != 0 and link.is_symlink(), (probe, r)


def test_a_legacy_hooks_directory_that_cannot_be_looked_up_is_left_unjudged(tmp_path):
    root, link, bindir = _legacy_link(tmp_path)
    hooks = root / ".git/hooks"
    r = _remove_with(root, bindir, "stat", f'case " $* " in *"-- {hooks} "*) echo "stat: cannot statx '
                                           f"'{hooks}': Input/output error\" >&2; exit 1 ;; esac")
    assert r.returncode != 0 and "could not be looked up" in r.stderr and link.is_symlink(), r


def test_a_replacement_that_cannot_be_put_back_fails_the_removal(tmp_path):
    """Another installer's hook was moved aside and the put-back fails with the name empty: no pre-push hook
    runs, so the removal fails and names where the hook is, never reports a new hook appeared."""
    root = _plain_repo(tmp_path)
    link = root / ".git/hooks/pre-push"
    link.parent.mkdir(exist_ok=True)
    link.symlink_to(root / "scripts/hooks/pre-push")
    bindir = tmp_path / "bin"
    bindir.mkdir()
    flag = tmp_path / "raced"
    real = shutil.which("mv")
    (bindir / "mv").write_text(
        f'#!/bin/sh\ncase " $* " in *" -n "*) echo "mv: cannot move: Input/output error" >&2; exit 1 ;; esac\n'
        f'case " $* " in *"{link} "*|*"{link}") [ -e "{flag}" ] || {{ : > "{flag}"; '
        f'"{shutil.which("rm")}" -f "{link}"; echo replacement > "{link}"; }} ;; esac\nexec "{real}" "$@"\n')
    (bindir / "mv").chmod(0o755)
    env = {**_env(), "PATH": f"{bindir}:{os.environ['PATH']}"}
    r = subprocess.run(["bash", "scripts/install-hooks.sh", "--remove"], cwd=root, capture_output=True, text=True,
                       timeout=60, env=env)
    assert r.returncode != 0 and "could not be put back" in r.stderr and not link.exists(), r
    kept = list((root / ".git/hooks").glob(".install-hooks.*/pre-push"))
    assert kept and kept[0].read_text() == "replacement\n"
