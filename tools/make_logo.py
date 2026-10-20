#!/usr/bin/env python3
"""Draws the LocalSend mark (solid disc inside a ring of 8 rounded dashes) in two
period styles, pure Python, no dependencies:

  * Early Mac OS X (Aqua): glossy gel on a glass plate -> assets/AppIcon.icns
  * Mac OS 9 (Platinum): flat 8-bit pixel art -> assets/os9/localsend_icons.r (Rez source)

Run:  python3 -I tools/make_logo.py
"""
import math, os, struct, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "assets")
OS9 = os.path.join(OUT, "os9")

# ---- geometry, as ratios of the ring's outer radius (measured from the real logo)
R_THICK = 0.23          # ring thickness
R_DISC = 0.52           # centre disc radius
DASH_DEG = 36.0         # angular width of one dash (8 dashes, 45 degree pitch)


def smooth(d, w):
    return max(0.0, min(1.0, 0.5 - d / w))


def sd_round_box(px, py, hx, hy, r):
    qx, qy = abs(px) - hx + r, abs(py) - hy + r
    return math.hypot(max(qx, 0.0), max(qy, 0.0)) + min(max(qx, qy), 0.0) - r


def logo_sd(dx, dy, R, dash_deg=DASH_DEG, rc=None, ring=R_THICK, disc=R_DISC):
    """Signed distance (same units as R) to the mark centred on the origin."""
    r = math.hypot(dx, dy)
    d_disc = r - disc * R
    thick = ring * R
    rmid = R - thick / 2
    th = math.atan2(dy, dx)
    k = round(th / (math.pi / 4))
    dth = th - k * math.pi / 4
    a = math.radians(dash_deg) / 2 * rmid
    d_dash = sd_round_box(dth * rmid, r - rmid, a, thick / 2, rc if rc is not None else 0.28 * thick)
    return min(d_disc, d_dash)


# ---------------------------------------------------------------- png / icns bits
def to8(v):
    return max(0, min(255, int(round(v * 255))))


def png(px):
    h, wd = len(px), len(px[0])
    raw = b"".join(b"\x00" + bytes(c for (r, g, b, a) in row
                                   for c in (to8(r), to8(g), to8(b), to8(a))) for row in px)

    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", wd, h, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def rle(data):
    out, i = bytearray(), 0
    while i < len(data):
        run = 1
        while i + run < len(data) and data[i + run] == data[i] and run < 130:
            run += 1
        if run >= 3:
            out += bytes([run + 125, data[i]])
            i += run
        else:
            lit = bytearray()
            while i < len(data) and len(lit) < 128:
                if i + 2 < len(data) and data[i] == data[i + 1] == data[i + 2]:
                    break
                lit.append(data[i])
                i += 1
            out += bytes([len(lit) - 1]) + lit
    return bytes(out)


def elem(tag, data):
    return tag + struct.pack(">I", len(data) + 8) + data


def classic(px, rgb_tag, mask_tag, pad=b""):
    flat = [p for row in px for p in row]
    chans = [rle(bytes(to8(p[k]) for p in flat)) for k in range(3)]
    return elem(rgb_tag, pad + b"".join(chans)) + elem(mask_tag, bytes(to8(p[3]) for p in flat))


def downscale(px, n):
    N = len(px)
    out = []
    for j in range(n):
        row = []
        y0, y1 = j * N / n, (j + 1) * N / n
        for i in range(n):
            x0, x1 = i * N / n, (i + 1) * N / n
            ra = ga = ba = aa = 0.0
            cnt = 0
            for yy in range(int(y0), min(N, int(math.ceil(y1)))):
                for xx in range(int(x0), min(N, int(math.ceil(x1)))):
                    r, g, b, a = px[yy][xx]
                    ra += r * a; ga += g * a; ba += b * a; aa += a; cnt += 1
            row.append((ra / aa, ga / aa, ba / aa, aa / cnt) if aa else (0, 0, 0, 0))
        out.append(row)
    return out


# ------------------------------------------------------------------- Aqua style
def over(dst, src):
    sr, sg, sb, sa = src
    dr, dg, db, da = dst
    oa = sa + da * (1 - sa)
    if oa <= 0:
        return (0, 0, 0, 0)
    return ((sr * sa + dr * da * (1 - sa)) / oa, (sg * sa + dg * da * (1 - sa)) / oa,
            (sb * sa + db * da * (1 - sa)) / oa, oa)


def lerp(a, b, t):
    return a + (b - a) * t


def mix(c0, c1, t):
    return tuple(lerp(c0[k], c1[k], t) for k in range(3))


def render_aqua(N=512, flat=False):
    px = [[(0, 0, 0, 0)] * N for _ in range(N)]
    w = 1.6 / N
    R = 0.31                      # ring outer radius (canvas units)
    plate_r = 0.47
    teal_top, teal_mid, teal_bot = (0.42, 0.86, 0.86), (0.0, 0.50, 0.50), (0.0, 0.30, 0.32)
    for j in range(N):
        y = (j + 0.5) / N
        for i in range(N):
            x = (i + 0.5) / N
            dx, dy = x - 0.5, y - 0.5
            r = math.hypot(dx, dy)
            c = (0, 0, 0, 0)
            if flat:
                a = smooth(logo_sd(dx, dy, 0.40), w)
                px[j][i] = (0.0, 0.5, 0.5, a)
                continue
            # drop shadow of the plate
            sh = smooth(math.hypot(dx, dy - 0.028) - plate_r, 0.07) * 0.45
            c = over(c, (0, 0, 0, sh))
            # glass plate
            sd_p = r - plate_r
            a_p = smooth(sd_p, w)
            if a_p > 0:
                t = y
                col = mix((1.0, 1.0, 1.0), (0.80, 0.85, 0.90), max(0.0, min(1.0, (t - 0.1) / 0.8)))
                rim = math.exp(-max(0.0, -sd_p) / 0.012)
                col = mix(col, (0.45, 0.52, 0.60), rim * 0.85)
                # glossy cap on the plate
                yb = 0.40 - 0.10 * (dx / plate_r) ** 2
                if y < yb + 0.05:
                    gt = smooth(y - yb, 0.012)
                    gl = gt * (0.75 - 0.55 * max(0.0, min(1.0, (y - 0.04) / (yb - 0.04))))
                    col = mix(col, (1, 1, 1), gl)
                c = over(c, (col[0], col[1], col[2], a_p))
            # logo drop shadow
            ls = smooth(logo_sd(dx - 0.006, dy - 0.016, R), 0.035) * 0.35
            c = over(c, (0.0, 0.1, 0.12, ls))
            # logo gel
            sd = logo_sd(dx, dy, R)
            a_l = smooth(sd, w)
            if a_l > 0:
                t = max(0.0, min(1.0, (y - (0.5 - R)) / (2 * R)))
                col = mix(teal_top, teal_mid, min(1.0, t * 1.6)) if t < 0.625 else mix(teal_mid, teal_bot, (t - 0.625) / 0.375)
                depth = max(0.0, -sd)
                col = mix(col, (0.0, 0.18, 0.2), math.exp(-depth / 0.008) * 0.55)   # dark rim
                # bright inner glow along the bottom edge
                col = mix(col, (0.45, 0.95, 0.92), 0.55 * math.exp(-depth / 0.014) * smooth(0.5 - y - 0.0, 0.0001) * 0 +
                          0.40 * math.exp(-depth / 0.016) * max(0.0, (y - 0.52) / 0.2))
                # aqua gloss on the upper half
                yb = 0.505 - 0.06 * (dx / R) ** 2
                gt = smooth(y - yb, 0.010)
                if gt > 0:
                    f = max(0.0, min(1.0, (y - (0.5 - R)) / max(1e-6, yb - (0.5 - R))))
                    col = mix(col, (1, 1, 1), gt * (0.62 - 0.45 * f))
                # specular dot on the centre disc
                sp = math.exp(-((x - 0.455) ** 2 + (y - 0.455) ** 2) / 0.0006)
                col = mix(col, (1, 1, 1), 0.55 * sp)
                c = over(c, (col[0], col[1], col[2], a_l))
            px[j][i] = c
    return px


# -------------------------------------------------------------------- OS 9 style
def mac_palette():
    pal = []
    for r in range(5, -1, -1):
        for g in range(5, -1, -1):
            for b in range(5, -1, -1):
                if (r, g, b) != (0, 0, 0):
                    pal.append((r * 51, g * 51, b * 51))
    ramp = [0xEE, 0xDD, 0xBB, 0xAA, 0x88, 0x77, 0x55, 0x44, 0x22, 0x11]
    pal += [(v, 0, 0) for v in ramp] + [(0, v, 0) for v in ramp] + [(0, 0, v) for v in ramp]
    pal += [(v, v, v) for v in ramp]
    pal.append((0, 0, 0))
    assert len(pal) == 256
    return pal


PAL = mac_palette()


def nearest(rgb):
    best, bi = 1e9, 0
    for i, p in enumerate(PAL):
        d = (p[0] - rgb[0]) ** 2 + (p[1] - rgb[1]) ** 2 + (p[2] - rgb[2]) ** 2
        if d < best:
            best, bi = d, i
    return bi


def mask_grid(n, R, cx, cy, dash_deg, rc, ring, disc):
    M = [[False] * n for _ in range(n)]
    for j in range(n):
        for i in range(n):
            hit = 0
            for sj in range(4):
                for si in range(4):
                    x = i + (si + 0.5) / 4 - cx
                    y = j + (sj + 0.5) / 4 - cy
                    if logo_sd(x, y, R, dash_deg, rc, ring, disc) <= 0:
                        hit += 1
            M[j][i] = hit >= 8
    return M


def os9_icon(n):
    """Returns (index grid with None for transparent, mask grid)."""
    if n == 32:
        M = mask_grid(32, 15.2, 16, 16, 32.0, 0.8, 0.31, 0.50)
    else:
        M = mask_grid(16, 7.7, 8, 8, 28.0, 0.3, 0.31, 0.46)

    def m(y, x):
        return 0 <= y < n and 0 <= x < n and M[y][x]

    BLACK = nearest((0, 0, 0))
    BASE = nearest((0, 128, 128))
    HI = nearest((100, 205, 205))
    SHADE = nearest((0, 77, 80))
    WHITE = nearest((255, 255, 255))
    G = [[None] * n for _ in range(n)]
    outline = [[False] * n for _ in range(n)]
    if n == 32:
        for y in range(n):
            for x in range(n):
                if M[y][x] and not (m(y - 1, x) and m(y + 1, x) and m(y, x - 1) and m(y, x + 1)):
                    outline[y][x] = True
    for y in range(n):
        for x in range(n):
            if not M[y][x]:
                continue
            if n == 32:
                if outline[y][x]:
                    G[y][x] = BLACK
                elif (y > 0 and outline[y - 1][x]) or (x > 0 and outline[y][x - 1]):
                    G[y][x] = HI
                elif outline[y + 1][x] or outline[y][x + 1]:
                    G[y][x] = SHADE
                else:
                    G[y][x] = BASE
            else:
                if not m(y + 1, x) or not m(y, x + 1):
                    G[y][x] = SHADE
                elif not m(y - 1, x) or not m(y, x - 1):
                    G[y][x] = HI
                else:
                    G[y][x] = BASE
    # specular spot on the centre disc
    if n == 32:
        for (yy, xx) in ((13, 12), (13, 13), (12, 13), (14, 12)):
            if G[yy][xx] is not None:
                G[yy][xx] = WHITE
    else:
        if G[7][6] is not None:
            G[7][6] = WHITE
    return G, M


def one_bit(G, M, n):
    """Black & white version for ICN#/ics#: dark pixels black, teal dithered."""
    bits = [[0] * n for _ in range(n)]
    for y in range(n):
        for x in range(n):
            if G[y][x] is None:
                continue
            p = PAL[G[y][x]]
            lum = (p[0] * 3 + p[1] * 6 + p[2]) / 10
            if lum < 40:
                bits[y][x] = 1
            elif lum < 150:
                bits[y][x] = 1 if (x + y) % 2 == 0 else 0
            else:
                bits[y][x] = 0
    return bits


def pack_bits(grid, n):
    out = bytearray()
    for y in range(n):
        for x in range(0, n, 8):
            v = 0
            for b in range(8):
                v = (v << 1) | (1 if grid[y][x + b] else 0)
            out.append(v)
    return bytes(out)


def rez_hex(name, rid, data, label):
    lines = []
    for i in range(0, len(data), 32):
        chunk = data[i:i + 32]
        s = " ".join(chunk[k:k + 2].hex().upper() for k in range(0, len(chunk), 2))
        lines.append('    $"%s"' % s)
    return "data '%s' (%d, \"%s\") {\n%s\n};\n" % (name, rid, label, "\n".join(lines))


def write_os9():
    os.makedirs(OS9, exist_ok=True)
    G32, M32 = os9_icon(32)
    G16, M16 = os9_icon(16)
    icl8 = bytes((G32[y][x] if G32[y][x] is not None else 0) for y in range(32) for x in range(32))
    ics8 = bytes((G16[y][x] if G16[y][x] is not None else 0) for y in range(16) for x in range(16))
    icn = pack_bits(one_bit(G32, M32, 32), 32) + pack_bits(M32, 32)
    ics = pack_bits(one_bit(G16, M16, 16), 16) + pack_bits(M16, 16)
    # indices of 0 (white) under a transparent pixel is fine: the mask ('ICN#'/'ics#') hides it
    rez = ("/* LocalSend mark, Mac OS 9 (Platinum) icon family. Generated by tools/make_logo.py.\n"
           "   Compile into the app's resource fork with Rez, e.g.  Rez -o LocalSend localsend_icons.r */\n\n")
    rez += rez_hex("ICN#", 128, icn, "LocalSend")
    rez += rez_hex("icl8", 128, icl8, "LocalSend")
    rez += rez_hex("ics#", 128, ics, "LocalSend")
    rez += rez_hex("ics8", 128, ics8, "LocalSend")
    with open(os.path.join(OS9, "localsend_icons.r"), "w") as fh:
        fh.write(rez)

    # preview sheet: both sizes, magnified, on Platinum grey, as they will look
    scale32, scale16 = 8, 16
    sheet = 2 * 256 + 3 * 16
    H = 256 + 32
    px = [[(0.867, 0.867, 0.867, 1.0)] * sheet for _ in range(H)]

    def blit(G, n, scale, ox, oy):
        for y in range(n):
            for x in range(n):
                if G[y][x] is None:
                    continue
                r, g, b = PAL[G[y][x]]
                col = (r / 255, g / 255, b / 255, 1.0)
                for sy in range(scale):
                    for sx in range(scale):
                        px[oy + y * scale + sy][ox + x * scale + sx] = col
    blit(G32, 32, scale32, 16, 16)
    blit(G16, 16, scale16, 16 + 256 + 16, 16)
    with open(os.path.join(OS9, "os9-preview.png"), "wb") as fh:
        fh.write(png(px))
    return len(rez)


def main():
    os.makedirs(OUT, exist_ok=True)
    flat = render_aqua(256, flat=True)
    with open(os.path.join(OUT, "logo-flat.png"), "wb") as fh:
        fh.write(png(flat))

    big = render_aqua(512)
    s = {n: downscale(big, n) for n in (256, 128, 48, 32, 16)}
    body = (classic(s[16], b"is32", b"s8mk") + classic(s[32], b"il32", b"l8mk")
            + classic(s[48], b"ih32", b"h8mk") + classic(s[128], b"it32", b"t8mk", pad=b"\x00\x00\x00\x00")
            + elem(b"ic07", png(s[128])) + elem(b"ic08", png(s[256])) + elem(b"ic09", png(big)))
    with open(os.path.join(OUT, "AppIcon.icns"), "wb") as fh:
        fh.write(b"icns" + struct.pack(">I", len(body) + 8) + body)
    with open(os.path.join(OUT, "aqua-512.png"), "wb") as fh:
        fh.write(png(big))
    n = write_os9()
    print("ok; rez bytes:", n)


main()
