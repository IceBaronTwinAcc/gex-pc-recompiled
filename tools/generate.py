#!/usr/bin/env python3
"""Reproduce the GOG Gex lift with the pinned pcrecomp toolchain."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.graphics_rewrite import (
    FUNCTIONS as GRAPHICS_FUNCTIONS, FRAME_BEGIN, FRAME_END, PACKET_BEGIN, TITLE_HOOK as GRAPHICS_TITLE_HOOK,
    ARENA_HOOK, VIEW_HOOKS, SCREEN_HOOKS, HUD_DRAWS, PLAYER_DRAWS, rewrite_graphics,
)
from tools.save_rewrite import rewrite_saves, TITLE_HOOK, TITLE_INIT_HOOK, CHECKPOINT_HOOK, MANUAL_HOOK

ROOT = Path(__file__).resolve().parents[1]
GEX_SHA256 = "e1fd63ecb09fca29d63286e22447752dcb120d7e682f8759d1021776f7698b86"
TOOLCHAIN_COMMIT = "12dd9642cd3675024161bfc567b5b42ada0134d2"
FRAME_WAIT = (
    "L_00405188:\n"
    "    PUSH32(esp, MEM32(0x4517E8) - eax); /* 0x00405188: push 0 */"
)


def replace_frame_wait(text):
    # Keep the original deadline, clock recheck and late-frame policy.
    return text.replace(
        "L_00405188:\n    PUSH32(esp, 0); /* 0x00405188: push 0 */",
        FRAME_WAIT,
    )


def replace_audio_waits(text):
    for target, source in ((0x40A69E, 0x40A6A5), (0x40A6A7, 0x40A6AE), (0x40A6BA, 0x40A6C1)):
        original = (f"    if (TEST_Z(_flag_a, _flag_b)) goto L_{target:08X}; "
                    f"/* 0x{source:08X}: je 0x{target:x} */")
        replacement = (
            "    if (TEST_Z(_flag_a, _flag_b)) {\n"
            "        RECOMP_REGS_OUT(); gex_guest_wait(); RECOMP_REGS_IN();\n"
            f"        goto L_{target:08X};\n"
            f"    }} /* 0x{source:08X}: je 0x{target:x} */")
        text = text.replace(original, replacement)
    return text



def add_scheduler_points(text):
    def branch(match):
        target, source = int(match[2], 16), int(match[3], 16)
        return ("    RECOMP_BACKEDGE();\n" if target <= source else "") + match[0]
    return re.sub(
        r"(^[ \t]*[^\n]*goto L_([0-9A-Fa-f]+);[^\n]*?/\* 0x([0-9A-Fa-f]+):[^\n]*)",
        branch, text, flags=re.MULTILINE,
    )


def share_entry_bodies(sources):
    """Outline byte-identical lifted bodies that differ only by entry label."""
    functions = re.compile(r"^void (sub_([0-9A-Fa-f]+))\(void\) \{\n(.*?)^\}\n",
                           re.MULTILINE | re.DOTALL)
    groups = {}
    for filename, text in sources.items():
        for match in functions.finditer(text):
            name, address, body = match.groups()
            entry = f"    RECOMP_ENTER(0x{address}u);"
            if body.count(entry) != 1 or "RECOMP_EH_ENTER" in body:
                continue
            prefix, rest = body.split(entry)
            # Aliases jump into the same labeled instruction stream; the owner
            # falls through to its first label. No other code may be removed.
            rest = re.sub(r"\A\n(?:\n)?    goto L_" + address + r";\n", "\n",
                          rest, count=1)
            labels = re.findall(r"^L_([0-9A-Fa-f]+):", rest, re.MULTILINE)
            if address not in labels:
                continue
            groups.setdefault((prefix, rest), []).append((filename, match, address))
    replacements = {}
    declarations = []
    bodies = []
    for (prefix, rest), entries in groups.items():
        if len(entries) < 2:
            continue
        helper = f"gex_shared_{entries[0][2]}"
        declarations.append(f"void {helper}(uint32_t entry);")
        cases = "\n".join(f"    case 0x{address}u: goto L_{address};"
                          for _, _, address in entries)
        bodies.append(f"void {helper}(uint32_t entry) {{\n{prefix}"
                      f"    RECOMP_ENTER(entry);\n    switch (entry) {{\n{cases}\n"
                      f"    default: abort();\n    }}\n{rest}}}\n")
        for filename, match, address in entries:
            replacement = (f"void {match[1]}(void) {{\n"
                           f"    {helper}(0x{address}u);\n}}\n")
            replacements.setdefault(filename, []).append((match.start(), match.end(), replacement))
    result = dict(sources)
    prototypes = "\n".join(declarations) + "\n"
    for filename, edits in replacements.items():
        text = result[filename]
        for start, end, replacement in sorted(edits, reverse=True):
            text = text[:start] + replacement + text[end:]
        # uint32_t and the other generated declarations come from the normal
        # forced guest_runtime.h include, just as in the original chunks.
        result[filename] = '#include "recomp_types.h"\n' + prototypes + text
    if bodies:
        result["recomp_shared.c"] = (
            '/* Generated shared entry bodies; do not edit. */\n'
            '#define RECOMP_GENERATED_CODE\n#include "recomp_types.h"\n'
            + "\n".join(bodies))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, default=ROOT / "Gex/GEX.exe")
    parser.add_argument("--toolchain", type=Path, default=ROOT / "pcrecomp")
    args = parser.parse_args()
    exe, toolkit = args.exe.resolve(), args.toolchain.resolve()
    digest = hashlib.sha256(exe.read_bytes()).hexdigest()
    if digest != GEX_SHA256:
        parser.error(f"unsupported GEX.exe SHA-256: {digest}; expected {GEX_SHA256}")
    commit = subprocess.check_output(
        ["git", "-C", str(toolkit), "rev-parse", "HEAD"], text=True
    ).strip()
    if commit != TOOLCHAIN_COMMIT:
        parser.error(f"unsupported pcrecomp revision {commit}; expected {TOOLCHAIN_COMMIT}")
    config = ROOT / "config"
    config.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".gex-lift-", dir=ROOT) as work:
        work = Path(work)
        generated = work / "gen"
        subprocess.run(
            [sys.executable, "-u", "-m", "tools", str(exe), "--all",
             "-o", str(generated), "--split", "100",
             "--functions-json", str(work / "functions.json"),
             "--pe-json", str(work / "pe_analysis.json")],
            cwd=toolkit, check=True,
        )
        traps = 0
        for path in sorted(generated.glob("*.c")):
            text = path.read_text()
            if "ERROR lifting" in text or "/* FAILED */" in text:
                raise RuntimeError(f"pcrecomp failed to lift functions in {path.name}")
            # Discovery can include data bytes. Never execute an unsupported
            # instruction as a no-op if such an entry is actually reached.
            text, count = re.subn(
                r"; /\* UNIMPLEMENTED:.*?\*/ (/\* 0x([0-9A-Fa-f]+):.*?\*/)",
                lambda m: f'fprintf(stderr, "Unsupported x86 instruction at 0x{m[2]}\\n"); abort(); {m[1]}',
                text,
            )
            traps += count
            text = add_scheduler_points(text)
            text = replace_frame_wait(text)
            text = replace_audio_waits(text)
            text = rewrite_graphics(text)
            text = rewrite_saves(text)
            path.write_text(text)
        rewritten = "\n".join(path.read_text() for path in generated.glob("*.c"))
        if any(hook not in rewritten for hook in (TITLE_HOOK, TITLE_INIT_HOOK, CHECKPOINT_HOOK, MANUAL_HOOK)):
            raise RuntimeError("missing password save hooks")
        if FRAME_WAIT not in rewritten:
            raise RuntimeError("missing blocking frame deadline wait")
        for address, replacement in GRAPHICS_FUNCTIONS.items():
            if f"g_cur_func = 0x{address}u;\n    {replacement}();" not in rewritten:
                raise RuntimeError(f"missing renderer replacement at 0x{address}")
        if any(hook not in rewritten for hook in
               (FRAME_BEGIN, FRAME_END, PACKET_BEGIN, GRAPHICS_TITLE_HOOK, ARENA_HOOK,
                *VIEW_HOOKS.values(), *SCREEN_HOOKS.values())):
            raise RuntimeError("missing native renderer hooks")
        for address, anchor in HUD_DRAWS.items():
            scaled = int(address in (0x41B85E, 0x41BAAD, 0x41BB69))
            hook = f"gex_graphics_hud_begin(esi, {anchor}, {scaled})"
            if hook not in rewritten or f"/* 0x{address:08X}: call" not in rewritten:
                raise RuntimeError(f"missing HUD layout hook at 0x{address:08X}")
        for address in PLAYER_DRAWS:
            if (f"RECOMP_CALL(gex_graphics_draw_player_sprite); /* 0x{address:08X}: call"
                    not in rewritten):
                raise RuntimeError(f"missing player draw camera hook at 0x{address:08X}")
        sources = share_entry_bodies({
            path.name: path.read_text() for path in generated.glob("*.c")
        })
        for name, text in sources.items():
            (generated / name).write_text(text)
        functions = json.loads((work / "functions.json").read_text())
        if not any(f["address_int"] == 0x449BD0 for f in functions):
            raise RuntimeError("PE entry point is missing from dispatch")
        target = ROOT / "src/recomp/gen"
        target.mkdir(parents=True, exist_ok=True)
        for old in target.glob("recomp_*.c"):
            old.unlink()
        for path in generated.iterdir():
            path.replace(target / path.name)
        for name in ("functions.json", "pe_analysis.json"):
            (work / name).replace(config / name)
        manifest = {
            "binary_sha256": digest,
            "pcrecomp_commit": commit,
            "functions": len(functions),
            "unsupported_instruction_traps": traps,
            "entry_va": "0x00449BD0",
            "image_base": "0x00400000",
            "graphics_replacements": GRAPHICS_FUNCTIONS,
        }
        (config / "lift_manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
        print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
