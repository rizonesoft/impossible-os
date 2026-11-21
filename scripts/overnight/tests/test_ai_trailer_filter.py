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


def _scan(tmp_path, records=None, fail=False):
    """Run the production history scan (--zero-trailers) with its `git log`
    answered by a shim: `records` are (commit ID, message bytes) pairs."""
    import os
    real = subprocess.run(["bash", "-c", "command -v git"], capture_output=True, text=True, check=True).stdout.strip()
    data = tmp_path / "log.bin"
    data.write_bytes(b"".join(c.encode() + b"\n" + body + b"\0" for c, body in records or []))
    answer = "exit 128" if fail else f'cat "{data}"; exit 0'
    shim = tmp_path / "git"
    # The shim answers the whole synthetic history: the adoption anchor (a
    # root, so the range is HEAD) and the log; the real lookup stays covered
    # by the production run of the suite.
    shim.write_text('#!/bin/bash\n'
                    'for a in "$@"; do [ "$a" = "-S" ] && { echo ' + "a" * 40 + '; exit 0; }; done\n'
                    '[ "$1" = rev-parse ] && [ "$2" = --verify ] && exit 1\n'
                    f'for a in "$@"; do [ "$a" = "--format=%H%n%B" ] && {{ {answer}; }}; done\n'
                    f'exec {real} "$@"\n')
    shim.chmod(0o755)
    # A different awk first on PATH and a Turkish locale must not change the
    # scan: it runs gawk, in the C locale.
    (tmp_path / "awk").write_text("#!/bin/sh\nexit 3\n")
    (tmp_path / "awk").chmod(0o755)
    env = dict(os.environ, PATH=f"{tmp_path}:{os.environ['PATH']}", LC_ALL="tr_TR.UTF-8")
    r = subprocess.run(["bash", str(SUITE), "--zero-trailers"], capture_output=True, text=True, timeout=120, env=env)
    return r.returncode, r.stdout + r.stderr


def test_a_capital_i_key_is_found_in_any_locale(tmp_path):
    rc, out = _scan(tmp_path, [("9" * 40, b"subject\n\nAI-Author: S <s@example.org>\n")])
    assert rc == 1 and "found 1 post-anchor commit(s)" in out, out[-2000:]
    import os
    r = subprocess.run(["bash", str(SUITE), "--ai-trailers"], input="s\n\nAI-AUTHOR: x\n", capture_output=True,
                       text=True, timeout=30, env=dict(os.environ, LC_ALL="tr_TR.UTF-8"))
    assert r.stdout.strip() == "AI-AUTHOR: x", r


def test_a_failed_history_read_fails_the_gate(tmp_path):
    """The history scan's own git log fails: the suite must report it, not pass."""
    rc, out = _scan(tmp_path, fail=True)
    assert rc == 1 and "zero-trailer scan read the history" in out, out[-2000:]


def test_the_history_scan_finds_a_mixed_case_key_after_a_divider(tmp_path):
    """Drives the suite's own history scanner, not the --ai-trailers filter."""
    fake = "f" * 40
    rc, out = _scan(tmp_path, [(fake, b"subject\n\n---\n\nnotes\n\nco-AUTHORED-by: S <s@example.org>\n")])
    assert rc == 1 and "found 1 post-anchor commit(s)" in out and fake[:12] in out, out[-2000:]


def test_a_control_byte_is_read_as_the_filter_reads_it(tmp_path):
    """The scan never rewrites a byte: a line starting with 0x01 before a key
    is not a trailer, to the filter or the scan; a real one beside it is."""
    line = "\x01AI-Author: S <s@example.org>\n"
    assert _trailers(f"subject\n\n{line}") == ""
    rc, out = _scan(tmp_path, [("e" * 40, b"subject\n\n" + line.encode())])
    assert rc == 0 and "no NEW AI-attribution trailers" in out, out[-2000:]
    rc, out = _scan(tmp_path, [("d" * 40, b"subject\n\n" + line.encode() + b"\nAssisted-by: TOOL:MODEL\n")])
    assert rc == 1 and "found 1 post-anchor commit(s)" in out and "d" * 12 in out, out[-2000:]


def test_each_commit_is_named_once_and_records_stay_apart(tmp_path):
    two = b"subject\n\nCo-authored-by: A <a@example.org>\nAssisted-by: TOOL:MODEL\n"
    rc, out = _scan(tmp_path, [("a" * 40, two), ("b" * 40, b"clean\n"), ("c" * 40, b"x\n\nai-author: m\n")])
    assert rc == 1 and "found 2 post-anchor commit(s)" in out and "b" * 12 not in out, out[-2000:]


def test_an_exception_covers_its_own_commit_and_no_other_with_its_prefix(tmp_path):
    """The grandfathered commits are named by full ID; a commit sharing a prefix is refused."""
    fake = "c6b30e0f" + "0" * 32
    rc, out = _scan(tmp_path, [(fake, b"subject\n\nCo-authored-by: S <s@example.org>\n"),
                               ("c6b30e0f625c344393b262697af4aa67decb1b41", b"s\n\nCo-authored-by: S <s@x>\n")])
    assert rc == 1 and "found 1 post-anchor commit(s)" in out and fake[:12] in out, out[-2000:]
