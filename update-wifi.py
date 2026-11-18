#!/usr/bin/env python3
"""Upload an authenticated ESP32 Observer image over the local network."""

import argparse
import hashlib
import hmac
from pathlib import Path
import struct
import time
import urllib.request


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("firmware", type=Path, nargs="?")
    parser.add_argument("--host", required=True)
    parser.add_argument("--key", type=Path, default=Path("ota.key"))
    args = parser.parse_args()
    base = f"http://{args.host}:8266"
    http = urllib.request.build_opener(urllib.request.ProxyHandler({}))

    def status():
        with http.open(base + "/status", timeout=3) as response:
            return dict(line.split("=", 1) for line in response.read(512).decode().splitlines())

    before = status()
    print(before, flush=True)
    if args.firmware is None:
        return
    image = args.firmware.read_bytes()
    key = bytes.fromhex(args.key.read_text().strip())
    if len(key) != 32:
        raise SystemExit("Expected a 32-byte OTA key")
    # ESP-IDF app descriptor follows the 24-byte image and 8-byte segment headers.
    if not 208 <= len(image) <= 0x160000 or image[0] != 0xE9 or image[32:36] != b"\x32\x54\xcd\xab":
        raise SystemExit("Expected an ESP32 app binary that fits the OTA slot")
    image_id = image[176:208].hex()
    with http.open(base + "/challenge", timeout=3) as response:
        nonce = bytes.fromhex(response.read(128).decode().strip())
    if len(nonce) != 16:
        raise SystemExit("Unexpected OTA challenge")
    digest = hashlib.sha256(image).digest()
    signature = hmac.digest(key, nonce + digest + struct.pack(">I", len(image)), "sha256")
    request = urllib.request.Request(
        base + "/firmware", data=image,
        headers={"Content-Type": "application/octet-stream",
                 "X-Firmware-SHA256": digest.hex(), "X-Firmware-HMAC": signature.hex()},
    )
    with http.open(request, timeout=110) as response:
        print(response.read(512).decode().strip(), flush=True)

    deadline = time.monotonic() + 260
    while time.monotonic() < deadline:
        time.sleep(3)
        try:
            after = status()
        except OSError:
            continue
        if after["slot"] != before["slot"] and after["validating"] == "0" and after.get("image") == image_id:
            print(after, flush=True)
            return
    raise SystemExit("The new image did not report a confirmed boot. Check the current slot with --host.")


if __name__ == "__main__":
    main()
