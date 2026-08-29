# res/openkrisp.png から res/openkrisp.ico を作り直す。
# 各サイズを PNG のまま ICO に格納する（Vista 以降が対応。256px を BMP で持つと
# ファイルが無駄に大きくなる）。
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing

$res = $PSScriptRoot
$png = Join-Path $res "openkrisp.png"
$ico = Join-Path $res "openkrisp.ico"
if (-not (Test-Path $png)) { throw "元画像が見つかりません: $png" }

$sizes = 16,20,24,32,40,48,64,128,256
$orig  = [System.Drawing.Bitmap]::FromFile($png)
$parts = @()
foreach ($s in $sizes) {
  $b = New-Object System.Drawing.Bitmap $s, $s
  $g = [System.Drawing.Graphics]::FromImage($b)
  $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
  $g.PixelOffsetMode   = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
  $g.SmoothingMode     = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
  $g.DrawImage($orig, 0, 0, $s, $s)
  $g.Dispose()
  $ms = New-Object System.IO.MemoryStream
  $b.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
  $parts += ,@($s, $ms.ToArray())
  $b.Dispose(); $ms.Dispose()
}
$orig.Dispose()

$out = New-Object System.IO.MemoryStream
$w = New-Object System.IO.BinaryWriter $out
$w.Write([UInt16]0); $w.Write([UInt16]1); $w.Write([UInt16]$parts.Count)  # ICONDIR
$offset = 6 + 16 * $parts.Count
foreach ($p in $parts) {
  $s = $p[0]; $bytes = $p[1]
  $dim = if ($s -ge 256) { 0 } else { $s }   # 256 は 0 で表す決まり
  $w.Write([Byte]$dim); $w.Write([Byte]$dim)
  $w.Write([Byte]0); $w.Write([Byte]0)
  $w.Write([UInt16]1); $w.Write([UInt16]32)
  $w.Write([UInt32]$bytes.Length); $w.Write([UInt32]$offset)
  $offset += $bytes.Length
}
foreach ($p in $parts) { $w.Write($p[1]) }
$w.Flush()
[System.IO.File]::WriteAllBytes($ico, $out.ToArray())
$w.Dispose(); $out.Dispose()

"=> $ico  ($((Get-Item $ico).Length) bytes / $($parts.Count) サイズ)"
