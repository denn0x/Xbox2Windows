"""pe_run_package reports a package image that is not a PE as an error line, not a traceback."""

from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools" / "pe_run_package.py"
sys.path.insert(0, str(ROOT / "tests"))
from compat_report_check import POSITIVE_IMPORTS, build_image  # noqa: E402


def run_tool(root, mapper, entry):
    return subprocess.run([sys.executable, "-B", str(TOOL), str(mapper), str(root), entry, "--preflight-only"],
                          capture_output=True, text=True, timeout=60)


class InvalidPackageImageChecks(unittest.TestCase):
    def test_entry_that_is_not_a_pe_fails_cleanly(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            root = base / "package"
            root.mkdir()
            (root / "app.exe").write_bytes(b"MZ not a portable executable")
            mapper = base / "mapper.dll"
            mapper.write_bytes(b"")
            process = run_tool(root, mapper, "app.exe")
            self.assertEqual(process.returncode, 1, process.stderr)
            self.assertNotIn("Traceback", process.stderr)
            self.assertEqual(process.stdout, "")
            self.assertTrue(process.stderr.strip(), "an error message is expected")

    def test_bundled_dll_that_is_not_a_pe_fails_cleanly(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            root = base / "package"
            root.mkdir()
            (root / "app.exe").write_bytes(build_image(POSITIVE_IMPORTS))
            (root / "helper.dll").write_bytes(b"MZ not a portable executable")
            mapper = base / "mapper.dll"
            mapper.write_bytes(b"")
            process = run_tool(root, mapper, "app.exe")
            self.assertEqual(process.returncode, 1, process.stderr)
            self.assertNotIn("Traceback", process.stderr)
            self.assertEqual(process.stdout, "")
            self.assertTrue(process.stderr.strip(), "an error message is expected")

    def test_entry_outside_the_package_is_still_a_clean_error(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            root = base / "package"
            root.mkdir()
            mapper = base / "mapper.dll"
            mapper.write_bytes(b"")
            process = run_tool(root, mapper, "missing.exe")
            self.assertEqual(process.returncode, 1, process.stderr)
            self.assertNotIn("Traceback", process.stderr)
            self.assertEqual(process.stdout, "")
            self.assertTrue(process.stderr.strip(), "an error message is expected")


if __name__ == "__main__":
    unittest.main()
