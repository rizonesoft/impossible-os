# Code Style Policies

> Two related policies that share a single failure mode (bytes that look right in source but render as mojibake on Windows or rot in source code over time): no Unicode dashes, and no bare `section` references in code comments. CLAUDE.md carries one-line rules pointing here.

## No Unicode Dashes (en/em): ASCII Only

Do not use Unicode **en dash** (U+2013) or **em dash** (U+2014) in source files, comments, strings, docs, or scripts. Windows serial and CMD/PowerShell often garble those bytes as mojibake (for example `ΓÇö`).

### Prose and explanatory comments

Do not paste two ASCII hyphens (`--`) where an em dash would go; that is not clearer. It looks like minus or `--flag` noise. **Rewrite** the sentence (colon, semicolon, parentheses, comma pair, or two short sentences). Example: prefer `/* Redzone: detect underflow */` over `/* ... -- detect underflow */`.

### Where ASCII `--` stays correct

Technical uses readers already parse as non-prose:

- markdown `---` separators
- decrement operator in docs
- long options in command examples
- search patterns
- ranges where house style already uses double hyphen

### Scope

This applies to `.c`, `.h`, `.md`, `.sh`, `.ps1`, `.bat`, `.conf`, and all other tracked files.

## Comments: No Bare Section Refs in Code

Do not write bare "section-sign + number" references in source code comments, headers, assembly, tests, or tooling scripts. A comment like `/* capability negotiation */` carries its meaning forever; a comment with the section-sign glyph and just a number goes stale the moment the owning TODO renumbers its sections, and even before then it names no TODO and no domain so a reader has no way to re-anchor it.

### Rule

In any file that is NOT `.md`, NOT inside `todo/`, and NOT inside `.claude/`, the section-sign glyph followed by a digit is only legal when the same line carries an external-spec qualifier that points at a stable published standard:

UEFI, Intel, SDM, AMD, RFC `<n>`, ACPI `<n>`, NTFS, FAT32/16, NVMe, PCI, PCIe, PE/COFF, PE32, USB `<n>`, xHCI / EHCI / OHCI / UHCI, VirtIO, SMBIOS, IEEE, NIST, TCG, WHEA, HPET, MP Spec, the literal word "spec " or "specification".

Anything else is a bare reference and must be rewritten.

### Fix options (pick one)

- Rewrite the comment to name the feature (`/* capability negotiation */`, `/* xHCI BIOS -> OS handover */`).
- Prefix with an external-spec qualifier (`/* UEFI 2.10 section 4.6 */`).
- Replace with a markdown-doc link (`/* see docs/boot/boot-info-fields.md#capability-negotiation */`).

### Enforcement

`scripts/lint.sh` Check 5 + a PreToolUse hook in `.claude/settings.json`. The hook blocks edits at save-time so CI never rejects; the lint check is the full-repo backstop. Current legacy violations are warn-listed; any NEW file or any unlisted file must not introduce bare section refs. Legacy cleanup is tracked as a follow-up item in `todo/00-infrastructure/TODO-01-developer-tooling-stack.md` Tooling Doctor section.
