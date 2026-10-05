# Generates assets/icon.ico (PNG-compressed, 16/32/48/256) - an arrow with a fading motion trail.
Add-Type -AssemblyName System.Drawing

function New-IconPng([int]$size) {
    $bmp = New-Object System.Drawing.Bitmap $size, $size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $g.Clear([System.Drawing.Color]::Transparent)

    # rounded dark tile
    $r = $size * 0.22
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = $r * 2
    $path.AddArc(0, 0, $d, $d, 180, 90)
    $path.AddArc($size - $d - 1, 0, $d, $d, 270, 90)
    $path.AddArc($size - $d - 1, $size - $d - 1, $d, $d, 0, 90)
    $path.AddArc(0, $size - $d - 1, $d, $d, 90, 90)
    $path.CloseFigure()
    $g.FillPath((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 32, 36, 52))), $path)

    # arrow polygon in a 100x100 box, with ghost copies trailing down-right
    $pts = @(@(22,10), @(22,78), @(38,63), @(50,88), @(60,83), @(48,59), @(70,59))
    $scale = $size / 100.0
    for ($k = 3; $k -ge 0; $k--) {
        $off = $k * 7
        $alpha = if ($k -eq 0) { 255 } else { [int](110 / $k) }
        $poly = $pts | ForEach-Object { New-Object System.Drawing.PointF ((($_[0] + $off) * $scale), (($_[1] + $off) * $scale)) }
        $fill = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb($alpha, 255, 255, 255))
        $g.FillPolygon($fill, [System.Drawing.PointF[]]$poly)
        if ($k -eq 0) {
            $pen = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(255, 20, 20, 30)), ([Math]::Max(1, $size / 40))
            $pen.LineJoin = 'Round'
            $g.DrawPolygon($pen, [System.Drawing.PointF[]]$poly)
        }
    }
    $g.Dispose()
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    return ,$ms.ToArray()
}

$sizes = 16, 32, 48, 256
$images = $sizes | ForEach-Object { , (New-IconPng $_) }

$out = New-Object System.IO.MemoryStream
$w = New-Object System.IO.BinaryWriter $out
$w.Write([uint16]0); $w.Write([uint16]1); $w.Write([uint16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $s = $sizes[$i]; $data = $images[$i]
    $w.Write([byte]($s % 256)); $w.Write([byte]($s % 256)); $w.Write([byte]0); $w.Write([byte]0)
    $w.Write([uint16]1); $w.Write([uint16]32)
    $w.Write([uint32]$data.Length); $w.Write([uint32]$offset)
    $offset += $data.Length
}
foreach ($data in $images) { $w.Write($data) }
$w.Flush()

$dest = Join-Path (Split-Path $PSScriptRoot -Parent) 'assets\icon.ico'
[System.IO.File]::WriteAllBytes($dest, $out.ToArray())
"Wrote $dest"
