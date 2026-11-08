#!/usr/bin/env python3
"""Exercise the built web port on a GitHub Pages-like, headerless subpath."""

import argparse
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from threading import Thread
from urllib.request import urlopen

from playwright.sync_api import sync_playwright


PREFIX = "/gex-pc-recompiled/"


class Handler(SimpleHTTPRequestHandler):
    def log_message(self, format, *args):
        pass

    def do_GET(self):
        self.server.requests.append(self.path.split("?", 1)[0])
        if not self.path.startswith(PREFIX):
            self.send_error(404)
            return
        if self.server.fail_worker and self.path.split("?", 1)[0].endswith("/gex-coi-serviceworker.js"):
            self.send_error(404)
            return
        if self.server.fail_runtime and self.path.split("?", 1)[0].endswith("/gex_recomp.js"):
            self.send_error(404)
            return
        self.path = "/" + self.path[len(PREFIX):]
        super().do_GET()

    def end_headers(self):
        if self.server.isolated:
            self.send_header("Cross-Origin-Opener-Policy", "same-origin")
            self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        super().end_headers()


def wait_ready(page):
    page.wait_for_function(
        "() => window.crossOriginIsolated && typeof Module.gexImportGameFiles === 'function'",
        timeout=45000,
    )


def run_game(page, game_dir, expected):
    page.locator("#game-folder").set_input_files(str(game_dir))
    page.wait_for_function(
        """expected => {
            const log = document.querySelector('#output').textContent;
            return log.includes('[renderer] selected ' + expected + ' packet backend') ||
                   log.includes('[win32hle] FATAL:');
        }""", arg=expected, timeout=80000,
    )
    log = page.locator("#output").text_content()
    if f"[renderer] selected {expected} packet backend" not in log or "[win32hle] FATAL:" in log:
        raise AssertionError(log)
    page.wait_for_function(
        "() => document.querySelector('#status').textContent === 'Gex is running.'",
        timeout=30000,
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--web-dir", type=Path, default=Path("build/wasm/web"))
    parser.add_argument("--game-dir", type=Path, default=Path("Gex"))
    parser.add_argument("--webgpu", action="store_true")
    args = parser.parse_args()
    web_dir = args.web_dir.resolve()
    game_dir = args.game_dir.resolve()
    for path in (web_dir / "gex_recomp.html", web_dir / "gex-coi-serviceworker.js", game_dir / "GEX.exe"):
        if not path.is_file():
            parser.error(f"required file not found: {path}")

    server = ThreadingHTTPServer(("127.0.0.1", 0), partial(Handler, directory=str(web_dir)))
    server.requests = []
    server.fail_worker = False
    server.fail_runtime = False
    server.isolated = False
    thread = Thread(target=server.serve_forever, daemon=True)
    thread.start()
    url = f"http://127.0.0.1:{server.server_port}{PREFIX}gex_recomp.html"
    try:
        with urlopen(url, timeout=5) as response:
            assert response.headers.get("Cross-Origin-Opener-Policy") is None
            assert response.headers.get("Cross-Origin-Embedder-Policy") is None
        with sync_playwright() as playwright:
            cases = [("software", ["--disable-webgpu"])]
            if args.webgpu:
                cases.append(("bgfx", ["--enable-unsafe-webgpu", "--use-angle=swiftshader",
                                       "--enable-features=Vulkan"]))
            for expected, flags in cases:
                server.requests.clear()
                browser = playwright.chromium.launch(headless=True, args=["--mute-audio", *flags])
                try:
                    context = browser.new_context()
                    page = context.new_page()
                    errors = []
                    page.on("pageerror", lambda error: errors.append(str(error)))
                    page.goto(url)
                    wait_ready(page)
                    assert page.evaluate("typeof SharedArrayBuffer") == "function"
                    assert page.evaluate("!!navigator.serviceWorker.controller")
                    assert page.evaluate("fetch('missing-asset').then(r => r.status)") == 404
                    html = PREFIX + "gex_recomp.html"
                    script = PREFIX + "gex_recomp.js"
                    assert server.requests.count(html) >= 2, server.requests
                    assert server.requests.index(script) > server.requests.index(html, server.requests.index(html) + 1), server.requests
                    page.reload()
                    wait_ready(page)
                    run_game(page, game_dir, expected)
                    if errors:
                        raise AssertionError(errors)
                    print(f"headerless subpath, reload and {expected}: PASS")
                finally:
                    browser.close()

            server.isolated = True
            browser = playwright.chromium.launch(headless=True, args=["--mute-audio"])
            try:
                page = browser.new_page()
                page.goto(url)
                wait_ready(page)
                assert page.evaluate("navigator.serviceWorker.controller === null")
                assert not page.evaluate("navigator.serviceWorker.getRegistrations().then(r => r.length)")
                print("server-provided isolation without service worker: PASS")
            finally:
                browser.close()

            server.isolated = False
            server.fail_runtime = True
            browser = playwright.chromium.launch(headless=True, args=["--mute-audio"])
            try:
                page = browser.new_page()
                page.goto(url)
                page.wait_for_function(
                    "() => document.querySelector('#status').textContent.includes('Could not load the WebAssembly runtime script')",
                    timeout=15000,
                )
                assert page.locator("#choose-folder").is_disabled()
                print("missing runtime script reports failure: PASS")
            finally:
                browser.close()

            server.fail_runtime = False
            server.fail_worker = True
            server.requests.clear()
            browser = playwright.chromium.launch(headless=True, args=["--mute-audio"])
            try:
                page = browser.new_page()
                page.goto(url)
                page.wait_for_function(
                    "() => document.querySelector('#status').textContent.includes('Could not enable cross-origin isolation')",
                    timeout=15000,
                )
                assert page.locator("#choose-folder").is_disabled()
                assert PREFIX + "gex_recomp.js" not in server.requests
                print("missing service worker reports failure without starting runtime: PASS")
            finally:
                browser.close()
    finally:
        server.shutdown()
        server.server_close()
        thread.join()


if __name__ == "__main__":
    main()
