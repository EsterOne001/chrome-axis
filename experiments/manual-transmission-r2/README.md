# Prototype de boîte manuelle R2

Cette expérimentation vise Chrome Axis 0.4.38 et Cyberpunk 2077 2.5, exécutable 3.0.88.46235. La base exacte est le commit Git a2577b406597cfcda0474d7925516df44e2522ba. [Code de la base](https://github.com/EsterOne001/chrome-axis/tree/a2577b406597cfcda0474d7925516df44e2522ba)

Le dossier [src/manual](src/manual) contient les six fichiers du prototype. Le [patch d’intégration](integration.patch) raccorde l’essai au plugin et ajoute son test à CTest. La DLL expérimentale reçoit le contexte de conduite du script standard. Le [guide d’essai](packaging/README-Prototype.fr.md) détaille les commandes clavier et les mesures.

## Reconstruire les sources

Depuis la racine du dépôt, utilisez un dossier de travail neuf sous build.

```powershell
$experiment = 'experiments/manual-transmission-r2'
$base = 'a2577b406597cfcda0474d7925516df44e2522ba'
$work = 'build/manual-transmission-r2-rebuild'
$source = "$work/source"
if (Test-Path -LiteralPath $work) { throw 'Choisissez un dossier de travail neuf.' }
New-Item -ItemType Directory -Path $source -ErrorAction Stop | Out-Null

git archive --format=tar --output="$work/base.tar" $base CMakeLists.txt LICENSE README.md README.en.md THIRD_PARTY_NOTICES.md assets include scripts src tests
if ($LASTEXITCODE -ne 0) { throw 'Impossible de préparer la base Git.' }
tar.exe -xf "$work/base.tar" -C $source
if ($LASTEXITCODE -ne 0) { throw 'Impossible de décompresser la base.' }
git apply --directory=$source "$experiment/integration.patch"
if ($LASTEXITCODE -ne 0) { throw 'Le patch R2 ne correspond pas à la base.' }
Copy-Item -LiteralPath "$experiment/src/manual" -Destination "$source/src/manual" -Recurse -ErrorAction Stop
```

## Compiler et vérifier

Le SDK RED4ext est épinglé dans le projet CMake de la base. Visual Studio 2026 et le SDK Windows sont requis pour les commandes ci-dessous. Ouvrez le PowerShell développeur de Visual Studio ou ajoutez son dossier CMake et CTest au PATH.

```powershell
cmake -S $source -B "$work/build" -G 'Visual Studio 18 2026' -A x64
if ($LASTEXITCODE -ne 0) { throw 'Échec de la configuration CMake.' }
cmake --build "$work/build" --config Release
if ($LASTEXITCODE -ne 0) { throw 'Échec de la compilation Release.' }
ctest --test-dir "$work/build" -C Release --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'Échec des tests CTest.' }
```

## Générer le ZIP Vortex

Le script vérifie l’empreinte SHA-256 de la 0.4.38 stable corrigée et celle de la DLL Release validée. Il remplace le plugin dans une nouvelle archive et ajoute les deux guides. Le ZIP de travail est créé sous build. Le script nécessite Python 3.11 ou une version ultérieure.

```powershell
$dll = "$work/build/Release/ChromeAxis.dll"
$dllHash = (Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash
$standardHash = '338F31627227953F73426829B9B243DAEC87B610798612E5E73EEDFEAF931FA3'
python "$experiment/packaging/package.py" --standard 'out/Chromaxis-0.4.38.zip' --standard-sha256 $standardHash --dll $dll --dll-sha256 $dllHash --output-dir "$work/packages"
```

La compilation et le contrôle du ZIP se font hors du jeu. Le test de conduite en jeu reste nécessaire pour valider le comportement de la boîte.
