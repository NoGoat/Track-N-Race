[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string] $Configuration = 'Debug'
)

$ErrorActionPreference = 'Stop'
$nativeRoot = Join-Path $PSScriptRoot 'native'
$buildRoot = Join-Path $nativeRoot 'build'

cmake -S $nativeRoot -B $buildRoot -A x64
if ($LASTEXITCODE -ne 0) {
    throw "Native bridge configuration failed with exit code $LASTEXITCODE."
}

cmake --build $buildRoot --config $Configuration --target track_n_race_engine_bridge
if ($LASTEXITCODE -ne 0) {
    throw "Native bridge build failed with exit code $LASTEXITCODE."
}

