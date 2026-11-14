# Historique de Chrome Axis

Chrome Axis ajoute la conduite analogique à Cyberpunk 2077 avec un volant et des pédales DirectInput. Le projet a commencé sous le nom **Direct Wheel V2**, à partir de l’étude du mod tiers Direct Wheel 3.4.1. L’[archive de référence](references/DirectWheel-3.4.1.zip) est conservée localement dans `references/`.

Les entrées sont classées de la plus récente à la plus ancienne. Les dates indiquent la préparation des paquets ou des prototypes, en heure de Bruxelles. L’historique initial couvre 36 versions de travail, dont 26 ZIP de distribution conservés et vérifiés par CRC. Les premiers prototypes ont été reconstitués à partir des comptes rendus de développement, de fichiers sources historiques et des README embarqués. La présence d’un ZIP local atteste sa préparation pour Vortex. Les essais en jeu sont précisés lorsqu’ils sont documentés.

Les ZIP de distribution se trouvent dans [`out/`](out/). Ce dossier est local et ignoré par Git. Le [guide de validation](docs/manual-validation.md#français) décrit les essais matériels, en jeu et de coexistence avec les autres mods.

## Chrome Axis 0.4.29 · 14 novembre 2026

- **Aperçu des accessoires hors jeu.** Le panneau **Accessoires…** montre en direct si la touche liée à un bouton, une direction de croix ou un axe serait maintenue ou relâchée. Pour un axe, la jauge affiche la course et les deux seuils. L’aperçu efface la dernière lecture lorsque le périphérique devient indisponible.
- **Mesure facultative dans Diagnostic.** Une capture en conduite USB relève le nombre de mesures, les durées moyennes et maximales des lectures USB et des appels de retour de force, ainsi que l’âge des commandes lorsqu’elles sont écrites dans le jeu. Les lectures expirées et interruptions temporaires sont comptées. Les résultats restent disponibles après Alt-Tab et figurent dans **Copier le diagnostic**. La capture est limitée à cinq minutes et demande des versions compatibles de l’application et de la DLL. La mesure des FPS fait partie du protocole d’essai en jeu.
- **Traitement des commandes.** L’application et le plugin partagent davantage de logique de normalisation des axes et d’interprétation des croix directionnelles. Le diagnostic et la simulation reconnaissent la même installation de Cyberpunk lorsqu’elle est accessible par des chemins différents.
- **Paquet Vortex.** CPack utilise la version de CMake pour nommer le ZIP et place `archive` et `red4ext` à sa racine. La production du paquet demande une compilation avec le plugin activé.
- **Compatibilité documentée.** Windows x64, Cyberpunk 2077 2.5 (`3.0.88.46235`), RED4ext 1.31.1 et redscript 0.5.32.
- **Validation de la livraison.** Compilation Release x64, neuf tests CTest réussis, compilation REDscript isolée, essais FR/EN avec entrées simulées et vérification des dix fichiers du ZIP. L’EXE et la DLL portent tous deux la version 0.4.29. Les essais physiques des nouveautés 0.4.25 à 0.4.29 et la mesure des temps de trame restaient à confirmer lors de la préparation du paquet.

[ZIP Vortex](out/ChromeAxis-0.4.29-Cyberpunk-2.5.zip) · [Diff depuis la 0.4.28](https://github.com/EsterOne001/chrome-axis/compare/844464c0ae3a878843a0fbead007d5272316bebd...51f8b4f48aca9506b747f4890f3ef490b0abb71f)

SHA-256 `5B4A7478800A52030D3EC9CAB7431567CA5165D9544CC0126E49E0A3074B3F1D`

## Chrome Axis 0.4.28 · 14 novembre 2026

- **Reprise après déconnexion.** Pendant la calibration, reconnecter le même appareil permet de recommencer la position interrompue et de garder les captures déjà terminées. Le lecteur USB distingue une interruption de lecture d’un périphérique indisponible et peut réarmer les commandes après une interruption prolongée.
- **Vérification guidée.** Chaque axe indique son propre état. Le frein affiche sa course brute et la sortie après réglage. Annuler la reprise d’une commande permet de retrouver ses mesures précédentes.
- **Diagnostic.** Le panneau distingue une interruption USB temporaire d’une déconnexion, conserve le dernier essai en conduite et signale un écart de version entre l’application et le plugin.
- **Boîte et palettes.** Les nouvelles affectations conservent le nom du rapport ou de la palette quand on rouvre l’assistant ou qu’on change la touche. Les affectations des anciens profils restent lisibles.

[ZIP Vortex](out/ChromeAxis-0.4.28-Cyberpunk-2.5.zip) · [Commit](https://github.com/EsterOne001/chrome-axis/commit/844464c0ae3a878843a0fbead007d5272316bebd)

## Chrome Axis 0.4.27 · 14 novembre 2026

- **HUD et diagnostic.** Le HUD affiche **USB PARTIEL** quand une commande atteint le jeu pendant que d’autres restent en attente. Le diagnostic associe la valeur calibrée et la commande demandée issues de la même mesure. La dernière valeur réellement écrite dans Cyberpunk est affichée séparément, avec son ancienneté.
- **Calibration.** Reprendre les mesures du même axe conserve sa zone morte personnalisée, y compris 0 %. L’assistant précise les cas liés aux pédales combinées. Le sélecteur d’affectation rappelle le rôle de **Verr. Num.** pour les touches du pavé numérique.
- **Profils.** Une fenêtre qui travaille sur d’anciens réglages demande de recharger le profil si une autre fenêtre a sauvegardé entre-temps. La protection couvre notamment la calibration, les accessoires et le retour de force.

[ZIP Vortex](out/ChromeAxis-0.4.27-Cyberpunk-2.5.zip) · [Commit](https://github.com/EsterOne001/chrome-axis/commit/964e25f271c087d5307d396dda655df8ef1d4bc3)

## Chrome Axis 0.4.26 · 14 novembre 2026

- **Frein.** Ajout de la progressivité et du réglage de la course nécessaire pour obtenir 100 % de freinage, de 50 à 100 % de la course calibrée. À progressivité nulle, la réponse reste linéaire. Le panneau permet de comparer la course et la réponse, avec un aperçu utilisable sans périphérique.
- **Accessoires analogiques.** Les seuils d’appui et de relâchement deviennent réglables séparément, avec 55 % et 35 % par défaut. Un axe de frein à main ou d’embrayage peut déclencher et relâcher une touche à ces seuils.
- **Direction.** La zone morte peut descendre à **0 %**. Le réarmement conserve une tolérance distincte de 0,5 % autour du neutre.
- **Interface.** L’activation USB passe dans le bandeau. La simulation clavier passe dans **Aide → Outils de test**, avec son activation en haut et le bouton **Retour** en bas. Le diagnostic reste accessible depuis le panneau de calibration. Plusieurs commits du même numéro ont précédé le ZIP final.

[ZIP Vortex](out/ChromeAxis-0.4.26-Cyberpunk-2.5.zip) · [Diff jusqu’au paquet final](https://github.com/EsterOne001/chrome-axis/compare/c7dab2d3f558dfd1f35346d61d9c33779c1d82c4...83169bdb2d4a380b29d2f29ed2a613b3e3363e4f)

## Chrome Axis 0.4.25 · 14 novembre 2026

- **Modifier une affectation.** Le bouton **Modifier la touche…** change la touche d’un accessoire, rapport ou palette déjà capturé. L’édition reste possible avec le périphérique déconnecté.
- **Remplacer un périphérique.** Jeu fermé, le profil peut être réassocié à un appareil compatible. L’application vérifie que le nouvel appareil expose les axes, boutons et directions de croix nécessaires, puis conserve les mesures, les zones mortes, les touches et les réglages de retour de force.

[ZIP Vortex](out/ChromeAxis-0.4.25-Cyberpunk-2.5.zip) · [Commit](https://github.com/EsterOne001/chrome-axis/commit/c7dab2d3f558dfd1f35346d61d9c33779c1d82c4)

## Chrome Axis 0.4.24 · 13 novembre 2026

- **Logo du HUD.** Corrige le carré opaque autour du logo de volant. La texture utilise un alpha prémultiplié, avec les canaux de couleur à zéro sur les pixels complètement transparents. Le logo s’affiche lorsque sa texture est prête.
- **Essai physique documenté.** Le créateur du mod et un ami ont confirmé la conduite, la synchronisation et la disparition du carré. Le créateur a testé le retour après Alt-Tab et son ami a testé le débranchement et le rebranchement.

[ZIP Vortex](out/ChromeAxis-0.4.24-Cyberpunk-2.5.zip) · [Commit](https://github.com/EsterOne001/chrome-axis/commit/7554b5f15b5688cc4b36ff7b8f07b5aea1a32c51)

## Chrome Axis 0.4.23 · 13 novembre 2026

- **Simulation.** Le signal destiné au jeu est rattaché à l’installation où le mod est déployé. Une copie autonome peut toujours afficher l’aperçu local. L’application indique **Simulation demandée** en attendant que le plugin de l’installation confirme l’activation.
- **Restauration.** Un dossier `Profil` ordinaire et vide est refusé. Les anciennes copies manuelles qui contiennent des réglages et les sauvegardes volontairement vides créées par l’application sont reconnues.
- **Installation.** Le diagnostic indique quelle DLL ou quel script REDscript manque et affiche la version de Cyberpunk lorsqu’elle est disponible.

[ZIP Vortex](out/ChromeAxis-0.4.23-Cyberpunk-2.5.zip) · [Commit](https://github.com/EsterOne001/chrome-axis/commit/1080023257264964ec13f92037baeed2f22ec56d)

## Chrome Axis 0.4.22 · 13 novembre 2026

- **Aide.** Le menu **Infos sur le mod (Nexus Mods)** ouvre la fiche du mod dans le navigateur pour retrouver les fichiers et le suivi des bugs.
- **Audit de la livraison.** L’audit a vérifié la compilation Release x64, les neuf tests CTest, la compilation REDscript isolée, les ressources du HUD et le manifeste du ZIP. Les cas de restauration et de simulation relevés par l’audit ont motivé les corrections de la 0.4.23. Les essais en jeu et la mesure des temps de trame étaient à poursuivre.

[ZIP Vortex](out/ChromeAxis-0.4.22-Cyberpunk-2.5.zip) · [Rapport d’audit local](out/Chrome-Axis-0.4.22-rapport-audit-FR.md) · [Commit](https://github.com/EsterOne001/chrome-axis/commit/d76ca3cd6db1c9d7ff416baa591234f6f4240e96)

## Chrome Axis 0.4.21 · 13 novembre 2026

- **Copier le diagnostic.** Le rapport local inclut le dernier état en conduite après Alt-Tab, jusqu’à huit transitions récentes, les modèles de périphériques et les versions installées. Pour chaque axe, il distingue la lecture calibrée, la commande demandée et la dernière valeur écrite dans Cyberpunk. Le texte omet les chemins d’installation et les identifiants de périphériques.

[ZIP Vortex](out/ChromeAxis-0.4.21-Cyberpunk-2.5.zip) · [Commit](https://github.com/EsterOne001/chrome-axis/commit/05be9c08af2eb99d994a0a4a4436cb4e80ba984c)

## Chrome Axis 0.4.20 · 13 novembre 2026

- **Vérification de la calibration.** Des jauges en direct permettent de vérifier les commandes et de reprendre un seul axe en conservant les autres mesures.
- **Sauvegarder et restaurer.** La sauvegarde emporte le profil par défaut, les profils nommés, le profil actif, les mesures et zones mortes, les affectations, le retour de force, l’activation USB et la langue. La restauration garde une copie de récupération. La langue de l’application déployée est désormais conservée dans le dossier `Profil` de cette installation.

[ZIP Vortex](out/ChromeAxis-0.4.20-Cyberpunk-2.5.zip) · [Commit](https://github.com/EsterOne001/chrome-axis/commit/0bf5e43a8350363d356fac22ef60dacb9697ed19)

## Chrome Axis 0.4.19 · 13 novembre 2026

- **Calibration animée.** L’assistant montre le braquage du volant et les gestes d’appui ou de relâchement des pédales. Le frein et l’accélérateur sont dessinés et nommés séparément.

[ZIP Vortex](out/ChromeAxis-0.4.19-Cyberpunk-2.5.zip) · [Commit](https://github.com/EsterOne001/chrome-axis/commit/0a8ccc1de2d3a44bc90dd57f2bb71d32e703844f)

## Chrome Axis 0.4.18 · 13 novembre 2026

- **Reprise de position.** Le bouton **Précédent** refait la position choisie en conservant les captures antérieures. Une page de vérification apparaît avant de terminer.
- **Illustrations.** Les images du volant et des pédales montrent les positions demandées pendant la capture.

[ZIP Vortex](out/ChromeAxis-0.4.18-Cyberpunk-2.5.zip) · [Commit](https://github.com/EsterOne001/chrome-axis/commit/a4fb0eea1a7a7a51bcaa05d234554b592bab52e9)

## Chrome Axis 0.4.17 · 13 novembre 2026

- **Calibration pas à pas.** L’assistant nomme le périphérique, illustre la position à prendre et propose une capture manuelle une fois la commande stable. Il permet de choisir l’axe si plusieurs entrées réagissent.

[ZIP Vortex](out/ChromeAxis-0.4.17-Cyberpunk-2.5.zip) · [Commit](https://github.com/EsterOne001/chrome-axis/commit/13f7016f3816c3ce56e220cf0aae2b34e02e2fc6)

## Chrome Axis 0.4.16 · 13 novembre 2026

- **Diagnostic en jeu.** L’application lit les états publiés par le plugin. Pour chaque axe, elle distingue un périphérique absent, un axe introuvable et l’attente du retour au neutre. Elle affiche la dernière valeur appliquée et son ancienneté, la dernière touche acceptée par Windows et l’état du retour de force.
- **Recalibration ciblée.** Il devient possible de remesurer une commande ou les axes d’un périphérique en gardant les autres réglages et les affectations. Une suite de tests dédiée au diagnostic s’ajoute à CTest.

[ZIP Vortex](out/ChromeAxis-0.4.16-Cyberpunk-2.5.zip) · [Commit](https://github.com/EsterOne001/chrome-axis/commit/3a94183eed9d80ee2d91844f4ae4692b68a5660a)

## Chrome Axis 0.4.15 · 12 novembre 2026

- **Sélecteur de langue.** Les drapeaux français et britannique sont réunis dans un carré à séparation diagonale. La moitié choisie s’illumine tout de suite. Le redémarrage applique la langue.
- **Livraison.** Le code correspondant est balisé `v0.4.15`. La préparation du paquet a consigné huit tests automatisés, la compilation REDscript et la vérification du changement de langue dans les deux sens.
- **Travaux après ce ZIP.** Le dépôt a ensuite été organisé en `src/core`, `src/app` et `src/plugin`, avec une configuration CTest séparée et un protocole d’essais manuels. Le commit ajoute aussi les sources des tutoriels d’installation et un script de publication Nexus. Il conserve le numéro 0.4.15. [Commit de réorganisation](https://github.com/EsterOne001/chrome-axis/commit/e8d0401f3b1b0a9bfd644ef6bfc5399c6b06e058)

[ZIP Vortex](out/ChromeAxis-0.4.15-Cyberpunk-2.5.zip) · [Tag v0.4.15](https://github.com/EsterOne001/chrome-axis/tree/v0.4.15)

## Chrome Axis 0.4.14 · 12 novembre 2026

- **Raccourcis de langue.** Les drapeaux français et britannique apparaissent à droite de **Simulation**. Le choix actif est entouré de jaune. Le menu **Aide → Langue** conserve aussi le réglage automatique selon Windows.

[ZIP Vortex](out/ChromeAxis-0.4.14-Cyberpunk-2.5.zip)

## Chrome Axis 0.4.13 · 12 novembre 2026

- **Interface bilingue.** L’application propose le français, l’anglais ou la langue de Windows, avec application du réglage au redémarrage. Le HUD suit la langue de Cyberpunk. Les journaux techniques du plugin restent en anglais.
- **Distribution.** Le paquet inclut désormais la documentation française et anglaise, une notice `Profil` bilingue et le fichier de licence MIT.

[ZIP Vortex](out/ChromeAxis-0.4.13-Cyberpunk-2.5.zip)

## Chrome Axis 0.4.12 · 11 novembre 2026

- **Couleurs du HUD.** L’icône et le texte prennent ensemble la couleur de l’état. Gris clair pour le chargement, ambre pour l’attente, cyan pour l’USB prêt, vert pour le signal envoyé et violet pour la simulation.
- **Code source.** Première publication des sources de Chrome Axis sur GitHub sous licence MIT.

[ZIP Vortex](out/ChromeAxis-0.4.12-Cyberpunk-2.5.zip) · [Premier commit public](https://github.com/EsterOne001/chrome-axis/commit/0f9eb565fe6a3bf159654a4f4289877e2c203394)

## Chrome Axis 0.4.11 · 11 novembre 2026

- **Nom de l’application.** Chrome Axis 0 devient **Chrome Axis** dans l’interface et les métadonnées Windows. L’exécutable `ChromeAxis0.exe` devient `ChromeAxis.exe`.

[ZIP Vortex](out/ChromeAxis-0.4.11-Cyberpunk-2.5.zip)

## Chrome Axis 0.4.10 · 11 novembre 2026

- **Profils et interface.** Le lancement depuis une copie temporaire du ZIP bloque l’enregistrement des réglages. En arrière-plan, l’application interrompt l’aperçu USB et les captures. Les jauges se redessinent lorsqu’un mouvement à l’écran ou un état change.
- **Commandes.** Le plugin écarte les entrées issues d’un ancien contexte conducteur. Un bouton ou une direction de croix absente reste désarmée après reconnexion. La reprise de la simulation exige le relâchement des touches et évite de réactiver brièvement le volant à la fermeture de l’ancienne instance.
- **Retour de force.** Le renouvellement d’un effet identique passe d’environ 20 à 60 ms au plus tôt. Chaque effet est limité à 100 ms. Changer le volant ou l’axe de direction demande de réactiver le retour de force pour ce profil.
- **Validation consignée.** Huit tests CTest en Release, compilation REDscript contre le cache de Cyberpunk 2.5 et contrôle du ZIP. L’audit réserve la cadence réellement ressentie au volant et les performances aux essais en jeu.

[ZIP Vortex](out/ChromeAxis-0.4.10-Cyberpunk-2.5.zip) · [Audit détaillé](docs/archives/audit-0.4.10.md)

## Chrome Axis 0.4.9 · 11 novembre 2026

- **Clavier visuel.** Dans **Accessoires…** et **Boîte / palettes…**, la touche choisie est mise en évidence. La rangée de chiffres et le pavé numérique sont distincts. Fermer le clavier conserve la touche déjà affectée.
- **HUD.** Le panneau de fond disparaît et le texte reçoit une ombre.

[ZIP Vortex](out/ChromeAxis-0.4.9-Cyberpunk-2.5.zip)

## Chrome Axis 0.4.8 · 11 novembre 2026

- **Profils nommés.** Le profil par défaut reste accessible et peut servir de base à une copie nommée. Chaque profil garde ses axes, ses accessoires et son retour de force. Le jeu reprend la même sélection au prochain contrôle des réglages. L’activation USB reste commune aux profils.
- **Retour de force DirectInput.** Ajout d’un effet réglable sur les volants compatibles, tenant compte de la vitesse, de la masse, du mouvement latéral et des chocs du véhicule. Un profil peut régler l’intensité, inverser le sens ou couper les chocs. L’effet se coupe à la sortie du contexte de conduite ou lorsque le volant cesse de répondre.

[ZIP Vortex](out/ChromeAxis-0.4.8-Cyberpunk-2.5.zip)

## Chrome Axis 0.4.7 · 11 novembre 2026

- **HUD de conduite.** Le logo et l’état du mod apparaissent au volant. **USB EN ATTENTE** et **USB PRÊT** indiquent l’armement des commandes. **SIGNAL USB** apparaît lorsqu’un axe a été appliqué ou qu’une touche a été envoyée à Windows. **SIMULATION** indique que le jeu reçoit la conduite simulée.

[ZIP Vortex](out/ChromeAxis-0.4.7-Cyberpunk-2.5.zip)

## Chrome Axis 0.4.6 · 11 novembre 2026

- **Vortex.** Si l’application est lancée depuis son dossier de préparation, elle retrouve le dossier du jeu quand un lien physique désigne une seule installation correspondante. Le menu **Profil** permet de vérifier l’emplacement retenu pour les réglages.

[ZIP Vortex](out/ChromeAxis-0.4.6-Cyberpunk-2.5.zip)

## Chrome Axis 0.4.5 · 11 novembre 2026

- **Activation USB.** L’application bloque l’activation depuis une copie hors du dossier attendu et demande d’enregistrer les réglages en cours. Elle améliore le réarmement après déconnexion et lorsqu’une pédale dérive légèrement au repos.
- **Journaux.** Le plugin indique le dossier de réglages effectivement lu, les problèmes de lecture DirectInput, les appareils et axes indisponibles, l’attente du neutre et le premier axe ou la première touche transmis.
- **Compatibilité.** La version RED4ext recommandée passe de 1.31.0 à 1.31.1 pour corriger un problème de compilation des scripts.

[ZIP Vortex](out/ChromeAxis-0.4.5-Cyberpunk-2.5.zip)

## Chrome Axis 0.4.4 · 11 novembre 2026

- **Paquet candidat.** Rétablit le cadre natif de Windows tout en conservant les menus noirs et jaunes de la 0.4.3. Le retour à la 0.4.2 décidé ensuite a rétabli les menus natifs et gardé le thème des fenêtres **Accessoires** et **Boîte et palettes**.

## Chrome Axis 0.4.3 · 11 novembre 2026

- **Essai de thème.** Les menus **Profil**, **Périphériques** et **Aide** ainsi que leurs sous-menus passent en noir avec des libellés jaunes. Les commandes désactivées restent grises. L’ouverture a été vérifiée à la souris et au clavier avec Alt+A.

## Chrome Axis 0.4.2 · 11 novembre 2026

- **Fenêtres Accessoires et Boîte et palettes.** Adoption du logo, du bandeau jaune, de la typographie, des panneaux sombres et des boutons cyan de la fenêtre principale. Les listes déroulantes, l’enregistrement avec Entrée et la fermeture avec Échap ont été vérifiés sous Windows. Ce paquet a aussi servi au retour arrière après les essais 0.4.3 et 0.4.4.

[ZIP Vortex](out/ChromeAxis-0.4.2-Cyberpunk-2.5.zip)

## Chrome Axis 0.4.1 · 11 novembre 2026

- **Boîte et palettes.** Ajout d’un assistant pour le séquentiel numérique ou analogique et les grilles en H à 6, 7 ou 8 rapports avec marche arrière. Il capture des boutons, des directions de croix ou les positions d’un axe. La sélection directe d’un rapport en jeu demande un mod de transmission exposant une touche par rapport.
- **Identité visuelle.** Le logo apparaît dans le bandeau de l’application, dans **À propos**, dans les icônes Windows et dans la barre des tâches.

## Chrome Axis 0.4.0 · 11 novembre 2026

- **Calibration multimatériel.** Le volant, l’accélérateur et le frein peuvent venir de trois périphériques distincts. Les profils partiels conservent les commandes du jeu sur les axes non calibrés. Les pédales combinées sont prises en charge lorsque leurs courses partent d’un repos commun et évoluent en sens opposés. La mesure tolère mieux les pics isolés.
- **Accessoires.** Un bouton, une direction de croix ou un axe peut devenir une touche clavier. Ce mécanisme permet notamment à un frein à main sur axe de piloter la commande numérique du jeu.
- **Lectures USB.** Le plugin interroge moins souvent les périphériques au repos, partage les lectures lorsque plusieurs commandes utilisent le même appareil et évite de relire les profils inchangés.

## Chrome Axis 0.3.2 · 11 novembre 2026

- **Renommage du projet.** Direct Wheel V2 devient **Chrome Axis**, avec **Chrome Axis 0** comme nom initial de l’utilitaire. La DLL devient `ChromeAxis.dll` et l’application `ChromeAxis0.exe`. L’interface, les métadonnées Windows, la documentation et les notices adoptent les nouveaux noms.
- **Paquet corrigé sous le même numéro.** L’installation migre vers `red4ext/plugins/chrome_axis`. Une calibration précédente peut être copiée dans `Profil/chrome_axis.ini`. La préparation a vérifié le rechargement d’un ancien profil renommé.

## Direct Wheel V2 0.3.2 · 11 novembre 2026

- **Profil avec le mod.** La calibration et le fichier `usb.enabled` déménagent dans `Profil`, à côté des dossiers `licenses` et `scripts`. L’utilitaire et la DLL utilisent le même emplacement.
- **Interface.** Correction des fragments de boutons lors du redimensionnement et du changement d’onglet. Le thème affine le bandeau, les couleurs et les boutons. L’application utilise Blender Pro et Refinery 25 si ces polices sont installées, avec Barlow fourni comme police de secours.

## Direct Wheel V2 0.3.1 · 11 novembre 2026

- **Activation USB.** L’USB en jeu est désactivé par défaut et peut être activé ou coupé depuis l’application pendant une partie. Le plugin attend le retour au neutre avant de réarmer les axes. Le profil est alors conservé dans `%LOCALAPPDATA%\DirectWheelV2`.
- **Installation réversible.** Le guide décrit l’import du ZIP, le déploiement et le retrait du mod via Vortex. Les affectations clavier et souris de Cyberpunk sont conservées.

## Direct Wheel V2 0.3.0 · 11 novembre 2026

- **Simulation clavier.** Le pavé numérique 4, 6, 8 et 2 ou les touches F6, F7, F8 et F10 font varier progressivement la direction, l’accélérateur et le frein. Le mode comprend un aperçu dans l’utilitaire et un essai en jeu sans profil de calibration.
- **Thème de l’application.** Première présentation jaune et cyan sur fond sombre, avec panneaux biseautés et police Barlow Condensed.

## Direct Wheel V2 0.2.0 · 11 novembre 2026

- **Application Windows.** La calibration quitte le terminal. Une fenêtre permet de choisir le volant et le pédalier, de mesurer le centre, les butées et la course, de lire les axes en direct et d’ajuster leur zone morte. Le profil peut être enregistré et rechargé.
- **Périphériques.** L’application reste accessible sans matériel branché. Si le pédalier se déconnecte, ses valeurs sont neutralisées et le relâchement des pédales est requis pour réarmer les axes à la reconnexion.

## Direct Wheel V2 0.1.0 · 11 novembre 2026

- **Premier prototype.** Un utilitaire de calibration, une DLL RED4ext Windows x64 et un script REDscript ciblent Cyberpunk 2077 2.5 (`3.0.88.46235`). Direction, accélération et freinage analogiques fonctionnent avec un volant et un pédalier USB séparés dans le modèle d’entrée du prototype.
- **Validation initiale.** La DLL et le script compilent. Les tests simulés couvrent notamment la reconnexion d’un pédalier avec une pédale maintenue. Le premier compte rendu réserve le chargement de la DLL dans le jeu et les essais sur matériel physique à une étape ultérieure.
