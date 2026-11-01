import importlib.util
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("generate", ROOT / "tools/generate.py")
generate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(generate)


class SchedulerTests(unittest.TestCase):
    def test_audio_waits_only_block_on_failed_verified_polls(self):
        for target, address in ((0x40A69E, 0x40A6A5), (0x40A6A7, 0x40A6AE), (0x40A6BA, 0x40A6C1)):
            source = (f"    if (TEST_Z(_flag_a, _flag_b)) goto L_{target:08X}; "
                      f"/* 0x{address:08X}: je 0x{target:x} */")
            result = generate.replace_audio_waits(source)
            self.assertIn("if (TEST_Z(_flag_a, _flag_b)) {\n", result)
            self.assertIn("RECOMP_REGS_OUT(); gex_guest_wait(); RECOMP_REGS_IN();", result)
            self.assertIn(f"goto L_{target:08X};", result)
            self.assertEqual(generate.replace_audio_waits(result), result)
        untouched = "    if (TEST_Z(_flag_a, _flag_b)) goto L_0040A6A7; /* 0x00400000: je 0x40a6a7 */"
        self.assertEqual(generate.replace_audio_waits(untouched), untouched)

    def test_frame_wait(self):
        source = "L_00405188:\n    PUSH32(esp, 0); /* 0x00405188: push 0 */"
        result = generate.replace_frame_wait(source)
        self.assertEqual(result, generate.FRAME_WAIT)
        self.assertEqual(generate.replace_frame_wait(result), result)

    def test_other_yields_unchanged(self):
        source = "L_004050FC:\n    PUSH32(esp, 0); /* 0x004050FC: push 0 */"
        self.assertEqual(generate.replace_frame_wait(source), source)

    def test_backward_conditional_branch(self):
        source = "    if (CMP_NE(a, b)) goto L_00401000; /* 0x00401010: jne 0x401000 */\n"
        self.assertEqual(generate.add_scheduler_points(source),
                         "    RECOMP_BACKEDGE();\n" + source)

    def test_self_loop(self):
        source = "    goto L_00401000; /* 0x00401000: jmp 0x401000 */\n"
        self.assertIn("RECOMP_BACKEDGE();", generate.add_scheduler_points(source))

    def test_forward_branch_unchanged(self):
        source = "    goto L_00401020; /* 0x00401010: jmp 0x401020 */\n"
        self.assertEqual(generate.add_scheduler_points(source), source)

    def test_non_branch_unchanged(self):
        source = "L_00401000:\n    eax = 1; /* 0x00401000: mov eax, 1 */\n"
        self.assertEqual(generate.add_scheduler_points(source), source)


class SharedEntryTests(unittest.TestCase):
    def body(self, address, jump=""):
        return (f"void sub_{address}(void) {{\n"
                "    uint32_t _flag_k = 0, _flag_a = 0, _flag_b = 0, _cf = 0;\n"
                f"    RECOMP_ENTER(0x{address}u);\n{jump}\n"
                "L_00401000:\n    eax = 1;\n"
                "L_00401010:\n    esp += 4;\n    return;\n}\n")

    def test_owner_and_alias_share_only_the_entry(self):
        sources = {"recomp_0.c": self.body("00401000"),
                   "recomp_1.c": self.body("00401010", "\n    goto L_00401010;\n")}
        result = generate.share_entry_bodies(sources)
        self.assertIn("recomp_shared.c", result)
        helper = result["recomp_shared.c"]
        self.assertEqual(helper.count("eax = 1;"), 1)
        self.assertIn("case 0x00401000u: goto L_00401000;", helper)
        self.assertIn("case 0x00401010u: goto L_00401010;", helper)
        self.assertIn("RECOMP_ENTER(entry);", helper)
        self.assertIn("gex_shared_00401000(0x00401010u)", result["recomp_1.c"])

    def test_different_work_and_exception_frames_are_not_shared(self):
        first = self.body("00401000")
        second = self.body("00401010", "\n    goto L_00401010;\n").replace("eax = 1", "eax = 2")
        sources = {"a.c": first, "b.c": second}
        self.assertEqual(generate.share_entry_bodies(sources), sources)
        sources = {key: value.replace("eax = ", "RECOMP_EH_ENTER(); eax = ")
                   for key, value in sources.items()}
        self.assertEqual(generate.share_entry_bodies(sources), sources)

    def test_shared_entries_and_native_calls_execute(self):
        compiler = shutil.which("cc")
        if not compiler:
            self.skipTest("C compiler is unavailable")
        sources = {
            "owner.c": self.body("00401000"),
            "alias.c": self.body("00401010", "\n    goto L_00401010;\n"),
            "native.c": ("void sub_00402000(void) {\n"
                         "    g_eax += 1; g_esp += 4;\n}\n"),
            "caller.c": ("void sub_00403000(void) {\n"
                         "    uint32_t _flag_k = 0, _flag_a = 0, _flag_b = 0, _cf = 0;\n"
                         "    RECOMP_ENTER(0x00403000u);\n"
                         "    eax = 10;\n    RECOMP_CALL(sub_00402000);\n"
                         "    RECOMP_FLAGS_IN();\n"
                         "    if (ecx) { esp += 4; return; }\n"
                         "    eax += 2; esp += 4;\n}\n"),
        }
        sources = generate.share_entry_bodies(sources)
        fixture = (
            '#define RECOMP_GENERATED_CODE\n#include "recomp_types.h"\n#include <assert.h>\n'
            "uint32_t g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi, g_cur_func;\n"
            "ptrdiff_t g_mem_base;\n"
            + "\n".join(sources.values()) +
            "int main(void) {\n"
            "    uint8_t stack[512] = {0}; g_mem_base = (ptrdiff_t)(uintptr_t)stack;\n"
            "    g_eax = 77; g_esp = 128; sub_00401010();\n"
            "    assert(g_eax == 77 && g_esp == 132 && g_cur_func == 0x401010);\n"
            "    g_eax = 77; g_esp = 128; sub_00401000();\n"
            "    assert(g_eax == 1 && g_esp == 132 && g_cur_func == 0x401000);\n"
            "    g_ecx = 1; g_esp = 256; sub_00403000();\n"
            "    assert(g_eax == 11 && g_esp == 260 && g_cur_func == 0x403000);\n"
            "    g_ecx = 0; g_esp = 256; sub_00403000();\n"
            "    assert(g_eax == 13 && g_esp == 260);\n"
            "    return 0;\n}\n"
        )
        with tempfile.TemporaryDirectory(prefix="gex-registers-") as work:
            source = Path(work) / "registers.c"
            program = Path(work) / "registers"
            source.write_text(fixture)
            subprocess.run([
                compiler, "-std=c11", "-O2", "-fno-strict-aliasing", "-fwrapv",
                "-I", str(ROOT / "pcrecomp/runtime/recomp32"),
                str(source), "-o", str(program),
            ], check=True, capture_output=True, text=True)
            subprocess.run([str(program)], check=True, timeout=5)
