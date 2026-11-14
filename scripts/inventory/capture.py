#!/usr/bin/env python3
"""Capture a Windows 11 surface from the reference VM and publish its records.

Owner: todo/08-graphics-ui/TODO-17-win11-parity-inventory.md section 3.

Runs on the host (WSL), drives the VM through ref-run.ps1:

    python3 scripts/inventory/capture.py --surface taskbar --file bar [--merge] [--dry-run]
    python3 scripts/inventory/capture.py --verify-shots

1. Stages capture-ui.py, capture_core.py, the surface module and profiles.json,
   and runs capture-ui.py on a fresh restore of clean.
2. Refuses a capture that says it is incomplete (exit 3): nothing is published
   and nothing is deleted from what was published before.
3. Builds the new file: without --merge an existing file is refused (exit 2); with
   --merge the hand-written fields (behaviour, disposition, inconsistency) are
   carried across by id, every captured fact is refreshed, and a record the
   capture did not see is dropped (named on stdout). A note that would land on an
   ambiguous id stops the merge.
4. Runs check-inventory.py over a scratch copy of the inventory holding the new
   file; a file that fails is not published.
5. Publishes docs/design/inventory/<surface>/<file>.json, moves the screenshots
   to the store (R:\\11\\ref-shots\\<surface>\\<state>.<profile>.png) and records
   each in docs/design/inventory/shots.json (surface, state, profile, size,
   SHA-256); writes build/inventory/<surface>-unnamed.json for hand labels.

--verify-shots re-hashes the store against shots.json and names every missing or
changed file. --dry-run does steps 1 to 4 and publishes nothing.
"""
import argparse
import hashlib
import importlib.util
import json
import pathlib
import shutil
import subprocess
import sys
import tempfile
import time

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parents[1]
_spec = importlib.util.spec_from_file_location("capture_core", HERE / "capture_core.py")
core = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(core)

INVENTORY = REPO / "docs" / "design" / "inventory"
SHOTS_MANIFEST = INVENTORY / "shots.json"
STORE = pathlib.Path("/mnt/r/11/ref-shots")
WORK = pathlib.Path("/mnt/r/11/captures")
RUN_TIMEOUT = 3600   # deadline: ref-run.ps1 holds its own per-command timeout inside this


def winpath(p):
    return subprocess.run(["wslpath", "-w", str(p)], capture_output=True, text=True, check=True, timeout=30).stdout.strip()


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for block in iter(lambda: fh.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def load_manifest(path):
    return json.loads(path.read_text(encoding="utf-8")) if path.exists() else []


def verify_shots(manifest, store):
    """Messages for every manifest entry whose stored file is missing or changed."""
    out = []
    for e in manifest:
        f = store / e["surface"] / f"{e['state']}.{e['profile']}.png"
        if not f.is_file():
            out.append(f"missing: {f}")
        elif f.stat().st_size != e["size"] or sha256(f) != e["sha256"]:
            out.append(f"changed: {f}")
    return out


def run_guest(surface, profile, timeout):
    """Run capture-ui.py in the VM. Returns the output folder."""
    stamp = time.strftime("%Y%m%d-%H%M%S")
    stage = WORK / f"{surface}-{stamp}-in"
    out = WORK / f"{surface}-{stamp}"
    (stage / "surfaces").mkdir(parents=True)
    for f in ("capture-ui.py", "capture_core.py"):
        shutil.copy(HERE / f, stage / f)
    shutil.copy(HERE / "surfaces" / f"{surface}.py", stage / "surfaces" / f"{surface}.py")
    shutil.copy(INVENTORY / "profiles.json", stage / "profiles.json")
    cmd = (f"python C:\\ios\\in\\capture-ui.py --surface {surface} --profile {profile} --out C:\\ios\\out\\records.json "
           f"--shots C:\\ios\\out\\shots --meta C:\\ios\\out\\meta.json --dump C:\\ios\\out\\tree.json")
    r = subprocess.run(["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", winpath(HERE / "ref-run.ps1"),
                        "-In", winpath(stage), "-Out", winpath(out), "-TimeoutSec", str(timeout), "-Command", cmd],
                       capture_output=True, text=True, timeout=RUN_TIMEOUT)
    shutil.rmtree(stage)
    print(r.stdout.replace("\r", "").strip())
    return out, r.returncode


def check_scratch(rel_file, records):
    """check-inventory.py over a copy of the inventory holding the new file."""
    scratch = pathlib.Path(tempfile.mkdtemp(prefix="capture-"))
    try:
        tree = scratch / "inventory"
        shutil.copytree(INVENTORY, tree)
        target = tree / rel_file
        target.parent.mkdir(parents=True, exist_ok=True)
        core.write_records(target, records)
        r = subprocess.run([sys.executable, str(HERE / "check-inventory.py"), "--root", str(tree)],
                           capture_output=True, text=True, timeout=600)
        return r.returncode, r.stdout + r.stderr
    finally:
        shutil.rmtree(scratch)


def build_file(target, new, meta, do_merge):
    """The records to publish, or raise SystemExit with the reason and exit code."""
    if not meta.get("complete"):
        print(f"capture: incomplete ({meta.get('incomplete', 'no reason given')}); nothing published, nothing removed")
        raise SystemExit(3)
    if target.exists() and not do_merge:
        print(f"capture: {target} exists; pass --merge to refresh it and keep its hand-written fields")
        raise SystemExit(2)
    old = core.read_records(target) if target.exists() else []
    try:
        records, dropped = core.merge(old, new, meta.get("ambiguous", []))
    except ValueError as exc:
        print(f"capture: {exc}")
        raise SystemExit(1) from None
    for rid in dropped:
        print(f"capture: dropped {rid}, which the capture no longer sees, with its hand-written fields")
    return records


def publish_shots(surface, out, states, manifest_path, store):
    manifest = [e for e in load_manifest(manifest_path) if e["surface"] != surface]
    dest = store / surface
    dest.mkdir(parents=True, exist_ok=True)
    for s in states:
        src = out / "shots" / surface / s["file"]
        state, profile = s["file"][: -len(".png")].rsplit(".", 1)
        shutil.copy(src, dest / s["file"])
        manifest.append({"surface": surface, "state": state, "profile": profile, "width": s["width"],
                         "height": s["height"], "size": src.stat().st_size, "sha256": sha256(src)})
    manifest.sort(key=lambda e: (e["surface"], e["state"], e["profile"]))
    manifest_path.write_text(json.dumps(manifest, indent=1) + "\n", encoding="utf-8")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--surface")
    ap.add_argument("--file", help="the record file under the surface folder, without .json")
    ap.add_argument("--profile", default="fhd-100")
    ap.add_argument("--merge", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--verify-shots", action="store_true")
    ap.add_argument("--timeout", type=int, default=900)
    args = ap.parse_args(argv)
    if args.verify_shots:
        bad = verify_shots(load_manifest(SHOTS_MANIFEST), STORE)
        for b in bad:
            print(b)
        print(f"shots: {len(load_manifest(SHOTS_MANIFEST))} in the manifest, {len(bad)} missing or changed")
        return 1 if bad else 0
    if not args.surface or not args.file:
        ap.error("--surface and --file are required")
    out, rc = run_guest(args.surface, args.profile, args.timeout)
    meta_path = out / "meta.json"
    if rc == 2:
        return 2
    if not meta_path.is_file():
        print(f"capture: the guest wrote no meta.json (ref-run exit {rc})")
        return 3
    meta = json.loads(meta_path.read_text(encoding="utf-8"))
    new = core.read_records(out / "records.json") if (out / "records.json").is_file() else []
    rel = pathlib.PurePosixPath(core.slug(args.surface)) / f"{args.file}.json"
    try:
        records = build_file(INVENTORY / rel, new, meta, args.merge)
    except SystemExit as exc:
        return exc.code
    code, report = check_scratch(rel, records)
    print(report.strip())
    if code != 0:
        print("capture: the result fails check-inventory.py; nothing published")
        return 1
    if args.dry_run:
        print(f"capture: dry run, {len(records)} records checked, nothing published (raw output in {out})")
        return 0
    core.write_records(INVENTORY / rel, records)
    publish_shots(args.surface, out, meta.get("states", []), SHOTS_MANIFEST, STORE)
    unnamed = REPO / "build" / "inventory" / f"{args.surface}-unnamed.json"
    unnamed.parent.mkdir(parents=True, exist_ok=True)
    unnamed.write_text(json.dumps(meta.get("unnamed", []), indent=1) + "\n", encoding="utf-8")
    print(f"capture: published {len(records)} records to {INVENTORY / rel}; {len(meta.get('unnamed', []))} need a hand label ({unnamed})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
