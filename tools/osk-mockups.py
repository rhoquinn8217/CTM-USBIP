#!/usr/bin/env python3
"""Draws the three overlay-keyboard candidates as standalone SVGs.

The layouts are DATA, not drawings: each key is (label, width-in-units, kind).
That is deliberate -- the same table becomes the C++ layout, so what is drawn
here and what gets built cannot drift.

kinds:  ''    ordinary key
        'mod' modifier (latches)
        'sc'  has a shoulder shortcut
        'fn'  swaps to F-keys while L2 is held
"""

U, GAP, KEY_H, ROW_GAP, PAD = 40, 4, 34, 6, 14

# --- osk1: a left column, ten-wide letters, no extra punctuation -------------
OSK1 = [
    [("esc", 1.5, "mod"), *[(c, 1, "") for c in "1234567890"], ("back  L1", 2, "sc")],
    [("tab", 1.5, "mod"), *[(c, 1, "") for c in "qwertyuiop"], ("enter  R2", 2, "sc")],
    [("shift", 1.5, "mod"), *[(c, 1, "") for c in "asdfghjkl;"]],
    [("ctrl", 1.5, "mod"), *[(c, 1, "") for c in "zxcvbnm,./"], ("↑", 2, "")],
    [("alt", 1.5, "mod"), ("win", 1.5, "mod"), ("space  R1", 6, "sc"),
     ("←", 1, ""), ("↓", 1, ""), ("→", 1, "")],
]

# --- osk2: widened to carry the rest of the punctuation ---------------------
OSK2 = [
    [("esc", 1.5, "mod"), *[(c, 1, "fn") for c in "1234567890-="], ("back  L1", 1.8, "sc")],
    [("tab", 1.5, "mod"), *[(c, 1, "") for c in "qwertyuiop[]"], ("\\", 1.8, "")],
    [("ctrl", 1.5, "mod"), *[(c, 1, "") for c in "asdfghjkl;'"], ("enter  R2", 2.8, "sc")],
    [("shift", 1.5, "mod"), *[(c, 1, "") for c in "zxcvbnm,./"], ("del", 1, ""), ("↑", 1, "")],
    [("alt", 1.4, "mod"), ("win", 1.4, "mod"), ("space  R1", 9.4, "sc"),
     ("←", 1, ""), ("↓", 1, ""), ("→", 1.1, "")],
]

# --- osk3: the same face, staggered like a physical keyboard ----------------
OSK3 = [
    [("esc", 1, "mod"), *[(c, 1, "fn") for c in "1234567890-="], ("back  L1", 2.1, "sc")],
    [("tab", 1.4, "mod"), *[(c, 1, "") for c in "qwertyuiop[]"], ("\\", 1.7, "")],
    [("ctrl", 1.65, "mod"), *[(c, 1, "") for c in "asdfghjkl;'"], ("enter  R2", 2.5, "sc")],
    [("shift", 2.15, "mod"), *[(c, 1, "") for c in "zxcvbnm,./"],
     ("↑", 1, ""), ("del", 1, ""), ("top", 1.1, "mod")],
    [("alt", 1.3, "mod"), ("win", 1.3, "mod"), ("space  R1", 9.3, "sc"),
     ("←", 1, ""), ("↓", 1, ""), ("→", 1, ""), ("close", 1.1, "mod")],
]

FILL = {"": "#12141a", "mod": "#191c24", "sc": "#1d2740", "fn": "#161d1b"}
EDGE = {"": "#262a34", "mod": "#2f333d", "sc": "#3a4d7a", "fn": "#26443a"}
TEXT = {"": "#e8eaf2", "mod": "#c9cbd3", "sc": "#cfe0ff", "fn": "#cfe8dd"}


def render(rows, note):
    width = max(sum(w * U + GAP for _, w, _ in r) - GAP for r in rows) + PAD * 2
    height = len(rows) * (KEY_H + ROW_GAP) - ROW_GAP + PAD * 2 + 58
    out = [
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width:.0f} {height:.0f}" '
        f'width="{width:.0f}" height="{height:.0f}" font-family="Segoe UI, sans-serif">',
        # ⓘ A checkerboard behind it, so the transparency is visible in the file
        # rather than only imaginable.
        '<defs><pattern id="b" width="24" height="24" patternUnits="userSpaceOnUse">'
        '<rect width="24" height="24" fill="#3a3f4a"/>'
        '<rect width="12" height="12" fill="#4a505c"/>'
        '<rect x="12" y="12" width="12" height="12" fill="#4a505c"/></pattern></defs>',
        f'<rect width="{width:.0f}" height="{height:.0f}" fill="url(#b)"/>',
        f'<g opacity="0.86"><rect x="4" y="4" width="{width - 8:.0f}" '
        f'height="{len(rows) * (KEY_H + ROW_GAP) - ROW_GAP + PAD * 2:.0f}" '
        'rx="10" fill="#08090c" stroke="#2c2e36"/>',
    ]
    y = PAD
    for row in rows:
        x = PAD
        for label, w, kind in row:
            kw = w * U
            out.append(
                f'<rect x="{x:.0f}" y="{y:.0f}" width="{kw:.0f}" height="{KEY_H}" rx="5" '
                f'fill="{FILL[kind]}" stroke="{EDGE[kind]}"/>'
                f'<text x="{x + kw / 2:.0f}" y="{y + KEY_H / 2 + 5:.0f}" text-anchor="middle" '
                f'font-size="{13 if len(label) > 2 else 15}" fill="{TEXT[kind]}">'
                f'{label.replace("&", "&amp;").replace("<", "&lt;")}</text>')
            x += kw + GAP
        y += KEY_H + ROW_GAP
    out.append('</g>')
    ny = y + PAD + 6
    for line in note:
        out.append(f'<text x="{PAD}" y="{ny}" font-size="12" fill="#c9cbd3">{line}</text>')
        ny += 17
    out.append('</svg>')
    return "\n".join(out)


NOTES = {
    "osk1": ["ten-wide letters, a left column, no extra punctuation",
             "blue keys have a shoulder shortcut"],
    "osk2": ["widened for - = [ ] \\ ; ' and del · teal row becomes F1-F12 while L2 is held",
             "blue keys have a shoulder shortcut · rows still align in columns"],
    "osk3": ["the same face, staggered like a physical keyboard",
             "rows no longer align, so movement is nearest-neighbour"],
}

for name, rows in (("osk1", OSK1), ("osk2", OSK2), ("osk3", OSK3)):
    with open(f"/mnt/user-data/outputs/{name}.svg", "w", encoding="utf-8") as f:
        f.write(render(rows, NOTES[name]))
    print(f"{name}: {len(rows)} rows, {sum(len(r) for r in rows)} keys")
