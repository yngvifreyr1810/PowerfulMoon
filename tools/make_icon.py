"""The app icon (docs/icon/): a crescent moon in the accent red on the menu's panel colour - our own drawing, no game art.
Geometry: a disc centred on the canvas, its diameter 2/3 of the canvas, minus the same disc moved half its radius right
and up. Colours: the palette of runtime/ui/theme.h. Pure Python (no Pillow): rasterised with 4x4 supersampling, written
as PNG; icon.ico holds PNG images of the Windows sizes, icon.svg is the same drawing as a vector file.
Run: python tools/make_icon.py"""
import os
import struct
import zlib

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'docs', 'icon')
# palette (runtime/ui/theme.h)
PANEL = (0x12, 0x13, 0x16)   # kPanel: the menu window
ACCENT = (0xE5, 0x38, 0x3B)  # kAccent
# geometry, canvas 0..1024: the rounded square, the disc, the cut-out disc
SIZE = 1024
SQUARE_MARGIN, SQUARE_RADIUS = 32, 210
R = SIZE / 3
DISC = (SIZE / 2, SIZE / 2, R)
CUT = (SIZE / 2 + R / 2, SIZE / 2 - R / 2, R)


def in_rrect(x, y, x0, y0, x1, y1, r):
    if x < x0 or x > x1 or y < y0 or y > y1:
        return False
    cx = min(max(x, x0 + r), x1 - r)
    cy = min(max(y, y0 + r), y1 - r)
    return (x - cx) ** 2 + (y - cy) ** 2 <= r * r


def in_circle(x, y, c):
    return (x - c[0]) ** 2 + (y - c[1]) ** 2 <= c[2] * c[2]


def color_at(x, y):
    """Colour of the drawing at (x, y), or None (transparent)."""
    if not in_rrect(x, y, SQUARE_MARGIN, SQUARE_MARGIN, SIZE - SQUARE_MARGIN, SIZE - SQUARE_MARGIN, SQUARE_RADIUS):
        return None
    return ACCENT if in_circle(x, y, DISC) and not in_circle(x, y, CUT) else PANEL


def render(size):
    s = 4  # supersampling
    rows = []
    for py in range(size):
        row = bytearray()
        for px in range(size):
            acc = [0, 0, 0, 0]
            for sy in range(s):
                for sx in range(s):
                    c = color_at((px + (sx + 0.5) / s) * SIZE / size, (py + (sy + 0.5) / s) * SIZE / size)
                    if c:
                        acc[0] += c[0]; acc[1] += c[1]; acc[2] += c[2]; acc[3] += 255
            n = acc[3] // 255
            row += bytes((acc[0] // n, acc[1] // n, acc[2] // n, acc[3] // (s * s)) if n else (0, 0, 0, 0))
        rows.append(bytes(row))
    return rows


def png(size, rows):
    raw = b''.join(b'\x00' + r for r in rows)
    def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 6, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def svg():
    hexc = lambda c: '#%02X%02X%02X' % c
    w = SIZE - 2 * SQUARE_MARGIN
    return f"""<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {SIZE} {SIZE}">
  <defs><mask id="moon"><rect width="{SIZE}" height="{SIZE}" fill="white"/>
    <circle cx="{CUT[0]:.3f}" cy="{CUT[1]:.3f}" r="{CUT[2]:.3f}" fill="black"/></mask></defs>
  <rect x="{SQUARE_MARGIN}" y="{SQUARE_MARGIN}" width="{w}" height="{w}" rx="{SQUARE_RADIUS}" fill="{hexc(PANEL)}"/>
  <circle cx="{DISC[0]:.3f}" cy="{DISC[1]:.3f}" r="{DISC[2]:.3f}" fill="{hexc(ACCENT)}" mask="url(#moon)"/>
</svg>
"""


if __name__ == '__main__':
    os.makedirs(OUT, exist_ok=True)
    sizes = [16, 24, 32, 48, 64, 128, 256]
    images = {n: png(n, render(n)) for n in sizes}
    # ICO: header, one directory entry per size (0 = 256), then the PNG images
    ico = struct.pack('<HHH', 0, 1, len(sizes))
    off = 6 + 16 * len(sizes)
    for n in sizes:
        ico += struct.pack('<BBBBHHII', n % 256, n % 256, 0, 0, 1, 32, len(images[n]), off)
        off += len(images[n])
    ico += b''.join(images[n] for n in sizes)
    open(os.path.join(OUT, 'icon.ico'), 'wb').write(ico)
    open(os.path.join(OUT, 'icon_512.png'), 'wb').write(png(512, render(512)))
    open(os.path.join(OUT, 'icon.svg'), 'w', newline='\n').write(svg())
    print('wrote', OUT)
