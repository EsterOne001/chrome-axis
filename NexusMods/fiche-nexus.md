# Chrome Axis sur Nexus Mods

Le résumé et le texte du fichier ont leurs propres champs. Pour la description longue, copiez le contenu de [l’export anglais](description-en.bbcode.txt) ou de [l’export français](description-fr.bbcode.txt) dans le mode source BBCode de l’éditeur Nexus Mods. Les noms des boutons sont ceux de l’application.

**Version du fichier principal**  
Saisissez `0.4.12` pour le ZIP [ChromeAxis-0.4.12-Cyberpunk-2.5.zip](../out/ChromeAxis-0.4.12-Cyberpunk-2.5.zip).

## English

**Mod name**  
Chrome Axis - Steering Wheel and Pedal Support

**Short summary**  
Analog steering, throttle and brake for Cyberpunk 2077. Calibrate USB wheels and pedals separately, save profiles, map accessories to keys, or try keyboard simulation without a wheel.

**Main file name**  
Chrome Axis for Cyberpunk 2077 2.5

**Main file description**  
Includes the RED4ext plugin, Chrome Axis calibration app and in-game status HUD. For Windows x64 and Cyberpunk 2077 2.5 (exe 3.0.88.46235). Read the setup steps before enabling USB input.

### Full description

Chrome Axis adds analog steering, throttle and brake to **Cyberpunk 2077 on Windows**. Connect your wheel and pedals through a wheelbase or select a separate DirectInput device for each axis. The included **Chrome Axis** app lets you calibrate your setup, see live input, set dead zones and save named profiles. You can keep the game's own controls for any axis.

This prototype targets **Cyberpunk 2077 2.5**. Hardware behavior depends on the axes and buttons your Windows drivers expose.

#### What you can do

- Calibrate a wheel and two pedals, including pedals that share an axis. If your driver offers separate pedal axes, select them to measure both pedals simultaneously.
- Save different setups as profiles. Each keeps its axis calibration, accessory bindings and force-feedback settings.
- Bind wheel buttons, D-pad directions, paddles, a shifter, a handbrake or an accessory axis to keyboard keys.
- Enable optional DirectInput force feedback on a compatible steering axis. Steering resistance varies with the vehicle, and collision jolts can be turned off separately.
- Try progressive steering, throttle and braking with the keyboard in **Simulation**, even without a wheel, pedals or a saved profile.

#### Requirements and compatibility

- **Windows x64 and Cyberpunk 2077 patch 2.5**, with `Cyberpunk2077.exe` file version **3.0.88.46235**. The plugin checks this version when it loads.
- **RED4ext 1.31.1**. [Nexus Mods](https://www.nexusmods.com/cyberpunk2077/mods/2380) · [GitHub release 1.31.1](https://github.com/wopss/RED4ext/releases/tag/v1.31.1)
- **redscript**, with **0.5.32** as the reference version for this release. [Nexus Mods](https://www.nexusmods.com/cyberpunk2077/mods/1511) · [GitHub release 0.5.32](https://github.com/jac3km4/redscript/releases/tag/v0.5.32)
- For physical controls, use a device whose driver exposes the axes or buttons through **DirectInput**. Check them in Windows Game Controllers (`joy.cpl`). Force feedback also requires a DirectInput actuator on the calibrated steering axis.

#### Install and set up a wheel

1. Close the game and install the requirements. In Vortex, import the Chrome Axis ZIP using **Install From File**, then enable and deploy it. For a manual install, extract the ZIP into the game folder so that `archive` and `red4ext` sit alongside `bin`.
2. Open `red4ext/plugins/chrome_axis/ChromeAxis.exe` from the **game folder**. The app is in French. In **Matériel USB**, click **Actualiser** and select a device for **Volant** (wheel), **Accélérateur** (throttle) and **Frein** (brake). Select **Commandes du jeu** to keep the game's controls for an axis.
3. Click **Calibrer les axes**. Capture the wheel at its centre and both limits, then each pedal at rest and fully pressed. Check the live values and adjust **Zone morte (%)** if a control moves at rest. Click **Enregistrer le profil** to save your setup. Use **Sélectionner un profil…** to switch to or create a named profile.
4. Click **Activer l’USB en jeu**. USB input starts disabled. Once you've saved and enabled it, you can close the app. Sit in the driver's seat with the game in the foreground, centre the wheel and release the pedals and accessories to arm them. The HUD shows **USB PRÊT** when your configured controls are armed and briefly shows **SIGNAL USB** when the mod applies input.

For force feedback, save the steering axis first, then open **Retour de force…** and enable it for the current profile. Start at low intensity. The setting ranges from **1% to 35%** of nominal DirectInput force. The panel checks whether the driver exposes the required actuator and offers an invert option if the wheel pulls away from centre.

To return to the game's regular controls, open the app and select **Désactiver l’USB en jeu**.

#### Try it without hardware

Open **Simulation**, then click **Activer dans le jeu** and **leave Chrome Axis running**. In the game, sit in the driver's seat and release the simulation keys once before testing. Hold **Numpad 4 / 6** to steer left or right, **Numpad 8** to accelerate and **Numpad 2** to brake, with Num Lock on. **F6 / F7 / F8 / F10** are the alternatives in the same order. The HUD displays **SIMULATION** when the game receives the app's signal. To finish, click **Arrêter la simulation** or close the app.

#### Paddles, shifters and accessories

Open **Matériel USB → Accessoires…**, then choose the wheelbase or standalone USB device that exposes the input. In **Boîte / palettes…**, capture the up/down positions for a sequential shifter, or individual positions on a 6-, 7- or 8-speed H-pattern shifter, including reverse. Assign the keys, click **Appliquer** in the wizard, then **Enregistrer** in the accessory panel. USB input must be enabled for bindings to work in the game.

Chrome Axis maps accessory inputs to keys. For manual shifting, set the same keys in a separate transmission mod. An H-pattern shifter needs a transmission mod that accepts a separate key for each gear. For sequential shifting, **Numpad + / −** are suggested bindings to try with Manual Transmission V10 4.3. That combination still needs an in-game test. [Manual Transmission V10](https://www.nexusmods.com/cyberpunk2077/mods/28342)

#### Troubleshooting and updates

- **The HUD stays on USB EN ATTENTE.** Keep the game in the foreground, centre the wheel, release the pedals and check that each configured input responds in `joy.cpl`. For a device that no longer exposes the saved axis, check its driver settings and recalibrate.
- **The gauges move in the app but the car does not respond.** Check the newest `red4ext/logs/chromeaxis-*.log`. `Chrome Axis USB configuration directory` shows which profile folder the plugin reads. Compare it with **Dossier des profils** in the app. `USB axes` reports the state of each axis. If the mod log is missing, check `red4ext-*.log` for plugin loading or script compilation errors.
- **Windows reports a missing `MSVCP140.dll` or `VCRUNTIME140.dll`.** Install the Microsoft Visual C++ v14 x64 Redistributable. [Microsoft download](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist?view=msvc-170#latest-supported-redistributable-version)
- **Updating or uninstalling.** Close the game and app, and back up the entire `red4ext/plugins/chrome_axis/Profil` folder. For an update, disable and deploy the removal of the old release in Vortex, deploy the new release, restore `Profil`, then click **Recharger**. To uninstall, remove the mod in Vortex and deploy the change. For a manual install, remove the Chrome Axis plugin folder and `archive/pc/mod/chrome_axis_hud.archive`.

When reporting a bug, include your wheel and pedal models, how they're connected, your game and framework versions, steps to reproduce and the latest relevant log excerpt.

#### Credits and AI disclosure

The analog vehicle hook and the Cyberpunk 2.5 field layout were developed with reference to **Direct Wheel 3.4.1**. Thanks to Grant Perdue (clevergrant), SERG (natpoh) and DesignWithTommy. [Direct Wheel 3.4.1](https://github.com/DesignWithTommy/cp2077-wheel-mod/releases/tag/v3.4.1) · [Grant Perdue’s original project](https://github.com/clevergrant/cp2077-wheel-mod) · [SERG’s Moza and Direct Drive Wheel Support on Nexus Mods](https://www.nexusmods.com/cyberpunk2077/mods/29246)

Thanks to Octavian Dima and the RED4ext contributors, jac3km4 and the redscript contributors, and the Barlow Project Authors. The release includes `THIRD_PARTY_NOTICES.md` and `licenses/Barlow-OFL.txt`. [RED4ext SDK](https://github.com/wopss/RED4ext.SDK/commit/5246e659d783e5834cc08a90e56c8932c9ed3449) · [redscript](https://github.com/jac3km4/redscript) · [Barlow license](https://github.com/google/fonts/blob/main/ofl/barlowcondensed/OFL.txt)

OpenAI tools assisted with the page copy and promotional artwork. OpenAI ImageGen was also used to remove the background from the in-game HUD logo (`chrome_axis/hud/logo.xbm` in `chrome_axis_hud.archive`).

## Français

**Nom du mod**  
Chrome Axis - Volant et pédales analogiques

**Résumé court**  
Direction, accélérateur et frein analogiques pour Cyberpunk 2077. Calibrez vos périphériques USB, enregistrez vos profils, attribuez des touches aux accessoires ou testez la conduite sans volant.

**Nom du fichier principal**  
Chrome Axis pour Cyberpunk 2077 2.5

**Description du fichier principal**  
Plugin RED4ext, application de calibration Chrome Axis et indicateur d’état en jeu. Pour Windows x64 et Cyberpunk 2077 2.5 (exécutable 3.0.88.46235).

### Description complète

Chrome Axis ajoute la direction, l’accélérateur et le frein analogiques à **Cyberpunk 2077 sur Windows**. Branchez le volant et le pédalier sur une même base ou sélectionnez un périphérique DirectInput différent pour chaque axe. L’application **Chrome Axis** permet de calibrer les commandes, de visualiser les valeurs en direct, d’ajuster les zones mortes et d’enregistrer des profils nommés. Vous pouvez conserver les commandes du jeu pour chaque axe de votre choix.

Ce prototype cible **Cyberpunk 2077 2.5**. La prise en charge du matériel dépend des axes et boutons exposés par vos pilotes Windows.

#### Fonctionnalités

- Calibrez le volant et les deux pédales, y compris si les pédales partagent un axe. Choisissez des axes séparés dans le pilote, si possible, pour mesurer les deux pressions simultanément.
- Enregistrez vos configurations sous différents profils. Chacun garde sa calibration, ses affectations d’accessoires et ses réglages de retour de force.
- Associez des touches aux boutons du volant, aux directions de croix, aux palettes, à un levier de vitesses, à un frein à main ou à un axe d’accessoire.
- Activez le retour de force DirectInput sur un axe de direction compatible. L’effort varie selon le véhicule et les secousses de collision peuvent être désactivées séparément.
- Essayez la direction, l’accélération et le freinage progressifs au clavier dans **Simulation**, même sans volant, pédalier ni profil enregistré.

#### Prérequis et compatibilité

- **Windows x64 et Cyberpunk 2077 2.5**, avec `Cyberpunk2077.exe` en version **3.0.88.46235**. Le plugin vérifie cette version au chargement.
- **RED4ext 1.31.1**. [Nexus Mods](https://www.nexusmods.com/cyberpunk2077/mods/2380) · [Version 1.31.1 sur GitHub](https://github.com/wopss/RED4ext/releases/tag/v1.31.1)
- **redscript**, avec **0.5.32** comme version de référence pour cette version du mod. [Nexus Mods](https://www.nexusmods.com/cyberpunk2077/mods/1511) · [Version 0.5.32 sur GitHub](https://github.com/jac3km4/redscript/releases/tag/v0.5.32)
- Pour utiliser du matériel, le pilote doit exposer ses axes et boutons via **DirectInput**. Vérifiez-les dans les contrôleurs de jeu Windows (`joy.cpl`). Le retour de force nécessite aussi un actionneur DirectInput sur l’axe de direction calibré.

#### Installer et configurer le volant

1. Fermez le jeu et installez les prérequis. Dans Vortex, importez le ZIP avec **Installer depuis un fichier**, puis activez et déployez le mod. Pour une installation manuelle, extrayez le ZIP à la racine du jeu, avec `archive` et `red4ext` au même niveau que `bin`.
2. Ouvrez `red4ext/plugins/chrome_axis/ChromeAxis.exe` depuis le **dossier du jeu**. Dans **Matériel USB**, cliquez sur **Actualiser**, puis choisissez le périphérique pour le **Volant**, l’**Accélérateur** et le **Frein**. Pour conserver les commandes habituelles sur un axe, sélectionnez **Commandes du jeu**.
3. Cliquez sur **Calibrer les axes**. Capturez le centre et les deux butées du volant, puis le repos et la pleine course de chaque pédale. Vérifiez les jauges et ajustez **Zone morte (%)** si une commande bouge au repos. Cliquez sur **Enregistrer le profil**. Le bouton **Sélectionner un profil…** permet de choisir ou de créer un profil nommé.
4. Cliquez sur **Activer l’USB en jeu**. L’USB est désactivé par défaut. Après l’enregistrement et l’activation, vous pouvez fermer l’application. Installez V au siège conducteur, laissez le jeu au premier plan, centrez le volant et relâchez les pédales et les accessoires. Le HUD affiche **USB PRÊT** quand les commandes sont armées, puis brièvement **SIGNAL USB** à l’envoi d’une commande.

Pour régler le retour de force, enregistrez d’abord l’axe de direction, ouvrez **Retour de force…**, puis activez l’effet pour le profil courant. Commencez à faible intensité. Le réglage va de **1 % à 35 %** de la force nominale DirectInput. Le panneau vérifie si le pilote expose un actionneur et permet d’inverser l’effet si le volant pousse vers la butée.

Pour reprendre les commandes habituelles du jeu, ouvrez l’application et cliquez sur **Désactiver l’USB en jeu**.

#### Essayer sans matériel

Ouvrez **Simulation**, cliquez sur **Activer dans le jeu** et **laissez Chrome Axis ouvert**. Installez V au siège conducteur et relâchez les touches de simulation avant l’essai. Avec Verr. num. activé, maintenez **Pavé 4 / 6** pour braquer à gauche ou à droite, **Pavé 8** pour accélérer et **Pavé 2** pour freiner. Vous pouvez aussi utiliser **F6 / F7 / F8 / F10** dans le même ordre. Le HUD affiche **SIMULATION** quand le jeu reçoit le signal de l’application. Pour terminer, cliquez sur **Arrêter la simulation** ou fermez l’application.

#### Palettes, leviers et accessoires

Ouvrez **Matériel USB → Accessoires…** et sélectionnez la base ou le périphérique USB qui expose la commande. Dans **Boîte / palettes…**, capturez les commandes de montée et de descente pour un levier séquentiel, ou chaque position d’une grille en H de 6, 7 ou 8 rapports, marche arrière comprise. Attribuez les touches, cliquez sur **Appliquer** dans l’assistant puis sur **Enregistrer** dans le panneau des accessoires. L’USB en jeu doit être activé pour utiliser les affectations.

Chrome Axis transforme les entrées d’accessoires en touches. Pour changer de rapport, configurez les mêmes touches dans un mod de transmission. Une boîte en H nécessite une touche par rapport dans ce mod. En séquentiel, **Pavé + / −** sont les touches proposées à essayer avec Manual Transmission V10 4.3. Cette combinaison reste à tester en jeu. [Manual Transmission V10](https://www.nexusmods.com/cyberpunk2077/mods/28342)

#### Dépannage et mises à jour

- **Le HUD reste sur USB EN ATTENTE.** Gardez le jeu au premier plan, centrez le volant, relâchez les pédales et vérifiez chaque commande configurée dans `joy.cpl`. Si le pilote ne présente plus l’axe enregistré, vérifiez ses réglages et refaites la calibration.
- **Les jauges réagissent dans l’application mais le véhicule ne bouge pas.** Consultez le dernier `red4ext/logs/chromeaxis-*.log`. `Chrome Axis USB configuration directory` indique le dossier lu par le plugin. Comparez-le à **Dossier des profils** dans l’application. `USB axes` indique l’état de chaque axe. Si le journal du mod est absent, consultez `red4ext-*.log` pour vérifier le chargement du plugin et la compilation des scripts.
- **Windows signale `MSVCP140.dll` ou `VCRUNTIME140.dll` manquant.** Installez le redistribuable Microsoft Visual C++ v14 x64. [Téléchargement Microsoft](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist?view=msvc-170#latest-supported-redistributable-version)
- **Mettre à jour ou désinstaller.** Fermez le jeu et l’application, puis sauvegardez le dossier `red4ext/plugins/chrome_axis/Profil` en entier. Pour une mise à jour, désactivez l’ancienne version dans Vortex et déployez le retrait, déployez la nouvelle version, restaurez `Profil`, puis cliquez sur **Recharger**. Pour désinstaller, retirez le mod dans Vortex et déployez le changement. En installation manuelle, retirez le dossier du plugin Chrome Axis et `archive/pc/mod/chrome_axis_hud.archive`.

Pour signaler un problème, indiquez les modèles de volant et de pédales, leur branchement, les versions du jeu et des prérequis, les étapes pour reproduire le problème et un extrait du dernier journal pertinent.

#### Crédits et utilisation de l’IA

Le branchement sur les commandes analogiques du jeu et la structure des champs de Cyberpunk 2.5 s’appuient sur **Direct Wheel 3.4.1** comme référence. Merci à Grant Perdue (clevergrant), SERG (natpoh) et DesignWithTommy. [Direct Wheel 3.4.1](https://github.com/DesignWithTommy/cp2077-wheel-mod/releases/tag/v3.4.1) · [Projet d’origine de Grant Perdue](https://github.com/clevergrant/cp2077-wheel-mod) · [Moza and Direct Drive Wheel Support de SERG sur Nexus Mods](https://www.nexusmods.com/cyberpunk2077/mods/29246)

Merci à Octavian Dima et aux contributeurs de RED4ext, à jac3km4 et aux contributeurs de redscript, et aux auteurs de Barlow. Le ZIP inclut `THIRD_PARTY_NOTICES.md` et `licenses/Barlow-OFL.txt`. [SDK RED4ext](https://github.com/wopss/RED4ext.SDK/commit/5246e659d783e5834cc08a90e56c8932c9ed3449) · [redscript](https://github.com/jac3km4/redscript) · [Licence Barlow](https://github.com/google/fonts/blob/main/ofl/barlowcondensed/OFL.txt)

Des outils OpenAI ont aidé à rédiger cette page et à créer les visuels de présentation. OpenAI ImageGen a également servi à retirer le fond du logo affiché dans le HUD (`chrome_axis/hud/logo.xbm` dans `chrome_axis_hud.archive`).
