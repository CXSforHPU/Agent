"""Minimal MQTT 3.1.1 subscriber (stdlib only) to verify board->broker publishes.

Usage: python mqtt_sub_test.py <topic-filter> <seconds> [host] [port]
"""
import socket
import struct
import sys
import time

topic = sys.argv[1].encode()
seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 60
host = sys.argv[3] if len(sys.argv) > 3 else "47.93.225.100"
port = int(sys.argv[4]) if len(sys.argv) > 4 else 1883
user = b"USER1"
pw = b"USER1"
client_id = b"pc-e2e-sub"


def enc_len(n):
    out = b""
    while True:
        b = n % 128
        n //= 128
        if n > 0:
            b |= 0x80
        out += bytes([b])
        if n == 0:
            break
    return out


def mstr(s):
    return struct.pack("!H", len(s)) + s


def read_packet(sock):
    hdr = sock.recv(1)
    if not hdr:
        return None, None
    mult = 1
    rl = 0
    while True:
        b = sock.recv(1)
        if not b:
            return None, None
        rl += (b[0] & 0x7F) * mult
        if not (b[0] & 0x80):
            break
        mult *= 128
    body = b""
    while len(body) < rl:
        chunk = sock.recv(rl - len(body))
        if not chunk:
            break
        body += chunk
    return hdr[0], body


payload = (
    mstr(b"MQTT")
    + b"\x04"
    + b"\xc2"
    + struct.pack("!H", 60)
    + mstr(client_id)
    + mstr(user)
    + mstr(pw)
)
s = socket.create_connection((host, port), timeout=10)
s.sendall(b"\x10" + enc_len(len(payload)) + payload)
connack = s.recv(4)
print("CONNACK:", connack.hex(), "rc=", connack[3] if len(connack) >= 4 else -1, flush=True)

# SUBSCRIBE (packet id 1, one topic filter, QoS1)
sub = struct.pack("!H", 1) + mstr(topic) + b"\x01"
s.sendall(b"\x82" + enc_len(len(sub)) + sub)
hdr, body = read_packet(s)
print("SUBACK:", hex(hdr) if hdr else None, body.hex() if body else None, flush=True)
print("listening on", topic.decode(), "for", seconds, "s", flush=True)

s.settimeout(1.0)
deadline = time.time() + seconds
got = 0
last_ping = time.time()
while time.time() < deadline:
    if time.time() - last_ping > 20:
        s.sendall(b"\xc0\x00")  # PINGREQ, keeps the session alive
        last_ping = time.time()
    try:
        hdr, body = read_packet(s)
    except socket.timeout:
        continue
    if hdr is None:
        print("connection closed", flush=True)
        break
    ptype = hdr >> 4
    if ptype == 3:  # PUBLISH
        tlen = struct.unpack("!H", body[0:2])[0]
        t = body[2:2 + tlen].decode(errors="replace")
        rest = body[2 + tlen:]
        qos = (hdr >> 1) & 0x03
        if qos > 0:
            rest = rest[2:]  # skip packet id
        print("RECEIVED topic=%s payload=%s" % (t, rest.decode(errors="replace")), flush=True)
        got += 1
    elif ptype == 13:  # PINGRESP
        pass

print("total received:", got, flush=True)
s.close()
