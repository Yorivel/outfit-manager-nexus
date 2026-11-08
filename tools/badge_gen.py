"""Slot mode badges for the Outfits wheel, made like the game's "+" (Common_Icon_Keyguide_Plus) so all three look alike.

The "+" texture (80px canvas, shown in a 50px box): a light grey worn fill (~213, specks down to ~189) with a 1px soft
edge, and a 3px dark outline (grey 24 at 60%, its outer pixel ~50%). This draws the cog (cd_icon_common_KuKu_Cog_00)
and the flag (cd_icon_common_faction_leader_flag) the same way, sized like the "+" (glyph ~34-36 of the 80px), writes them as DXT5
DDS into the browser mod and registers them in its texture list (cd_hud_itemquickslot_08.xml.merge).

badge_gen.py   (needs the extracted atlases: tools/tex.py 0012 cd_icon_common_00.dds <png>, and the "+" atlas)
"""
import os, random, struct, sys
from PIL import Image, ImageChops, ImageFilter
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from omring_gen import enc_block, MOD, IMG_DIR

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 're', 'omring', 'badge')          # extracted atlases + previews
CANVAS = 80                                                 # as the "+" texture
TEX = 128                                                   # DDS size (image at 1,1)
FILL, OUTLINE, OUTLINE_A = 213, 24, 153
import glyphs
BADGES = {  # name: (atlas png, rect, glyph extent in the 80px canvas) or (None, glyph function, extent)
    'outfitmanager_badge_cog': ('cd_icon_common_00.png', (1, 633, 100, 100), 35),   # (the "+": 34; smaller
    'outfitmanager_badge_flag': ('cd_icon_common_00.png', (693, 269, 60, 60), 36),  #  loses the cog's holes)
    # Appearance: the looks (Shown = our open eye, also the eye scroll mode and the outfit marker), as approved
    'outfitmanager_badge_eye': (None, glyphs.open_eye, 36),
    'outfitmanager_badge_hidden': ('cd_icon_common_01.png', (533, 845, 52, 52), 36),  # cd_icon_ability_hide
    'outfitmanager_badge_combat': ('cd_icon_common_00.png', (693, 331, 60, 60), 36),  # cd_icon_common_faction_sword
    'outfitmanager_badge_peace': (None, glyphs.sheathed, 36),
    'outfitmanager_badge_return': (None, glyphs.return_arrow, 36),   # the view's Back to Outfits step
    'outfitmanager_badge_unequip': (None, glyphs.slashed_plate_armour, 36),  # the "+" slot's Unequip Outfit
    # (weapon sets, #27: their badge is wset_badge_gen.py's, in the thermometer's style)
}
PLUS = ('cd_icon_keyguide_00.png', (83, 411, 80, 80))


def source(png, rect):
    return glyphs.as_src(rect()) if png is None else crop(png, rect)


def crop(png, rect):
    x, y, w, h = rect
    return Image.open(os.path.join(SRC, png)).convert('RGBA').crop((x, y, x + w, y + h))


def grain(w, h, seed):
    """The "+"'s worn fill: mostly ~213-221, soft darker specks down to ~189."""
    rnd = random.Random(seed)
    base = Image.new('L', (w, h), FILL)
    px = base.load()
    for y in range(h):
        for x in range(w):
            px[x, y] = FILL + rnd.choice((0, 0, 0, 4, 8, -4))
    for _ in range(w * h // 60):  # specks
        cx, cy = rnd.randrange(w), rnd.randrange(h)
        for dx, dy in ((0, 0), (1, 0), (0, 1)):
            if 0 <= cx + dx < w and 0 <= cy + dy < h: px[cx + dx, cy + dy] = rnd.randint(186, 200)
    return base.filter(ImageFilter.GaussianBlur(0.6))


def badge(src, extent, seed):
    # the glyph is the light part: the cog's holes are painted dark, not transparent
    lum = src.convert('L').point(lambda v: max(0, min(255, (v - 60) * 255 // 120)))
    a = ImageChops.multiply(src.getchannel('A'), lum)
    a = a.crop(a.getbbox())
    k = extent / max(a.size)
    a = a.resize((max(1, round(a.width * k)), max(1, round(a.height * k))), Image.LANCZOS)
    mask = Image.new('L', (CANVAS, CANVAS), 0)
    mask.paste(a, ((CANVAS - a.width) // 2, (CANVAS - a.height) // 2))
    solid = mask.point(lambda v: 255 if v > 96 else 0)
    ring = solid.filter(ImageFilter.MaxFilter(7))                     # 3px around the glyph
    outer = solid.filter(ImageFilter.MaxFilter(9))                    # its soft last pixel
    out_a = Image.eval(ring, lambda v: OUTLINE_A if v else 0)
    out_a = Image.composite(out_a, Image.eval(outer, lambda v: 126 if v else 0), ring)
    img = Image.new('RGBA', (CANVAS, CANVAS), (OUTLINE, OUTLINE, OUTLINE, 0))
    img.putalpha(out_a)
    fill = grain(CANVAS, CANVAS, seed)
    glyph = Image.merge('RGBA', (fill, fill, fill, mask.point(lambda v: min(255, int(v * 1.08)))))
    img.alpha_composite(glyph)
    return img


GOLD = (203, 147, 73)  # the game's Equipped / selected gold (the Outfit Arc's selected ring)


def gold(img):
    """A badge lit: its light glyph tinted gold (brightness kept), the dark outline as it was."""
    out = img.copy()
    px = out.load()
    for y in range(out.height):
        for x in range(out.width):
            r, g, b, a = px[x, y]
            if r > 120:  # (the glyph; the outline is dark)
                k = r / 255 * 1.15
                px[x, y] = (min(255, int(GOLD[0] * k)), min(255, int(GOLD[1] * k)), min(255, int(GOLD[2] * k)), a)
    return out


def ink(img):
    a = img.getchannel('A')
    l = img.convert('L')
    return sum(1 for al, lv in zip(a.getdata(), l.getdata()) if al > 200 and lv > 150)


def write_dds(path, img, tex=None):
    tex = tex or TEX
    canvas = bytearray(tex * tex * 4)
    raw = img.tobytes()
    for y in range(img.height):
        o = ((y + 1) * tex + 1) * 4
        canvas[o:o + img.width * 4] = raw[y * img.width * 4:(y + 1) * img.width * 4]
    body = bytearray()
    for by in range(0, tex, 4):
        for bx in range(0, tex, 4):
            body += enc_block(canvas, tex, bx, by)
    hdr = struct.pack('<4sIIIIIII44sII4s20sI16s', b'DDS ', 124, 0x000A1007, tex, tex, len(body), 0, 1, bytes(44),
                      32, 4, b'DXT5', bytes(20), 0x1000, bytes(16))
    with open(path, 'wb') as f: f.write(hdr + body)


def register(names):
    path = os.path.join(MOD, 'ui', 'xml', 'texture', 'cd_hud_itemquickslot_08.xml.merge')
    raw = open(path, 'rb').read()
    bom, nl = raw.startswith(b'\xef\xbb\xbf'), '\r\n' if b'\r\n' in raw else '\n'  # (written back as it was)
    lines = [l for l in raw.decode('utf-8-sig').replace('\r\n', '\n').split('\n') if 'outfitmanager_badge_' not in l]
    end = max(i for i, l in enumerate(lines) if '</__dmm_root__>' in l)
    for nm in names:
        lines.insert(end, f'<Texture Name="{nm}" Filename="UI/texture/image/customizeimage/{nm}.dds" Type="Image" '
                          f'GetRect="1,1,{CANVAS},{CANVAS}"/>')
        end += 1
    open(path, 'wb').write((('﻿' if bom else '') + nl.join(lines)).encode('utf-8'))


if __name__ == '__main__':
    plus = crop(*PLUS)
    print('ink: "+"', ink(plus))
    d = os.path.join(MOD, *IMG_DIR.split('/'))
    for seed, (nm, (png, rect, extent)) in enumerate(BADGES.items()):
        img = badge(source(png, rect), extent, seed + 1)
        if nm.endswith('_gold'):  # (the same badge, lit: the same grain as its plain one)
            img = gold(badge(source(png, rect), extent, seed))
        img.save(os.path.join(SRC, nm + '.png'))
        write_dds(os.path.join(d, nm + '.dds'), img)
        print(nm, 'ink', ink(img), 'extent', extent)
    register(list(BADGES))
    # preview: the "+" and the badges side by side on the ring's grey, 4x
    sheet = Image.new('RGBA', ((len(BADGES) + 1) * (CANVAS * 4 + 10) + 10, CANVAS * 4 + 20), (45, 44, 42, 255))
    for i, im in enumerate([plus] + [Image.open(os.path.join(SRC, nm + '.png')) for nm in BADGES]):
        sheet.alpha_composite(im.resize((CANVAS * 4, CANVAS * 4), Image.NEAREST), (10 + i * (CANVAS * 4 + 10), 10))
    sheet.convert('RGB').save(os.path.join(SRC, 'badges_preview.png'))
    print('written; preview', os.path.join(SRC, 'badges_preview.png'))
