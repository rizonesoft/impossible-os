#!/usr/bin/env python3
import json, sys, os, subprocess, re
# Post-ship review gate: if HEAD commit flips a TODO section to [x] but
# does NOT add a **Verified:** stamp in the same commit, the next
# non-gathering tool call MUST be Skill(review-todo-section).
# See feedback_never_skip_review memory.
d = json.load(sys.stdin)
tn = d.get('tool_name', '')
ti = d.get('tool_input', {})

# Opt-out
if os.environ.get('SKIP_REVIEW_HOOK', '') == '1':
    sys.exit(0)

# Info-gathering tools always pass (the review itself needs them)
if tn in ('Read', 'Grep', 'Glob'):
    sys.exit(0)

# The review skill itself passes
if tn == 'Skill' and (ti.get('skill', '') == 'review-todo-section' or
                      ti.get('name', '') == 'review-todo-section'):
    sys.exit(0)

# Git operations + script wrappers + codex dispatches pass
# (the review pipeline runs git diff / lint / tests / codex, all via Bash)
if tn == 'Bash':
    cmd = ti.get('command', '').strip()
    if (cmd.startswith(('git ', 'bash scripts/', 'node ', 'python3 ',
                        'grep ', 'awk ', 'sed ', 'wc ', 'head ', 'tail ',
                        'ls ', 'cat ', 'find ', 'rg '))
        or cmd.startswith('cd ') or '&&' in cmd[:5]):
        sys.exit(0)

# Look up repo root (hook runs with CWD=cwd of the parent tool call)
try:
    root = subprocess.check_output(['git','rev-parse','--show-toplevel'],
                                    text=True, timeout=2,
                                    stderr=subprocess.DEVNULL).strip()
except Exception:
    sys.exit(0)
if not root:
    sys.exit(0)

# HEAD commit message + diff
try:
    msg = subprocess.check_output(['git','-C',root,'log','-1','--format=%s'],
                                   text=True, timeout=3).strip()
    diff = subprocess.check_output(['git','-C',root,'show','HEAD','--','todo/'],
                                    text=True, timeout=5,
                                    stderr=subprocess.DEVNULL)
except Exception:
    sys.exit(0)

# Commits that are themselves the review, or pure todo/doc edits, skip
msg_low = msg.lower()
if msg_low.startswith(('review:', 'todo:', 'docs:', 'merge', 'revert', 'fix:')):
    sys.exit(0)

if not diff.strip():
    sys.exit(0)

# Detect [x] flip in the Implementation Order table: added line with
# "| [x] |", and the same region had "[ ]" or "[/]" before.
has_x_flip = bool(re.search(r'^\+.*\|\s*\[x\]\s*\|', diff, re.M))
if not has_x_flip:
    sys.exit(0)

# If the same commit ADDED a **Verified:** stamp, review was done
# in-commit (review-todo-section already ran or was inline). No gate.
if re.search(r'^\+\s*>\s*\*\*Verified:\*\*', diff, re.M):
    sys.exit(0)

# Extract TODO path from diff header
m = re.search(r'^\+\+\+ b/(todo/[^\s]+\.md)', diff, re.M)
todo_path = m.group(1) if m else 'todo/...'
# Section number from commit msg
m = re.search(r'\xa7(\d+)|section\s+(\d+)', msg)
sec = (m.group(1) or m.group(2)) if m else '?'

sys.stderr.write(
    '[review-todo-section REQUIRED -- post-ship gate] ' +
    'HEAD commit (' + msg[:60] + ') stamps a TODO section [x] but does NOT carry the ' +
    '**Verified:** / **Quality reviewed:** stamps. The /review-todo-section skill MUST run ' +
    'before any Edit / Write / non-review Skill / non-script Bash can fire. ' +
    'Next tool call: Skill(skill="review-todo-section", args="' + todo_path + ' \xa7' + sec + ' <title>"). ' +
    'Allowed while gated: Read / Grep / Glob (for info gathering), Bash with git / bash scripts / node / python3 / grep / awk / sed / wc / head / tail / ls / cat / find / rg / cd prefixes (for the review pipeline itself). ' +
    'Opt-out for legitimate false positives (revert commits, stamp-only edits, etc.): SKIP_REVIEW_HOOK=1 env var on the next tool call. ' +
    'Canonical rule: feedback_never_skip_review memory + CLAUDE.md "Mandatory Skill Triggers" row.'
)
sys.exit(2)
