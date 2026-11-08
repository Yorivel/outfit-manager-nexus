"""Weapon set badge in the climate thermometer's style (#27, 2026-11-08): solid, chunky shapes with a thin dark outline,
two pieces - a tall one (the sword, as the thermometer) beside a wider one (axe head / shield / second sword). Plain =
the thermometer's light steel, gold = its weapons worn. 46x46 like the game's temperature icons, drawn at 8x.
Preview: in the thermometer's spot (centred under the picture) on the user's wheel, with the real cold thermometer.
weaponset_thermo_style.py <screenshot>  -> re/omring/badge/weaponset_thermo_style.png (+ the 46px icons)"""
import os, sys
from PIL import Image, ImageDraw, ImageFilter, ImageFont
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import badge_gen as bg

SHOT = sys.argv[1] if __name__ == '__main__' else None
OUT = bg.SRC
K = 8                      # drawn at 8x, scaled down to 46
N = 46 * K
STEEL = ((222, 226, 230), (150, 158, 166))   # light, shade (the thermometer's body greys)
GOLD = ((240, 190, 110), (176, 122, 52))
BROWN = (120, 78, 46)


def sword(d, x, top, bot, light, shade, w=7):
    """A tall sword, point up, centred on x (8x units)."""
    w *= K // 2
    d.polygon([(x, top), (x + w, top + 2.2 * w), (x + w, bot - 9 * K), (x - w, bot - 9 * K), (x - w, top + 2.2 * w)], fill=light)
    d.polygon([(x + 1, top + 2.2 * w), (x + w, top + 2.2 * w), (x + w, bot - 9 * K), (x + 1, bot - 9 * K)], fill=shade)
    d.rounded_rectangle((x - 3.2 * w, bot - 9 * K, x + 3.2 * w, bot - 6 * K), radius=K, fill=light)   # guard
    d.rectangle((x - 0.8 * w, bot - 6 * K, x + 0.8 * w, bot - 1.5 * K), fill=BROWN)                 # grip
    d.ellipse((x - 1.4 * w, bot - 2.6 * K, x + 1.4 * w, bot + 0.4 * K), fill=light)                  # pommel


def axe(d, x, top, bot, light, shade):
    d.rectangle((x - 1.4 * K, top + 2 * K, x + 1.4 * K, bot), fill=BROWN)                            # haft
    d.pieslice((x - 1 * K, top, x + 22 * K, top + 22 * K), 90, 270, fill=light)                       # (unused half)
    d.polygon([(x + 1 * K, top + 3 * K), (x + 14 * K, top - 1 * K), (x + 16 * K, top + 9 * K), (x + 14 * K, top + 19 * K),
               (x + 1 * K, top + 15 * K)], fill=light)
    d.polygon([(x + 9 * K, top + 1 * K), (x + 14 * K, top - 1 * K), (x + 16 * K, top + 9 * K), (x + 14 * K, top + 19 * K),
               (x + 9 * K, top + 17 * K)], fill=shade)


def shield(d, cx, cy, r, light, shade):
    d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=shade)
    d.ellipse((cx - r + 2.5 * K, cy - r + 2.5 * K, cx + r - 2.5 * K, cy + r - 2.5 * K), fill=light)
    d.ellipse((cx - 3.2 * K, cy - 3.2 * K, cx + 3.2 * K, cy + 3.2 * K), fill=shade)


def finish(img):
    """The temperature icons' thin dark outline, then 46 px."""
    a = img.getchannel('A')
    ring = a.filter(ImageFilter.MaxFilter(2 * K + 1))
    out = Image.new('RGBA', img.size, (14, 12, 10, 0))
    out.putalpha(ring.point(lambda v: v * 230 // 255))
    out.alpha_composite(img)
    return out.resize((46, 46), Image.LANCZOS)


def variant(kind, pal):
    light, shade = pal
    img = Image.new('RGBA', (N, N), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    if kind == 'sword + axe':
        sword(d, 13 * K, 3 * K, 44 * K, light, shade)
        axe(d, 27 * K, 8 * K, 43 * K, light, shade)
    elif kind == 'sword + shield':
        shield(d, 30 * K, 27 * K, 13 * K, light, shade)
        sword(d, 13 * K, 3 * K, 44 * K, light, shade)
    else:  # two swords side by side (not crossed: crossed is the combat look)
        sword(d, 15 * K, 3 * K, 44 * K, light, shade)
        sword(d, 31 * K, 9 * K, 44 * K, light, shade, w=6)
    return finish(img)


def main():
    shot = Image.open(SHOT).convert('RGBA')
    clean = shot.copy()
    old = (350, 210, 378, 250)
    clean.paste(shot.crop((old[0] + 70, old[1] - 4, old[2] + 70, old[3] - 4)), old[:2])
    atlas = Image.open(os.path.join(bg.SRC, 'cd_icon_common_00.png')).convert('RGBA')
    cold = atlas.crop((971, 263, 1017, 309))
    view = (250, 95, 470, 300)
    spot = (330, 262)          # the thermometer's spot: centred under the picture (css translate-y 16)
    size = 22                  # 26 css px, as shown in this screenshot
    try:
        font = ImageFont.truetype('arial.ttf', 13)
    except OSError:
        font = ImageFont.load_default()
    cells = [('cold thermometer (reference)', cold, cold)]
    for kind in ('sword + axe', 'sword + shield', 'two swords'):
        p, g = variant(kind, STEEL), variant(kind, GOLD)
        p.save(os.path.join(OUT, f'wset_thermo_{kind.replace(" ", "").replace("+", "_")}.png'))
        cells.append((kind, p, g))
    cw, ch = view[2] - view[0], view[3] - view[1]
    sheet = Image.new('RGBA', (len(cells) * cw, 2 * (ch + 22)), (30, 28, 26, 255))
    d = ImageDraw.Draw(sheet)
    for k, (label, p, g) in enumerate(cells):
        for row, img in enumerate((p, g)):
            t = clean.copy()
            b = img.resize((size, size), Image.LANCZOS)
            t.alpha_composite(b, (spot[0] - size // 2, spot[1] - size // 2))
            sheet.alpha_composite(t.crop(view), (k * cw, row * (ch + 22)))
            if k == 0 and row == 1:
                continue
            d.text((k * cw + 6, row * (ch + 22) + ch + 4), f'{label}{"" if k == 0 else " (gold)" if row else " (plain)"}',
                   font=font, fill=(225, 215, 195, 255))
    # and the icons alone, big
    big = Image.new('RGBA', (len(cells) * 2 * 140, 150), (40, 38, 36, 255))
    for k, (label, p, g) in enumerate(cells):
        big.alpha_composite(p.resize((130, 130), Image.LANCZOS), (k * 280 + 5, 10))
        big.alpha_composite(g.resize((130, 130), Image.LANCZOS), (k * 280 + 140, 10))
    sheet = sheet.resize((sheet.width * 2, sheet.height * 2), Image.LANCZOS)
    full = Image.new('RGBA', (max(sheet.width, big.width), sheet.height + big.height), (30, 28, 26, 255))
    full.alpha_composite(sheet, (0, 0))
    full.alpha_composite(big, (0, sheet.height))
    path = os.path.join(OUT, 'weaponset_thermo_style.png')
    full.save(path)
    print(path)


if __name__ == '__main__':
    main()
