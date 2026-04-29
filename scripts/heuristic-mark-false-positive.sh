#!/usr/bin/env bash
# heuristic-mark-false-positive.sh -- retroactively flag a heuristic
# miss-log entry as a false positive (TODO-08 partial-enforcement
# heuristics, step false-positive-flagging mechanism).
#
# Usage:
#   scripts/heuristic-mark-false-positive.sh <step>           # mark most-recent miss for step
#   scripts/heuristic-mark-false-positive.sh <step> <n>       # mark Nth-most-recent (n>=1)
#
# The marker rewrites the JSONL line in-place under flock (matching the
# advisory lock emit_warn uses) so the read/modify/replace cycle cannot
# race a peer's rotate+append.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LOG="${HEURISTIC_MISSES_LOG:-$REPO_ROOT/.claude/state/heuristic-misses.jsonl}"

if [ $# -lt 1 ] || [ $# -gt 2 ]; then
    echo "usage: heuristic-mark-false-positive.sh <step> [<n>]" >&2
    exit 2
fi

STEP="$1"
N="${2:-1}"

if ! [[ "$STEP" =~ ^[0-9]+$ ]]; then
    echo "step must be an integer (got: $STEP)" >&2
    exit 2
fi
if ! [[ "$N" =~ ^[0-9]+$ ]] || [ "$N" -lt 1 ]; then
    echo "n must be a positive integer (got: $N)" >&2
    exit 2
fi

if [ ! -f "$LOG" ]; then
    echo "[mark-fp] no log at $LOG" >&2
    exit 1
fi

python3 - "$LOG" "$STEP" "$N" <<'PY'
import json
import os
import sys
import tempfile

log_path = sys.argv[1]
step = int(sys.argv[2])
n = int(sys.argv[3])

# Codex adversarial M2 fix 2026-04-29: take the same advisory flock
# emit_warn uses so the marker's read/modify/replace cycle does not
# race a peer's rotate+append.
lock_path = log_path + ".lock"
try:
    import fcntl as _fcntl
    have_flock = True
except ImportError:
    _fcntl = None
    have_flock = False

lock_fd = None
if have_flock:
    lock_fd = os.open(lock_path, os.O_RDWR | os.O_CREAT, 0o600)
    _fcntl.flock(lock_fd, _fcntl.LOCK_EX)

rc = 0
try:
    with open(log_path, "r", encoding="utf-8") as f:
        raw = f.readlines()

    matching = []
    for idx, line in enumerate(raw):
        line_strip = line.strip()
        if not line_strip:
            continue
        try:
            rec = json.loads(line_strip)
        except Exception:
            continue
        if isinstance(rec, dict) and rec.get("step") == step:
            matching.append((idx, rec))

    if not matching:
        print(f"[mark-fp] no entries for step {step}")
        rc = 1
    else:
        matching.sort(key=lambda x: x[1].get("ts_ns", 0), reverse=True)
        if n > len(matching):
            print(f"[mark-fp] step {step} has only {len(matching)} entr(ies); n={n} out of range")
            rc = 1
        else:
            target_idx, target_rec = matching[n - 1]
            target_rec["false_positive_user_flagged"] = True
            new_line = json.dumps(target_rec, separators=(",", ":")) + "\n"
            raw[target_idx] = new_line

            fd, tmp = tempfile.mkstemp(
                prefix=os.path.basename(log_path) + ".tmp.",
                dir=os.path.dirname(log_path),
            )
            try:
                with os.fdopen(fd, "w", encoding="utf-8") as f:
                    f.writelines(raw)
                os.replace(tmp, log_path)
            except Exception:
                try:
                    os.unlink(tmp)
                except Exception:
                    pass
                raise

            print(f"[mark-fp] step {step} entry n={n} flagged as false positive")
            print(f"  signal: {target_rec.get('signal', '')}")
            print(f"  detail: {target_rec.get('detail', '')[:120]}")
finally:
    if lock_fd is not None:
        try:
            if have_flock:
                _fcntl.flock(lock_fd, _fcntl.LOCK_UN)
        except Exception:
            pass
        try:
            os.close(lock_fd)
        except Exception:
            pass

sys.exit(rc)
PY
