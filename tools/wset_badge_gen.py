"""The weapon set badges (#27): one per kind of set, by the piece its picture shows (wheel_draw.cpp, wset_kind) -
melee (the game's crossed swords, cd_icon_common_faction_sword), ranged (cd_icon_ability_offense_bow) and tool
(cd_icon_ability_Passive_Axe) - coloured in the climate thermometer's style (weaponset_thermo_style.from_icon), plain and
gold (its weapons worn). Chosen with the user 2026-11-17 (until then one badge: a drawn sword + shield).
Writes outfitmanager_wset_<kind>[_gold].dds (46x46 at 1,1 like the game's temperature icons) into the mod and registers
them in cd_hud_itemquickslot_08.xml.merge. The css classes (om-wset-<kind>-c, -gold-c, -busy-c) are omring_gen.py's.
wset_badge_gen.py [mod files/0012 dir]   (needs the extracted atlases in badge_gen.SRC: tools/tex.py 0012 cd_icon_common_00.dds
and cd_icon_common_01.dds - re-extract after a game update, the atlas layout changes)"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import badge_gen as bg
import weaponset_thermo_style as ws

FILES = sys.argv[1] if len(sys.argv) > 1 else os.path.join(bg.MOD)
KINDS = {  # kind: (atlas png, rect) - rects from the game's texture xml (cd_icon_common_00 / _01, game 2.05)
    'melee': ('cd_icon_common_00.png', (693, 331, 60, 60)),   # cd_icon_common_faction_sword
    'ranged': ('cd_icon_common_00.png', (814, 685, 52, 52)),  # cd_icon_ability_offense_bow
    'tool': ('cd_icon_common_01.png', (688, 927, 52, 52)),    # cd_icon_ability_Passive_Axe
}
STATES = {'': ws.STEEL, '_gold': ws.GOLD}


def names():
    return [f'outfitmanager_wset_{k}{s}' for k in KINDS for s in STATES]


def main():
    img_dir = os.path.join(FILES, 'ui', 'texture', 'image', 'customizeimage')
    for kind, (png, rect) in KINDS.items():
        src = bg.crop(png, rect)
        for suffix, pal in STATES.items():
            nm = f'outfitmanager_wset_{kind}{suffix}'
            icon = ws.from_icon(src, pal)
            icon.save(os.path.join(bg.SRC, nm + '.png'))
            bg.write_dds(os.path.join(img_dir, nm + '.dds'), icon, 64)
    path = os.path.join(FILES, 'ui', 'xml', 'texture', 'cd_hud_itemquickslot_08.xml.merge')
    raw = open(path, 'rb').read()
    bom, nl = raw.startswith(b'\xef\xbb\xbf'), '\r\n' if b'\r\n' in raw else '\n'
    lines = [l for l in raw.decode('utf-8-sig').replace('\r\n', '\n').split('\n') if 'outfitmanager_wset' not in l]
    end = max(i for i, l in enumerate(lines) if '</__dmm_root__>' in l)
    for nm in names():
        lines.insert(end, f'<Texture Name="{nm}" Filename="UI/texture/image/customizeimage/{nm}.dds" Type="Image" '
                          f'GetRect="1,1,46,46"/>')
        end += 1
    open(path, 'wb').write((('﻿' if bom else '') + nl.join(lines)).encode('utf-8'))
    print('written:', ', '.join(names()))


if __name__ == '__main__':
    main()
