#!/usr/bin/env python3
"""Verify that exiting from the game's main menu also leaves browser fullscreen."""

import argparse
from functools import partial
from http.server import ThreadingHTTPServer
from pathlib import Path
from threading import Thread

from playwright.sync_api import sync_playwright

from wasm_coi import Handler, PREFIX, run_game, wait_ready


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--web-dir", type=Path, default=Path("build/wasm/web"))
    parser.add_argument("--game-dir", type=Path, default=Path("Gex"))
    args = parser.parse_args()
    web_dir, game_dir = args.web_dir.resolve(), args.game_dir.resolve()
    for path in (web_dir / "gex_recomp.html", game_dir / "GEX.exe"):
        if not path.is_file():
            parser.error(f"required file not found: {path}")
    server = ThreadingHTTPServer(("127.0.0.1", 0), partial(Handler, directory=str(web_dir)))
    server.requests = []
    server.fail_worker = server.fail_runtime = False
    server.isolated = True
    thread = Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        with sync_playwright() as p:
            browser = p.chromium.launch(headless=True, args=["--mute-audio"])
            try:
                page = browser.new_page()
                errors = []
                page.on("pageerror", lambda error: errors.append(str(error)))
                page.goto(f"http://127.0.0.1:{server.server_port}{PREFIX}gex_recomp.html")
                wait_ready(page)
                page.evaluate("Module.arguments.push('--skip-movies')")
                run_game(page, game_dir, "software")
                page.wait_for_timeout(3000)
                page.locator("#fullscreen").click()
                page.wait_for_function("() => document.fullscreenElement === Module.canvas")
                for key in ("ArrowUp", "x"):
                    page.keyboard.down(key)
                    page.wait_for_timeout(150)
                    page.keyboard.up(key)
                    page.wait_for_timeout(150)
                page.wait_for_function(
                    "() => document.querySelector('#output').textContent.includes('ExitProcess(0)')",
                    timeout=15000)
                log = page.locator("#output").text_content()
                assert "[MessageBox] GEX: Are you sure you" in log, log
                page.wait_for_function("() => !document.fullscreenElement", timeout=5000)
                page.wait_for_function(
                    "() => document.querySelector('#status').textContent === "
                    "'Gex has exited. Reload the page to play again.'", timeout=5000)
                page.mouse.click(100, 100)
                page.wait_for_timeout(500)
                assert not page.evaluate("!!document.fullscreenElement")
                assert not errors, errors
                print("Main-menu exit leaves fullscreen and subsequent clicks stay windowed: PASS")
            finally:
                browser.close()
    finally:
        server.shutdown()
        server.server_close()
        thread.join()


if __name__ == "__main__":
    main()
