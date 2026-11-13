Outfit Manager 0.7.1 by Strahlimeier
Adds an Outfits tab to Crimson Desert's gear wheel: save what you wear, switch outfits with one click.

INSTALL (DMM)
  Install with "Mod Manager Download" on the Nexus page (or drop the whole archive on DMM). It installs two things:
    - OutfitManager.asi   (ASI tab)    the plugin
    - Outfit Manager      (Mods tab)   the gear-wheel tab, its art and the languages
  Enable both, then mount.

UPDATING
  Installed with Mod Manager Download, DMM links the mod to its Nexus page and tells you when an update is out.
  A dropped archive isn't linked until you link it from the mod's Nexus badge in DMM.
  Your outfits live outside the game's folder (see FILES), so updating or reinstalling the mod never touches them.
  Version: the plugin's file properties and the first line of OutfitManager.log show which one you run.

USE
  Open the gear wheel, switch to the Outfits tab.
  Click + to save the armour you are wearing; click an outfit to put it on.
  Scroll over an outfit for its settings, or over + for the global ones.
  Once you have outfits, + has three dots: Unequip Outfit (the default), Save and Settings.
  Unequip Outfit takes nothing off: the outfit stops applying (its visibility settings are off and your gear shows as
  it is) until you load or save an outfit.
  Controller: aim with the stick, press A / Cross to confirm, L2 / R2 to scroll a slot (shown on the slot).

OUTFIT ARC
  Switch without opening the wheel: hold the Outfit Arc's key, scroll to pick an outfit, let go to wear it.
  Its first entry, No Outfit, unequips the outfit. It opens on the outfit you have on; letting go there changes nothing.
  Set the key on the + page (Outfit Arc, Off by default): click it, then press any key or mouse button (middle, 4, 5).
  Esc cancels, Backspace turns it off.
  Controller: turn it on in the + page (it shows the pad's entry while you use a pad), then hold L1 / LB and press
  down on the d-pad; up and down move along it, let go of L1 / LB to wear the highlighted outfit.
  While it is open, the mouse wheel and the d-pad's up and down only move the arc.

WEAPON SETS
  On an outfit's settings page, scroll Weapons to Weapons Only: the slot becomes a weapon set. Clicking it (or its
  hotkey) changes only your weapons; your outfit and its visibility settings stay. Its badge turns gold while you wear
  exactly its weapons. An outfit on Load still brings its own weapons; on Leave, your weapons stay.
  A weapon set's Icon setting chooses which of its weapons pictures it (scroll it: the slot shows the weapon).

PAGES
  From the 12th outfit on, the wheel shows 10 outfits per page with a Page slot next to +: scroll it to choose a
  page (the gold dot holds the outfit you wear), click to open it.

OUTFIT ARC PINS
  The Arc lists No Outfit, your pinned outfits, then the 3 you wore most recently. Its first entry, Edit Pins, pins
  or unpins the highlighted outfit; hold a pinned one to move it. Press No Outfit to switch the Arc to weapon sets.

SLEEPWEAR
  An outfit's Activity setting: set to Sleeping, it goes on when you get into bed; what you wore comes back on waking.

HELMET KEY
  A key on the + page that hides or shows your helmet whatever the outfit says; on foot your character takes it off
  or puts it on with the game's own animation. Press it again, or change outfit, and the outfit decides again.

CHARACTER LOOK
  An outfit's Character Look setting: scroll to Snapshot and click to save your character's look as it is now
  (hairstyle, beard, colours, tattoos, makeup). The outfit brings that look whenever you wear it; other outfits and
  No Outfit show your own look. Changing your look at the barber while such an outfit is on asks once, after you
  leave, whether the change goes to that outfit or becomes your own look.

OUTFITS PER SAVE
  Your outfits are saved with each game save. The + page's Outfits setting can make them the same in every save
  instead. A save made while the mod was off takes your current outfits.

NO OUTFIT VISIBILITY
  The + page's Visibility entry: how each piece shows while no outfit is applied, per character.

UNDERWEAR
  An outfit's visibility settings end their Armour page with Underwear: hide it or show it like any piece.

EFFECTS
  Changing outfits plays a glow over the character; the new outfit appears at its peak. Hidden weapons glow when you
  draw them and when you put them away. Set it in the global settings (scroll over +): Effects Off, Performance (no
  weapon sparkles) or Quality (the default).

HOTKEYS
  On an outfit's settings page, click Hotkey and press a key, with Ctrl, Alt or Shift if you like (or none).
  Pressing it while you play puts the outfit on. It shows on the outfit's slot with the game's key icons.
  Each outfit keeps its own, so each character has their own hotkeys, saved with your game saves.
  A combination belongs to one outfit; giving it to another moves it. Backspace (while choosing) clears it.
  No controller hotkeys yet: the game uses every button. Ideas are welcome on the Nexus page.

LANGUAGES
  The mod follows the game's language (its own setting, else Steam's). To pick another one, scroll Language in the
  global settings (scroll over +). All 15 of the game's languages are included.
  The translations were made with AI, so they may read stiffly. Corrections are very welcome: post them on the Nexus
  page and they go into the next version, credited to you.
  All languages are built into the plugin. The texts are also in "Outfit Manager\lang" in the game's bin64 folder,
  one file per language (en.txt lists every line, with notes on the format); those files win over the built-in text.
  To change lines just for yourself, put them in a file named after your language with ".custom" added
  (fr.custom.txt, ja.custom.txt, ...): it has the same format, only needs the lines you change, and updates never
  overwrite it.
  Codes: en English, fr French, de German, it Italian, es Spanish (Spain), es-mx Spanish (Latin America),
  pt-br Portuguese (Brazil), pl Polish, ru Russian, tr Turkish, ar Arabic, ja Japanese, ko Korean,
  zh-cn Chinese (Simplified), zh-tw Chinese (Traditional).
  Japanese, Korean, Chinese and Arabic may show as empty boxes when the game itself is set to another language: the
  game loads the fonts for its own language only.

REMOVE
  Run Uninstall from the global settings (scroll over +) first, then remove both entries in DMM.
  Uninstall deletes your saved outfits on every save, along with the mod's settings, backups, cache and log.

FILES
  Your outfits (and the mod's settings) are kept in %LOCALAPPDATA%\Outfit Manager\OutfitManager.outfits
  (paste that path into Explorer's address bar). Since 0.7.0: an older version's file in the game's bin64 folder
  moves there by itself on the first start.
  OutfitManager.log in the game's bin64 folder records what the plugin did; please attach it to bug reports.

NOTES
  Tested on Crimson Desert 1.0.0.3663 (2.05, with the Charting the Unknown expansion) and DMM 5.0.0.
  Controllers: tested on a DualSense, both directly and through Steam Input (Xbox layout).
  The mod ships none of the game's own files: the plugin adds the Outfits tab to the gear wheel while the game loads
  it, onto whatever copy it loads (the game's, or another mod's), and DMM merges the mod's CSS and texture list into
  the game's. So it works alongside other mods that change the gear wheel (tested with Better Radial Menus).

LICENCE
  MIT, see LICENSE.txt (covers the mod's own work, not the game's content).
  MinHook: see THIRD-PARTY-MinHook-LICENSE.txt.
