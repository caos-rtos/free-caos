# STM32 RNG (U5/H5/H7RS: CONDRST in CR) stub fed by the host's /dev/urandom.
# Renode's STM32_RNG does not cover these series: without a model the block
# reads 0, DRDY never rises and ca_rng_init() fails (ex46/47/89/97 ASSERT).
# CR keeps what is written, except CONDRST: the conditioning reset completes
# at once. SR: DRDY whenever RNGEN is set, never a clock or seed error.
# DR: 4 fresh bytes from /dev/urandom per read (System.Random if it cannot
# be opened). Health-test registers (NSCR, HTCR) are a plain register file.
CR, SR, DR = 0x00, 0x04, 0x08
RNGEN = 1 << 2
CONDRST = 1 << 30
if request.IsInit:
    regs = {}
    try:
        src = open('/dev/urandom', 'rb')
    except Exception:
        src = None
        import System
        rnd = System.Random()
elif request.IsWrite:
    off = request.Offset
    if off == CR:
        regs[CR] = request.Value & ~CONDRST
    elif off != SR and off != DR:
        regs[off] = request.Value
elif request.IsRead:
    off = request.Offset
    if off == SR:
        request.Value = 1 if regs.get(CR, 0) & RNGEN else 0
    elif off == DR:
        if src is not None:
            b = src.read(4)
            request.Value = ord(b[0]) | (ord(b[1]) << 8) | (ord(b[2]) << 16) | (ord(b[3]) << 24)
        else:
            request.Value = rnd.Next(0, 0x10000) | (rnd.Next(0, 0x10000) << 16)
    else:
        request.Value = regs.get(off, 0)
