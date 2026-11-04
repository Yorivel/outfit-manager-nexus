"""The game's own words for terms Outfit Manager uses, in all its languages: English texts looked up in the English string
tables, the same ids read from every other language's tables. For matching the mod's translations to the game.
lang_terms.py [word ...]   (default: the terms below)"""
import sys

sys.path.insert(0, 'F:/MyProjects/CDOverlayLoader')
words = sys.argv[1:]
sys.argv = sys.argv[:1]
import pack
import paloc_dump

TERMS = words or ['Kliff', 'Damiane', 'Oongka', 'Storage', 'Wardrobe', 'Inventory', 'Cancel', 'Outfit', 'Outfits',
                  'Helmet', 'Gloves', 'Shoes', 'Cloak', 'Necklace', 'Mask', 'Glasses', 'Earring', 'Ring', 'Armor',
                  'Weapons', 'Accessories', 'Language', 'Settings', 'Delete', 'Rename', 'Back', 'Auto', 'Off', 'On',
                  'Default', 'Chest', 'Back Shield', 'Ice Resistance', 'Fire Resistance', 'Party']
LANGS = ['eng', 'fre', 'ger', 'ita', 'spa-es', 'spa-mx', 'por-br', 'pol', 'rus', 'tur', 'ara', 'jpn', 'kor', 'zho-cn', 'zho-tw']

v = pack.vanilla_index()


def table(lang):
    out = {}
    for n in (x for x in v if x.startswith(f'gamedata/stringtable/binary__/{lang}/')):
        name = n.split('/')[-1].rsplit('.', 1)[0]
        for k, t in paloc_dump.records(paloc_dump.unpack(v[n].read(n))):
            out[(name, k)] = t
    return out


eng = table('eng')
ids = {}
for w in TERMS:
    hits = [key for key, t in eng.items() if t.strip() == w]
    ids[w] = hits[:1]
tables = {l: table(l) for l in LANGS[1:]}
for w in TERMS:
    if not ids[w]:
        print(f'{w}: not in the game')
        continue
    key = ids[w][0]
    print(f'{w}  ({key[0]} {key[1]}): ' + ' | '.join(f'{l} {tables[l].get(key, "?")}' for l in LANGS[1:]))
