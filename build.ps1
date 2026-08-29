# OpenKrisp のビルド。VS の x64 環境を読み込んでから CMake を回す。
#   .\build.ps1            構成 + ビルド
#   .\build.ps1 -Clean     build/ を作り直してから構成 + ビルド
param([switch]$Clean)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$vcvars = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat が見つかりません: $vcvars" }

# vcvars64 が設定する環境変数を、この PowerShell セッションへ取り込む
cmd /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
  if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "Env:$($matches[1])" -Value $matches[2] }
}

if ($Clean -and (Test-Path "$root\build")) { Remove-Item -Recurse -Force "$root\build" }

cmake -S $root -B "$root\build" -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release | Out-Null
if ($LASTEXITCODE -ne 0) { throw "cmake の構成に失敗しました" }

cmake --build "$root\build"
if ($LASTEXITCODE -ne 0) { throw "ビルドに失敗しました" }

"" ; "=> $root\build\openkrisp.exe"
