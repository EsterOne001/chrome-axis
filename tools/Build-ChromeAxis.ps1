#Requires -Version 7.2

[CmdletBinding()]
param(
    [string] $CMake,
    [string] $SdkSource,
    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9_-]*$')]
    [string] $BuildDirectory = 'dev-msvc'
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Release-Package.ps1')
$repository = Split-Path -Parent $PSScriptRoot
$buildRoot = Join-Path $repository 'build'
$build = Join-Path $buildRoot $BuildDirectory
$packages = Join-Path $build 'packages'
foreach ($directory in @($buildRoot, $build, $packages)) {
    $item = Get-Item -LiteralPath $directory -Force -ErrorAction SilentlyContinue
    if ($item -and (-not $item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint))) {
        throw "The build path must be a regular directory inside the repository: $directory"
    }
}
$generator = 'Visual Studio 18 2026'

function Test-Generator([string] $Executable) {
    if (-not (Test-Path -LiteralPath $Executable -PathType Leaf)) {
        return $false
    }
    $helpText = & $Executable --help
    return ($LASTEXITCODE -eq 0 -and [bool] ($helpText -match [regex]::Escape($generator)))
}

function Find-CMake {
    $fromPath = Get-Command cmake.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($fromPath -and (Test-Generator $fromPath.Source)) {
        return $fromPath.Source
    }

    $installer = [Environment]::GetEnvironmentVariable('ProgramFiles(x86)')
    if ($installer) {
        $vswhere = Join-Path $installer 'Microsoft Visual Studio/Installer/vswhere.exe'
        if (Test-Path -LiteralPath $vswhere -PathType Leaf) {
            $installation = & $vswhere -latest -products '*' -version '[18.0,19.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
            if ($LASTEXITCODE -eq 0 -and $installation) {
                $bundled = Join-Path $installation 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
                if (Test-Generator $bundled) {
                    return $bundled
                }
            }
        }
    }
    throw "CMake with '$generator' support is required. Install CMake with the Visual Studio C++ workload or pass -CMake."
}

function Invoke-Step([string] $Label, [string] $Executable, [string[]] $Arguments) {
    Write-Host $Label
    & $Executable @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Label failed with exit code $LASTEXITCODE."
    }
}

if ($CMake) {
    $cmakePath = (Resolve-Path -LiteralPath $CMake).ProviderPath
    if (-not (Test-Generator $cmakePath)) {
        throw "The specified CMake cannot use '$generator': $cmakePath"
    }
} else {
    $cmakePath = Find-CMake
}
$toolBin = Split-Path -Parent $cmakePath
$ctest = Join-Path $toolBin 'ctest.exe'
$cpack = Join-Path $toolBin 'cpack.exe'
foreach ($tool in @($ctest, $cpack)) {
    if (-not (Test-Path -LiteralPath $tool -PathType Leaf)) {
        throw "Required CMake tool not found: $tool"
    }
}

$configure = @('-S', $repository, '-B', $build, '-G', $generator, '-A', 'x64',
               '-DCHROME_AXIS_BUILD_PLUGIN=ON', '-DBUILD_TESTING=ON')
if ($SdkSource) {
    $sdk = (Resolve-Path -LiteralPath $SdkSource).ProviderPath
    if (-not (Test-Path -LiteralPath (Join-Path $sdk 'include/RED4ext/RED4ext.hpp') -PathType Leaf)) {
        throw "The specified RED4ext SDK source directory is invalid: $sdk"
    }
    Write-Host "RED4ext SDK from local source: $sdk"
    $configure += "-DFETCHCONTENT_SOURCE_DIR_RED4EXT_SDK:PATH=$sdk"
} else {
    Write-Host 'RED4ext SDK pinned by CMakeLists.txt'
    $configure += @('-U', 'FETCHCONTENT_SOURCE_DIR_RED4EXT_SDK')
}

Invoke-Step 'Configure Release x64' $cmakePath $configure
Invoke-Step 'Build Release x64' $cmakePath @('--build', $build, '--config', 'Release', '--parallel')
Invoke-Step 'Run CTest' $ctest @('--test-dir', $build, '-C', 'Release', '--output-on-failure', '--no-tests=error')
$packagingStarted = [DateTime]::UtcNow
Invoke-Step 'Build Vortex ZIP' $cpack @('--config', (Join-Path $build 'CPackConfig.cmake'), '-C', 'Release')

$version = Select-String -LiteralPath (Join-Path $build 'CMakeCache.txt') -Pattern '^CMAKE_PROJECT_VERSION:STATIC=(\d+\.\d+\.\d+)$'
if (-not $version) {
    throw 'Cannot read the project version from CMakeCache.txt.'
}
$archive = Join-Path $packages "Chromaxis-$($version.Matches[0].Groups[1].Value).zip"
$archiveFile = Get-Item -LiteralPath $archive
if ($archiveFile.LastWriteTimeUtc -lt $packagingStarted.AddSeconds(-2)) {
    throw "CPack did not create a fresh ZIP: $archive"
}
Confirm-ChromeAxisPackage -ArchivePath $archive -Version $version.Matches[0].Groups[1].Value `
    -RepositoryRoot $repository -BuildPath $build
Write-Host "Verified Vortex ZIP: $archive"
Write-Output "SHA-256: $((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash)"
