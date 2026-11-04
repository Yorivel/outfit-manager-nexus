"""Crimson Desert PAMT (archive index) explorer."""
import struct, sys

def _varint(d, o):
    v = s = 0
    while True:
        b = d[o]; o += 1
        v |= (b & 0x7F) << s; s += 7
        if not b & 0x80:
            return v, o

def read_trie(d, off):
    """Name tree: entries (parent, u8 len, name). Up to 1.0.0.2976 the parent is a u32 offset (0xFFFFFFFF = root);
    since the 2026-10-30 update it is a varint backward distance (0 = root)."""
    size = struct.unpack_from('<I', d, off)[0]; off += 4
    start, end = off, off + size
    nodes = {}
    old = size >= 4 and struct.unpack_from('<I', d, off)[0] == 0xFFFFFFFF
    while off < end:
        at = off - start
        if old:
            parent, ln = struct.unpack_from('<IB', d, off)
            off += 5
        else:
            delta, off = _varint(d, off)
            ln = d[off]; off += 1
            parent = at - delta if delta else 0xFFFFFFFF
        name = d[off:off+ln].decode('utf-8', 'replace')
        nodes[at] = (parent, name)
        off += ln
    def full(o):
        parts = []
        while o != 0xFFFFFFFF:
            p, n = nodes[o]; parts.append(n); o = p
        return ''.join(reversed(parts))
    return nodes, full, end

def parse(path):
    d = open(path, 'rb').read()
    npaz = struct.unpack_from('<I', d, 4)[0]
    off = 12 + npaz * 12
    dirs, dfull, off = read_trie(d, off)
    files, ffull, off = read_trie(d, off)
    return d, dirs, dfull, files, ffull, off

if __name__ == '__main__':
    d, dirs, dfull, files, ffull, off = parse(sys.argv[1])
    print('dir nodes', len(dirs), 'file nodes', len(files), 'rest at', hex(off), 'remaining', len(d) - off)
    print(d[off:off+96].hex(' '))

def records(path):
    d, dirs, dfull, files, ffull, off = parse(path)
    nd = struct.unpack_from('<I', d, off)[0]; off += 4
    folders = [struct.unpack_from('<IIII', d, off + i*16) for i in range(nd)]; off += nd*16
    nf = struct.unpack_from('<I', d, off)[0]; off += 4
    recs = [struct.unpack_from('<IIIIHH', d, off + i*20) for i in range(nf)]; off += nf*20
    # Since the 2026-10-30 update: names carry no extension; a trailer (u32 size + NUL-separated strings, first "")
    # lists them, and a record's flags high byte indexes it (the low byte keeps the old flags).
    exts = None
    if len(d) - off > 4:
        n = struct.unpack_from('<I', d, off)[0]
        if off + 4 + n == len(d):
            exts = d[off + 4:off + 4 + n].split(b'\0')
            exts = [e.decode('utf-8', 'replace') for e in exts]
    out = []
    for h, dname, first, cnt in folders:
        for r in recs[first:first+cnt]:
            nameoff, pazoff, csize, dsize, pazidx, flags = r
            name = dfull(dname) + '/' + ffull(nameoff)
            if exts is not None:
                ext = exts[flags >> 8] if (flags >> 8) < len(exts) else ''
                name += ext
                flags &= 0xFF
            out.append((name, pazidx, pazoff, csize, dsize, flags))
    return out, len(d) - off
