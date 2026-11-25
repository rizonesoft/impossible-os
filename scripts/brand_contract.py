#!/usr/bin/env python3
"""The icon and cursor contract Impossible Brand draws against (docs/design/icons.md#how-are-icons-rendered).

    python3 scripts/brand_contract.py                 write docs/design/brand-contract.json
    python3 scripts/brand_contract.py --check         fail when the committed contract is stale
    python3 scripts/brand_contract.py --check --staged    the same, judged on the git index
    python3 scripts/brand_contract.py --sizes         print the packed icon sizes, one per line
    python3 scripts/brand_contract.py --validate-sources [--literal-colours] FILE...

Everything published is read from the OS sources that enforce it: the icon names,
providers and kernel table, the packer's lists and limits, the IRES loader's bounds,
the cursor loader's selection rule and buffers, the image writer's name limit, the
theme tokens and the system SVG profile. A consumer of the art (Impossible Brand, the
host converter, the asset validator, the installers) validates against this file, so
none of them keeps its own copy of a list or a limit.
"""
import argparse
import ast
import hashlib
import json
import os
import re
import struct
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CONTRACT = "docs/design/brand-contract.json"
SCHEMA = 1
INT32_MAX = 2**31 - 1
# Behaviour the published rules describe is pinned, data is parsed. Each function here is read
# as a whole (comments stripped, whitespace collapsed) and compared with the digest it had when
# the rules below were derived from it; any edit refuses generation until the rules are
# re-derived and the digest updated with them. Data (name tables, enums, size and file lists,
# #define values) is read with strict grammars instead, so adding an icon never touches a pin.
BEHAVIOUR_PINS = {
    ("src/kernel/drivers/cursor.c", "xcur_parse"): "cfe00b0d31f9e4ca2b99f6a17b49bac7c0682140f3ab9a5ff5910198f151fce9",   # frame selection, the per-frame checks (Q65)
    ("src/kernel/drivers/cursor.c", "cursor_init"): "11bce278a1db3e1c469d44bdc12a69a3a24a37658d9bea23a1b8ed96aee7c34d",  # slot size, pool allocation, the slot passed to the parser
    ("src/kernel/icon_store.c", "ires_load"): "aeca6ddbe9852cccf9e948060b6061dbd3b9d49ad9462bd28aea5651847b25d9",        # the archive byte bound and the size-count clamp
    ("tools/irespack.c", "main"): "d18117748c455d25914f1af253755a573af8d7ddc056914c8b02dcb026d22138",                    # which sizes the packer reads and writes, names, layout
    ("src/kernel/drivers/cursor.c", "xcur_load"): "7d382275b5cca648893d10dd256d20a1e5a754779ddb57dd2e8bfbd71e0c2247",    # how many bytes reach the parser (read_prefix_bytes)
    ("tools/irespack.c", "is_known_icon"): "f417d46570fc43a5429f88eed1d1fbe55e06e13f3d2082f3db5d7cfaa94ca6ec",           # packer membership is accepted_icons[] up to its NULL
    ("src/kernel/icon_store.c", "icon_get_by_name"): "91cb599d74b12df328681e5539f2936fe8c96dd77308ec8ec8a83ecdc000d76a",  # lookup precedence (the published `lookup`)
    ("tools/irespack.c", "find_size_index"): "1c1ba358c50aa326436f80b809ca1e47cde89e1fcde5bc68326beb7d5a2ef7eb",         # which directory sizes the packer accepts
}

INPUTS = {
    "icon_header": "include/icon_store.h",
    "icon_store": "src/kernel/icon_store.c",
    "codepoints": "include/generated/fluent_codepoints.h",
    "irespack": "tools/irespack.c",
    "cursor_header": "include/cursor.h",
    "cursor_driver": "src/kernel/drivers/cursor.c",
    "makefile": "Makefile",
    "mkfs": "tools/mkfs-ixfs.c",
    "tokens": "docs/design/tokens.json",
    "profile": "docs/design/svg-profile.json",
    "icons_md": "docs/design/icons.md",
}
ICON_SRC = "resources/icons/src"
LITERAL_ROOTS = ("src", "user")


class ContractError(Exception):
    """A source the contract is read from is missing, unparsable or breaks a published limit."""


# ---------------------------------------------------------------- reading the tree

class Tree:
    """The files a contract is generated from: the working tree, or the git index (--staged)."""

    def __init__(self, root: Path, staged: bool = False):
        self.root, self.staged = Path(root), staged
        self._cache = {}

    def _git(self, *args: str) -> bytes:
        try:
            return subprocess.run(["git", "-C", str(self.root), *args], check=True, capture_output=True,
                                  timeout=120).stdout
        except subprocess.CalledProcessError as error:
            raise ContractError(f"git {' '.join(args[:2])}: {error.stderr.decode(errors='replace').strip()}") from None

    def exists(self, rel: str) -> bool:
        if self.staged:
            return bool(self._git("ls-files", "--cached", "--", rel).strip())
        return (self.root / rel).exists()

    def read_bytes(self, rel: str) -> bytes:
        if rel in self._cache:
            return self._cache[rel]
        if self.staged:
            proc = subprocess.run(["git", "-C", str(self.root), "show", f":{rel}"], capture_output=True, timeout=120)
            if proc.returncode:
                raise ContractError(f"{rel}: missing from the index")
            self._cache[rel] = proc.stdout
            return proc.stdout
        path = self.root / rel
        if not path.is_file():
            raise ContractError(f"{rel}: missing")
        return path.read_bytes()

    def read_text(self, rel: str) -> str:
        """Text with CRLF and CR normalised to LF, so a core.autocrlf=true checkout reads like the index."""
        try:
            return self.read_bytes(rel).decode("utf-8").replace("\r\n", "\n").replace("\r", "\n")
        except UnicodeDecodeError as error:
            raise ContractError(f"{rel}: not UTF-8 ({error.reason})") from None

    def read_json(self, rel: str):
        try:
            return json.loads(self.read_text(rel))
        except json.JSONDecodeError as error:
            raise ContractError(f"{rel}: not valid JSON ({error.msg} at line {error.lineno})") from None

    def files_containing(self, needle: str, rel_dirs: tuple, suffixes: tuple) -> list:
        """Files under rel_dirs with one of the suffixes whose text contains needle (a fixed string).

        git grep does the search in one process (the index under --staged, tracked and
        untracked files otherwise); a tree outside git is walked instead."""
        args = ["grep", "-l", "-F", "-z", "-e", needle]
        args += ["--cached"] if self.staged else ["--untracked"]
        try:
            proc = subprocess.run(["git", "-C", str(self.root), *args, "--", *rel_dirs], capture_output=True, timeout=120)
        except (OSError, subprocess.TimeoutExpired) as error:
            raise ContractError(f"git grep: {error}") from None
        if proc.returncode in (0, 1) and not proc.stderr:
            found = [p for p in proc.stdout.decode().split("\0") if p and p.endswith(suffixes)]
            return sorted(found)
        if self.staged:
            raise ContractError(f"git grep: {proc.stderr.decode(errors='replace').strip()}")
        return [rel for d in rel_dirs for rel in self.files(d, suffixes) if needle in self.read_text(rel)]

    def files(self, rel_dir: str, suffixes: tuple) -> list:
        """Every file under rel_dir (recursively) with one of the suffixes, as sorted relative paths."""
        if self.staged:
            out = self._git("ls-files", "--cached", "-z", "--", rel_dir).decode().split("\0")
            return sorted(p for p in out if p and p.endswith(suffixes))
        base = self.root / rel_dir
        if not base.is_dir():
            return []
        found = []
        for dirpath, dirnames, filenames in os.walk(base):
            dirnames[:] = sorted(d for d in dirnames if not (Path(dirpath) / d).is_symlink())
            found += [str((Path(dirpath) / f).relative_to(self.root)) for f in filenames if f.endswith(suffixes)]
        return sorted(found)


# ---------------------------------------------------------------- C source helpers

def strip_c_comments(text: str) -> str:
    """Remove /* */ and // comments, keeping string literals intact. A line continuation is refused:
    C splices it before comments are removed, which this reader does not model."""
    if "\\\n" in text:
        raise ContractError("a backslash line continuation in a C source the contract reads")
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c == '"':
            j = i + 1
            while j < n and text[j] != '"':
                j += 2 if text[j] == "\\" else 1
            out.append(text[i:j + 1])
            i = j + 1
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
            out.append(" ")
        elif text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def c_entries(block: str) -> list:
    """The comma-separated entries of an initializer, split outside string literals, stripped, empties dropped."""
    entries, cur, i, n = [], [], 0, len(block)
    while i < n:
        c = block[i]
        if c == '"':
            j = i + 1
            while j < n and block[j] != '"':
                j += 2 if block[j] == "\\" else 1
            cur.append(block[i:j + 1])
            i = j + 1
            continue
        if c == ",":
            entries.append("".join(cur).strip())
            cur = []
        else:
            cur.append(c)
        i += 1
    entries.append("".join(cur).strip())
    return [e for e in entries if e]


SINGLE_LITERAL = re.compile(r'"(?:[^"\\]|\\.)*"')


def literal_entries(block: str, what: str) -> list:
    """Every entry must be exactly one string literal: two adjacent literals (a missing comma) are
    concatenated by C, shifting every later entry, so they are refused rather than read."""
    out = []
    for entry in c_entries(block):
        if not SINGLE_LITERAL.fullmatch(entry):
            raise ContractError(f"{what}: entry {entry[:40]!r} is not exactly one string literal (a missing comma?)")
        out.append(decode_literal(entry, what))
    return out



def initializer(text: str, name: str, rel: str) -> str:
    """The body of `... name[...] = { ... };`."""
    m = re.search(r"\b" + re.escape(name) + r"\s*\[[^\]]*\]\s*=\s*\{(.*?)\};", text, re.S)
    if not m:
        raise ContractError(f"{rel}: cannot find the initializer of {name}[]")
    return m.group(1)


def array_size(text: str, decl: str, rel: str) -> str:
    m = re.search(re.escape(decl) + r"\s*\[([^\]]+)\]", text)
    if not m:
        raise ContractError(f"{rel}: cannot find {decl}[...]")
    return m.group(1).strip()


_INT_OPS = {ast.Add: lambda a, b: a + b, ast.Mult: lambda a, b: a * b}


def int_expr(expr: str, where: str) -> int:
    """A C integer constant expression of literals and + - * / only (as #defines and array bounds use)."""
    expr = expr.strip()
    if re.search(r"\b0[0-9]", expr):
        raise ContractError(f"{where}: {expr!r} has a leading-zero (octal) literal")

    def ev(node):
        if isinstance(node, ast.Expression):
            return ev(node.body)
        if isinstance(node, ast.Constant) and type(node.value) is int:
            return node.value
        if isinstance(node, ast.BinOp) and type(node.op) in _INT_OPS:
            value = _INT_OPS[type(node.op)](ev(node.left), ev(node.right))
            if value > 2**31 - 1:
                raise ValueError("intermediate beyond 2^31-1, where C int widths differ")
            return value
        raise ValueError(type(node).__name__)
    try:
        return ev(ast.parse(expr, mode="eval"))
    except (SyntaxError, ValueError):
        raise ContractError(f"{where}: {expr!r} is not a plain integer expression") from None


# Every array the contract reads must be declared exactly like this (whitespace aside): an explicit
# bound, a different element type or a second declaration would change what C compiles from what
# the reader parses, so any other form is refused.
DECLARATIONS = {
    ("src/kernel/icon_store.c", "icon_names"): "static const char *icon_names[ICON_TOTAL_COUNT] = {",
    ("include/generated/fluent_codepoints.h", "icon_codepoints"): "static const uint32_t icon_codepoints[ICON_MONO_COUNT] = {",
    ("tools/irespack.c", "accepted_icons"): "static const char *accepted_icons[] = {",
    ("tools/irespack.c", "supported_sizes"): "static const uint16_t supported_sizes[] = {",
    ("src/kernel/drivers/cursor.c", "cursor_filenames"): "static const char *cursor_filenames[CURSOR_COUNT] = {",
}
# The IRES wire structs the packer writes and the loader reads: identical on both sides, and as
# the full-pack budget in generate() was derived from them.
LAYOUT_FILES = ("tools/irespack.c", "src/kernel/icon_store.c")
LAYOUT_PINS = {
    "ires_header_t": "6b11312bf07df9733396fd969330fc1a1d55fdba1c3bb2e92cf575df9e8d8c5d",
    "ires_size_entry_t": "ef4eb8fdf4b98f6ec1ad131f7f4e527760977a945682c521045081864ed21307",
    "ires_index_entry_t": "1155e17d3d8dc52204f3c4970dcf73bbcdebc26bd4210e73c3aaefb9ecc0341d",
}
# Structs on one side whose array extents bound a published limit (sizes per icon, icons per pack).
EXTENT_PINS = {
    ("src/kernel/icon_store.c", "ires_icon_size_t"): "cc3961699ef0c96c3fa3e201d8efe8e7eed309dd33bac5607a990811b7f35382",
    ("src/kernel/icon_store.c", "ires_icon_t"): "c88e8853f76f61cc937545cfab029aafe83c5cd4ed7d600c98490e798fe65e9e",
    ("tools/irespack.c", "icon_data_t"): "8cd9fe04574fd702227e5c7cc26832cad35e9332b96b75304e9d8161fbee0831",
}
# Loader tables sized by a published limit: exactly this declaration, once.
EXTENT_LINES = {
    "src/kernel/icon_store.c": ("static uint16_t ires_sizes[IRES_MAX_SIZES];", "static ires_icon_t ires_icons[IRES_COLOR_COUNT];"),
}
# Make variables the contract reads: exactly one plain `:=` assignment each, nothing appended.
MAKE_VARIABLES = ("CURSOR_SRCS", "CURSOR_GUEST_DIR")


def check_declaration(text: str, rel: str, name: str) -> None:
    want = " ".join(DECLARATIONS[(rel, name)].split())
    decls = re.findall(r"^[^\n;{}]*\b" + re.escape(name) + r"\s*\[[^\]]*\]\s*=\s*\{", text, re.M)
    if len(decls) != 1 or " ".join(decls[0].split()) != want:
        raise ContractError(f"{rel}: {name}[] must be declared exactly once as `{want}` (found {decls})")
    if in_conditional(text, text.index(decls[0])):
        raise ContractError(f"{rel}: {name}[] sits inside a conditional block")


def struct_text(text: str, name: str, rel: str) -> str:
    end = re.search(r"\}\s*" + re.escape(name) + r"\s*;", text)
    start = text.rfind("typedef struct", 0, end.start()) if end else -1
    if not end or start < 0:
        raise ContractError(f"{rel}: cannot find the struct {name}")
    return " ".join(text[start:end.end()].split())


def check_layouts(tree: "Tree") -> None:
    texts = {rel: strip_c_comments(tree.read_text(rel)) for rel in LAYOUT_FILES}
    for name, want in sorted(LAYOUT_PINS.items()):
        sides = {rel: struct_text(texts[rel], name, rel) for rel in LAYOUT_FILES}
        if len(set(sides.values())) != 1:
            raise ContractError(f"{' and '.join(LAYOUT_FILES)} define {name} differently: the loader would misread the archive")
        have = hashlib.sha256(next(iter(sides.values())).encode()).hexdigest()
        if have != want:
            raise ContractError(f"{name} changed; re-derive the full-pack budget in scripts/brand_contract.py "
                                f"and update its LAYOUT_PINS digest to {have}")


def check_extents(tree: "Tree") -> None:
    for (rel, name), want in sorted(EXTENT_PINS.items()):
        have = hashlib.sha256(struct_text(strip_c_comments(tree.read_text(rel)), name, rel).encode()).hexdigest()
        if have != want:
            raise ContractError(f"{rel}: {name} changed; re-derive the published limits and update its EXTENT_PINS digest to {have}")
    for rel, lines in EXTENT_LINES.items():
        text = strip_c_comments(tree.read_text(rel))
        normal = [" ".join(line.split()) for line in text.splitlines()]
        for line in lines:
            ident = re.search(r"(\w+)\[", line).group(1)
            declared = [n for n in normal if re.search(r"\b" + ident + r"\s*\[", n) and n.startswith("static")]
            if declared != [line]:
                raise ContractError(f"{rel}: {ident} must be declared exactly once as `{line}` (found {declared})")


def check_make_variables(make: str) -> None:
    for var in MAKE_VARIABLES:
        hits = list(re.finditer(r"^(?!\t).*\b" + re.escape(var) + r"\s*[:+?!]*=", make, re.M))
        found = [m.group(0) for m in hits]
        depth = 0
        for line in make[:hits[0].start()].splitlines() if hits else []:
            word = line.split(None, 1)[0] if line.strip() and not line.startswith("\t") else ""
            depth += 1 if word in ("ifeq", "ifneq", "ifdef", "ifndef") else -1 if word == "endif" else 0
        if depth:
            raise ContractError(f"Makefile: {var} is assigned inside a Make conditional")
        if len(found) != 1 or not re.fullmatch(re.escape(var) + r"\s*:=", found[0]):
            raise ContractError(f"Makefile: {var} must be assigned exactly once with `:=` (found {found or 'none'})")


def function_body(text: str, name: str, rel: str) -> str:
    """The definition of function `name` (signature through its closing brace) in comment-stripped C."""
    # A definition starts its line with its return type (a call such as `if (name(...) == 0) {` does not).
    found = [m for m in re.finditer(r"^[ \t]*(?:(?:static|inline|const|unsigned|signed|struct)\s+)*[A-Za-z_]\w*[\s*]+"
                                    + re.escape(name) + r"\s*\([^;{}]*\)\s*\{", text, re.M)]
    if len(found) != 1:
        raise ContractError(f"{rel}: expected one definition of {name}(), found {len(found)}")
    start, i, depth = found[0].start(), found[0].end() - 1, 0
    line_start = text.rfind("\n", 0, start) + 1
    while i < len(text):
        c = text[i]
        if c in "\"'":
            j = i + 1
            while j < len(text) and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            i = j + 1
            continue
        depth += {"{": 1, "}": -1}.get(c, 0)
        if depth == 0:
            return text[line_start:i + 1]
        i += 1
    raise ContractError(f"{rel}: {name}() has no closing brace")


def check_pins(tree: "Tree") -> dict:
    """{(file, function): body} after every behaviour pin matched."""
    bodies = {}
    for (rel, name), want in sorted(BEHAVIOUR_PINS.items()):
        text = strip_c_comments(tree.read_text(rel))
        body = function_body(text, name, rel)
        if in_conditional(text, text.index(body)):
            raise ContractError(f"{rel}: {name}() sits inside a conditional block")
        have = hashlib.sha256(" ".join(body.split()).encode()).hexdigest()
        if have != want:
            raise ContractError(f"{rel}: {name}() changed; re-derive what the contract publishes from it "
                                f"(scripts/brand_contract.py) and update its BEHAVIOUR_PINS digest to {have}")
        bodies[(rel, name)] = body
    return bodies


def in_conditional(text: str, pos: int) -> bool:
    depth = 0
    for m in re.finditer(r"^[ \t]*#[ \t]*(ifdef|ifndef|if|endif)\b", text[:pos], re.M):
        depth += -1 if m.group(1) == "endif" else 1
    return depth > 0


def decode_literal(literal: str, where: str) -> str:
    """A C string literal whose only escape is a doubled backslash; any other escape is refused
    so the published spelling is the one the compiled code uses."""
    body, out, i = literal[1:-1], [], 0
    while i < len(body):
        if body[i] == "\\":
            if body[i + 1:i + 2] != "\\":
                raise ContractError(f"{where}: escape {body[i:i + 2]!r} in {literal[:40]!r}; only \\\\ is allowed")
            out.append("\\")
            i += 2
            continue
        out.append(body[i])
        i += 1
    return "".join(out)


def first_gap(slots) -> int:
    """The smallest non-negative integer missing from a set of unique non-negative slots (no range built)."""
    for expected, value in enumerate(sorted(slots)):
        if value != expected:
            return expected
    return len(slots)


def define(text: str, name: str, rel: str) -> int:
    found = list(re.finditer(r"^[ \t]*#[ \t]*define[ \t]+" + re.escape(name) + r"\b(.*)$", text, re.M))
    if len(found) != 1 or re.search(r"^[ \t]*#[ \t]*undef[ \t]+" + re.escape(name) + r"\b", text, re.M):
        raise ContractError(f"{rel}: {name} must be #defined exactly once and never #undef'd (found {len(found)})")
    if in_conditional(text, found[0].start()):
        raise ContractError(f"{rel}: #define {name} sits inside a conditional block; the contract cannot know its value")
    return int_expr(macro_value(found[0].group(1), f"{rel}: {name}"), f"{rel}: {name}")


def macro_value(text: str, where: str) -> str:
    value = text.strip()
    whole = value.startswith("(") and value.endswith(")") and _balanced(value[1:-1])
    if not (re.fullmatch(r"0[xX][0-9a-fA-F]+|[0-9]+", value) or whole):
        raise ContractError(f"{where}: {value!r} must be one integer literal or one fully parenthesised expression")
    return value


def _balanced(text: str) -> bool:
    depth = 0
    for c in text:
        depth += {"(": 1, ")": -1}.get(c, 0)
        if depth < 0:
            return False
    return depth == 0


def checked_macro_text(text: str, name: str, rel: str) -> str:
    """The replacement text of a macro defined exactly once, unconditionally, never undefined."""
    found = list(re.finditer(r"^[ \t]*#[ \t]*define[ \t]+" + re.escape(name) + r"\b(.*)$", text, re.M))
    if len(found) != 1 or re.search(r"^[ \t]*#[ \t]*undef[ \t]+" + re.escape(name) + r"\b", text, re.M):
        raise ContractError(f"{rel}: {name} must be #defined exactly once and never #undef'd (found {len(found)})")
    if in_conditional(text, found[0].start()):
        raise ContractError(f"{rel}: #define {name} sits inside a conditional block")
    return found[0].group(1).strip()


def parse_enum(text: str, type_name: str, rel: str) -> list:
    """[(member, value)] of `typedef enum { ... } type_name;`, implicit increments and member aliases resolved."""
    end = re.search(r"\}\s*" + re.escape(type_name) + r"\s*;", text)
    if end and in_conditional(text, end.start()):
        raise ContractError(f"{rel}: the enum {type_name} sits inside a conditional block")
    start = text.rfind("typedef", 0, end.start()) if end else -1
    brace = text.find("{", start) if start >= 0 else -1
    if not end or start < 0 or not re.fullmatch(r"typedef\s+enum\s*", text[start:brace]):
        raise ContractError(f"{rel}: cannot find the enum {type_name}")
    members, values, nxt = [], {}, 0
    for part in text[brace + 1:end.start()].split(","):
        part = part.strip()
        if not part:
            continue
        name, _, value = (s.strip() for s in part.partition("="))
        if not re.fullmatch(r"[A-Za-z_]\w*", name):
            raise ContractError(f"{rel}: {type_name}: cannot read member {part!r}")
        if value:
            nxt = values[value] if value in values else int_expr(value, f"{rel}: {name}")
        values[name] = nxt
        members.append((name, nxt))
        nxt += 1
    return members


# ---------------------------------------------------------------- the generator

def _icon_inputs(tree: Tree, bodies: dict) -> dict:
    header = strip_c_comments(tree.read_text(INPUTS["icon_header"]))
    store_raw = tree.read_text(INPUTS["icon_store"])
    store = strip_c_comments(store_raw)
    enum = dict(parse_enum(header, "system_icon_t", INPUTS["icon_header"]))
    for key in ("ICON_MONO_COUNT", "ICON_COLOR_COUNT", "ICON_TOTAL_COUNT"):
        if key not in enum:
            raise ContractError(f"{INPUTS['icon_header']}: system_icon_t has no {key}")
    mono, total = enum["ICON_MONO_COUNT"], enum["ICON_TOTAL_COUNT"]
    slot_member = {}
    for member, value in parse_enum(header, "system_icon_t", INPUTS["icon_header"]):
        if member.endswith("_COUNT"):
            continue
        if value in slot_member or not 0 <= value < total:
            raise ContractError(f"{INPUTS['icon_header']}: {member} = {value} "
                                f"{'shares a slot with ' + slot_member[value] if value in slot_member else 'is outside the table'}")
        slot_member[value] = member
    if len(slot_member) != total:        # unique and in range, so equal length means every slot is named
        raise ContractError(f"{INPUTS['icon_header']}: system_icon_t leaves slot(s) "
                            f"[{first_gap(slot_member)}] (and possibly more) without a member")
    check_declaration(store, INPUTS["icon_store"], "icon_names")
    names = literal_entries(initializer(store, "icon_names", INPUTS["icon_store"]), f"{INPUTS['icon_store']}: icon_names[]")
    for slot in range(total):
        if slot >= len(names) or not names[slot]:
            raise ContractError(f"{INPUTS['icon_store']}: icon_names[] has no name for "
                                f"{slot_member.get(slot, f'slot {slot}')} (a zero-filled slot the IRES loader would dereference)")
    if len(names) > total:
        raise ContractError(f"{INPUTS['icon_store']}: icon_names[] has {len(names)} names for {total} slots")
    for half, lo, hi in (("glyph", 0, mono), ("colour", mono, total)):
        seen = {}
        for slot in range(lo, hi):
            if names[slot] in seen:
                raise ContractError(f"{INPUTS['icon_store']}: {names[slot]!r} names both "
                                    f"{slot_member.get(seen[names[slot]])} and {slot_member.get(slot)} in the {half} half")
            seen[names[slot]] = slot
    cps_text = strip_c_comments(tree.read_text(INPUTS["codepoints"]))
    check_declaration(cps_text, INPUTS["codepoints"], "icon_codepoints")
    cps = []
    for entry in c_entries(initializer(cps_text, "icon_codepoints", INPUTS["codepoints"])):
        if not re.fullmatch(r"0x[0-9a-fA-F]+", entry) or int(entry, 16) > 0xFFFFFFFF:
            raise ContractError(f"{INPUTS['codepoints']}: icon_codepoints[] entry {entry!r} is not one hex literal "
                                f"that fits its uint32_t slot")
        cps.append(int(entry, 16))
    if len(cps) != mono:
        raise ContractError(f"{INPUTS['codepoints']}: icon_codepoints[] holds {len(cps)} codepoints for {mono} glyph slots")
    for slot in range(mono):
        if slot >= len(cps) or cps[slot] == 0:
            raise ContractError(f"{INPUTS['codepoints']}: icon_codepoints[] has no codepoint for "
                                f"{slot_member.get(slot, f'slot {slot}')}")
    # The bound is read from the pinned ires_load() only: `size > <bound>` rejects anything larger.
    load = bodies[(INPUTS["icon_store"], "ires_load")]
    found = re.findall(r"\bsize\s*>\s*([A-Za-z_]\w*|[0-9][0-9\s*()]*?)\s*\)", load)
    if len(found) != 1:
        raise ContractError(f"{INPUTS['icon_store']}: ires_load() must hold exactly one `size > <bound>` check")
    bound = found[0].strip()
    ires_bytes = (define(store, bound, INPUTS["icon_store"]) if re.fullmatch(r"[A-Za-z_]\w*", bound)
                  else int_expr(bound, f"{INPUTS['icon_store']}: ires_load size bound"))
    capacity = " ".join(checked_macro_text(store, "IRES_COLOR_COUNT", INPUTS["icon_store"]).split())
    if capacity != "(ICON_COLOR_COUNT - ICON_MONO_COUNT)" or enum["ICON_COLOR_COUNT"] != total:
        raise ContractError(f"{INPUTS['icon_store']}: IRES_COLOR_COUNT must be (ICON_COLOR_COUNT - ICON_MONO_COUNT) "
                            f"and ICON_TOTAL_COUNT must equal ICON_COLOR_COUNT, so every colour slot loads")
    return {"names": names, "mono": mono, "total": total, "ires_max_sizes": define(store, "IRES_MAX_SIZES", INPUTS["icon_store"]),
            "ires_bytes": ires_bytes}


def _packer_inputs(tree: Tree) -> dict:
    text = strip_c_comments(tree.read_text(INPUTS["irespack"]))
    rel = INPUTS["irespack"]
    # The packer walks SIZE_COUNT sizes; the published list is the whole array only while
    # SIZE_COUNT is exactly that array's length.
    size_count = [checked_macro_text(text, "SIZE_COUNT", rel)]
    if not re.fullmatch(r"\(\s*sizeof\s*\(\s*supported_sizes\s*\)\s*/\s*"
                                                 r"sizeof\s*\(\s*supported_sizes\s*\[\s*0\s*\]\s*\)\s*\)\s*",
                                                 size_count[0]):
        raise ContractError(f"{rel}: SIZE_COUNT must be the length of supported_sizes[] "
                            f"(sizeof(supported_sizes) / sizeof(supported_sizes[0]))")
    check_declaration(text, rel, "supported_sizes")
    check_declaration(text, rel, "accepted_icons")
    sizes = []
    for entry in c_entries(initializer(text, "supported_sizes", rel)):
        if not re.fullmatch(r"[1-9][0-9]*", entry):
            raise ContractError(f"{rel}: supported_sizes[] entry {entry!r} must be a plain decimal literal")
        sizes.append(int(entry))
    items = c_entries(initializer(text, "accepted_icons", rel))
    if not items or items[-1] != "NULL" or items.count("NULL") != 1 or \
            not all(SINGLE_LITERAL.fullmatch(i) for i in items[:-1]):
        raise ContractError(f"{rel}: accepted_icons[] must be string literals ending in exactly one NULL "
                            f"(the packer stops at the first NULL)")
    accepted = literal_entries(",".join(items[:-1]), f"{rel}: accepted_icons[]")
    header = re.search(r"typedef\s+struct[^{]*\{(.*?)\}\s*ires_header_t\s*;", text, re.S)
    field = re.search(r"\b(uint8_t|uint16_t|uint32_t)\s+size_count\b", header.group(1)) if header else None
    if not field:
        raise ContractError(f"{rel}: cannot find ires_header_t.size_count")
    store = strip_c_comments(tree.read_text(INPUTS["icon_store"]))
    for name, field_max in (("IRES_MAGIC", 2**32 - 1), ("IRES_VERSION", 2**16 - 1)):   # uint32_t magic, uint16_t version
        if define(text, name, rel) != define(store, name, INPUTS["icon_store"]):
            raise ContractError(f"{rel} and {INPUTS['icon_store']} disagree on {name}: the loader would reject every archive")
        if not 0 <= define(text, name, rel) <= field_max:
            raise ContractError(f"{rel}: {name} does not fit its ires_header_t field")
    return {"sizes": sizes, "accepted": accepted, "max_icons": define(text, "MAX_ICONS", rel),
            "max_sizes": define(text, "MAX_SIZES", rel), "max_name_len": define(text, "MAX_NAME_LEN", rel),
            "name_table": int_expr(array_size(text, "char name_table", rel), f"{rel}: name_table"),
            "wire_sizes": {"uint8_t": 255, "uint16_t": 65535, "uint32_t": 2**32 - 1}[field.group(1)]}


def _literal_names(tree: Tree) -> dict:
    found = {}
    for rel in tree.files_containing("icon_get_by_name", LITERAL_ROOTS, (".c", ".h")):
        text = strip_c_comments(tree.read_text(rel))
        for m in re.finditer(r"\bicon_get_by_name\s*\(", text):
            depth, i = 1, m.end()
            while i < len(text) and depth:
                if text[i] == '"':
                    j = i + 1
                    while j < len(text) and text[j] != '"':
                        j += 2 if text[j] == "\\" else 1
                    i = j + 1
                    continue
                depth += {"(": 1, ")": -1}.get(text[i], 0)
                i += 1
            arg = text[m.end():i - 1].strip()
            if '"' not in arg:
                continue                                   # a runtime value, or the definition itself
            if in_conditional(text, m.start()):
                raise ContractError(f"{rel}: icon_get_by_name({arg[:40]}) sits inside a conditional block; "
                                    f"the contract cannot know whether a build requests it")
            if not SINGLE_LITERAL.fullmatch(arg):
                raise ContractError(f"{rel}: icon_get_by_name({arg[:40]}) must pass exactly one plain string literal")
            found.setdefault(decode_literal(arg, f"{rel}: icon_get_by_name"), []).append(rel)
    return found


def _the_set(tree: Tree) -> dict:
    """{stem: used_for} from the rows of icons.md "The set"."""
    text = tree.read_text(INPUTS["icons_md"])
    m = re.search(r"^## The set\s*$(.*?)^## ", text, re.M | re.S)
    if not m:
        raise ContractError(f"{INPUTS['icons_md']}: no '## The set' table")
    rows = {}
    for line in m.group(1).splitlines():
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if len(cells) == 3 and cells[1].startswith("`") and cells[1].endswith(".svg`"):
            rows[cells[1][1:-5]] = cells[2]
    return rows


def _tokens(tree: Tree) -> dict:
    data = tree.read_json(INPUTS["tokens"])
    themes = data["color"]["themes"]
    names = {theme: set(values) for theme, values in themes.items()}
    every = set.union(*names.values())
    for theme in sorted(names):
        missing = sorted(every - names[theme])
        if missing:
            raise ContractError(f"{INPUTS['tokens']}: theme {theme!r} lacks token {missing[0]!r} that another theme defines")

    def svg_order(value: str) -> str:
        value = value.strip()
        if re.fullmatch(r"#[0-9A-Fa-f]{8}", value):
            return "#" + value[3:9].upper() + value[1:3].upper()   # tokens.json is #AARRGGBB; SVG is #RRGGBBAA
        if re.fullmatch(r"#[0-9A-Fa-f]{6}", value):
            return value.upper()
        raise ContractError(f"{INPUTS['tokens']}: cannot read colour {value!r}")

    if data["color"].get("default_theme") not in themes:
        raise ContractError(f"{INPUTS['tokens']}: default_theme {data['color'].get('default_theme')!r} is not a theme")
    values = {name: {theme: svg_order(themes[theme][name]) for theme in sorted(themes)} for name in sorted(every)}
    return {"fallback_theme": data["color"]["default_theme"], "values": values}


def _makefile_list(text: str, var: str) -> list:
    m = re.search(r"^" + re.escape(var) + r"\s*:?=((?:[^\n]*\\\n)*[^\n]*)$", text, re.M)
    if not m:
        raise ContractError(f"Makefile: cannot find {var}")
    return m.group(1).replace("\\\n", " ").split()


def _makefile_value(text: str, var: str) -> str:
    m = re.search(r"^" + re.escape(var) + r"\s*\??:?=[ \t]*(.*)$", text, re.M)
    if not m:
        raise ContractError(f"Makefile: cannot find {var}")
    if not re.fullmatch(r"\S+", m.group(1)):
        # GNU Make keeps trailing whitespace (and every inner space) in the value, so the recipe would
        # see a different path from the one the contract publishes.
        raise ContractError(f"Makefile: {var} must be one word with no trailing whitespace (found {m.group(1)!r})")
    return m.group(1)


GUEST_FORBIDDEN = ("\\", "/", "\0")


def guest_fold(name: str) -> str:
    """The name the guest's IXFS lookup compares: ASCII letters folded to lower case."""
    return "".join(chr(ord(c) + 32) if "A" <= c <= "Z" else c for c in name)


def _cursor_inputs(tree: Tree, guest_name_bytes: int, bodies: dict) -> dict:
    header = strip_c_comments(tree.read_text(INPUTS["cursor_header"]))
    driver = strip_c_comments(tree.read_text(INPUTS["cursor_driver"]))
    make = tree.read_text(INPUTS["makefile"])
    rel = INPUTS["cursor_driver"]
    # C indexes cursor_filenames[] by value, so shapes are resolved to numeric slots: every slot
    # below CURSOR_COUNT needs exactly one member and exactly one mapping (no alias, no gap).
    members = parse_enum(header, "cursor_shape_t", INPUTS["cursor_header"])
    values = dict(members)
    if "CURSOR_COUNT" not in values:
        raise ContractError(f"{INPUTS['cursor_header']}: cursor_shape_t has no CURSOR_COUNT")
    count = values["CURSOR_COUNT"]
    by_slot = {}
    for member, value in members:
        if member == "CURSOR_COUNT":
            continue
        if not 0 <= value < count or value in by_slot:
            raise ContractError(f"{INPUTS['cursor_header']}: {member} = {value} "
                                f"{'shares a slot with ' + by_slot[value] if value in by_slot else 'is outside 0..CURSOR_COUNT-1'}")
        by_slot[value] = member
    if len(by_slot) != count:
        raise ContractError(f"{INPUTS['cursor_header']}: cursor_shape_t leaves slot(s) "
                            f"[{first_gap(by_slot)}] (and possibly more) without a shape")
    shapes = [by_slot[s] for s in range(count)]
    check_declaration(driver, rel, "cursor_filenames")
    check_make_variables(make)
    table = {}
    for entry in c_entries(initializer(driver, "cursor_filenames", rel)):
        m = re.fullmatch(r"\[\s*(CURSOR_\w+)\s*\]\s*=\s*(\"(?:[^\"\\]|\\.)*\")", entry)
        if not m or m.group(1) not in values or values[m.group(1)] in table:
            raise ContractError(f"{rel}: cursor_filenames[] entry {entry[:50]!r} is not one [CURSOR_X] = \"literal\" "
                                f"for a slot not yet mapped")
        table[values[m.group(1)]] = decode_literal(m.group(2), f"{rel}: cursor_filenames[]")
    guest_dir = _makefile_value(make, "CURSOR_GUEST_DIR")
    sources = _makefile_list(make, "CURSOR_SRCS")
    by_name = {}
    for src in sources:
        name = Path(src).name
        if any(c in name for c in GUEST_FORBIDDEN) or name in (".", "..") or any(ord(c) < 32 for c in name):
            raise ContractError(f"Makefile: cursor {src!r}: {name!r} is not one guest path component")
        if len(name.encode()) > guest_name_bytes:
            raise ContractError(f"Makefile: cursor {src!r}: {len(name.encode())}-byte name exceeds the image's {guest_name_bytes}")
        key = guest_fold(name)
        if key in by_name and by_name[key] != src:
            raise ContractError(f"Makefile: cursors {by_name[key]!r} and {src!r} claim the same guest file {name!r}")
        by_name[key] = src
    prefix = "C:\\" + guest_dir.replace("/", "\\") + "\\"
    out_shapes = []
    for slot, shape in enumerate(shapes):
        if slot not in table:
            raise ContractError(f"{rel}: cursor_filenames[] has no file for {shape}")
        path = table[slot]
        name = path[len(prefix):] if path.startswith(prefix) else None
        if not name or "\\" in name:
            raise ContractError(f"{rel}: {shape} opens {path!r} but CURSOR_GUEST_DIR installs to {prefix}")
        if guest_fold(name) not in by_name:
            raise ContractError(f"{rel}: {shape} opens {path!r}, which no CURSOR_SRCS entry installs")
        out_shapes.append({"shape": shape, "name": name, "path": path})
    # The values below are read from the pinned bodies (BEHAVIOUR_PINS), so they cannot drift
    # from what xcur_parse() and cursor_init() do.
    parser = bodies[(rel, "xcur_parse")]
    m = re.search(r"\bw\s*>\s*(\d+)\s*\|\|\s*h\s*>\s*(\d+)", parser)
    cutoff = re.search(r"\bbest_diff\s*=\s*(\d+)\s*;", parser)
    if not m or not cutoff:
        raise ContractError(f"{rel}: cannot read the frame selection rule (axis bound and best_diff cutoff)")
    target = define(driver, "XCUR_TARGET_SIZE", rel)
    save = define(header, "CURSOR_MAX_SIZE", INPUTS["cursor_header"])
    rules = {
        "magic": define(driver, "XCUR_MAGIC", rel),
        "image_type": define(driver, "XCUR_IMAGE_TYPE", rel),
        "target_size": target,
        "selection": "closest_nominal_first",
        "max_nominal": INT32_MAX,
        "distance_cutoff": int(cutoff.group(1)),
        "max_axis": min(int(m.group(1)), int(m.group(2))),
        "frame_pixels": min(target * target, save * save),
        "save_under": save,
        "read_prefix_bytes": define(driver, "XCUR_MAX_FILE", rel),
        "all_frames_max_axis": min(int(m.group(1)), int(m.group(2))),
        "all_frames_hotspot_inside": True,
    }
    files = [{"name": Path(src).name, "source": src} for src in sorted(sources, key=lambda s: Path(s).name)]
    return {"guest_dir": guest_dir, "rules": rules, "shapes": out_shapes, "files": files}


def generate(tree: Tree) -> dict:
    """The contract for the tree, after every source-side check (outputs are never read)."""
    bodies = check_pins(tree)
    check_layouts(tree)
    check_extents(tree)
    icons_in, pack = _icon_inputs(tree, bodies), _packer_inputs(tree)
    mkfs = strip_c_comments(tree.read_text(INPUTS["mkfs"]))
    guest_name_bytes = define(mkfs, "IXFS_MAX_NAME", INPUTS["mkfs"]) - 1
    profile = tree.read_json(INPUTS["profile"])
    check_geometry_costs(profile)
    tokens = _tokens(tree)
    the_set = _the_set(tree)
    literals = _literal_names(tree)

    limits = {
        "ires_sizes": min(pack["max_sizes"], icons_in["ires_max_sizes"], pack["wire_sizes"]),
        "name_bytes": pack["max_name_len"] - 1,
        "pack_icons": pack["max_icons"],
        "name_table": min(pack["name_table"], 65536),
        "ires_bytes": icons_in["ires_bytes"],
        "guest_name_bytes": guest_name_bytes,
    }
    sizes = pack["sizes"]
    if not 1 <= len(sizes) <= limits["ires_sizes"]:
        raise ContractError(f"{INPUTS['irespack']}: supported_sizes holds {len(sizes)} sizes; 1 to {limits['ires_sizes']} fit")
    if any(not 1 <= s <= 65535 for s in sizes) or sizes != sorted(set(sizes)):
        raise ContractError(f"{INPUTS['irespack']}: supported_sizes must be unique, ascending and 1 to 65535: {sizes}")
    accepted = pack["accepted"]
    if len(accepted) > limits["pack_icons"]:
        raise ContractError(f"{INPUTS['irespack']}: {len(accepted)} accepted icons exceed MAX_ICONS ({limits['pack_icons']})")
    names_bytes = sum(len(n.encode()) + 1 for n in accepted)
    if names_bytes > limits["name_table"]:
        raise ContractError(f"{INPUTS['irespack']}: accepted names need {names_bytes} name-table bytes; {limits['name_table']} fit")
    header = 16 + 2 * len(sizes) + len(accepted) * (4 + 8 * len(sizes)) + names_bytes
    budget = ((header + 3) & ~3) + len(accepted) * sum(s * s * 4 for s in sizes)
    if budget > limits["ires_bytes"]:
        raise ContractError(f"{INPUTS['irespack']}: a full pack of {len(accepted)} icons at {sizes} is {budget} bytes; "
                            f"the loader accepts {limits['ires_bytes']}")

    names, mono = icons_in["names"], icons_in["mono"]
    glyph, colour = names[:mono], names[mono:icons_in["total"]]
    # Only direct children count: the converter renders resources/icons/src/*.svg and nothing below it.
    sources_present = {Path(p).stem for p in tree.files(ICON_SRC, (".svg",)) if Path(p).parent.as_posix() == ICON_SRC}
    entries = []
    for name in sorted(set(glyph)):
        srcs = ["kernel_table"] + (["literal"] if name in literals else [])
        entries.append({"name": name, "provider": "glyph", "sources": srcs})
    candidates = set(colour) | set(accepted) | set(the_set) | {n for n in literals if n not in glyph}
    for name in sorted(candidates):
        if len(name.encode()) > limits["name_bytes"]:
            raise ContractError(f"colour icon {name!r} is {len(name.encode())} bytes; the packer keeps {limits['name_bytes']}")
        srcs = [s for s, hit in (("irespack", name in accepted), ("kernel_table", name in colour),
                                 ("literal", name in literals and name not in glyph), ("spec", name in the_set)) if hit]
        first = names.index(name) if name in names else None
        lookup = None if first is None else ("glyph" if first < mono else "colour")
        missing = [m for m, hit in (("irespack", name in accepted), ("kernel_table", name in colour)) if not hit]
        if name in colour and lookup != "colour":
            missing.append("lookup")
        required = name in colour or name in accepted
        if required and name not in sources_present:
            raise ContractError(f"{ICON_SRC}: required colour icon {name!r} has no source {name}.svg")
        entries.append({"name": name, "provider": "colour", "sources": sorted(srcs), "used_for": the_set.get(name),
                        "lookup": lookup, "required": required, "wired": not missing, "missing": missing})

    cursors = _cursor_inputs(tree, guest_name_bytes, bodies)
    contract = {"schema": SCHEMA, "profile": profile, "tokens": tokens, "sizes": sizes, "limits": limits,
                "icons": entries, "cursors": cursors}
    for rel in tree.files(ICON_SRC, (".svg",)):
        try:
            # The OS converter hands these bytes straight to rsvg-convert, so they keep literal colours
            # until it resolves tokens; the published profile still admits tokens for Impossible Brand.
            validate_svg(tree.read_text(rel), contract, literal_colours=True)
        except ContractError as error:
            raise ContractError(f"{rel}: {error}") from None
    for f in cursors["files"]:
        try:
            validate_xcursor(tree.read_bytes(f["source"]), cursors["rules"])
        except ContractError as error:
            raise ContractError(f"{f['source']}: {error}") from None
    return contract


def dumps(contract: dict) -> str:
    return json.dumps(contract, indent=1, sort_keys=True, ensure_ascii=False) + "\n"


def first_difference(a, b, path="$"):
    if type(a) is not type(b):
        return path
    if isinstance(a, dict):
        for key in sorted(set(a) | set(b)):
            if key not in a or key not in b:
                return f"{path}.{key}"
            found = first_difference(a[key], b[key], f"{path}.{key}")
            if found:
                return found
        return None
    if isinstance(a, list):
        for i, (x, y) in enumerate(zip(a, b)):
            found = first_difference(x, y, f"{path}[{i}]")
            if found:
                return found
        return None if len(a) == len(b) else f"{path}[{min(len(a), len(b))}]"
    return None if a == b else path


# ---------------------------------------------------------------- SVG validation

SVG_NS = "{http://www.w3.org/2000/svg}"
XLINK_HREF = "{http://www.w3.org/1999/xlink}href"
PREDEFINED_ENTITIES = {"amp", "lt", "gt", "quot", "apos"}
HEX = re.compile(r"#(?:[0-9A-Fa-f]{3}|[0-9A-Fa-f]{6}|[0-9A-Fa-f]{8})")
TOKEN = re.compile(r"var\(--([A-Za-z0-9_]+)\)")
LOCAL_URL = re.compile(r"url\(#([^)\s]+)\)")
NUMBER = re.compile(r"[-+]?(?:[0-9]+\.?[0-9]*|\.[0-9]+)(?:[eE][-+]?[0-9]+)?")
PATH_ARGS = {"m": 2, "l": 2, "h": 1, "v": 1, "c": 6, "s": 4, "q": 4, "t": 2, "a": 7, "z": 0}


def _check_prolog(text: str) -> None:
    text = strip_comments(text)          # comment text is not markup
    head = text[:text.find("<svg")] if "<svg" in text else text
    declaration = re.match(r"\s*<\?xml\s([^?]*)\?>", head.lstrip("\ufeff"))
    if declaration:
        encoding = re.search(r"""\bencoding\s*=\s*["']([^"']*)["']""", declaration.group(1))
        if encoding and encoding.group(1).lower() not in ("utf-8", "utf8"):
            raise ContractError(f"prolog: encoding {encoding.group(1)!r} declared; system icons are UTF-8")
    rest = re.sub(r"^\s*<\?xml\s[^?]*\?>", "", head.lstrip("\ufeff"))
    if re.search(r"<[^\s>/!?<]*:", strip_comments(text)):     # `<` excluded: each attempt ends at the next `<`
        raise ContractError("namespace prefixes on elements are outside the profile")
    if "<![CDATA[" in strip_comments(text):
        raise ContractError("CDATA sections are outside the profile")
    if "<?" in rest or "<!DOCTYPE" in text or "<!ENTITY" in text:
        raise ContractError("prolog: only an XML declaration may precede <svg> (no processing instruction, DOCTYPE or entity)")
    if re.search(r"<\?", text[text.find("<svg"):] if "<svg" in text else ""):
        raise ContractError("prolog: processing instruction inside the document")
    for ref in re.findall(r"&([A-Za-z_][\w.-]*);", text):
        if ref not in PREDEFINED_ENTITIES:
            raise ContractError(f"entity reference &{ref}; is outside the profile")


def path_commands(d: str) -> int:
    """Decoded drawing commands of path data, each implicit repetition counted.

    One left-to-right pass (linear time): a command letter, then its numbers; a comma is allowed
    only between two numbers; the data starts with a moveto; arc flags are single 0 or 1."""
    i, n, count, cmd, args, last = 0, len(d), 0, None, 0, None

    def close():
        nonlocal count
        if cmd is None:
            return
        need = PATH_ARGS[cmd.lower()]
        if need and (args == 0 or args % need):
            raise ContractError(f"path data: {cmd} with {args} numbers")
        count += 1 if need == 0 else args // need

    while True:
        while i < n and d[i] in " \t\r\n":
            i += 1
        if i >= n:
            break
        c = d[i]
        if c == ",":
            if last != "num":
                raise ContractError("path data: a comma must follow a number")
            last, i = "comma", i + 1
            continue
        if c.isalpha():
            if last == "comma":
                raise ContractError("path data: a comma before a command letter")
            if c.lower() not in PATH_ARGS:
                raise ContractError(f"path data: unknown command {c!r}")
            if cmd is None and c not in "Mm":
                raise ContractError("path data must start with a moveto (M or m)")
            close()
            cmd, args, last, i = c, 0, "cmd", i + 1
            continue
        if cmd is None:
            raise ContractError("path data starts with a number")
        if PATH_ARGS[cmd.lower()] == 0:
            raise ContractError("path data: numbers after Z")
        if cmd.lower() == "a" and args % 7 in (3, 4):
            if c not in "01":
                raise ContractError("path data: arc flag must be 0 or 1")
            args, last, i = args + 1, "num", i + 1
            continue
        m = NUMBER.match(d, i)
        if not m:
            raise ContractError(f"path data: unexpected {c!r}")
        args, last, i = args + 1, "num", m.end()
    if last == "comma":
        raise ContractError("path data: a trailing comma")
    close()
    return count


def scan_points(points: str, tag: str) -> int:
    """The coordinate count of a points list, read in one linear pass (comma only between numbers)."""
    i, n, count, last = 0, len(points), 0, None
    while True:
        spaced = False
        while i < n and points[i] in " \t\r\n":
            i, spaced = i + 1, True
        if i >= n:
            break
        if points[i] == ",":
            if last != "num":
                raise ContractError(f"{tag}: points must be numbers separated by spaces or commas")
            last, i = "comma", i + 1
            continue
        m = NUMBER.match(points, i)
        if not m or (last == "num" and not spaced):
            raise ContractError(f"{tag}: points must be numbers separated by spaces or commas")
        count, last, i = count + 1, "num", m.end()
    if last == "comma" or count % 2:
        raise ContractError(f"{tag}: points must hold coordinate pairs, with no trailing comma")
    return count


GEOMETRY_FORMULAS = {"path": "decoded commands, each implicit repetition counted", "polyline": "points",
                     "polygon": "points + 1"}


def check_geometry_costs(profile: dict) -> None:
    costs = profile["geometry_cost"]
    for tag, formula in GEOMETRY_FORMULAS.items():
        if costs.get(tag) != formula:
            raise ContractError(f"{INPUTS['profile']}: geometry_cost.{tag} must be {formula!r}, which validate_svg implements")
    for tag in ("line", "rect", "rect_rounded", "circle", "ellipse"):
        if type(costs.get(tag)) is not int or costs[tag] < 1:
            raise ContractError(f"{INPUTS['profile']}: geometry_cost.{tag} must be a positive integer")


def _geometry_cost(tag: str, el, costs: dict) -> int:
    if tag == "path":
        return path_commands(el.get("d", ""))
    if tag in ("polyline", "polygon"):
        return scan_points(el.get("points", ""), tag) // 2 + (1 if tag == "polygon" else 0)
    if tag == "rect":
        return costs["rect_rounded"] if (el.get("rx") or el.get("ry")) else costs["rect"]
    if tag in ("line", "circle", "ellipse"):
        return costs[tag]
    return 0


TAG = re.compile(r"""(<[A-Za-z][A-Za-z0-9_:.-]*)((?:\s+[A-Za-z_:][A-Za-z0-9_:.-]*\s*=\s*(?:"[^"]*"|'[^']*'))*)(\s*/?>)""")
ATTR = re.compile(r"""(\s+)([A-Za-z_:][A-Za-z0-9_:.-]*)(\s*=\s*)(["'])(.*?)\4""", re.S)


def strip_comments(text: str) -> str:
    """The text without XML comments, in one forward pass (an unterminated comment is refused)."""
    out, pos = [], 0
    while True:
        start = text.find("<!--", pos)
        if start < 0:
            out.append(text[pos:])
            return "".join(out)
        end = text.find("-->", start + 4)
        if end < 0:
            raise ContractError("an unterminated XML comment")
        out.append(text[pos:start])
        pos = end + 3


def split_comments(text: str) -> list:
    """[(is_comment, part)] in order, in one forward pass."""
    parts, pos = [], 0
    while True:
        start = text.find("<!--", pos)
        if start < 0:
            parts.append((False, text[pos:]))
            return parts
        end = text.find("-->", start + 4)
        if end < 0:
            raise ContractError("an unterminated XML comment")
        parts += [(False, text[pos:start]), (True, text[start:end + 3])]
        pos = end + 3


def raw_attributes(text: str):
    """(name, raw value) of every attribute of every tag outside comments, as written."""
    for is_comment, part in split_comments(text):
        if is_comment:
            continue
        for tag in TAG.finditer(part):
            for a in ATTR.finditer(tag.group(2)):
                yield a.group(2), a.group(5)


def validate_svg(text: str, contract: dict, literal_colours: bool = False) -> None:
    """Raise ContractError unless the SVG lies inside the system SVG profile."""
    profile, tokens = contract["profile"], contract["tokens"]["values"]
    limits = profile["limits"]
    _check_prolog(text)
    try:
        root = ET.fromstring(text)
    except ET.ParseError as error:
        raise ContractError(f"not well-formed XML: {error}") from None
    elements, forbidden = profile["elements"], set(profile["forbidden"])
    drawing_attrs = set(profile["drawing_attributes"])
    ids, parent, geometry = {}, {}, {}

    def tag_of(el):
        if not el.tag.startswith(SVG_NS):
            raise ContractError(f"element {el.tag!r} is outside the SVG namespace")
        return el.tag[len(SVG_NS):]

    def walk(el, depth):
        tag = tag_of(el)
        if depth > limits["nesting"]:
            raise ContractError(f"nesting deeper than {limits['nesting']}")
        if tag in forbidden or tag not in elements:
            raise ContractError(f"element <{tag}> is outside the profile")
        allowed = set(elements[tag]["attributes"]) | (drawing_attrs if elements[tag]["drawing"] else set())
        for attr in el.attrib:
            local = "href" if attr == XLINK_HREF else attr.split("}")[-1]
            if attr.startswith("{") and attr != XLINK_HREF:
                raise ContractError(f"<{tag}> attribute {attr!r} is outside the profile")
            if local not in allowed:
                raise ContractError(f"<{tag}> attribute {local!r} is outside the profile")
        geometry[el] = _geometry_cost(tag, el, profile["geometry_cost"])   # every element, drawn or not
        if el.get("id") is not None:
            if el.get("id") in ids:
                raise ContractError(f"duplicate id {el.get('id')!r}")
            ids[el.get("id")] = el
        for child in el:
            parent[child] = el
            walk(child, depth + 1)

    if tag_of(root) != "svg":
        raise ContractError(f"the root element is <{tag_of(root)}>, not <svg>")
    if len(TAG.findall(strip_comments(text))) != sum(1 for _ in root.iter()):
        raise ContractError("a start tag uses names or attributes outside ASCII XML names; the profile reads only those")
    for name, raw in raw_attributes(text):
        # resolve_paint and the consumers read these attributes as written, so they must not
        # spell anything through character or entity references.
        if name.split(":")[-1] in ("fill", "stroke", "stop-color", "clip-path", "href") and "&" in raw:
            raise ContractError(f"{name}: character and entity references are not allowed in paint and reference values")
    walk(root, 1)

    def target(ref: str, kinds, what: str):
        el = ids.get(ref)
        if el is None:
            raise ContractError(f"{what} #{ref} does not resolve")
        if tag_of(el) not in kinds:
            raise ContractError(f"{what} #{ref} resolves to <{tag_of(el)}>, not {'/'.join(sorted(kinds))}")
        return el

    gradients = {"linearGradient", "radialGradient"}
    paint = profile["paint"]
    for el in root.iter():
        tag = tag_of(el)
        for attr in ("fill", "stroke", "stop-color"):
            value = el.get(attr)
            if value is None:
                continue
            kinds = paint[attr]
            if value != value.strip():
                raise ContractError(f"<{tag}> {attr}={value!r}: no leading or trailing spaces in paint values")
            if value == "none" and "none" in kinds:
                continue
            if HEX.fullmatch(value) and "hex" in kinds:
                continue
            m = TOKEN.fullmatch(value)
            if m and "token" in kinds:
                if literal_colours:
                    raise ContractError(f"<{tag}> {attr}={value!r}: this renderer takes literal colours only")
                if m.group(1) not in tokens:
                    raise ContractError(f"<{tag}> {attr}={value!r}: unknown token {m.group(1)!r}")
                continue
            m = LOCAL_URL.fullmatch(value)
            if m and "gradient" in kinds:
                target(m.group(1), gradients, f"<{tag}> {attr}")
                continue
            raise ContractError(f"<{tag}> {attr}={value!r} is outside the paint grammar")
        clip = el.get("clip-path")
        if clip is not None and clip.strip() != "none":
            m = LOCAL_URL.fullmatch(clip.strip())
            if not m:
                raise ContractError(f"<{tag}> clip-path={clip!r} must be none or url(#id)")
            target(m.group(1), {"clipPath"}, f"<{tag}> clip-path")
        if el.get("href") is not None and el.get(XLINK_HREF) is not None:
            raise ContractError(f"<{tag}> carries both href and xlink:href; use one")
        href = el.get("href", el.get(XLINK_HREF))
        if href is not None:
            if not href.startswith("#"):
                raise ContractError(f"<{tag}> href={href!r}: only a same-document #id is allowed")
            target(href[1:], set(profile["references"]["href"]["targets"]), f"<{tag}> href")

    # Expanded cost. Each element gets ONE summary, independent of the paint it inherits:
    # (elements, commands, use depth, shapes whose fill is inherited, shapes whose stroke is
    # inherited). A use instance or clip application charges its target's summary again, and an
    # element that sets fill or stroke charges the paint server once per inheriting shape below it,
    # so painted shapes are counted as SVG draws them while the work stays linear in the document.
    memo, active = {}, set()
    cap_elements, cap_commands, cap_depth = limits["elements"], limits["path_commands"], limits["use_depth"]
    cap_shapes = cap_elements + 1
    shapes = {"path", "rect", "circle", "ellipse", "line", "polyline", "polygon"}

    def server(value):
        m = LOCAL_URL.fullmatch((value or "").strip())
        return ids[m.group(1)] if m and tag_of(ids[m.group(1)]) in gradients else None

    def edges(el):
        """(element, kind) for everything el draws: kind is child, use, clip or server."""
        tag, out = tag_of(el), []
        href = el.get("href", el.get(XLINK_HREF))
        if tag == "use" and href:
            out.append((ids[href[1:]], "use"))
        clip = (el.get("clip-path") or "").strip()
        if LOCAL_URL.fullmatch(clip):
            out.append((ids[LOCAL_URL.fullmatch(clip).group(1)], "clip"))
        for attr in ("fill", "stroke"):
            if server(el.get(attr)) is not None:
                out.append((server(el.get(attr)), "server"))
        for child in el:
            if tag_of(child) in ("defs", "clipPath", "symbol") or tag_of(child) in gradients:
                continue   # definitions draw only where referenced, and are charged there
            out.append((child, "child"))
        return out

    def summarise(el):
        tag = tag_of(el)
        elements_n, commands, depth, nfill, nstroke = 1, geometry[el], 0, 0, 0
        for target, kind in edges(el):
            e, c, d, f, s = memo[target]
            if kind == "server":
                continue                    # charged below, once per shape that paints with it
            elements_n, commands = elements_n + e, commands + c
            depth = max(depth, d + (1 if kind == "use" else 0))
            if kind != "clip":              # clip geometry is not painted
                nfill, nstroke = nfill + f, nstroke + s
        if tag in shapes:
            nfill, nstroke = nfill + 1, nstroke + 1
        for attr, count in (("fill", nfill), ("stroke", nstroke)):
            if el.get(attr) is not None:    # this element decides the paint of every inheriting shape below
                srv = server(el.get(attr))
                if srv is not None:
                    elements_n += count * memo[srv][0]
                if attr == "fill":
                    nfill = 0
                else:
                    nstroke = 0
        return (min(elements_n, cap_elements + 1), min(commands, cap_commands + 1), min(depth, cap_depth + 1),
                min(nfill, cap_shapes), min(nstroke, cap_shapes))

    def cost(start):
        """The summary of start, by an explicit-stack depth-first walk with cycle detection."""
        stack = [(start, False)]
        while stack:
            el, done = stack.pop()
            if done:
                memo[el] = summarise(el)
                active.discard(el)
                continue
            if el in memo:
                continue
            if el in active:
                raise ContractError(f"reference cycle through <{tag_of(el)}> {('#' + el.get('id')) if el.get('id') else ''}")
            active.add(el)
            stack.append((el, True))
            for target, _ in edges(el):
                if target in memo:
                    continue
                if target in active:
                    raise ContractError(f"reference cycle through <{tag_of(target)}> "
                                        f"{('#' + target.get('id')) if target.get('id') else ''}")
                stack.append((target, False))
        return memo[start]

    for el in ids.values():          # a cycle among referenced definitions is refused even when nothing draws it
        cost(el)
    total = cost(root)
    if total[0] > cap_elements:
        raise ContractError(f"expanded drawing exceeds {cap_elements} elements")
    if total[1] > cap_commands:
        raise ContractError(f"expanded drawing exceeds {cap_commands} geometry commands")
    if total[2] > cap_depth:
        raise ContractError(f"use references nest deeper than {cap_depth}")


def resolve_paint(text: str, contract: dict, theme: str = "") -> str:
    """The SVG with every var(--token) replaced by its value in the theme (default: the fallback theme)."""
    tokens = contract["tokens"]
    theme = theme or tokens["fallback_theme"]

    # Only fill, stroke and stop-color values that are exactly one token; ids, comments and other
    # attributes are left as written.
    def tag(m):
        # Walk the tag's attributes in order (name = quoted value), so a quoted value of another
        # attribute is never mistaken for a paint attribute.
        head, attrs, tail = m.group(1), m.group(2), m.group(3)
        out, pos = [], 0
        for a in re.finditer(r"""(\s+)([A-Za-z_:][\w:.-]*)(\s*=\s*)(["'])(.*?)\4""", attrs, re.S):
            if a.start() != pos:
                raise ContractError(f"cannot read the attributes of <{head[1:]}>")
            value = a.group(5)
            token = TOKEN.fullmatch(value)
            if a.group(2) in ("fill", "stroke", "stop-color") and token:
                if token.group(1) not in tokens["values"]:
                    raise ContractError(f"unknown token {token.group(1)!r}")
                value = tokens["values"][token.group(1)][theme]
            out.append(a.group(1) + a.group(2) + a.group(3) + a.group(4) + value + a.group(4))
            pos = a.end()
        if attrs[pos:].strip():
            raise ContractError(f"cannot read the attributes of <{head[1:]}>")
        return head + "".join(out) + attrs[pos:] + tail
    parts = [part for _, part in split_comments(text)]
    return "".join(part if part.startswith("<!--") else
                   re.sub(r"(<[A-Za-z][\w:.-]*)((?:\s+[A-Za-z_:][\w:.-]*\s*=\s*(?:\"[^\"]*\"|'[^']*'))*)(\s*/?>)", tag, part)
                   for part in parts)


# ---------------------------------------------------------------- Xcursor and IRES validation

def validate_xcursor(data: bytes, rules: dict) -> dict:
    """Raise ContractError unless the cursor loader, as it stands, selects a frame that fits its buffers.

    The selection is judged on the prefix the loader reads; the all-frame rules (the build's
    long-standing checks) on every frame of the whole file."""
    whole = data
    data = data[:rules["read_prefix_bytes"]]
    if len(data) < 16:
        raise ContractError("shorter than the Xcursor header")
    magic, header_size, _version, toc_count = struct.unpack_from("<IIII", data, 0)
    if magic != rules["magic"]:
        raise ContractError("not an Xcursor file")
    if header_size < 16 or header_size > len(data) or toc_count > (len(data) - header_size) // 12:
        raise ContractError("table of contents runs past the bytes the loader reads")
    frames = []
    for i in range(toc_count):
        kind, nominal, pos = struct.unpack_from("<III", data, header_size + i * 12)
        if kind == rules["image_type"]:
            frames.append((nominal, pos))

    def chunk(pos):
        if pos > len(data) or len(data) - pos < 36:
            raise ContractError(f"image chunk at {pos} lies outside the bytes the loader reads")
        ctype = struct.unpack_from("<I", data, pos + 4)[0]
        w, h, hx, hy = struct.unpack_from("<IIII", data, pos + 16)
        return ctype, w, h, hx, hy

    for nominal, pos in frames:
        if nominal > rules["max_nominal"]:
            raise ContractError(f"nominal size {nominal} exceeds {rules['max_nominal']} (the loader ranks it as signed)")
        if pos > len(whole) or len(whole) - pos < 36:
            raise ContractError(f"image chunk at {pos} runs past the end of the file")
        w, h, hx, hy = struct.unpack_from("<IIII", whole, pos + 16)
        if w > rules["all_frames_max_axis"] or h > rules["all_frames_max_axis"]:
            raise ContractError(f"frame {w}x{h} exceeds {rules['all_frames_max_axis']} per axis")
        if rules["all_frames_hotspot_inside"] and not (hx < w and hy < h):
            raise ContractError(f"frame {w}x{h} has its hotspot ({hx}, {hy}) outside it")
    best, picked = rules["distance_cutoff"], None
    for nominal, pos in frames:
        distance = abs(nominal - rules["target_size"])
        if distance < best:
            best, picked = distance, (nominal, pos)
    if picked is None:
        raise ContractError("the loader selects no frame")
    ctype, w, h, hx, hy = chunk(picked[1])
    if ctype != rules["image_type"]:
        raise ContractError("the selected chunk is not an image")
    if not (0 < w <= rules["max_axis"] and 0 < h <= rules["max_axis"]):
        raise ContractError(f"selected frame {w}x{h} is outside 1..{rules['max_axis']} per axis")
    if w > rules["save_under"] or h > rules["save_under"] or w * h > rules["frame_pixels"]:
        raise ContractError(f"selected frame {w}x{h} does not fit the kernel's {rules['frame_pixels']}-pixel slot")
    if len(data) - picked[1] - 36 < w * h * 4:
        raise ContractError("selected frame's pixels run past the bytes the loader reads")
    if not (hx < w and hy < h):
        raise ContractError(f"selected frame's hotspot ({hx}, {hy}) lies outside {w}x{h}")
    return {"nominal": picked[0], "width": w, "height": h}


# ---------------------------------------------------------------- command line

def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--check", action="store_true", help="fail when the committed contract differs from a fresh one")
    ap.add_argument("--staged", action="store_true", help="read every input and the contract from the git index")
    ap.add_argument("--sizes", action="store_true", help="print the packed icon sizes")
    ap.add_argument("--validate-sources", nargs="+", metavar="SVG", help="validate SVG files against the profile")
    ap.add_argument("--literal-colours", action="store_true", help="with --validate-sources: refuse theme tokens")
    ap.add_argument("--root", type=Path, default=ROOT, help=argparse.SUPPRESS)
    args = ap.parse_args(argv)
    tree = Tree(args.root, staged=args.staged)
    try:
        contract = generate(tree)
        if args.sizes:
            print("\n".join(str(s) for s in contract["sizes"]))
            return 0
        if args.validate_sources:
            for name in args.validate_sources:
                try:
                    validate_svg(Path(name).read_text(encoding="utf-8"), contract, literal_colours=args.literal_colours)
                except (ContractError, OSError, UnicodeDecodeError) as error:
                    print(f"brand-contract: {name}: {error}", file=sys.stderr)
                    return 1
            return 0
        if args.check:
            try:
                raw = tree.read_bytes(CONTRACT)
            except ContractError as error:
                print(f"brand-contract: {error}; run: python3 scripts/brand_contract.py", file=sys.stderr)
                return 1
            if raw == dumps(contract).encode("utf-8"):
                return 0
            try:
                where = first_difference(json.loads(dumps(contract)), json.loads(raw))
            except (json.JSONDecodeError, UnicodeDecodeError):
                where = None
            what = f"is stale at {where}" if where else "differs from the generated bytes (formatting, key order or duplicates)"
            print(f"brand-contract: {CONTRACT} {what}; run: python3 scripts/brand_contract.py", file=sys.stderr)
            return 1
        if args.staged:
            ap.error("--staged needs --check or --sizes")
        out = args.root / CONTRACT
        fd, tmp = tempfile.mkstemp(prefix=".brand-contract-", suffix=".tmp", dir=out.parent)
        if out.exists():
            mode = out.stat().st_mode & 0o777
        else:
            mask = os.umask(0)
            os.umask(mask)
            mode = 0o666 & ~mask
        os.fchmod(fd, mode)
        try:
            with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as handle:
                handle.write(dumps(contract))
            os.replace(tmp, out)
        except BaseException:
            Path(tmp).unlink(missing_ok=True)
            raise
        print(f"brand-contract: wrote {CONTRACT} ({len(contract['icons'])} icons, {len(contract['cursors']['shapes'])} cursors)")
        return 0
    except ContractError as error:
        print(f"brand-contract: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
