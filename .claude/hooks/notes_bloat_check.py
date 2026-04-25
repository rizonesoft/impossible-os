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
findings = []
for txt in texts:
    lines = txt.splitlines()
    in_block = False
    bullets = 0
    sub_bullets = 0
    def flush():
        if bullets > 6 or sub_bullets > 0:
            findings.append({'bullets': bullets, 'sub_bullets': sub_bullets})
    for line in lines:
        if re.match(r'^>\s*\*\*Notes:\*\*', line):
            in_block = True
            bullets = 0
            sub_bullets = 0
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
    parts.append(', '.join(pieces))
sys.stderr.write(
    '[Notes-block bloat BLOCK -- implement-todo-section step 10 / TODO Notes brevity discipline] '
    'Edit to ' + path + ' violates the Notes-block rule (3-6 short bullets, NO sub-bullets, NO multi-paragraph per-finding adoption blocks): ' +
    '; '.join(parts) + '. ' +
    'Canonical shape: What shipped / How it runs / Downstream effects / Canonical doc / Scope boundary -- one bullet each, one logical line each. ' +
    'Adoption details (Codex finding evidence, file:line citations, per-dispatch breakdowns) belong in commit messages, NOT in Notes. ' +
    'See feedback_todo_notes_brevity memory and the implement-todo-section skill step 10 grammar. ' +
    'Trim to canonical shape, then retry.'
)
sys.exit(2)
