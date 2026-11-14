#!/usr/bin/env python3
"""Read the completeness baselines and publish them into the inventory.

Owner: todo/08-graphics-ui/TODO-17-win11-parity-inventory.md section 5.

    python3 scripts/inventory/baselines.py [--dry-run]

1. Runs guest/read-baselines.py in the reference VM on a fresh restore of clean
   (through capture.py's ref-run, under the same capture lock: one VM job at a
   time, exit 4 when held). It reads six baselines: settings, control-panel,
   known-folders, start-apps, classic-controls, shell-commands.
2. Refuses a read that is incomplete, or from a build other than
   docs/design/inventory/reference.json names (exit 3): nothing is published.
3. Imports the seventh, controls, from the WinUI Gallery at the pinned commit
   (baseline_core.GALLERY); the file is fetched over HTTPS and only the control
   names and groups are kept.
4. Writes docs/design/inventory/<name>/baseline.json for all seven into a
   scratch copy of the inventory and runs check-inventory.py over it; only a
   clean result is published, and all seven are swapped in together through
   capture.py's journalled swap, so a failure or a kill leaves the previous set.

--dry-run does steps 1 to 4 and publishes nothing.
"""
import argparse
import importlib.util
import json
import os
import pathlib
import signal
import sys
import time
import urllib.request

HERE = pathlib.Path(__file__).resolve().parent


def _load(name):
    spec = importlib.util.spec_from_file_location(name, HERE / f"{name}.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


capture = _load("capture")
core = _load("baseline_core")

INVENTORY = capture.INVENTORY
FETCH_TIMEOUT = 60   # deadline: the whole download, one JSON file of about 200 KB from GitHub
FETCH_STALL = 15     # deadline: one wait on the socket
FETCH_CHUNK = 1 << 16
FETCH_MAX = 8 << 20  # the Gallery file is about 200 KB; a larger answer is not it
GUEST_TIMEOUT = 600


def header(name, build=None, source=None, files=None, upstream=None):
    h = {"baseline": name, "owner": core.OWNERS[name], "source": source}
    if build is not None:
        h["build"] = build
    if upstream is not None:
        h["upstream"] = upstream
    if files:
        h["files"] = files
    return h


def gallery_rows(fetch=None):
    """The controls baseline from the pinned Gallery commit."""
    g = core.GALLERY
    url = f"https://raw.githubusercontent.com/{g['repo']}/{g['commit']}/{g['path']}"
    if fetch is None:
        def fetch(u):
            with urllib.request.urlopen(u, timeout=FETCH_STALL) as r:  # deadline: FETCH_STALL a wait, FETCH_TIMEOUT in all
                return read_bounded(r, u)
    try:
        data = json.loads(fetch(url).decode("utf-8-sig"))
    except (OSError, ValueError) as exc:
        raise core.SourceError(f"controls: the Gallery list could not be read ({exc})") from None
    return core.control_rows(data)


def read_bounded(resp, what, clock=time.monotonic):
    """The whole response, or SourceError once it passes FETCH_MAX bytes or
    FETCH_TIMEOUT seconds in total. A socket timeout bounds one wait only, so a
    source that keeps trickling is cut off here; the caller's `with` closes it.
    This runs under the capture lock, which no stalled download may hold."""
    end = clock() + FETCH_TIMEOUT
    data = bytearray()
    while True:
        chunk = resp.read(FETCH_CHUNK)
        if not chunk:
            return bytes(data)
        data += chunk
        if len(data) > FETCH_MAX:
            raise core.SourceError(f"controls: {what} is larger than {FETCH_MAX} bytes")
        if clock() > end:
            raise core.SourceError(f"controls: {what} did not finish within {FETCH_TIMEOUT} s")


def received(out, rc, build):
    """The guest's baselines, or raise core.SourceError with the reason."""
    path = out / "baselines.json"
    if not path.is_file():
        raise core.SourceError(f"the guest wrote no baselines.json (ref-run exit {rc})")
    try:
        got = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise core.SourceError(f"baselines.json is unreadable ({exc})") from None
    if not got.get("complete"):
        raise core.SourceError(f"incomplete ({got.get('incomplete', 'no reason given')})")
    if rc != 0:
        raise core.SourceError(f"ref-run exited {rc} after a complete read; the transfer may be partial")
    if got.get("build") != build:
        raise core.SourceError(f"the VM is build {got.get('build')}, but reference.json names {build}")
    names = set(got.get("baselines", {}))
    if names != set(core.VM_BASELINES):
        raise core.SourceError(f"the guest read {sorted(names)}, expected {sorted(core.VM_BASELINES)}")
    return got["baselines"]


def assemble(vm, build, controls):
    """{path under the inventory: lines} for all seven baselines."""
    files = {}
    for name in core.VM_BASELINES:
        b = vm[name]
        files[f"{name}/baseline.json"] = [header(name, build=build, source=b["source"], files=b.get("files"))] + b["rows"]
    g = core.GALLERY
    files["controls/baseline.json"] = [header("controls", source=g["path"], upstream={
        "repo": g["repo"], "commit": g["commit"], "license": g["license"]})] + controls
    return files


def publish(files, journal):
    """Swap every baseline file in together (capture.swap)."""
    staged = []
    try:
        for rel, lines in files.items():
            final = INVENTORY / rel
            final.parent.mkdir(parents=True, exist_ok=True)
            tmp = final.with_name(f"{final.name}.{os.getpid()}.staged")
            capture.core.write_records(tmp, lines)
            staged.append((tmp, final))
    except BaseException:
        for tmp, _ in staged:
            tmp.unlink(missing_ok=True)
        raise
    capture.swap(staged, journal)


def run(args):
    if capture.recover(capture.JOURNAL):
        print("baselines: rolled back a publication that was interrupted; the previous one is restored")
    build = json.loads((INVENTORY / "reference.json").read_text(encoding="utf-8"))["build"]
    files = {"read-baselines.py": HERE / "guest" / "read-baselines.py", "baseline_core.py": HERE / "baseline_core.py"}
    out, rc = capture.ref_run("baselines", files, r"python C:\ios\in\read-baselines.py --out C:\ios\out\baselines.json",
                              GUEST_TIMEOUT, who="baselines")
    try:
        vm = received(out, rc, build)
        controls = gallery_rows()
    except core.SourceError as exc:
        print(f"baselines: {exc}; nothing published")
        return 3
    files = assemble(vm, build, controls)
    code, report = capture.check_scratch(files)
    print(report.strip())
    if code != 0:
        print("baselines: the result fails check-inventory.py; nothing published")
        return 1
    counts = ", ".join(f"{rel.split('/')[0]} {len(lines) - 1}" for rel, lines in files.items())
    if args.dry_run:
        print(f"baselines: dry run, checked {counts}; nothing published (raw output in {out})")
        return 0
    try:
        publish(files, capture.JOURNAL)
    except OSError as exc:
        print(f"baselines: publishing failed ({exc}); the previous baselines are unchanged")
        return 1
    print(f"baselines: published {counts}")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args(argv)
    signal.signal(signal.SIGTERM, signal.default_int_handler)
    try:
        with capture.exclusive(capture.LOCK):
            return run(args)
    except BlockingIOError:
        print(f"baselines: another VM job holds {capture.LOCK}; run one at a time")
        return 4


if __name__ == "__main__":
    sys.exit(main())
