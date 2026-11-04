#!/usr/bin/env python3
"""Package a macOS app and asset-free archive without modifying imported game data."""

import argparse
from pathlib import Path
import plistlib
import shutil
import tempfile
import zipfile


def package(build, prefix):
    bundle = build / "Gex Recompiled.app"
    contents = bundle / "Contents"
    metadata = plistlib.loads((contents / "Info.plist").read_bytes())
    if metadata["CFBundleExecutable"] != "Gex Recompiled":
        raise ValueError("Unexpected macOS bundle executable")
    binary = contents / "MacOS" / metadata["CFBundleExecutable"]
    if not binary.is_file():
        raise FileNotFoundError(binary)
    if metadata.get("CFBundleIconFile") != "gex.icns":
        raise ValueError("Unexpected macOS bundle icon")
    icon = contents / "Resources/gex.icns"
    if not icon.is_file():
        raise FileNotFoundError(icon)
    libraries = sorted((prefix / "lib").glob("*.dylib"))
    if not libraries:
        raise FileNotFoundError(f"No macOS runtime libraries in {prefix / 'lib'}")
    frameworks = contents / "Frameworks"
    notices = contents / "Resources" / "licenses"
    frameworks.mkdir(parents=True, exist_ok=True)
    notices.mkdir(parents=True, exist_ok=True)
    for library in libraries:
        shutil.copy2(library, frameworks / library.name)
    for notice in sorted((build / "licenses").glob("*.txt")):
        shutil.copy2(notice, notices / notice.name)
    shutil.copy2(binary, build / "gex_recomp")
    output = build / "gex-recomp-macos.zip"
    # Allowlist distributable content, even when this build's app has imported assets.
    packaged = [contents / "Info.plist", icon]
    for directory in (contents / "MacOS", frameworks, notices):
        packaged.extend(path for path in sorted(directory.rglob("*")) if path.is_file())
    with tempfile.TemporaryDirectory(prefix="gex-package-", dir=build) as temporary:
        archive = Path(temporary) / output.name
        with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as destination:
            for path in packaged:
                destination.write(path, Path(bundle.name) / path.relative_to(bundle))
        archive.replace(output)
    return bundle, output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prefix", type=Path, required=True)
    parser.add_argument("build", type=Path)
    arguments = parser.parse_args()
    bundle, archive = package(arguments.build.resolve(), arguments.prefix.resolve())
    print(f"macOS app: {bundle}")
    print(f"Distribution archive (no original game files): {archive}")


if __name__ == "__main__":
    main()
