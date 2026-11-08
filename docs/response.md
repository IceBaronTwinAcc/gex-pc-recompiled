# Porting and maintaining platform backends

This guide is for adding a host platform, moving an existing host-dependent
operation behind a platform backend, or maintaining a port when shared code
changes. Start with [project structure](project-structure.md); use
[platform builds and tests](platform-build-testing.md) for target-specific
builds and evidence, and [development](development.md#platform-boundaries)
for the current interface inventory. The source and selected CMake manifest
remain authoritative.

## What is being ported

The supported original Windows x86 executable is translated into native C;
it and the game assets are still required as data. The game is not a full
source decompilation, and the normal HLE host does not execute original x86
instructions. A host platform does **not** need Windows merely because the
guest uses Windows API names.

Keep these layers separate:

1. **Guest behavior and ABI.** Lifted code uses guest addresses, registers,
   flags, calling conventions and imported Win32 behavior. The compatibility
   layer implements the observable guest contract, not necessarily every
   feature of a real Windows API.
2. **Portable game integration.** `src/` implements game-specific logic,
   menus, drawing, dispatch integration and portable interfaces. The HLE
   compatibility sources selected by `cmake/runtime-core.cmake` implement
   guest semantics. Keep platform SDK dependencies out of this shared code.
3. **Host mechanisms.** Sources selected under `src/platform/` implement
   clocks, threads, files, audio, input and rendering using the host's APIs.
   The manifest chooses these implementations; the toolchain chooses the
   compiler and target.
4. **Build and delivery.** CMake selects sources, dependencies and features;
   launchers and packaging provide the correct assets, writable storage and
   platform lifecycle. A successful link does not establish runtime support.

The HLE path is shared by Linux, macOS, MinGW x64, Android and Dreamcast.
The experimental MSVC x86 `native32` path has a separate launcher and uses
real Windows imports. Do not assume its entry point or services are the HLE
ones. A new platform that cannot preserve a guest contract should explicitly
report the unsupported operation or disable the affected feature; it should
not pretend the operation succeeded.

## Start a new port

### 1. Establish the target and constraints

Record the intended CPU/ABI, pointer width, endianness and alignment,
compiler/C11 support, RAM and address-space budget, graphics and audio APIs,
threading and callback restrictions, storage model, and lifecycle rules.
Identify whether the target can run detached workers and let a blocking guest
thread yield while another runs. A single-thread-only environment is not
automatically compatible with the current guest scheduling model.

Determine the required feature set and explicitly list deferred features.
Decide how users provide the supported executable and assets, how writable
settings are persisted, and what the platform permits distributing. Never
bundle or commit the user's retail executable, assets or generated game code.
Consult `config/game-assets.sha256`, `src/identity.c`, and the existing import
and installation checks rather than inventing a different asset contract.

### 2. Wire a toolchain and manifest

Use an existing manifest as a working reference, not a template to copy
blindly: `cmake/platforms/desktop.cmake` shares SDL/POSIX services;
`cmake/platforms/mingw.cmake` combines the desktop UI with Windows services;
`cmake/platforms/dreamcast.cmake` uses KallistiOS and a separate entry point.
Android currently uses the desktop/HLE selection with Android-specific wiring
in root CMake and `src/platform/android/entry.c`.

For an HLE host, the root `CMakeLists.txt` requires:

| Manifest setting | Purpose |
| --- | --- |
| `GEX_PLATFORM_RUNTIME` | Set to `hle` for the shared guest compatibility runtime. |
| `GEX_HOST_SOURCES` | Host services, input and guest-allocation adapter used by `gex_host`. |
| `GEX_LAUNCHER_SOURCES` | Launcher and integration sources, including a target entry point if required. |
| `GEX_DEFAULT_RENDERER` | Source defining `gex_renderer_backend_get()` for framebuffer presentation. |
| `GEX_DEFAULT_MOVIE_DECODER` | Source implementing `src/movie_decoder.h`. |
| `GEX_RUNTIME_PLATFORM_SOURCES` | Host implementations of runtime, storage, audio, font and HLE screen/presentation contracts as needed. |
| `GEX_RUNTIME_{MACHINE,MEMORY,SYSTEM,STORAGE,DISPLAY,AUDIO,COMPAT}_SOURCES` | Nonempty guest runtime source groups; include `cmake/runtime-core.cmake` to reuse the existing groups. |

Dependency discovery belongs in the manifest. Set `GEX_PLATFORM_LIBRARIES`
and, where needed, `GEX_HOST_LIBRARIES`; implement
`gex_configure_movie_decoder(target, source)` when the selected decoder
needs extra libraries or sources. A non-FFmpeg decoder avoids the desktop
decoder's FFmpeg linkage, but does not automatically remove other dependencies.
Keep target-specific compile and link options scoped where possible.

`GEX_PLATFORM_DESKTOP_TESTS` is not a generic “enable tests” switch: root
CMake also uses it for desktop icon/setup wiring, renderer defaults and
password-save feature definitions. Enable it only when the target provides
the corresponding desktop contracts; review all its uses before changing it.
Likewise, enabling an existing GPU option is not a generic way to register a
new renderer. Check the source selection, prerequisites and guard conditions
in root CMake and update the selector and build wiring as appropriate.

Follow the repository's existing platform layout rather than splitting code
into new cross-platform adapter directories: SDL desktop services and import
logic live in `src/platform/desktop/`, POSIX host runtime and filesystem code
in `src/platform/linux/`, and target-specific implementations in `windows/`,
`android/` and `dreamcast/`. Some existing files intentionally use target
conditionals for macOS, Android and WebAssembly; update their manifest and
callers together when changing that behavior. Graphics APIs remain grouped in
their backend directories, separate from the desktop SDL presenter.

`GEX_PLATFORM_FILE` selects the manifest. A toolchain file can select the
compiler, sysroot and target, and can supply the manifest as the Dreamcast
toolchain does. Configure in a **new, target-specific build directory**:
cached `GEX_RENDERER_BACKEND_SOURCE` and `GEX_MOVIE_DECODER_SOURCE` can
otherwise retain the previous platform's files. Before configuration,
generated lift sources must exist; `gex_codegen` cannot bootstrap an initial
configuration that has no generated sources. Configuration of an HLE target
also runs `tools/prepare_runtime.py`, which can apply checked-in patches to
the separate pcrecomp checkout; inspect local changes and its effects first.
See [generated-code changes](generated-code-changes.md) for generation
prerequisites and preservation precautions.

### 3. Implement contracts, not just link symbols

Read each header and its callers as well as comparable implementations. Useful
starting points and obligations:

| Area | Interface and examples | Porting invariant |
| --- | --- | --- |
| Startup and services | `src/main.c`, `src/platform/host.h`; `src/platform/desktop/services.c`, `src/platform/dreamcast/services.c` | Provide a valid guest VA for guest allocations; choose writable, session-only or failed settings storage explicitly. Arrange host entry, lifecycle and shutdown without changing guest startup semantics. |
| Machine, threads and time | `src/platform/runtime.h`; `src/platform/linux/runtime.c`, `src/platform/windows/runtime.c`, `src/platform/dreamcast/runtime.c`; target branches include macOS, Android and WebAssembly | The machine lock is nonrecursive; the compatibility layer handles nesting. Workers must start asynchronously, and their arguments must outlive them. Use a monotonic clock for runtime timing; honor blocking/yield and callback rules. |
| Guest and virtual memory | `src/platform/runtime.h`, `src/platform/hle/memory.c`, `src/platform/hle/flat_memory.c`, Linux `src/platform/linux/virtual_memory.c`; Dreamcast `memory.c` | Preserve guest address translation, alignment and allocation lifetime. Reservations must not replace unrelated host memory; file mappings must satisfy their requested semantics or fail explicitly. Dreamcast's restricted fixed mapping is target-specific, not a generic recipe. |
| Files and settings | `src/platform/filesystem.h`; Linux `src/platform/linux/filesystem.c`, Windows `src/platform/windows/filesystem.c`, Dreamcast `src/platform/dreamcast/filesystem.c`; settings are handled by selected host services | Preserve file token, path, metadata and thread-local error contracts. Atomic replacement consumes the stream, flushes/syncs/closes and replaces in the same directory; report failures and do not destroy the prior settings. |
| Input | `src/platform/input.h`, `src/gamepad.c`; desktop and Dreamcast `input.c` | Poll on the owning thread, publish focus/connect/disconnect and reset state, and preserve key edges and keyboard/pad merging. Honor modal input filters and coordinate conventions. |
| Audio | `src/platform/audio.h`, `src/platform/audio_backend.h`, `src/platform/audio.c`; Linux, Windows and Dreamcast `audio.c` | Implement voice ownership, format, cursor, gain and looping contracts. Native polling runs under the shared audio lock and must not invoke guest code; avoid running callbacks while holding locks they may re-enter. |
| Fonts and display | `src/platform/font.h`, `src/platform/video.h` and HLE screen/presentation entry points; desktop and Dreamcast implementations | Keep font metrics, mask ownership, display sizes, event pumping and required thread affinity consistent with callers. |
| Presentation and packets | `src/renderer.h`, `src/packet_gpu.h`; SDL `src/platform/desktop/renderer.c`, `src/platform/dreamcast/renderer.c`, `src/platform/packet_gpu_select.c` | CPU framebuffer presentation and packet rendering are distinct. Respect RGB565/pitch, borrowed-frame lifetime and configure failure behavior. A GPU-owned frame can have NULL CPU pixels. |
| Movies | `src/movie_decoder.h`, `src/movie.c`; desktop decoder and Dreamcast `movie_io.c` / `movie_pixels.c` | Stream events with specified timestamp, buffer lifetime and EOF/rewind behavior. The caller, not the decoder, owns timing and audio output. |

Some HLE contracts are declared in the selected pcrecomp runtime headers, not
only in `src/platform/*.h`; read their callers and the existing backend before
declaring an implementation complete. Keep guest blocking waits surrounded by
`hle_block_begin()` / `hle_block_end()` where the compatibility code requires
them, so other guest work can progress. Do not use a no-op machine lock or
execute a guest callback from a native audio poll.

For an initial graphics path, assess the portable software packet rasterizer
and implement its presentation backend first where feasible. Hardware packet
rendering has separate resource, synchronization and correctness requirements.
`gex_packet_gpu_get()` returning NULL means no packet GPU backend; it is not
itself a fallback. Confirm the selected features in the actual CMake build.

### 4. Integrate and validate incrementally

1. Bring up the target entry point and diagnostics; check executable identity,
   asset discovery, paths and failure messages before starting the guest.
2. Exercise memory allocation, synchronization, timer and filesystem
   contracts, including failure and cleanup paths. Confirm guest threads and
   callbacks can make progress under the target lifecycle.
3. Bring up software presentation and input; check actual output and input
   state, not just frame counts. Then check audio, movie transitions, settings
   and game shutdown. Add hardware rendering only after its contract is covered.
4. Add appropriate tests and register them under real build conditions in
   CMake. Distinguish tests that can run on the build host from tests that
   require a target, emulator, device, special driver or original assets.
5. Add a target-specific build/package route and document compiler and
   dependency versions, feature limitations, user-data import, storage
   behavior and recovery. Keep proprietary files out of packages and commits.
6. Report separately what compiled, what ran, and where; include skipped tests
   and outstanding target limitations. Cross-compilation is not evidence that
   presentation, sound or gameplay works on the target.

Routine validation is on the current host; cross-builds, emulators and devices
require an explicit target request. Automated game and test launches must be
headless, bounded and isolated; use the
[testing skill](../.github/skills/testing/SKILL.md) and
[platform build runbook](platform-build-testing.md).

## Move existing functionality behind a platform backend

### Decision procedure

1. Trace the behavior from the guest call or game hook to its implementation
   and callers. Identify return values, guest-visible errors, handle and
   buffer ownership, ordering, thread affinity, callbacks, register/flag
   state and shutdown behavior. Note any target-specific assumptions.
2. Decide whether the change belongs in **guest semantics**, a **portable
   game operation**, or a **host primitive**. Preserve guest API names and
   calling conventions when the guest still imports them. Prefer extending an
   existing narrow interface over scattering OS conditionals in shared code.
3. If a new interface is necessary, define explicit success/failure and
   ownership rules, choose its portable declaration and implement or
   deliberately reject it on **every affected target**. Select implementations
   through manifests and target linkage. Do not introduce a success-shaped
   stub or silently fall back to a different behavior.
4. Update feature detection, startup/teardown, tests and user-visible
   limitations together. Avoid exposing a UI option when the underlying
   capability is unavailable. Test negative paths, not only the happy path.
5. If the behavior is inside lifted instructions rather than an existing
   import or hook, use a checked-in, address-specific generation rewrite and
   handwritten implementation. Validate the original semantics and
   non-intercepted path, guest register/flag synchronization, expected hook
   counts and idempotence; never hand-edit `src/recomp/gen/`. Follow the
   [generated-code recipe](generated-code-changes.md).

Changes to reusable pcrecomp runtime behavior belong in this project's
checked-in `patches/` workflow when appropriate, not as undocumented edits to
the ignored checkout. Check patch applicability and the pinned toolchain;
`tools/prepare_runtime.py` applies patches and can fail on conflicting changes.

### Example: replace original DirectDraw behavior

The existing graphics route illustrates a deliberate guest-level bypass, not
a blanket replacement of all Win32 APIs:

1. `tools/graphics_rewrite.py` names verified guest functions and instruction
   anchors. `tools/generate.py` invokes the transformation and validates
   required hooks.
2. The rewritten game calls `src/graphics_guest.c`, which connects guest
   drawing operations to portable rendering and the selected presentation or
   packet backend. `src/renderer.h` and `src/packet_gpu.h` are separate
   contracts with different ownership rules.
3. `src/platform_shims.c` registers an `obsolete_directdraw` handler for
   `DirectDrawCreate` that fails if reached. Root CMake defines
   `GEX_NO_DIRECTDRAW` for the HLE runtime. Reaching that import is evidence
   of an incomplete graphics rewrite, not a successful port.
4. `tests/test_graphics_rewrite.py` checks the transformation; renderer,
   guest-graphics and applicable backend tests cover different layers. Actual
   presentation still requires appropriate output evidence.

For an ordinary host operation that already has an interface, do **not** add a
new guest rewrite just to choose a backend. For example, save writes in
`src/save_store.c` call `gex_fs_replace_begin()` and
`gex_fs_replace_commit()`; selected filesystem backends implement the host
mechanism while shared code retains the save format and error handling.
The Dreamcast backend explicitly lacks persistent VMU saving; VMU LCD image
display is a separate feature.

## Maintain existing ports

When modifying a portable interface or shared behavior:

1. Search for every declaration, implementation and caller, including the
   selected pcrecomp HLE sources and checked-in patches. Review all platform
   manifests and `CMakeLists.txt` conditions; do not assume similarly named
   backends are selected on every target.
2. Update implementations and shared tests together. Preserve binary data,
   ownership, error, timing, threading and lifecycle contracts; document
   intentional capability differences and migration/fallback behavior.
   Native32 is a separate route and may need a distinct change or an explicit
   statement that a shared HLE feature does not apply.
3. Review Android activity/import and shared-library lifecycle, macOS bundle
   and architecture packaging, Windows MinGW versus MSVC dependencies, and
   Dreamcast storage/memory/device constraints when the affected subsystem
   crosses those surfaces. Use the existing target runbooks rather than
   assuming one host's result covers the others.
4. Run the smallest relevant current-host tests, then broader current-host
   checks when shared runtime behavior warrants them. `ctest -N` shows what
   the configured build actually registered; a missing or skipped test is not
   a pass. `tests/test_platform_boundary.py` guards the portable source
   boundary and manifest validation, but cannot prove behavior on each host.
5. Record a port's build and runtime evidence separately from its intended
   support. For graphics, a CPU framebuffer check does not prove a GPU-owned
   present; inspect the actual output with a suitable target-specific method.
   For gameplay, the documented first-graveyard smoke check is not a full
   playthrough. Record target-specific checks that remain unperformed.
6. Update this guide and the relevant platform-specific documentation when
   contracts, setup, feature gates or known limitations change. Preserve the
   user's existing settings, original game files and release artifacts.

For a subsystem starting point and test names, use the task tables in
[project structure](project-structure.md). For missing dependencies, stale
build caches or missing tests, use
[build and test troubleshooting](build-test-troubleshooting.md); for renderer
evidence use [renderer debugging](renderer-debugging.md).
