# User-Mode Test Output Formats

The user-mode test launcher emits **four** output streams on serial.
Three are machine-readable (TAP, JUnit XML, JSON), one is the
human-readable default. All four can be enabled simultaneously; the
launcher interleaves them tagged with distinct prefixes so downstream
tooling can split them by `grep`.

Governing roadmap: [TODO-04 -- User-Mode Test Framework](../../todo/00-infrastructure/TODO-04-usermode-test-framework.md).
Launcher implementation: [src/kernel/test/test_usermode.c](../../src/kernel/test/test_usermode.c).

---

## Enabling formats

Each format is controlled by an independent `boot.conf` flag. Defaults
are all `0`; opt in per-run:

| `boot.conf` key    | Effect                                                                                    |
|--------------------|-------------------------------------------------------------------------------------------|
| `test=1`           | (prereq) launcher runs at all                                                             |
| `tap=1`            | `1..N` plan line + `ok N - <name>` / `not ok N - <name>` per binary                       |
| `xml=1`            | `[UTEST-XML] <testsuite>` / `[UTEST-XML] <testcase>` + `[UTEST-XML-SUMMARY]` envelope     |
| `json=1`           | One `[UTEST-JSON] {...}` per binary + the trailing `run_report`, `summary`, `run_meta` records |
| `utest_filter=<glob>` | Restrict the run to binaries matching the glob                                        |

`scripts/test.sh` mirrors these as `QUIET=1`, `XML=1`, `JSON=1`, `TAP=1`,
`UTEST_FILTER=<glob>` command-line flags and patches them into the booted
`boot.conf`.

`XML=1` and `JSON=1` also assemble host-side artifacts --
`build/test-results.xml` and `build/test-results.json`. Both are gates: if
the requested stream never reaches serial, or reaches it in a state that
disagrees with its own summary, the run FAILS and the artifact says why.
Neither ever publishes a clean-looking empty result to stand in for a broken
pipeline.

### The report dimension (2026-07-28)

Every format now carries a THIRD outcome alongside pass and fail. A test
binary submits its own counts through `SYS_TEST_REPORT` at `UTEST_END`
(assertions passed, assertions failed, and skip BLOCKS taken), because a
process exit code has room for two outcomes only -- `0` and the
whole-binary `77`. Before this, a binary that skipped one sub-test and
passed the rest reached every machine consumer as an ordinary exit 0,
claiming coverage the run never had.

Two independent dimensions, never summed:

- **binary verdicts** -- what the exit codes said. These are the numbers
  in the legacy `=== N passed ... ===` line, in `summary.passed/failed/
  skipped`, and in one TAP point / one `<testcase>` per binary.
- **the report** -- assertion counts and skip blocks the binaries reported
  about themselves. One `UTEST_SKIP` guards a block that may contain
  several assertions, so a skip block is never comparable to an assertion
  count.

Reports are ring-3 data and are treated as untrusted. The kernel refuses a
repeat submission or an out-of-range count; the launcher then reconciles
the report against the exit status it observed and escalates a
self-contradicting binary to FAIL rather than publishing its numbers. Two
diagnostic lines carry the dimension: `[UTEST-REPORT] <name> ...` per
reporting binary, and one `[UTEST-REPORT-SUMMARY]` for the run.

---

## Record framing (always on)

**Every launcher-owned record is framed with a per-boot nonce, and every
host parser refuses a record that is not.** Ring-3 stdout and the launcher
share one serial stream -- `sys_write(fd=1, ...)` copies caller-controlled
bytes straight to `serial_putchar` with no annotation -- so before framing a
test binary could print a byte-identical summary line and the runner's
boot-completion poll would accept it, stop QEMU, and report success without
ever seeing the real verdict. The same shared namespace let a binary inject
`[UTEST-XML]` / `[UTEST-JSON]` records straight into the assembled artifact.

The nonce rides the klog SUBSYSTEM field, so records read `UTEST-<8 hex>:`
where a forgery can only manage `UTEST:`:

```
[INFO] UTEST-1a2b3c4d: [UTEST-FRAME] v=1 run=1
[ OK ] UTEST-1a2b3c4d: test_syscall.exe: PASS (exit=0)
[ OK ] UTEST-1a2b3c4d: [UTEST-FRAME-END] run=1 records=105
```

It rides the subsystem rather than the message because every record is
bounded by klog's 256-byte message field and the JSON summary already
measured 242 of 255 usable bytes; the subsystem is a `const char *`, so the
tag costs the record budget nothing.

**Learn the frame, never pattern-match it.** A consumer takes the nonce from
the TAG of the FIRST `[UTEST-FRAME]` announcement and then requires exactly
that prefix on every record it reads. Accepting any `UTEST-[0-9a-f]{8}:`
prefix accepts a GUESSED one, which is not hypothetical: `test_forge.exe`
prints `UTEST-deadbeef: [UTEST-FRAME-END]`, and against a generic pattern
that line ended the run early.

The announcement body carries NO copy of the nonce. Every ring-3-readable
disk sink (`kernel.log`, `events.jsonl`, the per-subsystem files) renders
the tag through an alias back to the plain `UTEST`, and a body copy would
survive that alias and put the value back into the log a test binary can
open. There is likewise no "frame absent" placeholder prefix: ring 3
controls serial, so any printable sentinel becomes an acceptance prefix. An
unlearned frame runs NO acceptance pattern at all.

Each run brackets itself with `[UTEST-FRAME] ... run=N` and
`[UTEST-FRAME-END] run=N`, and a terminator only counts when it closes the
open announcement with the SAME ordinal -- otherwise a `run=1` open and a
`run=2` close reconcile, and two runs can compensate each other's count
errors across the boundary.

First announcement wins: the launcher announces before it creates any ring-3
task, so nothing of that run can have printed ahead of it. A later, different
nonce is an imitator -- its records never match the learned prefix, and
`scripts/test.sh` additionally fails the run on the conflict. It does not
discard the frame, because that would let one binary printing one string
throw away every genuine result.

**`[UTEST-FRAME-END]` is the run's completion signal, not the summary.**
`records=` counts every framed record emitted before it, so a consumer that
saw the whole stream counts exactly `records + 1` framed lines for that run.
A binary that prints a plausible summary and then hangs cannot produce this
line, and a truncated stream cannot reconcile. `test_usermode_run()` is safe
to call repeatedly: the nonce is generated once per boot and never
rewritten (klog retains the subsystem pointer, so rewriting it would
re-attribute retained entries), while each run gets its own ordinal and its
own record count.

The mechanism's boundary is framing, not cryptography: the threat model is a
BUGGY binary printing something that matches a launcher pattern by accident
as much as a hostile one. Ring 3 cannot read the nonce back through any
syscall, but a binary that opens the on-disk kernel log can read it there.

---

## Human-readable default (always on)

Every binary produces two kernel-side lines:

```
[INFO] UTEST-1a2b3c4d: <name>: PASS (exit=0)
[ OK ] UTEST-1a2b3c4d: === N passed, M failed, K skipped of T total ===
```

Plus whatever the user-mode harness writes via `[PASS]` / `[FAIL]` /
`[UTEST-BEGIN]` / `[UTEST-END]` lines. This stream is parsed by
`scripts/test.sh` step 5 for the terminal summary; every machine-
readable format is derived from the same verdict.

---

## TAP (Test Anything Protocol)

Enabled with `boot.conf tap=1` (or `scripts/test.sh QUIET=1` + a later
TAP flag). Emits TAP-13 shape:

```
ok 1 - test_harness_smoke.exe
ok 2 - test_harness_smoke.exe::skipped-block-1 # SKIP reported by binary
ok 3 - test_syscall.exe
not ok 4 - test_leaky.exe # exit=2
1..4
```

**The plan is TRAILING** (changed 2026-07-28; TAP-13 permits it at either
end). A binary that reports skip blocks contributes one extra point per
block, so the launcher cannot state the total until every binary has run.
A skipped block gets its own point, named `<binary>::skipped-block-<k>`,
carrying a real `# SKIP` directive -- the binary's own point keeps its
true verdict, because reporting a partly-verified binary as skipped is
just the opposite falsehood. The label is positional because the kernel
receives a COUNT, not the identity of each skipped block.

After a `Bail out!` the stream ends: no further points and **no plan**,
per the TAP contract that nothing follows a bail-out. `scripts/test.sh`
treats any observed bail-out as a run failure, since an aborted suite left
most binaries unexecuted.

**A point the launcher cannot represent faithfully is emitted as a
FAILURE, never omitted and never silently shortened:**

```
[UTEST-RECORD-OVERFLOW] TAP point 7 could not be represented
not ok 7 - unrepresentable # TAP record refused
```

Two cases produce it, and both used to corrupt the point silently. TAP
puts its directive LAST, and every record is bounded by the 256-byte
transport, so a name long enough to push `# SKIP` past the limit turned a
skipped test into a bare passing `ok`; synthetic skip-record points, whose
names are built into a `VFS_MAX_NAME + 24` buffer, lost it for any long
parent. And a name containing `#` would choose its own TAP meaning --
`test_a # SKIP fake.exe` reads as a skip -- because the name validator
rejects path characters and control bytes but permits `#`, and
glob-discovered names come from the filesystem rather than the manifest.
Refusing in the failing direction is the only safe option: the alternative
reads as a pass.

Lines pass through the kernel log prefix (`[INFO] UTEST-<nonce>: `), so a
TAP parser consuming the raw serial log must learn the frame, require it,
and then strip everything up to the first `1..` / `ok` / `not ok` /
`Bail out!` token per line.
`scripts/test.sh`'s post-processor does not currently produce a
standalone TAP file (TAP is less useful than XML/JSON for CI), but
nothing stops a downstream consumer from writing one.

---

## JUnit XML (`xml=1`)

JUnit XML is the de-facto CI test-result format. GitLab's
`artifacts.reports.junit`, GitHub Actions' `actions/upload-artifact`
+ test annotation readers, and Jenkins' JUnit plugin all parse it
natively.

### Wire format on serial

Because the launcher does not know the final test counts until after
every binary runs, the `<testsuite ...>` opener uses placeholder zero
attributes; the real counts arrive in a trailing
`[UTEST-XML-SUMMARY]` line. `scripts/test.sh`'s post-processor
harvests both streams and regenerates a valid file at
`build/test-results.xml`.

```
[UTEST-XML] <testsuite name="impossible-os-usermode" tests="0" failures="0" skipped="0" errors="0" time="0">
[UTEST-XML] <testcase name="test_harness_smoke.exe" classname="correctness" time="0.080"/>
[UTEST-XML] <testcase name="test_leaky.exe" classname="correctness" time="0.090"><failure message="1 handle(s) leaked"/></testcase>
[UTEST-XML] <testcase name="test_skip_me.exe" classname="correctness" time="0.010"><skipped message="exit=77"/></testcase>
[UTEST-XML-SUMMARY] tests=3 failures=1 skipped=1 time=0.180 aborted=0 not_run=0
[UTEST-XML] </testsuite>
```

`aborted=` and `not_run=` are the run-completeness dimension: `aborted=1`
means the smoke gate cut the suite short and `not_run=` counts the planned
binaries that never executed. They are appended after `time=` because every
host parser is a greedy `.*<key>=(...)` expression plus an end-unanchored
validating grep, so trailing fields extend the line without disturbing any
existing extraction. `scripts/test.sh` fails the run if they are absent and
publishes an infrastructure-error document instead of assembling a normal
one: it boots the kernel it just built, so a summary without them is
producer drift, not an old artifact, and a document asserting
`aborted="false"` would be a claim the stream never made.

### Element schema

```xml
<testsuite
    name="impossible-os-usermode"
    tests="N"              <!-- RECORDS: binaries that ran + skip-block records
                                 + the synthetic abort record when aborted -->
    failures="N"           <!-- verdict == FAIL (incl. leaks/timeouts/isolation) -->
    skipped="N"            <!-- binaries that exited 77 + skip-block records -->
    errors="N"             <!-- 1 when the smoke gate aborted the suite, else 0 -->
    time="S.MMM"           <!-- wall-clock seconds for the whole suite -->
    timestamp="..."        <!-- ISO-8601 UTC, xs:dateTime with the trailing Z -->
    hostname="...">        <!-- uname -n, filtered to [A-Za-z0-9._-] -->
  <!-- Always emitted, and always the first child (the JUnit schema requires
       that position). Absence means the artifact predates the completeness
       dimension; it never means the run completed. -->
  <properties>
    <property name="aborted" value="true|false"/>
    <property name="not_run" value="N"/>
    <!-- Run identity. Present on every document this pipeline writes,
         including the synthetic error documents and the refusal envelopes:
         an artifact nobody can place is the one a dashboard most needs to
         place. Identity never softens a refusal -- errors="1" and the
         <error> element stay exactly as they were. -->
    <property name="schema" value="utest-run-identity-v1"/>
    <property name="run_id" value="TIMESTAMP-PID-RANDOM"/>
    <property name="commit" value="SHA[-dirty]"/>
    <property name="leg" value="HOST-ACCEL-Ncpu[-ciparity][-LABEL]"/>
    <property name="leg_source" value="derived|derived+override"/>
    <property name="accel" value="kvm|tcg"/>
    <property name="cpus" value="N"/>
    <property name="qemu" value="QEMU-BINARY-BASENAME"/>
    <property name="host" value="wsl2|linux|..."/>
    <property name="ci_parity" value="true|false"/>
  </properties>
  <!-- Emitted ONLY on an aborted run. The properties above are queryable but
       inert: the smoke gate aborts on any non-PASS verdict INCLUDING skip,
       and a skipped smoke leaves failures="0" with every remaining count
       internally consistent -- so a plain JUnit consumer would report a run
       where most binaries never executed as green. This record, and the
       errors="1" that matches it, are what make the document itself red. -->
  <testcase name="suite-abort" classname="infrastructure">
    <error message="smoke gate aborted the suite; N binaries never ran"/>
  </testcase>
  <testcase
      name="test_<stem>.exe"
      classname="correctness"     <!-- test-type taxonomy label -->
      time="S.MMM">               <!-- per-binary wall-clock seconds -->
    <!-- PASS: self-closing <testcase ... /> -->
    <failure message="..."/>      <!-- FAIL: launcher-formatted reason -->
    <skipped message="..."/>      <!-- SKIP (exit=77): optional reason -->
  </testcase>
  <!-- One synthetic record per skip BLOCK a binary reported. classname is
       always "skip-block", which is how a consumer separates these from
       real binaries structurally rather than by parsing the name. The
       parent binary keeps its own true verdict above. -->
  <testcase name="test_<stem>.exe::skipped-block-<k>" classname="skip-block" time="0.000">
    <skipped message="sub-test block skipped (reason on serial log)"/>
  </testcase>
</testsuite>
```

`tests=` counts RECORDS, not binaries, so it equals the number of
`<testcase>` elements in the file and `skipped=` equals the number of
`<skipped/>` elements. To recover the binary count alone, subtract the
`skip_records` value from the JSON summary or the `[UTEST-REPORT-SUMMARY]`
line.

### CI integration

**GitHub Actions** -- upload the file as an artifact so a workflow like
[dorny/test-reporter](https://github.com/dorny/test-reporter) can read
it:

```yaml
- name: Upload JUnit XML
  uses: actions/upload-artifact@v4
  with:
    name: usermode-test-results
    path: build/test-results.xml
```

**GitLab CI** -- report directly via the built-in junit schema:

```yaml
artifacts:
  when: always
  reports:
    junit: build/test-results.xml
```

**Jenkins** -- use the JUnit plugin:

```groovy
junit 'build/test-results.xml'
```

---

## JSON lines (`json=1`)

Typed schema for downstream automation (dashboards, trend analysis,
regression bisect). `scripts/test.sh JSON=1` assembles the record stream
into `build/test-results.json` -- see "Assembled artifact" below, which is
what a consumer should read. This section documents the wire stream behind
it.

### Wire format

```
[UTEST-JSON] {"record_kind":"binary","name":"test_harness_smoke.exe","type":"correctness","status":"PASS","time_ms":80,"asserts_passed":3,"asserts_failed":0,"skip_blocks":1}
[UTEST-JSON] {"record_kind":"skip_block","name":"test_harness_smoke.exe::skipped-block-1","parent":"test_harness_smoke.exe","skip_index":1,"status":"SKIP","reason":"sub-test block skipped (reason on serial log)"}
[UTEST-JSON] {"record_kind":"binary","name":"test_leaky.exe","type":"correctness","status":"FAIL","time_ms":90,"reason":"1 handle(s) leaked"}
[UTEST-JSON] {"record_kind":"binary","name":"test_skip_me.exe","type":"correctness","status":"SKIP","time_ms":10,"reason":"exit=77"}
[UTEST-JSON] {"record_kind":"run_report","asserts_passed":3,"asserts_failed":0,"skip_blocks":1,"skip_records":1,"binaries_reported":1,"binaries_invalid":0,"binaries_unreported":2}
[UTEST-JSON] {"summary":{"passed":1,"failed":1,"skipped":1,"total":3,"time_ms":180}}
[UTEST-JSON] {"record_kind":"run_meta","aborted":false,"not_run":0}
```

**Why the run-level data is split across three records.** Every record
crosses to the host through `klog`, whose ring entry is `message[256]` and
which bounds the formatted message to that size. A record wider than that is
cut mid-object in transit, and a truncated JSON object is unparseable rather
than merely short -- silently. With the assertion dimension nested inline the
summary record measured 242 of the 255 usable bytes on a live run, so it
overflowed on a suite with four-digit binary counts. Each dimension therefore
gets its own record, sized to fit every field at its `uint32` maximum, and
the host assembler nests them back together. The constraint stops at the
wire: a consumer of `build/test-results.json` sees one summary object.

The stream always ends `run_report`, `summary`, `run_meta`, in that order.
The assembler rejects any other tail as a cut-and-resumed stream.

**`record_kind` leads every record and is the stream's discriminator.**
The stream carries FOUR kinds, and a consumer must accept all of them:
`binary` (one per binary), `skip_block` (one per synthetic skip record),
plus the two run-level records `run_report` and `run_meta`. A consumer
that counted every object with a `name` would report more testcases than
`summary.total` and skew every pass rate derived from it; one that
accepted only the two name-bearing kinds would reject the run-level
records the harvester requires. Key on `record_kind`, never on the
presence of `name`.

`summary.total` counts BINARIES. The synthetic records are counted
separately as `summary.reported.skip_records`, so
`records == total + skip_records`.

`summary.reported` is the report dimension described at the top of this
document. `binaries_unreported` is the partition slot for every binary
WITHOUT an accepted self-report -- a binary that never called
`SYS_TEST_REPORT`, one whose launch failed, and an ingest refusal that
never ran at all. It is what makes silence legible: not reporting is a
different fact from reporting zero skips, and only the second is evidence
of full coverage.
`binaries_invalid` counts every REJECTED or INCOMPLETE report, not only
exit-status contradictions. A report is invalid when the binary submitted
twice, submitted a count above its ceiling, died between claiming the
submission slot and publishing it, or reported counts that contradict its
exit status (zero failures with a failing exit, failures with exit 0, or
any report alongside the whole-binary skip status 77). All four are
trust-boundary failures rather than test outcomes: the launcher has
already failed those binaries, and their counts are excluded from the
totals rather than published.

### Field schema

Per-binary record:

| Field             | Type     | Notes                                                                 |
|-------------------|----------|-----------------------------------------------------------------------|
| `record_kind`     | string   | Always `"binary"`; discriminates from `"skip_block"` records          |
| `name`            | string   | Binary filename (validated to test_*.exe prefix/suffix at ingest)     |
| `type`            | string   | Test-type taxonomy label                                              |
| `status`          | string   | `"PASS"` / `"FAIL"` / `"SKIP"`                                        |
| `time_ms`         | integer  | Wall-clock milliseconds from `task_create` to launcher verdict        |
| `reason`          | string   | Optional; launcher-formatted fail/skip explanation                    |
| `asserts_passed`  | integer  | Optional; present only when the binary submitted an ACCEPTED report   |
| `asserts_failed`  | integer  | Optional; same condition                                              |
| `skip_blocks`     | integer  | Optional; skip SITES taken, not assertions                            |

Skip-block record (one per reported skip block):

| Field         | Type     | Notes                                                      |
|---------------|----------|------------------------------------------------------------|
| `record_kind` | string   | Always `"skip_block"`                                      |
| `name`        | string   | `<binary>::skipped-block-<k>`                              |
| `parent`      | string   | The binary that reported it -- group on this               |
| `skip_index`  | integer  | 1-based index of the skip site within that binary          |
| `status`      | string   | Always `"SKIP"`                                            |
| `reason`      | string   | Fixed text; the human-readable reason stays on serial      |

Summary record (wire) -- binary counts only:

| Field                                 | Type     | Notes                                              |
|---------------------------------------|----------|----------------------------------------------------|
| `summary.passed`                      | integer  | binaries with verdict=PASS                         |
| `summary.failed`                      | integer  | binaries with verdict=FAIL (all causes)            |
| `summary.skipped`                     | integer  | binaries with verdict=SKIP (exit=77)               |
| `summary.total`                       | integer  | `passed + failed + skipped` -- BINARIES only       |
| `summary.time_ms`                     | integer  | Suite wall-clock milliseconds                      |

`run_report` record -- the assertion dimension, nested under
`summary.reported` in the assembled artifact:

| Field                   | Type     | Notes                                              |
|-------------------------|----------|----------------------------------------------------|
| `record_kind`           | string   | Always `"run_report"`                              |
| `asserts_passed`        | integer  | assertions that held, summed over accepted reports |
| `asserts_failed`        | integer  | assertions that did not                            |
| `skip_blocks`           | integer  | skip sites taken, summed over accepted reports     |
| `skip_records`          | integer  | synthetic records emitted (== skip_blocks unless the run-wide budget clipped them) |
| `binaries_reported`     | integer  | binaries whose report was accepted                 |
| `binaries_invalid`      | integer  | binaries whose report contradicted their exit      |
| `binaries_unreported`   | integer  | binaries with no accepted self-report: never reported, launch failed, or refused at ingest |

`run_meta` record -- the run-completeness dimension, merged into `summary`
in the assembled artifact:

| Field         | Type     | Notes                                                       |
|---------------|----------|-------------------------------------------------------------|
| `record_kind` | string   | Always `"run_meta"`                                         |
| `aborted`     | boolean  | the smoke gate cut the suite short. A JSON boolean, not 0/1, so a consumer walking the object generically cannot sum it with a count |
| `not_run`     | integer  | planned binaries that never executed. Always 0 when `aborted` is false; the assembler rejects any other combination |

Emitted on EVERY run, not only aborted ones: absence must mean "this
producer predates the dimension", never "the run completed". This is the
only thing separating an aborted suite from a smaller complete one --
the gate aborts on any non-PASS smoke verdict *including SKIP*, which
leaves `failed` at 0 and every other count internally consistent.

### Assembled artifact (`build/test-results.json`)

`scripts/test.sh JSON=1` runs `scripts/utest-json-harvest.py`, which
validates the stream and publishes the document below via a temporary file
and atomic rename. Any artifact from a previous run is removed first, so a
death before publication cannot leave a stale file that reads as current.

```json
{
  "schema": "utest-json-v1",
  "testcases": [ /* every record_kind=binary record, in stream order */ ],
  "skip_blocks": [ /* every record_kind=skip_block record */ ],
  "summary": {
    "passed": 1, "failed": 1, "skipped": 1, "total": 3, "time_ms": 180,
    "reported": { /* the run_report fields */ },
    "aborted": false, "not_run": 0
  },
  "run_identity": { /* build/test-run-identity.json, verbatim; null if absent */ }
}
```

### Run identity and artifact paths

The RECORD is a per-invocation directory; every stable pathname is an alias
into it. `scripts/test.sh` derives a `run_id` in Step 0a, ahead of every exit
path, and creates `build/test-runs/<run_id>/` with a non-recursive `mkdir` --
which FAILS if the directory exists, so the identity is collision-proof rather
than merely improbable.

| Path | Meaning |
|---|---|
| `build/test-runs/<run_id>/test-results.{xml,json}` | the RECORD: immutable, never rewritten by another run |
| `build/test-runs/<run_id>/record-complete.json` | the commit marker (`utest-run-record-v1`) |
| `build/test-results.{xml,json}` | alias: THIS invocation. Cleared in Step 0a before anything can fail |
| `build/test-results-<leg>.{xml,json}` | alias: the latest COMPLETED run of that leg |
| `build/test-run-identity.json` | alias for the record's identity document |

Publication is **settle -> commit marker -> aliases**, in that order, and each
step gates the next:

1. Every format the run OWED but never published gets its refusal document, so
   the record's contents are settled before the marker describes them.
2. The marker is written. **If it cannot land, NO alias is published** and the
   run fails -- a stable pathname pointing into a record with no marker is the
   uncommitted-record exposure the marker exists to prevent.
3. The aliases are copied out, identity LAST.

Every alias write goes through a private temp plus `mv`, so no alias is ever
newer than the record it points at and none can be observed half-written. A
`SIGKILL` before the marker leaves every alias absent, which is fail-CLOSED:
absence sends a consumer to the record directory, whereas a stale alias would
answer with another run's document.

Per-file `mv` makes each alias atomic but does not make the alias SET atomic,
so **identity is the generation pointer**. It carries `run_id` and is
published after the documents, so a consumer that resolves through
`build/test-run-identity.json` and cross-checks its `run_id` against the
`run_id` inside the document it read either sees one coherent generation or
detects the skew. The record plus its marker remains the authoritative pair;
the aliases are a compatibility surface.

Both formats go through that one path. An alias that cannot be written fails
the run rather than going silently missing, and finalization happens before
the final verdict precisely so that failure can still change the exit code.

**A directory that merely exists is not a completed record.** It is created
before the run does anything, so a death can leave it holding only an identity
file or one of two requested formats. `record-complete.json` is written LAST
and names exactly which documents the record contains; check it before
trusting any of them:

```json
{ "schema": "utest-run-record-v1", "run_id": "...", "status": "complete",
  "qemu_pid": 12345, "qemu_state": "reaped",
  "xml": "test-results.xml", "json": "test-results.json",
  "identity": "test-run-identity.json" }
```

`status` is `complete` once both formats have had their chance to assemble;
an exit before that point records `incomplete`, and an absent marker means the
run never reached its own cleanup. `status` describes the RECORD, not the
verdict -- a run whose tests failed still leaves a complete record. A format
the run did not produce is `null` rather than missing. A marker that cannot be
written fails the run: a green exit over an uncommitted record would be the
same false-green this lifecycle exists to close.

`qemu_pid` names the VM this run launched (`null` for an exit that never
reached the launch) and `qemu_state` says what became of it: `none` (never
launched), `running`, `reaped` (confirmed gone), or `unreaped` (it survived
both a `SIGTERM` and a `SIGKILL`). The pair is what lets a later invocation
tell "exited cleanly" from "died with a live VM". `status` alone cannot:
finalization runs BEFORE the final verdict, so the VM is reaped inside
finalization -- ahead of the marker -- precisely so `qemu_state` is a fact at
the time the marker claims it rather than a prediction the cleanup trap has yet
to fulfil.

The pid is ALSO written to `build/test-runs/<run_id>/qemu.pid` at launch time,
as `<pid> <starttime> <boot_id> <run_id>`. That copy exists because a `SIGKILL`
prevents any marker from ever being written, which is exactly the case an
orphan investigation cares about; see
[Orphaned-QEMU recovery](#orphaned-qemu-recovery) below.

The two alias kinds mean DIFFERENT things and only one is "current".

The canonical unsuffixed pair names **this invocation**. It is cleared in Step
0a ahead of every exit path, so no preflight exit -- one occurring before the
leg is even derivable -- can leave a stale document under it, and an
incomplete run publishes its refusal there and nowhere else.

The leg-suffixed pair accumulates one per configuration and means **the latest
COMPLETED run of that leg**. No leg alias is cleared up front -- not another
configuration's, and not even the running leg's own -- because clearing it
would erase a perfectly good completed artifact on behalf of a run that might
then die. A leg alias is only ever REPLACED, and only from a record whose
marker says `complete`. A matrix that runs several legs in sequence therefore
finds every leg readable at the end, and an aborted leg leaves the previous
completed one in place rather than overwriting it with a refusal.

The leg set always describes **exactly one run**. At finalization any format
the current record does not carry is dropped from the set, and it is dropped
BEFORE the formats it does carry are published: otherwise a run producing only
XML would leave its new XML beside a previous commit's JSON, and a reader
landing mid-publication would see that same pair. In this order a reader sees
the previous generation, a missing file, or the new generation, never two
generations paired.

A reader that already holds an open descriptor keeps reading its bytes
regardless; no rename-or-pointer scheme can revoke that. **For a
generation-coherent view, resolve through the record directory and its
marker**, not by enumerating the leg pathnames; that is what the record is
for. A per-leg pointer naming the record each leg resolves to is tracked in
§34.

Records are durable but BOUNDED: the newest `UTEST_RECORD_KEEP` (default 20)
survive, pruned at the START of a run so an investigation's evidence is never
removed by the run still writing it.

`scripts/test.sh` derives identity before the build and writes it to the
record, aliased to `build/test-run-identity.json` (`utest-run-identity-v1`),
which the harvester reads via `--identity`:

Every field below is emitted by BOTH projections, and a tooling assertion
compares the two field sets so one cannot gain a field the other lacks.
`timestamp` and `hostname` ride as `<testsuite>` attributes rather than
properties; everything else is a `<property>`.

| Field | Source |
|---|---|
| `schema` | `utest-run-identity-v1`, the identity contract's own version marker |
| `run_id` | the record directory name: UTC timestamp, pid, and random suffix |
| `timestamp` | `date -u` at Step 0a, when the run started, ISO-8601 with `Z` |
| `commit` | `git rev-parse HEAD`, `-dirty` appended when tracked files differ |
| `leg` | `<host>-<accel>-<n>cpu`, plus `-ciparity` and any validated `UTEST_LEG` label |
| `leg_source` | `derived`, or `derived+override` when `UTEST_LEG` added a label |
| `host` | `wsl2` when WSL is detected, else lowercased `uname -s`, charset-filtered |
| `hostname` | `uname -n`, charset-filtered |
| `accel` | read back from the arguments QEMU receives, never from display text |
| `cpus` | `SMP_CPUS`, the same variable behind the `-smp` flag |
| `qemu` | basename of the QEMU binary actually invoked, charset-filtered |
| `ci_parity` | whether `CI_PARITY=1` selected the distro QEMU and forced TCG |

`UTEST_LEG` may only ADD a label. `scripts/test.sh` can select KVM or TCG and
nothing else, so an override that renamed a leg to `whpx`, `vbox` or
`baremetal` would publish coverage nothing executed; the derived accelerator
and CPU count stay in the name and in their own fields regardless, and the
provenance is recorded. A label that does not match `^[a-z0-9][a-z0-9-]{0,31}$`
-- the FIRST character must be alphanumeric, so a leading hyphen is refused --
is rejected rather than ignored, because the value lands in a filename.

Publication is fail-closed at both ends. The canonical pair is invalidated
before anything in the run can fail -- including the argument check above --
and an EXIT/INT/TERM trap armed before the environment preflight publishes an
identity-bearing refusal (`summary_error: "run_incomplete"`, and in XML
`errors="1"` with `aborted`/`not_run` present so a current refusal is never
read as an artifact predating the completeness dimension) for any exit that
never reached assembly. A signalled run carries `128+signo`, never 0. A failed or interrupted run therefore leaves a document that names
itself, never the previous run's success sitting where CI would upload it as
current.

**Concurrent same-tree runs are NOT a supported mode, and the refusal is
explicit.** `scripts/test.sh` takes an exclusive `flock` on
`build/.test-run.lock` and exits 1 if another run holds it. This is a
DECISION, not a limitation waiting to be lifted: the script patches the shared
`boot.conf`, boots with a fixed `build/test.log` serial sink, and copies OVMF
vars to one fixed path, so two overlapping runs corrupt each other's BOOT
state long before their artifacts could collide. Per-run records make
concurrent runs non-colliding without making them meaningful; supporting them
for real means per-invocation `boot.conf`, serial log and OVMF paths, which is
a much larger change than the artifacts and one nothing has asked for. The
leg-suffixed aliases separate CONFIGURATIONS run one after another, not
simultaneous runs.

Two details make the refusal safe. The lock is taken BEFORE the artifact
lifecycle is armed, so a refusing run touches no shared name and publishes no
document -- acquiring it after the canonical clear would delete the artifacts
of the run it is deferring to. And QEMU is launched with the lock descriptor
closed (`9>&-`): `SIGKILL` bypasses the cleanup trap, and an inherited
descriptor would let an orphaned VM hold the lock and wedge every later run.
An orphaned QEMU still owning the shared `boot.conf` after a `SIGKILL` is a
pre-existing hazard the lock neither creates nor closes; it is tracked
separately. If `flock` is unavailable the run says so and proceeds rather than
failing a lone run that would have been fine.

The artifact is a GATE, not a convenience. A stream that does not agree with
its own summary must fail the run rather than publish a smaller plausible
result. Every refusal reason it can emit:

| `summary_error`             | Meaning                                                            |
|-----------------------------|--------------------------------------------------------------------|
| `missing_stream`            | `JSON=1` was requested and no `[UTEST-JSON]` record reached serial  |
| `malformed_record`          | a record is not parseable JSON, or is not an object                 |
| `producer_overflow`         | the launcher published its own overflow marker instead of a record  |
| `unknown_record_kind`       | a record carries a `record_kind` this assembler does not know       |
| `missing_summary`           | records reached serial but the summary did not -- stream truncated  |
| `duplicate_summary`         | more than one summary record                                        |
| `missing_report`            | no `run_report` record -- the assertion dimension is absent         |
| `duplicate_run_report`      | more than one `run_report` record                                   |
| `missing_completeness`      | no `run_meta`, or its `aborted`/`not_run` are not a bool/int        |
| `duplicate_run_meta`        | more than one `run_meta` record                                     |
| `records_after_summary`     | the stream does not end `run_report`, `summary`, `run_meta`         |
| `malformed_summary`         | the summary is not an object                                        |
| `malformed_report`          | a `run_report` counter is not a non-negative integer                |
| `unknown_status`            | a binary record carries a status outside `PASS`/`FAIL`/`SKIP`       |
| `count_mismatch`            | binary count, per-status counts, or skip-record count disagree with the summary |
| `report_partition_mismatch` | `binaries_reported + binaries_invalid + binaries_unreported` does not equal `summary.total`, although the launcher increments exactly one of them per binary |
| `inconsistent_completeness` | a not-aborted run nevertheless reports binaries as not run          |
| `no_python3`                | written by `scripts/test.sh` when the assembler cannot run at all   |
| `run_incomplete`            | written by `scripts/test.sh` when a run that OWED a JSON artifact ended before assembly (early exit, signal, or a format that never published) |

On refusal it writes an explicit error envelope -- `"summary": null` plus a
`summary_error` reason and `detail` -- and exits nonzero, so `test.sh` fails
the run. `summary` is null rather than zero-filled on purpose: a consumer
keying on the summary must be unable to read a refused artifact as a clean
run that happened to have no tests. The same stance applies when `JSON=1`
was requested and no stream reached serial at all (`missing_stream`).

An empty suite is a valid, publishable state: a run whose filter selects
zero binaries writes a complete envelope with empty arrays and explicit
zeros, never a missing file.

### Extraction

The assembled artifact is the intended consumer surface; `jq` over the raw
serial stream still works for ad-hoc queries:

```sh
# Did this run actually finish, or did the smoke gate cut it short?
jq '.summary | {total, aborted, not_run}' build/test-results.json

# Per-binary pass rate. The artifact already separates the record kinds, so
# there is no discriminator to remember and no way to inflate the count with
# the synthetic skip-block records:
jq -c '.testcases[] | {name, status, time_ms}' build/test-results.json

# Which binaries skipped work, and how much:
jq -c '.testcases[] | select((.skip_blocks // 0) > 0)
       | {name, skip_blocks, asserts_passed}' build/test-results.json

# Every skipped block, grouped under its binary:
jq -c '.skip_blocks | group_by(.parent)' build/test-results.json

# Trend the honesty signal: silence is not the same as zero skips.
jq '.summary.reported
    | {skip_blocks, binaries_unreported, binaries_invalid}' build/test-results.json

# Refused artifact? summary is null and the reason is named:
jq 'if .summary == null then {refused: .summary_error, detail} else "ok" end' \
   build/test-results.json
```

Reading the raw serial stream instead (no artifact, e.g. triaging a run that
refused to publish) means keying on `record_kind` yourself, and remembering
that `reported` lives in the separate `run_report` record on the wire:

```sh
# Per-binary pass-rate from serial. Prefer build/test-results.json: the
# harvester already learns the frame, binds each terminator to its
# announcement, reconciles the record count per run, and publishes only the
# last COMPLETE run. Reading serial directly means doing all of that
# yourself. If you must, use the same parser rather than a grep:
python3 scripts/utest-frame.py build/test.log     # {"nonce":..., "ok":true, ...}

# An executable recipe. It emits ONLY the parser-selected run's records: a
# full-log grep on the learned prefix still merges repeated runs, and an empty
# prefix (refused stream) degrades it to an unframed search that accepts
# ring-3 forgeries. A refusal terminates the recipe rather than continuing
# with partial data:
python3 -c '
import importlib.util, sys
spec = importlib.util.spec_from_file_location("f", "scripts/utest-frame.py")
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
r = m.parse_file("build/test.log")
if not r["ok"]:
    sys.exit("no complete framed run on serial -- refuse this log")
marker = r["prefix"] + "[UTEST-JSON] "
for line in r["lines"]:
    i = line.find(marker)
    if i >= 0:
        print(line[i + len(marker):].strip())
' | jq -c 'select(.record_kind == "binary") | {name, status}'

# The assertion dimension and the completeness dimension, from their own records:
grep '\[UTEST-JSON\] {' build/test.log |
  sed 's/.*\[UTEST-JSON\] //' |
  jq -c 'select(.record_kind == "run_report" or .record_kind == "run_meta")'
```

---

## Escaping contract

User-controlled strings (test binary names, reason messages) are
escaped before emission:

- **XML**: `&`, `<`, `>`, `"`, `'` become `&amp;` / `&lt;` / `&gt;` /
  `&quot;` / `&apos;`. Control bytes < 0x20 other than TAB/LF/CR are
  dropped (XML 1.0 forbids them).
- **JSON**: `"` -> `\"`, `\` -> `\\`, `\n` / `\r` / `\t` -> C-style
  escapes, any other control byte < 0x20 -> `\u00HH`.

Today's inputs never trigger escaping on the happy path: since the
charset gate below, an accepted binary name contains no byte either
escaper rewrites, so N accepted bytes cost N bytes on the wire. The
escape stays as defense-in-depth for reason strings and for future
consumers that might let a binary set its own.

---

## Binary name bound and refusal records

### The bound is derived, not chosen

A binary name used to be bounded only by `VFS_MAX_NAME` (256) while
every record is bounded by `UTEST_RECORD_LINE_MAX` (256, klog's
message field). A long enough name therefore pushed each emitter onto
its own overflow fallback -- the XML testcase dropped the name, the
JSON record substituted `"overflow"`, the TAP point became
`unrepresentable`, and the human verdict line lost its `: PASS` /
`: FAIL` token off the end. That last one is what the host's
fail-closed recount greps for, so the binary silently left the count.

`UTEST_MAX_BINARY_NAME` is the **minimum**, across every name-bearing
record kind, of `(UTEST_RECORD_LINE_MAX - 1 - fixed_k) / mult_k`:

| Record kind | Name appears | Room left for the name |
|---|:---:|:---:|
| Human verdict line | 1x | 196 |
| `[UTEST-XML] <testcase>` | 1x | 120 |
| `[UTEST-JSON]` `record_kind:binary` | 1x | 39 |
| `[UTEST-JSON]` `record_kind:skip_block` | **2x** | **37** |
| TAP point (skip-record shape) | 1x | 182 |
| `[UTEST-REPORT]` line | 1x | 160 |

Each `fixed_k` is summed from that kind's own format literals through
`UTEST_LIT(s)`, so editing a format string moves the bound with it.
The skip_block binds at **37** because it carries the name twice (as
`name` and again as `parent`); the JSON binary record is two bytes
behind it, which is why the derivation takes the minimum instead of
assuming the twice-carried kind is worst. Digit widths come from each
value's own cap, and `time_ms` is clamped at emit (`u_clamp_time_ms`)
so its width is a fact rather than an assumption about run length.

`%s: format=%s` is deliberately outside the minimum: no consumer
parses it, and its second operand is a loader constant rather than
part of the record contract.

### Accepted charset

`[A-Za-z0-9._-]`. This is what makes the bound provable: both
escapers are the identity on every accepted byte. The set also
subsumes the path checks the validator used to make one at a time
(`\`, `/`, `:`, control bytes) and additionally excludes `#` and
space, which closes the TAP-directive injection the point formatter
previously had to defend against on its own.

### A refusal is counted, never dropped

Two outcomes that used to look the same are now distinct:

- **Not a test file.** A readdir entry that is not `test_*.exe` is
  ignored silently, as before.
- **Test-shaped but refused.** A manifest entry, or a discovered
  `test_*.exe`, that fails the length, charset, traversal or
  embedded-NUL gate is a binary somebody intended to run. It is
  counted into `total_planned`, never launched, and published as a
  FAIL through the *existing* record kinds -- so no consumer needs a
  new shape:

```
refused_1_test_a_b.exe_9f2c41ab.exe: FAIL (name refused: charset)
not ok 4 - refused_1_test_a_b.exe_9f2c41ab.exe # name refused: charset
[UTEST-XML] <testcase name="refused_1_..." classname="correctness" time="0.000"><failure message="name refused: charset"/></testcase>
[UTEST-JSON] {"record_kind":"binary","name":"refused_1_...","type":"correctness","status":"FAIL","time_ms":0,"reason":"name refused: charset"}
```

The identity is `refused_<ordinal>_<sanitized prefix>_<8 hex>.exe`.
The raw name never reaches serial, an artifact or a log line. Within
a run, distinctness comes from the **ordinal** rather than from hash
strength -- no digest width can guarantee it, and an operator who
cannot tell two refusals apart cannot act on either. The FNV-1a
digest covers the entry's exact byte **span**, so it survives an
embedded NUL and correlates the same bad name across runs. The
identity ends in `.exe` because the host's recount greps for a `.exe`
name followed by a verdict token.

Three counters move together with each refusal or an artifact would
contradict itself: `counters[1]` (failed binaries), the TAP point,
and `rt.unreported` -- the JSON harvester requires
`reported + invalid + unreported == summary.total`, and a refused
binary submitted no self-report. The caller adds `total_ran`, so
`not_run = total_planned - total_ran` stays honest.

Refusals are published in a **preflight pass before any binary
launches**. They are enumeration results, not executions, and emitting
them inside the two-phase run loop made them hostage to it: refusals
are correctness-typed, so they landed in the non-smoke phase, which a
failing smoke binary breaks out of before reaching.

A refusal is counted **regardless of `filter=`**, and a refused
*manifest* entry is counted **regardless of whether the manifest is
authoritative**: the filter selects among binaries the launcher can
identify, and matching it would mean trusting the very bytes the
refusal rejected.

Refused manifest entries live in their own bounded array
(`UTEST_MANIFEST_REFUSAL_MAX`, 64) rather than competing with runnable
entries for `UTEST_MANIFEST_MAX` slots. That separation is
load-bearing: while they shared one array, the runnable cap was
checked *before* classification and stopped parsing, so a malformed
entry in the tail of an oversized manifest was never examined -- and
the documented "tail runs via glob" fallback cannot recover it,
because a name refused for an embedded NUL or an illegal byte may not
exist as a directory entry at all.

Exhausting the refusal array is itself published as a refusal, through
the same path as any other (`reason: refusal array full`), rather than
as a bare diagnostic. Both artifact formats reconcile record *count*
against the summary total -- the JSON harvester checks
`len(testcases) == summary.total` separately from the report partition
-- so a counter bumped without a record does not merely under-describe
the run, it makes the whole artifact unparseable.

A refused name discovered from *both* the manifest and the directory is
published once. That dedup compares names the way the filesystem does:
`C:` is IXFS, which folds ASCII case, so `test_Bad.exe` and
`test_bad.exe` are one file. `REFUSE_NUL` entries are excluded from it,
because their stored string is the truncation at the NUL and no
filename can contain one. The dedup for *accepted* entries stays
bytewise on purpose -- it is followed by a `filter=` match, which
compares literally, and folding one without the other can suppress a
requested binary entirely.

### Incomplete runs

The launcher plans in one walk of `C:\` and executes in a second one
taken after live children have run. A binary that disappears between
them is now reported rather than subtracted: a completed run whose
`total_ran` is short of `total_planned` emits

```
[UTEST-RUN-INCOMPLETE] planned 18 binaries but ran 17 -- 1 planned binary/binaries produced no result
```

which `scripts/test.sh` counts as a run failure. Previously this
reconciliation ran only for smoke-aborted runs, so a completed run
could publish `not_run=0` while quietly having skipped a binary.

---

## Orphaned-QEMU recovery

`scripts/test.sh` reaps its backgrounded QEMU from a cleanup trap. `SIGKILL`
runs no trap, so a killed wrapper can leave a live VM still holding the shared
boot state the tree owns -- the patched `boot.conf`, the `build/test.log`
serial sink, the `build/OVMF_VARS_4M.fd` pflash copy, and
`build/system-disk.img`. The run lock cannot see that VM: QEMU is launched with
the lock descriptor closed (`9>&-`), so it holds no lock to contend for.

Three mechanisms close the window, in the order a run meets them.

**1. Parent death reaps the VM, in the kernel.** QEMU is launched through
[`scripts/pdeathsig.py`](../../scripts/pdeathsig.py), which arms
`prctl(PR_SET_PDEATHSIG, SIGKILL)` and then `exec`s the real QEMU -- so `$!`
is still QEMU's own pid and the existing kill/wait path is unchanged. The
helper takes the expected parent pid and refuses to `exec` at all unless
`getppid()` matches it BOTH before and after the `prctl`. That is what closes
the fork-then-prctl race: re-reading `getppid()` after the call alone proves
nothing, because a parent that died before the FIRST read makes both reads
agree on the already-reparented value. `test.sh` probes the mechanism with
`--check` before launching (a set-id target or one carrying file capabilities
would have PDEATHSIG cleared across the `exec`) and WARNS on the terminal
rather than silently offering a guarantee it cannot keep.

**2. A live orphan is detected before anything shared is touched.** The check
runs immediately after the run lock is taken, which is earlier than it looks
like it needs to be: below that point the script clears the canonical aliases,
prunes older run records, and REBUILDS `system-disk.img` -- an image the orphan
holds open as a writable AHCI drive. A refusal at the `boot.conf` patch would
arrive after the disk had already been rewritten underneath a live VM. A
refusing run therefore touches no shared name at all, exactly like the
concurrent-run refusal, and owes no document to anyone.

Ownership is proven by an EXACT open-descriptor match in
[`scripts/qemu-orphan.py`](../../scripts/qemu-orphan.py), which reads `/proc`
directly (no `lsof`/`fuser` dependency) and never gates on process name --
`QEMU_BIN` is caller-supplied and need not contain "qemu". A descriptor whose
file has been unlinked reads back as `<path> (deleted)`, which is the MAIN case
rather than an edge case: `test.sh` deletes `build/test.log` before each launch,
so a previous run's orphan holds precisely a deleted serial sink.

The default on detection is REFUSAL, naming the pid and the path it holds.
`UTEST_ORPHAN_REAP=1` reaps instead, for an unattended caller where a refusal
would halt every later gate -- but only a holder whose pid, `starttime` and
`boot_id` match what `build/.test-qemu.pid` recorded when THIS TREE launched a
VM. An open descriptor proves a process is using one of our files, not that we
started it: a `tail -f build/test.log` or a disk-image inspector presents the
same evidence a leaked VM does, and reaping on that alone would SIGKILL an
operator's unrelated process unattended. A holder with no recorded provenance is
named and refused even in reap mode.

Two distinct guarantees are easy to conflate here. Provenance (above) answers
"is this OUR VM". Identity answers "is this still the SAME process": a pid is a
reused name, so `qemu-orphan.py reap` pins the target with a pidfd before
validating anything, re-checks the recorded `starttime` and `boot_id`, and
re-confirms the process still holds one of this tree's files immediately before
it signals. Termination is then detected by pidfd readability rather than by
signal 0, which a zombie still accepts. Failing any check refuses rather than
signalling on a guess.

**3. The pid is recorded so a later run can attribute one.** See `qemu_pid` /
`qemu_state` in the [commit marker](#run-identity-and-artifact-paths) above,
plus `build/test-runs/<run_id>/qemu.pid` written at launch and the shared
`build/.test-qemu.pid` the next run reads. Neither file is authority to signal;
they name the run that leaked a VM. A clean run clears the shared pidfile, and
so does a run that finds nothing holding its files -- a pidfile whose VM is
already gone is stale by construction.

If the detector itself cannot run, the run REFUSES and prints the detector's own
error. This is deliberately not the stance taken for a missing `flock`, and the
difference is what the two silences mean: a missing `flock` is a static property
of the host guarding a hazard the operator can see, while a detector that ran and
errored has produced no evidence either way about a hazard that is invisible by
construction -- an orphan from a `SIGKILL`ed run, holding the `system-disk.img`
this run is about to rewrite underneath it. Treating "no evidence" as "no orphan"
is the corruption the guard exists to prevent. `UTEST_ORPHAN_UNCHECKED=1` is the
separate escape hatch that proceeds anyway, saying loudly that the guarantee is
absent; it is deliberately not the same switch as `UTEST_ORPHAN_REAP=1`, because
"reap what you find" and "proceed having found nothing out" are different
decisions.

---

## Cross-references

- **Launcher implementation:** [src/kernel/test/test_usermode.c](../../src/kernel/test/test_usermode.c)
- **Orphan detection / reaping:** [scripts/qemu-orphan.py](../../scripts/qemu-orphan.py) -- `/proc` descriptor ownership, identity-checked reap
- **Parent-death wrapper:** [scripts/pdeathsig.py](../../scripts/pdeathsig.py) -- `PR_SET_PDEATHSIG` + expected-parent verification
- **Public API:** [include/kernel/test/test_usermode.h](../../include/kernel/test/test_usermode.h)
- **Governing TODO:** [TODO-04 -- User-Mode Test Framework](../../todo/00-infrastructure/TODO-04-usermode-test-framework.md)
- **Sibling environment matrix:** [usermode-env-matrix.md](usermode-env-matrix.md)
- **CI runner:** [scripts/test.sh](../../scripts/test.sh) -- XML post-processor at step 6, JSON at step 6b
- **JSON assembler:** [scripts/utest-json-harvest.py](../../scripts/utest-json-harvest.py) -- validation rules and refusal reasons
