#!/usr/bin/env python3
"""Parse a usage-limit reset hint from the tail of an overnight run report and
write the snooze-until epoch the pre-flight honors.

Usage: parse-usage-limit.py <report-path> <snooze-file>

Reads the report FILE directly (NOT stdin). The previous inline form piped
`tail -60 "$REPORT" | python3 - "$SNOOZE_FILE" <<'PYEOF'`, but a heredoc redirect
WINS over the pipe: `python3 -` consumed the program from stdin and
`sys.stdin.read()` then hit EOF, so the report text never reached the parser and
the "resets ..." banner never matched -- the runner could not snooze on a real
usage limit and relaunched straight back into it (2026-07-12 fix, P1.4).

Session limits give a time of day ("resets 8:10pm (Area/City)"); weekly limits
give a DATE AND a time ("resets Jun 19, 9am"). The time may omit minutes ("9am"),
so minutes are optional. A +3 min margin lands the relaunch just AFTER the real
reset; an unparseable hint snoozes 30 min; the cap is 8 days. Exits 0 always (no
banner = no snooze). Stdlib only.
"""
import datetime
import re
import sys
import time

_BANNER_RE = re.compile(r"hit your .{0,40}limit.{0,12}resets ([^\n]*)", re.I)
_TIME_RE = re.compile(r"(\d{1,2})(?::(\d{2}))?\s*(am|pm)", re.I)
_DATE_RE = re.compile(r"([A-Z][a-z]{2,8})\s+(\d{1,2})")


def parse(text: str, now: datetime.datetime):
    """(snooze_until_datetime, hint) for a usage-limit banner in `text`, or
    (None, None) when no banner is present. `now` is injected for determinism."""
    m = _BANNER_RE.search(text)
    if not m:
        return None, None
    hint = m.group(1).strip()

    tm = _TIME_RE.search(hint)
    hour = minute = None
    if tm:
        hour = int(tm.group(1)) % 12
        minute = int(tm.group(2) or 0)
        if tm.group(3).lower() == "pm":
            hour += 12

    dm = _DATE_RE.search(hint)   # a date accompanies a weekly limit
    until = None
    if dm:
        try:
            month = datetime.datetime.strptime(dm.group(1)[:3], "%b").month
            until = now.replace(month=month, day=int(dm.group(2)),
                                hour=(hour if hour is not None else 9),
                                minute=(minute if minute is not None else 0),
                                second=0, microsecond=0)
            if until <= now:
                until = until.replace(year=until.year + 1)
        except ValueError:
            until = None
    elif hour is not None:
        until = now.replace(hour=hour, minute=minute, second=0, microsecond=0)
        if until <= now:
            until += datetime.timedelta(days=1)

    if until is None:
        until = now + datetime.timedelta(minutes=30)
    # land just AFTER the reset; never a minute early (early = re-hit the limit).
    until += datetime.timedelta(minutes=3)
    cap = now + datetime.timedelta(days=8)
    if until > cap:
        until = cap
    return until, hint


def _tail(path: str, n: int = 60) -> str:
    try:
        with open(path, encoding="utf-8", errors="replace") as fh:
            return "".join(fh.readlines()[-n:])
    except OSError:
        return ""


def main(argv) -> int:
    if len(argv) < 2:
        sys.stderr.write("usage: parse-usage-limit.py <report> <snooze-file>\n")
        return 2
    report, snooze_file = argv[0], argv[1]
    until, hint = parse(_tail(report), datetime.datetime.now())
    if until is None:
        return 0
    epoch = int(time.mktime(until.timetuple()))
    try:
        with open(snooze_file, "w", encoding="ascii") as fh:
            fh.write(str(epoch))
    except OSError as exc:
        sys.stderr.write(f"parse-usage-limit: cannot write {snooze_file}: {exc}\n")
        return 1
    print(f"usage limit detected (resets {hint}); snoozing launches "
          f"until {until.isoformat()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
