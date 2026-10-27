"""caos_tftp - push files to a caOS board over TFTP (the board is the server).

    python3 tools/caos_tftp put <board-ip> <file> [--name NAME]

As a library:
    from caos_tftp import put, TftpError
    put("192.168.10.100", open("img.cimg", "rb").read(), "img.cimg")

Standard library only, Python 3.6+.
"""
from .client import put, TftpError

__all__ = ["put", "TftpError"]
