# STM32H5 RCC stub (RM0481 ch. 11): a register file with reset values where
# every oscillator/PLL is ready as soon as it is switched on, HSIDIVF is always
# set and SWS follows SW. The HSE (ST-LINK MCO on the NUCLEO) answers.
# No clock is computed: SysTick runs at the platform's systickFrequency.
CR, CFGR1, BDCR = 0x00, 0x1C, 0xF0
# (ON bit, RDY bit): HSI CSI HSI48 HSE PLL1 PLL2 PLL3
CR_RDY = ((0, 1), (8, 9), (12, 13), (16, 17), (24, 25), (26, 27), (28, 29))
BDCR_RDY = ((0, 1), (26, 27))                # LSEON, LSION
HSIDIVF = 1 << 5
def ready(v, pairs):
    for on, rdy in pairs:
        v = (v | (1 << rdy)) if v & (1 << on) else (v & ~(1 << rdy))
    return v
if request.IsInit:
    regs = {CR: 0x2B}                        # HSION HSIRDY HSIDIV=/2 HSIDIVF
elif request.IsWrite:
    regs[request.Offset] = request.Value
elif request.IsRead:
    off = request.Offset
    v = regs.get(off, 0)
    if off == CR:
        v = ready(v, CR_RDY) | HSIDIVF
    elif off == BDCR:
        v = ready(v, BDCR_RDY)
    elif off == CFGR1:
        v = (v & ~0x18) | ((v & 0x3) << 3)   # SWS[4:3] = SW[1:0]
    request.Value = v
