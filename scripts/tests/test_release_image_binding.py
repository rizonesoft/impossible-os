#!/usr/bin/env python3
"""scripts/check-release-image-binding.sh against real GPT + FAT fixture images.

The offsets are the point of the gate, so the fixture is a genuine image with a
decoy FAT at the Makefile's old default offset: a gate that guesses an offset
reads the decoy's matching kernel and passes, one that reads the GPT does not.
"""
import os
import pathlib
import re
import shutil
import stat
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
GATE = ROOT / "scripts" / "check-release-image-binding.sh"
WORKFLOW = ROOT / ".github" / "workflows" / "release.yml"

MIB = 1024 * 1024
ESP_TYPE = "C12A7328-F81F-11D2-BA4B-00A0C93EC93B"
RECOVERY_TYPE = "49504F53-7265-636F-7665-727900000001"
DECOY_OFFSET = 1 * MIB
ESP_OFFSET = 40 * MIB
RECOVERY_OFFSET = 80 * MIB
FAT_SIZE = 34 * MIB
KERNEL = b"gated kernel bytes\n" * 64


def run(*cmd, **kw):
    return subprocess.run(cmd, check=True, capture_output=True, timeout=60, **kw)


class Fixture:
    def __init__(self, tmp):
        self.dir = pathlib.Path(tmp)
        self.image = self.dir / "disk.img"
        self.sidecar = pathlib.Path(str(self.image) + ".info")
        self.kernel = self.dir / "kernel.exe"
        self.provenance = self.dir / "provenance.sh"
        self.kernel.write_bytes(KERNEL)
        self.provenance.write_text('#!/bin/sh\n[ "$1" = --verify-provenance ] && [ -f "$2" ]\n')
        self.provenance.chmod(self.provenance.stat().st_mode | stat.S_IXUSR)
        with open(self.image, "wb") as f:
            f.truncate(128 * MIB)
        layout = (
            "label: gpt\nsector-size: 512\n"
            f"start={ESP_OFFSET // 512}, size={FAT_SIZE // 512}, type={ESP_TYPE}\n"
            f"start={RECOVERY_OFFSET // 512}, size={FAT_SIZE // 512}, type={RECOVERY_TYPE}\n"
        )
        run("sfdisk", "--quiet", str(self.image), input=layout.encode())
        self.format_fat(DECOY_OFFSET)
        self.format_fat(ESP_OFFSET)
        self.format_fat(RECOVERY_OFFSET)
        self.write_sidecar(ESP_OFFSET, RECOVERY_OFFSET)
        self.put(DECOY_OFFSET, "::/boot", KERNEL)
        self.put(ESP_OFFSET, "::/boot", KERNEL)
        self.put_file(RECOVERY_OFFSET, "::kernel.bak", KERNEL)

    def format_fat(self, offset):
        run("mkfs.fat", "-F", "32", "-s", "1", "--offset", str(offset // 512), str(self.image), str(FAT_SIZE // 1024))

    def write_sidecar(self, esp, recovery):
        self.sidecar.write_text(f"EFI_OFFSET={esp}\nEFI_SIZE={FAT_SIZE}\nRECOVERY_OFFSET={recovery}\n")

    def put(self, offset, directory, data):
        run("mmd", "-i", f"{self.image}@@{offset}", directory)
        self.put_file(offset, f"{directory}/kernel.exe", data)

    def put_file(self, offset, target, data):
        src = self.dir / "payload"
        src.write_bytes(data)
        run("mcopy", "-o", "-i", f"{self.image}@@{offset}", str(src), target)

    def gate(self):
        env = dict(os.environ, RELEASE_PROVENANCE_CHECK=str(self.provenance))
        return subprocess.run(
            ["bash", str(GATE), str(self.image), str(self.kernel)],
            capture_output=True,
            text=True,
            env=env,
            timeout=60,
        )


class ImageBinding(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.fx = Fixture(tmp.name)

    def assertRefused(self, needle):
        res = self.fx.gate()
        self.assertNotEqual(res.returncode, 0, res.stdout)
        self.assertIn("::error::", res.stdout)
        self.assertIn(needle, res.stdout)

    def test_clean_image_binds_both_copies_at_gpt_offsets(self):
        res = self.fx.gate()
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        self.assertIn(f"ESP offset {ESP_OFFSET}", res.stdout)
        self.assertIn("OK: ESP /boot/kernel.exe", res.stdout)
        self.assertIn("OK: recovery kernel.bak", res.stdout)

    def test_stale_esp_kernel_behind_a_matching_decoy_is_refused(self):
        # The decoy at the old default offset still holds the gated bytes.
        self.fx.put_file(ESP_OFFSET, "::/boot/kernel.exe", b"stale kernel\n")
        self.assertRefused("ESP /boot/kernel.exe kernel")

    def test_stale_recovery_kernel_is_refused(self):
        self.fx.put_file(RECOVERY_OFFSET, "::kernel.bak", b"stale kernel\n")
        self.assertRefused("recovery kernel.bak kernel")

    def test_sidecar_pointing_at_the_decoy_is_refused(self):
        self.fx.write_sidecar(DECOY_OFFSET, RECOVERY_OFFSET)
        self.assertRefused(f"EFI_OFFSET={DECOY_OFFSET}, the GPT says {ESP_OFFSET}")

    def test_missing_sidecar_key_is_refused(self):
        self.fx.sidecar.write_text(f"EFI_OFFSET={ESP_OFFSET}\n")
        self.assertRefused("RECOVERY_OFFSET missing or repeated")

    def test_repeated_sidecar_key_is_refused(self):
        self.fx.sidecar.write_text(f"EFI_OFFSET={ESP_OFFSET}\nEFI_OFFSET={ESP_OFFSET}\nRECOVERY_OFFSET={RECOVERY_OFFSET}\n")
        self.assertRefused("EFI_OFFSET missing or repeated")

    def test_missing_sidecar_is_refused(self):
        self.fx.sidecar.unlink()
        self.assertRefused("cannot cross-check")

    def test_image_without_a_gpt_is_refused(self):
        with open(self.fx.image, "r+b") as f:
            f.seek(0)
            f.write(b"\0" * (34 * 512))
            f.seek(-(33 * 512), os.SEEK_END)
            f.write(b"\0" * (33 * 512))
        self.assertRefused("no single partition of type")

    def test_provenance_failure_is_refused(self):
        self.fx.provenance.write_text("#!/bin/sh\nexit 1\n")
        self.assertRefused("failed the provenance check")


class PatchBootConf(unittest.TestCase):
    """patch-boot-conf.sh writes where the image's sidecar says the ESP is."""

    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.fx = Fixture(tmp.name)
        root = self.fx.dir / "project"
        (root / "scripts").mkdir(parents=True)
        (root / "resources" / "boot").mkdir(parents=True)
        (root / "build").mkdir()
        shutil.copy(ROOT / "scripts" / "patch-boot-conf.sh", root / "scripts")
        (root / "resources" / "boot" / "boot.conf").write_text("timeout=1\n")
        self.script = root / "scripts" / "patch-boot-conf.sh"
        self.image = root / "build" / "system-disk.img"
        os.replace(self.fx.image, self.image)
        self.fx.image = self.image
        self.sidecar = pathlib.Path(str(self.image) + ".info")
        self.sidecar.write_text(f"EFI_OFFSET={ESP_OFFSET}\n")
        for offset in (DECOY_OFFSET, ESP_OFFSET):
            run("mmd", "-i", f"{self.image}@@{offset}", "::/EFI", "::/EFI/ImpossibleOS")

    def conf_at(self, offset):
        res = subprocess.run(
            ["mtype", "-i", f"{self.image}@@{offset}", "::/EFI/ImpossibleOS/boot.conf"],
            capture_output=True,
            text=True,
            timeout=60,
        )
        return res.stdout

    def patch(self):
        return subprocess.run(["bash", str(self.script), "test_suite", "fs"], capture_output=True, text=True, timeout=60)

    def test_patches_the_esp_the_sidecar_names_not_the_old_default(self):
        res = self.patch()
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        self.assertIn("test_suite=fs", self.conf_at(ESP_OFFSET))
        self.assertEqual(self.conf_at(DECOY_OFFSET), "")

    def test_missing_offset_is_refused(self):
        self.sidecar.write_text("EFI_SIZE=1\n")
        res = self.patch()
        self.assertNotEqual(res.returncode, 0)
        self.assertIn("no EFI_OFFSET", res.stdout)
        self.assertEqual(self.conf_at(DECOY_OFFSET), "")


HARNESS = r"""
param($Launcher, [switch]$Reset)
$ast = [System.Management.Automation.Language.Parser]::ParseFile($Launcher, [ref]$null, [ref]$null)
$stmt = $ast.Find({ param($n)
    $n -is [System.Management.Automation.Language.IfStatementAst] -and
    $n.Extent.Text.StartsWith('if ($PatchArgs.Count -gt 0)') }, $true)
$WSL_PROJECT = '/x'
$PatchArgs = if ($Reset) { @() } else { @('test_suite', 'fs') }
Invoke-Expression $stmt.Extent.Text
Write-Output 'REACHED-QEMU-START'
"""


@unittest.skipUnless(shutil.which("pwsh"), "pwsh not installed")
class WindowsLauncher(unittest.TestCase):
    """run-qemu.ps1 runs its real patch statement against a stub wsl.exe."""

    def launch(self, wsl_rc, *args):
        with tempfile.TemporaryDirectory() as tmp:
            stub = pathlib.Path(tmp) / "wsl.exe"
            stub.write_text(f"#!/bin/sh\nexit {wsl_rc}\n")
            stub.chmod(0o755)
            harness = pathlib.Path(tmp) / "harness.ps1"
            harness.write_text(HARNESS)
            launcher = ROOT / "scripts" / "machines" / "run-qemu.ps1"
            env = dict(os.environ, PATH=f"{tmp}:{os.environ['PATH']}")
            return subprocess.run(
                ["pwsh", "-NoProfile", "-File", str(harness), "-Launcher", str(launcher), *args],
                capture_output=True,
                text=True,
                env=env,
                timeout=60,
            )

    def test_failed_patch_stops_before_qemu(self):
        res = self.launch(7)
        self.assertEqual(res.returncode, 1, res.stdout + res.stderr)
        self.assertNotIn("REACHED-QEMU-START", res.stdout)

    def test_failed_reset_stops_before_qemu(self):
        res = self.launch(7, "-Reset")
        self.assertEqual(res.returncode, 1, res.stdout + res.stderr)
        self.assertNotIn("REACHED-QEMU-START", res.stdout)

    def test_successful_patch_and_reset_continue(self):
        for args in ((), ("-Reset",)):
            res = self.launch(0, *args)
            self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
            self.assertIn("REACHED-QEMU-START", res.stdout)


class Workflow(unittest.TestCase):
    def test_release_runs_the_gate_and_no_longer_scrapes_the_makefile(self):
        text = WORKFLOW.read_text()
        self.assertIn("bash scripts/check-release-image-binding.sh build/system-disk.img build/kernel.exe", text)
        self.assertIsNone(re.search(r"grep[^\n]*EFI_OFFSET[^\n]*Makefile", text))
        self.assertNotIn("echo 1048576", text)


if __name__ == "__main__":
    unittest.main()
