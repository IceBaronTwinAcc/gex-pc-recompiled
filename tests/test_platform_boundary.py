from pathlib import Path
import os
import json
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
RUNTIME_SOURCES = re.findall(
    r'\$\{hle\}/([^"]+)', ROOT.joinpath("cmake/runtime-core.cmake").read_text())


class PlatformBoundaryTests(unittest.TestCase):
    def test_wasm_runtime_sources_keep_browser_code_in_wasm_backend(self):
        forbidden = re.compile(
            r"__EMSCRIPTEN__|<emscripten(?:/|\.h)|\b(?:emscripten_\w+|"
            r"EM_ASM\w*|EM_JS|EM_ASYNC_JS|MAIN_THREAD_\w+)\b"
        )
        sources = list(ROOT.joinpath("src").glob("*.[ch]"))
        sources += list(ROOT.joinpath("src/platform").rglob("*.[ch]"))
        for source in sources:
            if source.is_relative_to(ROOT / "src/platform/wasm"):
                continue
            with self.subTest(path=source.relative_to(ROOT)):
                self.assertIsNone(forbidden.search(source.read_text()))
        for source in ("CMakeLists.txt", "cmake/bgfx.cmake"):
            with self.subTest(path=source):
                text = ROOT.joinpath(source).read_text()
                for token in ("EMSCRIPTEN", "GEX_PLATFORM_WEB", "GEX_WASM_", "/web/"):
                    self.assertNotIn(token, text)

    def test_shared_sources_have_no_platform_dependencies(self):
        forbidden = re.compile(
            r"#\s*include\s*[<\"](?:SDL|AL/|al\.h|alc\.h|alext\.h|dsound\.h|windows\.h|bcrypt\.h|libav|libsw|"
            r"kos\.h|dc/|arch/|pthread\.h|unistd\.h|sys/|signal\.h|dirent\.h|fcntl\.h|"
            r"strings\.h|netinet/|arpa/|netdb\.h)|"
            r"\b(?:SDL_\w+|pthread_\w+|mmap|munmap|chdir|getcwd|"
            r"clock_gettime|strdup|strn?casecmp|__sync_\w+|"
            r"GetModuleHandleA|BCrypt\w+)\s*\(|"
            r"^\s*#\s*(?:if|ifdef|ifndef).*?(?:_WIN32|__linux__|__APPLE__|__EMSCRIPTEN__|__DREAMCAST__|_arch_dreamcast)",
            re.MULTILINE,
        )
        for path in sorted(ROOT.joinpath("src").glob("*.[ch]")):
            with self.subTest(path=path.name):
                self.assertIsNone(forbidden.search(path.read_text()))
        for path in ROOT.joinpath("src/platform").glob("*.h"):
            with self.subTest(path=path.name):
                self.assertIsNone(forbidden.search(path.read_text()))
        for path in ROOT.joinpath("src/platform").glob("*.c"):
            with self.subTest(path=path.name):
                self.assertIsNone(forbidden.search(path.read_text()))
        for name in RUNTIME_SOURCES:
            path = Path(os.environ.get("GEX_PCRECOMP_DIR", ROOT / "pcrecomp")).joinpath(
                "runtime/win32hle", name)
            with self.subTest(path=name):
                self.assertIsNone(forbidden.search(path.read_text()))

    def test_runtime_core_compiles_without_host_sdk(self):
        toolchain = Path(os.environ.get("GEX_PCRECOMP_DIR", ROOT / "pcrecomp"))
        with tempfile.TemporaryDirectory(prefix="gex-core-compile-") as work:
            poison = Path(work)
            for name in ("SDL.h", "SDL2/SDL.h", "SDL2/SDL_ttf.h", "pthread.h",
                         "unistd.h", "dirent.h", "windows.h", "sys/mman.h",
                         "sys/socket.h", "arpa/inet.h", "netdb.h", "kos.h",
                         "dc/video.h", "dc/maple.h", "arch/mmu.h"):
                path = poison / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("#error Host SDK included by portable runtime\n")
            sources = [toolchain / "runtime/win32hle" / name for name in RUNTIME_SOURCES]
            sources.append(ROOT / "src/platform/runtime_core.c")
            sources.append(ROOT / "src/platform/audio.c")
            result = subprocess.run(
                [os.environ.get("GEX_TEST_CC", "cc"), "-std=c11",
                 "-Werror=implicit-function-declaration", "-fsyntax-only",
                 "-DGEX_NO_DIRECTDRAW", "-DGEX_NO_NETWORK", "-DGEX_NO_MILES",
                 "-I", str(poison), "-I", str(ROOT / "src"),
                 "-I", str(toolchain / "runtime/win32hle"),
                 "-I", str(toolchain / "runtime/recomp32"),
                 *map(str, sources)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_gex_does_not_require_network(self):
        imports = json.loads(ROOT.joinpath("config/pe_analysis.json").read_text())["imports"]
        self.assertFalse(any(item["dll"].lower() in
                             ("wsock32.dll", "ws2_32.dll", "wininet.dll")
                             for item in imports))
        self.assertNotIn("wsock32.c", RUNTIME_SOURCES)
        self.assertIn("GEX_NO_NETWORK", ROOT.joinpath("CMakeLists.txt").read_text())
        self.assertFalse(ROOT.joinpath("src/platform/network.h").exists())

    def test_host_discovery_is_owned_by_manifest(self):
        text = ROOT.joinpath("CMakeLists.txt").read_text()
        self.assertNotIn("pkg_check_modules", text)
        self.assertNotIn("find_package(Threads", text)
        self.assertNotIn("runtime/win32hle/*.c", text)
        for service in ("MACHINE", "MEMORY", "SYSTEM", "STORAGE", "DISPLAY", "AUDIO", "COMPAT"):
            self.assertIn(f"GEX_RUNTIME_{service}_SOURCES",
                          ROOT.joinpath("cmake/runtime-core.cmake").read_text())

    def test_manifest_validation_before_host_dependency_discovery(self):
        with tempfile.TemporaryDirectory(prefix="gex-platform-config-") as work:
            manifest = Path(work) / "incomplete.cmake"
            manifest.write_text("set(GEX_PLATFORM_RUNTIME hle)\n")
            result = subprocess.run(
                ["cmake", "-S", str(ROOT), "-B", str(Path(work) / "build"),
                 "-DBUILD_TESTING=OFF", f"-DGEX_PLATFORM_FILE={manifest}",
                 f"-DPCRECOMP_DIR={os.environ.get('GEX_PCRECOMP_DIR', ROOT / 'pcrecomp')}"],
                capture_output=True, text=True, timeout=30,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("Platform manifest must define GEX_HOST_SOURCES", result.stderr)
            self.assertNotIn("Checking for module 'sdl2'", result.stdout)
