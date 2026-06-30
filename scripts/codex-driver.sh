#!/usr/bin/env bash
# codex-driver.sh -- Codex driver adapter for the shared AI workflow.
#
# This is NOT a .codex skill tree. It is a thin adapter around the
# repo-owned scripts/ai-workflow protocol. Codex-exclusive development uses
# this wrapper as the active driver entry point, while Codex review still
# runs through fresh reviewer dispatches.
#
# Usage:
#   bash scripts/codex-driver.sh --dry-run --todo todo/...md --section N
#   AI_WORKFLOW_CODEX_DRIVER=1 bash scripts/codex-driver.sh --live --todo todo/...md --section N
#   AI_WORKFLOW_CODEX_DRIVER=1 bash scripts/codex-driver.sh --live --checkpoint pre-commit --todo todo/...md --section N

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

MODE=""
TODO_PATH=""
SECTION=""
RUN_ID=""
CHECKPOINT="pre-edit"

usage() {
    sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --dry-run|--live)
            MODE="${1#--}"
            shift
            ;;
        --todo)
            TODO_PATH="${2:-}"
            shift 2
            ;;
        --section)
            SECTION="${2:-}"
            shift 2
            ;;
        --run-id)
            RUN_ID="${2:-}"
            shift 2
            ;;
        --checkpoint)
            CHECKPOINT="${2:-}"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "unknown argument: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

if [ -z "$MODE" ] || [ -z "$TODO_PATH" ] || [ -z "$SECTION" ]; then
    usage >&2
    exit 2
fi

case "$CHECKPOINT" in
    pre-edit|pre-stamp|pre-commit|release|complete) ;;
    *)
        echo "unsupported checkpoint: $CHECKPOINT" >&2
        echo "expected one of: pre-edit, pre-stamp, pre-commit, release, complete" >&2
        exit 2
        ;;
esac

if [ -z "$RUN_ID" ]; then
    RUN_ID="codex-driver-$(date +%s)-$$"
fi

if [ -n "${AI_WORKFLOW_STATE_DIR:-}" ]; then
    case "$AI_WORKFLOW_STATE_DIR" in
        /*) STATE_DIR="$AI_WORKFLOW_STATE_DIR" ;;
        *) STATE_DIR="$REPO_ROOT/$AI_WORKFLOW_STATE_DIR" ;;
    esac
else
    STATE_DIR="$REPO_ROOT/build/ai-workflow"
fi
RUN_DIR="$STATE_DIR/runs/$RUN_ID"
export AI_WORKFLOW_REPO_ROOT="$REPO_ROOT"
export AI_WORKFLOW_STATE_DIR="$STATE_DIR"
cd "$REPO_ROOT"
mkdir -p "$RUN_DIR"

OBLIGATIONS_JSON="$RUN_DIR/obligations.json"
python3 "$REPO_ROOT/scripts/ai-workflow/obligations.py" \
    "$TODO_PATH" --section "$SECTION" --workflow implement \
    --driver-run-id "$RUN_ID" --format json > "$OBLIGATIONS_JSON"

if [ "$MODE" = "dry-run" ]; then
    PLAN="$RUN_DIR/plan.md"
    {
        echo "# Codex Driver Dry Run"
        echo
        echo "- run_id: $RUN_ID"
        echo "- todo: $TODO_PATH"
        echo "- section: $SECTION"
        echo "- mode: dry-run"
        echo
        echo "## Context Files"
        echo
        echo "- AGENTS.md"
        echo "- CLAUDE.md"
        echo "- $TODO_PATH"
        echo "- $OBLIGATIONS_JSON"
        echo
        echo "## Obligations"
        echo
        python3 "$REPO_ROOT/scripts/ai-workflow/obligations.py" \
            "$TODO_PATH" --section "$SECTION" --workflow implement \
            --driver-run-id "$RUN_ID" --format text
        echo
        echo "## Guardrail"
        echo
        echo "Dry-run mode records no shipping evidence and performs no edits."
    } > "$PLAN"
    echo "$PLAN"
    exit 0
fi

if [ "$MODE" != "live" ]; then
    echo "unsupported mode: $MODE" >&2
    exit 2
fi

if [ "${AI_WORKFLOW_CODEX_DRIVER:-0}" != "1" ]; then
    echo "Codex live driver is disabled." >&2
    echo "Set AI_WORKFLOW_CODEX_DRIVER=1 for an explicit live run." >&2
    exit 2
fi

if [ "$CHECKPOINT" = "release" ]; then
    python3 "$REPO_ROOT/scripts/ai-workflow/lease.py" release --run-id "$RUN_ID"
    exit 0
fi

if [ "$CHECKPOINT" = "complete" ]; then
    python3 "$REPO_ROOT/scripts/ai-workflow/lease.py" complete --run-id "$RUN_ID"
    exit 0
fi

python3 "$REPO_ROOT/scripts/ai-workflow/lease.py" acquire \
    --todo "$TODO_PATH" --section "$SECTION" \
    --driver codex --run-id "$RUN_ID" \
    --mutation edit --mutation build --mutation test --mutation stamp --mutation commit \
    >/dev/null

echo "Codex live driver: lease active for $TODO_PATH section $SECTION (run_id=$RUN_ID)."

case "$CHECKPOINT" in
    pre-edit)
        python3 "$REPO_ROOT/scripts/ai-workflow/gates.py" pre-edit \
            --todo "$TODO_PATH" --section "$SECTION" \
            --driver-run-id "$RUN_ID" --format text
        echo "Codex live driver: pre-edit gate passed; inspect obligations before mutating."
        python3 "$REPO_ROOT/scripts/ai-workflow/obligations.py" \
            "$TODO_PATH" --section "$SECTION" --workflow implement \
            --driver-run-id "$RUN_ID" --format text
        ;;
    pre-stamp)
        python3 "$REPO_ROOT/scripts/ai-workflow/gates.py" stamp \
            --todo "$TODO_PATH" --section "$SECTION" \
            --driver-run-id "$RUN_ID" --format text
        ;;
    pre-commit)
        python3 "$REPO_ROOT/scripts/ai-workflow/gates.py" commit \
            --todo "$TODO_PATH" --section "$SECTION" \
            --driver-run-id "$RUN_ID" --format text
        ;;
esac
