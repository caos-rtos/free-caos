#!/usr/bin/env python3
"""Minimal serial terminal for the simulated console (used by run.sh -i when
picocom is not installed): what you type goes to the port, what the port sends
is printed. No flow control, Enter sends CR like a serial terminal.

usage: simu/term.py <port>        exit with Ctrl-]
"""
import os
import select
import sys
import termios
import tty

EXIT = b'\x1d'                                  # Ctrl-]


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__.strip())
    port = os.open(sys.argv[1], os.O_RDWR | os.O_NOCTTY)
    attr = termios.tcgetattr(port)
    tty.setraw(port)                            # bytes through as they are
    stdin = sys.stdin.fileno()
    saved = termios.tcgetattr(stdin)
    print("connected to %s - Ctrl-] to exit\r" % sys.argv[1])
    try:
        tty.setraw(stdin)
        while True:
            ready, _, _ = select.select([stdin, port], [], [])
            if port in ready:
                data = os.read(port, 4096)
                if not data:
                    break
                os.write(sys.stdout.fileno(), data)
            if stdin in ready:
                data = os.read(stdin, 1024)
                if EXIT in data:
                    break
                os.write(port, data)
    except OSError:
        pass                                    # the simulation went away
    finally:
        termios.tcsetattr(stdin, termios.TCSADRAIN, saved)
        try:
            termios.tcsetattr(port, termios.TCSANOW, attr)
        except termios.error:
            pass
        print()


if __name__ == '__main__':
    main()
