#!/usr/bin/env python3
import json, sys, re
d = json.load(sys.stdin)
ti = d.get('tool_input', {})
tn = d.get('tool_name', '')
path = ti.get('file_path', '')
if not path:
    sys.exit(0)
p = path.replace('\\', '/')
# Markdown gets anchor-link enforcement via Check 4, not this hook.
if p.endswith('.md'):
    sys.exit(0)
# Deliberate shorthand zones.
if '/todo/' in p or p.startswith('todo/') or '.claude/' in p:
    sys.exit(0)
# Path-based spec-code exemption (whole-file implements an external standard).
# Mirrors scripts/lint.sh is_bare_section_spec_code. Extend when adding a new
# spec-code directory.
spec_code_prefixes = (
    'src/kernel/fs/ntfs/',   # NTFS on-disk format spec.
)
spec_code_exact = (
    'include/kernel/fs/ntfs.h',
)
# Normalize: strip leading slash + repo-root-ish prefixes so matching works
# whether the path is absolute or relative.
norm = p
for prefix in ('/home/derickpayne/impossible-os/', './'):
    if norm.startswith(prefix):
        norm = norm[len(prefix):]
        break
if any(norm.startswith(pp) for pp in spec_code_prefixes):
    sys.exit(0)
if norm in spec_code_exact:
    sys.exit(0)
# Enforce only on code-ish files.
if not p.endswith(('.c', '.h', '.asm', '.S', '.py', '.sh', '.bat', '.ps1', '.yml', '.yaml', '.mk')):
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
bare_re = re.compile('§[0-9]')
spec_re = re.compile(r'UEFI|Intel|SDM|AMD|APM|RFC [0-9]|ACPI [0-9]|NTFS|FAT[0-9]|NVMe|PCIe?|PE/COFF|PE32|COFF|USB [0-9]|xHCI|EHCI|OHCI|UHCI|VirtIO|SMBIOS|IEEE|NIST|TCG|WHEA|HPET|MP Spec|spec |specification')
hits = []
for line in all_text.splitlines():
    if not bare_re.search(line):
        continue
    if spec_re.search(line):
        continue
    hits.append(line.strip()[:80])
if not hits:
    sys.exit(0)
sys.stderr.write(
    '[bare section ref BLOCK -- scripts/lint.sh Check 5] ' +
    'Detected bare section-sign reference in code file ' + path + ': ' +
    ' | '.join(hits[:3]) + '. ' +
    'Code comments must name the FEATURE (e.g. "capability negotiation", "typed payload descriptors"), not a bare section number. ' +
    'Section numbers drift silently on TODO renumber and carry no TODO/domain context. ' +
    'External-spec citations stay legal when the line also carries a qualifier (UEFI, Intel, SDM, AMD, RFC <n>, ACPI <n>, NTFS, FAT<n>, NVMe, PCI/PCIe, PE/COFF, PE32, USB <n>, xHCI/EHCI/OHCI/UHCI, VirtIO, SMBIOS, IEEE, NIST, TCG, WHEA, HPET, MP Spec, "spec ", "specification"), and whole-file spec-code dirs (src/kernel/fs/ntfs/) are path-exempted. ' +
    'Fix: rewrite the comment to name the feature; OR prefix with an external-spec qualifier (e.g. "UEFI 2.10 section 4.6"); OR replace with a markdown-doc link. ' +
    'Same pattern scripts/lint.sh Check 5 enforces at CI time; this hook catches it at edit time so CI never rejects. ' +
    'See feedback_no_bare_section_refs_in_code memory and CLAUDE.md "Comments -- No Bare Section Refs in Code".'
)
sys.exit(2)
