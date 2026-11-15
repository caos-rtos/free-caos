# simu/ — caOS on Renode: issues found and workarounds

Renode **1.17.0** (`/opt/renode`; source tree in `~/renode` but **without** the
`src/Infrastructure` submodule, which holds the peripheral models: model bugs
cannot be fixed there and are worked around in the `simu/` platforms).

**No Renode file is copied into the repo**: `gen_platforms.py` (run by `run.sh`
and `test.sh`) derives the patched CPU models from the installed Renode into
`simu/out/platforms/*_caos.repl` at every run. Nothing to keep in sync, nothing
of Renode redistributed. If a Renode update moves a replaced block, the
generator stops with an error instead of producing a wrong platform.

Test ELFs: the ones already built in `elfs/<MCU>/` (Release build), no rebuild.

What the simulation does and does not do, and how to extend it: `LIMITS.md`.

## Usage

```bash
simu/run.sh h753 elfs/H753/h753_example_2.elf 6000   # boot, 6000 ms virtual, print the console
simu/test.sh                                          # Robot suite (renode-test), report in simu/out/robot
```

Supported boards: `h753` `f767` `f439` `f446` `h563` `h773` (console on USART3), `u575` (USART1).
Not supported: **F407/F103** (not tried).

## Status

| Board | Base model | ex1 | ex2 (banner + tasks) |
|---|---|---|---|
| NUCLEO-H753ZI | `stm32h7.repl` (+h743/h753), generated | ✅ | ✅ |
| NUCLEO-F767ZI | `stm32f777.repl`, generated | — | ✅ |
| NUCLEO-F439ZI | `stm32f4.repl`, generated | — | ✅ |
| NUCLEO-F446ZE | `stm32f4.repl`, generated | — | ✅ |
| NUCLEO-U575ZI-Q | our own `stm32u575_caos.repl.in` | — | ✅ (+ ex68 faults, see 8, 20) |
| NUCLEO-H563ZI | our own `stm32h563_caos.repl.in` | — | ✅ (+ ex68 faults, see 8, 20) |
| NUCLEO-H7S3L8 | our own `stm32h7s3_caos.repl.in` | ✅ | ✅ (+ ex68 faults, see 22-24) |

## Renode issues (models / platforms)

### 1. `STM32H7_ADC`: Renode crash (IndexOutOfRangeException)
- **Symptom**: dotnet crashes (core dump) as soon as caOS reads the temperature
  sensor for the banner: `STM32_ADC_Common.WarnOnCurrentChannelNotPreselected`
  → `IndexOutOfRangeException`.
- **Cause**: the model does not handle ADC3 internal channels 18/19 (TS/VREFINT).
- **Workaround**: `stm32h753_caos.repl` = `stm32h7.repl` (generated) with `adc3`
  replaced by a Python stub (`adc_stub.py`, inlined by the generator): calibration,
  ADRDY and conversions complete instantly, fixed DR.
  A peripheral **cannot be redeclared with a different type** in a repl
  (`Error E02: Variable 'adc3' was already declared`), hence the generated model.

### 2. `STM32_ADC` (F7): Renode crash (DivideByZeroException)
- **Symptom**: `STM32_ADC.OnConversionFinished` → `DivideByZeroException`, again
  while reading the temperature for the banner.
- **Workaround**: `stm32f777_caos.repl` = `stm32f777.repl` (generated) with `adc1..3`
  replaced by a single stub (`adc_stub_f4.py`, F2/F4/F7 ADC layout, 0x400 bytes).

### 3. `stm32f4.repl`: no ADC
- **Symptom**: the F4 stays in `main` forever: caOS polls `ADC1->SR.EOC`, which
  never rises.
- **Workaround**: the same `adc_stub_f4.py` stub appended in `stm32f4_caos.repl`.

### 4. `stm32f4.repl`: no CCM RAM
- **Symptom (F439)**: no output, CPU at `0xEFFFFFFE` (lockup) after ~17 k
  instructions. LoadELF warned: `non-existing peripheral in range <0x10000000, 0x1000DFFF>`.
- **Cause**: caOS places data in the CCM (64 KB at `0x10000000`), missing in the model.
- **Workaround**: `ccm: Memory.MappedMemory @ 0x10000000` in `nucleo_f439.repl`.

### 5. `stm32f4.repl`: SysTick fixed at 72 MHz
- **Symptom**: the 3 s pre-scheduler ST-LINK wait (see 7) lasted 7 s virtual.
- **Cause**: in Renode the SysTick frequency is a platform parameter
  (`nvic.systickFrequency`), it **does not follow RCC**. caOS runs F439/F446 at 168 MHz.
- **Workaround**: `systickFrequency: 168000000` in `nucleo_f4*.repl`.
  Must be kept in sync by hand with the "System Clock" line of the banner.

### 6. Factory calibration reads zero → DIVBYZERO in caOS
- **Symptom**: `[USAGE FAULT] DIVBYZERO`, task `banner` removed.
- **Cause**: `ca_temp_hw_convert()` divides by `TS_CAL2 - TS_CAL1`, read from
  system memory, which is 0 in Renode.
- **Workaround**: `nucleo_<board>.resc` (run after LoadELF) writes TS_CAL1,
  TS_CAL2 and VREFINT_CAL with values chosen to read ~25 °C with the stubs' fixed DR.
- caOS note: real silicon always has the calibration; the fault only shows up
  in simulation. Firmware left untouched.

### 7. Polled SysTick waits are very slow
- **Symptom**: `emulation RunFor` did not return within the timeout; CPU always in `main`.
- **Cause**: `EN_STLINK_WAIT_READY` (caos.c) waits 3 s reading `SysTick->VAL`
  in a tight loop; every MMIO access leaves translated code (~3 real MIPS).
- **Workaround**: `cpu.PerformanceInMips: 20` in the boards: more virtual time
  per instruction, the 3 s end in a few real seconds. Config not touched.
- Side effect: instruction-counted delay loops (not timer-based) take longer in
  virtual time. Irrelevant for boot tests.

### 8. No STM32U5/H5 model: our own U575 and H563 platforms
- Renode 1.17 has the `cortex-m33` CPU and several STM32 models of the same
  generation (USART v2, GPIO, DWT), but no STM32U5 platform.
- `simu/platforms/stm32u575_caos.repl.in`, written from RM0456 and the CMSIS
  addresses (non-secure aliases, TZEN=0): flash 2 MB, SRAM1-3 768 KB contiguous,
  SRAM4, USART1, GPIOA-I, DWT, system memory (TS_CAL), and register stubs for
  RCC (`u5_rcc_stub.py`: every ON bit sets its RDY, SWS follows SW, LSE ready),
  PWR (`u5_pwr_stub.py`: VOSRDY/BOOSTRDY, ACTVOS follows VOS), FLASH, ICACHE,
  DCACHE1 and ADC1 (`adc_stub.py`, same layout as H7; LDORDY added).
  `@inline(stub.py)` lines are expanded by `gen_platforms.py`.
- H563: `stm32h563_caos.repl.in` from RM0481, same scheme: flash 2 MB, SRAM1-3
  640 KB contiguous, USART3 (IRQ 60), engineering bytes at 0x08FFF000 (TS_CAL),
  stubs `h5_rcc_stub.py` (HSIDIVF always set, HSE from the ST-LINK MCO answers,
  SWS at bits 4:3) and `h5_pwr_stub.py` (VOSSR.VOSRDY/ACTVOSRDY, ACTVOS = VOS).
  It booted at the first try. The DTS (0x40008C00) is not modelled: its init
  times out and caOS treats the part as one without DTS, the safe path.
- **ARMv8-M MPU works**: ex68 on the U575 reproduces the silicon for the stack
  guard (`fault stack`/`stackjmp`/`ro`/`cross` -> DACCVIOL, "HIT LOWER CANARY"),
  XN (`xn`/`runaway` -> IACCVIOL), `ibuserr`, `div0`, `undef`, `invstate`, and the
  kernel PSP range check (`stackup`). Same results on the H563, where also
  `fault fpctx` passes (r4-r6, r8 and s16 survive 6 context switches).

### 9. Unmapped accesses do not raise a BusFault
- **Symptom**: ex68 `fault null_r`/`null_w` (M33), `ram`, `flash`, `fp` reported
  nothing: on silicon they are BusFaults.
- **Cause**: Renode logs an access to a non-existing peripheral and returns 0; it
  raises no fault, and no sysbus/cpu option changes that.
- **Status**: fixed in the platforms, see 20.

### 10. CCR.UNALIGN_TRP is ignored
- **Symptom**: ex68 `fault unalign` does not fault (UsageFault UNALIGNED on silicon).
- **Status**: known limit, not pursued: Renode CPU behaviour (its C core), marginal
  in a simulation.

### 11. ADC stub: LDORDY and DR width
- The U5 driver waits for `ISR.LDORDY` (bit 12) before calibrating: added to
  `adc_stub.py` (same bit on H7). DR is now 0x2000 instead of 0x8000: the U5 result
  is 14 bit and `& 0x3FFF` turned 0x8000 into 0, i.e. a division by zero. The
  temperature does not depend on DR (with TS and VREFINT both read as DR,
  `ts_comp` equals `VREFINT_CAL`).

### 32. No RNG on U575, H563, H7S3 (`rng_stub.py`)
- **Symptom**: ex46/ex47/ex89/ex97 stop at boot in `CA_SAFE_FAIL("ca_rng_init")`,
  then Mode: HALT, on U575, H563 and H773 only.
- **Cause**: Renode's `STM32_RNG` covers F4/H7, not the CONDRST variant of U5/H5/
  H7RS, and the three platforms had no RNG at all: the block read 0, DRDY never set.
- **Fix**: `rng_stub.py` at 0x420C0800 (U575/H563, non-secure alias) and 0x48020000
  (H7S3): CONDRST clears at once, DRDY follows RNGEN, DR = 4 bytes of the host's
  `/dev/urandom` per read.

## Usage / environment issues (not Renode bugs)

### 12. Italian locale: `RunFor "0,200"`
- `awk` with Italian `LC_NUMERIC` prints a decimal comma; Renode accepts the
  command but runs nothing (PC stuck at `Reset_Handler`, 0 instructions).
  `run.sh` forces `LC_ALL=C`.

### 13. Peripheral logging = slow simulation
- With `logLevel 2` every unmodelled access produces a warning and slows the
  simulation a lot. `run.sh` uses `logLevel 3` (errors only), with the system
  bus alone at warning level (see 18).

### 14. `PythonPeripheral filename:` not resolved
- The script file name is resolved against the Renode root (`/opt/renode`), not
  the repl folder — neither `path add` nor changing the current directory helps.
  The stubs are therefore **inline** (`script: '''...'''`, unindented, like
  `pwr`/`fmc` in `stm32h7.repl`); `adc_stub*.py` are the only sources, the
  generator inlines them.

### 15. `renode-test` missing dependencies
- `No module named 'robot'`: the requirements in
  `/opt/renode/tests/requirements.txt` are needed, installed in `simu/.venv`
  (git-ignored). `test.sh` puts it in PATH.
- `renode-test` writes `log.html`/`report.html`/`robot_output.xml` in the
  current directory: `test.sh` runs from `simu/out/robot`.
- The `Setup`/`Teardown`/`Test Teardown` keywords are deprecated: use
  `Renode Suite Setup` / `Renode Suite Teardown` / `Renode Test Teardown`.

### 16. `renode --console` hangs when started from a terminal
- **Symptom**: `simu/run.sh` launched by hand in a terminal printed nothing; Renode
  sat at 1% CPU, the log stopped after `mach create`. From a script without a TTY
  it worked.
- **Cause**: in console mode Renode takes over the terminal it finds on stdin.
- **Fix**: `run.sh` starts Renode with `< /dev/null`; `run.sh -i` runs it with no
  monitor at all (`-P -1`) and puts the console on a PTY
  (`emulation CreateUartPtyTerminal` + `connector Connect`), opened with picocom or
  `simu/term.py`. Two traps there: dotnet may ignore SIGTERM (kill -9 after 3 s), and
  minicom's default hardware flow control blocks the input on a PTY.

### 17. Orphan simulations: the `renode` launcher is a script
- **Symptom**: after leaving `run.sh -i`, the next run failed with "Renode did not
  open /tmp/caos_h753"; the log said `0.0.0.0:1234 ... Address already in use`.
- **Cause**: `/usr/bin/renode` is a bash script that starts dotnet as a child
  (no `exec`). Killing `$!` killed the script, and dotnet went on, orphaned, holding
  the PTY and the monitor port. Closing the terminal left one behind too.
- **Fix**: `run.sh -i` takes the real PID from `renode --pid-file`
  (`/tmp/caos_<board>.pid`) and stops that (SIGTERM, -9 after 3 s), on EXIT, HUP,
  INT and TERM. At start it stops a previous simulation of the same board left
  running, and the monitor takes the first free port from 1234.

### 18. Unmapped accesses: logged and summarised
- The default log level is errors, plus **warnings of the system bus**: every
  access to an address with no model. `run.sh` prints a summary at the end
  (`simu/unmapped.py`: address, reads, writes, function of the first access).
  The other peripherals stay at errors: their "unhandled bits" warnings are many
  and slow the simulation.
- What the summary found on ex2, all fixed in the platforms:
  - **H563 DTS** (0x40008C00): `ca_temp_hw_dts_init` polls `SR.TS1_RDY` up to
    4,000,000 times (`DTS_WAIT`, ~2 ms on silicon): ~7 s of real time per boot in
    Renode. Stub `h5_dts_stub.py` (one-shot RDY, calibration for ~25 C): boot
    21.6 s -> 14.7 s.
  - **F439/F446 DWT** missing in `stm32f4.repl`: CYCCNT read 0 (30,000 reads per
    boot), so tick calibration and CPU statistics were wrong. DWT added by the
    generator.
  - **H753**: ST `SystemInit` reads the DBGMCU IDCODE and writes the AXI GPV
    (errata workaround): harmless, silent tags in `nucleo_h753.resc`.

### 19. Timers on U575/H563
- The two M33 platforms had no timers: TIM1-8, TIM15-17 added (`STM32_Timer`, the
  same model as the other boards) with the CMSIS bases and IRQs (TIM8/15/16/17
  differ between U575 and H563), kernel clock 160/250 MHz as caOS sets the APB
  prescalers, TIM2-5 32 bit. Checked with ex68 `fault irq`: the TIM3 update ISR runs.

### 20. BusFault on the holes of the memory map
- `busfault_hole.py`: a Python peripheral over each hole (past the end of flash
  and of SRAM, address 0 on the M33) that sets CFSR.PRECISERR (`cpu.FaultStatus`)
  and pends a BusFault (`nvic.SetPendingSynchronousFault(5)`). It reaches the CPU
  and the NVIC through `self.GetMachine()["sysbus.cpu"]` / `["sysbus.nvic"]`
  (`self.SystemBus`, used by the Renode examples, does not exist in 1.17).
- ex68 on the six boards now matches the silicon for `null_r`/`null_w` (BusFault
  on M33; on M4/M7 address 0 is the flash alias and the compiler's `udf` after
  the NULL dereference gives UNDEFINSTR, as on the board), `ram`, `flash` and
  `fp` (extended frame: EXC_RETURN 0xFFFFFFAC on M33, 0xFFFFFFED on M4/M7). On
  the H563 `ram` does not fault, as on silicon: ex68's address is still SRAM.
- Problems met on the way:
  - **BFAR cannot be set**: `cpu.BusFaultAddress` is read-only, and a write to
    0xE000ED38 through the bus is ignored. The fault is reported without
    BFARVALID (`PRECISERR @ 0x0`), as when the silicon cannot give the address.
  - **F4 holes collided** with the bit-band alias (0x22000000) and, on the F439,
    the CCM (0x10000000): `Error E39 ... conflicts with`. The F4 holes stop
    below them.
  - Holes are board facts (flash and SRAM sizes), so the board files became
    templates too, `nucleo_<board>.repl.in`, expanded by `gen_platforms.py` into
    `simu/out/platforms/` with the stub inlined once (no copy per board).

### 21. M33: a fault inside an ISR arrives with the interrupted context
- **Symptom**: ex68 `fault irq` (divide by zero in the TIM3 update ISR) on U575
  and H563 prints `Task: idle`, `Mode: RESTART`, with the stacked PC in the idle
  task right after `cpsie i`. On silicon (and on the H753 in Renode, an M7) caOS
  prints `Faulted in handler mode ... EXC_RETURN=0xFFFFFFF1`, system halted.
- **Cause**: Renode's ARMv8-M core: the UsageFault taken inside the ISR gets the
  frame and EXC_RETURN of the context the ISR interrupted (PSP), not the ISR's
  (MSP). caOS reads EXC_RETURN bit 2 correctly; the value is wrong.
- **Status**: known limit (Renode CPU core); the M7 path behaves as on silicon.

### 22. NUCLEO-H7S3L8 (H773): own platform, the XIP window as memory
- **Situation**: Renode 1.17 has no STM32H7Rx/7Sx platform and no XSPI model.
  The images link at 0x70000000: on the board the trampoline (workspace/app_5,
  internal flash) maps the Octo-SPI flash on XSPI2 and jumps there.
- **Done**: `platforms/stm32h7s3_caos.repl.in` (Cortex-M7, ITCM/DTCM/AXI/AHB/
  backup SRAM, a 2 MB `MappedMemory` at 0x70000000 the ELF loads into, USART3,
  GPIO A-H and M-P, timers with the H7S3 bases and IRQs), new stubs
  `h7rs_rcc_stub.py` (RCC RM0477: ON -> RDY, SWS follows SW, LSE/LSI ready),
  `h7rs_pwr_stub.py` (ACTVOSRDY, VOSRDY, ACTVOS = VOS, USB33RDY follows
  USB33DEN), `h7rs_dts_stub.py` (the H5 one, DR for APB4 = 150 MHz);
  `regfile_stub.py` for FLASH, SBS, RTC, TAMP. `nucleo_h773.resc` sets
  `VectorTableOffset 0x70000000` (what the trampoline does), a unique ID and
  the DBGMCU IDCODE. The trampoline and the XSPI controller are not simulated.
- **Result**: ex1 and ex2 boot with no unmapped access at the first try.

### 23. H773: DIVBYZERO in the banner task
- **Symptom**: `[USAGE FAULT] DIVBYZERO`, task `banner`, right after `SysTick:`.
- **Cause**: as on the other boards (see 2), the factory calibration reads 0:
  `temp_sensor_h7rsxx.c` (from the H5) divides by TS_CAL2 - TS_CAL1.
- **Workaround**: `nucleo_h773.resc` writes TS_CAL1/TS_CAL2/VREFINT_CAL at
  0x08FFF81x with the H563 values (~25 C).

### 24. ex2 test out of date; ex2 left no stack for the CLI on the H773
- **Symptom**: the Robot case waited for `TASK 1: Hi!`, which ex2 no longer
  prints (rewritten as the scheduler demo on 01/11). With the new commands,
  `stop` after `same 3 busy` printed `[SAFEGUARD] create 'cli_cmd' ... no stack
  block of 8192 B (PSP arena: 0 B free)` in the middle of its own line.
- **Cause**: ex2 sized the workers on the whole free PSP arena; on the 48 KB
  arena of the H773 nothing was left for the `cli_cmd` task of the next command
  (the CLI then runs it in its own task, after the SAFEGUARD line).
- **Fix**: the Robot case drives the demo (`same 3 busy`, `get` -> `round
  robin:`, `stop`); ex2 keeps the STACK_LARGE of one `cli_cmd` out of the worker
  count (caOS example fix, `test_ex02` 8/8 on the real H773 afterwards).

### 25. Ethernet to the PC: PHY, switch, TAP (`run.sh -e`)
- **Situation**: the MACs were already in the CPU models (`SynopsysEthernetMAC` on
  F4/F7, `SynopsysDWCEthernetQualityOfService` on H7), with no PHY and no cable.
- **Fix**: `run.sh -e` adds `Network.EthernetPhysicalLayer @ ethernet 0` with the
  LAN8742A identifiers (0x0007/0xC131), BMSR link up + AN complete, partner 10/100
  HD/FD and reg 31 = AN done + 100FD (`VendorSpecific15`, registers 16-31 are
  `VendorSpecific0..15`); then `emulation CreateSwitch`, the MAC on it, and
  `emulation CreateTap "caos-tap0" ... true` on the same switch. The TAP is a
  persistent one owned by the user (`bench_net.sh tap up`: `ip tuntap add ... user`,
  then the bench `up` = .1 + dnsmasq on it), so Renode opens it without root.
- **Trap**: the first DHCP attempts looked lost (dnsmasq logged nothing, the board
  waited). The DISCOVER was on the TAP, correct, checksum filled by the MAC model
  (tcpdump); dnsmasq answers only ~3 s later, after pinging the address it is about
  to offer, and the 12 s simulated run ended first. Headless runs need 40 s or more.
- **Result**: ex35 DHCP BOUND and ping from the PC on F767, F439 and H753, zero
  unmapped accesses. Harmless warnings: MACCR/DMABMR/DMAOMR tag bits not modelled.

### 26. Jumpers as GPIO connections (`run.sh -w`)
- `simu/wires/<board>_ex<NN>.repl` connects GPIO outputs to inputs
  (`gpioPortA: 5 -> gpioPortC@10`), one direction per wire, the one the example's
  `wire` command drives. ex29 on F767: `WIRE RESULT : OK (3/3)`, 0/3 without `-w`.
- Not peripheral traffic: Renode's `STM32SPI` is master only ("Enabled SPI in slave
  mode, which is not supported" on SPI3 in ex29). SPI master-slave: see 28. `SPI.SPILoopback` and `emulation CreateUARTHub` exist for same-peripheral
  loopbacks.

### 27. A software reset did not restart the board (all boards)
- **Symptom**: after `reset` (NVIC_SystemReset), or ex29 rebooting after `wire`, the
  CPU ended at PC 0xEFFFFFFE with stacking writes at 0xFFFFFFE0-FC. F767 first,
  then the same on H753 and H563.
- **Cause**: at reset the core fetches SP/PC from `InitVectorTableOffset`, 0 by
  default; the images live at 0x08000000 (0x70000000 on the H773), and at 0 the
  models have nothing or an empty ITCM. LoadELF sets SP/PC for the first boot only.
- **Fix**: `cpu InitVectorTableOffset 0x08000000` in every `nucleo_<board>.resc`
  (0x70000000 on the H773).
- **Result**: `reset` restarts F767, F439, H753, H563, U575 (two banners, PC back in
  flash); ex29 resumes after `wire`. F446 and H773 not tried with `reset`.

### 28. SPI master <-> slave between two SPIs (F4/F7)
- **Situation**: ex29 wires SPI1 (master) to SPI3 (slave); Renode's `STM32SPI`
  refuses slave mode ("Enabled SPI in slave mode, which is not supported").
- **Fix**: `spi_pair_f4.py` replaces SPI1 and SPI3 in the generated F4 and F7
  platforms (gen_platforms.py, `spi_pair()`), with `ME, PEER, IRQ` prepended per
  instance. A byte written to the master's DR crosses at once: into the slave's RX
  FIFO (RXNE, RXNEIE -> NVIC line via `nvic.OnGPIO`), and the byte the slave
  preloaded in its DR (0 if none, 0xFF with no slave) into the master's. 8-bit
  frames, no timing, CPOL/CPHA not checked.
- **Trap**: two PythonPeripherals do not share a scope; the shared state lives in
  `AppDomain.CurrentDomain.SetData`. Their init is lazy (first access of each), and
  `GetLocalName(self)` at init does not work: hence the prepended constants.
- **Result**: ex29 with `-w`: F767 26/26 frames received by the slave IRQ, F439
  23/23, 0 ERR, `starve` times out (0/2 bytes in 1000 ms), `wire` 3/3.
  H7/U5/H5 (SPI v3 block) not done.

### 29. Watchdog (`reboot`) on every board
- **Situation**: `reboot` arms the IWDG at its shortest timeout and stops feeding
  it. Only the F4 (`iwdg`) and H753 (`watchdog`) models had one: the F767, U575,
  H563 and H7S3 stayed in the `while(1)` for good.
- **Fix**: `Timers.STM32_IndependentWatchdog` (32 kHz, window option) added in
  `nucleo_f767.repl.in`, `stm32u575_caos.repl.in` and `stm32h563_caos.repl.in` at
  0x40003000, `stm32h7s3_caos.repl.in` at 0x58004800.
- **Result**: ex10 `reboot` restarts the board on F767, F439, H753, U575, H563,
  H773. The reset cause in RCC_CSR is not modelled (ex10: "last reset by watchdog:
  none"); WWDG not modelled.

### 30. Jumpers for every homo example, generated (`gen_wires.py`, `wires.robot`)
- **Situation**: 11 examples have a `wire` command for their bench jumpers
  (ex21 22 24 25 27 29 64 102 103 104 107), each with per-MCU pins.
- **Fix**: `simu/gen_wires.py` reads each example's source, resolves its
  `#if MCU_ID` blocks per board (feature flags taken as on, macros expanded in
  the conditions) and writes `simu/wires/<board>_ex<NN>.repl` from the pairs its
  `wire` drives and reads; boards where the example compiles
  `CA_EXAMPLE_UNSUPPORTED()` are left out. It also writes `tests/wires.robot`:
  boot with the jumpers, `wire` -> `WIRE RESULT : OK (N/N)`, reboot.
- **Traps**: an `#if EX64_ALT_GPIO` must expand the macro (true/false), or every
  board got the alternate pins; ex64's command is `e64 wire`.
- **Result**: 45/45 on the boards with ELFs (F767, F439, H753, U575, H563).
  GPIO continuity only: the traffic crosses where both ends are modelled (ex29 on
  F4/F7, see 28); I2C, timer ETR/encoder, DAC/ADC, SAI, SPDIFRX, COMP are not.

### 31. Malformed-frame storm through the TAP (`tests/net.robot`, `net/inject.py`)
- **Situation**: network regression without boards: ex35 (DHCP, UDP) and ex41
  (TCP echo) on F767/H753/F439, the PC sending a storm of malformed frames
  (`sudo tools/bench_net.sh inject <ip> [n] [seed]`, targeted + seeded scapy
  `fuzz()`), then ping, TCP echo, `arp info`, no fault banner.
- **Trap 1 - Renode aborts on a malformed frame**: `LinuxTapInterface.TransmitLoop`
  decodes every host frame with PacketDotNet (`EthernetFrame.TryCreateEthernetFrame`)
  and an exception there is not caught: SIGABRT of the whole simulation. Seen with
  an IPv4 IHL=15 in a 31-byte frame ("IPv4Packet must have at least 60 bytes") and
  with a total length too short for UDP ("UdpPacket must have at least 8 bytes").
  Not fixable without touching Renode. `inject.py --renode` (passed by bench_net.sh
  when the TAP holds .1) pads every frame to 1514 bytes and leaves out what
  PacketDotNet cannot parse (`renode_safe`: IHL, total length, L4 minimum, TCP data
  offset, ARP 6/4, through VLAN tags) - about 60% of the random part. That
  breakage stays covered by the PC fuzzer (`caos/net/test/fuzz`) and by the storm
  on a real board.
- **Trap 2 - `IP() / bytes` has proto 0**: a TCP segment written as raw bytes
  (needed for verbatim malformed options) left the IP protocol at 0 and the
  checksum without pseudo-header; the board dropped it before TCP. `tcp_raw()`
  sets proto 6 and the right checksum.
- **Trap 3 - frames above the TAP MTU** (1514) make `sendp` stop: sent one by one,
  oversize ones skipped and counted.
- **Result**: 6/6 with the fixed firmware. With the firmware of bd3524cd ex41 hangs
  after the storm (no ping): one SYN with options NOP + length 255 looped the TCP
  option parser for ever (fixed 2026-11-10, `ca_net_proto_tcp_peer_mss`).

## Remaining warnings (harmless for boot)
- RCC: HSEBYP, PLL1GRE, USBSEL, ADCSEL bits not handled; `syscfg` offset 0x2C.
- `cpu: Trying to enable instruction cache, but it's not supported`.
- NVIC: PendSV priority 0xFF written, masked to 0xF0 (correct: 4 priority bits).
