"""Build the app icons for every platform from the 32 x 32 pixel-art master, port/icon/apple.png.

Usage: python tools/make_icons.py

Writes (all committed, so builds need nothing but the results):
  port/icon/coi.ico            Windows: 16-256 pixels, built into coi.exe (port/icon/coi.rc)
  port/icon/coi.png            256 x 256, for Linux (Steam's non-Steam game icon, desktop files)
  android/app/src/main/res/    launcher icons: legacy mipmaps, and an adaptive icon (Android 8+)
                               with a themed monochrome layer (Android 13+)

The art is enlarged by whole numbers only, so its pixels stay square and sharp; 16 pixels,
smaller than the art, averages each 2 x 2 block instead. Standard library only.
"""
import struct
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MASTER = ROOT / "port" / "icon" / "apple.png"
RES = ROOT / "android" / "app" / "src" / "main" / "res"
# Behind the adaptive icon's apple: a dark night blue.
BACKGROUND = "#1E2A4A"


def read_png(path):
    """An 8-bit RGBA, non-interlaced PNG (as the master is saved) as rows of (r, g, b, a)."""
    data = path.read_bytes()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", path
    pos, idat = 8, b""
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            width, height, depth, color, _, _, interlace = struct.unpack(">IIBBBBB", body)
            assert depth == 8 and color == 6 and interlace == 0, "the master must be 8-bit RGBA"
        elif kind == b"IDAT":
            idat += body
        pos += 12 + length
    raw = zlib.decompress(idat)
    stride = width * 4
    rows, prev = [], bytearray(stride)
    for y in range(height):
        kind = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - 4] if i >= 4 else 0
            b = prev[i]
            c = prev[i - 4] if i >= 4 else 0
            if kind == 1:
                line[i] = (line[i] + a) & 255
            elif kind == 2:
                line[i] = (line[i] + b) & 255
            elif kind == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif kind == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append([tuple(line[x * 4:x * 4 + 4]) for x in range(width)])
        prev = line
    return rows


def png_bytes(rows):
    height, width = len(rows), len(rows[0])
    raw = b"".join(b"\0" + bytes(v for px in row for v in px) for row in rows)

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def enlarge(rows, scale):
    return [[px for px in row for _ in range(scale)] for row in rows for _ in range(scale)]


def halve(rows):
    """Each 2 x 2 block averaged, colours weighted by their opacity."""
    out = []
    for y in range(0, len(rows), 2):
        line = []
        for x in range(0, len(rows[0]), 2):
            block = [rows[y][x], rows[y][x + 1], rows[y + 1][x], rows[y + 1][x + 1]]
            alpha = sum(p[3] for p in block)
            if alpha == 0:
                line.append((0, 0, 0, 0))
                continue
            line.append(tuple(round(sum(p[i] * p[3] for p in block) / alpha) for i in range(3))
                        + (round(alpha / 4),))
        out.append(line)
    return out


def place(rows, size):
    """`rows` centred on a transparent size x size canvas."""
    top, left = (size - len(rows)) // 2, (size - len(rows[0])) // 2
    canvas = [[(0, 0, 0, 0)] * size for _ in range(size)]
    for y, row in enumerate(rows):
        canvas[top + y][left:left + len(row)] = row
    return canvas


def at(art, size, fill):
    """The art enlarged by the largest whole number that keeps it within fill * size."""
    scale = max(1, int(size * fill) // len(art))
    return place(enlarge(art, scale), size)


def write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    print(path.relative_to(ROOT))


def main():
    art = read_png(MASTER)
    assert len(art) == 32 and len(art[0]) == 32, "the master is 32 x 32"

    # Windows: an .ico of PNG images.
    images = [png_bytes(halve(art))] + [png_bytes(enlarge(art, s)) for s in (1, 2, 4, 8)]
    sizes = [16, 32, 64, 128, 256]
    header = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    entries = b""
    for size, data in zip(sizes, images):
        entries += struct.pack("<BBBBHHII", size % 256, size % 256, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    write(ROOT / "port" / "icon" / "coi.ico", header + entries + b"".join(images))

    # Linux.
    write(ROOT / "port" / "icon" / "coi.png", png_bytes(enlarge(art, 8)))

    # Android. Legacy launcher icons (before Android 8): the apple at most 5/6 of the icon.
    # Adaptive icons: a 108 dp foreground, of which only the middle 66 dp is sure to show.
    # The monochrome layer (themed icons): the outline and the shine, as line art.
    densities = {"mdpi": 1, "hdpi": 1.5, "xhdpi": 2, "xxhdpi": 3, "xxxhdpi": 4}
    mono_art = [[(255, 255, 255, 255) if px[3] and (px[:3] == (0, 0, 0) or px[:3] == (255, 255, 255))
                 else (0, 0, 0, 0) for px in row] for row in art]
    for name, density in densities.items():
        legacy = round(48 * density)
        write(RES / f"mipmap-{name}" / "ic_launcher.png", png_bytes(at(art, legacy, 5 / 6)))
        layer = round(108 * density)
        write(RES / f"mipmap-{name}" / "ic_launcher_foreground.png", png_bytes(at(art, layer, 62 / 108)))
        write(RES / f"mipmap-{name}" / "ic_launcher_monochrome.png", png_bytes(at(mono_art, layer, 62 / 108)))
    write(RES / "mipmap-anydpi-v26" / "ic_launcher.xml", (
        '<?xml version="1.0" encoding="utf-8"?>\n'
        '<!-- Generated by tools/make_icons.py. -->\n'
        '<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">\n'
        '    <background android:drawable="@color/ic_launcher_background" />\n'
        '    <foreground android:drawable="@mipmap/ic_launcher_foreground" />\n'
        '    <monochrome android:drawable="@mipmap/ic_launcher_monochrome" />\n'
        '</adaptive-icon>\n').encode())
    write(RES / "values" / "ic_launcher_background.xml", (
        '<?xml version="1.0" encoding="utf-8"?>\n'
        '<!-- Generated by tools/make_icons.py. -->\n'
        '<resources>\n'
        f'    <color name="ic_launcher_background">{BACKGROUND}</color>\n'
        '</resources>\n').encode())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
