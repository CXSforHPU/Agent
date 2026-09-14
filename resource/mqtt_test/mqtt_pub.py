"""Minimal MQTT 3.1.1 publisher (stdlib only) for end-to-end testing.

Usage: python mqtt_pub.py <topic> [payload] [host] [port]

If <payload> is omitted or "-", the payload is read from the MQTT_PAYLOAD
environment variable. Use that form on Windows/PowerShell: passing JSON with
inner double quotes as a command-line argument would lose the quotes.
"""
import os
import socket
import struct
import sys
import time

topic = sys.argv[1].encode()
if len(sys.argv) > 2 and sys.argv[2] != "-":
    msg = sys.argv[2].encode()
else:
    msg = os.environ.get("MQTT_PAYLOAD", "{}").encode()
host = sys.argv[3] if len(sys.argv) > 3 else "47.93.225.100"
port = int(sys.argv[4]) if len(sys.argv) > 4 else 1883
user = b"USER1"
pw = b"USER1"
client_id = b"pc-e2e-pub"


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


# ---- CONNECT (clean session + username + password) ----
payload = (
    mstr(b"MQTT")
    + b"\x04"
    + b"\xc2"
    + struct.pack("!H", 60)
    + mstr(client_id)
    + mstr(user)
    + mstr(pw)
)
pkt = b"\x10" + enc_len(len(payload)) + payload

s = socket.create_connection((host, port), timeout=10)
s.sendall(pkt)
connack = s.recv(4)
rc = connack[3] if len(connack) >= 4 else -1
print("CONNACK:", connack.hex(), "return_code=", rc, flush=True)
if rc != 0:
    print("connect rejected", flush=True)
    sys.exit(1)

time.sleep(0.3)

# ---- PUBLISH QoS0 ----
p = mstr(topic) + msg
s.sendall(b"\x30" + enc_len(len(p)) + p)
print("PUBLISH topic=%s payload=%s" % (topic.decode(), msg.decode()), flush=True)

time.sleep(0.5)
s.close()
print("done", flush=True)
