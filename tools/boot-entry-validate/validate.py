#!/usr/bin/env python3
"""Boot Entry Store host validator.

Validates `\\EFI\\ImpossibleOS\\bootentries.json` (or an offline copy) against the schema
defined in docs/boot/boot-entry-schema.md. Constants here mirror include/boot/boot_entries.h;
they MUST stay in sync. A drift detector is filed as a follow-up under tools/ + tests CI.

Exit codes:
  0 -- store is valid
  1 -- store fails validation (reason printed to stderr with [FAIL] prefix)
  2 -- usage error (missing argument, file not readable)

Usage:
  python3 tools/boot-entry-validate/validate.py <path-to-bootentries.json>
  python3 tools/boot-entry-validate/validate.py --emit-crc <path-to-bootentries.json>
      (recompute the store's crc32 field in-place; useful for editing the sample)
"""

from __future__ import annotations

import errno
import json
import math
import os
import re
import tempfile
import sys
import zlib
from pathlib import Path
from typing import Any
from urllib.parse import urlsplit

# ---- Mirror constants from include/boot/boot_entries.h ----

SCHEMA_VERSION = 1

MAX_ENTRIES = 64
MAX_TOTAL_BYTES = 16 * 1024
MAX_TITLE_LEN = 63
MAX_ID_LEN = 47
MAX_PATH_LEN = 255
MAX_SORT_KEY_LEN = 63       # BOOT_ENTRIES_MAX_SORT_KEY_LEN
MAX_POLICY_TAGS = 4         # BOOT_ENTRIES_MAX_POLICY_TAGS
MAX_POLICY_TAG_LEN = 23     # BOOT_ENTRIES_MAX_POLICY_TAG_LEN
MAX_HEALTH_SUBSET_NAMES = 8   # BOOT_ENTRIES_HEALTH_SUBSET_MAX_NAMES
MAX_HEALTH_SUBSET_NAME_LEN = 23  # BOOT_ENTRIES_HEALTH_SUBSET_NAME_LEN - 1 (NUL)

KNOWN_FLAGS = frozenset({
    "active",
    "hidden",
    "trusted_chainload",
    "hide_when_alone",
    "allow_editor",
})

# Stable kind names (numeric 0..9). Kinds with numeric value 100..199 are vendor / experimental
# and skipped-with-warn for unknown values. >=200 is reserved-future (also skipped).
KIND_NAMES = {
    "split": 0,
    "uki": 1,
    "chainload": 2,
    "network": 3,
    "resume": 4,
    "recovery": 5,
    "installer": 6,
    "safe": 7,
    "diagnostics": 8,
    "test": 9,
}

VENDOR_KIND_FIRST = 100
VENDOR_KIND_LAST = 199

# Per-kind required fields. Optional fields are not exhaustively listed here -- the schema doc
# (docs/boot/boot-entry-schema.md "Per-Kind Fields") is authoritative; this validator checks the
# load-bearing required set.
PER_KIND_REQUIRED = {
    "split":       {"kernel", "cmdline", "root"},
    "uki":         {"uki_path"},
    "chainload":   {"efi_path", "device_guid"},
    "network":     {"url", "uri_scheme", "asset_digest"},
    "resume":      {"snapshot_path", "snapshot_digest"},
    "recovery":    {"recovery_partition_guid"},
    "installer":   {"installer_image_guid", "media_role"},
    "safe":        {"safe_mode_subset", "kernel"},
    "diagnostics": {"kernel", "verbose_log"},
    "test":        {"kernel", "test_suite"},
}

VALID_INSTALLER_ROLES = frozenset({"live", "install", "recovery-install"})
VALID_SAFE_SUBSETS = frozenset({"minimal", "network", "cmd"})
VALID_TEST_SUITES = frozenset({
    "mm", "fs", "boot", "ob", "security", "ipc", "sched", "abi", "storage", "exec",
})
VALID_NETWORK_SCHEMES = frozenset({"http", "https", "tftp"})

# UKI path prefix policy (per docs/boot/boot-entry-schema.md kind: uki).
UKI_PATH_PREFIXES = ("\\EFI\\Linux\\", "\\EFI\\ImpossibleOS\\")

# RFC 4122 UUID textual form (8-4-4-4-12 hex).
_UUID_RE = re.compile(
    r"^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$"
)

# Kebab-case id: lowercase letters/digits, dash-separated, no leading/trailing dash.
_KEBAB_ID_RE = re.compile(r"^[a-z0-9]+(?:-[a-z0-9]+)*$")

# The crc32 header's VALUE shape: exactly `0x` + 8 hex digits, lowercase `x`. This is
# a shape check on an already-located value, never a search -- the field is located
# structurally by _root_crc32_span() below, because a value-blind search matches the
# characters `crc32` inside any string VALUE that happens to contain them.
_CRC_VALUE_RE = re.compile(rb"^0x([0-9a-fA-F]{8})$")


def fail(msg: str) -> None:
    """Print [FAIL] line to stderr and exit 1."""
    print(f"[FAIL] {msg}", file=sys.stderr)
    sys.exit(1)


def warn(msg: str) -> None:
    print(f"[WARN] {msg}", file=sys.stderr)


def _is_int(x: Any) -> bool:
    """Strict integer check (excludes bool). Python's `isinstance(True, int)` is True; here it isn't."""
    return isinstance(x, int) and not isinstance(x, bool)


# ---- Strict JSON load (unique, literally-spelled key names) ----
#
# `json.loads` accepts a repeated key and silently keeps the last one. The firmware
# parser cannot afford that: its `entries` cap counter is per-occurrence while the
# output index spans occurrences, so a second `entries` array used to write past the
# 64-slot output array (boot_entries_parser.c key_seen_before). The firmware now
# rejects any repeated key, and the host must reject the same stores or it would
# bless a file that fails to boot.
#
# The same applies to escape sequences in KEY names. The firmware compares key bytes
# raw, and find_crc_field() matches the root CRC key's RAW bytes against `crc32`, so
# an escaped spelling is not the same key there while `json.loads` decodes it into
# one.
#
# Deliberate asymmetry: this runs at EVERY object level, including inside `payload`
# objects that the firmware envelope parser skips wholesale. The host is the producer
# and validates payload contents anyway (validate_payload); being stricter there is
# producer validation, not a claim that the two parsers accept identical inputs.

def _key_spans(text: str):
    """Yield (offset, raw_body) for every string in KEY position.

    In valid JSON a string followed by `:` is always a key -- a string VALUE is
    always followed by `,`, `}` or `]`. This scans strictly left to right,
    consuming each string as a unit, so a candidate can never BEGIN inside
    another string. That is the reason it is a scanner and not a regex: a regex
    alternation is free to start matching at an escaped quote in the middle of a
    value, which turns a value like `"\\"a\\\\b\\":x"` into a spurious key.
    Only meaningful on text json.loads has already proven well-formed.
    """
    i, n = 0, len(text)
    while i < n:
        if text[i] != '"':
            i += 1
            continue
        start = i
        i += 1
        body_start = i
        while i < n:
            ch = text[i]
            if ch == "\\":
                i += 2
                continue
            if ch == '"':
                break
            i += 1
        body = text[body_start:i]
        i += 1                       # step past the closing quote
        j = i
        while j < n and text[j] in " \t\r\n":
            j += 1
        if j < n and text[j] == ":":
            yield start, body


def _reject_duplicate_keys(pairs: list[tuple[str, Any]]) -> dict:
    seen: set[str] = set()
    for key, _ in pairs:
        if key in seen:
            fail(f"repeated key {key!r} in a JSON object (keys must be unique)")
        seen.add(key)
    return dict(pairs)


def strict_loads(text: str) -> Any:
    """json.loads with the store's two key rules enforced: keys are unique within
    an object, and key names are spelled literally. Raises json.JSONDecodeError on
    malformed input (callers own the message); calls fail() on a rule violation."""
    data = json.loads(
        text,
        object_pairs_hook=_reject_duplicate_keys,
        parse_constant=lambda c: fail(f"non-finite JSON token {c!r} not allowed"),
    )
    # Only meaningful once json.loads has proven the text is well-formed JSON.
    for offset, body in _key_spans(text):
        if "\\" in body:
            fail(f"key name at offset {offset} uses a JSON escape; "
                 "key names must be spelled literally")
    return data


def _is_bool(x: Any) -> bool:
    return isinstance(x, bool)


def _expect_str(idx: int, eid: str, kind: str, payload: dict, key: str, *, max_len: int | None = None) -> None:
    if key not in payload:
        return  # missing-required is checked elsewhere
    val = payload[key]
    if not isinstance(val, str):
        fail(f"entry[{idx}] {eid} (kind={kind}): {key} must be string, got {type(val).__name__}")
    if max_len is not None and len(val) > max_len:
        fail(f"entry[{idx}] {eid} (kind={kind}): {key} length {len(val)} > max {max_len}")


def _expect_ascii_clean(idx: int, eid: str, kind: str, payload: dict, key: str, *, max_len: int) -> None:
    """ASCII-printable (0x20..0x7E only), NUL- and control-byte free, length-bounded.

    Applied to path / cmdline / URL fields per the schema's "ASCII" contract for paths. The
    bootloader will eventually consume these as NUL-terminated C strings; non-ASCII or embedded
    control bytes are rejected to prevent the host validator from blessing a store the firmware
    would mis-interpret or truncate.
    """
    if key not in payload:
        return
    val = payload[key]
    if not isinstance(val, str):
        fail(f"entry[{idx}] {eid} (kind={kind}): {key} must be string, got {type(val).__name__}")
    if len(val) > max_len:
        fail(f"entry[{idx}] {eid} (kind={kind}): {key} length {len(val)} > max {max_len}")
    for i, ch in enumerate(val):
        cp = ord(ch)
        if cp == 0:
            fail(f"entry[{idx}] {eid} (kind={kind}): {key} contains NUL byte at offset {i}")
        if cp < 0x20 or cp == 0x7F:
            fail(f"entry[{idx}] {eid} (kind={kind}): {key} contains control byte 0x{cp:02X} at offset {i}")
        if cp > 0x7F:
            fail(f"entry[{idx}] {eid} (kind={kind}): {key} contains non-ASCII byte 0x{cp:02X} at offset {i}")


def _expect_uuid(idx: int, eid: str, kind: str, payload: dict, key: str) -> None:
    if key not in payload:
        return
    val = payload[key]
    if not isinstance(val, str) or not _UUID_RE.match(val):
        fail(f"entry[{idx}] {eid} (kind={kind}): {key} must be RFC 4122 UUID format, got {val!r}")


def _check_esp_path_grammar(idx: int, eid: str, kind: str, key: str, val: str, *, label: str = None) -> None:
    """ESP-path grammar gate: non-empty, leading backslash, no '.' or '..' segments.

    Layered on top of _expect_ascii_clean (byte safety). Catches empty/relative/drive-form/
    traversal paths the host validator would otherwise bless. The bootloader runtime parser
    is the authoritative validator; this is host-side defense in depth so a producer's mistake
    fails loud at CI time rather than at first boot.

    `label` is used in the error message when key alone doesn't disambiguate (e.g. initrd[i]).
    """
    descriptor = label or key
    if val == "":
        fail(f"entry[{idx}] {eid} (kind={kind}): {descriptor} must be non-empty ESP-relative path")
    if not val.startswith("\\"):
        fail(f"entry[{idx}] {eid} (kind={kind}): {descriptor} must start with '\\\\' (ESP-relative absolute), got {val!r}")
    # Split on backslash; reject any '.' or '..' segment.
    for seg in val.split("\\"):
        if seg == "..":
            fail(f"entry[{idx}] {eid} (kind={kind}): {descriptor} contains '..' traversal segment")
        if seg == ".":
            fail(f"entry[{idx}] {eid} (kind={kind}): {descriptor} contains '.' segment")


def _expect_esp_path(idx: int, eid: str, kind: str, payload: dict, key: str, *, max_len: int) -> None:
    """ESP-relative path: ASCII-clean (byte safety) + non-empty + leading-backslash + no traversal."""
    if key not in payload:
        return
    _expect_ascii_clean(idx, eid, kind, payload, key, max_len=max_len)
    val = payload[key]
    if isinstance(val, str):  # _expect_ascii_clean already failed on non-string
        _check_esp_path_grammar(idx, eid, kind, key, val)


def _expect_ascii_path_list(idx: int, eid: str, kind: str, payload: dict, key: str, *, max_items: int, max_len: int) -> None:
    """Array of ESP-relative path strings (per-item byte + grammar validation).

    Each element must pass: type=str, length<=max_len, byte-clean (no NUL/control/non-ASCII),
    and ESP-path grammar (non-empty, leading backslash, no '.' or '..' segments).
    """
    if key not in payload:
        return
    val = payload[key]
    if not isinstance(val, list):
        fail(f"entry[{idx}] {eid} (kind={kind}): {key} must be array, got {type(val).__name__}")
    if len(val) > max_items:
        fail(f"entry[{idx}] {eid} (kind={kind}): {key} length {len(val)} > max {max_items}")
    for i, item in enumerate(val):
        if not isinstance(item, str):
            fail(f"entry[{idx}] {eid} (kind={kind}): {key}[{i}] must be string, got {type(item).__name__}")
        if len(item) > max_len:
            fail(f"entry[{idx}] {eid} (kind={kind}): {key}[{i}] length {len(item)} > max {max_len}")
        for j, ch in enumerate(item):
            cp = ord(ch)
            if cp == 0:
                fail(f"entry[{idx}] {eid} (kind={kind}): {key}[{i}] contains NUL byte at offset {j}")
            if cp < 0x20 or cp == 0x7F:
                fail(f"entry[{idx}] {eid} (kind={kind}): {key}[{i}] contains control byte 0x{cp:02X} at offset {j}")
            if cp > 0x7F:
                fail(f"entry[{idx}] {eid} (kind={kind}): {key}[{i}] contains non-ASCII byte 0x{cp:02X} at offset {j}")
        _check_esp_path_grammar(idx, eid, kind, key, item, label=f"{key}[{i}]")


def _expect_split_root(idx: int, eid: str, payload: dict) -> None:
    """split's `root` must be slot id 'A', 'B', or RFC 4122 UUID per schema."""
    if "root" not in payload:
        return
    val = payload["root"]
    if not isinstance(val, str):
        fail(f"entry[{idx}] {eid} (kind=split): root must be string, got {type(val).__name__}")
    if val in ("A", "B"):
        return
    if _UUID_RE.match(val):
        return
    fail(f"entry[{idx}] {eid} (kind=split): root must be 'A', 'B', or RFC 4122 UUID, got {val!r}")


def _expect_int(idx: int, eid: str, kind: str, payload: dict, key: str, *, lo: int, hi: int) -> None:
    if key not in payload:
        return
    val = payload[key]
    if not _is_int(val) or not (lo <= val <= hi):
        fail(f"entry[{idx}] {eid} (kind={kind}): {key} must be int in {lo}..{hi}, got {val!r}")


def _expect_bool(idx: int, eid: str, kind: str, payload: dict, key: str) -> None:
    if key not in payload:
        return
    val = payload[key]
    if not _is_bool(val):
        fail(f"entry[{idx}] {eid} (kind={kind}): {key} must be bool, got {type(val).__name__}")


def _expect_hex(idx: int, eid: str, kind: str, payload: dict, key: str, *, length: int) -> None:
    if key not in payload:
        return
    val = payload[key]
    if not isinstance(val, str) or len(val) != length:
        fail(f"entry[{idx}] {eid} (kind={kind}): {key} must be {length}-char hex string")
    if not all(c in "0123456789abcdefABCDEF" for c in val):
        fail(f"entry[{idx}] {eid} (kind={kind}): {key} must be hex digits only")


def _expect_enum(idx: int, eid: str, kind: str, payload: dict, key: str, *, allowed: frozenset[str]) -> None:
    if key not in payload:
        return
    val = payload[key]
    if not isinstance(val, str) or val not in allowed:
        fail(f"entry[{idx}] {eid} (kind={kind}): {key} must be one of {sorted(allowed)}, got {val!r}")


class _CrcLocateError(Exception):
    """The root object's grammar broke before the crc32 header was reached."""


# The locator is a byte-level MIRROR of the firmware's lexer + skip_value_depth
# (src/boot/uefi/boot_entries_parser.c). It has to be: the two sides must agree on
# which bytes carry the CRC, and a host more permissive than the firmware would
# stamp a store the firmware then refuses to boot from. So the token rules below are
# the firmware's rules, not Python's -- strict RFC 8259 strings (no raw controls, no
# invented escapes), literals spelled exactly, and numbers accepted as TOKENS
# wherever lex_number() accepts them, even where it marks the parsed value unusable.
_JSON_WS = b" \t\r\n"
_JSON_SINGLE_ESCAPES = b'"\\/bfnrt'
_HEX_DIGITS = b"0123456789abcdefABCDEF"

# Token kinds. A string also reports the span BETWEEN the quotes, which is what both
# the key comparison and the CRC value shape check read.
_T_EOF, _T_LBRACE, _T_RBRACE, _T_LBRACKET, _T_RBRACKET = "eof", "{", "}", "[", "]"
_T_COLON, _T_COMMA, _T_STR, _T_ATOM = ":", ",", "str", "atom"


def _skip_ws(raw: bytes, i: int) -> int:
    while i < len(raw) and raw[i] in _JSON_WS:
        i += 1
    return i


def _scan_string(raw: bytes, i: int) -> tuple[int, int, int]:
    """`raw[i]` is the opening quote. Returns (content_start, content_end, next_index).

    Escapes are validated but NOT decoded: the caller compares raw bytes, exactly as
    the firmware does, so an escaped spelling is never the literal key. Mirrors
    lex_string(): only the eight RFC 8259 single-character escapes, `\\uHHHH` with
    exactly four hex digits, and no raw byte below 0x20 inside the string.
    """
    n = len(raw)
    if i >= n or raw[i] != 0x22:
        raise _CrcLocateError(f"expected a string at byte {i}")
    j = i + 1
    while j < n:
        c = raw[j]
        if c == 0x22:
            return i + 1, j, j + 1
        if c == 0x5C:                      # backslash
            if j + 1 >= n:
                raise _CrcLocateError(f"escape runs past the end at byte {j}")
            esc = raw[j + 1]
            if esc == 0x75:                # u
                if j + 5 >= n or any(raw[j + 2 + k] not in _HEX_DIGITS for k in range(4)):
                    raise _CrcLocateError(f"unicode escape without 4 hex digits at byte {j}")
                j += 6
                continue
            if esc not in _JSON_SINGLE_ESCAPES:
                raise _CrcLocateError(f"invalid escape at byte {j}")
            j += 2
            continue
        if c < 0x20:
            raise _CrcLocateError(f"raw control byte 0x{c:02X} inside a string at byte {j}")
        j += 1
    raise _CrcLocateError(f"unterminated string starting at byte {i}")


def _next_token(raw: bytes, i: int) -> tuple[str, int, int, int]:
    """Return (kind, content_start, content_end, next_index) for the token at/after `i`.

    content_start/content_end span the bytes between the quotes for a string, and the
    token's own bounds otherwise. Mirrors lex_next().
    """
    i = _skip_ws(raw, i)
    n = len(raw)
    if i >= n:
        return _T_EOF, i, i, i
    c = raw[i]
    single = {0x7B: _T_LBRACE, 0x7D: _T_RBRACE, 0x5B: _T_LBRACKET,
              0x5D: _T_RBRACKET, 0x3A: _T_COLON, 0x2C: _T_COMMA}
    if c in single:
        return single[c], i, i + 1, i + 1
    if c == 0x22:
        cs, ce, nxt = _scan_string(raw, i)
        return _T_STR, cs, ce, nxt
    for lit in (b"true", b"false", b"null"):
        if c == lit[0]:
            if raw[i:i + len(lit)] != lit:
                raise _CrcLocateError(f"bad literal at byte {i}")
            return _T_ATOM, i, i + len(lit), i + len(lit)
    if c == 0x2D or 0x30 <= c <= 0x39:      # '-' or digit
        j = i + 1 if c == 0x2D else i
        digits = 0
        while j < n and 0x30 <= raw[j] <= 0x39:
            j += 1
            digits += 1
        if digits == 0:
            raise _CrcLocateError(f"number without digits at byte {i}")
        # lex_number() accepts a fraction/exponent as a TOKEN (it only marks the
        # parsed value unusable), so the mirror must accept the same byte run.
        if j < n and raw[j] in b".eE":
            while j < n and raw[j] in b"0123456789.eE+-":
                j += 1
        return _T_ATOM, i, j, j
    raise _CrcLocateError(f"unparsable byte 0x{c:02X} at {i}")


def _skip_value(raw: bytes, i: int) -> int:
    """`i` is at the FIRST TOKEN of a value. Returns the index just past the value.

    Mirrors skip_value_depth(): an explicit container stack that VALIDATES object and
    array grammar, not a brace count. `{"k":}` balances and is still malformed, and
    the firmware rejects it before it ever reaches the header -- so a host that
    accepted it would claim a parity the two sides do not have.
    """
    kind, _cs, _ce, nxt = _next_token(raw, i)
    if kind in (_T_STR, _T_ATOM):
        return nxt
    if kind not in (_T_LBRACE, _T_LBRACKET):
        raise _CrcLocateError(f"expected a value at byte {i}")

    OBJ_FIRST_KEY, OBJ_NEXT_KEY, OBJ_COMMA_END = "ofk", "onk", "oce"
    ARR_FIRST_VAL, ARR_NEXT_VAL, ARR_COMMA_END = "afv", "anv", "ace"
    stack = [OBJ_FIRST_KEY if kind == _T_LBRACE else ARR_FIRST_VAL]
    i = nxt
    while stack:
        if len(stack) > MAX_SCAN_DEPTH:
            raise _CrcLocateError(
                f"value nests over the firmware's {MAX_SCAN_DEPTH}-container scan budget")
        kind, _cs, _ce, i = _next_token(raw, i)
        if kind == _T_EOF:
            raise _CrcLocateError("input ended inside a value")
        st = stack[-1]

        if st in (OBJ_FIRST_KEY, OBJ_NEXT_KEY):
            if st == OBJ_FIRST_KEY and kind == _T_RBRACE:
                stack.pop()
                continue
            if kind != _T_STR:
                raise _CrcLocateError(f"expected an object key at byte {i}")
            kind, _cs, _ce, i = _next_token(raw, i)
            if kind != _T_COLON:
                raise _CrcLocateError(f"expected a colon at byte {i}")
            kind, _cs, _ce, i = _next_token(raw, i)
            if kind in (_T_LBRACE, _T_LBRACKET):
                stack[-1] = OBJ_COMMA_END
                stack.append(OBJ_FIRST_KEY if kind == _T_LBRACE else ARR_FIRST_VAL)
            elif kind in (_T_STR, _T_ATOM):
                stack[-1] = OBJ_COMMA_END
            else:
                raise _CrcLocateError(f"expected a value at byte {i}")
        elif st == OBJ_COMMA_END:
            if kind == _T_RBRACE:
                stack.pop()
                continue
            if kind != _T_COMMA:
                raise _CrcLocateError(f"expected a comma or closing brace at byte {i}")
            stack[-1] = OBJ_NEXT_KEY
        elif st in (ARR_FIRST_VAL, ARR_NEXT_VAL):
            if st == ARR_FIRST_VAL and kind == _T_RBRACKET:
                stack.pop()
                continue
            if kind in (_T_LBRACE, _T_LBRACKET):
                stack[-1] = ARR_COMMA_END
                stack.append(OBJ_FIRST_KEY if kind == _T_LBRACE else ARR_FIRST_VAL)
            elif kind in (_T_STR, _T_ATOM):
                stack[-1] = ARR_COMMA_END
            else:
                raise _CrcLocateError(f"expected an array element at byte {i}")
        else:  # ARR_COMMA_END
            if kind == _T_RBRACKET:
                stack.pop()
                continue
            if kind != _T_COMMA:
                raise _CrcLocateError(f"expected a comma or closing bracket at byte {i}")
            stack[-1] = ARR_NEXT_VAL
    return i


def _root_crc32_span(raw: bytes) -> tuple[int, int] | None:
    """Locate the ROOT object's crc32 member structurally.

    Returns (offset_of_8_hex_digits, expected_value), or None when the root walked
    cleanly and carries no usable crc32 member. Raises _CrcLocateError when the root
    object's grammar breaks before the header is reached.

    Only a key at depth 1 of the root object can match, which is the whole point: a
    value-blind search matches `crc32` inside a string VALUE that precedes the real
    header (`{"note":"crc32", ...}`) or inside a nested `payload` member, and firmware
    and host then disagree about which bytes carry the CRC.
    """
    kind, _cs, _ce, i = _next_token(raw, 0)
    if kind != _T_LBRACE:
        raise _CrcLocateError("top-level value is not an object")
    # A `}` closes the object legally at the FIRST key position and illegally right
    # after a comma. Without this flag `{"schema_version":1,}` -- a trailing comma the
    # firmware rejects -- reads as "the root carries no crc32 member".
    after_comma = False
    while True:
        kind, key_start, key_end, i = _next_token(raw, i)
        if kind == _T_RBRACE:
            if after_comma:
                raise _CrcLocateError(f"trailing comma before the closing brace at byte {key_start}")
            return None
        if kind == _T_EOF:
            raise _CrcLocateError("input ended inside the root object")
        if kind != _T_STR:
            raise _CrcLocateError(f"expected a root key at byte {key_start}")
        kind, _cs, _ce, i = _next_token(raw, i)
        if kind != _T_COLON:
            raise _CrcLocateError(f"expected a colon after the root key at byte {key_start}")
        if raw[key_start:key_end] == b"crc32":
            # First occurrence wins; a repeated root key is rejected by the strict
            # load. The value must be a string of exactly `0x` + 8 hex digits -- the
            # span the producer patches in place, so a longer, differently-cased or
            # newline-bearing one has no agreed 8 bytes. fullmatch, not match: `$`
            # also matches before a trailing newline, and an 11-byte value would then
            # locate here and be ABSENT to the firmware.
            kind, val_start, val_end, _ = _next_token(raw, i)
            # Truncation right after the colon is a broken root object, not a
            # missing member -- the firmware makes the same distinction.
            if kind == _T_EOF:
                raise _CrcLocateError("input ended after the crc32 key's colon")
            if kind != _T_STR:
                return None
            m = _CRC_VALUE_RE.fullmatch(raw[val_start:val_end])
            if not m:
                return None
            return val_start + 2, int(m.group(1).decode("ascii"), 16)
        i = _skip_value(raw, i)
        kind, _cs, _ce, i = _next_token(raw, i)
        if kind == _T_RBRACE:
            return None
        if kind != _T_COMMA:
            raise _CrcLocateError(f"expected a comma or closing brace after a root value at byte {i}")
        after_comma = True


def find_crc_field(raw: bytes) -> tuple[int, int]:
    """Locate the crc32 field's 8 hex digit byte range in raw file bytes.

    Returns (offset, expected_value) where offset is the byte index of the first hex digit
    and expected_value is the parsed uint32. Calls fail() if the field is absent or
    mis-shaped, matching the firmware's BOOT_ENTRIES_REJECT_BAD_CRC32_FIELD, or if the
    root object is malformed before the header, matching its JSON_PARSE reject.

    Producer requirement: emit the crc32 field as a ROOT member `"crc32": "0xHHHHHHHH"`.
    Whitespace around the key, colon, and value is permitted; the value itself must be
    exactly ten characters with a lowercase `x`.
    """
    try:
        found = _root_crc32_span(raw)
    except _CrcLocateError as exc:
        fail(f"malformed JSON before the crc32 header: {exc}")
        return 0, 0  # unreachable
    if found is None:
        fail('no root crc32 member of the form "crc32": "0xHHHHHHHH"')
        return 0, 0  # unreachable
    return found


def compute_crc_from_file(raw: bytes) -> int:
    """IEEE 802.3 CRC-32 over file bytes with the crc32 field's 8 hex digits zeroed.

    GPT-style: locate the crc32 field, replace the 8 hex chars with `00000000` in a
    scratch copy, CRC the result. Independent of JSON canonicalization -- producers
    just need stable bytes (the bootloader, validate.py --emit-crc, and bootcfg.exe
    all emit deterministic output).
    """
    offset, _ = find_crc_field(raw)
    zeroed = raw[:offset] + b"00000000" + raw[offset + 8:]
    return zlib.crc32(zeroed) & 0xFFFFFFFF


def validate_envelope(entry: dict, idx: int) -> None:
    """Validate a single entry's envelope fields. Calls fail() on any violation."""

    required = {"id", "title", "kind", "flags", "sort_key", "machine_id", "policy_tags", "payload"}
    missing = required - set(entry.keys())
    if missing:
        fail(f"entry[{idx}]: missing required envelope fields: {sorted(missing)}")

    if not isinstance(entry["id"], str) or not (1 <= len(entry["id"]) <= MAX_ID_LEN):
        fail(f"entry[{idx}]: id must be a string of length 1..{MAX_ID_LEN}")
    if not _KEBAB_ID_RE.match(entry["id"]):
        fail(f"entry[{idx}]: id must be kebab-case (lowercase a-z 0-9 dash-separated, no leading/trailing dash), got {entry['id']!r}")
    if not isinstance(entry["title"], str) or not (1 <= len(entry["title"]) <= MAX_TITLE_LEN):
        fail(f"entry[{idx}]: title must be a string of length 1..{MAX_TITLE_LEN}")
    # Same ASCII-clean discipline as sort_key / policy_tags: the
    # bootloader parser caps title against raw JSON content bytes
    # (content_end - content_start), and bootcfg writes with
    # ensure_ascii=True. Non-ASCII or escape-bearing titles would
    # expand under serialization and trip the parser cap at boot
    # despite passing Python len() at host time.
    title = entry["title"]
    if any(ord(c) < 0x20 or ord(c) > 0x7E or c in ('\\', '"') for c in title):
        fail(f"entry[{idx}]: title must be printable ASCII without "
             f"backslash or double-quote, got {title!r}")

    if not isinstance(entry["flags"], list):
        fail(f"entry[{idx}]: flags must be an array")
    for flag in entry["flags"]:
        if not isinstance(flag, str):
            fail(f"entry[{idx}]: flags entry not a string: {flag!r}")
        if flag not in KNOWN_FLAGS:
            warn(f"entry[{idx}] {entry['id']}: unknown flag {flag!r} -- dropped (forward-compat)")

    if "timeout_override" in entry:
        t = entry["timeout_override"]
        if not _is_int(t) or not (0 <= t <= 600):
            fail(f"entry[{idx}]: timeout_override must be int in 0..600, got {t!r}")

    if not isinstance(entry["sort_key"], str):
        fail(f"entry[{idx}]: sort_key must be a string")
    # sort_key cap matches the bootloader parser's check, which
    # measures content_end - content_start (raw JSON content bytes
    # between quotes). For pure ASCII without backslash / double-
    # quote / control chars, Python char count equals raw JSON byte
    # count -- so we enforce ASCII-clean + length here. Non-ASCII or
    # escape-bearing values would expand under JSON serialization
    # (with ensure_ascii=True) and trip the parser's cap at boot
    # despite passing Python len() at host time.
    sk = entry["sort_key"]
    if any(ord(c) < 0x20 or ord(c) > 0x7E or c in ('\\', '"') for c in sk):
        fail(f"entry[{idx}]: sort_key must be printable ASCII without "
             f"backslash or double-quote, got {sk!r}")
    if len(sk) > MAX_SORT_KEY_LEN:
        fail(f"entry[{idx}]: sort_key length {len(sk)} > "
             f"MAX_SORT_KEY_LEN {MAX_SORT_KEY_LEN}")
    if not isinstance(entry["machine_id"], str):
        fail(f"entry[{idx}]: machine_id must be a string, got {entry['machine_id']!r}")
    # Empty machine_id is the explicit "match any machine" wildcard
    # (the boot-policy ladder treats e->machine_id[0]=='\\0' as universal).
    # Non-empty values must be RFC 4122 textual UUID form.
    if entry["machine_id"] != "" and not _UUID_RE.match(entry["machine_id"]):
        fail(f"entry[{idx}]: machine_id must be empty or RFC 4122 UUID format, got {entry['machine_id']!r}")
    if not isinstance(entry["policy_tags"], list):
        fail(f"entry[{idx}]: policy_tags must be an array")
    # policy_tags array + per-tag caps match the parser's fixed-size
    # storage. Same ASCII-clean discipline as sort_key so Python
    # len() equals raw JSON content byte length.
    if len(entry["policy_tags"]) > MAX_POLICY_TAGS:
        fail(f"entry[{idx}]: policy_tags has {len(entry['policy_tags'])} "
             f"items > MAX_POLICY_TAGS {MAX_POLICY_TAGS}")
    for i, tag in enumerate(entry["policy_tags"]):
        if not isinstance(tag, str):
            fail(f"entry[{idx}]: policy_tags[{i}] must be a string, got {type(tag).__name__}")
        if any(ord(c) < 0x20 or ord(c) > 0x7E or c in ('\\', '"') for c in tag):
            fail(f"entry[{idx}]: policy_tags[{i}] must be printable "
                 f"ASCII without backslash or double-quote, got {tag!r}")
        if len(tag) > MAX_POLICY_TAG_LEN:
            fail(f"entry[{idx}]: policy_tags[{i}] length {len(tag)} > "
                 f"MAX_POLICY_TAG_LEN {MAX_POLICY_TAG_LEN}")
    # Optional health_check_subset (per-entry health-gate override). Absent / empty
    # -> kernel runs the default check set. Non-empty restricts the run
    # to the named checks (intersection with the kernel registry).
    if "health_check_subset" in entry:
        subset = entry["health_check_subset"]
        if not isinstance(subset, list):
            fail(f"entry[{idx}]: health_check_subset must be an array")
        if len(subset) > MAX_HEALTH_SUBSET_NAMES:
            fail(f"entry[{idx}]: health_check_subset has {len(subset)} "
                 f"items > MAX_HEALTH_SUBSET_NAMES {MAX_HEALTH_SUBSET_NAMES}")
        for i, name in enumerate(subset):
            if not isinstance(name, str):
                fail(f"entry[{idx}]: health_check_subset[{i}] must be a "
                     f"string, got {type(name).__name__}")
            if len(name) == 0 or len(name) > MAX_HEALTH_SUBSET_NAME_LEN:
                fail(f"entry[{idx}]: health_check_subset[{i}] length "
                     f"{len(name)} out of 1..{MAX_HEALTH_SUBSET_NAME_LEN}")
            if any(ord(c) < 0x20 or ord(c) > 0x7E or c in ('\\', '"')
                   for c in name):
                fail(f"entry[{idx}]: health_check_subset[{i}] must be "
                     f"printable ASCII without backslash or double-quote, "
                     f"got {name!r}")
    if not isinstance(entry["payload"], dict):
        fail(f"entry[{idx}]: payload must be an object")


def validate_kind(entry: dict, idx: int) -> str | None:
    """Return the canonical kind name (string) if known/parseable, else None for skip-with-warn."""

    kind = entry["kind"]
    if isinstance(kind, str):
        if kind in KIND_NAMES:
            return kind
        warn(f"entry[{idx}] {entry['id']}: unknown kind {kind!r} -- skipped (forward-compat)")
        return None
    if _is_int(kind):
        for name, num in KIND_NAMES.items():
            if num == kind:
                return name
        if 0 <= kind <= 99:
            fail(f"entry[{idx}]: kind={kind} in stable range 0..99 but not a known name")
        if VENDOR_KIND_FIRST <= kind <= VENDOR_KIND_LAST or kind >= 200:
            warn(f"entry[{idx}] {entry['id']}: kind={kind} (vendor/reserved) -- skipped")
            return None
        fail(f"entry[{idx}]: kind={kind} out of valid range")
    fail(f"entry[{idx}]: kind must be string or integer, got {type(kind).__name__}")
    return None


def validate_payload(entry: dict, idx: int, kind_name: str) -> None:
    payload = entry["payload"]
    eid = entry["id"]

    required = PER_KIND_REQUIRED[kind_name]
    missing = required - set(payload.keys())
    if missing:
        fail(f"entry[{idx}] {eid} (kind={kind_name}): missing payload fields {sorted(missing)}")

    # Per-kind type / value validation. Path / cmdline / URL fields use _expect_ascii_clean
    # (rejects NUL, control bytes, non-ASCII) since the bootloader will consume them as
    # NUL-terminated C strings. GUID fields use _expect_uuid for RFC 4122 textual form.
    if kind_name == "split":
        _expect_esp_path(idx, eid, kind_name, payload, "kernel", max_len=MAX_PATH_LEN)
        _expect_ascii_clean(idx, eid, kind_name, payload, "cmdline", max_len=4096)
        _expect_split_root(idx, eid, payload)
        _expect_ascii_path_list(idx, eid, kind_name, payload, "initrd", max_items=8, max_len=MAX_PATH_LEN)
    elif kind_name == "uki":
        _expect_esp_path(idx, eid, kind_name, payload, "uki_path", max_len=MAX_PATH_LEN)
        if "uki_path" in payload and isinstance(payload["uki_path"], str):
            if not any(payload["uki_path"].startswith(p) for p in UKI_PATH_PREFIXES):
                fail(
                    f"entry[{idx}] {eid} (kind=uki): uki_path must start with "
                    f"{' or '.join(UKI_PATH_PREFIXES)}"
                )
        _expect_int(idx, eid, kind_name, payload, "profile", lo=0, hi=15)
    elif kind_name == "chainload":
        _expect_ascii_clean(idx, eid, kind_name, payload, "efi_path", max_len=MAX_PATH_LEN)
        _expect_uuid(idx, eid, kind_name, payload, "device_guid")
        flag_set = set(entry["flags"])
        if "trusted_chainload" not in flag_set:
            fail(f"entry[{idx}] {eid}: kind=chainload requires flags include trusted_chainload")
    elif kind_name == "network":
        _expect_ascii_clean(idx, eid, kind_name, payload, "url", max_len=512)
        _expect_enum(idx, eid, kind_name, payload, "uri_scheme", allowed=VALID_NETWORK_SCHEMES)
        _expect_hex(idx, eid, kind_name, payload, "asset_digest", length=64)
        # Cross-validate via urlsplit: scheme must match uri_scheme, hostname must be non-empty,
        # and userinfo (user/password) is rejected outright. Boot URLs are unauthenticated
        # fetch targets; a userinfo-only authority like "https://@/k" has empty hostname but
        # non-empty netloc, which the netloc-only check would have missed.
        if "url" in payload and "uri_scheme" in payload:
            url = payload["url"]
            scheme = payload["uri_scheme"]
            if isinstance(url, str) and isinstance(scheme, str) and scheme in VALID_NETWORK_SCHEMES:
                parsed = urlsplit(url)
                if parsed.scheme != scheme:
                    fail(f"entry[{idx}] {eid} (kind=network): url scheme {parsed.scheme!r} does not match uri_scheme {scheme!r}")
                if not parsed.hostname:
                    fail(f"entry[{idx}] {eid} (kind=network): url has empty host (got {url!r})")
                if parsed.username is not None or parsed.password is not None:
                    fail(f"entry[{idx}] {eid} (kind=network): url must not contain userinfo (got {url!r})")
    elif kind_name == "resume":
        _expect_esp_path(idx, eid, kind_name, payload, "snapshot_path", max_len=MAX_PATH_LEN)
        _expect_hex(idx, eid, kind_name, payload, "snapshot_digest", length=64)
    elif kind_name == "recovery":
        _expect_uuid(idx, eid, kind_name, payload, "recovery_partition_guid")
    elif kind_name == "installer":
        _expect_uuid(idx, eid, kind_name, payload, "installer_image_guid")
        _expect_enum(idx, eid, kind_name, payload, "media_role", allowed=VALID_INSTALLER_ROLES)
    elif kind_name == "safe":
        _expect_enum(idx, eid, kind_name, payload, "safe_mode_subset", allowed=VALID_SAFE_SUBSETS)
        _expect_esp_path(idx, eid, kind_name, payload, "kernel", max_len=MAX_PATH_LEN)
    elif kind_name == "diagnostics":
        _expect_esp_path(idx, eid, kind_name, payload, "kernel", max_len=MAX_PATH_LEN)
        _expect_bool(idx, eid, kind_name, payload, "verbose_log")
    elif kind_name == "test":
        _expect_esp_path(idx, eid, kind_name, payload, "kernel", max_len=MAX_PATH_LEN)
        _expect_enum(idx, eid, kind_name, payload, "test_suite", allowed=VALID_TEST_SUITES)


def _reject_non_finite(node: Any, path: str = "") -> None:
    """Iteratively reject NaN / Infinity / -Infinity floats anywhere under the parsed JSON.

    Defense-in-depth: parse_constant catches the JSON tokens NaN / Infinity directly, but a value
    like 1e500 parses to float('inf') without firing parse_constant. This walks the tree and
    rejects any non-finite float so canonical_entries_bytes never sees one (it would also fail
    via allow_nan=False, but failing earlier produces a clearer error site).

    Iterative stack walk -- a recursive form would hit Python's default recursion limit on a
    deeply-nested-but-small adversarial JSON (e.g. 1000 nested arrays in <16 KiB)."""
    stack: list[tuple[Any, str]] = [(node, path)]
    while stack:
        cur, cur_path = stack.pop()
        if isinstance(cur, float):
            if math.isnan(cur) or math.isinf(cur):
                fail(f"non-finite float at {cur_path or '<root>'} (NaN/Infinity not allowed)")
        elif isinstance(cur, dict):
            for k, v in cur.items():
                stack.append((v, f"{cur_path}.{k}" if cur_path else str(k)))
        elif isinstance(cur, list):
            for i, v in enumerate(cur):
                stack.append((v, f"{cur_path}[{i}]"))


# ---- Firmware value-depth budget (context-relative) ----
#
# The firmware parser walks the root object and each entry object STRUCTURALLY,
# but hands any value it does not interpret to a bounded skipper
# (skip_value_post_token -> skip_value_depth with BOOT_ENTRIES_MAX_PARSE_DEPTH).
# That budget counts containers and RESTARTS at each such value, so the limit is
# context-relative: 8 levels measured from the skipped value itself, not 8 levels
# from the root. Values the firmware interprets itself (flags, policy_tags,
# health_check_subset) are shape-checked elsewhere and never reach the skipper.
#
# Without this check the host accepted stores the firmware rejects -- a nine-deep
# array under an unknown extension key validated here and then triggered
# invalid-store fallback at boot, which is exactly the divergence the unique-key
# work set out to remove.
MAX_PARSE_DEPTH = 8         # BOOT_ENTRIES_MAX_PARSE_DEPTH

# The CRC-header locator (_skip_value above) skips a ROOT value, which is two
# container levels further out than the authoritative walk ever skips from: the
# firmware descends the `entries` array and each entry object structurally and only
# hands `payload` to the skipper. Charging the same 8 would refuse to locate the
# header in stores the parser ACCEPTS, so the locator gets the firmware's
# BOOT_ENTRIES_MAX_SCAN_DEPTH, not its parse budget.
MAX_SCAN_DEPTH = MAX_PARSE_DEPTH + 2    # BOOT_ENTRIES_MAX_SCAN_DEPTH

_ENTRY_STRUCTURAL_KEYS = {"id", "title", "kind", "flags", "sort_key",
                          "machine_id", "policy_tags", "timeout_override",
                          "health_check_subset"}


def _container_depth(value: Any) -> int:
    """Max container nesting of `value`, counting the value itself. Scalars are 0.

    Iterative for the same reason _reject_non_finite is: a recursive form dies on
    a deeply-nested-but-small adversarial store before it can report anything."""
    best = 0
    stack: list[tuple[Any, int]] = [(value, 1)]
    while stack:
        cur, d = stack.pop()
        if isinstance(cur, (dict, list)):
            if d > best:
                best = d
            children = cur.values() if isinstance(cur, dict) else cur
            for child in children:
                if isinstance(child, (dict, list)):
                    stack.append((child, d + 1))
    return best


def _check_skipped_value_depth(value: Any, where: str) -> None:
    depth = _container_depth(value)
    if depth > MAX_PARSE_DEPTH:
        fail(f"{where}: value nests {depth} containers, over the firmware "
             f"parser's {MAX_PARSE_DEPTH}-level budget for a skipped value")


def validate_value_depths(data: dict) -> None:
    """Apply the firmware's per-value depth budget everywhere the firmware skips."""
    for key, value in data.items():
        if key in ("schema_version", "crc32", "entries"):
            continue
        _check_skipped_value_depth(value, f"top-level key {key!r}")
    entries = data.get("entries")
    if not isinstance(entries, list):
        return
    for idx, entry in enumerate(entries):
        if not isinstance(entry, dict):
            continue
        for key, value in entry.items():
            if key in _ENTRY_STRUCTURAL_KEYS:
                continue
            # `payload` and any unknown extension key are both skipped wholesale.
            _check_skipped_value_depth(value, f"entry[{idx}] key {key!r}")


def atomic_write_bytes(path: Path, raw: bytes) -> None:
    """Write-new + fsync + atomic replace. Raises OSError; callers map it to
    their own exit-code contract.

    The temporary file is created with tempfile.mkstemp, so this process
    exclusively OWNS it. A fixed `<path>.tmp` would not be owned: an existing
    file there is silently overwritten, a symlink pointed at the destination
    defeats the whole failure-preservation guarantee, two concurrent runs share
    one inode (A can fsync and replace while B is still writing the same file),
    and the cleanup path would unlink something it did not create. os.replace
    makes the RENAME atomic; it says nothing about who else can write the
    source. Cleanup catches BaseException so an interrupt cannot strand a temp.

    Both directory fsyncs are part of the FAT32 ESP durability protocol the
    boot-counter rename uses: without them a host crash can leave the directory
    entry missing after the data is on disk. They are skipped on error because
    fsync of a directory fd is undefined on Windows, where os.replace's native
    MoveFileEx is itself atomic.
    """
    parent = path.parent
    if not parent.exists():
        parent.mkdir(parents=True, exist_ok=True)
    fd, tmp_name = tempfile.mkstemp(dir=parent, prefix=path.name + ".", suffix=".new")
    tmp_path = Path(tmp_name)
    try:
        with os.fdopen(fd, "wb") as fh:
            fh.write(raw)
            fh.flush()
            os.fsync(fh.fileno())
        _fsync_dir(parent)
        os.replace(tmp_path, path)
        _fsync_dir(parent)
    except BaseException:
        try:
            tmp_path.unlink()
        except OSError:
            pass
        raise


# Directory fsync is undefined on Windows (os.open of a directory raises
# EACCES/EPERM there) and unsupported on some filesystems. Those are PLATFORM
# LIMITATIONS and are skipped. A genuine I/O failure -- EIO, ENOSPC -- is NOT:
# swallowing it reported a durable write while the directory entry may never
# have reached the disk, which is precisely the "old-or-new, never torn"
# guarantee this protocol exists to make. Suppress the limitation, propagate
# the failure.
_DIR_FSYNC_UNSUPPORTED = frozenset({
    errno.EINVAL, errno.ENOSYS,
    getattr(errno, "ENOTSUP", errno.EOPNOTSUPP), errno.EOPNOTSUPP,
})

# Windows has no directory handle to fsync: os.open(dir, O_RDONLY) fails with a
# permission error there for every directory, so on THAT platform EACCES/EPERM
# means "unsupported". On POSIX it means what it says -- a parent granting write
# and search but not read is a real configuration, and suppressing it there
# skipped both flushes and returned success with rename durability unestablished.
# Platform limitation is a property of the platform, not of the errno.
_WINDOWS_DIR_OPEN_LIMITATION = frozenset({errno.EACCES, errno.EPERM})


def _fsync_dir(parent: Path) -> None:
    try:
        dir_fd = os.open(parent, os.O_RDONLY)
    except OSError as e:
        if os.name == "nt" and e.errno in _WINDOWS_DIR_OPEN_LIMITATION:
            return
        if e.errno in _DIR_FSYNC_UNSUPPORTED:
            return
        raise
    try:
        os.fsync(dir_fd)
    except OSError as e:
        if e.errno not in _DIR_FSYNC_UNSUPPORTED:
            raise
    finally:
        os.close(dir_fd)


def validate_store(data: dict, raw: bytes, *, recompute_crc: bool) -> int:
    """Validate the parsed store. Returns the computed CRC. Calls fail() on any violation.

    `raw` is the file bytes (used for CRC verification). When recompute_crc=True, do not check
    crc32 equality -- caller is in --emit-crc mode."""

    if not isinstance(data, dict):
        fail("top-level value must be a JSON object")

    required = {"schema_version", "crc32", "entries"}
    missing = required - set(data.keys())
    if missing:
        fail(f"missing top-level fields: {sorted(missing)}")

    _reject_non_finite(data, "<store>")
    validate_value_depths(data)

    sv = data["schema_version"]
    if not _is_int(sv) or sv != SCHEMA_VERSION:
        fail(f"schema_version must be {SCHEMA_VERSION} (got {sv!r})")

    crc_text = data["crc32"]
    if not isinstance(crc_text, str) or len(crc_text) != 10 or not crc_text.startswith("0x"):
        fail(f'crc32 must be "0x" + 8 hex digits (got {crc_text!r})')
    if not all(c in "0123456789abcdefABCDEF" for c in crc_text[2:]):
        fail(f"crc32 must be hex digits only (got {crc_text!r})")

    entries = data["entries"]
    if not isinstance(entries, list):
        fail("entries must be an array")
    if not (1 <= len(entries) <= MAX_ENTRIES):
        fail(f"entries length must be 1..{MAX_ENTRIES} (got {len(entries)})")

    if len(raw) > MAX_TOTAL_BYTES:
        fail(f"file size {len(raw)} > MAX_TOTAL_BYTES {MAX_TOTAL_BYTES}")

    seen_ids: set[str] = set()
    for idx, entry in enumerate(entries):
        if not isinstance(entry, dict):
            fail(f"entry[{idx}] must be an object")
        validate_envelope(entry, idx)
        if entry["id"] in seen_ids:
            fail(f"entry[{idx}]: duplicate id {entry['id']!r}")
        seen_ids.add(entry["id"])
        kind_name = validate_kind(entry, idx)
        if kind_name is None:
            continue  # skip-with-warn already logged
        validate_payload(entry, idx, kind_name)

    computed = compute_crc_from_file(raw)
    if not recompute_crc:
        _, expected = find_crc_field(raw)
        if computed != expected:
            fail(
                f"CRC mismatch: header=0x{expected:08X} computed=0x{computed:08X} "
                f"(file bytes differ from what produced the header CRC)"
            )

    return computed


def main(argv: list[str]) -> int:
    emit_crc = False
    args = list(argv[1:])
    if args and args[0] == "--emit-crc":
        emit_crc = True
        args.pop(0)

    if len(args) != 1:
        print("usage: validate.py [--emit-crc] <bootentries.json>", file=sys.stderr)
        return 2

    path = Path(args[0])
    if not path.is_file():
        print(f"[FAIL] file not found: {path}", file=sys.stderr)
        return 2

    # Size cap is enforced BEFORE read so a hostile / malformed multi-megabyte file does not
    # consume host memory before the validator gets a chance to reject it. The canonical
    # MAX_TOTAL_BYTES bound applies to the on-disk size; raw_size is rechecked in validate_store
    # against the same constant for the post-decode path (defense in depth).
    file_size = path.stat().st_size
    if file_size > MAX_TOTAL_BYTES:
        print(
            f"[FAIL] file size {file_size} > MAX_TOTAL_BYTES {MAX_TOTAL_BYTES} (rejected pre-read)",
            file=sys.stderr,
        )
        return 1

    raw = path.read_bytes()
    try:
        data = strict_loads(raw.decode("utf-8"))
    except json.JSONDecodeError as e:
        print(f"[FAIL] JSON parse error at line {e.lineno} col {e.colno}: {e.msg}", file=sys.stderr)
        return 1
    except UnicodeDecodeError as e:
        print(f"[FAIL] file is not valid UTF-8: {e}", file=sys.stderr)
        return 1
    except RecursionError:
        print(f"[FAIL] JSON nesting exceeds parser depth limit (DoS shape rejected)", file=sys.stderr)
        return 1

    if emit_crc:
        # --emit-crc mode: write the file with `"0x00000000"` placeholder so the CRC has a
        # stable basis, then patch the 8 hex digits in place with the real CRC. The on-disk
        # bytes used for verification are the SAME bytes the producer just wrote, so the
        # placeholder + patch flow guarantees the CRC matches.
        # EVERYTHING happens in memory; the destination is touched exactly once,
        # atomically, and only after the bytes have passed every check.
        #
        # This used to write the placeholder straight to `path` and validate
        # afterwards, so any failure destroyed the store it was asked to stamp.
        # Two ways that fired: a rejected store left the destination holding the
        # invalid placeholder (CRC 0x00000000), and -- once ensure_ascii=False
        # let a lone surrogate such as "\ud800" survive to the encoder --
        # write_text() truncated the file before UnicodeEncodeError was raised,
        # leaving ZERO bytes where the boot store had been. A store the store
        # editor is asked to fix must never come back emptier than it went in.
        data["crc32"] = "0x00000000"
        # ensure_ascii=False: the default would re-spell a literal non-ASCII key
        # as an ESCAPED one, which the firmware hard-rejects and strict_loads
        # refuses to reload -- this writer would emit a store nothing can read.
        # Same defect as bootcfg's _canonical_dumps; both writers must agree.
        placeholder_text = json.dumps(data, indent=2, ensure_ascii=False) + "\n"
        try:
            raw_with_placeholder = placeholder_text.encode("utf-8")
        except UnicodeEncodeError as e:
            fail(f"store contains text that is not encodable as UTF-8 ({e}); "
                 f"{path} left unchanged")
        # Validate the BYTES about to be persisted, not just the dict: the key
        # rules live in the serialized form, so validate_store() alone cannot
        # see them.
        strict_loads(raw_with_placeholder.decode("utf-8"))
        crc = validate_store(data, raw_with_placeholder, recompute_crc=True)
        offset, _ = find_crc_field(raw_with_placeholder)
        final = raw_with_placeholder[:offset] + f"{crc:08X}".encode("ascii") + raw_with_placeholder[offset + 8:]
        try:
            atomic_write_bytes(path, final)
        except OSError as e:
            fail(f"{path}: write failed: {e}")
        print(f"[OK] {path}: crc32 -> 0x{crc:08X}")
        return 0

    crc = validate_store(data, raw, recompute_crc=False)
    print(f"[OK] {path}: {len(data['entries'])} entries, crc32=0x{crc:08X}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
