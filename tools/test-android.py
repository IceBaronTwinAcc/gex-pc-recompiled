#!/usr/bin/env python3
"""Headless Android GPU checks on an explicitly selected adb device."""

import argparse
from pathlib import Path
import re
import shlex
import subprocess
import tarfile
import tempfile
import time


PACKAGE = "org.gexrecomp.app"
ACTIVITY = PACKAGE + "/.GexActivity"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", required=True, help="adb device serial (use a headless emulator for automation)")
    parser.add_argument("--build", type=Path, default=Path("build/android/x86_64"))
    installation = parser.add_mutually_exclusive_group(required=True)
    installation.add_argument("--data", type=Path, help="Your unmodified GOG Gex directory")
    installation.add_argument("--reuse-data", action="store_true",
                              help="Preserve and use the game's existing private installation")
    parser.add_argument("--full-movies", action="store_true", help="Also complete both intro movies on each API")
    parser.add_argument("--api", action="append", choices=("vulkan", "opengles"),
                        help="Check this bundled API (repeatable; default: both)")
    parser.add_argument("--native-display", type=int,
                        help="Also check native swapchain submissions on this display of a dedicated test device")
    args = parser.parse_args()
    build = args.build.resolve()
    if args.data is not None and not (args.data / "GEX.exe").is_file():
        parser.error("--data must contain GEX.exe")
    libraries = list((build / "lib").iterdir())
    if len(libraries) != 1:
        parser.error("Expected one ABI in the selected build's lib/ directory")
    abi = libraries[0].name

    def adb(*arguments, timeout=180, check=True):
        result = subprocess.run(["adb", "-s", args.serial, *map(str, arguments)],
                                capture_output=True, text=True, timeout=timeout)
        if check and result.returncode:
            raise RuntimeError(f"adb failed: {arguments}\n{result.stdout}{result.stderr}")
        return result

    def shell(*arguments, **kwargs):
        return adb("shell", shlex.join(map(str, arguments)), **kwargs)

    available = shell("getprop", "ro.product.cpu.abilist").stdout.strip().split(",")
    if abi not in available:
        parser.error(f"{abi} build cannot run on device ABIs {available}")
    adb("install", "-r", build / "gex-recomp-debug.apk")
    if args.reuse_data:
        if shell("run-as", PACKAGE, "test", "-f", "files/Gex/GEX.exe", check=False).returncode:
            parser.error("--reuse-data requires an existing private Gex installation")
    else:
        shell("run-as", PACKAGE, "mkdir", "-p", "files/Gex")
        with tempfile.TemporaryFile() as archive:
            with tarfile.open(fileobj=archive, mode="w", dereference=True) as contents:
                contents.add(args.data.resolve(), arcname=".")
            archive.seek(0)
            command = shlex.join(["run-as", PACKAGE, "tar", "-xf", "-", "-C", "files/Gex"])
            subprocess.run(["adb", "-s", args.serial, "exec-in", command], stdin=archive,
                           check=True, timeout=180)
    remote = shell("mktemp", "-d", "/data/local/tmp/gex-recomp-XXXXXX").stdout.strip()
    if not re.fullmatch(r"/data/local/tmp/gex-recomp-[A-Za-z0-9]+", remote):
        raise RuntimeError(f"Unexpected remote test directory: {remote!r}")
    passed = 0
    try:
        adb("push", libraries[0], remote + "/lib")
        for binary in ("gex_bgfx_test", "gex_bgfx_present_test",
                       "gex_runtime_primitives_test", "gex_runtime_unaligned_test",
                       "gex_audio_device_test", "gex_presentation_test",
                       "gex_password_saves_test"):
            adb("push", build / binary, remote + "/" + binary)
            shell("chmod", "700", remote + "/" + binary)
        shell("mkdir", "-p", remote + "/settings")
        environment = ["env", f"LD_LIBRARY_PATH={remote}/lib", "SDL_VIDEODRIVER=dummy",
                       "SDL_AUDIODRIVER=dummy", "ALSOFT_DRIVERS=null",
                       f"XDG_DATA_HOME={remote}/settings"]
        if args.data is not None:
            adb("push", args.data.resolve() / "GEX.exe", remote + "/GEX.exe")
        else:
            with tempfile.TemporaryDirectory(prefix="gex-android-passwords-") as work:
                executable = Path(work) / "GEX.exe"
                with executable.open("wb") as file:
                    subprocess.run(["adb", "-s", args.serial, "exec-out", "run-as", PACKAGE,
                                    "cat", "files/Gex/GEX.exe"], stdout=file, check=True, timeout=30)
                adb("push", executable, remote + "/GEX.exe")
        shell("mkdir", "-p", remote + "/settings/decks", remote + "/settings/operations")
        for mode in ("write", "read", "threaded", "menu", "operations",
                     "options-write", "options-read", "options-error"):
            directory = "operations" if mode == "operations" else "decks"
            result = shell("timeout", "-k", "5", "30", *environment,
                           remote + "/gex_password_saves_test", remote + "/GEX.exe", mode,
                           remote + "/settings/" + directory)
            print(result.stdout + result.stderr, end="", flush=True)
            print("PASS password-decks", mode, flush=True)
            passed += 1
        for binary in ("gex_runtime_primitives_test", "gex_runtime_unaligned_test",
                       "gex_audio_device_test", "gex_presentation_test"):
            result = shell("timeout", "-k", "5", "170", *environment, remote + "/" + binary)
            print(result.stdout + result.stderr, end="", flush=True)
            print("PASS", binary, flush=True)
            passed += 1
        apis = tuple((api, marker) for api, marker in
                     (("vulkan", "[bgfx] Vulkan"), ("opengles", "[bgfx] OpenGL ES"))
                     if api in (args.api or ("vulkan", "opengles")))
        for api, marker in apis:
            for binary in ("gex_bgfx_present_test", "gex_bgfx_test"):
                result = shell("timeout", "-k", "5", "170", *environment,
                               f"GEX_BGFX_API={api}", remote + "/" + binary)
                output = result.stdout + result.stderr
                print(output, end="", flush=True)
                if marker not in output or "PASS" not in output:
                    raise RuntimeError(f"{api} {binary} did not verify real GPU output")
                print("PASS", api, binary, flush=True)
                passed += 1

        for api, marker in apis:
            checks = [("gameplay", 90, ["--ez", "skip-movies", "true", "--ez", "autoplay", "true",
                                      "--ez", "verify-gameplay", "true", "--ei", "smoke-seconds", "60"],
                       ["first-graveyard=PASS"])]
            if args.full_movies:
                checks.append(("movies", 115, ["--ei", "smoke-seconds", "95"],
                               ["playback completed"]))
                checks.append(("exit", 90, ["--ez", "skip-movies", "true", "--ez", "autoplay", "true",
                                           "--ei", "smoke-seconds", "75"], ["ExitProcess(0)"]))
            for name, timeout, extras, required in checks:
                shell("am", "force-stop", PACKAGE)
                shell("run-as", PACKAGE, "rm", "-f", "files/last-run.log")
                shell("am", "start", "-W", "-n", ACTIVITY, "--es", "api", api,
                      "--ez", "headless", "true", *extras)
                deadline = time.monotonic() + timeout
                log = ""
                while time.monotonic() < deadline:
                    log = shell("run-as", PACKAGE, "cat", "files/last-run.log", check=False).stdout
                    if all(text in log for text in [marker, *required]):
                        if name != "movies" or log.count("playback completed") == 2:
                            break
                    if ("[gex] signal " in log or "[android] game returned " in log
                            or "first-graveyard=FAIL" in log):
                        raise RuntimeError(f"{api} {name} failed:\n{log}")
                    time.sleep(1)
                else:
                    raise RuntimeError(f"{api} {name} timed out:\n{log}")
                print(log, end="", flush=True)
                print("PASS", api, name, flush=True)
                passed += 1
            if args.native_display is not None:
                shell("am", "force-stop", PACKAGE)
                shell("run-as", PACKAGE, "rm", "-f", "files/last-run.log")
                shell("dumpsys", "SurfaceFlinger", "--timestats", "-clear", "-enable")
                try:
                    shell("am", "start", "-W", "--display", args.native_display,
                          "-n", ACTIVITY, "--es", "api", api, "--ez", "validation", "true",
                          "--ez", "skip-movies", "true", "--ei", "smoke-seconds", "12")
                    deadline = time.monotonic() + 45
                    while time.monotonic() < deadline:
                        log = shell("run-as", PACKAGE, "cat", "files/last-run.log", check=False).stdout
                        frames = re.search(r"\[smoke\] frames=(\d+) ", log)
                        if frames and marker in log and "[smoke] scene=63 " in log:
                            break
                        if "[gex] signal " in log or "[android] game returned " in log:
                            raise RuntimeError(f"{api} native presentation failed:\n{log}")
                        time.sleep(1)
                    else:
                        raise RuntimeError(f"{api} native presentation timed out:\n{log}")
                    stats = shell("dumpsys", "SurfaceFlinger", "--timestats", "-dump").stdout
                finally:
                    shell("dumpsys", "SurfaceFlinger", "--timestats", "-disable")
                displayed = 0
                for layer in re.split(r"(?=layerName = )", stats):
                    if layer.startswith(f"layerName = SurfaceView[{PACKAGE}/{PACKAGE}.GexActivity]"):
                        count = re.search(r"^totalFrames = (\d+)$", layer, re.MULTILINE)
                        if count:
                            displayed += int(count[1])
                complete = int(frames[1])
                if complete < 30 or not displayed or displayed > complete + 2:
                    raise RuntimeError(f"{api}: {displayed} native submissions for {complete} "
                                       f"complete images; packet flushes must not swap the screen\n{log}")
                print(f"PASS {api} native-presentation: complete={complete} displayed={displayed}",
                      flush=True)
                passed += 1
    finally:
        shell("am", "force-stop", PACKAGE)
        shell("rm", "-r", remote)
        shell("run-as", PACKAGE, "rm", "-rf", "cache/validation")
    print(f"RESULT: {passed} Android checks passed", flush=True)


if __name__ == "__main__":
    main()
