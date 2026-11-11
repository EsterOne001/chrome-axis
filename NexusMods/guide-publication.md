# Publier Chrome Axis sur Nexus Mods

Le texte de la page est dans [fiche-nexus.md](fiche-nexus.md), en anglais et en français. Les légendes ci-dessous sont à copier dans l’onglet **Images**. L’anglais est la version principale proposée pour Nexus.

## Fichier du mod

| Champ | Valeur proposée |
| --- | --- |
| Jeu | Cyberpunk 2077 |
| Nom | Chrome Axis - Steering Wheel and Pedal Support |
| Catégorie proposée | Gameplay |
| Version du ZIP | 0.4.12 |
| Compatibilité ciblée | Windows · Cyberpunk 2077 2.5 · exécutable 3.0.88.46235 |
| Fichier principal | `out/ChromeAxis-0.4.12-Cyberpunk-2.5.zip` dans l’espace de travail de publication |
| Requirements | [RED4ext 1.31.1](https://www.nexusmods.com/cyberpunk2077/mods/2380?tab=files) et [redscript 0.5.32](https://www.nexusmods.com/cyberpunk2077/mods/1511?tab=files) |

Le ZIP contient `ChromeAxis.dll`, `ChromeAxis.exe`, le script, l’archive du HUD et les notices. Pour préparer un nouveau ZIP, suis les étapes du [guide de compilation](../README.md#compilation), puis calcule le SHA-256 du fichier final avec `Get-FileHash`.

## Visuels et légendes

Choisis le paysage comme image principale et contrôle son aperçu dans le formulaire. Le carré est une variante pour les recadrages serrés. Dans la galerie, place le guide de démarrage, la calibration et la simulation en premier.

| Fichier | Format et placement | Légende EN | Légende FR |
| --- | --- | --- | --- |
| [01-couverture-nexus.jpg](images/01-couverture-nexus.jpg) | 1600 × 900 · image principale | Chrome Axis brings analog steering, throttle and brake to Cyberpunk 2077 on PC. Promotional artwork. | Chrome Axis ajoute la direction, l’accélérateur et le frein analogiques à Cyberpunk 2077 sur PC. Illustration promotionnelle. |
| [02-couverture-carree.jpg](images/02-couverture-carree.jpg) | 960 × 960 · variante carrée | Chrome Axis logo and title. | Logo et titre de Chrome Axis. |
| [03-premiere-conduite.jpg](images/03-premiere-conduite.jpg) | 1600 × 900 · début de galerie ou section Configuration | Calibrate your USB controls for your first drive or try the keyboard simulation. | Calibrez les commandes USB pour votre premier trajet ou essayez la simulation clavier. |
| [04-calibration.png](images/04-calibration.png) | 878 × 681 · galerie | USB Hardware tab. Select a device for each axis, adjust dead zones and save the profile. Shown before a device is connected. | Onglet Matériel USB. Choisissez un périphérique par axe, ajustez les zones mortes et enregistrez le profil. Vue avant le branchement du matériel. |
| [05-simulation.png](images/05-simulation.png) | 878 × 681 · galerie | Keyboard Simulation tab in preview mode. Use the numpad or F6, F7, F8 and F10 to test progressive inputs. | Onglet Simulation en mode aperçu. Testez les entrées progressives au pavé numérique ou avec F6, F7, F8 et F10. |
| [06-accessoires.png](images/06-accessoires.png) | 716 × 665 · galerie | Assign a DirectInput button, D-pad direction or axis to a key. | Associez un bouton, une direction de croix ou un axe DirectInput à une touche. |
| [07-clavier.png](images/07-clavier.png) | 737 × 470 · galerie | Visual key picker. The number row and numpad are mapped separately. | Sélecteur de touches visuel. La rangée de chiffres et le pavé numérique sont distincts. |
| [08-profils.png](images/08-profils.png) | 655 × 435 · galerie | Load the default profile or create a named copy of the active profile. | Chargez le profil par défaut ou créez une copie nommée du profil actif. |
| [09-boite-palettes.png](images/09-boite-palettes.png) | 716 × 665 · galerie | Shifter and paddle setup, shown in six-speed H-pattern mode with reverse. An H-pattern setup requires a transmission mod with per-gear key bindings. | Assistant de boîte et palettes, ici en grille H à six rapports et marche arrière. La grille H nécessite un mod de transmission avec une touche par rapport. |
| [10-retour-de-force.png](images/10-retour-de-force.png) | 716 × 474 · galerie | Force feedback settings before steering calibration. A compatible DirectInput actuator is required. | Réglages du retour de force avant calibration de la direction. Un actionneur DirectInput compatible est nécessaire. |
| [chrome-axis-hud-transparent.png](logos/chrome-axis-hud-transparent.png) | 128 × 128 · logo transparent d’origine | Chrome Axis HUD logo. | Logo de Chrome Axis dans le HUD. |
| [chrome-axis-carre.png](logos/chrome-axis-carre.png) | 960 × 960 · version PNG du visuel carré | Chrome Axis square artwork. | Visuel carré Chrome Axis. |

Les sept captures ont été vérifiées avec l’exécutable 0.4.11 du ZIP. Elles montrent l’interface française avec un profil de démonstration vierge et sans matériel connecté. « Conduite libre » est un exemple saisi dans le dialogue des profils.

## Capture en jeu à ajouter

1. Avec le mod déployé, active **Simulation → Activer dans le jeu** dans Chrome Axis et laisse l’application ouverte.
2. Dans Cyberpunk 2077, installe V au siège conducteur avec le jeu au premier plan. Relâche les touches pour armer les axes. Prends une capture dès que le logo et l’état **SIMULATION** sont visibles dans le HUD.
3. Si tu as un volant, refais la prise avec un profil enregistré et l’USB en jeu activé. Centre le volant et relâche les pédales. Capture **USB PRÊT**, ou **SIGNAL USB** pendant une entrée. Garde la zone du HUD dans le recadrage.

Légende EN pour la première prise — “Chrome Axis HUD in the driver’s seat with keyboard simulation active.” Légende FR — « HUD de Chrome Axis au siège conducteur avec la simulation clavier active. »

L’[audit de la 0.4.10](../docs/audit-0.4.10.md) liste les essais matériels qui restent à faire en jeu.

## Crédits et champs de publication

- Les crédits de la fiche reprennent [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md). Garde-les dans la description et conserve la notice dans le ZIP. Pour le champ relatif aux forks et variantes, appuie-toi sur la licence amont et les permissions de publication applicables au projet. [Règles de soumission Nexus Mods](https://help.nexusmods.com/article/28-file-) · [Règles Nexus sur les forks et variantes](https://help.nexusmods.com/article/180-ai-policy-changes-2026#Forks-and-Parallel-Versions)
- Ajoute **AI Media** pour la couverture générée et le texte de la page rédigé avec Codex. Le logo du HUD a également été détouré avec ImageGen dans le projet. Renseigne la part éventuelle d’IA dans le reste du mod selon son historique de développement. [Règles Nexus sur les contenus générés ou assistés](https://help.nexusmods.com/article/180-ai-policy-changes-2026)

## Sources des visuels

L’illustration seule est dans [sources/illustration-volant.png](sources/illustration-volant.png). Elle a été produite avec l’outil ImageGen intégré. La composition de la couverture et du guide reprend le [logo du HUD](../assets/hud/logo.png), les polices Barlow fournies avec le projet, le jaune `#FCEE0A` et le cyan `#00F0FF`. Les textes des visuels ont été composés après la génération.

<details>
<summary>Consigne ImageGen</summary>

> Use case: ads-marketing. Asset type: text-free background for the Nexus Mods hero image of Chrome Axis, a Cyberpunk 2077 PC racing wheel input mod. Produce a wide, 16:9 premium industrial automotive key art image. A single realistic direct-drive sim racing steering wheel, machined black metal, textured grips and sculpted three-spoke brushed chrome, dramatically close in three-quarter angle on the RIGHT half of the frame. A crisp acidic yellow rim light and a restrained electric cyan secondary edge light describe its silhouette. Black graphite studio background, faint receding diagonal road-light streaks low on the right, extremely subtle film grain, industrial editorial photography, hard shadows, convincing materials. Leave the ENTIRE LEFT 49 percent flat very dark charcoal and deliberately clean for typography to be added later. Keep recognizable rim and hub fully inside central 80% of image height. One wheel only, one coherent rim, avoid extra controls floating in space. Generic hardware, no brand emblems, no logos, no text, no lettering, no numbers, no HUD, no interface, no game screenshot, no people. Full bleed horizontal composition.

</details>
