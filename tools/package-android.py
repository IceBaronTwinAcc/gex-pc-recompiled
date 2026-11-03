#!/usr/bin/env python3
"""Package the cross-built SDL game without Gradle or bundled retail data."""

import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile
import zipfile
from game_asset_manifest import generate_java


def run(*arguments):
    subprocess.run([str(value) for value in arguments], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", type=Path, required=True)
    parser.add_argument("--sdl", type=Path, required=True)
    parser.add_argument("build", type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    sdk, build = args.sdk.resolve(), args.build.resolve()
    tools = sdk / "build-tools/36.1.0"
    android = sdk / "platforms/android-36/android.jar"
    lambdas = tools / "core-lambda-stubs.jar"
    for path in [android, lambdas, *(tools / name for name in ("aapt2", "d8", "zipalign", "apksigner"))]:
        if not path.is_file():
            parser.error(f"Required Android SDK component missing: {path}")
    for name in ("javac", "keytool"):
        if not shutil.which(name):
            parser.error(f"Required JDK tool missing: {name}")
    sources = sorted((args.sdl / "android-project/app/src/main/java").rglob("*.java"))
    sources += sorted((root / "android/src").rglob("*.java"))
    if not sources or not any(source.name == "SDLActivity.java" for source in sources):
        parser.error("SDL Java sources are missing")
    libraries = sorted((build / "lib").glob("*/*.so"))
    if not any(path.name == "libgex_recomp.so" for path in libraries):
        parser.error("Build the native Android libraries first")
    key = root / ".deps/android-debug.keystore"
    if not key.exists():
        run("keytool", "-genkeypair", "-keystore", key, "-storepass", "android",
            "-keypass", "android", "-alias", "androiddebugkey", "-keyalg", "RSA",
            "-validity", "10000", "-dname", "CN=Android Debug")
    with tempfile.TemporaryDirectory(prefix="gex-apk-") as directory:
        work = Path(directory)
        classes, dex = work / "classes", work / "dex"
        classes.mkdir()
        dex.mkdir()
        manifest = work / "GameAssets.java"
        generate_java(manifest)
        run("javac", "-source", "8", "-target", "8", "-bootclasspath", f"{android}:{lambdas}",
            "-d", classes, *sources, manifest)
        jar = work / "classes.jar"
        with zipfile.ZipFile(jar, "w", zipfile.ZIP_DEFLATED) as archive:
            for path in sorted(classes.rglob("*.class")):
                archive.write(path, path.relative_to(classes))
        run(tools / "d8", "--min-api", "26", "--lib", android, "--output", dex, jar)
        unsigned = work / "unsigned.apk"
        run(tools / "aapt2", "link", "-o", unsigned, "-I", android,
            "--manifest", root / "android/AndroidManifest.xml")
        with zipfile.ZipFile(unsigned, "a", zipfile.ZIP_DEFLATED) as archive:
            for path in sorted(dex.glob("*.dex")):
                archive.write(path, path.name)
            for path in libraries:
                archive.write(path, path.relative_to(build))
            for path in sorted((build / "licenses").glob("*.txt")):
                archive.write(path, "assets/licenses/" + path.name)
        aligned = work / "aligned.apk"
        run(tools / "zipalign", "-P", "16", "-f", "4", unsigned, aligned)
        output = build / "gex-recomp-debug.apk"
        run(tools / "apksigner", "sign", "--ks", key, "--ks-pass", "pass:android",
            "--key-pass", "pass:android", "--out", output, aligned)
        run(tools / "apksigner", "verify", "--verbose", output)
        print(f"Android development APK: {output}")


if __name__ == "__main__":
    main()
