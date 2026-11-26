# example_76 — not built on the NUCLEO-F446ZE (FREE edition)

**Status:** not built by `./do` for the F446 in the FREE package. It is built and runs on
the other boards.

## Why

caOS TCP Server (passive open / echo server) over USB-NCM (example_76).

The FREE libraries are built with one fixed configuration that turns on
every service of the network stack, the crypto and the USB-NCM link with it. On the F446
(128 KB of RAM) the static buffers of those services together with this example exceed the
RAM by less than 1 KB: the link fails with `cannot move location counter backwards`.

The full edition builds this example with its own configuration and runs it on the F446.
