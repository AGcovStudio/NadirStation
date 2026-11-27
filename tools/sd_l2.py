#!/usr/bin/env python3
"""
GOES SD (GOES-N/O/P sensor data, 1676 MHz) -> Level-2 products.

Input : the folder written by tools/stitch_segments.py
        (GOES_SD_IR1.png, GOES_SD_IR2.png, GOES_SD_IR3.png, GOES_SD_VIS.png,
         stitch_info.json).
Output: calibrated brightness temperatures, VIS reflectance, cloud mask,
        cloud-top temperature / height, deep-convection flags, a day
        microphysics-style RGB, PNG renderings and one netCDF file.

The GOES SD stream carries raw, uncalibrated detector counts.  NOAA never made
L2 products from SD; this tool builds them with a calibration transferred from
GVAR (see CALIBRATION below), so the results are *approximate*.  Typical
uncertainty (one sigma) against GVAR on the same satellite: about 0.2-0.3 K
for uniform ocean scenes, 1-3 K at the warm and cold extremes, VIS reflectance
about +/-10 % (vicarious, deep-convective-cloud anchored).

Dependencies: numpy and Pillow only.

Usage:
  python sd_l2.py <stitched_dir> [--out DIR] [--borders countries.geojson]
                  [--lon0 63.5] [--nav-dx 15] [--nav-dy 6] [--no-netcdf]
"""
import argparse, datetime as dt, json, math, os, sys
import numpy as np
from PIL import Image, ImageDraw, ImageFont

Image.MAX_IMAGE_PIXELS = None

# --------------------------------------------------------------------------
# CALIBRATION
# SD IR groups were identified by correlating them with a GVAR sector
# received from the same satellite 2026-11-22 06:15 UTC (SD full disc
# 05:44-06:13 UTC):  IR1 -> ch2 3.9 um (r=0.74), IR2 -> ch4 10.7 um (r=0.81),
# IR3 -> ch3 6.5 um (r=0.57); IR group 4 carries no image (constant).
# SD raw counts are mapped to GVAR counts by a line forced through the
# measured space count (zero radiance <-> GVAR offset b), with the gain fitted
# on ~3000 co-located 6x6-pixel blocks (robust LSQ, 2.5 MAD clipping):
#     C_gvar = gain * (C_sd - C_space) + b
# then the NOAA OSPO GOES-15 tables: R = (C_gvar - b) / m,
# Teff = c2 n / ln(1 + c1 n^3 / R),  T = a + beta * Teff.
# --------------------------------------------------------------------------
IR_CAL = {
    # group: (name, wavelength, gain SD->GVAR, GVAR m, GVAR b, n, a, beta)
    'IR1': ('ch2', 3.9,  1.0181, 227.3889, 68.2167, 2562.7905, -1.5693377, 1.0025034),
    'IR2': ('ch4', 10.7, 1.0302, 5.2285,   15.6854, 935.83788, -0.35733864, 1.00126425),
    'IR3': ('ch3', 6.5,  1.9169, 38.8383,  29.1287, 1521.36325, -3.47310565, 1.0093067),
}
C1, C2 = 1.191066e-5, 1.438833          # mW/(m2 sr cm-4), K cm
# GOES-15 VIS (OSPO table 7): R = m (X - 29), albedo = k R, mean of 8 detectors
VIS_M, VIS_X0, VIS_K = 0.58550, 29.0, 1.88852e-3
DCC_TARGET = 0.92        # assumed deep-convective-cloud reflectance at 0.65 um
SUN_39 = 15.3            # TOA solar irradiance in ch2 band, mW/(m2 cm-1), ~5800 K Planck

RE, RP, H0 = 6378.137, 6356.7523, 42164.16


def log(*a):
    print(*a, flush=True)


# ------------------------------------------------------------------ helpers
def raw_counts(png):
    s = np.array(Image.open(png)).astype(np.float32)
    raw = np.where(s > 0, np.round((65535.0 - s) / 64.0), np.nan)
    return raw


def nan_median_filter(a, ky=3, kx=5, chunk=256):
    """numpy-only NaN-aware median filter (odd kernel)."""
    H, W = a.shape
    py, px = ky // 2, kx // 2
    p = np.pad(a, ((py, py), (px, px)), mode='edge')
    out = np.empty_like(a)
    for y0 in range(0, H, chunk):
        y1 = min(H, y0 + chunk)
        win = np.lib.stride_tricks.sliding_window_view(p[y0:y1 + 2 * py], (ky, kx))
        with np.errstate(all='ignore'):
            import warnings
            with warnings.catch_warnings():
                warnings.simplefilter('ignore')
                out[y0:y1] = np.nanmedian(win.reshape(y1 - y0, W, -1), axis=2)
    out[np.isnan(a)] = np.nan
    return out


def box_mean(a, k):
    """k x k mean, NaN aware, same shape."""
    v = np.nan_to_num(a)
    m = np.isfinite(a).astype(np.float32)
    def cs(x):
        c = np.cumsum(np.cumsum(np.pad(x, ((k // 2 + 1, k // 2), (k // 2 + 1, k // 2))), 0), 1)
        return c[k:, k:] - c[:-k, k:] - c[k:, :-k] + c[:-k, :-k]
    with np.errstate(all='ignore'):
        return cs(v) / cs(m)


def planck_bt(raw, group, space):
    name, wl, gain, m, b, n, a, beta = IR_CAL[group]
    R = gain * (raw - space) / m                     # mW/(m2 sr cm-1)
    R = np.where(R > 1e-4, R, np.nan)
    teff = C2 * n / np.log1p(C1 * n ** 3 / R)
    return (a + beta * teff).astype(np.float32), R.astype(np.float32)


def planck_rad(T, group):
    name, wl, gain, m, b, n, a, beta = IR_CAL[group]
    teff = (T - a) / beta
    return C1 * n ** 3 / np.expm1(C2 * n / teff)



def load_vis(src):
    """16-bit visible counts x 64: GOES_SD_VIS.png, or GOES_SD_VIS_FULL.png, or the
    GOES_SD_VIS_partNNofMM.png bands stacked top to bottom. None if absent."""
    import glob, re
    for name in ('GOES_SD_VIS.png', 'GOES_SD_VIS_FULL.png'):
        p = os.path.join(src, name)
        if os.path.exists(p):
            im = Image.open(p)
            if im.mode not in ('I;16', 'I;16B', 'I'):
                log('      %s is not 16-bit, skipped' % name)
                continue
            return np.array(im).astype(np.float32) / 64.0
    parts = sorted(glob.glob(os.path.join(src, 'GOES_SD_VIS_part*of*.png')),
                   key=lambda p: int(re.search(r'part(\d+)of', p).group(1)))
    parts = [p for p in parts if 'Enhanced' not in os.path.basename(p)]
    if parts:
        return np.vstack([np.array(Image.open(p)).astype(np.float32) for p in parts]) / 64.0
    log('      no 16-bit visible image found: VIS products skipped')
    return None

# ---------------------------------------------------------------- navigation
def limb_fit(raw2):
    """Axis-aligned ellipse fit to the IR 10.7 um earth limb."""
    H, W = raw2.shape
    r = np.nan_to_num(raw2, nan=0)
    pts = []
    for y in range(0, H, 2):
        on = r[y] > 150
        if on.sum() < 50:
            continue
        c = np.convolve(on.astype(int), np.ones(15, int), 'valid')
        ok = np.where(c == 15)[0]
        if len(ok) == 0:
            continue
        L, R = ok[0], ok[-1] + 14
        if L < 5 or R > W - 6:
            continue
        if not np.all(np.isfinite(raw2[y, max(L - 10, 0):L])):
            continue
        pts += [(y, L), (y, R)]
    P = np.array(pts, float)
    y, x = P[:, 0], P[:, 1]
    keep = np.ones(len(x), bool)
    for _ in range(4):
        M = np.c_[x * x, y * y, x, y][keep]
        A, B, Cc, D = np.linalg.lstsq(M, np.ones(keep.sum()), rcond=None)[0]
        cx, cy = -Cc / (2 * A), -D / (2 * B)
        F = 1 + A * cx * cx + B * cy * cy
        a, b = math.sqrt(F / A), math.sqrt(F / B)
        res = (np.sqrt(((x - cx) / a) ** 2 + ((y - cy) / b) ** 2) - 1) * a
        keep = np.abs(res) < 3 * np.median(np.abs(res[keep])) + 1
    return cx, cy, a, b, float(np.std(res[keep]))


def geos_inv(xr, yr, lon0):
    cx, sx, cy, sy = np.cos(xr), np.sin(xr), np.cos(yr), np.sin(yr)
    a = sx ** 2 + cx ** 2 * (cy ** 2 + (RE / RP) ** 2 * sy ** 2)
    b = -2 * H0 * cx * cy
    c = H0 ** 2 - RE ** 2
    disc = b * b - 4 * a * c
    with np.errstate(invalid='ignore'):
        rs = (-b - np.sqrt(disc)) / (2 * a)
    s1, s2, s3 = rs * cx * cy, -rs * sx, rs * cx * sy
    lat = np.degrees(np.arctan((RE / RP) ** 2 * s3 / np.sqrt((H0 - s1) ** 2 + s2 ** 2)))
    lon = lon0 - np.degrees(np.arctan(s2 / (H0 - s1)))
    bad = ~(disc >= 0)
    lat[bad] = np.nan
    lon[bad] = np.nan
    # satellite zenith angle
    with np.errstate(invalid='ignore'):
        vza = np.degrees(np.arcsin(np.clip(H0 / RE * np.sin(np.hypot(xr, yr)), -1, 1)))
    return lat.astype(np.float32), lon.astype(np.float32), vza.astype(np.float32)


def geos_fwd(lat, lon, lon0):
    phi, lam = np.radians(lat), np.radians(lon - lon0)
    pc = np.arctan((RP / RE) ** 2 * np.tan(phi))
    rc = RP / np.sqrt(1 - (1 - (RP / RE) ** 2) * np.cos(pc) ** 2)
    s1 = H0 - rc * np.cos(pc) * np.cos(lam)
    s2 = -rc * np.cos(pc) * np.sin(lam)
    s3 = rc * np.sin(pc)
    vis = (H0 * (H0 - s1)) > (s2 ** 2 + (RE / RP) ** 2 * s3 ** 2)
    y = np.arctan(s3 / s1)
    x = np.arcsin(-s2 / np.sqrt(s1 ** 2 + s2 ** 2 + s3 ** 2))
    return x, y, vis


def solar_zenith(lat, lon, t):
    """t: datetime64 array (UTC). Returns degrees (NOAA approximate)."""
    doy = (t - t.astype('datetime64[Y]')).astype('timedelta64[s]').astype(np.float64) / 86400.0
    g = 2 * np.pi / 365.0 * doy
    decl = (0.006918 - 0.399912 * np.cos(g) + 0.070257 * np.sin(g) - 0.006758 * np.cos(2 * g)
            + 0.000907 * np.sin(2 * g) - 0.002697 * np.cos(3 * g) + 0.00148 * np.sin(3 * g))
    eqt = 229.18 * (0.000075 + 0.001868 * np.cos(g) - 0.032077 * np.sin(g)
                    - 0.014615 * np.cos(2 * g) - 0.040849 * np.sin(2 * g))
    utc_min = (doy % 1.0) * 1440.0
    tst = utc_min + eqt + 4 * lon
    ha = np.radians(tst / 4.0 - 180.0)
    la = np.radians(lat)
    cz = np.sin(la) * np.sin(decl) + np.cos(la) * np.cos(decl) * np.cos(ha)
    return np.degrees(np.arccos(np.clip(cz, -1, 1))).astype(np.float32)


# ------------------------------------------------- climatological profiles
# AFGL standard atmospheres, T(K) at 0..20 km
Z = np.arange(0, 21, dtype=np.float32)
T_TROP = np.array([299.7, 293.7, 287.7, 283.7, 277.0, 270.3, 263.6, 257.0, 250.3, 243.6, 237.0,
                   230.1, 223.6, 217.0, 210.3, 203.7, 197.0, 194.8, 198.8, 202.7, 206.7], np.float32)
T_MLS = np.array([294.2, 289.7, 285.2, 279.2, 273.2, 267.2, 261.2, 254.7, 248.2, 241.7, 235.3,
                  228.8, 222.3, 215.8, 215.7, 215.7, 215.7, 215.7, 216.8, 217.9, 219.2], np.float32)
T_MLW = np.array([272.2, 268.7, 265.2, 261.7, 255.7, 249.7, 243.7, 237.7, 231.7, 225.7, 219.7,
                  219.2, 218.7, 218.2, 217.7, 217.2, 216.7, 216.2, 215.7, 215.2, 215.2], np.float32)


def cloud_top_height(ctt, lat, month, tsurf):
    """Height (km) where a latitude-blended climatological profile, shifted so
    its surface equals the local clear-sky surface temperature in the lowest
    3 km, reaches the cloud-top temperature.  Approximate."""
    nh_summer = month in (4, 5, 6, 7, 8, 9)
    w = np.clip((np.abs(lat) - 15.0) / 30.0, 0, 1)        # 0 tropics .. 1 midlat
    mid_n = T_MLS if nh_summer else T_MLW
    mid_s = T_MLW if nh_summer else T_MLS
    out = np.full(ctt.shape, np.nan, np.float32)
    ok = np.isfinite(ctt) & np.isfinite(lat)
    c, wl, lt, ts = ctt[ok], w[ok], lat[ok], tsurf[ok]
    prof = (1 - wl)[:, None] * T_TROP[None, :] + wl[:, None] * np.where(
        lt[:, None] >= 0, mid_n[None, :], mid_s[None, :])
    # surface adjustment fades out by 3 km
    dts = np.where(np.isfinite(ts), ts - prof[:, 0], 0.0)
    fade = np.clip(1 - Z / 3.0, 0, 1)
    prof = prof + dts[:, None] * fade[None, :]
    tmin_idx = np.argmin(prof[:, :19], axis=1)
    h = np.zeros(len(c), np.float32)
    for k in range(1, 19):
        t0, t1 = prof[:, k - 1], prof[:, k]
        inlayer = (c <= t0) & (c > t1) & (k <= tmin_idx) & (h == 0)
        frac = (t0 - c) / np.maximum(t0 - t1, 1e-3)
        h = np.where(inlayer, (k - 1) + frac, h)
    colder = c <= prof[np.arange(len(c)), tmin_idx]
    h = np.where(colder & (h == 0), tmin_idx.astype(np.float32), h)
    warmer = c > prof[:, 0]
    h = np.where(warmer, 0.3, h)
    h = np.where(h == 0, 0.3, h)
    out[ok] = h
    return out


# ---------------------------------------------------------------- palettes
def ramp(stops, v):
    """stops: list of (value, (r,g,b)); v array -> uint8 rgb"""
    xs = np.array([s[0] for s in stops], np.float32)
    cs = np.array([s[1] for s in stops], np.float32)
    order = np.argsort(xs)
    xs, cs = xs[order], cs[order]
    out = np.zeros(v.shape + (3,), np.float32)
    vv = np.clip(np.nan_to_num(v, nan=xs[0]), xs[0], xs[-1])
    for ch in range(3):
        out[..., ch] = np.interp(vv, xs, cs[:, ch])
    return out.astype(np.uint8)


PAL_IR = [(330, (20, 20, 20)), (290, (110, 110, 110)), (250, (215, 215, 215)), (240, (240, 240, 240)),
          (239.9, (0, 200, 255)), (230, (0, 90, 255)), (220, (0, 200, 70)), (210, (255, 235, 0)),
          (200, (255, 120, 0)), (190, (220, 0, 0)), (183, (255, 0, 255)), (175, (255, 255, 255))]
PAL_WV = [(290, (60, 30, 0)), (270, (170, 90, 20)), (255, (235, 235, 235)), (240, (120, 170, 255)),
          (228, (0, 70, 200)), (215, (0, 170, 60)), (205, (255, 230, 0)), (195, (230, 0, 0))]
PAL_CTH = [(0, (240, 240, 140)), (2, (255, 190, 60)), (4, (240, 110, 40)), (6, (200, 40, 90)),
           (9, (120, 40, 170)), (12, (40, 80, 210)), (15, (40, 190, 230)), (18, (230, 255, 255))]
MASK_COL = {0: (0, 0, 0), 1: (25, 80, 30), 2: (90, 150, 80), 3: (170, 170, 170), 4: (250, 250, 250),
            5: (25, 40, 90)}


def font(sz):
    for f in ['DejaVuSans.ttf', '/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf',
              'C:/Windows/Fonts/segoeui.ttf', 'C:/Windows/Fonts/arial.ttf', 'arial.ttf']:
        try:
            return ImageFont.truetype(f, sz)
        except Exception:
            pass
    return ImageFont.load_default()


class Renderer:
    def __init__(self, nav, borders, lon0, title_time, sat):
        self.nav, self.borders, self.lon0 = nav, borders, lon0
        self.title_time, self.sat = title_time, sat

    def overlay(self, img):
        d = ImageDraw.Draw(img)
        cx, cy, ax, ay = self.nav
        def proj(lat, lon):
            x, y, vis = geos_fwd(np.asarray(lat, float), np.asarray(lon, float), self.lon0)
            return x / ax + cx, -y / ay + cy, vis
        def draw_poly(lat, lon, col, w=1):
            px, py, vis = proj(lat, lon)
            seg = []
            for i in range(len(px)):
                if vis[i] and np.isfinite(px[i]):
                    seg.append((float(px[i]), float(py[i])))
                else:
                    if len(seg) > 1:
                        d.line(seg, fill=col, width=w)
                    seg = []
            if len(seg) > 1:
                d.line(seg, fill=col, width=w)
        for la in range(-80, 81, 10):
            lo = np.arange(self.lon0 - 90, self.lon0 + 90.1, 0.5)
            draw_poly(np.full_like(lo, la), lo, (120, 120, 120))
        for lo in range(int(self.lon0 // 10 * 10) - 80, int(self.lon0) + 90, 10):
            la = np.arange(-85, 85.1, 0.5)
            draw_poly(la, np.full_like(la, lo), (120, 120, 120))
        for ring in self.borders:
            draw_poly(ring[:, 1], ring[:, 0], (255, 230, 0))
        return img

    def finish(self, rgb, title, legend=None, path=None):
        img = Image.fromarray(rgb)
        img = self.overlay(img)
        W, H = img.size
        band = 150
        canvas = Image.new('RGB', (W, H + band), (0, 0, 0))
        canvas.paste(img, (0, 0))
        d = ImageDraw.Draw(canvas)
        d.text((20, H + 12), title, font=font(34), fill=(255, 255, 255))
        d.text((20, H + 58), '%s  |  %s  |  GOES SD raw counts, GVAR-transferred calibration (approximate)' %
               (self.sat, self.title_time), font=font(22), fill=(190, 190, 190))
        if legend is not None:
            legend(d, W, H + 95)
        canvas.save(path, optimize=False, compress_level=6)
        log('  wrote', os.path.basename(path))


def colorbar(stops, lo, hi, ticks, unit):
    def f(d, W, y):
        x0, x1 = W // 2 - 700, W // 2 + 700
        v = np.linspace(lo, hi, x1 - x0)
        rgb = ramp(stops, v)
        for i, c in enumerate(rgb):
            d.line([(x0 + i, y), (x0 + i, y + 22)], fill=tuple(int(t) for t in c))
        for t in ticks:
            xx = x0 + (t - lo) / (hi - lo) * (x1 - x0)
            d.line([(xx, y + 22), (xx, y + 30)], fill=(255, 255, 255))
            d.text((xx - 18, y + 30), '%g' % t, font=font(18), fill=(255, 255, 255))
        d.text((x1 + 15, y), unit, font=font(20), fill=(255, 255, 255))
    return f


def classes(items):
    def f(d, W, y):
        x = W // 2 - 800
        for col, name in items:
            d.rectangle([x, y, x + 28, y + 24], fill=col, outline=(200, 200, 200))
            d.text((x + 38, y), name, font=font(20), fill=(255, 255, 255))
            x += 60 + int(d.textlength(name, font=font(20)))
    return f


# ---------------------------------------------------------------- netCDF3
def write_netcdf(path, H, W, vars_, attrs):
    """Minimal classic netCDF (CDF-1) writer; vars_: name -> (array, dtype,
    scale, offset, units, long_name, fill)."""
    import struct
    def pstr(s):
        b = s.encode('utf-8')
        return struct.pack('>i', len(b)) + b + b'\0' * ((4 - len(b) % 4) % 4)
    def att(name, val):
        if isinstance(val, str):
            return pstr(name) + struct.pack('>ii', 2, len(val.encode())) + \
                (val.encode() + b'\0' * ((4 - len(val.encode()) % 4) % 4))
        if isinstance(val, int):
            return pstr(name) + struct.pack('>iii', 4, 1, val)
        return pstr(name) + struct.pack('>ii', 6, 1) + struct.pack('>d', float(val))
    def attlist(d):
        if not d:
            return struct.pack('>ii', 0, 0)
        return struct.pack('>ii', 12, len(d)) + b''.join(att(k, v) for k, v in d.items())
    hdr = b'CDF\x01' + struct.pack('>i', 0)
    hdr += struct.pack('>ii', 10, 2) + pstr('y') + struct.pack('>i', H) + pstr('x') + struct.pack('>i', W)
    hdr += attlist(attrs)
    names = list(vars_)
    tmap = {'i1': (1, 1), 'i2': (3, 2), 'f4': (5, 4)}
    sizes = []
    for n in names:
        a, dt_, *_ = vars_[n]
        sz = H * W * tmap[dt_][1]
        sizes.append(sz + ((4 - sz % 4) % 4))
    def varblock(offsets):
        b = struct.pack('>ii', 11, len(names))
        for n, off, sz in zip(names, offsets, sizes):
            a, dt_, scale, offset, units, lname, fill = vars_[n]
            at = {'long_name': lname, 'units': units}
            if scale is not None:
                at['scale_factor'] = scale
                at['add_offset'] = offset
            if fill is not None:
                at['_FillValue'] = int(fill)
            b += pstr(n) + struct.pack('>iii', 2, 0, 1) + attlist(at)
            b += struct.pack('>iii', tmap[dt_][0], sz, off)
        return b
    base = len(hdr) + len(varblock([0] * len(names)))
    offs, o = [], base
    for sz in sizes:
        offs.append(o)
        o += sz
    with open(path, 'wb') as f:
        f.write(hdr + varblock(offs))
        for n, sz in zip(names, sizes):
            a, dt_, *_ = vars_[n]
            raw = a.astype('>' + dt_).tobytes()
            f.write(raw + b'\0' * (sz - len(raw)))
    log('  wrote', os.path.basename(path))


def pack(a, scale, offset, fill=-32768):
    q = np.round((a - offset) / scale)
    q = np.where(np.isfinite(a), np.clip(q, -32767, 32767), fill)
    return q.astype(np.int16)


# ------------------------------------------------------------------- main
def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('stitched_dir')
    ap.add_argument('--out', default=None)
    ap.add_argument('--borders', default=None, help='countries GeoJSON for coast/border overlay')
    ap.add_argument('--lon0', type=float, default=63.5, help='sub-satellite longitude (deg E)')
    ap.add_argument('--nav-dx', type=float, default=15.0, help='navigation fine shift, IR px (+ = east)')
    ap.add_argument('--nav-dy', type=float, default=6.0, help='navigation fine shift, IR px (+ = south)')
    ap.add_argument('--start', default=None, help='scan start UTC, e.g. 2026-11-22T05:44:18')
    ap.add_argument('--end', default=None, help='scan end UTC')
    ap.add_argument('--no-netcdf', action='store_true')
    ap.add_argument('--satellite', default='EWS-G2 (ex GOES-15)')
    args = ap.parse_args()
    src = args.stitched_dir
    out = args.out or os.path.join(src, 'L2')
    os.makedirs(out, exist_ok=True)

    info = {}
    if os.path.exists(os.path.join(src, 'stitch_info.json')):
        info = json.load(open(os.path.join(src, 'stitch_info.json')))
    segs = info.get('source_segments') or info.get('segments', [])
    if args.start:
        t0 = np.datetime64(args.start)
    elif segs:
        t0 = np.datetime64(dt.datetime.strptime(segs[0], '%Y-%m-%d_%H-%M-%S'))
    else:
        sys.exit('need --start (no stitch_info.json)')
    if args.end:
        t1 = np.datetime64(args.end)
    elif len(segs) > 1:
        ts = [dt.datetime.strptime(s, '%Y-%m-%d_%H-%M-%S') for s in segs]
        mean_dur = (ts[-1] - ts[0]) / (len(ts) - 1)
        t1 = np.datetime64(ts[-1] + mean_dur)
    else:
        t1 = t0 + np.timedelta64(26, 'm')

    log('[1/7] loading raw counts')
    raw, unfilt = {}, {}
    for g in ('IR1', 'IR2', 'IR3'):
        r = raw_counts(os.path.join(src, 'GOES_SD_%s.png' % g))
        unfilt[g] = r
        raw[g] = nan_median_filter(r, 3, 5)
    H, W = raw['IR2'].shape
    log('      IR grid %dx%d' % (W, H))

    log('[2/7] navigation (limb fit)')
    cx, cy, a, b, rms = limb_fit(raw['IR2'])
    cx += args.nav_dx
    cy += args.nav_dy
    ax, ay = math.asin(RE / H0) / a, math.asin(RP / H0) / b
    log('      centre (%.1f, %.1f) px, semi-axes %.1f x %.1f px, limb rms %.2f px, %.1f/%.1f urad per px' %
        (cx, cy, a, b, rms, ax * 1e6, ay * 1e6))
    yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)
    lat, lon, vza = geos_inv((xx - cx) * ax, -(yy - cy) * ay, args.lon0)
    earth = np.isfinite(lat) & (vza < 80)
    row_t = t0 + ((yy / (H - 1)) * (t1 - t0).astype('timedelta64[s]').astype(np.float64)).astype('timedelta64[s]')
    sza = solar_zenith(lat, lon, row_t)
    del yy, xx
    day = earth & (sza < 80)
    night = earth & (sza >= 88)

    log('[3/7] calibration')
    space = {}
    # space count: west-of-disc strip (same definition as used for the GVAR fit)
    yv = np.arange(H)[:, None]
    xl = cx - a * np.sqrt(np.clip(1 - ((yv - cy) / b) ** 2, 0, 1)) - 40
    sp_mask = (np.abs(yv - cy) < 0.75 * b) & (np.arange(W)[None, :] > 50) & (np.arange(W)[None, :] < xl)
    bt, rad = {}, {}
    for g in ('IR1', 'IR2', 'IR3'):
        v = unfilt[g][sp_mask & np.isfinite(unfilt[g])]
        v = v[(v > 30) & (v < 90)]
        space[g] = float(np.median(v)) if len(v) > 1000 else {'IR1': 57.0, 'IR2': 58.0, 'IR3': 60.0}[g]
        bt[g], rad[g] = planck_bt(raw[g], g, space[g])
        bt[g][~earth] = np.nan
        log('      %s (%s %.1f um): space count %.1f, BT p1/p50/p99 = %s K' %
            (g, IR_CAL[g][0], IR_CAL[g][1], space[g],
             np.round(np.nanpercentile(bt[g][earth], [1, 50, 99]), 1)))
    T11, T39, T65 = bt['IR2'], bt['IR1'], bt['IR3']

    refl = np.full((H, W), np.nan, np.float32)
    vis_factor = None
    v = load_vis(src)
    if v is not None:
        hh, ww = min(H * 4, v.shape[0]) // 4, min(W * 4, v.shape[1]) // 4
        v = v[:hh * 4, :ww * 4]
        vz = np.where(v > 0, v, np.nan).reshape(hh, 4, ww, 4)
        import warnings
        with warnings.catch_warnings():
            warnings.simplefilter('ignore')
            vb = np.nanmean(np.nanmean(vz, axis=3), axis=1)
        del v, vz
        albedo = VIS_K * VIS_M * (vb - VIS_X0)          # pre-launch, normalised to overhead sun
        mu0 = np.cos(np.radians(sza[:hh, :ww]))
        pre = albedo / np.maximum(mu0, 0.1)
        dcc = (T11[:hh, :ww] < 205) & (sza[:hh, :ww] < 40) & (vza[:hh, :ww] < 40) & np.isfinite(pre)
        if dcc.sum() > 200:
            hist, edges = np.histogram(pre[dcc], bins=200, range=(0.2, 1.6))
            k = np.convolve(hist, np.ones(5) / 5, 'same')
            mode = edges[np.argmax(k)] + (edges[1] - edges[0]) / 2
            vis_factor = DCC_TARGET / mode
            log('      VIS: %d deep-convective-cloud pixels, mode %.3f -> post-launch factor %.3f' %
                (dcc.sum(), mode, vis_factor))
        else:
            vis_factor = 1.64044 * 1.10
            log('      VIS: too few DCC pixels, using assumed post-launch factor %.3f' % vis_factor)
        refl[:hh, :ww] = np.where(sza[:hh, :ww] < 85, pre * vis_factor, np.nan)
        refl[~earth] = np.nan

    log('[4/7] land mask / borders')
    borders, land = [], None
    if args.borders and os.path.exists(args.borders):
        g = json.load(open(args.borders, encoding='utf-8'))
        polys = []
        for f in g['features']:
            geo = f.get('geometry')
            if not geo:
                continue
            cs = geo['coordinates'] if geo['type'] == 'MultiPolygon' else [geo['coordinates']]
            for p in cs:
                borders += [np.array(r) for r in p]
                polys.append(np.array(p[0]))
        res = 0.05
        im = Image.new('L', (int(360 / res), int(180 / res)), 0)
        dr = ImageDraw.Draw(im)
        for r in polys:
            if len(r) > 2:
                dr.polygon([((lo + 180) / res, (90 - la) / res) for lo, la in r], fill=1)
        lm = np.array(im)
        ii = np.clip(((90 - np.nan_to_num(lat)) / res).astype(int), 0, lm.shape[0] - 1)
        jj = np.clip((((np.nan_to_num(lon) + 180) % 360) / res).astype(int), 0, lm.shape[1] - 1)
        land = (lm[ii, jj] > 0) & earth
        del ii, jj
    if land is None:
        land = np.zeros((H, W), bool)
        borders = []
        log('      no --borders file: everything treated as ocean in the cloud tests')
    # navigation is good to ~10-20 px, so pixels near a coastline get the
    # (stricter) land tests and are never used as clear-ocean reference
    near_land = box_mean(land.astype(np.float32), 41) > 0.02
    near_sea = box_mean((earth & ~land).astype(np.float32), 41) > 0.02
    coast = earth & near_land & near_sea
    land_true = land.copy()
    ocean = earth & ~land & ~coast
    land = land | coast

    log('[5/7] cloud mask, 3.9 um reflectance, cloud top')
    # 3.9 um reflectance (day): r = (L39 - B39(T11)) / (E cos(sza)/pi - B39(T11))
    b39 = planck_rad(T11, 'IR1')
    mu0 = np.cos(np.radians(sza))
    with np.errstate(all='ignore'):
        r39 = (rad['IR1'] - b39) / (SUN_39 * mu0 / np.pi - b39)
    r39 = np.where(day, r39, np.nan).astype(np.float32)
    # sun-glint geometry: approximate glint angle with sun-satellite separation
    # (glint zone handled by raising VIS / 3.9 thresholds near the specular point)
    with np.errstate(all='ignore'):
        sublat = 0.0
        # specular point roughly midway between sub-solar and sub-satellite point
    # clear-sky reference.
    #  ocean: warm percentile per 1-degree latitude band (SST changes little
    #         across the disc), minus a limb-cooling term;
    #  land : warm percentile in 128x128 px blocks (handles day/night and the
    #         terminator), falling back to the latitude-band value.
    limb = np.clip(3.0 * (1 / np.cos(np.radians(np.minimum(vza, 80))) - 1), 0, 10)
    tref = np.full((H, W), np.nan, np.float32)
    band = np.floor(np.nan_to_num(lat, nan=-999)).astype(int)
    def band_ref(msk, pct):
        bands, vals = band[msk], (T11 + limb)[msk]
        ok = np.isfinite(vals)
        bands, vals = bands[ok], vals[ok]
        ref = {}
        for bnd in np.unique(bands):
            sv = vals[bands == bnd]
            if len(sv) > 200:
                ref[bnd] = np.percentile(sv, pct)
        keys = np.array(sorted(ref))
        out_ = np.full((H, W), np.nan, np.float32)
        if len(keys):
            out_[earth] = np.interp(band[earth], keys, np.array([ref[k] for k in keys])) - limb[earth]
        return out_
    ref_o = band_ref(ocean, 92)
    ref_l = band_ref(land & ~coast, 95)
    BS = 128
    nby, nbx = (H + BS - 1) // BS, (W + BS - 1) // BS
    blk = np.full((nby, nbx), np.nan, np.float32)
    for i in range(nby):
        for j in range(nbx):
            sl = (slice(i * BS, (i + 1) * BS), slice(j * BS, (j + 1) * BS))
            v_ = (T11[sl] + limb[sl])[land[sl] & np.isfinite(T11[sl])]
            if len(v_) > 600:
                blk[i, j] = np.percentile(v_, 95)
    # fill empty blocks from neighbours, then bilinear upsample
    for _ in range(6):
        nb = np.nan_to_num(np.pad(blk, 1, constant_values=np.nan))
        cnt = np.pad(np.isfinite(blk), 1).astype(np.float32)
        ssum = sum(np.roll(np.roll(nb, dy, 0), dx, 1) for dy in (-1, 0, 1) for dx in (-1, 0, 1))[1:-1, 1:-1]
        csum = sum(np.roll(np.roll(cnt, dy, 0), dx, 1) for dy in (-1, 0, 1) for dx in (-1, 0, 1))[1:-1, 1:-1]
        with np.errstate(all='ignore'):
            blk = np.where(np.isfinite(blk), blk, np.where(csum > 0, ssum / csum, np.nan))
    gy = np.clip((np.arange(H) + 0.5) / BS - 0.5, 0, nby - 1)
    gx = np.clip((np.arange(W) + 0.5) / BS - 0.5, 0, nbx - 1)
    y0i = np.floor(gy).astype(int); x0i = np.floor(gx).astype(int)
    y1i = np.minimum(y0i + 1, nby - 1); x1i = np.minimum(x0i + 1, nbx - 1)
    fy = (gy - y0i)[:, None]; fx = (gx - x0i)[None, :]
    up = (blk[y0i][:, x0i] * (1 - fy) * (1 - fx) + blk[y1i][:, x0i] * fy * (1 - fx) +
          blk[y0i][:, x1i] * (1 - fy) * fx + blk[y1i][:, x1i] * fy * fx) - limb
    ref_land = np.where(np.isfinite(up), up, ref_l)
    tref = np.where(land, ref_land, ref_o).astype(np.float32)
    tref[~earth] = np.nan
    tsurf = box_mean(np.where(np.isfinite(T11) & (T11 > tref - 3), T11, np.nan), 61)
    tsurf = np.where(np.isfinite(tsurf), tsurf, tref)

    score = np.zeros((H, W), np.float32)
    dT = tref - T11
    score += np.where(ocean & (dT > 7), 2, np.where(ocean & (dT > 3.5), 1, 0))
    score += np.where(land & (dT > 14), 2, np.where(land & (dT > 8), 1, 0))
    score += np.where(earth & (T11 < 250), 2, 0)
    vis_ok = np.isfinite(refl)
    score += np.where(vis_ok & ocean & (refl > 0.22), 2, np.where(vis_ok & ocean & (refl > 0.12), 1, 0))
    score += np.where(vis_ok & land & (refl > 0.50), 2, np.where(vis_ok & land & (refl > 0.40), 1, 0))
    score += np.where(day & ocean & (r39 > 0.10) & (refl > 0.08), 1, 0)
    score += np.where(night & ((T11 - T39) > 3.0), 1, 0)                      # low cloud / fog at night
    score += np.where(earth & ((T65 - T11) > -12) & (T11 < 270), 1, 0)       # thick high cloud
    cmask = np.where(score >= 3, 4, np.where(score == 2, 3, np.where(score == 1, 2, 1))).astype(np.uint8)
    cmask[~earth] = 0
    cmask[~np.isfinite(T11) & earth] = 0
    cloudy = cmask >= 3

    ctt = np.where(cloudy, T11, np.nan).astype(np.float32)
    cth = cloud_top_height(ctt, lat, int(str(t0)[5:7]), tsurf)
    conv = np.zeros((H, W), np.uint8)
    conv[earth & (T11 < 235)] = 1
    conv[earth & (T11 < 220)] = 2
    # overshooting-top candidate: very cold, >=5 K colder than the mean anvil
    # (<225 K pixels) within ~120 km,
    # and 6.5 um warmer than 10.7 um (moisture above the tropopause)
    t11_env = box_mean(np.where(T11 < 225, T11, np.nan), 31)     # mean anvil temperature
    conv[earth & (T11 < 212) & ((t11_env - T11) > 5.0) & ((T65 - T11) > 0.0)] = 3
    stats = {
        'cloud_fraction_earth': float(np.mean(cloudy[earth])),
        'deep_convection_px(T11<220K)': int((conv >= 2).sum()),
        'overshoot_candidates_px': int((conv == 3).sum()),
        'T11_p0.01_K': float(np.nanpercentile(T11[earth], 0.01)),
    }
    log('      cloud fraction %.1f %%, deep convection %d px, overshoot candidates %d px' %
        (100 * stats['cloud_fraction_earth'], stats['deep_convection_px(T11<220K)'], stats['overshoot_candidates_px']))

    log('[6/7] rendering')
    tt = '%s - %s UTC' % (str(t0)[:16].replace('T', ' '), str(t1)[11:16])
    R = Renderer((cx, cy, ax, ay), borders, args.lon0, tt, args.satellite)
    bg = np.zeros((H, W, 3), np.uint8)

    def put(mask, rgb):
        img = bg.copy()
        img[mask] = rgb[mask]
        return img

    R.finish(put(earth, ramp(PAL_IR, T11)), 'Brightness temperature 10.7 um / cloud-top temperature',
             colorbar(PAL_IR, 175, 330, [180, 190, 200, 210, 220, 230, 240, 260, 280, 300, 320], 'K'),
             os.path.join(out, 'L2_BT10p7_CTT.png'))
    R.finish(put(earth, ramp(PAL_WV, T65)), 'Water vapour brightness temperature 6.5 um',
             colorbar(PAL_WV, 195, 290, [200, 210, 220, 230, 240, 250, 260, 270, 280, 290], 'K'),
             os.path.join(out, 'L2_BT6p5_WV.png'))
    g39 = np.clip((T39 - 220) / (330 - 220) * 255, 0, 255)
    R.finish(put(earth, np.repeat(np.nan_to_num(255 - g39).astype(np.uint8)[..., None], 3, 2)),
             'Brightness temperature 3.9 um (includes reflected sunlight by day)',
             colorbar([(220, (255, 255, 255)), (330, (0, 0, 0))], 220, 330, [220, 240, 260, 280, 300, 320], 'K'),
             os.path.join(out, 'L2_BT3p9.png'))
    mrgb = np.zeros((H, W, 3), np.uint8)
    for k, c in MASK_COL.items():
        if k <= 4:
            mrgb[cmask == k] = c
    mrgb[(cmask == 1) & ~land_true] = MASK_COL[5]
    R.finish(mrgb, 'Cloud mask (threshold tests: 10.7 um, VIS, 3.9 um, 6.5-10.7 um)',
             classes([(MASK_COL[5], 'clear ocean'), (MASK_COL[1], 'clear land'), (MASK_COL[2], 'probably clear'),
                      (MASK_COL[3], 'probably cloudy'), (MASK_COL[4], 'cloudy'), ((0, 0, 0), 'space / no data')]),
             os.path.join(out, 'L2_CloudMask.png'))
    crgb = ramp(PAL_CTH, cth)
    crgb[~cloudy] = (35, 35, 35)
    crgb[~earth] = 0
    R.finish(crgb, 'Cloud-top height (from CTT + climatological profile, approximate)',
             colorbar(PAL_CTH, 0, 18, [0, 2, 4, 6, 8, 10, 12, 14, 16, 18], 'km'),
             os.path.join(out, 'L2_CloudTopHeight.png'))
    gry = np.clip((T11 - 200) / (310 - 200), 0, 1)
    base = np.repeat((np.nan_to_num(1 - gry) * 200).astype(np.uint8)[..., None], 3, 2)
    base[conv == 1] = (255, 200, 60)
    base[conv == 2] = (255, 80, 0)
    base[conv == 3] = (255, 0, 255)
    base[~earth] = 0
    R.finish(base, 'Deep convection (10.7 um) and overshooting-top candidates (6.5 um > 10.7 um)',
             classes([((255, 200, 60), 'T11 < 235 K'), ((255, 80, 0), 'T11 < 220 K'),
                      ((255, 0, 255), 'overshoot candidate (T11<212 K, 5 K below anvil, T6.5>T11)')]),
             os.path.join(out, 'L2_DeepConvection.png'))
    if vis_factor is not None:
        rr = np.clip(np.nan_to_num(refl) / 1.0, 0, 1)
        gg = np.clip(np.nan_to_num(r39) / 0.6, 0, 1) ** (1 / 2.5)
        bb = np.clip((T11 - 203) / (323 - 203), 0, 1)
        rgb = (np.stack([rr, gg, np.nan_to_num(bb)], -1) * 255).astype(np.uint8)
        nightc = earth & ~day
        ir = np.repeat((np.nan_to_num(1 - gry) * 140).astype(np.uint8)[..., None], 3, 2)
        rgb[nightc] = ir[nightc]
        rgb[~earth] = 0
        R.finish(rgb, 'Day microphysics-style RGB  (R: VIS 0.65 um refl, G: 3.9 um refl, B: 10.7 um BT)',
                 classes([((255, 60, 40), 'thick ice cloud'), ((235, 160, 210), 'water cloud'),
                          ((230, 240, 170), 'small-droplet / supercooled cloud'), ((20, 30, 210), 'ocean'),
                          ((90, 170, 230), 'land'), ((90, 90, 90), 'night (10.7 um only)')]),
                 os.path.join(out, 'L2_DayMicrophysics_RGB.png'))
        vg = (np.clip(np.nan_to_num(refl) / 1.0, 0, 1) ** (1 / 1.6) * 255).astype(np.uint8)
        R.finish(put(earth, np.repeat(vg[..., None], 3, 2)), 'VIS 0.65 um reflectance (DCC-anchored calibration)',
                 colorbar([(0, (0, 0, 0)), (1, (255, 255, 255))], 0, 1, [0, 0.2, 0.4, 0.6, 0.8, 1.0], ''),
                 os.path.join(out, 'L2_VIS_Reflectance.png'))

    log('[7/7] data files')
    meta = {
        'product': 'GOES SD Level-2 (unofficial, approximate)',
        'satellite': args.satellite,
        'scan_start_utc': str(t0), 'scan_end_utc': str(t1),
        'segments': segs,
        'navigation': {'lon0_deg': args.lon0, 'centre_px': [cx, cy], 'semi_axes_px': [a, b],
                       'urad_per_px': [ax * 1e6, ay * 1e6], 'limb_rms_px': rms,
                       'accuracy': 'about 10 IR px (40 km); stitch residuals not modelled'},
        'calibration_ir': {g: {'gvar_channel': IR_CAL[g][0], 'wavelength_um': IR_CAL[g][1],
                               'space_count': space[g], 'gain_sd_to_gvar': IR_CAL[g][2],
                               'gvar_m': IR_CAL[g][3], 'gvar_b': IR_CAL[g][4],
                               'n': IR_CAL[g][5], 'a': IR_CAL[g][6], 'beta': IR_CAL[g][7]} for g in IR_CAL},
        'calibration_ir_reference': 'GVAR sector 2026-11-22 06:15 UTC, same satellite; '
                                    'BT residual MAD 0.18-0.23 K on uniform scenes',
        'calibration_vis': {'m': VIS_M, 'x0': VIS_X0, 'k': VIS_K, 'post_launch_factor': vis_factor,
                            'method': 'deep convective clouds (T11<205 K) mode set to %.2f' % DCC_TARGET},
        'statistics': stats,
        'caveats': [
            'SD carries raw counts; NOAA produced no L2 from SD. Calibration is transferred from GVAR.',
            'Detectors are not calibrated individually; striping of ~0.3 K may remain.',
            'Cloud-top height uses climatological profiles; semi-transparent cirrus is placed too low.',
            'Cloud mask is threshold based; sun glint, bright deserts and the day/night terminator are weak spots.',
            'No 12 um channel on GOES-15, so no split-window SST or dust product.',
        ],
    }
    json.dump(meta, open(os.path.join(out, 'L2_metadata.json'), 'w'), indent=1, default=float)
    log('  wrote L2_metadata.json')
    if not args.no_netcdf:
        vars_ = {
            'lat': (pack(lat, 0.005, 0), 'i2', 0.005, 0.0, 'degrees_north', 'latitude', -32768),
            'lon': (pack(lon, 0.01, 0), 'i2', 0.01, 0.0, 'degrees_east', 'longitude', -32768),
            'bt_3p9': (pack(T39, 0.01, 250), 'i2', 0.01, 250.0, 'K', 'brightness temperature 3.9 um', -32768),
            'bt_6p5': (pack(T65, 0.01, 250), 'i2', 0.01, 250.0, 'K', 'brightness temperature 6.5 um', -32768),
            'bt_10p7': (pack(T11, 0.01, 250), 'i2', 0.01, 250.0, 'K', 'brightness temperature 10.7 um', -32768),
            'refl_0p65': (pack(refl, 1e-4, 1.5), 'i2', 1e-4, 1.5, '1', 'VIS reflectance factor', -32768),
            'refl_3p9': (pack(r39, 1e-4, 1.5), 'i2', 1e-4, 1.5, '1', '3.9 um reflectance (day)', -32768),
            'cloud_mask': (cmask.astype(np.int8), 'i1', None, None, '1',
                           '0 no data, 1 clear, 2 probably clear, 3 probably cloudy, 4 cloudy', None),
            'cloud_top_height': (pack(cth, 0.001, 10), 'i2', 0.001, 10.0, 'km', 'cloud-top height (approx.)', -32768),
            'convection': (conv.astype(np.int8), 'i1', None, None, '1',
                           '0 none, 1 T11<235K, 2 T11<220K, 3 overshooting-top candidate', None),
            'solar_zenith': (pack(sza, 0.01, 90), 'i2', 0.01, 90.0, 'degree', 'solar zenith angle', -32768),
        }
        write_netcdf(os.path.join(out, 'GOES_SD_L2.nc'), H, W, vars_,
                     {'title': 'GOES SD Level-2 (unofficial)', 'satellite': args.satellite,
                      'time_coverage_start': str(t0), 'time_coverage_end': str(t1),
                      'Conventions': 'CF-1.6', 'history': 'sd_l2.py',
                      'comment': 'approximate; see L2_metadata.json caveats'})
    log('done ->', out)


if __name__ == '__main__':
    main()
