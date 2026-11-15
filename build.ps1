<#
.SYNOPSIS
    Baut und testet Compressor NEXT und erzeugt das Setup in "dist".

.DESCRIPTION
    1. Natives Audio-Modul, Hilfsprogramm/Dienst und Tests (CMake + Visual Studio 2022, C++-Arbeitslast)
    2. Tests: Maximierer (maximizer_test), Equalizer (equalizer_test) und Audio-Modul wie von der Audio-Engine aufgerufen (apo_host_test)
    3. Bedienoberfläche (dotnet publish, .NET 10)
    4. dist\CompressorNext-Setup-<Version>.exe (Inno Setup 6)
#>
param([switch]$SkipTests)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$version = ([xml](Get-Content (Join-Path $root 'Directory.Build.props'))).Project.PropertyGroup.Version
$dotnet = Get-Command dotnet -ErrorAction SilentlyContinue
$dotnet = if ($dotnet) { $dotnet.Source } else { Join-Path $env:ProgramFiles 'dotnet\dotnet.exe' }

# --- 1. Nativ -----------------------------------------------------------------------------------
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { throw 'Visual Studio 2022 mit C++-Arbeitslast nicht gefunden.' }
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$cmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
if (-not (Test-Path $cmake)) { throw 'CMake aus Visual Studio nicht gefunden (Komponente "C++-CMake-Tools für Windows").' }

$native = Join-Path $root 'build\native'
& $cmake -S (Join-Path $root 'src\native') -B $native -G 'Visual Studio 17 2022' -A x64 "-DCNX_VERSION=$version" | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'CMake-Konfiguration fehlgeschlagen.' }
& $cmake --build $native --config Release -- /nologo /v:m
if ($LASTEXITCODE -ne 0) { throw 'Nativer Build fehlgeschlagen.' }
$bin = Join-Path $native 'Release'

# --- 2. Tests -----------------------------------------------------------------------------------
if (-not $SkipTests) {
    & (Join-Path $bin 'maximizer_test.exe')
    if ($LASTEXITCODE -ne 0) { throw 'Maximierer-Tests fehlgeschlagen.' }
    & (Join-Path $bin 'equalizer_test.exe')
    if ($LASTEXITCODE -ne 0) { throw 'Equalizer-Tests fehlgeschlagen.' }

    # Das Audio-Modul sucht seine Zustandsdatei unter %ProgramData%; für den Test umlenken.
    $testData = Join-Path $root 'build\testdata'
    Remove-Item $testData -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force (Join-Path $testData 'CompressorNext') | Out-Null
    $saved = $env:ProgramData
    try {
        $env:ProgramData = Join-Path $testData 'leer'
        & (Join-Path $bin 'apo_host_test.exe') (Join-Path $bin 'CompressorNextApo.dll')
        if ($LASTEXITCODE -ne 0) { throw 'APO-Test (ohne Zustandsdatei) fehlgeschlagen.' }
        $env:ProgramData = $testData
        & (Join-Path $bin 'apo_host_test.exe') (Join-Path $bin 'CompressorNextApo.dll') (Join-Path $testData 'CompressorNext\state.bin')
        if ($LASTEXITCODE -ne 0) { throw 'APO-Test (mit Zustandsdatei) fehlgeschlagen.' }
    } finally {
        $env:ProgramData = $saved
    }
}

# --- 3. Bedienoberfläche ------------------------------------------------------------------------
$project = Join-Path $root 'src\CompressorNext\CompressorNext.csproj'
$icon = Join-Path $root 'src\CompressorNext\app.ico'
if (-not (Test-Path $icon)) {
    # Das Symbol zeichnet das Programm selbst: einmal ohne Symbol bauen, Symbol erzeugen.
    & $dotnet build $project -c Release -nologo -v q
    if ($LASTEXITCODE -ne 0) { throw 'Build der Oberfläche fehlgeschlagen.' }
    $exe = Join-Path $root 'src\CompressorNext\bin\Release\net10.0-windows\win-x64\CompressorNext.exe'
    Start-Process $exe -ArgumentList '--make-icon', "`"$icon`"" -Wait
    if (-not (Test-Path $icon)) { throw 'Programmsymbol konnte nicht erzeugt werden.' }
}

$publish = Join-Path $root 'publish'
$dist = Join-Path $root 'dist'
Remove-Item $publish -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $dist | Out-Null
$app = Join-Path $publish 'app'
& $dotnet publish $project -c Release -r win-x64 --self-contained false -o $app -p:DebugType=none -nologo -v q
if ($LASTEXITCODE -ne 0) { throw 'Publish fehlgeschlagen.' }
Copy-Item (Join-Path $bin 'CompressorNextApo.dll'), (Join-Path $bin 'CompressorNextSvc.exe'), (Join-Path $root 'README.md'), (Join-Path $root 'LICENSE.md') $app

# --- 4. Setup -----------------------------------------------------------------------------------
$iscc = @(
    (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe'),
    (Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe'),
    (Join-Path $env:ProgramFiles 'Inno Setup 6\ISCC.exe')
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $iscc) { throw 'Inno Setup 6 (ISCC.exe) nicht gefunden: winget install JRSoftware.InnoSetup' }
& $iscc /Q "/DAppVersion=$version" (Join-Path $root 'installer\CompressorNext.iss')
if ($LASTEXITCODE -ne 0) { throw 'Setup-Erstellung fehlgeschlagen.' }
# Diagnose-Werkzeug zum Mitgeben (liest nur aus, ohne Adminrechte)
Copy-Item (Join-Path $root 'tools\CompressorNext-Diagnose.cmd'), (Join-Path $root 'tools\CompressorNext-Untersuchung.cmd') $dist -Force

Write-Host ''
Write-Host 'Fertig:'
Get-ChildItem $dist | ForEach-Object { Write-Host ("  {0}  ({1:N1} MB)" -f $_.FullName, ($_.Length / 1MB)) }
