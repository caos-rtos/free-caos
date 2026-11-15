# STM32H5 DTS (digital temperature sensor, RM0481 ch. 23) stub. Without it
# caOS polls SR.TS1_RDY DTS_WAIT (4 million) times at every boot: ~7 s of real
# time in Renode. One-shot model: RDY is set whenever TS1_START is 0. The
# calibration (FMT0 1 MHz at T0 30 C, ramp 1000 Hz/C) and DR make ~25 C at a
# 250 MHz clock: f = 250e6 * 15 / 3769 = 994958 Hz -> 30 + (994958-1e6)/1000.
CFGR1, T0VALR1, RAMPVALR, DR, SR = 0x00, 0x08, 0x10, 0x1C, 0x20
START = 1 << 4
if request.IsInit:
    regs = {T0VALR1: 10000, RAMPVALR: 1000, DR: 3769}
elif request.IsWrite:
    if request.Offset not in (T0VALR1, RAMPVALR, DR, SR):   # factory / read-only
        regs[request.Offset] = request.Value
elif request.IsRead:
    off = request.Offset
    if off == SR:
        request.Value = 0 if regs.get(CFGR1, 0) & START else (1 << 15)
    else:
        request.Value = regs.get(off, 0)
