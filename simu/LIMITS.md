# caOS on Renode — what is simulated, the limits, and how to extend it

The simulation lets you **see caOS run without a board**: the kernel, tasks, memory
protection, faults and the console, on the same `.elf` you would flash. It is not a
replacement for the hardware: timing, analog values and most peripherals stay on the
real board.

How to run it: `QUICKSTART.md`. The history of every problem met while building
it: `CHANGES.md`.

## 1. What behaves as on the chip

Checked on the seven boards of the FREE edition with example 2 (boot,
scheduler, tasks) and example 68 (fault test bench):

| Area | Status |
|---|---|
| Boot, scheduler, tasks, sleeps | as on the chip |
| Console and CLI (USART) | as on the chip, typed live with `run.sh -i` |
| MPU: stack guards, canaries, execute-never | as on the chip (ARMv7-M and ARMv8-M) |
| UsageFault (divide by zero, undefined instruction, invalid state) | as on the chip |
| BusFault on addresses with no memory | as on the chip, without the address (see 2) |
| FPU and FP context switch | as on the chip |
| Timers and their interrupts | as on the chip |
| GPIO | registers only (no LED, no button to press) |

## 2. Known limits

**Time is not real time.** Renode runs a fixed number of instructions per simulated
second: the scripts set it to 20 million, on purpose. A busy-wait loop then ends
quickly, and a 5 s simulation takes about 20 s on a PC. Waits and periods based on
timers and SysTick are correct; anything that counts CPU cycles (DWT, `top`,
the scheduler statistics) is not representative. This is a choice, not a defect: the
simulation is for seeing caOS work, not for measuring it.

**The clock tree is not simulated.** caOS programs the RCC as on the chip and the
simulated RCC says "ready" to everything, but nothing computes the resulting
frequencies: SysTick and the timers run at fixed values written in the board files,
equal to what caOS sets. If an application changed the clock at run time, the
simulation would not follow. See 4.1.

**Faults, three differences:**
- a BusFault does not report the faulting address: Renode does not let a model
  set BFAR, so BFARVALID stays 0 (`PRECISERR @ 0x0`);
- an unaligned access does not trap even with `CCR.UNALIGN_TRP` set;
- on the Cortex-M33 boards (U575, H563) a fault inside an interrupt handler
  reaches caOS as if it had happened in the interrupted task, so caOS removes or
  restarts that task instead of halting. On the Cortex-M7/M4 boards it is correct.

All three are inside Renode's CPU core and cannot be changed from the platform files.

**Peripherals.** Many are only "register stubs": small Python models that answer
what caOS waits for, without the function behind it.

| Peripheral | In the simulation |
|---|---|
| ADC, internal temperature sensor | stub: fixed value, the banner temperature (~25 C) is invented |
| RCC, PWR, flash interface, caches (U575/H563/H7S3) | stubs: always ready |
| DTS (H563, H7S3) | stub: a fixed ~25 C |
| XSPI2 + trampoline (H7S3) | not simulated: the image is loaded at 0x70000000 as memory and starts from its own vector table |
| Ethernet | F439, F767, H753 with `run.sh -e`: LAN8742 PHY, TAP to the PC, DHCP and ping verified (see 4.2); H563 not yet |
| USB | not connected (see 4.3) |
| Software reset, IWDG | `reset` and `reboot` restart every board; reset cause flags (RCC_CSR) and WWDG not modelled |
| Jumpers between pins | `run.sh -w`: GPIO connections from `simu/wires/`, enough for the `wire` checks |
| SPI1 <-> SPI3 (F439, F446, F767) | stub `spi_pair_f4.py`: master and slave wired together, bytes cross both ways, slave IRQ (ex29); other SPIs and other families: Renode master-only model with nothing attached |
| SPI, I2C, SD card, QSPI, DMA2D, JPEG, crypto, RNG ... | missing or partial: examples that need them stop or report errors |

An example that drives a missing peripheral fails in the simulation: that is
expected, not a caOS fault. At the end of every run `run.sh` lists the **unmapped
accesses**, the addresses the firmware touched where the simulated board has nothing:
that list tells which peripheral is missing.

## 3. How the simulation is built, and how to extend it

**Rules.**
- Everything lives in `simu/`. Renode itself is never modified (no patch, no
  fork), and neither is caOS: the simulation adapts to the firmware, not the
  other way round.
- No Renode file is copied: `gen_platforms.py` derives the platforms from your
  Renode installation at every run, into `simu/out/platforms/`.
- Every problem met goes into `CHANGES.md`, with symptom, cause and fix.

**The pieces.**

| File | What it is |
|---|---|
| `platforms/nucleo_<board>.repl.in` | the board: which CPU platform, RAM/flash sizes, CPU speed, holes of the memory map |
| `platforms/stm32u575_caos.repl.in`, `stm32h563_caos.repl.in`, `stm32h7s3_caos.repl.in` | CPU platforms written for caOS (Renode has no STM32U5/H5/H7Rx-7Sx) |
| `platforms/*.py` | register stubs, inlined by the generator where a template says `@inline(file.py)` |
| `platforms/nucleo_<board>.resc` | commands run after loading the `.elf`: factory calibration values, silent tags |
| `gen_platforms.py` | builds `simu/out/platforms/` from Renode's models and the templates |

Renode platform files (`.repl`) declare peripherals and their addresses; the syntax
is in the Renode documentation (renode.readthedocs.io, "Platform description format").

**Adding a peripheral**, in order of preference:
1. **Renode already has a model** (`STM32_Timer`, `STM32F7_USART`, `STM32_GPIOPort`,
   `STM32WBA55_GPDMA` ...): declare it in the `.repl.in` with the CMSIS base address
   and IRQ number. The timers of the U575/H563 were added this way.
2. **A register stub**: a Python script that keeps a register file and returns the
   bits caOS polls (ready flags, status). Use it when caOS only needs an answer,
   not the function. Write it so that it cannot divide or loop forever in caOS.
3. **A full model** in C#: Renode can load a `.cs` file at run time
   (`include @Model.cs`) without being rebuilt. Not used so far.

A stub reaches the rest of the machine through `self.GetMachine()`:
`self.GetMachine()["sysbus.cpu"]`, `["sysbus.nvic"]`. `busfault_hole.py` uses it to
raise a BusFault.

**Adding a board.** Write `nucleo_<board>.repl.in` (CPU platform, memory, holes), a
`.resc` with the calibration values caOS reads, the console UART in `run.sh` and
`tests/boot.robot`; then check that example 2 boots with no unmapped accesses.

## 4. Extensions not done, and what they would take

None of these is needed to see caOS run. Each one has an uncertain outcome: it
depends on how closely Renode's models follow what the caOS drivers expect, and that
is known only by trying.

### 4.1 Clock tree

**Goal:** SysTick, timers and DWT follow the frequencies caOS programs in the RCC,
instead of fixed values in the board files.

**What exists:** in Renode the SysTick frequency (`nvic Frequency`), the timers'
frequency and divider, and the CPU speed can all be changed while the simulation runs.

**How:**
- U575/H563: the RCC is our stub, so the computation goes inside it.
- H7/F7/F4: the RCC is a Renode model, so a Python hook on its writes
  (`rcc AddAfterWriteDoubleWordHook`) does it.

In both cases the code reads the PLL and prescaler registers, computes SYSCLK, HCLK
and the APB timer clocks, and updates SysTick and the timers.

**Effort:** about half a day per family (F4, F7) and one day for H7 and U5/H5, plus
tests (banner "System Clock", tick calibration, one timer period in simulated time).

**Decision:** the CPU speed stays reduced (20 million instructions per simulated
second). Real speed would make cycle counts realistic and every boot minutes long,
for a simulation whose purpose is to show caOS working.

### 4.2 Ethernet (done on F439, F767, H753, 06/11/2026)

**Status:** `run.sh -e`. Steps 1-3 below are done on the three boards with a MAC
model (DHCP lease and ping from the PC, `CHANGES.md` 25); the H563 still lacks the
MAC in its template. The rest of this section is the original plan.

**Goal:** the caOS network stack (DHCP, ping, TCP, HTTP, SSH ...) reachable from the
PC, or between two simulated boards.

**What exists in Renode:**
- the two Ethernet MACs caOS drives: `SynopsysDWCEthernetQualityOfService` (H7,
  same IP as the H563) and `SynopsysEthernetMAC` (F7/F4);
- a generic PHY with programmable registers (`EthernetPhysicalLayer`);
- a virtual switch and a host TAP interface (`emulation CreateSwitch`,
  `emulation CreateTap`): the simulated board appears to Linux as a network
  interface.

**How:**
1. Enable the MAC already present in the H753 platform; connect it to a switch and
   the switch to a TAP.
2. Add a PHY at the address the caOS driver uses, with the LAN8742 identifiers and an
   auto-negotiation that completes (the driver waits for it).
3. Run a DHCP server on the TAP, as the test bench does, then the network examples.
4. F767/F439 with the other MAC model; H563 by adding the MAC to its template.

**Risks:** the DMA descriptor formats and the interrupt bits must match what the
caOS driver does; the PHY must answer like the LAN8742; the TAP needs root once.

**Effort:** 1-2 days on the H753 up to DHCP and ping, about one more for F767/F439.

### 4.3 USB

**Goal:** the caOS USB device stack (CDC console, NCM network, MSC disk) seen by the
PC as a USB device.

**What exists in Renode:** the USB controller models `DesignWare_USB_OTG` (OTG of
F4/F7/H7) and `STM32_USB` (USB FS device of the H563 family), and a USB/IP server to
export simulated USB devices to Linux.

**How:** declare the controller in the board platform, start the USB/IP server and
attach the device on the PC with `usbip attach`.

**Risks:** Renode's USB support is aimed mainly at its own device models and at host
mode. Whether a firmware-driven device controller, enumerated by the PC through
USB/IP, works with the caOS stack is not known.

**Effort:** a half-day probe on the H753 is needed before any estimate.
