# ICACHE / DCACHE stub (U5/H5): a register file; SR reports the reset
# invalidation finished (BUSYF=0, BSYENDF=1). No cache is modelled.
SR = 0x04
if request.IsInit:
    regs = {}
elif request.IsWrite:
    regs[request.Offset] = request.Value
elif request.IsRead:
    request.Value = 0x2 if request.Offset == SR else regs.get(request.Offset, 0)
