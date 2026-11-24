"""Synthetic PE controls for the offline import-routing report."""

import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools" / "compat_report.py"
sys.path.insert(0, str(ROOT / "tools"))
from import_routing import POLICY_VERSION, classify  # noqa: E402


SECTION_RVA = 0x1000
RAW_OFFSET = 0x200
RAW_SIZE = 0x1000
IMAGE_BASE = 0x180000000

POSITIVE_IMPORTS = [
    ("normal", "kernel32.dll", "ExitProcess"),
    ("normal", "winhttp.dll", "WinHttpOpen"),
    ("normal", "xmem.dll", "XMemAlloc"),
    ("normal", "xmem.dll", 7),
    # Synthetic ordinal: no checked-in xmem export table is consulted.
    ("normal", "xmem.dll", 65535),
    # Synthetic API-set name: the prefix alone selects the candidate route.
    ("normal", "api-ms-win-fixture-l1-1-0.dll", "FabricatedApi"),
    ("delay", "xgameruntime.dll", "XUserGetId"),
    ("delay", "xg_xs.dll", 3),
]
NEGATIVE_IMPORTS = POSITIVE_IMPORTS + [("normal", "mystery.dll", "MissingExport")]


def build_image(imports, *, tls=True, seh=True, load_config=True):
    """Build a source-owned PE32+ image with normal and delay imports."""
    data = bytearray(RAW_SIZE)
    cursor = 0

    def alloc(size, align):
        nonlocal cursor
        cursor = (cursor + align - 1) & ~(align - 1)
        rva = SECTION_RVA + cursor
        if cursor + size > len(data):
            raise AssertionError("synthetic image section overflow")
        cursor += size
        return rva

    def put(rva, blob):
        offset = rva - SECTION_RVA
        data[offset:offset + len(blob)] = blob

    def put_struct(rva, form, *values):
        put(rva, struct.pack(form, *values))

    module_rvas = {}
    for module in sorted({module for _, module, _ in imports}):
        raw = module.encode("ascii") + b"\0"
        rva = alloc(len(raw), 2)
        put(rva, raw)
        module_rvas[module] = rva

    hint_rvas = {}
    for kind, module, symbol in imports:
        if isinstance(symbol, int):
            continue
        raw = struct.pack("<H", 0) + symbol.encode("ascii") + b"\0"
        rva = alloc(len(raw), 2)
        put(rva, raw)
        hint_rvas[(kind, module, symbol)] = rva

    groups = {}
    for kind, module, symbol in imports:
        groups.setdefault((kind, module), []).append(symbol)

    tables = {}
    for key in sorted(groups):
        values = []
        for symbol in groups[key]:
            if isinstance(symbol, int):
                values.append((1 << 63) | symbol)
            else:
                values.append(hint_rvas[(key[0], key[1], symbol)])
        blob = b"".join(struct.pack("<Q", value) for value in [*values, 0])
        lookup = alloc(len(blob), 8)
        put(lookup, blob)
        iat = alloc(len(blob), 8)
        put(iat, blob)
        tables[key] = (lookup, iat)

    normal_groups = sorted(key for key in groups if key[0] == "normal")
    delay_groups = sorted(key for key in groups if key[0] == "delay")
    normal_rva = normal_size = delay_rva = delay_size = 0

    if normal_groups:
        normal_size = 20 * (len(normal_groups) + 1)
        normal_rva = alloc(normal_size, 4)
        for index, key in enumerate(normal_groups):
            lookup, iat = tables[key]
            put_struct(normal_rva + index * 20, "<5I",
                       lookup, 0, 0, module_rvas[key[1]], iat)

    if delay_groups:
        delay_size = 32 * (len(delay_groups) + 1)
        delay_rva = alloc(delay_size, 4)
        for index, key in enumerate(delay_groups):
            lookup, iat = tables[key]
            put_struct(delay_rva + index * 32, "<8I",
                       1, module_rvas[key[1]], 0, iat, lookup, 0, 0, 0)

    tls_rva = alloc(40, 8) if tls else 0
    seh_rva = alloc(12, 4) if seh else 0
    load_config_rva = alloc(0x70, 8) if load_config else 0

    pe_offset = 0x80
    buf = bytearray(RAW_OFFSET + RAW_SIZE)
    buf[0:2] = b"MZ"
    struct.pack_into("<I", buf, 60, pe_offset)
    buf[pe_offset:pe_offset + 4] = b"PE\0\0"
    struct.pack_into("<HHIIIHH", buf, pe_offset + 4,
                     0x8664, 1, 0, 0, 0, 240, 0x2022)
    opt = pe_offset + 24
    struct.pack_into("<HBBIIIIIQIIHHHHHHIIIIHHQQQQII", buf, opt,
                     0x20B, 0, 0, 0, RAW_SIZE, 0, 0, SECTION_RVA, IMAGE_BASE,
                     0x1000, 0x200, 6, 0, 0, 0, 6, 0, 0, 0x2000, RAW_OFFSET,
                     0, 3, 0, 0, 0, 0, 0, 0, 16)
    sec = opt + 240
    struct.pack_into("<8sIIIIIIHHI", buf, sec,
                     b".rdata\0\0", RAW_SIZE, SECTION_RVA, RAW_SIZE, RAW_OFFSET,
                     0, 0, 0, 0, 0x40000040)
    directories = opt + 112
    if normal_groups:
        struct.pack_into("<II", buf, directories + 1 * 8, normal_rva, normal_size)
    if seh:
        struct.pack_into("<II", buf, directories + 3 * 8, seh_rva, 12)
    if tls:
        struct.pack_into("<II", buf, directories + 9 * 8, tls_rva, 40)
    if load_config:
        struct.pack_into("<II", buf, directories + 10 * 8, load_config_rva, 0x70)
    if delay_groups:
        struct.pack_into("<II", buf, directories + 13 * 8, delay_rva, delay_size)

    buf[RAW_OFFSET:RAW_OFFSET + RAW_SIZE] = data
    return bytes(buf)


def patch_machine(image_bytes, machine):
    """Patch the PE COFF machine field; the fixture is otherwise unchanged."""
    data = bytearray(image_bytes)
    struct.pack_into("<H", data, 0x84, machine)
    return bytes(data)


def run_tool(image, out_dir, stem):
    json_path = Path(out_dir) / f"{stem}.json"
    markdown_path = Path(out_dir) / f"{stem}.md"
    process = subprocess.run(
        [sys.executable, "-B", str(TOOL), str(image),
         "--json", str(json_path), "--markdown", str(markdown_path)],
        capture_output=True, text=True)
    return process, json_path, markdown_path


def check_policy():
    assert POLICY_VERSION == 2
    assert classify("xmem.dll")[0] == "hle"
    assert classify("XMem.DLL")[0] == "hle"
    assert classify("d3d12_xs.dll")[0] == "hle"
    assert classify("xgameruntime.dll") == (
        "stub", "GDK runtime module; no implemented Xbox2Windows provider in this tree")
    assert classify("xg_xs.dll") == (
        "stub", "GDK console GPU module; no Xbox2Windows shim in this tree")
    assert classify("xaudio2_8.dll") == ("windows", "Windows system DLL")
    assert classify("xaudio2_9.dll") == ("windows", "Windows system DLL")
    for module in ("winhttp.dll", "mfplat.dll", "mfreadwrite.dll", "gameinput.dll", "GameInput.dll"):
        assert classify(module) == ("windows", "Windows system DLL"), module
    # Exact names only: a near miss or the GDK redistributable name keeps no route.
    assert classify("winhttp2.dll") == ("unsupported", "no matching policy entry")
    assert classify("gameinputredist.dll") == ("unsupported", "no matching policy entry")
    assert classify("kernel32.dll")[0] == "windows"
    assert classify("api-ms-win-core-file-l1-1-0.dll")[0] == "windows"
    assert classify("api-ms-win-fixture-l1-1-0.dll") == (
        "windows", "Windows API-set contract")
    assert classify("ext-ms-win-ntuser-window-l1-1-0.dll")[0] == "windows"
    assert classify("mystery.dll") == ("unsupported", "no matching policy entry")


def check_positive():
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        image = root / "positive.exe"
        image.write_bytes(build_image(POSITIVE_IMPORTS))
        first, json1, markdown1 = run_tool(image, root / "out" / "first", "compat-report")
        assert first.returncode == 0, first.stderr

        report = json.loads(json1.read_text(encoding="utf-8"))
        assert report["schema"] == "xbox2windows.report/1"
        assert set(report) == {"schema", "generated_by", "inputs", "sections", "attachments"}
        assert set(report["sections"]) == {"import_routing"}
        assert report["generated_by"] == {"tool": "compat_report", "version": 1}
        assert report["inputs"]["image"] == "positive.exe"
        assert report["attachments"] == []
        json_bytes = json1.read_bytes()
        markdown_bytes = markdown1.read_bytes()
        assert json_bytes.endswith(b"\n") and not json_bytes.endswith(b"\n\n")
        assert markdown_bytes.endswith(b"\n") and not markdown_bytes.endswith(b"\n\n")
        assert json_bytes.isascii()
        assert str(root) not in json1.read_text(encoding="utf-8")

        routing = report["sections"]["import_routing"]
        assert routing["policy_version"] == POLICY_VERSION
        assert "candidate" in routing["scope"].lower()
        assert "does not resolve" in routing["scope"].lower()
        assert "prove a provider" in routing["scope"].lower()
        assert routing["summary"] == {
            "images": 1,
            "imports": 8,
            "normal": 6,
            "delay": 2,
            "by_ordinal": 3,
            "routes": {"hle": 3, "windows": 3, "stub": 2, "unsupported": 0},
            "route_kind": "candidate",
            "resolution": "not_checked",
            "stub_rows": 2,
            "stub_providers": 0,
            "unknown": 0,
        }
        rows = [(row["kind"], row["module"], row["symbol"], row["route"])
                for row in routing["imports"]]
        assert rows == [
            ("normal", "api-ms-win-fixture-l1-1-0.dll", "FabricatedApi", "windows"),
            ("normal", "kernel32.dll", "ExitProcess", "windows"),
            ("normal", "winhttp.dll", "WinHttpOpen", "windows"),
            ("normal", "xmem.dll", "#7", "hle"),
            ("normal", "xmem.dll", "#65535", "hle"),
            ("normal", "xmem.dll", "XMemAlloc", "hle"),
            ("delay", "xg_xs.dll", "#3", "stub"),
            ("delay", "xgameruntime.dll", "XUserGetId", "stub"),
        ]
        assert routing["unknown"] == []

        for row in routing["imports"]:
            assert row["route_kind"] == "candidate"
            assert row["resolution"] == "not_checked"
            assert row["provider"] is None
            assert row["stub"] == (row["route"] == "stub")
            if row["route"] == "hle":
                assert row["candidate_provider"] == row["module"]
            else:
                assert row["candidate_provider"] is None

        absent_ordinal = next(row for row in routing["imports"]
                              if row["module"] == "xmem.dll" and row["symbol"] == "#65535")
        assert absent_ordinal["route"] == "hle"
        assert absent_ordinal["resolution"] == "not_checked"
        assert absent_ordinal["candidate_provider"] == "xmem.dll"

        fabricated_api = next(row for row in routing["imports"]
                              if row["module"] == "api-ms-win-fixture-l1-1-0.dll")
        assert fabricated_api["route"] == "windows"
        assert fabricated_api["resolution"] == "not_checked"
        assert fabricated_api["provider"] is None
        assert fabricated_api["candidate_provider"] is None

        stub_rows = [row for row in routing["imports"] if row["stub"]]
        assert len(stub_rows) == 2
        assert all(row["provider"] is None for row in stub_rows)

        features = routing["features"]["positive.exe"]
        assert features["machine"] == "IMAGE_FILE_MACHINE_AMD64"
        assert features["tls"]["present"] is True
        assert features["seh"]["present"] is True
        assert features["seh"]["entries"] == 1
        assert features["seh"]["trailing_bytes"] == 0
        assert features["load_config"]["present"] is True

        markdown = markdown1.read_text(encoding="utf-8")
        assert "## Unknown imports" in markdown
        assert "None." in markdown
        assert "Resolution: not checked" in markdown
        assert "Stub providers: 0" in markdown
        assert "| Stub rows | 2 |" in markdown
        assert str(root) not in markdown

        second, json2, markdown2 = run_tool(image, root / "out" / "second", "compat-report")
        assert second.returncode == 0, second.stderr
        assert json1.read_bytes() == json2.read_bytes()
        assert markdown1.read_bytes() == markdown2.read_bytes()


def check_negative():
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        image = root / "negative.exe"
        image.write_bytes(build_image(NEGATIVE_IMPORTS))
        process, json_path, markdown_path = run_tool(image, root / "out" / "negative", "compat-report")
        assert process.returncode == 1, process.stderr
        assert "unsupported: negative.exe -> mystery.dll!MissingExport" in process.stdout

        report = json.loads(json_path.read_text(encoding="utf-8"))
        routing = report["sections"]["import_routing"]
        assert routing["summary"]["routes"]["unsupported"] == 1
        assert routing["summary"]["unknown"] == 1
        row, = routing["unknown"]
        assert row == {
            "image": "negative.exe",
            "kind": "normal",
            "module": "mystery.dll",
            "symbol": "MissingExport",
            "route": "unsupported",
            "reason": "no matching policy entry",
            "route_kind": "candidate",
            "resolution": "not_checked",
            "provider": None,
            "stub": False,
            "candidate_provider": None,
        }
        assert markdown_path.exists()
        assert "mystery.dll" in markdown_path.read_text(encoding="utf-8")


def check_read_errors():
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        missing = root / "missing.exe"
        process, json_path, markdown_path = run_tool(missing, root / "out" / "missing", "compat-report")
        assert process.returncode == 2
        assert "compat_report:" in process.stderr
        assert "Traceback" not in process.stderr
        assert not json_path.exists() and not markdown_path.exists()

        invalid = root / "invalid.exe"
        invalid.write_bytes(b"not a PE image")
        process, json_path, markdown_path = run_tool(invalid, root / "out" / "invalid", "compat-report")
        assert process.returncode == 2
        assert "compat_report:" in process.stderr
        assert "Traceback" not in process.stderr
        assert not json_path.exists() and not markdown_path.exists()


def check_machine_scope():
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        for machine, name in ((0xAA64, "IMAGE_FILE_MACHINE_ARM64"),
                              (0x14C, "IMAGE_FILE_MACHINE_I386")):
            image = root / f"machine-{machine:x}.exe"
            image.write_bytes(patch_machine(build_image(POSITIVE_IMPORTS), machine))
            process, json_path, markdown_path = run_tool(
                image, root / "out" / f"machine-{machine:x}", "compat-report")
            assert process.returncode == 2, (machine, process.stdout, process.stderr)
            assert name in process.stderr
            assert hex(machine) in process.stderr
            assert not json_path.exists() and not markdown_path.exists()


def main():
    check_policy()
    check_positive()
    check_negative()
    check_read_errors()
    check_machine_scope()
    print("compat report synthetic checks passed")


if __name__ == "__main__":
    main()
