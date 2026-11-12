#Requires -Version 7.2

[CmdletBinding(SupportsShouldProcess)]
param(
    [Parameter(Mandatory)]
    [string] $Archive
)

$apiBase = 'https://api.nexusmods.com/v3'
$modUrl = 'https://www.nexusmods.com/cyberpunk2077/mods/38104'
$versionsPath = '/mod-files/9ka2kySGT/versions'

function Invoke-NexusRequest {
    param(
        [string] $Method,
        [string] $Path,
        [hashtable] $Body
    )

    $request = @{
        Uri = "$apiBase$Path"
        Method = $Method
        Headers = $apiHeaders
        TimeoutSec = 20
        MaximumRedirection = 0
        ErrorAction = 'Stop'
    }
    if ($null -ne $Body) {
        $request.ContentType = 'application/json'
        $request.Body = $Body | ConvertTo-Json -Compress
    }

    try {
        $response = Invoke-RestMethod @request
    } catch [Microsoft.PowerShell.Commands.HttpResponseException] {
        $status = [int] $_.Exception.Response.StatusCode
        throw "Nexus Mods returned HTTP $status for $Method $Path."
    } catch {
        throw "Could not complete $Method $Path on Nexus Mods."
    }

    if ($null -eq $response.data) {
        throw "Nexus Mods returned an unexpected response for $Method $Path."
    }
    $response.data
}

$file = Get-Item -LiteralPath $Archive -ErrorAction Stop
if ($file -isnot [System.IO.FileInfo]) {
    throw 'Select the Chrome Axis release ZIP.'
}

$archiveName = [regex]::Match(
    $file.Name, '^ChromeAxis-(?<version>[0-9]+\.[0-9]+\.[0-9]+)-Cyberpunk-(?<game>[0-9]+\.[0-9]+)\.zip$'
)
if (-not $archiveName.Success) {
    throw 'Use the release name ChromeAxis-X.Y.Z-Cyberpunk-A.B.zip.'
}
if ($file.Length -gt 100MB) {
    throw 'The ZIP exceeds the Nexus Mods single-part upload limit of 100 MiB.'
}

$zip = [System.IO.Compression.ZipFile]::OpenRead($file.FullName)
try {
    $entries = $zip.Entries | Where-Object Length -gt 0 | ForEach-Object { $_.FullName.Replace('\', '/') }
    foreach ($entry in @(
        'red4ext/plugins/chrome_axis/ChromeAxis.exe'
        'red4ext/plugins/chrome_axis/ChromeAxis.dll'
        'red4ext/plugins/chrome_axis/scripts/driving_context.reds'
        'archive/pc/mod/chrome_axis_hud.archive'
    )) {
        if ($entry -cnotin $entries) {
            throw "The release ZIP is missing $entry."
        }
    }
} finally {
    $zip.Dispose()
}

$version = $archiveName.Groups['version'].Value
$gameVersion = $archiveName.Groups['game'].Value
if (-not $PSCmdlet.ShouldProcess("$modUrl (file 9ka2kySGT)", "Publish $($file.Name) as the main file")) {
    return
}

if ([string]::IsNullOrWhiteSpace($env:NEXUSMODS_API_KEY)) {
    throw 'Set NEXUSMODS_API_KEY in this PowerShell session using your personal Nexus Mods API key.'
}
$apiHeaders = @{
    Accept = 'application/json'
    apikey = $env:NEXUSMODS_API_KEY.Trim()
}

$existing = Invoke-NexusRequest -Method Get -Path $versionsPath
if ($existing.versions -isnot [array]) {
    throw 'Nexus Mods returned an unexpected file version list.'
}
if ($version -in $existing.versions.version) {
    throw "Version $version already exists for this Nexus Mods file."
}

$digest = (Get-FileHash -LiteralPath $file.FullName -Algorithm MD5 -ErrorAction Stop).Hash.ToLowerInvariant()
$upload = Invoke-NexusRequest -Method Post -Path '/uploads' -Body @{
    size_bytes = $file.Length
    filename = $file.Name
    md5 = $digest
}
$uploadId = [guid]::Empty
$uploadUri = $null
if (-not [guid]::TryParse([string] $upload.id, [ref] $uploadId) -or
    -not [uri]::TryCreate([string] $upload.presigned_url, [UriKind]::Absolute, [ref] $uploadUri) -or
    $uploadUri.Scheme -ne 'https' -or $uploadUri.UserInfo) {
    throw 'Nexus Mods returned an invalid upload session.'
}

# The signed storage request has its own headers and never receives the Nexus API key.
$uploadHeaders = @{
    'Content-Disposition' = 'attachment; filename="{0}"' -f $file.Name
    'Content-MD5' = [Convert]::ToBase64String([Convert]::FromHexString($digest))
}
try {
    $null = Invoke-WebRequest -Uri $uploadUri -Method Put -Headers $uploadHeaders -InFile $file.FullName `
        -ContentType 'application/octet-stream' -TimeoutSec 120 -MaximumRedirection 0 -ErrorAction Stop
} catch [Microsoft.PowerShell.Commands.HttpResponseException] {
    $status = [int] $_.Exception.Response.StatusCode
    throw "Nexus Mods upload storage rejected the ZIP (HTTP $status)."
} catch {
    throw 'Could not transfer the ZIP to Nexus Mods upload storage.'
}

$uploadPath = "/uploads/$uploadId"
$upload = Invoke-NexusRequest -Method Post -Path "$uploadPath/finalise"
$wait = [System.Diagnostics.Stopwatch]::StartNew()
while ($upload.state -eq 'created' -and $wait.Elapsed.TotalSeconds -lt 60) {
    Start-Sleep -Seconds 2
    $upload = Invoke-NexusRequest -Method Get -Path $uploadPath
}
if ($upload.state -ne 'available') {
    throw "Nexus Mods did not make upload $uploadId available. No file version was created."
}

try {
    $created = Invoke-NexusRequest -Method Post -Path $versionsPath -Body @{
        upload_id = $uploadId.ToString()
        name = "Chrome Axis for Cyberpunk 2077 $gameVersion"
        description = "RED4ext plugin, English/French setup app and in-game status HUD for Cyberpunk 2077 $gameVersion on Windows x64."
        version = $version
        file_category = 'main'
        primary_mod_manager_download = $true
        allow_mod_manager_download = $true
        show_requirements_pop_up = $true
        update_mod_version = $true
        archive_existing_file = $false
    }
    $versionId = [string] $created.version.id
    if (-not $versionId) {
        throw 'Nexus Mods did not return a version ID.'
    }
} catch {
    throw "$($_.Exception.Message) Check the Files tab for version $version before retrying."
}

$scanStatus = 'unknown'
try {
    $encodedVersionId = [uri]::EscapeDataString($versionId)
    $published = Invoke-NexusRequest -Method Get -Path "/mod-file-versions/$encodedVersionId"
    if ($published.id -cne $versionId -or $published.version -cne $version -or -not $published.security_scan_status) {
        throw 'Nexus Mods returned unexpected version details.'
    }
    $scanStatus = $published.security_scan_status
} catch {
    Write-Warning "Nexus Mods created version $versionId. Could not verify its scan status. $($_.Exception.Message)"
}

[pscustomobject] @{
    Version = $version
    NexusVersionId = $versionId
    SecurityScanStatus = $scanStatus
    FilesUrl = "$($modUrl)?tab=files"
}
