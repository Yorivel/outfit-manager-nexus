Outfit Manager 0.3.0 by Strahlimeier
Adds an Outfits tab to Crimson Desert's gear wheel: save what you wear, switch outfits with one click.

INSTALL (DMM)
  Install with "Mod Manager Download" on the Nexus page (or drop the whole archive on DMM). It installs two things:
    - OutfitManager.asi   (ASI tab)    the plugin
    - Outfit Manager      (Mods tab)   the gear-wheel tab, its art and the languages
  Enable both, then mount.

UPDATING
  Installed with Mod Manager Download, DMM links the mod to its Nexus page and tells you when an update is out.
  A dropped archive isn't linked until you link it from the mod's Nexus badge in DMM.
  DMM only replaces the files it installed, so OutfitManager.outfits should stay; keep a copy before updating anyway.
  Version: the plugin's file properties and the first line of OutfitManager.log show which one you run.

USE
  Open the gear wheel, switch to the Outfits tab.
  Click + to save the armour you are wearing; click an outfit to put it on.
  Scroll over an outfit for its settings, or over + for the global ones.
  Controller: aim with the stick, press A / Cross to confirm, L2 / R2 to scroll a slot (shown on the slot).

LANGUAGES
  The mod follows the game's language (its own setting, else Steam's). To pick another one, scroll Language in the
  global settings (scroll over +). All 15 of the game's languages are included.
  The translations were made with AI, so they may read stiffly. Corrections are very welcome: post them on the Nexus
  page and they go into the next version, credited to you.
  The texts are in "Outfit Manager\lang" in the game's bin64 folder, one file per language (en.txt lists every line,
  with notes on the format). To change lines just for yourself, put them in a file named after your language with
  ".custom" added (fr.custom.txt, ja.custom.txt, ...): it has the same format, only needs the lines you change, and
  updates never overwrite it.
  Codes: en English, fr French, de German, it Italian, es Spanish (Spain), es-mx Spanish (Latin America),
  pt-br Portuguese (Brazil), pl Polish, ru Russian, tr Turkish, ar Arabic, ja Japanese, ko Korean,
  zh-cn Chinese (Simplified), zh-tw Chinese (Traditional).
  Japanese, Korean, Chinese and Arabic may show as empty boxes when the game itself is set to another language: the
  game loads the fonts for its own language only.

REMOVE
  Run Uninstall from the global settings (scroll over +) first, then remove both entries in DMM.
  Uninstall deletes your saved outfits on every save, along with the mod's settings, cache and log.

FILES
  Your outfits are kept in OutfitManager.outfits in the game's bin64 folder. Keep a copy if you reinstall.
  OutfitManager.log there records what the plugin did; please attach it to bug reports.

NOTES
  Tested on Crimson Desert 1.0.0.3479 (with the Charting the Unknown expansion) and DMM 4.7.0.
  Controllers: tested on a DualSense, both directly and through Steam Input (Xbox layout).
  The mod replaces four of the game's compiled gear-wheel UI files, so other mods that change the gear wheel's
  layout will clash with it, and a game update that changes those files needs a new version of this mod.

LICENCE
  MIT, see LICENSE.txt (covers the mod's own work, not the game's content).
  MinHook: see THIRD-PARTY-MinHook-LICENSE.txt.
