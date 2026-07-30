#!/usr/bin/env python3
"""Exec a command with `PR_SET_PDEATHSIG`, so the child dies with its parent.

`scripts/test.sh` reaps its backgrounded QEMU from a cleanup trap. A trap cannot
run when the wrapper is `SIGKILL`ed, so the VM survives its parent and keeps
holding the shared boot state the tree owns. `prctl(PR_SET_PDEATHSIG)` moves the
guarantee into the KERNEL: it signals the child when its parent dies, whatever
killed the parent and whether or not any handler got to run.

Used as: `python3 pdeathsig.py --parent <expected-ppid> -- <argv...>`. The
process execs the real command, so the caller's `$!` remains the command's pid
and an existing kill/wait path keeps working unchanged.

There is a race the obvious implementation loses. `PR_SET_PDEATHSIG` can only be
set AFTER the fork, so if the parent dies in the window between the fork and the
prctl, no signal is ever armed and the exec produces exactly the orphan this
helper exists to prevent. Merely re-reading `getppid()` after the prctl does not
close it either: if the parent died before this process's FIRST `getppid()`, both
reads return the same (already re-parented) value and look consistent. The
expected ppid must therefore come from the PARENT, which knows its own pid, and
is checked both before and after the prctl.

`--check` verifies the mechanism is usable at all -- prctl is callable, and, with
`--exe`, that the target is not set-id and carries no file capabilities, both of
which make the kernel CLEAR PDEATHSIG across the exec.

Exit codes:
      3 = the parent is already gone (refuses to exec: doing so is the orphan)
      4 = --check failed (mechanism unavailable; caller should degrade + warn)
      5 = exec failed
    127 = usage error
"""

import argparse
import ctypes
import ctypes.util
import os
import shutil
import signal
import stat
import sys

PR_SET_PDEATHSIG = 1


def _libc():
    try:
        name = ctypes.util.find_library("c") or "libc.so.6"
        return ctypes.CDLL(name, use_errno=True)
    except OSError:
        return None


def set_pdeathsig(sig):
    """Arm the parent-death signal. Returns an error string, or "" on success."""
    libc = _libc()
    if libc is None or not hasattr(libc, "prctl"):
        return "libc prctl is unavailable"
    if libc.prctl(PR_SET_PDEATHSIG, ctypes.c_ulong(sig), 0, 0, 0) != 0:
        return f"prctl(PR_SET_PDEATHSIG) failed: errno {ctypes.get_errno()}"
    return ""


def exec_hazard(exe):
    """Why PDEATHSIG would not survive an exec of `exe`, or "" if it would.

    Linux clears the parent-death signal when a process's credentials change
    across an exec, which a set-id bit or a file capability both do. Reporting
    that up front is the difference between an honest degrade and a guarantee
    that silently is not one.
    """
    path = shutil.which(exe) if not os.path.isabs(exe) else exe
    if not path:
        return f"{exe} not found on PATH"
    try:
        mode = os.stat(path).st_mode
    except OSError as exc:
        return f"cannot stat {path}: {exc}"
    if mode & (stat.S_ISUID | stat.S_ISGID):
        return f"{path} is set-id, which clears PDEATHSIG on exec"
    try:
        os.getxattr(path, "security.capability")
    except OSError:
        pass  # the common case: no file capabilities
    except AttributeError:
        pass  # no getxattr on this platform; nothing to report
    else:
        return f"{path} carries file capabilities, which clear PDEATHSIG on exec"
    return ""


def _self_starttime():
    """Field 22 of /proc/self/stat -- ticks since boot at this process's start.

    Read after the LAST ')' because field 2 is `comm` and a comm may contain
    both spaces and parens.
    """
    try:
        with open("/proc/self/stat", "r") as fh:
            data = fh.read()
    except OSError:
        return ""
    tail = data.rpartition(")")[2].split()
    return tail[19] if len(tail) > 19 else ""


def _boot_id():
    try:
        with open("/proc/sys/kernel/random/boot_id", "r") as fh:
            return fh.read().strip()
    except OSError:
        return ""


def _publish_provenance(path, run_id):
    """Write '<pid> <starttime> <boot_id> <run_id>' atomically. True on success.

    Atomic because the reader is a LATER RUN's recovery path: a torn line there
    means an unparseable identity, which reads as "no provenance" and makes a
    genuine orphan unreapable. Write-then-rename gives the reader either the old
    record or the whole new one.
    """
    start, bid = _self_starttime(), _boot_id()
    if not start:
        print("pdeathsig: cannot read own starttime -- refusing to publish an "
              "identity that cannot be re-verified", file=sys.stderr)
        return False
    tmp = f"{path}.{os.getpid()}.tmp"
    try:
        d = os.path.dirname(path)
        if d:
            os.makedirs(d, exist_ok=True)
        with open(tmp, "w") as fh:
            fh.write(f"{os.getpid()} {start} {bid} {run_id}\n")
            fh.flush()
            os.fsync(fh.fileno())
        os.rename(tmp, path)
        return True
    except OSError as exc:
        print(f"pdeathsig: cannot publish provenance to {path}: {exc}",
              file=sys.stderr)
        try:
            os.unlink(tmp)
        except OSError:
            pass
        return False


def main(argv):
    ap = argparse.ArgumentParser(prog="pdeathsig.py", add_help=True,
                                 description="exec a command under PR_SET_PDEATHSIG")
    ap.add_argument("--parent", type=int, default=0,
                    help="expected parent pid; refuse to exec if it is not ours")
    ap.add_argument("--signal", type=int, default=int(signal.SIGKILL),
                    help="signal delivered on parent death (default SIGKILL)")
    ap.add_argument("--check", action="store_true",
                    help="verify the mechanism is usable and exit")
    ap.add_argument("--exe", default="",
                    help="with --check, the command whose exec would clear it")
    ap.add_argument("--provenance", default="",
                    help="publish '<pid> <starttime> <boot_id> <run_id>' to "
                         "this path before exec, so a later run can prove this "
                         "tree launched the VM")
    ap.add_argument("--run-id", default="-",
                    help="run identity recorded alongside the pid")
    ap.add_argument("cmd", nargs="*", help="command to exec after the marker --")
    args = ap.parse_args(argv)

    if args.check:
        err = set_pdeathsig(0)  # 0 disarms: a harmless probe of callability
        if err:
            print(f"pdeathsig: {err}", file=sys.stderr)
            return 4
        if args.exe:
            err = exec_hazard(args.exe)
            if err:
                print(f"pdeathsig: {err}", file=sys.stderr)
                return 4
        return 0

    if not args.cmd:
        ap.print_usage(sys.stderr)
        return 127

    # Before: if the parent is already gone, exec'ing IS the orphan. Refuse.
    if args.parent and os.getppid() != args.parent:
        print(f"pdeathsig: parent {args.parent} already gone (ppid is "
              f"{os.getppid()}) -- not starting the child", file=sys.stderr)
        return 3

    err = set_pdeathsig(args.signal)
    if err:
        # A failed prctl must NOT cost the caller its whole run. Report and exec
        # anyway: that is exactly the pre-existing behaviour, and the caller's
        # --check probe is what is supposed to have warned about it already.
        print(f"pdeathsig: {err} -- continuing without parent-death reaping",
              file=sys.stderr)

    # After: closes the window between the check above and the prctl.
    if args.parent and os.getppid() != args.parent:
        print(f"pdeathsig: parent {args.parent} died while arming "
              "PR_SET_PDEATHSIG -- not starting the child", file=sys.stderr)
        return 3

    # Provenance is published from the CHILD, BEFORE the exec, because the
    # parent cannot publish it in time. The parent only learns the pid after
    # the fork, so its write happens with QEMU already running -- and a wrapper
    # `SIGKILL`ed in that window leaves a VM this tree really did launch but
    # can no longer prove it launched, which the recovery path must then refuse
    # to reap. Written here, the record exists before the process becomes QEMU.
    #
    # The identity is read from THIS process, not passed in: `os.getpid()` and
    # our own `starttime` are facts, and `execvp` preserves both, so they still
    # describe the VM afterwards.
    if args.provenance:
        if not _publish_provenance(args.provenance, args.run_id):
            # Refuse rather than exec: an unprovable VM is precisely what this
            # is here to prevent, and failing before the exec costs the caller
            # a clear error instead of an unattributable orphan.
            return 6

    try:
        os.execvp(args.cmd[0], args.cmd)
    except OSError as exc:
        print(f"pdeathsig: cannot exec {args.cmd[0]}: {exc}", file=sys.stderr)
        return 5
    return 5  # unreachable: execvp either replaces this process or raises


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
