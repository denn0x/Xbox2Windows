"""Synthetic PE32+ controls for candidate import routing in intake and reports."""

from contextlib import redirect_stderr, redirect_stdout
import hashlib
import io
import json
from pathlib import Path
import struct
import sys
import tempfile
from unittest.mock import patch

import pefile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import intake_exe  # noqa: E402
from import_routing import ROUTE_KIND, ROUTES, RESOLUTION, classify  # noqa: E402
from pe_inventory import import_tables  # noqa: E402


SECTION_RVA = 0x1000
RAW_OFFSET = 0x200
RAW_SIZE = 0x1000
IMAGE_BASE = 0x180000000
CALLER = "main.exe"
IMPORTS = [
    ("normal", "xmem.dll", "XMemAlloc"),
    ("normal", "kernel32.dll", "ExitProcess"),
    ("delay", "xgameruntime.dll", "XUserGetId"),
    ("normal", "mystery.dll", "MissingExport"),
]


def build_image(imports):
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

    module_rvas = {}
    for module in sorted({module for _, module, _ in imports}):
        raw = module.encode("ascii") + b"\0"
        rva = alloc(len(raw), 2)
        put(rva, raw)
        module_rvas[module] = rva

    hint_rvas = {}
    for kind, module, symbol in imports:
        raw = struct.pack("<H", 0) + symbol.encode("ascii") + b"\0"
        rva = alloc(len(raw), 2)
        put(rva, raw)
        hint_rvas[(kind, module, symbol)] = rva

    groups = {}
    for kind, module, symbol in imports:
        groups.setdefault((kind, module), []).append(symbol)

    tables = {}
    for key in sorted(groups):
        values = [hint_rvas[(key[0], key[1], symbol)] for symbol in groups[key]]
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
            struct.pack_into("<5I", data, normal_rva - SECTION_RVA + index * 20,
                             lookup, 0, 0, module_rvas[key[1]], iat)
    if delay_groups:
        delay_size = 32 * (len(delay_groups) + 1)
        delay_rva = alloc(delay_size, 4)
        for index, key in enumerate(delay_groups):
            lookup, iat = tables[key]
            struct.pack_into("<8I", data, delay_rva - SECTION_RVA + index * 32,
                             1, module_rvas[key[1]], 0, iat, lookup, 0, 0, 0)

    pe_offset = 0x80
    buf = bytearray(RAW_OFFSET + RAW_SIZE)
    buf[0:2] = b"MZ"
    struct.pack_into("<I", buf, 60, pe_offset)
    buf[pe_offset:pe_offset + 4] = b"PE\0\0"
    # Executable and large-address-aware, without IMAGE_FILE_SYSTEM.
    struct.pack_into("<HHIIIHH", buf, pe_offset + 4, 0x8664, 1, 0, 0, 0, 240, 0x0022)
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
        struct.pack_into("<II", buf, directories + 8, normal_rva, normal_size)
    if delay_groups:
        struct.pack_into("<II", buf, directories + 104, delay_rva, delay_size)
    buf[RAW_OFFSET:RAW_OFFSET + RAW_SIZE] = data
    return bytes(buf)


def check_route_rows(image_bytes):
    rows = intake_exe.import_routes(image_bytes, CALLER)
    assert len(rows) == len(IMPORTS)
    by_module = {row["dll"]: row for row in rows}
    assert set(by_module) == {module for _, module, _ in IMPORTS}
    assert {row["route"] for row in rows} == set(ROUTES)
    assert by_module["xmem.dll"]["route"] == "hle"
    assert by_module["kernel32.dll"]["route"] == "windows"
    assert by_module["xgameruntime.dll"]["route"] == "stub"
    assert by_module["mystery.dll"]["route"] == "unsupported"
    assert {row["kind"] for row in rows} == {"normal", "delay"}

    keys = {"image", "kind", "dll", "symbol", "route", "reason", "route_kind",
            "resolution", "provider", "stub", "candidate_provider"}
    for row in rows:
        assert set(row) == keys
        assert row["image"] == CALLER
        assert row["route_kind"] == ROUTE_KIND == "candidate"
        assert row["resolution"] == RESOLUTION == "not_checked"
        assert row["provider"] is None
        assert row["stub"] == (row["route"] == "stub")
        assert row["candidate_provider"] == (row["dll"] if row["route"] == "hle" else None)
        assert "desktop_resolution" not in row and "package_resolution" not in row
    assert by_module["mystery.dll"]["reason"] == "no matching policy entry"
    assert classify("XMem.DLL") == ("hle", "Xbox2Windows builds src/xmem_shim.cpp as xmem.dll")


def package_fixture(tmp, image_bytes):
    root = (tmp / "fixture-build").resolve()
    root.mkdir()
    (root / CALLER).write_bytes(image_bytes)
    (root / "MicrosoftGame.Config").write_text(
        '<Game><ExecutableList><Executable TargetDeviceFamily="Scarlett" Name="main.exe"/>'
        '</ExecutableList></Game>', encoding="utf-8")
    (root / "layout.xml").write_text(
        '<Package><Chunk Id="1001"><FileGroup DestinationPath="/" Include="main.exe"/>'
        '</Chunk></Package>', encoding="utf-8")
    torrent_bytes = b"synthetic torrent fixture"
    (root / "fixture.torrent").write_bytes(torrent_bytes)
    integrity = tmp / "integrity.json"
    integrity.write_text(json.dumps({
        "directory": root.name,
        "torrent_payload_passed": True,
        "torrent": "fixture.torrent",
        "torrent_sha256": hashlib.sha256(torrent_bytes).hexdigest(),
        "layout": {"source": "layout.xml", "absent_from_torrent": {CALLER: ["1001"]}},
        "pieces": {"matched": 1},
        "finished_utc": "2026-01-01T00:00:00+00:00",
    }), encoding="utf-8")
    baseline = tmp / "baseline.json"
    baseline.write_text(json.dumps({
        "schema": 1, "images": {}, "imports": [], "unresolved_edges": []}),
        encoding="utf-8")
    return root, baseline, integrity


def synthetic_inventory(image_bytes):
    with pefile.PE(data=image_bytes, fast_load=True) as pe:
        imports = import_tables(pe, CALLER)
    rows = []
    for kind, dll, symbol in imports:
        rows.append({
            "image": CALLER, "kind": kind, "dll": dll, "symbol": symbol,
            "bundle_exports_in": [], "desktop_provider": None,
            "desktop_export": "provider_not_found", "desktop_candidate": None,
            "desktop_resolution": {
                "status": "provider_not_found",
                "chain": [{"dll": dll, "provider": None, "symbol": symbol}],
            },
            "package_resolution": None,
        })
    return {
        "schema": 1,
        "images": {CALLER: {
            "sha256": hashlib.sha256(image_bytes).hexdigest(),
            "exports": [],
        }},
        "imports": rows,
        "unresolved_edges": [dict(row) for row in rows],
        "summary": {},
    }


def run_intake(args):
    out, err = io.StringIO(), io.StringIO()
    with redirect_stdout(out), redirect_stderr(err):
        code = intake_exe.main(args)
    return code, out.getvalue(), err.getvalue()


def check_intake(image_bytes):
    with tempfile.TemporaryDirectory() as tmp_name:
        tmp = Path(tmp_name)
        root, baseline, integrity = package_fixture(tmp, image_bytes)
        inventory = synthetic_inventory(image_bytes)

        def fake_scan(package, *, extra_image=None, system=None):
            assert package == root
            assert extra_image == (CALLER, root / CALLER)
            return json.loads(json.dumps(inventory))

        common = ["--build", str(root), "--baseline", str(baseline),
                  "--integrity", str(integrity)]
        with patch.object(intake_exe, "scan", fake_scan):
            code, _, err = run_intake([*common, "--reports", str(tmp / "default")])
        assert code == 0, err
        report, = (tmp / "default").glob("*.inspected.json")
        report = json.loads(report.read_text(encoding="utf-8"))
        rows = report["inventory"]["imports"]
        assert {(row["kind"], row["dll"], row["symbol"], row["route"]) for row in rows} == {
            (kind, dll, symbol, classify(dll)[0]) for kind, dll, symbol in IMPORTS}
        for row in rows:
            assert row["route_kind"] == "candidate"
            assert row["resolution"] == "not_checked"
            assert row["provider"] is None
            assert row["stub"] == (row["route"] == "stub")
            assert row["candidate_provider"] == (row["dll"] if row["route"] == "hle" else None)
        gaps = {(row["dll"], row["symbol"]): row for row in report["entry_import_gaps"]}
        assert set(gaps) == {(dll, symbol) for _, dll, symbol in IMPORTS}
        assert all(row["route"] == classify(row["dll"])[0] for row in gaps.values())

        # The default report still succeeds; --require-routing rejects the unsupported row.
        with patch.object(intake_exe, "scan", fake_scan):
            code, _, err = run_intake([*common, "--require-routing",
                                       "--reports", str(tmp / "required")])
        assert code == 1, (code, err)
        assert "unsupported: main.exe -> mystery.dll!MissingExport" in err
        assert err.count("unsupported:") == 1, err
        assert all(part in err for part in ("main.exe", "mystery.dll", "MissingExport"))
        required, = (tmp / "required").glob("*.inspected.json")
        assert json.loads(required.read_text(encoding="utf-8"))["status"] == "inspected"


def main():
    image_bytes = build_image(IMPORTS)
    check_route_rows(image_bytes)
    check_intake(image_bytes)
    print("intake routing synthetic checks passed")


if __name__ == "__main__":
    main()
