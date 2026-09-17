# ============================================================
#  ShieldCord — make_artwork.ps1
#
#  Emits the two pieces of artwork the product needs, both drawn from the SAME
#  geometry as the app's own mark (src/ui/Controls/AppMark.xaml):
#
#      installer/shieldcord.ico              the application icon — used for
#                                            shieldcordui.exe (so File Explorer
#                                            and the taskbar show it) and for
#                                            Setup.exe and its uninstaller
#
#      installer/wizard-small-image.bmp      the 55x55 shown top-right of the
#                                            setup wizard, matching that icon
#
#  There is deliberately NO tall wizard image. Inno's sidebar picture was
#  replaced with the product mark once and it read as decoration bolted onto
#  somebody else's installer; the icon in the corner is the branding.
#
#  Generated rather than committed: a bitmap and a XAML file that must agree
#  will eventually stop agreeing, and deriving one from the other is the only
#  thing that keeps them together.
#
#  Usage:  powershell -ExecutionPolicy Bypass -File tools\make_artwork.ps1
# ============================================================
[CmdletBinding()]
param(
    # Empty rather than a $PSScriptRoot default: Windows PowerShell does not
    # populate $PSScriptRoot while binding parameters, so a default there is an
    # empty string and Join-Path refuses it.
    [string]$OutDir = ''
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

if ([string]::IsNullOrWhiteSpace($OutDir)) {
    $OutDir = Join-Path (Split-Path $MyInvocation.MyCommand.Path -Parent) '..\installer'
}

# ── palette, taken from src/ui/Themes/Light.xaml so the icon matches the app ──
$Accent      = [System.Drawing.ColorTranslator]::FromHtml('#2C6FD1')
$AccentSoft  = [System.Drawing.ColorTranslator]::FromHtml('#E8F0FC')
$TextInverse = [System.Drawing.ColorTranslator]::FromHtml('#FFFFFF')

function New-RoundedRect {
    param([single]$X, [single]$Y, [single]$W, [single]$H, [single]$R)

    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = $R * 2
    $path.AddArc($X,           $Y,           $d, $d, 180, 90)
    $path.AddArc($X + $W - $d, $Y,           $d, $d, 270, 90)
    $path.AddArc($X + $W - $d, $Y + $H - $d, $d, $d,   0, 90)
    $path.AddArc($X,           $Y + $H - $d, $d, $d,  90, 90)
    $path.CloseFigure()
    return $path
}

# Coordinates are mapped EXPLICITLY — origin + unit * scale — rather than through
# a Matrix.
#
# A Matrix.Scale followed by Matrix.Translate looked right and was not: the
# translate ends up scaled by the same factor, so the offset shrinks with the
# mark. At one scale that was a ~3px shift nobody would see; at another it moved
# the shield off the tile and onto white — a mark that renders perfectly and is
# invisible. Mapping each point by hand cannot be wrong that way.
function Convert-Point {
    param([single]$OriginX, [single]$OriginY, [single]$S, [single]$U, [single]$V)
    return @(($OriginX + $U * $S), ($OriginY + $V * $S))
}

# The shield, traced from the Path Data in AppMark.xaml:
#   M12 1 L21 4.5 V11 C21 17 17.2 21.4 12 23 C6.8 21.4 3 17 3 11 V4.5 Z
function New-ShieldPath {
    param([single]$X, [single]$Y, [single]$S)

    $ox = $X + 38 * $S
    $oy = $Y + 39 * $S

    $a = Convert-Point $ox $oy $S 12   1
    $b = Convert-Point $ox $oy $S 21   4.5
    $c = Convert-Point $ox $oy $S 21   11
    $d = Convert-Point $ox $oy $S 21   17
    $e = Convert-Point $ox $oy $S 17.2 21.4
    $f = Convert-Point $ox $oy $S 12   23
    $g = Convert-Point $ox $oy $S 6.8  21.4
    $h = Convert-Point $ox $oy $S 3    17
    $i = Convert-Point $ox $oy $S 3    11
    $j = Convert-Point $ox $oy $S 3    4.5

    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $p.AddLine($a[0], $a[1], $b[0], $b[1])
    $p.AddLine($b[0], $b[1], $c[0], $c[1])
    $p.AddBezier($c[0], $c[1], $d[0], $d[1], $e[0], $e[1], $f[0], $f[1])
    $p.AddBezier($f[0], $f[1], $g[0], $g[1], $h[0], $h[1], $i[0], $i[1])
    $p.AddLine($i[0], $i[1], $j[0], $j[1])
    $p.CloseFigure()
    return $p
}

# The tick: M8.6 11.7 L11 14.1 L15.7 9.4 — stroked, not filled.
function New-TickPath {
    param([single]$X, [single]$Y, [single]$S)

    $ox = $X + 38 * $S
    $oy = $Y + 39 * $S

    $a = Convert-Point $ox $oy $S 8.6  11.7
    $b = Convert-Point $ox $oy $S 11   14.1
    $c = Convert-Point $ox $oy $S 15.7 9.4

    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $p.AddLine($a[0], $a[1], $b[0], $b[1])
    $p.AddLine($b[0], $b[1], $c[0], $c[1])
    return $p
}

<#
  Draw the mark into a box whose top-left is ($X,$Y) and whose edge is $Size.

  -Backdrop controls the soft accent circle behind the tile. It is drawn in the
  app, where the mark sits in a card, and skipped in the icon, where it would be
  a pale ring competing with the tile at every size.
#>
function Draw-Mark {
    param(
        [System.Drawing.Graphics]$G,
        [single]$X, [single]$Y, [single]$Size,
        [switch]$Backdrop
    )

    $s = $Size / 100.0

    if ($Backdrop) {
        $soft = New-Object System.Drawing.SolidBrush $AccentSoft
        $G.FillEllipse($soft, $X + 10 * $s, $Y + 10 * $s, 80 * $s, 80 * $s)
        $soft.Dispose()
    }

    $tile = New-RoundedRect ($X + 25 * $s) ($Y + 25 * $s) (50 * $s) (50 * $s) (14 * $s)
    $tileBrush = New-Object System.Drawing.SolidBrush $Accent
    $G.FillPath($tileBrush, $tile)
    $tileBrush.Dispose()
    $tile.Dispose()

    $shield = New-ShieldPath $X $Y $s
    $shieldBrush = New-Object System.Drawing.SolidBrush $TextInverse
    $G.FillPath($shieldBrush, $shield)
    $shieldBrush.Dispose()
    $shield.Dispose()

    $tick = New-TickPath $X $Y $s
    $pen = New-Object System.Drawing.Pen $Accent, (2.4 * $s)
    $pen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
    $pen.EndCap   = [System.Drawing.Drawing2D.LineCap]::Round
    $pen.LineJoin = [System.Drawing.Drawing2D.LineJoin]::Round
    $G.DrawPath($pen, $tick)
    $pen.Dispose()
    $tick.Dispose()
}

<#
  Draw the mark so its TILE fills a given fraction of a square canvas.

  Drawing "the mark" at N pixels does not give an N-pixel logo. The mark is a
  100-unit box and the tile is only the middle 50 of it — 25 units of margin on
  every side — so a 55px image got a 22px tile and read as a small blue speck
  with a lot of white around it.

  This solves for the scale and origin that put the tile at the size asked for:

      tile = Fill * Canvas            (the tile is 50 units, so scale = tile/50)
      origin = (Canvas - tile) / 2 - 25 * scale

  The mark's outer margin then falls outside the bitmap, which is exactly what
  is wanted: it is empty space either way.
#>
function Draw-MarkFilled {
    param(
        [System.Drawing.Graphics]$G,
        [single]$Canvas,
        [single]$Fill
    )

    $s = ($Fill * $Canvas) / 50.0
    $origin = ($Canvas * (1 - $Fill)) / 2 - 25 * $s

    Draw-Mark $G $origin $origin (100 * $s)
}

<#
  Render the icon at one size, with a transparent background.

  The mark occupies the middle 72% so the tile has the breathing room an
  Explorer thumbnail needs — a tile butted against the edge reads as clipped
  rather than as a designed icon.
#>
function New-IconBitmap {
    param([int]$Size)

    $bmp = New-Object System.Drawing.Bitmap $Size, $Size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.Clear([System.Drawing.Color]::Transparent)

    Draw-MarkFilled $g $Size 0.84

    $g.Dispose()
    return $bmp
}

<#
  Render one entry as a DIB — BITMAPINFOHEADER, then the XOR bitmap, then the
  AND mask.

  DIB rather than PNG, and that is a compatibility choice rather than a
  preference. PNG-compressed entries are legal and Windows Explorer renders them
  happily, but .NET's own Icon class cannot read them above 32x32 — measured:
  16/24/32 load, 48/64/128/256 all throw. Since the icon has to survive being
  embedded by the .NET build and read back, the form everything can read wins
  over the form that is a few kilobytes smaller.

  Both bitmaps are BOTTOM-UP, which is the DIB convention and the classic reason
  a hand-built icon comes out upside down.
#>
function Get-DibBytes {
    param([System.Drawing.Bitmap]$Bitmap)

    $w = $Bitmap.Width
    $h = $Bitmap.Height

    $rect = New-Object System.Drawing.Rectangle 0, 0, $w, $h
    $locked = $Bitmap.LockBits($rect,
        [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
        [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)

    # Format32bppArgb is BGRA in memory, which is already the byte order a DIB
    # wants — no channel shuffling needed.
    $stride = $locked.Stride
    $pixels = New-Object byte[] ($stride * $h)
    [System.Runtime.InteropServices.Marshal]::Copy($locked.Scan0, $pixels, 0, $pixels.Length)
    $Bitmap.UnlockBits($locked)

    $ms = New-Object System.IO.MemoryStream
    $bw = New-Object System.IO.BinaryWriter $ms

    # BITMAPINFOHEADER — height is doubled because the AND mask is stacked under
    # the colour data.
    $bw.Write([uint32]40)
    $bw.Write([int32]$w)
    $bw.Write([int32]($h * 2))
    $bw.Write([uint16]1)
    $bw.Write([uint16]32)
    $bw.Write([uint32]0)                        # BI_RGB
    $bw.Write([uint32]($w * $h * 4))
    $bw.Write([int32]0); $bw.Write([int32]0)
    $bw.Write([uint32]0); $bw.Write([uint32]0)

    for ($y = $h - 1; $y -ge 0; $y--) {
        $bw.Write($pixels, $y * $stride, $w * 4)
    }

    # AND mask: all zero, meaning "draw everything" — with a real alpha channel
    # this is what Windows uses anyway. It must be present and correctly sized
    # even though its contents are ignored.
    $maskStride = [int]([math]::Floor(($w + 31) / 32) * 4)
    $mask = New-Object byte[] ($maskStride * $h)
    $bw.Write($mask, 0, $mask.Length)

    $bw.Flush()
    $result = $ms.ToArray()
    $bw.Dispose(); $ms.Dispose()

    # The unary comma is load-bearing. Returning a byte[] straight out of a
    # PowerShell function sends it through the pipeline, where it is unrolled and
    # recollected as an Object[] — and BinaryWriter has no Write(Object[])
    # overload, so it silently writes NOTHING. The file comes out as a valid
    # header with seven directory entries and zero bytes of image data.
    return ,$result
}

<#
  Assemble a multi-size .ico.

  Every size Windows actually asks for is present: Explorer picks 16 for the
  details list, 32 and 48 for icons and tiles, and 256 for extra-large.
#>
function Write-Icon {
    param([string]$Path, [int[]]$Sizes)

    $entries = @()
    foreach ($size in $Sizes) {
        $bmp = New-IconBitmap $size
        $entries += ,@{ Size = $size; Bytes = (Get-DibBytes $bmp) }
        $bmp.Dispose()
    }

    $out = New-Object System.IO.MemoryStream
    $w = New-Object System.IO.BinaryWriter $out

    # ICONDIR
    $w.Write([uint16]0)
    $w.Write([uint16]1)                 # 1 = icon
    $w.Write([uint16]$entries.Count)

    $offset = 6 + 16 * $entries.Count

    # ICONDIRENTRY per image
    foreach ($e in $entries) {
        $dim = if ($e.Size -ge 256) { 0 } else { $e.Size }   # 0 means 256
        $w.Write([byte]$dim)
        $w.Write([byte]$dim)
        $w.Write([byte]0)               # palette count
        $w.Write([byte]0)               # reserved
        $w.Write([uint16]1)             # colour planes
        $w.Write([uint16]32)            # bits per pixel
        $w.Write([uint32]$e.Bytes.Length)
        $w.Write([uint32]$offset)
        $offset += $e.Bytes.Length
    }

    foreach ($e in $entries) { $w.Write([byte[]]$e.Bytes) }

    $w.Flush()
    [System.IO.File]::WriteAllBytes($Path, $out.ToArray())
    $w.Dispose(); $out.Dispose()
}

# ── write them out ───────────────────────────────────────────
if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Force $OutDir | Out-Null }

$icoPath = Join-Path $OutDir 'shieldcord.ico'
Write-Icon $icoPath @(16, 24, 32, 48, 64, 128, 256)

# The wizard's corner image sits on the wizard's own white background, so it gets
# a white ground rather than transparency — and the tile fills 0.82 of the 55px
# box, which is as large as it can be without the rounded corners touching the
# edge.
$smallPath = Join-Path $OutDir 'wizard-small-image.bmp'
$bmp = New-Object System.Drawing.Bitmap 55, 55, ([System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
$g.Clear([System.Drawing.Color]::White)
Draw-MarkFilled $g 55 0.82
$g.Dispose()
$bmp.Save($smallPath, [System.Drawing.Imaging.ImageFormat]::Bmp)
$bmp.Dispose()

Write-Host ("  [+] {0}  ({1} sizes: {2})" -f 'shieldcord.ico', 7, '16-256') -ForegroundColor Green
$img = [System.Drawing.Image]::FromFile($smallPath)
Write-Host ("  [+] {0}  {1}x{2}" -f 'wizard-small-image.bmp', $img.Width, $img.Height) -ForegroundColor Green
$img.Dispose()
