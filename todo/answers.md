# Operator Answers

> Operator-reserved decisions surfaced by the overnight runner. Each Q carries a
> stable id. Answer inline with `A: (operator, YYYY-MM-DD) <decision>` and the
> runner's heal probe will pick it up on the next pass and unblock the deferred
> section. Low-risk conservative defaults may appear as `A: (proposed default)`
> for the operator to confirm or veto.

## Q1 (TODO-16 §8): WNF_STATE_NAME compat level -- ABI-defining, cannot change post-ship

The Native WNF syscall surface (`NtCreateWnfStateName` / `NtQueryWnfStateData` /
`NtUpdateWnfStateData` / `NtSubscribeWnfStateChange` / ...) must commit a
`WNF_STATE_NAME` wire encoding before the handlers ship. Two options:

- **(a) Byte-compatible:** the real Windows 64-bit encoded/XOR-obfuscated
  `WNF_STATE_NAME` scheme, so genuine Windows WNF constants (e.g.
  `WNF_SHEL_DESKTOP_APPLICATION_STARTED`) round-trip unchanged. Maximizes Win32
  binary compatibility (North Star), but pins us to Microsoft's undocumented
  obfuscation constants.
- **(b) Syscall-arg-shape only:** KNF maps opaque 64-bit names to internal OB
  paths (`\Notifications\<category>\<name>`). Simpler, fully ours, but a Windows
  binary passing a real `WNF_STATE_NAME` constant would not resolve.

This is ABI-defining and cannot change once a handler ships. Blocking TODO-16 §8.

A: (pending)

## Q2 (TODO-21 §15/§18): PROCESS_OBJECT terminal-state ownership under slot reuse

POSIX reaping (§15) needs a process to stay observable after exit until reaped,
but `PROCESS_OBJECT` (`include/kernel/ob/ob_process.h:17`) holds a raw
`struct task *` into the static `tasks[]` array. Once TODO-06 §357 adds
slot-reuse-after-drain, an old NT process handle could observe a DIFFERENT
process in the reused slot (an NT handle can outlive the POSIX reaper). Where
does terminal state (exit_status + signaled bit) live so it survives reuse?

- **(a) Task tombstone:** pin a non-reusable slot/tombstone carrying terminal
  state until every process handle closes; slot returns to the free pool only
  then. Least code change to the reaping design; costs one slot per
  handle-referenced zombie (bounded by open handles).
- **(b) Move terminal state into the refcounted `PROCESS_OBJECT`:** exit_status
  + signaled bit live on the OB object (refcounted by handles), the `tasks[]`
  slot frees immediately at reap. Cleaner NT-handle lifetime, but decouples
  process identity from the task slot (larger change to `wait_is_ready`,
  `NtWaitForSingleObject`, `NtQueryInformationProcess`).
- **(c) Generation-stable task id:** pair (slot, generation) everywhere a
  `struct task *`/pid is cached; a stale handle detects a generation mismatch.
  Composes with TODO-06 §357 monotonic PID but touches every task lookup.

Blocks TODO-21 §15 (and §18). Depends on TODO-06 §357 slot-reuse landing first.

A: (proposed default) (a) task tombstone -- least invasive, keeps §15's reaping
design intact and bounds the extra slots by open handle count; revisit if the
per-handle slot cost proves too high under the TASK_MAX ceiling.
