# STM32H5 PWR stub (RM0481 ch. 10): voltage scaling always ready, the active
# range (VOSSR.ACTVOS) follows the requested one (VOSCR.VOS).
VOSCR, VOSSR = 0x10, 0x14
if request.IsInit:
    regs = {}
elif request.IsWrite:
    regs[request.Offset] = request.Value
elif request.IsRead:
    off = request.Offset
    v = regs.get(off, 0)
    if off == VOSSR:
        vos = (regs.get(VOSCR, 0) >> 4) & 3
        v = (1 << 3) | (1 << 13) | (vos << 14)   # VOSRDY ACTVOSRDY ACTVOS
    request.Value = v
