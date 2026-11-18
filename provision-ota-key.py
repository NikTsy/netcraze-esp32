#!/usr/bin/env python3
"""Create a local OTA client key and its first-install firmware header."""

import argparse
import os
from pathlib import Path
import secrets


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--key", type=Path, default=Path("ota.key"))
    args = parser.parse_args()
    header = Path(__file__).resolve().parent / "main" / "ota_bootstrap.h"
    if header.exists() or args.key.exists():
        parser.error("refusing to replace an existing key or bootstrap header")
    key = secrets.token_bytes(32)
    with open(args.key, "x", opener=private_create) as output:
        output.write(key.hex() + "\n")
    with open(header, "x", opener=private_create) as output:
        initializer = ", ".join(f"0x{byte:02x}" for byte in key)
        output.write(f"#define NETCRAZE_OTA_BOOTSTRAP {{{initializer}}}\n")
    print("Local key and bootstrap header created. Keep both private.")


def private_create(path, flags):
    return os.open(path, flags, 0o600)


if __name__ == "__main__":
    main()
