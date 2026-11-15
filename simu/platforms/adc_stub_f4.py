# Minimal STM32 F2/F4/F7 ADC block stub (ADC1..3 + common, 0x400 bytes).
# Renode's STM32_ADC divides by zero in OnConversionFinished (F7), and the
# stm32f4 platform has no ADC at all (caOS would poll SR.EOC forever).
# Conversions complete instantly; DR is a fixed value (~mid-scale, 12 bit).
SR, CR2, DR = 0x00, 0x08, 0x4C
if request.IsInit:
    regs = {}
elif request.IsWrite:
    off = request.Offset
    if off % 0x100 == SR:
        pass                                  # flags always reported set
    elif off % 0x100 == CR2:
        regs[off] = request.Value & ~(1 << 30 | 1 << 22)   # SWSTART/JSWSTART self-clear
    else:
        regs[off] = request.Value
elif request.IsRead:
    off = request.Offset
    if off < 0x300 and off % 0x100 == SR:
        request.Value = 0x1E                  # EOC JEOC JSTRT STRT
    elif off < 0x300 and off % 0x100 == DR:
        request.Value = 0x800
    else:
        request.Value = regs.get(off, 0)
