#!/usr/bin/env python3
"""Verify a deployed Windows build can open its window and render the demo."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

binary = Path(sys.argv[1]).resolve(strict=True)
env = dict(os.environ, QT_QPA_PLATFORM="windows", QT_DEBUG_PLUGINS="1")
for variable in ("QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH"):
    env.pop(variable, None)
with tempfile.TemporaryDirectory(prefix="navidog-smoke-") as temporary:
    workspace = Path(temporary)
    screenshot = workspace / "demo.png"
    try:
        subprocess.run(
            [binary, "--data-dir", workspace / "profile", "--screenshot", screenshot],
            cwd=binary.parent,
            env=env,
            check=True,
            timeout=60,
        )
    except subprocess.TimeoutExpired:
        raise SystemExit(f"Navidog startup timed out: {binary}") from None
    if not screenshot.is_file() or not screenshot.read_bytes().startswith(b"\x89PNG\r\n\x1a\n"):
        raise SystemExit(f"Navidog did not render the demo: {binary}")
print(f"Navidog opened and rendered the demo: {binary}")
