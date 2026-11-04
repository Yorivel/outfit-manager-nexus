# Changelog

Outfit Manager for Crimson Desert. Downloads: [Nexus Mods](https://www.nexusmods.com/crimsondesert/mods/4029) or this repository's [releases](https://github.com/Yorivel/outfit-manager-nexus/releases).

## 0.3.3 (2026-11-05)

- Compatibility: works alongside other mods that change the gear wheel, in any load order. Tested with Better Radial Menus: both mods now apply in full, with no conflict in DMM.
- The mod no longer ships any of the game's own files. Until 0.3.2 it replaced four compiled gear-wheel UI files (ui/bin__/*.uibinary) and the wheel's texture list (cd_hud_itemquickslot_08.xml) as whole files. DMM keeps only one mod's copy of a whole file, so whichever mod lost the conflict lost its changes: with Outfit Manager losing, the Outfits tab disappeared.
- Technical: the plugin now hooks the game's UI loader where it hands a compiled UI file to its parser (pattern-scanned, not a fixed address). The hook goes in while the plugin itself loads, before the game builds any screen, so nothing depends on timing.
- Technical: when one of the three gear-wheel files comes through (itemquickslotview2_game, its _component file, quickslotitem_component_game), the plugin copies whatever version DMM chose (the game's or another mod's) and inserts the Outfits tab header (a copy of the last tab header, QuickSlotHeader8) and the gold value text (a copy of each slot's CoolTimeChargedCount text). The parser gets the patched copy. On the game's own files the result is byte for byte what 0.3.2 shipped.
- Technical: if a file isn't laid out as expected (say, an update renames those elements), it is left untouched and the log says why. The gear wheel then works as normal, just without the Outfits tab, instead of breaking. Every patched file is logged ("gear wheel UI: ... added") so a bug report shows which copy was patched.
- Technical: the texture list entries now come only from the XML merge, which DMM adds onto whatever copy of the list it mounts. The quickslotitem.thtml merge is gone: since the 10-30 patch the game builds its screens from the compiled .uibinary files and never reads .thtml.
- Game updates that change the gear-wheel files no longer need a new version of the mod, as long as the two elements it copies are still there.
- Update resilience: seven more fields the plugin reads from the game are now checked against the game's own code at start-up and followed if a patch moves them: the name box's caret, selection end, caret-redraw flag, busy flag and text element (typing symbols), an element's on-screen rectangle (controller aiming, button prompts) and the dots-per-page field (slot dots). That makes 57 fields and 129 code addresses, all found again by search after an update.

## 0.3.2 (2026-11-04)

- Updated for the game patch of 2026-11-04: the gear-wheel UI files are rebuilt from the patched game (the patch changed two of them).
- Fixed: after that patch, Climate on Auto found no resistance on any piece, so no cold / heat badges showed. The mod now finds where the game keeps an item's resistances by itself, so a later patch that moves them again doesn't break it.

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
