"""Outfit Manager: the controller confirm icons (Cross, A) as the mod's own textures. The game doesn't draw its
Common_Icon_Keyguide_PS_A / XB_A on the mod's slot badge (the platform icons are swapped in at run time), so their
pixels are copied from the game's atlas (cd_icon_keyguide_00.dds, group 0012) into outfitmanager_key_cross.dds /
outfitmanager_key_a.dds: 128x128 DXT5 with the 80x80 icon at 1,1, like the other badges (texture list entries in
cd_hud_itemquickslot_08.xml.merge).
om_key_icons.py <atlas png> <out dir>"""
import os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from PIL import Image
from omring_gen import enc_block

ICONS = {'outfitmanager_key_cross': (83, 657), 'outfitmanager_key_a': (493, 247)}  # GetRect x, y (80x80)
TEX = 128


def write_dds(path, im):
    canvas = Image.new('RGBA', (TEX, TEX), (0, 0, 0, 0))
    canvas.paste(im, (1, 1))
    buf = canvas.tobytes()
    body = bytearray()
    for by in range(0, TEX, 4):
        for bx in range(0, TEX, 4):
            body += enc_block(buf, TEX, bx, by)
    hdr = struct.pack('<4sIIIIIII44sII4s20sI16s', b'DDS ', 124, 0x000A1007, TEX, TEX, len(body), 0, 1, bytes(44),
                      32, 4, b'DXT5', bytes(20), 0x1000, bytes(16))
    with open(path, 'wb') as f:
        f.write(hdr + body)


if __name__ == '__main__':
    atlas = Image.open(sys.argv[1]).convert('RGBA')
    for name, (x, y) in ICONS.items():
        write_dds(os.path.join(sys.argv[2], name + '.dds'), atlas.crop((x, y, x + 80, y + 80)))
        print(name)
