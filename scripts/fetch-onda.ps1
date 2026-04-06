param(
    [Parameter(Mandatory = $true)]
    [string]$Destination,
    [string]$Version = $env:ONDA_VERSION
)

$ErrorActionPreference = "Stop"

if ((Test-Path (Join-Path $Destination "include\\onda.h")) -and (Test-Path (Join-Path $Destination "lib"))) {
    Write-Host "[INFO] Reusing existing Onda SDK at $Destination"
    exit 0
}

$arch = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture
if ($arch -ne [System.Runtime.InteropServices.Architecture]::X64) {
    throw "No official Onda SDK asset configured for Windows/$arch. Pass an extracted SDK path explicitly instead."
}

$apiBaseUrl = if ($env:ONDA_API_BASE_URL) { $env:ONDA_API_BASE_URL } else { "https://api.github.com/repos/onda-lang/onda/releases" }
$assetPattern = "onda-*-windows-x64.zip"
$releaseLabel = if ($Version) { $Version } else { "latest" }
$headers = @{
    Accept = "application/vnd.github+json"
    "X-GitHub-Api-Version" = "2022-11-28"
}

Write-Host "[INFO] Resolving Onda release $releaseLabel"

if ($Version) {
    $releaseApiUrl = "${apiBaseUrl}/tags/$Version"
    $release = Invoke-RestMethod -Uri $releaseApiUrl -Headers $headers
} else {
    try {
        $release = Invoke-RestMethod -Uri "${apiBaseUrl}/latest" -Headers $headers
    }
    catch {
        Write-Host "[INFO] '/releases/latest' is unavailable; falling back to the releases list."
        $releases = Invoke-RestMethod -Uri "${apiBaseUrl}?per_page=20" -Headers $headers
        $release = $releases | Where-Object { -not $_.draft } | Select-Object -First 1
    }
}

$releaseTag = if ($release) { $release.tag_name } else { $null }
if (-not $releaseTag) {
    throw "Failed to resolve an Onda release from GitHub."
}

$asset = $release.assets | Where-Object { $_.name -like $assetPattern } | Select-Object -First 1
if (-not $asset) {
    throw "Release $releaseTag does not contain a Windows x64 SDK asset"
}

$archiveUrl = $asset.browser_download_url
$assetName = $asset.name
$cacheDir = Join-Path (Split-Path -Parent $Destination) ".onda-downloads"
$archivePath = Join-Path $cacheDir "$releaseTag-$assetName"
$tmpDir = Join-Path ([System.IO.Path]::GetTempPath()) ("onda-sdk-" + [System.Guid]::NewGuid().ToString("N"))

New-Item -ItemType Directory -Path $cacheDir -Force | Out-Null
New-Item -ItemType Directory -Path $tmpDir -Force | Out-Null

try {
    Write-Host "[INFO] Downloading $archiveUrl"
    Invoke-WebRequest -Uri $archiveUrl -OutFile $archivePath

    Write-Host "[INFO] Extracting $assetName"
    Expand-Archive -LiteralPath $archivePath -DestinationPath $tmpDir -Force

    $header = Get-ChildItem -Path $tmpDir -Recurse -Filter onda.h | Select-Object -First 1
    if (-not $header) {
        throw "Extracted archive does not contain include/onda.h"
    }

    $sdkRoot = Split-Path -Parent (Split-Path -Parent $header.FullName)
    if (-not (Test-Path (Join-Path $sdkRoot "lib"))) {
        throw "Extracted archive does not contain a lib directory"
    }

    if (Test-Path $Destination) {
        Remove-Item -LiteralPath $Destination -Recurse -Force
    }

    $parentDir = Split-Path -Parent $Destination
    if (-not (Test-Path $parentDir)) {
        New-Item -ItemType Directory -Path $parentDir -Force | Out-Null
    }

    Copy-Item -LiteralPath $sdkRoot -Destination $Destination -Recurse -Force
    Write-Host "[INFO] Onda SDK $releaseTag ready at $Destination"
}
finally {
    if (Test-Path $tmpDir) {
        Remove-Item -LiteralPath $tmpDir -Recurse -Force
    }
}
