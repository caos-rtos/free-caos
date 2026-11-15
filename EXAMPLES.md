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

Before each release the examples are run on the test bench against the FREE libraries, on
all seven boards (`./eol.sh`, see QUICKSTART.md). The per-board reports,
`end_of_line/<mcu>_self_test.html`, are published in the repository with the release; the
`fail_exNN_info.md` notes travel with this package. An example a board cannot run (the part
lacks the peripheral) is reported as disabled for that board, not as a failure.

Examples that drive the network or USB have a PC-side counterpart script in their own
directory — adapt its addresses to your own network before running it.

## Known issues in this release

The end-of-line run of this release passed every phase on NUCLEO-F446ZE, -F767ZI,
-H563ZI and -U575ZI-Q. The red phases on the other three boards, and what they mean:

| Example | Board | What happens | Why |
|---|---|---|---|
| 56 (USB composite CDC + MSC + NCM) | F439, H7S3 | the MSC function does not come up | the example asks for a 128 KB RAM disk on the heap; with the FREE heap cap these two boards have less than that free. CDC and NCM work; on F767, H563, H753 and U575 the example passes |
| 40 (TCP client) | F439 | occasionally stops at boot with `[SAFEGUARD FATAL] ... ca_net_init` | a known intermittent fault of the network start-up on the F439, under investigation; a reset brings the board up |
| 88 (Thread-Metric) | H7S3 | the `sync` test reported one error in one run | seen once, not reproduced yet; the other Thread-Metric tests pass |
| 100 (input capture of the LSE) | H753 | the measured LSE frequency is out of spec | a property of the board, not of caOS: on the NUCLEO-H753ZI the CPU clock comes from the ST-LINK MCO, about 4700 ppm off |

They are listed here rather than hidden: the per-board reports show them in full.

## Without a board

Most examples also run on a simulated NUCLEO, on your PC with Renode (`simu/run.sh`, see
`simu/QUICKSTART.md`). The kernel, IPC, fault and console examples behave as on the board;
those that need Ethernet run on F439/F767/H753 with `simu/run.sh -e`, those with bench
jumpers with `-w`. USB examples and the accelerators that are not modelled need the board.

## Where to look

- The entry point of each example shows the caOS API in use.
- The manual (`doc/`) describes the examples and the API in full.
- To start your own program, copy `workspace/app_1` — see **QUICKSTART.md**.
