# WebAssembly build

The Emscripten target is an experimental browser port using the HLE runtime,
SDL input, OpenAL's browser audio backend and the software packet renderer.
GPU packet renderers, including bgfx/WebGPU, are disabled for this target.
Generated game code still comes from the checked-in pcrecomp workflow; the
original game installation remains user-supplied data.

## Build and serve

Install and activate the Emscripten SDK, with `emcc`, `emcmake`, CMake, Ninja,
Python 3 and Node.js available on `PATH`. Generate `src/recomp/gen/` first if
it is absent, following [development.md](development.md#generate-the-c-sources).
Then build:

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
the existing header-based server path, browser startup failures, and software
renderer selection when Chromium can obtain a WebGPU adapter. The WebGPU check
uses Chromium's test-only SwiftShader flags.

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
- The WebAssembly build always enables the software packet renderer and
  disables OpenGL, PVR, GLdc and bgfx, even when an existing CMake cache has
  different renderer settings. WebGPU availability does not affect backend
  selection. If browser settings contain a renderer that this build does not
  support, the options loader resets it to the available software renderer.
- SDL's browser canvas does not use a high-DPI backing buffer. The page follows
  the backing buffer's aspect ratio in windowed mode and scales the
  logical-resolution software output to the page and fullscreen viewport,
  avoiding an unnecessary per-frame CPU upscale for high-DPI screens.
  The Web build uses a stable 16:9 default instead of deriving the game
  resolution from an embedded browser viewport's arbitrary dimensions.
  Older saved resolutions narrower than 16:9 are migrated to that default.
  Fullscreen resizes keep the original backing size; CSS scales it to the
  viewport instead of asking the software renderer to redraw at screen size.
  SDL uses aspect-preserving fullscreen on Web, and delays a saved fullscreen
  mode until the initial backing size is established.
  The fullscreen canvas uses `object-fit: contain` within the viewport, so
  widescreen output remains fully visible without being stretched or clipped.
- With Python Playwright, a running Web server and a local game installation,
  run `python tests/wasm_renderer_selection.py --webgpu` to verify software
  selection both with WebGPU disabled and with Chromium's test adapter
  available. The adapter check uses test-only SwiftShader flags in a headless
  browser; it does not depend on the user's browser settings.
  Add `--firefox` (and install Playwright's Firefox browser if needed) to include
  a headless Firefox check for fullscreen canvas backing-size stability.
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

Treat streamed game-data access, movies and fonts as separate follow-up
projects. Browser saves and settings use the existing
filesystem interfaces backed by IndexedDB. Preserve the guest API contract and
platform interface ownership rules in
[porting and maintaining platform backends](response.md). A successful
Emscripten compile alone does not establish browser startup or gameplay.
