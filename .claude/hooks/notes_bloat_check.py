#!/usr/bin/env python3
import json, sys, re
d = json.load(sys.stdin)
ti = d.get('tool_input', {})
tn = d.get('tool_name', '')
path = ti.get('file_path', '')
if not path.endswith('.md'):
    sys.exit(0)
p = path.replace('\\', '/')
if 'todo/' not in p and not p.startswith('todo/'):
    sys.exit(0)
texts = []
if tn == 'Write':
    texts.append(ti.get('content', ''))
elif tn == 'Edit':
    texts.append(ti.get('new_string', ''))
elif tn == 'MultiEdit':
    for e in ti.get('edits', []) or []:
        texts.append(e.get('new_string', ''))
if not texts:
    sys.exit(0)
BULLET_CHAR_CAP = 250
findings = []
for txt in texts:
    lines = txt.splitlines()
    in_block = False
    bullets = 0
    sub_bullets = 0
    long_bullets = []  # list of (preview, length)
    def flush():
        if bullets > 6 or sub_bullets > 0 or long_bullets:
            findings.append({'bullets': bullets, 'sub_bullets': sub_bullets,
                             'long': list(long_bullets)})
    for line in lines:
        if re.match(r'^>\s*\*\*Notes:\*\*', line):
            in_block = True
            bullets = 0
            sub_bullets = 0
            long_bullets = []
            continue
        if not in_block:
            continue
        if re.match(r'^>\s*\*\*(Verified|Accepted|Deferred|Quality reviewed):\*\*', line):
            flush()
            in_block = False
            continue
        if not line.startswith('>'):
            flush()
            in_block = False
            continue
        if re.match(r'^>\s+- ', line) and not re.match(r'^>\s{3,}- ', line):
            bullets += 1
            # Strip leading "> - " framing to measure the actual bullet content
            content = re.sub(r'^>\s+-\s+', '', line)
            if len(content) > BULLET_CHAR_CAP:
                preview = content[:80] + ('...' if len(content) > 80 else '')
                long_bullets.append((preview, len(content)))
        elif re.match(r'^>\s{3,}- ', line):
            sub_bullets += 1
    if in_block:
        flush()
if not findings:
    sys.exit(0)
parts = []
for f in findings:
    pieces = []
    if f['bullets'] > 6:
        pieces.append(str(f['bullets']) + ' bullets (max 6)')
    if f['sub_bullets'] > 0:
        pieces.append(str(f['sub_bullets']) + ' sub-bullet(s) (forbidden)')
    for prev, ln in f['long']:
        pieces.append('bullet ' + str(ln) + ' chars (cap ' + str(BULLET_CHAR_CAP) + '): ' + prev)
    parts.append('; '.join(pieces))
sys.stderr.write(
    '[Notes-block bloat BLOCK -- implement-todo-section step 10 / TODO Notes brevity discipline] '
    'Edit to ' + path + ' violates the Notes-block rule (3-6 short bullets, each <= ' + str(BULLET_CHAR_CAP) + ' chars, NO sub-bullets, NO multi-paragraph per-finding adoption blocks): ' +
    ' || '.join(parts) + '. ' +
    'Canonical shape: What shipped / How it runs / Downstream effects / Canonical doc / Scope boundary -- one bullet each, one logical line each. ' +
    'Adoption details (Codex finding evidence, file:line citations, per-dispatch breakdowns) belong in commit messages, NOT in Notes. ' +
    'See feedback_todo_notes_brevity memory and the implement-todo-section skill step 10 grammar. ' +
    'Trim to canonical shape, then retry.'
)
sys.exit(2)
