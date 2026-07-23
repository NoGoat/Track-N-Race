param(
    [string]$BuildDir = "$PSScriptRoot\build-windows",
    [string]$DistDir = "$PSScriptRoot\dist\windows",
    [switch]$WithBreeze
)

$ErrorActionPreference = "Stop"

# The native Win32 frontend is statically linked and has no Breeze dependency.
# This switch keeps its packaging entry points consistent with the Qt scripts.

$BuildDir = [System.IO.Path]::GetFullPath($BuildDir)
$DistDir = [System.IO.Path]::GetFullPath($DistDir)

$distRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot "dist"))
if (-not $DistDir.StartsWith($distRoot + [System.IO.Path]::DirectorySeparatorChar,
                            [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "DistDir must remain under $distRoot so stale staged files can be removed safely."
}
if (Test-Path -LiteralPath $DistDir) {
    Remove-Item -LiteralPath $DistDir -Recurse -Force
}

cmake -S $PSScriptRoot -B $BuildDir -A x64 `
    -DCMAKE_BUILD_TYPE=Release `
    "-DCMAKE_INSTALL_PREFIX=$DistDir"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

cmake --build $BuildDir --config Release --target minimal_app --parallel
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

cmake --install $BuildDir --config Release
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "Portable static Win32 application staged at: $DistDir"
