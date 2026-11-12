# Chrome Axis 0.4.15 · Nexus Mods

[Français](#français) · [English](#english)

## Français

### Préparer la page

- Ouvrez l’[aperçu de la page](apercu-description.html) pour parcourir les deux versions avec leurs images.
- Copiez le nom, le résumé court et le texte du fichier principal depuis la [fiche Nexus](fiche-nexus.md).
- Dans **Description**, collez la [description bilingue illustrée](description-bilingue.bbcode.txt) en mode source BBCode. Les couvertures, cartes de démarrage et captures 0.4.15 sont déjà intégrées dans les deux langues. Les exports [français](description-fr.bbcode.txt) et [anglais](description-en.bbcode.txt) incluent les visuels correspondants.
- Dans **Media → Header**, ajoutez la [bannière bilingue](images/00-header-nexus-fr-en.jpg) de 1300 × 372 px.
- Téléversez les visuels de la galerie depuis [images/fr](images/fr/) ou [images/en](images/en/) dans **Media**. Utilisez `01-cover.jpg` comme couverture paysage et `02-square.jpg` pour les emplacements au format carré.

### Fichier principal et prérequis

| Champ Nexus | Valeur |
| --- | --- |
| Jeu et catégorie | Cyberpunk 2077 · Gameplay |
| Version du mod | 0.4.15 |
| Compatibilité ciblée | Windows x64 · Cyberpunk 2077 2.5 · exécutable 3.0.88.46235 |
| ZIP du mod | [ChromeAxis-0.4.15-Cyberpunk-2.5.zip](../out/ChromeAxis-0.4.15-Cyberpunk-2.5.zip) |
| SHA-256 | `D0888D8782011692F997C1AA0D31824BD166C7E857B9E06A0C4AED1EEF707191` |
| Requirements | [RED4ext 1.31.1](https://www.nexusmods.com/cyberpunk2077/mods/2380?tab=files) et [redscript 0.5.32](https://www.nexusmods.com/cyberpunk2077/mods/1511?tab=files) |

Le ZIP inclut le plugin, l’application `ChromeAxis.exe`, le HUD et les deux guides d’utilisation. Dans Chrome Axis, choisissez le drapeau à droite de **Simulation** ou passez par **Aide → Langue**, puis relancez l’application. Le HUD s’affiche en français si le jeu est en français, et en anglais dans les autres cas. [Guide d’utilisation français](../README.md)

### Intégrer les images dans la description

Pour ajouter un autre visuel, téléversez-le dans **Media**, ouvrez-le en grand et copiez son adresse HTTPS. Utilisez le bouton d’insertion d’image ou placez l’adresse entre `[img]` et `[/img]`. Vérifiez le résultat en mode visuel. [Formulaire Nexus](https://www.nexusmods.com/blackbook/news/15491) · [Conseil de l’équipe Nexus sur les URL d’images](https://forums.nexusmods.com/topic/13534985-beta-form-feedback/)

| Image | Emplacement dans la description française |
| --- | --- |
| `01-cover.jpg` | Juste sous le titre « Français » si vous souhaitez répéter la couverture dans la description |
| `03-first-drive.jpg` | Après les deux puces sur le volant et la simulation dans l’introduction |
| `04-calibration.png` | Après les étapes de « Conduire au volant » |
| `05-simulation.png` | Après les raccourcis et la note sur Verr. num. dans « Essayer au clavier » |

### Légendes de la galerie

| Image FR | Légende à copier |
| --- | --- |
| [01-cover.jpg](images/fr/01-cover.jpg) | Chrome Axis ajoute la direction, l’accélérateur et le frein analogiques à Cyberpunk 2077 sur PC. Illustration promotionnelle. |
| [02-square.jpg](images/fr/02-square.jpg) | Visuel carré de Chrome Axis pour Cyberpunk 2077. |
| [03-first-drive.jpg](images/fr/03-first-drive.jpg) | Deux façons de faire un premier trajet avec Chrome Axis, au volant ou avec la simulation clavier. |
| [04-calibration.png](images/fr/04-calibration.png) | Onglet Matériel USB. Choisissez un périphérique par axe, ajustez les zones mortes et enregistrez le profil. |
| [05-simulation.png](images/fr/05-simulation.png) | Onglet Simulation en mode aperçu. Les commandes fonctionnent avec le pavé numérique ou F6, F7, F8 et F10. |
| [06-accessories.png](images/fr/06-accessories.png) | Associez un bouton, une direction de croix ou un axe DirectInput à une touche. |
| [07-key-picker.png](images/fr/07-key-picker.png) | Clavier visuel. La rangée de chiffres et le pavé numérique sont distincts. |
| [08-profiles.png](images/fr/08-profiles.png) | Chargez un profil existant ou créez une copie nommée du profil actif. |
| [09-shifter-paddles.png](images/fr/09-shifter-paddles.png) | Assistant de boîte et palettes, affiché en grille H à six rapports et marche arrière. Une grille H nécessite un mod de transmission avec une commande par rapport. |
| [10-force-feedback.png](images/fr/10-force-feedback.png) | Réglages du retour de force. L’effet nécessite un axe de direction calibré et un actionneur DirectInput compatible. |

Les captures montrent l’application 0.4.15 avec un profil vierge et sans périphérique connecté. Pour ajouter une vraie capture du HUD, lancez le jeu avec Chrome Axis déployé, choisissez la langue d’affichage du jeu et activez **Simulation → Activer dans le jeu** dans l’application. Laissez-la ouverte. Installez V au siège conducteur, relâchez les touches et capturez l’écran quand le HUD affiche **SIMULATION**. Reprenez la même scène dans l’autre langue pour obtenir la paire.

### Logos et crédits

- Le [logo transparent du HUD](logos/chrome-axis-hud-transparent.png) est commun aux deux langues. Le [visuel carré français en PNG](logos/fr/chrome-axis-square.png) et son [équivalent anglais](logos/en/chrome-axis-square.png) reprennent le même design.
- L’[illustration source sans texte](sources/illustration-volant.png) a été générée avec ImageGen. Les anciens visuels directement sous `images/` servent aux liens existants. Les visuels localisés de la 0.4.15 se trouvent sous `images/fr/` et `images/en/`.
- Conservez les crédits dans la description et la notice dans le ZIP. Indiquez **AI Media** pour l’illustration promotionnelle et le texte de présentation assisté par Codex. Le détourage du logo du HUD a été réalisé avec ImageGen. [Crédits du projet](../THIRD_PARTY_NOTICES.md) · [Règles Nexus sur l’IA](https://help.nexusmods.com/article/180-ai-policy-changes-2026)

## English

### Prepare the page

- Open the [page preview](apercu-description.html) to browse both versions with their images.
- Copy the mod name, short description and main file description from the [Nexus listing sheet](fiche-nexus.md).
- In **Description**, paste the [illustrated bilingual description](description-bilingue.bbcode.txt) in BBCode source mode. It embeds the 0.4.15 covers, quick-start cards and screenshots in both languages. The [English](description-en.bbcode.txt) and [French](description-fr.bbcode.txt) exports include the matching visuals.
- In **Media → Header**, add the [bilingual banner](images/00-header-nexus-fr-en.jpg) at 1300 × 372 px.
- Upload the gallery images from [images/en](images/en/) or [images/fr](images/fr/) in **Media**. Use `01-cover.jpg` for the landscape cover and `02-square.jpg` for square placements.

### Main file and requirements

| Nexus field | Value |
| --- | --- |
| Game and category | Cyberpunk 2077 · Gameplay |
| Mod version | 0.4.15 |
| Target compatibility | Windows x64 · Cyberpunk 2077 2.5 · executable 3.0.88.46235 |
| Mod ZIP | [ChromeAxis-0.4.15-Cyberpunk-2.5.zip](../out/ChromeAxis-0.4.15-Cyberpunk-2.5.zip) |
| SHA-256 | `D0888D8782011692F997C1AA0D31824BD166C7E857B9E06A0C4AED1EEF707191` |
| Requirements | [RED4ext 1.31.1](https://www.nexusmods.com/cyberpunk2077/mods/2380?tab=files) and [redscript 0.5.32](https://www.nexusmods.com/cyberpunk2077/mods/1511?tab=files) |

The ZIP includes the plugin, the `ChromeAxis.exe` app, the HUD and both user guides. In Chrome Axis, select a flag to the right of **Simulation** or use **Help → Language**, then restart the app. The HUD follows the Cyberpunk 2077 display language and falls back to English for game languages other than French. [English user guide](../README.en.md)

### Add images to the description

To add another image, upload it in **Media**, open it at full size and copy its HTTPS address. Use the editor’s insert image button or put the address between `[img]` and `[/img]`. Check the result in visual mode. [Nexus upload form](https://www.nexusmods.com/blackbook/news/15491) · [Nexus staff guidance on image URLs](https://forums.nexusmods.com/topic/13534985-beta-form-feedback/)

| Image | Placement in the English description |
| --- | --- |
| `01-cover.jpg` | Directly below “English” if you want to repeat the cover in the description |
| `03-first-drive.jpg` | After the two bullets on wheel driving and simulation in the introduction |
| `04-calibration.png` | After the steps in “Drive with a wheel” |
| `05-simulation.png` | After the shortcuts and Num Lock note in “Try it with a keyboard” |

### Gallery captions

| EN image | Caption to copy |
| --- | --- |
| [01-cover.jpg](images/en/01-cover.jpg) | Chrome Axis adds analog steering, throttle and braking to Cyberpunk 2077 on PC. Promotional artwork. |
| [02-square.jpg](images/en/02-square.jpg) | Square Chrome Axis artwork for Cyberpunk 2077. |
| [03-first-drive.jpg](images/en/03-first-drive.jpg) | Two ways to take your first drive with Chrome Axis, with a wheel or with keyboard simulation. |
| [04-calibration.png](images/en/04-calibration.png) | USB Hardware tab. Choose a device for each axis, adjust dead zones and save the profile. |
| [05-simulation.png](images/en/05-simulation.png) | Simulation tab in preview mode. Try the controls with the numpad or F6, F7, F8 and F10. |
| [06-accessories.png](images/en/06-accessories.png) | Assign a DirectInput button, D-pad direction or axis to a key. |
| [07-key-picker.png](images/en/07-key-picker.png) | Visual keyboard. The number row and numpad are mapped separately. |
| [08-profiles.png](images/en/08-profiles.png) | Load an existing profile or create a named copy of the active profile. |
| [09-shifter-paddles.png](images/en/09-shifter-paddles.png) | Shifter and paddles wizard in six-speed H-pattern mode with reverse. H-pattern shifting requires a transmission mod with a command for each gear. |
| [10-force-feedback.png](images/en/10-force-feedback.png) | Force feedback settings. The effect requires a calibrated steering axis and a compatible DirectInput actuator. |

The screenshots show the 0.4.15 app with a blank profile and no devices connected. For a genuine HUD screenshot, launch the game with Chrome Axis deployed, choose the game display language, and select **Simulation → Enable in game** in the app. Leave it open. Put V in the driver’s seat, release the keys and take the screenshot when the HUD reads **SIMULATION**. Repeat the scene in the other language for a matching pair.

### Logos and credits

- The [transparent HUD logo](logos/chrome-axis-hud-transparent.png) is shared by both languages. The [English square PNG](logos/en/chrome-axis-square.png) and its [French counterpart](logos/fr/chrome-axis-square.png) use the same design.
- The [text-free source illustration](sources/illustration-volant.png) was generated with ImageGen. Earlier images directly under `images/` support existing links. The localized 0.4.15 images are in `images/en/` and `images/fr/`.
- Keep the credits in the description and the notices in the ZIP. Select **AI Media** for the promotional artwork and Codex-assisted listing copy. ImageGen was also used to remove the HUD logo’s background. [Project credits](../THIRD_PARTY_NOTICES.md) · [Nexus AI policy](https://help.nexusmods.com/article/180-ai-policy-changes-2026)
