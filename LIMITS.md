# caOS FREE — limits

The FREE edition is for evaluation. It is the full caOS kernel and driver set, built with
some features left out and some resources capped. This page lists exactly what differs
from the full edition, so there are no surprises.

## What differs from the full edition

The FREE edition carries the same drivers, stacks and services as the full edition. Only
two things differ:

1. **One fixed configuration** (`include/os_config.h`, see below) instead of one per project.
2. **Half the heap** (see "Resource cap").

The fixed configuration leaves off three modules; their API is not in the headers and not
in the libraries:

- **CAN / FDCAN** (`EN_CAN`).
- **USB High-Speed device** (`EN_USB_HS = 0`). USB Full-Speed (CDC, MSC, NCM, DFU, custom)
  **is** included on every board. Why: High-Speed works only on the NUCLEO-H7S3L8 (integrated
  PHY); the other Nucleo boards have no ULPI PHY. With Full-Speed and High-Speed built into
  the same fixed configuration, the High-Speed driver is linked into every USB example, the
  Full-Speed queues shrink and the device answers as a High-Speed one: a Full-Speed example
  failed on the test bench, and the F446 ran out of RAM.
- **MicroPython** (`EN_PYTHON = 0`). Why: with one configuration for every firmware, the CLI
  `py` command would link the Python VM (about 120 KB of flash and 5 KB of RAM) into all of
  them, Python or not: the F446 runs out of RAM and the bootloader and the application slots
  (512 KB) on the H563 and H753 no longer fit.

## Everything else is on

UART, LPUART, SPI, I2C, SAI (I2S audio), SPDIFRX, DMA2D, JPEG, ADC, DAC, COMP, OPAMP, CORDIC,
FMAC, DMA, timers, LPTIM, RTC, tamper and backup domain, power modes, flash, QSPI, SDIO,
the full TCP/IP stack and its services (DHCP, DNS, mDNS, NTP, syslog, TFTP, HTTP, HTTPS with
TLS 1.3, MQTT, Modbus, iperf, SSH, IP fragmentation, packet tracer) over Ethernet and over
USB-NCM, the crypto (AES, SHA, HMAC/HKDF/PBKDF2, X25519, Ed25519, ECDSA P-256, ChaCha20-Poly1305,
BLAKE2s, keystore, secure pipe), USB Full-Speed device (CDC, MSC, NCM, DFU,
custom), FatFs, the framebuffer (`fb`), the VNC server (plain and over TLS), LVGL (graphics
library, MIT, with the DMA2D drawing unit where the part has one), the bootloader support
and the CLI.

## Resource cap

- **Heap**: the FREE edition reserves half of the available heap. `free` on the CLI shows
  the reduced figure. This cap is built into the library and is not changed by editing the
  linker script.
- **Graphics examples** are sized for half the heap when built from this package
  (`CAOS_FREE` is defined for them): on the H753 example 138 and 139 use a 480x320
  framebuffer and example 141 a 320x240 screen; on the U575 example 138 uses 480x320.
  Example 141 (LVGL over VNC) does not fit half the heap of the F767, H563 and H773 and
  says so at boot; LVGL alone runs there with example 140. Your own application can use
  the same `#if CAOS_FREE` to pick its sizes.

## Behaviour of the FREE binaries

- **Not debuggable.** The FREE firmware detects an attached halting debugger (a GDB/JTAG/SWD
  session, as needed to set a breakpoint or single-step) and stops with a safeguard message.
  Flashing and running normally is unaffected; attaching a debugger to step through the
  FREE firmware is not supported. Debug from inside instead: `peek.py` reads your
  variables by name through the CLI (`mem`, `hexdump`), or a task prints them when they
  change - see QUICKSTART.md, "Debugging without a debugger".
- **Integrity-checked.** The library verifies itself at runtime. A modified FREE library
  stops with a safeguard message.

Neither affects a program you build and run normally on a supported board.

## Fixed configuration

`include/os_config.h` is the configuration the libraries were compiled with. It is fixed:
changing it does not change the precompiled libraries, and a value that disagrees with them
can break the build. Enabling CAN, USB HS or MicroPython there does not add them: the libraries do not carry them.

## Not a limit, but good to know

- Supported boards: NUCLEO-F439ZI, -F446ZE, -F767ZI, -H563ZI, -H753ZI, -H7S3L8, -U575ZI-Q.
- Release build only (no debug build is shipped).
- The kernel sources are not part of this package.

See **EULA.md** for the terms of use.
