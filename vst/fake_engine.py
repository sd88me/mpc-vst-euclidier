#!/usr/bin/env python3
"""Stand-in for the euclidier engine in host_test: same control-socket protocol (GET/SET, one request per
connection), 8 lanes with a euclidean pattern and a play-head that advances 20 steps a second."""
import os, socket, sys, time

ctrl = sys.argv[sys.argv.index("--ctrl-sock") + 1]
lanes = [dict(enable=1, steps=16, fill=4, shift=0, loop=0, note=60, div=0, gate=50, ch=1, vel=100, velh=0, mode=0) for _ in range(8)]
state = dict(sel=0, preset=0)
t0 = time.time()
mask = [0] * 8


def bits(l):
    n, k = l["steps"], l["fill"]
    return [1 if (i * k) % n < k else 0 for i in range(n)]


def get(key):
    if key in state:
        return str(state[key])
    if key == "transport":
        return "RUN 120"
    if key.startswith("sel_"):
        lane, p = state["sel"], key[4:]
    elif key[0] == "l" and key[2] == "_":
        lane, p = int(key[1]) - 1, key[3:]
    else:
        return "ERR"
    l = lanes[lane]
    if p == "pattern":
        return "%d|%s|%d|%d|%d|%d" % (l["steps"], ",".join(map(str, bits(l))), int((time.time() - t0) * 20) % l["steps"], l["loop"], l["enable"], lane == state["sel"])
    if p == "info":
        return "%d/%d SH%d" % (l["steps"], l["fill"], l["shift"])
    return str(l[p]) if p in l else "ERR"


def put(key, v):
    if key in state:
        state[key] = v
        return True
    if key.startswith("rand_l") and len(key) == 7:   # the randomise lane flags
        mask[int(key[6]) - 1] = v
        return True
    if key == "rand_go":   # randomise the flagged lanes (none flagged: all); here that just sets steps to 7
        for n in range(8):
            if any(mask) and not mask[n]:
                continue
            lanes[n]["steps"] = 7
        return True
    if key.startswith("sel_"):
        lane, p = state["sel"], key[4:]
    elif key[0] == "l" and key[2] == "_":
        lane, p = int(key[1]) - 1, key[3:]
    else:
        return True
    if p in lanes[lane]:
        lanes[lane][p] = v
        if p == "mode":   # like the real engine: a mode switch picks that mode's note
            lanes[lane]["note"] = 60 if int(v) == 0 else 36 + lane
    return True


if os.path.exists(ctrl):
    os.unlink(ctrl)
s = socket.socket(socket.AF_UNIX)
s.bind(ctrl)
s.listen(8)
while True:
    c, _ = s.accept()
    try:
        parts = c.recv(256).decode().split()
        if parts and parts[0] == "GET":
            c.sendall((get(parts[1]) + "\n").encode())
        elif parts and parts[0] == "SET" and len(parts) >= 3:
            c.sendall(b"OK\n" if put(parts[1], int(float(parts[2]))) else b"ERR\n")
    finally:
        c.close()
