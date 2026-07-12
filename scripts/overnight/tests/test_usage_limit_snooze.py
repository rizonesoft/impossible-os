#!/usr/bin/env python3
# P1.4: the usage-limit snooze parser must read the report FILE (the old inline
# `tail | python3 - <<'PYEOF'` heredoc clobbered stdin so the banner never
# parsed and the runner never snoozed). Unit-tests parse() with an injected
# `now` for determinism; the end-to-end test proves the file (not stdin) path.
import datetime
import importlib.util
import pathlib
import subprocess
import sys
import tempfile
import time

HERE = pathlib.Path(__file__).resolve().parent
SCRIPT = HERE.parent / "parse-usage-limit.py"

_spec = importlib.util.spec_from_file_location("parse_usage_limit", SCRIPT)
mod = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(mod)

NOW = datetime.datetime(2026, 7, 12, 15, 0, 0)   # fixed 'now' (3pm) for determinism


def test_no_banner_returns_none():
    until, hint = mod.parse("boot ok\nnothing here\ndone\n", NOW)
    assert until is None and hint is None


def test_session_time_later_today():
    until, hint = mod.parse("You've hit your usage limit. resets 8:10pm (US/East)", NOW)
    assert until == datetime.datetime(2026, 7, 12, 20, 13, 0), until   # 20:10 + 3m
    assert "8:10pm" in hint


def test_time_without_minutes_rolls_to_next_day():
    until, _ = mod.parse("hit your limit, resets 9am", NOW)      # 9am < 3pm now
    assert until == datetime.datetime(2026, 7, 13, 9, 3, 0), until


def test_weekly_with_date():
    until, _ = mod.parse("hit your weekly limit -- resets Jul 19, 9am", NOW)
    assert until == datetime.datetime(2026, 7, 19, 9, 3, 0), until


def test_unparseable_hint_falls_back_30min():
    until, _ = mod.parse("hit your limit resets soonish", NOW)
    assert until == NOW + datetime.timedelta(minutes=33), until      # 30 + 3 margin


def test_8day_cap():
    # A far-future weekly reset is capped at 8 days out.
    until, _ = mod.parse("hit your limit resets Dec 25, 9am", NOW)
    assert until <= NOW + datetime.timedelta(days=8), until


def test_end_to_end_reads_file_not_stdin():
    # THE regression: parse from the report FILE by path, never stdin.
    with tempfile.TemporaryDirectory() as d:
        report = pathlib.Path(d) / "run.log"
        snooze = pathlib.Path(d) / "snooze"
        report.write_text("boot ok\nYou've hit your usage limit. resets 9am (UTC)\ndone\n")
        r = subprocess.run([sys.executable, str(SCRIPT), str(report), str(snooze)],
                           text=True, capture_output=True)
        assert r.returncode == 0, r.stderr
        assert snooze.exists(), "snooze file must be written from the report FILE"
        assert int(snooze.read_text().strip()) > time.time(), "snooze in the future"
        assert "usage limit detected" in r.stdout


def test_end_to_end_no_banner_no_snooze():
    with tempfile.TemporaryDirectory() as d:
        report = pathlib.Path(d) / "run.log"
        snooze = pathlib.Path(d) / "snooze"
        report.write_text("boot ok\nall good\ndone\n")
        r = subprocess.run([sys.executable, str(SCRIPT), str(report), str(snooze)],
                           text=True, capture_output=True)
        assert r.returncode == 0, r.stderr
        assert not snooze.exists(), "no banner -> no snooze file"


if __name__ == "__main__":
    test_no_banner_returns_none()
    test_session_time_later_today()
    test_time_without_minutes_rolls_to_next_day()
    test_weekly_with_date()
    test_unparseable_hint_falls_back_30min()
    test_8day_cap()
    test_end_to_end_reads_file_not_stdin()
    test_end_to_end_no_banner_no_snooze()
    print("PASS: usage-limit-snooze")
