# Cloud-top nowcasting for GK-2A IR10.5 brightness temperature: block-matching motion, semi-Lagrangian
# advection, and a small neural network that corrects what advection misses (growth, decay, smoothing).
# Everything here is mirrored in C++ (src/nowcast.*) for the offline app.
import numpy as np, os, json, sys, time
from scipy import ndimage

D = os.path.dirname(os.path.abspath(__file__))
H = 2200
BLOCK = 24          # half-resolution block size for motion (96 km)
SEARCH = 6          # half-resolution search radius (24 km per 10 min ~ 40 m/s)

def load(name):
    a = np.load(os.path.join(D, name + '.npy')).astype(np.float32) / 100.0
    a[a <= 0] = np.nan
    return a

def half(a):
    v = a.reshape(H // 2, 2, H // 2, 2)
    with np.errstate(all='ignore'):
        return np.nanmean(v, axis=(1, 3))

def motion(a0, a1):
    """Displacement (px per step, full resolution) of the field from a0 to a1, per block, then smoothed
    and interpolated to every pixel. Returns (vy, vx) full-res arrays and a block confidence map."""
    with np.errstate(all='ignore'):
        h0, h1 = half(a0), half(a1)
    n = H // 2
    nb = n // BLOCK
    best = np.full((nb, nb), np.inf); by = np.zeros((nb, nb)); bx = np.zeros((nb, nb))
    costs = {}
    v0 = np.nan_to_num(h0, nan=0.0); m0 = np.isfinite(h0)
    v1 = np.nan_to_num(h1, nan=0.0); m1 = np.isfinite(h1)
    crop = slice(SEARCH, SEARCH + nb * BLOCK - 2 * SEARCH)
    for dy in range(-SEARCH, SEARCH + 1):
        for dx in range(-SEARCH, SEARCH + 1):
            # a1(x) ~ a0(x - d): compare a1 with a0 shifted by +d
            s = np.roll(np.roll(v0, dy, 0), dx, 1); sm = np.roll(np.roll(m0, dy, 0), dx, 1)
            ok = sm & m1
            diff = np.where(ok, np.abs(v1 - s), 0.0)[:nb * BLOCK, :nb * BLOCK]
            cnt = ok[:nb * BLOCK, :nb * BLOCK]
            sad = diff.reshape(nb, BLOCK, nb, BLOCK).sum((1, 3))
            c = cnt.reshape(nb, BLOCK, nb, BLOCK).sum((1, 3))
            cost = np.where(c > BLOCK * BLOCK * 0.6, sad / np.maximum(c, 1), np.inf)
            costs[(dy, dx)] = cost
            better = cost < best
            best[better] = cost[better]; by[better] = dy; bx[better] = dx
    # sub-pixel by parabola along each axis
    def para(cm, c0, cp):
        d = cm - 2 * c0 + cp
        return np.where(np.isfinite(d) & (d > 1e-6), 0.5 * (cm - cp) / d, 0.0).clip(-0.5, 0.5)
    fy = np.zeros_like(by); fx = np.zeros_like(bx)
    for i in range(nb):
        for j in range(nb):
            if not np.isfinite(best[i, j]): continue
            y, x = int(by[i, j]), int(bx[i, j])
            if abs(y) < SEARCH:
                fy[i, j] = para(costs[(y - 1, x)][i, j], best[i, j], costs[(y + 1, x)][i, j])
            if abs(x) < SEARCH:
                fx[i, j] = para(costs[(y, x - 1)][i, j], best[i, j], costs[(y, x + 1)][i, j])
    vy = by + fy; vx = bx + fx
    # confidence: texture of the block (flat clear sky gives no motion information) and match quality
    with np.errstate(all='ignore'):
        tex = np.nanstd(h1[:nb * BLOCK, :nb * BLOCK].reshape(nb, BLOCK, nb, BLOCK).transpose(0, 2, 1, 3).reshape(nb, nb, -1), axis=2)
    good = np.isfinite(best) & (tex > 2.0)
    # fill weak blocks from neighbours (normalised convolution), then smooth
    w = good.astype(np.float32)
    k = np.ones((5, 5), np.float32)
    def fill(v):
        num = ndimage.convolve(np.where(good, v, 0) * 1.0, k, mode='nearest')
        den = ndimage.convolve(w, k, mode='nearest')
        out = np.where(good, v, num / np.maximum(den, 1e-6))
        out = np.where(den > 0, out, 0.0)
        return ndimage.gaussian_filter(ndimage.median_filter(out, 3), 0.8)
    vy = fill(vy); vx = fill(vx)
    # to full resolution (block centres), px of the full grid per step
    zy = ndimage.zoom(vy, (BLOCK * 2), order=1) * 2.0
    zx = ndimage.zoom(vx, (BLOCK * 2), order=1) * 2.0
    full_y = np.zeros((H, H), np.float32); full_x = np.zeros((H, H), np.float32)
    m = min(H, zy.shape[0])
    full_y[:m, :m] = zy[:m, :m]; full_x[:m, :m] = zx[:m, :m]
    return full_y, full_x, good

def advect(a, vy, vx, steps=1.0):
    """Semi-Lagrangian: value at x after `steps` comes from x - v*steps (v constant)."""
    yy, xx = np.mgrid[0:H, 0:H].astype(np.float32)
    src = np.nan_to_num(a, nan=-1.0)
    out = ndimage.map_coordinates(src, [yy - vy * steps, xx - vx * steps], order=1, mode='constant', cval=-1.0)
    valid = ndimage.map_coordinates(np.isfinite(a).astype(np.float32), [yy - vy * steps, xx - vx * steps], order=1, mode='constant', cval=0.0)
    out[valid < 0.99] = np.nan
    return out.astype(np.float32)

YY, XX = np.mgrid[0:H, 0:H].astype(np.float32)
RR = np.sqrt((YY - 1100) ** 2 + (XX - 1100) ** 2) / 1092.0

def features(t_prev, t_now, vy, vx):
    """Per-pixel features for predicting the next step. Returns (F [N, k] for all pixels, adv)."""
    adv = advect(t_now, vy, vx, 1.0)
    # Lagrangian tendency: how the parcel arriving at x changed over the last step
    src_now = advect(t_now, vy, vx, 1.0)            # T_now at x - v
    src_prev = advect(t_prev, vy, vx, 2.0)          # T_prev at x - 2v
    trend = src_now - src_prev
    filled = np.nan_to_num(adv, nan=np.nanmean(adv))
    mean9 = ndimage.uniform_filter(filled, 9)
    sq = ndimage.uniform_filter(filled * filled, 5); m5 = ndimage.uniform_filter(filled, 5)
    std5 = np.sqrt(np.maximum(sq - m5 * m5, 0))
    speed = np.hypot(vy, vx)
    F = np.stack([adv, t_now, adv - t_now, trend, std5, mean9 - adv, speed, RR], axis=-1)
    return F, adv

FEATURE_NAMES = ['adv', 'now', 'adv-now', 'trend', 'std5', 'mean9-adv', 'speed', 'r']
