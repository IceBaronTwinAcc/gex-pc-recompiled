#!/usr/bin/env python3
"""Verify Web builds keep software rendering with or without WebGPU available."""

import argparse
from pathlib import Path

from playwright.sync_api import sync_playwright


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", default="http://127.0.0.1:8000/gex_recomp.html")
    parser.add_argument("--game-dir", type=Path, default=Path("Gex"))
    parser.add_argument("--webgpu", action="store_true",
                        help="verify software selection when Chromium's SwiftShader adapter is available")
    args = parser.parse_args()
    game_dir = args.game_dir.resolve()
    if not game_dir.is_dir():
        parser.error(f"game directory does not exist: {game_dir}")

    cases = [("WebGPU disabled", ["--disable-webgpu"], False)]
    if args.webgpu:
        cases.append(("WebGPU available", ["--enable-unsafe-webgpu", "--use-angle=swiftshader",
                                            "--enable-features=Vulkan"], True))

    with sync_playwright() as playwright:
        for label, flags, require_webgpu in cases:
            browser = playwright.chromium.launch(headless=True, args=["--mute-audio", *flags])
            try:
                page = browser.new_page()
                errors = []
                page.on("pageerror", lambda error: errors.append(str(error)))
                page.goto(args.url, wait_until="load")
                if require_webgpu and not page.evaluate("""async () => {
                    if (!navigator.gpu) return false;
                    return !!(await navigator.gpu.requestAdapter());
                }"""):
                    raise AssertionError(f"{label}: Chromium's WebGPU adapter is unavailable")
                page.wait_for_function("() => !document.querySelector('#remember-game').disabled",
                                       timeout=45000)
                page.locator("#remember-game").uncheck()
                page.locator("#game-folder").set_input_files(str(game_dir))
                page.wait_for_function(
                    """() => {
                        const log = document.querySelector('#output').textContent;
                        return log.includes('[renderer] selected software packet backend') ||
                               log.includes('[win32hle] FATAL:');
                    }""", timeout=80000)
                log = page.locator("#output").text_content()
                if ("[renderer] selected software packet backend" not in log or
                        "[renderer] selected bgfx packet backend" in log or
                        "[win32hle] FATAL:" in log or errors):
                    raise AssertionError(f"{label}: {log}\nBrowser errors: {errors}")
                page.wait_for_function(
                    "() => document.querySelector('#status').textContent === 'Gex is running.'",
                    timeout=30000)
                if errors:
                    raise AssertionError(f"{label}: browser errors: {errors}")
                print(f"{label}: software PASS")
            finally:
                browser.close()


if __name__ == "__main__":
    main()
