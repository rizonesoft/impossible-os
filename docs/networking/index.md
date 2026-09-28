# Networking

The network stack and the programs built on it: the IPv4 core that runs today (Ethernet, ARP, IPv4, ICMP, UDP and DHCP) and the planned TCP, DNS and sockets, HTTP and TLS, IPv6, firewall, time sync and network tools, and the protocol engines behind the browser, SSH and FTP, email and PDF apps. The network card driver is covered in [Network Drivers](../hardware/network-drivers.md), and the app windows in the apps roadmaps.

## Roadmap Overviews

One page per networking roadmap file, each following the [page contract](../contributing/docs-page-contract.md).

| Document | Topics |
| --- | --- |
| [TCP and Network Infrastructure](tcp-network-infrastructure.md) | The IPv4, ICMP, UDP and DHCP stack today, planned TCP, interfaces, loopback and connection tracking |
| [DNS Resolver and Sockets](dns-sockets.md) | The ping and netinfo calls today, planned resolver, cache and BSD sockets |
| [HTTP, HTTPS and TLS](http-tls.md) | Vendored but unbuilt Mbed TLS, planned HTTP client, certificate store and `wget` / `curl` |
| [IPv6 Dual Stack](ipv6.md) | Planned IPv6 packets, Neighbor Discovery, SLAAC, DHCPv6 and `AF_INET6` |
| [Network Firewall](firewall.md) | Today's unfiltered surface, planned stateful rules, Registry persistence and counters |
| [Time Sync, Network Status and Winsock](ntp-status-winsock.md) | `ifconfig`, `ping` and kernel clock discipline today, planned NTP, tools and `ws2_32.dll` |
| [Web Browser](web-browser.md) | Planned HTML, CSS and layout engine on the system image and font code |
| [SSH, FTP and SMB Clients](ssh-ftp.md) | Monocypher already in the kernel, planned FTP, SSH, SFTP and SMB shares |
| [Email Protocols](email-client.md) | Planned MIME parser, SMTP, POP3, IMAP and accounts |
| [PDF Viewer Engine](pdf-viewer.md) | Planned PDF parser and renderer on stb zlib, image and font decoders |
| [Remote Syslog Forwarding](syslog-forwarding.md) | Planned RFC 5424 forwarding of the kernel log over UDP |
