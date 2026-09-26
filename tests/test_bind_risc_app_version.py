"""Tests that release packaging binds the RiscRTE app version to firmware."""
import json
import tempfile
import unittest
from pathlib import Path

from scripts.bind_risc_app_version import bind_version, read_platformio_version


class BindRiscAppVersionTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name) / "gameboy.json"
        self.path.write_text(json.dumps({"version": "1.2.29", "display_name": "GameBoy"}), encoding="utf-8")

    def test_reads_version_from_platformio(self):
        platformio = Path(self.directory.name) / "platformio.ini"
        platformio.write_text("[version]\nVersion = 1.3.3\n", encoding="utf-8")
        self.assertEqual(read_platformio_version(platformio), "1.3.3")

    def test_rejects_invalid_platformio_version(self):
        platformio = Path(self.directory.name) / "platformio.ini"
        platformio.write_text("[version]\nVersion = v1.3.3\n", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "MAJOR.MINOR.PATCH"):
            read_platformio_version(platformio)

    def test_sets_version_from_firmware_release(self):
        bind_version(self.path, "1.3.1")
        self.assertEqual(json.loads(self.path.read_text(encoding="utf-8"))["version"], "1.3.1")

    def test_check_passes_for_matching_version(self):
        bind_version(self.path, "1.2.29", check=True)

    def test_check_rejects_mismatch(self):
        with self.assertRaisesRegex(ValueError, "does not match firmware release"):
            bind_version(self.path, "1.3.1", check=True)

    def test_rejects_non_semver_release(self):
        with self.assertRaisesRegex(ValueError, "MAJOR.MINOR.PATCH"):
            bind_version(self.path, "v1.3.1")


if __name__ == "__main__":
    unittest.main()
