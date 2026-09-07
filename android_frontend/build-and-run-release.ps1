[CmdletBinding()]
param(
    [string]$DeviceSerial,
    [string]$SdkRoot,
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$BuildAndRunScript = Join-Path $PSScriptRoot 'build-and-run.ps1'

& $BuildAndRunScript @PSBoundParameters -BuildType Release
