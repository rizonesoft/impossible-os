---
schema_version: 1
id: firewall
domain: 07-networking
status: active
title: "TODO-05 -- Network Firewall & Packet Filter"
---

# TODO-05 -- Network Firewall & Packet Filter

> **Goal:** Build a stateful packet filter engine (`struct fw_rule`, 64-rule ordered table, first-match-wins), wire it into `ipv4_handle()`/`ipv4_send()` and the IPv6 receive/send paths, integrate with the connection tracking table (TODO-01 §7) for automatic inbound allow of established sessions, ship a default allow-outbound/block-inbound ruleset loaded from the Registry at boot, add per-rule atomic hit counters exposed via `/sys/firewall`, a firewall CLI (`fw list/add/remove/flush/enable/disable`), Registry persistence, and a `firewall.cpl` Control Panel applet. The firewall is the security boundary that separates the OS from untrusted network traffic.

> [!IMPORTANT]
> Connection tracking (TODO-01 §7) must be complete before stateful integration (§3) can be built -- the CT table's 256-slot 4-tuple hash is the core data structure consulted by `fw_check()` for inbound auto-allow. The firewall hook in §2 modifies `ipv4_handle()` and `ipv4_send()` in `src/kernel/net/ip.c` -- both functions have a single call site for inbound/outbound; the hook is a single `if (fw_check(...) == FW_BLOCK) return;` guard. IPv6 filtering (§5) reuses the same engine but queries `ipv6_receive()`/`ipv6_send()` from TODO-04. Registry uses Win32 API (`RegSetValueEx`, `RegGetValue`, `HKLM`) from `include/registry.h`.

## Inputs

- `src/kernel/net/ip.c` -- `ipv4_handle()` (inbound hook point) and `ipv4_send()` (outbound hook point); §2 adds `fw_check()` calls at the start of each
- `src/kernel/net/ip6.c` + `src/kernel/net/icmp6.c` -- `ipv6_receive()`/`ipv6_send()` hook points for IPv6 filter (§5); same `fw_check()` call pattern
- `src/kernel/net/conntrack.c` + `include/kernel/net/net.h` -- connection tracking 4-tuple hash table from TODO-01 §7; `ct_lookup(src_ip, src_port, dst_ip, dst_port, proto)` used in §3 inbound auto-allow
- `include/registry.h` -- `RegSetValueEx`, `RegGetValue`, `RegCreateKeyEx`, `RegDeleteKey`, `HKLM` for §7 persistence
- `src/kernel/fs/sysfs.c` (or VFS `/sys/` mount point) -- expose `/sys/firewall` read-only file for §9 hit counter dump
- `src/shell/` -- `cmd_fw.c` (new) for §6 CLI; register in shell command table
- `src/desktop/controls.c` + desktop compositing -- `firewall.cpl` applet UI in §8
- → XREF: `07-networking/TODO-01-tcp-network-infrastructure.md` -- §7 stateful connection tracking table (`ct_lookup()`) is mandatory for §3 inbound established auto-allow
- → XREF: `07-networking/TODO-04-ipv6-dual-stack.md` -- `ipv6_receive()`/`ipv6_send()` hook points needed by §5 IPv6 filter; ICMPv6 ALLOW rule in default ruleset (§4)

## Outcome

- `fw_check(hdr, src_port, dst_port, direction)` returns ALLOW or BLOCK in O(64) linear scan.
- Established inbound sessions auto-allowed via CT lookup before rule scan.
- Firewall hooks active in both IPv4 and IPv6 receive/send paths; blocked packets dropped silently (stealth mode).
- Default ruleset: allow all outbound, block all inbound except CT-established + ICMP/DHCPv4/DHCPv6/DNS/NTP responses.
- Per-rule atomic hit counters; `/sys/firewall` VFS file readable at runtime.
- `fw list/add/remove/flush/enable/disable/status` shell commands.
- Rules persisted to `HKLM\SYSTEM\Network\Firewall\Rules` and loaded at boot.
- `firewall.cpl` applet: enable/disable toggle, rule list CRUD, 100-entry blocked-packet log viewer.

## Implementation Order

| ⭐  | Order | Deliverable                                                                                      | Depends On                                                                 | Status |
| --- | :---: | ------------------------------------------------------------------------------------------------ | -------------------------------------------------------------------------- | :----: |
| 💎  |   1   | §1 Packet filter engine -- `fw_rule`, 64-rule table, `fw_check()`, add/remove/set-default API   | Nothing -- standalone engine                                                |  [ ]   |
| 💎  |   2   | §2 IP layer hook -- `fw_check()` in `ipv4_handle` + `ipv4_send`; stealth drop; debug log        | §1 (engine must exist before hooks call it)                                |  [ ]   |
| 💎  |   3   | §3 Stateful CT integration -- CT lookup before rule scan; inbound established auto-allow         | §1 engine + §2 hook wired; TODO-01 §7 CT table                             |  [ ]   |
| 💎  |   4   | §4 Default ruleset -- allow-out/block-in policy; ICMP/DHCP/DHCPv6/DNS/NTP allow rules           | §1–§3 (engine + CT integration must work before default rules are loaded)  |  [ ]   |
| 💎  |   9   | §9 Per-rule hit counters -- atomic `u64 hits`, `/sys/firewall` VFS file                         | §1 (`fw_rule` struct must be frozen before adding counter field)           |  [ ]   |
| 💎  |   5   | §5 IPv6 firewall -- dual-prefix `fw_rule`, hook in `ipv6_receive()`/`ipv6_send()`               | §1–§4 (IPv4 path must be proven before extending to IPv6); TODO-04         |  [ ]   |
| 💎  |   6   | §6 Firewall CLI -- `fw list/add/remove/flush/enable/disable/status`                              | §1 (rule API); §9 hit counters (for `fw list` counter column)              |  [ ]   |
| 💎  |   7   | §7 Registry persistence -- rules to `HKLM\SYSTEM\Network\Firewall\Rules`; load at boot          | §6 CLI (rule struct finalized); Registry API from `include/registry.h`     |  [ ]   |
| 💎  |   8   | §8 `firewall.cpl` Control Panel applet -- toggle, rule CRUD, blocked-packet log viewer          | §7 Registry (applet reads/writes same keys); §6 CLI logic reused           |  [ ]   |

---

## 1. Packet Filter Engine `[Opus]`

Define `struct fw_rule`. Implement the 64-rule ordered table with first-match-wins semantics. `fw_check()` linear scan returns ALLOW or BLOCK. `fw_add_rule()`, `fw_remove_rule(index)`, `fw_set_default_policy(action)`.

**Files:** `src/kernel/net/firewall.c` (new), `include/kernel/net/firewall.h` (new)

> [!NOTE]
> This is `[Opus]` -- the firewall engine is a novel security-critical component with no prior implementation in Impossible OS. The rule match logic must be correct for all edge cases: zero-prefix (`0.0.0.0/0` or `::/0`) matches any address; `port_min == port_max == 0` means any port; protocol `FW_PROTO_ANY=0` matches all protocols; direction `FW_DIR_BOTH` matches inbound and outbound. `fw_check()` is called on every received and sent packet -- it must be O(64) worst case and branch-predictor-friendly (no malloc, no locks held across I/O). Guard the rule table with a spinlock held only during reads (rule scan) and writes (add/remove); the spinlock must not be held during logging. IPv4 address matching: `(pkt_ip & mask) == (rule_ip & mask)` where `mask = ~0u << (32 - prefix_len)`. IPv6 matching: byte-level prefix compare up to `prefix_len / 8` full bytes + partial byte mask for non-octet-aligned prefixes.

- [ ] `fw_action_t { FW_ALLOW=0, FW_BLOCK=1 }` and `fw_proto_t { FW_PROTO_ANY=0, FW_PROTO_TCP=6, FW_PROTO_UDP=17, FW_PROTO_ICMP=1, FW_PROTO_ICMPV6=58 }` in `firewall.h`
- [ ] `fw_dir_t { FW_DIR_IN=0, FW_DIR_OUT=1, FW_DIR_BOTH=2 }` in `firewall.h`
- [ ] `fw_rule_t { uint32_t src_ip4; uint8_t src_prefix4; uint32_t dst_ip4; uint8_t dst_prefix4; uint8_t src_ip6[16]; uint8_t src_prefix6; uint8_t dst_ip6[16]; uint8_t dst_prefix6; uint16_t src_port_min, src_port_max; uint16_t dst_port_min, dst_port_max; fw_proto_t proto; fw_dir_t dir; fw_action_t action; uint8_t log; uint8_t enabled; uint64_t hits; }` (see §5 for hits)
- [ ] `fw_table[64]` + `fw_rule_count` + `fw_default_policy` (default `FW_ALLOW` for outbound, `FW_BLOCK` for inbound at init) + `fw_enabled` flag + `fw_lock` (spinlock)
- [ ] `fw_ip4_match(pkt_ip, rule_ip, prefix)` → bool: mask = `(prefix==0) ? 0 : (~0u << (32-prefix))`; return `(pkt_ip & mask) == (rule_ip & mask)`
- [ ] `fw_ip6_match(pkt_ip6, rule_ip6, prefix)` → bool: full-byte compare + partial byte mask
- [ ] `fw_port_match(pkt_port, min, max)` → bool: `min==0 && max==0` → true; else `pkt_port >= min && pkt_port <= max`
- [ ] `fw_check(src_ip4, dst_ip4, src_ip6, dst_ip6, src_port, dst_port, proto, dir)` → `fw_action_t`: if `!fw_enabled` return FW_ALLOW; lock; scan `fw_table[0..fw_rule_count-1]`; per rule: check `enabled`, `dir`, `proto`, src/dst IP match, src/dst port match; on first match: unlock; return rule action (+ increment hits in §5); unlock; return `fw_default_policy`
- [ ] `fw_add_rule(rule)` → index or -ENOSPC: lock; find first slot with `!enabled`; copy rule; increment count; unlock
- [ ] `fw_remove_rule(index)` → 0 or -EINVAL: lock; validate index < 64 + enabled; clear `enabled`; decrement count; unlock
- [ ] `fw_set_default_policy(dir, action)`: set `fw_default_in_policy` or `fw_default_out_policy`
- [ ] `fw_init()`: zero table; set defaults (see §4); called from network init
- [ ] Commit: `"net/fw: packet filter engine -- fw_rule, 64-rule table, fw_check(), add/remove/set-default"`

## 2. IP Layer Hooks `[Sonnet]`

Add `fw_check()` calls to `ipv4_handle()` (inbound) and `ipv4_send()` (outbound) in `ip.c`. Blocked packets are dropped silently (stealth mode). Blocked packets logged to serial in debug builds.

**Files:** `src/kernel/net/ip.c` (extend)

> [!NOTE]
> Hook placement in `ipv4_handle()`: after the IPv4 header validation and checksum verify, before the TCP/UDP/ICMP dispatch switch -- extract `src_ip`, `dst_ip`, `protocol`, `src_port`, `dst_port` (from the transport header at offset `ihl*4`); call `fw_check(src_ip, dst_ip, NULL, NULL, src_port, dst_port, protocol, FW_DIR_IN)`; if BLOCK: `#ifdef DEBUG serial_log("[FW] DROP inbound src=%u.%u.%u.%u:%u dst=%u.%u.%u.%u:%u"); #endif return;`. Hook placement in `ipv4_send()`: after the destination IP is resolved but before `eth_send()` -- call `fw_check(local_ip, dst_ip, NULL, NULL, src_port, dst_port, protocol, FW_DIR_OUT)`; if BLOCK: log + return -EPERM. Transport port extraction: TCP/UDP both have src_port at offset 0, dst_port at offset 2 within the transport header (big-endian); ICMP has no ports -- pass `src_port=0`, `dst_port=0`.

- [ ] Extract transport-layer port helper `ip4_get_ports(ipv4_hdr, &src_port, &dst_port)`: compute transport header offset = `(hdr->ver_ihl & 0xF) * 4`; for TCP/UDP: read `uint16_t` at offset 0 and 2 (`ntohs`); for ICMP and others: set both to 0
- [ ] Inbound hook in `ipv4_handle()`: after header validation, before dispatch: call `fw_check(src, dst, NULL, NULL, sp, dp, proto, FW_DIR_IN)`; on BLOCK: debug-log + return
- [ ] Outbound hook in `ipv4_send()`: before `eth_send()`: call `fw_check(local_ip, dst, NULL, NULL, sp, dp, proto, FW_DIR_OUT)`; on BLOCK: debug-log + return -EPERM
- [ ] `FW_DEBUG_LOG` macro: expands to `serial_printf(...)` if `DEBUG` defined; expands to nothing in release builds
- [ ] Log (debug only): `[FW] DROP IN src=%u.%u.%u.%u:%u dst=%u.%u.%u.%u:%u proto=%u rule=%d` (rule index or -1 for default policy)
- [ ] Commit: `"net/fw: ipv4_handle + ipv4_send hooks -- fw_check inbound/outbound, stealth drop, debug log"`

## 3. Stateful Connection Tracking Integration `[Opus]`

Before the rule table scan, `fw_check()` for inbound packets consults the CT table from TODO-01 §7. If the packet matches an established or related outbound session, auto-allow without scanning rules.

**Files:** `src/kernel/net/firewall.c` (extend)

> [!NOTE]
> This is `[Opus]` -- the CT auto-allow is security-critical: incorrect CT matching could allow spoofed packets to bypass the rule table. The CT lookup from TODO-01 §7 is `ct_lookup(src_ip, src_port, dst_ip, dst_port, proto)` returning a `ct_entry_t*` or NULL. For inbound auto-allow, the 4-tuple is **reversed** relative to the outbound connection: an inbound TCP packet with `src=remote, dst=local` matches a CT entry whose `src_ip=local, dst_ip=remote`. Auto-allow applies only to `CT_ESTABLISHED` and `CT_UDP` entries; `CT_SYN_SENT` entries (incomplete handshakes) do **not** auto-allow -- they must pass the rule table. This prevents SYN-flood bypass. The CT check must happen under the firewall lock (or use the CT table's own spinlock) to avoid TOCTOU between the CT check and packet delivery.

- [ ] Add CT auto-allow path to `fw_check()`: if `dir == FW_DIR_IN` and `proto == TCP or UDP`: call `ct_lookup(dst_ip, dst_port, src_ip, src_port, proto)` (reversed 4-tuple for inbound); if found and `entry->state == CT_ESTABLISHED || entry->state == CT_UDP`: increment `fw_ct_bypass_count`; return FW_ALLOW immediately (before rule scan)
- [ ] `fw_ct_bypass_count` global counter (for `fw status` display); atomic increment
- [ ] TCP SYN-flood guard: if CT entry state is `CT_SYN_SENT` (incomplete handshake): do NOT auto-allow; fall through to rule scan
- [ ] ICMP echo-reply auto-allow: if `proto == ICMP` and ICMP type == 0 (echo reply): check CT for a matching `CT_UDP`-style entry keyed on ICMP identifier + source IP; if found: auto-allow
- [ ] ICMPv6 NA/RA auto-allow: always allow inbound ICMPv6 types 134 (RA), 136 (NA), 137 (Redirect) -- required for NDP to function even when default inbound policy is BLOCK
- [ ] Log (debug): `[FW] CT bypass: established %u.%u.%u.%u:%u → local:%u` and `[FW] CT miss: new inbound → rule scan`
- [ ] Commit: `"net/fw: CT stateful integration -- established inbound auto-allow, SYN guard, ICMPv6 NDP bypass"`

## 4. Default Ruleset `[Sonnet]`

Load the default allow-outbound / block-inbound ruleset on boot. Allow established via CT (§3). Explicit ALLOW rules for inbound ICMP, ICMPv6, DHCP (UDP 67/68), DHCPv6 (UDP 546), DNS responses (UDP 53), and NTP responses (UDP 123). Load from Registry if available; else install defaults.

**Files:** `src/kernel/net/firewall.c` (extend)

> [!NOTE]
> Default policy: `fw_default_out_policy = FW_ALLOW`; `fw_default_in_policy = FW_BLOCK`. The explicit ALLOW rules below override the default BLOCK for essential protocol responses that are not tracked by the CT table (ICMP, DHCP, NTP are typically stateless from the CT perspective). Rule priority order: CT auto-allow first (§3), then explicit ALLOW rules for protocols below, then default BLOCK. DHCP inbound: port 68 (client port, src_port_min=67 dst_port 68 from server); allow UDP src=any dst=255.255.255.255 port 68 (broadcast). ICMPv6 NDP (types 133–137) must never be blocked; the CT bypass in §3 handles this -- no explicit rule needed, but add one as belt-and-suspenders. NTP: allow inbound UDP src_port=123 (server response to our query on ephemeral port); DNS: allow inbound UDP src_port=53.

- [ ] `fw_install_defaults()`: called from `fw_init()` when Registry key absent or `fw_load_registry()` fails; installs 8 explicit ALLOW rules in order:
  1. Any → ICMP inbound ALLOW (proto=ICMP, dir=IN, src/dst any, ports 0/0)
  2. Any → ICMPv6 inbound ALLOW (proto=ICMPV6, dir=IN)
  3. UDP port 68 inbound ALLOW (DHCP client response from server:67)
  4. UDP port 546 inbound ALLOW (DHCPv6 client response)
  5. UDP src_port=53 inbound ALLOW (DNS response from server)
  6. UDP src_port=123 inbound ALLOW (NTP response from server)
  7. All outbound ALLOW (dir=OUT, proto=ANY, src/dst any) -- belt-and-suspenders; default policy handles this
  8. (slot 8 reserved for user rules loaded from Registry)
- [ ] `fw_load_registry()` → rule count loaded or -ENOENT: open `HKLM\SYSTEM\Network\Firewall\Rules`; enumerate subkeys `Rule0`…`RuleN`; read each DWORD/string value; call `fw_add_rule()`; set `fw_enabled` from `HKLM\SYSTEM\Network\Firewall\Enabled`
- [ ] `fw_init()`: call `fw_load_registry()`; if -ENOENT: call `fw_install_defaults()`; log loaded rule count
- [ ] Log: `[FW] Loaded N rules from Registry` or `[FW] No Registry config -- installing defaults (N rules)`
- [ ] Commit: `"net/fw: default ruleset -- allow-out/block-in, ICMP/DHCP/DNS/NTP allow, Registry load at boot"`

## 5. IPv6 Firewall `[Sonnet]`

Extend the same engine to filter IPv6 packets. Add `fw_check()` hooks in `ipv6_receive()` and `ipv6_send()`. `fw_rule` already carries IPv6 prefix fields; add IPv6-specific default rules.

**Files:** `src/kernel/net/ip6.c` (extend), `src/kernel/net/firewall.c` (extend)

> [!NOTE]
> IPv6 rule matching: pass `src_ip6`/`dst_ip6` (16-byte arrays) to `fw_check()`; the engine already has `fw_ip6_match()` (§1). IPv4 rules with `src_prefix4 == 0` and `dst_prefix4 == 0` match any IPv4 packet; they should **not** match IPv6 packets -- disambiguate by checking whether the `src_ip6` argument is non-NULL in `fw_check()`. A rule with all IPv4 fields zero and all IPv6 fields zero matches both (the universal `any` case). IPv6-specific default rules to add in `fw_install_defaults()`: allow inbound ICMPv6 all types (already covered by rule 2 in §4); no additional rules needed for SLAAC/NDP because ICMPv6 proto rule covers them. `fw_check()` signature extension: add `const uint8_t *src_ip6, const uint8_t *dst_ip6` parameters (NULL for IPv4-only calls); update all existing callers from §2.

- [ ] Update `fw_check()` signature: `fw_check(uint32_t src4, uint32_t dst4, const uint8_t *src6, const uint8_t *dst6, uint16_t sp, uint16_t dp, fw_proto_t proto, fw_dir_t dir)` -- update §2 callers to pass NULL for src6/dst6
- [ ] IPv6 rule filter logic in `fw_check()`: if `src6 != NULL`: run `fw_ip6_match()` for src and dst; for rules where IPv6 prefix fields are all zero and IPv4 fields are also zero: treat as "any-protocol any-address" match
- [ ] Hook in `ipv6_receive()` (from TODO-04 §4): after version check, before dispatch: extract `src6`, `dst6`, `next_header`, `src_port`, `dst_port`; call `fw_check(..., FW_DIR_IN)`; on BLOCK: drop
- [ ] Hook in `ipv6_send()` (from TODO-04 §4): before `eth_send()`: call `fw_check(..., FW_DIR_OUT)`; on BLOCK: return -EPERM
- [ ] IPv6 CT auto-allow in §3: extend `ct_lookup()` to accept IPv6 4-tuple; ICMPv6 NDP always bypass (as per §3)
- [ ] Add IPv6 default rules to `fw_install_defaults()` if not already covered by proto=ICMPV6 rule
- [ ] Commit: `"net/fw: IPv6 filter -- fw_check IPv6 prefix match, ipv6_receive/send hooks, dual-stack rules"`

## 6. Firewall CLI `[Sonnet]`

`fw list` (rules with index + hit counter), `fw add <in|out> <proto> <port|any> <allow|block> [log]`, `fw remove <index>`, `fw flush`, `fw enable`/`fw disable`, `fw status`.

**Files:** `src/shell/cmd_fw.c` (new)

> [!NOTE]
> `fw list` output format: `IDX DIR  PROTO   SRC            DST            DPORT   ACT   HITS`. `fw add` syntax: `fw add in tcp 443 allow` → rule: dir=IN, proto=TCP, dst_port_min=dst_port_max=443, src/dst IP any, action=ALLOW. Extended syntax: `fw add in tcp 1024-65535 block log` (port range + log flag). `fw add out any any block` → block all outbound. Protocol keyword mapping: `tcp=6`, `udp=17`, `icmp=1`, `icmpv6=58`, `any=0`. IP/prefix not yet exposed in the CLI (address-based rules added via API or Registry directly). `fw status` output: `Firewall: enabled | Inbound default: BLOCK | Outbound default: ALLOW | Rules: N/64 | CT bypass: M packets | Blocked (inbound): P packets | Blocked (outbound): Q packets`.

- [ ] `cmd_fw(argc, argv)`: parse subcommands: `list`, `add`, `remove`, `flush`, `enable`, `disable`, `status`
- [ ] `fw_print_rule(index, rule)`: format one table row (80-char wide); right-align HITS with 7 digits
- [ ] `fw_parse_proto(str)` → `fw_proto_t`: map `"tcp"/"udp"/"icmp"/"icmpv6"/"any"` → numeric
- [ ] `fw_parse_port_range(str, &min, &max)` → 0 or -EINVAL: parse `"N"` (min=max=N) or `"N-M"` (min=N, max=M); `"any"` → (0,0)
- [ ] `fw flush`: call `fw_remove_rule()` for all active slots; reset counters; re-run `fw_install_defaults()`
- [ ] `fw enable`/`fw disable`: set `fw_enabled`; log `[FW] Firewall enabled/disabled`
- [ ] `fw status`: print policy + counts from `fw_enabled`, `fw_default_*_policy`, `fw_rule_count`, `fw_ct_bypass_count`, `fw_blocked_in_count`, `fw_blocked_out_count`
- [ ] Register `fw` in shell command table
- [ ] Commit: `"shell/fw: firewall CLI -- list/add/remove/flush/enable/disable/status, port range parser"`

## 7. Registry Persistence `[Sonnet]`

Serialize rules to `HKLM\SYSTEM\Network\Firewall\Rules\Rule0`…`RuleN`. Each subkey stores DWORD/string values for all `fw_rule_t` fields. `fw_save_registry()` called after any rule add/remove. `fw_load_registry()` at boot.

**Files:** `src/kernel/net/firewall.c` (extend)

> [!NOTE]
> Registry path: `HKLM\SYSTEM\Network\Firewall\Rules` -- one subkey per rule named `"Rule%d"` (index). Per-rule values: `Action` (DWORD 0=ALLOW/1=BLOCK), `Direction` (DWORD), `Protocol` (DWORD), `SrcPort` (DWORD, encodes min in low 16 bits + max in high 16 bits), `DstPort` (DWORD, same), `Log` (DWORD), `Enabled` (DWORD). IP address fields (for future GUI-added rules): `SrcIP4` (DWORD), `SrcPrefix4` (DWORD), `DstIP4` (DWORD), `DstPrefix4` (DWORD). `HKLM\SYSTEM\Network\Firewall\Enabled` (DWORD): 0=disabled, 1=enabled. `HKLM\SYSTEM\Network\Firewall\InboundDefault` and `OutboundDefault` (DWORD). `fw_save_registry()` uses `RegCreateKeyEx` + `RegSetValueEx`; `fw_load_registry()` uses `RegGetValue`.

- [ ] `fw_save_registry()`: open/create `HKLM\SYSTEM\Network\Firewall`; `RegSetValueEx(key, "Enabled", ...)` + `RegSetValueEx(key, "InboundDefault", ...)`; for each active rule: `RegCreateKeyEx(key, "Rules\\Rule%d", ...)` + set all DWORD values
- [ ] `fw_load_registry()` → n or -ENOENT: open `HKLM\SYSTEM\Network\Firewall`; read `Enabled`, `InboundDefault`, `OutboundDefault`; iterate `Rules\Rule0`…until not found; for each: read all values; call `fw_add_rule()`
- [ ] `fw_delete_registry_rule(index)`: `RegDeleteKey(key, "Rules\\Rule%d")` when rule is removed
- [ ] Hook `fw_add_rule()` and `fw_remove_rule()` to call `fw_save_registry()` after successful modification
- [ ] `fw_export_rules(path)` optional: write human-readable text dump of all rules to a VFS file path (for backup/restore)
- [ ] Commit: `"net/fw: Registry persistence -- HKLM\\SYSTEM\\Network\\Firewall, save/load rules + policy, auto-save on change"`

## 8. `firewall.cpl` Control Panel Applet `[Sonnet]`

**Design:** [`shell.md#window-chrome`](../../docs/design/shell.md#window-chrome), [`controls.md#tabs`](../../docs/design/controls.md#tabs), [`controls.md#list-tree-and-grid-views`](../../docs/design/controls.md#list-tree-and-grid-views), [`controls.md#dialog`](../../docs/design/controls.md#dialog)

Enable/disable toggle. Rule list with add/remove buttons (inbound/outbound tabs). Log viewer showing last 100 blocked packets.

**Files:** `src/desktop/firewall_cpl.c` (new), `include/desktop/firewall_cpl.h` (new)

> [!NOTE]
> The applet is a desktop window registered with the compositor. UI layout: top section has enable/disable toggle button + status label (`"Firewall: ON -- blocking inbound"`). Tab bar with `Inbound Rules` and `Outbound Rules` tabs. Each tab: scrollable rule list (index, direction, protocol, port, action, hits); `Add Rule` button opens a modal dialog with protocol dropdown, port field, direction toggle, action toggle; `Remove Rule` button removes the selected rule. `Blocked Packets Log` third tab: circular 100-entry buffer of `{ timestamp_ms, src_ip, dst_ip, src_port, dst_port, proto, direction }`; auto-refreshes every 2 s. All rule changes call `fw_add_rule()`/`fw_remove_rule()` which in turn calls `fw_save_registry()`. The applet opens on `fw_open_cpl()` call; register in the Control Panel launcher.

- [ ] `fw_log_entry_t { uint64_t ts_ms; uint32_t src4, dst4; uint8_t src6[16], dst6[16]; uint16_t src_port, dst_port; uint8_t proto; uint8_t dir; }` -- circular 100-entry ring `fw_block_log[100]` + `fw_log_head`; populated in `fw_check()` when action==BLOCK and `rule->log` flag set (or default policy block)
- [ ] `fw_open_cpl()`: create desktop window `"Windows Firewall"`; 640×480; draw enable toggle + two rule tabs + log tab
- [ ] Enable toggle: reads `fw_enabled`; on click: calls `fw_enable()`/`fw_disable()` + redraws label
- [ ] Rule list widget: iterate `fw_table[0..63]`; draw rows for enabled rules; selected row highlighted; `Add`/`Remove` buttons call `fw_add_rule()`/`fw_remove_rule()` + redraw
- [ ] Add Rule modal dialog: protocol dropdown (TCP/UDP/ICMP/Any), port text field, direction radio, action radio, OK/Cancel
- [ ] Log tab: read `fw_block_log[]`; render table of last 100 blocked entries; `Clear` button zeroes ring
- [ ] Register `firewall.cpl` in the Control Panel launcher (same mechanism as other `.cpl` applets)
- [ ] Commit: `"desktop: firewall.cpl -- enable toggle, rule list CRUD, blocked packet log viewer"`

## 9. Per-Rule Hit Counters `[Sonnet]`

Atomic `u64 hits` per `fw_rule_t` entry, incremented on each rule match in `fw_check()`. Global counters for CT bypass, blocked inbound, and blocked outbound. Exposed via read-only `/sys/firewall` VFS file.

**Files:** `src/kernel/net/firewall.c` (extend), `src/kernel/fs/sysfs.c` (extend)

> [!NOTE]
> `hits` in `fw_rule_t` uses `__sync_fetch_and_add(&rule->hits, 1)` (GCC built-in atomic); this avoids holding the spinlock for the counter increment on the hot path -- the counter can tolerate a few missed counts under race. Global counters: `fw_ct_bypass_count` (auto-allowed by CT), `fw_blocked_in_count` (default BLOCK inbound), `fw_blocked_out_count` (default BLOCK outbound) -- all `uint64_t`; also incremented with `__sync_fetch_and_add`. The `/sys/firewall` VFS file: opened as a read-only synthetic file; on `read()`: call `fw_dump_stats(buf)` which formats all rule hits + globals into a text table. `fw list` in the CLI reads `rule->hits` directly (no VFS needed).

- [ ] `uint64_t hits` field already in `fw_rule_t` from §1; confirm it is the last field to avoid struct padding surprises
- [ ] In `fw_check()` on rule match: `__sync_fetch_and_add(&fw_table[i].hits, 1);` before returning the action
- [ ] Global counters `fw_ct_bypass_count`, `fw_blocked_in_count`, `fw_blocked_out_count`: `__sync_fetch_and_add` in CT bypass path and default-policy BLOCK path
- [ ] `fw_dump_stats(buf, max)` → bytes written: write header `"IDX DIR PROTO DPORT ACT HITS"` then one row per enabled rule; append global counters at bottom
- [ ] Register `/sys/firewall` synthetic VFS file: on read, call `fw_dump_stats()`; on write: return -EROFS
- [ ] `fw_reset_counters()`: zero all `rule->hits` and global counters; exposed as `fw reset` CLI subcommand
- [ ] Commit: `"net/fw: per-rule hit counters atomic u64, CT bypass + blocked globals, /sys/firewall VFS"`

---

## OS Comparison


| ⭐  | Feature                                                 | 🪟 Win11                                                 | 🐧 Linux                                                                | 🚀 Impossible OS                                            |
| --- | ------------------------------------------------------- | -------------------------------------------------------- | ----------------------------------------------------------------------- | ----------------------------------------------------------- |
| 💎  | Packet filter engine                                    | ✅ Windows Firewall (`mpssvc`); thousands of             | ✅ `nftables`/`iptables` in kernel; arbitrary rule                      | ⬜ §1 -- 64-rule compact table; O(64) linear                |
| 💎  | Stateful CT integration                                 | ✅ WFP stateful inspection; SYN-flood protection         | ✅ `conntrack` module; `nftables ct state                               | ⬜ §3 -- CT 4-tuple lookup (reversed) before                |
| 💎  | IP layer hooks                                          | ✅ WFP callout drivers; NDIS filter                      | ✅ netfilter hook points: `NF_INET_PRE_ROUTING`, `NF_INET_POST_ROUTING` | ⬜ §2 -- single `fw_check()` call in `ipv4_handle`          |
| 💎  | Default ruleset                                         | ✅ Default Windows Firewall blocks all                   | ✅ `ufw` default-deny inbound; distro-configurable                      | ⬜ §4 -- 8 explicit ALLOW rules installed                   |
| 💎  | IPv6 filter                                             | ✅ WFP handles IPv4 + IPv6                               | ✅ `nf_tables` dual-stack; `ip6tables` or unified                       | ⬜ §5 -- `fw_ip6_match()` byte-level prefix compare; shared |
| 💎  | Firewall CLI                                            | ✅ `netsh advfirewall`; `New-NetFirewallRule` PowerShell | ✅ `nft list ruleset`; `iptables -L`;                                   | ⬜ §6 -- single `fw` command; port-range parser             |
| 💎  | Registry persistence                                    | ✅ Rules under `SharedAccess\Parameters\FirewallPolicy`  | ✅ `iptables-save`/`nftables.conf` flat files                           | ⬜ §7 -- Win32 Registry API (`RegSetValueEx`); auto-save    |
| 💎  | `firewall.cpl` applet                                   | ✅ Windows Defender Firewall GUI in                      | ✅ `gufw` (userspace); `firewall-config` for firewalld                  | ⬜ §8 -- kernel-native desktop applet; 100-entry block      |
| ⭐  | Per-rule atomic hit counters + `/sys/firewall` VFS file | ✅ Per-rule packet/byte counters in WFP;                 | ✅ `nftables` per-rule `counter packets bytes`;                         | ⬜ §9 -- `⭐` atomic `u64` per rule                         |

> **After §1–§9:** Impossible OS has a kernel-native stateful firewall matching Windows Firewall and Linux `nftables` in coverage. The atomic per-rule hit counters exposed via `/sys/firewall` (`⭐`) provide real-time observability without any userspace tool dependency. The `firewall.cpl` applet gives GUI management from day one, and Registry persistence means all rules survive reboots using the same Win32 API that system administrators already know.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] Default policy: after boot, `fw status` shows `Inbound default: BLOCK | Outbound default: ALLOW | Rules: 8`
- [ ] Outbound allow: `http_get("http://example.com/", ...)` succeeds (not blocked by default policy)
- [ ] Inbound block: start TCP listener; connect from QEMU host without an established outbound session; connection refused (packet dropped silently -- no RST sent); serial log shows `[FW] DROP IN` in debug build
- [ ] CT bypass: initiate outbound TCP connection; inbound reply packets auto-allowed without explicit rule; `fw status` shows non-zero CT bypass count
- [ ] SYN guard: forge a TCP SYN packet on an active CT SYN_SENT connection; it must pass to the rule table, not CT-bypass
- [ ] ICMPv6 bypass: `ndp_resolve()` works (NA packet received) even with inbound default BLOCK policy
- [ ] `fw add in tcp 8080 block` → `fw list` shows the new rule; inbound TCP to port 8080 dropped; `fw list` shows `HITS=N` after test traffic
- [ ] `fw remove 8` → rule removed; `fw list` no longer shows it; traffic to 8080 flows again
- [ ] `fw flush` → rules reset to defaults (8 rules); `fw list` shows only default rules
- [ ] Registry: `fw add in udp 9999 block` → reboot → rule survives; `fw list` after reboot shows `udp in 9999 BLOCK`
- [ ] `/sys/firewall`: `cat /sys/firewall` → text table with rule hit counts; non-zero hits after test traffic
- [ ] IPv6: inbound TCP on IPv6 dropped by default BLOCK policy; outbound allowed; ICMPv6 NDP continues working
- [ ] `firewall.cpl`: applet opens; enable toggle works; add/remove rule updates `fw list` output
- [ ] Commit: `"net: complete firewall -- packet filter, CT stateful, default ruleset, CLI, Registry, firewall.cpl"`
