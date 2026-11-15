# STM32H7Rx/7Sx DTS stub: the H5 one (same registers, RM0477) with DR for the
# clock the H7RS driver measures against, APB4 = 150 MHz: f = 150e6 * 15 /
# 2262 = 994695 Hz -> 30 + (994695 - 1e6) / 1000 = ~25 C. Without it caOS polls
# SR.TS1_RDY DTS_WAIT (4 million) times at every boot. One-shot model: RDY is
# set whenever TS1_START is 0.
CFGR1, T0VALR1, RAMPVALR, DR, SR = 0x00, 0x08, 0x10, 0x1C, 0x20
START = 1 << 4
if request.IsInit:
    regs = {T0VALR1: 10000, RAMPVALR: 1000, DR: 2262}
elif request.IsWrite:
    if request.Offset not in (T0VALR1, RAMPVALR, DR, SR):   # factory / read-only
        regs[request.Offset] = request.Value
elif request.IsRead:
    off = request.Offset
    if off == SR:
        request.Value = 0 if regs.get(CFGR1, 0) & START else (1 << 15)
    else:
        request.Value = regs.get(off, 0)
