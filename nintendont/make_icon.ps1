# Placeholder Homebrew Channel icon for Kegstand's Tournament Mod (128x48 PNG).
# Kiosk look: rounded navy panel, light-blue rim, bold italic lettering with a
# drop shadow (the TOURNAMENT wordmark's font, Franklin Gothic Medium).
# Drawn at 4x and downscaled for clean antialiasing.
#   powershell -ExecutionPolicy Bypass -File nintendont/make_icon.ps1
param([string]$Out = (Join-Path $PSScriptRoot "icon.png"))
Add-Type -AssemblyName System.Drawing

$S = 4; $W = 128; $H = 48
$big = New-Object System.Drawing.Bitmap ($W * $S), ($H * $S), ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$g = [System.Drawing.Graphics]::FromImage($big)
$g.SmoothingMode = 'AntiAlias'
$g.TextRenderingHint = 'AntiAliasGridFit'
$g.Clear([System.Drawing.Color]::Transparent)

function RoundRect([float]$x, [float]$y, [float]$w, [float]$h, [float]$r) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = 2 * $r
    $p.AddArc($x, $y, $d, $d, 180, 90)
    $p.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $p.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
    $p.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $p.CloseFigure()
    return $p
}

# Panel: vertical navy gradient, light-blue rim.
$panel = RoundRect (1 * $S) (1 * $S) (($W - 2) * $S) (($H - 2) * $S) (7 * $S)
$rect = New-Object System.Drawing.RectangleF 0, 0, ($W * $S), ($H * $S)
$fill = New-Object System.Drawing.Drawing2D.LinearGradientBrush $rect,
    ([System.Drawing.Color]::FromArgb(255, 40, 56, 128)),
    ([System.Drawing.Color]::FromArgb(255, 14, 20, 56)), 90
$g.FillPath($fill, $panel)
$rim = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(255, 120, 170, 255)), (1.5 * $S)
$g.DrawPath($rim, $panel)

$family = "Franklin Gothic Medium"
$style = [System.Drawing.FontStyle]::Bold -bor [System.Drawing.FontStyle]::Italic
$fmt = New-Object System.Drawing.StringFormat
$fmt.Alignment = 'Center'
$fmt.LineAlignment = 'Center'
$shadow = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(200, 0, 0, 0))

function Text([string]$str, [float]$px, [float]$cy, [System.Drawing.Color]$c) {
    $font = New-Object System.Drawing.Font $family, ($px * $S), $style, ([System.Drawing.GraphicsUnit]::Pixel)
    $r = New-Object System.Drawing.RectangleF 0, (($cy - $px) * $S), ($W * $S), (2 * $px * $S)
    $rs = New-Object System.Drawing.RectangleF ($r.X + 1 * $S), ($r.Y + 1 * $S), $r.Width, $r.Height
    $g.DrawString($str, $font, $shadow, $rs, $fmt)
    $g.DrawString($str, $font, (New-Object System.Drawing.SolidBrush $c), $r, $fmt)
}

Text "KEGSTAND'S" 9.5 10.5 ([System.Drawing.Color]::FromArgb(255, 150, 190, 255))
Text "TOURNAMENT" 17 25 ([System.Drawing.Color]::White)
Text "MOD" 9.5 39 ([System.Drawing.Color]::FromArgb(255, 255, 200, 60))

$small = New-Object System.Drawing.Bitmap $W, $H, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$gs = [System.Drawing.Graphics]::FromImage($small)
$gs.InterpolationMode = 'HighQualityBicubic'
$gs.PixelOffsetMode = 'HighQuality'
$gs.CompositingQuality = 'HighQuality'
$gs.DrawImage($big, 0, 0, $W, $H)
$small.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$gs.Dispose(); $g.Dispose(); $big.Dispose(); $small.Dispose()
Write-Output "wrote $Out ($W x $H)"
