#!/usr/bin/env python3
"""The reference VM takes turns with the SDK's machine.

ref-run.ps1 runs as it is, in its own Windows process, over a copy of
ref-common.ps1 whose queue has a name and times of the test's own and whose VM
cmdlets are stubs: no machine starts and no SDK job waits. Those cases need
Windows PowerShell and Bosun and skip without them; the source checks do not.
"""
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import uuid

INV = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(INV))
import capture  # noqa: E402

POWERSHELL = shutil.which("powershell.exe")
BOSUN = shutil.which("bosun.exe")
LIMIT = 120          # no case waits longer for a process than this
BYPASS = ["-NoProfile", "-ExecutionPolicy", "Bypass"]
# WSL's own report that Windows never answered the start of a process, seen on a
# host short of memory (2026-11-25): nothing ran, so it is started once more.
NOT_STARTED = "UtilAcceptVsock"
# The suite itself may run inside a Bosun slot; these cases start outside one.
ENV = {k: v for k, v in os.environ.items() if not k.startswith("BOSUN_")}
ENV["WSLENV"] = ":".join(x for x in ENV.get("WSLENV", "").split(":") if x and not x.startswith("BOSUN_"))

STUBS = r"""
$script:State = 'Off'
function Note([string]$Text) { Add-Content -LiteralPath '@LOG@' -Value $Text }
function Get-VM([string]$Name) { [pscustomobject]@{ Name = $Name; State = $script:State; Id = 'x' } }
function Get-VMSnapshot([string]$VMName) { [pscustomobject]@{ Name = 'clean' } }
function Get-VMDvdDrive([string]$VMName) { }
function Restore-VMSnapshot($VMSnapshot, [switch]$Confirm) { Note 'restore' }
function Start-VM([string]$Name) {
    Note "start pid=$PID slot=$env:BOSUN_SLOT ns=$env:BOSUN_NS wslenv=$env:WSLENV"
    if ($Name -like '*fail*') { throw 'the stub does not start' }
    $script:State = 'Running'
}
function Stop-VM([string]$Name, [switch]$TurnOff, [switch]$Force) { Note 'stop'; $script:State = 'Off' }
function Get-RefCredential([string]$VmPath) { }
function New-RefSession([string]$Name, [string]$VmPath) { 'session' }
function Test-RefIdentity($Session, [string]$Name) { if ($Session -ne 'session') { "the session is $Session" } }
function Invoke-Command { }
function Remove-PSSession { }
function Receive-RefTree { 0 }
function Invoke-RefInteractive($Session, [string]$Command, [int]$TimeoutSec) {
    if ($Command -eq 'hold') { for ($i = 0; $i -lt 600 -and -not (Test-Path -LiteralPath '@GO@'); $i++) { Start-Sleep -Milliseconds 100 } }
    if ($Command -eq 'nested') {
        $t = [Diagnostics.Stopwatch]::StartNew()
        bosun.exe run --ns @NS@ --slots 1 --max-wait 60 -- cmd.exe /d /c exit 9
        Note "nested exit=$LASTEXITCODE quick=$($t.Elapsed.TotalSeconds -lt 30)"
    }
    [pscustomobject]@{ ExitCode = 7; Output = '' }
}
"""

# A Bosun of the test's own: hangs for at most a minute (--max-wait 1), fails
# after writing 200,000 characters to stderr (2), or writes lines that name no
# place, as fast as it can, for at most half a minute (3).
FAKE_BOSUN = r"""
$src = 'using System; using System.IO; using System.Threading; public static class P { public static int Main(string[] a) { int t = Environment.TickCount;' +
    'File.WriteAllText(Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "pid.txt"), System.Diagnostics.Process.GetCurrentProcess().Id.ToString());' +
    'if (a[6] == "2") { Console.Error.Write(new string((char)101, 200000)); return 70; }' +
    'if (a[6] == "3") { for (int i = 0; Environment.TickCount - t < 30000; i++) Console.WriteLine("BOSUN_X=" + i); return 0; } Thread.Sleep(60000); File.WriteAllText(AppDomain.CurrentDomain.BaseDirectory + "lived.txt", ""); return 0; } }'
Add-Type -TypeDefinition $src -OutputAssembly '@EXE@' -OutputType ConsoleApplication
"""


def winpath(path):
    return subprocess.run(["wslpath", "-w", str(path)], capture_output=True, text=True, check=True).stdout.strip()


@unittest.skipUnless(POWERSHELL and BOSUN, "needs Windows PowerShell and Bosun")
class Turns(unittest.TestCase):
    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.ns = "ios-turns-" + uuid.uuid4().hex[:12]
        self.log, self.go = self.tmp / "log.txt", self.tmp / "go"
        (self.tmp / "vm").mkdir()
        self.win = winpath(self.tmp)

    def scripts(self, wait, limit):
        """Copies of ref-run.ps1 and ref-common.ps1 with this test's queue and stubs."""
        folder = self.tmp / f"scripts-{wait}-{limit}"
        folder.mkdir(exist_ok=True)
        shutil.copy(INV / "ref-run.ps1", folder)
        text = (INV / "ref-common.ps1").read_text(encoding="utf-8")
        for old, new in (("$script:RefTurnNs = 'Bosun.VM'", f"$script:RefTurnNs = '{self.ns}'"),
                         ("$script:RefTurnWaitSec = 360", f"$script:RefTurnWaitSec = {wait}"),
                         ("$script:RefTurnLimitSec = 420", f"$script:RefTurnLimitSec = {limit}")):
            self.assertEqual(text.count(old), 1, old)
            text = text.replace(old, new)
        stubs = STUBS.replace("@LOG@", self.win + "\\log.txt").replace("@GO@", self.win + "\\go").replace("@NS@", self.ns)
        (folder / "ref-common.ps1").write_text(text + stubs, encoding="utf-8")
        return folder

    def args(self, folder, *extra, name=None, out=True, command="work", vm=None):
        a = [winpath(folder) + "\\ref-run.ps1", "-VmPath", vm or self.win + "\\vm",
             "-Name", name or f"{self.ns}-vm", "-Command", command]
        return a + (["-Out", self.win + "\\out-" + uuid.uuid4().hex[:6]] if out else []) + list(extra)

    def ref_run(self, *a, **kw):
        return [POWERSHELL, *BYPASS, "-File", *self.args(*a, **kw)]

    def ref_run_with_path(self, path, *a, **kw):
        """ref-run in a process whose PATH is the PowerShell expression `path`."""
        first, *rest = self.args(*a, **kw)
        line = " ".join([f"& '{first}'"] + [x if x.startswith("-") else f"'{x}'" for x in rest])
        return [POWERSHELL, *BYPASS, "-Command", f"$env:PATH = {path}; {line}; exit $LASTEXITCODE"]

    def start(self, argv):
        p = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             text=True, cwd="/mnt/c", env=ENV)
        # Read by a thread, so no wait for a line or for the end lacks a deadline.
        p.lines, p.reader = [], threading.Thread(target=lambda: p.lines.extend(p.stdout), daemon=True)
        p.reader.start()
        self.addCleanup(self.end, p)
        return p

    def wait_for(self, ready, what):
        end = time.monotonic() + LIMIT
        while not ready():
            self.assertLess(time.monotonic(), end, what())
            time.sleep(0.1)

    def finish(self, p):
        """The exit code and everything printed."""
        code = p.wait(LIMIT)
        p.reader.join(LIMIT)
        return code, "".join(p.lines)

    def end(self, p):
        self.go.touch()          # a held run goes on
        p.stdin.close()          # a dummy job ends
        try:
            p.wait(LIMIT)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait()
        p.reader.join(5)
        if not p.reader.is_alive():
            p.stdout.close()

    def call(self, argv):
        for _ in range(2):
            r = subprocess.run(argv, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=LIMIT,
                               cwd="/mnt/c", env=ENV)
            if NOT_STARTED not in r.stdout + r.stderr:
                break
        return r.returncode, r.stdout + r.stderr

    def first_line(self, argv):
        """The process and the first line it prints."""
        for _ in range(2):
            p = self.start(argv)
            self.wait_for(lambda p=p: p.lines or p.poll() is not None, lambda: f"no line from {argv[0]}")
            line = p.lines[0] if p.lines else ""
            if NOT_STARTED not in line:
                break
        return p, line

    def bosun(self, wait, *cmd):
        return [BOSUN, "run", "--ns", self.ns, "--slots", "1", "--max-wait", str(wait), "--", *cmd]

    def hold(self):
        """A dummy job with the only place; closing its stdin frees it."""
        p, line = self.first_line(self.bosun(20, "cmd.exe", "/d", "/c", "set", "BOSUN_SLOT&", "sort", ">nul"))
        self.assertEqual(line.strip(), "BOSUN_SLOT=0")
        return p

    def slot(self, wait):
        """The place a job gets that waits that long; -1 is none."""
        out = self.call(self.bosun(wait, "cmd.exe", "/d", "/c", "set", "BOSUN_SLOT"))[1]
        found = re.search(r"BOSUN_SLOT=(-?\d+)", out)
        self.assertTrue(found, out)
        return found[1]

    def noted(self):
        return self.log.read_text(encoding="utf-8-sig") if self.log.exists() else ""

    def wait_noted(self, what):
        self.wait_for(lambda: what in self.noted(), lambda: f"{what} was never noted: {self.noted()}")

    def test_a_run_waits_for_the_place_and_goes_on_when_it_frees(self):
        dummy = self.hold()
        p, line = self.first_line(self.ref_run(self.scripts(60, 90)))
        self.assertIn(f"waiting for a place in {self.ns}", line)
        self.assertEqual(self.noted(), "")       # the VM is untouched while it waits
        dummy.stdin.close()
        code, out = self.finish(p)
        self.assertEqual(code, 7, out)
        self.assertIn("took a place", out)
        self.assertRegex(self.noted(), rf"start pid=\d+ slot=0 ns={self.ns} wslenv=\S*BOSUN_SLOT:BOSUN_NS")
        self.assertLess(self.noted().index("restore"), self.noted().index("start"))

    def test_a_place_held_too_long_never_refuses_the_run(self):
        dummy = self.hold()
        code, out = self.call(self.ref_run(self.scripts(4, 90)))
        self.assertIsNone(dummy.poll())
        self.assertEqual(code, 7, out)
        self.assertIn("starting anyway", out)
        self.assertIn("slot=-1", self.noted())

    def test_without_bosun_it_runs_directly(self):
        dummy = self.hold()
        code, out = self.call(self.ref_run_with_path('"$env:SystemRoot\\System32"', self.scripts(60, 90)))
        self.assertIsNone(dummy.poll())
        self.assertEqual(code, 7, out)
        self.assertIn("Bosun is not installed", out)
        self.assertIn("slot= ns= ", self.noted())

    def fake(self, wait):
        folder = self.tmp / "fake"
        folder.mkdir()
        made = self.call([POWERSHELL, *BYPASS, "-Command", FAKE_BOSUN.replace("@EXE@", winpath(folder) + "\\bosun.exe")])
        self.assertTrue((folder / "bosun.exe").exists(), made[1])
        path = f"'{winpath(folder)};' + $env:PATH"
        code, out = self.call(self.ref_run_with_path(path, self.scripts(wait, 6)))
        self.assertEqual(code, 7, out)
        self.assertIn("Bosun gave no place", out)
        self.assertIn("slot= ns= ", self.noted())
        # Ended, not waited for: WSL returns only once every child of the run is gone.
        self.assertFalse((folder / "lived.txt").exists())
        pid = (folder / "pid.txt").read_text()
        ask = f"[bool](Get-Process -Id {pid} -ErrorAction SilentlyContinue)"
        self.assertEqual(self.call([POWERSHELL, *BYPASS, "-Command", ask])[1].strip(), "False")
        return out

    def assert_bounded(self, out):
        took = int(re.search(r"gave no place after (\d+) s", out)[1])
        self.assertIn(took, (6, 7, 8), out)              # the limit of 6, and a second to end it

    def test_a_bosun_that_never_answers_is_ended_and_the_run_goes_on(self):
        self.assert_bounded(self.fake(1))

    def test_a_bosun_that_only_talks_is_ended_and_the_run_goes_on(self):
        self.assert_bounded(self.fake(3))

    def test_a_bosun_that_fails_is_reported_and_the_run_goes_on(self):
        out = self.fake(2)
        self.assertIn("eee", out)
        self.assertLess(len(out), 2000)

    def test_a_run_inside_the_queue_does_not_wait_for_itself(self):
        inner = self.ref_run(self.scripts(60, 90), command="nested")
        code, out = self.call(self.bosun(20, "powershell.exe", *inner[1:]))
        self.assertEqual(code, 7, out)
        self.assertNotIn("waiting for a place", out)
        self.assertIn("slot=0", self.noted())
        self.assertIn("nested exit=9 quick=True", self.noted())

    def test_a_job_started_by_a_run_passes_through(self):
        code, out = self.call(self.ref_run(self.scripts(60, 90), command="nested"))
        self.assertEqual(code, 7, out)
        self.assertIn("nested exit=9 quick=True", self.noted())
        self.assertEqual(self.slot(30), "0")

    def test_the_other_exit_codes_come_back_unchanged(self):
        folder = self.scripts(60, 90)
        self.assertEqual(self.call(self.ref_run(folder, out=False))[0], 2)
        self.assertEqual(self.call(self.ref_run(folder, name="ios-turns-fail"))[0], 3)
        self.log.unlink()
        self.start(self.ref_run(folder, command="hold"))
        self.wait_noted("start")
        began = time.monotonic()
        self.assertEqual(self.call(self.ref_run(folder))[0], 4)
        self.assertLess(time.monotonic() - began, 30)    # refused at once, not queued

    def test_stop_never_waits(self):
        dummy = self.hold()
        code, out = self.call(self.ref_run(self.scripts(60, 90), "-Stop"))
        self.assertIsNone(dummy.poll())
        self.assertEqual(code, 0, out)
        self.assertNotIn("place", out)
        self.assertNotIn("start", self.noted())

    def test_a_run_that_is_killed_frees_the_place(self):
        self.start(self.ref_run(self.scripts(60, 90), command="hold"))
        self.wait_noted("start")
        pid = re.search(r"pid=(\d+)", self.noted())[1]
        self.assertEqual(self.slot(2), "-1")             # the run has the place
        code, out = self.call([POWERSHELL, *BYPASS, "-Command", f"Stop-Process -Id {pid} -Force -ErrorAction Stop"])
        self.assertEqual(code, 0, out)
        self.assertEqual(self.slot(30), "0")

    def test_leaving_the_queue_restores_the_environment_and_frees_the_place(self):
        common = winpath(self.scripts(60, 90)) + "\\ref-common.ps1"
        other = " ".join(["bosun.exe", *self.bosun(2, "cmd.exe", "/d", "/c", "set", "BOSUN_SLOT")[1:]])
        code, out = self.call([POWERSHELL, *BYPASS, "-Command",
                               f". '{common}'; $w = \"$env:WSLENV\"; Enter-RefTurn; Enter-RefTurn; \"in=$env:BOSUN_SLOT\"; "
                               f"Exit-RefTurn; \"out=$env:BOSUN_SLOT$env:BOSUN_NS same=$(\"$env:WSLENV\" -eq $w)\"; {other}; "
                               "Enter-RefTurn; \"again=$env:BOSUN_SLOT\"; Exit-RefTurn"])
        self.assertEqual(code, 0, out)
        self.assertEqual([x for x in out.split() if "=" in x],
                         ["in=0", "out=", "same=True", "BOSUN_SLOT=0", "again=0"], out)

    def test_the_sdk_machine_is_refused_before_anything_changes(self):
        folder = self.scripts(60, 90)
        for name, path in (("ui-test-win11", None), ("UI-Test-Win11", None), ("ui-test*", None),
                           (None, "D:\\11\\ui-test-win11"), (None, "D:\\11\\x\\..\\UI-test-win11\\disks")):
            code, out = self.call(self.ref_run(folder, name=name, vm=path))
            self.assertEqual(code, 2, out)
            self.assertIn("Impossible SDK's machine", out)
        self.assertEqual(self.noted(), "")
        self.assertEqual(list(self.tmp.glob("out-*")), [])
        self.assertEqual(self.call(self.ref_run(folder, name="ui-test-win11-ours"))[0], 7)


class Sources(unittest.TestCase):
    SCRIPTS = [p for p in INV.rglob("*.ps1") if "tests" not in p.parts]

    @staticmethod
    def code(path):
        text = re.sub(r"<#.*?#>", "", path.read_text(encoding="utf-8-sig"), flags=re.S)
        return re.sub(r"(?m)(^|\s)#.*$", "", text)

    def test_only_the_shared_code_starts_the_vm(self):
        # A new script that starts the VM itself would skip the queue.
        starts = re.compile(r"\b(Start-VM|Restore-VM\w+|Resume-VM|Restart-VM|RequestStateChange)\b", re.I)
        found = {p.name: sorted(starts.findall(self.code(p))) for p in self.SCRIPTS}
        self.assertEqual({k: v for k, v in found.items() if v},
                         {"ref-common.ps1": ["Restore-VMSnapshot", "Start-VM"]})
        for p in INV.rglob("*.py"):
            if "tests" not in p.parts:
                self.assertEqual(starts.findall(p.read_text(encoding="utf-8")), [], p)

    def test_every_script_names_its_target_before_it_locks(self):
        users = [p for p in self.SCRIPTS if p.name != "ref-common.ps1" and "Enter-RefLock" in self.code(p)]
        self.assertEqual(sorted(p.name for p in users),
                         ["derive.ps1", "ref-host.ps1", "ref-run.ps1", "ref-vm.ps1", "update-iso.ps1"])
        for p in users:
            lines = [x.strip() for x in self.code(p).splitlines() if x.strip()]
            dot = next(i for i, x in enumerate(lines) if x.startswith(". (Join-Path") and "ref-common.ps1" in x)
            named = re.match(r"Assert-RefTarget \$Name @\((.*)\)$", lines[dot + 1])
            self.assertTrue(named, p.name)
            # Every string a script takes is a path, but for these.
            taken = set(re.findall(r"\[string\]\$(\w+)", self.code(p).split("$ErrorActionPreference")[0]))
            self.assertEqual({x.strip(" $") for x in named[1].split(",")}, taken - {"Name", "Checkpoint", "Command", "ProfileId"}, p.name)
            self.assertIn("Exit-RefTurn", lines, p.name)

    def test_the_queue_is_the_sdks_and_never_skips(self):
        text = self.code(INV / "ref-common.ps1")
        for line in ("$script:RefTurnNs = 'Bosun.VM'", "$script:RefTurnWaitSec = 360"):
            self.assertIn(line, text)
        self.assertIn("run --ns $script:RefTurnNs --slots 1 --max-wait $script:RefTurnWaitSec -- ", text)
        for p in self.SCRIPTS:
            self.assertNotIn("--or-skip", p.read_text(encoding="utf-8-sig"), p.name)

    def test_the_callers_outlast_the_wait(self):
        text = self.code(INV / "ref-common.ps1")
        limit, wait = (int(re.search(rf"\$script:RefTurn{n}Sec = (\d+)", text)[1]) for n in ("Limit", "Wait"))
        self.assertGreaterEqual(limit, wait + 30)
        self.assertGreaterEqual(capture.TURN_WAIT, limit + 30)
        self.assertIn("timeout + HOST_MARGIN + capture.TURN_WAIT", (INV / "host-capture.py").read_text(encoding="utf-8"))


if __name__ == "__main__":
    os.chdir(INV)
    unittest.main()
