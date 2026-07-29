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
    <property name="commit" value="SHA[-dirty]"/>
    <property name="leg" value="HOST-ACCEL-Ncpu[-ciparity][-LABEL]"/>
    <property name="leg_source" value="derived|derived+override"/>
    <property name="accel" value="kvm|tcg"/>
    <property name="cpus" value="N"/>
    <property name="qemu" value="QEMU-BINARY-BASENAME"/>
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
The stream carries two kinds of object: one per BINARY and one per
synthetic skip block. A consumer that counted every object with a `name`
would report more testcases than `summary.total` and skew every pass rate
derived from it. Key on `record_kind`, never on the presence of `name`.

`summary.total` counts BINARIES. The synthetic records are counted
separately as `summary.reported.skip_records`, so
`records == total + skip_records`.

`summary.reported` is the report dimension described at the top of this
document. `binaries_unreported` is what makes silence legible: a binary
that never called `SYS_TEST_REPORT` is a different fact from one that
reported zero skips, and only the second is evidence of full coverage.
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
| `binaries_unreported`   | integer  | binaries that never reported (legacy path)         |

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

Both artifacts are written twice: to a leg-suffixed path that accumulates
(`build/test-results-<leg>.xml` / `.json`) and to the canonical unsuffixed
path, which is an alias for the invocation that wrote it. The leg artifact is
published first and the alias second, so the alias is never newer than the
record it points at.

`scripts/test.sh` derives identity before the build and writes it to
`build/test-run-identity.json` (`utest-run-identity-v1`), which the harvester
reads via `--identity`:

| Field | Source |
|---|---|
| `timestamp` | `date -u`, ISO-8601 with `Z` |
| `commit` | `git rev-parse HEAD`, `-dirty` appended when tracked files differ |
| `leg` | `<host>-<accel>-<n>cpu`, plus `-ciparity` and any validated `UTEST_LEG` label |
| `leg_source` | `derived`, or `derived+override` when `UTEST_LEG` added a label |
| `accel` | read back from the arguments QEMU receives, never from display text |
| `cpus` | `SMP_CPUS`, the same variable behind the `-smp` flag |

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

One invocation at a time per checkout. `scripts/test.sh` patches the shared
`boot.conf`, writes a fixed `build/test.log`, and copies OVMF vars to a fixed
path, so two concurrent runs in the same working tree already corrupt each
other's boot configuration and serial capture regardless of artifacts; the
leg-suffixed paths separate CONFIGURATIONS run one after another, not
simultaneous runs. Giving each invocation its own private record is tracked
separately.

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

Today's inputs (names validated by `u_is_valid_manifest_name`,
reasons launcher-formatted) never trigger escaping on the happy
path; the escape is defense-in-depth for future consumers that might
let a binary set its own reason string.

---

## Cross-references

- **Launcher implementation:** [src/kernel/test/test_usermode.c](../../src/kernel/test/test_usermode.c)
- **Public API:** [include/kernel/test/test_usermode.h](../../include/kernel/test/test_usermode.h)
- **Governing TODO:** [TODO-04 -- User-Mode Test Framework](../../todo/00-infrastructure/TODO-04-usermode-test-framework.md)
- **Sibling environment matrix:** [usermode-env-matrix.md](usermode-env-matrix.md)
- **CI runner:** [scripts/test.sh](../../scripts/test.sh) -- XML post-processor at step 6, JSON at step 6b
- **JSON assembler:** [scripts/utest-json-harvest.py](../../scripts/utest-json-harvest.py) -- validation rules and refusal reasons
