"""Crimson Desert PAZ extraction: decrypt (ChaCha20, filename-derived key) + LZ4.

Key derivation follows the community's work (lazorr410/crimson-desert-unpacker,
NattKh/CrimsonDesertModdingTools paz_crypto.py).
"""
import os, struct, sys
try:  # fast paths; the pure-Python fallbacks below are slow but fine for small UI files
    import lz4.block
except ImportError:
    lz4 = None
try:
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms
except ImportError:
    Cipher = None

import pamt

HASH_INITVAL = 0x000C5EDE
IV_XOR = 0x60616263
XOR_DELTAS = [0x00000000, 0x0A0A0A0A, 0x0C0C0C0C, 0x06060606,
              0x0E0E0E0E, 0x0A0A0A0A, 0x06060606, 0x02020202]
FLAG_COMPRESSED = 0x02
FLAG_ENCRYPTED = 0x30


def _rot(v, k):
    return ((v << k) | (v >> (32 - k))) & 0xFFFFFFFF


def hashlittle(data, initval=0):
    M = 0xFFFFFFFF
    length = len(data)
    a = b = c = (0xDEADBEEF + length + initval) & M
    off = 0
    while length > 12:
        a = (a + struct.unpack_from('<I', data, off)[0]) & M
        b = (b + struct.unpack_from('<I', data, off + 4)[0]) & M
        c = (c + struct.unpack_from('<I', data, off + 8)[0]) & M
        a = (a - c) & M; a ^= _rot(c, 4);  c = (c + b) & M
        b = (b - a) & M; b ^= _rot(a, 6);  a = (a + c) & M
        c = (c - b) & M; c ^= _rot(b, 8);  b = (b + a) & M
        a = (a - c) & M; a ^= _rot(c, 16); c = (c + b) & M
        b = (b - a) & M; b ^= _rot(a, 19); a = (a + c) & M
        c = (c - b) & M; c ^= _rot(b, 4);  b = (b + a) & M
        off += 12; length -= 12
    if length == 0:
        return c
    tail = data[off:] + b'\0' * 12
    w0, w1, w2 = struct.unpack_from('<III', tail)
    if length > 8:
        c = (c + (w2 & (M >> (8 * (12 - length))))) & M
    if length > 4:
        b = (b + (w1 if length >= 8 else w1 & (M >> (8 * (8 - length))))) & M
    a = (a + (w0 if length >= 4 else w0 & (M >> (8 * (4 - length))))) & M
    c ^= b; c = (c - _rot(b, 14)) & M
    a ^= c; a = (a - _rot(c, 11)) & M
    b ^= a; b = (b - _rot(a, 25)) & M
    c ^= b; c = (c - _rot(b, 16)) & M
    a ^= c; a = (a - _rot(c, 4)) & M
    b ^= a; b = (b - _rot(a, 14)) & M
    c ^= b; c = (c - _rot(b, 24)) & M
    return c


def decrypt(data, path):
    seed = hashlittle(os.path.basename(path).lower().encode(), HASH_INITVAL)
    iv = struct.pack('<I', seed) * 4
    key = b''.join(struct.pack('<I', (seed ^ IV_XOR) ^ d) for d in XOR_DELTAS)
    if Cipher:
        return Cipher(algorithms.ChaCha20(key, iv), mode=None).encryptor().update(data)
    return chacha20(key, iv, data)


def chacha20(key, iv, data):
    """ChaCha20 as `cryptography` takes it: 16-byte nonce = 32-bit LE counter + 96-bit nonce."""
    M = 0xFFFFFFFF
    k = struct.unpack('<8I', key)
    counter, n0, n1, n2 = struct.unpack('<4I', iv)

    def qr(x, a, b, c, d):
        x[a] = (x[a] + x[b]) & M; x[d] ^= x[a]; x[d] = ((x[d] << 16) | (x[d] >> 16)) & M
        x[c] = (x[c] + x[d]) & M; x[b] ^= x[c]; x[b] = ((x[b] << 12) | (x[b] >> 20)) & M
        x[a] = (x[a] + x[b]) & M; x[d] ^= x[a]; x[d] = ((x[d] << 8) | (x[d] >> 24)) & M
        x[c] = (x[c] + x[d]) & M; x[b] ^= x[c]; x[b] = ((x[b] << 7) | (x[b] >> 25)) & M

    out = bytearray()
    for blk in range(0, len(data), 64):
        st = [0x61707865, 0x3320646e, 0x79622d32, 0x6b206574, *k, (counter + blk // 64) & M, n0, n1, n2]
        x = st[:]
        for _ in range(10):
            qr(x, 0, 4, 8, 12); qr(x, 1, 5, 9, 13); qr(x, 2, 6, 10, 14); qr(x, 3, 7, 11, 15)
            qr(x, 0, 5, 10, 15); qr(x, 1, 6, 11, 12); qr(x, 2, 7, 8, 13); qr(x, 3, 4, 9, 14)
        ks = struct.pack('<16I', *((x[i] + st[i]) & M for i in range(16)))
        chunk = data[blk:blk + 64]
        out += bytes(a ^ b for a, b in zip(chunk, ks))
    return bytes(out)


def lz4_block(src, size):
    """LZ4 block format decompression (no frame)."""
    out = bytearray()
    i, n = 0, len(src)
    while i < n:
        tok = src[i]; i += 1
        lit = tok >> 4
        if lit == 15:
            while True:
                b = src[i]; i += 1; lit += b
                if b != 255:
                    break
        out += src[i:i + lit]; i += lit
        if i >= n:
            break
        off = src[i] | (src[i + 1] << 8); i += 2
        ml = tok & 15
        if ml == 15:
            while True:
                b = src[i]; i += 1; ml += b
                if b != 255:
                    break
        ml += 4
        start = len(out) - off
        for j in range(ml):
            out.append(out[start + j])
    return bytes(out[:size])


def read_entry(group_dir, entry):
    path, pazidx, off, csize, dsize, flags = entry
    with open(os.path.join(group_dir, f'{pazidx}.paz'), 'rb') as f:
        f.seek(off)
        data = f.read(csize)
    if flags & FLAG_ENCRYPTED:
        data = decrypt(data, path)
    if flags & FLAG_COMPRESSED:
        data = lz4.block.decompress(data, uncompressed_size=dsize) if lz4 else lz4_block(data, dsize)
    return data


def extract(pamt_path, group_dir, out_dir, match):
    entries, _ = pamt.records(pamt_path)
    n = 0
    for e in entries:
        if match(e[0]):
            dst = os.path.join(out_dir, e[0])
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            open(dst, 'wb').write(read_entry(group_dir, e))
            n += 1
    return n


if __name__ == '__main__':
    # paz.py <pamt> <group_dir> <out_dir> <substring>...
    pamt_path, group_dir, out_dir, *subs = sys.argv[1:]
    n = extract(pamt_path, group_dir, out_dir, lambda p: any(s in p for s in subs))
    print(f'extracted {n} file(s)')
