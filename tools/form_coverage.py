"""Count which instruction forms the translation tests cover.

Usage:
    ctest --preset dev --no-tests=error --output-junit <repo>/out/test-results.xml
    python tools/form_coverage.py out/test-results.xml [--records DIR] [--json out/coverage.json]

This measures instruction-form coverage only: which forms from a reference list appear in a
source-built fixture whose translation test passed. It is not a measure of how complete a port is;
covering a form in one fixture does not prove every operand variant or any title behavior.

Rules:
- A fixture's forms are covered only when its test passed. A skipped or failed test (or one that is
  not registered) leaves them uncovered; forms held back only by a skip are listed separately.
- x64: the mnemonics of a tests/cpu/*.asm source count for the test CPU_FIXTURES names. An unlisted
  .asm is never counted; it is reported as unclassified so it can be added, without failing the run.
- gfx: only programs the translator accepted count. tests/pixel_aot_check.py records the first word
  of each line of every program tools/pixel_aot.py lowered without an error, in the file CTest names
  in FORM_COVERAGE_RECORD (<build>/form_coverage/<test>.txt). Programs a check expects to be rejected
  raise before they are recorded. A record not written during the JUnit file's run is ignored, so a
  record and a test status always come from the same run.
- Reference lists (denominators) live in docs/coverage/ and are produced by tools/form_reference.py.
Python standard library only.
"""
import argparse
from datetime import datetime
import json
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REFERENCE = ROOT / "docs" / "coverage"
RECORDS = ROOT / "out" / "build" / "dev" / "form_coverage"  # CMakePresets.json "dev" binaryDir

# The test that proves each CPU fixture was translated. Fixtures that run unchanged (not translated)
# map to None: their forms are known but never covered. An unlisted .asm is reported as unclassified.
CPU_FIXTURES = {
    "branch": "cpu_leaf_aot_branches",
    "direct": "cpu_aot_direct_call",
    "chained": "cpu_aot_chained_ranges",
    "callback": "cpu_aot_object_callback",
    "x87_data": "cpu_aot_x87_data",
    "x87_state": None,       # native measurement harness for x87_data
    "destroy": "cpu_aot_destroy_contract",
    "create_context": "cpu_aot_context_lifecycle",
    "bink_heap": "cpu_aot_bink_heap",
    "heap_layout": "cpu_aot_heap_layout",
    "shift_count": "cpu_aot_shift_count",
    "version_query": "cpu_aot_version_query",
    "bink_descriptors": "cpu_bink_descriptors",
    "switch": "cpu_aot_jump_tables",
    "switch_addr32": "cpu_aot_switch_addr32",
    "fourth_arg": "cpu_aot_fourth_argument",
    "repeat_call": "cpu_aot_repeated_call",
    "working_set_delta": "cpu_working_set_delta",
    "exe_entry": "cpu_aot_exe_entry",
    "address_size": "cpu_aot_address_size",
    "call_pair": "cpu_aot_call_pair",
    "bmi1": "cpu_aot_bmi1",
    "register_probe": None,  # measurement harness
    "two_imports": None,     # mapped and run unchanged by pe_map
    "exe_start": None,       # mapped and run unchanged by pe_map
    "reloc_route": None,     # mapped and run unchanged by pe_map
    "seh_route": None,       # mapped and run unchanged by pe_map
    "tls_route": None,       # mapped and run unchanged by pe_map
}

MASM_DIRECTIVES = {"ifdef", "ifndef", "if", "else", "elseif", "endif", "end", "public", "extern",
                   "externdef", "include", "includelib", "option", "align", "even", "org", "assume", "local"}
MASM_DEFINERS = {"proc", "endp", "label", "equ", "db", "dw", "dd", "dq", "byte", "word", "dword",
                 "qword", "segment", "ends", "struct", "macro", "endm", "textequ"}
MASM_PREFIXES = {"lock", "rep", "repe", "repz", "repne", "repnz"}
# MASM condition-code spellings -> iced-x86's canonical mnemonic suffix.
CONDITION_ALIASES = {"z": "e", "nz": "ne", "c": "b", "nae": "b", "nc": "ae", "nb": "ae", "nbe": "a",
                     "na": "be", "nge": "l", "nl": "ge", "nle": "g", "ng": "le", "pe": "p", "po": "np"}


def masm_mnemonics(text):
    """Instruction mnemonics in MASM source; both arms of IFDEF/ELSE count (both variants are built)."""
    found = set()
    macros = set()
    for line in text.splitlines():
        line = re.sub(r"^\s*\w+::?", "", line.split(";", 1)[0]).strip()  # local or global label
        words = line.lower().split()
        if len(words) > 1 and words[1] == "macro":
            macros.add(words[0])
            continue
        if (not words or words[0].startswith(".") or words[0] in MASM_DIRECTIVES | MASM_DEFINERS | macros
                or (len(words) > 1 and words[1] in MASM_DEFINERS and words[2:3] != ["ptr"])):
            continue
        while words and words[0] in MASM_PREFIXES:
            words = words[1:]
        if not words:
            continue
        found.add(x64_canonical(words[0]))
    return found


def x64_canonical(mnemonic):
    for prefix in ("cmov", "set", "j"):
        if mnemonic.startswith(prefix) and mnemonic[len(prefix):] in CONDITION_ALIASES:
            return prefix + CONDITION_ALIASES[mnemonic[len(prefix):]]
    return mnemonic


def gfx_forms(mnemonics, reference):
    """Reference forms a fixture mnemonic selects: _e32 is the plain encoding, _e64 is VOP3."""
    wanted = {m if m.endswith("_e64") else m.removesuffix("_e32") for m in mnemonics}
    return {form for form in reference if form[1] in wanted}


def x64_forms(mnemonics, reference):
    return {form for form in reference if form[1] in mnemonics}


def load_reference(name):
    lines = (REFERENCE / name).read_text(encoding="utf-8").splitlines()
    return {tuple(line.split("\t")) for line in lines if line and not line.startswith("#")}


def read_results(junit):
    """CTest JUnit status per test (pass, fail or skip; CTest writes skips as status="notrun"), and the
    run's (start, end) in seconds since the epoch. CTest writes the start as local time in whole seconds."""
    suite = ET.parse(junit).getroot()
    status = {"run": "pass", "fail": "fail", "notrun": "skip"}
    results = {}
    for case in suite.iter("testcase"):
        state = status.get(case.get("status"), "fail")
        if case.find("failure") is not None or case.find("error") is not None:
            state = "fail"
        elif state == "pass" and case.find("skipped") is not None:
            state = "skip"
        results[case.get("name")] = state
    start = datetime.fromisoformat(suite.get("timestamp")).timestamp()
    return results, (start - 1, start + float(suite.get("time")) + 2)


def read_records(directory, window):
    """{test: first words of the accepted program lines} from records written within window."""
    start, end = window
    return {path.stem: set(path.read_text(encoding="utf-8").split())
            for path in sorted(Path(directory).glob("*.txt")) if start <= path.stat().st_mtime <= end}


def fixture_status(results, tests):
    """pass if any listed test passed, skip if none passed but one skipped, otherwise fail/absent."""
    states = [results.get(t) for t in tests if t]
    if "pass" in states:
        return "pass"
    return "skip" if "skip" in states else "fail" if "fail" in states else "absent"


def summarize(reference, fixtures, select):
    """fixtures: {name: (mnemonics, status)}. Returns covered/known counts and per-group detail."""
    covered = set().union(set(), *(select(m, reference) for m, s in fixtures.values() if s == "pass"))
    skipped = set().union(set(), *(select(m, reference) for m, s in fixtures.values() if s == "skip")) - covered
    known = set().union(set(), *(select(m, reference) for m, _ in fixtures.values()))
    groups = {}
    for group, form in sorted(reference):
        g = groups.setdefault(group, {"covered": 0, "total": 0})
        g["total"] += 1
        g["covered"] += (group, form) in covered
    unmatched = sorted(m for m in set().union(set(), *(m for m, _ in fixtures.values()))
                       if not select({m}, reference))
    return {"covered": len(covered), "total": len(reference), "known_in_fixtures": len(known),
            "blocked_by_skip": sorted(f"{g} {f}" for g, f in skipped), "groups": groups,
            "fixture_forms_not_in_reference": unmatched}


def measure(results, root=ROOT, records=None):
    """results: {test: status}. root: the repository whose tests/cpu is scanned (the check passes a
    temporary one). records: {test: accepted gfx mnemonics} from read_records."""
    cpu = {}
    for asm in sorted((root / "tests" / "cpu").glob("*.asm")):
        if asm.stem not in CPU_FIXTURES:
            status = "unclassified"
        else:
            test = CPU_FIXTURES[asm.stem]
            status = fixture_status(results, [test]) if test else "not translated"
        cpu[asm.name] = (masm_mnemonics(asm.read_text(encoding="utf-8")), status)
    shaders = {test: (forms, fixture_status(results, [test])) for test, forms in (records or {}).items()}
    x64 = load_reference("x64_zen2.tsv")
    fixtures = {name: status for name, (_, status) in {**cpu, **shaders}.items()}
    return {
        "note": "Instruction-form coverage of source-built fixtures; not a measure of port completion.",
        "tests": {s: sum(v == s for v in results.values()) for s in ("pass", "fail", "skip")},
        "fixtures": fixtures,
        "unclassified": sorted(name for name, status in fixtures.items() if status == "unclassified"),
        "shader_records": sorted(shaders),
        "x64_zen2": summarize(x64, cpu, x64_forms),
        "gfx1013": summarize(load_reference("gfx1013.tsv"), shaders, gfx_forms),
        "gfx1030": summarize(load_reference("rdna2.tsv"), shaders, gfx_forms),
    }


def report(m):
    t = m["tests"]
    lines = [m["note"], f"Tests: {t['pass']} passed, {t['fail']} failed, {t['skip']} skipped.", ""]
    for key, label in (("x64_zen2", "x64 (Zen 2 user-mode mnemonics)"), ("gfx1013", "gfx1013 forms"),
                       ("gfx1030", "gfx1030 (RDNA2) forms")):
        r = m[key]
        lines.append(f"{label}: {r['covered']} of {r['total']} covered "
                     f"({r['known_in_fixtures']} appear in fixtures; {len(r['blocked_by_skip'])} held back by skips)")
    lines += ["", "Shader translation records from this run: "
              + (", ".join(m["shader_records"]) or "none, so no gfx form is covered")]
    if m["unclassified"]:
        lines.append("Not counted until classified in tools/form_coverage.py: " + ", ".join(m["unclassified"]))
    return "\n".join(lines)


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("junit", type=Path)
    parser.add_argument("--records", type=Path, default=RECORDS,
                        help="directory of shader translation records (default: %(default)s)")
    parser.add_argument("--json", type=Path, help="also write the full result, per group, as JSON")
    args = parser.parse_args(argv)
    results, window = read_results(args.junit)
    m = measure(results, records=read_records(args.records, window))
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(m, indent=1) + "\n", encoding="utf-8")
    print(report(m))


if __name__ == "__main__":
    main(sys.argv[1:])
