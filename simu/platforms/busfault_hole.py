# A hole in the memory map: on silicon an access here is a BusFault, while
# Renode alone would log it and return 0. Pends a precise BusFault (CFSR
# PRECISERR) on the CPU. BFAR cannot be set from here (read-only in Renode):
# BFARVALID stays 0, as when the silicon cannot report the address either.
if not request.IsInit:
    m = self.GetMachine()
    cpu = m["sysbus.cpu"]
    cpu.FaultStatus = cpu.FaultStatus | (1 << 9)         # BFSR.PRECISERR
    m["sysbus.nvic"].SetPendingSynchronousFault(5)       # BusFault
    if request.IsRead:
        request.Value = 0
