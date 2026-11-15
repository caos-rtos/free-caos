# STM32H7Rx/7Sx PWR stub (RM0477 ch. 6): the supply is valid (SR1.ACTVOSRDY)
# and the active voltage scale follows the requested one (SR1.ACTVOS =
# CSR4.VOS, CSR4.VOSRDY set); USB33RDY follows USB33DEN. The write-once low
# byte of CSR2 is not modelled: caOS writes it once (the trampoline did).
SR1, CSR2, CSR4 = 0x04, 0x0C, 0x14
if request.IsInit:
    regs = {CSR2: 0x2}                       # LDOEN, as the trampoline leaves it
elif request.IsWrite:
    regs[request.Offset] = request.Value
elif request.IsRead:
    off = request.Offset
    v = regs.get(off, 0)
    if off == SR1:
        v = (1 << 1) | (regs.get(CSR4, 0) & 1)            # ACTVOSRDY, ACTVOS
    elif off == CSR4:
        v = v | (1 << 1)                                   # VOSRDY
    elif off == CSR2:
        v = (v | (1 << 26)) if v & (1 << 24) else (v & ~(1 << 26))   # USB33RDY
    request.Value = v
