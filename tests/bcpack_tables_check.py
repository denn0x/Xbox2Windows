"""Verify src/startup_stubs/bcpack_tables.h against rANS model and codebook references."""
import hashlib
import json
from pathlib import Path
import re
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "src" / "startup_stubs" / "bcpack_tables.h"
REF_DATA = ROOT / "out" / "bcpack-rs" / "data"

sys.path.insert(0, str(ROOT / "tools" / "_bcpack_rans"))
import bcpack_tables as py_tables  # noqa: E402


def strip_comments(text: str) -> str:
    return re.sub(r"//[^\n]*", "", text)


def extract_array(text: str, name: str):
    m = re.search(r"\b" + re.escape(name) + r"\s*(?:\[[^\]]*\])*\s*=\s*\{", text)
    if not m:
        raise ValueError(f"Array {name} not found in header")
    start = m.end() - 1
    depth = 0
    for i in range(start, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                body = text[start : i + 1]
                break
    else:
        raise ValueError(f"Array {name}: unbalanced braces")
    body = body.replace("{", "[").replace("}", "]")
    body = re.sub(r"\b(0[xX][0-9a-fA-F]+|\d+)[uUlL]+\b", r"\1", body)
    return eval(body, {"__builtins__": {}}, {})  # noqa: S307 - generated numeric literal


class BCPackTablesCheck(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.text = strip_comments(HEADER.read_text(encoding="utf-8"))

    def test_rans_model_tables(self):
        for name in ("BASE_TABLE", "FREQ_TABLE", "RF_TABLE", "RN_TABLE", "RS_TABLE"):
            actual = extract_array(self.text, name)
            expected = getattr(py_tables, name)
            self.assertEqual(actual, expected, f"Mismatch in table {name}")

    def test_rans_canonical_sha256(self):
        canonical = "".join(
            ",".join(str(v) for alphabet in getattr(py_tables, table) for row in alphabet for v in row) + "\n"
            for table in ("BASE_TABLE", "FREQ_TABLE", "RF_TABLE", "RS_TABLE", "RN_TABLE")
        )
        self.assertEqual(hashlib.sha256(canonical.encode()).hexdigest(), py_tables.TABLES_SHA256)

    def test_bc7_control_table_structure(self):
        control = extract_array(self.text, "BC7_CONTROL_TABLE")
        self.assertEqual(len(control), 512)
        valid_count = 0
        for entry in control:
            self.assertEqual(len(entry), 5)
            mode, partition, rotation, index_sel, valid = entry
            if valid:
                valid_count += 1
                self.assertIn(mode, range(8))
                self.assertIn(partition, range(64))
                self.assertIn(rotation, range(4))
                self.assertIn(index_sel, range(2))
            else:
                self.assertEqual(entry, [0, 0, 0, 0, 0])
        self.assertEqual(valid_count, 285)

    def test_bc7_dictionary_structure(self):
        dictionary = extract_array(self.text, "BC7_DICTIONARY")
        self.assertEqual(len(dictionary), 255)
        for entry in dictionary:
            self.assertEqual(len(entry), 3)
            subsets, ep, ix = entry
            self.assertIn(subsets, (0, 1, 2, 3))
            self.assertEqual(len(ep), 3)
            for row in ep:
                self.assertEqual(len(row), 8)
                for b in row:
                    self.assertIn(b, range(256))
            self.assertEqual(len(ix), 2)
            for row in ix:
                self.assertEqual(len(row), 16)
                for b in row:
                    self.assertIn(b, range(256))

    def test_bc1_window_structure(self):
        window = extract_array(self.text, "BC1_WINDOW")
        self.assertEqual(len(window), 256)
        for row in window:
            self.assertEqual(len(row), 6)
            for b in row:
                self.assertIn(b, range(256))

    def test_bc1_quad_masks_structure(self):
        quad_masks = extract_array(self.text, "BC1_QUAD_MASKS")
        self.assertEqual(len(quad_masks), 4)
        for row in quad_masks:
            self.assertEqual(len(row), 256)
            for mask in row:
                self.assertGreaterEqual(mask, 0)
                self.assertLessEqual(mask, 0xFFFFFFFF)

    def test_compare_with_reference_data_if_present(self):
        if not (REF_DATA / "bc7.json").exists():
            self.skipTest("Reference JSON data in out/bcpack-rs/data not present")

        control_json = json.loads((REF_DATA / "bc7.json").read_text(encoding="utf-8"))["control"]
        expected_control = [[0, 0, 0, 0, 0] for _ in range(512)]
        for e, o, mode, partition, rotation, index_sel in control_json:
            code = ((o & 15) << 5) | (e & 31)
            self.assertEqual(expected_control[code], [0, 0, 0, 0, 0])
            expected_control[code] = [mode, partition, rotation, index_sel, 1]
        self.assertEqual(extract_array(self.text, "BC7_CONTROL_TABLE"), expected_control)

        dict_json = json.loads((REF_DATA / "bc7.json").read_text(encoding="utf-8"))["dictionary"]
        expected_dict = []
        for entry in dict_json:
            if entry is None:
                expected_dict.append([0, [[0] * 8 for _ in range(3)], [[0] * 16 for _ in range(2)]])
                continue
            subsets = entry.get("subsets") or 0
            ep = [0 if v is None else v for v in entry["ep"]]
            ix = [0 if v is None else v for v in entry["ix"]]
            expected_dict.append([subsets, [ep[0:8], ep[8:16], ep[16:24]], [ix[0:16], ix[16:32]]])
        self.assertEqual(extract_array(self.text, "BC7_DICTIONARY"), expected_dict)

        window_json = json.loads((REF_DATA / "bc1_window.json").read_text(encoding="utf-8"))["slots"]
        expected_window = [[0] * 6 if e is None else e["value"] for e in window_json]
        self.assertEqual(extract_array(self.text, "BC1_WINDOW"), expected_window)


if __name__ == "__main__":
    unittest.main()
