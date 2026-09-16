import socket, json, time, threading

DEV = ('10.241.126.252', 50003)
stop = threading.Event()

sig = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sig.settimeout(5)
rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
rx.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
rx.bind(("", 50000))
rx.settimeout(0.5)

def heartbeat():
    while not stop.is_set():
        sig.sendto(json.dumps({"type": "HEARTBEAT", "ver": 1, "ts": int(time.time()*1000),
                               "sw_ver": "0.1", "listen_ports": [50000, 50002]}).encode(), DEV)
        time.sleep(1.0)

# learn
for i in range(3):
    sig.sendto(json.dumps({"type": "HEARTBEAT", "ver": 1, "ts": int(time.time()*1000),
                           "sw_ver": "0.1"}).encode(), DEV)
    try:
        d, a = sig.recvfrom(512)
        print("HB_ACK", d.decode())
    except socket.timeout:
        print("hb timeout", i)
    time.sleep(0.3)

threading.Thread(target=heartbeat, daemon=True).start()
time.sleep(0.5)
sig.sendto(json.dumps({"type": "SESSION_START", "ver": 1, "mode": "udp"}).encode(), DEV)
print("sent SESSION_START")

cnt = 0
seqs = []
lens = set()
bad = 0
t0 = time.time()
while time.time() - t0 < 8.0:
    try:
        d, a = rx.recvfrom(1024)
        cnt += 1
        if len(d) >= 12 and d[0] == 0xA5 and d[1] == 0x5A:
            seqs.append(int.from_bytes(d[4:6], "little"))
            lens.add(len(d))
        else:
            bad += 1
    except socket.timeout:
        pass

print("RX frames:", cnt, "bad:", bad, "sizes:", lens)
if seqs:
    gap = 0
    for i in range(1, len(seqs)):
        gap += (seqs[i] - seqs[i-1] - 1) & 0xFFFF
    print("first_seq", seqs[0], "last_seq", seqs[-1], "recv", len(seqs), "lost~", gap)

sig.sendto(json.dumps({"type": "SESSION_STOP", "ver": 1, "reason": "probe"}).encode(), DEV)
print("sent SESSION_STOP")
stop.set()
time.sleep(0.5)
sig.close()
rx.close()
