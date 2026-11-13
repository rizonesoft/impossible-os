#!/usr/bin/env python3
"""Write one UEFI variable into a copy of an OVMF variable store (OVMF_VARS*.fd).

The smoke test boots from a fresh store, so a value an OLDER build left in a
real machine's NVRAM is never seen by any gate. On 2026-11-13 such a value, the
klog crash-region pointer ImpossibleCrashLog, named what the new layout made the
heap guard page, and the first boot of the new build faulted in Phase 0. This
tool plants the value a stale store would hold, so the boot that reads it runs
in the gates.

Format (EDK2 MdeModulePkg VariableFormat.h, authenticated store, as OVMF ships):
the firmware volume header, then a VARIABLE_STORE_HEADER whose GUID is
gEfiAuthenticatedVariableGuid, format 0x5A and state 0xFE, then records. Each
record is a 60-byte AUTHENTICATED_VARIABLE_HEADER (StartId 0x55AA, State,
Reserved, Attributes, MonotonicCount, TimeStamp, PubKeyIndex, NameSize,
DataSize, VendorGuid), the UTF-16LE name with its terminator, and the data;
the next record starts at the next 4-byte boundary. Free space is 0xFF. A
record already holding the same name and GUID is marked deleted, as the
firmware does on a rewrite.

Usage: plant.py STORE NAME GUID HEXDATA [--attrs N]
       plant.py --dump STORE
"""
from __future__ import annotations

import argparse
import struct
import sys
import uuid

AUTH_VAR_GUID = uuid.UUID("aaf32c78-947b-439a-a180-2e144ec37792")
FV_SIGNATURE = b"_FVH"
START_ID = 0x55AA
VAR_ADDED = 0x3F
VAR_DELETED = 0xFD
HEADER = struct.Struct("<HBBIQ16sIII16s")  # 60 bytes
STORE_HEADER = struct.Struct("<16sIBB")   # + 6 reserved bytes = 28


class StoreError(Exception):
    pass


def _store_bounds(data: bytes) -> tuple[int, int]:
    """Return (first record offset, end of the variable store)."""
    if data[40:44] != FV_SIGNATURE:
        raise StoreError("no firmware volume header (_FVH)")
    hlen = struct.unpack_from("<H", data, 48)[0]
    guid, size, fmt, state = STORE_HEADER.unpack_from(data, hlen)
    if uuid.UUID(bytes_le=guid) != AUTH_VAR_GUID:
        raise StoreError("variable store is not the authenticated format")
    if fmt != 0x5A or state != 0xFE:
        raise StoreError(f"variable store format 0x{fmt:x} state 0x{state:x} is not healthy")
    end = hlen + size
    if end > len(data):
        raise StoreError("variable store extends past the file")
    return hlen + STORE_HEADER.size + 6, end


def records(data: bytes):
    """Yield (offset, state, attrs, name, guid, payload) for every record."""
    p, end = _store_bounds(data)
    while p + HEADER.size <= end:
        sid, state, _r, attrs, _mc, _ts, _pk, ns, ds, guid = HEADER.unpack_from(data, p)
        if sid != START_ID:
            return
        body = p + HEADER.size
        if body + ns + ds > end:
            raise StoreError(f"record at 0x{p:x} runs past the store")
        name = data[body:body + ns].decode("utf-16le").rstrip("\0")
        yield p, state, attrs, name, uuid.UUID(bytes_le=guid), data[body + ns:body + ns + ds]
        p = (body + ns + ds + 3) & ~3


def plant(data: bytes, name: str, guid: uuid.UUID, payload: bytes, attrs: int) -> bytes:
    buf = bytearray(data)
    first, end = _store_bounds(data)
    tail = first
    for off, state, _a, n, g, _d in records(data):
        if n == name and g == guid and state == VAR_ADDED:
            buf[off + 2] = VAR_DELETED
        ns = struct.unpack_from("<I", data, off + 36)[0]
        ds = struct.unpack_from("<I", data, off + 40)[0]
        tail = (off + HEADER.size + ns + ds + 3) & ~3
    raw_name = (name + "\0").encode("utf-16le")
    rec = HEADER.pack(START_ID, VAR_ADDED, 0, attrs, 0, bytes(16), 0,
                      len(raw_name), len(payload), guid.bytes_le) + raw_name + payload
    if tail + len(rec) > end:
        raise StoreError("no room left in the variable store")
    if any(b != 0xFF for b in buf[tail:tail + len(rec)]):
        raise StoreError(f"free space at 0x{tail:x} is not erased")
    buf[tail:tail + len(rec)] = rec
    return bytes(buf)


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--dump", action="store_true", help="list the store's records")
    ap.add_argument("store")
    ap.add_argument("name", nargs="?")
    ap.add_argument("guid", nargs="?")
    ap.add_argument("hexdata", nargs="?")
    ap.add_argument("--attrs", type=lambda v: int(v, 0), default=0x7,
                    help="attributes (default NV|BS|RT = 0x7)")
    a = ap.parse_args(argv)
    try:
        data = open(a.store, "rb").read()
        if a.dump:
            for off, state, attrs, n, g, d in records(data):
                print(f"0x{off:x} state=0x{state:x} attrs=0x{attrs:x} {g} {n} {d.hex()}")
            return 0
        if not (a.name and a.guid and a.hexdata):
            ap.error("NAME, GUID and HEXDATA are required unless --dump")
        out = plant(data, a.name, uuid.UUID(a.guid), bytes.fromhex(a.hexdata), a.attrs)
    except (OSError, ValueError, StoreError) as e:
        print(f"plant.py: {e}", file=sys.stderr)
        return 1
    with open(a.store, "wb") as f:
        f.write(out)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
