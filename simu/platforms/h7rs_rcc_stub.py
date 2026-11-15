# STM32H7Rx/7Sx RCC stub (RM0477 ch. 7): a register file with reset values
# where every oscillator/PLL is ready as soon as it is switched on, HSIDIVF is
# always set and SWS follows SW. No clock is computed: SysTick runs at the
# platform's systickFrequency.
CR, CFGR, BDCR, CSR = 0x00, 0x10, 0x70, 0x74
# (ON bit, RDY bit): HSI CSI HSI48 HSE PLL1 PLL2 PLL3
CR_RDY = ((0, 2), (7, 8), (12, 13), (16, 17), (24, 25), (26, 27), (28, 29))
LSE_RDY = ((0, 1),)                          # BDCR: LSEON -> LSERDY
LSI_RDY = ((0, 1),)                          # CSR:  LSION -> LSIRDY
HSIDIVF = 1 << 5
def ready(v, pairs):
    for on, rdy in pairs:
        v = (v | (1 << rdy)) if v & (1 << on) else (v & ~(1 << rdy))
    return v
if request.IsInit:
    regs = {CR: 0x25}                        # HSION HSIRDY HSIDIVF
elif request.IsWrite:
    regs[request.Offset] = request.Value
elif request.IsRead:
    off = request.Offset
    v = regs.get(off, 0)
    if off == CR:
        v = ready(v, CR_RDY) | HSIDIVF
    elif off == BDCR:
        v = ready(v, LSE_RDY)
    elif off == CSR:
        v = ready(v, LSI_RDY)
    elif off == CFGR:
        v = (v & ~0x38) | ((v & 0x7) << 3)   # SWS[5:3] = SW[2:0]
    request.Value = v
