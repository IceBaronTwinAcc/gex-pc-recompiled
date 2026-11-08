# WebAssembly build

The Emscripten target is an experimental browser port using the HLE runtime,
SDL input and OpenAL's browser audio backend. It supports the software packet
renderer and an optional bgfx/WebGPU packet renderer.
Generated game code still comes from the checked-in pcrecomp workflow; the
original game installation remains user-supplied data.

## Build and serve

Install and activate the Emscripten SDK, with `emcc`, `emcmake`, CMake, Ninja,
Python 3 and Node.js available on `PATH`. Generate `src/recomp/gen/` first if
it is absent, following [development.md](development.md#generate-the-c-sources).
Prepare bgfx and its host shader compiler with `tools/build-bgfx.sh`, then build:

```sh
tools/build-wasm.sh
```

The output is `build/wasm/web/gex_recomp.html` with its generated JavaScript,
WebAssembly and `gex-coi-serviceworker.js` files. To serve it locally:

```sh
python3 tools/serve-wasm.py
```

Open the printed `http://127.0.0.1:8000/gex_recomp.html` URL. Do not open the
HTML file directly from disk. Pthreads require a cross-origin-isolated page and
SharedArrayBuffer support. The helper supplies the required COOP and COEP
headers. On static hosting such as GitHub Pages that cannot set those headers,
the same-origin service worker adds them to responses and reloads the first
visit before starting the runtime. Deploy the worker beside the HTML, JavaScript
and Wasm files, and use relative paths so it controls the project's subdirectory.
This requires HTTPS (or localhost), enabled service workers and a browser that
supports cross-origin isolation. A hosting configuration with the appropriate
headers works without installing the worker. Cross-origin resources must comply
with the embedder policy.

With Python Playwright and its Chromium browser installed, run
`python tests/wasm_coi.py --webgpu` to check a headerless project subpath,
the existing header-based server path and browser startup failures. The
optional WebGPU check uses Chromium's test-only SwiftShader flags.

For a branch-based GitHub Pages site, place the generated HTML at the branch
root as `index.html`, alongside `gex_recomp.js`, `gex_recomp.wasm`,
`gex-coi-serviceworker.js`, and `.nojekyll`; configure Pages to publish that
branch's root. The Wasm binary contains translated game code, so confirm that
you have permission to distribute it before publishing. Never include the
original game installation. After rebuilding, refresh all four generated
files together. To check the packaged site on a headerless project subpath:

```sh
python tests/wasm_coi.py --web-dir . --html index.html --webgpu
```

The page asks for one game directory using the browser's directory picker.
Files are read in the page and copied to the Wasm virtual filesystem at `/Gex`;
the selection must contain `GEX.exe` at its root and be no larger than 1 GiB.
The files are not uploaded or embedded in the build output. By default, the
page also saves a copy in IndexedDB on this device and restores it on later
visits to the same site and directory. Select the folder containing the game
files, not a parent containing unrelated directories. Uncheck **Remember game
files on this device** for a session-only import. **Remove saved game files**
removes the browser copy without changing the original installation; reload
to select a different folder.

The browser copy uses disk space, is subject to storage quotas and may be
removed when browser data is cleared or storage is evicted. If saving fails,
the page reports the error and offers a session-only import. The runtime still
loads the files into WebAssembly memory on every visit; browser/device memory
limits can prevent startup.
With Playwright installed, `python tests/wasm_storage.py` exercises import,
browser restart, removal, session-only use, storage failures, and restoring
settings and password decks before game startup. For a branch-root Pages site,
use `python tests/wasm_storage.py --web-dir . --html index.html`.

Password decks, options and settings are enabled in the browser build. They are
stored in a separate IndexedDB filesystem on this device, scoped to the site
path. Browser storage can be cleared or evicted, and private browsing may block
it; the page reports storage failures. Original game files, saves and settings
remain local and are never uploaded.

The browser OpenAL mix uses fixed headroom (about 8.5 dB quieter) so effects
can overlap music without routinely ducking it. A fast peak limiter and guard
protect unusually loud mixes. Native OpenAL builds retain their existing output
limiter. With Playwright installed, `python tests/wasm_audio.py` measures the
browser mix without playing audio.

## Initial port scope and limitations

- Memory is WebAssembly linear memory. The linker global base is set to
  `0x01000000`; the HLE flat-memory allocator reserves guest image ranges
  below that boundary and allocates other guest addresses from the Wasm heap.
  The build starts with 512 MiB and may grow to 2 GiB. Large data folders or
  browser/device memory limits may prevent startup.
- The game runs on an Emscripten worker. Shared-memory workers require
  cross-origin isolation, provided by server headers or the service worker.
- The standard build includes both packet renderers. It checks for a usable
  WebGPU adapter and device before selecting bgfx, or selects the software
  renderer when that check fails. The choice is logged. bgfx requires compute,
  which WebGL 2 does not provide. Its dedicated worker owns the transferred
  OffscreenCanvas and WebGPU resources; the software path uses SDL instead.
  Builds can still exclude either renderer explicitly, for example:

  ```sh
  tools/build-wasm.sh build/wasm-bgfx \
    -DGEX_ENABLE_BGFX_RENDERER=ON \
    -DGEX_ENABLE_SOFTWARE_RENDERER=OFF
  ```

  The bgfx-enabled build uses Asyncify for WebGPU adapter/device
  initialization and worker event-loop yields. CMake applies the checked-in
  WebGPU fixes to the pinned bgfx source, and rejects incompatible sources.
  WebGPU support depends on the browser and GPU. A bgfx-only build cannot fall
  back to software. If bgfx initialization fails after the availability check,
  the error is reported rather than silently switching renderers.
  On Linux, Firefox may leave WebGPU disabled by default. In Firefox 159, setting
  `dom.webgpu.enabled` to `true` in `about:config` enabled WebGPU in an isolated
  test profile; availability still depends on the adapter and driver. This is
  an advanced browser preference and may expose experimental functionality.
- With Python Playwright and its Chromium browser installed, a running Web
  server and a local game installation can be checked with
  `python tests/wasm_renderer_selection.py --webgpu`. The optional `--webgpu`
  check uses Chromium's unsafe WebGPU and SwiftShader flags in an isolated
  headless test, not the user's browser settings.
- AVI playback is not implemented. Movie open requests still require the
  selected installation's movie files; playback is skipped by default.
- No font files are bundled. The Wasm font lookup can use matching TTF files
  present in the selected `Gex/Fonts`, `Gex/fonts`, or `Gex` tree; missing
  fonts can leave guest-rendered text unavailable.
- This is a desktop-browser prototype, not an Android/browser-mobile package.
  Touch controls and optimized GPU rendering are not included.

## Extending the port

`cmake/platforms/wasm.cmake` reuses the desktop SDL and Linux host sources,
with Emscripten-specific branches in the desktop services/screen,
Linux runtime/filesystem/virtual-memory/audio, and HLE flat-memory files.
Browser-specific font lookup and the intentionally unsupported movie decoder
are in `src/platform/wasm/`; browser shell and import glue are in
`web/shell.html` and `web/gex-pre.js`.
`tools/serve-wasm.py` is a local development server, not a production hosting
configuration. The software renderer uses the existing desktop SDL presenter;
the guest-address memory layout is implemented in
`src/platform/hle/flat_memory.c`, and file reads use the Emscripten branch in
`src/platform/linux/virtual_memory.c`.

Treat streamed game-data access, movies, fonts and hardware rendering as
separate follow-up projects. Browser saves and settings use the existing
filesystem interfaces backed by IndexedDB. Preserve the guest API contract and
platform interface ownership rules in
[porting and maintaining platform backends](response.md). A successful
Emscripten compile alone does not establish browser startup or gameplay.
