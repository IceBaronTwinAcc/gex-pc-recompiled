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
