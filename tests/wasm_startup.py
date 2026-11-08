#!/usr/bin/env python3
"""Check audio activation and SDL image/backing agreement at browser startup."""

import argparse
from functools import partial
from http.server import ThreadingHTTPServer
from pathlib import Path
from threading import Thread
from urllib.parse import unquote

from playwright.sync_api import sync_playwright

from wasm_coi import Handler, PREFIX, run_game, wait_ready


class StartupHandler(Handler):
    def translate_path(self, path):
        if path.startswith("/_startup_game/"):
            relative = unquote(path[len("/_startup_game/"):].split("?", 1)[0])
            candidate = (self.server.game_dir / relative).resolve()
            if candidate.is_relative_to(self.server.game_dir):
                return str(candidate)
            return str(self.server.game_dir / "missing-startup-test-file")
        return super().translate_path(path)


def saved_options(display):
    values = {"Version": 1, "Renderer": 0, "API": 0, "Display": display,
              "Width": 427, "Height": 240, "Triple": 0}
    bindings = ((67, 46, 32), (67, 46, 32), (90, 44, 64), (88, 45, 16),
                (86, 47, 128), (27, 1, 256), (88, 45, 16), (67, 46, 32))
    for i, (key, scan, pad) in enumerate(bindings):
        values[f"Key{i}"] = key | scan << 16
        values[f"Pad{i}"] = pad
    values.update({f"Volume{i}": 100 for i in range(3)})
    return "".join(
        f"{root}\\Software\\GexRecomp\\Options\\{key}=dword:{value}\n"
        for root in ("HKCU", "HKLM") for key, value in values.items())


PROBE = """(() => {
    Object.defineProperty(screen, 'width', {value: 2560});
    Object.defineProperty(screen, 'height', {value: 1440});
    window.startupProbe = {writes: 0, mismatches: [], times: []};
    const original = CanvasRenderingContext2D.prototype.putImageData;
    CanvasRenderingContext2D.prototype.putImageData = function (...args) {
        if (this.canvas.id === 'canvas') {
            const p = startupProbe;
            ++p.writes;
            p.times.push(performance.now());
            if (args[0].width !== this.canvas.width ||
                args[0].height !== this.canvas.height)
                p.mismatches.push({
                    image: [args[0].width, args[0].height],
                    backing: [this.canvas.width, this.canvas.height]
                });
        }
        return original.apply(this, args);
    };
})()"""

SUSPEND_AUDIO = """(() => {
    const AudioContext = window.AudioContext;
    window.AudioContext = class extends AudioContext {
        constructor(...args) {
            super(...args);
            this.suspend().catch(error => console.error(error));
        }
    };
})()"""


def assert_presentation(page):
    result = page.evaluate("""(() => {
        const c = Module.canvas, r = c.getBoundingClientRect();
        return {...startupProbe, backing: [c.width, c.height],
                rect: [r.width, r.height], fullscreen: !!document.fullscreenElement,
                inlineSize: [c.style.width, c.style.height]};
    })()""")
    assert result["writes"] > 0, result
    assert not result["mismatches"], result
    assert result["backing"] == [1100, 618], result
    if not result["fullscreen"]:
        assert result["rect"] == [1100, 618], result
        assert result["inlineSize"] == ["", ""], result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--web-dir", type=Path, default=Path("build/wasm/web"))
    parser.add_argument("--game-dir", type=Path, default=Path("Gex"))
    parser.add_argument("--browser", choices=("chromium", "firefox"), default="chromium")
    args = parser.parse_args()
    web_dir, game_dir = args.web_dir.resolve(), args.game_dir.resolve()
    for path in (web_dir / "gex_recomp.html", game_dir / "GEX.exe"):
        if not path.is_file():
            parser.error(f"required file not found: {path}")
    server = ThreadingHTTPServer(("127.0.0.1", 0), partial(StartupHandler, directory=str(web_dir)))
    server.game_dir = game_dir
    server.requests = []
    server.fail_worker = server.fail_runtime = False
    server.isolated = True
    thread = Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        with sync_playwright() as p:
            browser = (p.firefox.launch(headless=True,
                       firefox_user_prefs={"media.volume_scale": "0.0",
                                           "media.autoplay.default": 1,
                                           "media.autoplay.block-webaudio": True,
                                           "media.autoplay.blocking_policy": 2})
                       if args.browser == "firefox" else
                       p.chromium.launch(headless=True, args=[
                           "--mute-audio", "--autoplay-policy=document-user-activation-required"]))
            try:
                context = browser.new_context(viewport={"width": 1518, "height": 1218})
                try:
                    page = context.new_page()
                    page.add_init_script(PROBE)
                    page.add_init_script(SUSPEND_AUDIO)
                    page.goto(f"http://127.0.0.1:{server.server_port}{PREFIX}gex_recomp.html")
                    wait_ready(page)
                    page.wait_for_function(
                        "() => !document.querySelector('#remember-game').disabled")
                    page.evaluate("document.querySelector('#remember-game').checked = false")
                    files = [path.relative_to(game_dir).as_posix()
                             for path in game_dir.rglob("*") if path.is_file()]
                    page.evaluate("""async files => {
                        const entries = [];
                        for (const path of files) {
                            const response = await fetch('_startup_game/' + path);
                            if (!response.ok) throw new Error(path + ': ' + response.status);
                            entries.push({path: '/Gex/' + path, file: await response.blob()});
                        }
                        await gexLoadGameEntries(entries, gexValidateGameEntries(entries), false);
                    }""", files)
                    page.wait_for_function(
                        "() => document.querySelector('#status').textContent === "
                        "'Click the game window for sound.'")
                    page.wait_for_timeout(35000)
                    log = page.locator("#output").text_content()
                    assert "[movie] opened" not in log, log
                    assert "[movie] playback cancelled" not in log, log
                    assert page.locator("#status").text_content() == \
                        "Click the game window for sound."
                    page.locator("#canvas").click(position={"x": 100, "y": 100})
                    page.wait_for_function(
                        "() => document.querySelector('#output').textContent.includes("
                        "'[movie] opened AVI')")
                    page.wait_for_timeout(2500)
                    assert_presentation(page)
                    assert not page.evaluate("!!document.fullscreenElement")
                    print(f"{args.browser} {browser.version}: "
                          "35-second audio wait and windowed activation PASS")
                finally:
                    context.close()
                for display in (0, 1, 2):
                    context = browser.new_context(viewport={"width": 1518, "height": 1218})
                    try:
                        page = context.new_page()
                        errors = []
                        page.on("pageerror", lambda error: errors.append(str(error)))
                        page.add_init_script(PROBE)
                        page.goto(f"http://127.0.0.1:{server.server_port}{PREFIX}gex_recomp.html")
                        wait_ready(page)
                        page.wait_for_function(
                            "() => !document.querySelector('#remember-game').disabled")
                        page.evaluate("text => Module.FS.writeFile('/save/settings.reg', text)",
                                      saved_options(display))
                        run_game(page, game_dir, "software")
                        page.wait_for_timeout(2500)
                        assert_presentation(page)
                        page.locator("#canvas").click(position={"x": 100, "y": 100})
                        page.wait_for_timeout(2500)
                        assert_presentation(page)
                        if args.browser == "chromium":
                            if page.evaluate("!!document.fullscreenElement"):
                                page.evaluate("document.exitFullscreen()")
                                page.wait_for_function("() => !document.fullscreenElement")
                                page.wait_for_timeout(1000)
                                assert_presentation(page)
                            page.locator("#fullscreen").click()
                            page.wait_for_function("() => document.fullscreenElement === Module.canvas")
                            page.wait_for_timeout(1000)
                            assert_presentation(page)
                            page.evaluate("document.exitFullscreen()")
                            page.wait_for_function("() => !document.fullscreenElement")
                            page.wait_for_timeout(1000)
                            assert_presentation(page)
                        assert not errors, errors
                        print(f"{args.browser} {browser.version}, saved display {display}: "
                              "startup and gesture presentation PASS")
                    finally:
                        context.close()
            finally:
                browser.close()
    finally:
        server.shutdown()
        server.server_close()
        thread.join()


if __name__ == "__main__":
    main()
