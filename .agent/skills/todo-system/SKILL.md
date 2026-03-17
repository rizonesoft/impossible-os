---
name: todo-system
description: How to navigate, read, and update the Impossible OS TODO system
---

# TODO System Navigation

## Finding the Right TODO

1. **Start at the index:** `todo/TODO-000-INDEX.md` — master table of all TODOs by layer
2. **Folder prefixes match TODO numbers:** `010-Kernel-Foundations/` → TODOs 010–059
3. **Use Srclight:** `search_symbols("your topic")` to find relevant TODO sections

## Folder Structure

| Prefix | Layer | Examples |
|---|---|---|
| `000-` | Infrastructure | Build system, tooling, agent config |
| `010-` | Kernel Foundations | Bootloader, threading, filesystem, registry |
| `060-` | Hardware & Drivers | Keyboard, mouse, power management |
| `110-` | GFX & UI Framework | UI controls, theme, animation |
| `160-` | Desktop Shell | Taskbar, start menu, boot splash |
| `230-` | Core Services | Clipboard, search, security |
| `310-` | Core Apps | Terminal, file manager, notepad |
| `380-` | Multimedia | Audio, paint |
| `400-` | Networking | Browser, FTP, SSH, email |
| `460-` | Polish & Extras | DPI, screensaver, widgets |
| `510-` | Long-Term Stretch | Win32, Linux compat, SDK |

## TODO File Anatomy

Each TODO file follows this structure:

```markdown
## N. Section Title              ← numbered section
### N.M Subsection *(status)* ✅  ← status emoji: ⏳ 🔄 ✅

**Prompt:** Instructions for the agent...  ← what to do + verification steps

> [!IMPORTANT]                    ← cross-references, prerequisites
> → XREF: `TODO-NNN §M.M`

- [ ] Task item                   ← unchecked
- [/] Task in progress            ← in progress (custom notation)
- [x] Completed task              ← done
- [ ] Commit: `"scope: msg"`      ← final commit instruction
```

## Cross-References (XREFs)

Format: `→ XREF: \`TODO-NNN-Name.md §M.M\` — brief reason`

Add XREFs when:
- A section depends on another section being completed first
- Two sections share implementation (e.g., boot_info used by both bootloader and kernel)
- A change in one section affects another

## Marking Items Complete

1. Change `- [ ]` to `- [x]` for completed items
2. Update section header: add `✅` and remove `⏳`
3. Update the verification **Prompt** to confirm-and-verify language
4. Commit with the message specified in the TODO's commit item

## Commit Message Convention

Format: `"scope: short description"`

| Scope | When |
|---|---|
| `agent:` | TODO updates, agent config, skills, rules |
| `kernel:` | Kernel code changes |
| `boot:` | Bootloader changes |
| `gfx:` | Graphics/UI framework |
| `specs:` | Spec documents |
| `docs:` | Documentation |

## DO NOT

- **Don't create duplicate TODOs** — always check the index first
- **Don't renumber existing TODOs** — gaps are intentional (allows inserting)
- **Don't edit TODOs in other layers** unless adding an XREF
- **Don't remove completed items** — keep them checked off for history
