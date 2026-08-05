---
schema_version: 1
id: android-app-compatibility
domain: 18-future-research
status: active
title: "TODO-06 -- Android App Compatibility (Research Spike)"
---

# TODO-06 -- Android App Compatibility (Research Spike)

> **Goal:** Research spike to scope how Impossible OS could run **Android applications**
> (APK / AAB ecosystem) without abandoning the Win32-native host model -- surveying
> proven industry patterns (VM guest, containerized Android userspace, translation-only
> dead ends), host prerequisites (hypervisor, VirtIO, networking), guest runtime choices
> (AOSP level, ART, update cadence), display and input bridging into the desktop
> compositor, ABI strategy (ARM64 guest vs x86_64 + binary translation), and producing
> a phased roadmap with explicit rejection criteria for infeasible shortcuts.

> [!IMPORTANT]
> **Current state:** Impossible OS is **Win32-first on x86-64** with no Android kernel
> interfaces (no binder driver, no ashmem, no Zygote). There is **no supported path**
> to execute Dalvik bytecode or load `.apk` payloads as native PE today. This TODO is
> **research and planning only** until `TODO-02` / `TODO-03` / `TODO-01` mature enough
> to host a guest or port a stack.
>
> **Do not conflate** "run an APK" with "port ART into ring 3 as a Win32 DLL" -- ART
> assumes a Linux-like process model, Bionic libc, ashmem/binder, and a permission
> framework. A credible first milestone is almost always **a full Android guest**
> (VM or hardware-partitioned environment), not PE wrapping of DEX.
>
> **Windows Subsystem for Android (WSA)** demonstrated Hyper-V-backed Android-on-Windows
> with Amazon Appstore distribution; Microsoft deprecated WSA (2025). Lessons: guest VM
> model works for compatibility; **Google Mobile Services (GMS)** and Play Integrity
> remain ecosystem blockers for any unofficial stack.
>
> **Chrome OS ARC++ vs ARCVM** shows the industry trend from in-container Android to
> **crosvm-based VMs** for isolation and upgrade velocity -- assume VM-class isolation
> for any serious Impossible OS design.
>
> Research deliverable: `docs/architecture/android-app-compatibility-plan.md` plus
> optional **QEMU-only** prototype notes -- no production binder/ART merge during the
> spike unless a later TODO promotes execution.

---

## Inputs

- `18-future-research/TODO-01-multi-arch-port.md` §3 §5 §6 (→ XREF) -- AArch64 guest for
  native ARM APK ABI; MS ABI vs AAPCS64 if Android processes ever become first-class PE
- `18-future-research/TODO-02-hypervisor.md` §4 §5 (→ XREF) -- VM guest RAM, VirtIO
  blk/net/gpu, serial console for Android bring-up
- `18-future-research/TODO-03-gpu-compositor.md` §1 §6 §3 (→ XREF) -- VirtIO-GPU scanout,
  host compositor integration, DMA upload path for guest framebuffer
- `12-user-platform-sdk/TODO-07-win32-compat-matrix.md` (→ XREF) -- launcher UX,
  lifecycle, and test harness patterns beside real Win32 programs
- `include/kernel/mm/pmm.h`, `include/kernel/mm/vmm.h` -- guest RAM carve-out and
  mapping discipline (future host VMM work)
- AOSP / ART upstream documentation (external reference only; do not vendor URLs into
  this file)

---

## Outcome

A `docs/architecture/android-app-compatibility-plan.md` that records: (1) rejected
options with rationale, (2) recommended phased architecture (likely VM-first), (3)
host kernel and userland prerequisites table, (4) ABI matrix (ARM64 vs x86_64 vs
translation), (5) display/input/Wayland bridging sketch, (6) GMS vs pure-AOSP stance,
(7) effort bands (person-years) and hard dependencies on `TODO-01` / `TODO-02` /
`TODO-03`. Optional: one-page QEMU command line proving an **off-the-shelf** AOSP x86_64
or ARM64 emulator image boots with VirtIO console (manual step, not CI).

---

## Implementation Order

| Step | Section                                  | 💎/⭐ | Dependency                        |
| ---- | ---------------------------------------- | --- | --------------------------------- |
| 1    | Research charter and success criteria    | ⭐   | This file; stakeholder north star |
| 2    | Architecture options record (VM vs container vs translation) | ⭐   | Industry survey; `TODO-02`        |
| 3    | Host kernel prerequisites                | ⭐   | `TODO-02` §5; networking; storage |
| 4    | Guest Android runtime plan               | ⭐   | AOSP version; ART; update cadence |
| 5    | Display and input bridging               | ⭐   | `TODO-03` §1 §3; compositor       |
| 6    | ABI and ISA strategy                     | ⭐   | `TODO-01` §3 §6; multi-ABI APK    |
| 7    | Distribution and ecosystem stance        | ⭐   | AOSP vs GMS; sideload policy      |
| 8    | Milestones, exit gates, deliverables     | ⭐   | §1--§7 complete                   |

---

## 1. Research Charter and Success Criteria `[Sonnet]`

- [ ] Define **north star**: e.g. "F-Droid AOSP-only apps tier-1" vs "Play Store parity"
  (the latter is likely out of scope for years).
- [ ] Define **minimum viable demo (MVD)**: e.g. static `hello-world.apk` Activity under
  AOSP guest with serial log proof; no GMS.
- [ ] Define **non-goals**: shipping Google Play Services, passing Play Integrity,
  banking-app attestation, or hiding the guest from the user as native Win32.
- [ ] Capture **security bar**: guest must not share host kernel address space; document
  threat model (APK as untrusted code).
- [ ] List **measurable exit criteria** for closing the research spike vs opening an
  implementation epic in another domain.

**Commit:** Charter bullets merged into plan doc §0.

**Test checkpoint:** N/A (documentation milestone).

---

## 2. Architecture Options Record `[Sonnet]`

- [ ] **Option A -- Full Android guest VM** (recommended first): Impossible OS as host
  VMM boots minimal AOSP generic target; VirtIO console / GPU / net / disk; lifecycle
  from a Win32 helper process.
- [ ] **Option B -- Container / LXC-style** (Waydroid-class): requires Linux-like
  cgroups, network namespaces, binder, ashmem or memfd replacements, **Wayland** peer --
  map each prerequisite to Impossible OS gaps; expect **high** host kernel work.
- [ ] **Option C -- Translation-only / PE shim**: run ARM/x86 native `.so` from APK on
  Win32 -- document why DEX + framework + permissions make this a **research dead end**
  for general APK compatibility.
- [ ] **Option D -- Remote streaming**: enterprise VDI pattern; note as fallback only.
- [ ] **Comparison table** in plan doc: isolation, engineering years, GMS feasibility,
  GPU path, input latency.

**Commit:** Options chapter in `android-app-compatibility-plan.md`.

**Test checkpoint:** Peer review checklist: every option has explicit reject or defer
rationale.

---

## 3. Host Kernel Prerequisites `[Sonnet]`

- [ ] **Memory**: contiguous guest RAM budget; ballooning (stretch); host OOM policy.
- [ ] **VirtIO**: align with `TODO-02` §5 host-side queue emulation; virtio-gpu,
  virtio-input, virtio-net minimum set for Android bring-up.
- [ ] **Time and timers**: TSC / wall clock drift for Android `alarm` and media.
- [ ] **Binder analog**: if Option B ever resurfaces, standalone subsection on binder
  IPC porting cost; default spike assumes guest-internal binder only.
- [ ] **Networking**: NAT vs bridged; DNS; captive portal behavior (document only).

**Commit:** Prerequisite table with owning TODO references.

**Test checkpoint:** Each prerequisite row links to existing code path or `TODO-02`
issue ID placeholder.

---

## 4. Guest Android Runtime Plan `[Sonnet]`

- [ ] Target **AOSP branch / tag** (e.g. quarterly release train) and **generic x86_64
  vs arm64** product choice tied to §6.
- [ ] **ART / dex2oat / profiles**: boot image, JIT vs AOT trade-offs for cold start on
  VM without bare-metal GPU (CPU interpreter acceptable for spike).
- [ ] **Update cadence**: full image OTA vs differential; who signs images.
- [ ] **Zygote / system_server** assumptions: document that shortening this stack is not
  in scope for spike.
- [ ] **Kernel requirements inside guest**: common Android kernel config options
  (namespaces, cgroups) as a checklist for the chosen AOSP version.

**Commit:** Runtime subsection in plan doc.

**Test checkpoint:** Manual: boot chosen AOSP image once in upstream QEMU with same
VirtIO set planned for Impossible HV -- capture serial transcript in `docs/` appendix
optional.

---

## 5. Display and Input Bridging `[Sonnet]`

- [ ] **Frame delivery**: guest VirtIO-GPU scanout to host texture vs shared memory
  blit; color space (sRGB) and DPI scaling.
- [ ] **Input**: multi-touch, keyboard, mouse capture vs seamless integration; security
  UX (host key combos).
- [ ] **Window management**: single full-screen guest vs per-app surfaces (ARC-style);
  tie to desktop shell TODOs without re-specifying compositor internals.
- [ ] **Audio** (stretch): enumerate virtio-snd vs pulse tunnel; defer if out of scope.

**Commit:** Bridging diagram (ASCII or mermaid) in plan doc.

**Test checkpoint:** N/A until `TODO-03` POC exists; note dependency explicitly.

---

## 6. ABI and ISA Strategy `[Sonnet]`

- [ ] **ARM64 guest on x86_64 host** (Apple-silicon-class pattern): native ARM JNI and
  NDK `.so` without Houdini; requires `TODO-01` AArch64 maturity or a big-endian /
  little-endian aligned AArch64 hypervisor configuration.
- [ ] **x86_64 AOSP guest**: better host ISA match; fewer ARM-only APKs run; document
  **native bridge** risk and maintenance.
- [ ] **Multi-ABI APK** selection rules on install; FAT APK size vs coverage.
- [ ] **NDK / JNI / HWASAN** interactions at a high level (no deep implementation).

**Commit:** ABI matrix table in plan doc.

**Test checkpoint:** Enumerate 5 real APKs (FOSS) and mark expected outcome per ABI
choice.

---

## 7. Distribution and Ecosystem Stance `[Sonnet]`

- [ ] **Pure AOSP + sideload** (F-Droid, `adb install`) as default research stance.
- [ ] **GMS / Play Protect / SafetyNet / Play Integrity**: document certification wall;
  no engineering commitment in spike.
- [ ] **Legal / branding**: do not ship "Android" trademark without compliance review
  (placeholder note for legal owner).
- [ ] **Update signing keys** and AVB (Android Verified Boot) if shipping images to end
  users (stretch).

**Commit:** Ecosystem subsection; explicit "no GMS in MVP" callout.

**Test checkpoint:** Checklist: licensing of prebuilt AOSP blobs reviewed (yes / no /
deferred).

---

## 8. Milestones, Exit Gates, and Deliverables `[Sonnet]`

- [ ] **Phase 0** (this TODO): plan doc only; no kernel merge.
- [ ] **Phase 1** (future epic, not authorized here): QEMU or ImpossibleHV boots AOSP
  generic with VirtIO serial; human-visible `logcat` line on host serial.
- [ ] **Phase 2**: GPU scanout visible in host compositor window (depends `TODO-03`).
- [ ] **Phase 3**: Input loop; one interactive FOSS app (keyboard + pointer).
- [ ] **Exit gate**: promote to active execution domain only when `TODO-02` §4 POC or
  agreed substitute exists.
- [ ] **Handoff**: if Phase 1 lands, create child TODOs under `01-boot-platform` or new
  domain for VMM integration -- do not balloon this file.

**Commit:** Roadmap + gates in plan doc; INDEX cross-links updated.

**Test checkpoint:** `docs/architecture/android-app-compatibility-plan.md` exists and
  contains all §1--§7 headings; `scripts/build.sh` still passes unchanged tree.

---

## OS Comparison


| ⭐   | Feature                           | 🪟 Win11                                 | 🐧 Linux                                  | 🚀 Impossible OS                          |
| --- | --------------------------------- | --------------------------------------- | ---------------------------------------- | ---------------------------------------- |
| ⭐   | Run Android apps on desktop       | ✅ WSA (deprecated); emulators common    | ✅ Waydroid; Anbox-class containers       | ⬜ §2 §4 VM-first guest; no host ART today |
| ⭐   | Isolated Android from host kernel | ✅ Hyper-V VM boundary for WSA           | ✅ Namespaces; cgroups; (ARCVM VM trend)  | ⬜ §2 Option A; ImpossibleHV guest RAM    |
| 💎   | Google Play Store certified       | ✅ Official WSA used Amazon store        | ⬜ GAPPS scripts; no cert                 | ⬜ §7 pure AOSP default; no Play Integrity |
| ⭐   | ARM APK on x86_64 silicon         | ✅ ARM64 WSA guest; emulator translation | ✅ Waydroid arm64 profile; binfmt misc patterns | ⬜ §6 AArch64 guest per `TODO-01` or x86_64 AOSP trade-off |
| ⭐   | GPU composited Android UI         | ✅ Host compositor + guest GPU paravirt  | ✅ Mesa + Wayland pipewire stacks         | ⬜ §5 `TODO-03` VirtIO-GPU path           |

Impossible OS should treat Android as **a guest environment** with a crisp security
boundary, integrate its framebuffer through the same GPU research thread as `TODO-03`,
and avoid ecosystem commitments (GMS) until the hypervisor and compositor foundations
exist.

---

## Unit Tests

> Research spike: no in-kernel Android runtime yet. Tests guard **documentation
> presence** and **non-regression** of the build until code lands under a future epic.

- [ ] Add `scripts/verify-android-compat-plan.sh` (or extend existing doc verifier) that
  fails if `docs/architecture/android-app-compatibility-plan.md` is missing required
  headings (`## Architecture options`, `## Host prerequisites`, `## ABI strategy`,
  `## Roadmap`).
- [ ] When host-side launcher code exists: register `test_register_android_compat_stub`
  in `src/kernel/test/test_runner.c` with `TEST_CAT_EXEC`, single `TEST_SKIP` until
  guest boot path is implemented -- do not add `TEST-SIDE-EFFECT-ALLOWED` hooks for
  live binder or hypervisor without a dedicated implementation TODO.

**Commit:** Script + optional stub test file wired with skip.

**Test checkpoint:** `bash scripts/verify-android-compat-plan.sh` exits 0 after plan doc
  lands.

---

## Verification

- [ ] `docs/architecture/android-app-compatibility-plan.md` checked in with signed-off
  architecture decision record (ADR) for VM-first versus container-first.
- [ ] All XREFs in this file remain valid (no broken relative paths).
- [ ] INDEX entry present in `18-future-research/INDEX.md`.
- [ ] Optional manual evidence: serial log excerpt from upstream QEMU AOSP boot stored
  under `docs/research/` (if repo policy allows large logs, keep under 200 lines).

---

## History

| Timestamp  | Author       | Change                                   |
| ---------- | ------------ | ---------------------------------------- |
| 2026-04-12 | Cursor Agent | Initial research TODO scaffold; VM-first stance; ecosystem and ABI sections |
