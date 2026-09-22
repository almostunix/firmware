#!/usr/bin/env python3
"""Connect to a Meshtastic TCP API and timestamp the config download.

    bin/meshtastic-config-timeline.py HOST PORT [SECONDS]

`meshtastic --info` reports only pass/fail after a 30s wait. This shows *where*
a stalled download stops: whether the TCP connect is slow, whether any bytes
arrive at all, and when the stream goes quiet relative to connect.
"""
import select
import socket
import struct
import sys
import time

host, port = sys.argv[1], int(sys.argv[2])
limit = float(sys.argv[3]) if len(sys.argv) > 3 else 35.0

def varint(n):
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        out.append(b | (0x80 if n else 0))
        if not n:
            return bytes(out)

# ToRadio { want_config_id (field 3, varint) = 42424 }, in stream framing.
payload = b"\x18" + varint(42424)
frame = b"\x94\xc3" + struct.pack(">H", len(payload)) + payload

t0 = time.monotonic()
ts = lambda: time.monotonic() - t0
s = socket.create_connection((host, port), timeout=limit)
print("%7.3fs  TCP connected" % ts())
s.sendall(frame)
print("%7.3fs  want_config sent" % ts())

total, last_rx, bucket_start, bucket_bytes = 0, None, None, 0
s.setblocking(False)
while ts() < limit:
    r, _, _ = select.select([s], [], [], 0.25)
    now = ts()
    if r:
        try:
            data = s.recv(65536)
        except BlockingIOError:
            continue
        if not data:
            print("%7.3fs  peer closed after %d bytes" % (now, total))
            break
        if last_rx is not None and now - last_rx > 1.0:
            print("%7.3fs  ... gap of %.2fs with no data" % (now, now - last_rx))
        if bucket_start is None:
            print("%7.3fs  first bytes" % now)
            bucket_start = now
        total += len(data)
        bucket_bytes += len(data)
        last_rx = now
        # config_complete_id echoes our id: FromRadio field 7 = 0x38, varint 42424
        if b"\x38" + varint(42424) in data:
            print("%7.3fs  config complete, %d bytes total" % (now, total))
            break
    elif last_rx is not None and now - last_rx > 5.0 and (int(now) % 5 == 0):
        pass
else:
    print("%7.3fs  GAVE UP: %d bytes received, last at %s" % (
        ts(), total, "%.3fs" % last_rx if last_rx is not None else "never"))
s.close()
