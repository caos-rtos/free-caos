#!/usr/bin/env python3
"""
test_usb_loopback.py - USB bulk loopback test for example_50

Usage:
    sudo python3 test_usb_loopback.py [--packets N] [--size B] [--ep COUNT]

Device:
    VID=0xCA01  PID=0x0001
    EP1..EP8  IN=0x81..0x88  OUT=0x01..0x08  (bulk, 64B FS)
    The actual EP count is read from the device config descriptor
    (H563=up to 7 data, H7=6, U575=5).

Throughput strategy:
    Per EP: an independent writer thread and reader thread.
    The writer sends packets without waiting for the echo (pipeline).
    The reader consumes the echoes and checks the data.
    The per-EP firmware queue (variable depth) absorbs the burst; the writer is
    capped at depth-1 packets in flight so the firmware RX queue never saturates.

Default: 1 EP, 10000 packets x 64B.
"""

import usb.core
import usb.util
import argparse
import sys
import random
import time
import threading
import queue

VID     = 0xCA01
PID     = 0x0001
TIMEOUT = 2000   # ms

# Per-EP in-flight packet bound (corr_q = depth-1). Caps how far the writer can
# get ahead of the reader; with the firmware flow control (per-EP ring) an excess causes
# only NAK+retry, not drop. Index 0 = EP1 ... index 7 = EP8.
# Capped at 8 EPs to cover the H563 (USB_EP_MAX=8 -> EP0 + 7 data).
FW_DEPTH = [64, 64, 32, 32, 32, 32, 32, 32]
MAX_EP   = len(FW_DEPTH)

def disable_autosuspend(dev):
    try:
        path = f"/sys/bus/usb/devices/{dev.bus}-{dev.port_numbers[0]}/power/autosuspend_delay_ms"
        with open(path, "w") as f:
            f.write("-1\n")
    except Exception:
        pass

def find_device(retries=5):
    for attempt in range(retries):
        dev = usb.core.find(idVendor=VID, idProduct=PID)
        if dev is not None:
            print(f"Found: bus {dev.bus} dev {dev.address}")
            return dev
        if attempt < retries - 1:
            time.sleep(0.5)
    print(f"Device {VID:04x}:{PID:04x} not found")
    sys.exit(1)

def flush_ep_in(dev, ep_in, pkt_size, timeout_ms=300):
    """Drain stale data left in the IN pipe by earlier tests."""
    drained = 0
    while True:
        try:
            dev.read(ep_in, pkt_size, timeout_ms)
            drained += 1
        except usb.core.USBTimeoutError:
            break
        except usb.core.USBError:
            break
    if drained:
        print(f"  [flush EP{ep_in & 0x7F}] drained {drained} stale packet(s)")

def ep_worker(dev, ep_idx, num_packets, pkt_size, result):
    """
    Parallel writer and reader on the same EP.
    Writer: sends num_packets without waiting for the echo.
    Reader: reads num_packets and checks them.
    The check uses a Python queue (bounded=16) as a correlation buffer.
    """
    ep_out = ep_idx
    ep_in  = ep_idx | 0x80

    # correlation queue: the writer stores what it sent, the reader takes it
    # back to check. Bound = depth-1 of the EP firmware queue, so the writer
    # never sends more packets than the RX queue
    # firmware can hold (avoids drops on low-depth EPs, e.g. 8).
    depth  = FW_DEPTH[ep_idx - 1] if (ep_idx - 1) < len(FW_DEPTH) else 16
    corr_q = queue.Queue(maxsize=max(1, depth - 1))

    ok    = [0]
    err   = [0]
    writer_exc = [None]
    reader_exc = [None]

    def writer():
        try:
            for _ in range(num_packets):
                data = bytes(random.randint(0, 255) for _ in range(pkt_size))
                dev.write(ep_out, data, TIMEOUT)
                corr_q.put(data)          # store for checking (blocks when full)
        except Exception as e:
            writer_exc[0] = e
            corr_q.put(None)              # unblock the reader on error

    def reader():
        received = 0
        try:
            while received < num_packets:
                expected = corr_q.get()   # wait for data from the writer
                if expected is None:
                    break
                try:
                    rx = bytes(dev.read(ep_in, pkt_size, TIMEOUT))
                    if rx == expected:
                        ok[0] += 1
                    else:
                        err[0] += 1
                        if err[0] <= 3:
                            print(f"[EP{ep_idx}] MISMATCH sent={expected[:4].hex()} got={rx[:4].hex()}")
                except usb.core.USBTimeoutError:
                    err[0] += 1
                    print(f"[EP{ep_idx}] TIMEOUT rx")
                except usb.core.USBError as e:
                    err[0] += 1
                    print(f"[EP{ep_idx}] USBError rx: {e}")
                received += 1
        except Exception as e:
            reader_exc[0] = e

    wt = threading.Thread(target=writer, daemon=True)
    rt = threading.Thread(target=reader, daemon=True)
    wt.start(); rt.start()
    wt.join();  rt.join()

    if writer_exc[0]:
        print(f"[EP{ep_idx}] writer error: {writer_exc[0]}")
    if reader_exc[0]:
        print(f"[EP{ep_idx}] reader error: {reader_exc[0]}")

    result['ok']  = ok[0]
    result['err'] = err[0]

def test_loopback(dev, num_packets, pkt_size, ep_count):
    disable_autosuspend(dev)

    try:
        dev.set_configuration(1)
    except usb.core.USBError as e:
        if e.errno == 5:
            dev = find_device()
            dev.set_configuration(1)
        elif e.errno != 16:
            print(f"set_configuration failed: {e}")
            sys.exit(1)

    print(f"Testing {ep_count} EP x {num_packets} packets x {pkt_size} bytes  [writer+reader paralleli]...")

    # flush stale echoes from the previous test
    for i in range(ep_count):
        flush_ep_in(dev, (i + 1) | 0x80, pkt_size)

    results = [{} for _ in range(ep_count)]
    threads = []

    t0 = time.time()
    for i in range(ep_count):
        t = threading.Thread(target=ep_worker,
                             args=(dev, i + 1, num_packets, pkt_size, results[i]))
        threads.append(t)
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    elapsed = time.time() - t0

    total_ok    = sum(r.get('ok',  0) for r in results)
    total_err   = sum(r.get('err', 0) for r in results)
    total_bytes = total_ok * pkt_size

    print()
    for i, r in enumerate(results):
        ok  = r.get('ok',  0)
        err = r.get('err', 0)
        print(f"  EP{i+1}: {ok}/{num_packets} OK  {err} err")

    rate = total_bytes / elapsed / 1024 / 1024 if elapsed > 0 else 0
    print(f"\nTotale: {total_ok}/{ep_count * num_packets} OK  {total_err} err")
    print(f"Throughput aggregato: {rate:.3f} MB/s  "
          f"({total_bytes/1024:.1f} KB in {elapsed:.2f}s)")
    if ep_count > 1:
        print(f"Average per EP: {rate/ep_count:.3f} MB/s")

def count_device_ep_pairs(dev):
    """Counts the bulk IN+OUT pairs in the device's active config descriptor.
       H753 exposes 6 EPs, U575 exposes 5 -> no magic number on the test side."""
    try:
        cfg = dev.get_active_configuration()
    except usb.core.USBError:
        cfg = dev[0]
    intf = cfg[(0, 0)]
    ins  = sum(1 for ep in intf if (ep.bEndpointAddress & 0x80))
    outs = sum(1 for ep in intf if not (ep.bEndpointAddress & 0x80))
    return min(ins, outs)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--packets", type=int, default=10000)
    ap.add_argument("--size",    type=int, default=64)
    ap.add_argument("--ep",      type=int, default=0,
                    help=f"Number of EPs to test (1..{MAX_EP}, default: all those the device exposes)")
    args = ap.parse_args()

    dev = find_device()
    dev_ep = count_device_ep_pairs(dev)
    if dev_ep < 1:
        print("No EP IN+OUT pair found in the config descriptor")
        sys.exit(1)

    if args.ep == 0:
        args.ep = dev_ep
    elif args.ep < 1 or args.ep > MAX_EP:
        print(f"--ep must be 1..{MAX_EP}")
        sys.exit(1)
    elif args.ep > dev_ep:
        print(f"Device exposes only {dev_ep} EP bulk pair(s); --ep reduced to {dev_ep}")
        args.ep = dev_ep

    print(f"Device: {dev_ep} EP bulk pair(s) - using {args.ep}")
    test_loopback(dev, args.packets, args.size, args.ep)

if __name__ == "__main__":
    main()
