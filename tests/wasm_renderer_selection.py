#!/usr/bin/env python3
"""Verify Web builds keep software rendering with or without WebGPU available."""

import argparse
import re
from pathlib import Path

from playwright.sync_api import sync_playwright


def legacy_square_settings():
    root = r"HKEY_CURRENT_USER\Software\GexRecomp\Options"
    bindings = [(67, 0x2E, 32), (67, 0x2E, 32), (90, 0x2C, 64),
                (88, 0x2D, 16), (86, 0x2F, 128), (27, 1, 256),
                (88, 0x2D, 16), (67, 0x2E, 32)]
    lines = [f"{root}\\Version=dword:0x1"]
    for index, (key, scan, pad) in enumerate(bindings):
        lines.extend((f"{root}\\Key{index}=dword:0x{(scan << 16) | key:x}",
                      f"{root}\\Pad{index}=dword:0x{pad:x}"))
    lines.extend(f"{root}\\Volume{index}=dword:0x64" for index in range(3))
    lines.extend(f"{root}\\{name}=dword:0x{value:x}" for name, value in (
        ("Renderer", 0), ("API", 0), ("Display", 1),
        ("Width", 240), ("Height", 240), ("Triple", 0)))
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", default="http://127.0.0.1:8000/gex_recomp.html")
    parser.add_argument("--game-dir", type=Path, default=Path("Gex"))
    parser.add_argument("--webgpu", action="store_true",
                        help="verify software selection when Chromium's SwiftShader adapter is available")
    parser.add_argument("--firefox", action="store_true",
                        help="also verify software and fullscreen backing size in Firefox")
    args = parser.parse_args()
    game_dir = args.game_dir.resolve()
    if not game_dir.is_dir():
        parser.error(f"game directory does not exist: {game_dir}")

    cases = [("WebGPU disabled", ["--disable-webgpu"], False)]
    if args.webgpu:
        cases.append(("WebGPU available", ["--enable-unsafe-webgpu", "--use-angle=swiftshader",
                                            "--enable-features=Vulkan"], True))

    with sync_playwright() as playwright:
        browser_cases = [("Chromium", playwright.chromium, cases)]
        if args.firefox:
            browser_cases.append(("Firefox", playwright.firefox, [("software", [], False)]))
        for browser_name, browser_type, engine_cases in browser_cases:
            for label, flags, require_webgpu in engine_cases:
                if browser_name == "Firefox":
                    browser = browser_type.launch(
                        headless=True, firefox_user_prefs={"media.volume_scale": "0.0"})
                else:
                    browser = browser_type.launch(headless=True, args=["--mute-audio", *flags])
                context = browser.new_context(
                    viewport={"width": 1252, "height": 1180},
                    device_scale_factor=2,
                )
                try:
                    page = context.new_page()
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
                        raise AssertionError(
                            f"{browser_name} {label}: {log}\nBrowser errors: {errors}")
                    viewport_match = re.search(
                        r"\[renderer\] level viewport (\d+)x(\d+)", log)
                    if not viewport_match:
                        raise AssertionError(
                            f"{browser_name} {label}: game render resolution was not logged")
                    render_width, render_height = map(int, viewport_match.groups())
                    if render_width * 9 < render_height * 16:
                        raise AssertionError(
                            f"{browser_name} {label}: default Web render resolution is not "
                            f"widescreen: {render_width}x{render_height}")
                    page.wait_for_function(
                        "() => document.querySelector('#status').textContent === 'Gex is running.'",
                        timeout=30000)
                    dimensions = page.evaluate("""() => {
                        const canvas = document.querySelector('#canvas');
                        const rect = canvas.getBoundingClientRect();
                        return {width: canvas.width, height: canvas.height,
                                cssWidth: canvas.clientWidth, cssHeight: canvas.clientHeight,
                                rect: {width: rect.width, height: rect.height}};
                    }""")
                    if (dimensions["width"] > dimensions["cssWidth"] or
                            dimensions["height"] > dimensions["cssHeight"]):
                        raise AssertionError(
                            f"{browser_name} {label}: high-DPI canvas backing is oversized: "
                            f"{dimensions}")
                    if abs(dimensions["rect"]["width"] / dimensions["rect"]["height"] -
                           dimensions["width"] / dimensions["height"]) > 0.01:
                        raise AssertionError(
                            f"{browser_name} {label}: windowed canvas aspect ratio does not "
                            f"match its backing: {dimensions}")
                    page.locator("#fullscreen").click()
                    page.wait_for_function(
                        "() => document.fullscreenElement === document.querySelector('#canvas')",
                        timeout=5000)
                    page.wait_for_timeout(500)
                    fullscreen_dimensions = page.evaluate("""() => {
                        const canvas = document.querySelector('#canvas');
                        const rect = canvas.getBoundingClientRect();
                        return {width: canvas.width, height: canvas.height,
                                cssWidth: canvas.clientWidth, cssHeight: canvas.clientHeight,
                                rect: {x: rect.x, y: rect.y, width: rect.width, height: rect.height},
                                viewport: {width: innerWidth, height: innerHeight},
                                objectFit: getComputedStyle(canvas).objectFit};
                    }""")
                    if (fullscreen_dimensions["width"] != dimensions["width"] or
                            fullscreen_dimensions["height"] != dimensions["height"]):
                        raise AssertionError(
                            f"{browser_name} {label}: fullscreen changed the software canvas "
                            f"backing from {dimensions} to {fullscreen_dimensions}")
                    rect = fullscreen_dimensions["rect"]
                    viewport = fullscreen_dimensions["viewport"]
                    if (abs(rect["x"]) > 1 or abs(rect["y"]) > 1 or
                            abs(rect["width"] - viewport["width"]) > 1 or
                            abs(rect["height"] - viewport["height"]) > 1 or
                            fullscreen_dimensions["objectFit"] != "contain"):
                        raise AssertionError(
                            f"{browser_name} {label}: fullscreen canvas is not fitted to the "
                            f"viewport without cropping: {fullscreen_dimensions}")
                    if errors:
                        raise AssertionError(f"{browser_name} {label}: browser errors: {errors}")
                    print(f"{browser_name} {label}: software and fullscreen canvas fit PASS")
                finally:
                    context.close()
                    browser.close()

        browser = playwright.chromium.launch(
            headless=True, args=["--mute-audio", "--disable-webgpu"])
        context = browser.new_context(
            viewport={"width": 1252, "height": 1180},
            device_scale_factor=2,
        )
        try:
            page = context.new_page()
            errors = []
            page.on("pageerror", lambda error: errors.append(str(error)))
            page.goto(args.url, wait_until="load")
            page.wait_for_function("() => !document.querySelector('#remember-game').disabled",
                                   timeout=45000)
            page.evaluate("""async settings => {
                FS.writeFile('/save/settings.reg', settings);
                await new Promise((resolve, reject) =>
                    FS.syncfs(false, error => error ? reject(error) : resolve()));
            }""", legacy_square_settings())
            page.locator("#remember-game").uncheck()
            page.locator("#game-folder").set_input_files(str(game_dir))
            page.wait_for_function(
                """() => {
                    const log = document.querySelector('#output').textContent;
                    return log.includes('[renderer] level viewport') ||
                           log.includes('[win32hle] FATAL:');
                }""", timeout=80000)
            log = page.locator("#output").text_content()
            if ("[options] migrated unsupported saved resolution 240x240 to 427x240" not in log or
                    "[renderer] level viewport 427x240" not in log or
                    "[win32hle] FATAL:" in log or errors):
                raise AssertionError(f"Legacy square resolution was not migrated:\n{log}\n"
                                     f"Browser errors: {errors}")
            page.wait_for_function(
                "() => document.querySelector('#status').textContent === 'Gex is running.'",
                timeout=30000)
            dimensions = page.evaluate("""() => {
                const canvas = document.querySelector('#canvas');
                return {width: canvas.width, height: canvas.height,
                        fullscreen: document.fullscreenElement === canvas};
            }""")
            if (not dimensions["fullscreen"] or
                    abs(dimensions["width"] / dimensions["height"] - 16 / 9) > 0.01):
                raise AssertionError(
                    f"Startup fullscreen did not preserve the game aspect ratio: {dimensions}\n"
                    f"Runtime log:\n{log}")
            print("Chromium legacy square resolution migration PASS")
        finally:
            context.close()
            browser.close()


if __name__ == "__main__":
    main()
