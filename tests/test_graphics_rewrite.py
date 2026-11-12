from pathlib import Path
import re
import unittest

from tools.graphics_rewrite import (
    FUNCTIONS, FRAME_BEGIN, FRAME_ENTRY, FRAME_END, FRAME_RETURN,
    PACKET_BEGIN, PACKET_ENTRY, TITLE_ANCHOR, TITLE_HOOK, ARENA_RETURN, ARENA_HOOK, VIEW_HOOKS,
    VIEW_BOUNDS, TILE_EDGES, SCREEN_HOOKS, HUD_DRAWS, PLAYER_DRAWS, rewrite_graphics,
)

ROOT = Path(__file__).resolve().parents[1]


class GraphicsRewriteTests(unittest.TestCase):
    def test_title_hook_follows_cel_selection_and_is_idempotent(self):
        source = TITLE_ANCHOR + "\n"
        result = rewrite_graphics(source)
        self.assertEqual(result, TITLE_HOOK + "\n")
        self.assertEqual(rewrite_graphics(result), result)
        self.assertEqual(rewrite_graphics(source.replace("004445A8", "004445A9")),
                         source.replace("004445A8", "004445A9"))

    def test_tv_collapse_loops_are_paced_without_changing_animation_steps(self):
        source = (ROOT / "src/recomp/gen/recomp_0015.c").read_text()
        for label in ("L_0044093B:", "L_00440ACE:"):
            hook = f"{label}\n    RECOMP_CALL(sub_00405120);\n    RECOMP_FLAGS_IN();"
            self.assertEqual(source.count(hook), 1)
            self.assertEqual(rewrite_graphics(label), hook)
        self.assertIn("esi = esi + eax; /* 0x004409F6:", source)
        self.assertIn("esi = esi + eax; /* 0x00440BFC:", source)
        self.assertEqual(rewrite_graphics(source), source)

    def test_player_draws_use_render_camera_without_wrapping_updates(self):
        source = (ROOT / "src/recomp/gen/recomp_0004.c").read_text()
        self.assertEqual(source, rewrite_graphics(source))
        for address in PLAYER_DRAWS:
            original = f"    RECOMP_CALL(sub_00441150); /* 0x{address:08X}: call 0x441150 */"
            expected = original.replace("sub_00441150", "gex_graphics_draw_player_sprite")
            self.assertEqual(rewrite_graphics(original), expected)
            self.assertEqual(rewrite_graphics(expected), expected)
            self.assertIn(expected, source)
            self.assertNotIn(original, source)
        untouched = ("    RECOMP_CALL(sub_00416320); /* 0x00417E60: call 0x416320 */\n"
                     "    RECOMP_CALL(sub_00441150); /* 0x00400000: call 0x441150 */")
        self.assertEqual(rewrite_graphics(untouched), untouched)

    def test_packet_allocation_hooks(self):
        source = ("    MEM32(0x4A2AE4) = ebp; /* 0x0043DD43: mov */\n"
                  "    MEM32(0x4A2AE4) = ecx; /* 0x00441145: mov */\n")
        result = rewrite_graphics(source)
        self.assertIn("gex_graphics_packet_allocated(ebp);\n", result)
        self.assertNotIn("gex_graphics_packet_allocated(ecx);", result)
        self.assertEqual(result, rewrite_graphics(result))
        for path in (ROOT / "src/recomp/gen").glob("recomp_*.c"):
            generated = path.read_text()
            for match in re.finditer(
                r"^    MEM32\(0x4A2AE4\) = (\w+); /\* 0x([0-9A-F]+):",
                generated, flags=re.MULTILINE):
                if match[2] != "00441145":
                    self.assertTrue(generated[:match.start()].endswith(
                        f"    gex_graphics_packet_allocated({match[1]});\n"))

    def test_fixed_screen_hooks(self):
        source = "\n".join(SCREEN_HOOKS) + "\n"
        result = rewrite_graphics(source)
        self.assertEqual(result, rewrite_graphics(result))
        generated = "".join(path.read_text()
                            for path in (ROOT / "src/recomp/gen").glob("recomp_*.c"))
        for anchor, hook in SCREEN_HOOKS.items():
            ordered = f"{anchor}\n{hook}" if "RECOMP_ENTER" in anchor else f"{hook}\n{anchor}"
            self.assertIn(ordered, result)
            self.assertIn(ordered, generated)

    def test_death_transition_layout_begins_at_animation(self):
        source = (
            "void sub_00440850(void) {\n"
            "    RECOMP_ENTER(0x00440850u);\n"
            "    gex_graphics_transition_begin();\n"
            "    RECOMP_CALL(sub_0040AA60);\n"
            "    RECOMP_CALL(sub_0040A990);\n"
            "    RECOMP_CALL(sub_0041ECD0);\n"
            "    RECOMP_CALL(sub_00440930); /* 0x004408D6: call 0x440930 */\n"
            "    RECOMP_CALL(sub_00440A30);\n"
            "}\n"
        )
        result = rewrite_graphics(source)
        hook = ("    gex_graphics_transition_begin();\n"
                "    RECOMP_CALL(sub_00440930); /* 0x004408D6: call 0x440930 */")
        self.assertIn(hook, result)
        self.assertNotIn(
            "RECOMP_ENTER(0x00440850u);\n    gex_graphics_transition_begin();",
            result)
        self.assertEqual(result.count("gex_graphics_transition_begin();"), 1)
        self.assertLess(result.index("RECOMP_CALL(sub_0040A990)"),
                        result.index("gex_graphics_transition_begin();"))
        self.assertEqual(result, rewrite_graphics(result))

    def test_hud_draws_are_scoped_and_idempotent(self):
        self.assertEqual({address for address, anchor in HUD_DRAWS.items() if anchor == 2},
                         {0x41B929, 0x41B996})
        self.assertEqual({address for address, anchor in HUD_DRAWS.items() if anchor == 3},
                         {0x41B85E, 0x41B8F0})
        source = (ROOT / "src/recomp/gen/recomp_0006.c").read_text()
        self.assertEqual(source, rewrite_graphics(source))
        self.assertEqual(source.count("gex_graphics_hud_begin("), len(HUD_DRAWS))
        self.assertEqual(source.count("gex_graphics_hud_end(esi);"), len(HUD_DRAWS))
        for address, anchor in HUD_DRAWS.items():
            scaled = address in (0x41B85E, 0x41BAAD, 0x41BB69)
            original = "00441150" if scaled else "00444590"
            target = "00441150" if scaled else "004432C0"
            snippet = f"    RECOMP_CALL(sub_{original}); /* 0x{address:08X}: call */"
            result = rewrite_graphics(snippet)
            self.assertIn(f"gex_graphics_hud_begin(esi, {anchor}, {int(scaled)})", result)
            self.assertIn(f"RECOMP_CALL(sub_{target});", result)
            self.assertIn(snippet, result)
            self.assertEqual(result, rewrite_graphics(result))
        untouched = "    RECOMP_CALL(sub_00444590); /* 0x00400000: call */"
        self.assertEqual(untouched, rewrite_graphics(untouched))

    def test_function_boundaries_and_idempotence(self):
        source = "\n".join(
            f"void sub_{address}(void) {{\n    obsolete();\n}}\n"
            for address in FUNCTIONS
        )
        result = rewrite_graphics(source)
        self.assertNotIn("obsolete();", result)
        for function in FUNCTIONS.values():
            self.assertIn(f"{function}();", result)
        self.assertEqual(result, rewrite_graphics(result))

    def test_only_verified_movie_instructions_removed(self):
        source = (
            "    keep(); /* 0x0040478B: je next */\n"
            "    push(); /* 0x0040478D: push 0x13 */\n"
            "    com(); /* 0x0040479E: call [eax + 0x50] */\n"
            "    keep(); /* 0x004047A1: lea eax, [esp] */\n"
            "    push(); /* 0x004047D8: push 8 */\n"
            "    com(); /* 0x004047E9: call [eax + 0x50] */\n"
        )
        result = rewrite_graphics(source)
        self.assertEqual(result.count("keep();"), 2)
        self.assertNotIn("push();", result)
        self.assertNotIn("com();", result)

    def test_unreplaced_access_rejected(self):
        with self.assertRaises(RuntimeError):
            rewrite_graphics("eax = MEM32(0x451030);")

    def test_frame_begin_is_inserted_once(self):
        source = f"void sub_00445140(void) {{\n{FRAME_ENTRY}\n}}\n"
        result = rewrite_graphics(source)
        self.assertIn(FRAME_BEGIN, result)
        self.assertEqual(result.count("gex_graphics_begin_frame();"), 1)
        self.assertEqual(result, rewrite_graphics(result))
        generated = (ROOT / "src/recomp/gen/recomp_0017.c").read_text()
        self.assertIn(FRAME_BEGIN, generated)

    def test_generated_sources_have_no_directdraw_access(self):
        definitions = set()
        for path in (ROOT / "src/recomp/gen").glob("recomp_*.c"):
            source = path.read_text()
            self.assertNotRegex(source, r"MEM32\(0x(?:451030|4A53CC)\)")
            definitions.update(re.findall(
                r"g_cur_func = 0x([0-9A-F]+)u;\n    (gex_graphics_\w+)\(\);", source))
        self.assertTrue(set(FUNCTIONS.items()).issubset(definitions))

    def test_native_packet_and_frame_end_hooks(self):
        source = f"{PACKET_ENTRY}\n{FRAME_RETURN}\n"
        result = rewrite_graphics(source)
        self.assertIn(PACKET_BEGIN, result)
        self.assertIn(FRAME_END, result)
        self.assertEqual(result, rewrite_graphics(result))
        generated = (ROOT / "src/recomp/gen/recomp_0017.c").read_text()
        self.assertIn(PACKET_BEGIN, generated)
        self.assertIn(FRAME_END, generated)
        self.assertIn("if (gex_graphics_draw_packet", generated)
        self.assertIn("RECOMP_FLAGS_OUT(); esp += 4; return;", PACKET_BEGIN)

    def test_viewport_hooks(self):
        source = "\n".join(VIEW_HOOKS) + "\n" + ARENA_RETURN + "\n"
        result = rewrite_graphics(source)
        for hook in (*VIEW_HOOKS.values(), ARENA_HOOK):
            self.assertIn(hook, result)
        self.assertEqual(result, rewrite_graphics(result))
        generated = "\n".join(
            path.read_text() for path in (ROOT / "src/recomp/gen").glob("recomp_*.c"))
        for anchor, hook in VIEW_HOOKS.items():
            self.assertIn(f"{anchor}\n{hook}", generated)
        self.assertIn(ARENA_HOOK, generated)
        for address, (_, axis, _) in VIEW_BOUNDS.items():
            matches = re.findall(
                rf"^([^\n]*)/\* 0x{address:08X}: [^\n]*", generated, re.MULTILINE)
            self.assertTrue(matches, hex(address))
            for code in matches:
                # CMP's flag operands are emitted on the following line.
                if "/* cmp" not in code:
                    self.assertIn(f"gex_graphics_view_{axis}()", code, hex(address))

    def test_camera_barriers_are_observed_without_changing_simulation(self):
        source = ("L_0040A0AD:\n"
                  "    RECOMP_ENTER(0x0040FE80u);\n"
                  "    MEM32(0x4A2A38) = eax; /* 0x0040FFED: mov */\n")
        result = rewrite_graphics(source)
        self.assertIn("L_0040A0AD:\n    gex_graphics_camera_begin();", result)
        self.assertIn("gex_graphics_camera_barrier(MEM32(esp + 4));", result)
        self.assertIn("MEM32(0x4A2A38) = eax;", result)
        self.assertEqual(result, rewrite_graphics(result))

    def test_bounds_and_flag_operands_only_change_at_verified_addresses(self):
        source = (
            "eax = 0x01400000u; /* 0x00400000: unchanged */\n"
            "/* cmp ax, 0x140 */ /* 0x004422E7: cmp ax, 0x140 */\n"
            "_flag_b = (uint32_t)(0x140) << 16;\n"
            "eax = 0x140; /* 0x00400001: unchanged */\n"
        )
        result = rewrite_graphics(source)
        self.assertIn("_flag_b = (uint32_t)(((uint32_t)gex_graphics_view_width())) << 16;", result)
        self.assertIn("eax = 0x01400000u;", result)
        self.assertIn("eax = 0x140;", result)
        self.assertIn("/* 0x004422E7: cmp ax, 0x140 */", result)
        self.assertEqual(result, rewrite_graphics(result))
        for address, (constant, expression) in TILE_EDGES.items():
            source = (f"_flag_b = {constant};\n_cf = 0;\n"
                      f"eax = ebx + {constant}; /* 0x{address:08X}: lea */\n")
            result = rewrite_graphics(source)
            self.assertEqual(result.count(f"((uint32_t)({expression}) << 16)"), 2)
            self.assertEqual(result, rewrite_graphics(result))
