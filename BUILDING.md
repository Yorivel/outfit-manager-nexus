# Outfit Manager 0.2.0: source

Source of [Outfit Manager](https://www.nexusmods.com/crimsondesert/mods/4029) for Crimson Desert, by Strahlimeier.
MIT licence (see `LICENSE.txt`); MinHook keeps its own BSD licence (`THIRD-PARTY-MinHook-LICENSE.txt`).
To play, use the main file, not this one.

## Layout

| Folder | What |
|---|---|
| `src/` | the plugin, `OutfitManager.asi` (C++20, MSVC). `outfit_manager.cpp` is the mod; `resolver.*` finds the game's functions and data again after a game update; `om_sigs.h` holds their signatures |
| `mod/Outfit Manager/` | the DMM browser mod: gear-wheel UI files with the Outfits tab, CSS merges, texture list, textures, manifest |
| `tools/` | Python scripts that made the mod's art and UI files (see below) |

## Building the plugin

Visual Studio 2022 (Desktop C++), then run `build.cmd`. CMake fetches MinHook v1.3.4.
Output: `build\OutfitManager.asi`.

- `OutfitManagerResolveTest`: a build that ignores its address cache, to check every address is still found by
  search alone after a game update (one launch, then put the normal build back).
- `ResolverTest`: the same check offline against game executables on disk.

## Tools (reference)

Python 3 scripts used to make the mod's files. They have the author's folder paths at the top; edit them before use.

- `omring_gen.py`: the Outfits wheel art for 1 to 12 slots (rings, hover and worn wedges), from the game's own wheels.
- `uibin_tab.py`: inserts the Outfits tab header into the game's compiled gear-wheel UI files (`ui/bin__/*.uibinary`).
  It needs the vanilla files; the author's setup gets them from a separate tool (`pack.vanilla_index()`), not included.
- `pamt.py`, `paz.py`, `tex.py`, `pngread.py`: read the game's archives and textures (archive format and key
  derivation from lazorr410's crimson-desert-unpacker and NattKh's CrimsonDesertModdingTools).

Crimson Desert and its content belong to Pearl Abyss; the licence covers the mod's own work only.
