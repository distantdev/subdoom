param(
    [string]$TargetHost = "DAW-LAPTOP",
    [string]$BuildDir = ""
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($BuildDir)) {
    $BuildDir = Join-Path $repoRoot "build-msvc\Subdoom_artefacts\Release"
} elseif (-not [System.IO.Path]::IsPathRooted($BuildDir)) {
    $BuildDir = Join-Path $repoRoot $BuildDir
}
$BuildDir = [System.IO.Path]::GetFullPath($BuildDir)

# Refuse to ship a build older than the sources: a failed build must not be
# silently deployed as if it were current.
$newestSource = Get-ChildItem -Path (Join-Path $repoRoot "plugin"), (Join-Path $repoRoot "dsp") `
    -Recurse -File -Include *.cpp, *.h |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($null -ne $newestSource) {
    $artefactFiles = Get-ChildItem -Path $BuildDir -Recurse -File `
        -Include *.vst3, *.clap, *.exe -ErrorAction SilentlyContinue
    $stale = $artefactFiles | Where-Object { $_.LastWriteTime -lt $newestSource.LastWriteTime }
    if ($stale) {
        $names = ($stale | ForEach-Object { $_.Name }) -join ", "
        Write-Error "Artefacts are older than $($newestSource.Name); build before deploying ($names)." -ErrorAction Stop
        exit 1
    }
}

function Get-CopiedBytes([string]$Path) {
    $item = Get-Item -LiteralPath $Path
    if (-not $item.PSIsContainer) {
        return [int64]$item.Length
    }
    $sum = Get-ChildItem -LiteralPath $Path -Recurse -File | Measure-Object -Property Length -Sum
    if ($null -eq $sum.Sum) {
        return [int64]0
    }
    return [int64]$sum.Sum
}

function Test-FileInUse([System.Management.Automation.ErrorRecord]$ErrorRecord) {
    $message = $ErrorRecord.Exception.Message
    if ($message -match "used by another process") {
        return $true
    }
    $current = $ErrorRecord.Exception
    while ($null -ne $current) {
        if ($current.HResult -eq -2147024864) {
            return $true
        }
        $current = $current.InnerException
    }
    return $false
}

function Copy-Required([string]$Label, [string]$Source, [string]$Share) {
    $problems = @()
    if (-not (Test-Path -LiteralPath $Share)) {
        $problems += "$Label share is missing: $Share"
    }
    if (-not (Test-Path -LiteralPath $Source)) {
        $problems += "$Label artefact is missing: $Source"
    }
    if ($problems.Count -gt 0) {
        throw ($problems -join [Environment]::NewLine)
    }

    $leaf = Split-Path -Leaf $Source
    if ($leaf -like "Doomgazer*") {
        throw "Refusing to copy $leaf. This script deploys Subdoom only."
    }
    $destination = Join-Path $Share $leaf
    try {
        Copy-Item -LiteralPath $Source -Destination $Share -Recurse -Force
    } catch {
        if (Test-FileInUse $_) {
            throw @"
$Label is in use: $Source
Unload Subdoom in Bitwig and REAPER, quit the laptop standalone, and rerun.
$($_.Exception.Message)
"@
        }
        throw "$Label copy failed: $Source -> $Share. $($_.Exception.Message)"
    }

    $copied = Get-Item -LiteralPath $destination
    $bytes = Get-CopiedBytes $destination
    $when = $copied.LastWriteTime.ToString("yyyy-MM-dd HH:mm:ss")
    Write-Host ("  -> {0}  {1} bytes  {2}  {3}" -f $Label, $bytes, $when, $destination) -ForegroundColor Green
    return $destination
}

Write-Host ">>> Deploying SUBDOOM to $TargetHost from $BuildDir" -ForegroundColor Cyan

if (-not (Test-Path -LiteralPath $BuildDir)) {
    throw "Build directory not found: $BuildDir"
}

$vst3 = Join-Path $BuildDir "VST3\Subdoom.vst3"
$clap = Join-Path $BuildDir "CLAP\Subdoom.clap"
$standalone = Join-Path $BuildDir "Standalone\Subdoom.exe"

Copy-Required "VST3" $vst3 "\\$TargetHost\VST3" | Out-Null
Copy-Required "CLAP" $clap "\\$TargetHost\CLAP" | Out-Null

# Subdoom gets its own standalone folder. The Doomgazer-era "Standalone" share
# points at C:\Program Files\Doomgazer, so only use it as a fallback and say so.
$dedicated = "\\$TargetHost\SubdoomStandalone"
$fallback = "\\$TargetHost\Standalone"
$usedFallback = $false
if (Test-Path -LiteralPath $dedicated) {
    Copy-Required "Standalone" $standalone $dedicated | Out-Null
    $standaloneTarget = "C:\Program Files\Subdoom\Subdoom.exe"
} else {
    $usedFallback = $true
    Copy-Required "Standalone" $standalone $fallback | Out-Null
    $standaloneTarget = "C:\Program Files\Doomgazer\Subdoom.exe"
}

Write-Host "  -> Factory presets are compiled in. User presets live in %APPDATA%\Subdoom\Presets on the machine that saves them."
Write-Host "Local standalone: $standalone"
Write-Host "Laptop standalone: $standaloneTarget"

if ($usedFallback) {
    Write-Warning "  No SubdoomStandalone share on $TargetHost, so the standalone went into the Doomgazer folder."
    # Leave the one-time setup next to the exe so it can be run on the laptop
    # without copying files by hand.
    $setupScript = Join-Path $PSScriptRoot "setup_daw_share.ps1"
    if (Test-Path -LiteralPath $setupScript) {
        Copy-Item -LiteralPath $setupScript -Destination (Join-Path $fallback "setup_daw_share.ps1") -Force
        Write-Host ("  -> Also copied setup_daw_share.ps1 to {0}" -f (Join-Path $fallback "setup_daw_share.ps1")) -ForegroundColor Yellow
    }
    Write-Host "  Run that script on the laptop once (elevated), then deploy again." -ForegroundColor Yellow
}

Write-Host ">>> Deployment pass finished. Rescan plugins in Bitwig and REAPER." -ForegroundColor Green
