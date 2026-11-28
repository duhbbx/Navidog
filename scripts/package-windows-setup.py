#!/usr/bin/env python3
"""Compile the Windows installer from the same staged payload as the ZIP."""
import os
from pathlib import Path
import re
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
version = re.search(r"project\(Navidog VERSION ([0-9.]+)", (root / "CMakeLists.txt").read_text())[1]
edition = sys.argv[1]
if edition not in ("7", "8", "10", "11"):
    raise SystemExit("Expected Windows edition 7, 8, 10 or 11")
payload = root / "dist" / f"Navidog-Win{edition}-x64"
if not (payload / "navidog.exe").is_file():
    raise SystemExit(f"Build the portable payload first: {payload}")
output = payload.parent / f"Navidog-{version}-Win{edition}-x64-Setup.exe"
icon = root / "resources/branding/navidog.ico"
compiler = sys.argv[2] if len(sys.argv) > 2 else "makensis"
option = "/" if os.name == "nt" else "-"
subprocess.run([compiler, f"{option}DPAYLOAD_DIR={payload}", f"{option}DOUTPUT_FILE={output}",
                f"{option}DAPP_VERSION={version}", f"{option}DWINDOWS_EDITION={edition}", f"{option}DICON_FILE={icon}",
                root / "packaging/windows/navidog.nsi"], check=True)
with output.open("rb") as binary:
    if binary.read(2) != b"MZ":
        raise SystemExit("Installer was not generated as a Windows executable")
print(output)
