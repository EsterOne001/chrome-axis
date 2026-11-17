function Confirm-ChromeAxisBinary {
    param(
        [IO.Compression.ZipArchiveEntry] $Entry,
        [version] $ExpectedVersion,
        [bool] $IsDll
    )

    $temporaryFile = [IO.Path]::GetTempFileName()
    try {
        [IO.Compression.ZipFileExtensions]::ExtractToFile($Entry, $temporaryFile, $true)
        $stream = [IO.File]::OpenRead($temporaryFile)
        try {
            $reader = [System.Reflection.PortableExecutable.PEReader]::new(
                $stream, [System.Reflection.PortableExecutable.PEStreamOptions]::LeaveOpen
            )
            try {
                $headers = $reader.PEHeaders
                if ($headers.CoffHeader.Machine -ne 'Amd64' -or $headers.PEHeader.Magic -ne 'PE32Plus' -or
                    $headers.IsDll -ne $IsDll -or (-not $IsDll -and -not $headers.IsExe)) {
                    throw "Expected a Windows x64 executable or DLL: $($Entry.FullName)"
                }
            } finally {
                $reader.Dispose()
            }
        } finally {
            $stream.Dispose()
        }

        $info = [Diagnostics.FileVersionInfo]::GetVersionInfo($temporaryFile)
        $fileVersion = [version]::new($info.FileMajorPart, $info.FileMinorPart, $info.FileBuildPart, $info.FilePrivatePart)
        $productVersion = [version]::new(
            $info.ProductMajorPart, $info.ProductMinorPart, $info.ProductBuildPart, $info.ProductPrivatePart
        )
        if ($fileVersion -ne $ExpectedVersion -or $productVersion -ne $ExpectedVersion) {
            throw "Expected version $ExpectedVersion in $($Entry.FullName), found file $fileVersion and product $productVersion."
        }
    } finally {
        [IO.File]::Delete($temporaryFile)
    }
}

function Confirm-ChromeAxisPackage {
    param(
        [string] $ArchivePath,
        [version] $Version,
        [string] $RepositoryRoot,
        [string] $BuildPath,
        [ValidatePattern('^[A-Fa-f0-9]{64}$')]
        [string] $ExpectedSha256
    )

    $ErrorActionPreference = 'Stop'
    $plugin = 'red4ext/plugins/chrome_axis'
    $dll = "$plugin/ChromeAxis.dll"
    $exe = "$plugin/ChromeAxis.exe"
    $sourceFiles = @{
        'archive/pc/mod/chrome_axis_hud.archive' = 'assets/hud/chrome_axis_hud.archive'
        $dll                                   = $null
        $exe                                   = $null
        "$plugin/LICENSE"                      = 'LICENSE'
        "$plugin/README.md"                    = 'README.md'
        "$plugin/README.en.md"                 = 'README.en.md'
        "$plugin/THIRD_PARTY_NOTICES.md"        = 'THIRD_PARTY_NOTICES.md'
        "$plugin/Profil/Lisez-moi.txt"         = 'assets/Profil/Lisez-moi.txt'
        "$plugin/licenses/Barlow-OFL.txt"      = 'assets/fonts/OFL.txt'
        "$plugin/scripts/driving_context.reds" = 'scripts/driving_context.reds'
    }
    $binaryVersion = [version]::new($Version.Major, $Version.Minor, $Version.Build, 0)
    $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $files = @{}
    $archive = [IO.Compression.ZipFile]::OpenRead($ArchivePath)
    try {
        if ($ExpectedSha256 -and (Get-FileHash -LiteralPath $ArchivePath -Algorithm SHA256).Hash -ne $ExpectedSha256) {
            throw 'ZIP SHA-256 differs from the validated release checksum.'
        }
        foreach ($entry in $archive.Entries) {
            $name = $entry.FullName.Replace('\', '/')
            if (-not $seen.Add($name.TrimEnd('/'))) {
                throw "Repeated ZIP entry: $name"
            }
            if ($name.EndsWith('/')) {
                if ($entry.Length -ne 0 -or
                    -not ($sourceFiles.Keys | Where-Object { $_.StartsWith($name, [StringComparison]::Ordinal) })) {
                    throw "Unexpected ZIP directory: $name"
                }
                continue
            }
            if ($name -cnotin $sourceFiles.Keys -or $entry.Length -eq 0) {
                throw "Unexpected or empty ZIP entry: $name"
            }
            $files[$name] = $entry
        }

        $missing = $sourceFiles.Keys | Where-Object { -not $files.ContainsKey($_) }
        if ($missing) {
            throw "Missing ZIP entries: $($missing -join ', ')"
        }

        Confirm-ChromeAxisBinary $files[$exe] $binaryVersion $false
        Confirm-ChromeAxisBinary $files[$dll] $binaryVersion $true

        foreach ($name in $sourceFiles.Keys) {
            $relativeSource = $sourceFiles[$name]
            $sourcePath = if ($relativeSource -and $RepositoryRoot) {
                Join-Path $RepositoryRoot $relativeSource
            } elseif (!$relativeSource -and $BuildPath) {
                Join-Path $BuildPath "Release/$([IO.Path]::GetFileName($name))"
            }
            if (!$sourcePath) {
                continue
            }
            $stream = $files[$name].Open()
            try {
                $actual = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($stream))
            } finally {
                $stream.Dispose()
            }
            if ($actual -cne (Get-FileHash -LiteralPath $sourcePath -Algorithm SHA256).Hash) {
                throw "ZIP entry differs from its Release build or source file: $name"
            }
        }
    } finally {
        $archive.Dispose()
    }
}
