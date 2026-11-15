# STM32U5 RCC stub (RM0456 ch. 11): a register file with reset values where
# every oscillator/PLL is ready as soon as it is switched on and SWS follows SW.
# No clock is computed: SysTick runs at the platform's systickFrequency.
CR, ICSCR1, CFGR1, BDCR, CSR = 0x00, 0x08, 0x1C, 0xF0, 0xF4
# (ON bit, RDY bit) pairs
CR_RDY = ((0, 2), (4, 5), (8, 10), (12, 13), (14, 15), (16, 17), (24, 25), (26, 27), (28, 29))
BDCR_RDY = ((0, 1), (7, 11), (26, 27))       # LSEON, LSESYSEN, LSION
def ready(v, pairs):
    for on, rdy in pairs:
        v = (v | (1 << rdy)) if v & (1 << on) else (v & ~(1 << rdy))
    return v
if request.IsInit:
    regs = {CR: 0x35, ICSCR1: 0x44000000, CSR: 0x00004400}
elif request.IsWrite:
    regs[request.Offset] = request.Value
elif request.IsRead:
    off = request.Offset
    v = regs.get(off, 0)
    if off == CR:
        v = ready(v, CR_RDY)
    elif off == BDCR:
        v = ready(v, BDCR_RDY)
    elif off == CFGR1:
        v = (v & ~0xC) | ((v & 0x3) << 2)    # SWS = SW
    request.Value = v
