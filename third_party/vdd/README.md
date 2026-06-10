# Vendored Virtual-Display-Driver (VDD)

A standalone, signed virtual-display component for CTM Bridge. It creates a
Windows virtual monitor that **mirrors the target LG HDR 4K TV** (its real EDID,
modes, and HDR), so the desktop can be captured and streamed to the TV — with
**no driver signing on our side**.

## Why this and not our own driver
Windows creates a virtual monitor only via an **IddCx** (user-mode) indirect
display driver — not via USB. VDD is exactly that, already **signed by SignPath
Foundation** (publicly trusted), so it installs on stock Windows 11 without
test-signing. See `../../docs/` and the project notes for the full rationale
(the DisplayLink-over-USB route can't create a monitor without DisplayLink's
closed user-mode service).

## Upstream
- Project: VirtualDrivers/Virtual-Display-Driver — https://github.com/VirtualDrivers/Virtual-Display-Driver
- Version vendored: **25.7.23**
- License: **MIT** (© 2024 Virtual Display) — see `LICENSE.VDD.txt`. The driver
  (`x64/MttVDD.*`) and `devcon.exe` are redistributed unmodified.

## Contents
| File | What |
|------|------|
| `x64/MttVDD.{inf,dll,cat}` | the signed x64 IddCx driver (`Root\MttVDD`, IddCx 1.2) |
| `devcon.exe` | Microsoft device console, used to create/remove the root device |
| `vdd_settings.xml` | CTM config: `CustomEdid=true`, `HDRPlus=true`, `PreventSpoof=false` |
| `user_edid.bin` | verbatim 256-byte EDID of the LG HDR 4K TV (mirrors its identity + HDR) |
| `install-ctm-vdd.ps1` / `uninstall-ctm-vdd.ps1` | self-elevating install / remove |

At runtime the driver reads `C:\VirtualDisplayDriver\vdd_settings.xml` and
`C:\VirtualDisplayDriver\user_edid.bin`; the install script copies them there.

## Use
```powershell
# install (self-elevates) -> "LG HDR 4K" virtual monitor appears in Settings > Display
.\install-ctm-vdd.ps1
# remove
.\uninstall-ctm-vdd.ps1
```

To re-target a different TV, replace `user_edid.bin` with that display's 256-byte
EDID (dump it from `HKLM\SYSTEM\CurrentControlSet\Enum\DISPLAY\<id>\Device Parameters\EDID`).
