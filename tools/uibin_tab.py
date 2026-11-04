"""REFERENCE ONLY since 0.3.3: the plugin does this in memory (plugin/src/uibin_patch.cpp, the same steps; tested byte
for byte against this script's output on the 20261104 files), onto whatever copy of the files the game loads, and the
mod ships none of them. Kept to compare with and to inspect files.

uibin_tab.py: adds Outfit Manager's tab header to the game's compiled UI (ui/bin__/*.uibinary). Since the 10-30 game
update the screens are built from these files, not from the .thtml, so the mod's quickslotitem.thtml merge alone adds no
tab (OM logs "headers=4 ... no Outfits tab injected"). A copy of QuickSlotHeader8's element goes in right after it, as
QuickSlotHeaderOutfits, and the parent's child count goes up by one; "!cpp-none" keeps it hidden until OM shows it.

Element format (no absolute offsets seen):
  classes   u8 flag; if 1: u32 n, n strings
  children  u8 flag; if 1: u32 n, n elements
  attrs     u8 flag; if 1: u32 n, n (key, value) strings
  5 bytes
  id, text / X, component, template, script: strings (u32 length + bytes)
  u32, 3 tail bytes (kind, 0, 6)

Also a second text in each slot's label wrap (CoolTimeChargedCountWrap, whose CoolTimeChargedCount OM uses for a
setting's name): a copy of that text as OmValueText, classes om-value-text and font-text-caption-yellow, for the
setting's value in gold (one text holds one colour). The wrap's child count goes up by one. The slot components the
wheel builds from are in itemquickslotview2_component_game.uibinary, so that file gets it too.

uibin_tab.py [--write]: reports; with --write, writes the patched files into the Outfit Manager DMM mod (rebuild after
every game patch: they're whole files)."""
import os, struct, sys

sys.path.insert(0, 'F:/MyProjects/CDOverlayLoader')
write = '--write' in sys.argv
sys.argv = sys.argv[:1]
import pack

HEADER, NEW = b'QuickSlotHeader8', b'QuickSlotHeaderOutfits'
CHILDREN = 7
PARENT = struct.pack('<I', 28) + b'quickslot-tabbar-header-wrap' + bytes([1]) + struct.pack('<I', CHILDREN)
FILES = ['ui/bin__/quickslotitem_game.uibinary', 'ui/bin__/quickslotitem_component_game.uibinary',
         'ui/bin__/itemquickslotview2_game.uibinary']
MOD = 'F:/MyProjects/CDInventory/mod/Outfit Manager/files/0012'   # the mod's source (DMM's copy is an install)


class Bad(Exception):
    pass


def u32(b, i):
    if i + 4 > len(b):
        raise Bad(f'end at {i}')
    return struct.unpack_from('<I', b, i)[0]


def string(b, i):
    n = u32(b, i)
    if n > 4096 or i + 4 + n > len(b):
        raise Bad(f'string length {n} at {i}')
    return b[i + 4:i + 4 + n], i + 4 + n


def flag(b, i):
    if i >= len(b) or b[i] > 1:
        raise Bad(f'flag {b[i] if i < len(b) else None} at {i}')
    return b[i], i + 1


def element(b, i, out=None, parent=None, depth=0):
    """End offset of the element at i; `out` collects (id, start, end, parent start, count offset)."""
    if depth > 64:
        raise Bad('too deep')
    start = i
    f, i = flag(b, i)
    if f:
        n = u32(b, i)
        i += 4
        if n > 256:
            raise Bad(f'{n} classes at {i}')
        for _ in range(n):
            _, i = string(b, i)
    f, i = flag(b, i)
    count_at = i if f else None
    if f:
        n = u32(b, i)
        i += 4
        if n > 4096:
            raise Bad(f'{n} children at {i}')
        for _ in range(n):
            i = element(b, i, out, (start, count_at), depth + 1)
    f, i = flag(b, i)
    if f:
        n = u32(b, i)
        i += 4
        if n > 512:
            raise Bad(f'{n} attrs at {i}')
        for _ in range(2 * n):
            _, i = string(b, i)
    if any(b[i:i + 5]):
        raise Bad(f'5 bytes not zero at {i}: {b[i:i + 5].hex()}')
    i += 5
    ident, i = string(b, i)
    for _ in range(4):
        _, i = string(b, i)
    i += 4
    if b[i + 1:i + 3] != b'\x00\x06':
        raise Bad(f'tail {b[i:i + 3].hex()} at {i}')
    i += 3
    if out is not None:
        out.append((ident, start, i, parent))
    return i


def patch(b):
    """b with the new header after each QuickSlotHeader8 (found by parsing back from its id)."""
    res, pos, done = b, 0, 0
    while True:
        at = res.find(struct.pack('<I', len(HEADER)) + HEADER, pos)
        if at < 0:
            return res, done
        hit = None
        # the element starts before its id; its end comes first: try starts going back
        for start in range(at - 1, max(at - 3000, 0), -1):
            if res[start] != 1:
                continue
            found = []
            try:
                end = element(res, start, found)
            except Bad:
                continue
            if found and found[-1][0] == HEADER and found[-1][1] == start:
                hit = (start, end)
                break
        if not hit:
            raise Bad(f'no element for the header id at {at}')
        start, end = hit
        # the parent, QuickSlotTabbarHeaderWrap (its classes end with this string), has CHILDREN children in the source;
        # its earlier children hold compiled script data this parser doesn't read, so it's found by that byte pattern
        at_parent = res.rfind(PARENT, 0, start)
        assert at_parent >= 0 and start - at_parent < 20000 and res.count(PARENT, at_parent, start) == 1, 'parent not found'
        count_at = at_parent + len(PARENT) - 4
        n = u32(res, count_at)
        old = struct.pack('<I', len(HEADER)) + HEADER
        copy = res[start:end]
        assert copy.count(old) == 1
        copy = copy.replace(old, struct.pack('<I', len(NEW)) + NEW)
        res = res[:end] + copy + res[end:]
        res = res[:count_at] + struct.pack('<I', n + 1) + res[count_at + 4:]
        check = []
        assert element(res, end, check) == end + len(copy) and check[-1][0] == NEW and check[-1][1] == end
        print(f'  header at {start}..{end}: parent count at {count_at}: {n} -> {n + 1}; the copy re-parses OK')
        pos = end + len(copy)
        done += 1


def lp(x): return struct.pack('<I', len(x)) + x


WRAP, TEXT, VALUE = b'CoolTimeChargedCountWrap', b'CoolTimeChargedCount', b'OmValueText'


def patch_value(b):
    """b with OmValueText after the CoolTimeChargedCount in every CoolTimeChargedCountWrap."""
    res, pos, done = b, 0, 0
    while True:
        at = res.find(lp(WRAP), pos)
        if at < 0:
            return res, done
        hit = None
        for start in range(at - 1, max(at - 3000, 0), -1):
            if res[start] != 1:
                continue
            found = []
            try:
                end = element(res, start, found)
            except Bad:
                continue
            if found and found[-1][0] == WRAP and found[-1][1] == start:
                hit = (start, end, found)
                break
        if not hit:
            raise Bad(f'no element for the wrap id at {at}')
        start, end, found = hit
        kids = [f for f in found if f[3] and f[3][0] == start]
        assert len(kids) == 1 and kids[0][0] == TEXT, [k[0] for k in kids]
        _, cs, ce, (_, count_at) = kids[0]
        copy = res[cs:ce]
        for old, new in ((lp(TEXT), lp(VALUE)), (lp(b'charged-count'), lp(b'om-value-text')),
                         (lp(b'font-text-caption-white'), lp(b'font-text-caption-yellow'))):
            assert copy.count(old) == 1, old
            copy = copy.replace(old, new)
        n = u32(res, count_at)
        assert n == 1
        res = res[:ce] + copy + res[ce:]
        res = res[:count_at] + struct.pack('<I', n + 1) + res[count_at + 4:]
        check = []
        assert element(res, start, check) == end + len(copy) and any(c[0] == VALUE for c in check)
        print(f'  value text in the wrap at {start}: children 1 -> 2; re-parses OK')
        pos = end + len(copy)
        done += 1


game = pack.vanilla_index()
# (the slot components themselves come from itemquickslotview2_component_game: the value text goes there too; it has no
# tab headers)
for p in FILES + ['ui/bin__/itemquickslotview2_component_game.uibinary']:
    b = game[p].read(p)
    print(p, len(b))
    r, n = patch(b) if p in FILES else (b, 0)
    print(f'  {n} header(s) added, {len(b)} -> {len(r)}')
    assert n or p not in FILES
    r, v = patch_value(r)
    print(f'  {v} value text(s) added, -> {len(r)}')
    assert v
    if write:
        full = os.path.join(MOD, *p.split('/'))
        os.makedirs(os.path.dirname(full), exist_ok=True)
        open(full, 'wb').write(r)
        print('  written')
