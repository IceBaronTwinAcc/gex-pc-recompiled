#!/usr/bin/env python3
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--elf", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--game-dir", type=Path, default=Path("Gex"))
    parser.add_argument("--mkdcdisc", default="mkdcdisc")
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--original-movies", action="store_true")
    parser.add_argument("--file", type=Path, action="append", default=[])
    parser.add_argument("--name", default="Gex recompilation")
    args = parser.parse_args()

    game_dir = args.game_dir.resolve(strict=True)
    elf = args.elf.resolve(strict=True)
    output = args.output.resolve()
    files = [path.resolve(strict=True) for path in args.file]
    if output.suffix.lower() != ".cdi":
        parser.error("output must be a CDI file")
    if output.is_relative_to(game_dir):
        parser.error("output must not be inside the original game directory")
    if not output.parent.is_dir():
        parser.error("output directory does not exist")

    with tempfile.TemporaryDirectory(prefix=".gex-disc-", dir=output.parent) as temp:
        work = Path(temp)
        staged = work / "Gex"
        shutil.copytree(game_dir, staged)
        if not args.original_movies:
            for name in ("GEX000.AVI", "GEX001.AVI"):
                source = game_dir / "AVI" / name
                destination = staged / "AVI" / name
                subprocess.run([
                    args.ffmpeg, "-hide_banner", "-loglevel", "error", "-nostdin", "-y",
                    "-i", str(source), "-vf", "fps=15,scale=320:240",
                    "-c:v", "mpeg1video", "-strict", "unofficial", "-b:v", "850k", "-minrate", "850k",
                    "-maxrate", "850k", "-bufsize", "850k", "-ac", "2",
                    "-ar", "32000", "-c:a", "mp2", "-b:a", "128k",
                    "-muxrate", "1200000", "-preload", "700000", "-f", "mpeg",
                    str(destination),
                ], check=True)
        image = work / "image.cdi"
        command = [args.mkdcdisc, "-e", str(elf), "-d", str(staged)]
        for file in files:
            command.extend(["-f", str(file)])
        command.extend(["-n", args.name, "-o", str(image)])
        subprocess.run(command, check=True)
        os.replace(image, output)


if __name__ == "__main__":
    main()
