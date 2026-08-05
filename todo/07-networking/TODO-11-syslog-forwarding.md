---
schema_version: 1
id: syslog-forwarding
domain: 07-networking
status: active
title: "TODO-11 -- Remote Syslog Forwarding (RFC 5424)"
---

# TODO-11 -- Remote Syslog Forwarding (RFC 5424)

> **Goal:** Forward kernel log entries to a remote syslog server over UDP port 514 for enterprise monitoring and headless debug use. Enables centralized log collection from Impossible OS machines without serial access.

> [!IMPORTANT]
> **Current state:** `src/kernel/net/udp.c` exists with basic UDP send capability. `klog_disk_flush()` writes to local disk. No syslog packet formatting or remote forwarding. Registry key `HKLM\SYSTEM\Logs\SyslogServer` does not exist yet.

---

## Inputs

- `src/kernel/klog_disk.c` -- disk flush path to hook syslog sending into
- `src/kernel/net/udp.c` -- existing UDP send function
- `include/kernel/klog.h` -- `log_level_t` enum for severity mapping
- `src/kernel/registry.c` -- Registry API for reading syslog server config
- → XREF: `TODO-04-system-logging.md` -- original home was TODO-02 §7 (moved here)
- → XREF: `TODO-01-kernel-init-sequencing.md` -- `klog_disk_enable()` is Phase 2 gate

---

## Outcome

- When `HKLM\SYSTEM\Logs\SyslogServer` is set to an IP address, klog entries are forwarded as RFC 5424 syslog packets over UDP 514.
- No configuration = no syslog (zero overhead).
- Network-down is graceful -- never blocks the flush path.
- Standard syslog servers (rsyslog, syslog-ng, Graylog) receive and parse the packets.

---

## Implementation Order

| ⭐  | Order | Deliverable                              | Depends On | Status |
| --- | :---: | ---------------------------------------- | ---------- | :----: |
| 💎  |   1   | RFC 5424 packet formatter and UDP sender | --         |  [ ]   |
| 💎  |   2   | Registry-gated syslog init in klog_disk_enable | §1         |  [ ]   |
| 💎  |   3   | Network-down resilience and queue drain  | §2         |  [ ]   |

---

## 1. RFC 5424 Packet Formatter and UDP Sender

- [ ] Map klog levels to RFC 5424 severity: `DEBUG→7`, `INFO→6`, `WARN→4`, `ERROR→3`, `FATAL→2`
- [ ] Facility: `LOG_KERN (0)` -- PRI = facility × 8 + severity
- [ ] Format each packet: `<PRI>1 TIMESTAMP HOSTNAME APPNAME - - - MSG`
  - TIMESTAMP: ISO 8601 from RTC (`2026-04-02T01:55:00Z`)
  - HOSTNAME: `ImpossibleOS` (or from Registry if configured)
  - APPNAME: subsystem tag from klog entry
- [ ] Implement `syslog_send(const klog_entry_t *entry)` -- formats + sends via `udp_send()`
- [ ] Commit: `"net: RFC 5424 syslog packet formatter + UDP sender"`

## 2. Registry-Gated Syslog Init

- [ ] Read `HKLM\SYSTEM\Logs\SyslogServer` at `klog_disk_enable()` time; skip if not set
- [ ] Parse value as IPv4 address (e.g., `"192.168.1.100"`)
- [ ] Optional port override: `HKLM\SYSTEM\Logs\SyslogPort` (default 514)
- [ ] In `klog_disk_flush()`: if syslog is configured, call `syslog_send()` for each new entry
- [ ] Commit: `"net: syslog forwarding gated on Registry SyslogServer key"`

## 3. Network-Down Resilience

- [ ] Queue entries in the ring buffer if network is not yet up; drain queue once network is ready
- [ ] Graceful no-op if network goes down mid-session -- never block the flush path waiting for network
- [ ] Rate limit syslog sends to avoid flooding the network (reuse klog rate limiter pattern)
- [ ] Commit: `"net: syslog resilience -- queue on network-down, drain on recovery"`

---

## OS Comparison

| ⭐  | Feature         | 🪟 Win11        | 🐧 Linux            | 🚀 Impossible OS                |
| --- | --------------- | --------------- | ------------------- | ------------------------------- |
| 💎  | Remote syslog   | ✅ WEF          | ✅ rsyslog/journald | ⬜ §1–§2                        |
| 💎  | RFC 5424 format | ⚠️ Custom ETW    | ✅ rsyslog          | ⬜ §1                           |
| ⭐  | Registry-gated  | ❌ Group Policy | ✅ Config file      | ⬜ §2 -- zero config by default |

---

## Unit Tests

> Wire into `test_runner_init()` via `test_register_syslog()` (XREF: `00-infrastructure/TODO-03-kernel-test-harness.md`).

- [ ] Create `src/kernel/test/test_syslog.c` with:
  - Syslog packet formatter produces valid `<PRI>1 ...` format
  - PRI calculation: LOG_INFO → `<6>`, LOG_ERROR → `<3>`
  - No crash when syslog not configured (graceful no-op)
- [ ] Register in `test_runner_init()`: `test_register_syslog()`
- [ ] Commit: `"test: add syslog formatter test suite"`

---

## Verification

- [ ] `bash scripts/build.sh clean` → `=== BUILD OK ===`
- [ ] QEMU WHPX: set SyslogServer in Registry → entries appear on test syslog server (UDP 514)
- [ ] QEMU WHPX: no SyslogServer key → zero syslog traffic (tcpdump confirms)
- [ ] QEMU WHPX: network down → klog continues without hang or error
- [ ] Bare metal: syslog packets received by rsyslog on LAN
- [ ] Commit: `"net: syslog forwarding verified -- RFC 5424, Registry-gated, resilient"`
