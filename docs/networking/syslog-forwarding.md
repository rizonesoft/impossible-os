<!-- docs: covers=todo/07-networking/TODO-11-syslog-forwarding.md sources=include/kernel/klog.h,src/kernel/klog.c,src/kernel/klog_disk.c,src/kernel/net/udp.c reviewed=2026-09-29 order=11 -->
# Remote Syslog Forwarding

## What is it?

Remote syslog forwarding sends the kernel log to a log server on the network, so a fleet of machines, or one headless machine without a serial cable, can be monitored from one place. This roadmap formats each kernel log entry as an RFC 5424 syslog message and sends it over UDP port 514 when the Registry names a server. With no server configured it does nothing and costs nothing. Impossible OS has no syslog code today; the log goes to serial, the screen, an in-memory ring and disk. All three sections are unstarted.

## How does it work?

**Today.** The kernel log in [`klog.h`](../../include/kernel/klog.h) has five levels, `LOG_DEBUG`, `LOG_INFO`, `LOG_WARN`, `LOG_ERROR` and `LOG_FATAL`. Each entry (`klog_entry_t`) records its level, a subsystem tag such as `net`, a timestamp in 10 ms units since boot, the CPU, process and thread, and a message of up to 256 bytes. Entries land in a 1000-entry ring and are written to disk by [`klog_disk.c`](../../src/kernel/klog_disk.c). Settings already live in the Registry under `HKLM\SYSTEM\Logs` (per-subsystem `Levels` and `RateLimit`, and `MaxSize`, `MaxRotated` and `Compress` for disk files), read by [`klog.c`](../../src/kernel/klog.c). The full logging design is in [System Logging](../kernel/system-logging.md).

The only network path a forwarder could use is `udp_send()` in [`udp.c`](../../src/kernel/net/udp.c). It returns nothing, silently discards a message larger than one packet, and drops the packet when the next hop's address is not yet known, so a forwarder built on it cannot tell whether a message left the machine.

**Planned design.**

1. **Formatting.** Each level maps to a syslog severity (debug 7, info 6, warning 4, error 3, fatal 2) with the kernel facility, and each message becomes `<PRI>1 TIMESTAMP HOSTNAME APPNAME - - - MSG`, with the subsystem tag as the app name. A kernel entry's own timestamp counts from boot, so the date and time are reconstructed as the disk log already does it: the current wall clock minus the entry's age. Entries sent late, after the network comes up, therefore keep the time they were logged.
2. **Configuration.** `HKLM\SYSTEM\Logs\SyslogServer` holds the server's IPv4 address and `SyslogPort` an optional port (default 514). Both are read when disk logging starts, and each new entry is sent from the disk flush path.
3. **Resilience.** Entries logged before the network is up wait in the ring and are sent once it is; a network outage never blocks the flush; and sending is rate-limited like the rest of the log.

```mermaid
flowchart LR
    K[klog entry] --> R[ring buffer]
    R --> D[disk flush]
    D -->|SyslogServer set| F[RFC 5424 formatter]
    F --> U[udp_send, port 514]
    U --> S[remote log server]
```

## What are its interfaces?

| Interface | Status |
| --- | --- |
| `klog()`, `klog_entry_t`, the five levels | Shipped ([`klog.h`](../../include/kernel/klog.h)) |
| `HKLM\SYSTEM\Logs\Levels`, `RateLimit`, `MaxSize`, `MaxRotated`, `Compress` | Shipped Registry settings |
| `syslog_send(const klog_entry_t *)` | Planned |
| `HKLM\SYSTEM\Logs\SyslogServer`, `SyslogPort` | Planned Registry settings |

## How do I use it?

It cannot be used yet. When it ships, the setup will be: set `SyslogServer` to the collector's address, run a standard receiver such as rsyslog, syslog-ng or Graylog on UDP 514, and restart.

## What is not implemented yet?

- [RFC 5424 Packet Formatter and UDP Sender](../../todo/07-networking/TODO-11-syslog-forwarding.md#1-rfc-5424-packet-formatter-and-udp-sender)
- [Registry-Gated Syslog Init](../../todo/07-networking/TODO-11-syslog-forwarding.md#2-registry-gated-syslog-init)
- [Network-Down Resilience](../../todo/07-networking/TODO-11-syslog-forwarding.md#3-network-down-resilience)
- **Not planned**: TCP or TLS transport (RFC 6587 and RFC 5425), host names for the server (it must be an IP address), and forwarding user-mode event logs.

## How does it compare with Windows 11 and Linux?

Windows 11 has no built-in syslog sender; it forwards its own event format with Windows Event Forwarding, and syslog needs a third-party agent. Linux sends syslog natively through rsyslog or syslog-ng, configured in a file; the systemd journal's own remote transport uses a different format. Impossible OS plans a small in-kernel sender switched on by one Registry value, so kernel messages reach a collector without any service running.

## See also

- [Syslog forwarding roadmap](../../todo/07-networking/TODO-11-syslog-forwarding.md)
- [System Logging](../kernel/system-logging.md)
- [Registry](../kernel/registry.md)
- [TCP and Network Infrastructure](tcp-network-infrastructure.md)
- [Networking](index.md)
