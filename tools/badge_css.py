"""badge_css.py: Outfit Manager's slot badge rules that follow the slot count, written into the mod's
quickslotitem.css.merge between the BADGES markers (rerun replaces them; 2026-11-13):

- the badge row: the slot's badge spot (the occupancy-chain element: the thermometer's place) shows the first of
  Climate, the bed (Activity: Sleeping) and the Character Look (#35, om-lookmain-c: the game's barber map icon); the
  others go left of it on the side badges OmSideBadge0 / 1 (in the mark wrap, uibin_patch.cpp), placed for the wrap's
  state (om-sb-plain: the bare anchor at the slot's centre; -eye: the eye's 50px box, 12px left / 4px under the centre;
  -eye2: the raised eye's box, 24px higher);
- the hotkey caps / text under the raised eye (om-hk-caps-eye2, om-hotkey-eye2: they sat on the bed's row);
- om-z11 / om-z12: every badge smaller as the wheel fills (11 / 12 slots), shrunk about its own centre so it stays where
  it is and covers less of the outfit's picture; the side badges keep their spacing at the smaller size; the caps too.
  Children of the eye's box are moved back by the box's own shrink, so they stay put.

badge_css.py   (then the mod's merge file has them; the plugin sets the classes)"""
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CSS = os.path.join(ROOT, 'mod', 'Outfit Manager', 'files', '0012', 'ui', 'xml', 'gamemain', 'template',
                   'quickslotitem.css.merge')
BEGIN, END = '/* BADGES (tools/badge_css.py) begin */', '/* BADGES end */'
TIERS = {'': 1.0, 'om-z11': 0.9, 'om-z12': 0.8}
SIDE = 26                       # a side badge's size (the thermometers')
STEP = 28                       # badge row spacing (centre to centre)
SPOT = (13, 29)                 # the badge spot's centre from the slot's centre (thermometer: 26px at translate 0, 16)
EYE_BOX = 50                    # the eye marker's box (om-eye-c / om-eye2-c), its corner from the slot's centre:
BOX = {'plain': (0, 0), 'eye': (-12, 4), 'eye2': (-12, -20)}
CAPS_AT = {'plain': (-90, 42), 'eye': (-78, 38), 'eye2': (-78, 62)}
TEX = {'bed': 'cd_uitexture_tab_icon_housing_Bed', 'look': 'cd_icon_map_beauty'}


def fmt(v):
    v = round(v, 1)
    return str(int(v)) if v == int(v) else f'{v:.1f}'


def sel(base, t):
    return base + (f'.{t}' if t else '')


def base_rules():
    out = ['.self-center.om-lookmain-c { display: block; opacity: 1; left: 50%; top: 50%; width: 26px; height: 26px; '
           'rotate-z: 0deg; translate-x: 0px; translate-y: 16px; background-image: textureid(cd_icon_map_beauty); }']
    for k, tex in TEX.items():
        out.append(f'.om-sb.om-sb-{k} {{ background-image: textureid({tex}); }}')
    # the centre disc's badge row (under the name's divider line, 250 x 1 px): the abyss gear sockets' frames, centred
    out.append('.om-cb-row { position: relative; width: 250px; height: 38px; margin-top: 2px; display: flex; '
               'flex-direction: row; justify-content: center; align-items: center; }')
    out.append('.om-cb { position: relative; width: 34px; height: 34px; margin-left: 3px; margin-right: 3px; '
               'background-image: textureid(cd_slot_bg_small_abyssgear_socket); }')
    out.append('.om-cb-icon { position: absolute; left: 5px; top: 5px; width: 24px; height: 24px; }')
    for k, tex in (('eye', 'outfitmanager_badge_eye'), ('cold', 'cd_icon_temperature_freeze_00'),
                   ('heat', 'cd_icon_temperature_burn_00'), ('cold2', 'cd_icon_temperature_freeze_phase_00'),
                   ('heat2', 'cd_icon_temperature_burn_phase_00'), ('bed', 'cd_uitexture_tab_icon_housing_Bed'),
                   ('look', 'cd_icon_map_beauty')):
        out.append(f'.om-cb-icon.om-cbi-{k} {{ background-image: textureid({tex}); }}')
    # (the eye's texture is a 50px badge canvas with margins: drawn larger to match the others)
    out.append('.om-cb-icon.om-cbi-eye { left: -1px; top: -1px; width: 36px; height: 36px; }')
    out.append(f'.om-hk-caps-eye2 {{ left: {CAPS_AT["eye2"][0]}px; top: {CAPS_AT["eye2"][1]}px; }}')
    out.append('.quickslot-item-name.om-value-text.om-hotkey-eye2 { bottom: -34px; height: 22px; line-height: 22px; '
               'font-size: 15px; translate-x: -150px; }')
    return out


def tier_rules(css):
    out = []
    # every badge rule placed by translate on a self-center element: scaled about its own centre
    for m in re.finditer(r'^\.self-center\.(om-[\w-]+) \{([^}]*)\}', css, re.M):
        cls, body = m.group(1), m.group(2)
        w, h = re.search(r'width: (-?[\d.]+)px', body), re.search(r'height: (-?[\d.]+)px', body)
        tx, ty = re.search(r'translate-x: (-?[\d.]+)px', body), re.search(r'translate-y: (-?[\d.]+)px', body)
        if not (w and h and tx and ty) or not cls.endswith('-c'):
            continue
        w, h, tx, ty = (float(v.group(1)) for v in (w, h, tx, ty))
        for t, s in TIERS.items():
            if not t:
                continue
            nb = re.sub(r'width: -?[\d.]+px', f'width: {fmt(w * s)}px', body)
            nb = re.sub(r'height: -?[\d.]+px', f'height: {fmt(h * s)}px', nb)
            nb = re.sub(r'translate-x: -?[\d.]+px', f'translate-x: {fmt(tx + w * (1 - s) / 2)}px', nb)
            nb = re.sub(r'translate-y: -?[\d.]+px', f'translate-y: {fmt(ty + h * (1 - s) / 2)}px', nb)
            out.append(f'.self-center.{cls}.{t} {{{nb}}}')
    for t, s in TIERS.items():
        shrink = EYE_BOX * (1 - s) / 2  # (how far the eye's box corner moved: its children move back by it)
        size = SIDE * s
        out.append(f'{sel(".om-sb", t)} {{ position: absolute; width: {fmt(size)}px; height: {fmt(size)}px; }}')
        for k in range(2):  # (side badge k: the (k+1)th left of the spot, at the smaller spacing too)
            cx, cy = SPOT[0] - STEP * (k + 1) * s, SPOT[1]
            for st, (bx, by) in BOX.items():
                back = 0 if st == 'plain' else shrink
                out.append(f'{sel(f".om-sb.om-sb-k{k}.om-sb-{st}", t)} {{ left: {fmt(cx - size / 2 - bx - back)}px; '
                           f'top: {fmt(cy - size / 2 - by - back)}px; }}')
        if not t:
            continue
        out.append(f'.om-hk-caps.{t} {{ height: {fmt(22 * s)}px; }}')
        for k, (x, y) in CAPS_AT.items():
            back = 0 if k == 'plain' else shrink
            out.append(f'.om-hk-caps.om-hk-caps-{k}.{t} {{ left: {fmt(x - back)}px; top: {fmt(y - back)}px; }}')
        out.append(f'.om-hk-mod.{t} {{ width: {fmt(36 * s)}px; height: {fmt(22 * s)}px; }}')
        out.append(f'.om-hk-key.{t} {{ width: {fmt(22 * s)}px; height: {fmt(22 * s)}px; }}')
        out.append(f'.om-hk-key.om-hk-wide.{t} {{ width: {fmt(36 * s)}px; }}')
        out.append(f'.om-hk-plus.{t} {{ width: {fmt(12 * s)}px; height: {fmt(22 * s)}px; line-height: {fmt(22 * s)}px; '
                   f'font-size: {fmt(15 * s)}px; }}')
        for k in ('plain', 'eye', 'eye2'):
            out.append(f'.quickslot-item-name.om-value-text.om-hotkey-{k}.{t} {{ font-size: {fmt(15 * s)}px; }}')
    return out


def main():
    raw = open(CSS, 'rb').read()
    bom = raw.startswith(b'\xef\xbb\xbf')
    css = raw.decode('utf-8-sig')
    nl = '\r\n' if '\r\n' in css else '\n'
    if BEGIN in css:
        a, b = css.index(BEGIN), css.index(END) + len(END)
        css = css[:a].rstrip('\r\n') + css[b:]
    base = base_rules()
    lines = [BEGIN] + base + tier_rules(css + '\n' + '\n'.join(base)) + [END]
    css = css.rstrip('\r\n') + nl + nl.join(lines) + nl
    open(CSS, 'wb').write((b'\xef\xbb\xbf' if bom else b'') + css.encode('utf-8'))
    print(len(lines) - 2, 'rules ->', CSS)


if __name__ == '__main__':
    main()
