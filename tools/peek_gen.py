"""The quick peek's art and layout (Outfit Manager): outfits on an arc left of the character, in the stamina gauge's
tracker layer of statusgaugeview2 (elements added in memory by plugin/src/uibin_patch.cpp: OmPeekWrap > OmPeekArc,
OmPeekFrame0-4, OmPeekIcon0-4, OmPeekLabel, OmPeekName).

Writes into <out>/files/0012 (default: the mod's source, mod/Outfit Manager):
  ui/texture/image/customizeimage/outfitmanager_peek_slot.dds, _sel.dds   slot frames (dark disc + ring; gold ring)
  ui/xml/texture/cd_hud_status_02.xml.merge   registers them in the HUD's own texture list (the one with the arc
                                              art, loaded with the HUD; the gear wheel's list may not be)
  ui/xml/gamemain/play/statusgaugeview2.css.merge   the layout

The arc is the game's cd_status_hud_center_gauge (160x319 in cd_hud_status_02.dds, the white curve the hidden HP arc
tints red): measured, its centre line is a circle of radius 234.6 px whose centre lies 266.6 px right of the
texture's left edge, at mid-height (row 155); it spans about +-41 degrees.

peek_gen.py [out dir]"""
import math, os, sys
from PIL import Image, ImageDraw, ImageFilter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from badge_gen import write_dds

OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'mod', 'Outfit Manager')  # (the repo's mod)
FILES = os.path.join(OUT, 'files', '0012')
SPR = r'F:\MyProjects\CDInventory\nexus\sprites'

# geometry, in the peek layer's CSS px (its origin = the HP tracker's anchor on the character, at 0.6 of his height;
# a layer like the stamina tracker's). (A fixed spot
# on the screen was tried, 2026-11-05: the user liked following the character better.)
C = (17.0, 0.0)       # the arc's circle centre: fixed (the user's choice, 2026-11-05, after trying distances that
                      # followed the zoom / the character's measured size, and a fixed spot on the screen)
R = 230.0             # its radius
TEX_R, TEX_CX, TEX_CY, TEX_W, TEX_H = 234.6, 266.6, 155.0, 160, 319
STEP = 17.0           # degrees between slot positions (5 positions: 0, +-17, +-34 within the art's +-41)
ARC_MARGIN = 12.0     # degrees of arc past the outermost shown slot (fewer than 5 outfits)
SIZES = {0: 58, 1: 44, 2: 36}            # frame size by distance from the middle
OPACITY = {0: 1.0, 1: 0.9, 2: 0.7}
ICON = 0.8            # icon size / frame size
IVORY, GOLD = '#ece4d6', '#cb9349'
# The opening animation: the HUD key guide's unfold (the user's pick of the game-native previews, 2026-11-05). The arc
# opens vertically from 0.6 of its height with the keyframes of the game's ani-show-additional-keyguide (hudview2.css),
# slower; the slots ride along (translate-y) and each pops with the inventory's item-icon bump (1 -> 1.15 -> 1), middle
# first; the texts fade in last; then the middle slot's frame pulses once. The game's keyframes are linear between
# stops, so each element's motion is sampled into its own keyframes (om-peek-in-<element>, single classes). Closing:
# the whole peek fades with a slight shrink. Triggered by the plugin's classes om-peek-in-* / om-peek-out-wrap (added
# when the peek opens or closes, removed between, so they play again); it hides the peek once OUT_TOTAL has passed
# (kPeekOutMs >= it).
UNFOLD_SY = [(0, 0.6), (0.3, 0.9), (0.5, 0.95), (0.8, 0.98), (1, 1.0)]
UNFOLD_OP = [(0, 0.0), (0.3, 0.2), (0.5, 0.7), (0.8, 1.0), (1, 1.0)]
OPEN_S, PULSE_S, OUT_TOTAL = 0.42, 0.35, 0.25


def keys(p, stops):
    """Piecewise-linear value through (t, v) stops."""
    for (t0, v0), (t1, v1) in zip(stops, stops[1:]):
        if p <= t1:
            return v0 + (v1 - v0) * (0 if t1 == t0 else (p - t0) / (t1 - t0))
    return stops[-1][1]


def keyframes(name, total, fn, steps):
    """@keyframes name sampled from fn(t seconds) -> {property: value}."""
    out = []
    for i in range(steps + 1):
        t = total * i / steps
        props = ' '.join(f'{k}: {v};' for k, v in fn(t).items())
        out.append(f'{100 * i / steps:.1f}% {{ {props} }}')
    return f'@keyframes {name} {{ ' + ' '.join(out) + ' }'


def slot_motion(k, base_op, pulse):
    """The slot k element's opening: riding the unfold, the pop, then (pulse) the middle frame's pulse."""
    y = slot_pos(k)[1]
    def fn(t):
        if t > OPEN_S:
            u = min(1.0, (t - OPEN_S) / PULSE_S)
            sc = 1 + 0.18 * math.sin(math.pi * u)
            return {'opacity': f'{base_op:.3f}', 'translate-y': '0px', 'scale-x': f'{sc:.3f}', 'scale-y': f'{sc:.3f}'}
        q = t / OPEN_S
        r = min(1.0, max(0.0, (q - 0.15 - 0.06 * abs(k - 2)) / 0.5))
        sc = keys(r, [(0, 1.0), (0.5, 1.15), (1, 1.0)])
        op = keys(r, [(0, 0.0), (0.5, 0.5), (1, 1.0)]) * base_op
        return {'opacity': f'{op:.3f}', 'translate-y': f'{(keys(q, UNFOLD_SY) - 1) * y:.2f}px',
                'scale-x': f'{sc:.3f}', 'scale-y': f'{sc:.3f}'}
    total = OPEN_S + (PULSE_S if pulse else 0)
    return total, fn


# The scroll's slide (2026-11-06, the user's pick of round 14's mockups): on a step every slot's frame, icon and badge
# glides from where its new outfit was (position, size, opacity) to its own place; an outfit coming in at an end of the
# arc fades in from one step beyond it, or from a position not shown (fewer than 5 outfits). The texts rise / drop in.
# Classes (set by the plugin on each step): om-peek-sl<a|b>-<m|e><u|d>-<frame|icon|badge>-<k> and
# om-peek-sl<a|b>-t<u|d>; a / b alternate so a step right after another restarts; m = moved from a shown neighbour,
# e = entered; u = the outfits moved up (the next outfit), d = down.
SLIDE_S, SLIDE_STEPS, SLIDE_TEXT_DY = 0.18, 12, 9.0


def frac_slot(kf):
    """A fractional position along the arc (beyond 0..4: past an end): its centre, frame size, base opacity."""
    a = math.radians((kf - 2) * STEP)
    d = abs(kf - 2)
    if d <= 2:
        i = min(int(d), 1)
        t = d - i
        size = SIZES[i] + (SIZES[i + 1] - SIZES[i]) * t
        op = OPACITY[i] + (OPACITY[i + 1] - OPACITY[i]) * t
    else:
        size, op = SIZES[2] * 0.75, 0.0
    return C[0] - R * math.cos(a), C[1] + R * math.sin(a), size, op


def slide_css():
    out = []
    ease = lambda t: 1 - (1 - t) ** 3  # OutCubic, sampled (the game's keyframes are linear between stops)
    for ab in 'ab':
        for dd, step in (('u', 1), ('d', -1)):
            for k in range(5):
                x, y, f, op = frac_slot(k)
                for me in 'me':
                    if me == 'm' and not 0 <= k + step <= 4:
                        continue
                    sx, sy, sf, sop = frac_slot(k + step)
                    if me == 'e':
                        sop = 0.0
                    for part, ox, oy, scale_op in (('frame', 0, 0, 1.0), ('icon', 0, 0, 1.0 if k == 2 else 0.9),
                                                   ('badge', 0.36, 0.34, 1.0)):
                        own_op = op * scale_op
                        src_op = sop * scale_op
                        dx = (sx + sf * ox) - (x + f * ox)
                        dy = (sy + sf * oy) - (y + f * oy)
                        sc0 = sf / f
                        name = f'om-peek-sl{ab}-{me}{dd}-{part}-{k}'

                        def fn(t, dx=dx, dy=dy, sc0=sc0, src_op=src_op, own_op=own_op):
                            e = ease(t / SLIDE_S)
                            sc = sc0 + (1 - sc0) * e
                            return {'opacity': f'{src_op + (own_op - src_op) * e:.3f}',
                                    'translate-x': f'{dx * (1 - e):.2f}px', 'translate-y': f'{dy * (1 - e):.2f}px',
                                    'scale-x': f'{sc:.3f}', 'scale-y': f'{sc:.3f}'}
                        out.append(keyframes(name, SLIDE_S, fn, SLIDE_STEPS))
                        out.append(f'.{name} {{ animation-name: {name}; animation-duration: {SLIDE_S:.2f}s; '
                                   'animation-timing-function: Linear; animation-fill-mode: forwards; }')
            name = f'om-peek-sl{ab}-t{dd}'
            out.append(keyframes(name, SLIDE_S, lambda t, step=step: {
                'opacity': f'{ease(t / SLIDE_S):.3f}',
                'translate-y': f'{step * SLIDE_TEXT_DY * (1 - ease(t / SLIDE_S)):.2f}px'}, SLIDE_STEPS))
            out.append(f'.{name} {{ animation-name: {name}; animation-duration: {SLIDE_S:.2f}s; '
                       'animation-timing-function: Linear; animation-fill-mode: forwards; }')
    return out


def frame(selected):
    n = 126
    im = Image.new('RGBA', (n, n), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.ellipse((5, 5, n - 6, n - 6), fill=(18, 17, 20, 215))
    ring = Image.open(os.path.join(SPR, 'ring.png')).convert('RGBA').resize((n, n), Image.LANCZOS)
    rgb = (203, 147, 73) if selected else (150, 140, 125)
    tinted = Image.new('RGBA', ring.size, rgb + (255,))
    tinted.putalpha(ring.split()[3].point(lambda v: int(v * (1 if selected else 0.8))))
    if selected:  # a faint gold glow around the ring
        glow = tinted.filter(ImageFilter.GaussianBlur(4))
        im.alpha_composite(glow)
    im.alpha_composite(tinted)
    return im


NONE_PX = 254          # the "No outfit" icon: as the item icons (256px, drawn in the same icon box)
NONE_FILL = 0.8        # its glyph's extent in that box


def no_outfit_icon():
    """The Outfit Arc's "No outfit" entry: glyphs.no_outfit() off-white with a soft dark edge, in a box the size of the
    item icons the other entries show, so the Arc draws it at their scale."""
    import glyphs
    m = glyphs.no_outfit()
    m = m.crop(m.getbbox())
    k = NONE_PX * NONE_FILL / max(m.size)
    m = m.resize((round(m.width * k), round(m.height * k)), Image.LANCZOS)
    mask = Image.new('L', (NONE_PX, NONE_PX), 0)
    mask.paste(m, ((NONE_PX - m.width) // 2, (NONE_PX - m.height) // 2))
    edge = mask.filter(ImageFilter.MaxFilter(7)).filter(ImageFilter.GaussianBlur(2))
    img = Image.new('RGBA', mask.size, (10, 9, 8, 0))
    img.putalpha(edge.point(lambda v: v * 150 // 255))
    tone = [Image.new('L', mask.size, t) for t in (222, 214, 200)]
    img.alpha_composite(Image.merge('RGBA', (*tone, mask)))
    return img


def slot_pos(k):
    """Position k (0..4, 2 = the middle): its centre on the circle, its distance from the middle."""
    p = k - 2
    a = math.radians(p * STEP)
    return C[0] - R * math.cos(a), C[1] + R * math.sin(a), abs(p)


ARC_PX = 2.0          # arc texture px per css px (crisp at high resolutions)
ARC_FULL = 2 * STEP + ARC_MARGIN  # 5+ outfits: the arc reaches this far each way (degrees)
ARC_MIN = 24.0        # the shortest arc reaches at least this far each way (degrees)


def arc_span(n):
    """The arc for n outfits: angles (degrees, negative = up) from the outermost shown slots plus a margin."""
    lo, hi = -min(2, (n - 1) // 2), min(2, n // 2)  # the positions shown (offsets from the middle), as the plugin
    if n >= 5:
        return -ARC_FULL, ARC_FULL
    # (at least ARC_MIN each way: one or two slots still get a visible stretch of arc, not just its fades)
    return min(lo * STEP - ARC_MARGIN, -ARC_MIN), max(hi * STEP + ARC_MARGIN, ARC_MIN)


def arc_box(a0, a1, pad=14.0):
    """The arc element's box (css px): the stretch of the circle from a0 to a1, padded for its glow / shadow."""
    ys = [C[1] + R * math.sin(math.radians(a)) for a in (a0, a1)]
    xs = [C[0] - R * math.cos(math.radians(a)) for a in (a0, a1, 0.0) if a0 <= a <= a1]
    return min(xs) - pad, min(ys) - pad, max(xs) + pad, max(ys) + pad


def arc_image(n):
    """Our own arc line (as in the mockups the user picked over the game's thick arc art): a thin ivory line with a
    soft glow, a faint dark shadow under it, fading out at both ends."""
    a0, a1 = arc_span(n)
    x0, y0, x1, y1 = arc_box(a0, a1)
    w, h = int((x1 - x0) * ARC_PX) + 1, int((y1 - y0) * ARC_PX) + 1
    line = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    shade = Image.new('L', (w, h), 0)
    dl, ds = ImageDraw.Draw(line), ImageDraw.Draw(shade)
    steps = 240
    fade_deg = min(9.0, (a1 - a0) / 5)
    for i in range(steps):
        t0, t1 = a0 + (a1 - a0) * i / steps, a0 + (a1 - a0) * (i + 1) / steps
        fade = max(0.0, min(1.0, (t0 - a0) / fade_deg, (a1 - t1) / fade_deg))
        p0 = ((C[0] - R * math.cos(math.radians(t0)) - x0) * ARC_PX, (C[1] + R * math.sin(math.radians(t0)) - y0) * ARC_PX)
        p1 = ((C[0] - R * math.cos(math.radians(t1)) - x0) * ARC_PX, (C[1] + R * math.sin(math.radians(t1)) - y0) * ARC_PX)
        dl.line((p0, p1), fill=(236, 228, 214, int(200 * fade)), width=int(2.8 * ARC_PX))
        ds.line((p0, p1), fill=int(150 * fade), width=int(10 * ARC_PX))
    out = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    dark = Image.new('RGBA', (w, h), (6, 6, 8, 255))
    dark.putalpha(shade.filter(ImageFilter.GaussianBlur(5 * ARC_PX)))
    out.alpha_composite(dark)
    out.alpha_composite(line.filter(ImageFilter.GaussianBlur(2.5 * ARC_PX)))  # the glow
    out.alpha_composite(line)
    return out


def tex_size(img):
    """The DDS square that holds img at (1, 1)."""
    n = 64
    while n < max(img.size) + 2:
        n *= 2
    return n


# (the texts: single-class rules. Compound ones with the font class, e.g. .om-peek-name.font-text-caption-minor-white,
# were ignored in game, 2026-11-05: the texts kept the default place and width.)
def css():
    out = ['/* Outfit Manager: the quick peek (generated by tools/peek_gen.py). Elements added by OutfitManager.asi to the',
           '   stamina gauge tracker; hidden until it shows them. */',
           '.om-peek-wrap { position: absolute; left: 0px; top: 0px; width: 0px; height: 0px; transform-origin: 0% 0%; }',
           # (opened on a mount the arc waits, laid out but invisible, until the HP tracker has moved to the mount:
           # a frame at the rider's head otherwise, 2026-11-07)
           '.om-peek-veil { opacity: 0; }',
           '.om-peek-arc { position: absolute; }']
    # the arc fits the shown slots (om-peek-arc-n1..n5, set by the plugin): our own line for that stretch, fading at
    # both ends
    for n in range(1, 6):
        x0, y0, x1, y1 = arc_box(*arc_span(n))
        out.append(f'.om-peek-arc-n{n} {{ left: {x0:.1f}px; top: {y0:.1f}px; width: {x1 - x0:.1f}px; height: {y1 - y0:.1f}px; '
                   f'background-image: textureid(outfitmanager_peek_arc_n{n}); '
                   f'transform-origin: 50% {100 * (C[1] - y0) / (y1 - y0):.1f}%; }}')  # (unfolds about his anchor's height)
    for k in range(5):
        x, y, dist = slot_pos(k)
        f = SIZES[dist]
        i = f * ICON
        tex = 'outfitmanager_peek_slot_sel' if dist == 0 else 'outfitmanager_peek_slot'
        out.append(f'.om-peek-frame-{k} {{ position: absolute; left: {x - f / 2:.1f}px; top: {y - f / 2:.1f}px; '
                   f'width: {f}px; height: {f}px; background-image: textureid({tex}); opacity: {OPACITY[dist]}; transform-origin: 50% 50%; }}')
        out.append(f'.om-peek-icon-{k} {{ position: absolute; left: {x - i / 2:.1f}px; top: {y - i / 2:.1f}px; '
                   f'width: {i:.1f}px; height: {i:.1f}px; opacity: {OPACITY[dist] * (1 if dist == 0 else 0.9):.2f}; transform-origin: 50% 50%; }}')
    # the key guide unfold (see UNFOLD_*)
    def anim(cls, name, total):
        out.append(f'.{cls} {{ animation-name: {name}; animation-duration: {total:.2f}s; '
                   'animation-timing-function: Linear; animation-fill-mode: forwards; }')
    out.append(keyframes('om-peek-unfold-arc', OPEN_S, lambda t: {
        'opacity': f'{keys(t / OPEN_S, UNFOLD_OP):.3f}', 'scale-x': '1', 'scale-y': f'{keys(t / OPEN_S, UNFOLD_SY):.3f}'}, 20))
    anim('om-peek-in-arc', 'om-peek-unfold-arc', OPEN_S)
    for k in range(5):
        dist = slot_pos(k)[2]
        for part, base in (('frame', OPACITY[dist]), ('icon', OPACITY[dist] * (1 if dist == 0 else 0.9))):
            total, fn = slot_motion(k, base, pulse=(part == 'frame' and k == 2))
            out.append(keyframes(f'om-peek-unfold-{part}-{k}', total, fn, int(round(total / 0.02))))
            anim(f'om-peek-in-{part}-{k}', f'om-peek-unfold-{part}-{k}', total)
    out.append('@keyframes om-peek-unfold-text { 0% { opacity: 0; } 50% { opacity: 0; } 100% { opacity: 1; } }')
    anim('om-peek-in-label', 'om-peek-unfold-text', OPEN_S)
    anim('om-peek-in-name', 'om-peek-unfold-text', OPEN_S)
    out.append('@keyframes om-peek-fade-out { 0% { opacity: 1; scale-x: 1; scale-y: 1; } '
               '100% { opacity: 0; scale-x: 0.96; scale-y: 0.96; } }')
    out.append(f'.om-peek-out-wrap {{ animation-name: om-peek-fade-out; animation-duration: {OUT_TOTAL}s; '
               'animation-timing-function: InOutSine; animation-fill-mode: forwards; }')
    # the badges (2026-11-06): each other slot's climate thermometer on its disc's lower edge (as on the wheel's
    # slots), riding with its frame's opening; the highlighted outfit's next to its label (the label moves left for
    # them); its hotkey as key caps under its name
    for k in range(5):
        bx, by, dist = slot_pos(k)
        f = SIZES[dist]
        bs = round(f * 0.46)
        out.append(f'.om-peek-badge-{k} {{ position: absolute; left: {bx + f * 0.36 - bs / 2:.1f}px; top: {by + f * 0.34 - bs / 2:.1f}px; '
                   f'width: {bs}px; height: {bs}px; opacity: {OPACITY[dist]}; transform-origin: 50% 50%; }}')
        anim(f'om-peek-in-badge-{k}', f'om-peek-unfold-frame-{k}', slot_motion(k, OPACITY[dist], pulse=(k == 2))[0])
    x, y, _ = slot_pos(2)
    right = x - SIZES[0] / 2 - 12  # the texts end left of the middle slot
    for k in range(2):
        out.append(f'.om-peek-nb-{k} {{ position: absolute; left: {right - 22 - (1 - k) * 24:.1f}px; top: {y - 27:.1f}px; '
                   f'width: 22px; height: 22px; }}')
    out.append('.om-peek-label-s1 { translate-x: -26px; }')
    out.append('.om-peek-label-s2 { translate-x: -50px; }')
    out.append(f'.om-peek-hk {{ position: absolute; left: {right - 200:.1f}px; top: {y + 27:.1f}px; width: 200px; height: 20px; '
               'display: flex; flex-direction: row; justify-content: flex-end; align-items: center; }')
    out.append('.om-hk-mod { position: relative; width: 33px; height: 20px; background-color: #f6f0f0; background-blend-mode: multiply; }')
    out.append('.om-hk-key { position: relative; width: 20px; height: 20px; background-color: #f6f0f0; background-blend-mode: multiply; }')
    out.append('.om-hk-wide { width: 33px; }')
    out.append('.om-hk-plus { position: relative; width: 12px; height: 20px; line-height: 20px; text-align: center; font-size: 14px; }')
    out.append(f'.om-peek-label {{ position: absolute; left: {right - 320:.1f}px; '
               f'top: {y - 26:.1f}px; width: 320px; height: 20px; text-align: right; font-size: 14px; color: {GOLD}; }}')
    out.append(f'.om-peek-name {{ position: absolute; left: {right - 320:.1f}px; '
               f'top: {y - 6:.1f}px; width: 320px; height: 30px; text-align: right; font-size: 22px; color: {IVORY}; }}')
    out += slide_css()  # (last: a slide's class wins over the opening's)
    return '\n'.join(out) + '\n'


ARCS = {n: arc_image(n) for n in range(1, 6)}


def atlas_merge():
    rows = [f'<Texture Name="{n}" Filename="UI/texture/image/customizeimage/{n}.dds" Type="Image" GetRect="1,1,126,126"/>'
            for n in ('outfitmanager_peek_slot', 'outfitmanager_peek_slot_sel')]
    rows.append(f'<Texture Name="outfitmanager_peek_none" Filename="UI/texture/image/customizeimage/outfitmanager_peek_none.dds" '
                f'Type="Image" GetRect="1,1,{NONE_PX},{NONE_PX}"/>')
    rows += [f'<Texture Name="outfitmanager_peek_arc_n{n}" Filename="UI/texture/image/customizeimage/outfitmanager_peek_arc_n{n}.dds" '
             f'Type="Image" GetRect="1,1,{im.width},{im.height}"/>' for n, im in ARCS.items()]
    return '<__dmm_root__>\n' + '\n'.join(rows) + '\n</__dmm_root__>\n'


if __name__ == '__main__':
    img = os.path.join(FILES, 'ui', 'texture', 'image', 'customizeimage')
    os.makedirs(img, exist_ok=True)
    write_dds(os.path.join(img, 'outfitmanager_peek_slot.dds'), frame(False), 128)
    write_dds(os.path.join(img, 'outfitmanager_peek_slot_sel.dds'), frame(True), 128)
    write_dds(os.path.join(img, 'outfitmanager_peek_none.dds'), no_outfit_icon(), 256)
    for n, im in ARCS.items():
        write_dds(os.path.join(img, f'outfitmanager_peek_arc_n{n}.dds'), im, tex_size(im))
    for rel, text in ((('ui', 'xml', 'texture', 'cd_hud_status_02.xml.merge'), atlas_merge()),
                      (('ui', 'xml', 'gamemain', 'play', 'statusgaugeview2.css.merge'), css())):
        p = os.path.join(FILES, *rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        open(p, 'w', encoding='utf-8', newline='\n').write(text)
        print('wrote', p)
    print(css())
