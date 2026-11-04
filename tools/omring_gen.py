"""Outfit Manager: the Outfits-tab wheel art for N = 1..12 visible slots: generated ring / focus / worn textures for the
counts vanilla has no wheel for, the atlas entries that name them and the browser mod's quickslotitem.css.merge.

Vanilla has four wheels (ring, slot radius, hover wedge, equipped wedge; pieces cut 1:1 from the atlases into
re/omring/vanilla/ by vref.py), and N = 4, 8, 10, 12 use them unchanged (CSS only):
   4: bg_square   208  square_focus 400x182   square_select 400x189     (comrades wheel)
   8: bg_00       224  focus_bg 232x152       selected_bg 232x165       (food wheel)
  10: armor_bg    224  armor_focus 178x145    armor_select_bg 178x164   (armor wheel)
  12: armor_bg_01 230  armor_focus 150x145    armor_select_bg 150x164   (vanilla squeezes the 10 focus for 12; the
                                                                         worn wedge gets the same squeeze)
Every other N is built from the vanilla wheel with the nearest segment size (FAMILY): its radius, and
ring_N : N segments, each one whole family segment (N = 2: two, inner gap painted over; N = 1: the gapless ring),
         angularly stretched / squeezed in the middle only; within GAP_B deg of each gap 1:1, so gaps are the vanilla ones.
focus_N, worn_N: the family's wedge, sampled where vanilla draws it (centred on the slot point, at its CSS size), with
         its angles remapped about the ring centre and radii untouched (all vanilla wedges are concentric with the ring):
         within WB deg of each side 1:1 (edges, rims, the worn arc's overshoot), the middle stretched (N = 1, 2: tiled).
Output: 900px images shown at 650px centred on the ring centre, written as 1024x1024 DXT5 DDS (image at 1,1) + PNGs.

omring_gen.py [N ...]   (default: all)
"""
import math, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pngread, tex

ROOT = r'F:\MyProjects\CDInventory'
VAN = os.path.join(ROOT, 're', 'omring', 'vanilla')
GEN = os.path.join(ROOT, 're', 'omring', 'gen')
MOD = r'F:\MyProjects\CDInventory\mod\Outfit Manager\files\0012'   # the mod's source (DMM's copy is an install)
IMG_DIR = 'ui/texture/image/customizeimage'   # DMM routes new DDS here (as Barber Unlocked's)

S = 900                 # ring image size (and generated canvases)
C = (S - 1) / 2         # centre in pixel-index coordinates
DISP = 650              # displayed size
TEX = 1024              # DDS size (image at 1,1)
SLOTS = 12              # Outfits tab slots
WRAP = 50               # quickslot-item-wrap-12 size

# family: ring (texture, png), slot radius, focus / worn (texture, png, CSS width, height)
P = 'cd_hud_itemquickslot_'
FAM = {
    4: dict(ring=(P + 'bg_square', 'bg_square'), R=208,
            focus=(P + 'square_focus', 'square_focus', 400, 182), worn=(P + 'square_select', 'square_select', 400, 189)),
    8: dict(ring=(P + 'bg_00', 'bg_00'), R=224,
            focus=(P + 'focus_bg', 'focus_bg', 232, 152), worn=(P + 'selected_bg', 'selected_bg', 232, 165)),
    10: dict(ring=(P + 'armor_bg', 'armor_bg'), R=224,
             focus=(P + 'armor_focus', 'armor_focus', 178, 145), worn=(P + 'armor_select_bg', 'armor_select_bg', 178, 164)),
    12: dict(ring=(P + 'armor_bg_01', 'armor_bg_01'), R=230,
             focus=(P + 'armor_focus', 'armor_focus', 150, 145), worn=(P + 'armor_select_bg', 'armor_select_bg', 150, 164)),
}
FAMILY = {1: 4, 2: 4, 3: 4, 5: 4, 6: 8, 7: 8, 9: 10, 11: 10}   # nearest segment size (9: vanilla also uses the 10 ring)
GENERATED = sorted(FAMILY)


def fam(n): return FAM[FAMILY.get(n, n)]


ICON_R4 = 226           # 4-family: slot icons on the middle of its wood band (vanilla's 208 is for the big comrade
                        # portraits and leaves small icons hugging the inner edge); the wedges stay on vanilla's 208


def icon_r(n): return ICON_R4 if FAMILY.get(n, n) == 4 else fam(n)['R']


GAP_B = 6.0             # ring: deg on each side of a gap kept 1:1 (the gap's dark core + shading reach ~4.5)
GAP_SEAM = 1.5          # ring: deg across a gap over which the two neighbouring segments' gap halves cross-fade
GL_IN, GL_FADE = 4.5, 2.5   # gapless ring: gap band replaced within 4.5 deg, faded out over 2.5 more
WB = 4.5                # wedges: deg inside each side kept 1:1
MAX_STRETCH = 1.5       # wedges: above this the middle is tiled instead of stretched
TILE_SEAM = 2.0         # wedges: half width (deg) of the cross-fade between tiles


def load(path):
    w, h, px = pngread.load(path)
    buf = bytearray(w * h * 4)
    for y in range(h):
        for x in range(w):
            q = (y * w + x) * 4
            buf[q:q + 4] = px(x, y)
    return w, h, buf


def van(nm): return load(os.path.join(VAN, nm + '.png'))


def sample(img, x, y):
    """Bilinear RGBA (premultiplied interpolation) at pixel-index coordinates; outside = transparent."""
    w, h, buf = img
    x0, y0 = math.floor(x), math.floor(y)
    fx, fy = x - x0, y - y0
    acc = [0.0, 0.0, 0.0, 0.0]
    for yy, wy in ((y0, 1 - fy), (y0 + 1, fy)):
        if wy == 0 or yy < 0 or yy >= h: continue
        for xx, wx in ((x0, 1 - fx), (x0 + 1, fx)):
            k = wx * wy
            if k == 0 or xx < 0 or xx >= w: continue
            q = (yy * w + xx) * 4
            a = buf[q + 3] * k
            acc[0] += buf[q] * a; acc[1] += buf[q + 1] * a; acc[2] += buf[q + 2] * a; acc[3] += a
    a = acc[3]
    if a <= 1e-6: return (0.0, 0.0, 0.0, 0.0)
    return (acc[0] / a, acc[1] / a, acc[2] / a, a)


def polar_sample(img, r, deg):
    """Ring image at display radius r / angle deg (0 = up, clockwise) about its centre."""
    t = math.radians(deg)
    k = S / DISP
    return sample(img, C + r * k * math.sin(t), C - r * k * math.cos(t))


def mix(p, q, t):
    """Blend two RGBA pixels (premultiplied): p at t = 0, q at t = 1."""
    a = p[3] * (1 - t) + q[3] * t
    if a <= 1e-6: return (0.0, 0.0, 0.0, 0.0)
    return tuple((p[i] * p[3] * (1 - t) + q[i] * q[3] * t) / a for i in range(3)) + (a,)


def smooth(t):
    t = max(0.0, min(1.0, t))
    return t * t * (3 - 2 * t)


def put(buf, w, x, y, p):
    q = (y * w + x) * 4
    buf[q:q + 4] = bytes(max(0, min(255, int(round(v)))) for v in p)


def disp_polar(x, y):
    """Canvas pixel -> display radius and angle (-180..180, 0 = up, clockwise) about the ring centre."""
    k = DISP / S
    X, Y = (x + 0.5) * k - DISP / 2, (y + 0.5) * k - DISP / 2
    return math.hypot(X, Y), math.degrees(math.atan2(X, -Y))


# --- ring -----------------------------------------------------------------------------------------
def gapless(src, m):
    """The family ring without gaps (N = 1, and the inner gap of an N = 2 segment): each gap band is replaced by the
    wood 2 * (GL_IN + GL_FADE) deg before it."""
    hs = 180 / m
    gb = GL_IN + GL_FADE
    out = bytearray(src[2])
    for y in range(S):
        for x in range(S):
            r, phi = disp_polar(x, y)
            d = (phi - hs) % (2 * hs)
            d = d - 2 * hs if d > hs else d
            ad = abs(d)
            if ad >= gb: continue
            w = 1.0 if ad <= GL_IN else smooth(1 - (ad - GL_IN) / GL_FADE)
            q = (y * S + x) * 4
            here = tuple(float(v) for v in src[2][q:q + 4])
            put(out, S, x, y, mix(here, polar_sample(src, r, phi - 2 * gb), w))
    return (S, S, out)


def ring(src, gl, n, m):
    """N segments from the family ring (m segments). Segment j (centre j * 360/N) is the source segment nearest in
    order (N = 2: a pair centred on a gap), stretched in the middle; a source segment used twice in a row is mirrored.
    The two gap halves of neighbouring segments cross-fade over GAP_SEAM deg (they may come from different gaps)."""
    if n == 1: return gl
    hs, hn = 180 / m, 180 / n
    k = 2 if n == 2 else 1
    hw = k * hs                                          # source half-span
    off = 0.0 if k == 1 else hs                          # span centre: a segment centre (k = 1) or a gap (k = 2)
    cen, mir = [], []
    for j in range(n):
        c = (off + 2 * hs * round((j * 2 * hn - off) / (2 * hs))) % 360 if k == 2 else 2 * hs * (round(j * m / n) % m)
        mir.append(bool(cen) and c == cen[-1] and not mir[-1])
        cen.append(c)

    def seg(j, r, l):
        e = hn - abs(l)                                  # deg inside this segment's gap (< 0: past it)
        es = e if e < GAP_B else GAP_B + (e - GAP_B) * (hw - GAP_B) / (hn - GAP_B)
        sg = math.copysign(1, l) * (-1 if mir[j] else 1)
        s = cen[j] + sg * (hw - es)
        if k == 1: return polar_sample(src, r, s)
        p = polar_sample(gl, r, s)                       # the span's own inner gap painted over
        w = 1.0 if es <= GL_IN else smooth(1 - (es - GL_IN) / GL_FADE) if es < GL_IN + GL_FADE else 0.0
        return mix(p, polar_sample(src, r, s), w) if w > 0 else p

    out = bytearray(S * S * 4)
    for y in range(S):
        for x in range(S):
            r, phi = disp_polar(x, y)
            j = round(phi / (2 * hn)) % n
            l = (phi - j * 2 * hn + 180) % 360 - 180
            p = seg(j, r, l)
            e = hn - abs(l)
            if e < GAP_SEAM:
                sg = 1 if l >= 0 else -1
                q = seg((j + sg) % n, r, l - sg * 2 * hn)
                p = mix(q, p, 0.5 + 0.5 * smooth(e / GAP_SEAM))
            if p[3] > 0: put(out, S, x, y, p)
    return (S, S, out)


RO_IN = 260.0           # ring outer edge: radii below this untouched (the disc and most of the wood)
RO_BASE, RO_RAMP = 2.0, 9.0   # ... edge baseline = median over +-RO_BASE deg; its steps become ramps (mean over +-RO_RAMP)
RO_GAP, RO_GAP_FADE = 6.0, 3.0   # ... except within RO_GAP deg of a real gap (its corners stay vanilla), faded over 3 more


def round_outer(img, n):
    """The ring's outer edge (outermost alpha 128) evened out: vanilla segments run ~3 px higher before a gap than after
    it, so a painted-over gap leaves a step in the circle. Each angle's wood between RO_IN and the edge is radially
    remapped so the edge's baseline steps become ramps; the fine roughness of the edge stays."""
    def edge(d):
        r = 312.0
        while r > 250 and polar_sample(img, r, d)[3] < 128: r -= 0.5
        lo, hi = r, r + 0.5
        for _ in range(8):
            mid = (lo + hi) / 2
            if polar_sample(img, mid, d)[3] >= 128: lo = mid
            else: hi = mid
        return lo
    st = 4                                               # samples per deg
    L = 360 * st
    es = [edge(i / st) for i in range(L)]
    gd = [min(abs((i / st - (180 / n + j * 360 / n) + 180) % 360 - 180) for j in range(n)) if n > 1 else 999.0
          for i in range(L)]
    kb, kr = int(RO_BASE * st), int(RO_RAMP * st)
    base = [sorted(es[(i + j) % L] for j in range(-kb, kb + 1))[kb] for i in range(L)]
    tg = []
    for i in range(L):
        w = smooth((gd[i] - RO_GAP) / RO_GAP_FADE)
        if w <= 0: tg.append(es[i]); continue
        vs = [base[(i + j) % L] for j in range(-kr, kr + 1) if gd[(i + j) % L] > RO_GAP]
        tg.append(es[i] + w * (sum(vs) / len(vs) - base[i]))
    out = bytearray(img[2])
    for y in range(S):
        for x in range(S):
            r, phi = disp_polar(x, y)
            if r < RO_IN or r > 315: continue
            i = (phi % 360) * st
            i0 = int(i) % L; f = i - int(i); i1 = (i0 + 1) % L
            e = es[i0] * (1 - f) + es[i1] * f
            t = tg[i0] * (1 - f) + tg[i1] * f
            if abs(t - e) < 0.05: continue
            put(out, S, x, y, polar_sample(img, RO_IN + (r - RO_IN) * (e - RO_IN) / (t - RO_IN), phi))
    return (S, S, out)


# --- wedges ---------------------------------------------------------------------------------------
def at(piece, pw, ph, R, r, psi):
    """The vanilla wedge drawn like vanilla (pw x ph, centred on the slot point R px above the ring centre, slot at the
    top), sampled at display radius r / angle psi about the ring centre."""
    t = math.radians(psi)
    X, Y = r * math.sin(t), R - r * math.cos(t)
    return sample(piece, (X + pw / 2) * piece[0] / pw - 0.5, (Y + ph / 2) * piece[1] / ph - 0.5)


def edge_fits(piece, pw, ph, R):
    """The wedge's alpha-128 edges along the radius (focus: inner, outer; worn: arc in / out, body in / out) as smooth
    fits r = a + b * psi^2 over its clean middle (|psi| <= TILE_SRC; the brushed corners beyond break up)."""
    rows = []
    for psi in range(0, TILE_SRC + 1, 2):
        e, on = [], False
        for r10 in range(1300, 3250, 2):
            r = r10 / 10
            a = at(piece, pw, ph, R, r, psi)[3] >= 128
            if a != on: e.append(r); on = a
        rows.append((psi * psi, e))
    use = [(x, e) for x, e in rows if len(e) == len(rows[0][1])]
    fits = []
    for i in range(len(rows[0][1])):
        xs, ys = [x for x, _ in use], [e[i] for _, e in use]
        mx, my = sum(xs) / len(xs), sum(ys) / len(ys)
        b = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sum((x - mx) ** 2 for x in xs)
        fits.append((my - b * mx, b))
    return fits


def norm_r(fits, r, psi, k=0.0):
    """Display radius at the wedge centre -> the radius at source angle psi with the same place between the edges, so
    tiled copies keep the centre's edge radii (no wavy rims). k = 1: the source's own radii (no change)."""
    sv = [a + b * psi * psi for a, b in fits]
    tg = [a + k * (s - a) for (a, b), s in zip(fits, sv)]
    if r <= tg[0]: return r + sv[0] - tg[0]
    for i in range(1, len(tg)):
        if r <= tg[i]: return sv[i - 1] + (r - tg[i - 1]) * (sv[i] - sv[i - 1]) / (tg[i] - tg[i - 1])
    return r + sv[-1] - tg[-1]


TILE_SRC = 28           # tiled wedges: the source's clean middle half width (deg); the rest is kept 1:1 at the sides
RIM_IN, RIM_OUT, RIM_FADE = 3.5, 1.5, 1.5   # tiled wedges: px inside / outside each edge drawn from one continuous
                                            # stretch (the rims and the worn arc), faded into the tiles over RIM_FADE
LIGHT_BLUR = 8          # tiled wedges: source px box blur giving the large-scale light, which follows the stretch too
GLINT = (19, -3)        # 1 slot: square_focus's glint spans psi -19..-3 incl. the fades (-(GLINT[0]) .. GLINT[1])


def blur(img, rad):
    """Two-pass box blur (premultiplied) of a piece: its large-scale light without the hatching."""
    w, h, buf = img
    pm = [[buf[q + c] * buf[q + 3] / 255 for c in range(3)] + [buf[q + 3]] for q in range(0, w * h * 4, 4)]
    for _ in range(2):
        for horiz in (True, False):
            nxt = []
            for y in range(h):
                for x in range(w):
                    acc = [0.0] * 4; cnt = 0
                    for d in range(-rad, rad + 1):
                        xx, yy = (x + d, y) if horiz else (x, y + d)
                        if 0 <= xx < w and 0 <= yy < h:
                            p = pm[yy * w + xx]
                            for c in range(4): acc[c] += p[c]
                        cnt += 1
                    nxt.append([v / cnt for v in acc])
            pm = nxt
    out = bytearray(w * h * 4)
    for i, p in enumerate(pm):
        a = p[3]
        out[i * 4:i * 4 + 4] = bytes(max(0, min(255, round(v))) for v in ([p[c] * 255 / a for c in range(3)] if a > 0.5 else [0, 0, 0]) + [a])
    return (w, h, out)


def wedge(piece, pw, ph, R, n, m):
    """The family wedge (half angle 180/m) as a 360/N deg wedge pointing up (N = 1: a full annulus). Middles stretched
    more than MAX_STRETCH are tiled with copies of the source's clean middle instead (edges held on the centre's radii,
    seams cross-faded); then the brushed corners (|psi| > TILE_SRC) are the 1:1 side bands."""
    hs, ht = 180 / m, 180 / n
    wb = WB
    a_src = hs - WB                                      # source middle half width
    a_dst = 180.0 if n == 1 else ht - WB                 # target middle half width
    stretch = a_dst / a_src
    tiles = 0
    if stretch > MAX_STRETCH:
        wb = hs - TILE_SRC
        a_src, a_dst = TILE_SRC, (180.0 if n == 1 else ht - wb)
        tsrc = a_src - TILE_SEAM                         # inner tile edges + their seams stay in the clean middle
        tiles = max(2, round(a_dst / tsrc))
        twd = 2 * a_dst / tiles                          # tile width (deg)
        g = twd / (2 * tsrc)
        fits = edge_fits(piece, pw, ph, R)
        lit = blur(piece, LIGHT_BLUR)

    def cont(r, phi):
        """The continuous stretch of the source middle over the whole tiled span: source angle and radius."""
        if n == 1:
            return phi * a_src / 180, norm_r(fits, r, phi * a_src / 180)
        q = a_dst - abs(phi)                             # deg from the side band
        k = 1 - smooth(q / (twd / 2))                    # natural radii towards the sides (they are 1:1 there)
        ps = phi * a_src / a_dst
        return ps, norm_r(fits, r, ps, k)

    def lit_rgb(rr, ps):
        p = at(lit, pw, ph, R, rr, ps)
        return p if p[3] > 8 else None

    def fill(r, ps, p):
        """square_focus has a white glint inside its outer rim (psi -14..-8): fine once per vanilla wedge, but the full
        circle repeats it in every tile. Patched there from the clean source 20 deg to the right."""
        if n != 1 or len(fits) != 2: return p
        w = smooth((ps + GLINT[0]) / 3) * smooth((GLINT[1] - ps) / 3) * smooth((r - fits[1][0] + 12) / 3)
        if w <= 0: return p
        return mix(p, at(piece, pw, ph, R, norm_r(fits, r, ps + 20), ps + 20), w)

    def tsample(r, phi, ps, rr):
        """A tile sample with its large-scale light swapped for the continuous stretch's."""
        p = fill(r, ps, at(piece, pw, ph, R, rr, ps))
        if p[3] <= 0: return p
        lt = lit_rgb(rr, ps)
        cs, cr = cont(r, phi)
        ls = lit_rgb(cr, cs)
        if not lt or not ls: return p
        return tuple(max(0.0, min(255.0, p[c] * max(0.6, min(1.6, (ls[c] + 4) / (lt[c] + 4))))) for c in range(3)) + (p[3],)

    def rim_w(r):
        """1 on the rims (and the worn arc), fading to 0 into the tiles."""
        w = 0.0
        for i, (a, b) in enumerate(fits):
            d = (r - a) if i % 2 == 0 else (a - r)       # px inside the opaque side of edge i
            if d < -RIM_OUT - RIM_FADE: continue
            t = 1.0 if -RIM_OUT <= d <= RIM_IN else smooth(1 - (d - RIM_IN) / RIM_FADE) if d > RIM_IN else \
                smooth(1 - (-RIM_OUT - d) / RIM_FADE)
            w = max(w, t)
        return w

    def tile(r, phi):
        u = phi + a_dst
        t = math.floor(u / twd)
        if n > 1: t = max(0, min(tiles - 1, t))
        lu = u - (t + 0.5) * twd                         # deg from this tile's centre
        if n > 1 and ((t == 0 and lu < 0) or (t == tiles - 1 and lu > 0)):   # outer halves run into the side 1:1
            ps = lu * a_src / (twd / 2)
            p = tsample(r, phi, ps, r)
        else:
            p = tsample(r, phi, lu / g, norm_r(fits, r, lu / g))
            db = twd / 2 - abs(lu)
            if db < TILE_SEAM:
                o = (lu - math.copysign(twd, lu)) / g
                p = mix(tsample(r, phi, o, norm_r(fits, r, o)), p, 0.5 + 0.5 * smooth(db / TILE_SEAM))
        w = rim_w(r)
        if w > 0:
            cs, cr = cont(r, phi)
            q = fill(r, cs, at(piece, pw, ph, R, cr, cs))
            if n == 1 and 180 - abs(phi) < 4:            # the full circle's one seam (+-a_src meet at the bottom)
                o = phi - math.copysign(360, phi)
                q2 = at(piece, pw, ph, R, norm_r(fits, r, o * a_src / 180), o * a_src / 180)
                q = mix(q2, q, 0.5 + 0.5 * smooth((180 - abs(phi)) / 4))
            p = mix(p, q, w)
        return p

    out = bytearray(S * S * 4)
    for y in range(S):
        for x in range(S):
            r, phi = disp_polar(x, y)
            if r < 130 or r > 325: continue
            if n == 1:
                p = tile(r, phi)
            else:
                e = ht - abs(phi)                        # deg inside the side
                if e < -10: continue
                sg = 1 if phi >= 0 else -1
                if e <= wb:
                    p = at(piece, pw, ph, R, r, sg * (hs - e))
                elif not tiles:
                    p = at(piece, pw, ph, R, r, phi / stretch)
                else:
                    p = tile(r, phi)
            if p[3] > 0: put(out, S, x, y, p)
    return (S, S, out)


# --- DXT5 ---------------------------------------------------------------------------------------
def q565(r, g, b):
    return (min(31, round(r * 31 / 255)) << 11) | (min(63, round(g * 63 / 255)) << 5) | min(31, round(b * 31 / 255))


def enc_block(buf, w, bx, by):
    px = []
    for j in range(4):
        o = ((by + j) * w + bx) * 4
        row = buf[o:o + 16]
        for i in range(4): px.append(row[i * 4:i * 4 + 4])
    al = [p[3] for p in px]
    amin, amax = min(al), max(al)
    if amax == 0: return bytes(16)
    if amin == amax:
        ab = bytes((amax, amin)) + bytes(6)
    else:
        bits = 0
        for i, a in enumerate(al):
            s = round((amax - a) * 7 / (amax - amin))
            idx = 0 if s == 0 else 1 if s == 7 else s + 1
            bits |= idx << (3 * i)
        ab = bytes((amax, amin)) + bits.to_bytes(6, 'little')
    vis = [p for p in px if p[3] > 0]
    lo = [min(p[c] for p in vis) for c in range(3)]
    hi = [max(p[c] for p in vis) for c in range(3)]
    ins = [(hi[c] - lo[c]) / 16 for c in range(3)]
    c0 = q565(*(hi[c] - ins[c] for c in range(3)))
    c1 = q565(*(lo[c] + ins[c] for c in range(3)))
    if c0 < c1: c0, c1 = c1, c0
    if c0 == c1: return ab + struct.pack('<HHI', c0, c1, 0)
    e0, e1 = tex.c565(c0), tex.c565(c1)
    pal = [e0, e1, tuple((2 * a + b) / 3 for a, b in zip(e0, e1)), tuple((a + 2 * b) / 3 for a, b in zip(e0, e1))]
    bits = 0
    for i, p in enumerate(px):
        best = min(range(4), key=lambda k: (p[0] - pal[k][0]) ** 2 + (p[1] - pal[k][1]) ** 2 + (p[2] - pal[k][2]) ** 2)
        bits |= best << (2 * i)
    return ab + struct.pack('<HHI', c0, c1, bits)


def write_dds(path, img):
    canvas = bytearray(TEX * TEX * 4)
    for y in range(S):
        o = ((y + 1) * TEX + 1) * 4
        canvas[o:o + S * 4] = img[2][y * S * 4:(y + 1) * S * 4]
    body = bytearray()
    for by in range(0, TEX, 4):
        for bx in range(0, TEX, 4):
            body += enc_block(canvas, TEX, bx, by)
    hdr = struct.pack('<4sIIIIIII44sII4s20sI16s', b'DDS ', 124, 0x000A1007, TEX, TEX, len(body), 0, 1, bytes(44),
                      32, 4, b'DXT5', bytes(20), 0x1000, bytes(16))
    assert len(hdr) == 128
    with open(path, 'wb') as f: f.write(hdr + body)


# --- preview: the wheel composed like the game (650px) ---------------------------------------------
def compose(layers, tint_first=True):
    """layers: (img, w, h, cx, cy, deg) drawn in order, each scaled to w x h, centred on cx, cy, rotated deg; the first
    (the ring) multiplied by #333030 like the game. Over grey 60."""
    D = DISP
    out = [[60.0, 60.0, 60.0] for _ in range(D * D)]
    for li, (img, w, h, cx, cy, deg) in enumerate(layers):
        tint = (0x33 / 255, 0x30 / 255, 0x30 / 255) if (li == 0 and tint_first) else (1, 1, 1)
        t = math.radians(deg)
        ca, sa = math.cos(t), math.sin(t)
        rad = math.hypot(w, h) / 2 + 2
        for y in range(max(0, int(cy - rad)), min(D, int(cy + rad) + 1)):
            for x in range(max(0, int(cx - rad)), min(D, int(cx + rad) + 1)):
                dx, dy = x + 0.5 - cx, y + 0.5 - cy
                lx, ly = dx * ca + dy * sa, -dx * sa + dy * ca
                p = sample(img, (lx + w / 2) * img[0] / w - 0.5, (ly + h / 2) * img[1] / h - 0.5)
                if p[3] <= 0: continue
                a = p[3] / 255
                o = out[y * D + x]
                for c in range(3): o[c] = o[c] * (1 - a) + p[c] * tint[c] * a
    buf = bytearray(D * D * 4)
    for i, o in enumerate(out):
        buf[i * 4:i * 4 + 4] = bytes((int(o[0]), int(o[1]), int(o[2]), 255))
    return buf


def preview_layers(n, rg, fc, wn, focus_slot):
    """Ring, the worn wedge on the top slot, the focus on focus_slot (None: no focus)."""
    f = fam(n)
    layers = [(rg, DISP, DISP, DISP / 2, DISP / 2, 0)]
    for img, slot, key in ((wn, 0, 'worn'), (fc, focus_slot, 'focus')):
        if slot is None: continue
        th = slot * 360 / n
        if n in FAMILY or key == 'worn': # (the worn wedge is generated for every count)
            layers.append((img, DISP, DISP, DISP / 2, DISP / 2, th))
        else:
            _, _, pw, ph = f[key]
            t = math.radians(th)
            layers.append((img, pw, ph, DISP / 2 + f['R'] * math.sin(t), DISP / 2 - f['R'] * math.cos(t), th))
    return layers


def name_ring(n): return f'outfitmanager_ring_{n}'
def name_focus(n): return f'outfitmanager_focus_{n}'
def name_worn(n): return f'outfitmanager_worn_{n}'
def name_worn_amber(n): return f'outfitmanager_worn_amber_{n}'
def name_worn_green(n): return f'outfitmanager_worn_green_{n}'


AMBER = (1.25, 0.88, 0.45)  # the worn wedge of an outfit changed by hand since it was loaded (not saved): white -> amber
# The "+" slot while an unsaved set is worn (ready to save): the game's own green #31BA7E (common-color-green-01 /
# font-text-caption-minor-green-positive), at the line's brightness (a ~200 grey -> 1.4 x that colour).
GREEN = tuple(c * 1.4 / 200 for c in (0x31, 0xBA, 0x7E))


def worn_src(f):
    """The family's worn wedge as the game shows it: the armor wheels draw the white line under the wedge's inner edge
    as a texture of its own (armor_select_line, same canvas as armor_select_bg); the food / comrade wheels have it in."""
    w, h, b = van(f['worn'][1])
    if f['worn'][1] != 'armor_select_bg':
        return w, h, b
    lw, lh, lb = van('armor_select_line')
    assert (lw, lh) == (w, h)
    out = bytearray(b)
    for i in range(0, len(out), 4):
        la, ba = lb[i + 3] / 255, out[i + 3] / 255
        if la <= 0: continue
        oa = la + ba * (1 - la)
        for c in range(3): out[i + c] = min(255, int(round((lb[i + c] * la + out[i + c] * ba * (1 - la)) / oa)))
        out[i + 3] = min(255, int(round(oa * 255)))
    return w, h, out


def tinted(img, k):
    """Only the bright parts take the tint (edges, hatching, the line: luminance ~185); the dark brushed fill (~50-75)
    stays as it is."""
    w, h, b = img
    out = bytearray(b)
    for i in range(0, len(out), 4):
        t = min(1.0, max(0.0, ((out[i] + out[i + 1] + out[i + 2]) / 3 - 85) / 70))
        if t <= 0: continue
        for c in range(3): out[i + c] = min(255, int(out[i + c] * (1 + (k[c] - 1) * t)))
    return w, h, out


def gen(n):
    f = fam(n)
    m = FAMILY.get(n, n)
    d = os.path.join(MOD, *IMG_DIR.split('/'))
    # The worn wedge is generated for every count (vanilla counts 1:1): one source with the white line, and its amber copy.
    wn = wedge(worn_src(f), f['worn'][2], f['worn'][3], f['R'], n, m)
    wa = tinted(wn, AMBER)
    wg = tinted(wn, GREEN)
    for img, nm, png in ((wn, name_worn(n), 'worn'), (wa, name_worn_amber(n), 'worn_amber'), (wg, name_worn_green(n), 'worn_green')):
        tex.png(os.path.join(GEN, f'{png}_{n}.png'), S, S, img[2])
        write_dds(os.path.join(d, nm + '.dds'), img)
    if n in FAMILY:
        src = van(f['ring'][1])
        gl = load(os.path.join(GEN, f'gapless_{f["ring"][1]}.png')) if n <= 2 else None
        rg = round_outer(ring(src, gl, n, m), n)
        fc = wedge(van(f['focus'][1]), f['focus'][2], f['focus'][3], f['R'], n, m)
        for img, nm, png in ((rg, name_ring(n), 'ring'), (fc, name_focus(n), 'focus')):
            tex.png(os.path.join(GEN, f'{png}_{n}.png'), S, S, img[2])
            write_dds(os.path.join(d, nm + '.dds'), img)
    else:
        rg, fc = van(f['ring'][1]), van(f['focus'][1])
    tex.png(os.path.join(GEN, f'preview_{n}.png'), DISP, DISP, compose(preview_layers(n, rg, fc, wn, 1 % n)))
    tex.png(os.path.join(GEN, f'preview_{n}_worn.png'), DISP, DISP, compose(preview_layers(n, rg, fc, wn, None)))
    tex.png(os.path.join(GEN, f'preview_{n}_amber.png'), DISP, DISP, compose(preview_layers(n, rg, fc, wa, None)))
    return n


def write_atlas():
    """Registers the ring / focus / worn textures in the merge for the game's texture list (DMM adds them to whatever
    copy of the list it mounts; the mod ships no whole copy, so it neither masks a game update nor clashes with another
    mod's list). The merge's other lines (badge_gen.py, om_key_icons.py) stay."""
    names = [nm for n in GENERATED for nm in (name_ring(n), name_focus(n))]
    names += [nm for n in range(1, SLOTS + 1) for nm in (name_worn(n), name_worn_amber(n), name_worn_green(n))]
    path = os.path.join(MOD, 'ui', 'xml', 'texture', 'cd_hud_itemquickslot_08.xml.merge')
    raw = open(path, 'rb').read()
    bom, nl = raw.startswith(b'\xef\xbb\xbf'), '\r\n' if b'\r\n' in raw else '\n'  # (written back as it was)
    mine = ('outfitmanager_ring_', 'outfitmanager_focus_', 'outfitmanager_worn_')
    lines = [l for l in raw.decode('utf-8-sig').replace('\r\n', '\n').split('\n') if not any(m in l for m in mine)]
    at = next(i for i, l in enumerate(lines) if '<__dmm_root__>' in l) + 1
    lines[at:at] = [f'<Texture Name="{nm}" Filename="UI/texture/image/customizeimage/{nm}.dds" Type="Image" '
                    f'GetRect="1,1,{S},{S}"/>' for nm in names]
    open(path, 'wb').write((('\ufeff' if bom else '') + nl.join(lines)).encode('utf-8'))


CSS_HEAD = """/* Outfit Manager: Outfits tab looks (classes set by OutfitManager.asi; merged into the game's file by DMM).
   Generated by tools/omring_gen.py. */
.self-center.om-empty-armor { opacity: 0.35; width: 105px; height: 105px; background-image: textureid(cd_hud_itemquickslot_catecory_img_armor); }
/* The Appearance view's page slot on its Armour step: where a click goes, the weapons (the wheel's own silhouette). */
.self-center.om-empty-sword { opacity: 0.35; width: 105px; height: 105px; background-image: textureid(cd_hud_itemquickslot_catecory_img_sword); }
.self-center.om-empty-necklace { opacity: 0.35; width: 105px; height: 105px; background-image: textureid(cd_hud_itemquickslot_catecory_img_necklace); }
/* The centre disc's caption (the item-count line) describing the highlighted entry of a submenu page: wrapped inside
   the disc. */
.quickslot-center-tooltip-item-count.om-desc { width: 230px; text-align: center; line-height: 20px; }
/* Slot badges (the "+" slot's "+", cog = settings mode, flag = conditions mode): our own textures made like the
   vanilla "+" (tools/badge_gen.py: grey worn fill, dark outline, 80px canvas, 50px box), placed where the game puts
   its own slot badge (the lantern's Auto arrows, drawn into ItemIcon_Lantern_auto: centre +12.8 / +29.1 from the
   92px picture's centre, ~23px): on the slot's occupancy-chain element (unused by our slots, given self-center by the
   plugin), above the picture and the focus, so like the game's it does not change with the focus. */
.self-center.om-plus-c { display: block; opacity: 1; left: 50%; top: 50%; width: 50px; height: 50px; rotate-z: 0deg; translate-x: -12px; translate-y: 4px; background-image: textureid(Common_Icon_Keyguide_Plus); }
.self-center.om-cog-c { display: block; opacity: 1; left: 50%; top: 50%; width: 50px; height: 50px; rotate-z: 0deg; translate-x: -12px; translate-y: 4px; background-image: textureid(outfitmanager_badge_cog); }
.self-center.om-cross-c { display: block; opacity: 1; left: 50%; top: 50%; width: 34px; height: 34px; rotate-z: 0deg; translate-x: -17px; translate-y: 4px; background-image: textureid(outfitmanager_key_cross); }
.self-center.om-abtn-c { display: block; opacity: 1; left: 50%; top: 50%; width: 34px; height: 34px; rotate-z: 0deg; translate-x: -17px; translate-y: 4px; background-image: textureid(outfitmanager_key_a); }
.self-center.om-flag-c { display: block; opacity: 1; left: 50%; top: 50%; width: 50px; height: 50px; rotate-z: 0deg; translate-x: -12px; translate-y: 4px; background-image: textureid(outfitmanager_badge_flag); }
/* Condition markers (main mode, saved outfits): the game's own thermometers, in their colours, at the badge spot (centre
   +13 / +29 from the picture's centre, like the lantern's "Auto"); the 46px icons fill their box, so 26px reads as large
   as the other badges' glyphs. */
.self-center.om-cold-c { display: block; opacity: 1; left: 50%; top: 50%; width: 26px; height: 26px; rotate-z: 0deg; translate-x: 0px; translate-y: 16px; background-image: textureid(cd_icon_temperature_freeze_00); }
.self-center.om-heat-c { display: block; opacity: 1; left: 50%; top: 50%; width: 26px; height: 26px; rotate-z: 0deg; translate-x: 0px; translate-y: 16px; background-image: textureid(cd_icon_temperature_burn_00); }
/* Resistance Lv. 15+ (immune to Freeze / Burn): the "phase" thermometers. */
.self-center.om-cold2-c { display: block; opacity: 1; left: 50%; top: 50%; width: 26px; height: 26px; rotate-z: 0deg; translate-x: 0px; translate-y: 16px; background-image: textureid(cd_icon_temperature_freeze_phase_00); }
.self-center.om-heat2-c { display: block; opacity: 1; left: 50%; top: 50%; width: 26px; height: 26px; rotate-z: 0deg; translate-x: 0px; translate-y: 16px; background-image: textureid(cd_icon_temperature_burn_phase_00); }
/* Appearance. om-eye-c: the eye (badge spot): the eye scroll mode on the occupancy-chain element, and the marker of a
   saved outfit with looks set (main mode) on its charge-count wrap (a second badge element, given self-center by the
   plugin); om-eye2-c: that marker above the climate thermometer (same centre as the badges, one badge up). In the
   outfit's view (its pieces): om-look-*-c, the piece's look (Default: none). */
.self-center.om-eye-c { display: block; opacity: 1; left: 50%; top: 50%; width: 50px; height: 50px; rotate-z: 0deg; translate-x: -12px; translate-y: 4px; background-image: textureid(outfitmanager_badge_eye); }
.self-center.om-eye2-c { display: block; opacity: 1; left: 50%; top: 50%; width: 50px; height: 50px; rotate-z: 0deg; translate-x: -12px; translate-y: -20px; background-image: textureid(outfitmanager_badge_eye); }
.self-center.om-look-shown-c { display: block; opacity: 1; left: 50%; top: 50%; width: 50px; height: 50px; rotate-z: 0deg; translate-x: -12px; translate-y: 4px; background-image: textureid(outfitmanager_badge_eye); }
.self-center.om-look-hidden-c { display: block; opacity: 1; left: 50%; top: 50%; width: 50px; height: 50px; rotate-z: 0deg; translate-x: -12px; translate-y: 4px; background-image: textureid(outfitmanager_badge_hidden); }
.self-center.om-look-combat-c { display: block; opacity: 1; left: 50%; top: 50%; width: 50px; height: 50px; rotate-z: 0deg; translate-x: -12px; translate-y: 4px; background-image: textureid(outfitmanager_badge_combat); }
.self-center.om-look-peace-c { display: block; opacity: 1; left: 50%; top: 50%; width: 50px; height: 50px; rotate-z: 0deg; translate-x: -12px; translate-y: 4px; background-image: textureid(outfitmanager_badge_peace); }
.self-center.om-return-c { display: block; opacity: 1; left: 50%; top: 50%; width: 50px; height: 50px; rotate-z: 0deg; translate-x: -12px; translate-y: 4px; background-image: textureid(outfitmanager_badge_return); }
/* The settings view: a setting's name (and its value below it) on the slot's charge-count text, centred in the segment
   (its wrap is an anchor at the slot's centre); an action's name sits above the centre with the game's click prompt
   (om-prompt-c, its key guide icon) below. */
.self-center.om-label-wrap { display: block; opacity: 1; left: 50%; top: 50%; width: 0px; height: 0px; }
.quickslot-item-name.charged-count.om-label { bottom: 0px; height: 24px; line-height: 24px; font-size: 18px; white-space: normal; }
.quickslot-item-name.charged-count.om-label.om-label-value { bottom: -24px; height: 48px; }
/* A setting's value under its name: the label wrap's second text (OmValueText, tools/uibin_tab.py), the game's gold
   caption style; its box as the old value pictures had (30px high, from 2px under the slot's centre). */
.quickslot-item-name.om-value-text { bottom: -32px; height: 30px; line-height: 30px; }
/* A value setting's value under its name: the game's value text (tools/value_text_gen.py textures, 126x30). */
.self-center.om-prompt-c { display: block; opacity: 1; left: 50%; top: 50%; width: 34px; height: 34px; rotate-z: 0deg; translate-x: -17px; translate-y: 4px; background-image: textureid(Common_Icon_Keyguide_PC_Mouse_left); }
/* (Unused, kPlusSlotCentred = true: the "+" slot as an "add" button: no silhouette, its symbol centred, 72px box.) */
.self-center.om-plus-big { display: block; opacity: 1; left: 50%; top: 50%; width: 72px; height: 72px; rotate-z: 0deg; translate-x: -36px; translate-y: -36px; background-image: textureid(Common_Icon_Keyguide_Plus); }
.self-center.om-cog-big { display: block; opacity: 1; left: 50%; top: 50%; width: 72px; height: 72px; rotate-z: 0deg; translate-x: -36px; translate-y: -36px; background-image: textureid(outfitmanager_badge_cog); }
/* An outfit with a piece that is no longer in the inventory (and no other copy of it): greyed out, as vanilla greys
   unavailable items. */
.self-center.om-missing { filter: grayscale(100%); filter-grayscale: 100%; opacity: 0.2; }
.quickslot-item-img.om-missing { filter: grayscale(100%); filter-grayscale: 100%; opacity: 0.45; }
/* The reverse, set whenever it is not greyed: the engine keeps a removed class's filter, so un-greying needs a rule. */
.quickslot-item-img.om-present { filter: grayscale(0%); filter-grayscale: 0%; opacity: 1; }
/* N visible slots (saved outfits + the "+" slot) share the whole wheel: om-list-N + om-sN-i space them 360/N apart at the
   radius of the vanilla wheel they look like (4: comrades, 8: food, 10 / 12: armor; other counts the nearest of those);
   below 12 om-ring-N swaps the ring; om-fN-i is slot i's hover wedge (pa-pickable none: the slots keep their mouse areas).
   Vanilla counts use the vanilla wedge on the slot point; the others a generated 650px wedge centred on the ring centre. */
/* The outfit the character is wearing: om-wN-i puts the vanilla "equipped" wedge in its segment, on the slot's otherwise
   unused disable element (the lowest: below the badges on the reserve element and the focus, which still shows on
   hover). The engine keeps a removed class's transforms, so every rule sets its own (rotate-z, translate, left/top). */
"""


def fmt(v):
    s = f'{v:.4f}'.rstrip('0').rstrip('.')
    return '0' if s == '-0' else s


def write_css():
    out = [CSS_HEAD]
    # The engine's radial layout misplaces slots 1+ at 360/N (only slot 0 lands right), and switching it back on
    # leaves stale positions, so the Outfits list always drops it (12 slots too) and each visible slot root is
    # centred on the family's circle at (i+1)*360/N (the cursor's sectors).
    for n in range(1, SLOTS + 1):
        out.append(f'.quickslot-item-list-12.om-list-{n} {{ layout: none; }}\n')
    for n in range(1, SLOTS + 1):
        R = icon_r(n)
        for i in range(n):
            t = math.radians((i + 1) * 360 / n)
            out.append(f'.quickslot-item-wrap-12.om-s{n}-{i} {{ left: 50%; top: 50%; '
                       f'margin-left: {fmt(R * math.sin(t) - WRAP / 2)}px; '
                       f'margin-top: {fmt(-R * math.cos(t) - WRAP / 2)}px; }}\n')
    for n in range(1, SLOTS):
        nm = name_ring(n) if n in FAMILY else FAM[n]['ring'][0]
        out.append(f'.quickslot-item-list-bg-12.om-ring-{n} {{ background-image: textureid({nm}); }}\n')
    for n in range(1, SLOTS + 1):
        f = fam(n)
        R, IR = f['R'], icon_r(n)
        for i in range(n):
            th = (i + 1) * 360 / n
            t = math.radians(th)
            rot = f'rotate-z: {fmt(0 if i == n - 1 else th)}deg;'
            # generated art: 650px, centred on the ring centre (the wrap sits on the icon circle)
            full = (f'left: {fmt(WRAP / 2 - IR * math.sin(t))}px; top: {fmt(WRAP / 2 + IR * math.cos(t))}px; '
                    f'width: {DISP}px; height: {DISP}px; {rot}')
            if n in FAMILY:
                foc, tf = full, name_focus(n)
            else:               # the vanilla focus on vanilla's slot point (R), R - IR outwards of the wrap centre
                _, _, fw, fh = f['focus']
                pos = f'left: {fmt(WRAP / 2 + (R - IR) * math.sin(t))}px; top: {fmt(WRAP / 2 - (R - IR) * math.cos(t))}px;'
                foc, tf = f'{pos} width: {fw}px; height: {fh}px; {rot}', f['focus'][0]
            out.append(f'.quickslot-item-focus-bg-10.om-f{n}-{i} {{ {foc} pa-pickable: none; background-image: textureid({tf}); }}\n')
            # The worn wedge (generated for every count, with the white line): white, or amber for a worn outfit
            # changed by hand since it was loaded. translate = .self-center's own translate(-50%, -50%) (every rule
            # sets its own: the engine keeps stale ones).
            for cls, tw in ((f'om-w{n}-{i}', name_worn(n)), (f'om-wa{n}-{i}', name_worn_amber(n)), (f'om-wg{n}-{i}', name_worn_green(n))):
                out.append(f'.self-center.{cls} {{ display: block; opacity: 1; {full} pa-pickable: none; '
                           f'translate-x: -{fmt(DISP / 2)}px; translate-y: -{fmt(DISP / 2)}px; background-image: textureid({tw}); }}\n')
    path = os.path.join(MOD, 'ui', 'xml', 'gamemain', 'template', 'quickslotitem.css.merge')
    open(path, 'w', newline='\n').write(''.join(out))


def remove_stale():
    """DDS this generator wrote before for counts that now use vanilla art."""
    d = os.path.join(MOD, *IMG_DIR.split('/'))
    for n in range(1, SLOTS + 1):
        if n in FAMILY: continue
        for nm in (name_ring(n), name_focus(n)): # (the worn wedge is generated for every count)
            p = os.path.join(d, nm + '.dds')
            if os.path.exists(p):
                os.remove(p)
                print('removed', p)


if __name__ == '__main__':
    os.makedirs(GEN, exist_ok=True)
    os.makedirs(os.path.join(MOD, *IMG_DIR.split('/')), exist_ok=True)
    ns = [int(a) for a in sys.argv[1:]] or list(range(1, SLOTS + 1))
    gp = os.path.join(GEN, 'gapless_bg_square.png')
    if not os.path.exists(gp):
        tex.png(gp, S, S, gapless(van('bg_square'), 4)[2])
        print('gapless ring written')
    for n in ns: print('N =', gen(n), 'done', flush=True)   # (a multiprocessing Pool hangs with this Python)
    remove_stale()
    write_atlas()
    write_css()
    print('atlas + css written')
