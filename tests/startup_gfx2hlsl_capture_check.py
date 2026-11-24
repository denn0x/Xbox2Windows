"""gfx2hlsl.py --capture on synthetic capture files (pure Python, no Windows, no GPU).

The reader must write the request files for a path-57 pipeline, report an unknown code, and refuse
malformed pipeline records (a wrapper chain that loops, a desc shorter than 96 bytes) with a message
instead of hanging or raising a traceback. Every run is bounded by a timeout.
"""
import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tests'))
sys.path.insert(0, str(ROOT / 'tools' / 'startup'))
from startup_capture_check import chunk, mem  # noqa: E402
import gfx2hlsl  # noqa: E402

TIMEOUT = 20


def code(t):
    return struct.pack('<3I', 0xf800080f | t << 4, 0, 0xbf810000)


VS, PS = code(12), code(0)
ROOTSIG = b'DXBC' + bytes(16) + struct.pack('<III', 1, 36, 1) + struct.pack('<I', 0x9999)


def call(seq, w):
    return chunk(b'CALL', struct.pack('<IIIIQ7QQ', 1, seq, 1, 25, 0x1000, w, 0, 0, 0, 0, 0, 0, 0))


def pso(w, base, path, desc=b''):
    return chunk(b'PSO ', struct.pack('<QQII', w, base, path, 1), desc)


def desc96(vs_len=len(VS), ps_len=len(PS)):
    q = [0] * 12
    q[0], q[5], q[6], q[9], q[10] = 0x77, 0x20000, vs_len, 0x30000, ps_len
    return struct.pack('<12Q', *q)


BASE = (chunk(b'FRAM', struct.pack('<II', 1, 0)) + mem(0x20000, VS, 1) + mem(0x30000, PS, 1)
        + chunk(b'ROOT', struct.pack('<QQQQII', 0x77, 1, 2, 3, 4, 0), ROOTSIG))


def run(data):
    with tempfile.TemporaryDirectory() as d:
        cap = os.path.join(d, 'capture.bin')
        out = os.path.join(d, 'cache')
        Path(cap).write_bytes(data)
        try:
            r = subprocess.run([sys.executable, '-B', str(ROOT / 'tools' / 'startup' / 'gfx2hlsl.py'), '--capture', out, cap],
                               capture_output=True, text=True, timeout=TIMEOUT)
        except subprocess.TimeoutExpired:
            return -1, 'HUNG: no exit within %d s' % TIMEOUT, []
        files = sorted(os.listdir(out)) if os.path.isdir(out) else []
        return r.returncode, r.stdout + r.stderr, files


def check(name, cond, detail):
    if not cond:
        print('FAIL %s: %s' % (name, detail))
        sys.exit(1)
    print('ok   %s' % name)


rc, text, files = run(BASE + pso(0x55, 0, 57, desc96()) + call(2, 0x55))
key = gfx2hlsl.pipeline_key(VS, PS, ROOTSIG)
want = ['req_%016x%s' % (key, e) for e in ('.a', '.b', '.root')]
check('good path writes request files', all(f in files for f in want) and 'Traceback' not in text, (files, text))

rc, text, files = run(BASE + pso(0x55, 0x55, 55, desc96()) + call(2, 0x55))
check('self-referencing wrapper terminates cleanly', 'loops back' in text and 'Traceback' not in text and not any(f.startswith('req_') for f in files), text)

rc, text, files = run(BASE + pso(0x55, 0x66, 55) + pso(0x66, 0x55, 55) + call(2, 0x55))
check('two-wrapper cycle terminates cleanly', 'loops back' in text and 'Traceback' not in text, text)

rc, text, files = run(BASE + pso(0x55, 0, 57, desc96()[:40]) + call(2, 0x55))
check('short desc is refused with a message', 'need 96' in text and 'Traceback' not in text and not any(f.startswith('req_') for f in files), text)

rc, text, files = run(BASE + pso(0x55, 0, 57, desc96(len(VS) + 4096)) + call(2, 0x55))
check('unknown code reports not captured', 'code not captured' in text and 'Traceback' not in text, text)
print('startup_gfx2hlsl_capture_check passed')
