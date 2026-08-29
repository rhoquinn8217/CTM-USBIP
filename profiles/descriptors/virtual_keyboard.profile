# Synthetic HID keyboard presented to Windows alongside a bridged controller.
#
# WHY IT EXISTS. Button rebinding turns a controller button into a keyboard key.
# Something has to BE a keyboard for that key to come from, and this is it --
# Windows binds it to its stock HID keyboard driver, exactly as it would a real
# one. The reports are generated on the Windows side from controller input.
#
# ⭐ WHY A DEVICE AND NOT SendInput. SendInput is synthetic injection and Windows
# flags it as such; games using raw input can see the flag and anti-cheat often
# blocks it, so it would fail in exactly the games people care about, and fail
# invisibly. A virtual USB keyboard is indistinguishable from real hardware --
# which is this project's whole premise. The gyro mouse already proves the
# pattern.
#
# SHAPE. A single HID interface, one interrupt IN endpoint (0x81), boot-keyboard
# protocol: 8-byte reports.
#   [0] modifier bits  [1] reserved  [2..7] up to six key usages
# As standard a USB keyboard as exists, so no driver surprises.
#
# IDs. Neutral and clearly synthetic, matching the gyro mouse: 0x1D6B is the
# Linux Foundation VID commonly used for virtual USB/IP devices. Product 0x0003
# so it never collides with the mouse.

[profile]
id = virtual_keyboard
name = CTM Rebind Keyboard
status = synthetic

[device_descriptor]
# 18-byte device descriptor. USB 2.0, class 0 (defined at interface), 8-byte
# EP0, VID 0x1D6B / PID 0x0003, bcdDevice 1.00, string indices 1/2/3, 1 config.
bytes = 0x12 0x01 0x00 0x02 0x00 0x00 0x00 0x08 0x6B 0x1D 0x03 0x00 0x00 0x01 0x01 0x02 0x03 0x01

[configuration_descriptor]
# 34-byte config: config(9) + interface(9) + HID(9) + endpoint(7).
#  config:    9,2, wTotalLength=0x0022(34), 1 iface, cfg#1, iConfig 0,
#             bmAttributes 0xA0 (bus-powered, remote-wakeup), 50mA (0x32).
#  interface: 9,4, iface0, alt0, 1 endpoint, class 3 (HID), sub 1 (boot),
#             proto 1 (KEYBOARD -- the mouse uses 2), iInterface 0.
#  HID:       9,0x21, bcdHID 1.11, country 0, 1 descriptor, type 0x22 (report),
#             wReportLength 0x002D (45 bytes -- see the report descriptor).
#  endpoint:  7,5, addr 0x81 (IN 1), attr 0x03 (interrupt), wMaxPacket 8,
#             bInterval 1.
#
# ⓘ bInterval 1 (1000Hz) matches the mouse. A keystroke does not need that rate,
# but a slow poll is how the mouse silently dropped reports when its queue
# filled -- see that profile. Polling faster than reports arrive costs nothing
# and cannot lose one.
bytes = 0x09 0x02 0x22 0x00 0x01 0x01 0x00 0xA0 0x32 0x09 0x04 0x00 0x00 0x01 0x03 0x01 0x01 0x00 0x09 0x21 0x11 0x01 0x00 0x01 0x22 0x2D 0x00 0x07 0x05 0x81 0x03 0x08 0x00 0x01

[hid_report_descriptor]
# 45-byte keyboard report descriptor: the HID spec's boot-keyboard example with
# the LED OUTPUT block removed.
#
#   8 modifier bits (LeftCtrl..RightGUI, usages 0xE0-0xE7)
#   1 reserved byte
#   6 key slots, one byte each, usages 0x00-0x65
#
# Report is 8 bytes: [0] modifiers  [1] reserved  [2..7] keys.
#
# ⛔ THE LED OUTPUTS ARE GONE, deliberately. The spec's example declares five
# LED bits, which makes the host send SET_REPORT on EP0 -- and this device has
# nothing to light up. It is also the ONE structural difference from the gyro
# mouse, which works: that profile declares no output items at all.
#
# ⚠️ Measured 2026-08-28: with the outputs present, Windows enumerated the
# device and reported it OK, the agent published correct reports, and no
# keystroke ever arrived.
bytes = 0x05 0x01 0x09 0x06 0xA1 0x01 0x05 0x07 0x19 0xE0 0x29 0xE7 0x15 0x00 0x25 0x01 0x75 0x01 0x95 0x08 0x81 0x02 0x95 0x01 0x75 0x08 0x81 0x03 0x95 0x06 0x75 0x08 0x15 0x00 0x25 0x65 0x05 0x07 0x19 0x00 0x29 0x65 0x81 0x00 0xC0

[string_descriptors]
# LangID en-US (0x0409), then "CTM" (mfr), "Rebind Keyboard" (product),
# "CTM-KBD-01" (serial). Each: length, 0x03, UTF-16LE.
bytes = 0x04 0x03 0x09 0x04 0x08 0x03 0x43 0x00 0x54 0x00 0x4D 0x00 0x20 0x03 0x52 0x00 0x65 0x00 0x62 0x00 0x69 0x00 0x6E 0x00 0x64 0x00 0x20 0x00 0x4B 0x00 0x65 0x00 0x79 0x00 0x62 0x00 0x6F 0x00 0x61 0x00 0x72 0x00 0x64 0x00 0x16 0x03 0x43 0x00 0x54 0x00 0x4D 0x00 0x2D 0x00 0x4B 0x00 0x42 0x00 0x44 0x00 0x2D 0x00 0x30 0x00 0x31 0x00
