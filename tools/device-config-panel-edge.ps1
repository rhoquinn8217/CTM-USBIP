# device-config-panel-edge.ps1 -- the DualSense EDGE panel.
#
# A SEPARATE FILE FROM device-config-panel.ps1, and identical to it except
# that every setting is written to the [ds5_edge] section instead of [ds5].
#
# WHY THIS EXISTS. The config engine keys sections on the USB product id --
# 0x0ce6 is "ds5", 0x0df2 is "ds5_edge" -- and a device reads ONLY its own
# section. That is deliberate, and there is a test asserting the Edge does not
# inherit from ds5.
#
# The original panel hardcodes 'ds5' in all nine places it writes a setting, so
# everything set through it landed in [ds5] and the Edge correctly ignored it.
# Measured 2026-08-11: speaker volume and audio off had no effect on an Edge,
# while audio gain appeared to work -- because the gain path reads [ds5]
# regardless of device, which is a known soft spot recorded in the project docs.
#
# KEEP THE TWO IN STEP. A change to the original belongs here too. Both are
# expected to be replaced by the web app, at which point a device
# selector makes both files unnecessary.
#
# Generated from device-config-panel.ps1 by substitution -- diff the two if
# anything looks wrong; the only differences should be the section name and
# the window title.

# Control panel for ctm-device-config.txt.
#
# Writes the config file; the agent's watcher notices the save and applies it to
# a running session. No reseat, no listener restart, and NO C++ SIDE TO THIS AT
# ALL -- the file is the entire interface.
#
# !! THERE IS NO APPLY BUTTON. Changes are written as you move a control, so a
# !! slider can be dragged while listening. Two debounces sit in the way and
# !! both are deliberate:
# !!   * this panel waits 60 ms after the last movement before writing, so a
# !!     drag produces a handful of writes rather than dozens
# !!   * the agent's watcher waits 120 ms after the last file event before
# !!     reading, which also means it never catches a half-written file
#
# PowerShell rather than an executable on purpose: real Windows controls with no
# compiler, no project file, and nothing added to the solution.
#
# !! ONLY THE KEYS THIS PANEL MANAGES ARE REWRITTEN. Comments, section headers,
# !! layout and any key it does not know about are preserved exactly. Someone
# !! who hand-edits the file must not lose that work by opening this.

[CmdletBinding()]
param(
    [string]$ConfigPath
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

if (-not $ConfigPath) {
    # Walk up from this script until the config file turns up, so the panel
    # works wherever it is placed in the tree.
    $dir = $PSScriptRoot
    while ($dir) {
        if (Test-Path (Join-Path $dir 'ctm-device-config.txt')) { break }
        $parent = Split-Path $dir -Parent
        if (-not $parent -or $parent -eq $dir) { $dir = $null; break }
        $dir = $parent
    }
    if ($dir) { $ConfigPath = Join-Path $dir 'ctm-device-config.txt' }
    else { $ConfigPath = 'ctm-device-config.txt' }
}
$ConfigPath = [System.IO.Path]::GetFullPath($ConfigPath)

if (-not (Test-Path -LiteralPath $ConfigPath)) {
    [System.Windows.Forms.MessageBox]::Show(
        "Config file not found:`n$ConfigPath`n`nPass the path as the first argument.",
        'CTM device config', 'OK', 'Error') | Out-Null
    return
}

# --- Reading -----------------------------------------------------------------
# Values only; the file's own text is left alone until a save.
function Read-Config {
    $values = @{}
    $section = ''
    foreach ($line in Get-Content -LiteralPath $ConfigPath) {
        $text = $line
        $hash = $text.IndexOfAny([char[]]('#', ';'))
        if ($hash -ge 0) { $text = $text.Substring(0, $hash) }
        $text = $text.Trim()
        if (-not $text) { continue }
        if ($text.StartsWith('[') -and $text.EndsWith(']')) {
            $section = $text.Trim('[', ']').Trim().ToLower()
            continue
        }
        $eq = $text.IndexOf('=')
        if ($eq -lt 0 -or -not $section) { continue }
        $key = $text.Substring(0, $eq).Trim().ToLower()
        $values["$section/$key"] = $text.Substring($eq + 1).Trim()
    }
    return $values
}

# --- Writing -----------------------------------------------------------------
# Rewrites in place, line by line: only a line whose key is being changed is
# touched, and only its value. A key that is present but commented out is
# uncommented; a key that is absent is appended to its section.
function Write-Setting {
    param([string[]]$Lines, [string]$Section, [string]$Key, [string]$Value)

    $current = ''
    $sectionStart = -1
    $sectionEnd = $Lines.Count
    for ($i = 0; $i -lt $Lines.Count; $i++) {
        $trimmed = $Lines[$i].Trim()
        if ($trimmed.StartsWith('[') -and $trimmed.EndsWith(']')) {
            $current = $trimmed.Trim('[', ']').Trim().ToLower()
            if ($current -eq $Section) { $sectionStart = $i }
            elseif ($sectionStart -ge 0 -and $sectionEnd -eq $Lines.Count) { $sectionEnd = $i }
            continue
        }
        if ($current -ne $Section) { continue }

        # Match the key whether the line is live or commented out.
        if ($Lines[$i] -match "^\s*#?\s*$([regex]::Escape($Key))\s*=") {
            $indent = ''
            if ($Lines[$i] -match '^(\s*)') { $indent = $Matches[1] }
            $Lines[$i] = "$indent$Key = $Value"
            return $Lines
        }
    }

    if ($sectionStart -lt 0) {
        return $Lines + @('', "[$Section]", "$Key = $Value")
    }
    $insert = $sectionEnd
    while ($insert -gt $sectionStart + 1 -and -not $Lines[$insert - 1].Trim()) { $insert-- }
    $head = if ($insert -gt 0) { $Lines[0..($insert - 1)] } else { @() }
    $tail = if ($insert -lt $Lines.Count) { $Lines[$insert..($Lines.Count - 1)] } else { @() }
    return @($head) + @("$Key = $Value") + @($tail)
}

$values = Read-Config

# --- Form --------------------------------------------------------------------
$form = New-Object System.Windows.Forms.Form
$form.Text = 'DualSense EDGE settings'
$form.Size = New-Object System.Drawing.Size(430, 360)
$form.StartPosition = 'CenterScreen'
$form.FormBorderStyle = 'FixedSingle'
$form.MaximizeBox = $false

$y = 16
$labelWidth = 130
$controlLeft = 150
$sliderWidth = 175
$boxLeft = 335

# Every row is a coarse slider plus a precise box. The slider is for dragging
# while listening -- it snaps to a step, so a drag produces a handful of
# distinct values rather than one per pixel, and therefore a handful of writes.
# The box takes any value in range, for when a specific number is wanted.
$script:rows = @()
$script:suppress = $false

function Add-Label([string]$Text) {
    $label = New-Object System.Windows.Forms.Label
    $label.Text = $Text
    $label.Location = New-Object System.Drawing.Point(16, ($script:y + 3))
    $label.Size = New-Object System.Drawing.Size($labelWidth, 20)
    $form.Controls.Add($label)
}

function Add-Slider([string]$Text, [string]$Key, [int]$Max, [int]$Step, [int]$Default) {
    Add-Label $Text

    $raw = $values["ds5_edge/$Key"]
    $parsed = 0
    $value = if ([int]::TryParse($raw, [ref]$parsed) -and $parsed -ge 0 -and $parsed -le $Max) {
        $parsed
    } else { $Default }

    # Scaled so the slider can only land on multiples of the step.
    $bar = New-Object System.Windows.Forms.TrackBar
    $bar.Minimum = 0
    $bar.Maximum = [int]($Max / $Step)
    $bar.TickFrequency = 1
    $bar.SmallChange = 1
    $bar.LargeChange = 2
    $bar.Value = [Math]::Min($bar.Maximum, [Math]::Round($value / $Step))
    $bar.Location = New-Object System.Drawing.Point($controlLeft, $script:y)
    $bar.Size = New-Object System.Drawing.Size($sliderWidth, 30)
    $form.Controls.Add($bar)

    $box = New-Object System.Windows.Forms.TextBox
    $box.Text = "$value"
    $box.TextAlign = 'Center'
    $box.Location = New-Object System.Drawing.Point($boxLeft, ($script:y + 3))
    $box.Size = New-Object System.Drawing.Size(52, 22)
    $form.Controls.Add($box)

    $row = [pscustomobject]@{
        Key = $Key; Bar = $bar; Box = $box; Step = $Step; Max = $Max; Value = $value
    }
    $script:rows += $row

    # $this is the control that raised the event, so no closures are needed.
    $bar.Add_ValueChanged({ Sync-FromSlider $this })
    $box.Add_Leave({ Sync-FromBox $this })
    $box.Add_KeyDown({ if ($_.KeyCode -eq 'Enter') { Sync-FromBox $this } })

    $script:y += 42
    return $row
}

function Find-Row($control) {
    foreach ($row in $script:rows) {
        if ($row.Bar -eq $control -or $row.Box -eq $control) { return $row }
    }
    return $null
}

function Sync-FromSlider($bar) {
    if ($script:suppress) { return }
    $row = Find-Row $bar
    if (-not $row) { return }
    $row.Value = $row.Bar.Value * $row.Step
    $script:suppress = $true
    $row.Box.Text = "$($row.Value)"
    $script:suppress = $false
    Queue-Save
}

function Sync-FromBox($box) {
    if ($script:suppress) { return }
    $row = Find-Row $box
    if (-not $row) { return }
    $parsed = 0
    if (-not [int]::TryParse($row.Box.Text.Trim(), [ref]$parsed)) {
        $row.Box.Text = "$($row.Value)"      # not a number: put it back
        return
    }
    $clamped = [Math]::Max(0, [Math]::Min($row.Max, $parsed))
    $row.Value = $clamped
    $script:suppress = $true
    $row.Box.Text = "$clamped"
    $row.Bar.Value = [Math]::Min($row.Bar.Maximum, [Math]::Round($clamped / $row.Step))
    $script:suppress = $false
    Queue-Save
}

function Add-Dropdown([string]$Text, [string]$Key, [string[]]$Options, [string]$Default) {
    Add-Label $Text
    $box = New-Object System.Windows.Forms.ComboBox
    $box.DropDownStyle = 'DropDownList'
    $box.Items.AddRange($Options)
    $current = $values["ds5_edge/$Key"]
    $box.SelectedItem = if ($current -and $Options -contains $current) { $current } else { $Default }
    $box.Location = New-Object System.Drawing.Point($controlLeft, $script:y)
    $box.Size = New-Object System.Drawing.Size(237, 24)
    $form.Controls.Add($box)
    $script:y += 38
    return $box
}

function Add-Check([string]$Text, [string]$Key, [bool]$Default) {
    $box = New-Object System.Windows.Forms.CheckBox
    $box.Text = $Text
    $current = $values["ds5_edge/$Key"]
    $box.Checked = if ($current) { $current -in @('true', '1', 'yes', 'on') } else { $Default }
    $box.Location = New-Object System.Drawing.Point(16, $script:y)
    $box.Size = New-Object System.Drawing.Size(370, 24)
    $form.Controls.Add($box)
    $script:y += 34
    return $box
}

$output  = Add-Dropdown 'Audio output' 'audio_output' `
    @('auto', 'headset', 'headset_mono', 'speaker', 'both', 'off') 'auto'
$speaker = Add-Slider 'Speaker volume' 'speaker_volume'     100 5  100
$headset = Add-Slider 'Headset volume' 'headset_volume'     100 5  100
$gain    = Add-Slider 'Audio gain'     'audio_gain'         100 5  100
$rumble  = Add-Slider 'Rumble'         'master_rumble_gain' 200 10 100
$echo    = Add-Check 'Force echo and noise cancellation' 'force_echo_cancel' $true

$status = New-Object System.Windows.Forms.Label
$status.Location = New-Object System.Drawing.Point(16, ($y + 6))
$status.Size = New-Object System.Drawing.Size(380, 20)
$status.ForeColor = [System.Drawing.Color]::ForestGreen
$form.Controls.Add($status)

# Debounced writer. Every control restarts the timer; the timer does the write.
$script:writeTimer = New-Object System.Windows.Forms.Timer
$script:writeTimer.Interval = 60

function Save-Now {
    try {
        $lines = @(Get-Content -LiteralPath $ConfigPath)
        $lines = Write-Setting $lines 'ds5_edge' 'audio_output'       $output.SelectedItem
        $lines = Write-Setting $lines 'ds5_edge' 'speaker_volume'     "$($speaker.Value)"
        $lines = Write-Setting $lines 'ds5_edge' 'headset_volume'     "$($headset.Value)"
        $lines = Write-Setting $lines 'ds5_edge' 'audio_gain'         "$($gain.Value)"
        $lines = Write-Setting $lines 'ds5_edge' 'master_rumble_gain' "$($rumble.Value)"
        $lines = Write-Setting $lines 'ds5_edge' 'force_echo_cancel'  $(if ($echo.Checked) { 'true' } else { 'false' })
        Set-Content -LiteralPath $ConfigPath -Value $lines -Encoding UTF8
        $status.ForeColor = [System.Drawing.Color]::ForestGreen
        $status.Text = "Applied"
    } catch {
        $status.ForeColor = [System.Drawing.Color]::Firebrick
        $status.Text = "Write failed: $($_.Exception.Message)"
    }
}

$script:writeTimer.Add_Tick({
    $script:writeTimer.Stop()
    Save-Now
})

function Queue-Save {
    $status.ForeColor = [System.Drawing.Color]::DimGray
    $status.Text = 'Applying...'
    $script:writeTimer.Stop()
    $script:writeTimer.Start()
}

$output.Add_SelectedIndexChanged({ Queue-Save })
$echo.Add_CheckedChanged({ Queue-Save })

$form.Add_FormClosing({ $script:writeTimer.Stop() })
$form.Add_Shown({ $form.Activate() })
[void]$form.ShowDialog()
