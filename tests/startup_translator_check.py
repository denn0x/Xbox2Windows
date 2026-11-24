"""Checks for the startup shader translator (tools/startup/gfx2hlsl.py) that need no game data.

Descriptor offsets: one s_load_dwordx16 from a descriptor table fetches two 8-dword image descriptors.
An image_sample that names the upper one (s[N+8:N+15]) must resolve to byte offset 32 of the table,
not to the start of the load; otherwise both samples bind the same texture.

fxc discovery: a translation asked to compile (check) with no fxc.exe anywhere must write <key>.fail
naming X2W_FXC and no manifest; with the empty X2W_FXC or check=False it publishes the manifest alone.
"""
import contextlib
import os
import sys
import tempfile

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


class StubPipeline:
    def __init__(self, *args, **kwargs):
        self.ps_entry_reads = {}  # LOCAL translate_to refuses a pixel stage that reads an SPI input first

    def hlsl_vs(self):
        return 'void main() {}\n'

    hlsl_ps = hlsl_vs

    def manifest(self):
        return 'manifest\n'


@contextlib.contextmanager
def fxc_environment(work, configured, kits, path=''):
    """X2W_FXC, the Windows Kits bin directory and PATH pointed at temporary locations."""
    saved = (dict(os.environ), gfx2hlsl.DEFAULT_FXC, gfx2hlsl.KITS_BIN, gfx2hlsl.Pipeline)
    os.environ.pop('X2W_FXC', None)
    if configured is not None:
        os.environ['X2W_FXC'] = configured
    os.environ['PATH'] = path
    gfx2hlsl.DEFAULT_FXC = os.path.join(work, 'no-default', 'fxc.exe')
    gfx2hlsl.KITS_BIN = kits
    gfx2hlsl.Pipeline = StubPipeline
    try:
        yield
    finally:
        os.environ.clear()
        os.environ.update(saved[0])
        gfx2hlsl.DEFAULT_FXC, gfx2hlsl.KITS_BIN, gfx2hlsl.Pipeline = saved[1:]


def stub_fxc(root, version):
    directory = os.path.join(root, version, 'x64')
    os.makedirs(directory)
    path = os.path.join(directory, 'fxc.exe')
    open(path, 'w').close()
    return path


def translate(work, check=True):
    out = os.path.join(work, 'out')
    os.makedirs(out, exist_ok=True)
    result = gfx2hlsl.translate_to(out, 0x1234, b'', b'', b'', check=check)
    return result, sorted(os.listdir(out))


def check_missing_fxc(failures):
    with tempfile.TemporaryDirectory() as work, fxc_environment(work, os.path.join(work, 'missing.exe'), os.path.join(work, 'none')):
        result, files = translate(work)
        if '0000000000001234.fail' not in files or '0000000000001234.txt' in files or 'X2W_FXC' not in result:
            failures.append('missing fxc: result %r, files %s' % (result, files))
        fail = os.path.join(work, 'out', '0000000000001234.fail')
        if os.path.exists(fail) and 'X2W_FXC' not in open(fail).read():
            failures.append('missing fxc: the .fail does not name X2W_FXC')
    with tempfile.TemporaryDirectory() as work, fxc_environment(work, None, os.path.join(work, 'none')):
        result, files = translate(work, check=False)
        if files != ['0000000000001234.ps.hlsl', '0000000000001234.txt', '0000000000001234.vs.hlsl'] or result != 'ok':
            failures.append('check=False without fxc: result %r, files %s' % (result, files))
    with tempfile.TemporaryDirectory() as work, fxc_environment(work, '', os.path.join(work, 'none')):
        result, files = translate(work)
        if '0000000000001234.txt' not in files or '0000000000001234.fail' in files:
            failures.append('empty X2W_FXC: result %r, files %s' % (result, files))


def check_fxc_discovery(failures):
    with tempfile.TemporaryDirectory() as work:
        kits = os.path.join(work, 'Windows Kits', '10', 'bin')
        stub_fxc(kits, '10.0.22621.0')
        newest = stub_fxc(kits, '10.0.26200.0')
        stub_fxc(kits, '10.0.9.0')  # a string sort would rank 10.0.9.0 above 10.0.26200.0
        os.makedirs(os.path.join(kits, 'x86'))  # not a version directory
        on_path = os.path.join(work, 'path')
        os.makedirs(on_path)
        open(os.path.join(on_path, 'fxc.exe'), 'w').close()
        os.chmod(os.path.join(on_path, 'fxc.exe'), 0o755)  # a PATH search outside Windows needs the execute bit
        explicit = os.path.join(work, 'explicit.exe')
        open(explicit, 'w').close()
        for configured, path, want, label in ((None, '', newest, 'newest Windows Kits version'),
                                              (explicit, '', explicit, 'X2W_FXC'),
                                              (os.path.join(work, 'missing.exe'), '', newest, 'missing X2W_FXC falls back'),
                                              ('', '', '', 'empty X2W_FXC')):
            with fxc_environment(work, configured, kits, path):
                got = gfx2hlsl.find_fxc()
                if got != want:
                    failures.append('find_fxc (%s): got %r, expected %r' % (label, got, want))
        with fxc_environment(work, None, os.path.join(work, 'none'), on_path):
            got = gfx2hlsl.find_fxc()
            if not got or os.path.normcase(got) != os.path.normcase(os.path.join(on_path, 'fxc.exe')):
                failures.append('find_fxc (PATH): got %r' % (got,))


def check_negative_discovery(failures):
    # Control: the Windows Kits tree above holds a stub, so a search that ignores KITS_BIN must be caught
    # by check_fxc_discovery, and one that ignores the missing-fxc branch by check_missing_fxc. Here the
    # stub tree is present and the same translation must no longer fail.
    with tempfile.TemporaryDirectory() as work:
        kits = os.path.join(work, 'bin')
        stub_fxc(kits, '10.0.1.0')
        with fxc_environment(work, None, kits):
            if gfx2hlsl.find_fxc() is None:
                failures.append('negative control: a Windows Kits tree with fxc.exe was not found')


def main():
    failures = []
    for check in (check_wide_load, check_offset_load, check_negative_control, check_missing_fxc,
                  check_fxc_discovery, check_negative_discovery):
        check(failures)
    for line in failures:
        print('FAIL', line)
    print('startup translator checks: %d failed' % len(failures))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
