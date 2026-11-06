# Changelog

Outfit Manager for Crimson Desert. Downloads: [Nexus Mods](https://www.nexusmods.com/crimsondesert/mods/4029) or this repository's [releases](https://github.com/Yorivel/outfit-manager-nexus/releases).

## 0.4.1 (2026-11-06)

- Fix: hidden pieces showed again on the character you switch away from (switching Kliff, Damiane and Oongka through the gear wheel). The wheel compared the new character's gear with the old character's outfits, took it for a new set and dropped that character's visibility settings. Thanks for the report!
- Fix: two one-handed weapons worn in each other's hands. The game refuses to take either off, so every load left both hands as they were, ended with "18 of 20" in the log and the outfit showed as changed (amber). Weapons swapped between the hands now count as the outfit's: loads leave them alone and the outfit shows as worn.
- Safety: after every load the mod checks that each piece it took off or swapped out is still worn, in a bag or with a companion. If the game loses one on the way to your bags, you get a message telling you not to save (restarting the game brings it back), and the log names the piece. This is for a rare case where a piece swapped out by a load vanished: it could not be spawned again and came back after a restart. We could not reproduce it, so this catches it and tells you, and the next log will show exactly which step lost it.
- Safety: if the game refuses to take a piece off during a load, nothing is put on in that slot in the same load.
- Translations: the new message in all 15 languages.
- Technical: the character switch: the wheel's refresh and the "+" slot's check now skip the worn-outfit comparison while the controlled character isn't the one whose outfit list is loaded (the model-swap recheck already did). The race: the wheel refreshed in the 15 ms between the switch and the mod picking up the new character's list.
- Technical: hands_crossed(): equipment-0 / equipment-1 (MainWeapon_*_R / _L) holding the outfit's two weapons crossed count as matching in the load, its check ("slots match") and the worn-outfit detection.
- Technical: the loss check runs when a load is done, before Stash moves anything, against the same inventory read as the missing-pieces question (worn, every bag, companions); log line "loss check: N piece(s) left their slots, M lost".
- Tested on: Crimson Desert 1.0.0.3549 (Steam build 26448710) with DMM 4.7.3.

## 0.4.0 (2026-11-06)

- New: the Outfit Arc. Hold a key and your outfits fan out on an arc beside your character; scroll to pick one and let go to wear it, without opening the gear wheel. It opens on the outfit you're wearing and wraps around, in the same order as the Outfits tab. If what you wear isn't a saved outfit, a "Current Outfit" entry comes first, and letting go on it changes nothing.
- New: the Outfit Arc on controllers: hold L1 / LB and press down on the d-pad; up and down move along it, letting go of L1 / LB wears the highlighted outfit.
- The Outfit Arc's binding is on the gear wheel's + settings page, Off by default (in every layout, every key is already used by something). Click it and press any key or mouse button (middle, 4 or 5); Esc cancels, Backspace turns it off. The page shows the binding of the input you're using (keyboard and mouse, or your controller's own button names).
- Loading from the Outfit Arc works exactly like a click in the wheel: pieces come from your bags, storage, wardrobe or companions, with the same questions. It closes by itself when you switch character or open the gear wheel.
- While the Outfit Arc is open, the mouse wheel and the d-pad's up and down only move the arc: the camera doesn't zoom and the horse isn't called.
- The Outfit Arc shows the same badges as the wheel: each outfit's climate thermometer on its slot (manual Cold / Heat, or Auto from its resistance), and beside the highlighted outfit's name its thermometer, the eye for custom looks and its hotkey.
- New: per-outfit hotkeys. On an outfit's settings page, click Hotkey and press a key (with Ctrl, Alt or Shift if you like, or the key alone); pressing it while you play puts that outfit on. Hotkeys belong to the outfit, so each character has their own and they follow your saves.
- Hotkeys show on the outfit's slot in the gear wheel as the game's own key icons. One combination belongs to one outfit: giving it to another moves it there. The Outfit Arc's key can't also be a hotkey.
- A hotkey's key doesn't also do its normal action in the game. A key alone (say X) still works while you hold Shift to sprint, unless Shift + X is another outfit's hotkey. Overwrite with "Keep settings" keeps the hotkey; "Reset all" clears it. Hotkeys don't work in menus, while naming an outfit, or with the Outfit Arc open.
- Fix: after a game patch renumbers items, a save written before the patch brings back outfits with the old item numbers. The gear wheel repaired them when it opened, but anything that loaded before that could fetch the wrong pieces. The repair now runs for every way of loading.
- Translations: the new lines in all 14 languages (AI-made like the rest; corrections welcome).
- Technical: the Outfit Arc's elements are added in memory to the status screen (statusgaugeview2_game.uibinary, the same loader hook as the gear wheel's tab), inside the HP tracker so the arc follows the character. Its art and layout ship as an XML merge and a CSS merge, like the rest of the mod.
- Technical: the arc runs from the status screen's frame call, found by its class name (UIGamePlayControlRootStatusGauge) and checked against an address the resolver already finds. A load started from the arc finishes even if the gear wheel was never opened (the UI thread is now known from the status screen).
- Technical: inputs are read from Windows, not the game: keyboard keys from the game window's messages (the game reads them there too), the mouse wheel through a low-level mouse hook that only exists while the arc is open (in controller mode the game passes no wheel to its window). The arc key and hotkey keys are filtered out of the game's window messages and raw mouse input while you play.
- Technical: controllers: PlayStation pads (DualSense, DualShock 4) are read from their HID reports, Xbox pads through XInput. While the arc is open from the pad, the d-pad's up and down are taken out of what the game reads: the PlayStation pad's reports (ReadFile / GetOverlappedResult on the game's own pad handle; the Bluetooth checksum is rewritten) and XInput's button state (xinput1_4). No game addresses are involved.
- Technical: the hotkey key caps: the gear-wheel patch adds a row of seven image elements next to each slot's value text; the plugin fills them with the game's key guide icons (Common_Icon_Keyguide_PC_*). Keys the game has no icon for (F13-F24) show as text.
- Tested on: Crimson Desert 1.0.0.3549 (Steam build 26448710, the 5 November patch) with DMM 4.7.3. Controllers: DualSense (native); Xbox through Steam Input.

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
