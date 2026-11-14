---
name: testing
description: Build, test, and debug this Gex recompilation with headless automated checks and visible fullscreen manual tests with Vulkan and audio. Use for regressions, gameplay checks, movie playback, and rendering investigations.
---

# Gex testing

For build or test setup failures, see
[`docs/build-test-troubleshooting.md`](../../../docs/build-test-troubleshooting.md).
For renderer evidence and backend isolation, see
[`docs/renderer-debugging.md`](../../../docs/renderer-debugging.md).

## Decide before building

Invoke the applicable skill before work; skills are instructions, not the
toolchain itself.

| Target | Build route |
| --- | --- |
| Linux | [testing-linux](../testing-linux/SKILL.md) |
| Windows | [testing-windows](../testing-windows/SKILL.md) |
| macOS | [testing-macos](../testing-macos/SKILL.md) |
| Dreamcast | [testing-dreamcast](../testing-dreamcast/SKILL.md) |
| Android | [testing-android](../testing-android/SKILL.md) |

## Safety and scope

- Run from the project root and inspect the affected code before choosing checks.
- Build and test only for the current host platform unless the user explicitly
  requests another target. Do not cross-compile, run Wine/emulator/device tests,
  or rebuild/package other-platform artifacts as part of routine validation.
- Launch automated game and test applications headlessly. Use `--headless` for
  `gex_recomp` and `SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy` for SDL tests.
  Do not open visible windows for automated checks, including through Wine.
  An explicit manual test means visible fullscreen, as described below; do not
  ask for headless-preference confirmation each time.
- Preserve `Gex/` and its original executable and assets. Never clean that
  directory or modify it to make a test pass.
- Isolate automated settings with `XDG_DATA_HOME` in a temporary directory.
  Only reproduce against the user's settings when that is necessary; do not
  overwrite or delete those settings.
- Use bounded timeouts. Do not leave test processes running after their results
  have been collected. Stop only the specific process/session you started.

## Manual tests

- "Manual" means **visible fullscreen**. Open the game or emulator visibly in
  fullscreen and leave control to the user.
- Explicitly use the **Vulkan backend and audible game audio**. Do not inherit
  `SDL_AUDIODRIVER=dummy`, muted volume, or disabled audio drivers from
  automated tests.
- Disable autoplay and emulator fast-forward. Do not set a dummy video driver
  or launch the visible application with a headless flag.
- Manual tests may use keyboard or gamepad.
- Keep the application running across prompts for manual play, with a bounded
  timeout. Verify startup before handing it over, and stop only that test
  process when the user finishes or asks to stop.

## Choose meaningful checks

Build before running CTest, especially after temporary test binaries have been
cleaned. Start with the narrowest relevant selector.
For changes spanning the runtime, run all tests for the current platform.

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
