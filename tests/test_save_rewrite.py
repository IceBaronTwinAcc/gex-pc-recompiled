import unittest
from tools.save_rewrite import (
    TITLE, TITLE_HOOK, TITLE_INIT, TITLE_INIT_HOOK,
    CHECKPOINT, CHECKPOINT_HOOK, MANUAL, MANUAL_HOOK, rewrite_saves,
    TEXT_DRAWS, TEXT_DRAW_HOOK,
    CLOSE_PAUSE, CLOSE_PAUSE_HOOK,
)


class SaveHookTests(unittest.TestCase):
    def test_modal_close_skips_only_the_quit_pause_prelude(self):
        ordinary_pause = "    RECOMP_CALL(sub_004051D0); /* 0x00405311: call 0x4051d0 */"
        source = "\n".join((CLOSE_PAUSE, ordinary_pause))
        result = rewrite_saves(source)
        self.assertIn(CLOSE_PAUSE_HOOK + "\n" + CLOSE_PAUSE, result)
        self.assertEqual(result.count(CLOSE_PAUSE_HOOK), 1)
        self.assertTrue(result.endswith(ordinary_pause))
        self.assertEqual(rewrite_saves(result), result)

    def test_scaled_font_keeps_retail_text_and_selection_logic(self):
        source = "\n".join(
            f"    RECOMP_CALL(sub_00444590); /* 0x{address:08X}: call 0x444590 */"
            for address in (*TEXT_DRAWS, 0x40C43E)
        )
        result = rewrite_saves(source)
        self.assertEqual(result.count(TEXT_DRAW_HOOK), len(TEXT_DRAWS))
        self.assertTrue(result.endswith(
            "    RECOMP_CALL(sub_00444590); /* 0x0040C43E: call 0x444590 */"))
        self.assertEqual(rewrite_saves(result), result)

    def test_only_verified_boundaries_are_intercepted(self):
        source = "\n".join((TITLE_INIT, TITLE, CHECKPOINT, MANUAL, "    RECOMP_CALL(sub_00429200);"))
        result = rewrite_saves(source + "\n")
        for hook in (TITLE_INIT_HOOK, TITLE_HOOK, CHECKPOINT_HOOK, MANUAL_HOOK):
            self.assertEqual(result.count(hook), 1)
        self.assertIn("    RECOMP_CALL(sub_00429200);\n", result)
        self.assertIn("RECOMP_REGS_OUT();", result)
        self.assertIn("if (!gex_choice) goto L_0040E1E2;", result)
        self.assertIn("#if GEX_ENABLE_PASSWORD_SAVES", result)
        self.assertEqual(rewrite_saves(result), result)

    def test_options_do_not_replace_start_decks_or_manual_entry(self):
        self.assertIn("if (eax == 3)", TITLE_HOOK)
        self.assertIn("gex_save_options();", TITLE_HOOK)
        self.assertIn("if (eax == 1)", TITLE_HOOK)
        self.assertIn("gex_save_choose();", TITLE_HOOK)
        self.assertIn("eax = gex_choice == 2 ? 3 : 1;", TITLE_HOOK)
        self.assertNotIn("eax == 2", TITLE_HOOK)
