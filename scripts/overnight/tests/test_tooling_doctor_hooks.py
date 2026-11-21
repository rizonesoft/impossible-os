"""tooling-doctor.sh reads install-hooks.sh --status by its labels; a renamed label must not drop a row."""
import json
import shutil
import subprocess
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
HOOKS = ("pre-commit", "prepare-commit-msg", "commit-msg", "post-commit", "pre-push")


def _hooks(root):
    r = subprocess.run(["bash", str(root / "scripts/tooling-doctor.sh"), "--json"], cwd=root,
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
