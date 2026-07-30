#!/usr/bin/env python3
"""Detect -- and, only on explicit request, reap -- a QEMU process orphaned by a
killed `scripts/test.sh` wrapper.

`scripts/test.sh` launches QEMU in the background and reaps it from its cleanup
trap. `SIGKILL` bypasses that trap, so a killed wrapper can leave a live VM
still holding the shared boot state the run owns: the `build/OVMF_VARS_4M.fd`
pflash copy, the `build/test.log` serial sink, and -- most destructively --
`build/system-disk.img`, which the NEXT run's build rewrites underneath it.
The run lock cannot see that orphan: QEMU is launched with the lock descriptor
closed, so it holds no lock to contend for.

Ownership is decided by an EXACT open-descriptor match, never by process name.
`QEMU_BIN` is caller-supplied and need not contain "qemu", so a name-gated scan
would both miss a renamed or wrapped binary and happily signal an unrelated
process that merely looks like ours. What makes a process THIS tree's orphan is
that it holds one of THIS tree's files open, which is exactly what
`/proc/<pid>/fd` answers.

The scan reads `/proc` directly rather than shelling out to `lsof` or `fuser`,
neither of which is guaranteed present on a supported host.

Exit codes:
    detect:    0 = no holder, 3 = holder(s) found, 2 = usage/environment error
    reap:      0 = the pid is gone, 4 = identity re-check failed (refused to
               signal), 5 = still alive after SIGKILL, 2 = usage error
    identity:  0 = printed "<pid> <starttime> <boot_id>", 1 = pid is gone
"""

import argparse
import errno
import json
import os
import select
import signal
import sys
import time

PROC = "/proc"
_DELETED = " (deleted)"


def boot_id():
    """Identity of the running kernel boot.

    A `starttime` is only meaningful WITHIN one boot: `build/` survives a
    reboot, so a pid recorded before one would otherwise be compared against an
    unrelated process on the assumption that ticks-since-boot are comparable
    across boots. They are not.
    """
    try:
        with open("/proc/sys/kernel/random/boot_id", "r") as fh:
            return fh.read().strip()
    except OSError:
        return ""


def starttime(pid):
    """Field 22 of /proc/<pid>/stat -- clock ticks since boot at process start.

    Parsed after the LAST ')' because field 2 is `comm`, and a comm may itself
    contain spaces and parentheses; splitting the whole line on whitespace is
    the classic way to read the wrong field for a process named `(a b)`.
    """
    try:
        with open(f"{PROC}/{pid}/stat", "r") as fh:
            raw = fh.read()
    except OSError:
        return None
    close = raw.rfind(")")
    if close < 0:
        return None
    fields = raw[close + 1:].split()
    # After comm: state is fields[0] (field 3), so starttime (field 22) is 19.
    if len(fields) < 20:
        return None
    return fields[19]


def comm(pid):
    try:
        with open(f"{PROC}/{pid}/comm", "r") as fh:
            return fh.read().strip()
    except OSError:
        return "?"


def _real(path):
    """Best-effort absolute resolution.

    `realpath` normalises a path that does not exist yet, which matters because
    the OVMF copy and the serial log are both recreated per run: they may
    legitimately be absent at detection time while an orphan still holds the
    now-deleted inode they used to name.
    """
    try:
        return os.path.realpath(path)
    except OSError:
        return os.path.abspath(path)


def _link_target(link):
    """Resolve one /proc/<pid>/fd entry to the path it names.

    A descriptor whose file has been unlinked reads back as `<path> (deleted)`.
    That is not an edge case here, it is the MAIN case: `test.sh` runs
    `rm -f build/test.log` before each launch, so an orphan from the previous
    run holds precisely a deleted `test.log`. Stripping the suffix is what makes
    that orphan visible at all.
    """
    try:
        target = os.readlink(link)
    except OSError:
        return None
    if target.endswith(_DELETED):
        target = target[: -len(_DELETED)]
    return target


def holders(paths, exclude_pids, opaque=None):
    """Every process holding any of `paths` open, as a list of records.

    Silently skips processes that vanish mid-scan (a pid directory is a race by
    construction) and processes whose fd directory cannot be read.

    That second skip is a FAIL-OPEN, and it is deliberately narrow rather than
    free: a live process whose `/proc/<pid>/fd` is unreadable might be holding
    one of these paths and the scan cannot tell. Same-uid is not enough to make
    it readable either -- a non-dumpable process (set-id exec, a dropped
    `dumpable` flag) hides its descriptors from its own user, and this is
    exactly the degraded case `pdeathsig.py --check` warns about. Refusing every
    such process would refuse most hosts, since any unrelated non-dumpable
    process would look like a possible holder. So the scan RECORDS them in
    `opaque` and lets the caller decide which ones it actually cares about --
    which, in practice, is the one pid this tree recorded launching.
    """
    wanted = {_real(p) for p in paths if p}
    if not wanted:
        return []
    skip = set(exclude_pids) | {os.getpid()}
    found = []
    try:
        entries = os.listdir(PROC)
    except OSError as exc:
        raise RuntimeError(f"cannot read {PROC}: {exc}") from exc
    for name in entries:
        if not name.isdigit():
            continue
        pid = int(name)
        if pid in skip:
            continue
        fddir = f"{PROC}/{name}/fd"
        try:
            fds = os.listdir(fddir)
        except OSError as exc:
            if exc.errno in (errno.EACCES, errno.EPERM):
                # Unreadable, NOT absent. Record it as indeterminate.
                if opaque is not None and alive(pid):
                    opaque.add(pid)
                continue
            if exc.errno in (errno.ENOENT, errno.ESRCH):
                continue
            raise
        for fd in fds:
            target = _link_target(f"{fddir}/{fd}")
            if target is None:
                continue
            if _real(target) in wanted:
                found.append({
                    "pid": pid,
                    "comm": comm(pid),
                    "starttime": starttime(pid) or "",
                    "boot_id": boot_id(),
                    "path": _real(target),
                })
                break  # one held path is enough to own the finding
    found.sort(key=lambda rec: rec["pid"])
    return found


def alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        # Alive, but not ours to signal. Report alive so the caller refuses
        # rather than treating "cannot touch it" as "it is gone".
        return True


def cmd_detect(args):
    opaque = set()
    try:
        found = holders(args.path, args.exclude_pid, opaque)
    except RuntimeError as exc:
        print(f"qemu-orphan: {exc}", file=sys.stderr)
        return 2
    # INDETERMINATE beats a clean bill of health. If the pid this tree recorded
    # launching is alive but hides its descriptors, the scan has no evidence
    # either way about the one process it most needs to answer for -- and
    # answering 0 there is the fail-open the whole guard exists to avoid. Exit 2
    # (detector error), which the caller already treats as fail-closed.
    if args.recorded_pid and args.recorded_pid in opaque:
        print(f"qemu-orphan: recorded pid {args.recorded_pid} is alive but its "
              "descriptors are unreadable (non-dumpable process) -- cannot "
              "prove it does not hold this tree's files", file=sys.stderr)
        return 2
    if args.json:
        print(json.dumps({"schema": "qemu-orphan-v1", "holders": found}))
    else:
        for rec in found:
            print("{pid}\t{starttime}\t{boot_id}\t{comm}\t{path}".format(**rec))
    return 3 if found else 0


def _pin(pid):
    """Pin `pid`'s IDENTITY for the rest of this call, or None if unavailable.

    A pid is a reused name, not an identity, and every check below it -- the
    `starttime` compare, the descriptor scan -- is a check on the NAME. The
    window between the last such check and the signal is real work (the scan
    walks all of `/proc`), so "exited, then the number was reused" is not a
    theoretical interleaving: it is the one this whole function exists to
    refuse, and re-reading `starttime` after the scan would only narrow it.

    A pidfd closes it instead of narrowing it. The descriptor refers to the
    PROCESS, not the number, so `pidfd_send_signal` on it either reaches the
    process it was opened for or fails with ESRCH -- it can never be redirected
    to a successor that inherited the pid.

    Returns None (never raises) when the host cannot provide one: a pre-5.3
    kernel, a pre-3.9 Python, or a process that exited between `alive()` and
    here. Callers fall back to the re-check-then-signal path, which is what
    shipped before and is still strictly better than not checking.
    """
    if os.environ.get("QEMU_ORPHAN_NO_PIDFD") == "1":
        # Test hook: the degraded path is the one that runs on an old host, so
        # it needs to be reachable on a new one. Nothing else reads this.
        return None
    opener = getattr(os, "pidfd_open", None)
    sender = getattr(signal, "pidfd_send_signal", None)
    if opener is None or sender is None:
        return None
    try:
        return opener(pid, 0)
    except (OSError, ValueError):
        return None


def _signal_pinned(pid, fd, sig):
    """Send `sig` to the pinned process. Returns "gone" / "sent" / "denied".

    `fd` may be None (no pidfd on this host), in which case this degrades to
    `os.kill` on the raw pid -- the pre-pidfd behaviour.
    """
    try:
        if fd is None:
            os.kill(pid, sig)
        else:
            signal.pidfd_send_signal(fd, sig)
    except ProcessLookupError:
        return "gone"
    except PermissionError:
        return "denied"
    except OSError as exc:
        # ESRCH through the pidfd path arrives as a bare OSError on some libcs.
        if exc.errno == errno.ESRCH:
            return "gone"
        raise
    return "sent"


def cmd_reap(args):
    """Kill a holder, but ONLY after re-proving it is still the same process and
    still holds one of this tree's files.

    Three proofs, in the order that makes each one load-bearing:

      1. A pidfd pins the process identity BEFORE anything is validated, so the
         signal at the end cannot land on a successor that reused the pid (see
         `_pin`). Everything below validates the process this descriptor names.
      2. `starttime` (and `boot_id`) still match what was recorded.
      3. The pid STILL holds one of this tree's files open.

    Failing 2 or 3 is exit 4: refuse and report, never signal on a guess.
    """
    if not alive(args.pid):
        return 0
    # Pinned FIRST, so the identity every later check validates is the identity
    # that gets signalled. Opening it after the checks would leave the same gap
    # in a more expensive place.
    pidfd = _pin(args.pid)
    try:
        if args.expect_boot_id and args.expect_boot_id != boot_id():
            print("qemu-orphan: recorded boot_id is from a previous boot -- "
                  "refusing to signal", file=sys.stderr)
            return 4
        if args.expect_starttime:
            actual = starttime(args.pid)
            if actual != args.expect_starttime:
                print(f"qemu-orphan: pid {args.pid} starttime {actual} != "
                      f"recorded {args.expect_starttime} (pid reused) -- "
                      "refusing to signal", file=sys.stderr)
                return 4
        try:
            still = holders(args.path, [])
        except RuntimeError as exc:
            print(f"qemu-orphan: {exc}", file=sys.stderr)
            return 2
        if args.pid not in {rec["pid"] for rec in still}:
            print(f"qemu-orphan: pid {args.pid} no longer holds any of this "
                  "tree's files -- refusing to signal", file=sys.stderr)
            return 4
        # Without a pidfd the pid is still just a name here, so re-read
        # `starttime` immediately before the signal. It does not close the
        # window -- nothing but a pidfd does -- but it shrinks it from "the
        # whole /proc scan" to "two syscalls".
        if pidfd is None and args.expect_starttime:
            if starttime(args.pid) != args.expect_starttime:
                print(f"qemu-orphan: pid {args.pid} was reused during the scan "
                      "-- refusing to signal", file=sys.stderr)
                return 4
        outcome = _signal_pinned(args.pid, pidfd, signal.SIGKILL)
        if outcome == "gone":
            return 0
        if outcome == "denied":
            print(f"qemu-orphan: pid {args.pid} is not ours to signal",
                  file=sys.stderr)
            return 5
        # SIGKILL is not synchronous: the pid stays visible until the kernel
        # reaps it, and it is NOT our child, so `wait` is unavailable.
        #
        # With a pidfd, wait for READABILITY rather than probing with signal 0.
        # The two answer different questions: a pidfd becomes readable when the
        # process TERMINATES, while signal 0 keeps succeeding for a zombie --
        # one that has released every descriptor it held and is no longer a
        # hazard to anything, but has not yet been reaped by its new parent.
        # Under a PID 1 that reaps lazily (some container inits), probing with
        # signal 0 therefore burns the whole timeout and reports exit 5 for a
        # VM that is already dead, which pushes the next run into a refusal or
        # towards UTEST_ORPHAN_UNCHECKED=1.
        deadline = time.monotonic() + max(0.1, args.timeout)
        if pidfd is not None:
            remaining = deadline - time.monotonic()
            while remaining > 0:
                try:
                    ready, _, _ = select.select([pidfd], [], [], remaining)
                except (OSError, ValueError):
                    break   # fall through to the signal-0 poll below
                if ready:
                    return 0
                remaining = deadline - time.monotonic()
            else:
                return 5
        # No pidfd (older host), or select was unusable. Signal 0 is all that
        # is left; it cannot distinguish a zombie, which is why it is second.
        while True:
            if _signal_pinned(args.pid, pidfd, 0) == "gone":
                return 0
            if time.monotonic() >= deadline:
                return 5
            time.sleep(0.05)
    finally:
        if pidfd is not None:
            try:
                os.close(pidfd)
            except OSError:
                pass


def cmd_identity(args):
    start = starttime(args.pid)
    if start is None:
        return 1
    print(f"{args.pid} {start} {boot_id()}")
    return 0


def main(argv):
    ap = argparse.ArgumentParser(prog="qemu-orphan.py",
                                 description="detect or reap an orphaned QEMU")
    sub = ap.add_subparsers(dest="cmd", required=True)

    det = sub.add_parser("detect", help="list processes holding the named paths")
    det.add_argument("--path", action="append", default=[], required=True)
    det.add_argument("--exclude-pid", action="append", type=int, default=[])
    det.add_argument("--recorded-pid", type=int, default=0,
                     help="pid this tree recorded launching; if it is alive but "
                          "hides its descriptors, detection is INDETERMINATE "
                          "(exit 2) rather than clean")
    det.add_argument("--json", action="store_true")
    det.set_defaults(fn=cmd_detect)

    rap = sub.add_parser("reap", help="identity-checked SIGKILL of one holder")
    rap.add_argument("--pid", type=int, required=True)
    rap.add_argument("--path", action="append", default=[], required=True)
    rap.add_argument("--expect-starttime", default="")
    rap.add_argument("--expect-boot-id", default="")
    rap.add_argument("--timeout", type=float, default=5.0)
    rap.set_defaults(fn=cmd_reap)

    ident = sub.add_parser("identity", help="print '<pid> <starttime> <boot_id>'")
    ident.add_argument("--pid", type=int, required=True)
    ident.set_defaults(fn=cmd_identity)

    args = ap.parse_args(argv)
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
