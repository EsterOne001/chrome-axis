# Logo du HUD / HUD logo

[Français](#français) · [English](#english) · [PowerShell](#powershell)

## Français

L’archive est construite avec [WolvenKit Console 10.0.0-beta.2-nightly.2026-11-10](https://github.com/WolvenKit/WolvenKit-nightly-releases/releases/tag/10.0.0-beta.2-nightly.2026-11-10). Elle contient deux ressources sous `chrome_axis\hud`. L’atlas `logo.inkatlas` associe la zone `logo` à la totalité de `logo.xbm` pour chacune des résolutions.

Le détourage de `assets/hud/logo.png` a été réalisé avec l’outil intégré ImageGen. La consigne était de retirer le fond noir, de conserver la silhouette du volant et ses couleurs jaune et cyan, et de rendre le fond réellement transparent. Cette image reste la source du logo de marque. Le jeu utilise une texture blanche sans compression XBM qui reprend exactement son canal alpha. Ink peut ainsi teinter la silhouette avec la même couleur que le texte d’état.

Le texte est défini dans [le script du HUD](../../scripts/driving_context.reds). Il suit la langue d’affichage du jeu, avec le français et l’anglais. Les deux langues utilisent la même archive de ressources.

1. Depuis un dossier de travail distinct du dépôt, générez le PNG de travail `logo.png` à partir de la source de marque. Remplacez `$source` dans les commandes par le chemin absolu de `assets/hud/logo.png`.
2. Créez `chrome_axis_hud/chrome_axis/hud` dans le dossier de travail. Extrayez `your_custom_weapon_hudicon.xbm` du [modèle de texture pour HUD](https://github.com/CDPR-Modding-Documentation/Cyberpunk-Modding-Docs/blob/main/_resources_and_assets/icons/weapon_hudicon.zip) et copiez-le à la racine du dossier de travail sous le nom `logo.xbm`. Son SHA-256 est `AA5661998CFEEF8D6FF9471FFDFCD37FDF29E25950AB51073637EFE6F4638537`.
3. Copiez aussi `logo.inkatlas.json` à la racine du dossier de travail. Exécutez les commandes depuis ce dossier pour remplacer la compression du modèle, importer la texture et construire l’archive.

## English

The archive is built with [WolvenKit Console 10.0.0-beta.2-nightly.2026-11-10](https://github.com/WolvenKit/WolvenKit-nightly-releases/releases/tag/10.0.0-beta.2-nightly.2026-11-10). It contains two resources under `chrome_axis\hud`. The `logo.inkatlas` atlas maps its `logo` region to the full `logo.xbm` texture at each resolution.

ImageGen removed the black background from `assets/hud/logo.png` while preserving the wheel silhouette and its yellow and cyan colors. This PNG is the brand source. The game uses an uncompressed white XBM texture with the same alpha channel. Ink can tint the silhouette to match the status text.

The [HUD script](../../scripts/driving_context.reds) uses the game’s on-screen language to choose the French or English status text. Both languages use the same resource archive.

1. In a working directory outside the repository, generate a working `logo.png` from the brand source. Set `$source` in the commands below to the absolute path of `assets/hud/logo.png`.
2. Create `chrome_axis_hud/chrome_axis/hud` in the working directory. Extract `your_custom_weapon_hudicon.xbm` from the [HUD texture template](https://github.com/CDPR-Modding-Documentation/Cyberpunk-Modding-Docs/blob/main/_resources_and_assets/icons/weapon_hudicon.zip) and copy it to the working directory as `logo.xbm`. Its SHA-256 is `AA5661998CFEEF8D6FF9471FFDFCD37FDF29E25950AB51073637EFE6F4638537`.
3. Copy `logo.inkatlas.json` to the working directory. Run the commands there to replace the template’s compression, import the texture and build the archive.

## PowerShell

```powershell
Add-Type -AssemblyName System.Drawing
$source = 'C:/chemin/du/projet/assets/hud/logo.png'
$art = [Drawing.Bitmap]::new($source)
try {
    $mask = [Drawing.Bitmap]::new($art.Width, $art.Height, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
    try {
        for ($y = 0; $y -lt $art.Height; ++$y) {
            for ($x = 0; $x -lt $art.Width; ++$x) {
                $mask.SetPixel($x, $y, [Drawing.Color]::FromArgb($art.GetPixel($x, $y).A, 255, 255, 255))
            }
        }
        $mask.Save((Join-Path (Get-Location) 'logo.png'), [Drawing.Imaging.ImageFormat]::Png)
    } finally { $mask.Dispose() }
} finally { $art.Dispose() }
```

```powershell
WolvenKit.CLI convert s logo.xbm
$texture = Get-Content -Raw -LiteralPath logo.xbm.json
[IO.File]::WriteAllText((Join-Path (Get-Location) 'logo.xbm.json'),
    $texture.Replace('"compression": "TCM_DXTAlpha"', '"compression": "TCM_None"'))
WolvenKit.CLI convert d logo.xbm.json
Copy-Item -LiteralPath logo.xbm -Destination chrome_axis_hud/chrome_axis/hud
WolvenKit.CLI import logo.png -o chrome_axis_hud/chrome_axis/hud --keep
WolvenKit.CLI convert d logo.inkatlas.json
Copy-Item -LiteralPath logo.inkatlas -Destination chrome_axis_hud/chrome_axis/hud
WolvenKit.CLI pack chrome_axis_hud
WolvenKit.CLI archive chrome_axis_hud.archive -l
```
