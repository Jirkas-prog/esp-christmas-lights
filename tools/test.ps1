param([ValidateSet('tdisplay', 'esp32wroom32')][string]$Environment = 'tdisplay')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio C++ build tools are required.' }
$out = Join-Path $root 'test-output'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$json = Join-Path $root ".pio\libdeps\$Environment\ArduinoJson\src"
if (-not (Test-Path $json)) { throw 'Run PlatformIO build first to install ArduinoJson.' }
$batch = Join-Path $out 'test.cmd'
@"
@echo off
chcp 65001 >nul
call "$vs\VC\Auxiliary\Build\vcvars64.bat" >nul
cl /nologo /EHsc /std:c++17 /utf-8 /I"$json" "$root\tests\core.cpp" /Fo"$out\core.obj" /Fe"$out\core.exe"
if errorlevel 1 exit /b 1
"$out\core.exe"
"@ -replace "`r?`n", "`r`n" | Set-Content -LiteralPath $batch -Encoding utf8NoBOM
& cmd /c $batch
if ($LASTEXITCODE -ne 0) { throw 'Core checks failed.' }
& node --check (Join-Path $root 'web\app.js')
if ($LASTEXITCODE -ne 0) { throw 'Web syntax check failed.' }
& node --check (Join-Path $root 'web\i18n.js')
if ($LASTEXITCODE -ne 0) { throw 'Language syntax check failed.' }
