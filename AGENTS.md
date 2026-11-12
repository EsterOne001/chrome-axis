# Réinjection de Chrome Axis

Quand une tâche prépare une modification du mod pour une réinjection, termine les vérifications applicables décrites dans le README. Pour une modification du code, compile en Release x64, exécute CTest et vérifie le paquet destiné à Vortex.

Une fois la modification prête, crée un commit avec les changements de cette réinjection et pousse-le sur la branche distante suivie avant le déploiement. Si la branche ne suit pas encore de branche distante, utilise la branche du même nom sur origin. La préparation de la réinjection inclut cette synchronisation. Vérifie que GitHub a reçu le commit. En cas de divergence ou d’échec du push, signale le blocage et garde le commit local.
