#!/usr/bin/env python3
import json, sys, re
d = json.load(sys.stdin)
ti = d.get('tool_input', {})
tn = d.get('tool_name', '')
path = ti.get('file_path', '')
if not path:
    sys.exit(0)
p = path.replace('\\', '/')
if not p.endswith('.md'):
    sys.exit(0)
parts = p.split('/')
if 'todo' not in parts:
    sys.exit(0)
texts = []
if tn == 'Write':
    texts.append(ti.get('content', ''))
elif tn == 'Edit':
    texts.append(ti.get('new_string', ''))
elif tn == 'MultiEdit':
    for e in ti.get('edits', []) or []:
        texts.append(e.get('new_string', ''))
all_text = '\n'.join(texts)
new_section = re.search(r'(?m)^##\s+\d', all_text)
new_file = (tn == 'Write')
if not (new_section or new_file):
    sys.exit(0)
msg = '[scope-gap dedup CHECK] Adding a new TODO section or file at ' + path + '. If this is a scope-gap Branch B/C/D invocation, confirm you ran the dedup sweep across all other TODO files BEFORE creating this section. Branch D (add to existing TODO) is preferred over Branch C (new TODO file) when any existing TODO covers the scope. A duplicate TODO is expensive to unwind -- the dedup sweep is the entire point.'
print(json.dumps({'systemMessage': msg}))
