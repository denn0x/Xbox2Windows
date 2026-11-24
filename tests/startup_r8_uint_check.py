"""Run the translated vertex-fetch body on a raw SRV and read all four component bits."""
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

from startup_sampler_heap_check import root_blob
from startup_stream_check import gfx2hlsl


def pipeline(format_code):
    # s_load_dwordx4 s[16:19], s[0:1], 16  (vertex-buffer descriptor 1)
    # tbuffer_load_format_xyzw v[4:7], v0, s[16:19], 0 idxen offset:1
    # exp pos0 v4, v5, v6, v7 done
    words = (0xf4080400, 0xfa000010, 0xe8032001 | format_code << 19, 0x80040400,
             0xf80008cf, 0x07060504, 0xbf810000)
    vs = struct.pack('<7I', *words)
    ps = struct.pack('<3I', 0xf800180f, 0, 0xbf810000)
    result = gfx2hlsl.Pipeline(vs, ps, root_blob(flags=0))
    assert 'srv 0 buf vb 1' in result.manifest().splitlines()
    return result


def check(directory, compiler, consumer, adapter):
    source, binary, case = (directory / name for name in ('fetch.hlsl', 'fetch.cso', 'fetch.case'))
    # offscreen_check --compute-store-case fills the raw t0 with this little-endian pattern.
    input_bytes = b''.join(struct.pack('<I', (0x8b02a7f1 + i * 0x413b1) & 0xffffffff) for i in range(320))
    expected = [0xc5c5c5c5] * 320
    for vertex in range(16):
        first = vertex * 4
        expected[first:first + 4] = [input_bytes[vertex * 3 + 1], 0, 0, 1]

    for format_code in (5, 1):  # R8_UINT, then R8_UNORM on the same bytes
        translated = pipeline(format_code)
        stride_slot = translated.uds.index(('stride', 0))
        constants = [0] * 16
        constants[stride_slot] = 3
        case.write_bytes(struct.pack('<337I', 1, *expected, *constants))
        # Compile the actual VS entry, then call the same generated vertex body in a compute
        # wrapper so we can inspect integer component bits without interpolation or RT conversion.
        source.write_text(translated.hlsl_vs(), encoding='ascii')
        subprocess.run([compiler, '-T', 'vs_6_0', '-E', 'main', '-Fo', binary, source], check=True)
        with source.open('a', encoding='ascii') as handle:
            handle.write('''
RWByteAddressBuffer outputWords : register(u0);
[numthreads(16, 1, 1)]
void readback(uint id : SV_GroupIndex) {
    outputWords.Store4(id * 16, asuint(body(id).pos));
}
''')
        subprocess.run([compiler, '-T', 'cs_6_0', '-E', 'readback', '-Fo', binary, source], check=True)
        result = subprocess.run([consumer, adapter, '--compute-store-case', binary, case],
                                capture_output=True, text=True)
        if result.returncode == 77:
            print(result.stdout, result.stderr)
            return 77
        if format_code == 5 and result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
        if format_code == 5:
            print(result.stdout, end='')
        if format_code == 1 and (result.returncode != 1 or 'word=0 expected=' not in result.stderr):
            raise AssertionError(f'UNORM control: {result.returncode}\n{result.stderr}')
    print(f'{adapter}: vertex buffer 1, stride 3, offset 1: 16 exact uint4 values; R8_UNORM differs')
    return 0


if __name__ == '__main__':
    with tempfile.TemporaryDirectory() as temp:
        sys.exit(check(Path(temp), *sys.argv[1:]))
