#!/usr/bin/env python3
"""c3-central-flash-capture: wait for the ESP32-C3 (serial 8841400262090244)
to appear, flash the a13 central harness into it, then capture its console
for a fixed window. Prints FOUND/CONNECTED/SUBSCRIBED/NOTIFY/DISCONNECT/
HEARTBEAT lines. Self-terminating."""
import os
import re
import subprocess
import sys
import time

C3_SERIAL = "8841400262090244"
WS = "/Users/ryan/Documents/CobanStationeryAssets/Firmware/src/zmk"
BIN = f"{WS}/build/c3-central/zephyr/zephyr.bin"
CAPTURE_S = int(sys.argv[1]) if len(sys.argv) > 1 else 240


def find_c3_port():
    out = subprocess.run(["ioreg", "-p", "IOUSB", "-l", "-w0"],
                         capture_output=True, text=True).stdout
    for chunk in out.split("+-o IOUSBHostDevice"):
        if C3_SERIAL in chunk:
            m = re.search(r'"IOCalloutDevice"\s*=\s*"(/dev/cu\.[^"]+)"', chunk)
            if m:
                return m.group(1)
    return None


print("waiting for C3...", flush=True)
end = time.time() + 1800
port = None
while time.time() < end:
    port = find_c3_port()
    if port:
        busy = subprocess.run(["lsof", port], capture_output=True, text=True)
        if not busy.stdout.strip():
            break
        print(f"{port} busy", flush=True)
    time.sleep(2)
else:
    print("timeout waiting for C3", flush=True)
    sys.exit(0)

print(f"C3 on {port} — flashing central", flush=True)
r = subprocess.run(
    ["esptool", "--chip", "esp32c3", "--port", port, "--baud", "460800",
     "write_flash", "0x0", BIN],
    capture_output=True, text=True, timeout=420)
last = [l for l in (r.stdout + r.stderr).splitlines() if l.strip()][-2:]
print(" | ".join(last), flush=True)
if "Hash of data verified" not in r.stdout:
    print("FLASH FAILED", flush=True)
    sys.exit(1)

time.sleep(2)
print(f"capturing C3 console {port} for {CAPTURE_S}s", flush=True)
import serial  # noqa: E402
s = serial.Serial(port, 115200, timeout=0.2)
s.dtr = False
s.rts = True
time.sleep(0.1)
s.rts = False  # RTS pulse = reset into fresh central boot
t0 = time.time()
buf = b""
while time.time() - t0 < CAPTURE_S:
    d = s.read(4096)
    if d:
        buf += d
        for line in buf.split(b"\r\n"):
            if line.endswith((b"\n", b"\r")) or b"\r\n" in line:
                continue
        buf = buf[-200:] if len(buf) > 5000 else buf
        txt = d.decode(errors="replace")
        if re.search(r"FOUND|CONNECT|SUBSCR|NOTIFY|DISCONN|HEARTBEAT|error|ERR|lost|LOST", txt):
            print(txt.strip()[:300], flush=True)
print("capture done", flush=True)
