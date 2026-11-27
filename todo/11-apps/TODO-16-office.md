---
schema_version: 1
id: office
domain: 11-apps
status: active
title: "TODO-16 -- Rizonesoft Office"
---

# TODO-16 -- Rizonesoft Office

> **Goal:** Bundle Rizonesoft Office (`github.com/rizonesoft/Office`, public) as a permanent app of Impossible OS. Office runs as an Impossible SDK program: it is built from imported source with the pinned SDK toolchain, calls the operating system only through the functions listed in the SDK's `abi/tiers.json`, and has no backend of its own on this OS. Impossible OS imports its source at a pinned commit, builds it, and compares each executable it built with the hash the Office release recorded for it.

> [!IMPORTANT]
> **Current state:** Measured 2026-11-27. Nothing in this file is built, and Office has no code yet: its tech stack record plans C++23 with its own toolkit. Under the approved design (`impossible-sdk` `docs/specs/2026-11-27-sdk-as-platform-design.md`), Office's toolkit is built on Impossible SDK from its first line, with `sdk.json` and `app.json` in its first program. Impossible OS user mode has a C library and no C++ runtime (the SDK's `cxx` part arrives through `11-apps/TODO-15-imported-programs.md` §22). So §1 and §2 are ours now, §3 waits for Office's first program, and §4 and §5 follow.

> [!NOTE]
> **Owner direction, 2026-11-22:** Office is a permanent part of Impossible OS, like Recto, Inkeys and Isotone. Each is made to work on Windows first, then Impossible OS is made compatible with it. **Owner decisions, 2026-11-27:** one shared seam, built into Impossible SDK; no double work, so Impossible OS builds no per-app backend and no drawing library, text engine, control set or C++ runtime for Office. **Approach decision (this file):** Office runs as an Impossible SDK program built from imported source with the pinned SDK toolchain; it gets no Impossible OS backend (Recto's §12 seam model of `11-apps/TODO-05-pdf-viewer.md` is not used here), and its build is reproducible, so what this repository builds is the executable the release shipped. Why: every call Office makes is a listed SDK call, and each listed call is already a roadmap section here (`10-platform-services/TODO-13-program-needs.md`, with the GPU tier at §7 and §8). Reversal cost: medium; a backend would add a section between §3 and §4. **Vendor verdict:** Office's licence is read from its `LICENSE` at the pinned commit by §1 and recorded before anything is copied.

---

## Inputs

- `github.com/rizonesoft/Office` -- the source, its `sdk.json`, `app.json`, tech stack record and release record -- imported by §1
- `impossible-sdk` `docs/specs/2026-11-27-sdk-as-platform-design.md` -- the approved design: the boundary rules, the `cxx` part, the GPU tier and who builds what
- -> XREF: `11-apps/TODO-15-imported-programs.md §1,§2,§22,§23` -- the SDK import, its parts, the `cxx` part here and the reproducible build check this file's §3 uses; one copy of each, never repeated in this file
- -> XREF: `10-platform-services/TODO-13-program-needs.md §1,§2,§3` -- the Windows functions of tiers 0 to 2 that a CPU screen calls (§4)
- -> XREF: `03-memory-concurrency/TODO-06-scheduler-enhancement.md §16` -- per-thread vector state, which must be in place before Office runs threaded here (§4)
- -> XREF: `11-apps/TODO-14-inkeys.md` -- the other bundled app that follows the Windows-first rule; Inkeys keeps its own Win32 approach and does not use the SDK seam
- `src/libs/PROVENANCE.md`, `CREDITS.md` -- vendoring records (§1)

---

## Outcome

`C:\Program Files\Rizonesoft Office\` starts from the Start menu and runs the Office programs of the pinned commit, built here with the SDK toolchain from Office's own source. The SDK's check refuses a build that calls anything not on the list, and the hash of each executable equals the one the Office release recorded. A newer Office arrives by re-importing a pinned commit and passing §5.

---

## Implementation Order

| Step | Section                             | 💎/⭐ | Dependency                        |
| ---- | ----------------------------------- | ----- | --------------------------------- |
| 1    | Office Source Import and Provenance | ⭐    | --                                |
| 2    | Office Boundary Check on Import     | ⭐    | §1, D11 T15 §1                    |
| 3    | Office Build with the SDK Toolchain | 💎    | §2, D11 T15 §2,§22,§23            |
| 4    | Office App on Impossible OS         | 💎    | §3, D10 T13 §1,§2,§3, D03 T06 §16 |
| 5    | Office Conformance and Performance  | ⭐    | §4                                |

---

## 1. Office Source Import and Provenance

> **Spawned-by:** root

**Design:** n/a -- source import and licence records, it draws nothing

**Import:** office

Office is developed in its own repository and proved on Windows first. It is imported as source, never forked: the tree under `src/apps/office/` always equals a pinned upstream commit, and every change Office needs is made upstream and re-imported.

- [ ] Read Office's `LICENSE` and every third-party notice at the commit to pin, and record the verdict per component in `src/libs/PROVENANCE.md` before anything is copied
  - Exclude what cannot be redistributed under this project's GPL-3.0-only, each exclusion named with its reason
- [ ] Confirm the `from` list of the `office` source in `todo/imports.json` against Office's top level at that commit (the ledger entry has `commit` null until the first pull, so it may still be corrected)
- [ ] Copy the pinned program with `python3 scripts/imports.py --pull office` into `src/apps/office/`
- [ ] Review the imported tree before it lands: a Claude read of the imported code plus the Astra passes of `.conclave/reviewers.toml`, and every re-import is reviewed the same way
  - Operator direction 2026-11-23: all imported code is reviewed, not only the integration seam
- [ ] Add the Office row to `src/libs/PROVENANCE.md` and its notice set to `CREDITS.md` in the same commit
- [ ] Write the update procedure in `docs/apps/office.md`: bump the commit with `--pull office --to <commit>`, rebuild, run §5
- [ ] Commit: `"apps: Office imported"`

**Test checkpoint:** `python3 scripts/imports.py --check` exits 0 with `office` pinned, and `python3 scripts/imports.py --list` shows its commit; editing one imported file without proposing it makes the check exit 1 naming the file.

## 2. Office Boundary Check on Import

> **Spawned-by:** root

**Design:** n/a -- a check over source and a built file, it draws nothing

An Office program may call Windows only through Impossible SDK. The SDK checks that in Office's own CI; this section runs the same checks again when Office is imported, with the SDK's own tools, and writes no second checker.

- [ ] Run the SDK's source check (no Windows, COM or DirectX header in Office or its dependencies) over `src/apps/office/` on import, using the tool imported by `11-apps/TODO-15-imported-programs.md` §1
- [ ] Measure the tier of Office's programs with the SDK's effective-tier calculation (imports, plus the tiers its linked parts declare in `parts.json`) and list every Windows function it can reach
- [ ] File each listed function that no roadmap section answers (`python3 scripts/imports.py --needs` names it) in `10-platform-services/TODO-13-program-needs.md` or the file that owns the subject, with a reciprocal XREF, so every call has an owner
- [ ] Fail the import on a call outside the list, never on a missing OS feature: a missing OS feature is a section, not a refusal
- [ ] Commit: `"apps: Office boundary check on import"`

**Test checkpoint:** `bash scripts/test-office.sh --boundary` prints `office: boundary OK (tier N)` and exits 0; a planted source file that includes `<windows.h>` makes it exit 1 naming that file.

## 3. Office Build with the SDK Toolchain

> **Spawned-by:** root

**Needs:** third-party

**Design:** n/a -- a build, it draws nothing

Office is built from the imported tree with the pinned SDK toolchain (`toolchain.json` of the imported SDK), the same one Office's release used, with the C++ runtime of the SDK's `cxx` part. The Need is upstream: Office has no program yet, and its `sdk.json` and `app.json` arrive through §1.

- [ ] `scripts/build.sh` builds `src/apps/office/` with the SDK toolchain into `build/apps/office/`, linking no prebuilt object or archive and no system import library
- [ ] Compare each executable built here with the hash the Office release recorded for it, through the shared check of `11-apps/TODO-15-imported-programs.md` §23; a mismatch fails the build
- [ ] Pass the SDK's import check on every executable against its effective tier, as the last word
- [ ] The imported tree builds with zero local patches (`python3 scripts/imports.py --check` still passes after the build)
- [ ] Commit: `"apps: Office built with the SDK toolchain"`

**Test checkpoint:** `bash scripts/build.sh` ends with `=== BUILD OK ===`; the log prints `office: <exe> hash matches the release` for each executable; changing one byte of an imported source file makes the hash comparison fail.

## 4. Office App on Impossible OS

> **Spawned-by:** root

**Needs:** third-party

**Design:** n/a -- Office draws its own interface with its own toolkit; Impossible OS supplies the window, input and files it runs on

- [ ] Ship Office in the image at `C:\Program Files\Rizonesoft Office\` with a Start menu entry for each program
- [ ] Each program opens its window, draws its first screen and takes mouse and keyboard input
- [ ] Open, edit, save and reopen a document, with the save atomic (a simulated power cut leaves the original intact)
- [ ] Threaded work runs only after per-thread vector state is in place: `03-memory-concurrency/TODO-06-scheduler-enhancement.md` §16 is done and its SSE2 isolation test passes
- [ ] A fault inside an Office program ends that program only and is logged with its module and offset
- [ ] Commit: `"apps: Office runs on Impossible OS"`

**Test checkpoint:** the serial log contains `office: started` for each program and `office: document saved` after a scripted save; the desktop stays responsive throughout. Test on: QEMU TCG and KVM at 1 and 2 CPUs, QEMU WHPX.

## 5. Office Conformance and Performance

> **Spawned-by:** root

**Design:** n/a -- a test suite, it draws nothing

- [ ] `scripts/test-office.sh`: a scripted session under the headless compositor (start, open, edit, save, close) with a frame-time record, comparing pictures and timings separately, before and after, on the same machine
- [ ] A move or an import lands only when its pictures match within Office's own tolerance and its timings are no worse than Office's own targets, which Office sets with its first screens
- [ ] Wire the suite into the smoke matrix as one leg once §4 ships
- [ ] Commit: `"test: Office conformance suite"`

**Test checkpoint:** `bash scripts/test-office.sh` ends with `office: conformance OK` and exits 0, recording median and 99th-percentile frame times; breaking the file read of the platform layer makes it fail at the first document open.

---

## OS Comparison

| ⭐  | Feature                                | 🪟 Win11                 | 🐧 Linux                   | 🚀 Impossible OS |
| --- | -------------------------------------- | ------------------------ | -------------------------- | ---------------- |
| 💎  | Office suite bundled in the image      | ❌ Store or subscription | ⚠️ LibreOffice, per distro | ⬜ Planned -- §4 |
| ⭐  | Same source and hash on Windows and OS | ❌ Separate builds       | ❌ Per distro build        | ⬜ Planned -- §3 |
| ⭐  | Bundled app measured by CI             | ❌ No                    | ❌ No                      | ⬜ Planned -- §5 |

> **After §1-§2:** Office's source is in the tree and every call it makes is checked against the list. **After §3-§4:** Impossible OS ships Office built from the same source and toolchain as the Windows release. **After §5:** every Office update and every OS change is measured against the same session.

---

## Unit Tests

> Office's own tests at the pinned commit are the unit tests: they run on Windows first and inside the OS. No kernel test category is needed.

- [ ] Office's automated test suite at the pinned commit builds and passes on Impossible OS under the user-mode test launcher
- [ ] Commit: `"test: Office suites on Impossible OS"`

## Verification

- [ ] `bash scripts/build.sh clean` then `tail -1 build/build.log` shows `=== BUILD OK ===`, with Office built and every hash matching
- [ ] `python3 scripts/imports.py --check` exits 0
- [ ] `bash scripts/test-office.sh` ends with `office: conformance OK`
- [ ] `bash scripts/test-smoke-matrix.sh` passes with the Office leg; the serial log contains `office: started`
- [ ] QEMU WHPX at 2 CPUs and VirtualBox: a 5-minute editing session shows no fault in the serial log
