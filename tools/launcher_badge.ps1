# The mark that says "this is the launcher, not the game": the icon rounded off into a tile, with a play badge
# in the lower right. Dot-source this and call Add-LauncherBadge on a square bitmap.
#
#   . "$PSScriptRoot\launcher_badge.ps1"
#   $badged = Add-LauncherBadge $bitmap
#
# Both icon writers put it on, so the two builds wear the same mark over different art: a local build badges the
# game's own icon, which is what makes the launcher tellable from the game in a taskbar holding both, and a
# release badges the icon this project draws for itself (that artwork is Konami's, so a release cannot carry it).
#
# The badge is blue because the button it stands for is - Launch, on the Play tab.
#
# It is an outline with nothing behind it: the arrow is a stroke, its middle is the art showing through, and there
# is no disc. That leaves contrast entirely to whatever the arrow lands on, and the lower right of the game's icon
# is dark but not evenly so, so the stroke is drawn twice - a near-black pass a little wider, then the blue over
# it. The dark is a rim around the stroke rather than a fill, so the middle stays open.
#
# Sizes are the point of the rest of the file. A stroke that is a fixed fraction of the tile is a smear at 16 px,
# where the whole arrow has eight pixels to live in, so the arrow grows as the tile shrinks and the rim is dropped
# at 16 px - there the rim and the stroke would be the same pixel.

Add-Type -AssemblyName System.Drawing

function New-RoundedPath([single]$x, [single]$y, [single]$w, [single]$h, [single]$r) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = 2 * $r
    $p.AddArc($x, $y, $d, $d, 180, 90)
    $p.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $p.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
    $p.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $p.CloseFigure()
    return $p
}

function Add-LauncherBadge([System.Drawing.Bitmap]$src) {
    $s = $src.Width
    $out = New-Object System.Drawing.Bitmap $s, $s
    $g = [System.Drawing.Graphics]::FromImage($out)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.Clear([System.Drawing.Color]::Transparent)
    $u = $s / 16.0

    # the art, clipped into a rounded tile
    $r = [math]::Max(1.0, 3.0 * $u)
    $clip = New-RoundedPath 0 0 ([single]($s - 1)) ([single]($s - 1)) ([single]$r)
    $g.SetClip($clip)
    $g.DrawImage($src, 0, 0, $s, $s)
    $g.ResetClip()

    # the arrow grows as the tile shrinks: a fixed fraction that looks right at 48 px is a smudge at 16
    $frac = $(if ($s -le 16) { 0.50 } elseif ($s -le 24) { 0.46 } elseif ($s -le 32) { 0.44 } else { 0.42 })
    $size = $s * $frac
    $m = [math]::Max(0.8, 1.0 * $u)                 # how far the arrow sits off the tile's corner
    $cx = $s - $m - $size / 2
    $cy = $s - $m - $size / 2

    # A play triangle centred on its bounding box reads as sitting too far left, because its mass is on the left
    # edge; nudging it right a little is what makes it look centred on its own corner.
    $cx = $cx + $size * 0.04
    $tw = $size * 0.86
    $th = $size
    $pts = @(
        (New-Object System.Drawing.PointF ([single]($cx - $tw / 2), [single]($cy - $th / 2))),
        (New-Object System.Drawing.PointF ([single]($cx + $tw / 2), [single]$cy)),
        (New-Object System.Drawing.PointF ([single]($cx - $tw / 2), [single]($cy + $th / 2)))
    )

    $blue = [System.Drawing.Color]::FromArgb(255, 124, 156, 255)    # the window's own accent, which reads on dark
    $w = [math]::Max(1.0, $size * 0.15)

    if ($s -ge 24) {
        $rim = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(205, 8, 10, 14)), ([single]($w * 1.9))
        $rim.LineJoin = [System.Drawing.Drawing2D.LineJoin]::Round
        $rim.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
        $rim.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
        $g.DrawPolygon($rim, $pts)
        $rim.Dispose()
    }

    $pen = New-Object System.Drawing.Pen $blue, ([single]$w)
    $pen.LineJoin = [System.Drawing.Drawing2D.LineJoin]::Round
    $pen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
    $pen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
    $g.DrawPolygon($pen, $pts)
    $pen.Dispose()

    $clip.Dispose(); $g.Dispose()
    return $out
}
