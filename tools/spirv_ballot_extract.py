"""Lower subgroup ballot extraction and find-lowest-bit in the bridge's named uint32 SPIR-V profile."""
import re

OP = re.compile(r"\s*(%\w+)\s*=\s*OpGroupNonUniformBallotBitExtract\s+%bool\s+%\w+\s+(%\w+)\s+(%\w+)\s*(?:;.*)?")
FIND_LSB = re.compile(r"\s*(%\w+)\s*=\s*OpGroupNonUniformBallotFindLSB\s+%uint\s+(%\w+)\s+(%\w+)\s*(?:;.*)?")
GLSL_IMPORT = re.compile(r'\s*(%\w+)\s*=\s*OpExtInstImport\s+"GLSL\.std\.450"\s*(?:;.*)?')
UINT_CONSTANT = re.compile(r"\s*(%\w+)\s*=\s*OpConstant\s+%uint\s+(\w+)\s*(?:;.*)?")
SUBGROUP_SCOPE = 3
EXTRACT_CONSTANTS = (0, 1, 5, 31)
FIND_CONSTANTS = (31, 32, 64, 96)


def _find_lsb(dst, ballot, glsl):
    """Each word's lowest set bit plus its offset; an empty word (-1) stays all ones, and the smallest wins."""
    tmp = "%x2w_ballot_" + dst[1:] + "_"
    lines = [f"{tmp}w0 = OpCompositeExtract %uint {ballot} 0",
             f"{tmp}c0 = OpExtInst %uint {glsl} FindILsb {tmp}w0"]
    for index, offset in enumerate((32, 64, 96), 1):
        lines.extend([
            f"{tmp}w{index} = OpCompositeExtract %uint {ballot} {index}",
            f"{tmp}l{index} = OpExtInst %uint {glsl} FindILsb {tmp}w{index}",
            f"{tmp}m{index} = OpShiftRightArithmetic %uint {tmp}l{index} %x2w_ballot_c31",
            f"{tmp}a{index} = OpIAdd %uint {tmp}l{index} %x2w_ballot_c{offset}",
            f"{tmp}c{index} = OpBitwiseOr %uint {tmp}a{index} {tmp}m{index}",
        ])
    lines.extend([
        f"{tmp}u2 = OpExtInst %uint {glsl} UMin {tmp}c2 {tmp}c3",
        f"{tmp}u1 = OpExtInst %uint {glsl} UMin {tmp}c1 {tmp}u2",
        f"{dst} = OpExtInst %uint {glsl} UMin {tmp}c0 {tmp}u1",
    ])
    return lines


def lower_ballot_extract(assembly):
    extracts = "OpGroupNonUniformBallotBitExtract" in assembly
    finds = any(FIND_LSB.fullmatch(line) for line in assembly.splitlines())
    if not extracts and not finds:
        return assembly
    if "%x2w_ballot_" in assembly:
        raise ValueError("Reserved ballot lowering ID already in use")
    glsl = next((m[1] for m in map(GLSL_IMPORT.fullmatch, assembly.splitlines()) if m), None)
    import_needed = finds and glsl is None
    glsl = glsl or "%x2w_ballot_glsl"
    scopes = {m[1]: int(m[2], 0) for m in map(UINT_CONSTANT.fullmatch, assembly.splitlines()) if m}
    constants = sorted(set(EXTRACT_CONSTANTS if extracts else ()) | set(FIND_CONSTANTS if finds else ()))
    lines = []
    declared = imported = False
    for line in assembly.splitlines():
        if import_needed and not imported and line.lstrip().startswith("OpMemoryModel"):
            lines.append(f'{glsl} = OpExtInstImport "GLSL.std.450"')
            imported = True
        if not declared and re.search(r"=\s*OpFunction\s", line):
            lines.extend(f"%x2w_ballot_c{value} = OpConstant %uint {value}" for value in constants)
            declared = True
        found = FIND_LSB.fullmatch(line)
        if found:
            dst, scope, ballot = found.groups()
            if scopes.get(scope) != SUBGROUP_SCOPE:
                raise ValueError("Ballot find-lsb lowering needs a Subgroup scope constant")
            lines.extend(_find_lsb(dst, ballot, glsl))
            continue
        match = OP.fullmatch(line)
        if not match:
            lines.append(line)
            continue
        dst, ballot, index = match.groups()
        tmp = "%x2w_ballot_" + dst[1:] + "_"
        lines.extend([
            f"{tmp}word_index = OpShiftRightLogical %uint {index} %x2w_ballot_c5",
            f"{tmp}word = OpVectorExtractDynamic %uint {ballot} {tmp}word_index",
            f"{tmp}bit = OpBitwiseAnd %uint {index} %x2w_ballot_c31",
            f"{tmp}shifted = OpShiftRightLogical %uint {tmp}word {tmp}bit",
            f"{tmp}low = OpBitwiseAnd %uint {tmp}shifted %x2w_ballot_c1",
            f"{dst} = OpINotEqual %bool {tmp}low %x2w_ballot_c0",
        ])
    return "\n".join(lines) + "\n"
