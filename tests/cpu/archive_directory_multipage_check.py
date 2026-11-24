"""Check full 16-page zlib destinations and the two BC7 pages selected for drawing."""
import os
from pathlib import Path
import struct
import subprocess
import sys
from tempfile import TemporaryDirectory
import zlib

from archive_directory_gpu_check import runners
from startup_storage_directory_batch import PAGE, write_intervals
from startup_xg_bc_chain_check import block
from texture_descriptors import iter_boundaries, read_tables

GUARD = bytes([0xa5]) * 32


def compare(mapper, native, lifted, provider):
    colors = [bytes((8 + 12 * i, 16, 48, 254)) for i in range(17)]
    pages = [block(98, color) * 256 for color in colors]
    records = (b"".join(pages[:16]), pages[16])
    commands = runners(mapper, native, lifted, provider)
    env = dict(os.environ, X2W_XMEM_PERMISSIVE="1")
    adapter = None
    with TemporaryDirectory() as temp:
        for case, indices, damaged in (("first", (0, 16), None), ("middle", (7, 16), None),
                                       ("last", (15, 16), None), ("reverse", (16, 8), None),
                                       ("outside", (8, 17), None), ("truncated-first", (7, 16), 0),
                                       ("truncated-second", (15, 16), 1)):
            directory = Path(temp) / case
            directory.mkdir()
            path = directory / "fixture.ttpf"
            streams = [zlib.compress(data, 9) for data in records]
            if damaged is not None:
                streams[damaged] = streams[damaged][:-4]  # Remove the RFC 1950 checksum, preserving the directory.
            write_intervals(path, 1, list(zip(streams, (16, 1), strict=True)))
            with path.open("rb") as f:
                _, start, following, end = iter_boundaries(read_tables(f))
            entries = (start, following)
            assert following.logical - start.logical == len(records[0]) and end.logical - following.logical == PAGE
            inputs = struct.pack("<IIQI", *(start.logical // PAGE + i for i in indices), 0, 0)
            for runner, command in commands.items():
                work = directory / runner
                work.mkdir()
                run = subprocess.run(command, cwd=work, env=env, input=inputs, capture_output=True, timeout=12)
                if run.returncode == 77 and runner == "native" and env.get("X2W_SHIM_DRAW_ADAPTER") == "hardware":
                    return 77
                expected_exit = 3 if case == "outside" else 6 if damaged is not None else 0
                assert run.returncode == expected_exit, (runner, case, run.returncode, run.stderr)
                if case == "outside":
                    assert not list(work.iterdir()), (runner, case, "unexpected queue output")
                    continue
                trace = (work / "ranges.txt").read_text().splitlines()
                expected_trace = [f"file {path.stat().st_ino & 0xffffffffffffffff:016x}"]
                statuses, full, selected = [], bytearray(), bytearray()
                for i in indices:
                    record = int(i >= 16)
                    offset = (i - (16 if record else 0)) * PAGE
                    expected_trace.append(f"request {record+1} {entries[record].stored} {len(streams[record])} "
                                          f"{len(records[record])} {offset}")
                    failed = record == damaged
                    statuses.append("status 80004005" if failed else "status 00000000")
                    output = bytes([0xa5]) * len(records[record]) if failed else records[record]
                    full += GUARD + output + GUARD
                    selected += output[offset:offset + PAGE]
                assert trace[:5] == expected_trace + statuses, (runner, case, trace)
                assert (work / "destinations.bin").read_bytes() == full, (runner, case, "decoded record or guard")
                assert (work / "pages.bin").read_bytes() == selected, (runner, case, "selected pages")
                assert trace[-1] == "storage release 0 0 0", (runner, case, "storage references")
                pixels = work / "pixels.bin"
                if damaged is None:
                    if adapter is None:
                        adapter = trace[5]
                    assert adapter.startswith("adapter ") and trace[5:-1] == [adapter, "fence 1", "release 0"]
                    assert pixels.read_bytes() == b"".join(colors[i] * 8192 for i in indices), (runner, case, "RGBA")
                else:
                    assert len(trace) == 6 and not pixels.exists(), (runner, case, "unexpected draw")
            print(f"{case}: native, mapped and AOT match")
    print(adapter)
    return 0


if __name__ == "__main__":
    sys.exit(compare(*[Path(arg).resolve() for arg in sys.argv[1:]]))
