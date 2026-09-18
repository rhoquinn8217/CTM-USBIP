#pragma once

// ⭐⭐ THE PRODUCT'S NAME AND VERSION: OURS, beside upstream's version.h and
// deliberately not in it.
//
// rhoquinn8217, 2026-09-08: "CTM-USBIP is just a relay and is far from anything
// we've built. Ours is a remapper, a virtual keyboard, a gyro/touchpad/sticks to
// mouse, an all in one game-and-TV solution via stream." The product is
// DS5-USBIP, a tool built around the relay, so it has a version of its own.
//
// ⛔ Neither number that already existed will do. version.h's 0.0.2 (build 2) is
// upstream's and versions the relay; bumping it to track our releases would
// muddle a number that is not ours to bump. The settings page's 2.6x is a
// changelog counter for one file.
//
// ⭐ SEMANTIC VERSIONING, 0.x FOR NOW: PATCH for fixes, MINOR when a capability
// lands. 1.0.0 is the day a stranger can extract the zip and use it without
// rhoquinn8217 in the room.
//
// ONE PLACE, READ EVERYWHERE: the settings window's title (from /api/v1/status,
// never a copy in the page), --version, the release zip's name (release.ps1
// reads this file) and the exe's product fields (app/ctm-usbip.rc). The git tag
// is v<PRODUCT_VERSION>.
//
// ⚠️ Keep the numbers and the string in step; tests/product_version_test.cpp
// fails when they are not.

#define PRODUCT_NAME          "DS5-USBIP"
#define PRODUCT_VERSION_MAJOR 0
#define PRODUCT_VERSION_MINOR 1
#define PRODUCT_VERSION_PATCH 0
#define PRODUCT_VERSION       "0.1.0"
