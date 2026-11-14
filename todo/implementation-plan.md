# Impossible OS -- Implementation Plan

The order to build every roadmap section in, from today to the release. Every numbered section of every TODO file appears in exactly one row.

> **Progress:** **969 of 2704 sections complete (35%).** 127 more are shipped or in progress and still owed work or review. Derived by `python3 scripts/todo-plan.py --sync`, never edited by hand.

**How to use this.** Ask for the next rows instead of reading this file, which is long: `python3 scripts/todo-plan.py --next 5` prints the first rows that can be built now (open, every dependency done, not parked), in plan order. A row is a complete instruction. Copy it and paste it:

```
implement todo section: | [ ] | `D01 T14 §2` | Boot Progress Named-Stage API | none | 9 |
```

`python3 scripts/todo-plan.py --resolve '<the row>'` turns a row, in whatever shape it arrives, into its file and section. One row is `/implement-todo-section` and then `/review-todo-section`.

> [!IMPORTANT]
> **The boxes are derived. Never tick one by hand.**
>
> A box is the section's lifecycle class, read by the same classifier the run obeys: `[x]` done (shipped with both the Verified and the Quality reviewed stamp, or parked by a terminal deferral), `[/]` shipped or in progress and still owed work or review, `[ ]` not started. They change in one place, the section's own TODO file.
>
> ```bash
> python3 scripts/todo-plan.py --sync     # rewrite boxes and titles, place new sections, update the progress line
> python3 scripts/todo-plan.py --check    # fail when the plan is stale or a row sits before its dependency
> ```

**Needs and Items.** Items is the number of checklist items in the section. Needs is what the section cannot finish without, from its `**Needs:**` line: `bare-metal`, `whpx-host`, `display-session`, `attended`, `owner-decision`, `third-party` or `operator`, and `none` when it has no such line. A Need never holds a section back. The run builds it, proves what the emulators and the host can prove, and parks only the one check that needs the named thing.

**What is written by hand.** The phase headings, the paragraph under each one, and the order of the rows. A sync never moves a row and never touches prose. A new section is placed by the sync after the last row of its own file, or later when it depends on something later; move it by hand if it belongs elsewhere. The check holds the order: an open section never sits before an open section it depends on. Finished work constrains nothing.

**How phases are authored.** A phase is a `### Phase <N> -- <Title>` heading, one paragraph on why it runs where it does, and one table. The phases were seeded on 2026-11-11 from the roadmap domains: a section starts in its domain's phase and moves later only when it depends on a section in a later one.

**Known defect in the source tables.** Some Implementation Order tables name dependencies that form a cycle. `python3 scripts/todo-plan.py --cycles` lists the open sections involved. They are never offered by `--next`, and they may sit in either order here, until the `Depends On` cells are corrected.

## The phases

### Phase 0 -- Infrastructure: tooling, test harnesses and the roadmap machinery

Everything later is proven by these gates, so they come first: the build and analysis tooling, the kernel, user-mode and desktop test harnesses, the TODO graph this plan is derived from, the documentation site and the repository's own housekeeping. The Windows 11 parity inventory (`D08 T17`) runs first, ahead of everything else in this phase (operator decision 2026-11-14): it is host-side tooling with no kernel dependency, and every interface section authored after it is measured from its records. Its built-OS layout check stays in Phase 8, behind the automation transport it reads.

| Done | Section       | Deliverable                                                                                                           | Needs      | Items |
| :--: | ------------- | --------------------------------------------------------------------------------------------------------------------- | ---------- | ----: |
| [x]  | `D08 T17 §1`  | Inventory Schema and Checker                                                                                          | none       |    12 |
| [/]  | `D08 T17 §2`  | Reference Build and Capture Profile                                                                                   | none       |    14 |
| [ ]  | `D08 T17 §4`  | Microsoft-Free Rule                                                                                                   | none       |     7 |
| [ ]  | `D08 T17 §3`  | UIA Capture Tool                                                                                                      | none       |    12 |
| [ ]  | `D08 T17 §5`  | Completeness Baselines                                                                                                | none       |     9 |
| [ ]  | `D08 T17 §6`  | Record Dispositions and Consistency Ledger                                                                            | none       |     6 |
| [ ]  | `D08 T17 §7`  | Coverage Report and Ownership Check                                                                                   | none       |     9 |
| [ ]  | `D08 T17 §19` | Capture Matrix, Fixtures and Device Stand-ins                                                                         | bare-metal |    10 |
| [ ]  | `D08 T17 §8`  | Shell Surfaces Inventory: Desktop, Taskbar, Start and Flyouts                                                         | none       |     8 |
| [ ]  | `D08 T17 §9`  | Shell Surfaces Inventory: Windows, Session Screens and Overlays                                                       | none       |     7 |
| [ ]  | `D08 T17 §10` | Settings Inventory: Frame, System, Devices, Network and Personalization                                               | none       |     7 |
| [ ]  | `D08 T17 §11` | Settings Inventory: Apps, Accounts, Language, Gaming, Accessibility, Privacy and Update                               | none       |     8 |
| [ ]  | `D08 T17 §12` | File Explorer and Common Dialogs Inventory                                                                            | none       |     6 |
| [ ]  | `D08 T17 §13` | Control Panel, System Tools and Inbox Apps Inventory                                                                  | none       |     9 |
| [ ]  | `D08 T17 §14` | Shortcut, Gesture, Metric, Motion and State Catalogues                                                                | none       |     5 |
| [ ]  | `D08 T17 §15` | Inventory-to-Roadmap Authoring                                                                                        | none       |     9 |
| [ ]  | `D08 T17 §17` | Design Spec Reconciliation                                                                                            | none       |     8 |
| [x]  | `D00 T01 §1`  | Host Bootstrap and Dependency Contract                                                                                | none       |     5 |
| [x]  | `D00 T01 §2`  | Supported Host Profiles and Reproducible Environments                                                                 | none       |     7 |
| [x]  | `D00 T01 §3`  | Build, Test, Lint, and Run Wrapper Contract                                                                           | none       |     7 |
| [x]  | `D00 T01 §4`  | Machine Launcher and Debug Profile Matrix                                                                             | none       |     7 |
| [x]  | `D00 T01 §5`  | Git Hooks and Local Automation Lifecycle                                                                              | none       |     7 |
| [x]  | `D00 T01 §6`  | GitHub Actions and Artifact Policy Alignment                                                                          | none       |     6 |
| [x]  | `D00 T01 §7`  | Tooling Doctor and Regression Pack                                                                                    | none       |     8 |
| [x]  | `D00 T01 §8`  | Unify Specialist-Launcher Argument Surface                                                                            | none       |     4 |
| [x]  | `D00 T01 §9`  | Legacy-XREF Sweep                                                                                                     | none       |     5 |
| [x]  | `D00 T01 §10` | Smoke-Test POST16 Assertions                                                                                          | none       |     5 |
| [x]  | `D00 T01 §11` | Bare Section-Ref Sweep                                                                                                | none       |     5 |
| [x]  | `D00 T01 §12` | Enforce Required-Tool Version Floors                                                                                  | none       |     8 |
| [x]  | `D00 T01 §13` | Duplicate-Recipe Sweep in the Root Makefile                                                                           | none       |     5 |
| [x]  | `D00 T01 §14` | Defect-Finding Tools: Fuzzers, UBSan, Static Analysis, Compile Cache                                                  | none       |    11 |
| [x]  | `D00 T01 §15` | Static-Analysis Triage and Changed-File Gate                                                                          | none       |     5 |
| [x]  | `D00 T01 §16` | Fuzz Targets for the Remaining Untrusted-Input Parsers                                                                | none       |     8 |
| [x]  | `D00 T01 §17` | Shard the todo-graph Build Suite                                                                                      | none       |     5 |
| [x]  | `D00 T01 §18` | Implementation Plan: Clear the Dependency Cycles and Drive the Run From It                                            | none       |     4 |
| [x]  | `D00 T02 §1`  | Canonical Ownership and Source-of-Truth Map                                                                           | none       |     8 |
| [x]  | `D00 T02 §2`  | Skill Lifecycle, Templates, and Catalog Rules                                                                         | none       |     6 |
| [x]  | `D00 T02 §3`  | Hook Routing and Policy Contract                                                                                      | none       |     7 |
| [x]  | `D00 T02 §4`  | External-Reviewer Contract (Codex)                                                                                    | none       |     7 |
| [x]  | `D00 T02 §5`  | MCP, Permissions, and Extension Boundary                                                                              | none       |     7 |
| [x]  | `D00 T02 §6`  | `AGENTS.md` Cross-Tool Pointer File                                                                                   | none       |     7 |
| [x]  | `D00 T02 §7`  | AI-Assist Commit Disclosure Policy                                                                                    | none       |     7 |
| [x]  | `D00 T02 §8`  | Autonomous-Agent Boundary Policy                                                                                      | none       |     7 |
| [x]  | `D00 T02 §9`  | AI Workflow Regression Suite                                                                                          | none       |     5 |
| [x]  | `D00 T03 §1`  | kmalloc Fault Injection                                                                                               | none       |     6 |
| [x]  | `D00 T03 §2`  | Deterministic Race Barrier                                                                                            | none       |     8 |
| [x]  | `D00 T03 §3`  | Scratch-Buffer Helper                                                                                                 | none       |     6 |
| [x]  | `D00 T03 §4`  | Retrofit Existing "Test Gaps" Stamps                                                                                  | none       |     4 |
| [x]  | `D00 T03 §5`  | Test-Scoped klog Level Demotion (`TEST_KLOG_SUPPRESS`)                                                                | none       |     8 |
| [x]  | `D00 T03 §6`  | Fault-Injection Hardening (task-filter, total-hits cap, multi-allocator)                                              | none       |     7 |
| [x]  | `D00 T03 §7`  | Test Action/Cleanup Registry (`test_add_action`)                                                                      | none       |     7 |
| [x]  | `D00 T03 §8`  | Per-Test Heap-Leak Detection                                                                                          | none       |     7 |
| [x]  | `D00 T03 §9`  | Audit + Classify the 50+ [LEAK] Failures §8 Surfaced                                                                  | none       |     9 |
| [x]  | `D00 T03 §10` | An Over-Long Failing Assertion Wedges the Boot Instead of Reporting                                                   | none       |     3 |
| [x]  | `D00 T03 §11` | A Read One Byte Past a Buffer Is Invisible to Every Test We Can Write                                                 | none       |     4 |
| [x]  | `D00 T03 §12` | Assert or Fix the 55 Test-Phase [FAIL] Lines                                                                          | none       |     5 |
| [x]  | `D00 T03 §13` | Expect or Attribute Kernel Errors per User-Mode Binary                                                                | none       |     8 |
| [x]  | `D00 T04 §1`  | User-Mode Test Assertion Macro                                                                                        | none       |     6 |
| [x]  | `D00 T04 §2`  | Userland syscall.h Parity with Kernel INT 0x80 ABI                                                                    | none       |     6 |
| [x]  | `D00 T04 §3`  | Kernel Test Launcher                                                                                                  | none       |     9 |
| [x]  | `D00 T04 §4`  | Launcher Manifest, Timeouts, and CI-Friendly Output                                                                   | none       |     7 |
| [x]  | `D00 T04 §5`  | User-Mode Fault-Injection Bridge                                                                                      | none       |     7 |
| [x]  | `D00 T04 §6`  | Per-Test Isolation + Cleanup Hook                                                                                     | none       |     6 |
| [x]  | `D00 T04 §7`  | JUnit XML + JSON Output Formats                                                                                       | none       |     6 |
| [x]  | `D00 T04 §8`  | Test Type Taxonomy: smoke / correctness / stress / perf                                                               | none       |     6 |
| [x]  | `D00 T04 §9`  | Syscall Test Binary                                                                                                   | none       |     3 |
| [x]  | `D00 T04 §10` | Libc Test Binary                                                                                                      | none       |     2 |
| [x]  | `D00 T04 §11` | IPC Test Binary                                                                                                       | none       |     3 |
| [x]  | `D00 T04 §12` | Process Lifecycle Test                                                                                                | none       |     2 |
| [x]  | `D00 T04 §13` | File I/O Test                                                                                                         | none       |     3 |
| [x]  | `D00 T04 §14` | Win32 API Test Binary                                                                                                 | none       |     3 |
| [x]  | `D00 T04 §15` | Binary Format Loader Coverage (ELF / PE32+ / EIF)                                                                     | none       |     7 |
| [x]  | `D00 T04 §16` | Build Integration                                                                                                     | none       |     9 |
| [x]  | `D00 T04 §17` | Fast-Path Transport Hardening (probes + ABI drift + silent-hang elimination)                                          | none       |     8 |
| [x]  | `D00 T04 §18` | Fast-Path Observability + ABI Versioning (beyond Win11 + Linux)                                                       | none       |     5 |
| [x]  | `D00 T04 §19` | Fast-Path Transition Ring Buffer + Cross-Transport Fuzz                                                               | none       |     7 |
| [x]  | `D00 T04 §20` | Site-Targeted and PMM-Countdown Fault Selectors                                                                       | none       |     9 |
| [x]  | `D00 T04 §21` | Naming Fork's Child-PML4 Allocation, and Failing That Fork Closed                                                     | none       |     5 |
| [x]  | `D00 T04 §22` | Honest Machine Artifacts for Skipped Sub-Tests                                                                        | none       |     8 |
| [x]  | `D00 T04 §23` | Generated Exit-Status ABI for Ring-3 Assertions                                                                       | none       |     3 |
| [x]  | `D00 T04 §24` | Host-Side JSON Artifact File and Abort-Aware Summaries                                                                | none       |     6 |
| [x]  | `D00 T04 §25` | Non-Forgeable Launcher Record Framing                                                                                 | none       |     8 |
| [x]  | `D00 T04 §26` | Generator Hardening: Preprocessor-Faithful Constant Extraction                                                        | none       |    11 |
| [x]  | `D00 T04 §27` | Artifact Consumer Completeness: Run Identity and the Record Name Bound                                                | none       |     5 |
| [x]  | `D00 T04 §28` | Atomic Publication of the Generated Header Pair                                                                       | none       |     5 |
| [x]  | `D00 T04 §29` | Serial-Transport Volume: Derived Burst Ceiling and Incremental Parse                                                  | none       |     5 |
| [x]  | `D00 T04 §30` | Per-Run Artifact Records and Per-Leg Aliases                                                                          | none       |     6 |
| [x]  | `D00 T04 §31` | The Record Name Bound and the Refusal That Must Be Counted                                                            | none       |     6 |
| [x]  | `D00 T04 §32` | ABI Validation on Every Artifact Target, Not Just `all`                                                               | none       |     5 |
| [x]  | `D00 T04 §33` | Per-Binary Output Capture at the Source                                                                               | none       |     3 |
| [x]  | `D00 T04 §34` | Orphaned-QEMU Recovery                                                                                                | none       |     4 |
| [x]  | `D00 T04 §35` | Immutable Enumeration Plan for the Two-Walk Launcher                                                                  | none       |     7 |
| [x]  | `D00 T04 §36` | Per-Binary Output Artifacts and Fail-Closed Byte Reconciliation                                                       | none       |     4 |
| [x]  | `D00 T04 §37` | Refusal-Name Correlator and JUnit Error/Failure Classification                                                        | none       |     4 |
| [x]  | `D00 T04 §38` | Per-Leg Generation Pointer for Coherent Document Resolution                                                           | none       |     2 |
| [x]  | `D00 T04 §39` | Executable Identity for a Planned Entry                                                                               | none       |     4 |
| [x]  | `D00 T04 §40` | Producer-Side Capture Emission Budget                                                                                 | none       |     5 |
| [x]  | `D00 T04 §41` | Post-Ship Follow-Up Backfill (orphan cohort 2026-07-31)                                                               | none       |     1 |
| [x]  | `D00 T04 §42` | Structural Reap-Grace Assertions Instead of a Wall-Clock Margin                                                       | none       |     3 |
| [x]  | `D00 T04 §43` | Distinguishable Identity Kind for Aggregate Refusals                                                                  | none       |     4 |
| [x]  | `D00 T04 §44` | Close the Residual Completeness Gaps §37's Review Found                                                               | none       |     4 |
| [x]  | `D00 T04 §45` | Reader Leases for a Resolved Generation                                                                               | none       |     5 |
| [x]  | `D00 T04 §46` | Inode-Safe Classification in the Pointer Sweep                                                                        | none       |     4 |
| [x]  | `D00 T04 §47` | End-to-End Proof of the Loader's Identity-Mismatch Branch                                                             | none       |     8 |
| [x]  | `D00 T04 §48` | Wire-Published Facts the Host Enforces: Write Identity, Spawn Expectation, Chunk Bound                                | none       |     4 |
| [x]  | `D00 T04 §49` | Run-Boundary Fence for Emitters That Outlive Their Binary                                                             | none       |     4 |
| [x]  | `D00 T04 §50` | Per-Child Loader Evidence: Ring-3 Entry and Stage Flags in the Child, Not in Globals                                  | none       |     4 |
| [x]  | `D00 T04 §51` | A Reproducible Kernel Assertion Total, and the Intermittent TEB Halt Behind It                                        | none       |     3 |
| [x]  | `D00 T04 §52` | Bind a Leg Pointer to the Record's Own Identity                                                                       | none       |     7 |
| [x]  | `D00 T04 §53` | A Directory Occupying a Pointer or Lease Name                                                                         | none       |     9 |
| [x]  | `D00 T04 §54` | A Write Abandoned Before Its First Chunk Ever Reached the Wire                                                        | none       |     3 |
| [x]  | `D00 T04 §55` | Klog-Ring Assertions That Depend on How Much the Boot Logged                                                          | none       |     4 |
| [x]  | `D00 T04 §56` | Fork Publication Interlocked With the Capture-Tree Reap                                                               | none       |     8 |
| [x]  | `D00 T04 §57` | A Klog Delivery Receipt, So a Record's Settlement Is Exact                                                            | none       |     4 |
| [x]  | `D00 T04 §58` | Launcher Waits Bounded Independently of the Monotonic Clock                                                           | none       |     4 |
| [x]  | `D00 T04 §59` | Capture Reconciliation Runs on the Default Test Path, Not Only for Artifacts                                          | none       |     6 |
| [x]  | `D00 T04 §60` | An Aggregate Budget for Pointer Validation Under the Retention Mutex                                                  | none       |     8 |
| [x]  | `D00 T04 §61` | Validate the Commit Marker as a Shape, and Read Each Record Document From One Snapshot                                | none       |     9 |
| [x]  | `D00 T04 §62` | Detaching an Age Candidate Before Classifying It                                                                      | none       |    10 |
| [x]  | `D00 T05 §1`  | Framebuffer Snapshot API                                                                                              | none       |     5 |
| [x]  | `D00 T05 §2`  | QEMU Framebuffer Dump                                                                                                 | none       |     5 |
| [x]  | `D00 T05 §3`  | Desktop Smoke Test                                                                                                    | none       |     7 |
| [x]  | `D00 T05 §4`  | Input Event Injection                                                                                                 | none       |     5 |
| [x]  | `D00 T05 §5`  | Terminal Output Verification                                                                                          | none       |     6 |
| [x]  | `D00 T05 §6`  | Reference Screenshot Comparison                                                                                       | none       |     9 |
| [x]  | `D00 T05 §7`  | Visual Regression CI Pipeline                                                                                         | none       |    11 |
| [x]  | `D00 T05 §8`  | Window Manager State Verification                                                                                     | none       |     9 |
| [x]  | `D00 T05 §9`  | Perceptual Diff and Structured Screenshot Needles                                                                     | none       |    11 |
| [x]  | `D00 T05 §10` | Frame Timing and Drop Oracle                                                                                          | none       |     8 |
| [x]  | `D00 T05 §11` | Input Record and Replay (Unicode and IME aware)                                                                       | none       |     8 |
| [x]  | `D00 T05 §12` | Headless Compositor and Frame-Lock Stepping                                                                           | none       |     8 |
| [x]  | `D00 T05 §13` | Multi-Monitor and DPI Scaling Test Matrix                                                                             | none       |     7 |
| [x]  | `D00 T05 §14` | WCAG Sweep Over Automation Tree                                                                                       | none       |     7 |
| [x]  | `D00 T05 §15` | Test Isolation and Crash Artifact Capture                                                                             | none       |     3 |
| [x]  | `D00 T06 §1`  | Frontmatter Schema and Spec                                                                                           | none       |     7 |
| [x]  | `D00 T06 §2`  | Generator and Cache Format                                                                                            | none       |     9 |
| [x]  | `D00 T06 §3`  | Validator (stale XREF / dangling dep / orphan / cycle / bat / status / schema)                                        | none       |     6 |
| [x]  | `D00 T06 §4`  | Query CLI (ready / blocked / blocking / by-domain / backlinks / deferred / orphans / stale / stats / code)            | none       |     9 |
| [x]  | `D00 T06 §5`  | Migration: Back-Fill Frontmatter on Existing TODOs                                                                    | none       |     8 |
| [x]  | `D00 T06 §6`  | CI Gate and Make Target                                                                                               | none       |     8 |
| [x]  | `D00 T06 §7`  | Visualization Output (mermaid / graphviz / ascii / gantt / markdown)                                                  | none       |    10 |
| [x]  | `D00 T06 §8`  | MCP Server (AI-Agent Transport over the Cache)                                                                        | none       |    10 |
| [x]  | `D00 T06 §9`  | Per-Item `stamped_items` Cache Extension                                                                              | none       |     9 |
| [x]  | `D00 T06 §10` | Multi-Line Function-Head Resolution in `resolve_symbol`                                                               | none       |     7 |
| [x]  | `D00 T06 §11` | Count What Check 7 Cannot Resolve Instead of Skipping It                                                              | none       |    15 |
| [x]  | `D00 T06 §12` | Resolver Coverage Past the Head Limit, and Lexer/Cache Robustness                                                     | none       |     7 |
| [x]  | `D00 T06 §13` | Stored-Ref Repair: Unpaired Symbols and Bare Filenames                                                                | none       |     7 |
| [x]  | `D00 T06 §14` | Identity Gate Over the Whole Symbol-Ref Population                                                                    | none       |     5 |
| [x]  | `D00 T06 §15` | The Shared Rule's Cost: Structural-Walk Hot Path and Split Cache Lifecycles                                           | none       |     3 |
| [x]  | `D00 T06 §16` | Wire the Identity Gate So Something Actually Runs It                                                                  | none       |     4 |
| [x]  | `D00 T06 §17` | One Shared Cache-Schema Validator for Both Cache Readers                                                              | none       |    10 |
| [x]  | `D00 T06 §18` | Identity-Gate Hardening: Producer Differential and Protocol-Constant Extraction                                       | none       |     7 |
| [x]  | `D00 T06 §19` | Cache-Validation Reach: the Remaining Raw Cache Readers                                                               | none       |     5 |
| [x]  | `D00 T06 §20` | A Machine-Checkable Bucket-Emission Contract                                                                          | none       |     4 |
| [x]  | `D00 T06 §21` | The Producer-Side Generation Window in `build.py`                                                                     | none       |     5 |
| [x]  | `D00 T06 §22` | `query.py` Fail-Closed Through Every Transport                                                                        | none       |     5 |
| [x]  | `D00 T06 §23` | `validate.py` Rebuild Recovery and the Baseline Cache                                                                 | none       |     6 |
| [x]  | `D00 T06 §24` | The History-Identity Cost Decision: `requires_history` for the Query Profile                                          | none       |     6 |
| [x]  | `D00 T06 §25` | Fields the Readers Consume That the Producer Never Emits                                                              | none       |     4 |
| [x]  | `D00 T06 §26` | Domain-Code Resolution Picks a Directory by Cache Order                                                               | none       |     4 |
| [x]  | `D00 T06 §27` | Effective-History Identity: Replace Refs and Grafts                                                                   | none       |     6 |
| [x]  | `D00 T06 §28` | Section-Level Readiness: Verbs That Consult the Dependency Evidence the Graph Actually Has                            | none       |     6 |
| [x]  | `D00 T06 §29` | `--fix-line-numbers` Reports Success Over Targets It Could Not Resolve                                                | none       |     5 |
| [x]  | `D00 T06 §30` | `--watch` Never Ticks on a History Change for the History-Consuming Verbs                                             | none       |     9 |
| [x]  | `D00 T06 §31` | An Equal-Count Shallow Boundary Still Moves `created_at` Under an Unchanged Identity                                  | none       |     6 |
| [x]  | `D00 T06 §32` | In-File Anchor Links Are Checked by Nothing                                                                           | none       |     4 |
| [x]  | `D00 T06 §33` | Stamp-Target Capture Splits a Link Whose Label Contains a Space                                                       | none       |    10 |
| [x]  | `D00 T06 §34` | The Performance Budget Times Itself on a Clock That Steps                                                             | none       |     5 |
| [x]  | `D00 T06 §35` | A Same-File Stamp Matches Its Own Text, So Repair Answers With the Stamp's Line                                       | none       |     5 |
| [x]  | `D00 T06 §36` | The Cache Producer Adopts the Shared Fence Tracker                                                                    | none       |     8 |
| [x]  | `D00 T06 §37` | One Clause Parser Decides How Many Destinations a Clause Names                                                        | none       |     6 |
| [x]  | `D00 T06 §38` | Every Gate Parser Adopts the Shared Fence Tracker                                                                     | none       |    13 |
| [x]  | `D00 T06 §39` | The Shared Fence Tracker Becomes Container-Aware                                                                      | none       |     6 |
| [x]  | `D00 T06 §40` | An Inputs Section Marker Keeps Its Closing Backtick, So One Dependency Is Two Edges                                   | none       |     4 |
| [x]  | `D00 T06 §41` | The Two Mutating Repair Tools Adopt the Shared Fence Tracker                                                          | none       |     6 |
| [x]  | `D00 T06 §42` | HTML Blocks Join the Tracker, and the Terminal Contract Carries the Block Kind                                        | none       |     8 |
| [x]  | `D00 T06 §43` | Finish the One-Contract Consolidation Across the Consumer Closure                                                     | none       |     3 |
| [x]  | `D00 T06 §44` | The Inline Scanner Honours CommonMark Precedence, and the Table Cell Reaches It Unmodified                            | none       |    10 |
| [x]  | `D00 T06 §45` | The Container Phase Stops Being Quadratic, Stops Erasing, and Becomes the Section Context the Four Walks Share        | none       |     3 |
| [x]  | `D00 T06 §46` | CommonMark HTML Block Type 7 Needs a Non-Backtracking Matcher                                                         | none       |     2 |
| [x]  | `D00 T06 §47` | The Gate Consumers Adopt the Section-Context Projections the Producer Already Uses                                    | none       |     5 |
| [x]  | `D00 T06 §48` | The Mutating Repair Tool Stops Rewriting the Raw HTML It Is Shown                                                     | none       |     3 |
| [x]  | `D00 T06 §49` | Decide Whether `alias-staleness` Is Producer-Assertable, Then Promote It or Record Why Not                            | none       |     5 |
| [x]  | `D00 T06 §50` | A Base Older Than the Gate's Own Protocol Must Not Be Reported as a Broken Gate                                       | none       |     4 |
| [x]  | `D00 T06 §51` | The Byte-Identical Closure Exit Must Not Outrank the Protocol Refusals                                                | none       |     3 |
| [x]  | `D00 T06 §52` | Object-Replacement Metadata Can Misrepresent an Endpoint by Routes Other Than Its Commit                              | none       |     7 |
| [x]  | `D00 T06 §53` | The Protocol Probe Executes the Tree It Is Judging, So No In-Process Channel Is Trusted                               | none       |     4 |
| [x]  | `D00 T06 §54` | The Gate Adjudicates the WORKING TREE While the Push Carries the COMMIT                                               | none       |     4 |
| [x]  | `D00 T06 §55` | The Byte-Identical Path Pays for a Full Base Checkout It Does Not Need                                                | none       |     6 |
| [x]  | `D00 T06 §56` | The Resolver Walks Leak What the Tree Spawns on the Path Where They SUCCEED                                           | none       |     6 |
| [/]  | `D00 T06 §57` | The Hook Certifies Repository HEAD, Not the Commit Being Pushed                                                       | none       |     4 |
| [x]  | `D00 T06 §58` | Head-Versus-Tree Checks Section 54 Made Unreachable Are Retired or Re-Fixtured                                        | none       |     5 |
| [x]  | `D00 T06 §59` | The Identity-Gate Fixture Group Pays for a Whole Repository and Proves Less Than It Claims                            | none       |     6 |
| [x]  | `D00 T06 §60` | Every Name-Resolved Use of TMP_DIR Is Still a Name                                                                    | none       |     5 |
| [x]  | `D00 T07 §1`  | Bridge Skeleton + LSP JSON-RPC Client                                                                                 | none       |     6 |
| [x]  | `D00 T07 §2`  | clangd Integration (C / H)                                                                                            | none       |     6 |
| [x]  | `D00 T07 §3`  | asm-lsp Integration (NASM)                                                                                            | none       |     5 |
| [x]  | `D00 T07 §4`  | bash-language-server Integration (.sh)                                                                                | none       |     6 |
| [x]  | `D00 T07 §5`  | pyright Integration (Python)                                                                                          | none       |     6 |
| [x]  | `D00 T07 §6`  | PowerShellEditorServices Integration (.ps1)                                                                           | none       |     6 |
| [x]  | `D00 T07 §7`  | Six MCP Tools (Hover, Definition, References, Diagnostics, Workspace/Document Symbol)                                 | none       |     7 |
| [x]  | `D00 T07 §8`  | Extension Routing, Per-LSP Lock, Concurrent-Call Safety                                                               | none       |     6 |
| [x]  | `D00 T07 §9`  | Setup Deps, Manifest, Makefile Target, Docs, Boundary Compliance                                                      | none       |     8 |
| [x]  | `D00 T07 §10` | Unit Tests, --self-test Harness, test-tooling Surface                                                                 | none       |     4 |
| [x]  | `D00 T07 §11` | Extended LSP Tools (Completion, Signature Help, Type-Definition, Implementation, Declaration, Call Hierarchy, Code... | none       |     9 |
| [x]  | `D00 T07 §12` | File-Change Lifecycle (`didOpen`, `didChange`, `didSave`, `didClose` Forwarding)                                      | none       |     6 |
| [x]  | `D00 T07 §13` | LSP Subprocess Health Monitoring + Auto-Restart                                                                       | none       |     6 |
| [x]  | `D00 T07 §14` | Structured JSON Logging + Request Correlation IDs                                                                     | none       |     5 |
| [x]  | `D00 T07 §15` | Path Sandboxing + Workspace Boundary Enforcement                                                                      | none       |     6 |
| [x]  | `D00 T07 §16` | Warm-Index Preloading + Cold-Start Budget                                                                             | none       |     6 |
| [x]  | `D00 T07 §17` | Background Warm-Start Mode (MCP Launcher Compatibility)                                                               | none       |     9 |
| [x]  | `D00 T07 §18` | Type Hierarchy Tools (Supertypes / Subtypes, Read-Only)                                                               | none       |     7 |
| [x]  | `D00 T07 §19` | Scale Roadmap (Deferred)                                                                                              | none       |     1 |
| [x]  | `D00 T07 §20` | Reap Determinism and Leak Attribution for the Bridge's Language Servers                                               | none       |     4 |
| [x]  | `D00 T07 §21` | Thread-Group Edge Cases in the Language-Server Reap                                                                   | none       |     5 |
| [x]  | `D00 T07 §22` | Reap Sweep Internals: pidfd Ownership, One-Snapshot Enumeration, Ledger Reader                                        | none       |     4 |
| [x]  | `D00 T07 §23` | Injected-Clock Deadline Coverage and the Non-Dumpable-Child Reap Blind Spot                                           | none       |     7 |
| [x]  | `D00 T07 §24` | Single-Pass Procfs Cleanup in the Teardown Sweep                                                                      | none       |     4 |
| [x]  | `D00 T07 §25` | Restart-Publish Confirmed-Dead Gate and Single-Flight Teardown                                                        | none       |     7 |
| [/]  | `D00 T07 §26` | Teardown Verdict Propagation and a Non-Reaping Liveness Probe                                                         | none       |     6 |
| [x]  | `D00 T08 §1`  | Codex MCP Wiring + Cross-Config Drift Validator                                                                       | none       |    10 |
| [x]  | `D00 T08 §2`  | Codex Invocation Policy: No `--model` / `--effort` From Claude                                                        | none       |     7 |
| [x]  | `D00 T08 §3`  | `receiving-code-review` Hard Gate: State File + Post-Codex Hook Block                                                 | none       |    10 |
| [x]  | `D00 T08 §4`  | Section-Commit Gate Hardens to BLOCK With Build + Codex + Receiving Evidence                                          | none       |     9 |
| [x]  | `D00 T08 §5`  | `review-todo-section` Four-Dispatch Enforcement                                                                       | none       |     9 |
| [x]  | `D00 T08 §6`  | MCP Usage Discipline -- Doc + CLAUDE.md Pointer                                                                       | none       |     7 |
| [x]  | `D00 T08 §7`  | Hook System Audit + Dedupe                                                                                            | none       |    10 |
| [x]  | `D00 T08 §8`  | Superpowers Skill Catalog Audit + Trigger Rows                                                                        | none       |     7 |
| [x]  | `D00 T08 §9`  | Cross-Tool Drift Detection -- `scripts/audit-ai-system.sh`                                                            | none       |     6 |
| [x]  | `D00 T08 §10` | Skill Step-State Telemetry + Non-Contiguous-Step Block                                                                | none       |     7 |
| [x]  | `D00 T08 §11` | Hook Event Surface Expansion (SessionStart / UserPromptSubmit / Stop / SubagentStop / PreCompact)                     | none       |    10 |
| [x]  | `D00 T08 §12` | AI-Slop Content Lints (Tautological-Test / Stub-Behind-Stamp)                                                         | none       |     8 |
| [x]  | `D00 T08 §13` | Documentation Sync -- CLAUDE.md / ai-system.md / mcp-usage.md / Reciprocal XREFs                                      | none       |     9 |
| [x]  | `D00 T08 §14` | Remove Copilot CLI Reviewer Wholesale (Subordinate-Reviewer Reduction)                                                | none       |     7 |
| [x]  | `D00 T08 §15` | Design-Review Hook Scope Fix: Review-Kind Recognition + Commit-Clears-Gate                                            | none       |     4 |
| [x]  | `D00 T08 §16` | Compaction-Resilient Skill-Step State -- PreCompact Orphan-Marking                                                    | none       |     8 |
| [x]  | `D00 T08 §17` | Review-Pipeline Pre-Codex Enforcement (Phase-1 + Re-Adversarial)                                                      | none       |     8 |
| [x]  | `D00 T08 §18` | Stamp-Region + OS Comparison Completeness Lints (Check 10 + Check 11)                                                 | none       |     4 |
| [x]  | `D00 T08 §19` | Codex Prompt-Scope Helper -- Wrapper Plumbing for Re-Reviews                                                          | none       |     4 |
| [x]  | `D00 T08 §20` | Implement-Pipeline Section-Commit Gates (Steps 5/8/13/16)                                                             | none       |     8 |
| [x]  | `D00 T08 §21` | Implement-TODO-Section Partial-Enforcement Heuristics (WARN-First)                                                    | none       |    14 |
| [x]  | `D00 T08 §22` | Review-Pipeline Right-Sizing Doctrine -- RISK_TIER + Bootstrap + Tier-Aware Triage + State Diagnosis + Spiral Check   | none       |     8 |
| [x]  | `D00 T08 §23` | Unify SKIP_REVIEW_HOOK Opt-Out Scanner Across PreToolUse Gates                                                        | none       |     8 |
| [x]  | `D00 T08 §24` | last-codex-review.json State-Write Reliability -- Close §22 #4 Properly                                               | none       |     7 |
| [x]  | `D00 T08 §25` | Observer Step-13/13.5 Regex Coverage for All Codex Dispatch Shapes                                                    | none       |     7 |
| [x]  | `D00 T08 §26` | `scripts/commit-with-skip.sh` Wrapper Retired (Closed by §23)                                                         | none       |     4 |
| [x]  | `D00 T08 §27` | Phase 1 Evidence-Gate Threshold Tuning via Miss-Log Telemetry                                                         | none       |     5 |
| [x]  | `D00 T08 §28` | Codex Prompt Argument Escaping Doctrine                                                                               | none       |     7 |
| [x]  | `D00 T08 §29` | Standardize PreToolUse Prefix-Allowlist Across Gates                                                                  | none       |     7 |
| [x]  | `D00 T08 §30` | Shell-Aware Codex Dispatch Segmentation in `_review_kind`                                                             | none       |     9 |
| [x]  | `D00 T08 §31` | Post-Commit Hook Concurrency: Serialize Build/Smoke via flock                                                         | none       |     5 |
| [/]  | `D00 T08 §32` | Grammar-Accurate Shell Segmentation for the Gate Exemptions                                                           | none       |     4 |
| [x]  | `D00 T08 §33` | Section-commit gate reads an untokenizable commit line as a commit                                                    | none       |     8 |
| [x]  | `D00 T08 §34` | Section-commit gate test wiring that kernel sources can satisfy                                                       | none       |     5 |
| [/]  | `D00 T08 §35` | Cross-section review of a closed TODO file                                                                            | none       |     4 |
| [x]  | `D00 T08 §36` | Second-boot leg in the smoke matrix                                                                                   | none       |     4 |
| [x]  | `D00 T08 §37` | Syscall argument counts in the ABI fingerprint                                                                        | none       |     5 |
| [x]  | `D00 T08 §38` | Release-flavor build at the section boundary                                                                          | none       |     8 |
| [x]  | `D00 T08 §39` | Re-judge the closed parks under the never-park rule                                                                   | none       |     5 |
| [x]  | `D00 T08 §40` | Pytest-style control-plane tests run in the suite                                                                     | none       |     5 |
| [x]  | `D00 T08 §41` | CI jobs run in the pinned image                                                                                       | none       |     7 |
| [ ]  | `D00 T08 §42` | The graph jobs run in a small pinned image                                                                            | none       |     3 |
| [x]  | `D00 T09 §1`  | Preflight Inventory and Risk Register                                                                                 | none       |    10 |
| [/]  | `D00 T09 §2`  | `rizonetech` Organization Readiness and Policy Parity                                                                 | operator   |    11 |
| [x]  | `D00 T09 §3`  | GitHub Pages Custom-Domain Continuity Plan                                                                            | none       |    11 |
| [x]  | `D00 T09 §4`  | Repository Transfer Runbook and Rollback Window                                                                       | none       |    16 |
| [/]  | `D00 T09 §5`  | Post-Transfer Settings, Workflows, Secrets, and Environments Audit                                                    | operator   |    12 |
| [x]  | `D00 T09 §6`  | URL, Badge, Docs, and Generated-Link Sweep                                                                            | none       |    11 |
| [x]  | `D00 T09 §7`  | Validation Suite: Pages, Actions, Releases, Clone, Hooks, Graph                                                       | none       |     8 |
| [x]  | `D00 T09 §8`  | Move-Back and Public-Visibility Runbook                                                                               | none       |    16 |
| [x]  | `D00 T09 §9`  | Documentation Sync and Closure                                                                                        | none       |     8 |
| [x]  | `D00 T10 §1`  | Site Generator, Project Facts, Coverage, and Drift Gate                                                               | none       |    14 |
| [x]  | `D00 T10 §2`  | Map Existing Docs Pages to Their Roadmap Files                                                                        | none       |     5 |
| [x]  | `D00 T10 §3`  | Documentation Page Contract, Template, and create-todo Step                                                           | none       |     8 |
| [x]  | `D00 T10 §4`  | Document: Infrastructure                                                                                              | none       |     6 |
| [x]  | `D00 T10 §5`  | Document: Boot platform, part 1                                                                                       | none       |     8 |
| [x]  | `D00 T10 §6`  | Document: Boot platform, part 2                                                                                       | none       |     8 |
| [x]  | `D00 T10 §7`  | Document: Boot platform, part 3                                                                                       | none       |     7 |
| [x]  | `D00 T10 §8`  | Document: Kernel core, part 1                                                                                         | none       |     8 |
| [x]  | `D00 T10 §9`  | Document: Kernel core, part 2                                                                                         | none       |     8 |
| [x]  | `D00 T10 §10` | Document: Kernel core, part 3                                                                                         | none       |     8 |
| [x]  | `D00 T10 §11` | Document: Memory and concurrency                                                                                      | none       |     8 |
| [x]  | `D00 T10 §12` | Document: Drivers and hardware, part 1                                                                                | none       |     9 |
| [x]  | `D00 T10 §13` | Document: Drivers and hardware, part 2                                                                                | none       |     8 |
| [x]  | `D00 T10 §14` | Document: Storage and filesystems                                                                                     | none       |    10 |
| [x]  | `D00 T10 §15` | Document: Networking                                                                                                  | none       |     8 |
| [x]  | `D00 T10 §16` | Document: Desktop foundation and graphics, part 1                                                                     | none       |     9 |
| [x]  | `D00 T10 §17` | Document: Graphics and UI, part 2                                                                                     | none       |     8 |
| [x]  | `D00 T10 §18` | Document: Desktop shell                                                                                               | none       |    10 |
| [x]  | `D00 T10 §19` | Document: Platform services                                                                                           | none       |    10 |
| [x]  | `D00 T10 §20` | Document: Applications and accessories                                                                                | none       |    10 |
| [x]  | `D00 T10 §21` | Document: SDK and release                                                                                             | none       |    10 |
| [x]  | `D00 T10 §22` | Doc Freshness: `sources=` and a Stale-Page Warning                                                                    | none       |     7 |
| [x]  | `D00 T10 §23` | Site Polish: Sitemap, Last-Updated, Link Health, OpenGraph                                                            | none       |     8 |
| [x]  | `D00 T10 §24` | Versioned Release Docs: Pinned Refs and Version-Scoped Rendering                                                      | none       |     4 |
| [x]  | `D00 T10 §25` | Docs Search Completeness and Accessibility                                                                            | none       |     7 |
| [x]  | `D00 T10 §26` | Document: Host tools                                                                                                  | none       |     8 |
| [x]  | `D00 T10 §27` | Document: Architecture ports and future research                                                                      | none       |     8 |
| [x]  | `D00 T10 §28` | Review-Class Nets for Site Tooling: Real Parsers, Scheduler Stress                                                    | none       |     3 |
| [x]  | `D00 T10 §29` | Retained Release Trees: Snapshots, Manifest, Live Verification                                                        | none       |     7 |
| [x]  | `D00 T10 §30` | SDK Release Docs and API Reference                                                                                    | none       |     4 |
| [x]  | `D00 T10 §31` | Docs Search Quality and Automated Accessibility Audit                                                                 | none       |     7 |
| [x]  | `D00 T10 §32` | Review-Class Nets: Fail Direction and Bounded Waits in Site Tooling                                                   | none       |     4 |
| [x]  | `D00 T10 §33` | Docs Search Typo Tolerance and Wider Accessibility Audit                                                              | none       |     9 |

### Phase 1 -- Boot platform: firmware handoff, loader, measured boot and recovery

Nothing runs until the machine boots, and a boot defect hides every defect behind it. This phase carries the loader, the boot_info handoff, boot entries and the menu, Secure Boot and TPM measurement, diagnostics, network boot, resume and recovery. Rows that wait on later work (the installer, storage repair) sit in the phase of what they wait on.

| Done | Section       | Deliverable                                                                                       | Needs      | Items |
| :--: | ------------- | ------------------------------------------------------------------------------------------------- | ---------- | ----: |
| [x]  | `D01 T01 §1`  | Canonical boot_info Field Ownership Table                                                         | none       |     5 |
| [x]  | `D01 T01 §2`  | Generated ABI Manifest and Offset Fingerprint                                                     | none       |     5 |
| [x]  | `D01 T01 §3`  | Full Bootloader/Kernel Mirror Drift Checker                                                       | none       |     7 |
| [x]  | `D01 T01 §4`  | Optional Payload Descriptor Array                                                                 | none       |     7 |
| [x]  | `D01 T01 §5`  | Module and initrd Handoff Contract                                                                | none       |     5 |
| [x]  | `D01 T01 §6`  | Handoff Memory Ownership and PMM Reservation Table                                                | none       |     7 |
| [x]  | `D01 T01 §7`  | Version Negotiation and Stale-Loader Error Path                                                   | none       |     6 |
| [x]  | `D01 T01 §8`  | Boot Protocol Documentation and Schema Changelog                                                  | none       |     6 |
| [x]  | `D01 T01 §9`  | ABI Fuzz and Compatibility Tests                                                                  | none       |     6 |
| [x]  | `D01 T01 §10` | Cross-Domain Owner Audit for Every boot_info Field                                                | none       |     8 |
| [x]  | `D01 T01 §11` | Capability Negotiation and Degraded-Feature Flags                                                 | none       |     6 |
| [x]  | `D01 T01 §12` | Common Boot-Path Provenance and Decision Record                                                   | none       |     6 |
| [x]  | `D01 T01 §13` | Anti-Rollback and Security-Version Binding                                                        | none       |     7 |
| [x]  | `D01 T01 §14` | Warm-Kernel-Update Handoff ABI                                                                    | none       |     6 |
| [x]  | `D01 T01 §15` | Capability-Gated Consumer Retrofit                                                                | none       |     9 |
| [x]  | `D01 T01 §16` | Anti-Rollback Raise Timing Hardening                                                              | none       |     9 |
| [x]  | `D01 T01 §17` | Bootloader Pre-Jump ABI Mismatch Screen                                                           | none       |    12 |
| [x]  | `D01 T01 §18` | Anti-Rollback Integration in Version Fault Diagnostics                                            | none       |     8 |
| [x]  | `D01 T01 §19` | Stale-ABI QEMU Fixture Harness                                                                    | none       |    12 |
| [x]  | `D01 T01 §20` | Bootloader Build Identity                                                                         | none       |     8 |
| [/]  | `D01 T01 §21` | Post-Ship Follow-Up Backfill (orphan cohort 2026-07-31)                                           | none       |     5 |
| [/]  | `D01 T01 §22` | Handoff Base in the Deploy-Time ABI Fingerprint                                                   | none       |     9 |
| [/]  | `D01 T01 §23` | Integrity Coverage for the Handoff Payload Body                                                   | none       |     3 |
| [x]  | `D01 T01 §24` | Scripted Anti-Rollback NVRAM Fixture Harness                                                      | none       |    17 |
| [x]  | `D01 T01 §25` | Bounded Retry for a Transient Rollback-Floor Write                                                | none       |     5 |
| [/]  | `D01 T01 §26` | Enum-VALUE Drift Detection for the Bootloader Mirror                                              | none       |     5 |
| [x]  | `D01 T01 §27` | Descriptor Capacity for the Implicit Publishers                                                   | none       |     5 |
| [x]  | `D01 T01 §28` | Per-Type Payload Length Contract Before Reserving                                                 | none       |    23 |
| [x]  | `D01 T01 §29` | One Warm-Update Descriptor, Chosen the Same Way by Both Phases                                    | none       |     4 |
| [x]  | `D01 T01 §30` | Handoff Memory Claims Must Match What PMM Actually Tracks                                         | none       |    11 |
| [x]  | `D01 T02 §1`  | UEFI Runtime Services Preservation                                                                | none       |     6 |
| [x]  | `D01 T02 §2`  | UEFI Variable Services                                                                            | none       |     7 |
| [x]  | `D01 T02 §3`  | GOP Resolution Auto-Detection                                                                     | none       |     4 |
| [x]  | `D01 T02 §4`  | SMBIOS Table Parsing                                                                              | none       |     7 |
| [x]  | `D01 T02 §5`  | Secure Boot State Detection                                                                       | none       |     3 |
| [x]  | `D01 T02 §6`  | Secure Boot Shim Chain-Loading                                                                    | none       |     6 |
| [x]  | `D01 T02 §7`  | Boot UX Polish                                                                                    | none       |     5 |
| [x]  | `D01 T02 §8`  | Serial Log Standardization and Race Fix                                                           | none       |     4 |
| [x]  | `D01 T02 §9`  | SBAT Ops, Secure Boot DB Registry Mirror, and ExitBootServices Retry                              | none       |     6 |
| [x]  | `D01 T02 §10` | UEFI Runtime Services Sleepable Lock Migration                                                    | none       |     6 |
| [x]  | `D01 T02 §11` | Unified Signed Boot Artifact (UKI-style)                                                          | none       |     7 |
| [x]  | `D01 T02 §12` | MS UEFI CA 2023 Transition + 2011 Retirement Tracking                                             | none       |     6 |
| [x]  | `D01 T02 §13` | EFI System Partition Integrity Check                                                              | none       |     6 |
| [x]  | `D01 T02 §14` | Win32 Firmware Variable + Table Surface                                                           | none       |     5 |
| [x]  | `D01 T02 §15` | Post-boot SecureBoot Revalidation                                                                 | none       |     6 |
| [x]  | `D01 T02 §16` | Signed `.initrd` / Recovery / Module PE Sections in UKI                                           | none       |    16 |
| [x]  | `D01 T02 §17` | SBAT Revocation Metadata in Boot Artifacts                                                        | none       |     6 |
| [/]  | `D01 T02 §18` | Build Idempotency: UKI SBAT Survives Incremental Rebuild                                          | bare-metal |     6 |
| [x]  | `D01 T02 §19` | Bootloader `ReadBlocks` IoAlign Compliance                                                        | none       |     4 |
| [x]  | `D01 T02 §20` | Post-Ship Follow-Up Backfill (orphan cohort 2026-07-31)                                           | none       |     9 |
| [x]  | `D01 T03 §1`  | ELF Bounds Checking                                                                               | none       |    11 |
| [x]  | `D01 T03 §2`  | ExitBootServices Retry Loop                                                                       | none       |     8 |
| [x]  | `D01 T03 §3`  | Fallback Kernel Search                                                                            | none       |     6 |
| [x]  | `D01 T03 §4`  | Serial Port Probe and COM2 Fallback                                                               | none       |     6 |
| [x]  | `D01 T03 §5`  | GOP Timeout and Graceful Degradation                                                              | none       |     5 |
| [x]  | `D01 T03 §6`  | Memory Map Overflow Detection                                                                     | none       |     6 |
| [x]  | `D01 T03 §7`  | boot.conf Validation                                                                              | none       |     7 |
| [x]  | `D01 T03 §8`  | Kernel Load Memory Allocation Fallback                                                            | none       |     7 |
| [x]  | `D01 T03 §9`  | Boot Failure Error Screen                                                                         | none       |     8 |
| [x]  | `D01 T03 §10` | ACPI SPCR Serial Port Auto-Detection                                                              | none       |     8 |
| [x]  | `D01 T03 §11` | UEFI Watchdog Timer Management                                                                    | none       |     6 |
| [x]  | `D01 T03 §12` | Memory Map Descriptor Validation                                                                  | none       |     5 |
| [x]  | `D01 T03 §13` | Boot Error Code Registry & NVRAM Persistence                                                      | none       |     7 |
| [x]  | `D01 T03 §14` | Error Screen QR Code                                                                              | none       |     8 |
| [x]  | `D01 T03 §15` | boot_info ABI Header and Bootloader Populate                                                      | none       |     9 |
| [x]  | `D01 T03 §16` | boot_info Kernel Validation, boot_hw Path, and Unit Tests                                         | none       |    10 |
| [x]  | `D01 T03 §17` | Memory Map Overlap Normalization                                                                  | none       |     8 |
| [x]  | `D01 T03 §18` | Graphical Error Screen (ChromeOS/Win11-Style)                                                     | none       |    11 |
| [x]  | `D01 T03 §19` | PT_LOAD Destination Policy                                                                        | none       |     7 |
| [x]  | `D01 T03 §20` | Boot Error History Ring -- Producer (Schema, Append Sites, NVRAM Cookie)                          | none       |     9 |
| [x]  | `D01 T03 §21` | Boot Error History Ring -- Consumer (Storage Reader, Kernel Renderer, Tests, Docs, Smoke Fixture) | none       |     7 |
| [x]  | `D01 T03 §22` | Rejected Boot-Entry Store Is a Silent Failure on Screen                                           | none       |     4 |
| [x]  | `D01 T03 §23` | No On-Screen Words Below 800x600 -- the QR Renders, the Text Does Not                             | none       |     7 |
| [x]  | `D01 T03 §24` | A Rejected Store Leaves No Trace the User Can Act On Afterwards                                   | none       |     9 |
| [x]  | `D01 T04 §1`  | Firmware Table Catalog API                                                                        | none       |     5 |
| [x]  | `D01 T04 §2`  | Physical Range and Checksum Validation                                                            | none       |     5 |
| [x]  | `D01 T04 §3`  | ACPI/SMBIOS/DTB Table Arbitration                                                                 | none       |     5 |
| [x]  | `D01 T04 §4`  | FPDT and Boot Timing Normalization                                                                | none       |     5 |
| [x]  | `D01 T04 §5`  | UEFI Memory Attributes and Runtime Properties Inventory                                           | none       |     5 |
| [x]  | `D01 T04 §6`  | ESRT Firmware Inventory Mirror                                                                    | none       |     9 |
| [x]  | `D01 T04 §7`  | UEFI Conformance Profile and EBBR Detection                                                       | none       |     9 |
| [x]  | `D01 T04 §8`  | Registry and BlackBox Firmware Report                                                             | none       |     5 |
| [x]  | `D01 T04 §9`  | Firmware Quirk Database                                                                           | none       |     5 |
| [x]  | `D01 T04 §10` | Firmware Inventory Tests and Host Decoder                                                         | bare-metal |     9 |
| [x]  | `D01 T04 §11` | Firmware Inventory Bug-Fix Debt                                                                   | none       |     4 |
| [x]  | `D01 T04 §12` | Firmware Inventory Refactor Debt                                                                  | none       |     4 |
| [x]  | `D01 T05 §1`  | Boot Device Identification via LoadedImage                                                        | none       |     7 |
| [x]  | `D01 T05 §2`  | Filesystem Access Scoped to Boot Device                                                           | none       |     6 |
| [x]  | `D01 T05 §3`  | Boot Device Info in boot_info                                                                     | none       |     5 |
| [x]  | `D01 T05 §4`  | Boot Device Type Detection                                                                        | none       |     7 |
| [x]  | `D01 T05 §5`  | Device Fallback Chain                                                                             | none       |     7 |
| [x]  | `D01 T05 §6`  | UEFI Boot Variable Reading (BootOrder, BootCurrent, BootNext)                                     | none       |     7 |
| [x]  | `D01 T05 §7`  | Partition GUID Extraction and Validation                                                          | none       |     6 |
| [x]  | `D01 T05 §8`  | Removable Media Detection                                                                         | none       |     6 |
| [x]  | `D01 T05 §9`  | Boot Device Registry Population                                                                   | none       |    10 |
| [x]  | `D01 T05 §10` | Boot Device Logging and Diagnostics                                                               | none       |     8 |
| [x]  | `D01 T05 §11` | Pre-Boot Device Health Check                                                                      | none       |     6 |
| [x]  | `D01 T05 §12` | Boot#### Load Option Decode (Diagnostics)                                                         | none       |     6 |
| [x]  | `D01 T05 §13` | Extended Boot Variable Capability Surface                                                         | none       |     9 |
| [x]  | `D01 T05 §14` | Local Boot Device Path Detail Capture                                                             | none       |     9 |
| [x]  | `D01 T06 §1`  | Boot Artifact Matrix and Manifest Format                                                          | none       |     6 |
| [x]  | `D01 T06 §2`  | Reproducible Raw/USB Image Build                                                                  | none       |     7 |
| [x]  | `D01 T06 §3`  | VHD/VHDX/VDI Conversion and Validation                                                            | none       |     6 |
| [x]  | `D01 T06 §4`  | Hybrid ISO / El Torito UEFI Boot                                                                  | none       |     5 |
| [x]  | `D01 T06 §5`  | Installer/Live/Recovery Media Detection                                                           | none       |     5 |
| [x]  | `D01 T06 §6`  | Bootloader Handoff of Media Role                                                                  | none       |     7 |
| [x]  | `D01 T06 §7`  | Artifact Signing and Manifest Verification                                                        | none       |     5 |
| [x]  | `D01 T06 §8`  | Offline Artifact Inspector                                                                        | none       |     5 |
| [x]  | `D01 T06 §9`  | CI Boot Matrix for Every Artifact                                                                 | none       |     7 |
| [x]  | `D01 T06 §10` | Release Checklist and Documentation                                                               | none       |     5 |
| [x]  | `D01 T06 §11` | Alternate Artifact Formats: UKI and Network Boot                                                  | none       |     5 |
| [x]  | `D01 T06 §12` | Windows Host Parity: Manifest Tooling                                                             | none       |     6 |
| [x]  | `D01 T06 §13` | Windows Host Parity: Disk Artifact Converters                                                     | none       |     5 |
| [x]  | `D01 T06 §14` | Windows Host Parity: Release Test Subdir + Aggregate Runner                                       | none       |     6 |
| [x]  | `D01 T07 §1`  | Boot Entry File Format                                                                            | none       |     8 |
| [x]  | `D01 T07 §2`  | Boot Entry Parser and Validator                                                                   | none       |     7 |
| [x]  | `D01 T07 §3`  | Boot Policy Merge Order                                                                           | none       |     7 |
| [x]  | `D01 T07 §4`  | ESP Store Read + Boot#### OptionalData + Policy Wiring                                            | none       |     6 |
| [x]  | `D01 T07 §5`  | Crash-Tolerant Counter Protocol + v19 ABI Validator                                               | none       |     7 |
| [x]  | `D01 T07 §6`  | Boot Menu Renderer + Countdown + Input Infrastructure                                             | none       |    16 |
| [x]  | `D01 T07 §7`  | Boot Menu Indicators, Hotkeys, hide_when_alone                                                    | none       |     9 |
| [x]  | `D01 T07 §8`  | Safe Mode, Test Mode, and Diagnostics Entries                                                     | none       |     8 |
| [x]  | `D01 T07 §9`  | A/B and Recovery Entry Integration                                                                | none       |     7 |
| [x]  | `D01 T07 §10` | Previous-Kernel and Known-Good Entries                                                            | none       |     6 |
| [x]  | `D01 T07 §11` | Boot Entry Editor Tooling                                                                         | none       |     9 |
| [x]  | `D01 T07 §12` | Policy Audit Trail and Rollback Reason Codes                                                      | none       |     7 |
| [x]  | `D01 T07 §13` | Entry Kinds: Split, UKI, Chainload, Network, Resume                                               | none       |    11 |
| [x]  | `D01 T07 §14` | Per-Entry Health-Gated Mark-Good                                                                  | none       |    14 |
| [x]  | `D01 T07 §15` | OS-Visible Loader UEFI Variables                                                                  | none       |     9 |
| [x]  | `D01 T07 §16` | Bootstrap and First-Install Entry Seeding                                                         | none       |     9 |
| [x]  | `D01 T07 §17` | Boot Entry Tests                                                                                  | none       |    17 |
| [x]  | `D01 T07 §18` | Boot Loader Interface (systemd BLI) Parity Fixes                                                  | none       |     6 |
| [x]  | `D01 T07 §19` | Repeated Top-Level Keys Defeat the Entries-Array Bound                                            | none       |     8 |
| [x]  | `D01 T07 §20` | `crc32` Header Located by String Match, Not Structurally                                          | none       |     5 |
| [x]  | `D01 T07 §21` | Published Loader Variables Need an Explicit Set-or-Clear Outcome                                  | none       |     5 |
| [x]  | `D01 T07 §22` | Store Size Cap Is Untested on Both Sides of the 16 KiB Boundary                                   | none       |     5 |
| [x]  | `D01 T07 §23` | A Rejected Store Has No Repair Path, Only a One-Entry Fallback                                    | none       |     7 |
| [x]  | `D01 T07 §24` | Boot Menu Help, Power and Saved-Default Keys                                                      | none       |     5 |
| [x]  | `D01 T07 §25` | Menu Timeout Keys and One-Off Edit                                                                | none       |     5 |
| [ ]  | `D01 T07 §26` | Cross-Section Policy, Menu and Loader-Variable Seams                                              | none       |     4 |
| [x]  | `D01 T08 §1`  | Alternate Boot Protocol Policy                                                                    | none       |     7 |
| [x]  | `D01 T08 §2`  | Multiboot2 Feature Parity Audit                                                                   | none       |     0 |
| [x]  | `D01 T08 §3`  | Multiboot2-to-boot_info Adapter                                                                   | none       |     0 |
| [x]  | `D01 T08 §4`  | Unsupported-Feature Degradation Matrix                                                            | none       |     0 |
| [x]  | `D01 T08 §5`  | GRUB / Limine / Legacy BIOS Documentation                                                         | none       |     0 |
| [x]  | `D01 T08 §6`  | Compatibility Test Images                                                                         | none       |     0 |
| [x]  | `D01 T08 §7`  | Deprecation or Promotion Gate                                                                     | none       |     3 |
| [x]  | `D01 T09 §1`  | CPUID Detection & Per-CPU Capability Capture                                                      | none       |     6 |
| [x]  | `D01 T09 §2`  | Phase 0 CPU Security Activation Order                                                             | none       |     7 |
| [x]  | `D01 T09 §3`  | Hypervisor Detection Before Timer Selection                                                       | none       |     9 |
| [x]  | `D01 T09 §4`  | AP CPU Hardening (`ap_cpu_harden()`)                                                              | none       |     6 |
| [x]  | `D01 T09 §5`  | Phase 1 XSAVE & PCID Activation Window                                                            | none       |     7 |
| [x]  | `D01 T09 §6`  | AP Feature Consistency Validation                                                                 | none       |    11 |
| [x]  | `D01 T09 §7`  | CR0/CR4 Safety-Bit Pinning                                                                        | none       |    10 |
| [x]  | `D01 T09 §8`  | MTRR/PAT AP Synchronization                                                                       | none       |     8 |
| [x]  | `D01 T09 §9`  | CPU Register State Audit Trail                                                                    | none       |    10 |
| [x]  | `D01 T09 §10` | AP Bringup Hardening & Robustness                                                                 | none       |    10 |
| [x]  | `D01 T09 §11` | Post-Ship Follow-Up Backfill (orphan cohort 2026-07-31)                                           | none       |     1 |
| [x]  | `D01 T10 §1`  | Minimal UC MMIO Mapping (`vmm_map_mmio_uc`)                                                       | none       |     7 |
| [x]  | `D01 T10 §2`  | IST Stacks for Critical Exceptions                                                                | none       |     6 |
| [x]  | `D01 T10 §3`  | Hardware Interrupt Root Cause Investigation                                                       | none       |     5 |
| [x]  | `D01 T10 §4`  | ACPI FADT Boot Architecture Flags                                                                 | none       |     6 |
| [x]  | `D01 T10 §5`  | PS/2 Controller Detection and Safe Init                                                           | none       |     6 |
| [x]  | `D01 T10 §6`  | AHCI Interrupt Hardening                                                                          | none       |     7 |
| [x]  | `D01 T10 §7`  | Resilient Boot with Graceful Degradation                                                          | none       |    12 |
| [x]  | `D01 T10 §8`  | Per-Process Page Tables (Minimal Base)                                                            | none       |     9 |
| [x]  | `D01 T10 §9`  | CPU Security Activation and Verification                                                          | none       |     6 |
| [x]  | `D01 T10 §10` | Boot Order Hardening (Timer-Last, UEFI-Safe)                                                      | none       |     5 |
| [x]  | `D01 T10 §11` | ~~`boot.conf` Subsystem Skip List~~                                                               | none       |     1 |
| [x]  | `D01 T10 §12` | Logging and Diagnostic Storage Moved to TODO-24                                                   | none       |     3 |
| [x]  | `D01 T10 §13` | CPU Feature Minimum Requirements and Verification                                                 | none       |     5 |
| [x]  | `D01 T10 §14` | Bare-Metal Test Matrix and Validation Plan                                                        | bare-metal |     6 |
| [x]  | `D01 T10 §15` | Boot Splash Spinner Bare-Metal Fix                                                                | none       |     7 |
| [x]  | `D01 T10 §16` | Post-Ship Follow-Up Backfill (orphan cohort 2026-07-31)                                           | none       |     4 |
| [x]  | `D01 T10 §17` | Emergency-Writer Robustness Residuals (from the §16 review)                                       | none       |     4 |
| [x]  | `D01 T10 §18` | Panic-Path Cross-CPU Ownership Residuals (from the §16 review)                                    | none       |     5 |
| [x]  | `D01 T10 §19` | Panic-Path Caller-String Snapshot (from the §17 review)                                           | none       |    14 |
| [x]  | `D01 T10 §20` | Panic-Path Serial Ownership Atomicity (from the §18 review)                                       | none       |     4 |
| [x]  | `D01 T10 §21` | Live CPU Online Lifecycle (split from §20)                                                        | none       |     3 |
| [x]  | `D01 T10 §22` | Dedicated NMI Entry Stub (split from §20)                                                         | none       |     4 |
| [x]  | `D01 T10 §23` | Cross-Boot Evidence Lifecycle and Epoch (from §20)                                                | none       |     5 |
| [x]  | `D01 T10 §24` | Panic Collector Publication Latency (from §20)                                                    | none       |     2 |
| [x]  | `D01 T10 §25` | GS-Validated Per-CPU Identity Cache (split from §20)                                              | none       |     4 |
| [x]  | `D01 T10 §26` | Attributable Emergency-Ledger Claim (split from §20)                                              | none       |     5 |
| [x]  | `D01 T10 §27` | Async Worker Quiescence and Terminal Bringup Handshake (from the §21 review)                      | none       |     4 |
| [/]  | `D01 T10 §28` | Cross-Boot Evidence Durability Across a Hardware Reset (from the §23 review)                      | bare-metal |     2 |
| [x]  | `D01 T10 §29` | Abandoned-Reservation Reclamation on a Parking CPU (from the §26 review)                          | none       |     4 |
| [x]  | `D01 T10 §30` | Panic-Path States That Are Documented But Not Implemented (from the §29 review)                   | none       |     3 |
| [x]  | `D01 T10 §31` | Guard Page Under the BSP Ring-0 Entry Stack                                                       | none       |     5 |
| [x]  | `D01 T10 §32` | Guarded Kernel Stack for Phase 0/1 Execution                                                      | none       |    13 |
| [x]  | `D01 T11 §1`  | ACPI MADT Parsing                                                                                 | none       |     7 |
| [x]  | `D01 T11 §2`  | LAPIC / IOAPIC Init Before PIT                                                                    | none       |     8 |
| [x]  | `D01 T11 §3`  | Conditional PIC Disable                                                                           | none       |     6 |
| [x]  | `D01 T11 §4`  | Full IDT Coverage                                                                                 | none       |     8 |
| [x]  | `D01 T11 §5`  | Dynamic IRQ Registration API                                                                      | none       |    13 |
| [x]  | `D01 T11 §6`  | Unified Timer Subsystem (UTS)                                                                     | none       |    11 |
| [x]  | `D01 T11 §7`  | LAPIC Timer Calibration                                                                           | none       |     8 |
| [x]  | `D01 T11 §8`  | Migrate Boot Splash Spinner Off PIT Callback                                                      | none       |     8 |
| [x]  | `D01 T11 §9`  | Boot Time Visualization                                                                           | none       |     6 |
| [x]  | `D01 T11 §10` | Remove Hyper-V Debug Workarounds                                                                  | none       |     6 |
| [x]  | `D01 T12 §1`  | Entropy Source Inventory and Quality Model                                                        | none       |     7 |
| [x]  | `D01 T12 §2`  | EFI_RNG_PROTOCOL Collection                                                                       | none       |     6 |
| [x]  | `D01 T12 §3`  | CPU RDRAND/RDSEED Collection                                                                      | none       |     5 |
| [x]  | `D01 T12 §4`  | TPM RNG Collection                                                                                | none       |     5 |
| [x]  | `D01 T12 §5`  | Boot Timing and Interrupt Jitter Mix-In                                                           | none       |     5 |
| [x]  | `D01 T12 §6`  | Seed File Carryover Lifecycle                                                                     | none       |     6 |
| [x]  | `D01 T12 §7`  | boot_info Seed Handoff                                                                            | none       |     7 |
| [x]  | `D01 T12 §8`  | Kernel Early CSPRNG Seeding                                                                       | none       |     7 |
| [x]  | `D01 T12 §9`  | Entropy Diagnostics and Policy Gates                                                              | none       |     6 |
| [x]  | `D01 T12 §10` | Entropy Tests                                                                                     | none       |     7 |
| [x]  | `D01 T12 §11` | Capability Gate on the Seed Payload Consumer                                                      | none       |     7 |
| [x]  | `D01 T13 §1`  | Harden TCG Event-Log Parser                                                                       | none       |     8 |
| [x]  | `D01 T13 §2`  | TPM2 Command Transport                                                                            | none       |     7 |
| [x]  | `D01 T13 §3`  | PCR Read API                                                                                      | none       |     5 |
| [x]  | `D01 T13 §4`  | PCR Replay Engine                                                                                 | none       |    10 |
| [x]  | `D01 T13 §5`  | Secure Boot Variable Measurement Reconciliation                                                   | none       |     5 |
| [x]  | `D01 T13 §6`  | Baseline Enrollment and Storage                                                                   | none       |     6 |
| [x]  | `D01 T13 §7`  | TPM NV Index Support                                                                              | none       |     3 |
| [x]  | `D01 T13 §8`  | Sealed-Secret Boot Policy Hooks                                                                   | none       |     8 |
| [x]  | `D01 T13 §11` | TPM Tests and Event-Log Fixtures                                                                  | none       |     7 |
| [x]  | `D01 T13 §12` | PCR Allocation and Policy Mask Table                                                              | none       |     5 |
| [x]  | `D01 T13 §13` | Attestation Key Provisioning and TPM2 Quote                                                       | none       |    10 |
| [x]  | `D01 T13 §9`  | Attestation Report Export                                                                         | none       |    12 |
| [x]  | `D01 T13 §14` | Post-Ship Follow-Up Backfill (orphan cohort 2026-07-31)                                           | none       |     2 |
| [x]  | `D01 T13 §15` | Trusted Enrollment Provenance                                                                     | none       |    20 |
| [x]  | `D01 T13 §16` | Baseline ABI-Manifest Identity -- Populate the Reserved Digest                                    | none       |     6 |
| [x]  | `D01 T13 §17` | Write-Locked and Monotonic NV Index Primitives                                                    | none       |     6 |
| [x]  | `D01 T13 §18` | Atomic Boot-Integrity Report Publication                                                          | none       |     7 |
| [x]  | `D01 T13 §19` | Versioned Baseline Growth and NV Index Migration                                                  | none       |    11 |
| [/]  | `D01 T13 §20` | BOOTX64.EFI On-Disk Self-Measurement                                                              | none       |     7 |
| [x]  | `D01 T13 §21` | Authenticated NV Index Lifecycle and Anti-Rollback Trust Anchor                                   | none       |     9 |
| [/]  | `D01 T13 §22` | Bootloader-Side NV Floor Read (EFI_TCG2 Adapter)                                                  | none       |     8 |
| [x]  | `D01 T13 §23` | Headless Enrollment Authorization Escape Hatch                                                    | none       |     9 |
| [x]  | `D01 T13 §24` | Extend the Bounded Sequence and Teardown Proof to Seal and Attestation                            | none       |     7 |
| [x]  | `D01 T13 §25` | Field-Level Baseline Mismatch Attribution and Status Scoping                                      | none       |     4 |
| [x]  | `D01 T13 §26` | Baseline-Blob NV Fake and Wrapper-Level Verify Coverage                                           | none       |     8 |
| [x]  | `D01 T13 §27` | Authorized NV Record Transitions                                                                  | none       |     5 |
| [x]  | `D01 T13 §28` | Crash-Consistent Record Pairing, Floor APIs and Verified-Read Boot Budget                         | none       |     6 |
| [x]  | `D01 T13 §29` | Authorized-Record Hardening                                                                       | none       |     8 |
| [x]  | `D01 T13 §30` | Headless Authorization Transport and Full-Record Binding                                          | none       |    12 |
| [x]  | `D01 T13 §31` | Loader Digest Correlation with the Firmware PCR 4 Measurement                                     | none       |     8 |
| [ ]  | `D01 T13 §32` | Authorized-Record Coverage Residue on Existing Fixtures                                           | none       |     6 |
| [ ]  | `D01 T13 §33` | PCR-Layer Contention Loses Its Retry                                                              | none       |     6 |
| [ ]  | `D01 T13 §34` | Headless Authority Lifecycle: Revocation, Durable Verdict and Aggregate Budget                    | none       |     7 |
| [ ]  | `D01 T13 §35` | Shared Fake-TIS Fixture Extension and the Coverage It Unlocks                                     | none       |     6 |
| [x]  | `D01 T14 §1`  | UEFI Pre-Kernel POST Codes                                                                        | none       |     5 |
| [ ]  | `D01 T14 §2`  | Boot Progress Named-Stage API                                                                     | none       |    11 |
| [x]  | `D01 T14 §3`  | POST-Style Hex Code Display                                                                       | none       |     6 |
| [x]  | `D01 T14 §4`  | Alive Blink / Visual Heartbeat Indicator                                                          | none       |     1 |
| [x]  | `D01 T14 §5`  | Panic Forensic Evidence                                                                           | none       |     9 |
| [ ]  | `D01 T14 §6`  | Panic QR Code                                                                                     | none       |     7 |
| [ ]  | `D01 T14 §8`  | Runtime Vital Signs Strip                                                                         | none       |     7 |
| [x]  | `D01 T14 §9`  | Boot Timeline Visualization and Import Parity                                                     | none       |     3 |
| [x]  | `D01 T14 §10` | Bootloader Build Identity Dump in BlackBox                                                        | none       |     5 |
| [x]  | `D01 T14 §11` | Boot Load Status Log (ntbtlog Parity)                                                             | none       |     7 |
| [x]  | `D01 T14 §12` | Boot Load Status Granularity (per-driver records, probe aggregation, NVMe partial init)           | none       |     6 |
| [x]  | `D01 T14 §14` | Bootloader Reservation of the Panic-Evidence Page (`0x80000`)                                     | none       |     4 |
| [x]  | `D01 T14 §16` | Boot Timeline Export Formats (SVG and Chrome Trace)                                               | none       |     4 |
| [x]  | `D01 T14 §18` | Pre-Serial `serial_early_print` Drives a Poisoned UART Port                                       | none       |     3 |
| [x]  | `D01 T14 §20` | The `.bss` Poison Discipline Stops at the Serial Path                                             | none       |     9 |
| [ ]  | `D01 T15 §1`  | 4-Digit POST Code System (0x0000-0xFFFF)                                                          | none       |     8 |
| [ ]  | `D01 T15 §2`  | POST Codes in Every Boot Function                                                                 | none       |     9 |
| [ ]  | `D01 T15 §3`  | Embedded 5x7 Bitmap Micro-Font                                                                    | none       |    10 |
| [ ]  | `D01 T15 §4`  | Tier 1: Pre-Splash VPD Renderer                                                                   | none       |    12 |
| [x]  | `D01 T15 §5`  | `boot.conf` `postbars` Configuration                                                              | none       |     7 |
| [x]  | `D01 T15 §6`  | Named Stages with TSC Timing                                                                      | none       |     5 |
| [x]  | `D01 T15 §7`  | Status Indicators and Progress Bar                                                                | none       |     4 |
| [ ]  | `D01 T15 §8`  | NVRAM Crash Persistence and "Last Boot Failed" Display                                            | none       |     9 |
| [/]  | `D01 T15 §9`  | Tier 2: Splash-Integrated Progress                                                                | none       |     5 |
| [x]  | `D01 T15 §10` | Seamless Tier Transition                                                                          | none       |     6 |
| [x]  | `D01 T15 §11` | Phase Grouping and Diagnostic Layout                                                              | none       |     6 |
| [x]  | `D01 T15 §12` | HV_BAR Removal and Migration                                                                      | none       |     5 |
| [x]  | `D01 T15 §13` | Panic Integration and Failure Highlighting                                                        | none       |     6 |
| [x]  | `D01 T16 §1`  | NVMe Controller Discovery and BAR Mapping                                                         | none       |     9 |
| [x]  | `D01 T16 §2`  | Admin Queue Setup and Identify Commands                                                           | none       |     8 |
| [x]  | `D01 T16 §3`  | I/O Queue Creation and Sector Read/Write                                                          | none       |     9 |
| [x]  | `D01 T16 §4`  | Block Device Registration and VFS Integration                                                     | none       |     5 |
| [x]  | `D01 T16 §5`  | Advanced NVMe parity backlog (D04 T08 owned)                                                      | none       |    10 |
| [x]  | `D01 T16 §6`  | NVMe Controller Lifecycle: Clean Shutdown, Cache Flush, and I/O Validation                        | none       |     6 |
| [x]  | `D01 T17 §1`  | xHCI Controller Bring-Up and Port Scan                                                            | none       |     5 |
| [x]  | `D01 T17 §2`  | USB Device Enumeration and Configuration                                                          | none       |     6 |
| [x]  | `D01 T17 §3`  | USB MSC BOT (Bulk-Only Transport) Driver                                                          | none       |     7 |
| [x]  | `D01 T17 §4`  | Block Device Registration and VFS Integration                                                     | none       |     5 |
| [x]  | `D01 T17 §5`  | Interrupt-Driven Hot-Plug and Post-Boot Lifecycle                                                 | none       |     8 |
| [x]  | `D01 T17 §6`  | Hardware Compatibility -- Non-Intel xHCI Vendors                                                  | none       |     7 |
| [x]  | `D01 T18 §1`  | xHCI Interrupt Endpoint Setup                                                                     | none       |     6 |
| [x]  | `D01 T18 §2`  | Interrupt Transfer Polling                                                                        | none       |     6 |
| [x]  | `D01 T18 §3`  | USB HID Boot-Protocol Keyboard Driver                                                             | none       |     9 |
| [x]  | `D01 T18 §4`  | USB HID Boot-Protocol Mouse Driver                                                                | none       |     7 |
| [x]  | `D01 T18 §5`  | Input Source Coexistence                                                                          | none       |     7 |
| [x]  | `D01 T18 §6`  | Hot-Plug Keyboard/Mouse Detection                                                                 | none       |     6 |
| [x]  | `D01 T18 §7`  | USB Input Diagnostic Logging                                                                      | none       |     4 |
| [x]  | `D01 T19 §1`  | SCSI REQUEST SENSE and Error Classification                                                       | none       |     6 |
| [x]  | `D01 T19 §2`  | TEST UNIT READY Poll Loop                                                                         | none       |     6 |
| [x]  | `D01 T19 §3`  | MSC BOT Retry on Transient Errors                                                                 | none       |    10 |
| [x]  | `D01 T19 §4`  | USB Transport Error Recovery (Stall/Halt Handling)                                                | none       |     9 |
| [ ]  | `D01 T19 §5`  | Bulk Transfer Timeouts                                                                            | none       |    10 |
| [ ]  | `D01 T19 §7`  | XUSB2PR Port-Ready Polling (Intel EHCI→xHCI)                                                      | none       |     5 |
| [x]  | `D01 T19 §6`  | Remove sleep_ms(2000) Hack                                                                        | none       |     4 |
| [x]  | `D01 T19 §8`  | klog_disk_flush Bounded Loop                                                                      | none       |     7 |
| [x]  | `D01 T19 §9`  | klog Deferred Flush Mode                                                                          | none       |     6 |
| [x]  | `D01 T19 §10` | Boot Media Speed Detection                                                                        | none       |     6 |
| [x]  | `D01 T19 §11` | EHCI/UHCI Companion Controller Fallback                                                           | none       |     9 |
| [x]  | `D01 T19 §12` | USB Boot Diagnostic Report                                                                        | none       |     7 |
| [x]  | `D01 T19 §13` | Single-Pass Per-Subsystem Log Routing                                                             | none       |     5 |
| [x]  | `D01 T19 §14` | IXFS Boot Tests: Slow-Media-Aware                                                                 | none       |     7 |
| [ ]  | `D01 T19 §15` | Flush Progress on Splash Diagnostic Line                                                          | none       |     6 |
| [x]  | `D01 T19 §16` | xHCI Command Ring + BOT Transport SMP Serialization                                               | none       |     6 |
| [ ]  | `D01 T19 §17` | USB-Disk Boot Legs in the Smoke Matrix                                                            | none       |     5 |
| [ ]  | `D01 T20 §1`  | Bootloader Allocates xHCI DMA Structures                                                          | none       |    14 |
| [ ]  | `D01 T20 §2`  | Bootloader Performs USBLEGSUP + Controller Takeover                                               | none       |    12 |
| [/]  | `D01 T20 §3`  | Bootloader Enumerates Devices with Persistent State                                               | none       |     6 |
| [/]  | `D01 T20 §4`  | boot_info Passes Controller + Device DMA State                                                    | none       |     6 |
| [ ]  | `D01 T20 §5`  | Kernel Inherits Controller Without Halt/Reset                                                     | none       |    10 |
| [ ]  | `D01 T20 §6`  | Kernel Registers MSC Devices from boot_info Geometry                                              | none       |     4 |
| [ ]  | `D01 T20 §7`  | Fallback: Detect Corrupt State, Revert to TODO-17 Path                                            | none       |     6 |
| [x]  | `D01 T21 §1`  | Boot Metadata Structure                                                                           | none       |     7 |
| [x]  | `D01 T21 §2`  | Dual-Slot Disk Layout                                                                             | none       |     7 |
| [x]  | `D01 T21 §3`  | Bootloader Slot Selection                                                                         | none       |    11 |
| [x]  | `D01 T21 §4`  | Boot Failure Counting and Rollback                                                                | none       |     9 |
| [x]  | `D01 T21 §5`  | Kernel `mark_boot_successful()`                                                                   | none       |     7 |
| [x]  | `D01 T21 §6`  | Slot Status in Boot Diagnostics                                                                   | none       |     5 |
| [x]  | `D01 T21 §7`  | Boot Metadata Integrity and Atomic Writes                                                         | none       |     8 |
| [x]  | `D01 T21 §8`  | Per-Slot Anti-Rollback Version Floor                                                              | none       |     6 |
| [x]  | `D01 T22 §1`  | Recovery Partition in Disk Layout                                                                 | none       |     7 |
| [ ]  | `D01 T22 §2`  | Recovery Bootloader                                                                               | none       |     8 |
| [ ]  | `D01 T22 §3`  | Filesystem Integrity Check                                                                        | none       |     7 |
| [ ]  | `D01 T22 §4`  | Backup Kernel Restore                                                                             | none       |     5 |
| [ ]  | `D01 T22 §5`  | Boot Metadata Reset                                                                               | none       |     4 |
| [ ]  | `D01 T22 §6`  | NVRAM Boot Entry Reconstruction                                                                   | none       |     6 |
| [ ]  | `D01 T22 §7`  | Recovery UI                                                                                       | none       |     6 |
| [ ]  | `D01 T22 §8`  | Gate IXFS fsck Repairs on Complete, Readable Scans                                                | none       |     6 |
| [ ]  | `D01 T13 §10` | Recovery and Mismatch UX                                                                          | none       |     5 |
| [ ]  | `D01 T13 §36` | Kernel Event-Log Parse Capacity Beyond 256 Events                                                 | none       |     3 |
| [ ]  | `D01 T13 §37` | First Provisioning of the Anti-Rollback Anchors                                                   | none       |     4 |
| [x]  | `D01 T23 §1`  | Software Watchdog via LAPIC NMI Timer                                                             | none       |     9 |
| [x]  | `D01 T23 §2`  | Per-Phase Timeout Configuration                                                                   | none       |     6 |
| [x]  | `D01 T23 §3`  | Watchdog Pet at boot_progress()                                                                   | none       |     5 |
| [x]  | `D01 T23 §4`  | Watchdog-Triggered Reboot with Diagnostics                                                        | none       |     7 |
| [x]  | `D01 T23 §5`  | ACPI WDAT Hardware Watchdog                                                                       | none       |     8 |
| [x]  | `D01 T23 §6`  | Watchdog Status in VPD                                                                            | none       |     4 |
| [ ]  | `D01 T23 §7`  | Post-Ship Follow-Up Backfill (orphan cohort 2026-07-31)                                           | none       |     4 |
| [x]  | `D01 T24 §1`  | Disk Tooling -- Add BlackBox Partition to GPT Layout                                              | none       |    10 |
| [x]  | `D01 T24 §2`  | Build Pipeline -- Format and Populate BlackBox                                                    | none       |     5 |
| [x]  | `D01 T24 §3`  | Kernel Mount -- Discover "BlackBox" GPT Name, Mount as X:\                                        | none       |     6 |
| [x]  | `D01 T24 §4`  | Directory Skeleton -- Create Dirs on First Boot                                                   | none       |     6 |
| [ ]  | `D01 T24 §5`  | Klog Migration -- Move All Log Output to X:\Logs\                                                 | none       |    10 |
| [x]  | `D01 T24 §6`  | Boot Logs -- Per-Boot Session Files to X:\Boot\                                                   | none       |     6 |
| [x]  | `D01 T24 §7`  | Crash Dump Path -- crash_recovery.log to X:\Crash\                                                | none       |     4 |
| [x]  | `D01 T24 §8`  | Perf and Diag -- boot-profile, hwdump to X:\Perf\ and X:\Diag\                                    | none       |     6 |
| [x]  | `D01 T24 §9`  | Host Tools -- SDK Reads BlackBox from Disk Image                                                  | none       |     5 |
| [ ]  | `D01 T24 §10` | Disk Space Management -- Log Aging, Quota, Cleanup                                                | none       |    12 |
| [x]  | `D01 T24 §11` | FAT32 Volume Label -- Set "BLACKBOX" at Format Time                                               | none       |     5 |
| [x]  | `D01 T24 §12` | Partition Health -- fsck on Mount, Dirty-Bit Check                                                | none       |    10 |
| [ ]  | `D01 T24 §13` | WER Staging Area -- Error Reports in X:\Crash\WER\                                                | none       |     9 |
| [x]  | `D01 T24 §14` | A/B Layout Compatibility -- 4+ Partition Coexistence                                              | none       |     6 |
| [/]  | `D01 T24 §15` | Boot Platform TODO Updates -- XREFs and Domain Sync                                               | none       |    11 |
| [x]  | `D01 T24 §16` | Crash/WER Report Retention in Partition Cleanup                                                   | none       |     6 |
| [x]  | `D01 T25 §1`  | UEFI SNP/PXE Protocol Discovery                                                                   | none       |     5 |
| [ ]  | `D01 T25 §2`  | DHCP/PXE Provenance Capture                                                                       | none       |     6 |
| [x]  | `D01 T25 §3`  | TFTP Kernel and boot.conf Load                                                                    | none       |     5 |
| [x]  | `D01 T25 §4`  | UEFI HTTP Boot Load Path                                                                          | none       |     6 |
| [ ]  | `D01 T25 §5`  | Network Boot Asset Integrity                                                                      | none       |     9 |
| [ ]  | `D01 T25 §6`  | boot_info Network Provenance                                                                      | none       |     8 |
| [x]  | `D01 T25 §7`  | Fallback Ordering with Local Media                                                                | none       |     5 |
| [/]  | `D01 T25 §8`  | Recovery and Installer over Network                                                               | none       |     6 |
| [ ]  | `D01 T25 §9`  | Network Boot Diagnostics and BlackBox Report                                                      | none       |     5 |
| [ ]  | `D01 T25 §10` | PXE/HTTP Boot Tests                                                                               | bare-metal |     5 |
| [x]  | `D01 T26 §1`  | Hibernation Image Metadata Format                                                                 | none       |     7 |
| [x]  | `D01 T26 §2`  | Bootloader Image Discovery                                                                        | none       |     5 |
| [x]  | `D01 T26 §3`  | Resume Eligibility Policy                                                                         | none       |     5 |
| [x]  | `D01 T26 §4`  | Integrity and Version Validation                                                                  | none       |     7 |
| [x]  | `D01 T26 §5`  | boot_info Resume Handoff                                                                          | none       |     5 |
| [x]  | `D01 T26 §6`  | Resume Failure Fallback                                                                           | none       |     5 |
| [x]  | `D01 T26 §7`  | Fast Startup Mode                                                                                 | none       |     5 |
| [x]  | `D01 T26 §8`  | Diagnostics and BlackBox Resume Report                                                            | none       |     5 |
| [x]  | `D01 T26 §9`  | Resume Tests                                                                                      | none       |     8 |
| [x]  | `D01 T27 §1`  | UEFI Multi-OS Detection and Chainload Entries                                                     | none       |     7 |
| [x]  | `D01 T27 §2`  | Firmware Update Advisor (read-only LVFS-style)                                                    | none       |     9 |
| [ ]  | `D01 T27 §3`  | UEFI Memory Attributes (W^X)                                                                      | none       |     6 |
| [x]  | `D01 T27 §4`  | Multi-GPU GOP Handle Enumeration                                                                  | none       |     5 |
| [x]  | `D01 T27 §5`  | Secure Boot Enforcement Policy                                                                    | none       |     6 |
| [x]  | `D01 T27 §6`  | SMBIOS Extended Type Parsing                                                                      | none       |     5 |
| [/]  | `D01 T27 §7`  | DBX Revocation List Sync                                                                          | none       |    10 |
| [ ]  | `D01 T27 §9`  | Stable Ids for Discovered OS Entries                                                              | none       |     4 |
| [x]  | `D01 T28 §1`  | Boot Certification Matrix Schema                                                                  | none       |     5 |
| [x]  | `D01 T28 §2`  | VM Automation Suite                                                                               | none       |     8 |
| [ ]  | `D01 T28 §3`  | Storage/Media Boot Suite                                                                          | none       |     7 |
| [ ]  | `D01 T28 §4`  | Security Boot Suite                                                                               | none       |     5 |
| [ ]  | `D01 T28 §5`  | Recovery and Rollback Suite                                                                       | none       |     8 |
| [x]  | `D01 T28 §6`  | Network Boot Suite                                                                                | none       |     5 |
| [x]  | `D01 T28 §7`  | Bare-Metal Lab Inventory                                                                          | none       |     5 |
| [ ]  | `D01 T28 §8`  | Boot Support Bundle Collector                                                                     | none       |     6 |
| [ ]  | `D01 T28 §9`  | Release Gate and Dashboard                                                                        | none       |     6 |
| [ ]  | `D01 T28 §10` | Certification Docs                                                                                | none       |     5 |
| [ ]  | `D01 T28 §11` | Firmware Sanity Certification Gate                                                                | none       |     5 |
| [ ]  | `D01 T28 §12` | Error-Screen Pixel Oracle -- Assert What Was Drawn, Not What the Loader Says It Drew              | none       |     4 |
| [x]  | `D01 T29 §1`  | Per-Phase Boot Perf Budgets and Threshold Alarms                                                  | none       |     8 |
| [x]  | `D01 T29 §2`  | Boot Health Audit JSON (`X:\Diag\boot-health.json`)                                               | none       |     6 |
| [x]  | `D01 T29 §3`  | Boot Perf Trend File + Regression Detection                                                       | none       |     6 |
| [x]  | `D01 T29 §4`  | SMBIOS Init Profiling + Optimization                                                              | none       |     6 |
| [x]  | `D01 T29 §5`  | MAT W^X Violation Root-Cause Attribution                                                          | none       |     5 |
| [x]  | `D01 T29 §6`  | Mouse PS/2 Init Profiling + Optimization                                                          | none       |     7 |
| [ ]  | `D01 T29 §7`  | Async Font and Icon Loader (Post-Desktop-Ready)                                                   | none       |     6 |
| [/]  | `D01 T29 §8`  | Boot Heartbeat Telemetry During Long Phases                                                       | none       |     4 |
| [ ]  | `D01 T29 §9`  | EXEC Step Latency Profile                                                                         | none       |     5 |
| [ ]  | `D01 T29 §10` | UEFI RT SetVariable Latency                                                                       | none       |     4 |
| [ ]  | `D01 T29 §11` | Phase-3 X:\Diag JSON Writer Batching                                                              | none       |     5 |
| [ ]  | `D01 T29 §12` | AVX-512 Throttle Policy When APERF/MPERF Unavailable                                              | none       |     3 |
| [/]  | `D01 T29 §13` | Boot History Ring Depth + Format                                                                  | none       |     4 |
| [x]  | `D01 T29 §14` | FAT32 Dirty-Mount fsck Cost in VFS Step                                                           | none       |     4 |
| [ ]  | `D01 T29 §15` | User-Mode Binary Spawn Latency (~1s task_create → ELF entry)                                      | none       |     4 |
| [ ]  | `D01 T29 §16` | TSC Frequency Variability Under Hypervisor                                                        | none       |     5 |
| [ ]  | `D01 T29 §17` | PAT WC -> WT Hypervisor Trap Quirk                                                                | none       |     5 |
| [ ]  | `D01 T29 §18` | Boot Critical-Path / Dependency / Resource-Wait Attribution                                       | none       |     7 |
| [ ]  | `D01 T29 §20` | Boot Trend JSON Writer Hardening                                                                  | none       |     5 |
| [ ]  | `D01 T29 §21` | Per-Driver Degraded-State Registry for Storage                                                    | none       |     1 |

### Phase 2 -- Kernel core: objects, processes, syscalls, the registry and executables

The kernel's own model comes straight after boot because every driver, filesystem and user-mode API is written against it: the object manager, processes and threads, the syscall tables, security, the registry, exceptions and the binary loaders.

| Done | Section       | Deliverable                                                                                       | Needs      | Items |
| :--: | ------------- | ------------------------------------------------------------------------------------------------- | ---------- | ----: |
| [x]  | `D02 T01 §1`  | Boot Init Infrastructure                                                                          | none       |    11 |
| [x]  | `D02 T01 §2`  | Phase 0 -- Critical Init (Interrupts Disabled)                                                    | none       |    21 |
| [x]  | `D02 T01 §3`  | Phase 1 -- Platform Services (Interrupts Enabled at End)                                          | none       |    25 |
| [ ]  | `D02 T01 §4`  | Phase 2 -- System Services                                                                        | none       |    23 |
| [x]  | `D02 T01 §5`  | Phase 3 -- User Platform                                                                          | none       |    11 |
| [ ]  | `D02 T01 §6`  | Dependency Gates                                                                                  | none       |     5 |
| [ ]  | `D02 T01 §7`  | Failure Policy                                                                                    | none       |    12 |
| [x]  | `D02 T01 §8`  | Code Cleanup                                                                                      | none       |    11 |
| [ ]  | `D02 T01 §9`  | Degraded-Boot Recovery Screen                                                                     | none       |    10 |
| [x]  | `D02 T01 §10` | POST Code + UEFI Variable Log                                                                     | none       |     9 |
| [ ]  | `D02 T01 §11` | Deferred Init for Non-Critical Subsystems                                                         | none       |    12 |
| [ ]  | `D02 T01 §12` | Boot Performance Regression Detection                                                             | none       |    10 |
| [ ]  | `D02 T01 §13` | Async Subsystem Init (SMP Parallel)                                                               | none       |    15 |
| [x]  | `D02 T02 §1`  | Boot Argument Schema and Parser                                                                   | none       |     7 |
| [x]  | `D02 T02 §2`  | `kernel_config_t` Immutable Phase 0 Snapshot                                                      | none       |     6 |
| [/]  | `D02 T02 §4`  | ControlSet and LastKnownGood Selection                                                            | none       |     6 |
| [/]  | `D02 T02 §3`  | Registry-Backed Policy Merge                                                                      | none       |     7 |
| [x]  | `D02 T02 §5`  | Safe Mode and Recovery Policy Object                                                              | none       |     7 |
| [x]  | `D02 T02 §6`  | Runtime Tunable Registry                                                                          | none       |     7 |
| [x]  | `D02 T02 §7`  | Feature Flag Gates and Experiment Cohorts                                                         | none       |     6 |
| [x]  | `D02 T02 §8`  | Native Query/Set Config Syscalls                                                                  | none       |     6 |
| [x]  | `D02 T02 §9`  | Policy Lock Phases and Tamper Audit                                                               | none       |     7 |
| [x]  | `D02 T02 §10` | Boot Status Policy and Boot Success Ledger                                                        | none       |     7 |
| [x]  | `D02 T02 §11` | Config Dump, Tests, and Docs                                                                      | none       |     6 |
| [ ]  | `D02 T02 §12` | Post-Ship Follow-Up Backfill (orphan cohort 2026-07-31)                                           | none       |     1 |
| [ ]  | `D02 T03 §1`  | Freestanding String Library                                                                       | none       |    12 |
| [ ]  | `D02 T03 §2`  | Complete Floating-Point Math Library                                                              | none       |    15 |
| [x]  | `D02 T03 §3`  | LZ4 Block Compressor                                                                              | none       |    10 |
| [x]  | `D02 T03 §4`  | miniz Deflate / Inflate + ZIP                                                                     | none       |    10 |
| [x]  | `D02 T03 §5`  | Monocypher Crypto Primitives + Kernel CSPRNG                                                      | none       |    13 |
| [ ]  | `D02 T03 §6`  | cJSON DOM Parser                                                                                  | none       |    11 |
| [/]  | `D02 T03 §7`  | Mbed TLS Freestanding Port                                                                        | none       |     6 |
| [x]  | `D02 T03 §8`  | Checksum and Codec Dispatch (CRC32 / CRC32C / base64 / hex)                                       | none       |     7 |
| [/]  | `D02 T03 §9`  | Zstandard (zstd) Compression                                                                      | none       |     7 |
| [x]  | `D02 T03 §10` | SHA-3 / SHAKE (Keccak) Hash Family                                                                | none       |     6 |
| [x]  | `D02 T04 §1`  | Boot-Phase Aware klog Init                                                                        | none       |     6 |
| [ ]  | `D02 T04 §2`  | Per-Subsystem Log Splitting                                                                       | none       |     9 |
| [x]  | `D02 T04 §3`  | Per-Subsystem Verbosity Control                                                                   | none       |     6 |
| [ ]  | `D02 T04 §4`  | Log Rotation                                                                                      | none       |    10 |
| [x]  | `D02 T04 §5`  | Rate Limiting                                                                                     | none       |     6 |
| [x]  | `D02 T04 §6`  | Structured JSON Log Events                                                                        | none       |     9 |
| [x]  | `D02 T04 §7`  | ETW Tracing Syscalls Wired to SSDT                                                                | none       |     9 |
| [ ]  | `D02 T04 §8`  | Crash-Persistent Log Capture                                                                      | none       |    14 |
| [x]  | `D02 T04 §9`  | Per-Entry Context Metadata                                                                        | none       |    10 |
| [ ]  | `D02 T04 §10` | Log Integrity Verification                                                                        | none       |    10 |
| [/]  | `D02 T04 §11` | ETW Provider Registration and Session Filtering                                                   | none       |     9 |
| [x]  | `D02 T04 §12` | ETW Advanced Capture -- Stack-Walk, Autologger, Self-Describing Schema                            | none       |     6 |
| [x]  | `D02 T04 §13` | Rotated-Log Compression                                                                           | none       |     8 |
| [ ]  | `D02 T04 §14` | Serial Timestamp Render Bound                                                                     | none       |     2 |
| [ ]  | `D02 T04 §15` | Post-Ship Follow-Up Backfill (orphan cohort 2026-07-31)                                           | none       |     7 |
| [ ]  | `D02 T04 §16` | Klog Assertions and Scans That Depend on Nothing Else Having Logged                               | none       |     6 |
| [ ]  | `D02 T04 §17` | Bounded Wait Until the Sinks Have Caught Up to a Given Sequence                                   | none       |     4 |
| [x]  | `D02 T04 §18` | The Previous Boot's Crash Region Is Claimed Before Any Allocation                                 | none       |     5 |
| [ ]  | `D02 T05 §1`  | OBJECT_HEADER and OBJECT_TYPE Infrastructure                                                      | none       |     8 |
| [x]  | `D02 T05 §2`  | Reference Counting and Object Lifetime                                                            | none       |     6 |
| [ ]  | `D02 T05 §3`  | Per-Process Handle Table                                                                          | none       |    15 |
| [ ]  | `D02 T05 §4`  | Object Namespace                                                                                  | none       |    20 |
| [ ]  | `D02 T05 §5`  | File, Process, Thread object types                                                                | none       |     5 |
| [ ]  | `D02 T05 §6`  | Synchronisation Object Types                                                                      | none       |    11 |
| [ ]  | `D02 T05 §7`  | Section (Shared Memory) Object Type                                                               | none       |     9 |
| [x]  | `D02 T05 §8`  | Security Descriptor Integration                                                                   | none       |     7 |
| [ ]  | `D02 T05 §10` | Handle Inheritance Across CreateProcess                                                           | none       |     7 |
| [ ]  | `D02 T05 §11` | Unified Kernel-User Namespace Browser API                                                         | none       |     5 |
| [x]  | `D02 T05 §12` | Per-Type Object and Handle Statistics                                                             | none       |     9 |
| [x]  | `D02 T05 §13` | Object Callbacks -- Handle Operation Filtering                                                    | none       |    14 |
| [ ]  | `D02 T05 §14` | Per-Process Handle Quota                                                                          | none       |     9 |
| [ ]  | `D02 T05 §15` | Handle Tracing and Leak Detection                                                                 | none       |    12 |
| [x]  | `D02 T06 §1`  | Executive Headers and Namespace                                                                   | none       |     6 |
| [x]  | `D02 T06 §2`  | Interlocked SLIST                                                                                 | none       |     6 |
| [x]  | `D02 T06 §3`  | Rundown Protection                                                                                | none       |     6 |
| [x]  | `D02 T06 §4`  | Callback Objects                                                                                  | none       |     9 |
| [x]  | `D02 T06 §5`  | Lookaside Lists (NPaged, Paged, and Ex)                                                           | none       |     7 |
| [x]  | `D02 T06 §6`  | Fast References                                                                                   | none       |     5 |
| [x]  | `D02 T06 §7`  | Generic Tables and Bitmaps                                                                        | none       |     6 |
| [x]  | `D02 T06 §8`  | Push Locks                                                                                        | none       |     4 |
| [x]  | `D02 T06 §9`  | Fast and Guarded Mutexes                                                                          | none       |     4 |
| [x]  | `D02 T06 §10` | Executive Resource Wrapper                                                                        | none       |     6 |
| [x]  | `D02 T06 §11` | Run-Once Initialization                                                                           | none       |     4 |
| [x]  | `D02 T06 §12` | Worker Items, Delayed Work, and Ex Timers                                                         | none       |     6 |
| [x]  | `D02 T06 §13` | Bugcheck Reason Callbacks                                                                         | none       |     5 |
| [x]  | `D02 T06 §14` | Executive Verifier Hooks                                                                          | none       |     5 |
| [x]  | `D02 T07 §1`  | `KIRQL` Type, Constants, and Core Contract                                                        | none       |     6 |
| [x]  | `D02 T07 §2`  | Per-CPU IRQL Tracking and Transition Primitives                                                   | none       |     7 |
| [ ]  | `D02 T07 §3`  | Interrupt Entry/Exit IRQL Integration                                                             | none       |     8 |
| [x]  | `D02 T07 §4`  | DPC Object Type and Per-CPU Queue                                                                 | none       |     6 |
| [x]  | `D02 T07 §5`  | DPC Drain Loop at `DISPATCH_LEVEL`                                                                | none       |     7 |
| [ ]  | `D02 T07 §6`  | Timer/APIC Scheduling Path for DPC Dispatch                                                       | none       |     7 |
| [x]  | `D02 T07 §7`  | DPC Targeting, Importance, and Flush                                                              | none       |    10 |
| [x]  | `D02 T07 §8`  | Threaded DPCs (`PASSIVE_LEVEL` DPC Variant)                                                       | none       |     6 |
| [ ]  | `D02 T07 §9`  | Timer-DPC Association                                                                             | none       |     8 |
| [ ]  | `D02 T07 §10` | Driver Migration and Workqueue Contract Split                                                     | none       |     8 |
| [ ]  | `D02 T07 §11` | APC Object Type and Per-Thread Queues                                                             | none       |    13 |
| [ ]  | `D02 T07 §12` | APC Delivery Mechanism (KiDeliverApc)                                                             | none       |    10 |
| [ ]  | `D02 T07 §13` | IRQL Violation Traps and Structured Telemetry                                                     | none       |     8 |
| [ ]  | `D02 T07 §14` | Budgeted DPC/APC Fairness and Starvation Watchdog                                                 | none       |    10 |
| [ ]  | `D02 T07 §15` | Threaded DPC List Synchronization                                                                 | none       |     9 |
| [x]  | `D02 T07 §16` | KeFlushQueuedDpcs Threaded DPC Completion                                                         | none       |     6 |
| [x]  | `D02 T07 §17` | Per-CPU Threaded DPC Worker Affinity                                                              | none       |     6 |
| [x]  | `D02 T07 §18` | System Worker Thread Pool (Long-Period Periodic Callbacks)                                        | none       |    12 |
| [x]  | `D02 T07 §19` | Interference-Proof DPC Depth-Accounting Test Seam                                                 | none       |     5 |
| [x]  | `D02 T08 §1`  | `FILETIME` Type, Epoch Constants, and Conversion Math                                             | none       |     6 |
| [x]  | `D02 T08 §2`  | Monotonic Nanosecond Clock Source Selection                                                       | none       |     9 |
| [x]  | `D02 T08 §3`  | Invariant TSC Detection and Per-CPU Offset Calibration                                            | none       |    11 |
| [x]  | `D02 T08 §4`  | HPET Standalone Driver                                                                            | none       |     7 |
| [x]  | `D02 T08 §5`  | Wall Clock Init from UEFI GetTime / RTC                                                           | none       |     8 |
| [x]  | `D02 T08 §6`  | Kernel Time Service                                                                               | none       |     6 |
| [x]  | `D02 T08 §7`  | Interrupt Time and Unbiased Interrupt Time APIs                                                   | none       |     5 |
| [x]  | `D02 T08 §8`  | Timer Resolution Management                                                                       | none       |     8 |
| [x]  | `D02 T08 §9`  | `NtQuerySystemTime`, `NtSetSystemTime`, `NtQueryPerformanceCounter`                               | none       |     6 |
| [x]  | `D02 T08 §10` | Precise System Time (`KeQuerySystemTimePrecise`)                                                  | none       |     3 |
| [x]  | `D02 T08 §11` | Timezone Bias and DST Management                                                                  | none       |     5 |
| [x]  | `D02 T08 §12` | KUSER_SHARED_DATA Time Field Updates                                                              | none       |    10 |
| [x]  | `D02 T08 §13` | Filesystem Timestamp Encoding (FAT32 + NTFS)                                                      | none       |     5 |
| [x]  | `D02 T08 §14` | Suspend/Hibernate Time Bias Tracking                                                              | none       |     6 |
| [x]  | `D02 T08 §15` | Leap Second Policy                                                                                | none       |     6 |
| [x]  | `D02 T08 §16` | Coarse Time Fast Path                                                                             | none       |     6 |
| [x]  | `D02 T08 §17` | NTP Clock Adjustment Hooks                                                                        | none       |     9 |
| [ ]  | `D02 T08 §18` | Clocksource Quality Watchdog                                                                      | none       |    11 |
| [x]  | `D02 T08 §19` | NTP Continuous Wall-Time Discipline                                                               | none       |     5 |
| [x]  | `D02 T09 §1`  | XSAVE / XRSTOR State Management                                                                   | none       |    13 |
| [x]  | `D02 T09 §2`  | AVX/AVX2 Kernel Memops + Framebuffer Blit                                                         | none       |     6 |
| [x]  | `D02 T09 §3`  | AVX-512 Opt-In + Future Silicon Detection                                                         | none       |     7 |
| [x]  | `D02 T09 §4`  | MSR Management Infrastructure                                                                     | none       |     6 |
| [x]  | `D02 T09 §5`  | UMIP + PKU Protection Keys                                                                        | none       |    10 |
| [x]  | `D02 T09 §6`  | 1 GiB Huge Pages + Write-Combining PAT                                                            | none       |     8 |
| [/]  | `D02 T09 §7`  | FRED Unified Event Delivery                                                                       | bare-metal |    11 |
| [/]  | `D02 T09 §8`  | LKGS Fast GS-Base Swap                                                                            | bare-metal |     7 |
| [x]  | `D02 T09 §9`  | CPU Topology: Zen Chiplets + Intel Hybrid                                                         | none       |     7 |
| [x]  | `D02 T09 §10` | Performance Monitoring Counters: Intel + AMD                                                      | none       |    11 |
| [x]  | `D02 T09 §11` | OSVW Errata + RDTSCP Processor ID Setup                                                           | none       |     5 |
| [/]  | `D02 T09 §12` | AMD IBS Profiling (Stretch)                                                                       | bare-metal |     9 |
| [x]  | `D02 T09 §13` | Virtualization Detection (AMD-V + Intel VT-x)                                                     | none       |     4 |
| [ ]  | `D02 T09 §14` | Boot Self-Benchmark + Auto-Tune                                                                   | none       |    10 |
| [x]  | `D02 T09 §15` | Future Silicon Extensions                                                                         | none       |     6 |
| [/]  | `D02 T09 §16` | Boot Page Tables: 1 GiB Pages From entry.asm                                                      | bare-metal |     7 |
| [x]  | `D02 T09 §17` | AMX Tile State + XFD Dynamic XSAVE                                                                | bare-metal |    11 |
| [ ]  | `D02 T09 §18` | Split-Lock + Bus-Lock Detection                                                                   | none       |     8 |
| [x]  | `D02 T09 §19` | Additional CPU Feature Adoption: WAITPKG, SERIALIZE, RDPID                                        | none       |     6 |
| [ ]  | `D02 T09 §20` | Post-Ship Follow-Up Backfill (orphan cohort 2026-07-31)                                           | none       |     1 |
| [x]  | `D02 T10 §1`  | NX Bit: EFER.NXE + PTE NX on All Non-Code Mappings                                                | none       |    10 |
| [x]  | `D02 T10 §2`  | SMEP & SMAP: CR4 Activation + CLAC/STAC Wrappers                                                  | none       |    13 |
| [x]  | `D02 T10 §3`  | KPTI Trampoline Page + Per-CPU CR3 Fields                                                         | none       |     5 |
| [x]  | `D02 T10 §4`  | KPTI SYSCALL CR3 Swap                                                                             | none       |     6 |
| [x]  | `D02 T10 §5`  | KPTI IDT CR3 Swap (All 256 Vectors)                                                               | none       |     7 |
| [x]  | `D02 T10 §6`  | KPTI User CR3 Allocation + Context Switch                                                         | none       |     6 |
| [x]  | `D02 T10 §7`  | PCID: TLB Tagging for No-Flush CR3 Switch                                                         | none       |     6 |
| [x]  | `D02 T10 §8`  | Spectre Mitigations: IBRS/IBPB + Retpoline                                                        | none       |    12 |
| [x]  | `D02 T10 §10` | CET Indirect Branch Tracking (IBT / ENDBR64)                                                      | none       |     8 |
| [x]  | `D02 T10 §11` | Kernel Heap Hardening: Cookies & Redzones                                                         | none       |    10 |
| [x]  | `D02 T10 §12` | Stack Canaries (`-fstack-protector-strong`)                                                       | none       |     8 |
| [ ]  | `D02 T10 §13` | Kernel Stack Guard Pages                                                                          | none       |    10 |
| [x]  | `D02 T10 §14` | KASLR: RDRAND Kernel Load Address                                                                 | none       |     9 |
| [x]  | `D02 T10 §15` | Enclave and Code Signing Syscalls Wired to SSDT                                                   | none       |    10 |
| [ ]  | `D02 T10 §16` | Secure Boot Lockdown Enforcement                                                                  | none       |     8 |
| [ ]  | `D02 T10 §17` | Kernel-Image W^X: .text Read-Only + .rodata RO-After-Init                                         | none       |     9 |
| [x]  | `D02 T10 §18` | CPU Feature-Flag 128-bit Expansion (`cpu_feature_mask_t`)                                         | none       |     7 |
| [x]  | `D02 T10 §19` | Microarchitectural Data-Sampling Clears (MDS / TAA / SRBDS / MMIO)                                | none       |     6 |
| [ ]  | `D02 T10 §20` | kCFI: Software Control-Flow Integrity for Indirect Calls                                          | none       |     6 |
| [ ]  | `D02 T10 §21` | FORTIFY_SOURCE: Bounds-Checked str/mem Builtins                                                   | none       |     5 |
| [ ]  | `D02 T10 §22` | stackleak: Erase Kernel Stack on Return to User                                                   | none       |     5 |
| [ ]  | `D02 T10 §23` | KFENCE: Sampling Use-After-Free / Out-of-Bounds Detector                                          | none       |     6 |
| [x]  | `D02 T10 §24` | Mitigation Visibility: Queryable Security Posture                                                 | none       |     5 |
| [x]  | `D02 T10 §25` | Spectre Predictor-Policy Mitigations: SSBD, STIBP, RSB, BHI, ITS, Retbleed                        | none       |    10 |
| [x]  | `D02 T10 §26` | Release-Build Test-Surface Exclusion                                                              | none       |    11 |
| [x]  | `D02 T10 §27` | Release-Flavor Proof: Seam-Inventory Gate and CI Attestation                                      | none       |     6 |
| [x]  | `D02 T10 §28` | Test-Only Translation Units Outside `src/kernel/test/`                                            | none       |     8 |
| [/]  | `D02 T10 §29` | Guard Unguarded Test-Only Helper Functions in Production TUs                                      | none       |     6 |
| [/]  | `D02 T10 §30` | Signed CI Attestation for the Release-Flavor Proof Gate                                           | none       |     5 |
| [ ]  | `D02 T10 §31` | Legacy-Syscall User-Pointer Validation                                                            | none       |     5 |
| [ ]  | `D02 T10 §32` | Post-Ship Follow-Up Backfill (orphan cohort 2026-07-31)                                           | none       |     3 |
| [x]  | `D02 T11 §1`  | TEB Struct and GS Self-Pointer                                                                    | none       |     5 |
| [x]  | `D02 T11 §2`  | PEB Struct and RTL_USER_PROCESS_PARAMETERS                                                        | none       |     7 |
| [x]  | `D02 T11 §3`  | swapgs on INT 0x80 Entry and Exit                                                                 | none       |     6 |
| [x]  | `D02 T11 §4`  | KERNEL_GS_BASE Written at task_exec and Fork                                                      | none       |     7 |
| [x]  | `D02 T11 §5`  | PEB Allocation and Population at task_exec                                                        | none       |     9 |
| [x]  | `D02 T11 §6`  | TEB Allocation and Population at Thread Create                                                    | none       |     7 |
| [x]  | `D02 T11 §7`  | Initial User Stack Frame                                                                          | none       |     6 |
| [x]  | `D02 T11 §8`  | PEB Ldr (Module List) Basic Population                                                            | none       |     7 |
| [x]  | `D02 T11 §9`  | TLS Slot Allocation (64 Static Slots)                                                             | none       |     6 |
| [x]  | `D02 T11 §10` | PEB / TEB Exposed in Ob Namespace                                                                 | none       |     6 |
| [ ]  | `D02 T11 §11` | KUSER_SHARED_DATA -- Kernel-User Shared Page                                                      | none       |    11 |
| [x]  | `D02 T11 §12` | TLS Expansion Slots (1024 Dynamic Slots)                                                          | none       |    10 |
| [x]  | `D02 T11 §13` | Extended Auxiliary Vector (AT_RANDOM, AT_PHDR, AT_PHNUM)                                          | none       |    12 |
| [x]  | `D02 T11 §14` | User-Mode Thread Bootstrap (uthread_create)                                                       | none       |    11 |
| [x]  | `D02 T11 §15` | Per-thread TEB Allocation at uthread_create()                                                     | none       |    11 |
| [/]  | `D02 T11 §17` | PEB x64 Version-Field Offsets                                                                     | none       |     6 |
| [ ]  | `D02 T11 §18` | Paranoid swapgs Entry for NMI / #DF / #MCE                                                        | none       |     6 |
| [/]  | `D02 T11 §19` | exec/fork/switch GS-base Staging Correctness and Cost                                             | none       |     6 |
| [/]  | `D02 T11 §16` | TEB Multi-Page Mapping and User-VA Non-Overlap                                                    | none       |     9 |
| [ ]  | `D02 T11 §20` | PEB Allocation Robustness and Win32 ABI Handoff                                                   | none       |     7 |
| [/]  | `D02 T11 §21` | Format-Specific User Startup Frame (ELF / PE / EIF) + Env Unification                             | none       |     3 |
| [ ]  | `D02 T11 §22` | PEB Ldr Module Identity (DllBase / SizeOfImage / Name)                                            | none       |     3 |
| [/]  | `D02 T11 §23` | Per-thread TLS Value Storage (Static + Expansion)                                                 | none       |     3 |
| [ ]  | `D02 T11 §24` | PEB/TEB Ob-Namespace Exposure: Safe Wrapper Objects + Lifecycle                                   | none       |     6 |
| [ ]  | `D02 T11 §25` | TLS Expansion Mapping Safety and Alloc Race                                                       | none       |     3 |
| [ ]  | `D02 T11 §26` | auxv Hardening: Classified AT_RANDOM + Validated AT_PHDR                                          | none       |     3 |
| [ ]  | `D02 T11 §27` | uthread_create Robustness: Guard-page Cleanup + tid Reservation + Stack-size Validation           | none       |     5 |
| [ ]  | `D02 T11 §28` | Intermittent TEB-without-kernel_gs_base Halt at Ring-3 Entry                                      | none       |     4 |
| [x]  | `D02 T12 §1`  | NTSTATUS Type and Canonical Status Codes                                                          | none       |     6 |
| [x]  | `D02 T12 §2`  | SYSCALL/SYSRET Fast Path                                                                          | none       |     8 |
| [ ]  | `D02 T12 §3`  | INT 0x2E Compatibility Path                                                                       | none       |     7 |
| [x]  | `D02 T12 §4`  | System Service Descriptor Table (SSDT)                                                            | none       |    11 |
| [x]  | `D02 T12 §5`  | Nt/Zw Naming and Existing Syscall Migration                                                       | none       |    19 |
| [ ]  | `D02 T12 §6`  | NtCreateFile / NtOpenFile / NtClose / NtReadFile / NtWriteFile                                    | none       |    15 |
| [ ]  | `D02 T12 §7`  | NtCreateProcess / NtCreateThread / Process-Thread Lifecycle                                       | none       |    39 |
| [x]  | `D02 T12 §8`  | Synchronisation Objects + NtWaitForMultipleObjects                                                | none       |    31 |
| [x]  | `D02 T12 §9`  | Virtual Memory (Alloc, Free, Protect, Lock, Cross-Process)                                        | none       |    17 |
| [ ]  | `D02 T12 §10` | NtQuerySystemInformation / NtQueryInformationProcess                                              | none       |    15 |
| [x]  | `D02 T12 §11` | Extended Error Information (IOSB + LastError)                                                     | none       |     6 |
| [x]  | `D02 T12 §12` | ZwXxx Kernel-Mode Alias Layer                                                                     | none       |     8 |
| [x]  | `D02 T12 §13` | File Metadata and Device Control                                                                  | none       |    17 |
| [ ]  | `D02 T12 §16` | Token Open, Query, and Adjust Syscalls                                                            | none       |    11 |
| [ ]  | `D02 T12 §17` | Directory and Symbolic Link Object Syscalls                                                       | none       |     9 |
| [ ]  | `D02 T12 §18` | Section and Memory-Mapped File Syscalls                                                           | none       |    11 |
| [ ]  | `D02 T12 §19` | Timer Control Syscalls                                                                            | none       |    19 |
| [x]  | `D02 T12 §20` | Legacy LPC Port Syscalls                                                                          | none       |    16 |
| [x]  | `D02 T12 §21` | Exception and Debug Syscalls                                                                      | none       |     9 |
| [x]  | `D02 T12 §22` | Power and System Control                                                                          | none       |     7 |
| [x]  | `D02 T12 §23` | Atom, Locale, and Miscellaneous Syscalls                                                          | none       |    14 |
| [ ]  | `D02 T12 §24` | Syscall Audit and Tracing Hook                                                                    | none       |     9 |
| [x]  | `D02 T12 §25` | Per-Process Syscall Filtering                                                                     | none       |     8 |
| [x]  | `D02 T12 §26` | Kernel-to-User Mode Callback Dispatch                                                             | none       |     7 |
| [x]  | `D02 T12 §27` | SSDT Integrity Protection                                                                         | none       |     6 |
| [x]  | `D02 T12 §28` | Extended Directory Enumeration Classes                                                            | none       |     9 |
| [ ]  | `D02 T12 §29` | Token Lifecycle and SRM Access Check Syscalls                                                     | none       |    21 |
| [x]  | `D02 T12 §30` | Generic Object Management Syscalls                                                                | none       |    10 |
| [x]  | `D02 T12 §31` | Modern ALPC Port Syscalls                                                                         | none       |    17 |
| [ ]  | `D02 T12 §32` | Post-Ship Follow-Up Backfill (orphan cohort 2026-07-31)                                           | none       |     5 |
| [ ]  | `D02 T12 §33` | Legacy `SYS_*` Number Allocation Across Roadmaps                                                  | none       |     4 |
| [x]  | `D02 T13 §1`  | Unicode String Primitive Layer                                                                    | none       |     5 |
| [x]  | `D02 T13 §2`  | Case Folding and Invariant Compare                                                                | none       |     8 |
| [x]  | `D02 T13 §3`  | Global and Local Atom Tables                                                                      | none       |     4 |
| [x]  | `D02 T13 §4`  | NLS Table File Format and Loader                                                                  | none       |     6 |
| [x]  | `D02 T13 §5`  | Code Page Conversion Providers                                                                    | none       |     9 |
| [ ]  | `D02 T13 §6`  | Locale and LCID Metadata                                                                          | none       |    10 |
| [x]  | `D02 T13 §7`  | Sort Keys and Normalization Policy                                                                | none       |     8 |
| [x]  | `D02 T14 §1`  | Access Rights, API Limits & RegFlushKey                                                           | none       |    19 |
| [x]  | `D02 T14 §2`  | Advanced Key Operations                                                                           | none       |    12 |
| [x]  | `D02 T14 §3`  | Change Notifications                                                                              | none       |    15 |
| [x]  | `D02 T14 §7`  | `regedit` Shell Tool                                                                              | none       |    11 |
| [x]  | `D02 T14 §8`  | Advanced Hive Features                                                                            | none       |    22 |
| [x]  | `D02 T14 §9`  | Transactions, Search API & Snapshot/Diff                                                          | none       |    15 |
| [x]  | `D02 T14 §10` | Performance Optimisations                                                                         | none       |    13 |
| [x]  | `D02 T14 §11` | KTM Transaction Syscalls Wired to SSDT                                                            | none       |     9 |
| [x]  | `D02 T14 §12` | Registry Symlink Completion                                                                       | none       |     4 |
| [x]  | `D02 T14 §13` | Schema-Validated Registry Keys ⭐                                                                 | none       |     6 |
| [x]  | `D02 T14 §15` | Registry Value Size Expansion (16 KiB Names, 1 MiB Data)                                          | none       |     7 |
| [ ]  | `D02 T14 §16` | Post-Ship Follow-Up Backfill (orphan cohort 2026-07-31)                                           | none       |     4 |
| [x]  | `D02 T15 §1`  | SID & LUID Primitives                                                                             | none       |     7 |
| [x]  | `D02 T15 §2`  | Privilege Constants & PRIVILEGE_SET                                                               | none       |     4 |
| [x]  | `D02 T15 §3`  | SECURITY_DESCRIPTOR, ACL & ACE Types                                                              | none       |    29 |
| [ ]  | `D02 T15 §4`  | ACCESS_TOKEN Object                                                                               | none       |    17 |
| [ ]  | `D02 T05 §9`  | NtClose / NtDuplicateObject / NtQueryObject                                                       | none       |     8 |
| [ ]  | `D02 T15 §5`  | SeAccessCheck Engine                                                                              | none       |    22 |
| [x]  | `D02 T15 §6`  | Mandatory Integrity Control (MIC)                                                                 | none       |    11 |
| [x]  | `D02 T15 §7`  | Process/Thread Token Assignment & Impersonation                                                   | none       |     9 |
| [x]  | `D02 T15 §8`  | SePrivilegeCheck & Per-Privilege Enforcement                                                      | none       |    12 |
| [x]  | `D02 T15 §9`  | UAC Token Split & NtFilterToken                                                                   | none       |     8 |
| [x]  | `D02 T15 §10` | Win32 Security API Wrappers                                                                       | none       |    22 |
| [x]  | `D02 T15 §11` | Live Token Inspector                                                                              | none       |     4 |
| [x]  | `D02 T15 §12` | Security and Token Syscalls Wired to SSDT                                                         | none       |    21 |
| [x]  | `D02 T15 §13` | Security Descriptor Inheritance (SeAssignSecurity)                                                | none       |     9 |
| [x]  | `D02 T15 §14` | AppContainer / LowBox Tokens                                                                      | none       |    12 |
| [x]  | `D02 T15 §15` | Access Denial Explainer                                                                           | none       |     7 |
| [ ]  | `D02 T15 §16` | Security Primitive Hardening                                                                      | none       |     4 |
| [x]  | `D02 T16 §1`  | Notification State Object Type                                                                    | none       |     9 |
| [x]  | `D02 T16 §2`  | Kernel Publish/Subscribe API                                                                      | none       |     9 |
| [x]  | `D02 T16 §3`  | Waitable User Subscriptions                                                                       | none       |    11 |
| [x]  | `D02 T16 §4`  | Security and Namespace Policy                                                                     | none       |     7 |
| [x]  | `D02 T16 §5`  | Built-In State-Name Catalog                                                                       | none       |    10 |
| [x]  | `D02 T16 §6`  | ETW/klog Bridge                                                                                   | none       |     4 |
| [ ]  | `D02 T16 §7`  | Coalescing and Payload Retention                                                                  | none       |     8 |
| [x]  | `D02 T16 §9`  | Diagnostics Browser and Counters                                                                  | none       |     4 |
| [x]  | `D02 T17 §1`  | `exec_load()` Multi-Format Dispatcher                                                             | none       |    11 |
| [x]  | `D02 T17 §2`  | Enhanced ELF Loader                                                                               | none       |    13 |
| [x]  | `D02 T17 §3`  | ELF Security Segments (GNU_STACK, RELRO, GNU_PROPERTY)                                            | none       |    10 |
| [x]  | `D02 T17 §4`  | EIF Format Specification                                                                          | none       |     9 |
| [x]  | `D02 T17 §5`  | EIF Kernel Loader                                                                                 | none       |     8 |
| [x]  | `D02 T17 §6`  | Module List Registration (LDR_DATA_TABLE_ENTRY)                                                   | none       |     8 |
| [ ]  | `D02 T17 §7`  | PE32+ Header Parser                                                                               | none       |     8 |
| [ ]  | `D02 T17 §8`  | PE32+ Section Loader + `.pdata` Registration                                                      | none       |    12 |
| [ ]  | `D02 T17 §9`  | PE32+ Import Table Resolver (Win32 Dispatch)                                                      | none       |     9 |
| [x]  | `D02 T17 §10` | PE32+ Base Relocation                                                                             | none       |     4 |
| [x]  | `D02 T17 §11` | PE32+ TLS Directory Processing                                                                    | none       |     6 |
| [x]  | `D02 T17 §12` | PE32+ Load Config and CFG Bitmap                                                                  | none       |     6 |
| [ ]  | `D02 T17 §13` | `elf2eif` Host-Side Converter                                                                     | none       |     7 |
| [x]  | `D02 T17 §14` | ELF Dynamic Linker                                                                                | none       |     8 |
| [x]  | `D02 T17 §15` | ASLR -- Address Space Layout Randomization                                                        | none       |     7 |
| [x]  | `D02 T17 §16` | Script/Shebang Interpreter Support                                                                | none       |     5 |
| [x]  | `D02 T17 §17` | EIF Code Signing                                                                                  | none       |     6 |
| [x]  | `D02 T17 §18` | ELF `PT_TLS` Template Loading                                                                     | none       |     9 |
| [x]  | `D02 T17 §19` | PE API-Set + Delay-Load/Bound Import Support                                                      | none       |     7 |
| [x]  | `D02 T17 §20` | ELF Dynamic Linker -- Advanced (IFUNC, Init/Fini, Versioning, Search Order)                       | none       |     8 |
| [x]  | `D02 T18 §1`  | `KIMAGE_ENTRY` Data Model                                                                         | none       |     7 |
| [/]  | `D02 T18 §3`  | Address Range Index                                                                               | none       |     6 |
| [/]  | `D02 T18 §4`  | Symbol Provider Abstraction                                                                       | none       |     6 |
| [x]  | `D02 T19 §1`  | Code Integrity Policy Object                                                                      | none       |     9 |
| [x]  | `D02 T19 §2`  | Image Validation API and Callback                                                                 | none       |     8 |
| [x]  | `D02 T19 §3`  | Hashing and Signature Provider Bridge                                                             | none       |     5 |
| [/]  | `D02 T19 §4`  | Embedded Signature Validation                                                                     | none       |     9 |
| [/]  | `D02 T19 §5`  | Catalog Database                                                                                  | none       |     6 |
| [x]  | `D02 T19 §6`  | Revocation and Deny Lists                                                                         | none       |     6 |
| [x]  | `D02 T19 §7`  | Measured-Boot and Secure Boot Binding                                                             | none       |     6 |
| [x]  | `D02 T19 §8`  | Driver/Module Enforcement                                                                         | none       |     5 |
| [x]  | `D02 T19 §9`  | User-Mode Image Enforcement                                                                       | none       |     5 |
| [x]  | `D02 T19 §10` | CI Audit, Telemetry, and Syscalls                                                                 | none       |     5 |
| [ ]  | `D02 T19 §11` | Post-Ship Follow-Up Backfill (orphan cohort 2026-07-31)                                           | none       |     5 |
| [/]  | `D02 T18 §9`  | Provenance, CI, and Hotpatch Metadata                                                             | none       |     7 |
| [x]  | `D02 T20 §1`  | Range Overlap Validation (Normative Rule 2)                                                       | none       |     5 |
| [ ]  | `D02 T20 §2`  | Segment Permission Enforcement (R/W/X via PTE)                                                    | none       |     8 |
| [ ]  | `D02 T20 §3`  | Module Registration for EIF                                                                       | none       |     5 |
| [x]  | `D02 T20 §4`  | API Version Gating                                                                                | none       |     4 |
| [x]  | `D02 T20 §5`  | Metadata Section Parser                                                                           | none       |     8 |
| [x]  | `D02 T20 §6`  | LZ4 Compressed Segments                                                                           | none       |     5 |
| [/]  | `D02 T20 §7`  | Optional Import Stubs                                                                             | none       |     7 |
| [x]  | `D02 T20 §8`  | EIF ASLR (load_base=0 Randomization)                                                              | none       |    10 |
| [ ]  | `D02 T20 §9`  | Per-Process Dispatch Table Isolation                                                              | none       |     8 |
| [x]  | `D02 T20 §10` | CET Compatibility Flag (Format Reservation)                                                       | none       |     5 |
| [/]  | `D02 T20 §11` | EIF Signature-Block ABI (Signer Key Delivery)                                                     | none       |     4 |
| [ ]  | `D02 T20 §12` | Resource Section with SVG Icons                                                                   | none       |     7 |
| [x]  | `D02 T21 §1`  | Working Directory                                                                                 | none       |    13 |
| [ ]  | `D02 T21 §2`  | Standard Handle Pre-Wiring at Process Creation                                                    | none       |     8 |
| [x]  | `D02 T21 §3`  | User-Mode Program Break (brk/sbrk -- Linux Compat)                                                | none       |    12 |
| [x]  | `D02 T21 §4`  | Process Priority Class                                                                            | none       |     7 |
| [x]  | `D02 T21 §5`  | Per-Task Scheduling Policy                                                                        | none       |     6 |
| [/]  | `D02 T21 §6`  | Process Capabilities and Privilege Bitmask                                                        | none       |     6 |
| [/]  | `D02 T21 §7`  | Capability Inheritance and Drop-Only Policy                                                       | none       |     6 |
| [x]  | `D02 T21 §8`  | Process Accounting Fields (Times, I/O Counters)                                                   | none       |     8 |
| [x]  | `D02 T21 §9`  | Per-Process Resource Limits (rlimits)                                                             | none       |    11 |
| [x]  | `D02 T21 §10` | CPU Affinity per Process                                                                          | none       |     6 |
| [x]  | `D02 T21 §11` | Per-Process Mitigation Policy                                                                     | none       |     9 |
| [x]  | `D02 T21 §12` | Pledge/Unveil-Style Process Restriction                                                           | none       |    10 |
| [x]  | `D02 T21 §13` | Job Object Syscalls Wired to SSDT                                                                 | none       |    14 |
| [x]  | `D02 T21 §14` | Process Exit Cleanup                                                                              | none       |     7 |
| [/]  | `D02 T21 §15` | Process Parenting, Reaping, and Wait Semantics                                                    | none       |     9 |
| [x]  | `D02 T21 §16` | Protected Process Light (PS_PROTECTION)                                                           | none       |     6 |
| [x]  | `D02 T21 §17` | Process Groups and Sessions                                                                       | none       |     8 |
| [x]  | `D02 T21 §18` | Rich Wait Variants, NT Multi-Waiter Wake, and Dumpable Policy                                     | none       |     7 |
| [x]  | `D02 T21 §19` | `task_exec` Commit Point and Kernel-Stack Reclamation                                             | none       |    19 |
| [ ]  | `D02 T21 §20` | Page-Table Lifetime Across Reap + fork/exec                                                       | none       |    13 |
| [ ]  | `D02 T21 §21` | Post-Ship Follow-Up Backfill (orphan cohort 2026-07-31)                                           | none       |    15 |
| [x]  | `D02 T22 §1`  | Per-Process Environ Storage & Kernel API                                                          | none       |    11 |
| [x]  | `D02 T22 §2`  | System Default Variables from Registry                                                            | none       |     5 |
| [x]  | `D02 T22 §3`  | `%VAR%` Expansion                                                                                 | none       |     4 |
| [ ]  | `D02 T22 §4`  | argv Array: Kernel Storage & Shell Parsing                                                        | none       |    11 |
| [x]  | `D02 T22 §5`  | Nt/Zw Environment Variable Syscalls                                                               | none       |     4 |
| [x]  | `D02 T22 §6`  | Win32 API Wrappers                                                                                | none       |    15 |
| [x]  | `D02 T22 §7`  | Shell Integration: PATH Lookup & SET/ECHO                                                         | none       |    11 |
| [x]  | `D02 T22 §8`  | `.profile` Startup Script                                                                         | none       |     8 |
| [x]  | `D02 T22 §9`  | Environment Change Notifications                                                                  | none       |    10 |
| [x]  | `D02 T22 §10` | Environment Block Sorting & Size Limits                                                           | none       |    13 |
| [x]  | `D02 T22 §11` | PATHEXT Variable & Extension Search Order                                                         | none       |     5 |
| [x]  | `D02 T22 §12` | Hidden Drive-Letter Variables (`=C:`, `=D:`)                                                      | none       |     9 |
| [x]  | `D02 T22 §13` | CreateEnvironmentBlock / DestroyEnvironmentBlock                                                  | none       |     8 |
| [ ]  | `D02 T22 §14` | SearchPathW / SearchPathA Win32 API                                                               | none       |     8 |
| [x]  | `D02 T22 §15` | CommandLineToArgvW Win32 API                                                                      | none       |     5 |
| [x]  | `D02 T22 §16` | Environment Variable Security & Sanitization                                                      | none       |     7 |
| [x]  | `D02 T22 §17` | App Paths Registry-Based Executable Lookup                                                        | none       |     6 |
| [x]  | `D02 T22 §18` | cmd.exe Dynamic Pseudo-Variables & Delayed Expansion                                              | none       |     6 |
| [x]  | `D02 T22 §19` | Rtl Expansion Hardening: Output-Count Wrap & `%=X:%` Parity                                       | none       |     6 |
| [x]  | `D02 T22 §20` | Rtl Export Prerequisites: Lookup Bound, Foreign-Block Extent, At-Cap Allocation                   | none       |     6 |
| [x]  | `D02 T22 §21` | ntdll Rtl Environment Exports                                                                     | none       |    12 |
| [x]  | `D02 T22 §22` | env Allocator Safety and UTF-8 Expansion Budget                                                   | none       |     8 |
| [x]  | `D02 T22 §23` | Live-Environment Adoption: SetCurrentEnvironment, SetEnvironmentStrings, CreateEnvironmentEx      | none       |    12 |
| [ ]  | `D02 T22 §24` | Rtl Expansion Completeness: Boundary Copy, Measured Budget, Lookup Cache                          | none       |    15 |
| [x]  | `D02 T22 §25` | Counted (non-`_U`) Rtl Env Read Forms                                                             | none       |     9 |
| [x]  | `D02 T23 §1`  | EXCEPTION_RECORD, CONTEXT, and EXCEPTION_POINTERS Types                                           | none       |     8 |
| [x]  | `D02 T23 §2`  | #PF Triage -- User vs. Kernel Decode, EXCEPTION_RECORD/CONTEXT Build, Dispatch Routing            | none       |     6 |
| [x]  | `D02 T23 §4`  | Debugger First-Chance / Second-Chance Notification                                                | none       |     9 |
| [x]  | `D02 T23 §5`  | KiUserExceptionDispatcher -- Ring-3 Exception Delivery                                            | none       |     9 |
| [ ]  | `D02 T23 §6`  | x64 Table-Based Unwind (`.pdata`, `RtlVirtualUnwind`)                                             | none       |    14 |
| [x]  | `D02 T23 §7`  | Stack Walking (`RtlCaptureStackBackTrace`, `RtlWalkFrameChain`)                                   | none       |     8 |
| [x]  | `D02 T23 §8`  | SEH Chain Walk and `__try`/`__except` Frame Dispatch                                              | none       |     5 |
| [ ]  | `D02 T23 §9`  | RtlUnwindEx -- Unwind to a Target Frame                                                           | none       |     9 |
| [x]  | `D02 T23 §10` | Vectored Exception Handlers (VEH)                                                                 | none       |     5 |
| [x]  | `D02 T23 §11` | Vectored Continue Handlers (VCH)                                                                  | none       |     3 |
| [x]  | `D02 T23 §12` | Unhandled Exception Filter and WER Hook                                                           | none       |     6 |
| [x]  | `D02 T23 §13` | Kernel Safe Probing (`ProbeForRead`, `ProbeForWrite`)                                             | none       |     5 |
| [ ]  | `D02 T23 §14` | Kernel-Mode `__try`/`__except` for Drivers                                                        | none       |     8 |
| [x]  | `D02 T23 §15` | POSIX Signal Delivery from Exceptions (Linux Compat)                                              | none       |     8 |
| [x]  | `D02 T23 §16` | Exception Dispatch Telemetry                                                                      | none       |     7 |
| [x]  | `D02 T23 §17` | Guard-Page Stack Auto-Grow -- Commit/Reserve Tracking, Guard Re-Arm, Terminal Overflow            | none       |     5 |
| [ ]  | `D02 T23 §18` | Unwind Fixtures Independent of Where the Linker Put Real Code                                     | none       |     3 |
| [/]  | `D02 T23 §19` | Nothing Compiles the EXCEPT_TELEMETRY=off Flavor                                                  | none       |     3 |
| [ ]  | `D02 T23 §20` | Kernel SEH Cannot Be Armed Without Rewriting Scheduler Thread Metadata                            | none       |     3 |
| [x]  | `D02 T24 §1`  | Message Header, Port Attributes & Type Codes                                                      | none       |    12 |
| [x]  | `D02 T24 §2`  | ALPC_PORT Object & Object Manager Registration                                                    | none       |     9 |
| [x]  | `D02 T24 §3`  | Connection State Machine                                                                          | none       |    15 |
| [x]  | `D02 T24 §4`  | Synchronous Send+Wait+Receive Engine                                                              | none       |     8 |
| [x]  | `D02 T24 §5`  | Asynchronous Delivery & Completion List                                                           | none       |    10 |
| [ ]  | `D02 T24 §6`  | Large Data: Port Sections & View Mapping                                                          | none       |    13 |
| [ ]  | `D02 T24 §7`  | Security: Client Token Capture & Impersonation                                                    | none       |    11 |
| [/]  | `D02 T24 §8`  | NtAlpc* SSDT Registration & Stub Retrofit                                                         | none       |    10 |
| [ ]  | `D02 T24 §9`  | NtAlpc QueryInformation, SetInformation & CancelMessage                                           | none       |     9 |
| [x]  | `D02 T24 §10` | CSRSS ApiPort Bootstrap                                                                           | none       |     9 |
| [x]  | `D02 T24 §11` | Message Zones & Resource Reserves (Pre-Allocated Buffers)                                         | none       |     9 |
| [x]  | `D02 T24 §12` | Live Port Monitor & IPC Profiler                                                                  | none       |     7 |
| [ ]  | `D02 T24 §13` | Post-Ship Follow-Up Backfill (orphan cohort 2026-07-31)                                           | none       |     1 |
| [x]  | `D02 T25 §1`  | Resource Type Registry                                                                            | none       |     5 |
| [x]  | `D02 T25 §2`  | Quota Block and Charge API                                                                        | none       |     6 |
| [x]  | `D02 T25 §3`  | Process/Token/Job Ownership Model                                                                 | none       |     7 |
| [x]  | `D02 T25 §4`  | Receipt Identity (Generation-Tokened Charges)                                                     | none       |    10 |
| [x]  | `D02 T25 §5`  | Pool and Allocation Quota Integration                                                             | none       |     8 |
| [x]  | `D02 T25 §6`  | Registry, ALPC, Notification Quotas                                                               | none       |     7 |
| [x]  | `D02 T25 §7`  | CPU, I/O, and Wakeup Accounting                                                                   | none       |    10 |
| [x]  | `D02 T25 §8`  | Native Query/Set Quota Syscalls                                                                   | none       |    12 |
| [x]  | `D02 T25 §9`  | Resource Pressure Events and Recovery Hooks                                                       | none       |     8 |
| [ ]  | `D02 T25 §10` | Tests, Leak Sweeps, and Dashboards                                                                | none       |    10 |
| [x]  | `D02 T25 §11` | Charge Ledger and Transactional Adjustment                                                        | none       |     6 |
| [x]  | `D02 T25 §12` | Resource Pressure Stall Telemetry                                                                 | none       |     9 |
| [x]  | `D02 T25 §13` | Object Manager Charge Points                                                                      | none       |     5 |
| [x]  | `D02 T25 §14` | Ledger Lifetime: ISR-Safe Return and Obligation Adoption                                          | none       |     5 |
| [x]  | `D02 T25 §15` | Charge-Path Cost Reduction                                                                        | none       |     7 |
| [x]  | `D02 T25 §16` | Charge Attribution and Status Fidelity                                                            | none       |     5 |
| [x]  | `D02 T25 §17` | Infrastructure-Gated Charge-Path Bounding and Stall Lanes                                         | none       |    16 |
| [x]  | `D02 T25 §18` | Ledger Obligation Ceiling and Charge-Source Pass-Through                                          | none       |     3 |
| [x]  | `D02 T25 §19` | Lockless Job Membership and the Live Pressure-Retry Seam                                          | none       |     5 |
| [ ]  | `D02 T25 §20` | Per-Process Network I/O Accounting                                                                | none       |     4 |
| [x]  | `D02 T26 §1`  | ACPI Sleep Object Parsing & PM1 State Machine                                                     | none       |    14 |
| [x]  | `D02 T26 §2`  | C1 Idle Entry: Race-Safe HLT + Per-CPU Idle Accounting                                            | none       |     7 |
| [x]  | `D02 T26 §3`  | S3: Suspend to RAM                                                                                | none       |    11 |
| [x]  | `D02 T26 §4`  | S4: Hibernation Image Format and Compression Codec                                                | none       |     8 |
| [x]  | `D02 T26 §5`  | ACPI Embedded Controller (EC) Driver                                                              | none       |    19 |
| [x]  | `D02 T26 §6`  | Battery & AC Adapter (ACPI Source Layer)                                                          | none       |     8 |
| [x]  | `D02 T26 §7`  | Power & Sleep Button Event Dispatch                                                               | none       |    24 |
| [x]  | `D02 T26 §8`  | PCI Power Management Capability and D0--D3hot State Machine                                       | none       |    19 |
| [x]  | `D02 T26 §9`  | Driver Power Callbacks & Resume Ordering                                                          | none       |     6 |
| [x]  | `D02 T26 §10` | S0ix Firmware Advertisement and MWAIT Capability Layer                                            | none       |     7 |
| [x]  | `D02 T26 §11` | Fast Startup (Hybrid Shutdown / Hiberboot)                                                        | none       |    11 |
| [ ]  | `D02 T26 §12` | Runtime Device Idle Management                                                                    | none       |    30 |
| [ ]  | `D02 T26 §13` | Power Request Tracking & Wake Source Management                                                   | none       |    16 |
| [ ]  | `D02 T26 §17` | Driver Power Query & Veto (IRP_MN_QUERY_POWER)                                                    | none       |     5 |
| [ ]  | `D02 T26 §20` | Power Syscalls Wired to SSDT                                                                      | none       |     9 |
| [ ]  | `D02 T26 §21` | Linux `/sys/power` Suspend Variant Parity                                                         | none       |     8 |
| [ ]  | `D02 T26 §23` | PCIe ASPM and L1 Substates                                                                        | none       |    10 |
| [ ]  | `D02 T26 §24` | ACPI General-Purpose Event (GPE) Blocks                                                           | none       |    14 |
| [ ]  | `D02 T26 §25` | Per-CPU Idle-Time Accounting via `NtQuerySystemInformation`                                       | none       |     8 |
| [x]  | `D02 T26 §26` | Stop-the-World CPU Rendezvous for Sleep Transitions                                               | none       |     6 |
| [ ]  | `D02 T26 §27` | Rendezvous Safety Residue: Test Seam and the Retract-Side Window                                  | none       |     9 |
| [x]  | `D02 T26 §28` | S4 Orchestration: Hibernation Image Write and Resume Path                                         | none       |    10 |
| [ ]  | `D02 T26 §29` | Composite Battery State Model, Power Policy, and Registry Publication                             | none       |    17 |
| [x]  | `D02 T26 §30` | Battery Charge Limiting and Smart Charging                                                        | none       |     6 |
| [x]  | `D02 T26 §31` | Lid State and Lid-Close Policy                                                                    | none       |     6 |
| [x]  | `D02 T26 §32` | Human Presence Detection (HPD)                                                                    | none       |     8 |
| [ ]  | `D02 T26 §33` | PM Device Registry (`pm_device_t`, `pm_register_device`)                                          | none       |    10 |
| [ ]  | `D02 T26 §35` | Directed Power (DFx) and DRIPS Residency Accounting                                               | none       |     9 |
| [ ]  | `D02 T26 §36` | NIC Wake Offloads for Connected Standby (ARP/NS/WoL/Patterns)                                     | none       |     8 |
| [x]  | `D02 T26 §37` | System Connected Standby Entry (All-CPU S0ix Transition)                                          | none       |     7 |
| [x]  | `D02 T26 §38` | ACPI Table Discovery: Validate Extents Before Checksumming and Publishing                         | none       |     6 |
| [ ]  | `D02 T26 §39` | ACPI Root-Pointer Integrity and Validator Unification                                             | none       |     7 |
| [/]  | `D02 T27 §1`  | Bugcheck Codes & `KeBugCheckEx`                                                                   | none       |    15 |
| [/]  | `D02 T27 §2`  | FPU/XMM/XSAVE State Capture                                                                       | none       |     6 |
| [/]  | `D02 T27 §3`  | Module Registry for Crash Dumps                                                                   | none       |     4 |
| [/]  | `D02 T27 §4`  | MDMP Binary Format: Header, Directory & Stream Types                                              | none       |    11 |
| [ ]  | `D02 T27 §5`  | Minidump Writer                                                                                   | none       |     8 |
| [ ]  | `D02 T27 §6`  | Kernel Dump & Full Dump Variants                                                                  | none       |     6 |
| [ ]  | `D02 T27 §7`  | Raw-Partition Dump Sink (VFS Bypass)                                                              | none       |    10 |
| [ ]  | `D02 T27 §8`  | Post-Boot Crash Recovery & CrashDumps Archive                                                     | none       |     7 |
| [ ]  | `D02 T27 §9`  | `dmpanalyze.exe` Crash Analyzer                                                                   | none       |     7 |
| [ ]  | `D02 T27 §10` | Dump Policy, WinDbg Interoperability & Confidentiality                                            | none       |     9 |
| [ ]  | `D02 T28 §1`  | TTF Font Rendering in Panic Screen                                                                | none       |     7 |
| [ ]  | `D02 T28 §2`  | Improved Layout and Visual Hierarchy                                                              | none       |     8 |
| [ ]  | `D02 T28 §3`  | Crash Context: Last 10 klog Entries Inline                                                        | none       |     6 |
| [ ]  | `D02 T28 §4`  | Smart QR Code with Compressed Crash Data                                                          | none       |     9 |
| [ ]  | `D02 T28 §5`  | Auto-Restart Countdown Improvements                                                               | none       |     6 |
| [ ]  | `D02 T28 §6`  | Crash Analysis Hints On-Screen                                                                    | none       |     3 |
| [ ]  | `D02 T28 §7`  | Crash Statistics & Loop Protection                                                                | none       |    10 |
| [ ]  | `D02 T28 §8`  | Safe Mode Suggestion After Repeated Crashes                                                       | none       |     5 |
| [ ]  | `D02 T28 §9`  | "What Failed" Faulting Module Identification                                                      | none       |     4 |
| [ ]  | `D02 T28 §10` | Panic Screen Modes (User / Developer / QR)                                                        | none       |     6 |
| [ ]  | `D02 T28 §11` | Audio Crash Notification (PC Speaker Beep Codes)                                                  | none       |     5 |
| [ ]  | `D02 T28 §12` | Keyboard-Driven Recovery Actions at Crash Screen                                                  | none       |     8 |
| [ ]  | `D02 T28 §13` | Dump Collection Progress Percentage                                                               | none       |     7 |
| [ ]  | `D02 T29 §1`  | Serial Line Setup: 115200 Baud, 8N1, FIFO                                                         | none       |     4 |
| [ ]  | `D02 T29 §2`  | IRQ-Driven RX Ring Buffer and KD COM Port Selection                                               | none       |     9 |
| [ ]  | `D02 T29 §3`  | KD Packet Framing: Send / Receive, Checksum, ACK/RESEND                                           | none       |     7 |
| [ ]  | `D02 T29 §4`  | KD Connection Handshake & Breakin Detection                                                       | none       |     7 |
| [ ]  | `D02 T29 §5`  | `#DB` / `#BP` Exception Routing to KD, AP Freeze/Thaw                                             | none       |     9 |
| [ ]  | `D02 T23 §3`  | General Fault-to-Exception Mapping (#DE, #DB, #BP, #OF, #UD, #NP, #SS, #GP, #CP)                  | none       |     9 |
| [/]  | `D02 T10 §9`  | CET Shadow Stack (Kernel Ring 0)                                                                  | none       |    15 |
| [/]  | `D02 T18 §5`  | Unwind Metadata Registry                                                                          | none       |     7 |
| [ ]  | `D02 T29 §6`  | Context Get / Set                                                                                 | none       |     6 |
| [ ]  | `D02 T29 §7`  | Memory Read / Write                                                                               | none       |     7 |
| [ ]  | `D02 T29 §8`  | Software Breakpoint Management                                                                    | none       |     8 |
| [ ]  | `D02 T29 §9`  | Hardware Breakpoints: DR0--DR3, DR7, `#DB` Reporting                                              | none       |     5 |
| [ ]  | `D02 T29 §10` | Single-Step (RFLAGS.TF) + Continue / Step                                                         | none       |     5 |
| [ ]  | `D02 T29 §11` | DbgKdGetVersionApi + Module List                                                                  | none       |     6 |
| [ ]  | `D02 T29 §12` | I/O Port & MSR Read / Write                                                                       | none       |     9 |
| [ ]  | `D02 T29 §13` | `kd_break()`, Keyboard F12 Breakin & QEMU Guide                                                   | none       |     7 |
| [ ]  | `D02 T29 §14` | Debug Syscalls Wired to SSDT                                                                      | none       |     8 |
| [ ]  | `D02 T29 §15` | KD Transport Roadmap, SMP Policy, and Coexistence                                                 | none       |     7 |
| [/]  | `D02 T18 §8`  | KD/Crash Dump Integration                                                                         | none       |     9 |
| [ ]  | `D02 T30 §1`  | Health State Model and Registry                                                                   | none       |     4 |
| [ ]  | `D02 T30 §2`  | Subsystem Heartbeat API                                                                           | none       |     4 |
| [ ]  | `D02 T30 §3`  | Failure Bucket Taxonomy                                                                           | none       |     4 |
| [ ]  | `D02 T30 §4`  | Live Kernel Dump Trigger                                                                          | none       |     4 |
| [ ]  | `D02 T30 §5`  | Degraded-Mode Transition Engine                                                                   | none       |     4 |
| [ ]  | `D02 T30 §6`  | Recovery Action Dispatcher                                                                        | none       |     6 |
| [ ]  | `D02 T30 §7`  | Repeated-Failure Escalation                                                                       | none       |     5 |
| [/]  | `D02 T31 §1`  | per_cpu_data Assembly Offsets                                                                     | none       |     7 |
| [/]  | `D02 T31 §2`  | boot_config and Early Config Struct Layout                                                        | none       |     8 |
| [/]  | `D02 T31 §3`  | User ELF Range (0x800000-0x900000) 3-File Sync                                                    | none       |     7 |
| [/]  | `D02 T31 §4`  | AP Trampoline Data Area Layout (0x8E00)                                                           | none       |     6 |
| [/]  | `D02 T31 §5`  | Task Interrupt Frame Layout                                                                       | none       |     6 |
| [/]  | `D02 T31 §6`  | IDT Vector Assignment Collision Detection                                                         | none       |     6 |
| [/]  | `D02 T31 §7`  | SSDT Service Number Count Stability                                                               | none       |     5 |
| [/]  | `D02 T31 §8`  | XSAVE/FXSAVE Area Alignment                                                                       | none       |     6 |
| [/]  | `D02 T31 §9`  | ISR swapgs Symmetry Verification                                                                  | none       |     6 |
| [/]  | `D02 T31 §10` | Memory Layout Guard Pages                                                                         | none       |    12 |
| [/]  | `D02 T31 §11` | IXFS Superblock Layout and Magic                                                                  | none       |     7 |
| [/]  | `D02 T31 §12` | Security Structs (SID, TOKEN, ACL/ACE)                                                            | none       |     9 |
| [/]  | `D02 T31 §13` | VFS Drive Letter Range and Partition Offsets                                                      | none       |     8 |
| [/]  | `D02 T31 §14` | exec_pending State Machine Verification                                                           | none       |     6 |
| [/]  | `D02 T31 §15` | Framebuffer Bare-Metal Safety (5 Rules)                                                           | none       |     7 |
| [ ]  | `D02 T32 §1`  | Per-CPU SPSC Ring Buffer + Atomic Primitives                                                      | none       |     9 |
| [ ]  | `D02 T32 §2`  | Lockless `klog_v2()` Enqueue + Format Helpers                                                     | none       |     8 |
| [ ]  | `D02 T32 §3`  | Priority Lanes + Per-Level Sub-Budgets                                                            | none       |     9 |
| [ ]  | `D02 T32 §4`  | Fail-Proof FATAL Emergency Path                                                                   | none       |     8 |
| [ ]  | `D02 T32 §5`  | Configurable Per-Subsystem Rate Limits + Diagnostic Exemption                                     | none       |     8 |
| [ ]  | `D02 T32 §6`  | Background Drain Worker                                                                           | none       |     9 |
| [ ]  | `D02 T32 §7`  | Backpressure-Aware Serial Drain + Pressure Counters                                               | none       |     7 |
| [ ]  | `D02 T32 §8`  | Native Structured Field Wire Format + ETW + JSON Lines Feed                                       | none       |     9 |
| [ ]  | `D02 T32 §9`  | Boot-Survival Ring Snapshot + Recovery                                                            | none       |     7 |
| [ ]  | `D02 T32 §10` | v1 -> v2 Migration Switch + Benchmarks + Retire v1                                                | none       |     8 |
| [x]  | `D02 T33 §1`  | Memory-Map Design + Canonical Layout Decision                                                     | none       |     9 |
| [ ]  | `D02 T33 §2`  | Direct Map Construction (kernel still low)                                                        | none       |    11 |
| [/]  | `D02 T33 §3`  | Linker VMA/LMA Split + Higher-Half Jump                                                           | none       |    14 |
| [/]  | `D02 T33 §4`  | Descriptor Tables + Per-CPU at High Addresses + AP Path                                           | none       |     7 |
| [/]  | `D02 T33 §5`  | Bootloader / `boot_info` / Framebuffer High Handoff                                               | none       |     8 |
| [/]  | `D02 T33 §6`  | Per-Process PML4: Kernel High Shared, User Low Private                                            | none       |     7 |
| [ ]  | `D02 T33 §7`  | Retire the `0x800000` USER_BASE Ceiling + BSS Guard                                               | none       |    35 |
| [ ]  | `D01 T14 §13` | klog Format-Width Contract, Compiler-Checked                                                      | none       |     4 |
| [ ]  | `D01 T14 §15` | Anti-Rollback Terminal Give-Up: Durable Record                                                    | none       |     4 |
| [ ]  | `D01 T14 §17` | Restore Panic Evidence Before Phase 0 Can Overwrite It                                            | none       |     4 |
| [ ]  | `D01 T14 §19` | Record the Panic-Page Pin Outcome in the `boot_info` Handoff                                      | none       |     5 |
| [ ]  | `D01 T29 §19` | Post-Ship Follow-Up Backfill (orphan cohort 2026-07-31)                                           | none       |     3 |
| [/]  | `D02 T33 §8`  | 5-Level Paging (LA57) Support -- Exceeds Win11                                                    | none       |     6 |
| [x]  | `D02 T33 §9`  | VMM Walker Conversion -- Route Every Deref Through the HHDM Helper                                | none       |    14 |
| [x]  | `D02 T33 §10` | Tactical BSS Headroom -- Large Static Pools to Dynamic Allocation                                 | none       |     7 |
| [ ]  | `D02 T33 §11` | Unpark the Ceiling-Stalled Kernel Queue                                                           | none       |    11 |
| [x]  | `D02 T33 §12` | Second Tactical BSS Reclamation Pass                                                              | none       |     7 |
| [x]  | `D02 T33 §13` | Third Tactical Reclamation Pass -- the Trigger Section 12 Named Has Fired                         | none       |     8 |
| [x]  | `D02 T33 §14` | Fourth Tactical Reclamation Pass -- Reclaim the Test-Only Statics                                 | none       |     8 |
| [x]  | `D02 T33 §15` | Fifth Tactical Reclamation Pass -- Convert the `pipes` Pool                                       | none       |     4 |
| [x]  | `D02 T33 §16` | Test-Side Residue of the Fourth Pass -- `s_cap_val`, the 36 KB Stack Locals, and the Scratch Cast | none       |     7 |
| [ ]  | `D02 T34 §1`  | Mark Expected Test-Path Log Output at Emission                                                    | none       |     5 |
| [ ]  | `D02 T34 §2`  | Log-Cleanliness Gate in the Kernel Test Runner                                                    | none       |     4 |
| [/]  | `D02 T34 §3`  | Smoke Test Fails on Any `[FAIL]` in Its Own Log                                                   | none       |     4 |
| [ ]  | `D02 T34 §4`  | Surface the Platform Blind Spots in the Run Summary                                               | none       |     4 |
| [/]  | `D02 T34 §5`  | Rate-Limit the Repeat-Offender Warnings That Produce 86% of the Volume                            | none       |     4 |
| [ ]  | `D02 T35 §1`  | Registry Tree SMP Synchronization and Hive Durability                                             | none       |    14 |
| [ ]  | `D02 T35 §2`  | Registry Save/Restore Hive Bodies and Transactional Load                                          | none       |     4 |
| [ ]  | `D02 T35 §3`  | Quota Enforcement and Job Memory Accounting                                                       | none       |     8 |
| [ ]  | `D02 T35 §4`  | Module Identity and ELF Unwind Registration                                                       | none       |     5 |
| [ ]  | `D02 T35 §5`  | User-Buffer Hardening on Fault-Recoverable Usercopy                                               | none       |     5 |
| [ ]  | `D02 T35 §6`  | Boot, Entropy, Config and SRM Leftovers                                                           | none       |    13 |
| [ ]  | `D02 T12 §14` | Registry Syscalls (Core CRUD)                                                                     | none       |    13 |
| [ ]  | `D02 T14 §14` | Registry SMP Synchronization                                                                      | none       |    17 |
| [ ]  | `D02 T14 §4`  | Nt/Zw Registry Syscalls                                                                           | none       |    35 |
| [ ]  | `D02 T12 §15` | Registry Syscalls (Advanced)                                                                      | none       |     9 |
| [ ]  | `D02 T12 §34` | Per-service SSDT argument counts                                                                  | none       |     4 |
| [ ]  | `D02 T13 §8`  | Native Atom/NLS/Locale Syscalls                                                                   | none       |     9 |
| [ ]  | `D02 T14 §5`  | advapi32.dll Win32 Compatibility                                                                  | none       |    15 |
| [ ]  | `D02 T14 §6`  | Registry Virtualization & .reg Import/Export                                                      | none       |    11 |
| [ ]  | `D02 T13 §9`  | Retrofit Kernel Consumers                                                                         | none       |    11 |
| [ ]  | `D02 T13 §10` | Tests and Compatibility Corpus                                                                    | none       |     9 |
| [/]  | `D02 T16 §8`  | Native WNF-Compatible Syscall Surface                                                             | none       |     6 |
| [ ]  | `D02 T16 §10` | Unit/Boot Tests                                                                                   | none       |     6 |
| [/]  | `D02 T18 §7`  | Image Notifications and Callbacks                                                                 | none       |     8 |
| [ ]  | `D02 T30 §8`  | Native Health Query/Control APIs                                                                  | none       |     4 |
| [ ]  | `D02 T30 §9`  | Health Notifications and Dashboards                                                               | none       |     4 |
| [ ]  | `D02 T30 §10` | Tests and Chaos Scenarios                                                                         | none       |     4 |

### Phase 3 -- Memory and concurrency

Memory protection, the allocators, paging and reclaim, the scheduler, SMP and the synchronisation primitives are used by everything above the core, and their defects are the most expensive to find later, so they are settled before the drivers that lean on them.

| Done | Section       | Deliverable                                                                        | Needs | Items |
| :--: | ------------- | ---------------------------------------------------------------------------------- | ----- | ----: |
| [ ]  | `D03 T01 §1`  | `mprotect` / `NtProtectVirtualMemory`                                              | none  |     9 |
| [ ]  | `D03 T01 §2`  | W^X Enforcement                                                                    | none  |     5 |
| [ ]  | `D03 T01 §3`  | Demand Paging -- MEM_RESERVE / MEM_COMMIT                                          | none  |     9 |
| [ ]  | `D03 T01 §4`  | `NtQueryVirtualMemory`                                                             | none  |     6 |
| [ ]  | `D03 T01 §5`  | `VirtualAlloc` / `VirtualFree` / `VirtualProtect` Win32 Wrappers                   | none  |     7 |
| [ ]  | `D03 T01 §6`  | kmalloc Size Audit                                                                 | none  |     8 |
| [ ]  | `D03 T01 §7`  | Build-Time kmalloc Lint                                                            | none  |     4 |
| [ ]  | `D03 T01 §8`  | PMM Statistics                                                                     | none  |     6 |
| [ ]  | `D03 T01 §9`  | Heap Canaries + Double-Free Detection                                              | none  |     4 |
| [ ]  | `D03 T01 §10` | Kernel Memory Leak Detector                                                        | none  |     7 |
| [ ]  | `D03 T01 §11` | MMIO Mapping with UC Attributes + HPET Validation                                  | none  |    12 |
| [/]  | `D03 T01 §12` | Per-Process User Page Mapping (`vmm_map_user_page`)                                | none  |    10 |
| [ ]  | `D03 T01 §13` | Auto-Growing User Stacks                                                           | none  |     6 |
| [ ]  | `D03 T01 §14` | NtLockVirtualMemory / mlock -- Pin Pages in RAM                                    | none  |     6 |
| [ ]  | `D03 T01 §15` | Commit Charge Tracking + Enforcement                                               | none  |     6 |
| [ ]  | `D03 T01 §16` | Process Memory Counters (`GetProcessMemoryInfo`)                                   | none  |     8 |
| [ ]  | `D03 T02 §1`  | User-Space ASLR                                                                    | none  |     8 |
| [ ]  | `D03 T02 §2`  | KASLR -- Kernel Address Space Randomization                                        | none  |     8 |
| [ ]  | `D03 T02 §3`  | SMEP                                                                               | none  |     6 |
| [ ]  | `D03 T02 §4`  | SMAP + `copy_from_user` / `copy_to_user`                                           | none  |     9 |
| [ ]  | `D03 T02 §5`  | NX / DEP Mapping Audit                                                             | none  |     6 |
| [ ]  | `D03 T02 §6`  | KPTI Basic                                                                         | none  |     7 |
| [ ]  | `D03 T02 §7`  | CET Shadow Stack Stub                                                              | none  |     6 |
| [ ]  | `D03 T02 §8`  | Security Layout Report                                                             | none  |     7 |
| [ ]  | `D03 T03 §1`  | Growable Kernel Heap + SMP Spinlock                                                | none  |    11 |
| [ ]  | `D03 T03 §2`  | Memory Pressure Notifications                                                      | none  |     7 |
| [ ]  | `D03 T03 §3`  | vmalloc -- Virtual Contiguous Allocator                                            | none  |     7 |
| [ ]  | `D03 T03 §4`  | SLAB Allocator + Lock-Free Per-CPU Fast Path                                       | none  |    10 |
| [ ]  | `D03 T03 §5`  | SLAB Shrinker                                                                      | none  |     5 |
| [ ]  | `D03 T03 §6`  | Tagged Allocation API                                                              | none  |     7 |
| [ ]  | `D03 T03 §7`  | NonPagedPool / PagedPool                                                           | none  |     9 |
| [ ]  | `D03 T03 §8`  | Out-of-Band Metadata                                                               | none  |     7 |
| [ ]  | `D03 T03 §9`  | Type-Isolated Pools (kalloc_type Model)                                            | none  |     6 |
| [ ]  | `D03 T03 §10` | Per-Page Header Encoding Keys                                                      | none  |     5 |
| [ ]  | `D03 T03 §11` | Probabilistic Guard Pages (KFENCE Model)                                           | none  |     6 |
| [ ]  | `D03 T03 §12` | Production Quarantine (Delayed Reuse)                                              | none  |     6 |
| [ ]  | `D03 T03 §13` | Zero-on-Free                                                                       | none  |     5 |
| [ ]  | `D03 T03 §14` | Bulk Alloc/Free API                                                                | none  |     4 |
| [ ]  | `D03 T03 §15` | `/sys/slab` + `/sys/pooltags` + Shell Commands                                     | none  |     6 |
| [ ]  | `D03 T04 §1`  | Pagefile Manager + Swap Metadata Foundation                                        | none  |     9 |
| [ ]  | `D03 T04 §2`  | Working-Set Tracking + Lock-Aware Residency Rules                                  | none  |     8 |
| [ ]  | `D03 T04 §3`  | Background Reclaim + Modified-Page Writer                                          | none  |     8 |
| [ ]  | `D03 T04 §4`  | Lazy File-Backed Pager + Soft/Hard Fault Resolution                                | none  |     8 |
| [ ]  | `D03 T04 §5`  | Replacement Policy + Refault Tracking                                              | none  |     8 |
| [ ]  | `D03 T04 §6`  | Control Surface + Observability Handoff                                            | none  |     7 |
| [ ]  | `D03 T05 §1`  | COW `fork()`                                                                       | none  |     8 |
| [ ]  | `D03 T05 §2`  | Huge Pages (2 MiB)                                                                 | none  |     7 |
| [ ]  | `D03 T05 §3`  | 1 GiB Pages (Kernel MMIO)                                                          | none  |     6 |
| [ ]  | `D03 T05 §4`  | `madvise` / `MEM_RESET` Hints                                                      | none  |     8 |
| [ ]  | `D03 T05 §5`  | Section Object Multi-View Mappings                                                 | none  |     9 |
| [ ]  | `D03 T05 §6`  | Per-Process Memory Limits (Job Objects)                                            | none  |     7 |
| [ ]  | `D03 T05 §7`  | Zero-Copy DMA Buffer Pool                                                          | none  |     9 |
| [ ]  | `D03 T05 §8`  | Compressed Memory (zRAM-style)                                                     | none  |     8 |
| [ ]  | `D03 T05 §9`  | NUMA-Aware PMM                                                                     | none  |     7 |
| [ ]  | `D03 T05 §10` | Transparent Huge Pages Collapser                                                   | none  |     6 |
| [ ]  | `D03 T05 §11` | Address Windowing Extensions (AWE)                                                 | none  |     6 |
| [ ]  | `D03 T06 §1`  | 40-Level Priority Queues + O(1) Bitmap Dequeue                                     | none  |     8 |
| [ ]  | `D03 T06 §2`  | Dynamic Priority Aging                                                             | none  |     6 |
| [ ]  | `D03 T06 §6`  | CPU Affinity                                                                       | none  |     6 |
| [ ]  | `D03 T06 §8`  | Scheduler Tick Calibration                                                         | none  |     6 |
| [ ]  | `D03 T06 §3`  | CFS vruntime + `prio_to_weight` Table                                              | none  |     9 |
| [ ]  | `D03 T06 §4`  | `SCHED_FIFO` / `SCHED_RR` Real-Time Classes                                        | none  |     8 |
| [ ]  | `D03 T06 §5`  | `SCHED_DEADLINE` EDF Scheduling                                                    | none  |     8 |
| [ ]  | `D03 T06 §7`  | Scheduler Stats + `/sys/sched` VFS File                                            | none  |     6 |
| [ ]  | `D03 T06 §9`  | CPU Frequency Scaling Hook                                                         | none  |     8 |
| [ ]  | `D03 T06 §10` | Worker Factory Syscalls Wired to SSDT                                              | none  |     8 |
| [/]  | `D03 T06 §11` | Per-Thread Kernel Stack + TSS.rsp0 Switching                                       | none  |     8 |
| [ ]  | `D03 T06 §12` | Dynamic Thread Table + Resource-Driven Thread Limits                               | none  |     9 |
| [ ]  | `D03 T06 §13` | Dynamic Task Table + Reusable PID Slot Allocation                                  | none  |     9 |
| [ ]  | `D03 T06 §14` | Process/Job CPU Bandwidth Control                                                  | none  |     7 |
| [ ]  | `D03 T06 §15` | Wait/Wake Transaction Locking (Lost-Wakeup Class)                                  | none  |     9 |
| [ ]  | `D03 T06 §16` | Move XSAVE / FPU State Ownership from `struct task` to `struct thread`             | none  |     7 |
| [ ]  | `D03 T07 §8`  | `READ_ONCE` / `WRITE_ONCE` Macros                                                  | none  |     7 |
| [ ]  | `D03 T07 §1`  | SMP-Safe Atomics Audit + `smp_mb` Barriers                                         | none  |    10 |
| [ ]  | `D03 T07 §2`  | TLB Shootdown IPI                                                                  | none  |    12 |
| [ ]  | `D03 T07 §9`  | CPU Feature Intersection at AP Bringup                                             | none  |     7 |
| [ ]  | `D03 T07 §3`  | Per-CPU Run Queues                                                                 | none  |    15 |
| [ ]  | `D03 T07 §6`  | Per-CPU RCU Upgrade                                                                | none  |     7 |
| [ ]  | `D03 T07 §4`  | Work-Stealing Load Balancer                                                        | none  |     6 |
| [ ]  | `D03 T07 §7`  | CPU Hotplug Stub                                                                   | none  |     8 |
| [ ]  | `D03 T07 §5`  | Adaptive Mutex Spin-on-Owner                                                       | none  |     6 |
| [ ]  | `D03 T08 §1`  | Ticket Locks -- FIFO Fair Spinlocks                                                | none  |     8 |
| [ ]  | `D03 T08 §2`  | Preemption Count                                                                   | none  |     8 |
| [ ]  | `D03 T08 §3`  | Thread-Local Storage (TLS)                                                         | none  |     9 |
| [ ]  | `D03 T08 §4`  | Futexes                                                                            | none  |     8 |
| [ ]  | `D03 T08 §5`  | `pthread_once` / `call_once`                                                       | none  |     6 |
| [ ]  | `D03 T08 §7`  | Thread Cancellation                                                                | none  |    10 |
| [ ]  | `D03 T08 §11` | Mutex Wait-Queue SMP-Safety Backfill                                               | none  |    11 |
| [ ]  | `D03 T09 §1`  | Named Pipes (NPFS) -- Server Endpoint                                              | none  |     8 |
| [ ]  | `D03 T09 §2`  | Named Pipe Instances + Overlapped I/O                                              | none  |     7 |
| [ ]  | `D03 T09 §3`  | Mailslots (MSFS)                                                                   | none  |     8 |
| [ ]  | `D03 T09 §11` | Anonymous-Pipe Hardening -- SMP-Safe Slot State, Real Error Codes, Honest Capacity | none  |     6 |
| [ ]  | `D03 T10 §1`  | Thread Stack Guard Pages                                                           | none  |     7 |
| [ ]  | `D03 T10 §2`  | Preemption Count -- Debug Extensions                                               | none  |     6 |
| [ ]  | `D03 T10 §3`  | Heap Canaries + SLAB Red Zones                                                     | none  |     7 |
| [ ]  | `D03 T10 §5`  | Kernel Watchdog Thread                                                             | none  |     8 |
| [ ]  | `D03 T10 §8`  | Named Lock Browser (`/sys/locks`)                                                  | none  |     8 |
| [ ]  | `D03 T10 §4`  | Lock Dependency Validator (Lockdep)                                                | none  |     9 |
| [ ]  | `D03 T10 §7`  | Graphical Deadlock Visualisation                                                   | none  |     8 |
| [ ]  | `D03 T10 §9`  | KASAN -- Kernel Address Sanitiser                                                  | none  |    10 |
| [ ]  | `D03 T10 §6`  | KCSAN -- Kernel Concurrency Sanitiser                                              | none  |     8 |
| [ ]  | `D03 T10 §10` | `/sys/mem` Unified Memory Observability                                            | none  |     8 |
| [ ]  | `D03 T11 §1`  | Outgoing-Kernel Staging Area + Folio Preservation                                  | none  |     3 |
| [ ]  | `D03 T11 §2`  | Per-Subsystem Quiesce Callback Registry                                            | none  |     2 |
| [ ]  | `D03 T11 §3`  | VFS Writeback + FD Table Serialize                                                 | none  |     2 |
| [ ]  | `D03 T11 §4`  | Scheduler Drain + Thread Freeze                                                    | none  |     2 |
| [ ]  | `D03 T11 §5`  | Kexec-Equivalent Jump Into New Kernel Image                                        | none  |     2 |
| [ ]  | `D03 T11 §6`  | Incoming-Kernel Reattach Path (Splice Memory, Restore)                             | none  |     2 |
| [ ]  | `D03 T11 §7`  | Live-Update Syscall + nt_live_update SSDT Entry                                    | none  |     2 |
| [ ]  | `D03 T08 §9`  | Unified `_timeout(ms)` API                                                         | none  |     7 |
| [ ]  | `D03 T08 §8`  | `WaitForMultipleObjects` (`waitable_t` vtable)                                     | none  |     8 |
| [ ]  | `D03 T08 §6`  | `pthread_barrier_t` -- Kernel-Level Barrier                                        | none  |     8 |
| [ ]  | `D03 T08 §10` | Sync Syscalls Wired to SSDT (Keyed Events, Alerts)                                 | none  |    11 |
| [ ]  | `D03 T09 §4`  | I/O Completion Ports (IOCP)                                                        | none  |     9 |
| [ ]  | `D03 T09 §5`  | IOCP Worker Thread Pool + DMA Completion Hook                                      | none  |     9 |
| [ ]  | `D03 T09 §6`  | Win32 Ob-Backed Named Sync Objects                                                 | none  |     9 |
| [ ]  | `D03 T09 §7`  | LPC Basic -- Local Procedure Call                                                  | none  |    10 |
| [ ]  | `D03 T09 §8`  | ALPC Extensions                                                                    | none  |     4 |
| [ ]  | `D03 T09 §9`  | `ImpossibleRing` -- Async Submission Rings                                         | none  |    11 |
| [ ]  | `D03 T09 §10` | IPC Syscalls Wired to SSDT                                                         | none  |    12 |

### Phase 4 -- Drivers and hardware

With the kernel model and its memory and locking rules in place, the hardware comes up: ACPI and power, PCI, USB, input, timers, display and the driver model that loads them. Bare metal is the target, so these rows close on real hardware, not on an emulator alone.

| Done | Section       | Deliverable                                                    | Needs | Items |
| :--: | ------------- | -------------------------------------------------------------- | ----- | ----: |
| [ ]  | `D04 T02 §1`  | x2APIC Mode -- MSR Register Access                             | none  |    12 |
| [ ]  | `D04 T02 §3`  | MSI Vector Bitmap Allocator                                    | none  |     9 |
| [ ]  | `D04 T02 §4`  | TLB Shootdown IPI                                              | none  |     7 |
| [ ]  | `D04 T02 §5`  | NMI Watchdog via PMI Counter Overflow                          | none  |     8 |
| [ ]  | `D04 T03 §1`  | ACPICA AML Interpreter Integration                             | none  |    15 |
| [ ]  | `D02 T26 §34` | PCI D3cold via ACPI `_PS0`/`_PS3` Platform Methods             | none  |     6 |
| [ ]  | `D04 T03 §2`  | Clean Shutdown Sequence                                        | none  |     8 |
| [ ]  | `D04 T03 §3`  | ACPI Power Button SCI                                          | none  |     8 |
| [ ]  | `D04 T03 §4`  | Thermal Monitoring                                             | none  |     9 |
| [ ]  | `D02 T26 §14` | ACPI Thermal Zone Management                                   | none  |    20 |
| [ ]  | `D04 T03 §5`  | Battery Status (`_BST` / `_BIF`)                               | none  |     9 |
| [ ]  | `D04 T03 §6`  | CPU Frequency Scaling -- DVFS                                  | none  |     7 |
| [ ]  | `D02 T26 §15` | CPU Frequency Scaling Governor Framework                       | none  |    26 |
| [ ]  | `D04 T03 §9`  | ACPI S3 Suspend / Resume                                       | none  |     5 |
| [ ]  | `D04 T03 §10` | Hibernate (S4)                                                 | none  |     6 |
| [ ]  | `D04 T04 §1`  | SMEP + SMAP Late Init on All APs                               | none  |     6 |
| [ ]  | `D04 T04 §2`  | Hardware RNG -- `RDRAND` / `RDSEED` + ChaCha20 CSPRNG Fallback | none  |     9 |
| [ ]  | `D04 T04 §3`  | DMA Bounce Buffer Manager                                      | none  |     8 |
| [ ]  | `D04 T04 §5`  | Secure Boot UEFI Variable Read                                 | none  |     7 |
| [ ]  | `D04 T04 §6`  | TPM 2.0 Command Layer and IOCTL Surface                        | none  |     7 |
| [ ]  | `D04 T04 §7`  | Boot-Time Kernel Integrity Check                               | none  |    11 |
| [ ]  | `D04 T05 §1`  | Kernel Symbol Table + `EXPORT_SYMBOL`                          | none  |     8 |
| [ ]  | `D04 T05 §2`  | Module Build System                                            | none  |     7 |
| [ ]  | `D04 T05 §3`  | Driver Model + HAL Vtables + PCI Match Tables                  | none  |    11 |
| [ ]  | `D04 T01 §1`  | Canonical Device Node and Bus Path Schema                      | none  |     4 |
| [ ]  | `D04 T01 §2`  | Full PCI/PCIe Enumeration with Bridges                         | none  |     4 |
| [ ]  | `D04 T01 §4`  | ACPI Correlation                                               | none  |     4 |
| [ ]  | `D04 T01 §5`  | IRQ Routing and MSI/MSI-X Handoff                              | none  |     4 |
| [ ]  | `D04 T01 §6`  | Driver Bind/Unbind State Machine                               | none  |     4 |
| [ ]  | `D04 T05 §4`  | ELF Relocatable Module Loader                                  | none  |     6 |
| [ ]  | `D04 T05 §5`  | Auto-Load Modules at Boot                                      | none  |     9 |
| [ ]  | `D04 T05 §6`  | RTL8139 as First Loadable Module                               | none  |     8 |
| [ ]  | `D04 T05 §7`  | Plug and Play syscalls wired to SSDT                           | none  |     4 |
| [/]  | `D02 T18 §2`  | Global and Per-Process Image Registries                        | none  |     9 |
| [/]  | `D02 T18 §6`  | Loader Integration                                             | none  |     7 |
| [/]  | `D02 T18 §10` | Tests and Consistency Verifier                                 | none  |     5 |
| [ ]  | `D04 T06 §1`  | Firmware Directory and Manifest Format                         | none  |     4 |
| [ ]  | `D04 T06 §2`  | `request_firmware()` Kernel API                                | none  |     4 |
| [ ]  | `D04 T06 §3`  | Pinning and DMA-safe Copies                                    | none  |     4 |
| [ ]  | `D04 T06 §5`  | Version Matching                                               | none  |     3 |
| [ ]  | `D04 T06 §8`  | License Audit Report                                           | none  |     3 |
| [ ]  | `D04 T06 §9`  | Driver Conversion Pass                                         | none  |     3 |
| [ ]  | `D04 T07 §1`  | PCI Device Registry                                            | none  |     8 |
| [ ]  | `D04 T07 §2`  | Embedded PCI ID Database                                       | none  |     7 |
| [ ]  | `D04 T07 §3`  | Live Interrupt Counter                                         | none  |     7 |
| [ ]  | `D04 T07 §4`  | Driver Health Registry                                         | none  |     7 |
| [ ]  | `D04 T07 §5`  | `/sys/devices` + `/sys/interrupts` VFS Files                   | none  |     7 |
| [ ]  | `D04 T07 §7`  | Device Manager GUI                                             | none  |     8 |
| [ ]  | `D04 T07 §9`  | `SetupDiGetClassDevs` / `SetupDiEnumDeviceInfo` Win32 API      | none  |     6 |
| [ ]  | `D04 T07 §10` | Driver Unload / Reload                                         | none  |     7 |
| [ ]  | `D04 T08 §1`  | PCI Enhanced Config Space (ECAM)                               | none  |     7 |
| [ ]  | `D04 T01 §3`  | BAR Sizing and Resource Window Allocator                       | none  |     4 |
| [ ]  | `D04 T08 §4`  | HPET Timer Driver                                              | none  |     8 |
| [ ]  | `D04 T02 §2`  | LAPIC Timer Calibration via HPET                               | none  |     6 |
| [ ]  | `D04 T02 §6`  | Interrupt Profiler -- `/sys/interrupts`                        | none  |     7 |
| [ ]  | `D04 T08 §2`  | PCIe Capability Chain Scanner                                  | none  |     7 |
| [ ]  | `D04 T08 §3`  | MSI / MSI-X Interrupt Support                                  | none  |     8 |
| [ ]  | `D04 T08 §5`  | NVMe Storage Driver (Built-in)                                 | none  |    14 |
| [ ]  | `D04 T08 §6`  | PCIe Hot-Plug                                                  | none  |     8 |
| [ ]  | `D04 T01 §7`  | Hot-Plug and Surprise Removal                                  | none  |     5 |
| [ ]  | `D04 T09 §1`  | Hypervisor Detection                                           | none  |     6 |
| [ ]  | `D04 T09 §2`  | Unified `hv.h` Interface + Backend Dispatch                    | none  |     5 |
| [ ]  | `D04 T09 §3`  | Bare-Metal Null Backend                                        | none  |     5 |
| [ ]  | `D04 T09 §4`  | VBox Display Auto-Resize                                       | none  |     7 |
| [ ]  | `D04 T09 §5`  | VBox HGCM Client                                               | none  |     6 |
| [ ]  | `D04 T09 §6`  | VBox Shared Folders                                            | none  |     6 |
| [ ]  | `D04 T09 §7`  | VBox Shared Clipboard                                          | none  |     5 |
| [ ]  | `D04 T09 §8`  | VirtIO GPU Display Resize + Page Flip                          | none  |    10 |
| [ ]  | `D04 T09 §9`  | VirtIO 9P Shared Folders                                       | none  |     8 |
| [ ]  | `D04 T09 §10` | Hyper-V Synthetic HID                                          | none  |     7 |
| [ ]  | `D04 T09 §11` | Hyper-V Synthetic Video                                        | none  |     7 |
| [ ]  | `D04 T09 §12` | VirtIO RNG Guest Entropy                                       | none  |     6 |
| [ ]  | `D04 T10 §1`  | USB Core Abstraction Layer                                     | none  |    10 |
| [ ]  | `D04 T10 §2`  | USB String Descriptor Retrieval                                | none  |     5 |
| [ ]  | `D04 T10 §3`  | Isochronous Endpoint Support                                   | none  |     5 |
| [ ]  | `D04 T10 §4`  | Interrupt Endpoint Setup for HID                               | none  |     5 |
| [ ]  | `D04 T10 §5`  | USB HID Class Driver                                           | none  |     7 |
| [ ]  | `D04 T10 §6`  | PS/2 ↔ USB Input Fallback                                      | none  |     7 |
| [ ]  | `D04 T10 §7`  | USB MSC BOT Completion                                         | none  |    11 |
| [ ]  | `D04 T10 §11` | USB CDC-ECM Ethernet                                           | none  |    10 |
| [ ]  | `D04 T10 §12` | USB CDC-ACM Serial                                             | none  |     9 |
| [ ]  | `D04 T10 §13` | Bluetooth HCI via USB                                          | none  |     8 |
| [ ]  | `D04 T11 §1`  | Keyboard Layout System                                         | none  |     7 |
| [ ]  | `D04 T11 §5`  | PS/2 Packet Resync                                             | none  |     6 |
| [ ]  | `D04 T12 §1`  | I2C/SMBus Host Controller Driver                               | none  |    10 |
| [ ]  | `D04 T12 §2`  | Synaptics PS/2 Touchpad Fallback                               | none  |     7 |
| [ ]  | `D04 T12 §3`  | ACPI I2C Device Enumeration                                    | none  |     6 |
| [ ]  | `D04 T12 §9`  | Touchpad Control Panel (`mouse.cpl` Touchpad Tab)              | none  |     6 |
| [ ]  | `D04 T13 §1`  | Storage Driver Capability Matrix                               | none  |     3 |
| [ ]  | `D04 T13 §2`  | AHCI/SATA Parity Completion                                    | none  |     3 |
| [ ]  | `D04 T13 §3`  | ATA/ATAPI and Optical Media                                    | none  |     3 |
| [ ]  | `D04 T14 §1`  | License Tracking                                               | none  |     8 |
| [ ]  | `D04 T14 §2`  | Intel e1000 Module                                             | none  |    12 |
| [ ]  | `D04 T14 §3`  | VirtIO-net Module                                              | none  |     9 |
| [ ]  | `D04 T14 §4`  | RTL8169 / RTL8111 Gigabit Module                               | none  |    11 |
| [ ]  | `D04 T14 §5`  | Intel igc (I225 / I226 2.5 GbE) Module                         | none  |     9 |
| [ ]  | `D04 T14 §6`  | RTL8125 2.5 GbE Module                                         | none  |     9 |
| [ ]  | `D04 T14 §7`  | Network Driver Test Suite                                      | none  |     9 |
| [ ]  | `D04 T14 §8`  | WiFi 802.11 MAC Layer Stub                                     | none  |     7 |
| [ ]  | `D04 T14 §9`  | Intel iwlwifi Stub (P4 Stretch)                                | none  |     5 |
| [ ]  | `D04 T14 §10` | Realtek rtw89 Stub (P4 Stretch)                                | none  |     5 |
| [ ]  | `D04 T15 §1`  | WiFi Driver Abstraction Layer                                  | none  |     9 |
| [ ]  | `D04 T15 §2`  | 802.11 Frame Layer                                             | none  |     8 |
| [ ]  | `D04 T15 §3`  | WiFi Scan & Association State Machine                          | none  |     7 |
| [ ]  | `D04 T15 §5`  | WPA2-PSK Supplicant -- EAPOL 4-Way Handshake + CCMP            | none  |     8 |
| [ ]  | `D04 T15 §6`  | RTL8821CE / RTL8822BE PCIe WiFi Driver                         | none  |    10 |
| [ ]  | `D04 T15 §7`  | Intel AX200 / AX210 iwlwifi PCIe Driver                        | none  |    12 |
| [ ]  | `D04 T15 §8`  | MediaTek MT7921 / MT7922 PCIe WiFi Driver                      | none  |    11 |
| [ ]  | `D04 T15 §9`  | WiFi Power Management                                          | none  |     7 |
| [ ]  | `D04 T15 §10` | `ncpa.cpl` WiFi Tab + `netsh wlan` Commands                    | none  |    11 |
| [ ]  | `D04 T15 §11` | Airplane Mode Radio Coordinator                                | none  |     4 |
| [ ]  | `D04 T16 §1`  | HCI Transport Layer                                            | none  |     7 |
| [ ]  | `D04 T16 §2`  | HCI Firmware Loading                                           | none  |     8 |
| [ ]  | `D04 T16 §3`  | L2CAP -- Logical Link Control and Adaptation Protocol          | none  |     7 |
| [ ]  | `D04 T16 §4`  | SDP -- Service Discovery Protocol                              | none  |     6 |
| [ ]  | `D04 T16 §5`  | HID Profile -- Bluetooth HID                                   | none  |     8 |
| [ ]  | `D04 T16 §6`  | BLE -- Bluetooth Low Energy                                    | none  |     9 |
| [ ]  | `D04 T16 §7`  | RFCOMM & Serial Port Profile (SPP)                             | none  |     8 |
| [ ]  | `D04 T16 §8`  | A2DP + SBC Encoder + AVRCP                                     | none  |     8 |
| [ ]  | `D04 T16 §9`  | Bluetooth Manager + Pairing UI + `bluetooth.cpl`               | none  |     9 |
| [ ]  | `D04 T16 §10` | `btctl` Shell Command                                          | none  |    11 |
| [ ]  | `D04 T17 §1`  | `display_device_t` Vtable + Compositor Hook                    | none  |     7 |
| [ ]  | `D04 T17 §2`  | VBE/GOP Fallback Display Device                                | none  |     5 |
| [ ]  | `D04 T17 §3`  | Bochs/BGA Display Module                                       | none  |     7 |
| [ ]  | `D04 T17 §4`  | VirtIO-GPU Display Module                                      | none  |    10 |
| [ ]  | `D04 T17 §5`  | VMSVGA 2D Display Module                                       | none  |    13 |
| [ ]  | `D04 T17 §6`  | Multi-Head Support                                             | none  |    10 |
| [ ]  | `D04 T17 §7`  | Intel HD/UHD iGPU Modesetting Stub (P3)                        | none  |     7 |
| [ ]  | `D04 T17 §8`  | AMD APU Vega/RDNA Display Stub (P4)                            | none  |     8 |
| [ ]  | `D04 T18 §1`  | Audio HAL -- `audio_device_t` Vtable                           | none  |     7 |
| [ ]  | `D04 T18 §2`  | AC97 Sound Driver                                              | none  |    14 |
| [ ]  | `D04 T18 §3`  | Intel HDA Driver                                               | none  |    14 |
| [ ]  | `D04 T18 §4`  | VirtIO Sound Module                                            | none  |    11 |
| [ ]  | `D04 T18 §5`  | USB Audio Class 1.0 (UAC1) Playback                            | none  |     9 |
| [ ]  | `D04 T18 §6`  | Audio Device Hot-Plug                                          | none  |     6 |
| [ ]  | `D04 T18 §7`  | Convert All Audio Drivers to `.kmod`                           | none  |     6 |
| [ ]  | `D04 T19 §1`  | Sensor Class API                                               | none  |     3 |
| [ ]  | `D04 T19 §6`  | Audio Jack and Presence Sensors                                | none  |     3 |
| [ ]  | `D04 T19 §7`  | Event Notifications                                            | none  |     3 |
| [ ]  | `D04 T19 §9`  | BlackBox Timeline                                              | none  |     2 |
| [ ]  | `D04 T20 §1`  | Runtime COM Device Model                                       | none  |     3 |
| [ ]  | `D04 T20 §2`  | UART 16550A Cleanup                                            | none  |     3 |
| [ ]  | `D04 T20 §4`  | USB CDC-ACM Integration                                        | none  |     3 |
| [ ]  | `D04 T20 §5`  | Ownership Arbitration                                          | none  |     3 |
| [ ]  | `D04 T20 §6`  | Parallel/LPT Driver                                            | none  |     3 |
| [ ]  | `D04 T22 §1`  | Camera Class API and Buffer Model                              | none  |     3 |
| [ ]  | `D04 T22 §2`  | USB UVC Discovery and Controls                                 | none  |     3 |
| [ ]  | `D04 T22 §3`  | UVC Streaming and Frame Formats                                | none  |     3 |
| [ ]  | `D04 T22 §6`  | Capture Devices                                                | none  |     3 |
| [ ]  | `D04 T22 §8`  | Permissions and Audit                                          | none  |     3 |
| [ ]  | `D04 T25 §1`  | Certification Schema and Support Tiers                         | none  |     3 |
| [ ]  | `D04 T25 §2`  | VM Driver Test Matrix                                          | none  |     3 |
| [ ]  | `D04 T25 §3`  | Desktop/Laptop Bare-Metal Matrix                               | none  |     3 |
| [ ]  | `D04 T25 §4`  | USB/Peripheral Matrix                                          | none  |     3 |
| [ ]  | `D04 T25 §5`  | Network/Wireless/Bluetooth Matrix                              | none  |     2 |
| [ ]  | `D04 T25 §6`  | Suspend/Resume and Hot-Plug Matrix                             | none  |     3 |
| [ ]  | `D04 T25 §8`  | Failure Injection and Recovery                                 | none  |     2 |
| [ ]  | `D04 T03 §8`  | ACPI C-States Idle                                             | none  |     7 |
| [/]  | `D04 T03 §7`  | Power Profiles                                                 | none  |     3 |
| [ ]  | `D02 T26 §16` | CPU Idle Governor Framework                                    | none  |    14 |
| [ ]  | `D02 T26 §18` | Power Plan UI & `powercfg`                                     | none  |    23 |
| [ ]  | `D02 T26 §19` | Energy-Aware Scheduling Integration                            | none  |    10 |
| [ ]  | `D02 T26 §22` | User-Interaction-Aware QoS Throttling                          | none  |     8 |
| [ ]  | `D02 T26 §40` | Display Backlight Control (Brightness)                         | none  |     6 |
| [ ]  | `D04 T01 §8`  | Device Power States                                            | none  |     3 |
| [ ]  | `D04 T01 §9`  | Resource Conflict Diagnostics                                  | none  |     3 |
| [ ]  | `D04 T01 §10` | Tests                                                          | none  |     4 |
| [ ]  | `D04 T04 §4`  | IOMMU / VT-d + AMD-Vi                                          | none  |    11 |
| [ ]  | `D04 T04 §8`  | Intel CET Shadow Stacks                                        | none  |     9 |
| [ ]  | `D04 T06 §4`  | Integrity and Signature Policy                                 | none  |     4 |
| [ ]  | `D04 T06 §6`  | Firmware Update and Rollback                                   | none  |     3 |
| [ ]  | `D04 T13 §4`  | VirtIO-blk Production Integration                              | none  |     4 |
| [ ]  | `D04 T13 §5`  | SDHCI/eMMC/SD Card Driver                                      | none  |     3 |
| [ ]  | `D04 T19 §2`  | ACPI Sensors                                                   | none  |     3 |
| [ ]  | `D04 T20 §3`  | PCI Multiport Serial                                           | none  |     3 |
| [ ]  | `D04 T20 §7`  | GPIO Framework                                                 | none  |     3 |
| [ ]  | `D04 T20 §8`  | SPI Framework                                                  | none  |     3 |
| [ ]  | `D04 T20 §9`  | Industrial I/O Diagnostics                                     | none  |     3 |
| [ ]  | `D04 T20 §10` | Tests                                                          | none  |     3 |
| [ ]  | `D04 T23 §1`  | Device Class Model                                             | none  |     3 |
| [ ]  | `D04 T23 §4`  | Parallel/LPT Transport                                         | none  |     2 |
| [ ]  | `D04 T23 §8`  | Permissions                                                    | none  |     3 |
| [ ]  | `D04 T24 §1`  | External Expansion Topology Model                              | none  |     3 |
| [ ]  | `D04 T24 §3`  | Thunderbolt/USB4 Security Policy                               | none  |     3 |
| [ ]  | `D04 T24 §4`  | PCIe Tunneling Authorization                                   | none  |     3 |
| [ ]  | `D04 T24 §5`  | Display Alt-mode and eGPU Boundary                             | none  |     3 |
| [ ]  | `D04 T24 §6`  | Dock Power and Wake Events                                     | none  |     2 |
| [ ]  | `D04 T24 §9`  | External DMA Risk Report                                       | none  |     3 |
| [ ]  | `D04 T10 §8`  | Hot-Plug Interrupt Handling                                    | none  |    11 |
| [ ]  | `D04 T10 §9`  | USB Hub Class Driver                                           | none  |     7 |
| [ ]  | `D04 T10 §10` | EHCI Fallback (USB 2.0)                                        | none  |    11 |
| [ ]  | `D04 T07 §8`  | USB Device Tree Integration                                    | none  |     6 |
| [ ]  | `D04 T07 §6`  | `lspci` / `lsusb` Shell Commands                               | none  |     6 |
| [ ]  | `D04 T06 §7`  | Device Manager and Log Integration                             | none  |     3 |
| [ ]  | `D04 T06 §10` | Tests                                                          | none  |     3 |
| [ ]  | `D04 T13 §6`  | USB Card Readers and Multi-LUN                                 | none  |     3 |
| [ ]  | `D04 T13 §7`  | Storage Identity and Health                                    | none  |     3 |
| [ ]  | `D04 T13 §8`  | Surprise Removal and Media Change                              | none  |     4 |
| [ ]  | `D04 T13 §9`  | Diagnostics Report                                             | none  |     3 |
| [ ]  | `D04 T13 §10` | Tests                                                          | none  |     3 |
| [ ]  | `D04 T15 §4`  | RTL8188 / RTL8192 USB WiFi Driver                              | none  |     9 |
| [ ]  | `D04 T22 §5`  | MIPI/IPU Boundary                                              | none  |     3 |
| [ ]  | `D04 T22 §7`  | Scanner Class                                                  | none  |     3 |
| [ ]  | `D04 T23 §2`  | USB Printer Class Transport                                    | none  |     3 |
| [ ]  | `D04 T23 §3`  | IPP-over-USB Boundary                                          | none  |     3 |
| [ ]  | `D04 T23 §5`  | Multifunction Composition                                      | none  |     2 |
| [ ]  | `D04 T23 §6`  | Scanner Transport Handoff                                      | none  |     2 |
| [ ]  | `D04 T23 §7`  | Status Reporting                                               | none  |     2 |
| [ ]  | `D04 T23 §9`  | Diagnostics                                                    | none  |     2 |
| [ ]  | `D04 T23 §10` | Tests                                                          | none  |     3 |
| [ ]  | `D04 T24 §2`  | USB-C Dock and Hub Classification                              | none  |     3 |
| [ ]  | `D04 T24 §7`  | Hot-Plug Storm Resilience                                      | none  |     3 |
| [ ]  | `D04 T24 §8`  | Device Manager Topology                                        | none  |     2 |
| [ ]  | `D04 T24 §10` | Certification Matrix                                           | none  |     2 |
| [ ]  | `D04 T25 §7`  | Firmware and License Matrix                                    | none  |     3 |
| [ ]  | `D04 T25 §9`  | Support Bundle and Hardware Fingerprint                        | none  |     2 |
| [ ]  | `D04 T25 §10` | Release Gate Automation                                        | none  |     2 |
| [ ]  | `D04 T11 §6`  | PS/2 Intellimouse Scroll Wheel (ID 3)                          | none  |     6 |
| [ ]  | `D04 T11 §2`  | Built-in Layouts -- en-US, en-GB, de-DE, fr-FR, es-ES, Dvorak  | none  |     8 |
| [ ]  | `D04 T11 §3`  | UTF-8 / Unicode Codepoint Output                               | none  |     7 |
| [ ]  | `D04 T11 §4`  | Dead Key Compose                                               | none  |     6 |
| [ ]  | `D04 T11 §7`  | Explorer 5-Button Extension (ID 4)                             | none  |     6 |
| [ ]  | `D04 T11 §8`  | Mouse Acceleration + Sensitivity                               | none  |     6 |
| [ ]  | `D04 T11 §9`  | Raw Input Grab API                                             | none  |     7 |
| [ ]  | `D04 T11 §10` | Layout Switching -- Win+Space, Tray Indicator                  | none  |     7 |
| [ ]  | `D04 T11 §11` | Sticky Keys + Typematic Rate                                   | none  |     9 |
| [ ]  | `D04 T21 §1`  | Controller Class API                                           | none  |     3 |
| [ ]  | `D04 T21 §2`  | HID Gamepad Parser                                             | none  |     3 |
| [ ]  | `D04 T21 §3`  | XInput-Compatible Profiles                                     | none  |     3 |
| [ ]  | `D04 T21 §4`  | Bluetooth Controllers                                          | none  |     3 |
| [ ]  | `D04 T21 §5`  | Force Feedback and Rumble                                      | none  |     3 |
| [ ]  | `D04 T21 §6`  | Battery, LED and Player Index                                  | none  |     3 |
| [ ]  | `D04 T21 §7`  | Calibration and Profiles                                       | none  |     3 |
| [ ]  | `D04 T21 §8`  | Hot-Plug and Latency                                           | none  |     3 |
| [ ]  | `D04 T21 §9`  | Diagnostics                                                    | none  |     3 |
| [ ]  | `D04 T21 §10` | Tests                                                          | none  |     3 |
| [ ]  | `D04 T12 §4`  | HID-over-I2C (HoI2C) Transport                                 | none  |     7 |
| [ ]  | `D04 T12 §5`  | HID Report Descriptor Parser                                   | none  |     7 |
| [ ]  | `D04 T12 §7`  | ELAN and Goodix I2C Touchpad Quirks                            | none  |     7 |
| [ ]  | `D04 T12 §6`  | Microsoft Precision Touchpad (PTP) Multi-Touch                 | none  |     8 |
| [ ]  | `D04 T12 §8`  | PTP Gesture Engine                                             | none  |     9 |
| [ ]  | `D04 T12 §10` | `xinput list` / `touchpad-info` Shell Commands                 | none  |     6 |
| [ ]  | `D04 T19 §3`  | SMBus/I2C hwmon Transport                                      | none  |     3 |
| [ ]  | `D04 T19 §4`  | Fan, Voltage, and Chassis Sensors                              | none  |     3 |
| [ ]  | `D04 T19 §5`  | Accelerometer and Orientation Sensors                          | none  |     3 |
| [ ]  | `D04 T19 §8`  | Diagnostics                                                    | none  |     3 |
| [ ]  | `D04 T19 §10` | Tests                                                          | none  |     3 |
| [ ]  | `D04 T22 §4`  | Privacy LED and Switch Enforcement                             | none  |     3 |
| [ ]  | `D04 T22 §9`  | Diagnostics                                                    | none  |     3 |
| [ ]  | `D04 T22 §10` | Tests                                                          | none  |     3 |

### Phase 5 -- Storage and filesystems

Disks need the driver model of Phase 4, and everything from the registry hives to the installer needs a filesystem that survives power loss: AHCI and NVMe, the block layer, the cache, FAT, NTFS, ext4 and the native filesystem.

| Done | Section       | Deliverable                                                                     | Needs | Items |
| :--: | ------------- | ------------------------------------------------------------------------------- | ----- | ----: |
| [ ]  | `D05 T01 §1`  | VirtIO-blk Flush & Write-back                                                   | none  |     5 |
| [ ]  | `D05 T01 §2`  | VirtIO-blk Error Recovery                                                       | none  |     4 |
| [ ]  | `D05 T01 §6`  | Block Device I/O Metrics                                                        | none  |     8 |
| [ ]  | `D05 T01 §7`  | Block-Level Disk Cache                                                          | none  |    10 |
| [ ]  | `D05 T01 §8`  | Block-I/O QoS (IOPS / Bandwidth Caps)                                           | none  |     7 |
| [ ]  | `D05 T02 §1`  | B+ Tree Mutation Audit + Journaling                                             | none  |     7 |
| [ ]  | `D05 T02 §2`  | Write Data Path Wiring                                                          | none  |     8 |
| [ ]  | `D05 T02 §3`  | NTFS Volume Initialiser                                                         | none  |     7 |
| [ ]  | `D05 T02 §4`  | `$Secure` Security Stream                                                       | none  |     7 |
| [ ]  | `D05 T02 §5`  | Dirty Volume Recovery Wiring                                                    | none  |     5 |
| [ ]  | `D05 T02 §6`  | NTFS Format + Crash + Replay Test                                               | none  |     8 |
| [ ]  | `D05 T03 §1`  | Filesystem Probe Chain + Drive-Letter Assignment                                | none  |     8 |
| [ ]  | `D05 T03 §2`  | Boot Mount Sequence Rewrite                                                     | none  |     6 |
| [ ]  | `D05 T03 §6`  | Win32 Volume Query APIs                                                         | none  |     6 |
| [ ]  | `D05 T03 §7`  | Optical Drive Handling                                                          | none  |     7 |
| [x]  | `D05 T04 §1`  | FAT32 BPB Strict Validation                                                     | none  |     7 |
| [/]  | `D05 T04 §2`  | FSInfo Mount-Time Fallback                                                      | none  |     6 |
| [/]  | `D05 T04 §3`  | Dual-FAT Compare and Repair                                                     | none  |     4 |
| [/]  | `D05 T04 §4`  | LFN Write                                                                       | none  |    11 |
| [/]  | `D05 T04 §5`  | FAT32 Timestamps                                                                | none  |     6 |
| [/]  | `D05 T04 §6`  | FAT32 fsck                                                                      | none  |    13 |
| [/]  | `D05 T04 §7`  | VFS Case-Insensitive Path Resolution                                            | none  |     7 |
| [/]  | `D05 T04 §8`  | VFS Share-Mode Enforcement                                                      | none  |     7 |
| [/]  | `D05 T04 §9`  | Mark-for-Delete-on-Close                                                        | none  |     5 |
| [/]  | `D05 T04 §10` | Byte-Range Locks                                                                | none  |     6 |
| [/]  | `D05 T04 §11` | VFS Feature-Spoofing Stubs                                                      | none  |     5 |
| [/]  | `D05 T04 §12` | SFN Numeric Tail Collision Avoidance                                            | none  |     4 |
| [/]  | `D05 T04 §13` | FAT32 4 GiB Write Guard                                                         | none  |     4 |
| [/]  | `D05 T04 §14` | VFS Opportunistic Locks (Oplocks)                                               | none  |     6 |
| [x]  | `D05 T04 §15` | VFS_O_TRUNC End-to-End                                                          | none  |    11 |
| [x]  | `D05 T04 §16` | VFS Rename Replace-Existing -- Atomic Temp-File Durable-Write Primitive         | none  |    12 |
| [ ]  | `D05 T04 §17` | FAT32 Volume-Lock Hold Time -- No Blocking Disk I/O Under the `cli` `vol->lock` | none  |     2 |
| [ ]  | `D05 T04 §18` | Post-Ship Follow-Up Backfill (orphan cohort 2026-07-31)                         | none  |    14 |
| [ ]  | `D05 T05 §1`  | IRP Engine                                                                      | none  |     8 |
| [ ]  | `D05 T05 §2`  | MDL -- Memory Descriptor List                                                   | none  |     7 |
| [ ]  | `D05 T05 §3`  | File Handle Table                                                               | none  |     9 |
| [ ]  | `D05 T05 §4`  | `CreateFile` / `CloseHandle`                                                    | none  |     5 |
| [ ]  | `D05 T05 §5`  | `ReadFile` / `WriteFile` / `SetFilePointerEx`                                   | none  |    10 |
| [ ]  | `D05 T05 §6`  | Directory APIs + Per-Process CWD                                                | none  |    14 |
| [ ]  | `D05 T05 §7`  | File Management -- Delete, Move, Copy                                           | none  |     5 |
| [ ]  | `D05 T05 §8`  | File Metadata APIs                                                              | none  |     7 |
| [ ]  | `D05 T05 §9`  | Async I/O + APC                                                                 | none  |     6 |
| [ ]  | `D05 T05 §10` | Filter Manager Stub                                                             | none  |     7 |
| [ ]  | `D05 T05 §11` | Internal Migration -- `vfs_open` → `CreateFile`                                 | none  |     4 |
| [ ]  | `D05 T05 §12` | File Change Notifications                                                       | none  |     8 |
| [ ]  | `D05 T05 §14` | `DeviceIoControl` + FSCTL Dispatch + Oplock Ioctls                              | none  |    10 |
| [ ]  | `D05 T06 §1`  | Subsystem Verification                                                          | none  |    12 |
| [ ]  | `D05 T06 §2`  | Extended Inode v3                                                               | none  |     8 |
| [ ]  | `D05 T06 §3`  | Case-Insensitive Paths                                                          | none  |     6 |
| [ ]  | `D05 T06 §4`  | Alternate Data Streams (ADS)                                                    | none  |     9 |
| [ ]  | `D05 T06 §5`  | Security Descriptors                                                            | none  |     8 |
| [ ]  | `D05 T06 §6`  | Hard Links                                                                      | none  |     6 |
| [ ]  | `D05 T06 §7`  | Symbolic Links + Reparse Points                                                 | none  |     8 |
| [ ]  | `D05 T06 §8`  | Extended Attributes                                                             | none  |     7 |
| [ ]  | `D05 T06 §9`  | Object IDs                                                                      | none  |     8 |
| [ ]  | `D05 T06 §10` | Format Tool v3                                                                  | none  |     6 |
| [ ]  | `D05 T06 §11` | Parity Gate                                                                     | none  |     6 |
| [ ]  | `D05 T07 §1`  | Sparse File Support                                                             | none  |     8 |
| [ ]  | `D05 T07 §2`  | Real-Time TRIM                                                                  | none  |     6 |
| [ ]  | `D05 T07 §3`  | Transparent LZ4/Zstd Compression                                                | none  |     9 |
| [ ]  | `D05 T07 §4`  | Inline Block Deduplication                                                      | none  |     8 |
| [ ]  | `D05 T07 §5`  | Reflink Instant Copy                                                            | none  |     6 |
| [ ]  | `D05 T07 §6`  | Online Defragmentation                                                          | none  |     7 |
| [ ]  | `D05 T07 §7`  | Online Volume Resize                                                            | none  |     6 |
| [ ]  | `D05 T07 §8`  | Self-Healing Metadata                                                           | none  |     6 |
| [ ]  | `D05 T07 §9`  | Automatic Scheduled Snapshots                                                   | none  |     6 |
| [ ]  | `D05 T07 §10` | Per-File AES-256-XTS Encryption                                                 | none  |     9 |
| [ ]  | `D05 T07 §11` | USN Change Journal                                                              | none  |     6 |
| [ ]  | `D05 T07 §12` | Volume Quotas                                                                   | none  |     7 |
| [ ]  | `D05 T07 §13` | Filesystem-Native Storage Tiering                                               | none  |     8 |
| [ ]  | `D05 T07 §14` | Volume Health Dashboard                                                         | none  |     5 |
| [ ]  | `D05 T07 §15` | Comprehensive Test Suite                                                        | none  |     6 |
| [ ]  | `D05 T07 §16` | Inode-Table + On-Disk-Layout Hardening                                          | none  |     8 |
| [ ]  | `D05 T08 §1`  | VBR Parser + Boot Checksum                                                      | none  |     8 |
| [ ]  | `D05 T08 §2`  | Allocation Bitmap                                                               | none  |     7 |
| [ ]  | `D05 T08 §3`  | Up-Case Table                                                                   | none  |     5 |
| [ ]  | `D05 T08 §4`  | Directory Entry Sets                                                            | none  |     7 |
| [ ]  | `D05 T08 §5`  | Directory Read                                                                  | none  |     6 |
| [ ]  | `D05 T08 §6`  | File Read                                                                       | none  |     4 |
| [ ]  | `D05 T08 §7`  | File Write                                                                      | none  |     7 |
| [ ]  | `D05 T08 §8`  | File Create / Delete / Rename                                                   | none  |     6 |
| [ ]  | `D05 T08 §9`  | Timestamp Encoding                                                              | none  |     4 |
| [ ]  | `D05 T08 §10` | VFS Registration + Probe + fsck                                                 | none  |    10 |
| [ ]  | `D05 T08 §11` | Unicode Edge Cases                                                              | none  |     5 |
| [ ]  | `D05 T08 §12` | Large File Support (> 4 GiB)                                                    | none  |     5 |
| [ ]  | `D05 T09 §1`  | Superblock + Block Group Descriptors                                            | none  |     8 |
| [ ]  | `D05 T09 §2`  | Inode Reader                                                                    | none  |     6 |
| [ ]  | `D05 T09 §3`  | Extent Tree Decoder                                                             | none  |     6 |
| [ ]  | `D05 T09 §4`  | Directory htree (B+ Tree) + Readdir                                             | none  |     7 |
| [ ]  | `D05 T09 §5`  | File Read                                                                       | none  |     5 |
| [ ]  | `D05 T09 §6`  | JBD2 Journal Replay                                                             | none  |     8 |
| [ ]  | `D05 T09 §7`  | File Write                                                                      | none  |     4 |
| [ ]  | `D05 T09 §8`  | Extent Tree Write                                                               | none  |     5 |
| [ ]  | `D05 T03 §3`  | USB Hot-Plug Volume Arrival                                                     | none  |     6 |
| [ ]  | `D05 T03 §4`  | USB Safe Removal                                                                | none  |     4 |
| [ ]  | `D05 T03 §5`  | Manual `mount` / `umount` Shell Commands                                        | none  |     5 |
| [ ]  | `D05 T03 §8`  | Volume Control Ioctls                                                           | none  |     8 |
| [ ]  | `D05 T05 §13` | `GetDiskFreeSpaceEx` / `GetVolumeInformation`                                   | none  |     5 |
| [ ]  | `D05 T09 §9`  | Directory Write                                                                 | none  |     5 |
| [ ]  | `D05 T09 §10` | File Create / Delete / Rename                                                   | none  |     7 |
| [ ]  | `D05 T09 §11` | Extended Attributes                                                             | none  |     6 |
| [ ]  | `D05 T09 §12` | VFS Registration + Probe + fsck                                                 | none  |     9 |
| [ ]  | `D05 T10 §1`  | Superblock Parse + CRC32C Verify                                                | none  |     6 |
| [ ]  | `D05 T10 §2`  | B-Tree Node Format                                                              | none  |     6 |
| [ ]  | `D05 T10 §3`  | Chunk Tree -- Logical→Physical Map                                              | none  |     7 |
| [ ]  | `D05 T10 §4`  | Tree Search + Walk                                                              | none  |     5 |
| [ ]  | `D05 T10 §5`  | Root Tree + Subvolume Enumeration                                               | none  |     7 |
| [ ]  | `D05 T10 §6`  | Inode Reader                                                                    | none  |     5 |
| [ ]  | `D05 T10 §7`  | Extent Data Decoder                                                             | none  |     7 |
| [ ]  | `D05 T10 §8`  | Directory Reader                                                                | none  |     6 |
| [ ]  | `D05 T10 §9`  | Symlinks + XAttrs                                                               | none  |     6 |
| [ ]  | `D05 T10 §10` | VFS Registration + Probe + Read-Only Mount                                      | none  |     8 |
| [ ]  | `D05 T11 §1`  | SCSI MMC Layer -- READ TOC, DISC INFO, TRACK INFO                               | none  |     7 |
| [ ]  | `D05 T11 §2`  | ISO 9660 Base                                                                   | none  |    11 |
| [ ]  | `D05 T11 §3`  | Rock Ridge Extensions                                                           | none  |     8 |
| [ ]  | `D05 T11 §4`  | Joliet                                                                          | none  |     7 |
| [ ]  | `D05 T11 §5`  | UDF                                                                             | none  |    12 |
| [ ]  | `D05 T11 §6`  | Auto-Probe Priority                                                             | none  |     4 |
| [ ]  | `D05 T11 §7`  | Disc Label + Metadata                                                           | none  |     7 |
| [ ]  | `D05 T11 §8`  | Audio CD Raw Reads                                                              | none  |     6 |
| [ ]  | `D05 T12 §1`  | APFS Container Superblock + Fletcher-64                                         | none  |     7 |
| [ ]  | `D05 T12 §2`  | APFS Object Map B-Tree                                                          | none  |     7 |
| [ ]  | `D05 T12 §3`  | APFS Volume Mount                                                               | none  |     6 |
| [ ]  | `D05 T12 §4`  | APFS Filesystem B-Tree Search + Walk                                            | none  |     5 |
| [ ]  | `D05 T12 §5`  | APFS Inode Reader                                                               | none  |     6 |
| [ ]  | `D05 T12 §6`  | APFS File Extent Reader                                                         | none  |     4 |
| [ ]  | `D05 T12 §7`  | APFS Directory Reader                                                           | none  |     7 |
| [ ]  | `D05 T12 §8`  | APFS Symlinks + XAttrs                                                          | none  |     5 |
| [ ]  | `D05 T12 §9`  | HFS+ Volume Header + Journal Replay                                             | none  |     6 |
| [ ]  | `D05 T12 §10` | HFS+ Catalog B-Tree + Directory Reader                                          | none  |    10 |
| [ ]  | `D05 T12 §11` | HFS+ File Read + Extents Overflow B-Tree                                        | none  |     5 |
| [ ]  | `D05 T12 §12` | VFS Registration -- `apfs_probe`, `hfsplus_probe`, Read-Only Mount              | none  |    12 |
| [ ]  | `D05 T13 §1`  | GPT Partition Write                                                             | none  |     8 |
| [ ]  | `D05 T13 §2`  | MBR Partition Write                                                             | none  |     6 |
| [ ]  | `D05 T13 §3`  | `diskpart` CLI                                                                  | none  |    12 |
| [ ]  | `D05 T13 §4`  | `chkdsk` CLI + GUI                                                              | none  |     6 |
| [ ]  | `D05 T13 §5`  | `defrag` CLI + GUI                                                              | none  |     7 |
| [ ]  | `D05 T13 §6`  | `sfc` CLI + GUI                                                                 | none  |     7 |
| [ ]  | `D05 T13 §7`  | `recover` CLI + GUI                                                             | none  |     7 |
| [ ]  | `D05 T13 §8`  | Disk Management GUI                                                             | none  |     7 |
| [ ]  | `D05 T13 §9`  | `diskuse` CLI + GUI                                                             | none  |     6 |
| [ ]  | `D05 T13 §10` | Snapshot Manager CLI + GUI                                                      | none  |     5 |
| [ ]  | `D05 T14 §4`  | Generic `blkdev_stats_t` + Histogram API                                        | none  |    10 |
| [ ]  | `D05 T14 §1`  | `diskbench` CLI                                                                 | none  |     9 |
| [ ]  | `D05 T14 §3`  | SMART Extended Diagnostics                                                      | none  |     9 |
| [ ]  | `D05 T14 §5`  | Per-Device Latency Histogram + `/sys/ioqueue`                                   | none  |     6 |
| [ ]  | `D05 T14 §2`  | Disk Benchmark GUI                                                              | none  |     7 |
| [ ]  | `D05 T14 §6`  | VFS Hot-Path Profiler                                                           | none  |     7 |
| [ ]  | `D05 T14 §7`  | Storage Health Monitor Daemon                                                   | none  |     7 |
| [ ]  | `D05 T01 §3`  | AHCI Native Command Queuing (NCQ)                                               | none  |     8 |
| [ ]  | `D05 T01 §4`  | AHCI Error Recovery                                                             | none  |     7 |
| [ ]  | `D05 T01 §5`  | AHCI SMART Read                                                                 | none  |     5 |

### Phase 6 -- Desktop foundation

The window manager, the compositor, the input system and the control library are the base the graphics stack and the shell are built on, so they precede both.

| Done | Section      | Deliverable                               | Needs | Items |
| :--: | ------------ | ----------------------------------------- | ----- | ----: |
| [x]  | `D06 T01 §1` | Minimize, Maximize, Restore               | none  |     1 |
| [x]  | `D06 T01 §2` | Resize by Dragging Window Edges           | none  |     1 |
| [x]  | `D06 T01 §3` | Snap to Left/Right Half                   | none  |     1 |
| [x]  | `D06 T01 §4` | Global Hotkeys                            | none  |     1 |
| [x]  | `D06 T02 §1` | Dirty Rect Tracking Infrastructure        | none  |     1 |
| [/]  | `D06 T02 §2` | Partial Wallpaper Restore                 | none  |     2 |
| [/]  | `D06 T02 §3` | Per-Window Damage and Compositor Loop     | none  |     3 |
| [/]  | `D06 T02 §4` | Frame Timing and VSync                    | none  |     2 |
| [ ]  | `D06 T02 §5` | Terminal Render Clipping (Boot Bleed Fix) | none  |     4 |
| [ ]  | `D06 T03 §1` | Modifier Key Tracking                     | none  |     5 |
| [ ]  | `D06 T03 §2` | Key Event Struct + Dispatch Pipeline      | none  |     5 |
| [ ]  | `D06 T03 §3` | Focus Model and Tab Navigation            | none  |     6 |
| [ ]  | `D06 T03 §4` | Global Hotkey Dispatch Table              | none  |     6 |
| [ ]  | `D06 T03 §5` | Bare-Metal Input Bugs (Focus + Mouse)     | none  |    11 |
| [/]  | `D06 T04 §1` | Checkbox and Radio Button                 | none  |     2 |
| [/]  | `D06 T04 §2` | Progress Bar                              | none  |     2 |
| [/]  | `D06 T04 §3` | Listbox with Scrollbar                    | none  |     2 |
| [/]  | `D06 T04 §4` | Combobox (Dropdown)                       | none  |     2 |
| [/]  | `D06 T04 §5` | Context Menu (Popup)                      | none  |     2 |
| [/]  | `D06 T05 §1` | Desktop Icon Click Actions                | none  |     2 |
| [x]  | `D06 T05 §4` | Start Menu Program Launch                 | none  |     1 |
| [/]  | `D06 T05 §5` | Power and Settings Buttons                | none  |     2 |
| [x]  | `D06 T06 §1` | Icon Data Model and Grid Layout           | none  |     1 |
| [ ]  | `D06 T06 §2` | Special Folder Icons (Always Present)     | none  |     8 |
| [/]  | `D06 T06 §3` | Dynamic Icons from Desktop Directory      | none  |     3 |
| [/]  | `D06 T06 §4` | Icon Interaction (Select, Drag, Rename)   | none  |     4 |
| [ ]  | `D06 T06 §5` | Shortcut (.lnk) File Support              | none  |     6 |
| [ ]  | `D06 T06 §6` | File Type Icon Association                | none  |     7 |

### Phase 7 -- Networking

The network stack needs drivers and the kernel's I/O model but nothing from the desktop, so it runs here, ahead of the services and apps that need a socket.

| Done | Section       | Deliverable                                                 | Needs | Items |
| :--: | ------------- | ----------------------------------------------------------- | ----- | ----: |
| [ ]  | `D07 T01 §5`  | Network Interface Manager                                   | none  |    10 |
| [ ]  | `D07 T01 §1`  | TCP Header + Checksum                                       | none  |     8 |
| [ ]  | `D07 T01 §2`  | TCP Connection State Machine                                | none  |     7 |
| [ ]  | `D07 T01 §3`  | TCP API                                                     | none  |     8 |
| [ ]  | `D07 T01 §4`  | TCP Robustness                                              | none  |     7 |
| [ ]  | `D07 T01 §6`  | Loopback Interface                                          | none  |     6 |
| [ ]  | `D07 T01 §7`  | Stateful Connection Tracking                                | none  |    12 |
| [ ]  | `D07 T02 §1`  | DNS Query Builder                                           | none  |     7 |
| [ ]  | `D07 T02 §2`  | DNS Response Parser                                         | none  |     7 |
| [ ]  | `D07 T02 §3`  | DNS Cache                                                   | none  |     7 |
| [ ]  | `D07 T02 §4`  | AAAA Query Stub                                             | none  |     6 |
| [ ]  | `D07 T02 §5`  | Kernel Socket Layer                                         | none  |     9 |
| [ ]  | `D07 T02 §6`  | Server Sockets                                              | none  |     6 |
| [ ]  | `D07 T02 §7`  | Socket Options + Syscalls                                   | none  |     7 |
| [ ]  | `D07 T02 §8`  | `select()`                                                  | none  |     6 |
| [ ]  | `D07 T03 §1`  | URL Parser                                                  | none  |     6 |
| [ ]  | `D07 T03 §2`  | HTTP GET                                                    | none  |     8 |
| [ ]  | `D07 T03 §3`  | HTTP POST + Chunked Transfer Encoding                       | none  |     7 |
| [ ]  | `D07 T03 §4`  | `wget` / `curl` Shell Commands                              | none  |     8 |
| [ ]  | `D07 T03 §5`  | Mbed TLS Kernel Port                                        | none  |     8 |
| [ ]  | `D07 T03 §6`  | CA Certificate Store                                        | none  |     8 |
| [ ]  | `D07 T03 §7`  | HTTPS GET / POST                                            | none  |    11 |
| [ ]  | `D01 T27 §8`  | Firmware Advisor Live Metadata Refresh and Fixture Coverage | none  |     3 |
| [ ]  | `D07 T03 §8`  | HTTP/1.1 Keep-Alive Connection Pool                         | none  |     8 |
| [ ]  | `D07 T04 §1`  | IPv6 Header + Ethertype Routing                             | none  |     7 |
| [ ]  | `D07 T04 §2`  | IPv6 Send / Receive                                         | none  |     7 |
| [ ]  | `D07 T04 §3`  | ICMPv6 -- Neighbor Discovery (NS / NA / RS / RA)            | none  |    10 |
| [ ]  | `D07 T04 §4`  | Link-Local EUI-64 Autoconfiguration                         | none  |     7 |
| [ ]  | `D07 T04 §5`  | NDP Neighbor Cache                                          | none  |     9 |
| [ ]  | `D07 T04 §6`  | SLAAC -- Stateless Address Autoconfiguration                | none  |     7 |
| [ ]  | `D07 T04 §7`  | DHCPv6 Client                                               | none  |    10 |
| [ ]  | `D07 T04 §8`  | DNS AAAA Activation                                         | none  |     7 |
| [ ]  | `D07 T04 §9`  | Dual-Stack Socket API                                       | none  |    10 |
| [ ]  | `D07 T04 §10` | `ifconfig` IPv6 Display                                     | none  |     7 |
| [ ]  | `D07 T05 §1`  | Packet Filter Engine                                        | none  |    13 |
| [ ]  | `D07 T05 §2`  | IP Layer Hooks                                              | none  |     6 |
| [ ]  | `D07 T05 §3`  | Stateful Connection Tracking Integration                    | none  |     7 |
| [ ]  | `D07 T05 §4`  | Default Ruleset                                             | none  |     5 |
| [ ]  | `D07 T05 §5`  | IPv6 Firewall                                               | none  |     7 |
| [ ]  | `D07 T05 §9`  | Per-Rule Hit Counters                                       | none  |     7 |
| [ ]  | `D07 T05 §6`  | Firewall CLI                                                | none  |     9 |
| [ ]  | `D07 T05 §7`  | Registry Persistence                                        | none  |     6 |
| [ ]  | `D07 T05 §8`  | `firewall.cpl` Control Panel Applet                         | none  |     8 |
| [ ]  | `D07 T06 §1`  | DHCP Renewal Daemon                                         | none  |     9 |
| [ ]  | `D07 T06 §2`  | NTP Client                                                  | none  |    12 |
| [ ]  | `D07 T06 §3`  | Network Statistics API                                      | none  |     8 |
| [ ]  | `D07 T06 §4`  | `ifconfig` Command                                          | none  |     7 |
| [ ]  | `D07 T06 §5`  | Extended `ping` Command                                     | none  |     6 |
| [ ]  | `D07 T06 §6`  | `traceroute` Command                                        | none  |     7 |
| [ ]  | `D07 T06 §7`  | `netstat` Command                                           | none  |     7 |
| [ ]  | `D07 T06 §8`  | `/sys/net` VFS Aggregation File                             | none  |     7 |
| [ ]  | `D07 T06 §9`  | `ws2_32.dll` Winsock Stubs                                  | none  |    14 |
| [ ]  | `D07 T07 §1`  | Text-Only Browser                                           | none  |     9 |
| [ ]  | `D07 T07 §2`  | HTML Parser + DOM Tree                                      | none  |     9 |
| [ ]  | `D07 T07 §3`  | Tab Management                                              | none  |     8 |
| [ ]  | `D07 T07 §4`  | Block / Inline Layout Engine                                | none  |     9 |
| [ ]  | `D07 T07 §6`  | CSS Parser + Style Engine                                   | none  |     9 |
| [ ]  | `D07 T07 §7`  | JavaScript Stub                                             | none  |     6 |
| [ ]  | `D07 T07 §8`  | Bookmarks Manager                                           | none  |     9 |
| [ ]  | `D07 T07 §9`  | Download Manager                                            | none  |     8 |
| [ ]  | `D07 T07 §10` | Privacy + Security                                          | none  |     9 |
| [ ]  | `D07 T08 §1`  | FTP Protocol                                                | none  |    12 |
| [ ]  | `D07 T08 §2`  | FTP Shell + GUI Integration                                 | none  |     6 |
| [ ]  | `D07 T08 §3`  | FTPS / FTPES                                                | none  |     7 |
| [ ]  | `D07 T08 §4`  | SSH2 Transport Layer                                        | none  |    11 |
| [ ]  | `D07 T08 §5`  | SSH Authentication                                          | none  |     9 |
| [ ]  | `D07 T08 §6`  | SSH Channel + PTY                                           | none  |    10 |
| [ ]  | `D07 T08 §7`  | SSH Shell Integration                                       | none  |     6 |
| [ ]  | `D07 T08 §8`  | SSH Agent Stub                                              | none  |     9 |
| [ ]  | `D07 T08 §9`  | SFTP Subsystem                                              | none  |    14 |
| [ ]  | `D07 T08 §10` | Network Browsing and SMB Client                             | none  |     5 |
| [ ]  | `D07 T09 §1`  | Message Parser                                              | none  |    11 |
| [ ]  | `D07 T09 §2`  | SMTP Client                                                 | none  |    10 |
| [ ]  | `D07 T09 §3`  | POP3 Client                                                 | none  |    11 |
| [ ]  | `D07 T09 §4`  | IMAP Client                                                 | none  |    14 |
| [ ]  | `D07 T09 §5`  | Email Account Manager                                       | none  |     9 |
| [/]  | `D07 T09 §6`  | Notifications                                               | none  |     3 |
| [/]  | `D07 T09 §7`  | Email GUI                                                   | none  |     3 |
| [/]  | `D07 T09 §8`  | Compose Window                                              | none  |     3 |
| [ ]  | `D07 T09 §9`  | Contacts Integration                                        | none  |     9 |
| [ ]  | `D07 T09 §10` | Search                                                      | none  |     7 |
| [x]  | `D07 T10 §1`  | PDF Structure Parser                                        | none  |     1 |
| [x]  | `D07 T10 §2`  | Object Model                                                | none  |     1 |
| [x]  | `D07 T10 §3`  | Page Tree                                                   | none  |     1 |
| [x]  | `D07 T10 §4`  | Content Stream Interpreter                                  | none  |     1 |
| [x]  | `D07 T10 §5`  | Font Handling                                               | none  |     1 |
| [ ]  | `D07 T11 §1`  | RFC 5424 Packet Formatter and UDP Sender                    | none  |     5 |
| [ ]  | `D07 T11 §2`  | Registry-Gated Syslog Init                                  | none  |     5 |
| [ ]  | `D07 T11 §3`  | Network-Down Resilience                                     | none  |     4 |

### Phase 8 -- Graphics and UI

GDI, the compositor, fonts, text and the controls sit on the desktop foundation and are what every visible surface is drawn with, so they come before the shell and the apps.

| Done | Section       | Deliverable                                                             | Needs | Items |
| :--: | ------------- | ----------------------------------------------------------------------- | ----- | ----: |
| [ ]  | `D08 T01 §1`  | Render-Target Allocator, Views, and Cached Layers                       | none  |     8 |
| [ ]  | `D08 T01 §2`  | Clip, Transform, and State Stack                                        | none  |     7 |
| [ ]  | `D08 T01 §3`  | Path, Stroke, Fill, and SVG-Ready Vector Raster Contract                | none  |     8 |
| [ ]  | `D08 T01 §4`  | Theme-Aware Icon, Cursor, and Scalable Asset Pipeline                   | none  |    13 |
| [ ]  | `D08 T01 §5`  | Thumbnail, Preview, and Multi-Size Asset Cache                          | none  |     9 |
| [ ]  | `D07 T07 §5`  | Image Loading in Browser                                                | none  |     8 |
| [x]  | `D07 T10 §6`  | Image Rendering                                                         | none  |     1 |
| [x]  | `D07 T10 §7`  | Page Renderer                                                           | none  |     1 |
| [x]  | `D07 T10 §8`  | PDF Viewer App                                                          | none  |     1 |
| [ ]  | `D07 T10 §9`  | PDF from HTTP                                                           | none  |     6 |
| [x]  | `D07 T10 §10` | PDF Forms Stub                                                          | none  |     1 |
| [ ]  | `D08 T01 §6`  | Recorded Scene Lists and Deterministic Re-render                        | none  |     6 |
| [ ]  | `D08 T01 §7`  | SVG Runtime Renderer (Vendored plutovg and plutosvg)                    | none  |     8 |
| [ ]  | `D08 T01 §8`  | Icon Engine (Build-Time Generator and Validator)                        | none  |    10 |
| [ ]  | `D08 T01 §9`  | Missing Icons -- Detect, Draft-Generate, and Queue for Polish           | none  |     9 |
| [ ]  | `D08 T01 §10` | Automatic Icon Polish and Promotion                                     | none  |    10 |
| [ ]  | `D08 T02 §1`  | Font Catalog, Enumeration, Install/Remove, and Default Stacks           | none  |    10 |
| [ ]  | `D08 T02 §2`  | Fallback Chains, Emoji, and Color-Font Support                          | none  |     7 |
| [ ]  | `D08 T02 §3`  | Shaping, Bidi, Line-Break, and Paragraph Layout Engine                  | none  |     7 |
| [ ]  | `D08 T02 §4`  | Caret, Hit-Test, Selection, and Composition-Aware Text Editing Services | none  |     8 |
| [ ]  | `D08 T02 §5`  | Desktop and Win32 Wiring: `DrawText`, `ChooseFont`, `WM_FONTCHANGE`     | none  |     6 |
| [ ]  | `D08 T02 §6`  | Persistent Text-Run Cache and No-FPU Steady-State Draw Path             | none  |     6 |
| [ ]  | `D08 T02 §7`  | Vendor FreeType and HarfBuzz as the Rasterizer and Shaper               | none  |     5 |
| [ ]  | `D08 T03 §1`  | `theme_t` Struct                                                        | none  |     5 |
| [ ]  | `D08 T03 §2`  | `theme_get()` Singleton                                                 | none  |     5 |
| [ ]  | `D08 T03 §3`  | Built-in Presets                                                        | none  |     4 |
| [ ]  | `D08 T03 §4`  | Registry Load                                                           | none  |     4 |
| [ ]  | `D08 T03 §5`  | Custom Accent Color                                                     | none  |     3 |
| [ ]  | `D08 T03 §6`  | Migration Pass                                                          | none  |     8 |
| [ ]  | `D08 T03 §7`  | Theme-Aware Shadow Rendering                                            | none  |     5 |
| [ ]  | `D08 T03 §8`  | Hot-Reload                                                              | none  |     6 |
| [ ]  | `D08 T03 §9`  | Fluent Token Corpus and Win11 Personalization Contract                  | none  |     9 |
| [ ]  | `D08 T04 §1`  | Core Tween Engine                                                       | none  |     7 |
| [ ]  | `D08 T04 §2`  | Easing Functions                                                        | none  |     9 |
| [ ]  | `D08 T04 §3`  | Global Animation Manager                                                | none  |    12 |
| [ ]  | `D08 T04 §4`  | Registry Controls                                                       | none  |     9 |
| [ ]  | `D08 T04 §5`  | Window Transition Animations                                            | none  |    13 |
| [ ]  | `D08 T04 §6`  | Spring Physics                                                          | none  |     8 |
| [ ]  | `D08 T04 §7`  | Compositor Integration                                                  | none  |     7 |
| [ ]  | `D08 T05 §1`  | Checkbox                                                                | none  |     9 |
| [ ]  | `D08 T05 §2`  | Radio Button                                                            | none  |     8 |
| [ ]  | `D08 T05 §3`  | Slider / TrackBar                                                       | none  |     7 |
| [ ]  | `D08 T05 §4`  | Progress Bar                                                            | none  |     7 |
| [ ]  | `D08 T05 §5`  | Dropdown / ComboBox                                                     | none  |    12 |
| [ ]  | `D08 T05 §6`  | Tab Strip                                                               | none  |    11 |
| [ ]  | `D08 T05 §7`  | Theming                                                                 | none  |     4 |
| [ ]  | `D08 T05 §8`  | Accessibility Stubs                                                     | none  |     4 |
| [ ]  | `D08 T05 §9`  | Overlay Scroll Bar (Collapse and Expand on Hover)                       | none  |     6 |
| [ ]  | `D08 T06 §3`  | Toolbar                                                                 | none  |     9 |
| [ ]  | `D08 T06 §4`  | MenuBar                                                                 | none  |    11 |
| [ ]  | `D08 T06 §6`  | GroupBox + Separator                                                    | none  |     6 |
| [ ]  | `D08 T06 §5`  | StatusBar                                                               | none  |     8 |
| [ ]  | `D08 T06 §7`  | Tooltip Integration                                                     | none  |     8 |
| [ ]  | `D08 T06 §1`  | ListView                                                                | none  |    14 |
| [ ]  | `D08 T06 §2`  | TreeView                                                                | none  |    12 |
| [ ]  | `D08 T06 §8`  | Dialog System                                                           | none  |    11 |
| [ ]  | `D08 T06 §9`  | Info Bar Control                                                        | none  |     6 |
| [ ]  | `D08 T07 §1`  | Semantic Node Tree, Roles, States, Values, and Action Descriptors       | none  |     8 |
| [ ]  | `D08 T07 §2`  | Provider/Query API, Focus/Hit-Test Lookup, and Accessibility Events     | none  |     7 |
| [ ]  | `D08 T07 §3`  | Inspect Bridge, Screen-Reader Hooks, and Automation Subscription Model  | none  |     6 |
| [ ]  | `D08 T07 §4`  | IME and Text-Composition Framework                                      | none  |     6 |
| [ ]  | `D08 T07 §5`  | Win32 and Platform-Service Wiring                                       | none  |     6 |
| [ ]  | `D08 T07 §6`  | Deterministic Automation Transport for Testing and Assistive Tooling    | none  |     6 |
| [ ]  | `D08 T17 §16` | Built-OS Layout Check                                                   | none  |     8 |
| [ ]  | `D08 T17 §18` | Pixel Comparison Against Reference Screenshots                          | none  |     9 |
| [ ]  | `D08 T08 §2`  | Window Decorations                                                      | none  |    11 |
| [ ]  | `D08 T08 §1`  | Minimize / Maximize / Restore                                           | none  |    11 |
| [ ]  | `D08 T08 §3`  | Snap Layouts                                                            | none  |     6 |
| [ ]  | `D08 T08 §4`  | Desktop Icons                                                           | none  |     8 |
| [ ]  | `D08 T08 §5`  | Keyboard Shortcuts & Task Switching                                     | none  |     9 |
| [ ]  | `D08 T08 §6`  | Alt+Tab Task Switcher                                                   | none  |     9 |
| [ ]  | `D08 T08 §7`  | Drag and Drop                                                           | none  |     8 |
| [ ]  | `D08 T08 §8`  | Compositor Performance                                                  | none  |     9 |
| [ ]  | `D01 T14 §7`  | System-Wide Multi-Instance Spinner                                      | none  |     9 |
| [ ]  | `D08 T09 §3`  | Wallpaper Engine                                                        | none  |     8 |
| [ ]  | `D08 T09 §4`  | DPI Scaling                                                             | none  |     9 |
| [ ]  | `D08 T09 §1`  | Context Menu Engine                                                     | none  |    11 |
| [ ]  | `D06 T06 §7`  | Icon Right-Click Context Menu                                           | none  |     9 |
| [ ]  | `D08 T09 §2`  | Desktop Right-Click Menu                                                | none  |    10 |
| [x]  | `D06 T05 §2`  | Desktop Right-Click Context Menu                                        | none  |     1 |
| [ ]  | `D08 T09 §5`  | Screenshot                                                              | none  |     8 |
| [ ]  | `D08 T09 §6`  | Night Light                                                             | none  |     7 |
| [ ]  | `D08 T09 §7`  | Focus / Do Not Disturb                                                  | none  |     8 |
| [ ]  | `D08 T09 §8`  | Quick Settings Panel                                                    | none  |    11 |
| [ ]  | `D08 T09 §9`  | Virtual Desktops                                                        | none  |    11 |
| [ ]  | `D08 T10 §1`  | Taskbar Window List                                                     | none  |    12 |
| [x]  | `D06 T05 §3`  | Taskbar Window State Sync                                               | none  |     1 |
| [ ]  | `D08 T10 §3`  | Aero Peek                                                               | none  |     9 |
| [ ]  | `D08 T10 §4`  | Taskbar Progress Badges                                                 | none  |     7 |
| [ ]  | `D08 T10 §5`  | Pinned Apps                                                             | none  |    10 |
| [ ]  | `D08 T10 §2`  | Taskbar Button Context Menu                                             | none  |     7 |
| [ ]  | `D08 T10 §6`  | Jump Lists                                                              | none  |     8 |
| [ ]  | `D08 T10 §7`  | Taskbar Auto-Hide                                                       | none  |     9 |
| [ ]  | `D08 T10 §8`  | Taskbar Customization                                                   | none  |     5 |
| [ ]  | `D08 T11 §1`  | Start Menu Data Loading                                                 | none  |     9 |
| [ ]  | `D08 T11 §2`  | Start Menu Interaction                                                  | none  |     8 |
| [ ]  | `D08 T11 §3`  | Start Menu Search                                                       | none  |     5 |
| [ ]  | `D08 T11 §4`  | System Tray Icons                                                       | none  |    10 |
| [ ]  | `D08 T11 §5`  | Toast Notifications                                                     | none  |    11 |
| [ ]  | `D08 T11 §6`  | Notification Center                                                     | none  |    11 |
| [ ]  | `D08 T11 §7`  | Notification Settings                                                   | none  |     8 |
| [ ]  | `D08 T12 §1`  | Kernel Time API                                                         | none  |    13 |
| [ ]  | `D08 T12 §2`  | Time Formatting                                                         | none  |     6 |
| [ ]  | `D08 T12 §5`  | Timezone Database                                                       | none  |     8 |
| [ ]  | `D08 T12 §3`  | Taskbar Clock                                                           | none  |     9 |
| [ ]  | `D08 T12 §4`  | Date/Time Control Panel                                                 | none  |     5 |
| [ ]  | `D08 T12 §6`  | Monotonic Uptime Extension                                              | none  |     6 |
| [ ]  | `D08 T13 §3`  | Build Pipeline                                                          | none  |     4 |
| [ ]  | `D08 T13 §1`  | Boot Splash Renderer                                                    | none  |     6 |
| [ ]  | `D08 T13 §2`  | Loading Spinner                                                         | none  |     5 |
| [ ]  | `D08 T13 §4`  | Boot Progress Milestones                                                | none  |     6 |
| [ ]  | `D08 T13 §5`  | F8 Boot Menu                                                            | none  |    11 |
| [x]  | `D08 T13 §6`  | [MOVED] Crash Loop Protection                                           | none  |     1 |
| [x]  | `D08 T13 §7`  | [MOVED] BSOD Auto-Restart Validation                                    | none  |     1 |
| [ ]  | `D08 T14 §1`  | Win32 Shell Icon Index Map                                              | none  |     7 |
| [ ]  | `D08 T14 §2`  | GDI Device Context Stubs                                                | none  |    13 |
| [ ]  | `D08 T14 §3`  | GDI Drawing Functions                                                   | none  |    13 |
| [ ]  | `D08 T14 §4`  | GDI Text Functions                                                      | none  |     7 |
| [ ]  | `D08 T14 §5`  | USER32 Window Functions                                                 | none  |    13 |
| [ ]  | `D08 T14 §6`  | USER32 Message Loop                                                     | none  |    13 |
| [ ]  | `D08 T14 §7`  | USER32 Cursor, Icon & System Metrics                                    | none  |     9 |
| [ ]  | `D08 T14 §8`  | USER32 Dialogs                                                          | none  |     8 |
| [ ]  | `D08 T15 §1`  | Shadow SSDT Infrastructure (Table 1 Dispatch)                           | none  |     7 |
| [ ]  | `D08 T15 §2`  | GDI Device Context (DC) Object Table                                    | none  |     8 |
| [ ]  | `D08 T15 §3`  | GDI Drawing Syscalls                                                    | none  |    15 |
| [ ]  | `D08 T15 §4`  | GDI Text and Font Syscalls                                              | none  |     9 |
| [ ]  | `D08 T15 §5`  | GDI Bitmap and DIB Syscalls                                             | none  |     8 |
| [ ]  | `D08 T15 §6`  | GDI Pen, Brush, and Region Syscalls                                     | none  |    11 |
| [ ]  | `D08 T15 §7`  | USER Window Management Syscalls                                         | none  |    17 |
| [ ]  | `D08 T15 §8`  | USER Message Queue Syscalls                                             | none  |    14 |
| [ ]  | `D08 T15 §9`  | USER Input and Cursor Syscalls                                          | none  |    12 |
| [ ]  | `D08 T15 §10` | USER Menu and Accelerator Syscalls                                      | none  |    11 |
| [ ]  | `D08 T15 §11` | USER Clipboard Syscalls                                                 | none  |    11 |
| [ ]  | `D08 T15 §12` | Migrate Legacy SYS_* to Shadow SSDT                                     | none  |     7 |
| [ ]  | `D08 T15 §13` | GDI Path, Curve, and Extended Drawing                                   | none  |     9 |
| [ ]  | `D08 T15 §14` | GDI Transform, Palette, and Color Management                            | none  |     6 |
| [ ]  | `D08 T15 §15` | GDI Print and Metafile                                                  | none  |     5 |
| [ ]  | `D08 T15 §16` | GDI Font Advanced                                                       | none  |     9 |
| [ ]  | `D08 T15 §17` | GDI Extended Object Management                                          | none  |     6 |
| [ ]  | `D08 T15 §18` | USER Window Properties, Styles, and Enumeration                         | none  |    10 |
| [ ]  | `D08 T15 §19` | USER Dialog, Caret, and Drawing Helpers                                 | none  |     7 |
| [ ]  | `D08 T15 §20` | USER Scrollbar                                                          | none  |     5 |
| [ ]  | `D08 T15 §21` | USER Keyboard, IME, and Hook                                            | none  |     7 |
| [ ]  | `D08 T15 §22` | USER DPI, Accessibility, and System Parameters                          | none  |     6 |
| [ ]  | `D08 T15 §23` | USER Raw Input, Touch, and Gesture                                      | none  |     6 |
| [ ]  | `D08 T15 §24` | USER Multi-Monitor and Display                                          | none  |     6 |
| [ ]  | `D08 T15 §25` | USER Shell Integration                                                  | none  |     7 |
| [ ]  | `D08 T15 §26` | GDI/USER DirectX and DXGI Kernel Integration                            | none  |    11 |
| [ ]  | `D08 T15 §27` | Impossible OS Exclusive Graphics Extensions                             | none  |    14 |
| [ ]  | `D08 T16 §1`  | Dispatch split and guard rails                                          | none  |     3 |
| [ ]  | `D08 T16 §2`  | NTSTATUS and user-buffer rules                                          | none  |     2 |
| [ ]  | `D08 T16 §3`  | Static asserts, headers, and audits                                     | none  |     2 |
| [ ]  | `D08 T16 §4`  | User-mode index contract                                                | none  |     1 |
| [ ]  | `D08 T16 §5`  | KeUserModeCallback integration review                                   | none  |     2 |
| [ ]  | `D08 T18 §1`  | Element Tree and Property System                                        | none  |     9 |
| [ ]  | `D08 T18 §2`  | Layout Engine: Measure and Arrange                                      | none  |     8 |
| [ ]  | `D08 T18 §3`  | Layout Panels                                                           | none  |     8 |
| [ ]  | `D08 T18 §4`  | Scrolling and Virtualization Engine                                     | none  |     8 |
| [ ]  | `D08 T18 §5`  | Style, Template and Visual State Engine                                 | none  |     8 |
| [ ]  | `D08 T18 §6`  | Theme Resource Resolution                                               | none  |     8 |
| [ ]  | `D08 T18 §7`  | Routed Input, Focus and Keyboard Navigation                             | none  |     9 |
| [ ]  | `D08 T18 §8`  | Pointer, Touch and Gesture Recognition                                  | none  |     8 |
| [ ]  | `D08 T18 §9`  | Command Engine                                                          | none  |     8 |
| [ ]  | `D08 T18 §10` | Data Binding and Observable Collections                                 | none  |     8 |
| [ ]  | `D08 T18 §11` | UI Markup Compiler                                                      | none  |     8 |
| [ ]  | `D08 T18 §12` | Composition Visual Tree                                                 | none  |     9 |
| [ ]  | `D08 T18 §13` | Backdrop Materials Engine                                               | none  |     8 |
| [ ]  | `D08 T18 §14` | Popup, Flyout and Light-Dismiss Layer                                   | none  |     9 |
| [ ]  | `D08 T18 §15` | Navigation Framework                                                    | none  |     8 |
| [ ]  | `D08 T18 §16` | Settings Platform                                                       | none  |     9 |
| [ ]  | `D08 T18 §17` | Shell Namespace Engine                                                  | none  |     9 |
| [ ]  | `D08 T18 §18` | Property and Column System                                              | none  |     8 |
| [ ]  | `D08 T18 §19` | Shell Extension Points                                                  | none  |     9 |

### Phase 9 -- Desktop shell

The taskbar, Start, the file manager and the settings surfaces are built from the controls of Phase 8 and are the first thing a user judges the system by.

| Done | Section       | Deliverable                                   | Needs | Items |
| :--: | ------------- | --------------------------------------------- | ----- | ----: |
| [ ]  | `D09 T01 §1`  | Kernel Clipboard Buffer                       | none  |    10 |
| [ ]  | `D09 T01 §3`  | Win32 Clipboard API Stubs                     | none  |    11 |
| [ ]  | `D09 T01 §5`  | Multi-Format Clipboard                        | none  |     8 |
| [ ]  | `D09 T01 §2`  | Keyboard Shortcut Wiring                      | none  |    10 |
| [ ]  | `D09 T01 §4`  | Clipboard History                             | none  |    11 |
| [ ]  | `D09 T02 §1`  | Extension-to-App Mapping                      | none  |     7 |
| [ ]  | `D09 T02 §2`  | Default Associations on First Boot            | none  |     6 |
| [ ]  | `D09 T02 §3`  | Open With Dialog                              | none  |     6 |
| [ ]  | `D09 T02 §4`  | Shortcut Files (.lnk)                         | none  |     6 |
| [ ]  | `D09 T02 §5`  | Desktop Shortcut Integration                  | none  |     6 |
| [ ]  | `D09 T02 §7`  | System Sounds                                 | none  |     8 |
| [x]  | `D09 T02 §8`  | Font Manager App                              | none  |     1 |
| [ ]  | `D09 T02 §9`  | Icon Extraction: EIF SVG and PE ICO Resources | none  |     5 |
| [ ]  | `D09 T03 §1`  | User Account Stub                             | none  |     9 |
| [ ]  | `D09 T03 §2`  | Service Manager Core                          | none  |    13 |
| [ ]  | `D09 T03 §3`  | Auto-Restart & Crash Recovery                 | none  |     8 |
| [ ]  | `D09 T03 §4`  | Graceful Degradation                          | none  |     5 |
| [ ]  | `D09 T03 §5`  | `sc` Shell Command                            | none  |     7 |
| [ ]  | `D09 T03 §6`  | Built-In Services                             | none  |     9 |
| [ ]  | `D09 T03 §7`  | Notification Service                          | none  |     7 |
| [ ]  | `D09 T03 §8`  | Autostart Programs                            | none  |     8 |
| [ ]  | `D09 T04 §1`  | Recycle Bin Core                              | none  |    11 |
| [ ]  | `D09 T02 §6`  | Recycle Bin Desktop Icon States               | none  |     4 |
| [ ]  | `D09 T04 §2`  | Recycle Bin Window App                        | none  |    10 |
| [ ]  | `D09 T04 §3`  | miniz ZIP Library Integration                 | none  |     5 |
| [ ]  | `D09 T04 §4`  | ZIP Kernel API                                | none  |    12 |
| [ ]  | `D09 T04 §5`  | ZIP Shell Commands                            | none  |     5 |
| [ ]  | `D09 T05 §1`  | Search Index Core                             | none  |    10 |
| [ ]  | `D09 T05 §2`  | Search Query API                              | none  |     7 |
| [ ]  | `D09 T05 §3`  | `find` Shell Command                          | none  |     6 |
| [ ]  | `D09 T05 §4`  | Start Menu Integration                        | none  |     6 |
| [ ]  | `D09 T05 §6`  | Index Change Notifications                    | none  |     8 |
| [/]  | `D09 T06 §1`  | CSPRNG                                        | none  |     2 |
| [ ]  | `D09 T06 §2`  | Password Hashing                              | none  |     6 |
| [ ]  | `D09 T06 §3`  | User Account System                           | none  |     9 |
| [ ]  | `D09 T06 §4`  | Authentication API                            | none  |     7 |
| [ ]  | `D09 T06 §5`  | User Home Directories                         | none  |     5 |
| [ ]  | `D09 T06 §6`  | Login Screen UI                               | none  |    10 |
| [ ]  | `D09 T06 §7`  | Login Flow                                    | none  |     7 |
| [ ]  | `D09 T06 §8`  | Lock Screen                                   | none  |     7 |
| [ ]  | `D09 T06 §9`  | User Switching                                | none  |     5 |
| [ ]  | `D09 T06 §10` | File Permissions                              | none  |     9 |
| [ ]  | `D09 T06 §11` | UAC-Equivalent Elevation                      | none  |     9 |
| [ ]  | `D09 T07 §1`  | Crypto Primitives                             | none  |    18 |
| [ ]  | `D09 T07 §2`  | TLS Handshake Helper                          | none  |     5 |
| [ ]  | `D09 T07 §3`  | X.509 Certificate Store                       | none  |     9 |
| [ ]  | `D09 T07 §4`  | Key Store                                     | none  |     6 |
| [ ]  | `D09 T07 §5`  | BCrypt API                                    | none  |    14 |
| [ ]  | `D09 T07 §6`  | NCrypt API                                    | none  |    11 |
| [ ]  | `D09 T07 §7`  | Code Signing                                  | none  |     8 |
| [ ]  | `D09 T07 §8`  | EFS Integration                               | none  |    11 |
| [ ]  | `D09 T08 §1`  | Terminal Cell Grid                            | none  |    15 |
| [ ]  | `D09 T08 §2`  | Character Rendering                           | none  |     8 |
| [ ]  | `D09 T08 §3`  | ANSI Escape Parser                            | none  |    10 |
| [ ]  | `D09 T08 §4`  | Scrollback & Text Selection                   | none  |    10 |
| [ ]  | `D09 T08 §5`  | Resize Handling                               | none  |     5 |
| [ ]  | `D09 T08 §6`  | Registry Settings                             | none  |     7 |
| [ ]  | `D09 T08 §7`  | Advanced Features                             | none  |     9 |
| [ ]  | `D09 T08 §8`  | Pseudo-Console (ConPTY)                       | none  |     4 |
| [ ]  | `D09 T09 §1`  | Core Layout                                   | none  |    12 |
| [ ]  | `D09 T09 §2`  | Sidebar                                       | none  |     6 |
| [ ]  | `D09 T09 §6`  | File Search                                   | none  |     8 |
| [ ]  | `D09 T05 §5`  | File Manager Integration                      | none  |     4 |
| [x]  | `D09 T10 §1`  | Gap Buffer                                    | none  |     1 |
| [x]  | `D09 T10 §2`  | Text Rendering & Cursor                       | none  |     1 |
| [x]  | `D09 T10 §3`  | Undo/Redo                                     | none  |     1 |
| [x]  | `D09 T10 §4`  | File Menu                                     | none  |     1 |
| [x]  | `D09 T10 §5`  | Editing Features                              | none  |     1 |
| [x]  | `D09 T10 §6`  | Find & Replace                                | none  |     1 |
| [x]  | `D09 T10 §7`  | Stretch Features                              | none  |     1 |
| [ ]  | `D09 T09 §3`  | View Modes                                    | none  |     9 |
| [ ]  | `D09 T11 §1`  | CPL Framework                                 | none  |     5 |
| [ ]  | `D09 T11 §2`  | Control Panel Host App                        | none  |    10 |
| [ ]  | `D09 T11 §3`  | Core Applets                                  | none  |    15 |
| [ ]  | `D09 T11 §4`  | Additional Applets                            | none  |     6 |
| [ ]  | `D09 T11 §5`  | Settings Search                               | none  |     7 |
| [ ]  | `D09 T03 §9`  | Win32 System Info Stubs                       | none  |    11 |
| [ ]  | `D09 T04 §6`  | Task Scheduler Core                           | none  |     9 |
| [ ]  | `D09 T04 §7`  | Built-In Scheduled Tasks                      | none  |     9 |
| [ ]  | `D09 T04 §8`  | `at` Shell Command                            | none  |     7 |
| [ ]  | `D09 T09 §4`  | File Operations                               | none  |    11 |
| [ ]  | `D09 T09 §5`  | Context Menus                                 | none  |    11 |
| [ ]  | `D09 T09 §7`  | Drag and Drop                                 | none  |     8 |
| [ ]  | `D09 T09 §8`  | Advanced Features                             | none  |     9 |
| [ ]  | `D09 T12 §1`  | Shell Command Expansion                       | none  |    12 |
| [x]  | `D09 T12 §2`  | Calculator                                    | none  |     1 |
| [x]  | `D09 T12 §3`  | Image Viewer                                  | none  |     1 |
| [x]  | `D09 T12 §4`  | Screenshot Enhancements                       | none  |     1 |
| [x]  | `D09 T12 §5`  | Archive Manager                               | none  |     1 |
| [x]  | `D09 T12 §6`  | Calendar App                                  | none  |     1 |
| [ ]  | `D09 T12 §7`  | Task Manager                                  | none  |    12 |
| [ ]  | `D09 T12 §8`  | Device Manager                                | none  |     9 |
| [x]  | `D09 T12 §9`  | System Info                                   | none  |     1 |
| [ ]  | `D09 T13 §3`  | Taskbar and desktop integration               | none  |     3 |
| [ ]  | `D09 T14 §1`  | Post-Desktop-Init Test Harness                | none  |     5 |
| [ ]  | `D09 T14 §2`  | Failure Capture Hook and Artifact Bundle      | none  |     7 |
| [ ]  | `D09 T14 §3`  | Committed JSONL Input Traces                  | none  |     6 |
| [ ]  | `D09 T14 §4`  | Snapshot-Time Read-Side Sync                  | none  |     4 |
| [ ]  | `D09 T14 §5`  | Virtio-GPU Multi-Output Driver                | none  |     6 |
| [x]  | `D09 T14 §6`  | ObjectManager FrameStats Pseudo-File          | none  |     6 |
| [/]  | `D09 T14 §7`  | QEMU HMP Monitor UNIX-Socket Hardening        | none  |     5 |
| [/]  | `D09 T14 §8`  | Visual Regression Shared-Session Refactor     | none  |     5 |
| [ ]  | `D08 T18 §20` | App Model and Activation                      | none  |     9 |
| [ ]  | `D08 T18 §21` | Resource and Localization Engine              | none  |     8 |
| [ ]  | `D08 T18 §22` | Shell Process Model and Crash Recovery        | none  |     8 |
| [ ]  | `D08 T18 §23` | Shell Data Stores                             | none  |     8 |
| [ ]  | `D08 T18 §24` | Automation Peers and Inventory Ids            | none  |     8 |
| [ ]  | `D08 T19 §1`  | Control Authoring Contract                    | none  |     6 |
| [ ]  | `D08 T19 §2`  | Button Family                                 | none  |    10 |
| [ ]  | `D08 T19 §3`  | Toggle Switch                                 | none  |     7 |
| [ ]  | `D08 T19 §4`  | Text Box Family                               | none  |     9 |
| [ ]  | `D08 T19 §5`  | Number Box                                    | none  |     7 |
| [ ]  | `D08 T19 §6`  | Auto-Suggest Box                              | none  |     8 |
| [ ]  | `D08 T19 §7`  | Rich Text Display and Hyperlink               | none  |     7 |
| [ ]  | `D08 T19 §8`  | Date, Time and Calendar Pickers               | none  |     9 |
| [ ]  | `D08 T19 §9`  | Colour Picker                                 | none  |     8 |
| [ ]  | `D08 T19 §10` | Progress Ring                                 | none  |     7 |
| [ ]  | `D08 T19 §11` | Rating, Person Picture and Info Badge         | none  |     7 |
| [ ]  | `D08 T19 §12` | Flyout and Menu Flyout                        | none  |     9 |
| [ ]  | `D08 T19 §13` | Command Bar and Command Bar Flyout            | none  |     9 |
| [ ]  | `D08 T19 §14` | Content Dialog and Teaching Tip               | none  |     9 |
| [ ]  | `D08 T19 §15` | Navigation View                               | none  |    10 |
| [ ]  | `D08 T19 §16` | Breadcrumb Bar                                | none  |     8 |
| [ ]  | `D08 T19 §17` | Tab View                                      | none  |     8 |
| [ ]  | `D08 T19 §18` | Expander, Settings Card and Settings Expander | none  |     8 |
| [ ]  | `D08 T19 §19` | Grid View, Items View and Flip View           | none  |     8 |
| [ ]  | `D08 T19 §20` | Split View and Two-Pane View                  | none  |     8 |
| [ ]  | `D08 T19 §21` | Selector Bar and Segmented Control            | none  |     7 |
| [ ]  | `D08 T19 §22` | Title Bar Control                             | none  |     8 |
| [ ]  | `D08 T19 §23` | Media Transport Controls                      | none  |     8 |
| [ ]  | `D08 T19 §24` | Classic Control Parity                        | none  |     8 |
| [ ]  | `D08 T19 §25` | Controls Gallery App                          | none  |     8 |
| [ ]  | `D08 T19 §26` | State Matrix Conformance Suite                | none  |     7 |

### Phase 10 -- Platform services

Audio, updates and packages, restore, accessibility, the Win32 PE loader and API surface, and Linux compatibility need the kernel, storage and the shell in place, and the apps of Phase 11 need them.

| Done | Section       | Deliverable                                                                                                            | Needs | Items |
| :--: | ------------- | ---------------------------------------------------------------------------------------------------------------------- | ----- | ----: |
| [ ]  | `D10 T01 §1`  | Audio Abstraction Layer                                                                                                | none  |     9 |
| [ ]  | `D10 T01 §2`  | Audio Mixer                                                                                                            | none  |    11 |
| [ ]  | `D10 T01 §3`  | WAV Decoder                                                                                                            | none  |     7 |
| [ ]  | `D10 T01 §4`  | MP3 Decoder                                                                                                            | none  |     8 |
| [ ]  | `D10 T01 §5`  | OGG Vorbis Decoder                                                                                                     | none  |     7 |
| [ ]  | `D10 T01 §6`  | FLAC Decoder (Stretch)                                                                                                 | none  |     8 |
| [ ]  | `D10 T01 §7`  | Unified Audio Loader                                                                                                   | none  |     7 |
| [ ]  | `D10 T01 §9`  | Volume Control Popup                                                                                                   | none  |     6 |
| [ ]  | `D10 T02 §1`  | Canvas & Viewport                                                                                                      | none  |     8 |
| [ ]  | `D10 T02 §7`  | Zoom & Resize (Stretch)                                                                                                | none  |     6 |
| [ ]  | `D10 T04 §1`  | Event Log                                                                                                              | none  |     8 |
| [ ]  | `D10 T04 §7`  | F8 Recovery Environment                                                                                                | none  |    10 |
| [ ]  | `D10 T04 §8`  | Factory Reset                                                                                                          | none  |     8 |
| [ ]  | `D10 T04 §9`  | Startup Repair                                                                                                         | none  |     8 |
| [ ]  | `D10 T05 §1`  | Screensaver System                                                                                                     | none  |    12 |
| [ ]  | `D10 T05 §3`  | Desktop Widget Framework                                                                                               | none  |     9 |
| [ ]  | `D10 T06 §1`  | High Contrast Mode                                                                                                     | none  |     7 |
| [ ]  | `D10 T06 §2`  | Large Text / DPI Override                                                                                              | none  |     5 |
| [ ]  | `D10 T06 §3`  | Sticky Keys                                                                                                            | none  |     8 |
| [ ]  | `D10 T06 §6`  | Mouse Keys                                                                                                             | none  |     7 |
| [ ]  | `D10 T06 §7`  | Reduced Motion & Color Blind Mode                                                                                      | none  |     7 |
| [ ]  | `D10 T05 §2`  | Screensaver → Lock Screen                                                                                              | none  |     5 |
| [ ]  | `D10 T07 §1`  | Fix Ring-3 User-Mode Execution                                                                                         | none  |     8 |
| [ ]  | `D10 T07 §2`  | Windows x64 Syscall ABI                                                                                                | none  |     9 |
| [ ]  | `D10 T07 §3`  | PE Header Structures                                                                                                   | none  |     7 |
| [ ]  | `D10 T07 §4`  | PE Loader Core                                                                                                         | none  |     9 |
| [ ]  | `D10 T07 §5`  | Base Relocations                                                                                                       | none  |     7 |
| [ ]  | `D10 T07 §6`  | Import Resolution                                                                                                      | none  |     8 |
| [ ]  | `D10 T07 §7`  | PE Execution                                                                                                           | none  |     8 |
| [ ]  | `D10 T07 §8`  | EXE Format Precedence                                                                                                  | none  |     6 |
| [ ]  | `D10 T07 §9`  | Minimal User-Mode Runtime                                                                                              | none  |     7 |
| [ ]  | `D10 T06 §4`  | Large Cursor                                                                                                           | none  |     6 |
| [ ]  | `D10 T08 §1`  | Win32 Type Definitions                                                                                                 | none  |     3 |
| [ ]  | `D10 T08 §2`  | Console & Process API (`kernel32.dll` tier 1)                                                                          | none  |    22 |
| [ ]  | `D10 T08 §3`  | C Runtime (`msvcrt.dll`)                                                                                               | none  |    13 |
| [ ]  | `D10 T08 §4`  | NT Runtime Stubs (`ntdll.dll`)                                                                                         | none  |     8 |
| [ ]  | `D10 T08 §5`  | Memory Management API                                                                                                  | none  |    13 |
| [ ]  | `D10 T08 §6`  | Synchronization API                                                                                                    | none  |    12 |
| [ ]  | `D10 T08 §7`  | DLL Loading API                                                                                                        | none  |     6 |
| [ ]  | `D10 T08 §8`  | Error API                                                                                                              | none  |     5 |
| [ ]  | `D10 T08 §9`  | Unimplemented Function Logger                                                                                          | none  |     5 |
| [ ]  | `D10 T08 §10` | Window Management (`user32.dll`)                                                                                       | none  |     4 |
| [ ]  | `D10 T08 §11` | GDI Rendering (`gdi32.dll`)                                                                                            | none  |     3 |
| [ ]  | `D10 T08 §12` | Shell & Icon API (`shell32.dll`)                                                                                       | none  |     3 |
| [ ]  | `D10 T08 §13` | IxUI Native Toolkit                                                                                                    | none  |     6 |
| [ ]  | `D10 T08 §14` | Win32 Shell Integration                                                                                                | none  |     4 |
| [ ]  | `D09 T13 §4`  | ShellExecute open-verb wiring                                                                                          | none  |     2 |
| [ ]  | `D10 T08 §15` | Developer SDK                                                                                                          | none  |     9 |
| [ ]  | `D10 T09 §1`  | SDK C Headers                                                                                                          | none  |    15 |
| [ ]  | `D10 T09 §2`  | SDK GUI Headers                                                                                                        | none  |    11 |
| [ ]  | `D10 T09 §3`  | SDK Import Libraries                                                                                                   | none  |     9 |
| [ ]  | `D10 T09 §4`  | Cross-Compile TCC for Impossible OS                                                                                    | none  |    11 |
| [ ]  | `D10 T09 §5`  | Install TCC on OS Disk Image                                                                                           | none  |    10 |
| [ ]  | `D10 T09 §6`  | TCC Self-Hosting Tests                                                                                                 | none  |     9 |
| [ ]  | `D10 T09 §7`  | TCC IxUI Integration                                                                                                   | none  |     7 |
| [ ]  | `D10 T09 §8`  | Shell Compiler Integration                                                                                             | none  |     7 |
| [ ]  | `D10 T09 §9`  | SDK Installer + On-OS Pre-Install                                                                                      | none  |     7 |
| [ ]  | `D10 T09 §10` | SDK Documentation                                                                                                      | none  |     6 |
| [ ]  | `D10 T09 §11` | GCC/Clang (Long-Term C++ Support)                                                                                      | none  |    10 |
| [ ]  | `D10 T04 §6`  | Crash Dump Viewer                                                                                                      | none  |    11 |
| [ ]  | `D10 TA §1`   | Tier 1 rows below kept in sync with Section 10 / T14 implementation                                                    | none  |     0 |
| [ ]  | `D10 TA §2`   | Tier 2 rows below kept in sync with TODO-14 Sections 2 through 8                                                       | none  |     0 |
| [ ]  | `D10 TA §3`   | Run PE export dump on pinned Win11 `user32.dll`; file deltas vs Wine scaffold in Notes column (new symbols, renames... | none  |     0 |
| [ ]  | `D10 TA §4`   | Replace `NO_OWNING_TODO` owners in Tier 3 as `TODO-08` / `TODO-14` / `TODO-05` subsections are opened                  | none  |     0 |
| [ ]  | `D09 T13 §1`  | Binary layout and boot                                                                                                 | none  |     4 |
| [ ]  | `D10 TB §1`   | Tier 1 rows kept in sync with PE loader + `TODO-08` when `comctl32` is wired                                           | none  |     0 |
| [ ]  | `D10 TB §2`   | Tier 2 roadmap bullets updated when `TODO-06` / `TODO-14` gain matching control APIs                                   | none  |     0 |
| [ ]  | `D10 TB §3`   | Dump exports from pinned Win11 `comctl32.dll`; merge deltas into Tier 3 Notes (new names, forwards, ordinals)          | none  |     0 |
| [ ]  | `D10 TB §4`   | Replace `NO_OWNING_TODO` in Tier 3 as owning TODO sections are opened                                                  | none  |     0 |
| [ ]  | `D10 TC §1`   | Tier 1 rows kept in sync with PE loader + `TODO-08` when `shell32` is wired                                            | none  |     0 |
| [ ]  | `D10 TC §2`   | Tier 2 roadmap bullets updated when Explorer / compat matrix expands                                                   | none  |     0 |
| [ ]  | `D10 TC §3`   | Dump exports from pinned Win11 `shell32.dll`; merge deltas into Tier 3 Notes (new names, forwards, ordinals)           | none  |     0 |
| [ ]  | `D10 TC §4`   | Replace `NO_OWNING_TODO` in Tier 3 as owning TODO sections are opened                                                  | none  |     0 |
| [ ]  | `D09 T13 §2`  | Shell32 dependency gate                                                                                                | none  |     2 |
| [ ]  | `D10 T10 §1`  | `task_t` Linux Flags + FD Table                                                                                        | none  |    10 |
| [ ]  | `D10 T10 §2`  | ELF Linux Loader (SYSV Stack + auxv)                                                                                   | none  |     6 |
| [ ]  | `D10 T10 §3`  | Syscall Entry Dispatch (`is_linux_elf` Branch)                                                                         | none  |     4 |
| [ ]  | `D10 T10 §4`  | Linux Syscall Translation Table                                                                                        | none  |    41 |
| [ ]  | `D10 T10 §5`  | Path Translation                                                                                                       | none  |     3 |
| [ ]  | `D10 T10 §6`  | Linux File Descriptor Table                                                                                            | none  |     6 |
| [ ]  | `D10 T10 §7`  | POSIX Filesystem Stubs                                                                                                 | none  |     7 |
| [ ]  | `D10 T10 §8`  | Signal Stubs                                                                                                           | none  |     9 |
| [ ]  | `D10 T10 §9`  | Shell ELF Integration + `[Linux]` Process Tag                                                                          | none  |     5 |
| [ ]  | `D10 T10 §10` | Test: Static Hello + Busybox                                                                                           | none  |    11 |
| [ ]  | `D10 T10 §11` | Future: Dynamic ELF (Stretch)                                                                                          | none  |     0 |
| [ ]  | `D10 T11 §1`  | ISO Build Script                                                                                                       | none  |     7 |
| [ ]  | `D10 T11 §2`  | Installer Init Process                                                                                                 | none  |     6 |
| [ ]  | `D09 T13 §5`  | Boot-time explorer default                                                                                             | none  |     2 |
| [ ]  | `D10 T11 §3`  | Installer GUI Wizard                                                                                                   | none  |    24 |
| [ ]  | `D10 T11 §4`  | GPT Partition Write                                                                                                    | none  |     7 |
| [ ]  | `D10 T11 §5`  | Partition Format (ESP FAT32 + IXFS System)                                                                             | none  |     7 |
| [ ]  | `D10 T05 §5`  | Display Management                                                                                                     | none  |     9 |
| [ ]  | `D10 T05 §6`  | Multi-Monitor Stubs                                                                                                    | none  |     6 |
| [ ]  | `D10 T11 §6`  | File Copy Engine                                                                                                       | none  |     6 |
| [ ]  | `D10 T11 §7`  | UEFI Bootloader Install + NVRAM Boot Entry                                                                             | none  |     6 |
| [ ]  | `D10 T11 §8`  | Post-Install First Boot + OOBE Trigger                                                                                 | none  |     6 |
| [ ]  | `D10 T11 §9`  | Validation: QEMU + Hyper-V Gen2 + VirtualBox                                                                           | none  |    13 |
| [ ]  | `D10 T11 §10` | Stability + Performance Pass                                                                                           | none  |     6 |
| [ ]  | `D10 T01 §10` | `mmsys.cpl` -- Sound Settings Applet                                                                                   | none  |     9 |
| [ ]  | `D10 T12 §1`  | Kernel Debugger (GDB RSP Stub)                                                                                         | none  |     9 |
| [ ]  | `D10 T05 §4`  | Built-in Widgets                                                                                                       | none  |     8 |
| [ ]  | `D10 T01 §8`  | Media Player App                                                                                                       | none  |    10 |
| [ ]  | `D10 T02 §3`  | Color System                                                                                                           | none  |     8 |
| [ ]  | `D10 T02 §2`  | Drawing Tools                                                                                                          | none  |    11 |
| [ ]  | `D10 T02 §4`  | Undo/Redo                                                                                                              | none  |    10 |
| [ ]  | `D10 T02 §5`  | File Operations                                                                                                        | none  |    10 |
| [ ]  | `D10 T02 §6`  | Selection Tool (Stretch)                                                                                               | none  |     8 |
| [ ]  | `D10 T06 §5`  | Screen Magnifier                                                                                                       | none  |     8 |
| [ ]  | `D10 T06 §8`  | `ease.cpl` -- Accessibility Settings Applet                                                                            | none  |    11 |
| [ ]  | `D10 T12 §2`  | User-Mode Debugger (`debugger.exe`)                                                                                    | none  |    21 |
| [ ]  | `D10 T12 §3`  | Developer Tools                                                                                                        | none  |     8 |
| [ ]  | `D10 T12 §4`  | Touch Input + Gesture Recognizer                                                                                       | none  |     7 |
| [ ]  | `D10 T12 §5`  | Gamepad / Controller Input + XInput Stubs                                                                              | none  |    10 |
| [ ]  | `D10 T12 §6`  | Print Support                                                                                                          | none  |     6 |
| [ ]  | `D10 T12 §7`  | Text-to-Speech (SAM → eSpeak-NG)                                                                                       | none  |     7 |
| [ ]  | `D10 T12 §8`  | Software OpenGL (TinyGL Port)                                                                                          | none  |    10 |
| [ ]  | `D10 T12 §9`  | Multi-User Session Management + Fast Switching                                                                         | none  |     8 |
| [ ]  | `D10 T12 §10` | Telemetry (Opt-In, Anonymous)                                                                                          | none  |     5 |
| [ ]  | `D10 T12 §11` | Parental Controls (`parcon.cpl`)                                                                                       | none  |    18 |
| [ ]  | `D10 T03 §1`  | Update Check API                                                                                                       | none  |     7 |
| [ ]  | `D10 T03 §2`  | Update Download & Verification                                                                                         | none  |     6 |
| [ ]  | `D10 T03 §5`  | IPKG Package Format                                                                                                    | none  |     7 |
| [ ]  | `D10 T03 §6`  | App Installer Wizard                                                                                                   | none  |    10 |
| [ ]  | `D10 T03 §7`  | App Uninstaller                                                                                                        | none  |     9 |
| [ ]  | `D10 T03 §8`  | `appwiz.cpl` -- Programs & Features Applet                                                                             | none  |     8 |
| [ ]  | `D10 T03 §9`  | IPKG Build Tool (Host)                                                                                                 | none  |     7 |
| [ ]  | `D10 T04 §2`  | Restore Point Creation                                                                                                 | none  |     8 |
| [ ]  | `D10 T03 §3`  | Update Application                                                                                                     | none  |    10 |
| [ ]  | `D10 T03 §4`  | `wuapp.cpl` -- Windows Update Applet                                                                                   | none  |    10 |
| [ ]  | `D10 T04 §5`  | First-Boot Setup Wizard                                                                                                | none  |     9 |
| [ ]  | `D10 T04 §3`  | System Rollback                                                                                                        | none  |     7 |
| [ ]  | `D10 T04 §4`  | `rstrui.cpl` -- System Restore UI                                                                                      | none  |     8 |
| [ ]  | `D10 T04 §10` | Disk Cleanup Utility                                                                                                   | none  |     9 |

### Phase 11 -- Apps

The bundled applications are the proof that the platform beneath them is complete, so they follow the services they call.

| Done | Section       | Deliverable                                        | Needs       | Items |
| :--: | ------------- | -------------------------------------------------- | ----------- | ----: |
| [ ]  | `D11 T01 §1`  | Text-Only Browser (Phase 1)                        | none        |    12 |
| [ ]  | `D11 T01 §2`  | HTML Tokenizer + DOM Tree                          | none        |     5 |
| [ ]  | `D11 T01 §3`  | Layout Engine (Block + Inline Flow)                | none        |    12 |
| [ ]  | `D11 T01 §4`  | CSS Parser + Cascade (Stretch)                     | none        |     8 |
| [ ]  | `D11 T01 §5`  | JavaScript Engine (Long-Term Stretch)              | none        |     6 |
| [ ]  | `D11 T01 §6`  | Alternative: NetSurf / Dillo Port Evaluation       | none        |     5 |
| [ ]  | `D11 T01 §7`  | Browser Chrome                                     | none        |     8 |
| [ ]  | `D11 T01 §8`  | Browser Settings + History + Cookie Jar            | none        |     7 |
| [ ]  | `D11 T02 §1`  | FTP Protocol Core                                  | none        |    11 |
| [ ]  | `D11 T02 §2`  | FTP Shell Commands + `wget ftp://`                 | none        |     4 |
| [ ]  | `D11 T02 §3`  | `wget` Command (HTTP/HTTPS + Progress)             | none        |     9 |
| [ ]  | `D11 T02 §4`  | `curl` Command (Full Flag Set)                     | none        |    15 |
| [ ]  | `D11 T02 §5`  | FTP GUI Client (Dual-Pane, Stretch)                | none        |     9 |
| [ ]  | `D11 T02 §6`  | WiFi Framework (Stretch)                           | none        |     8 |
| [ ]  | `D11 T05 §1`  | PDF Structure Parser                               | none        |     1 |
| [ ]  | `D11 T05 §2`  | Page Tree Traversal                                | none        |     1 |
| [ ]  | `D11 T05 §3`  | Stream Decompression                               | none        |     1 |
| [ ]  | `D11 T05 §4`  | Content Stream Renderer                            | none        |     1 |
| [ ]  | `D11 T05 §5`  | Image Rendering                                    | none        |     1 |
| [ ]  | `D11 T05 §6`  | PDF Viewer UI                                      | none        |     1 |
| [ ]  | `D11 T05 §7`  | Text Search                                        | none        |     1 |
| [ ]  | `D11 T05 §8`  | Continuous Scroll View (Stretch)                   | none        |     1 |
| [ ]  | `D11 T05 §9`  | File Association                                   | none        |     7 |
| [ ]  | `D11 T05 §10` | Recto Source Import and Provenance                 | none        |     6 |
| [ ]  | `D11 T05 §11` | C++23 User-Mode Runtime                            | none        |     7 |
| [ ]  | `D11 T05 §12` | Recto Platform Seam                                | third-party |     5 |
| [ ]  | `D11 T05 §13` | Recto PDF Engine on Impossible OS                  | third-party |     6 |
| [ ]  | `D11 T05 §14` | Recto UI Backend on Impossible OS Engines          | third-party |     7 |
| [ ]  | `D11 T05 §15` | Recto App on Impossible OS                         | third-party |     7 |
| [ ]  | `D11 T05 §16` | OCR Provider                                       | third-party |     5 |
| [ ]  | `D11 T05 §17` | Convert and Organize Features                      | third-party |     5 |
| [ ]  | `D11 T05 §18` | System Integration                                 | none        |     6 |
| [ ]  | `D11 T05 §19` | Packaging and Updates                              | none        |     5 |
| [ ]  | `D11 T05 §20` | Recto Conformance on Impossible OS                 | none        |     4 |
| [ ]  | `D11 T06 §1`  | pl_mpeg Integration                                | none        |    10 |
| [ ]  | `D11 T06 §2`  | YCbCr → RGB Conversion                             | none        |     3 |
| [ ]  | `D11 T06 §3`  | A/V Sync Engine                                    | none        |     7 |
| [ ]  | `D11 T06 §4`  | Player UI                                          | none        |     5 |
| [ ]  | `D11 T06 §5`  | File Operations                                    | none        |     6 |
| [ ]  | `D11 T06 §6`  | Playlist                                           | none        |     7 |
| [ ]  | `D11 T06 §7`  | Subtitle Support (Stretch)                         | none        |     5 |
| [ ]  | `D11 T06 §8`  | Format Support Roadmap                             | none        |     4 |
| [ ]  | `D11 T07 §4`  | `ping` + `traceroute` Commands                     | none        |     3 |
| [ ]  | `D11 T07 §5`  | Network Diagnostic Tools: `arp -a` + `route print` | none        |     5 |
| [ ]  | `D11 T08 §6`  | Gap Buffer                                         | none        |    16 |
| [ ]  | `D11 T08 §7`  | Text Rendering & Cursor                            | none        |    11 |
| [ ]  | `D11 T08 §8`  | Undo/Redo                                          | none        |    10 |
| [ ]  | `D11 T08 §9`  | File Menu                                          | none        |     9 |
| [ ]  | `D11 T08 §10` | Editing Features                                   | none        |    10 |
| [ ]  | `D11 T08 §11` | Find & Replace                                     | none        |    10 |
| [ ]  | `D11 T08 §12` | Stretch Features                                   | none        |    10 |
| [ ]  | `D11 T08 §1`  | Gap Buffer Text Engine                             | none        |     8 |
| [ ]  | `D11 T08 §2`  | Text Rendering + Cursor                            | none        |     7 |
| [ ]  | `D11 T08 §3`  | File Operations                                    | none        |     7 |
| [ ]  | `D11 T08 §4`  | Editing Features                                   | none        |     9 |
| [ ]  | `D11 T08 §5`  | File Associations + App Registration               | none        |     5 |
| [ ]  | `D11 T09 §6`  | Calculator                                         | none        |    12 |
| [ ]  | `D11 T09 §1`  | Standard Calculator UI                             | none        |     7 |
| [ ]  | `D11 T09 §2`  | Arithmetic Engine                                  | none        |     7 |
| [ ]  | `D11 T09 §3`  | Memory + History                                   | none        |     3 |
| [ ]  | `D11 T09 §4`  | Scientific Mode                                    | none        |    14 |
| [ ]  | `D11 T09 §5`  | Programmer Mode                                    | none        |     8 |
| [ ]  | `D11 T10 §1`  | Rich Text Document Model                           | none        |    11 |
| [ ]  | `D11 T10 §2`  | RTF File Format                                    | none        |     3 |
| [ ]  | `D11 T10 §3`  | Rich Text Rendering + Layout Engine                | none        |     8 |
| [ ]  | `D11 T10 §4`  | Toolbar + Menus                                    | none        |     3 |
| [ ]  | `D11 T10 §5`  | Formatting Interactions                            | none        |     9 |
| [ ]  | `D11 T10 §6`  | File Operations + File Associations                | none        |     9 |
| [ ]  | `D11 T10 §7`  | Print (Stretch)                                    | none        |     5 |
| [ ]  | `D11 T11 §9`  | Image Viewer                                       | none        |    11 |
| [ ]  | `D11 T11 §1`  | Image Loading + Display                            | none        |    10 |
| [ ]  | `D11 T11 §2`  | Zoom + Pan                                         | none        |     8 |
| [ ]  | `D11 T11 §3`  | Folder Navigation + Thumbnail Strip                | none        |     4 |
| [ ]  | `D11 T11 §4`  | Toolbar + Menus                                    | none        |     3 |
| [ ]  | `D11 T11 §5`  | Slideshow                                          | none        |     5 |
| [ ]  | `D11 T11 §6`  | EXIF Metadata Panel                                | none        |     3 |
| [ ]  | `D11 T11 §7`  | Basic Editing                                      | none        |     6 |
| [ ]  | `D11 T11 §8`  | File Associations + App Registration               | none        |     4 |
| [ ]  | `D11 T12 §6`  | Screenshot Enhancements                            | none        |     9 |
| [ ]  | `D11 T12 §7`  | Archive Manager                                    | none        |     9 |
| [ ]  | `D11 T12 §1`  | Full + Window Capture + Hotkeys                    | none        |     6 |
| [ ]  | `D11 T12 §2`  | Region Select Overlay                              | none        |     8 |
| [ ]  | `D11 T12 §3`  | Snipping Tool UI                                   | none        |     4 |
| [ ]  | `D11 T13 §8`  | Calendar App                                       | none        |     9 |
| [ ]  | `D11 T13 §9`  | System Info                                        | none        |     8 |
| [ ]  | `D11 T13 §10` | Font Manager App                                   | none        |     8 |
| [ ]  | `D11 T13 §1`  | Calendar App (Recurring Events + ICS Export)       | none        |     3 |
| [ ]  | `D11 T13 §2`  | Sticky Notes                                       | none        |     7 |
| [ ]  | `D11 T13 §3`  | System Information (`sysinfo.exe`)                 | none        |     3 |
| [ ]  | `D11 T13 §4`  | On-Screen Keyboard                                 | none        |     8 |
| [ ]  | `D11 T13 §5`  | Color Picker                                       | none        |     6 |
| [ ]  | `D11 T13 §6`  | Font Manager (OS/2 + Unicode Coverage)             | none        |     4 |
| [ ]  | `D11 T13 §7`  | Shared Help / About Dialog                         | none        |     3 |
| [ ]  | `D11 T03 §1`  | SSH2 Transport Layer                               | none        |    10 |
| [ ]  | `D11 T03 §2`  | SSH Authentication                                 | none        |     4 |
| [ ]  | `D11 T03 §3`  | Interactive Channel + PTY Relay                    | none        |     5 |
| [ ]  | `D11 T03 §4`  | Shell Integration + TOFU + Known Hosts             | none        |     6 |
| [ ]  | `D11 T03 §5`  | SCP File Transfer                                  | none        |     8 |
| [ ]  | `D11 T03 §6`  | SSH Config File                                    | none        |     7 |
| [ ]  | `D11 T03 §7`  | sftp Command (Stretch)                             | none        |     5 |
| [ ]  | `D11 T04 §1`  | SMTP Client                                        | none        |     6 |
| [ ]  | `D11 T04 §2`  | POP3 Client + Message Storage                      | none        |     7 |
| [ ]  | `D11 T04 §3`  | IMAP Client (Stretch)                              | none        |     9 |
| [ ]  | `D11 T04 §4`  | Three-Panel Email GUI                              | none        |     6 |
| [ ]  | `D11 T04 §7`  | Auto-Check + Notifications + Tray                  | none        |     5 |
| [ ]  | `D11 T04 §9`  | Spam & Junk Filter                                 | none        |     9 |
| [ ]  | `D11 T07 §1`  | VNC Client                                         | none        |    10 |
| [ ]  | `D11 T07 §2`  | IRC / Chat Client                                  | none        |     9 |
| [ ]  | `D11 T07 §3`  | RSS / News Reader                                  | none        |    10 |
| [ ]  | `D11 T07 §6`  | VNC Server (Stretch)                               | none        |     9 |
| [ ]  | `D11 T04 §6`  | Account Management + Credential Store              | none        |     6 |
| [ ]  | `D11 T04 §8`  | Contacts Store                                     | none        |     9 |
| [ ]  | `D11 T04 §5`  | Compose Window                                     | none        |     8 |

### Phase 12 -- User platform and SDK

The kernel libraries, the process ABI, ntdll, the Win32 subsystem in user mode, the SDK and the compatibility matrix turn the system into one that runs other people's programs. They need nearly everything before them.

| Done | Section       | Deliverable                                                                 | Needs | Items |
| :--: | ------------- | --------------------------------------------------------------------------- | ----- | ----: |
| [ ]  | `D12 T01 §1`  | String Library Consolidation                                                | none  |     8 |
| [ ]  | `D12 T01 §2`  | Math Library Extension                                                      | none  |     4 |
| [ ]  | `D12 T01 §4`  | monocypher + CSPRNG + SYS_GETRANDOM                                         | none  |     5 |
| [ ]  | `D11 T12 §4`  | Archive Manager                                                             | none  |    10 |
| [ ]  | `D11 T12 §5`  | ZIP Shell Integration                                                       | none  |     6 |
| [ ]  | `D12 T01 §3`  | miniz (ZIP Writer + Stream Extension)                                       | none  |     4 |
| [ ]  | `D12 T01 §5`  | cJSON                                                                       | none  |     4 |
| [ ]  | `D12 T01 §6`  | Mbed TLS Subset                                                             | none  |     5 |
| [ ]  | `D12 T01 §7`  | STB Consolidation + `stb_image_write`                                       | none  |     5 |
| [ ]  | `D12 T01 §8`  | Library Integration Tests + README                                          | none  |     2 |
| [ ]  | `D12 T02 §1`  | Per-Process Environ Storage (SDK Contract)                                  | none  |     4 |
| [ ]  | `D12 T02 §2`  | System Default Variables                                                    | none  |     2 |
| [ ]  | `D12 T02 §3`  | `%VAR%` Expansion + `env_expand_path`                                       | none  |     4 |
| [ ]  | `D12 T02 §4`  | SYS_GETENV / SYS_SETENV / SYS_UNSETENV                                      | none  |     6 |
| [ ]  | `D12 T02 §5`  | PATH-Based Command Lookup + Session Cache                                   | none  |     5 |
| [ ]  | `D12 T02 §8`  | `set` / `echo` / `env` / `where` Shell Commands                             | none  |     9 |
| [ ]  | `D12 T03 §1`  | ELF Relocation Engine                                                       | none  |     5 |
| [ ]  | `D12 T03 §2`  | Kernel Module Format `.kmod`                                                | none  |     5 |
| [ ]  | `D12 T03 §3`  | Kernel Module Loader                                                        | none  |     6 |
| [ ]  | `D12 T03 §4`  | Kernel Symbol Export (`EXPORT_SYMBOL`)                                      | none  |     5 |
| [ ]  | `D12 T03 §7`  | Module Hot-Swap (Stretch)                                                   | none  |     3 |
| [ ]  | `D12 T03 §8`  | `lsmod` / `insmod` / `rmmod` / `modprobe` Shell Commands                    | none  |     5 |
| [ ]  | `D12 T05 §1`  | Win32 Subsystem Architecture + Message Queue                                | none  |     8 |
| [ ]  | `D12 T05 §8`  | Common Dialog Boxes                                                         | none  |     4 |
| [ ]  | `D12 T06 §1`  | SDK Packaging (`make sdk`)                                                  | none  |     5 |
| [ ]  | `D12 T06 §2`  | SDK Documentation (`gendoc.c`)                                              | none  |     5 |
| [ ]  | `D12 T06 §3`  | Code Samples                                                                | none  |     8 |
| [ ]  | `D12 T06 §4`  | Sampling Profiler                                                           | none  |     6 |
| [ ]  | `D12 T06 §5`  | Unit Test Framework (`itest.h`)                                             | none  |     5 |
| [ ]  | `D12 T06 §6`  | Debugger Enhancements                                                       | none  |     5 |
| [ ]  | `D12 T06 §7`  | IxUI Starter Templates (`ixui-new`)                                         | none  |     6 |
| [ ]  | `D12 T06 §8`  | SDK Release Pipeline                                                        | none  |     5 |
| [ ]  | `D12 T07 §1`  | API Coverage Tracker                                                        | none  |     4 |
| [ ]  | `D12 T07 §2`  | Bring-Up Ladder Framework                                                   | none  |     4 |
| [ ]  | `D12 T07 §3`  | Tier 1 -- Process Exit Only                                                 | none  |     4 |
| [ ]  | `D12 T07 §4`  | Tier 2 -- Console I/O                                                       | none  |     4 |
| [ ]  | `D12 T07 §6`  | Tier 4 -- Process & Registry                                                | none  |     4 |
| [ ]  | `D12 T07 §7`  | Tier 5 -- Memory & Sync                                                     | none  |     4 |
| [ ]  | `D12 T07 §5`  | Tier 3 -- File I/O                                                          | none  |     4 |
| [ ]  | `D12 T07 §10` | Tier 8 -- Extended Win32 Surface                                            | none  |     4 |
| [ ]  | `D12 T07 §11` | Stub Call Log Analysis                                                      | none  |     3 |
| [ ]  | `D12 T03 §6`  | GOT / PLT Lazy Binding                                                      | none  |     4 |
| [ ]  | `D12 T03 §5`  | `dlopen` / `dlsym` for Linux Compat                                         | none  |     5 |
| [ ]  | `D12 T04 §1`  | ntdll.dll Structure + TEB / PEB Extensions                                  | none  |     6 |
| [ ]  | `D12 T02 §6`  | argv / argc Process ABI + `cmd_tokenize`                                    | none  |     4 |
| [ ]  | `D12 T02 §7`  | Shell `.profile` Startup Script                                             | none  |     5 |
| [ ]  | `D12 T04 §2`  | RtlHeap Process Heap Allocator                                              | none  |     7 |
| [ ]  | `D12 T04 §3`  | LdrLoadDll (PE DLL Loader)                                                  | none  |     7 |
| [ ]  | `D12 T04 §4`  | Thread-Local Storage (TLS)                                                  | none  |     5 |
| [ ]  | `D12 T04 §5`  | Vectored Exception Handling (VEH)                                           | none  |    16 |
| [ ]  | `D12 T04 §6`  | Process Startup (CRT0)                                                      | none  |     6 |
| [ ]  | `D12 T04 §7`  | User-Mode libc Shims                                                        | none  |    17 |
| [ ]  | `D12 T04 §8`  | Fiber API                                                                   | none  |     5 |
| [ ]  | `D12 T04 §9`  | Local Atom Tables (RtlAtomTable)                                            | none  |     5 |
| [ ]  | `D12 T04 §10` | WNF User Runtime (RtlPublish / RtlSubscribe)                                | none  |     7 |
| [ ]  | `D12 T05 §2`  | Window Class Registry + Built-in Classes                                    | none  |     8 |
| [ ]  | `D12 T05 §3`  | WndProc Dispatch + DefWindowProc                                            | none  |     6 |
| [ ]  | `D12 T05 §4`  | Win32 Painting Model (HDC + Dirty Rect)                                     | none  |     9 |
| [ ]  | `D12 T05 §5`  | Message Filters + Accelerator Tables                                        | none  |     4 |
| [ ]  | `D12 T05 §6`  | Window Subclassing + Property Store                                         | none  |     4 |
| [ ]  | `D12 T05 §7`  | Inter-Process Window Messaging                                              | none  |     6 |
| [ ]  | `D12 T05 §9`  | Win11 Visual Opt-In Surface: `dwmapi`, `uxtheme`, Personalization Broadcast | none  |     6 |
| [ ]  | `D12 T07 §8`  | Tier 6 -- MessageBox & Basic GUI                                            | none  |     3 |
| [ ]  | `D12 T07 §9`  | Tier 7 -- Full Win32 Window + Controls                                      | none  |     3 |
| [ ]  | `D12 T07 §12` | CI Compatibility Gate                                                       | none  |     4 |
| [ ]  | `D12 T07 §13` | Tier 9 -- WinRT + DirectX + WinUI 3 / Windows App SDK Apps                  | none  |     5 |

### Phase 13 -- Tools and accessories

The object namespace browser and the event viewer: small diagnostic programs that depend on the SDK surface and on nothing after it.

| Done | Section      | Deliverable                  | Needs | Items |
| :--: | ------------ | ---------------------------- | ----- | ----: |
| [ ]  | `D13 T01 §1` | User-Mode Syscall Wrappers   | none  |     4 |
| [ ]  | `D13 T01 §2` | Console-Mode ObBrowse        | none  |     5 |
| [ ]  | `D13 T01 §3` | GUI ObBrowse with Tree View  | none  |     4 |
| [ ]  | `D13 T01 §4` | Object Detail and Type Icons | none  |     4 |
| [ ]  | `D13 T02 §1` | Console-Mode Log Viewer      | none  |     7 |
| [ ]  | `D13 T02 §2` | GUI Event Viewer             | none  |    10 |
| [ ]  | `D13 T02 §3` | Live Tail and Auto-Refresh   | none  |     4 |

### Phase 14 -- Host tools

Tools that run on the developer's machine: the SDK build system, filesystem mount and repair, crash decoding and log analysis. They are placed here because they inspect and package what the earlier phases produce.

| Done | Section      | Deliverable                                            | Needs | Items |
| :--: | ------------ | ------------------------------------------------------ | ----- | ----: |
| [/]  | `D14 T01 §1` | Linux Build Script                                     | none  |    10 |
| [/]  | `D14 T01 §2` | Dependency Detection and Reporting                     | none  |     4 |
| [/]  | `D14 T01 §3` | Auto-Discovery of SDK Tool Directories                 | none  |     5 |
| [/]  | `D14 T02 §1` | Shared IXFS Core Parser                                | none  |     8 |
| [/]  | `D14 T02 §2` | Disk I/O Layer with GPT                                | none  |     7 |
| [/]  | `D14 T02 §3` | Linux FUSE Mount (Read-Only)                           | none  |     8 |
| [/]  | `D14 T02 §4` | Write Support                                          | none  |     8 |
| [/]  | `D14 T02 §5` | USB Auto-Mount Script                                  | none  |     6 |
| [ ]  | `D14 T02 §6` | Resync the SDK Parser with the IXFS v2 On-Disk Format  | none  |     7 |
| [ ]  | `D14 T01 §4` | SDK Tools in CI and Honest Skip Reporting              | none  |     3 |
| [ ]  | `D14 T03 §1` | Symbol Map Parser                                      | none  |     6 |
| [ ]  | `D14 T03 §2` | Source Context Display                                 | none  |     5 |
| [ ]  | `D14 T03 §3` | Error Code Decoder                                     | none  |     5 |
| [ ]  | `D14 T03 §4` | Memory Region Mapper                                   | none  |     4 |
| [ ]  | `D14 T03 §5` | Full Crash Decode Mode                                 | none  |     5 |
| [ ]  | `D14 T04 §1` | Serial Log Parser                                      | none  |     5 |
| [ ]  | `D14 T04 §2` | Register Dump Decoder                                  | none  |     6 |
| [ ]  | `D14 T04 §3` | Timeline Extraction                                    | none  |     4 |
| [ ]  | `D14 T04 §4` | Root Cause Hypothesis Engine                           | none  |     6 |
| [ ]  | `D14 T05 §1` | Log Parser                                             | none  |     5 |
| [ ]  | `D14 T05 §2` | Timing Analysis                                        | none  |     4 |
| [ ]  | `D14 T05 §3` | Warning/Error Highlighter                              | none  |     4 |
| [ ]  | `D14 T05 §4` | Boot Comparison                                        | none  |     6 |
| [ ]  | `D14 T05 §5` | HTML Report                                            | none  |     3 |
| [ ]  | `D14 T06 §1` | GPT Partition Table Parser                             | none  |     5 |
| [ ]  | `D14 T06 §2` | IXFS Superblock + Inode Inspector                      | none  |     4 |
| [ ]  | `D14 T06 §3` | Directory Listing                                      | none  |     4 |
| [ ]  | `D14 T06 §4` | Hex Dump and Raw Sector Read                           | none  |     4 |
| [ ]  | `D14 T06 §5` | Interactive Shell                                      | none  |     4 |
| [ ]  | `D14 T07 §1` | Superblock Validation                                  | none  |     6 |
| [ ]  | `D14 T07 §2` | Block Bitmap Verification                              | none  |     3 |
| [ ]  | `D14 T07 §3` | Inode Table Scan                                       | none  |     4 |
| [ ]  | `D14 T07 §4` | Extent Integrity Check                                 | none  |     4 |
| [ ]  | `D14 T07 §5` | Directory Structure + Orphan Detection                 | none  |     5 |
| [ ]  | `D14 T07 §6` | Cross-Reference                                        | none  |     3 |
| [ ]  | `D14 T07 §7` | Repair Mode                                            | none  |     5 |
| [ ]  | `D14 T08 §1` | GPT Parser -- Find BlackBox Partition by Name          | none  |     8 |
| [ ]  | `D14 T08 §2` | FAT32 Reader -- Traverse Dirs, Read Files              | none  |     7 |
| [ ]  | `D14 T08 §3` | `blackbox extract` -- Dump All Logs to Local Directory | none  |    10 |
| [ ]  | `D14 T08 §4` | `blackbox cat` -- View a Specific Log File             | none  |     6 |
| [ ]  | `D14 T08 §5` | `blackbox events` -- Parse and Filter events.jsonl     | none  |    11 |
| [ ]  | `D14 T08 §6` | `blackbox boot` -- Show Boot Timeline from JSON        | none  |     6 |
| [ ]  | `D14 T08 §7` | `blackbox verify` -- HMAC Chain Verification           | none  |     6 |
| [ ]  | `D14 T08 §8` | Windows Build                                          | none  |     5 |
| [ ]  | `D14 T08 §9` | Shell Wrapper Scripts for Claude Code Integration      | none  |     5 |

### Phase 15 -- Installer and release

The installer, the release pipeline, hosting and the status page run when there is a complete system to install and ship.

| Done | Section      | Deliverable                                    | Needs | Items |
| :--: | ------------ | ---------------------------------------------- | ----- | ----: |
| [ ]  | `D15 T01 §1` | Versioning Scheme                              | none  |     7 |
| [ ]  | `D15 T01 §2` | GPT Disk Image Release                         | none  |     5 |
| [ ]  | `D15 T01 §3` | USB-Bootable Image                             | none  |     3 |
| [ ]  | `D15 T01 §4` | Bootable ISO (Joliet + Rock Ridge)             | none  |     5 |
| [ ]  | `D15 T01 §5` | Code Signing                                   | none  |     5 |
| [ ]  | `D15 T01 §7` | VM Image Variants                              | none  |     4 |
| [ ]  | `D15 T01 §9` | Third-Party Attribution Shipped With Artifacts | none  |     8 |
| [ ]  | `D15 T02 §1` | Answer File Format + INI Parser                | none  |     4 |
| [ ]  | `D15 T02 §2` | Unattended Install Mode                        | none  |     4 |
| [ ]  | `D15 T02 §3` | `sysprep.exe` (Generalize + OOBE Seal)         | none  |     6 |
| [ ]  | `D15 T02 §4` | OEM Customization                              | none  |     3 |
| [ ]  | `D15 T02 §5` | WIM / Image Capture (`imagex.exe`)             | none  |     5 |
| [ ]  | `D15 T02 §6` | Network Boot (PXE + TFTP Server)               | none  |     6 |
| [ ]  | `D15 T02 §7` | VM Provisioning Templates                      | none  |     4 |
| [ ]  | `D15 T02 §8` | Enterprise Deployment Guide                    | none  |     3 |
| [ ]  | `D15 T03 §1` | Version Manifest API                           | none  |     7 |
| [ ]  | `D15 T03 §4` | IPKG Package Repository                        | none  |     4 |
| [ ]  | `D15 T03 §5` | Package Submission                             | none  |     4 |
| [ ]  | `D15 T03 §6` | Update Delta Packages                          | none  |     3 |
| [ ]  | `D15 T03 §7` | Telemetry Pipeline                             | none  |     3 |
| [ ]  | `D15 T04 §1` | Automated Regression Test Suite                | none  |     4 |
| [ ]  | `D15 T04 §2` | QEMU Validation                                | none  |     3 |
| [ ]  | `D15 T04 §3` | Hyper-V Certification                          | none  |     5 |
| [ ]  | `D15 T04 §4` | VirtualBox Certification                       | none  |     5 |
| [ ]  | `D15 T04 §6` | Performance Benchmarks                         | none  |     4 |
| [ ]  | `D15 T04 §8` | Crash Analytics Review                         | none  |     4 |
| [ ]  | `D15 T04 §5` | Real Hardware Test Checklist                   | none  |     4 |
| [ ]  | `D15 T04 §7` | Release Readiness Checklist                    | none  |     2 |
| [ ]  | `D15 T01 §6` | Artifact Manifest                              | none  |     4 |
| [ ]  | `D15 T01 §8` | Reproducible Builds                            | none  |     6 |
| [ ]  | `D15 T03 §2` | CDN & Hosting                                  | none  |     3 |
| [ ]  | `D15 T05 §1` | GitHub Release Workflow                        | none  |     6 |
| [ ]  | `D15 T03 §3` | Release Promotion Pipeline                     | none  |     5 |
| [ ]  | `D15 T05 §2` | Changelog Discipline                           | none  |     4 |
| [ ]  | `D15 T03 §8` | Status Page                                    | none  |     2 |
| [ ]  | `D15 T05 §3` | Contribution Guide Overhaul                    | none  |     2 |
| [ ]  | `D15 T05 §4` | Issue & PR Templates                           | none  |     4 |
| [ ]  | `D15 T05 §5` | README Overhaul                                | none  |     6 |
| [ ]  | `D15 T05 §6` | Project Website                                | none  |     6 |
| [ ]  | `D15 T05 §7` | Community Channels                             | none  |     5 |
| [ ]  | `D15 T05 §8` | Roadmap Publication + GitHub Milestones Sync   | none  |     4 |

### Phase 16 -- Architecture ports

The architecture abstraction layer, the AArch64 port and processor-group scaling are cheapest once the x86-64 system is whole, so they follow the release.

| Done | Section       | Deliverable                                   | Needs | Items |
| :--: | ------------- | --------------------------------------------- | ----- | ----: |
| [ ]  | `D16 T01 §1`  | Define HAL Interface                          | none  |    15 |
| [ ]  | `D16 T01 §2`  | Create arch/x86_64/ and Move Files            | none  |    16 |
| [ ]  | `D16 T01 §3`  | Update Makefile                               | none  |     5 |
| [ ]  | `D16 T01 §4`  | Update Include Paths                          | none  |     3 |
| [ ]  | `D16 T01 §5`  | Verify 4-Platform Boot                        | none  |     5 |
| [ ]  | `D16 T02 §1`  | UEFI AA64 Bootloader                          | none  |     6 |
| [ ]  | `D16 T02 §2`  | Exception Vectors + EL1 Entry                 | none  |     5 |
| [ ]  | `D16 T02 §3`  | GICv3 Interrupt Controller                    | none  |     5 |
| [ ]  | `D16 T02 §4`  | ARM Generic Timer                             | none  |     5 |
| [ ]  | `D16 T02 §5`  | TTBR Page Tables                              | none  |     5 |
| [ ]  | `D16 T02 §6`  | PSCI SMP Bringup                              | none  |     5 |
| [ ]  | `D16 T02 §7`  | SVC Syscall Entry                             | none  |     5 |
| [ ]  | `D16 T02 §8`  | NEON/SVE Context Switch                       | none  |     4 |
| [ ]  | `D16 T02 §9`  | ARM Security Features                         | none  |     5 |
| [ ]  | `D16 T02 §10` | QEMU AArch64 Test Suite                       | none  |     5 |
| [ ]  | `D16 T03 §1`  | Raise MAX_CPUS and Dynamic Per-CPU Allocation | none  |     8 |
| [ ]  | `D16 T03 §2`  | PROCESSOR_NUMBER Type                         | none  |     3 |
| [ ]  | `D16 T03 §3`  | KeSetTargetProcessorDpcEx                     | none  |     3 |
| [ ]  | `D16 T03 §4`  | GROUP_AFFINITY for Affinity APIs              | none  |     2 |
| [ ]  | `D16 T03 §5`  | MADT Parsing for >64 LAPICs                   | none  |     2 |

### Phase 18 -- Future research

A hypervisor, a GPU compositor, an AI runtime, Android app compatibility and further ports: work with no committed place in the release. It stays planned so that it is not lost, and it runs last.

| Done | Section      | Deliverable                                        | Needs | Items |
| :--: | ------------ | -------------------------------------------------- | ----- | ----: |
| [ ]  | `D18 T01 §1` | x86-64 Gap Analysis                                | none  |     5 |
| [ ]  | `D18 T01 §2` | Build System Multi-Arch                            | none  |     4 |
| [ ]  | `D18 T01 §3` | AArch64 Boot Path (Minimal Kernel Prototype)       | none  |     7 |
| [ ]  | `D18 T01 §4` | RISC-V (RV64GC) Gap Analysis                       | none  |     4 |
| [ ]  | `D18 T01 §5` | ABI Considerations                                 | none  |     7 |
| [ ]  | `D18 T01 §6` | AArch64 Port Plan (`src/arch/arm64/` Layer Design) | none  |     4 |
| [ ]  | `D18 T01 §7` | Research Deliverables                              | none  |     4 |
| [ ]  | `D18 T02 §1` | Intel VT-x / AMD-V Gap Analysis                    | none  |     5 |
| [ ]  | `D18 T02 §2` | Minimal Hypervisor Design (Type-1.5 VMM)           | none  |     7 |
| [ ]  | `D18 T02 §3` | Minimal Linux Guest Proof-of-Concept               | none  |     7 |
| [ ]  | `D18 T02 §4` | virtio Device Emulation Design (Host-Side)         | none  |     5 |
| [ ]  | `D18 T02 §5` | Snapshot & Live Migration Research                 | none  |     3 |
| [ ]  | `D18 T02 §6` | GPU Passthrough Research (VT-d / AMD-Vi)           | none  |     5 |
| [ ]  | `D18 T02 §7` | Research Deliverables                              | none  |     3 |
| [ ]  | `D18 T03 §1` | GPU Access Strategy Options                        | none  |     4 |
| [ ]  | `D18 T03 §2` | TinyGL → Mesa lavapipe Upgrade Research            | none  |     5 |
| [ ]  | `D18 T03 §3` | Display Engine Research (AMD DCN + Intel Arc)      | none  |     3 |
| [ ]  | `D18 T03 §4` | Vulkan Kernel Driver Architecture                  | none  |     4 |
| [ ]  | `D18 T03 §5` | Compositor GPU Path Design                         | none  |     6 |
| [ ]  | `D18 T03 §6` | Research Deliverables                              | none  |     3 |
| [ ]  | `D18 T04 §1` | TPM 2.0 CSPRNG Integration + `swtpm` Dev Setup     | none  |     4 |
| [ ]  | `D18 T04 §2` | Measured Boot Chain (PCR 8–10 Extensions)          | none  |     6 |
| [ ]  | `D18 T04 §3` | UEFI Secure Boot PK/KEK/db Key Hierarchy           | none  |     5 |
| [ ]  | `D18 T04 §4` | AES-256-XTS Full Disk Encryption (FDE)             | none  |     4 |
| [ ]  | `D18 T04 §5` | vTPM for ImpossibleHV Guests                       | none  |     4 |
| [ ]  | `D18 T04 §6` | Research Deliverables                              | none  |     3 |
| [ ]  | `D18 T05 §1` | Inference Runtime Options Survey                   | none  |     5 |
| [ ]  | `D18 T05 §2` | SIMD Enablement for User Mode                      | none  |     5 |
| [ ]  | `D18 T05 §3` | ggml C99 Kernel Port                               | none  |     5 |
| [ ]  | `D18 T05 §4` | GGUF Model Format & Loading                        | none  |     6 |
| [ ]  | `D18 T05 §5` | OS Integration Points (`SYS_AI_INFER` + Apps)      | none  |     5 |
| [ ]  | `D18 T05 §6` | GPU Inference Research (Stretch)                   | none  |     5 |
| [ ]  | `D18 T05 §7` | Research Deliverables                              | none  |     3 |
| [ ]  | `D18 T06 §1` | Research Charter and Success Criteria              | none  |     5 |
| [ ]  | `D18 T06 §2` | Architecture Options Record                        | none  |     5 |
| [ ]  | `D18 T06 §3` | Host Kernel Prerequisites                          | none  |     5 |
| [ ]  | `D18 T06 §4` | Guest Android Runtime Plan                         | none  |     5 |
| [ ]  | `D18 T06 §5` | Display and Input Bridging                         | none  |     4 |
| [ ]  | `D18 T06 §6` | ABI and ISA Strategy                               | none  |     4 |
| [ ]  | `D18 T06 §7` | Distribution and Ecosystem Stance                  | none  |     4 |
| [ ]  | `D18 T06 §8` | Milestones, Exit Gates, and Deliverables           | none  |     6 |
