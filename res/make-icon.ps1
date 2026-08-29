# res/openkrisp.png から res/openkrisp.ico を作り直す。
#
# 各サイズは PNG ではなく従来型の DIB（BITMAPINFOHEADER + 32bpp BGRA + AND マスク）
# で格納する。PNG 圧縮の ICO は Windows 自体は読めるが、リソースコンパイラ
# （rc.exe）が "RC2176: old DIB" で撥ねるため exe に埋め込めない。
#
# 組み立てを C# に置いているのは、PowerShell 5.1 だと LockBits や byte[] の
# 生成でオーバーロードの解決に失敗するため。
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing

Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @"
using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.IO;
using System.Runtime.InteropServices;

public static class IcoBuilder {
    public static void Build(string pngPath, string icoPath, int[] sizes) {
        var parts = new byte[sizes.Length][];
        using (var orig = new Bitmap(pngPath)) {
            for (int i = 0; i < sizes.Length; i++) {
                int s = sizes[i];
                using (var b = new Bitmap(s, s, PixelFormat.Format32bppArgb)) {
                    using (var g = Graphics.FromImage(b)) {
                        g.InterpolationMode = InterpolationMode.HighQualityBicubic;
                        g.PixelOffsetMode   = PixelOffsetMode.HighQuality;
                        g.SmoothingMode     = SmoothingMode.HighQuality;
                        g.DrawImage(orig, 0, 0, s, s);
                    }
                    // Format32bppArgb はメモリ上 B,G,R,A の並びで DIB と同じ
                    var data = b.LockBits(new Rectangle(0, 0, s, s),
                                          ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
                    var px = new byte[data.Stride * s];
                    Marshal.Copy(data.Scan0, px, 0, px.Length);
                    int stride = data.Stride;
                    b.UnlockBits(data);

                    using (var ms = new MemoryStream())
                    using (var w = new BinaryWriter(ms)) {
                        // BITMAPINFOHEADER。高さは XOR と AND を重ねるので 2 倍で書く決まり
                        w.Write((uint)40); w.Write(s); w.Write(s * 2);
                        w.Write((ushort)1); w.Write((ushort)32);
                        w.Write((uint)0); w.Write((uint)0);
                        w.Write(0); w.Write(0); w.Write((uint)0); w.Write((uint)0);
                        // XOR 部。DIB は下から上へ並べるので行を逆順に
                        for (int y = s - 1; y >= 0; y--) w.Write(px, y * stride, s * 4);
                        // AND マスク。32bpp はアルファで抜くので全 0 でよい。
                        // 行は 4 バイト境界に揃える決まり
                        w.Write(new byte[(((s + 31) / 32) * 4) * s]);
                        w.Flush();
                        parts[i] = ms.ToArray();
                    }
                }
            }
        }
        using (var fs = File.Create(icoPath))
        using (var w = new BinaryWriter(fs)) {
            w.Write((ushort)0); w.Write((ushort)1); w.Write((ushort)sizes.Length);  // ICONDIR
            int offset = 6 + 16 * sizes.Length;
            for (int i = 0; i < sizes.Length; i++) {
                byte dim = (byte)(sizes[i] >= 256 ? 0 : sizes[i]);   // 256 は 0 で表す決まり
                w.Write(dim); w.Write(dim); w.Write((byte)0); w.Write((byte)0);
                w.Write((ushort)1); w.Write((ushort)32);
                w.Write((uint)parts[i].Length); w.Write((uint)offset);
                offset += parts[i].Length;
            }
            for (int i = 0; i < sizes.Length; i++) w.Write(parts[i]);
        }
    }
}
"@

$res = $PSScriptRoot
$png = Join-Path $res "openkrisp.png"
$ico = Join-Path $res "openkrisp.ico"
if (-not (Test-Path $png)) { throw "元画像が見つかりません: $png" }

[IcoBuilder]::Build($png, $ico, @(16,20,24,32,40,48,64,128,256))
"=> $ico  ($((Get-Item $ico).Length) bytes)"
