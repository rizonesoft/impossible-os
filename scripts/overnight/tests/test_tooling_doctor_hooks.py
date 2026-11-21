"""tooling-doctor.sh reads install-hooks.sh --status by its labels; a renamed label must not drop a row."""
import json
import subprocess
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]


def test_the_doctor_reports_the_pre_push_build_step_and_every_hook():
    r = subprocess.run(["bash", str(REPO / "scripts/tooling-doctor.sh"), "--json"], cwd=REPO,
                       capture_output=True, text=True, timeout=120)
    checks = {c["name"]: c for c in json.loads(r.stdout)["checks"] if c["group"] == "hooks"}
    step = checks.get("pre-push build/test step")
    assert step and step["status"] == "pass" and step["detail"].startswith(("ENABLED", "disabled")), checks
    for hook in ("pre-commit", "commit-msg", "post-commit", "pre-push"):
        assert f".githooks/{hook}" in checks, hook
