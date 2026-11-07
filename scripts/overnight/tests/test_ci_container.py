#!/usr/bin/env python3
"""The container tier installs what CI installs, and refuses a workflow it cannot read."""
import pathlib
import subprocess
import sys

import yaml

REPO = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / "scripts"))
import ci_container_setup as setup  # noqa: E402

WF = (REPO / ".github/workflows/build.yml").read_text()


def test_setup_comes_from_the_real_workflow():
    got = setup.setup_script(WF)
    assert "coreutils-from-gnu" in got and "clang-19" in got and "qemu-system-x86" in got
    assert "${{" not in got


def test_refusal_a_missing_step_or_expression_is_an_error():
    wf = yaml.safe_load(WF)
    wf["jobs"]["build"]["steps"] = [s for s in wf["jobs"]["build"]["steps"]
                                    if not str(s.get("name", "")).startswith("Install LLVM")]
    try:
        setup.setup_script(yaml.safe_dump(wf))
    except SystemExit as e:
        assert "Install LLVM" in str(e)
    else:
        raise AssertionError("a missing install step must refuse")
    wf = yaml.safe_load(WF)
    wf.pop("env", None)
    try:
        setup.setup_script(yaml.safe_dump(wf))
    except SystemExit as e:
        assert "unresolved" in str(e)
    else:
        raise AssertionError("an unresolved expression must refuse")


def test_test_sh_hands_off_before_touching_this_tree():
    text = (REPO / "scripts/test.sh").read_text()
    assert text.index('exec bash "$PROJECT/scripts/ci-container.sh"') < text.index("# --- Step 1: Build ---")


def test_split_keeps_required_check_and_the_pack_still_runs():
    jobs = yaml.safe_load(WF)["jobs"]
    # The ruleset requires this name, and it must answer for both jobs even when one
    # fails or is skipped, or a failing pack could merge.
    gate = jobs["required-check"]
    assert gate["name"] == "Build Impossible OS"
    assert set(gate["needs"]) == {"build", "tooling-pack"}
    assert gate["if"] == "always()"
    script = gate["steps"][0]["run"]
    assert 'OS_RESULT" = success' in script and 'PACK_RESULT" = success' in script
    assert not [j for k, j in jobs.items() if k != "required-check"
                and j.get("name") == "Build Impossible OS"]
    names = [s.get("name") for s in jobs["tooling-pack"]["steps"]]
    assert "Run tooling regression pack" in names
    assert not [s for s in jobs["build"]["steps"] if s.get("name") == "Run tooling regression pack"]
    build_names = [s.get("name", "") for s in jobs["build"]["steps"]]
    for step in ("Use GNU coreutils", "Install dependencies", "Verify toolchain sentinels and version floors"):
        assert step in names and step in build_names, step
    run = [s for s in jobs["tooling-pack"]["steps"] if s.get("name") == "Run tooling regression pack"][0]
    assert run["env"]["TEST_TOOLING_SKIP_LSP_MCP"] == "1"


def test_container_gets_full_history():
    text = (REPO / "scripts/ci-container.sh").read_text()
    assert "git bundle create" in text and "git clone" in text
    assert "git archive" not in text, "a snapshot has no history for lint to judge against"


def test_shell_syntax():
    subprocess.run(["bash", "-n", str(REPO / "scripts/ci-container.sh")], check=True)


if __name__ == "__main__":
    for name, fn in list(globals().items()):
        if name.startswith("test_"):
            fn()
    print("OK")
