"""Check that require_warp_results.py requires every SPIR-V bridge WARP test that CMake registers.

The bridge names are read from the foreach(bridge IN ITEMS ...) loop in CMakeLists.txt. The result
check is then run on synthetic CTest JUnit reports: every required test passed, and each required
test in turn absent, skipped or failed.
"""
import contextlib
import io
from pathlib import Path
import re
import sys
import tempfile

HERE = Path(__file__).resolve().parent
CMAKE = HERE.parents[1] / "CMakeLists.txt"
sys.path.insert(0, str(HERE))
import require_warp_results

CASES = {
    "passed": '<testcase name="{0}" classname="{0}" time="0.2" status="run"><system-out/></testcase>',
    "skipped": '<testcase name="{0}" classname="{0}" time="0.1" status="notrun">'
               '<skipped message="SKIP_RETURN_CODE=77"/><system-out/></testcase>',
    "failed": '<testcase name="{0}" classname="{0}" time="0.1" status="fail">'
              '<failure message=""/><system-out/></testcase>',
}


def registered():
    loops = re.findall(r"foreach\(bridge IN ITEMS\s([^)]*)\)", CMAKE.read_text(encoding="utf-8"))
    if len(loops) != 1:
        sys.exit(f"CMakeLists.txt: expected one foreach(bridge IN ITEMS ...) loop, found {len(loops)}")
    items = loops[0].split()
    bad = [item for item in items if not re.fullmatch(r"\w+:\w+", item)]
    if bad or not items:
        sys.exit(f"CMakeLists.txt: the bridge loop has no name:driver items or malformed ones: {bad}")
    return [f"spirv_{item.split(':')[0]}_warp" for item in items]


def run(directory, states):
    report = directory / "junit.xml"
    cases = "".join(CASES[state].format(name) for name, state in states.items())
    report.write_text(f'<?xml version="1.0" encoding="UTF-8"?>\n<testsuite name="(empty)">{cases}</testsuite>\n',
                      encoding="utf-8")
    output = io.StringIO()
    with contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
        code = require_warp_results.main([str(report)])
    return code, output.getvalue()


def main():
    required = require_warp_results.REQUIRED
    tests = registered()
    missing = sorted(set(tests) - set(required))
    stale = sorted(set(required) - set(tests))
    if missing or stale or len(required) != len(set(required)):
        sys.exit(f"require_warp_results.py: REQUIRED does not match the bridge loop in CMakeLists.txt: "
                 f"missing {missing}, not registered {stale}, {len(required)} entries for {len(tests)} tests")
    with tempfile.TemporaryDirectory() as tmp:
        directory = Path(tmp)
        passed = dict.fromkeys(required, "passed")
        code, text = run(directory, passed)
        if code != 0 or f"all {len(required)} SPIR-V bridge WARP tests ran and passed" not in text:
            sys.exit(f"a report with every required test passed returned {code}:\n{text}")
        for name in required:
            for state in (None, "skipped", "failed"):
                states = {**passed, name: state} if state else {key: "passed" for key in required if key != name}
                code, text = run(directory, states)
                if code != 1 or f"{name}: {state or 'not in the report'}\n" not in text:
                    sys.exit(f"{name} {state or 'absent'}: returned {code}, expected 1 with the test named:\n{text}")
    print(f"require_warp_results: {len(required)} required tests match CMakeLists.txt; "
          "absent, skipped and failed reports are rejected")


if __name__ == "__main__":
    main()
