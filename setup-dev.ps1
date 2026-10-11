# Windows dev setup. Safe to re-run.
# Checks Visual Studio, sets VCPKG_ROOT, creates .venv.
# Usage: powershell -ExecutionPolicy Bypass -File setup-dev.ps1
$root = $PSScriptRoot
$vsconfig = "$root\reconstruct\.vsconfig"
$installer = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer"
$vswhere = "$installer\vswhere.exe"

# Visual Studio (any edition, or Build Tools) with the C++ tools and vcpkg.
$vs = $null
if ( Test-Path $vswhere )
{
    $required = @( "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "Microsoft.VisualStudio.Component.Vcpkg" )
    $vs = & $vswhere -latest -products * -requires $required -property installationPath | Select-Object -First 1
}
if ( -not $vs )
{
    $existing = if ( Test-Path $vswhere ) { & $vswhere -latest -products * -property installationPath | Select-Object -First 1 }
    if ( $existing )
    {
        "Visual Studio at $existing is missing components. Add them with:"
        "  & `"$installer\setup.exe`" modify --installPath `"$existing`" --config `"$vsconfig`""
    }
    else
    {
        "Visual Studio not found. Install it with:"
        "  winget install --id Microsoft.VisualStudio.2022.Community --override `"--passive --config $vsconfig`""
    }
    exit 1
}
"Visual Studio: $vs"

# VCPKG_ROOT: keep an existing working one, otherwise use VS's bundled vcpkg.
if ( $env:VCPKG_ROOT -and ( Test-Path "$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake" ) )
{
    "VCPKG_ROOT: $env:VCPKG_ROOT (existing)"
}
else
{
    $env:VCPKG_ROOT = "$vs\VC\vcpkg"
    [Environment]::SetEnvironmentVariable( "VCPKG_ROOT", $env:VCPKG_ROOT, "User" )
    "VCPKG_ROOT: $env:VCPKG_ROOT (set for your user; restart VS Code and open terminals to pick it up)"
}

# Python venv for the web side and the reconstruction dev tools.
if ( -not ( Test-Path "$root\.venv" ) )
{
    if ( -not ( Get-Command py -ErrorAction SilentlyContinue ) )
    {
        "Python launcher not found. Install Python 3.12 with:  winget install --id Python.Python.3.12"
        exit 1
    }
    py -3.12 -m venv "$root\.venv"
    if ( $LASTEXITCODE -ne 0 ) { "Could not create .venv with Python 3.12 (winget install --id Python.Python.3.12)"; exit 1 }
}
& "$root\.venv\Scripts\python.exe" -m pip install --quiet --disable-pip-version-check -r "$root\stweb\requirements.txt" -r "$root\reconstruct\tools\requirements.txt"
if ( $LASTEXITCODE -ne 0 ) { "pip install failed"; exit 1 }
"Python: $root\.venv"

""
"Done. Build the solver from a Developer PowerShell (or open reconstruct\ in Visual Studio, or the repo root in VS Code):"
"  cd `"$root\reconstruct`"; cmake --preset windows; cmake --build --preset windows-release"
