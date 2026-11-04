"""Extract a UI DDS (flag 1: 128-byte DDS header + LZ4 block, DXT5) from a PAZ group and save a PNG.
tex.py <group> <dds name substring> <out.png>"""
import os, sys, struct, zlib
import pamt
G = r'D:\SteamLibrary\steamapps\common\Crimson Desert'

def lz4_block(src, dsize):
    out = bytearray(); i = 0; n = len(src)
    while i < n:
        tok = src[i]; i += 1
        lit = tok >> 4
        if lit == 15:
            while True:
                b = src[i]; i += 1; lit += b
                if b != 255: break
        out += src[i:i + lit]; i += lit
        if i >= n or len(out) >= dsize: break
        off = src[i] | (src[i + 1] << 8); i += 2
        ml = tok & 15
        if ml == 15:
            while True:
                b = src[i]; i += 1; ml += b
                if b != 255: break
        ml += 4
        st = len(out) - off
        for k in range(ml): out.append(out[st + k])
    return bytes(out)

def c565(c):
    return ((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31

def dxt5(data, w, h):
    px = bytearray(w * h * 4); p = 0
    for by in range(0, h, 4):
        for bx in range(0, w, 4):
            a0, a1 = data[p], data[p + 1]
            abits = int.from_bytes(data[p + 2:p + 8], 'little')
            al = [a0, a1] + ([(a0 * (7 - k) + a1 * k) // 7 for k in range(1, 7)] if a0 > a1 else
                             [(a0 * (5 - k) + a1 * k) // 5 for k in range(1, 5)] + [0, 255])
            c0, c1 = struct.unpack_from('<HH', data, p + 8)
            bits = struct.unpack_from('<I', data, p + 12)[0]
            r0, r1 = c565(c0), c565(c1)
            cols = [r0, r1, tuple((2 * a + b) // 3 for a, b in zip(r0, r1)), tuple((a + 2 * b) // 3 for a, b in zip(r0, r1))]
            for k in range(16):
                x, y = bx + (k & 3), by + (k >> 2)
                c = cols[(bits >> (2 * k)) & 3]; a = al[(abits >> (3 * k)) & 7]
                q = (y * w + x) * 4
                px[q:q + 4] = bytes((c[0], c[1], c[2], a))
            p += 16
    return px

def png(path, w, h, px):
    raw = b''.join(b'\0' + bytes(px[y * w * 4:(y + 1) * w * 4]) for y in range(h))
    ch = lambda t, d: struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d))
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + ch(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) +
                           ch(b'IDAT', zlib.compress(raw, 6)) + ch(b'IEND', b''))

if __name__ == '__main__':
    group, sub, out = sys.argv[1:4]
    entries, _ = pamt.records(os.path.join(G, group, '0.pamt'))
    e = next(r for r in entries if sub.lower() in r[0].lower())
    with open(os.path.join(G, group, f'{e[1]}.paz'), 'rb') as f:
        f.seek(e[2]); raw = f.read(e[3])
    h, w = struct.unpack_from('<II', raw, 12)
    body = lz4_block(raw[128:], e[4] - 128) if e[5] == 1 and e[3] != e[4] else raw[128:]  # (stored: packed == unpacked)
    print(e, w, h, len(body))
    png(out, w, h, dxt5(body, w, h))
