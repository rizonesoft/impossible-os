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
- -> XREF: `11-apps/TODO-05-pdf-viewer.md` §21 -- Recto's one transfer onto the SDK build; it needs §21, §22 and §23 here before it replaces anything
- -> XREF: `11-apps/TODO-16-office.md` §3, `11-apps/TODO-17-isotone.md` §3 -- the programs that use the shared reproducible build check of §23 and the `cxx` part of §22
- `impossible-sdk` `docs/specs/2026-11-27-sdk-as-platform-design.md` -- the approved design these sections carry out

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
| ⭐ | 21 | §21 Moved rows carry the old text's hash | -- | [ ] |
| 💎 | 22 | §22 Impossible SDK's cxx part here | §1, `02-kernel-core/TODO-23` §5, §6, §9 | [ ] |
| ⭐ | 23 | §23 Reproducible build check for imported programs | §1, §22 | [ ] |

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

## 21. Moved Rows Carry the Old Text's Hash

> **Spawned-by:** root

**Design:** n/a -- a ledger field and its check, it draws nothing

A moved row accepts only `section`, `source` and `to` today (`ROW` in `scripts/imports.py`), so the ledger cannot record the text a section had when it moved. The 2026-11-23 ledger rule says a section moves only when its new item exists, with a ledger row and the hash of its old text; the approved design `impossible-sdk` `docs/specs/2026-11-27-sdk-as-platform-design.md` reconciles Recto's `11-apps/TODO-05-pdf-viewer.md` through one transfer that follows that rule. This section adds the field and its check, and it precedes every transfer: `11-apps/TODO-05-pdf-viewer.md` §21 does not start before it is done.

- [ ] Define the hash once, in `scripts/imports.py` as `old_text_hash()` with a docstring, and in the ledger table of `docs/infrastructure/todo-metadata.md`: SHA-256 as lowercase hex of the section's text as the check reads it from the staged tree
  - The text runs from the heading line to the line before the next `## ` heading or `---` rule, with LF line ends and trailing blank lines trimmed
  - It leaves out the `**Moved:**` line, the stamps (`**Verified:**`, `**Quality reviewed:**`, `**Deferred:**`) and checkbox state (`[x]` and `[/]` read as `[ ]`), so shipping or stamping a section does not change it
- [ ] Add `old` to the moved row shape (`ROW`, `load()`): a row without it fails with a named fault
- [ ] `python3 scripts/imports.py --hash-moved` fills `old` for every row that lacks it from the section's current text, and never changes a row that has it
  - Run it once over the existing rows in the same commit as the code
  - Document the writing of a new row (the ledger table, and every script that writes a moved row) with the field
- [ ] `check()` recomputes the hash for each moved row and refuses a difference, naming the section and both hashes
  - A section whose text a transfer replaced carries `**Old text:** <commit>`, the commit that last held the old text; the check then reads the old text from that commit with git, hashes it and compares it with `old`, so a replaced section stays provable
  - Changing the old text on purpose means writing the row again with the new hash in a commit that says why; an `**Old text:**` commit that does not hold the text fails the check
- [ ] Add tests to `scripts/tests/test_imports.py`, each failing before the change and passing after
  - A row with the right hash passes; one word changed in the old text, staged, is refused naming the section
  - Ticking a box, adding a stamp and adding the `**Moved:**` line do not change the hash
  - A row without `old` is refused; a section with `**Old text:** <commit>` is judged by that commit's text, and a wrong hash fails
- [ ] Commit: `"imports: moved rows carry the hash of the old text"`

**Test checkpoint:** `python3 scripts/tests/test_imports.py` passes with the new cases; `python3 scripts/imports.py --check` exits 0 over the real ledger with every moved row carrying `old`; editing one word of a moved section makes it exit 1 naming that section. Test on: WSL2 dev host.

## 22. Impossible SDK's cxx part here

> **Spawned-by:** root

**Design:** n/a -- a language runtime, it draws nothing

Impossible SDK's `cxx` part supplies the C++ runtime for user programs: LLVM libc++, libc++abi and libunwind built on the SDK's base and linked statically, with exceptions and RTTI on and no Microsoft runtime DLL. It arrives with §1. Impossible OS builds none of it (the work `11-apps/TODO-05-pdf-viewer.md` §11 planned moves here by the owner decision of 2026-11-27, no double work) and keeps only what the OS alone can do: the unwinder on its exception dispatch, the link from the compiler roadmap to the supported subset, and the tests of the runtime on this kernel.

- [ ] Build the imported `cxx` part for user mode with the flags of user programs, and run its own tests
- [ ] Wire the part's unwinder to this kernel's user-mode exception dispatch and the image's unwind tables (`02-kernel-core/TODO-23-exception-dispatch-seh.md` §5, §6, §9)
  - An exception thrown two frames up through a live object is caught and the object's destructor runs
- [ ] Link the SDK's supported C++ subset (`docs/guide/cxx.md` in Impossible SDK) from `10-platform-services/TODO-09-compiler-sdk.md` §11, which keeps on-device compilers
  - It is the one reference: write no `docs/sdk/cxx-runtime.md` of our own
- [ ] Run the whole of the old `11-apps/TODO-05-pdf-viewer.md` §11 checkpoint here: all six `user/tests/cxx/` tests under the user-mode launcher
  - Exceptions across frames, `std::thread` with `std::mutex`, `std::string`, `std::vector`, `std::format`, `std::expected`
- [ ] Propose what had to change here with `python3 scripts/imports.py --propose impossible-sdk`
- [ ] Commit: `"libs: Impossible SDK's cxx part imported"`

**Test checkpoint:** `user/tests/cxx/test_cxx.exe` prints `cxx: 6 of 6 passed` under the user-mode test launcher, and a thrown exception caught two frames up returns the expected value. Test on: QEMU TCG and KVM at 1 and 2 CPUs.

## 23. Reproducible Build Check for Imported Programs

> **Spawned-by:** root

**Design:** n/a -- a check over built files, it draws nothing

Programs built with the SDK toolchain are reproducible (no timestamps), so an executable built here must equal the one the program's release shipped. One check does this for every imported program (Office, Isotone and Recto), so no program writes its own. The record format belongs to the SDK (its `tools/app_build.py` and the program's `app.json`); this section reads it and defines none.

- [ ] Read each executable's recorded SHA-256 from the program's release record, in the format the SDK defines, with one reader here
- [ ] `python3 scripts/imports.py --repro <source>` compares every executable the OS build wrote under `build/apps/<name>/` with its recorded hash, and exits 1 naming the executable, the expected hash and the actual one on any difference
  - An executable with no recorded hash, and a recorded hash with no executable, fail too
- [ ] Build with the pinned toolchain of the imported SDK (`toolchain.json`), never the host's, and call the check from the build of every imported SDK program, so a mismatch fails the import
- [ ] Add tests to `scripts/tests/test_imports.py`: a matching executable passes; one changed byte fails naming the executable; a missing record fails
- [ ] Commit: `"imports: reproducible build check for imported programs"`

**Test checkpoint:** `python3 scripts/tests/test_imports.py` passes with the new cases; after a build, `python3 scripts/imports.py --repro office` prints `office: N executables match the release` and exits 0. Test on: WSL2 dev host.

---

## OS Comparison

| ⭐  | Feature                           | 🪟 Win11           | 🐧 Linux             | 🚀 Impossible OS     |
| --- | --------------------------------- | ------------------ | -------------------- | -------------------- |
| 💎  | Games, tools and media programs   | ✅ Inbox and Store | ✅ Per desktop       | ⬜ Planned -- §3-§23 |
| ⭐  | One source for the OS and Windows | ❌                 | ⚠️ Toolkit dependent | ⬜ Planned -- §1-§23 |

## Verification

- [ ] `python3 scripts/imports.py --check` passes and `--list` shows a commit for every source of this file
- [ ] Each imported program opens on the desktop, and its engine tests pass on the OS
