# Outfit Manager 0.5.0-dev4: source

Source of [Outfit Manager](https://www.nexusmods.com/crimsondesert/mods/4029) for Crimson Desert, by Strahlimeier.
MIT licence (see `LICENSE.txt`); MinHook keeps its own BSD licence (`THIRD-PARTY-MinHook-LICENSE.txt`).
To play, use the main file, not this one.

## Layout

| Folder | What |
|---|---|
| `src/` | the plugin, `OutfitManager.asi` (C++20, MSVC): the modules below, plus `uibin_patch.*` (adds the Outfits tab header, the value texts and the Outfit Arc to the gear wheel's compiled UI files as the game loads them), `lang.*` (its languages), `resolver.*` (finds the game's functions and data again after a game update) and `om_sigs.h` (their signatures) |
| `mod/Outfit Manager/` | the DMM browser mod: the CSS merge, the texture list merge, textures, the language files (`lang/`), manifest |
| `tools/` | Python scripts that made the mod's art and language files (see below) |

## The plugin's modules

Every module is in `namespace om`, includes `om.h` and has a header declaring what the others use from it.

| Module | What |
|---|---|
| `om.h`, `core.cpp` | shared types (`Outfit`, `OutfitItem`, `CharacterOutfits`), constants and state; `om.h` holds the includes |
| `mem.h` | safe memory reads and writes (survive bad pointers) |
| `addrs` | game addresses, struct offsets, game function pointers, the resolver's recipes |
| `ui` | UI element helpers: classes, layout checks, labels, images, dumps, messages |
| `items` | item names, the item table (permanent ids), resistances and climate, key repair |
| `gear` | worn gear, the player and the characters, the bags, where pieces are |
| `store` | the outfits file (`OutfitManager.outfits`), one list per character |
| `saves` | save binding: watching the game's save files, which save is loaded |
| `looks`, `looks_view` | how each piece shows (the visibility engine); the appearance view on the wheel |
| `load`, `stash` | loading an outfit (equip steps, the load pump, the loss check); pieces left in the bag go to storage |
| `prompts` | the game's dialogs: the rename prompt, typed characters, yes / no questions |
| `actions` | saving, overwriting, deleting, cleaning up, uninstalling |
| `settings` | the settings pages and their texts; the plugin's settings |
| `wheel_draw`, `wheel_hooks` | the Outfits tab's slots (images, badges, key caps, the ring, the centre tooltip); the gear wheel's hooks |
| `input`, `pads` | keyboard and mouse (window messages, raw input, the wheel hook); controllers (PlayStation HID, XInput) |
| `arc` | the Outfit Arc and per-outfit hotkeys |
| `selftest`, `ui_hook`, `setup` | the self-test; the hook on the game's UI file loader; start-up (hooks, the init thread) |
| `outfit_manager.cpp` | `DllMain` |

## Building the plugin

Visual Studio 2022 (Desktop C++), then run `build.cmd`. CMake fetches MinHook v1.3.4.
Output: `build\OutfitManager.asi`.

- `OutfitManagerResolveTest`: a build that ignores its address cache, to check every address is still found by
  search alone after a game update (one launch, then put the normal build back).
- `ResolverTest`: the same check offline against game executables on disk.

## Languages

`mod/Outfit Manager/lang/en.txt` is the source of every line the mod shows. After changing it, run
`tools/lang_gen.py`: it writes `src/lang_en.inc` (the English built into the plugin, used for anything a language file
lacks) and checks every other language file against English (unknown lines, `{placeholders}` that differ, lines
missing). Then rebuild the plugin.

## Tools (reference)

Python 3 scripts used to make the mod's files. They have the author's folder paths at the top; edit them before use.

- `omring_gen.py`: the Outfits wheel art for 1 to 12 slots (rings, hover and worn wedges), from the game's own wheels,
  and the mod's CSS.
- `uibin_tab.py`: the Python original of `src/uibin_patch.cpp` (until 0.3.2 the mod shipped the gear wheel's compiled
  UI files patched by it; since 0.3.3 the plugin patches whatever copy the game loads). Kept as reference and to
  compare: its output and the plugin's are byte for byte the same. It needs the vanilla files; the author's setup gets
  them from a separate tool (`pack.vanilla_index()`), not included.
- `om_key_icons.py`: the controller confirm icons (Cross, A), cut from the game's key guide atlas.
- `lang_gen.py`: see Languages. `lang_terms.py` (with `paloc_dump.py`): the game's own words for terms the mod
  uses, in all its languages, from its string tables (also needs `pack`).
- `pamt.py`, `paz.py`, `tex.py`, `pngread.py`: read the game's archives and textures (archive format and key
  derivation from lazorr410's crimson-desert-unpacker and NattKh's CrimsonDesertModdingTools).

Crimson Desert and its content belong to Pearl Abyss; the licence covers the mod's own work only.
