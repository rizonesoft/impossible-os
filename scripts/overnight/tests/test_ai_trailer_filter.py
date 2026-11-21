"""The zero-trailer gate refuses a line that starts with a forbidden key, in any case, wherever it sits."""
import subprocess
from pathlib import Path

SUITE = Path(__file__).resolve().parents[3] / "scripts/test-ai-system.sh"


def _trailers(message: str) -> str:
    return subprocess.run(["bash", str(SUITE), "--ai-trailers"], input=message, capture_output=True, text=True,
                          timeout=30, check=True).stdout


def test_forbidden_trailers_are_found_in_any_case():
    for key in ("Co-Authored-By", "co-authored-by", "CO-AUTHORED-BY", "assisted-by", "AI-Author"):
        assert _trailers(f"subject\n\nbody\n\n{key}: Someone <s@example.org>\n"), key


def test_other_trailers_and_prose_are_not():
    assert _trailers("subject\n\nbody\n\nReviewed-by: Someone <s@example.org>\n") == ""
    assert _trailers("subject\n\nthe Co-Authored-By: rule is in prose here, then more text\n") == ""


def test_a_divider_line_does_not_hide_the_trailers():
    assert _trailers("subject\n\nnotes\n\n---\n\nverification\n\nCo-Authored-By: Someone <s@example.org>\n")


def test_no_divider_or_scissors_line_hides_a_trailer_on_either_side():
    scissors = "# ------------------------ >8 ------------------------"
    for message in (f"subject\n\nnotes\n\n{scissors}\n\nCo-authored-by: S <s@example.org>\n",
                    "subject\n\nAssisted-by: TOOL:MODEL\n\n---\n\nverification notes\n",
                    "subject\n\nbody\n    co-authored-by: S <s@example.org>\nmore body\n"):
        assert _trailers(message), message


def test_a_failed_history_read_fails_the_gate(tmp_path):
    """The history scan's own git log fails: the suite must report it, not pass."""
    import os
    shim = tmp_path / "git"
    real = subprocess.run(["bash", "-c", "command -v git"], capture_output=True, text=True, check=True).stdout.strip()
    shim.write_text(f'#!/bin/bash\nfor a in "$@"; do [ "$a" = "--format=%x00%H%n%B" ] && exit 128; done\nexec {real} "$@"\n')
    shim.chmod(0o755)
    env = dict(os.environ, PATH=f"{tmp_path}:{os.environ['PATH']}")
    r = subprocess.run(["bash", str(SUITE)], capture_output=True, text=True, timeout=300, env=env)
    assert r.returncode == 1 and "zero-trailer scan read the history" in r.stdout + r.stderr, r.stdout[-2000:]


def test_the_history_scan_finds_a_mixed_case_key_after_a_divider(tmp_path):
    """Drives the suite's own history scanner, not the --ai-trailers filter."""
    import os
    fake = "f" * 40
    real = subprocess.run(["bash", "-c", "command -v git"], capture_output=True, text=True, check=True).stdout.strip()
    shim = tmp_path / "git"
    shim.write_text('#!/bin/bash\nfor a in "$@"; do if [ "$a" = "--format=%x00%H%n%B" ]; then '
                    f"printf '\\000{fake}\\nsubject\\n\\n---\\n\\nnotes\\n\\nco-AUTHORED-by: S <s@example.org>\\n'; "
                    f'exit 0; fi; done\nexec {real} "$@"\n')
    shim.chmod(0o755)
    env = dict(os.environ, PATH=f"{tmp_path}:{os.environ['PATH']}")
    r = subprocess.run(["bash", str(SUITE)], capture_output=True, text=True, timeout=300, env=env)
    out = r.stdout + r.stderr
    assert r.returncode == 1 and "found 1 post-anchor commit(s)" in out and fake[:8] in out, out[-2000:]


def test_a_message_line_holding_the_old_header_byte_hides_nothing(tmp_path):
    """A message may hold a 0x01 byte; it must not pose as the next commit's header."""
    import os
    fake = "e" * 40
    real = subprocess.run(["bash", "-c", "command -v git"], capture_output=True, text=True, check=True).stdout.strip()
    shim = tmp_path / "git"
    shim.write_text('#!/bin/bash\nfor a in "$@"; do if [ "$a" = "--format=%x00%H%n%B" ]; then '
                    f"printf '\\000{fake}\\nsubject\\n\\n\\001\\n\\nAssisted-by: TOOL:MODEL\\n'; "
                    f'exit 0; fi; done\nexec {real} "$@"\n')
    shim.chmod(0o755)
    env = dict(os.environ, PATH=f"{tmp_path}:{os.environ['PATH']}")
    r = subprocess.run(["bash", str(SUITE)], capture_output=True, text=True, timeout=300, env=env)
    out = r.stdout + r.stderr
    assert r.returncode == 1 and "found 1 post-anchor commit(s)" in out and fake[:8] in out, out[-2000:]
