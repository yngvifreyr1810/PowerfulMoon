"""Size, mean colour and spread of each PNG given (pure Python: zlib + PNG filters), to see which passes have content."""
import struct
import sys
import zlib


def read_png(path):
    data = open(path, 'rb').read()
    assert data[:8] == b'\x89PNG\r\n\x1a\n'
    pos, idat, w = 8, b'', None
    while pos < len(data):
        n, typ = struct.unpack_from('>I4s', data, pos)
        chunk = data[pos + 8:pos + 8 + n]
        if typ == b'IHDR':
            w, h, depth, ctype = struct.unpack_from('>IIBB', chunk)
        elif typ == b'IDAT':
            idat += chunk
        pos += 12 + n
    bpp = {6: 4, 2: 3, 0: 1, 4: 2}[ctype] * depth // 8
    raw = zlib.decompress(idat)
    stride = w * bpp
    rows, prev, i = [], bytearray(stride), 0
    for _ in range(h):
        f = raw[i]; line = bytearray(raw[i + 1:i + 1 + stride]); i += 1 + stride
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b = prev[x]
            cc = prev[x - bpp] if x >= bpp else 0
            if f == 1: line[x] = (line[x] + a) & 255
            elif f == 2: line[x] = (line[x] + b) & 255
            elif f == 3: line[x] = (line[x] + (a + b) // 2) & 255
            elif f == 4:
                pa, pb, pc = abs(b - cc), abs(a - cc), abs(a + b - 2 * cc)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else cc)) & 255
        rows.append(line); prev = line
    return w, h, bpp, rows


for path in sys.argv[1:]:
    w, h, bpp, rows = read_png(path)
    step = max(1, (w * h) // 4000)  # (a sample of the pixels)
    sums, sq, n, k = [0] * bpp, [0] * bpp, 0, 0
    for y in range(h):
        for x in range(w):
            k += 1
            if k % step: continue
            for ch in range(bpp):
                v = rows[y][x * bpp + ch]; sums[ch] += v; sq[ch] += v * v
            n += 1
    mean = [s / n for s in sums]
    sd = [max(0.0, q / n - m * m) ** 0.5 for q, m in zip(sq, mean)]
    print(f'{path.split("/")[-1]:16} {w:5}x{h:<4} mean ' + ' '.join(f'{m:5.1f}' for m in mean) +
          '  sd ' + ' '.join(f'{s:5.1f}' for s in sd))
