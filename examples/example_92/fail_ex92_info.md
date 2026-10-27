# example_92 — requires extra hardware

**Status:** not built by `./do` in the FREE package.

## Why

example_92 needs an **external SPI NOR flash module (W25Q128)** wired to the Nucleo-144.
A stock Nucleo board does not have it, so the example cannot run out of the box and is
left out of the default build.

## What the example does

It drives the W25Q128 through the QSPI / OCTOSPI peripheral. It runs erase, write,
single and dual read-back, memory-mapped read, a clock sweep, and checks that the data
survives an MCU reset. The first phase, `e92 wire`, reads the JEDEC ID of the flash on
plain GPIO. Without the module it reports:

```
JEDEC ID  : 00 00 00  FAIL (no answer: VCC/GND/CS/CLK/D0/D1)
WIRE RESULT : FAIL  (0/1)
```

and the rest of the example does not run.

## Wiring (Zio connector, every Nucleo-144)

| Module pin | Nucleo position |
|---|---|
| VCC | CN8-7 (3V3) |
| CS | CN10-13 |
| CLK | CN10-15 |
| GND | CN10-17 |
| DO (data out of the flash) | CN10-21 (IO1) |
| DI (data in of the flash) | CN10-23 (IO0) |

On W25Q modules, "DO/DI" is easily read as "D0/D1". `e92 wire` also tries the two data
lines swapped and tells you if they are.

## If you have the module

The QSPI driver API is part of the FREE edition. Wire the module as shown, remove this
file and build the example with `./do 92`.
