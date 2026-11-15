# app_5 — the XSPI2 trampoline of the NUCLEO-H7S3L8: user guide

The STM32H7S3 has only **64 KB of internal flash**: no caOS image fits in it. The images
run from the external Octo-SPI flash **MX25UW25645G** (32 MB) on XSPI2, executed in place
(XIP) at `0x70000000`.

`app_5` is the **trampoline**: the small program that lives in the internal flash, maps
XSPI2 and jumps to the image. It also has a **serial monitor** for bring-up and recovery.
It is self-contained: no caOS library, no kernel, registers only.

| | |
|---|---|
| Board | NUCLEO-H7S3L8 (MB1737), `MCU_ID 773` |
| Source | `workspace/app_5/trampoline_h773.c`, linker `trampoline_h773.ld` |
| Where it lives | internal flash, `0x08000000`, sector 0 (about 7.6 KB of 8) |
| Version | 1.3 |

On every other board `app_5` does nothing: it prints that it is for the H773 only.

---

## 1. Memory map

| Area | Address | Content |
|---|---|---|
| internal flash | `0x08000000` | trampoline (sector 0, 8 KB sectors) |
| XSPI2 (XIP) | `0x70000000` | caOS image: vector table at the start |
| DTCM | `0x20000000` | task stacks and the image's MSP stack |
| AXI SRAM | `0x24000000` | data, heap, USB and network pools |

The caOS flash driver **refuses sector 0**: a `flash erase` or `flash test` from an image
cannot erase the trampoline. The other seven sectors of the internal flash are free for
data.

---

## 2. What it does at power-up

1. It starts on the 64 MHz HSI, no PLL: the image finds the clocks as after a reset.
2. It writes `PWR_CSR2` **in one go** with the LDO supply and `EN_XSPIM2`. The low byte of
   that register can be written **only once** after power-on, and its reset value is an
   illegal combination (RM0477, Table 44): a partial write would lock it, and the image
   could no longer raise the core voltage.
3. It sets XSPIM in direct mode (port N, AF9); the XSPI2 kernel clock is hclk5.
4. It resets the flash in all three modes (8D, 8S, 1S), because a reset of the MCU alone
   can leave it in octal mode; then it reads the JEDEC ID, which must be `C2 81 39`.
5. It maps the flash in single-line mode, then tries **octal DTR** (8D-8D-8D with DQS).
   DTR is kept only if the first 64 words read in DTR match those read in SPI; otherwise
   it stays in SPI.
6. It checks the image's vector table (see §4).
7. If B2 is not pressed, it puts the console back to reset and jumps to the image.

Typical console output (ST-LINK VCP, 115200 baud):

```
caOS trampoline H773 1.3 (XSPI2 -> 0x70000000)
XSPI2 flash ID 0x3981C2 (MX25UW25645G)
XSPI2: HSLV, octal DTR checked at 64 MHz, run clock hclk5/4
image SP=0x2000FF00 PC=0x70010125
-> jump
```

The image **never reprograms XSPI2**: speed and mode stay as chosen here.

---

## 3. Code flash speed

| Option byte `XSPI2_HSLV` | Mode | Clock while running |
|---|---|---|
| 1 (set on this board) | octal DTR, checked at 64 MHz | hclk5 / 4 = **75 MHz** |
| 0 (factory value) | octal DTR | hclk5 / 8 = 37.5 MHz |

hclk5 is the AXI/AHB bus: 300 MHz with the CPU at 600 MHz.

`XSPI2_HSLV` enables the high-speed low-voltage I/Os of port 2. It is allowed because on
this board VDDXSPI2 is **1.8 V** (SB34, MB1737 schematic p.4). It is a persistent board
setting, reversible with STM32CubeProgrammer (`-ob XSPI2_HSLV=0`). **Never `XSPI1_HSLV`**:
port 1 is supplied at 3.3 V, and HSLV at 3.3 V damages the I/Os.

### 3.1 Measurements above 75 MHz

Measured on 02/11/2026 on this board, at room temperature, with the `e92 xspeed` command
of ex92. The test only reads the flash (never writes it), runs from RAM with interrupts
off and compares the CRC of 512 KB of the image read three times:

| Divider | Clock | With the clocks already up | As the trampoline default |
|---|---|---|---|
| /4 | 75 MHz | reference | ✅ boots |
| /3 | 100 MHz | ✅ 6 runs of 6 | ❌ the image hangs right after the jump |
| /2 | 150 MHz | ✅ 6 runs of 6 | ❌ same |
| /1 | 300 MHz | never tried: the MX25UW25645G stops at 200 MHz DTR | — |

The reason: the image switches SYSCLK to the PLL **while executing from XSPI2 itself**, and
at that moment the XSPI2 kernel clock jumps from 32 to 300 MHz while the controller is
fetching instructions. At /4 the jump goes from 8 to 75 MHz and holds; to 100 or 150 MHz
it does not.

Going above 75 MHz would need that clock switch run from RAM with XSPI2 idle, and then
a new check. Not needed today: the clock stays at 75 MHz.

---

## 4. When it stops in the monitor

The trampoline enters the monitor instead of jumping to the image when:

| Case | Message | Red LED LD3 |
|---|---|---|
| the flash does not answer (ID other than `C2 81 39`) | `flash does not answer` | blinking |
| memory mapping fails | `XSPI2: memory-mapped mode failed` | blinking |
| there is no valid image | `image: <reason>` | blinking |
| **B2 held at reset** | `B2 held: monitor` | off |

Image checks (the first two words at `0x70000000`):

- `erased (no image)`: both `0xFFFFFFFF`;
- `stack pointer not in DTCM/AXI SRAM`;
- `reset vector without the Thumb bit`;
- `reset vector outside XSPI2`.

### 4.1 B2 and the tamper

B2 is on **PC13**, which is also the **TAMP_IN1** input. The board has no external pull-up
on B2: the trampoline uses the internal pull-up, pressed = 0.

If an image left **tamper 1 armed** (it stays armed across a system reset), PC13 is driven
by the TAMP whatever the GPIO configuration, and reads low: with version 1.2 every reset
stopped in the monitor as if B2 were held. Since version **1.3**, B2 is ignored while
`TAMP_CR1.TAMP1E` is set:

```
B2 ignored: PC13 is armed as TAMP_IN1
-> jump
```

The trampoline does not disarm the tamper: the image decides. An invalid image still
lands in the monitor.

---

## 5. The monitor

On the ST-LINK VCP console (USART3, 115200 8N1), with the same syntax as the caOS CLI:

| Command | What it does |
|---|---|
| `mem AAAAAAAA [VVVVVVVV]` | reads, or writes and reads back, one 32-bit word |
| `hexdump AAAAAAAA [N]` | N lines of 16 bytes (default 4, max 32) |
| `id` | JEDEC ID of the XSPI2 flash |
| `go [AAAAAAAA]` | jumps to the image (default `0x70000000`) |
| `reset` | system reset |
| `help` | the command list |

An access to an invalid address with `mem` or `hexdump` prints the fault's CFSR and BFAR
and returns to the prompt: the monitor does not hang.

Example: look at the head of the image, then start it.

```
> hexdump 70000000 2
> go
```

Reading the backup-domain registers (TAMP, backup registers) needs `PWR_CR1.DBP = 1` and
the `RCC_APB4ENR.RTCAPBEN` clock first: without DBP those registers read 0.
Trampoline 1.3 already turns both on when it checks B2; otherwise set bit 16 of
`RCC_APB4ENR` (`0x58024554`) first, reading the register back and leaving the other bits.
Example, disarming tamper 1 by hand:

```
> mem 58024800 00000101     (PWR_CR1: DBP)
> mem 58004400 00000000     (TAMP_CR1: TAMP1E = 0)
> reset
```

---

## 6. Building and programming

The trampoline is built with the workspace, on the H773:

```bash
./build_caos.sh H773 -wp --app 5          # -> elfs/H773/h773_app_5.elf
```

It is built by `workspace/h7rsxx/h773/CMakeLists.txt` alone, with its own linker script
and without the caOS libraries.

Programming with the board's ST-LINK:

```bash
CAOS_MCU=H773 ./prog.sh -s -a 5           # trampoline into the internal flash
CAOS_MCU=H773 ./prog.sh -s -e 92          # an example into the XSPI2 flash
```

`prog.sh` looks at the ELF entry point: if it starts with `0x7`, it adds ST's external
loader for this board by itself (`MX25UW25645G_NUCLEO-H7S3L8.stldr`, in the
STM32CubeProgrammer folder). The trampoline goes into the internal flash with no loader.

The trampoline is rarely reprogrammed: images are updated without touching it.

---

## 7. Recovery

| Situation | What to do |
|---|---|
| the image does not start, the trampoline prints `image: ...` | reprogram the image with `prog.sh -e` |
| the image hangs after `-> jump` | hold B2 and press reset: the monitor starts; then reprogram the image |
| every reset stops in the monitor with B2 not pressed | tamper armed with a 1.2 trampoline: update the trampoline, or disarm the tamper from the monitor (§5) |
| the trampoline itself is damaged | the ST-LINK always reprograms it: `prog.sh -s -a 5` |

A wrong trampoline or image **does not brick the board**: both the internal flash and the
XSPI2 flash are reprogrammed from the ST-LINK in any case.

---

## 8. Version history

| Version | Date | What changes |
|---|---|---|
| 1.0 | 01/11/2026 | first version: XSPI2 in single-line SPI, monitor |
| 1.1 | 01/11/2026 | octal DTR with DQS, kept only when verified |
| 1.2 | 01/11/2026 | HSLV and compensation cell: 75 MHz while running |
| 1.3 | 02/11/2026 | B2 ignored when PC13 is armed as TAMP_IN1 |
