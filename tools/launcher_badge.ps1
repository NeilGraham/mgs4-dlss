# The mark that says "this is the launcher, not the game": the icon rounded off into a tile, with a play mark in
# the lower right. Dot-source this and call Add-LauncherBadge on a square bitmap.
#
#   . "$PSScriptRoot\launcher_badge.ps1"
#   $badged = Add-LauncherBadge $bitmap
#
# Both icon writers put it on, so the two builds wear the same mark over different art: a local build badges the
# game's own icon, which is what makes the launcher tellable from the game in a taskbar holding both, and a
# release badges the icon this project draws for itself (that artwork is Konami's, so a release cannot carry it).
#
# The mark is what a video thumbnail wears: a shade rising out of the tile's lower right corner, and a white play
# triangle sitting in it. No disc and no outline - both were tried, and a disc on a portrait reads as a sticker on
# it, where a shade reads as part of the picture. The shade is a radial gradient centred past the corner, dark at
# the corner and gone by a third of the way across, so the face and the logo on the rest of the art are untouched;
# the triangle is drawn twice, a soft dark pass a little wider under the white, so its edge holds on the busiest
# part of the collar.
#
# Sizes are the point of the rest of the file. A fraction of the tile that looks right at 48 px is a smear at 16,
# where the whole mark has eight pixels to live in, so the triangle grows as the tile shrinks and the shade goes
# with it, and at 16 the two passes are one pass on whole pixels.

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

    # The shade: a radial gradient centred a little past the corner, so the darkest point is the corner itself.
    # Its reach grows as the tile shrinks, the way the mark does, so the small sizes keep their contrast.
    $reach = $(if ($s -le 16) { 1.15 } elseif ($s -le 32) { 1.05 } else { 0.95 }) * $s
    $shade = New-Object System.Drawing.Drawing2D.GraphicsPath
    $shade.AddEllipse([single]($s + 0.12 * $s - $reach), [single]($s + 0.12 * $s - $reach), [single](2 * $reach), [single](2 * $reach))
    $brush = New-Object System.Drawing.Drawing2D.PathGradientBrush $shade
    $brush.CenterPoint = New-Object System.Drawing.PointF ([single]($s + 0.12 * $s), [single]($s + 0.12 * $s))
    $brush.CenterColor = [System.Drawing.Color]::FromArgb(225, 6, 8, 12)
    $brush.SurroundColors = @([System.Drawing.Color]::FromArgb(0, 6, 8, 12))
    # Pull the fall-off in: dark for the first third, then away. Blend positions run from the edge (0) to the centre (1).
    $blend = New-Object System.Drawing.Drawing2D.Blend 4
    $blend.Positions = [single[]]@(0.0, 0.45, 0.75, 1.0)
    $blend.Factors = [single[]]@(0.0, 0.25, 0.8, 1.0)
    $brush.Blend = $blend
    $g.FillPath($brush, $shade)
    $brush.Dispose(); $shade.Dispose()
    $g.ResetClip()

    # The play mark, sitting in the shade. A triangle centred on its bounding box reads as too far left, because
    # its mass is on the left edge; a nudge right is what makes it look centred in the corner.
    $frac = $(if ($s -le 16) { 0.50 } elseif ($s -le 24) { 0.44 } elseif ($s -le 32) { 0.40 } else { 0.34 })
    $th = $s * $frac
    $tw = $th * 0.88
    $m = $(if ($s -le 24) { 1.0 * $u } else { 1.6 * $u })       # off the tile's corner
    $cx = $s - $m - $tw / 2 + $th * 0.06
    $cy = $s - $m - $th / 2
    if ($s -le 16) { $cx = [math]::Round($cx); $cy = [math]::Round($cy); $th = [math]::Round($th); $tw = [math]::Round($tw) }
    $pts = @(
        (New-Object System.Drawing.PointF ([single]($cx - $tw / 2), [single]($cy - $th / 2))),
        (New-Object System.Drawing.PointF ([single]($cx + $tw / 2), [single]$cy)),
        (New-Object System.Drawing.PointF ([single]($cx - $tw / 2), [single]($cy + $th / 2)))
    )
    if ($s -gt 16) {
        # the soft dark pass under the white, a stroke with round joins so it reads as a shadow and not a border
        $pen = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(150, 0, 0, 0)), ([single](0.9 * $u))
        $pen.LineJoin = [System.Drawing.Drawing2D.LineJoin]::Round
        $g.DrawPolygon($pen, $pts)
        $pen.Dispose()
    }
    $white = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 255, 255, 255))
    $g.FillPolygon($white, $pts)
    if ($s -ge 32) {
        # rounded corners on the mark, where a sharp one shows as a spike
        $pen = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(255, 255, 255, 255)), ([single](0.35 * $u))
        $pen.LineJoin = [System.Drawing.Drawing2D.LineJoin]::Round
        $g.DrawPolygon($pen, $pts)
        $pen.Dispose()
    }
    $white.Dispose()

    $clip.Dispose(); $g.Dispose()
    return $out
}
