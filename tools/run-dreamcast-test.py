#!/usr/bin/env python3
"""Bound a headless Flycast run and stop before REIOS reboots an exited guest."""
from pathlib import Path
import os
import re
import selectors
import subprocess
import sys
import time


def main():
    arguments = sys.argv[1:]
    if len(arguments) not in (5, 7):
        raise ValueError("expected flycast disc log seconds gdb [screenshot frame]")
    flycast, disc, log, seconds, gdb = arguments[:5]
    capture = []
    if len(arguments) == 7:
        screenshot, frame = arguments[5:]
        if int(frame) <= 0:
            raise ValueError("invalid screenshot frame")
        capture = ["-screenshot", str(Path(screenshot).resolve()),
                   "-screenshot-frame", frame]
    deadline = time.monotonic() + float(seconds)
    if float(seconds) <= 0 or gdb not in ("yes", "no"):
        raise ValueError("invalid test duration or GDB setting")
    readback = os.environ.get("GEX_TEST_RTT_READBACK", "no")
    if readback not in ("yes", "no"):
        raise ValueError("GEX_TEST_RTT_READBACK must be yes or no")
    fast_forward = os.environ.get("GEX_TEST_FAST_FORWARD", "yes")
    vsync = os.environ.get("GEX_TEST_VSYNC", "no")
    if fast_forward not in ("yes", "no") or vsync not in ("yes", "no"):
        raise ValueError("GEX_TEST_FAST_FORWARD and GEX_TEST_VSYNC must be yes or no")
    config = (f"config:Dynarec.Enabled=yes,config:Debug.GDBEnabled={gdb},"
              "config:Debug.GDBPort=3263,config:Debug.GDBWaitForConnection=no,"
              f"config:Debug.SerialConsoleEnabled=yes,config:Debug.FastForward={fast_forward},"
              f"rend:vsync={vsync},config:rend.vsync={vsync},config:pvr.rend=4,"
              f"config:rend.RenderToTextureBuffer={readback}")
    with Path(log).open("wb") as output:
        process = subprocess.Popen([flycast, "-headless", *capture, "-config", config, disc],
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        try:
            with selectors.DefaultSelector() as selector:
                selector.register(process.stdout, selectors.EVENT_READ)
                pending = b""
                while time.monotonic() < deadline:
                    if not selector.select(min(0.1, max(0, deadline - time.monotonic()))):
                        if process.poll() is not None:
                            break
                        continue
                    data = os.read(process.stdout.fileno(), 8192)
                    if not data:
                        break
                    output.write(data)
                    output.flush()
                    pending += data
                    if (capture and b"Framebuffer screenshot saved to '" in pending
                            and Path(capture[1]).is_file()):
                        return 0
                    exited = re.search(rb"arch: exit return code (-?[0-9]+)\r?\n", pending)
                    if exited:
                        return int(exited[1]) or 0
                    if re.search(rb"(?:Out of memory|GEX: Memory Error|Unhandled exception|kernel panic:|\[win32hle\] FATAL:|arch: aborting)[^\r\n]*\r?\n", pending):
                        return 1
                    pending = pending[-256:]
                if process.poll() is not None:
                    return process.returncode
                return 124
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
            process.stdout.close()


if __name__ == "__main__":
    sys.exit(main())
