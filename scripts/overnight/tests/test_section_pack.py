#!/usr/bin/env python3
"""Contract test for section-pack.py -- the one-shot section orientation tool.

Runs with --no-clangd for determinism (the clangd precision layer is verified
manually against the live repo; here we pin the rg-based path, artifact
production, symbol resolution, and working-tree binding).
"""
import json
import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
PACK = HERE.parent / "section-pack.py"


def _fixture(d):
    root = pathlib.Path(d)
    (root / "src/kernel").mkdir(parents=True)
    (root / "include/kernel").mkdir(parents=True)
    (root / "todo/02-kernel-core").mkdir(parents=True)
    subprocess.run(["git", "init", "-q", str(root)], check=True)
    (root / "include/kernel/frob.h").write_text(
        "#define FROB_MAX 8\n"
        "int frob_init(void);\n")
    (root / "src/kernel/frob.c").write_text(
        "#include \"kernel/frob.h\"\n"
        "int frob_init(void) { return FROB_MAX; }\n"
        "int frob_caller(void) { return frob_init(); }\n")
    (root / "src/kernel/test").mkdir(parents=True)
    (root / "src/kernel/test/test_frob.c").write_text("// test_frob\n")
    (root / "todo/02-kernel-core/TODO-99.md").write_text(
        "# TODO-99\n\n"
        "> **Validated:** 2026-07-01 | x\n\n"
        "> **Gap-audited:** 2026-07-02 | x\n\n"
        "## 1. Frob subsystem\n\n"
        "Implement `frob_init` using `FROB_MAX` in "
        "`src/kernel/frob.c` and `include/kernel/frob.h`. Touches the syscall "
        "path (ABI).\n\n- [ ] wire frob_init\n- [ ] test it\n")
    subprocess.run(["git", "-C", str(root), "add", "-A"], check=True)
    subprocess.run(["git", "-C", str(root), "-c", "user.email=t@t", "-c",
                    "user.name=t", "commit", "-qm", "init"],
                   check=True, capture_output=True)
    return root


def _pack(root, extra=()):
    r = subprocess.run(
        [sys.executable, str(PACK), "todo/02-kernel-core/TODO-99.md", "1",
         "--project", str(root), "--no-clangd", *extra],
        capture_output=True, text=True, cwd=str(root))
    assert r.returncode == 0, r.stderr
    return json.loads(r.stdout)


def test_pack_resolves_symbols_and_writes_artifact():
    with tempfile.TemporaryDirectory() as d:
        root = _fixture(d)
        summ = _pack(root)
        # artifact written
        pack_path = root / summ["pack_path"]
        assert pack_path.exists(), "pack.json not written"
        pack = json.loads(pack_path.read_text())
        # rg resolved the definitions from the section's backticked symbols
        assert pack["symbol_defs"].get("frob_init", "").startswith("src/kernel/frob.c:"), pack["symbol_defs"]
        assert pack["symbol_defs"].get("FROB_MAX", "").startswith("include/kernel/frob.h:"), pack["symbol_defs"]
        # likely files + ABI + gates surfaced
        assert "src/kernel/frob.c" in pack["likely_files"]
        assert pack["abi_impact"] is True
        assert summ["resolver"] == "rg"
        assert summ["bundle_dir"], "bundle not produced"


def test_pack_digest_is_worktree_bound():
    with tempfile.TemporaryDirectory() as d:
        root = _fixture(d)
        d1 = _pack(root)["digest"]
        # edit a likely file (unstaged) -> digest must change
        (root / "src/kernel/frob.c").write_text("int frob_init(void){return 9;}\n")
        d2 = _pack(root)["digest"]
        assert d1 != d2, "digest did not follow the working tree"


if __name__ == "__main__":
    test_pack_resolves_symbols_and_writes_artifact()
    test_pack_digest_is_worktree_bound()
    print("PASS: section-pack")
