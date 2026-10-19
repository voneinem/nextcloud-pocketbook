#!/bin/sh
# Generates the two 122x122, 8 bpp, opaque BMP launcher icons (normal and focused) into
# res/icons/. Needs python3 only (no imaging library). The generated files are committed.
# The glyph has no outer frame (spec 010), so it sits with the reader's own frameless line-art icons.
#
#   scripts/make-icons.sh
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/res/icons"
mkdir -p "$OUT"
python3 -I - "$OUT" <<'PY'
import struct, sys

W = H = 122
S = 4                      # supersampling factor: drawn at 4x, box-filtered down, so strokes are anti-aliased

def canvas():
    return [[0] * (W * S) for _ in range(H * S)]   # 0 = background, 1 = ink

def rect(px, x0, y0, x1, y1):
    for y in range(max(0, y0 * S), min(H * S, y1 * S)):
        row = px[y]
        for x in range(max(0, x0 * S), min(W * S, x1 * S)):
            row[x] = 1

def inside_round_rect(x, y, x0, y0, x1, y1, r):
    # x, y in supersampled units; the box and the radius in canvas pixels
    X0, Y0, X1, Y1, R = x0 * S, y0 * S, x1 * S, y1 * S, r * S
    if x < X0 or x >= X1 or y < Y0 or y >= Y1:
        return False
    cx = X0 + R if x < X0 + R else (X1 - R if x >= X1 - R else x)
    cy = Y0 + R if y < Y0 + R else (Y1 - R if y >= Y1 - R else y)
    return (x - cx) ** 2 + (y - cy) ** 2 <= R * R

def draw():
    """The cloud glyph, no frame: a cloud outline (3 px) made of three round puffs on a rounded base.
    The built-in icons of the reader are line art of about 75-90 px in a 122 px cell; this one is 95 x 83."""
    px = canvas()
    t = 3.5
    circles = ((41.8, 63.4, 21.6), (64.6, 49, 28.8), (87.4, 64.8, 20.4))
    base = (14.2, 63.4, 107.8, 101.8, 19.2)                           # x0, y0, x1, y1, radius

    def inside(x, y, d):
        # the cloud with every part shrunk by d canvas pixels (d = 0 is the full shape)
        for cx, cy, r in circles:
            if (x - cx * S) ** 2 + (y - cy * S) ** 2 <= ((r - d) * S) ** 2:
                return True
        x0, y0, x1, y1, r = base
        return inside_round_rect(x, y, x0 + d, y0 + d, x1 - d, y1 - d, max(0, r - d))

    for y in range(H * S):
        row = px[y]
        for x in range(W * S):
            if inside(x, y, 0) and not inside(x, y, t):
                row[x] = 1
    return px

def down(px, invert):
    out = [[0] * W for _ in range(H)]
    for y in range(H):
        for x in range(W):
            n = 0
            for dy in range(S):
                n += sum(px[y * S + dy][x * S:(x + 1) * S])
            ink = n / float(S * S)                          # 0..1
            v = int(round(255 * (1 - ink)))
            out[y][x] = 255 - v if invert else v
    return out

def write_bmp(path, px):
    row = (W + 3) & ~3                              # rows padded to 4 bytes
    pal = b"".join(struct.pack("<BBBB", i, i, i, 0) for i in range(256))
    data = b"".join(bytes(px[y]) + b"\0" * (row - W) for y in range(H - 1, -1, -1))
    off = 14 + 40 + len(pal)
    hdr = b"BM" + struct.pack("<IHHI", off + len(data), 0, 0, off)
    info = struct.pack("<IiiHHIIiiII", 40, W, H, 1, 8, 0, len(data), 2835, 2835, 256, 0)
    with open(path, "wb") as f:
        f.write(hdr + info + pal + data)

out = sys.argv[1]
glyph = draw()
write_bmp(out + "/pbcloud.bmp", down(glyph, False))   # black on white
write_bmp(out + "/pbcloud_f.bmp", down(glyph, True))  # focused: inverted
PY
echo "wrote $OUT/pbcloud.bmp and $OUT/pbcloud_f.bmp"
