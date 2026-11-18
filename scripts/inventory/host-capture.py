#!/usr/bin/env python3
"""Capture the screens UI Automation cannot reach, from the host, and publish their records.

Owner: todo/08-graphics-ui/TODO-17-win11-parity-inventory.md section 9.

    python3 scripts/inventory/host-capture.py --scenario session-secure [--dry-run] [--from DIR]

A scenario is scripts/inventory/host/<name>.py. It names SURFACE, FILE,
PROFILE, CHECKPOINT and TIMEOUT (and SESSION = False for a checkpoint with no
account to open a session as), the STEPS ref-host.ps1 runs on the reference
VM (keys, clicks, guest commands, shots), SHOTS (the shots it publishes) and
records(images, build, taken), which measures its records from those shots
(images: name to capture_core.read_png image; taken: name to its shots.json
entry, with the time it was taken) and returns them. Every record is a hand record:
source hand:<build>, since no tool read it from the running system.

Refuses (exit 3, nothing published) a run whose step failed, a shot it
publishes that is missing, or one taken while the video mode changed (its
geometry would not be pixel for pixel). Then, as capture.py does: the file is
checked over a scratch copy of the inventory and published with its shots into
the same store and manifest, all or nothing. One capture at a time, under
capture.py's lock (exit 4 when held). --from re-measures the shots of an
earlier run in DIR instead of running the VM again, for a scenario whose
measuring changed and whose steps did not.
"""
import argparse
import importlib.util
import json
import pathlib
import shutil
import subprocess
import sys
import time

HERE = pathlib.Path(__file__).resolve().parent


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


capture = load("capture", HERE / "capture.py")
core = capture.core
# Scenarios measure with this same module, so the core.Incomplete one raises is
# the class publish() catches.
sys.modules["capture_core"] = core
SCENARIOS = HERE / "host"
# deadline: ref-host.ps1 has no deadline of its own past its steps' waits, so
# the scenario's TIMEOUT bounds it; this margin covers the restore and the
# teardown, after which the VM is turned off from here.
HOST_MARGIN = 600


def build():
    return json.loads((capture.INVENTORY / "reference.json").read_text(encoding="utf-8"))["build"]


def run_host(label, steps, checkpoint, timeout, session=True):
    """Run ref-host.ps1 over `steps` on a fresh restore of `checkpoint`.
    Returns (output folder, exit code); 3 when it outlived its deadline (the VM
    is then turned off)."""
    stamp = time.strftime("%Y%m%d-%H%M%S")
    out = capture.WORK / f"{label}-{stamp}"
    plan = capture.WORK / f"{label}-{stamp}-steps.json"
    plan.write_text(json.dumps(steps, indent=1), encoding="utf-8")
    runner = ["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File"]
    try:
        r = subprocess.run(runner + [capture.winpath(HERE / "ref-host.ps1"), "-Steps", capture.winpath(plan),
                                     "-Out", capture.winpath(out), "-Checkpoint", checkpoint] + ([] if session else ["-NoSession"]),
                           capture_output=True, text=True, timeout=timeout + HOST_MARGIN)
    except subprocess.TimeoutExpired:
        stop = runner + [capture.winpath(HERE / "ref-run.ps1")]
        print(f"host-capture: ref-host did not finish within {timeout + HOST_MARGIN} s; " + capture.stop_vm(stop))
        return out, 3
    finally:
        plan.unlink(missing_ok=True)
    print(r.stdout.replace("\r", "").strip())
    if r.returncode and r.stderr.strip():
        print(f"host-capture: ref-host exit {r.returncode}: " + r.stderr.replace("\r", "").strip())
    return out, r.returncode


def taken(out, rc, wanted):
    """The shots ref-host wrote, checked: {name: entry}. Raises core.Incomplete
    on a failed step, a missing shot or one whose mode changed under it."""
    index = out / "shots.json"
    if not index.is_file():
        raise core.Incomplete(f"ref-host wrote no shots.json (exit {rc})")
    doc = json.loads(index.read_text(encoding="utf-8-sig"))
    if doc.get("failed"):
        raise core.Incomplete(f"ref-host stopped: {doc['failed']}")
    if rc != 0:
        raise core.Incomplete(f"ref-host exit {rc}")
    shots = {s["name"]: s for s in doc.get("shots", [])}
    missing = [n for n in wanted if n not in shots]
    if missing:
        raise core.Incomplete(f"no shot of {', '.join(missing)}")
    moved = [n for n in wanted if not shots[n].get("exact")]
    if moved:
        raise core.Incomplete(f"the video mode changed during {', '.join(moved)}")
    return {n: shots[n] for n in wanted}


def checked(records, surface, at):
    """Refuse a record that does not say it was taken by hand from this build."""
    for r in records:
        if r.get("surface") != surface or r.get("source") != f"hand:{at}" or r.get("capturedFrom") != at:
            raise ValueError(f"{r.get('id')}: a host record is surface {surface}, source hand:{at}, capturedFrom {at}")
    return records


def scenario(name):
    return load(f"host_{name.replace('-', '_')}", SCENARIOS / f"{name}.py")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--scenario", required=True, help="a module in scripts/inventory/host/, without .py")
    ap.add_argument("--dry-run", action="store_true", help="capture and check, publish nothing")
    ap.add_argument("--from", dest="earlier", type=pathlib.Path, help="measure the shots of an earlier run in this folder")
    args = ap.parse_args(argv)
    if args.scenario != core.slug(args.scenario) or not (SCENARIOS / f"{args.scenario}.py").is_file():
        have = ", ".join(sorted(p.stem for p in SCENARIOS.glob("*.py")))
        ap.error(f"--scenario {args.scenario!r} names no module in {SCENARIOS} (there are: {have})")
    sc = scenario(args.scenario)
    if args.earlier is not None and not (args.earlier / "shots.json").is_file():
        ap.error(f"--from {args.earlier}: no shots.json there")
    try:
        with capture.exclusive(capture.LOCK):
            if capture.recover(capture.JOURNAL):
                print("host-capture: rolled back a publication that was interrupted; the previous one is restored")
            if args.earlier is not None:
                out, rc = args.earlier, 0
            else:
                out, rc = run_host(args.scenario, sc.STEPS, sc.CHECKPOINT, sc.TIMEOUT, getattr(sc, "SESSION", True))
            return publish(sc, out, rc, args.dry_run)
    except BlockingIOError:
        print(f"host-capture: another capture holds {capture.LOCK}; run one at a time")
        return 4


def publish(sc, out, rc, dry_run):
    try:
        shots = taken(out, rc, sc.SHOTS)
    except core.Incomplete as exc:
        print(f"host-capture: {exc}; nothing published")
        return 3
    at = build()
    images = {n: core.read_png(out / s["file"]) for n, s in shots.items()}
    try:
        records = checked(sc.records(images, at, shots), sc.SURFACE, at)
    except (ValueError, core.Incomplete) as exc:
        print(f"host-capture: {exc}; nothing published")
        return 3
    rel = pathlib.PurePosixPath(sc.SURFACE) / f"{sc.FILE}.json"
    code, report = capture.check_scratch({rel: records})
    print(report.strip())
    if code != 0:
        print("host-capture: the result fails check-inventory.py; nothing published")
        return 1
    if dry_run:
        print(f"host-capture: dry run, {len(records)} records checked, nothing published (shots in {out})")
        return 0
    # publish() takes screenshots from <out>/shots/<surface>/<state>.<profile>.png.
    lay = out / "shots" / sc.SURFACE
    lay.mkdir(parents=True, exist_ok=True)
    states = []
    for n, s in shots.items():
        shutil.copyfile(out / s["file"], lay / f"{n}.{sc.PROFILE}.png")
        states.append({"file": f"{n}.{sc.PROFILE}.png", "width": s["width"], "height": s["height"]})
    try:
        capture.publish(capture.INVENTORY / rel, records, sc.SURFACE, out, states, capture.SHOTS_MANIFEST,
                        capture.STORE, profile=sc.PROFILE, file=sc.FILE)
    except OSError as exc:
        print(f"host-capture: publishing failed ({exc}); the previous publication is unchanged")
        return 1
    print(f"host-capture: published {len(records)} records and {len(states)} shots to {capture.INVENTORY / rel}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
