"""tools/startup/capture.py against synthetic capture.bin files (pure Python, no Windows, no GPU).

The writer is src/startup_stubs/d3d12_capture.inl: every chunk is {char tag[4]; u32 header bytes;
u64 data bytes; header; data}. The builders below pack the writer's structs field by field, so a
reader that drifts from the writer layout fails here. Also checks Memory.read ordering, describe(),
and that damaged files raise a ValueError naming the tag and offset instead of decoding garbage.
"""
import io
import struct
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'tools' / 'startup'))
import capture as C  # noqa: E402

RES_W, HEAP_W, PSO_W, ROOT_W, OBJ_W = 0x7ff600123456, 0x7ff600222222, 0x7ff600333333, 0x7ff600444444, 0x7ff600555555
CONSOLE = (3, 0x10000, 64, 32, 1, 4, 28, 1, 0, 9, 0x2)   # Dimension .. Flags, SampleDesc.Quality is field 8
DESKTOP = (3, 0x10000, 128, 64, 1, 6, 87, 1, 0, 0, 0x5)


def chunk(tag, header=b'', body=b''):
    return struct.pack('<4sIQ', tag, len(header), len(body)) + header + body


def desc(dim, align, width, height, depth, mips, fmt, count, quality, layout, flags):
    # D3D12_RESOURCE_DESC: 52 bytes of fields padded to 56 (8-byte alignment)
    return struct.pack('<I4xQQIHHIIIII4x', dim, align, width, height, depth, mips, fmt, count, quality, layout, flags)


def res(wrapper=RES_W, console=CONSOLE, desktop=DESKTOP):
    return chunk(b'RES ', struct.pack('<4Q', wrapper, 0xaa0000, 4096, 0xbb0000) + desc(*console) + desc(*desktop))


def view(vid, slot, resource, desc_bytes=b'\x11\x22\x33\x44'):
    header = struct.pack('<IIQQQQII', vid, slot, resource, 0xc0, 0xd0, 0xe0, len(desc_bytes), 0)
    return chunk(b'VIEW', header + desc_bytes.ljust(64, b'\0'))


def mem(address, data, seq, why=0x50):
    return chunk(b'MEM ', struct.pack('<QQQII', address, len(data), len(data) + 8, why, seq), data)


def table(seq, slot, parameter, entries, gpu, heap, index, ids=()):
    return chunk(b'TABL', struct.pack('<IIIIQQQ', seq, slot, parameter, entries, gpu, heap, index), struct.pack('<%dI' % len(ids), *ids))


CALL = chunk(b'CALL', struct.pack('<IIIIQ7QQ', 1, 5, 77, 36, 0x1000abc, RES_W, 2, 3, 4, 5, 6, 7, 0x99))
FILE = b''.join([
    chunk(b'FRAM', struct.pack('<II', 1, 0)),
    CALL,
    mem(0x20000, b'ABCDEFGH', 1), mem(0x20004, b'WXYZ', 2), mem(0x20000, b'abcdefgh', 3),
    chunk(b'PSO ', struct.pack('<QQII', PSO_W, 0x4000, 57, 1), b'desc!'),
    chunk(b'ROOT', struct.pack('<QQQQII', ROOT_W, 1, 2, 3, 4, 0), b'rootblob'),
    res(),
    chunk(b'HEAP', struct.pack('<QIIIIQQQQQ', HEAP_W, 0, 8, 32, 0, 0x1000, 0x2000, 0x3000, 0x4000, 0x5000)),
    chunk(b'OBJ ', struct.pack('<QQQQII', OBJ_W, 1, 2, 3, 7, 0)),
    view(0, 18, RES_W), view(1, 17, 0, b''),
    table(5, 36, 2, 2, 0x3000, HEAP_W, 1, ids=(1, 2, 2, 0)),   # two shadow ids, then two token ids
    table(5, 36, 0xffffffff, 0, 0x9000, 0, 0),                  # the form without a known heap
    chunk(b'TGTS', struct.pack('<II', 5, 2), struct.pack('<2I', 1, 2)),
    chunk(b'PRES', struct.pack('<IIQ', 4, 0, RES_W)),
])


def load(data):
    with tempfile.TemporaryDirectory() as folder:
        path = Path(folder) / 'capture.bin'
        path.write_bytes(data)
        return C.Capture(str(path))


def refuses(data, *words):
    try:
        load(data)
    except ValueError as error:
        assert all(w in str(error) for w in words), (words, str(error))
        return
    raise AssertionError('accepted damaged input: %r' % (words,))


def check_records():
    cap = load(FILE)
    assert cap.calls == [dict(frame=1, seq=5, tid=77, slot=36, list=0x1000abc, args=(RES_W, 2, 3, 4, 5, 6, 7), ret=0x99)]
    assert cap.mem_by_seq[2] == [(0x20004, 4, 0x50)] and cap.mem_by_seq[3] == [(0x20000, 8, 0x50)]
    assert cap.psos[PSO_W] == dict(base=0x4000, path=57, real=1, desc=b'desc!')
    assert cap.roots[ROOT_W] == b'rootblob' and cap.objects[ROOT_W] == 'RootSignature'
    expect = lambda f: dict(dim=f[0], align=f[1], width=f[2], height=f[3], depth=f[4], mips=f[5], format=f[6],
                            samples=f[7], layout=f[9], flags=f[10])
    assert cap.resources[RES_W] == dict(title_memory=0xaa0000, bytes=4096, gpu=0xbb0000,
                                        console=expect(CONSOLE), desktop=expect(DESKTOP))
    assert cap.heaps[HEAP_W] == dict(type=0, count=8, increment=32, title_cpu=0x1000, desktop_cpu=0x2000, desktop_gpu=0x3000)
    assert cap.objects[OBJ_W] == 'kind7' and cap.objects[PSO_W] == 'PipelineState' and cap.objects[HEAP_W] == 'DescriptorHeap'
    assert cap.views[0] == dict(slot=18, resource=RES_W, counter=0xc0, dest_title=0xd0, desc=b'\x11\x22\x33\x44')
    assert cap.views[1]['slot'] == 17 and cap.views[1]['desc'] == b''
    with_heap, without = cap.tables[5]
    assert with_heap == dict(slot=36, parameter=2, entries=2, gpu=0x3000, heap=HEAP_W, index=1, shadow=(1, 2), tokens=(2, 0))
    assert without == dict(slot=36, parameter=0xffffffff, entries=0, gpu=0x9000, heap=0, index=0, shadow=(), tokens=())
    assert cap.targets == {5: (1, 2)} and cap.presents == [(4, RES_W)]


def check_memory():
    memory = load(FILE).memory
    assert memory.read(0x20000, 8, 1) == b'ABCDEFGH'
    assert memory.read(0x20000, 8, 2) == b'ABCDEFGH'          # the block at 0x20004 starts later
    assert memory.read(0x20000, 8) == b'abcdefgh' and memory.read(0x20000, 8, 3) == b'abcdefgh'
    assert memory.read(0x20000, 8, 0) is None                  # before any capture
    assert memory.read(0x1ffff, 4) is None and memory.read(0x20008, 1) is None
    assert memory.read(0x20002, 3) == b'cde'                   # address inside a block
    # The newest block that holds the address wins, even when it started later in memory.
    assert memory.read(0x20005, 2, 2) == b'XY' and memory.read(0x20005, 2, 1) == b'FG'
    # Today a read past the end of the block returns the shorter bytes without an error.
    assert memory.read(0x20004, 16, 3) == b'efgh'
    assert memory.read(0x20007, 4, 2) == b'Z'                  # newest block holding 0x20007 is the 4-byte one


def check_describe():
    out = io.StringIO()
    C.describe(load(FILE), out)
    text = out.getvalue()
    assert text.startswith('calls 1, memory blocks 2 (20 bytes), pipelines 1, roots 1, resources 1, heaps 1, views 2')
    assert 'slot histogram: SetGraphicsRootDescriptorTable x1' in text
    assert '   5 f1 Labc SetGraphicsRootDescriptorTable' in text
    assert 'Resource@123456, 0x2, 0x3 | 0x4 0x5' in text
    assert 'table param 2 heap index 1: 2 of 2 entries known' in text
    assert 'SRV view 0 (shadow 0 token 1) res=123456 dim3 64x32x1 mips4 fmt28 layout0x9 flags0x2 mem=0xaa0000 desc=11223344' in text
    assert 'CBV view 1 (shadow 1 token -1) res=0' in text and 'target view 1 (unknown)' in text and 'target view 0 res=123456 64x32 fmt28 flags0x2' in text
    assert 'table param -1 heap index 0: 0 of 0 entries known' in text
    assert 'present frame 4 surface Resource@123456' in text


def describe_text(*tables):
    data = b''.join([CALL, res(), view(0, 18, RES_W), view(1, 17, 0, b'')] + list(tables))
    out = io.StringIO()
    C.describe(load(data), out)
    return out.getvalue()


def check_describe_shadow_only():
    # A table that carries only the shadow ids (no token half) still lists its entries.
    text = describe_text(table(5, 36, 2, 2, 0x3000, HEAP_W, 1, ids=(1, 2)))
    assert 'table param 2 heap index 1: 2 of 2 entries known' in text
    assert 'SRV view 0 res=123456 dim3 64x32x1' in text and 'CBV view 1 res=0' in text
    assert 'token' not in text and 'shadow' not in text


def check_describe_one_sided_ids():
    # Zero in one half means "not recorded there"; the entry is still listed from the other half.
    text = describe_text(table(5, 36, 2, 2, 0x3000, HEAP_W, 1, ids=(0, 2, 1, 0)))
    assert 'table param 2 heap index 1: 2 of 2 entries known' in text
    assert 'SRV view 0 (shadow -1 token 0) res=123456' in text
    assert 'CBV view 1 (shadow 1 token -1) res=0' in text


def check_damaged_files():
    for cut, words in ((len(FILE) - 3, ('PRES', 'truncated')),            # body or header cut short
                       (len(FILE) - 12, ('PRES', 'truncated'))):
        refuses(FILE[:cut], *words)
    half_header = FILE + b'\x01' * 10
    refuses(half_header, 'truncated', 'offset %d' % len(FILE))
    body_cut = mem(0x30000, b'0123456789', 9)[:-4]
    refuses(FILE + body_cut, "b'MEM '", 'offset %d' % len(FILE))
    refuses(FILE + chunk(b'ZZZZ', b'\0' * 8), 'unknown', 'ZZZZ', 'offset %d' % len(FILE))
    refuses(FILE + chunk(b'\xff\xfe\x00\x01'), 'unknown', 'offset %d' % len(FILE))
    refuses(FILE + chunk(b'FRAM', b'\0' * 4), "b'FRAM'", 'offset %d' % len(FILE))
    refuses(FILE + chunk(b'CALL', b'\0' * 20), "b'CALL'", 'offset %d' % len(FILE))   # header shorter than the struct


def check_mutated_record():
    good = load(FILE).resources[RES_W]
    changed = list(CONSOLE)
    changed[2], changed[10] = 65, 0x1
    bad = load(FILE.replace(res(), res(console=tuple(changed)), 1)).resources[RES_W]
    assert bad['console']['width'] == 65 and bad['console']['flags'] == 1 and bad['console'] != good['console']
    assert bad['desktop'] == good['desktop']
    swapped = load(FILE.replace(res(), res(console=DESKTOP, desktop=CONSOLE), 1)).resources[RES_W]
    assert swapped['console'] == good['desktop'] and swapped['desktop'] == good['console']


if __name__ == '__main__':
    for check in (check_records, check_memory, check_describe, check_describe_shadow_only, check_describe_one_sided_ids, check_damaged_files, check_mutated_record):
        check()
        print('ok', check.__name__)
