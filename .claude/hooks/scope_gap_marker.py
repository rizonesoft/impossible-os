#!/usr/bin/env python3
import json, sys, re
d = json.load(sys.stdin)
ti = d.get('tool_input', {})
tn = d.get('tool_name', '')
path = ti.get('file_path', '')
if not path:
    sys.exit(0)
p = path.replace('\\', '/')
allow_dirs = ('src/kernel/', 'include/kernel/', 'src/boot/', 'src/desktop/', 'src/shell/', 'user/', 'src/apps/')
if not any(d in p for d in allow_dirs):
    sys.exit(0)
if not p.endswith(('.c', '.h', '.asm', '.S', '.cpp')):
    sys.exit(0)
if 'src/kernel/test/' in p:
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
all_text = '\n'.join(texts)
if 'SCOPE-GAP-ALLOWED' in all_text:
    sys.exit(0)
patterns = [
    (r'//\s*TODO\b', '// TODO comment'),
    (r'//\s*FIXME\b', '// FIXME comment'),
    (r'//\s*HACK\b', '// HACK comment'),
    (r'//\s*XXX\b', '// XXX comment'),
    (r'/\*\s*TODO\b', '/* TODO block comment'),
    (r'/\*\s*FIXME\b', '/* FIXME block comment'),
    (r'/\*\s*HACK\b', '/* HACK block comment'),
    (r'/\*\s*XXX\b', '/* XXX block comment'),
    (r'//\s*for now\b', '// for now comment'),
    (r'//\s*placeholder\b', '// placeholder comment'),
    (r'//\s*stub\b', '// stub comment'),
    (r'\bSTATUS_NOT_IMPLEMENTED\b', 'STATUS_NOT_IMPLEMENTED return'),
    (r'\bE_NOTIMPL\b', 'E_NOTIMPL return'),
    (r'\bkernel_unimplemented\b', 'kernel_unimplemented call'),
    (r'\bSTUB\s*\(', 'STUB( macro'),
]
hits = []
for pat, name in patterns:
    if re.search(pat, all_text):
        hits.append(name)
if not hits:
    sys.exit(0)
sys.stderr.write('[scope-gap protocol REQUIRED] Detected scope-gap marker(s) in ' + path + ': ' + ', '.join(hits) + '. ')
sys.stderr.write('You are about to ship a workaround/stub instead of resolving the gap. STOP and walk the scope-gap protocol decision tree in implement-todo-section step 6 (see .claude/skills/implement-todo-section/scope-gap-protocol.md): Branch A (inline expansion under 1000 lines), Branch B (new section in same TODO), Branch C (new TODO via /create-todo after dedup sweep), or Branch D (add to existing TODO). The paper trail belongs in the TODO file, not in source comments. ')
sys.stderr.write('If this is a TRUE false positive (test code probing unimplemented path, sentinel for hardware-not-yet-supported, etc.), add a /* SCOPE-GAP-ALLOWED: <one-line reason> */ comment to opt out -- the sentinel is searchable for periodic audit.')
sys.exit(2)
