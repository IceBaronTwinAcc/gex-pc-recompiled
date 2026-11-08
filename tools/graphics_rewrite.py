"""Replace platform boundaries and parameterize verified world-view bounds."""

import re

FUNCTIONS = {
    "00401000": "gex_graphics_format",
    "004010A0": "gex_graphics_create",
    "004010E0": "gex_graphics_destroy",
    "00401100": "gex_graphics_resolution",
    "00406C30": "gex_graphics_flush",
    "00406FBA": "gex_graphics_forbidden",
    "00409876": "gex_graphics_forbidden",
}

# Cooperative/exclusive display ownership is not part of the new renderer.
MOVIE_OWNERSHIP = ((0x40478D, 0x40479E), (0x4047D8, 0x4047E9))
FRAME_ENTRY = "    RECOMP_ENTER(0x00445140u);"
FRAME_BEGIN = f"{FRAME_ENTRY}\n    gex_graphics_begin_frame();"
PACKET_ENTRY = "    RECOMP_ENTER(0x00444DC0u);"
PACKET_OLD = f"{PACKET_ENTRY}\n    gex_graphics_draw_packet(MEM32(esp + 4));"
PACKET_BEGIN = (f"{PACKET_ENTRY}\n"
                "    if (gex_graphics_draw_packet(MEM32(esp + 4))) {\n"
                "        RECOMP_FLAGS_OUT(); esp += 4; return;\n    }")
FRAME_RETURN = "    RECOMP_FLAGS_OUT(); esp += 4; return; /* 0x0044516D: ret  */"
FRAME_END = f"    gex_graphics_end_frame();\n{FRAME_RETURN}"
ARENA_RETURN = "    RECOMP_FLAGS_OUT(); esp += 4; return; /* 0x0044114B: ret  */"
ARENA_HOOK = f"    gex_graphics_packet_arena();\n{ARENA_RETURN}"
SCREEN_HOOKS = {
    "    RECOMP_ENTER(0x00405240u);": "    gex_graphics_cpu_access();",
    "L_0040527A:": "    if (gex_graphics_native_operation(0x405240)) goto L_0040529C;",
    "    RECOMP_ENTER(0x00405450u);": (
        "    if (gex_graphics_native_operation(0x405450)) { esp += 4; return; }\n"
        "    gex_graphics_cpu_access();"),
    "    RECOMP_ENTER(0x00409FE0u);": (
        "    if (gex_graphics_native_operation(0x409FE0)) { esp += 4; return; }\n"
        "    gex_graphics_cpu_access();"),
    "    RECOMP_ENTER(0x00444C90u);": (
        "    gex_graphics_native_operation(0x444C90);\n    gex_graphics_cpu_access();"),
    "    RECOMP_ENTER(0x00444D10u);": (
        "    if (gex_graphics_native_operation(0x444D10)) { esp += 4; return; }\n"
        "    gex_graphics_cpu_access();"),
    "    RECOMP_ENTER(0x004451E0u);": (
        "    if (gex_graphics_native_operation(0x4451E0)) { esp += 4; return; }\n"
        "    gex_graphics_atlas_access(MEM32(esp + 4));"),
    "    RECOMP_CALL(sub_00440930); /* 0x004408D6: call 0x440930 */":
        "    gex_graphics_transition_begin();",
    "    RECOMP_FLAGS_OUT(); esp += 4; return; /* 0x004408BD: ret  */":
        "    gex_graphics_transition_end();",
    "    RECOMP_FLAGS_OUT(); esp += 4; return; /* 0x00440921: ret  */":
        "    gex_graphics_transition_end();",
    "    RECOMP_ENTER(0x0043F080u);": "    gex_graphics_backdrop_begin();",
    "    RECOMP_FLAGS_OUT(); esp += 4; return; /* 0x0043F287: ret  */":
        "    gex_graphics_backdrop_end();",
    "    RECOMP_ENTER(0x0040E570u);": "    gex_graphics_backdrop_begin();",
    "    RECOMP_FLAGS_OUT(); esp += 4; return; /* 0x0040E69B: ret  */":
        "    gex_graphics_backdrop_end();",
    "    RECOMP_ENTER(0x004053A0u);": (
        "    if (gex_graphics_native_operation(0x4053A0)) { esp += 4; return; }\n"
        "    gex_graphics_pause_begin();"),
    "    RECOMP_FLAGS_OUT(); esp += 4; return; /* 0x00405442: ret  */":
        "    gex_graphics_pause_end();",
}
VIEW_HOOKS = {
    "L_0040A882:": "    gex_graphics_hold_black(0);",
    "L_0044093B:": "    RECOMP_CALL(sub_00405120);\n    RECOMP_FLAGS_IN();",
    "L_00440ACE:": "    RECOMP_CALL(sub_00405120);\n    RECOMP_FLAGS_IN();",
    "L_0040A0AD:": "    gex_graphics_camera_begin();",
    "    RECOMP_ENTER(0x0040FE80u);":
        "    gex_graphics_camera_barrier(MEM32(esp + 4));",
    "L_0040A36C:": "    gex_graphics_scene_begin();",
    "L_0040A4C0:": "    gex_graphics_scene_end();",
    "    RECOMP_ENTER(0x0043FB40u);": "    if (gex_graphics_draw_tiles()) return;",
}

# Original instruction addresses, not global numerical substitutions: atlas
# layout, simulation activation, and UI coordinates must keep their constants.
VIEW_BOUNDS = {
    0x43FD37: ("1400000", "width", 16),
    0x43FD58: ("f00000", "height", 16),
    0x440DA5: ("1400000", "width", 16),
    0x440DBE: ("1400000", "width", 16),
    0x440E0D: ("f00000", "height", 16),
    0x440E1F: ("f00000", "height", 16),
    0x4422E7: ("140", "width", 0),
    0x4422FA: ("f0", "height", 0),
    0x44371D: ("140", "width", 0),
    0x443738: ("f0", "height", 0),
    0x44471D: ("1400000", "width", 16),
    0x444735: ("1400000", "width", 16),
    0x444786: ("f00000", "height", 16),
    0x444797: ("f00000", "height", 16),
    0x41FF2D: ("f00000", "height", 16),
    0x41FF34: ("f00000", "height", 16),
    0x42016E: ("1400000", "width", 16),
}
TILE_EDGES = {
    0x43FDBD: ("0x400000", "gex_graphics_view_width() - 256"),
    0x43FDD3: ("0x00400000u", "gex_graphics_view_width() - 256"),
    0x43FE1E: ("(-0x100000)", "gex_graphics_view_height() - 256"),
    0x43FE34: ("0x00100000u", "256 - gex_graphics_view_height()"),
}

# Verified draws in the gameplay HUD callback, including animated inventory
# and bonus counters. Never transform world objects or menu CELs by position.
HUD_DRAWS = {
    0x41B7F7: 0, 0x41B85E: 3, 0x41B8F0: 3, 0x41B929: 2, 0x41B996: 2,
    0x41BA3E: 1, 0x41BAAD: 1, 0x41BB69: 1,
    0x41BC07: 1, 0x41BC40: 0, 0x41BCC4: 0,
}

# These world-space player/effect draws can run after scene submission, notably
# RezPortal/RezInFromPortal. Scope the camera to each draw, not the state update.
PLAYER_DRAWS = (
    0x41668E, 0x416787, 0x41680C, 0x416964, 0x416BEB,
    0x416DFB, 0x416F6A, 0x4170AF, 0x4171F1, 0x41723F,
)


def rewrite_graphics(text):
    text = text.replace(
        "    RECOMP_ENTER(0x00440850u);\n    gex_graphics_transition_begin();",
        "    RECOMP_ENTER(0x00440850u);")
    for address in PLAYER_DRAWS:
        text = re.sub(
            rf"RECOMP_CALL\((?:sub_00441150|gex_graphics_draw_player_sprite)\);"
            rf"( /\* 0x{address:08X}: [^\n]*\*/)",
            r"RECOMP_CALL(gex_graphics_draw_player_sprite);\1", text)
    text = re.sub(r"^    gex_graphics_packet_allocated\([^\n]+\);\n", "", text,
                  flags=re.MULTILINE)
    def allocation(match):
        if match[3] == "00441145":  # Arena initialization, not packet allocation.
            return match[0]
        return f"    gex_graphics_packet_allocated({match[2]});\n{match[0]}"
    text = re.sub(
        r"^(    MEM32\(0x4A2AE4\) = (\w+); /\* 0x([0-9A-F]+):[^\n]*)",
        allocation, text, flags=re.MULTILINE)
    for address, anchor in HUD_DRAWS.items():
        pattern = rf"(    RECOMP_CALL\(sub_\w+\); /\* 0x{address:08X}: [^\n]*\*/)"
        # Normalize the wrapper first so regeneration is idempotent.
        text = re.sub(
            r"    if \(gex_graphics_hud_begin\(esi, [0-3], [01]\)\) \{\n"
            r"        RECOMP_CALL\(sub_0044(?:32C0|1150)\);\n    \} else \{\n"
            rf"({pattern})\n    \}}\n    gex_graphics_hud_end\(esi\);",
            r"\1", text)
        def hud_draw(match):
            original = match[0]
            scaled = int("sub_00441150" in original)
            # Rotated draws already honor scale; static HUD draws bypass it.
            target = "00441150" if scaled else "004432C0"
            return (f"    if (gex_graphics_hud_begin(esi, {anchor}, {scaled})) {{\n"
                    f"        RECOMP_CALL(sub_{target});\n    }} else {{\n"
                    f"{original}\n    }}\n    gex_graphics_hud_end(esi);")
        # Both branches use the same normalized wrapper, including rotated CELs.
        text = re.sub(pattern, hud_draw, text)
    text = text.replace(FRAME_BEGIN, FRAME_ENTRY).replace(FRAME_ENTRY, FRAME_BEGIN)
    text = text.replace(PACKET_OLD, PACKET_ENTRY)
    text = text.replace(PACKET_BEGIN, PACKET_ENTRY).replace(PACKET_ENTRY, PACKET_BEGIN)
    text = text.replace(FRAME_END, FRAME_RETURN).replace(FRAME_RETURN, FRAME_END)
    for anchor, hook in VIEW_HOOKS.items():
        text = text.replace(f"{anchor}\n{hook}", anchor)
        text = text.replace(anchor, f"{anchor}\n{hook}")
    # This return has no label in the mechanical lift.
    text = text.replace(ARENA_HOOK, ARENA_RETURN).replace(ARENA_RETURN, ARENA_HOOK)
    for anchor, hook in SCREEN_HOOKS.items():
        if "RECOMP_ENTER" in anchor:
            text = text.replace(f"{anchor}\n{hook}", anchor)
            text = text.replace(anchor, f"{anchor}\n{hook}")
        else:
            text = text.replace(f"{hook}\n{anchor}", anchor)
            text = text.replace(anchor, f"{hook}\n{anchor}")
    for address, replacement in FUNCTIONS.items():
        pattern = rf"void sub_{address}\(void\) \{{.*?^\}}"
        text = re.sub(
            pattern,
            f"void sub_{address}(void) {{\n"
            f"    g_cur_func = 0x{address}u;\n    {replacement}();\n}}",
            text, flags=re.MULTILINE | re.DOTALL,
        )
    lines = []
    bounds = edge = None
    for line in text.splitlines(keepends=True):
        instruction = re.search(r"/\* 0x([0-9A-Fa-f]+):", line)
        if instruction:
            address = int(instruction[1], 16)
            bounds = VIEW_BOUNDS.get(address)
            edge = TILE_EDGES.get(address)
            if edge:
                constant, expression = edge
                for previous in range(len(lines) - 1, -1, -1):
                    if "/* 0x" in lines[previous]:
                        break
                    lines[previous] = lines[previous].replace(
                        constant, f"((uint32_t)({expression}) << 16)")
            if any(start <= address <= end for start, end in MOVIE_OWNERSHIP):
                continue
        if bounds:
            constant, axis, shift = bounds
            # Keep disassembly comments intact for auditing against the binary.
            code, separator, comment = line.partition("/*")
            expression = f"((uint32_t)gex_graphics_view_{axis}()"
            expression += f" << {shift})" if shift else ")"
            code = re.sub(rf"\b0x0*{constant}(?:u)?\b", expression, code, flags=re.I)
            line = code + separator + comment
        if edge:
            constant, expression = edge
            code, separator, comment = line.partition("/*")
            code = code.replace(constant, f"((uint32_t)({expression}) << 16)")
            line = code + separator + comment
        lines.append(line)
    text = "".join(lines)
    if "MEM32(0x451030)" in text or "MEM32(0x4A53CC)" in text:
        raise RuntimeError("unreplaced DirectDraw access in generated code")
    return text
