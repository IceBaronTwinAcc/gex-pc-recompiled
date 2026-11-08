#!/usr/bin/env python3
"""Verify browser-local game imports survive restarts and fail safely."""

import argparse
import hashlib
import tempfile
import zlib
from functools import partial
from http.server import ThreadingHTTPServer
from pathlib import Path
from threading import Thread

from playwright.sync_api import TimeoutError as PlaywrightTimeoutError, sync_playwright

from wasm_coi import Handler, PREFIX


class StorageHandler(Handler):
    def do_GET(self):
        if self.path.split("?", 1)[0] == PREFIX + "seed":
            self.server.requests.append(self.path.split("?", 1)[0])
            body = b"<!doctype html><meta charset=utf-8><title>Storage test</title>"
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        super().do_GET()


def wait_picker(page):
    page.wait_for_function(
        "() => !document.querySelector('#choose-folder').disabled", timeout=60000
    )


def wait_game(page):
    page.wait_for_function(
        """() => {
          const log = document.querySelector('#output').textContent;
          return document.querySelector('#status').textContent === 'Gex is running.' ||
                 log.includes('[win32hle] FATAL:');
        }""", timeout=120000,
    )
    log = page.locator("#output").text_content()
    if "[renderer] selected software packet backend" not in log or "[win32hle] FATAL:" in log:
        raise AssertionError(log)


def wait_log(page, message, timeout=30000):
    try:
        page.wait_for_function(
            "(message) => document.querySelector('#output').textContent.includes(message)",
            arg=message,
            timeout=timeout,
        )
    except PlaywrightTimeoutError as error:
        databases = page.evaluate("""async () => {
          const names = (await indexedDB.databases()).map(database => database.name);
          const name = "gex-recomp-saves:" + new URL(".", location.href).pathname;
          if (!names.includes(name)) return {names};
          const request = indexedDB.open(name, 21);
          const db = await new Promise((resolve, reject) => {
            request.onsuccess = () => resolve(request.result);
            request.onerror = () => reject(request.error);
          });
          try {
            return {names, keys: await new Promise((resolve, reject) => {
              const all = db.transaction("FILE_DATA", "readonly").objectStore("FILE_DATA").getAllKeys();
              all.onsuccess = () => resolve(all.result);
              all.onerror = () => reject(all.error);
            })};
          } finally {
            db.close();
          }
        }""")
        raise AssertionError(
            f"runtime did not log {message!r}\n"
            f"status: {page.locator('#status').text_content()}\n"
            f"save storage status: {page.locator('#save-storage-status').text_content()}\n"
            f"databases: {databases}\n"
            f"log:\n{page.locator('#output').text_content()}"
        ) from error


def stored_state(page):
    return page.evaluate("""async () => {
      const request = indexedDB.open('gex-recomp-game:' + new URL('.', location.href).pathname);
      const db = await new Promise((resolve, reject) => {
        request.onsuccess = () => resolve(request.result);
        request.onerror = () => reject(request.error);
      });
      try {
        const tx = db.transaction('files', 'readonly');
        const store = tx.objectStore('files');
        const all = store.getAll();
        const entries = await new Promise((resolve, reject) => {
          all.onsuccess = () => resolve(all.result);
          all.onerror = () => reject(all.error);
        });
        const executable = entries.find(entry => entry.path === '/Gex/GEX.exe');
        let sha256 = null;
        if (executable) {
          const digest = await crypto.subtle.digest('SHA-256', await executable.file.arrayBuffer());
          sha256 = Array.from(new Uint8Array(digest), b => b.toString(16).padStart(2, '0')).join('');
        }
        return {count: entries.length, sha256};
      } finally {
        db.close();
      }
    }""")


def alter_saved_game(page, add_invalid):
    page.evaluate("""async add => {
      const request = indexedDB.open('gex-recomp-game:' + new URL('.', location.href).pathname);
      const db = await new Promise((resolve, reject) => {
        request.onsuccess = () => resolve(request.result);
        request.onerror = () => reject(request.error);
      });
      try {
        await new Promise((resolve, reject) => {
          const tx = db.transaction('files', 'readwrite');
          const store = tx.objectStore('files');
          if (add) store.put({path: '/Gex/../invalid', file: new Blob(['invalid'])});
          else store.delete('/Gex/../invalid');
          tx.oncomplete = resolve;
          tx.onabort = () => reject(tx.error);
        });
      } finally {
        db.close();
      }
    }""", add_invalid)


def save_fixtures(renderer=0):
    settings = ["HKEY_CURRENT_USER\\Software\\GexRecomp\\Options\\Version=dword:0x1"]
    keys = [(0x43, 0x2E), (0x43, 0x2E), (0x5A, 0x2C), (0x58, 0x2D),
            (0x56, 0x2F), (0x1B, 0x01), (0x58, 0x2D), (0x43, 0x2E)]
    buttons = [0x20, 0x20, 0x40, 0x10, 0x80, 0x100, 0x10, 0x20]
    for index, ((key, scan), button) in enumerate(zip(keys, buttons)):
        settings.extend((
            f"HKEY_CURRENT_USER\\Software\\GexRecomp\\Options\\Key{index}=dword:0x{scan:02x}{key:04x}",
            f"HKEY_CURRENT_USER\\Software\\GexRecomp\\Options\\Pad{index}=dword:0x{button:x}",
        ))
    settings.extend(
        f"HKEY_CURRENT_USER\\Software\\GexRecomp\\Options\\Volume{index}=dword:0x64"
        for index in range(3)
    )
    settings.extend((
        f"HKEY_CURRENT_USER\\Software\\GexRecomp\\Options\\Renderer=dword:0x{renderer:x}",
        "HKEY_CURRENT_USER\\Software\\GexRecomp\\Options\\API=dword:0x0",
        "HKEY_CURRENT_USER\\Software\\GexRecomp\\Options\\Display=dword:0x0",
        "HKEY_CURRENT_USER\\Software\\GexRecomp\\Options\\Width=dword:0x140",
        "HKEY_CURRENT_USER\\Software\\GexRecomp\\Options\\Height=dword:0xf0",
        "HKEY_CURRENT_USER\\Software\\GexRecomp\\Options\\Triple=dword:0x0",
    ))
    data = b"GEXDECK1" + bytes(27)
    passwords = data + zlib.crc32(data).to_bytes(4, "little")
    return "\n".join(settings) + "\n", passwords


def seed_save_files(page, settings, passwords, timestamp):
    page.evaluate("""async ({settings, passwords, timestampOffset}) => {
      const name = "gex-recomp-saves:" + new URL(".", location.href).pathname;
      const request = indexedDB.open(name, 21);
      const db = await new Promise((resolve, reject) => {
        request.onsuccess = () => resolve(request.result);
        request.onerror = () => reject(request.error);
      });
      try {
        await new Promise((resolve, reject) => {
          const tx = db.transaction("FILE_DATA", "readwrite");
          const store = tx.objectStore("FILE_DATA");
          const timestamp = new Date(Date.now() + timestampOffset * 60000);
          store.put({timestamp, mode: 0x81a4, contents: new TextEncoder().encode(settings)},
                    "/save/settings.reg");
          store.put({timestamp, mode: 0x81a4, contents: new Uint8Array(passwords)},
                    "/save/passwords.sav");
          tx.oncomplete = resolve;
          tx.onabort = () => reject(tx.error);
        });
      } finally {
        db.close();
      }
    }""", {
        "settings": settings,
        "passwords": list(passwords),
        "timestampOffset": timestamp,
    })


def verify_save_database(page, settings, passwords):
    actual = page.evaluate("""async () => {
      const name = "gex-recomp-saves:" + new URL(".", location.href).pathname;
      const request = indexedDB.open(name, 21);
      const db = await new Promise((resolve, reject) => {
        request.onsuccess = () => resolve(request.result);
        request.onerror = () => reject(request.error);
      });
      try {
        const tx = db.transaction("FILE_DATA", "readonly");
        const store = tx.objectStore("FILE_DATA");
        const read = path => new Promise((resolve, reject) => {
          const request = store.get(path);
          request.onsuccess = () => resolve(request.result && Array.from(request.result.contents));
          request.onerror = () => reject(request.error);
        });
        return {settings: await read("/save/settings.reg"), passwords: await read("/save/passwords.sav")};
      } finally {
        db.close();
      }
    }""")
    assert actual["settings"] == list(settings.encode("utf-8")), actual
    assert actual["passwords"] == list(passwords), actual


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--web-dir", type=Path, default=Path("build/wasm/web"))
    parser.add_argument("--game-dir", type=Path, default=Path("Gex"))
    parser.add_argument("--html", default="gex_recomp.html")
    args = parser.parse_args()
    web_dir = args.web_dir.resolve()
    game_dir = args.game_dir.resolve()
    executable = game_dir / "GEX.exe"
    if not (web_dir / args.html).is_file() or not executable.is_file():
        parser.error("built web output and a local game installation are required")
    expected_hash = hashlib.sha256(executable.read_bytes()).hexdigest()
    expected_count = sum(path.is_file() for path in game_dir.rglob("*"))

    server = ThreadingHTTPServer(("127.0.0.1", 0), partial(StorageHandler, directory=str(web_dir)))
    server.requests, server.fail_worker, server.fail_runtime, server.isolated = [], False, False, False
    thread = Thread(target=server.serve_forever, daemon=True)
    thread.start()
    url = f"http://127.0.0.1:{server.server_port}{PREFIX}{args.html}"
    try:
        with tempfile.TemporaryDirectory(prefix="gex-browser-storage-") as profile, sync_playwright() as playwright:
            def open_browser():
                return playwright.chromium.launch_persistent_context(
                    profile, headless=True, args=["--mute-audio", "--disable-webgpu"]
                )

            context = open_browser()
            try:
                page = context.pages[0]
                page.goto(url)
                wait_picker(page)
                assert page.locator("#remember-game").is_checked()
                page.locator("#game-folder").set_input_files(str(game_dir))
                wait_game(page)
                assert page.locator("#forget-game").is_enabled()
                assert stored_state(page) == {"count": expected_count, "sha256": expected_hash}
            finally:
                context.close()
            print("initial import persisted locally: PASS")

            settings, passwords = save_fixtures()
            old_webgpu_settings, old_webgpu_passwords = save_fixtures(renderer=4)
            context = open_browser()
            try:
                page = context.pages[0]
                page.goto(url.rsplit("/", 1)[0] + "/seed")
                seed_save_files(page, old_webgpu_settings, old_webgpu_passwords, 1)
                verify_save_database(page, old_webgpu_settings, old_webgpu_passwords)
            finally:
                context.close()

            context = open_browser()
            try:
                page = context.pages[0]
                page.goto(url)
                wait_log(page, "[options] saved renderer 4 is unavailable in this build; using renderer 0")
                wait_game(page)
                verify_save_database(page, old_webgpu_settings, old_webgpu_passwords)
            finally:
                context.close()
            print("saved WebGPU renderer falls back to software: PASS")

            invalid_settings = (
                "HKEY_CURRENT_USER\\Software\\GexRecomp\\Options\\Version=dword:0x1\n"
            )
            invalid_passwords = b"invalid password deck"
            context = open_browser()
            try:
                page = context.pages[0]
                page.goto(url.rsplit("/", 1)[0] + "/seed")
                seed_save_files(page, invalid_settings, passwords, 1)
                verify_save_database(page, invalid_settings, passwords)
            finally:
                context.close()

            context = open_browser()
            try:
                page = context.pages[0]
                page.goto(url)
                wait_log(page, "[options] saved Options settings are invalid or unavailable in this build")
                verify_save_database(page, invalid_settings, passwords)
            finally:
                context.close()

            context = open_browser()
            try:
                page = context.pages[0]
                page.goto(url.rsplit("/", 1)[0] + "/seed")
                seed_save_files(page, settings, invalid_passwords, 2)
                verify_save_database(page, settings, invalid_passwords)
            finally:
                context.close()

            context = open_browser()
            try:
                page = context.pages[0]
                page.goto(url)
                wait_log(page, "[saves] /save/passwords.sav: invalid save file")
                verify_save_database(page, settings, invalid_passwords)
            finally:
                context.close()

            context = open_browser()
            try:
                page = context.pages[0]
                page.goto(url.rsplit("/", 1)[0] + "/seed")
                seed_save_files(page, settings, passwords, 3)
                verify_save_database(page, settings, passwords)
            finally:
                context.close()

            context = open_browser()
            try:
                page = context.pages[0]
                page.goto(url)
                wait_game(page)
                assert "[saves] three password decks: /save/passwords.sav" in page.locator(
                    "#output"
                ).text_content()
            finally:
                context.close()
            print("settings and password decks restore before game startup: PASS")

            context = open_browser()
            try:
                page = context.pages[0]
                page.goto(url)
                wait_game(page)
                assert stored_state(page) == {"count": expected_count, "sha256": expected_hash}
                alter_saved_game(page, True)
                page.reload()
                page.wait_for_function(
                    "() => document.querySelector('#status').textContent.includes('Saved game files are invalid')",
                    timeout=60000,
                )
                wait_picker(page)
                before = stored_state(page)
                assert before["count"] == expected_count + 1
                page.evaluate("""() => {
                  IDBObjectStore.prototype.put = function () {
                    throw new DOMException('Simulated storage limit', 'QuotaExceededError');
                  };
                }""")
                page.locator("#game-folder").set_input_files(str(game_dir))
                page.wait_for_function(
                    "() => document.querySelector('#status').textContent.includes('could not save game files')",
                    timeout=60000,
                )
                assert stored_state(page) == before
                assert page.locator("#choose-folder").is_enabled()
                assert page.locator("#forget-game").is_enabled()
                page.reload()
                wait_picker(page)
                page.evaluate("""() => {
                  const originalPut = IDBObjectStore.prototype.put;
                  let scheduled = false;
                  IDBObjectStore.prototype.put = function (...args) {
                    const request = originalPut.apply(this, args);
                    if (!scheduled) {
                      scheduled = true;
                      request.onsuccess = () => this.transaction.abort();
                    }
                    return request;
                  };
                }""")
                page.locator("#game-folder").set_input_files(str(game_dir))
                page.wait_for_function(
                    "() => document.querySelector('#status').textContent.includes('could not save game files')",
                    timeout=60000,
                )
                assert stored_state(page) == before
                assert page.locator("#choose-folder").is_enabled()
                alter_saved_game(page, False)
                page.reload()
                wait_game(page)
                page.locator("#forget-game").click()
                page.wait_for_function(
                    "() => document.querySelector('#status').textContent.includes('Saved game files removed')",
                    timeout=30000,
                )
                assert stored_state(page) == {"count": 0, "sha256": None}
            finally:
                context.close()
            print("restart restore, failed replacement rollback and removal: PASS")

            context = open_browser()
            try:
                page = context.pages[0]
                page.goto(url)
                wait_picker(page)
                assert page.locator("#forget-game").is_disabled()
                page.locator("#remember-game").uncheck()
                page.locator("#game-folder").set_input_files(str(game_dir))
                wait_game(page)
                assert stored_state(page) == {"count": 0, "sha256": None}
                page.reload()
                wait_picker(page)
            finally:
                context.close()
            print("session-only import and reload: PASS")

            context = open_browser()
            try:
                page = context.pages[0]
                page.add_init_script("""Object.defineProperty(window, 'indexedDB', {
                  get: () => { throw new DOMException('Storage blocked', 'SecurityError'); }
                });""")
                page.goto(url)
                wait_picker(page)
                assert "Browser storage is unavailable" in page.locator("#status").text_content()
                assert not page.locator("#remember-game").is_checked()
                page.locator("#game-folder").set_input_files(str(game_dir))
                wait_game(page)
                page.wait_for_function(
                    "() => document.querySelector('#save-storage-status').textContent.includes("
                    "'could not be stored')",
                    timeout=30000,
                )
                assert "Storage blocked" in page.locator("#save-storage-status").text_content()
            finally:
                context.close()
            print("unavailable storage reports error and permits session-only play: PASS")

            fixture_executable = b"temporary test executable"
            with tempfile.TemporaryDirectory(prefix="gex-source-fixture-") as source:
                fixture = Path(source)
                (fixture / "GEX.exe").write_bytes(fixture_executable)
                (fixture / "asset.dat").write_bytes(b"temporary test asset")
                context = open_browser()
                try:
                    page = context.pages[0]
                    page.goto(url)
                    wait_picker(page)
                    page.locator("#game-folder").set_input_files(str(fixture))
                    page.wait_for_function(
                        "() => !document.querySelector('#forget-game').disabled",
                        timeout=30000,
                    )
                finally:
                    context.close()
            context = open_browser()
            try:
                page = context.pages[0]
                page.goto(url)
                assert stored_state(page) == {
                    "count": 2,
                    "sha256": hashlib.sha256(fixture_executable).hexdigest(),
                }
            finally:
                context.close()
            print("browser copy remains readable after source files are removed: PASS")
    finally:
        server.shutdown()
        server.server_close()
        thread.join()


if __name__ == "__main__":
    main()
