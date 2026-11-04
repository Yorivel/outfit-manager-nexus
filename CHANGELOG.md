# Changelog

Outfit Manager for Crimson Desert. Downloads: [Nexus Mods](https://www.nexusmods.com/crimsondesert/mods/4029) or this repository's [releases](https://github.com/Yorivel/outfit-manager-nexus/releases).

## 0.3.1 (2026-11-04)

- Fixed: the mod could stay in English whatever language was picked, when DMM's update didn't copy the new language folder. All 15 languages are now built into the plugin; the files in Outfit Manager\lang still win over it, for corrections.

## 0.3.0 (2026-11-04)

- Controllers: proper support, tested on a DualSense (direct and through Steam Input). A / Cross confirms, L2 / R2 scroll a slot, and the slots show those buttons only where they do something; the click prompts switch to Cross or A while you play on a controller.
- Languages: all 15 of the game's languages, following the game's own (or pick one in the global settings). Made with AI: corrections welcome, and your own fixes can go in a .custom.txt file that updates never touch.
- Outfit names: symbols and accented letters (! + / * é ...) can now be typed, not only pasted.
- Visibility pages: a Back slot of their own; the page slot now only switches between Armour, Weapons and Accessories.
- Back slots read "Back" with a click prompt, like the other actions.
- Fixed: a slot's dots could show the wrong count after closing the wheel inside the settings or Visibility.
- Fixed: redrawing the dots no longer plays a burst of clicks.
- Setting values are now text in the game's own style instead of pictures (needed for the translations).

## 0.2.0 (2026-11-03)

- First public release.
- Outfits tab in the gear wheel: save what you are wearing and switch outfits with one click (up to 11 per character).
- Missing pieces are fetched from your bags, storage, the wardrobe or your party (asks before taking a companion's).
- Separate outfits for Kliff, Damiane and Oongka, kept per save.
- Per-outfit settings: rename, overwrite, position, climate (cold / heat), weapons, stash and visibility.
- Visibility: every piece and weapon can be always shown, always hidden, shown only in combat or hidden in combat.
- Global settings: Stash (off by default), Party and Messages.
- Basic controller support: A confirms (untested, made without a controller).
- Finds the game's code again after updates; switches off only the affected feature if it can't.
