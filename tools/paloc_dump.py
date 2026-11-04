"""paloc_dump.py [out dir]: decompresses the game's English string tables to text (id <tab> text per line), one .txt per table.
A .paloc: "paloc" + 4 zero bytes, u32 stored size, u32 size unpacked, zeros, then the LZ4 block (the last `stored` bytes). Unpacked: records of
{u32 key length, key, u32 text length, text}, with some bytes between them."""
import os, re, struct, sys
sys.path.insert(0, 'F:/MyProjects/CDOverlayLoader')
out = sys.argv[1] if len(sys.argv) > 1 and __name__ == '__main__' else 'F:/MyProjects/CDInventory/re/strings_eng'
sys.argv = sys.argv[:1]
import pack


def lz4(src, size):
    d, i = bytearray(), 0
    while i < len(src):
        t = src[i]
        i += 1
        n = t >> 4
        if n == 15:
            while True:
                b = src[i]
                i += 1
                n += b
                if b != 255:
                    break
        d += src[i:i + n]
        i += n
        if i >= len(src) or len(d) >= size:
            break
        off = src[i] | src[i + 1] << 8
        i += 2
        m = t & 15
        if m == 15:
            while True:
                b = src[i]
                i += 1
                m += b
                if b != 255:
                    break
        m += 4
        for _ in range(m):
            d.append(d[-off])
    return bytes(d)


def unpack(raw):
    assert raw[:5] == b'paloc'
    stored, usize = struct.unpack_from('<II', raw, 9)
    d = lz4(raw[len(raw) - stored:], usize)
    assert len(d) == usize, (len(d), usize)
    return d


def records(d):
    """(key, text) pairs: a key (ascii, u32 length before it) followed by u32 length + text."""
    i, res = 0, []
    while i + 8 <= len(d):
        n = struct.unpack_from('<I', d, i)[0]
        key = d[i + 4:i + 4 + n] if 1 <= n <= 96 else b''
        j = i + 4 + n
        if key and re.fullmatch(rb'[0-9A-Za-z_]+', key) and j + 4 <= len(d):
            m = struct.unpack_from('<I', d, j)[0]
            if m <= 200000 and j + 4 + m <= len(d):
                try:
                    res.append((key.decode(), d[j + 4:j + 4 + m].decode('utf-8')))
                    i = j + 4 + m
                    continue
                except UnicodeDecodeError:
                    pass
        i += 1
    return res


if __name__ == '__main__':
    v = pack.vanilla_index()
    os.makedirs(out, exist_ok=True)
    for n in sorted(x for x in v if x.startswith('gamedata/stringtable/binary__/eng/')):
        name = n.split('/')[-1].rsplit('.', 1)[0]
        d = unpack(v[n].read(n))
        r = records(d)
        with open(os.path.join(out, name + '.txt'), 'w', encoding='utf-8', newline='\n') as f:
            for k, t in r:
                f.write(k + '\t' + t.replace('\n', '\\n').replace('\r', '') + '\n')
        print(f'{name:28} {len(d):>9} bytes, {len(r):>6} strings')
