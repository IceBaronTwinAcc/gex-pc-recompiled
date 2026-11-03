#!/usr/bin/env python3
"""Generate desktop and Android checksum tables from the pinned GOG asset manifest."""

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "config/game-assets.sha256"
DIRECTORIES = ("AVI", "IDL", "LEV", "MUS", "SFX", "VFX")


def load_manifest(path=MANIFEST):
    entries = []
    seen = set()
    for line in path.read_text().splitlines():
        if not line or line.startswith("#"):
            continue
        digest, size, name = line.split("\t")
        parts = PurePosixPath(name).parts
        if (not re.fullmatch(r"[a-f0-9]{64}", digest) or not size.isdecimal()
                or int(size) <= 0 or "\\" in name or name.startswith("/")
                or any(part in ("", ".", "..") for part in name.split("/"))
                or (name != "GEX.exe" and (len(parts) != 2 or parts[0] not in DIRECTORIES))
                or name.casefold() in seen):
            raise ValueError(f"Invalid asset manifest entry: {name}")
        seen.add(name.casefold())
        entries.append((name, int(size), digest))
    if "gex.exe" not in seen or not all(any(name.startswith(folder + "/")
            for name, _, _ in entries) for folder in DIRECTORIES):
        raise ValueError("Asset manifest is missing the executable or required asset categories")
    return entries


def generate_c(output, manifest=MANIFEST):
    entries = load_manifest(manifest)
    lines = ["/* Generated from config/game-assets.sha256; no retail data. */",
             "static const struct { const char *path; uint64_t size; const char *sha256; }",
             "gex_required_assets[] = {"]
    lines += [f"    {{{json.dumps(name)}, UINT64_C({size}), \"{digest}\"}},"
              for name, size, digest in entries]
    output.write_text("\n".join([*lines, "};", ""]))


def generate_java(output, manifest=MANIFEST):
    entries = load_manifest(manifest)
    lines = ["// Generated from config/game-assets.sha256; no retail data.",
             "package org.gexrecomp.app;", "", "final class GameAssets {",
             "    static final class Entry {",
             "        final String path, sha256;",
             "        final long size;",
             "        Entry(String path, long size, String sha256) {",
             "            this.path = path; this.size = size; this.sha256 = sha256;",
             "        }", "    }", "    static final Entry[] FILES = {"]
    lines += [f"        new Entry({json.dumps(name)}, {size}L, \"{digest}\"),"
              for name, size, digest in entries]
    output.write_text("\n".join([*lines, "    };", "    private GameAssets() {}", "}", ""]))


def record(source, output):
    """Explicit maintainer operation; normal builds never derive trusted hashes from user files."""
    executable = source / "GEX.exe"
    expected = "e1fd63ecb09fca29d63286e22447752dcb120d7e682f8759d1021776f7698b86"
    def checksum(file):
        digest = hashlib.sha256()
        with file.open("rb") as stream:
            for block in iter(lambda: stream.read(65536), b""):
                digest.update(block)
        return digest.hexdigest()

    if checksum(executable) != expected:
        raise ValueError("Unsupported reference executable")
    files = [executable]
    for name in DIRECTORIES:
        directory = source / name
        assets = sorted(directory.iterdir())
        if not assets or not all(path.is_file() and not path.is_symlink() for path in assets):
            raise ValueError(f"Invalid reference asset directory: {directory}")
        files.extend(assets)
    lines = ["# Supported GOG PC installation: SHA-256<TAB>bytes<TAB>relative path",
             "# Metadata only. Normal builds consume this pinned manifest, never regenerate it."]
    for file in files:
        digest = checksum(file)
        lines.append(f"{digest}\t{file.stat().st_size}\t{file.relative_to(source).as_posix()}")
    output.write_text("\n".join([*lines, ""]))
    load_manifest(output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("format", choices=("c", "java", "record"))
    parser.add_argument("output", type=Path)
    parser.add_argument("--source", type=Path)
    args = parser.parse_args()
    if args.format == "record":
        if args.source is None:
            parser.error("--source is required for recording a reference manifest")
        record(args.source, args.output)
    elif args.format == "c":
        generate_c(args.output)
    else:
        generate_java(args.output)


if __name__ == "__main__":
    main()
