param(
    [string]$Config = "Release",
    [string]$BuildDir = ""
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($BuildDir)) {
    $BuildDir = Join-Path $repoRoot "build-msvc"
} elseif (-not [System.IO.Path]::IsPathRooted($BuildDir)) {
    $BuildDir = Join-Path $repoRoot $BuildDir
}
$BuildDir = [System.IO.Path]::GetFullPath($BuildDir)

$expectedGenerator = "Visual Studio 18 2026"

function Get-CMakeCacheValue([string]$CachePath, [string]$Key) {
    $pattern = "^" + [regex]::Escape($Key) + "=(.*)$"
    $match = Select-String -LiteralPath $CachePath -Pattern $pattern | Select-Object -First 1
    if ($null -eq $match) {
        return ""
    }
    return $match.Matches[0].Groups[1].Value.Trim()
}

$cache = Join-Path $BuildDir "CMakeCache.txt"
if (-not (Test-Path -LiteralPath $cache)) {
    Write-Host ">>> Configuring $BuildDir with $expectedGenerator" -ForegroundColor Cyan
    & cmake -S $repoRoot -B $BuildDir -G $expectedGenerator -A x64 -DSUBDOOM_BUILD_PLUGIN=ON
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
} else {
    $generator = Get-CMakeCacheValue $cache "CMAKE_GENERATOR:INTERNAL"
    if ($generator -ne $expectedGenerator) {
        throw "CMake generator is '$generator', expected '$expectedGenerator'. Refusing to reconfigure $BuildDir. Do not pass -G Ninja."
    }
    $plugin = Get-CMakeCacheValue $cache "SUBDOOM_BUILD_PLUGIN:BOOL"
    if ($plugin -eq "OFF") {
        throw "SUBDOOM_BUILD_PLUGIN is '$plugin' in $BuildDir. Wrapper tests need a plugin tree; they do not run in build-audit."
    }
}

Write-Host ">>> Building $Config subdoom_plugin_tests" -ForegroundColor Cyan
& cmake --build $BuildDir --config $Config --target subdoom_plugin_tests
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

$exe = Join-Path $BuildDir "$Config\subdoom_plugin_tests.exe"
if (-not (Test-Path -LiteralPath $exe)) {
    throw "Plugin tests are missing: $exe"
}

Write-Host ">>> Plugin wrapper tests" -ForegroundColor Cyan
& $exe
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

Write-Host ">>> Plugin checks passed." -ForegroundColor Green
