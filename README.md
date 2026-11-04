<p align="center"><img src="page/banner_2600x744.png" alt="Outfit Manager" width="100%"></p>

<p align="center">
  <b>Save and switch outfits from Crimson Desert's gear wheel.</b><br>
  <a href="https://www.nexusmods.com/crimsondesert/mods/4029"><b>Download on Nexus Mods</b></a>
</p>

---

This repository holds [Outfit Manager](https://www.nexusmods.com/crimsondesert/mods/4029)'s source code, its
[releases](https://github.com/Yorivel/outfit-manager-nexus/releases) and [changelog](CHANGELOG.md), and the images
used on its Nexus page. To play, install it from Nexus with DMM (or download a release here).

<p align="center"><img src="description/03_media_hero.gif" alt="Saving an outfit" width="560"></p>

## What the mod does

- **An Outfits tab in the gear wheel**, next to Weapons and Armour: save what you're wearing with one click and keep
  up to 11 outfits per character.
- **One-click switching.** Missing pieces are fetched from your bags, storage, the wardrobe or your party.
- **Kliff, Damiane and Oongka** each keep their own outfits, separately for every save.
- **Visibility per outfit:** every piece and weapon can be always shown, always hidden, shown only in combat or
  hidden in combat. Keep the armour, choose the look.
- **Controllers:** aim with the stick, A / Cross to confirm, L2 / R2 to change a slot's settings.
- **15 languages**, following the game's (translations made with AI: corrections welcome).

<p align="center"><img src="description/10_media_visibility_world.gif" alt="Hiding the hat and cloak" width="600"></p>

## Folders

| Folder | Used for |
|---|---|
| [`src/`](src) | the plugin, `OutfitManager.asi` (C++20); build it with `build.cmd`, see [BUILDING.md](BUILDING.md) |
| [`mod/`](mod) | the DMM mod: the gear wheel's UI files, art and the language files ([`mod/Outfit Manager/lang`](mod/Outfit%20Manager/lang)) |
| [`tools/`](tools) | the Python scripts that made the mod's art, UI and language files |
| [`description/`](description) | the mod page's description, numbered in page order: header, separators, screenshots and GIFs |
| [`page/`](page) | the page banner, cover, thumbnail and logos |
| [`files/`](files) | the description images of the main file and the source download |
| [`posts/`](posts) | images for the pinned posts (bug reports) |

## Credits and licence

Page art built from Crimson Desert's own UI art and its UI font (Vollkorn by Friedrich Althausen, SIL Open Font
License); screenshots and GIFs are from the game. Crimson Desert and its content belong to Pearl Abyss.
The mod is released under the [MIT licence](LICENSE.txt); MinHook keeps its own ([BSD](THIRD-PARTY-MinHook-LICENSE.txt)).

Made by Strahlimeier.
