#!/usr/bin/env python3
"""Per-function STACK-FRAME ceiling for the TPM modules.

WHY THIS EXISTS
---------------
The TPM subsystem builds TPM2 command and response buffers as ordinary locals,
and those buffers are sized by protocol maxima (a sealed blob's priv+pub is 416
bytes, an EK/AK public area 768) rather than by anything the calling context
knows about. They stack up along chains three and four frames deep:

    tpm_unseal_secret -> tpm2_seq_run -> unseal_secret_seq
                      -> tpm_policy_session_run_seq -> seal_unseal_op

against KERNEL TASK STACKS OF 8 KiB with a guard page at the bottom. A frame
that grows past its budget does not fail a build, a test, or a QEMU boot -- it
takes out a guard page on real hardware, on whichever path happens to be deepest
when it happens.

A one-off measurement cannot hold that line, which is the point of the acceptance
clause this implements: the ceiling has to be CHECKED, and it has to fail
something a person will notice.

THE RATCHET
-----------
Two rules, both enforced here:

  1. No function in a covered module may exceed CEILING_BYTES.
  2. A function listed in BASELINE is a PRE-EXISTING exception. It may shrink,
     and it may not GROW. Each one names the section that owns bringing it under
     the ceiling.

Rule 2 is what makes this landable without a subsystem-wide refactor. A flat
ceiling with no exceptions would have to be set above the worst frame in the
tree, which is the same as not having one; a ratchet binds every new function
immediately and squeezes the old ones monotonically.

Static locals do not count toward a frame, which is the intended repair: a buffer
whose lifetime is genuinely one-at-a-time (guarded by the transport sequence
gate, which admits ONE sequence at a time for the whole subsystem) belongs off
the stack, and moving it there is what took seal_secret_seq from 1128 to 360.

USAGE
    python3 scripts/tpm-stack-check.py            # check, exit 1 on violation
    python3 scripts/tpm-stack-check.py --report   # print every frame, exit 0

Requires clang-19. When it is absent the check reports SKIP and exits 0: a gate
that cannot run proves nothing, and saying so is better than a false green.
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Modules whose frames are governed. Chosen because these are the ones that
# marshal protocol-sized buffers; adding a module here is cheap and is the right
# move the moment it grows a multi-hundred-byte local.
MODULES = [
    "src/kernel/tpm_seal.c",
    "src/kernel/tpm_attest.c",
    "src/kernel/tpm_nv.c",
    "src/kernel/tpm_baseline.c",
    "src/kernel/tpm_transport.c",
]

# One frame may spend at most this much of an 8 KiB kernel task stack. 1 KiB
# leaves room for a four-deep chain of at-ceiling frames plus interrupt frames
# and the guard page, which is the deepest shape these modules actually form. It
# is deliberately a ROUND, defensible number rather than one fitted to the
# current worst frame -- a ceiling derived from today's code cannot fail.
CEILING_BYTES = 1024

# Pre-existing exceptions: may shrink, may never grow. Each names its owner.
# Exact values rather than a fudge factor, so any movement at all is visible.
BASELINE = {
    "tpm_baseline_verify": (
        1320, "owner: the authenticated-baseline verified-read chain "
              "(tpm_baseline.c)"),
    "nv_policy_op_cb": (
        1272, "owner: the NV bounded-sequence policy seam (tpm_nv.c)"),
    "tpm_baseline_enroll_unauthenticated": (
        1272, "owner: the authenticated-baseline enroll path (tpm_baseline.c)"),
    "tpm_baseline_enroll_bound": (
        888, "owner: the authenticated-baseline enroll path (tpm_baseline.c)"),
    "nv_define_seq": (
        952, "owner: the NV bounded-sequence define flow (tpm_nv.c)"),
}

CFLAGS = [
    "--target=x86_64-elf", "-Wall", "-Wextra", "-ffreestanding", "-nostdlib",
    "-nostdinc", "-fstack-protector-strong", "-mstack-protector-guard=global",
    "-fno-pie", "-mno-red-zone", "-mno-mmx", "-mno-sse", "-mno-sse2",
    "-fno-omit-frame-pointer", "-mcmodel=kernel", "-std=gnu11", "-O2",
]

SU_LINE = re.compile(r"^(?P<file>[^\t]+):(?P<line>\d+):(?P<fn>[^\t]+)\t"
                     r"(?P<bytes>\d+)\t(?P<kind>\S+)")


def measure(cc, workdir):
    """Compile each module with -fstack-usage; return [(fn, bytes, file, line)]."""
    frames = []
    for rel in MODULES:
        src = os.path.join(REPO, rel)
        if not os.path.exists(src):
            print("tpm-stack-check: MISSING %s" % rel, file=sys.stderr)
            return None
        obj = os.path.join(workdir, os.path.basename(rel) + ".o")
        proc = subprocess.run(
            [cc] + CFLAGS + ["-I", os.path.join(REPO, "include"),
                             "-fstack-usage", "-c", src, "-o", obj],
            cwd=workdir, capture_output=True, text=True)
        if proc.returncode != 0:
            print("tpm-stack-check: compile failed for %s:\n%s" % (rel, proc.stderr),
                  file=sys.stderr)
            return None
        su = os.path.splitext(obj)[0] + ".su"
        if not os.path.exists(su):
            print("tpm-stack-check: no .su produced for %s" % rel, file=sys.stderr)
            return None
        with open(su) as fh:
            for raw in fh:
                m = SU_LINE.match(raw.rstrip("\n"))
                if m:
                    frames.append((m.group("fn"), int(m.group("bytes")),
                                   os.path.relpath(m.group("file"), REPO),
                                   int(m.group("line"))))
    return frames


def main():
    report = "--report" in sys.argv
    cc = shutil.which("clang-19") or shutil.which("clang")
    if not cc:
        # A check that cannot run proves nothing. Say so and pass; the CI image
        # and the dev host both have clang-19, so this is the unusual case.
        print("tpm-stack-check: SKIP -- clang-19 not found")
        return 0

    with tempfile.TemporaryDirectory(prefix="tpm-su-") as workdir:
        frames = measure(cc, workdir)
    if frames is None:
        print("tpm-stack-check: FAIL -- could not measure stack usage")
        return 1

    if report:
        for fn, nbytes, path, line in sorted(frames, key=lambda f: -f[1]):
            print("%6d  %s:%d:%s" % (nbytes, path, line, fn))
        return 0

    violations, shrunk = [], []
    for fn, nbytes, path, line in frames:
        if fn in BASELINE:
            allowed, owner = BASELINE[fn]
            if nbytes > allowed:
                violations.append(
                    "%s:%d: %s grew to %d bytes, over its recorded baseline of "
                    "%d (%s)" % (path, line, fn, nbytes, allowed, owner))
            elif nbytes < allowed:
                shrunk.append("%s: %d -> %d" % (fn, allowed, nbytes))
        elif nbytes > CEILING_BYTES:
            violations.append(
                "%s:%d: %s uses %d bytes of stack, over the %d-byte TPM ceiling. "
                "Move the largest buffer off the stack (a static guarded by the "
                "sequence gate) or split the function; do NOT raise the ceiling."
                % (path, line, fn, nbytes, CEILING_BYTES))

    # A shrink is reported but never fails: tightening BASELINE is a deliberate
    # edit, and failing a build for improving the code would be perverse.
    for line in shrunk:
        print("tpm-stack-check: baseline slack (safe to tighten) %s" % line)

    if violations:
        print("tpm-stack-check: FAIL -- %d violation(s)" % len(violations))
        for v in violations:
            print("  %s" % v)
        return 1

    worst = max(frames, key=lambda f: f[1])
    print("tpm-stack-check: OK -- %d frames, ceiling %d, worst %d (%s)"
          % (len(frames), CEILING_BYTES, worst[1], worst[0]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
