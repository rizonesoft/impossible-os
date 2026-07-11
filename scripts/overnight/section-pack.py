#!/usr/bin/env python3
"""section-pack.py -- ONE deterministic command that fully orients a section.

Replaces the sequential grep/read/dispatch loop a fresh worker used to run
(measured 2026-07-11: ~10-40 search/read decisions + often both discovery
agents on a complex section) with one content-bound artifact. Given a TODO
section it:

  * runs section-manifest.py  -> section coordinates, open items, XREFs, likely
    files, relevant tests, required gates, complexity, working-tree hashes;
  * extracts candidate symbols (backticked identifiers + names in the section);
  * resolves each symbol's DEFINITION + reference count -- via clangd +
    compile_commands.json when both are present (precise), else ripgrep (one
    batched `rg --json` over src/include/user), always producing a result;
  * flags registration / ABI / syscall touchpoints and likely callers;
  * runs evidence-bundle.py   -> compact structural views;
  * writes the whole pack under .claude/overnight/packs/<key>/pack.json and
    prints a BOUNDED JSON summary + the artifact path.

The pack key binds to WORKING-TREE bytes (worktree_hash: section text + likely
files, untracked included), so an unchanged section reuses the pack across
sessions and relaunches, and ANY edit invalidates it -- no clock, no staleness.

Usage:
  section-pack.py <todo-path> <section-n> [--project DIR] [--no-clangd]

This is a read-only orientation tool. It never edits, builds, or commits; the
full implement/review pipeline and its gates are unchanged -- the pack bounds
DISCOVERY, not the gates.
"""
from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from worktree_hash import content_hashes, worktree_key  # noqa: E402

SECTION_RE = re.compile(r"^## (\d+)\.\s*(.*)")
# Backticked identifiers + bare C-ish identifiers worth resolving.
IDENT_RE = re.compile(r"`([A-Za-z_][A-Za-z0-9_]{3,})`")
BARE_IDENT_RE = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]{4,})\b")
REG_MARKERS = ("ssdt_register", "shadow_register", "test_suite_register",
               "register_", "dispatch_table", "_init(", "syscall")
# English/common words to drop from bare-identifier candidates.
STOP = {"should", "return", "these", "which", "there", "where", "under",
        "using", "value", "table", "field", "state", "count", "check",
        "items", "files", "tests", "gates", "review", "section", "kernel",
        "process", "thread", "memory", "shared", "system", "handle",
        "object", "before", "after", "while", "false", "NULL", "true"}
PACK_DIR_REL = ".claude/overnight/packs"
MAX_PACKS = 12
MAX_SYMBOLS = 24


def _sh(cmd, cwd, timeout=60):
    try:
        return subprocess.run(cmd, cwd=str(cwd), capture_output=True,
                              text=True, timeout=timeout)
    except Exception:
        return None


def section_block(text: str, n: int) -> str:
    lines = text.splitlines()
    start = end = None
    for i, ln in enumerate(lines):
        m = SECTION_RE.match(ln)
        if m:
            if int(m.group(1)) == n and start is None:
                start = i
            elif start is not None:
                end = i
                break
    return "\n".join(lines[start:end]) if start is not None else ""


def candidate_symbols(block: str) -> list:
    """Symbols worth resolving: backticked identifiers first (highest signal),
    then bare snake_case/CamelCase names, capped."""
    ranked = []
    seen = set()
    for m in IDENT_RE.finditer(block):
        s = m.group(1)
        if s not in seen:
            seen.add(s)
            ranked.append(s)
    for m in BARE_IDENT_RE.finditer(block):
        s = m.group(1)
        if s in seen or s in STOP:
            continue
        # keep snake_case or CamelCase (identifier-shaped), drop prose words
        if "_" in s or (any(c.isupper() for c in s[1:]) and not s.isupper()):
            seen.add(s)
            ranked.append(s)
    return ranked[:MAX_SYMBOLS]


def _def_rank(sym: str, line: str, path: str) -> int:
    """Higher = better definition candidate. 0 = not a definition. Ranks a real
    function DEFINITION above a header DECLARATION so `int f(void);` in a .h
    never wins over `int f(void) {` in a .c."""
    s = re.escape(sym)
    if re.search(rf"^\s*#define\s+{s}\b", line):
        return 4
    if re.search(rf"^\s*(?:typedef\s+)?(?:struct|enum|union)\s+{s}\b", line):
        return 4
    if re.search(rf"^\s*(?:static\s+|inline\s+)*[\w][\w\s*]*[\s*]{s}\s*\(", line):
        if line.rstrip().endswith(";"):
            return 1  # declaration (prototype)
        rank = 3      # definition body (opens `{` here or next line)
        if path.endswith((".c", ".asm", ".S")):
            rank += 1  # prefer the implementation TU over a header
        return rank
    return 0


def rg_resolve(root: Path, symbols: list, paths: list) -> dict:
    """One batched `rg --json` over paths; per-symbol ref count + best-ranked
    definition line (definition beats declaration). Always available."""
    out = {s: {"def": None, "refs": 0, "source": "rg", "_rank": 0}
           for s in symbols}
    if not symbols:
        return out
    pattern = r"\b(" + "|".join(re.escape(s) for s in symbols) + r")\b"
    r = _sh(["rg", "--json", "-e", pattern, "--", *paths], root, timeout=90)
    if r is None:
        return {s: {k: v for k, v in d.items() if k != "_rank"}
                for s, d in out.items()}
    for ln in r.stdout.splitlines():
        try:
            obj = json.loads(ln)
        except Exception:
            continue
        if obj.get("type") != "match":
            continue
        data = obj["data"]
        path = data["path"]["text"]
        lno = data["line_number"]
        text = data["lines"]["text"].rstrip("\n")
        for sm in data.get("submatches", []):
            sym = sm["match"]["text"]
            if sym not in out:
                continue
            out[sym]["refs"] += 1
            rank = _def_rank(sym, text, path)
            if rank > out[sym]["_rank"]:
                out[sym]["_rank"] = rank
                out[sym]["def"] = f"{path}:{lno}"
    return {s: {k: v for k, v in d.items() if k != "_rank"}
            for s, d in out.items()}


# --- optional clangd precision layer (best-effort; any failure -> rg kept) ----
class _Clangd:
    def __init__(self, root: Path, ccdir: Path):
        exe = shutil.which("clangd") or shutil.which("clangd-19")
        if not exe:
            raise RuntimeError("no clangd")
        self.p = subprocess.Popen(
            [exe, f"--compile-commands-dir={ccdir}", "--background-index=false",
             "--log=error"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL, cwd=str(root))
        self.root = root
        self._id = 0
        self._rpc("initialize", {"processId": os.getpid(),
                                 "rootUri": root.as_uri(),
                                 "capabilities": {}}, want=True)
        self._notify("initialized", {})

    def _send(self, obj):
        body = json.dumps(obj).encode()
        self.p.stdin.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
        self.p.stdin.flush()

    def _read(self, deadline):
        # read one LSP message (headers + body), honoring a wall deadline
        headers = b""
        while b"\r\n\r\n" not in headers:
            if time.time() > deadline:
                raise TimeoutError()
            ch = self.p.stdout.read(1)
            if not ch:
                raise EOFError()
            headers += ch
        length = 0
        for h in headers.split(b"\r\n"):
            if h.lower().startswith(b"content-length:"):
                length = int(h.split(b":")[1].strip())
        body = self.p.stdout.read(length)
        return json.loads(body)

    def _rpc(self, method, params, want=False, budget=8):
        self._id += 1
        rid = self._id
        self._send({"jsonrpc": "2.0", "id": rid, "method": method,
                    "params": params})
        deadline = time.time() + budget
        while True:
            msg = self._read(deadline)
            if msg.get("id") == rid:
                return msg.get("result") if want else None

    def _notify(self, method, params):
        self._send({"jsonrpc": "2.0", "method": method, "params": params})

    def definition(self, rel, line0, col0, budget=8):
        uri = (self.root / rel).as_uri()
        try:
            text = (self.root / rel).read_text(encoding="utf-8", errors="replace")
        except OSError:
            return None
        self._notify("textDocument/didOpen", {"textDocument": {
            "uri": uri, "languageId": "c", "version": 1, "text": text}})
        res = self._rpc("textDocument/definition", {
            "textDocument": {"uri": uri},
            "position": {"line": line0, "character": col0}}, want=True,
            budget=budget)
        self._notify("textDocument/didClose", {"textDocument": {"uri": uri}})
        locs = res if isinstance(res, list) else ([res] if res else [])
        for loc in locs:
            u = loc.get("uri", "")
            ln = loc.get("range", {}).get("start", {}).get("line")
            if u.startswith("file://") and ln is not None:
                p = u[7:]
                try:
                    p = str(Path(p).resolve().relative_to(self.root))
                except Exception:
                    pass
                return f"{p}:{ln + 1}"
        return None

    def close(self):
        try:
            self.p.terminate()
            self.p.wait(timeout=3)
        except Exception:
            try:
                self.p.kill()
            except Exception:
                pass


def clangd_upgrade(root: Path, resolved: dict, rg_hits: dict, budget_s=25):
    """Upgrade rg definitions to precise clangd definitions where possible.
    rg_hits maps sym -> (rel, line0, col0) of one occurrence. Strictly bounded;
    any failure leaves the rg answer in place."""
    cc = root / "compile_commands.json"
    if not cc.exists() or not (shutil.which("clangd") or shutil.which("clangd-19")):
        return resolved
    try:
        cd = _Clangd(root, root)
    except Exception:
        return resolved
    stop = time.time() + budget_s
    try:
        for sym, pos in rg_hits.items():
            if time.time() > stop:
                break
            if sym not in resolved or pos is None:
                continue
            try:
                d = cd.definition(*pos)
            except Exception:
                continue
            if d:
                resolved[sym]["def"] = d
                resolved[sym]["source"] = "clangd"
    finally:
        cd.close()
    return resolved


def _rg_first_positions(root: Path, symbols: list, paths: list) -> dict:
    """One occurrence (rel, line0, col0) per symbol, for clangd to anchor on."""
    pos = {}
    if not symbols:
        return pos
    pattern = r"\b(" + "|".join(re.escape(s) for s in symbols) + r")\b"
    r = _sh(["rg", "--json", "-e", pattern, "--", *paths], root, timeout=90)
    if r is None:
        return pos
    for ln in r.stdout.splitlines():
        try:
            obj = json.loads(ln)
        except Exception:
            continue
        if obj.get("type") != "match":
            continue
        data = obj["data"]
        for sm in data.get("submatches", []):
            sym = sm["match"]["text"]
            if sym in symbols and sym not in pos:
                pos[sym] = (data["path"]["text"],
                            data["line_number"] - 1, sm["start"])
    return pos


def main(argv) -> int:
    if len(argv) < 2:
        print("usage: section-pack.py <todo-path> <section-n> [--project DIR] "
              "[--no-clangd]", file=sys.stderr)
        return 2
    todo_rel, n = argv[0], int(argv[1])
    root = Path(argv[argv.index("--project") + 1]).resolve() \
        if "--project" in argv else Path(".").resolve()
    use_clangd = "--no-clangd" not in argv
    here = Path(__file__).resolve().parent

    todo = root / todo_rel
    try:
        text = todo.read_text(encoding="utf-8")
    except OSError as exc:
        print(json.dumps({"error": f"unreadable TODO: {exc}"}))
        return 1
    block = section_block(text, n)
    if not block:
        print(json.dumps({"error": f"section {n} not found in {todo_rel}"}))
        return 1

    # 1. manifest (reuse the worktree-correct section-manifest.py)
    mr = _sh([sys.executable, str(here / "section-manifest.py"), todo_rel,
              str(n), "--project", str(root)], root, timeout=60)
    try:
        manifest = json.loads(mr.stdout) if mr else {}
    except Exception:
        manifest = {}
    likely = manifest.get("likely_files", []) or []
    search_paths = [p for p in ("src", "include", "user") if (root / p).is_dir()]

    # 2. symbols + resolution
    symbols = candidate_symbols(block)
    resolved = rg_resolve(root, symbols, search_paths)
    if use_clangd and symbols:
        positions = _rg_first_positions(root, symbols, search_paths)
        resolved = clangd_upgrade(root, resolved, positions)

    # 3. registration / ABI touchpoints in the section + likely files
    reg_hits = sorted({m for m in REG_MARKERS if m in block})
    abi = bool(re.search(r"(?i)\b(ABI|NTSTATUS|SSDT|boot_info|syscall number|"
                         r"PEB|TEB|struct offset)\b", block))

    # 4. structural views bundle (feed the manifest we just built via stdin)
    bundle_path = None
    if mr and mr.stdout.strip():
        try:
            br = subprocess.run(
                [sys.executable, str(here / "evidence-bundle.py"),
                 "--manifest", "-"], input=mr.stdout, cwd=str(root),
                capture_output=True, text=True, timeout=90)
            bundle_path = json.loads(br.stdout).get("dir")
        except Exception:
            bundle_path = None

    # 5. pack key over section text + likely files (working-tree bound)
    key_inputs = [todo_rel, *likely]
    section_digest = worktree_key(root, key_inputs)
    pack_dir = root / PACK_DIR_REL / section_digest
    pack_dir.mkdir(parents=True, exist_ok=True)

    defined = {s: r["def"] for s, r in resolved.items() if r["def"]}
    unresolved = [s for s, r in resolved.items() if not r["def"]]
    pack = {
        "todo": todo_rel, "section": n,
        "heading": manifest.get("heading", ""),
        "digest": section_digest,
        "open_items": manifest.get("open_items", []),
        "xrefs": manifest.get("xrefs", []),
        "likely_files": likely,
        "input_files": manifest.get("input_files", []),
        "relevant_tests": manifest.get("relevant_tests", []),
        "required_gates": manifest.get("required_gates", []),
        "complexity": manifest.get("complexity", {}),
        "symbols": resolved,
        "symbol_defs": defined,
        "unresolved_symbols": unresolved,
        "registration_touchpoints": reg_hits,
        "abi_impact": abi,
        "resolver": ("clangd+rg" if use_clangd and
                     any(r["source"] == "clangd" for r in resolved.values())
                     else "rg"),
        "manifest_path": str((pack_dir / "manifest.json").relative_to(root)),
        "bundle_dir": bundle_path,
        "enrich_with": manifest.get("enrich_with"),
    }
    (pack_dir / "manifest.json").write_text(json.dumps(manifest, indent=1))
    (pack_dir / "pack.json").write_text(json.dumps(pack, indent=1))
    # prune old packs
    try:
        allp = sorted((root / PACK_DIR_REL).iterdir(),
                      key=lambda p: p.stat().st_mtime, reverse=True)
        for old in allp[MAX_PACKS:]:
            subprocess.run(["rm", "-rf", str(old)], timeout=30)
    except Exception:
        pass

    # 6. bounded summary to stdout
    summary = {
        "pack_path": str((pack_dir / "pack.json").relative_to(root)),
        "digest": section_digest,
        "heading": pack["heading"],
        "open_items": len(pack["open_items"]),
        "likely_files": likely,
        "symbols_defined": len(defined),
        "symbols_unresolved": len(unresolved),
        "resolver": pack["resolver"],
        "registration_touchpoints": reg_hits,
        "abi_impact": abi,
        "bundle_dir": bundle_path,
        "complexity_verdict": pack["complexity"].get("verdict"),
    }
    print(json.dumps(summary, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
