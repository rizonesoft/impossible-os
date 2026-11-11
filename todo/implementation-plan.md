# Impossible OS -- Implementation Plan

The order to build every roadmap section in, from today to the release. Every numbered section of every TODO file appears in exactly one row.

> **Progress:** **939 of 2592 sections complete (36%).** 116 more are shipped or in progress and still owed work or review. Derived by `python3 scripts/todo-plan.py --sync`, never edited by hand.

**How to use this.** Ask for the next rows instead of reading this file, which is long: `python3 scripts/todo-plan.py --next 5` prints the first rows that can be built now (open, every dependency done, not parked), in plan order. A row is a complete instruction. Copy it and paste it:

```
implement todo section: | [ ] | `D01 T14 §2` | Boot Progress Named-Stage API |
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

**What is written by hand.** The phase headings, the paragraph under each one, and the order of the rows. A sync never moves a row and never touches prose. A new section is placed by the sync after the last row of its own file, or later when it depends on something later; move it by hand if it belongs elsewhere. The check holds the order: an open section never sits before an open section it depends on. Finished work constrains nothing.

**How phases are authored.** A phase is a `### Phase <N> -- <Title>` heading, one paragraph on why it runs where it does, and one table. The phases were seeded on 2026-11-11 from the roadmap domains: a section starts in its domain's phase and moves later only when it depends on a section in a later one.

**Known defect in the source tables.** Some Implementation Order tables name dependencies that form a cycle. `python3 scripts/todo-plan.py --cycles` lists the open sections involved. They are never offered by `--next`, and they may sit in either order here, until the `Depends On` cells are corrected.

## The phases

### Phase 0 -- Infrastructure: tooling, test harnesses and the roadmap machinery

Everything later is proven by these gates, so they come first: the build and analysis tooling, the kernel, user-mode and desktop test harnesses, the TODO graph this plan is derived from, the documentation site and the repository's own housekeeping.

| Done | Section       | Deliverable                                                                                                           |
| :--: | ------------- | --------------------------------------------------------------------------------------------------------------------- |
| [x]  | `D00 T01 §1`  | Host Bootstrap and Dependency Contract                                                                                |
| [x]  | `D00 T01 §2`  | Supported Host Profiles and Reproducible Environments                                                                 |
| [x]  | `D00 T01 §3`  | Build, Test, Lint, and Run Wrapper Contract                                                                           |
| [x]  | `D00 T01 §4`  | Machine Launcher and Debug Profile Matrix                                                                             |
| [x]  | `D00 T01 §5`  | Git Hooks and Local Automation Lifecycle                                                                              |
| [x]  | `D00 T01 §6`  | GitHub Actions and Artifact Policy Alignment                                                                          |
| [x]  | `D00 T01 §7`  | Tooling Doctor and Regression Pack                                                                                    |
| [x]  | `D00 T01 §8`  | Unify Specialist-Launcher Argument Surface                                                                            |
| [x]  | `D00 T01 §9`  | Legacy-XREF Sweep                                                                                                     |
| [x]  | `D00 T01 §10` | Smoke-Test POST16 Assertions                                                                                          |
| [x]  | `D00 T01 §11` | Bare Section-Ref Sweep                                                                                                |
| [x]  | `D00 T01 §12` | Enforce Required-Tool Version Floors                                                                                  |
| [x]  | `D00 T01 §13` | Duplicate-Recipe Sweep in the Root Makefile                                                                           |
| [x]  | `D00 T01 §14` | Defect-Finding Tools: Fuzzers, UBSan, Static Analysis, Compile Cache                                                  |
| [x]  | `D00 T01 §15` | Static-Analysis Triage and Changed-File Gate                                                                          |
| [x]  | `D00 T01 §16` | Fuzz Targets for the Remaining Untrusted-Input Parsers                                                                |
| [x]  | `D00 T01 §17` | Shard the todo-graph Build Suite                                                                                      |
| [/]  | `D00 T01 §18` | Implementation Plan: Clear the Dependency Cycles and Drive the Run From It                                            |
| [x]  | `D00 T02 §1`  | Canonical Ownership and Source-of-Truth Map                                                                           |
| [x]  | `D00 T02 §2`  | Skill Lifecycle, Templates, and Catalog Rules                                                                         |
| [x]  | `D00 T02 §3`  | Hook Routing and Policy Contract                                                                                      |
| [x]  | `D00 T02 §4`  | External-Reviewer Contract (Codex)                                                                                    |
| [x]  | `D00 T02 §5`  | MCP, Permissions, and Extension Boundary                                                                              |
| [x]  | `D00 T02 §6`  | `AGENTS.md` Cross-Tool Pointer File                                                                                   |
| [x]  | `D00 T02 §7`  | AI-Assist Commit Disclosure Policy                                                                                    |
| [x]  | `D00 T02 §8`  | Autonomous-Agent Boundary Policy                                                                                      |
| [x]  | `D00 T02 §9`  | AI Workflow Regression Suite                                                                                          |
| [x]  | `D00 T03 §1`  | kmalloc Fault Injection                                                                                               |
| [x]  | `D00 T03 §2`  | Deterministic Race Barrier                                                                                            |
| [x]  | `D00 T03 §3`  | Scratch-Buffer Helper                                                                                                 |
| [x]  | `D00 T03 §4`  | Retrofit Existing "Test Gaps" Stamps                                                                                  |
| [x]  | `D00 T03 §5`  | Test-Scoped klog Level Demotion (`TEST_KLOG_SUPPRESS`)                                                                |
| [x]  | `D00 T03 §6`  | Fault-Injection Hardening (task-filter, total-hits cap, multi-allocator)                                              |
| [x]  | `D00 T03 §7`  | Test Action/Cleanup Registry (`test_add_action`)                                                                      |
| [x]  | `D00 T03 §8`  | Per-Test Heap-Leak Detection                                                                                          |
| [x]  | `D00 T03 §9`  | Audit + Classify the 50+ [LEAK] Failures §8 Surfaced                                                                  |
| [x]  | `D00 T03 §10` | An Over-Long Failing Assertion Wedges the Boot Instead of Reporting                                                   |
| [x]  | `D00 T03 §11` | A Read One Byte Past a Buffer Is Invisible to Every Test We Can Write                                                 |
| [ ]  | `D00 T03 §12` | Assert or Fix the 55 Test-Phase [FAIL] Lines                                                                          |
| [x]  | `D00 T04 §1`  | User-Mode Test Assertion Macro                                                                                        |
| [x]  | `D00 T04 §2`  | Userland syscall.h Parity with Kernel INT 0x80 ABI                                                                    |
| [x]  | `D00 T04 §3`  | Kernel Test Launcher                                                                                                  |
| [x]  | `D00 T04 §4`  | Launcher Manifest, Timeouts, and CI-Friendly Output                                                                   |
| [x]  | `D00 T04 §5`  | User-Mode Fault-Injection Bridge                                                                                      |
| [x]  | `D00 T04 §6`  | Per-Test Isolation + Cleanup Hook                                                                                     |
| [x]  | `D00 T04 §7`  | JUnit XML + JSON Output Formats                                                                                       |
| [x]  | `D00 T04 §8`  | Test Type Taxonomy: smoke / correctness / stress / perf                                                               |
| [x]  | `D00 T04 §9`  | Syscall Test Binary                                                                                                   |
| [x]  | `D00 T04 §10` | Libc Test Binary                                                                                                      |
| [x]  | `D00 T04 §11` | IPC Test Binary                                                                                                       |
| [x]  | `D00 T04 §12` | Process Lifecycle Test                                                                                                |
| [x]  | `D00 T04 §13` | File I/O Test                                                                                                         |
| [x]  | `D00 T04 §14` | Win32 API Test Binary                                                                                                 |
| [x]  | `D00 T04 §15` | Binary Format Loader Coverage (ELF / PE32+ / EIF)                                                                     |
| [x]  | `D00 T04 §16` | Build Integration                                                                                                     |
| [x]  | `D00 T04 §17` | Fast-Path Transport Hardening (probes + ABI drift + silent-hang elimination)                                          |
| [x]  | `D00 T04 §18` | Fast-Path Observability + ABI Versioning (beyond Win11 + Linux)                                                       |
| [x]  | `D00 T04 §19` | Fast-Path Transition Ring Buffer + Cross-Transport Fuzz                                                               |
| [x]  | `D00 T04 §20` | Site-Targeted and PMM-Countdown Fault Selectors                                                                       |
| [x]  | `D00 T04 §21` | Naming Fork's Child-PML4 Allocation, and Failing That Fork Closed                                                     |
| [x]  | `D00 T04 §22` | Honest Machine Artifacts for Skipped Sub-Tests                                                                        |
| [x]  | `D00 T04 §23` | Generated Exit-Status ABI for Ring-3 Assertions                                                                       |
| [x]  | `D00 T04 §24` | Host-Side JSON Artifact File and Abort-Aware Summaries                                                                |
| [x]  | `D00 T04 §25` | Non-Forgeable Launcher Record Framing                                                                                 |
| [x]  | `D00 T04 §26` | Generator Hardening: Preprocessor-Faithful Constant Extraction                                                        |
| [x]  | `D00 T04 §27` | Artifact Consumer Completeness: Run Identity and the Record Name Bound                                                |
| [x]  | `D00 T04 §28` | Atomic Publication of the Generated Header Pair                                                                       |
| [x]  | `D00 T04 §29` | Serial-Transport Volume: Derived Burst Ceiling and Incremental Parse                                                  |
| [x]  | `D00 T04 §30` | Per-Run Artifact Records and Per-Leg Aliases                                                                          |
| [x]  | `D00 T04 §31` | The Record Name Bound and the Refusal That Must Be Counted                                                            |
| [x]  | `D00 T04 §32` | ABI Validation on Every Artifact Target, Not Just `all`                                                               |
| [x]  | `D00 T04 §33` | Per-Binary Output Capture at the Source                                                                               |
| [x]  | `D00 T04 §34` | Orphaned-QEMU Recovery                                                                                                |
| [x]  | `D00 T04 §35` | Immutable Enumeration Plan for the Two-Walk Launcher                                                                  |
| [x]  | `D00 T04 §36` | Per-Binary Output Artifacts and Fail-Closed Byte Reconciliation                                                       |
| [x]  | `D00 T04 §37` | Refusal-Name Correlator and JUnit Error/Failure Classification                                                        |
| [x]  | `D00 T04 §38` | Per-Leg Generation Pointer for Coherent Document Resolution                                                           |
| [x]  | `D00 T04 §39` | Executable Identity for a Planned Entry                                                                               |
| [x]  | `D00 T04 §40` | Producer-Side Capture Emission Budget                                                                                 |
| [x]  | `D00 T04 §41` | Post-Ship Follow-Up Backfill (orphan cohort 2026-07-31)                                                               |
| [x]  | `D00 T04 §42` | Structural Reap-Grace Assertions Instead of a Wall-Clock Margin                                                       |
| [x]  | `D00 T04 §43` | Distinguishable Identity Kind for Aggregate Refusals                                                                  |
| [x]  | `D00 T04 §44` | Close the Residual Completeness Gaps §37's Review Found                                                               |
| [x]  | `D00 T04 §45` | Reader Leases for a Resolved Generation                                                                               |
| [x]  | `D00 T04 §46` | Inode-Safe Classification in the Pointer Sweep                                                                        |
| [x]  | `D00 T04 §47` | End-to-End Proof of the Loader's Identity-Mismatch Branch                                                             |
| [x]  | `D00 T04 §48` | Wire-Published Facts the Host Enforces: Write Identity, Spawn Expectation, Chunk Bound                                |
| [x]  | `D00 T04 §49` | Run-Boundary Fence for Emitters That Outlive Their Binary                                                             |
| [x]  | `D00 T04 §50` | Per-Child Loader Evidence: Ring-3 Entry and Stage Flags in the Child, Not in Globals                                  |
| [x]  | `D00 T04 §51` | A Reproducible Kernel Assertion Total, and the Intermittent TEB Halt Behind It                                        |
| [x]  | `D00 T04 §52` | Bind a Leg Pointer to the Record's Own Identity                                                                       |
| [x]  | `D00 T04 §53` | A Directory Occupying a Pointer or Lease Name                                                                         |
| [x]  | `D00 T04 §54` | A Write Abandoned Before Its First Chunk Ever Reached the Wire                                                        |
| [x]  | `D00 T04 §55` | Klog-Ring Assertions That Depend on How Much the Boot Logged                                                          |
| [x]  | `D00 T04 §56` | Fork Publication Interlocked With the Capture-Tree Reap                                                               |
| [x]  | `D00 T04 §57` | A Klog Delivery Receipt, So a Record's Settlement Is Exact                                                            |
| [x]  | `D00 T04 §58` | Launcher Waits Bounded Independently of the Monotonic Clock                                                           |
| [x]  | `D00 T04 §59` | Capture Reconciliation Runs on the Default Test Path, Not Only for Artifacts                                          |
| [x]  | `D00 T04 §60` | An Aggregate Budget for Pointer Validation Under the Retention Mutex                                                  |
| [x]  | `D00 T04 §61` | Validate the Commit Marker as a Shape, and Read Each Record Document From One Snapshot                                |
| [/]  | `D00 T04 §62` | Detaching an Age Candidate Before Classifying It                                                                      |
| [x]  | `D00 T05 §1`  | Framebuffer Snapshot API                                                                                              |
| [x]  | `D00 T05 §2`  | QEMU Framebuffer Dump                                                                                                 |
| [x]  | `D00 T05 §3`  | Desktop Smoke Test                                                                                                    |
| [x]  | `D00 T05 §4`  | Input Event Injection                                                                                                 |
| [x]  | `D00 T05 §5`  | Terminal Output Verification                                                                                          |
| [x]  | `D00 T05 §6`  | Reference Screenshot Comparison                                                                                       |
| [x]  | `D00 T05 §7`  | Visual Regression CI Pipeline                                                                                         |
| [x]  | `D00 T05 §8`  | Window Manager State Verification                                                                                     |
| [x]  | `D00 T05 §9`  | Perceptual Diff and Structured Screenshot Needles                                                                     |
| [x]  | `D00 T05 §10` | Frame Timing and Drop Oracle                                                                                          |
| [x]  | `D00 T05 §11` | Input Record and Replay (Unicode and IME aware)                                                                       |
| [x]  | `D00 T05 §12` | Headless Compositor and Frame-Lock Stepping                                                                           |
| [x]  | `D00 T05 §13` | Multi-Monitor and DPI Scaling Test Matrix                                                                             |
| [x]  | `D00 T05 §14` | WCAG Sweep Over Automation Tree                                                                                       |
| [x]  | `D00 T05 §15` | Test Isolation and Crash Artifact Capture                                                                             |
| [x]  | `D00 T06 §1`  | Frontmatter Schema and Spec                                                                                           |
| [x]  | `D00 T06 §2`  | Generator and Cache Format                                                                                            |
| [x]  | `D00 T06 §3`  | Validator (stale XREF / dangling dep / orphan / cycle / bat / status / schema)                                        |
| [x]  | `D00 T06 §4`  | Query CLI (ready / blocked / blocking / by-domain / backlinks / deferred / orphans / stale / stats / code)            |
| [x]  | `D00 T06 §5`  | Migration: Back-Fill Frontmatter on Existing TODOs                                                                    |
| [x]  | `D00 T06 §6`  | CI Gate and Make Target                                                                                               |
| [x]  | `D00 T06 §7`  | Visualization Output (mermaid / graphviz / ascii / gantt / markdown)                                                  |
| [x]  | `D00 T06 §8`  | MCP Server (AI-Agent Transport over the Cache)                                                                        |
| [x]  | `D00 T06 §9`  | Per-Item `stamped_items` Cache Extension                                                                              |
| [x]  | `D00 T06 §10` | Multi-Line Function-Head Resolution in `resolve_symbol`                                                               |
| [x]  | `D00 T06 §11` | Count What Check 7 Cannot Resolve Instead of Skipping It                                                              |
| [x]  | `D00 T06 §12` | Resolver Coverage Past the Head Limit, and Lexer/Cache Robustness                                                     |
| [x]  | `D00 T06 §13` | Stored-Ref Repair: Unpaired Symbols and Bare Filenames                                                                |
| [x]  | `D00 T06 §14` | Identity Gate Over the Whole Symbol-Ref Population                                                                    |
| [x]  | `D00 T06 §15` | The Shared Rule's Cost: Structural-Walk Hot Path and Split Cache Lifecycles                                           |
| [x]  | `D00 T06 §16` | Wire the Identity Gate So Something Actually Runs It                                                                  |
| [x]  | `D00 T06 §17` | One Shared Cache-Schema Validator for Both Cache Readers                                                              |
| [x]  | `D00 T06 §18` | Identity-Gate Hardening: Producer Differential and Protocol-Constant Extraction                                       |
| [x]  | `D00 T06 §19` | Cache-Validation Reach: the Remaining Raw Cache Readers                                                               |
| [x]  | `D00 T06 §20` | A Machine-Checkable Bucket-Emission Contract                                                                          |
| [x]  | `D00 T06 §21` | The Producer-Side Generation Window in `build.py`                                                                     |
| [x]  | `D00 T06 §22` | `query.py` Fail-Closed Through Every Transport                                                                        |
| [x]  | `D00 T06 §23` | `validate.py` Rebuild Recovery and the Baseline Cache                                                                 |
| [x]  | `D00 T06 §24` | The History-Identity Cost Decision: `requires_history` for the Query Profile                                          |
| [x]  | `D00 T06 §25` | Fields the Readers Consume That the Producer Never Emits                                                              |
| [x]  | `D00 T06 §26` | Domain-Code Resolution Picks a Directory by Cache Order                                                               |
| [x]  | `D00 T06 §27` | Effective-History Identity: Replace Refs and Grafts                                                                   |
| [x]  | `D00 T06 §28` | Section-Level Readiness: Verbs That Consult the Dependency Evidence the Graph Actually Has                            |
| [x]  | `D00 T06 §29` | `--fix-line-numbers` Reports Success Over Targets It Could Not Resolve                                                |
| [x]  | `D00 T06 §30` | `--watch` Never Ticks on a History Change for the History-Consuming Verbs                                             |
| [x]  | `D00 T06 §31` | An Equal-Count Shallow Boundary Still Moves `created_at` Under an Unchanged Identity                                  |
| [x]  | `D00 T06 §32` | In-File Anchor Links Are Checked by Nothing                                                                           |
| [x]  | `D00 T06 §33` | Stamp-Target Capture Splits a Link Whose Label Contains a Space                                                       |
| [x]  | `D00 T06 §34` | The Performance Budget Times Itself on a Clock That Steps                                                             |
| [x]  | `D00 T06 §35` | A Same-File Stamp Matches Its Own Text, So Repair Answers With the Stamp's Line                                       |
| [x]  | `D00 T06 §36` | The Cache Producer Adopts the Shared Fence Tracker                                                                    |
| [x]  | `D00 T06 §37` | One Clause Parser Decides How Many Destinations a Clause Names                                                        |
| [x]  | `D00 T06 §38` | Every Gate Parser Adopts the Shared Fence Tracker                                                                     |
| [x]  | `D00 T06 §39` | The Shared Fence Tracker Becomes Container-Aware                                                                      |
| [x]  | `D00 T06 §40` | An Inputs Section Marker Keeps Its Closing Backtick, So One Dependency Is Two Edges                                   |
| [x]  | `D00 T06 §41` | The Two Mutating Repair Tools Adopt the Shared Fence Tracker                                                          |
| [x]  | `D00 T06 §42` | HTML Blocks Join the Tracker, and the Terminal Contract Carries the Block Kind                                        |
| [x]  | `D00 T06 §43` | Finish the One-Contract Consolidation Across the Consumer Closure                                                     |
| [x]  | `D00 T06 §44` | The Inline Scanner Honours CommonMark Precedence, and the Table Cell Reaches It Unmodified                            |
| [x]  | `D00 T06 §45` | The Container Phase Stops Being Quadratic, Stops Erasing, and Becomes the Section Context the Four Walks Share        |
| [x]  | `D00 T06 §46` | CommonMark HTML Block Type 7 Needs a Non-Backtracking Matcher                                                         |
| [x]  | `D00 T06 §47` | The Gate Consumers Adopt the Section-Context Projections the Producer Already Uses                                    |
| [x]  | `D00 T06 §48` | The Mutating Repair Tool Stops Rewriting the Raw HTML It Is Shown                                                     |
| [x]  | `D00 T06 §49` | Decide Whether `alias-staleness` Is Producer-Assertable, Then Promote It or Record Why Not                            |
| [x]  | `D00 T06 §50` | A Base Older Than the Gate's Own Protocol Must Not Be Reported as a Broken Gate                                       |
| [x]  | `D00 T06 §51` | The Byte-Identical Closure Exit Must Not Outrank the Protocol Refusals                                                |
| [x]  | `D00 T06 §52` | Object-Replacement Metadata Can Misrepresent an Endpoint by Routes Other Than Its Commit                              |
| [x]  | `D00 T06 §53` | The Protocol Probe Executes the Tree It Is Judging, So No In-Process Channel Is Trusted                               |
| [x]  | `D00 T06 §54` | The Gate Adjudicates the WORKING TREE While the Push Carries the COMMIT                                               |
| [x]  | `D00 T06 §55` | The Byte-Identical Path Pays for a Full Base Checkout It Does Not Need                                                |
| [x]  | `D00 T06 §56` | The Resolver Walks Leak What the Tree Spawns on the Path Where They SUCCEED                                           |
| [x]  | `D00 T06 §57` | The Hook Certifies Repository HEAD, Not the Commit Being Pushed                                                       |
| [x]  | `D00 T06 §58` | Head-Versus-Tree Checks Section 54 Made Unreachable Are Retired or Re-Fixtured                                        |
| [x]  | `D00 T06 §59` | The Identity-Gate Fixture Group Pays for a Whole Repository and Proves Less Than It Claims                            |
| [x]  | `D00 T06 §60` | Every Name-Resolved Use of TMP_DIR Is Still a Name                                                                    |
| [x]  | `D00 T07 §1`  | Bridge Skeleton + LSP JSON-RPC Client                                                                                 |
| [x]  | `D00 T07 §2`  | clangd Integration (C / H)                                                                                            |
| [x]  | `D00 T07 §3`  | asm-lsp Integration (NASM)                                                                                            |
| [x]  | `D00 T07 §4`  | bash-language-server Integration (.sh)                                                                                |
| [x]  | `D00 T07 §5`  | pyright Integration (Python)                                                                                          |
| [x]  | `D00 T07 §6`  | PowerShellEditorServices Integration (.ps1)                                                                           |
| [x]  | `D00 T07 §7`  | Six MCP Tools (Hover, Definition, References, Diagnostics, Workspace/Document Symbol)                                 |
| [x]  | `D00 T07 §8`  | Extension Routing, Per-LSP Lock, Concurrent-Call Safety                                                               |
| [x]  | `D00 T07 §9`  | Setup Deps, Manifest, Makefile Target, Docs, Boundary Compliance                                                      |
| [x]  | `D00 T07 §10` | Unit Tests, --self-test Harness, test-tooling Surface                                                                 |
| [x]  | `D00 T07 §11` | Extended LSP Tools (Completion, Signature Help, Type-Definition, Implementation, Declaration, Call Hierarchy, Code... |
| [x]  | `D00 T07 §12` | File-Change Lifecycle (`didOpen`, `didChange`, `didSave`, `didClose` Forwarding)                                      |
| [x]  | `D00 T07 §13` | LSP Subprocess Health Monitoring + Auto-Restart                                                                       |
| [x]  | `D00 T07 §14` | Structured JSON Logging + Request Correlation IDs                                                                     |
| [x]  | `D00 T07 §15` | Path Sandboxing + Workspace Boundary Enforcement                                                                      |
| [x]  | `D00 T07 §16` | Warm-Index Preloading + Cold-Start Budget                                                                             |
| [x]  | `D00 T07 §17` | Background Warm-Start Mode (MCP Launcher Compatibility)                                                               |
| [x]  | `D00 T07 §18` | Type Hierarchy Tools (Supertypes / Subtypes, Read-Only)                                                               |
| [x]  | `D00 T07 §19` | Scale Roadmap (Deferred)                                                                                              |
| [x]  | `D00 T07 §20` | Reap Determinism and Leak Attribution for the Bridge's Language Servers                                               |
| [x]  | `D00 T07 §21` | Thread-Group Edge Cases in the Language-Server Reap                                                                   |
| [x]  | `D00 T07 §22` | Reap Sweep Internals: pidfd Ownership, One-Snapshot Enumeration, Ledger Reader                                        |
| [x]  | `D00 T07 §23` | Injected-Clock Deadline Coverage and the Non-Dumpable-Child Reap Blind Spot                                           |
| [x]  | `D00 T07 §24` | Single-Pass Procfs Cleanup in the Teardown Sweep                                                                      |
| [x]  | `D00 T07 §25` | Restart-Publish Confirmed-Dead Gate and Single-Flight Teardown                                                        |
| [x]  | `D00 T07 §26` | Teardown Verdict Propagation and a Non-Reaping Liveness Probe                                                         |
| [x]  | `D00 T08 §1`  | Codex MCP Wiring + Cross-Config Drift Validator                                                                       |
| [x]  | `D00 T08 §2`  | Codex Invocation Policy: No `--model` / `--effort` From Claude                                                        |
| [x]  | `D00 T08 §3`  | `receiving-code-review` Hard Gate: State File + Post-Codex Hook Block                                                 |
| [x]  | `D00 T08 §4`  | Section-Commit Gate Hardens to BLOCK With Build + Codex + Receiving Evidence                                          |
| [x]  | `D00 T08 §5`  | `review-todo-section` Four-Dispatch Enforcement                                                                       |
| [x]  | `D00 T08 §6`  | MCP Usage Discipline -- Doc + CLAUDE.md Pointer                                                                       |
| [x]  | `D00 T08 §7`  | Hook System Audit + Dedupe                                                                                            |
| [x]  | `D00 T08 §8`  | Superpowers Skill Catalog Audit + Trigger Rows                                                                        |
| [x]  | `D00 T08 §9`  | Cross-Tool Drift Detection -- `scripts/audit-ai-system.sh`                                                            |
| [x]  | `D00 T08 §10` | Skill Step-State Telemetry + Non-Contiguous-Step Block                                                                |
| [x]  | `D00 T08 §11` | Hook Event Surface Expansion (SessionStart / UserPromptSubmit / Stop / SubagentStop / PreCompact)                     |
| [x]  | `D00 T08 §12` | AI-Slop Content Lints (Tautological-Test / Stub-Behind-Stamp)                                                         |
| [x]  | `D00 T08 §13` | Documentation Sync -- CLAUDE.md / ai-system.md / mcp-usage.md / Reciprocal XREFs                                      |
| [x]  | `D00 T08 §14` | Remove Copilot CLI Reviewer Wholesale (Subordinate-Reviewer Reduction)                                                |
| [x]  | `D00 T08 §15` | Design-Review Hook Scope Fix: Review-Kind Recognition + Commit-Clears-Gate                                            |
| [x]  | `D00 T08 §16` | Compaction-Resilient Skill-Step State -- PreCompact Orphan-Marking                                                    |
| [x]  | `D00 T08 §17` | Review-Pipeline Pre-Codex Enforcement (Phase-1 + Re-Adversarial)                                                      |
| [x]  | `D00 T08 §18` | Stamp-Region + OS Comparison Completeness Lints (Check 10 + Check 11)                                                 |
| [x]  | `D00 T08 §19` | Codex Prompt-Scope Helper -- Wrapper Plumbing for Re-Reviews                                                          |
| [x]  | `D00 T08 §20` | Implement-Pipeline Section-Commit Gates (Steps 5/8/13/16)                                                             |
| [x]  | `D00 T08 §21` | Implement-TODO-Section Partial-Enforcement Heuristics (WARN-First)                                                    |
| [x]  | `D00 T08 §22` | Review-Pipeline Right-Sizing Doctrine -- RISK_TIER + Bootstrap + Tier-Aware Triage + State Diagnosis + Spiral Check   |
| [x]  | `D00 T08 §23` | Unify SKIP_REVIEW_HOOK Opt-Out Scanner Across PreToolUse Gates                                                        |
| [x]  | `D00 T08 §24` | last-codex-review.json State-Write Reliability -- Close §22 #4 Properly                                               |
| [x]  | `D00 T08 §25` | Observer Step-13/13.5 Regex Coverage for All Codex Dispatch Shapes                                                    |
| [x]  | `D00 T08 §26` | `scripts/commit-with-skip.sh` Wrapper Retired (Closed by §23)                                                         |
| [x]  | `D00 T08 §27` | Phase 1 Evidence-Gate Threshold Tuning via Miss-Log Telemetry                                                         |
| [x]  | `D00 T08 §28` | Codex Prompt Argument Escaping Doctrine                                                                               |
| [x]  | `D00 T08 §29` | Standardize PreToolUse Prefix-Allowlist Across Gates                                                                  |
| [x]  | `D00 T08 §30` | Shell-Aware Codex Dispatch Segmentation in `_review_kind`                                                             |
| [x]  | `D00 T08 §31` | Post-Commit Hook Concurrency: Serialize Build/Smoke via flock                                                         |
| [x]  | `D00 T08 §32` | Grammar-Accurate Shell Segmentation for the Gate Exemptions                                                           |
| [x]  | `D00 T09 §1`  | Preflight Inventory and Risk Register                                                                                 |
| [/]  | `D00 T09 §2`  | `rizonetech` Organization Readiness and Policy Parity                                                                 |
| [x]  | `D00 T09 §3`  | GitHub Pages Custom-Domain Continuity Plan                                                                            |
| [x]  | `D00 T09 §4`  | Repository Transfer Runbook and Rollback Window                                                                       |
| [/]  | `D00 T09 §5`  | Post-Transfer Settings, Workflows, Secrets, and Environments Audit                                                    |
| [x]  | `D00 T09 §6`  | URL, Badge, Docs, and Generated-Link Sweep                                                                            |
| [x]  | `D00 T09 §7`  | Validation Suite: Pages, Actions, Releases, Clone, Hooks, Graph                                                       |
| [x]  | `D00 T09 §8`  | Move-Back and Public-Visibility Runbook                                                                               |
| [x]  | `D00 T09 §9`  | Documentation Sync and Closure                                                                                        |
| [x]  | `D00 T10 §1`  | Site Generator, Project Facts, Coverage, and Drift Gate                                                               |
| [x]  | `D00 T10 §2`  | Map Existing Docs Pages to Their Roadmap Files                                                                        |
| [x]  | `D00 T10 §3`  | Documentation Page Contract, Template, and create-todo Step                                                           |
| [x]  | `D00 T10 §4`  | Document: Infrastructure                                                                                              |
| [x]  | `D00 T10 §5`  | Document: Boot platform, part 1                                                                                       |
| [x]  | `D00 T10 §6`  | Document: Boot platform, part 2                                                                                       |
| [x]  | `D00 T10 §7`  | Document: Boot platform, part 3                                                                                       |
| [x]  | `D00 T10 §8`  | Document: Kernel core, part 1                                                                                         |
| [x]  | `D00 T10 §9`  | Document: Kernel core, part 2                                                                                         |
| [x]  | `D00 T10 §10` | Document: Kernel core, part 3                                                                                         |
| [x]  | `D00 T10 §11` | Document: Memory and concurrency                                                                                      |
| [x]  | `D00 T10 §12` | Document: Drivers and hardware, part 1                                                                                |
| [x]  | `D00 T10 §13` | Document: Drivers and hardware, part 2                                                                                |
| [x]  | `D00 T10 §14` | Document: Storage and filesystems                                                                                     |
| [x]  | `D00 T10 §15` | Document: Networking                                                                                                  |
| [x]  | `D00 T10 §16` | Document: Desktop foundation and graphics, part 1                                                                     |
| [x]  | `D00 T10 §17` | Document: Graphics and UI, part 2                                                                                     |
| [x]  | `D00 T10 §18` | Document: Desktop shell                                                                                               |
| [x]  | `D00 T10 §19` | Document: Platform services                                                                                           |
| [x]  | `D00 T10 §20` | Document: Applications and accessories                                                                                |
| [x]  | `D00 T10 §21` | Document: SDK and release                                                                                             |
| [x]  | `D00 T10 §22` | Doc Freshness: `sources=` and a Stale-Page Warning                                                                    |
| [x]  | `D00 T10 §23` | Site Polish: Sitemap, Last-Updated, Link Health, OpenGraph                                                            |
| [x]  | `D00 T10 §24` | Versioned Release Docs: Pinned Refs and Version-Scoped Rendering                                                      |
| [x]  | `D00 T10 §25` | Docs Search Completeness and Accessibility                                                                            |
| [x]  | `D00 T10 §26` | Document: Host tools                                                                                                  |
| [x]  | `D00 T10 §27` | Document: Architecture ports and future research                                                                      |
| [x]  | `D00 T10 §28` | Review-Class Nets for Site Tooling: Real Parsers, Scheduler Stress                                                    |
| [x]  | `D00 T10 §29` | Retained Release Trees: Snapshots, Manifest, Live Verification                                                        |
| [x]  | `D00 T10 §30` | SDK Release Docs and API Reference                                                                                    |
| [x]  | `D00 T10 §31` | Docs Search Quality and Automated Accessibility Audit                                                                 |
| [x]  | `D00 T10 §32` | Review-Class Nets: Fail Direction and Bounded Waits in Site Tooling                                                   |
| [x]  | `D00 T10 §33` | Docs Search Typo Tolerance and Wider Accessibility Audit                                                              |

### Phase 1 -- Boot platform: firmware handoff, loader, measured boot and recovery

Nothing runs until the machine boots, and a boot defect hides every defect behind it. This phase carries the loader, the boot_info handoff, boot entries and the menu, Secure Boot and TPM measurement, diagnostics, network boot, resume and recovery. Rows that wait on later work (the installer, storage repair) sit in the phase of what they wait on.

| Done | Section       | Deliverable                                                                                       |
| :--: | ------------- | ------------------------------------------------------------------------------------------------- |
| [x]  | `D01 T01 §1`  | Canonical boot_info Field Ownership Table                                                         |
| [x]  | `D01 T01 §2`  | Generated ABI Manifest and Offset Fingerprint                                                     |
| [x]  | `D01 T01 §3`  | Full Bootloader/Kernel Mirror Drift Checker                                                       |
| [x]  | `D01 T01 §4`  | Optional Payload Descriptor Array                                                                 |
| [x]  | `D01 T01 §5`  | Module and initrd Handoff Contract                                                                |
| [x]  | `D01 T01 §6`  | Handoff Memory Ownership and PMM Reservation Table                                                |
| [x]  | `D01 T01 §7`  | Version Negotiation and Stale-Loader Error Path                                                   |
| [x]  | `D01 T01 §8`  | Boot Protocol Documentation and Schema Changelog                                                  |
| [x]  | `D01 T01 §9`  | ABI Fuzz and Compatibility Tests                                                                  |
| [x]  | `D01 T01 §10` | Cross-Domain Owner Audit for Every boot_info Field                                                |
| [x]  | `D01 T01 §11` | Capability Negotiation and Degraded-Feature Flags                                                 |
| [x]  | `D01 T01 §12` | Common Boot-Path Provenance and Decision Record                                                   |
| [x]  | `D01 T01 §13` | Anti-Rollback and Security-Version Binding                                                        |
| [x]  | `D01 T01 §14` | Warm-Kernel-Update Handoff ABI                                                                    |
| [x]  | `D01 T01 §15` | Capability-Gated Consumer Retrofit                                                                |
| [x]  | `D01 T01 §16` | Anti-Rollback Raise Timing Hardening                                                              |
| [x]  | `D01 T01 §17` | Bootloader Pre-Jump ABI Mismatch Screen                                                           |
| [x]  | `D01 T01 §18` | Anti-Rollback Integration in Version Fault Diagnostics                                            |
| [x]  | `D01 T01 §19` | Stale-ABI QEMU Fixture Harness                                                                    |
| [x]  | `D01 T01 §20` | Bootloader Build Identity                                                                         |
| [x]  | `D01 T01 §21` | Post-Ship Follow-Up Backfill (orphan cohort 2026-07-31)                                           |
| [/]  | `D01 T01 §22` | Handoff Base in the Deploy-Time ABI Fingerprint                                                   |
| [/]  | `D01 T01 §23` | Integrity Coverage for the Handoff Payload Body                                                   |
| [x]  | `D01 T01 §24` | Scripted Anti-Rollback NVRAM Fixture Harness                                                      |
| [x]  | `D01 T01 §25` | Bounded Retry for a Transient Rollback-Floor Write                                                |
| [x]  | `D01 T01 §26` | Enum-VALUE Drift Detection for the Bootloader Mirror                                              |
| [x]  | `D01 T01 §27` | Descriptor Capacity for the Implicit Publishers                                                   |
| [x]  | `D01 T01 §28` | Per-Type Payload Length Contract Before Reserving                                                 |
| [x]  | `D01 T01 §29` | One Warm-Update Descriptor, Chosen the Same Way by Both Phases                                    |
| [x]  | `D01 T01 §30` | Handoff Memory Claims Must Match What PMM Actually Tracks                                         |
| [x]  | `D01 T02 §1`  | UEFI Runtime Services Preservation                                                                |
| [x]  | `D01 T02 §2`  | UEFI Variable Services                                                                            |
| [x]  | `D01 T02 §3`  | GOP Resolution Auto-Detection                                                                     |
| [x]  | `D01 T02 §4`  | SMBIOS Table Parsing                                                                              |
| [x]  | `D01 T02 §5`  | Secure Boot State Detection                                                                       |
| [x]  | `D01 T02 §6`  | Secure Boot Shim Chain-Loading                                                                    |
| [x]  | `D01 T02 §7`  | Boot UX Polish                                                                                    |
| [x]  | `D01 T02 §8`  | Serial Log Standardization and Race Fix                                                           |
| [x]  | `D01 T02 §9`  | SBAT Ops, Secure Boot DB Registry Mirror, and ExitBootServices Retry                              |
| [x]  | `D01 T02 §10` | UEFI Runtime Services Sleepable Lock Migration                                                    |
| [x]  | `D01 T02 §11` | Unified Signed Boot Artifact (UKI-style)                                                          |
| [x]  | `D01 T02 §12` | MS UEFI CA 2023 Transition + 2011 Retirement Tracking                                             |
| [x]  | `D01 T02 §13` | EFI System Partition Integrity Check                                                              |
| [x]  | `D01 T02 §14` | Win32 Firmware Variable + Table Surface                                                           |
| [x]  | `D01 T02 §15` | Post-boot SecureBoot Revalidation                                                                 |
| [x]  | `D01 T02 §16` | Signed `.initrd` / Recovery / Module PE Sections in UKI                                           |
| [x]  | `D01 T02 §17` | SBAT Revocation Metadata in Boot Artifacts                                                        |
| [x]  | `D01 T02 §18` | Build Idempotency: UKI SBAT Survives Incremental Rebuild                                          |
| [x]  | `D01 T02 §19` | Bootloader `ReadBlocks` IoAlign Compliance                                                        |
| [x]  | `D01 T02 §20` | Post-Ship Follow-Up Backfill (orphan cohort 2026-07-31)                                           |
| [x]  | `D01 T03 §1`  | ELF Bounds Checking                                                                               |
| [x]  | `D01 T03 §2`  | ExitBootServices Retry Loop                                                                       |
| [x]  | `D01 T03 §3`  | Fallback Kernel Search                                                                            |
| [x]  | `D01 T03 §4`  | Serial Port Probe and COM2 Fallback                                                               |
| [x]  | `D01 T03 §5`  | GOP Timeout and Graceful Degradation                                                              |
| [x]  | `D01 T03 §6`  | Memory Map Overflow Detection                                                                     |
| [x]  | `D01 T03 §7`  | boot.conf Validation                                                                              |
| [x]  | `D01 T03 §8`  | Kernel Load Memory Allocation Fallback                                                            |
| [x]  | `D01 T03 §9`  | Boot Failure Error Screen                                                                         |
| [x]  | `D01 T03 §10` | ACPI SPCR Serial Port Auto-Detection                                                              |
| [x]  | `D01 T03 §11` | UEFI Watchdog Timer Management                                                                    |
| [x]  | `D01 T03 §12` | Memory Map Descriptor Validation                                                                  |
| [x]  | `D01 T03 §13` | Boot Error Code Registry & NVRAM Persistence                                                      |
| [x]  | `D01 T03 §14` | Error Screen QR Code                                                                              |
| [x]  | `D01 T03 §15` | boot_info ABI Header and Bootloader Populate                                                      |
| [x]  | `D01 T03 §16` | boot_info Kernel Validation, boot_hw Path, and Unit Tests                                         |
| [x]  | `D01 T03 §17` | Memory Map Overlap Normalization                                                                  |
| [x]  | `D01 T03 §18` | Graphical Error Screen (ChromeOS/Win11-Style)                                                     |
| [x]  | `D01 T03 §19` | PT_LOAD Destination Policy                                                                        |
| [x]  | `D01 T03 §20` | Boot Error History Ring -- Producer (Schema, Append Sites, NVRAM Cookie)                          |
| [x]  | `D01 T03 §21` | Boot Error History Ring -- Consumer (Storage Reader, Kernel Renderer, Tests, Docs, Smoke Fixture) |
| [x]  | `D01 T03 §22` | Rejected Boot-Entry Store Is a Silent Failure on Screen                                           |
| [x]  | `D01 T03 §23` | No On-Screen Words Below 800x600 -- the QR Renders, the Text Does Not                             |
| [x]  | `D01 T03 §24` | A Rejected Store Leaves No Trace the User Can Act On Afterwards                                   |
| [x]  | `D01 T04 §1`  | Firmware Table Catalog API                                                                        |
| [x]  | `D01 T04 §2`  | Physical Range and Checksum Validation                                                            |
| [x]  | `D01 T04 §3`  | ACPI/SMBIOS/DTB Table Arbitration                                                                 |
| [x]  | `D01 T04 §4`  | FPDT and Boot Timing Normalization                                                                |
| [x]  | `D01 T04 §5`  | UEFI Memory Attributes and Runtime Properties Inventory                                           |
| [x]  | `D01 T04 §6`  | ESRT Firmware Inventory Mirror                                                                    |
| [x]  | `D01 T04 §7`  | UEFI Conformance Profile and EBBR Detection                                                       |
| [x]  | `D01 T04 §8`  | Registry and BlackBox Firmware Report                                                             |
| [x]  | `D01 T04 §9`  | Firmware Quirk Database                                                                           |
| [x]  | `D01 T04 §10` | Firmware Inventory Tests and Host Decoder                                                         |
| [x]  | `D01 T04 §11` | Firmware Inventory Bug-Fix Debt                                                                   |
| [x]  | `D01 T04 §12` | Firmware Inventory Refactor Debt                                                                  |
| [x]  | `D01 T05 §1`  | Boot Device Identification via LoadedImage                                                        |
| [x]  | `D01 T05 §2`  | Filesystem Access Scoped to Boot Device                                                           |
| [x]  | `D01 T05 §3`  | Boot Device Info in boot_info                                                                     |
| [x]  | `D01 T05 §4`  | Boot Device Type Detection                                                                        |
| [x]  | `D01 T05 §5`  | Device Fallback Chain                                                                             |
| [x]  | `D01 T05 §6`  | UEFI Boot Variable Reading (BootOrder, BootCurrent, BootNext)                                     |
| [x]  | `D01 T05 §7`  | Partition GUID Extraction and Validation                                                          |
| [x]  | `D01 T05 §8`  | Removable Media Detection                                                                         |
| [x]  | `D01 T05 §9`  | Boot Device Registry Population                                                                   |
| [x]  | `D01 T05 §10` | Boot Device Logging and Diagnostics                                                               |
| [x]  | `D01 T05 §11` | Pre-Boot Device Health Check                                                                      |
| [x]  | `D01 T05 §12` | Boot#### Load Option Decode (Diagnostics)                                                         |
| [x]  | `D01 T05 §13` | Extended Boot Variable Capability Surface                                                         |
| [x]  | `D01 T05 §14` | Local Boot Device Path Detail Capture                                                             |
| [x]  | `D01 T06 §1`  | Boot Artifact Matrix and Manifest Format                                                          |
| [x]  | `D01 T06 §2`  | Reproducible Raw/USB Image Build                                                                  |
| [x]  | `D01 T06 §3`  | VHD/VHDX/VDI Conversion and Validation                                                            |
| [x]  | `D01 T06 §4`  | Hybrid ISO / El Torito UEFI Boot                                                                  |
| [x]  | `D01 T06 §5`  | Installer/Live/Recovery Media Detection                                                           |
| [x]  | `D01 T06 §6`  | Bootloader Handoff of Media Role                                                                  |
| [x]  | `D01 T06 §7`  | Artifact Signing and Manifest Verification                                                        |
| [x]  | `D01 T06 §8`  | Offline Artifact Inspector                                                                        |
| [x]  | `D01 T06 §9`  | CI Boot Matrix for Every Artifact                                                                 |
| [x]  | `D01 T06 §10` | Release Checklist and Documentation                                                               |
| [x]  | `D01 T06 §11` | Alternate Artifact Formats: UKI and Network Boot                                                  |
| [x]  | `D01 T06 §12` | Windows Host Parity: Manifest Tooling                                                             |
| [x]  | `D01 T06 §13` | Windows Host Parity: Disk Artifact Converters                                                     |
| [x]  | `D01 T06 §14` | Windows Host Parity: Release Test Subdir + Aggregate Runner                                       |
| [x]  | `D01 T07 §1`  | Boot Entry File Format                                                                            |
| [x]  | `D01 T07 §2`  | Boot Entry Parser and Validator                                                                   |
| [x]  | `D01 T07 §3`  | Boot Policy Merge Order                                                                           |
| [x]  | `D01 T07 §4`  | ESP Store Read + Boot#### OptionalData + Policy Wiring                                            |
| [x]  | `D01 T07 §5`  | Crash-Tolerant Counter Protocol + v19 ABI Validator                                               |
| [x]  | `D01 T07 §6`  | Boot Menu Renderer + Countdown + Input Infrastructure                                             |
| [x]  | `D01 T07 §7`  | Boot Menu Indicators, Hotkeys, hide_when_alone                                                    |
| [x]  | `D01 T07 §8`  | Safe Mode, Test Mode, and Diagnostics Entries                                                     |
| [x]  | `D01 T07 §9`  | A/B and Recovery Entry Integration                                                                |
| [x]  | `D01 T07 §10` | Previous-Kernel and Known-Good Entries                                                            |
| [x]  | `D01 T07 §11` | Boot Entry Editor Tooling                                                                         |
| [x]  | `D01 T07 §12` | Policy Audit Trail and Rollback Reason Codes                                                      |
| [x]  | `D01 T07 §13` | Entry Kinds: Split, UKI, Chainload, Network, Resume                                               |
| [x]  | `D01 T07 §14` | Per-Entry Health-Gated Mark-Good                                                                  |
| [x]  | `D01 T07 §15` | OS-Visible Loader UEFI Variables                                                                  |
| [x]  | `D01 T07 §16` | Bootstrap and First-Install Entry Seeding                                                         |
| [x]  | `D01 T07 §17` | Boot Entry Tests                                                                                  |
| [x]  | `D01 T07 §18` | Boot Loader Interface (systemd BLI) Parity Fixes                                                  |
| [x]  | `D01 T07 §19` | Repeated Top-Level Keys Defeat the Entries-Array Bound                                            |
| [x]  | `D01 T07 §20` | `crc32` Header Located by String Match, Not Structurally                                          |
| [x]  | `D01 T07 §21` | Published Loader Variables Need an Explicit Set-or-Clear Outcome                                  |
| [x]  | `D01 T07 §22` | Store Size Cap Is Untested on Both Sides of the 16 KiB Boundary                                   |
| [x]  | `D01 T07 §23` | A Rejected Store Has No Repair Path, Only a One-Entry Fallback                                    |
| [x]  | `D01 T07 §24` | Boot Menu Help, Power and Saved-Default Keys                                                      |
| [x]  | `D01 T07 §25` | Menu Timeout Keys and One-Off Edit                                                                |
| [x]  | `D01 T08 §1`  | Alternate Boot Protocol Policy                                                                    |
| [x]  | `D01 T08 §2`  | Multiboot2 Feature Parity Audit                                                                   |
| [x]  | `D01 T08 §3`  | Multiboot2-to-boot_info Adapter                                                                   |
| [x]  | `D01 T08 §4`  | Unsupported-Feature Degradation Matrix                                                            |
| [x]  | `D01 T08 §5`  | GRUB / Limine / Legacy BIOS Documentation                                                         |
| [x]  | `D01 T08 §6`  | Compatibility Test Images                                                                         |
| [x]  | `D01 T08 §7`  | Deprecation or Promotion Gate                                                                     |
| [x]  | `D01 T09 §1`  | CPUID Detection & Per-CPU Capability Capture                                                      |
| [x]  | `D01 T09 §2`  | Phase 0 CPU Security Activation Order                                                             |
| [x]  | `D01 T09 §3`  | Hypervisor Detection Before Timer Selection                                                       |
| [x]  | `D01 T09 §4`  | AP CPU Hardening (`ap_cpu_harden()`)                                                              |
| [x]  | `D01 T09 §5`  | Phase 1 XSAVE & PCID Activation Window                                                            |
| [x]  | `D01 T09 §6`  | AP Feature Consistency Validation                                                                 |
| [x]  | `D01 T09 §7`  | CR0/CR4 Safety-Bit Pinning                                                                        |
| [x]  | `D01 T09 §8`  | MTRR/PAT AP Synchronization                                                                       |
| [x]  | `D01 T09 §9`  | CPU Register State Audit Trail                                                                    |
| [x]  | `D01 T09 §10` | AP Bringup Hardening & Robustness                                                                 |
| [x]  | `D01 T09 §11` | Post-Ship Follow-Up Backfill (orphan cohort 2026-07-31)                                           |
| [x]  | `D01 T10 §1`  | Minimal UC MMIO Mapping (`vmm_map_mmio_uc`)                                                       |
| [x]  | `D01 T10 §2`  | IST Stacks for Critical Exceptions                                                                |
| [x]  | `D01 T10 §3`  | Hardware Interrupt Root Cause Investigation                                                       |
| [x]  | `D01 T10 §4`  | ACPI FADT Boot Architecture Flags                                                                 |
| [x]  | `D01 T10 §5`  | PS/2 Controller Detection and Safe Init                                                           |
| [x]  | `D01 T10 §6`  | AHCI Interrupt Hardening                                                                          |
| [x]  | `D01 T10 §7`  | Resilient Boot with Graceful Degradation                                                          |
| [x]  | `D01 T10 §8`  | Per-Process Page Tables (Minimal Base)                                                            |
| [x]  | `D01 T10 §9`  | CPU Security Activation and Verification                                                          |
| [x]  | `D01 T10 §10` | Boot Order Hardening (Timer-Last, UEFI-Safe)                                                      |
| [x]  | `D01 T10 §11` | ~~`boot.conf` Subsystem Skip List~~                                                               |
| [x]  | `D01 T10 §12` | Logging and Diagnostic Storage Moved to TODO-24                                                   |
| [x]  | `D01 T10 §13` | CPU Feature Minimum Requirements and Verification                                                 |
| [x]  | `D01 T10 §14` | Bare-Metal Test Matrix and Validation Plan                                                        |
| [x]  | `D01 T10 §15` | Boot Splash Spinner Bare-Metal Fix                                                                |
| [x]  | `D01 T10 §16` | Post-Ship Follow-Up Backfill (orphan cohort 2026-07-31)                                           |
| [x]  | `D01 T10 §17` | Emergency-Writer Robustness Residuals (from the §16 review)                                       |
| [x]  | `D01 T10 §18` | Panic-Path Cross-CPU Ownership Residuals (from the §16 review)                                    |
| [ ]  | `D01 T10 §19` | Panic-Path Caller-String Snapshot (from the §17 review)                                           |
| [x]  | `D01 T10 §20` | Panic-Path Serial Ownership Atomicity (from the §18 review)                                       |
| [x]  | `D01 T10 §21` | Live CPU Online Lifecycle (split from §20)                                                        |
| [x]  | `D01 T10 §22` | Dedicated NMI Entry Stub (split from §20)                                                         |
| [x]  | `D01 T10 §23` | Cross-Boot Evidence Lifecycle and Epoch (from §20)                                                |
| [x]  | `D01 T10 §24` | Panic Collector Publication Latency (from §20)                                                    |
| [x]  | `D01 T10 §25` | GS-Validated Per-CPU Identity Cache (split from §20)                                              |
| [x]  | `D01 T10 §26` | Attributable Emergency-Ledger Claim (split from §20)                                              |
| [x]  | `D01 T10 §27` | Async Worker Quiescence and Terminal Bringup Handshake (from the §21 review)                      |
| [x]  | `D01 T10 §28` | Cross-Boot Evidence Durability Across a Hardware Reset (from the §23 review)                      |
| [x]  | `D01 T10 §29` | Abandoned-Reservation Reclamation on a Parking CPU (from the §26 review)                          |
| [x]  | `D01 T10 §30` | Panic-Path States That Are Documented But Not Implemented (from the §29 review)                   |
| [x]  | `D01 T10 §31` | Guard Page Under the BSP Ring-0 Entry Stack                                                       |
| [x]  | `D01 T10 §32` | Guarded Kernel Stack for Phase 0/1 Execution                                                      |
| [x]  | `D01 T11 §1`  | ACPI MADT Parsing                                                                                 |
| [x]  | `D01 T11 §2`  | LAPIC / IOAPIC Init Before PIT                                                                    |
| [x]  | `D01 T11 §3`  | Conditional PIC Disable                                                                           |
| [x]  | `D01 T11 §4`  | Full IDT Coverage                                                                                 |
| [x]  | `D01 T11 §5`  | Dynamic IRQ Registration API                                                                      |
| [x]  | `D01 T11 §6`  | Unified Timer Subsystem (UTS)                                                                     |
| [x]  | `D01 T11 §7`  | LAPIC Timer Calibration                                                                           |
| [x]  | `D01 T11 §8`  | Migrate Boot Splash Spinner Off PIT Callback                                                      |
| [x]  | `D01 T11 §9`  | Boot Time Visualization                                                                           |
| [x]  | `D01 T11 §10` | Remove Hyper-V Debug Workarounds                                                                  |
| [x]  | `D01 T12 §1`  | Entropy Source Inventory and Quality Model                                                        |
| [x]  | `D01 T12 §2`  | EFI_RNG_PROTOCOL Collection                                                                       |
| [x]  | `D01 T12 §3`  | CPU RDRAND/RDSEED Collection                                                                      |
| [x]  | `D01 T12 §4`  | TPM RNG Collection                                                                                |
| [x]  | `D01 T12 §5`  | Boot Timing and Interrupt Jitter Mix-In                                                           |
| [x]  | `D01 T12 §6`  | Seed File Carryover Lifecycle                                                                     |
| [x]  | `D01 T12 §7`  | boot_info Seed Handoff                                                                            |
| [x]  | `D01 T12 §8`  | Kernel Early CSPRNG Seeding                                                                       |
| [x]  | `D01 T12 §9`  | Entropy Diagnostics and Policy Gates                                                              |
| [x]  | `D01 T12 §10` | Entropy Tests                                                                                     |
| [x]  | `D01 T12 §11` | Capability Gate on the Seed Payload Consumer                                                      |
| [ ]  | `D01 T13 §1`  | Harden TCG Event-Log Parser                                                                       |
| [x]  | `D01 T13 §2`  | TPM2 Command Transport                                                                            |
| [x]  | `D01 T13 §3`  | PCR Read API                                                                                      |
| [x]  | `D01 T13 §4`  | PCR Replay Engine                                                                                 |
| [x]  | `D01 T13 §5`  | Secure Boot Variable Measurement Reconciliation                                                   |
| [x]  | `D01 T13 §6`  | Baseline Enrollment and Storage                                                                   |
| [x]  | `D01 T13 §7`  | TPM NV Index Support                                                                              |
| [ ]  | `D01 T13 §8`  | Sealed-Secret Boot Policy Hooks                                                                   |
| [x]  | `D01 T13 §11` | TPM Tests and Event-Log Fixtures                                                                  |
| [x]  | `D01 T13 §12` | PCR Allocation and Policy Mask Table                                                              |
| [ ]  | `D01 T13 §13` | Attestation Key Provisioning and TPM2 Quote                                                       |
| [ ]  | `D01 T13 §9`  | Attestation Report Export                                                                         |
| [x]  | `D01 T13 §14` | Post-Ship Follow-Up Backfill (orphan cohort 2026-07-31)                                           |
| [ ]  | `D01 T13 §15` | Trusted Enrollment Provenance                                                                     |
| [x]  | `D01 T13 §16` | Baseline ABI-Manifest Identity -- Populate the Reserved Digest                                    |
| [x]  | `D01 T13 §17` | Write-Locked and Monotonic NV Index Primitives                                                    |
| [x]  | `D01 T13 §18` | Atomic Boot-Integrity Report Publication                                                          |
| [x]  | `D01 T13 §19` | Versioned Baseline Growth and NV Index Migration                                                  |
| [x]  | `D01 T13 §20` | BOOTX64.EFI On-Disk Self-Measurement                                                              |
| [x]  | `D01 T13 §21` | Authenticated NV Index Lifecycle and Anti-Rollback Trust Anchor                                   |
| [x]  | `D01 T13 §22` | Bootloader-Side NV Floor Read (EFI_TCG2 Adapter)                                                  |
| [x]  | `D01 T13 §23` | Headless Enrollment Authorization Escape Hatch                                                    |
| [x]  | `D01 T13 §24` | Extend the Bounded Sequence and Teardown Proof to Seal and Attestation                            |
| [x]  | `D01 T13 §25` | Field-Level Baseline Mismatch Attribution and Status Scoping                                      |
| [x]  | `D01 T13 §26` | Baseline-Blob NV Fake and Wrapper-Level Verify Coverage                                           |
| [x]  | `D01 T13 §27` | Authorized NV Record Transitions                                                                  |
| [x]  | `D01 T13 §28` | Crash-Consistent Record Pairing, Floor APIs and Verified-Read Boot Budget                         |
| [x]  | `D01 T13 §29` | Authorized-Record Hardening                                                                       |
| [x]  | `D01 T13 §30` | Headless Authorization Transport and Full-Record Binding                                          |
| [x]  | `D01 T13 §31` | Loader Digest Correlation with the Firmware PCR 4 Measurement                                     |
| [ ]  | `D01 T13 §32` | Authorized-Record Coverage Residue on Existing Fixtures                                           |
| [ ]  | `D01 T13 §33` | PCR-Layer Contention Loses Its Retry                                                              |
| [ ]  | `D01 T13 §34` | Headless Authority Lifecycle: Revocation, Durable Verdict and Aggregate Budget                    |
| [ ]  | `D01 T13 §35` | Shared Fake-TIS Fixture Extension and the Coverage It Unlocks                                     |
| [x]  | `D01 T14 §1`  | UEFI Pre-Kernel POST Codes                                                                        |
| [ ]  | `D01 T14 §2`  | Boot Progress Named-Stage API                                                                     |
| [x]  | `D01 T14 §3`  | POST-Style Hex Code Display                                                                       |
| [x]  | `D01 T14 §4`  | Alive Blink / Visual Heartbeat Indicator                                                          |
| [x]  | `D01 T14 §5`  | Panic Forensic Evidence                                                                           |
| [ ]  | `D01 T14 §6`  | Panic QR Code                                                                                     |
| [ ]  | `D01 T14 §8`  | Runtime Vital Signs Strip                                                                         |
| [x]  | `D01 T14 §9`  | Boot Timeline Visualization and Import Parity                                                     |
| [x]  | `D01 T14 §10` | Bootloader Build Identity Dump in BlackBox                                                        |
| [x]  | `D01 T14 §11` | Boot Load Status Log (ntbtlog Parity)                                                             |
| [x]  | `D01 T14 §12` | Boot Load Status Granularity (per-driver records, probe aggregation, NVMe partial init)           |
| [x]  | `D01 T14 §14` | Bootloader Reservation of the Panic-Evidence Page (`0x80000`)                                     |
| [x]  | `D01 T14 §16` | Boot Timeline Export Formats (SVG and Chrome Trace)                                               |
| [x]  | `D01 T14 §18` | Pre-Serial `serial_early_print` Drives a Poisoned UART Port                                       |
| [x]  | `D01 T14 §20` | The `.bss` Poison Discipline Stops at the Serial Path                                             |
| [ ]  | `D01 T15 §1`  | 4-Digit POST Code System (0x0000-0xFFFF)                                                          |
| [ ]  | `D01 T15 §2`  | POST Codes in Every Boot Function                                                                 |
| [ ]  | `D01 T15 §3`  | Embedded 5x7 Bitmap Micro-Font                                                                    |
| [ ]  | `D01 T15 §4`  | Tier 1: Pre-Splash VPD Renderer                                                                   |
| [x]  | `D01 T15 §5`  | `boot.conf` `postbars` Configuration                                                              |
| [x]  | `D01 T15 §6`  | Named Stages with TSC Timing                                                                      |
| [x]  | `D01 T15 §7`  | Status Indicators and Progress Bar                                                                |
| [ ]  | `D01 T15 §8`  | NVRAM Crash Persistence and "Last Boot Failed" Display                                            |
| [/]  | `D01 T15 §9`  | Tier 2: Splash-Integrated Progress                                                                |
| [x]  | `D01 T15 §10` | Seamless Tier Transition                                                                          |
| [x]  | `D01 T15 §11` | Phase Grouping and Diagnostic Layout                                                              |
| [x]  | `D01 T15 §12` | HV_BAR Removal and Migration                                                                      |
| [/]  | `D01 T15 §13` | Panic Integration and Failure Highlighting                                                        |
| [x]  | `D01 T16 §1`  | NVMe Controller Discovery and BAR Mapping                                                         |
| [x]  | `D01 T16 §2`  | Admin Queue Setup and Identify Commands                                                           |
| [x]  | `D01 T16 §3`  | I/O Queue Creation and Sector Read/Write                                                          |
| [x]  | `D01 T16 §4`  | Block Device Registration and VFS Integration                                                     |
| [x]  | `D01 T16 §5`  | Advanced NVMe parity backlog (D04 T08 owned)                                                      |
| [x]  | `D01 T16 §6`  | NVMe Controller Lifecycle: Clean Shutdown, Cache Flush, and I/O Validation                        |
| [x]  | `D01 T17 §1`  | xHCI Controller Bring-Up and Port Scan                                                            |
| [x]  | `D01 T17 §2`  | USB Device Enumeration and Configuration                                                          |
| [x]  | `D01 T17 §3`  | USB MSC BOT (Bulk-Only Transport) Driver                                                          |
| [x]  | `D01 T17 §4`  | Block Device Registration and VFS Integration                                                     |
| [x]  | `D01 T17 §5`  | Interrupt-Driven Hot-Plug and Post-Boot Lifecycle                                                 |
| [/]  | `D01 T17 §6`  | Hardware Compatibility -- Non-Intel xHCI Vendors                                                  |
| [x]  | `D01 T18 §1`  | xHCI Interrupt Endpoint Setup                                                                     |
| [x]  | `D01 T18 §2`  | Interrupt Transfer Polling                                                                        |
| [x]  | `D01 T18 §3`  | USB HID Boot-Protocol Keyboard Driver                                                             |
| [x]  | `D01 T18 §4`  | USB HID Boot-Protocol Mouse Driver                                                                |
| [x]  | `D01 T18 §5`  | Input Source Coexistence                                                                          |
| [x]  | `D01 T18 §6`  | Hot-Plug Keyboard/Mouse Detection                                                                 |
| [x]  | `D01 T18 §7`  | USB Input Diagnostic Logging                                                                      |
| [x]  | `D01 T19 §1`  | SCSI REQUEST SENSE and Error Classification                                                       |
| [x]  | `D01 T19 §2`  | TEST UNIT READY Poll Loop                                                                         |
| [x]  | `D01 T19 §3`  | MSC BOT Retry on Transient Errors                                                                 |
| [x]  | `D01 T19 §4`  | USB Transport Error Recovery (Stall/Halt Handling)                                                |
| [ ]  | `D01 T19 §5`  | Bulk Transfer Timeouts                                                                            |
| [ ]  | `D01 T19 §7`  | XUSB2PR Port-Ready Polling (Intel EHCI→xHCI)                                                      |
| [/]  | `D01 T19 §6`  | Remove sleep_ms(2000) Hack                                                                        |
| [x]  | `D01 T19 §8`  | klog_disk_flush Bounded Loop                                                                      |
| [x]  | `D01 T19 §9`  | klog Deferred Flush Mode                                                                          |
| [x]  | `D01 T19 §10` | Boot Media Speed Detection                                                                        |
| [x]  | `D01 T19 §11` | EHCI/UHCI Companion Controller Fallback                                                           |
| [x]  | `D01 T19 §12` | USB Boot Diagnostic Report                                                                        |
| [x]  | `D01 T19 §13` | Single-Pass Per-Subsystem Log Routing                                                             |
| [x]  | `D01 T19 §14` | IXFS Boot Tests: Slow-Media-Aware                                                                 |
| [ ]  | `D01 T19 §15` | Flush Progress on Splash Diagnostic Line                                                          |
| [x]  | `D01 T19 §16` | xHCI Command Ring + BOT Transport SMP Serialization                                               |
| [ ]  | `D01 T20 §1`  | Bootloader Allocates xHCI DMA Structures                                                          |
| [ ]  | `D01 T20 §2`  | Bootloader Performs USBLEGSUP + Controller Takeover                                               |
| [/]  | `D01 T20 §3`  | Bootloader Enumerates Devices with Persistent State                                               |
| [/]  | `D01 T20 §4`  | boot_info Passes Controller + Device DMA State                                                    |
| [ ]  | `D01 T20 §5`  | Kernel Inherits Controller Without Halt/Reset                                                     |
| [ ]  | `D01 T20 §6`  | Kernel Registers MSC Devices from boot_info Geometry                                              |
| [ ]  | `D01 T20 §7`  | Fallback: Detect Corrupt State, Revert to TODO-17 Path                                            |
| [x]  | `D01 T21 §1`  | Boot Metadata Structure                                                                           |
| [x]  | `D01 T21 §2`  | Dual-Slot Disk Layout                                                                             |
| [x]  | `D01 T21 §3`  | Bootloader Slot Selection                                                                         |
| [x]  | `D01 T21 §4`  | Boot Failure Counting and Rollback                                                                |
| [x]  | `D01 T21 §5`  | Kernel `mark_boot_successful()`                                                                   |
| [x]  | `D01 T21 §6`  | Slot Status in Boot Diagnostics                                                                   |
| [x]  | `D01 T21 §7`  | Boot Metadata Integrity and Atomic Writes                                                         |
| [x]  | `D01 T21 §8`  | Per-Slot Anti-Rollback Version Floor                                                              |
| [x]  | `D01 T22 §1`  | Recovery Partition in Disk Layout                                                                 |
| [ ]  | `D01 T22 §2`  | Recovery Bootloader                                                                               |
| [ ]  | `D01 T22 §3`  | Filesystem Integrity Check                                                                        |
| [ ]  | `D01 T22 §4`  | Backup Kernel Restore                                                                             |
| [ ]  | `D01 T22 §5`  | Boot Metadata Reset                                                                               |
| [ ]  | `D01 T22 §6`  | NVRAM Boot Entry Reconstruction                                                                   |
| [ ]  | `D01 T22 §7`  | Recovery UI                                                                                       |
| [ ]  | `D01 T22 §8`  | Gate IXFS fsck Repairs on Complete, Readable Scans                                                |
| [ ]  | `D01 T13 §10` | Recovery and Mismatch UX                                                                          |
| [x]  | `D01 T23 §1`  | Software Watchdog via LAPIC NMI Timer                                                             |
| [x]  | `D01 T23 §2`  | Per-Phase Timeout Configuration                                                                   |
| [x]  | `D01 T23 §3`  | Watchdog Pet at boot_progress()                                                                   |
| [x]  | `D01 T23 §4`  | Watchdog-Triggered Reboot with Diagnostics                                                        |
| [x]  | `D01 T23 §5`  | ACPI WDAT Hardware Watchdog                                                                       |
| [x]  | `D01 T23 §6`  | Watchdog Status in VPD                                                                            |
| [ ]  | `D01 T23 §7`  | Post-Ship Follow-Up Backfill (orphan cohort 2026-07-31)                                           |
| [x]  | `D01 T24 §1`  | Disk Tooling -- Add BlackBox Partition to GPT Layout                                              |
| [x]  | `D01 T24 §2`  | Build Pipeline -- Format and Populate BlackBox                                                    |
| [x]  | `D01 T24 §3`  | Kernel Mount -- Discover "BlackBox" GPT Name, Mount as X:\                                        |
| [x]  | `D01 T24 §4`  | Directory Skeleton -- Create Dirs on First Boot                                                   |
| [ ]  | `D01 T24 §5`  | Klog Migration -- Move All Log Output to X:\Logs\                                                 |
| [x]  | `D01 T24 §6`  | Boot Logs -- Per-Boot Session Files to X:\Boot\                                                   |
| [x]  | `D01 T24 §7`  | Crash Dump Path -- crash_recovery.log to X:\Crash\                                                |
| [x]  | `D01 T24 §8`  | Perf and Diag -- boot-profile, hwdump to X:\Perf\ and X:\Diag\                                    |
| [x]  | `D01 T24 §9`  | Host Tools -- SDK Reads BlackBox from Disk Image                                                  |
| [ ]  | `D01 T24 §10` | Disk Space Management -- Log Aging, Quota, Cleanup                                                |
| [x]  | `D01 T24 §11` | FAT32 Volume Label -- Set "BLACKBOX" at Format Time                                               |
| [x]  | `D01 T24 §12` | Partition Health -- fsck on Mount, Dirty-Bit Check                                                |
| [ ]  | `D01 T24 §13` | WER Staging Area -- Error Reports in X:\Crash\WER\                                                |
| [x]  | `D01 T24 §14` | A/B Layout Compatibility -- 4+ Partition Coexistence                                              |
| [/]  | `D01 T24 §15` | Boot Platform TODO Updates -- XREFs and Domain Sync                                               |
| [x]  | `D01 T24 §16` | Crash/WER Report Retention in Partition Cleanup                                                   |
| [x]  | `D01 T25 §1`  | UEFI SNP/PXE Protocol Discovery                                                                   |
| [ ]  | `D01 T25 §2`  | DHCP/PXE Provenance Capture                                                                       |
| [x]  | `D01 T25 §3`  | TFTP Kernel and boot.conf Load                                                                    |
| [x]  | `D01 T25 §4`  | UEFI HTTP Boot Load Path                                                                          |
| [/]  | `D01 T25 §5`  | Network Boot Asset Integrity                                                                      |
| [ ]  | `D01 T25 §6`  | boot_info Network Provenance                                                                      |
| [x]  | `D01 T25 §7`  | Fallback Ordering with Local Media                                                                |
| [x]  | `D01 T25 §8`  | Recovery and Installer over Network                                                               |
| [ ]  | `D01 T25 §9`  | Network Boot Diagnostics and BlackBox Report                                                      |
| [ ]  | `D01 T25 §10` | PXE/HTTP Boot Tests                                                                               |
| [x]  | `D01 T26 §1`  | Hibernation Image Metadata Format                                                                 |
| [x]  | `D01 T26 §2`  | Bootloader Image Discovery                                                                        |
| [x]  | `D01 T26 §3`  | Resume Eligibility Policy                                                                         |
| [x]  | `D01 T26 §4`  | Integrity and Version Validation                                                                  |
| [x]  | `D01 T26 §5`  | boot_info Resume Handoff                                                                          |
| [x]  | `D01 T26 §6`  | Resume Failure Fallback                                                                           |
| [x]  | `D01 T26 §7`  | Fast Startup Mode                                                                                 |
| [x]  | `D01 T26 §8`  | Diagnostics and BlackBox Resume Report                                                            |
| [x]  | `D01 T26 §9`  | Resume Tests                                                                                      |
| [x]  | `D01 T27 §1`  | UEFI Multi-OS Detection and Chainload Entries                                                     |
| [x]  | `D01 T27 §2`  | Firmware Update Advisor (read-only LVFS-style)                                                    |
| [ ]  | `D01 T27 §3`  | UEFI Memory Attributes (W^X)                                                                      |
| [x]  | `D01 T27 §4`  | Multi-GPU GOP Handle Enumeration                                                                  |
| [x]  | `D01 T27 §5`  | Secure Boot Enforcement Policy                                                                    |
| [x]  | `D01 T27 §6`  | SMBIOS Extended Type Parsing                                                                      |
| [/]  | `D01 T27 §7`  | DBX Revocation List Sync                                                                          |
| [ ]  | `D01 T27 §9`  | Stable Ids for Discovered OS Entries                                                              |
| [x]  | `D01 T28 §1`  | Boot Certification Matrix Schema                                                                  |
| [x]  | `D01 T28 §2`  | VM Automation Suite                                                                               |
| [ ]  | `D01 T28 §3`  | Storage/Media Boot Suite                                                                          |
| [ ]  | `D01 T28 §4`  | Security Boot Suite                                                                               |
| [ ]  | `D01 T28 §5`  | Recovery and Rollback Suite                                                                       |
| [x]  | `D01 T28 §6`  | Network Boot Suite                                                                                |
| [x]  | `D01 T28 §7`  | Bare-Metal Lab Inventory                                                                          |
| [ ]  | `D01 T28 §8`  | Boot Support Bundle Collector                                                                     |
| [ ]  | `D01 T28 §9`  | Release Gate and Dashboard                                                                        |
| [ ]  | `D01 T28 §10` | Certification Docs                                                                                |
| [ ]  | `D01 T28 §11` | Firmware Sanity Certification Gate                                                                |
| [ ]  | `D01 T28 §12` | Error-Screen Pixel Oracle -- Assert What Was Drawn, Not What the Loader Says It Drew              |
| [x]  | `D01 T29 §1`  | Per-Phase Boot Perf Budgets and Threshold Alarms                                                  |
| [x]  | `D01 T29 §2`  | Boot Health Audit JSON (`X:\Diag\boot-health.json`)                                               |
| [x]  | `D01 T29 §3`  | Boot Perf Trend File + Regression Detection                                                       |
| [x]  | `D01 T29 §4`  | SMBIOS Init Profiling + Optimization                                                              |
| [x]  | `D01 T29 §5`  | MAT W^X Violation Root-Cause Attribution                                                          |
| [x]  | `D01 T29 §6`  | Mouse PS/2 Init Profiling + Optimization                                                          |
| [ ]  | `D01 T29 §7`  | Async Font and Icon Loader (Post-Desktop-Ready)                                                   |
| [/]  | `D01 T29 §8`  | Boot Heartbeat Telemetry During Long Phases                                                       |
| [ ]  | `D01 T29 §9`  | EXEC Step Latency Profile                                                                         |
| [ ]  | `D01 T29 §10` | UEFI RT SetVariable Latency                                                                       |
| [ ]  | `D01 T29 §11` | Phase-3 X:\Diag JSON Writer Batching                                                              |
| [ ]  | `D01 T29 §12` | AVX-512 Throttle Policy When APERF/MPERF Unavailable                                              |
| [/]  | `D01 T29 §13` | Boot History Ring Depth + Format                                                                  |
| [x]  | `D01 T29 §14` | FAT32 Dirty-Mount fsck Cost in VFS Step                                                           |
| [ ]  | `D01 T29 §15` | User-Mode Binary Spawn Latency (~1s task_create → ELF entry)                                      |
| [ ]  | `D01 T29 §16` | TSC Frequency Variability Under Hypervisor                                                        |
| [ ]  | `D01 T29 §17` | PAT WC -> WT Hypervisor Trap Quirk                                                                |
| [ ]  | `D01 T29 §18` | Boot Critical-Path / Dependency / Resource-Wait Attribution                                       |
| [ ]  | `D01 T29 §20` | Boot Trend JSON Writer Hardening                                                                  |
| [ ]  | `D01 T29 §21` | Per-Driver Degraded-State Registry for Storage                                                    |

### Phase 2 -- Kernel core: objects, processes, syscalls, the registry and executables

The kernel's own model comes straight after boot because every driver, filesystem and user-mode API is written against it: the object manager, processes and threads, the syscall tables, security, the registry, exceptions and the binary loaders.

| Done | Section       | Deliverable                                                                                       |
| :--: | ------------- | ------------------------------------------------------------------------------------------------- |
| [x]  | `D02 T01 §1`  | Boot Init Infrastructure                                                                          |
| [x]  | `D02 T01 §2`  | Phase 0 -- Critical Init (Interrupts Disabled)                                                    |
| [x]  | `D02 T01 §3`  | Phase 1 -- Platform Services (Interrupts Enabled at End)                                          |
| [ ]  | `D02 T01 §4`  | Phase 2 -- System Services                                                                        |
| [x]  | `D02 T01 §5`  | Phase 3 -- User Platform                                                                          |
| [ ]  | `D02 T01 §6`  | Dependency Gates                                                                                  |
| [ ]  | `D02 T01 §7`  | Failure Policy                                                                                    |
| [x]  | `D02 T01 §8`  | Code Cleanup                                                                                      |
| [ ]  | `D02 T01 §9`  | Degraded-Boot Recovery Screen                                                                     |
| [x]  | `D02 T01 §10` | POST Code + UEFI Variable Log                                                                     |
| [ ]  | `D02 T01 §11` | Deferred Init for Non-Critical Subsystems                                                         |
| [ ]  | `D02 T01 §12` | Boot Performance Regression Detection                                                             |
| [ ]  | `D02 T01 §13` | Async Subsystem Init (SMP Parallel)                                                               |
| [x]  | `D02 T02 §1`  | Boot Argument Schema and Parser                                                                   |
| [x]  | `D02 T02 §2`  | `kernel_config_t` Immutable Phase 0 Snapshot                                                      |
| [x]  | `D02 T02 §3`  | Registry-Backed Policy Merge                                                                      |
| [x]  | `D02 T02 §4`  | ControlSet and LastKnownGood Selection                                                            |
| [x]  | `D02 T02 §5`  | Safe Mode and Recovery Policy Object                                                              |
| [x]  | `D02 T02 §6`  | Runtime Tunable Registry                                                                          |
| [x]  | `D02 T02 §7`  | Feature Flag Gates and Experiment Cohorts                                                         |
| [x]  | `D02 T02 §8`  | Native Query/Set Config Syscalls                                                                  |
| [x]  | `D02 T02 §9`  | Policy Lock Phases and Tamper Audit                                                               |
| [x]  | `D02 T02 §10` | Boot Status Policy and Boot Success Ledger                                                        |
| [x]  | `D02 T02 §11` | Config Dump, Tests, and Docs                                                                      |
| [ ]  | `D02 T02 §12` | Post-Ship Follow-Up Backfill (orphan cohort 2026-07-31)                                           |
| [ ]  | `D02 T03 §1`  | Freestanding String Library                                                                       |
| [ ]  | `D02 T03 §2`  | Complete Floating-Point Math Library                                                              |
| [x]  | `D02 T03 §3`  | LZ4 Block Compressor                                                                              |
| [x]  | `D02 T03 §4`  | miniz Deflate / Inflate + ZIP                                                                     |
| [x]  | `D02 T03 §5`  | Monocypher Crypto Primitives + Kernel CSPRNG                                                      |
| [ ]  | `D02 T03 §6`  | cJSON DOM Parser                                                                                  |
| [x]  | `D02 T03 §7`  | Mbed TLS Freestanding Port                                                                        |
| [x]  | `D02 T03 §8`  | Checksum and Codec Dispatch (CRC32 / CRC32C / base64 / hex)                                       |
| [x]  | `D02 T03 §9`  | Zstandard (zstd) Compression                                                                      |
| [x]  | `D02 T03 §10` | SHA-3 / SHAKE (Keccak) Hash Family                                                                |
| [x]  | `D02 T04 §1`  | Boot-Phase Aware klog Init                                                                        |
| [ ]  | `D02 T04 §2`  | Per-Subsystem Log Splitting                                                                       |
| [x]  | `D02 T04 §3`  | Per-Subsystem Verbosity Control                                                                   |
| [ ]  | `D02 T04 §4`  | Log Rotation                                                                                      |
| [x]  | `D02 T04 §5`  | Rate Limiting                                                                                     |
| [x]  | `D02 T04 §6`  | Structured JSON Log Events                                                                        |
| [x]  | `D02 T04 §7`  | ETW Tracing Syscalls Wired to SSDT                                                                |
| [ ]  | `D02 T04 §8`  | Crash-Persistent Log Capture                                                                      |
| [x]  | `D02 T04 §9`  | Per-Entry Context Metadata                                                                        |
| [/]  | `D02 T04 §10` | Log Integrity Verification                                                                        |
| [/]  | `D02 T04 §11` | ETW Provider Registration and Session Filtering                                                   |
| [x]  | `D02 T04 §12` | ETW Advanced Capture -- Stack-Walk, Autologger, Self-Describing Schema                            |
| [x]  | `D02 T04 §13` | Rotated-Log Compression                                                                           |
| [ ]  | `D02 T04 §14` | Serial Timestamp Render Bound                                                                     |
| [ ]  | `D02 T04 §15` | Post-Ship Follow-Up Backfill (orphan cohort 2026-07-31)                                           |
| [ ]  | `D02 T04 §16` | Klog Assertions and Scans That Depend on Nothing Else Having Logged                               |
| [ ]  | `D02 T04 §17` | Bounded Wait Until the Sinks Have Caught Up to a Given Sequence                                   |
| [ ]  | `D02 T05 §1`  | OBJECT_HEADER and OBJECT_TYPE Infrastructure                                                      |
| [x]  | `D02 T05 §2`  | Reference Counting and Object Lifetime                                                            |
| [ ]  | `D02 T05 §3`  | Per-Process Handle Table                                                                          |
| [ ]  | `D02 T05 §4`  | Object Namespace                                                                                  |
| [ ]  | `D02 T05 §5`  | File, Process, Thread object types                                                                |
| [ ]  | `D02 T05 §6`  | Synchronisation Object Types                                                                      |
| [ ]  | `D02 T05 §7`  | Section (Shared Memory) Object Type                                                               |
| [x]  | `D02 T05 §8`  | Security Descriptor Integration                                                                   |
| [ ]  | `D02 T05 §10` | Handle Inheritance Across CreateProcess                                                           |
| [ ]  | `D02 T05 §11` | Unified Kernel-User Namespace Browser API                                                         |
| [x]  | `D02 T05 §12` | Per-Type Object and Handle Statistics                                                             |
| [x]  | `D02 T05 §13` | Object Callbacks -- Handle Operation Filtering                                                    |
| [ ]  | `D02 T05 §14` | Per-Process Handle Quota                                                                          |
| [ ]  | `D02 T05 §15` | Handle Tracing and Leak Detection                                                                 |
| [x]  | `D02 T06 §1`  | Executive Headers and Namespace                                                                   |
| [x]  | `D02 T06 §2`  | Interlocked SLIST                                                                                 |
| [x]  | `D02 T06 §3`  | Rundown Protection                                                                                |
| [x]  | `D02 T06 §4`  | Callback Objects                                                                                  |
| [x]  | `D02 T06 §5`  | Lookaside Lists (NPaged, Paged, and Ex)                                                           |
| [x]  | `D02 T06 §6`  | Fast References                                                                                   |
| [x]  | `D02 T06 §7`  | Generic Tables and Bitmaps                                                                        |
| [x]  | `D02 T06 §8`  | Push Locks                                                                                        |
| [x]  | `D02 T06 §9`  | Fast and Guarded Mutexes                                                                          |
| [x]  | `D02 T06 §10` | Executive Resource Wrapper                                                                        |
| [x]  | `D02 T06 §11` | Run-Once Initialization                                                                           |
| [x]  | `D02 T06 §12` | Worker Items, Delayed Work, and Ex Timers                                                         |
| [x]  | `D02 T06 §13` | Bugcheck Reason Callbacks                                                                         |
| [x]  | `D02 T06 §14` | Executive Verifier Hooks                                                                          |
| [x]  | `D02 T07 §1`  | `KIRQL` Type, Constants, and Core Contract                                                        |
| [x]  | `D02 T07 §2`  | Per-CPU IRQL Tracking and Transition Primitives                                                   |
| [ ]  | `D02 T07 §3`  | Interrupt Entry/Exit IRQL Integration                                                             |
| [x]  | `D02 T07 §4`  | DPC Object Type and Per-CPU Queue                                                                 |
| [x]  | `D02 T07 §5`  | DPC Drain Loop at `DISPATCH_LEVEL`                                                                |
| [ ]  | `D02 T07 §6`  | Timer/APIC Scheduling Path for DPC Dispatch                                                       |
| [x]  | `D02 T07 §7`  | DPC Targeting, Importance, and Flush                                                              |
| [x]  | `D02 T07 §8`  | Threaded DPCs (`PASSIVE_LEVEL` DPC Variant)                                                       |
| [ ]  | `D02 T07 §9`  | Timer-DPC Association                                                                             |
| [ ]  | `D02 T07 §10` | Driver Migration and Workqueue Contract Split                                                     |
| [ ]  | `D02 T07 §11` | APC Object Type and Per-Thread Queues                                                             |
| [ ]  | `D02 T07 §12` | APC Delivery Mechanism (KiDeliverApc)                                                             |
| [ ]  | `D02 T07 §13` | IRQL Violation Traps and Structured Telemetry                                                     |
| [ ]  | `D02 T07 §14` | Budgeted DPC/APC Fairness and Starvation Watchdog                                                 |
| [ ]  | `D02 T07 §15` | Threaded DPC List Synchronization                                                                 |
| [x]  | `D02 T07 §16` | KeFlushQueuedDpcs Threaded DPC Completion                                                         |
| [x]  | `D02 T07 §17` | Per-CPU Threaded DPC Worker Affinity                                                              |
| [x]  | `D02 T07 §18` | System Worker Thread Pool (Long-Period Periodic Callbacks)                                        |
| [x]  | `D02 T07 §19` | Interference-Proof DPC Depth-Accounting Test Seam                                                 |
| [x]  | `D02 T08 §1`  | `FILETIME` Type, Epoch Constants, and Conversion Math                                             |
| [x]  | `D02 T08 §2`  | Monotonic Nanosecond Clock Source Selection                                                       |
| [x]  | `D02 T08 §3`  | Invariant TSC Detection and Per-CPU Offset Calibration                                            |
| [x]  | `D02 T08 §4`  | HPET Standalone Driver                                                                            |
| [x]  | `D02 T08 §5`  | Wall Clock Init from UEFI GetTime / RTC                                                           |
| [x]  | `D02 T08 §6`  | Kernel Time Service                                                                               |
| [x]  | `D02 T08 §7`  | Interrupt Time and Unbiased Interrupt Time APIs                                                   |
| [x]  | `D02 T08 §8`  | Timer Resolution Management                                                                       |
| [x]  | `D02 T08 §9`  | `NtQuerySystemTime`, `NtSetSystemTime`, `NtQueryPerformanceCounter`                               |
| [x]  | `D02 T08 §10` | Precise System Time (`KeQuerySystemTimePrecise`)                                                  |
| [x]  | `D02 T08 §11` | Timezone Bias and DST Management                                                                  |
| [x]  | `D02 T08 §12` | KUSER_SHARED_DATA Time Field Updates                                                              |
| [x]  | `D02 T08 §13` | Filesystem Timestamp Encoding (FAT32 + NTFS)                                                      |
| [x]  | `D02 T08 §14` | Suspend/Hibernate Time Bias Tracking                                                              |
| [x]  | `D02 T08 §15` | Leap Second Policy                                                                                |
| [x]  | `D02 T08 §16` | Coarse Time Fast Path                                                                             |
| [x]  | `D02 T08 §17` | NTP Clock Adjustment Hooks                                                                        |
| [ ]  | `D02 T08 §18` | Clocksource Quality Watchdog                                                                      |
| [x]  | `D02 T08 §19` | NTP Continuous Wall-Time Discipline                                                               |
| [x]  | `D02 T09 §1`  | XSAVE / XRSTOR State Management                                                                   |
| [x]  | `D02 T09 §2`  | AVX/AVX2 Kernel Memops + Framebuffer Blit                                                         |
| [x]  | `D02 T09 §3`  | AVX-512 Opt-In + Future Silicon Detection                                                         |
| [x]  | `D02 T09 §4`  | MSR Management Infrastructure                                                                     |
| [x]  | `D02 T09 §5`  | UMIP + PKU Protection Keys                                                                        |
| [x]  | `D02 T09 §6`  | 1 GiB Huge Pages + Write-Combining PAT                                                            |
| [x]  | `D02 T09 §7`  | FRED Unified Event Delivery                                                                       |
| [x]  | `D02 T09 §8`  | LKGS Fast GS-Base Swap                                                                            |
| [x]  | `D02 T09 §9`  | CPU Topology: Zen Chiplets + Intel Hybrid                                                         |
| [x]  | `D02 T09 §10` | Performance Monitoring Counters: Intel + AMD                                                      |
| [x]  | `D02 T09 §11` | OSVW Errata + RDTSCP Processor ID Setup                                                           |
| [x]  | `D02 T09 §12` | AMD IBS Profiling (Stretch)                                                                       |
| [x]  | `D02 T09 §13` | Virtualization Detection (AMD-V + Intel VT-x)                                                     |
| [ ]  | `D02 T09 §14` | Boot Self-Benchmark + Auto-Tune                                                                   |
| [x]  | `D02 T09 §15` | Future Silicon Extensions                                                                         |
| [x]  | `D02 T09 §16` | Boot Page Tables: 1 GiB Pages From entry.asm                                                      |
| [x]  | `D02 T09 §17` | AMX Tile State + XFD Dynamic XSAVE                                                                |
| [ ]  | `D02 T09 §18` | Split-Lock + Bus-Lock Detection                                                                   |
| [x]  | `D02 T09 §19` | Additional CPU Feature Adoption: WAITPKG, SERIALIZE, RDPID                                        |
| [ ]  | `D02 T09 §20` | Post-Ship Follow-Up Backfill (orphan cohort 2026-07-31)                                           |
| [x]  | `D02 T10 §1`  | NX Bit: EFER.NXE + PTE NX on All Non-Code Mappings                                                |
| [x]  | `D02 T10 §2`  | SMEP & SMAP: CR4 Activation + CLAC/STAC Wrappers                                                  |
| [x]  | `D02 T10 §3`  | KPTI Trampoline Page + Per-CPU CR3 Fields                                                         |
| [x]  | `D02 T10 §4`  | KPTI SYSCALL CR3 Swap                                                                             |
| [x]  | `D02 T10 §5`  | KPTI IDT CR3 Swap (All 256 Vectors)                                                               |
| [x]  | `D02 T10 §6`  | KPTI User CR3 Allocation + Context Switch                                                         |
| [x]  | `D02 T10 §7`  | PCID: TLB Tagging for No-Flush CR3 Switch                                                         |
| [x]  | `D02 T10 §8`  | Spectre Mitigations: IBRS/IBPB + Retpoline                                                        |
| [x]  | `D02 T10 §9`  | CET Shadow Stack (Kernel Ring 0)                                                                  |
| [x]  | `D02 T10 §10` | CET Indirect Branch Tracking (IBT / ENDBR64)                                                      |
| [x]  | `D02 T10 §11` | Kernel Heap Hardening: Cookies & Redzones                                                         |
| [x]  | `D02 T10 §12` | Stack Canaries (`-fstack-protector-strong`)                                                       |
| [ ]  | `D02 T10 §13` | Kernel Stack Guard Pages                                                                          |
| [x]  | `D02 T10 §14` | KASLR: RDRAND Kernel Load Address                                                                 |
| [x]  | `D02 T10 §15` | Enclave and Code Signing Syscalls Wired to SSDT                                                   |
| [ ]  | `D02 T10 §16` | Secure Boot Lockdown Enforcement                                                                  |
| [ ]  | `D02 T10 §17` | Kernel-Image W^X: .text Read-Only + .rodata RO-After-Init                                         |
| [x]  | `D02 T10 §18` | CPU Feature-Flag 128-bit Expansion (`cpu_feature_mask_t`)                                         |
| [x]  | `D02 T10 §19` | Microarchitectural Data-Sampling Clears (MDS / TAA / SRBDS / MMIO)                                |
| [ ]  | `D02 T10 §20` | kCFI: Software Control-Flow Integrity for Indirect Calls                                          |
| [ ]  | `D02 T10 §21` | FORTIFY_SOURCE: Bounds-Checked str/mem Builtins                                                   |
| [ ]  | `D02 T10 §22` | stackleak: Erase Kernel Stack on Return to User                                                   |
| [ ]  | `D02 T10 §23` | KFENCE: Sampling Use-After-Free / Out-of-Bounds Detector                                          |
| [x]  | `D02 T10 §24` | Mitigation Visibility: Queryable Security Posture                                                 |
| [x]  | `D02 T10 §25` | Spectre Predictor-Policy Mitigations: SSBD, STIBP, RSB, BHI, ITS, Retbleed                        |
| [x]  | `D02 T10 §26` | Release-Build Test-Surface Exclusion                                                              |
| [x]  | `D02 T10 §27` | Release-Flavor Proof: Seam-Inventory Gate and CI Attestation                                      |
| [x]  | `D02 T10 §28` | Test-Only Translation Units Outside `src/kernel/test/`                                            |
| [x]  | `D02 T10 §29` | Guard Unguarded Test-Only Helper Functions in Production TUs                                      |
| [x]  | `D02 T10 §30` | Signed CI Attestation for the Release-Flavor Proof Gate                                           |
| [ ]  | `D02 T10 §31` | Legacy-Syscall User-Pointer Validation                                                            |
| [ ]  | `D02 T10 §32` | Post-Ship Follow-Up Backfill (orphan cohort 2026-07-31)                                           |
| [x]  | `D02 T11 §1`  | TEB Struct and GS Self-Pointer                                                                    |
| [x]  | `D02 T11 §2`  | PEB Struct and RTL_USER_PROCESS_PARAMETERS                                                        |
| [x]  | `D02 T11 §3`  | swapgs on INT 0x80 Entry and Exit                                                                 |
| [x]  | `D02 T11 §4`  | KERNEL_GS_BASE Written at task_exec and Fork                                                      |
| [x]  | `D02 T11 §5`  | PEB Allocation and Population at task_exec                                                        |
| [x]  | `D02 T11 §6`  | TEB Allocation and Population at Thread Create                                                    |
| [x]  | `D02 T11 §7`  | Initial User Stack Frame                                                                          |
| [x]  | `D02 T11 §8`  | PEB Ldr (Module List) Basic Population                                                            |
| [x]  | `D02 T11 §9`  | TLS Slot Allocation (64 Static Slots)                                                             |
| [x]  | `D02 T11 §10` | PEB / TEB Exposed in Ob Namespace                                                                 |
| [ ]  | `D02 T11 §11` | KUSER_SHARED_DATA -- Kernel-User Shared Page                                                      |
| [x]  | `D02 T11 §12` | TLS Expansion Slots (1024 Dynamic Slots)                                                          |
| [x]  | `D02 T11 §13` | Extended Auxiliary Vector (AT_RANDOM, AT_PHDR, AT_PHNUM)                                          |
| [x]  | `D02 T11 §14` | User-Mode Thread Bootstrap (uthread_create)                                                       |
| [x]  | `D02 T11 §15` | Per-thread TEB Allocation at uthread_create()                                                     |
| [/]  | `D02 T11 §17` | PEB x64 Version-Field Offsets                                                                     |
| [ ]  | `D02 T11 §18` | Paranoid swapgs Entry for NMI / #DF / #MCE                                                        |
| [/]  | `D02 T11 §19` | exec/fork/switch GS-base Staging Correctness and Cost                                             |
| [/]  | `D02 T11 §16` | TEB Multi-Page Mapping and User-VA Non-Overlap                                                    |
| [ ]  | `D02 T11 §20` | PEB Allocation Robustness and Win32 ABI Handoff                                                   |
| [/]  | `D02 T11 §21` | Format-Specific User Startup Frame (ELF / PE / EIF) + Env Unification                             |
| [ ]  | `D02 T11 §22` | PEB Ldr Module Identity (DllBase / SizeOfImage / Name)                                            |
| [/]  | `D02 T11 §23` | Per-thread TLS Value Storage (Static + Expansion)                                                 |
| [ ]  | `D02 T11 §24` | PEB/TEB Ob-Namespace Exposure: Safe Wrapper Objects + Lifecycle                                   |
| [ ]  | `D02 T11 §25` | TLS Expansion Mapping Safety and Alloc Race                                                       |
| [ ]  | `D02 T11 §26` | auxv Hardening: Classified AT_RANDOM + Validated AT_PHDR                                          |
| [ ]  | `D02 T11 §27` | uthread_create Robustness: Guard-page Cleanup + tid Reservation + Stack-size Validation           |
| [ ]  | `D02 T11 §28` | Intermittent TEB-without-kernel_gs_base Halt at Ring-3 Entry                                      |
| [x]  | `D02 T12 §1`  | NTSTATUS Type and Canonical Status Codes                                                          |
| [x]  | `D02 T12 §2`  | SYSCALL/SYSRET Fast Path                                                                          |
| [ ]  | `D02 T12 §3`  | INT 0x2E Compatibility Path                                                                       |
| [x]  | `D02 T12 §4`  | System Service Descriptor Table (SSDT)                                                            |
| [x]  | `D02 T12 §5`  | Nt/Zw Naming and Existing Syscall Migration                                                       |
| [ ]  | `D02 T12 §6`  | NtCreateFile / NtOpenFile / NtClose / NtReadFile / NtWriteFile                                    |
| [ ]  | `D02 T12 §7`  | NtCreateProcess / NtCreateThread / Process-Thread Lifecycle                                       |
| [x]  | `D02 T12 §8`  | Synchronisation Objects + NtWaitForMultipleObjects                                                |
| [x]  | `D02 T12 §9`  | Virtual Memory (Alloc, Free, Protect, Lock, Cross-Process)                                        |
| [ ]  | `D02 T12 §10` | NtQuerySystemInformation / NtQueryInformationProcess                                              |
| [x]  | `D02 T12 §11` | Extended Error Information (IOSB + LastError)                                                     |
| [x]  | `D02 T12 §12` | ZwXxx Kernel-Mode Alias Layer                                                                     |
| [x]  | `D02 T12 §13` | File Metadata and Device Control                                                                  |
| [ ]  | `D02 T12 §16` | Token Open, Query, and Adjust Syscalls                                                            |
| [ ]  | `D02 T12 §17` | Directory and Symbolic Link Object Syscalls                                                       |
| [ ]  | `D02 T12 §18` | Section and Memory-Mapped File Syscalls                                                           |
| [ ]  | `D02 T12 §19` | Timer Control Syscalls                                                                            |
| [x]  | `D02 T12 §20` | Legacy LPC Port Syscalls                                                                          |
| [x]  | `D02 T12 §21` | Exception and Debug Syscalls                                                                      |
| [x]  | `D02 T12 §22` | Power and System Control                                                                          |
| [x]  | `D02 T12 §23` | Atom, Locale, and Miscellaneous Syscalls                                                          |
| [ ]  | `D02 T12 §24` | Syscall Audit and Tracing Hook                                                                    |
| [x]  | `D02 T12 §25` | Per-Process Syscall Filtering                                                                     |
| [x]  | `D02 T12 §26` | Kernel-to-User Mode Callback Dispatch                                                             |
| [x]  | `D02 T12 §27` | SSDT Integrity Protection                                                                         |
| [x]  | `D02 T12 §28` | Extended Directory Enumeration Classes                                                            |
| [ ]  | `D02 T12 §29` | Token Lifecycle and SRM Access Check Syscalls                                                     |
| [x]  | `D02 T12 §30` | Generic Object Management Syscalls                                                                |
| [x]  | `D02 T12 §31` | Modern ALPC Port Syscalls                                                                         |
| [ ]  | `D02 T12 §32` | Post-Ship Follow-Up Backfill (orphan cohort 2026-07-31)                                           |
| [ ]  | `D02 T12 §33` | Legacy `SYS_*` Number Allocation Across Roadmaps                                                  |
| [x]  | `D02 T13 §1`  | Unicode String Primitive Layer                                                                    |
| [x]  | `D02 T13 §2`  | Case Folding and Invariant Compare                                                                |
| [x]  | `D02 T13 §3`  | Global and Local Atom Tables                                                                      |
| [x]  | `D02 T13 §4`  | NLS Table File Format and Loader                                                                  |
| [x]  | `D02 T13 §5`  | Code Page Conversion Providers                                                                    |
| [ ]  | `D02 T13 §6`  | Locale and LCID Metadata                                                                          |
| [x]  | `D02 T13 §7`  | Sort Keys and Normalization Policy                                                                |
| [x]  | `D02 T14 §1`  | Access Rights, API Limits & RegFlushKey                                                           |
| [x]  | `D02 T14 §2`  | Advanced Key Operations                                                                           |
| [x]  | `D02 T14 §3`  | Change Notifications                                                                              |
| [x]  | `D02 T14 §7`  | `regedit` Shell Tool                                                                              |
| [x]  | `D02 T14 §8`  | Advanced Hive Features                                                                            |
| [x]  | `D02 T14 §9`  | Transactions, Search API & Snapshot/Diff                                                          |
| [x]  | `D02 T14 §10` | Performance Optimisations                                                                         |
| [x]  | `D02 T14 §11` | KTM Transaction Syscalls Wired to SSDT                                                            |
| [x]  | `D02 T14 §12` | Registry Symlink Completion                                                                       |
| [x]  | `D02 T14 §13` | Schema-Validated Registry Keys ⭐                                                                 |
| [x]  | `D02 T14 §15` | Registry Value Size Expansion (16 KiB Names, 1 MiB Data)                                          |
| [ ]  | `D02 T14 §16` | Post-Ship Follow-Up Backfill (orphan cohort 2026-07-31)                                           |
| [x]  | `D02 T15 §1`  | SID & LUID Primitives                                                                             |
| [x]  | `D02 T15 §2`  | Privilege Constants & PRIVILEGE_SET                                                               |
| [x]  | `D02 T15 §3`  | SECURITY_DESCRIPTOR, ACL & ACE Types                                                              |
| [ ]  | `D02 T15 §4`  | ACCESS_TOKEN Object                                                                               |
| [ ]  | `D02 T05 §9`  | NtClose / NtDuplicateObject / NtQueryObject                                                       |
| [ ]  | `D02 T15 §5`  | SeAccessCheck Engine                                                                              |
| [x]  | `D02 T15 §6`  | Mandatory Integrity Control (MIC)                                                                 |
| [x]  | `D02 T15 §7`  | Process/Thread Token Assignment & Impersonation                                                   |
| [x]  | `D02 T15 §8`  | SePrivilegeCheck & Per-Privilege Enforcement                                                      |
| [x]  | `D02 T15 §9`  | UAC Token Split & NtFilterToken                                                                   |
| [x]  | `D02 T15 §10` | Win32 Security API Wrappers                                                                       |
| [x]  | `D02 T15 §11` | Live Token Inspector                                                                              |
| [x]  | `D02 T15 §12` | Security and Token Syscalls Wired to SSDT                                                         |
| [x]  | `D02 T15 §13` | Security Descriptor Inheritance (SeAssignSecurity)                                                |
| [x]  | `D02 T15 §14` | AppContainer / LowBox Tokens                                                                      |
| [x]  | `D02 T15 §15` | Access Denial Explainer                                                                           |
| [ ]  | `D02 T15 §16` | Security Primitive Hardening                                                                      |
| [x]  | `D02 T16 §1`  | Notification State Object Type                                                                    |
| [x]  | `D02 T16 §2`  | Kernel Publish/Subscribe API                                                                      |
| [x]  | `D02 T16 §3`  | Waitable User Subscriptions                                                                       |
| [x]  | `D02 T16 §4`  | Security and Namespace Policy                                                                     |
| [x]  | `D02 T16 §5`  | Built-In State-Name Catalog                                                                       |
| [x]  | `D02 T16 §6`  | ETW/klog Bridge                                                                                   |
| [ ]  | `D02 T16 §7`  | Coalescing and Payload Retention                                                                  |
| [x]  | `D02 T16 §9`  | Diagnostics Browser and Counters                                                                  |
| [x]  | `D02 T17 §1`  | `exec_load()` Multi-Format Dispatcher                                                             |
| [x]  | `D02 T17 §2`  | Enhanced ELF Loader                                                                               |
| [x]  | `D02 T17 §3`  | ELF Security Segments (GNU_STACK, RELRO, GNU_PROPERTY)                                            |
| [x]  | `D02 T17 §4`  | EIF Format Specification                                                                          |
| [x]  | `D02 T17 §5`  | EIF Kernel Loader                                                                                 |
| [x]  | `D02 T17 §6`  | Module List Registration (LDR_DATA_TABLE_ENTRY)                                                   |
| [ ]  | `D02 T17 §7`  | PE32+ Header Parser                                                                               |
| [ ]  | `D02 T17 §8`  | PE32+ Section Loader + `.pdata` Registration                                                      |
| [ ]  | `D02 T17 §9`  | PE32+ Import Table Resolver (Win32 Dispatch)                                                      |
| [x]  | `D02 T17 §10` | PE32+ Base Relocation                                                                             |
| [x]  | `D02 T17 §11` | PE32+ TLS Directory Processing                                                                    |
| [x]  | `D02 T17 §12` | PE32+ Load Config and CFG Bitmap                                                                  |
| [/]  | `D02 T17 §13` | `elf2eif` Host-Side Converter                                                                     |
| [x]  | `D02 T17 §14` | ELF Dynamic Linker                                                                                |
| [x]  | `D02 T17 §15` | ASLR -- Address Space Layout Randomization                                                        |
| [x]  | `D02 T17 §16` | Script/Shebang Interpreter Support                                                                |
| [x]  | `D02 T17 §17` | EIF Code Signing                                                                                  |
| [x]  | `D02 T17 §18` | ELF `PT_TLS` Template Loading                                                                     |
| [x]  | `D02 T17 §19` | PE API-Set + Delay-Load/Bound Import Support                                                      |
| [x]  | `D02 T17 §20` | ELF Dynamic Linker -- Advanced (IFUNC, Init/Fini, Versioning, Search Order)                       |
| [x]  | `D02 T18 §1`  | `KIMAGE_ENTRY` Data Model                                                                         |
| [/]  | `D02 T18 §3`  | Address Range Index                                                                               |
| [/]  | `D02 T18 §4`  | Symbol Provider Abstraction                                                                       |
| [x]  | `D02 T19 §1`  | Code Integrity Policy Object                                                                      |
| [x]  | `D02 T19 §2`  | Image Validation API and Callback                                                                 |
| [x]  | `D02 T19 §3`  | Hashing and Signature Provider Bridge                                                             |
| [/]  | `D02 T19 §4`  | Embedded Signature Validation                                                                     |
| [/]  | `D02 T19 §5`  | Catalog Database                                                                                  |
| [x]  | `D02 T19 §6`  | Revocation and Deny Lists                                                                         |
| [x]  | `D02 T19 §7`  | Measured-Boot and Secure Boot Binding                                                             |
| [x]  | `D02 T19 §8`  | Driver/Module Enforcement                                                                         |
| [x]  | `D02 T19 §9`  | User-Mode Image Enforcement                                                                       |
| [x]  | `D02 T19 §10` | CI Audit, Telemetry, and Syscalls                                                                 |
| [ ]  | `D02 T19 §11` | Post-Ship Follow-Up Backfill (orphan cohort 2026-07-31)                                           |
| [/]  | `D02 T18 §9`  | Provenance, CI, and Hotpatch Metadata                                                             |
| [x]  | `D02 T20 §1`  | Range Overlap Validation (Normative Rule 2)                                                       |
| [ ]  | `D02 T20 §2`  | Segment Permission Enforcement (R/W/X via PTE)                                                    |
| [ ]  | `D02 T20 §3`  | Module Registration for EIF                                                                       |
| [x]  | `D02 T20 §4`  | API Version Gating                                                                                |
| [x]  | `D02 T20 §5`  | Metadata Section Parser                                                                           |
| [x]  | `D02 T20 §6`  | LZ4 Compressed Segments                                                                           |
| [/]  | `D02 T20 §7`  | Optional Import Stubs                                                                             |
| [x]  | `D02 T20 §8`  | EIF ASLR (load_base=0 Randomization)                                                              |
| [ ]  | `D02 T20 §9`  | Per-Process Dispatch Table Isolation                                                              |
| [x]  | `D02 T20 §10` | CET Compatibility Flag (Format Reservation)                                                       |
| [/]  | `D02 T20 §11` | EIF Signature-Block ABI (Signer Key Delivery)                                                     |
| [ ]  | `D02 T20 §12` | Resource Section with SVG Icons                                                                   |
| [x]  | `D02 T21 §1`  | Working Directory                                                                                 |
| [ ]  | `D02 T21 §2`  | Standard Handle Pre-Wiring at Process Creation                                                    |
| [x]  | `D02 T21 §3`  | User-Mode Program Break (brk/sbrk -- Linux Compat)                                                |
| [x]  | `D02 T21 §4`  | Process Priority Class                                                                            |
| [x]  | `D02 T21 §5`  | Per-Task Scheduling Policy                                                                        |
| [/]  | `D02 T21 §6`  | Process Capabilities and Privilege Bitmask                                                        |
| [/]  | `D02 T21 §7`  | Capability Inheritance and Drop-Only Policy                                                       |
| [x]  | `D02 T21 §8`  | Process Accounting Fields (Times, I/O Counters)                                                   |
| [x]  | `D02 T21 §9`  | Per-Process Resource Limits (rlimits)                                                             |
| [x]  | `D02 T21 §10` | CPU Affinity per Process                                                                          |
| [x]  | `D02 T21 §11` | Per-Process Mitigation Policy                                                                     |
| [x]  | `D02 T21 §12` | Pledge/Unveil-Style Process Restriction                                                           |
| [x]  | `D02 T21 §13` | Job Object Syscalls Wired to SSDT                                                                 |
| [x]  | `D02 T21 §14` | Process Exit Cleanup                                                                              |
| [x]  | `D02 T21 §15` | Process Parenting, Reaping, and Wait Semantics                                                    |
| [x]  | `D02 T21 §16` | Protected Process Light (PS_PROTECTION)                                                           |
| [x]  | `D02 T21 §17` | Process Groups and Sessions                                                                       |
| [x]  | `D02 T21 §18` | Rich Wait Variants, NT Multi-Waiter Wake, and Dumpable Policy                                     |
| [x]  | `D02 T21 §19` | `task_exec` Commit Point and Kernel-Stack Reclamation                                             |
| [ ]  | `D02 T21 §20` | Page-Table Lifetime Across Reap + fork/exec                                                       |
| [ ]  | `D02 T21 §21` | Post-Ship Follow-Up Backfill (orphan cohort 2026-07-31)                                           |
| [x]  | `D02 T22 §1`  | Per-Process Environ Storage & Kernel API                                                          |
| [x]  | `D02 T22 §2`  | System Default Variables from Registry                                                            |
| [x]  | `D02 T22 §3`  | `%VAR%` Expansion                                                                                 |
| [ ]  | `D02 T22 §4`  | argv Array: Kernel Storage & Shell Parsing                                                        |
| [x]  | `D02 T22 §5`  | Nt/Zw Environment Variable Syscalls                                                               |
| [x]  | `D02 T22 §6`  | Win32 API Wrappers                                                                                |
| [x]  | `D02 T22 §7`  | Shell Integration: PATH Lookup & SET/ECHO                                                         |
| [x]  | `D02 T22 §8`  | `.profile` Startup Script                                                                         |
| [x]  | `D02 T22 §9`  | Environment Change Notifications                                                                  |
| [x]  | `D02 T22 §10` | Environment Block Sorting & Size Limits                                                           |
| [x]  | `D02 T22 §11` | PATHEXT Variable & Extension Search Order                                                         |
| [x]  | `D02 T22 §12` | Hidden Drive-Letter Variables (`=C:`, `=D:`)                                                      |
| [x]  | `D02 T22 §13` | CreateEnvironmentBlock / DestroyEnvironmentBlock                                                  |
| [ ]  | `D02 T22 §14` | SearchPathW / SearchPathA Win32 API                                                               |
| [x]  | `D02 T22 §15` | CommandLineToArgvW Win32 API                                                                      |
| [x]  | `D02 T22 §16` | Environment Variable Security & Sanitization                                                      |
| [x]  | `D02 T22 §17` | App Paths Registry-Based Executable Lookup                                                        |
| [x]  | `D02 T22 §18` | cmd.exe Dynamic Pseudo-Variables & Delayed Expansion                                              |
| [x]  | `D02 T22 §19` | Rtl Expansion Hardening: Output-Count Wrap & `%=X:%` Parity                                       |
| [x]  | `D02 T22 §20` | Rtl Export Prerequisites: Lookup Bound, Foreign-Block Extent, At-Cap Allocation                   |
| [x]  | `D02 T22 §21` | ntdll Rtl Environment Exports                                                                     |
| [x]  | `D02 T22 §22` | env Allocator Safety and UTF-8 Expansion Budget                                                   |
| [x]  | `D02 T22 §23` | Live-Environment Adoption: SetCurrentEnvironment, SetEnvironmentStrings, CreateEnvironmentEx      |
| [ ]  | `D02 T22 §24` | Rtl Expansion Completeness: Boundary Copy, Measured Budget, Lookup Cache                          |
| [x]  | `D02 T22 §25` | Counted (non-`_U`) Rtl Env Read Forms                                                             |
| [x]  | `D02 T23 §1`  | EXCEPTION_RECORD, CONTEXT, and EXCEPTION_POINTERS Types                                           |
| [x]  | `D02 T23 §2`  | #PF Triage -- User vs. Kernel Decode, EXCEPTION_RECORD/CONTEXT Build, Dispatch Routing            |
| [x]  | `D02 T23 §4`  | Debugger First-Chance / Second-Chance Notification                                                |
| [x]  | `D02 T23 §5`  | KiUserExceptionDispatcher -- Ring-3 Exception Delivery                                            |
| [ ]  | `D02 T23 §6`  | x64 Table-Based Unwind (`.pdata`, `RtlVirtualUnwind`)                                             |
| [x]  | `D02 T23 §7`  | Stack Walking (`RtlCaptureStackBackTrace`, `RtlWalkFrameChain`)                                   |
| [x]  | `D02 T23 §8`  | SEH Chain Walk and `__try`/`__except` Frame Dispatch                                              |
| [ ]  | `D02 T23 §9`  | RtlUnwindEx -- Unwind to a Target Frame                                                           |
| [x]  | `D02 T23 §10` | Vectored Exception Handlers (VEH)                                                                 |
| [x]  | `D02 T23 §11` | Vectored Continue Handlers (VCH)                                                                  |
| [x]  | `D02 T23 §12` | Unhandled Exception Filter and WER Hook                                                           |
| [x]  | `D02 T23 §13` | Kernel Safe Probing (`ProbeForRead`, `ProbeForWrite`)                                             |
| [ ]  | `D02 T23 §14` | Kernel-Mode `__try`/`__except` for Drivers                                                        |
| [x]  | `D02 T23 §15` | POSIX Signal Delivery from Exceptions (Linux Compat)                                              |
| [x]  | `D02 T23 §16` | Exception Dispatch Telemetry                                                                      |
| [x]  | `D02 T23 §17` | Guard-Page Stack Auto-Grow -- Commit/Reserve Tracking, Guard Re-Arm, Terminal Overflow            |
| [ ]  | `D02 T23 §18` | Unwind Fixtures Independent of Where the Linker Put Real Code                                     |
| [x]  | `D02 T23 §19` | Nothing Compiles the EXCEPT_TELEMETRY=off Flavor                                                  |
| [ ]  | `D02 T23 §20` | Kernel SEH Cannot Be Armed Without Rewriting Scheduler Thread Metadata                            |
| [x]  | `D02 T24 §1`  | Message Header, Port Attributes & Type Codes                                                      |
| [x]  | `D02 T24 §2`  | ALPC_PORT Object & Object Manager Registration                                                    |
| [x]  | `D02 T24 §3`  | Connection State Machine                                                                          |
| [x]  | `D02 T24 §4`  | Synchronous Send+Wait+Receive Engine                                                              |
| [x]  | `D02 T24 §5`  | Asynchronous Delivery & Completion List                                                           |
| [ ]  | `D02 T24 §6`  | Large Data: Port Sections & View Mapping                                                          |
| [ ]  | `D02 T24 §7`  | Security: Client Token Capture & Impersonation                                                    |
| [/]  | `D02 T24 §8`  | NtAlpc* SSDT Registration & Stub Retrofit                                                         |
| [ ]  | `D02 T24 §9`  | NtAlpc QueryInformation, SetInformation & CancelMessage                                           |
| [x]  | `D02 T24 §10` | CSRSS ApiPort Bootstrap                                                                           |
| [x]  | `D02 T24 §11` | Message Zones & Resource Reserves (Pre-Allocated Buffers)                                         |
| [x]  | `D02 T24 §12` | Live Port Monitor & IPC Profiler                                                                  |
| [ ]  | `D02 T24 §13` | Post-Ship Follow-Up Backfill (orphan cohort 2026-07-31)                                           |
| [x]  | `D02 T25 §1`  | Resource Type Registry                                                                            |
| [x]  | `D02 T25 §2`  | Quota Block and Charge API                                                                        |
| [x]  | `D02 T25 §3`  | Process/Token/Job Ownership Model                                                                 |
| [x]  | `D02 T25 §4`  | Receipt Identity (Generation-Tokened Charges)                                                     |
| [x]  | `D02 T25 §5`  | Pool and Allocation Quota Integration                                                             |
| [x]  | `D02 T25 §6`  | Registry, ALPC, Notification Quotas                                                               |
| [x]  | `D02 T25 §7`  | CPU, I/O, and Wakeup Accounting                                                                   |
| [x]  | `D02 T25 §8`  | Native Query/Set Quota Syscalls                                                                   |
| [x]  | `D02 T25 §9`  | Resource Pressure Events and Recovery Hooks                                                       |
| [ ]  | `D02 T25 §10` | Tests, Leak Sweeps, and Dashboards                                                                |
| [x]  | `D02 T25 §11` | Charge Ledger and Transactional Adjustment                                                        |
| [x]  | `D02 T25 §12` | Resource Pressure Stall Telemetry                                                                 |
| [x]  | `D02 T25 §13` | Object Manager Charge Points                                                                      |
| [x]  | `D02 T25 §14` | Ledger Lifetime: ISR-Safe Return and Obligation Adoption                                          |
| [x]  | `D02 T25 §15` | Charge-Path Cost Reduction                                                                        |
| [x]  | `D02 T25 §16` | Charge Attribution and Status Fidelity                                                            |
| [x]  | `D02 T25 §17` | Infrastructure-Gated Charge-Path Bounding and Stall Lanes                                         |
| [x]  | `D02 T25 §18` | Ledger Obligation Ceiling and Charge-Source Pass-Through                                          |
| [x]  | `D02 T25 §19` | Lockless Job Membership and the Live Pressure-Retry Seam                                          |
| [ ]  | `D02 T25 §20` | Per-Process Network I/O Accounting                                                                |
| [x]  | `D02 T26 §1`  | ACPI Sleep Object Parsing & PM1 State Machine                                                     |
| [x]  | `D02 T26 §2`  | C1 Idle Entry: Race-Safe HLT + Per-CPU Idle Accounting                                            |
| [x]  | `D02 T26 §3`  | S3: Suspend to RAM                                                                                |
| [x]  | `D02 T26 §4`  | S4: Hibernation Image Format and Compression Codec                                                |
| [x]  | `D02 T26 §5`  | ACPI Embedded Controller (EC) Driver                                                              |
| [x]  | `D02 T26 §6`  | Battery & AC Adapter (ACPI Source Layer)                                                          |
| [x]  | `D02 T26 §7`  | Power & Sleep Button Event Dispatch                                                               |
| [x]  | `D02 T26 §8`  | PCI Power Management Capability and D0--D3hot State Machine                                       |
| [x]  | `D02 T26 §9`  | Driver Power Callbacks & Resume Ordering                                                          |
| [x]  | `D02 T26 §10` | S0ix Firmware Advertisement and MWAIT Capability Layer                                            |
| [x]  | `D02 T26 §11` | Fast Startup (Hybrid Shutdown / Hiberboot)                                                        |
| [ ]  | `D02 T26 §12` | Runtime Device Idle Management                                                                    |
| [ ]  | `D02 T26 §13` | Power Request Tracking & Wake Source Management                                                   |
| [ ]  | `D02 T26 §17` | Driver Power Query & Veto (IRP_MN_QUERY_POWER)                                                    |
| [ ]  | `D02 T26 §20` | Power Syscalls Wired to SSDT                                                                      |
| [ ]  | `D02 T26 §21` | Linux `/sys/power` Suspend Variant Parity                                                         |
| [ ]  | `D02 T26 §23` | PCIe ASPM and L1 Substates                                                                        |
| [ ]  | `D02 T26 §24` | ACPI General-Purpose Event (GPE) Blocks                                                           |
| [ ]  | `D02 T26 §25` | Per-CPU Idle-Time Accounting via `NtQuerySystemInformation`                                       |
| [x]  | `D02 T26 §26` | Stop-the-World CPU Rendezvous for Sleep Transitions                                               |
| [ ]  | `D02 T26 §27` | Rendezvous Safety Residue: Test Seam and the Retract-Side Window                                  |
| [x]  | `D02 T26 §28` | S4 Orchestration: Hibernation Image Write and Resume Path                                         |
| [ ]  | `D02 T26 §29` | Composite Battery State Model, Power Policy, and Registry Publication                             |
| [x]  | `D02 T26 §30` | Battery Charge Limiting and Smart Charging                                                        |
| [x]  | `D02 T26 §31` | Lid State and Lid-Close Policy                                                                    |
| [x]  | `D02 T26 §32` | Human Presence Detection (HPD)                                                                    |
| [ ]  | `D02 T26 §33` | PM Device Registry (`pm_device_t`, `pm_register_device`)                                          |
| [ ]  | `D02 T26 §35` | Directed Power (DFx) and DRIPS Residency Accounting                                               |
| [ ]  | `D02 T26 §36` | NIC Wake Offloads for Connected Standby (ARP/NS/WoL/Patterns)                                     |
| [x]  | `D02 T26 §37` | System Connected Standby Entry (All-CPU S0ix Transition)                                          |
| [x]  | `D02 T26 §38` | ACPI Table Discovery: Validate Extents Before Checksumming and Publishing                         |
| [ ]  | `D02 T26 §39` | ACPI Root-Pointer Integrity and Validator Unification                                             |
| [/]  | `D02 T27 §1`  | Bugcheck Codes & `KeBugCheckEx`                                                                   |
| [/]  | `D02 T27 §2`  | FPU/XMM/XSAVE State Capture                                                                       |
| [/]  | `D02 T27 §3`  | Module Registry for Crash Dumps                                                                   |
| [/]  | `D02 T27 §4`  | MDMP Binary Format: Header, Directory & Stream Types                                              |
| [ ]  | `D02 T27 §5`  | Minidump Writer                                                                                   |
| [ ]  | `D02 T27 §6`  | Kernel Dump & Full Dump Variants                                                                  |
| [ ]  | `D02 T27 §7`  | Raw-Partition Dump Sink (VFS Bypass)                                                              |
| [ ]  | `D02 T27 §8`  | Post-Boot Crash Recovery & CrashDumps Archive                                                     |
| [ ]  | `D02 T27 §9`  | `dmpanalyze.exe` Crash Analyzer                                                                   |
| [ ]  | `D02 T27 §10` | Dump Policy, WinDbg Interoperability & Confidentiality                                            |
| [ ]  | `D02 T28 §1`  | TTF Font Rendering in Panic Screen                                                                |
| [ ]  | `D02 T28 §2`  | Improved Layout and Visual Hierarchy                                                              |
| [ ]  | `D02 T28 §3`  | Crash Context: Last 10 klog Entries Inline                                                        |
| [ ]  | `D02 T28 §4`  | Smart QR Code with Compressed Crash Data                                                          |
| [ ]  | `D02 T28 §5`  | Auto-Restart Countdown Improvements                                                               |
| [ ]  | `D02 T28 §6`  | Crash Analysis Hints On-Screen                                                                    |
| [ ]  | `D02 T28 §7`  | Crash Statistics & Loop Protection                                                                |
| [ ]  | `D02 T28 §8`  | Safe Mode Suggestion After Repeated Crashes                                                       |
| [ ]  | `D02 T28 §9`  | "What Failed" Faulting Module Identification                                                      |
| [ ]  | `D02 T28 §10` | Panic Screen Modes (User / Developer / QR)                                                        |
| [ ]  | `D02 T28 §11` | Audio Crash Notification (PC Speaker Beep Codes)                                                  |
| [ ]  | `D02 T28 §12` | Keyboard-Driven Recovery Actions at Crash Screen                                                  |
| [ ]  | `D02 T28 §13` | Dump Collection Progress Percentage                                                               |
| [ ]  | `D02 T29 §1`  | Serial Line Setup: 115200 Baud, 8N1, FIFO                                                         |
| [ ]  | `D02 T29 §2`  | IRQ-Driven RX Ring Buffer and KD COM Port Selection                                               |
| [ ]  | `D02 T29 §3`  | KD Packet Framing: Send / Receive, Checksum, ACK/RESEND                                           |
| [ ]  | `D02 T29 §4`  | KD Connection Handshake & Breakin Detection                                                       |
| [ ]  | `D02 T29 §5`  | `#DB` / `#BP` Exception Routing to KD, AP Freeze/Thaw                                             |
| [ ]  | `D02 T23 §3`  | General Fault-to-Exception Mapping (#DE, #DB, #BP, #OF, #UD, #NP, #SS, #GP, #CP)                  |
| [/]  | `D02 T18 §5`  | Unwind Metadata Registry                                                                          |
| [ ]  | `D02 T29 §6`  | Context Get / Set                                                                                 |
| [ ]  | `D02 T29 §7`  | Memory Read / Write                                                                               |
| [ ]  | `D02 T29 §8`  | Software Breakpoint Management                                                                    |
| [ ]  | `D02 T29 §9`  | Hardware Breakpoints: DR0--DR3, DR7, `#DB` Reporting                                              |
| [ ]  | `D02 T29 §10` | Single-Step (RFLAGS.TF) + Continue / Step                                                         |
| [ ]  | `D02 T29 §11` | DbgKdGetVersionApi + Module List                                                                  |
| [ ]  | `D02 T29 §12` | I/O Port & MSR Read / Write                                                                       |
| [ ]  | `D02 T29 §13` | `kd_break()`, Keyboard F12 Breakin & QEMU Guide                                                   |
| [ ]  | `D02 T29 §14` | Debug Syscalls Wired to SSDT                                                                      |
| [ ]  | `D02 T29 §15` | KD Transport Roadmap, SMP Policy, and Coexistence                                                 |
| [/]  | `D02 T18 §8`  | KD/Crash Dump Integration                                                                         |
| [ ]  | `D02 T30 §1`  | Health State Model and Registry                                                                   |
| [ ]  | `D02 T30 §2`  | Subsystem Heartbeat API                                                                           |
| [ ]  | `D02 T30 §3`  | Failure Bucket Taxonomy                                                                           |
| [ ]  | `D02 T30 §4`  | Live Kernel Dump Trigger                                                                          |
| [ ]  | `D02 T30 §5`  | Degraded-Mode Transition Engine                                                                   |
| [ ]  | `D02 T30 §6`  | Recovery Action Dispatcher                                                                        |
| [ ]  | `D02 T30 §7`  | Repeated-Failure Escalation                                                                       |
| [/]  | `D02 T31 §1`  | per_cpu_data Assembly Offsets                                                                     |
| [/]  | `D02 T31 §2`  | boot_config and Early Config Struct Layout                                                        |
| [/]  | `D02 T31 §3`  | User ELF Range (0x800000-0x900000) 3-File Sync                                                    |
| [/]  | `D02 T31 §4`  | AP Trampoline Data Area Layout (0x8E00)                                                           |
| [/]  | `D02 T31 §5`  | Task Interrupt Frame Layout                                                                       |
| [/]  | `D02 T31 §6`  | IDT Vector Assignment Collision Detection                                                         |
| [/]  | `D02 T31 §7`  | SSDT Service Number Count Stability                                                               |
| [/]  | `D02 T31 §8`  | XSAVE/FXSAVE Area Alignment                                                                       |
| [/]  | `D02 T31 §9`  | ISR swapgs Symmetry Verification                                                                  |
| [/]  | `D02 T31 §10` | Memory Layout Guard Pages                                                                         |
| [/]  | `D02 T31 §11` | IXFS Superblock Layout and Magic                                                                  |
| [/]  | `D02 T31 §12` | Security Structs (SID, TOKEN, ACL/ACE)                                                            |
| [/]  | `D02 T31 §13` | VFS Drive Letter Range and Partition Offsets                                                      |
| [/]  | `D02 T31 §14` | exec_pending State Machine Verification                                                           |
| [/]  | `D02 T31 §15` | Framebuffer Bare-Metal Safety (5 Rules)                                                           |
| [ ]  | `D02 T32 §1`  | Per-CPU SPSC Ring Buffer + Atomic Primitives                                                      |
| [ ]  | `D02 T32 §2`  | Lockless `klog_v2()` Enqueue + Format Helpers                                                     |
| [ ]  | `D02 T32 §3`  | Priority Lanes + Per-Level Sub-Budgets                                                            |
| [ ]  | `D02 T32 §4`  | Fail-Proof FATAL Emergency Path                                                                   |
| [ ]  | `D02 T32 §5`  | Configurable Per-Subsystem Rate Limits + Diagnostic Exemption                                     |
| [ ]  | `D02 T32 §6`  | Background Drain Worker                                                                           |
| [ ]  | `D02 T32 §7`  | Backpressure-Aware Serial Drain + Pressure Counters                                               |
| [ ]  | `D02 T32 §8`  | Native Structured Field Wire Format + ETW + JSON Lines Feed                                       |
| [ ]  | `D02 T32 §9`  | Boot-Survival Ring Snapshot + Recovery                                                            |
| [ ]  | `D02 T32 §10` | v1 -> v2 Migration Switch + Benchmarks + Retire v1                                                |
| [x]  | `D02 T33 §1`  | Memory-Map Design + Canonical Layout Decision                                                     |
| [ ]  | `D02 T33 §2`  | Direct Map Construction (kernel still low)                                                        |
| [/]  | `D02 T33 §3`  | Linker VMA/LMA Split + Higher-Half Jump                                                           |
| [x]  | `D02 T33 §4`  | Descriptor Tables + Per-CPU at High Addresses + AP Path                                           |
| [x]  | `D02 T33 §5`  | Bootloader / `boot_info` / Framebuffer High Handoff                                               |
| [x]  | `D02 T33 §6`  | Per-Process PML4: Kernel High Shared, User Low Private                                            |
| [ ]  | `D02 T33 §7`  | Retire the `0x800000` USER_BASE Ceiling + BSS Guard                                               |
| [ ]  | `D01 T14 §13` | klog Format-Width Contract, Compiler-Checked                                                      |
| [ ]  | `D01 T14 §15` | Anti-Rollback Terminal Give-Up: Durable Record                                                    |
| [ ]  | `D01 T14 §17` | Restore Panic Evidence Before Phase 0 Can Overwrite It                                            |
| [ ]  | `D01 T14 §19` | Record the Panic-Page Pin Outcome in the `boot_info` Handoff                                      |
| [ ]  | `D01 T29 §19` | Post-Ship Follow-Up Backfill (orphan cohort 2026-07-31)                                           |
| [x]  | `D02 T33 §8`  | 5-Level Paging (LA57) Support -- Exceeds Win11                                                    |
| [x]  | `D02 T33 §9`  | VMM Walker Conversion -- Route Every Deref Through the HHDM Helper                                |
| [x]  | `D02 T33 §10` | Tactical BSS Headroom -- Large Static Pools to Dynamic Allocation                                 |
| [ ]  | `D02 T33 §11` | Unpark the Ceiling-Stalled Kernel Queue                                                           |
| [x]  | `D02 T33 §12` | Second Tactical BSS Reclamation Pass                                                              |
| [x]  | `D02 T33 §13` | Third Tactical Reclamation Pass -- the Trigger Section 12 Named Has Fired                         |
| [x]  | `D02 T33 §14` | Fourth Tactical Reclamation Pass -- Reclaim the Test-Only Statics                                 |
| [x]  | `D02 T33 §15` | Fifth Tactical Reclamation Pass -- Convert the `pipes` Pool                                       |
| [x]  | `D02 T33 §16` | Test-Side Residue of the Fourth Pass -- `s_cap_val`, the 36 KB Stack Locals, and the Scratch Cast |
| [ ]  | `D02 T34 §1`  | Mark Expected Test-Path Log Output at Emission                                                    |
| [ ]  | `D02 T34 §2`  | Log-Cleanliness Gate in the Kernel Test Runner                                                    |
| [/]  | `D02 T34 §3`  | Smoke Test Fails on Any `[FAIL]` in Its Own Log                                                   |
| [ ]  | `D02 T34 §4`  | Surface the Platform Blind Spots in the Run Summary                                               |
| [/]  | `D02 T34 §5`  | Rate-Limit the Repeat-Offender Warnings That Produce 86% of the Volume                            |
| [ ]  | `D02 T35 §1`  | Registry Tree SMP Synchronization and Hive Durability                                             |
| [ ]  | `D02 T35 §2`  | Registry Save/Restore Hive Bodies and Transactional Load                                          |
| [ ]  | `D02 T35 §3`  | Quota Enforcement and Job Memory Accounting                                                       |
| [ ]  | `D02 T35 §4`  | Module Identity and ELF Unwind Registration                                                       |
| [ ]  | `D02 T35 §5`  | User-Buffer Hardening on Fault-Recoverable Usercopy                                               |
| [ ]  | `D02 T35 §6`  | Boot, Entropy, Config and SRM Leftovers                                                           |
| [ ]  | `D02 T12 §14` | Registry Syscalls (Core CRUD)                                                                     |
| [ ]  | `D02 T14 §14` | Registry SMP Synchronization                                                                      |
| [ ]  | `D02 T14 §4`  | Nt/Zw Registry Syscalls                                                                           |
| [ ]  | `D02 T12 §15` | Registry Syscalls (Advanced)                                                                      |
| [ ]  | `D02 T13 §8`  | Native Atom/NLS/Locale Syscalls                                                                   |
| [ ]  | `D02 T14 §5`  | advapi32.dll Win32 Compatibility                                                                  |
| [ ]  | `D02 T14 §6`  | Registry Virtualization & .reg Import/Export                                                      |
| [ ]  | `D02 T13 §9`  | Retrofit Kernel Consumers                                                                         |
| [ ]  | `D02 T13 §10` | Tests and Compatibility Corpus                                                                    |
| [/]  | `D02 T16 §8`  | Native WNF-Compatible Syscall Surface                                                             |
| [ ]  | `D02 T16 §10` | Unit/Boot Tests                                                                                   |
| [/]  | `D02 T18 §7`  | Image Notifications and Callbacks                                                                 |
| [ ]  | `D02 T30 §8`  | Native Health Query/Control APIs                                                                  |
| [ ]  | `D02 T30 §9`  | Health Notifications and Dashboards                                                               |
| [ ]  | `D02 T30 §10` | Tests and Chaos Scenarios                                                                         |

### Phase 3 -- Memory and concurrency

Memory protection, the allocators, paging and reclaim, the scheduler, SMP and the synchronisation primitives are used by everything above the core, and their defects are the most expensive to find later, so they are settled before the drivers that lean on them.

| Done | Section       | Deliverable                                                                        |
| :--: | ------------- | ---------------------------------------------------------------------------------- |
| [ ]  | `D03 T01 §1`  | `mprotect` / `NtProtectVirtualMemory`                                              |
| [ ]  | `D03 T01 §2`  | W^X Enforcement                                                                    |
| [ ]  | `D03 T01 §3`  | Demand Paging -- MEM_RESERVE / MEM_COMMIT                                          |
| [ ]  | `D03 T01 §4`  | `NtQueryVirtualMemory`                                                             |
| [ ]  | `D03 T01 §5`  | `VirtualAlloc` / `VirtualFree` / `VirtualProtect` Win32 Wrappers                   |
| [ ]  | `D03 T01 §6`  | kmalloc Size Audit                                                                 |
| [ ]  | `D03 T01 §7`  | Build-Time kmalloc Lint                                                            |
| [ ]  | `D03 T01 §8`  | PMM Statistics                                                                     |
| [ ]  | `D03 T01 §9`  | Heap Canaries + Double-Free Detection                                              |
| [ ]  | `D03 T01 §10` | Kernel Memory Leak Detector                                                        |
| [ ]  | `D03 T01 §11` | MMIO Mapping with UC Attributes + HPET Validation                                  |
| [/]  | `D03 T01 §12` | Per-Process User Page Mapping (`vmm_map_user_page`)                                |
| [ ]  | `D03 T01 §13` | Auto-Growing User Stacks                                                           |
| [ ]  | `D03 T01 §14` | NtLockVirtualMemory / mlock -- Pin Pages in RAM                                    |
| [ ]  | `D03 T01 §15` | Commit Charge Tracking + Enforcement                                               |
| [ ]  | `D03 T01 §16` | Process Memory Counters (`GetProcessMemoryInfo`)                                   |
| [ ]  | `D03 T02 §1`  | User-Space ASLR                                                                    |
| [ ]  | `D03 T02 §2`  | KASLR -- Kernel Address Space Randomization                                        |
| [ ]  | `D03 T02 §3`  | SMEP                                                                               |
| [ ]  | `D03 T02 §4`  | SMAP + `copy_from_user` / `copy_to_user`                                           |
| [ ]  | `D03 T02 §5`  | NX / DEP Mapping Audit                                                             |
| [ ]  | `D03 T02 §6`  | KPTI Basic                                                                         |
| [ ]  | `D03 T02 §7`  | CET Shadow Stack Stub                                                              |
| [ ]  | `D03 T02 §8`  | Security Layout Report                                                             |
| [ ]  | `D03 T03 §1`  | Growable Kernel Heap + SMP Spinlock                                                |
| [ ]  | `D03 T03 §2`  | Memory Pressure Notifications                                                      |
| [ ]  | `D03 T03 §3`  | vmalloc -- Virtual Contiguous Allocator                                            |
| [ ]  | `D03 T03 §4`  | SLAB Allocator + Lock-Free Per-CPU Fast Path                                       |
| [ ]  | `D03 T03 §5`  | SLAB Shrinker                                                                      |
| [ ]  | `D03 T03 §6`  | Tagged Allocation API                                                              |
| [ ]  | `D03 T03 §7`  | NonPagedPool / PagedPool                                                           |
| [ ]  | `D03 T03 §8`  | Out-of-Band Metadata                                                               |
| [ ]  | `D03 T03 §9`  | Type-Isolated Pools (kalloc_type Model)                                            |
| [ ]  | `D03 T03 §10` | Per-Page Header Encoding Keys                                                      |
| [ ]  | `D03 T03 §11` | Probabilistic Guard Pages (KFENCE Model)                                           |
| [ ]  | `D03 T03 §12` | Production Quarantine (Delayed Reuse)                                              |
| [ ]  | `D03 T03 §13` | Zero-on-Free                                                                       |
| [ ]  | `D03 T03 §14` | Bulk Alloc/Free API                                                                |
| [ ]  | `D03 T03 §15` | `/sys/slab` + `/sys/pooltags` + Shell Commands                                     |
| [ ]  | `D03 T04 §1`  | Pagefile Manager + Swap Metadata Foundation                                        |
| [ ]  | `D03 T04 §2`  | Working-Set Tracking + Lock-Aware Residency Rules                                  |
| [ ]  | `D03 T04 §3`  | Background Reclaim + Modified-Page Writer                                          |
| [ ]  | `D03 T04 §4`  | Lazy File-Backed Pager + Soft/Hard Fault Resolution                                |
| [ ]  | `D03 T04 §5`  | Replacement Policy + Refault Tracking                                              |
| [ ]  | `D03 T04 §6`  | Control Surface + Observability Handoff                                            |
| [ ]  | `D03 T05 §1`  | COW `fork()`                                                                       |
| [ ]  | `D03 T05 §2`  | Huge Pages (2 MiB)                                                                 |
| [ ]  | `D03 T05 §3`  | 1 GiB Pages (Kernel MMIO)                                                          |
| [ ]  | `D03 T05 §4`  | `madvise` / `MEM_RESET` Hints                                                      |
| [ ]  | `D03 T05 §5`  | Section Object Multi-View Mappings                                                 |
| [ ]  | `D03 T05 §6`  | Per-Process Memory Limits (Job Objects)                                            |
| [ ]  | `D03 T05 §7`  | Zero-Copy DMA Buffer Pool                                                          |
| [ ]  | `D03 T05 §8`  | Compressed Memory (zRAM-style)                                                     |
| [ ]  | `D03 T05 §9`  | NUMA-Aware PMM                                                                     |
| [ ]  | `D03 T05 §10` | Transparent Huge Pages Collapser                                                   |
| [ ]  | `D03 T05 §11` | Address Windowing Extensions (AWE)                                                 |
| [ ]  | `D03 T06 §1`  | 40-Level Priority Queues + O(1) Bitmap Dequeue                                     |
| [ ]  | `D03 T06 §2`  | Dynamic Priority Aging                                                             |
| [ ]  | `D03 T06 §6`  | CPU Affinity                                                                       |
| [ ]  | `D03 T06 §8`  | Scheduler Tick Calibration                                                         |
| [ ]  | `D03 T06 §3`  | CFS vruntime + `prio_to_weight` Table                                              |
| [ ]  | `D03 T06 §4`  | `SCHED_FIFO` / `SCHED_RR` Real-Time Classes                                        |
| [ ]  | `D03 T06 §5`  | `SCHED_DEADLINE` EDF Scheduling                                                    |
| [ ]  | `D03 T06 §7`  | Scheduler Stats + `/sys/sched` VFS File                                            |
| [ ]  | `D03 T06 §9`  | CPU Frequency Scaling Hook                                                         |
| [ ]  | `D03 T06 §10` | Worker Factory Syscalls Wired to SSDT                                              |
| [/]  | `D03 T06 §11` | Per-Thread Kernel Stack + TSS.rsp0 Switching                                       |
| [ ]  | `D03 T06 §12` | Dynamic Thread Table + Resource-Driven Thread Limits                               |
| [ ]  | `D03 T06 §13` | Dynamic Task Table + Reusable PID Slot Allocation                                  |
| [ ]  | `D03 T06 §14` | Process/Job CPU Bandwidth Control                                                  |
| [ ]  | `D03 T06 §15` | Wait/Wake Transaction Locking (Lost-Wakeup Class)                                  |
| [ ]  | `D03 T06 §16` | Move XSAVE / FPU State Ownership from `struct task` to `struct thread`             |
| [ ]  | `D03 T07 §8`  | `READ_ONCE` / `WRITE_ONCE` Macros                                                  |
| [ ]  | `D03 T07 §1`  | SMP-Safe Atomics Audit + `smp_mb` Barriers                                         |
| [ ]  | `D03 T07 §2`  | TLB Shootdown IPI                                                                  |
| [ ]  | `D03 T07 §9`  | CPU Feature Intersection at AP Bringup                                             |
| [ ]  | `D03 T07 §3`  | Per-CPU Run Queues                                                                 |
| [ ]  | `D03 T07 §6`  | Per-CPU RCU Upgrade                                                                |
| [ ]  | `D03 T07 §4`  | Work-Stealing Load Balancer                                                        |
| [ ]  | `D03 T07 §7`  | CPU Hotplug Stub                                                                   |
| [ ]  | `D03 T07 §5`  | Adaptive Mutex Spin-on-Owner                                                       |
| [ ]  | `D03 T08 §1`  | Ticket Locks -- FIFO Fair Spinlocks                                                |
| [ ]  | `D03 T08 §2`  | Preemption Count                                                                   |
| [ ]  | `D03 T08 §3`  | Thread-Local Storage (TLS)                                                         |
| [ ]  | `D03 T08 §4`  | Futexes                                                                            |
| [ ]  | `D03 T08 §5`  | `pthread_once` / `call_once`                                                       |
| [ ]  | `D03 T08 §7`  | Thread Cancellation                                                                |
| [ ]  | `D03 T08 §11` | Mutex Wait-Queue SMP-Safety Backfill                                               |
| [ ]  | `D03 T09 §1`  | Named Pipes (NPFS) -- Server Endpoint                                              |
| [ ]  | `D03 T09 §2`  | Named Pipe Instances + Overlapped I/O                                              |
| [ ]  | `D03 T09 §3`  | Mailslots (MSFS)                                                                   |
| [ ]  | `D03 T09 §11` | Anonymous-Pipe Hardening -- SMP-Safe Slot State, Real Error Codes, Honest Capacity |
| [ ]  | `D03 T10 §1`  | Thread Stack Guard Pages                                                           |
| [ ]  | `D03 T10 §2`  | Preemption Count -- Debug Extensions                                               |
| [ ]  | `D03 T10 §3`  | Heap Canaries + SLAB Red Zones                                                     |
| [ ]  | `D03 T10 §5`  | Kernel Watchdog Thread                                                             |
| [ ]  | `D03 T10 §8`  | Named Lock Browser (`/sys/locks`)                                                  |
| [ ]  | `D03 T10 §4`  | Lock Dependency Validator (Lockdep)                                                |
| [ ]  | `D03 T10 §7`  | Graphical Deadlock Visualisation                                                   |
| [ ]  | `D03 T10 §9`  | KASAN -- Kernel Address Sanitiser                                                  |
| [ ]  | `D03 T10 §6`  | KCSAN -- Kernel Concurrency Sanitiser                                              |
| [ ]  | `D03 T10 §10` | `/sys/mem` Unified Memory Observability                                            |
| [ ]  | `D03 T11 §1`  | Outgoing-Kernel Staging Area + Folio Preservation                                  |
| [ ]  | `D03 T11 §2`  | Per-Subsystem Quiesce Callback Registry                                            |
| [ ]  | `D03 T11 §3`  | VFS Writeback + FD Table Serialize                                                 |
| [ ]  | `D03 T11 §4`  | Scheduler Drain + Thread Freeze                                                    |
| [ ]  | `D03 T11 §5`  | Kexec-Equivalent Jump Into New Kernel Image                                        |
| [ ]  | `D03 T11 §6`  | Incoming-Kernel Reattach Path (Splice Memory, Restore)                             |
| [ ]  | `D03 T11 §7`  | Live-Update Syscall + nt_live_update SSDT Entry                                    |
| [ ]  | `D03 T08 §9`  | Unified `_timeout(ms)` API                                                         |
| [ ]  | `D03 T08 §8`  | `WaitForMultipleObjects` (`waitable_t` vtable)                                     |
| [ ]  | `D03 T08 §6`  | `pthread_barrier_t` -- Kernel-Level Barrier                                        |
| [ ]  | `D03 T08 §10` | Sync Syscalls Wired to SSDT (Keyed Events, Alerts)                                 |
| [ ]  | `D03 T09 §4`  | I/O Completion Ports (IOCP)                                                        |
| [ ]  | `D03 T09 §5`  | IOCP Worker Thread Pool + DMA Completion Hook                                      |
| [ ]  | `D03 T09 §6`  | Win32 Ob-Backed Named Sync Objects                                                 |
| [ ]  | `D03 T09 §7`  | LPC Basic -- Local Procedure Call                                                  |
| [ ]  | `D03 T09 §8`  | ALPC Extensions                                                                    |
| [ ]  | `D03 T09 §9`  | `ImpossibleRing` -- Async Submission Rings                                         |
| [ ]  | `D03 T09 §10` | IPC Syscalls Wired to SSDT                                                         |

### Phase 4 -- Drivers and hardware

With the kernel model and its memory and locking rules in place, the hardware comes up: ACPI and power, PCI, USB, input, timers, display and the driver model that loads them. Bare metal is the target, so these rows close on real hardware, not on an emulator alone.

| Done | Section       | Deliverable                                                    |
| :--: | ------------- | -------------------------------------------------------------- |
| [ ]  | `D04 T02 §1`  | x2APIC Mode -- MSR Register Access                             |
| [ ]  | `D04 T02 §3`  | MSI Vector Bitmap Allocator                                    |
| [ ]  | `D04 T02 §4`  | TLB Shootdown IPI                                              |
| [ ]  | `D04 T02 §5`  | NMI Watchdog via PMI Counter Overflow                          |
| [ ]  | `D04 T03 §1`  | ACPICA AML Interpreter Integration                             |
| [ ]  | `D02 T26 §34` | PCI D3cold via ACPI `_PS0`/`_PS3` Platform Methods             |
| [ ]  | `D04 T03 §2`  | Clean Shutdown Sequence                                        |
| [ ]  | `D04 T03 §3`  | ACPI Power Button SCI                                          |
| [ ]  | `D04 T03 §4`  | Thermal Monitoring                                             |
| [ ]  | `D02 T26 §14` | ACPI Thermal Zone Management                                   |
| [ ]  | `D04 T03 §5`  | Battery Status (`_BST` / `_BIF`)                               |
| [ ]  | `D04 T03 §6`  | CPU Frequency Scaling -- DVFS                                  |
| [ ]  | `D02 T26 §15` | CPU Frequency Scaling Governor Framework                       |
| [ ]  | `D04 T03 §9`  | ACPI S3 Suspend / Resume                                       |
| [ ]  | `D04 T03 §10` | Hibernate (S4)                                                 |
| [ ]  | `D04 T04 §1`  | SMEP + SMAP Late Init on All APs                               |
| [ ]  | `D04 T04 §2`  | Hardware RNG -- `RDRAND` / `RDSEED` + ChaCha20 CSPRNG Fallback |
| [ ]  | `D04 T04 §3`  | DMA Bounce Buffer Manager                                      |
| [ ]  | `D04 T04 §5`  | Secure Boot UEFI Variable Read                                 |
| [ ]  | `D04 T04 §6`  | TPM 2.0 Command Layer and IOCTL Surface                        |
| [ ]  | `D04 T04 §7`  | Boot-Time Kernel Integrity Check                               |
| [ ]  | `D04 T05 §1`  | Kernel Symbol Table + `EXPORT_SYMBOL`                          |
| [ ]  | `D04 T05 §2`  | Module Build System                                            |
| [ ]  | `D04 T05 §3`  | Driver Model + HAL Vtables + PCI Match Tables                  |
| [ ]  | `D04 T01 §1`  | Canonical Device Node and Bus Path Schema                      |
| [ ]  | `D04 T01 §2`  | Full PCI/PCIe Enumeration with Bridges                         |
| [ ]  | `D04 T01 §4`  | ACPI Correlation                                               |
| [ ]  | `D04 T01 §5`  | IRQ Routing and MSI/MSI-X Handoff                              |
| [ ]  | `D04 T01 §6`  | Driver Bind/Unbind State Machine                               |
| [ ]  | `D04 T05 §4`  | ELF Relocatable Module Loader                                  |
| [ ]  | `D04 T05 §5`  | Auto-Load Modules at Boot                                      |
| [ ]  | `D04 T05 §6`  | RTL8139 as First Loadable Module                               |
| [ ]  | `D04 T05 §7`  | Plug and Play syscalls wired to SSDT                           |
| [/]  | `D02 T18 §2`  | Global and Per-Process Image Registries                        |
| [/]  | `D02 T18 §6`  | Loader Integration                                             |
| [/]  | `D02 T18 §10` | Tests and Consistency Verifier                                 |
| [ ]  | `D04 T06 §1`  | Firmware Directory and Manifest Format                         |
| [ ]  | `D04 T06 §2`  | `request_firmware()` Kernel API                                |
| [ ]  | `D04 T06 §3`  | Pinning and DMA-safe Copies                                    |
| [ ]  | `D04 T06 §5`  | Version Matching                                               |
| [ ]  | `D04 T06 §8`  | License Audit Report                                           |
| [ ]  | `D04 T06 §9`  | Driver Conversion Pass                                         |
| [ ]  | `D04 T07 §1`  | PCI Device Registry                                            |
| [ ]  | `D04 T07 §2`  | Embedded PCI ID Database                                       |
| [ ]  | `D04 T07 §3`  | Live Interrupt Counter                                         |
| [ ]  | `D04 T07 §4`  | Driver Health Registry                                         |
| [ ]  | `D04 T07 §5`  | `/sys/devices` + `/sys/interrupts` VFS Files                   |
| [ ]  | `D04 T07 §7`  | Device Manager GUI                                             |
| [ ]  | `D04 T07 §9`  | `SetupDiGetClassDevs` / `SetupDiEnumDeviceInfo` Win32 API      |
| [ ]  | `D04 T07 §10` | Driver Unload / Reload                                         |
| [ ]  | `D04 T08 §1`  | PCI Enhanced Config Space (ECAM)                               |
| [ ]  | `D04 T01 §3`  | BAR Sizing and Resource Window Allocator                       |
| [ ]  | `D04 T08 §4`  | HPET Timer Driver                                              |
| [ ]  | `D04 T02 §2`  | LAPIC Timer Calibration via HPET                               |
| [ ]  | `D04 T02 §6`  | Interrupt Profiler -- `/sys/interrupts`                        |
| [ ]  | `D04 T08 §2`  | PCIe Capability Chain Scanner                                  |
| [ ]  | `D04 T08 §3`  | MSI / MSI-X Interrupt Support                                  |
| [ ]  | `D04 T08 §5`  | NVMe Storage Driver (Built-in)                                 |
| [ ]  | `D04 T08 §6`  | PCIe Hot-Plug                                                  |
| [ ]  | `D04 T01 §7`  | Hot-Plug and Surprise Removal                                  |
| [ ]  | `D04 T09 §1`  | Hypervisor Detection                                           |
| [ ]  | `D04 T09 §2`  | Unified `hv.h` Interface + Backend Dispatch                    |
| [ ]  | `D04 T09 §3`  | Bare-Metal Null Backend                                        |
| [ ]  | `D04 T09 §4`  | VBox Display Auto-Resize                                       |
| [ ]  | `D04 T09 §5`  | VBox HGCM Client                                               |
| [ ]  | `D04 T09 §6`  | VBox Shared Folders                                            |
| [ ]  | `D04 T09 §7`  | VBox Shared Clipboard                                          |
| [ ]  | `D04 T09 §8`  | VirtIO GPU Display Resize + Page Flip                          |
| [ ]  | `D04 T09 §9`  | VirtIO 9P Shared Folders                                       |
| [ ]  | `D04 T09 §10` | Hyper-V Synthetic HID                                          |
| [ ]  | `D04 T09 §11` | Hyper-V Synthetic Video                                        |
| [ ]  | `D04 T09 §12` | VirtIO RNG Guest Entropy                                       |
| [ ]  | `D04 T10 §1`  | USB Core Abstraction Layer                                     |
| [ ]  | `D04 T10 §2`  | USB String Descriptor Retrieval                                |
| [ ]  | `D04 T10 §3`  | Isochronous Endpoint Support                                   |
| [ ]  | `D04 T10 §4`  | Interrupt Endpoint Setup for HID                               |
| [ ]  | `D04 T10 §5`  | USB HID Class Driver                                           |
| [ ]  | `D04 T10 §6`  | PS/2 ↔ USB Input Fallback                                      |
| [ ]  | `D04 T10 §7`  | USB MSC BOT Completion                                         |
| [ ]  | `D04 T10 §11` | USB CDC-ECM Ethernet                                           |
| [ ]  | `D04 T10 §12` | USB CDC-ACM Serial                                             |
| [ ]  | `D04 T10 §13` | Bluetooth HCI via USB                                          |
| [ ]  | `D04 T11 §1`  | Keyboard Layout System                                         |
| [ ]  | `D04 T11 §5`  | PS/2 Packet Resync                                             |
| [ ]  | `D04 T12 §1`  | I2C/SMBus Host Controller Driver                               |
| [ ]  | `D04 T12 §2`  | Synaptics PS/2 Touchpad Fallback                               |
| [ ]  | `D04 T12 §3`  | ACPI I2C Device Enumeration                                    |
| [ ]  | `D04 T12 §9`  | Touchpad Control Panel (`mouse.cpl` Touchpad Tab)              |
| [ ]  | `D04 T13 §1`  | Storage Driver Capability Matrix                               |
| [ ]  | `D04 T13 §2`  | AHCI/SATA Parity Completion                                    |
| [ ]  | `D04 T13 §3`  | ATA/ATAPI and Optical Media                                    |
| [ ]  | `D04 T14 §1`  | License Tracking                                               |
| [ ]  | `D04 T14 §2`  | Intel e1000 Module                                             |
| [ ]  | `D04 T14 §3`  | VirtIO-net Module                                              |
| [ ]  | `D04 T14 §4`  | RTL8169 / RTL8111 Gigabit Module                               |
| [ ]  | `D04 T14 §5`  | Intel igc (I225 / I226 2.5 GbE) Module                         |
| [ ]  | `D04 T14 §6`  | RTL8125 2.5 GbE Module                                         |
| [ ]  | `D04 T14 §7`  | Network Driver Test Suite                                      |
| [ ]  | `D04 T14 §8`  | WiFi 802.11 MAC Layer Stub                                     |
| [ ]  | `D04 T14 §9`  | Intel iwlwifi Stub (P4 Stretch)                                |
| [ ]  | `D04 T14 §10` | Realtek rtw89 Stub (P4 Stretch)                                |
| [ ]  | `D04 T15 §1`  | WiFi Driver Abstraction Layer                                  |
| [ ]  | `D04 T15 §2`  | 802.11 Frame Layer                                             |
| [ ]  | `D04 T15 §3`  | WiFi Scan & Association State Machine                          |
| [ ]  | `D04 T15 §5`  | WPA2-PSK Supplicant -- EAPOL 4-Way Handshake + CCMP            |
| [ ]  | `D04 T15 §6`  | RTL8821CE / RTL8822BE PCIe WiFi Driver                         |
| [ ]  | `D04 T15 §7`  | Intel AX200 / AX210 iwlwifi PCIe Driver                        |
| [ ]  | `D04 T15 §8`  | MediaTek MT7921 / MT7922 PCIe WiFi Driver                      |
| [ ]  | `D04 T15 §9`  | WiFi Power Management                                          |
| [ ]  | `D04 T15 §10` | `ncpa.cpl` WiFi Tab + `netsh wlan` Commands                    |
| [ ]  | `D04 T15 §11` | Airplane Mode Radio Coordinator                                |
| [ ]  | `D04 T16 §1`  | HCI Transport Layer                                            |
| [ ]  | `D04 T16 §2`  | HCI Firmware Loading                                           |
| [ ]  | `D04 T16 §3`  | L2CAP -- Logical Link Control and Adaptation Protocol          |
| [ ]  | `D04 T16 §4`  | SDP -- Service Discovery Protocol                              |
| [ ]  | `D04 T16 §5`  | HID Profile -- Bluetooth HID                                   |
| [ ]  | `D04 T16 §6`  | BLE -- Bluetooth Low Energy                                    |
| [ ]  | `D04 T16 §7`  | RFCOMM & Serial Port Profile (SPP)                             |
| [ ]  | `D04 T16 §8`  | A2DP + SBC Encoder + AVRCP                                     |
| [ ]  | `D04 T16 §9`  | Bluetooth Manager + Pairing UI + `bluetooth.cpl`               |
| [ ]  | `D04 T16 §10` | `btctl` Shell Command                                          |
| [ ]  | `D04 T17 §1`  | `display_device_t` Vtable + Compositor Hook                    |
| [ ]  | `D04 T17 §2`  | VBE/GOP Fallback Display Device                                |
| [ ]  | `D04 T17 §3`  | Bochs/BGA Display Module                                       |
| [ ]  | `D04 T17 §4`  | VirtIO-GPU Display Module                                      |
| [ ]  | `D04 T17 §5`  | VMSVGA 2D Display Module                                       |
| [ ]  | `D04 T17 §6`  | Multi-Head Support                                             |
| [ ]  | `D04 T17 §7`  | Intel HD/UHD iGPU Modesetting Stub (P3)                        |
| [ ]  | `D04 T17 §8`  | AMD APU Vega/RDNA Display Stub (P4)                            |
| [ ]  | `D04 T18 §1`  | Audio HAL -- `audio_device_t` Vtable                           |
| [ ]  | `D04 T18 §2`  | AC97 Sound Driver                                              |
| [ ]  | `D04 T18 §3`  | Intel HDA Driver                                               |
| [ ]  | `D04 T18 §4`  | VirtIO Sound Module                                            |
| [ ]  | `D04 T18 §5`  | USB Audio Class 1.0 (UAC1) Playback                            |
| [ ]  | `D04 T18 §6`  | Audio Device Hot-Plug                                          |
| [ ]  | `D04 T18 §7`  | Convert All Audio Drivers to `.kmod`                           |
| [ ]  | `D04 T19 §1`  | Sensor Class API                                               |
| [ ]  | `D04 T19 §6`  | Audio Jack and Presence Sensors                                |
| [ ]  | `D04 T19 §7`  | Event Notifications                                            |
| [ ]  | `D04 T19 §9`  | BlackBox Timeline                                              |
| [ ]  | `D04 T20 §1`  | Runtime COM Device Model                                       |
| [ ]  | `D04 T20 §2`  | UART 16550A Cleanup                                            |
| [ ]  | `D04 T20 §4`  | USB CDC-ACM Integration                                        |
| [ ]  | `D04 T20 §5`  | Ownership Arbitration                                          |
| [ ]  | `D04 T20 §6`  | Parallel/LPT Driver                                            |
| [ ]  | `D04 T22 §1`  | Camera Class API and Buffer Model                              |
| [ ]  | `D04 T22 §2`  | USB UVC Discovery and Controls                                 |
| [ ]  | `D04 T22 §3`  | UVC Streaming and Frame Formats                                |
| [ ]  | `D04 T22 §6`  | Capture Devices                                                |
| [ ]  | `D04 T22 §8`  | Permissions and Audit                                          |
| [ ]  | `D04 T25 §1`  | Certification Schema and Support Tiers                         |
| [ ]  | `D04 T25 §2`  | VM Driver Test Matrix                                          |
| [ ]  | `D04 T25 §3`  | Desktop/Laptop Bare-Metal Matrix                               |
| [ ]  | `D04 T25 §4`  | USB/Peripheral Matrix                                          |
| [ ]  | `D04 T25 §5`  | Network/Wireless/Bluetooth Matrix                              |
| [ ]  | `D04 T25 §6`  | Suspend/Resume and Hot-Plug Matrix                             |
| [ ]  | `D04 T25 §8`  | Failure Injection and Recovery                                 |
| [ ]  | `D04 T03 §8`  | ACPI C-States Idle                                             |
| [/]  | `D04 T03 §7`  | Power Profiles                                                 |
| [ ]  | `D02 T26 §16` | CPU Idle Governor Framework                                    |
| [ ]  | `D02 T26 §18` | Power Plan UI & `powercfg`                                     |
| [ ]  | `D02 T26 §19` | Energy-Aware Scheduling Integration                            |
| [ ]  | `D02 T26 §22` | User-Interaction-Aware QoS Throttling                          |
| [ ]  | `D02 T26 §40` | Display Backlight Control (Brightness)                         |
| [ ]  | `D04 T01 §8`  | Device Power States                                            |
| [ ]  | `D04 T01 §9`  | Resource Conflict Diagnostics                                  |
| [ ]  | `D04 T01 §10` | Tests                                                          |
| [ ]  | `D04 T04 §4`  | IOMMU / VT-d + AMD-Vi                                          |
| [ ]  | `D04 T04 §8`  | Intel CET Shadow Stacks                                        |
| [ ]  | `D04 T06 §4`  | Integrity and Signature Policy                                 |
| [ ]  | `D04 T06 §6`  | Firmware Update and Rollback                                   |
| [ ]  | `D04 T13 §4`  | VirtIO-blk Production Integration                              |
| [ ]  | `D04 T13 §5`  | SDHCI/eMMC/SD Card Driver                                      |
| [ ]  | `D04 T19 §2`  | ACPI Sensors                                                   |
| [ ]  | `D04 T20 §3`  | PCI Multiport Serial                                           |
| [ ]  | `D04 T20 §7`  | GPIO Framework                                                 |
| [ ]  | `D04 T20 §8`  | SPI Framework                                                  |
| [ ]  | `D04 T20 §9`  | Industrial I/O Diagnostics                                     |
| [ ]  | `D04 T20 §10` | Tests                                                          |
| [ ]  | `D04 T23 §1`  | Device Class Model                                             |
| [ ]  | `D04 T23 §4`  | Parallel/LPT Transport                                         |
| [ ]  | `D04 T23 §8`  | Permissions                                                    |
| [ ]  | `D04 T24 §1`  | External Expansion Topology Model                              |
| [ ]  | `D04 T24 §3`  | Thunderbolt/USB4 Security Policy                               |
| [ ]  | `D04 T24 §4`  | PCIe Tunneling Authorization                                   |
| [ ]  | `D04 T24 §5`  | Display Alt-mode and eGPU Boundary                             |
| [ ]  | `D04 T24 §6`  | Dock Power and Wake Events                                     |
| [ ]  | `D04 T24 §9`  | External DMA Risk Report                                       |
| [ ]  | `D04 T10 §8`  | Hot-Plug Interrupt Handling                                    |
| [ ]  | `D04 T10 §9`  | USB Hub Class Driver                                           |
| [ ]  | `D04 T10 §10` | EHCI Fallback (USB 2.0)                                        |
| [ ]  | `D04 T07 §8`  | USB Device Tree Integration                                    |
| [ ]  | `D04 T07 §6`  | `lspci` / `lsusb` Shell Commands                               |
| [ ]  | `D04 T06 §7`  | Device Manager and Log Integration                             |
| [ ]  | `D04 T06 §10` | Tests                                                          |
| [ ]  | `D04 T13 §6`  | USB Card Readers and Multi-LUN                                 |
| [ ]  | `D04 T13 §7`  | Storage Identity and Health                                    |
| [ ]  | `D04 T13 §8`  | Surprise Removal and Media Change                              |
| [ ]  | `D04 T13 §9`  | Diagnostics Report                                             |
| [ ]  | `D04 T13 §10` | Tests                                                          |
| [ ]  | `D04 T15 §4`  | RTL8188 / RTL8192 USB WiFi Driver                              |
| [ ]  | `D04 T22 §5`  | MIPI/IPU Boundary                                              |
| [ ]  | `D04 T22 §7`  | Scanner Class                                                  |
| [ ]  | `D04 T23 §2`  | USB Printer Class Transport                                    |
| [ ]  | `D04 T23 §3`  | IPP-over-USB Boundary                                          |
| [ ]  | `D04 T23 §5`  | Multifunction Composition                                      |
| [ ]  | `D04 T23 §6`  | Scanner Transport Handoff                                      |
| [ ]  | `D04 T23 §7`  | Status Reporting                                               |
| [ ]  | `D04 T23 §9`  | Diagnostics                                                    |
| [ ]  | `D04 T23 §10` | Tests                                                          |
| [ ]  | `D04 T24 §2`  | USB-C Dock and Hub Classification                              |
| [ ]  | `D04 T24 §7`  | Hot-Plug Storm Resilience                                      |
| [ ]  | `D04 T24 §8`  | Device Manager Topology                                        |
| [ ]  | `D04 T24 §10` | Certification Matrix                                           |
| [ ]  | `D04 T25 §7`  | Firmware and License Matrix                                    |
| [ ]  | `D04 T25 §9`  | Support Bundle and Hardware Fingerprint                        |
| [ ]  | `D04 T25 §10` | Release Gate Automation                                        |
| [ ]  | `D04 T11 §6`  | PS/2 Intellimouse Scroll Wheel (ID 3)                          |
| [ ]  | `D04 T11 §2`  | Built-in Layouts -- en-US, en-GB, de-DE, fr-FR, es-ES, Dvorak  |
| [ ]  | `D04 T11 §3`  | UTF-8 / Unicode Codepoint Output                               |
| [ ]  | `D04 T11 §4`  | Dead Key Compose                                               |
| [ ]  | `D04 T11 §7`  | Explorer 5-Button Extension (ID 4)                             |
| [ ]  | `D04 T11 §8`  | Mouse Acceleration + Sensitivity                               |
| [ ]  | `D04 T11 §9`  | Raw Input Grab API                                             |
| [ ]  | `D04 T11 §10` | Layout Switching -- Win+Space, Tray Indicator                  |
| [ ]  | `D04 T11 §11` | Sticky Keys + Typematic Rate                                   |
| [ ]  | `D04 T21 §1`  | Controller Class API                                           |
| [ ]  | `D04 T21 §2`  | HID Gamepad Parser                                             |
| [ ]  | `D04 T21 §3`  | XInput-Compatible Profiles                                     |
| [ ]  | `D04 T21 §4`  | Bluetooth Controllers                                          |
| [ ]  | `D04 T21 §5`  | Force Feedback and Rumble                                      |
| [ ]  | `D04 T21 §6`  | Battery, LED and Player Index                                  |
| [ ]  | `D04 T21 §7`  | Calibration and Profiles                                       |
| [ ]  | `D04 T21 §8`  | Hot-Plug and Latency                                           |
| [ ]  | `D04 T21 §9`  | Diagnostics                                                    |
| [ ]  | `D04 T21 §10` | Tests                                                          |
| [ ]  | `D04 T12 §4`  | HID-over-I2C (HoI2C) Transport                                 |
| [ ]  | `D04 T12 §5`  | HID Report Descriptor Parser                                   |
| [ ]  | `D04 T12 §7`  | ELAN and Goodix I2C Touchpad Quirks                            |
| [ ]  | `D04 T12 §6`  | Microsoft Precision Touchpad (PTP) Multi-Touch                 |
| [ ]  | `D04 T12 §8`  | PTP Gesture Engine                                             |
| [ ]  | `D04 T12 §10` | `xinput list` / `touchpad-info` Shell Commands                 |
| [ ]  | `D04 T19 §3`  | SMBus/I2C hwmon Transport                                      |
| [ ]  | `D04 T19 §4`  | Fan, Voltage, and Chassis Sensors                              |
| [ ]  | `D04 T19 §5`  | Accelerometer and Orientation Sensors                          |
| [ ]  | `D04 T19 §8`  | Diagnostics                                                    |
| [ ]  | `D04 T19 §10` | Tests                                                          |
| [ ]  | `D04 T22 §4`  | Privacy LED and Switch Enforcement                             |
| [ ]  | `D04 T22 §9`  | Diagnostics                                                    |
| [ ]  | `D04 T22 §10` | Tests                                                          |

### Phase 5 -- Storage and filesystems

Disks need the driver model of Phase 4, and everything from the registry hives to the installer needs a filesystem that survives power loss: AHCI and NVMe, the block layer, the cache, FAT, NTFS, ext4 and the native filesystem.

| Done | Section       | Deliverable                                                                     |
| :--: | ------------- | ------------------------------------------------------------------------------- |
| [ ]  | `D05 T01 §1`  | VirtIO-blk Flush & Write-back                                                   |
| [ ]  | `D05 T01 §2`  | VirtIO-blk Error Recovery                                                       |
| [ ]  | `D05 T01 §6`  | Block Device I/O Metrics                                                        |
| [ ]  | `D05 T01 §7`  | Block-Level Disk Cache                                                          |
| [ ]  | `D05 T01 §8`  | Block-I/O QoS (IOPS / Bandwidth Caps)                                           |
| [ ]  | `D05 T02 §1`  | B+ Tree Mutation Audit + Journaling                                             |
| [ ]  | `D05 T02 §2`  | Write Data Path Wiring                                                          |
| [ ]  | `D05 T02 §3`  | NTFS Volume Initialiser                                                         |
| [ ]  | `D05 T02 §4`  | `$Secure` Security Stream                                                       |
| [ ]  | `D05 T02 §5`  | Dirty Volume Recovery Wiring                                                    |
| [ ]  | `D05 T02 §6`  | NTFS Format + Crash + Replay Test                                               |
| [ ]  | `D05 T03 §1`  | Filesystem Probe Chain + Drive-Letter Assignment                                |
| [ ]  | `D05 T03 §2`  | Boot Mount Sequence Rewrite                                                     |
| [ ]  | `D05 T03 §6`  | Win32 Volume Query APIs                                                         |
| [ ]  | `D05 T03 §7`  | Optical Drive Handling                                                          |
| [x]  | `D05 T04 §1`  | FAT32 BPB Strict Validation                                                     |
| [/]  | `D05 T04 §2`  | FSInfo Mount-Time Fallback                                                      |
| [/]  | `D05 T04 §3`  | Dual-FAT Compare and Repair                                                     |
| [/]  | `D05 T04 §4`  | LFN Write                                                                       |
| [/]  | `D05 T04 §5`  | FAT32 Timestamps                                                                |
| [/]  | `D05 T04 §6`  | FAT32 fsck                                                                      |
| [/]  | `D05 T04 §7`  | VFS Case-Insensitive Path Resolution                                            |
| [/]  | `D05 T04 §8`  | VFS Share-Mode Enforcement                                                      |
| [/]  | `D05 T04 §9`  | Mark-for-Delete-on-Close                                                        |
| [/]  | `D05 T04 §10` | Byte-Range Locks                                                                |
| [/]  | `D05 T04 §11` | VFS Feature-Spoofing Stubs                                                      |
| [/]  | `D05 T04 §12` | SFN Numeric Tail Collision Avoidance                                            |
| [/]  | `D05 T04 §13` | FAT32 4 GiB Write Guard                                                         |
| [/]  | `D05 T04 §14` | VFS Opportunistic Locks (Oplocks)                                               |
| [x]  | `D05 T04 §15` | VFS_O_TRUNC End-to-End                                                          |
| [x]  | `D05 T04 §16` | VFS Rename Replace-Existing -- Atomic Temp-File Durable-Write Primitive         |
| [ ]  | `D05 T04 §17` | FAT32 Volume-Lock Hold Time -- No Blocking Disk I/O Under the `cli` `vol->lock` |
| [ ]  | `D05 T04 §18` | Post-Ship Follow-Up Backfill (orphan cohort 2026-07-31)                         |
| [ ]  | `D05 T05 §1`  | IRP Engine                                                                      |
| [ ]  | `D05 T05 §2`  | MDL -- Memory Descriptor List                                                   |
| [ ]  | `D05 T05 §3`  | File Handle Table                                                               |
| [ ]  | `D05 T05 §4`  | `CreateFile` / `CloseHandle`                                                    |
| [ ]  | `D05 T05 §5`  | `ReadFile` / `WriteFile` / `SetFilePointerEx`                                   |
| [ ]  | `D05 T05 §6`  | Directory APIs + Per-Process CWD                                                |
| [ ]  | `D05 T05 §7`  | File Management -- Delete, Move, Copy                                           |
| [ ]  | `D05 T05 §8`  | File Metadata APIs                                                              |
| [ ]  | `D05 T05 §9`  | Async I/O + APC                                                                 |
| [ ]  | `D05 T05 §10` | Filter Manager Stub                                                             |
| [ ]  | `D05 T05 §11` | Internal Migration -- `vfs_open` → `CreateFile`                                 |
| [ ]  | `D05 T05 §12` | File Change Notifications                                                       |
| [ ]  | `D05 T05 §14` | `DeviceIoControl` + FSCTL Dispatch + Oplock Ioctls                              |
| [ ]  | `D05 T06 §1`  | Subsystem Verification                                                          |
| [ ]  | `D05 T06 §2`  | Extended Inode v3                                                               |
| [ ]  | `D05 T06 §3`  | Case-Insensitive Paths                                                          |
| [ ]  | `D05 T06 §4`  | Alternate Data Streams (ADS)                                                    |
| [ ]  | `D05 T06 §5`  | Security Descriptors                                                            |
| [ ]  | `D05 T06 §6`  | Hard Links                                                                      |
| [ ]  | `D05 T06 §7`  | Symbolic Links + Reparse Points                                                 |
| [ ]  | `D05 T06 §8`  | Extended Attributes                                                             |
| [ ]  | `D05 T06 §9`  | Object IDs                                                                      |
| [ ]  | `D05 T06 §10` | Format Tool v3                                                                  |
| [ ]  | `D05 T06 §11` | Parity Gate                                                                     |
| [ ]  | `D05 T07 §1`  | Sparse File Support                                                             |
| [ ]  | `D05 T07 §2`  | Real-Time TRIM                                                                  |
| [ ]  | `D05 T07 §3`  | Transparent LZ4/Zstd Compression                                                |
| [ ]  | `D05 T07 §4`  | Inline Block Deduplication                                                      |
| [ ]  | `D05 T07 §5`  | Reflink Instant Copy                                                            |
| [ ]  | `D05 T07 §6`  | Online Defragmentation                                                          |
| [ ]  | `D05 T07 §7`  | Online Volume Resize                                                            |
| [ ]  | `D05 T07 §8`  | Self-Healing Metadata                                                           |
| [ ]  | `D05 T07 §9`  | Automatic Scheduled Snapshots                                                   |
| [ ]  | `D05 T07 §10` | Per-File AES-256-XTS Encryption                                                 |
| [ ]  | `D05 T07 §11` | USN Change Journal                                                              |
| [ ]  | `D05 T07 §12` | Volume Quotas                                                                   |
| [ ]  | `D05 T07 §13` | Filesystem-Native Storage Tiering                                               |
| [ ]  | `D05 T07 §14` | Volume Health Dashboard                                                         |
| [ ]  | `D05 T07 §15` | Comprehensive Test Suite                                                        |
| [ ]  | `D05 T07 §16` | Inode-Table + On-Disk-Layout Hardening                                          |
| [ ]  | `D05 T08 §1`  | VBR Parser + Boot Checksum                                                      |
| [ ]  | `D05 T08 §2`  | Allocation Bitmap                                                               |
| [ ]  | `D05 T08 §3`  | Up-Case Table                                                                   |
| [ ]  | `D05 T08 §4`  | Directory Entry Sets                                                            |
| [ ]  | `D05 T08 §5`  | Directory Read                                                                  |
| [ ]  | `D05 T08 §6`  | File Read                                                                       |
| [ ]  | `D05 T08 §7`  | File Write                                                                      |
| [ ]  | `D05 T08 §8`  | File Create / Delete / Rename                                                   |
| [ ]  | `D05 T08 §9`  | Timestamp Encoding                                                              |
| [ ]  | `D05 T08 §10` | VFS Registration + Probe + fsck                                                 |
| [ ]  | `D05 T08 §11` | Unicode Edge Cases                                                              |
| [ ]  | `D05 T08 §12` | Large File Support (> 4 GiB)                                                    |
| [ ]  | `D05 T09 §1`  | Superblock + Block Group Descriptors                                            |
| [ ]  | `D05 T09 §2`  | Inode Reader                                                                    |
| [ ]  | `D05 T09 §3`  | Extent Tree Decoder                                                             |
| [ ]  | `D05 T09 §4`  | Directory htree (B+ Tree) + Readdir                                             |
| [ ]  | `D05 T09 §5`  | File Read                                                                       |
| [ ]  | `D05 T09 §6`  | JBD2 Journal Replay                                                             |
| [ ]  | `D05 T09 §7`  | File Write                                                                      |
| [ ]  | `D05 T09 §8`  | Extent Tree Write                                                               |
| [ ]  | `D05 T03 §3`  | USB Hot-Plug Volume Arrival                                                     |
| [ ]  | `D05 T03 §4`  | USB Safe Removal                                                                |
| [ ]  | `D05 T03 §5`  | Manual `mount` / `umount` Shell Commands                                        |
| [ ]  | `D05 T03 §8`  | Volume Control Ioctls                                                           |
| [ ]  | `D05 T05 §13` | `GetDiskFreeSpaceEx` / `GetVolumeInformation`                                   |
| [ ]  | `D05 T09 §9`  | Directory Write                                                                 |
| [ ]  | `D05 T09 §10` | File Create / Delete / Rename                                                   |
| [ ]  | `D05 T09 §11` | Extended Attributes                                                             |
| [ ]  | `D05 T09 §12` | VFS Registration + Probe + fsck                                                 |
| [ ]  | `D05 T10 §1`  | Superblock Parse + CRC32C Verify                                                |
| [ ]  | `D05 T10 §2`  | B-Tree Node Format                                                              |
| [ ]  | `D05 T10 §3`  | Chunk Tree -- Logical→Physical Map                                              |
| [ ]  | `D05 T10 §4`  | Tree Search + Walk                                                              |
| [ ]  | `D05 T10 §5`  | Root Tree + Subvolume Enumeration                                               |
| [ ]  | `D05 T10 §6`  | Inode Reader                                                                    |
| [ ]  | `D05 T10 §7`  | Extent Data Decoder                                                             |
| [ ]  | `D05 T10 §8`  | Directory Reader                                                                |
| [ ]  | `D05 T10 §9`  | Symlinks + XAttrs                                                               |
| [ ]  | `D05 T10 §10` | VFS Registration + Probe + Read-Only Mount                                      |
| [ ]  | `D05 T11 §1`  | SCSI MMC Layer -- READ TOC, DISC INFO, TRACK INFO                               |
| [ ]  | `D05 T11 §2`  | ISO 9660 Base                                                                   |
| [ ]  | `D05 T11 §3`  | Rock Ridge Extensions                                                           |
| [ ]  | `D05 T11 §4`  | Joliet                                                                          |
| [ ]  | `D05 T11 §5`  | UDF                                                                             |
| [ ]  | `D05 T11 §6`  | Auto-Probe Priority                                                             |
| [ ]  | `D05 T11 §7`  | Disc Label + Metadata                                                           |
| [ ]  | `D05 T11 §8`  | Audio CD Raw Reads                                                              |
| [ ]  | `D05 T12 §1`  | APFS Container Superblock + Fletcher-64                                         |
| [ ]  | `D05 T12 §2`  | APFS Object Map B-Tree                                                          |
| [ ]  | `D05 T12 §3`  | APFS Volume Mount                                                               |
| [ ]  | `D05 T12 §4`  | APFS Filesystem B-Tree Search + Walk                                            |
| [ ]  | `D05 T12 §5`  | APFS Inode Reader                                                               |
| [ ]  | `D05 T12 §6`  | APFS File Extent Reader                                                         |
| [ ]  | `D05 T12 §7`  | APFS Directory Reader                                                           |
| [ ]  | `D05 T12 §8`  | APFS Symlinks + XAttrs                                                          |
| [ ]  | `D05 T12 §9`  | HFS+ Volume Header + Journal Replay                                             |
| [ ]  | `D05 T12 §10` | HFS+ Catalog B-Tree + Directory Reader                                          |
| [ ]  | `D05 T12 §11` | HFS+ File Read + Extents Overflow B-Tree                                        |
| [ ]  | `D05 T12 §12` | VFS Registration -- `apfs_probe`, `hfsplus_probe`, Read-Only Mount              |
| [ ]  | `D05 T13 §1`  | GPT Partition Write                                                             |
| [ ]  | `D05 T13 §2`  | MBR Partition Write                                                             |
| [ ]  | `D05 T13 §3`  | `diskpart` CLI                                                                  |
| [ ]  | `D05 T13 §4`  | `chkdsk` CLI + GUI                                                              |
| [ ]  | `D05 T13 §5`  | `defrag` CLI + GUI                                                              |
| [ ]  | `D05 T13 §6`  | `sfc` CLI + GUI                                                                 |
| [ ]  | `D05 T13 §7`  | `recover` CLI + GUI                                                             |
| [ ]  | `D05 T13 §8`  | Disk Management GUI                                                             |
| [ ]  | `D05 T13 §9`  | `diskuse` CLI + GUI                                                             |
| [ ]  | `D05 T13 §10` | Snapshot Manager CLI + GUI                                                      |
| [ ]  | `D05 T14 §4`  | Generic `blkdev_stats_t` + Histogram API                                        |
| [ ]  | `D05 T14 §1`  | `diskbench` CLI                                                                 |
| [ ]  | `D05 T14 §3`  | SMART Extended Diagnostics                                                      |
| [ ]  | `D05 T14 §5`  | Per-Device Latency Histogram + `/sys/ioqueue`                                   |
| [ ]  | `D05 T14 §2`  | Disk Benchmark GUI                                                              |
| [ ]  | `D05 T14 §6`  | VFS Hot-Path Profiler                                                           |
| [ ]  | `D05 T14 §7`  | Storage Health Monitor Daemon                                                   |
| [ ]  | `D05 T01 §3`  | AHCI Native Command Queuing (NCQ)                                               |
| [ ]  | `D05 T01 §4`  | AHCI Error Recovery                                                             |
| [ ]  | `D05 T01 §5`  | AHCI SMART Read                                                                 |

### Phase 6 -- Desktop foundation

The window manager, the compositor, the input system and the control library are the base the graphics stack and the shell are built on, so they precede both.

| Done | Section      | Deliverable                               |
| :--: | ------------ | ----------------------------------------- |
| [/]  | `D06 T01 §1` | Minimize, Maximize, Restore               |
| [/]  | `D06 T01 §2` | Resize by Dragging Window Edges           |
| [/]  | `D06 T01 §3` | Snap to Left/Right Half                   |
| [/]  | `D06 T01 §4` | Global Hotkeys                            |
| [/]  | `D06 T02 §1` | Dirty Rect Tracking Infrastructure        |
| [/]  | `D06 T02 §2` | Partial Wallpaper Restore                 |
| [/]  | `D06 T02 §3` | Per-Window Damage and Compositor Loop     |
| [/]  | `D06 T02 §4` | Frame Timing and VSync                    |
| [ ]  | `D06 T02 §5` | Terminal Render Clipping (Boot Bleed Fix) |
| [ ]  | `D06 T03 §1` | Modifier Key Tracking                     |
| [ ]  | `D06 T03 §2` | Key Event Struct + Dispatch Pipeline      |
| [ ]  | `D06 T03 §3` | Focus Model and Tab Navigation            |
| [ ]  | `D06 T03 §4` | Global Hotkey Dispatch Table              |
| [ ]  | `D06 T03 §5` | Bare-Metal Input Bugs (Focus + Mouse)     |
| [/]  | `D06 T04 §1` | Checkbox and Radio Button                 |
| [/]  | `D06 T04 §2` | Progress Bar                              |
| [/]  | `D06 T04 §3` | Listbox with Scrollbar                    |
| [/]  | `D06 T04 §4` | Combobox (Dropdown)                       |
| [/]  | `D06 T04 §5` | Context Menu (Popup)                      |
| [/]  | `D06 T05 §1` | Desktop Icon Click Actions                |
| [/]  | `D06 T05 §4` | Start Menu Program Launch                 |
| [/]  | `D06 T05 §5` | Power and Settings Buttons                |
| [/]  | `D06 T06 §1` | Icon Data Model and Grid Layout           |
| [ ]  | `D06 T06 §2` | Special Folder Icons (Always Present)     |
| [/]  | `D06 T06 §3` | Dynamic Icons from Desktop Directory      |
| [/]  | `D06 T06 §4` | Icon Interaction (Select, Drag, Rename)   |
| [ ]  | `D06 T06 §5` | Shortcut (.lnk) File Support              |
| [ ]  | `D06 T06 §6` | File Type Icon Association                |

### Phase 7 -- Networking

The network stack needs drivers and the kernel's I/O model but nothing from the desktop, so it runs here, ahead of the services and apps that need a socket.

| Done | Section       | Deliverable                                                 |
| :--: | ------------- | ----------------------------------------------------------- |
| [ ]  | `D07 T01 §5`  | Network Interface Manager                                   |
| [ ]  | `D07 T01 §1`  | TCP Header + Checksum                                       |
| [ ]  | `D07 T01 §2`  | TCP Connection State Machine                                |
| [ ]  | `D07 T01 §3`  | TCP API                                                     |
| [ ]  | `D07 T01 §4`  | TCP Robustness                                              |
| [ ]  | `D07 T01 §6`  | Loopback Interface                                          |
| [ ]  | `D07 T01 §7`  | Stateful Connection Tracking                                |
| [ ]  | `D07 T02 §1`  | DNS Query Builder                                           |
| [ ]  | `D07 T02 §2`  | DNS Response Parser                                         |
| [ ]  | `D07 T02 §3`  | DNS Cache                                                   |
| [ ]  | `D07 T02 §4`  | AAAA Query Stub                                             |
| [ ]  | `D07 T02 §5`  | Kernel Socket Layer                                         |
| [ ]  | `D07 T02 §6`  | Server Sockets                                              |
| [ ]  | `D07 T02 §7`  | Socket Options + Syscalls                                   |
| [ ]  | `D07 T02 §8`  | `select()`                                                  |
| [ ]  | `D07 T03 §1`  | URL Parser                                                  |
| [ ]  | `D07 T03 §2`  | HTTP GET                                                    |
| [ ]  | `D07 T03 §3`  | HTTP POST + Chunked Transfer Encoding                       |
| [ ]  | `D07 T03 §4`  | `wget` / `curl` Shell Commands                              |
| [ ]  | `D07 T03 §5`  | Mbed TLS Kernel Port                                        |
| [ ]  | `D07 T03 §6`  | CA Certificate Store                                        |
| [ ]  | `D07 T03 §7`  | HTTPS GET / POST                                            |
| [ ]  | `D01 T27 §8`  | Firmware Advisor Live Metadata Refresh and Fixture Coverage |
| [ ]  | `D07 T03 §8`  | HTTP/1.1 Keep-Alive Connection Pool                         |
| [ ]  | `D07 T04 §1`  | IPv6 Header + Ethertype Routing                             |
| [ ]  | `D07 T04 §2`  | IPv6 Send / Receive                                         |
| [ ]  | `D07 T04 §3`  | ICMPv6 -- Neighbor Discovery (NS / NA / RS / RA)            |
| [ ]  | `D07 T04 §4`  | Link-Local EUI-64 Autoconfiguration                         |
| [ ]  | `D07 T04 §5`  | NDP Neighbor Cache                                          |
| [ ]  | `D07 T04 §6`  | SLAAC -- Stateless Address Autoconfiguration                |
| [ ]  | `D07 T04 §7`  | DHCPv6 Client                                               |
| [ ]  | `D07 T04 §8`  | DNS AAAA Activation                                         |
| [ ]  | `D07 T04 §9`  | Dual-Stack Socket API                                       |
| [ ]  | `D07 T04 §10` | `ifconfig` IPv6 Display                                     |
| [ ]  | `D07 T05 §1`  | Packet Filter Engine                                        |
| [ ]  | `D07 T05 §2`  | IP Layer Hooks                                              |
| [ ]  | `D07 T05 §3`  | Stateful Connection Tracking Integration                    |
| [ ]  | `D07 T05 §4`  | Default Ruleset                                             |
| [ ]  | `D07 T05 §5`  | IPv6 Firewall                                               |
| [ ]  | `D07 T05 §9`  | Per-Rule Hit Counters                                       |
| [ ]  | `D07 T05 §6`  | Firewall CLI                                                |
| [ ]  | `D07 T05 §7`  | Registry Persistence                                        |
| [ ]  | `D07 T05 §8`  | `firewall.cpl` Control Panel Applet                         |
| [ ]  | `D07 T06 §1`  | DHCP Renewal Daemon                                         |
| [ ]  | `D07 T06 §2`  | NTP Client                                                  |
| [ ]  | `D07 T06 §3`  | Network Statistics API                                      |
| [ ]  | `D07 T06 §4`  | `ifconfig` Command                                          |
| [ ]  | `D07 T06 §5`  | Extended `ping` Command                                     |
| [ ]  | `D07 T06 §6`  | `traceroute` Command                                        |
| [ ]  | `D07 T06 §7`  | `netstat` Command                                           |
| [ ]  | `D07 T06 §8`  | `/sys/net` VFS Aggregation File                             |
| [ ]  | `D07 T06 §9`  | `ws2_32.dll` Winsock Stubs                                  |
| [ ]  | `D07 T07 §1`  | Text-Only Browser                                           |
| [ ]  | `D07 T07 §2`  | HTML Parser + DOM Tree                                      |
| [ ]  | `D07 T07 §3`  | Tab Management                                              |
| [ ]  | `D07 T07 §4`  | Block / Inline Layout Engine                                |
| [ ]  | `D07 T07 §6`  | CSS Parser + Style Engine                                   |
| [ ]  | `D07 T07 §7`  | JavaScript Stub                                             |
| [ ]  | `D07 T07 §8`  | Bookmarks Manager                                           |
| [ ]  | `D07 T07 §9`  | Download Manager                                            |
| [ ]  | `D07 T07 §10` | Privacy + Security                                          |
| [ ]  | `D07 T08 §1`  | FTP Protocol                                                |
| [ ]  | `D07 T08 §2`  | FTP Shell + GUI Integration                                 |
| [ ]  | `D07 T08 §3`  | FTPS / FTPES                                                |
| [ ]  | `D07 T08 §4`  | SSH2 Transport Layer                                        |
| [ ]  | `D07 T08 §5`  | SSH Authentication                                          |
| [ ]  | `D07 T08 §6`  | SSH Channel + PTY                                           |
| [ ]  | `D07 T08 §7`  | SSH Shell Integration                                       |
| [ ]  | `D07 T08 §8`  | SSH Agent Stub                                              |
| [ ]  | `D07 T08 §9`  | SFTP Subsystem                                              |
| [ ]  | `D07 T08 §10` | Network Browsing and SMB Client                             |
| [ ]  | `D07 T09 §1`  | Message Parser                                              |
| [ ]  | `D07 T09 §2`  | SMTP Client                                                 |
| [ ]  | `D07 T09 §3`  | POP3 Client                                                 |
| [ ]  | `D07 T09 §4`  | IMAP Client                                                 |
| [ ]  | `D07 T09 §5`  | Email Account Manager                                       |
| [/]  | `D07 T09 §6`  | Notifications                                               |
| [/]  | `D07 T09 §7`  | Email GUI                                                   |
| [/]  | `D07 T09 §8`  | Compose Window                                              |
| [ ]  | `D07 T09 §9`  | Contacts Integration                                        |
| [ ]  | `D07 T09 §10` | Search                                                      |
| [ ]  | `D07 T10 §1`  | PDF Structure Parser                                        |
| [ ]  | `D07 T10 §2`  | Object Model                                                |
| [ ]  | `D07 T10 §3`  | Page Tree                                                   |
| [ ]  | `D07 T10 §4`  | Content Stream Interpreter                                  |
| [ ]  | `D07 T10 §5`  | Font Handling                                               |
| [ ]  | `D07 T11 §1`  | RFC 5424 Packet Formatter and UDP Sender                    |
| [ ]  | `D07 T11 §2`  | Registry-Gated Syslog Init                                  |
| [ ]  | `D07 T11 §3`  | Network-Down Resilience                                     |

### Phase 8 -- Graphics and UI

GDI, the compositor, fonts, text and the controls sit on the desktop foundation and are what every visible surface is drawn with, so they come before the shell and the apps.

| Done | Section       | Deliverable                                                             |
| :--: | ------------- | ----------------------------------------------------------------------- |
| [ ]  | `D08 T01 §1`  | Render-Target Allocator, Views, and Cached Layers                       |
| [ ]  | `D08 T01 §2`  | Clip, Transform, and State Stack                                        |
| [ ]  | `D08 T01 §3`  | Path, Stroke, Fill, and SVG-Ready Vector Raster Contract                |
| [ ]  | `D08 T01 §4`  | Theme-Aware Icon, Cursor, and Scalable Asset Pipeline                   |
| [ ]  | `D08 T01 §5`  | Thumbnail, Preview, and Multi-Size Asset Cache                          |
| [ ]  | `D07 T07 §5`  | Image Loading in Browser                                                |
| [ ]  | `D07 T10 §6`  | Image Rendering                                                         |
| [ ]  | `D07 T10 §7`  | Page Renderer                                                           |
| [/]  | `D07 T10 §8`  | PDF Viewer App                                                          |
| [ ]  | `D07 T10 §9`  | PDF from HTTP                                                           |
| [ ]  | `D07 T10 §10` | PDF Forms Stub                                                          |
| [ ]  | `D08 T01 §6`  | Recorded Scene Lists and Deterministic Re-render                        |
| [ ]  | `D08 T01 §7`  | SVG Runtime Renderer (Vendored plutovg and plutosvg)                    |
| [ ]  | `D08 T01 §8`  | Icon Engine (Build-Time Generator and Validator)                        |
| [ ]  | `D08 T01 §9`  | Missing Icons -- Detect, Draft-Generate, and Queue for Polish           |
| [ ]  | `D08 T02 §1`  | Font Catalog, Enumeration, Install/Remove, and Default Stacks           |
| [ ]  | `D08 T02 §2`  | Fallback Chains, Emoji, and Color-Font Support                          |
| [ ]  | `D08 T02 §3`  | Shaping, Bidi, Line-Break, and Paragraph Layout Engine                  |
| [ ]  | `D08 T02 §4`  | Caret, Hit-Test, Selection, and Composition-Aware Text Editing Services |
| [ ]  | `D08 T02 §5`  | Desktop and Win32 Wiring: `DrawText`, `ChooseFont`, `WM_FONTCHANGE`     |
| [ ]  | `D08 T02 §6`  | Persistent Text-Run Cache and No-FPU Steady-State Draw Path             |
| [ ]  | `D08 T02 §7`  | Vendor FreeType and HarfBuzz as the Rasterizer and Shaper               |
| [ ]  | `D08 T03 §1`  | `theme_t` Struct                                                        |
| [ ]  | `D08 T03 §2`  | `theme_get()` Singleton                                                 |
| [ ]  | `D08 T03 §3`  | Built-in Presets                                                        |
| [ ]  | `D08 T03 §4`  | Registry Load                                                           |
| [ ]  | `D08 T03 §5`  | Custom Accent Color                                                     |
| [ ]  | `D08 T03 §6`  | Migration Pass                                                          |
| [ ]  | `D08 T03 §7`  | Theme-Aware Shadow Rendering                                            |
| [ ]  | `D08 T03 §8`  | Hot-Reload                                                              |
| [ ]  | `D08 T03 §9`  | Fluent Token Corpus and Win11 Personalization Contract                  |
| [ ]  | `D08 T04 §1`  | Core Tween Engine                                                       |
| [ ]  | `D08 T04 §2`  | Easing Functions                                                        |
| [ ]  | `D08 T04 §3`  | Global Animation Manager                                                |
| [ ]  | `D08 T04 §4`  | Registry Controls                                                       |
| [ ]  | `D08 T04 §5`  | Window Transition Animations                                            |
| [ ]  | `D08 T04 §6`  | Spring Physics                                                          |
| [ ]  | `D08 T04 §7`  | Compositor Integration                                                  |
| [ ]  | `D08 T05 §1`  | Checkbox                                                                |
| [ ]  | `D08 T05 §2`  | Radio Button                                                            |
| [ ]  | `D08 T05 §3`  | Slider / TrackBar                                                       |
| [ ]  | `D08 T05 §4`  | Progress Bar                                                            |
| [ ]  | `D08 T05 §5`  | Dropdown / ComboBox                                                     |
| [ ]  | `D08 T05 §6`  | Tab Strip                                                               |
| [ ]  | `D08 T05 §7`  | Theming                                                                 |
| [ ]  | `D08 T05 §8`  | Accessibility Stubs                                                     |
| [ ]  | `D08 T05 §9`  | Overlay Scroll Bar (Collapse and Expand on Hover)                       |
| [ ]  | `D08 T06 §3`  | Toolbar                                                                 |
| [ ]  | `D08 T06 §4`  | MenuBar                                                                 |
| [ ]  | `D08 T06 §6`  | GroupBox + Separator                                                    |
| [ ]  | `D08 T06 §5`  | StatusBar                                                               |
| [ ]  | `D08 T06 §7`  | Tooltip Integration                                                     |
| [ ]  | `D08 T06 §1`  | ListView                                                                |
| [ ]  | `D08 T06 §2`  | TreeView                                                                |
| [ ]  | `D08 T06 §8`  | Dialog System                                                           |
| [ ]  | `D08 T06 §9`  | Info Bar Control                                                        |
| [ ]  | `D08 T07 §1`  | Semantic Node Tree, Roles, States, Values, and Action Descriptors       |
| [ ]  | `D08 T07 §2`  | Provider/Query API, Focus/Hit-Test Lookup, and Accessibility Events     |
| [ ]  | `D08 T07 §3`  | Inspect Bridge, Screen-Reader Hooks, and Automation Subscription Model  |
| [ ]  | `D08 T07 §4`  | IME and Text-Composition Framework                                      |
| [ ]  | `D08 T07 §5`  | Win32 and Platform-Service Wiring                                       |
| [ ]  | `D08 T07 §6`  | Deterministic Automation Transport for Testing and Assistive Tooling    |
| [ ]  | `D08 T08 §2`  | Window Decorations                                                      |
| [ ]  | `D08 T08 §1`  | Minimize / Maximize / Restore                                           |
| [ ]  | `D08 T08 §3`  | Snap Layouts                                                            |
| [ ]  | `D08 T08 §4`  | Desktop Icons                                                           |
| [ ]  | `D08 T08 §5`  | Keyboard Shortcuts & Task Switching                                     |
| [ ]  | `D08 T08 §6`  | Alt+Tab Task Switcher                                                   |
| [ ]  | `D08 T08 §7`  | Drag and Drop                                                           |
| [ ]  | `D08 T08 §8`  | Compositor Performance                                                  |
| [ ]  | `D01 T14 §7`  | System-Wide Multi-Instance Spinner                                      |
| [ ]  | `D08 T09 §3`  | Wallpaper Engine                                                        |
| [ ]  | `D08 T09 §4`  | DPI Scaling                                                             |
| [ ]  | `D08 T09 §1`  | Context Menu Engine                                                     |
| [ ]  | `D06 T06 §7`  | Icon Right-Click Context Menu                                           |
| [ ]  | `D08 T09 §2`  | Desktop Right-Click Menu                                                |
| [/]  | `D06 T05 §2`  | Desktop Right-Click Context Menu                                        |
| [ ]  | `D08 T09 §5`  | Screenshot                                                              |
| [ ]  | `D08 T09 §6`  | Night Light                                                             |
| [ ]  | `D08 T09 §7`  | Focus / Do Not Disturb                                                  |
| [ ]  | `D08 T09 §8`  | Quick Settings Panel                                                    |
| [ ]  | `D08 T09 §9`  | Virtual Desktops                                                        |
| [ ]  | `D08 T10 §1`  | Taskbar Window List                                                     |
| [/]  | `D06 T05 §3`  | Taskbar Window State Sync                                               |
| [ ]  | `D08 T10 §3`  | Aero Peek                                                               |
| [ ]  | `D08 T10 §4`  | Taskbar Progress Badges                                                 |
| [ ]  | `D08 T10 §5`  | Pinned Apps                                                             |
| [ ]  | `D08 T10 §2`  | Taskbar Button Context Menu                                             |
| [ ]  | `D08 T10 §6`  | Jump Lists                                                              |
| [ ]  | `D08 T10 §7`  | Taskbar Auto-Hide                                                       |
| [ ]  | `D08 T10 §8`  | Taskbar Customization                                                   |
| [ ]  | `D08 T11 §1`  | Start Menu Data Loading                                                 |
| [ ]  | `D08 T11 §2`  | Start Menu Interaction                                                  |
| [ ]  | `D08 T11 §3`  | Start Menu Search                                                       |
| [ ]  | `D08 T11 §4`  | System Tray Icons                                                       |
| [ ]  | `D08 T11 §5`  | Toast Notifications                                                     |
| [ ]  | `D08 T11 §6`  | Notification Center                                                     |
| [ ]  | `D08 T11 §7`  | Notification Settings                                                   |
| [ ]  | `D08 T12 §1`  | Kernel Time API                                                         |
| [ ]  | `D08 T12 §2`  | Time Formatting                                                         |
| [ ]  | `D08 T12 §5`  | Timezone Database                                                       |
| [ ]  | `D08 T12 §3`  | Taskbar Clock                                                           |
| [ ]  | `D08 T12 §4`  | Date/Time Control Panel                                                 |
| [ ]  | `D08 T12 §6`  | Monotonic Uptime Extension                                              |
| [ ]  | `D08 T13 §3`  | Build Pipeline                                                          |
| [ ]  | `D08 T13 §1`  | Boot Splash Renderer                                                    |
| [ ]  | `D08 T13 §2`  | Loading Spinner                                                         |
| [ ]  | `D08 T13 §4`  | Boot Progress Milestones                                                |
| [ ]  | `D08 T13 §5`  | F8 Boot Menu                                                            |
| [ ]  | `D08 T13 §6`  | [MOVED] Crash Loop Protection                                           |
| [ ]  | `D08 T13 §7`  | [MOVED] BSOD Auto-Restart Validation                                    |
| [ ]  | `D08 T14 §1`  | Win32 Shell Icon Index Map                                              |
| [ ]  | `D08 T14 §2`  | GDI Device Context Stubs                                                |
| [ ]  | `D08 T14 §3`  | GDI Drawing Functions                                                   |
| [ ]  | `D08 T14 §4`  | GDI Text Functions                                                      |
| [ ]  | `D08 T14 §5`  | USER32 Window Functions                                                 |
| [ ]  | `D08 T14 §6`  | USER32 Message Loop                                                     |
| [ ]  | `D08 T14 §7`  | USER32 Cursor, Icon & System Metrics                                    |
| [ ]  | `D08 T14 §8`  | USER32 Dialogs                                                          |
| [ ]  | `D08 T15 §1`  | Shadow SSDT Infrastructure (Table 1 Dispatch)                           |
| [ ]  | `D08 T15 §2`  | GDI Device Context (DC) Object Table                                    |
| [ ]  | `D08 T15 §3`  | GDI Drawing Syscalls                                                    |
| [ ]  | `D08 T15 §4`  | GDI Text and Font Syscalls                                              |
| [ ]  | `D08 T15 §5`  | GDI Bitmap and DIB Syscalls                                             |
| [ ]  | `D08 T15 §6`  | GDI Pen, Brush, and Region Syscalls                                     |
| [ ]  | `D08 T15 §7`  | USER Window Management Syscalls                                         |
| [ ]  | `D08 T15 §8`  | USER Message Queue Syscalls                                             |
| [ ]  | `D08 T15 §9`  | USER Input and Cursor Syscalls                                          |
| [ ]  | `D08 T15 §10` | USER Menu and Accelerator Syscalls                                      |
| [ ]  | `D08 T15 §11` | USER Clipboard Syscalls                                                 |
| [ ]  | `D08 T15 §12` | Migrate Legacy SYS_* to Shadow SSDT                                     |
| [ ]  | `D08 T15 §13` | GDI Path, Curve, and Extended Drawing                                   |
| [ ]  | `D08 T15 §14` | GDI Transform, Palette, and Color Management                            |
| [ ]  | `D08 T15 §15` | GDI Print and Metafile                                                  |
| [ ]  | `D08 T15 §16` | GDI Font Advanced                                                       |
| [ ]  | `D08 T15 §17` | GDI Extended Object Management                                          |
| [ ]  | `D08 T15 §18` | USER Window Properties, Styles, and Enumeration                         |
| [ ]  | `D08 T15 §19` | USER Dialog, Caret, and Drawing Helpers                                 |
| [ ]  | `D08 T15 §20` | USER Scrollbar                                                          |
| [ ]  | `D08 T15 §21` | USER Keyboard, IME, and Hook                                            |
| [ ]  | `D08 T15 §22` | USER DPI, Accessibility, and System Parameters                          |
| [ ]  | `D08 T15 §23` | USER Raw Input, Touch, and Gesture                                      |
| [ ]  | `D08 T15 §24` | USER Multi-Monitor and Display                                          |
| [ ]  | `D08 T15 §25` | USER Shell Integration                                                  |
| [ ]  | `D08 T15 §26` | GDI/USER DirectX and DXGI Kernel Integration                            |
| [ ]  | `D08 T15 §27` | Impossible OS Exclusive Graphics Extensions                             |
| [ ]  | `D08 T16 §1`  | Dispatch split and guard rails                                          |
| [ ]  | `D08 T16 §2`  | NTSTATUS and user-buffer rules                                          |
| [ ]  | `D08 T16 §3`  | Static asserts, headers, and audits                                     |
| [ ]  | `D08 T16 §4`  | User-mode index contract                                                |
| [ ]  | `D08 T16 §5`  | KeUserModeCallback integration review                                   |

### Phase 9 -- Desktop shell

The taskbar, Start, the file manager and the settings surfaces are built from the controls of Phase 8 and are the first thing a user judges the system by.

| Done | Section       | Deliverable                                   |
| :--: | ------------- | --------------------------------------------- |
| [ ]  | `D09 T01 §1`  | Kernel Clipboard Buffer                       |
| [ ]  | `D09 T01 §3`  | Win32 Clipboard API Stubs                     |
| [ ]  | `D09 T01 §5`  | Multi-Format Clipboard                        |
| [ ]  | `D09 T01 §2`  | Keyboard Shortcut Wiring                      |
| [ ]  | `D09 T01 §4`  | Clipboard History                             |
| [ ]  | `D09 T02 §1`  | Extension-to-App Mapping                      |
| [ ]  | `D09 T02 §2`  | Default Associations on First Boot            |
| [ ]  | `D09 T02 §3`  | Open With Dialog                              |
| [ ]  | `D09 T02 §4`  | Shortcut Files (.lnk)                         |
| [ ]  | `D09 T02 §5`  | Desktop Shortcut Integration                  |
| [ ]  | `D09 T02 §7`  | System Sounds                                 |
| [ ]  | `D09 T02 §8`  | Font Manager App                              |
| [ ]  | `D09 T02 §9`  | Icon Extraction: EIF SVG and PE ICO Resources |
| [ ]  | `D09 T03 §1`  | User Account Stub                             |
| [ ]  | `D09 T03 §2`  | Service Manager Core                          |
| [ ]  | `D09 T03 §3`  | Auto-Restart & Crash Recovery                 |
| [ ]  | `D09 T03 §4`  | Graceful Degradation                          |
| [ ]  | `D09 T03 §5`  | `sc` Shell Command                            |
| [ ]  | `D09 T03 §6`  | Built-In Services                             |
| [ ]  | `D09 T03 §7`  | Notification Service                          |
| [ ]  | `D09 T03 §8`  | Autostart Programs                            |
| [ ]  | `D09 T04 §1`  | Recycle Bin Core                              |
| [ ]  | `D09 T02 §6`  | Recycle Bin Desktop Icon States               |
| [ ]  | `D09 T04 §2`  | Recycle Bin Window App                        |
| [ ]  | `D09 T04 §3`  | miniz ZIP Library Integration                 |
| [ ]  | `D09 T04 §4`  | ZIP Kernel API                                |
| [ ]  | `D09 T04 §5`  | ZIP Shell Commands                            |
| [ ]  | `D09 T05 §1`  | Search Index Core                             |
| [ ]  | `D09 T05 §2`  | Search Query API                              |
| [ ]  | `D09 T05 §3`  | `find` Shell Command                          |
| [ ]  | `D09 T05 §4`  | Start Menu Integration                        |
| [ ]  | `D09 T05 §6`  | Index Change Notifications                    |
| [/]  | `D09 T06 §1`  | CSPRNG                                        |
| [ ]  | `D09 T06 §2`  | Password Hashing                              |
| [ ]  | `D09 T06 §3`  | User Account System                           |
| [ ]  | `D09 T06 §4`  | Authentication API                            |
| [ ]  | `D09 T06 §5`  | User Home Directories                         |
| [ ]  | `D09 T06 §6`  | Login Screen UI                               |
| [ ]  | `D09 T06 §7`  | Login Flow                                    |
| [ ]  | `D09 T06 §8`  | Lock Screen                                   |
| [ ]  | `D09 T06 §9`  | User Switching                                |
| [ ]  | `D09 T06 §10` | File Permissions                              |
| [ ]  | `D09 T06 §11` | UAC-Equivalent Elevation                      |
| [ ]  | `D09 T07 §1`  | Crypto Primitives                             |
| [ ]  | `D09 T07 §2`  | TLS Handshake Helper                          |
| [ ]  | `D09 T07 §3`  | X.509 Certificate Store                       |
| [ ]  | `D09 T07 §4`  | Key Store                                     |
| [ ]  | `D09 T07 §5`  | BCrypt API                                    |
| [ ]  | `D09 T07 §6`  | NCrypt API                                    |
| [ ]  | `D09 T07 §7`  | Code Signing                                  |
| [ ]  | `D09 T07 §8`  | EFS Integration                               |
| [ ]  | `D09 T08 §1`  | Terminal Cell Grid                            |
| [ ]  | `D09 T08 §2`  | Character Rendering                           |
| [ ]  | `D09 T08 §3`  | ANSI Escape Parser                            |
| [ ]  | `D09 T08 §4`  | Scrollback & Text Selection                   |
| [ ]  | `D09 T08 §5`  | Resize Handling                               |
| [ ]  | `D09 T08 §6`  | Registry Settings                             |
| [ ]  | `D09 T08 §7`  | Advanced Features                             |
| [ ]  | `D09 T08 §8`  | Pseudo-Console (ConPTY)                       |
| [ ]  | `D09 T09 §1`  | Core Layout                                   |
| [ ]  | `D09 T09 §2`  | Sidebar                                       |
| [ ]  | `D09 T09 §6`  | File Search                                   |
| [ ]  | `D09 T05 §5`  | File Manager Integration                      |
| [ ]  | `D09 T10 §1`  | Gap Buffer                                    |
| [ ]  | `D09 T10 §2`  | Text Rendering & Cursor                       |
| [ ]  | `D09 T10 §3`  | Undo/Redo                                     |
| [ ]  | `D09 T10 §4`  | File Menu                                     |
| [ ]  | `D09 T10 §5`  | Editing Features                              |
| [ ]  | `D09 T10 §6`  | Find & Replace                                |
| [ ]  | `D09 T10 §7`  | Stretch Features                              |
| [ ]  | `D09 T09 §3`  | View Modes                                    |
| [ ]  | `D09 T11 §1`  | CPL Framework                                 |
| [ ]  | `D09 T11 §2`  | Control Panel Host App                        |
| [ ]  | `D09 T11 §3`  | Core Applets                                  |
| [ ]  | `D09 T11 §4`  | Additional Applets                            |
| [ ]  | `D09 T11 §5`  | Settings Search                               |
| [ ]  | `D09 T03 §9`  | Win32 System Info Stubs                       |
| [ ]  | `D09 T04 §6`  | Task Scheduler Core                           |
| [ ]  | `D09 T04 §7`  | Built-In Scheduled Tasks                      |
| [ ]  | `D09 T04 §8`  | `at` Shell Command                            |
| [ ]  | `D09 T09 §4`  | File Operations                               |
| [ ]  | `D09 T09 §5`  | Context Menus                                 |
| [ ]  | `D09 T09 §7`  | Drag and Drop                                 |
| [ ]  | `D09 T09 §8`  | Advanced Features                             |
| [ ]  | `D09 T12 §1`  | Shell Command Expansion                       |
| [ ]  | `D09 T12 §2`  | Calculator                                    |
| [ ]  | `D09 T12 §3`  | Image Viewer                                  |
| [ ]  | `D09 T12 §4`  | Screenshot Enhancements                       |
| [ ]  | `D09 T12 §5`  | Archive Manager                               |
| [ ]  | `D09 T12 §6`  | Calendar App                                  |
| [ ]  | `D09 T12 §7`  | Task Manager                                  |
| [ ]  | `D09 T12 §8`  | Device Manager                                |
| [ ]  | `D09 T12 §9`  | System Info                                   |
| [ ]  | `D09 T13 §3`  | Taskbar and desktop integration               |
| [ ]  | `D09 T14 §1`  | Post-Desktop-Init Test Harness                |
| [ ]  | `D09 T14 §2`  | Failure Capture Hook and Artifact Bundle      |
| [ ]  | `D09 T14 §3`  | Committed JSONL Input Traces                  |
| [ ]  | `D09 T14 §4`  | Snapshot-Time Read-Side Sync                  |
| [ ]  | `D09 T14 §5`  | Virtio-GPU Multi-Output Driver                |
| [x]  | `D09 T14 §6`  | ObjectManager FrameStats Pseudo-File          |
| [/]  | `D09 T14 §7`  | QEMU HMP Monitor UNIX-Socket Hardening        |
| [/]  | `D09 T14 §8`  | Visual Regression Shared-Session Refactor     |

### Phase 10 -- Platform services

Audio, updates and packages, restore, accessibility, the Win32 PE loader and API surface, and Linux compatibility need the kernel, storage and the shell in place, and the apps of Phase 11 need them.

| Done | Section       | Deliverable                                                                                                            |
| :--: | ------------- | ---------------------------------------------------------------------------------------------------------------------- |
| [ ]  | `D10 T01 §1`  | Audio Abstraction Layer                                                                                                |
| [ ]  | `D10 T01 §2`  | Audio Mixer                                                                                                            |
| [ ]  | `D10 T01 §3`  | WAV Decoder                                                                                                            |
| [ ]  | `D10 T01 §4`  | MP3 Decoder                                                                                                            |
| [ ]  | `D10 T01 §5`  | OGG Vorbis Decoder                                                                                                     |
| [ ]  | `D10 T01 §6`  | FLAC Decoder (Stretch)                                                                                                 |
| [ ]  | `D10 T01 §7`  | Unified Audio Loader                                                                                                   |
| [ ]  | `D10 T01 §9`  | Volume Control Popup                                                                                                   |
| [ ]  | `D10 T02 §1`  | Canvas & Viewport                                                                                                      |
| [ ]  | `D10 T02 §7`  | Zoom & Resize (Stretch)                                                                                                |
| [ ]  | `D10 T04 §1`  | Event Log                                                                                                              |
| [ ]  | `D10 T04 §7`  | F8 Recovery Environment                                                                                                |
| [ ]  | `D10 T04 §8`  | Factory Reset                                                                                                          |
| [ ]  | `D10 T04 §9`  | Startup Repair                                                                                                         |
| [ ]  | `D10 T05 §1`  | Screensaver System                                                                                                     |
| [ ]  | `D10 T05 §3`  | Desktop Widget Framework                                                                                               |
| [ ]  | `D10 T06 §1`  | High Contrast Mode                                                                                                     |
| [ ]  | `D10 T06 §2`  | Large Text / DPI Override                                                                                              |
| [ ]  | `D10 T06 §3`  | Sticky Keys                                                                                                            |
| [ ]  | `D10 T06 §6`  | Mouse Keys                                                                                                             |
| [ ]  | `D10 T06 §7`  | Reduced Motion & Color Blind Mode                                                                                      |
| [ ]  | `D10 T05 §2`  | Screensaver → Lock Screen                                                                                              |
| [ ]  | `D10 T07 §1`  | Fix Ring-3 User-Mode Execution                                                                                         |
| [ ]  | `D10 T07 §2`  | Windows x64 Syscall ABI                                                                                                |
| [ ]  | `D10 T07 §3`  | PE Header Structures                                                                                                   |
| [ ]  | `D10 T07 §4`  | PE Loader Core                                                                                                         |
| [ ]  | `D10 T07 §5`  | Base Relocations                                                                                                       |
| [ ]  | `D10 T07 §6`  | Import Resolution                                                                                                      |
| [ ]  | `D10 T07 §7`  | PE Execution                                                                                                           |
| [ ]  | `D10 T07 §8`  | EXE Format Precedence                                                                                                  |
| [ ]  | `D10 T07 §9`  | Minimal User-Mode Runtime                                                                                              |
| [ ]  | `D10 T06 §4`  | Large Cursor                                                                                                           |
| [ ]  | `D10 T08 §1`  | Win32 Type Definitions                                                                                                 |
| [ ]  | `D10 T08 §2`  | Console & Process API (`kernel32.dll` tier 1)                                                                          |
| [ ]  | `D10 T08 §3`  | C Runtime (`msvcrt.dll`)                                                                                               |
| [ ]  | `D10 T08 §4`  | NT Runtime Stubs (`ntdll.dll`)                                                                                         |
| [ ]  | `D10 T08 §5`  | Memory Management API                                                                                                  |
| [ ]  | `D10 T08 §6`  | Synchronization API                                                                                                    |
| [ ]  | `D10 T08 §7`  | DLL Loading API                                                                                                        |
| [ ]  | `D10 T08 §8`  | Error API                                                                                                              |
| [ ]  | `D10 T08 §9`  | Unimplemented Function Logger                                                                                          |
| [ ]  | `D10 T08 §10` | Window Management (`user32.dll`)                                                                                       |
| [ ]  | `D10 T08 §11` | GDI Rendering (`gdi32.dll`)                                                                                            |
| [ ]  | `D10 T08 §12` | Shell & Icon API (`shell32.dll`)                                                                                       |
| [ ]  | `D10 T08 §13` | IxUI Native Toolkit                                                                                                    |
| [ ]  | `D10 T08 §14` | Win32 Shell Integration                                                                                                |
| [ ]  | `D09 T13 §4`  | ShellExecute open-verb wiring                                                                                          |
| [ ]  | `D10 T08 §15` | Developer SDK                                                                                                          |
| [ ]  | `D10 T09 §1`  | SDK C Headers                                                                                                          |
| [ ]  | `D10 T09 §2`  | SDK GUI Headers                                                                                                        |
| [ ]  | `D10 T09 §3`  | SDK Import Libraries                                                                                                   |
| [ ]  | `D10 T09 §4`  | Cross-Compile TCC for Impossible OS                                                                                    |
| [ ]  | `D10 T09 §5`  | Install TCC on OS Disk Image                                                                                           |
| [ ]  | `D10 T09 §6`  | TCC Self-Hosting Tests                                                                                                 |
| [ ]  | `D10 T09 §7`  | TCC IxUI Integration                                                                                                   |
| [ ]  | `D10 T09 §8`  | Shell Compiler Integration                                                                                             |
| [ ]  | `D10 T09 §9`  | SDK Installer + On-OS Pre-Install                                                                                      |
| [ ]  | `D10 T09 §10` | SDK Documentation                                                                                                      |
| [ ]  | `D10 T09 §11` | GCC/Clang (Long-Term C++ Support)                                                                                      |
| [ ]  | `D10 T04 §6`  | Crash Dump Viewer                                                                                                      |
| [ ]  | `D10 TA §1`   | Tier 1 rows below kept in sync with Section 10 / T14 implementation                                                    |
| [ ]  | `D10 TA §2`   | Tier 2 rows below kept in sync with TODO-14 Sections 2 through 8                                                       |
| [ ]  | `D10 TA §3`   | Run PE export dump on pinned Win11 `user32.dll`; file deltas vs Wine scaffold in Notes column (new symbols, renames... |
| [ ]  | `D10 TA §4`   | Replace `NO_OWNING_TODO` owners in Tier 3 as `TODO-08` / `TODO-14` / `TODO-05` subsections are opened                  |
| [ ]  | `D09 T13 §1`  | Binary layout and boot                                                                                                 |
| [ ]  | `D10 TB §1`   | Tier 1 rows kept in sync with PE loader + `TODO-08` when `comctl32` is wired                                           |
| [ ]  | `D10 TB §2`   | Tier 2 roadmap bullets updated when `TODO-06` / `TODO-14` gain matching control APIs                                   |
| [ ]  | `D10 TB §3`   | Dump exports from pinned Win11 `comctl32.dll`; merge deltas into Tier 3 Notes (new names, forwards, ordinals)          |
| [ ]  | `D10 TB §4`   | Replace `NO_OWNING_TODO` in Tier 3 as owning TODO sections are opened                                                  |
| [ ]  | `D10 TC §1`   | Tier 1 rows kept in sync with PE loader + `TODO-08` when `shell32` is wired                                            |
| [ ]  | `D10 TC §2`   | Tier 2 roadmap bullets updated when Explorer / compat matrix expands                                                   |
| [ ]  | `D10 TC §3`   | Dump exports from pinned Win11 `shell32.dll`; merge deltas into Tier 3 Notes (new names, forwards, ordinals)           |
| [ ]  | `D10 TC §4`   | Replace `NO_OWNING_TODO` in Tier 3 as owning TODO sections are opened                                                  |
| [ ]  | `D09 T13 §2`  | Shell32 dependency gate                                                                                                |
| [ ]  | `D10 T10 §1`  | `task_t` Linux Flags + FD Table                                                                                        |
| [ ]  | `D10 T10 §2`  | ELF Linux Loader (SYSV Stack + auxv)                                                                                   |
| [ ]  | `D10 T10 §3`  | Syscall Entry Dispatch (`is_linux_elf` Branch)                                                                         |
| [ ]  | `D10 T10 §4`  | Linux Syscall Translation Table                                                                                        |
| [ ]  | `D10 T10 §5`  | Path Translation                                                                                                       |
| [ ]  | `D10 T10 §6`  | Linux File Descriptor Table                                                                                            |
| [ ]  | `D10 T10 §7`  | POSIX Filesystem Stubs                                                                                                 |
| [ ]  | `D10 T10 §8`  | Signal Stubs                                                                                                           |
| [ ]  | `D10 T10 §9`  | Shell ELF Integration + `[Linux]` Process Tag                                                                          |
| [ ]  | `D10 T10 §10` | Test: Static Hello + Busybox                                                                                           |
| [ ]  | `D10 T10 §11` | Future: Dynamic ELF (Stretch)                                                                                          |
| [ ]  | `D10 T11 §1`  | ISO Build Script                                                                                                       |
| [ ]  | `D10 T11 §2`  | Installer Init Process                                                                                                 |
| [ ]  | `D09 T13 §5`  | Boot-time explorer default                                                                                             |
| [ ]  | `D10 T11 §3`  | Installer GUI Wizard                                                                                                   |
| [ ]  | `D10 T11 §4`  | GPT Partition Write                                                                                                    |
| [ ]  | `D10 T11 §5`  | Partition Format (ESP FAT32 + IXFS System)                                                                             |
| [ ]  | `D10 T05 §5`  | Display Management                                                                                                     |
| [ ]  | `D10 T05 §6`  | Multi-Monitor Stubs                                                                                                    |
| [ ]  | `D10 T11 §6`  | File Copy Engine                                                                                                       |
| [ ]  | `D10 T11 §7`  | UEFI Bootloader Install + NVRAM Boot Entry                                                                             |
| [ ]  | `D10 T11 §8`  | Post-Install First Boot + OOBE Trigger                                                                                 |
| [ ]  | `D10 T11 §9`  | Validation: QEMU + Hyper-V Gen2 + VirtualBox                                                                           |
| [ ]  | `D10 T11 §10` | Stability + Performance Pass                                                                                           |
| [ ]  | `D10 T01 §10` | `mmsys.cpl` -- Sound Settings Applet                                                                                   |
| [ ]  | `D10 T12 §1`  | Kernel Debugger (GDB RSP Stub)                                                                                         |
| [ ]  | `D10 T05 §4`  | Built-in Widgets                                                                                                       |
| [ ]  | `D10 T01 §8`  | Media Player App                                                                                                       |
| [ ]  | `D10 T02 §3`  | Color System                                                                                                           |
| [ ]  | `D10 T02 §2`  | Drawing Tools                                                                                                          |
| [ ]  | `D10 T02 §4`  | Undo/Redo                                                                                                              |
| [ ]  | `D10 T02 §5`  | File Operations                                                                                                        |
| [ ]  | `D10 T02 §6`  | Selection Tool (Stretch)                                                                                               |
| [ ]  | `D10 T06 §5`  | Screen Magnifier                                                                                                       |
| [ ]  | `D10 T06 §8`  | `ease.cpl` -- Accessibility Settings Applet                                                                            |
| [ ]  | `D10 T12 §2`  | User-Mode Debugger (`debugger.exe`)                                                                                    |
| [ ]  | `D10 T12 §3`  | Developer Tools                                                                                                        |
| [ ]  | `D10 T12 §4`  | Touch Input + Gesture Recognizer                                                                                       |
| [ ]  | `D10 T12 §5`  | Gamepad / Controller Input + XInput Stubs                                                                              |
| [ ]  | `D10 T12 §6`  | Print Support                                                                                                          |
| [ ]  | `D10 T12 §7`  | Text-to-Speech (SAM → eSpeak-NG)                                                                                       |
| [ ]  | `D10 T12 §8`  | Software OpenGL (TinyGL Port)                                                                                          |
| [ ]  | `D10 T12 §9`  | Multi-User Session Management + Fast Switching                                                                         |
| [ ]  | `D10 T12 §10` | Telemetry (Opt-In, Anonymous)                                                                                          |
| [ ]  | `D10 T12 §11` | Parental Controls (`parcon.cpl`)                                                                                       |
| [ ]  | `D10 T03 §1`  | Update Check API                                                                                                       |
| [ ]  | `D10 T03 §2`  | Update Download & Verification                                                                                         |
| [ ]  | `D10 T03 §5`  | IPKG Package Format                                                                                                    |
| [ ]  | `D10 T03 §6`  | App Installer Wizard                                                                                                   |
| [ ]  | `D10 T03 §7`  | App Uninstaller                                                                                                        |
| [ ]  | `D10 T03 §8`  | `appwiz.cpl` -- Programs & Features Applet                                                                             |
| [ ]  | `D10 T03 §9`  | IPKG Build Tool (Host)                                                                                                 |
| [ ]  | `D10 T04 §2`  | Restore Point Creation                                                                                                 |
| [ ]  | `D10 T03 §3`  | Update Application                                                                                                     |
| [ ]  | `D10 T03 §4`  | `wuapp.cpl` -- Windows Update Applet                                                                                   |
| [ ]  | `D10 T04 §5`  | First-Boot Setup Wizard                                                                                                |
| [ ]  | `D10 T04 §3`  | System Rollback                                                                                                        |
| [ ]  | `D10 T04 §4`  | `rstrui.cpl` -- System Restore UI                                                                                      |
| [ ]  | `D10 T04 §10` | Disk Cleanup Utility                                                                                                   |

### Phase 11 -- Apps

The bundled applications are the proof that the platform beneath them is complete, so they follow the services they call.

| Done | Section      | Deliverable                                        |
| :--: | ------------ | -------------------------------------------------- |
| [ ]  | `D11 T01 §1` | Text-Only Browser (Phase 1)                        |
| [ ]  | `D11 T01 §2` | HTML Tokenizer + DOM Tree                          |
| [ ]  | `D11 T01 §3` | Layout Engine (Block + Inline Flow)                |
| [ ]  | `D11 T01 §4` | CSS Parser + Cascade (Stretch)                     |
| [ ]  | `D11 T01 §5` | JavaScript Engine (Long-Term Stretch)              |
| [ ]  | `D11 T01 §6` | Alternative: NetSurf / Dillo Port Evaluation       |
| [ ]  | `D11 T01 §7` | Browser Chrome                                     |
| [ ]  | `D11 T01 §8` | Browser Settings + History + Cookie Jar            |
| [ ]  | `D11 T02 §1` | FTP Protocol Core                                  |
| [ ]  | `D11 T02 §2` | FTP Shell Commands + `wget ftp://`                 |
| [ ]  | `D11 T02 §3` | `wget` Command (HTTP/HTTPS + Progress)             |
| [ ]  | `D11 T02 §4` | `curl` Command (Full Flag Set)                     |
| [ ]  | `D11 T02 §5` | FTP GUI Client (Dual-Pane, Stretch)                |
| [ ]  | `D11 T02 §6` | WiFi Framework (Stretch)                           |
| [ ]  | `D11 T05 §1` | PDF Structure Parser                               |
| [ ]  | `D11 T05 §2` | Page Tree Traversal                                |
| [ ]  | `D11 T05 §3` | Stream Decompression                               |
| [ ]  | `D11 T05 §4` | Content Stream Renderer                            |
| [ ]  | `D11 T05 §5` | Image Rendering                                    |
| [ ]  | `D11 T05 §6` | PDF Viewer UI                                      |
| [ ]  | `D11 T05 §7` | Text Search                                        |
| [ ]  | `D11 T05 §8` | Continuous Scroll View (Stretch)                   |
| [ ]  | `D11 T05 §9` | File Association                                   |
| [ ]  | `D11 T06 §1` | pl_mpeg Integration                                |
| [ ]  | `D11 T06 §2` | YCbCr → RGB Conversion                             |
| [ ]  | `D11 T06 §3` | A/V Sync Engine                                    |
| [ ]  | `D11 T06 §4` | Player UI                                          |
| [ ]  | `D11 T06 §5` | File Operations                                    |
| [ ]  | `D11 T06 §6` | Playlist                                           |
| [ ]  | `D11 T06 §7` | Subtitle Support (Stretch)                         |
| [ ]  | `D11 T06 §8` | Format Support Roadmap                             |
| [ ]  | `D11 T07 §4` | `ping` + `traceroute` Commands                     |
| [ ]  | `D11 T07 §5` | Network Diagnostic Tools: `arp -a` + `route print` |
| [ ]  | `D11 T08 §1` | Gap Buffer Text Engine                             |
| [ ]  | `D11 T08 §2` | Text Rendering + Cursor                            |
| [ ]  | `D11 T08 §3` | File Operations                                    |
| [ ]  | `D11 T08 §4` | Editing Features                                   |
| [ ]  | `D11 T08 §5` | File Associations + App Registration               |
| [ ]  | `D11 T09 §1` | Standard Calculator UI                             |
| [ ]  | `D11 T09 §2` | Arithmetic Engine                                  |
| [ ]  | `D11 T09 §3` | Memory + History                                   |
| [ ]  | `D11 T09 §4` | Scientific Mode                                    |
| [ ]  | `D11 T09 §5` | Programmer Mode                                    |
| [ ]  | `D11 T10 §1` | Rich Text Document Model                           |
| [ ]  | `D11 T10 §2` | RTF File Format                                    |
| [ ]  | `D11 T10 §3` | Rich Text Rendering + Layout Engine                |
| [ ]  | `D11 T10 §4` | Toolbar + Menus                                    |
| [ ]  | `D11 T10 §5` | Formatting Interactions                            |
| [ ]  | `D11 T10 §6` | File Operations + File Associations                |
| [ ]  | `D11 T10 §7` | Print (Stretch)                                    |
| [ ]  | `D11 T11 §1` | Image Loading + Display                            |
| [ ]  | `D11 T11 §2` | Zoom + Pan                                         |
| [ ]  | `D11 T11 §3` | Folder Navigation + Thumbnail Strip                |
| [ ]  | `D11 T11 §4` | Toolbar + Menus                                    |
| [ ]  | `D11 T11 §5` | Slideshow                                          |
| [ ]  | `D11 T11 §6` | EXIF Metadata Panel                                |
| [ ]  | `D11 T11 §7` | Basic Editing                                      |
| [ ]  | `D11 T11 §8` | File Associations + App Registration               |
| [ ]  | `D11 T12 §1` | Full + Window Capture + Hotkeys                    |
| [ ]  | `D11 T12 §2` | Region Select Overlay                              |
| [ ]  | `D11 T12 §3` | Snipping Tool UI                                   |
| [ ]  | `D11 T13 §1` | Calendar App (Recurring Events + ICS Export)       |
| [ ]  | `D11 T13 §2` | Sticky Notes                                       |
| [ ]  | `D11 T13 §3` | System Information (`sysinfo.exe`)                 |
| [ ]  | `D11 T13 §4` | On-Screen Keyboard                                 |
| [ ]  | `D11 T13 §5` | Color Picker                                       |
| [ ]  | `D11 T13 §6` | Font Manager (OS/2 + Unicode Coverage)             |
| [ ]  | `D11 T13 §7` | Shared Help / About Dialog                         |
| [ ]  | `D11 T03 §1` | SSH2 Transport Layer                               |
| [ ]  | `D11 T03 §2` | SSH Authentication                                 |
| [ ]  | `D11 T03 §3` | Interactive Channel + PTY Relay                    |
| [ ]  | `D11 T03 §4` | Shell Integration + TOFU + Known Hosts             |
| [ ]  | `D11 T03 §5` | SCP File Transfer                                  |
| [ ]  | `D11 T03 §6` | SSH Config File                                    |
| [ ]  | `D11 T03 §7` | sftp Command (Stretch)                             |
| [ ]  | `D11 T04 §1` | SMTP Client                                        |
| [ ]  | `D11 T04 §2` | POP3 Client + Message Storage                      |
| [ ]  | `D11 T04 §3` | IMAP Client (Stretch)                              |
| [ ]  | `D11 T04 §4` | Three-Panel Email GUI                              |
| [ ]  | `D11 T04 §7` | Auto-Check + Notifications + Tray                  |
| [ ]  | `D11 T04 §9` | Spam & Junk Filter                                 |
| [ ]  | `D11 T07 §1` | VNC Client                                         |
| [ ]  | `D11 T07 §2` | IRC / Chat Client                                  |
| [ ]  | `D11 T07 §3` | RSS / News Reader                                  |
| [ ]  | `D11 T07 §6` | VNC Server (Stretch)                               |
| [ ]  | `D11 T04 §6` | Account Management + Credential Store              |
| [ ]  | `D11 T04 §8` | Contacts Store                                     |
| [ ]  | `D11 T04 §5` | Compose Window                                     |

### Phase 12 -- User platform and SDK

The kernel libraries, the process ABI, ntdll, the Win32 subsystem in user mode, the SDK and the compatibility matrix turn the system into one that runs other people's programs. They need nearly everything before them.

| Done | Section       | Deliverable                                                                 |
| :--: | ------------- | --------------------------------------------------------------------------- |
| [ ]  | `D12 T01 §1`  | String Library Consolidation                                                |
| [ ]  | `D12 T01 §2`  | Math Library Extension                                                      |
| [ ]  | `D12 T01 §4`  | monocypher + CSPRNG + SYS_GETRANDOM                                         |
| [ ]  | `D11 T12 §4`  | Archive Manager                                                             |
| [ ]  | `D11 T12 §5`  | ZIP Shell Integration                                                       |
| [ ]  | `D12 T01 §3`  | miniz (ZIP Writer + Stream Extension)                                       |
| [ ]  | `D12 T01 §5`  | cJSON                                                                       |
| [ ]  | `D12 T01 §6`  | Mbed TLS Subset                                                             |
| [ ]  | `D12 T01 §7`  | STB Consolidation + `stb_image_write`                                       |
| [ ]  | `D12 T01 §8`  | Library Integration Tests + README                                          |
| [ ]  | `D12 T02 §1`  | Per-Process Environ Storage (SDK Contract)                                  |
| [ ]  | `D12 T02 §2`  | System Default Variables                                                    |
| [ ]  | `D12 T02 §3`  | `%VAR%` Expansion + `env_expand_path`                                       |
| [ ]  | `D12 T02 §4`  | SYS_GETENV / SYS_SETENV / SYS_UNSETENV                                      |
| [ ]  | `D12 T02 §5`  | PATH-Based Command Lookup + Session Cache                                   |
| [ ]  | `D12 T02 §8`  | `set` / `echo` / `env` / `where` Shell Commands                             |
| [ ]  | `D12 T03 §1`  | ELF Relocation Engine                                                       |
| [ ]  | `D12 T03 §2`  | Kernel Module Format `.kmod`                                                |
| [ ]  | `D12 T03 §3`  | Kernel Module Loader                                                        |
| [ ]  | `D12 T03 §4`  | Kernel Symbol Export (`EXPORT_SYMBOL`)                                      |
| [ ]  | `D12 T03 §7`  | Module Hot-Swap (Stretch)                                                   |
| [ ]  | `D12 T03 §8`  | `lsmod` / `insmod` / `rmmod` / `modprobe` Shell Commands                    |
| [ ]  | `D12 T05 §1`  | Win32 Subsystem Architecture + Message Queue                                |
| [ ]  | `D12 T05 §8`  | Common Dialog Boxes                                                         |
| [ ]  | `D12 T06 §1`  | SDK Packaging (`make sdk`)                                                  |
| [ ]  | `D12 T06 §2`  | SDK Documentation (`gendoc.c`)                                              |
| [ ]  | `D12 T06 §3`  | Code Samples                                                                |
| [ ]  | `D12 T06 §4`  | Sampling Profiler                                                           |
| [ ]  | `D12 T06 §5`  | Unit Test Framework (`itest.h`)                                             |
| [ ]  | `D12 T06 §6`  | Debugger Enhancements                                                       |
| [ ]  | `D12 T06 §7`  | IxUI Starter Templates (`ixui-new`)                                         |
| [ ]  | `D12 T06 §8`  | SDK Release Pipeline                                                        |
| [ ]  | `D12 T07 §1`  | API Coverage Tracker                                                        |
| [ ]  | `D12 T07 §2`  | Bring-Up Ladder Framework                                                   |
| [ ]  | `D12 T07 §3`  | Tier 1 -- Process Exit Only                                                 |
| [ ]  | `D12 T07 §4`  | Tier 2 -- Console I/O                                                       |
| [ ]  | `D12 T07 §6`  | Tier 4 -- Process & Registry                                                |
| [ ]  | `D12 T07 §7`  | Tier 5 -- Memory & Sync                                                     |
| [ ]  | `D12 T07 §5`  | Tier 3 -- File I/O                                                          |
| [ ]  | `D12 T07 §10` | Tier 8 -- Extended Win32 Surface                                            |
| [ ]  | `D12 T07 §11` | Stub Call Log Analysis                                                      |
| [ ]  | `D12 T03 §6`  | GOT / PLT Lazy Binding                                                      |
| [ ]  | `D12 T03 §5`  | `dlopen` / `dlsym` for Linux Compat                                         |
| [ ]  | `D12 T04 §1`  | ntdll.dll Structure + TEB / PEB Extensions                                  |
| [ ]  | `D12 T02 §6`  | argv / argc Process ABI + `cmd_tokenize`                                    |
| [ ]  | `D12 T02 §7`  | Shell `.profile` Startup Script                                             |
| [ ]  | `D12 T04 §2`  | RtlHeap Process Heap Allocator                                              |
| [ ]  | `D12 T04 §3`  | LdrLoadDll (PE DLL Loader)                                                  |
| [ ]  | `D12 T04 §4`  | Thread-Local Storage (TLS)                                                  |
| [ ]  | `D12 T04 §5`  | Vectored Exception Handling (VEH)                                           |
| [ ]  | `D12 T04 §6`  | Process Startup (CRT0)                                                      |
| [ ]  | `D12 T04 §7`  | User-Mode libc Shims                                                        |
| [ ]  | `D12 T04 §8`  | Fiber API                                                                   |
| [ ]  | `D12 T04 §9`  | Local Atom Tables (RtlAtomTable)                                            |
| [ ]  | `D12 T04 §10` | WNF User Runtime (RtlPublish / RtlSubscribe)                                |
| [ ]  | `D12 T05 §2`  | Window Class Registry + Built-in Classes                                    |
| [ ]  | `D12 T05 §3`  | WndProc Dispatch + DefWindowProc                                            |
| [ ]  | `D12 T05 §4`  | Win32 Painting Model (HDC + Dirty Rect)                                     |
| [ ]  | `D12 T05 §5`  | Message Filters + Accelerator Tables                                        |
| [ ]  | `D12 T05 §6`  | Window Subclassing + Property Store                                         |
| [ ]  | `D12 T05 §7`  | Inter-Process Window Messaging                                              |
| [ ]  | `D12 T05 §9`  | Win11 Visual Opt-In Surface: `dwmapi`, `uxtheme`, Personalization Broadcast |
| [ ]  | `D12 T07 §8`  | Tier 6 -- MessageBox & Basic GUI                                            |
| [ ]  | `D12 T07 §9`  | Tier 7 -- Full Win32 Window + Controls                                      |
| [ ]  | `D12 T07 §12` | CI Compatibility Gate                                                       |
| [ ]  | `D12 T07 §13` | Tier 9 -- WinRT + DirectX + WinUI 3 / Windows App SDK Apps                  |

### Phase 13 -- Tools and accessories

The object namespace browser and the event viewer: small diagnostic programs that depend on the SDK surface and on nothing after it.

| Done | Section      | Deliverable                  |
| :--: | ------------ | ---------------------------- |
| [ ]  | `D13 T01 §1` | User-Mode Syscall Wrappers   |
| [ ]  | `D13 T01 §2` | Console-Mode ObBrowse        |
| [ ]  | `D13 T01 §3` | GUI ObBrowse with Tree View  |
| [ ]  | `D13 T01 §4` | Object Detail and Type Icons |
| [ ]  | `D13 T02 §1` | Console-Mode Log Viewer      |
| [ ]  | `D13 T02 §2` | GUI Event Viewer             |
| [ ]  | `D13 T02 §3` | Live Tail and Auto-Refresh   |

### Phase 14 -- Host tools

Tools that run on the developer's machine: the SDK build system, filesystem mount and repair, crash decoding and log analysis. They are placed here because they inspect and package what the earlier phases produce.

| Done | Section      | Deliverable                                            |
| :--: | ------------ | ------------------------------------------------------ |
| [/]  | `D14 T01 §1` | Linux Build Script                                     |
| [/]  | `D14 T01 §2` | Dependency Detection and Reporting                     |
| [/]  | `D14 T01 §3` | Auto-Discovery of SDK Tool Directories                 |
| [/]  | `D14 T02 §1` | Shared IXFS Core Parser                                |
| [/]  | `D14 T02 §2` | Disk I/O Layer with GPT                                |
| [/]  | `D14 T02 §3` | Linux FUSE Mount (Read-Only)                           |
| [/]  | `D14 T02 §4` | Write Support                                          |
| [/]  | `D14 T02 §5` | USB Auto-Mount Script                                  |
| [ ]  | `D14 T02 §6` | Resync the SDK Parser with the IXFS v2 On-Disk Format  |
| [ ]  | `D14 T01 §4` | SDK Tools in CI and Honest Skip Reporting              |
| [ ]  | `D14 T03 §1` | Symbol Map Parser                                      |
| [ ]  | `D14 T03 §2` | Source Context Display                                 |
| [ ]  | `D14 T03 §3` | Error Code Decoder                                     |
| [ ]  | `D14 T03 §4` | Memory Region Mapper                                   |
| [ ]  | `D14 T03 §5` | Full Crash Decode Mode                                 |
| [ ]  | `D14 T04 §1` | Serial Log Parser                                      |
| [ ]  | `D14 T04 §2` | Register Dump Decoder                                  |
| [ ]  | `D14 T04 §3` | Timeline Extraction                                    |
| [ ]  | `D14 T04 §4` | Root Cause Hypothesis Engine                           |
| [ ]  | `D14 T05 §1` | Log Parser                                             |
| [ ]  | `D14 T05 §2` | Timing Analysis                                        |
| [ ]  | `D14 T05 §3` | Warning/Error Highlighter                              |
| [ ]  | `D14 T05 §4` | Boot Comparison                                        |
| [ ]  | `D14 T05 §5` | HTML Report                                            |
| [ ]  | `D14 T06 §1` | GPT Partition Table Parser                             |
| [ ]  | `D14 T06 §2` | IXFS Superblock + Inode Inspector                      |
| [ ]  | `D14 T06 §3` | Directory Listing                                      |
| [ ]  | `D14 T06 §4` | Hex Dump and Raw Sector Read                           |
| [ ]  | `D14 T06 §5` | Interactive Shell                                      |
| [ ]  | `D14 T07 §1` | Superblock Validation                                  |
| [ ]  | `D14 T07 §2` | Block Bitmap Verification                              |
| [ ]  | `D14 T07 §3` | Inode Table Scan                                       |
| [ ]  | `D14 T07 §4` | Extent Integrity Check                                 |
| [ ]  | `D14 T07 §5` | Directory Structure + Orphan Detection                 |
| [ ]  | `D14 T07 §6` | Cross-Reference                                        |
| [ ]  | `D14 T07 §7` | Repair Mode                                            |
| [ ]  | `D14 T08 §1` | GPT Parser -- Find BlackBox Partition by Name          |
| [ ]  | `D14 T08 §2` | FAT32 Reader -- Traverse Dirs, Read Files              |
| [ ]  | `D14 T08 §3` | `blackbox extract` -- Dump All Logs to Local Directory |
| [ ]  | `D14 T08 §4` | `blackbox cat` -- View a Specific Log File             |
| [ ]  | `D14 T08 §5` | `blackbox events` -- Parse and Filter events.jsonl     |
| [ ]  | `D14 T08 §6` | `blackbox boot` -- Show Boot Timeline from JSON        |
| [ ]  | `D14 T08 §7` | `blackbox verify` -- HMAC Chain Verification           |
| [ ]  | `D14 T08 §8` | Windows Build                                          |
| [ ]  | `D14 T08 §9` | Shell Wrapper Scripts for Claude Code Integration      |

### Phase 15 -- Installer and release

The installer, the release pipeline, hosting and the status page run when there is a complete system to install and ship.

| Done | Section      | Deliverable                                    |
| :--: | ------------ | ---------------------------------------------- |
| [ ]  | `D15 T01 §1` | Versioning Scheme                              |
| [ ]  | `D15 T01 §2` | GPT Disk Image Release                         |
| [ ]  | `D15 T01 §3` | USB-Bootable Image                             |
| [ ]  | `D15 T01 §4` | Bootable ISO (Joliet + Rock Ridge)             |
| [ ]  | `D15 T01 §5` | Code Signing                                   |
| [ ]  | `D15 T01 §7` | VM Image Variants                              |
| [ ]  | `D15 T01 §9` | Third-Party Attribution Shipped With Artifacts |
| [ ]  | `D15 T02 §1` | Answer File Format + INI Parser                |
| [ ]  | `D15 T02 §2` | Unattended Install Mode                        |
| [ ]  | `D15 T02 §3` | `sysprep.exe` (Generalize + OOBE Seal)         |
| [ ]  | `D15 T02 §4` | OEM Customization                              |
| [ ]  | `D15 T02 §5` | WIM / Image Capture (`imagex.exe`)             |
| [ ]  | `D15 T02 §6` | Network Boot (PXE + TFTP Server)               |
| [ ]  | `D15 T02 §7` | VM Provisioning Templates                      |
| [ ]  | `D15 T02 §8` | Enterprise Deployment Guide                    |
| [ ]  | `D15 T03 §1` | Version Manifest API                           |
| [ ]  | `D15 T03 §4` | IPKG Package Repository                        |
| [ ]  | `D15 T03 §5` | Package Submission                             |
| [ ]  | `D15 T03 §6` | Update Delta Packages                          |
| [ ]  | `D15 T03 §7` | Telemetry Pipeline                             |
| [ ]  | `D15 T04 §1` | Automated Regression Test Suite                |
| [ ]  | `D15 T04 §2` | QEMU Validation                                |
| [ ]  | `D15 T04 §3` | Hyper-V Certification                          |
| [ ]  | `D15 T04 §4` | VirtualBox Certification                       |
| [ ]  | `D15 T04 §6` | Performance Benchmarks                         |
| [ ]  | `D15 T04 §8` | Crash Analytics Review                         |
| [ ]  | `D15 T04 §5` | Real Hardware Test Checklist                   |
| [ ]  | `D15 T04 §7` | Release Readiness Checklist                    |
| [ ]  | `D15 T01 §6` | Artifact Manifest                              |
| [ ]  | `D15 T01 §8` | Reproducible Builds                            |
| [ ]  | `D15 T03 §2` | CDN & Hosting                                  |
| [ ]  | `D15 T05 §1` | GitHub Release Workflow                        |
| [ ]  | `D15 T03 §3` | Release Promotion Pipeline                     |
| [ ]  | `D15 T05 §2` | Changelog Discipline                           |
| [ ]  | `D15 T03 §8` | Status Page                                    |
| [ ]  | `D15 T05 §3` | Contribution Guide Overhaul                    |
| [ ]  | `D15 T05 §4` | Issue & PR Templates                           |
| [ ]  | `D15 T05 §5` | README Overhaul                                |
| [ ]  | `D15 T05 §6` | Project Website                                |
| [ ]  | `D15 T05 §7` | Community Channels                             |
| [ ]  | `D15 T05 §8` | Roadmap Publication + GitHub Milestones Sync   |

### Phase 16 -- Architecture ports

The architecture abstraction layer, the AArch64 port and processor-group scaling are cheapest once the x86-64 system is whole, so they follow the release.

| Done | Section       | Deliverable                                   |
| :--: | ------------- | --------------------------------------------- |
| [ ]  | `D16 T01 §1`  | Define HAL Interface                          |
| [ ]  | `D16 T01 §2`  | Create arch/x86_64/ and Move Files            |
| [ ]  | `D16 T01 §3`  | Update Makefile                               |
| [ ]  | `D16 T01 §4`  | Update Include Paths                          |
| [ ]  | `D16 T01 §5`  | Verify 4-Platform Boot                        |
| [ ]  | `D16 T02 §1`  | UEFI AA64 Bootloader                          |
| [ ]  | `D16 T02 §2`  | Exception Vectors + EL1 Entry                 |
| [ ]  | `D16 T02 §3`  | GICv3 Interrupt Controller                    |
| [ ]  | `D16 T02 §4`  | ARM Generic Timer                             |
| [ ]  | `D16 T02 §5`  | TTBR Page Tables                              |
| [ ]  | `D16 T02 §6`  | PSCI SMP Bringup                              |
| [ ]  | `D16 T02 §7`  | SVC Syscall Entry                             |
| [ ]  | `D16 T02 §8`  | NEON/SVE Context Switch                       |
| [ ]  | `D16 T02 §9`  | ARM Security Features                         |
| [ ]  | `D16 T02 §10` | QEMU AArch64 Test Suite                       |
| [ ]  | `D16 T03 §1`  | Raise MAX_CPUS and Dynamic Per-CPU Allocation |
| [ ]  | `D16 T03 §2`  | PROCESSOR_NUMBER Type                         |
| [ ]  | `D16 T03 §3`  | KeSetTargetProcessorDpcEx                     |
| [ ]  | `D16 T03 §4`  | GROUP_AFFINITY for Affinity APIs              |
| [ ]  | `D16 T03 §5`  | MADT Parsing for >64 LAPICs                   |

### Phase 18 -- Future research

A hypervisor, a GPU compositor, an AI runtime, Android app compatibility and further ports: work with no committed place in the release. It stays planned so that it is not lost, and it runs last.

| Done | Section      | Deliverable                                        |
| :--: | ------------ | -------------------------------------------------- |
| [ ]  | `D18 T01 §1` | x86-64 Gap Analysis                                |
| [ ]  | `D18 T01 §2` | Build System Multi-Arch                            |
| [ ]  | `D18 T01 §3` | AArch64 Boot Path (Minimal Kernel Prototype)       |
| [ ]  | `D18 T01 §4` | RISC-V (RV64GC) Gap Analysis                       |
| [ ]  | `D18 T01 §5` | ABI Considerations                                 |
| [ ]  | `D18 T01 §6` | AArch64 Port Plan (`src/arch/arm64/` Layer Design) |
| [ ]  | `D18 T01 §7` | Research Deliverables                              |
| [ ]  | `D18 T02 §1` | Intel VT-x / AMD-V Gap Analysis                    |
| [ ]  | `D18 T02 §2` | Minimal Hypervisor Design (Type-1.5 VMM)           |
| [ ]  | `D18 T02 §3` | Minimal Linux Guest Proof-of-Concept               |
| [ ]  | `D18 T02 §4` | virtio Device Emulation Design (Host-Side)         |
| [ ]  | `D18 T02 §5` | Snapshot & Live Migration Research                 |
| [ ]  | `D18 T02 §6` | GPU Passthrough Research (VT-d / AMD-Vi)           |
| [ ]  | `D18 T02 §7` | Research Deliverables                              |
| [ ]  | `D18 T03 §1` | GPU Access Strategy Options                        |
| [ ]  | `D18 T03 §2` | TinyGL → Mesa lavapipe Upgrade Research            |
| [ ]  | `D18 T03 §3` | Display Engine Research (AMD DCN + Intel Arc)      |
| [ ]  | `D18 T03 §4` | Vulkan Kernel Driver Architecture                  |
| [ ]  | `D18 T03 §5` | Compositor GPU Path Design                         |
| [ ]  | `D18 T03 §6` | Research Deliverables                              |
| [ ]  | `D18 T04 §1` | TPM 2.0 CSPRNG Integration + `swtpm` Dev Setup     |
| [ ]  | `D18 T04 §2` | Measured Boot Chain (PCR 8–10 Extensions)          |
| [ ]  | `D18 T04 §3` | UEFI Secure Boot PK/KEK/db Key Hierarchy           |
| [ ]  | `D18 T04 §4` | AES-256-XTS Full Disk Encryption (FDE)             |
| [ ]  | `D18 T04 §5` | vTPM for ImpossibleHV Guests                       |
| [ ]  | `D18 T04 §6` | Research Deliverables                              |
| [ ]  | `D18 T05 §1` | Inference Runtime Options Survey                   |
| [ ]  | `D18 T05 §2` | SIMD Enablement for User Mode                      |
| [ ]  | `D18 T05 §3` | ggml C99 Kernel Port                               |
| [ ]  | `D18 T05 §4` | GGUF Model Format & Loading                        |
| [ ]  | `D18 T05 §5` | OS Integration Points (`SYS_AI_INFER` + Apps)      |
| [ ]  | `D18 T05 §6` | GPU Inference Research (Stretch)                   |
| [ ]  | `D18 T05 §7` | Research Deliverables                              |
| [ ]  | `D18 T06 §1` | Research Charter and Success Criteria              |
| [ ]  | `D18 T06 §2` | Architecture Options Record                        |
| [ ]  | `D18 T06 §3` | Host Kernel Prerequisites                          |
| [ ]  | `D18 T06 §4` | Guest Android Runtime Plan                         |
| [ ]  | `D18 T06 §5` | Display and Input Bridging                         |
| [ ]  | `D18 T06 §6` | ABI and ISA Strategy                               |
| [ ]  | `D18 T06 §7` | Distribution and Ecosystem Stance                  |
| [ ]  | `D18 T06 §8` | Milestones, Exit Gates, and Deliverables           |
