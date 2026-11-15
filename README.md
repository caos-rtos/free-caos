# caOS — FREE edition

**caOS** stands for **C**oppi **A**ngelo **O**perating **S**ystem. It is a small,
bare-metal real-time operating system for STM32 microcontrollers, written and owned
by **Angelo Coppi**. This STM32 version is the bare-metal port of an earlier caOS that
ran on Raspberry Pi, also by Angelo Coppi.

This package is the **FREE edition**: precompiled libraries, the public headers, a set
of examples with their PC-side test scripts, and the documentation — everything you need
to build firmware against caOS for the seven supported boards, without the kernel sources.

**New to caOS?** Start from `doc/caos_overview_en.pdf`: how the scheduler, the heap, the
network stack and the crypto work, what they cost on the boards, and where caOS is weak.

## AI-assistant friendly

The package is made to be used together with an AI coding assistant. It ships:

- **`CLAUDE.md`**, a guidance file for Claude Code that any coding assistant can follow:
  the conventions, the semantics that differ from other RTOSes, and a list of traps found
  on the test bench, each with its symptom and its rule;
- an **offline API reference and Q&A** (`tools/db/`) with two lookup tools,
  `tools/lookup_sym.py` and `tools/lookup_qa.py`, so the assistant uses the real names and
  signatures instead of guessing them;
- **examples that are also tests**, and a **Renode** setup (`simu/`) to run the firmware
  without a board.

The assistant gets the real API and a way to check its work; the board still has the last
word.

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
  tools/lookup_sym.py tools/lookup_qa.py tools/db/   API reference and Q&A, searchable offline
  test/ eol.sh    the test bench, and the end-of-line run over every board
  simu/           run the same .elf on a simulated NUCLEO with Renode (no board needed)
  doc/            the caOS manual (PDF, English and Italian) and the caOS overview (PDF)
  CLAUDE.md       guidance for an AI coding assistant working on this package
  README.md QUICKSTART.md LIMITS.md EXAMPLES.md EULA.md LICENSE CHANGELOG.md VERSION
```

## Supported boards

NUCLEO-F439ZI, NUCLEO-F446ZE, NUCLEO-F767ZI, NUCLEO-H563ZI, NUCLEO-H753ZI,
NUCLEO-H7S3L8, NUCLEO-U575ZI-Q.

On the NUCLEO-H7S3L8 the firmware runs from the external Octo-SPI flash (XSPI2);
the internal flash holds a small trampoline, `workspace/app_5`, flashed once
(guide in `workspace/app_5/doc`).

## Build in one minute

See **QUICKSTART.md**. In short:

```bash
./bootstrap -m H563     # pick your board
./do                    # build all the examples   ->  elfs/H563/
./do -a 1               # build workspace/app_1
```

## Give it a try

If you have one of the seven Nucleo boards on your desk, you are a few minutes away from
running caOS: build an example, flash it, open the serial console and type `help`. Start
from the examples in **EXAMPLES.md** — tasks, IPC, peripherals, networking — then generate
your own application with `./new_app.py` (it writes the next free `workspace/app_N`) and write
your code there. The FREE edition is meant exactly for this: try it,
push it, and see whether caOS fits your next project.

No board? `simu/run.sh` runs the `.elf` you built on a simulated NUCLEO in
[Renode](https://renode.io), with the console in your terminal, Ethernet to your PC on
F439/F767/H753, and the automatic Renode tests (`simu/test.sh`). See `simu/QUICKSTART.md`.

## Find an API

Two offline lookup tools come with the package, on the same data as the reference chapter
of the manual:

```bash
tools/lookup_sym.py ca_uart_put          # signature, header, parameters, return, notes
tools/lookup_sym.py --find mutex timeout # by meaning: every word in the name or description
tools/lookup_qa.py ca_mutex_lock         # questions and answers: intent, common mistakes
```

The declaration in `include/` is the authority; a name the tools do not know does not exist.

## Test results

Every release is run on the test bench on all seven boards before it is published. The
per-board reports (`end_of_line/<mcu>_self_test.html`) are in the public repository, next
to the release they belong to. What is still red, and why, is in **EXAMPLES.md**, "Known
issues in this release".

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

## Links and contact

- Repository and releases: **https://github.com/caos-rtos/free-caos**
- Commercial licence, questions and bug reports: **caos.rtos@protonmail.com**.

## Author and copyright

caOS — Copyright © 2026 Angelo Coppi. All rights reserved. caOS, the STM32 bare-metal
operating system, is the port of Angelo Coppi's earlier Raspberry Pi caOS.

Developed with the assistance of Claude (Anthropic), from the Claude 3 to the Claude 5
models, an AI assistant used for coding, review and documentation: about 350 working
sessions since February 2026 (some 37 a month), about 40 million tokens per session and
more than 15 billion in total (most of them the working context read again at every
step). Design, testing on hardware and responsibility for the code are the author's.
