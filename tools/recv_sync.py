#!/usr/bin/env python3
"""Receives WLED audio sync V2 packets and prints them decoded.

Joins multicast group 239.0.0.1 on port 11988 by default; broadcast and
unicast packets to the same port arrive too. One line per second by default
(rate, gaps, last packet); --all prints every packet.

    python3 tools/recv_sync.py
    python3 tools/recv_sync.py --all --port 11988
"""

import argparse
import socket
import struct
import sys
import time

FORMAT = "<6s2sffBB16sHff"  # docs/wled-sync.md
SIZE = struct.calcsize(FORMAT)
GROUP = "239.0.0.1"


def decode(data):
    (header, pressure, raw, smth, peak, counter, fft, zc, mag, major) = struct.unpack(
        FORMAT, data
    )
    return {
        "header": header,
        "pressure": pressure[0] + pressure[1] / 256.0,
        "sampleRaw": raw,
        "sampleSmth": smth,
        "samplePeak": peak,
        "frameCounter": counter,
        "fftResult": list(fft),
        "zeroCrossingCount": zc,
        "FFT_Magnitude": mag,
        "FFT_MajorPeak": major,
    }


def bars(values):
    blocks = " .:-=+*#%@"
    return "".join(blocks[min(len(blocks) - 1, v * len(blocks) // 256)] for v in values)


def line(src, p):
    return (
        "%-15s #%3d smth %6.1f raw %6.1f peak %d press %5.1f major %7.1f Hz mag %8.1f zc %3d |%s|"
        % (
            src,
            p["frameCounter"],
            p["sampleSmth"],
            p["sampleRaw"],
            p["samplePeak"],
            p["pressure"],
            p["FFT_MajorPeak"],
            p["FFT_Magnitude"],
            p["zeroCrossingCount"],
            bars(p["fftResult"]),
        )
    )


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=11988)
    ap.add_argument("--no-multicast", action="store_true", help="don't join 239.0.0.1")
    ap.add_argument("--interface", default="0.0.0.0", help="local address to join the group on")
    ap.add_argument("--all", action="store_true", help="print every packet")
    args = ap.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("", args.port))
    if not args.no_multicast:
        mreq = struct.pack("4s4s", socket.inet_aton(GROUP), socket.inet_aton(args.interface))
        sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)
    sock.settimeout(1.0)
    print(
        "listening on port %d%s" % (args.port, "" if args.no_multicast else ", group " + GROUP),
        flush=True,
    )

    count = bad = gaps = 0
    last_counter = None
    last = None
    window = time.monotonic()
    while True:
        try:
            data, (src, _) = sock.recvfrom(256)
        except socket.timeout:
            data = None
        except KeyboardInterrupt:
            return 0
        now = time.monotonic()
        if data is not None:
            if len(data) != SIZE or not data.startswith(b"00002\0"):
                bad += 1
            else:
                p = decode(data)
                c = p["frameCounter"]
                if last_counter is not None and c != 0:
                    expected = 1 if last_counter >= 255 else last_counter + 1
                    if c != expected:
                        gaps += 1
                last_counter = c
                count += 1
                last = (src, p)
                if args.all:
                    print(line(src, p), flush=True)
        if now - window >= 1.0:
            if not args.all:
                if last is None:
                    print("%s  no packets" % time.strftime("%H:%M:%S"), flush=True)
                else:
                    print(
                        "%s  %3d/s gaps %d bad %d  %s"
                        % (time.strftime("%H:%M:%S"), count, gaps, bad, line(*last)),
                        flush=True,
                    )
            count = bad = gaps = 0
            last = None
            window = now


if __name__ == "__main__":
    sys.exit(main())
