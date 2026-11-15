# STM32U5 PWR stub (RM0456 ch. 10): voltage scaling and booster always ready,
# the active range (SVMSR.ACTVOS) follows the requested one (VOSR.VOS).
VOSR, SVMSR = 0x0C, 0x3C
if request.IsInit:
    regs = {}
elif request.IsWrite:
    regs[request.Offset] = request.Value
elif request.IsRead:
    off = request.Offset
    v = regs.get(off, 0)
    if off == VOSR:
        v |= (1 << 15) | (1 << 14)           # VOSRDY BOOSTRDY
    elif off == SVMSR:
        v = (1 << 15) | (regs.get(VOSR, 0) & (3 << 16))   # ACTVOSRDY, ACTVOS = VOS
    request.Value = v
