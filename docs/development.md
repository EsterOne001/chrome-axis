# Développer Chrome Axis

## Où intervenir

| Dossier | Contenu |
| --- | --- |
| `src/core`, `include/chrome_axis` | Entrées, profils, calibration, commandes, retour de force et diagnostic partagés. |
| `src/app` | Application Windows et textes français et anglais. |
| `src/plugin` | Plugin RED4ext qui transmet les commandes à Cyberpunk. |
| `scripts` | Scripts REDscript chargés dans Cyberpunk, dont le contexte de conduite et le HUD. |
| `assets` | Ressources Windows, police, aide visuelle et HUD. |
| `tests`, `tools` | Tests C++ et commandes locales de vérification. |
| `experiments` | Sources et procédure de reconstruction du prototype de boîte manuelle R2. |
| `build`, `out` | Compilations locales dans `build`, ZIP à distribuer et historique dans `out`. |
| `NexusMods` | Textes, vidéos et outils de publication. |

## Compiler et vérifier le ZIP

Depuis la racine du projet avec PowerShell 7.2 ou une version ultérieure, Visual Studio 2026 Build Tools et le SDK Windows.

```powershell
pwsh -NoProfile -File .\tools\Build-ChromeAxis.ps1
```

La commande configure CMake, compile en Release x64, exécute CTest, crée le ZIP Vortex et vérifie son contenu. Elle affiche le résultat dans `build/dev-msvc/packages`. La configuration et les fichiers objets de `build/dev-msvc` sont réutilisés lors des vérifications suivantes. `out` sert à ranger les archives retenues pour distribution.

Après un déplacement du dépôt, utilisez par exemple `-BuildDirectory dev-msvc-reprise` si CMake signale un cache lié à l’ancien emplacement. Cette option choisit un nouveau sous-dossier de `build`. Vérifiez les sources et les preuves d’essai qui subsistent dans les anciens dossiers avant de les archiver.

Passez `-SdkSource` pour réutiliser une copie locale du SDK RED4ext de la révision épinglée dans CMake. Le script affiche le chemin choisi. Lors d’un lancement sans `-SdkSource`, CMake utilise l’archive et le SHA-256 épinglés, y compris si le dossier de compilation contient un ancien chemin de SDK local.

Pour prévalider un ZIP destiné à la publication sans appeler Nexus Mods, utilisez la simulation. Le script contrôle le contenu du ZIP, l’architecture et la version des deux binaires avant la simulation de publication.

```powershell
pwsh -NoProfile -File .\NexusMods\Publish-ChromeAxis.ps1 -Archive .\out\Chromaxis-X.Y.Z.zip -WhatIf
```

## Vérifier un changement REDscript

Utilisez Python 3, le compilateur REDscript de référence indiqué dans le README, et choisissez le dossier du jeu, avec un `final.redscripts` d’origine qui correspond à la version ciblée.

```powershell
$jeu = 'D:/SteamLibrary/steamapps/common/Cyberpunk 2077'
python .\tools\check_redscript.py --scc-library "$jeu/engine/tools/scc_lib.dll" --base-cache "$jeu/r6/cache/final.redscripts"
```

Le script de conduite du projet est sélectionné par défaut. Pour contrôler une combinaison particulière, passez chaque script enregistré par RED4ext avec une option `--script` et utilisez `--loose-scripts` pour un dossier de scripts à ajouter. Le compilateur travaille sur des copies, avec les fenêtres d’erreur désactivées. La commande affiche le chemin du journal local.

Pour diagnostiquer une erreur pendant un lancement de Cyberpunk, rapprochez les heures et les chemins du journal `redscript_rCURRENT.log` de la session qui a affiché le message.

## Vérifier en jeu

Suivez la [check-list manuelle](manual-validation.md) quand une modification touche le déploiement ou les périphériques. Consignez séparément les résultats de compilation, le démarrage en jeu et le test avec le matériel. Les étapes de réinjection du dépôt sont dans [AGENTS.md](../AGENTS.md).