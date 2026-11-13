#!/bin/bash
# test_exit_reason_table.sh -- the reserved exit-reason table enforces itself
#
# include/kernel/sched/task.h generates the reserved exit reasons, their count,
# _LAST, the per-row ordinal checks and the classifier bounds from ONE X-macro
# table (TASK_EXIT_REASONS). These mutants prove the table is the only thing to
# edit and that a wrong edit cannot build:
#   - a valid new row builds, classifies as a kernel reason (not a signal or an
#     application status), is named, and moves the count and _LAST with it;
#   - a duplicate ordinal, a gap, a reordered row and a duplicate name each fail
#     to compile;
#   - no reserved reason is defined outside the table.
# Each mutant is a copy of task.h in a temporary include root ahead of the real
# one, compiled and run on the host.
set -u
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TASK_H="$REPO_ROOT/include/kernel/sched/task.h"
CC="${CC_HOST:-clang-19}"
pass=0; fail=0
ok()  { echo "PASS $1"; pass=$((pass + 1)); }
bad() { echo "FAIL $1${2:+ -- $2}"; fail=$((fail + 1)); }

work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
mkdir -p "$work/inc/kernel/sched"

cat > "$work/probe.c" <<'C'
#include "kernel/sched/task.h"
#include "kernel/types.h"
static int same(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
int main(void)
{
#ifdef NEW_REASON
    if (!task_exit_is_kernel_reason(TASK_EXIT_UTEST_PROBE)) return 10;
    if (task_exit_is_signal(TASK_EXIT_UTEST_PROBE)) return 11;
    if (task_exit_is_app_status(TASK_EXIT_UTEST_PROBE)) return 12;
    if (TASK_EXIT_UTEST_PROBE != TASK_EXIT_REASON_BASE - 6) return 13;
    if (TASK_EXIT_REASON_COUNT != 6 || TASK_EXIT_REASON_LAST != TASK_EXIT_UTEST_PROBE) return 14;
    if (!task_exit_reason_name(TASK_EXIT_UTEST_PROBE)
        || !same(task_exit_reason_name(TASK_EXIT_UTEST_PROBE), "TASK_EXIT_UTEST_PROBE")) return 15;
#endif
    if (!task_exit_reason_name(TASK_EXIT_UTEST_TIMEOUT)
        || !same(task_exit_reason_name(TASK_EXIT_UTEST_TIMEOUT), "TASK_EXIT_UTEST_TIMEOUT")) return 20;
    if (task_exit_reason_name(-9)) return 21;
    if (!task_exit_is_app_status(TASK_EXIT_REASON_LAST - 1)) return 22;
    return 0;
}
C

# mutant <name> <python edit of the table body>: writes the copy, prints it.
mutant() {
    python3 - "$TASK_H" "$work/inc/kernel/sched/task.h" "$1" <<'PY'
import sys
src, dst, kind = sys.argv[1:4]
s = open(src).read()
last = "    X(UTEST_STALLED,        5)\n"
assert s.count(last) == 1, "the table's last row moved; update this test"
edits = {
    "baseline": s,
    "new-row": s.replace(last, last[:-1] + "                                                \\\n    X(UTEST_PROBE,          6)\n"),
    "dup-ordinal": s.replace(last, last[:-1] + "                                                \\\n    X(UTEST_PROBE,          5)\n"),
    "gap": s.replace(last, last[:-1] + "                                                \\\n    X(UTEST_PROBE,          7)\n"),
    "reorder": s.replace("    X(UTEST_IDENTITY,       2)", "    X(UTEST_IDENTITY,       3)").replace("    X(UTEST_TIMEOUT,        3)", "    X(UTEST_TIMEOUT,        2)"),
    "dup-name": s.replace(last, last[:-1] + "                                                \\\n    X(UTEST_TIMEOUT,        6)\n"),
}
out = edits[kind]
assert kind == "baseline" or out != s, f"{kind}: the edit did not apply"
open(dst, "w").write(out)
PY
}

build() {  # build [-DNEW_REASON]
    "$CC" -ffreestanding -fno-builtin "$@" -I "$work/inc" -I "$REPO_ROOT/include" \
        -o "$work/probe" "$work/probe.c" >"$work/cc.log" 2>&1
}

mutant baseline && build && "$work/probe" && ok "baseline table builds and classifies" \
    || bad "baseline table" "$(head -2 "$work/cc.log")"

mutant new-row && build -DNEW_REASON
if [ $? -ne 0 ]; then
    bad "a valid new row builds" "$(head -2 "$work/cc.log")"
else
    "$work/probe"; rc=$?
    [ "$rc" -eq 0 ] && ok "a valid new row builds, classifies as a kernel reason, is named, moves COUNT and _LAST" \
        || bad "a valid new row classifies wrongly" "probe exit $rc"
fi

for m in dup-ordinal gap reorder; do
    mutant "$m"
    if build; then bad "$m fails to build" "it compiled"
    elif grep -q 'its ordinal is not its 1-based row' "$work/cc.log"; then ok "$m fails to build, naming the ordinal check"
    else bad "$m fails for the wrong reason" "$(grep -m1 error: "$work/cc.log")"; fi
done

mutant dup-name
if build; then bad "a duplicate name fails to build" "it compiled"
else ok "a duplicate name fails to build"; fi

# No reserved reason outside the table: a #define in the reserved block's
# namespace computed from the base, anywhere in the tree.
strays=$(grep -rnE '^[[:space:]]*#[[:space:]]*define[[:space:]]+TASK_EXIT_[A-Z0-9_]+[[:space:]]+\(?[[:space:]]*TASK_EXIT_REASON_BASE' \
    "$REPO_ROOT/include" "$REPO_ROOT/src" | grep -v 'TASK_EXIT_REASON_LAST' || true)
[ -z "$strays" ] && ok "no reserved exit reason is defined outside TASK_EXIT_REASONS" \
    || bad "a reserved exit reason is defined outside TASK_EXIT_REASONS" "$(head -2 <<<"$strays")"

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
