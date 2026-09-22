# Changes from upstream

This is a fork of [`CTM-Bridge/CTM-USBIP`](https://github.com/CTM-Bridge/CTM-USBIP),
forked at `08df624` (2026-07-26), 31 commits after the `v0.0.1` tag.
Licensed GPL-3.0-or-later, the same as upstream.

Changes below are by rhoquinn8217. Each line names the commits that
carry it; upstream's own history is unchanged.

## Changes

| Date | Change | Commits |
|---|---|---|
| 2026-07-02 | Added `ds5_usb` device kind, dispatched to its own profile | `4f50540` |
| 2026-07-02 | Added `maps/ds5_usb_over_ds5_usb.map` for wired connections | `0f09507`, `8a31dbd`, `4b4630f` |
| 2026-07-02 | Added message type `MsgIsoAudio = 11` | `b8a754c` |
| 2026-07-02 | Build: allow the VS2026 toolchain; copy the new map | `5f30787` |
| 2026-07-28 | Added ISO passthrough map flag and backend send path | `3bc5624`, `67d8e4d` |
| 2026-07-28 | Route wired ISO audio through a separate path | `19f22a9` |
| 2026-07-28 | Added map parser test project and test harness | `04754b3`, `bdb14f0` |
| 2026-07-29 | Added PCM amplitude logging to the wired ISO audio path | `e3afa55`, `841d9a2`, `cbae4b4` |
| 2026-07-30 | Check the keepalive enable result instead of discarding it | `61302f7` |
| 2026-07-30 | Log bridge session transitions; flush log output so teardown lines are not lost | `dbec96f`, `87d1427` |
| 2026-08-01 | Windows-side device configuration. A text config file drives echo cancellation, audio routing, volumes and gains; edits apply live | `570ba61`, `20d14cb`, `fcc55ba`, `7cb8f1f`, `706b88c`, `382d18f`, `78dbdd1`, `2e6b42f`, `70c17dd`, `a7efc78`, `b496d08`, `a42dbe4`, `4fdf97f` |
| 2026-08-02 | Controller microphone audio, host side. Completions release after the audio they carry, ending a busy loop of ~28,000 requests a second | `76493f2`, `ec3c9de`, `99030fa`, `d7553ef` |
| 2026-08-04 | Microphone buffers are per session, not per process. One shared buffer let a second controller drain the first one's audio | `310df17`, `4a1d11e` |
| 2026-08-05 | Mute the controller's microphone from the host, so nothing else can leave it streaming. The bit layout is asserted in tests | `f85de1a` |
| 2026-08-05 | Accept the DualSense Edge. Its own captured USB descriptor makes Windows identify it as an Edge, which its extra controls need | `8fbb26d`, `9d8e3d7` |
| 2026-08-11 | Hold the audio stream open briefly after bridging. The controller's audio sleeps when idle, and waking it swallowed the start of a sound | `4edd287`, `e9eceea`, `0d0c907` |
| 2026-08-12 | A settings panel for the DualSense Edge, so its configuration can be edited without hand-writing the file | `e998d29` |
| 2026-08-12 | Drop the audio hold. The TV signals its own controllers now, so the host need not keep the stream awake for it | `f36f272` |
| 2026-08-13 | Drop microphone reports before anything reads them as pad state. Audio arrives in the button report, one flag apart, and reads as presses | `9bdbf81` |
| 2026-08-16 | The host owns the controller's Bluetooth audio buffer. `audio_latency_ms` reaches a live session in a second or two, with no reconnect | `c3e2496`, `1b12d9e` |
| 2026-08-19 | Stop preloading feature reports a Bluetooth DualSense never answers. They shared the link with the TV's tone and took the pad offline | `366b0de`, `08dabcd` |
| 2026-08-20 | Optional HTTP/JSON control API (`--rest <port>`): status, sessions, bridge start/stop, restart. Loopback-only unless `--rest-lan` | `4875cb3` |
| 2026-08-21 | Per-controller configuration. A controller links to its own config file, can claim a serial so it re-attaches at bridge time, updates live | `b08a90f`, `8b6e26a`, `3f63e49`, `e550c01`, `bf325cc`, `5abf076`, `71b6360`, `7132194`, `16c5054` |
| 2026-08-21 | Gyro-to-mouse. Motion drives a synthetic USB mouse using the pad's own calibration, so a sensitivity number means the same on every pad | `8556242`, `b02ea17`, `2cb9c26`, `395b8b3`, `a640c2e`, `25e9c76` |
| 2026-08-23 | The agent serves its own settings page (`--ui`), embedded in the executable and usable entirely from a controller | `ccb1f56`, `b7bf340`, `78c6095`, `c162412`, `21b5999`, `9b0d4ea`, `3a85ee7`, `ff5b164`, `022f131`, `46109c7`, `8ce9227`, `13fcd7a`, `a036db3`, `e0a5719`, `11f3301` |
| 2026-08-24 | A release script and launcher: one folder with a README. Upstream's release FFmpeg replaces the debug binaries, and the build fetches vcpkg | `27eea1e`, `ab8cb60`, `0b2202a`, `23d7ad4` |
| 2026-08-26 | One log format, tagged by layer and quiet by default, with repeated lines collapsed rather than printed hundreds of times | `fdc8325`, `f82d303` |
| 2026-08-28 | Button rebinding. A button sends a keyboard key or mouse action through synthetic USB devices, and the game stops seeing the button | `e1ae04d`, `693f00c`, `69635b6` |
| 2026-08-28 | Config mode and the chord that opens it. Two fingers plus Options opens the settings window; the pad drives it and the game hears nothing | `f9c2d8e`, `76ab5e8`, `a72c741`, `fa9bfa1`, `ce2aa02`, `f9270a5` |
| 2026-08-31 | The touchpad drives the mouse: one finger the cursor, two fingers scroll, a tap clicks. Clicking the pad in grabs a drag, lifting drops it | `229314e`, `6bddf3e` |
| 2026-08-31 | A stick drives the mouse, and a stick scrolls. Travel is speed times elapsed time, so it does not change with the report rate | `25527ef`, `b3cd198` |
| 2026-08-31 | An on-screen keyboard on a button. `OSKeyboard` toggles Steam's keyboard or Windows' `osk.exe`; only Steam's takes a controller | `470f1ea` |
| 2026-08-31 | Presets on New: gyro-to-mouse, stick-to-mouse, touchpad-mouse, L2-gyro-mouse-aiming. Ordinary configs, named after the preset | `6bddf3e`, `e44f113` |
| 2026-08-31 | One settings section at a time, and Safe Edit Mode names what the lock costs: basic controls only, or your inputs mirror in the game | `25527ef`, `988562f`, `b989322`, `35424b7` |
| 2026-08-31 | Tests stopped deleting the agent's configs. They ran where the agent reads and removed the user's files; they use a scratch dir now | `229314e` |
| 2026-09-08 | Options moves the settings window and R3 sizes it, the same gesture the on-screen keyboard uses. A tap places it, a hold steers it with the stick or the mouse | `764e06c`, `b8ffac7`, `01f247d`, `239000b`, `79c03b4` |
| 2026-09-09 | Three layouts for the settings window: SIMPLE, one controller and its config; QUICK, the same cut to a name and a selector for reaching mid-game; ADVANCED, the full page. Create goes straight to Quick, and the listener remembers the layout, size, place and controller across a close | `339ef21`, `6f83ccf`, `fc6982a`, `9097921`, `faa5760`, `36dae19`, `241e69a` |
| 2026-09-09 | One Options mover per controller: two bridged pads shared one, so a hold on either snapped the window on every report of the other. The stick that steers a window no longer also drives the mouse, and travel is speed times elapsed time, so it does not change with the report rate | `1526c97`, `1256eea`, `5f9e4db` |
| 2026-09-10 | The touchpad preset scrolls naturally, and its description says so | `f8ad9ac` |
| 2026-09-10 | Adaptive trigger effects: a resistance break, a wall, or a climb that lets go, placed anywhere in the pull | `bbfcd5f`, `9f9b847`, `6cbe348`, `eaf2e55`, `33ff84d` |
| 2026-09-10 | R2 as a mouse click: the cursor freezes for the whole gesture, so a double click lands twice on one pixel, and a held click becomes a drag | `890fab0`, `1db8342` |
| 2026-09-11 | A trigger is bound in one place. `rebind_6` and `rebind_7` say what it sends, as for any other button, and a per-side mode says what pulling it does to the cursor. Setting both no longer sends two presses per pull | `c948bad`, `7abdd1e`, `6e873e6`, `962e7fc`, `b285c83`, `9262f50`, `f0d70f6` |
| 2026-09-11 | The press lands on the break the finger feels. The controller is asked rather than a travel number guessed: byte 42 for the right trigger, byte 43 for the left, both confirmed on hardware. Two thresholds so a resting trigger cannot chatter | `aa00e2a`, `4950379`, `573d079`, `f8ab6eb`, `e935859`, `dac514b`, `8a9cd41`, `fd15276` |
| 2026-09-11 | Four trigger feels, each measured rather than assumed: a break, a wall, a wall with a detent, and a break that returns itself. A notch and a snap press on travel, because the hardware cannot name their moment. An effect of "off" now clears the trigger whenever it is asked for | `64efd09`, `86317a5`, `265418c`, `8f1a672`, `856e615`, `5325e54`, `458f4f6`, `7e7001e`, `ca47551`, `033758f` |
| 2026-09-11 | The gyro gate and the trigger's steady are separate settings, so one trigger can open the gate on a light hold and click at its break. The drag and double-click windows are per side, because which trigger wants them follows what it is bound to | `258c07b`, `fdbc0ca`, `ccfa2d0` |
| 2026-09-11 | Two ways a mouse button could be left held down, both closed: the rebinder went silent once it gave a trigger up, and the pump recorded a release as sent before checking there was a device to send it to | `012fef6` |
| 2026-09-11 | Settings page 2.64.48: trigger settings grouped and ordered, the compact config picker steps in place instead of opening a list taller than its window, preset previews drop tuning numbers and passthroughs, and the trigger log reports both sides | `dad60d1`, `f763fe1`, `e0b521d`, `91b2c38`, `b06d011`, `683b091`, `3daa7a3`, `3c33c4f`, `9d74ebd`, `012d986`, `9006b65` |
| 2026-09-12 | Check which pad it is before reading the pad. The mouse hooks, the rebinder and the gates read DualSense offsets on whatever arrived, so a DS4's timestamp read as a button; each path now asks the layout first | `5e270d1` |
| 2026-09-12 | A button layout per pad, resolved once per report, with an Xbox table read off its own map file. Config mode rests a pad at its own offsets, where DualSense positions had frozen an Xbox report instead of resting it | `96ecdcf`, `f60b096` |
| 2026-09-12 | A config is no longer tied to a controller type. Any config links to any controller, each setting checks the pad before it acts, and an existing DS5 config keeps working unchanged | `3800cff` |
| 2026-09-14 | A new bridge retires an older one only when the serial and the TV's node both match, so a second pad no longer displaces the first | `e99dcf5` |
| 2026-09-14 | A mouse, keyboard or other bridged part is never dropped for being idle. Only a pad has an idle timeout, because only a pad is expected to keep talking | `8177f0e` |
| 2026-09-15 | A cabled DualShock 4 is a controller in its own right: its own kind, map and button table, steering the mouse by gyro, touchpad and stick from its own layout, named as itself in the text and the presets | `c3e7852`, `a926df5`, `ef0c5a9`, `53bd775`, `d4c1d5b`, `dbbcd83` |
| 2026-09-15 | An Xbox trigger pulled past a threshold presses like a button. Its triggers carry no digital bit, so every binding on them had been silent | `192e2d1` |
| 2026-09-15 | Each pad keeps its own held mouse buttons and its own chord state, so a pad lying at rest no longer releases another pad's drag | `f955fd4`, `449003f` |
| 2026-09-15 | The left stick scrolls even when no stick is moving the cursor | `d709b51` |
| 2026-09-15 | A trigger whose steady switch is off fires its remap again | `15da386` |
| 2026-09-15 | A map's handshake outranks the pad's own input in the queue, and every bridged pad announces its own id rather than the captured one | `55b6a79`, `581efbe` |
| 2026-09-15 | Diagnostics: the first fifty served packets are written out, the queue cap says when it eats one, and the log says whether the host is collecting what we queue | `833029e`, `5bf617d` |
| 2026-09-16 | The microphone guard writes to a DualSense, not to every device whose report is the same shape | `1f84fec` |
| 2026-09-17 | The gyro's hold has one definition, shared with the trigger tests rather than copied | `5ce687a` |
| 2026-09-17 | `device.log` is capped at 20 MB with one older file kept, and the every-report lines move behind their own switch. An unattended run had written 227 MB | `757b51a`, `61b0ddd` |
| 2026-09-17 | DS5-USBIP has a name and a version of its own, 0.1.0, shown from one place: the command line, the file properties, the status endpoint and the page | `827556c` |
| 2026-09-17 | A device is named by its model, then its type, and "hid" only when nothing else is known | `f407995` |
| 2026-09-17 | A serial of ALL ZEROS is no identity. One refusal in `normalise_serial`, so a config file's claim is dropped as it is read, nothing auto-links to it, and adding such a claim is refused. All zeros, not leading zeros: a Switch Pro Controller's `000000000001` is a real per-unit serial | `49084cc` |
| 2026-09-18 | The pad's own charge, read off the report the listener already receives, and shown as a battery beside the controller's name in all three views and as a column in both device tables. Offsets measured on the pads, not taken from a header; a pad with no battery byte, or in a fault state, shows nothing at all rather than zero | `929760c`, `88bc398`, `4e17c9b`, `a69278f`, `01f8d67`, `2823912`, `73f298c` |
| 2026-09-18 | The settings page talks to the port it was SERVED from instead of a hardcoded 48055, so a listener on any other REST port no longer looks dead while answering | `98046e8` |
| 2026-09-18 | The tray icon is a controller rather than a keyboard, and a click opens a menu -- settings or the keyboard -- instead of opening the keyboard outright | `cfefe25`, `c211ae4` |
| 2026-09-18 | The TV's overlay chord is taken out of a bridged Xbox pad's report: while both bumpers are held, Select and Start are cleared before Windows is served them, so the chord opens the TV's overlay without Steam opening its keyboard behind it. The bumpers, the face buttons and the d-pad are untouched | `0063ce8` |
| 2026-09-19 | The parts of one device share one nickname. A dongle arrives as two or three separate bridges and each was named on its own, so one device answered to two words that exist only to be said out loud. Measured across three multi-part devices: every part reaches the listener with an identical product string, because the TV sends the USB device's iProduct and not the part's own name | `c799ad3` |
| 2026-09-19 | A rumble floor, so a faint cue stays faint rather than vanishing. A straight multiply at low gain leaves a motor below the amplitude that starts it turning. `rumble_floor` is a percentage, default 12, which is the floor DS4Windows settled on. A motor the game asked to stop, and a motor the person gained to zero, are both left silent | `ae13560` |
| 2026-09-19 | A game's own adaptive trigger effect no longer replaces the config's. The config's effect is sent once, when a config links, while a DualSense-aware game sends its own trigger blocks in every output report -- which took the trigger remaps with them, since a click remap fires on a break that was no longer there. Only a trigger the host is actually claiming is rewritten | `33bcede` |
| 2026-09-19 | A gear on the on-screen keyboard's tab, on all three faces, opening the config window. The keyboard swallows the pad while it is up, so the window was unreachable without knowing a chord. Each tab row must now span its face exactly, checked at compile time rather than noticed on a television | `3b6383c` |
| 2026-09-19 | An Unlink button wherever the config is shown, and Reset retired. Every view gets a button that sets the controller to no config: at the end of the row in the full view, left of the selector in the two compact ones, with the mark after its word. It asks nothing, because picking "(no config)" asks nothing either. The mark is U+1F6C7, a text-presentation glyph, so it takes the button's colour instead of being drawn as a red emoji. "Auto link no config" now means remove the auto link: the auto-link button used to be dead whenever no config was linked, which is exactly the state Unlink leaves behind, and with a claim standing it now offers to drop it. Reset and its column leave both device tables and its question goes with them | `b757c87`, `afe30dc`, `26d9545`, `03d3561`, `3f5c155` |
| 2026-09-20 | The Editor tab owns every config action and Overview owns none. Its New button was dead rather than narrow -- it returned on its second line whenever no controller matched -- and Archive moved there because it was the only action in Overview's table belonging to a config rather than a controller. The config picker now opens as far as the window allows, upward as well as down, out of flow so nothing below it is pushed: a select with a size grows downward IN FLOW, which is why row counts of 8 and then 5 were both compromises with a layout rather than a size. The row under the mouse highlights, a mouse can pick -- preventDefault on mousedown had been killing the selection before it happened -- and in Quick it grows sideways too, measured against the longest name rather than guessed. Archiving the config you are editing closes it and takes its section tabs with it | `7e275b6`, `2265873`, `f2efa9c`, `6471aa8`, `e7999a8`, `cd1213e`, `b06930a`, `b1a0e18`, `db60748`, `f2a6c9a` |
| 2026-09-20 | One Mode picker replaces the view buttons and locks the window to a layout, and a day of shaping it: reachable by pad, sized like the Close button beside it and paired with it in the bottom right, carrying the mode identifier itself in yellow capitals with a padlock so no legend names its mode any more. It always opens UPWARD, by pad and by mouse, because it sits in the footer and a native popup is not clipped by the window. New, Edit, Mode and Close read as one sideways chain and arriving at the selector row means the config picker. Circle in Simple goes TO the Close button rather than closing. A compact view opens at bottom centre, which is where reposition now starts from rather than stepping on from whichever place was nearest by x alone. Create resizes the window and R3 is given back to the gyro | `2730d5c`, `be236d8`, `532a2de`, `64a7129`, `28aa2b7`, `c5c0a97`, `113420d`, `0bad48f`, `c32d475`, `5dd1d94`, `281370c`, `d2ef3c6`, `f008327`, `d8ba7bb` |
| 2026-09-20 | The adaptive trigger settings are greyed on a pad that has none, with the reason on the row. Eight keys rather than the Triggers section: press_at, the two timings and steady_cursor_pull all work on a DS4, because a trigger is an analogue axis whatever the pad | `de1710b`, `d4ad04a` |
| 2026-09-20 | The preset set renamed into one shape and gained an R3-gated one, which meant adding R3 to the gyro gate -- four lines, because the button already had a spot in both tables. Both always-on gyro presets steady the cursor on either trigger, so a click lands where you were pointing. "Blank" is called "custom", a config made from it is named after the pad that asked -- ds5_config, ds4_config, xbox_config, pad_config -- and the list opens on the stick, the one preset every pad can use | `01c58bc`, `b393888`, `f2a6c9a` |
| 2026-09-20 | trigger_probe is hidden on the settings page. It is a diagnostic whose own help says to leave it off, and it sat in Triggers for everyone. Listed by name rather than renamed to a _debug key, which would have hidden it for free: the key is read in two places in the listener, and anyone with it set in a config file would have lost it silently | `006290a` |
| 2026-09-20 | A DS4 is still NOT offered the Audio section, after a day spent finding out why it cannot be. The three settings travel to the TV rather than being patched into a DualSense report, so they reach any pad and the section was opened up on that reasoning -- then neither volume moved anything on the pad, and the routing mode neither silenced a connected headset nor started the speaker. Arriving is not acting, so it is hidden again with the finding and an undo list against T-229 | `d68fc4d`, `006290a` |
| 2026-09-20 | Copy asks for a name before it makes one, the way Rename does: an expanding row opened on the name it would have chosen, editable, with Create and Cancel. Both Copy buttons, because one expanding and one firing instantly is two buttons with the same word behaving differently. ONE row serves both jobs -- a second would have been a dozen more places to keep the guard flag in step, since it gates the d-pad, the plain keys, Escape, Space and the config poll. And the row now closes on a tab change: it used to survive, still pointing at the config from the tab you left, so Save renamed something off screen while the ten-second config poll stayed suppressed | `6f5daa4` |
| 2026-09-20 | The gyro gate is TWO settings: `gyro_to_mouse_gate_type` says when -- off, on button release, on button hold -- and `gyro_to_mouse_gate_button` says which, from any of the 17 button indices plus six touchpad gestures. The one key before it mixed both questions, so "off" and "always" hid among the buttons and the button list was hand-written: T-235 paid four edits to add R3. The invert IS the type, so "always on" is the type with no button and `!touchpad` becomes "on button release" on the touchpad, which is what "move unless a finger is down" says. Two traps, both pinned by tests: reading the triggers through `is_pressed()` would have passed on an Xbox pad while making a DualSense's L2 a hair trigger, and a draft with four touchpad gestures had no plain "any finger" case, which is what the old `touchpad` value meant. The old key is hidden and still read, proven on three legacy files covering `always`, `L2` and the dead `trigger` alias. `Gate::TriggerHold`, unreachable, is gone | `1d6267e`, `0854cb2`, `5c24bfb`, `a92775f`, merge `f510a3d` |
| 2026-09-21 | The settings window comes back where it was left, in every mode. Two placers were fighting: the page's `sizeOnOpen` reaches `compactHome`, which does not only resize but MOVES the window to bottom centre, so a restored place was discarded milliseconds after it landed; and `fitWindowIfOversized` read a stale `window.outerWidth` -- `resizeTo` is asynchronous, a fact stated a few lines above it -- saw Chrome's remembered 2341x1009 instead of the size just set, and yanked the window to Advanced's geometry. Twice per open, measured. Advanced was the one mode unaffected, because it is the one `sizeOnOpen` skips, and that is what named the bug. The layout, size, place and controller now also survive a listener restart, in `window-state.txt` beside `configs/`. A false alarm is recorded with it: the fix was twice declared incomplete from a PowerShell probe that was DPI-unaware while the listener is `PER_MONITOR_AWARE_V2`, so every coordinate it read came back divided by the 1.25 display scale. Three separate unexplained numbers were that one mistake | `833eb44`, `1587dfc`, `cfd5a84`, merge `9116c5a` |
| 2026-09-21 | The settings page's button legend follows the pad in your hand, not the pad the window is showing. It asks the LISTENER which device pressed last, because the browser structurally cannot answer: the rebinder clears a bound button out of the report before Windows sees it, so `navigator.getGamepads()` only ever shows the buttons a config has not claimed -- in practice L1 and R1 alone, which is exactly what was observed. A new `GET /api/v1/lastpress` returns the ordinal and kind, recorded at the top of `apply()` before anything clears a button, for ANY button rather than the ten the page navigates with, since a trigger pull is a press. The page polls it four times a second while its window is in front and not at all when it is behind, so the legend is right the moment you look at it; `/devices` at four seconds could never have done this. The browser scan stays as the answer for a pad plugged in but not bridged, whose buttons nothing clears. The legend also shows the marks on the buttons rather than their names: a compass for the d-pad in both sets, and the three-lines and two-rectangles marks in place of spelling out Menu and View | `c9df0a2`, `f3f2b2b`, `16fb5f4`, `13e80ac`, `867bccc`, `386f441`, merge `9116c5a` |
| 2026-09-21 | PARTIAL, and the ticket stays open. A touchpad tap is remappable: one finger, two fingers, and press-and-drag each take a mouse action instead of being hard-wired to left, right and drag. The ask was wider -- keyboard keys, controller buttons and the on-screen keyboard openers as well. Keyboard openers were reachable and were simply not done. Keyboard keys need a synthetic hold, because `set_state_for()` publishes a device's WHOLE held-key set and the rebinder republishes it every report, so an outside writer is overwritten within about 4ms and a tap holds nothing. Controller buttons need a `set_button()` that does not exist: the layout only offers `clear_button()` and the rebinder only ever removes buttons from a report | `d5ac645`, merge `9116c5a` |

## Files changed

Generated from `git diff origin/main origin/rhqn-main --stat`, excluding
`third_party/` (9 files, +1333 -- the GamepadMotionHelpers library and
upstream's release FFmpeg binaries replacing the repo's debug ones).

```
 .gitattributes                                |   48 +
 .gitignore                                    |   34 +-
 CHANGES.md                                    |  224 +
 LINK                                          |    0
 README.md                                     |   18 +
 app/ctm-usbip-tests.vcxproj                   |  108 +
 app/ctm-usbip.rc                              |   11 +-
 app/ctm-usbip.vcxproj                         |    4 +-
 attic/flydigi_apex4_identity.map              |   58 +
 attic/flydigi_apex4_usb.profile               |   24 +
 build-tests.ps1                               |   86 +
 build.ps1                                     |   88 +-
 device-config.md                              |  195 +
 docs/rest_api.md                              |  139 +
 include/ctm/map/runtime.h                     |   30 +
 include/ctm/product.h                         |   32 +
 maps/ds4_usb_over_ds4_usb.map                 |   61 +
 maps/ds5_usb_over_ds5_usb.map                 |   61 +
 maps/virtual_keyboard.map                     |   46 +
 maps/virtual_mouse.map                        |   54 +
 maps/xbox_gip_usb_over_xbox_bt.map            |    5 +
 profiles/descriptors/ds5e_composite.profile   |   30 +
 profiles/descriptors/virtual_keyboard.profile |   74 +
 profiles/descriptors/virtual_mouse.profile    |   59 +
 release.ps1                                   |  152 +
 src/app/agent.inl                             |  570 +-
 src/app/agent_session_sweep.inl               |  320 +
 src/app/cli.inl                               |   24 +-
 src/app/common.inl                            |   32 +
 src/app/config_move.inl                       |  770 +++
 src/app/device_capabilities.inl               |   75 +
 src/app/device_type.inl                       |   70 +
 src/app/nickname.inl                          |   90 +
 src/app/open_ui.inl                           |  489 ++
 src/app/overlay_window.inl                    | 1995 ++++++
 src/app/rest.inl                              |  760 +++
 src/app/rest_config.inl                       | 1176 ++++
 src/app/rest_config_sessions.inl              |  204 +
 src/app/rest_sessions.inl                     |   33 +
 src/app/same_controller.inl                   |   38 +
 src/app/same_device.inl                       |   79 +
 src/app/service.inl                           |   25 +-
 src/app/tray_icon.inl                         |  238 +
 src/app/ui_page.inl                           |   73 +
 src/app/window_move.inl                       |  245 +
 src/audio/audio_gain.inl                      |  178 +
 src/audio/ds5_apply_settings.inl              |  310 +
 src/audio/ds5_output_overrides.inl            |  699 +++
 src/audio/iso_in_pacing.inl                   |  221 +
 src/audio/iso_in_test_tone.inl                |   95 +
 src/audio/mic_ring.inl                        |  202 +
 src/audio/pcm_amplitude_log.inl               |  162 +
 src/audio/rumble_floor.inl                    |   58 +
 src/backend/backend.inl                       |   29 +
 src/backend/bridge.inl                        |  256 +-
 src/backend/bridge_enet.inl                   |   35 +-
 src/backend/bt.inl                            |   16 +-
 src/config/config_presets.inl                 |  452 ++
 src/config/config_store.inl                   |  820 +++
 src/config/config_watcher.inl                 |  170 +
 src/config/device_config.inl                  |  218 +
 src/input/battery.inl                         |  105 +
 src/input/button_layout.inl                   |  919 +++
 src/input/chord_gate.inl                      |   66 +
 src/input/gyro_calibration.inl                |  168 +
 src/input/gyro_calibration_fetch.inl          |   99 +
 src/input/gyro_hold.inl                       |   48 +
 src/input/gyro_mouse.inl                      | 1053 ++++
 src/input/keyboard_device.inl                 |  301 +
 src/input/mic_report.inl                      |   41 +
 src/input/mouse_device.inl                    |  304 +
 src/input/mouse_exclusive.inl                 |  122 +
 src/input/mouse_held.inl                      |   99 +
 src/input/osk.inl                             |  200 +
 src/input/rebind.inl                          | 1278 ++++
 src/input/stick_mouse.inl                     |  471 ++
 src/input/touch_mouse.inl                     |  466 ++
 src/input/trigger_click.inl                   |  801 +++
 src/input/trigger_effect.inl                  |  630 ++
 src/log/capped_log.inl                        |  117 +
 src/log/device_log.inl                        |  233 +
 src/main.cpp                                  |  449 +-
 src/map/runtime.cpp                           |   68 +-
 src/usbip/device.inl                          |  650 +-
 src/usbip/server.inl                          |   55 +-
 tests/button_layout_test.cpp                  | 1005 +++
 tests/capped_log_test.cpp                     |  140 +
 tests/config_store_test.cpp                   |  906 +++
 tests/device_capabilities_test.cpp            |   75 +
 tests/device_config_test.cpp                  |  521 ++
 tests/device_type_test.cpp                    |   89 +
 tests/gyro_mouse_test.cpp                     |  688 ++
 tests/harness.h                               |   55 +
 tests/host_audio_settings_test.cpp            |  125 +
 tests/iso_in_pacing_test.cpp                  |  118 +
 tests/map_defaults_test.cpp                   |  100 +
 tests/mic_report_test.cpp                     |   64 +
 tests/mouse_held_test.cpp                     |  162 +
 tests/nickname_test.cpp                       |   98 +
 tests/osk_test.cpp                            |   81 +
 tests/product_version_test.cpp                |   33 +
 tests/rest_parser_test.cpp                    |  195 +
 tests/rumble_floor_test.cpp                   |   87 +
 tests/same_controller_test.cpp                |   49 +
 tests/same_device_test.cpp                    |   95 +
 tests/schema_json_test.cpp                    |  158 +
 tests/stick_mouse_test.cpp                    |  715 +++
 tests/tests_main.cpp                          |  117 +
 tests/touch_mouse_test.cpp                    |  749 +++
 tests/trigger_click_test.cpp                  |  835 +++
 tests/trigger_effect_test.cpp                 |  529 ++
 tests/units.h                                 |   54 +
 tools/controller-config-test-client.html      | 8388 +++++++++++++++++++++++++
 tools/device-config-panel-edge.bat            |    9 +
 tools/device-config-panel-edge.ps1            |  327 +
 tools/device-config-panel.bat                 |    4 +
 tools/device-config-panel.ps1                 |  303 +
 tools/osk-mockups.py                          |  103 +
 tools/start-ctm-usbip.bat                     |   67 +
 119 files changed, 38510 insertions(+), 145 deletions(-)
```
