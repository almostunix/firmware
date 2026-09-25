#!/usr/bin/env python3
"""Board performance probe over the tailnet (read-only: no config writes, no radio TX).

    bin/meshtastic-perf-bench.py HOST N            timed API sessions: connect, full config download, clean close
    bin/meshtastic-perf-bench.py HOST orphan       does a client displaced by a newer connection get a FIN?
"""
import select, socket, statistics, struct, sys, time

host = sys.argv[1]
PORT = 4403


def varint(n):
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        out.append(b | (0x80 if n else 0))
        if not n:
            return bytes(out)


def want_config(nonce):
    p = b"\x18" + varint(nonce)
    return b"\x94\xc3" + struct.pack(">H", len(p)) + p


def session(nonce, limit=30.0):
    t0 = time.monotonic()
    s = socket.create_connection((host, PORT), timeout=limit)
    t_conn = time.monotonic() - t0
    s.sendall(want_config(nonce))
    marker = b"\x38" + varint(nonce)
    buf, t_first, t_done = b"", None, None
    s.setblocking(False)
    while time.monotonic() - t0 < limit:
        r, _, _ = select.select([s], [], [], 0.25)
        if not r:
            continue
        d = s.recv(65536)
        if not d:
            break
        if t_first is None:
            t_first = time.monotonic() - t0
        buf += d
        if marker in buf:
            t_done = time.monotonic() - t0
            break
    s.close()
    return t_conn, t_first, t_done, len(buf)


def pct(xs, p):
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(round(p / 100 * (len(xs) - 1))))]


if sys.argv[2] == "orphan":
    a = socket.create_connection((host, PORT), timeout=10)
    a.sendall(want_config(111))
    time.sleep(3)
    a.setblocking(False)
    try:
        while a.recv(65536):
            pass
    except BlockingIOError:
        pass
    print("client A connected and idle; opening client B (the node closes A)")
    b = socket.create_connection((host, PORT), timeout=10)
    b.sendall(want_config(222))
    t0 = time.monotonic()
    fin = None
    while time.monotonic() - t0 < 20:
        r, _, _ = select.select([a], [], [], 0.5)
        if r:
            try:
                d = a.recv(65536)
            except (BlockingIOError, ConnectionResetError):
                d = b""
            if not d:
                fin = time.monotonic() - t0
                break
    print("A saw the node close its connection after %.1fs" % fin if fin is not None
          else "A got NO close within 20s: it still looks connected but the node has dropped it")
    if fin is None:
        a.sendall(want_config(333))
        t1 = time.monotonic()
        while time.monotonic() - t1 < 10:
            r, _, _ = select.select([a], [], [], 0.5)
            if r:
                try:
                    d = a.recv(65536)
                except (BlockingIOError, ConnectionResetError):
                    d = b""
                if not d:
                    print("after A sent a request, its connection closed %.1fs later" % (time.monotonic() - t1))
                    break
    a.close()
    b.close()
    sys.exit(0)

n = int(sys.argv[2])
rows = []
for i in range(n):
    try:
        rows.append(session(50000 + i))
    except OSError as e:
        print("session %d failed: %s" % (i, e))
    time.sleep(1.0)
ok = [r for r in rows if r[2] is not None]
print("sessions: %d ok of %d" % (len(ok), n))
for name, idx in (("tcp connect", 0), ("first byte", 1), ("config complete", 2)):
    xs = [r[idx] * 1000 for r in ok]
    print("%-16s min %5.0f  median %5.0f  p90 %5.0f  max %5.0f ms" % (name, min(xs), statistics.median(xs), pct(xs, 90), max(xs)))
sizes = [r[3] for r in ok]
rate = [r[3] / (r[2] - r[1]) / 1024 for r in ok if r[2] > r[1]]
print("config size %d bytes; download rate after first byte: median %.1f KB/s" % (statistics.median(sizes), statistics.median(rate)))
