# Changelog

Fixes and changes to the caOS sources that a user can notice, newest first.
One entry per fix: date, area, what changed for the user. The details are in
the git history; documentation-only changes are not listed.

Entries about what the FREE edition leaves off (CAN, MicroPython) start
with `<!-- paid -->`: they are removed from the FREE package.

## Unreleased

### Changed
- 2026-11-27  USB (H563): the endpoint queues of the USB_DRD driver are sized
  by the classes that are built and bound at ca_usb_ep_open, as on the other
  families since 26/11: an NCM or CDC example gets ~54 KB of heap back (ex157
  244 -> 298 KB). An endpoint beyond the budget is refused by ca_usb_ep_open.
- 2026-11-26  USB: the endpoint queues are sized by the classes that are built
  (CDC, MSC, NCM, CUSTOM), not for every endpoint the controller has, and a
  queue is given to an endpoint when it is opened. RAM per USB example drops
  a lot (CDC alone: H753/H773 104 -> 26 KB, F767 43.5 -> 13 KB, F446 26 -> 13
  KB). With FS and HS both on, queue depth no longer halves: each core has its
  own pool. An application that opens bulk endpoints itself (ca_usb_ep_open)
  or uses the CUSTOM class sets USB_CUSTOM_EPS to the number of IN+OUT pairs;
  an endpoint beyond the budget is refused by ca_usb_ep_open.
- 2026-11-26  examples: ex142 (LVGL over VNC on USB-NCM) runs on the F767 at
  320x200; it was disabled for lack of RAM, which the USB queues now give back.
- 2026-11-26  FREE edition: same drivers as the full edition. Tamper, FMAC,
  HTTPS/TLS 1.3, ECDSA P-256, VNC over TLS, ChaCha20-Poly1305 and BLAKE2s
  are now in the FREE libraries; the only limits left are the fixed
  os_config.h and half the heap. CAN, USB HS and MicroPython stay off
  (LIMITS.md says why).
- 2026-11-26  USB: the serial number string is `<MCU_ID>-<USB_STR_SERIAL>`
  (e.g. `0753-0001` on H753), no longer `0001` on every board: two caOS
  devices on one PC now get distinct /dev/serial/by-id names and udev links.
- 2026-11-24  examples: the network examples' MAC depends on the board too:
  `02:<role>:<MCU_ID>:<example>` in decimal digits (ex149 on H753 =
  `02:CA:07:53:01:49`), from one macro, `CA_EXAMPLE_MAC(n)` in net_cfg.h.
  Two boards running the same example on one switch no longer share a MAC
  and a lease. Role CA = the board, CB = a relay's NCM side, CC = the PC
  side of a USB-NCM link (the PC's interface is now `enx02cc...`).
  Examples with a static IP keep it.
- 2026-11-24  CLI: `config` lists every os_config.h symbol the project
  defines (it showed 169 of 261), under its real name (`EN_TCP_SERVICE`,
  not `tcp`), then the board's values from objmap.h; long texts are cut.
  `ca_caos_config_hash()` covers the same rows. The command's table and the
  hash are built only with `EN_CLI_CONFIG`: the other firmwares no longer
  carry them. `ca_caos_config_status` and `ca_caos_get_config()` are gone
  (a partial copy, 219 of 261 symbols, read by nobody): the configuration
  is read with `ca_caos_config_rows()`, two builds are compared with a diff
  of their `config` output.
- 2026-11-23  build: caOS links without newlib (`-nostdlib`, libgcc only for
  64-bit division). `memcpy`, `memset`, `memmove`, `memcmp`, `strlen`,
  `strcmp`, `strncmp`, `strchr` - the calls GCC emits by itself and the ones
  LVGL, FatFs and TinyUSB make - come from caOS (`libc_shim.c`, on the `ca_*`
  functions of memaux), declared by caOS's own `string.h`. Any other C
  library call is now a link error instead of newlib code in the image. The
  newlib stubs `sysmem.c` and `syscalls.c` are gone.
- 2026-11-22  kernel: the idle, stdout and cli tasks take STACK_SMALL (cli
  keeps STACK_MEDIUM on F103), sized on their measured peaks: 6 KB more free
  stack arena on F767, H773, H7, U5 and H5 (F4 unchanged). The banner task is
  created last, just before the scheduler starts: it no longer leaves a 2 KB
  hole in the arena when it ends.

### Fixed
- 2026-11-27  drivers: ca_st7735_init() can be called again on the same SPI
  (a retry, a new MicroPython VM); it failed because the SPI DMA was already
  bound. The binding keeps the first call's clock and CS; another SPI is refused.
- 2026-11-26  FREE edition: `memlist` froze the board (console included).
  Printing a block name checked the pointer against the FREE heap cap, which
  took the heap lock `memlist` already held with interrupts off. The heap
  start/end getters no longer take the lock (the values are fixed at init).
- 2026-11-26  net: with IP fragmentation on, a task sending while the
  fragmentation queue is full now waits for a slot in order (up to 20 ms)
  instead of being refused at once. Several tasks bursting large datagrams
  were served unevenly: one could be refused for 200 ms in a row while the
  others went through (ex136 on H563/F767). Now the longest wait is ~5 ms.
- 2026-11-26  examples: ex141 on F767/F746 draws a 320x200 screen (was
  320x240): with the software-AES key store the VNC service found no heap
  for its buffers and did not start.
- 2026-11-26  examples: ex11 `dts alarm` ran the DTS/ADC comparison instead
  of the alarm test since the CLI command tables (22/11): the handler still
  skipped the command word, which no longer arrives with the line.
- 2026-11-26  crypto: the key store takes 2 KB again on parts with an AES
  peripheral (F439, H753, H773); 4 KB only where AES runs in software. The
  doubling of 21/11 cost ~4 KB of RAM there (array + MPU alignment) and VNC
  logins on ex139/F439 were refused for lack of heap.
- 2026-11-24  examples: ex1 waits one second between lines again
  (`ca_os_delay_ms`): it runs without the scheduler, where `ca_os_sleep()`
  returns at once, so the Hello line was printed in a burst.
- 2026-11-23  ADC (F4, F7): ADC3 channels IN4-IN9, IN14, IN15 set ADC1's
  pins analog instead of their own (PF3-PF10); `ca_adc_stop()` always
  stopped ADC1.
- 2026-11-23  CLI: `ca_cli_exec_capture()` could return, at the start of the
  captured text, output another task had queued just before (the
  MicroPython REPL's echo, seen in `caos.cli()` on F767): the stdout queue
  is now drained before the capture starts.
- 2026-11-23  ADC (H5): the same F4 channel map in `ca_adc_pin_init()` (on
  the H563, 9 channels of 13 left their pin digital), now the datasheet's.
  `ca_adc_init()` refuses ADC2: the H5 driver drives ADC1 only, and ADC2 was
  served by ADC1 without a word.
- 2026-11-23  ADC (H7): `ca_adc_pin_init()` set the pin of the F4 channel
  map, so most channels left their real pin digital (channel 4 is PC4, PA4
  is channel 18 on the H7); now the map is per converter, from the
  datasheet. `ca_adc_stop()` always stopped ADC1, whatever the number given.
  ADC3, which the temperature sensor holds from boot, could not be used by
  the ADC driver (`ca_adc_init(3)` failed): the driver now shares it - no
  recalibration of an enabled ADC, only its own channels' settings - and a
  temperature reading puts the resolution back as it found it. Writes to the
  ADC control register keep the linearity-calibration bits: clearing them
  hung the conversion.
- 2026-11-23  FAT: `EN_FAT` without `EN_RTC` did not compile (`get_fattime()`
  used the RTC driver even with `FF_FS_NORTC 1`, where FatFS never calls it).
- 2026-11-22  LVGL/VNC: the gui task and the VNC task with TLS take the
  first stack size that reaches their 8 KB floor; on F767 and H773 they
  skipped the 8 KB step and took 10 KB each. On H773 ex141 the CLI commands
  were refused (`busy: no stack`) for lack of arena; now they run.

### Added
- 2026-11-27  FAT: a second FatFS volume, "1:", on a RAM buffer the application
  gives (ca_fat_ram_disk), with mkfs/mount/umount/statvfs; the handle calls of
  fat_service pick the volume from the path. ca_fat_ram_read/_write fit the
  USB MSC callbacks, so the same memory can be a USB stick.
- 2026-11-27  USB MSC: the medium can be made absent and present again
  (ca_usb_msc_set_ready); the host is told the medium changed and rereads it;
  an eject from the host (START STOP UNIT) is seen (ca_usb_msc_ready/_ejects).
- 2026-11-27  LVGL: ca_lvgl_call() wakes the GUI task at once (it waited up
  to 10 ms for its next loop); new ca_lvgl_call_sync() runs a function in the
  GUI task and waits for it.
- 2026-11-26  drivers: st7735.h, ST7735 TFT controller (1.8" 128x160 RGB565
  modules) over SPI with DMA: init, fill, blit from RGB565 memory, raw
  window writes, PWM or GPIO backlight; module variant (offsets, RGB/BGR,
  inversion, rotation) in the config and at run time. Example 152 (U575):
  test pattern to recognise the variant, `lcd` CLI, 28 fps full frame at 10 MHz.
- 2026-11-26  drivers: ca_st7735_show_fb() shows a caOS framebuffer on the
  ST7735, sending only the changed 16x16 tiles; ca_st7735_blit() takes a
  source stride. Example 153: LVGL on the 1.8" TFT (U575, landscape 160x128).
- 2026-11-26  examples: 152 and 153 run on every bench Nucleo (153 not on the
  F446: RAM); `lcd wire` / `e153 wire` print the TFT wiring of the board they
  run on, from the same table the driver is configured with (lcd_pins.h).
- 2026-11-26  crypto: ChaCha20, HChaCha20, Poly1305, ChaCha20-Poly1305
  (96-bit nonce of RFC 8439 and the 64-bit nonce of WireGuard) and
  XChaCha20-Poly1305 (`EN_CHACHA20_POLY1305`); BLAKE2s (RFC 7693) and
  HMAC-BLAKE2s (`EN_BLAKE2S`). Software, the same on every family; the
  working state is the caller's context. Known-answer tests in example 66,
  CLI `chacha test|speed` and `blake2s test|speed`. MicroPython:
  `caos.crypto.chacha20poly1305_encrypt/_decrypt` (nonce as int, 12 or 24
  bytes), `blake2s`, `hmac_blake2s`, keys by keystore index too.
- 2026-11-23  fat_service: handle API for programs, with no printing -
  `ca_fat_fopen/fread/fwrite/fseek/ftell/fsize/fsync/fclose`, directories
  (`ca_fat_dopen/dread/dclose`), `ca_fat_pstat`, `ca_fat_pmkdir`,
  `ca_fat_punlink`, `ca_fat_prename`, `ca_fat_statvfs`. The FIL/DIR objects
  are on the caller's task heap, never on a stack.
- 2026-11-23  udp_client: `ca_udp_client_sendto()` (any destination) and
  `ca_udp_client_recvfrom()` (gives the sender); a client opened with
  `server_ip` 0 is bound to its local port only.
- 2026-11-23  tcp_service: `ca_tcp_peer()` gives the remote address and port
  of a connection (e.g. after `ca_tcp_accept()`).
- 2026-11-23  memaux: `ca_memmove()` (overlapping areas) and `ca_strchr()`.
- 2026-11-23  console: a task can take the console from the CLI and read it a
  character at a time (`ca_console_take`, `ca_console_getc`,
  `ca_console_give`), with an optional ISR filter for Ctrl-C. The RX ring is
  `CONSOLE_RX_BUF_SIZE` bytes (default 256). A console left to a task that
  ended is given back to the CLI by the next byte. Example 144.
- 2026-11-22  CLI: command tables (`ca_cli_register_table`, `ca_cli_find`);
  the built-in commands are the first table, `help` prints one line per
  command (now including `help` itself). Temporary `find_cmd <line>` runs a
  line through the tables only (exact word match), for the migration.
  Built-ins are matched by the exact word (`topx` is no longer `top`);
  `callto` is the first module on a table.
- 2026-11-22  CLI: every caOS module command (drivers, net, crypto) is on a
  command table. A missing or unknown subcommand prints the command's help
  line (it used to print a per-module message, or `huh!`). Each command has
  one help line, wrapped at its ` | ` separators past 100 characters (long
  lines used to arrive cut, `~OVF`). `ed25519`, `p256` and `tftp` run in a
  task with their own stack; with every command slot busy they answer
  `busy: no stack for '<cmd>', retry` instead of running on the cli stack.
- 2026-11-22  CLI: the console never waits on a command: only near-instant
  commands (< 50 ms) run inside the cli task, the others (network waits,
  self-tests, flash/SD, `top`) in a task named after the command. The same
  command typed again while it runs answers `busy: '<cmd>' already running`.
  After every command the cli wipes its copies of the line (passwords).
- 2026-11-22  CLI: `ca_cli_capture_next(fn)` hands the next whole line to
  `fn` (prompts: passwords, menus). Every example and workspace app is on a
  command table, one entry per command word.
- 2026-11-20  Workspace: app_6 "Comet Guard", a fixed shooter played over VNC
  (TLS + login) on the H753: double-buffered 320x240 framebuffer at 30 fps,
  status line drawn by LVGL on a framebuffer of its own, keys from the viewer
  or from the console (`left`, `right`, `fire`, `shield`, `credit`), `app6 stats`.
- 2026-11-20  Examples: ex143 splits AES-GCM into its phases (key setup, fixed
  cost, AES rounds, GHASH, whole GCM) and times each with the DWT cycle counter,
  16 B to 4 KB, AES-128 and AES-256 (`e143 run`, `e143 test` with a verdict).
  H563 software: GHASH is 84% of the GCM time.
- 2026-11-19  fb: double buffer. `ca_fb_create(..., 2)` falls back to one buffer
  when two do not fit; the application draws in A, `ca_fb_present()` (called by
  the LVGL port at each frame end) copies A to B at most every `FB_COPY_MS`,
  and the reader (VNC) reads B from `ca_fb_info().base`: never half a frame.
  Copy by DMA2D (whole screen, 0.5 ms on H753) or by CPU (changed tiles).
- 2026-11-19  Examples: ex142, example 141 (LVGL over VNC, TLS + login) over
  USB-NCM Full Speed instead of Ethernet. H753: interface in use ~0.3 MB/s as
  on Ethernet, a full 480x272 redraw 0.26 s (16 bpp) / 0.5 s (32 bpp).
- 2026-11-19  fb: dirty map. Every drawing call marks the 16x16 tiles it touches;
  `ca_fb_take_dirty()` hands them to one reader (the VNC service),
  `ca_fb_mark_dirty()` marks pixels written another way (DMA, a cast of base).
- 2026-11-19  VNC: login for TLS sessions (VeNCrypt X509Plain, user and password
  inside TLS, TigerVNC's default). The board keeps only a PBKDF2-SHA256 hash
  (`ca_vnc_cfg_t.cred`, made by `tools/gen_vnc_cred.py`); after a wrong
  password the next client waits 1 s, doubling up to 30 s. `vnc info` counts
  logins. Examples 139/141: user `caos`, password `caos-vnc-demo`.
- 2026-11-19  Config: `TCP_REAP_MS` and `UDP_REAP_MS` (default 1000 ms) set
  how often the TCP and UDP resources of ended tasks are closed.
- 2026-11-18  FREE edition: framebuffer (`fb`), the VNC server (plain RFB) and
  LVGL are included; the heap cap of F439 and H773 is half of their heap like
  the other boards (it was 64 KB). Graphics examples sized for the full heap
  may not fit and say so at boot (LIMITS.md).
- 2026-11-18  GFX: `ca_lvgl_stop()` ends LVGL and gives back all of its heap
  (pool, draw buffer, extra heap); `ca_lvgl_start()` can run again. ex141:
  `e141 stop` / `e141 start`.
- 2026-11-18  Examples: ex141 (LVGL over VNC) runs on parts without a DMA2D
  too, with LVGL's software renderer: H563 (plain RFB, 320x240).
- 2026-11-18  GFX: LVGL text goes to the DMA2D too: each glyph is blended with
  the text colour, polled (5-7% faster per text frame on F439 F767 H753 U575).
  On by default; `ca_lvgl_dma2d_text(min_px)` sets a size threshold or
  `CA_LVGL_TEXT_OFF`. ex140: `e140 text` measures it.
- 2026-11-17  GFX: `ca_lvgl_start()` runs LVGL in its own task on an RGB565
  framebuffer (partial buffer flushed with `ca_fb_blit`, pointer and keypad
  input, optional software cursor, DMA2D unit). Other tasks feed input with
  `ca_lvgl_input_pointer/key()` and run code on LVGL objects with
  `ca_lvgl_call()`. ex141: an LVGL screen (buttons, text area, on-screen
  keyboard) served over VNC, driven by the viewer's mouse and keyboard.
- 2026-11-17  Drivers: `fb` gets the RGB565 format (`CA_FB_FMT_RGB565`, 2 bytes
  per pixel) and `ca_fb_blit()` (copy a block of pixels, clipped, under the
  owner check). Drawing colours are now `u32`: an index for L8, an RGB565
  word for RGB565 (callers passing an index are unchanged).
- 2026-11-17  NET: VNC serves RGB565 framebuffers too; `ca_vnc_proto_update()`
  takes the source format (`CA_VNC_PIX_L8` / `CA_VNC_PIX_RGB565`). ex139:
  `e139 fmt l8|rgb565`.
- 2026-11-17  GFX: LVGL v9.6 (external/lvgl, MIT) built and linked as `liblvgl.a`;
  configuration in `external/caos_lvgl/lv_conf.h` (RGB565). On parts with a
  DMA2D (F439, F767, H753, H773, U575) `ca_lvgl_dma2d_init()` adds a drawing
  unit on `ca_dma2d`: fills and plain bitmaps go to the DMA2D, the rest stays
  in software (about -23% per frame). ex140 compares the two renderers.
- 2026-11-17  NET: encrypted VNC (`EN_VNC_TLS`): VeNCrypt X509None, the whole
  session over TLS 1.3 (AES-256-GCM-SHA384). `cfg.security` AUTO (default)
  picks TLS where the part has AES in hardware (`ca_aes_is_hw()`, from the
  CMSIS header: H753, H773, F439), plain RFB and forced view only elsewhere;
  TLS and NONE force the choice. ex139 serves its pattern this way
  (`e139 sec`), `vnc_check.py` speaks VeNCrypt.
- 2026-11-17  TLS: TLS_AES_256_GCM_SHA384 (with `EN_SHA512`), preferred when the
  client offers it, TLS_AES_128_GCM_SHA256 otherwise; `https info` shows the
  last negotiated suite. New `ca_sha384*` and `ca_hmac_sha384*`. ex26 gets
  `sha384` (FIPS 180-4 and RFC 4231 vectors); the HTTPS examples (ex45, ex80,
  ex129) turn `EN_SHA512` on.
- 2026-11-17  NET: VNC server (`EN_VNC_SERVICE`, RFB 3.3/3.7/3.8, Raw) showing
  an `fb` framebuffer to one client on port 5900, in the client's pixel format
  (8/16/32 bpp). Only what changed is sent, found by tile hashes: drawing code
  does not report its changes. View only by default; input reaches
  `on_key`/`on_pointer` with its source (VNC client, local hardware or another
  connection through `ca_vnc_inject_key/pointer()`); client input can be
  switched at any time with `ca_vnc_service_set_client_input()`. Destroying the
  service gives back the framebuffer and all its buffers. No security: trusted
  networks only.
- 2026-11-17  Examples: ex26 `aes_bench`, AES-GCM throughput (128 and 256 bit,
  1-16 KB blocks): about 43 MB/s with the H753 CRYP.
- 2026-11-17  Examples: ex139, VNC server on a test pattern;
  `vnc_check.py` is the PC side.
- 2026-11-17  Drivers: framebuffer `fb` (`EN_FB`), an L8 canvas in heap RAM with a
  256-color palette. Handles with generation; the service that shows it takes it
  with `ca_fb_take()` and it dies with that task; drawing (`ca_fb_fill_rect`,
  `ca_fb_set_pixel`, ...) is clipped and refused once the owner is gone.
- 2026-11-17  Examples: ex138, the framebuffer on the CLI (`fb test` self-test),
  sized for each MCU from 200x100 to 640x480. First step of the VNC service.
- 2026-11-17  NET: static path MTU per next hop, for a radio modem/router on
  the LAN: `ip pmtu <mac> <mtu>` (or `ca_net_pmtu_set()`, two entries) cuts
  the frames sent to that MAC to the given MTU (`EN_IP_FRAG`) and lowers the
  TCP MSS of the connections through it, also those already open.
- 2026-11-17  NET: `ip defrag [reset]` in the CLI shows the reassembly counters
  (open sessions, fragments, completed datagrams, timeouts, drops by cause);
  `ca_ip_defrag_stats()` / `ca_ip_defrag_stats_reset()` give them to code.
- 2026-11-17  Examples: ex137, a TCP connection that lives through an MTU drop
  (`ip mtu 512` while it is open, between two echoes or mid transfer);
  `tcp_mtu_drop.py` is the PC side.
- 2026-11-17  Examples: ex136, IP fragmentation on TX under load. Up to four
  board tasks burst UDP datagrams at the PC through `ca_udp_client` while
  `ip mtu` simulates a radio link; `e136 start` runs one pass and prints a
  verdict, `frag_rx_pc.py` checks every datagram on the PC side.
- 2026-11-16  NET: IPv4 fragmentation on transmit for links with a reduced
  MTU (`EN_IP_FRAG`, start it with `ca_ip_frag_service_start()`). Any datagram
  larger than the MTU of the interface it leaves from is cut into fragments by
  a service task; frames that fit go straight out as before. New
  `ca_net_iface_set_mtu()` / `ca_net_iface_mtu()`, and `ip mtu [iface] <N>` in
  the CLI to simulate a radio link on ETH or NCM; `ip stats` shows the
  fragmentation counters. ex48 and ex81 turn it on. A TCP connection already
  open when the MTU drops keeps working: new segments follow the current MTU
  and an old-size retransmission is fragmented rather than dropped (ex137).
- 2026-11-15  USB CDC: pull reception. Set `on_rx_ready` in the CDC config and
  read with `ca_usb_cdc_read()` from a task: unread data stays in the driver
  queue and the host is held off (NAK), so a fast stream is slowed down instead
  of losing bytes. The old `on_rx` callback (bytes delivered in the ISR, no
  back-pressure) still works; ex52 and ex56 now use the pull mode.
- 2026-11-15  USB: `ca_usb_bulk_mps(usb_num)` returns the bulk packet size of
  the negotiated speed (64 at full speed, 512 at high speed). The CDC class uses
  it when its config leaves `mps = 0`; so do the custom, MSC and NCM classes
  (before, `mps = 0` meant 64 in all of them).
- 2026-11-15  USB HS: high-speed device driver for the NUCLEO-H7S3L8
  (OTG_HS with the integrated PHY, 480 Mbit/s, `EN_USB_HS=1`). With `EN_USB_HS`
  the device answers DEVICE_QUALIFIER and builds its config descriptor for the
  negotiated speed. New examples: ex109 (vendor loopback, the HS twin of ex50,
  PID 0x0101) and ex110 (CDC-ACM, the HS twin of ex52, PID 0x0113).
- 2026-11-15  USB HS: more high-speed twins on the NUCLEO-H7S3L8 -
  ex111 (custom class, of ex51), ex112 (MSC, of ex53), ex113 (NCM L2, of ex54),
  ex114 (DFU runtime, of ex57), ex115 (IP stack on NCM, of ex55), ex116 (iperf
  on NCM, of ex79: 35.7 Mbit/s board->PC, 61.4 Mbit/s PC->board, against 6.5
  and 5.6 at full speed).
- 2026-11-16  USB HS: high-speed twins of the remaining NCM and CDC
  examples on the NUCLEO-H7S3L8, ex117-ex135 (PID = FS twin + 0x0100, own MAC
  and static IP): ex117 composite CDC+MSC+NCM (of ex56), ex118 CDC console (of
  ex90), ex119-ex132 network services over NCM (of ex69-ex78, ex80-ex83),
  ex133-ex135 ETH + NCM (of ex94-ex96).

### Changed
- 2026-11-22  CLI: the old command chain is gone: `ca_cli_register()`,
  `ca_cli_node_t`, `ca_cli_set_hook()`/`ca_cli_get_hook()` are removed; use
  `ca_cli_register_table()`. ex58's own SD commands moved from `fat ...` to
  `e58 blob|sdtest|test_all|log`.
- 2026-11-22  Example 26: its own tests are now `crc` and `kstest` (were
  `crc32` and `kstore`), so the driver commands `crc32 calc` and
  `kstore info|list` work again in this example.
- 2026-11-21  Kernel: task stack sizes are fixed values per board, no longer
  ratios that doubled at every class. F4: 2/2/4/6/8 KB (SMALL was 1 KB),
  F767/H773: 2/4/6/8/10 KB, H7/U5/H5: 4/6/8/10/12 KB (LARGE was 16 KB, XLARGE
  and KERNEL 32 KB), F103: 1/2/2/2/2 KB. `STACK_SLICE` in `objmap.h` is the
  step; `config` shows it.
- 2026-11-21  Examples: ex12, ex14 and ex15 print their own commands in `help`
  in the standard `name      : syntax` form, one line per command.
- 2026-11-20  Kernel: the stack alarm threshold (`STACK WARNING`, 75 %) is now
  `STACK_WARN_PERMILLE` in each board's `objmap.h`.
- 2026-11-20  F439: task stack sizes 2/2/4/8 KB (`STACK_SMALL` 1 -> 2 KB): the
  HTTP task ran at 89 % of 1 KB.
- 2026-11-20  crypto: software AES-GCM (parts without an AES unit: F446, F767,
  H563, U575) about 8x faster, results unchanged: GHASH with a 4-bit table,
  AES rounds with the T-table formulation, both tables in RAM (the parts with a
  software build have no data cache there: lookups take constant time).
  H563, 4 KB: GCM-128 191 -> 1789 KB/s, GCM-256 180 -> 1603 KB/s (ex143).
  `ca_aes_ctx_t` grows by 256 B in software builds (GHASH table per key).
- 2026-11-20  CLI: a command runs in a `cli_cmd` task with a 4 KB stack floor
  (the smallest STACK_* covering it) instead of STACK_LARGE: 4 KB instead of
  8 KB on H773, 16 KB on H753/U575/H563.
- 2026-11-19  Crypto: every cryptographic function takes its working context from the
  caller, as first argument, instead of declaring it on the task stack:
  `ca_sha256/ca_sha512/ca_sha384` (one-shot), `ca_hmac_sha256`, `ca_hkdf_sha256`
  (`ca_hmac_sha256_ctx_t`), `ca_pbkdf2_sha256` (`ca_pbkdf2_sha256_ctx_t`),
  `ca_ecdsa_p256_*` (`ca_ecdsa_p256_ctx_t`), `ca_ed25519_*` (`ca_ed25519_ctx_t`),
  `ca_x25519/ca_x25519_base` (`ca_x25519_ctx_t`). The caller chooses where it lives:
  heap (free it with `ca_memory_free(p, NULL, true)` to wipe it), static, or the
  stack, which is the fast choice on H7/F7 (DTCM; PBKDF2 on the H753 runs twice as
  fast as from heap) if the task stack is sized for it. Worst stack measured on the
  H773: VNC/HTTPS task 4.8 -> 2.0 KB, SSH task 4.1 -> 2.1 KB. `ca_hmac_sha384()` is
  removed (no users; `ca_hmac_sha384_ctx()` stays).
- 2026-11-19  VNC: only what was drawn is read and sent. A connected viewer doing
  nothing cost 14% CPU on the H753 (the whole frame re-hashed every poll),
  now 0.6%; a small change reaches the viewer in ~1.3 ms instead of ~10 ms.
- 2026-11-19  VNC: `ca_vnc_service_init()` refuses TLS without `cfg.cred`: a TLS
  session always asks for a login (X509None is no longer offered).
- 2026-11-19  VNC: an RGB565 or L8 framebuffer is converted for 32 bpp clients by
  the DMA2D where the part has one, `VNC_CONV_LINES` rows at a time (default
  8): a full update over TLS on the H753, 640x240 RGB565 90 -> 69 ms, 640x480
  L8 153 -> 130 ms. DMA2D: new
  `ca_dma2d_lock()`/`ca_dma2d_unlock()`, LVGL and VNC share it.
- 2026-11-19  FREE edition: examples are built with `CAOS_FREE` defined, and the
  graphics ones size themselves for half the heap: H753 ex138/ex139 480x320,
  ex141 320x240; U575 ex138 480x320. ex141 does not fit on F767, H563, H773
  in the FREE edition and says so at boot (LVGL alone: ex140).
- 2026-11-19  Headers: no anonymous types in the OS. `CAQUEUE_STATIC_DEFINE(NAME,...)`
  now declares `union NAME_qu` (use is unchanged, `static` in front still
  works); the two words of `ca_mac_t.raw` have their own type `ca_mac_raw_t`.
- 2026-11-19  UDP: a port belongs to the task that binds it. Binding a port
  another live task holds is now refused (`ca_net_proto_bind_udp` returns
  false) instead of silently stealing its datagrams; once the owner ends or
  is restarted the port is free again. `ca_udp_client` handles left open by
  a task that ended are closed within a second (buffers back to the pool).
  ex136: `e136 orphan`.
- 2026-11-19  Software timers belong to the task that creates them: when that
  task ends or is restarted its timers are dropped (no more callbacks, slot
  reused) instead of calling back into freed memory. Timers created before
  the scheduler stay forever; `ca_sw_timer_detach()` hands a timer over to
  the system when its callback uses statics only (`dac run` does).
- 2026-11-18  Examples: ex85 `msink start` asks the example's own task to open
  the port (a listener belongs to the task that opens it; a CLI command's task
  ends with the command).
<!-- paid -->
- 2026-11-17  app_3: the router fragments forwarded datagrams over the MTU of
  the egress iface (`EN_IP_FRAG`, `ip mtu`); with DF set the sender gets ICMP 3/4.
  UDP fragments still do not cross the router.
- 2026-11-17  TCP: `ca_tcp_send()` is all or nothing. It checks for an inflight
  slot and a buffer for every segment first: `CA_TCP_RETRY` now means nothing
  was sent (send the same buffer again after `ca_tcp_wait_writable()`), and
  `CA_TCP_SENT` means the whole buffer is queued. Before, a multi-segment buffer
  could stop half way and return `CA_TCP_RETRY` with part of it already sent.
  Segments the peer's window or the driver cannot take yet wait in the store
  and leave on the next ACK or timer tick. A buffer of more than
  `TCP_MAX_INFLIGHT` segments returns `CA_TCP_GONE`.
- 2026-11-16  NET: TCP announces an MSS that fits the MTU of the interface
  (MTU - 40, at most 1460), so on a reduced-MTU link no segment needs
  fragmenting. A change of MTU applies to the connections opened after it.
- 2026-11-17  NET: IP reassembly no longer prints a line for every fragment
  and every completed datagram (`DEFRAG frag:` / `DEFRAG COMPLETE:`); under a
  burst the console starved the network. Drops, timeouts and init still print.
- 2026-11-16  NET: `ca_ip_fragment_send()` and `ca_net_tx_mtu()` are removed:
  fragmentation is the job of the `EN_IP_FRAG` service. The UDP server, UDP
  client and TFTP no longer set DF, so their datagrams can be fragmented.
- 2026-11-16  USB NCM: TCP data segments (buffers kept for retransmission) are
  now sent by reference like every other frame - one copy per frame (to the
  USB FIFO) instead of three. Received packets are read by the driver straight
  into the network buffer on the NUCLEO-H7S3L8 high-speed driver: one copy
  instead of two. New optional driver callbacks `rx_dest`/`rx_placed` in
  `ca_usb_cb_t` (NULL-safe: drivers and classes without them are unchanged).
- 2026-11-16  net: the Internet checksum (`ca_net_cksum`, TCP pseudo-header
  checksum) reads by aligned 32-bit words instead of byte by byte; new
  `ca_net_cksum_sum()` returns the folded, non-inverted sum. Same results.

### Fixed
- 2026-11-21  CLI: `stack` without a number lists every task again (since May
  it showed only task 0). One line per task: name, size, used, free bytes,
  PSP and floor check; `stack N` adds the addresses of task N. Both end with
  the free space of the stack arena and its largest contiguous block.
- 2026-11-21  TCP: a connection whose data was all queued waiting for the
  window, behind a driver that refuses every frame, now ends in the abort
  after the retransmission bound instead of staying open for ever.
- 2026-11-21  Crypto: the key store grows from 2 KB to 4 KB. With software AES
  (F446, F767, H563, U575) a second secure-pipe link no longer fails to start:
  the AES context with its GCM tables takes ~560 B per session context.
- 2026-11-20  TCP: a connection closed by its task just before the task ends
  (a CLI command) now completes its FIN/ACK close instead of being reset by
  the cleanup of ended tasks.
- 2026-11-20  HTTP: a connection closed without an answer because no request
  arrived within `HTTP_RECV_TIMEOUT_MS` is now reported on the console
  (`HTTP: no request within 1500 ms`) instead of being dropped silently.
- 2026-11-20  Examples: ex143 `e143 test` checks the AES/GHASH split only on the
  software path: on parts with an AES unit the two phases overlap and the test
  failed on a healthy board. ex143 no longer enables the unused CRC and RNG CLIs.
- 2026-11-20  Examples: ex42 `mqx cycle` / `mqx conn` open the MQTT connection
  from the MQTT task instead of the CLI command: the session no longer drops a
  second after the command ends (its socket belonged to the finished command
  task, which the task reaper cleans up).
- 2026-11-20  TFTP: a filename longer than 128 characters (`tftp get` takes the
  rest of the line) overflowed the request buffer on the caller's stack; the
  transfer is now refused. Request and retransmit buffer live in the transfer
  slot: `tftp put` needs ~650 B less stack.
- 2026-11-19  VNC: on a tight heap the TLS service stopped at once ("no memory for TLS"):
  the TLS context is now taken before the optional buffers, which fall back to one
  row. Example 139 on F439: framebuffer 320x160 (was 320x200), so TLS fits.
- 2026-11-19  Examples 141/142: 'e14x start' after a VNC session failed on H773 (the
  command's own task took the stack the GUI task needed); LVGL is now started by a
  resident task once the command has ended.
- 2026-11-19  Example 142: disabled on F767 (159 KB of heap with USB-NCM, LVGL could not
  start); use example 141 over Ethernet there.
- 2026-11-19  VNC: after a non-incremental update of part of the screen, a tile it
  covered only in part could later be skipped (the viewer kept an old pixel).
- 2026-11-18  NET: TCP resources of a task that ended or restarted are released:
  within 1 s its listeners stop and its connections are reset, and
  `ca_tcp_listen()` on a port left by such a task takes it back. A port held
  by a live task is refused (one listener per port). Fixes VNC unreachable
  after a fault of its task (two listeners on 5900, the dead one answering).
- 2026-11-18  NET: VNC no longer frees the buffers of a previous `vnc` task
  that died (two "Invalid Free" on `vnc start` after a fault).
- 2026-11-18  Examples: ex138 `fb test` passes again (44/44): its "bad format"
  case used the value that became `CA_FB_FMT_RGB565`, the framebuffer it then
  created took the slot of the later checks.
- 2026-11-17  NET: VNC over TLS no longer overflows the `vnc` task stack on
  parts with a small `STACK_MIN` (H773): with `EN_VNC_TLS` the task gets at
  least 8 KB, as the HTTPS one.
- 2026-11-17  NET: two tasks calling `ca_udp_client_connect()` at the same
  time could get the same handle and share one client slot (the slot was
  claimed only after the ARP resolve); the slot is now reserved up front.
  The lazy init of the module is atomic too, and `ca_udp_client_disconnect()`
  no longer leaves stale wake-ups for the next client on the same slot.
  ex136 gets `e136 conn [tasks] [rounds]` to exercise concurrent connects.
- 2026-11-17  NET: `ca_tcp_recv()` with a buffer smaller than the received
  segment lost the rest of the segment, which the board had already
  acknowledged, so the peer never resent it: a silent hole in the stream
  (TCP echo of ex41 with 1000-byte segments; SSH could hit it with a nearly
  full receive buffer). The rest is now returned by the next calls, in order.
- 2026-11-17  Examples: the TCP echo of ex41, ex76 and ex126 ignored
  `CA_TCP_RETRY` from `ca_tcp_send()` and dropped that chunk when the TX
  slots were full; it now waits for room and sends it again.
- 2026-11-16  USB NCM: an IN NTB that was an exact multiple of the packet size
  needed a closing zero-length packet, which was dropped when the TX queue was
  full - the frame then waited in the PC until the next one. Such an NTB now
  carries one pad byte instead, so its last packet is always short.
- 2026-11-15  USB CDC: when a write was an exact multiple of the packet size and
  the TX queue was full, the closing zero-length packet was skipped, and the host
  held back the last packet of a stream until the next write. It is now retried
  for up to ~20 ms.
- 2026-11-15  ex52/ex56: the echo task copied only one buffer per wake-up and the
  receive ring in the ISR dropped bytes under a continuous stream; both examples
  now read with the pull mode and lose nothing.
- 2026-11-15  ex50 on the NUCLEO-H7S3L8: the CLI built-in commands failed with
  `[SAFEGUARD] create 'cli_cmd' ... no stack block`; the loopback tasks now use
  `STACK_SMALL` and leave room in the PSP arena.
- 2026-11-15  ETH: about 1 boot in 300 stopped on `SAFEGUARD FATAL ca_net_init`
  (seen on the NUCLEO-F439ZI). The LAN87xx PHY sometimes latches its strap pins
  as power-down mode at reset and gives no RMII clock. `ca_eth_init()` now
  detects this, puts the PHY back in autonegotiation mode and retries the init;
  the console shows `ETH: PHY strap mode repaired`.
- 2026-11-15  ETH (F4, F7): the RMII/MII interface is now selected with the MAC
  held in reset and its clocks off, as the reference manual requires.

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
- 2026-11-03  USB HS (H7, H7S): two tasks writing the same IN
  endpoint (e.g. CDC from several tasks) could fill the same queue slot, so
  one write was lost and the other sent twice.
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
- 2026-11-02  Tamper (H7S): tamper 1 on the B2 pin never fired. The
  pin has no external pull-up on the NUCLEO-H7S3L8 and the tamper ignores the
  GPIO pull; it now uses level detection with the internal precharge pull-up
  (a press = low level), sampled on the RTC clock.
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
