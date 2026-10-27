#!/usr/bin/env python3
"""
caOS Example 80 - HTTP server + CLI via HTTP, over USB-NCM

NCM variant of examples/example_45/http_test.py. Starts DHCP (dnsmasq)
on the USB-NCM interface, detects the IP assigned to the board and lets you:
  - open a browser on the board page
  - query /status (JSON)
  - run CLI commands via POST /cli

Usage:
    sudo python3 http_test_ncm.py                       # auto-detect + browser
    sudo python3 http_test_ncm.py --cmd "top"           # one CLI command and nothing else
    sudo python3 http_test_ncm.py --status              # /status only
    sudo python3 http_test_ncm.py --no-browser          # DHCP, then stays waiting
    sudo python3 http_test_ncm.py --host 192.168.10.180 --no-dhcp

Note:
    - Requires: dnsmasq, ip (iproute2)
    - Automatic cleanup on Ctrl+C or SIGTERM

POWER-UP ORDER: the BOARD first, then this script.
The USB interface does not exist until the device has enumerated: it is not like
eth0, which is always there. Without --iface the script looks it up by the
host-side MAC announced in the NCM descriptors (02:ca:05:*) and waits up to 30s
for it to appear.
"""

import argparse
import os
import signal
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import webbrowser

DHCP_START = "192.168.10.100"
DHCP_END   = "192.168.10.200"
MASK       = "255.255.255.0"
DHCP_CONF  = "/tmp/dnsmasq-caos80.conf"
DHCP_PID   = "/tmp/dnsmasq-caos80.pid"
DHCP_LOG   = "/tmp/dnsmasq-caos80.log"

# --------------------------------------------------------------------------
# Interface
# --------------------------------------------------------------------------

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

NCM_MAC_PREFIX = "02:ca:05"   # host-side MAC announced by the NCM descriptors


def find_ncm_iface(timeout_s=30):
    """Look up the USB-NCM interface by MAC, waiting for it to appear.

    The interface comes up with USB enumeration, so it may not be there yet when
    the script starts: retry for timeout_s seconds.
    """
    net_dir = "/sys/class/net"
    for _ in range(timeout_s):
        try:
            for name in os.listdir(net_dir):
                if name == "lo":
                    continue
                try:
                    with open(os.path.join(net_dir, name, "address")) as f:
                        if f.read().strip().lower().startswith(NCM_MAC_PREFIX):
                            return name
                except OSError:
                    continue
        except OSError:
            pass
        time.sleep(1)
    return None


def iface_exists(iface):
    return subprocess.run(
        ["ip", "link", "show", iface],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0

# --------------------------------------------------------------------------
# DHCP (dnsmasq)
# --------------------------------------------------------------------------

_original_cidr  = None
_iface_ip_added = False

def setup_interface(iface, pc_ip):
    global _iface_ip_added, _original_cidr
    current_cidr = get_iface_cidr(iface)
    if current_cidr and current_cidr.split("/")[0] == pc_ip:
        print(f"[*] {iface} already configured with {pc_ip}")
        return pc_ip
    if current_cidr:
        _original_cidr = current_cidr
        print(f"[*] {iface} has {current_cidr} - replacing with {pc_ip}/24")
    subprocess.run(["ip", "addr", "flush", "dev", iface], stderr=subprocess.DEVNULL)
    subprocess.run(["ip", "addr", "add", f"{pc_ip}/24", "dev", iface], check=True)
    subprocess.run(["ip", "link", "set", iface, "up"], check=True)
    _iface_ip_added = True
    print(f"[*] {iface} configured -> {pc_ip}/24")
    return pc_ip

def start_dnsmasq(iface, pc_ip):
    conf = (
        f"# dnsmasq caOS EX80\n"
        f"interface={iface}\n"
        f"bind-interfaces\n"
        f"except-interface=lo\n"
        f"dhcp-range={DHCP_START},{DHCP_END},{MASK},1h\n"
        f"dhcp-option=option:router,{pc_ip}\n"
        f"dhcp-option=option:dns-server,8.8.8.8\n"
        f"dhcp-option=option:netmask,{MASK}\n"
        f"log-dhcp\n"
        f"no-resolv\n"
        f"no-hosts\n"
    )
    with open(DHCP_CONF, "w") as f:
        f.write(conf)

    subprocess.run(["pkill", "-f", "dnsmasq-caos80"], stderr=subprocess.DEVNULL)
    for p in [DHCP_PID, DHCP_LOG]:
        try:
            os.remove(p)
        except FileNotFoundError:
            pass
    time.sleep(0.4)

    subprocess.run(
        ["/usr/sbin/dnsmasq", "-C", DHCP_CONF,
         f"--pid-file={DHCP_PID}",
         f"--log-facility={DHCP_LOG}"],
        check=True)
    time.sleep(0.5)

    if not os.path.exists(DHCP_PID):
        print(f"[!] dnsmasq did not start - see {DHCP_LOG}")
        try:
            print(open(DHCP_LOG).read())
        except Exception:
            pass
        sys.exit(1)

    pid = open(DHCP_PID).read().strip()
    print(f"[*] dnsmasq started (pid {pid})")
    print(f"[*] DHCP log: tail -f {DHCP_LOG}")

def stop_dnsmasq():
    if os.path.exists(DHCP_PID):
        try:
            pid = int(open(DHCP_PID).read().strip())
            os.kill(pid, signal.SIGTERM)
            print(f"[*] dnsmasq stopped (pid {pid})")
        except Exception:
            pass
        for p in [DHCP_PID, DHCP_CONF]:
            try:
                os.remove(p)
            except Exception:
                pass
    else:
        subprocess.run(["pkill", "-f", "dnsmasq-caos80"], stderr=subprocess.DEVNULL)

def wait_board_ip(timeout_s=60):
    """Wait for the first DHCP ACK in the dnsmasq log and return the board IP."""
    print(f"[*] Waiting for DHCP ACK from the board (max {timeout_s}s)...")
    for i in range(timeout_s):
        time.sleep(1)
        try:
            log = open(DHCP_LOG).read()
            for line in log.splitlines():
                if "DHCPACK" in line:
                    # es: "DHCPACK(enp7s0) 192.168.10.146 02:00:..."
                    parts = line.split()
                    for p in parts:
                        if p.count(".") == 3 and not p.startswith("("):
                            return p
        except FileNotFoundError:
            pass
        print(f"\r[*] Waiting for DHCP... {i+1}s", end="", flush=True)
    print()
    return None

# --------------------------------------------------------------------------
# Global cleanup
# --------------------------------------------------------------------------

_cleanup_done = False

def cleanup(iface, pc_ip, with_dhcp):
    global _cleanup_done
    if _cleanup_done:
        return
    _cleanup_done = True
    print("\n[*] Cleanup...")
    if with_dhcp:
        stop_dnsmasq()
    if _iface_ip_added:
        subprocess.run(["ip", "addr", "flush", "dev", iface], stderr=subprocess.DEVNULL)
        if _original_cidr:
            subprocess.run(
                ["ip", "addr", "add", _original_cidr, "dev", iface],
                stderr=subprocess.DEVNULL)
            subprocess.run(["ip", "link", "set", iface, "up"], stderr=subprocess.DEVNULL)
            print(f"[*] {iface}: restored {_original_cidr}")
        else:
            print(f"[*] {iface}: IP removed.")
    print("[*] Done.")

# --------------------------------------------------------------------------
# Echo client
# --------------------------------------------------------------------------


# --------------------------------------------------------------------------
# HTTP
# --------------------------------------------------------------------------

def http_get(url, timeout=5):
    try:
        with urllib.request.urlopen(url, timeout=timeout) as r:
            return r.status, r.read().decode(errors="replace")
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode(errors="replace")
    except Exception as e:
        return None, str(e)


def http_post_cli(base, cmd, timeout=8):
    data = urllib.parse.urlencode({"cmd": cmd}).encode()
    try:
        req = urllib.request.Request(base + "/cli", data=data, method="POST")
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, r.read().decode(errors="replace")
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode(errors="replace")
    except Exception as e:
        return None, str(e)


def wait_http_ready(base, timeout_s=30):
    """The board opens the HTTP listener inside on_dhcp_bound, so right after the
    lease: retry here until port 80 actually answers."""
    for _ in range(timeout_s):
        st, _body = http_get(base + "/status", timeout=2)
        if st == 200:
            return True
        time.sleep(1)
    return False


def run_checks(base):
    """Non-interactive check over the four routes."""
    ok = True

    print(f"\n[1] GET {base}/status")
    st, body = http_get(base + "/status")
    print(f"    -> {st}  {body.strip()}")
    ok &= (st == 200 and '"ip"' in body)

    print(f"\n[2] GET {base}/")
    st, body = http_get(base + "/")
    head = body.strip().splitlines()[0][:70] if body else ""
    print(f"    -> {st}  {len(body)} byte  {head}")
    ok &= (st == 200 and "caOS" in body)

    print(f"\n[3] GET {base}/help")
    st, body = http_get(base + "/help")
    nlines = len(body.strip().splitlines())
    print(f"    -> {st}  {nlines} help lines")
    ok &= (st == 200 and nlines > 1)

    print(f"\n[4] POST {base}/cli  cmd=free")
    st, body = http_post_cli(base, "free")
    print(f"    -> {st}")
    for line in body.strip().splitlines()[:6]:
        print(f"       {line}")
    ok &= (st == 200 and body.strip() != "" and "timeout" not in body[:20])

    print("\n" + "=" * 60)
    print(" RESULT:", "PASS" if ok else "FAIL")
    print("=" * 60)
    return ok


# --------------------------------------------------------------------------
# Main
# --------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description="caOS ex80 - HTTP server su USB-NCM")
    ap.add_argument("--iface", help="NCM interface (default: auto-detect from MAC 02:ca:05)")
    ap.add_argument("--pc-ip", default="192.168.10.1", help="PC IP on the NCM iface")
    ap.add_argument("--host", help="board IP (skips waiting for the DHCP lease)")
    ap.add_argument("--port", type=int, default=80, help="board HTTP port")
    ap.add_argument("--no-dhcp", action="store_true", help="do not start dnsmasq")
    ap.add_argument("--no-browser", action="store_true", help="do not open the browser")
    ap.add_argument("--cmd", help="run a single CLI command via POST /cli and exit")
    ap.add_argument("--status", action="store_true", help="print /status only and exit")
    args = ap.parse_args()

    with_dhcp = not args.no_dhcp
    if with_dhcp and os.geteuid() != 0:
        print("[!] DHCP requires root: use sudo (or --no-dhcp --host <ip>)")
        sys.exit(1)

    if with_dhcp and not args.iface:
        print("[*] Looking for the USB-NCM interface (MAC 02:ca:05:*)...")
        args.iface = find_ncm_iface()
        if not args.iface:
            print("[!] No NCM interface found within 30s.")
            print("    Is the board powered and enumerated? (dmesg | tail)")
            sys.exit(1)
        print(f"[*] NCM interface: {args.iface}")

    actual_pc_ip = args.pc_ip
    board_ip     = args.host

    # The signal handlers go in BEFORE start_dnsmasq: a Ctrl+C
    # during startup would otherwise leave dnsmasq alive and the IP taken.
    def _sig_handler(signum, frame):
        cleanup(args.iface or "", actual_pc_ip, with_dhcp)
        sys.exit(0)

    signal.signal(signal.SIGINT,  _sig_handler)
    signal.signal(signal.SIGTERM, _sig_handler)

    if args.iface:
        if not iface_exists(args.iface):
            print(f"[!] Interface '{args.iface}' not found.")
            sys.exit(1)
        actual_pc_ip = setup_interface(args.iface, args.pc_ip)
        if with_dhcp:
            start_dnsmasq(args.iface, actual_pc_ip)

    if board_ip is None:
        board_ip = wait_board_ip(timeout_s=60)
        if board_ip is None:
            print("[!] No DHCP ACK in 60s. Enter the IP by hand:")
            board_ip = input("    IP board: ").strip()

    base = f"http://{board_ip}:{args.port}" if args.port != 80 else f"http://{board_ip}"

    print("=" * 60)
    print(f" caOS EX80 - HTTP server su USB-NCM  ->  {base}/")
    print("=" * 60)

    try:
        if not wait_http_ready(base):
            print(f"[!] {base}/status is not answering.")
            print("    The board opens the HTTP listener only after the DHCP lease.")
            sys.exit(1)

        if args.status:
            st, body = http_get(base + "/status")
            print(f"{st}  {body}")
        elif args.cmd:
            st, body = http_post_cli(base, args.cmd)
            print(f"[{st}] {args.cmd}:")
            print(body)
        else:
            run_checks(base)
            if not args.no_browser:
                print(f"\n[*] Opening the browser on {base}/")
                webbrowser.open(base + "/")
                print("[*] Ctrl+C to stop (DHCP stays up).")
                while True:
                    time.sleep(1)
    finally:
        cleanup(args.iface or "", actual_pc_ip, with_dhcp)


if __name__ == "__main__":
    main()
