"""Synthetic hardware-thread comments for tools/hardware_reports.py."""

from contextlib import redirect_stderr, redirect_stdout
import io
import json
from pathlib import Path
import re
import sys
from tempfile import TemporaryDirectory
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import hardware_reports  # noqa: E402

RUNNER = ROOT / "tools" / "gpu-check" / "Run-GpuCheck.ps1"
THREAD = "https://github.com/R0Y00/Xbox2Windows/issues/1"
COMMIT = "0123456789abcdef0123456789abcdef01234567"
DASH = "\u2013"
SKIP_32 = f"Skipped, lane range 32{DASH}32"
PASSED = "408 queries passed"


def table_row(*cells):
    return "| " + " | ".join(cells) + " |"


README = "\n".join([
    "# Example",
    "",
    hardware_reports.TABLE_HEADER,
    "|---|---:|---|---|---|",
    table_row("Example GPU 100 (driver 1.0)", "32", SKIP_32, PASSED,
              f"[1.0 report]({THREAD}#issuecomment-101)"),
    table_row("Example Graphics (device 2222)", "64", "Passed, 64 lanes", "Skipped, DXR unsupported",
              f"[2.0 report]({THREAD}#issuecomment-102)"),
    table_row("Example Arc 500", "32", f"Skipped, lane range 16{DASH}32", PASSED,
              f"[5.0 report]({THREAD}#issuecomment-104)"),
    table_row("Microsoft WARP (software)", "4", "Skipped", PASSED, "[Baseline](docs/experiments.md)"),
    "",
    f"A translation-layer run is linked in prose: [run]({THREAD}#issuecomment-103).",
    "",
])


def report(adapter="Example GPU 100", device="1111", driver="1.0", backend="hardware", lanes=32,
           lane_range="32..32", model="0x66", wave64="SKIP", tier=11, bvh="PASS", debug="unavailable"):
    """Return a report in the layout that tools/gpu-check/Run-GpuCheck.ps1 writes."""
    head = [f"backend={backend} debug_layer={debug}", f"adapter={adapter} vendor=10de device={device}",
            f"driver={driver}"]
    w64 = [f"native_lane_range={lane_range} shader_model_capped_at_6_6={model}"]
    if wave64 == "PASS":
        w64.append("native_wave_width=64 logical_width=64 groups=38 fields_per_thread=28 mismatches=0")
    diff = [f"dxr_tier={tier} shader_model_capped_at_6_5=0x65"]
    if bvh == "PASS":
        diff += [f"epoch={n} comparisons=136 hits=90 misses=46 changed=1 witness=0" for n in range(3)]
    cases = [
        ("bvh_waves", "PASS", head + [f"native_wave_width={lanes} logical_width=64 groups=38 mismatches=0"]),
        ("bvh_wave64", wave64, head + w64),
        ("bvh_differential", bvh, head + diff),
    ]
    lines = ["## Xbox2Windows GPU report", "", f"Source commit: {COMMIT}", "Windows build: 26100.1000",
             f"Backend: {backend}", "", "### Summary", "",
             f"- Adapter: {adapter} (vendor=10de device={device})", f"- Driver: {driver}",
             f"- Debug layer: {debug}", f"- Native lane range: {lane_range}", "",
             "| Test | Result |", "|---|---|"]
    lines += [f"| {name}_{backend} | {status} |" for name, status, _ in cases]
    for name, status, output in cases:
        lines += ["", f"### {name}_{backend} - {status}", "", "```text", *output, "```"]
    lines += ["", "### Submit this report", "", THREAD, "", "Do not open a new GitHub issue.", ""]
    return "\r\n".join(lines)


def comment(number, body, login="tester"):
    return {"id": number, "html_url": f"{THREAD}#issuecomment-{number}", "user": {"login": login},
            "created_at": "2026-01-01T00:00:00Z", "body": body}


def only_report(body):
    reports, reason = hardware_reports.parse_comment(comment(900, body))
    if len(reports) != 1:
        raise AssertionError(f"expected one report, got {len(reports)} ({reason})")
    return reports[0]


def unrecorded_ids(summary):
    return [item["comment_id"] for item in summary["unrecorded"]]


class HardwareReportChecks(unittest.TestCase):
    def test_parser_follows_the_runner_layout(self):
        script = RUNNER.read_text(encoding="utf-8")
        self.assertIn(f"$report.Add('{hardware_reports.HEADER}')", script)
        for line in ('"Source commit: $revision"', '"Backend: $Backend"',
                     '"### $($case.Name) - $($case.Status)"'):
            self.assertIn(f"$report.Add({line})", script)
        runner_patterns = dict(re.findall(
            r"^\$(adapter|vendor|device|driver|debugLayer|laneRange) = Get-GpuField \$blocks '([^']+)'",
            script, flags=re.MULTILINE))
        self.assertEqual(runner_patterns, hardware_reports.FIELD_PATTERNS)

    def test_reads_the_generated_fields(self):
        parsed = only_report(report(debug="enabled"))
        self.assertEqual(parsed.adapter, "Example GPU 100")
        self.assertEqual((parsed.fields["device"], parsed.fields["driver"], parsed.fields["debugLayer"]),
                         ("1111", "1.0", "enabled"))
        self.assertEqual((parsed.commit, parsed.windows_build, parsed.backend),
                         (COMMIT, "26100.1000", "hardware"))
        self.assertEqual(parsed.results,
                         {"bvh_waves": "PASS", "bvh_wave64": "SKIP", "bvh_differential": "PASS"})
        self.assertEqual(parsed.notes, [])
        self.assertEqual(hardware_reports.evidence_label(parsed), "1.0 report, debug layer enabled")

    def test_cells_use_the_readme_wording(self):
        cases = [
            ({}, ("32", SKIP_32, PASSED)),
            ({"lanes": 64, "lane_range": "32..64", "wave64": "PASS"}, ("64", "Passed, 64 lanes", PASSED)),
            ({"lane_range": "32..64", "model": "0x65"}, ("32", "Skipped, Shader Model 6.5", PASSED)),
            ({"model": "0x65"}, ("32", f"{SKIP_32}, Shader Model 6.5", PASSED)),
            ({"tier": 0, "bvh": "SKIP"}, ("32", SKIP_32, "Skipped, DXR unsupported")),
            ({"tier": 10, "bvh": "SKIP"}, ("32", SKIP_32, "Skipped, DXR tier 1.0")),
            ({"bvh": "FAIL"}, ("32", SKIP_32, "Failed")),
        ]
        for options, expected in cases:
            with self.subTest(options=options):
                parsed = only_report(report(**options))
                cells = (hardware_reports.lanes_cell(parsed), hardware_reports.wave64_cell(parsed),
                         hardware_reports.bvh_cell(parsed))
                self.assertEqual(cells, expected)

    def test_lists_only_reports_the_table_does_not_link(self):
        comments = [
            comment(101, report()),
            comment(150, "Thanks for the reports. The README links #101."),
            comment(201, report(driver="1.1")),
            comment(202, report(adapter="Example GPU 300", device="3333", driver="3.0")),
        ]
        summary = hardware_reports.summarize(comments, README)
        self.assertEqual((summary["reports"], summary["linked"]), (3, 1))
        self.assertEqual(unrecorded_ids(summary), [201, 202])
        first, second = summary["unrecorded"]
        self.assertEqual((first["row"], first["notes"]), ("Example GPU 100 (driver 1.0)", []))
        self.assertEqual(first["cells"]["evidence"], f"[1.1 report]({THREAD}#issuecomment-201)")
        self.assertIsNone(second["row"])
        self.assertEqual(summary["review"], [])
        self.assertEqual(unrecorded_ids(hardware_reports.summarize(comments, README, after=201)), [202])

    def test_matches_rows_by_device_and_the_software_adapter(self):
        rows = hardware_reports.coverage_rows(README)
        cases = [
            (report(adapter="Example(TM) Graphics", device="2222"), "Example Graphics (device 2222)"),
            (report(adapter="Example(TM) Graphics", device="4444"), None),
            (report(adapter="Example(R) Arc(TM) 500 Graphics"), "Example Arc 500"),
            (report(adapter="Microsoft Basic Render Driver", backend="warp"), "Microsoft WARP (software)"),
            (report(adapter="Microsoft Basic Render Driver"), "Microsoft WARP (software)"),
        ]
        for body, expected in cases:
            with self.subTest(expected=expected):
                row = hardware_reports.matching_row(only_report(body), rows)
                self.assertEqual(row.adapter if row else None, expected)

    def test_flags_results_that_differ_from_the_row(self):
        body = report(adapter="Example Graphics", device="2222", lanes=64, lane_range="32..64", model="0x65",
                      tier=0, bvh="SKIP")
        summary = hardware_reports.summarize([comment(301, body)], README)
        self.assertEqual(summary["unrecorded"][0]["notes"],
                         ["forced wave64 'Skipped, Shader Model 6.5'; the row says 'Passed, 64 lanes'"])

    def test_other_layouts_go_to_manual_review(self):
        attachment = ("[gpu-report-hardware.md]"
                      "(https://github.com/user-attachments/files/1/gpu-report-hardware.md)")
        cases = [
            (f"Example GPU run {attachment}", "report attached as a file"),
            (report().replace("adapter=", "gpu="), "no adapter line in the test output"),
            (report().replace("Source commit", "Commit"), "generated layout without a source commit"),
            (report() + "\nRan under Proton with vkd3d-proton.",
             "Linux translation-layer run; README records these in prose"),
            (report().split("### Summary")[0], "results without the generated test sections"),
        ]
        comments = [comment(400 + number, body) for number, (body, _) in enumerate(cases)]
        # A linked comment and a quoted report are not listed.
        quoted = "\n".join("> " + line for line in report().split("\r\n"))
        comments += [comment(103, report().split("### Summary")[0]), comment(500, quoted)]
        summary = hardware_reports.summarize(comments, README)
        self.assertEqual(summary["unrecorded"], [])
        self.assertEqual([item["reason"] for item in summary["review"]], [reason for _, reason in cases])

    def test_splits_pasted_reports_and_marks_duplicates(self):
        warp = report(adapter="Microsoft Basic Render Driver", backend="warp", lanes=4, lane_range="4..4")
        reports, _ = hardware_reports.parse_comment(comment(600, report() + "\n\n" + warp))
        self.assertEqual([(item.backend, item.adapter) for item in reports],
                         [("hardware", "Example GPU 100"), ("warp", "Microsoft Basic Render Driver")])
        twice, _ = hardware_reports.parse_comment(comment(601, report() + "\n\n" + report()))
        self.assertEqual(len(twice), 1)
        same = [comment(602, report(driver="1.2")), comment(603, report(driver="1.2"))]
        summary = hardware_reports.summarize(same, README)
        self.assertEqual([item["notes"] for item in summary["unrecorded"]],
                         [[], ["same text as comment 602"]])

    def test_duplicate_test_section_goes_to_manual_review(self):
        # A corrected rerun pasted into the same report has no reliable winner.
        # Keep the separate, unambiguous WARP report from the same comment.
        hardware, warp = report(), report(backend="warp", adapter="Microsoft Basic Render Driver",
                                          lanes=4, lane_range="4..4")
        # The existing behavior for a partial paste plus a valid report is unchanged.
        partial = hardware.replace("Source commit", "Commit")
        comments = [comment(699, partial + "\n\n" + warp)]
        summary = hardware_reports.summarize(comments, README)
        self.assertEqual([(item["comment_id"], item["backend"]) for item in summary["unrecorded"]], [(699, "warp")])
        self.assertEqual(summary["review"], [])
        # A WARP section without a new report header also shares the same
        # result key, so it cannot be attributed to a separate WARP run.
        for backend, status in (("hardware", "FAIL"), ("hardware", "PASS"), ("warp", "PASS")):
            with self.subTest(backend=backend, status=status):
                name = f"bvh_differential_{backend}"
                duplicate = f"### {name} - {status}\n\n```text\ncomparisons=999\n```"
                malformed = hardware.replace("### Submit this report", duplicate + "\n\n### Submit this report")
                comments = [comment(700, "\n\n".join((partial, malformed, warp))), comment(701, hardware)]
                summary = hardware_reports.summarize(comments, README)
                self.assertEqual([(item["comment_id"], item["backend"]) for item in summary["unrecorded"]],
                                 [(700, "warp"), (701, "hardware")])
                self.assertEqual(summary["unrecorded"][1]["cells"]["bvh"], PASSED)
                self.assertEqual([(item["comment_id"], item["reason"]) for item in summary["review"]],
                                 [(700, f"duplicate test section: {name}")])

    def test_edited_reports_are_read_with_a_note(self):
        edited = report().replace(f"Source commit: {COMMIT}", "**Source commit:** `0123456`")
        parsed = only_report(edited.replace(" - SKIP", " \u2014 SKIP"))
        self.assertEqual((parsed.commit, parsed.results["bvh_wave64"]), ("0123456", "SKIP"))
        self.assertEqual(parsed.notes, ["edited report layout; compare the cells with the comment"])

    def test_command_reads_paginated_cli_output(self):
        with TemporaryDirectory() as directory:
            folder = Path(directory)
            readme = folder / "README.md"
            readme.write_text(README, encoding="utf-8")
            pages = folder / "comments.json"
            new_adapter = report(adapter="Example GPU 300", device="3333", driver="3.0")
            first_page = json.dumps([comment(101, report())])
            pages.write_text(first_page + "\n" + json.dumps([comment(202, new_adapter)]), encoding="utf-8")
            output = io.StringIO()
            with redirect_stdout(output):
                self.assertEqual(hardware_reports.main([str(pages), "--readme", str(readme)]), 0)
            text = output.getvalue()
            self.assertIn("README.md links 1 of them.", text)
            self.assertIn("## 1. Example GPU 300 (new row)", text)
            self.assertIn(table_row("Example GPU 300", "32", SKIP_32, PASSED,
                                    f"[3.0 report]({THREAD}#issuecomment-202)"), text)
            wide = folder / "comments-utf16.json"
            wide.write_text(pages.read_text(encoding="utf-8"), encoding="utf-16")
            output = io.StringIO()
            with redirect_stdout(output):
                hardware_reports.main([str(wide), "--readme", str(readme), "--json"])
            self.assertEqual(unrecorded_ids(json.loads(output.getvalue())), [202])
            bad = folder / "bad.json"
            bad.write_text(json.dumps({"total": 1}), encoding="utf-8")
            errors = io.StringIO()
            with redirect_stderr(errors):
                self.assertEqual(hardware_reports.main([str(bad), "--readme", str(readme)]), 2)
            self.assertIn("expected GitHub issue comment objects", errors.getvalue())

    def test_repository_readme_has_the_coverage_table(self):
        readme = (ROOT / "README.md").read_text(encoding="utf-8")
        rows = hardware_reports.coverage_rows(readme)
        self.assertTrue(rows and all(row.adapter for row in rows))
        in_table = set().union(*(row.comment_ids for row in rows))
        self.assertTrue(in_table)
        self.assertLessEqual(in_table, hardware_reports.linked_comment_ids(readme))


if __name__ == "__main__":
    unittest.main()
