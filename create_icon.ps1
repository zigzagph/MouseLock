Add-Type -AssemblyName System.Drawing

function New-IconBitmap {
    param([int]$size)

    $bmp = New-Object System.Drawing.Bitmap($size, $size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g   = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.Clear([System.Drawing.Color]::Transparent)

    $s = $size / 16.0

    # --- Mouse cursor arrow (points upper-left) ---
    $arrow = [System.Drawing.PointF[]]@(
        [System.Drawing.PointF]::new([float](0.5*$s), [float](0.5*$s)),   # tip
        [System.Drawing.PointF]::new([float](0.5*$s), [float](10.0*$s)),  # bottom-left
        [System.Drawing.PointF]::new([float](3.0*$s), [float](7.0*$s)),   # notch
        [System.Drawing.PointF]::new([float](5.0*$s), [float](10.5*$s)),  # tail lower-left
        [System.Drawing.PointF]::new([float](6.5*$s), [float](9.8*$s)),   # tail lower-right
        [System.Drawing.PointF]::new([float](4.5*$s), [float](6.0*$s)),   # tail upper-right
        [System.Drawing.PointF]::new([float](7.0*$s), [float](2.0*$s))    # upper-right
    )

    $whiteBrush = [System.Drawing.Brushes]::White
    $blackPen   = New-Object System.Drawing.Pen([System.Drawing.Color]::Black, [float]([Math]::Max(1.0, $s * 0.8)))

    $g.FillPolygon($whiteBrush, $arrow)
    $g.DrawPolygon($blackPen, $arrow)

    # --- Padlock (lower-right, overlapping cursor) ---
    $lx = [float](8.0 * $s)
    $ly = [float](9.0 * $s)
    $lw = [float](7.0 * $s)
    $lh = [float](5.5 * $s)

    # Shackle arc above body
    $shW = [float](4.0 * $s)
    $shH = [float](3.0 * $s)
    $shX = [float]($lx + ($lw - $shW) / 2)
    $shY = [float]($ly - $shH + $s * 0.5)

    $shacklePen = New-Object System.Drawing.Pen(
        [System.Drawing.Color]::FromArgb(160, 100, 0),
        [float]([Math]::Max(1.5, $s * 1.2))
    )
    $g.DrawArc($shacklePen, $shX, $shY, $shW, [float]($shH * 2), 180, 180)

    # Body
    $goldBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 200, 40))
    $darkColor = [System.Drawing.Color]::FromArgb(160, 100, 0)
    $darkBrush = New-Object System.Drawing.SolidBrush($darkColor)
    $bodyPen   = New-Object System.Drawing.Pen($darkColor, [float]([Math]::Max(1.0, $s * 0.7)))

    $g.FillRectangle($goldBrush, $lx, $ly, $lw, $lh)
    $g.DrawRectangle($bodyPen, $lx, $ly, $lw, $lh)

    # Keyhole — only at 32px and above (too small to see at 16)
    if ($size -ge 32) {
        $kr = [float]($s * 1.5)
        $kx = [float]($lx + $lw / 2 - $kr / 2)
        $ky = [float]($ly + $lh * 0.15)
        $g.FillEllipse($darkBrush, $kx, $ky, $kr, $kr)
        $g.FillRectangle($darkBrush,
            [float]($kx + $kr * 0.3),
            [float]($ky + $kr * 0.7),
            [float]($kr * 0.4),
            [float]($s * 1.2))
    }

    $g.Dispose()
    $blackPen.Dispose()
    $shacklePen.Dispose()
    $goldBrush.Dispose()
    $darkBrush.Dispose()
    $bodyPen.Dispose()

    return $bmp
}

$sizes   = @(16, 32, 48, 256)
$streams = [System.Collections.Generic.List[System.IO.MemoryStream]]::new()

foreach ($sz in $sizes) {
    $bmp = New-IconBitmap -size $sz
    $ms  = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    [void]$streams.Add($ms)
    $bmp.Dispose()
}

# Write ICO (PNG-in-ICO format, Vista+)
$icoPath = Join-Path $PSScriptRoot "CursorLocker.ico"
$fs = [System.IO.File]::Create($icoPath)
$bw = New-Object System.IO.BinaryWriter($fs)

$bw.Write([uint16]0)                   # reserved
$bw.Write([uint16]1)                   # type: icon
$bw.Write([uint16]$sizes.Count)        # image count

$dataOffset = [uint32](6 + 16 * $sizes.Count)

for ($i = 0; $i -lt $sizes.Count; $i++) {
    $w = if ($sizes[$i] -ge 256) { [byte]0 } else { [byte]$sizes[$i] }
    $h = if ($sizes[$i] -ge 256) { [byte]0 } else { [byte]$sizes[$i] }
    $bw.Write($w)
    $bw.Write($h)
    $bw.Write([byte]0)       # color count
    $bw.Write([byte]0)       # reserved
    $bw.Write([uint16]1)     # planes
    $bw.Write([uint16]32)    # bits per pixel
    $bw.Write([uint32]$streams[$i].Length)
    $bw.Write([uint32]$dataOffset)
    $dataOffset += [uint32]$streams[$i].Length
}

foreach ($ms in $streams) {
    $bw.Write($ms.ToArray())
    $ms.Dispose()
}

$bw.Dispose()
$fs.Dispose()
Write-Host "Created: $icoPath"
