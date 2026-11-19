#!/usr/bin/env python3
"""Capture the screens UI Automation cannot reach, from the host, and publish their records.

Owner: todo/08-graphics-ui/TODO-17-win11-parity-inventory.md section 9.

    python3 scripts/inventory/host-capture.py --scenario session-secure [--dry-run] [--from DIR]

A scenario is scripts/inventory/host/<name>.py. It names SURFACE, FILE,
PROFILE, CHECKPOINT and TIMEOUT (and SESSION = False for a checkpoint with no
account to open a session as), the STEPS ref-host.ps1 runs on the reference
VM (keys, clicks, guest commands, shots), SHOTS (the shots it publishes),
MOVING (optional: prefixes of bursts across a mode change; only their numbered frames, <prefix><n>, may be inexact), PROOF
(optional: a shot and the guest's own screenshot of the same screen, fetched
into the run folder, which must match it pixel for pixel after the 16-bit
rounding of Hyper-V's host path) and
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
    is then turned off and restored to `checkpoint`, as on an interrupt). The folder's build.json
    names the reference build the shots are of."""
    stamp = time.strftime("%Y%m%d-%H%M%S")
    out = capture.WORK / f"{label}-{stamp}"
    # A folder that exists holds another run's shots (two runs in one second):
    # stamping its build.json would relabel them, so nothing runs (review,
    # 2026-11-19: ref-host refused it, and the build was written anyway).
    if out.exists():
        print(f"host-capture: {out} exists already; nothing run")
        return out, 2
    at = build()             # read before the run: the build the restored checkpoint is
    plan = capture.WORK / f"{label}-{stamp}-steps.json"
    plan.write_text(json.dumps(steps, indent=1), encoding="utf-8")
    runner = ["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File"]
    try:
        r = subprocess.run(runner + [capture.winpath(HERE / "ref-host.ps1"), "-Steps", capture.winpath(plan),
                                     "-Out", capture.winpath(out), "-Checkpoint", checkpoint] + ([] if session else ["-NoSession"]),
                           capture_output=True, text=True, timeout=timeout + HOST_MARGIN)
    except subprocess.TimeoutExpired:
        # -Stop restores the checkpoint too: the killed walk's teardown never ran.
        stop = runner + [capture.winpath(HERE / "ref-run.ps1")]
        print(f"host-capture: ref-host did not finish within {timeout + HOST_MARGIN} s; " + capture.stop_vm(stop, checkpoint))
        return out, 3
    except KeyboardInterrupt:
        # subprocess.run has killed ref-host, whose finally may not have run, so
        # the boot walk's fixture could outlive it (review, 2026-11-18).
        stop = runner + [capture.winpath(HERE / "ref-run.ps1")]
        print("host-capture: interrupted; " + capture.stop_vm(stop, checkpoint))
        raise
    finally:
        plan.unlink(missing_ok=True)
    if out.is_dir():         # ref-host refuses a folder that is not empty, so this comes after
        (out / "build.json").write_text(json.dumps({"build": at}), encoding="utf-8")
    print(r.stdout.replace("\r", "").strip())
    if r.returncode and r.stderr.strip():
        print(f"host-capture: ref-host exit {r.returncode}: " + r.stderr.replace("\r", "").strip())
    return out, r.returncode


# The revision of ref-common.ps1's Save-Thumbnail that wrote the shots. Shots
# from revision 1 (no field) sit 2 pixels right with Hyper-V's 4 leading bytes
# as their first pixels, so a --from replay of one is refused (2026-11-18).
THUMBNAIL = 2


def burst_frame(name, moving):
    """True when name is a numbered frame of one of the `moving` bursts:
    <burst>-<n>, never a still shot that merely shares the prefix."""
    return any(name.startswith(p) and name[len(p):].isdigit() for p in moving)


def taken(out, rc, wanted, moving=(), early=()):
    """The shots ref-host wrote, checked: {name: entry}. Raises core.Incomplete
    on a failed step, shots from another thumbnail revision, a missing shot or
    one whose mode changed under it, unless it is a numbered frame of one of
    the `moving` bursts (`<prefix><n>`, a burst across a restart, where a mode
    change is the screen passing; it is kept with exact false and the scenario
    measures no geometry from it). A burst in `early` ends at the first frame
    that shows what it waits for, so its frames from that one on may be
    missing; its first frame never is, and a gap inside it is refused."""
    index = out / "shots.json"
    if not index.is_file():
        raise core.Incomplete(f"ref-host wrote no shots.json (exit {rc})")
    doc = json.loads(index.read_text(encoding="utf-8-sig"))
    if doc.get("thumbnail") != THUMBNAIL:
        raise core.Incomplete(f"the shots were read by thumbnail revision {doc.get('thumbnail', 1)}, not {THUMBNAIL}; capture them again")
    if doc.get("failed"):
        raise core.Incomplete(f"ref-host stopped: {doc['failed']}")
    if rc != 0:
        raise core.Incomplete(f"ref-host exit {rc}")
    shots = {s["name"]: s for s in doc.get("shots", [])}

    def ended(n):                    # a frame after the last one an early burst took
        for p in early:
            if burst_frame(n, (p,)):
                took = [int(m[len(p):]) for m in shots if burst_frame(m, (p,))]
                return bool(took) and int(n[len(p):]) > max(took)
        return False
    wanted = [n for n in wanted if n in shots or not ended(n)]
    missing = [n for n in wanted if n not in shots]
    if missing:
        raise core.Incomplete(f"no shot of {', '.join(missing)}")
    moved = [n for n in wanted if not shots[n].get("exact") and not burst_frame(n, moving)]
    if moved:
        raise core.Incomplete(f"the video mode changed during {', '.join(moved)}")
    return {n: shots[n] for n in wanted}


# Hyper-V's host path is 16-bit colour: 5 bits of red and blue, 6 of green,
# so a channel can differ from the guest's own screenshot by up to 8.
ROUNDING = 8
# A share of pixels allowed to differ beyond rounding: the taskbar clock may
# change its minute between the two screenshots.
PROOF_SHARE = 0.999


def mapping(host, guest):
    """(share of pixels within ROUNDING on every channel, largest difference)
    of two read_png images; raises core.Incomplete when their sizes differ."""
    if host[:2] != guest[:2]:
        raise core.Incomplete(f"the host shot is {host[0]}x{host[1]}, the guest's screenshot {guest[0]}x{guest[1]}")
    a, b = host[2], guest[2]
    worst = near = 0
    for i in range(0, len(a), 3):
        d = max(abs(a[i] - b[i]), abs(a[i + 1] - b[i + 1]), abs(a[i + 2] - b[i + 2]))
        worst = max(worst, d)
        near += d <= ROUNDING
    return near / (len(a) // 3), worst


def proven(sc, out, shots):
    """Check the scenario's PROOF: its host shot against the guest's screenshot."""
    if not hasattr(sc, "PROOF"):
        return
    name, file = sc.PROOF
    host = core.read_png(out / shots[name]["file"])
    guest = core.read_png(out / file)
    share, worst = mapping(host, guest)
    if share < PROOF_SHARE:
        raise core.Incomplete(f"the host shot {name} is not the guest's screen 1:1: {share:.2%} of pixels within {ROUNDING}")
    print(f"host-capture: {name} is the guest's screen 1:1 at {host[0]}x{host[1]}: {share:.3%} of pixels within "
          f"16-bit rounding, largest difference {worst}")


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


def captured_build(out):
    """The reference build the run in `out` shot, from the build.json run_host
    wrote; raises core.Incomplete when it is missing or not the build the
    inventory names now, so a --from replay never stamps old shots with a newer
    build (review, 2026-11-18)."""
    f = out / "build.json"
    if not f.is_file():
        raise core.Incomplete(f"{out} names no reference build (no build.json); capture it again")
    shot = json.loads(f.read_text(encoding="utf-8")).get("build")
    if shot != build():
        raise core.Incomplete(f"the shots are of build {shot}, the inventory is of {build()}; capture them again")
    return shot


def publish(sc, out, rc, dry_run):
    proof = [sc.PROOF[0]] if hasattr(sc, "PROOF") else []
    try:
        at = captured_build(out)
        shots = taken(out, rc, sc.SHOTS + proof, getattr(sc, "MOVING", ()), getattr(sc, "EARLY", ()))
        proven(sc, out, shots)
        # A burst that ended early has no frames past its last; taken() has
        # already refused a gap or a missing still (review, 2026-11-18).
        shots = {n: shots[n] for n in sc.SHOTS if n in shots}
    except core.Incomplete as exc:
        print(f"host-capture: {exc}; nothing published")
        return 3
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
        # Output size from the image itself; source size and depth from the
        # mode the video head reported (the host path is always 16-bit).
        w, h = images[n][:2]
        states.append({"file": f"{n}.{sc.PROFILE}.png", "width": w, "height": h, "depth": s["depth"],
                       "sourceWidth": s["modeBefore"][0], "sourceHeight": s["modeBefore"][1], "exact": bool(s.get("exact"))})
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
