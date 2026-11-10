import importlib.util
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("dc_runner", ROOT / "tools/run-dreamcast-test.py")
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class DreamcastRunnerTests(unittest.TestCase):
    def run_serial(self, serial, code):
        read, write = os.pipe()
        os.write(write, serial)
        os.close(write)
        process = Mock(stdout=os.fdopen(read, "rb"))
        process.poll.return_value = None
        with tempfile.TemporaryDirectory(prefix="gex-dc-runner-") as work:
            log = Path(work) / "serial.log"
            args = ["runner", "flycast", "game.cdi", str(log), "1", "no"]
            with patch.object(runner.sys, "argv", args), \
                    patch.dict(os.environ, {}, clear=True), \
                    patch.object(runner.subprocess, "Popen", return_value=process) as launch:
                self.assertEqual(runner.main(), code)
            self.assertEqual(log.read_bytes(), serial)
            self.assertIn("-headless", launch.call_args.args[0])
            self.assertIn("config:Dynarec.Enabled=yes", launch.call_args.args[0][3])
            self.assertIn("config:rend.EmulateFramebuffer=no", launch.call_args.args[0][3])
            self.assertIn("config:rend.RenderToTextureBuffer=no", launch.call_args.args[0][3])
        process.terminate.assert_called_once()
        process.wait.assert_called_once_with(timeout=5)
        self.assertTrue(process.stdout.closed)

    def test_guest_exit_stops_before_emulator_reboots(self):
        self.run_serial(b"presented first framebuffer\r\narch: exit return code 0\r\n", 0)

    def test_guest_failure_is_not_reported_as_success(self):
        self.run_serial(b"arch: exit return code 3\n", 3)

    def test_first_frame_does_not_hide_memory_failure(self):
        self.run_serial(b"presented first framebuffer\nOut of memory\n", 1)

    def test_sh4_exception_fails(self):
        self.run_serial(b"Unhandled exception: PC 8c34645a\n", 1)

    def test_invalid_framebuffer_option_fails_before_launch(self):
        args = ["runner", "flycast", "game.cdi", "serial.log", "1", "no"]
        with patch.object(runner.sys, "argv", args), \
                patch.dict(os.environ, {"GEX_TEST_FRAMEBUFFER": "invalid"}), \
                patch.object(runner.subprocess, "Popen") as launch:
            with self.assertRaisesRegex(ValueError, "GEX_TEST_FRAMEBUFFER"):
                runner.main()
            launch.assert_not_called()

    def test_guest_exit_before_screenshot_fails(self):
        read, write = os.pipe()
        os.write(write, b"arch: exit return code 0\n")
        os.close(write)
        process = Mock(stdout=os.fdopen(read, "rb"))
        process.poll.return_value = None
        with tempfile.TemporaryDirectory(prefix="gex-dc-runner-") as work:
            args = ["runner", "flycast", "game.cdi", str(Path(work) / "serial.log"),
                    "1", "no", str(Path(work) / "screen.png"), "2600"]
            with patch.object(runner.sys, "argv", args), \
                    patch.object(runner.subprocess, "Popen", return_value=process):
                self.assertEqual(runner.main(), 1)
        process.terminate.assert_called_once()
        self.assertTrue(process.stdout.closed)
