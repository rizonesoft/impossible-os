# 06 Networking

This domain tracks networking inside the OS runtime: packet flow, protocol layers, and the kernel-side network API surface.

## Belongs Here

- Kernel network stack work such as Ethernet, ARP, IPv4, ICMP, UDP, DHCP, and future protocol layers.
- Socket-like runtime behavior, packet routing, and kernel-side network integration.
- Networking features that are shared infrastructure for many apps.

## Does Not Belong Here

- NIC driver implementation details. Put that in [04 Drivers Hardware](../04-drivers-hardware/INDEX.md).
- User-facing internet applications such as browser, SSH, FTP, or email. Put that in [10 Apps](../10-apps/INDEX.md).

## Likely Source Areas

- [src/kernel/net](../src/kernel/net/)
- [src/kernel/drivers/rtl8139.c](../src/kernel/drivers/rtl8139.c)
- [include/kernel/net](../include/kernel/net/)

## Epics

- None yet.

## Active TODOs

- None yet.

## Completed / Doc-converted

- None yet.

## Local Naming

- Use `TODO-01-ipv4-routing.md` as the filename style for new leaf TODOs in this folder.
- Create a parent TODO only when one topic needs multiple leaf files or shared verification.
