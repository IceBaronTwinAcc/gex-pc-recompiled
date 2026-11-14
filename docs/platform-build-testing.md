# Platform builds and tests: agent guide

Use this page to choose a target and distinguish a successful build from a
successful runtime test. For file ownership see
[project-structure.md](project-structure.md); for errors see
[build-test-troubleshooting.md](build-test-troubleshooting.md).
The [testing skill](../.github/skills/testing/SKILL.md) governs test scope,
headless automation, manual tests, isolation, timeouts, and cleanup.

## Decide before building

Routine validation is for the current host only. Cross-compiling, running Wine,
an emulator or a remote/physical device, and rebuilding or packaging artifacts
for another platform require an explicit request for that target. A request to
document platforms is not a request to build or test them.

| Target | Build route | Runtime evidence and limits |
| --- | --- | --- |
| Linux | Native CMake / `tools/build-bgfx.sh` | Linux CTest and headless gameplay on Linux; not validation of Windows or macOS. |
| Windows x64 | MinGW HLE cross-build on Linux | Isolated Wine checks exercise the Windows binary, not native Windows presentation, drivers or hardware. |
| Windows x86 | Experimental MSVC native32, on Windows | Different launcher/runtime from MinGW; remains unvalidated in the development guide. |
| macOS arm64 / x86-64 | Separate OSXCross builds on Linux | Cross-compilation or Mach-O inspection alone is not a Mac runtime test. A Mac or Rosetta test needs explicit authorization and reports its actual CPU/GPU environment. |
| Dreamcast | KallistiOS / SH4 cross-build | Compile-only is not Flycast or hardware validation. Emulator checks have separate guest and emulator renderer settings. |
| Android | NDK build | See [android-testing.md](android-testing.md), including device-side effects and evidence. |

All examples below start in the repository root and assume prerequisites
are already available. `Gex/` is the user's original installation:
never modify, delete, or redistribute it. Generated lift sources must exist
before CMake configuration; see [generated-code changes](generated-code-changes.md).
Inspect `git status`, destination directories and CMake caches before building.
Do not silently replace existing release artifacts or another session's
outputs. Build helpers may download sources and modify `.deps/`, build
directories and the separate pcrecomp checkout; inspect the helper and run it
only when its side effects and missing prerequisites are appropriate.

## Linux: current-host baseline

For an existing, correctly configured build, build before selecting tests:

```sh
cmake --build build/linux/release --parallel 3
ctest --test-dir build/linux/release -N
ctest --test-dir build/linux/release --output-on-failure -R '^(renderer-contract|portable-input-mapping)$'
```

Change the selector to fit the task and verify `ctest -N` lists each expected
test. For changes spanning the runtime, run the current-host suite as directed
by the testing skill. Do not call a missing or skipped test a pass. GPU tests
may require a headless GL-capable driver rather than SDL dummy video; check
the test's configured environment. Tests involving original movies require
the relevant AVI data and may be conditional at configure time.

If configuring a new build, first inspect the
[Linux prerequisites and options](development.md#build-and-run-on-linux).
The default desktop configuration enables bgfx; `tools/build-bgfx.sh` prepares
its pinned dependencies and shader compiler. For an intentionally software-only
or non-bgfx build, configure with `-DGEX_ENABLE_BGFX_RENDERER=OFF`; do not
silently disable a requested renderer to make a test pass. Use a fresh,
non-existing build path when diagnosing a stale cache.

The [Linux testing skill](../.github/skills/testing-linux/SKILL.md) gives
bounded, isolated, headless gameplay and full-movie commands.
`first-graveyard=PASS` confirms the measured scene/movement checks, not
collision fidelity or a full playthrough. Movie checks require both completion
markers; normal exit is a separate check.

## Windows: MinGW x64 and isolated Wine

The [MinGW build instructions](development.md#mingw-w64-and-wine) describe
the toolchain and required dependencies. With an explicitly requested Windows
build, a prepared environment and an appropriate output directory:

```sh
bash tools/build-mingw.sh build/windows/mingw
```

The helper configures and builds that directory, can download/build
dependencies, and copies DLLs and licences into it. It produces
`gex_recomp.exe` plus runtime DLLs; keep the DLLs and licences with the
executable. An optional first argument chooses another build directory. Check
it is not occupied by user-owned output before invoking the helper.

If Wine testing is also requested, inspect the script first and use its
disposable prefix rather than the user's normal prefix:

```sh
bash tools/test-mingw.sh build/windows/mingw
```

The script configures silent dummy SDL and disabled Wine audio drivers inside
its own prefix, unsets visible display variables, runs **non-GPU** CTest
(`-LE gpu`), and runs bounded gameplay, movie and normal-exit checks. It
waits for its Wine server before removing the prefix; it leaves the prefix
if Wine is still running. Require successful commands and the appropriate
gameplay/movie/exit evidence. The excluded GPU tests are **not** passed.
Wine results do not prove native Windows display, audio, controller behavior
or a full playthrough. Never change the user's regular Wine prefix or host
sound settings to test.

The separate [MSVC native32 instructions](development.md#experimental-msvc-native32)
require an x86 MSVC developer environment and `-A Win32`. Its build and
limitations must be reported separately from the MinGW HLE build. Do not
execute its visible example as an automated test.

## macOS: separate architectures and runtime

The [macOS instructions](development.md#macos-arm64-and-intel) describe
OSXCross, the locally obtained SDK and compiler runtime, dependencies and
packaging. Those inputs may not be present on another machine and must not be
committed. With an explicitly requested cross-build and the prerequisites in
place, the build helper defaults to arm64; request Intel explicitly:

```sh
tools/build-macos.sh
GEX_MACOS_ARCH=x86_64 tools/build-macos.sh
```

Each invocation can download/build dependencies, writes its architecture's
`.deps/` and build output, and creates a thin `.app` and archive. Its first
argument can select another build directory; do not reuse an arm64 cache for
x86-64. These two commands do **not** create a Universal 2 app. A universal
deliverable needs a separately requested and verified merge of the executable
**and every bundled Mach-O library**; verify all required architectures in
each file. Do not overwrite an existing app or release when testing.

`tests/test_macos_bundle.py` is a host-side packaging unit test; passing it
checks the synthetic bundle/archive behavior, not Mach-O execution or macOS:

```sh
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests -p 'test_macos_bundle.py'
```

For an explicitly requested runtime test, use an authorized Mac with matching
architecture, dependencies and isolated settings. Follow the
[Mac fixture and gameplay procedures](development.md#macos-arm64-and-intel);
run automated applications headlessly and silently with a bounded runner.
Ad-hoc signing changes the chosen files, so only sign an authorized disposable
deployment, not the user's installed app. Preserve imported `Resources/Gex`.
On Apple Silicon, `arch -x86_64` tests via Rosetta, not an Intel CPU/GPU.
Require the selected renderer's own evidence and report actual hardware/API.
The normal macOS bgfx backend selects Metal; do not claim a Vulkan manual test
occurred if the required configuration is unavailable.

## Dreamcast: build versus Flycast

The [Dreamcast build instructions](development.md#build-and-smoke-check) and
[Flycast runbook](flycast-testing.md) cover KOS, the supplied compatible
emulator, manual/fullscreen and headless checks. For an explicitly requested
Dreamcast build, `tools/build-dreamcast.sh` requires a sourced KOS environment,
prepares the pinned MPEG decoder, and writes fixed paths in `build/dreamcast/`. It
produces an ELF and a `.bin`; it does **not** itself create a CDI. Inspect
those destinations first. Use the runbook's separate PVR scratch build when
that specific manual profile is required.

`tools/test-dreamcast.sh` creates a **test CDI** from the ELF and the user's
`Gex/` after converting the startup movies in temporary staging, and writes `gex-test.cfg` for certain modes plus `disc.log` and
`test.log` under `build/dreamcast/`. Its fixed paths may overwrite prior test
artifacts; its emulator settings are not automatically isolated. Read the
script and set up isolated emulator settings before use. Do not create a CDI
when the user requests only a binary, and never distribute a CDI containing
retail assets.

PVR GPU presentation and the expected MMU/import markers demonstrate startup,
not visual correctness or a full gameplay pass. A software autoplay check
uses `first-graveyard=PASS`; full movie testing additionally requires normal
guest exit and both completion markers. A compile, Flycast run and physical
Dreamcast run are three distinct levels of evidence. Follow the Flycast
runbook's test-profile and user-control requirements for manual play.

## Final report

Name the exact platform, architecture, build profile, runtime/device and
rendering API actually exercised. Separate build-only, fixture, headless,
manual visual/audio/controller, and physical-device results. Mention missing
or skipped checks and unresolved errors; historical results in
`development.md` do not establish that the current build was tested.
