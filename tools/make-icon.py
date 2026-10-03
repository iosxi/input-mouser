"""input-mouser のアイコンを作る。

並んだ 2 台の画面と、その境目をまたぐマウス カーソル。
「隣の PC へ移る」を表す。操作中(トレイ用)は右の画面が光る。

    python tools/make-icon.py   -> src/input-mouser.ico, src/input-mouser-active.ico
"""
from pathlib import Path
from PIL import Image, ImageDraw

SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]
SS = 8  # 縦横 8 倍で描いて縮める

BEZEL = (52, 58, 68, 255)
SCREEN = (233, 239, 247, 255)
LIT = (76, 194, 255, 255)
CURSOR = (255, 255, 255, 255)
CURSOR_EDGE = (20, 20, 20, 255)


def draw(size: int, active: bool) -> Image.Image:
    s = size * SS
    im = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    u = s / 32.0
    small = size <= 24

    def monitor(x0, lit):
        y0, w, h = 8.6 * u, 14 * u, 11 * u
        b = 1.6 * u if small else 1.3 * u
        d.rounded_rectangle([x0, y0, x0 + w, y0 + h], 1.6 * u, fill=BEZEL)
        d.rounded_rectangle([x0 + b, y0 + b, x0 + w - b, y0 + h - b], 0.8 * u,
                            fill=LIT if lit else SCREEN)
        # 台
        cx = x0 + w / 2
        d.rectangle([cx - 1.0 * u, y0 + h, cx + 1.0 * u, y0 + h + 2.6 * u], fill=BEZEL)
        d.rounded_rectangle([cx - 3.6 * u, y0 + h + 2.4 * u, cx + 3.6 * u, y0 + h + 3.8 * u],
                            0.6 * u, fill=BEZEL)

    monitor(1 * u, False)
    monitor(17 * u, active)

    # カーソル(左の画面から右の画面へまたぐ位置)
    k = (0.98 if small else 0.80) * u
    ox, oy = (12.4 if small else 13.0) * u, (10.7 if small else 11.1) * u
    pts = [(0, 0), (0, 16), (4, 12.4), (6.8, 18.6), (9.4, 17.5), (6.7, 11.4), (11.6, 11.4)]
    poly = [(ox + x * k, oy + y * k) for x, y in pts]
    d.polygon(poly, fill=CURSOR, outline=CURSOR_EDGE, width=max(SS, int((1.9 if small else 1.3) * u)))

    return im.resize((size, size), Image.LANCZOS)


def save(name: str, active: bool):
    out = Path(__file__).resolve().parent.parent / "src" / name
    images = [draw(n, active) for n in SIZES]
    images[-1].save(out, format="ICO", sizes=[(n, n) for n in SIZES], append_images=images[:-1])
    if not active:
        images[-1].save(out.with_suffix(".png"))
    print("wrote", out)


def main():
    save("input-mouser.ico", False)
    save("input-mouser-active.ico", True)


if __name__ == "__main__":
    main()
