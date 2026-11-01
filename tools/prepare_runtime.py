#!/usr/bin/env python3
"""Apply the project's checked-in fixes to the pinned pcrecomp checkout."""

import argparse
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--toolchain", type=Path, default=ROOT / "pcrecomp")
    args = parser.parse_args()
    toolkit = args.toolchain.resolve()
    for patch in sorted((ROOT / "patches").glob("*.patch")):
        command = ["git", "-C", str(toolkit), "apply"]
        applied = subprocess.run(
            command + ["--reverse", "--check", str(patch)], capture_output=True
        )
        if applied.returncode == 0:
            continue
        subprocess.run(command + ["--check", str(patch)], check=True)
        subprocess.run(command + [str(patch)], check=True)
        print(f"Applied {patch.name}")


if __name__ == "__main__":
    main()
