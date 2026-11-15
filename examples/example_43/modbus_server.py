#!/usr/bin/env python3
"""
modbus_server.py - simulated Modbus TCP server for caOS example 43

Simulates a Modbus TCP device with:
  - 8  coils       (addr 0..7)   - read FC01
  - 10 holding registers (addr 0..9) - read FC03, write FC06/FC16

Usage:
  sudo python3 modbus_server.py --iface enp7s0 [--pc-ip 192.168.10.1]
  sudo python3 modbus_server.py --iface enp7s0 --no-dhcp  # IP already configured

Fault injection (test bench only; the server is normal until told otherwise):
  A UDP control socket on 127.0.0.1:5502 (--ctl-port, 0 = off) takes one text
  command per datagram and answers with one line:
      ping | status | reset | thaw | disarm | arm <mode>
  `arm` loads ONE fault that hits the next matching request only and then
  disarms itself, so a broken phase cannot poison the next ones. Every applied
  fault is logged as "[FAULT] applied <mode> tid=N fc=0xNN addr=N".
  `reset` restores the demo register values, freezes the 5 s updater and
  disarms; `thaw` lets the updater run again. Modes (FC / start address hit):
      echo_value   FC06 @10   write answer echoes value+1
      echo_addr    FC06 @10   write answer echoes address+1
      fc16_addr    FC16 any   write answer echoes address+1
      fc16_qty     FC16 any   write answer echoes quantity+1
      unit         FC03 @8    answer carries unit id+1
      pid          FC03 @8    answer carries protocol id 1
      partial      FC03 @9    first 4 bytes of the MBAP, then the link is closed
      stale        FC03 @8    stale-tid answer (data 0xDEAD) + right answer, ONE send
      len_regs     FC03 @8    MBAP length one more than the byte count (1 pad byte)
      len_coils    FC01 @8    same for the coil read
  The addresses are outside the board's own poll cycle (FC03@0, FC06@4,
  FC16@5, FC01@0), which therefore never consumes a fault by accident.

Dependencies:
  apt install dnsmasq
"""

import argparse
import atexit
import os
import signal
import socket
import struct
import subprocess
import sys
import threading
import time

# Every log line carries a wall-clock timestamp in milliseconds.
import builtins as _builtins
def print(*a, **k):
    t = time.time()
    stamp = time.strftime("%H:%M:%S", time.localtime(t)) + ".%03d" % int((t % 1) * 1000)
    k.setdefault("flush", True)
    _builtins.print(f"[{stamp}]", *a, **k)

# ---------------------------------------------------------------------------
# DHCP / network
# ---------------------------------------------------------------------------
DHCP_START = "192.168.10.100"
DHCP_END   = "192.168.10.200"
MASK       = "255.255.255.0"
DHCP_CONF  = "/tmp/dnsmasq-caos43.conf"
DHCP_PID   = "/tmp/dnsmasq-caos43.pid"
DHCP_LOG   = "/tmp/dnsmasq-caos43.log"

def get_iface_cidr(iface):
    try:
        out = subprocess.check_output(
            ["ip", "-4", "addr", "show", "dev", iface],
            stderr=subprocess.DEVNULL).decode()
        for line in out.splitlines():
            s = line.strip()
            if s.startswith("inet "):
                return s.split()[1]
    except Exception:
        pass
    return None

def setup_interface(iface, pc_ip):
    current_cidr = get_iface_cidr(iface)
    if current_cidr and current_cidr.split("/")[0] == pc_ip:
        print(f"[*] {iface} already configured with {pc_ip}")
        return
    if current_cidr:
        print(f"[*] {iface} has {current_cidr} - replacing with {pc_ip}/24")
    subprocess.run(["ip", "addr", "flush", "dev", iface], stderr=subprocess.DEVNULL)
    subprocess.run(["ip", "addr", "add", f"{pc_ip}/24", "dev", iface], check=True)
    subprocess.run(["ip", "link", "set", iface, "up"], check=True)
    print(f"[*] {iface} configured -> {pc_ip}/24")

def start_dnsmasq(iface, pc_ip):
    conf = (
        f"interface={iface}\n"
        f"bind-interfaces\n"
        f"except-interface=lo\n"
        f"dhcp-range={DHCP_START},{DHCP_END},{MASK},1h\n"
        f"dhcp-option=option:router,{pc_ip}\n"
        f"dhcp-option=option:dns-server,8.8.8.8\n"
        f"dhcp-option=option:netmask,{MASK}\n"
        f"no-resolv\n"
        f"no-hosts\n"
    )
    with open(DHCP_CONF, "w") as f:
        f.write(conf)

    subprocess.run(["pkill", "-f", "dnsmasq-caos43"], stderr=subprocess.DEVNULL)
    for p in [DHCP_PID, DHCP_LOG]:
        try: os.remove(p)
        except FileNotFoundError: pass
    time.sleep(0.4)

    subprocess.run(
        ["/usr/sbin/dnsmasq", "-C", DHCP_CONF,
         f"--pid-file={DHCP_PID}",
         f"--log-facility={DHCP_LOG}"],
        check=True)
    time.sleep(0.5)

    if not os.path.exists(DHCP_PID):
        print(f"[!] dnsmasq did not start - see {DHCP_LOG}")
        try: print(open(DHCP_LOG).read())
        except Exception: pass
        sys.exit(1)

    pid = open(DHCP_PID).read().strip()
    print(f"[*] dnsmasq started (pid {pid})")
    # Stopped on EVERY exit: a dnsmasq that outlived its script once grew its
    # log to 4.2 GB and filled the root disk (ex44, 28/10)
    atexit.register(stop_dnsmasq)
    for _s in (signal.SIGTERM, signal.SIGHUP):
        if signal.getsignal(_s) == signal.SIG_DFL:
            signal.signal(_s, lambda n, f: sys.exit(128 + n))
    print(f"[*] DHCP pool {DHCP_START} - {DHCP_END}")
    print(f"[*] DHCP log: tail -f {DHCP_LOG}")

def stop_dnsmasq():
    if os.path.exists(DHCP_PID):
        try:
            pid = int(open(DHCP_PID).read().strip())
            os.kill(pid, signal.SIGTERM)
            print(f"[*] dnsmasq stopped (pid {pid})")
        except Exception:
            pass
    else:
        subprocess.run(["pkill", "-f", "dnsmasq-caos43"], stderr=subprocess.DEVNULL)
    for p in [DHCP_PID, DHCP_CONF, DHCP_LOG]:
        try: os.remove(p)
        except Exception: pass

def wait_board_ip(timeout_s=60):
    """Wait for the first DHCP ACK in the dnsmasq log and return the board IP."""
    print(f"[*] Waiting for DHCP ACK from the board (max {timeout_s}s)...")
    for i in range(timeout_s):
        time.sleep(1)
        try:
            log = open(DHCP_LOG).read()
            for line in log.splitlines():
                if "DHCPACK" in line:
                    parts = line.split()
                    for p in parts:
                        if p.count(".") == 3 and not p.startswith("("):
                            print()
                            return p
        except FileNotFoundError:
            pass
        print(f"\r[*] Waiting for DHCP... {i+1}s", end="", flush=True)
    print()
    return None

# ---------------------------------------------------------------------------
# Simulated device state
# ---------------------------------------------------------------------------
coils    = [False] * 16
hold_reg = [0] * 16

def _init_demo_values():
    for i in range(len(hold_reg)):
        hold_reg[i] = 100 * (i + 1)
    for i in range(len(coils)):
        coils[i] = bool(i % 2)

# ---------------------------------------------------------------------------
# MBAP helpers
# ---------------------------------------------------------------------------
MBAP_LEN = 6

def parse_mbap(data):
    if len(data) < MBAP_LEN:
        return None
    tid, proto, length = struct.unpack('>HHH', data[:MBAP_LEN])
    return tid, proto, length

# ---------------------------------------------------------------------------
# PDU handlers
# ---------------------------------------------------------------------------
def pdu_read_coils(addr, count):
    if addr + count > len(coils) or count < 1 or count > 2000:
        return bytes([0x81, 0x02])
    n_bytes = (count + 7) // 8
    data = bytearray(n_bytes)
    for i in range(count):
        if coils[addr + i]:
            data[i // 8] |= (1 << (i % 8))
    return bytes([0x01, n_bytes]) + bytes(data)

def pdu_read_holding_regs(addr, count):
    if addr + count > len(hold_reg) or count < 1 or count > 125:
        return bytes([0x83, 0x02])
    byte_count = count * 2
    data = bytearray(byte_count)
    for i in range(count):
        struct.pack_into('>H', data, i * 2, hold_reg[addr + i])
    return bytes([0x03, byte_count]) + bytes(data)

def pdu_write_single_reg(addr, value):
    if addr >= len(hold_reg):
        return bytes([0x86, 0x02])
    hold_reg[addr] = value
    print(f"  [WR] reg[{addr}] = {value}")
    return bytes([0x06]) + struct.pack('>HH', addr, value)

def pdu_write_multiple_regs(addr, count, values):
    if addr + count > len(hold_reg) or count < 1 or count > 123:
        return bytes([0x90, 0x02])
    for i, v in enumerate(values):
        hold_reg[addr + i] = v
    print(f"  [WR] reg[{addr}..{addr+count-1}] = {values}")
    return bytes([0x10]) + struct.pack('>HH', addr, count)

def process_pdu(unit_id, pdu):
    if not pdu:
        return None
    fc = pdu[0]
    if fc == 0x01 and len(pdu) >= 5:
        addr, count = struct.unpack('>HH', pdu[1:5])
        resp = pdu_read_coils(addr, count)
    elif fc == 0x03 and len(pdu) >= 5:
        addr, count = struct.unpack('>HH', pdu[1:5])
        resp = pdu_read_holding_regs(addr, count)
    elif fc == 0x06 and len(pdu) >= 5:
        addr, value = struct.unpack('>HH', pdu[1:5])
        resp = pdu_write_single_reg(addr, value)
    elif fc == 0x10 and len(pdu) >= 6:
        addr, count = struct.unpack('>HH', pdu[1:5])
        byte_count = pdu[5]
        values = [struct.unpack('>H', pdu[6+i*2:8+i*2])[0] for i in range(count)]
        resp = pdu_write_multiple_regs(addr, count, values)
    else:
        resp = bytes([fc | 0x80, 0x01])
    return bytes([unit_id]) + resp

# ---------------------------------------------------------------------------
# Fault injection (one-shot, driven from the control socket)
# ---------------------------------------------------------------------------
FAULTS = {
    #  mode        (fc,   start address or None = any)
    'echo_value': (0x06, 10),
    'echo_addr':  (0x06, 10),
    'fc16_addr':  (0x10, None),
    'fc16_qty':   (0x10, None),
    'unit':       (0x03, 8),
    'pid':        (0x03, 8),
    'partial':    (0x03, 9),
    'stale':      (0x03, 8),
    'len_regs':   (0x03, 8),
    'len_coils':  (0x01, 8),
}
fault_lock = threading.Lock()
fault_armed = None          # mode name or None
fault_applied = 0
updater_frozen = threading.Event()

def take_fault(fc, addr):
    """Return the armed mode if it targets this request, and disarm it."""
    global fault_armed, fault_applied
    with fault_lock:
        if fault_armed is None:
            return None
        want_fc, want_addr = FAULTS[fault_armed]
        if fc != want_fc or (want_addr is not None and addr != want_addr):
            return None
        mode, fault_armed = fault_armed, None
        fault_applied += 1
        return mode

def bump16(pdu, off):
    """Return pdu with the big-endian u16 at `off` incremented by one."""
    v = (struct.unpack_from('>H', pdu, off)[0] + 1) & 0xFFFF
    b = bytearray(pdu)
    struct.pack_into('>H', b, off, v)
    return bytes(b)

def build_faulty(mode, tid, resp_pdu):
    """Bytes to put on the wire for the armed fault. resp_pdu = unit+FC+data."""
    def mbap(t, proto, n):
        return struct.pack('>HHH', t, proto, n)
    if mode in ('echo_value', 'fc16_qty'):
        resp_pdu = bump16(resp_pdu, 4)
    elif mode in ('echo_addr', 'fc16_addr'):
        resp_pdu = bump16(resp_pdu, 2)
    if mode == 'unit':
        resp_pdu = bytes([(resp_pdu[0] + 1) & 0xFF]) + resp_pdu[1:]
    if mode == 'pid':
        return mbap(tid, 1, len(resp_pdu)) + resp_pdu
    if mode in ('len_regs', 'len_coils'):
        return mbap(tid, 0, len(resp_pdu) + 1) + resp_pdu + b'\x00'
    if mode == 'stale':
        stale = resp_pdu[:3] + b'\xDE\xAD' * ((len(resp_pdu) - 3) // 2)
        return (mbap((tid - 1) & 0xFFFF, 0, len(stale)) + stale +
                mbap(tid, 0, len(resp_pdu)) + resp_pdu)
    return mbap(tid, 0, len(resp_pdu)) + resp_pdu

def control_command(line):
    global fault_armed
    words = line.split()
    cmd = words[0] if words else ''
    if cmd == 'ping':
        return 'pong'
    if cmd == 'status':
        with fault_lock:
            return f"armed={fault_armed or 'none'} applied={fault_applied} frozen={int(updater_frozen.is_set())}"
    if cmd == 'arm' and len(words) == 2 and words[1] in FAULTS:
        with fault_lock:
            fault_armed = words[1]
        print(f"[FAULT] armed {words[1]}", flush=True)
        return 'OK armed ' + words[1]
    if cmd == 'disarm':
        with fault_lock:
            fault_armed = None
        print("[FAULT] disarmed", flush=True)
        return 'OK disarmed'
    if cmd == 'reset':
        with fault_lock:
            fault_armed = None
        _init_demo_values()
        updater_frozen.set()
        print("[FAULT] reset: demo values restored, updater frozen", flush=True)
        return 'OK reset'
    if cmd == 'thaw':
        updater_frozen.clear()
        return 'OK thawed'
    return 'ERR unknown command'

def control_loop(port):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.bind(('127.0.0.1', port))
    except OSError as e:
        print(f"[!] control socket 127.0.0.1:{port} unavailable: {e}")
        return
    print(f"[*] fault control on udp 127.0.0.1:{port}")
    while True:
        data, peer = sock.recvfrom(256)
        try:
            reply = control_command(data.decode('ascii', 'replace').strip())
        except Exception as e:
            reply = f'ERR {e}'
        sock.sendto(reply.encode(), peer)

# ---------------------------------------------------------------------------
# Client handler
# ---------------------------------------------------------------------------
def handle_client(conn, addr):
    print(f"[+] client {addr[0]}:{addr[1]}")
    buf = b''
    try:
        while True:
            chunk = conn.recv(512)
            if not chunk:
                break
            buf += chunk

            while len(buf) >= MBAP_LEN:
                result = parse_mbap(buf)
                if result is None:
                    buf = b''
                    break
                tid, proto, length = result
                total = MBAP_LEN + length
                if len(buf) < total:
                    break

                frame   = buf[:total]
                buf     = buf[total:]
                unit_id = frame[MBAP_LEN]
                pdu_req = frame[MBAP_LEN + 1:]

                fc = pdu_req[0] if pdu_req else 0
                print(f"  RX tid={tid} unit={unit_id} FC=0x{fc:02X}")

                resp_pdu = process_pdu(unit_id, pdu_req)
                if resp_pdu is None:
                    continue

                addr_req = struct.unpack('>H', pdu_req[1:3])[0] if len(pdu_req) >= 3 else None
                mode = take_fault(fc, addr_req)
                if mode is not None:
                    print(f"[FAULT] applied {mode} tid={tid} fc=0x{fc:02X} addr={addr_req}", flush=True)
                    wire = build_faulty(mode, tid, resp_pdu)
                    if mode == 'partial':
                        conn.sendall(wire[:4])
                        conn.shutdown(socket.SHUT_RDWR)
                        return
                    conn.sendall(wire)
                    continue

                length_field = len(resp_pdu)
                mbap = struct.pack('>HHH', tid, 0, length_field)
                conn.sendall(mbap + resp_pdu)

    except Exception as e:
        print(f"[-] client {addr[0]}:{addr[1]} error: {e}")
    finally:
        conn.close()
        print(f"[-] client {addr[0]}:{addr[1]} closed")

# ---------------------------------------------------------------------------
# Background: update registers and coils every 5 s
# ---------------------------------------------------------------------------
def background_updater():
    seq = 0
    while True:
        time.sleep(5)
        if updater_frozen.is_set():
            continue
        seq += 1
        for i in range(len(hold_reg)):
            hold_reg[i] = (hold_reg[i] + 1) % 65536
        for i in range(len(coils)):
            coils[i] = not coils[i]
        print(f"  [SIM] seq={seq} reg[0]={hold_reg[0]} coils[0]={int(coils[0])}")

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def main():
    parser = argparse.ArgumentParser(description='Modbus TCP server (test caOS ex43)')
    parser.add_argument('--iface',   default='enp7s0',      help='Ethernet interface')
    parser.add_argument('--pc-ip',   default='192.168.10.1', help='PC IP on the link')
    parser.add_argument('--port',    type=int, default=502)
    parser.add_argument('--no-dhcp', action='store_true',    help='skip DHCP/IP setup')
    parser.add_argument('--ctl-port', type=int, default=5502,
                        help='UDP fault-control port on 127.0.0.1 (0 = off)')
    args = parser.parse_args()

    if os.geteuid() != 0:
        print("[!] Requires root (port 502 and dnsmasq).")
        sys.exit(1)

    _init_demo_values()

    if not args.no_dhcp:
        setup_interface(args.iface, args.pc_ip)
        start_dnsmasq(args.iface, args.pc_ip)
        board_ip = wait_board_ip(60)
        if board_ip is None:
            print("[!] The board got no IP - check the cable and retry.")
            stop_dnsmasq()
            sys.exit(1)
        print(f"[*] Board IP: {board_ip}")
    else:
        print("[*] --no-dhcp: skipping DHCP setup")

    print(f"\n{'='*50}")
    print(f"  Modbus TCP server  {args.pc_ip}:{args.port}")
    print(f"  Holding regs: {hold_reg[:8]}")
    print(f"  Coils:        {[int(c) for c in coils[:8]]}")
    print(f"{'='*50}\n")

    upd = threading.Thread(target=background_updater, daemon=True)
    upd.start()
    if args.ctl_port:
        threading.Thread(target=control_loop, args=(args.ctl_port,),
                         daemon=True).start()

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((args.pc_ip, args.port))
    srv.listen(4)
    print(f"Modbus TCP listening on {args.pc_ip}:{args.port}")
    print("Ctrl-C to stop\n")

    try:
        while True:
            conn, addr = srv.accept()
            t = threading.Thread(target=handle_client, args=(conn, addr), daemon=True)
            t.start()
    except KeyboardInterrupt:
        print("\nStop.")
    finally:
        srv.close()
        if not args.no_dhcp:
            stop_dnsmasq()

if __name__ == '__main__':
    main()
