# Essais manuels / Manual validation

[Français](#français) · [English](#english)

## Français

Déployez le ZIP via Vortex. Notez les versions de Chrome Axis, Cyberpunk 2077, RED4ext et redscript. Pour chaque périphérique, relevez le modèle, la version du pilote et le type de branchement. Commencez avec un véhicule à l’arrêt et la conduite automatique désactivée.

- **Diagnostic.** Avec l’USB activé, ouvrez le diagnostic, revenez au jeu et actionnez chaque commande. Basculez dans l’application et vérifiez l’ancienneté des dernières valeurs appliquées et de la dernière touche. Redémarrez l’essai avec une commande maintenue au départ, puis avec un périphérique débranché, pour vérifier les raisons affichées et le réarmement au neutre.
- **Calibration et profils.** Vérifiez le repos et la course complète du volant et des pédales. Essayez les pédales séparées et, si le pilote le permet, combinées. Enregistrez un profil, passez à un autre, puis rechargez le premier. Vérifiez que le jeu utilise les réglages sélectionnés avec l’application fermée. Recalibrez le frein seul, puis tous les axes d’un appareil. Vérifiez que les axes hors sélection et les affectations d’accessoires sont conservés.
- **Commandes et HUD.** Contrôlez la direction, l’accélération, le frein et la marche arrière. Vérifiez les états du HUD et sa langue. Ouvrez le menu et le mode photo, changez de fenêtre, puis quittez le siège conducteur. Vérifiez que les commandes reviennent au repos et se réarment une fois relâchées.
- **Déconnexion.** Débranchez un périphérique pendant un essai. Vérifiez que ses commandes reviennent au repos, que ses touches sont relâchées et que les autres périphériques fonctionnent. Rebranchez-le et vérifiez le réarmement au neutre.
- **Retour de force.** À faible intensité, vérifiez le centrage, le sens et les chocs. Vérifiez que l’effet s’arrête à la perte du premier plan ou à la sortie du siège conducteur, puis que le profil chargé applique ses propres réglages. Si possible, comparez une base à engrenages ou courroie et une base à entraînement direct.
- **Transmission et accessoires.** Essayez les palettes et, avec un mod de transmission compatible, chaque position de la boîte en H, y compris le point mort et la marche arrière. Vérifiez les touches configurées dans les deux mods et le relâchement des touches en quittant le siège ou en changeant de fenêtre.
- **Simulation clavier.** Avec l’application ouverte, vérifiez la conduite et l’état **SIMULATION**. Arrêtez la simulation, puis vérifiez que le jeu rend la main aux commandes configurées.

Pour mesurer les performances, comparez le même trajet avec l’USB désactivé, l’USB actif, puis le retour de force actif. Gardez les mêmes périphériques et réglages graphiques et relevez le temps CPU et le temps de trame.

Consignez le résultat de chaque essai et joignez les lignes pertinentes de `red4ext/logs/chromeaxis-*.log` si un problème apparaît. [Audit archivé de la version 0.4.10](archives/audit-0.4.10.md)

## English

Deploy the ZIP through Vortex. Record the Chrome Axis, Cyberpunk 2077, RED4ext, and redscript versions. For each device, record the model, driver version, and connection type. Start with a stationary vehicle and automatic driving disabled.

- **Diagnostics.** With USB enabled, open Diagnostics, switch to the game, and move each control. Switch back and check the age of the last applied values and accessory key. Start again with a control held, then with one device disconnected, and check the reported reasons and rearming at neutral.
- **Calibration and profiles.** Check the resting positions and full range of the wheel and pedals. Try separate pedals and, if the driver supports it, combined pedals. Save a profile, switch to another one, then reload the first. With the app closed, check that the game uses the selected settings. Recalibrate only the brake, then all axes on one device. Check that axes outside the selection and accessory bindings are kept.
- **Controls and HUD.** Check steering, throttle, braking, and reverse. Verify the HUD states and language. Open the menu and photo mode, switch windows, then leave the driver’s seat. Check that inputs return to rest and rearm once released.
- **Disconnection.** Unplug one device during a test. Check that its axes return to rest, its keys are released, and other devices still work. Reconnect it and check rearming at neutral.
- **Force feedback.** At low intensity, check centering, direction, and impacts. Check that the effect stops when the game loses focus or you leave the driver’s seat. Check that the loaded profile applies its own settings. If available, compare a gear- or belt-driven base with a direct-drive base.
- **Transmission and accessories.** Test the paddles and, with a compatible transmission mod, each H-pattern position, including neutral and reverse. Check the keys configured in both mods and verify that keys are released when you leave the driver’s seat or switch windows.
- **Keyboard simulation.** With the app open, test driving and check the **SIMULATION** status. Stop the simulation and verify that the game returns to the configured controls.

For performance measurements, repeat the same route with USB disabled, with USB active, and with force feedback active. Keep the hardware and graphics settings fixed and record CPU time and frame time.

Record each result and attach relevant lines from `red4ext/logs/chromeaxis-*.log` if an issue occurs. [Archived 0.4.10 audit in French](archives/audit-0.4.10.md)
