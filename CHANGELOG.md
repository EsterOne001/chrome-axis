# Historique de Chrome Axis

Chrome Axis ajoute la conduite analogique à Cyberpunk 2077 avec un volant et des pédales DirectInput. Le projet a commencé sous le nom **Direct Wheel V2**, à partir de l’étude du mod tiers Direct Wheel 3.4.1. L’[archive de référence](references/DirectWheel-3.4.1.zip) est conservée localement dans `references/`.

Les entrées sont classées de la plus récente à la plus ancienne. Les dates indiquent la préparation des paquets ou des prototypes, en heure de Bruxelles. L’historique initial couvre 36 versions de travail, dont 26 ZIP de distribution conservés et vérifiés par CRC. Les premiers prototypes ont été reconstitués à partir des comptes rendus de développement, de fichiers sources historiques et des README embarqués. La présence d’un ZIP local atteste sa préparation pour Vortex. Les essais en jeu sont précisés lorsqu’ils sont documentés.

Les ZIP de distribution se trouvent dans [`out/`](out/). Ce dossier est local et ignoré par Git. Le [guide de validation](docs/manual-validation.md#français) décrit les essais matériels, en jeu et de coexistence avec les autres mods.

## Chrome Axis 0.4.66

- **Transmission.** L'assistant permet de convertir un levier en H par touches vers le mode natif en réutilisant ses contacts. La montée et la descente en séquentiel natif peuvent être capturées sur deux appareils USB.
- **Application USB.** Les lectures de calibration, de capture et d'aperçu passent en arrière-plan. L'inventaire retente les sondages momentanément indisponibles et garde les commandes déjà reconnues.
- **Arrêt du plugin.** Les threads de lecture et de retour de force reçoivent leur demande d'arrêt avant les attentes de fermeture. Le journal signale une attente prolongée.
- **Validation.** CTest vérifie la liaison entre deux processus et le rejeu après refus d'acquisition DirectInput. Le build et le prévol de publication contrôlent les binaires x64 et leur version dans le ZIP.

[ZIP Vortex](out/Chromaxis-0.4.66.zip)

## Chrome Axis 0.4.65

- **Palettes sur deux périphériques.** Les relevés au repos reçus avant un appui restent disponibles pour le rejeu, même si la boucle de traitement prend du retard. La validation continue d’écarter un relevé absent, ancien ou postérieur à l’appui.
- **Détection DirectInput.** Après un `E_PENDING`, un refus d’acquisition de la poignée de détection laisse le sondage réessayer les commandes en attente. Les commandes déjà reconnues restent utilisables.

[ZIP Vortex](out/Chromaxis-0.4.65.zip)

## Chrome Axis 0.4.64

- **Palettes sur deux périphériques.** Une demande de rapport exige un relevé suffisamment récent de chaque palette au moment de l’appui. Les appuis brefs distincts gardent leur ordre.
- **Modification des affectations.** La détection d’un nouvel axe ou bouton conserve les éléments déjà vérifiés du même périphérique connecté. Seuls les nouveaux éléments attendent leur détection.
- **Détection DirectInput.** Lorsqu’une vérification retourne `E_PENDING`, le thread de détection peut retenter l’appel avec une acquisition temporaire non exclusive.

[ZIP Vortex](out/Chromaxis-0.4.64.zip)

## Chrome Axis 0.4.63

- **Palettes et accessoires.** Un chevauchement des palettes bloque le changement de rapport. Les variations d’un axe déjà maintenu n’encombrent plus la file. Le rejeu d’un appui conserve l’état valide des autres périphériques.
- **Lecture USB et calibration.** La détection des commandes configurées se poursuit indépendamment de la lecture en conduite. L’application continue la mesure du périphérique sélectionné pendant l’actualisation de l’inventaire et conserve les axes utilisés au début de la mesure.
- **Retour de force et raccourci.** Un refus temporaire d’accès au pilote est retenté rapidement. L’appui physique AUTO / MANUEL reste valide lors de l’actualisation du même contexte de conduite.
- **Diagnostic.** Le rapport engagé et les consignes de transmission disposent de lignes dédiées. L’export CSV garde son ordre chronologique et laisse la fenêtre utilisable.

[ZIP Vortex](out/Chromaxis-0.4.63.zip)

## Chrome Axis 0.4.62

- **Pédales.** Après une pause, une pédale disponible peut reprendre en marche avant ou au point mort même si l’autre est déconnectée. Les protections de routage restent actives pendant une transition de marche arrière.
- **Palettes et accessoires.** Les appuis brefs lus par DirectInput sont conservés jusqu’au traitement de la commande. Les événements anciens ou enregistrés avant une interruption sont écartés.
- **Capture.** La touche choisie dans l’assistant de boîte séquentielle reste associée à la commande si la reprise USB impose une nouvelle mesure au repos.
- **Retour de force.** La lecture valide qui suit une réouverture après une attente prolongée du pilote permet de relancer le retour de force.

[ZIP Vortex](out/Chromaxis-0.4.62.zip)

## Chrome Axis 0.4.61

- **Reprise USB.** Les commandes sont lues et leur fraîcheur est contrôlée par périphérique. Une redétection lente laisse les autres périphériques utilisables.
- **Retour de force.** La reprise confirmée du même volant réinitialise le délai de tentative. Les demandes annulées sont contrôlées entre les opérations du pilote.
- **Capture et diagnostic.** Les assistants attendent le retour réel au centre d’une croix redevenue lisible. Ils continuent la capture pendant l’actualisation d’un autre périphérique. Le rapport retient les commandes fraîches en USB partiel et l’export CSV classe les incidents selon leur horodatage.

[ZIP Vortex](out/Chromaxis-0.4.61.zip)

## Chrome Axis 0.4.60

- **Reprise USB.** Les appels de retour de force sont isolés de la lecture des axes et boutons. La découverte DirectInput retente les vérifications momentanément indisponibles et l’application actualise son inventaire en arrière-plan.
- **Conduite et calibration.** Un embrayage désactivé est exclu de l’état USB en jeu. La calibration conserve la précision des valeurs et la grille indique les limites de la boîte native.
- **Transmission et diagnostic.** La bascule AUTO / MANUEL traite les appuis physiques successifs, le diagnostic conserve le dernier relevé valide et le CSV suit les changements de raccourci dans un menu. L’application distingue l’autorisation USB enregistrée des commandes effectivement actives dans le jeu.

[ZIP Vortex](out/Chromaxis-0.4.60.zip)

## Chrome Axis 0.4.59

- **Reprise USB.** Chrome Axis réutilise ses connexions DirectInput lorsque la fenêtre du jeu reste la même et retire `GetCapabilities` de la lecture des axes et des boutons. Une perte de périphérique déclenche toujours sa réacquisition avec réarmement au neutre.
- **Transmission.** Le raccourci AUTO / MANUEL reconnaît les appuis physiques brefs et conserve la dernière affectation valide si sa configuration ne peut pas être relue. Le mode sélectionné est conservé pendant une pause dans le même véhicule.
- **HUD et diagnostic.** Le badge distingue des commandes armées, une commande récemment appliquée et un réarmement partiel. Le diagnostic conserve le dernier relevé de conduite valide et indique les commandes en attente, la touche de transmission et les modes demandé et actif.

[ZIP Vortex](out/Chromaxis-0.4.59.zip)

## Chrome Axis 0.4.58

- **Diagnostic en direct.** L’enregistrement continue après la fermeture de la fenêtre Diagnostic, jusqu’à l’arrêt manuel ou à la fermeture de l’application. L’application sauvegarde le CSV par lots dans `Profil/Diagnostics` et signale les lignes perdues si le tampon est dépassé. L’export reprend les données de conduite et les incidents USB dans la même chronologie.

[ZIP Vortex](out/Chromaxis-0.4.58.zip)

## Chrome Axis 0.4.57

- **Diagnostic USB.** Le rapport copié conserve les incidents USB récents après un changement de véhicule. Il distingue les opérations DirectInput lentes, les erreurs et la reprise d’un périphérique, les cycles rejetés, l’attente de réarmement et les interruptions du contexte du jeu.

[ZIP Vortex](out/Chromaxis-0.4.57.zip)

## Chrome Axis 0.4.56

- **HUD de conduite.** Le fond du badge reprend la teinte et le réglage d’opacité de la minicarte de Cyberpunk, à 75 % dans le thème standard.

[ZIP Vortex](out/Chromaxis-0.4.56.zip)

## Chrome Axis 0.4.55

- **Marche arrière.** L’engagement de R est autorisé jusqu’à 3 km/h inclus. Le passage de la marche arrière à un rapport avant reste limité à 1,8 km/h.
- **HUD de conduite.** Le titre et la valeur du rapport partagent le même axe horizontal. Le titre est dégagé du biseau du liseré gris.

[ZIP Vortex](out/Chromaxis-0.4.55.zip)

## Chrome Axis 0.4.54

- **Boîte manuelle.** Au point mort après un menu, le déplacement vers l’avant permet de sélectionner directement tout rapport avant disponible, même si le dernier rapport est inconnu. La marche arrière et les changements de sens conservent leur protection.

[ZIP Vortex](out/Chromaxis-0.4.54.zip)

## Chrome Axis 0.4.53

- **Boîte manuelle.** Après la fermeture de la carte ou d’un menu, un rapport avant peut être réengagé en roulant si la boîte est passée au point mort depuis un rapport avant. Le changement entre marche avant et marche arrière exige toujours une vitesse proche de zéro.
- **Reprise USB.** Le plugin rouvre les périphériques lorsque le jeu reprend la conduite après une interruption du contexte, y compris si le joueur reste au volant. Les commandes se réarment après leur retour au repos.

[ZIP Vortex](out/Chromaxis-0.4.53.zip)

## Chrome Axis 0.4.52

- **HUD de conduite.** Le badge est plus haut pour espacer les textes du liseré et séparer les deux lignes dans chaque colonne. Le fond bleu est plus transparent.

[ZIP Vortex](out/Chromaxis-0.4.52.zip)

## Chrome Axis 0.4.51

- **HUD de conduite.** Le liseré gris intérieur est davantage espacé du contour extérieur et des équerres du volant. Le rapport est légèrement relevé pour dégager le bas du badge.

[ZIP Vortex](out/Chromaxis-0.4.51.zip)

## Chrome Axis 0.4.50

- **HUD Painter.** Les couleurs du contour et de la transmission s’appliquent dès leur modification. Elles s’enregistrent automatiquement avec le préréglage actif. Les couleurs enregistrées auparavant restent disponibles comme base pour les préréglages qui n’ont pas encore de réglage propre.
- **HUD de conduite.** Le cadre intérieur et les équerres autour du logo utilisent des motifs gris de l’atlas natif de Cyberpunk.

[ZIP Vortex](out/Chromaxis-0.4.50.zip)

## Chrome Axis 0.4.49

- **HUD Painter.** La section Chrome Axis est rattachée au contrôleur du menu à l’ouverture. La liste est retrouvée à partir de ses lignes de couleur, y compris quand HUD Painter reconstruit l’écran après un changement de préréglage.

[ZIP Vortex](out/Chromaxis-0.4.49.zip)

## Chrome Axis 0.4.48

- **HUD Painter.** La section Chrome Axis permet de régler séparément le contour du badge et le texte de la boîte. Le bouton Appliquer enregistre ces couleurs dans un fichier dédié à Chrome Axis. Elles sont indépendantes de la palette et des préréglages du jeu.

[ZIP Vortex](out/Chromaxis-0.4.48.zip)

## Chrome Axis 0.4.47

- **HUD de conduite.** Le cadre passe au cyan. Le mode de boîte et le rapport engagé apparaissent en rouge, avec un séparateur rouge discret entre les deux colonnes. Le logo et le libellé gardent les couleurs des états USB.

- **HUD Painter.** Les préréglages personnalisés peuvent recolorer le cadre et la transmission depuis la palette du jeu. La compatibilité est facultative.

[ZIP Vortex](out/Chromaxis-0.4.47.zip)

## Chrome Axis 0.4.46

- **HUD de conduite.** Le badge compact reprend le trait rouge, le décroché et le fond translucide de l’interface de Cyberpunk 2077. L’état et le mode de boîte figurent à gauche, avec le rapport engagé en grand à droite. Le pictogramme et le libellé conservent les couleurs des états.

[ZIP Vortex](out/Chromaxis-0.4.46.zip)

## Chrome Axis 0.4.45

- **Boîte et pédales USB.** Le retour en AUTO libère les pédales après une observation de boîte indisponible lorsque le routage est sûr. Les demandes de rapport expirent même si le jeu ne met plus la boîte à jour. Le réarmement en marche arrière reste pris en compte.
- **Affectations analogiques.** Les conflits entre axes de conduite, embrayage et frein à main sont signalés. Les réglages d’un ancien profil sont conservés pour permettre leur correction. Les pédales combinées de sens opposés restent compatibles.
- **Stabilisation.** L’état du filtre est visible et l’aperçu utilise la même évolution dans le temps que le jeu.
- **Diagnostic et profils.** La capture relève la conduite avec l’USB désactivé et reste exportable après réouverture de Diagnostic dans la même instance de l’application. L’activation USB, le raccourci AUTO / MANUEL et la langue partagent le verrou de restauration des profils.

[ZIP Vortex](out/Chromaxis-0.4.45.zip)

## Chrome Axis 0.4.44

- **Périphériques.** Les réglages sont regroupés dans Volant, Pédales, Transmission, Frein à main et Accessoires.
- **Transmission.** Les réglages de boîte et de palettes et le raccourci AUTO / MANUEL sont accessibles depuis le même assistant. L’embrayage dispose de son propre écran sous Transmission. Les affectations et les calibrations existantes sont conservées.
- **Édition des commandes.** Les affectations de boîte ou de palettes se modifient et se retirent depuis Transmission. Accessoires conserve les touches génériques. Le frein à main progressif s’ouvre directement depuis Matériel USB.

[ZIP Vortex](out/Chromaxis-0.4.44.zip)

## Chrome Axis 0.4.43

- **Transmission.** Accès direct depuis Matériel USB, choix entre levier en H et palettes natives, suivi de la demande jusqu’à l’observation du rapport engagé. Les raccourcis clavier existants restent utilisables.
- **Diagnostic.** Capture locale exportable en CSV, estimation P95/P99 et comptage des dépassements de cadence. Les relevés de vitesse et régime sont activés uniquement pendant une capture.
- **Retour de force et pédales.** Effort à basse vitesse, amortissement et suggestion de gain après mesure. Stabilisation indépendante de l’accélérateur et du frein.
- **Embrayage.** Calibration avec jauge, aide facultative au passage des rapports et essai d’atténuation de l’accélérateur à cinq paliers. Le couple natif et le frein moteur restent à mesurer en jeu.

[ZIP Vortex](out/Chromaxis-0.4.43.zip)

## Chrome Axis 0.4.42

- **Simulation clavier.** La bascule AUTO / MANUEL lit directement la touche configurée quand la simulation est active. Elle attend le relâchement des touches d’accessoires en cours lors d’un changement de mode. Chaque bascule est consignée dans le journal du plugin.

[ZIP Vortex](out/Chromaxis-0.4.42.zip)

## Chrome Axis 0.4.41

- **Raccourci AUTO / MANUEL.** Choisissez la touche dans le menu **Périphériques**. Le réglage concerne l’installation, s’applique sans relancer le jeu et suit les sauvegardes de profils. F8 est la valeur par défaut.
- **Simulation clavier.** Les commandes réservées aux rapports sont désactivées dans le sélecteur. La touche choisie ne déclenche aucun axe simulé et l’autre commande de l’axe reste disponible.

[ZIP Vortex](out/Chromaxis-0.4.41.zip)

## Chrome Axis 0.4.40

- **Marche arrière.** Le routage des pédales suit le rapport engagé lors de la bascule AUTO/MANUEL ou de la perte du levier. Le contrôle d’arrêt suit également le dernier sens engagé à travers le point mort.
- **Profils et commandes.** L’assistant confirme la grille du levier et indique quelles anciennes grilles doivent être recalibrées. La bascule F8 utilise l’état du clavier physique. La simulation attend le relâchement complet des touches de rapport pour se réarmer.
- **Diagnostic et lecture.** La fenêtre indique le rapport sélectionné, demandé et engagé. Accessoires et Boîte / palettes partagent l’inventaire des périphériques. Le plugin mesure la vitesse de la boîte lors des changements de sens qui l’exigent.

[ZIP Vortex](out/Chromaxis-0.4.40.zip)

## Chrome Axis 0.4.39

- **Boîte en H native.** L’assistant Chrome Axis enregistre les positions du levier dans le profil actif et les transmet directement à Cyberpunk. Il conserve un mode par touches pour les configurations existantes. La sélection directe inclut le point mort et les rapports autorisés par le véhicule.
- **Bascule et diagnostic.** F8 bascule entre AUTO et MANUEL à l’arrêt. Le HUD affiche N, R ou le rapport engagé et le rapport copié distingue la sélection, la demande et le résultat côté jeu. Après une perte d’entrée, réarmez le levier au point mort.
- **Essai clavier.** La simulation permet la sélection directe d’un rapport et l’essai du point mort ainsi que la montée et la descente séquentielles.

[ZIP Vortex](out/Chromaxis-0.4.39.zip)

## Chrome Axis 0.4.38 · 15 novembre 2026

- **Réarmement après Alt+Tab.** La direction se réarme après trois lectures consécutives d’au plus 1,5 % après application de la zone morte. Un volant relâché qui indique environ 1 % peut ainsi reprendre la direction et permettre la reprise du retour de force. La zone morte du profil, y compris 0 %, s’applique à la conduite.

[ZIP Vortex](out/ChromeAxis-0.4.38-Cyberpunk-2.5.zip)

## Chrome Axis 0.4.37 · 15 novembre 2026

- **Accessoires.** L’onglet **Touches** regroupe les affectations clavier et l’assistant de boîte et palettes. L’onglet **Frein à main** réunit la calibration, le plafond et l’aperçu de la commande progressive.
- **Profils.** L’onglet affiche le levier et le plafond enregistrés. **Reprendre l’axe Espace** réutilise une calibration existante. Les deux onglets enregistrent leurs réglages dans le profil actif.

- **Reprise et diagnostic.** La calibration s’interrompt si la fenêtre perd le premier plan. Le levier garde son signal lorsqu’une touche d’accessoire est bloquée, et la consigne est invalidée si les données du véhicule expirent. Le diagnostic du frein à main affiche les valeurs au dixième de point.

[ZIP Vortex](out/ChromeAxis-0.4.37-Cyberpunk-2.5.zip)

## Chrome Axis 0.4.36 · 15 novembre 2026

- **Frein à main progressif.** Dans **Accessoires…**, calibrez un axe au repos et en fin de course, puis définissez sa sortie progressive. Le plafond est réglable de 1 à 100 % et commence à 15 %. Le profil conserve la calibration et le plafond.
- **Aperçu et diagnostic.** L’aperçu montre la course du levier et la commande calculée. En conduite USB, le plugin transmet cette commande au jeu et conserve la priorité de la commande native lorsqu’elle est plus forte.

[ZIP Vortex](out/ChromeAxis-0.4.36-Cyberpunk-2.5.zip)

## Chrome Axis 0.4.35 · 15 novembre 2026

- **Accessoires et palettes.** Choisissez **Maintien** ou **Impulsion unique** pour chaque affectation. Un nouvel appui déclenche une touche brève en mode impulsion. Le maintien reste le comportement des affectations existantes.
- **Diagnostic des commandes.** **Détails…** et le rapport copié affichent la touche, le type d’envoi, l’état de lecture et le dernier envoi accepté par Windows pour chaque affectation. Le dernier relevé de conduite reste disponible au retour dans l’application.

[ZIP Vortex](out/ChromeAxis-0.4.35-Cyberpunk-2.5.zip)

## Chrome Axis 0.4.34 · 15 novembre 2026

- **Pédalier.** Un graphique montre ensemble l’accélérateur et le frein, avec la course lue, la sortie de la courbe et, pendant la conduite USB, les commandes retenues par le jeu. Le frein propose un réglage progressif pour les pédales mesurant la course et un réglage linéaire pour les capteurs de pression. Ces préréglages conservent la course utile et la zone morte existantes.
- **Retour de force.** La résistance évolue plus progressivement lors d’un dérapage et à la reprise d’adhérence. Le diagnostic compare l’effort calculé, la consigne plafonnée et l’effet accepté par le pilote. La mesure distingue la saturation de l’effort continu de celle provoquée par un choc ou une réception.

[ZIP Vortex](out/ChromeAxis-0.4.34-Cyberpunk-2.5.zip)

## Chrome Axis 0.4.33 · 15 novembre 2026

- **Direction.** Réglez la sensibilité autour du centre et la course utile de 50 à 100 % de la course calibrée. Les jauges comparent les deux sens de rotation avant et après réglage.
- **Accélérateur.** Réglez la progressivité et la course nécessaire pour atteindre 100 %. Les jauges réagissent au pédalier ou à une valeur simulée. Les réglages d’axes sont enregistrés dans le profil et repris lors de sa sauvegarde ou de sa restauration.
- **Retour de force.** Le curseur **Intensité des chocs** règle de 0 à 200 % les secousses des collisions et des réceptions de saut. Les anciens profils prennent la valeur de 100 % et le pilote reçoit une consigne limitée à sa plage nominale.

[ZIP Vortex](out/ChromeAxis-0.4.33-Cyberpunk-2.5.zip)

## Chrome Axis 0.4.32 · 15 novembre 2026

- **Comportement en conduite.** Le retour de force tient compte de la charge latérale, de la rotation du véhicule, du dérapage et de la perte de contact avec le sol. Les reprises de contact et la direction des chocs produisent des réactions distinctes.
- **Résistance de rappel.** Le curseur **Rappel** règle de 50 à 250 % la force de recentrage et la résistance en virage, indépendamment du gain général. Le pilote reçoit une consigne bornée à sa plage nominale. Les anciens profils reprennent un rappel de 100 %.
- **Diagnostic.** Le rapport indique l’âge des données du véhicule, l’effort demandé, les effets transmis au pilote et l’écrêtage à la limite nominale. Le ZIP fournit ensemble l’application, la DLL et le script mis à jour.

[ZIP Vortex](out/ChromeAxis-0.4.32-Cyberpunk-2.5.zip)

## Chrome Axis 0.4.31 · 15 novembre 2026

- **Gain du retour de force.** Le curseur va de 5 à 200 % par paliers de 5 %. Au-delà de 100 %, le gain amplifie l’effort calculé dans la limite de la force nominale DirectInput. Les profils existants conservent leur gain jusqu’au premier déplacement du curseur. La valeur par défaut reste à 12 %, avec le retour de force désactivé.

[ZIP Vortex](out/ChromeAxis-0.4.31-Cyberpunk-2.5.zip)

## Chrome Axis 0.4.30 · 15 novembre 2026

- **Reprise de conduite.** Au retour au siège conducteur ou après l’expiration du signal du contexte de conduite, le plugin rouvre les périphériques USB pour la nouvelle session et attend le retour des commandes au neutre. Une lecture DirectInput durablement en attente déclenche aussi une réouverture de l’appareil concerné.
- **HUD.** Le badge se rattache de nouveau au HUD actif lorsque le jeu remplace son interface pendant une transition.
- **Essai en jeu.** Rejouez la cinématique qui interrompait les commandes, puis centrez le volant et relâchez les pédales et palettes une fois la conduite rendue à V. Vérifiez que le badge et les commandes reviennent sans toucher au bouton USB.

[ZIP Vortex](out/ChromeAxis-0.4.30-Cyberpunk-2.5.zip)

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
