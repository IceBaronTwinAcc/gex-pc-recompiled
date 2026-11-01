"""Verify that unused DirectDraw, networking and Miles modules are not linked."""

import subprocess
import sys

symbols = subprocess.check_output([sys.argv[2], sys.argv[1]], text=True)
for forbidden in ("hle_dd_", "win32hle_ddraw", "dd_Create", "s_Blt",
                  "win32hle_wsock32", "hle_ws_poll", "w_socket", "win32hle_mss"):
    if forbidden in symbols:
        raise AssertionError(f"Unused implementation remains linked: {forbidden}")
for required in ("gex_renderer_commit", "gex_renderer_upload_bgr555",
                 "gex_graphics_resolution", "gex_graphics_flush"):
    if required not in symbols:
        raise AssertionError(f"Renderer entry missing: {required}")
