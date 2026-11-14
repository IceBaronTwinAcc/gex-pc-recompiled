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
FRAME_BEGIN_OLD = f"{FRAME_ENTRY}\n    gex_graphics_begin_frame();"
FRAME_BEGIN = f"{FRAME_BEGIN_OLD}\n    gex_graphics_list_begin(MEM32(esp + 4));"
PACKET_ENTRY = "    RECOMP_ENTER(0x00444DC0u);"
PACKET_OLD = f"{PACKET_ENTRY}\n    gex_graphics_draw_packet(MEM32(esp + 4));"
PACKET_BEGIN = (f"{PACKET_ENTRY}\n"
                "    if (gex_graphics_draw_packet(MEM32(esp + 4))) {\n"
                "        RECOMP_FLAGS_OUT(); esp += 4; return;\n    }")
TITLE_ANCHOR = "    if (TEST_Z(_flag_a, _flag_b)) goto L_004447EF; /* 0x004445A8: je 0x4447ef */"
TITLE_HOOK = (f"{TITLE_ANCHOR}\n"
              "    if (gex_graphics_title_try(MEM32(esp + 0x3C), ecx)) goto L_004447EF;")
BACKDROP_ANCHOR = "    MEM32(0x4A2B14) = ecx; /* 0x0043F14F: mov dword ptr [0x4a2b14], ecx */"
BACKDROP_HOOK_OLD = (f"{BACKDROP_ANCHOR}\n"
                     "    if (gex_graphics_backdrop_try()) { edx = 0x100; goto L_0043F221; }")
BACKDROP_HOOK = (f"{BACKDROP_ANCHOR}\n"
                 "    if (gex_graphics_backdrop_try(ebp, MEM16(esp + 0x12))) "
                 "{ edx = 0x100; goto L_0043F221; }")
MAP_CHUNK_CALL = "    RECOMP_CALL(sub_0043FCE0); /* 0x0043FCA7: call 0x43fce0 */"
MAP_CHUNK_HOOK_OLD = (
    "    if (!gex_graphics_map_chunk_try(MEM32(esp), MEM32(esp + 4),\n"
    "                                    (int32_t)MEM32(esp + 8), (int32_t)MEM32(esp + 12)))\n"
    f"    {MAP_CHUNK_CALL}")
MAP_CHUNK_HOOK = "    RECOMP_REGS_OUT();\n" + MAP_CHUNK_HOOK_OLD
MAP_FLIPPED_ANCHOR = ("    esp = esp + 4; /* 0x0043DCEE: add esp, 4 */\n"
                      "    _flag_k = FK_ADD;")
MAP_FLIPPED_HOOK_OLD = (
    f"{MAP_FLIPPED_ANCHOR}\n"
    "    uint32_t map_previous_tail;\n"
    "    if (gex_graphics_map_flipped_cel_try(ebx - 0x14, MEM8(esp + 0x2C) ?\n"
    "            MEM32(esp + 0x20) : MEM32(esp + 0x1C), MEM32(esp + 0x4C),\n"
    "            MEM32(esp + 0x48), MEM32(esp + 0x28), MEM16(esp + 0x3C),\n"
    "            MEM16(esp + 0x40), MEM16(esp + 0x50), MEM16(esp + 0x54),\n"
    "            MEM16(esp + 0x16), &map_previous_tail)) {\n"
    "        eax = map_previous_tail; edx = MEM32(0x4A2B14); ecx = MEM16(0x4A2B20);\n"
    "        goto L_0043E2B2;\n"
    "    }\n"
    "    gex_gpu_cel_result map_result;\n"
    "    if (gex_graphics_map_unflipped_cel_try(ebx - 0x14, MEM8(esp + 0x2C) ?\n"
    "            MEM32(esp + 0x20) : MEM32(esp + 0x1C),\n"
    "            MEM32(esp + 0x4C), MEM32(esp + 0x48), MEM32(esp + 0x28),\n"
    "            MEM16(esp + 0x3C), MEM16(esp + 0x40), MEM16(esp + 0x50),\n"
    "            MEM16(esp + 0x54), MEM16(esp + 0x16), &map_result)) {\n"
    "        eax = map_result.eax; ecx = map_result.ecx; edx = map_result.edx;\n"
    "        goto L_0043DEDA;\n"
    "    }")
MAP_FLIPPED_HOOK = MAP_FLIPPED_HOOK_OLD.replace(
    MAP_FLIPPED_ANCHOR,
    MAP_FLIPPED_ANCHOR + "\n"
    "    if (gex_graphics_map_cel_direct_try(ebx - 0x14, MEM8(esp + 0x2C) ?\n"
    "            MEM32(esp + 0x20) : MEM32(esp + 0x1C), MEM32(esp + 0x4C),\n"
    "            MEM32(esp + 0x48), MEM32(esp + 0x28), MEM16(esp + 0x3C),\n"
    "            MEM16(esp + 0x40), MEM16(esp + 0x50), MEM16(esp + 0x54),\n"
    "            MEM16(esp + 0x16))) {\n"
    "        if (MEM32(esp + 0x4C) & 0xc0000000u) goto L_0043E2B2;\n"
    "        goto L_0043DEDA;\n"
    "    }")
MAP_SCALED_ANCHOR = "L_00443895:"
MAP_SCALED_HOOK = (
    f"{MAP_SCALED_ANCHOR}\n"
    "    {\n"
    "        gex_gpu_scaled_cel_draw map_scaled = {\n"
    "            MEM32(esp + 0x18), ebx, MEM8(MEM32(esp + 0x18) + 0x10) & 0x40 ?\n"
    "                MEM32(esp + 0x30) : MEM32(esp + 0x24),\n"
    "            MEM32(esp + 0x54), MEM32(esp + 0x60),\n"
    "            (int32_t)MEM32(esp + 0x34), (int32_t)MEM32(esp + 0x3C),\n"
    "            (int32_t)MEM32(esp + 0x38), (int32_t)MEM32(esp + 0x4C),\n"
    "            MEM16(esp + 0x1C), MEM16(esp + 0x20), MEM16(esp + 0x14),\n"
    "            MEM16(esp + 0x16), MEM16(esp + 0x12)};\n"
    "        if (gex_graphics_map_scaled_cel_try(&map_scaled, &edx)) goto L_00443AB4;\n"
    "    }")
FRAME_RETURN = "    RECOMP_FLAGS_OUT(); esp += 4; return; /* 0x0044516D: ret  */"
FRAME_END = f"    gex_graphics_end_frame();\n{FRAME_RETURN}"
ARENA_RETURN = "    RECOMP_FLAGS_OUT(); esp += 4; return; /* 0x0044114B: ret  */"
ARENA_HOOK = f"    gex_graphics_packet_arena();\n{ARENA_RETURN}"
SCREEN_HOOKS = {
    "    RECOMP_ENTER(0x00440510u);": "    gex_graphics_tile_lists_merge(0);",
    "    RECOMP_ENTER(0x00440560u);": "    gex_graphics_tile_lists_merge(1);",
    "    MEM32(0x464E48) = ecx; /* 0x0043DB2E: mov dword ptr [0x464e48], ecx */":
        "    gex_graphics_palette_reset();",
    "    esi = MEM32(0x464E48); /* 0x0043DB7D: mov esi, dword ptr [0x464e48] */":
        "    gex_graphics_palette_flush(0x464E48);",
    "    esi = MEM32(esi); /* 0x0043DB97: mov esi, dword ptr [esi] */":
        "    gex_graphics_palette_flush(esi);",
    "    MEM32(0x464E48) = 0; /* 0x0043DB9D: mov dword ptr [0x464e48], 0 */":
        "    gex_graphics_palette_reset();",
    "    eax = MEM32(0x4A2AE4); /* 0x0043EDD2: mov eax, dword ptr [0x4a2ae4] */": (
        "    {\n"
        "        uint32_t palette = MEM32(ebp + (-0xC));\n"
        "        unsigned width = MEM8(palette) ? 256 : 16;\n"
        "        if (gex_graphics_palette_try(MEM32(0x4A2B1C), MEM32(ecx + 0xC),\n"
        "                                     palette + 4, width)) {\n"
        "            _flag_a = width == 16 ? 0xFFFFFF10u : 0;\n"
        "            _flag_b = 0x100u; _cf = width == 16; _flag_k = FK_ADD;\n"
        "            goto L_0043EE3E;\n"
        "        }\n"
        "    }"),
    "    MEM32(0x4A2B14) = ebx; /* 0x0043DB1B: mov dword ptr [0x4a2b14], ebx */":
        "    gex_graphics_title_lists_reset(MEM32(0x4A2B18), ebx);",
    "    MEM32(0x4A2B14) = edx; /* 0x0043DC5C: mov dword ptr [0x4a2b14], edx */":
        "    gex_graphics_title_lists_reset(MEM32(0x4A2B18), edx);",
    "    MEM32(0x4A2B14) = ebx; /* 0x0043E569: mov dword ptr [0x4a2b14], ebx */":
        "    gex_graphics_title_lists_reset(MEM32(0x4A2B18), ebx);",
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
    "    RECOMP_ENTER(0x0043FB40u);": (
        "    if (gex_graphics_draw_tiles()) return;\n"
        "    gex_graphics_tile_lists_reset();"),
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
        "    RECOMP_ENTER(0x0043FB40u);\n    if (gex_graphics_draw_tiles()) return;\n"
        "    gex_graphics_tile_lists_reset();",
        "    RECOMP_ENTER(0x0043FB40u);")
    text = text.replace(
        "    RECOMP_ENTER(0x0043FB40u);\n    if (gex_graphics_draw_tiles()) return;",
        "    RECOMP_ENTER(0x0043FB40u);")
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
    text = text.replace(FRAME_BEGIN, FRAME_ENTRY).replace(FRAME_BEGIN_OLD, FRAME_ENTRY)
    text = text.replace(FRAME_ENTRY, FRAME_BEGIN)
    text = text.replace(PACKET_OLD, PACKET_ENTRY)
    text = text.replace(PACKET_BEGIN, PACKET_ENTRY).replace(PACKET_ENTRY, PACKET_BEGIN)
    text = text.replace(TITLE_HOOK, TITLE_ANCHOR).replace(TITLE_ANCHOR, TITLE_HOOK)
    text = text.replace(BACKDROP_HOOK, BACKDROP_ANCHOR).replace(
        BACKDROP_HOOK_OLD, BACKDROP_ANCHOR).replace(BACKDROP_ANCHOR, BACKDROP_HOOK)
    text = text.replace(MAP_CHUNK_HOOK, MAP_CHUNK_CALL).replace(MAP_CHUNK_HOOK_OLD, MAP_CHUNK_CALL)
    text = text.replace(MAP_CHUNK_CALL, MAP_CHUNK_HOOK)
    text = text.replace(MAP_FLIPPED_HOOK, MAP_FLIPPED_ANCHOR).replace(
        MAP_FLIPPED_HOOK_OLD, MAP_FLIPPED_ANCHOR).replace(
        MAP_FLIPPED_ANCHOR, MAP_FLIPPED_HOOK)
    text = text.replace(MAP_SCALED_HOOK, MAP_SCALED_ANCHOR).replace(
        MAP_SCALED_ANCHOR, MAP_SCALED_HOOK)
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
