# SPI v1/v2 (STM32 F4/F7) as a PAIR of peripherals wired together, master <-> slave.
# Renode's STM32SPI is master only ("slave mode not supported"): ex29 (SPI1 master,
# SPI3 slave, three jumpers) could not run. gen_platforms.py inlines this script for
# each SPI of the pair, with ME, PEER and IRQ prepended, e.g.
#   ME, PEER, IRQ = 'spi1', 'spi3', 35
# The two instances share their state through the AppDomain (each PythonPeripheral
# has its own script scope); init is lazy, at the first access of each.
# A byte written to the master's DR is clocked at once: it lands in the slave's RX
# FIFO, and the byte the slave had preloaded in its DR (0 if none) lands in the
# master's. No timing, no clock polarity/phase check, 8-bit frames only.
from System import AppDomain
CR1, CR2, SR, DR = 0x00, 0x04, 0x08, 0x0C
SPE, MSTR, RXNEIE = 1 << 6, 1 << 2, 1 << 6
FIFO = 4                                   # SPI v2 RX FIFO, 32 bits


def spi_all():
    d = AppDomain.CurrentDomain
    st = d.GetData("caos_spi_pair")
    if st is None:
        st = {}
        d.SetData("caos_spi_pair", st)
    return st


def spi_get(st, name):
    if name not in st:
        st[name] = {'cr1': 0, 'cr2': 0x0700, 'rx': [], 'tx': None, 'ovr': False,
                    'irq': -1, 'regs': {}}
    return st[name]


def spi_irq(s):
    if s['irq'] < 0:
        return
    lvl = bool(s['cr1'] & SPE) and bool(s['cr2'] & RXNEIE) and len(s['rx']) > 0
    self.GetMachine()["sysbus.nvic"].OnGPIO(s['irq'], lvl)


def spi_push(s, b):
    if len(s['rx']) >= FIFO:
        s['ovr'] = True
    else:
        s['rx'].append(b & 0xFF)


st = spi_all()
me = spi_get(st, ME)
me['irq'] = IRQ
off = request.Offset

if request.IsInit:
    pass
elif request.IsWrite:
    v = request.Value
    if off == CR1:
        me['cr1'] = v & 0xFFFF
        spi_irq(me)
    elif off == CR2:
        me['cr2'] = v & 0xFFFF
        spi_irq(me)
    elif off == DR:
        b = v & 0xFF
        if (me['cr1'] & (SPE | MSTR)) == (SPE | MSTR):
            peer = st.get(PEER)
            resp = 0xFF                       # no slave: MISO idles high
            if peer is not None and (peer['cr1'] & SPE) and not (peer['cr1'] & MSTR):
                resp = peer['tx'] if peer['tx'] is not None else 0x00
                peer['tx'] = None
                spi_push(peer, b)
                spi_irq(peer)
            spi_push(me, resp)
            spi_irq(me)
        else:
            me['tx'] = b                      # slave: next byte out on MISO
    elif off != SR:
        me['regs'][off] = v
elif request.IsRead:
    if off == SR:
        n = len(me['rx'])
        master = bool(me['cr1'] & MSTR)
        txe = master or me['tx'] is None
        request.Value = ((1 if n else 0) | ((1 << 1) if txe else 0)
                         | ((1 << 6) if me['ovr'] else 0) | (min(n, 3) << 9))
        me['ovr'] = False                     # DR read then SR read clears OVR
    elif off == DR:
        request.Value = me['rx'].pop(0) if me['rx'] else 0
        spi_irq(me)
    elif off == CR1:
        request.Value = me['cr1']
    elif off == CR2:
        request.Value = me['cr2']
    else:
        request.Value = me['regs'].get(off, 0)
