"""Compute entry registers from COMPUTE_PGM_RSRC2 in tools/compute_store_aot.py (#1544).

  (no arguments)                        decoding and translation controls, pure Python
  binary <llvm-mc>                      assemble the fixture programs for gfx1013
  draw <dxc> <offscreen_check> <warp|hardware>
                                        translate, dispatch and compare the readback

A gfx10 compute wave receives USER_SGPR user SGPRs, then one SGPR for each group ID that
TGID_X/Y/Z_EN enables, in X, Y, Z order with no gap for an omitted axis, and TIDIG_COMP_CNT + 1
local-ID VGPRs from v0. Each fixture program below is source-built for gfx1013 and names the
group IDs only by the registers that layout gives them; no group register is passed by hand.
Every thread computes its dispatch coordinate on each axis it uses (group * size + local) and
stores them as a record at an address built from those coordinates, so each thread has its own
destination. Words outside the records keep the sentinel, and all 320 are compared.

The cases enable X with Z (Y omitted), Y with Z (X omitted), Z alone, all three, and X behind
four user SGPRs. They run with different group counts per axis, so the coordinates on different
axes are distinct. Mutations that swap two stored coordinates must mismatch.

TG_SIZE_EN is refused: its word layout and the guest wave width are not established (#1544).
"""
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "tools"))
import compute_store_aot as aot  # noqa: E402

SENTINEL = 0xc5c5c5c5
WORDS = 320
TGID_X, TGID_Y, TGID_Z, TG_SIZE_EN = 1 << 7, 1 << 8, 1 << 9, 1 << 10


def user(count):
    return count << 1


def tidig(count):
    return count << 11


HEAD = ["s_mov_b32 s1, 0", "s_load_dwordx4 s[12:15], s[0:1], null"]
TAIL = ["s_waitcnt lgkmcnt(0)"]

# X and Z enabled, Y omitted: s2 is group X and s3 is group Z. 2x1x4 threads; each stores (x, z)
# at row z (idxen, 48-byte stride) and column x (offen, 8 bytes).
X_Z = HEAD + [
    "v_lshl_add_u32 v3, s2, 1, v0",
    "v_lshl_add_u32 v4, s3, 2, v2",
    "v_lshlrev_b32_e32 v5, 3, v3",
    "v_mov_b32_e32 v8, v3",
    "v_mov_b32_e32 v9, v4",
] + TAIL + ["buffer_store_dwordx2 v[8:9], v[4:5], s[12:15], 0 idxen offen", "s_endpgm"]
# Y and Z enabled, X omitted: s2 is group Y and s3 is group Z. 1x2x4 threads; each stores (y, z).
Y_Z = HEAD + [
    "v_lshl_add_u32 v4, s2, 1, v1",
    "v_lshl_add_u32 v6, s3, 2, v2",
    "v_lshlrev_b32_e32 v7, 3, v4",
    "v_mov_b32_e32 v8, v4",
    "v_mov_b32_e32 v9, v6",
] + TAIL + ["buffer_store_dwordx2 v[8:9], v[6:7], s[12:15], 0 idxen offen", "s_endpgm"]
# Z alone: s2 is group Z. 1x1x4 threads; each stores z at word z.
Z_ONLY = HEAD + [
    "v_lshl_add_u32 v6, s2, 2, v2",
] + TAIL + ["buffer_store_dword v6, v6, s[12:15], 0 idxen", "s_endpgm"]
# All three: s2, s3 and s4 are groups X, Y and Z. 2x2x2 threads; each stores (x, y, z) at plane z
# (idxen, 256-byte stride), row y (64 bytes) and column x (16 bytes).
XYZ = HEAD + [
    "v_lshl_add_u32 v3, s2, 1, v0",
    "v_lshl_add_u32 v4, s3, 1, v1",
    "v_lshl_add_u32 v6, s4, 1, v2",
    "v_lshlrev_b32_e32 v5, 4, v3",
    "v_lshl_add_u32 v7, v4, 6, v5",
    "v_mov_b32_e32 v8, v3",
    "v_mov_b32_e32 v9, v4",
    "v_mov_b32_e32 v10, v6",
] + TAIL + ["buffer_store_dwordx3 v[8:10], v[6:7], s[12:15], 0 idxen offen", "s_endpgm"]
# Four user SGPRs (the table in s[0:1], s2 and s3 unused here) put group X in s4. 32 threads; each
# stores x at word x.
X_USER4 = HEAD + [
    "v_lshl_add_u32 v3, s4, 5, v0",
] + TAIL + ["buffer_store_dword v3, v3, s[12:15], 0 idxen", "s_endpgm"]

# name: (program, threads per group, rsrc2, stride, dispatches, record(x, y, z) -> (word, values))
CASES = {
    "x_z": (X_Z, (2, 1, 4), user(2) | TGID_X | TGID_Z | tidig(2), 48,
            ((1, 1, 1), (3, 1, 2)), lambda x, y, z: ((48 * z + 8 * x) // 4, (x, z))),
    "y_z": (Y_Z, (1, 2, 4), user(2) | TGID_Y | TGID_Z | tidig(2), 48,
            ((1, 1, 1), (1, 3, 2)), lambda x, y, z: ((48 * z + 8 * y) // 4, (y, z))),
    "z_only": (Z_ONLY, (1, 1, 4), user(2) | TGID_Z | tidig(2), 4,
               ((1, 1, 1), (1, 1, 5)), lambda x, y, z: (z, (z,))),
    "xyz": (XYZ, (2, 2, 2), user(2) | TGID_X | TGID_Y | TGID_Z | tidig(2), 256,
            ((1, 1, 1), (2, 1, 1), (1, 2, 2), (2, 2, 2)),
            lambda x, y, z: ((256 * z + 64 * y + 16 * x) // 4, (x, y, z))),
    "x_user4": (X_USER4, (32, 1, 1), user(4) | TGID_X, 4,
                ((1, 1, 1), (3, 1, 1)), lambda x, y, z: (x, (x,))),
}
# Swapping two stored coordinates must change the readback on the largest dispatch.
MUTATIONS = {
    "x_z": ("uint2(tid.x, tid.z)", "uint2(tid.z, tid.x)"),
    "y_z": ("uint2(tid.y, tid.z)", "uint2(tid.z, tid.y)"),
    "xyz": ("uint3(tid.x, tid.y, tid.z)", "uint3(tid.y, tid.x, tid.z)"),
}


def translate(name, **change):
    program, dims, rsrc2, stride, _, _ = CASES[name]
    binding = dict(table_reg=0, descriptor_offset=0, stride=stride, rsrc2=rsrc2) | change
    return aot.lower(program, dims, **binding)


def expected(name, groups):
    """The readback from the record layout, independent of the translator."""
    _, dims, _, _, _, record = CASES[name]
    words = [SENTINEL] * WORDS
    for x in range(groups[0] * dims[0]):
        for y in range(groups[1] * dims[1]):
            for z in range(groups[2] * dims[2]):
                word, values = record(x, y, z)
                assert word + len(values) <= WORDS and words[word] == SENTINEL, (name, x, y, z)
                words[word:word + len(values)] = values
    return words


def rejects(message, function, *args, **kwargs):
    try:
        function(*args, **kwargs)
    except ValueError as error:
        assert message in str(error), (message, error)
        return
    raise AssertionError(f"accepted; expected {message!r}")


def controls():
    # Placement: user SGPRs, then each enabled group ID in X, Y, Z order with no gap for an omitted
    # axis; TIDIG_COMP_CNT + 1 local-ID VGPRs.
    for rsrc2, placed in (
            (user(2) | TGID_X, (2, {0: 2}, 1)),
            (user(2) | TGID_Y, (2, {1: 2}, 1)),
            (user(2) | TGID_Z | tidig(2), (2, {2: 2}, 3)),
            (user(2) | TGID_X | TGID_Z | tidig(2), (2, {0: 2, 2: 3}, 3)),
            (user(3) | TGID_Y | TGID_Z | tidig(1), (3, {1: 3, 2: 4}, 2)),
            (user(5) | TGID_X | TGID_Y | TGID_Z | tidig(2), (5, {0: 5, 1: 6, 2: 7}, 3)),
            (user(2), (2, {}, 1)),
            # USER_SGPR is five bits wide: 16 and 31 user SGPRs put group X in s16 and s31.
            (user(16) | TGID_X, (16, {0: 16}, 1)),
            (user(31) | TGID_X | TGID_Z, (31, {0: 31, 2: 32}, 1))):
        assert aot.entry_registers(rsrc2) == placed, (hex(rsrc2), aot.entry_registers(rsrc2))
    rejects("thread-ID component count", aot.entry_registers, user(2) | TGID_X | tidig(3))
    # Fields outside USER_SGPR, TGID_X/Y/Z_EN, TG_SIZE_EN and TIDIG_COMP_CNT do not change the placement:
    # bit 0, TRAP_PRESENT (bit 6), bits 13-14, an LDS size in bits 15-23 and bit 31.
    plain = user(3) | TGID_Y | TGID_Z | tidig(1)
    for other in (1, 1 << 6, 3 << 13, 0x1ff << 15, 1 << 24, 1 << 31):
        assert aot.entry_registers(plain | other) == aot.entry_registers(plain), hex(other)
    for value in (-1, 1 << 32, 1.0, "0x484", True):
        rejects("Invalid COMPUTE_PGM_RSRC2", aot.entry_registers, value)
    print("entry_registers: user SGPRs, then the enabled group IDs in X, Y, Z order; TIDIG_COMP_CNT 0-2")

    # TG_SIZE_EN is refused, alone, with group IDs and in a translation.
    for rsrc2 in (user(2) | TG_SIZE_EN, user(2) | TGID_X | TG_SIZE_EN, CASES["xyz"][2] | TG_SIZE_EN):
        rejects("TG_SIZE_EN is not supported", aot.entry_registers, rsrc2)
    rejects("TG_SIZE_EN is not supported", translate, "x_z", rsrc2=CASES["x_z"][2] | TG_SIZE_EN)
    print("TG_SIZE_EN: refused")

    # Each case translates from rsrc2 alone, to the same source as with its group registers by hand.
    by_hand = {"x_z": dict(group_reg=2, group_reg_z=3), "y_z": dict(group_reg_y=2, group_reg_z=3),
               "z_only": dict(group_reg_z=2), "xyz": dict(group_reg=2, group_reg_y=3, group_reg_z=4),
               "x_user4": dict(group_reg=4)}
    for name, registers in by_hand.items():
        assert translate(name) == translate(name, rsrc2=None, **registers), name

    # An omitted or moved group ID leaves the program's register naming another axis or nothing.
    omitted = [
        ("x_z", user(2) | TGID_X | tidig(2)),                       # Z omitted: s3 is not a group ID
        ("x_z", user(2) | TGID_X | TGID_Y | TGID_Z | tidig(2)),     # Y enabled: s3 becomes group Y
        ("x_z", user(2) | TGID_Z | tidig(2)),                       # X omitted: s2 becomes group Z
        ("y_z", user(2) | TGID_X | TGID_Y | TGID_Z | tidig(2)),     # X enabled: s2 becomes group X
        ("y_z", user(2) | TGID_Y | tidig(2)),                       # Z omitted
        ("z_only", user(2) | TGID_Y | tidig(2)),                    # s2 is group Y
        ("xyz", user(2) | TGID_X | TGID_Z | tidig(2)),              # Y omitted: s3 becomes group Z
        ("xyz", user(2) | TGID_X | TGID_Y | tidig(2)),              # Z omitted: s4 is not a group ID
        ("x_user4", user(2) | TGID_X),                              # two user SGPRs: group X is s2
        ("x_user4", user(5) | TGID_X),                              # five user SGPRs: s4 is a user SGPR
    ]
    for name, rsrc2 in omitted:
        rejects("Unverified dispatch index", translate, name, rsrc2=rsrc2)
    # Local IDs beyond TIDIG_COMP_CNT are not bound.
    for name, count in (("x_z", 1), ("y_z", 0), ("z_only", 1), ("xyz", 1)):
        rejects("Unverified dispatch index", translate, name, rsrc2=CASES[name][2] & ~tidig(3) | tidig(count))
    print("controls: an omitted, moved or unenabled entry register is refused")

    rejects("do not also give group registers", translate, "x_z", group_reg=2)
    rejects("within the user SGPRs", translate, "x_z", rsrc2=user(1) | TGID_X | TGID_Z | tidig(2))
    # A 4x4 pitched store (the PITCH program of compute_store_check.py): ten user SGPRs with the table
    # at s[8:9] put the group IDs in s10 and s11, as that test names them by hand.
    pitch = ["s_mov_b32 s9, 0", "s_load_dwordx4 s[12:15], s[8:9], null",
             "v_lshl_add_u32 v3, s10, 2, v0", "v_lshl_add_u32 v4, s11, 2, v1",
             "v_lshlrev_b32_e32 v5, 3, v3", "v_mad_u32_u24 v6, 0x48, v4, v5",
             "v_mov_b32_e32 v8, v3", "v_mov_b32_e32 v9, v4", "s_waitcnt lgkmcnt(0)",
             "buffer_store_dwordx2 v[8:9], v6, s[12:15], 0 offen", "s_endpgm"]
    xy = user(10) | TGID_X | TGID_Y | tidig(1)
    assert aot.lower(pitch, (4, 4, 1), table_reg=8, descriptor_offset=0, rsrc2=xy) == aot.lower(
        pitch, (4, 4, 1), table_reg=8, descriptor_offset=0, group_reg=10, group_reg_y=11)
    print("controls: group registers only without rsrc2; table in the user SGPRs; hand-named PITCH unchanged")


def check_binary(llvm_mc):
    for name, (program, *_rest) in CASES.items():
        run = subprocess.run([llvm_mc, "-arch=amdgcn", "-mcpu=gfx1013", "-show-encoding"],
                             input="\n".join(program) + "\n", capture_output=True, text=True)
        if run.returncode or "error" in run.stderr:
            raise AssertionError(f"{name}: {run.stderr}")
    print("fixture programs assemble for gfx1013")


def dispatch(directory, dxc, renderer, backend, label, source, groups, words):
    src, cso, case = (directory / f"{label}.{ext}" for ext in ("hlsl", "cso", "case"))
    src.write_text(source, encoding="ascii")
    subprocess.run([dxc, "-T", "cs_6_0", "-E", "main", "-Fo", str(cso), str(src)], check=True)
    case.write_bytes(struct.pack(f"<{1 + WORDS}I", groups[0] | groups[1] << 8 | groups[2] << 16, *words))
    return subprocess.run([renderer, backend, "--compute-store-case", str(cso), str(case)],
                          capture_output=True, text=True)


def check_draw(directory, dxc, renderer, backend):
    controls()
    for name, (_, dims, _, _, dispatches, _) in CASES.items():
        source = translate(name)
        for groups in dispatches:
            label = f"{name}_{'x'.join(map(str, groups))}"
            run = dispatch(directory, dxc, renderer, backend, label, source, groups, expected(name, groups))
            if run.returncode == 77:
                print(run.stdout + run.stderr)
                return 77
            if run.returncode:
                raise AssertionError(f"{label}\n{run.stdout}\n{run.stderr}")
            debug = next((line for line in (run.stdout + run.stderr).splitlines() if "debug_errors=" in line), "")
            print(f"{label}: groups of {'x'.join(map(str, dims))} exact {debug.strip()}")
        if name in MUTATIONS:
            old, new = MUTATIONS[name]
            assert source.count(old) == 1, (name, source)
            groups = dispatches[-1]
            run = dispatch(directory, dxc, renderer, backend, f"{name}_swapped", source.replace(old, new),
                           groups, expected(name, groups))
            if run.returncode != 1 or "expected=" not in run.stderr:
                raise AssertionError(f"{name}: swapped coordinates were not detected\n{run.stdout}\n{run.stderr}")
            print(f"{name}: swapped coordinates mismatch, as required")
    return 0


def main():
    if len(sys.argv) == 1:
        controls()
        return 0
    mode, *args = sys.argv[1:]
    if mode == "binary":
        check_binary(*args)
        return 0
    if mode == "draw":
        with tempfile.TemporaryDirectory() as tmp:
            return check_draw(Path(tmp), *args)
    raise SystemExit(__doc__)


if __name__ == "__main__":
    sys.exit(main())
