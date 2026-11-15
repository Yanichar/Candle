param(
    [Parameter(Mandatory)][string]$QtBin,
    [string]$TargetDir = "$PSScriptRoot\..\..\build\Candle",
    [string]$Configuration = "release"
)

if (-not (Test-Path (Join-Path $QtBin "windeployqt.exe"))) {
    Write-Error "windeployqt.exe not found in $QtBin"
    exit 1
}

$windeployqt = Join-Path $QtBin "windeployqt.exe"
$target = (Resolve-Path $TargetDir).Path

$env:Path = "$QtBin;$env:Path"

$commonArgs = @(
    "--dir", $target,
    "--$Configuration",
    "--no-compiler-runtime",
    "--no-system-d3d-compiler",
    "--no-opengl-sw"
)

$targets = @(
    (Join-Path $target "candle.exe"),
    (Join-Path $target "designerplugins\cameraplugin.dll"),
    (Join-Path $target "designerplugins\customwidgetsplugin.dll"),
    (Join-Path $target "script\qtscript_multimedia.dll")
)

foreach ($binary in $targets) {
    if (-not (Test-Path $binary)) {
        Write-Host "skip (missing): $binary"
        continue
    }

    Write-Host "deploying Qt dependencies for: $binary"
    & $windeployqt @commonArgs $binary
    if ($LASTEXITCODE -ne 0) {
        Write-Error "windeployqt failed for $binary"
        exit $LASTEXITCODE
    }
}

Write-Host "done: $target"
