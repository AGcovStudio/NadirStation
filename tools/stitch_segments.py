#!/usr/bin/env python3
"""Stitch consecutive GOES SD segments into one full-disc picture.

The decoder writes a new folder every 360 scans (about 7 minutes) or when the signal drops. This tool puts the
segments of one pass back together:

  1. segments in time order, top to bottom. By default they are taken as continuous (the decoder starts the next
     segment with the next scan). With --time-gaps, a gap between them is worked out from the start times in
     GOES_SD_Observation.json and left as empty scans; use it only for segments decoded by version 1.0.0 or later
     (the folder names of older NadirStation builds are the time of decoding, not of the recording),
  2. every scan is shifted sideways so that the centre of the Earth disc (the midpoint between the left and the right
     limb in IR group 2, where space is cold and bright on both sides) lies on one column. This removes the slow
     east-west drift of the scan start and makes the disc round,
  3. scans that are entirely empty are filled by linear interpolation from the nearest scans above and below,
  4. optional 3 x 3 median filter (--median) on the visible and IR counts,
  5. writes VIS / IR1..3 (16-bit counts), VIS_Enhanced, IR*_Enhanced and FalseColor.

Needs only numpy and Pillow:   pip install numpy pillow
Usage:  python stitch_segments.py <folder with the segment folders | segment folders ...> -o <output folder> [--median] [--time-gaps]
"""
import argparse
import glob
import json
import os
import re
import sys
from datetime import datetime, timezone

import numpy as np
from PIL import Image

Image.MAX_IMAGE_PIXELS = None
SCAN_PERIOD_S = 6286 * 480 / 2620800.0   # one east-west scan cycle of the imager (about 1.151 s)
IR_SPACE = 940                           # raw IR counts above this are cold space (IR images: colder = higher)


def load16(path):
    a = np.asarray(Image.open(path))
    return a.astype(np.uint16)


def save16(path, a):
    Image.fromarray(a.astype(np.uint16)).save(path)


def rolling_median_1d(x, w):
    """Median over a centred window, ignoring NaN; NaN where the window has no value."""
    n = len(x)
    out = np.full(n, np.nan)
    h = w // 2
    for i in range(n):
        v = x[max(0, i - h):i + h + 1]
        v = v[np.isfinite(v)]
        if len(v):
            out[i] = np.median(v)
    return out


def median_filter(a, ky, kx, chunk=256):
    """ky x kx median of a 2-D array (edges replicated), in row chunks to keep memory small."""
    py, px = ky // 2, kx // 2
    p = np.pad(a, ((py, py), (px, px)), mode="edge")
    out = np.empty_like(a)
    for y0 in range(0, a.shape[0], chunk):
        y1 = min(a.shape[0], y0 + chunk)
        win = np.lib.stride_tricks.sliding_window_view(p[y0:y1 + 2 * py], (ky, kx))
        out[y0:y1] = np.median(win.reshape(win.shape[0], win.shape[1], -1), axis=2)
    return out


def limb_midpoints(ir):
    """Per IR row: midpoint between the first and the last disc pixel, NaN when a limb is outside the data."""
    a = ir.astype(np.float32) / 64.0
    zero = ir == 0
    f = median_filter(np.where(zero, 975.0, a), 3, 11)
    mids = np.full(a.shape[0], np.nan)
    for y in range(a.shape[0]):
        nz = np.flatnonzero(~zero[y])
        if len(nz) < a.shape[1] // 2:
            continue
        d = (f[y] < IR_SPACE - 35).astype(np.int32)
        run = np.convolve(d, np.ones(15, np.int32), "valid") == 15
        idx = np.flatnonzero(run)
        if len(idx) < 30:
            continue
        left, right = idx[0], idx[-1] + 14
        if left <= nz[0] + 2 or right >= nz[-1] - 2:
            continue           # a limb is clipped by the edge of the scan
        mids[y] = 0.5 * (left + right)
    return mids


def fill_blank_rows(a, c0, c1):
    blank = ~a[:, c0:c1].any(axis=1)
    h = a.shape[0]
    y = 0
    filled = 0
    while y < h:
        if not blank[y]:
            y += 1
            continue
        s = y
        while y < h and blank[y]:
            y += 1
        e = y - 1
        if s == 0 or e == h - 1:
            continue
        up = a[s - 1].astype(np.float32)
        dn = a[e + 1].astype(np.float32)
        n = e - s + 2
        for k, yy in enumerate(range(s, e + 1)):
            w = (k + 1) / n
            row = up * (1 - w) + dn * w
            row = np.where(up == 0, dn, np.where(dn == 0, up, row))
            a[yy] = np.rint(row).astype(a.dtype)
        filled += e - s + 1
    return filled


def percentile_counts(raw, lo_p, hi_p, mask=None):
    v = (raw >> 6).astype(np.int64)
    m = v > 0 if mask is None else (v > 0) & mask(v)
    h = np.bincount(v[m], minlength=1024).astype(np.float64)
    cs = np.cumsum(h[1:])
    lo = int(np.searchsorted(cs, lo_p * cs[-1])) + 1
    hi = max(lo + 16, int(np.searchsorted(cs, hi_p * cs[-1])) + 1)
    return lo, hi


def ramp_cloud_top(t):
    stops = [(0.0, (0, 0, 0)), (0.62, (225, 225, 225)), (0.68, (80, 160, 230)), (0.76, (40, 200, 80)),
             (0.84, (240, 220, 40)), (0.91, (240, 100, 30)), (0.97, (200, 20, 20)), (1.0, (255, 255, 255))]
    xs = [s[0] for s in stops]
    return np.stack([np.interp(t, xs, [s[1][k] for s in stops]) for k in range(3)], -1)


def load_lut():
    here = os.path.dirname(os.path.abspath(__file__))
    path = os.path.join(here, "..", "src", "gvar_fc_lut.inc")
    text = open(path, encoding="utf-8").read()
    body = "\n".join(l for l in text.splitlines() if not l.lstrip().startswith("//"))
    nums = np.array([int(x) for x in re.findall(r"\b\d+\b", body)], np.uint8)
    return nums[:256 * 256 * 3].reshape(256, 256, 3)   # [IR, VIS] -> RGB


def segment_folders(args):
    out = []
    for p in args:
        if os.path.isfile(os.path.join(p, "GOES_SD_Observation.json")):
            out.append(p)
        else:
            out += [os.path.dirname(j) for j in glob.glob(os.path.join(p, "*", "GOES_SD_Observation.json"))]
    out = sorted(set(os.path.abspath(p) for p in out), key=os.path.basename)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("inputs", nargs="+", help="segment folders, or a folder that contains them")
    ap.add_argument("-o", "--output", required=True, help="output folder")
    ap.add_argument("--median", action="store_true", help="3 x 3 median filter on the counts before the products")
    ap.add_argument("--time-gaps", action="store_true", help="leave empty scans for time gaps between segments")
    a = ap.parse_args()

    segs = segment_folders(a.inputs)
    if not segs:
        sys.exit("no GOES_SD_Observation.json found")
    os.makedirs(a.output, exist_ok=True)
    print("segments:", *[os.path.basename(s) for s in segs], sep="\n  ")

    # ---- load, with empty scans for time gaps between segments
    vis_parts, ir_parts = [], [[], [], []]
    width = None
    prev_end = None
    for s in segs:
        meta = json.load(open(os.path.join(s, "GOES_SD_Observation.json"), encoding="utf-8"))
        v = load16(os.path.join(s, "GOES_SD_VIS.png"))
        irs = [load16(os.path.join(s, "GOES_SD_IR%d.png" % g)) for g in (1, 2, 3)]
        if width is None:
            width = v.shape[1]
        if v.shape[1] != width:
            print("  skipping %s: scan width %d differs from %d (another scan mode)" % (os.path.basename(s), v.shape[1], width))
            continue
        start = datetime.strptime(str(meta.get("observation_start", os.path.basename(s))), "%Y-%m-%d_%H-%M-%S").replace(tzinfo=timezone.utc)
        if a.time_gaps and prev_end is not None:
            gap = int(round((start - prev_end).total_seconds() / SCAN_PERIOD_S))
            if gap > 1:
                print("  gap of %d scans before %s" % (gap, os.path.basename(s)))
                vis_parts.append(np.zeros((gap * 8, width), np.uint16))
                for g in range(3):
                    ir_parts[g].append(np.zeros((gap * 2, irs[g].shape[1]), np.uint16))
        scans = v.shape[0] // 8
        prev_end = start.timestamp() + scans * SCAN_PERIOD_S
        prev_end = datetime.fromtimestamp(prev_end, timezone.utc)
        vis_parts.append(v)
        for g in range(3):
            ir_parts[g].append(irs[g])
    V0 = np.vstack(vis_parts)
    I0 = [np.vstack(p) for p in ir_parts]
    nsc = V0.shape[0] // 8
    print("scans:", nsc)

    # ---- per scan: shift that puts the disc centre on one column
    mids = limb_midpoints(I0[1])
    mids = rolling_median_1d(mids, 9)
    ok = np.isfinite(mids)
    if ok.sum() < 10:
        sys.exit("could not find the Earth's limbs in IR group 2")
    idx = np.arange(len(mids))
    mids = np.interp(idx, idx[ok], mids[ok])
    target = float(np.median(mids))
    sh_ir = (target - mids)[: nsc * 2].reshape(nsc, 2).mean(axis=1)
    vsh = np.rint(4 * sh_ir).astype(int)
    ish = np.rint(sh_ir).astype(int)
    x0v, x0i = vsh - vsh.min(), ish - ish.min()
    Wv, Wi = width + int(x0v.max()), I0[0].shape[1] + int(x0i.max())
    print("shift per scan: %d .. %d px" % (vsh.min(), vsh.max()))
    V = np.zeros((nsc * 8, Wv), np.uint16)
    for k in range(nsc):
        V[k * 8:(k + 1) * 8, x0v[k]:x0v[k] + width] = V0[k * 8:(k + 1) * 8]
    IR = []
    for g in range(3):
        w = I0[g].shape[1]
        I = np.zeros((nsc * 2, Wi), np.uint16)
        for k in range(nsc):
            I[k * 2:(k + 1) * 2, x0i[k]:x0i[k] + w] = I0[g][k * 2:(k + 1) * 2]
        IR.append(I)
    del V0, I0

    # ---- empty scans, median
    print("empty rows filled: VIS %d, IR %d" % (fill_blank_rows(V, Wv // 6, Wv - Wv // 6),
                                                 sum(fill_blank_rows(I, Wi // 6, Wi - Wi // 6) for I in IR)))
    if a.median:
        def med3(x):
            m = median_filter(x, 3, 3)
            return np.where((x > 0) & (m > 0), m, x).astype(np.uint16)
        V = med3(V)
        IR = [med3(I) for I in IR]
        print("3 x 3 median applied")

    out = lambda n: os.path.join(a.output, n)
    save16(out("GOES_SD_VIS.png"), V)
    for g in range(3):
        save16(out("GOES_SD_IR%d.png" % (g + 1)), IR[g])

    # ---- products
    lo, hi = percentile_counts(V, 0.01, 0.997)
    vc = (V >> 6).astype(np.float32)
    t = np.clip((vc - lo) / (hi - lo), 0, 1)
    venh = np.where(vc > 0, np.clip(np.rint(t ** 0.8 * 254) + 1, 1, 255), 0).astype(np.uint8)
    Image.fromarray(venh).save(out("GOES_SD_VIS_Enhanced.png"))
    vv = np.where(vc > 0, np.clip(np.rint(t ** 0.8 * 254), 0, 254), 0).astype(np.int64)   # column 255 of the LUT is black
    del vc, t, venh

    cold8 = None
    for g in range(3):
        v = (IR[g] >> 6).astype(np.int64)
        m = v > 0
        lo_i, hi_i = percentile_counts(IR[g], 0.005, 0.995, mask=lambda x: x < IR_SPACE)
        t = np.clip((v - lo_i) / (hi_i - lo_i), 0, 1)
        if g == 1:
            rgb = np.zeros(v.shape + (3,), np.uint8)
            rgb[m] = np.rint(ramp_cloud_top(t[m])).astype(np.uint8)
            Image.fromarray(rgb).save(out("GOES_SD_IR2_Enhanced.png"))
            lo2, hi2 = percentile_counts(IR[g], 0.005, 0.995)
            t2 = np.clip((v - lo2) / (hi2 - lo2), 0, 1)
            cold8 = np.where(m, 1 + np.rint(t2 * 253), 0).astype(np.int64)
        else:
            Image.fromarray(np.where(m, np.rint(t * 255), 0).astype(np.uint8)).save(out("GOES_SD_IR%d_Enhanced.png" % (g + 1)))

    lut = load_lut()
    h, w = vv.shape
    fc = np.zeros((h, w, 3), np.uint8)
    cols = np.minimum(np.arange(w) // 4, cold8.shape[1] - 1)
    for y0 in range(0, h, 512):
        y1 = min(h, y0 + 512)
        ir = cold8[np.arange(y0, y1) // 4][:, cols]
        fc[y0:y1] = lut[ir, vv[y0:y1]]
    Image.fromarray(fc).save(out("GOES_SD_FalseColor.png"))
    json.dump({"segments": [os.path.basename(s) for s in segs], "scans": nsc, "canvas_vis_px": [Wv, nsc * 8],
               "shift_px_range": [int(vsh.min()), int(vsh.max())], "median_3x3": bool(a.median)},
              open(out("stitch_info.json"), "w"), indent=1)
    print("done:", os.path.abspath(a.output))


if __name__ == "__main__":
    main()
