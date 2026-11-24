"""Exercise the named ballot profile before HLSL compilation and at GPU readback."""
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from spirv_ballot_extract import lower_ballot_extract


def check_ballot_profile(original):
    extract = "%bit = OpGroupNonUniformBallotBitExtract %bool %scope %mask %i"
    assert original.splitlines().count(extract) == 1
    for instruction in (extract.replace("%bool", "%uint"), extract + " %one"):
        probe = original.replace(extract, instruction)
        assert instruction in lower_ballot_extract(probe).splitlines(), instruction

    reserved = original.replace("%one", "%x2w_ballot_existing")
    try:
        lower_ballot_extract(reserved)
    except ValueError as error:
        assert str(error) == "Reserved ballot lowering ID already in use", error
    else:
        raise AssertionError("accepted reserved ballot lowering ID")


SENTINEL = 0xc5c5c5c5
NONE = 0xffffffff


def bit_vector(name, position, dead="%never"):
    return [f"%{name}_hi = OpShiftRightLogical %uint {position} %k5", f"%{name}_lo = OpBitwiseAnd %uint {position} %k31",
            f"%{name}_sh = OpShiftLeftLogical %uint %k1 %{name}_lo", f"%{name}_bit = OpSelect %uint {dead} %k0 %{name}_sh",
            f"%{name} = OpVectorInsertDynamic %vec %empty %{name}_bit %{name}_hi"]


def find_lsb_source(header):
    """Thread i ballots bit i (word i) and, unless i % 8 == 7, bits 5i+3 and 7i+11 (word 128 + i)."""
    header = header.split("%main = OpFunction")[0] + "%empty = OpConstantNull %vec\n%never = OpConstantFalse %bool\n" + "".join(
        f"%k{value} = OpConstant %uint {value}\n" for value in (0, 1, 2, 3, 5, 7, 11, 31, 127, 128))
    body = ["%main = OpFunction %void None %fn", "%label = OpLabel", "%ids = OpLoad %xyz %gid",
            "%i = OpCompositeExtract %uint %ids 0", "%mod = OpBitwiseAnd %uint %i %k7", "%dead = OpIEqual %bool %mod %k7"]
    body += bit_vector("va", "%i")
    for name, scale, add in (("vp", 5, 3), ("vq", 7, 11)):
        body += [f"%{name}_m = OpIMul %uint %i %k{scale}", f"%{name}_s = OpIAdd %uint %{name}_m %k{add}",
                 f"%{name}_pos = OpBitwiseAnd %uint %{name}_s %k127"] + bit_vector(name, f"%{name}_pos", "%dead")
    # The unused bit extraction makes one module need both lowerings' constants.
    body += ["%vb = OpBitwiseOr %vec %vp %vq", "%unused = OpGroupNonUniformBallotBitExtract %bool %scope %va %i",
             "%lsb_a = OpGroupNonUniformBallotFindLSB %uint %scope %va",
             "%lsb_b = OpGroupNonUniformBallotFindLSB %uint %scope %vb",
             "%dst_a = OpAccessChain %ptr %out %k0 %i", "OpStore %dst_a %lsb_a",
             "%j = OpIAdd %uint %i %k128", "%dst_b = OpAccessChain %ptr %out %k0 %j", "OpStore %dst_b %lsb_b",
             "OpReturn", "OpFunctionEnd"]
    return header + "\n".join(body) + "\n"


def lowest_bit(words):
    return next((n for n in range(128) if words[n // 32] >> (n % 32) & 1), NONE)


def ballots(thread):
    def words(*positions):
        value = [0] * 4
        for position in positions:
            value[position // 32] |= 1 << position % 32
        return value
    return words(thread), ([0] * 4 if thread % 8 == 7 else words((5 * thread + 3) % 128, (7 * thread + 11) % 128))


def find_lsb_expected(lowest=lowest_bit):
    pairs = [ballots(thread) for thread in range(128)]
    return [lowest(a) for a, _ in pairs] + [lowest(b) for _, b in pairs] + [SENTINEL] * 64


def emulated_lowering(offsets=(0, 32, 64, 96), order=(0, 1, 2, 3), highest=False, arithmetic=True):
    """What the lowered module computes, with one deliberate fault per negative control."""
    def candidate(words, k):
        word = words[order[k]]
        index = (word.bit_length() - 1 if highest else (word & -word).bit_length() - 1) & NONE if word else NONE
        return index if k == 0 else (index + offsets[k]) & NONE | (NONE if arithmetic and index == NONE else index >> 31)
    return lambda words: min(candidate(words, k) for k in range(4))


FAULTS = {  # name: (pattern, replacement, matches, lowering model)
    "wrong offset": (r"(_l2) %x2w_ballot_c64", r"\1 %x2w_ballot_c32", 2, emulated_lowering(offsets=(0, 32, 32, 96))),
    "swapped words": (r"(_w[12] = OpCompositeExtract %uint %\w+) ([12])", lambda m: f"{m[1]} {3 - int(m[2])}", 4,
                      emulated_lowering(order=(0, 2, 1, 3))),
    "highest bit": (r"FindILsb", "FindUMsb", 8, emulated_lowering(highest=True)),
    "no empty fill": (r"OpShiftRightArithmetic", "OpShiftRightLogical", 6, emulated_lowering(arithmetic=False)),
}


def check_find_lsb_profile(original):
    lowered = lower_ballot_extract(original)
    lines = lowered.splitlines()
    assert "OpGroupNonUniformBallotFindLSB" not in lowered
    assert lines.count('%x2w_ballot_glsl = OpExtInstImport "GLSL.std.450"') == 1
    imported = original.replace("OpMemoryModel", '%glsl = OpExtInstImport "GLSL.std.450"\nOpMemoryModel', 1)
    reused = lower_ballot_extract(imported)
    assert reused.count("OpExtInstImport") == 1 and "%x2w_ballot_glsl" not in reused
    assert "OpExtInst %uint %glsl FindILsb %x2w_ballot_lsb_a_w0" in reused

    wrong_type = original.replace("%lsb_a = OpGroupNonUniformBallotFindLSB %uint", "%lsb_a = OpGroupNonUniformBallotFindLSB %bool")
    assert "%lsb_a = OpGroupNonUniformBallotFindLSB %bool %scope %va" in lower_ballot_extract(wrong_type).splitlines()
    for scope in ("%k2", "%k128", "%ids", "%missing"):  # Workgroup, a non-scope value, a non-constant, an unknown id
        probe = original.replace("FindLSB %uint %scope %va", f"FindLSB %uint {scope} %va")
        try:
            lower_ballot_extract(probe)
        except ValueError as error:
            assert str(error) == "Ballot find-lsb lowering needs a Subgroup scope constant", error
        else:
            raise AssertionError(f"accepted scope {scope}")
    try:
        lower_ballot_extract(original.replace("%k1 ", "%x2w_ballot_existing ").replace("%k1\n", "%x2w_ballot_existing\n"))
    except ValueError as error:
        assert str(error) == "Reserved ballot lowering ID already in use", error
    else:
        raise AssertionError("accepted reserved ballot lowering ID")

    expected = find_lsb_expected()
    assert expected[:128] == list(range(128)) and expected[128 + 7] == NONE and NONE not in expected[:128]
    for name, (pattern, replacement, matches, model) in FAULTS.items():
        assert re.subn(pattern, replacement, lowered)[1] == matches, name
        assert find_lsb_expected(model) != expected, name


def emit(directory, assembler, validator, cross):
    source = Path(__file__).with_name("ballot.spvasm")
    original = source.read_text()
    check_ballot_profile(original)

    renamed = (original.replace("%mask", "%input_mask")
               .replace("%i =", "%lane =")
               .replace(" %i\n", " %lane\n")
               .replace("%zero", "%output_zero")
               .replace("%one", "%output_one")
               .replace("%main = OpFunction", "%main =  OpFunction"))
    variant = directory / "ballot-renamed.spvasm"
    variant.write_text(renamed)
    lowered = lower_ballot_extract(renamed)
    # The variant is valid SPIR-V even unlowered, so check that the extraction was replaced.
    assert "OpGroupNonUniformBallotBitExtract" not in lowered
    assert "OpVectorExtractDynamic %uint %input_mask" in lowered
    variant_adapted = directory / "ballot-renamed-logical.spvasm"
    variant_adapted.write_text(lowered)

    adapted = directory / "logical.spvasm"
    adapted.write_text(lower_ballot_extract(original))
    module = directory / "module.spv"
    for path in (source, variant, variant_adapted, adapted):
        subprocess.run([assembler, "--target-env", "vulkan1.2", path, "-o", module], check=True)
        subprocess.run([validator, "--target-env", "vulkan1.2", module], check=True)
    hlsl = directory / "ballot.hlsl"
    subprocess.run([cross, module, "--hlsl", "--shader-model", "66", "--output", hlsl], check=True)
    return hlsl, emit_find_lsb(directory, original, assembler, validator, cross)


def to_hlsl(directory, name, assembly, assembler, validator, cross, models):
    """Assemble and validate a module, then convert it to HLSL for each shader model."""
    source = directory / f"{name}.spvasm"
    source.write_text(assembly)
    module = directory / f"{name}.spv"
    subprocess.run([assembler, "--target-env", "vulkan1.2", source, "-o", module], check=True)
    subprocess.run([validator, "--target-env", "vulkan1.2", module], check=True)
    for model in models:
        hlsl = directory / f"{name}-{model}.hlsl"
        subprocess.run([cross, module, "--hlsl", "--shader-model", model, "--output", hlsl], check=True)
    return hlsl


def emit_find_lsb(directory, header, assembler, validator, cross):
    original = find_lsb_source(header)
    check_find_lsb_profile(original)
    tools = (assembler, validator, cross)
    lowered = lower_ballot_extract(original)
    shaders = {"lowered": to_hlsl(directory, "find-lsb", lowered, *tools, ("60", "66"))}
    for name, (pattern, replacement, _, _) in FAULTS.items():
        shaders[name] = to_hlsl(directory, "find-lsb-" + name.replace(" ", "-"),
                                re.sub(pattern, replacement, lowered), *tools, ("66",))
    return shaders


def draw(directory, dxil, renderer, adapter):
    mask = 0x784632A0A156789102CABEDE81234567
    bits = [(mask >> i) & 1 for i in range(128)]
    for wrong in (False, True):
        expected = bits.copy()
        if wrong:
            expected[31], expected[32] = expected[32], expected[31]
            assert expected != bits
        case = directory / "ballot.case"
        case.write_bytes(struct.pack("<321I", 4, *expected, *([0xc5c5c5c5] * 192)))
        run = subprocess.run([renderer, adapter, "--spirv-ballot", dxil, case],
                             capture_output=True, text=True)
        print(run.stdout, run.stderr, flush=True)
        if run.returncode == 77 and not wrong:
            return 77
        mismatches = 2 if wrong else 0
        if (run.returncode != int(wrong)
                or f"compared_words=320 mismatches={mismatches}" not in run.stdout):
            raise RuntimeError("Unexpected ballot readback")
    return 0


def draw_find_lsb(directory, dxils, renderer, adapter):
    """The lowered module must match the bit-scan reference; each faulty one must miss at its first word."""
    expected = find_lsb_expected()
    case = directory / "find_lsb.case"
    case.write_bytes(struct.pack("<321I", 4, *expected))
    for name, dxil in dxils.items():
        actual = expected if name == "lowered" else find_lsb_expected(FAULTS[name][3])
        wrong = [n for n, (a, b) in enumerate(zip(expected, actual)) if a != b]
        run = subprocess.run([renderer, adapter, "--spirv-ballot", dxil, case], capture_output=True, text=True)
        print(name, run.stdout, run.stderr, flush=True)
        if run.returncode == 77 and name == "lowered":
            return 77
        first = f"word={wrong[0]} expected={expected[wrong[0]]:08x} actual={actual[wrong[0]]:08x}" if wrong else ""
        if (run.returncode != int(bool(wrong)) or f"compared_words=320 mismatches={len(wrong)}" not in run.stdout
                or first not in run.stderr):
            raise RuntimeError(f"Unexpected find-lsb readback for {name}")
    return 0


if __name__ == "__main__":
    with tempfile.TemporaryDirectory() as tmp:
        directory = Path(tmp)
        hlsl, find_lsb = emit(directory, *sys.argv[1:4])
        if len(sys.argv) > 4:
            def compile_dxil(name, source):
                dxil = directory / f"{name}.dxil"
                subprocess.run([sys.argv[4], "-T", "cs_6_6", "-E", "main", "-Fo", dxil, source], check=True)
                return dxil
            code = draw(directory, compile_dxil("ballot", hlsl), *sys.argv[5:])
            if code == 0:
                dxils = {name: compile_dxil(name.replace(" ", "-"), source) for name, source in find_lsb.items()}
                code = draw_find_lsb(directory, dxils, *sys.argv[5:])
            sys.exit(code)
