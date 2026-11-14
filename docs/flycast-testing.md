# Flycast testing: agent runbook

Use this guide for Dreamcast tests, especially with small-context models.
Do not reconstruct the commands from conversation history.

## Decide before acting

| Request | Action |
| --- | --- |
| Linux-only change or documentation | Do not build Dreamcast or run Flycast. |
| Explicit Dreamcast automated test | Hidden window, muted audio, bounded runner. |
| Explicit manual Flycast test | Visible fullscreen, Vulkan, audible audio, keyboard or gamepad; no autoplay or fast-forward. |
| VMU image check | Manual test, VMU on port A, inspect its LCD. |

Preserve `Gex/`, normal Flycast settings, existing saves, and release artifacts.
Read `AGENTS.md` and the testing skill. Do not change branches or commit.

## Find and verify the supplied emulator

Set `FLYCAST` to the actual absolute path of the user-supplied executable for
the current workspace. Use an attached file's current path if available; do
not assume another session's attachment directory exists. If its location is
unknown, ask the user. Run these checks before using it:

```sh
: "${FLYCAST:?Set FLYCAST to the absolute path of the supplied emulator}"
test -x "$FLYCAST"
file "$FLYCAST"
sha256sum "$FLYCAST"
timeout 10s "$FLYCAST" -help
```

Compare the checksum with the checksum of the supplied build, if known. A
different checksum identifies a different binary; do not claim it has this
build's capabilities. Obtain its provenance and verify compatibility before
using it. Help output alone cannot establish full-MMU behavior or the local
patches. The runtime evidence in **Verify before handing over** still applies.

### Previously supplied build: local example

The attachment in one workspace was located at:

```text
/home/ice/.copilot/workspaces/7c976ef4-72ce-40f3-9661-babd50c2bfaf/attachments/6d3e30c1-73ae-441e-8d06-a1501f2c9aa2-flycast
```

Its expected SHA-256 was:

```text
91021d2fe4b9ddb670fedec57aa35aab7e26746540b6cfeaacae94c97d5e61f1
```

This is a dynamically linked Linux x86-64 ELF, not a game binary or CDI.
Help-only startup was verified without opening a window. It lists `-headless`,
`-screenshot`, and `-screenshot-frame`; binary strings include `2ac5c77` and
`Enabling Full MMU support`. These checks do not establish a successful game
run or prove every local patch is present.

Use the supplied file directly; do not copy the 36-MiB executable into Git.
If missing, changed, or unable to load its libraries, report that problem.
Do not silently fall back
to `/usr/bin/flycast`, a stock AppImage, or an arbitrary newer release.
Only build a replacement when necessary, using the pinned commit and
`tools/flycast-pvr.patch` described in
[development.md](development.md#flycast-compatibility). Never disable MMU.

## Manual test: complete launch recipe

Prerequisites: Linux desktop, installed KOS toolchain,
`mkdcdisc`, `ffmpeg`, `.venv/bin/python`, generated game sources, and the
project's pinned Dreamcast MPEG decoder.
Check these before building. Follow the existing build tools for missing
project prerequisites; do not download another emulator.

Set `FLYCAST` to the verified path above and, if necessary, set `KOS_ENV` to
the local KallistiOS `environ.sh` path. Run the following block from the
repository root in one shell. It builds the
current source into a new scratch directory, creates a **test-only** CDI, copies
only emulator configuration/controller mappings, and launches for at most
15 minutes. It does not copy the user's VMU saves or change release binaries.
This PVR-only profile skips movies intentionally for the VMU/gameplay test,
not as evidence that movie playback works. The normal combined-renderer RAM
repair and full-intro checks are documented under
[Dreamcast reserve-only heap RAM](development.md#dreamcast-reserve-only-heap-ram).

```sh
set -euo pipefail
: "${FLYCAST:?Set FLYCAST to the verified supplied emulator}"
KOS_ENV=${KOS_ENV:-/opt/toolchains/dc/kos/environ.sh}
test -x "$FLYCAST"
test -d Gex
test -x .venv/bin/python
test -f "$KOS_ENV"
command -v mkdcdisc
WORK=$(mktemp -d /tmp/gex-flycast-manual.XXXXXX)
printf 'Manual test workspace: %s\n' "$WORK"
TEST_HOME="$WORK/home"
mkdir -p "$TEST_HOME/.config/flycast" "$TEST_HOME/.local/share/flycast"
if test -f "$HOME/.config/flycast/emu.cfg"; then
    cp "$HOME/.config/flycast/emu.cfg" "$TEST_HOME/.config/flycast/"
fi
if test -d "$HOME/.config/flycast/mappings"; then
    cp -R "$HOME/.config/flycast/mappings" "$TEST_HOME/.config/flycast/"
fi
source "$KOS_ENV"
bash tools/prepare-dreamcast-mpeg.sh
cmake -S . -B "$WORK/build" \
    -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/toolchains/dreamcast.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DPython3_EXECUTABLE="$PWD/.venv/bin/python" \
    -DBUILD_TESTING=OFF \
    -DGEX_ENABLE_SOFTWARE_RENDERER=OFF \
    -DGEX_ENABLE_PVR_RENDERER=ON > "$WORK/config.log" 2>&1
cmake --build "$WORK/build" --parallel 3 > "$WORK/build.log" 2>&1
printf '%s\n' '--data /cd/Gex --skip-movies --renderer pvr' \
    > "$WORK/gex-test.cfg"
python3 tools/package-dreamcast.py --elf "$WORK/build/gex_recomp.elf" \
    --file "$WORK/gex-test.cfg" --name 'Gex VMU manual test' \
    --output "$WORK/manual.cdi" > "$WORK/disc.log" 2>&1
env -u SDL_AUDIODRIVER -u SDL_VIDEODRIVER \
    HOME="$TEST_HOME" \
    XDG_CONFIG_HOME="$TEST_HOME/.config" \
    XDG_DATA_HOME="$TEST_HOME/.local/share" \
    XDG_CACHE_HOME="$TEST_HOME/.cache" \
    timeout --signal=INT --kill-after=5s 900s "$FLYCAST" \
    -config 'window:fullscreen=yes,config:pvr.rend=4,config:rend.EmulateFramebuffer=no,config:rend.RenderToTextureBuffer=no,config:aica.Volume=100,audio:backend=auto,config:Debug.SerialConsoleEnabled=yes,config:Debug.GDBEnabled=no,config:Debug.FastForward=no,config:rend.vsync=yes' \
    "$WORK/manual.cdi" 2>&1 | tee "$WORK/manual.log"
```

For agent tools, prepare/build first, then launch the final `env ... timeout`
command in a long-lived Bash session. Use `mode: async, detach: true` when the
manual application must survive returning control to the user, if permitted.
If detaching is unavailable, use an attached session and explain its lifetime.
Keep the 900-second limit. Save the returned `shellId` and printed `WORK` path.
Fresh tool shells do not inherit variables: repeat the exact recorded paths
in subsequent commands, not an assumed `$WORK` or `$TEST_HOME`.

Do not use `tools/test-dreamcast.sh` to launch a manual test: it deliberately
invokes a hidden, muted runner. Do not add guest `--headless`, `--autoplay`,
`--smoke-seconds`, or screenshot-and-exit options to the manual disc.
Guest `--renderer pvr` and Flycast `config:pvr.rend=4` select different layers.

## Verify before handing over

Read `manual.log`, check the emulator is still running, and confirm fullscreen.
Wayland windows may not appear in `xdotool`; an empty X11 window search alone
does not prove the application is invisible.

| Evidence | Required interpretation |
| --- | --- |
| Vulkan device/renderer initialization | Host presentation uses Vulkan. |
| `Opened joystick ... on port 0` | If a controller is used, it is mapped to player one; not proof every button is correct. |
| `Enabling Full MMU support` | MMU enabled, but game startup is not yet proven. |
| `bound 180 imports, 0 unresolved` | Guest executable imports resolved. |
| `presented first GPU framebuffer` | Guest reached native presentation; not a gameplay verdict. |
| `[dreamcast] VMU startup image sent` | Startup image submitted to the first detected VMU LCD; not proof of LCD appearance. |

Enable a VMU on port A and its LCD overlay in the isolated Flycast settings if
needed. The image is in `src/platform/dreamcast/vmu_image.xbm`: 48x32, 192 bytes,
displayed from `gex_host_settings_path()` at startup. Confirm the supplied
picture, orientation, and polarity by looking at the actual LCD overlay.
If the VMU was enabled after startup, restart the guest in this same profile.
Do not claim the new picture works based on the previous placeholder logo.

VMU save persistence is not implemented. LCD display is independent of storage;
the game does not read or write VMU saves.

## Failures and stopping

| Symptom | Next action |
| --- | --- |
| Executable/library failure | Inspect the actual loader error; report missing dependency or attachment. No emulator substitution. |
| No serial guest output | Check `config:Debug.SerialConsoleEnabled=yes`; do not infer success from MMU logs. |
| `bound 0 imports`, blocked SH4 exception | Suspect wrong/incompatible emulator. Verify path/hash before changing game code. |
| RAM exhaustion, `GEX: Memory Error`, panic, fatal error | Keep the failure log; do not report readiness. Check the selected build/profile. |
| No gamepad on port 0 | Use keyboard input, or correct the isolated input mapping if the user wants a controller. |
| Exit zero before readiness markers | Test ended, not necessarily passed. Inspect output and whether the user closed it. |
| Timeout exit 124 | Host time limit reached, not proof of a passing game test. |

The prior manual launch ended after about 18 seconds with MMU initialization
and a joystick disconnect, but without serial guest readiness evidence.
That run did **not** verify the new LCD bitmap or a VMU save.

Once ready, give the user control and leave the bounded process running.
Stop only the recorded test session with `stop_bash(shellId)` when requested;
never use `pkill` or `killall`. After stopping, inspect the recorded scratch
directory and delete only that test's known outputs. Retain failure logs while
unresolved.
Never delete the attachment, normal Flycast data, or `Gex/`.

## Automated tests

Use `tools/run-dreamcast-test.py` for an already prepared automated disc.
It supplies hidden-window mode, serial logging, muted audio, and failure/exit
handling. Isolate `HOME` and XDG directories as above. Its fast-forward default
is intentional for automation only; set `GEX_TEST_FAST_FORWARD=no` for timing
comparisons. Require the appropriate readiness markers and no fatal errors.
TODO: Document Flycast framebuffer emulation and diagnostic readback settings.
Do not convert a manual disc into an unattended gameplay claim: autoplay and
guest deadlines must be selected explicitly for automated tests.

Package test arguments as a newline-terminated file named **`gex-test.cfg`**.
The launcher reads only `/cd/gex-test.cfg`; a differently named `.cfg` leaves
the default interactive arguments in effect. Check for `[smoke]` observations
when a smoke deadline was requested instead of diagnosing a host timeout as
a guest deadlock. Timed `--autoplay` requires `--skip-movies`; full-intro checks
use `--headless --smoke-seconds 120 --renderer pvr` without autoplay. Require
both movie-completion messages, post-movie scene 63 and exit zero. Check normal
main-menu exit separately with skip-movie autoplay, requiring `ExitProcess(0)`.
