#!/usr/bin/env python3
import json, sys, os, subprocess, re
from pathlib import Path
# Post-ship review gate: if HEAD commit flips a TODO section to [x] but
# does NOT add a **Verified:** stamp in the same commit, the next
# non-gathering tool call MUST be Skill(review-todo-section).
# See feedback_never_skip_review memory.

# TODO-08 section-23: shared SKIP-env scanner so inline opt-outs
# (`SKIP_REVIEW_HOOK=1 git commit ...`) reach this hook the same way
# they reach section_commit_gate. The harness env-only path was a
# real friction point during the TODO-02 section-11 review-pipeline
# closure (commit 3bc47f6f).
_HOOK_DIR = Path(__file__).resolve().parent
if str(_HOOK_DIR) not in sys.path:
    sys.path.insert(0, str(_HOOK_DIR))
import _skip_env as _se  # noqa: E402
import _review_pipeline_passthrough as _rpp  # noqa: E402

d = json.load(sys.stdin)
tn = d.get('tool_name', '')
ti = d.get('tool_input', {})

# Opt-out -- read from BOTH inline cmd env-prefix AND os.environ.
_cmd_for_skip = ti.get('command', '') if tn == 'Bash' else ''
if _se.read_skip_envs(_cmd_for_skip, keys=('SKIP_REVIEW_HOOK',)).get('SKIP_REVIEW_HOOK') == '1':
    sys.exit(0)

# Info-gathering tools always pass (the review itself needs them)
if tn in ('Read', 'Grep', 'Glob'):
    sys.exit(0)

# The review skill itself passes, plus skills it explicitly delegates to.
# `superpowers:receiving-code-review` is mandated by review-todo-section
# step 5 / 6 / 8 to handle Codex findings -- blocking it here is
# self-defeating. The other entries cover step 7 domain code-quality
# gates and step 15.5 verification-before-completion.
_REVIEW_PIPELINE_SKILLS = {
    'review-todo-section',
    'superpowers:receiving-code-review',
    'superpowers:verification-before-completion',
    'kernel-code-quality', 'boot-code-quality', 'desktop-code-quality',
    'shell-code-quality', 'userland-code-quality',
}
if tn == 'Skill' and (ti.get('skill', '') in _REVIEW_PIPELINE_SKILLS or
                      ti.get('name', '') in _REVIEW_PIPELINE_SKILLS):
    sys.exit(0)


def _review_pipeline_active(repo_root):
    """Return True iff review-todo-section is the active multi-step skill
    in this session and was started after the HEAD commit (the user
    invoked review AFTER the section-ship commit landed -- exactly the
    window where step-13 fixes need to write to src/include/todo).
    """
    try:
        state_p = Path(repo_root) / '.claude/state/skill-progress.json'
        if not state_p.exists():
            return False
        with state_p.open() as fh:
            state = json.load(fh)
    except Exception:
        return False
    rts = state.get('review-todo-section')
    if not isinstance(rts, dict):
        return False
    try:
        sess_p = Path(repo_root) / '.claude/state/session.json'
        cur_sid = json.loads(sess_p.read_text()).get('session_id', '')
    except Exception:
        cur_sid = ''
    if cur_sid and rts.get('session_id') and rts.get('session_id') != cur_sid:
        return False
    try:
        head_ts_str = subprocess.check_output(
            ['git', '-C', repo_root, 'log', '-1', '--format=%ct'],
            text=True, timeout=2, stderr=subprocess.DEVNULL).strip()
        head_ts_ns = int(head_ts_str) * 1_000_000_000
    except Exception:
        return False
    rts_ts_ns = rts.get('started_ts') or 0
    return isinstance(rts_ts_ns, int) and rts_ts_ns >= head_ts_ns


# When a review-todo-section invocation is currently active, allow
# Edit / Write / MultiEdit on the implementation surface -- review
# step 13 ("Fix ALL findings") is the legitimate write path between
# commit-without-stamp and commit-with-stamp.
if tn in ('Edit', 'Write', 'MultiEdit'):
    file_path = ti.get('file_path', '') or ''
    try:
        _root_for_pipeline = subprocess.check_output(
            ['git', 'rev-parse', '--show-toplevel'],
            text=True, timeout=2, stderr=subprocess.DEVNULL).strip()
    except Exception:
        _root_for_pipeline = ''
    if (_root_for_pipeline and _review_pipeline_active(_root_for_pipeline)
            and any(seg in file_path for seg in ('/src/', '/include/',
                                                  '/todo/', '/scripts/',
                                                  '/docs/'))):
        sys.exit(0)

# Git operations + script wrappers + codex dispatches pass via the
# shared review-pipeline-passthrough helper. Section-29 of TODO-08
# extracted this list from this file. Other gates with different
# threat models MUST NOT inherit this list -- it permits broad code
# execution (node, python3, bash scripts/) which is safe for the
# review pipeline but not for write-sensitive contexts.
if tn == 'Bash':
    cmd = ti.get('command', '').strip()
    # Pass through if the whole pipeline is review-allowed, or if the
    # FIRST segment of an `&&` / `;` / `||` chain is. The previous
    # `'&&' in cmd[:5]` was vestigial -- `&&` cannot legally appear in
    # the first 5 chars of any command.
    if _rpp.is_review_pipeline_passthrough(cmd):
        sys.exit(0)
    for sep in ('&&', ';', '||'):
        if sep in cmd:
            first_seg = cmd.split(sep, 1)[0].strip()
            if first_seg and _rpp.is_review_pipeline_passthrough(first_seg):
                sys.exit(0)
            break

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

# Detect [x] flip in the Implementation Order table: an added line carrying
# "| [x] |" whose row is genuinely NEW -- not the same row re-padded.
#
# The second half used to be only a comment. The test was a bare search for an
# added "| [x] |" line, so ANY diff touching a row that already read [x] fired
# the gate. A whitespace-only table realignment rewrites every row in the
# table, [x] rows included, and so demanded a section review for a commit that
# changed no status at all (measured 2026-08-07 on 9c92ef889, a 56-file column
# realignment: zero cell-content drift, gate fired anyway).
#
# A row that appears on BOTH sides of the diff with the same content modulo
# inter-cell whitespace is reformatting, not a flip. Compare normalized forms:
# a real "[ ] -> [x]" flip still differs (the status cell changed), and a
# brand-new row landing as [x] has no counterpart on the minus side at all.
def _norm_row(line):
    return re.sub(r'\s+', ' ', line[1:]).strip()

_added_x = [l for l in diff.splitlines()
            if l.startswith('+') and re.search(r'\|\s*\[x\]\s*\|', l)]
_removed = {_norm_row(l) for l in diff.splitlines() if l.startswith('-')}
has_x_flip = any(_norm_row(l) not in _removed for l in _added_x)
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
