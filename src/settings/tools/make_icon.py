"""Draw the Nilesoft Shell Settings logo with Pillow and write a multi-size app.ico.

The shapes mirror res/logo.svg (a 256x256 design grid). Every icon size is rendered
separately at 8x supersampling and reduced with a box filter, so small sizes stay crisp.

    python -m venv D:/Projects/_deps/venv
    D:/Projects/_deps/venv/Scripts/python -m pip install pillow
    D:/Projects/_deps/venv/Scripts/python src/settings/tools/make_icon.py

Writes src/settings/res/app.ico and src/settings/res/logo-256.png.
"""
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

RES = Path(__file__).resolve().parent.parent / "res"
SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]
SS = 8  # supersampling factor


def hex_rgb(h):
    h = h.lstrip("#")
    return tuple(int(h[i:i + 2], 16) for i in (0, 2, 4))


def gradient(size, c0, c1, x0, y0, x1, y1, scale):
    """Linear gradient image (RGBA) along the vector (x0,y0)->(x1,y1) in design units."""
    a, b = hex_rgb(c0), hex_rgb(c1)
    w = h = size
    img = Image.new("RGBA", (w, h))
    px = img.load()
    dx, dy = (x1 - x0), (y1 - y0)
    den = dx * dx + dy * dy
    for y in range(h):
        for x in range(w):
            u, v = x / scale, y / scale
            t = ((u - x0) * dx + (v - y0) * dy) / den
            t = 0.0 if t < 0 else 1.0 if t > 1 else t
            px[x, y] = tuple(int(a[i] + (b[i] - a[i]) * t + 0.5) for i in range(3)) + (255,)
    return img


def quad(p0, c, p1, n=24):
    return [((1 - t) ** 2 * p0[0] + 2 * (1 - t) * t * c[0] + t * t * p1[0],
             (1 - t) ** 2 * p0[1] + 2 * (1 - t) * t * c[1] + t * t * p1[1])
            for t in (i / n for i in range(1, n + 1))]


def render(size):
    big = size * SS
    s = big / 256.0

    def box(x, y, w, h):
        return [x * s, y * s, (x + w) * s - 1, (y + h) * s - 1]

    out = Image.new("RGBA", (big, big), (0, 0, 0, 0))

    # Tile with a diagonal gradient.
    mask = Image.new("L", (big, big), 0)
    ImageDraw.Draw(mask).rounded_rectangle(box(16, 16, 224, 224), radius=56 * s, fill=255)
    grad_small = gradient(64, "#38BDF8", "#4F46E5", 16, 16, 240, 240, 64 / 256.0)
    out.paste(grad_small.resize((big, big), Image.BILINEAR), (0, 0), mask)

    # Soft shadow under the menu card.
    shadow = Image.new("L", (big, big), 0)
    ImageDraw.Draw(shadow).rounded_rectangle(box(42, 84, 120, 128), radius=18 * s, fill=56)
    shadow = shadow.filter(ImageFilter.GaussianBlur(3 * s))
    out.paste(Image.new("RGBA", (big, big), (11, 16, 32, 255)), (0, 0), shadow)

    d = ImageDraw.Draw(out)
    d.rounded_rectangle(box(38, 76, 120, 128), radius=18 * s, fill="#FFFFFF")
    d.rounded_rectangle(box(56, 98, 64, 14), radius=7 * s, fill="#CBD5E1")

    row_mask = Image.new("L", (big, big), 0)
    ImageDraw.Draw(row_mask).rounded_rectangle(box(48, 122, 100, 32), radius=10 * s, fill=255)
    row_grad = gradient(64, "#0EA5E9", "#6366F1", 48, 0, 148, 0, 64 / 256.0)
    out.paste(row_grad.resize((big, big), Image.BILINEAR), (0, 0), row_mask)

    d = ImageDraw.Draw(out)
    d.rounded_rectangle(box(60, 131, 56, 14), radius=7 * s, fill="#FFFFFF")
    d.rounded_rectangle(box(56, 168, 80, 14), radius=7 * s, fill="#CBD5E1")

    # Four-point spark: quadratic curves pulled towards its centre.
    top, right, bottom, left = (186, 30), (224, 68), (186, 106), (148, 68)
    pts = [top]
    pts += quad(top, (190, 64), right)
    pts += quad(right, (190, 72), bottom)
    pts += quad(bottom, (182, 72), left)
    pts += quad(left, (182, 64), top)
    d.polygon([(x * s, y * s) for x, y in pts], fill="#FDE68A")

    return out.resize((size, size), Image.LANCZOS)


def main():
    images = [render(sz) for sz in SIZES]
    largest = images[-1]
    largest.save(RES / "logo-256.png")
    largest.save(RES / "app.ico", format="ICO", sizes=[(sz, sz) for sz in SIZES],
                 append_images=images[:-1])
    print("wrote", RES / "app.ico")


if __name__ == "__main__":
    main()
