#!/usr/bin/env python3
"""
GTA.PAK generator: graphics pack for the SD card (loaded into PSRAM at startup).
All sprites are drawn here at a larger scale and downscaled with smoothing.

    pip install pillow
    python make_pak.py GTA.PAK

Format: "GPAK", u32 version, u32 entry count, 20-byte entries
(char name[8], u16 w, u16 h, u16 frames, u16 reserved, u32 offset), then frames of RGBA8888.
"""
import math, random, struct, sys
from PIL import Image, ImageDraw, ImageFilter, ImageFont

random.seed(7)
SS = 4  # supersampling

def rgb(c, k=1.0, add=0):
    return tuple(max(0, min(255, int(v * k + add))) for v in c[:3]) + (255,)

# ------------------------------------------------------------------ cars
# body color for col 0..12 and wrecked (13); order as in game.c
CAR_COLORS = [
    (200, 40, 40), (40, 80, 200), (230, 200, 40), (235, 235, 235),
    (40, 160, 70), (230, 120, 30), (130, 60, 170), (30, 30, 34),
    (238, 238, 238),   # 8 police
    (220, 180, 50),    # 9 gold
    (205, 32, 28),     # 10 fire truck
    (36, 34, 44),      # 11 VORTEX GT
    (236, 120, 24),    # 12 sports car
    (52, 46, 42),      # 13 wrecked
]
CAR_W, CAR_H = 32, 32

def draw_car(kind):
    S = CAR_W * SS
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    cx, cy = S // 2, S // 2
    L, W = 24 * SS, 12 * SS          # length and width (same as the collision in game)
    x0, x1, y0, y1 = cx - L // 2, cx + L // 2, cy - W // 2, cy + W // 2
    base = CAR_COLORS[kind]
    wreck = kind == 13
    sport = kind in (11, 12)
    # wheels
    wc = (18, 18, 20, 255)
    for wx in (x0 + 4 * SS, x1 - 9 * SS):
        d.rounded_rectangle([wx, y0 - SS, wx + 5 * SS, y0 + 2 * SS], radius=SS, fill=wc)
        d.rounded_rectangle([wx, y1 - 2 * SS, wx + 5 * SS, y1 + SS], radius=SS, fill=wc)
    # body with shading: lighter on top (light from the north-west)
    r = 4 * SS if not sport else 5 * SS
    d.rounded_rectangle([x0, y0, x1, y1], radius=r, fill=rgb(base, 0.62))
    d.rounded_rectangle([x0 + SS, y0 + SS, x1 - SS, y1 - SS // 2], radius=r - SS, fill=rgb(base, 0.85))
    d.rounded_rectangle([x0 + 2 * SS, y0 + SS, x1 - 2 * SS, cy], radius=r - SS, fill=rgb(base, 1.0))
    d.line([x0 + 3 * SS, y0 + SS + 2, x1 - 4 * SS, y0 + SS + 2], fill=rgb(base, 1.25, 30), width=SS)  # highlight along the side
    if wreck:
        for _ in range(40):
            px, py = random.randint(x0, x1), random.randint(y0, y1)
            d.ellipse([px, py, px + 3 * SS, py + 3 * SS], fill=random.choice([(20, 18, 16, 255), (110, 60, 30, 255), (70, 66, 60, 255)]))
        return im
    glass = (40, 60, 86, 255)
    glass_hi = (130, 170, 200, 255)
    # hood and trunk
    if kind == 8:  # police: black hood and trunk
        d.rounded_rectangle([x1 - 7 * SS, y0 + SS, x1 - SS, y1 - SS], radius=2 * SS, fill=(25, 25, 30, 255))
        d.rounded_rectangle([x0 + SS, y0 + SS, x0 + 4 * SS, y1 - SS], radius=2 * SS, fill=(25, 25, 30, 255))
        d.rectangle([x0 + 9 * SS, y0, x1 - 9 * SS, y0 + SS], fill=(30, 60, 150, 255))
        d.rectangle([x0 + 9 * SS, y1 - SS, x1 - 9 * SS, y1], fill=(30, 60, 150, 255))
    # windshield (trapezoid) and rear window
    fx = x1 - 7 * SS
    d.polygon([(fx, y0 + 2 * SS), (fx + 3 * SS, y0 + 3 * SS), (fx + 3 * SS, y1 - 3 * SS), (fx, y1 - 2 * SS)], fill=glass)
    d.line([(fx + SS, y0 + 3 * SS), (fx + 2 * SS, y0 + 5 * SS)], fill=glass_hi, width=SS)
    bx = x0 + 4 * SS
    d.polygon([(bx, y0 + 3 * SS), (bx + 2 * SS, y0 + 2 * SS), (bx + 2 * SS, y1 - 2 * SS), (bx, y1 - 3 * SS)], fill=glass)
    # roof
    roofc = rgb(base, 0.95) if kind != 8 else (238, 238, 238, 255)
    d.rounded_rectangle([bx + 2 * SS, y0 + 2 * SS, fx, y1 - 2 * SS], radius=2 * SS, fill=roofc)
    d.rounded_rectangle([bx + 3 * SS, y0 + 2 * SS + 2, fx - SS, cy - SS], radius=SS, fill=rgb(base, 1.12, 18))
    # side windows
    d.line([(bx + 2 * SS, y0 + 2 * SS), (fx, y0 + 2 * SS)], fill=glass, width=SS)
    d.line([(bx + 2 * SS, y1 - 2 * SS), (fx, y1 - 2 * SS)], fill=glass, width=SS)
    # headlights and brake lights
    hl = (255, 248, 210, 255) if kind != 11 else (130, 240, 255, 255)
    d.rounded_rectangle([x1 - 2 * SS, y0 + SS, x1, y0 + 4 * SS], radius=SS, fill=hl)
    d.rounded_rectangle([x1 - 2 * SS, y1 - 4 * SS, x1, y1 - SS], radius=SS, fill=hl)
    d.rectangle([x0, y0 + SS, x0 + SS, y0 + 4 * SS], fill=(210, 20, 20, 255))
    d.rectangle([x0, y1 - 4 * SS, x0 + SS, y1 - SS], fill=(210, 20, 20, 255))
    # mirrors
    d.rectangle([fx - SS, y0 - SS, fx + SS, y0], fill=rgb(base, 0.6))
    d.rectangle([fx - SS, y1, fx + SS, y1 + SS], fill=rgb(base, 0.6))
    # special cars
    if kind == 8:   # police: light bar base (the game draws the lights)
        d.rectangle([cx - 2 * SS, y0 + 3 * SS, cx, y1 - 3 * SS], fill=(30, 30, 34, 255))
    if kind == 10:  # fire truck: ladder
        for k in range(0, 14 * SS, 2 * SS):
            d.rectangle([bx + k, cy - 2 * SS, bx + k + SS // 2, cy + 2 * SS], fill=(230, 230, 230, 255))
        d.line([(bx, cy - 2 * SS), (fx, cy - 2 * SS)], fill=(230, 230, 230, 255), width=SS // 2)
        d.line([(bx, cy + 2 * SS), (fx, cy + 2 * SS)], fill=(230, 230, 230, 255), width=SS // 2)
    if kind == 11:  # VORTEX GT: gold stripe and spoiler
        d.rectangle([x0, cy - SS // 2 - 1, x1, cy + SS // 2 + 1], fill=(230, 190, 60, 255))
        d.rectangle([x0 - SS, y0, x0 + SS, y1], fill=(15, 15, 18, 255))
    if kind == 12:  # sports car: two stripes
        d.rectangle([x0, cy - 2 * SS, x1, cy - SS], fill=(20, 20, 20, 255))
        d.rectangle([x0, cy + SS, x1, cy + 2 * SS], fill=(20, 20, 20, 255))
    if kind == 9:   # gold: sparkles
        for _ in range(12):
            px, py = random.randint(x0, x1), random.randint(y0, y1)
            d.point((px, py), fill=(255, 250, 200, 255))
    return im

def rotate_down(im, ang_deg, w, h):
    rot = im.rotate(-ang_deg, resample=Image.BICUBIC)
    return rot.resize((w, h), Image.LANCZOS)

# ------------------------------------------------------------------ people
# clothing colors: 0..5 pedestrians, 6 police, 7 suit, 8/9 gangs, 10 robber, 11 player
SHIRTS = [(50, 90, 200), (200, 50, 60), (240, 240, 240), (60, 160, 80), (160, 80, 200), (90, 70, 50),
          (30, 50, 140), (30, 30, 34), (40, 170, 60), (150, 50, 190), (70, 64, 56), (255, 120, 0)]
HAIR = [(40, 28, 20), (90, 60, 30), (20, 20, 20), (170, 140, 80), (60, 40, 30), (30, 30, 30)]
P_W = P_H = 16

def draw_person(ci, frame):
    S = P_W * 8
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    c = S // 2
    sh = SHIRTS[ci]
    pants = (40, 44, 60, 255) if ci not in (6, 7) else (20, 24, 50, 255)
    step = [0, 1, 0, -1][frame]
    # legs (facing right): step forward/back
    for side, s in ((-1, step), (1, -step)):
        lx = c + s * 22
        ly = c + side * 13
        d.ellipse([lx - 10, ly - 7, lx + 10, ly + 7], fill=pants)
        d.ellipse([lx + 2, ly - 6, lx + 14, ly + 6], fill=(25, 22, 20, 255))  # shoe
    # arms in antiphase
    for side, s in ((-1, -step), (1, step)):
        ax = c + s * 16
        ay = c + side * 26
        d.ellipse([ax - 9, ay - 7, ax + 9, ay + 7], fill=rgb(sh, 0.85))
        d.ellipse([ax + 4, ay - 5, ax + 12, ay + 5], fill=(230, 190, 150, 255))
    # shoulders/torso with volume
    d.ellipse([c - 22, c - 30, c + 18, c + 30], fill=rgb(sh, 0.7))
    d.ellipse([c - 20, c - 28, c + 14, c + 22], fill=rgb(sh, 1.0))
    d.ellipse([c - 14, c - 24, c + 4, c - 4], fill=rgb(sh, 1.18, 15))
    if ci == 7:   # suit: white collar
        d.polygon([(c + 6, c - 8), (c + 16, c), (c + 6, c + 8)], fill=(240, 240, 240, 255))
    # head
    d.ellipse([c - 12, c - 13, c + 14, c + 13], fill=(230, 190, 150, 255))
    hair = HAIR[ci % len(HAIR)]
    if ci == 6:     # police cap
        d.ellipse([c - 14, c - 14, c + 12, c + 14], fill=(15, 20, 60, 255))
        d.rectangle([c + 8, c - 8, c + 18, c + 8], fill=(10, 12, 30, 255))
    elif ci in (8, 9):  # bandana
        d.ellipse([c - 13, c - 13, c + 11, c + 13], fill=rgb(sh, 1.1))
        d.ellipse([c - 18, c - 4, c - 8, c + 4], fill=rgb(sh, 0.8))
    elif ci == 10:  # beanie
        d.ellipse([c - 13, c - 13, c + 11, c + 13], fill=(18, 18, 18, 255))
    else:
        d.chord([c - 13, c - 13, c + 13, c + 13], 90, 270, fill=hair + (255,))
    d.ellipse([c - 4, c - 9, c + 4, c - 3], fill=(255, 255, 255, 60))
    return im

# ------------------------------------------------------------------ trees
TREE_SIZES = [18, 18, 24, 24, 30, 30]

def draw_tree(i):
    S = TREE_SIZES[i] * SS
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    c = S / 2
    rnd = random.Random(100 + i)
    pal = [(28, 70, 30), (40, 100, 44), (56, 124, 52), (78, 150, 64), (110, 176, 80)]
    if i % 2 == 0:   # deciduous: foliage clusters
        for layer, k in ((0, 0.62), (1, 0.85), (2, 1.0), (3, 1.0), (4, 1.0)):
            for _ in range(10 if layer < 2 else 7):
                a = rnd.random() * 2 * math.pi
                rr = rnd.random() * c * (0.55 if layer < 2 else 0.4)
                px = c + math.cos(a) * rr - layer * SS * 1.2
                py = c + math.sin(a) * rr - layer * SS * 1.2
                cr = c * (0.42 if layer < 2 else 0.3 - layer * 0.03)
                d.ellipse([px - cr, py - cr, px + cr, py + cr], fill=pal[layer] + (255,))
    else:            # conifer: star-shaped tiers
        for layer in range(5):
            rr = c * (0.95 - layer * 0.17)
            pts = []
            for k in range(16):
                a = k * math.pi / 8 + layer * 0.2
                rad = rr if k % 2 == 0 else rr * 0.7
                pts.append((c + math.cos(a) * rad - layer * SS * 0.6, c + math.sin(a) * rad - layer * SS * 0.6))
            d.polygon(pts, fill=pal[min(4, layer)] + (255,))
    im = im.filter(ImageFilter.SMOOTH)
    return im.resize((TREE_SIZES[i], TREE_SIZES[i]), Image.LANCZOS)

# ------------------------------------------------------------------ slot machine
SLOT_W, SLOT_H = 96, 128

def font(sz):
    try:
        return ImageFont.truetype("DejaVuSans-Bold.ttf", sz)
    except Exception:
        return ImageFont.load_default()

def draw_slot():
    S = 4
    im = Image.new("RGBA", (SLOT_W * S, SLOT_H * S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    W, H = SLOT_W * S, SLOT_H * S
    # cabinet: gradient red -> dark red, gold trim
    for y in range(H):
        k = y / H
        d.line([(16, y), (W - 28, y)], fill=(int(190 - 90 * k), int(30 - 10 * k), int(40 - 10 * k), 255))
    d.rounded_rectangle([16, 0, W - 28, H - 1], radius=40, outline=(240, 200, 70, 255), width=10)
    # display on top
    d.rounded_rectangle([40, 20, W - 52, 110], radius=24, fill=(30, 10, 40, 255), outline=(240, 200, 70, 255), width=6)
    f = font(64)
    t = "LUCKY 7"
    tw = d.textlength(t, font=f)
    d.text(((W - 24 - tw) / 2, 30), t, font=f, fill=(255, 220, 60, 255))
    # reel window (the game draws reels in the rectangle 14..74 x 40..76 in sprite pixels)
    d.rounded_rectangle([14 * S - 8, 40 * S - 8, 74 * S + 8, 76 * S + 8], radius=16, fill=(240, 200, 70, 255))
    d.rectangle([14 * S, 40 * S, 74 * S, 76 * S], fill=(0, 0, 0, 0))
    # button panel
    d.rounded_rectangle([40, 340, W - 52, 420], radius=18, fill=(40, 20, 20, 255))
    for i, col in enumerate([(240, 60, 60), (60, 200, 80), (250, 210, 60)]):
        x = 70 + i * 70
        d.ellipse([x, 360, x + 44, 404], fill=col + (255,), outline=(255, 255, 255, 255), width=4)
    # tray
    d.rounded_rectangle([60, 450, W - 72, 490], radius=12, fill=(25, 25, 25, 255), outline=(200, 170, 60, 255), width=4)
    # lever on the right
    d.rectangle([W - 28, 200, W - 12, 240], fill=(160, 160, 170, 255))
    d.line([(W - 18, 220), (W - 18, 60)], fill=(200, 200, 210, 255), width=10)
    d.ellipse([W - 36, 30, W - 0, 70], fill=(230, 30, 30, 255), outline=(255, 255, 255, 255), width=3)
    return im.resize((SLOT_W, SLOT_H), Image.LANCZOS)

# ------------------------------------------------------------------ logo
LOGO_W, LOGO_H = 240, 72

def draw_logo():
    S = 4
    W, H = LOGO_W * S, LOGO_H * S
    im = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    f = font(210)
    t = "GTA"
    d0 = ImageDraw.Draw(im)
    tw = d0.textlength(t, font=f)
    x = (W - tw) / 2
    # shadow and outline
    sh = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    ImageDraw.Draw(sh).text((x + 14, 8 + 14), t, font=f, fill=(0, 0, 0, 200))
    sh = sh.filter(ImageFilter.GaussianBlur(6))
    im.alpha_composite(sh)
    mask = Image.new("L", (W, H), 0)
    ImageDraw.Draw(mask).text((x, 8), t, font=f, fill=255)
    stroke = Image.new("L", (W, H), 0)
    ImageDraw.Draw(stroke).text((x, 8), t, font=f, fill=255, stroke_width=10, stroke_fill=255)
    im.paste((10, 10, 10, 255), (0, 0), stroke)
    # letter gradient: yellow -> orange -> red
    grad = Image.new("RGBA", (W, H))
    gd = ImageDraw.Draw(grad)
    for y in range(H):
        k = y / H
        gd.line([(0, y), (W, y)], fill=(255, int(230 - 170 * k), int(80 - 60 * k), 255))
    im.paste(grad, (0, 0), mask)
    return im.resize((LOGO_W, LOGO_H), Image.LANCZOS)

# ------------------------------------------------------------------ assembly
def main(out):
    entries = []

    def add(name, frames, w, h):
        data = b"".join(f.convert("RGBA").tobytes() for f in frames)
        assert len(data) == w * h * 4 * len(frames)
        entries.append((name, w, h, len(frames), data))

    cars = []
    for k in range(14):
        base = draw_car(k)
        for a in range(32):
            cars.append(rotate_down(base, a * 360 / 32, CAR_W, CAR_H))
    add("CARS", cars, CAR_W, CAR_H)

    people = []
    for ci in range(len(SHIRTS)):
        for f in range(4):
            base = draw_person(ci, f)
            for a in range(16):
                people.append(rotate_down(base, a * 360 / 16, P_W, P_H))
    # order: color*64 + angle*4 + frame
    ordered = []
    for ci in range(len(SHIRTS)):
        for a in range(16):
            for f in range(4):
                ordered.append(people[ci * 64 + f * 16 + a])
    add("PEOPLE", ordered, P_W, P_H)

    for i in range(6):
        t = draw_tree(i)
        add("TREE%d" % i, [t], t.width, t.height)
    add("SLOT", [draw_slot()], SLOT_W, SLOT_H)
    add("LOGO", [draw_logo()], LOGO_W, LOGO_H)

    hdr = b"GPAK" + struct.pack("<II", 1, len(entries))
    off = len(hdr) + 20 * len(entries)
    table, blob = b"", b""
    for name, w, h, n, data in entries:
        table += struct.pack("<8sHHHHI", name.encode()[:8], w, h, n, 0, off + len(blob))
        blob += data
    with open(out, "wb") as f:
        f.write(hdr + table + blob)
    print("%s: %d entries, %.2f MB" % (out, len(entries), (len(hdr) + len(table) + len(blob)) / 1048576))

    # preview
    prev = Image.new("RGBA", (32 * 16, 32 * 3 + 16 * 4 + 140), (60, 62, 68, 255))
    for k in range(14):
        prev.alpha_composite(cars[k * 32 + 4], ((k % 16) * 32, 0))
        prev.alpha_composite(cars[k * 32 + 0], ((k % 16) * 32, 32))
    for ci in range(12):
        for f in range(4):
            prev.alpha_composite(ordered[ci * 64 + 0 * 4 + f], (ci * 32 + f * 0, 70 + f * 16))
    x = 0
    for i in range(6):
        t = draw_tree(i)
        prev.alpha_composite(t, (x, 140)); x += t.width + 4
    prev.alpha_composite(draw_slot(), (200, 70 + 64))
    prev.alpha_composite(draw_logo(), (260, 140))
    prev.save(out + ".preview.png")

if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "GTA.PAK")
