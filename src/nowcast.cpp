#include "nowcast.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <functional>
#include <limits>
#include <sstream>
#include <thread>

// The steps follow tools/nowcast/nowcast.py one for one (the model was trained on that pipeline's
// features, so the two must agree): half-resolution block matching, 24-pixel blocks, +-6 search,
// parabolic sub-pixel, texture test, normalised-convolution fill, 3x3 median, Gaussian 0.8,
// linear zoom to the full grid; scipy's boundary modes are reproduced where they matter.

namespace nadirstation::nowcast {
namespace {
constexpr int kBlock = 24;
constexpr int kSearch = 6;
constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

void parallelRows(int rows, int threads, const std::function<void(int, int)>& fn) {
    threads = std::max(1, std::min(threads, rows));
    if (threads == 1) { fn(0, rows); return; }
    std::vector<std::thread> pool;
    const int band = (rows + threads - 1) / threads;
    for (int i = 0; i < threads; ++i) {
        const int a = i * band, b = std::min(rows, (i + 1) * band);
        if (a < b) pool.emplace_back(fn, a, b);
    }
    for (auto& t : pool) t.join();
}

// scipy.ndimage 'reflect' (half-sample symmetric: d c b a | a b c d | d c b a) and 'nearest'.
inline int reflect(int i, int n) {
    if (n == 1) return 0;
    while (i < 0 || i >= n) {
        if (i < 0) i = -i - 1;
        if (i >= n) i = 2 * n - i - 1;
    }
    return i;
}
inline int nearest(int i, int n) { return std::clamp(i, 0, n - 1); }

// Separable filters on an n x n grid.
std::vector<float> boxFilter(const std::vector<float>& a, int n, int size) {
    const int r = size / 2;
    std::vector<float> tmp(a.size()), out(a.size());
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            double s = 0.0;
            for (int k = -r; k <= r; ++k) s += a[static_cast<std::size_t>(y) * n + reflect(x + k, n)];
            tmp[static_cast<std::size_t>(y) * n + x] = static_cast<float>(s / size);
        }
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            double s = 0.0;
            for (int k = -r; k <= r; ++k) s += tmp[static_cast<std::size_t>(reflect(y + k, n)) * n + x];
            out[static_cast<std::size_t>(y) * n + x] = static_cast<float>(s / size);
        }
    return out;
}

std::vector<double> sumNearest5(const std::vector<double>& a, int n) {
    std::vector<double> out(a.size(), 0.0);
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            double s = 0.0;
            for (int dy = -2; dy <= 2; ++dy)
                for (int dx = -2; dx <= 2; ++dx)
                    s += a[static_cast<std::size_t>(nearest(y + dy, n)) * n + nearest(x + dx, n)];
            out[static_cast<std::size_t>(y) * n + x] = s;
        }
    return out;
}

std::vector<double> median3(const std::vector<double>& a, int n) {
    std::vector<double> out(a.size());
    double w[9];
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            int k = 0;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) w[k++] = a[static_cast<std::size_t>(reflect(y + dy, n)) * n + reflect(x + dx, n)];
            std::nth_element(w, w + 4, w + 9);
            out[static_cast<std::size_t>(y) * n + x] = w[4];
        }
    return out;
}

std::vector<double> gauss08(const std::vector<double>& a, int n) {
    // sigma 0.8, truncate 4 -> radius 3
    double k[7], sum = 0.0;
    for (int i = -3; i <= 3; ++i) { k[i + 3] = std::exp(-0.5 * i * i / (0.8 * 0.8)); sum += k[i + 3]; }
    for (double& v : k) v /= sum;
    std::vector<double> tmp(a.size()), out(a.size());
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            double s = 0.0;
            for (int i = -3; i <= 3; ++i) s += k[i + 3] * a[static_cast<std::size_t>(y) * n + reflect(x + i, n)];
            tmp[static_cast<std::size_t>(y) * n + x] = s;
        }
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            double s = 0.0;
            for (int i = -3; i <= 3; ++i) s += k[i + 3] * tmp[static_cast<std::size_t>(reflect(y + i, n)) * n + x];
            out[static_cast<std::size_t>(y) * n + x] = s;
        }
    return out;
}

// Linear zoom with scipy's default grid (corners aligned): in = out * (nIn - 1) / (nOut - 1).
std::vector<float> zoomLinear(const std::vector<double>& a, int nIn, int nOut) {
    std::vector<float> out(static_cast<std::size_t>(nOut) * nOut);
    const double s = nOut > 1 ? static_cast<double>(nIn - 1) / (nOut - 1) : 0.0;
    for (int y = 0; y < nOut; ++y) {
        const double fy = y * s;
        const int y0 = std::min(static_cast<int>(fy), nIn - 1), y1 = std::min(y0 + 1, nIn - 1);
        const double wy = fy - y0;
        for (int x = 0; x < nOut; ++x) {
            const double fx = x * s;
            const int x0 = std::min(static_cast<int>(fx), nIn - 1), x1 = std::min(x0 + 1, nIn - 1);
            const double wx = fx - x0;
            const double v = (a[static_cast<std::size_t>(y0) * nIn + x0] * (1 - wx) + a[static_cast<std::size_t>(y0) * nIn + x1] * wx) * (1 - wy)
                           + (a[static_cast<std::size_t>(y1) * nIn + x0] * (1 - wx) + a[static_cast<std::size_t>(y1) * nIn + x1] * wx) * wy;
            out[static_cast<std::size_t>(y) * nOut + x] = static_cast<float>(v);
        }
    }
    return out;
}

Field half(const Field& f) {
    Field h;
    h.size = f.size / 2;
    h.k.assign(static_cast<std::size_t>(h.size) * h.size, kNaN);
    for (int y = 0; y < h.size; ++y)
        for (int x = 0; x < h.size; ++x) {
            double s = 0.0; int c = 0;
            for (int dy = 0; dy < 2; ++dy)
                for (int dx = 0; dx < 2; ++dx) {
                    const float v = f.at(2 * y + dy, 2 * x + dx);
                    if (std::isfinite(v)) { s += v; ++c; }
                }
            if (c) h.k[static_cast<std::size_t>(y) * h.size + x] = static_cast<float>(s / c);
        }
    return h;
}
}  // namespace

Field fromRaw16(const std::uint16_t* data, int size) {
    Field f;
    f.size = size;
    f.k.resize(static_cast<std::size_t>(size) * size);
    for (std::size_t i = 0; i < f.k.size(); ++i) f.k[i] = data[i] == 0 ? kNaN : data[i] * 0.01f;
    return f;
}

std::vector<std::uint16_t> toRaw16(const Field& f) {
    std::vector<std::uint16_t> out(f.k.size(), 0);
    for (std::size_t i = 0; i < f.k.size(); ++i)
        if (std::isfinite(f.k[i])) out[i] = static_cast<std::uint16_t>(std::clamp(std::lround(f.k[i] * 100.0f), 1L, 65535L));
    return out;
}

Motion estimateMotion(const Field& a0, const Field& a1) {
    Motion m;
    m.size = a1.size;
    const Field h0 = half(a0), h1 = half(a1);
    const int n = h1.size;
    const int nb = n / kBlock;
    const int span = 2 * kSearch + 1;
    // cost[(dy,dx)][block]
    std::vector<double> costs(static_cast<std::size_t>(span) * span * nb * nb, std::numeric_limits<double>::infinity());
    std::vector<double> best(static_cast<std::size_t>(nb) * nb, std::numeric_limits<double>::infinity());
    std::vector<int> by(best.size(), 0), bx(best.size(), 0);
    auto costAt = [&](int dy, int dx, int b) -> double& {
        return costs[(static_cast<std::size_t>(dy + kSearch) * span + (dx + kSearch)) * nb * nb + b];
    };
    for (int dy = -kSearch; dy <= kSearch; ++dy)
        for (int dx = -kSearch; dx <= kSearch; ++dx) {
            for (int bi = 0; bi < nb; ++bi)
                for (int bj = 0; bj < nb; ++bj) {
                    double sad = 0.0; int cnt = 0;
                    for (int y = bi * kBlock; y < (bi + 1) * kBlock; ++y) {
                        // np.roll: shifted[y][x] = h0[y - dy][x - dx], wrapping
                        const int sy = ((y - dy) % n + n) % n;
                        for (int x = bj * kBlock; x < (bj + 1) * kBlock; ++x) {
                            const int sx = ((x - dx) % n + n) % n;
                            const float v1 = h1.k[static_cast<std::size_t>(y) * n + x];
                            const float v0 = h0.k[static_cast<std::size_t>(sy) * n + sx];
                            if (std::isfinite(v1) && std::isfinite(v0)) { sad += std::fabs(v1 - v0); ++cnt; }
                        }
                    }
                    const int b = bi * nb + bj;
                    const double c = cnt > kBlock * kBlock * 0.6 ? sad / std::max(cnt, 1) : std::numeric_limits<double>::infinity();
                    costAt(dy, dx, b) = c;
                    if (c < best[b]) { best[b] = c; by[b] = dy; bx[b] = dx; }
                }
        }
    auto para = [](double cm, double c0, double cp) {
        const double d = cm - 2 * c0 + cp;
        return (std::isfinite(d) && d > 1e-6) ? std::clamp(0.5 * (cm - cp) / d, -0.5, 0.5) : 0.0;
    };
    std::vector<double> vy(best.size()), vx(best.size());
    std::vector<char> good(best.size(), 0);
    int goodCount = 0;
    for (int bi = 0; bi < nb; ++bi)
        for (int bj = 0; bj < nb; ++bj) {
            const int b = bi * nb + bj;
            double fy = 0.0, fx = 0.0;
            if (std::isfinite(best[b])) {
                if (std::abs(by[b]) < kSearch) fy = para(costAt(by[b] - 1, bx[b], b), best[b], costAt(by[b] + 1, bx[b], b));
                if (std::abs(bx[b]) < kSearch) fx = para(costAt(by[b], bx[b] - 1, b), best[b], costAt(by[b], bx[b] + 1, b));
            }
            vy[b] = by[b] + fy;
            vx[b] = bx[b] + fx;
            // texture: population standard deviation of the later field over the block
            double s = 0.0, s2 = 0.0; int c = 0;
            for (int y = bi * kBlock; y < (bi + 1) * kBlock; ++y)
                for (int x = bj * kBlock; x < (bj + 1) * kBlock; ++x) {
                    const float v = h1.k[static_cast<std::size_t>(y) * n + x];
                    if (std::isfinite(v)) { s += v; s2 += double(v) * v; ++c; }
                }
            const double tex = c > 0 ? std::sqrt(std::max(0.0, s2 / c - (s / c) * (s / c))) : 0.0;
            good[b] = std::isfinite(best[b]) && tex > 2.0;
            goodCount += good[b];
        }
    m.goodBlocks = best.empty() ? 0.0 : static_cast<double>(goodCount) / best.size();

    auto fill = [&](const std::vector<double>& v) {
        std::vector<double> num(v.size()), w(v.size());
        for (std::size_t i = 0; i < v.size(); ++i) { num[i] = good[i] ? v[i] : 0.0; w[i] = good[i] ? 1.0 : 0.0; }
        const std::vector<double> ns = sumNearest5(num, nb), ws = sumNearest5(w, nb);
        std::vector<double> out(v.size());
        for (std::size_t i = 0; i < v.size(); ++i) {
            double o = good[i] ? v[i] : ns[i] / std::max(ws[i], 1e-6);
            out[i] = ws[i] > 0 ? o : 0.0;
        }
        return gauss08(median3(out, nb), nb);
    };
    const std::vector<double> sy = fill(vy), sx = fill(vx);
    const int zoomed = nb * kBlock * 2;
    const std::vector<float> zy = zoomLinear(sy, nb, zoomed), zx = zoomLinear(sx, nb, zoomed);
    m.vy.assign(static_cast<std::size_t>(m.size) * m.size, 0.0f);
    m.vx.assign(m.vy.size(), 0.0f);
    const int lim = std::min(m.size, zoomed);
    for (int y = 0; y < lim; ++y)
        for (int x = 0; x < lim; ++x) {
            m.vy[static_cast<std::size_t>(y) * m.size + x] = zy[static_cast<std::size_t>(y) * zoomed + x] * 2.0f;
            m.vx[static_cast<std::size_t>(y) * m.size + x] = zx[static_cast<std::size_t>(y) * zoomed + x] * 2.0f;
        }
    return m;
}

namespace {
Field advectThreads(const Field& f, const Motion& m, double steps, int threads) {
    Field out;
    out.size = f.size;
    out.k.assign(f.k.size(), kNaN);
    const int n = f.size;
    parallelRows(n, threads, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < n; ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * n + x;
                const double sy = y - m.vy[i] * steps, sx = x - m.vx[i] * steps;
                const int iy = static_cast<int>(std::floor(sy)), ix = static_cast<int>(std::floor(sx));
                const double wy = sy - iy, wx = sx - ix;
                double val = 0.0, mask = 0.0;
                const int ys[2] = {iy, iy + 1}, xs[2] = {ix, ix + 1};
                const double wys[2] = {1 - wy, wy}, wxs[2] = {1 - wx, wx};
                for (int a = 0; a < 2; ++a)
                    for (int b = 0; b < 2; ++b) {
                        const double w = wys[a] * wxs[b];
                        if (w == 0.0) continue;
                        float v = -1.0f;
                        double ok = 0.0;
                        if (ys[a] >= 0 && ys[a] < n && xs[b] >= 0 && xs[b] < n) {
                            const float s = f.k[static_cast<std::size_t>(ys[a]) * n + xs[b]];
                            if (std::isfinite(s)) { v = s; ok = 1.0; }
                        }
                        val += w * v;
                        mask += w * ok;
                    }
                if (mask >= 0.99) out.k[i] = static_cast<float>(val);
            }
    });
    return out;
}
}  // namespace

Field advect(const Field& f, const Motion& m, double steps) {
    return advectThreads(f, m, steps, 4);
}

Model loadModel(const std::string& path, std::string* error) {
    // Text format written by tools/nowcast/train.py:
    //   nowcast-mlp 1
    //   name <rest of line>
    //   features <n>  then n means, n scales
    //   layers <L>    then per layer: <in> <out>, in*out weights (row-major in x out), out biases
    Model m;
    std::ifstream in(path);
    auto fail = [&](const char* why) { if (error) *error = why; return Model{}; };
    if (!in) return fail("cannot open model file");
    std::string magic; int version = 0;
    in >> magic >> version;
    if (magic != "nowcast-mlp" || version != 1) return fail("not a nowcast-mlp v1 file");
    std::string key;
    in >> key;
    if (key != "name") return fail("missing name");
    std::getline(in, m.name);
    if (!m.name.empty() && m.name.front() == ' ') m.name.erase(0, 1);
    int nf = 0;
    in >> key >> nf;
    if (key != "features" || nf <= 0 || nf > 64) return fail("bad feature count");
    m.mean.resize(nf); m.scale.resize(nf);
    for (float& v : m.mean) in >> v;
    for (float& v : m.scale) in >> v;
    int layers = 0;
    in >> key >> layers;
    if (key != "layers" || layers <= 0 || layers > 8) return fail("bad layer count");
    int prev = nf;
    for (int l = 0; l < layers; ++l) {
        int a = 0, b = 0;
        in >> a >> b;
        if (a != prev || b <= 0 || b > 256) return fail("layer sizes do not chain");
        std::vector<float> w(static_cast<std::size_t>(a) * b), bias(b);
        for (float& v : w) in >> v;
        for (float& v : bias) in >> v;
        m.weights.push_back(std::move(w));
        m.biases.push_back(std::move(bias));
        prev = b;
    }
    if (!in || prev != 1) return fail("truncated model or output is not one value");
    return m;
}

Field step(const Field& prev, const Field& now, const Motion& m, const Model& model, int threads) {
    const int n = now.size;
    Field adv = advectThreads(now, m, 1.0, threads);
    if (!model.valid()) return adv;
    const Field prev2 = advectThreads(prev, m, 2.0, threads);
    // filled: NaN replaced by the mean of the advected field (as nan_to_num(adv, nanmean(adv)))
    double sum = 0.0; std::size_t cnt = 0;
    for (float v : adv.k) if (std::isfinite(v)) { sum += v; ++cnt; }
    const float fillValue = cnt ? static_cast<float>(sum / cnt) : 0.0f;
    std::vector<float> filled(adv.k.size());
    for (std::size_t i = 0; i < filled.size(); ++i) filled[i] = std::isfinite(adv.k[i]) ? adv.k[i] : fillValue;
    std::vector<float> sq(filled.size());
    for (std::size_t i = 0; i < sq.size(); ++i) sq[i] = filled[i] * filled[i];
    const std::vector<float> mean9 = boxFilter(filled, n, 9), m5 = boxFilter(filled, n, 5), sq5 = boxFilter(sq, n, 5);

    Field out = adv;
    const int nf = static_cast<int>(model.mean.size());
    parallelRows(n, threads, [&](int y0, int y1) {
        std::vector<float> a(256), b(256);
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < n; ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * n + x;
                const float ad = adv.k[i], nw = now.k[i];
                const float trend = adv.k[i] - prev2.k[i];
                const float std5 = std::sqrt(std::max(0.0f, sq5[i] - m5[i] * m5[i]));
                const float speed = std::hypot(m.vy[i], m.vx[i]);
                const float r = static_cast<float>(std::hypot(y - 1100.0, x - 1100.0) / 1092.0);
                const float f[8] = {ad, nw, ad - nw, trend, std5, mean9[i] - ad, speed, r};
                bool ok = nf == 8;
                for (float v : f) ok = ok && std::isfinite(v);
                if (!ok) continue;
                for (int k = 0; k < nf; ++k) a[k] = (f[k] - model.mean[k]) / model.scale[k];
                int width = nf;
                for (std::size_t l = 0; l < model.weights.size(); ++l) {
                    const int outW = static_cast<int>(model.biases[l].size());
                    const float* W = model.weights[l].data();
                    for (int o = 0; o < outW; ++o) b[o] = model.biases[l][o];
                    for (int k = 0; k < width; ++k) {
                        const float ak = a[k];
                        const float* row = W + static_cast<std::size_t>(k) * outW;
                        for (int o = 0; o < outW; ++o) b[o] += ak * row[o];
                    }
                    const bool last = l + 1 == model.weights.size();
                    for (int o = 0; o < outW; ++o) a[o] = last ? b[o] : std::max(0.0f, b[o]);
                    width = outW;
                }
                out.k[i] = ad + a[0];
            }
    });
    return out;
}

std::vector<Field> forecast(const Field& previous, const Field& now, const Motion& m, const Model& model,
                            int steps, int threads, void (*progress)(int, void*), void* user) {
    std::vector<Field> out;
    Field p = previous, c = now;
    for (int s = 1; s <= steps; ++s) {
        Field next = step(p, c, m, model, threads);
        out.push_back(next);
        p = std::move(c);
        c = std::move(next);
        if (progress) progress(s, user);
    }
    return out;
}

}  // namespace nadirstation::nowcast
