#!/usr/bin/env python3
# ============================================================================
# bootimg.py -- offline boot artifact inspector.
#
# Reads a boot image (raw / VHD / VHDX / VDI / ISO 9660) without booting it
# and prints a structured report: partition map, FAT / IXFS labels, the
# /IPOS/manifest.json content, on-disk SHA-256 vs manifest comparison,
# signature verification status, boot entries (manifest entries[] block),
# bootloader-and-kernel ABI version pin, and the media role marker.
#
# Subcommands:
#   inspect <image>        Print human-readable report (default).
#   inspect <image> --json Emit machine-parseable JSON (single object).
#   verify  <image>        Alias for `inspect` that emphasises the exit code:
#                          0 = clean, 2 = hash mismatch, 3 = signature FAIL,
#                          4 = manifest absent/malformed, 5 = unsupported
#                          format.
#
# Format detection is content-first (GPT, ISO9660, VHD/VHDX/VDI signatures);
# path extension is the fallback for ambiguous content.
#
# Virtual disk formats (VHD, VHDX, VDI) are unwrapped via `qemu-img convert
# -O raw -- <image> <tmp>`; the tmp is removed after inspection. ISO is
# parsed in-place using the standard 2 KiB sector reads (PVD at sector 16,
# El Torito Boot Record at sector 17). Raw images are parsed directly.
#
# Signature verification: today host-side Ed25519 verify is not vendored.
# Blockers tracked in todo/09-desktop-shell/TODO-07-cng-crypto.md and
# todo/15-installer-release/TODO-01-release-artifacts.md. When the .sig
# file exists we report `unverified`; absent -> `unsigned`. Once host
# crypto lands, this returns `OK` / `FAIL`.
#
# Owning TODO: todo/01-boot-platform/TODO-06 offline-artifact-inspector
# section. Manifest schema lives at docs/release/boot-artifact-
# manifest.md and is produced by scripts/release/build-manifest.sh.
# ============================================================================

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field, asdict
from pathlib import Path
from typing import Any, Optional

EXIT_OK = 0
EXIT_USAGE = 1
EXIT_HASH_MISMATCH = 2
EXIT_SIGNATURE_FAIL = 3
EXIT_MANIFEST_ABSENT = 4
EXIT_UNSUPPORTED = 5
EXIT_IO_ERROR = 6

GPT_SIGNATURE = b"EFI PART"
ISO9660_PVD_OFFSET = 0x8000
ISO9660_PVD_SIGNATURE = b"\x01CD001\x01\x00"
VHD_FOOTER_COOKIE = b"conectix"
VHDX_HEADER_SIGNATURE = b"vhdxfile"
VDI_HEADER_SIGNATURE = b"<<< Oracle VM VirtualBox Disk Image >>>"

LBA_SIZE = 512
GPT_HEADER_LBA = 1
GPT_HEADER_SIZE = 92
GPT_PARTITION_ENTRY_SIZE = 128

ESP_TYPE_GUID = "C12A7328-F81F-11D2-BA4B-00A0C93EC93B"
LINUX_BASIC_DATA_GUID = "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7"

IXFS_MAGIC = b"IXFS\x00\x00\x00\x00"

MANIFEST_PATH = "/IPOS/manifest.json"
MANIFEST_SIG_PATH = "/IPOS/manifest.json.sig"

ISO9660_SECTOR = 2048


@dataclass
class PartitionInfo:
    index: int
    type_guid: str
    unique_guid: str
    first_lba: int
    last_lba: int
    name: str
    filesystem: str = "unknown"
    label: str = ""
    size_mib: int = 0


@dataclass
class HashCheck:
    name: str
    path: str
    manifest_sha256: str
    actual_sha256: str
    size_bytes: int
    ok: bool


@dataclass
class InspectReport:
    image_path: str
    image_format: str
    image_size_bytes: int
    partitions: list[PartitionInfo] = field(default_factory=list)
    manifest_path_on_esp: Optional[str] = None
    manifest: Optional[dict[str, Any]] = None
    manifest_sha256: Optional[str] = None
    signature_status: str = "unsigned"
    signature_path: Optional[str] = None
    hash_checks: list[HashCheck] = field(default_factory=list)
    media_role: Optional[str] = None
    boot_info_version: Optional[int] = None
    bootloader_sha256: Optional[str] = None
    kernel_sha256: Optional[str] = None
    errors: list[str] = field(default_factory=list)
    warnings: list[str] = field(default_factory=list)


def _guid_from_bytes(buf: bytes) -> str:
    if len(buf) != 16:
        raise ValueError(f"GUID requires 16 bytes, got {len(buf)}")
    d1 = struct.unpack_from("<I", buf, 0)[0]
    d2 = struct.unpack_from("<H", buf, 4)[0]
    d3 = struct.unpack_from("<H", buf, 6)[0]
    d4 = buf[8:10]
    d5 = buf[10:16]
    return (
        f"{d1:08X}-{d2:04X}-{d3:04X}-"
        f"{d4[0]:02X}{d4[1]:02X}-"
        f"{d5[0]:02X}{d5[1]:02X}{d5[2]:02X}{d5[3]:02X}{d5[4]:02X}{d5[5]:02X}"
    )


def _utf16le_truncate(buf: bytes) -> str:
    end = len(buf)
    for i in range(0, len(buf), 2):
        if buf[i] == 0 and buf[i + 1] == 0:
            end = i
            break
    return buf[:end].decode("utf-16-le", errors="replace")


def detect_format(path: Path) -> str:
    try:
        with open(path, "rb") as f:
            head = f.read(4096)
            f.seek(ISO9660_PVD_OFFSET)
            pvd = f.read(8)
            try:
                f.seek(-512, os.SEEK_END)
                vhd_footer = f.read(8)
            except OSError:
                vhd_footer = b""
    except OSError as e:
        raise IOError(f"cannot read {path}: {e}") from e

    if head.startswith(VHDX_HEADER_SIGNATURE):
        return "vhdx"
    if head.startswith(VDI_HEADER_SIGNATURE):
        return "vdi"
    if vhd_footer == VHD_FOOTER_COOKIE:
        return "vhd"
    if pvd == ISO9660_PVD_SIGNATURE:
        return "iso"
    if len(head) >= 512 + 8 and head[512:520] == GPT_SIGNATURE:
        return "raw"
    suffix = path.suffix.lower().lstrip(".")
    if suffix in ("img", "raw", "bin"):
        return "raw"
    if suffix == "iso":
        return "iso"
    if suffix in ("vhd", "vhdx", "vdi"):
        return suffix
    return "unknown"


def _have_qemu_img() -> bool:
    return shutil.which("qemu-img") is not None


def unwrap_to_raw(src: Path, fmt: str, tmpdir: Path) -> Path:
    if not _have_qemu_img():
        raise RuntimeError(
            f"qemu-img not on PATH; cannot inspect {fmt} image without it"
        )
    raw = tmpdir / "image.raw"
    cmd = ["qemu-img", "convert", "-O", "raw", "--", str(src), str(raw)]
    try:
        subprocess.run(cmd, check=True, capture_output=True)
    except subprocess.CalledProcessError as e:
        raise RuntimeError(
            f"qemu-img convert failed: {e.stderr.decode(errors='replace')}"
        ) from e
    return raw


def parse_gpt(image: Path) -> list[PartitionInfo]:
    """Parse the GPT primary header + entry array.

    Rejects entries that fall outside the image, that have inverted LBAs
    (last < first), or whose type GUID is the zero sentinel. A malformed
    image therefore produces an empty partition list rather than a
    traceback during downstream filesystem detection."""
    out: list[PartitionInfo] = []
    image_bytes = image.stat().st_size
    image_lbas = image_bytes // LBA_SIZE
    with open(image, "rb") as f:
        f.seek(GPT_HEADER_LBA * LBA_SIZE)
        hdr = f.read(GPT_HEADER_SIZE)
        if len(hdr) < GPT_HEADER_SIZE or hdr[:8] != GPT_SIGNATURE:
            return out
        part_lba = struct.unpack_from("<Q", hdr, 72)[0]
        num_parts = struct.unpack_from("<I", hdr, 80)[0]
        part_size = struct.unpack_from("<I", hdr, 84)[0]
        if part_size != GPT_PARTITION_ENTRY_SIZE:
            return out
        if num_parts > 256:
            num_parts = 256
        f.seek(part_lba * LBA_SIZE)
        for i in range(num_parts):
            entry = f.read(GPT_PARTITION_ENTRY_SIZE)
            if len(entry) != GPT_PARTITION_ENTRY_SIZE:
                break
            type_guid = _guid_from_bytes(entry[0:16])
            if type_guid == "00000000-0000-0000-0000-000000000000":
                continue
            unique_guid = _guid_from_bytes(entry[16:32])
            first_lba = struct.unpack_from("<Q", entry, 32)[0]
            last_lba = struct.unpack_from("<Q", entry, 40)[0]
            # Reject malformed entries: inverted range, zero range, or
            # any LBA past EOF. detect_filesystem() and the FAT walker
            # both seek by first_lba; without these guards a hostile
            # image causes f.read(512) to return b'' and indexing
            # boot[0] crashes.
            if first_lba == 0 or last_lba < first_lba:
                continue
            if image_lbas and last_lba >= image_lbas:
                continue
            name = _utf16le_truncate(entry[56:128])
            size_bytes = (last_lba - first_lba + 1) * LBA_SIZE
            out.append(PartitionInfo(
                index=i + 1,
                type_guid=type_guid,
                unique_guid=unique_guid,
                first_lba=first_lba,
                last_lba=last_lba,
                name=name,
                size_mib=size_bytes // (1024 * 1024),
            ))
    return out


def detect_filesystem(image: Path, part: PartitionInfo) -> tuple[str, str]:
    with open(image, "rb") as f:
        f.seek(part.first_lba * LBA_SIZE)
        boot = f.read(512)
        # Short read = partition extends past EOF; treat as unknown
        # rather than indexing boot[0] on an empty bytes object.
        if len(boot) < 90:
            return ("unknown", "")
        if boot[:8] == IXFS_MAGIC:
            label = boot[24:48].rstrip(b"\x00").decode("ascii", errors="replace")
            return ("ixfs", label)
        if (boot[0] in (0xEB, 0xE9)) and boot[82:90].startswith(b"FAT32"):
            label = boot[71:82].rstrip(b" \x00").decode("ascii", errors="replace")
            return ("fat32", label)
        if (boot[0] in (0xEB, 0xE9)) and boot[54:62].startswith(b"FAT"):
            label = boot[43:54].rstrip(b" \x00").decode("ascii", errors="replace")
            return ("fat16", label)
    return ("unknown", "")


def _fat32_read_path(image: Path, part: PartitionInfo, posix_path: str) -> Optional[bytes]:
    try:
        with open(image, "rb") as f:
            f.seek(part.first_lba * LBA_SIZE)
            bpb = f.read(512)
            if not (bpb[0] in (0xEB, 0xE9) and bpb[82:90].startswith(b"FAT32")):
                return None
            bytes_per_sec = struct.unpack_from("<H", bpb, 11)[0]
            sec_per_clus = bpb[13]
            rsvd_sec = struct.unpack_from("<H", bpb, 14)[0]
            num_fats = bpb[16]
            sec_per_fat32 = struct.unpack_from("<I", bpb, 36)[0]
            root_cluster = struct.unpack_from("<I", bpb, 44)[0]
            if bytes_per_sec == 0 or sec_per_clus == 0:
                return None
            cluster_bytes = bytes_per_sec * sec_per_clus
            fat_offset = part.first_lba * LBA_SIZE + rsvd_sec * bytes_per_sec
            data_offset = (
                part.first_lba * LBA_SIZE
                + (rsvd_sec + num_fats * sec_per_fat32) * bytes_per_sec
            )

            def read_cluster(cluster: int) -> bytes:
                f.seek(data_offset + (cluster - 2) * cluster_bytes)
                return f.read(cluster_bytes)

            def fat_next(cluster: int) -> int:
                f.seek(fat_offset + cluster * 4)
                return struct.unpack("<I", f.read(4))[0] & 0x0FFFFFFF

            def follow_chain(start: int) -> bytes:
                buf = bytearray()
                cl = start
                seen: set[int] = set()
                while cl < 0x0FFFFFF8 and cl >= 2:
                    if cl in seen:
                        break
                    seen.add(cl)
                    buf.extend(read_cluster(cl))
                    cl = fat_next(cl)
                return bytes(buf)

            def list_dir(start_cluster: int) -> list[tuple[str, int, int, int]]:
                raw = follow_chain(start_cluster)
                entries: list[tuple[str, int, int, int]] = []
                long_parts: list[bytes] = []
                for i in range(0, len(raw), 32):
                    e = raw[i:i + 32]
                    if len(e) < 32:
                        break
                    if e[0] == 0x00:
                        break
                    if e[0] == 0xE5:
                        long_parts.clear()
                        continue
                    attr = e[11]
                    if attr == 0x0F:
                        seq = e[0] & 0x1F
                        chunk = e[1:11] + e[14:26] + e[28:32]
                        while len(long_parts) < seq:
                            long_parts.append(b"")
                        long_parts[seq - 1] = chunk
                        continue
                    if attr & 0x08:
                        long_parts.clear()
                        continue
                    short = e[0:11].decode("ascii", errors="replace")
                    base = short[0:8].rstrip(" ").lower()
                    ext = short[8:11].rstrip(" ").lower()
                    short_name = base + ("." + ext if ext else "")
                    if long_parts:
                        lfn = b"".join(long_parts)
                        out_chars: list[str] = []
                        for j in range(0, len(lfn), 2):
                            cp = struct.unpack("<H", lfn[j:j + 2])[0]
                            if cp == 0 or cp == 0xFFFF:
                                break
                            out_chars.append(chr(cp))
                        name = "".join(out_chars)
                        long_parts.clear()
                    else:
                        name = short_name
                    first_clus = (
                        (struct.unpack_from("<H", e, 20)[0] << 16)
                        | struct.unpack_from("<H", e, 26)[0]
                    )
                    size = struct.unpack_from("<I", e, 28)[0]
                    entries.append((name, attr, first_clus, size))
                return entries

            posix_path = posix_path.lstrip("/")
            parts = posix_path.split("/")
            cur = root_cluster
            for idx, comp in enumerate(parts):
                listing = list_dir(cur)
                hit: Optional[tuple[str, int, int, int]] = None
                for name, attr, fc, sz in listing:
                    if name.lower() == comp.lower():
                        hit = (name, attr, fc, sz)
                        break
                if not hit:
                    return None
                _name, attr, fc, sz = hit
                if idx == len(parts) - 1:
                    if attr & 0x10:
                        return None
                    raw_bytes = follow_chain(fc) if fc else b""
                    return raw_bytes[:sz]
                if not (attr & 0x10):
                    return None
                cur = fc
        return None
    except (OSError, ValueError, struct.error):
        return None


def parse_iso(image: Path) -> tuple[list[PartitionInfo], dict[str, Any]]:
    descriptors: list[PartitionInfo] = []
    meta: dict[str, Any] = {}
    with open(image, "rb") as f:
        f.seek(16 * ISO9660_SECTOR)
        pvd = f.read(ISO9660_SECTOR)
        if pvd[1:6] != b"CD001":
            return descriptors, meta
        meta["volume_id"] = pvd[40:72].rstrip(b" \x00").decode("ascii", errors="replace")
        boot_record_lba: Optional[int] = None
        sector = 16
        while True:
            f.seek(sector * ISO9660_SECTOR)
            vd = f.read(ISO9660_SECTOR)
            if vd[1:6] != b"CD001":
                break
            if vd[0] == 0xFF:
                break
            if vd[0] == 0:
                if vd[7:39].startswith(b"EL TORITO SPECIFICATION"):
                    boot_record_lba = struct.unpack_from("<I", vd, 71)[0]
            sector += 1
            if sector > 256:
                break
        if boot_record_lba is None:
            return descriptors, meta
        f.seek(boot_record_lba * ISO9660_SECTOR)
        cat = f.read(ISO9660_SECTOR)
        idx = 0
        for entry_off in (0x20, 0x40, 0x60, 0x80):
            if entry_off + 32 > len(cat):
                break
            entry = cat[entry_off:entry_off + 32]
            boot_id = entry[0]
            platform = entry[1]
            if boot_id not in (0x88, 0x91):
                continue
            if entry_off == 0x20 and boot_id == 0x88:
                idx += 1
                load_lba = struct.unpack_from("<I", entry, 8)[0]
                sec_count = struct.unpack_from("<H", entry, 6)[0]
                platform_name = {0x00: "x86", 0xEF: "uefi"}.get(platform, f"plat-{platform:#04x}")
                descriptors.append(PartitionInfo(
                    index=idx,
                    type_guid=f"ELTORITO-{platform:02X}",
                    unique_guid="",
                    first_lba=load_lba * 4,
                    last_lba=load_lba * 4 + sec_count - 1,
                    name=f"el-torito-{platform_name}",
                    size_mib=(sec_count * 512) // (1024 * 1024),
                    filesystem="el-torito",
                    label=platform_name,
                ))
    return descriptors, meta


def _validate_manifest_shape(parsed: Any) -> Optional[dict[str, Any]]:
    """Return parsed when it matches the boot-artifact-manifest schema
    enough for downstream consumers (top-level dict; entries[] is a
    list of dicts with string `path` and `sha256`); None otherwise.
    A scalar / list / wrongly-typed dict short-circuits to None so the
    caller surfaces a structured "malformed" error instead of crashing
    inside .get() on a non-dict."""
    if not isinstance(parsed, dict):
        return None
    entries = parsed.get("entries", [])
    if not isinstance(entries, list):
        return None
    for e in entries:
        if not isinstance(e, dict):
            return None
        if not isinstance(e.get("path", ""), str):
            return None
        if not isinstance(e.get("sha256", ""), str):
            return None
    return parsed


def read_manifest_from_partition(image: Path, part: PartitionInfo) -> tuple[Optional[dict[str, Any]], Optional[bytes], Optional[bytes]]:
    raw = _fat32_read_path(image, part, MANIFEST_PATH)
    if raw is None:
        return (None, None, None)
    sig = _fat32_read_path(image, part, MANIFEST_SIG_PATH)
    try:
        parsed = json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError):
        return (None, raw, sig)
    validated = _validate_manifest_shape(parsed)
    return (validated, raw, sig)


# ---------------------------------------------------------------------------
# ISO 9660 file lookup
# ---------------------------------------------------------------------------
def _iso9660_read_path(image: Path, posix_path: str) -> Optional[bytes]:
    """Walk the ISO 9660 directory hierarchy and return the file
    contents at posix_path (e.g. '/IPOS/manifest.json'), or None when
    absent. Uses the standard PVD root directory record at offset 156
    of sector 16. Single-extent files only (multi-extent CDs would
    need to chain Continuation Areas; not used by Impossible OS
    images today). Read-only."""
    try:
        with open(image, "rb") as f:
            f.seek(16 * ISO9660_SECTOR)
            pvd = f.read(ISO9660_SECTOR)
            if pvd[1:6] != b"CD001":
                return None
            # Root directory record is at offset 156, 34 bytes long.
            root = pvd[156:156 + 34]
            root_lba = struct.unpack_from("<I", root, 2)[0]
            root_size = struct.unpack_from("<I", root, 10)[0]

            def read_dir(lba: int, size: int) -> bytes:
                f.seek(lba * ISO9660_SECTOR)
                return f.read(size)

            def find_in_dir(dir_bytes: bytes, name: str, want_dir: bool) -> Optional[tuple[int, int]]:
                # ISO 9660 directory entries: variable length, length
                # in byte 0; 0 = padding to next sector boundary.
                want_upper = name.upper()
                pos = 0
                while pos < len(dir_bytes):
                    rec_len = dir_bytes[pos]
                    if rec_len == 0:
                        # Skip to next sector boundary.
                        next_sec = ((pos // ISO9660_SECTOR) + 1) * ISO9660_SECTOR
                        if next_sec >= len(dir_bytes):
                            break
                        pos = next_sec
                        continue
                    if pos + rec_len > len(dir_bytes):
                        break
                    rec = dir_bytes[pos:pos + rec_len]
                    if len(rec) < 33:
                        break
                    flags = rec[25]
                    is_dir = bool(flags & 0x02)
                    if want_dir != is_dir:
                        pos += rec_len
                        continue
                    file_id_len = rec[32]
                    if 33 + file_id_len > rec_len:
                        break
                    fid = rec[33:33 + file_id_len]
                    # Strip ISO 9660 version suffix (";N")
                    fid_str = fid.decode("ascii", errors="replace")
                    if ";" in fid_str:
                        fid_str = fid_str.split(";", 1)[0]
                    if fid_str.upper() == want_upper:
                        ext_lba = struct.unpack_from("<I", rec, 2)[0]
                        ext_size = struct.unpack_from("<I", rec, 10)[0]
                        return (ext_lba, ext_size)
                    pos += rec_len
                return None

            comps = posix_path.lstrip("/").split("/")
            cur_lba, cur_size = root_lba, root_size
            for idx, comp in enumerate(comps):
                want_dir = idx < len(comps) - 1
                listing = read_dir(cur_lba, cur_size)
                hit = find_in_dir(listing, comp, want_dir)
                if not hit:
                    return None
                cur_lba, cur_size = hit
            f.seek(cur_lba * ISO9660_SECTOR)
            return f.read(cur_size)
    except (OSError, ValueError, struct.error):
        return None


def read_manifest_from_iso(image: Path) -> tuple[Optional[dict[str, Any]], Optional[bytes], Optional[bytes]]:
    raw = _iso9660_read_path(image, MANIFEST_PATH)
    if raw is None:
        return (None, None, None)
    sig = _iso9660_read_path(image, MANIFEST_SIG_PATH)
    try:
        parsed = json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError):
        return (None, raw, sig)
    return (_validate_manifest_shape(parsed), raw, sig)


def validate_hashes_iso(image: Path, manifest: dict[str, Any]) -> tuple[list[HashCheck], int]:
    """Hash validation against ISO9660 contents. Manifest entries[]
    paths are taken as ISO paths (forward slashes); backslashes from
    the FAT shape are converted to forward slashes."""
    out: list[HashCheck] = []
    mismatch = 0
    for ent in manifest.get("entries") or []:
        path = ent.get("path", "")
        posix = path.replace("\\", "/")
        actual = _iso9660_read_path(image, posix)
        if actual is None:
            out.append(HashCheck(
                name=ent.get("name", "?"),
                path=path,
                manifest_sha256=ent.get("sha256", ""),
                actual_sha256="",
                size_bytes=0,
                ok=False,
            ))
            mismatch += 1
            continue
        digest = _sha256(actual)
        ok = digest == ent.get("sha256", "")
        if not ok:
            mismatch += 1
        out.append(HashCheck(
            name=ent.get("name", "?"),
            path=path,
            manifest_sha256=ent.get("sha256", ""),
            actual_sha256=digest,
            size_bytes=len(actual),
            ok=ok,
        ))
    return (out, mismatch)


def signature_status_for(sig_bytes: Optional[bytes]) -> tuple[str, Optional[str]]:
    if sig_bytes is None:
        return ("unsigned", None)
    return (
        "unverified (host Ed25519 not vendored; "
        "see todo/09-desktop-shell/TODO-07 + "
        "todo/15-installer-release/TODO-01)",
        MANIFEST_SIG_PATH,
    )


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def validate_hashes(
    image: Path, part: PartitionInfo, manifest: dict[str, Any]
) -> tuple[list[HashCheck], int]:
    out: list[HashCheck] = []
    mismatch = 0
    entries = manifest.get("entries") or []
    for ent in entries:
        path = ent.get("path", "")
        posix = path.replace("\\", "/")
        actual = _fat32_read_path(image, part, posix)
        if actual is None:
            out.append(HashCheck(
                name=ent.get("name", "?"),
                path=path,
                manifest_sha256=ent.get("sha256", ""),
                actual_sha256="",
                size_bytes=0,
                ok=False,
            ))
            mismatch += 1
            continue
        digest = _sha256(actual)
        ok = digest == ent.get("sha256", "")
        if not ok:
            mismatch += 1
        out.append(HashCheck(
            name=ent.get("name", "?"),
            path=path,
            manifest_sha256=ent.get("sha256", ""),
            actual_sha256=digest,
            size_bytes=len(actual),
            ok=ok,
        ))
    return (out, mismatch)


def inspect(image: Path) -> tuple[InspectReport, int]:
    fmt = detect_format(image)
    rep = InspectReport(
        image_path=str(image),
        image_format=fmt,
        image_size_bytes=image.stat().st_size,
    )
    if fmt == "unknown":
        rep.errors.append(f"unsupported image format for {image}")
        return (rep, EXIT_UNSUPPORTED)

    if fmt in ("raw", "iso"):
        raw_path = image
        tmpdir: Optional[tempfile.TemporaryDirectory] = None
    else:
        tmpdir = tempfile.TemporaryDirectory(prefix="bootimg-")
        try:
            raw_path = unwrap_to_raw(image, fmt, Path(tmpdir.name))
        except RuntimeError as e:
            rep.errors.append(str(e))
            return (rep, EXIT_UNSUPPORTED)

    try:
        if fmt == "iso":
            parts, iso_meta = parse_iso(raw_path)
            rep.partitions = parts
            if iso_meta:
                rep.warnings.append(
                    f"ISO volume id: {iso_meta.get('volume_id', '?')}"
                )
            # Read manifest from ISO9660 root and validate just like
            # the GPT/FAT path. Without this, a tampered ISO can
            # produce EXIT_OK with no manifest evidence -- the
            # false-clean attestation the offline-artifact-inspector
            # feature exists to prevent.
            manifest, raw_bytes, sig_bytes = read_manifest_from_iso(raw_path)
            if manifest is None and raw_bytes is None:
                rep.warnings.append(f"{MANIFEST_PATH} absent on ISO")
                return (rep, EXIT_MANIFEST_ABSENT)
            if manifest is None and raw_bytes is not None:
                rep.errors.append(f"{MANIFEST_PATH} present but malformed JSON or shape")
                return (rep, EXIT_MANIFEST_ABSENT)
            rep.manifest = manifest
            rep.manifest_path_on_esp = MANIFEST_PATH
            rep.manifest_sha256 = _sha256(raw_bytes) if raw_bytes else None
            rep.media_role = manifest.get("media_role")
            rep.boot_info_version = manifest.get("boot_info_version")
            rep.bootloader_sha256 = manifest.get("bootloader_sha256")
            rep.kernel_sha256 = manifest.get("kernel_sha256")
            sig_status, sig_path = signature_status_for(sig_bytes)
            rep.signature_status = sig_status
            rep.signature_path = sig_path
            rep.hash_checks, mismatches = validate_hashes_iso(raw_path, manifest)
            if mismatches > 0:
                return (rep, EXIT_HASH_MISMATCH)
            if rep.signature_status.startswith("FAIL"):
                return (rep, EXIT_SIGNATURE_FAIL)
            return (rep, EXIT_OK)

        rep.partitions = parse_gpt(raw_path)
        if not rep.partitions:
            rep.errors.append("no GPT partitions found")
            return (rep, EXIT_MANIFEST_ABSENT)
        for p in rep.partitions:
            fs, label = detect_filesystem(raw_path, p)
            p.filesystem = fs
            p.label = label

        esp = next((p for p in rep.partitions if p.type_guid == ESP_TYPE_GUID), None)
        if esp is None:
            rep.errors.append("ESP not found in GPT")
            return (rep, EXIT_MANIFEST_ABSENT)

        manifest, raw_bytes, sig_bytes = read_manifest_from_partition(raw_path, esp)
        if manifest is None and raw_bytes is None:
            rep.warnings.append(f"{MANIFEST_PATH} absent on ESP")
            return (rep, EXIT_MANIFEST_ABSENT)
        if manifest is None and raw_bytes is not None:
            rep.errors.append(f"{MANIFEST_PATH} present but malformed JSON")
            return (rep, EXIT_MANIFEST_ABSENT)

        rep.manifest = manifest
        rep.manifest_path_on_esp = MANIFEST_PATH
        rep.manifest_sha256 = _sha256(raw_bytes) if raw_bytes else None
        rep.media_role = manifest.get("media_role")
        rep.boot_info_version = manifest.get("boot_info_version")
        rep.bootloader_sha256 = manifest.get("bootloader_sha256")
        rep.kernel_sha256 = manifest.get("kernel_sha256")

        sig_status, sig_path = signature_status_for(sig_bytes)
        rep.signature_status = sig_status
        rep.signature_path = sig_path

        rep.hash_checks, mismatches = validate_hashes(raw_path, esp, manifest)
        if mismatches > 0:
            return (rep, EXIT_HASH_MISMATCH)
        if rep.signature_status.startswith("FAIL"):
            return (rep, EXIT_SIGNATURE_FAIL)
        return (rep, EXIT_OK)
    finally:
        if tmpdir is not None:
            tmpdir.cleanup()


def render_human(rep: InspectReport) -> str:
    out: list[str] = []
    out.append(f"Image:          {rep.image_path}")
    out.append(f"Format:         {rep.image_format}")
    out.append(f"Size:           {rep.image_size_bytes} bytes ({rep.image_size_bytes // (1024*1024)} MiB)")
    if rep.boot_info_version is not None:
        out.append(f"BootInfoVer:    {rep.boot_info_version}")
    if rep.media_role:
        out.append(f"MediaRole:      {rep.media_role}")
    if rep.manifest_sha256:
        out.append(f"Manifest:       {rep.manifest_path_on_esp} (sha256={rep.manifest_sha256})")
    out.append(f"Signature:      {rep.signature_status}")
    if rep.signature_path:
        out.append(f"SignaturePath:  {rep.signature_path}")
    out.append("")
    out.append("Partitions:")
    if not rep.partitions:
        out.append("  (none)")
    for p in rep.partitions:
        out.append(
            f"  [{p.index}] {p.name or '(unnamed)'} "
            f"type={p.type_guid} fs={p.filesystem} label={p.label!r} "
            f"size={p.size_mib} MiB lba=[{p.first_lba}..{p.last_lba}]"
        )
    out.append("")
    if rep.hash_checks:
        out.append("Hash validation (manifest entries vs on-disk):")
        for h in rep.hash_checks:
            verdict = "OK" if h.ok else "MISMATCH"
            out.append(
                f"  {verdict:8s} {h.name:14s} {h.path}  "
                f"{h.actual_sha256 or '(absent)'}"
            )
        out.append("")
    if rep.warnings:
        out.append("Warnings:")
        for w in rep.warnings:
            out.append(f"  {w}")
        out.append("")
    if rep.errors:
        out.append("Errors:")
        for e in rep.errors:
            out.append(f"  {e}")
        out.append("")
    return "\n".join(out)


def render_json(rep: InspectReport) -> str:
    return json.dumps(asdict(rep), indent=2, sort_keys=True)


def cmd_inspect(args: argparse.Namespace) -> int:
    image = Path(args.image)
    if not image.exists():
        print(f"[ERROR] {image}: not found", file=sys.stderr)
        return EXIT_USAGE
    try:
        rep, exit_code = inspect(image)
    except IOError as e:
        print(f"[ERROR] {e}", file=sys.stderr)
        return EXIT_IO_ERROR
    if args.json:
        print(render_json(rep))
    else:
        print(render_human(rep))
    return exit_code


def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        prog="bootimg",
        description="Offline boot artifact inspector for Impossible OS images.",
    )
    sub = parser.add_subparsers(dest="cmd", required=True)
    p_inspect = sub.add_parser(
        "inspect", help="Inspect an image and print a structured report.")
    p_inspect.add_argument("image", help="Path to .img / .iso / .vhd / .vhdx / .vdi")
    p_inspect.add_argument("--json", action="store_true", help="Emit JSON instead of human text")
    p_inspect.set_defaults(func=cmd_inspect)
    p_verify = sub.add_parser(
        "verify", help="Alias for inspect; emphasises the exit code (non-zero on tamper / sig fail).")
    p_verify.add_argument("image", help="Path to .img / .iso / .vhd / .vhdx / .vdi")
    p_verify.add_argument("--json", action="store_true", help="Emit JSON instead of human text")
    p_verify.set_defaults(func=cmd_inspect)
    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
