# Project structure and navigation

This is a map for coding agents, including small-context models. Start with
the task table, read the listed files, then follow their actual call sites.
This guide is an index, not a substitute for checking current code and build
configuration. Paths are relative to the repository root.

## What this project is

Gex Recompiled statically translates the supported original Windows x86 Gex
executable into C, builds the translated functions for a native host, and
provides a compatibility runtime and platform services. The original executable
and game assets are still required as data; this is not a full source
decompilation. The normal HLE launcher explicitly reports that original x86 is
not executed. A Dreamcast emulator such as Flycast runs the **compiled SH4
host**, not the original PC instructions.

There are two distinct codebases:

- This repository owns the game-specific integration, rewrites, backends,
  launchers, build configuration, and tests.
- `pcrecomp/` is a separate, ignored checkout of the upstream lifting tool and
  runtime. `PCRECOMP_DIR` can point elsewhere. It is not the Copilot skill in
  `.github/skills/pcrecomp/`.

## Repository map

| Path | Purpose | Editing guidance |
| --- | --- | --- |
| `AGENTS.md` | Repository-wide agent instructions. | Read first; protected-document rules apply. |
| `CMakeLists.txt` | Root targets, options, platform selection, and CTest registrations. | Check target/condition wiring when adding source or tests. |
| `src/` | Shared launcher, guest integration, menus, graphics, audio-facing logic, and interfaces. | Prefer these sources for game-specific behavior. |
| `src/platform/` | Platform contracts, SDL desktop implementations (`desktop/`), host-specific implementations (`linux/`, `windows/`, `dreamcast/`), and graphics backends (`bgfx/`, `gl/`). | Follow the existing source layout and platform manifest; several files retain target-specific branches for macOS, Android, and WebAssembly. |
| `src/recomp/gen/` | Generated lift, dispatch, and declarations. | Ignored output: **do not hand-edit**. Change generation/rewrites and regenerate when needed. |
| `cmake/` | Platform manifests, toolchains, runtime source groups, and renderer dependency wiring. | A manifest selects implementations; a toolchain selects compiler/target. |
| `config/` | Tracked lift metadata and game-asset hashes; locally generated analysis files are ignored. | Treat hashes and lift inputs as compatibility contracts, not defaults to silently replace. |
| `tools/` | Lift/rewrite scripts, asset tooling, build/package scripts, and test runners. | Read a script before running it: some build dependencies or create artifacts. |
| `tests/` | C test programs and Python unit/integration drivers. | CMake and platform conditions determine which tests exist. |
| `android/` | Manifest, Java activity/import code, and Android resources. | Native game code is still in `src/`; packaging is in `tools/`. |
| `assets/` | Checked-in project artwork, icons, and save-menu asset pack. | Distinguish project assets from the user's retail installation. |
| `patches/` | Checked-in patches for the separate pcrecomp checkout. | `tools/prepare_runtime.py` checks and applies these to that checkout. |
| `docs/` | Detailed development guide, Flycast test runbook, and this map. | Read the relevant section rather than the entire long development guide. |
| `.github/skills/` | Project-local Copilot skills: `testing` and `pcrecomp`. | Invoke the applicable skill before work; skills are instructions, not the toolchain itself. |

The following are normally local/ignored and might not exist in another
checkout: `Gex/` (user's authorized original game), `pcrecomp/` (toolchain
checkout), `.venv/` (Python environment), `.deps/` (dependencies and
toolchains), `build/` (builds and artifacts), `src/recomp/gen/`, and
`config/functions.json` / `config/pe_analysis.json` (generated analysis).
Check `.gitignore` and the actual tree rather than assuming these are tracked.
Never delete or modify the user's `Gex/` to make a test pass. Do not distribute
the original game, its assets, or mechanically recovered game code.

## Generation and build flow

For an example of changing and validating a rewrite, see
[generated-code changes](generated-code-changes.md).

1. The supported GOG `Gex/GEX.exe` and the pinned pcrecomp checkout are inputs
   to `tools/generate.py`. The script validates their identity; alternate
   paths are accepted through its CLI. See **Generate the C sources** in
   [development.md](development.md#generate-the-c-sources).
2. The generator invokes the pcrecomp lifter, checks the output, inserts
   scheduling/frame-wait changes, and applies game-specific rewrites through
   `tools/graphics_rewrite.py` and `tools/save_rewrite.py`. These hooks connect
   translated guest behavior to the project code.
3. Generated `src/recomp/gen/recomp_*.c` and dispatch files become the
   `gex_game` object library. CMake configuration requires generated sources
   already to exist; `gex_codegen` is a separate custom target, not a substitute
   for that initial prerequisite.
4. `CMakeLists.txt` includes the selected `cmake/platforms/*.cmake` manifest,
   which chooses the runtime, launcher, services, renderer and libraries. It
   links those with the generated game and shared logic into `gex_recomp`
   (a shared library on Android, an executable on other supported targets).
5. At runtime the host loads the supported original executable as data, binds
   its imports to compatibility shims, and dispatches native lifted functions.
   Platform services provide filesystem, input, video, audio and timing.

`config/lift_manifest.json` is tracked lift metadata;
`config/game-assets.sha256` is the authoritative checked-in asset manifest.
`tools/game_asset_manifest.py` produces tables used by desktop and Android
installation checks. `tools/prepare_runtime.py` applies checked-in pcrecomp
patches to the separate checkout; inspect its effects before running it.

## Runtime route: where a change belongs

For the HLE-based launcher, `src/main.c` parses options, selects and validates
the game directory and executable, initializes the compatibility host and
shims, loads options/saves, selects and initializes rendering/input, and calls
`recomp_host_run()`. Its monitor pumps platform, movie, renderer and input
events and provides smoke-test observation. It is shared by the SDL desktop,
MinGW HLE, Android and Dreamcast paths, with platform-specific setup.
Dreamcast starts at `src/platform/dreamcast/main.c`; Android enters through
`src/platform/android/entry.c` and Java activities. The experimental MSVC
native32 platform has a separate `src/platform/windows/main.c` and runtime
route; do not assume every HLE path applies to it.

| Concern | First files to inspect | Related boundary |
| --- | --- | --- |
| Guest startup, dispatch, callbacks | `src/main.c`, `src/guest_runtime.c`, `src/platform_shims.c` | `pcrecomp/runtime/win32hle/`; generated `recomp_dispatch.c`. |
| Original executable and assets | `src/identity.c`, `src/sha256.c`, `src/platform/desktop/game_install.c` | `config/game-assets.sha256`, `tools/game_asset_manifest.py`; Android `GameData.java`. |
| Save slots and title handoff | `src/save_menu.c`, `src/save_store.c` | `tools/save_rewrite.py`, `src/platform/filesystem.h`, `tests/password_saves.*`, `tests/save_handoff.*`; [in-game menus](in-game-menus.md). |
| Options and settings | `src/options_menu.c`, `src/preferences.c` | Host settings path in `src/platform/host.h`; `tests/options_screens.*`; [in-game menus](in-game-menus.md). |
| Input and controller mapping | `src/gamepad.c`, `src/platform/input.h`, relevant `src/platform/*/input.c` | `tests/gamepad.c`, `tests/input_mapping.c`. |
| Guest graphics and packet semantics | `src/graphics_guest.c`, `src/raster.c`, `src/renderer.c` | Hooks in `tools/graphics_rewrite.py`; backend contracts in `src/renderer.h` and `src/packet_gpu.h`. |
| Host presentation / GPU backend | `src/platform/desktop/renderer.c`, `src/platform/desktop/packet_gpu.c`, `src/platform/dreamcast/packet_gpu.c` | Backend selector `src/platform/packet_gpu_select.c`; shared GL and bgfx implementations under `src/platform/gl/` and `src/platform/bgfx/`. |
| Movie playback | `src/movie.c`, `src/movie_decoder.h`, `src/platform/desktop/movie_decoder.c` | Dreamcast movie I/O/pixels under `src/platform/dreamcast/`; `tests/movie*`. |
| Audio, host services, filesystem | `src/platform/audio.h`, `src/platform/audio.c`, `src/platform/filesystem.h`, `src/platform/host.h` | SDL host services in `src/platform/desktop/`, POSIX host operations in `src/platform/linux/`, and target implementations under `windows/` and `dreamcast/`; Web playback permission before guest execution in `src/platform/wasm/playback.c`; pcrecomp Win32 HLE semantics. |
| Dreamcast VMU LCD | `src/platform/dreamcast/services.c` | `src/platform/dreamcast/vmu_image.xbm` when present; [flycast-testing.md](flycast-testing.md). VMU save persistence is not implemented. |
| Game import or launcher UI | `src/platform/desktop/game_install.c` and desktop pickers in `src/platform/desktop/`; Android import in `android/src/org/gexrecomp/app/` | `tests/game_install.*`, `tests/folder_picker.*`, `tests/test_android_import.py`. |
| Gameplay smoke checks | `src/autoplay.c`, `src/gameplay.c`, `src/main.c` | `tests/autoplay.c`, `tests/gameplay.c`; testing skill. |

The guest compatibility API is not a direct dependency on a Windows host:
`cmake/runtime-core.cmake` explicitly selects pcrecomp `runtime/win32hle`
sources and `src/platform/runtime_core.c`. It defines machine, memory, system,
storage, display, audio and compatibility groups. Host implementations are
selected separately. Preserve guest address, threading, file, event and audio
semantics; do not replace unavailable operations with success-shaped stubs.

The CPU framebuffer/presentation interface (`src/renderer.h`) is separate
from packet rendering (`src/packet_gpu.h`). GPU-owned output may not expose CPU
pixels: a successful GPU presentation or a changing frame counter does not
prove image correctness or gameplay. Use appropriate external scanout evidence.

## Platform and build selection

| Target | Manifest and implementation | Entry/build helpers |
| --- | --- | --- |
| Linux desktop | `cmake/platforms/desktop.cmake`; `src/platform/desktop/`, `src/platform/linux/`, `src/platform/hle/` | Root CMake; `src/main.c`. SDL, FFmpeg and host dependencies are discovered by the manifest. |
| macOS | Desktop manifest with macOS branches in the desktop/Linux implementations and the macOS folder picker | `cmake/toolchains/macos*.cmake`, `tools/build-macos.sh`, `tools/package-macos.py`; bundle configuration lives in build tooling. |
| Windows 64-bit MinGW HLE | `cmake/platforms/mingw.cmake`; desktop SDL sources and `src/platform/windows/` | `cmake/toolchains/mingw64.cmake`, `tools/build-mingw.sh`. |
| Windows MSVC native32 | `cmake/platforms/windows.cmake`; separate native32 runtime | `src/platform/windows/main.c`; requires MSVC x86 / `-A Win32`. Not interchangeable with MinGW HLE. |
| Android | Desktop/HLE manifest with Android-specific branches and `src/platform/android/` entry/font helpers | `android/`, `tools/build-android.sh`, `tools/package-android.py`. |
| Dreamcast | `cmake/platforms/dreamcast.cmake`, KallistiOS services in `src/platform/dreamcast/` | `cmake/toolchains/dreamcast.cmake`, `tools/build-dreamcast.sh`, `tools/test-dreamcast.sh`. |
| WebAssembly (experimental) | `cmake/platforms/wasm.cmake`, Wasm font support under `src/platform/wasm/`, and Emscripten branches in desktop/Linux/HLE sources; browser shell/import in `web/` | `tools/build-wasm.sh`, `tools/serve-wasm.py`, [WebAssembly guide](webassembly.md). |

`GEX_PLATFORM_FILE` selects the manifest. `GEX_RENDERER_BACKEND_SOURCE`,
`GEX_MOVIE_DECODER_SOURCE`, and `GEX_ENABLE_*_RENDERER` affect selected
implementations. Read the manifest and CMake cache when diagnosing build
behavior; use a separate build directory for different platforms or materially
different configurations. `cmake/bgfx*.cmake` and `cmake/gldc-build.cmake`
cover renderer-specific dependencies. `tools/flycast-pvr.patch` patches an
external Flycast source checkout, not this project's runtime.

## Tests and verification

For a general, step-by-step investigation process, use
[debugging](debugging.md).
For missing prerequisites, stale caches, or missing tests, use
[build and test troubleshooting](build-test-troubleshooting.md).
For drawing and presentation problems, use
[renderer debugging](renderer-debugging.md).
For platform-specific build outputs, helper effects and test evidence, use
[platform builds and tests](platform-build-testing.md).
For adding platforms, extracting host backends, and maintaining ports, use
[porting and maintaining platform backends](response.md).

`tests/` contains C test executables plus Python drivers and unit tests;
some similarly named `.c` and `.py` files work together. `CMakeLists.txt` and
the selected manifest register CTest names, conditions, timeouts, and
environment. Tests may require the original installation, renderer
dependencies, or a specific target. Useful starting selectors include:

| Change | Relevant CTest names or files |
| --- | --- |
| Saves/options | `password-decks`, `save-handoff`, `options-screens`; `tests/test_save_rewrite.py`. |
| Rendering | `renderer-contract`, `guest-graphics`, `guest-packet-gpu`, `packet-gpu-differential`, `renderer-memory-backend`. |
| Input | `portable-input-mapping`, `gamepad-input`, `input-state`. |
| Movies/audio | `movie-backend-contract`, `movie-decoder`, `movie-lifecycle`, `audio-streams`, `wave-streaming`. |
| Generator/portable boundaries | `generator-and-patches` and relevant `tests/test_*.py`, including `test_generate.py` and `test_platform_boundary.py`. |
| Installation | `game-install`, `folder-picker`, `tests/test_android_import.py`. |

Check `ctest --test-dir <build-dir> -N` for tests in the **actual configured
build**, build first, and run the narrowest applicable selector. CTest names
are conditional, not a promise that every target supports every test.
The [testing skill](../.github/skills/testing/SKILL.md) contains exact current
host, headless, isolation, timeout, manual-play, and cleanup rules. Do not
cross-compile or launch emulators for routine validation without an explicit
target request. For explicit Flycast work use
[flycast-testing.md](flycast-testing.md), including the supplied binary and
serial log checks. For explicit Android testing and device-side effects, use
[android-testing.md](android-testing.md).

## Efficient task workflow

1. Read `AGENTS.md`, this map, and the applicable skill. Inspect `git status`;
   preserve existing work, including unrelated changes in files you touch.
2. Choose the concern row above and trace actual callers and interfaces. Read
   the applicable platform manifest before assuming a source is compiled.
3. If behavior comes from a generated hook, modify the checked-in generator or
   rewrite rather than `src/recomp/gen/`; verify the targeted transformation.
4. Change shared logic and every relevant backend/consumer only as needed.
   Keep resource ownership, error propagation, and guest contracts intact.
5. Build before running focused tests. Report platform or runtime validation
   that was not performed; do not infer a real-device or visual result from a
   successful compile.

For deeper subsystem details, use the relevant headings in
[development.md](development.md), especially **Platform boundaries**,
**Modular renderer**, **Other targets**, and **Dreamcast backend**.
