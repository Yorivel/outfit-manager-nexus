Outfit Manager 0.2.0 by Strahlimeier
Adds an Outfits tab to Crimson Desert's gear wheel: save what you wear, switch outfits with one click.

INSTALL (DMM)
  Install with "Mod Manager Download" on the Nexus page (or drop the whole archive on DMM). It installs two things:
    - OutfitManager.asi   (ASI tab)    the plugin
    - Outfit Manager      (Mods tab)   the gear-wheel tab and its art
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
  Controller: aim with the stick and press A (Cross with Steam Input) instead of clicking.

REMOVE
  Run Uninstall from the global settings (scroll over +) first, then remove both entries in DMM.
  Uninstall deletes your saved outfits on every save, along with the mod's settings, cache and log.

FILES
  Your outfits are kept in OutfitManager.outfits in the game's bin64 folder. Keep a copy if you reinstall.
  OutfitManager.log there records what the plugin did; please attach it to bug reports.

NOTES
  Tested on Crimson Desert 1.0.0.3479 (with the Charting the Unknown expansion) and DMM 4.7.0.
  Controller support is a first, basic version made without a controller to test on: A confirms, and
  PlayStation pads need Steam Input for now. If you play on a controller, please report what works and
  what doesn't; your reports decide what comes next.
  The mod replaces three of the game's gear-wheel UI files, so other mods that change the gear wheel's
  layout will clash with it, and a game update that changes those files needs a new version of this mod.

LICENCE
  MIT, see LICENSE.txt (covers the mod's own work, not the game's content).
  MinHook: see THIRD-PARTY-MinHook-LICENSE.txt.
