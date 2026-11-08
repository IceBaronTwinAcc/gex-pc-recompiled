#!/usr/bin/env python3
"""Exercise automatic browser renderer selection against a running Web build."""

import argparse
from pathlib import Path

from playwright.sync_api import sync_playwright


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", default="http://127.0.0.1:8000/gex_recomp.html")
    parser.add_argument("--game-dir", type=Path, default=Path("Gex"))
    parser.add_argument("--webgpu", action="store_true",
                        help="also exercise WebGPU with Chromium's test-only SwiftShader flags")
    args = parser.parse_args()
    game_dir = args.game_dir.resolve()
    if not game_dir.is_dir():
        parser.error(f"game directory does not exist: {game_dir}")

    cases = [
        ("no adapter", ["--disable-webgpu"], None, "software"),
        ("no canvas transfer", [], None, "software"),
        ("no WebGPU API", [], "undefined", "software"),
        ("missing feature", [], "{requestAdapter: async () => ({features: new Set()})}", "software"),
        ("adapter timeout", [], "{requestAdapter: () => new Promise(() => {})}", "software"),
        ("device rejected", [], """{requestAdapter: async () => ({
            features: new Set(['indirect-first-instance']),
            requestDevice: async () => { throw new Error('test device failure'); }
        })}""", "software"),
        ("device timeout", [], """{requestAdapter: async () => ({
            features: new Set(['indirect-first-instance']),
            requestDevice: () => new Promise(() => {})
        })}""", "software"),
    ]
    if args.webgpu:
        cases.append(("WebGPU", ["--enable-unsafe-webgpu", "--use-angle=swiftshader",
                                  "--enable-features=Vulkan"], None, "bgfx"))

    with sync_playwright() as playwright:
        for label, flags, mock_gpu, expected in cases:
            browser = playwright.chromium.launch(headless=True, args=["--mute-audio", *flags])
            try:
                context = browser.new_context()
                if mock_gpu is not None:
                    def patch_script(route):
                        response = route.fetch()
                        prefix = """if (typeof WorkerGlobalScope !== 'undefined' && self instanceof WorkerGlobalScope) {
                            Object.defineProperty(navigator, 'gpu', {value: API, configurable: true});
                        }
                        """.replace("API", mock_gpu)
                        route.fulfill(response=response, body=prefix + response.text())
                    context.route("**/gex_recomp.js", patch_script)
                page = context.new_page()
                if label == "no canvas transfer":
                    page.add_init_script(
                        "HTMLCanvasElement.prototype.transferControlToOffscreen = undefined;")
                errors = []
                page.on("pageerror", lambda error: errors.append(str(error)))
                page.goto(args.url, wait_until="load")
                page.wait_for_function("() => !document.querySelector('#remember-game').disabled",
                                       timeout=45000)
                page.locator("#remember-game").uncheck()
                page.locator("#game-folder").set_input_files(str(game_dir))
                page.wait_for_function(
                    """expected => {
                        const log = document.querySelector('#output').textContent;
                        return log.includes('[renderer] selected ' + expected + ' packet backend') ||
                               log.includes('[win32hle] FATAL:');
                    }""", arg=expected, timeout=80000)
                log = page.locator("#output").text_content()
                if f"[renderer] selected {expected} packet backend" not in log or "[win32hle] FATAL:" in log or errors:
                    raise AssertionError(f"{label}: {log}\nBrowser errors: {errors}")
                page.wait_for_function(
                    "() => document.querySelector('#status').textContent === 'Gex is running.'",
                    timeout=30000)
                if errors:
                    raise AssertionError(f"{label}: browser errors: {errors}")
                print(f"{label}: {expected} PASS")
            finally:
                browser.close()


if __name__ == "__main__":
    main()
