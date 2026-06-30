#!/usr/bin/env bash
# codex-sequencer-supervisor.sh -- bounded Codex driver for overnight runs.
#
# This script owns the outer overnight phase loop for Codex. It invokes Codex
# for one bounded phase or section at a time, verifies repo-visible progress
# after each step, and blocks further watchdog relaunches when the same target
# makes no section-level progress repeatedly.
set -euo pipefail

PROJECT_DIR="${1:?project dir required}"
TODO_FILE="${2:?todo doctrine file required}"
CODEX_BIN="${3:?codex binary required}"
CODEX_SANDBOX="${4:-danger-full-access}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

cd "$PROJECT_DIR"

RUNTIME_BASE="${OVERNIGHT_RUNNER_BASE:-.codex/overnight}"
STEP_TIMEOUT="${OVERNIGHT_CODEX_STEP_TIMEOUT_SECONDS:-5400}"
MAX_STEPS="${OVERNIGHT_CODEX_SUPERVISOR_STEPS:-8}"
NO_PROGRESS_LIMIT="${OVERNIGHT_CODEX_NO_PROGRESS_LIMIT:-3}"
BLOCK_FILE="$RUNTIME_BASE/codex-supervisor.block"
NO_PROGRESS_FILE="$RUNTIME_BASE/codex-no-progress"

require_positive_int() {
    local name="$1"
    local value="$2"
    case "$value" in
        ''|*[!0-9]*|0)
            echo "FATAL: $name must be a positive integer, got: ${value:-<empty>}" >&2
            exit 2
            ;;
    esac
}

require_positive_int OVERNIGHT_CODEX_STEP_TIMEOUT_SECONDS "$STEP_TIMEOUT"
require_positive_int OVERNIGHT_CODEX_SUPERVISOR_STEPS "$MAX_STEPS"
require_positive_int OVERNIGHT_CODEX_NO_PROGRESS_LIMIT "$NO_PROGRESS_LIMIT"

log() {
    printf '%s codex-supervisor: %s\n' "$(date '+%H:%M:%S')" "$*"
}

run_guard() {
    log "cmd: python3 .claude/hooks/run_phase_guard.py $*"
    python3 .claude/hooks/run_phase_guard.py "$@"
}

run_logged() {
    log "cmd: $*"
    "$@"
}

run_preflight_checks() {
    run_logged bash scripts/todo-graph/build-and-validate.sh --keep-cache || return 1
    run_logged bash scripts/build.sh || return 1
    tail -1 build/build.log || true
    run_logged bash scripts/test.sh QUIET=1 || return 1
}

json_get() {
    python3 -c '
import json, sys
d = json.load(sys.stdin)
v = d
for key in sys.argv[1].split("."):
    if isinstance(v, dict):
        v = v.get(key, "")
    else:
        v = ""
if isinstance(v, bool):
    print("true" if v else "false")
elif v is None:
    print("")
else:
    print(v)
' "$1"
}

first_needs_work_section() {
    python3 -c '
import json, sys
d = json.load(sys.stdin)
for sec in d.get("sections", []):
    if sec.get("class") != "DONE":
        print(sec.get("n", ""))
        break
'
}

section_class() {
    local section="$1"
    python3 -c '
import json, sys
want = str(sys.argv[1])
d = json.load(sys.stdin)
for sec in d.get("sections", []):
    if str(sec.get("n")) == want:
        print(sec.get("class", ""))
        break
' "$section"
}

file_class() {
    python3 -c 'import json,sys; print(json.load(sys.stdin).get("file_class",""))'
}

lifecycle_flag() {
    local key="$1"
    python3 -c '
import json, sys
d = json.load(sys.stdin)
v = (d.get("lifecycle") or {}).get(sys.argv[1], False)
print("true" if v else "false")
' "$key"
}

state_json() {
    python3 .claude/hooks/run_phase_guard.py status 2>/dev/null
}

classify_json() {
    python3 .claude/hooks/sequencer_triage.py --classify "$1"
}

safe_classify_json() {
    classify_json "$1" 2>/dev/null || printf '{}'
}

reset_no_progress() {
    rm -f "$NO_PROGRESS_FILE" "$BLOCK_FILE"
}

block_supervisor() {
    local reason="$1"
    mkdir -p "$RUNTIME_BASE"
    {
        echo "blocked_at=$(date -Is)"
        echo "reason=$reason"
        echo "clear_with=rm -f $PROJECT_DIR/$BLOCK_FILE $PROJECT_DIR/$NO_PROGRESS_FILE"
    } > "$BLOCK_FILE"
    log "BLOCKED: $reason"
    log "watchdog relaunches will no-op until $BLOCK_FILE is removed"
    exit 0
}

record_no_progress() {
    local signature="$1"
    local reason="$2"
    local sig_hash prev_hash prev_count count
    mkdir -p "$RUNTIME_BASE"
    sig_hash="$(printf '%s' "$signature" | sha256sum | awk '{print $1}')"
    prev_hash=""
    prev_count=0
    if [ -f "$NO_PROGRESS_FILE" ]; then
        prev_hash="$(sed -n '1p' "$NO_PROGRESS_FILE" 2>/dev/null || true)"
        prev_count="$(sed -n '2p' "$NO_PROGRESS_FILE" 2>/dev/null || echo 0)"
    fi
    case "$prev_count" in
        ''|*[!0-9]*) prev_count=0 ;;
    esac
    if [ "$prev_hash" = "$sig_hash" ]; then
        count=$((prev_count + 1))
    else
        count=1
    fi
    {
        echo "$sig_hash"
        echo "$count"
        echo "$signature"
        echo "$reason"
    } > "$NO_PROGRESS_FILE"
    log "no section-level progress on $signature ($count/$NO_PROGRESS_LIMIT): $reason"
    if [ "$count" -ge "$NO_PROGRESS_LIMIT" ]; then
        block_supervisor "no section-level progress after $count bounded Codex step(s): $signature"
    fi
}

install_nested_codex_guard() {
    local dir="$1"
    local real_codex="$2"
    mkdir -p "$dir"
    {
        printf '%s\n' '#!/usr/bin/env bash'
        printf 'REAL_CODEX=%q\n' "$real_codex"
        cat <<'EOF'
if [ "${CODEX_REVIEWER_DISPATCH:-0}" = "1" ]; then
    exec "$REAL_CODEX" "$@"
fi
if [ "$#" -gt 0 ] && { [ "$1" = "--version" ] || [ "$1" = "app-server" ]; }; then
    echo "[codex-supervisor] nested codex CLI availability check blocked outside reviewer dispatch." >&2
    exit 125
fi
if [ "$#" -gt 0 ] && { [ "$1" = "exec" ] || [ "$1" = "e" ] || [ "$1" = "task" ]; }; then
    echo "[codex-supervisor] nested codex $1 calls are disabled inside the overnight Codex driver." >&2
    echo "[codex-supervisor] Use repo review wrappers such as scripts/codex-dispatch.sh for fresh reviewer runs." >&2
    exit 125
fi
if [ "$#" -ge 2 ] && { [ "$2" = "exec" ] || [ "$2" = "e" ] || [ "$2" = "task" ]; }; then
    echo "[codex-supervisor] nested codex $2 calls are disabled inside the overnight Codex driver." >&2
    echo "[codex-supervisor] Use repo review wrappers such as scripts/codex-dispatch.sh for fresh reviewer runs." >&2
    exit 125
fi
echo "[codex-supervisor] nested codex CLI calls are disabled inside the overnight Codex driver." >&2
echo "[codex-supervisor] Use repo review wrappers such as scripts/codex-dispatch.sh; do not spawn codex exec/task from the driver." >&2
exit 125
EOF
    } > "$dir/codex"
    chmod +x "$dir/codex"
}

run_codex_step() {
    local label="$1"
    local prompt="$2"
    local guard_dir
    guard_dir="$(mktemp -d "${TMPDIR:-/tmp}/codex-overnight-guard.XXXXXX")"
    install_nested_codex_guard "$guard_dir" "$CODEX_BIN"
    log "codex step: $label (timeout=${STEP_TIMEOUT}s)"
    set +o pipefail
    env \
        PATH="$guard_dir:$PATH" \
        OVERNIGHT_DRIVER=codex \
        OVERNIGHT_SEQUENCER_RUN=1 \
        AI_WORKFLOW_CODEX_DRIVER=1 \
        AI_WORKFLOW_ENFORCE_SHARED_GATES=1 \
        OVERNIGHT_STREAM_KEEP_UNKNOWN=1 \
        timeout "$STEP_TIMEOUT" "$CODEX_BIN" --ask-for-approval never exec --json \
            --cd "$PROJECT_DIR" \
            --sandbox "$CODEX_SANDBOX" \
            "$prompt" 2>&1 \
        | python3 "$SCRIPT_DIR/stream-report.py"
    local -a pipe_status=("${PIPESTATUS[@]}")
    set -o pipefail
    rm -rf "$guard_dir"
    local status="${pipe_status[0]}"
    local stream_status="${pipe_status[1]:-0}"
    if [ "$stream_status" -ne 0 ]; then
        log "stream-report exited non-zero: $stream_status"
        return "$stream_status"
    fi
    if [ "$status" -eq 124 ]; then
        record_no_progress "$label" "Codex step timed out after ${STEP_TIMEOUT}s"
        return 1
    fi
    if [ "$status" -ne 0 ]; then
        log "codex step exited non-zero: $status"
        return "$status"
    fi
}

bounded_intro() {
    cat <<EOF
You are Codex running as the active overnight steering driver for Impossible OS.
This is a bounded supervisor step, not a reviewer run and not a parallel skill tree.

Read AGENTS.md first, then CLAUDE.md, docs/infrastructure/ai-system.md,
todo/TODO-Claude-Overnight-Runner.md, docs/infrastructure/ai-driver-interchangeability.md,
and the target TODO before acting.

Hard limits for this step:
- Work only on the target named below.
- Do not continue to the next phase, file, or section after this target.
- Do not ask the operator.
- Do not run codex exec, codex task, or background Codex jobs from inside this driver step.
- Do not pass Codex model or reasoning flags.
- Keep output terse and operational.
- If blocked, record the exact blocker in the TODO using the repo's Deferred/XREF style, then stop.
EOF
}

validate_prompt() {
    local file="$1"
    cat <<EOF
$(bounded_intro)

Target phase: VALIDATE
Target file: $file

Run the validate-todo-file equivalent for this one file only:
1. Inspect the target file structure and the implementation-order table.
2. Run python3 scripts/todo-hygiene.py "$file".
3. Run bash scripts/todo-graph/build-and-validate.sh --keep-cache.
4. Fix structural TODO-file issues only.
5. When clean, add or refresh the file preamble line under the H1 and before Goal:
   > **Validated:** $(date +%F) | Codex supervisor validation; todo-hygiene and todo-graph clean
6. Stop immediately after validation. Do not gap-audit, implement, review, commit, or move to another file.
EOF
}

gap_prompt() {
    local file="$1"
    cat <<EOF
$(bounded_intro)

Target phase: GAP_AUDIT
Target file: $file

Run the gap-audit-todo equivalent for this one file only:
1. Inspect Inputs, Outcome, Implementation Order, section checklists, XREFs, and nearby TODO ownership.
2. Add concrete missing checklist items or XREFs in the target file only.
3. Do not implement code.
4. Run python3 scripts/todo-hygiene.py "$file" and bash scripts/todo-graph/build-and-validate.sh --keep-cache.
5. When clean, add or refresh the file preamble line under the H1 and before Goal:
   > **Gap-audited:** $(date +%F) | Codex supervisor gap audit; todo-graph clean
6. Stop immediately after the gap audit. Do not implement, review, commit, or move to another file.
EOF
}

preflight_prompt() {
    cat <<EOF
$(bounded_intro)

Target phase: PREFLIGHT
Target: repository baseline before TODO traversal

The deterministic preflight failed before the supervisor could enter TRIAGE.
Do one bounded baseline-repair pass only:
1. Run bash scripts/todo-graph/build-and-validate.sh --keep-cache.
2. Run bash scripts/build.sh and inspect tail -50 build/build.log if it fails.
3. Run bash scripts/test.sh QUIET=1 and rerun only the failing suite if needed.
4. Fix the root cause if it is a clear repository regression.
5. Do not implement a TODO section, do not stamp a section, and do not advance phases.
6. Stop after one repair attempt or after recording the blocker clearly in the report.
EOF
}

section_prompt() {
    local file="$1"
    local section="$2"
    local run_id="codex-overnight-$(date +%Y%m%d%H%M%S)-$$-s${section}"
    cat <<EOF
$(bounded_intro)

Target phase: SECTIONS
Target file: $file
Target section: $section
Driver run ID: $run_id

Ship or explicitly defer this one section only, following the same gates as the Claude section flow:
1. Start with:
   AI_WORKFLOW_CODEX_DRIVER=1 bash scripts/codex-driver.sh --live --todo "$file" --section "$section" --run-id "$run_id" --checkpoint pre-edit
2. Use the obligation output to choose the next missing action. Do not rerun satisfied work.
3. Use deterministic scripts for build/test/smoke/todo-graph validation and record evidence through scripts/ai-workflow/evidence.py where the workflow requires it.
4. Reviewer obligations must use fresh reviewer roles through repo review wrappers, not this driver run ID. Receive and classify findings as Fix, Reject, or Accept-XREF before applying fixes.
5. Use scripts/ai-workflow/stamp.py for generated stamps whenever it supports the needed stamp. Do not invent proof fields.
6. Before any commit attempt, run:
   AI_WORKFLOW_CODEX_DRIVER=1 bash scripts/codex-driver.sh --live --todo "$file" --section "$section" --run-id "$run_id" --checkpoint pre-commit
7. If the section is genuinely blocked, mark it [/] with a Deferred stamp and concrete XREF, release the lease, and stop.
8. Stop after this section is DONE/deferred or after the first hard gate failure. Do not start another section.
EOF
}

file_close_prompt() {
    local file="$1"
    cat <<EOF
$(bounded_intro)

Target phase: FILE_CLOSE
Target file: $file

Run the complete-todo-file equivalent for this one file only:
1. Verify every section is DONE or deferred with concrete XREFs.
2. Run python3 scripts/todo-hygiene.py "$file" and bash scripts/todo-graph/build-and-validate.sh --keep-cache.
3. Do only loose-end TODO-file cleanup required for closure.
4. Stop after this file-close pass. Do not advance to another TODO file.
EOF
}

smoke_prompt() {
    cat <<EOF
Codex running as the active overnight steering driver under the bounded supervisor smoke path.
Never ask the operator for permission or clarification during this unattended run.
Run exactly one harmless status command and then stop:
python3 .claude/hooks/run_phase_guard.py status
EOF
}

if [ -f "$BLOCK_FILE" ]; then
    log "blocked: $(tr '\n' ' ' < "$BLOCK_FILE")"
    exit 0
fi

if [ "${OVERNIGHT_CODEX_SUPERVISOR_SMOKE:-0}" = "1" ]; then
    run_codex_step "smoke" "$(smoke_prompt)"
    exit $?
fi

if [ "$MAX_STEPS" -lt 1 ]; then
    log "MAX_STEPS=$MAX_STEPS; nothing to do"
    exit 0
fi

for step in $(seq 1 "$MAX_STEPS"); do
    log "supervisor step $step/$MAX_STEPS"
    state="$(state_json)"
    active="$(printf '%s' "$state" | json_get active)"
    if [ "$active" != "true" ]; then
        run_guard start "$(date +%F)"
        reset_no_progress
        continue
    fi

    phase="$(printf '%s' "$state" | json_get phase)"
    file="$(printf '%s' "$state" | json_get file)"

    case "$phase" in
        PREFLIGHT)
            if run_preflight_checks; then
                run_guard phase TRIAGE
                reset_no_progress
            else
                run_codex_step "PREFLIGHT:baseline" "$(preflight_prompt)" || true
                if run_preflight_checks; then
                    run_guard phase TRIAGE
                    reset_no_progress
                else
                    record_no_progress "PREFLIGHT:baseline" "baseline build/test/todo-graph preflight still failing"
                fi
            fi
            ;;
        TRIAGE)
            run_logged bash scripts/todo-graph/build-and-validate.sh --keep-cache
            triage="$(python3 .claude/hooks/sequencer_triage.py --next)"
            log "triage: $triage"
            status="$(printf '%s' "$triage" | json_get status)"
            if [ "$status" = "DONE" ]; then
                run_guard fixpoint || run_guard next-pass
                reset_no_progress
                continue
            fi
            next_file="$(printf '%s' "$triage" | json_get file)"
            domain="$(printf '%s' "$triage" | json_get domain)"
            stages_done="$(printf '%s' "$triage" | json_get stages_1_2_done)"
            run_guard cursor "$domain" "$next_file"
            if [ "$stages_done" = "true" ]; then
                run_guard phase SECTIONS
            else
                class="$(safe_classify_json "$next_file")"
                validated="$(printf '%s' "$class" | lifecycle_flag validated)"
                if [ "$validated" = "true" ]; then
                    run_guard phase GAP_AUDIT
                else
                    run_guard phase VALIDATE
                fi
            fi
            reset_no_progress
            ;;
        VALIDATE)
            if [ -z "$file" ]; then
                run_guard phase TRIAGE
                continue
            fi
            before="$(safe_classify_json "$file")"
            before_validated="$(printf '%s' "$before" | lifecycle_flag validated)"
            if [ "$before_validated" = "true" ]; then
                run_guard phase GAP_AUDIT
                reset_no_progress
                continue
            fi
            run_codex_step "VALIDATE:$file" "$(validate_prompt "$file")" || true
            after="$(safe_classify_json "$file")"
            after_validated="$(printf '%s' "$after" | lifecycle_flag validated)"
            if [ "$after_validated" = "true" ]; then
                run_guard phase GAP_AUDIT
                reset_no_progress
            else
                record_no_progress "VALIDATE:$file" "validation stamp not present after bounded Codex step"
            fi
            ;;
        GAP_AUDIT)
            if [ -z "$file" ]; then
                run_guard phase TRIAGE
                continue
            fi
            before="$(safe_classify_json "$file")"
            before_gap="$(printf '%s' "$before" | lifecycle_flag gap_audited)"
            if [ "$before_gap" = "true" ]; then
                run_guard phase SECTIONS
                reset_no_progress
                continue
            fi
            run_codex_step "GAP_AUDIT:$file" "$(gap_prompt "$file")" || true
            after="$(safe_classify_json "$file")"
            after_gap="$(printf '%s' "$after" | lifecycle_flag gap_audited)"
            if [ "$after_gap" = "true" ]; then
                run_guard phase SECTIONS
                reset_no_progress
            else
                record_no_progress "GAP_AUDIT:$file" "gap-audit stamp not present after bounded Codex step"
            fi
            ;;
        SECTIONS)
            if [ -z "$file" ]; then
                run_guard phase TRIAGE
                continue
            fi
            before="$(safe_classify_json "$file")"
            fc="$(printf '%s' "$before" | file_class)"
            if [ "$fc" = "DONE" ]; then
                run_guard phase FILE_CLOSE
                reset_no_progress
                continue
            fi
            section="$(printf '%s' "$before" | first_needs_work_section)"
            if [ -z "$section" ]; then
                run_guard phase FILE_CLOSE
                reset_no_progress
                continue
            fi
            before_class="$(printf '%s' "$before" | section_class "$section")"
            run_codex_step "SECTIONS:$file:$section" "$(section_prompt "$file" "$section")" || true
            after="$(safe_classify_json "$file")"
            after_class="$(printf '%s' "$after" | section_class "$section")"
            after_file_class="$(printf '%s' "$after" | file_class)"
            if [ "$after_class" = "DONE" ] && [ "$before_class" != "DONE" ]; then
                run_guard progress
                reset_no_progress
            elif [ "$after_file_class" = "DONE" ]; then
                run_guard progress
                run_guard phase FILE_CLOSE
                reset_no_progress
            else
                record_no_progress "SECTIONS:$file:$section" "section remains $after_class after bounded Codex step"
            fi
            ;;
        FILE_CLOSE)
            if [ -z "$file" ]; then
                run_guard phase TRIAGE
                continue
            fi
            before="$(safe_classify_json "$file")"
            fc="$(printf '%s' "$before" | file_class)"
            if [ "$fc" != "DONE" ]; then
                run_guard phase SECTIONS
                continue
            fi
            run_codex_step "FILE_CLOSE:$file" "$(file_close_prompt "$file")" || true
            run_guard phase ADVANCE
            reset_no_progress
            ;;
        ADVANCE)
            log "advance complete for ${file:-unknown}; returning to TRIAGE"
            run_guard phase TRIAGE
            reset_no_progress
            ;;
        FIXPOINT)
            log "fixpoint already reached"
            exit 0
            ;;
        *)
            block_supervisor "unknown sequencer phase: $phase"
            ;;
    esac
done

log "step budget reached; watchdog will relaunch for the next bounded batch"
