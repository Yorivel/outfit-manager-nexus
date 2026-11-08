"""The weapon set badge (#27): sword and shield in the climate thermometer's style (chosen 2026-11-08 from
weaponset_thermo_style.py), plain and gold (its weapons worn). Writes outfitmanager_wset.dds / outfitmanager_wset_gold.dds
(46x46 at 1,1 like the game's temperature icons) into the mod and registers them in cd_hud_itemquickslot_08.xml.merge.
The css classes (om-wset-c, om-wset-gold-c, om-wset-busy-c) are omring_gen.py's.
wset_badge_gen.py [mod files/0012 dir]"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import badge_gen as bg
import weaponset_thermo_style as ws

FILES = sys.argv[1] if len(sys.argv) > 1 else os.path.join(bg.MOD)
NAMES = {'outfitmanager_wset': ws.STEEL, 'outfitmanager_wset_gold': ws.GOLD}


def main():
    img_dir = os.path.join(FILES, 'ui', 'texture', 'image', 'customizeimage')
    for nm, pal in NAMES.items():
        icon = ws.variant('sword + shield', pal)
        icon.save(os.path.join(bg.SRC, nm + '.png'))
        bg.write_dds(os.path.join(img_dir, nm + '.dds'), icon, 64)
    path = os.path.join(FILES, 'ui', 'xml', 'texture', 'cd_hud_itemquickslot_08.xml.merge')
    raw = open(path, 'rb').read()
    bom, nl = raw.startswith(b'\xef\xbb\xbf'), '\r\n' if b'\r\n' in raw else '\n'
    lines = [l for l in raw.decode('utf-8-sig').replace('\r\n', '\n').split('\n') if 'outfitmanager_wset' not in l]
    end = max(i for i, l in enumerate(lines) if '</__dmm_root__>' in l)
    for nm in NAMES:
        lines.insert(end, f'<Texture Name="{nm}" Filename="UI/texture/image/customizeimage/{nm}.dds" Type="Image" '
                          f'GetRect="1,1,46,46"/>')
        end += 1
    open(path, 'wb').write((('﻿' if bom else '') + nl.join(lines)).encode('utf-8'))
    print('written:', ', '.join(NAMES))


if __name__ == '__main__':
    main()
