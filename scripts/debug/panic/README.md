# Panic / BSOD Test Runners

Test runners for triggering and verifying kernel panics and crash recovery.

## Test Scenarios

| Script | Exception | What it tests |
|--------|-----------|---------------|
| `run-bsod-test.bat` | Deliberate panic | BSOD rendering, crash dump, crash log persist |
| `run-null-deref-test.bat` | #PF (Page Fault) | NULL pointer dereference handling |
| `run-stack-overflow-test.bat` | #DF (Double Fault) | IST-based DF handler, alternate stack BSOD |
| `run-divide-by-zero-test.bat` | #DE (Divide Error) | Arithmetic exception routing |
| `run-crash-recovery-test.bat` | Normal boot | Verify [CRASH-PREV] entries after a crash |

## How to Use

### 1. Build with test flag

Each test requires a compile-time flag. Uncomment or add to Makefile CFLAGS:

```
-DBSOD_TEST           # Deliberate panic in boot_desktop.c
-DNULL_DEREF_TEST     # NULL pointer read (not yet implemented)
-DDIV_ZERO_TEST       # Division by zero (not yet implemented)
-DSTACK_OVERFLOW_TEST # Infinite recursion (not yet implemented)
```

Currently only `BSOD_TEST` is implemented in the kernel. The other flags are placeholders for future crash test instrumentation.

### 2. Run the crash test

Double-click the corresponding `.bat` file. The OS will boot and crash.

### 3. Verify crash recovery

After the crash, run `run-crash-recovery-test.bat` or any normal boot. Check serial output for `[CRASH-PREV]` entries. On QEMU (cold restart), recovery depends on physical memory preservation -- works best on VBox or bare metal warm reboot.

## Crash Recovery Architecture

```
Crash Boot:  panic_screen() -> klog_crash_persist() -> 128 KiB PMM region + NVRAM address
Normal Boot: klog_crash_recover() -> read NVRAM -> check magic + CRC32 -> [CRASH-PREV] on serial
Phase 2:     klog_crash_write_to_disk() -> crash_recovery.log on C:\
```
