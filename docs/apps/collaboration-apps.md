<!-- docs: covers=todo/11-apps/TODO-07-collaboration-apps.md sources=include/kernel/net/net.h,src/kernel/net/arp.c,src/kernel/net/ip.c,user/cmd.c,include/desktop/wm.h reviewed=2026-09-29 order=7 -->
# Collaboration and Network Client Apps

## What is it?

This roadmap plans a set of network client apps: a VNC remote desktop viewer, an IRC chat client and an RSS and Atom feed reader, plus `arp` and `route` diagnostic commands and a stretch VNC server. It deliberately does not re-implement `ping`, `traceroute`, `ifconfig`, `netstat` or `nslookup`, which belong to the networking roadmaps. None of the apps exist yet; only the lower network layers and a basic `ping` ship.

## How does it work?

**Today.** The kernel network stack has Ethernet, ARP, IPv4, ICMP, UDP and DHCP ([`net.h`](../../include/kernel/net/net.h)), with no TCP or sockets. What the diagnostics part can already build on:

- **ARP.** `arp_init()`, `arp_resolve()`, `arp_request()` and `arp_handle()` ([`arp.c`](../../src/kernel/net/arp.c)) keep the address cache, but nothing can list or delete its entries yet.
- **Routing.** There is no routing table: IP sends off-link traffic to the single gateway from the network configuration ([`ip.c`](../../src/kernel/net/ip.c)).
- **ICMP.** `icmp_send_echo()` sends echo requests, and the shell's `ping` and `ifconfig` commands live in [`cmd.c`](../../user/cmd.c).
- **Windows.** `wm_create_window()` and `wm_mark_dirty()` ([`wm.h`](../../include/desktop/wm.h)); the capture and input-injection hooks a VNC server needs do not exist.

**Planned design.**

1. **VNC client.** The RFB 3.8 handshake, VNC password authentication (DES with the bit-reversed key the protocol uses), the Raw, CopyRect, RRE and Hextile encodings drawn with `gfx_blit()`, keyboard and mouse forwarding, a Ctrl+Alt+Del button and the last host remembered in the Registry.
2. **IRC client.** Plain and TLS connections on ports 6667 and 6697, the core commands (`NICK`, `USER`, `JOIN`, `PRIVMSG`, `NOTICE`, `PART`, `QUIT`, `PING` and `PONG`), a channel sidebar, scroll-back, coloured nicknames, Tab completion, slash commands and a toast for private messages.
3. **Feed reader.** RSS 2.0 and Atom parsed into title, link, date and summary, stored per feed, with unread badges, a reading pane, Open in Browser, feed discovery from a page's `<link rel="alternate">` and a refresh every 30 minutes.
4. **Shell wrappers** for `ping` and `traceroute` over the protocol code the networking roadmap owns.
5. **`arp -a`, `arp -d`, `route print`, `route add` and `route delete`**, which need an ARP cache dump and a real routing table first.
6. **VNC server** (a stretch): capture the compositor's frame buffer, track dirty tiles, and inject received keys and pointer events.

## What are its interfaces?

| Interface | Status |
| --- | --- |
| `arp_resolve()`, `icmp_send_echo()`, `ping`, `ifconfig` | Shipped |
| Kernel TCP sockets, including listening sockets for the VNC server | Planned in the [DNS and Sockets](../networking/dns-sockets.md) roadmap, sections 5 and 6 |
| TLS for IRC on port 6697, HTTP for feeds | Planned in the [HTTP and TLS](../networking/http-tls.md) roadmap |
| ARP cache dump, routing table | Planned in the [Time Sync, Network Status and Winsock](../networking/ntp-status-winsock.md) roadmap |
| Toasts, scheduled tasks | Planned in the [notifications](../graphics/start-menu-tray-notifications.md) and [task scheduler](../desktop/recycle-bin-zip-scheduler.md) roadmaps |

## How do I use it?

Only `ping` runs today:

```text
C:\> ping 10.0.2.2
PING 10.0.2.2
  Sent echo request seq=1
```

It sends four ICMP echo requests; replies are logged by the kernel rather than printed. None of the apps or the `arp` and `route` commands exist yet.

## What is not implemented yet?

Nothing in this roadmap has started:

- [VNC Client](../../todo/11-apps/TODO-07-collaboration-apps.md#1-vnc-client-opus), which needs TCP sockets
- [IRC Chat Client](../../todo/11-apps/TODO-07-collaboration-apps.md#2-irc--chat-client-sonnet) and the [RSS and News Reader](../../todo/11-apps/TODO-07-collaboration-apps.md#3-rss--news-reader-sonnet)
- [`ping` and `traceroute` Commands](../../todo/11-apps/TODO-07-collaboration-apps.md#4-ping--traceroute-commands-sonnet)
- [`arp -a` and `route print`](../../todo/11-apps/TODO-07-collaboration-apps.md#5-network-diagnostic-tools-arp--a--route-print-sonnet), which need a routing table
- [VNC Server](../../todo/11-apps/TODO-07-collaboration-apps.md#6-vnc-server-stretch-opus), a stretch goal that overlaps the remote-session idea in [Long-Term Features](../services/long-term-features.md)

## How does it compare with Windows 11 and Linux?

Windows 11 has no inbox VNC client, IRC client or feed reader; it ships Remote Desktop instead of VNC and includes `ping`, `tracert`, `arp` and `route`. Linux distributions offer Remmina or TigerVNC, HexChat or irssi, Liferea or Newsboat, and `x11vnc` for serving a desktop, with `ip neigh` and `ip route` for diagnostics. The Impossible OS plan puts all three clients and the diagnostic commands in the base system on its own network stack. It does not exist yet.

## See also

- [Collaboration and Network Client Apps roadmap](../../todo/11-apps/TODO-07-collaboration-apps.md)
- [TCP and Network Infrastructure](../networking/tcp-network-infrastructure.md)
- [DNS and Sockets](../networking/dns-sockets.md)
- [Time Sync, Network Status and Winsock](../networking/ntp-status-winsock.md)
- [Web Browser App](web-browser.md)
