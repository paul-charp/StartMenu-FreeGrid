# Generates res\StartGrid.ico: 2x2 rounded squares, sizes 16 to 256 (PNG frames).
Add-Type -AssemblyName System.Drawing
$out = Join-Path $PSScriptRoot '..\res\StartGrid.ico'
New-Item -ItemType Directory -Force (Split-Path $out) | Out-Null
$sizes = 16, 24, 32, 48, 256

function New-Frame([int]$size) {
    $bmp = New-Object System.Drawing.Bitmap $size, $size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $g.Clear([System.Drawing.Color]::Transparent)
    $pad = $size * 0.12
    $gap = $size * 0.08
    $cell = ($size - 2 * $pad - $gap) / 2
    $r = [Math]::Max(1.0, $cell * 0.28)
    $brush = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 0, 120, 212))
    foreach ($row in 0, 1) {
        foreach ($col in 0, 1) {
            $x = $pad + $col * ($cell + $gap)
            $y = $pad + $row * ($cell + $gap)
            $p = New-Object System.Drawing.Drawing2D.GraphicsPath
            $d = 2 * $r
            $p.AddArc($x, $y, $d, $d, 180, 90)
            $p.AddArc($x + $cell - $d, $y, $d, $d, 270, 90)
            $p.AddArc($x + $cell - $d, $y + $cell - $d, $d, $d, 0, 90)
            $p.AddArc($x, $y + $cell - $d, $d, $d, 90, 90)
            $p.CloseFigure()
            $g.FillPath($brush, $p)
            $p.Dispose()
        }
    }
    $g.Dispose()
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    , $ms.ToArray()
}

$frames = foreach ($s in $sizes) { , (New-Frame $s) }
$ms = New-Object System.IO.MemoryStream
$w = New-Object System.IO.BinaryWriter $ms
$w.Write([uint16]0); $w.Write([uint16]1); $w.Write([uint16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $s = $sizes[$i]
    $dim = if ($s -ge 256) { 0 } else { $s }
    $w.Write([byte]$dim); $w.Write([byte]$dim); $w.Write([byte]0); $w.Write([byte]0)
    $w.Write([uint16]1); $w.Write([uint16]32)
    $w.Write([uint32]$frames[$i].Length); $w.Write([uint32]$offset)
    $offset += $frames[$i].Length
}
foreach ($f in $frames) { $w.Write([byte[]]$f) }
$w.Flush()
[System.IO.File]::WriteAllBytes($out, $ms.ToArray())
