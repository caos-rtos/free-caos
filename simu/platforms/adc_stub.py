# Minimal STM32 ADC (H7/U5/H5 register layout) stub: Renode's STM32H7_ADC model
# crashes (IndexOutOfRange) when caOS samples the temperature channel.
# Calibration, ADRDY and conversions complete instantly; DR is a fixed value (0x2000, valid for 12..16-bit results).
ISR, CR, DR = 0x00, 0x08, 0x40
ADEN, ADDIS, ADSTART, ADCAL = 1 << 0, 1 << 1, 1 << 2, 1 << 31
if request.IsInit:
    regs = {}
elif request.IsWrite:
    off, val = request.Offset, request.Value
    if off == ISR:
        pass                                  # flags are w1c: always set anyway
    elif off == CR:
        if val & ADDIS:
            val &= ~(ADEN | ADDIS)
        regs[CR] = val & ~(ADCAL | ADSTART | ADDIS)
    else:
        regs[off] = val
elif request.IsRead:
    off = request.Offset
    if off == ISR:
        request.Value = (0x0F if regs.get(CR, 0) & ADEN else 0x0E) | (1 << 12)   # + LDORDY
    elif off == DR:
        request.Value = 0x2000                # 14-bit safe; T depends on TS_CAL only
    else:
        request.Value = regs.get(off, 0)
