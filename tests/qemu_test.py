#!/usr/bin/env python3
"""Runs the real ESP32-C3 firmware in Espressif's QEMU and checks it end to end.

- heartbeats reach the OHP reference server (`ohp serve`), with the boot reason;
- Improv serial answers GET_INFO and GET_STATE like ESP Web Tools expects;
- the status page and its JSON API work, and cross-site form posts are refused.

usage: qemu_test.py FLASH_IMAGE   (needs qemu-system-riscv32 and ohp on PATH)
"""
import json
import os
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request

KEY = "e2etestkey0123456789abcdef012345"
OHP_PORT = 18080
WEB_PORT = 18081


class Serial:
    """Collects everything QEMU prints on the emulated UART."""

    def __init__(self, proc):
        self.proc = proc
        self.buf = bytearray()
        self.lock = threading.Lock()
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        while True:
            chunk = os.read(self.proc.stdout.fileno(), 4096)
            if not chunk:
                return
            sys.stdout.buffer.write(chunk)
            sys.stdout.flush()
            with self.lock:
                self.buf += chunk

    def wait_for(self, needle, timeout, start=0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            with self.lock:
                i = self.buf.find(needle, start)
            if i >= 0:
                return i
            time.sleep(0.1)
        raise AssertionError(f"timeout waiting for {needle!r} on the serial port")

    def size(self):
        with self.lock:
            return len(self.buf)

    def write(self, data):
        self.proc.stdin.write(data)
        self.proc.stdin.flush()


def improv_frame(ftype, data):
    frame = b"IMPROV" + bytes([1, ftype, len(data)]) + bytes(data)
    return frame + bytes([sum(frame) & 0xFF])


def read_frames(serial, start, ftype, timeout):
    """Returns the data of the first valid frame of the given type after start."""
    deadline = time.time() + timeout
    pos = start
    while time.time() < deadline:
        with serial.lock:
            buf = bytes(serial.buf)
        i = buf.find(b"IMPROV", pos)
        if i < 0 or len(buf) < i + 9:
            time.sleep(0.1)
            continue
        length = buf[i + 8]
        end = i + 9 + length
        if len(buf) < end + 1:
            time.sleep(0.1)
            continue
        if buf[i + 6] == 1 and buf[i + 7] == ftype and sum(buf[i:end]) & 0xFF == buf[end]:
            return buf[i + 9 : end]
        pos = i + 1
    raise AssertionError(f"no Improv frame of type {ftype}")


def strings(rpc_result):
    """Decodes the strings of an RPC result: command, length, strings..."""
    out, data, p = [], rpc_result[2 : 2 + rpc_result[1]], 0
    while p < len(data):
        n = data[p]
        out.append(data[p + 1 : p + 1 + n].decode())
        p += 1 + n
    return out


def checks():
    req = urllib.request.Request(
        f"http://127.0.0.1:{OHP_PORT}/api/checks", headers={"Authorization": f"Bearer {KEY}"}
    )
    with urllib.request.urlopen(req, timeout=5) as r:
        return {c["slug"]: c for c in json.load(r)}


def wait_signal(slug, signal, body, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        c = checks().get(slug)
        if c:
            for e in c["events"]:
                if e["signal"] == signal and body in e.get("body", "") and e.get("host") == "canary-qemu":
                    print(f"\nok: {slug} {signal} {e['body']!r}", flush=True)
                    return e
        time.sleep(0.5)
    raise AssertionError(f"no {signal} signal with {body!r} for {slug}: {checks().get(slug)}")


def http(path, data=None, headers=None):
    req = urllib.request.Request(f"http://127.0.0.1:{WEB_PORT}{path}", data=data, headers=headers or {})
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            return r.status, r.read().decode()
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode()


def main():
    image = sys.argv[1]
    ohp = subprocess.Popen(["ohp", "serve", "-addr", f"127.0.0.1:{OHP_PORT}", "-key", KEY])
    qemu = subprocess.Popen(
        [
            "qemu-system-riscv32", "-nographic", "-icount", "3", "-machine", "esp32c3",
            "-drive", f"file={image},if=mtd,format=raw",
            "-nic", f"user,model=open_eth,hostfwd=tcp:127.0.0.1:{WEB_PORT}-:80",
        ],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
    )
    serial = Serial(qemu)
    try:
        serial.wait_for(b"canary 0.", 120)
        serial.wait_for(b"I (", 10)
        serial.wait_for(b"address 10.0.2.15", 120)

        wait_signal("qemu-alive", "success", "boot=power-on", 120)
        wait_signal("qemu-contact", "success", "contact=idle", 60)

        # Improv: device information.
        start = serial.size()
        serial.write(improv_frame(0x03, [0x03, 0x00]))
        result = read_frames(serial, start, 0x04, 30)
        info = strings(result)
        print(f"\nok: improv info {info}", flush=True)
        assert result[0] == 0x03, result
        assert info[0] == "canary" and info[2] == "ESP32-C3" and info[3] == "canary-qemu", info

        # Improv: state; Ethernet is up, so the device is provisioned and
        # returns the setup page URL.
        start = serial.size()
        serial.write(improv_frame(0x03, [0x02, 0x00]))
        state = read_frames(serial, start, 0x01, 30)
        assert state == bytes([0x04]), state
        urls = strings(read_frames(serial, start, 0x04, 30))
        print(f"ok: improv state provisioned, {urls}", flush=True)
        assert urls == ["http://10.0.2.15/"], urls

        # Improv: a frame with a bad checksum is answered with an error.
        start = serial.size()
        bad = bytearray(improv_frame(0x03, [0x02, 0x00]))
        bad[-1] ^= 0xFF
        serial.write(bytes(bad))
        assert read_frames(serial, start, 0x02, 30) == bytes([0x01])
        print("ok: improv rejects a bad checksum", flush=True)

        status, body = http("/api/status")
        assert status == 200, (status, body)
        doc = json.loads(body)
        print(f"ok: /api/status {doc}", flush=True)
        assert doc["name"] == "canary-qemu" and doc["setup_open"] is True
        assert [p["type"] for p in doc["probes"]] == ["alive", "contact"]
        assert all(KEY not in p["url"] for p in doc["probes"]), "check URL not redacted"

        status, body = http("/")
        assert status == 200 and "canary-qemu" in body and "Save and restart" in body, status
        print("ok: setup page", flush=True)

        status, _ = http("/save", b"name=evil", {"Origin": "http://evil.example"})
        assert status == 403, status
        print("ok: cross-site post refused", flush=True)

        status, body = http("/save", b"probe1.type=alive&probe1.url=nope", {"Origin": f"http://127.0.0.1:{WEB_PORT}"})
        assert status == 200 and "an http(s) check URL is required" in body, (status, body[:200])
        print("ok: invalid settings rejected with a message", flush=True)

        # Heartbeats keep coming at the configured interval (10 s).
        first = len(checks()["qemu-alive"]["events"])
        deadline = time.time() + 120
        while len(checks()["qemu-alive"]["events"]) <= first:
            assert time.time() < deadline, "no second heartbeat"
            time.sleep(1)
        print("ok: periodic heartbeat", flush=True)
        print("\nqemu: all checks passed", flush=True)
    finally:
        qemu.kill()
        ohp.kill()


if __name__ == "__main__":
    main()
