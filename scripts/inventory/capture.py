#!/usr/bin/env python3
"""Capture a Windows 11 surface from the reference VM and publish its records.

Owner: todo/08-graphics-ui/TODO-17-win11-parity-inventory.md section 3.

Runs on the host (WSL), drives the VM through ref-run.ps1:

    python3 scripts/inventory/capture.py --surface taskbar --file bar [--merge] [--dry-run]
    python3 scripts/inventory/capture.py --verify-shots

1. Stages capture-ui.py, capture_core.py, the surface module and profiles.json,
   and runs capture-ui.py on a fresh restore of clean.
   One capture runs at a time (build/inventory/capture.lock, exit 4 when held),
   and ref-run gets its timeout plus a margin before the host gives up and runs
   ref-run -Stop, which turns the VM off only when no live run holds it.
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
changed file. --dry-run does steps 1 to 4 and publishes nothing. --fixture also
keeps the walked tree (tree.json in the raw output) for a test fixture; it costs
one more walk, so an ordinary capture does not take it.

Publishing stages every file first (records, screenshots, manifest) and then
swaps them in, records last, keeping each replaced file aside until every swap
has succeeded: a failure at any point leaves the previous publication exactly as
it was.
"""
import argparse
import contextlib
import fcntl
import hashlib
import importlib.util
import json
import os
import pathlib
import shutil
import signal
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
# deadline: ref-run.ps1 bounds the guest command by --timeout and its session by
# 600 s; this margin covers restore, transfer and teardown on top, so ref-run's
# own finally (which turns the VM off) always gets to run before the host gives up.
RUN_MARGIN = 1800
LOCK = REPO / "build" / "inventory" / "capture.lock"
JOURNAL = LOCK.with_name("publish.journal")   # an unfinished swap, rolled back by the next run


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


def run_guest(surface, profile, timeout, fixture=False):
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
           f"--shots C:\\ios\\out\\shots --meta C:\\ios\\out\\meta.json")
    if fixture:
        cmd += " --dump C:\\ios\\out\\tree.json"
    runner = ["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", winpath(HERE / "ref-run.ps1")]
    try:
        r = subprocess.run(runner + ["-In", winpath(stage), "-Out", winpath(out), "-TimeoutSec", str(timeout),
                                     "-Command", cmd], capture_output=True, text=True, timeout=timeout + RUN_MARGIN)
    except subprocess.TimeoutExpired:
        # ref-run outlived every bound of its own; turn the VM off unless a live run holds it.
        print(f"capture: ref-run did not finish within {timeout + RUN_MARGIN} s; " + stop_vm(runner))
        return out, 3
    except KeyboardInterrupt:
        # subprocess.run has killed the runner, whose own finally may not run.
        print("capture: interrupted; " + stop_vm(runner))
        raise
    finally:
        shutil.rmtree(stage, ignore_errors=True)
    print(r.stdout.replace("\r", "").strip())
    return out, r.returncode


def stop_vm(runner):
    """ref-run -Stop: the VM off unless a live run still holds it. Returns its message."""
    try:
        stop = subprocess.run(runner + ["-Stop"], capture_output=True, text=True, timeout=600)  # deadline: Stop-VM -TurnOff
    except (OSError, subprocess.TimeoutExpired) as exc:
        return f"ref-run -Stop failed ({exc}); turn the VM off with ref-run.ps1 -Stop"
    return stop.stdout.replace("\r", "").strip() or f"ref-run -Stop exited {stop.returncode}"


def check_scratch(rel_file, records):
    """check-inventory.py over a copy of the inventory holding the new file."""
    scratch = pathlib.Path(tempfile.mkdtemp(prefix="capture-"))
    try:
        tree = scratch / "inventory"
        shutil.copytree(INVENTORY, tree)
        target = tree / rel_file
        if not target.resolve().is_relative_to(tree.resolve()):
            raise ValueError(f"{rel_file} is outside the inventory")
        target.parent.mkdir(parents=True, exist_ok=True)
        core.write_records(target, records)
        r = subprocess.run([sys.executable, str(HERE / "check-inventory.py"), "--root", str(tree)],
                           capture_output=True, text=True, timeout=600)
        return r.returncode, r.stdout + r.stderr
    finally:
        shutil.rmtree(scratch)


def received(out, rc):
    """(meta, records) from the guest's output, or raise Incomplete.

    A capture is taken only whole: the runner exited 0, meta.json says complete,
    and records.json holds exactly the records meta.json counted. A transfer
    that stopped part way can leave a complete meta.json beside a short or
    missing records file, and the checker cannot tell a short file from a
    small surface.
    """
    meta_path, rec_path = out / "meta.json", out / "records.json"
    if not meta_path.is_file():
        raise core.Incomplete(f"the guest wrote no meta.json (ref-run exit {rc})")
    try:
        meta = json.loads(meta_path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise core.Incomplete(f"meta.json is unreadable ({exc})") from None
    if not meta.get("complete"):
        raise core.Incomplete(f"incomplete ({meta.get('incomplete', 'no reason given')})")
    if rc != 0:
        raise core.Incomplete(f"ref-run exited {rc} after a complete capture; the transfer may be partial")
    if not rec_path.is_file():
        raise core.Incomplete("records.json was not received")
    try:
        new = core.read_records(rec_path)
    except (OSError, ValueError) as exc:
        raise core.Incomplete(f"records.json is unreadable ({exc})") from None
    want = meta.get("records")
    if not isinstance(want, int) or want < 1 or len(new) != want:
        raise core.Incomplete(f"received {len(new)} records, meta.json counted {want}")
    return meta, new


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


def roll_back(entries):
    """Put every file of an unfinished swap back as it was before it started.

    An entry's .old copy exists exactly while its destination is moved aside, so
    restoring every .old copy and removing every destination that did not exist
    before undoes any prefix of the swap, however it was interrupted.
    """
    for e in reversed(entries):
        final, old = pathlib.Path(e["final"]), pathlib.Path(e["old"])
        if old.exists():
            os.replace(old, final)
        elif not e["had"]:
            final.unlink(missing_ok=True)
    for e in entries:
        pathlib.Path(e["tmp"]).unlink(missing_ok=True)


def recover(journal):
    """Undo a publication that was killed part way. Call under the capture lock."""
    if not journal.exists():
        return False
    roll_back(json.loads(journal.read_text(encoding="utf-8")))
    journal.unlink()
    return True


def publish(target, records, surface, out, states, manifest_path, store, journal=None):
    """Publish records and screenshots together, or leave the old publication alone.

    Stage: copy every screenshot next to its destination under a temporary name,
    write the new manifest and the new records beside theirs. Then swap: each
    existing destination is first moved aside to <name>.old, then the staged file
    takes its place. The swap is written to `journal` before it starts, and
    removing the journal is its commit point; the .old copies go only after it.
    A failure puts every file back (roll_back) and re-raises; a kill, which no
    handler sees, leaves the journal, and the next run's recover() rolls back.
    """
    journal = journal or JOURNAL
    names = [s["file"] for s in states]
    if len(set(names)) != len(names):
        raise OSError(f"two screenshots of {surface} share a file name; nothing staged")
    manifest = [e for e in load_manifest(manifest_path) if e["surface"] != surface]
    dest = store / surface
    staged = []
    try:
        dest.mkdir(parents=True, exist_ok=True)
        target.parent.mkdir(parents=True, exist_ok=True)
        for s in states:
            src = out / "shots" / surface / s["file"]
            state, profile = s["file"][: -len(".png")].rsplit(".", 1)
            tmp = dest / f".{s['file']}.{os.getpid()}.tmp"
            shutil.copyfile(src, tmp)
            staged.append((tmp, dest / s["file"]))
            if sha256(tmp) != sha256(src):
                raise OSError(f"{tmp}: the copy differs from {src}")
            manifest.append({"surface": surface, "state": state, "profile": profile, "width": s["width"],
                             "height": s["height"], "size": src.stat().st_size, "sha256": sha256(src)})
        manifest.sort(key=lambda e: (e["surface"], e["state"], e["profile"]))
        man_tmp = manifest_path.with_name(f"{manifest_path.name}.{os.getpid()}.tmp")
        man_tmp.write_text(json.dumps(manifest, indent=1) + "\n", encoding="utf-8")
        staged.append((man_tmp, manifest_path))
        rec_tmp = target.with_name(f"{target.name}.{os.getpid()}.staged")
        core.write_records(rec_tmp, records)
        staged.append((rec_tmp, target))
    except BaseException:
        for tmp, _ in staged:
            tmp.unlink(missing_ok=True)
        raise
    entries = [{"tmp": str(tmp), "final": str(final), "old": str(final.with_name(f"{final.name}.{os.getpid()}.old")),
                "had": final.exists()} for tmp, final in staged]
    try:
        journal.parent.mkdir(parents=True, exist_ok=True)
        jtmp = journal.with_name(journal.name + ".tmp")
        with open(jtmp, "w", encoding="utf-8") as fh:
            json.dump(entries, fh)
            fh.flush()
            os.fsync(fh.fileno())
        os.replace(jtmp, journal)
        for e in entries:
            if e["had"]:
                os.replace(e["final"], e["old"])
            os.replace(e["tmp"], e["final"])
    except BaseException:
        roll_back(entries)
        journal.unlink(missing_ok=True)
        raise
    journal.unlink()                               # the commit point
    for e in entries:
        pathlib.Path(e["old"]).unlink(missing_ok=True)


@contextlib.contextmanager
def exclusive(path):
    """Hold the capture lock for a whole run, from the guest to the last swap.

    ref-run's VM lock ends when the guest does; this one also covers the merge,
    the check and publication, which read and rewrite shared files (shots.json,
    a surface's record file). Raises BlockingIOError when another run holds it.
    """
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w") as fh:
        fcntl.flock(fh, fcntl.LOCK_EX | fcntl.LOCK_NB)
        yield


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--surface")
    ap.add_argument("--file", help="the record file under the surface folder, without .json")
    ap.add_argument("--profile", default="fhd-100")
    ap.add_argument("--merge", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--fixture", action="store_true", help="also keep the walked tree, for a test fixture")
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
    for flag, value in (("--surface", args.surface), ("--file", args.file)):
        if value != core.slug(value):
            ap.error(f"{flag} {value!r} must be a slug (lower-case letters, digits, single hyphens), not a path")
    if args.file.startswith("baseline"):
        # check-inventory.py reads a baseline* file as completeness rows and skips its records.
        ap.error(f"--file {args.file!r}: names starting with baseline are completeness baselines, not record files")
    # A terminated capture cleans up as an interrupted one does (stop_vm, the
    # publication rollback), instead of dying with the VM running.
    signal.signal(signal.SIGTERM, signal.default_int_handler)
    try:
        with exclusive(LOCK):
            return capture_and_publish(args)
    except BlockingIOError:
        print(f"capture: another capture holds {LOCK}; run one at a time")
        return 4


def capture_and_publish(args):
    if recover(JOURNAL):
        print("capture: rolled back a publication that was interrupted; the previous one is restored")
    out, rc = run_guest(args.surface, args.profile, args.timeout, args.fixture)
    if rc == 2:
        return 2
    try:
        meta, new = received(out, rc)
    except core.Incomplete as exc:
        print(f"capture: {exc}; nothing published, nothing removed")
        return 3
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
    try:
        publish(INVENTORY / rel, records, args.surface, out, meta.get("states", []), SHOTS_MANIFEST, STORE)
    except OSError as exc:
        print(f"capture: publishing failed ({exc}); the previous publication is unchanged")
        return 1
    unnamed = REPO / "build" / "inventory" / f"{args.surface}-unnamed.json"
    unnamed.parent.mkdir(parents=True, exist_ok=True)
    unnamed.write_text(json.dumps(meta.get("unnamed", []), indent=1) + "\n", encoding="utf-8")
    print(f"capture: published {len(records)} records to {INVENTORY / rel}; {len(meta.get('unnamed', []))} need a hand label ({unnamed})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
