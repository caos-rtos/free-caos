# Changelog

Fixes and changes to the caOS sources that a user can notice, newest first.
One entry per fix: date, area, what changed for the user. The details are in
the git history; documentation-only changes are not listed.

Entries about features of the full edition only (CAN, USB HS, tamper, FMAC)
start with `<!-- paid -->`: they are removed from the FREE package.

## free-v0.1.1 - 2026-11-15

### Added
- 2026-11-12  net CLI: `ip stats` prints the counters of the network core -
  every interface, ETH and USB-NCM: frames in and out, drops, VLAN-tagged
  frames dropped (`rx_vlan`), frames without a handler, ETH errors.
- 2026-11-08  net CLI: fixed-target Internet tests for the bench and for a
  quick check of the uplink: `ping test` (8.8.4.4), `dns test` (resolves www.google.com and
  pings it), `ntp test` (one sync from pool.ntp.org, then the configured
  server is restored). `ip gw <ip>|auto` sets the default gateway, `ip info`
  shows mask and gateway.
- 2026-11-05  ex108: the heap API by example, every board, no wiring. `heap all`
  shows allocate/free with wipe and per-task accounting, the blocks a task
  leaves freed by the kernel when it ends, and a shared block surviving its
  producer and adopted by a consumer (`ca_memory_adopt`), each with a verdict.
- 2026-11-05  ex68: `fault emerg` and `fault emerg_rst` exercise the two fault
  policies with an emergency handler (`CA_FAULT_EXEC_EMERGENCY`, alone and with
  `CA_FAULT_RESTART_THREAD`): the handler runs once, then the task is removed
  or renewed, and the example prints PASS or FAIL.

### Changed
- 2026-11-12  eth CLI: `eth stats` is removed - its counters belong to the
  network core, not to the ETH driver, and were out of reach on USB-NCM boards.
  Use `ip stats` (same line, plus `rx_vlan`).
- 2026-11-12  net: 802.1Q priority-tagged frames (VID 0, sent by some managed
  switches when 802.1p QoS is on) are now received: the tag is stripped on
  entry and the priority kept in `meta.rx.vlan_pcp`. caOS does not support
  VLANs - put the board on an untagged (access) port of the switch; a frame
  tagged with a VLAN id is dropped and counted in `rx_dropped_vlan`
  (`ca_net_stats_t`, shown as `rx_vlan` by `ip stats`).
- 2026-11-11  http: a request the server rejects now gets `400 Bad Request`
  instead of a silent close: unknown method, bad or repeated Content-Length,
  body shorter than declared, no blank line, empty target, missing or unknown
  HTTP version. The request line must be `METHOD target HTTP/1.0|1.1` and
  lines end with CRLF only.
- 2026-11-11  http: HEAD answers carry no body at all (`ca_http_resp_puti()`
  honours HEAD, a chunked HEAD sends no terminating chunk) and their
  Content-Length is the one the GET would send. `405` now lists the allowed
  methods in `Allow`.
- 2026-11-11  modbus: a response is accepted only if it is the answer to the
  pending request: protocol id 0, the unit id of the request, exact length.
  Frames with another transaction id are skipped, so a stale answer followed
  by the right one returns the right data; a stale answer alone now ends in
  the `MODBUS_TIMEOUT_MS` timeout instead of an immediate `false`.
- 2026-11-11  mqtt: an incoming QoS 1 PUBLISH is delivered without its packet
  id and acknowledged with PUBACK. QoS 2 (not supported) and malformed
  packets from the broker (QoS 3 or truncated PUBLISH, malformed or repeated
  CONNACK) close the connection. A client id longer than 23 characters is
  cut to 23.
- 2026-11-11  tls: stricter RFC 8446 checks on the server: ClientHello
  compression, `supported_groups` and extension syntax, ChangeCipherSpec
  placement, one KeyUpdate per record, malformed alerts. Alerts carry the
  description the RFC asks for.
- 2026-11-08  net: ARP now tracks whether a neighbour is still reachable. An
  entry not confirmed for `ARP_ENTRY_TTL` stays in use and is checked with a
  unicast ARP; if nothing answers it is dropped and the next packet asks by
  broadcast. TCP, UDP client, TFTP, DNS, NTP and syslog look up the next hop for
  every packet, so a gateway or MAC change no longer breaks an open connection.
  A connection blocks on ARP only at first contact. `arp info` shows
  `[REACHABLE]`, `[STALE]` or `[PROBE]`. New: `ca_arp_lookup`, `ca_arp_confirm`,
  `ca_arp_learn`. `ARP_CACHE_SIZE` now defaults to 16.
- 2026-11-08  net: `ca_net_cfg_t.arp` is gone (`renew_ms` and `cache_size` were
  never read). Remove `.arp = { ... }` from your `ca_net_cfg_t`; use
  `ARP_ENTRY_TTL` and `ARP_CACHE_SIZE` in the project config.
- 2026-11-06  USB (all families): a write to a USB endpoint (CDC console,
  MSC, vendor class) no longer copies its data with interrupts off: the
  ring slots are reserved and published under the lock, the copy runs with
  interrupts on. Longest interrupts-off stretch on F767: 3.9 -> 2.3 us
  (CDC), 3.4 -> 2.4 us (MSC 512 B sectors); independent of the length now.
- 2026-11-05  USB-NCM: transmitted frames are no longer copied either: the NTB
  header is written in front of the frame and the USB driver sends it
  straight from the network buffer, with interrupts off only for a few
  pointer stores instead of the whole copy. All families.
- 2026-11-05  USB-NCM: received frames are no longer copied. The NTB from the
  host is read straight into a network buffer and handed to the stack as it
  is; the OUT NTB size announced to the host goes from 2048 to 1868 bytes
  (still well above one full frame).
- 2026-11-05  ETH (F4, F7): zero copy, as on H7/H5/H7S. The DMA now reads and
  writes the network buffers directly: no copy per frame in either direction,
  and about 12 KB of RAM less (the driver's own frame buffers are gone).
  `tcp buf` now shows the buffers the ETH ring holds, as on the other families.
- 2026-11-05  Scheduler: the fault policy `0x03` (emergency handler, then
  restart) is now `CA_FAULT_EMERGENCY_RESTART`; the old name `CA_FAULT_CHECK`
  is gone. Code using it must switch to the new name (or to
  `CA_FAULT_EXEC_EMERGENCY | CA_FAULT_RESTART_THREAD`).

### Fixed
- 2026-11-14  dns: with the hardware RNG enabled (`EN_RNG`, as in the FREE
  edition), `ca_dns_service_init()` called from `ca_os_configure()` stopped the
  board with a fatal safeguard before the scheduler started (examples 37 and 72
  never printed their banner). The RNG is now mixed into the query id by the
  DNS task when it starts.
- 2026-11-13  ssh: on the H7S3 (H773) the SSH service task now gets 8 KB of
  stack instead of the 20 KB fallback: with every service on, the stack arena
  was full and CLI commands that run as a task (`banner`, ...) failed to start.
- 2026-11-12  tcp: `ca_tcp_abort()` now sends a RST on a synchronised
  connection (RFC 793 ABORT): the peer learns at once instead of on its next
  send. A connection with an `on_rx` callback that closed or aborted it from
  inside the callback no longer sends an ACK from the freed slot.
- 2026-11-12  tftp: a DATA with block 0 during a GET is ignored; it was taken
  as a duplicate and answered with ACK 0.
- 2026-11-12  modbus: two tasks using the client API at the same time could
  read each other's answer - the reply buffer was released before the caller
  had copied it out. The API is now serialised until the answer is read.
- 2026-11-12  mqtt: a remaining length with a fifth byte now closes the
  connection (it left the stream out of step); a SUBACK must answer a
  SUBSCRIBE in flight; at most `MQTT_SUB_PENDING_MAX` (4) SUBSCRIBEs wait for
  their SUBACK and their ids are never reused meanwhile; a connection refused
  by the broker before `ca_tcp_connect()` returned is now reported
  (`on_disconnected`) instead of leaving the client pending.
- 2026-11-12  modbus: a server that sent its answer and closed the connection
  at once (before the calling task woke up) overflowed a semaphore, that is a
  safeguard in the network task. The answer is now delivered and the next call
  sees the connection closed.
- 2026-11-12  arp: a sender with a group MAC address (broadcast, multicast) is
  no longer put in the cache nor answered - every frame for that IP address
  went out to all hosts.
- 2026-11-12  net: on an interface without checksum offload (USB-NCM) and for
  reassembled datagrams, the UDP (when present), TCP and ICMP checksums are now
  verified in software; the Ethernet MAC already did it in hardware. A
  reassembled datagram now goes back out of the interface it came in on.
- 2026-11-12  dns: an answer must carry the question that was asked, not only
  the query id (RFC 5452). ntp: the request carries a random fraction in its
  transmit timestamp, so a reply cannot be forged from off the path.
  `ca_net_proto_ntp_build_request()` takes the nonce as a new argument.
- 2026-11-12  dhcp: the client takes only the replies to its own exchange
  (transaction id and hardware address, RFC 2131 4.4.1). It asks for broadcast
  replies, so it saw the OFFER, ACK and NAK meant for the other clients of the
  LAN and took them: another host's address, or a valid lease dropped.
  `ca_dhcp_offer_t` has two new fields, `xid` and `chaddr`.
- 2026-11-12  ntp: a reply sets the clock only if it answers the request in
  flight (origin timestamp), comes from a configured server, and is usable
  (RFC 4330): not from an unsynchronised server, not a kiss-o'-death, stratum
  1..15, a transmit time. Before, any mode 4 packet on port 123 was taken.
  `ca_net_proto_ntp_parse_response()` now fills every field, in host order.
- 2026-11-12  dns: answers are taken only from the configured server, port 53;
  the query id no longer counts from 1. A repeated answer to the same query
  ended in a semaphore overflow, that is a safeguard in the network task.
- 2026-11-12  mdns: a burst of queries for the board's name (more than 8 before
  the task ran) ended in a semaphore overflow, a safeguard in the network task.
  A question whose name is compressed (the A question after an AAAA one) is
  now recognised.
- 2026-11-12  net: 16-bit offsets in the DHCP, DNS and mDNS parsers wrapped on
  datagrams above 65 KB (options or records near the end): read past the
  datagram, or parsing resumed inside a record.
- 2026-11-12  net: while the board has no address (0.0.0.0, before the DHCP
  lease) it no longer answers ARP requests for 0.0.0.0 - an address probe of
  another host (RFC 5227) got a reply claiming 0.0.0.0 at the board's MAC.
- 2026-11-12  net: an IPv4 datagram is accepted only if addressed to the board,
  to a broadcast (limited or of the local subnet) or to a multicast group
  (RFC 1122 3.2.1.3). One that reached the board's MAC for another address was
  delivered to UDP and TCP and its ping answered. Until the board has an
  address (DHCP) everything still passes.
- 2026-11-12  net: datagrams with a broadcast or multicast source (IPv4 or MAC)
  or with the board's own address as source are dropped: a ping from such a
  source was answered to the broadcast address, and the source ended up in the
  ARP cache.
- 2026-11-11  net: an IPv4 packet whose total length is 0xFFF2 or more is now
  dropped. The length check wrapped in 16 bits, and a single crafted ping in
  a short frame made the echo reply copy 64 KB over a network buffer.
- 2026-11-11  http: the request body is the Content-Length bytes after the
  blank line (it was taken from the end of the buffer), and a request whose
  body did not arrive in full no longer reaches the handler.
- 2026-11-11  http: OPTIONS answers were malformed (two bytes lost, headers
  not ended by CRLF CRLF).
- 2026-11-11  http: Content-Length is parsed once and strictly: the name is
  case-insensitive; two headers were summed, `5abc` read as 5 and long
  values wrapped.
- 2026-11-11  modbus: `ca_modbus_write_single_reg()` and
  `ca_modbus_write_multiple_regs()` return false unless the answer echoes
  the request; a wrong echo was reported as success.
- 2026-11-11  modbus: a half-received frame no longer survives a disconnect
  or reconnect (the first answer on the new connection was lost), and a
  second frame in the same TCP segment no longer corrupts the stored answer.
- 2026-11-11  mqtt: an incoming packet larger than `MQTT_RX_BUF_SIZE` is
  skipped whole; its body was parsed as new packets and the following
  messages were lost.
- 2026-11-11  mqtt: `ca_mqtt_disconnect()` during the TCP connect now aborts
  it (the session opened anyway), and after a disconnect followed by a
  reconnect the old connection closing no longer tears down the new session.
  The SUBSCRIBE packet id skips 0 when it wraps.
- 2026-11-11  tls: a record whose legacy version is not 0x03xx is no longer
  rejected (RFC 8446 5.1 says to ignore the field).
- 2026-11-10  stdout: a producer is now slowed down as the queue fills (a few
  ms on the last 5 free slots, up to 1 s for a slot when full) instead of
  dropping a line after 20 ms. A long command output over SSH (`taskall`) to a
  client with a small window no longer loses its last lines. The put/printf
  calls return 0 when the line was dropped. The SSH shell gives a command
  5 s (was 2 s) to run, and as much again to send its output.
- 2026-11-10  TCP: on a callback connection (`on_rx`), a FIN arriving in the
  same segment as the last data was ignored, and the connection closed only
  when the peer retransmitted the FIN, one retransmission timeout later. It
  now closes at once.
- 2026-11-10  SSH: the shell channel now follows the client's flow control. The
  server ignored the window and the maximum packet size the client grants, so
  with a client that grants less (not OpenSSH) it could send more than allowed;
  and when the window ran out the rest of a command's output was dropped. It
  now sends within both limits and waits for the client to reopen the window.
- 2026-11-10  TCP: a FIN was accepted whatever its sequence number. When the
  last data segment of a stream was lost and the FIN behind it arrived, the
  connection closed with that data missing; a FIN with any number closed an
  open connection. A FIN now counts only right after the last byte received;
  otherwise the missing byte is asked for again.
- 2026-11-10  TCP: on a callback connection, a peer FIN arriving while no
  buffer was free for our own FIN left the connection established: it then
  waited for a second FIN until the FIN_WAIT_2 timeout, and data sent after
  the FIN still reached `on_rx`. It now waits in CLOSE_WAIT and closes as soon
  as a buffer is free.
- 2026-11-10  IP reassembly (`EN_IP_DEFRAG`): the reassembled datagram carried
  its identification field byte-swapped.
- 2026-11-10  SSH: a window adjust that took the channel window past 2^32 - 1
  wrapped it to a small value and the shell stalled; the window now saturates
  (RFC 4254). Found while bringing SSH under the PC fuzzer.
- 2026-11-10  Modbus TCP client: `ca_modbus_read_coils()` copied as many bytes
  as the server's byte count said (up to 255) into a buffer sized for the
  coils asked: a faulty or hostile server overwrote the caller's memory. The
  byte count must now match the request and fit in the answer received
  (also for `ca_modbus_read_holding_regs()`), and the answer must carry the
  function code of the request.
- 2026-11-10  TFTP: a DATA packet shorter than 4 bytes handed `on_block()`
  about 64 KB read past the packet (into flash, for the boot image update), and
  a DATA longer than 512 bytes was passed whole. Both are now dropped, as is
  an ACK shorter than 4 bytes in `ca_tftp_put()`.
- 2026-11-10  HTTP server: a request ending inside a `Content-Length:` header,
  with no line end after it, made the parser read past the request.
- 2026-11-10  IP reassembly (`EN_IP_DEFRAG`): a duplicated fragment was counted
  twice, so a datagram could be delivered with a hole of zeros, and an
  overlapping one could keep a session from ever completing. Completion now
  tracks which 8-byte blocks arrived; fragments that contradict the known end,
  or are not a multiple of 8 bytes before the last, are dropped. A session
  started before the 32-bit millisecond tick wrapped (49.7 days) now expires.
- 2026-11-10  TCP: a single SYN with crafted options (a NOP followed by an
  option of length 255) sent to a listening port hung the network task for
  good. The MSS option parser is rewritten and shared
  (`ca_net_proto_tcp_peer_mss`), and a peer MSS below 64 (`TCP_MSS_MIN`) is
  raised to it instead of being taken as is (an MSS of 0 sent nothing).
- 2026-11-10  DHCP client: an offer ending with an empty message-type option
  (length 0) made the parser read one byte past the datagram. Found by the new
  PC fuzzer (`caos/net/test/fuzz`).
- 2026-11-10  net: an ARP frame adds a cache entry only when it is addressed to
  the board; other frames refresh a sender already known. Broadcasts between
  other hosts no longer evict the gateway, and DHCP probes (sender 0.0.0.0)
  no longer create a `0.0.0.0` entry.
- 2026-11-08  TCP: after a loss of a whole window (cable pulled, wrong MAC) a
  connection could stall until it was reset: when the driver's transmit ring was
  full, the refused segments still used up a retry and the one the peer was
  waiting for was never sent again. They are now retried at the next timer tick;
  a driver that keeps refusing for a second still ends in the normal abort
  (`tcp refuse on|off` in the CLI simulates it).
- 2026-11-08  iperf CLI: `iperf client <ip> <port> <dur_s>` ignored port and
  duration, always using the defaults; both forms now work, plain numbers and
  `port=N dur_s=N`. The client also ended its run at the
  first stall longer than 200 ms: it now waits while the connection is alive.
- 2026-11-08  net: hosts outside the local subnet were unreachable unless
  `EN_ROUTE_SERVICE` was on (3 projects out of 26): ARP was sent for the
  destination itself. The default gateway is now part of the base IP layer:
  from the DHCP router option, from the new `ca_net_cfg_t.gw`, or the .1 of
  the subnet when not set (a static IP with no mask is taken as /24).
  DNS, NTP and syslog re-resolve their server MAC when the IP, mask or
  gateway changes, instead of keeping the MAC of the old gateway.
- 2026-11-08  ex33, ex67, ex83: the ARP task was never started, so the board
  could not reach a host that had not talked to it first.
- 2026-11-05  USB (H563): a CDC write or an NCM NTB of an exact multiple of 64
  bytes (above 64) was followed by two zero-length packets instead of one:
  the driver added its own on top of the class's. The driver no longer does.
- 2026-11-05  USB-NCM: a frame whose NTB was an exact multiple of 64 bytes
  stayed in the PC until the next frame arrived (no zero-length packet closed
  the transfer). The class now sends one, as CDC does.
- 2026-11-03  PWR: `pwr stopms` and `ca_pwr_enter(..., CA_PWR_WAKE_LPTIM, ms)`
  slept up to ~6 % longer or shorter than asked (LPTIM1 counted the LSI as an
  exact 32 kHz). LPTIM1 now runs on the LSE crystal when the RTC uses it, and
  otherwise on the LSI at the frequency measured at boot.
- 2026-11-03  TCP: the `stale handles refused` count of the `tcp` CLI could
  miss an event when two tasks used an expired handle at the same moment.
- 2026-11-03  Net: the frame counters of `ip if` / `ca_net_stats()` (tx frames,
  tx failed, rx dropped for an empty pool) could lose counts when two tasks
  transmitted at once, or a task and the USB interrupt.
- 2026-11-02  ex2: `same` and `multi` keep the stack of one CLI command free.
  On a small PSP arena (H7S3, 48 KB) the workers took all of it, and every
  following command printed a SAFEGUARD line before running.
- 2026-11-02  ADC: a project with `EN_ADC` on and `EN_DAC` off did not build
  (`ca_adc_hw_stop` / `ca_adc_hw_stop_isr` were declared under `EN_DAC`).
- 2026-11-02  ADC (H7S): the channel-to-pin map was the F4/H5 one and set the
  wrong pin in analog mode for channels 2-9, 11, 14 and 15; it now follows
  the STM32H7S3 pinout (ADC1 and ADC2 differ on channels 2, 6 and 18).
- 2026-11-02  I2C / UART (H7S): I2C4 and USART6 do not exist on this family;
  `num` 4 used to configure pins with no I2C4 function and then drive I2C1.
  Both are now refused.
- 2026-11-02  USB (H7S): the OTG_FS port (`usb_num` 1) set PA11/PA12, the
  H753 pins; on this part it is PM12/PM11. Not routed on the NUCLEO-H7S3L8.
- 2026-11-02  NUCLEO-H7S3L8 trampoline 1.3 (workspace/app_5): with tamper 1
  left armed (it survives a reset) every boot stopped in the monitor, as if B2
  were held. B2 is now ignored while PC13 is a tamper input.
- 2026-11-02  SPI (H7S): the alternate SPI3 map set MOSI on PB5 with AF6,
  which is PSSI_D10 on this part; SPI3_MOSI there is AF7.
- 2026-11-02  SDIO (H7S): the SDMMC1 pins used the H753 alternate function
  (AF12 on all six); on this part CK, CMD, D0, D1 and D3 are AF11, so no card
  ever answered.
- 2026-11-02  SPI (H7S): SPI1/2/3 had no kernel clock (reset source PLL1 Q,
  which is off), so nothing was shifted: DMA transfers never completed and a
  slave never received. They now run on per_ck (HSI 64 MHz).
- 2026-11-02  SAI (H7S): SAI1 now uses the pins the NUCLEO-H7S3L8 brings to
  Zio CN9 (MCLK_A PG7, FS_A PE4, SCK_A PE5, SD_A PC1, SD_B PF6). It used the
  H753 pins, and SD_A/SD_B (PE6/PE3) are not on CN9-20/22 on this board.
- 2026-11-02  USB-NCM: frames sent by two tasks at the same time were built
  into the same transfer buffer and one went out corrupted (an occasional
  lost reply, ex78). Transmission is now serialized, as on Ethernet.
- 2026-11-02  GPIO (H7S): ports O and P read and drove nothing after a
  power-on until the QSPI driver had run; their I/O supply (XSPIM port 1) is
  now switched on when the port is initialized.
- 2026-11-02  Ethernet: frames sent by two tasks at the same time (TCP from
  the application, ARP or DHCP from the net task) could corrupt the transmit
  ring; the board then stopped sending for good until a reset. Transmission is
  now serialized.
- 2026-11-02  Flash CLI (H7S): `flash test`, `erase` and `info` refused every
  internal sector, taking the image running from the external XSPI flash for
  code in the internal one. Sector 0 (the trampoline) is still refused.
- 2026-11-02  Temperature (H7S): the digital sensor (DTS) read about 1000 C
  too high; its frequency was measured against SYSCLK instead of APB4.
- 2026-11-02  JPEG (H7S): only the first decode after start-up worked, every
  later one failed with `CA_JPEG_ERR_HDR`. The header-parsed flag was never
  cleared (wrong bit in the ST device header).
- 2026-11-02  Backup registers (H7S): `ca_bkp_read()` read 0 until the first
  `ca_bkp_write()` after a reset, so values kept across the reset (the ex10
  early-warning mark, the bootloader state) were seen as empty.
- 2026-11-02  Kernel: task stacks freed side by side merge again, and a free
  block larger than needed is split. Before, a freed stack was reused only by
  a task asking for exactly the same size, so after many short-lived tasks a
  bigger task could fail to start with plenty of stack memory free. A failed
  creation now reports the free bytes and the largest block; new
  `ca_ccm_get_largest_free()` and `ca_ccm_get_segments()`.
- 2026-11-01  Ethernet (H7, H5, H7S): transmit buffers go back to the pool as
  soon as the DMA has sent them. Before, each TX slot kept its last buffer
  until the slot was reused, up to 16 buffers out of the pool for good.
- 2026-11-01  TCP: a connection in FIN_WAIT_2 whose peer never sends an
  acceptable FIN is closed after `TCP_CLOSE_WAIT_MS`; before, it held its slot
  for ever.
- 2026-11-01  TCP: a SYN arriving with the connection table full recycles the
  oldest TIME_WAIT slot instead of being dropped, so bursts of short requests
  (HTTP) no longer find the server deaf.
- 2026-11-01  NUCLEO-H7S3L8: Ethernet works. The AHB SRAM holding the ETH DMA
  descriptors was left unclocked after reset, so the driver saw garbage RX
  descriptors and looped on fake frames.
- 2026-11-01  Examples: ex91 `fpu_share` now really runs floating point in its
  tasks. The compiler had folded the FP series into a constant, so the case
  passed without testing the FPU context switch.
- 2026-10-30  HTTPS: the service task gets at least 8 KB of stack. On F4 parts
  `STACK_LARGE` is 4 KB and the TLS handshake (X25519 + ECDSA P-256)
  overflowed it: the task was restarted and the client saw the connection
  hang after the ServerHello.
- 2026-10-30  NUCLEO-F439ZI: network pool raised from 16 to 32 buffers. With 16
  the pool ran dry under SSH bursts (long `taskall` output came back empty).
- 2026-10-30  Ethernet: `ca_eth_init()` re-enables PHY auto-negotiation when
  the PHY comes out of reset with it disabled (strap pins latched wrong). About
  one boot in ten the link came up forced, the PC fell back to half duplex and
  DHCP got no lease.
- 2026-10-30  Examples: the USB device examples (ex50-57, ex69-83, ex90) and the
  jumper examples with per-board pins (ex21, 22, 24, 25, 29, 64, 65, 93) now
  build for the NUCLEO-F439ZI; before, its MCU was missing from their lists and
  they compiled as "not supported".
- 2026-10-30  Examples: ex68 on the F439 provoked its "read past the end of
  flash" faults at 0x08100000, which is flash bank 2 on a 2 MB part: no fault
  was raised. Now 0x08200000.
- 2026-10-30  Examples: ex100 (LSE input capture) now runs on F407/F439 via
  TIM5 CH4, as on the F446; before it stopped with "no LSE route known".
- 2026-10-30  Examples: ex105 (DMA2D) on a DMA2D without alpha inversion and
  red/blue swap (F439) checks that the driver refuses those flags, instead of
  counting the refusal as a failure.
- 2026-10-29  Crypto: on the STM32F439 AES-GCM now runs on the CRYP
  peripheral. Before, the build saw the CRYP in the device header, left the
  software AES out and called an empty driver, so SSH and HTTPS closed at the
  first encrypted packet.
- 2026-10-29  SSH: the packet staging buffers moved from the stack into the
  session. Long command output over SSH (`taskall`, `config`) overflowed the
  2 KB stack of the stdout thread on F4 boards ("STACK FLOOR CORRUPTED: stdout").
- 2026-10-28  Kernel: an IMPRECISE BusFault (a stray store still in the write
  buffer, M7) that surfaced during the context switch halted the whole system
  ("No task to blame"). The switch now completes the pending store first and
  charges the fault to the task that made it, with its fault policy (removed
  or restarted). Still a halt if it surfaces while another ISR is running.
- 2026-10-28  Kernel: a task that ends while owning mutexes (restarted or
  removed by a fault, or removed with `ca_os_remove_job_handle()`) now hands
  each mutex to its first waiter, or frees it. Before, a restarted owner hit
  "same task locked twice" at its first relock and restarted for ever, and the
  waiters of a removed owner waited for ever. The data the mutex protected is
  left as the task left it: the next owner can tell with the new
  `ca_mutex_owner_died()` (true until its `ca_mutex_unlock()`) and check or
  rebuild the data.
- 2026-10-28  Kernel: a mutex taken with `ca_mutex_trylock()` (or
  `ca_mutex_timedlock()` with timeout 0) was not tracked as held by the task,
  so it stayed locked if the task ended holding it.
- 2026-10-28  Kernel: a task that ends inside a read or write section of a
  `ca_rwlock_t` (restarted or removed) now releases it as its unlocks would.
  Before, a dead writer blocked every reader and writer for ever, and a dead
  reader blocked every writer. After a dead writer, the next writer can tell
  with the new `ca_rwlock_owner_died()`.

### Added
- 2026-11-03  FREE edition: two more boards, NUCLEO-F439ZI and NUCLEO-H7S3L8
  (libraries, linker scripts, Renode platforms, `./bootstrap -m F439|H773`). On
  the H7S3 the firmware runs from the XSPI2 flash behind the app_5 trampoline.
- 2026-11-02  app_3 (NCM <-> ETH router) runs on the NUCLEO-H7S3L8 too: ETH pins
  and USB port for the H7S3; checked against a NUCLEO-H753ZI (encrypted and
  clear traffic, rekey, peer reset).
- 2026-11-02  NUCLEO-H7S3L8: USB device at full speed on the Type-C connector
  (OTG_HS with the integrated PHY, `usb_num` 0). All USB examples (CDC, MSC,
  DFU, NCM, ETH-NCM relay) now run on this board.
- 2026-11-01  NUCLEO-H7S3L8 network pool raised from 32 to 36 buffers (all of
  AXI SRAM4).
- 2026-11-01  Kernel: a task that cannot be created (no stack block of the
  requested size, or task table full) is reported on the console with its
  name and the creating task; before, `ca_os_job()` returned
  `CA_TASK_HANDLE_NONE` without a word.
- 2026-11-01  Atomics: `ca_ldrex()`, `ca_strex()`, `ca_clrex()` in `atomic.h`,
  the LDREX/STREX exclusive-access primitives under the `ca_` namespace.
- 2026-11-01  New board NUCLEO-H7S3L8 (`CURR_MCU` H773, new family `hw/h7rsxx`):
  images run in place from the Octo-SPI flash on XSPI2 (0x70000000), started by
  the trampoline `workspace/app_5` in the 64 KB internal flash (with a serial
  monitor: `mem`, `hexdump`, `id`, `go`), flash read in octal DTR at 75 MHz with the
  `XSPI2_HSLV` option byte (37.5 MHz without it). `prog.sh` writes XSPI2 images through
  ST's external loader. All 107 examples build; 71 boot clean on the board,
  the others report "unsupported" (USB not ported yet; no DAC, OPAMP, COMP,
  FMAC on this part). ex92 (QSPI flash on the Zio) and ex100 pass.
- 2026-10-28  Watchdog last gasp: `ca_wdt_last_gasp_arm()` makes the early
  warning record the task that was running in backup registers 12-15, and
  `ca_wdt_last_gasp()` returns its name at the next boot - who starved the
  watchdog. Needs the early warning (WWDG, or IWDG on H5/U5) and 16 backup
  registers (not on F1).

### Changed
- 2026-11-03  JPEG: about 2.7x faster end to end on the H753 (64x48 4:2:0:
  413 -> 153 cycles per pixel). The codec FIFOs move a whole threshold block
  per status check, and `ca_jpeg_rgb_to_mcu` / `ca_jpeg_mcu_to_rgb` work MCU
  by MCU; their output is bit-identical to before.
- 2026-11-02  ex62: declared unsupported on the NUCLEO-H7S3L8 - LPUART1 has no
  pin a USB-serial adapter can reach on that board (PB6 is the Ethernet clock,
  PA9 only on the unsoldered morpho header).
- 2026-11-02  ex92 (NUCLEO-H7S3L8 only): `e92 xspeed` measures how fast the
  code flash on XSPI2 can be read, without writing it (100 and 150 MHz hold
  once the clocks are up; the default stays 75 MHz).
- 2026-11-02  ex65: `s65 wire` also lists the two extra ground jumpers
  (CN8-11, CN10-5) a master/slave pair needs, and the power pins to avoid.
- 2026-11-02  Examples: ex2 is now a scheduler demo driven from the CLI.
  `same <lvl>` and `multi` start as many tasks as fit, at one level or over
  the 7 levels, `get` shows how the CPU was shared (round robin at one level,
  CFS weights across levels, equal shares below 100 % load), `stop` ends them.
  It no longer starts the two printing tasks.
- 2026-11-01  Examples: ex91 sets its shared completion flags atomically
  (LDREX/STREX) instead of with a plain `|=`.
- 2026-11-01  GPIO: `ca_gpio_port_t` goes up to port P (the H7Rx/7Sx has ports
  M, N, O, P); the CLI `gpio` command accepts A..P.
- 2026-11-01  Examples: ex27 and ex31 say "no DAC on this part" instead of
  starting on a part without a DAC; ex30 takes the button pull and edge per
  board.
- 2026-10-29  Boards: STM32F429 support replaced by the NUCLEO-F439ZI
  (`CURR_MCU "F439"`, `MCU_ID 439`, `hw/f4xx/f439`). The board map is fixed
  against UM1974 and the datasheet: LD2 is PB7, 192 KB SRAM + 64 KB CCM and
  2 MB of flash in `memmap` and in the linker script.
- 2026-10-28  iperf server (`ca_iperf_server()`, CLI `iperf server`) serves
  every connection the peer opens (`iperf -P N`, up to 8) instead of only the
  first: the others completed the handshake and were never read. The result
  gains `streams` and the console line ends with `N stream(s)`; the duration
  runs from the first to the last byte received.
- 2026-10-28  Recursive mutex (`ca_recursive_mutex_t`) is now a mutex plus a
  reentrancy count: it gains priority inheritance (the owner runs at the
  priority of its highest waiter) and the hand-off when the owner ends, with
  `ca_rmutex_owner_died()`. The count of a dead owner never reaches the next
  one. The API is unchanged; the structure is different, so code must use the
  API and not its fields.
- 2026-10-28  rwlock: a task that already holds the read side and calls
  `ca_rwlock_read_lock()` again now stops on a safeguard. It used to pass, and
  deadlocked as soon as a writer was queued between the two reads.
- 2026-10-28  CLI `top`: the CPU share is measured over a window that `top`
  times itself (default 1 s, `-t <ms>`), against wall time from the tick, and
  the last line gives the scheduler and the free time (WFI): the column now
  adds up to 100%. Before, on parts where the cycle counter stops in WFI
  (F767, H753) the share was of the awake time only (a sleeping task could
  show 30%), and two calls more than ~9-20 s apart gave meaningless values.
  Options combine in any order (`top -m -t 3000`). Column `Clk` renamed
  `Peak`: it is the longest single run of the task, not a total.

## free-v0.1.0 - 2026-10-27
- First public FREE release.
