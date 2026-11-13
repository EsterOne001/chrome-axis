# Essais manuels / Manual validation

[Français](#français) · [English](#english)

## Français

Déployez le ZIP via Vortex. Notez les versions de Chrome Axis, Cyberpunk 2077, RED4ext et redscript. Pour chaque périphérique, relevez le modèle, la version du pilote et le type de branchement. Commencez avec un véhicule à l’arrêt et la conduite automatique désactivée.

- **Diagnostic.** Avec l’USB activé, ouvrez le diagnostic, revenez au jeu et actionnez chaque commande. Basculez dans l’application et vérifiez l’ancienneté des dernières valeurs appliquées et de la dernière touche. Redémarrez l’essai avec une commande maintenue au départ, puis avec un périphérique débranché, pour vérifier les raisons affichées et le réarmement au neutre.
- **Rapport de diagnostic.** Après Alt-Tab, cliquez sur **Copier le diagnostic** et collez le résultat dans un éditeur de texte. Vérifiez que le dernier essai en conduite reste lisible, que la valeur demandée et la dernière valeur écrite restent distinctes et que le texte omet les chemins locaux et les identifiants de périphériques.
- **Installation incomplète.** Dans une copie de test, ouvrez le diagnostic avec la DLL absente, avec le script absent, puis avec les deux absents. Vérifiez que le panneau et le rapport nomment les fichiers manquants.
- **Calibration et profils.** Vérifiez le repos et la course complète du volant et des pédales. Essayez les pédales séparées et, si le pilote le permet, combinées. Enregistrez un profil, passez à un autre, puis rechargez le premier. Vérifiez que le jeu utilise les réglages sélectionnés avec l’application fermée. Recalibrez le frein seul, puis tous les axes d’un appareil. Vérifiez que les axes hors sélection et les affectations d’accessoires sont conservés.
- **Vérification guidée.** Actionnez les commandes une à une sur l’écran de vérification. Reprenez un seul axe et contrôlez les autres jauges de retour sur cet écran. Annulez une reprise pour retrouver les mesures précédentes.
- **Sauvegarde et restauration.** Jeu fermé, sauvegardez le profil par défaut et un profil nommé, avec des réglages différents. Modifiez le profil actif et l’activation USB, puis restaurez. Vérifiez que les axes, zones mortes, accessoires, retour de force, profil actif et activation USB reviennent aux valeurs sauvegardées dans le dossier `Profil` déployé. Relancez l’application pour vérifier la langue.
- **Dossier vide.** Dans une copie de test, sélectionnez un dossier vide nommé `Profil`. Vérifiez le refus et la conservation de la calibration, de l’activation USB et du profil actif.
- **Commandes et HUD.** Contrôlez la direction, l’accélération, le frein et la marche arrière. Vérifiez les états du HUD et sa langue. Ouvrez le menu et le mode photo, changez de fenêtre, puis quittez le siège conducteur. Vérifiez que les commandes reviennent au repos et se réarment une fois relâchées.
- **Déconnexion.** Débranchez un périphérique pendant un essai. Vérifiez que ses commandes reviennent au repos, que ses touches sont relâchées et que les autres périphériques fonctionnent. Rebranchez-le et vérifiez le réarmement au neutre.
- **Retour de force.** À faible intensité, vérifiez le centrage, le sens et les chocs. Vérifiez que l’effet s’arrête à la perte du premier plan ou à la sortie du siège conducteur, puis que le profil chargé applique ses propres réglages. Si possible, comparez une base à engrenages ou courroie et une base à entraînement direct.
- **Transmission et accessoires.** Essayez les palettes et, avec un mod de transmission compatible, chaque position de la boîte en H, y compris le point mort et la marche arrière. Vérifiez les touches configurées dans les deux mods et le relâchement des touches en quittant le siège ou en changeant de fenêtre.
- **Simulation clavier.** Avec l’application déployée ouverte, vérifiez la conduite et l’état **SIMULATION** dans le HUD. Arrêtez la simulation, puis vérifiez que le jeu rend la main aux commandes configurées. Vérifiez aussi que l’aperçu d’une copie autonome fonctionne et que cette copie ne peut pas activer la simulation dans le jeu.

Pour mesurer les performances, comparez le même trajet avec l’USB désactivé, l’USB actif, puis le retour de force actif. Gardez les mêmes périphériques et réglages graphiques et relevez le temps CPU et le temps de trame.

Consignez le résultat de chaque essai et joignez les lignes pertinentes de `red4ext/logs/chromeaxis-*.log` si un problème apparaît. [Audit archivé de la version 0.4.10](archives/audit-0.4.10.md)

## English

Deploy the ZIP through Vortex. Record the Chrome Axis, Cyberpunk 2077, RED4ext, and redscript versions. For each device, record the model, driver version, and connection type. Start with a stationary vehicle and automatic driving disabled.

- **Diagnostics.** With USB enabled, open Diagnostics, switch to the game, and move each control. Switch back and check the age of the last applied values and accessory key. Start again with a control held, then with one device disconnected, and check the reported reasons and rearming at neutral.
- **Diagnostics report.** After Alt-Tab, click **Copy diagnostics** and paste the report into a text editor. Check that the last driving test is still available, requested and last-written values are distinct, and the report omits local paths and device identifiers.
- **Incomplete installation.** In a test copy, open diagnostics with the DLL missing, with the script missing, and with both missing. Check that the panel and report name the missing files.
- **Calibration and profiles.** Check the resting positions and full range of the wheel and pedals. Try separate pedals and, if the driver supports it, combined pedals. Save a profile, switch to another one, then reload the first. With the app closed, check that the game uses the selected settings. Recalibrate only the brake, then all axes on one device. Check that axes outside the selection and accessory bindings are kept.
- **Guided review.** Move each control on the review screen. Redo one axis and check the other gauges when the review returns. Cancel a retry to recover the previous measurements.
- **Backup and restore.** With the game closed, back up the default profile and a named profile with different settings. Change the active profile and USB activation, then restore. Check that the axes, deadzones, accessories, force feedback, active profile and USB activation return to the saved values in the deployed `Profil` folder. Relaunch the app to verify the language.
- **Empty folder.** In a test copy, select an empty folder named `Profil`. Check that the app refuses it and keeps the calibration, USB activation and active profile.
- **Controls and HUD.** Check steering, throttle, braking, and reverse. Verify the HUD states and language. Open the menu and photo mode, switch windows, then leave the driver’s seat. Check that inputs return to rest and rearm once released.
- **Disconnection.** Unplug one device during a test. Check that its axes return to rest, its keys are released, and other devices still work. Reconnect it and check rearming at neutral.
- **Force feedback.** At low intensity, check centering, direction, and impacts. Check that the effect stops when the game loses focus or you leave the driver’s seat. Check that the loaded profile applies its own settings. If available, compare a gear- or belt-driven base with a direct-drive base.
- **Transmission and accessories.** Test the paddles and, with a compatible transmission mod, each H-pattern position, including neutral and reverse. Check the keys configured in both mods and verify that keys are released when you leave the driver’s seat or switch windows.
- **Keyboard simulation.** With the deployed app open, test driving and check the **SIMULATION** HUD status. Stop the simulation and verify that the game returns to the configured controls. Check that a standalone copy can preview keyboard inputs and that it cannot enable simulation in the game.

For performance measurements, repeat the same route with USB disabled, with USB active, and with force feedback active. Keep the hardware and graphics settings fixed and record CPU time and frame time.

Record each result and attach relevant lines from `red4ext/logs/chromeaxis-*.log` if an issue occurs. [Archived 0.4.10 audit in French](archives/audit-0.4.10.md)
