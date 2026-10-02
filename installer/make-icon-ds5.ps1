# Generates app\ctm-usbip.ico with the DS5-USBIP treatment: a BLACK bridge on
# the original blue backdrop.
#
# ---------------------------------------------------------------------------
# WHY THIS SITS BESIDE make-icon.ps1 INSTEAD OF REPLACING IT.
#
# make-icon.ps1 and installer\brand.png are upstream's, and brand.png is the
# 1024px master the art actually lives in. Editing either would put our change
# in the middle of a file upstream also edits, for no gain -- so brand.png stays
# EXACTLY as Ciprian drew it and the recolour happens here, in code, where it
# can be read and re-run.
#
# ⛔ THE FAULT THIS EXISTS TO PREVENT (2026-09-28): the .ico was first built by
# hand from extracted PNGs, which left the repo in a state where anyone running
# make-icon.ps1 -- exactly as the README tells them to "if the brand art
# changed" -- would silently revert the icon with no error and no diff anyone
# would read. A generated file must have a generator.
#
# ✅ AND brand.png REALLY IS AUTHORITATIVE, checked rather than assumed:
# running upstream's own make-icon.ps1 over it reproduces the committed
# pre-recolour .ico BYTE FOR BYTE (156,019 bytes, same SHA-256). So the
# master and the shipped icon had NOT drifted, and this script inherits a
# sound pipeline rather than papering over a broken one.
# ⚠️ A first reading of the two side by side suggested they HAD drifted --
# the master looks full-bleed and the icon looks vignetted. That was the
# recolour changing what is visible, not a different crop: take the glow
# away and the structure dominates. Compare bytes, not impressions.
#
# ➡️ RUN THIS, NOT make-icon.ps1, whenever the art or the treatment changes.
# ➡️ RUN IT WITH -Check to ask whether the committed icon is still its own.
# ---------------------------------------------------------------------------
#
# THE TREATMENT, and why it is not a plain inversion.
#
# A flat RGB inversion turns the navy backdrop peach. What was wanted was the
# bridge dark and the backdrop untouched, so each pixel is darkened by HOW
# BRIGHT IT ALREADY IS: the navy stays navy, the white bridge goes black, and
# the glow keeps its own hue rather than turning brown -- which is what
# blending toward the inverse would do, the inverse of a light blue being a
# light brown.
#
# Small sizes get a threshold instead of a ramp. A ramp darkens the BLOOM
# nearly as much as the bridge, so the silhouette sits in a haze and reads
# soft; below 64px there are not enough pixels to survive that. Thresholding
# leaves the glow alone as backdrop and takes only the structure to black, so
# the bridge has something bright to read against. Smoothstep rather than a
# hard cut, because at 16 and 24px a cable is thinner than one pixel and
# exists only as a partial value.

# ⭐ -Check WRITES NOTHING. It builds the icon in memory and compares it with
# the committed one, byte for byte, and exits 1 if they differ. That is the
# whole of "a generated file must have a generator": the claim is only true
# while this passes.
param([switch]$Check)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$src = Join-Path $PSScriptRoot 'brand.png'
$dst = Join-Path $PSScriptRoot '..\app\ctm-usbip.ico'

# ⭐⭐ ONE FRAME FOR EVERY SIZE WINDOWS ASKS FOR, at every stock display scale
# from 100% to 300% (rhoquinn8217, 2026-10-01: *"use a higher resolution
# icon. the one in the task bar looks blurry."*). Windows has a small icon,
# 16 units, and a big one, 32 units, and a unit is one pixel at 100%:
#
#     scale    100  125  150  175  200  225  250  300
#     small     16   20   24   28   32   36   40   48
#     big       32   40   48   56   64   72   80   96
#
# ⛔ A SIZE MISSING HERE IS NOT AN ERROR ANYWHERE. Windows stretches the
# nearest frame to fit and says nothing, so the icon is simply soft at that
# one scale. Until that day this list was 16, 24, 32, 48, 64, 128, 256, which
# is exact at 100, 150 and 200% and stretched at every other scale.
# ⓘ src\app\window_icon_rule.inl works the same sizes out for the settings
# window's taskbar icon, and its test carries this table. 128 and 256 are for
# Explorer's large views.
$sizes = 16, 20, 24, 28, 32, 36, 40, 48, 56, 64, 72, 80, 96, 128, 256

# Sizes at or below this get the sharpened threshold; above it, the ramp.
# ⓘ 56, not 48. The reason given above is "below 64px there are not enough
# pixels", and 56 is below 64; it read 48 only while 48 was the last frame
# before 64. No frame that existed before changes treatment.
$sharpenUpTo = 56
$thresholdLo = 0.45
$thresholdHi = 0.65

function Convert-Frame {
    param([System.Drawing.Bitmap]$Bmp, [bool]$Sharpen)

    $rect = New-Object System.Drawing.Rectangle(0, 0, $Bmp.Width, $Bmp.Height)
    $data = $Bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadWrite,
                          [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    try {
        $count = [Math]::Abs($data.Stride) * $Bmp.Height
        $buf = New-Object byte[] $count
        [System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $buf, 0, $count)

        # GDI+ 32bppArgb is B,G,R,A in memory. Getting this backwards tints the
        # whole icon and looks like a colour-space bug rather than a typo.
        $lums = New-Object System.Collections.Generic.List[int]
        for ($i = 0; $i -lt $count; $i += 4) {
            if ($buf[$i + 3] -gt 8) {
                $lums.Add([int]((299 * $buf[$i + 2] + 587 * $buf[$i + 1] + 114 * $buf[$i]) / 1000))
            }
        }
        if ($lums.Count -eq 0) { return }
        $sorted = $lums.ToArray(); [Array]::Sort($sorted)

        # A low percentile, not the minimum: one stray black pixel would set the
        # floor and leave the whole backdrop very slightly darkened.
        $floor = $sorted[[int]($sorted.Length * 0.02)]
        $span = [Math]::Max(1, 255 - $floor)

        for ($i = 0; $i -lt $count; $i += 4) {
            $b = $buf[$i]; $g = $buf[$i + 1]; $r = $buf[$i + 2]
            $L = [int]((299 * $r + 587 * $g + 114 * $b) / 1000)
            $t = ($L - $floor) / $span
            if ($t -lt 0) { $t = 0.0 } elseif ($t -gt 1) { $t = 1.0 }

            if ($Sharpen) {
                if ($t -le $thresholdLo) { $f = 0.0 }
                elseif ($t -ge $thresholdHi) { $f = 1.0 }
                else {
                    $u = ($t - $thresholdLo) / ($thresholdHi - $thresholdLo)
                    $f = $u * $u * (3.0 - 2.0 * $u)
                }
            } else {
                $f = $t
            }

            $k = 1.0 - $f
            $buf[$i]     = [byte][Math]::Max(0, [Math]::Min(255, [int]($b * $k + 0.5)))
            $buf[$i + 1] = [byte][Math]::Max(0, [Math]::Min(255, [int]($g * $k + 0.5)))
            $buf[$i + 2] = [byte][Math]::Max(0, [Math]::Min(255, [int]($r * $k + 0.5)))
            # alpha untouched, or the soft edges square off
        }
        [System.Runtime.InteropServices.Marshal]::Copy($buf, 0, $data.Scan0, $count)
    }
    finally { $Bmp.UnlockBits($data) }
}

$image = [System.Drawing.Image]::FromFile((Resolve-Path $src).Path)
$frames = @()
foreach ($s in $sizes) {
    # Scale FIRST, then treat. Thresholding the 1024px master and scaling down
    # afterwards would average black structure with bright glow and give back
    # exactly the grey mush the threshold exists to avoid.
    $bmp = New-Object System.Drawing.Bitmap($s, $s, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.Clear([System.Drawing.Color]::Transparent)
    $g.DrawImage($image, (New-Object System.Drawing.Rectangle(0, 0, $s, $s)))
    $g.Dispose()

    Convert-Frame -Bmp $bmp -Sharpen ($s -le $sharpenUpTo)

    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    $frames += , ($ms.ToArray())
    $ms.Dispose()
    Write-Host ("  {0,3}x{1,-3} {2,7} bytes  {3}" -f $s, $s, $frames[-1].Length,
                $(if ($s -le $sharpenUpTo) { 'sharpened' } else { 'ramp' }))
}
$image.Dispose()

$out = New-Object System.IO.MemoryStream
$bw = New-Object System.IO.BinaryWriter($out)
$bw.Write([uint16]0)
$bw.Write([uint16]1)
$bw.Write([uint16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $s = $sizes[$i]
    $dim = if ($s -ge 256) { 0 } else { $s }
    $bw.Write([byte]$dim)
    $bw.Write([byte]$dim)
    $bw.Write([byte]0)
    $bw.Write([byte]0)
    $bw.Write([uint16]1)
    $bw.Write([uint16]32)
    $bw.Write([uint32]$frames[$i].Length)
    $bw.Write([uint32]$offset)
    $offset += $frames[$i].Length
}
foreach ($f in $frames) { $bw.Write($f) }
$bw.Flush()
$bytes = $out.ToArray()
$out.Dispose()

if ($Check) {
    $have = [System.IO.File]::ReadAllBytes((Resolve-Path $dst).Path)
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $made = [System.BitConverter]::ToString($sha.ComputeHash($bytes))
    $kept = [System.BitConverter]::ToString($sha.ComputeHash($have))
    $sha.Dispose()
    if ($made -eq $kept) {
        Write-Host "OK: the committed icon is what this script makes ($($bytes.Length) bytes, $($sizes.Count) sizes)"
        exit 0
    }
    Write-Host "DRIFT: the committed icon is NOT what this script makes"
    Write-Host "  committed  $($have.Length) bytes"
    Write-Host "  this script $($bytes.Length) bytes"
    exit 1
}

[System.IO.File]::WriteAllBytes((Join-Path $PSScriptRoot '..\app\ctm-usbip.ico'), $bytes)
Write-Host "Wrote $((Resolve-Path $dst).Path) ($($sizes.Count) sizes, DS5 treatment)"
