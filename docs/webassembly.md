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
WebAssembly, `GexRecompLogo.png`, `gex-coi-serviceworker.js`, and
`licenses/FFmpeg.txt` files. The build downloads and verifies FFmpeg 8.0.1,
then builds only the AVI demuxer, Indeo 3, Cinepak, PCM U8, and pixel-scaling
components needed for the supplied movie formats. To serve it locally:

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
renderer selection when Chromium can obtain a WebGPU adapter. The test also
checks that the bundled project logo loads. The WebGPU check uses Chromium's
test-only SwiftShader flags. To verify both intro AVI movies finish decoding and
playback, run `python tests/wasm_movies.py --game-dir /path/to/Gex`. That
headless test hides physical gamepads from its browser so concurrent manual
play cannot cancel the movies or exit the guest.

For a branch-based GitHub Pages site, place the generated HTML at the branch
root as `index.html`, alongside `gex_recomp.js`, `gex_recomp.wasm`,
`GexRecompLogo.png`, `gex-coi-serviceworker.js`, `licenses/FFmpeg.txt`, and
`.nojekyll`; configure Pages to publish that branch's root. Permission to
distribute the translated game code in this project's public Pages build has
been confirmed. Never include the original game installation. After
rebuilding, refresh the generated files and `licenses/` directory together. To
check the packaged site on a headerless project subpath:

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

When the browser blocks audio autoplay, startup displays **Click the game
window for sound.** and waits for audio activation before starting the guest.
The first click or keypress resumes WebAudio through Emscripten's existing
input listeners. Movie timers therefore do not expire while waiting for
permission, and no audio packets or movie frames need to be discarded.
Browsers that already allow audio proceed without this wait.

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
  Browser fullscreen is requested directly by `src/platform/wasm/fullscreen.c`,
  without switching SDL's window/display mode or injecting fullscreen dimensions
  into the canvas style. Saved fullscreen is delayed until the initial backing
  size is established.
  The fullscreen canvas uses `object-fit: contain` within the viewport, so
  widescreen output remains fully visible without being stretched or clipped.
  Exiting the game also leaves browser fullscreen and cancels any deferred
  fullscreen request, without changing the saved display preference.
- With Python Playwright, a running Web server and a local game installation,
  run `python tests/wasm_renderer_selection.py --webgpu` to verify software
  selection both with WebGPU disabled and with Chromium's test adapter
  available. The adapter check uses test-only SwiftShader flags in a headless
  browser; it does not depend on the user's browser settings.
  Add `--firefox` (and install Playwright's Firefox browser if needed) to include
  a headless Firefox check for fullscreen canvas backing-size stability.
  `python tests/wasm_startup.py` checks all three saved display modes against a
  simulated 2560x1440 monitor, including image/backing agreement from the first
  write, a trusted gesture, and Chromium fullscreen entry/exit.
  It also suspends a real AudioContext during initialization and checks that a
  35-second wait starts no movies, followed by normal windowed activation.
  Repeat with `--browser firefox` for Playwright's bundled Firefox.
  `python tests/wasm_exit.py` selects Exit from the actual main menu while in
  fullscreen and verifies that the page returns to windowed mode.
- AVI playback supports the original Indeo 3 and Cinepak video with PCM U8
  audio. Movie files must be present in the selected installation. FFmpeg is
  built under LGPL v2.1-or-later and its license is included in
  `licenses/FFmpeg.txt`.
- No font files are bundled. The Wasm font lookup can use matching TTF files
  present in the selected `Gex/Fonts`, `Gex/fonts`, or `Gex` tree; missing
  fonts can leave guest-rendered text unavailable.
- This is a desktop-browser prototype, not an Android/browser-mobile package.
  Touch controls and optimized GPU rendering are not included.

## Extending the port

For a complete small-context debugging procedure, see
[Web startup freeze or incorrect presentation](renderer-debugging.md#web-startup-freeze-or-incorrect-presentation-step-by-step).

For startup stalls or clipped output, append `?trace=1` to the local game URL.
This opt-in trace writes JSON snapshots to the existing runtime log every two
seconds for one minute, without changing settings, requesting fullscreen, or
resizing the canvas. It records the browser viewport, canvas backing and CSS
rectangle, SDL's presented image dimensions, sampled pixel changes, canvas-write
gaps and costs, backing-size mutations, and the OpenAL WebAudio clock and queues.
The pixel-change count is a sampled diagnostic, not an exact movie-frame count.
Reload without the query to disable it.
An optional `&tracePort=PORT` sends the same snapshots and the last 40 non-trace
runtime-log lines to `/trace` on a local collector. This is allowed only from an
HTTP loopback page and only to the same loopback hostname; it does not send game
files, saves, or settings. It lets a developer collect evidence when page controls
are inaccessible. The collector must allow the game's origin through CORS.
`tools/collect-wasm-trace.py --origin http://127.0.0.1:8000 --output /path/to/new-trace.jsonl`
provides a bounded collector without requiring interaction with the game page.

Before the startup activation gate, stock Firefox 159.0.1 could start with its WebAudio context suspended when no user
gesture has enabled playback. In an isolated headless reproduction, the first
AVI stopped at 12 decoded frames with all 16,383 audio-ring frames still queued;
the guest cancelled it at about 32 seconds and started the second AVI. A trusted
canvas click resumed the audio clock and roughly 15 movie updates per second
without changing the 1100x618 backing. This establishes an audio-blocking failure
mode, not the cause of every visible presentation problem. Playwright's bundled
Firefox 157 did not reproduce that startup policy in the earlier movie checks.
The activation gate is implemented by `gex_host_prepare_playback()` before
`recomp_host_run()`, with browser behavior in `src/platform/wasm/playback.c`.
In stock Firefox, waiting 38 seconds before clicking now allowed both complete
intro movies to play without cancellation. The readiness message changes to
the existing running status when audio becomes available.

The affected visible run also submitted a 2560x1440 SDL image into a 1100x618
canvas. This was reproduced in stock Firefox with isolated saved fullscreen
preferences: `SDL_SetWindowFullscreen` selects the monitor's only SDL display
mode and resizes the software surface before the browser grants fullscreen.
The canvas remains windowed, so `putImageData` clips the oversized image.
The resize clamp cannot repair this because `SDL_SetWindowSize` changes the
saved windowed size while SDL considers the window fullscreen. Replacing the
SDL mode switch with browser-only fullscreen keeps image/backing at 1100x618
and restores approximately 15 movie writes per second after audio activation,
instead of approximately eight in the oversized-surface reproduction.
The affected user confirmed correct presentation and smooth playback.
Their visible trace kept image/backing at 1100x618 both windowed and in a
2560x1440 fullscreen rectangle, with an advancing audio clock and approximately
15 movie writes per second after activation.

`cmake/platforms/wasm.cmake` reuses the desktop SDL and Linux host sources,
with Emscripten-specific branches in the desktop services/screen,
Linux runtime/filesystem/virtual-memory/audio, and HLE flat-memory files.
Browser-specific font lookup is in `src/platform/wasm/`; movie playback uses
the shared player and FFmpeg decoder in `src/movie.c` and
`src/platform/desktop/movie_decoder.c`. The browser shell and import glue are
in `web/shell.html` and `web/gex-pre.js`.
`tools/serve-wasm.py` is a local development server, not a production hosting
configuration. The software renderer uses the existing desktop SDL presenter;
the guest-address memory layout is implemented in
`src/platform/hle/flat_memory.c`, and file reads use the Emscripten branch in
`src/platform/linux/virtual_memory.c`.

Treat streamed game-data access and font bundling as follow-up projects.
Browser saves and settings use the existing
filesystem interfaces backed by IndexedDB. Preserve the guest API contract and
platform interface ownership rules in
[porting and maintaining platform backends](response.md). A successful
Emscripten compile alone does not establish browser startup or gameplay.
