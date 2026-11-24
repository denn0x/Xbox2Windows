"""Compile-only check of the minimal DXC compile-time pair from #775.

condstore_N holds N conditional raw UAV stores and no sampling beyond the
fixture's two control stores; stores_N is the same source without the
branches. This check uses no wall-time threshold. It checks:

- the generated pair differs only by the branches and keeps the fixture
  declarations and control stores verbatim, and the recorded sizes stay fixed;
- the -ftime-report parser reads both the 4-column and the 3-column timer
  table (LLVM drops the System column when it is zero everywhere);
- the DXC under test emits DXIL for control, condstore_256 and stores_256;
- a forced timeout is recorded as `timeout` and an invalid profile as
  `diagnostic` (negative controls for the outcome classification);
- pass attribution as shares of the reported pass wall time: Global Value
  Numbering holds most of condstore_1024, little of stores_1024, and is
  absent with -opt-disable gvn, which still emits DXIL.

Usage: dxc_compile_time_check.py <dxc.exe>

Exit 77 (skip) when the DXC under test has no -ftime-report (it arrived after
DXC 1.6, so an older Windows SDK cannot report pass attribution at all) or when
it no longer shows the GVN profile: both are changes in the compiler, not a
failure of this repository. Every other mismatch fails.
"""

import json
from pathlib import Path
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import dxc_compile_time as tool  # noqa: E402

GVN = "Global Value Numbering"
SKIP = 77
FAILURES = []


def check(condition, message):
    if not condition:
        FAILURES.append(message)
        print(f"FAIL: {message}")
    return condition


def check_sources():
    for n in (1, 7, 256):
        cond = tool.variant_info(f"condstore_{n}")[2]
        flat = tool.variant_info(f"stores_{n}")[2]
        unwrapped = cond
        for k in range(n):
            unwrapped = unwrapped.replace(
                f"    if (((gl_WorkGroupID.x + {k}u) & 1u) != 0u)\n    {{\n        _4.Store(",
                "    _4.Store(", 1)
        unwrapped = unwrapped.replace(");\n    }\n", ");\n")
        check(unwrapped == flat, f"condstore_{n} differs from stores_{n} beyond its branches")
        for source, name in ((cond, "condstore"), (flat, "stores")):
            check(source.startswith(tool.FIXTURE_HEADER), f"{name}_{n} lost the fixture header")
            check(tool.CONTROL_STORES in source, f"{name}_{n} lost the control stores")
            check("SampleLevel" not in source.replace(tool.CONTROL_STORES, ""),
                  f"{name}_{n} samples outside the control stores")
        check(cond.count("_4.Store(") == n + 2, f"condstore_{n} store count")
    expected = {"control": 685, "samplebranch_8192": 1946457, "condstore_1024": 107095,
                "stores_1024": 41645, "condstore_16384": 1734047}
    for name, size in expected.items():
        got = len(tool.variant_info(name)[2].encode("utf-8"))
        check(got == size, f"{name} is {got} bytes, recorded {size}")
    for bad in ("condstore_0", "stores_65537", "condstores_4"):
        try:
            tool.variant_info(bad)
        except ValueError:
            continue
        check(False, f"{bad} was accepted")


def rejects_ftime(output):
    """True when the compiler itself reports -ftime-report as an unknown argument."""
    return "Unknown argument" in output and "-ftime-report" in output


def ftime_supported(dxc, work):
    hlsl = work / "probe.hlsl"
    hlsl.write_text(tool.control_source(), encoding="utf-8", newline="\n")
    flags = ["-T", tool.DEFAULT_TARGET, "-E", tool.DEFAULT_ENTRY, "-enable-16bit-types",
             "-ftime-report"]
    return not rejects_ftime(tool.run_once(dxc, hlsl, work / "probe.cso", flags, 120)["output"])


def check_parser():
    four = (";    0.2969 ( 78.6%)   0.0781 (100.0%)   0.3750 ( 83.3%)   0.3782 ( 83.0%)  "
            "Global Value Numbering\n"
            ";    0.3778 (100.0%)   0.0781 (100.0%)   0.4500 (100.0%)   0.4558 (100.0%)  Total\n")
    three = (";    0.0156 (100.0%)   0.0156 (100.0%)   0.0161 ( 99.0%)  Simplify the CFG\n"
             ";    0.0156 (100.0%)   0.0156 (100.0%)   0.0161 (100.0%)  Total\n"
             ";    0.0156 (100.0%)   0.0161 (100.0%)  two columns are not a timer row\n"
             "warning: not a report line\n")
    rows, total = tool.parse_ftime(four)
    check(total == {"name": "Total", "combined_s": 0.45, "wall_s": 0.4558}, f"4-column total {total}")
    check([row["name"] for row in rows] == [GVN] and rows[0]["combined_s"] == 0.375,
          f"4-column rows {rows}")
    rows, total = tool.parse_ftime(three)
    check(total == {"name": "Total", "combined_s": 0.0156, "wall_s": 0.0161}, f"3-column total {total}")
    check([row["name"] for row in rows] == ["Simplify the CFG"], f"3-column rows {rows}")
    # The capability probe must not turn a working compiler into a skip.
    check(rejects_ftime("dxc failed : Unknown argument: '-ftime-report'"),
          "an unknown -ftime-report argument was not detected")
    for quiet in ("", four, "dxc failed : Unknown argument: '-Fh'"):
        check(not rejects_ftime(quiet), f"a usable -ftime-report was detected as unsupported: {quiet[:40]!r}")


def run_tool(dxc, work, tag, variants, timeout, extra=(), target=None, ftime=False):
    out = work / f"{tag}.json"
    argv = ["--dxc", str(dxc), "--variants", variants, "--timeout", str(timeout),
            "--work-dir", str(work / tag), "--out", str(out)]
    argv += [f"--extra-arg={arg}" for arg in extra]
    if target:
        argv += ["--target", target]
    if ftime:
        argv.append("--ftime-report")
    tool.main(argv)
    return {record["variant"]: record for record in json.loads(out.read_text())["variants"]}


def gvn_share(dxc, work, name, extra=()):
    """GVN share of reported pass wall time, from all rows (not just the top passes)."""
    hlsl = work / f"share_{name}.hlsl"
    hlsl.write_text(tool.variant_info(name)[2], encoding="utf-8", newline="\n")
    flags = ["-T", tool.DEFAULT_TARGET, "-E", tool.DEFAULT_ENTRY, "-enable-16bit-types",
             *extra, "-ftime-report"]
    run = tool.run_once(dxc, hlsl, work / f"share_{name}.cso", flags, 600)
    rows, total = tool.parse_ftime(run["output"])
    if not check(run["outcome"] == "dxil", f"{name} {extra} outcome {run['outcome']}"):
        return None, None
    if not check(total is not None and total["wall_s"] > 0, f"{name} has no -ftime-report total"):
        return None, None
    gvn = [row for row in rows if row["name"] == GVN]
    share = 100.0 * sum(row["wall_s"] for row in gvn) / total["wall_s"]
    print(f"{name} {' '.join(extra)}: pass total {total['wall_s']:.3f} s, "
          f"GVN instances {len(gvn)}, GVN share {share:.1f}%")
    return share, len(gvn)


def check_dxc(dxc):
    with tempfile.TemporaryDirectory(prefix="dxc-compile-time-check-") as temp:
        work = Path(temp)
        if not ftime_supported(dxc, work):
            print("SKIP: this DXC has no -ftime-report; pass attribution cannot be measured")
            return SKIP
        records = run_tool(dxc, work, "outcomes", "control,condstore_256,stores_256", 120)
        for name, record in records.items():
            check(record["outcome"] == "dxil", f"{name} outcome {record['outcome']}")
        timeout = run_tool(dxc, work, "timeout", "condstore_1024", 0.001)["condstore_1024"]
        check(timeout["outcome"] == "timeout", f"forced timeout recorded as {timeout['outcome']}")
        bad = run_tool(dxc, work, "diagnostic", "control", 120, target="cs_9_9")["control"]
        check(bad["outcome"] == "diagnostic", f"invalid profile recorded as {bad['outcome']}")

        cond, _ = gvn_share(dxc, work, "condstore_1024")
        flat, _ = gvn_share(dxc, work, "stores_1024")
        off, off_instances = gvn_share(dxc, work, "condstore_1024", ("-opt-disable", "gvn"))
        if FAILURES or None in (cond, flat, off):
            return
        check(off_instances == 0, f"-opt-disable gvn still reports {off_instances} GVN rows")
        if not (cond >= 50.0 and flat < 25.0):
            print(f"SKIP: this DXC does not show the GVN profile (condstore {cond:.1f}%, "
                  f"stores {flat:.1f}%); re-measure before relying on the attribution")
            return SKIP
    return 0


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 2
    dxc = Path(argv[1])
    check_sources()
    check_parser()
    if not check(dxc.is_file(), f"DXC not found: {dxc}"):
        return 1
    print(f"DXC: {tool.dxc_version(dxc)}")
    status = check_dxc(dxc)
    if FAILURES:
        print(f"{len(FAILURES)} failure(s)")
        return 1
    if status == SKIP:
        return SKIP
    print("dxc_compile_time_check: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
