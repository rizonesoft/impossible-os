"""The zero-trailer gate reads trailer keys the way git does: without regard to case."""
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
