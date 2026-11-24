# SPDX-License-Identifier: MIT
"""Run the two synthetic packed D16 store code objects with the Linux HIP runtime."""
import argparse
import ctypes as ct
import os
from pathlib import Path


DESCRIPTOR_RECORDS = 64
LANES = 32
GROUPS = (1, 2, 3)
# Allocate even the out-of-bounds dispatch lanes so a failed bounds check is observable.
RECORDS = max(GROUPS) * LANES + 16
WordArray = ct.c_uint16 * (4 * RECORDS)


def initial_words():
    return [0x8100 + i for i in range(4 * RECORDS)]


def expected_words(groups, write_w, *, respect_bounds=True):
    words = initial_words()
    active = groups * LANES
    if respect_bounds:
        active = min(active, DESCRIPTOR_RECORDS)
    for lane in range(active):
        packed = (0x1357 ^ lane, 0x2468, 0x389b ^ lane, 0xca5e)
        count = 4 if write_w else 3
        words[4 * lane:4 * lane + count] = packed[:count]
    return words


def first_difference(expected, actual):
    for i, (left, right) in enumerate(zip(expected, actual, strict=True)):
        if left != right:
            return i, left, right
    return None


def describe(difference):
    word, expected, actual = difference
    return (f"word={word} record={word // 4} channel={'XYZW'[word % 4]} "
            f"expected=0x{expected:04x} actual=0x{actual:04x}")


def model_check():
    for groups in GROUPS:
        preserve = expected_words(groups, False)
        overwrite = expected_words(groups, True)
        assert first_difference(preserve, overwrite) == (3, 0x8103, 0xca5e)
        tail_start = 4 * min(groups * LANES, DESCRIPTOR_RECORDS)
        assert preserve[tail_start:] == initial_words()[tail_start:]
        print(f"groups={groups} wrong-W control: {describe(first_difference(preserve, overwrite))}")
    for write_w in (False, True):
        groups = max(GROUPS)
        expected = expected_words(groups, write_w)
        unbounded = expected_words(groups, write_w, respect_bounds=False)
        difference = first_difference(expected, unbounded)
        assert difference == (256, 0x8200, 0x1317)
        shape = "xyzw" if write_w else "xyz"
        print(f"{shape} groups={groups} missing-bounds control: {describe(difference)}")


class Hip:
    def __init__(self):
        self.library = ct.CDLL("libamdhip64.so")
        self.library.hipGetErrorString.argtypes = [ct.c_int]
        self.library.hipGetErrorString.restype = ct.c_char_p
        integer, unsigned, pointer = ct.c_int, ct.c_uint, ct.c_void_p
        signatures = {
            "hipInit": [unsigned], "hipSetDevice": [integer],
            "hipDeviceGetName": [pointer, integer, integer],
            "hipRuntimeGetVersion": [ct.POINTER(integer)],
            "hipDriverGetVersion": [ct.POINTER(integer)],
            "hipModuleLoad": [ct.POINTER(pointer), ct.c_char_p],
            "hipModuleGetFunction": [ct.POINTER(pointer), pointer, ct.c_char_p],
            "hipMalloc": [ct.POINTER(pointer), ct.c_size_t],
            "hipMemcpy": [pointer, pointer, ct.c_size_t, integer],
            "hipModuleLaunchKernel": [pointer] + [unsigned] * 7 + [pointer] * 3,
            "hipDeviceSynchronize": [], "hipFree": [pointer],
            "hipModuleUnload": [pointer],
        }
        for name, args in signatures.items():
            fn = getattr(self.library, name)
            fn.argtypes = args
            fn.restype = integer

    def call(self, name, *args):
        status = getattr(self.library, name)(*args)
        if status:
            detail = self.library.hipGetErrorString(status)
            raise RuntimeError(f"{name}: {status} {detail.decode() if detail else ''}")

    def readback(self, path, groups):
        module, kernel, output = ct.c_void_p(), ct.c_void_p(), ct.c_void_p()
        self.call("hipModuleLoad", ct.byref(module), bytes(path))
        try:
            self.call("hipModuleGetFunction", ct.byref(kernel), module, b"store_d16")
            self.call("hipMalloc", ct.byref(output), ct.sizeof(WordArray))
            try:
                words = WordArray(*initial_words())
                self.call("hipMemcpy", output, words, ct.sizeof(words), 1)  # host to device
                params = (ct.c_void_p * 1)(ct.cast(ct.byref(output), ct.c_void_p))
                self.call("hipModuleLaunchKernel", kernel, groups, 1, 1, LANES, 1, 1,
                          0, None, params, None)
                self.call("hipDeviceSynchronize")
                self.call("hipMemcpy", words, output, ct.sizeof(words), 2)  # device to host
                return list(words)
            finally:
                self.call("hipFree", output)
        finally:
            self.call("hipModuleUnload", module)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", type=int, default=0)
    parser.add_argument("--target", choices=("gfx1030", "gfx1100"), default="gfx1030")
    parser.add_argument("--model-only", action="store_true")
    args = parser.parse_args()
    model_check()
    if args.model_only:
        return
    if "HSA_OVERRIDE_GFX_VERSION" in os.environ:
        parser.error("Unset HSA_OVERRIDE_GFX_VERSION to measure the actual device target")
    directory = Path(__file__).resolve().parents[2] / "out" / "native-mubuf-d16" / args.target
    for shape in ("xyz", "xyzw"):
        if not (directory / f"{shape}.hsaco").is_file():
            parser.error(f"Build {args.target} first with: sh tests/gfx1030_mubuf_d16/build.sh {args.target}")
    hip = Hip()
    hip.call("hipInit", 0)
    hip.call("hipSetDevice", args.device)
    name = ct.create_string_buffer(256)
    runtime, driver = ct.c_int(), ct.c_int()
    hip.call("hipDeviceGetName", name, len(name), args.device)
    hip.call("hipRuntimeGetVersion", ct.byref(runtime))
    hip.call("hipDriverGetVersion", ct.byref(driver))
    print(f"device={name.value.decode()} code_target={args.target} "
          f"hip_runtime={runtime.value} hip_driver={driver.value}")
    for shape, write_w in (("xyzw", True), ("xyz", False)):
        for groups in GROUPS:
            path = directory / f"{shape}.hsaco"
            actual = hip.readback(path, groups)
            expected = expected_words(groups, write_w)
            difference = first_difference(expected, actual)
            if difference:
                raise SystemExit(f"{shape} groups={groups}: {describe(difference)}")
            print(f"{shape} groups={groups} compared_halfwords={len(actual)} mismatches=0")


if __name__ == "__main__":
    main()
