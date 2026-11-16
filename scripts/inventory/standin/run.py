#!/usr/bin/env python3
"""Run one job on the stand-in machine: the reference disk under QEMU with laptop devices.

    standin/run.py --variant charging --job job.ps1 [--file NAME=PATH ...] --out DIR

The stand-in machine is the reference VM's clean disk (derive.ps1, converted
once to qcow2 beside it) booted under QEMU on a throwaway overlay, with the
devices Hyper-V cannot show: an AC adapter, a battery, a lid and a built-in
panel's backlight, from standin.asl. variants.json sets the battery state per
variant. (A virtio multitouch screen was tried for touch and pen: the
virtio-win 0.1.302 input driver fails to start it, code 10, 0xC000009A, so those
surfaces are absent rows.) The job runs in ref's desktop session through the logon
agent (agent.ps1) from a FAT volume labelled IOSJOB; its output folder is
copied to --out when the guest shuts down. Exit 0 when the job's exit code is
0; 1 when it failed; 2 on bad arguments; 3 when the guest never finished.
Owner: todo/08-graphics-ui/TODO-17-win11-parity-inventory.md section 21.
"""
from __future__ import annotations

import argparse
import json
import os
import pathlib
import shutil
import signal
import socket
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
# On the Linux disk: QEMU over the Windows drive share is slow. derive.ps1 writes
# R:\11\standin\standin.vhdx; `qemu-img convert -O qcow2` puts it here.
DISK = pathlib.Path.home() / ".local/share/ios-standin/standin.qcow2"
OVMF_CODE = pathlib.Path("/usr/share/OVMF/OVMF_CODE_4M.fd")
OVMF_VARS = pathlib.Path("/usr/share/OVMF/OVMF_VARS_4M.fd")
JOB_MB = 512
TOOL_TIMEOUT = 300  # deadline: iasl, mkfs.fat, mtools and qemu-img on local files


def fill(template: str, v: dict) -> str:
    for k, key in (("AC_ONLINE", "ac"), ("STATE", "state"), ("RATE", "rate"), ("REMAINING", "remaining")):
        template = template.replace(f"@{k}@", str(int(v[key])))
    return template


def _interrupt(signum, frame):
    raise KeyboardInterrupt


def tool(*cmd, **kw):
    return subprocess.run(cmd, check=True, capture_output=True, text=True, timeout=TOOL_TIMEOUT, **kw)


def screendump(sock_path, png):
    """Ask QEMU, over its QMP socket, for a PNG of the guest screen; False when it cannot."""
    try:
        with socket.socket(socket.AF_UNIX) as c:
            c.settimeout(20)  # deadline: one local QMP exchange
            c.connect(str(sock_path))
            f = c.makefile("rw")
            f.readline()
            for msg in ({"execute": "qmp_capabilities"},
                        {"execute": "screendump", "arguments": {"filename": str(png), "format": "png"}}):
                f.write(json.dumps(msg) + "\n")
                f.flush()
                while "return" not in (reply := json.loads(f.readline())) and "error" not in reply:
                    pass
            return "return" in reply
    except (OSError, ValueError):
        return False  # fail-direction: the dump only helps diagnose a timeout


def qemu_cmd(overlay, vars_fd, aml, job_img, variant, qmp):
    accel = "kvm" if os.access("/dev/kvm", os.W_OK) else "tcg"
    cmd = ["qemu-system-x86_64", "-machine", f"q35,accel={accel}", "-cpu", "host" if accel == "kvm" else "max",
           "-smp", "4", "-m", "4096", "-display", "none", "-no-reboot",
           "-drive", f"if=pflash,format=raw,unit=0,readonly=on,file={OVMF_CODE}",
           "-drive", f"if=pflash,format=raw,unit=1,file={vars_fd}",
           "-device", "ahci,id=ahci,addr=0x1", "-drive", f"if=none,id=sys,format=qcow2,file={overlay}",
           "-device", "ide-hd,drive=sys,bus=ahci.0",
           "-device", "qemu-xhci,id=xhci,addr=0x2",
           # Removable: Windows mounts a partitionless FAT volume only on removable media.
           "-drive", f"if=none,id=job,format=raw,file={job_img}", "-device", "usb-storage,drive=job,bus=xhci.0,removable=on",
           "-device", "usb-tablet,bus=xhci.0", "-device", "usb-kbd,bus=xhci.0",
           # Slot 3 is the S18 node standin.asl extends with backlight control.
           "-device", "VGA,xres=1920,yres=1080,edid=on,addr=0x3",
           "-nic", "none", "-acpitable", f"file={aml}", "-qmp", f"unix:{qmp},server=on,wait=off"]
    return cmd


# The job for --in/--command: the same layout ref-run gives a command on the
# reference VM (inputs in C:\ios\in, output in C:\ios\out, started in C:\ios),
# so capture.py runs unchanged on the stand-in machine.
COMMAND_JOB = r"""param([string]$Out)
$ErrorActionPreference = 'Stop'
$job = Split-Path -Parent $MyInvocation.MyCommand.Path
foreach ($d in 'C:\ios\in', 'C:\ios\out') {{
    if (Test-Path -LiteralPath $d) {{ Remove-Item -LiteralPath $d -Recurse -Force }}
    New-Item -ItemType Directory -Path $d | Out-Null
}}
if (Test-Path -LiteralPath "$job\in") {{ Copy-Item -Path "$job\in\*" -Destination 'C:\ios\in' -Recurse }}
Set-Location -LiteralPath 'C:\ios'
& cmd.exe /c "{command} > C:\ios\out\stdout.txt 2>&1"
$rc = $LASTEXITCODE
Copy-Item -Path 'C:\ios\out\*' -Destination $Out -Recurse
exit $rc
"""


def run(variant_name, job, files, out, *, disk=DISK, timeout=1200, variants=None):
    """Boot the stand-in machine for one job. `files` is [(path under ios-job, source)].
    The job's output folder lands in `out`. Returns 0, 1 (job failed) or 3 (no result)."""
    variants = variants or json.loads((HERE / "variants.json").read_text(encoding="utf-8"))
    variant = variants[variant_name]
    out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="standin-") as tmp:
        t = pathlib.Path(tmp)
        (t / "standin.asl").write_text(fill((HERE / "standin.asl").read_text(encoding="utf-8"), variant))
        tool("iasl", "-p", str(t / "standin"), str(t / "standin.asl"))
        job_img = t / "job.img"
        with open(job_img, "wb") as fh:
            fh.truncate(JOB_MB * 1024 * 1024)
        tool("mkfs.fat", "-F", "32", "-n", "IOSJOB", str(job_img))
        tool("mmd", "-i", str(job_img), "::/ios-job")
        tool("mcopy", "-i", str(job_img), str(job), "::/ios-job/job.ps1")
        made = set()
        for rel, src in files:
            parts = pathlib.PurePosixPath(rel).parts
            for k in range(1, len(parts)):
                d = "/".join(parts[:k])
                if d not in made:
                    tool("mmd", "-i", str(job_img), f"::/ios-job/{d}")
                    made.add(d)
            tool("mcopy", "-s", "-i", str(job_img), str(src), f"::/ios-job/{rel}")
        vars_fd = t / "vars.fd"
        shutil.copyfile(OVMF_VARS, vars_fd)
        # The overlay sits beside the disk, so the base is opened by a relative name.
        overlay = disk.with_name(f"run-{os.getpid()}.qcow2")
        tool("qemu-img", "create", "-q", "-f", "qcow2", "-b", disk.name, "-F", "qcow2", str(overlay))
        qmp = t / "qmp.sock"
        try:
            # A new session: on timeout the whole QEMU process group is stopped.
            p = subprocess.Popen(qemu_cmd(overlay, vars_fd, t / "standin.aml", job_img, variant, qmp),
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, start_new_session=True)
            # A stop of run.py itself (SIGTERM) also ends the guest: never orphan QEMU.
            previous = signal.signal(signal.SIGTERM, _interrupt)
            try:
                log, _ = p.communicate(timeout=timeout)
            except KeyboardInterrupt:
                os.killpg(p.pid, signal.SIGTERM)
                p.communicate(timeout=60)
                print("standin: stopped")
                return 3
            except subprocess.TimeoutExpired:
                screendump(qmp, out / "timeout.png")
                os.killpg(p.pid, signal.SIGTERM)
                log, _ = p.communicate(timeout=60)
                (out / "qemu.log").write_text(log or "")
                print(f"standin: the guest did not finish in {timeout}s (its screen: {out / 'timeout.png'})")
                return 3
            finally:
                signal.signal(signal.SIGTERM, previous)
            (out / "qemu.log").write_text(log or "")
        finally:
            overlay.unlink(missing_ok=True)
        done = subprocess.run(["mtype", "-i", str(job_img), "::/ios-job/done.txt"],
                              capture_output=True, text=True, timeout=TOOL_TIMEOUT)
        if done.returncode != 0:
            print("standin: the job left no done.txt on its volume")
            return 3
        got = subprocess.run(["mcopy", "-s", "-n", "-i", str(job_img), "::/ios-job/out/*", str(out)],
                             capture_output=True, text=True, timeout=TOOL_TIMEOUT)
        rc = done.stdout.strip()
        print(f"standin: {variant_name}: job exited {rc}; output in {out}")
        if got.returncode != 0:
            print(f"standin: copying the output failed: {got.stderr.strip()}")
            return 3
        return 0 if rc == "0" else 1


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--variant", required=True)
    ap.add_argument("--job", type=pathlib.Path, help="a job script taking -Out")
    ap.add_argument("--in", dest="in_dir", type=pathlib.Path, help="with --command: copied to C:\\ios\\in")
    ap.add_argument("--command", help="instead of --job: a command line run in C:\\ios, as ref-run runs it")
    ap.add_argument("--file", action="append", default=[], metavar="NAME=PATH")
    ap.add_argument("--out", required=True, type=pathlib.Path)
    ap.add_argument("--disk", type=pathlib.Path, default=DISK)
    ap.add_argument("--timeout", type=int, default=1200)
    args = ap.parse_args(argv)
    if bool(args.job) == bool(args.command):
        ap.error("give --job or --command")
    variants = json.loads((HERE / "variants.json").read_text(encoding="utf-8"))
    if args.variant not in variants:
        ap.error(f"unknown variant {args.variant}; variants.json has {', '.join(variants)}")
    if args.out.exists() and any(args.out.iterdir()):
        ap.error(f"{args.out} is not empty; give a new folder for each run")
    for need in ("iasl", "mkfs.fat", "mcopy", "qemu-img", "qemu-system-x86_64"):
        if not shutil.which(need):
            ap.error(f"{need} is not on PATH (iasl: acpica-tools; mcopy: mtools; mkfs.fat: dosfstools)")
    if not args.disk.is_file():
        ap.error(f"no stand-in disk at {args.disk}; derive it with standin/derive.ps1 and convert it with qemu-img")
    files = []
    for f in args.file:
        name, _, path = f.partition("=")
        if not name or not pathlib.Path(path).is_file():
            ap.error(f"--file {f}: give NAME=PATH of a file")
        files.append((name, pathlib.Path(path)))
    with tempfile.TemporaryDirectory(prefix="standin-job-") as tmp:
        job = args.job
        if args.command:
            job = pathlib.Path(tmp) / "job.ps1"
            job.write_text(COMMAND_JOB.format(command=args.command.replace('"', '""')), encoding="utf-8")
            if args.in_dir:
                files += [(f"in/{p.relative_to(args.in_dir).as_posix()}", p)
                          for p in sorted(args.in_dir.rglob("*")) if p.is_file()]
        return run(args.variant, job, files, args.out, disk=args.disk, timeout=args.timeout, variants=variants)


if __name__ == "__main__":
    sys.exit(main())
