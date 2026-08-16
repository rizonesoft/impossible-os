#!/usr/bin/env python3
# v14 close-out (2026-08-16): `receiving_review_required` gated only
# Edit/Write/MultiEdit, so applying review fixes through
# `python3 - <<'PY' ... open(p,"w")` heredocs bypassed it -- observed four
# times across TODO-06 sections 39 and 48, each time because an awkward edit
# (regex escapes, BOM/CRLF literals, whole-function rewrites) pushed an honest
# session toward Bash. The hook now also matches Bash and gates only
# file-MUTATING commands. These pin the detector in BOTH directions: the
# observed evasion shapes must match, and read-only Bash must never match --
# a chatty gate trains reflexive overrides, which is the failure the gate
# exists to prevent.
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[2] / ".claude/hooks"))
import receiving_review_required as rrr  # noqa: E402

FAILS = []


def check(name, cond):
    if not cond:
        FAILS.append(name)


MUST_MATCH = [
    # the observed evasion shape, verbatim class
    "python3 - <<'PY'\np = 'src/kernel/panic.c'\nopen(p,'w').write(body)\nPY",
    'python3 -c "open(\'x.c\', \'w\').write(s)"',
    "python3 - <<'PY'\npathlib.Path('a.md').write_text(s)\nPY",
    "python3 - <<'PY'\nopen(p, 'a').write(line)\nPY",
    # heredoc body with a `;` before the write (must not be mis-split)
    "python3 - <<'PY'\nx=1; open(p,'w').write(s)\nPY",
    "sed -i 's/a/b/' src/kernel/panic.c",
    "sed -r -i.bak 's/a/b/' file.c",
    "echo done > magic",
    "cat notes >> todo/file.md",
    "echo x | tee scripts/out.txt",
    # F2: the named mutators the deny-list used to miss
    "printf x | dd of=file.c",
    "cp a.c b.c",
    "mv a.c b.c",
    "git apply p.patch",
    "patch < p.diff",
    "truncate -s0 f.c",
    "install -m644 a b",
    "ln -sf a b",
    'python3 -c "import os; os.rename(1,2)"',
    'python3 -c "import shutil; shutil.copyfile(a,b)"',
    "git checkout -- src/a.c",
    "git restore src/a.c",
]

MUST_NOT_MATCH = [
    # F3: read-only search tools whose ARGUMENT contains a write-looking string
    "grep -rn 'open(' src/kernel/",
    "grep -F \"open(p, 'w')\" src/a.py",
    "grep -F '.write_text(' src/a.py",
    "rg \"os.rename(\" src/",
    "git status --porcelain",
    "bash scripts/build.sh",
    "python3 scripts/overnight/ci-check.py .",
    "make test-mm 2>&1 | tail -5",
    "echo progress > /tmp/ship-push.log",
    "bash scripts/test.sh QUIET=1 2>/dev/null",
    "cmd 2>&1 | head",
    "git diff HEAD~1 -- src/",
    "git log -1 --format=%s",
    # arrows in prose/code are not redirects
    "grep -n 'a -> b' file.md",
    "python3 -c \"print('x => y')\"",
    "echo x | tee /tmp/log.txt",
    # read-mode open in executed code is not a write
    "python3 -c \"print(open('f.c').read())\"",
    "python3 -c \"print(open('f.c', 'r').read())\"",
    # the sanctioned ship-push shape (redirect to /tmp only)
    "( git push origin main > /tmp/ship-push.log 2>&1; echo rc ) &",
    # run-artifact wrapper
    "bash scripts/overnight/run-artifact.sh build",
]

for cmd in MUST_MATCH:
    check(f"matches: {cmd[:60]!r}", rrr._bash_mutates_files(cmd))
for cmd in MUST_NOT_MATCH:
    check(f"passes: {cmd[:60]!r}", not rrr._bash_mutates_files(cmd))

if FAILS:
    print("test_receiving_bash_route: FAIL")
    for f in FAILS:
        print(f"  - {f}")
    sys.exit(1)
print(f"test_receiving_bash_route: OK ({len(MUST_MATCH)+len(MUST_NOT_MATCH)} checks)")
