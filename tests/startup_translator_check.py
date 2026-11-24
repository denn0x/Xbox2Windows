"""Checks for the startup shader translator (tools/startup/gfx2hlsl.py) that need no game data.

Descriptor offsets: one s_load_dwordx16 from a descriptor table fetches two 8-dword image descriptors.
An image_sample that names the upper one (s[N+8:N+15]) must resolve to byte offset 32 of the table,
not to the start of the load; otherwise both samples bind the same texture.
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tools', 'startup'))
import gfx2hlsl  # noqa: E402


def stage_with_table():
    stage = gfx2hlsl.Stage.__new__(gfx2hlsl.Stage)  # no machine code: only the scalar symbol state
    stage.sym = {8: ('ptr', ('table', 0))}
    return stage


def check_wide_load(failures):
    stage = stage_with_table()
    stage.op_s_load_dwordx16(['s[24:39]', 's[8:9]', '0x0'], [])
    got = [stage.descriptor('s[24:31]'), stage.descriptor('s[32:39]')]
    want = [(('table', 0), 0), (('table', 0), 32)]
    if got != want:
        failures.append('s_load_dwordx16 descriptors: got %s, expected %s' % (got, want))


def check_offset_load(failures):
    stage = stage_with_table()
    stage.op_s_load_dwordx8(['s[16:23]', 's[8:9]', '0x40'], [])
    got = [stage.descriptor('s[16:19]'), stage.descriptor('s[20:23]')]
    want = [(('table', 0), 0x40), (('table', 0), 0x50)]
    if got != want:
        failures.append('s_load_dwordx8 at +0x40: got %s, expected %s' % (got, want))


def check_negative_control(failures):
    # A key that ignores the register's dword index (the former behaviour) gives the second
    # descriptor offset 0, the same resource as the first: the check above must tell them apart.
    stage = stage_with_table()
    stage.op_s_load_dwordx16(['s[24:39]', 's[8:9]', '0x0'], [])
    first, second = stage.sym[24], stage.sym[32]
    if (first[1], first[2]) != (second[1], second[2]):
        failures.append('negative control: the load no longer records a shared base offset')
    if stage.descriptor('s[32:39]') == (first[1], first[2]):
        failures.append('negative control: the upper descriptor resolves to the load base')


def main():
    failures = []
    for check in (check_wide_load, check_offset_load, check_negative_control):
        check(failures)
    for line in failures:
        print('FAIL', line)
    print('startup translator checks: %d failed' % len(failures))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
