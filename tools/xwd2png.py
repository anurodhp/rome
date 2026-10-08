#!/usr/bin/env python3
"""Convert an xwd screenshot (xwd -root) from the Pi to PNG, honouring the file's own format.

    tools/xwd2png.py shot.xwd [shot.png] [--crop X,Y,W,H] [--scale N]

The Pi's X server reports bits_per_pixel 24 with a 4-byte-aligned stride: pixels are packed
3 bytes each, and a row is padded out to bytes_per_line. Reading them as 32-bit words squashes the
image to three quarters of its width (a black strip on the right) and smears the colours.  This
reads the pixel size from the header, so 24 and 32 bits per pixel both come out right.
"""
import struct
import sys
import zlib


def shift(mask):
    return (mask & -mask).bit_length() - 1 if mask else 0


def convert(data):
    names = ("header_size file_version pixmap_format pixmap_depth width height xoffset byte_order "
             "bitmap_unit bitmap_bit_order bitmap_pad bits_per_pixel bytes_per_line visual_class "
             "red_mask green_mask blue_mask bits_per_rgb colormap_entries ncolors").split()
    h = dict(zip(names, struct.unpack(">20I", data[:80])))
    w, ht, bpp, bpl = h["width"], h["height"], h["bits_per_pixel"], h["bytes_per_line"]
    if bpp not in (24, 32):
        raise SystemExit("unsupported bits_per_pixel %d" % bpp)
    if h["visual_class"] not in (4, 5):
        raise SystemExit("not a TrueColor/DirectColor image")
    off = h["header_size"] + h["ncolors"] * 12
    bytes_pp = bpp // 8
    big = h["byte_order"] == 1
    rm, gm, bm = h["red_mask"], h["green_mask"], h["blue_mask"]
    rs, gs, bs = shift(rm), shift(gm), shift(bm)
    rows = []
    for y in range(ht):
        line = data[off + y * bpl: off + y * bpl + w * bytes_pp]
        out = bytearray()
        for x in range(w):
            px = line[x * bytes_pp:(x + 1) * bytes_pp]
            v = int.from_bytes(px, "big" if big else "little")
            out += bytes(((v & rm) >> rs, (v & gm) >> gs, (v & bm) >> bs))
        rows.append(b"\x00" + bytes(out))
    return w, ht, b"".join(rows)


def write_png(path, w, h, raw):
    def chunk(tag, body):
        c = struct.pack(">I", len(body)) + tag + body
        return c + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def crop_scale(w, h, raw, crop, scale):
    """Crop (x, y, w, h) and enlarge by an integer factor (nearest neighbour), for reading small text."""
    stride = 1 + w * 3
    x0, y0, cw, ch = crop if crop else (0, 0, w, h)
    rows = []
    for y in range(y0, min(h, y0 + ch)):
        row = raw[y * stride + 1 + x0 * 3: y * stride + 1 + min(w, x0 + cw) * 3]
        if scale > 1:
            row = b"".join(row[i:i + 3] * scale for i in range(0, len(row), 3))
        rows += [b"\x00" + row] * scale
    return (min(w, x0 + cw) - x0) * scale, len(rows), b"".join(rows)


if __name__ == "__main__":
    args = [a for a in sys.argv[1:]]
    crop, scale = None, 1
    if "--crop" in args:
        i = args.index("--crop")
        crop = tuple(int(v) for v in args[i + 1].split(","))
        del args[i:i + 2]
    if "--scale" in args:
        i = args.index("--scale")
        scale = int(args[i + 1])
        del args[i:i + 2]
    if not args:
        raise SystemExit(__doc__)
    src = args[0]
    dst = args[1] if len(args) > 1 else src.rsplit(".", 1)[0] + ".png"
    w, h, raw = convert(open(src, "rb").read())
    if crop or scale > 1:
        w, h, raw = crop_scale(w, h, raw, crop, scale)
    write_png(dst, w, h, raw)
    print("%s: %dx%d" % (dst, w, h))
