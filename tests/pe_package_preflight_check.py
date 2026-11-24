"""Offline package-closure controls; synthetic import/export inventories only."""

import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import pefile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from compat_report_check import POSITIVE_IMPORTS, build_image  # noqa: E402
from pe_inventory import exports, parse_image  # noqa: E402
from pe_package_preflight import inspect, main, summarize  # noqa: E402
from pe_run_package import plan  # noqa: E402


def info(name, entries=(), imports=(), machine="IMAGE_FILE_MACHINE_AMD64"):
    exports = [dict(name=symbol, ordinal=i + 1, forwarder=forward)
               for i, (symbol, forward) in enumerate(entries)]
    return dict(machine=machine, sha256=name + "-fixture", exports=exports,
                parse_warnings=[]), imports


class OfflinePreflightChecks(unittest.TestCase):
    def setUp(self):
        self.package = {
            "main.exe": info("main", imports=[
                ("normal", "shim.dll", "Start"),
                ("normal", "shim.dll", "Missing"),
                ("normal", "shim.dll", "Broken"),
                ("normal", "shim.dll", "Loop"),
                ("normal", "shim.dll", "Api"),
                ("normal", "kernel32.dll", "ExitProcess"),
                ("normal", "twin.dll", "Twinned"),
                ("delay", "optional.dll", "Late")]),
            "bin/shim.dll": info("shim", entries=[
                ("Start", "core.Result"),
                ("Broken", "core.Absent"),
                ("Loop", "core.Loop"),
                ("Api", "notinpackage.Func")]),
            "bin/core.dll": info("core", entries=[
                ("Result", None), ("Loop", "shim.Loop")],
                imports=[("normal", "kernel32.dll", "Sleep")]),
            "optional.dll": info("optional", entries=[("Late", None)]),
            "a/twin.dll": info("twina", entries=[("Twinned", None)]),
            "b/twin.dll": info("twinb", entries=[("Twinned", None)]),
            "unused.dll": info("unused", entries=[("Ignore", None)]),
        }

    def test_package_forwarders_statuses_and_entry_graph(self):
        report = summarize(self.package, "main.exe")
        lookup = {(r["image"], r["symbol"]): r for r in report["imports"]}
        assert lookup["main.exe", "Start"]["status"] == "bundled_export_present"
        assert [(s["provider"], s["symbol"]) for s in lookup["main.exe", "Start"]["chain"]] == [
            ("bin/shim.dll", "Start"), ("bin/core.dll", "Result")]
        assert lookup["main.exe", "Missing"]["status"] == "bundled_export_not_found"
        assert lookup["main.exe", "Broken"]["status"] == "bundled_export_not_found"
        assert lookup["main.exe", "Loop"]["status"] == "bundled_forwarder_cycle"
        assert lookup["main.exe", "Api"]["status"] == "forwarded_external_unverified"
        assert lookup["main.exe", "ExitProcess"]["status"] == "external_unverified"
        assert lookup["main.exe", "Twinned"]["status"] == "ambiguous_package_basename"
        assert lookup["main.exe", "Late"]["status"] == "bundled_export_present"
        assert report["reachable"]["bin/core.dll"] == {
            "path": ["main.exe", "bin/shim.dll", "bin/core.dll"],
            "conditional": False,
            "reasons": ["normal", "forwarder"],
        }
        assert "bin/shim.dll" in report["reachable"]
        assert report["reachable"]["optional.dll"]["conditional"] is True
        assert "unused.dll" in report["ignored_images"]
        assert "a/twin.dll" in report["ignored_images"]
        assert "b/twin.dll" in report["ignored_images"]
        assert report["summary"]["static_imports"] == 9
        assert report["summary"]["bundled_failures"] == 4
        assert report["summary"]["external_unverified"] == 3
        assert report["scope"].startswith("Static package")
        assert "game" not in json.dumps(report).lower()

    def test_provider_names_case_insensitive_and_ordinal(self):
        package = {
            "app.exe": info("app", imports=[("normal", "SHIM.DLL", "#71")]),
            "sub/SHIM.dll": info("shim", entries=[("Alpha", None)]),
        }
        result = summarize(package, "app.exe")
        assert result["imports"][0]["status"] == "bundled_export_not_found"
        package["sub/SHIM.dll"] = (dict(package["sub/SHIM.dll"][0], exports=[
            dict(name="Alpha", ordinal=71, forwarder=None)]), ())
        assert summarize(package, "app.exe")["imports"][0]["status"] == "bundled_export_present"

    def test_report_is_deterministic(self):
        one = summarize(self.package, "main.exe")
        two = summarize(dict(reversed(list(self.package.items()))), "main.exe")
        assert json.dumps(one, sort_keys=True) == json.dumps(two, sort_keys=True)

    def test_mixed_x86_machines_do_not_satisfy_matching_exports(self):
        for importer, provider in [("AMD64", "I386"), ("I386", "AMD64")]:
            with self.subTest(importer=importer, provider=provider):
                package = {
                    "app.exe": info("app", imports=[("normal", "shim.dll", "Start")],
                                    machine="IMAGE_FILE_MACHINE_" + importer),
                    "shim.dll": info("shim", entries=[("Start", None)],
                                     machine="IMAGE_FILE_MACHINE_" + provider),
                }
                result = summarize(package, "app.exe")
                row = result["imports"][0]
                self.assertEqual(row["status"], "bundled_machine_mismatch")
                self.assertEqual(row["resolution_status"], "machine_mismatch")
                self.assertEqual(row["chain"][0]["importer_machine"], "IMAGE_FILE_MACHINE_" + importer)
                self.assertEqual(row["chain"][0]["provider_machine"], "IMAGE_FILE_MACHINE_" + provider)
                self.assertEqual(result["summary"]["bundled_failures"], 1)
                with self.assertRaisesRegex(ValueError, "bundled_machine_mismatch"):
                    plan(result)  # The package runner must refuse this before mapping or attachment.
                # The same exported name is a valid symbolic binding at the matching machine type.
                package["shim.dll"][0]["machine"] = "IMAGE_FILE_MACHINE_" + importer
                self.assertEqual(summarize(package, "app.exe")["imports"][0]["status"],
                                 "bundled_export_present")

    def test_forwarder_stops_at_incompatible_packaged_machine(self):
        package = {
            "app.exe": info("app", imports=[("delay", "shim.dll", "Start")]),
            "shim.dll": info("shim", entries=[("Start", "wrong.Next")]),
            "wrong.dll": info("wrong", entries=[("Next", "external.Result")],
                              machine="IMAGE_FILE_MACHINE_I386"),
        }
        result = summarize(package, "app.exe")
        row = result["imports"][0]
        self.assertEqual(row["status"], "bundled_machine_mismatch")
        self.assertEqual([step["provider"] for step in row["chain"]], ["shim.dll", "wrong.dll"])
        self.assertEqual(result["summary"]["external_unverified"], 0)
        self.assertTrue(result["reachable"]["wrong.dll"]["conditional"])

    def test_unreachable_or_hybrid_machines_do_not_fail_x86_check(self):
        package = {
            "app.exe": info("app", imports=[("normal", "shim.dll", "Start")]),
            "shim.dll": info("shim", entries=[("Start", None)]),
            "unused.dll": info("unused", entries=[("Unused", None)],
                               machine="IMAGE_FILE_MACHINE_I386"),
        }
        self.assertEqual(summarize(package, "app.exe")["summary"]["bundled_failures"], 0)
        for machine in ("IMAGE_FILE_MACHINE_ARM64EC", "IMAGE_FILE_MACHINE_ARM64X", "0xffff"):
            package["shim.dll"][0]["machine"] = machine
            self.assertEqual(summarize(package, "app.exe")["imports"][0]["status"],
                             "bundled_export_present")  # Other machine combinations remain unverified.

    def test_normal_forwarder_precedes_a_direct_delay_import(self):
        metadata, imports = self.package["main.exe"]
        self.package["main.exe"] = (metadata, [*imports, ("delay", "core.dll", "Result")])
        core = summarize(self.package, "main.exe")["reachable"]["bin/core.dll"]
        self.assertEqual(core["path"], ["main.exe", "bin/shim.dll", "bin/core.dll"])
        self.assertEqual(core["reasons"], ["normal", "forwarder"])
        self.assertFalse(core["conditional"])

    def test_incorrect_entry_rejected(self):
        with self.assertRaisesRegex(ValueError, "Entry is not"):
            summarize(self.package, "missing.exe")

    def test_strict_plan_refuses_external_provider(self):
        package = {
            "app.exe": info("app", imports=[("normal", "xgameruntime.dll", "XUserAddAsync")]),
        }
        report = summarize(package, "app.exe")
        row = report["imports"][0]
        self.assertEqual(row["status"], "external_unverified")
        self.assertFalse(row["chain"][0]["package"])
        with self.assertRaisesRegex(
                ValueError, r"app\.exe: xgameruntime\.dll!XUserAddAsync: external_unverified"):
            plan(report, allow_external=False)
        order, bindings = plan(report)
        self.assertEqual(order, ["app.exe"])
        self.assertEqual(bindings["app.exe"][("xgameruntime.dll", "XUserAddAsync")]["dll"],
                         "xgameruntime.dll")

    def test_inspect_never_executes_and_ignores_non_pe_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "main.exe").write_bytes(b"source-built synthetic fixture")
            (root / "shim.dll").write_bytes(b"source-built synthetic fixture")
            (root / "data.rpf").write_bytes(b"asset placeholder")
            def fake_parse(path):
                if path.name == "main.exe":
                    return info("main", imports=[("normal", "shim.dll", "Start")])
                return info("shim", entries=[("Start", None)])
            with patch("pe_package_preflight.parse_image", side_effect=fake_parse) as parser:
                report = inspect(root, "main.exe")
            assert parser.call_count == 2
            assert report["summary"]["images_scanned"] == 2
            assert report["summary"]["bundled_failures"] == 0
            with self.assertRaisesRegex(ValueError, "escapes package root"):
                inspect(root, "../outside.exe")
            with self.assertRaisesRegex(ValueError, "Entry must be"):
                inspect(root, "data.rpf")

    def test_inspect_refuses_a_symlinked_pe_candidate(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "main.exe").write_bytes(b"source-built synthetic fixture")
            (root / "shim.dll").write_bytes(b"source-built synthetic fixture")
            try:
                (root / "linked.dll").symlink_to(root / "shim.dll")
            except OSError as error:
                # Windows refuses symlink creation without Developer Mode or
                # SeCreateSymbolicLinkPrivilege (ERROR_PRIVILEGE_NOT_HELD).
                if getattr(error, "winerror", None) != 1314:
                    raise
                self.skipTest("this Windows account cannot create symlinks (WinError 1314)")
            with self.assertRaisesRegex(ValueError, "symlink"):
                inspect(root, "main.exe")

    def test_report_cannot_overwrite_input_through_hard_link(self):
        with tempfile.TemporaryDirectory() as tmp:
            parent = Path(tmp)
            package = parent / "package"
            package.mkdir()
            image = package / "main.exe"
            image.write_bytes(b"synthetic source image")
            output = parent / "report.json"
            output.hardlink_to(image)
            with patch("pe_package_preflight.inspect", return_value={"entry": "main.exe"}):
                self.assertEqual(main([str(package), "main.exe", "--output", str(output)]), 0)
            self.assertEqual(image.read_bytes(), b"synthetic source image")
            self.assertEqual(json.loads(output.read_text()), {"entry": "main.exe"})

    def test_cli_rejects_invalid_pe_without_traceback(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "main.exe").write_bytes(b"not a PE")
            process = subprocess.run([sys.executable, "-B", str(ROOT / "tools/pe_package_preflight.py"),
                                      str(root), "main.exe"], capture_output=True, text=True)
            assert process.returncode == 1
            assert "pe_package_preflight:" in process.stderr
            assert "Traceback" not in process.stderr
            assert process.stdout == ""

    def test_image_without_data_directories_has_no_exports(self):
        data = bytearray(build_image(POSITIVE_IMPORTS))
        count_offset = struct.unpack_from("<I", data, 60)[0] + 4 + 20 + 108
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            control = root / "control.exe"
            control.write_bytes(bytes(data))
            with pefile.PE(str(control), fast_load=True) as pe:
                assert len(pe.OPTIONAL_HEADER.DATA_DIRECTORY) == 16
                assert exports(pe) == []
            assert len(parse_image(control)[1]) > 0

            struct.pack_into("<I", data, count_offset, 0)
            (root / "app.exe").write_bytes(bytes(data))
            with pefile.PE(str(root / "app.exe"), fast_load=True) as pe:
                assert len(pe.OPTIONAL_HEADER.DATA_DIRECTORY) == 0
                assert exports(pe) == []
            info, imports = parse_image(root / "app.exe")
            assert info["exports"] == []
            process = subprocess.run([sys.executable, "-B", str(ROOT / "tools/pe_package_preflight.py"),
                                      str(root), "app.exe"], capture_output=True, text=True)
            assert process.returncode == 0, process.stderr
            assert "Traceback" not in process.stderr
            report = json.loads(process.stdout)
            assert report["images"]["app.exe"]["exports"] == 0


if __name__ == "__main__":
    unittest.main()
