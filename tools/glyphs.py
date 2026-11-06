"""Our own badge glyphs (no game icon for them), drawn white on a 400px canvas as 'L' masks; tools/badge_gen.py turns
them into badges like the cog / flag. Designs approved by the user (re/omring/appearance/: look_badges.png)."""
from PIL import Image, ImageDraw

S = 400


def open_eye():
    """The game's Hidden eye (cd_icon_ability_hide) without its slash: almond outline, iris with a pupil hole, a glint.
    Appearance's Shown look, the eye scroll mode, and the outfit marker 'has visibility changes'."""
    m = Image.new('L', (S, S), 0)
    d = ImageDraw.Draw(m)
    cx, cy, w, h = S / 2, S / 2, 330, 150

    def arc(up):
        pts = []
        for i in range(61):
            t = -1 + 2 * i / 60
            y = (h / 2) * (1 - t * t)
            pts.append((cx + t * w / 2, cy - y if up else cy + y))
        return pts

    d.line(arc(True), fill=255, width=22, joint='curve')
    d.line(arc(False), fill=255, width=22, joint='curve')
    d.ellipse((cx - 62, cy - 62, cx + 62, cy + 62), fill=255)
    d.ellipse((cx - 30, cy - 30, cx + 30, cy + 30), fill=0)
    d.ellipse((cx + 6, cy - 30, cx + 30, cy - 6), fill=255)
    return m


def sheathed():
    """A sword in its scabbard with two metal bands, tilted 45 degrees: Appearance's Out of combat only look."""
    m = Image.new('L', (S, S), 0)
    d = ImageDraw.Draw(m)
    cx = S / 2
    d.ellipse((cx - 16, 22, cx + 16, 54), fill=255)                         # pommel
    d.rectangle((cx - 10, 50, cx + 10, 118), fill=255)                      # grip
    for y in range(58, 116, 14):                                            # grip wrap
        d.line((cx - 10, y, cx + 10, y + 6), fill=0, width=4)
    d.rounded_rectangle((cx - 62, 116, cx + 62, 140), radius=10, fill=255)  # guard
    d.rounded_rectangle((cx - 24, 146, cx + 24, 360), radius=14, fill=255)  # scabbard
    d.polygon([(cx - 26, 340), (cx + 26, 340), (cx, 388)], fill=255)        # its tip
    for y in (190, 300):                                                    # two metal bands
        d.rectangle((cx - 30, y, cx + 30, y + 14), fill=255)
        d.rectangle((cx - 24, y - 4, cx + 24, y - 1), fill=0)
        d.rectangle((cx - 24, y + 15, cx + 24, y + 18), fill=0)
    return m.rotate(-45, resample=Image.BICUBIC, center=(S / 2, S / 2))


def as_src(m):
    """A mask as the light-on-transparent RGBA badge_gen.badge() expects."""
    return Image.merge('RGBA', (m, m, m, m))


def return_arrow():
    """A U-turn arrow (over the top, pointing back down on the left): the Appearance view's way back to the outfits."""
    m = Image.new('L', (S, S), 0)
    d = ImageDraw.Draw(m)
    cx, cy, r, w = S / 2 + 20, S / 2 + 10, 120, 34
    d.arc((cx - r, cy - r, cx + r, cy + r), start=180, end=420, fill=255, width=w)
    ax, ay = cx - r + w / 2, cy + 6
    d.polygon([(ax - 70, ay - 10), (ax + 70, ay - 10), (ax, ay + 80)], fill=255)
    return m
