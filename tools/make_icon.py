"""Draws the Plugin Analyzer app icon (Resources/icon.png, 1024x1024).

Usage:  python3 tools/make_icon.py        (requires Pillow: pip install pillow)

Drawn at 4x and downsampled for smooth edges. Layout follows the macOS icon grid:
an 824 px rounded square centred on a 1024 px canvas.
"""
import math
import os
from PIL import Image, ImageDraw, ImageFilter

S = 4                      # supersampling factor
N = 1024 * S               # canvas size
ORANGE = (255, 159, 28)
TEAL = (46, 196, 182)


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(len(a)))


def vertical_gradient(size, top, bottom):
    w, h = size
    g = Image.new("RGBA", size)
    px = g.load()
    for y in range(h):
        c = lerp(top, bottom, y / max(1, h - 1)) + (255,)
        for x in range(w):
            px[x, y] = c
    return g


def rounded_mask(size, box, radius):
    m = Image.new("L", size, 0)
    ImageDraw.Draw(m).rounded_rectangle(box, radius=radius, fill=255)
    return m


def main():
    canvas = Image.new("RGBA", (N, N), (0, 0, 0, 0))

    # --- body: rounded square with a soft drop shadow -------------------------------
    margin = 100 * S
    body = (margin, margin, N - margin, N - margin)
    radius = int(0.225 * (N - 2 * margin))

    shadow = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    ImageDraw.Draw(shadow).rounded_rectangle((body[0], body[1] + 18 * S, body[2], body[3] + 18 * S),
                                             radius=radius, fill=(0, 0, 0, 150))
    canvas.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(28 * S)))

    grad = vertical_gradient((N, N), (52, 57, 70), (16, 18, 23))
    canvas.paste(grad, (0, 0), rounded_mask((N, N), body, radius))

    # subtle top highlight
    hl = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    ImageDraw.Draw(hl).rounded_rectangle(body, radius=radius, outline=(255, 255, 255, 40), width=4 * S)
    canvas.alpha_composite(hl)

    # --- screen: the plot area ------------------------------------------------------
    sm = 70 * S
    screen = (body[0] + sm, body[1] + sm, body[2] - sm, body[3] - sm)
    sr = int(radius * 0.55)
    scr = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    ImageDraw.Draw(scr).rounded_rectangle(screen, radius=sr, fill=(11, 12, 16, 255),
                                          outline=(255, 255, 255, 22), width=3 * S)
    canvas.alpha_composite(scr)

    x0, y0, x1, y1 = screen[0] + 40 * S, screen[1] + 40 * S, screen[2] - 40 * S, screen[3] - 40 * S
    pw, ph = x1 - x0, y1 - y0
    clip = rounded_mask((N, N), screen, sr)

    # log-frequency grid
    grid = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    gd = ImageDraw.Draw(grid)
    for decade in range(3):
        for m in range(1, 10):
            t = (decade + math.log10(m)) / 3.0
            x = x0 + t * pw
            strong = m == 1
            gd.line([(x, y0), (x, y1)], fill=(255, 255, 255, 34 if strong else 14), width=(3 if strong else 2) * S)
    for i in range(1, 6):
        y = y0 + i * ph / 6
        gd.line([(x0, y), (x1, y)], fill=(255, 255, 255, 18), width=2 * S)
    canvas.paste(grid, (0, 0), Image.composite(grid, Image.new("RGBA", (N, N)), clip).split()[3])

    # harmonic bars (teal): fundamental + decaying harmonics on the log axis
    bars = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    bd = ImageDraw.Draw(bars)
    f0_t = 0.18
    for k in range(1, 9):
        t = f0_t + math.log10(k) / 3.0 * 2.2
        if t > 0.97:
            break
        x = x0 + t * pw
        level = 0.62 * (0.72 ** (k - 1)) if k > 1 else 0.78
        top = y1 - level * ph
        bw = (34 if k == 1 else 26) * S
        for i in range(int(y1 - top)):
            yy = y1 - i
            a = int(210 * (0.35 + 0.65 * i / max(1, (y1 - top))))
            bd.line([(x - bw / 2, yy), (x + bw / 2, yy)], fill=TEAL + (a,))
        bd.rounded_rectangle((x - bw / 2, top, x + bw / 2, top + bw * 0.6), radius=bw / 2, fill=TEAL + (235,))
    canvas.alpha_composite(bars)

    # frequency response (orange) with a resonant bump, plus glow
    pts = []
    for i in range(0, 2401):
        t = i / 2400
        bump = 0.30 * math.exp(-((t - 0.58) / 0.11) ** 2)
        lowcut = -0.22 * math.exp(-(t / 0.08) ** 2)
        hicut = -0.30 * max(0.0, (t - 0.86) / 0.14) ** 2
        v = 0.56 + bump + lowcut + hicut
        pts.append((x0 + t * pw, y1 - v * ph))

    def stroke(image, width, colour):
        # A round brush stamped densely along the curve: smooth, with round caps and joins.
        d = ImageDraw.Draw(image)
        r = width / 2
        for (px, py) in pts:
            d.ellipse((px - r, py - r, px + r, py + r), fill=colour)

    glow = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    stroke(glow, 40 * S, ORANGE + (200,))
    canvas.alpha_composite(glow.filter(ImageFilter.GaussianBlur(26 * S)))

    curve = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    stroke(curve, 17 * S, ORANGE + (255,))
    canvas.alpha_composite(curve)

    # keep everything inside the body shape
    final = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    body_mask = rounded_mask((N, N), (body[0] - 60 * S, body[1] - 60 * S, body[2] + 60 * S, body[3] + 90 * S), radius)
    final.paste(canvas, (0, 0), body_mask)

    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "Resources", "icon.png")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    final.resize((1024, 1024), Image.LANCZOS).save(out)
    print("wrote", os.path.normpath(out))


if __name__ == "__main__":
    main()
