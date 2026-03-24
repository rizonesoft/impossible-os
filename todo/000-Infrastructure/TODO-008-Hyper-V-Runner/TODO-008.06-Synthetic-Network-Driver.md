# 008.06-Synthetic-Network-Driver — Hyper-V Synthetic NIC (netvsc)

> **Goal:** Implement the Hyper-V Synthetic Network Driver (netvsc) to provide
> Ethernet connectivity on Hyper-V Generation 2 virtual machines. The driver
> communicates over VMBus using the RNDIS (Remote Network Driver Interface
> Specification) protocol to send/receive Ethernet frames — this is the **only**
> way to get networking on Hyper-V Gen 2 (the emulated E1000 NIC is absent).

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL ring buffers, send/receive buffers, and transfer areas (> 4 KB). `kmalloc` is ONLY for small kernel structs (≤ 4 KB). See `rules.md` Known Gotchas.

> [!WARNING]
> **Clean-room implementation.** Implement from the [Hyper-V TLFS](https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/tlfs)
> (public spec) and public RNDIS documentation. Do NOT reference Linux `hv_netvsc.c` (GPL contamination risk).

> [!IMPORTANT]
> **Spec Reference:** Architecture and protocol details reference the
> [Synthetic Network Driver Spec](file:///home/derickpayne/impossible-os/docs/specs/hyper-v/synthetic-network-driver.md)
> in the repo at `docs/specs/hyper-v/synthetic-network-driver.md`.

---

## 1. VMBus Channel Setup & RNDIS Protocol Negotiation

**Prompt:** The netvsc driver requires opening a VMBus channel to the Network VSP (GUID `F8615163-DF3E-46C5-913F-F2D2F965ED0E`) in the host partition. Once the channel is open, the driver must negotiate the RNDIS protocol: send `RNDIS_INITIALIZE_MSG` (type `0x00000002`) with RNDIS version 1.0 (major=1, minor=0), max transfer size, and max packets per message. The host responds with `RNDIS_INITIALIZE_CMPLT` confirming negotiation. The driver must allocate PMM-backed send and receive buffers, establish GPADL (Guest Physical Address Descriptor List) handles for shared memory with the host, and send `NVSP_MSG1_TYPE_SEND_SEND_BUF` / `NVSP_MSG1_TYPE_SEND_RECV_BUF` messages to register them. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"drivers: netvsc VMBus channel and RNDIS init"`. Add notes directly in this TODO section. After implementation, save any gotchas, solutions, and important information to MCP memory (`mcp_memory_create_entities` / `mcp_memory_add_observations`).

- [ ] Create `src/kernel/drivers/hyperv/netvsc.c` and `include/kernel/drivers/hyperv/netvsc.h`
- [ ] Open VMBus channel for Network VSP GUID `F8615163-DF3E-46C5-913F-F2D2F965ED0E`
- [ ] Negotiate NVSP protocol version (Win10 → Win8.1 → Win8 fallback)
  - [ ] Send `NVSP_MSG_TYPE_INIT` with requested version
  - [ ] Handle `NVSP_MSG_TYPE_INIT_COMPLETE` — verify status
- [ ] Allocate send buffer via `pmm_alloc_contiguous()` (default 1 MiB, divided into 6144-byte chunks)
- [ ] Allocate receive buffer via `pmm_alloc_contiguous()` (default 1 MiB, partitioned into MTU-sized sections)
- [ ] Establish GPADL handles for send and receive buffers with the host
- [ ] Send `NVSP_MSG1_TYPE_SEND_SEND_BUF` — register send buffer GPADL with NetVSP
- [ ] Send `NVSP_MSG1_TYPE_SEND_RECV_BUF` — register receive buffer GPADL with NetVSP
- [ ] Send `RNDIS_INITIALIZE_MSG` (type `0x00000002`, version 1.0, max transfer size)
- [ ] Handle `RNDIS_INITIALIZE_CMPLT` — store `max_xfer_size`, `max_pkts_per_msg`
- [ ] Log: `[netvsc] VMBus channel opened, RNDIS v1.0 negotiated`
- [ ] Commit: `"drivers: netvsc VMBus channel and RNDIS init"`

---

## 2. MAC Address & Device Configuration

**Prompt:** After RNDIS initialization, query the device for its hardware (permanent) MAC address and the current MAC address using RNDIS OID queries. The netvsc driver sends `RNDIS_QUERY_MSG` (type `0x00000004`) with OID `OID_802_3_PERMANENT_ADDRESS` (`0x01010101`) and `OID_802_3_CURRENT_ADDRESS` (`0x01010102`). The host responds with `RNDIS_QUERY_CMPLT` containing the 6-byte Ethernet address. Also query `OID_GEN_MAXIMUM_FRAME_SIZE` for MTU and `OID_GEN_LINK_SPEED` for link speed. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"drivers: netvsc MAC address and device config"`. Add notes directly in this TODO section. After implementation, save any gotchas, solutions, and important information to MCP memory (`mcp_memory_create_entities` / `mcp_memory_add_observations`).

- [ ] Implement `netvsc_rndis_query(oid, buf, len)` — generic RNDIS OID query helper
  - [ ] Build `RNDIS_QUERY_MSG` (type `0x00000004`) with OID, info buffer offset/length
  - [ ] Send via VMBus, wait for `RNDIS_QUERY_CMPLT` (type `0x00000005`)
  - [ ] Copy result from info buffer into caller's buffer
- [ ] Query `OID_802_3_PERMANENT_ADDRESS` (`0x01010101`) — 6-byte permanent MAC
- [ ] Query `OID_802_3_CURRENT_ADDRESS` (`0x01010102`) — 6-byte current MAC
- [ ] Query `OID_GEN_MAXIMUM_FRAME_SIZE` (`0x00010106`) — MTU (typically 1514)
- [ ] Query `OID_GEN_LINK_SPEED` (`0x00010107`) — link speed in 100 bps units
- [ ] Store MAC and config in `netvsc_dev` struct
- [ ] Log: `[netvsc] MAC=%02x:%02x:%02x:%02x:%02x:%02x, MTU=%u, speed=%u Mbps`
- [ ] Commit: `"drivers: netvsc MAC address and device config"`

---

## 3. RNDIS Packet Filter & Link Up

**Prompt:** Before the NIC can receive traffic, the driver must set the RNDIS packet filter to accept directed (unicast), broadcast, and optionally multicast frames. Send `RNDIS_SET_MSG` (type `0x00000009`) with OID `OID_GEN_CURRENT_PACKET_FILTER` (`0x0001010E`) and the desired filter bitmask (`NDIS_PACKET_TYPE_DIRECTED | NDIS_PACKET_TYPE_BROADCAST`). The host responds with `RNDIS_SET_CMPLT` (type `0x0000000A`). After setting the filter, the NIC is live and can send/receive Ethernet frames. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"drivers: netvsc packet filter and link up"`. Add notes directly in this TODO section. After implementation, save any gotchas, solutions, and important information to MCP memory (`mcp_memory_create_entities` / `mcp_memory_add_observations`).

- [ ] Implement `netvsc_rndis_set(oid, value, len)` — generic RNDIS OID set helper
  - [ ] Build `RNDIS_SET_MSG` (type `0x00000009`) with OID, info buffer
  - [ ] Send via VMBus, wait for `RNDIS_SET_CMPLT` (type `0x0000000A`)
  - [ ] Verify status == `RNDIS_STATUS_SUCCESS`
- [ ] Set `OID_GEN_CURRENT_PACKET_FILTER` (`0x0001010E`) with filter flags:
  - [ ] `NDIS_PACKET_TYPE_DIRECTED` (0x01) — unicast to our MAC
  - [ ] `NDIS_PACKET_TYPE_BROADCAST` (0x02) — broadcast frames
  - [ ] `NDIS_PACKET_TYPE_ALL_MULTICAST` (0x04) — optional, for multicast
- [ ] Mark NIC as link-up in `netvsc_dev` state
- [ ] Log: `[netvsc] Packet filter set, NIC is LINK UP`
- [ ] Commit: `"drivers: netvsc packet filter and link up"`

---

## 4. Transmit Path (netvsc_send)

**Prompt:** Implement the transmit path. When the networking stack calls `netvsc_send(packet, len)`, the driver must encapsulate the Ethernet frame in an RNDIS data message (`RNDIS_PACKET_MSG`, type `0x00000001`). The RNDIS header includes the data offset, data length, and optional out-of-band (OOB) data for offloads. The encapsulated message is then copied into the pre-registered send buffer and transmitted via `vmbus_sendpacket()`. For small packets, use memory copy into the send buffer. For LSO-tagged packets or when the send buffer is exhausted, fall back to passing memory pointers (zero-copy). After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"drivers: netvsc transmit path"`. Add notes directly in this TODO section. After implementation, save any gotchas, solutions, and important information to MCP memory (`mcp_memory_create_entities` / `mcp_memory_add_observations`).

- [ ] Implement `netvsc_send(const void *packet, size_t len)`:
  - [ ] Build `RNDIS_PACKET_MSG` header (type `0x00000001`):
    - [ ] `data_offset` = offset from header start to Ethernet frame
    - [ ] `data_len` = Ethernet frame length
    - [ ] `oob_data_offset` / `oob_data_len` = 0 (initially, no offloads)
  - [ ] Copy RNDIS header + Ethernet frame into send buffer chunk (6144-byte slots)
  - [ ] Track send buffer slot allocation (free list / bitmap)
  - [ ] Send via `vmbus_sendpacket()` with send buffer section index
- [ ] Handle send completion callback:
  - [ ] VMBus returns completion with send buffer section indices
  - [ ] Free corresponding send buffer slots for reuse
- [ ] Implement send buffer exhaustion fallback:
  - [ ] If no send buffer slots available, send packet via VMBus GPADL page set
  - [ ] Track outstanding page-set sends for completion
- [ ] Wire to Ethernet layer: `ethernet_send()` → `netvsc_send()`
- [ ] Log: `[netvsc] TX: %u bytes sent`
- [ ] Commit: `"drivers: netvsc transmit path"`

---

## 5. Receive Path (netvsc → ethernet_receive)

**Prompt:** Implement the receive path. The host places incoming packets into the pre-registered receive buffer and sends a VMBus completion message containing the receive buffer section index and length. The driver reads the RNDIS data message from the receive buffer, strips the `RNDIS_PACKET_MSG` header, extracts the raw Ethernet frame, and passes it up to `ethernet_receive()`. A single VMBus completion may reference multiple RNDIS packets (batched receive). After processing, the driver must signal the host to reclaim the receive buffer section. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"drivers: netvsc receive path"`. Add notes directly in this TODO section. After implementation, save any gotchas, solutions, and important information to MCP memory (`mcp_memory_create_entities` / `mcp_memory_add_observations`).

- [ ] Implement receive callback (triggered by VMBus channel interrupt):
  - [ ] Read VMBus transfer page packet from receive ring buffer
  - [ ] Extract receive buffer section index and byte count
  - [ ] Parse `RNDIS_PACKET_MSG` header from receive buffer:
    - [ ] Read `data_offset` and `data_len` to locate raw Ethernet frame
    - [ ] Handle batched messages: single VMBus packet may contain multiple RNDIS messages
  - [ ] For each RNDIS message: extract Ethernet frame, call `ethernet_receive(frame, len)`
- [ ] Signal host to reclaim receive buffer section after processing
- [ ] Handle `RNDIS_INDICATE_STATUS_MSG` (type `0x00000007`):
  - [ ] `RNDIS_STATUS_MEDIA_CONNECT` — link up event
  - [ ] `RNDIS_STATUS_MEDIA_DISCONNECT` — link down event
  - [ ] Update `netvsc_dev.link_status` accordingly
- [ ] Log: `[netvsc] RX: %u bytes received, %u packets`
- [ ] Commit: `"drivers: netvsc receive path"`

---

## 6. NIC Registration & Ethernet Layer Integration

**Prompt:** Register the netvsc adapter with the kernel's Ethernet layer so it appears as a standard NIC alongside RTL8139 / virtio-net. The netvsc driver must register a `nic_ops` struct with `send`, `get_mac`, and `get_mtu` callbacks. The driver init should be called from `boot_storage.c` after `vmbus_init()` succeeds, gated on Hyper-V detection. On non-Hyper-V platforms, the init is silently skipped. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"drivers: netvsc NIC registration"`. Add notes directly in this TODO section. After implementation, save any gotchas, solutions, and important information to MCP memory (`mcp_memory_create_entities` / `mcp_memory_add_observations`).

- [ ] Define `netvsc_nic_ops` with callbacks:
  - [ ] `.send = netvsc_send`
  - [ ] `.get_mac = netvsc_get_mac`
  - [ ] `.get_mtu = netvsc_get_mtu`
  - [ ] `.get_link_status = netvsc_get_link_status`
- [ ] Register NIC: `nic_register("hyperv-net0", &netvsc_nic_ops)`
- [ ] Call `netvsc_init()` from `boot_storage.c` after `vmbus_init()` + `storvsc_init()`
- [ ] Gate on Hyper-V: `if (platform_get() != PLATFORM_HYPERV) return;`
- [ ] Fallback: if VMBus channel not offered by host, log warning and skip
- [ ] Log: `[OK] netvsc: Hyper-V synthetic NIC registered (MAC=%02x:...)`
- [ ] Commit: `"drivers: netvsc NIC registration"`

---

## 7. DHCP & Basic Connectivity Test

**Prompt:** Validate end-to-end networking by performing a DHCP handshake and ping test over the netvsc adapter on a Hyper-V Gen 2 VM. This requires the existing DHCP client and ICMP echo responder to work over the new netvsc NIC. Verify that the VM obtains an IP address from the Hyper-V Default Switch and can respond to ICMP echo requests from the host. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"drivers: netvsc end-to-end connectivity"`. Add notes directly in this TODO section. After implementation, save any gotchas, solutions, and important information to MCP memory (`mcp_memory_create_entities` / `mcp_memory_add_observations`).

- [ ] Boot Impossible OS on Hyper-V Gen 2 with virtual switch attached
- [ ] Verify netvsc init log messages appear (channel open, RNDIS negotiation, MAC address)
- [ ] Verify DHCP discover → offer → request → ack sequence completes
- [ ] Verify IP address assigned and logged: `[net] IP: x.x.x.x via DHCP`
- [ ] Ping from host to VM — verify ICMP echo reply
- [ ] Ping from VM to host (if outbound ICMP implemented)
- [ ] Test with Hyper-V Default Switch (NAT) and Private Switch (isolated)
- [ ] Commit: `"drivers: netvsc end-to-end connectivity"`

---

## 8. Receive Side Scaling (RSS)

**Prompt:** Implement Receive Side Scaling to distribute incoming packet processing across multiple virtual CPUs within the guest. The netvsc driver advertises RSS capability during RNDIS initialization. Configure RSS parameters via `OID_GEN_RECEIVE_SCALE_PARAMETERS` — set the hash type (IPv4/IPv6 + TCP), hash function (Toeplitz), indirection table mapping queue indices to vCPUs, and the hash secret key. Without RSS, all incoming traffic interrupts are bound to vCPU 0, creating a bottleneck on high-throughput workloads. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"drivers: netvsc RSS support"`. Add notes directly in this TODO section. After implementation, save any gotchas, solutions, and important information to MCP memory (`mcp_memory_create_entities` / `mcp_memory_add_observations`).

- [ ] Negotiate RSS capability during RNDIS init (advertise in `RNDIS_INITIALIZE_MSG`)
- [ ] Configure RSS via `OID_GEN_RECEIVE_SCALE_PARAMETERS`:
  - [ ] Hash type: `NDIS_HASH_IPV4 | NDIS_HASH_TCP_IPV4 | NDIS_HASH_IPV6 | NDIS_HASH_TCP_IPV6`
  - [ ] Hash function: Toeplitz (standard for RSS)
  - [ ] Indirection table: map hash values to vCPU indices (round-robin across online vCPUs)
  - [ ] Hash secret key: 40-byte random key for Toeplitz computation
- [ ] Open sub-channels: request additional VMBus channels (one per vCPU, up to 64)
  - [ ] Send `NVSP_MSG5_TYPE_SUBCHANNEL` request with `num_sub_channels`
  - [ ] Handle sub-channel offers from host
  - [ ] Each sub-channel has its own send/receive ring buffers
- [ ] Route incoming packets to per-vCPU queues based on RSS hash
- [ ] Log: `[netvsc] RSS enabled: %u queues, hash=Toeplitz`
- [ ] Commit: `"drivers: netvsc RSS support"`

---

## 9. Checksum & Segmentation Offloads

**Prompt:** Enable hardware offloads to reduce CPU overhead during packet processing. The netvsc driver supports checksum offload (TX and RX) and Large Send Offload (LSO/LSOv2) for both IPv4 and IPv6. Set offload capabilities via `OID_TCP_OFFLOAD_PARAMETERS`. For transmit, set the appropriate flags in the RNDIS OOB data of `RNDIS_PACKET_MSG` to indicate that the host should compute TCP/UDP/IP checksums. For receive, parse the OOB data to determine whether checksums were verified by the host. LSO allows sending segments up to 64 KB that the host segments into MTU-sized packets. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"drivers: netvsc checksum and LSO offloads"`. Add notes directly in this TODO section. After implementation, save any gotchas, solutions, and important information to MCP memory (`mcp_memory_create_entities` / `mcp_memory_add_observations`).

- [ ] Query offload capabilities: `OID_TCP_OFFLOAD_HARDWARE_CAPABILITIES`
- [ ] Set offload parameters: `OID_TCP_OFFLOAD_PARAMETERS`
  - [ ] IPv4 TX checksum (IP + TCP + UDP)
  - [ ] IPv6 TX checksum (TCP + UDP)
  - [ ] IPv4/IPv6 RX checksum verification
  - [ ] LSOv2 for IPv4 (max offload size 64 KB)
  - [ ] LSOv2 for IPv6 (max offload size 64 KB)
- [ ] TX path: set checksum offload flags in RNDIS per-packet OOB info:
  - [ ] `NDIS_TCP_IP_CHECKSUM_NET_BUFFER_LIST_INFO` — indicate IP/TCP/UDP checksum offload
  - [ ] For LSO: set `NDIS_TCP_LARGE_SEND_OFFLOAD_NET_BUFFER_LIST_INFO` with MSS
- [ ] RX path: parse OOB data for checksum status:
  - [ ] `NDIS_TCP_IP_CHECKSUM_NET_BUFFER_LIST_INFO` — check if checksum valid/invalid
  - [ ] Pass checksum status to TCP/IP stack (skip software checksum verification if HW verified)
- [ ] Log: `[netvsc] Offloads: TX csum=%s, RX csum=%s, LSOv2=%s`
- [ ] Commit: `"drivers: netvsc checksum and LSO offloads"`

---

## 10. SR-IOV / VF Failover Awareness (Advanced)

**Prompt:** When the Hyper-V host has SR-IOV enabled and a physical NIC supports Virtual Functions (VFs), the host may assign a hardware VF directly to the guest for near-bare-metal throughput. The netvsc driver must detect when a VF is assigned (via PCI bus hot-plug event), bind its protocol edge to the VF driver, and transparently route data traffic through the hardware path while keeping the synthetic interface as the control/management path. On VF removal (Live Migration, host resource exhaustion), the netvsc must seamlessly fail back to the VMBus synthetic data path without dropping TCP connections. This maintains both peak performance and infrastructure mobility. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"drivers: netvsc SR-IOV VF failover"`. Add notes directly in this TODO section. After implementation, save any gotchas, solutions, and important information to MCP memory (`mcp_memory_create_entities` / `mcp_memory_add_observations`).

- [ ] Detect VF assignment: listen for `NVSP_MSG4_TYPE_SEND_VF_ASSOCIATION` from host
  - [ ] Message contains VF serial number and allocation status (assigned/revoked)
- [ ] On VF assigned:
  - [ ] Detect new PCI network device (VF) via PCI enumeration
  - [ ] Load appropriate VF driver (e.g., Mellanox mlx5 VF stub, Intel VF stub)
  - [ ] Bind netvsc protocol edge to VF driver
  - [ ] Set VF MAC address to match netvsc synthetic MAC (transparent bonding)
  - [ ] Redirect data path: TX/RX flows through hardware VF, bypassing VMBus
  - [ ] Keep netvsc synthetic path alive for control messages
- [ ] On VF revoked (Live Migration / host-initiated):
  - [ ] Receive `NVSP_MSG4_TYPE_SEND_VF_ASSOCIATION` with `allocated = 0`
  - [ ] Immediately failback: redirect all TX/RX to VMBus synthetic path
  - [ ] No TCP connection drops — seamless transition
  - [ ] Tear down VF driver, release PCI resources
- [ ] On destination host (post-Live Migration):
  - [ ] New VF assigned → re-bind and failover to hardware path
- [ ] Log: `[netvsc] VF assigned: PCI %02x:%02x.%x → hardware data path`
- [ ] Log: `[netvsc] VF revoked → failback to synthetic VMBus path`
- [ ] Commit: `"drivers: netvsc SR-IOV VF failover"`

---

## 11. VLAN Tagging Support

**Prompt:** Implement IEEE 802.1Q VLAN tagging support for the netvsc adapter. The Hyper-V virtual switch can assign VLAN IDs to VM network adapters. The netvsc driver must handle VLAN tag insertion on transmit and VLAN tag stripping on receive, passing the VLAN ID and priority through the RNDIS OOB data. This is necessary for enterprise network segmentation on Hyper-V. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"drivers: netvsc VLAN tagging"`. Add notes directly in this TODO section. After implementation, save any gotchas, solutions, and important information to MCP memory (`mcp_memory_create_entities` / `mcp_memory_add_observations`).

- [ ] Query VLAN capability: `OID_GEN_VLAN_ID`
- [ ] TX path: insert VLAN tag in RNDIS OOB data:
  - [ ] `NDIS_NET_BUFFER_LIST_8021Q_INFO` — set VLAN ID and 802.1p priority
- [ ] RX path: parse VLAN tag from RNDIS OOB data:
  - [ ] Extract VLAN ID and priority from `NDIS_NET_BUFFER_LIST_8021Q_INFO`
  - [ ] Pass VLAN info to Ethernet layer for filtering/routing
- [ ] Support trunk mode: accept frames from multiple VLANs
- [ ] Log: `[netvsc] VLAN: ID=%u, priority=%u`
- [ ] Commit: `"drivers: netvsc VLAN tagging"`

---

## 12. Link State Change & Status Monitoring

**Prompt:** Implement robust link state change handling and NIC status monitoring. The host can send `RNDIS_INDICATE_STATUS_MSG` at any time to signal media connect/disconnect, network speed changes, or other status updates. The driver must handle these asynchronously and update the NIC state. Expose NIC statistics (packets sent/received, errors, bytes transferred) via Registry for the Network Manager GUI. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"drivers: netvsc link state and statistics"`. Add notes directly in this TODO section. After implementation, save any gotchas, solutions, and important information to MCP memory (`mcp_memory_create_entities` / `mcp_memory_add_observations`).

- [ ] Handle `RNDIS_INDICATE_STATUS_MSG` (type `0x00000007`) asynchronously:
  - [ ] `RNDIS_STATUS_MEDIA_CONNECT` (`0x4001000B`) — update link state to UP
  - [ ] `RNDIS_STATUS_MEDIA_DISCONNECT` (`0x4001000C`) — update link state to DOWN
  - [ ] `RNDIS_STATUS_LINK_SPEED_CHANGE` — re-query link speed
- [ ] Maintain NIC statistics counters:
  - [ ] `tx_packets`, `tx_bytes`, `tx_errors`, `tx_dropped`
  - [ ] `rx_packets`, `rx_bytes`, `rx_errors`, `rx_dropped`
- [ ] Expose via Registry:
  - [ ] `HKLM\HARDWARE\Network\hyperv-net0\MAC`
  - [ ] `HKLM\HARDWARE\Network\hyperv-net0\LinkSpeed`
  - [ ] `HKLM\HARDWARE\Network\hyperv-net0\LinkStatus`
  - [ ] `HKLM\HARDWARE\Network\hyperv-net0\TxPackets`
  - [ ] `HKLM\HARDWARE\Network\hyperv-net0\RxPackets`
- [ ] Wire to Network Manager GUI for real-time status display
- [ ] Log: `[netvsc] Link state changed: %s (speed=%u Mbps)`
- [ ] Commit: `"drivers: netvsc link state and statistics"`

---

## Priority Order

| Priority | Section                               | Description                                              |
|----------|---------------------------------------|----------------------------------------------------------|
| 🔴 P0    | §1 VMBus Channel & RNDIS Init        | Foundation — channel open + protocol negotiation         |
| 🔴 P0    | §2 MAC Address & Device Config       | Foundation — need MAC and MTU before transmit/receive    |
| 🔴 P0    | §3 Packet Filter & Link Up           | Foundation — NIC must be enabled to send/receive         |
| 🔴 P0    | §4 Transmit Path                     | Core — send Ethernet frames via RNDIS over VMBus         |
| 🔴 P0    | §5 Receive Path                      | Core — receive Ethernet frames from host                 |
| 🟠 P1    | §6 NIC Registration                  | Integration — register with kernel Ethernet layer        |
| 🟠 P1    | §7 DHCP & Connectivity Test          | Validation — end-to-end networking proof                  |
| 🟡 P2    | §8 RSS                               | Performance — distribute RX across vCPUs                 |
| 🟡 P2    | §9 Checksum & LSO Offloads           | Performance — reduce CPU overhead                        |
| 🟡 P2    | §11 VLAN Tagging                     | Enterprise — network segmentation                        |
| 🟢 P3    | §10 SR-IOV / VF Failover             | Advanced — hardware-accelerated data path                |
| 🟢 P3    | §12 Link State & Statistics          | Monitoring — NIC health and metrics                      |

---

## OS Comparison

| ⭐ | Feature                              | 🪟 Windows 11 (Native)                  | 🐧 Linux (hv_netvsc)                    | 🚀 Impossible OS                                |
| -- | ------------------------------------ | ---------------------------------------- | ---------------------------------------- | ------------------------------------------------ |
| 💎 | VMBus channel + RNDIS init           | ✅ Native (built-in)                     | ✅ `hv_netvsc.ko`                        | ⬜ §1 P0 — not implemented                      |
| 💎 | MAC address query (RNDIS OID)        | ✅ Native                                | ✅ RNDIS query                            | ⬜ §2 P0 — not implemented                      |
| 💎 | Packet filter & link up              | ✅ Native NDIS                           | ✅ `rndis_filter_set_packet_filter()`     | ⬜ §3 P0 — not implemented                      |
| 💎 | TX path (RNDIS encapsulation)        | ✅ Native                                | ✅ `netvsc_start_xmit()`                  | ⬜ §4 P0 — not implemented                      |
| 💎 | RX path (RNDIS extraction)           | ✅ Native                                | ✅ `netvsc_receive()`                      | ⬜ §5 P0 — not implemented                      |
| 💎 | NIC registration                     | ✅ NDIS miniport                          | ✅ `register_netdevice()`                  | ⬜ §6 P1 — not implemented                      |
| 💎 | DHCP + ping over netvsc              | ✅ Native                                | ✅ Works out of box                        | ⬜ §7 P1 — not tested                           |
| 💎 | Receive Side Scaling (RSS)           | ✅ Per-vCPU queues                       | ✅ RSS + sub-channels                     | ⬜ §8 P2 — single-queue only                    |
| 💎 | Checksum offload (TX/RX)             | ✅ Full IPv4/IPv6                        | ✅ `NETIF_F_IP_CSUM`                      | ⬜ §9 P2 — software checksum                    |
| 💎 | Large Send Offload (LSOv2)           | ✅ 64 KB segments                        | ✅ `NETIF_F_TSO` / `TSO6`                 | ⬜ §9 P2 — no LSO                               |
| 💎 | SR-IOV / VF failover                 | ✅ Transparent VF bonding                | ✅ `netvsc_vf_join()`                      | ⬜ §10 P3 — no SR-IOV                           |
| 💎 | VLAN tagging (802.1Q)                | ✅ Native NDIS                           | ✅ `NETIF_F_HW_VLAN_*`                    | ⬜ §11 P2 — not implemented                     |
| 💎 | Link state monitoring                | ✅ NDIS status indication                | ✅ `netif_carrier_on/off()`                | ⬜ §12 P3 — not implemented                     |
| 💎 | NIC statistics (Registry/sysfs)      | ✅ Performance counters                  | ✅ `ethtool -S`                            | ⬜ §12 P3 — no statistics                       |
| 💎 | **Full netvsc stack**                | ✅ Native                                | ✅ With hv_netvsc module                   | ⬜ **§1-§7 required for basic networking**       |

> **After §1-§7:** Impossible OS has basic Ethernet connectivity on Hyper-V Gen 2.
> **After §8-§9:** Performance matches Linux's netvsc with multi-queue and offloads.
> **After §10-§12:** Full enterprise-grade networking with SR-IOV, VLAN, and monitoring.

---

## Key Files

| File                                                | Change  | Purpose                                        |
| --------------------------------------------------- | ------- | ---------------------------------------------- |
| `src/kernel/drivers/hyperv/netvsc.c`                | NEW     | Synthetic NIC driver (RNDIS over VMBus)        |
| `include/kernel/drivers/hyperv/netvsc.h`            | NEW     | RNDIS protocol types, NVSP messages, public API|
| `src/kernel/drivers/hyperv/vmbus.c`                 | MODIFY  | May need VMBus transfer page packet support    |
| `include/kernel/drivers/hyperv/vmbus.h`             | MODIFY  | Transfer page packet types for netvsc          |
| `src/kernel/main/boot_storage.c`                    | MODIFY  | Add `netvsc_init()` call after VMBus init      |
| `src/kernel/net/ethernet.c`                         | MODIFY  | Register netvsc as NIC backend                 |
