---
schema_version: 1
id: imported-programs
domain: 11-apps
status: active
title: "TODO-15 -- Imported Programs and Parts"
---

# TODO-15 -- Imported Programs and Parts

> **Goal:** Every program and part that is developed in its own repository, and has no roadmap file of its own here, arrives on Impossible OS and is not forgotten.

> [!NOTE]
> A program is written in its own repository, proved on Windows first, and waits for nothing here; nothing here waits for it either. An import is taken when what it depends on is there. A section here is one source of [`todo/imports.json`](../imports.json); `scripts/imports.py` brings it and keeps its pin. Programs that have a roadmap file here are imported by a section of that file.

## Inputs

- [`todo/imports.json`](../imports.json) and `scripts/imports.py`: the ledger, `--pull` and `--propose`
- `../10-platform-services/TODO-07-win32-pe-loader.md`: runs the programs
- -> XREF: `08-graphics-ui/TODO-19` §27 -- the interface the windowed programs are drawn with

## Outcome

- Each program starts from the Start menu and passes its own tests on the OS.
- What had to change here went back to its repository.

## Implementation Order

| ⭐  | Order | Deliverable | Depends On | Status |
| --- | :---: | --- | --- | :----: |
| 💎 | 1 | §1 Import Impossible SDK | -- | [ ] |
| 💎 | 2 | §2 Impossible SDK's parts here | §1 | [ ] |
| 💎 | 3 | §3 Import Command | §1, `10-platform-services/TODO-07` §7 (PE programs run), `10-platform-services/TODO-13` §1 | [ ] |
| 💎 | 4 | §4 Import Character Map | §1, `10-platform-services/TODO-07` §7 (PE programs run), `08-graphics-ui/TODO-19` §27, `10-platform-services/TODO-13` §3 | [ ] |
| 💎 | 5 | §5 Import Chess | §1, `10-platform-services/TODO-07` §7 (PE programs run), `08-graphics-ui/TODO-19` §27, `10-platform-services/TODO-13` §2 | [ ] |
| 💎 | 6 | §6 Import Clock | §1, `10-platform-services/TODO-07` §7 (PE programs run), `08-graphics-ui/TODO-19` §27, `10-platform-services/TODO-13` §2 | [ ] |
| 💎 | 7 | §7 Import Markdown | §1, `10-platform-services/TODO-07` §7 (PE programs run), `08-graphics-ui/TODO-19` §27, `10-platform-services/TODO-13` §3 | [ ] |
| 💎 | 8 | §8 Import Compare | §1, `10-platform-services/TODO-07` §7 (PE programs run), `08-graphics-ui/TODO-19` §27, `10-platform-services/TODO-13` §3 | [ ] |
| 💎 | 9 | §9 Import Disk Map | §1, `10-platform-services/TODO-07` §7 (PE programs run), `08-graphics-ui/TODO-19` §27, `10-platform-services/TODO-13` §3 | [ ] |
| 💎 | 10 | §10 Import Icon Editor | §1, `10-platform-services/TODO-07` §7 (PE programs run), `08-graphics-ui/TODO-19` §27, `10-platform-services/TODO-13` §3 | [ ] |
| 💎 | 11 | §11 Import Image Writer | §1, `10-platform-services/TODO-07` §7 (PE programs run), `08-graphics-ui/TODO-19` §27, `10-platform-services/TODO-13` §6 | [ ] |
| 💎 | 12 | §12 Import Minesweeper | §1, `10-platform-services/TODO-07` §7 (PE programs run), `08-graphics-ui/TODO-19` §27, `10-platform-services/TODO-13` §2 | [ ] |
| 💎 | 13 | §13 Import Music | §1, `10-platform-services/TODO-07` §7 (PE programs run), `08-graphics-ui/TODO-19` §27, `10-platform-services/TODO-13` §4 | [ ] |
| 💎 | 14 | §14 Import Hex Editor | §1, `10-platform-services/TODO-07` §7 (PE programs run), `08-graphics-ui/TODO-19` §27, `10-platform-services/TODO-13` §3 | [ ] |
| 💎 | 15 | §15 Import Perihelion | §1, `10-platform-services/TODO-07` §7 (PE programs run), `08-graphics-ui/TODO-19` §27, `10-platform-services/TODO-13` §2 | [ ] |
| 💎 | 16 | §16 Import Rename | §1, `10-platform-services/TODO-07` §7 (PE programs run), `08-graphics-ui/TODO-19` §27, `10-platform-services/TODO-13` §3 | [ ] |
| 💎 | 17 | §17 Import Solitaire | §1, `10-platform-services/TODO-07` §7 (PE programs run), `08-graphics-ui/TODO-19` §27, `10-platform-services/TODO-13` §2 | [ ] |
| 💎 | 18 | §18 Import Sound Recorder | §1, `10-platform-services/TODO-07` §7 (PE programs run), `08-graphics-ui/TODO-19` §27, `10-platform-services/TODO-13` §4 | [ ] |
| 💎 | 19 | §19 Import Sudoku | §1, `10-platform-services/TODO-07` §7 (PE programs run), `08-graphics-ui/TODO-19` §27, `10-platform-services/TODO-13` §2 | [ ] |
| 💎 | 20 | §20 Import Weather | §1, `10-platform-services/TODO-07` §7 (PE programs run), `08-graphics-ui/TODO-19` §27, `10-platform-services/TODO-13` §5 | [ ] |

---

## 1. Import Impossible SDK

**Design:** n/a -- imports code, it designs nothing

**Import:** impossible-sdk

Impossible SDK is developed in its own repository and proved on Windows. This section brings what every program is built with: its headers, its Windows layer, its build and the list of Windows functions a program may call.

- [ ] Copy it with `python3 scripts/imports.py --pull impossible-sdk` into `src/libs/impossible-sdk/`
- [ ] Build it here and run its tests
- [ ] Show that no import of a built program is outside `abi/tiers.json`, and that this OS provides each function of the tiers its programs use
- [ ] Propose what had to change here with `python3 scripts/imports.py --propose impossible-sdk`
- [ ] Commit: `"libs: Impossible SDK imported"`

## 2. Impossible SDK's parts here

**Design:** n/a -- imports code, it designs nothing

Impossible SDK is developed in its own repository and proved on Windows. They arrive with §1. This section builds and tests here its formats, network and web readers, each a part without system calls where it can be.

- [ ] Build each part with the flags of this kernel and of user programs, and run its tests
- [ ] Show that no import of a built program is outside `abi/tiers.json`, and that this OS provides each function of the tiers its programs use
- [ ] Propose what had to change here with `python3 scripts/imports.py --propose impossible-sdk`
- [ ] Commit: `"libs: Impossible SDK's parts imported"`

## 3. Import Command

**Design:** n/a -- imports a program, it designs nothing

**Import:** command

Command is developed in its own repository and proved on Windows. This section brings its program and tests here and runs them on the OS.

- [ ] Copy the pinned program with `python3 scripts/imports.py --pull command` into `src/apps/command/`
- [ ] Build it in the OS build and run its engine tests there
- [ ] Run each of its console programs in the terminal here, and compare its output and exit code with Windows for every case of its tests
- [ ] Propose what had to change here with `python3 scripts/imports.py --propose command`
- [ ] Commit: `"apps: Command imported"`

## 4. Import Character Map

**Design:** n/a -- imports a program, it designs nothing

**Import:** character-map

Character Map is developed in its own repository and proved on Windows. This section brings its program and tests here and runs them on the OS.

- [ ] Copy the pinned program with `python3 scripts/imports.py --pull character-map` into `src/apps/character-map/`
- [ ] Build it in the OS build and run its engine tests there
- [ ] Open Character Map on the desktop and walk it by pointer and by keyboard
- [ ] Propose what had to change here with `python3 scripts/imports.py --propose character-map`
- [ ] Commit: `"apps: Character Map imported"`

## 5. Import Chess

**Design:** n/a -- imports a program, it designs nothing

**Import:** chess

Chess is developed in its own repository and proved on Windows. This section brings its program and tests here and runs them on the OS.

- [ ] Copy the pinned program with `python3 scripts/imports.py --pull chess` into `src/apps/chess/`
- [ ] Build it in the OS build and run its engine tests there
- [ ] Open Chess on the desktop and walk it by pointer and by keyboard
- [ ] Propose what had to change here with `python3 scripts/imports.py --propose chess`
- [ ] Commit: `"apps: Chess imported"`

## 6. Import Clock

**Design:** n/a -- imports a program, it designs nothing

**Import:** clock

Clock is developed in its own repository and proved on Windows. This section brings its program and tests here and runs them on the OS.

- [ ] Copy the pinned program with `python3 scripts/imports.py --pull clock` into `src/apps/clock/`
- [ ] Build it in the OS build and run its engine tests there
- [ ] Open Clock on the desktop and walk it by pointer and by keyboard
- [ ] Propose what had to change here with `python3 scripts/imports.py --propose clock`
- [ ] Commit: `"apps: Clock imported"`

## 7. Import Markdown

**Design:** n/a -- imports a program, it designs nothing

**Import:** colophon

Markdown is developed in its own repository and proved on Windows. This section brings its program and tests here and runs them on the OS.

- [ ] Copy the pinned program with `python3 scripts/imports.py --pull colophon` into `src/apps/colophon/`
- [ ] Build it in the OS build and run its engine tests there
- [ ] Open Markdown on the desktop and walk it by pointer and by keyboard
- [ ] Propose what had to change here with `python3 scripts/imports.py --propose colophon`
- [ ] Commit: `"apps: Markdown imported"`

## 8. Import Compare

**Design:** n/a -- imports a program, it designs nothing

**Import:** compare

Compare is developed in its own repository and proved on Windows. This section brings its program and tests here and runs them on the OS.

- [ ] Copy the pinned program with `python3 scripts/imports.py --pull compare` into `src/apps/compare/`
- [ ] Build it in the OS build and run its engine tests there
- [ ] Open Compare on the desktop and walk it by pointer and by keyboard
- [ ] Propose what had to change here with `python3 scripts/imports.py --propose compare`
- [ ] Commit: `"apps: Compare imported"`

## 9. Import Disk Map

**Design:** n/a -- imports a program, it designs nothing

**Import:** disk-map

Disk Map is developed in its own repository and proved on Windows. This section brings its program and tests here and runs them on the OS.

- [ ] Copy the pinned program with `python3 scripts/imports.py --pull disk-map` into `src/apps/disk-map/`
- [ ] Build it in the OS build and run its engine tests there
- [ ] Open Disk Map on the desktop and walk it by pointer and by keyboard
- [ ] Propose what had to change here with `python3 scripts/imports.py --propose disk-map`
- [ ] Commit: `"apps: Disk Map imported"`

## 10. Import Icon Editor

**Design:** n/a -- imports a program, it designs nothing

**Import:** icon-editor

Icon Editor is developed in its own repository and proved on Windows. This section brings its program and tests here and runs them on the OS.

- [ ] Copy the pinned program with `python3 scripts/imports.py --pull icon-editor` into `src/apps/icon-editor/`
- [ ] Build it in the OS build and run its engine tests there
- [ ] Open Icon Editor on the desktop and walk it by pointer and by keyboard
- [ ] Propose what had to change here with `python3 scripts/imports.py --propose icon-editor`
- [ ] Commit: `"apps: Icon Editor imported"`

## 11. Import Image Writer

**Design:** n/a -- imports a program, it designs nothing

**Import:** imprint

Image Writer is developed in its own repository and proved on Windows. This section brings its program and tests here and runs them on the OS.

- [ ] Copy the pinned program with `python3 scripts/imports.py --pull imprint` into `src/apps/imprint/`
- [ ] Build it in the OS build and run its engine tests there
- [ ] Open Image Writer on the desktop and walk it by pointer and by keyboard
- [ ] Propose what had to change here with `python3 scripts/imports.py --propose imprint`
- [ ] Commit: `"apps: Image Writer imported"`

## 12. Import Minesweeper

**Design:** n/a -- imports a program, it designs nothing

**Import:** minesweeper

Minesweeper is developed in its own repository and proved on Windows. This section brings its program and tests here and runs them on the OS.

- [ ] Copy the pinned program with `python3 scripts/imports.py --pull minesweeper` into `src/apps/minesweeper/`
- [ ] Build it in the OS build and run its engine tests there
- [ ] Open Minesweeper on the desktop and walk it by pointer and by keyboard
- [ ] Propose what had to change here with `python3 scripts/imports.py --propose minesweeper`
- [ ] Commit: `"apps: Minesweeper imported"`

## 13. Import Music

**Design:** n/a -- imports a program, it designs nothing

**Import:** music

Music is developed in its own repository and proved on Windows. This section brings its program and tests here and runs them on the OS.

- [ ] Copy the pinned program with `python3 scripts/imports.py --pull music` into `src/apps/music/`
- [ ] Build it in the OS build and run its engine tests there
- [ ] Open Music on the desktop and walk it by pointer and by keyboard
- [ ] Propose what had to change here with `python3 scripts/imports.py --propose music`
- [ ] Commit: `"apps: Music imported"`

## 14. Import Hex Editor

**Design:** n/a -- imports a program, it designs nothing

**Import:** nibble

Hex Editor is developed in its own repository and proved on Windows. This section brings its program and tests here and runs them on the OS.

- [ ] Copy the pinned program with `python3 scripts/imports.py --pull nibble` into `src/apps/nibble/`
- [ ] Build it in the OS build and run its engine tests there
- [ ] Open Hex Editor on the desktop and walk it by pointer and by keyboard
- [ ] Propose what had to change here with `python3 scripts/imports.py --propose nibble`
- [ ] Commit: `"apps: Hex Editor imported"`

## 15. Import Perihelion

**Design:** n/a -- imports a program, it designs nothing

**Import:** perihelion

Perihelion is developed in its own repository and proved on Windows. This section brings its program and tests here and runs them on the OS.

- [ ] Copy the pinned program with `python3 scripts/imports.py --pull perihelion` into `src/apps/perihelion/`
- [ ] Build it in the OS build and run its engine tests there
- [ ] Open Perihelion on the desktop and walk it by pointer and by keyboard
- [ ] Propose what had to change here with `python3 scripts/imports.py --propose perihelion`
- [ ] Commit: `"apps: Perihelion imported"`

## 16. Import Rename

**Design:** n/a -- imports a program, it designs nothing

**Import:** rename

Rename is developed in its own repository and proved on Windows. This section brings its program and tests here and runs them on the OS.

- [ ] Copy the pinned program with `python3 scripts/imports.py --pull rename` into `src/apps/rename/`
- [ ] Build it in the OS build and run its engine tests there
- [ ] Open Rename on the desktop and walk it by pointer and by keyboard
- [ ] Propose what had to change here with `python3 scripts/imports.py --propose rename`
- [ ] Commit: `"apps: Rename imported"`

## 17. Import Solitaire

**Design:** n/a -- imports a program, it designs nothing

**Import:** solitaire

Solitaire is developed in its own repository and proved on Windows. This section brings its program and tests here and runs them on the OS.

- [ ] Copy the pinned program with `python3 scripts/imports.py --pull solitaire` into `src/apps/solitaire/`
- [ ] Build it in the OS build and run its engine tests there
- [ ] Open Solitaire on the desktop and walk it by pointer and by keyboard
- [ ] Propose what had to change here with `python3 scripts/imports.py --propose solitaire`
- [ ] Commit: `"apps: Solitaire imported"`

## 18. Import Sound Recorder

**Design:** n/a -- imports a program, it designs nothing

**Import:** sound-recorder

Sound Recorder is developed in its own repository and proved on Windows. This section brings its program and tests here and runs them on the OS.

- [ ] Copy the pinned program with `python3 scripts/imports.py --pull sound-recorder` into `src/apps/sound-recorder/`
- [ ] Build it in the OS build and run its engine tests there
- [ ] Open Sound Recorder on the desktop and walk it by pointer and by keyboard
- [ ] Propose what had to change here with `python3 scripts/imports.py --propose sound-recorder`
- [ ] Commit: `"apps: Sound Recorder imported"`

## 19. Import Sudoku

**Design:** n/a -- imports a program, it designs nothing

**Import:** sudoku

Sudoku is developed in its own repository and proved on Windows. This section brings its program and tests here and runs them on the OS.

- [ ] Copy the pinned program with `python3 scripts/imports.py --pull sudoku` into `src/apps/sudoku/`
- [ ] Build it in the OS build and run its engine tests there
- [ ] Open Sudoku on the desktop and walk it by pointer and by keyboard
- [ ] Propose what had to change here with `python3 scripts/imports.py --propose sudoku`
- [ ] Commit: `"apps: Sudoku imported"`

## 20. Import Weather

**Design:** n/a -- imports a program, it designs nothing

**Import:** weather

Weather is developed in its own repository and proved on Windows. This section brings its program and tests here and runs them on the OS.

- [ ] Copy the pinned program with `python3 scripts/imports.py --pull weather` into `src/apps/weather/`
- [ ] Build it in the OS build and run its engine tests there
- [ ] Open Weather on the desktop and walk it by pointer and by keyboard
- [ ] Propose what had to change here with `python3 scripts/imports.py --propose weather`
- [ ] Commit: `"apps: Weather imported"`

---

## OS Comparison

| ⭐  | Feature                           | 🪟 Win11           | 🐧 Linux             | 🚀 Impossible OS     |
| --- | --------------------------------- | ------------------ | -------------------- | -------------------- |
| 💎  | Games, tools and media programs   | ✅ Inbox and Store | ✅ Per desktop       | ⬜ Planned -- §3-§20 |
| ⭐  | One source for the OS and Windows | ❌                 | ⚠️ Toolkit dependent | ⬜ Planned -- §1-§20 |

## Verification

- [ ] `python3 scripts/imports.py --check` passes and `--list` shows a commit for every source of this file
- [ ] Each imported program opens on the desktop, and its engine tests pass on the OS
