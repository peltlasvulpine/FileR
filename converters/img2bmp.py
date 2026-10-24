#!/usr/bin/env python3
"""Convert a PNG to a 24-bit BMP that FileR's image viewer can open.

Standard library only, so it runs on Windows and Linux with nothing to install
(Python 3.6 or newer).

Usage:
    python img2bmp.py picture.png [output.bmp] [--width 320] [--height 240]

The picture is scaled to fit inside width x height (default 320x240, the
calculator screen), keeping its proportions. It is never enlarged.
Transparent pixels are placed on a white background.

JPEG, GIF and other formats are not read: save or export the picture as PNG
first (any image editor, or the Save As / Export menu of most viewers).
"""
import argparse
import os
import struct
import zlib

VIEWER_MAX_WIDTH = 640  # image_viewer.c rejects anything wider

# PNG color type -> number of samples per pixel
CHANNELS = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}


def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    if pb <= pc:
        return b
    return c


def unfilter(raw, width_bytes, bpp, height):
    """Undoes PNG row filters. Returns a list of rows (bytearrays)."""
    rows = []
    prev = bytearray(width_bytes)
    pos = 0
    for _ in range(height):
        ftype = raw[pos]
        line = bytearray(raw[pos + 1:pos + 1 + width_bytes])
        pos += 1 + width_bytes
        if ftype == 1:
            for i in range(bpp, width_bytes):
                line[i] = (line[i] + line[i - bpp]) & 0xFF
        elif ftype == 2:
            for i in range(width_bytes):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif ftype == 3:
            for i in range(width_bytes):
                left = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 0xFF
        elif ftype == 4:
            for i in range(width_bytes):
                a = line[i - bpp] if i >= bpp else 0
                c = prev[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + paeth(a, prev[i], c)) & 0xFF
        elif ftype != 0:
            raise ValueError("corrupt PNG (filter type %d)" % ftype)
        rows.append(line)
        prev = line
    return rows


def row_values(line, depth, count):
    """Splits a filtered row into `count` sample values of `depth` bits each."""
    if depth == 8:
        return list(line[:count])
    if depth == 16:
        return list(line[0:2 * count:2])  # keep the high byte of each sample
    out = []
    mask = (1 << depth) - 1
    per_byte = 8 // depth
    for byte in line:
        for k in range(per_byte):
            shift = 8 - depth * (k + 1)
            out.append((byte >> shift) & mask)
            if len(out) == count:
                return out
    return out[:count]


def read_png(path):
    """Returns (width, height, rows) where rows are top-down RGB bytearrays."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG file: save or export the picture as PNG first")

    pos = 8
    idat = []
    palette = b""
    width = height = depth = ctype = interlace = None
    while pos + 8 <= len(data):
        length = struct.unpack(">I", data[pos:pos + 4])[0]
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            width, height, depth, ctype, _comp, _filt, interlace = struct.unpack(">IIBBBBB", body)
        elif kind == b"PLTE":
            palette = body
        elif kind == b"IDAT":
            idat.append(body)
        elif kind == b"IEND":
            break

    if width is None:
        raise ValueError("corrupt PNG: no header")
    if interlace:
        raise ValueError("interlaced PNG is not supported: re-save it without interlacing")
    if ctype not in CHANNELS:
        raise ValueError("unsupported PNG color type %d" % ctype)
    if ctype == 3 and not palette:
        raise ValueError("corrupt PNG: palette missing")

    ch = CHANNELS[ctype]
    bits_per_pixel = ch * depth
    bpp = max(1, bits_per_pixel // 8)
    width_bytes = (width * bits_per_pixel + 7) // 8
    raw = zlib.decompress(b"".join(idat))
    lines = unfilter(raw, width_bytes, bpp, height)

    maxv = (1 << depth) - 1
    rows = []
    for line in lines:
        vals = row_values(line, depth, width * ch)
        rgb = bytearray(width * 3)
        for x in range(width):
            if ctype == 3:                       # palette index
                i = vals[x] * 3
                r, g, b = (palette[i:i + 3] + b"\0\0\0")[:3]
            elif ctype in (0, 4):                # gray, or gray + alpha
                v = vals[x * ch]
                if depth < 8:
                    v = v * 255 // maxv
                r = g = b = v
                if ctype == 4:
                    a = vals[x * ch + 1]
                    if depth < 8:
                        a = a * 255 // maxv
                    r = g = b = (v * a + 255 * (255 - a)) // 255
            else:                                # RGB or RGBA
                r, g, b = vals[x * ch], vals[x * ch + 1], vals[x * ch + 2]
                if ctype == 6:
                    a = vals[x * ch + 3]
                    r = (r * a + 255 * (255 - a)) // 255
                    g = (g * a + 255 * (255 - a)) // 255
                    b = (b * a + 255 * (255 - a)) // 255
            rgb[3 * x:3 * x + 3] = bytes((r, g, b))
        rows.append(rgb)
    return width, height, rows


def fit(width, height, max_w, max_h):
    scale = min(1.0, max_w / width, max_h / height)
    return max(1, int(width * scale)), max(1, int(height * scale))


def resize(rows, w, h, nw, nh):
    """Nearest-neighbor scale of top-down RGB rows."""
    out = []
    for y in range(nh):
        src = rows[y * h // nh]
        dst = bytearray(nw * 3)
        for x in range(nw):
            sx = x * w // nw
            dst[3 * x:3 * x + 3] = src[3 * sx:3 * sx + 3]
        out.append(dst)
    return out


def write_bmp(path, rows, w, h):
    """Writes a 24-bit, uncompressed, bottom-up BMP."""
    row_size = (w * 3 + 3) & ~3
    pad = b"\0" * (row_size - w * 3)
    pixels = bytearray()
    for row in reversed(rows):
        bgr = bytearray(row)
        bgr[0::3], bgr[2::3] = row[2::3], row[0::3]   # RGB -> BGR
        pixels += bgr + pad
    file_header = struct.pack("<2sIHHI", b"BM", 54 + len(pixels), 0, 0, 54)
    info_header = struct.pack("<IiiHHIIiiII", 40, w, h, 1, 24, 0, len(pixels),
                              2835, 2835, 0, 0)
    with open(path, "wb") as f:
        f.write(file_header + info_header + bytes(pixels))


def main():
    p = argparse.ArgumentParser(description="Make a FileR-compatible BMP from a PNG.")
    p.add_argument("input", help="PNG file")
    p.add_argument("output", nargs="?", help="output .bmp (default: input name + .bmp)")
    p.add_argument("--width", type=int, default=320, help="max width (default 320)")
    p.add_argument("--height", type=int, default=240, help="max height (default 240)")
    args = p.parse_args()

    out = args.output or os.path.splitext(args.input)[0] + ".bmp"
    max_w = min(args.width, VIEWER_MAX_WIDTH)

    w, h, rows = read_png(args.input)
    nw, nh = fit(w, h, max_w, args.height)
    if (nw, nh) != (w, h):
        rows = resize(rows, w, h, nw, nh)
    write_bmp(out, rows, nw, nh)
    print("wrote %s (%dx%d, 24-bit)" % (out, nw, nh))


if __name__ == "__main__":
    main()
