# caOS FREE — limits

The FREE edition is for evaluation. It is the full caOS kernel and driver set, built with
some features left out and some resources capped. This page lists exactly what differs
from the full edition, so there are no surprises.

## Peripherals and stacks NOT in the FREE edition

These are compiled out of the FREE libraries; their APIs are not even present in the headers:

- **CAN / FDCAN** (`EN_CAN`) — automotive bus.
- **USB High-Speed device** (`EN_USB_HS`) — USB HS via an external ULPI PHY. USB Full-Speed
  (CDC, MSC, NCM, DFU, custom) **is** included.
- **Tamper** (`EN_TAMP`) — hardware erase of the backup domain and of the secret area on an
  intrusion event (`ca_exti_tamper_enable`, `ca_memory_secret`). The backup registers and
  the backup SRAM themselves (`bkp.h`) **are** included.
- **Filter math accelerator** (`EN_FMAC`) — FIR/IIR filters computed by the FMAC unit of the
  H563 and U575 (`ca_fmac_*`). The CORDIC coprocessor (`cordic.h`) **is** included.

Everything else the five boards offer is on: UART, SPI, I2C, SAI (I2S audio), SPDIFRX, DMA2D, JPEG, ADC, DAC, DMA, timers, RTC,
flash, QSPI, SDIO, the full TCP/IP stack and its services (DHCP, DNS, mDNS, NTP, SNTP,
TFTP, HTTP, MQTT, Modbus, iperf, SSH), the crypto (AES, SHA, X25519, Ed25519, keystore),
USB Full-Speed, FatFs, and the CLI.

## Resource cap

- **Heap**: the FREE edition reserves half of the available heap. `free` on the CLI shows
  the reduced figure. This cap is built into the library and is not changed by editing the
  linker script.

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
can break the build. Driver drivers you enable there that the library does not carry (CAN,
USB HS, tamper, FMAC) will not link.

## Not a limit, but good to know

- Supported boards: NUCLEO-F446ZE, -F767ZI, -H563ZI, -H753ZI, -U575ZI-Q.
- Release build only (no debug build is shipped).
- The kernel sources are not part of this package.

See **EULA.md** for the terms of use.
