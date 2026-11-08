#!/usr/bin/env python3
"""Verify that the browser plays both original intro AVI movies."""

import argparse
from functools import partial
from http.server import ThreadingHTTPServer
from pathlib import Path
from threading import Thread

from playwright.sync_api import TimeoutError as PlaywrightTimeoutError
from playwright.sync_api import sync_playwright

from wasm_coi import Handler, PREFIX, run_game, wait_ready


EXPECTED = (
    "[movie] playback completed: video=457 audio=335899",
    "[movie] playback completed: video=867 audio=1274490",
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--web-dir", type=Path, default=Path("build/wasm/web"))
    parser.add_argument("--game-dir", type=Path, default=Path("Gex"))
    parser.add_argument("--html", default="gex_recomp.html")
    parser.add_argument("--timeout", type=int, default=150,
                        help="maximum seconds to wait for both movies to finish")
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    web_dir = args.web_dir.resolve()
    game_dir = args.game_dir.resolve()
    for path in (web_dir / args.html, web_dir / "gex-coi-serviceworker.js",
                 web_dir / "GexRecompLogo.png", game_dir / "GEX.exe",
                 game_dir / "AVI/GEX000.AVI", game_dir / "AVI/GEX001.AVI"):
        if not path.is_file():
            parser.error(f"required file not found: {path}")

    server = ThreadingHTTPServer(
        ("127.0.0.1", 0), partial(Handler, directory=str(web_dir)))
    server.requests = []
    server.fail_worker = False
    server.fail_runtime = False
    server.isolated = True
    thread = Thread(target=server.serve_forever, daemon=True)
    thread.start()
    url = f"http://127.0.0.1:{server.server_port}{PREFIX}{args.html}"
    try:
        with sync_playwright() as playwright:
            browser = playwright.chromium.launch(headless=True, args=["--mute-audio"])
            try:
                page = browser.new_page()
                page.add_init_script(
                    "Object.defineProperty(navigator, 'getGamepads', {value: () => []});")
                errors = []
                page.on("pageerror", lambda error: errors.append(str(error)))
                page.goto(url)
                wait_ready(page)
                run_game(page, game_dir, "software")
                try:
                    page.wait_for_function(
                        "expected => expected.every(line => "
                        "document.querySelector('#output').textContent.includes(line))",
                        arg=list(EXPECTED),
                        timeout=args.timeout * 1000,
                    )
                except PlaywrightTimeoutError as error:
                    raise AssertionError(
                        "movie playback timed out\n"
                        f"status: {page.locator('#status').text_content()}\n"
                        f"runtime log:\n{page.locator('#output').text_content()}"
                    ) from error
                if errors:
                    raise AssertionError(f"browser errors: {errors}")
                print("WebAssembly intro movie playback: both AVI movies PASS")
            finally:
                browser.close()
    finally:
        server.shutdown()
        server.server_close()
        thread.join()


if __name__ == "__main__":
    main()
