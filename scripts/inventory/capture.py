#!/usr/bin/env python3
"""Capture a Windows 11 surface from the reference VM and publish its records.

Owner: todo/08-graphics-ui/TODO-17-win11-parity-inventory.md section 3.

Runs on the host (WSL), drives the VM through ref-run.ps1:

    python3 scripts/inventory/capture.py --surface taskbar --file bar [--merge] [--dry-run]
    python3 scripts/inventory/capture.py --verify-shots

1. Stages capture-ui.py, capture_core.py, the surface module and profiles.json,
   and runs capture-ui.py on a fresh restore of clean (of profile-<id> for a
   machine profile, which ref-vm.ps1 -Derive takes).
   One capture runs at a time (build/inventory/capture.lock, and D:\\11\\ref-shots.lock
   for every checkout that shares the shot store; exit 4 when either is held),
   and ref-run gets its timeout plus a margin before the host gives up and runs
   ref-run -Stop, which turns the VM off only when no live run holds it.
2. Refuses a capture that says it is incomplete (exit 3): nothing is published
   and nothing is deleted from what was published before.
3. Builds the new file: without --merge an existing file is refused (exit 2); with
   --merge the hand-written fields (behaviour, disposition, inconsistency and the
   baseline match, baselineId or baselineMissing) are
   carried across by id, every captured fact is refreshed, and a record the
   capture did not see is dropped (named on stdout), and a carried match to a
   baseline row the build no longer has is removed (named on stdout). A note that would land on an
   ambiguous id stops the merge.
4. Runs check-inventory.py over a scratch copy of the inventory holding the new
   file; a file that fails is not published.
5. Publishes docs/design/inventory/<surface>/<file>.json, moves the screenshots
   to the store (D:\\11\\ref-shots\\<surface>\\<state>.<profile>.<sha16>.png) and records
   each in docs/design/inventory/shots.json (surface, state, profile, size,
   SHA-256); writes build/inventory/<surface>-unnamed.json for hand labels.

--standin VARIANT runs step 1 on the stand-in machine (standin/run.py: the
clean disk under QEMU with a laptop's battery, lid and backlight, section 21)
instead of the reference VM, for the rows expected.json marks "stand-in". A
profile with two displays is captured there without asking: Hyper-V gives the
reference VM one display, so the variant with two heads ("dual") provides it
(capture_core.standin_variant, section 22).

--observe publishes the capture as observations instead (schema.json
$defs/observation): the measured facts of each record under --profile, written
whole to <surface>/<file>.<profile>.json beside the canonical <file>.json,
which must exist and is not touched, so one profile's refresh leaves every
other profile's file as it was.

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
FIXTURES = HERE / "fixtures"
SHOTS_MANIFEST = INVENTORY / "shots.json"
STORE = pathlib.Path("/mnt/d/11/ref-shots")
WORK = pathlib.Path("/mnt/d/11/captures")
# deadline: ref-run.ps1 bounds the guest command by --timeout and its session by
# 600 s; this margin covers restore, transfer and teardown on top, so ref-run's
# own finally (which turns the VM off) always gets to run before the host gives up.
RUN_MARGIN = 1800
STANDIN_STOP_GRACE = 120   # run.py's own teardown: QEMU's 30 s grace, twice, plus the reap
LOCK = REPO / "build" / "inventory" / "capture.lock"
JOURNAL = LOCK.with_name("publish.journal")   # an unfinished swap of checkout files only (baselines)
# The shot store is shared by every checkout on this machine; this lock beside it
# keeps two checkouts from publishing to it, or recovering into it, at once.
STORE_LOCK = STORE.with_name(STORE.name + ".lock")


def journal_for(store):
    """Where a swap into `store` journals itself: beside the shared store, so the
    next run from ANY checkout finds and rolls back a swap another one was killed
    in (review, 2026-11-21: a per-checkout journal let a second checkout publish
    over a half-swapped shot, which the first one's later rollback then undid).
    With no drive under the store (a CI runner) there is nothing shared, and the
    checkout's journal serves."""
    return store.with_name(store.name + ".journal") if store.parent.is_dir() else JOURNAL


def recover_pending():
    """Roll back every unfinished swap this run could meet: the shared store's
    and the checkout's. Call under held(). True when one was rolled back."""
    found = False
    for journal in dict.fromkeys((journal_for(STORE), JOURNAL)):
        found = recover(journal) or found
    return found


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


def shot_path(store, entry):
    """Where a manifest entry's screenshot lives: named by its digest, so a
    publish never replaces bytes another checkout's manifest still names
    (review, 2026-11-21: two checkouts share one store, and a path keyed by
    surface, state and profile alone let the second overwrite the first's shot).
    Identical bytes share one file."""
    return store / entry["surface"] / f"{entry['state']}.{entry['profile']}.{entry['sha256'][:SHOT_KEY]}.png"


SHOT_KEY = 16                     # hex digits of the SHA-256 in a stored shot's name


def legacy_shot_path(store, entry):
    return store / entry["surface"] / f"{entry['state']}.{entry['profile']}.png"


def migrate_store(manifest, store):
    """Copy each entry's shot from its old name (<state>.<profile>.png) to its
    digest name, when the bytes there are the ones the entry records. The old
    file stays for a checkout that has not moved yet. Returns (copied, missing)."""
    def holds(path, e):
        return path.is_file() and path.stat().st_size == e["size"] and sha256(path) == e["sha256"]
    copied, missing = 0, []
    for e in manifest:
        new, old = shot_path(store, e), legacy_shot_path(store, e)
        if holds(new, e):
            continue                      # a damaged copy is replaced, not counted as moved
        if holds(old, e):
            tmp = new.with_name(f".{new.name}.{os.getpid()}.tmp")
            shutil.copyfile(old, tmp)
            if sha256(tmp) != e["sha256"]:
                tmp.unlink()
                raise OSError(f"{tmp}: the copy differs from {old}")
            os.replace(tmp, new)
            copied += 1
        else:
            missing.append(str(old))
    return copied, missing


def verify_shots(manifest, store):
    """Messages for every manifest entry whose stored file is missing or changed."""
    out = []
    owners = {}
    for e in manifest:
        owners.setdefault((e["surface"], e["state"], e["profile"]), []).append(e.get("file"))
    for e in manifest:
        f = shot_path(store, e)
        files = owners[(e["surface"], e["state"], e["profile"])]
        if len(files) > 1:
            # One state, two record files: before digest names, the later
            # capture overwrote the other's shot (session secure and setup, 2026-11-18).
            msg = f"shared: {legacy_shot_path(store, e)} is claimed by {', '.join(sorted(str(x) for x in files))}"
            if msg not in out:
                out.append(msg)
        elif not f.is_file():
            out.append(f"missing: {f}")
        elif f.stat().st_size != e["size"] or sha256(f) != e["sha256"]:
            out.append(f"changed: {f}")
    return out


def run_guest(surface, profile, timeout, fixture=False, prepare=None, standin=None, module=None):
    """Run capture-ui.py in the VM (or on the stand-in machine in variant
    `standin`), after the fixture `prepare` (name:variant) when one is named.
    `module` is the surface module to walk (default: the one named after the
    surface). Returns (output folder, exit code)."""
    module = module or surface
    files = {"capture-ui.py": HERE / "capture-ui.py", "capture_core.py": HERE / "capture_core.py",
             f"surfaces/{module}.py": HERE / "surfaces" / f"{module}.py",
             "profiles.json": INVENTORY / "profiles.json",
             "probe-profile.ps1": HERE / "guest" / "probe-profile.ps1", "set-profile.ps1": HERE / "guest" / "set-profile.ps1"}
    # Helpers several modules share (one surface's window, menus and panes); a
    # module imports them by name, and capture-ui puts their folder on the path.
    files.update({f"surfaces/shared/{p.name}": p for p in sorted((HERE / "surfaces" / "shared").glob("*.py"))})
    cmd = (f"python C:\\ios\\in\\capture-ui.py --surface {surface} --module {module} --profile {profile} --out C:\\ios\\out\\records.json "
           f"--shots C:\\ios\\out\\shots --meta C:\\ios\\out\\meta.json")
    if fixture:
        cmd += " --dump C:\\ios\\out\\tree.json"
    if prepare:
        # The fixture runs first, in the same session; a failed fixture stops the
        # capture, since its records would describe the wrong state.
        name, _, variant = prepare.partition(":")
        files[f"fixtures/{name}.ps1"] = FIXTURES / f"{name}.ps1"
        run = f"powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\\ios\\in\\fixtures\\{name}.ps1"
        cmd = f"{run} -Variant {variant} && {cmd}" if variant else f"{run} && {cmd}"
    if standin:
        return standin_run(surface, files, cmd, timeout, standin)
    profiles = json.loads((INVENTORY / "profiles.json").read_text(encoding="utf-8"))
    return ref_run(surface, files, cmd, timeout, checkpoint=core.checkpoint_for(profile, profiles))


def standin_run(label, files, cmd, timeout, variant):
    """ref_run's contract on the stand-in machine: the same C:\\ios\\in layout and
    command, run by standin/run.py; its exit codes are ref_run's (2 refused,
    3 no result). The staging folder is always removed."""
    stamp = time.strftime("%Y%m%d-%H%M%S")
    stage = WORK / f"{label}-{stamp}-in"
    out = WORK / f"{label}-{stamp}"
    try:
        for rel, src in files.items():
            (stage / rel).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy(src, stage / rel)
        # run.py stops its own guest at --timeout; the margin covers boot and teardown.
        p = subprocess.Popen([sys.executable, str(HERE / "standin" / "run.py"), "--variant", variant, "--in", str(stage),
                              "--command", cmd, "--out", str(out), "--timeout", str(timeout)],
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        try:
            log, _ = p.communicate(timeout=timeout + RUN_MARGIN)
        except (subprocess.TimeoutExpired, KeyboardInterrupt) as exc:
            stop_runner(p)
            if isinstance(exc, KeyboardInterrupt):
                raise
            print(f"capture: standin/run.py did not finish within {timeout + RUN_MARGIN} s")
            return out, 3
    finally:
        shutil.rmtree(stage, ignore_errors=True)
    print(log.strip())
    return out, p.returncode


def stop_runner(p, grace=STANDIN_STOP_GRACE):
    """Stop standin/run.py: SIGTERM runs its teardown (QEMU stopped and reaped,
    overlay removed); SIGKILL only after `grace` seconds, and QEMU then still
    stops, since run.py starts it with a parent-death signal."""
    p.terminate()
    try:
        p.communicate(timeout=grace)
    except subprocess.TimeoutExpired:
        p.kill()
        p.communicate()


def ref_run(label, files, cmd, timeout, who="capture", checkpoint="clean"):
    """Stage `files` ({path under C:\\ios\\in: source}) and run `cmd` in the VM on a
    fresh restore of `checkpoint` (clean, or a machine profile's profile-<id>).
    Returns (output folder, exit code); 3 when ref-run
    outlived its margin (the VM is then turned off and `checkpoint` restored), and
    on an interrupt the same happens before the interrupt goes on. The staging folder is always removed."""
    stamp = time.strftime("%Y%m%d-%H%M%S")
    stage = WORK / f"{label}-{stamp}-in"
    out = WORK / f"{label}-{stamp}"
    try:
        # Staging and every path conversion come before ref-run starts: an
        # interrupt during either restored the checkpoint of a VM no run had
        # touched (review, 2026-11-19). Only a launched ref-run is recovered.
        runner = ["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", winpath(HERE / "ref-run.ps1")]
        for rel, src in files.items():
            (stage / rel).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy(src, stage / rel)
        argv = runner + ["-In", winpath(stage), "-Out", winpath(out), "-TimeoutSec", str(timeout),
                         "-Checkpoint", checkpoint, "-Command", cmd]
        try:
            r = subprocess.run(argv, capture_output=True, text=True, timeout=timeout + RUN_MARGIN)
        except subprocess.TimeoutExpired:
            # ref-run outlived every bound of its own; turn the VM off unless a live run holds it.
            print(f"{who}: ref-run did not finish within {timeout + RUN_MARGIN} s; " + stop_vm(runner, checkpoint))
            return out, 3
        except KeyboardInterrupt:
            # subprocess.run has killed the runner, whose own finally may not run.
            print(f"{who}: interrupted; " + stop_vm(runner, checkpoint))
            raise
    finally:
        shutil.rmtree(stage, ignore_errors=True)
    print(r.stdout.replace("\r", "").strip())
    # A failure ref-run did not catch (the guest command's deadline throws)
    # reaches only stderr; without it the capture would report the missing
    # output and not its cause.
    if r.returncode and r.stderr.strip():
        print(f"{who}: ref-run exit {r.returncode}: " + r.stderr.replace("\r", "").strip())
    return out, r.returncode


def stop_vm(runner, checkpoint="clean"):
    """ref-run -Stop -Checkpoint: unless a live run still holds the VM, turn it
    off and restore `checkpoint`, the one the killed run started from (a
    machine profile's profile-<id> too, review 2026-11-18). Returns its message."""
    try:
        stop = subprocess.run(runner + ["-Stop", "-Checkpoint", checkpoint], capture_output=True, text=True, timeout=600)  # deadline: Stop-VM -TurnOff and the restore
    except (OSError, subprocess.TimeoutExpired) as exc:
        return f"ref-run -Stop failed ({exc}); turn the VM off and restore it with ref-run.ps1 -Stop -Checkpoint {checkpoint}"
    return stop.stdout.replace("\r", "").strip() or f"ref-run -Stop exited {stop.returncode}"


def check_scratch(files):
    """check-inventory.py over a copy of the inventory holding the new files
    ({path under the inventory: lines})."""
    scratch = pathlib.Path(tempfile.mkdtemp(prefix="capture-"))
    try:
        tree = scratch / "inventory"
        shutil.copytree(INVENTORY, tree)
        for rel_file, records in files.items():
            target = tree / rel_file
            if not target.resolve().is_relative_to(tree.resolve()):
                raise ValueError(f"{rel_file} is outside the inventory")
            target.parent.mkdir(parents=True, exist_ok=True)
            core.write_records(target, records)
        # A fresh capture is undecided: its dispositions are written by hand afterwards
        # (section 6), and the commit check, which passes no such flag, requires them.
        # The scratch tree holds the inventory alone, so the design pages are named.
        r = subprocess.run([sys.executable, str(HERE / "check-inventory.py"), "--root", str(tree),
                            "--allow-undecided", "--design-root", str(INVENTORY.parent)],
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


def baseline_rows():
    """{baseline: its row ids} for the published baselines, or None when one
    cannot be read (then no match is touched and check-inventory.py reports the
    file). A baseline with no file has no entry, so matches to it are kept."""
    live = {}
    try:
        for path in sorted(INVENTORY.glob("*/baseline.json")):
            lines = path.read_text(encoding="utf-8").splitlines()[1:]
            live[path.parent.name] = {json.loads(line)["id"] for line in lines if line.strip()}
    except (OSError, ValueError, KeyError, TypeError):
        return None  # fail-direction: None keeps every match; the checker then refuses the unreadable baseline
    return live


def build_file(target, new, meta, do_merge):
    """The records to publish, or raise SystemExit with the reason and exit code."""
    if not meta.get("complete"):
        print(f"capture: incomplete ({meta.get('incomplete', 'no reason given')}); nothing published, nothing removed")
        raise SystemExit(3)
    if target.exists() and not do_merge:
        print(f"capture: {target} exists; pass --merge to refresh it and keep its hand-written fields")
        raise SystemExit(2)
    old = core.read_records(target) if target.exists() else []
    live = baseline_rows()
    unbound = []
    if live is not None:
        # The capture's own matches first: one to a row the baseline lacks is
        # no match, so it must not displace a carried "no row lists it".
        new, unbound = core.unbind(new, live, owned=True)
    try:
        records, dropped = core.merge(old, new, meta.get("ambiguous", []))
    except ValueError as exc:
        print(f"capture: {exc}")
        raise SystemExit(1) from None
    if live is not None:
        records, carried = core.unbind(records, live)
        for rid in unbound + carried:
            print(f"capture: {rid} matched a baseline row this build no longer has; its match is removed")
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


# What a host shot adds to its manifest entry: the bit depth of the image and
# the size of the screen it was taken from (section 9: a host shot records
# source size, output size and depth).
SHOT_DETAIL = ("depth", "sourceWidth", "sourceHeight", "exact")


def publish(target, records, surface, out, states, manifest_path, store, *, profile, journal=None, file=None):
    """Publish records and screenshots together, or leave the old publication alone.

    Stage: copy every screenshot next to its destination under a temporary name,
    write the new manifest and the new records beside theirs. Then swap: each
    existing destination is first moved aside to <name>.old, then the staged file
    takes its place. The swap is written to `journal` before it starts, and
    removing the journal is its commit point; the .old copies go only after it.
    A failure puts every file back (roll_back) and re-raises; a kill, which no
    handler sees, leaves the journal, and the next run's recover() rolls back.
    Each manifest entry names the record file it was taken for; this run replaces
    only the entries of its own surface, file and profile, so a capture of
    another file or profile keeps its screenshots on record. `file` names the
    record file the screenshots belong to when the target is not that file
    (an observation file <file>.<profile>.json belongs to <file>).
    """
    journal = journal or journal_for(store)
    names = [s["file"] for s in states]
    if len(set(names)) != len(names):
        raise OSError(f"two screenshots of {surface} share a file name; nothing staged")
    file = file or target.stem
    owner = (surface, file, profile)
    manifest = [e for e in load_manifest(manifest_path)
                if (e["surface"], e.get("file"), e["profile"]) != owner]
    # The store path is <surface>/<state>.<profile>.png, so a state another
    # record file of this surface already published would overwrite its shot
    # (session secure and setup both had first-desktop and network, 2026-11-18).
    taken = {(e["state"], e["profile"]): e.get("file") for e in manifest if e["surface"] == surface}
    clash = sorted(f"{n} (published for {taken[k]})" for n in names
                   if (k := tuple(n[: -len(".png")].rsplit(".", 1))) in taken)
    if clash:
        raise OSError(f"{surface}/{file}: another record file owns {', '.join(clash)}; nothing staged")
    dest = store / surface
    staged = []
    try:
        dest.mkdir(parents=True, exist_ok=True)
        target.parent.mkdir(parents=True, exist_ok=True)
        for s in states:
            src = out / "shots" / surface / s["file"]
            state, p = s["file"][: -len(".png")].rsplit(".", 1)
            if p != profile:
                raise OSError(f"{s['file']}: taken at {p}, but this capture is {profile}")
            digest = sha256(src)
            entry = {"surface": surface, "file": file, "state": state, "profile": p, "width": s["width"],
                     "height": s["height"], "size": src.stat().st_size, "sha256": digest,
                     **{k: s[k] for k in SHOT_DETAIL if k in s}}
            final = shot_path(store, entry)
            manifest.append(entry)
            if final.is_file() and sha256(final) == digest:
                continue                  # these bytes are stored already; nothing to swap
            tmp = dest / f".{final.name}.{os.getpid()}.tmp"
            shutil.copyfile(src, tmp)
            staged.append((tmp, final))
            if sha256(tmp) != digest:
                raise OSError(f"{tmp}: the copy differs from {src}")
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
    swap(staged, journal)


def swap(staged, journal):
    """Move each staged (tmp, final) pair into place, all or none.

    Each existing destination is first moved aside to <name>.<pid>.old, then the
    staged file takes its place, in order. The swap is written to `journal`
    before it starts, and removing the journal is its commit point; the .old
    copies go only after it. A failure puts every file back (roll_back) and
    re-raises; a kill leaves the journal for the next run's recover().
    """
    entries = [{"tmp": str(tmp), "final": str(final), "old": str(final.with_name(f"{final.name}.{os.getpid()}.old")),
                "had": final.exists()} for tmp, final in staged]
    jtmp = journal.with_name(journal.name + ".tmp")
    try:
        journal.parent.mkdir(parents=True, exist_ok=True)
        with open(jtmp, "w", encoding="utf-8") as fh:
            json.dump(entries, fh)
            fh.flush()
            os.fsync(fh.fileno())
        os.replace(jtmp, journal)
        for e in entries:
            if e["had"]:
                os.replace(e["final"], e["old"])
            os.replace(e["tmp"], e["final"])
        journal.unlink()                           # the commit point: a failure here still rolls back
    except BaseException:
        roll_back(entries)
        journal.unlink(missing_ok=True)
        jtmp.unlink(missing_ok=True)
        raise
    # Committed. A replaced copy that cannot be removed is litter, never a failed
    # publication: reporting one would say "unchanged" about files that changed.
    for e in entries:
        try:
            pathlib.Path(e["old"]).unlink(missing_ok=True)
        except OSError as exc:
            print(f"capture: published; could not remove the replaced copy {e['old']} ({exc})")


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


@contextlib.contextmanager
def held(store=True):
    """The checkout's capture lock, then (store=True) the shot store's: what
    publication and swap recovery run under. Raises BlockingIOError naming the
    lock another job holds. With no drive under the store (a CI runner) there is
    no shared store to guard, and a publication into it fails on its own."""
    with contextlib.ExitStack() as stack:
        try:
            stack.enter_context(exclusive(LOCK))
        except BlockingIOError:
            raise BlockingIOError(f"another job holds {LOCK}") from None
        if store:
            stack.enter_context(store_held(wait=0))
        yield


STORE_WAIT = 120                          # s a finished capture waits for another checkout's publication


@contextlib.contextmanager
def store_held(wait=STORE_WAIT):
    """The shot store's lock alone, for recovery and publication only: a guest
    run in one checkout never holds it, so another checkout's publication does
    not fail for the length of a capture (review, 2026-11-21). A publication
    waits up to `wait` s for one in progress elsewhere."""
    if not STORE_LOCK.parent.is_dir():
        yield
        return
    deadline = time.monotonic() + wait
    with contextlib.ExitStack() as stack:
        while True:
            try:
                stack.enter_context(exclusive(STORE_LOCK))
                break
            except BlockingIOError:
                if time.monotonic() >= deadline:
                    raise BlockingIOError(f"another job holds {STORE_LOCK}") from None
                time.sleep(1)
        yield


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--surface")
    ap.add_argument("--file", help="the record file under the surface folder, without .json")
    ap.add_argument("--module", help="the surface module that writes this file (default: the one named after --surface)")
    ap.add_argument("--profile", default="fhd-100")
    ap.add_argument("--merge", action="store_true")
    ap.add_argument("--observe", action="store_true",
                    help="publish the capture as observations of the canonical records, in <file>.<profile>.json")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--fixture", action="store_true", help="also keep the walked tree, for a test fixture")
    ap.add_argument("--verify-shots", action="store_true")
    ap.add_argument("--migrate-store", action="store_true",
                    help="copy each shot in shots.json from its old name to its digest name, once per store")
    ap.add_argument("--prepare", metavar="NAME[:VARIANT]", help="run this capture fixture before the walk")
    ap.add_argument("--standin", metavar="VARIANT", help="capture on the stand-in machine in this variant")
    ap.add_argument("--timeout", type=int, default=900)
    args = ap.parse_args(argv)
    if args.migrate_store:
        try:
            with held():
                copied, missing = migrate_store(load_manifest(SHOTS_MANIFEST), STORE)
        except BlockingIOError as exc:
            print(f"capture: {exc}; run one at a time")
            return 4
        for m in missing:
            print(f"missing: {m}")
        print(f"shots: {copied} copied to their digest names, {len(missing)} not found as recorded")
        return 1 if missing else 0
    if args.verify_shots:
        bad = verify_shots(load_manifest(SHOTS_MANIFEST), STORE)
        for b in bad:
            print(b)
        print(f"shots: {len(load_manifest(SHOTS_MANIFEST))} in the manifest, {len(bad)} missing, changed or shared")
        return 1 if bad else 0
    if not args.surface or not args.file:
        ap.error("--surface and --file are required")
    args.module = args.module or args.surface
    for flag, value in (("--surface", args.surface), ("--file", args.file), ("--module", args.module)):
        if value != core.slug(value):
            ap.error(f"{flag} {value!r} must be a slug (lower-case letters, digits, single hyphens), not a path")
    # Refused here, before the VM starts: a walker this checkout lacks, or a
    # profile profiles.json does not define, cannot be captured.
    if not (HERE / "surfaces" / f"{args.module}.py").is_file():
        walkers = sorted(p.stem for p in (HERE / "surfaces").glob("*.py"))
        ap.error(f"--module {args.module!r} has no walker in scripts/inventory/surfaces/ (there are: {', '.join(walkers)})")
    if args.prepare and core.fixture_problem(FIXTURES, args.prepare):
        ap.error(f"--prepare: {core.fixture_problem(FIXTURES, args.prepare)}")
    variants = json.loads((HERE / "standin" / "variants.json").read_text(encoding="utf-8"))
    if args.standin is not None and args.standin not in variants:
        ap.error(f"--standin {args.standin!r} is not in scripts/inventory/standin/variants.json "
                 f"(there are: {', '.join(variants)})")
    profiles = json.loads((INVENTORY / "profiles.json").read_text(encoding="utf-8"))
    if args.profile not in profiles:
        ap.error(f"--profile {args.profile!r} is not in docs/design/inventory/profiles.json")
    args.standin, problem = core.standin_variant(args.profile, profiles[args.profile], variants, args.standin)
    if problem:
        ap.error(problem)
    if args.observe and args.merge:
        ap.error("--observe rewrites the observation file whole; --merge applies to a canonical file")
    if args.file.startswith("baseline"):
        # check-inventory.py reads a baseline* file as completeness rows and skips its records.
        ap.error(f"--file {args.file!r}: names starting with baseline are completeness baselines, not record files")
    # A terminated capture cleans up as an interrupted one does (stop_vm, the
    # publication rollback), instead of dying with the VM running.
    signal.signal(signal.SIGTERM, signal.default_int_handler)
    try:
        with held(store=False):
            return capture_and_publish(args)
    except BlockingIOError as exc:
        print(f"capture: {exc}; run one at a time")
        return 4


def capture_and_publish(args):
    with store_held():
        if recover_pending():
            print("capture: rolled back a publication that was interrupted; the previous one is restored")
    out, rc = run_guest(args.surface, args.profile, args.timeout, args.fixture, args.prepare, args.standin,
                        module=args.module)
    if rc == 2:
        return 2
    try:
        meta, new = received(out, rc)
    except core.Incomplete as exc:
        print(f"capture: {exc}; nothing published, nothing removed")
        return 3
    # From here to the last swap the store is held: a publication another
    # checkout was killed in is rolled back before this one reads its inputs.
    with store_held():
        if recover_pending():
            print("capture: rolled back a publication that was interrupted; the previous one is restored")
        return check_and_publish(args, out, meta, new)


def check_and_publish(args, out, meta, new):
    if args.observe:
        # An observation file holds only what this capture measured, so it is
        # rewritten whole; the canonical file and the other profiles' files
        # are not touched.
        rel = pathlib.PurePosixPath(core.slug(args.surface)) / f"{args.file}.{args.profile}.json"
        canonical = INVENTORY / rel.parent / f"{args.file}.json"
        if not canonical.is_file():
            print(f"capture: {canonical} does not exist; capture the canonical profile first")
            return 2
        try:
            records = core.observations(new, args.profile)
        except ValueError as exc:
            print(f"capture: {exc}")
            return 1
    else:
        rel = pathlib.PurePosixPath(core.slug(args.surface)) / f"{args.file}.json"
        try:
            records = build_file(INVENTORY / rel, new, meta, args.merge)
        except SystemExit as exc:
            return exc.code
    code, report = check_scratch({rel: records})
    print(report.strip())
    if code != 0:
        print("capture: the result fails check-inventory.py; nothing published")
        return 1
    if args.dry_run:
        print(f"capture: dry run, {len(records)} records checked, nothing published (raw output in {out})")
        return 0
    try:
        publish(INVENTORY / rel, records, args.surface, out, meta.get("states", []), SHOTS_MANIFEST, STORE,
                profile=args.profile, file=args.file)
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
