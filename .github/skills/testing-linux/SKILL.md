---
name: testing-linux
description: 'Linux: current-host baseline. Linux CTest and headless gameplay on Linux; not validation of Windows or macOS.'
---

# Linux: current-host baseline

Read the [testing skill](../testing/SKILL.md) and
[project structure](../../../docs/project-structure.md) first.

For build and test routes on Linux, see
[`docs/platform-build-testing.md`](../../../docs/platform-build-testing.md#linux-current-host-baseline).
The Linux commands below apply only when working on Linux.

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
