#!/bin/sh
# .claude/hooks/wrap.sh -- POSIX shell prefilter for hot-path Python hooks.
#
# Usage in settings.json:
#   "command": "sh .claude/hooks/wrap.sh <hook.py> <marker1> [<marker2> ...]"
#
# Behavior:
#   1. Reads the hook JSON payload from stdin into $PAYLOAD.
#   2. If ANY marker substring appears in $PAYLOAD, pipes it to
#      `python3 <hook.py>` and forwards that exit code.
#   3. If NO marker matches, exits 0 immediately -- saves the
#      ~25-35ms of python3 cold start that the hook would otherwise
#      pay on every matching tool call.
#
# Design constraints (from TODO-08 section 7 design review):
#   - OPT-IN ONLY. Reminder hooks (systemMessage; never block) are
#     wrap.sh-eligible. BLOCKING / stateful hooks (sys.exit(2),
#     git-commit alias detection, codex dispatch state writes,
#     transcript-walking gates) keep direct `python3 hook.py`
#     invocation -- the marker prefilter cannot replicate the
#     wrapper / alias / heredoc / segment classifier those hooks
#     rely on. See MANIFEST.md "Wrap.sh eligibility" column.
#   - POSIX-only. No bash-isms. The script body itself adds zero
#     measurable startup cost (sh is already running).
#   - Safe-by-default. On any unexpected stdin shape, falls
#     through to python3. The cost of a false positive is one
#     extra python startup; the cost of a false negative is
#     skipping a real hook fire, which is unacceptable.
#
# Markers: arbitrary substrings. Best practice is to pick markers
# distinctive enough that the hook's own gating logic would
# trigger on the same payload. Examples:
#   - codex_review_reception_reminder.py: `adversarial-review`
#   - todo_edit_reminder.py: `todo/`
#   - test_wiring_reminder.py: `src/kernel/test/`
#   - skill_pipeline_reminder.py: `implement-todo-section`
#   - skill_claudemd_sync_reminder.py: `.claude/skills/`
#   - domain_quality_router.py: `src/`  (or `.c", ".h"` -- the
#       hook's own routing scan is fast enough that a coarse
#       prefilter is fine)
#
# Idempotence: if `python3 hook.py` itself decides "not for me"
# and exits 0, the wrap.sh path adds no overhead beyond the
# substring scan. The only failure mode is python startup, and
# wrap.sh skips that when no marker matches.

set -eu

HOOK="${1:-}"
shift || true

if [ -z "$HOOK" ] || [ ! -f "$HOOK" ]; then
    # No hook script -- act as a no-op pass-through.
    cat >/dev/null
    exit 0
fi

PAYLOAD=$(cat)

# If no markers were supplied, always run the hook (caller wants
# unconditional invocation; equivalent to calling python3 directly,
# but keeps the wrap.sh shape uniform across settings.json).
if [ "$#" -eq 0 ]; then
    printf '%s' "$PAYLOAD" | python3 "$HOOK"
    exit $?
fi

for marker; do
    case "$PAYLOAD" in
        *"$marker"*)
            printf '%s' "$PAYLOAD" | python3 "$HOOK"
            exit $?
            ;;
    esac
done

# No marker matched -- fast exit, no python startup.
exit 0
