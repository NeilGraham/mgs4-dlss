# The launcher's own icon, drawn here so that a release exe wears artwork that is this project's to give away.
# Dot-source this and call Write-LauncherIconFile.
#
#   . "$PSScriptRoot\launcher_icon.ps1"
#   Write-LauncherIconFile $icoPath
#
# A local build keeps taking the game's icon out of mgs4.exe (tools\game_icon.ps1) because it looks right beside
# the game - but that artwork is Konami's, so the exe a release carries cannot wear it. This one is drawn with
# System.Drawing at every size the exe wants: a dark rounded tile, a coarse block of pixels in the lower left, and
# a sharp chevron rising out of it to the upper right - low resolution in, upscaled out, which is the whole add-on.
# The .ico holds each size as a PNG, the same layout game_icon.ps1 writes, so both builds embed the same way.

Add-Type -AssemblyName System.Drawing
. "$PSScriptRoot\launcher_badge.ps1"

function New-LauncherIconBitmap([int]$s) {
    $bmp = New-Object System.Drawing.Bitmap $s, $s
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.Clear([System.Drawing.Color]::Transparent)

    $u = $s / 16.0                                   # one design unit; the drawing is laid out on a 16 x 16 grid
    # the tile: a rounded square in the olive-black of the game's menus
    $r = [math]::Max(1.0, 2.5 * $u)
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $w = $s - 1
    $path.AddArc(0, 0, 2 * $r, 2 * $r, 180, 90)
    $path.AddArc($w - 2 * $r, 0, 2 * $r, 2 * $r, 270, 90)
    $path.AddArc($w - 2 * $r, $w - 2 * $r, 2 * $r, 2 * $r, 0, 90)
    $path.AddArc(0, $w - 2 * $r, 2 * $r, 2 * $r, 90, 90)
    $path.CloseFigure()
    $tile = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 30, 35, 30))
    $g.FillPath($tile, $path)

    # the coarse pixels: a 3 x 3 block in the lower left, the input resolution
    $cell = 2.4 * $u
    $x0 = 2.2 * $u
    $y0 = $s - 2.2 * $u - 3 * $cell
    $dim = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 84, 98, 62))
    $mid = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 122, 140, 86))
    $gap = [math]::Max(0.5, 0.3 * $u)
    for ($row = 0; $row -lt 3; $row++) {
        for ($col = 0; $col -lt 3; $col++) {
            if ($row -eq 0 -and $col -eq 2) { continue }     # the corner the chevron leaves from
            $b = $(if (($row + $col) % 2 -eq 0) { $mid } else { $dim })
            $g.FillRectangle($b, [single]($x0 + $col * $cell), [single]($y0 + $row * $cell),
                             [single]($cell - $gap), [single]($cell - $gap))
        }
    }

    # the chevron: a bright arrow to the upper right, the output
    $bright = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 214, 230, 150))
    $ax = 13.6 * $u; $ay = 2.4 * $u                  # the arrow's tip
    $len = 5.2 * $u                                  # the two arms along the edges of the tile
    $pts = @(
        (New-Object System.Drawing.PointF ([single]$ax, [single]$ay)),
        (New-Object System.Drawing.PointF ([single]($ax - $len), [single]$ay)),
        (New-Object System.Drawing.PointF ([single]($ax - $len), [single]($ay + 1.7 * $u))),
        (New-Object System.Drawing.PointF ([single]($ax - 1.7 * $u), [single]($ay + 1.7 * $u))),
        (New-Object System.Drawing.PointF ([single]($ax - 1.7 * $u), [single]($ay + $len))),
        (New-Object System.Drawing.PointF ([single]$ax, [single]($ay + $len)))
    )
    $g.FillPolygon($bright, $pts)
    # the shaft, from the missing corner of the block up to the arrow head
    $pen = New-Object System.Drawing.Pen $bright, ([single](1.6 * $u))
    $pen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
    $pen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
    $g.DrawLine($pen, [single]($x0 + 2.5 * $cell), [single]($y0 + 0.5 * $cell), [single]($ax - 1.4 * $u), [single]($ay + 1.4 * $u))

    $pen.Dispose(); $bright.Dispose(); $mid.Dispose(); $dim.Dispose(); $tile.Dispose(); $path.Dispose(); $g.Dispose()
    return $bmp
}

function Write-LauncherIconFile([string]$icoPath) {
    $sizes = @(16, 24, 32, 48, 64, 128, 256)
    $blobs = @()
    foreach ($s in $sizes) {
        $bmp = New-LauncherIconBitmap $s
        # The same play badge the local build wears, so the two exes read as one app over different art. The tile
        # is drawn rounded already; Add-LauncherBadge rounds it again to the same radius, which changes nothing.
        $badged = Add-LauncherBadge $bmp
        $ms = New-Object System.IO.MemoryStream
        $badged.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
        $blobs += , @{ Size = $s; Bytes = $ms.ToArray() }
        $ms.Dispose(); $badged.Dispose(); $bmp.Dispose()
    }
    $fs = [System.IO.File]::Create($icoPath)
    $bw = New-Object System.IO.BinaryWriter $fs
    try {
        $bw.Write([uint16]0); $bw.Write([uint16]1); $bw.Write([uint16]$blobs.Count)   # ICONDIR
        $offset = 6 + 16 * $blobs.Count
        foreach ($b in $blobs) {
            $dim = $(if ($b.Size -ge 256) { 0 } else { $b.Size })                      # 0 means 256
            $bw.Write([byte]$dim); $bw.Write([byte]$dim)
            $bw.Write([byte]0); $bw.Write([byte]0)
            $bw.Write([uint16]1); $bw.Write([uint16]32)
            $bw.Write([uint32]$b.Bytes.Length); $bw.Write([uint32]$offset)
            $offset += $b.Bytes.Length
        }
        foreach ($b in $blobs) { $bw.Write($b.Bytes) }
    } finally { $bw.Dispose(); $fs.Dispose() }
    return $true
}
