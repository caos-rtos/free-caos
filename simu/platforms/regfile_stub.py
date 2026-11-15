# Plain register file: reads return what was written (0 at reset). For blocks
# caOS only configures and never polls (FLASH ACR, GTZC, SYSCFG ...).
if request.IsInit:
    regs = {}
elif request.IsWrite:
    regs[request.Offset] = request.Value
elif request.IsRead:
    request.Value = regs.get(request.Offset, 0)
