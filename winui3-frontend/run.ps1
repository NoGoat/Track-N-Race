[CmdletBinding()]
param(
    [switch] $NoBuild,

    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]] $ModeArguments
)

$ErrorActionPreference = 'Stop'
$projectRoot = $PSScriptRoot
$projectPath = Join-Path $projectRoot 'TrackNRace.WinUI3\TrackNRace.WinUI3.csproj'

$Configuration = 'Debug'
if ($ModeArguments.Count -gt 1) {
    throw 'Specify only one build mode: --debug or --release.'
}
if ($ModeArguments.Count -eq 1) {
    $Configuration = switch ($ModeArguments[0].ToLowerInvariant()) {
        '--debug' { 'Debug' }
        '--release' { 'Release' }
        default {
            throw "Unknown argument '$($ModeArguments[0])'. Use --debug or --release."
        }
    }
}

$userDotnetPath = Join-Path $env:USERPROFILE '.dotnet\dotnet.exe'
if (Test-Path -LiteralPath $userDotnetPath) {
    $dotnetPath = $userDotnetPath
}
else {
    $dotnetCommand = Get-Command dotnet -ErrorAction SilentlyContinue
    if (-not $dotnetCommand) {
        throw 'The .NET 10 SDK was not found. Install it from https://dotnet.microsoft.com/download/dotnet/10.0.'
    }
    $dotnetPath = $dotnetCommand.Source
}

$runArguments = @(
    'run'
    '--project', $projectPath
    '--configuration', $Configuration
)
if ($NoBuild) {
    $runArguments += '--no-build'
}

Push-Location $projectRoot
try {
    & $dotnetPath @runArguments
    if ($LASTEXITCODE -ne 0) {
        throw "Track N Race exited with code $LASTEXITCODE."
    }
}
finally {
    Pop-Location
}
