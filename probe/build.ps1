param([string]$src, [string]$exe, [string[]]$runArgs)
$vcvars = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
cmd /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
  if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "Env:$($matches[1])" -Value $matches[2] }
}
Set-Location "C:\Users\utaaa\Documents\krisp\probe"
& cl /nologo /utf-8 /EHsc /std:c++17 /O2 $src /Fe:$exe /link user32.lib 2>&1 | Select-Object -Last 4
if ($LASTEXITCODE -ne 0) { "BUILD FAILED"; exit 1 }
"=== run ==="
& "$PWD\$exe" @runArgs
"exit=$LASTEXITCODE"
