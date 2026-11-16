# Chrome Axis 0.4.38 · Prototype de boîte manuelle R2

Ce paquet permet de tester les rapports de marche avant de Cyberpunk 2077 au clavier. La DLL gère les commandes de boîte et le script de conduite du prototype affiche le rapport engagé dans le HUD.

Le prototype cible **Cyberpunk 2077 2.5** (exécutable **3.0.88.46235**). Utilisez **RED4ext 1.31.1** et **REDscript 0.5.32**. [Guide de Chrome Axis](../README.md) · [Instructions en anglais](README-Prototype.en.md)

## Remplacer le prototype précédent dans Vortex

1. Fermez Cyberpunk et Chrome Axis. Si vous avez modifié vos profils, sauvegardez-les depuis le menu **Profil** de l’application.
2. Désactivez le premier prototype de boîte manuelle et tout autre paquet Chrome Axis actif. Déployez cette modification dans Vortex.
3. Si vous aviez copié le premier prototype manuellement, retirez de l’installation du jeu son ancienne DLL `ChromeAxisManualTransmissionProbe.dll` et son script `vehicle_transmission.reds`. Leur emplacement d’origine est `red4ext/plugins/ChromeAxisManualTransmissionProbe`. Vous pouvez conserver ailleurs les relevés de l’ancien essai.
4. Dans Vortex, importez le ZIP `Chromaxis-0.4.38-prototype.zip` avec **Installer depuis un fichier**, activez-le et déployez. Gardez un seul paquet Chrome Axis actif pendant l’essai.


Si l’ancienne DLL est encore présente, le mode manuel R2 reste désactivé et le nouveau fichier `status.txt` indique de désactiver le premier prototype et de redéployer. Les fichiers de mesure de l’ancien essai peuvent rester dans leur dossier.

## Tester au clavier

1. Dans le dossier du jeu, créez un fichier vide `arm.txt` dans `red4ext/plugins/chrome_axis/manual-transmission-probe`.
2. Lancez le jeu. Désactivez la simulation clavier de Chrome Axis pour utiliser les commandes habituelles de Cyberpunk, puis faites un passage en boîte automatique avec une accélération modérée sur une route droite et dégagée. Pour comparer, reprenez le même tronçon dans le même sens avec la même voiture.
3. En marche avant au-dessus d’environ **7 km/h (4,5 mph)**, appuyez sur **F8** pendant environ **0,2 seconde**, puis relâchez. Utilisez **Page précédente** pour monter un rapport et **Page suivante** pour descendre. Laissez chaque touche revenir au repos avant la commande suivante. Le prototype peut refuser un rétrogradage en fonction de la vitesse et du régime estimé.
4. Appuyez de nouveau sur **F8** pour reprendre les passages automatiques. Après une interruption de conduite ou un retour au jeu, réactivez le mode manuel avec **F8**.
5. Quittez le jeu normalement. Copiez `trials.csv` et `status.txt` depuis le dossier où se trouve `arm.txt`, puis retirez `arm.txt` avant le prochain lancement. Le premier armement après un redémarrage remplace l’ancien CSV.

Sous le logo Chrome Axis, le HUD associe le rapport engagé au mode de boîte, comme dans `RAPPORT 3 · AUTO` et `RAPPORT 3 · MANUEL`. Le mode s’actualise dès que la bascule avec **F8** est acceptée. Un tiret remplace le numéro en l’absence de rapport disponible.

Le HUD peut afficher les rapports 1 à 8 selon le véhicule. La commande manuelle de la R2 couvre au maximum sept rapports avant. Comparez le rapport engagé avec le rapport cible et le résultat de la commande dans le CSV.

Le CSV contient le rapport engagé, le rapport cible, le résultat des demandes de changement, le régime moteur et la vitesse. Une session dure jusqu’à 20 minutes.

Les vérifications de compilation sont réalisées hors du jeu. Le test de conduite sert à vérifier les rapports engagés et le retour à l’automatique.

## Revenir à Chrome Axis 0.4.38 standard

Fermez Cyberpunk et Chrome Axis, désactivez le prototype R2, réactivez le paquet 0.4.38 standard et déployez dans Vortex.