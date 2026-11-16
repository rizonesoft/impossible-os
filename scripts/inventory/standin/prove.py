#!/usr/bin/env python3
"""Prove each stand-in on the stand-in machine and record its proof.

    standin/prove.py [ID ...]

docs/design/inventory/stand-ins.json names, per stand-in, the variant that
shows it, the page that shows it and what the guest must report (`expect`):
`power` (AC line, battery percent and Energy saver from GetSystemPowerStatus),
`device` (a present, working PnP device whose instance id matches) or
`brightness` (a monitor that reports brightness through WMI). For each variant
one job on the stand-in machine (run.py) checks every expectation and takes a
screenshot of each page. A stand-in whose expectation holds gets its screenshot
moved to the store (R:\\11\\ref-shots\\stand-ins\\<id>.png) and a `proof`
(file, SHA-256, size, date) in stand-ins.json; one that fails is named, keeps
no proof, and the command exits 1. The `stand-in` rows of expected.json name
these ids; inventory-report.py --campaign --check refuses one without a proof.
Owner: todo/08-graphics-ui/TODO-17-win11-parity-inventory.md section 21.
"""
from __future__ import annotations

import argparse
import datetime
import hashlib
import importlib.util
import json
import pathlib
import shutil
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parents[2]
STAND_INS = REPO / "docs" / "design" / "inventory" / "stand-ins.json"
STORE = pathlib.Path("/mnt/r/11/ref-shots/stand-ins")

JOB = r"""param([string]$Out)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing, System.Windows.Forms
Add-Type -TypeDefinition @'
using System.Runtime.InteropServices;
public static class Power {
    [StructLayout(LayoutKind.Sequential)] public struct Status {
        public byte AC; public byte Flag; public byte Percent; public byte SaverFlag; public int Life; public int Full; }
    [DllImport("kernel32.dll")] public static extern bool GetSystemPowerStatus(out Status s);
}
'@
function Shot($name) {
    $s = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    $b = New-Object System.Drawing.Bitmap $s.Width, $s.Height
    $g = [System.Drawing.Graphics]::FromImage($b)
    $g.CopyFromScreen(0, 0, 0, 0, $b.Size); $b.Save("$Out\$name.png"); $g.Dispose(); $b.Dispose()
}
# The desktop and its first-boot device installs settle before anything is read.
Start-Sleep 60
$st = New-Object Power+Status
[void][Power]::GetSystemPowerStatus([ref]$st)
$facts = [ordered]@{
    power = [ordered]@{ ac = [int]$st.AC; percent = [int]$st.Percent; saver = [int]$st.SaverFlag }
    devices = @(Get-PnpDevice -PresentOnly | Where-Object { $_.Status -eq 'OK' } | ForEach-Object { $_.InstanceId })
    brightness = [bool]@(Get-CimInstance -Namespace root\wmi -ClassName WmiMonitorBrightness -ErrorAction SilentlyContinue).Count
}
$facts | ConvertTo-Json -Depth 4 | Set-Content -Encoding UTF8 "$Out\facts.json"
$pages = @()
PAGES
foreach ($pair in $pages) {
    Start-Process $pair[1]; Start-Sleep 15; Shot $pair[0]
    Get-Process SystemSettings -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep 3
}
exit 0
"""


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def job_for(ids, entries):
    """The proof job for one variant: its facts, then a screenshot of each id's page.
    Ids and pages are fixed text from stand-ins.json; the unary comma keeps each
    pair one element of $pages."""
    lines = "\n".join(f"$pages += ,@('{i}', '{entries[i]['page']}')" for i in ids)
    return JOB.replace("PAGES", lines)


def unmet(expect, facts):
    """The expectations `facts` (the guest's facts.json) does not meet, as text."""
    out = []
    for key, want in expect.items():
        if key == "power":
            got = facts.get("power", {})
            out += [f"{k} is {got.get(k)}, want {v}" for k, v in want.items() if got.get(k) != v]
        elif key == "device":
            pattern = want.replace("\\", "/")
            if not any(pathlib.PurePosixPath(d.replace("\\", "/")).match(pattern) for d in facts.get("devices", [])):
                out.append(f"no working device {want}")
        elif key == "brightness":
            if bool(facts.get("brightness")) != want:
                out.append(f"brightness reported is {facts.get('brightness')}, want {want}")
        else:
            out.append(f"unknown expectation {key!r}")
    return out


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("ids", nargs="*", help="stand-in ids (default: all)")
    ap.add_argument("--timeout", type=int, default=1200)
    args = ap.parse_args(argv)
    entries = json.loads(STAND_INS.read_text(encoding="utf-8"))
    ids = args.ids or list(entries)
    for i in ids:
        if i not in entries:
            ap.error(f"{i} is not in {STAND_INS.relative_to(REPO)}")
    run = _load("standin_run", HERE / "run.py")
    by_variant = {}
    for i in ids:
        by_variant.setdefault(entries[i]["variant"], []).append(i)
    failed = []
    for variant, group in by_variant.items():
        with tempfile.TemporaryDirectory(prefix="standin-prove-") as tmp:
            t = pathlib.Path(tmp)
            (t / "job.ps1").write_text(job_for(group, entries), encoding="utf-8")
            rc = run.run(variant, t / "job.ps1", [], t / "out", timeout=args.timeout)
            facts_file = t / "out" / "facts.json"
            facts = json.loads(facts_file.read_text(encoding="utf-8-sig")) if facts_file.is_file() else None
            for i in group:
                shot = t / "out" / f"{i}.png"
                why = (["the stand-in job failed"] if rc != 0 or facts is None else unmet(entries[i]["expect"], facts))
                if not why and not shot.is_file():
                    why = ["no screenshot"]
                if why:
                    failed.append(i)
                    entries[i].pop("proof", None)
                    print(f"prove: {i}: not proven ({'; '.join(why)})", flush=True)
                    continue
                STORE.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(shot, STORE / f"{i}.png")
                data = (STORE / f"{i}.png").read_bytes()
                entries[i]["proof"] = {"file": f"stand-ins/{i}.png", "sha256": hashlib.sha256(data).hexdigest(),
                                       "size": len(data), "date": datetime.date.today().isoformat()}
                print(f"prove: {i}: proven ({variant})", flush=True)
    STAND_INS.write_text(json.dumps(entries, indent=2) + "\n", encoding="utf-8")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
