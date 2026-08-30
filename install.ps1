# Installs the sxn binary release for Windows.
#
#   irm https://sxfescript.github.io/latest/install.ps1 | iex
#
# https://sxfescript.github.io/vX.Y.Z/install.ps1 pins a specific version's
# copy of this same script instead - see scripts/publish-docs.sh, which
# publishes both from this file on every release.
#
# Override the version with $env:SXN_VERSION (default: latest release tag),
# and the install directory with $env:SXN_INSTALL (default: ~\.sxn).

$ErrorActionPreference = "Stop"

$Repo = "SxfeScript/sxfescript"
$InstallDir = if ($env:SXN_INSTALL) { $env:SXN_INSTALL } else { "$env:USERPROFILE\.sxn" }
$BinDir = "$InstallDir\bin"

switch ($env:PROCESSOR_ARCHITECTURE) {
    "AMD64" { $Arch = "x64" }
    "ARM64" { $Arch = "arm64" }
    default { throw "unsupported architecture: $env:PROCESSOR_ARCHITECTURE" }
}

$Version = $env:SXN_VERSION
if (-not $Version) {
    # GitHub's /releases/latest only ever returns the newest *stable* release,
    # 404ing entirely when every release so far is a pre-release - true for
    # this project today. Fall back to the newest release of any kind.
    try {
        $Version = (Invoke-RestMethod "https://api.github.com/repos/$Repo/releases/latest").tag_name
    } catch {
        $Version = (Invoke-RestMethod "https://api.github.com/repos/$Repo/releases")[0].tag_name
    }
    if (-not $Version) { throw "couldn't resolve the latest release; set `$env:SXN_VERSION to install a specific one" }
}

$VersionNum = $Version -replace "^v", ""
$Asset = "sxn-$VersionNum-windows-$Arch.zip"
$Url = "https://github.com/$Repo/releases/download/$Version/$Asset"

Write-Host "Installing sxn $Version (windows-$Arch)..."

$Tmp = New-Item -ItemType Directory -Path ([System.IO.Path]::GetTempPath() + [System.Guid]::NewGuid())
try {
    $ZipPath = "$Tmp\$Asset"
    try {
        Invoke-WebRequest -Uri $Url -OutFile $ZipPath
    } catch {
        throw "couldn't download $Url (does this release ship a windows-$Arch binary?)"
    }

    New-Item -ItemType Directory -Force -Path $BinDir | Out-Null
    Expand-Archive -Path $ZipPath -DestinationPath $Tmp -Force
    Copy-Item "$Tmp\sxn-windows-$Arch.exe" "$BinDir\sxn.exe" -Force
} finally {
    Remove-Item -Recurse -Force $Tmp
}

$UserPath = [Environment]::GetEnvironmentVariable("Path", "User")
if ($UserPath -notlike "*$BinDir*") {
    [Environment]::SetEnvironmentVariable("Path", "$UserPath;$BinDir", "User")
    Write-Host "Added $BinDir to your User PATH"
}

Write-Host "sxn $Version installed to $BinDir\sxn.exe"
Write-Host "Open a new shell, or run: `$env:Path += `";$BinDir`""
