"""Source-built paged record for state hierarchy, typed values and lexical hashes."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

TOOLS = Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOLS))
from script_path_replay import select_tree
from script_state_probe import inspect, inspect_tree, joaat


def fixture(*, damage_value=False, self_parent=False, priorities=(9,), duplicate_attr=False,
            id_kind=5, parent_tag="state", drop=None, root_parent=65535, root_schema_size: int = 80):
    example = "synthetic_orbit"
    def attr(name, spelling, raw, kind):
        return struct.pack("<IIIBBH", joaat(name), joaat(spelling), raw, kind, 1, 0)

    attrs = bytearray()
    nodes = bytearray()

    def node(name, parent, attributes):
        index = len(nodes) // 16
        nodes.extend(struct.pack("<I5H2B", joaat(name), parent, 0, 0, 0,
                                 len(attrs) // 16, len(attributes), 1))
        attrs.extend(b"".join(attributes))
        return index

    if len(priorities) > 1:
        root_parent = node("root", root_parent, [])
    for priority in priorities:
        ids = [attr("id", example, 0, id_kind)]
        if duplicate_attr:
            ids.append(attr("id", "other", 0, 5))
        state = node(parent_tag, root_parent, [] if drop == 0 else ids)
        camera = [
            attr("priority", str(priority), priority, 3),
            attr("blendduration", "37", 37, 3),
            attr("orbitdistance", "1.500000", 0x40000000 if damage_value else 0x3fc00000, 2),
            attr("pitchlimits", "-0.250000,0.500000,0.000000", 0, 1),
            attr("headinglimits", "-1.000000,1.000000,0.000000", 1, 1),
        ]
        node("fixture_child", state + int(self_parent),
             [row for i, row in enumerate(camera, 1) if i != drop])
    vecs = struct.pack("<6f", -0.25, 0.5, 0, -1, 1, 0)
    arrays = ((len(attrs)//16, 2), (0, 0), (2, 4), (0, 0), (len(nodes)//16, 3))
    root = b"".join(struct.pack("<IIQ", n, n, ref) for n, ref in arrays)
    blocks = (root, attrs, nodes, vecs)
    instances = bytearray()
    for kind, blob in enumerate(blocks, 1):
        instances.extend(struct.pack("<IIQ", kind, len(blob), 0xaaaaaaaaaaaaaaaa))
    for blob in blocks:
        instances.extend(bytes(-(len(instances) + 8) % 16))
        instances.extend(blob)
    definitions = ((root_schema_size, (("attributes", 0, 0x5c),)),
                   (16, (("name", 0, 0x4a), ("valueHash", 4, 0x4a))),
                   (16, (("name", 0, 0x4a), ("parentIndex", 4, 0x13), ("attributeCount", 14, 0x11))),
                   (12, tuple((s, i * 4, 0x14) for i, s in enumerate("xyz"))))
    schema = b"".join(struct.pack("<IIIIQIHH", kind, 0, 0, 0, 0, size, 0, len(fields))
                      for kind, (size, fields) in enumerate(definitions, 1))
    schema += b"".join(struct.pack("<IIBBHI", joaat(s), off, code, 0, 0, 0)
                       for _, fields in definitions for s, off, code in fields)
    header = bytearray(80)
    header[:5] = b"0DRPz"
    struct.pack_into("<I", header, 16, 1)
    struct.pack_into("<3H", header, 56, 4, 0, 4)
    body = bytearray()
    for tag, part in ((b"PSIN", instances), (b"LSCH", schema), (b"LENM", b""), (b"LSNS", bytes(65536)),
                      (b"LGID", b""), (b"LRSC", header)):
        body += struct.pack(">4sI", tag, len(part) + 8) + part
    body += struct.pack(">4sII8x", b"CHKS", 20, len(body) + 20)
    archive = bytearray(b"9FPR")
    for i in range(0, len(body), 65536):
        archive += bytes(-len(archive) % 16)
        archive += zlib.compress(body[i:i + 65536])
    return archive


class DuplicateStateIdTests(unittest.TestCase):
    def test_duplicate_state_id_is_rejected(self):
        # Two label-kind id attributes on one state: the replay raises, so the probe must not let the last win.
        nodes = [(joaat("state"), 65535, 0, 0, 0, 0, 2, 1)]
        attrs = [(joaat("id"), joaat(label), 0, 5, 1, 0) for label in ("first", "second")]
        with self.assertRaisesRegex(ValueError, "Duplicate state ID"):
            inspect_tree(nodes, attrs, b"", joaat("second"))


class ScriptStateTests(unittest.TestCase):
    def run_probe(self, state="synthetic_orbit", **options):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "original.rpf"
            path.write_bytes(fixture(**options))
            return inspect(path, state)

    def test_linked_values(self):
        result = self.run_probe()
        self.assertEqual(result["counts"]["all_five_literals_verified"], 1)
        self.assertEqual(result["first_verified_example"]["orbitdistance"], "1.500000")

    def test_different_binary_value(self):
        result = self.run_probe(damage_value=True)
        self.assertEqual(result["counts"]["camera_parameter_sets"], 1)
        self.assertIsNone(result["first_verified_example"])

    def test_ambiguous_states_in_either_order(self):
        for priorities in ((9, 17), (17, 9)):
            result = self.run_probe(priorities=priorities)
            self.assertIsNone(result["first_verified_example"])
            matches = result["ambiguous_state_candidates"]
            self.assertEqual([m["node_path"] for m in matches], [[0, 1], [0, 3]])
            self.assertEqual([m["record_index"] for m in matches], [0, 0])
            self.assertEqual([m["first_verified_example"]["priority"] for m in matches],
                             list(map(str, priorities)))

    def test_matches_across_records(self):
        first = fixture()
        second = fixture(priorities=(17,))
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "two.rpf"
            path.write_bytes(first + bytes(-len(first) % 16) + second[16:])
            result = inspect(path, "synthetic_orbit")
            self.assertNotIn("ambiguous_state_candidates", inspect(path, "missing"))
        self.assertIsNone(result["first_verified_example"])
        self.assertEqual([(m["record_index"], m["node_path"])
                          for m in result["ambiguous_state_candidates"]], [(0, [0]), (1, [0])])

    def test_mismatched_root_schema_does_not_select_state(self) -> None:
        mismatched = fixture(root_schema_size=64)
        valid = fixture(priorities=(17,))
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "two.rpf"
            path.write_bytes(mismatched + bytes(-len(mismatched) % 16) + valid[16:])
            with self.subTest(consumer="probe"):
                result = inspect(path, "synthetic_orbit")
                self.assertEqual(result["counts"]["section_records"], 2)
                self.assertEqual(result["counts"]["node_trees"], 1)
                self.assertEqual(result["first_verified_example"]["priority"], "17")
            with self.subTest(consumer="replay"):
                tree, _, _ = select_tree(path, joaat("synthetic_orbit"))
                priorities = [value for name, _, value, _, _, _ in tree[1] if name == joaat("priority")]
                self.assertEqual(priorities, [17])

    def test_duplicate_id_attributes(self):
        with self.assertRaisesRegex(ValueError, "Duplicate state ID attribute"):
            self.run_probe(duplicate_attr=True)

    def test_parent_cycle(self):
        with self.assertRaisesRegex(ValueError, "parent index"):
            self.run_probe(self_parent=True)

    def test_non_label_state_id_is_malformed(self):
        # An integer or boolean id on a state is malformed, as resolve_script_path reports -3 for it.
        for kind in (3, 4):
            with self.subTest(kind=kind), self.assertRaisesRegex(ValueError, "Non-label state ID"):
                self.run_probe(id_kind=kind)

    def test_other_label_selects_nothing(self):
        result = self.run_probe(state="synthetic_other")
        self.assertEqual(result["counts"]["all_five_literals_verified"], 1)
        self.assertIsNone(result["first_verified_example"])

    def test_parent_must_be_a_state(self):
        counts = self.run_probe(parent_tag="zone")["counts"]
        self.assertNotIn("camera_parameter_sets", counts)
        self.assertNotIn("state_nodes", counts)

    def test_missing_camera_attribute(self):
        result = self.run_probe(drop=3)
        self.assertNotIn("camera_parameter_sets", result["counts"])
        self.assertIsNone(result["first_verified_example"])

    def test_root_with_parent(self):
        with self.assertRaisesRegex(ValueError, "parent index"):
            self.run_probe(root_parent=0)


class CommandLineTests(unittest.TestCase):
    def run_tool(self, script, *args):
        return subprocess.run([sys.executable, "-B", str(TOOLS / script), *map(str, args)],
                              capture_output=True, text=True)

    def test_damaged_input_is_reported_without_a_traceback(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            empty, text = root / "empty.bin", root / "text.bin"
            empty.write_bytes(b"")
            text.write_bytes(b"not an archive")
            for archive in (empty, text, root / "missing.bin"):
                for script, args in (("script_state_probe.py", ()),
                                     ("script_path_replay.py", ("--host", root / "host.dll",
                                                                "--record-with", "anchor", "path"))):
                    with self.subTest(archive=archive.name, script=script):
                        result = self.run_tool(script, archive, *args)
                        self.assertEqual(result.returncode, 1, result.stderr)
                        self.assertIn(archive.name, result.stderr)
                        self.assertNotIn("Traceback", result.stderr)

    def test_replay_failure_is_reported_without_a_traceback(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive = root / "replay.bin"
            archive.write_bytes(fixture(priorities=(9, 8)))
            result = self.run_tool("script_path_replay.py", archive, "--host", root / "host.dll",
                                   "--record-with", "synthetic_orbit", "a//b")
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertIn(archive.name, result.stderr)
            self.assertIn("Expected one root states container", result.stderr)
            self.assertNotIn("Traceback", result.stderr)
            self.assertEqual(result.stdout, "")

    def test_non_ascii_state_label_is_reported_without_a_traceback(self):
        with tempfile.TemporaryDirectory() as directory:
            archive = Path(directory) / "text.bin"
            archive.write_bytes(b"9FPR")
            for script, args in (("script_state_probe.py", ("--state", "café")),
                                 ("script_path_replay.py", ("--host", "host.dll", "--record-with", "café", "path"))):
                with self.subTest(script=script):
                    result = self.run_tool(script, archive, *args)
                    self.assertEqual(result.returncode, 1, result.stderr)
                    self.assertNotIn("Traceback", result.stderr)


if __name__ == "__main__":
    unittest.main()
