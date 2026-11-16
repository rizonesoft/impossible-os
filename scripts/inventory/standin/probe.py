#!/usr/bin/env python3
"""Apply a capture profile on the stand-in machine and print what the probe sees.

    standin/probe.py PROFILE

For a profile the reference VM cannot provide (two displays: Hyper-V gives a VM
one), boots the stand-in variant that has that many heads (variants.json
"displays"), applies the profile with set-profile.ps1 and prints probe-profile.ps1's
JSON line, the same line ref-vm.ps1 -Verify compares on the reference VM. Exit
0 with the line; 2 when the profile cannot be applied there (set-profile's own
refusal, printed) or no variant has the heads; 1 or 3 when the run failed.
ref-vm.ps1 -Verify calls it for each two-display profile.
Owner: todo/08-graphics-ui/TODO-17-win11-parity-inventory.md section 22.
"""
from __future__ import annotations

import argparse
import importlib.util
import json
import pathlib
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
INV = HERE.parent
sys.path.insert(0, str(INV))
import capture_core as core  # noqa: E402 -- the inventory folder is not a package

PROFILES = INV.parents[1] / "docs" / "design" / "inventory" / "profiles.json"

# In the guest: set-profile first, then the probe, whose line goes to its own
# file so set-profile's output never mixes with it. A refusal (set-profile's exit
# 2: a value this machine does not offer) is written to refused.txt, so it is
# told apart from a failure, which the job's exit code alone cannot do.
GUEST = r"""param([string]$Name)
& C:\ios\in\set-profile.ps1 -Profiles C:\ios\in\profiles.json -Name $Name
$c = $LASTEXITCODE
if ($c -eq 2) { Set-Content -LiteralPath C:\ios\out\refused.txt -Value 'refused' -Encoding ASCII }
if ($c -ne 0) { exit $c }
& C:\ios\in\probe-profile.ps1 | Set-Content -LiteralPath C:\ios\out\seen.json -Encoding ASCII
exit 0
"""
COMMAND = "powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\\ios\\in\\probe-job.ps1 -Name {name}"


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("profile")
    ap.add_argument("--timeout", type=int, default=1200)
    args = ap.parse_args(argv)
    profiles = json.loads(PROFILES.read_text(encoding="utf-8"))
    variants = json.loads((HERE / "variants.json").read_text(encoding="utf-8"))
    if args.profile not in profiles:
        print(f"probe: no profile {args.profile} in {PROFILES.name}")
        return 2
    variant, problem = core.standin_variant(args.profile, profiles[args.profile], variants)
    if variant is None:
        print(f"probe: {problem}" if problem else f"probe: profile {args.profile} has one display; ref-vm.ps1 -Verify checks it on the reference VM")
        return 2
    spec = importlib.util.spec_from_file_location("standin_run", HERE / "run.py")
    run = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(run)
    with tempfile.TemporaryDirectory(prefix="standin-probe-") as tmp:
        t = pathlib.Path(tmp)
        (t / "probe-job.ps1").write_text(GUEST, encoding="utf-8")
        files = [("in/probe-job.ps1", t / "probe-job.ps1"),
                 ("in/set-profile.ps1", INV / "guest" / "set-profile.ps1"),
                 ("in/probe-profile.ps1", INV / "guest" / "probe-profile.ps1"),
                 ("in/profiles.json", PROFILES)]
        (t / "job.ps1").write_text(run.COMMAND_JOB.format(command=COMMAND.format(name=args.profile)), encoding="utf-8")
        rc = run.run(variant, t / "job.ps1", files, t / "out", timeout=args.timeout, variants=variants)
        code, text = result(rc, t / "out")
        if text:
            print(text)
        return code


def result(rc, out):
    """(exit code, text to print) for a probe job that returned `rc` (run.run's
    0, 1 or 3) with its output folder `out`: the probe's line and 0; set-profile's
    own refusal and 2; otherwise what the guest said and the failure."""
    seen = out / "seen.json"
    line = seen.read_text(encoding="utf-8-sig", errors="replace").strip() if seen.is_file() else ""
    if rc == 0 and line:
        return 0, line
    stdout = out / "stdout.txt"
    said = stdout.read_text(encoding="utf-8", errors="replace").strip() if stdout.is_file() else ""
    if (out / "refused.txt").is_file():
        return 2, said
    return (rc or 1), said or "probe: the job left no probe line"


if __name__ == "__main__":
    sys.exit(main())
