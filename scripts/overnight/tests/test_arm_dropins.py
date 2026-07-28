#!/usr/bin/env python3
"""arm-sequencer.sh drop-in writers must RETURN 0, not merely write the file.

THE BUG THIS PINS (2026-07-28). `write_sequencer_model_dropin` ended each loop
iteration with a trailing conditional:

    [ -n "$ARM_EFFORT" ] && echo "Environment=OVERNIGHT_EFFORT=$ARM_EFFORT"

That was the LAST command in a `{ ... } > file` group. With ARM_EFFORT empty --
the DEFAULT, and what `--effort inherit` produces -- the test is false, `&&`
short-circuits, and the group exits 1. The status propagates group -> loop body
-> `for` -> function return, and `set -euo pipefail` then killed the script AT
THE CALL SITE, one line before its stage marker advanced.

Why it went unexplained across three arms (2026-07-20, -24, -28) and got
documented in the script as "root cause still unknown": the function DOES write
both files and DOES complete. Only its return status is fatal, and only under
`set -e`. Every investigation saw the files present and blamed the next
function.

Why it mattered: the dead tail skipped `write_claude_token_dropin`, so the run
fell back to the shared ~/.claude/.credentials.json and re-entered the
single-use-refresh-token race the long-lived token exists to end -- the #1
historical run-killer, silently re-armed on every arm for eight days.

The test therefore asserts the thing that actually broke -- the RETURN STATUS
under `set -e` -- not just that a file appeared. A test that only checked file
contents would have passed throughout the entire outage.
"""
from __future__ import annotations

import os
import subprocess
import sys
import tempfile

ARM = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                   "..", "..", "..",
                   ".claude/skills/overnight-sequencer/arm-sequencer.sh")
ARM = os.path.normpath(ARM)

# Sourcing only the writer keeps this independent of the rest of the script
# (which arms systemd units). The marker after the call is the whole point: it
# is printed only if execution SURVIVED the function under `set -e`.
_HARNESS = r"""
set -euo pipefail
TMPD=$(mktemp -d)
UNIT=u
DROPIN_BASE="$TMPD"
ARM_PRIMARY="%(primary)s"
ARM_FALLBACK="%(fallback)s"
ARM_EFFORT="%(effort)s"
source <(sed -n '/^%(fn)s()/,/^}/p' "%(arm)s")
%(fn)s
echo "SURVIVED"
cat "$TMPD/u.service.d/%(conf)s" 2>/dev/null || true
rm -rf "$TMPD"
"""


def _run(fn, conf, primary="opus", fallback="sonnet", effort=""):
    script = _HARNESS % {"fn": fn, "conf": conf, "arm": ARM, "primary": primary,
                         "fallback": fallback, "effort": effort}
    p = subprocess.run(["bash", "-c", script], capture_output=True, text=True)
    return p.returncode, p.stdout


def main() -> int:
    fails = []

    def check(name, cond, extra=""):
        if not cond:
            fails.append(f"{name}{(' -- ' + extra) if extra else ''}")

    # THE REGRESSION: empty ARM_EFFORT is the default and must not kill the run.
    rc, out = _run("write_sequencer_model_dropin", "sequencer-model.conf", effort="")
    check("empty-effort returns 0", rc == 0, f"rc={rc}")
    check("empty-effort survives set -e", "SURVIVED" in out)
    check("empty-effort still writes the model pin",
          "OVERNIGHT_MODEL=opus" in out and "OVERNIGHT_FALLBACK_MODEL=sonnet" in out)
    check("empty-effort omits the effort line", "OVERNIGHT_EFFORT" not in out)

    # An explicit effort is the path that ALWAYS worked -- keep it working.
    rc, out = _run("write_sequencer_model_dropin", "sequencer-model.conf", effort="high")
    check("explicit-effort returns 0", rc == 0, f"rc={rc}")
    check("explicit-effort writes the effort line", "OVERNIGHT_EFFORT=high" in out)

    # An empty PRIMARY is the same latent shape one reorder away from fatal.
    rc, out = _run("write_sequencer_model_dropin", "sequencer-model.conf",
                   primary="", effort="")
    check("empty-primary returns 0", rc == 0, f"rc={rc}")
    check("empty-primary omits the model line", "OVERNIGHT_MODEL=" not in out)
    check("empty-primary still writes the fallback",
          "OVERNIGHT_FALLBACK_MODEL=sonnet" in out)

    # The sibling writers are on the same critical path; a trailing conditional
    # introduced into any of them reproduces the same silent outage.
    for fn, conf in (("write_sequencer_env_dropin", "sequencer-run.conf"),
                     ("write_chromemcp_dropin", "no-chromemcp.conf")):
        rc, out = _run(fn, conf)
        check(f"{fn} returns 0", rc == 0, f"rc={rc}")
        check(f"{fn} survives set -e", "SURVIVED" in out)

    if fails:
        print("test_arm_dropins FAIL:")
        for f in sorted(set(fails)):
            print(f"  - {f}")
        return 1
    print("test_arm_dropins OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
