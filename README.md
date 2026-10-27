# caOS — FREE edition

**caOS** stands for **C**oppi **A**ngelo **O**perating **S**ystem. It is a small,
bare-metal real-time operating system for STM32 microcontrollers, written and owned
by **Angelo Coppi**. This STM32 version is the bare-metal port of an earlier caOS that
ran on Raspberry Pi, also by Angelo Coppi.

This package is the **FREE edition**: precompiled libraries, the public headers, a set
of examples with their PC-side test scripts, and the documentation — everything you need
to build firmware against caOS for the five supported boards, without the kernel sources.

## What's in the box

```
free_caos/
  include/        public caOS headers + the single os_config.h the libs were built with
  <fam>/<mcu>/    the precompiled libraries, objmap.h, linker script and CMake for each MCU
  examples/       example_N/ : entry point + PC test scripts (.py/.sh)
  workspace/      your own applications, app_N (one directory each) — start from app_1
  external/       CMSIS, FatFs, Thread-Metric (third parties, each with its licence)
  cmake/          the arm-none-eabi toolchain file
  CMakeLists.txt cpu.cmake bootstrap do clean   build system
  new_app.py      generates the skeleton of a new application (tasks, names, levels)
  peek.py         reads a variable of the running firmware by name, through the CLI
  prog.sh reset_mcu.sh tools/                    flashing (ST-LINK / J-Link)
  doc/            the caOS manual (PDF)
  README.md QUICKSTART.md LIMITS.md EXAMPLES.md EULA.md VERSION
```

## Supported boards

NUCLEO-F446ZE, NUCLEO-F767ZI, NUCLEO-H563ZI, NUCLEO-H753ZI, NUCLEO-U575ZI-Q.

## Build in one minute

See **QUICKSTART.md**. In short:

```bash
./bootstrap -m H563     # pick your board
./do                    # build all the examples   ->  elfs/H563/
./do -a 1               # build workspace/app_1
```

## Give it a try

If you have one of the five Nucleo boards on your desk, you are a few minutes away from
running caOS: build an example, flash it, open the serial console and type `help`. Start
from the examples in **EXAMPLES.md** — tasks, IPC, peripherals, networking — then generate
your own application with `./new_app.py` (it writes the next free `workspace/app_N`) and write
your code there. The FREE edition is meant exactly for this: try it,
push it, and see whether caOS fits your next project.

## Before you rely on it

- The FREE edition carries **limits**, listed in **LIMITS.md** (missing peripherals, a
  capped heap, and an integrity/anti-debug behaviour). Read it.
- Use of these binaries is governed by **EULA.md**.
- The configuration (`include/os_config.h`) is **fixed**: it is the one the libraries were
  built with. Editing it does not change the precompiled libraries.
- The cryptographic keys in the package (the bootloader signing key of `workspace/app_2`,
  the SSH keys of the examples) are **public demo keys**: they protect nothing. Before using
  the bootloader or SSH outside your bench, put your own keys in, as the manual explains.

## Trademarks

**STM32**, **NUCLEO**, **ST-LINK** and **STM32CubeProgrammer** are trademarks or registered
trademarks of **STMicroelectronics**. **Arm** and **Cortex** are registered trademarks of
**Arm Limited** (CMSIS). **Raspberry Pi** is a trademark of Raspberry Pi Ltd. **Claude** is a
trademark of **Anthropic, PBC**. All other
trademarks are the property of their respective owners.

caOS is an independent work by Angelo Coppi. It is **not** affiliated with, sponsored by or
endorsed by STMicroelectronics, Arm Limited, Raspberry Pi Ltd or Anthropic. References to their products
are for identification and interoperability only.

## Contact

Commercial licence, questions and bug reports: **caos.rtos@protonmail.com**.

## Author and copyright

caOS — Copyright © 2026 Angelo Coppi. All rights reserved. caOS, the STM32 bare-metal
operating system, is the port of Angelo Coppi's earlier Raspberry Pi caOS.

Developed with the assistance of Claude Opus (Anthropic), an AI assistant used for coding,
review and documentation. Design, testing on hardware and responsibility for the code are
the author's.
