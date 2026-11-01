from pathlib import Path
import os
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from tools.generate import TOOLCHAIN_COMMIT


class RuntimePatchTests(unittest.TestCase):
    def test_patch_replay_idempotence_and_conflict(self):
        sources = {}
        names = set()
        for patch in (ROOT / "patches").glob("*.patch"):
            for line in patch.read_text().splitlines():
                if line.startswith("+++ b/runtime/"):
                    names.add(line.removeprefix("+++ b/"))
        for name in sorted(names):
            sources[name] = subprocess.check_output([
                "git", "-C", os.environ.get("GEX_PCRECOMP_DIR", str(ROOT / "pcrecomp")), "show",
                f"{TOOLCHAIN_COMMIT}:{name}",
            ])
        source = sources["runtime/win32hle/user32.c"]
        with tempfile.TemporaryDirectory(prefix="gex-runtime-patch-") as work:
            toolkit = Path(work)
            subprocess.run(["git", "-C", work, "init", "--quiet"], check=True)
            path = toolkit / "runtime/win32hle/user32.c"
            path.parent.mkdir(parents=True)
            for name, content in sources.items():
                target = toolkit / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(content)
            command = [sys.executable, str(ROOT / "tools/prepare_runtime.py"),
                       "--toolchain", work]
            subprocess.run(command, check=True, capture_output=True)
            patched = path.read_bytes()
            self.assertIn(b"g_pressed[vk] = 0;", patched)
            thread_state = (path.parent / "host_lite.c").read_bytes()
            self.assertIn(b"r->flag_k=g_flag_k", thread_state)
            self.assertIn(b"memcpy(r->xmm, g_xmm", thread_state)
            self.assertIn(b"gex_runtime_machine_lock()", thread_state)
            self.assertNotIn(b"#include <pthread.h>", thread_state)
            timers = (path.parent / "winmm.c").read_bytes()
            self.assertIn(b"gex_runtime_start_thread", timers)
            self.assertNotIn(b"pthread_", timers)
            registers = (toolkit / "runtime/recomp32/recomp_types.h").read_bytes()
            self.assertIn(b"#elif defined(__MINGW32__)", registers)
            self.assertIn(b"__attribute__((selectany))", registers)
            self.assertIn(b"g_flag_k = 0, g_flag_a = 0", registers)
            self.assertIn(b"RECOMP_UNALIGNED_MEMORY", registers)
            self.assertIn(b"HLE_GUEST_STACK_SIZE", thread_state)
            self.assertIn(b"HLE_MAX_WINDOWS", patched)
            applied_sources = {name: (toolkit / name).read_bytes() for name in sources}
            subprocess.run(command, check=True, capture_output=True)
            for name, content in applied_sources.items():
                self.assertEqual((toolkit / name).read_bytes(), content)
            conflict = source.replace(b"static uint8_t g_keys[256];",
                                      b"static uint8_t incompatible_keys[256];")
            self.assertNotEqual(conflict, source)
            path.write_bytes(conflict)
            result = subprocess.run(command, capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(path.read_bytes(), conflict)
