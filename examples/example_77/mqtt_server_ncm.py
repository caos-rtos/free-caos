#!/usr/bin/env python3
"""
caOS Example 77 - PC MQTT broker + DHCP server, over USB-NCM

Starts mosquitto as an MQTT broker and a paho-mqtt client to:
  - print the messages received on caos/status
  - send LED commands on caos/led ("on" / "off")

DHCP (dnsmasq) assigns the board an IP on the same subnet as the broker.

Usage:
    sudo python3 mqtt_server_ncm.py                     # auto-detect usb0
    sudo python3 mqtt_server_ncm.py --iface usb0        # explicit interface
    sudo python3 mqtt_server_ncm.py --no-dhcp           # broker only

POWER-UP ORDER: the BOARD first, then this script.
The USB interface does not exist until the device has enumerated: it is not like
eth0, which is always there. Without --iface the script looks it up by the
host-side MAC announced in the NCM descriptors (02:ca:05:*) and waits up to 30s
for it to appear.

Dependencies:
    apt install mosquitto dnsmasq
    pip3 install paho-mqtt

Interactive commands (once running):
    on     -> publish "on"  on caos/led
    off    -> publish "off" on caos/led
    q      -> quit
"""

import argparse
import os
import queue
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time

try:
    import paho.mqtt.client as mqtt_client
except ImportError:
    # Under sudo, root's Python does not see the user's ~/.local: the package
    # may already be installed and still look missing here.
    _user = os.environ.get("SUDO_USER")
    if _user:
        _sp = os.path.expanduser(
            f"~{_user}/.local/lib/python{sys.version_info.major}."
            f"{sys.version_info.minor}/site-packages")
        if os.path.isdir(os.path.join(_sp, "paho")):
            sys.path.insert(0, _sp)
    try:
        import paho.mqtt.client as mqtt_client
    except ImportError:
        print("[!] paho-mqtt not found.")
        if _user:
            print(f"    Installed for the user but not for root? Retry with:")
            print(f"    sudo PYTHONPATH={_sp} {' '.join(sys.argv)}")
        else:
            print("    pip3 install paho-mqtt")
        sys.exit(1)

DHCP_START = "192.168.10.100"
DHCP_END   = "192.168.10.200"
MASK       = "255.255.255.0"
DHCP_CONF  = "/tmp/dnsmasq-caos77.conf"
DHCP_PID   = "/tmp/dnsmasq-caos77.pid"
DHCP_LOG   = "/tmp/dnsmasq-caos77.log"
MOSQ_CONF  = "/tmp/mosquitto-caos77.conf"
MOSQ_LOG   = "/tmp/mosquitto-caos77.log"
MOSQ_PID   = "/tmp/mosquitto-caos77.pid"

TOPIC_STATUS = "caos/status"
TOPIC_LED    = "caos/led"
BROKER_PORT  = 1883

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
# Interface IP
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

# --------------------------------------------------------------------------
# DHCP (dnsmasq)
# --------------------------------------------------------------------------

def start_dnsmasq(iface, pc_ip):
    conf = (
        f"# dnsmasq caOS EX77\n"
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

    subprocess.run(["pkill", "-f", "dnsmasq-caos77"], stderr=subprocess.DEVNULL)
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
        subprocess.run(["pkill", "-f", "dnsmasq-caos77"], stderr=subprocess.DEVNULL)
    for p in [DHCP_PID, DHCP_CONF]:
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

# --------------------------------------------------------------------------
# mosquitto broker
# --------------------------------------------------------------------------

def start_mosquitto(pc_ip):
    conf = (
        f"# mosquitto caOS EX77\n"
        f"listener {BROKER_PORT} {pc_ip}\n"
        f"allow_anonymous true\n"
        f"log_dest file {MOSQ_LOG}\n"
    )
    with open(MOSQ_CONF, "w") as f:
        f.write(conf)

    subprocess.run(["pkill", "-f", "mosquitto-caos77"], stderr=subprocess.DEVNULL)
    for p in [MOSQ_PID, MOSQ_LOG]:
        try: os.remove(p)
        except FileNotFoundError: pass
    time.sleep(0.3)

    if shutil.which("mosquitto") is None:
        print("[!] 'mosquitto' not installed:  sudo apt install mosquitto")
        print("    Then disable the system service (it holds 1883):")
        print("    sudo systemctl disable --now mosquitto")
        sys.exit(1)

    # start mosquitto in the background
    proc = subprocess.Popen(
        ["mosquitto", "-c", MOSQ_CONF],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL)
    time.sleep(0.8)

    if proc.poll() is not None:
        print(f"[!] mosquitto did not start - see {MOSQ_LOG}")
        try: log = open(MOSQ_LOG).read()
        except Exception: log = ""
        print(log)
        if "Address already in use" in log:
            print(f"    Port {BROKER_PORT} is already taken, usually by the")
            print("    system service:  sudo systemctl disable --now mosquitto")
        sys.exit(1)

    print(f"[*] mosquitto started (pid {proc.pid})  {pc_ip}:{BROKER_PORT}")
    print(f"[*] Broker log: tail -f {MOSQ_LOG}")
    return proc

def stop_mosquitto(proc):
    if proc and proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
        print(f"[*] mosquitto stopped")
    for p in [MOSQ_CONF, MOSQ_LOG]:
        try: os.remove(p)
        except Exception: pass

# --------------------------------------------------------------------------
# MQTT client (paho)
# --------------------------------------------------------------------------

_pub_client = None

def on_connect(client, userdata, flags, rc):
    if rc == 0:
        print(f"[*] Paho connected to the broker")
        client.subscribe(TOPIC_STATUS)
        print(f"[*] Subscribed to {TOPIC_STATUS}")
    else:
        print(f"[!] Paho connect failed rc={rc}")

def on_message(client, userdata, msg):
    payload = msg.payload.decode(errors="replace")
    print(f"  [{msg.topic}] {payload}")

def start_paho_client(broker_ip):
    global _pub_client
    c = mqtt_client.Client(client_id="caos-pc-77")
    c.on_connect = on_connect
    c.on_message = on_message
    c.connect(broker_ip, BROKER_PORT, keepalive=60)
    c.loop_start()
    _pub_client = c
    return c

def stop_paho_client(c):
    if c:
        c.loop_stop()
        c.disconnect()
    _pub_client = None

def publish_led(state):
    if _pub_client is None:
        print("[!] MQTT client not connected")
        return
    _pub_client.publish(TOPIC_LED, payload=state, qos=0)
    print(f"  -> [{TOPIC_LED}] {state}")

# --------------------------------------------------------------------------
# Global cleanup
# --------------------------------------------------------------------------

_cleanup_done = False
_mosq_proc    = None
_paho_client  = None

def cleanup(iface, pc_ip, with_dhcp):
    global _cleanup_done
    if _cleanup_done:
        return
    _cleanup_done = True
    print("\n[*] Cleanup...")
    # Each step isolated: one failure must not block the next ones, otherwise
    # dnsmasq stays alive holding the IP for the next example (Address in use).
    for _step in (lambda: stop_paho_client(_paho_client),
                  lambda: stop_mosquitto(_mosq_proc),
                  stop_dnsmasq if with_dhcp else None):
        if _step is None:
            continue
        try:
            _step()
        except Exception as e:
            print(f"[!] cleanup: {e}")
    if _iface_ip_added:
        subprocess.run(["ip", "addr", "flush", "dev", iface], stderr=subprocess.DEVNULL)
        if _original_cidr:
            subprocess.run(["ip", "addr", "add", _original_cidr, "dev", iface],
                           stderr=subprocess.DEVNULL)
            subprocess.run(["ip", "link", "set", iface, "up"], stderr=subprocess.DEVNULL)
            print(f"[*] {iface}: restored {_original_cidr}")
        else:
            print(f"[*] {iface}: IP removed (it was absent before).")
    print("[*] Done.")

# --------------------------------------------------------------------------
# Main
# --------------------------------------------------------------------------

def main():
    global _mosq_proc, _paho_client

    parser = argparse.ArgumentParser(
        description="caOS EX77 - MQTT broker + DHCP",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    parser.add_argument("--iface",   type=str, default=None,
                        help="Network interface (default: auto-detect NCM)")
    parser.add_argument("--pc-ip",   type=str, default="192.168.10.1",
                        help="PC IP to configure on the interface")
    parser.add_argument("--no-dhcp", action="store_true",
                        help="Disable the DHCP server (broker only)")
    args = parser.parse_args()

    # Auto-detect the NCM interface when not given. Not for --no-dhcp:
    # there the broker listens anyway and the interface is not needed.
    if args.iface is None and not args.no_dhcp:
        print(f"[*] Looking for the USB-NCM interface (MAC {NCM_MAC_PREFIX}:*)...")
        args.iface = find_ncm_iface()
        if args.iface is None:
            print("[!] No NCM interface found within 30s.")
            print("    Plug the USB cable and run the firmware, or")
            print("    pass the interface by hand:  sudo %s --iface <iface>"
                  % os.path.basename(sys.argv[0]))
            print("    (list them: ip -br link)")
            sys.exit(1)
        print(f"[+] NCM interface: {args.iface}")

    with_dhcp    = bool(args.iface) and not args.no_dhcp
    actual_pc_ip = args.pc_ip

    # Handlers installed BEFORE touching interface and dnsmasq: if cleanup were
    # hooked only afterwards, a Ctrl+C (or an exception) during startup would
    # leave dnsmasq alive holding the IP, and the example
    # next one would fail with 'Address already in use'.
    def _sig_handler(sig, frame):
        cleanup(args.iface or "", actual_pc_ip, with_dhcp)
        sys.exit(0)

    signal.signal(signal.SIGINT,  _sig_handler)
    signal.signal(signal.SIGTERM, _sig_handler)

    # Reap a leftover dnsmasq from a previous run that ended
    # badly: without this the IP stays taken and the bind fails with
    # 'Address already in use'.
    if with_dhcp:
        stop_dnsmasq()

    board_ip = None

    if args.iface:
        if not iface_exists(args.iface):
            print(f"[!] Interface '{args.iface}' not found.")
            sys.exit(1)
        actual_pc_ip = setup_interface(args.iface, args.pc_ip)
        if with_dhcp:
            start_dnsmasq(args.iface, actual_pc_ip)
            board_ip = wait_board_ip(timeout_s=60)
            if board_ip is None:
                print("[!] DHCP timeout - the board is not answering. Exiting.")
                stop_dnsmasq()
                sys.exit(1)
            print(f"[*] Board IP: {board_ip}")
    elif not args.no_dhcp:
        print("[!] Pass --iface to start the DHCP server.")
        print("    Or use --no-dhcp for a broker on localhost only.")
        sys.exit(1)

    _mosq_proc   = start_mosquitto(actual_pc_ip)
    time.sleep(0.5)
    _paho_client = start_paho_client(actual_pc_ip)
    time.sleep(0.5)


    print("=" * 60)
    print(f" caOS EX77 - MQTT broker  {actual_pc_ip}:{BROKER_PORT}")
    if board_ip:
        print(f" Board IP:  {board_ip}")
    print(f" Subscribe: {TOPIC_STATUS}")
    print(f" Publish:   {TOPIC_LED}")
    if with_dhcp:
        print(f" DHCP pool  {DHCP_START} - {DHCP_END}")
    print("=" * 60)
    print(" Commands: on | off | q")
    print("-" * 60)

    try:
        while True:
            try:
                cmd = input("> ").strip().lower()
            except EOFError:
                break
            if cmd in ("on", "off"):
                publish_led(cmd)
            elif cmd == "q":
                break
            elif cmd == "":
                pass
            else:
                print("  Valid commands: on | off | q")
    finally:
        cleanup(args.iface or "", actual_pc_ip, with_dhcp)


if __name__ == "__main__":
    main()
