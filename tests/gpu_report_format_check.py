"""Source checks for the standalone GPU report summary and submit path."""

from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
RUNNER = ROOT / "tools" / "gpu-check" / "Run-GpuCheck.ps1"
PACKAGE = ROOT / "tools" / "package-gpu-check.ps1"
PACKAGE_README = ROOT / "tools" / "gpu-check" / "README.md"
README = ROOT / "README.md"
THREAD_URL = "https://github.com/R0Y00/Xbox2Windows/issues/1"
EXPECTED_TESTS = (
    "dual-uav",
    "texture-mips",
    "bvh-waves",
    "bvh-wave64",
    "bvh-differential",
    "bvh-packed",
    "bvh-atomics",
    "bvh-numeric",
    "bvh-root-roundtrip",
    "dxr-pipeline",
    "dxr-update",
)

WAVES_BLOCK = """\
backend=hardware debug_layer=unavailable
adapter=NVIDIA GeForce RTX 2060 SUPER vendor=10de device=1f06
driver=32.0.16.2124
native_wave_width=32 logical_width=64 groups=38 fields_per_thread=28 mismatches=0
"""

WAVE64_BLOCK = """\
backend=hardware debug_layer=unavailable
adapter=NVIDIA GeForce RTX 2060 SUPER vendor=10de device=1f06
driver=32.0.16.2124
native_lane_range=32..32 shader_model_capped_at_6_6=0x66
"""

MISSING_DRIVER_BLOCK = """\
backend=warp debug_layer=enabled
adapter=Microsoft Basic Render Driver vendor=1414 device=008c
driver=unavailable via DXGI
"""

VIEW_HEAP_BLOCK = """\
max_view_descriptor_heap_size=1000000
"""


def field_patterns(script: str) -> dict[str, str]:
    matches = re.findall(
        r"^\$(adapter|vendor|device|driver|debugLayer|laneRange|viewHeapLimit) = Get-GpuField \$blocks '([^']+)'",
        script,
        flags=re.MULTILINE,
    )
    return dict(matches)


def first_field(pattern: str, blocks: list[str]) -> str | None:
    compiled = re.compile(pattern)
    for block in blocks:
        match = compiled.search(block)
        if match:
            return match.group(1).strip()
    return None


class GpuReportFormatChecks(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.script = RUNNER.read_text(encoding="utf-8")
        cls.patterns = field_patterns(cls.script)

    def test_runner_records_the_package_tests_in_order(self):
        listed = re.search(
            r"foreach \(\$test in ([^)]+)\)",
            self.script,
        )
        self.assertIsNotNone(listed)
        names = re.findall(r"'([^']+)'", listed.group(1))
        self.assertEqual(tuple(names), EXPECTED_TESTS)

    def test_runner_writes_the_coverage_thread_into_the_report(self):
        self.assertIn(f"$threadUrl = '{THREAD_URL}'", self.script)
        self.assertIn("$report.Add('### Summary')", self.script)
        self.assertIn("$report.Add('### Submit this report')", self.script)
        self.assertIn("$report.Add($threadUrl)", self.script)
        self.assertIn("Do not open a new GitHub issue.", self.script)

    def test_script_exports_the_expected_field_patterns(self):
        self.assertEqual(
            set(self.patterns),
            {"adapter", "vendor", "device", "driver", "debugLayer", "laneRange", "viewHeapLimit"},
        )

    def test_field_patterns_read_published_hardware_output(self):
        blocks = [WAVES_BLOCK, WAVE64_BLOCK]
        self.assertEqual(first_field(self.patterns["adapter"], blocks), "NVIDIA GeForce RTX 2060 SUPER")
        self.assertEqual(first_field(self.patterns["vendor"], blocks), "10de")
        self.assertEqual(first_field(self.patterns["device"], blocks), "1f06")
        self.assertEqual(first_field(self.patterns["driver"], blocks), "32.0.16.2124")
        self.assertEqual(first_field(self.patterns["debugLayer"], blocks), "unavailable")
        self.assertEqual(first_field(self.patterns["laneRange"], blocks), "32..32")

    def test_driver_pattern_keeps_a_multi_word_unavailable_value(self):
        self.assertEqual(
            first_field(self.patterns["driver"], [MISSING_DRIVER_BLOCK]),
            "unavailable via DXGI",
        )
        self.assertEqual(first_field(self.patterns["debugLayer"], [MISSING_DRIVER_BLOCK]), "enabled")
        self.assertEqual(first_field(self.patterns["adapter"], [MISSING_DRIVER_BLOCK]), "Microsoft Basic Render Driver")
        self.assertIsNone(first_field(self.patterns["laneRange"], [MISSING_DRIVER_BLOCK]))

    def test_view_heap_limit_is_optional(self):
        pattern = self.patterns["viewHeapLimit"]
        self.assertEqual(first_field(pattern, [WAVES_BLOCK, VIEW_HEAP_BLOCK]), "1000000")
        self.assertIsNone(first_field(pattern, [WAVES_BLOCK, WAVE64_BLOCK]))
        self.assertIn(
            '$report.Add("- Shader-visible view heap limit: $(Format-GpuValue $viewHeapLimit)")',
            self.script,
        )

    def test_package_smoke_test_requires_the_submit_path(self):
        text = PACKAGE.read_text(encoding="utf-8")
        self.assertIn("### Summary", text)
        self.assertIn("### Submit this report", text)
        self.assertIn(THREAD_URL, text)

    def test_powershell_scripts_are_ascii_for_windows_powershell(self):
        # Windows PowerShell 5.1 reads BOM-less scripts as the ANSI code page,
        # so a UTF-8 dash or curly quote can end a string early.
        for path in (RUNNER, PACKAGE):
            for number, line in enumerate(path.read_bytes().splitlines(), 1):
                with self.subTest(path=path.name, line=number):
                    self.assertTrue(line.isascii(), line.decode("utf-8", "replace"))

    def test_clean_build_report_links_to_an_installed_issue_template(self):
        guide = (ROOT / "CONTRIBUTING.md").read_text(encoding="utf-8")
        link = re.search(r"\[build bug report\]\(https://github\.com/R0Y00/Xbox2Windows/issues/new\?template=([^/)]+)\)", guide)
        self.assertIsNotNone(link)
        template = ROOT / ".github" / "ISSUE_TEMPLATE" / link.group(1)
        self.assertTrue(template.is_file(), template)

    def test_instructions_say_not_to_open_a_new_issue(self):
        for path in (README, PACKAGE_README):
            text = path.read_text(encoding="utf-8")
            self.assertIn("Do not open a new GitHub issue.", text)
            self.assertIn("issues/1", text)


if __name__ == "__main__":
    unittest.main()
