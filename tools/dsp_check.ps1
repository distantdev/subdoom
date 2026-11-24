param(
    [string]$Config = "Release",
    [string]$BuildDir = ""
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($BuildDir)) {
    $BuildDir = Join-Path $repoRoot "build-audit"
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
    Write-Host ">>> Configuring $BuildDir with $expectedGenerator (plugin off)" -ForegroundColor Cyan
    & cmake -S $repoRoot -B $BuildDir -G $expectedGenerator -A x64 -DSUBDOOM_BUILD_PLUGIN=OFF
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
} else {
    $generator = Get-CMakeCacheValue $cache "CMAKE_GENERATOR:INTERNAL"
    if ($generator -ne $expectedGenerator) {
        throw "CMake generator is '$generator', expected '$expectedGenerator'. Refusing to reconfigure $BuildDir. Do not pass -G Ninja."
    }
    $plugin = Get-CMakeCacheValue $cache "SUBDOOM_BUILD_PLUGIN:BOOL"
    if ($plugin -ne "OFF") {
        throw "SUBDOOM_BUILD_PLUGIN is '$plugin' in $BuildDir. This tree stays plugin-off. Do not deploy from it, and do not reconfigure it here."
    }
}

Write-Host ">>> Building $Config subdoom_test_cli subdoom_unit_tests" -ForegroundColor Cyan
& cmake --build $BuildDir --config $Config --target subdoom_test_cli subdoom_unit_tests
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

$cli = Join-Path $BuildDir "$Config\subdoom_test_cli.exe"
$units = Join-Path $BuildDir "$Config\subdoom_unit_tests.exe"
if (-not (Test-Path -LiteralPath $cli)) {
    throw "Test CLI is missing: $cli"
}
if (-not (Test-Path -LiteralPath $units)) {
    throw "Unit tests are missing: $units"
}

Write-Host ">>> Self-test" -ForegroundColor Cyan
& $cli --self-test
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

Write-Host ">>> Unit tests" -ForegroundColor Cyan
& $units
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

Write-Host ">>> DSP check passed." -ForegroundColor Green
