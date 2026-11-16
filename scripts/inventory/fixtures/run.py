#!/usr/bin/env python3
"""Run a capture fixture in the reference VM and prove it repeatable.

    fixtures/run.py --twice lists:populated [notifications:groups ...]

For each NAME:VARIANT, on one fresh restore of the `clean` checkpoint: run the
fixture, read its -State, run it again, read -State again. Passes when both
states are the same and the first is not empty; prints one line per fixture
and exits 1 when any differs or fails. `all` runs every variant every fixture
script declares. Owner: todo/08-graphics-ui/TODO-17-win11-parity-inventory.md
section 20.
"""
import argparse
import importlib.util
import json
import pathlib
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


core = _load("capture_core", HERE.parent / "capture_core.py")

DRIVE = r"""$ErrorActionPreference = 'Stop'
$fx = 'C:\ios\in\fixtures\{name}.ps1'
foreach ($round in 1, 2) {{
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $fx -Variant {variant}
    if ($LASTEXITCODE -ne 0) {{ Write-Output "run {name}:{variant}: round $round exited $LASTEXITCODE"; exit 1 }}
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $fx -State | Set-Content -Encoding UTF8 "C:\ios\out\state$round.json"
}}
"""


def all_specs():
    out = []
    for path in sorted(HERE.glob("*.ps1")):
        m = core.FIXTURE_VARIANTS.search(path.read_text(encoding="utf-8"))
        out += [f"{path.stem}:{v.strip()}" for v in m.group(1).split(",")] if m else [path.stem]
    return out


def verdict(out_dir, spec):
    """The line for one run's output folder, and whether it passed."""
    states = []
    for n in (1, 2):
        f = pathlib.Path(out_dir) / f"state{n}.json"
        if not f.is_file():
            return f"{spec}: no state after round {n}", False
        states.append(json.loads(f.read_text(encoding="utf-8-sig")))
    if not any(v for v in states[0].values()):
        return f"{spec}: the fixture left nothing", False
    if states[0] != states[1]:
        return f"{spec}: the second run left a different state", False
    return f"{spec}: same state twice", True


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--twice", nargs="+", required=True, metavar="NAME:VARIANT")
    ap.add_argument("--timeout", type=int, default=900)
    args = ap.parse_args(argv)
    specs = all_specs() if args.twice == ["all"] else args.twice
    for spec in specs:
        problem = core.fixture_problem(HERE, spec)
        if problem:
            ap.error(problem)
    capture = _load("capture", HERE.parent / "capture.py")
    try:
        # The same lock a capture takes: one restore of the VM at a time.
        with capture.exclusive(capture.LOCK):
            return run_all(capture, specs, args.timeout)
    except BlockingIOError:
        print(f"fixture: a capture holds {capture.LOCK}; run one at a time")
        return 4


def run_all(capture, specs, timeout):
    ok = True
    for spec in specs:
        name, _, variant = spec.partition(":")
        with tempfile.TemporaryDirectory() as tmp:
            drive = pathlib.Path(tmp) / "drive.ps1"
            drive.write_text(DRIVE.format(name=name, variant=variant), encoding="utf-8")
            files = {"drive.ps1": drive, f"fixtures/{name}.ps1": HERE / f"{name}.ps1"}
            cmd = "powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\\ios\\in\\drive.ps1"
            out, rc = capture.ref_run(f"fixture-{name}-{variant}", files, cmd, timeout, who="fixture")
        line, passed = verdict(out, spec) if rc == 0 else (f"{spec}: ref-run exited {rc}", False)
        print(line)
        ok = ok and passed
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
