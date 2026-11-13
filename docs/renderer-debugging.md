# Renderer debugging and evidence

For the general investigation process, start with
[step-by-step debugging](debugging.md). This guide adds rendering-specific
evidence and checks.

Use this guide for a visual regression. First read
[project-structure.md](project-structure.md), then the
[testing skill](../.github/skills/testing/SKILL.md). Start with a specific
scene, expected result and target backend; avoid treating a frame counter as
a picture. For Dreamcast emulator work, use [flycast-testing.md](flycast-testing.md)
and only run it when the user explicitly requested that target.

## Trace the layers

| Layer | Starting points | What evidence can establish |
| --- | --- | --- |
| Lifted guest and graphics hooks | `tools/graphics_rewrite.py`, `src/graphics_guest.c` | Hook placement, guest draw/transition behavior and guest-memory inputs. A rewrite match alone does not prove rendered pixels. |
| Raster/packet operations | `src/raster.c`, `src/packet_gpu.h`, selected `src/platform/*/packet_gpu.c` | Packet interpretation, texture/palette and backend output. Compare the same inputs and selected backend; a unit fixture may not cover a game scene. |
| Framebuffer interface | `src/renderer.c`, `src/renderer.h`, `src/renderer_headless.c` | CPU-readable pixel content when a framebuffer is actually available. A dummy/headless framebuffer is not the desktop window or emulator scanout. |
| Host presentation | `src/platform/desktop/renderer.c`, `src/platform/desktop/present.c`, or selected platform presenter | Presentation and event-thread behavior. Successful presentation or a nonzero frame count does not prove pixels are correct. |
| GPU-owned output / emulator scanout | `src/platform/dreamcast/packet_gpu.c`, `src/platform/bgfx/`, Flycast | Final visual output must be inspected at the presentation/scanout surface. GPU-owned frames may have no CPU pixels, and CPU screenshot or pixel-based gameplay verification is rejected in that mode. |

`--renderer pvr` selects the guest packet backend; Flycast
`config:pvr.rend=4` selects the emulator's Vulkan host renderer. They are
independent. Select supported backends for the configured platform; do not
silently switch backends and report that the original one works.

## Narrow a regression

1. Record platform, actual build/configuration, backend, game data, scene,
   viewport and settings. Reproduce both sides with the same conditions,
   including movie-to-menu handoff for a transition bug.
2. Check the relevant CTest registrations and run focused tests after building:
   `renderer-contract`, `renderer-memory-backend`, `guest-graphics`,
   `guest-packet-gpu`, `packet-gpu-differential`, or `presentation-thread`,
   as appropriate and available. See
   [build troubleshooting](build-test-troubleshooting.md). A skipped GPU
   test is not a pass; GPU tests may need a headless GL-capable driver rather
   than SDL's dummy video driver.
3. For CPU framebuffer investigations, use the testing skill's isolated,
   bounded headless run and `--screenshot` only for a specific comparison.
   Inspect the image pixels and actual scene on both sides, not just file
   existence, checksum or frame count. A black or unrelated frame can still
   have a different checksum.
4. For GPU-owned rendering, inspect an appropriate external output/scanout
   capture under equivalent conditions. Do not use the unsupported guest CPU
   screenshot or pixel-based `--verify-gameplay` as a proxy. Automated tests
   remain headless and silent; a visible fullscreen test requires an explicit
   manual request and follows the testing skill.
5. Report separately what was observed: hook/unit behavior, guest pixels,
   host presentation or emulator scanout. A screenshot does not establish
   collision or complete gameplay, and one scene does not cover transitions
   or all backends. Preserve evidence only while it is needed.

The long-form implementation details and historical measurements are in
[development.md](development.md#modular-renderer); verify the current source
and configuration before treating historical results as current behavior.

## Dreamcast title snapshot lifetime

The title renderer indexes live snapshots by guest image address, but still
checks the source bytes and palette before reuse. The PVR backend indexes
retained title textures by immutable image ID. Retained-cache hits do not need
a transient cleanup sweep; command retirement and allocation paths still
perform cleanup. Keep the existing arena retirement and VRAM cache limits when
changing either index.

Title packet retirement batches contiguous runs before installing new markers.
Runs split at arena wrap and before a wrapped range reaches the old cursor, so
the allocation callback can recognize the wrap. Pending draws hold their
snapshots throughout, and markers outside overwritten ranges remain live.

Compact atlas snapshots are indexed by page, palette, transparency and
region, and removed from the index when their texture is freed. Lookup still
checks the atlas, write epoch and zero mask; overlapping atlas writes invalidate
the old snapshot without changing queued commands. Opaque direct-color draws
reuse a matching snapshot before attempting to allocate a full page. Immutable
transient textures retain their PVR headers separately for each alpha pass;
per-draw tint remains in the vertices.

For cadence profiling, buffer frame and vblank measurements before printing
them: serial output every 30 frames can itself cause a missed vblank. Flycast
timings do not establish physical Dreamcast performance.

For a warmed idle scene-49 run with the experimental map enabled, a 180-frame
Flycast control averaged 37.1 ms wall and 21.3 ms game-thread CPU per frame.
It had 142 two-vblank intervals and 38 of three or more vblanks. A separate
lightly instrumented run measured about 11.0 ms CPU in guest packet-list
processing, 4.5 ms recording CELs, 2.4 ms generating chunks, 0.2 ms generating
flipped CELs, and 0.9 ms in presentation; roughly 2.6 ms remained unclassified.
The generator callbacks did not overlap packet-list processing in a subsequent
probe. Callback retirement time is included in the generator measurements.

The packet list processed about 150 packets per frame: 78 sprite packets,
40 deferred CEL markers, 10 textured quads, 21 page-state packets, and one
other packet. Sampling one in eight packets attributed roughly 5 ms CPU to
sprite dispatch and 3 ms to deferred CEL dispatch; these are included in the
packet-list total, not additional costs. The separately measured presentation
wall time included approximately 8 ms in the KOS render-completion wait and
7 ms waiting for the display flip. These waits include scheduling and emulator
effects and are not measurements of GPU execution alone. Animation counter
and camera position were matched across comparisons, but complete game state
was not; instrumentation and differing workloads make the figures approximate.

## Experimental Dreamcast level-selection map

`GEX_DREAMCAST_NATIVE_MAP` is off by default. When enabled, the Dreamcast
backend can replace the scene-49 repeating backdrop, supported unflipped CELs,
direct-color map chunks including flipped tiles, and supported flipped, unflipped, and scaled
CEL packet generation. Those CEL packet paths still use the original atlas and
palette preparation. Compressed map CEL capture is limited to sequential tile data.
`map_plan.c` preserves the original chunk and layer order; `map_draw.c` reserves
packet space before writing and falls back without writes for unsupported tiles
or unsafe arena reuse. Other map paths can still use legacy generation, so this
is not a complete legacy-free map implementation. A no-fallback diagnostic must
check each producer independently and exercise selection, fades, and
transitions.

In one headless scene-49 autoplay run, blocking those legacy loops still
allowed a down-selection and transition to scene 3. A separate run exercised
up, left, right and down inputs without reaching the blocked loops. These runs
did not exercise the standalone backdrop or unflipped fallback producers, and
do not establish full route coverage. A scanout comparison at the same
animation counter and camera position found small color differences between
native and legacy output; those fields alone do not prove identical game state.
An isolated Flycast PVR comparison also found RGB565 differences between
direct ARGB1555 CEL textures and indexed palette textures for neutral green
values 16 and 17. This can contribute to scanout differences independently of
animation state; do not treat a matching animation counter as pixel proof.

Chunk preflight tracks the at-most-two contiguous arena runs rather than
scanning every earlier packet. After preflight succeeds, contiguous chunk
packet retirements are batched, splitting at wrap and before a wrapped range
reaches the previous cursor. Preflight rejects a wrapping allocation whose end
would prevent the retirement callback from recognizing the wrap. Map CEL plans
use a small local buffer and retry with the full-sized allocation when necessary.

The map chunk path currently accepts direct-color tiles only. An inventory of
the loaded scene-49 map found two nonempty chunks with 72 drawable tile layers,
all direct-color; this does not establish support for other maps. Unflipped CEL
packet preflight also checks that wrapped arena allocations do not overlap
previously planned packets before making any writes.

The destination level ID can change before the outgoing map finishes drawing.
The guest bridge retains the displayed scene's ID while its scene and map
pointers still match; replacing either pointer ends that association.

## Web startup freeze or incorrect presentation: step-by-step

Use this procedure when a browser starts with a frozen, badly sized, or clipped
picture, especially if fullscreen changes the symptoms. Do not change CSS or
aspect ratios before locating the failing layer. More than one failure can be
present at once.

1. **Preserve the reproduction.** Read `AGENTS.md`, the testing skill, and
   [WebAssembly guide](webassembly.md). Record the affected browser/version,
   local URL, windowed/fullscreen state, and whether audio stops with video.
   Keep the user's installation, browser storage, existing server, and saved
   settings unchanged. Use session artifacts for diagnostic logs and screenshots.
2. **Confirm the actual build.** Run `git status --short`, check
   `build/wasm/CMakeCache.txt`, and confirm the local server responds with
   `curl -fsS -o /dev/null http://127.0.0.1:8000/gex_recomp.html`. Substitute the
   actual URL if different. Read `cmake/platforms/wasm.cmake` before assuming
   which renderer or decoder runs. With an existing configured build,
   `cmake --build build/wasm --parallel 3` refreshes changed shell/pre-JS files.
   For a missing configuration use `tools/build-wasm.sh build/wasm`.
3. **Separate the test browser from the user's browser.** Playwright bundles
   its own Firefox; its version can differ from `firefox --version`. Record both.
   Headless tests are controls, not proof that a visible-browser failure is absent.
   Do not launch a visible browser yourself unless explicitly authorized.
4. **Collect from the affected browser without changing its origin.** Append
   `?trace=1` to the existing game URL (or `&trace=1` if it already has a query).
   The trace logs JSON every two seconds for one minute. Keep the same hostname
   and port: changing either selects different browser-local game data/settings.
   Normal URLs do not enable the trace. Ask the user to reproduce the failure;
   do not ask them to calculate viewport dimensions.
5. **If the page controls cannot be used, collect automatically.** For a local
   HTTP page, start this bounded helper with a new evidence-file path:

   ```sh
   python tools/collect-wasm-trace.py \
     --origin http://127.0.0.1:8000 \
     --output /absolute/session-artifacts/visible-firefox-trace.jsonl
   ```

   It prints `tracePort=PORT` and a `/ready` URL. Verify that URL with `curl`.
   Ask the user to load the **original** game URL with
   `?trace=1&tracePort=PORT`, replacing `PORT` with the printed number.
   They do not need to click the page or expand its log. Read the JSONL file
   after samples arrive. The collector accepts only the specified loopback
   origin and sends no game files, saves, or settings. Do not replace or stop
   the existing game server. The helper exits after four minutes; stop only
   your own process if evidence is collected sooner.
6. **Locate the first stalled layer using consecutive snapshots.** Compare:

   | Fields | Meaning and next action |
   | --- | --- |
   | `audio.state`, `audio.time`, source queues | A suspended context with an unchanged clock can stop audio consumption. Trace `src/platform/audio.c`, `src/platform/linux/audio.c`, and `web/gex-audio.js`. `running` alone is insufficient; check that the clock advances. |
   | `writes` versus `changes` | SDL can repaint an unchanged image. Repeated writes with zero sampled changes are not advancing movie frames. Inspect `src/movie.c` and the decoder before blaming the compositor. Sampling can miss small changes; confirm with pixels. |
   | `intervalMs`, `maxGapMs`, `sinceWriteMs` | A delayed two-second sample suggests page-thread scheduling trouble. Large write gaps with a normally advancing page timer suggest upstream work or handoff stalls. These measurements do not time the entire worker-side renderer. |
   | `maxWriteMs` | Time spent inside the page's `putImageData`, not decode, scaling, the synchronous worker proxy, or compositor display time. A low value does not prove the whole renderer is fast. |
   | `image`, `backing`, `rect`, `viewport`, `resizes` | Compare the SDL image with canvas backing dimensions, then the browser rectangle with the viewport. A matching image/backing does not prove visible compositor output. A rectangle extending below the viewport is page overflow, not proof of a decode or pacing defect. |
   | `runtime` in collected samples | Contains recent ordinary runtime log lines. Check decoded/completed/cancelled movie counts rather than assuming that a transition means successful playback. |

7. **Trace movie/audio coupling explicitly.** In `gex_movie_pump()`, a partial
   audio write returns before the next video event. A full audio ring can therefore
   freeze both outputs while the guest continues waiting. `stream_poll()` consumes
   from the native audio cursor, not the browser page's animation clock. Follow
   the actual event and queue state; do not remove backpressure or discard audio
   to make a test finish.
8. **Check whether a click also changes fullscreen.** Emscripten's SDL backend
   can defer a requested fullscreen transition until a user gesture. A click
   anywhere may execute that request as well as resume WebAudio. Compare
   `fullscreen` and `resizes` before and after the click; do not call it an
   audio-only experiment if either changes. Follow `gex_video_display()`,
   `g_web_fullscreen_pending`, `capture_web_output()`, and the resize handling in
   `src/platform/desktop/screen.c`. Inspect the **installed SDK's** SDL backend
   if necessary; it is an external dependency, not a file to patch speculatively.
9. **Verify pixels at the right layer.** Compare the decoded source frame,
   shared renderer frame, SDL image, and visible browser screenshot under matching
   conditions. The browser main-thread RAF rate and movie-completion messages
   cannot establish visible correctness. `page.add_init_script` must execute its
   code: an uninvoked arrow-function expression does not install a probe.
10. **Fix only the established failure, then repeat the same reproduction.**
    Keep experimental changes isolated; do not revert unrelated logo, movie, or
    committed fullscreen/aspect behavior. If audio resumes but presentation
    remains wrong, retain both findings and continue tracing presentation.
    Remove temporary probes/captures no longer needed, leave the user's current
    runnable build and existing server intact, and report unresolved layers
    plainly instead of declaring the entire issue fixed.

### Established control reproduction

In stock Firefox 159.0.1 with no playback-enabling gesture, the first intro movie
can stop after 12 decoded frames: WebAudio remains suspended at time zero and
16,383 audio-ring frames remain queued. The guest cancels that movie at about
32 seconds and starts the second. A trusted canvas click resumed audio and about
15 movie updates per second in an isolated **windowed headless** run without
changing the 1100x618 backing. This establishes one audio-blocking failure mode.
The user's visible run still showed lag and incorrect presentation after audio
resumed, and their click also entered fullscreen. That presentation failure
was subsequently captured as a 2560x1440 SDL image written into a 1100x618
canvas. The same mismatch was reproduced with isolated saved fullscreen
preferences in stock Firefox. SDL's fullscreen mode selection resized its
software surface to the monitor mode before browser fullscreen was granted;
the existing resize clamp only changed SDL's saved windowed size.

Browser-only fullscreen in `src/platform/wasm/fullscreen.c` now avoids that
SDL mode switch and leaves layout to the existing page CSS. Under the same
stock-Firefox reproduction, image/backing remain 1100x618 and movie writes
return to approximately 15 per second after audio activation, compared with
approximately eight before the fix. `tests/wasm_startup.py` checks saved display
modes and image/backing agreement. The affected user subsequently confirmed
correct presentation and smooth playback. Their visible trace retained a
1100x618 image and backing both windowed and in 2560x1440 fullscreen, with
approximately 15 movie writes per second after audio activation.

The separate audio stall is now handled before guest execution:
`gex_host_prepare_playback()` opens audio and the Web implementation waits for
the context to run while continuing to pump display/input events. The page
shows the user-supplied **Click the game window for sound.** message while
waiting. Guest movie timers have not started yet; the player retains normal
backpressure and does not discard data. A stock-Firefox run waited 38 seconds,
then played both complete intro movies without cancellation. The persistent
startup test suspends a real AudioContext and waits 35 seconds before activation.
Do not confuse that controlled suspension test with reproducing a particular
browser's autoplay policy. The affected visible-browser run confirmed that
audio activated normally and the original startup presentation/lag symptoms
were resolved.
