# caOS FREE — examples

The `examples/` folder holds the caOS examples, one per directory (`example_N/`). Each is a
single firmware: its entry point (`ca_os_exN.c`) plus, where it applies, the PC-side test
scripts (`.py` / `.sh`) that exercise it from your computer.

## Building them

`./do` builds every example that works on the FREE edition, for the board you selected with
`./bootstrap -m <MCU>`. Pick some by number:

```bash
./do 1 3 33
```

The firmware goes to `elfs/<MCU>/<mcu>_example_N.elf`.

## Examples that do not run on the FREE edition

A few examples need something the FREE edition does not provide (a peripheral left out, or
more heap than the FREE cap allows). Each such example carries a **`fail_exNN_info.md`** next
to its source, saying why, and `./do` does not build it. If you build it by hand it will
either fail to link (missing API) or not fit; the note tells you which.

## Status

The examples were run on the bench against the FREE libraries. On the NUCLEO-H563ZI every
testable example passed; the per-board results and any `fail_exNN_info.md` notes travel with
this package. Examples that drive the network or USB have a PC-side counterpart script in
their own directory — adapt its addresses to your own network before running it.

## Where to look

- The entry point of each example shows the caOS API in use.
- The manual (`doc/`) describes the examples and the API in full.
- To start your own program, copy `workspace/app_1` — see **QUICKSTART.md**.
