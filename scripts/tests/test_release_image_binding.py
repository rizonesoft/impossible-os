#!/usr/bin/env python3
"""scripts/check-release-image-binding.sh against real GPT + FAT fixture images.

The offsets are the point of the gate, so the fixture is a genuine image with a
decoy FAT at the Makefile's old default offset: a gate that guesses an offset
reads the decoy's matching kernel and passes, one that reads the GPT does not.
"""
import os
import pathlib
import re
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


class Workflow(unittest.TestCase):
    def test_release_runs_the_gate_and_no_longer_scrapes_the_makefile(self):
        text = WORKFLOW.read_text()
        self.assertIn("bash scripts/check-release-image-binding.sh build/system-disk.img build/kernel.exe", text)
        self.assertIsNone(re.search(r"grep[^\n]*EFI_OFFSET[^\n]*Makefile", text))
        self.assertNotIn("echo 1048576", text)


if __name__ == "__main__":
    unittest.main()
