import zlib, struct
def load(path):
    d = open(path, 'rb').read(); i = 8; idat = b''; w = h = 0
    while i < len(d):
        n = struct.unpack('>I', d[i:i + 4])[0]; t = d[i + 4:i + 8]
        if t == b'IHDR': w, h = struct.unpack('>II', d[i + 8:i + 16])
        if t == b'IDAT': idat += d[i + 8:i + 8 + n]
        i += 12 + n
    raw = zlib.decompress(idat); s = w * 4 + 1
    return w, h, lambda x, y: raw[y * s + 1 + x * 4:y * s + 5 + x * 4]
