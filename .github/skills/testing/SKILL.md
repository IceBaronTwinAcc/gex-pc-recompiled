---
name: testing
description: Build, test, and debug this Gex recompilation with headless automated checks and visible fullscreen, gamepad-controlled manual tests with Vulkan and audio. Use for regressions, gameplay checks, movie playback, and rendering investigations.
---

# Gex testing

For build or test setup failures, see
[`docs/build-test-troubleshooting.md`](../../../docs/build-test-troubleshooting.md).
For renderer evidence and backend isolation, see
[`docs/renderer-debugging.md`](../../../docs/renderer-debugging.md).
For Android checks, device-side effects, and evidence, see
[`docs/android-testing.md`](../../../docs/android-testing.md).
For build and test routes on Linux, Windows, macOS, and Dreamcast, see
[`docs/platform-build-testing.md`](../../../docs/platform-build-testing.md).

## Flycast quick route

For any Dreamcast emulator test, read
[`docs/flycast-testing.md`](../../../docs/flycast-testing.md) before launching.
It explains how to locate and verify the supplied Flycast binary, and provides
an isolated manual-test recipe, required log markers, and failure handling.

Use the user-supplied emulator first, not `/usr/bin/flycast`, an AppImage, or a
new source build. The attachment path is local to this workspace; if missing,
ask for its new location. Do not silently substitute stock Flycast.
Always enable `config:Debug.SerialConsoleEnabled=yes` so guest failures are
visible. Full-MMU initialization alone does not prove the game started.

## Safety and scope

- Run from the project root and inspect the affected code before choosing checks.
- Build and test only for the current host platform unless the user explicitly
  requests another target. Do not cross-compile, run Wine/emulator/device tests,
  or rebuild/package other-platform artifacts as part of routine validation.
  The Linux commands below apply only when working on Linux.
- Launch automated game and test applications headlessly. Use `--headless` for
  `gex_recomp` and `SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy` for SDL tests.
  Do not open visible windows for automated checks, including through Wine.
  An explicit manual test means visible fullscreen with gamepad input, as
  described below; do not ask for headless-preference confirmation each time.
- Automated Wine tests must also be silent. `SDL_AUDIODRIVER=dummy` does not mute native
  DirectSound. Before starting Wine in a disposable prefix, disable
  `winepulse.drv,winealsa.drv,wineoss.drv` in `WINEDLLOVERRIDES` and set that
  prefix's `HKCU\Software\Wine\Drivers\Audio` value to an empty string. Keep
  SDL's dummy audio driver for the software fallback. Never mute the user's
  normal Wine prefix or change the host's sound settings.
- Preserve `Gex/` and its original executable and assets. Never clean that
  directory or modify it to make a test pass.
- Isolate automated settings with `XDG_DATA_HOME` in a temporary directory.
  Only reproduce against the user's settings when that is necessary; do not
  overwrite or delete those settings.
- Use bounded timeouts. Do not leave test processes running after their results
  have been collected. Stop only the specific process/session you started.

## Manual tests

- "Manual" means **visible fullscreen + gamepad input**. Open the game or
  emulator visibly in fullscreen and leave control to the user.
- Explicitly use the **Vulkan backend and audible game audio**. For Flycast,
  select `config:pvr.rend=4` (Vulkan), `config:aica.Volume=100`, and
  `audio:backend=auto`. Do not inherit `SDL_AUDIODRIVER=dummy`, muted volume,
  or disabled audio drivers from automated tests.
- Disable autoplay and emulator fast-forward. Do not set a dummy video driver
  or launch the visible application with a headless flag.
- Verify that the connected gamepad is detected and mapped to player one.
  Do not silently substitute keyboard-only input if the gamepad is unavailable.
- For a Dreamcast PVR test, launch the current build in Flycast with the guest
  arguments selecting `--renderer pvr`. Use a separate manual-test disc/config
  without autoplay arguments; preserve the normal production disc and settings.
  Flycast's Vulkan selection is separate from the guest's PVR renderer.
- Keep the application running across prompts for manual play, with a bounded
  timeout. Verify startup before handing it over, and stop only that test
  process when the user finishes or asks to stop.

## Choose meaningful checks

Build before running CTest, especially after temporary test binaries have been
cleaned. Start with the narrowest relevant selector:

```sh
cmake --build build/linux/debug --parallel 3
ctest --test-dir build/linux/debug --output-on-failure -R 'movie'
```

For changes spanning the runtime, run all tests for the current platform.
On Linux, GCC Release and Clang Release builds are available in
`build/linux/release` and `build/linux/clang`; build each before running its
tests. Linux Clang results do not validate macOS or Windows, and do not require
testing those platforms.

Headless gameplay check (create the settings directory before use):

```sh
XDG_DATA_HOME=/absolute/path/to/temporary-settings \
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy timeout 35s \
  build/linux/release/gex_recomp --headless --skip-movies --autoplay \
  --verify-gameplay --smoke-seconds 24
```

Require exit zero and `first-graveyard=PASS`. Changing frame counts alone do
not prove gameplay. The stricter check measures the recovered player position;
it still does not prove collisions, enemy behavior, or a full playthrough.

Complete intro playback:

```sh
XDG_DATA_HOME=/absolute/path/to/temporary-settings \
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy timeout 110s \
  build/linux/release/gex_recomp --headless --smoke-seconds 95
```

Require both movie completion messages. To exercise normal exit, use
`--headless --skip-movies --autoplay --smoke-seconds 40` with a 45-second
timeout and require `ExitProcess(0)` before the smoke deadline.

## Rendering investigations

- Capture screenshots only for a specific comparison, using `--screenshot`
  and a bounded smoke run. Use absolute output paths or root-relative paths.
- Inspect the captured pixels, not just checksums. Different frames can still
  be black, corrupted, or unrelated to the requested scene.
- Distinguish the guest framebuffer from the renderer's actual output.
  Dummy-driver framebuffer checks cannot prove that the desktop presenter
  displays it correctly. Exercise presentation headlessly if needed; do not
  substitute a visible window.
- For a transition bug, compare both sides under the same settings, including
  movie-to-menu handoff. Preserve only evidence still needed to investigate.

## Cleanup

- Reuse fixed paths for test logs, screenshots, binaries and build probes;
  overwrite prior outputs instead of accumulating timestamped or numbered
  copies. Use one disposable settings directory per run and remove it afterward.
- Use temporary directories with automatic cleanup for isolated build probes,
  generated fixtures, and settings. Do not create fake production backends.
- After inspecting a screenshot, delete it unless an active investigation or
  an explicitly requested deliverable still needs it. Delete redundant image
  conversions too.
- Remove temporary test executables and experimental binaries after their
  final use. Retain the current runnable `gex_recomp` build the user needs.
  Once validation is finished, also remove generated CMake/Ninja metadata,
  CTest results, object directories, and intermediate static libraries from
  the build directories. Preserve launchers and unrelated files. Reconfigure
  with the intended compiler and build type before the next build.
  Standard build outputs may be retained while rebuilding/debugging depends
  on them; do not confuse generated game sources with disposable binaries.
- Inspect and resolve each cleanup target first. Delete only known generated
  files; never recursively delete the project root, session root, original
  installation, or unrelated files. Do not delete another process's outputs.
- Keep concise failure logs while a problem remains unresolved. Report what
  was actually checked and any limitations; never mark an unresolved bug done.
