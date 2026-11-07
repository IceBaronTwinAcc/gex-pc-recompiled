# Android testing: agent runbook

The [development guide](development.md#android) describes Android build
dependencies, packaging, import, and historical results. This page is the
short decision and safety guide for a **new** test. Read the
[testing skill](../.github/skills/testing/SKILL.md) and
[project structure](project-structure.md) first.

## Choose the scope

| Request | Appropriate check |
| --- | --- |
| Ordinary Linux change or documentation | Current-host checks only; do not cross-build or start Android. |
| Android importer logic without a device | Run the focused host-side Python/JDK test below. It does not exercise Android's picker or device runtime. |
| Explicit Android automated test | Use a dedicated, explicitly selected headless emulator; run the applicable checks on the requested APIs. |
| Explicit Android manual test | Use the requested device or emulator with visible fullscreen, a player-one gamepad, Vulkan and audible audio, following the testing skill. Do not silently substitute autoplay or a keyboard. |
| Physical device | Use only when the user explicitly authorizes that device and the requested actions. A prior test on a device is not permission to change it now. |

Headless automation uses dummy/null audio. A windowless emulator's
`--native-display` validation uses a real Android surface but still must not
open a host window; it is not the same as user-controlled manual play.

## Host-only importer test

From the project root:

```sh
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests -p 'test_android_import.py'
```

The test needs `javac` and `java`; a skipped test is not a pass. It builds a
temporary Java fixture. If `Gex/GEX.exe` exists, it also copies original game
files into a temporary directory to test integrity failures; allow enough
space and preserve the original installation. This exercises `GameData`
validation on the host, **not** the Android document picker, APK installation,
activity lifecycle, graphics driver, or gameplay.

## Before using adb

`tools/test-android.py` requires an explicit serial and a built, debuggable APK
plus the test binaries from the **same ABI and build**. Both APIs are packaged
in the Android build, but actual Vulkan and GLES 3.1 support depends on the
device or emulator. Review the current script and the
[Android build instructions](development.md#android); an Android build is
cross-platform work and is not routine Linux validation.

**The runner changes device state.** Before running it, choose a dedicated test
emulator/device the user authorized and understand these effects:

- It runs `adb install -r` for `org.gexrecomp.app`, replacing the installed APK
  when the signing key is compatible, and force-stops that package.
- `--data Gex` archives the selected host directory and extracts it into
  `files/Gex` in the app's private storage. This can overwrite or merge with
  an existing private installation; it is not the safe document-picker import.
  Use `--data` only on a disposable, isolated installation.
- `--reuse-data` avoids copying game files, but still installs/replaces the
  APK, stops the app, removes and rewrites `files/last-run.log`, and removes
  `cache/validation`. It does **not** mean a read-only device test.
- `--native-display` clears, enables and then disables SurfaceFlinger timing
  statistics, which are device-wide. Use only a dedicated test target with
  explicit permission; it checks buffer submissions, not scanout pixels.
- The runner normally removes its own `/data/local/tmp/gex-recomp-*` fixture
  directory and `cache/validation` in `finally`. Setup failures before that
  block, interruptions, or adb failures during cleanup can leave artifacts or
  a running activity. Never assume cleanup succeeded; inspect only this run's
  recorded target and paths. Do not delete unrelated data or uninstall/clear
  the user's app.

Do not use `adb` without `-s`, select the first connected device automatically,
overwrite existing APKs/data, wipe an AVD, or change global device settings
without authorization. Preserve normal app saves and original game files.

Read-only discovery, after selecting a target:

```sh
adb devices -l
SERIAL='replace-with-explicitly-selected-serial'
adb -s "$SERIAL" get-state
adb -s "$SERIAL" shell getprop ro.product.cpu.abilist
adb -s "$SERIAL" shell getprop ro.build.version.sdk
```

The values must identify the intended online target, support the APK ABI, and
meet the API requirement in the development guide. Device identity alone is
not authorization. Android's system picker is interactive: use a separate
explicitly authorized manual import test when that behavior is the subject.

## Automated headless run

Use an already running **dedicated** headless emulator whose AVD/data state the
user permits this test to change. Android's emulator can be configured with
`-no-window -no-audio -gpu swangle -feature Vulkan` on suitable installations;
confirm its actual capabilities rather than assuming these flags always work.
Do not start or overwrite an existing personal AVD without permission. Record
the exact emulator process/session you started and its serial; stop only that
process after testing.

Confirm the Android test build contains `gex-recomp-debug.apk`, exactly one
`lib/<abi>/` directory, and the fixture binaries listed in
`tools/test-android.py`. The runner needs `BUILD_TESTING` and a compatible
software-enabled bgfx build for its packet fixture. For a test build, use
the appropriate `GEX_ANDROID_ABI` and build instructions in
[development.md](development.md#android); the helper may download/build
dependencies and writes build and `.deps/` outputs. Inspect existing outputs
and preserve them before choosing a build path.

On a disposable x86-64 emulator with no existing private installation, for
example:

```sh
SERIAL='replace-with-explicitly-selected-serial'
python3 tools/test-android.py --serial "$SERIAL" \
    --build build/android/x86_64 --data Gex
```

The default runs **both** forced APIs. To test just one, append `--api vulkan`
or `--api opengles`; repeat `--api` to select both. `--full-movies` adds
complete-intro and normal-exit checks for each selected API. With separately
authorized existing private game data, substitute `--reuse-data` for `--data`
after reviewing the side effects above. Do not add `--native-display` unless
surface-submission evidence is specifically needed and its device-wide
effects are authorized; the display ID must be the target's actual ID.

The runner uses per-command and per-check timeouts, not one overall deadline.
Allow enough time for the selected checks; capture its output and inspect the
exit status. An external forced timeout can interrupt its cleanup while a
remote process is still running. If interrupted, inspect the recorded serial,
activity, and exact fixture directory before stopping or cleaning anything.

## Interpret results

| Evidence | What it proves—and does not prove |
| --- | --- |
| `adb install -r` succeeds | The APK installed on that serial; it does not prove the activity booted or the expected API ran. |
| `[bgfx] Vulkan` or `[bgfx] OpenGL ES` plus fixture `PASS` | The selected API and fixture checks ran. Confirm a marker and PASS for **each** requested API. |
| `first-graveyard=PASS` | The automated movement/frame thresholds passed for that API; it is not a full playthrough or controller test. |
| Two `playback completed` messages and `ExitProcess(0)` | The runner's separate full-movie and normal-exit checks passed when `--full-movies` was requested. They are separate launches. |
| `PASS ... native-presentation` | The SurfaceFlinger submission-count constraint passed on that display. It does not verify visible pixels or physical scanout. |
| `RESULT: ... Android checks passed` and exit zero | The runner completed its selected checks. The count varies with APIs and flags; compare the actual `PASS` lines to the request, not a historical count. |

An APK build, host-only import test, headless frame count, or prior-device
result cannot establish present-day physical audio, gamepad input, visual
quality, another device/API, or a full playthrough. Actual screen inspection
requires appropriate explicit authorization and must be reported separately.

## Common failures

| Symptom | Next action |
| --- | --- |
| Offline, unauthorized, or ambiguous adb target | Check `adb devices -l`, serial and authorization; do not guess another device. |
| APK install/signature failure | Inspect the exact error and signing key. Do not uninstall or clear an existing app to bypass it. |
| ABI mismatch, missing APK/library/fixture | Check `--build`, its sole `lib/<abi>` and device ABI; verify the selected configuration built the required test targets. |
| `--reuse-data` missing executable or `run-as` failure | Check private installation and debuggable APK/signing context on the authorized target. Do not overwrite or use `--data` as a silent fallback. |
| API marker absent or GPU fixture fails | Confirm the selected emulator/device actually supports the forced API and required features. Do not report a fallback API as a pass. |
| Missing data, startup error, or timeout | Inspect the runner output and the authorized app's `files/last-run.log` where available; preserve failure evidence. Check cleanup rather than repeating destructive setup. |

The APK contains no retail game files. Never distribute the user's original
installation, test archive, or test data with artifacts or logs.
