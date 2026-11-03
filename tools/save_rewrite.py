"""Verified retail title dispatch and earned-password interception points."""

import re

TITLE = "    eax = MEM32(esi + 0xB0); /* 0x0040E1CD: mov eax, dword ptr [esi + 0xb0] */"
TITLE_HOOK = """#if GEX_ENABLE_PASSWORD_SAVES
    if (eax == 3) {
        RECOMP_REGS_OUT();
        gex_save_options();
        RECOMP_REGS_IN();
        goto L_0040E1E2;
    }
    if (eax == 1) {
        RECOMP_REGS_OUT();
        int gex_choice = gex_save_choose();
        RECOMP_REGS_IN();
        if (!gex_choice) goto L_0040E1E2;
        eax = gex_choice == 2 ? 3 : 1;
    }
#endif"""
TITLE_INIT = "    RECOMP_FLAGS_OUT(); esp += 4; return; /* 0x0040DED9: ret  */"
TITLE_INIT_HOOK = """#if GEX_ENABLE_PASSWORD_SAVES
    RECOMP_REGS_OUT();
    gex_options_title_init();
    RECOMP_REGS_IN();
#endif"""
CHECKPOINT = "    RECOMP_CALL(sub_00429200); /* 0x0042AAF1: call 0x429200 */"
CHECKPOINT_HOOK = """#if GEX_ENABLE_PASSWORD_SAVES
    gex_save_checkpoint(MEM32(esp));
#endif"""
MANUAL = "    RECOMP_CALL(sub_00429940); /* 0x0040E971: call 0x429940 */"
MANUAL_HOOK = """#if GEX_ENABLE_PASSWORD_SAVES
    gex_save_checkpoint(0x4A0200);
#endif"""
TEXT_DRAWS = (0x40BD9B, 0x40BFEB, 0x40C073)
TEXT_DRAW_HOOK = """#if GEX_ENABLE_PASSWORD_SAVES
    if (gex_options_scaled_text()) { RECOMP_CALL(sub_004432C0); }
    else
#endif"""
CLOSE_PAUSE = "    RECOMP_CALL(sub_004051D0); /* 0x004052D1: call 0x4051d0 */"
CLOSE_PAUSE_HOOK = """#if GEX_ENABLE_PASSWORD_SAVES
    if (!gex_save_take_close_request())
#endif"""


def rewrite_saves(text):
    replacement = CLOSE_PAUSE_HOOK + "\n" + CLOSE_PAUSE
    if replacement not in text:
        text = text.replace(CLOSE_PAUSE, replacement)
    for address in TEXT_DRAWS:
        original = f"    RECOMP_CALL(sub_00444590); /* 0x{address:08X}: call 0x444590 */"
        replacement = TEXT_DRAW_HOOK + "\n" + original
        if replacement not in text:
            text = text.replace(original, replacement)
    text = text.replace(TITLE_INIT, TITLE_INIT_HOOK + "\n" + TITLE_INIT) \
        if TITLE_INIT_HOOK not in text else text
    for original, hook in ((TITLE, TITLE_HOOK), (CHECKPOINT, CHECKPOINT_HOOK),
                           (MANUAL, MANUAL_HOOK)):
        text = re.sub(re.escape(original) + r"\n(?!#if GEX_ENABLE_PASSWORD_SAVES)",
                      lambda _: original + "\n" + hook + "\n", text)
    return text
