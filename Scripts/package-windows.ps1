<#
Builds SpectraBlade for 64-bit Windows, runs the tests with the built app as the plug-in scan worker,
and packs it two ways, each with its .sha256:
  dist/SpectraBlade-<version>-windows-x64.zip        the program and the licences, to run from anywhere
  dist/SpectraBlade-<version>-windows-x64-setup.exe  an installer (Scripts/SpectraBlade.iss, Inno Setup 6)

Usage: Scripts/package-windows.ps1 [-Juce <JUCE checkout>] [-Jobs <n>] [-SkipTests] [-NoInstaller]

Needs Visual Studio 2022 or later (or its Build Tools) with the C++ workload, found with vswhere, and
for the installer Inno Setup 6 (ISCC.exe on the PATH, in ISCC, or where its installer puts it).
#>
param (
    [string] $Juce = "",
    [int] $Jobs = 4,
    [switch] $SkipTests,
    [switch] $NoInstaller
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root "build-release-win"
$dist = Join-Path $root "dist"

$version = (Select-String -Path (Join-Path $root "CMakeLists.txt") -Pattern '^project\(SpectraBlade VERSION ([0-9.]+)').Matches[0].Groups[1].Value
if (-not $version) { throw "no version in CMakeLists.txt" }

# The compiler's environment, from the newest Visual Studio with the C++ tools.
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found: install Visual Studio 2022 or later, or its Build Tools" }
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "no Visual Studio with the C++ tools (Desktop development with C++)" }
cmd /c "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
}
foreach ($tool in "cmake", "ninja", "cl") {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) { throw "$tool not found after setting up Visual Studio's environment" }
}

Write-Host "== Building SpectraBlade $version (x64)"
$configure = @("-S", $root, "-B", $build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release")
if ($Juce) { $configure += "-DSPECTRABLADE_JUCE_DIR=$Juce" }
& cmake @configure
if ($LASTEXITCODE -ne 0) { throw "configuring failed" }
& cmake --build $build --config Release -j $Jobs
if ($LASTEXITCODE -ne 0) { throw "building failed" }

$exe = Join-Path $build "SpectraBlade_artefacts\Release\SpectraBlade.exe"
$tester = Join-Path $build "SpectraBladeTests_artefacts\Release\SpectraBladeTests.exe"

if (-not $SkipTests) {
    Write-Host "== Tests, with the built app as the scan worker"
    $log = Join-Path $build "tests.log"
    & $tester --app $exe *> $log
    if ($LASTEXITCODE -ne 0) {
        Select-String -Path $log -Pattern '^FAIL' | ForEach-Object { Write-Host $_.Line }
        throw "tests failed; the full output is in $log"
    }
    Get-Content $log -Tail 1
}

Write-Host "== Packages"
$stage = Join-Path $build "stage"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item $exe $stage
Copy-Item (Join-Path $root "LICENSE") (Join-Path $stage "LICENSE.txt")
Copy-Item (Join-Path $build "_deps\lame-src\COPYING") (Join-Path $stage "LAME-LICENSE.txt")
Copy-Item (Join-Path $build "_deps\signalsmith_stretch-src\LICENSE.txt") (Join-Path $stage "SIGNALSMITH-STRETCH-LICENSE.txt")
Copy-Item (Join-Path $build "_deps\signalsmith_linear-src\LICENSE.txt") (Join-Path $stage "SIGNALSMITH-LINEAR-LICENSE.txt")
New-Item -ItemType Directory -Force $dist | Out-Null

function Write-Sha256 ([string] $file) {
    $hash = (Get-FileHash -Algorithm SHA256 $file).Hash.ToLowerInvariant()
    $name = Split-Path -Leaf $file
    [System.IO.File]::WriteAllText("$file.sha256", "$hash  $name`n")
}

$zip = Join-Path $dist "SpectraBlade-$version-windows-x64.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $zip
Write-Sha256 $zip
$made = @($zip)

if (-not $NoInstaller) {
    $iscc = @($env:ISCC, (Get-Command ISCC.exe -ErrorAction SilentlyContinue).Source,
              (Join-Path $env:LOCALAPPDATA "Programs\Inno Setup 6\ISCC.exe"),
              (Join-Path ${env:ProgramFiles(x86)} "Inno Setup 6\ISCC.exe"),
              (Join-Path $env:ProgramFiles "Inno Setup 6\ISCC.exe")) |
            Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
    if (-not $iscc) { throw "Inno Setup 6 (ISCC.exe) not found: install it, or pass -NoInstaller" }

    $icon = Join-Path $build "SpectraBlade_artefacts\JuceLibraryCode\icon.ico"
    & $iscc /Q "/DAppVersion=$version" "/DStage=$stage" "/DIconFile=$icon" "/DOutputDir=$dist" (Join-Path $PSScriptRoot "SpectraBlade.iss")
    if ($LASTEXITCODE -ne 0) { throw "the installer failed to build" }
    $setup = Join-Path $dist "SpectraBlade-$version-windows-x64-setup.exe"
    Write-Sha256 $setup
    $made += $setup
}

Write-Host "== Done"
foreach ($file in $made) {
    Get-Item $file | Format-Table Name, Length -HideTableHeaders | Out-String | Write-Host
    Get-Content "$file.sha256"
}
